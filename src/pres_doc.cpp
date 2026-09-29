#include "pres_doc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "math_markup.h"
#include "pres_internal.h"

namespace pres {

std::string Paragraph::PlainText() const {
    std::string t;
    for (const TextRun &r : runs) t += r.text;
    return t;
}

TextRun MathRun(const std::string &tex, bool display) {
    TextRun r;
    r.text = kMathChar;
    r.tex = tex;
    r.display = display;
    return r;
}

bool IsEquationShape(const Shape &s) {
    return s.HasText() && s.paras.size() == 1 && s.paras[0].runs.size() == 1 && s.paras[0].runs[0].IsMath() && s.paras[0].runs[0].display;
}

bool Shape::HasText() const { return kind != ShapeKind::Image && kind != ShapeKind::Line; }

std::string Shape::PlainText() const {
    std::string t;
    for (size_t i = 0; i < paras.size(); ++i) {
        if (i) t += '\n';
        t += paras[i].PlainText();
    }
    return t;
}

namespace {

Paragraph Para(const std::string &text, Align align = Align::Left) {
    Paragraph p;
    p.align = align;
    if (text.empty()) return p;
    TextRun r;
    r.text = text;
    p.runs.push_back(r);
    return p;
}

}  // namespace

Presentation NewPresentation() {
    Presentation p;
    Slide s;
    Shape title;
    title.kind = ShapeKind::Text;
    title.name = "Title";
    title.is_title = true;
    title.x = p.width / 12;
    title.y = p.height * 30 / 100;
    title.w = p.width - 2 * title.x;
    title.h = p.height * 20 / 100;
    title.font_pt = 44;
    title.valign = VAlign::Bottom;
    title.paras.push_back(Para("Title", Align::Center));
    title.paras[0].runs[0].bold = true;
    s.shapes.push_back(title);
    Shape sub = title;
    sub.name = "Subtitle";
    sub.is_title = false;
    sub.y = title.y + title.h + p.height * 3 / 100;
    sub.h = p.height * 15 / 100;
    sub.font_pt = 24;
    sub.valign = VAlign::Top;
    sub.text_color = "59636E";
    sub.paras = {Para("Subtitle", Align::Center)};
    s.shapes.push_back(sub);
    p.slides.push_back(s);
    return p;
}

Slide NewSlide(const Presentation &p, bool with_boxes) {
    Slide s;
    if (!with_boxes) return s;
    const long margin = p.width / 24;
    Shape title;
    title.name = "Title";
    title.is_title = true;
    title.x = margin;
    title.y = p.height * 5 / 100;
    title.w = p.width - 2 * margin;
    title.h = p.height * 15 / 100;
    title.font_pt = 36;
    title.valign = VAlign::Middle;
    title.paras.push_back(Para("Title"));
    title.paras[0].runs[0].bold = true;
    s.shapes.push_back(title);
    Shape body;
    body.name = "Content";
    body.x = margin;
    body.y = title.y + title.h + p.height * 3 / 100;
    body.w = title.w;
    body.h = p.height - body.y - p.height * 7 / 100;
    body.font_pt = 24;
    body.autofit = true;
    Paragraph item = Para("Point");
    item.bullet = true;
    body.paras.push_back(item);
    s.shapes.push_back(body);
    return s;
}

Shape NewTextBox(const Presentation &p, const std::string &text) {
    Shape s;
    s.name = "Text";
    s.w = p.width / 3;
    s.h = p.height / 8;
    s.x = (p.width - s.w) / 2;
    s.y = (p.height - s.h) / 2;
    s.font_pt = 24;
    s.paras.push_back(Para(text));
    return s;
}

Shape NewShape(const Presentation &p, ShapeKind kind) {
    Shape s;
    s.kind = kind;
    s.name = kind == ShapeKind::Ellipse ? "Ellipse" : kind == ShapeKind::Line ? "Line" : "Rectangle";
    s.w = p.width / 4;
    s.h = kind == ShapeKind::Line ? 0 : p.height / 4;
    s.x = (p.width - s.w) / 2;
    s.y = (p.height - s.h) / 2;
    if (kind == ShapeKind::Line) {
        s.line = "1F2328";
        s.line_pt = 2;
    } else {
        s.fill = "6B8AFD";
        s.line = "3F5FD6";
        s.line_pt = 1;
        s.text_color = "FFFFFF";
        s.valign = VAlign::Middle;
        s.paras.push_back(Para("", Align::Center));
    }
    return s;
}

// --- Text editing ----------------------------------------------------------------------

namespace {

int PrevUtf8(const std::string &s, int off) {
    if (off <= 0) return 0;
    int i = off - 1;
    while (i > 0 && (static_cast<unsigned char>(s[static_cast<size_t>(i)]) & 0xC0) == 0x80) --i;
    return i;
}
int NextUtf8(const std::string &s, int off) {
    const int n = static_cast<int>(s.size());
    if (off >= n) return n;
    int i = off + 1;
    while (i < n && (static_cast<unsigned char>(s[static_cast<size_t>(i)]) & 0xC0) == 0x80) ++i;
    return i;
}

// The run holding byte `off` of a paragraph's text, and the offset in it.
// At a boundary between two runs the earlier one wins, so typing at the
// end of a bold word carries on in bold.
void LocateRun(const Paragraph &p, int off, size_t *run, int *in) {
    int pos = 0;
    for (size_t i = 0; i < p.runs.size(); ++i) {
        const int len = static_cast<int>(p.runs[i].text.size());
        if (off <= pos + len) {
            *run = i;
            *in = off - pos;
            return;
        }
        pos += len;
    }
    *run = p.runs.empty() ? 0 : p.runs.size() - 1;
    *in = p.runs.empty() ? 0 : static_cast<int>(p.runs.back().text.size());
}

// Deletes bytes [a, b) of a paragraph's text, across runs; runs left
// empty go, except the last one standing (it keeps the paragraph's style).
void DeleteTextRange(Paragraph &p, int a, int b) {
    int pos = 0;
    for (TextRun &r : p.runs) {
        const int len = static_cast<int>(r.text.size());
        const int lo = std::max(a, pos), hi = std::min(b, pos + len);
        if (lo < hi) r.text.erase(static_cast<size_t>(lo - pos), static_cast<size_t>(hi - lo));
        if (r.text.empty()) {
            r.tex.clear();  // an equation deleted is gone, not an empty equation
            r.display = false;
        }
        pos += len;
    }
    for (size_t i = p.runs.size(); i-- > 0;)
        if (p.runs[i].text.empty() && p.runs.size() > 1) p.runs.erase(p.runs.begin() + static_cast<long>(i));
}

void Clamp(Shape &s, int *para, int *off) {
    if (s.paras.empty()) s.paras.push_back(Paragraph{});
    *para = std::clamp(*para, 0, static_cast<int>(s.paras.size()) - 1);
    *off = std::clamp(*off, 0, static_cast<int>(s.paras[static_cast<size_t>(*para)].PlainText().size()));
}

}  // namespace

void InsertText(Shape &s, int *para, int *off, const std::string &text) {
    Clamp(s, para, off);
    Paragraph &p = s.paras[static_cast<size_t>(*para)];
    if (p.runs.empty()) {
        // A new paragraph takes the look of the text before it.
        TextRun r;
        for (int k = *para - 1; k >= 0; --k) {
            const Paragraph &prev = s.paras[static_cast<size_t>(k)];
            if (!prev.runs.empty()) {
                r = prev.runs.back();
                r.link.clear();
                break;
            }
        }
        r.text.clear();
        p.runs.push_back(r);
    }
    size_t run = 0;
    int in = 0;
    LocateRun(p, *off, &run, &in);
    if (p.runs[run].IsMath()) {
        // Beside an equation, not in it: a plain run of its own there
        // (the neighbouring text run's, when there is one).
        const bool after = in > 0;
        const size_t nb = after ? run + 1 : run;
        if (after && nb < p.runs.size() && !p.runs[nb].IsMath()) {
            run = nb;
            in = 0;
        } else if (!after && run > 0 && !p.runs[run - 1].IsMath()) {
            run = run - 1;
            in = static_cast<int>(p.runs[run].text.size());
        } else {
            TextRun plain = p.runs[run];
            plain.tex.clear();
            plain.display = false;
            plain.text.clear();
            p.runs.insert(p.runs.begin() + static_cast<long>(nb), plain);
            run = nb;
            in = 0;
        }
    }
    p.runs[run].text.insert(static_cast<size_t>(in), text);
    *off += static_cast<int>(text.size());
}

void InsertMath(Shape &s, int *para, int *off, const std::string &tex) {
    Clamp(s, para, off);
    Paragraph &p = s.paras[static_cast<size_t>(*para)];
    TextRun m = MathRun(tex, false);
    if (p.runs.empty()) {
        p.runs.push_back(m);
        *off += static_cast<int>(m.text.size());
        return;
    }
    size_t run = 0;
    int in = 0;
    LocateRun(p, *off, &run, &in);
    const TextRun &at = p.runs[run];
    m.bold = at.bold;
    m.italic = at.italic;
    m.color = at.color;
    m.size_pt = at.size_pt;
    if (at.IsMath() || in == static_cast<int>(at.text.size())) {
        p.runs.insert(p.runs.begin() + static_cast<long>(run) + (in > 0 ? 1 : 0), m);
    } else if (in == 0) {
        p.runs.insert(p.runs.begin() + static_cast<long>(run), m);
    } else {
        TextRun rest = at;
        rest.text = at.text.substr(static_cast<size_t>(in));
        p.runs[run].text.resize(static_cast<size_t>(in));
        p.runs.insert(p.runs.begin() + static_cast<long>(run) + 1, rest);
        p.runs.insert(p.runs.begin() + static_cast<long>(run) + 1, m);
    }
    *off += static_cast<int>(m.text.size());
}

bool DeleteChar(Shape &s, int *para, int *off, bool forward) {
    Clamp(s, para, off);
    Paragraph &p = s.paras[static_cast<size_t>(*para)];
    const std::string text = p.PlainText();
    if (!forward && *off > 0) {
        const int a = PrevUtf8(text, *off);
        DeleteTextRange(p, a, *off);
        *off = a;
    } else if (forward && *off < static_cast<int>(text.size())) {
        DeleteTextRange(p, *off, NextUtf8(text, *off));
    } else if (!forward && *para > 0) {
        // Joins the previous paragraph.
        Paragraph &prev = s.paras[static_cast<size_t>(*para - 1)];
        const int len = static_cast<int>(prev.PlainText().size());
        for (TextRun &r : p.runs)
            if (!r.text.empty()) prev.runs.push_back(r);
        s.paras.erase(s.paras.begin() + *para);
        --*para;
        *off = len;
    } else if (forward && *para + 1 < static_cast<int>(s.paras.size())) {
        const Paragraph next = s.paras[static_cast<size_t>(*para + 1)];
        for (const TextRun &r : next.runs)
            if (!r.text.empty()) p.runs.push_back(r);
        s.paras.erase(s.paras.begin() + *para + 1);
    } else {
        return false;
    }
    return true;
}

void SplitParagraph(Shape &s, int *para, int *off) {
    Clamp(s, para, off);
    Paragraph &p = s.paras[static_cast<size_t>(*para)];
    Paragraph tail = p;
    tail.runs.clear();
    size_t run = 0;
    int in = 0;
    LocateRun(p, *off, &run, &in);
    if (!p.runs.empty() && p.runs[run].IsMath()) {
        // An equation goes whole to whichever side the caret leaves it on.
        const size_t first_tail = in > 0 ? run + 1 : run;
        for (size_t k = first_tail; k < p.runs.size(); ++k) tail.runs.push_back(p.runs[k]);
        p.runs.resize(first_tail);
    } else if (!p.runs.empty()) {
        TextRun head = p.runs[run];
        TextRun rest = head;
        head.text = head.text.substr(0, static_cast<size_t>(in));
        rest.text = rest.text.substr(static_cast<size_t>(in));
        tail.runs.push_back(rest);
        for (size_t k = run + 1; k < p.runs.size(); ++k) tail.runs.push_back(p.runs[k]);
        p.runs.resize(run);
        p.runs.push_back(head);
    }
    s.paras.insert(s.paras.begin() + *para + 1, tail);
    ++*para;
    *off = 0;
}

Shape NewEquation(const Presentation &p, const std::string &tex, double pt) {
    Shape s;
    s.name = "Equation";
    double wem = 0, hem = 0;
    TexMathExtent(tex, true, &wem, &hem);
    s.font_pt = pt;
    // (With room to spare: the estimate is not the typesetter.)
    s.w = std::lround((wem * 1.15 + 1.0) * pt * kEmuPerPt);
    s.h = std::lround((hem + 0.8) * pt * kEmuPerPt);
    s.x = (p.width - s.w) / 2;
    s.y = (p.height - s.h) / 2;
    s.valign = VAlign::Middle;
    Paragraph para;
    para.align = Align::Center;
    para.runs.push_back(MathRun(tex, true));
    s.paras.push_back(para);
    return s;
}

bool IsPresentationPath(const std::string &path) {
    const std::string ext = detail::ExtOf(path);
    return ext == "pptx" || ext == "odp";
}

bool Load(const std::string &path, Presentation *out, std::string *error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string ext = detail::ExtOf(path);
    if (ext == "pptx") return LoadPptx(ss.str(), out, error);
    if (ext == "odp") return LoadOdp(ss.str(), out, error);
    if (error) *error = "not a presentation (.pptx or .odp): " + path;
    return false;
}

bool Save(const Presentation &p, const std::string &path, std::string *error) {
    const std::string ext = detail::ExtOf(path);
    std::string bytes;
    if (ext == "pptx") bytes = SavePptx(p);
    else if (ext == "odp") bytes = SaveOdp(p);
    else {
        if (error) *error = "cannot save a presentation as ." + ext + " (pptx or odp)";
        return false;
    }
    std::ofstream f(path, std::ios::binary);
    f << bytes;
    if (!f) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

std::string NormalizeColor(const std::string &c) {
    std::string s = c;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() == 3) s = std::string{s[0], s[0], s[1], s[1], s[2], s[2]};
    if (s.size() != 6) return "";
    for (char &ch : s) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return "";
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return s;
}

long OdfLengthToEmu(const std::string &len) {
    char *end = nullptr;
    const double v = std::strtod(len.c_str(), &end);
    const std::string unit = end ? std::string(end) : std::string();
    double emu_per = 360000.0;  // cm
    if (unit == "mm") emu_per = 36000.0;
    else if (unit == "in" || unit == "inch") emu_per = 914400.0;
    else if (unit == "pt") emu_per = 12700.0;
    else if (unit == "pc") emu_per = 152400.0;
    else if (unit == "px") emu_per = 9525.0;
    else if (unit == "m") emu_per = 36000000.0;
    return std::lround(v * emu_per);
}

std::string EmuToCm(long emu) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3fcm", static_cast<double>(emu) / 360000.0);
    return b;
}

// --- Shared helpers ------------------------------------------------------------------

namespace detail {

const char *Local(const char *name) {
    if (!name) return "";
    const char *colon = std::strrchr(name, ':');
    return colon ? colon + 1 : name;
}

bool Is(const xml::xml_node &n, const char *local) { return n && n.type() == xml::node_element && std::strcmp(Local(n.name()), local) == 0; }

xml::xml_node Child(const xml::xml_node &n, const char *local) {
    if (!n) return {};
    for (xml::xml_node c = n.first_child(); c; c = c.next_sibling())
        if (Is(c, local)) return c;
    return {};
}

std::vector<xml::xml_node> Children(const xml::xml_node &n, const char *local) {
    std::vector<xml::xml_node> out;
    if (!n) return out;
    for (xml::xml_node c = n.first_child(); c; c = c.next_sibling())
        if (Is(c, local)) out.push_back(c);
    return out;
}

std::vector<xml::xml_node> Elements(const xml::xml_node &n) {
    std::vector<xml::xml_node> out;
    if (!n) return out;
    for (xml::xml_node c = n.first_child(); c; c = c.next_sibling())
        if (c.type() == xml::node_element) out.push_back(c);
    return out;
}

xml::xml_node Path(const xml::xml_node &n, const char *path) {
    xml::xml_node cur = n;
    std::string p = path;
    size_t i = 0;
    while (cur && i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        cur = Child(cur, p.substr(i, j - i).c_str());
        i = j + 1;
    }
    return cur;
}

std::string Attr(const xml::xml_node &n, const char *local) {
    if (!n) return "";
    if (xml::xml_attribute a = n.attribute(local)) return a.as_string();
    // By local name, whatever its prefix.
    for (const char *prefix : {"r:", "xlink:", "svg:", "draw:", "fo:", "style:", "text:", "presentation:", "table:", "office:", "a:", "p:"}) {
        const std::string q = std::string(prefix) + local;
        if (xml::xml_attribute a = n.attribute(q.c_str())) return a.as_string();
    }
    return "";
}

bool HasAttr(const xml::xml_node &n, const char *local) {
    if (!n) return false;
    if (n.attribute(local)) return true;
    for (const char *prefix : {"r:", "xlink:", "svg:", "draw:", "fo:", "style:", "text:", "presentation:", "table:", "office:", "a:", "p:"}) {
        const std::string q = std::string(prefix) + local;
        if (n.attribute(q.c_str())) return true;
    }
    return false;
}

std::string AllText(const xml::xml_node &n) {
    std::string t;
    for (xml::xml_node c = n.first_child(); c; c = c.next_sibling()) {
        if (c.type() == xml::node_pcdata || c.type() == xml::node_cdata) t += c.value();
        else if (c.type() == xml::node_element) t += AllText(c);
    }
    return t;
}

bool ParseXml(const std::string &text, xml::xml_document *doc, xml::xml_node *root) {
    if (!doc->load_buffer(text.data(), text.size())) return false;
    *root = doc->document_element();
    return static_cast<bool>(*root);
}

std::string Esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n') break;
                o += c;
        }
    }
    return o;
}

std::string Num(long v) { return std::to_string(v); }

namespace {

void Rgb(const std::string &rgb, double *r, double *g, double *b) {
    const long v = std::strtol(rgb.c_str(), nullptr, 16);
    *r = static_cast<double>((v >> 16) & 0xFF) / 255.0;
    *g = static_cast<double>((v >> 8) & 0xFF) / 255.0;
    *b = static_cast<double>(v & 0xFF) / 255.0;
}

std::string Hex(double r, double g, double b) {
    char out[8];
    auto c = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    std::snprintf(out, sizeof out, "%02X%02X%02X", c(r), c(g), c(b));
    return out;
}

double HueToRgb(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

}  // namespace

std::string ApplyLum(const std::string &rgb, int lum_mod, int lum_off) {
    if (rgb.size() != 6 || (lum_mod == 100000 && lum_off == 0)) return rgb;
    double r, g, b;
    Rgb(rgb, &r, &g, &b);
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    double h = 0, s = 0, l = (mx + mn) / 2;
    if (mx != mn) {
        const double d = mx - mn;
        s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
        if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
        else if (mx == g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h /= 6;
    }
    l = std::clamp(l * lum_mod / 100000.0 + lum_off / 100000.0, 0.0, 1.0);
    if (s == 0) return Hex(l, l, l);
    const double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    return Hex(HueToRgb(p, q, h + 1.0 / 3), HueToRgb(p, q, h), HueToRgb(p, q, h - 1.0 / 3));
}

std::string ApplyShade(const std::string &rgb, int shade, int tint) {
    if (rgb.size() != 6) return rgb;
    double r, g, b;
    Rgb(rgb, &r, &g, &b);
    if (shade > 0) {
        const double k = shade / 100000.0;
        r *= k;
        g *= k;
        b *= k;
    }
    if (tint > 0) {
        const double k = tint / 100000.0;
        r = 1 - (1 - r) * k;
        g = 1 - (1 - g) * k;
        b = 1 - (1 - b) * k;
    }
    return Hex(r, g, b);
}

std::string ResolvePart(const std::string &owner_part, const std::string &target) {
    std::string base;
    if (!target.empty() && target[0] == '/') {
        base = target.substr(1);
    } else {
        const size_t slash = owner_part.rfind('/');
        base = (slash == std::string::npos ? std::string() : owner_part.substr(0, slash + 1)) + target;
    }
    std::vector<std::string> parts;
    std::stringstream ss(base);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(seg);
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? "/" : "") + parts[i];
    return out;
}

std::string ExtOf(const std::string &path) {
    const size_t dot = path.rfind('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = path.substr(dot + 1);
    for (char &c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (e == "jpg") e = "jpeg";
    return e;
}

std::string MimeForExt(const std::string &ext) {
    if (ext == "png") return "image/png";
    if (ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "bmp") return "image/bmp";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "emf") return "image/x-emf";
    if (ext == "wmf") return "image/x-wmf";
    if (ext == "tiff" || ext == "tif") return "image/tiff";
    return "application/octet-stream";
}

}  // namespace detail
}  // namespace pres
