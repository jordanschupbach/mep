#include "svg_doc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>

namespace {

constexpr float kPi = 3.14159265358979323846f;

// A 2D affine transform in SVG's own [a c e; b d f] column layout.
struct Matrix {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    // this * other: `other` is applied first, then this -- the composition
    // order SVG uses for both nested elements and a transform list.
    Matrix Mul(const Matrix &o) const {
        return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d,
                a * o.e + c * o.f + e, b * o.e + d * o.f + f};
    }
    void Apply(float x, float y, float &ox, float &oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
    // Uniform length scale: geometric mean of the axis scales, used for
    // stroke widths and font sizes under non-uniform/rotated transforms.
    float LengthScale() const { return std::sqrt(std::fabs(a * d - b * c)); }
};

std::string Trim(const std::string &s) {
    size_t begin = 0, end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string Lower(std::string s) {
    for (char &ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// Reads every number out of a whitespace/comma separated list; also used
// for `points`, `viewBox`, and transform function arguments.
std::vector<float> ParseNumberList(const std::string &text) {
    std::vector<float> out;
    const char *p = text.c_str();
    while (*p) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p;
        if (!*p) break;
        char *end = nullptr;
        float value = std::strtof(p, &end);
        if (end == p) { ++p; continue; }
        out.push_back(value);
        p = end;
    }
    return out;
}

bool ParseHexDigit(char ch, unsigned &out) {
    if (ch >= '0' && ch <= '9') { out = static_cast<unsigned>(ch - '0'); return true; }
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ch >= 'a' && ch <= 'f') { out = static_cast<unsigned>(ch - 'a') + 10U; return true; }
    return false;
}

const std::unordered_map<std::string, unsigned> &NamedColors() {
    static const std::unordered_map<std::string, unsigned> kNamed = {
        {"black", 0x000000},   {"white", 0xffffff},   {"red", 0xff0000},     {"green", 0x008000},
        {"blue", 0x0000ff},    {"yellow", 0xffff00},  {"orange", 0xffa500},  {"purple", 0x800080},
        {"gray", 0x808080},    {"grey", 0x808080},    {"silver", 0xc0c0c0},  {"maroon", 0x800000},
        {"olive", 0x808000},   {"navy", 0x000080},    {"teal", 0x008080},    {"aqua", 0x00ffff},
        {"cyan", 0x00ffff},    {"fuchsia", 0xff00ff}, {"magenta", 0xff00ff}, {"lime", 0x00ff00},
        {"pink", 0xffc0cb},    {"brown", 0xa52a2a},   {"gold", 0xffd700},    {"indigo", 0x4b0082},
        {"violet", 0xee82ee},  {"tomato", 0xff6347},  {"crimson", 0xdc143c}, {"coral", 0xff7f50},
        {"salmon", 0xfa8072},  {"khaki", 0xf0e68c},   {"beige", 0xf5f5dc},   {"ivory", 0xfffff0},
        {"tan", 0xd2b48c},     {"chocolate", 0xd2691e}, {"orchid", 0xda70d6}, {"plum", 0xdda0dd},
        {"turquoise", 0x40e0d0}, {"skyblue", 0x87ceeb}, {"steelblue", 0x4682b4}, {"royalblue", 0x4169e1},
        {"dodgerblue", 0x1e90ff}, {"darkblue", 0x00008b}, {"darkgreen", 0x006400}, {"darkred", 0x8b0000},
        {"darkgray", 0xa9a9a9}, {"darkgrey", 0xa9a9a9}, {"lightgray", 0xd3d3d3}, {"lightgrey", 0xd3d3d3},
        {"lightblue", 0xadd8e6}, {"lightgreen", 0x90ee90}, {"seagreen", 0x2e8b57}, {"forestgreen", 0x228b22},
        {"slategray", 0x708090}, {"slategrey", 0x708090}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
        {"whitesmoke", 0xf5f5f5}, {"gainsboro", 0xdcdcdc}, {"firebrick", 0xb22222}, {"goldenrod", 0xdaa520},
        {"rebeccapurple", 0x663399}, {"hotpink", 0xff69b4}, {"deeppink", 0xff1493}, {"lavender", 0xe6e6fa},
        {"mintcream", 0xf5fffa}, {"midnightblue", 0x191970}, {"cornflowerblue", 0x6495ed}, {"limegreen", 0x32cd32},
        {"yellowgreen", 0x9acd32}, {"greenyellow", 0xadff2f}, {"orangered", 0xff4500}, {"darkorange", 0xff8c00},
    };
    return kNamed;
}

// Parses a CSS color into `out`. Returns false for unrecognised text (the
// caller then leaves the inherited paint alone). "none"/"transparent" are
// recognised and yield an absent/fully transparent paint respectively.
bool ParseColor(const std::string &raw, SvgPaint current_color, SvgPaint &out) {
    std::string text = Lower(Trim(raw));
    if (text.empty()) return false;
    if (text == "none") { out = SvgPaint{}; return true; }
    if (text == "transparent") { out = SvgPaint{true, 0, 0, 0, 0}; return true; }
    if (text == "currentcolor") { out = current_color; return true; }
    if (text[0] == '#') {
        std::string hex = text.substr(1);
        unsigned digits[8] = {};
        if (hex.size() != 3 && hex.size() != 4 && hex.size() != 6 && hex.size() != 8) return false;
        for (size_t i = 0; i < hex.size(); ++i) if (!ParseHexDigit(hex[i], digits[i])) return false;
        out.present = true;
        if (hex.size() <= 4) {
            out.r = static_cast<unsigned char>(digits[0] * 17U); out.g = static_cast<unsigned char>(digits[1] * 17U);
            out.b = static_cast<unsigned char>(digits[2] * 17U); out.a = static_cast<unsigned char>(hex.size() == 4 ? digits[3] * 17U : 255U);
        } else {
            out.r = static_cast<unsigned char>(digits[0] * 16U + digits[1]); out.g = static_cast<unsigned char>(digits[2] * 16U + digits[3]);
            out.b = static_cast<unsigned char>(digits[4] * 16U + digits[5]); out.a = static_cast<unsigned char>(hex.size() == 8 ? digits[6] * 16U + digits[7] : 255U);
        }
        return true;
    }
    if (text.compare(0, 4, "rgb(") == 0 || text.compare(0, 5, "rgba(") == 0) {
        size_t open = text.find('('), close = text.find(')');
        if (open == std::string::npos || close == std::string::npos || close < open) return false;
        std::string inner = text.substr(open + 1, close - open - 1);
        std::replace(inner.begin(), inner.end(), '/', ' ');
        bool percent_alpha = false;
        // A trailing "%" on the alpha channel is the only percentage form
        // handled here; rgb(255, 0, 0) and rgb(255 0 0 / 0.5) are the
        // overwhelmingly common spellings in real inline SVG.
        if (!inner.empty() && inner.back() == '%') percent_alpha = true;
        std::vector<float> channels = ParseNumberList(inner);
        if (channels.size() < 3) return false;
        auto clamp_byte = [](float v) { return static_cast<unsigned char>(std::max(0.0f, std::min(255.0f, v))); };
        out.present = true;
        out.r = clamp_byte(channels[0]); out.g = clamp_byte(channels[1]); out.b = clamp_byte(channels[2]);
        float alpha = channels.size() >= 4 ? (percent_alpha ? channels[3] / 100.0f : channels[3]) : 1.0f;
        out.a = clamp_byte(alpha * 255.0f);
        return true;
    }
    auto it = NamedColors().find(text);
    if (it == NamedColors().end()) return false;
    out = SvgPaint{true, static_cast<unsigned char>(it->second >> 16), static_cast<unsigned char>((it->second >> 8) & 0xffU),
                   static_cast<unsigned char>(it->second & 0xffU), 255};
    return true;
}

// The inherited presentation state every SVG element contributes to.
struct SvgStyle {
    SvgPaint fill{true, 0, 0, 0, 255};  // SVG's initial fill is black
    SvgPaint stroke{};                  // and its initial stroke is none
    float stroke_width = 1.0f;
    float opacity = 1.0f;               // accumulated group opacity
    float fill_opacity = 1.0f, stroke_opacity = 1.0f;
    float font_size = 16.0f;
    std::string text_anchor = "start";
    bool hidden = false;                // display:none / visibility:hidden
    SvgPaint color{true, 0, 0, 0, 255};  // CSS `color`, the target of currentColor
};

// Presentation properties can come from attributes or from style="";
// style="" wins, matching the CSS cascade's treatment of inline style.
std::unordered_map<std::string, std::string> PresentationProps(const DomNode &node) {
    std::unordered_map<std::string, std::string> props;
    static const char *const kKeys[] = {"fill", "stroke", "stroke-width", "opacity", "fill-opacity", "stroke-opacity",
                                        "font-size", "text-anchor", "display", "visibility", "color", "transform"};
    for (const char *key : kKeys) {
        auto it = node.attrs.find(key);
        if (it != node.attrs.end()) props[key] = it->second;
    }
    auto style_it = node.attrs.find("style");
    if (style_it != node.attrs.end()) {
        std::istringstream decls(style_it->second);
        std::string decl;
        while (std::getline(decls, decl, ';')) {
            size_t colon = decl.find(':');
            if (colon == std::string::npos) continue;
            props[Lower(Trim(decl.substr(0, colon)))] = Trim(decl.substr(colon + 1));
        }
    }
    return props;
}

float ParseLength(const std::string &text, float fallback) {
    char *end = nullptr;
    float value = std::strtof(text.c_str(), &end);
    return end == text.c_str() ? fallback : value;
}

float Attr(const DomNode &node, const char *key, float fallback) {
    auto it = node.attrs.find(key);
    return it == node.attrs.end() ? fallback : ParseLength(it->second, fallback);
}

Matrix ParseTransform(const std::string &text) {
    Matrix result;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t open = text.find('(', pos);
        if (open == std::string::npos) break;
        size_t close = text.find(')', open);
        if (close == std::string::npos) break;
        std::string name = Lower(Trim(text.substr(pos, open - pos)));
        std::vector<float> args = ParseNumberList(text.substr(open + 1, close - open - 1));
        pos = close + 1;
        Matrix m;
        if (name == "translate" && !args.empty()) {
            m.e = args[0]; m.f = args.size() > 1 ? args[1] : 0.0f;
        } else if (name == "scale" && !args.empty()) {
            m.a = args[0]; m.d = args.size() > 1 ? args[1] : args[0];
        } else if (name == "rotate" && !args.empty()) {
            float rad = args[0] * kPi / 180.0f;
            Matrix rotation{std::cos(rad), std::sin(rad), -std::sin(rad), std::cos(rad), 0, 0};
            if (args.size() >= 3) {
                Matrix to{1, 0, 0, 1, args[1], args[2]}, back{1, 0, 0, 1, -args[1], -args[2]};
                m = to.Mul(rotation).Mul(back);
            } else m = rotation;
        } else if (name == "skewx" && !args.empty()) {
            m.c = std::tan(args[0] * kPi / 180.0f);
        } else if (name == "skewy" && !args.empty()) {
            m.b = std::tan(args[0] * kPi / 180.0f);
        } else if (name == "matrix" && args.size() >= 6) {
            m = {args[0], args[1], args[2], args[3], args[4], args[5]};
        } else continue;
        result = result.Mul(m);
    }
    return result;
}

void AppendCubic(std::vector<float> &pts, float x0, float y0, float cx1, float cy1, float cx2, float cy2, float x1, float y1) {
    constexpr int kSegments = 16;
    for (int i = 1; i <= kSegments; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(kSegments), u = 1.0f - t;
        pts.push_back(u * u * u * x0 + 3.0f * u * u * t * cx1 + 3.0f * u * t * t * cx2 + t * t * t * x1);
        pts.push_back(u * u * u * y0 + 3.0f * u * u * t * cy1 + 3.0f * u * t * t * cy2 + t * t * t * y1);
    }
}

void AppendQuadratic(std::vector<float> &pts, float x0, float y0, float cx, float cy, float x1, float y1) {
    constexpr int kSegments = 12;
    for (int i = 1; i <= kSegments; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(kSegments), u = 1.0f - t;
        pts.push_back(u * u * x0 + 2.0f * u * t * cx + t * t * x1);
        pts.push_back(u * u * y0 + 2.0f * u * t * cy + t * t * y1);
    }
}

// SVG elliptical arc, converted from endpoint to center parameterization
// per the SVG implementation notes (F.6.5), then flattened.
void AppendArc(std::vector<float> &pts, float x0, float y0, float rx, float ry, float rotation_deg, bool large_arc, bool sweep, float x1, float y1) {
    if (x0 == x1 && y0 == y1) return;
    rx = std::fabs(rx); ry = std::fabs(ry);
    if (rx == 0.0f || ry == 0.0f) { pts.push_back(x1); pts.push_back(y1); return; }
    float phi = rotation_deg * kPi / 180.0f, cos_phi = std::cos(phi), sin_phi = std::sin(phi);
    float dx = (x0 - x1) / 2.0f, dy = (y0 - y1) / 2.0f;
    float x1p = cos_phi * dx + sin_phi * dy, y1p = -sin_phi * dx + cos_phi * dy;
    float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0f) { float s = std::sqrt(lambda); rx *= s; ry *= s; }
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    float coef = den == 0.0f ? 0.0f : std::sqrt(std::max(0.0f, num / den));
    if (large_arc == sweep) coef = -coef;
    float cxp = coef * (rx * y1p / ry), cyp = coef * (-ry * x1p / rx);
    float cx = cos_phi * cxp - sin_phi * cyp + (x0 + x1) / 2.0f, cy = sin_phi * cxp + cos_phi * cyp + (y0 + y1) / 2.0f;
    auto angle = [](float ux, float uy, float vx, float vy) {
        float dot = ux * vx + uy * vy, len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
        float a = std::acos(std::max(-1.0f, std::min(1.0f, len == 0.0f ? 1.0f : dot / len)));
        return (ux * vy - uy * vx) < 0.0f ? -a : a;
    };
    float theta1 = angle(1.0f, 0.0f, (x1p - cxp) / rx, (y1p - cyp) / ry);
    float delta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!sweep && delta > 0.0f) delta -= 2.0f * kPi;
    else if (sweep && delta < 0.0f) delta += 2.0f * kPi;
    int segments = std::max(4, static_cast<int>(std::ceil(std::fabs(delta) / (kPi / 16.0f))));
    for (int i = 1; i <= segments; ++i) {
        float t = theta1 + delta * static_cast<float>(i) / static_cast<float>(segments);
        float ex = rx * std::cos(t), ey = ry * std::sin(t);
        pts.push_back(cos_phi * ex - sin_phi * ey + cx);
        pts.push_back(sin_phi * ex + cos_phi * ey + cy);
    }
    // Land exactly on the endpoint regardless of accumulated rounding.
    pts[pts.size() - 2] = x1; pts.back() = y1;
}

// Splits path data into tokens: single-letter commands and numbers. SVG
// allows "1.5.5" (two numbers) and "3-4" (3 and -4) without separators, and
// flags in arcs are single digits that may be run together ("0 01 5,5").
std::vector<std::string> TokenizePath(const std::string &data) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < data.size()) {
        char ch = data[i];
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',') { ++i; continue; }
        if (std::isalpha(static_cast<unsigned char>(ch))) { tokens.emplace_back(1, ch); ++i; continue; }
        std::string number;
        if (ch == '+' || ch == '-') { number += ch; ++i; }
        bool seen_dot = false, seen_exp = false;
        while (i < data.size()) {
            char c = data[i];
            if (std::isdigit(static_cast<unsigned char>(c))) { number += c; ++i; }
            else if (c == '.' && !seen_dot && !seen_exp) { seen_dot = true; number += c; ++i; }
            else if ((c == 'e' || c == 'E') && !seen_exp && i + 1 < data.size() &&
                     (std::isdigit(static_cast<unsigned char>(data[i + 1])) || data[i + 1] == '-' || data[i + 1] == '+')) {
                seen_exp = true; number += c; ++i;
                if (data[i] == '-' || data[i] == '+') { number += data[i]; ++i; }
            } else break;
        }
        if (number.empty() || number == "+" || number == "-") { ++i; continue; }
        tokens.push_back(number);
    }
    return tokens;
}

}  // namespace

bool ParseCssColor(const std::string &text, unsigned char &r, unsigned char &g, unsigned char &b, unsigned char &a) {
    SvgPaint paint;
    if (!ParseColor(text, SvgPaint{true, 0, 0, 0, 255}, paint) || !paint.present) return false;
    r = paint.r; g = paint.g; b = paint.b; a = paint.a;
    return true;
}

std::vector<SvgSubpath> ParseSvgPathData(const std::string &data) {
    std::vector<SvgSubpath> subpaths;
    std::vector<std::string> tokens = TokenizePath(data);
    size_t i = 0;
    char op = 0;
    float cx = 0, cy = 0, start_x = 0, start_y = 0;
    float last_ctrl_x = 0, last_ctrl_y = 0;  // for S/T reflection
    char last_op = 0;
    auto current = [&]() -> SvgSubpath & {
        if (subpaths.empty() || subpaths.back().closed) {
            subpaths.push_back({});
            subpaths.back().points = {cx, cy};
        }
        return subpaths.back();
    };
    auto number = [&](size_t index, float &out) {
        if (index >= tokens.size() || std::isalpha(static_cast<unsigned char>(tokens[index][0]))) return false;
        out = std::strtof(tokens[index].c_str(), nullptr);
        return true;
    };
    // Arc flags may be glued to each other or to the following number
    // ("1 0 0 1 10 10" vs "1001010"): split a multi-digit token into flags.
    auto flag = [&](size_t &index, bool &out, std::string &carry) -> bool {
        if (!carry.empty()) { out = carry[0] == '1'; carry.erase(0, 1); return true; }
        if (index >= tokens.size() || std::isalpha(static_cast<unsigned char>(tokens[index][0]))) return false;
        const std::string &tok = tokens[index++];
        out = tok[0] == '1';
        if (tok.size() > 1) carry = tok.substr(1);
        return true;
    };
    while (i < tokens.size()) {
        if (std::isalpha(static_cast<unsigned char>(tokens[i][0]))) { op = tokens[i][0]; ++i; }
        else if (op == 0) { ++i; continue; }
        else if (op == 'M') op = 'L';      // implicit lineto after moveto
        else if (op == 'm') op = 'l';
        bool relative = std::islower(static_cast<unsigned char>(op)) != 0;
        char cmd = static_cast<char>(std::toupper(static_cast<unsigned char>(op)));
        float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
        if (cmd == 'Z') {
            if (!subpaths.empty() && !subpaths.back().closed) {
                subpaths.back().closed = true;
                cx = start_x; cy = start_y;
            }
            last_op = 'Z';
            continue;
        }
        if (cmd == 'M') {
            if (!number(i, a) || !number(i + 1, b)) break;
            i += 2;
            if (relative) { a += cx; b += cy; }
            cx = a; cy = b; start_x = cx; start_y = cy;
            subpaths.push_back({});
            subpaths.back().points = {cx, cy};
        } else if (cmd == 'L') {
            if (!number(i, a) || !number(i + 1, b)) break;
            i += 2;
            if (relative) { a += cx; b += cy; }
            SvgSubpath &sp = current(); cx = a; cy = b; sp.points.push_back(cx); sp.points.push_back(cy);
        } else if (cmd == 'H') {
            if (!number(i, a)) break;
            ++i;
            SvgSubpath &sp = current(); cx = relative ? cx + a : a; sp.points.push_back(cx); sp.points.push_back(cy);
        } else if (cmd == 'V') {
            if (!number(i, a)) break;
            ++i;
            SvgSubpath &sp = current(); cy = relative ? cy + a : a; sp.points.push_back(cx); sp.points.push_back(cy);
        } else if (cmd == 'C') {
            if (!number(i, a) || !number(i + 1, b) || !number(i + 2, c) || !number(i + 3, d) || !number(i + 4, e) || !number(i + 5, f)) break;
            i += 6;
            if (relative) { a += cx; b += cy; c += cx; d += cy; e += cx; f += cy; }
            SvgSubpath &sp = current(); AppendCubic(sp.points, cx, cy, a, b, c, d, e, f);
            last_ctrl_x = c; last_ctrl_y = d; cx = e; cy = f;
        } else if (cmd == 'S') {
            if (!number(i, c) || !number(i + 1, d) || !number(i + 2, e) || !number(i + 3, f)) break;
            i += 4;
            if (relative) { c += cx; d += cy; e += cx; f += cy; }
            bool reflect = last_op == 'C' || last_op == 'S';
            a = reflect ? 2.0f * cx - last_ctrl_x : cx; b = reflect ? 2.0f * cy - last_ctrl_y : cy;
            SvgSubpath &sp = current(); AppendCubic(sp.points, cx, cy, a, b, c, d, e, f);
            last_ctrl_x = c; last_ctrl_y = d; cx = e; cy = f;
        } else if (cmd == 'Q') {
            if (!number(i, a) || !number(i + 1, b) || !number(i + 2, c) || !number(i + 3, d)) break;
            i += 4;
            if (relative) { a += cx; b += cy; c += cx; d += cy; }
            SvgSubpath &sp = current(); AppendQuadratic(sp.points, cx, cy, a, b, c, d);
            last_ctrl_x = a; last_ctrl_y = b; cx = c; cy = d;
        } else if (cmd == 'T') {
            if (!number(i, c) || !number(i + 1, d)) break;
            i += 2;
            if (relative) { c += cx; d += cy; }
            bool reflect = last_op == 'Q' || last_op == 'T';
            a = reflect ? 2.0f * cx - last_ctrl_x : cx; b = reflect ? 2.0f * cy - last_ctrl_y : cy;
            SvgSubpath &sp = current(); AppendQuadratic(sp.points, cx, cy, a, b, c, d);
            last_ctrl_x = a; last_ctrl_y = b; cx = c; cy = d;
        } else if (cmd == 'A') {
            bool large = false, sweep = false;
            std::string carry;
            if (!number(i, a) || !number(i + 1, b) || !number(i + 2, c)) break;
            i += 3;
            if (!flag(i, large, carry)) break;
            if (!flag(i, sweep, carry)) break;
            if (!carry.empty()) { e = std::strtof(carry.c_str(), nullptr); if (!number(i, f)) break; ++i; }
            else { if (!number(i, e) || !number(i + 1, f)) break; i += 2; }
            if (relative) { e += cx; f += cy; }
            SvgSubpath &sp = current(); AppendArc(sp.points, cx, cy, a, b, c, large, sweep, e, f);
            cx = e; cy = f;
        } else { ++i; continue; }
        last_op = cmd;
    }
    return subpaths;
}

std::vector<unsigned> TriangulateSvgPolygon(const std::vector<float> &raw) {
    std::vector<unsigned> out;
    // Drop consecutive duplicates and an explicit closing point so the
    // ear test never sees a zero-length edge.
    std::vector<unsigned> idx;
    size_t n = raw.size() / 2;
    for (size_t i = 0; i < n; ++i) {
        if (!idx.empty() && raw[static_cast<size_t>(idx.back()) * 2U] == raw[i * 2] && raw[static_cast<size_t>(idx.back()) * 2U + 1U] == raw[i * 2 + 1]) continue;
        idx.push_back(static_cast<unsigned>(i));
    }
    while (idx.size() > 1 && raw[static_cast<size_t>(idx.front()) * 2U] == raw[static_cast<size_t>(idx.back()) * 2U] &&
           raw[static_cast<size_t>(idx.front()) * 2U + 1U] == raw[static_cast<size_t>(idx.back()) * 2U + 1U]) idx.pop_back();
    if (idx.size() < 3) return out;
    auto px = [&](unsigned i) { return raw[static_cast<size_t>(i) * 2U]; };
    auto py = [&](unsigned i) { return raw[static_cast<size_t>(i) * 2U + 1U]; };
    auto cross = [&](unsigned a, unsigned b, unsigned c) {
        return (px(b) - px(a)) * (py(c) - py(a)) - (py(b) - py(a)) * (px(c) - px(a));
    };
    float area = 0.0f;
    for (size_t i = 0; i < idx.size(); ++i) {
        unsigned a = idx[i], b = idx[(i + 1) % idx.size()];
        area += px(a) * py(b) - px(b) * py(a);
    }
    // Emit with negative cross (screen counter-clockwise, see the header):
    // reverse the working order when the input winds the other way.
    if (area > 0.0f) std::reverse(idx.begin(), idx.end());
    auto inside = [&](unsigned p, unsigned a, unsigned b, unsigned c) {
        return cross(a, b, p) <= 0.0f && cross(b, c, p) <= 0.0f && cross(c, a, p) <= 0.0f;
    };
    std::vector<unsigned> work = idx;
    size_t guard = 0;
    while (work.size() > 3 && guard < work.size() * work.size() + 16) {
        bool clipped = false;
        for (size_t i = 0; i < work.size(); ++i) {
            unsigned a = work[(i + work.size() - 1) % work.size()], b = work[i], c = work[(i + 1) % work.size()];
            if (cross(a, b, c) >= 0.0f) continue;  // reflex or collinear vertex, not an ear
            bool blocked = false;
            for (unsigned p : work) {
                if (p == a || p == b || p == c) continue;
                if (inside(p, a, b, c)) { blocked = true; break; }
            }
            if (blocked) continue;
            out.insert(out.end(), {a, b, c});
            work.erase(work.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        ++guard;
        if (!clipped) break;
    }
    if (work.size() == 3 && cross(work[0], work[1], work[2]) < 0.0f) out.insert(out.end(), {work[0], work[1], work[2]});
    else if (work.size() >= 3) {
        // Self-intersecting or otherwise unclippable: fan from the first
        // vertex so the shape is at least visible, orienting each triangle.
        for (size_t i = 2; i < work.size(); ++i) {
            unsigned a = work[0], b = work[i - 1], c = work[i];
            if (cross(a, b, c) > 0.0f) std::swap(b, c);
            out.insert(out.end(), {a, b, c});
        }
    }
    return out;
}

bool SvgIntrinsicSize(const DomNode &svg, float &width, float &height) {
    float w = Attr(svg, "width", 0.0f), h = Attr(svg, "height", 0.0f);
    auto view_box_it = svg.attrs.find("viewbox");
    std::vector<float> view_box = view_box_it == svg.attrs.end() ? std::vector<float>{} : ParseNumberList(view_box_it->second);
    bool has_view_box = view_box.size() == 4 && view_box[2] > 0.0f && view_box[3] > 0.0f;
    if (w > 0.0f && h > 0.0f) { width = w; height = h; return true; }
    if (has_view_box) {
        if (w > 0.0f) { width = w; height = w * view_box[3] / view_box[2]; return true; }
        if (h > 0.0f) { height = h; width = h * view_box[2] / view_box[3]; return true; }
        width = view_box[2]; height = view_box[3]; return true;
    }
    return false;
}

namespace {

struct Builder {
    const DomNode &root;
    SvgDisplayList out;

    const DomNode *FindById(const DomNode *node, const std::string &id) const {
        if (node->type == DomNodeType::Element && node->Id() == id) return node;
        for (const auto &child : node->children) if (const DomNode *found = FindById(child.get(), id)) return found;
        return nullptr;
    }

    // Flat approximation of a gradient: the average of its stops. Real
    // gradient painting is listed as outstanding in svg_doc.h.
    bool ResolveGradient(const std::string &url, SvgPaint current_color, SvgPaint &resolved) const {
        size_t hash = url.find('#'), close = url.find(')');
        if (hash == std::string::npos) return false;
        std::string id = url.substr(hash + 1, close == std::string::npos ? std::string::npos : close - hash - 1);
        const DomNode *gradient = FindById(&root, Trim(id));
        if (!gradient) return false;
        unsigned r = 0, g = 0, b = 0, a = 0, count = 0;
        for (const auto &stop : gradient->children) {
            if (stop->type != DomNodeType::Element || stop->tag != "stop") continue;
            auto props = PresentationProps(*stop);
            SvgPaint paint;
            std::string color = props.count("stop-color") ? props["stop-color"] : std::string();
            if (color.empty()) { auto it = stop->attrs.find("stop-color"); if (it != stop->attrs.end()) color = it->second; }
            if (!ParseColor(color, current_color, paint) || !paint.present) continue;
            float stop_opacity = 1.0f;
            auto so = stop->attrs.find("stop-opacity");
            if (so != stop->attrs.end()) stop_opacity = ParseLength(so->second, 1.0f);
            r += paint.r; g += paint.g; b += paint.b; a += static_cast<unsigned>(static_cast<float>(paint.a) * stop_opacity); ++count;
        }
        if (!count) return false;
        resolved = SvgPaint{true, static_cast<unsigned char>(r / count), static_cast<unsigned char>(g / count), static_cast<unsigned char>(b / count), static_cast<unsigned char>(a / count)};
        return true;
    }

    void ApplyPaint(const std::string &value, SvgPaint current_color, SvgPaint &paint) const {
        if (Lower(Trim(value)).compare(0, 4, "url(") == 0) { SvgPaint resolved; if (ResolveGradient(value, current_color, resolved)) paint = resolved; return; }
        SvgPaint parsed;
        if (ParseColor(value, current_color, parsed)) paint = parsed;
    }

    SvgStyle Inherit(const DomNode &node, const SvgStyle &parent, Matrix &matrix) const {
        SvgStyle style = parent;
        auto props = PresentationProps(node);
        if (props.count("color")) { SvgPaint c; if (ParseColor(props["color"], style.color, c) && c.present) style.color = c; }
        if (props.count("fill")) ApplyPaint(props["fill"], style.color, style.fill);
        if (props.count("stroke")) ApplyPaint(props["stroke"], style.color, style.stroke);
        if (props.count("stroke-width")) style.stroke_width = ParseLength(props["stroke-width"], style.stroke_width);
        if (props.count("opacity")) style.opacity *= std::max(0.0f, std::min(1.0f, ParseLength(props["opacity"], 1.0f)));
        if (props.count("fill-opacity")) style.fill_opacity = std::max(0.0f, std::min(1.0f, ParseLength(props["fill-opacity"], 1.0f)));
        if (props.count("stroke-opacity")) style.stroke_opacity = std::max(0.0f, std::min(1.0f, ParseLength(props["stroke-opacity"], 1.0f)));
        if (props.count("font-size")) style.font_size = ParseLength(props["font-size"], style.font_size);
        if (props.count("text-anchor")) style.text_anchor = Lower(Trim(props["text-anchor"]));
        if (props.count("display") && Lower(Trim(props["display"])) == "none") style.hidden = true;
        if (props.count("visibility") && Lower(Trim(props["visibility"])) == "hidden") style.hidden = true;
        if (props.count("transform")) matrix = matrix.Mul(ParseTransform(props["transform"]));
        return style;
    }

    SvgPaint Modulate(SvgPaint paint, float opacity) const {
        if (!paint.present) return paint;
        paint.a = static_cast<unsigned char>(std::max(0.0f, std::min(255.0f, static_cast<float>(paint.a) * opacity)));
        return paint;
    }

    void Emit(const std::vector<float> &local, bool closed, const SvgStyle &style, const Matrix &m) {
        if (local.size() < 4) return;
        std::vector<float> pts;
        pts.reserve(local.size());
        for (size_t i = 0; i + 1 < local.size(); i += 2) { float x, y; m.Apply(local[i], local[i + 1], x, y); pts.push_back(x); pts.push_back(y); }
        SvgPaint fill = Modulate(style.fill, style.opacity * style.fill_opacity);
        SvgPaint stroke = Modulate(style.stroke, style.opacity * style.stroke_opacity);
        if (fill.present && fill.a > 0 && pts.size() >= 6) {
            SvgShape shape;
            shape.kind = SvgShape::Kind::Polygon;
            shape.points = pts;
            shape.closed = true;
            shape.triangles = TriangulateSvgPolygon(pts);
            shape.fill = fill;
            // Stroke rides along on the same shape when both are painted.
            if (stroke.present && stroke.a > 0) { shape.stroke = stroke; shape.stroke_width = style.stroke_width * m.LengthScale(); }
            out.shapes.push_back(std::move(shape));
        } else if (stroke.present && stroke.a > 0) {
            SvgShape shape;
            shape.kind = SvgShape::Kind::Polyline;
            shape.points = pts;
            shape.closed = closed;
            shape.stroke = stroke;
            shape.stroke_width = style.stroke_width * m.LengthScale();
            out.shapes.push_back(std::move(shape));
        }
    }

    void EmitStrokeOnly(const std::vector<float> &local, const SvgStyle &style, const Matrix &m) {
        SvgStyle open = style;
        open.fill = SvgPaint{};
        Emit(local, false, open, m);
    }

    void CollectText(const DomNode &node, std::string &text) const {
        for (const auto &child : node.children) {
            if (child->type == DomNodeType::Text) text += child->text;
            else if (child->tag == "tspan" || child->tag == "a") CollectText(*child, text);
        }
    }

    void Draw(const DomNode &node, const SvgStyle &parent, const Matrix &parent_matrix, int depth) {
        if (node.type != DomNodeType::Element || depth > 64) return;
        const std::string &tag = node.tag;
        // Non-rendered containers: definitions only reachable through <use>
        // or a paint reference.
        if (tag == "defs" || tag == "symbol" || tag == "title" || tag == "desc" || tag == "metadata" || tag == "style" ||
            tag == "clippath" || tag == "mask" || tag == "marker" || tag == "pattern" || tag == "lineargradient" ||
            tag == "radialgradient" || tag == "filter" || tag == "script")
            return;
        Matrix m = parent_matrix;
        SvgStyle style = Inherit(node, parent, m);
        if (style.hidden) return;
        if (tag == "g" || tag == "a" || tag == "switch") {
            for (const auto &child : node.children) Draw(*child, style, m, depth + 1);
        } else if (tag == "svg") {
            // A nested <svg> positions its content at x/y; its own viewBox is
            // not re-fit here (rare in inline icon markup).
            Matrix inner = m.Mul(Matrix{1, 0, 0, 1, Attr(node, "x", 0.0f), Attr(node, "y", 0.0f)});
            for (const auto &child : node.children) Draw(*child, style, inner, depth + 1);
        } else if (tag == "use") {
            std::string href;
            for (const char *key : {"href", "xlink:href"}) { auto it = node.attrs.find(key); if (it != node.attrs.end()) { href = it->second; break; } }
            if (href.empty() || href[0] != '#') return;
            const DomNode *target = FindById(&root, href.substr(1));
            if (!target || target == &node) return;
            Matrix placed = m.Mul(Matrix{1, 0, 0, 1, Attr(node, "x", 0.0f), Attr(node, "y", 0.0f)});
            if (target->tag == "symbol") {
                Matrix symbol_matrix = placed;
                SvgStyle symbol_style = Inherit(*target, style, symbol_matrix);
                for (const auto &child : target->children) Draw(*child, symbol_style, symbol_matrix, depth + 1);
            } else Draw(*target, style, placed, depth + 1);
        } else if (tag == "rect") {
            float x = Attr(node, "x", 0.0f), y = Attr(node, "y", 0.0f), w = Attr(node, "width", 0.0f), h = Attr(node, "height", 0.0f);
            if (w <= 0.0f || h <= 0.0f) return;
            float rx = Attr(node, "rx", -1.0f), ry = Attr(node, "ry", -1.0f);
            if (rx < 0.0f && ry < 0.0f) rx = ry = 0.0f;
            else if (rx < 0.0f) rx = ry; else if (ry < 0.0f) ry = rx;
            rx = std::min(rx, w / 2.0f); ry = std::min(ry, h / 2.0f);
            std::vector<float> pts;
            if (rx > 0.0f && ry > 0.0f) {
                pts = {x + rx, y};
                pts.push_back(x + w - rx); pts.push_back(y);
                AppendArc(pts, x + w - rx, y, rx, ry, 0, false, true, x + w, y + ry);
                pts.push_back(x + w); pts.push_back(y + h - ry);
                AppendArc(pts, x + w, y + h - ry, rx, ry, 0, false, true, x + w - rx, y + h);
                pts.push_back(x + rx); pts.push_back(y + h);
                AppendArc(pts, x + rx, y + h, rx, ry, 0, false, true, x, y + h - ry);
                pts.push_back(x); pts.push_back(y + ry);
                AppendArc(pts, x, y + ry, rx, ry, 0, false, true, x + rx, y);
            } else pts = {x, y, x + w, y, x + w, y + h, x, y + h};
            Emit(pts, true, style, m);
        } else if (tag == "circle" || tag == "ellipse") {
            float cx = Attr(node, "cx", 0.0f), cy = Attr(node, "cy", 0.0f);
            float rx = tag == "circle" ? Attr(node, "r", 0.0f) : Attr(node, "rx", 0.0f);
            float ry = tag == "circle" ? rx : Attr(node, "ry", 0.0f);
            if (rx <= 0.0f || ry <= 0.0f) return;
            // Segment count follows the on-screen radius so large circles
            // stay round and icon-sized ones stay cheap.
            float screen_r = std::max(rx, ry) * m.LengthScale();
            int segments = std::max(12, std::min(96, static_cast<int>(screen_r)));
            std::vector<float> pts;
            for (int i = 0; i < segments; ++i) {
                float t = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(segments);
                pts.push_back(cx + rx * std::cos(t)); pts.push_back(cy + ry * std::sin(t));
            }
            Emit(pts, true, style, m);
        } else if (tag == "line") {
            std::vector<float> pts = {Attr(node, "x1", 0.0f), Attr(node, "y1", 0.0f), Attr(node, "x2", 0.0f), Attr(node, "y2", 0.0f)};
            EmitStrokeOnly(pts, style, m);
        } else if (tag == "polyline" || tag == "polygon") {
            auto it = node.attrs.find("points");
            if (it == node.attrs.end()) return;
            std::vector<float> pts = ParseNumberList(it->second);
            if (pts.size() % 2) pts.pop_back();
            if (tag == "polygon") Emit(pts, true, style, m);
            else {
                // An open polyline can still carry a fill in SVG (the fill
                // treats it as implicitly closed); keep that behavior.
                if (style.fill.present) Emit(pts, false, style, m);
                else EmitStrokeOnly(pts, style, m);
            }
        } else if (tag == "path") {
            auto it = node.attrs.find("d");
            if (it == node.attrs.end()) return;
            for (const SvgSubpath &sub : ParseSvgPathData(it->second)) {
                if (sub.closed || style.fill.present) Emit(sub.points, sub.closed, style, m);
                else EmitStrokeOnly(sub.points, style, m);
            }
        } else if (tag == "text") {
            std::string text;
            CollectText(node, text);
            text = Trim(text);
            if (text.empty()) return;
            SvgShape shape;
            shape.kind = SvgShape::Kind::Text;
            float x, y;
            m.Apply(Attr(node, "x", 0.0f), Attr(node, "y", 0.0f), x, y);
            shape.points = {x, y};
            shape.text = text;
            shape.font_size = style.font_size * m.LengthScale();
            shape.text_anchor = style.text_anchor;
            shape.fill = Modulate(style.fill, style.opacity * style.fill_opacity);
            if (!shape.fill.present) shape.fill = Modulate(style.stroke, style.opacity * style.stroke_opacity);
            if (!shape.fill.present || shape.fill.a == 0) return;
            out.shapes.push_back(std::move(shape));
        }
        // Unknown elements (foreignObject, image, ...) are skipped silently.
    }
};

}  // namespace

SvgDisplayList BuildSvgDisplayList(const DomNode &svg, float target_width, float target_height, SvgPaint current_color) {
    Builder builder{svg, {}};
    builder.out.width = target_width;
    builder.out.height = target_height;
    if (target_width <= 0.0f || target_height <= 0.0f) return builder.out;

    // The root establishes the user-space -> target-pixel mapping: a
    // viewBox is fit with preserveAspectRatio (xMidYMid meet by default);
    // without one, user units map onto the intrinsic width/height.
    Matrix root;
    auto view_box_it = svg.attrs.find("viewbox");
    std::vector<float> view_box = view_box_it == svg.attrs.end() ? std::vector<float>{} : ParseNumberList(view_box_it->second);
    if (view_box.size() == 4 && view_box[2] > 0.0f && view_box[3] > 0.0f) {
        float sx = target_width / view_box[2], sy = target_height / view_box[3];
        std::string par;
        if (auto it = svg.attrs.find("preserveaspectratio"); it != svg.attrs.end()) par = Lower(it->second);
        float tx = 0.0f, ty = 0.0f;
        if (par != "none") {
            bool slice = par.find("slice") != std::string::npos;
            float s = slice ? std::max(sx, sy) : std::min(sx, sy);
            sx = sy = s;
            float extra_x = target_width - view_box[2] * s, extra_y = target_height - view_box[3] * s;
            float ax = 0.5f, ay = 0.5f;
            if (par.find("xmin") != std::string::npos) ax = 0.0f; else if (par.find("xmax") != std::string::npos) ax = 1.0f;
            if (par.find("ymin") != std::string::npos) ay = 0.0f; else if (par.find("ymax") != std::string::npos) ay = 1.0f;
            tx = extra_x * ax; ty = extra_y * ay;
        }
        root = Matrix{sx, 0, 0, sy, tx - view_box[0] * sx, ty - view_box[1] * sy};
    } else {
        float w = Attr(svg, "width", target_width), h = Attr(svg, "height", target_height);
        root = Matrix{target_width / std::max(1.0f, w), 0, 0, target_height / std::max(1.0f, h), 0, 0};
    }
    SvgStyle base;
    base.fill = SvgPaint{true, 0, 0, 0, 255};
    base.color = current_color;
    Matrix m = root;
    SvgStyle style = builder.Inherit(svg, base, m);
    if (style.hidden) return builder.out;
    for (const auto &child : svg.children) builder.Draw(*child, style, m, 1);
    return builder.out;
}
