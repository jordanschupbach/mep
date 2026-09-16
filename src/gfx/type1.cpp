#include "gfx/type1.h"

#include "gfx/rasterizer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace gfx {
namespace t1 {

namespace {

using raster::OutlineContour;
using raster::OutlineSegment;

// StandardEncoding (Type 1 spec Appendix E / CFF standard strings 1-149
// in that same order): code -> name.
const char *const kStandardNames[149] = {
    "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quoteright", "parenleft",
    "parenright", "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero", "one", "two", "three", "four",
    "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "at", "A",
    "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum", "underscore", "quoteleft", "a", "b", "c",
    "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "braceleft", "bar", "braceright", "asciitilde", "exclamdown", "cent", "sterling", "fraction", "yen", "florin",
    "section", "currency", "quotesingle", "quotedblleft", "guillemotleft", "guilsinglleft", "guilsinglright", "fi",
    "fl", "endash", "dagger", "daggerdbl", "periodcentered", "paragraph", "bullet", "quotesinglbase", "quotedblbase",
    "quotedblright", "guillemotright", "ellipsis", "perthousand", "questiondown", "grave", "acute", "circumflex",
    "tilde", "macron", "breve", "dotaccent", "dieresis", "ring", "cedilla", "hungarumlaut", "ogonek", "caron",
    "emdash", "AE", "ordfeminine", "Lslash", "Oslash", "OE", "ordmasculine", "ae", "dotlessi", "lslash", "oslash",
    "oe", "germandbls"};

// code -> index into kStandardNames + 1 (0 = unencoded); same table as
// gfx/cff.cpp's Standard Encoding code -> SID.
const uint8_t kStandardCodeToIndex[256] = {
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

// -- eexec / charstring decryption (Type 1 spec chapter 7) ------------------

std::string Decrypt(const unsigned char *p, size_t len, uint16_t r, int skip) {
    const uint16_t c1 = 52845, c2 = 22719;
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = p[i];
        unsigned char plain = static_cast<unsigned char>(c ^ (r >> 8));
        r = static_cast<uint16_t>((c + r) * c1 + c2);
        if (static_cast<int>(i) >= skip) out.push_back(static_cast<char>(plain));
    }
    return out;
}

bool IsHex(unsigned char c) { return std::isxdigit(c) != 0; }
int HexVal(unsigned char c) { return std::isdigit(c) ? c - '0' : (std::tolower(c) - 'a' + 10); }
bool IsWs(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0'; }

// -- tiny PostScript-ish tokenizer over a byte range -----------------------

struct Scanner {
    const std::string &s;
    size_t pos = 0;
    explicit Scanner(const std::string &str, size_t start = 0) : s(str), pos(start) {}
    bool End() const { return pos >= s.size(); }
    void SkipWs() {
        while (pos < s.size() && IsWs(static_cast<unsigned char>(s[pos]))) ++pos;
    }
    // Next whitespace/delimiter-separated token ('/' and '[' ']' '{' '}' are their own delimiters, PostScript-style).
    std::string Token() {
        SkipWs();
        if (End()) return "";
        size_t start = pos;
        unsigned char c = static_cast<unsigned char>(s[pos]);
        if (c == '[' || c == ']' || c == '{' || c == '}') {
            ++pos;
            return s.substr(start, 1);
        }
        if (c == '/') ++pos;
        while (pos < s.size()) {
            unsigned char d = static_cast<unsigned char>(s[pos]);
            if (IsWs(d) || d == '/' || d == '[' || d == ']' || d == '{' || d == '}' || d == '(') break;
            ++pos;
        }
        return s.substr(start, pos - start);
    }
    bool Int(int *out) {
        std::string t = Token();
        if (t.empty()) return false;
        char *end = nullptr;
        long v = std::strtol(t.c_str(), &end, 10);
        if (end == t.c_str()) return false;
        *out = static_cast<int>(v);
        return true;
    }
};

// Parses `/Encoding` out of the cleartext portion.
void ParseEncoding(const std::string &clear, FontInfo *info, std::vector<std::string> *code_names) {
    size_t at = clear.find("/Encoding");
    if (at == std::string::npos) return;
    Scanner sc(clear, at);
    sc.Token();  // "/Encoding"
    std::string first = sc.Token();
    if (first == "StandardEncoding") {
        info->encoding_standard = true;
        for (int c = 0; c < 256; ++c) {
            const char *n = StandardEncodingName(c);
            if (n) (*code_names)[static_cast<size_t>(c)] = n;
        }
        return;
    }
    // Custom: `256 array 0 1 255 {1 index exch /.notdef put} for` then
    // `dup <code> /<name> put` entries, ending at `readonly def`/`def`.
    int guard = 0;
    while (!sc.End() && guard++ < 100000) {
        std::string t = sc.Token();
        if (t.empty() || t == "readonly" || t == "def") break;
        if (t != "dup") continue;
        int code = 0;
        if (!sc.Int(&code)) continue;
        std::string name = sc.Token();
        if (name.size() < 2 || name[0] != '/') continue;
        std::string put = sc.Token();
        if (put != "put") continue;
        if (code >= 0 && code < 256) (*code_names)[static_cast<size_t>(code)] = name.substr(1);
    }
}

void ParseFontMatrix(const std::string &clear, FontInfo *info) {
    size_t at = clear.find("/FontMatrix");
    if (at == std::string::npos) return;
    Scanner sc(clear, at);
    sc.Token();
    if (sc.Token() != "[") return;
    double vals[6];
    for (double &v : vals) {
        std::string t = sc.Token();
        if (t.empty() || t == "]") return;
        v = std::strtod(t.c_str(), nullptr);
    }
    for (int k = 0; k < 6; ++k) info->font_matrix[k] = vals[k];
}

// Reads one `<len> RD <len bytes>` binary blob (the token before the
// data being any name, "RD"/"-|" being the conventional ones) starting
// at the length integer; advances past the blob.
bool ReadBlob(Scanner &sc, std::string *out) {
    int len = 0;
    if (!sc.Int(&len) || len < 0) return false;
    std::string rd = sc.Token();
    if (rd.empty()) return false;
    ++sc.pos;  // exactly one space separates the RD token from the binary data
    if (sc.pos + static_cast<size_t>(len) > sc.s.size()) return false;
    out->assign(sc.s, sc.pos, static_cast<size_t>(len));
    sc.pos += static_cast<size_t>(len);
    return true;
}

void ParsePrivate(const std::string &priv, FontInfo *info) {
    int len_iv = 4;
    if (size_t at = priv.find("/lenIV"); at != std::string::npos) {
        Scanner sc(priv, at);
        sc.Token();
        sc.Int(&len_iv);
    }
    auto decrypt_cs = [&](const std::string &raw) {
        if (len_iv < 0) return raw;  // lenIV -1: charstrings stored unencrypted
        return Decrypt(reinterpret_cast<const unsigned char *>(raw.data()), raw.size(), 4330, len_iv);
    };

    if (size_t at = priv.find("/Subrs"); at != std::string::npos) {
        Scanner sc(priv, at);
        sc.Token();
        int count = 0;
        if (sc.Int(&count) && count > 0 && count < 65536) {
            info->subrs.assign(static_cast<size_t>(count), std::string());
            for (int i = 0; i < count; ++i) {
                size_t dup = priv.find("dup ", sc.pos);
                if (dup == std::string::npos) break;
                sc.pos = dup + 4;
                int idx = 0;
                std::string blob;
                if (!sc.Int(&idx) || !ReadBlob(sc, &blob)) break;
                if (idx >= 0 && idx < count) info->subrs[static_cast<size_t>(idx)] = decrypt_cs(blob);
            }
        }
    }

    size_t cs_at = priv.find("/CharStrings");
    if (cs_at == std::string::npos) return;
    Scanner sc(priv, cs_at);
    sc.Token();
    size_t begin = priv.find("begin", sc.pos);
    if (begin == std::string::npos) return;
    sc.pos = begin + 5;
    int guard = 0;
    while (!sc.End() && guard++ < 70000) {
        sc.SkipWs();
        if (sc.End()) break;
        if (priv.compare(sc.pos, 3, "end") == 0) break;
        std::string t = sc.Token();
        if (t.size() < 2 || t[0] != '/') continue;  // e.g. a stray "ND"/"|-" token between entries
        std::string blob;
        if (!ReadBlob(sc, &blob)) break;
        info->glyph_names.push_back(t.substr(1));
        info->charstrings.push_back(decrypt_cs(blob));
    }
    info->num_glyphs = static_cast<int>(info->charstrings.size());
}

// -- Type 1 charstring interpreter (Type 1 spec chapter 6) ------------------

struct T1Interp {
    const FontInfo *info = nullptr;
    std::vector<double> stack;
    std::vector<double> ps_stack;  // what `pop` retrieves after callothersubr
    double x = 0, y = 0;
    double sbx = 0;
    double advance = 0;
    bool open_contour = false;
    bool in_flex = false;
    std::vector<std::pair<double, double>> flex_points;
    std::vector<OutlineContour> contours;
    int depth = 0;
    bool done = false;
    // seac request (composed by BuildGlyphContours)
    bool has_seac = false;
    double seac_asb = 0, seac_adx = 0, seac_ady = 0;
    int seac_bchar = 0, seac_achar = 0;

    void MoveTo(double nx, double ny) {
        x = nx;
        y = ny;
        if (in_flex) {
            flex_points.emplace_back(x, y);
            return;
        }
        OutlineContour c;
        c.start_x = static_cast<float>(x);
        c.start_y = static_cast<float>(y);
        contours.push_back(c);
        open_contour = true;
    }
    void LineTo(double nx, double ny) {
        if (!open_contour) MoveTo(x, y);
        x = nx;
        y = ny;
        OutlineSegment e;
        e.x = static_cast<float>(x);
        e.y = static_cast<float>(y);
        contours.back().segments.push_back(e);
    }
    void CurveTo(double c1x, double c1y, double c2x, double c2y, double ex, double ey) {
        if (!open_contour) MoveTo(x, y);
        OutlineSegment e;
        e.is_curve = true;
        e.c1x = static_cast<float>(c1x);
        e.c1y = static_cast<float>(c1y);
        e.c2x = static_cast<float>(c2x);
        e.c2y = static_cast<float>(c2y);
        e.x = static_cast<float>(ex);
        e.y = static_cast<float>(ey);
        contours.back().segments.push_back(e);
        x = ex;
        y = ey;
    }
    void RRCurveTo(double dx1, double dy1, double dx2, double dy2, double dx3, double dy3) {
        double c1x = x + dx1, c1y = y + dy1;
        double c2x = c1x + dx2, c2y = c1y + dy2;
        CurveTo(c1x, c1y, c2x, c2y, c2x + dx3, c2y + dy3);
    }
    void ClosePath() { open_contour = false; }  // contours are implicitly closed by the rasterizer

    void CallOtherSubr() {
        if (stack.size() < 2) {
            stack.clear();
            return;
        }
        int othersubr = static_cast<int>(stack.back());
        stack.pop_back();
        int n = static_cast<int>(stack.back());
        stack.pop_back();
        if (n < 0 || static_cast<size_t>(n) > stack.size()) n = static_cast<int>(stack.size());
        std::vector<double> args(stack.end() - n, stack.end());
        stack.resize(stack.size() - static_cast<size_t>(n));
        ps_stack.clear();
        switch (othersubr) {
            case 0: {  // flex end: args = flex depth, end x, end y; 7 collected points (reference + 6)
                in_flex = false;
                if (flex_points.size() >= 7) {
                    const auto &p = flex_points;
                    size_t b = p.size() - 6;
                    CurveTo(p[b].first, p[b].second, p[b + 1].first, p[b + 1].second, p[b + 2].first, p[b + 2].second);
                    CurveTo(p[b + 3].first, p[b + 3].second, p[b + 4].first, p[b + 4].second, p[b + 5].first,
                            p[b + 5].second);
                }
                flex_points.clear();
                double end_x = args.size() >= 3 ? args[1] : x;
                double end_y = args.size() >= 3 ? args[2] : y;
                ps_stack.push_back(end_y);  // popped second
                ps_stack.push_back(end_x);  // popped first
                break;
            }
            case 1:  // flex start
                in_flex = true;
                flex_points.clear();
                break;
            case 2:  // flex point marker: the preceding rmoveto already collected it
                break;
            case 3:  // hint replacement: `pop` yields the subr# for the following callsubr
                ps_stack.push_back(args.empty() ? 3 : args[0]);
                break;
            default:  // unknown: hand the arguments back in order through `pop`
                for (auto it = args.rbegin(); it != args.rend(); ++it) ps_stack.push_back(*it);
                break;
        }
    }
};

void RunCharstring(const std::string &cs, T1Interp &t) {
    if (t.depth > 15 || t.done) return;
    ++t.depth;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(cs.data());
    size_t len = cs.size();
    size_t i = 0;
    auto arg = [&](size_t k) { return k < t.stack.size() ? t.stack[k] : 0.0; };
    while (i < len && !t.done) {
        unsigned char v = p[i++];
        if (v >= 32) {
            if (v <= 246) {
                t.stack.push_back(static_cast<double>(static_cast<int>(v) - 139));
            } else if (v <= 250) {
                if (i >= len) break;
                t.stack.push_back(static_cast<double>((static_cast<int>(v) - 247) * 256 + p[i++] + 108));
            } else if (v <= 254) {
                if (i >= len) break;
                t.stack.push_back(static_cast<double>(-(static_cast<int>(v) - 251) * 256 - p[i++] - 108));
            } else {
                if (i + 4 > len) break;
                int32_t n = static_cast<int32_t>((static_cast<uint32_t>(p[i]) << 24) | (static_cast<uint32_t>(p[i + 1]) << 16) |
                                                 (static_cast<uint32_t>(p[i + 2]) << 8) | p[i + 3]);
                i += 4;
                t.stack.push_back(static_cast<double>(n));
            }
            continue;
        }
        switch (v) {
            case 13:  // hsbw: sbx wx
                t.sbx = arg(0);
                t.advance = arg(1);
                t.x = t.sbx;
                t.y = 0;
                t.stack.clear();
                break;
            case 9:  // closepath
                t.ClosePath();
                t.stack.clear();
                break;
            case 1:  // hstem
            case 3:  // vstem
                t.stack.clear();
                break;
            case 21:  // rmoveto
                t.MoveTo(t.x + arg(0), t.y + arg(1));
                t.stack.clear();
                break;
            case 22:  // hmoveto
                t.MoveTo(t.x + arg(0), t.y);
                t.stack.clear();
                break;
            case 4:  // vmoveto
                t.MoveTo(t.x, t.y + arg(0));
                t.stack.clear();
                break;
            case 5:  // rlineto
                t.LineTo(t.x + arg(0), t.y + arg(1));
                t.stack.clear();
                break;
            case 6:  // hlineto
                t.LineTo(t.x + arg(0), t.y);
                t.stack.clear();
                break;
            case 7:  // vlineto
                t.LineTo(t.x, t.y + arg(0));
                t.stack.clear();
                break;
            case 8:  // rrcurveto
                t.RRCurveTo(arg(0), arg(1), arg(2), arg(3), arg(4), arg(5));
                t.stack.clear();
                break;
            case 30:  // vhcurveto: dy1 dx2 dy2 dx3
                t.RRCurveTo(0, arg(0), arg(1), arg(2), arg(3), 0);
                t.stack.clear();
                break;
            case 31:  // hvcurveto: dx1 dx2 dy2 dy3
                t.RRCurveTo(arg(0), 0, arg(1), arg(2), 0, arg(3));
                t.stack.clear();
                break;
            case 10: {  // callsubr
                if (t.stack.empty()) break;
                int idx = static_cast<int>(t.stack.back());
                t.stack.pop_back();
                if (idx >= 0 && static_cast<size_t>(idx) < t.info->subrs.size()) RunCharstring(t.info->subrs[static_cast<size_t>(idx)], t);
                break;
            }
            case 11:  // return
                --t.depth;
                return;
            case 14:  // endchar
                t.done = true;
                break;
            case 12: {
                if (i >= len) break;
                unsigned char v2 = p[i++];
                switch (v2) {
                    case 0:  // dotsection
                    case 1:  // vstem3
                    case 2:  // hstem3
                        t.stack.clear();
                        break;
                    case 6:  // seac: asb adx ady bchar achar
                        t.has_seac = true;
                        t.seac_asb = arg(0);
                        t.seac_adx = arg(1);
                        t.seac_ady = arg(2);
                        t.seac_bchar = static_cast<int>(arg(3));
                        t.seac_achar = static_cast<int>(arg(4));
                        t.stack.clear();
                        t.done = true;
                        break;
                    case 7:  // sbw: sbx sby wx wy
                        t.sbx = arg(0);
                        t.x = arg(0);
                        t.y = arg(1);
                        t.advance = arg(2);
                        t.stack.clear();
                        break;
                    case 12: {  // div
                        if (t.stack.size() >= 2) {
                            double b = t.stack.back();
                            t.stack.pop_back();
                            double a = t.stack.back();
                            t.stack.pop_back();
                            t.stack.push_back(b != 0 ? a / b : 0);
                        }
                        break;
                    }
                    case 16:  // callothersubr
                        t.CallOtherSubr();
                        break;
                    case 17:  // pop
                        if (!t.ps_stack.empty()) {
                            t.stack.push_back(t.ps_stack.back());
                            t.ps_stack.pop_back();
                        } else {
                            t.stack.push_back(0);
                        }
                        break;
                    case 33:  // setcurrentpoint
                        t.x = arg(0);
                        t.y = arg(1);
                        t.stack.clear();
                        break;
                    default:
                        t.stack.clear();
                        break;
                }
                break;
            }
            default:
                t.stack.clear();
                break;
        }
    }
    --t.depth;
}

void BuildGlyphContours(const FontInfo *info, int gid, std::vector<OutlineContour> *out, double dx, double dy,
                        double *advance, int depth) {
    if (depth > 4 || gid < 0 || gid >= info->num_glyphs) return;
    T1Interp t;
    t.info = info;
    RunCharstring(info->charstrings[static_cast<size_t>(gid)], t);
    if (advance) *advance = t.advance;
    for (OutlineContour &c : t.contours) {
        if (c.segments.empty()) continue;
        if (dx != 0 || dy != 0) {
            c.start_x += static_cast<float>(dx);
            c.start_y += static_cast<float>(dy);
            for (OutlineSegment &e : c.segments) {
                e.x += static_cast<float>(dx);
                e.y += static_cast<float>(dy);
                e.c1x += static_cast<float>(dx);
                e.c1y += static_cast<float>(dy);
                e.c2x += static_cast<float>(dx);
                e.c2y += static_cast<float>(dy);
            }
        }
        out->push_back(std::move(c));
    }
    if (t.has_seac) {
        const char *bname = StandardEncodingName(t.seac_bchar);
        const char *aname = StandardEncodingName(t.seac_achar);
        int base = bname ? GidForName(info, bname) : -1;
        int accent = aname ? GidForName(info, aname) : -1;
        if (base >= 0) BuildGlyphContours(info, base, out, dx, dy, nullptr, depth + 1);
        // The accent's sidebearing point lands at (sbx + adx, ady) relative
        // to the composite's origin; its own hsbw already moved it to asb.
        if (accent >= 0) BuildGlyphContours(info, accent, out, dx + t.sbx + t.seac_adx - t.seac_asb, dy + t.seac_ady, nullptr, depth + 1);
    }
}

}  // namespace

const char *StandardEncodingName(int code) {
    if (code < 0 || code > 255) return nullptr;
    int idx = kStandardCodeToIndex[code];
    return idx == 0 ? nullptr : kStandardNames[idx - 1];
}

int GidForName(const FontInfo *info, const std::string &name) {
    for (size_t i = 0; i < info->glyph_names.size(); ++i) {
        if (info->glyph_names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

bool InitFont(FontInfo *info, const unsigned char *data, int data_size) {
    *info = FontInfo();
    std::fill(std::begin(info->builtin_encoding), std::end(info->builtin_encoding), -1);
    if (!data || data_size < 16) return false;

    // PFB container: strip the 6-byte segment headers, concatenating segments.
    std::string flat;
    if (data[0] == 0x80) {
        size_t pos = 0;
        while (pos + 6 <= static_cast<size_t>(data_size) && data[pos] == 0x80) {
            uint8_t type = data[pos + 1];
            if (type == 3) break;
            uint32_t seg_len = static_cast<uint32_t>(data[pos + 2]) | (static_cast<uint32_t>(data[pos + 3]) << 8) |
                               (static_cast<uint32_t>(data[pos + 4]) << 16) | (static_cast<uint32_t>(data[pos + 5]) << 24);
            pos += 6;
            if (pos + seg_len > static_cast<size_t>(data_size)) seg_len = static_cast<uint32_t>(data_size) - static_cast<uint32_t>(pos);
            flat.append(reinterpret_cast<const char *>(data + pos), seg_len);
            pos += seg_len;
        }
    } else {
        flat.assign(reinterpret_cast<const char *>(data), static_cast<size_t>(data_size));
    }

    size_t eexec = flat.find("eexec");
    if (eexec == std::string::npos) return false;
    std::string clear = flat.substr(0, eexec);
    size_t pos = eexec + 5;
    while (pos < flat.size() && IsWs(static_cast<unsigned char>(flat[pos]))) ++pos;
    if (pos >= flat.size()) return false;

    // Hex-encoded eexec section (PFA on disk) vs. raw binary (PFB, PDF).
    bool hex = pos + 4 <= flat.size();
    for (size_t k = 0; hex && k < 4; ++k) hex = IsHex(static_cast<unsigned char>(flat[pos + k]));
    std::string cipher;
    if (hex) {
        int hi = -1;
        for (size_t k = pos; k < flat.size(); ++k) {
            unsigned char c = static_cast<unsigned char>(flat[k]);
            if (IsWs(c)) continue;
            if (!IsHex(c)) break;
            if (hi < 0) {
                hi = HexVal(c);
            } else {
                cipher.push_back(static_cast<char>((hi << 4) | HexVal(c)));
                hi = -1;
            }
        }
    } else {
        cipher = flat.substr(pos);
    }
    std::string priv = Decrypt(reinterpret_cast<const unsigned char *>(cipher.data()), cipher.size(), 55665, 4);

    ParseFontMatrix(clear, info);
    std::vector<std::string> code_names(256);
    ParseEncoding(clear, info, &code_names);
    ParsePrivate(priv, info);
    if (info->num_glyphs == 0) return false;
    for (int c = 0; c < 256; ++c) {
        if (!code_names[static_cast<size_t>(c)].empty()) info->builtin_encoding[c] = GidForName(info, code_names[static_cast<size_t>(c)]);
    }
    return true;
}

double GetGlyphAdvance(const FontInfo *info, int gid) {
    if (gid < 0 || gid >= info->num_glyphs) return 0;
    std::vector<OutlineContour> scratch;
    double advance = 0;
    BuildGlyphContours(info, gid, &scratch, 0, 0, &advance, 0);
    return advance;
}

unsigned char *GetGlyphBitmapMatrix(const FontInfo *info, float a, float b, float c, float d, int gid, int *width,
                                    int *height, int *xoff, int *yoff) {
    *width = *height = *xoff = *yoff = 0;
    if (gid < 0 || gid >= info->num_glyphs) return nullptr;
    std::vector<OutlineContour> contours;
    BuildGlyphContours(info, gid, &contours, 0, 0, nullptr, 0);
    return raster::RasterizeOutline(contours, a, b, c, d, width, height, xoff, yoff);
}

unsigned char *GetGlyphBitmap(const FontInfo *info, float scale_x, float scale_y, int gid, int *width, int *height,
                               int *xoff, int *yoff) {
    return GetGlyphBitmapMatrix(info, scale_x, 0, 0, -scale_y, gid, width, height, xoff, yoff);
}

void FreeBitmap(unsigned char *bitmap) { std::free(bitmap); }

}  // namespace t1
}  // namespace gfx
