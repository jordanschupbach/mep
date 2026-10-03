#include "mepml_style.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_map>

#include "mepml_default_style.h"
#include "mepml_doc.h"

namespace mepml::style {

namespace {

bool IsNameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; }
bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::string Trim(const std::string &s) {
    size_t a = 0, z = s.size();
    while (a < z && IsSpace(s[a])) ++a;
    while (z > a && IsSpace(s[z - 1])) --z;
    return s.substr(a, z - a);
}

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void AppendUtf8(std::string *out, unsigned long cp) {
    if (cp < 0x80) {
        *out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *out += static_cast<char>(0xC0 | (cp >> 6));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += static_cast<char>(0xE0 | (cp >> 12));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        *out += static_cast<char>(0xF0 | (cp >> 18));
        *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// A quoted string at s[*i] (its opening quote): its text with the escapes
// resolved, *i left after the closing quote. False when it never closes.
bool ReadString(const std::string &s, size_t *i, std::string *out) {
    const char quote = s[*i];
    size_t p = *i + 1;
    std::string text;
    while (p < s.size() && s[p] != quote) {
        if (s[p] == '\n') return false;
        if (s[p] != '\\') {
            text += s[p++];
            continue;
        }
        ++p;
        if (p >= s.size()) return false;
        if (std::isxdigit(static_cast<unsigned char>(s[p]))) {
            size_t q = p;
            while (q < s.size() && q - p < 6 && std::isxdigit(static_cast<unsigned char>(s[q]))) ++q;
            AppendUtf8(&text, std::strtoul(s.substr(p, q - p).c_str(), nullptr, 16));
            if (q < s.size() && s[q] == ' ') ++q;  // the space that ends an escape
            p = q;
        } else {
            text += s[p++];
        }
    }
    if (p >= s.size()) return false;
    *i = p + 1;
    *out = std::move(text);
    return true;
}

// The end of the bracketed group opening at s[open] (`(` or `[`): the index
// just past its closer, or npos. Strings inside are skipped.
size_t GroupEnd(const std::string &s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"' || c == '\'') {
            std::string ignored;
            size_t j = i;
            if (!ReadString(s, &j, &ignored)) return std::string::npos;
            i = j - 1;
        } else if (c == '(' || c == '[') {
            ++depth;
        } else if (c == ')' || c == ']') {
            if (--depth == 0) return i + 1;
        }
    }
    return std::string::npos;
}

// `text` split at the top-level `sep`s (outside strings and brackets).
std::vector<std::pair<size_t, std::string>> SplitTop(const std::string &text, char sep) {
    std::vector<std::pair<size_t, std::string>> out;
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || (text[i] == sep && depth == 0)) {
            out.emplace_back(start, text.substr(start, i - start));
            start = i + 1;
            continue;
        }
        const char c = text[i];
        if (c == '"' || c == '\'') {
            std::string ignored;
            size_t j = i;
            if (!ReadString(text, &j, &ignored)) {
                out.emplace_back(start, text.substr(start));
                return out;
            }
            i = j - 1;
        } else if (c == '(' || c == '[') {
            ++depth;
        } else if ((c == ')' || c == ']') && depth > 0) {
            --depth;
        }
    }
    return out;
}

bool ParseNumber(const std::string &text, float *out) {
    if (text.empty()) return false;
    char *end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return false;
    *out = static_cast<float>(v);
    return true;
}

// `name(` ... `)` spanning all of `text`: its arguments, split at commas.
bool FunctionArgs(const std::string &text, const char *name, std::vector<std::string> *args) {
    const std::string head = std::string(name) + "(";
    if (text.size() < head.size() + 1 || Lower(text.substr(0, head.size())) != head) return false;
    if (GroupEnd(text, head.size() - 1) != text.size()) return false;
    args->clear();
    for (const auto &piece : SplitTop(text.substr(head.size(), text.size() - head.size() - 1), ','))
        args->push_back(Trim(piece.second));
    return true;
}

// Every `var(--name[, fallback])` in `value` replaced by the custom
// property's value. False when one is neither set nor given a fallback.
bool SubstituteVars(std::string value, const std::map<std::string, std::string> &vars, std::string *out) {
    for (int guard = 0; guard < 32; ++guard) {
        size_t at = std::string::npos;
        for (size_t i = 0; i + 4 <= value.size(); ++i) {
            if (value[i] == '"' || value[i] == '\'') {
                std::string ignored;
                size_t j = i;
                if (!ReadString(value, &j, &ignored)) break;
                i = j - 1;
            } else if (value.compare(i, 4, "var(") == 0 && (i == 0 || !IsNameChar(value[i - 1]))) {
                at = i;
                break;
            }
        }
        if (at == std::string::npos) {
            *out = Trim(value);
            return true;
        }
        const size_t end = GroupEnd(value, at + 3);
        if (end == std::string::npos) return false;
        const auto args = SplitTop(value.substr(at + 4, end - at - 5), ',');
        const std::string name = Trim(args[0].second);
        std::string with;
        auto it = vars.find(name);
        if (it != vars.end()) {
            with = it->second;
        } else if (args.size() > 1) {
            // The fallback is everything after the first comma.
            with = Trim(value.substr(at + 4 + args[1].first, end - 1 - (at + 4 + args[1].first)));
        } else {
            return false;
        }
        value = value.substr(0, at) + with + value.substr(end);
    }
    return false;  // (a custom property that names itself)
}

enum class Prop {
    Color,
    Background,
    FontWeight,
    FontStyle,
    TextDecoration,
    TextDecorationColor,
    FontSize,
    FontFamily,
    VerticalAlign,
    TextAlign,
    Content,
    BorderColor,
    BorderLeftColor,
    Unknown,
};

Prop PropFor(const std::string &name) {
    const auto &props = Properties();
    for (size_t i = 0; i < props.size(); ++i)
        if (name == props[i].name) return static_cast<Prop>(i);
    return Prop::Unknown;
}

// A colour property's value: `none` clears it.
bool ApplyColor(const std::string &value, Color *out) {
    Color c;
    if (!ParseColorValue(value, &c)) return false;
    *out = c;
    return true;
}

// One declaration onto `c`. `value` has its var()s substituted already.
// False when the property does not take the value.
bool Apply(Prop prop, const std::string &value, const Computed &parent, Computed *c) {
    const std::string v = Lower(value);
    const bool inherit = v == "inherit", initial = v == "initial";
    static const Computed kInitial;
    const Computed &from = inherit ? parent : kInitial;
    switch (prop) {
        case Prop::Color:
            if (inherit || initial || v == "none") {
                c->has_color = v == "none" ? false : from.has_color;
                c->color = v == "none" ? Color() : from.color;
                return true;
            }
            if (!ApplyColor(value, &c->color)) return false;
            c->has_color = c->color.kind != Color::None;
            return true;
        case Prop::Background:
            if (inherit || initial) {
                c->background = from.background;
                return true;
            }
            return ApplyColor(value, &c->background);
        case Prop::BorderColor:
            if (inherit || initial) {
                c->border_color = from.border_color;
                return true;
            }
            return ApplyColor(value, &c->border_color);
        case Prop::BorderLeftColor:
            if (inherit || initial) {
                c->border_left_color = from.border_left_color;
                return true;
            }
            return ApplyColor(value, &c->border_left_color);
        case Prop::TextDecorationColor:
            if (inherit || initial || v == "none") {
                c->has_decoration_color = v == "none" ? false : from.has_decoration_color;
                c->decoration_color = v == "none" ? Color() : from.decoration_color;
                return true;
            }
            if (!ApplyColor(value, &c->decoration_color)) return false;
            c->has_decoration_color = c->decoration_color.kind != Color::None;
            return true;
        case Prop::FontWeight:
            if (inherit || initial) c->bold = from.bold;
            else if (v == "bold") c->bold = true;
            else if (v == "normal") c->bold = false;
            else return false;
            return true;
        case Prop::FontStyle:
            if (inherit || initial) c->italic = from.italic;
            else if (v == "italic") c->italic = true;
            else if (v == "normal") c->italic = false;
            else return false;
            return true;
        case Prop::TextDecoration: {
            if (inherit || initial) {
                c->underline = from.underline;
                c->strike = from.strike;
                return true;
            }
            // Each keyword adds to (or takes from) what is inherited.
            bool underline = c->underline, strike = c->strike, any = false;
            for (const auto &piece : SplitTop(v, ' ')) {
                const std::string &w = piece.second;
                if (w.empty()) continue;
                any = true;
                if (w == "none") underline = strike = false;
                else if (w == "underline") underline = true;
                else if (w == "line-through") strike = true;
                else if (w == "no-underline") underline = false;
                else if (w == "no-line-through") strike = false;
                else return false;
            }
            if (!any) return false;
            c->underline = underline;
            c->strike = strike;
            return true;
        }
        case Prop::FontSize: {
            if (inherit || initial) {
                c->font_size = from.font_size;
                return true;
            }
            float n = 0.0f;
            if (v.size() > 2 && v.compare(v.size() - 2, 2, "pt") == 0 && ParseNumber(v.substr(0, v.size() - 2), &n) && n > 0.0f) {
                c->font_size = n / 12.0f;
                return true;
            }
            std::string num = v;
            if (num.size() > 2 && num.compare(num.size() - 2, 2, "em") == 0) num.resize(num.size() - 2);
            if (!ParseNumber(num, &n) || n <= 0.0f) return false;
            c->font_size = parent.font_size * n;
            return true;
        }
        case Prop::FontFamily: {
            if (inherit || initial) {
                c->font_family = from.font_family;
                c->font_names = from.font_names;
                return true;
            }
            std::string generic;
            bool has_generic = false;
            std::vector<std::string> names;
            for (const auto &piece : SplitTop(value, ',')) {
                std::string w = Trim(piece.second);
                if (w.empty()) return false;
                if (w[0] == '"' || w[0] == '\'') {
                    size_t i = 0;
                    std::string text;
                    if (!ReadString(w, &i, &text) || i != w.size()) return false;
                    names.push_back(text);
                    continue;
                }
                const std::string lw = Lower(w);
                if (lw == "serif" || lw == "sans" || lw == "mono" || lw == "body") {
                    if (has_generic) return false;
                    has_generic = true;
                    generic = lw == "body" ? "" : lw;
                } else {
                    names.push_back(w);
                }
            }
            c->font_family = generic;
            c->font_names = std::move(names);
            return true;
        }
        case Prop::VerticalAlign:
            if (inherit || initial) c->vertical_align = from.vertical_align;
            else if (v == "baseline") c->vertical_align = VerticalAlign::Baseline;
            else if (v == "super") c->vertical_align = VerticalAlign::Super;
            else if (v == "sub") c->vertical_align = VerticalAlign::Sub;
            else return false;
            return true;
        case Prop::TextAlign:
            if (inherit || initial) c->text_align = from.text_align;
            else if (v == "left") c->text_align = TextAlign::Left;
            else if (v == "center") c->text_align = TextAlign::Center;
            else if (v == "right") c->text_align = TextAlign::Right;
            else return false;
            return true;
        case Prop::Content: {
            if (inherit || initial || v == "none") {
                c->has_content = v == "none" ? false : from.has_content;
                c->content = v == "none" ? std::string() : from.content;
                return true;
            }
            // One string, or several run together.
            std::string text;
            size_t i = 0;
            bool any = false;
            while (i < value.size()) {
                if (IsSpace(value[i])) {
                    ++i;
                    continue;
                }
                if (value[i] != '"' && value[i] != '\'') return false;
                std::string piece;
                if (!ReadString(value, &i, &piece)) return false;
                text += piece;
                any = true;
            }
            if (!any) return false;
            c->has_content = true;
            c->content = std::move(text);
            return true;
        }
        case Prop::Unknown:
            return false;
    }
    return false;
}

// What an element starts from: its parent's inherited properties, the
// rest at their initial values.
Computed Inherited(const Computed &parent) {
    Computed c;
    c.has_color = parent.has_color;
    c.color = parent.color;
    c.bold = parent.bold;
    c.italic = parent.italic;
    c.underline = parent.underline;
    c.strike = parent.strike;
    c.has_decoration_color = parent.has_decoration_color;
    c.decoration_color = parent.decoration_color;
    c.font_size = parent.font_size;
    c.font_family = parent.font_family;
    c.font_names = parent.font_names;
    c.vertical_align = parent.vertical_align;
    c.text_align = parent.text_align;
    c.vars = parent.vars;
    return c;
}

// ---------------------------------------------------------------------------
// The parser.

struct Parser {
    const std::string &s;
    Sheet *sheet;
    size_t i = 0;
    std::vector<size_t> line_starts;

    Parser(const std::string &text, Sheet *out) : s(text), sheet(out) {
        line_starts.push_back(0);
        for (size_t k = 0; k < s.size(); ++k)
            if (s[k] == '\n') line_starts.push_back(k + 1);
    }

    void Diag(size_t at, const std::string &message) {
        at = std::min(at, s.size());
        auto it = std::upper_bound(line_starts.begin(), line_starts.end(), at);
        const size_t line = static_cast<size_t>(it - line_starts.begin()) - 1;
        Diagnostic d;
        d.line = static_cast<int>(line);
        d.col = static_cast<int>(at - line_starts[line]);
        d.message = message;
        sheet->diagnostics.push_back(std::move(d));
    }

    void SkipSpace() {
        while (i < s.size()) {
            if (IsSpace(s[i])) {
                ++i;
            } else if (s.compare(i, 2, "/*") == 0) {
                const size_t end = s.find("*/", i + 2);
                if (end == std::string::npos) {
                    Diag(i, "comment never closed");
                    i = s.size();
                } else {
                    i = end + 2;
                }
            } else {
                break;
            }
        }
    }

    // Up to (not including) the next top-level `stop` character -- strings,
    // comments and brackets skipped -- or the end. Comments become spaces.
    std::string Until(const char *stops, size_t *start) {
        *start = i;
        std::string out;
        int depth = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (s.compare(i, 2, "/*") == 0) {
                const size_t end = s.find("*/", i + 2);
                const size_t to = end == std::string::npos ? s.size() : end + 2;
                out.append(to - i, ' ');
                i = to;
                continue;
            }
            if (c == '"' || c == '\'') {
                std::string ignored;
                size_t j = i;
                if (!ReadString(s, &j, &ignored)) {
                    // Unclosed: it ends with its line.
                    j = s.find('\n', i);
                    if (j == std::string::npos) j = s.size();
                }
                out.append(s, i, j - i);
                i = j;
                continue;
            }
            // (A brace ends it whatever the depth: an unclosed bracket must
            // not swallow the rules after it.)
            if (std::string(stops).find(c) != std::string::npos && (depth == 0 || c == '{' || c == '}')) break;
            if (c == '(' || c == '[') ++depth;
            else if ((c == ')' || c == ']') && depth > 0) --depth;
            out += c;
            ++i;
        }
        return out;
    }

    // The `{ ... }` block at s[i] skipped whole (nested ones too).
    void SkipBlock() {
        int depth = 0;
        while (i < s.size()) {
            size_t start = 0;
            Until("{}", &start);
            if (i >= s.size()) return;
            if (s[i] == '{') ++depth;
            else if (--depth <= 0) {
                ++i;
                return;
            }
            ++i;
        }
    }

    bool ParseSelector(const std::string &text, size_t base, Selector *out) {
        size_t p = 0;
        bool child = false, need_compound = true;
        auto skip = [&] {
            while (p < text.size() && IsSpace(text[p])) ++p;
        };
        skip();
        if (p >= text.size()) {
            Diag(base, "empty selector");
            return false;
        }
        while (p < text.size()) {
            skip();
            if (p >= text.size()) break;
            if (text[p] == '>') {
                if (need_compound) {
                    Diag(base + p, "`>` needs a selector on both sides");
                    return false;
                }
                child = true;
                need_compound = true;
                ++p;
                continue;
            }
            if (!out->part.empty()) {
                Diag(base + p, "nothing may follow a `::part`");
                return false;
            }
            Compound c;
            c.child = child;
            child = false;
            const size_t compound_start = p;
            if (text[p] == '*') {
                ++p;
            } else {
                while (p < text.size() && IsNameChar(text[p])) c.name += text[p++];
            }
            while (p < text.size() && (text[p] == '[' || text[p] == '.')) {
                // `.name`: the element's class, `[class=name]` for short.
                if (text[p] == '.') {
                    AttrSelector a;
                    a.name = "class";
                    a.has_value = true;
                    ++p;
                    while (p < text.size() && IsNameChar(text[p])) a.value += text[p++];
                    if (a.value.empty()) {
                        Diag(base + p, "`.` needs a class name");
                        return false;
                    }
                    c.attrs.push_back(std::move(a));
                    continue;
                }
                const size_t close = text.find(']', p);
                if (close == std::string::npos) {
                    Diag(base + p, "`[` never closed");
                    return false;
                }
                const std::string inside = text.substr(p + 1, close - p - 1);
                AttrSelector a;
                const size_t eq = inside.find('=');
                a.name = Trim(inside.substr(0, eq));
                if (eq != std::string::npos) {
                    a.has_value = true;
                    std::string v = Trim(inside.substr(eq + 1));
                    if (!v.empty() && (v[0] == '"' || v[0] == '\'')) {
                        size_t k = 0;
                        std::string str;
                        if (!ReadString(v, &k, &str) || k != v.size()) {
                            Diag(base + p, "bad string in `[" + inside + "]`");
                            return false;
                        }
                        v = str;
                    }
                    a.value = v;
                }
                if (a.name.empty() || !std::all_of(a.name.begin(), a.name.end(), IsNameChar)) {
                    Diag(base + p, "bad attribute name in `[" + inside + "]`");
                    return false;
                }
                c.attrs.push_back(std::move(a));
                p = close + 1;
            }
            // `:state` -- the renderer's, not the document's (`:active`: the
            // block the cursor is in). Matched like an attribute.
            while (p + 1 < text.size() && text[p] == ':' && text[p + 1] != ':') {
                AttrSelector a;
                a.name = ":";
                ++p;
                while (p < text.size() && IsNameChar(text[p])) a.name += text[p++];
                if (a.name.size() == 1) {
                    Diag(base + p, "`:` needs a state name");
                    return false;
                }
                c.attrs.push_back(std::move(a));
            }
            if (text.compare(p, 2, "::") == 0) {
                p += 2;
                while (p < text.size() && IsNameChar(text[p])) out->part += text[p++];
                if (out->part.empty()) {
                    Diag(base + p, "`::` needs a part name");
                    return false;
                }
            }
            if (p == compound_start) {
                Diag(base + p, std::string("unexpected `") + text[p] + "` in selector");
                return false;
            }
            if (p < text.size() && !IsSpace(text[p]) && text[p] != '>') {
                Diag(base + p, std::string("unexpected `") + text[p] + "` in selector");
                return false;
            }
            out->spec_attrs += static_cast<int>(c.attrs.size());
            if (!c.name.empty()) ++out->spec_names;
            out->compounds.push_back(std::move(c));
            need_compound = false;
        }
        if (need_compound) {
            Diag(base + text.size(), "selector ends with `>`");
            return false;
        }
        if (!out->part.empty()) ++out->spec_names;
        return true;
    }

    void ParseDeclarations(Rule *rule) {
        // s[i] is just past the `{`.
        while (i < s.size()) {
            SkipSpace();
            if (i >= s.size()) {
                Diag(i, "`{` never closed");
                return;
            }
            if (s[i] == '}') {
                ++i;
                return;
            }
            if (s[i] == ';') {
                ++i;
                continue;
            }
            size_t start = 0;
            const std::string text = Until(";}{", &start);
            if (i < s.size() && s[i] == '{') {
                Diag(start, "a rule cannot hold another rule");
                SkipBlock();
                continue;
            }
            const size_t colon = text.find(':');
            if (colon == std::string::npos) {
                Diag(start, "expected `property: value`");
                continue;
            }
            Declaration d;
            d.property = Trim(text.substr(0, colon));
            d.value = Trim(text.substr(colon + 1));
            const bool custom = d.property.rfind("--", 0) == 0;
            if (!custom) d.property = Lower(d.property);
            {
                auto it = std::upper_bound(line_starts.begin(), line_starts.end(), start);
                const size_t line = static_cast<size_t>(it - line_starts.begin()) - 1;
                d.line = static_cast<int>(line);
                d.col = static_cast<int>(start - line_starts[line]);
            }
            if (d.property.empty() || !std::all_of(d.property.begin(), d.property.end(), IsNameChar)) {
                Diag(start, "bad property name `" + d.property + "`");
                continue;
            }
            if (d.value.empty()) {
                Diag(start, "`" + d.property + "` has no value");
                continue;
            }
            if (!custom) {
                const Prop prop = PropFor(d.property);
                if (prop == Prop::Unknown) {
                    Diag(start, "unknown property `" + d.property + "`");
                    continue;
                }
                // A value with a var() is checked when it is computed.
                if (d.value.find("var(") == std::string::npos) {
                    Computed scratch;
                    if (!Apply(prop, d.value, Computed(), &scratch)) {
                        Diag(start + colon + 1,
                             "`" + d.property + "` does not take `" + d.value + "` (" + Properties()[static_cast<size_t>(prop)].values + ")");
                        continue;
                    }
                }
            }
            rule->declarations.push_back(std::move(d));
        }
        Diag(i, "`{` never closed");
    }

    // One rule at s[i], inside `media`.
    void ParseRule(const std::vector<MediaTerm> &media) {
        size_t start = 0;
        const std::string prelude = Until("{}", &start);
        if (i >= s.size() || s[i] == '}') {
            if (!Trim(prelude).empty()) Diag(start, "expected `{` after the selector");
            return;
        }
        ++i;  // `{`
        Rule rule;
        rule.media = media;
        {
            const size_t first = std::min(s.find_first_not_of(" \t\r\n", start), s.size());
            auto it = std::upper_bound(line_starts.begin(), line_starts.end(), first);
            rule.line = static_cast<int>(it - line_starts.begin()) - 1;
        }
        bool ok = true;
        for (const auto &piece : SplitTop(prelude, ',')) {
            Selector sel;
            if (ParseSelector(piece.second, start + piece.first, &sel)) rule.selectors.push_back(std::move(sel));
            else ok = false;
        }
        ParseDeclarations(&rule);
        // A selector list with a bad selector drops the whole rule, as in CSS.
        if (ok && !rule.selectors.empty()) sheet->rules.push_back(std::move(rule));
    }

    void ParseSheet() {
        while (true) {
            SkipSpace();
            if (i >= s.size()) return;
            if (s[i] == '}') {
                Diag(i, "unexpected `}`");
                ++i;
                continue;
            }
            if (s[i] != '@') {
                ParseRule({});
                continue;
            }
            const size_t at = i++;
            std::string name;
            while (i < s.size() && IsNameChar(s[i])) name += s[i++];
            size_t start = 0;
            const std::string prelude = Until("{;", &start);
            if (name != "media") {
                Diag(at, "unknown rule `@" + name + "`");
                if (i < s.size() && s[i] == '{') SkipBlock();
                else if (i < s.size()) ++i;
                continue;
            }
            if (i >= s.size() || s[i] != '{') {
                Diag(at, "`@media` needs a `{ ... }` block");
                if (i < s.size()) ++i;
                continue;
            }
            ++i;
            std::vector<MediaTerm> media;
            bool ok = true;
            for (const auto &piece : SplitTop(prelude, ',')) {
                std::string term = Lower(Trim(piece.second));
                MediaTerm t;
                if (term.rfind("not ", 0) == 0 || term.rfind("not\t", 0) == 0) {
                    t.negate = true;
                    term = Trim(term.substr(3));
                }
                if (term.empty() || !std::all_of(term.begin(), term.end(), IsNameChar)) {
                    Diag(start + piece.first, "bad media tag `" + term + "`");
                    ok = false;
                    continue;
                }
                t.tag = term;
                media.push_back(std::move(t));
            }
            if (!ok || media.empty()) {
                --i;
                SkipBlock();
                continue;
            }
            while (true) {
                SkipSpace();
                if (i >= s.size()) {
                    Diag(at, "`@media` never closed");
                    return;
                }
                if (s[i] == '}') {
                    ++i;
                    break;
                }
                if (s[i] == '@') {
                    Diag(i, "`@` rules do not nest");
                    size_t ignored = 0;
                    Until("{;", &ignored);
                    if (i < s.size() && s[i] == '{') SkipBlock();
                    else if (i < s.size()) ++i;
                    continue;
                }
                ParseRule(media);
            }
        }
    }
};

bool CompoundMatches(const Compound &c, const Element &e) {
    if (!c.name.empty() && c.name != e.name) return false;
    for (const AttrSelector &a : c.attrs) {
        const std::string *v = e.Attr(a.name);
        if (!v) return false;
        if (a.has_value && *v != a.value) return false;
    }
    return true;
}

}  // namespace

const std::vector<PropertyInfo> &Properties() {
    // In Prop's order.
    static const std::vector<PropertyInfo> k = {
        {"color", true, "a colour", "The text's colour."},
        {"background", false, "a colour or none", "The colour behind the text, or behind a block's box."},
        {"font-weight", true, "normal or bold", "Bold or not."},
        {"font-style", true, "normal or italic", "Italic or not."},
        {"text-decoration", true, "none, underline, line-through, no-underline, no-line-through",
         "Underline and strike-through; each keyword adds to what is inherited."},
        {"text-decoration-color", true, "a colour", "The colour of the underline or strike-through."},
        {"font-size", true, "a number (times the parent's size) or Npt", "The text's size."},
        {"font-family", true, "serif, sans, mono or body, after any named families", "The text's face."},
        {"vertical-align", true, "baseline, super or sub", "Raised or lowered text."},
        {"text-align", true, "left, center or right", "How lines are set across the text width."},
        {"content", false, "a string or none", "Generated text: a label, a list marker, an end mark."},
        {"border-color", false, "a colour or none", "A block's outline."},
        {"border-left-color", false, "a colour or none", "A rule down a block's left edge."},
    };
    return k;
}

bool ParseColorValue(const std::string &text_in, Color *out) {
    const std::string text = Trim(text_in);
    const std::string low = Lower(text);
    Color c;
    if (low == "none" || low == "transparent") {
        *out = c;
        return true;
    }
    std::vector<std::string> args;
    if (FunctionArgs(text, "theme", &args)) {
        if (args.empty() || args.size() > 2 || args[0].empty() || !std::all_of(args[0].begin(), args[0].end(), IsNameChar)) return false;
        c.kind = Color::Theme;
        c.group = args[0];
        if (args.size() == 2) {
            Color fb;
            if (!ParseColorValue(args[1], &fb) || fb.kind != Color::Rgb) return false;
            c.rgb = fb.rgb;
            c.has_fallback = true;
        }
        *out = c;
        return true;
    }
    if (FunctionArgs(text, "fade", &args)) {
        float a = 0.0f;
        if (args.size() != 2 || !ParseColorValue(args[0], &c) || !ParseNumber(args[1], &a)) return false;
        c.alpha *= std::clamp(a, 0.0f, 1.0f);
        *out = c;
        return true;
    }
    if (low.size() == 9 && low[0] == '#') {  // #rrggbbaa
        for (size_t k = 1; k < low.size(); ++k)
            if (!std::isxdigit(static_cast<unsigned char>(low[k]))) return false;
        c.kind = Color::Rgb;
        c.rgb = static_cast<std::uint32_t>(std::strtoul(low.substr(1, 6).c_str(), nullptr, 16));
        c.alpha = static_cast<float>(std::strtoul(low.substr(7, 2).c_str(), nullptr, 16)) / 255.0f;
        *out = c;
        return true;
    }
    std::uint32_t rgb = 0;
    if (!mepml::ParseColor(low, &rgb)) return false;
    c.kind = Color::Rgb;
    c.rgb = rgb;
    *out = c;
    return true;
}

std::string GenericFamily(const std::string &family) {
    const std::string f = Lower(family);
    for (const char *mono : {"mono", "courier", "consol", "code", "menlo", "fixed"})
        if (f.find(mono) != std::string::npos) return "mono";
    if (f.find("sans") != std::string::npos) return "sans";
    for (const char *serif : {"serif", "times", "georgia", "garamond", "palatino", "cambria", "book", "roman"})
        if (f.find(serif) != std::string::npos) return "serif";
    return "sans";  // Helvetica, Arial, Verdana ...
}

Sheet Parse(const std::string &text, const std::string &origin) {
    Sheet sheet;
    sheet.origin = origin;
    Parser p(text, &sheet);
    p.ParseSheet();
    return sheet;
}

const char *DefaultSheetText() { return kMepmlDefaultSheet; }

const Sheet &DefaultSheet() {
    static const Sheet sheet = Parse(kMepmlDefaultSheet, "default");
    return sheet;
}

bool Matches(const Selector &sel, const std::vector<const Element *> &path) {
    if (path.empty() || sel.compounds.empty()) return false;
    const Element &subject = *path.back();
    if (subject.part != sel.part) return false;
    if (!CompoundMatches(sel.compounds.back(), subject)) return false;
    // The subject's ancestors, innermost first. A part is not an element
    // of the tree: what is inside a `::title` has the title's own element
    // as its parent.
    std::vector<const Element *> up;
    for (size_t k = path.size() - 1; k-- > 0;)
        if (path[k]->part.empty()) up.push_back(path[k]);
    // A part's own element is its subject, not its ancestor.
    if (!subject.part.empty() && !up.empty()) up.erase(up.begin());
    // Match the remaining compounds right to left; a descendant step may
    // skip ancestors, so try each one it could stand on.
    struct Step {
        static bool Go(const std::vector<Compound> &cs, size_t ci, const std::vector<const Element *> &anc, size_t ai) {
            // cs[ci] is matched; cs[ci - 1] must match an ancestor from anc[ai] on.
            if (ci == 0) return true;
            const bool child = cs[ci].child;
            for (size_t k = ai; k < anc.size(); ++k) {
                if (CompoundMatches(cs[ci - 1], *anc[k]) && Go(cs, ci - 1, anc, k + 1)) return true;
                if (child) break;
            }
            return false;
        }
    };
    return Step::Go(sel.compounds, sel.compounds.size() - 1, up, 0);
}

bool Cascade::MediaApplies(const std::vector<MediaTerm> &terms) const {
    if (terms.empty()) return true;
    for (const MediaTerm &t : terms) {
        const bool has = std::find(media.begin(), media.end(), t.tag) != media.end();
        if (has != t.negate) return true;
    }
    return false;
}

namespace {

// A rule with where it stands in the cascade: its sheet, and its place
// among all the rules of all the sheets.
struct RuleRef {
    size_t sheet;
    size_t order;
    const Rule *rule;
};

// The rules that could select an element, by its name: those with a
// selector whose subject names it, and those with one that names none
// (`*`, `[attr]`). Only rules whose @media applies are in it.
struct RuleIndex {
    std::unordered_map<std::string, std::vector<RuleRef>> by_name;
    std::vector<RuleRef> any;
    std::vector<RuleRef> all;
};

RuleIndex BuildIndex(const Cascade &cascade) {
    RuleIndex index;
    size_t order = 0;
    for (size_t si = 0; si < cascade.sheets.size(); ++si) {
        if (!cascade.sheets[si]) continue;
        for (const Rule &rule : cascade.sheets[si]->rules) {
            ++order;
            if (!cascade.MediaApplies(rule.media)) continue;
            const RuleRef ref{si, order, &rule};
            index.all.push_back(ref);
            bool nameless = false;
            std::vector<const std::string *> names;
            for (const Selector &sel : rule.selectors) {
                const std::string &name = sel.compounds.back().name;
                if (name.empty()) nameless = true;
                else if (std::none_of(names.begin(), names.end(), [&](const std::string *n) { return *n == name; })) names.push_back(&name);
            }
            if (nameless) {
                index.any.push_back(ref);
                continue;  // (it is tried for every element anyway)
            }
            for (const std::string *name : names) index.by_name[*name].push_back(ref);
        }
    }
    return index;
}

// The cascade for one element, over the rules `rules` (in cascade order).
void ApplyRules(const std::vector<const RuleRef *> &rules, const std::vector<const Element *> &path, const Computed &parent,
                Computed *out) {
    Computed &c = *out;
    struct Hit {
        size_t sheet;
        int attrs, names;
        size_t order;
        const Declaration *decl;
    };
    std::vector<Hit> hits;
    for (const RuleRef *ref : rules) {
        // The most specific of the rule's selectors that match.
        const Selector *best = nullptr;
        for (const Selector &sel : ref->rule->selectors) {
            if (!Matches(sel, path)) continue;
            if (!best || sel.spec_attrs > best->spec_attrs || (sel.spec_attrs == best->spec_attrs && sel.spec_names > best->spec_names))
                best = &sel;
        }
        if (!best) continue;
        for (const Declaration &d : ref->rule->declarations) hits.push_back({ref->sheet, best->spec_attrs, best->spec_names, ref->order, &d});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        if (a.sheet != b.sheet) return a.sheet < b.sheet;
        if (a.attrs != b.attrs) return a.attrs < b.attrs;
        if (a.names != b.names) return a.names < b.names;
        return a.order < b.order;
    });
    // Custom properties first: a declaration may use one set by a rule
    // that comes after it.
    for (const Hit &h : hits) {
        if (h.decl->property.rfind("--", 0) != 0) continue;
        std::string value;
        if (SubstituteVars(h.decl->value, c.vars, &value)) c.vars[h.decl->property] = value;
    }
    for (const Hit &h : hits) {
        if (h.decl->property.rfind("--", 0) == 0) continue;
        // (Most values name no custom property: nothing to substitute.)
        if (h.decl->value.find("var(") == std::string::npos) {
            Apply(PropFor(h.decl->property), h.decl->value, parent, &c);
            continue;
        }
        std::string value;
        if (!SubstituteVars(h.decl->value, c.vars, &value)) continue;
        Apply(PropFor(h.decl->property), value, parent, &c);
    }
}

// The rules of `index` that could select an element named `name`, in
// cascade order.
std::vector<const RuleRef *> Candidates(const RuleIndex &index, const std::string &name) {
    std::vector<const RuleRef *> out;
    auto it = index.by_name.find(name);
    static const std::vector<RuleRef> kNone;
    const std::vector<RuleRef> &named = it == index.by_name.end() ? kNone : it->second;
    size_t a = 0, b = 0;
    while (a < named.size() || b < index.any.size()) {
        if (b >= index.any.size() || (a < named.size() && named[a].order < index.any[b].order)) out.push_back(&named[a++]);
        else out.push_back(&index.any[b++]);
    }
    return out;
}

void Presentational(const std::vector<const Element *> &path, Computed *out);

}  // namespace

Computed Cascade::Compute(const std::vector<const Element *> &path, const Computed &parent) const {
    Computed c = Inherited(parent);
    if (path.empty()) return c;
    const RuleIndex index = BuildIndex(*this);
    std::vector<const RuleRef *> rules;
    for (const RuleRef &r : index.all) rules.push_back(&r);
    ApplyRules(rules, path, parent, &c);
    Presentational(path, &c);
    return c;
}

namespace {

void Presentational(const std::vector<const Element *> &path, Computed *out) {
    Computed &c = *out;
    // Presentational markup (rendering.md §4): the author's own colour,
    // face and size are the document's, whatever the sheets say.
    const Element &e = *path.back();
    if (e.part.empty()) {
        if (e.name == "color") {
            std::uint32_t rgb = 0;
            const std::string *v = e.Attr("value");
            if (v && mepml::ParseColor(*v, &rgb)) {
                c.has_color = true;
                c.color = Color();
                c.color.kind = Color::Rgb;
                c.color.rgb = rgb;
            }
        } else if (e.name == "font") {
            if (const std::string *v = e.Attr("family"); v && !v->empty()) {
                c.font_family = GenericFamily(*v);
                c.font_names = {*v};
            }
        } else if (e.name == "font-size") {
            float pt = 0.0f;
            if (const std::string *v = e.Attr("pt"); v && ParseNumber(Trim(*v), &pt) && pt > 0.0f) c.font_size = pt / 12.0f;
        }
    }
}

}  // namespace

std::vector<Computed> Cascade::ComputeAll(const ElementPaths &paths) const {
    std::vector<Computed> out(paths.nodes.size());
    const Computed root;
    const RuleIndex index = BuildIndex(*this);
    std::unordered_map<std::string, std::vector<const RuleRef *>> candidates;
    for (size_t n = 0; n < paths.nodes.size(); ++n) {
        const int parent = paths.nodes[n].parent;
        // (A parent is always interned before its children.)
        const Computed &from = parent >= 0 && static_cast<size_t>(parent) < n ? out[static_cast<size_t>(parent)] : root;
        // (Only the rules that name this element, or none: a sheet's few
        // hundred rules are mostly about something else.)
        const std::vector<const Element *> path = paths.Path(static_cast<int>(n));
        auto cached = candidates.find(path.back()->name);
        if (cached == candidates.end()) cached = candidates.emplace(path.back()->name, Candidates(index, path.back()->name)).first;
        Computed c = Inherited(from);
        ApplyRules(cached->second, path, from, &c);
        Presentational(path, &c);
        out[n] = std::move(c);
    }
    return out;
}

std::string ExpandContent(const std::string &content, const std::string &number, const std::string &kind,
                          const std::string &title) {
    std::string out;
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] != '%' || i + 1 >= content.size()) {
            out += content[i];
            continue;
        }
        const char c = content[++i];
        if (c == 'n') out += number;
        else if (c == 'k') out += kind;
        else if (c == 't') out += title;
        else if (c == '%') out += '%';
        else {
            out += '%';
            out += c;
        }
    }
    return out;
}

}  // namespace mepml::style
