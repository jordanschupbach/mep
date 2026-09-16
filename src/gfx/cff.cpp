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

// -- Standard Encoding (CFF spec Appendix B): code -> SID -------------------
// The predefined encoding a CFF font declares by omitting its Encoding
// offset (or writing 0) -- and the one `seac`'s bchar/achar codes are
// always interpreted through, whatever the font's own encoding is.

const uint16_t kStandardEncodingSid[256] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    // 0-15
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    // 16-31
    1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,   // 32-47
    17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,   // 48-63
    33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,   // 64-79
    49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,   // 80-95
    65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,   // 96-111
    81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  0,    // 112-127
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    // 128-143
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    // 144-159
    0,   96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110,  // 160-175
    0,   111, 112, 113, 114, 0,   115, 116, 117, 118, 119, 120, 121, 122, 0,   123,  // 176-191
    0,   124, 125, 126, 127, 128, 129, 130, 131, 0,   132, 133, 0,   134, 135, 136,  // 192-207
    137, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    // 208-223
    0,   138, 0,   139, 0,   0,   0,   0,   140, 141, 142, 143, 0,   0,   0,   0,    // 224-239
    0,   144, 0,   0,   0,   145, 0,   0,   146, 147, 148, 149, 0,   0,   0,   0,    // 240-255
};

// -- Encoding (CFF spec section 12) ----------------------------------------
// Maps code -> GID for a font's own built-in encoding: format 0 (a
// code per glyph, in GID order) or 1 (ranges of consecutive codes),
// either optionally followed (high bit of the format byte) by
// supplements mapping extra codes to glyphs by SID.

void ParseEncoding(const unsigned char *data, int data_size, uint32_t offset, const std::vector<uint16_t> &charset,
                   int num_glyphs, FontInfo *info) {
    if (offset >= static_cast<uint32_t>(data_size)) return;
    info->encoding_standard = false;
    info->builtin_encoding.assign(256, -1);
    const unsigned char *p = data + offset;
    size_t remaining = static_cast<size_t>(data_size) - offset;
    if (remaining < 1) return;
    uint8_t format = p[0];
    size_t pos = 1;
    auto set_code = [&](int code, int gid) {
        if (code >= 0 && code < 256 && gid >= 0 && gid < num_glyphs) info->builtin_encoding[static_cast<size_t>(code)] = static_cast<int16_t>(gid);
    };
    if ((format & 0x7f) == 0) {
        if (pos >= remaining) return;
        int n_codes = p[pos++];
        for (int i = 1; i <= n_codes && pos < remaining; ++i) set_code(p[pos++], i);
    } else if ((format & 0x7f) == 1) {
        if (pos >= remaining) return;
        int n_ranges = p[pos++];
        int gid = 1;
        for (int r = 0; r < n_ranges && pos + 2 <= remaining; ++r) {
            int first = p[pos];
            int n_left = p[pos + 1];
            pos += 2;
            for (int k = 0; k <= n_left; ++k) set_code(first + k, gid++);
        }
    } else {
        return;
    }
    if (format & 0x80) {
        if (pos >= remaining) return;
        int n_sups = p[pos++];
        for (int i = 0; i < n_sups && pos + 3 <= remaining; ++i) {
            int code = p[pos];
            uint16_t sid = U16(p + pos + 1);
            pos += 3;
            for (int gid = 1; gid < static_cast<int>(charset.size()); ++gid) {
                if (charset[static_cast<size_t>(gid)] == sid) {
                    set_code(code, gid);
                    break;
                }
            }
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

using raster::OutlineContour;
using raster::OutlineSegment;

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
    std::vector<OutlineContour> contours;
    // Set by a 4-argument `endchar` (the deprecated seac-like accented-
    // character form): BuildGlyphContours composes base + accent glyphs.
    bool has_seac = false;
    float seac_adx = 0, seac_ady = 0;
    int seac_bchar = 0, seac_achar = 0;
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
        OutlineContour c;
        c.start_x = x;
        c.start_y = y;
        contours.push_back(c);
        open_contour = true;
    }
    void LineTo(float dx, float dy) {
        if (!open_contour) MoveTo(0, 0);  // tolerate a charstring lacking an initial moveto
        x += dx;
        y += dy;
        OutlineSegment e;
        e.x = x;
        e.y = y;
        contours.back().segments.push_back(e);
    }
    void CurveTo(float dx1, float dy1, float dx2, float dy2, float dx3, float dy3) {
        if (!open_contour) MoveTo(0, 0);
        float c1x = x + dx1, c1y = y + dy1;
        float c2x = c1x + dx2, c2y = c1y + dy2;
        float ex = c2x + dx3, ey = c2y + dy3;
        OutlineSegment e;
        e.is_curve = true;
        e.c1x = c1x;
        e.c1y = c1y;
        e.c2x = c2x;
        e.c2y = c2y;
        e.x = ex;
        e.y = ey;
        contours.back().segments.push_back(e);
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
                // 4 remaining operands: the deprecated seac-like accent-
                // composition form (adx ady bchar achar, Standard
                // Encoding codes) -- recorded here, composed by
                // BuildGlyphContours, since it needs whole-font state
                // (charset, other charstrings) this interpreter doesn't.
                if (t.stack.size() >= 4) {
                    size_t n = t.stack.size();
                    t.has_seac = true;
                    t.seac_adx = static_cast<float>(t.stack[n - 4]);
                    t.seac_ady = static_cast<float>(t.stack[n - 3]);
                    t.seac_bchar = static_cast<int>(t.stack[n - 2]);
                    t.seac_achar = static_cast<int>(t.stack[n - 1]);
                }
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

    if (!info->is_cid) {
        // Encoding offset 0 (or absent) = Standard Encoding, 1 = Expert
        // (treated as Standard: vanishingly rare, and harmless), else a
        // custom table at that offset. CID-keyed fonts have no encoding
        // at all (codes map to CIDs via the PDF's own CMap).
        if (const std::vector<double> *enc_off = DictGet(top_dict, 16); enc_off && !enc_off->empty()) {
            uint32_t off = static_cast<uint32_t>((*enc_off)[0]);
            if (off > 1) ParseEncoding(data, data_size, off, info->charset, info->num_glyphs, info);
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

int StandardEncodingSid(int code) { return code >= 0 && code < 256 ? kStandardEncodingSid[code] : 0; }

int GidForSid(const FontInfo *info, int sid) {
    if (info->is_cid) return -1;
    if (sid == 0) return 0;
    if (info->charset.empty()) {
        // Predefined ISOAdobe charset: SIDs 0..228 in GID order.
        return sid < info->num_glyphs ? sid : -1;
    }
    for (int gid = 1; gid < static_cast<int>(info->charset.size()); ++gid) {
        if (info->charset[static_cast<size_t>(gid)] == sid) return gid;
    }
    return -1;
}

int BuiltinEncodingGid(const FontInfo *info, int code) {
    if (code < 0 || code > 255 || info->is_cid) return -1;
    if (!info->encoding_standard) {
        return info->builtin_encoding.size() == 256 ? info->builtin_encoding[static_cast<size_t>(code)] : -1;
    }
    int sid = kStandardEncodingSid[code];
    return sid == 0 ? -1 : GidForSid(info, sid);
}

namespace {

// Runs `glyph_index`'s charstring and appends its contours (font units)
// to `out`, composing a seac-style accented character (base glyph plus
// accent glyph offset by (adx, ady)) recursively when `endchar` asked
// for one. `depth` guards a malformed self-referential seac chain.
void BuildGlyphContours(const FontInfo *info, int glyph_index, std::vector<OutlineContour> *out, float dx, float dy,
                        int depth) {
    if (depth > 4 || glyph_index < 0 || glyph_index >= info->num_glyphs) return;
    const unsigned char *cs_data = nullptr;
    uint32_t cs_len = 0;
    if (!GetItem(info->charstrings, glyph_index, &cs_data, &cs_len)) return;

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

    for (OutlineContour &c : t.contours) {
        if (dx != 0 || dy != 0) {
            c.start_x += dx;
            c.start_y += dy;
            for (OutlineSegment &e : c.segments) {
                e.x += dx;
                e.y += dy;
                e.c1x += dx;
                e.c1y += dy;
                e.c2x += dx;
                e.c2y += dy;
            }
        }
        out->push_back(std::move(c));
    }
    if (t.has_seac) {
        int base = GidForSid(info, StandardEncodingSid(t.seac_bchar));
        int accent = GidForSid(info, StandardEncodingSid(t.seac_achar));
        if (base >= 0) BuildGlyphContours(info, base, out, dx, dy, depth + 1);
        if (accent >= 0) BuildGlyphContours(info, accent, out, dx + t.seac_adx, dy + t.seac_ady, depth + 1);
    }
}

}  // namespace

unsigned char *GetGlyphBitmapMatrix(const FontInfo *info, float a, float b, float c, float d, int glyph_index,
                                    int *width, int *height, int *xoff, int *yoff) {
    *width = *height = *xoff = *yoff = 0;
    if (glyph_index < 0 || glyph_index >= info->num_glyphs) return nullptr;
    std::vector<OutlineContour> contours;
    BuildGlyphContours(info, glyph_index, &contours, 0, 0, 0);
    return raster::RasterizeOutline(contours, a, b, c, d, width, height, xoff, yoff);
}

unsigned char *GetGlyphBitmap(const FontInfo *info, float scale_x, float scale_y, int glyph_index, int *width,
                               int *height, int *xoff, int *yoff) {
    return GetGlyphBitmapMatrix(info, scale_x, 0, 0, -scale_y, glyph_index, width, height, xoff, yoff);
}

void FreeBitmap(unsigned char *bitmap) { std::free(bitmap); }

}  // namespace cff
}  // namespace gfx
