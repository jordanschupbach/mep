// HTML, Markdown, Org, RTF, DOCX, ODT and plain text -> mepml text. See
// mepml_convert.h. Every reader turns its source into runs of text with a
// format (Seg) and hands them to one writer (RenderSegs), which knows
// mepml's rules: markers are only placed where they will parse as markup
// (mepml cannot open `*bold*` in the middle of a word, so such styling is
// dropped rather than written as literal asterisks), and prose characters
// that would read as markup are escaped.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "html_doc.h"
#include "mepml_convert.h"
#include "office_doc.h"
#include "xml_doc.h"
#include "zip_archive.h"

namespace mepml {

namespace {

// ===========================================================================
// Shared: runs of formatted text -> mepml
// ===========================================================================

struct Fmt {
    bool b = false, i = false, u = false, s = false, sup = false, sub = false, small = false, big = false;
    bool mono = false, mark = false, ins = false, del = false;
    std::string color, font, size, link;
};

struct Seg {
    enum Kind { Text, Code, Math, DisplayMath, Footnote, Cite, CiteP, Break } kind = Text;
    std::string text;  // Text/Code/Math: the content; Footnote: rendered mepml; Cite: the key
    Fmt f;
    bool hard = false;  // ODF: explicit spacing (<text:s/>), never collapsed
};

bool IsSp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool IsMarker(char c) { return c != 0 && std::strchr("*~_^<>|=-+!,", c) != nullptr; }
bool Pre(char c) { return c == 0 || IsSp(c) || std::strchr("([{\"'/", c) != nullptr || IsMarker(c); }
bool Post(char c) { return c == 0 || IsSp(c) || std::strchr(".,;:!?)]}\"'/", c) != nullptr || IsMarker(c); }

std::string Trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string Upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::vector<std::string> SplitLines(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
bool StartsWith(const std::string &s, const std::string &p) { return s.rfind(p, 0) == 0; }

// A group's text for a {...} argument: braces balanced by escaping.
std::string GroupEsc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '{' || c == '}' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

// The text of a `\name(...)` group: a parenthesis with no partner takes a
// backslash, so the group closes where it should. (An unmatched `(` never
// comes before an unmatched `)`, so no `\(` ... `\)` pair reads as maths.)
std::string ParenEsc(const std::string &s) {
    std::vector<size_t> open, lone;
    for (size_t k = 0; k < s.size(); ++k) {
        if (s[k] == '\\') ++k;
        else if (s[k] == '(') open.push_back(k);
        else if (s[k] == ')' && open.empty()) lone.push_back(k);
        else if (s[k] == ')') open.pop_back();
    }
    lone.insert(lone.end(), open.begin(), open.end());
    if (lone.empty()) return s;
    std::sort(lone.begin(), lone.end());
    std::string o;
    size_t at = 0;
    for (size_t k : lone) {
        o += s.substr(at, k - at) + "\\";
        at = k;
    }
    return o + s.substr(at);
}

// An argument before a command's text (a font, a colour, a key): it ends at
// a comma, so it holds none, nor any parenthesis.
std::string ArgEsc(const std::string &s) {
    std::string o;
    for (char c : s)
        if (c != ',' && c != '(' && c != ')') o += c;
    return Trim(o);
}

// A citation as mepml: `\citation(key, fields)`, or the older
// `@citation{key}{fields}` when a parenthesis in the fields would end the
// group early (inside a `{...}` value there is no escaping it).
std::string CitationEntry(const std::string &key, const std::string &fields) {
    int depth = 0;
    for (size_t k = 0; k < fields.size() && depth >= 0; ++k) {
        if (fields[k] == '\\') ++k;
        else if (fields[k] == '(') ++depth;
        else if (fields[k] == ')') --depth;
    }
    if (depth != 0) return "@citation{" + key + "}{" + fields + "\n}";
    return "\\citation(" + ArgEsc(key) + "," + fields + "\n)";
}

// A link target inside [text|url]: `|` and `]` would end it early.
std::string UrlEsc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '|') o += "%7C";
        else if (c == ']') o += "%5D";
        else if (c == ' ') o += "%20";
        else o += c;
    }
    return o;
}

struct MarkerDef {
    bool Fmt::*flag;
    const char *open, *close;
    bool intraword;
};
const MarkerDef kMarkerDefs[] = {
    {&Fmt::b, "*", "*", false},       {&Fmt::i, "~", "~", false},     {&Fmt::u, "_", "_", false},
    {&Fmt::s, "-", "-", false},       {&Fmt::ins, "+", "+", false},   {&Fmt::del, "!", "!", false},
    {&Fmt::mark, "=", "=", false},    {&Fmt::mono, "|", "|", false},  {&Fmt::small, "<", ">", false},
    {&Fmt::big, ">", "<", false},     {&Fmt::sup, "^", "^", true},    {&Fmt::sub, ",,", ",,", true},
};

std::string RenderPlainSegs(const std::vector<Seg> &segs, size_t from, size_t to, const std::string &before);

// Next character after segs[i] in the source, for a closer's POST check.
char NextChar(const std::vector<Seg> &segs, size_t i, size_t to) {
    for (size_t k = i + 1; k < to; ++k) {
        if (segs[k].kind == Seg::Break) return '\n';
        if (segs[k].kind != Seg::Text) return 'x';
        if (!segs[k].text.empty()) return segs[k].text[0];
    }
    return 0;
}

std::string RenderPlainSegs(const std::vector<Seg> &segs, size_t from, size_t to, const std::string &before) {
    std::string out;
    auto last = [&]() -> char {
        if (!out.empty()) return out.back();
        return before.empty() ? 0 : before.back();
    };
    for (size_t i = from; i < to; ++i) {
        const Seg &sg = segs[i];
        std::string piece;
        switch (sg.kind) {
            case Seg::Break: out += "\n"; continue;
            case Seg::Code:
                if (sg.text.find('`') == std::string::npos && !sg.text.empty()) piece = "`" + sg.text + "`";
                else piece = EscapeInline(sg.text);
                break;
            case Seg::Math: piece = "\\(" + Trim(sg.text) + "\\)"; break;
            case Seg::DisplayMath: piece = "$$" + Trim(sg.text) + "$$"; break;
            case Seg::Footnote: piece = "\\fn(" + ParenEsc(sg.text) + ")"; break;
            case Seg::Cite: piece = "\\cite(" + ArgEsc(sg.text) + ")"; break;
            case Seg::CiteP: piece = "\\citep(" + ArgEsc(sg.text) + ")"; break;
            case Seg::Text: {
                // Whitespace at the run's edges goes outside its markers.
                // Zero-width spaces are dropped: they are how Org escapes a
                // marker (see OrgWriter::Text), and have done that job.
                std::string t = sg.text;
                for (size_t z = t.find("\u200b"); z != std::string::npos; z = t.find("\u200b", z)) t.erase(z, 3);
                size_t a = 0, b = t.size();
                while (a < b && IsSp(t[a])) ++a;
                while (b > a && IsSp(t[b - 1])) --b;
                const std::string lead = t.substr(0, a), core = t.substr(a, b - a), trail = t.substr(b);
                out += lead;
                if (core.empty()) {
                    out += trail;
                    continue;
                }
                std::string body = EscapeInline(core);
                // Emphasis markers, innermost first, kept only where they
                // parse: an opener after a space or punctuation, a closer
                // before one (super/subscript excepted).
                const char prev = last();
                const char next = trail.empty() ? NextChar(segs, i, to) : trail[0];
                bool any_word_marker = false;
                for (const MarkerDef &m : kMarkerDefs)
                    if (sg.f.*m.flag && !m.intraword) any_word_marker = true;
                const bool bounds_ok = Pre(prev) && Post(next);
                for (auto it = std::rbegin(kMarkerDefs); it != std::rend(kMarkerDefs); ++it) {
                    const MarkerDef &m = *it;
                    if (!(sg.f.*m.flag)) continue;
                    if (!m.intraword && !bounds_ok) continue;
                    if (!m.intraword && std::strlen(m.open) == 1 && body[0] == m.open[0]) continue;  // `**x`
                    body = std::string(m.open) + body + m.close;
                }
                (void)any_word_marker;
                if (!sg.f.size.empty() || !sg.f.font.empty() || !sg.f.color.empty()) body = ParenEsc(body);
                if (!sg.f.size.empty()) body = "\\fs(" + ArgEsc(sg.f.size) + ", " + body + ")";
                if (!sg.f.font.empty()) body = "\\f(" + ArgEsc(sg.f.font) + ", " + body + ")";
                if (!sg.f.color.empty()) body = "\\color(" + ArgEsc(sg.f.color) + ", " + body + ")";
                out += body + trail;
                continue;
            }
        }
        out += piece;
    }
    return out;
}

// Runs -> mepml, grouping consecutive runs of one link into [text|url].
std::string RenderSegs(const std::vector<Seg> &segs) {
    std::string out;
    size_t i = 0;
    while (i < segs.size()) {
        const std::string &url = segs[i].f.link;
        size_t j = i + 1;
        while (j < segs.size() && segs[j].f.link == url && segs[j].kind != Seg::Break) ++j;
        if (url.empty()) {
            out += RenderPlainSegs(segs, i, j, out);
        } else {
            std::vector<Seg> inner(segs.begin() + static_cast<long>(i), segs.begin() + static_cast<long>(j));
            for (Seg &s : inner) s.f.link.clear();
            std::string text = Trim(RenderPlainSegs(inner, 0, inner.size(), "["));
            if (text.empty() || text == EscapeInline(url)) out += "[" + UrlEsc(url) + "]";
            else out += "[" + text + "|" + UrlEsc(url) + "]";
        }
        i = j;
    }
    return out;
}

// Consecutive text runs of one format become one run.
void Coalesce(std::vector<Seg> &segs) {
    std::vector<Seg> out;
    for (Seg &s : segs) {
        if (!out.empty() && s.kind == Seg::Text && out.back().kind == Seg::Text) {
            const Fmt &a = out.back().f, &b = s.f;
            bool same = a.link == b.link && a.color == b.color && a.font == b.font && a.size == b.size;
            for (const MarkerDef &m : kMarkerDefs) same = same && a.*m.flag == b.*m.flag;
            if (same) {
                out.back().text += s.text;
                continue;
            }
        }
        out.push_back(std::move(s));
    }
    segs.swap(out);
}

// --- document writer --------------------------------------------------------

struct Out {
    std::vector<std::string> meta;
    std::vector<std::string> blocks;
    enum Last { kNone, kCode, kFigure, kTable, kOther } last = kNone;

    void Block(const std::string &text, Last kind = kOther) {
        if (Trim(text).empty()) return;
        blocks.push_back(text);
        last = kind;
    }
    // Lines that must directly follow the previous block (results, a
    // caption), or become a block of their own when there is nothing to
    // attach to.
    void Attach(const std::string &text) {
        if (blocks.empty()) blocks.push_back(text);
        else blocks.back() += "\n" + text;
    }
    void Paragraph(const std::vector<Seg> &segs_in) {
        std::vector<Seg> segs = segs_in;
        Coalesce(segs);
        std::string text = Trim(RenderSegs(segs));
        if (text.empty()) return;
        std::vector<std::string> lines = SplitLines(text);
        std::string o;
        for (const std::string &l : lines) {
            const std::string t = Trim(l);
            if (t.empty()) continue;
            o += (o.empty() ? "" : "\n") + EscapeLineStart(t);
        }
        Block(o);
    }
    void Heading(int level, const std::vector<Seg> &segs) {
        std::vector<Seg> s = segs;
        Coalesce(s);
        std::string text = Trim(RenderSegs(s));
        for (char &c : text)
            if (c == '\n') c = ' ';
        Block(std::string(static_cast<size_t>(std::clamp(level, 1, 6)), '>') + " " + text);
    }
    void Code(const std::string &lang, const std::vector<std::pair<std::string, std::string>> &opts, const std::string &body) {
        std::string head = "```";
        if (!opts.empty()) {
            head += "{" + (lang.empty() ? std::string("text") : lang);
            for (const auto &o : opts) {
                std::string v;
                for (char c : o.second) v += (c == '"' || c == '\\') ? std::string("\\") + c : std::string(1, c);
                head += ", " + o.first + "=\"" + v + "\"";
            }
            head += "}";
        } else {
            head += lang;
        }
        std::string b = body;
        while (!b.empty() && b.back() == '\n') b.pop_back();
        // A body line that is exactly ``` would end the block early.
        std::string safe;
        for (const std::string &l : SplitLines(b + "\n")) safe += (Trim(l) == "```" ? " " + l : l) + "\n";
        Block(head + "\n" + safe + "```", kCode);
    }
    // `format` is the results' kind ("" for text, "html"; see
    // mepml::Block::result_format).
    void Results(const std::vector<std::string> &lines, const std::string &format = "") {
        if (last != kCode) {
            Code("", {}, [&] {
                std::string s;
                for (const std::string &l : lines) s += l + "\n";
                return s;
            }());
            return;
        }
        std::string r = format.empty() ? "// result_begin:" : "// result_begin: " + format;
        for (const std::string &l : lines) r += "\n" + (l.empty() ? std::string("//") : "// " + l);
        r += "\n// result_end";
        Attach(r);
    }
    void Caption(const std::vector<Seg> &segs) {
        std::vector<Seg> s = segs;
        Coalesce(s);
        std::string text = Trim(RenderSegs(s));
        for (char &c : text)
            if (c == '\n') c = ' ';
        if (text.empty()) return;
        if (last == kFigure || last == kTable || last == kCode) Attach("\\caption(" + ParenEsc(text) + ")");
        else Paragraph(segs);
    }
    void Image(const std::string &src, const std::string &alt) {
        if (src.empty()) return;
        std::string o = "\\image(" + ParenEsc(src) + ")";
        if (!alt.empty()) o += "\n\\alttext(" + ParenEsc(alt) + ")";
        Block(o, kFigure);
    }
    void Callout(const std::string &kind, const std::vector<Seg> &segs) {
        std::vector<Seg> s = segs;
        Coalesce(s);
        std::string text = Trim(RenderSegs(s));
        std::string k = Upper(kind);
        const auto &kws = CalloutKeywords();
        if (std::find(kws.begin(), kws.end(), k) == kws.end()) k = "NOTE";
        std::string o;
        bool first = true;
        for (const std::string &l : SplitLines(text)) {
            if (Trim(l).empty()) continue;
            o += first ? "// " + k + ": " + Trim(l) : "\n// " + Trim(l);
            first = false;
        }
        if (first) o = "// " + k + ":";
        Block(o);
    }
    void Math(const std::string &tex) { Block("$$\n" + Trim(tex) + "\n$$", kFigure); }
    // An abstract, one run of segments per paragraph.
    void Abstract(const std::vector<std::vector<Seg>> &paras) {
        std::vector<std::string> texts;
        for (const std::vector<Seg> &p : paras) {
            std::vector<Seg> segs = p;
            Coalesce(segs);
            std::string o;
            for (const std::string &l : SplitLines(Trim(RenderSegs(segs))))
                if (!Trim(l).empty()) o += (o.empty() ? "" : "\n") + Trim(l);
            if (!o.empty()) texts.push_back(o);
        }
        if (texts.empty()) return;
        std::string body;
        for (const std::string &t : texts) body += (body.empty() ? "" : "\n\n") + t;
        Block("\\abstract(\n" + ParenEsc(body) + "\n)");
    }
    void Rule() { Block("---"); }
    void Comment(const std::string &text) {
        std::string o;
        for (const std::string &l : SplitLines(text)) o += (o.empty() ? "" : "\n") + ("// " + Trim(l));
        // A comment line that happens to read as a callout or results marker
        // is not one here.
        Block(o);
    }
    // rows[0..header) are header rows.
    void Table(const std::vector<std::vector<std::vector<Seg>>> &rows, int header, const std::vector<Align> &aligns) {
        if (rows.empty()) return;
        size_t cols = 0;
        for (const auto &r : rows) cols = std::max(cols, r.size());
        if (cols == 0) return;
        std::vector<std::string> lines;
        auto cell = [&](const std::vector<Seg> &segs) {
            std::vector<Seg> s = segs;
            // |mono| would read as cell borders: `verbatim` instead.
            for (Seg &sg : s)
                if (sg.kind == Seg::Text && sg.f.mono && !sg.text.empty() && sg.text.find('`') == std::string::npos) {
                    sg.kind = Seg::Code;
                    sg.f.mono = false;
                }
            Coalesce(s);
            std::string t = Trim(RenderSegs(s));
            std::string o;
            for (char c : t) {
                if (c == '\n') c = ' ';
                if (c == '|') o += '\\';
                o += c;
            }
            return o.empty() ? std::string(" ") : o;
        };
        auto sep = [&]() {
            std::string o = "|";
            for (size_t c = 0; c < cols; ++c) {
                const Align a = c < aligns.size() ? aligns[c] : Align::Default;
                o += a == Align::Left ? " :--- |" : a == Align::Right ? " ---: |" : a == Align::Center ? " :---: |" : " --- |";
            }
            return o;
        };
        for (size_t r = 0; r < rows.size(); ++r) {
            std::string o = "|";
            for (size_t c = 0; c < cols; ++c) o += " " + (c < rows[r].size() ? cell(rows[r][c]) : std::string(" ")) + " |";
            lines.push_back(o);
            if (static_cast<int>(r) + 1 == header) lines.push_back(sep());
        }
        if (header <= 0 && !aligns.empty()) {
            // Alignment with no header row: mepml needs a row above the
            // separator, so the first row becomes the header.
            lines.insert(lines.begin() + 1, sep());
        }
        std::string o;
        for (const std::string &l : lines) o += (o.empty() ? "" : "\n") + l;
        Block(o, kTable);
    }
    struct Item {
        int indent = 0;
        bool ordered = false;
        int number = 1;
        int checkbox = -1;
        std::vector<Seg> segs;
    };
    void List(const std::vector<Item> &items) {
        std::string o;
        for (const Item &it : items) {
            std::vector<Seg> s = it.segs;
            Coalesce(s);
            std::string text = Trim(RenderSegs(s));
            for (char &c : text)
                if (c == '\n') c = ' ';
            std::string m = it.ordered ? std::to_string(it.number) + ". " : "- ";
            if (it.checkbox >= 0) m += it.checkbox ? "[x] " : "[ ] ";
            o += (o.empty() ? "" : "\n") + std::string(static_cast<size_t>(std::max(0, it.indent)), ' ') + m + text;
        }
        Block(o);
    }
    std::string Str() const {
        std::string o;
        for (const std::string &m : meta) o += m + "\n";
        if (!meta.empty() && !blocks.empty()) o += "\n";
        for (size_t i = 0; i < blocks.size(); ++i) o += (i ? "\n\n" : "") + blocks[i];
        return o + "\n";
    }
};

std::vector<Seg> TextSegs(const std::string &t, const Fmt &f = Fmt()) {
    Seg s;
    s.text = t;
    s.f = f;
    return {s};
}

}  // namespace

// ===========================================================================
// Escaping
// ===========================================================================

std::string EscapeInline(const std::string &t) {
    std::string o;
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        const char p = i ? t[i - 1] : ' ';
        const char n = i + 1 < t.size() ? t[i + 1] : ' ';
        bool esc = false;
        switch (c) {
            case '\\': esc = !IsSp(n); break;
            case '`':
            case '{':
            case '}':
            case '^': esc = true; break;
            case '$': esc = !IsSp(n) && !std::isdigit(static_cast<unsigned char>(n)); break;
            case ']': esc = true; break;  // (no link can close)
            case ',': esc = n == ','; break;
            case '/': esc = n == '/' && (IsSp(p) || i == 0); break;
            default:
                if (IsMarker(c)) esc = (Pre(p) && !IsSp(n)) || (!IsSp(p) && Post(n));
        }
        if (esc) o += '\\';
        o += c;
    }
    return o;
}

std::string EscapeLineStart(const std::string &line) {
    const size_t k = line.find_first_not_of(" \t");
    if (k == std::string::npos) return line;
    const std::string t = line.substr(k);
    if (t[0] == '\\') return line;
    bool block = false;
    if (t[0] == '>') {
        size_t j = 0;
        while (j < t.size() && t[j] == '>') ++j;
        block = j == t.size() || t[j] == ' ' || t[j] == '\t';
    } else if (StartsWith(t, "//") || StartsWith(t, "```") || StartsWith(t, "$$") || t[0] == '|' || t[0] == '@') {
        block = true;
    } else if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && (t[1] == ' ' || t[1] == '\t')) {
        block = true;
    } else if (std::isdigit(static_cast<unsigned char>(t[0]))) {
        size_t j = 0;
        while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) ++j;
        block = j < t.size() && (t[j] == '.' || t[j] == ')') && j + 1 < t.size() && t[j + 1] == ' ';
    } else if (t.size() >= 3 && std::strchr("-=_*", t[0]) && t.find_first_not_of(t[0]) == std::string::npos) {
        block = true;
    }
    return block ? line.substr(0, k) + "\\" + t : line;
}

// ===========================================================================
// HTML
// ===========================================================================

namespace {

struct HtmlReader {
    Out out;
    std::map<std::string, std::string> footnotes;  // "#fn1" -> rendered mepml
    std::vector<std::string> citations;            // \citation blocks from a bibliography's data-bib-*
    std::vector<std::string> html_result_sources;  // HtmlResultSources(), in document order
    size_t next_html_result = 0;

    static std::string Attr(const DomNode *n, const char *name) {
        auto it = n->attrs.find(name);
        return it == n->attrs.end() ? std::string() : it->second;
    }
    static bool HasClass(const DomNode *n, const std::string &cls) {
        std::istringstream ss(Attr(n, "class"));
        std::string c;
        while (ss >> c)
            if (c == cls) return true;
        return false;
    }
    static std::string TextOf(const DomNode *n) {
        if (n->type == DomNodeType::Text) return n->text;
        std::string o;
        for (const auto &c : n->children) o += TextOf(c.get());
        return o;
    }
    static std::string Collapse(const std::string &s) {
        std::string o;
        bool sp = false;
        for (char c : s) {
            if (IsSp(c)) {
                sp = true;
                continue;
            }
            if (sp) o += ' ';
            sp = false;
            o += c;
        }
        if (sp) o += ' ';
        return o;
    }
    static const DomNode *Child(const DomNode *n, const std::string &tag) {
        for (const auto &c : n->children)
            if (c->type == DomNodeType::Element && c->tag == tag) return c.get();
        return nullptr;
    }
    // "Figure 3: x" / "Table 1: x" -> "x" (mepml numbers them itself).
    static void StripLabel(std::vector<Seg> &segs) {
        for (Seg &s : segs) {
            if (s.kind != Seg::Text) break;
            std::string t = s.text;
            const size_t a = t.find_first_not_of(' ');
            if (a == std::string::npos) continue;
            t = t.substr(a);
            for (const char *label : {"Figure ", "Table "}) {
                if (!StartsWith(t, label)) continue;
                size_t k = std::strlen(label);
                while (k < t.size() && std::isdigit(static_cast<unsigned char>(t[k]))) ++k;
                if (k < t.size() && t[k] == ':') s.text = Trim(t.substr(k + 1)) + (t.back() == ' ' ? " " : "");
            }
            break;
        }
    }

    void Inl(const DomNode *n, Fmt f, std::vector<Seg> &segs) {
        if (n->type == DomNodeType::Text) {
            Seg s;
            s.text = Collapse(n->text);
            s.f = f;
            if (!s.text.empty()) segs.push_back(s);
            return;
        }
        if (n->type != DomNodeType::Element) return;
        const std::string &tag = n->tag;
        if (tag == "script" || tag == "style" || tag == "template") return;
        if (tag == "br") {
            Seg s;
            s.kind = Seg::Break;
            segs.push_back(s);
            return;
        }
        if (tag == "math") {
            Seg s;
            s.kind = Attr(n, "display") == "1" ? Seg::DisplayMath : Seg::Math;
            s.text = TextOf(n);
            segs.push_back(s);
            return;
        }
        if (tag == "code" || tag == "kbd" || tag == "samp" || tag == "tt") {
            Seg s;
            s.kind = Seg::Code;
            s.text = TextOf(n);
            s.f = f;
            segs.push_back(s);
            return;
        }
        if (tag == "img") {
            Seg s;
            s.text = Attr(n, "alt").empty() ? Attr(n, "src") : Attr(n, "alt");
            s.f = f;
            s.f.link = Attr(n, "src");
            segs.push_back(s);
            return;
        }
        if (tag == "input") {
            if (Lower(Attr(n, "type")) == "checkbox") segs.push_back(TextSegs(n->attrs.count("checked") ? "[x] " : "[ ] ")[0]);
            return;
        }
        if (tag == "sup" && HasClass(n, "fnref")) {
            const DomNode *a = Child(n, "a");
            const std::string href = a ? Attr(a, "href") : "";
            if (footnotes.count(href)) {
                Seg s;
                s.kind = Seg::Footnote;
                s.text = footnotes[href];
                segs.push_back(s);
                return;
            }
        }
        if (tag == "a" && (HasClass(n, "footnote-ref") || Attr(n, "role") == "doc-noteref") && footnotes.count(Attr(n, "href"))) {
            // pandoc's footnote reference
            Seg s;
            s.kind = Seg::Footnote;
            s.text = footnotes[Attr(n, "href")];
            segs.push_back(s);
            return;
        }
        if (tag == "a") {
            if (!Attr(n, "data-key").empty()) {
                Seg s;
                s.kind = Attr(n, "data-kind") == "citep" ? Seg::CiteP : Seg::Cite;
                s.text = Attr(n, "data-key");
                segs.push_back(s);
                return;
            }
            const std::string href = Attr(n, "href");
            if (StartsWith(href, "#fnref")) return;  // a footnote's back-link
            if (!href.empty() && href[0] != '#') f.link = href;
        }
        if (tag == "b" || tag == "strong") f.b = true;
        else if (tag == "i" || tag == "em" || tag == "cite" || tag == "var" || tag == "dfn") f.i = true;
        else if (tag == "u") f.u = true;
        else if (tag == "s" || tag == "strike") f.s = true;
        else if (tag == "del") f.del = true;
        else if (tag == "ins") f.ins = true;
        else if (tag == "mark") f.mark = true;
        else if (tag == "sup") f.sup = true;
        else if (tag == "sub") f.sub = true;
        else if (tag == "small") f.small = true;
        else if (tag == "big" || HasClass(n, "big")) f.big = true;
        if (HasClass(n, "mono")) f.mono = true;
        const std::string style = Attr(n, "style");
        if (!style.empty()) {
            std::istringstream ss(style);
            std::string decl;
            while (std::getline(ss, decl, ';')) {
                const size_t colon = decl.find(':');
                if (colon == std::string::npos) continue;
                const std::string prop = Lower(Trim(decl.substr(0, colon)));
                const std::string val = Trim(decl.substr(colon + 1));
                if (prop == "color") f.color = val;
                else if (prop == "font-family") f.font = Trim(val.substr(0, val.find(',')));
                else if (prop == "font-size") {
                    double v = std::atof(val.c_str());
                    if (val.find("px") != std::string::npos) v *= 0.75;
                    if (v > 0) {
                        char buf[16];
                        std::snprintf(buf, sizeof buf, "%g", v);
                        f.size = buf;
                    }
                } else if (prop == "text-decoration" && val.find("underline") != std::string::npos) f.u = true;
                else if (prop == "text-decoration" && val.find("line-through") != std::string::npos) f.s = true;
                else if (prop == "font-weight" && (val == "bold" || std::atoi(val.c_str()) >= 600)) f.b = true;
                else if (prop == "font-style" && val == "italic") f.i = true;
            }
        }
        for (const auto &c : n->children) Inl(c.get(), f, segs);
    }

    std::vector<Seg> InlOf(const DomNode *n) {
        std::vector<Seg> segs;
        for (const auto &c : n->children) Inl(c.get(), Fmt(), segs);
        return segs;
    }

    static bool IsBlockTag(const std::string &t) {
        static const std::set<std::string> k = {"p", "div", "section", "article", "main", "header", "footer", "nav", "aside",
                                                "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "table", "pre", "blockquote",
                                                "figure", "hr", "dl", "dt", "dd", "body", "html", "head", "form", "address",
                                                "details", "summary", "figcaption", "center"};
        return k.count(t) > 0;
    }

    void List(const DomNode *n, int indent, std::vector<Out::Item> &items) {
        const bool ordered = n->tag == "ol";
        int number = std::max(1, std::atoi(Attr(n, "start").c_str()));
        for (const auto &c : n->children) {
            if (c->type != DomNodeType::Element || c->tag != "li") continue;
            Out::Item it;
            it.indent = indent;
            it.ordered = ordered;
            it.number = number++;
            std::vector<const DomNode *> nested;
            for (const auto &k : c->children) {
                if (k->type == DomNodeType::Element && (k->tag == "ul" || k->tag == "ol")) {
                    nested.push_back(k.get());
                    continue;
                }
                if (k->type == DomNodeType::Element && k->tag == "input" && Lower(Attr(k.get(), "type")) == "checkbox") {
                    it.checkbox = k->attrs.count("checked") ? 1 : 0;
                    continue;
                }
                Inl(k.get(), Fmt(), it.segs);
            }
            items.push_back(it);
            for (const DomNode *sub : nested) List(sub, indent + 2, items);
        }
    }

    void Table(const DomNode *n) {
        std::vector<std::vector<std::vector<Seg>>> rows;
        int header = 0;
        std::vector<Align> aligns;
        std::vector<Seg> caption;
        std::function<void(const DomNode *)> walk = [&](const DomNode *t) {
            for (const auto &c : t->children) {
                if (c->type != DomNodeType::Element) continue;
                if (c->tag == "caption") {
                    caption = InlOf(c.get());
                } else if (c->tag == "thead" || c->tag == "tbody" || c->tag == "tfoot") {
                    walk(c.get());
                } else if (c->tag == "tr") {
                    std::vector<std::vector<Seg>> row;
                    bool all_th = true;
                    size_t col = 0;
                    for (const auto &cell : c->children) {
                        if (cell->type != DomNodeType::Element || (cell->tag != "td" && cell->tag != "th")) continue;
                        if (cell->tag != "th") all_th = false;
                        row.push_back(InlOf(cell.get()));
                        const std::string st = Lower(Attr(cell.get(), "style")) + " " + Lower(Attr(cell.get(), "align"));
                        Align a = st.find("center") != std::string::npos  ? Align::Center
                                  : st.find("right") != std::string::npos ? Align::Right
                                  : st.find("left") != std::string::npos  ? Align::Left
                                                                          : Align::Default;
                        if (aligns.size() <= col) aligns.resize(col + 1, Align::Default);
                        if (a != Align::Default) aligns[col] = a;
                        ++col;
                    }
                    if (all_th && static_cast<int>(rows.size()) == header && !row.empty()) ++header;
                    rows.push_back(row);
                }
            }
        };
        walk(n);
        bool any_align = false;
        for (Align a : aligns) any_align = any_align || a != Align::Default;
        out.Table(rows, header, any_align || header > 0 ? aligns : std::vector<Align>());
        if (!caption.empty()) {
            StripLabel(caption);
            out.Caption(caption);
        }
    }

    void Figure(const DomNode *n) {
        if (HasClass(n, "code")) {
            std::string lang, code;
            std::vector<std::string> results;
            bool has_results = false, html_results = false;
            for (const auto &c : n->children) {
                if (c->type != DomNodeType::Element) continue;
                if (c->tag == "figcaption" && HasClass(c.get(), "lang")) lang = Trim(TextOf(c.get()));
                std::string format;
                if (c->tag == "pre" && HasClass(c.get(), "results")) {
                    has_results = true;
                    results = SplitLines(TextOf(c.get()));
                } else if (c->tag == "div" && HasClass(c.get(), "results-html")) {
                    // HTML the block produced: its markup as written in the
                    // source (see HtmlResultSources), not its text.
                    has_results = true;
                    html_results = true;
                    results = next_html_result < html_result_sources.size() ? SplitLines(html_result_sources[next_html_result++]) : std::vector<std::string>();
                } else if (c->tag == "pre") {
                    code = TextOf(c.get());
                }
            }
            out.Code(lang, html_results ? std::vector<std::pair<std::string, std::string>>{{"results", "html"}} : std::vector<std::pair<std::string, std::string>>{}, code);
            if (has_results) out.Results(results, html_results ? "html" : "");
            return;
        }
        const DomNode *img = nullptr;
        std::vector<Seg> caption;
        std::function<void(const DomNode *)> find = [&](const DomNode *t) {
            for (const auto &c : t->children) {
                if (c->type != DomNodeType::Element) continue;
                if (c->tag == "img" && !img) img = c.get();
                else if (c->tag == "figcaption") caption = InlOf(c.get());
                else find(c.get());
            }
        };
        find(n);
        if (img) {
            out.Image(Attr(img, "src"), Attr(img, "alt"));
        } else {
            Blocks(n);
            return;
        }
        if (!caption.empty()) {
            StripLabel(caption);
            out.Caption(caption);
        }
    }

    std::vector<Seg> pending;  // inline content met at block level
    void Flush() {
        bool any = false;
        for (const Seg &s : pending) any = any || s.kind != Seg::Text || !Trim(s.text).empty();
        if (any) out.Paragraph(pending);
        pending.clear();
    }

    void Blocks(const DomNode *n) {
        for (const auto &cp : n->children) {
            const DomNode *c = cp.get();
            if (c->type == DomNodeType::Text) {
                Inl(c, Fmt(), pending);
                continue;
            }
            if (c->type != DomNodeType::Element) continue;
            const std::string &t = c->tag;
            if (!IsBlockTag(t) && t != "img") {
                Inl(c, Fmt(), pending);
                continue;
            }
            Flush();
            if (t == "head" || t == "script" || t == "style") continue;
            if (t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6') {
                if (HasClass(c, "title")) out.meta.push_back("//? Title: " + Trim(Collapse(TextOf(c))));
                else out.Heading(t[1] - '0', InlOf(c));
            } else if (t == "p" && (HasClass(c, "author") || HasClass(c, "date") || HasClass(c, "subtitle"))) {
                // pandoc's title block
                const std::string key = HasClass(c, "author") ? "Author" : HasClass(c, "date") ? "Date" : "Subtitle";
                out.meta.push_back("//? " + key + ": " + Trim(Collapse(TextOf(c))));
            } else if (t == "p") {
                if (HasClass(c, "caption")) {
                    std::vector<Seg> cap = InlOf(c);
                    StripLabel(cap);
                    out.Caption(cap);
                } else {
                    out.Paragraph(InlOf(c));
                }
            } else if (t == "pre") {
                if (HasClass(c, "results")) {
                    out.Results(SplitLines(TextOf(c)));
                } else {
                    std::string lang;
                    const DomNode *code = Child(c, "code");
                    if (code) {
                        std::istringstream ss(Attr(code, "class"));
                        std::string cls;
                        while (ss >> cls)
                            if (StartsWith(cls, "language-")) lang = cls.substr(9);
                    }
                    out.Code(lang, {}, TextOf(c));
                }
            } else if (t == "ul" || t == "ol") {
                std::vector<Out::Item> items;
                List(c, 0, items);
                out.List(items);
            } else if (t == "table") {
                Table(c);
            } else if (t == "figure") {
                Figure(c);
            } else if (t == "img") {
                out.Image(Attr(c, "src"), Attr(c, "alt"));
            } else if (t == "hr") {
                out.Rule();
            } else if (t == "blockquote") {
                out.Callout("NOTE", InlOf(c));
            } else if (t == "nav" && HasClass(c, "toc")) {
                out.Block("\\toc");
            } else if ((t == "section" || t == "div") && HasClass(c, "abstract")) {
                // mep's own and pandoc's: a title element, then paragraphs.
                std::vector<std::vector<Seg>> paras;
                for (const auto &k : c->children) {
                    if (k->type != DomNodeType::Element || HasClass(k.get(), "abstract-title")) continue;
                    paras.push_back(InlOf(k.get()));
                }
                out.Abstract(paras);
            } else if (t == "section" && HasClass(c, "footnotes")) {
                continue;  // folded into the \fn{} they belong to
            } else if (t == "section" && HasClass(c, "bibliography")) {
                std::function<void(const DomNode *)> entries = [&](const DomNode *s) {
                    for (const auto &k : s->children) {
                        if (k->type != DomNodeType::Element) continue;
                        if (k->tag == "li" && !Attr(k.get(), "data-key").empty()) {
                            std::string fields;
                            for (const auto &kv : k->attrs) {
                                if (!StartsWith(kv.first, "data-bib-")) continue;
                                fields += "\n  " + kv.first.substr(9) + " = {" + GroupEsc(kv.second) + "},";
                            }
                            if (!fields.empty()) fields.pop_back();
                            citations.push_back(CitationEntry(Attr(k.get(), "data-key"), fields));
                        }
                        entries(k.get());
                    }
                };
                entries(c);
                out.Block("\\bibliography");
            } else if (t == "div" && HasClass(c, "callout")) {
                std::string kind = "NOTE";
                std::vector<Seg> body;
                for (const auto &k : c->children) {
                    if (k->type == DomNodeType::Element && HasClass(k.get(), "callout-title")) {
                        kind = Trim(TextOf(k.get()));
                        continue;
                    }
                    Inl(k.get(), Fmt(), body);
                }
                out.Callout(kind, body);
            } else if (t == "div" && HasClass(c, "math-display")) {
                std::string tex;
                std::vector<Seg> cap;
                for (const auto &k : c->children) {
                    if (k->type == DomNodeType::Element && k->tag == "math") tex = TextOf(k.get());
                    else if (k->type == DomNodeType::Element && HasClass(k.get(), "caption")) cap = InlOf(k.get());
                }
                out.Math(tex);
                if (!Attr(c, "aria-label").empty()) out.Attach("\\alttext(" + ParenEsc(Attr(c, "aria-label")) + ")");
                if (!cap.empty()) out.Caption(cap);
            } else if (t == "dl") {
                for (const auto &k : c->children) {
                    if (k->type != DomNodeType::Element) continue;
                    std::vector<Seg> segs = InlOf(k.get());
                    if (k->tag == "dt")
                        for (Seg &s : segs) s.f.b = true;
                    out.Paragraph(segs);
                }
            } else {
                Blocks(c);
            }
            Flush();
        }
        Flush();
    }

    void CollectFootnotes(const DomNode *n) {
        for (const auto &c : n->children) {
            if (c->type != DomNodeType::Element) continue;
            if (c->tag == "li" && StartsWith(Attr(c.get(), "id"), "fn")) {
                std::vector<Seg> segs = InlOf(c.get());
                Coalesce(segs);
                std::string t = Trim(RenderSegs(segs));
                for (char &ch : t)
                    if (ch == '\n') ch = ' ';
                footnotes["#" + Attr(c.get(), "id")] = t;
            }
            CollectFootnotes(c.get());
        }
    }
};

}  // namespace

namespace {

// The markup inside each `<div class="results results-html">` (the HTML a
// code block produced, as mepml's export writes it), in document order:
// read from the source text itself, matching nested divs, so it comes back
// exactly as written rather than re-serialized from the DOM.
std::vector<std::string> HtmlResultSources(const std::string &html) {
    std::vector<std::string> out;
    const std::string lower = Lower(html);
    size_t at = 0;
    while ((at = lower.find("results-html", at)) != std::string::npos) {
        const size_t open = lower.rfind("<div", at);
        const size_t gt = lower.find('>', at);
        if (open == std::string::npos || gt == std::string::npos) break;
        int depth = 1;
        size_t k = gt + 1, end = std::string::npos;
        while (depth > 0) {
            const size_t o = lower.find("<div", k), c = lower.find("</div", k);
            if (c == std::string::npos) break;
            if (o != std::string::npos && o < c) {
                ++depth;
                k = o + 4;
            } else {
                if (--depth == 0) end = c;
                k = c + 5;
            }
        }
        if (end == std::string::npos) break;
        std::string inner = html.substr(gt + 1, end - gt - 1);
        while (!inner.empty() && (inner.front() == '\n' || inner.front() == '\r')) inner.erase(0, 1);
        while (!inner.empty() && (inner.back() == '\n' || inner.back() == '\r' || inner.back() == ' ')) inner.pop_back();
        out.push_back(inner);
        at = end;
    }
    return out;
}

}  // namespace

std::string FromHtml(const std::string &html) {
    HtmlDoc dom;
    ParseHtml(html, dom);
    HtmlReader r;
    r.html_result_sources = HtmlResultSources(html);
    if (!dom.root) return "";
    r.CollectFootnotes(dom.root.get());
    r.Blocks(dom.root.get());
    bool has_title = false;
    for (const std::string &m : r.out.meta) has_title = has_title || StartsWith(m, "//? Title:");
    if (!has_title && !Trim(dom.title).empty()) r.out.meta.insert(r.out.meta.begin(), "//? Title: " + Trim(dom.title));
    for (const std::string &c : r.citations) r.out.blocks.push_back(c);
    return r.out.Str();
}

// ===========================================================================
// Markdown
// ===========================================================================

namespace {

struct MdReader {
    std::map<std::string, std::string> refs;       // [id]: url
    std::map<std::string, std::string> footnotes;  // [^id]: text (raw markdown)
    Out out;

    // --- inline --------------------------------------------------------------
    void Inl(const std::string &s, size_t b, size_t e, Fmt f, std::vector<Seg> &segs, int depth = 0) {
        std::string text;
        auto flush = [&]() {
            if (text.empty()) return;
            Seg sg;
            sg.text = text;
            sg.f = f;
            segs.push_back(sg);
            text.clear();
        };
        auto push = [&](Seg sg) {
            flush();
            segs.push_back(std::move(sg));
        };
        std::vector<std::pair<std::string, Fmt>> tags;  // open inline HTML tags
        size_t i = b;
        while (i < e) {
            const char c = s[i];
            if (c == '\\' && i + 1 < e) {
                if (s[i + 1] == '\n') {
                    Seg br;
                    br.kind = Seg::Break;
                    push(br);
                } else if (std::ispunct(static_cast<unsigned char>(s[i + 1]))) {
                    text += s[i + 1];
                } else {
                    text += "\\";
                    text += s[i + 1];
                }
                i += 2;
                continue;
            }
            if (c == '`') {
                size_t n = 0;
                while (i + n < e && s[i + n] == '`') ++n;
                const size_t close = s.find(std::string(n, '`'), i + n);
                if (close != std::string::npos && close < e) {
                    std::string code = s.substr(i + n, close - i - n);
                    if (code.size() > 1 && code.front() == ' ' && code.back() == ' ') code = code.substr(1, code.size() - 2);
                    for (char &ch : code)
                        if (ch == '\n') ch = ' ';
                    Seg sg;
                    sg.kind = Seg::Code;
                    sg.text = code;
                    sg.f = f;
                    push(sg);
                    i = close + n;
                    continue;
                }
            }
            if (c == '$') {
                const bool disp = i + 1 < e && s[i + 1] == '$';
                const size_t open = disp ? 2 : 1;
                if (i + open < e && !IsSp(s[i + open])) {
                    size_t close = s.find(disp ? "$$" : "$", i + open);
                    while (!disp && close != std::string::npos && close < e && (IsSp(s[close - 1]) || (close + 1 < e && std::isdigit(static_cast<unsigned char>(s[close + 1])))))
                        close = s.find('$', close + 1);
                    if (close != std::string::npos && close < e && close > i + open) {
                        Seg sg;
                        sg.kind = disp ? Seg::DisplayMath : Seg::Math;
                        sg.text = s.substr(i + open, close - i - open);
                        push(sg);
                        i = close + open;
                        continue;
                    }
                }
            }
            if (c == '!' && i + 1 < e && s[i + 1] == '[') {
                size_t tb, te, ub, ue;
                size_t end = ParseLink(s, i + 1, e, &tb, &te, &ub, &ue);
                if (end) {
                    Fmt lf = f;
                    lf.link = ub == std::string::npos ? scratch : LinkUrl(s.substr(ub, ue - ub));
                    Seg sg;
                    sg.text = s.substr(tb, te - tb);
                    if (sg.text.empty()) sg.text = lf.link;
                    sg.f = lf;
                    push(sg);
                    i = end;
                    continue;
                }
            }
            if (c == '[') {
                // [^footnote]
                if (i + 1 < e && s[i + 1] == '^') {
                    const size_t close = s.find(']', i);
                    if (close != std::string::npos && close < e) {
                        const std::string id = s.substr(i + 2, close - i - 2);
                        if (footnotes.count(id) && depth < 4) {
                            std::vector<Seg> inner;
                            const std::string &def = footnotes[id];
                            Inl(def, 0, def.size(), Fmt(), inner, depth + 1);
                            Coalesce(inner);
                            Seg sg;
                            sg.kind = Seg::Footnote;
                            sg.text = Trim(RenderSegs(inner));
                            push(sg);
                            i = close + 1;
                            continue;
                        }
                    }
                }
                // [@key; @key2] -- pandoc citations
                if (i + 1 < e && s[i + 1] == '@') {
                    const size_t close = s.find(']', i);
                    if (close != std::string::npos && close < e) {
                        std::istringstream ss(s.substr(i + 1, close - i - 1));
                        std::string part;
                        bool ok = false;
                        while (std::getline(ss, part, ';')) {
                            part = Trim(part);
                            if (!StartsWith(part, "@")) continue;
                            size_t k = 1;
                            while (k < part.size() && (std::isalnum(static_cast<unsigned char>(part[k])) || std::strchr("_:.-", part[k]))) ++k;
                            Seg sg;
                            sg.kind = Seg::CiteP;
                            sg.text = part.substr(1, k - 1);
                            push(sg);
                            ok = true;
                        }
                        if (ok) {
                            i = close + 1;
                            continue;
                        }
                    }
                }
                size_t tb, te, ub, ue;
                size_t end = ParseLink(s, i, e, &tb, &te, &ub, &ue);
                if (end) {
                    Fmt lf = f;
                    lf.link = ub == std::string::npos ? scratch : LinkUrl(s.substr(ub, ue - ub));
                    flush();
                    Inl(s, tb, te, lf, segs, depth + 1);
                    i = end;
                    continue;
                }
            }
            if (c == '@' && (i == b || IsSp(s[i - 1])) && i + 1 < e && std::isalpha(static_cast<unsigned char>(s[i + 1]))) {
                size_t k = i + 1;
                while (k < e && (std::isalnum(static_cast<unsigned char>(s[k])) || std::strchr("_:-", s[k]))) ++k;
                // pandoc's textual citation, `@key says`; not an email
                // (whose `@` follows a word character).
                if (k < e && s[k] == '.' && k + 1 < e && std::isalnum(static_cast<unsigned char>(s[k + 1]))) {
                    // "@x.y" is more likely a handle or domain
                } else {
                    Seg sg;
                    sg.kind = Seg::Cite;
                    sg.text = s.substr(i + 1, k - i - 1);
                    push(sg);
                    i = k;
                    continue;
                }
            }
            if (c == '<') {
                const size_t close = s.find('>', i);
                if (close != std::string::npos && close < e) {
                    const std::string inner = s.substr(i + 1, close - i - 1);
                    if (StartsWith(inner, "http://") || StartsWith(inner, "https://") || StartsWith(inner, "mailto:")) {
                        Fmt lf = f;
                        lf.link = inner;
                        Seg sg;
                        sg.text = inner;
                        sg.f = lf;
                        push(sg);
                        i = close + 1;
                        continue;
                    }
                    if (!inner.empty() && (std::isalpha(static_cast<unsigned char>(inner[0])) || inner[0] == '/')) {
                        const bool closing = inner[0] == '/';
                        std::string name = Lower(closing ? inner.substr(1) : inner);
                        const size_t sp = name.find_first_of(" /\t");
                        const std::string attrs = sp == std::string::npos ? "" : inner.substr(sp + (closing ? 1 : 0));
                        if (sp != std::string::npos) name = name.substr(0, sp);
                        flush();
                        if (name == "br") {
                            Seg br;
                            br.kind = Seg::Break;
                            push(br);
                        } else if (closing) {
                            for (size_t k = tags.size(); k-- > 0;) {
                                if (tags[k].first == name) {
                                    f = tags[k].second;
                                    tags.resize(k);
                                    break;
                                }
                            }
                        } else {
                            tags.emplace_back(name, f);
                            if (name == "u") f.u = true;
                            else if (name == "sup") f.sup = true;
                            else if (name == "sub") f.sub = true;
                            else if (name == "mark") f.mark = true;
                            else if (name == "ins") f.ins = true;
                            else if (name == "del") f.del = true;
                            else if (name == "s" || name == "strike") f.s = true;
                            else if (name == "small") f.small = true;
                            else if (name == "big") f.big = true;
                            else if (name == "b" || name == "strong") f.b = true;
                            else if (name == "i" || name == "em") f.i = true;
                            else if (name == "code" || name == "kbd") f.mono = true;
                            else if (name == "span") {
                                const size_t st = attrs.find("style=\"");
                                if (st != std::string::npos) {
                                    const std::string style = attrs.substr(st + 7, attrs.find('"', st + 7) - st - 7);
                                    std::istringstream ss(style);
                                    std::string decl;
                                    while (std::getline(ss, decl, ';')) {
                                        const size_t colon = decl.find(':');
                                        if (colon == std::string::npos) continue;
                                        const std::string p = Lower(Trim(decl.substr(0, colon))), v = Trim(decl.substr(colon + 1));
                                        if (p == "color") f.color = v;
                                        else if (p == "font-family") f.font = v;
                                        else if (p == "font-size") {
                                            char buf[16];
                                            std::snprintf(buf, sizeof buf, "%g", std::atof(v.c_str()));
                                            f.size = buf;
                                        }
                                    }
                                }
                            }
                        }
                        i = close + 1;
                        continue;
                    }
                }
            }
            // pandoc's ^superscript^ and ~subscript~: no spaces inside.
            if ((c == '^' || (c == '~' && (i + 1 >= e || s[i + 1] != '~'))) && i + 1 < e && !IsSp(s[i + 1])) {
                size_t k = i + 1;
                while (k < e && s[k] != c && !IsSp(s[k])) k += s[k] == '\\' ? 2 : 1;
                if (k < e && s[k] == c && k > i + 1) {
                    Fmt inner = f;
                    (c == '^' ? inner.sup : inner.sub) = true;
                    flush();
                    Inl(s, i + 1, k, inner, segs, depth + 1);
                    i = k + 1;
                    continue;
                }
            }
            // Emphasis: ***, **, __, *, _, ~~, ==
            if (c == '*' || c == '_' || ((c == '~' || c == '=') && i + 1 < e && s[i + 1] == c)) {
                size_t n = 0;
                while (i + n < e && s[i + n] == c) ++n;
                const char before = i > b ? s[i - 1] : ' ';
                const char after = i + n < e ? s[i + n] : ' ';
                const bool can_open = !IsSp(after) && !(c == '_' && std::isalnum(static_cast<unsigned char>(before)));
                if (can_open) {
                    const size_t want = (c == '~' || c == '=') ? 2 : std::min<size_t>(n, 3);
                    size_t j = i + n;
                    size_t close = std::string::npos;
                    while (j < e) {
                        if (s[j] == '\\') {
                            j += 2;
                            continue;
                        }
                        if (s[j] == '`') {
                            size_t k = s.find('`', j + 1);
                            j = (k == std::string::npos || k >= e) ? j + 1 : k + 1;
                            continue;
                        }
                        if (s[j] == c) {
                            size_t m = 0;
                            while (j + m < e && s[j + m] == c) ++m;
                            const char pb = s[j - 1];
                            const char na = j + m < e ? s[j + m] : ' ';
                            if (m >= want && !IsSp(pb) && !(c == '_' && std::isalnum(static_cast<unsigned char>(na)))) {
                                close = j;
                                break;
                            }
                            j += m;
                            continue;
                        }
                        ++j;
                    }
                    if (close != std::string::npos) {
                        Fmt inner = f;
                        if (c == '~') inner.s = true;
                        else if (c == '=') inner.mark = true;
                        else if (want == 3) inner.b = inner.i = true;
                        else if (want == 2) inner.b = true;
                        else inner.i = true;
                        flush();
                        Inl(s, i + want, close, inner, segs, depth + 1);
                        i = close + want;
                        continue;
                    }
                }
                text += s.substr(i, n);
                i += n;
                continue;
            }
            // Bare URLs.
            if ((c == 'h') && (StartsWith(s.substr(i, 8), "https://") || StartsWith(s.substr(i, 7), "http://")) &&
                (i == b || IsSp(s[i - 1]) || s[i - 1] == '(')) {
                size_t k = i;
                while (k < e && !IsSp(s[k]) && s[k] != '<' && s[k] != ')') ++k;
                while (k > i && std::strchr(".,;:!?", s[k - 1])) --k;
                Fmt lf = f;
                lf.link = s.substr(i, k - i);
                Seg sg;
                sg.text = lf.link;
                sg.f = lf;
                push(sg);
                i = k;
                continue;
            }
            text += c;
            ++i;
        }
        flush();
    }

    static std::string LinkUrl(std::string u) {
        u = Trim(u);
        const size_t sp = u.find_first_of(" \t");
        if (sp != std::string::npos) u = u.substr(0, sp);  // drop a "title"
        if (u.size() > 1 && u.front() == '<' && u.back() == '>') u = u.substr(1, u.size() - 2);
        return u;
    }
    // [text](url) or [text][ref] / [ref]: returns the end index (0 if none).
    size_t ParseLink(const std::string &s, size_t i, size_t e, size_t *tb, size_t *te, size_t *ub, size_t *ue) {
        int depth = 0;
        size_t close = std::string::npos;
        for (size_t k = i; k < e; ++k) {
            if (s[k] == '\\') {
                ++k;
                continue;
            }
            if (s[k] == '[') ++depth;
            if (s[k] == ']' && --depth == 0) {
                close = k;
                break;
            }
        }
        if (close == std::string::npos) return 0;
        *tb = i + 1;
        *te = close;
        if (close + 1 < e && s[close + 1] == '(') {
            int pd = 0;
            for (size_t k = close + 1; k < e; ++k) {
                if (s[k] == '(') ++pd;
                if (s[k] == ')' && --pd == 0) {
                    *ub = close + 2;
                    *ue = k;
                    return k + 1;
                }
            }
            return 0;
        }
        std::string id = s.substr(i + 1, close - i - 1);
        size_t end = close + 1;
        if (close + 1 < e && s[close + 1] == '[') {
            const size_t c2 = s.find(']', close + 1);
            if (c2 != std::string::npos && c2 < e) {
                if (c2 > close + 2) id = s.substr(close + 2, c2 - close - 2);
                end = c2 + 1;
            }
        }
        auto it = refs.find(Lower(id));
        if (it == refs.end()) return 0;
        // A reference link's url lives in `refs`, not in `s`: hand it back
        // through a scratch copy appended to the refs themselves.
        scratch = it->second;
        *ub = *ue = std::string::npos;
        return end;
    }
    std::string scratch;

    std::vector<Seg> InlOf(const std::string &s) {
        std::vector<Seg> segs;
        InlRefAware(s, segs);
        return segs;
    }
    // Wraps Inl so reference links (whose url is not in the text) work:
    // they are rewritten to inline links first.
    void InlRefAware(const std::string &s, std::vector<Seg> &segs) {
        std::string t;
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '[' && (i == 0 || s[i - 1] != '!' || true)) {
                size_t tb, te, ub, ue;
                const size_t end = ParseLink(s, i, s.size(), &tb, &te, &ub, &ue);
                if (end && ub == std::string::npos) {
                    t += "[" + s.substr(tb, te - tb) + "](" + scratch + ")";
                    i = end;
                    continue;
                }
            }
            t += s[i++];
        }
        Inl(t, 0, t.size(), Fmt(), segs);
    }

    // --- blocks ------------------------------------------------------------------
    std::string Read(const std::string &md) {
        std::vector<std::string> lines = SplitLines(md);
        // Front matter.
        size_t start = 0;
        if (!lines.empty() && Trim(lines[0]) == "---") {
            for (size_t k = 1; k < lines.size(); ++k) {
                if (Trim(lines[k]) == "---" || Trim(lines[k]) == "...") {
                    for (size_t m = 1; m < k; ++m) {
                        const size_t colon = lines[m].find(':');
                        if (colon == std::string::npos) continue;
                        std::string key = Trim(lines[m].substr(0, colon)), val = Trim(lines[m].substr(colon + 1));
                        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') && val.back() == val.front())
                            val = val.substr(1, val.size() - 2);
                        if (key.empty()) continue;
                        if (Lower(key) == "title") out.meta.push_back("//? Title: " + val);
                        else if (Lower(key) == "author" || Lower(key) == "date")
                            out.meta.push_back("//? " + std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])))) + Lower(key.substr(1)) + ": " + val);
                        else out.meta.push_back("//? Option: " + key + "=" + (val.find_first_of(" ,") != std::string::npos ? "\"" + val + "\"" : val));
                    }
                    start = k + 1;
                    break;
                }
            }
        }
        // Reference definitions and footnote bodies.
        std::vector<std::string> body;
        for (size_t k = start; k < lines.size(); ++k) {
            const std::string t = Trim(lines[k]);
            if (StartsWith(t, "[^")) {
                const size_t close = t.find("]:");
                if (close != std::string::npos) {
                    std::string text = Trim(t.substr(close + 2));
                    while (k + 1 < lines.size() && (StartsWith(lines[k + 1], "    ") || StartsWith(lines[k + 1], "\t")) )
                        text += " " + Trim(lines[++k]);
                    footnotes[t.substr(2, close - 2)] = text;
                    continue;
                }
            }
            if (StartsWith(t, "[") && !StartsWith(t, "[^")) {
                const size_t close = t.find("]:");
                if (close != std::string::npos && t.find(']') == close) {
                    refs[Lower(t.substr(1, close - 1))] = LinkUrl(t.substr(close + 2));
                    continue;
                }
            }
            body.push_back(lines[k]);
        }
        lines.swap(body);

        std::vector<std::string> para;
        auto flush = [&]() {
            if (para.empty()) return;
            std::string text;
            for (const std::string &p : para) text += (text.empty() ? "" : "\n") + Trim(p);
            para.clear();
            // A paragraph that is one image is a figure.
            const std::string t = Trim(text);
            if (StartsWith(t, "![")) {
                size_t tb, te, ub, ue;
                const size_t end = ParseLink(t, 1, t.size(), &tb, &te, &ub, &ue);
                if (end == t.size() && ub != std::string::npos) {
                    out.Image(LinkUrl(t.substr(ub, ue - ub)), t.substr(tb, te - tb));
                    return;
                }
            }
            // pandoc's table caption: ": caption" or "Table: caption" by the table.
            if (out.last == Out::kTable && (StartsWith(t, ": ") || StartsWith(t, "Table: "))) {
                out.Caption(InlOf(Trim(t.substr(t[0] == ':' ? 2 : 7))));
                return;
            }
            // "*Figure 2: caption*" right under a figure or table.
            if ((out.last == Out::kFigure || out.last == Out::kTable || out.last == Out::kCode) && t.size() > 2 &&
                ((t.front() == '*' && t.back() == '*') || (t.front() == '_' && t.back() == '_'))) {
                std::vector<Seg> segs = InlOf(t.substr(1, t.size() - 2));
                HtmlReader::StripLabel(segs);
                out.Caption(segs);
                return;
            }
            out.Paragraph(InlOf(text));
        };
        auto is_hr = [](const std::string &l) {
            const std::string t = Trim(l);
            if (t.size() < 3 || !std::strchr("-*_", t[0])) return false;
            int n = 0;
            for (char c : t) {
                if (c == t[0]) ++n;
                else if (!IsSp(c)) return false;
            }
            return n >= 3;
        };
        auto list_marker = [](const std::string &l, int *indent, bool *ordered, int *number, size_t *content) {
            size_t k = 0;
            while (k < l.size() && l[k] == ' ') ++k;
            *indent = static_cast<int>(k);
            if (k < l.size() && std::strchr("-*+", l[k]) && k + 1 < l.size() && l[k + 1] == ' ') {
                *ordered = false;
                *content = k + 2;
                return true;
            }
            size_t d = k;
            while (d < l.size() && std::isdigit(static_cast<unsigned char>(l[d]))) ++d;
            if (d > k && d - k < 10 && d < l.size() && (l[d] == '.' || l[d] == ')') && d + 1 < l.size() && l[d + 1] == ' ') {
                *ordered = true;
                *number = std::atoi(l.substr(k, d - k).c_str());
                *content = d + 2;
                return true;
            }
            return false;
        };
        auto is_table_sep = [](const std::string &l) {
            const std::string t = Trim(l);
            if (t.find('-') == std::string::npos || t.find('|') == std::string::npos) return false;
            for (char c : t)
                if (!std::strchr("|-: \t", c)) return false;
            return true;
        };
        auto split_row = [](const std::string &l) {
            std::string t = Trim(l);
            if (!t.empty() && t.front() == '|') t = t.substr(1);
            if (!t.empty() && t.back() == '|' && (t.size() < 2 || t[t.size() - 2] != '\\')) t.pop_back();
            std::vector<std::string> cells;
            std::string cur;
            bool tick = false;
            for (size_t k = 0; k < t.size(); ++k) {
                if (t[k] == '\\' && k + 1 < t.size() && t[k + 1] == '|') {
                    cur += '|';
                    ++k;
                    continue;
                }
                if (t[k] == '`') tick = !tick;
                if (t[k] == '|' && !tick) {
                    cells.push_back(Trim(cur));
                    cur.clear();
                } else {
                    cur += t[k];
                }
            }
            cells.push_back(Trim(cur));
            return cells;
        };

        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string &l = lines[i];
            const std::string t = Trim(l);
            if (t.empty()) {
                flush();
                continue;
            }
            // Fenced code.
            const size_t ind = l.find_first_not_of(' ');
            if (ind <= 3 && (StartsWith(t, "```") || StartsWith(t, "~~~"))) {
                flush();
                const char fc = t[0];
                size_t n = 0;
                while (n < t.size() && t[n] == fc) ++n;
                std::string info = Trim(t.substr(n));
                std::string code;
                size_t k = i + 1;
                for (; k < lines.size(); ++k) {
                    const std::string tk = Trim(lines[k]);
                    size_t m = 0;
                    while (m < tk.size() && tk[m] == fc) ++m;
                    if (m >= n && tk.find_first_not_of(fc) == std::string::npos) break;
                    code += lines[k] + "\n";
                }
                i = k;
                // Info: `lang`, `lang {k=v}`, `{.lang k=v}`, `{lang, k=v}`.
                std::string lang;
                std::vector<std::pair<std::string, std::string>> opts;
                std::string attrs;
                const size_t brace = info.find('{');
                if (brace != std::string::npos) {
                    lang = Trim(info.substr(0, brace));
                    attrs = info.substr(brace + 1, info.rfind('}') == std::string::npos ? std::string::npos : info.rfind('}') - brace - 1);
                } else {
                    lang = info.substr(0, info.find(' '));
                }
                if (!attrs.empty()) {
                    std::string cur;
                    std::vector<std::string> parts;
                    char q = 0;
                    for (char c : attrs) {
                        if (q) {
                            cur += c;
                            if (c == q) q = 0;
                        } else if (c == '"' || c == '\'') {
                            q = c;
                            cur += c;
                        } else if (c == ',' || c == ' ') {
                            if (!Trim(cur).empty()) parts.push_back(Trim(cur));
                            cur.clear();
                        } else {
                            cur += c;
                        }
                    }
                    if (!Trim(cur).empty()) parts.push_back(Trim(cur));
                    for (const std::string &p : parts) {
                        const size_t eq = p.find('=');
                        if (eq == std::string::npos) {
                            if (lang.empty()) lang = p[0] == '.' ? p.substr(1) : p;
                            continue;
                        }
                        std::string v = p.substr(eq + 1);
                        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
                        opts.emplace_back(p.substr(0, eq), v);
                    }
                }
                if (Lower(lang) == "output" && out.last == Out::kCode) out.Results(SplitLines(code));
                else out.Code(lang, opts, code);
                continue;
            }
            // Display maths.
            if (StartsWith(t, "$$")) {
                flush();
                if (t.size() > 4 && t.compare(t.size() - 2, 2, "$$") == 0) {
                    out.Math(t.substr(2, t.size() - 4));
                    continue;
                }
                std::string tex = t.substr(2);
                size_t k = i + 1;
                for (; k < lines.size(); ++k) {
                    const size_t close = lines[k].find("$$");
                    if (close != std::string::npos) {
                        tex += "\n" + lines[k].substr(0, close);
                        break;
                    }
                    tex += "\n" + lines[k];
                }
                i = k;
                out.Math(tex);
                continue;
            }
            // ATX headings.
            if (ind <= 3 && t[0] == '#') {
                size_t n = 0;
                while (n < t.size() && t[n] == '#') ++n;
                if (n <= 6 && (n == t.size() || t[n] == ' ')) {
                    flush();
                    std::string title = Trim(t.substr(n));
                    while (!title.empty() && title.back() == '#') title.pop_back();
                    out.Heading(static_cast<int>(n), InlOf(Trim(title)));
                    continue;
                }
            }
            // Setext headings.
            if (!para.empty() && ind <= 3 && (t.find_first_not_of('=') == std::string::npos ||
                                               (t.find_first_not_of('-') == std::string::npos && t.size() >= 2))) {
                std::string title;
                for (const std::string &p : para) title += (title.empty() ? "" : " ") + Trim(p);
                para.clear();
                out.Heading(t[0] == '=' ? 1 : 2, InlOf(title));
                continue;
            }
            if (is_hr(l)) {
                flush();
                out.Rule();
                continue;
            }
            // Block quotes / GitHub alerts.
            if (t[0] == '>') {
                flush();
                std::string text;
                size_t k = i;
                for (; k < lines.size() && StartsWith(Trim(lines[k]), ">"); ++k) {
                    std::string q = Trim(lines[k]).substr(1);
                    if (!q.empty() && q[0] == ' ') q = q.substr(1);
                    text += (text.empty() ? "" : "\n") + q;
                }
                i = k - 1;
                std::string kind = "NOTE";
                if (StartsWith(Trim(text), "[!")) {
                    const std::string tt = Trim(text);
                    const size_t close = tt.find(']');
                    kind = Upper(tt.substr(2, close - 2));
                    text = tt.substr(close + 1);
                }
                out.Callout(kind, InlOf(Trim(text)));
                continue;
            }
            // HTML comments.
            if (StartsWith(t, "<!--")) {
                flush();
                std::string text = t.substr(4);
                size_t k = i;
                while (text.find("-->") == std::string::npos && k + 1 < lines.size()) text += "\n" + lines[++k];
                i = k;
                text = Trim(text.substr(0, text.find("-->")));
                // An html result (see MdWriter): the raw HTML up to the
                // closing marker is the code block's results.
                if (text == "mepml:results html") {
                    std::vector<std::string> raw;
                    size_t n = i + 1;
                    for (; n < lines.size() && Trim(lines[n]) != "<!-- /mepml:results -->"; ++n) raw.push_back(lines[n]);
                    i = n;
                    out.Results(raw, "html");
                    continue;
                }
                // An abstract (see MdWriter): its paragraphs up to the
                // closing marker, less the bold "Abstract" line.
                if (text == "mepml:abstract") {
                    std::vector<std::vector<Seg>> paras;
                    std::string cur;
                    size_t n = i + 1;
                    for (; n < lines.size() && Trim(lines[n]) != "<!-- /mepml:abstract -->"; ++n) {
                        const std::string lt = Trim(lines[n]);
                        if (lt == "**Abstract**") continue;
                        if (lt.empty()) {
                            if (!cur.empty()) paras.push_back(InlOf(cur));
                            cur.clear();
                        } else {
                            cur += (cur.empty() ? "" : "\n") + lt;
                        }
                    }
                    if (!cur.empty()) paras.push_back(InlOf(cur));
                    i = n;
                    out.Abstract(paras);
                    continue;
                }
                // mep's own markers (see MdWriter): \toc / \bibliography
                // stand for the heading and list that follow them.
                if (text == "mepml:toc" || text == "mepml:bibliography") {
                    out.Block(text == "mepml:toc" ? "\\toc" : "\\bibliography");
                    size_t n = i + 1;
                    while (n < lines.size() && Trim(lines[n]).empty()) ++n;
                    if (n < lines.size() && StartsWith(Trim(lines[n]), "#")) ++n;
                    while (n < lines.size() && Trim(lines[n]).empty()) ++n;
                    while (n < lines.size() && !Trim(lines[n]).empty()) ++n;
                    i = n - 1;
                    continue;
                }
                if (StartsWith(text, "mepml\n")) {
                    out.Block(Trim(text.substr(6)));
                    continue;
                }
                out.Comment(text);
                continue;
            }
            // Raw HTML blocks.
            if (para.empty() && ind <= 3 && t[0] == '<' && t.size() > 1 && std::isalpha(static_cast<unsigned char>(t[1]))) {
                std::string html;
                size_t k = i;
                for (; k < lines.size() && !Trim(lines[k]).empty(); ++k) html += lines[k] + "\n";
                i = k - 1;
                const std::string converted = FromHtml(html);
                for (const std::string &blk : [&] {
                         std::vector<std::string> v;
                         std::string cur;
                         for (const std::string &cl : SplitLines(converted)) {
                             if (Trim(cl).empty()) {
                                 if (!cur.empty()) v.push_back(cur);
                                 cur.clear();
                             } else {
                                 cur += (cur.empty() ? "" : "\n") + cl;
                             }
                         }
                         if (!cur.empty()) v.push_back(cur);
                         return v;
                     }())
                    out.Block(blk);
                continue;
            }
            // Tables.
            if (t.find('|') != std::string::npos && i + 1 < lines.size() && is_table_sep(lines[i + 1])) {
                flush();
                std::vector<std::vector<std::vector<Seg>>> rows;
                std::vector<Align> aligns;
                for (const std::string &c : split_row(lines[i + 1])) {
                    const bool left = !c.empty() && c.front() == ':', right = !c.empty() && c.back() == ':';
                    aligns.push_back(left && right ? Align::Center : right ? Align::Right : left ? Align::Left : Align::Default);
                }
                std::vector<std::vector<Seg>> head;
                for (const std::string &c : split_row(l)) head.push_back(InlOf(c));
                rows.push_back(head);
                size_t k = i + 2;
                for (; k < lines.size() && !Trim(lines[k]).empty() && lines[k].find('|') != std::string::npos; ++k) {
                    std::vector<std::vector<Seg>> row;
                    for (const std::string &c : split_row(lines[k])) row.push_back(InlOf(c));
                    rows.push_back(row);
                }
                i = k - 1;
                out.Table(rows, 1, aligns);
                continue;
            }
            // Lists.
            int indent = 0, number = 1;
            bool ordered = false;
            size_t content = 0;
            if (para.empty() && list_marker(l, &indent, &ordered, &number, &content)) {
                std::vector<Out::Item> items;
                size_t k = i;
                while (k < lines.size()) {
                    int ind2 = 0, num2 = 1;
                    bool ord2 = false;
                    size_t cont2 = 0;
                    if (Trim(lines[k]).empty()) {
                        // A blank line inside a list continues it only
                        // when the next line is another item.
                        if (k + 1 < lines.size() && list_marker(lines[k + 1], &ind2, &ord2, &num2, &cont2)) {
                            ++k;
                            continue;
                        }
                        break;
                    }
                    if (!list_marker(lines[k], &ind2, &ord2, &num2, &cont2)) {
                        if (items.empty() || lines[k].find_first_not_of(' ') < 2) break;
                        std::vector<Seg> more = InlOf(" " + Trim(lines[k]));
                        items.back().segs.insert(items.back().segs.end(), more.begin(), more.end());
                        ++k;
                        continue;
                    }
                    Out::Item it;
                    it.indent = ind2 / 2 * 2;
                    it.ordered = ord2;
                    it.number = num2;
                    std::string text = lines[k].substr(cont2);
                    if (StartsWith(text, "[ ] ") || StartsWith(text, "[x] ") || StartsWith(text, "[X] ")) {
                        it.checkbox = text[1] == ' ' ? 0 : 1;
                        text = text.substr(4);
                    }
                    it.segs = InlOf(text);
                    items.push_back(it);
                    ++k;
                }
                i = k - 1;
                out.List(items);
                continue;
            }
            // Indented code (outside a paragraph).
            if (para.empty() && StartsWith(l, "    ")) {
                std::string code;
                size_t k = i;
                for (; k < lines.size() && (StartsWith(lines[k], "    ") || Trim(lines[k]).empty()); ++k) code += (lines[k].size() >= 4 ? lines[k].substr(4) : "") + "\n";
                i = k - 1;
                out.Code("", {}, code);
                continue;
            }
            para.push_back(l);
        }
        flush();
        return out.Str();
    }
};

}  // namespace

std::string FromMarkdown(const std::string &md) {
    MdReader r;
    return r.Read(md);
}

// ===========================================================================
// Org
// ===========================================================================

namespace {

struct OrgReader {
    std::map<std::string, std::string> footnotes;
    Out out;
    std::vector<Seg> pending_caption;
    bool have_caption = false;

    static bool OrgPre(char c) { return c == 0 || IsSp(c) || std::strchr("-('\"{", c) != nullptr; }
    static bool OrgPost(char c) { return c == 0 || IsSp(c) || std::strchr("-.,:!?;'\")}[", c) != nullptr; }

    void Inl(const std::string &s, size_t b, size_t e, Fmt f, std::vector<Seg> &segs, int depth = 0) {
        std::string text;
        auto flush = [&]() {
            if (text.empty()) return;
            Seg sg;
            sg.text = text;
            sg.f = f;
            segs.push_back(sg);
            text.clear();
        };
        auto push = [&](Seg sg) {
            flush();
            segs.push_back(std::move(sg));
        };
        size_t i = b;
        while (i < e) {
            const char c = s[i];
            if (c == '[' && i + 1 < e && s[i + 1] == '[') {
                const size_t close = s.find("]]", i);
                if (close != std::string::npos && close < e) {
                    const std::string inner = s.substr(i + 2, close - i - 2);
                    const size_t mid = inner.find("][");
                    std::string target = mid == std::string::npos ? inner : inner.substr(0, mid);
                    std::string desc = mid == std::string::npos ? "" : inner.substr(mid + 2);
                    if (StartsWith(target, "file:")) target = target.substr(5);
                    Fmt lf = f;
                    lf.link = target;
                    flush();
                    if (desc.empty()) {
                        Seg sg;
                        sg.text = target;
                        sg.f = lf;
                        segs.push_back(sg);
                    } else {
                        Inl(desc, 0, desc.size(), lf, segs, depth + 1);
                    }
                    i = close + 2;
                    continue;
                }
            }
            // x^{sup} / x_{sub} (the braced form; bare x^2 is left as text).
            if ((c == '^' || c == '_') && i + 1 < e && s[i + 1] == '{') {
                const size_t close = s.find('}', i + 2);
                if (close != std::string::npos && close < e && close > i + 2) {
                    Fmt sf = f;
                    (c == '^' ? sf.sup : sf.sub) = true;
                    flush();
                    Inl(s, i + 2, close, sf, segs, depth + 1);
                    i = close + 1;
                    continue;
                }
            }
            if (c == '[' && StartsWith(s.substr(i, 4), "[fn:")) {
                const size_t close = s.find(']', i);
                if (close != std::string::npos && close < e) {
                    const std::string inner = s.substr(i + 4, close - i - 4);
                    std::string body;
                    if (!inner.empty() && inner[0] == ':') body = inner.substr(1);  // [fn::inline note]
                    else if (inner.find(':') != std::string::npos) body = inner.substr(inner.find(':') + 1);
                    else if (footnotes.count(inner)) body = footnotes[inner];
                    if (!body.empty() && depth < 4) {
                        std::vector<Seg> fn;
                        Inl(body, 0, body.size(), Fmt(), fn, depth + 1);
                        Coalesce(fn);
                        Seg sg;
                        sg.kind = Seg::Footnote;
                        sg.text = Trim(RenderSegs(fn));
                        push(sg);
                        i = close + 1;
                        continue;
                    }
                }
            }
            if (c == '[' && StartsWith(s.substr(i, 6), "[cite")) {
                const size_t colon = s.find(':', i);
                const size_t close = s.find(']', i);
                if (colon != std::string::npos && close != std::string::npos && colon < close && close < e) {
                    const bool textual = s.substr(i, colon - i).find("/t") != std::string::npos;
                    std::istringstream ss(s.substr(colon + 1, close - colon - 1));
                    std::string part;
                    while (std::getline(ss, part, ';')) {
                        part = Trim(part);
                        if (!StartsWith(part, "@")) continue;
                        Seg sg;
                        sg.kind = textual ? Seg::Cite : Seg::CiteP;
                        sg.text = part.substr(1);
                        push(sg);
                    }
                    i = close + 1;
                    continue;
                }
            }
            if (c == '\\' && i + 1 < e && (s[i + 1] == '(' || s[i + 1] == '[')) {
                const std::string closer = s[i + 1] == '(' ? "\\)" : "\\]";
                const size_t close = s.find(closer, i + 2);
                if (close != std::string::npos && close < e) {
                    Seg sg;
                    sg.kind = s[i + 1] == '(' ? Seg::Math : Seg::DisplayMath;
                    sg.text = s.substr(i + 2, close - i - 2);
                    push(sg);
                    i = close + 2;
                    continue;
                }
            }
            if (c == '\\' && i + 1 < e && s[i + 1] == '\\') {
                Seg br;
                br.kind = Seg::Break;
                push(br);
                i += 2;
                continue;
            }
            if (c == '$' && i + 1 < e && !IsSp(s[i + 1]) && (i == b || !std::isalnum(static_cast<unsigned char>(s[i - 1])))) {
                const bool disp = s[i + 1] == '$';
                const size_t open = disp ? 2 : 1;
                const size_t close = s.find(disp ? "$$" : "$", i + open);
                if (close != std::string::npos && close < e && close > i + open && !IsSp(s[close - 1])) {
                    Seg sg;
                    sg.kind = disp ? Seg::DisplayMath : Seg::Math;
                    sg.text = s.substr(i + open, close - i - open);
                    push(sg);
                    i = close + open;
                    continue;
                }
            }
            if ((c == '^' || c == '_') && i + 1 < e && s[i + 1] == '{' && i > b && !IsSp(s[i - 1])) {
                const size_t close = s.find('}', i + 2);
                if (close != std::string::npos && close < e) {
                    Fmt inner = f;
                    (c == '^' ? inner.sup : inner.sub) = true;
                    flush();
                    Inl(s, i + 2, close, inner, segs, depth + 1);
                    i = close + 1;
                    continue;
                }
            }
            if (std::strchr("*/_+=~", c) && OrgPre(i > b ? s[i - 1] : 0) && i + 1 < e && !IsSp(s[i + 1]) && s[i + 1] != c) {
                size_t close = std::string::npos;
                for (size_t j = i + 1; j < e; ++j) {
                    if (s[j] == c && !IsSp(s[j - 1]) && j > i + 1 && OrgPost(j + 1 < e ? s[j + 1] : 0)) {
                        close = j;
                        break;
                    }
                }
                if (close != std::string::npos) {
                    if (c == '=' || c == '~') {
                        Seg sg;
                        sg.kind = Seg::Code;
                        sg.text = s.substr(i + 1, close - i - 1);
                        sg.f = f;
                        push(sg);
                    } else {
                        Fmt inner = f;
                        if (c == '*') inner.b = true;
                        else if (c == '/') inner.i = true;
                        else if (c == '_') inner.u = true;
                        else inner.s = true;
                        flush();
                        Inl(s, i + 1, close, inner, segs, depth + 1);
                    }
                    i = close + 1;
                    continue;
                }
            }
            text += c;
            ++i;
        }
        flush();
    }
    std::vector<Seg> InlOf(const std::string &s) {
        std::vector<Seg> segs;
        Inl(s, 0, s.size(), Fmt(), segs);
        return segs;
    }

    static bool IsImagePath(const std::string &p) {
        const std::string l = Lower(p);
        for (const char *ext : {".png", ".jpg", ".jpeg", ".gif", ".svg", ".bmp", ".webp"})
            if (l.size() > std::strlen(ext) && l.compare(l.size() - std::strlen(ext), std::strlen(ext), ext) == 0) return true;
        return false;
    }

    void TakeCaption() {
        if (!have_caption) return;
        out.Caption(pending_caption);
        pending_caption.clear();
        have_caption = false;
    }

    std::string Read(const std::string &org) {
        std::vector<std::string> lines = SplitLines(org);
        // Footnote definitions.
        std::vector<std::string> body;
        for (size_t k = 0; k < lines.size(); ++k) {
            if (StartsWith(lines[k], "[fn:")) {
                const size_t close = lines[k].find(']');
                if (close != std::string::npos) {
                    const std::string name = lines[k].substr(4, close - 4);
                    std::string text = Trim(lines[k].substr(close + 1));
                    while (k + 1 < lines.size() && !Trim(lines[k + 1]).empty() && !StartsWith(lines[k + 1], "[fn:") &&
                           !StartsWith(lines[k + 1], "*"))
                        text += " " + Trim(lines[++k]);
                    footnotes[name] = text;
                    continue;
                }
            }
            body.push_back(lines[k]);
        }
        lines.swap(body);

        std::vector<std::string> para;
        auto flush = [&]() {
            if (para.empty()) return;
            std::string text;
            for (const std::string &p : para) text += (text.empty() ? "" : "\n") + Trim(p);
            para.clear();
            const std::string t = Trim(text);
            if (StartsWith(t, "[[") && t.size() > 4 && t.compare(t.size() - 2, 2, "]]") == 0 && t.find("]]") == t.size() - 2) {
                std::string target = t.substr(2, t.size() - 4);
                const size_t mid = target.find("][");
                std::string desc = mid == std::string::npos ? "" : target.substr(mid + 2);
                if (mid != std::string::npos) target = target.substr(0, mid);
                if (StartsWith(target, "file:")) target = target.substr(5);
                if (IsImagePath(target)) {
                    out.Image(target, desc);
                    TakeCaption();
                    return;
                }
            }
            out.Paragraph(InlOf(text));
        };
        auto key_of = [](const std::string &t, std::string *value) -> std::string {
            if (!StartsWith(t, "#+")) return "";
            const size_t colon = t.find(':');
            const size_t sp = t.find(' ');
            if (colon == std::string::npos || (sp != std::string::npos && sp < colon)) {
                const std::string word = t.substr(2, sp == std::string::npos ? std::string::npos : sp - 2);
                *value = sp == std::string::npos ? "" : Trim(t.substr(sp));
                return Lower(word);
            }
            *value = Trim(t.substr(colon + 1));
            return Lower(t.substr(2, colon - 2));
        };

        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string &l = lines[i];
            const std::string t = Trim(l);
            if (t.empty()) {
                flush();
                continue;
            }
            std::string value;
            const std::string key = key_of(t, &value);
            if (key == "title") {
                flush();
                out.meta.push_back("//? Title: " + value);
                continue;
            }
            if (key == "author" || key == "date" || key == "email" || key == "subtitle") {
                flush();
                out.meta.push_back("//? " + std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])))) + key.substr(1) + ": " + value);
                continue;
            }
            if (key == "caption") {
                flush();
                pending_caption = InlOf(value);
                have_caption = true;
                continue;
            }
            if (key == "toc") {
                flush();
                out.Block("\\toc");
                continue;
            }
            if (key == "print_bibliography") {
                flush();
                out.Block("\\bibliography");
                continue;
            }
            if (key == "results") {
                flush();
                std::vector<std::string> res;
                size_t k = i + 1;
                std::string image, format;
                for (; k < lines.size(); ++k) {
                    const std::string tk = Trim(lines[k]);
                    // `:results html`: the HTML itself, in an export block.
                    if (res.empty() && format.empty() && Lower(tk) == "#+begin_export html") {
                        format = "html";
                        for (++k; k < lines.size() && Lower(Trim(lines[k])).rfind("#+end_export", 0) != 0; ++k) res.push_back(lines[k]);
                        ++k;
                        break;
                    }
                    if (StartsWith(tk, ": ") || tk == ":") {
                        res.push_back(tk.size() > 2 ? tk.substr(2) : "");
                    } else if (StartsWith(Lower(tk), "#+begin_example")) {
                        for (++k; k < lines.size() && !StartsWith(Lower(Trim(lines[k])), "#+end_example"); ++k) res.push_back(lines[k]);
                    } else if (StartsWith(tk, "[[") && res.empty() && image.empty()) {
                        std::string target = tk.substr(2, tk.find(']') - 2);
                        if (StartsWith(target, "file:")) target = target.substr(5);
                        image = target;
                    } else {
                        break;
                    }
                }
                i = k - 1;
                if (!image.empty()) res.push_back("\\image(" + ParenEsc(image) + ")");
                out.Results(res, format);
                TakeCaption();
                continue;
            }
            if (key.rfind("begin_", 0) == 0) {
                flush();
                const std::string kind = key.substr(6);
                const std::string end = "#+end_" + kind;
                std::vector<std::string> blk;
                size_t k = i + 1;
                for (; k < lines.size() && Lower(Trim(lines[k])).rfind(end, 0) != 0; ++k) blk.push_back(lines[k]);
                i = k;
                if (kind == "comment" && !blk.empty() && Trim(blk[0]) == "mepml") {
                    // mep's own entries (see OrgWriter): mepml source.
                    std::string src;
                    for (size_t b = 1; b < blk.size(); ++b) src += (b > 1 ? "\n" : "") + blk[b];
                    out.Block(src);
                    continue;
                }
                if (kind == "src") {
                    std::istringstream ss(value);
                    std::string lang;
                    ss >> lang;
                    std::vector<std::pair<std::string, std::string>> opts;
                    std::string rest;
                    std::getline(ss, rest);
                    // :key value :key2 value two
                    size_t p = rest.find(':');
                    while (p != std::string::npos) {
                        size_t sp = rest.find(' ', p);
                        const std::string k2 = rest.substr(p + 1, sp == std::string::npos ? std::string::npos : sp - p - 1);
                        size_t nxt = rest.find(" :", p + 1);
                        const std::string v = sp == std::string::npos || (nxt != std::string::npos && sp > nxt) ? "" : Trim(rest.substr(sp, nxt == std::string::npos ? std::string::npos : nxt - sp));
                        if (!k2.empty()) opts.emplace_back(k2, v);
                        p = nxt == std::string::npos ? std::string::npos : nxt + 1;
                    }
                    std::string code;
                    for (const std::string &b : blk) code += (StartsWith(Trim(b), ",*") || StartsWith(Trim(b), ",#+") ? b.substr(0, b.find(',')) + b.substr(b.find(',') + 1) : b) + "\n";
                    out.Code(lang, opts, code);
                    TakeCaption();
                } else if (kind == "example" || kind == "verse") {
                    std::string code;
                    for (const std::string &b : blk) code += b + "\n";
                    out.Code("", {}, code);
                } else if (kind == "export" || kind == "comment") {
                    // not document text
                } else if (kind == "abstract") {
                    std::vector<std::vector<Seg>> paras;
                    std::string text;
                    for (const std::string &b : blk) {
                        if (Trim(b).empty()) {
                            if (!text.empty()) paras.push_back(InlOf(text));
                            text.clear();
                        } else {
                            text += (text.empty() ? "" : "\n") + Trim(b);
                        }
                    }
                    if (!text.empty()) paras.push_back(InlOf(text));
                    out.Abstract(paras);
                } else if (kind == "center") {
                    for (const std::string &b : blk) para.push_back(b);
                    flush();
                } else {
                    std::string text;
                    for (const std::string &b : blk) text += (text.empty() ? "" : "\n") + Trim(b);
                    out.Callout(kind == "quote" ? "NOTE" : kind, InlOf(text));
                }
                continue;
            }
            if (!key.empty()) {
                flush();
                if (key != "options" && key != "startup" && key != "property" && key != "setupfile" && key != "name" &&
                    key.rfind("attr_", 0) != 0 && key != "latex_header" && key != "html_head" && key != "bibliography" &&
                    key != "cite_export" && key != "language" && key != "todo" && key != "tblfm")
                    out.meta.push_back("//? " + key + ": " + value);
                continue;
            }
            // Drawers and planning lines.
            if (t == ":PROPERTIES:" || t == ":LOGBOOK:" || (t.size() > 2 && t.front() == ':' && t.back() == ':' && t.find(' ') == std::string::npos && Upper(t) == t)) {
                flush();
                for (size_t k = i + 1; k < lines.size(); ++k)
                    if (Trim(lines[k]) == ":END:") {
                        i = k;
                        break;
                    }
                continue;
            }
            if (StartsWith(t, "SCHEDULED:") || StartsWith(t, "DEADLINE:") || StartsWith(t, "CLOSED:")) continue;
            // Comments.
            if (t == "#" || StartsWith(t, "# ")) {
                flush();
                std::string text;
                size_t k = i;
                for (; k < lines.size() && (Trim(lines[k]) == "#" || StartsWith(Trim(lines[k]), "# ")); ++k)
                    text += (text.empty() ? "" : "\n") + (Trim(lines[k]).size() > 2 ? Trim(lines[k]).substr(2) : "");
                i = k - 1;
                // mep's own marker (see OrgWriter): the References headline
                // and list that follow are \bibliography.
                if (text == "mepml:bibliography") {
                    out.Block("\\bibliography");
                    size_t n = k;
                    if (n < lines.size() && StartsWith(lines[n], "* ")) ++n;
                    while (n < lines.size() && !Trim(lines[n]).empty()) ++n;
                    i = n - 1;
                    continue;
                }
                out.Comment(text);
                continue;
            }
            // Headlines.
            if (l[0] == '*') {
                size_t n = 0;
                while (n < l.size() && l[n] == '*') ++n;
                if (n < l.size() && l[n] == ' ') {
                    flush();
                    std::string title = Trim(l.substr(n));
                    for (const char *kw : {"TODO ", "DONE ", "NEXT ", "WAITING ", "CANCELLED "})
                        if (StartsWith(title, kw)) title = title.substr(std::strlen(kw));
                    if (StartsWith(title, "[#") && title.size() > 4 && title[3] == ']') title = Trim(title.substr(4));
                    // Trailing :tags:
                    const size_t sp = title.find_last_of(' ');
                    if (sp != std::string::npos && title.size() > sp + 2 && title[sp + 1] == ':' && title.back() == ':')
                        title = Trim(title.substr(0, sp));
                    if (title == "Footnotes") continue;
                    out.Heading(static_cast<int>(n), InlOf(title));
                    continue;
                }
            }
            // Tables.
            if (t[0] == '|') {
                flush();
                std::vector<std::vector<std::vector<Seg>>> rows;
                int header = 0;
                size_t k = i;
                for (; k < lines.size() && StartsWith(Trim(lines[k]), "|"); ++k) {
                    const std::string row = Trim(lines[k]);
                    if (StartsWith(row, "|-")) {
                        if (header == 0 && !rows.empty()) header = static_cast<int>(rows.size());
                        continue;
                    }
                    std::string inner = row.substr(1);
                    if (!inner.empty() && inner.back() == '|') inner.pop_back();
                    std::vector<std::vector<Seg>> cells;
                    std::istringstream ss(inner);
                    std::string cell;
                    while (std::getline(ss, cell, '|')) cells.push_back(InlOf(Trim(cell)));
                    rows.push_back(cells);
                }
                i = k - 1;
                out.Table(rows, header, {});
                TakeCaption();
                continue;
            }
            // Display maths.
            if (StartsWith(t, "\\[") || StartsWith(t, "$$") || StartsWith(t, "\\begin{")) {
                flush();
                std::string tex;
                const bool env = StartsWith(t, "\\begin{");
                const std::string closer = StartsWith(t, "\\[") ? "\\]" : "$$";
                std::string first = env ? t : t.substr(2);
                size_t k = i;
                if (env) {
                    const std::string name = t.substr(7, t.find('}') - 7);
                    const std::string end = "\\end{" + name + "}";
                    tex = t;
                    while (tex.find(end) == std::string::npos && k + 1 < lines.size()) tex += "\n" + lines[++k];
                } else if (first.find(closer) != std::string::npos) {
                    tex = first.substr(0, first.find(closer));
                } else {
                    tex = first;
                    while (k + 1 < lines.size()) {
                        const std::string &n = lines[++k];
                        const size_t c = n.find(closer);
                        if (c != std::string::npos) {
                            tex += "\n" + n.substr(0, c);
                            break;
                        }
                        tex += "\n" + n;
                    }
                }
                i = k;
                out.Math(tex);
                TakeCaption();
                continue;
            }
            // Fixed-width lines.
            if (StartsWith(t, ": ") || t == ":") {
                flush();
                std::string code;
                size_t k = i;
                for (; k < lines.size() && (StartsWith(Trim(lines[k]), ": ") || Trim(lines[k]) == ":"); ++k)
                    code += (Trim(lines[k]).size() > 2 ? Trim(lines[k]).substr(2) : "") + "\n";
                i = k - 1;
                out.Code("", {}, code);
                continue;
            }
            if (t.size() >= 5 && t.find_first_not_of('-') == std::string::npos) {
                flush();
                out.Rule();
                continue;
            }
            // Lists (a `*` bullet needs indentation, or it is a headline).
            {
                size_t k0 = l.find_first_not_of(' ');
                const bool bullet = k0 < l.size() && (l[k0] == '-' || l[k0] == '+' || (l[k0] == '*' && k0 > 0)) && k0 + 1 < l.size() && l[k0 + 1] == ' ';
                size_t d = k0;
                while (d < l.size() && std::isdigit(static_cast<unsigned char>(l[d]))) ++d;
                const bool numbered = d > k0 && d < l.size() && (l[d] == '.' || l[d] == ')') && d + 1 < l.size() && l[d + 1] == ' ';
                if (para.empty() && (bullet || numbered)) {
                    std::vector<Out::Item> items;
                    size_t k = i;
                    while (k < lines.size() && !Trim(lines[k]).empty()) {
                        const std::string &lk = lines[k];
                        const size_t kk = lk.find_first_not_of(' ');
                        size_t dd = kk;
                        while (dd < lk.size() && std::isdigit(static_cast<unsigned char>(lk[dd]))) ++dd;
                        const bool b2 = kk < lk.size() && (lk[kk] == '-' || lk[kk] == '+' || (lk[kk] == '*' && kk > 0)) && kk + 1 < lk.size() && lk[kk + 1] == ' ';
                        const bool n2 = dd > kk && dd < lk.size() && (lk[dd] == '.' || lk[dd] == ')') && dd + 1 < lk.size() && lk[dd + 1] == ' ';
                        if (!b2 && !n2) {
                            if (items.empty() || kk == 0) break;
                            std::vector<Seg> more = InlOf(" " + Trim(lk));
                            items.back().segs.insert(items.back().segs.end(), more.begin(), more.end());
                            ++k;
                            continue;
                        }
                        Out::Item it;
                        it.indent = static_cast<int>(kk) / 2 * 2;
                        it.ordered = n2;
                        it.number = n2 ? std::atoi(lk.substr(kk, dd - kk).c_str()) : 1;
                        std::string text = lk.substr(n2 ? dd + 2 : kk + 2);
                        if (StartsWith(text, "[ ] ") || StartsWith(text, "[X] ") || StartsWith(text, "[x] ") || StartsWith(text, "[-] ")) {
                            it.checkbox = text[1] == ' ' ? 0 : 1;
                            text = text.substr(4);
                        }
                        it.segs = InlOf(text);
                        items.push_back(it);
                        ++k;
                    }
                    i = k - 1;
                    out.List(items);
                    continue;
                }
            }
            para.push_back(l);
        }
        flush();
        return out.Str();
    }
};

}  // namespace

std::string FromOrg(const std::string &org) {
    OrgReader r;
    return r.Read(org);
}

// ===========================================================================
// DOCX / ODT
// ===========================================================================

bool FromOffice(const std::string &bytes, bool odt, const std::string &media_dir, const std::string &media_rel,
                std::string *mepml, std::string *error) {
    OfficeDoc od;
    std::string err;
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.data());
    const bool ok = odt ? LoadOdtFromMemory(data, bytes.size(), od, err) : LoadDocxFromMemory(data, bytes.size(), od, err);
    if (!ok) {
        if (error) *error = err;
        return false;
    }
    Out out;
    std::vector<Out::Item> list;
    auto flush_list = [&]() {
        if (!list.empty()) out.List(list);
        list.clear();
    };
    auto segs_of = [](const DocParagraph &p) {
        std::vector<Seg> segs;
        int at = 0;
        auto plain = [&](int a, int b) {
            if (b <= a) return;
            Seg s;
            s.text = p.text.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
            segs.push_back(s);
        };
        for (const DocSpan &sp : p.spans) {
            plain(at, sp.start);
            if (sp.end <= sp.start) continue;
            Seg s;
            s.text = p.text.substr(static_cast<size_t>(sp.start), static_cast<size_t>(sp.end - sp.start));
            const DocFormat &f = sp.fmt;
            if (f.math) {
                s.kind = Seg::Math;
            } else {
                s.f.b = f.bold;
                s.f.i = f.italic;
                s.f.u = f.underline;
                s.f.s = f.strike;
                s.f.sup = f.superscript;
                s.f.sub = f.subscript;
                s.f.mono = f.font_family == OfficeFontFamily::Mono;
                s.f.mark = f.has_highlight;
                if (f.has_color && (f.color_r | f.color_g | f.color_b) != 0) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", f.color_r, f.color_g, f.color_b);
                    s.f.color = buf;
                }
                if (f.font_size_pt > 0.0f && (f.font_size_pt < 10.5f || f.font_size_pt > 12.5f)) {
                    char buf[16];
                    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(f.font_size_pt));
                    s.f.size = buf;
                }
            }
            segs.push_back(s);
            at = sp.end;
        }
        plain(at, static_cast<int>(p.text.size()));
        for (Seg &s : segs)
            for (char &c : s.text)
                if (c == '\t') c = ' ';
        return segs;
    };
    int image_n = 0;
    for (const DocParagraph &p : od.paragraphs) {
        const std::vector<Seg> segs = segs_of(p);
        if (p.list_kind != DocParagraph::ListKind::None && !Trim(p.text).empty()) {
            Out::Item it;
            it.ordered = p.list_kind == DocParagraph::ListKind::Numbered;
            it.number = static_cast<int>(list.size()) + 1;
            it.segs = segs;
            list.push_back(it);
        } else {
            flush_list();
            if (p.heading_level > 0) out.Heading(p.heading_level, segs);
            else if (!Trim(p.text).empty()) out.Paragraph(segs);
        }
        if (p.table_ref >= 0 && p.table_ref < static_cast<int>(od.tables.size())) {
            flush_list();
            const DocTable &t = od.tables[static_cast<size_t>(p.table_ref)];
            std::vector<std::vector<std::vector<Seg>>> rows;
            for (int r = 0; r < t.rows; ++r) {
                std::vector<std::vector<Seg>> row;
                for (int c = 0; c < t.cols; ++c) row.push_back(TextSegs(t.Cell(r, c)));
                rows.push_back(row);
            }
            out.Table(rows, t.rows > 1 ? 1 : 0, {});
        }
        if (p.image_ref >= 0 && p.image_ref < static_cast<int>(od.images.size())) {
            flush_list();
            const DocImage &img = od.images[static_cast<size_t>(p.image_ref)];
            std::string ext = SniffImageExtension(img.bytes);
            if (ext.empty()) ext = "png";
            const std::string name = "image" + std::to_string(++image_n) + "." + ext;
            std::error_code ec;
            std::filesystem::create_directories(media_dir, ec);
            std::ofstream f(media_dir + "/" + name, std::ios::binary);
            f.write(img.bytes.data(), static_cast<std::streamsize>(img.bytes.size()));
            out.Image(media_rel + "/" + name, "");
        }
    }
    flush_list();
    if (mepml) *mepml = out.Str();
    return true;
}


// ===========================================================================
// DOCX and ODT, read directly (styles, lists, links, footnotes, images)
// ===========================================================================

namespace {

std::string ZipEntry(const std::string &bytes, const char *name) {
    std::string out;
    zip::Extract(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), name, out);
    return out;
}

bool LoadXml(xml::xml_document &d, const std::string &text) {
    if (text.empty()) return false;
    return static_cast<bool>(d.load_buffer(text.data(), text.size()));
}

std::string Attr(const xml::xml_node &n, const char *name) { return n.attribute(name).as_string(); }
xml::xml_node NextElement(xml::xml_node n) {
    for (n = n.next_sibling(); n && n.type() != xml::node_element; n = n.next_sibling()) {
    }
    return n;
}
xml::xml_node FirstElement(const xml::xml_node &n) {
    for (const xml::xml_node &k : n.children())
        if (k.type() == xml::node_element) return k;
    return xml::xml_node();
}

// --- Office maths to TeX --------------------------------------------------------
// Word's OMML and ODF's MathML describe maths as structure; mepml writes TeX.

// One character (or name) of maths text as TeX.
std::string TexSymbols(const std::string &s) {
    static const std::map<std::string, std::string> kSym = {
        {"α", "\\alpha"},  {"β", "\\beta"},     {"γ", "\\gamma"},  {"δ", "\\delta"},  {"ε", "\\epsilon"}, {"ζ", "\\zeta"},
        {"η", "\\eta"},    {"θ", "\\theta"},    {"ι", "\\iota"},   {"κ", "\\kappa"},  {"λ", "\\lambda"},  {"μ", "\\mu"},
        {"ν", "\\nu"},     {"ξ", "\\xi"},       {"π", "\\pi"},     {"ρ", "\\rho"},    {"σ", "\\sigma"},   {"τ", "\\tau"},
        {"υ", "\\upsilon"}, {"φ", "\\phi"},     {"χ", "\\chi"},    {"ψ", "\\psi"},    {"ω", "\\omega"},   {"Γ", "\\Gamma"},
        {"Δ", "\\Delta"},  {"Θ", "\\Theta"},    {"Λ", "\\Lambda"}, {"Ξ", "\\Xi"},     {"Π", "\\Pi"},      {"Σ", "\\Sigma"},
        {"Φ", "\\Phi"},    {"Ψ", "\\Psi"},      {"Ω", "\\Omega"},  {"∞", "\\infty"},  {"→", "\\to"},      {"←", "\\leftarrow"},
        {"⇒", "\\Rightarrow"}, {"⇔", "\\Leftrightarrow"}, {"≤", "\\le"}, {"≥", "\\ge"}, {"≠", "\\ne"}, {"≈", "\\approx"},
        {"±", "\\pm"},     {"∓", "\\mp"},       {"×", "\\times"},  {"÷", "\\div"},    {"·", "\\cdot"},    {"⋅", "\\cdot"},
        {"∂", "\\partial"}, {"∇", "\\nabla"},   {"∈", "\\in"},     {"∉", "\\notin"},  {"⊂", "\\subset"},  {"⊆", "\\subseteq"},
        {"∪", "\\cup"},    {"∩", "\\cap"},      {"∀", "\\forall"}, {"∃", "\\exists"}, {"∅", "\\emptyset"}, {"∑", "\\sum"},
        {"∏", "\\prod"},   {"∫", "\\int"},      {"∬", "\\iint"},   {"∮", "\\oint"},   {"√", "\\surd"},    {"…", "\\ldots"},
        {"⋯", "\\cdots"},  {"−", "-"},          {"∗", "*"},        {"′", "'"},        {"≡", "\\equiv"},   {"∝", "\\propto"},
        {"ℝ", "\\mathbb{R}"}, {"ℕ", "\\mathbb{N}"}, {"ℤ", "\\mathbb{Z}"}, {"ℚ", "\\mathbb{Q}"}, {"ℂ", "\\mathbb{C}"}};
    static const char *kFuncs[] = {"sin", "cos", "tan", "log", "ln", "exp", "lim", "max", "min", "sup", "inf", "det", "sinh", "cosh", "tanh"};
    for (const char *f : kFuncs)
        if (s == f) return "\\" + s;
    std::string o;
    for (size_t i = 0; i < s.size();) {
        size_t n = 1;
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0xF0) n = 4;
        else if (c >= 0xE0) n = 3;
        else if (c >= 0xC0) n = 2;
        const std::string ch = s.substr(i, n);
        auto it = kSym.find(ch);
        if (it != kSym.end()) {
            o += it->second;
            // A command followed by a letter needs a space between.
            if (i + n < s.size() && std::isalpha(static_cast<unsigned char>(s[i + n])) && it->second[0] == '\\') o += ' ';
        } else {
            o += ch;
        }
        i += n;
    }
    return o;
}

std::string Local(const xml::xml_node &n) {
    const std::string nm = n.name();
    const size_t colon = nm.find(':');
    return colon == std::string::npos ? nm : nm.substr(colon + 1);
}

std::string OmmlTex(const xml::xml_node &n);
std::string OmmlKids(const xml::xml_node &n) {
    std::string o;
    for (const xml::xml_node &k : n.children()) {
        const std::string l = Local(k);
        if (l.size() > 2 && l.compare(l.size() - 2, 2, "Pr") == 0) continue;  // properties
        o += OmmlTex(k);
    }
    return o;
}
std::string OmmlArg(const xml::xml_node &n, const char *child) { return OmmlKids(n.child(child)); }
std::string OmmlVal(const xml::xml_node &pr, const char *child, const std::string &fallback) {
    const xml::xml_node c = pr.child(child);
    if (!c) return fallback;
    return c.attribute("m:val").as_string();
}
std::string OmmlTex(const xml::xml_node &n) {
    const std::string l = Local(n);
    if (n.type() == xml::node_pcdata) return "";
    if (l == "t") return TexSymbols(n.text().get());
    if (l == "sSup") return OmmlArg(n, "m:e") + "^{" + OmmlArg(n, "m:sup") + "}";
    if (l == "sSub") return OmmlArg(n, "m:e") + "_{" + OmmlArg(n, "m:sub") + "}";
    if (l == "sSubSup") return OmmlArg(n, "m:e") + "_{" + OmmlArg(n, "m:sub") + "}^{" + OmmlArg(n, "m:sup") + "}";
    if (l == "sPre") return "{}_{" + OmmlArg(n, "m:sub") + "}^{" + OmmlArg(n, "m:sup") + "}" + OmmlArg(n, "m:e");
    if (l == "f") return "\\frac{" + OmmlArg(n, "m:num") + "}{" + OmmlArg(n, "m:den") + "}";
    if (l == "rad") {
        const std::string deg = OmmlArg(n, "m:deg");
        return deg.empty() ? "\\sqrt{" + OmmlArg(n, "m:e") + "}" : "\\sqrt[" + deg + "]{" + OmmlArg(n, "m:e") + "}";
    }
    if (l == "d") {
        const xml::xml_node pr = n.child("m:dPr");
        const auto delim = [](const std::string &c) {
            return c == "{" ? std::string("\\{") : c == "}" ? std::string("\\}") : TexSymbols(c);
        };
        std::string o = delim(OmmlVal(pr, "m:begChr", "("));
        const std::string sep = OmmlVal(pr, "m:sepChr", "|");
        bool first = true;
        for (const xml::xml_node &e : n.children("m:e")) {
            if (!first) o += TexSymbols(sep);
            o += OmmlKids(e);
            first = false;
        }
        return o + delim(OmmlVal(pr, "m:endChr", ")"));
    }
    if (l == "nary") {
        const xml::xml_node pr = n.child("m:naryPr");
        std::string o = TexSymbols(OmmlVal(pr, "m:chr", "∫"));
        const std::string sub = OmmlArg(n, "m:sub"), sup = OmmlArg(n, "m:sup");
        if (!sub.empty()) o += "_{" + sub + "}";
        if (!sup.empty()) o += "^{" + sup + "}";
        return o + " " + OmmlArg(n, "m:e");
    }
    if (l == "func") return OmmlArg(n, "m:fName") + " " + OmmlArg(n, "m:e");
    if (l == "acc") {
        const std::string chr = OmmlVal(n.child("m:accPr"), "m:chr", "̂");
        const std::string cmd = chr == "̇" ? "\\dot" : chr == "̈" ? "\\ddot" : chr == "̃" ? "\\tilde" : chr == "⃗" ? "\\vec" : chr == "̄" || chr == "¯" ? "\\bar" : "\\hat";
        return cmd + "{" + OmmlArg(n, "m:e") + "}";
    }
    if (l == "bar") return "\\overline{" + OmmlArg(n, "m:e") + "}";
    if (l == "limLow") return OmmlArg(n, "m:e") + "_{" + OmmlArg(n, "m:lim") + "}";
    if (l == "limUpp") return OmmlArg(n, "m:e") + "^{" + OmmlArg(n, "m:lim") + "}";
    if (l == "m") {
        std::string o = "\\begin{matrix}";
        bool first_row = true;
        for (const xml::xml_node &r : n.children("m:mr")) {
            o += first_row ? " " : " \\\\ ";
            bool first = true;
            for (const xml::xml_node &e : r.children("m:e")) {
                o += (first ? "" : " & ") + OmmlKids(e);
                first = false;
            }
            first_row = false;
        }
        return o + " \\end{matrix}";
    }
    if (l == "eqArr") {
        std::string o;
        for (const xml::xml_node &e : n.children("m:e")) o += (o.empty() ? "" : " \\\\ ") + OmmlKids(e);
        return "\\begin{aligned}" + o + "\\end{aligned}";
    }
    return OmmlKids(n);
}

std::string MathmlTex(const xml::xml_node &n);
std::string MathmlKids(const xml::xml_node &n) {
    std::string o;
    for (const xml::xml_node &k : n.children())
        if (k.type() == xml::node_element) o += MathmlTex(k);
    return o;
}
std::vector<xml::xml_node> Elements(const xml::xml_node &n) {
    std::vector<xml::xml_node> v;
    for (const xml::xml_node &k : n.children())
        if (k.type() == xml::node_element) v.push_back(k);
    return v;
}
std::string MathmlTex(const xml::xml_node &n) {
    const std::string l = Local(n);
    const std::vector<xml::xml_node> a = Elements(n);
    auto arg = [&](size_t i) { return i < a.size() ? MathmlTex(a[i]) : std::string(); };
    if (l == "semantics") {
        for (const xml::xml_node &k : a)
            if (Local(k) == "annotation" && std::string(k.attribute("encoding").as_string()).find("tex") != std::string::npos)
                return Trim(k.text().get());
        return arg(0);
    }
    if (l == "annotation" || l == "annotation-xml") return "";
    if (l == "mi" || l == "mn" || l == "mo") return TexSymbols(Trim(n.text().get()));
    if (l == "mtext") return "\\text{" + std::string(n.text().get()) + "}";
    if (l == "mspace") return "\\ ";
    if (l == "msup") return arg(0) + "^{" + arg(1) + "}";
    if (l == "msub") return arg(0) + "_{" + arg(1) + "}";
    if (l == "msubsup") return arg(0) + "_{" + arg(1) + "}^{" + arg(2) + "}";
    if (l == "mfrac") return "\\frac{" + arg(0) + "}{" + arg(1) + "}";
    if (l == "msqrt") return "\\sqrt{" + MathmlKids(n) + "}";
    if (l == "mroot") return "\\sqrt[" + arg(1) + "]{" + arg(0) + "}";
    if (l == "mover" || l == "munder") {
        const std::string mark = a.size() > 1 ? Trim(a[1].text().get()) : "";
        const bool over = l == "mover";
        if (over && (mark == "^" || mark == "ˆ")) return "\\hat{" + arg(0) + "}";
        if (over && (mark == "¯" || mark == "‾")) return "\\bar{" + arg(0) + "}";
        if (over && mark == "→") return "\\vec{" + arg(0) + "}";
        if (over && mark == "~") return "\\tilde{" + arg(0) + "}";
        if (over && mark == "˙") return "\\dot{" + arg(0) + "}";
        return arg(0) + (over ? "^{" : "_{") + arg(1) + "}";
    }
    if (l == "munderover") return arg(0) + "_{" + arg(1) + "}^{" + arg(2) + "}";
    if (l == "mfenced") {
        const std::string open = n.attribute("open") ? n.attribute("open").as_string() : "(";
        const std::string close = n.attribute("close") ? n.attribute("close").as_string() : ")";
        std::string o;
        for (size_t i = 0; i < a.size(); ++i) o += (i ? "," : "") + MathmlTex(a[i]);
        return open + o + close;
    }
    if (l == "mtable") {
        std::string o = "\\begin{matrix}";
        for (size_t r = 0; r < a.size(); ++r) {
            o += r ? " \\\\ " : " ";
            const std::vector<xml::xml_node> cells = Elements(a[r]);
            for (size_t c = 0; c < cells.size(); ++c) o += (c ? " & " : "") + MathmlKids(cells[c]);
        }
        return o + " \\end{matrix}";
    }
    return MathmlKids(n);
}

// A paragraph's role, from its style.
enum class ParaKind {
    Body, Title, Subtitle, Author, Date, Heading, Code, Quote, Caption, TocHeading, Toc, Bibliography, MathDisplay, Rule,
    AbstractTitle, Abstract
};

// mep's own writers spell <small>, >big<, +inserted+ and !deleted! as a
// size or a colour; read those back as the constructs they came from.
void OfficeFmt(std::vector<Seg> &segs) {
    for (Seg &sg : segs) {
        Fmt &f = sg.f;
        if (f.size == "9") {
            f.small = true;
            f.size.clear();
        } else if (f.size == "15") {
            f.big = true;
            f.size.clear();
        }
        if (f.color == "#2e7d32" && f.u) {
            f.ins = true;
            f.u = false;
            f.color.clear();
        } else if (f.color == "#c62828" && f.s) {
            f.del = true;
            f.s = false;
            f.color.clear();
        }
    }
}

// Prose whitespace, folded as a reader would: runs of spaces across runs
// of text become one.
void CollapseSpaces(std::vector<Seg> &segs) {
    char last = 0;
    for (Seg &sg : segs) {
        if (sg.kind == Seg::Break) {
            last = '\n';
            continue;
        }
        if (sg.kind != Seg::Text) {
            last = 'x';
            continue;
        }
        std::string o;
        for (char c : sg.text) {
            if (c == ' ' && (last == ' ' || last == '\n')) continue;
            o += c;
            last = c;
        }
        sg.text = o;
    }
}

// The custom properties mep's writers add (see OfficeProps in
// mepml_export.cpp): //? metadata, code block headers, bibliography fields.
struct OfficeProps {
    std::map<std::string, std::string> props;  // as stored: \n, \t, \\ escaped
    void Set(const std::string &name, const std::string &raw) {
        std::string v;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                const char n = raw[++i];
                v += n == 'n' ? '\n' : n == 't' ? '\t' : n;
            } else {
                v += raw[i];
            }
        }
        props[name] = v;
    }
    void Apply(Out &out, std::vector<std::string> *citations) const {
        std::map<int, std::string> meta;
        for (const auto &kv : props) {
            if (StartsWith(kv.first, "mepml.meta.")) meta[std::atoi(kv.first.c_str() + 11)] = kv.second;
            if (StartsWith(kv.first, "mepml.cite.")) {
                std::string fields;
                for (const std::string &l : SplitLines(kv.second)) {
                    const size_t tab = l.find('\t');
                    if (tab == std::string::npos) continue;
                    fields += "\n  " + l.substr(0, tab) + " = {" + GroupEsc(l.substr(tab + 1)) + "},";
                }
                if (!fields.empty()) fields.pop_back();
                citations->push_back(CitationEntry(kv.first.substr(11), fields));
            }
        }
        for (const auto &m : meta) out.meta.push_back("//? " + m.second);
    }
    // Code block N's html result (mepml.result.N), "" when it has none.
    std::string Result(int n) const {
        auto it = props.find("mepml.result." + std::to_string(n));
        return it == props.end() ? std::string() : it->second;
    }
    // Code block N's language and options.
    bool Code(int n, std::string *lang, std::vector<std::pair<std::string, std::string>> *opts) const {
        auto it = props.find("mepml.code." + std::to_string(n));
        if (it == props.end()) return false;
        const std::vector<std::string> lines = SplitLines(it->second);
        *lang = lines.empty() ? "" : lines[0];
        opts->clear();
        for (size_t i = 1; i < lines.size(); ++i) {
            const size_t tab = lines[i].find('\t');
            if (tab != std::string::npos) opts->push_back({lines[i].substr(0, tab), lines[i].substr(tab + 1)});
        }
        return true;
    }
};

// Writes an embedded image into the media directory; returns its path
// relative to the .mepml.
struct Media {
    std::string dir, rel;
    int n = 0;
    std::string Save(const std::string &bytes, std::string ext) {
        if (bytes.empty()) return "";
        if (ext.empty()) ext = "png";
        const std::string name = "image" + std::to_string(++n) + "." + ext;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream f(dir + "/" + name, std::ios::binary);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return rel + "/" + name;
    }
};

// Shared assembly: paragraphs arrive one at a time with a role; runs of
// code paragraphs become one code block, a caption above a table waits
// for it, list items gather into one list.
struct Assembler {
    Out out;
    std::vector<std::string> code;
    std::vector<std::string> results;
    std::vector<Out::Item> list;
    std::vector<Seg> held_caption;
    bool hold = false;
    bool bibliography = false;  // mep's own "References" was seen: its entries are regenerated
    std::string code_lang;
    std::vector<std::pair<std::string, std::string>> code_opts;
    std::vector<std::string> citations;
    std::vector<std::vector<Seg>> abstract;  // consecutive Abstract paragraphs, not yet written

    std::string code_html;  // the block's html result, from the package's properties
    // A code block begins here (a mepml_code_N bookmark).
    void StartCode(const std::string &lang, const std::vector<std::pair<std::string, std::string>> &opts,
                   const std::string &html_result = "") {
        FlushCode();
        code_lang = lang;
        code_opts = opts;
        code_html = html_result;
    }
    void FlushAbstract() {
        if (!abstract.empty()) out.Abstract(abstract);
        abstract.clear();
    }
    void FlushCode() {
        FlushAbstract();
        if (!code.empty()) {
            std::string body;
            for (const std::string &l : code) body += l + "\n";
            out.Code(code_lang, code_opts, body);
            code.clear();
        }
        if (!code_html.empty()) {
            // The page showed the HTML's text; the markup itself came along.
            out.Results(SplitLines(code_html), "html");
            results.clear();
            code_html.clear();
        }
        code_lang.clear();
        code_opts.clear();
        if (!results.empty()) {
            out.Results(results);
            results.clear();
        }
    }
    void FlushList() {
        FlushAbstract();
        if (!list.empty()) out.List(list);
        list.clear();
    }
    void Flush() {
        FlushCode();
        FlushList();
    }
    static std::string Plain(const std::vector<Seg> &segs) {
        std::string t;
        for (const Seg &s : segs) t += s.kind == Seg::Break ? "\n" : s.text;
        return t;
    }
    void Para(ParaKind kind, int level, std::vector<Seg> segs, bool gray) {
        if (kind == ParaKind::Toc) return;  // the table of contents' own entries: \toc regenerates them
        // An abstract's title is implied by \abstract; its paragraphs
        // gather until something else arrives.
        if (kind == ParaKind::AbstractTitle) {
            Flush();
            return;
        }
        if (kind == ParaKind::Abstract) {
            if (abstract.empty()) Flush();
            OfficeFmt(segs);
            CollapseSpaces(segs);
            if (!Trim(Plain(segs)).empty()) abstract.push_back(std::move(segs));
            return;
        }
        if (kind == ParaKind::Bibliography && bibliography) return;
        if (kind == ParaKind::Rule && Trim(Plain(segs)).empty()) {
            Flush();
            out.Rule();
            return;
        }
        OfficeFmt(segs);
        if (kind != ParaKind::Code) CollapseSpaces(segs);
        if (kind != ParaKind::Code) FlushCode();
        // A paragraph that is nothing but display maths.
        {
            std::vector<const Seg *> real;
            for (const Seg &sg : segs)
                if (!(sg.kind == Seg::Text && Trim(sg.text).empty())) real.push_back(&sg);
            if (real.size() == 1 && (real[0]->kind == Seg::DisplayMath || (kind == ParaKind::MathDisplay && real[0]->kind == Seg::Math))) {
                FlushList();
                out.Math(real[0]->text);
                return;
            }
        }
        if (kind == ParaKind::Code) {
            FlushList();
            const std::string line = Plain(segs);
            if (gray && !code.empty()) results.push_back(line);
            else {
                if (!results.empty()) FlushCode();
                code.push_back(line);
            }
            return;
        }
        const bool empty = Trim(Plain(segs)).empty();
        if (empty) return;
        FlushList();
        switch (kind) {
            case ParaKind::Title: out.meta.push_back("//? Title: " + Trim(Plain(segs))); break;
            case ParaKind::Subtitle: out.meta.push_back("//? Subtitle: " + Trim(Plain(segs))); break;
            case ParaKind::Author: out.meta.push_back("//? Author: " + Trim(Plain(segs))); break;
            case ParaKind::Date: out.meta.push_back("//? Date: " + Trim(Plain(segs))); break;
            case ParaKind::Heading: out.Heading(level, segs); break;
            case ParaKind::TocHeading: out.Block("\\toc"); break;
            case ParaKind::Quote: {
                std::string kind_word = "NOTE";
                // "KEYWORD: text" in bold -- mep's own callout export.
                if (!segs.empty() && segs[0].f.b) {
                    const std::string t = Trim(segs[0].text);
                    if (!t.empty() && t.back() == ':') {
                        kind_word = t.substr(0, t.size() - 1);
                        segs.erase(segs.begin());
                    }
                }
                out.Callout(kind_word, segs);
                break;
            }
            case ParaKind::Caption:
                HtmlReader::StripLabel(segs);
                if (!segs.empty() && segs[0].kind == Seg::Text && Trim(segs[0].text).empty()) segs.erase(segs.begin());
                if (out.last == Out::kFigure || out.last == Out::kTable || out.last == Out::kCode) out.Caption(segs);
                else {
                    held_caption = segs;
                    hold = true;
                }
                break;
            default: out.Paragraph(segs); break;
        }
    }
    void Item(Out::Item it) {
        FlushCode();
        OfficeFmt(it.segs);
        CollapseSpaces(it.segs);
        // "☐ task" / "☒ done": mep's own export of a task list item.
        if (!it.segs.empty() && it.segs[0].kind == Seg::Text) {
            std::string &t = it.segs[0].text;
            if (StartsWith(t, "☐ ")) { it.checkbox = 0; t = t.substr(4); }
            else if (StartsWith(t, "☒ ")) { it.checkbox = 1; t = t.substr(4); }
        }
        list.push_back(it);
    }
    void Table(std::vector<std::vector<std::vector<Seg>>> rows, int header, const std::vector<Align> &aligns) {
        Flush();
        for (auto &r : rows)
            for (auto &c : r) OfficeFmt(c);
        bool any_align = false;
        for (Align a : aligns) any_align = any_align || a != Align::Default;
        out.Table(rows, header, any_align ? aligns : std::vector<Align>());
        if (hold) {
            out.Caption(held_caption);
            hold = false;
        }
    }
    void Image(const std::string &path, const std::string &alt) {
        Flush();
        out.Image(path, alt);
        if (hold) {
            out.Caption(held_caption);
            hold = false;
        }
    }
    std::string Str() {
        Flush();
        if (hold) out.Paragraph(held_caption);
        for (const std::string &c : citations) out.blocks.push_back(c);
        return out.Str();
    }
    // A caption placed above the table it belongs to.
    void HoldCaption(std::vector<Seg> segs) {
        OfficeFmt(segs);
        CollapseSpaces(segs);
        HtmlReader::StripLabel(segs);
        if (!segs.empty() && segs[0].kind == Seg::Text && Trim(segs[0].text).empty()) segs.erase(segs.begin());
        FlushCode();
        held_caption = segs;
        hold = true;
    }
    // A heading carrying the mepml_bibliography bookmark.
    void Bibliography() {
        Flush();
        out.Block("\\bibliography");
        bibliography = true;
    }
};

// --- DOCX ---------------------------------------------------------------------

// A paragraph's mep bookmarks: the start of code block N (its header comes
// from the properties), or mep's own References heading (true: the
// paragraph itself is consumed).
bool Marks(const std::vector<std::string> &bookmarks, const OfficeProps &props, Assembler &as) {
    for (const std::string &b : bookmarks) {
        if (b == "mepml_bibliography") {
            as.Bibliography();
            return true;
        }
        if (StartsWith(b, "mepml_code_")) {
            std::string lang;
            std::vector<std::pair<std::string, std::string>> opts;
            const int n = std::atoi(b.c_str() + 11);
            props.Code(n, &lang, &opts);
            as.StartCode(lang, opts, props.Result(n));
        }
    }
    return false;
}

struct DocxReader {
    std::string bytes;
    Media media;
    Assembler as;
    std::map<std::string, std::string> style_name;   // styleId -> lowercased name
    std::map<std::string, std::string> style_base;   // styleId -> basedOn
    std::map<std::string, int> style_outline;        // styleId -> outline level (0-based)
    std::map<std::string, std::string> rels;         // rId -> target
    std::set<std::string> external;                  // rIds with TargetMode="External"
    std::map<std::string, xml::xml_node> footnotes;  // id -> <w:footnote>
    xml::xml_document fn_doc;
    std::map<std::string, std::string> num_abstract;               // numId -> abstractNumId
    std::map<std::string, std::map<int, std::string>> abstract_fmt;  // abstractNumId -> level -> numFmt
    OfficeProps props;
    std::vector<std::string> bookmarks;  // named in the paragraph being read
    int default_sz = 22;                 // half-points

    std::string StyleName(std::string id) {
        for (int guard = 0; guard < 8 && !id.empty(); ++guard) {
            auto it = style_name.find(id);
            const std::string n = it == style_name.end() ? Lower(id) : it->second;
            if (StartsWith(n, "heading") || n == "title" || n == "subtitle" || n.find("code") != std::string::npos ||
                n.find("preformatted") != std::string::npos || n.find("quote") != std::string::npos || n == "caption" ||
                StartsWith(n, "toc") || n == "block text" || n == "plain text" || n == "verbatim" || n == "bibliography" ||
                n == "author" || n == "date" || n == "abstract" || n == "abstract title")
                return n;
            auto b = style_base.find(id);
            if (b == style_base.end()) return n;
            id = b->second;
        }
        return "";
    }

    void Runs(const xml::xml_node &parent, Fmt f, std::vector<Seg> &segs, std::string &field_url, int &field_state) {
        for (const xml::xml_node &c : parent.children()) {
            const std::string name = c.name();
            if (name == "w:r") {
                Fmt rf = f;
                bool verbatim = false;
                const xml::xml_node pr = c.child("w:rPr");
                if (pr) {
                    auto on = [&](const char *tag) {
                        const xml::xml_node t = pr.child(tag);
                        if (!t) return -1;
                        const std::string v = Attr(t, "w:val");
                        return (v == "0" || v == "false" || v == "none") ? 0 : 1;
                    };
                    if (on("w:b") >= 0) rf.b = on("w:b") == 1;
                    if (on("w:i") >= 0) rf.i = on("w:i") == 1;
                    if (on("w:u") >= 0) rf.u = on("w:u") == 1;
                    if (on("w:strike") >= 0) rf.s = on("w:strike") == 1;
                    if (on("w:dstrike") >= 0) rf.s = on("w:dstrike") == 1;
                    const std::string va = Attr(pr.child("w:vertAlign"), "w:val");
                    if (va == "superscript") rf.sup = true;
                    if (va == "subscript") rf.sub = true;
                    const std::string col = Attr(pr.child("w:color"), "w:val");
                    if (!col.empty() && col != "auto" && col != "000000" && col.size() == 6) rf.color = "#" + Lower(col);
                    const std::string hl = Attr(pr.child("w:highlight"), "w:val");
                    if (!hl.empty() && hl != "none") rf.mark = true;
                    const std::string face = Attr(pr.child("w:rFonts"), "w:ascii");
                    const std::string font = Lower(face);
                    if (font.find("courier") != std::string::npos || font.find("mono") != std::string::npos ||
                        font.find("consolas") != std::string::npos)
                        rf.mono = true;
                    else if (!face.empty())
                        rf.font = face;
                    const int sz = pr.child("w:sz").attribute("w:val").as_int(0);
                    if (sz > 0 && sz != default_sz) {
                        char buf[16];
                        std::snprintf(buf, sizeof buf, "%g", sz / 2.0);
                        rf.size = buf;
                    }
                    const std::string rs = Lower(Attr(pr.child("w:rStyle"), "w:val"));
                    if (rs.find("verbatim") != std::string::npos || rs.find("code") != std::string::npos) rf.mono = true;
                    verbatim = rs == "verbatimchar";
                }
                if (!field_url.empty() && field_state == 2) rf.link = field_url;
                for (const xml::xml_node &k : c.children()) {
                    const std::string kn = k.name();
                    if (kn == "w:t" || kn == "w:delText") {
                        Seg sg;
                        sg.text = k.text().get();
                        sg.f = rf;
                        if (verbatim) {
                            sg.kind = Seg::Code;
                            sg.f.mono = false;
                        }
                        if (kn == "w:delText") sg.f.del = true;
                        segs.push_back(sg);
                    } else if (kn == "w:tab") {
                        segs.push_back(TextSegs(" ", rf)[0]);
                    } else if (kn == "w:br" || kn == "w:cr") {
                        Seg br;
                        br.kind = Seg::Break;
                        segs.push_back(br);
                    } else if (kn == "w:fldChar") {
                        const std::string t = Attr(k, "w:fldCharType");
                        if (t == "begin") {
                            field_state = 1;
                            field_url.clear();
                        } else if (t == "separate") {
                            field_state = 2;
                        } else if (t == "end") {
                            field_state = 0;
                            field_url.clear();
                        }
                    } else if (kn == "w:instrText") {
                        const std::string instr = k.text().get();
                        const size_t h = instr.find("HYPERLINK");
                        if (h != std::string::npos) {
                            const size_t q1 = instr.find('"', h);
                            const size_t q2 = q1 == std::string::npos ? q1 : instr.find('"', q1 + 1);
                            if (q2 != std::string::npos) field_url = instr.substr(q1 + 1, q2 - q1 - 1);
                        }
                    } else if (kn == "w:footnoteReference" || kn == "w:endnoteReference") {
                        auto it = footnotes.find(Attr(k, "w:id"));
                        if (it != footnotes.end()) {
                            std::vector<Seg> body;
                            for (const xml::xml_node &fp : it->second.children("w:p")) {
                                std::string u;
                                int st = 0;
                                if (!body.empty()) body.push_back(TextSegs(" ")[0]);
                                Runs(fp, Fmt(), body, u, st);
                            }
                            Coalesce(body);
                            Seg fn;
                            fn.kind = Seg::Footnote;
                            std::string t = Trim(RenderSegs(body));
                            for (char &ch : t)
                                if (ch == '\n') ch = ' ';
                            fn.text = t;
                            segs.push_back(fn);
                        }
                    } else if (kn == "w:pict" && std::string(k.child("v:rect").attribute("o:hr").as_string()) == "t") {
                        hr = true;  // Word's horizontal line shape
                    } else if (kn == "w:drawing" || kn == "w:pict") {
                        pending_images.push_back(Image(k));
                    }
                }
            } else if (name == "w:bookmarkStart") {
                bookmarks.push_back(Attr(c, "w:name"));
            } else if (name == "w:hyperlink" && StartsWith(Attr(c, "w:anchor"), "ref_") &&
                       (Attr(c, "w:tooltip") == "\\cite" || Attr(c, "w:tooltip") == "\\citep")) {
                // mep's own citation: a link to its bibliography entry.
                Seg cite;
                cite.kind = Attr(c, "w:tooltip") == "\\citep" ? Seg::CiteP : Seg::Cite;
                cite.text = Attr(c, "w:anchor").substr(4);
                segs.push_back(cite);
            } else if (name == "w:hyperlink") {
                Fmt lf = f;
                auto it = rels.find(Attr(c, "r:id"));
                if (it != rels.end()) lf.link = it->second;
                Runs(c, lf, segs, field_url, field_state);
            } else if (name == "m:oMath" || name == "m:oMathPara") {
                const std::string tex = Trim(OmmlTex(c));
                Seg sg;
                sg.kind = name == "m:oMathPara" ? Seg::DisplayMath : Seg::Math;
                sg.text = tex;
                segs.push_back(sg);
            } else if (name == "w:ins" || name == "w:del" || name == "w:smartTag" || name == "w:sdt" || name == "w:sdtContent" ||
                       name == "w:fldSimple" || name == "w:customXml") {
                Fmt inner = f;
                if (name == "w:ins") inner.ins = true;
                if (name == "w:fldSimple") {
                    const std::string instr = Attr(c, "w:instr");
                    const size_t q1 = instr.find('"');
                    if (instr.find("HYPERLINK") != std::string::npos && q1 != std::string::npos)
                        inner.link = instr.substr(q1 + 1, instr.find('"', q1 + 1) - q1 - 1);
                }
                Runs(c, inner, segs, field_url, field_state);
            }
        }
    }

    std::vector<std::pair<std::string, std::string>> pending_images;  // path, alt
    bool hr = false;
    std::pair<std::string, std::string> Image(const xml::xml_node &drawing) {
        std::string rid, alt;
        std::function<void(const xml::xml_node &)> walk = [&](const xml::xml_node &n) {
            const std::string nm = n.name();
            if (nm == "a:blip") rid = Attr(n, "r:embed").empty() ? Attr(n, "r:link") : Attr(n, "r:embed");
            if (nm == "v:imagedata") rid = Attr(n, "r:id");
            if (nm == "wp:docPr") alt = Attr(n, "descr");
            for (const xml::xml_node &k : n.children()) walk(k);
        };
        walk(drawing);
        auto it = rels.find(rid);
        if (it == rels.end()) return {"", ""};
        if (external.count(rid)) {
            // A linked picture: the path stays a path.
            std::string t = it->second;
            if (StartsWith(t, "file:///")) t = t.substr(7);
            return {t, alt};
        }
        const std::string target = StartsWith(it->second, "/") ? it->second.substr(1) : "word/" + it->second;
        const std::string data = ZipEntry(bytes, target.c_str());
        const size_t dot = target.find_last_of('.');
        std::string ext = dot == std::string::npos ? "png" : Lower(target.substr(dot + 1));
        if (ext == "jpeg") ext = "jpg";
        return {media.Save(data, ext), alt};
    }

    ParaKind Kind(const xml::xml_node &p, int *level) {
        const xml::xml_node ppr = p.child("w:pPr");
        const std::string id = Attr(ppr.child("w:pStyle"), "w:val");
        const std::string n = StyleName(id);
        *level = 0;
        const xml::xml_node ol = ppr.child("w:outlineLvl");
        if (ol) *level = ol.attribute("w:val").as_int(9) + 1;
        if (*level == 0) {
            std::string sid = id;
            for (int g = 0; g < 8 && !sid.empty(); ++g) {
                auto o = style_outline.find(sid);
                if (o != style_outline.end()) {
                    *level = o->second + 1;
                    break;
                }
                auto b = style_base.find(sid);
                if (b == style_base.end()) break;
                sid = b->second;
            }
        }
        if (StartsWith(n, "heading")) {
            if (*level == 0 || *level > 9) *level = std::atoi(n.c_str() + 7);
            if (*level <= 0) *level = 1;
            return ParaKind::Heading;
        }
        if (n == "title") return ParaKind::Title;
        if (n == "subtitle") return ParaKind::Subtitle;
        if (n == "author") return ParaKind::Author;
        if (n == "date") return ParaKind::Date;
        if (n.find("toc heading") != std::string::npos) return ParaKind::TocHeading;
        if (StartsWith(n, "toc")) return ParaKind::Toc;
        if (n == "abstract title") return ParaKind::AbstractTitle;
        if (n == "abstract") return ParaKind::Abstract;
        if (n.find("code") != std::string::npos || n.find("preformatted") != std::string::npos || n == "plain text" || n == "verbatim")
            return ParaKind::Code;
        if (n.find("quote") != std::string::npos || n == "block text") return ParaKind::Quote;
        if (n == "caption") return ParaKind::Caption;
        if (n == "bibliography") return ParaKind::Bibliography;
        if (*level > 0 && *level <= 6) return ParaKind::Heading;
        return ParaKind::Body;
    }

    std::vector<Seg> ParaSegs(const xml::xml_node &p) {
        std::vector<Seg> segs;
        std::string url;
        int st = 0;
        Runs(p, Fmt(), segs, url, st);
        return segs;
    }

    void Body(const xml::xml_node &body) {
        for (const xml::xml_node &c : body.children()) {
            const std::string name = c.name();
            if (name == "w:p") {
                int level = 0;
                const ParaKind kind = Kind(c, &level);
                pending_images.clear();
                bookmarks.clear();
                hr = false;
                std::vector<Seg> segs = ParaSegs(c);
                if (Marks(bookmarks, props, as)) continue;
                if (hr && Trim(Assembler::Plain(segs)).empty()) {
                    as.Para(ParaKind::Rule, 0, segs, false);
                    continue;
                }
                const xml::xml_node numpr = c.child("w:pPr").child("w:numPr");
                bool gray = !segs.empty();
                for (const Seg &sg : segs) gray = gray && sg.f.color == "#555555";
                if (kind == ParaKind::Body && c.child("w:pPr").child("w:pBdr").child("w:bottom") && Trim(Assembler::Plain(segs)).empty() &&
                    pending_images.empty()) {
                    as.Para(ParaKind::Rule, 0, segs, false);
                    continue;
                }
                if (numpr && kind == ParaKind::Body) {
                    Out::Item it;
                    const int ilvl = numpr.child("w:ilvl").attribute("w:val").as_int(0);
                    const std::string num = Attr(numpr.child("w:numId"), "w:val");
                    const std::string fmt = abstract_fmt[num_abstract[num]][ilvl];
                    it.indent = ilvl * 2;
                    it.ordered = !fmt.empty() && fmt != "bullet" && fmt != "none";
                    it.number = 1;
                    int count = 0;
                    for (const Out::Item &prev : as.list)
                        if (prev.indent == it.indent && prev.ordered) ++count;
                    it.number = count + 1;
                    it.segs = segs;
                    if (!Trim(Assembler::Plain(segs)).empty()) as.Item(it);
                } else if (kind == ParaKind::Caption && std::string(NextElement(c).name()) == "w:tbl") {
                    as.HoldCaption(segs);
                } else {
                    as.Para(kind, level, segs, gray);
                }
                for (const auto &img : pending_images)
                    if (!img.first.empty()) as.Image(img.first, img.second);
            } else if (name == "w:tbl") {
                std::vector<std::vector<std::vector<Seg>>> rows;
                std::vector<Align> aligns;
                int header = 0;
                for (const xml::xml_node &tr : c.children("w:tr")) {
                    std::vector<std::vector<Seg>> row;
                    size_t col = 0;
                    for (const xml::xml_node &tc : tr.children("w:tc")) {
                        std::vector<Seg> cell;
                        for (const xml::xml_node &p : tc.children("w:p")) {
                            if (!cell.empty()) cell.push_back(TextSegs(" ")[0]);
                            std::vector<Seg> ps = ParaSegs(p);
                            cell.insert(cell.end(), ps.begin(), ps.end());
                            const std::string jc = Attr(p.child("w:pPr").child("w:jc"), "w:val");
                            if (aligns.size() <= col) aligns.resize(col + 1, Align::Default);
                            if (jc == "center") aligns[col] = Align::Center;
                            else if (jc == "right" || jc == "end") aligns[col] = Align::Right;
                            else if (jc == "left" || jc == "start") aligns[col] = Align::Left;
                        }
                        row.push_back(cell);
                        ++col;
                    }
                    if (tr.child("w:trPr").child("w:tblHeader") && static_cast<int>(rows.size()) == header) {
                        ++header;
                        for (auto &cell : row)
                            for (Seg &sg : cell) sg.f.b = false;  // header rows are bold by their role
                    }
                    rows.push_back(row);
                }
                as.Table(rows, header, aligns);
            } else if (name == "w:sdt") {
                const xml::xml_node content = c.child("w:sdtContent");
                if (content) Body(content);
            }
        }
    }

    bool Read(std::string *out, std::string *error) {
        xml::xml_document document, styles, rel_doc, num_doc;
        if (!LoadXml(document, ZipEntry(bytes, "word/document.xml"))) {
            if (error) *error = "not a .docx (no readable word/document.xml)";
            return false;
        }
        if (LoadXml(styles, ZipEntry(bytes, "word/styles.xml"))) {
            const xml::xml_node dsz = styles.child("w:styles").child("w:docDefaults").child("w:rPrDefault").child("w:rPr").child("w:sz");
            default_sz = dsz ? dsz.attribute("w:val").as_int(20) : 20;
            for (const xml::xml_node &st : styles.child("w:styles").children("w:style")) {
                const std::string id = Attr(st, "w:styleId");
                style_name[id] = Lower(Attr(st.child("w:name"), "w:val"));
                const std::string base = Attr(st.child("w:basedOn"), "w:val");
                if (!base.empty()) style_base[id] = base;
                const xml::xml_node ol = st.child("w:pPr").child("w:outlineLvl");
                if (ol) style_outline[id] = ol.attribute("w:val").as_int(9);
            }
        }
        if (LoadXml(rel_doc, ZipEntry(bytes, "word/_rels/document.xml.rels")))
            for (const xml::xml_node &r : rel_doc.child("Relationships").children("Relationship")) {
                rels[Attr(r, "Id")] = Attr(r, "Target");
                if (Attr(r, "TargetMode") == "External") external.insert(Attr(r, "Id"));
            }
        if (LoadXml(fn_doc, ZipEntry(bytes, "word/footnotes.xml")))
            for (const xml::xml_node &fn : fn_doc.child("w:footnotes").children("w:footnote")) footnotes[Attr(fn, "w:id")] = fn;
        if (LoadXml(num_doc, ZipEntry(bytes, "word/numbering.xml"))) {
            const xml::xml_node root = num_doc.child("w:numbering");
            for (const xml::xml_node &a : root.children("w:abstractNum"))
                for (const xml::xml_node &lvl : a.children("w:lvl"))
                    abstract_fmt[Attr(a, "w:abstractNumId")][lvl.attribute("w:ilvl").as_int(0)] = Attr(lvl.child("w:numFmt"), "w:val");
            for (const xml::xml_node &n : root.children("w:num")) num_abstract[Attr(n, "w:numId")] = Attr(n.child("w:abstractNumId"), "w:val");
        }
        xml::xml_document custom;
        if (LoadXml(custom, ZipEntry(bytes, "docProps/custom.xml")))
            for (const xml::xml_node &pr : custom.child("Properties").children("property"))
                props.Set(Attr(pr, "name"), pr.first_child().text().get());
        Body(document.child("w:document").child("w:body"));
        props.Apply(as.out, &as.citations);
        *out = as.Str();
        return true;
    }
};

// --- ODT ------------------------------------------------------------------------

// ODF whitespace (§6.1.2): within a paragraph, runs of spaces -- across
// element boundaries too -- are one, and leading and trailing ones are
// not there at all; <text:s/> and <text:tab/> (hard segments) are kept.
void OdfSpaces(std::vector<Seg> &segs) {
    bool space = true;  // the paragraph start swallows spaces
    for (Seg &sg : segs) {
        if (sg.kind != Seg::Text || sg.hard) {
            space = sg.kind == Seg::Break;
            if (sg.hard) space = false;
            continue;
        }
        std::string o;
        for (char c : sg.text) {
            if (c == ' ') {
                if (space) continue;
                space = true;
            } else {
                space = false;
            }
            o += c;
        }
        sg.text = o;
    }
    for (auto it = segs.rbegin(); it != segs.rend(); ++it) {
        if (it->kind != Seg::Text || it->hard) break;
        while (!it->text.empty() && it->text.back() == ' ') it->text.pop_back();
        if (!it->text.empty()) break;
    }
}

struct OdtReader {
    std::string bytes;
    Media media;
    Assembler as;
    struct TextStyle {
        Fmt f;
        std::string parent, display;
        int outline = 0;
        bool center = false, right = false, left = false;
    };
    std::map<std::string, TextStyle> styles;  // style:name -> properties
    std::set<std::string> numbered_lists;     // list styles whose level 1 is a number
    OfficeProps props;
    std::vector<std::string> bookmarks;  // named in the paragraph being read
    double default_pt = 12.0;

    void ReadStyles(const xml::xml_node &container) {
        for (const xml::xml_node &st : container.children()) {
            const std::string nm = st.name();
            if (nm == "text:list-style") {
                const xml::xml_node first = FirstElement(st);
                if (first && std::string(first.name()) == "text:list-level-style-number") numbered_lists.insert(Attr(st, "style:name"));
                continue;
            }
            if (nm == "style:default-style" && Attr(st, "style:family") == "paragraph") {
                const double pt = std::atof(Attr(st.child("style:text-properties"), "fo:font-size").c_str());
                if (pt > 0) default_pt = pt;
                continue;
            }
            if (nm != "style:style") continue;
            TextStyle ts;
            ts.parent = Attr(st, "style:parent-style-name");
            // Without a display name, the name is the display name with
            // spaces encoded (_20_) -- or, from some writers, as plain _.
            std::string disp = Attr(st, "style:display-name");
            if (disp.empty()) {
                disp = Attr(st, "style:name");
                for (size_t at = disp.find("_20_"); at != std::string::npos; at = disp.find("_20_")) disp.replace(at, 4, " ");
                for (char &ch : disp)
                    if (ch == '_') ch = ' ';
            }
            ts.display = Lower(disp);
            ts.outline = st.attribute("style:default-outline-level").as_int(0);
            const xml::xml_node tp = st.child("style:text-properties");
            if (tp) {
                ts.f.b = Attr(tp, "fo:font-weight") == "bold";
                ts.f.i = Attr(tp, "fo:font-style") == "italic";
                const std::string ul = Attr(tp, "style:text-underline-style");
                ts.f.u = !ul.empty() && ul != "none";
                const std::string lt = Attr(tp, "style:text-line-through-style");
                ts.f.s = !lt.empty() && lt != "none";
                const std::string pos = Attr(tp, "style:text-position");
                ts.f.sup = StartsWith(pos, "super") || (!pos.empty() && std::atof(pos.c_str()) > 0);
                ts.f.sub = StartsWith(pos, "sub") || (!pos.empty() && std::atof(pos.c_str()) < 0);
                const std::string bg = Attr(tp, "fo:background-color");
                ts.f.mark = !bg.empty() && bg != "transparent";
                const std::string col = Attr(tp, "fo:color");
                if (!col.empty() && Lower(col) != "#000000") ts.f.color = Lower(col);
                const std::string font = Lower(Attr(tp, "style:font-name") + " " + Attr(tp, "fo:font-family"));
                if (font.find("mono") != std::string::npos || font.find("courier") != std::string::npos) ts.f.mono = true;
                else {
                    std::string face = Attr(tp, "fo:font-family");
                    if (face.empty()) face = Attr(tp, "style:font-name");
                    if (face.size() >= 2 && (face[0] == '\'' || face[0] == '"')) face = face.substr(1, face.size() - 2);
                    ts.f.font = face;
                }
                const double pt = std::atof(Attr(tp, "fo:font-size").c_str());
                if (pt > 0 && std::abs(pt - default_pt) > 0.01) {
                    char buf[16];
                    std::snprintf(buf, sizeof buf, "%g", pt);
                    ts.f.size = buf;
                }
            }
            const xml::xml_node pp = st.child("style:paragraph-properties");
            if (pp) {
                const std::string al = Attr(pp, "fo:text-align");
                ts.center = al == "center";
                ts.right = al == "end" || al == "right";
                ts.left = al == "start" || al == "left";
            }
            styles[Attr(st, "style:name")] = ts;
        }
    }
    // The paragraph style's role, following parents.
    std::string Role(std::string name) {
        for (int g = 0; g < 8 && !name.empty(); ++g) {
            auto it = styles.find(name);
            const std::string d = it == styles.end() ? Lower(name) : it->second.display;
            if (StartsWith(d, "heading") || d == "title" || d == "subtitle" || d == "author" || d == "date" || d.find("preformatted") != std::string::npos ||
                d.find("quotation") != std::string::npos || d.find("quote") != std::string::npos || d == "caption" ||
                StartsWith(d, "contents") || d == "source text" || d == "code" || d == "math display" || d == "bibliography" || d == "rule" ||
                d == "horizontal line" || d == "abstract" || d == "abstract title")
                return d;
            if (it == styles.end()) return d;
            name = it->second.parent;
        }
        return "";
    }
    Fmt Apply(Fmt f, const std::string &style) {
        auto it = styles.find(style);
        if (it == styles.end()) return f;
        const Fmt &s = it->second.f;
        f.b = f.b || s.b;
        f.i = f.i || s.i;
        f.u = f.u || s.u;
        f.s = f.s || s.s;
        f.sup = f.sup || s.sup;
        f.sub = f.sub || s.sub;
        f.mark = f.mark || s.mark;
        f.mono = f.mono || s.mono;
        if (!s.color.empty()) f.color = s.color;
        if (!s.size.empty()) f.size = s.size;
        if (!s.font.empty()) f.font = s.font;
        return f;
    }

    std::vector<std::pair<std::string, std::string>> pending_images;
    void Runs(const xml::xml_node &parent, Fmt f, std::vector<Seg> &segs) {
        for (xml::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
            if (c.type() == xml::node_pcdata) {
                // ODF collapses runs of whitespace to one space.
                Seg sg;
                for (const char *p = c.value(); *p; ++p) {
                    if (IsSp(*p)) {
                        if (sg.text.empty() || sg.text.back() != ' ') sg.text += ' ';
                    } else {
                        sg.text += *p;
                    }
                }
                sg.f = f;
                segs.push_back(sg);
                continue;
            }
            const std::string nm = c.name();
            auto styled = styles.find(Attr(c, "text:style-name"));
            if (nm == "text:span" && styled != styles.end() && styled->second.display == "math") {
                Seg m;
                m.kind = Seg::Math;
                m.text = c.text().get();
                std::function<void(const xml::xml_node &)> all = [&](const xml::xml_node &n) {
                    for (xml::xml_node k = n.first_child(); k; k = k.next_sibling()) {
                        if (k.type() == xml::node_pcdata) m.text += k.value();
                        else if (std::string(k.name()) == "text:s") m.text += std::string(static_cast<size_t>(std::max(1, k.attribute("text:c").as_int(1))), ' ');
                        else all(k);
                    }
                };
                m.text.clear();
                all(c);
                segs.push_back(m);
            } else if (nm == "text:span" && styled != styles.end() && styled->second.display == "source text") {
                Seg code;
                code.kind = Seg::Code;
                code.text = c.text().get();
                segs.push_back(code);
            } else if (nm == "text:span") Runs(c, Apply(f, Attr(c, "text:style-name")), segs);
            else if (nm == "text:bookmark" || nm == "text:bookmark-start") bookmarks.push_back(Attr(c, "text:name"));
            else if (nm == "text:a" && StartsWith(Attr(c, "xlink:href"), "#ref_") &&
                     (Attr(c, "office:title") == "\\cite" || Attr(c, "office:title") == "\\citep")) {
                Seg cite;
                cite.kind = Attr(c, "office:title") == "\\citep" ? Seg::CiteP : Seg::Cite;
                cite.text = Attr(c, "xlink:href").substr(5);
                segs.push_back(cite);
            } else if (nm == "text:a") {
                Fmt lf = f;
                lf.link = Attr(c, "xlink:href");
                Runs(c, lf, segs);
            } else if (nm == "text:s" || nm == "text:tab") {
                Seg sp = TextSegs(nm == "text:tab" ? std::string(" ") : std::string(static_cast<size_t>(std::max(1, c.attribute("text:c").as_int(1))), ' '), f)[0];
                sp.hard = true;
                segs.push_back(sp);
            }
            else if (nm == "text:line-break") {
                Seg br;
                br.kind = Seg::Break;
                segs.push_back(br);
            } else if (nm == "text:note") {
                std::vector<Seg> body;
                const xml::xml_node nb = c.child("text:note-body");
                for (const xml::xml_node &p : nb.children()) {
                    std::vector<Seg> ps;
                    Runs(p, Fmt(), ps);
                    OdfSpaces(ps);
                    if (!body.empty() && !ps.empty()) body.push_back(TextSegs(" ")[0]);
                    body.insert(body.end(), ps.begin(), ps.end());
                }
                Coalesce(body);
                Seg fn;
                fn.kind = Seg::Footnote;
                std::string t = Trim(RenderSegs(body));
                for (char &ch : t)
                    if (ch == '\n') ch = ' ';
                fn.text = t;
                segs.push_back(fn);
            } else if (nm == "draw:frame" && c.child("draw:object")) {
                // An embedded formula: its MathML, as TeX.
                std::string obj = Attr(c.child("draw:object"), "xlink:href");
                if (StartsWith(obj, "./")) obj = obj.substr(2);
                if (!obj.empty() && obj.back() != '/') obj += "/";
                xml::xml_document formula;
                if (LoadXml(formula, ZipEntry(bytes, (obj + "content.xml").c_str()))) {
                    const xml::xml_node math = formula.child("math") ? formula.child("math") : formula.child("math:math");
                    Seg m;
                    m.kind = Attr(math, "display") == "block" ? Seg::DisplayMath : Seg::Math;
                    m.text = Trim(MathmlTex(math));
                    if (!m.text.empty()) segs.push_back(m);
                }
            } else if (nm == "draw:frame") {
                const xml::xml_node img = c.child("draw:image");
                const std::string href = Attr(img, "xlink:href");
                if (!href.empty()) {
                    const size_t dot = href.find_last_of('.');
                    const std::string data = ZipEntry(bytes, href.c_str());
                    // Not in the package: a linked picture, relative to the
                    // package itself (so ../ is the document's directory).
                    const std::string path = !data.empty() ? media.Save(data, dot == std::string::npos ? "png" : Lower(href.substr(dot + 1)))
                                             : StartsWith(href, "../") ? href.substr(3)
                                                                       : href;
                    std::string alt = c.child("svg:desc").text().get();
                    if (alt.empty()) alt = c.child("svg:title").text().get();
                    pending_images.push_back({path, alt});
                }
            } else if (nm == "text:soft-page-break" || nm == "text:bookmark-end") {
                continue;
            } else {
                Runs(c, f, segs);
            }
        }
    }

    void List(const xml::xml_node &list, int indent, bool ordered) {
        if (!Attr(list, "text:style-name").empty()) ordered = numbered_lists.count(Attr(list, "text:style-name")) > 0;
        int n = 0;
        for (const xml::xml_node &item : list.children("text:list-item")) {
            ++n;
            Out::Item it;
            it.indent = indent;
            it.ordered = ordered;
            it.number = n;
            std::vector<xml::xml_node> nested;
            for (const xml::xml_node &c : item.children()) {
                const std::string nm = c.name();
                if (nm == "text:list") nested.push_back(c);
                else if (nm == "text:p" || nm == "text:h") {
                    std::vector<Seg> p;
                    Runs(c, Fmt(), p);
                    OdfSpaces(p);
                    if (!it.segs.empty() && !p.empty()) it.segs.push_back(TextSegs(" ")[0]);
                    it.segs.insert(it.segs.end(), p.begin(), p.end());
                }
            }
            if (!Trim(Assembler::Plain(it.segs)).empty()) as.Item(it);
            for (const xml::xml_node &sub : nested) List(sub, indent + 2, ordered);
        }
    }

    void Body(const xml::xml_node &body) {
        for (const xml::xml_node &c : body.children()) {
            const std::string nm = c.name();
            if (nm == "text:h" || nm == "text:p") {
                pending_images.clear();
                bookmarks.clear();
                std::vector<Seg> segs;
                const std::string style = Attr(c, "text:style-name");
                const std::string role = Role(style);
                int level = nm == "text:h" ? c.attribute("text:outline-level").as_int(1) : 0;
                ParaKind kind = ParaKind::Body;
                if (role == "title") kind = ParaKind::Title;
                else if (role == "subtitle") kind = ParaKind::Subtitle;
                else if (role == "author") kind = ParaKind::Author;
                else if (role == "date") kind = ParaKind::Date;
                else if (role == "contents heading") kind = ParaKind::TocHeading;
                else if (role == "abstract title") kind = ParaKind::AbstractTitle;
                else if (role == "abstract") kind = ParaKind::Abstract;
                else if (StartsWith(role, "contents")) kind = ParaKind::Toc;
                else if (nm == "text:h" || StartsWith(role, "heading")) {
                    kind = ParaKind::Heading;
                    if (level == 0) level = std::max(1, std::atoi(role.c_str() + 7));
                } else if (role.find("preformatted") != std::string::npos || role == "source text" || role == "code") kind = ParaKind::Code;
                else if (role.find("quot") != std::string::npos) kind = ParaKind::Quote;
                else if (role == "caption") kind = ParaKind::Caption;
                else if (role == "math display") kind = ParaKind::MathDisplay;
                else if (role == "bibliography") kind = ParaKind::Bibliography;
                else if (role == "rule" || role == "horizontal line") kind = ParaKind::Rule;
                // A role's own look (a heading's size, a caption's italics)
                // is the role's, not the text's.
                Runs(c, kind == ParaKind::Body ? Apply(Fmt(), style) : Fmt(), segs);
                OdfSpaces(segs);
                if (Marks(bookmarks, props, as)) continue;
                bool gray = !segs.empty();
                for (const Seg &sg : segs) gray = gray && sg.f.color == "#555555";
                if (kind == ParaKind::Caption && std::string(NextElement(c).name()) == "table:table") as.HoldCaption(segs);
                else as.Para(kind, level, segs, gray);
                for (const auto &img : pending_images)
                    if (!img.first.empty()) as.Image(img.first, img.second);
            } else if (nm == "text:list") {
                as.FlushCode();
                List(c, 0, false);
            } else if (nm == "table:table") {
                std::vector<std::vector<std::vector<Seg>>> rows;
                std::vector<Align> aligns;
                int header = 0;
                std::function<void(const xml::xml_node &, bool)> rows_of = [&](const xml::xml_node &n, bool head) {
                    for (const xml::xml_node &r : n.children()) {
                        const std::string rn = r.name();
                        if (rn == "table:table-header-rows") rows_of(r, true);
                        else if (rn == "table:table-rows") rows_of(r, head);
                        else if (rn == "table:table-row") {
                            std::vector<std::vector<Seg>> row;
                            size_t col = 0;
                            for (const xml::xml_node &cell : r.children("table:table-cell")) {
                                std::vector<Seg> segs;
                                for (const xml::xml_node &p : cell.children()) {
                                    std::vector<Seg> ps;
                                    Runs(p, Apply(Fmt(), Attr(p, "text:style-name")), ps);
                                    OdfSpaces(ps);
                                    if (!segs.empty() && !ps.empty()) segs.push_back(TextSegs(" ")[0]);
                                    segs.insert(segs.end(), ps.begin(), ps.end());
                                    auto it = styles.find(Attr(p, "text:style-name"));
                                    if (aligns.size() <= col) aligns.resize(col + 1, Align::Default);
                                    if (it != styles.end() && it->second.center) aligns[col] = Align::Center;
                                    if (it != styles.end() && it->second.right) aligns[col] = Align::Right;
                                    if (it != styles.end() && it->second.left) aligns[col] = Align::Left;
                                }
                                if (head)
                                    for (Seg &sg : segs) sg.f.b = false;  // header rows are bold by their role
                                row.push_back(segs);
                                ++col;
                            }
                            if (head) ++header;
                            rows.push_back(row);
                        }
                    }
                };
                rows_of(c, false);
                as.Table(rows, header, aligns);
            } else if (nm == "text:section" || nm == "text:index-body") {
                Body(c);
            } else if (nm == "text:table-of-content") {
                as.Flush();
                as.out.Block("\\toc");
            }
        }
    }

    bool Read(std::string *out, std::string *error) {
        xml::xml_document content, styles_doc, meta;
        if (!LoadXml(content, ZipEntry(bytes, "content.xml"))) {
            if (error) *error = "not an .odt (no readable content.xml)";
            return false;
        }
        if (LoadXml(styles_doc, ZipEntry(bytes, "styles.xml"))) ReadStyles(styles_doc.child("office:document-styles").child("office:styles"));
        const xml::xml_node root = content.child("office:document-content");
        ReadStyles(root.child("office:automatic-styles"));
        const bool have_meta = LoadXml(meta, ZipEntry(bytes, "meta.xml"));
        const xml::xml_node m = meta.child("office:document-meta").child("office:meta");
        if (have_meta)
            for (const xml::xml_node &u : m.children("meta:user-defined")) props.Set(Attr(u, "meta:name"), u.text().get());
        Body(root.child("office:body").child("office:text"));
        if (have_meta) {
            const std::string title = Trim(m.child("dc:title").text().get());
            bool has = false;
            for (const std::string &l : as.out.meta) has = has || StartsWith(l, "//? Title:");
            if (!has && !title.empty()) as.out.meta.insert(as.out.meta.begin(), "//? Title: " + title);
        }
        props.Apply(as.out, &as.citations);
        *out = as.Str();
        return true;
    }
};

}  // namespace

// ===========================================================================
// RTF
// ===========================================================================

namespace {

bool SameFmt(const Fmt &a, const Fmt &b) {
    return a.b == b.b && a.i == b.i && a.u == b.u && a.s == b.s && a.sup == b.sup && a.sub == b.sub && a.small == b.small &&
           a.big == b.big && a.mono == b.mono && a.mark == b.mark && a.ins == b.ins && a.del == b.del && a.color == b.color &&
           a.font == b.font && a.size == b.size && a.link == b.link;
}

// A styled paragraph's own look -- a heading's bold and size, a caption's
// italics -- is the style's, not the text's: formatting shared by every
// piece of text in it is dropped.
void StripUniform(std::vector<Seg> &segs) {
    std::vector<Seg *> text;
    for (Seg &sg : segs)
        if (sg.kind == Seg::Text && !Trim(sg.text).empty()) text.push_back(&sg);
    if (text.empty()) return;
    auto all = [&](auto get) {
        for (Seg *s : text)
            if (!get(s->f)) return false;
        return true;
    };
    const Fmt first = text[0]->f;
    const bool b = all([](const Fmt &f) { return f.b; }), i = all([](const Fmt &f) { return f.i; });
    const bool mono = all([](const Fmt &f) { return f.mono; });
    const bool size = all([&](const Fmt &f) { return f.size == first.size; });
    const bool color = all([&](const Fmt &f) { return f.color == first.color; });
    const bool font = all([&](const Fmt &f) { return f.font == first.font; });
    for (Seg &sg : segs) {
        if (b) sg.f.b = false;
        if (i) sg.f.i = false;
        if (mono) sg.f.mono = false;
        if (size) sg.f.size.clear();
        if (color) sg.f.color.clear();
        if (font) sg.f.font.clear();
    }
}

// A paragraph that is only a line: three or more dashes, rules or underscores.
bool IsTextRule(const std::string &t) {
    int n = 0;
    for (size_t i = 0; i < t.size();) {
        if (t.compare(i, 3, "\u2014") == 0 || t.compare(i, 3, "\u2013") == 0 || t.compare(i, 3, "\u2500") == 0) i += 3;
        else if (t[i] == '-' || t[i] == '_') ++i;
        else return false;
        ++n;
    }
    return n >= 3;
}

struct RtfReader {
    enum Dest { Normal, Skip, FontTbl, ColorTbl, StyleSheet, Title, Footnote, FldInst, ListText, Pict, Bookmark, PropName, StaticVal, Sn, Sv };
    struct State {
        Fmt f;
        int fs = 24, cf = 0, font = 0, cs = -1;
        Dest dest = Normal;
        std::string link;
    };
    std::vector<State> stack;
    State st;
    std::map<int, std::string> fonts;
    std::vector<std::string> colors;  // index 0 = auto
    std::map<int, std::string> styles;  // \sN and \csN -> lower-case name
    std::string info_title;
    bool saw_title = false;
    Media media;
    Assembler as;
    OfficeProps props;

    // The paragraph being built.
    std::vector<Seg> para;
    int outline = -1, style = 0, ilvl = 0, fi = 0, li = 0;
    bool intbl = false, border = false;
    Align align = Align::Default;
    std::string list_text;
    std::vector<std::string> bookmarks;
    std::vector<std::pair<std::string, std::string>> images;  // path, alt
    // A table being built.
    std::vector<std::vector<std::vector<Seg>>> rows;
    std::vector<std::vector<Seg>> row;
    std::vector<Align> aligns;
    int header_rows = 0;
    bool row_header = false;
    // Footnote capture: the paragraph under construction is swapped out.
    std::vector<std::vector<Seg>> saved;
    std::string fldinst;
    int uc = 1;
    bool include_alt = false;
    int default_fs = 24;  // the Normal style's size, else RTF's own default

    // Destination text.
    std::string dest_text, color_cur, pict_hex, pict_ext, pict_alt, sn, prop_name;
    int font_cur = 0, style_cur = 0;

    Fmt Current() const {
        Fmt f = st.f;
        if (st.cf > 0 && st.cf < static_cast<int>(colors.size())) f.color = colors[static_cast<size_t>(st.cf)];
        if (f.color == "#000000") f.color.clear();
        auto fit = fonts.find(st.font);
        if (fit != fonts.end() && st.font != 0) {
            const std::string fl = Lower(fit->second);
            if (fl.find("courier") != std::string::npos || fl.find("mono") != std::string::npos || fl.find("consolas") != std::string::npos)
                f.mono = true;
            else
                f.font = fit->second;
        }
        if (st.fs != default_fs && st.fs > 0) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%g", st.fs / 2.0);
            f.size = buf;
        }
        if (!st.link.empty()) {
            // A hyperlink's underline and blue are the link's own look.
            f.link = st.link;
            f.u = false;
            if (f.color == "#1a5fb4" || f.color == "#0000ff" || f.color == "#0563c1") f.color.clear();
        }
        return f;
    }
    std::string CharStyle() const {
        auto it = styles.find(st.cs);
        return it == styles.end() ? "" : it->second;
    }

    void AddText(const std::string &t) {
        switch (st.dest) {
            case Normal:
            case Footnote: {
                if (t.empty()) return;
                const std::string cs = CharStyle();
                Seg::Kind kind = cs == "math" ? Seg::Math : cs == "verbatim char" ? Seg::Code : Seg::Text;
                const Fmt f = kind == Seg::Text ? Current() : Fmt();
                if (!para.empty() && para.back().kind == kind && (kind != Seg::Text || SameFmt(para.back().f, f))) {
                    para.back().text += t;
                } else {
                    Seg s;
                    s.kind = kind;
                    s.text = t;
                    s.f = f;
                    para.push_back(s);
                }
                break;
            }
            case ListText: list_text += t; break;
            case FldInst: fldinst += t; break;
            case Pict: pict_hex += t; break;
            case FontTbl:
            case StyleSheet:
            case Title:
            case Bookmark:
            case PropName:
            case StaticVal:
            case Sn:
            case Sv: dest_text += t; break;
            case ColorTbl:
            case Skip: break;
        }
    }

    ParaKind Kind(int *level) const {
        *level = outline >= 0 ? outline + 1 : 0;
        auto sit = styles.find(style);
        const std::string n = sit == styles.end() ? "" : sit->second;
        if (StartsWith(n, "heading ")) {
            *level = std::max(1, std::atoi(n.c_str() + 8));
            return ParaKind::Heading;
        }
        if (n == "title") return ParaKind::Title;
        if (n == "subtitle") return ParaKind::Subtitle;
        if (n == "author") return ParaKind::Author;
        if (n == "date") return ParaKind::Date;
        if (n == "toc heading") return ParaKind::TocHeading;
        if (StartsWith(n, "toc ")) return ParaKind::Toc;
        if (n == "abstract title") return ParaKind::AbstractTitle;
        if (n == "abstract") return ParaKind::Abstract;
        if (n == "source code" || n.find("preformatted") != std::string::npos || n == "verbatim") return ParaKind::Code;
        if (n == "quote" || n == "block text") return ParaKind::Quote;
        if (n == "caption") return ParaKind::Caption;
        if (n == "bibliography") return ParaKind::Bibliography;
        if (n == "math display") return ParaKind::MathDisplay;
        if (*level > 0 && *level <= 6) return ParaKind::Heading;
        return ParaKind::Body;
    }

    void EndPara() {
        if (intbl) return;  // cell paragraphs are joined by \cell
        FlushTable();
        std::vector<Seg> p;
        p.swap(para);
        std::vector<std::string> marks;
        marks.swap(bookmarks);
        std::vector<std::pair<std::string, std::string>> imgs;
        imgs.swap(images);
        const std::string lt = Trim(list_text);
        list_text.clear();
        if (Marks(marks, props, as)) return;
        int level = 0;
        ParaKind kind = Kind(&level);
        const bool empty = Trim(Assembler::Plain(p)).empty();
        // Without list tables or styles (pandoc's RTF, say): a hanging
        // paragraph that starts with a bullet or a number is a list item,
        // and one set entirely in a monospaced font is code.
        std::string lead_marker;
        if (lt.empty() && fi < 0 && kind == ParaKind::Body && !p.empty() && p[0].kind == Seg::Text) {
            const std::string &t = p[0].text;
            size_t d = 0;
            while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d]))) ++d;
            static const char *kBullets[] = {"\u2022 ", "\u25e6 ", "\u25aa ", "\u2013 ", "- ", "* "};
            for (const char *b : kBullets)
                if (StartsWith(t, b)) lead_marker = b;
            if (lead_marker.empty() && d > 0 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') lead_marker = t.substr(0, d + 2);
            if (!lead_marker.empty()) {
                p[0].text = t.substr(lead_marker.size());
                ilvl = std::max(0, li / 360 - 1);
            }
        }
        if (kind == ParaKind::Body && lt.empty() && lead_marker.empty() && !empty) {
            bool mono = true;
            for (const Seg &sg : p) mono = mono && (sg.kind != Seg::Text || Trim(sg.text).empty() || sg.f.mono);
            if (mono) kind = ParaKind::Code;
        }
        const std::string marker = lt.empty() ? Trim(lead_marker) : lt;
        if (!marker.empty() && !empty) {
            Out::Item it;
            it.ordered = std::isdigit(static_cast<unsigned char>(marker[0])) != 0;
            it.indent = 2 * ilvl;
            int count = 0;
            for (const Out::Item &prev : as.list)
                if (prev.indent == it.indent && prev.ordered == it.ordered) ++count;
            it.number = count + 1;
            // A task box written as the item's first character.
            it.segs = p;
            as.Item(it);
        } else if ((border && empty && imgs.empty()) || IsTextRule(Trim(Assembler::Plain(p)))) {
            as.Para(ParaKind::Rule, 0, {}, false);
        } else {
            if (kind == ParaKind::Title) saw_title = true;
            bool gray = !p.empty();
            for (const Seg &sg : p) gray = gray && (sg.kind != Seg::Text || sg.f.color == "#555555");
            if (kind != ParaKind::Body) StripUniform(p);
            if (kind == ParaKind::Code)
                for (Seg &sg : p) sg.f.color.clear();
            as.Para(kind, level, p, gray);
        }
        for (const auto &im : imgs)
            if (!im.first.empty()) as.Image(im.first, im.second);
    }
    void FlushTable() {
        if (!row.empty()) {
            rows.push_back(row);
            row.clear();
        }
        if (rows.empty()) return;
        for (size_t r = 0; r < rows.size() && static_cast<int>(r) < header_rows; ++r)
            for (auto &c : rows[r])
                for (Seg &sg : c) sg.f.b = false;  // a header row's bold is its role's
        as.Table(rows, header_rows, aligns);
        rows.clear();
        aligns.clear();
        header_rows = 0;
    }

    void EndPict() {
        std::string bytes;
        int hi = -1;
        for (char ch : pict_hex) {
            int v = -1;
            if (ch >= '0' && ch <= '9') v = ch - '0';
            else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else {
                bytes += static_cast<char>(hi * 16 + v);
                hi = -1;
            }
        }
        if (!pict_ext.empty() && !bytes.empty() && !media.dir.empty()) images.push_back({media.Save(bytes, pict_ext), pict_alt});
        pict_hex.clear();
        pict_ext.clear();
        pict_alt.clear();
    }

    // \field's instruction: a hyperlink (url, or \l anchor with an \o tip).
    void StartFieldResult() {
        const size_t inc = fldinst.find("INCLUDEPICTURE");
        if (inc != std::string::npos) {
            const size_t q1 = fldinst.find('"', inc);
            const size_t q2 = q1 == std::string::npos ? q1 : fldinst.find('"', q1 + 1);
            if (q2 != std::string::npos) {
                pict_alt.clear();
                images.push_back({fldinst.substr(q1 + 1, q2 - q1 - 1), ""});
                include_alt = true;  // the result text is the picture's description
                st.dest = Title;
                dest_text.clear();
            }
            return;
        }
        std::vector<std::string> quoted;
        bool anchor = false, tip_next = false;
        std::string tip;
        const size_t h = fldinst.find("HYPERLINK");
        if (h == std::string::npos) return;
        for (size_t k = h + 9; k < fldinst.size(); ++k) {
            if (fldinst[k] == '\\' && k + 1 < fldinst.size()) {
                if (fldinst[k + 1] == 'l') anchor = true;
                if (fldinst[k + 1] == 'o') tip_next = true;
                ++k;
            } else if (fldinst[k] == '"') {
                const size_t close = fldinst.find('"', k + 1);
                if (close == std::string::npos) break;
                const std::string q = fldinst.substr(k + 1, close - k - 1);
                if (tip_next) {
                    tip = q;
                    tip_next = false;
                } else {
                    quoted.push_back(q);
                }
                k = close;
            }
        }
        if (quoted.empty()) return;
        if (anchor && StartsWith(quoted[0], "ref_") && (tip == "\\cite" || tip == "\\citep")) {
            // mep's own citation: the label is regenerated, so the result is skipped.
            Seg cite;
            cite.kind = tip == "\\citep" ? Seg::CiteP : Seg::Cite;
            cite.text = quoted[0].substr(4);
            para.push_back(cite);
            st.dest = Skip;
            return;
        }
        st.link = anchor ? "#" + quoted[0] : quoted[0];
    }

    void Word(const std::string &w, bool has_param, int param) {
        const bool on = !has_param || param != 0;
        if (st.dest == Pict) {
            if (w == "pngblip") pict_ext = "png";
            else if (w == "jpegblip") pict_ext = "jpg";
            else if (w == "bin") st.dest = Skip;
            return;
        }
        if (w == "par") {
            if (st.dest == Footnote) {
                Seg br;
                br.kind = Seg::Break;
                para.push_back(br);
            } else if (st.dest == Normal) {
                EndPara();
            }
        } else if (w == "line") {
            if (st.dest == Normal || st.dest == Footnote) {
                Seg br;
                br.kind = Seg::Break;
                para.push_back(br);
            }
        } else if (w == "tab") {
            AddText(st.dest == ListText ? "" : " ");
        } else if (w == "cell") {
            if (st.dest != Normal) return;
            if (aligns.size() <= row.size()) aligns.resize(row.size() + 1, Align::Default);
            if (align != Align::Default) aligns[row.size()] = align;
            row.push_back(para);
            para.clear();
        } else if (w == "row") {
            if (st.dest != Normal) return;
            rows.push_back(row);
            row.clear();
            para.clear();
            if (row_header && static_cast<int>(rows.size()) == header_rows + 1) ++header_rows;
        } else if (w == "intbl") {
            intbl = true;
        } else if (w == "trowd") {
            intbl = true;
            row_header = false;
        } else if (w == "trhdr") {
            row_header = true;
        } else if (w == "pard") {
            if (st.dest == Normal) {
                intbl = false;
                outline = -1;
                style = 0;
                ilvl = 0;
                fi = li = 0;
                border = false;
                align = Align::Default;
            }
        } else if (w == "plain") {
            st.f = Fmt();
            st.fs = 24;
            st.cf = 0;
            st.font = 0;
            st.cs = -1;
        } else if (w == "b") st.f.b = on;
        else if (w == "i") st.f.i = on;
        else if (w == "ul" || w == "uld" || w == "ulw" || w == "uldb") st.f.u = on;
        else if (w == "ulnone") st.f.u = false;
        else if (w == "strike" || w == "striked") st.f.s = on;
        else if (w == "super") { st.f.sup = on; st.f.sub = false; }
        else if (w == "sub") { st.f.sub = on; st.f.sup = false; }
        else if (w == "nosupersub") st.f.sup = st.f.sub = false;
        else if (w == "fs") {
            if (st.dest == StyleSheet) {
                if (style_cur == 0) default_fs = param;
            } else {
                st.fs = param;
            }
        } else if (w == "cf") st.cf = param;
        else if (w == "highlight") st.f.mark = param != 0;
        else if (w == "f") {
            if (st.dest == FontTbl) font_cur = param;
            else st.font = param;
        } else if (w == "s") {
            if (st.dest == StyleSheet) style_cur = param;
            else if (st.dest == Normal) style = param;
        } else if (w == "cs") {
            if (st.dest == StyleSheet) style_cur = param;
            else st.cs = param;
        } else if (w == "outlinelevel") {
            if (st.dest == Normal) outline = param;
        } else if (w == "ilvl") ilvl = param;
        else if (w == "fi") fi = param;
        else if (w == "li") li = param;
        else if (w == "brdrb") border = true;
        else if (w == "ql") align = Align::Left;
        else if (w == "qc") align = Align::Center;
        else if (w == "qr") align = Align::Right;
        else if (w == "uc") uc = param;
        else if (w == "red" || w == "green" || w == "blue") {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%02x", param & 255);
            color_cur += buf;
        } else if (w == "fonttbl") st.dest = FontTbl;
        else if (w == "colortbl") st.dest = ColorTbl;
        else if (w == "stylesheet") st.dest = StyleSheet;
        else if (w == "title" && st.dest == Skip) {
            st.dest = Title;
            dest_text.clear();
        } else if (w == "info") st.dest = Skip;
        else if (w == "footnote") {
            saved.push_back(para);
            para.clear();
            st.dest = Footnote;
        } else if (w == "fldinst") {
            st.dest = FldInst;
            fldinst.clear();
        } else if (w == "fldrslt") {
            st.dest = saved.empty() ? Normal : Footnote;
            StartFieldResult();
        } else if (w == "listtext" || w == "pntext") {
            st.dest = ListText;
            list_text.clear();
        } else if (w == "pict") {
            st.dest = Pict;
            pict_hex.clear();
        } else if (w == "bkmkstart") {
            st.dest = Bookmark;
            dest_text.clear();
        } else if (w == "propname") {
            st.dest = PropName;
            dest_text.clear();
        } else if (w == "staticval") {
            st.dest = StaticVal;
            dest_text.clear();
        } else if (w == "sn") {
            st.dest = Sn;
            dest_text.clear();
        } else if (w == "sv") {
            st.dest = Sv;
            dest_text.clear();
        } else if (w == "header" || w == "footer" || w == "headerl" || w == "headerr" || w == "footerl" || w == "footerr" || w == "object" ||
                   w == "themedata" || w == "colorschememapping" || w == "latentstyles" || w == "datastore" || w == "xmlnstbl" ||
                   w == "listtable" || w == "listoverridetable" || w == "rsidtbl" || w == "generator" || w == "pgdsctbl" ||
                   w == "nonshppict" || w == "bkmkend" || w == "pn" || w == "pnseclvl") {
            st.dest = Skip;
        } else if (w == "emdash") AddText("—");
        else if (w == "endash") AddText("–");
        else if (w == "bullet") AddText("•");
        else if (w == "lquote") AddText("‘");
        else if (w == "rquote") AddText("’");
        else if (w == "ldblquote") AddText("“");
        else if (w == "rdblquote") AddText("”");
    }

    // A group closes: whatever its destination collected is filed.
    void EndGroup(Dest ending, const State &parent) {
        std::string name = Trim(dest_text);
        switch (ending) {
            case FontTbl:
                if (!name.empty()) {
                    if (name.back() == ';') name.pop_back();
                    fonts[font_cur] = Trim(name);
                    dest_text.clear();
                }
                break;
            case StyleSheet:
                if (!name.empty() && parent.dest == StyleSheet) {
                    if (name.back() == ';') name.pop_back();
                    styles[style_cur] = Lower(Trim(name));
                    dest_text.clear();
                }
                break;
            case Title:
                if (include_alt && parent.dest != Title) {
                    if (!images.empty()) images.back().second = name;
                    include_alt = false;
                } else {
                    info_title = name;
                }
                break;
            case Bookmark:
                if (parent.dest != Bookmark) bookmarks.push_back(name);
                break;
            case PropName:
                if (parent.dest != PropName) prop_name = name;
                break;
            case StaticVal:
                if (parent.dest != StaticVal) props.Set(prop_name, dest_text);
                break;
            case Sn:
                if (parent.dest != Sn) sn = name;
                break;
            case Sv:
                if (parent.dest != Sv && sn == "wzDescription") pict_alt = dest_text;
                break;
            case Pict:
                if (parent.dest != Pict) EndPict();
                break;
            case Footnote:
                if (parent.dest != Footnote) {
                    std::vector<Seg> fn;
                    fn.swap(para);
                    para = saved.empty() ? std::vector<Seg>() : saved.back();
                    if (!saved.empty()) saved.pop_back();
                    StripUniform(fn);  // the note's small type is the note's
                    OfficeFmt(fn);
                    Coalesce(fn);
                    std::string t = Trim(RenderSegs(fn));
                    for (char &ch : t)
                        if (ch == '\n') ch = ' ';
                    Seg s;
                    s.kind = Seg::Footnote;
                    s.text = t;
                    para.push_back(s);
                }
                break;
            default: break;
        }
    }

    static std::string Utf8(unsigned cp) {
        std::string o;
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) {
            o += static_cast<char>(0xC0 | (cp >> 6));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return o;
    }
    static unsigned Cp1252(unsigned char c) {
        static const unsigned kHigh[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160,
                                           0x2039, 0x152, 0x8D, 0x17D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                           0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
        return c >= 0x80 && c < 0xA0 ? kHigh[c - 0x80] : c;
    }

    std::string Read(const std::string &rtf) {
        size_t i = 0;
        const size_t n = rtf.size();
        int skip_chars = 0;
        while (i < n) {
            const char c = rtf[i];
            if (c == '{') {
                stack.push_back(st);
                if (st.dest == StyleSheet) style_cur = 0;  // a style with no \sN is Normal
                ++i;
                // {\* ...} is an optional destination: skipped unless known.
                if (i + 1 < n && rtf[i] == '\\' && rtf[i + 1] == '*') {
                    size_t j = i + 2;
                    while (j < n && IsSp(rtf[j])) ++j;
                    std::string w;
                    if (j < n && rtf[j] == '\\') {
                        size_t k = j + 1;
                        while (k < n && std::isalpha(static_cast<unsigned char>(rtf[k]))) w += rtf[k++];
                    }
                    static const char *kKnown[] = {"footnote", "fldinst", "bkmkstart", "userprops", "picprop", "shppict"};
                    bool known = false;
                    for (const char *k : kKnown) known = known || w == k;
                    if (w == "cs" && st.dest == StyleSheet) known = true;
                    if (!known) st.dest = Skip;
                    i += 2;
                }
                continue;
            }
            if (c == '}') {
                const Dest ending = st.dest;
                const State parent = stack.empty() ? State() : stack.back();
                if (!stack.empty()) stack.pop_back();
                EndGroup(ending, parent);
                st = parent;
                ++i;
                continue;
            }
            if (c == '\\') {
                ++i;
                if (i >= n) break;
                const char d = rtf[i];
                if (d == '\\' || d == '{' || d == '}') {
                    if (skip_chars > 0) --skip_chars;
                    else AddText(std::string(1, d));
                    ++i;
                    continue;
                }
                if (d == '\'') {
                    const unsigned v = static_cast<unsigned>(std::strtoul(rtf.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 3;
                    if (skip_chars > 0) {
                        --skip_chars;
                        continue;
                    }
                    AddText(Utf8(Cp1252(static_cast<unsigned char>(v))));
                    continue;
                }
                if (d == '~') { AddText(" "); ++i; continue; }
                if (d == '_') { AddText("-"); ++i; continue; }
                if (d == '-' || d == '*') { ++i; continue; }
                if (d == '\n' || d == '\r') { Word("par", false, 0); ++i; continue; }
                if (!std::isalpha(static_cast<unsigned char>(d))) { ++i; continue; }
                std::string w;
                while (i < n && std::isalpha(static_cast<unsigned char>(rtf[i]))) w += rtf[i++];
                bool has_param = false;
                int param = 0;
                bool neg = false;
                if (i < n && rtf[i] == '-') {
                    neg = true;
                    ++i;
                }
                std::string num;
                while (i < n && std::isdigit(static_cast<unsigned char>(rtf[i]))) num += rtf[i++];
                if (!num.empty()) {
                    has_param = true;
                    param = std::atoi(num.substr(0, 9).c_str()) * (neg ? -1 : 1);
                }
                if (i < n && rtf[i] == ' ') ++i;  // the delimiter space
                if (w == "u" && has_param) {
                    AddText(Utf8(static_cast<unsigned>(param < 0 ? param + 65536 : param)));
                    skip_chars = uc;
                    continue;
                }
                if (w == "chftn") continue;
                Word(w, has_param, param);
                continue;
            }
            if (c == '\n' || c == '\r') {
                ++i;
                continue;
            }
            if (st.dest == ColorTbl && c == ';') {
                colors.push_back(color_cur.empty() ? "" : "#" + color_cur);
                color_cur.clear();
                ++i;
                continue;
            }
            if (skip_chars > 0) {
                --skip_chars;
                ++i;
                continue;
            }
            // Plain text run.
            size_t j = i;
            while (j < n && rtf[j] != '\\' && rtf[j] != '{' && rtf[j] != '}' && rtf[j] != '\n' && rtf[j] != '\r' &&
                   !(st.dest == ColorTbl && rtf[j] == ';'))
                ++j;
            std::string utf;
            for (size_t k = i; k < j; ++k) utf += Utf8(Cp1252(static_cast<unsigned char>(rtf[k])));
            AddText(utf);
            i = j;
        }
        intbl = false;
        EndPara();
        FlushTable();
        if (!saw_title && !info_title.empty()) as.out.meta.insert(as.out.meta.begin(), "//? Title: " + info_title);
        props.Apply(as.out, &as.citations);
        return as.Str();
    }
};

}  // namespace

std::string FromRtf(const std::string &rtf) {
    RtfReader r;
    return r.Read(rtf);
}

// ===========================================================================
// Files
// ===========================================================================

bool ImportFile(const std::string &in_path, const std::string &out_path, std::string *mepml, std::string *error) {
    std::ifstream f(in_path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot read " + in_path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string bytes = ss.str();
    switch (FormatFromPath(in_path)) {
        case Format::Html: *mepml = FromHtml(bytes); return true;
        case Format::Markdown: *mepml = FromMarkdown(bytes); return true;
        case Format::Org: *mepml = FromOrg(bytes); return true;
        case Format::Rtf: {
            // Pictures are written out beside the result, as for DOCX/ODT.
            std::filesystem::path out(out_path);
            const std::string stem = out.stem().string();
            const std::string dir = out.has_parent_path() ? out.parent_path().string() : ".";
            RtfReader r;
            r.media = Media{dir + "/" + stem + "_media", stem + "_media", 0};
            *mepml = r.Read(bytes);
            return true;
        }
        case Format::Docx:
        case Format::Odt: {
            std::filesystem::path out(out_path);
            const std::string stem = out.stem().string();
            const std::string dir = out.has_parent_path() ? out.parent_path().string() : ".";
            // mep's own readers, which understand styles, lists, links,
            // footnotes and images; the office-document model (FromOffice)
            // is the fallback for a package they cannot make sense of.
            const bool odt = FormatFromPath(in_path) == Format::Odt;
            Media media{dir + "/" + stem + "_media", stem + "_media", 0};
            if (odt) {
                OdtReader r;
                r.bytes = bytes;
                r.media = media;
                if (r.Read(mepml, error)) return true;
            } else {
                DocxReader r;
                r.bytes = bytes;
                r.media = media;
                if (r.Read(mepml, error)) return true;
            }
            return FromOffice(bytes, odt, dir + "/" + stem + "_media", stem + "_media", mepml, error);
        }
        case Format::Text: {
            Out out;
            std::string para;
            for (const std::string &l : SplitLines(bytes + "\n")) {
                if (Trim(l).empty()) {
                    if (!para.empty()) out.Paragraph(TextSegs(para));
                    para.clear();
                } else {
                    para += (para.empty() ? "" : "\n") + l;
                }
            }
            if (!para.empty()) out.Paragraph(TextSegs(para));
            *mepml = out.Str();
            return true;
        }
        default:
            if (error) *error = "cannot import " + in_path;
            return false;
    }
}

}  // namespace mepml
