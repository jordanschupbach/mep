#include "gfx/cff.h"

#include "gfx/rasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace gfx {
namespace cff {

namespace {

uint16_t U16(const unsigned char *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// -- INDEX (CFF spec section 5) -------------------------------------------

// Parses one INDEX starting at `p` (at most `remaining` bytes available).
// On success, *consumed is the INDEX's total byte length (including its
// own count/offSize/offsets header). Returns false only on a truncated/
// malformed INDEX -- an empty INDEX (count 0) is valid and succeeds.
bool ParseIndex(const unsigned char *p, size_t remaining, Index *out, size_t *consumed) {
    if (remaining < 2) return false;
    int count = U16(p);
    if (count == 0) {
        *out = Index();
        *consumed = 2;
        return true;
    }
    if (remaining < 3) return false;
    uint8_t off_size = p[2];
    if (off_size < 1 || off_size > 4) return false;
    size_t offsets_start = 3;
    size_t offsets_bytes = static_cast<size_t>(count + 1) * off_size;
    if (remaining < offsets_start + offsets_bytes) return false;

    out->count = count;
    out->offsets.resize(static_cast<size_t>(count) + 1);
    for (int i = 0; i <= count; ++i) {
        uint32_t v = 0;
        for (int b = 0; b < off_size; ++b) v = (v << 8) | p[offsets_start + static_cast<size_t>(i) * off_size + static_cast<size_t>(b)];
        if (v == 0) return false;  // offsets are 1-based; 0 is never valid
        out->offsets[static_cast<size_t>(i)] = v - 1;
    }
    size_t data_off = offsets_start + offsets_bytes;
    size_t data_len = out->offsets.back();
    if (remaining < data_off + data_len) return false;
    out->data_start = p + data_off;
    *consumed = data_off + data_len;
    return true;
}

bool GetItem(const Index &idx, int i, const unsigned char **out_ptr, uint32_t *out_len) {
    if (i < 0 || i >= idx.count || !idx.data_start) return false;
    *out_ptr = idx.data_start + idx.offsets[static_cast<size_t>(i)];
    *out_len = idx.offsets[static_cast<size_t>(i) + 1] - idx.offsets[static_cast<size_t>(i)];
    return true;
}

// -- DICT (CFF spec section 4) ---------------------------------------------
// Operator keys are the 1-byte operator value, or 1200+XX for the
// 2-byte "escape" operators (12 XX) -- distinguishing e.g. plain `7`
// from `12 7` (FontMatrix) without a second map.

double ParseDictReal(const unsigned char *p, size_t len, size_t &i) {
    std::string s;
    bool done = false;
    while (i < len && !done) {
        uint8_t byte = p[i++];
        for (int half = 0; half < 2; ++half) {
            int nibble = half == 0 ? (byte >> 4) : (byte & 0xf);
            if (nibble <= 9) {
                s.push_back(static_cast<char>('0' + nibble));
            } else if (nibble == 0xa) {
                s.push_back('.');
            } else if (nibble == 0xb) {
                s.push_back('E');
            } else if (nibble == 0xc) {
                s += "E-";
            } else if (nibble == 0xe) {
                s.push_back('-');
            } else if (nibble == 0xf) {
                done = true;
                break;
            }
            // 0xd: reserved, skip.
        }
    }
    return std::strtod(s.c_str(), nullptr);
}

bool ParseDict(const unsigned char *p, size_t len, std::map<int, std::vector<double>> *out) {
    std::vector<double> operands;
    size_t i = 0;
    while (i < len) {
        uint8_t b0 = p[i];
        if (b0 <= 21) {
            int op = b0;
            ++i;
            if (b0 == 12) {
                if (i >= len) return false;
                op = 1200 + p[i];
                ++i;
            }
            (*out)[op] = operands;
            operands.clear();
        } else if (b0 == 28) {
            if (i + 3 > len) return false;
            int16_t v = static_cast<int16_t>((p[i + 1] << 8) | p[i + 2]);
            operands.push_back(v);
            i += 3;
        } else if (b0 == 29) {
            if (i + 5 > len) return false;
            int32_t v = (p[i + 1] << 24) | (p[i + 2] << 16) | (p[i + 3] << 8) | p[i + 4];
            operands.push_back(v);
            i += 5;
        } else if (b0 == 30) {
            ++i;
            operands.push_back(ParseDictReal(p, len, i));
        } else if (b0 >= 32 && b0 <= 246) {
            operands.push_back(static_cast<double>(static_cast<int>(b0) - 139));
            ++i;
        } else if (b0 >= 247 && b0 <= 250) {
            if (i + 2 > len) return false;
            operands.push_back(static_cast<double>((b0 - 247) * 256 + p[i + 1] + 108));
            i += 2;
        } else if (b0 >= 251 && b0 <= 254) {
            if (i + 2 > len) return false;
            operands.push_back(static_cast<double>(-(b0 - 251) * 256 - p[i + 1] - 108));
            i += 2;
        } else {
            return false;  // 255: reserved, not used in Top/Private DICTs
        }
    }
    return true;
}

const std::vector<double> *DictGet(const std::map<int, std::vector<double>> &dict, int op) {
    auto it = dict.find(op);
    return it == dict.end() ? nullptr : &it->second;
}

// -- charset (CFF spec section 13) -----------------------------------------
// Maps gid -> SID (non-CID) or gid -> CID (CID-keyed); .notdef (gid 0)
// is always SID/CID 0 and isn't stored in the table itself.

void ParseCharset(const unsigned char *data, int data_size, uint32_t offset, int num_glyphs,
                   std::vector<uint16_t> *out) {
    // Offsets 0/1/2 mean a predefined charset (ISOAdobe/Expert/
    // ExpertSubset) -- essentially never used by embedded PDF fonts
    // (which always carry a custom charset); left empty, tolerated by
    // Phase 10's CID<->GID mapping via an identity fallback.
    if (offset <= 2 || offset >= static_cast<uint32_t>(data_size)) return;
    out->assign(static_cast<size_t>(num_glyphs), 0);
    const unsigned char *p = data + offset;
    size_t remaining = static_cast<size_t>(data_size) - offset;
    if (remaining < 1) return;
    uint8_t format = p[0];
    size_t pos = 1;
    int gid = 1;  // gid 0 (.notdef) is implicit
    if (format == 0) {
        while (gid < num_glyphs && pos + 2 <= remaining) {
            (*out)[static_cast<size_t>(gid)] = U16(p + pos);
            pos += 2;
            ++gid;
        }
    } else if (format == 1 || format == 2) {
        size_t left_size = format == 1 ? 1 : 2;
        while (gid < num_glyphs && pos + 2 + left_size <= remaining) {
            uint16_t first = U16(p + pos);
            pos += 2;
            uint32_t n_left = left_size == 1 ? p[pos] : U16(p + pos);
            pos += left_size;
            for (uint32_t k = 0; k <= n_left && gid < num_glyphs; ++k) (*out)[static_cast<size_t>(gid++)] = static_cast<uint16_t>(first + k);
        }
    }
}

// -- FDSelect (CFF spec section 19) ----------------------------------------

void ParseFdSelect(const unsigned char *data, int data_size, uint32_t offset, int num_glyphs,
                    std::vector<uint8_t> *out) {
    out->assign(static_cast<size_t>(num_glyphs), 0);
    if (offset >= static_cast<uint32_t>(data_size)) return;
    const unsigned char *p = data + offset;
    size_t remaining = static_cast<size_t>(data_size) - offset;
    if (remaining < 1) return;
    uint8_t format = p[0];
    if (format == 0) {
        if (remaining < 1 + static_cast<size_t>(num_glyphs)) return;
        for (int i = 0; i < num_glyphs; ++i) (*out)[static_cast<size_t>(i)] = p[1 + static_cast<size_t>(i)];
    } else if (format == 3) {
        if (remaining < 3) return;
        int n_ranges = U16(p + 1);
        size_t pos = 3;
        for (int r = 0; r < n_ranges; ++r) {
            if (pos + 3 > remaining) return;
            uint16_t first = U16(p + pos);
            uint8_t fd = p[pos + 2];
            uint16_t next_first = pos + 5 <= remaining ? U16(p + pos + 3) : static_cast<uint16_t>(num_glyphs);
            for (int g = first; g < next_first && g < num_glyphs; ++g) (*out)[static_cast<size_t>(g)] = fd;
            pos += 3;
        }
    }
}

// -- Type2 charstring interpreter ------------------------------------------

struct PathElem {
    enum Kind { kLine, kCurve } kind = kLine;
    float x = 0, y = 0;
    float c1x = 0, c1y = 0, c2x = 0, c2y = 0;
};

struct Contour {
    float start_x = 0, start_y = 0;
    std::vector<PathElem> elems;
};

double ReadT2Number(const unsigned char *p, size_t &i) {
    uint8_t b0 = p[i];
    if (b0 == 28) {
        int16_t v = static_cast<int16_t>((p[i + 1] << 8) | p[i + 2]);
        i += 3;
        return v;
    }
    if (b0 >= 32 && b0 <= 246) {
        ++i;
        return static_cast<double>(static_cast<int>(b0) - 139);
    }
    if (b0 >= 247 && b0 <= 250) {
        double v = (b0 - 247) * 256 + p[i + 1] + 108;
        i += 2;
        return v;
    }
    if (b0 >= 251 && b0 <= 254) {
        double v = -(b0 - 251) * 256 - p[i + 1] - 108;
        i += 2;
        return v;
    }
    // b0 == 255: Fixed 16.16.
    int32_t v = (p[i + 1] << 24) | (p[i + 2] << 16) | (p[i + 3] << 8) | p[i + 4];
    i += 5;
    return static_cast<double>(v) / 65536.0;
}

int SubrBias(int count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; }

struct T2Interp {
    std::vector<double> stack;
    float x = 0, y = 0;
    int stem_count = 0;
    bool width_parsed = false;
    bool open_contour = false;
    std::vector<Contour> contours;
    const Index *global_subrs = nullptr;
    const Index *local_subrs = nullptr;
    int global_bias = 0, local_bias = 0;
    int depth = 0;
    bool done = false;

    // Removes a leading width argument the first time a stack-clearing
    // operator runs, if the stack has more operands than that operator
    // expects (spec 16-bit Type 2 Charstring Format, section 5, "Width").
    void ConsumeWidthIfPresent(size_t expected) {
        if (width_parsed) return;
        width_parsed = true;
        if (stack.size() > expected) stack.erase(stack.begin());
    }

    void MoveTo(float dx, float dy) {
        x += dx;
        y += dy;
        Contour c;
        c.start_x = x;
        c.start_y = y;
        contours.push_back(c);
        open_contour = true;
    }
    void LineTo(float dx, float dy) {
        if (!open_contour) MoveTo(0, 0);  // tolerate a charstring lacking an initial moveto
        x += dx;
        y += dy;
        PathElem e;
        e.kind = PathElem::kLine;
        e.x = x;
        e.y = y;
        contours.back().elems.push_back(e);
    }
    void CurveTo(float dx1, float dy1, float dx2, float dy2, float dx3, float dy3) {
        if (!open_contour) MoveTo(0, 0);
        float c1x = x + dx1, c1y = y + dy1;
        float c2x = c1x + dx2, c2y = c1y + dy2;
        float ex = c2x + dx3, ey = c2y + dy3;
        PathElem e;
        e.kind = PathElem::kCurve;
        e.c1x = c1x;
        e.c1y = c1y;
        e.c2x = c2x;
        e.c2y = c2y;
        e.x = ex;
        e.y = ey;
        contours.back().elems.push_back(e);
        x = ex;
        y = ey;
    }
};

void RunVvCurveTo(T2Interp &t, const std::vector<double> &args) {
    size_t i = 0;
    float dx1 = 0;
    if (args.size() % 4 == 1) {
        dx1 = static_cast<float>(args[0]);
        i = 1;
    }
    while (i + 4 <= args.size()) {
        t.CurveTo(dx1, static_cast<float>(args[i]), static_cast<float>(args[i + 1]), static_cast<float>(args[i + 2]), 0,
                  static_cast<float>(args[i + 3]));
        dx1 = 0;
        i += 4;
    }
}
void RunHhCurveTo(T2Interp &t, const std::vector<double> &args) {
    size_t i = 0;
    float dy1 = 0;
    if (args.size() % 4 == 1) {
        dy1 = static_cast<float>(args[0]);
        i = 1;
    }
    while (i + 4 <= args.size()) {
        t.CurveTo(static_cast<float>(args[i]), dy1, static_cast<float>(args[i + 1]), static_cast<float>(args[i + 2]),
                  static_cast<float>(args[i + 3]), 0);
        dy1 = 0;
        i += 4;
    }
}
// hvcurveto (horiz=true first) / vhcurveto (horiz=false first): curves
// alternate which axis each one starts tangent to; the final curve may
// carry one extra trailing argument for its otherwise-implicit-zero
// "other axis" delta.
void RunAlternatingCurveTo(T2Interp &t, const std::vector<double> &args, bool horiz) {
    size_t i = 0;
    size_t n = args.size();
    while (i + 4 <= n) {
        bool last = (i + 4 == n - 1);
        double d0 = args[i], d1 = args[i + 1], d2 = args[i + 2], d3 = args[i + 3];
        double dlast = last ? args[i + 4] : 0.0;
        if (horiz) {
            t.CurveTo(static_cast<float>(d0), 0, static_cast<float>(d1), static_cast<float>(d2), static_cast<float>(dlast),
                      static_cast<float>(d3));
        } else {
            t.CurveTo(0, static_cast<float>(d0), static_cast<float>(d1), static_cast<float>(d2), static_cast<float>(d3),
                      static_cast<float>(dlast));
        }
        i += 4;
        horiz = !horiz;
    }
}

void RunCharstring(const unsigned char *data, size_t len, T2Interp &t) {
    if (t.depth > 10 || t.done) return;  // guard against a malformed self-referential subr chain
    ++t.depth;
    size_t i = 0;
    while (i < len && !t.done) {
        uint8_t b0 = data[i];
        if (b0 >= 32 || b0 == 28) {
            t.stack.push_back(ReadT2Number(data, i));
            continue;
        }
        ++i;
        switch (b0) {
            case 1:  // hstemhm
            case 3:  // vstem
            case 18:  // hstemhm
            case 23:  // vstemhm
                t.ConsumeWidthIfPresent(t.stack.size() % 2 == 0 ? t.stack.size() : t.stack.size() - 1);
                t.stem_count += static_cast<int>(t.stack.size() / 2);
                t.stack.clear();
                break;
            case 19:  // hintmask
            case 20: {  // cntrmask
                if (!t.stack.empty()) {
                    t.ConsumeWidthIfPresent(t.stack.size() % 2 == 0 ? t.stack.size() : t.stack.size() - 1);
                    t.stem_count += static_cast<int>(t.stack.size() / 2);
                }
                t.stack.clear();
                size_t mask_bytes = static_cast<size_t>((t.stem_count + 7) / 8);
                i += mask_bytes;
                break;
            }
            case 21:  // rmoveto
                t.ConsumeWidthIfPresent(2);
                if (t.stack.size() >= 2) t.MoveTo(static_cast<float>(t.stack[0]), static_cast<float>(t.stack[1]));
                t.stack.clear();
                break;
            case 22:  // hmoveto
                t.ConsumeWidthIfPresent(1);
                if (!t.stack.empty()) t.MoveTo(static_cast<float>(t.stack[0]), 0);
                t.stack.clear();
                break;
            case 4:  // vmoveto
                t.ConsumeWidthIfPresent(1);
                if (!t.stack.empty()) t.MoveTo(0, static_cast<float>(t.stack[0]));
                t.stack.clear();
                break;
            case 5:  // rlineto
                for (size_t k = 0; k + 2 <= t.stack.size(); k += 2) {
                    t.LineTo(static_cast<float>(t.stack[k]), static_cast<float>(t.stack[k + 1]));
                }
                t.stack.clear();
                break;
            case 6:  // hlineto
            case 7: {  // vlineto
                bool horiz = (b0 == 6);
                for (double v : t.stack) {
                    if (horiz)
                        t.LineTo(static_cast<float>(v), 0);
                    else
                        t.LineTo(0, static_cast<float>(v));
                    horiz = !horiz;
                }
                t.stack.clear();
                break;
            }
            case 8:  // rrcurveto
                for (size_t k = 0; k + 6 <= t.stack.size(); k += 6) {
                    t.CurveTo(static_cast<float>(t.stack[k]), static_cast<float>(t.stack[k + 1]), static_cast<float>(t.stack[k + 2]),
                              static_cast<float>(t.stack[k + 3]), static_cast<float>(t.stack[k + 4]), static_cast<float>(t.stack[k + 5]));
                }
                t.stack.clear();
                break;
            case 24: {  // rcurveline
                size_t k = 0;
                for (; k + 6 <= t.stack.size() - 2; k += 6) {
                    t.CurveTo(static_cast<float>(t.stack[k]), static_cast<float>(t.stack[k + 1]), static_cast<float>(t.stack[k + 2]),
                              static_cast<float>(t.stack[k + 3]), static_cast<float>(t.stack[k + 4]), static_cast<float>(t.stack[k + 5]));
                }
                if (k + 2 <= t.stack.size()) t.LineTo(static_cast<float>(t.stack[k]), static_cast<float>(t.stack[k + 1]));
                t.stack.clear();
                break;
            }
            case 25: {  // rlinecurve
                size_t k = 0;
                for (; k + 2 <= t.stack.size() - 6; k += 2) {
                    t.LineTo(static_cast<float>(t.stack[k]), static_cast<float>(t.stack[k + 1]));
                }
                if (k + 6 <= t.stack.size()) {
                    t.CurveTo(static_cast<float>(t.stack[k]), static_cast<float>(t.stack[k + 1]), static_cast<float>(t.stack[k + 2]),
                              static_cast<float>(t.stack[k + 3]), static_cast<float>(t.stack[k + 4]), static_cast<float>(t.stack[k + 5]));
                }
                t.stack.clear();
                break;
            }
            case 26:  // vvcurveto
                RunVvCurveTo(t, t.stack);
                t.stack.clear();
                break;
            case 27:  // hhcurveto
                RunHhCurveTo(t, t.stack);
                t.stack.clear();
                break;
            case 30:  // vhcurveto
                RunAlternatingCurveTo(t, t.stack, false);
                t.stack.clear();
                break;
            case 31:  // hvcurveto
                RunAlternatingCurveTo(t, t.stack, true);
                t.stack.clear();
                break;
            case 10: {  // callsubr
                if (t.stack.empty() || !t.local_subrs) break;
                int idx = static_cast<int>(t.stack.back()) + t.local_bias;
                t.stack.pop_back();
                const unsigned char *sp = nullptr;
                uint32_t slen = 0;
                if (GetItem(*t.local_subrs, idx, &sp, &slen)) RunCharstring(sp, slen, t);
                break;
            }
            case 29: {  // callgsubr
                if (t.stack.empty() || !t.global_subrs) break;
                int idx = static_cast<int>(t.stack.back()) + t.global_bias;
                t.stack.pop_back();
                const unsigned char *sp = nullptr;
                uint32_t slen = 0;
                if (GetItem(*t.global_subrs, idx, &sp, &slen)) RunCharstring(sp, slen, t);
                break;
            }
            case 11:  // return
                --t.depth;
                return;
            case 14:  // endchar
                t.ConsumeWidthIfPresent(t.stack.size() == 4 ? 4 : 0);
                // A remaining 4 operands here would be the deprecated
                // seac-like accent-composition form -- not implemented
                // (see this file's own top comment); the base glyph
                // outline built so far is still used as-is.
                t.stack.clear();
                t.done = true;
                break;
            case 12: {  // escape: 2-byte operators
                if (i >= len) {
                    t.stack.clear();
                    break;
                }
                uint8_t b1 = data[i];
                ++i;
                if (b1 == 35 && t.stack.size() >= 13) {  // flex
                    const auto &s = t.stack;
                    t.CurveTo(static_cast<float>(s[0]), static_cast<float>(s[1]), static_cast<float>(s[2]), static_cast<float>(s[3]),
                              static_cast<float>(s[4]), static_cast<float>(s[5]));
                    t.CurveTo(static_cast<float>(s[6]), static_cast<float>(s[7]), static_cast<float>(s[8]), static_cast<float>(s[9]),
                              static_cast<float>(s[10]), static_cast<float>(s[11]));
                } else if (b1 == 34 && t.stack.size() >= 7) {  // hflex
                    const auto &s = t.stack;
                    float dy2 = static_cast<float>(s[2]);
                    t.CurveTo(static_cast<float>(s[0]), 0, static_cast<float>(s[1]), dy2, static_cast<float>(s[3]), 0);
                    t.CurveTo(static_cast<float>(s[4]), 0, static_cast<float>(s[5]), -dy2, static_cast<float>(s[6]), 0);
                } else if (b1 == 36 && t.stack.size() >= 9) {  // hflex1
                    const auto &s = t.stack;
                    float dy1 = static_cast<float>(s[1]), dy2 = static_cast<float>(s[3]), dy5 = static_cast<float>(s[7]);
                    t.CurveTo(static_cast<float>(s[0]), dy1, static_cast<float>(s[2]), dy2, static_cast<float>(s[4]), 0);
                    t.CurveTo(static_cast<float>(s[5]), 0, static_cast<float>(s[6]), dy5, static_cast<float>(s[8]),
                              -(dy1 + dy2 + dy5));
                } else if (b1 == 37 && t.stack.size() >= 11) {  // flex1
                    const auto &s = t.stack;
                    double dx = s[0] + s[2] + s[4] + s[6] + s[8];
                    double dy = s[1] + s[3] + s[5] + s[7] + s[9];
                    t.CurveTo(static_cast<float>(s[0]), static_cast<float>(s[1]), static_cast<float>(s[2]), static_cast<float>(s[3]),
                              static_cast<float>(s[4]), static_cast<float>(s[5]));
                    if (std::abs(dx) > std::abs(dy)) {
                        t.CurveTo(static_cast<float>(s[6]), static_cast<float>(s[7]), static_cast<float>(s[8]), static_cast<float>(s[9]),
                                  static_cast<float>(s[10]), static_cast<float>(-dy));
                    } else {
                        t.CurveTo(static_cast<float>(s[6]), static_cast<float>(s[7]), static_cast<float>(s[8]), static_cast<float>(s[9]),
                                  static_cast<float>(-dx), static_cast<float>(s[10]));
                    }
                }
                // Any other escape operator (the arithmetic/logical/
                // storage set, see this file's own top comment): tolerated,
                // stack simply cleared below.
                t.stack.clear();
                break;
            }
            default:
                // Unrecognized operator: tolerate, matching this
                // codebase's "skip bad content" convention.
                t.stack.clear();
                break;
        }
    }
    --t.depth;
}

}  // namespace

bool GetIndexItem(const Index &index, int i, const unsigned char **out_ptr, uint32_t *out_len) {
    return GetItem(index, i, out_ptr, out_len);
}

bool InitFont(FontInfo *info, const unsigned char *data, int data_size) {
    if (!data || data_size < 4) return false;
    *info = FontInfo();
    info->data = data;
    info->data_size = data_size;

    uint8_t hdr_size = data[2];
    if (hdr_size >= static_cast<uint8_t>(data_size)) return false;
    size_t pos = hdr_size;
    size_t remaining = static_cast<size_t>(data_size) - pos;

    Index name_index, top_dict_index;
    size_t consumed = 0;
    if (!ParseIndex(data + pos, remaining, &name_index, &consumed)) return false;
    pos += consumed;
    remaining -= consumed;
    if (!ParseIndex(data + pos, remaining, &top_dict_index, &consumed)) return false;
    pos += consumed;
    remaining -= consumed;
    if (!ParseIndex(data + pos, remaining, &info->strings, &consumed)) return false;
    pos += consumed;
    remaining -= consumed;
    if (!ParseIndex(data + pos, remaining, &info->global_subrs, &consumed)) return false;

    const unsigned char *top_dict_data = nullptr;
    uint32_t top_dict_len = 0;
    if (!GetItem(top_dict_index, 0, &top_dict_data, &top_dict_len)) return false;
    std::map<int, std::vector<double>> top_dict;
    if (!ParseDict(top_dict_data, top_dict_len, &top_dict)) return false;

    if (const std::vector<double> *cst = DictGet(top_dict, 1206)) {  // CharstringType, default 2
        if (!cst->empty() && (*cst)[0] != 2) return false;           // Type 1 charstrings: out of scope
    }
    if (const std::vector<double> *fm = DictGet(top_dict, 1207)) {  // FontMatrix
        if (fm->size() == 6) {
            for (int k = 0; k < 6; ++k) info->font_matrix[k] = (*fm)[static_cast<size_t>(k)];
        }
    }

    const std::vector<double> *cs_off = DictGet(top_dict, 17);  // CharStrings
    if (!cs_off || cs_off->empty()) return false;
    uint32_t charstrings_off = static_cast<uint32_t>((*cs_off)[0]);
    if (charstrings_off >= static_cast<uint32_t>(data_size)) return false;
    size_t cs_consumed = 0;
    if (!ParseIndex(data + charstrings_off, static_cast<size_t>(data_size) - charstrings_off, &info->charstrings,
                     &cs_consumed)) {
        return false;
    }
    info->num_glyphs = info->charstrings.count;

    info->is_cid = DictGet(top_dict, 1230) != nullptr;  // ROS

    if (const std::vector<double> *charset_off = DictGet(top_dict, 15)) {
        if (!charset_off->empty()) {
            ParseCharset(data, data_size, static_cast<uint32_t>((*charset_off)[0]), info->num_glyphs, &info->charset);
        }
    }

    if (info->is_cid) {
        const std::vector<double> *fda = DictGet(top_dict, 1236);  // FDArray
        const std::vector<double> *fds = DictGet(top_dict, 1237);  // FDSelect
        if (!fda || fda->empty() || !fds || fds->empty()) return false;
        uint32_t fdarray_off = static_cast<uint32_t>((*fda)[0]);
        if (fdarray_off >= static_cast<uint32_t>(data_size)) return false;
        Index fd_array_index;
        size_t fd_consumed = 0;
        if (!ParseIndex(data + fdarray_off, static_cast<size_t>(data_size) - fdarray_off, &fd_array_index, &fd_consumed)) {
            return false;
        }
        for (int i = 0; i < fd_array_index.count; ++i) {
            const unsigned char *fd_data = nullptr;
            uint32_t fd_len = 0;
            FdEntry entry;
            if (GetItem(fd_array_index, i, &fd_data, &fd_len)) {
                std::map<int, std::vector<double>> fd_dict;
                if (ParseDict(fd_data, fd_len, &fd_dict)) {
                    if (const std::vector<double> *priv = DictGet(fd_dict, 18); priv && priv->size() == 2) {
                        uint32_t priv_size = static_cast<uint32_t>((*priv)[0]);
                        uint32_t priv_off = static_cast<uint32_t>((*priv)[1]);
                        if (priv_off < static_cast<uint32_t>(data_size) && priv_off + priv_size <= static_cast<uint32_t>(data_size)) {
                            std::map<int, std::vector<double>> priv_dict;
                            if (ParseDict(data + priv_off, priv_size, &priv_dict)) {
                                if (const std::vector<double> *subrs = DictGet(priv_dict, 19); subrs && !subrs->empty()) {
                                    uint32_t subrs_off = priv_off + static_cast<uint32_t>((*subrs)[0]);
                                    size_t subrs_consumed = 0;
                                    if (subrs_off < static_cast<uint32_t>(data_size)) {
                                        ParseIndex(data + subrs_off, static_cast<size_t>(data_size) - subrs_off, &entry.local_subrs,
                                                   &subrs_consumed);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            info->fd_array.push_back(entry);
        }
        ParseFdSelect(data, data_size, static_cast<uint32_t>((*fds)[0]), info->num_glyphs, &info->fd_select);
    } else if (const std::vector<double> *priv = DictGet(top_dict, 18); priv && priv->size() == 2) {
        uint32_t priv_size = static_cast<uint32_t>((*priv)[0]);
        uint32_t priv_off = static_cast<uint32_t>((*priv)[1]);
        if (priv_off < static_cast<uint32_t>(data_size) && priv_off + priv_size <= static_cast<uint32_t>(data_size)) {
            std::map<int, std::vector<double>> priv_dict;
            if (ParseDict(data + priv_off, priv_size, &priv_dict)) {
                if (const std::vector<double> *subrs = DictGet(priv_dict, 19); subrs && !subrs->empty()) {
                    uint32_t subrs_off = priv_off + static_cast<uint32_t>((*subrs)[0]);
                    size_t subrs_consumed = 0;
                    if (subrs_off < static_cast<uint32_t>(data_size)) {
                        ParseIndex(data + subrs_off, static_cast<size_t>(data_size) - subrs_off, &info->local_subrs,
                                   &subrs_consumed);
                    }
                }
            }
        }
    }

    return info->num_glyphs > 0;
}

unsigned char *GetGlyphBitmap(const FontInfo *info, float scale_x, float scale_y, int glyph_index, int *width,
                               int *height, int *xoff, int *yoff) {
    *width = *height = *xoff = *yoff = 0;
    if (glyph_index < 0 || glyph_index >= info->num_glyphs) return nullptr;

    const unsigned char *cs_data = nullptr;
    uint32_t cs_len = 0;
    if (!GetItem(info->charstrings, glyph_index, &cs_data, &cs_len)) return nullptr;

    T2Interp t;
    t.global_subrs = &info->global_subrs;
    t.global_bias = SubrBias(info->global_subrs.count);
    if (info->is_cid) {
        int fd = glyph_index < static_cast<int>(info->fd_select.size()) ? info->fd_select[static_cast<size_t>(glyph_index)] : 0;
        if (fd >= 0 && fd < static_cast<int>(info->fd_array.size())) {
            t.local_subrs = &info->fd_array[static_cast<size_t>(fd)].local_subrs;
        }
    } else {
        t.local_subrs = &info->local_subrs;
    }
    t.local_bias = t.local_subrs ? SubrBias(t.local_subrs->count) : 0;

    RunCharstring(cs_data, cs_len, t);
    if (t.contours.empty()) return nullptr;

    float xmin = 1e30f, ymin = 1e30f, xmax = -1e30f, ymax = -1e30f;
    auto consider = [&](float px, float py) {
        xmin = std::min(xmin, px);
        xmax = std::max(xmax, px);
        ymin = std::min(ymin, py);
        ymax = std::max(ymax, py);
    };
    for (const Contour &c : t.contours) {
        consider(c.start_x, c.start_y);
        for (const PathElem &e : c.elems) consider(e.x, e.y);
    }
    if (xmax <= xmin || ymax <= ymin) return nullptr;

    int ix0 = static_cast<int>(std::floor(xmin * scale_x));
    int ix1 = static_cast<int>(std::ceil(xmax * scale_x));
    int iy0 = static_cast<int>(std::floor(-ymax * scale_y));  // font y-up -> raster y-down
    int iy1 = static_cast<int>(std::ceil(-ymin * scale_y));
    int w = ix1 - ix0;
    int h = iy1 - iy0;
    if (w <= 0 || h <= 0) return nullptr;

    auto to_raster_x = [&](float px) { return px * scale_x - static_cast<float>(ix0); };
    auto to_raster_y = [&](float py) { return -py * scale_y - static_cast<float>(iy0); };

    std::vector<raster::Edge> edges;
    for (const Contour &c : t.contours) {
        float cur_x = to_raster_x(c.start_x), cur_y = to_raster_y(c.start_y);
        float start_x = cur_x, start_y = cur_y;
        for (const PathElem &e : c.elems) {
            float ex = to_raster_x(e.x), ey = to_raster_y(e.y);
            if (e.kind == PathElem::kLine) {
                raster::AddLine(edges, cur_x, cur_y, ex, ey);
            } else {
                raster::FlattenCubic(edges, cur_x, cur_y, to_raster_x(e.c1x), to_raster_y(e.c1y), to_raster_x(e.c2x),
                                      to_raster_y(e.c2y), ex, ey);
            }
            cur_x = ex;
            cur_y = ey;
        }
        raster::AddLine(edges, cur_x, cur_y, start_x, start_y);  // implicit close, matches endchar's own semantics
    }

    std::vector<unsigned char> pixels = raster::Rasterize(edges, w, h);
    auto *out = static_cast<unsigned char *>(std::malloc(pixels.size()));
    std::memcpy(out, pixels.data(), pixels.size());

    *width = w;
    *height = h;
    *xoff = ix0;
    *yoff = iy0;
    return out;
}

void FreeBitmap(unsigned char *bitmap) { std::free(bitmap); }

}  // namespace cff
}  // namespace gfx
