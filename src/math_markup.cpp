#include "math_markup.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "math_tex.h"

namespace {

std::string XmlEsc(const std::string &s) {
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

// The first codepoint of a UTF-8 string, 0 for an empty one.
int FirstCp(const std::string &s) {
    if (s.empty()) return 0;
    const auto b = [&](size_t k) { return k < s.size() ? static_cast<int>(static_cast<unsigned char>(s[k])) : 0; };
    const int c = b(0);
    if (c < 0x80) return c;
    if (c < 0xE0) return (c & 0x1F) << 6 | (b(1) & 0x3F);
    if (c < 0xF0) return (c & 0x0F) << 12 | (b(1) & 0x3F) << 6 | (b(2) & 0x3F);
    return (c & 0x07) << 18 | (b(1) & 0x3F) << 12 | (b(2) & 0x3F) << 6 | (b(3) & 0x3F);
}

std::string Utf8(int cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
    return out;
}

// A letter a variable could be named with: Latin, Greek, the letterlike
// block (ℝ, ℓ) and the mathematical alphanumerics (𝔸).
bool IsLetterCp(int cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= 0x370 && cp <= 0x3FF) ||
           (cp >= 0x2100 && cp <= 0x214F) || (cp >= 0x1D400 && cp <= 0x1D7FF);
}

bool IsDigits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!(c >= '0' && c <= '9') && c != '.') return false;
    return true;
}

bool HasScripts(const MathNode &n) { return !n.sup.empty() || !n.sub.empty(); }

// A run of upright single-character atoms with no scripts of their own --
// what \sin, \mathrm{d} and \text{if } parse to: set as one word.
bool IsWord(const MathNode &n) {
    if (n.kind != MathKind::Row || n.children.size() < 2) return false;
    bool letter = false;
    for (const MathNode &c : n.children) {
        if (c.kind != MathKind::Text || HasScripts(c) || c.face == MathFace::Italic) return false;
        letter = letter || IsLetterCp(FirstCp(c.text));
    }
    return letter;
}

std::string WordText(const MathNode &n) {
    std::string t;
    for (const MathNode &c : n.children) t += c.text;
    return t;
}

// An accent's mark as a combining character (OMML, text runs) and as the
// spacing one MathML wants over its base.
int CombiningAccent(const std::string &mark, bool below) {
    switch (FirstCp(mark)) {
        case 0x5E: return 0x302;
        case 0x7E: return 0x303;
        case 0x2192: return 0x20D7;
        case 0xB7: return 0x307;
        case 0xA8: return 0x308;
        case 0x2C7: return 0x30C;
        case 0x2D8: return 0x306;
        case 0xB4: return 0x301;
        case 0x60: return 0x300;
        case 0xB0: return 0x30A;
        case 0: return below ? 0x332 : 0x305;  // \underline / \overline
        default: return 0;
    }
}
std::string SpacingAccent(const std::string &mark, bool below) {
    switch (FirstCp(mark)) {
        case 0x5E: return Utf8(0x2C6);
        case 0x7E: return Utf8(0x2DC);
        case 0xB7: return Utf8(0x2D9);
        case 0: return below ? "_" : Utf8(0xAF);
        default: return mark;
    }
}

// --- MathML ---------------------------------------------------------------------

struct MathMlWriter {
    bool display = false;

    static std::string Variant(MathFace f) {
        switch (f) {
            case MathFace::Italic: return "";
            case MathFace::Upright: return " mathvariant=\"normal\"";
            case MathFace::Bold: return " mathvariant=\"bold\"";
            case MathFace::BoldItalic: return " mathvariant=\"bold-italic\"";
            case MathFace::Mono: return " mathvariant=\"monospace\"";
            case MathFace::Sans: return " mathvariant=\"sans-serif\"";
        }
        return "";
    }

    std::string Token(const MathNode &n) {
        const int cp = FirstCp(n.text);
        if (n.text.empty()) return "<mrow/>";
        if (n.big_op) return "<mo largeop=\"true\"" + std::string(n.limits_above ? "" : " movablelimits=\"false\"") + ">" + XmlEsc(n.text) + "</mo>";
        if (IsDigits(n.text)) return "<mn>" + XmlEsc(n.text) + "</mn>";
        if (n.cls == MathClass::Ord && IsLetterCp(cp)) return "<mi" + Variant(n.face) + ">" + XmlEsc(n.text) + "</mi>";
        if (n.cls == MathClass::Ord && cp != '|' && cp != 0x2016 && cp != '/' && cp != 0x2032 && cp != '!')
            return "<mi" + Variant(n.face == MathFace::Italic ? MathFace::Upright : n.face) + ">" + XmlEsc(n.text) + "</mi>";
        // An operator, relation, fence or punctuation. A fence written
        // bare (not \left..\right) keeps its size.
        std::string attrs;
        if (n.cls == MathClass::Open || n.cls == MathClass::Close || cp == '|' || cp == 0x2016) attrs = " stretchy=\"false\"";
        return "<mo" + attrs + ">" + XmlEsc(n.text) + "</mo>";
    }

    std::string Scripts(const std::string &base, const MathNode &n) {
        if (!HasScripts(n)) return base;
        const bool limits = (display && n.limits_above) || n.limits_always;
        const std::string sub = n.sub.empty() ? "" : Node(n.sub[0]);
        const std::string sup = n.sup.empty() ? "" : Node(n.sup[0]);
        std::string tag;
        if (!sub.empty() && !sup.empty()) tag = limits ? "munderover" : "msubsup";
        else if (!sub.empty()) tag = limits ? "munder" : "msub";
        else tag = limits ? "mover" : "msup";
        return "<" + tag + ">" + base + sub + sup + "</" + tag + ">";
    }

    // A row's children as a sequence of elements, a run of digits as one
    // number.
    std::string Items(const std::vector<MathNode> &kids, int *count) {
        std::string o;
        *count = 0;
        for (size_t k = 0; k < kids.size(); ++k) {
            const MathNode &c = kids[k];
            if (c.kind == MathKind::Text && IsDigits(c.text) && !HasScripts(c)) {
                std::string num = c.text;
                while (k + 1 < kids.size() && kids[k + 1].kind == MathKind::Text && IsDigits(kids[k + 1].text)) {
                    num += kids[++k].text;
                    if (HasScripts(kids[k])) break;
                }
                MathNode merged = kids[k];
                merged.text = num;
                o += Node(merged);
            } else if (c.kind == MathKind::Row && c.children.empty() && !HasScripts(c)) {
                continue;
            } else {
                o += Node(c);
            }
            ++*count;
        }
        return o;
    }

    std::string Group(const std::vector<MathNode> &kids) {
        int count = 0;
        const std::string inner = Items(kids, &count);
        return count == 1 ? inner : "<mrow>" + inner + "</mrow>";
    }

    std::string Node(const MathNode &n) {
        switch (n.kind) {
            case MathKind::Text: return Scripts(Token(n), n);
            case MathKind::Row: {
                if (IsWord(n)) {
                    const std::string word = WordText(n);
                    // \text{...} with a space in it is prose.
                    if (word.find(' ') != std::string::npos) return Scripts("<mtext>" + XmlEsc(word) + "</mtext>", n);
                    const MathFace f = n.children[0].face;
                    std::string base = "<mi" + std::string(f == MathFace::Upright ? "" : Variant(f)) + ">" + XmlEsc(word) + "</mi>";
                    // An operator name is followed by an invisible apply.
                    if (n.cls == MathClass::Op) base = "<mrow>" + base + "<mo>&#x2061;</mo></mrow>";
                    return Scripts(base, n);
                }
                return Scripts(Group(n.children), n);
            }
            case MathKind::Frac:
                return Scripts(std::string("<mfrac") + (n.frac_bar ? "" : " linethickness=\"0\"") + ">" +
                                   Group({n.children[0]}) + Group({n.children.size() > 1 ? n.children[1] : MathNode{}}) + "</mfrac>",
                               n);
            case MathKind::Sqrt: return Scripts("<msqrt>" + Group(n.children) + "</msqrt>", n);
            case MathKind::Space: {
                char b[32];
                std::snprintf(b, sizeof b, "%.3fem", static_cast<double>(n.space_em));
                return std::string("<mspace width=\"") + b + "\"/>";
            }
            case MathKind::Fenced: {
                std::string o = "<mrow>";
                if (!n.open_delim.empty()) o += "<mo fence=\"true\" stretchy=\"true\">" + XmlEsc(n.open_delim) + "</mo>";
                o += Group(n.children);
                if (!n.close_delim.empty()) o += "<mo fence=\"true\" stretchy=\"true\">" + XmlEsc(n.close_delim) + "</mo>";
                return Scripts(o + "</mrow>", n);
            }
            case MathKind::Accent: {
                const std::string base = Group(n.children);
                if (n.accent_brace) {
                    // The brace, then its label (limits_always) beyond it.
                    if (n.accent_below) return Scripts("<munder><mrow>" + base + "</mrow><mo stretchy=\"true\">" + Utf8(0x23DF) + "</mo></munder>", n);
                    return Scripts("<mover><mrow>" + base + "</mrow><mo stretchy=\"true\">" + Utf8(0x23DE) + "</mo></mover>", n);
                }
                const std::string mark = "<mo stretchy=\"" + std::string(n.accent_stretch ? "true" : "false") + "\">" +
                                         XmlEsc(SpacingAccent(n.accent, n.accent_below)) + "</mo>";
                if (n.accent_below) return Scripts("<munder accentunder=\"true\">" + base + mark + "</munder>", n);
                return Scripts("<mover accent=\"true\">" + base + mark + "</mover>", n);
            }
            case MathKind::Matrix: {
                const size_t cols = static_cast<size_t>(std::max(1, n.cols));
                std::string o = "<mtable";
                if (n.cells_pair_align) {
                    o += " columnalign=\"";
                    for (size_t c = 0; c < cols; ++c) o += std::string(c > 0 ? " " : "") + (c % 2 == 0 ? "right" : "left");
                    o += "\"";
                } else if (n.cells_left_align) {
                    o += " columnalign=\"left\"";
                }
                o += ">";
                for (size_t r = 0; r * cols < n.cells.size(); ++r) {
                    o += "<mtr>";
                    for (size_t c = 0; c < cols && r * cols + c < n.cells.size(); ++c) o += "<mtd>" + Group({n.cells[r * cols + c]}) + "</mtd>";
                    o += "</mtr>";
                }
                return Scripts(o + "</mtable>", n);
            }
            case MathKind::Phantom: return Scripts("<mphantom>" + Group(n.children) + "</mphantom>", n);
        }
        return "";
    }
};

// --- StarMath -------------------------------------------------------------------
//
// Only what a paragraph's inline maths needs: text style throughout, so a
// big operator carries its limits beside it as scripts. Every word of
// prose is quoted (`and`, `or`, `in` are StarMath keywords), every bare
// delimiter escaped (StarMath insists brackets pair), and a symbol's face
// set explicitly -- StarMath slants every letter it does not know.

std::string StarQuote(const std::string &text) {
    std::string o = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

// A fence's StarMath name for `left`/`right` ("none" for an empty one).
std::string StarFence(const std::string &d) {
    if (d.empty()) return "none";
    if (d == "(" || d == ")" || d == "[" || d == "]") return d;
    if (d == "{") return "lbrace";
    if (d == "}") return "rbrace";
    const int cp = FirstCp(d);
    switch (cp) {
        case '|': return "lline";
        case 0x2016: return "ldline";
        case 0x27E8: case 0x2329: return "langle";
        case 0x27E9: case 0x232A: return "rangle";
        case 0x2308: return "lceil";
        case 0x2309: return "rceil";
        case 0x230A: return "lfloor";
        case 0x230B: return "rfloor";
        default: return "none";
    }
}

struct StarMathWriter {
    std::string Text(const MathNode &n) {
        const std::string &t = n.text;
        if (t.empty()) return "{}";
        const int cp = FirstCp(t);
        // Bare delimiters, escaped so they need no partner.
        if (t == "(" || t == ")" || t == "[" || t == "]" || t == "{" || t == "}") return "\\" + t;
        if (cp == '|') return n.cls == MathClass::Close ? "\\rline" : "\\lline";
        if (cp == 0x2016) return n.cls == MathClass::Close ? "\\rdline" : "\\ldline";
        if (cp == 0x27E8) return "\\langle";
        if (cp == 0x27E9) return "\\rangle";
        std::string atom;
        if (IsDigits(t)) {
            atom = t;
        } else if (cp < 0x80 && !std::isalnum(cp) && std::string("+-=<>/*,.;:").find(static_cast<char>(cp)) == std::string::npos) {
            atom = StarQuote(t);  // `#`, `&`, `%`, `!`, ... all mean something
        } else {
            atom = t;
        }
        if (cp < 0x80 && std::isalpha(cp) == 0 && !IsLetterCp(cp)) return atom;
        switch (n.face) {
            case MathFace::Italic: return IsLetterCp(cp) ? "ital " + atom : atom;
            case MathFace::Upright: return IsLetterCp(cp) ? "nitalic " + atom : atom;
            case MathFace::Bold: return "bold nitalic " + atom;
            case MathFace::BoldItalic: return "bold ital " + atom;
            case MathFace::Mono: return "font fixed nitalic " + atom;
            case MathFace::Sans: return "font sans nitalic " + atom;
        }
        return atom;
    }

    std::string Group(const std::vector<MathNode> &kids) {
        std::string o;
        for (size_t k = 0; k < kids.size(); ++k) {
            const MathNode &c = kids[k];
            if (c.kind == MathKind::Row && c.children.empty() && !HasScripts(c)) continue;
            if (!o.empty()) o += " ";
            const std::string op = display ? StarOperator(c) : "";
            if (op.empty()) {
                o += Node(c);
                continue;
            }
            // The operator takes the atom after it as its body (as OMML's
            // n-ary does); one that ends its row gets an empty body.
            std::string term = op;
            if (!c.sub.empty()) term += " from" + Group(c.sub);
            if (!c.sup.empty()) term += " to" + Group(c.sup);
            size_t next = k + 1;
            while (next < kids.size() && kids[next].kind == MathKind::Row && kids[next].children.empty() && !HasScripts(kids[next])) ++next;
            const bool operand = next < kids.size() && kids[next].cls != MathClass::Rel && kids[next].cls != MathClass::Bin &&
                                 kids[next].cls != MathClass::Punct && kids[next].cls != MathClass::Close;
            if (operand) {
                term += " " + Node(kids[next]);
                k = next;
            } else {
                term += " {}";
            }
            o += "{" + term + "}";
        }
        // A relation needs something either side of it in StarMath (`> 0`
        // alone, a brace's label, is a syntax error): an empty group.
        const MathNode *first = nullptr, *last = nullptr;
        for (const MathNode &c : kids) {
            if (c.kind == MathKind::Row && c.children.empty() && !HasScripts(c)) continue;
            if (!first) first = &c;
            last = &c;
        }
        if (first && first->kind == MathKind::Text && first->cls == MathClass::Rel) o = "{} " + o;
        if (last && last->kind == MathKind::Text && (last->cls == MathClass::Rel || last->cls == MathClass::Bin) && (first != last || last->cls == MathClass::Rel))
            o += " {}";
        return "{" + (o.empty() ? std::string() : o) + "}";
    }

    std::string Scripts(const std::string &base, const MathNode &n) {
        std::string o = base;
        // Stacked over/under (\overset, a brace's label) is csup/csub.
        const bool limits = (display && n.limits_above) || n.limits_always;
        if (!n.sub.empty()) o += (limits ? " csub " : "_") + Group(n.sub);
        if (!n.sup.empty()) o += (limits ? " csup " : "^") + Group(n.sup);
        return HasScripts(n) ? "{" + o + "}" : o;
    }

    // Display style's big operators, with their limits over and under:
    // StarMath's own `sum from{..} to{..} body`, which (unlike a plain ∑
    // with csub/csup) also sets the sign at display size. Empty for an
    // operator StarMath has no keyword for.
    static std::string StarOperator(const MathNode &n) {
        if (n.kind == MathKind::Text && n.big_op) {
            switch (FirstCp(n.text)) {
                case 0x2211: return "sum";
                case 0x220F: return "prod";
                case 0x2210: return "coprod";
                case 0x222B: return "int";
                case 0x222C: return "iint";
                case 0x222D: return "iiint";
                case 0x222E: return "lint";
                default: return "";
            }
        }
        if (n.kind == MathKind::Row && n.cls == MathClass::Op && n.limits_above && IsWord(n)) {
            const std::string w = WordText(n);
            if (w == "lim" || w == "max" || w == "min" || w == "sup" || w == "inf" || w == "liminf" || w == "limsup") return w;
        }
        return "";
    }

    bool display = false;

    std::string Node(const MathNode &n) {
        switch (n.kind) {
            // Bare, not braced: a braced `{+}` is an operator with no operands.
            case MathKind::Text: return Scripts(HasScripts(n) ? "{" + Text(n) + "}" : Text(n), n);
            case MathKind::Row:
                if (IsWord(n)) {
                    const std::string word = WordText(n);
                    if (word.find(' ') != std::string::npos) return Scripts(StarQuote(word), n);
                    return Scripts("{func " + StarQuote(word) + "}", n);
                }
                return Scripts(Group(n.children), n);
            case MathKind::Frac: {
                // A fraction's parts are text style, even in a display: a
                // sum there keeps its limits beside it, as TeX sets it.
                const bool was_display = display;
                display = false;
                const std::string num = Group({n.children[0]});
                const std::string den = Group({n.children.size() > 1 ? n.children[1] : MathNode{}});
                display = was_display;
                if (!n.frac_bar) return Scripts("{stack{" + num + " # " + den + "}}", n);
                return Scripts("{" + num + " over " + den + "}", n);
            }
            case MathKind::Sqrt: return Scripts("{sqrt" + Group(n.children) + "}", n);
            case MathKind::Space: {
                const double em = static_cast<double>(n.space_em);
                if (em <= 0.0) return "";
                if (em >= 1.5) return "~~";
                if (em >= 0.3) return "~";
                return "`";
            }
            case MathKind::Fenced:
                return Scripts("{left " + StarFence(n.open_delim) + " " + Group(n.children) + " right " + StarFence(n.close_delim) + "}", n);
            case MathKind::Accent: {
                if (n.accent_brace) {
                    // `{base} underbrace {label}`: StarMath's brace takes its
                    // label as an operand, so that label is not a script.
                    MathNode rest = n;
                    std::vector<MathNode> &label = n.accent_below ? rest.sub : rest.sup;
                    const std::string lab = label.empty() ? "{}" : Group(label);
                    label.clear();
                    rest.limits_always = false;
                    return Scripts("{" + Group(n.children) + (n.accent_below ? " underbrace " : " overbrace ") + lab + "}", rest);
                }
                std::string cmd;
                switch (FirstCp(n.accent)) {
                    case 0x5E: cmd = n.accent_stretch ? "widehat" : "hat"; break;
                    case 0x7E: cmd = n.accent_stretch ? "widetilde" : "tilde"; break;
                    case 0x2192: cmd = "vec"; break;
                    case 0xB7: cmd = "dot"; break;
                    case 0xA8: cmd = "ddot"; break;
                    case 0x2C7: cmd = "check"; break;
                    case 0x2D8: cmd = "breve"; break;
                    case 0xB4: cmd = "acute"; break;
                    case 0x60: cmd = "grave"; break;
                    case 0xB0: cmd = "circle"; break;
                    default: cmd = n.accent_below ? "underline" : "overline";
                }
                return Scripts("{" + cmd + Group(n.children) + "}", n);
            }
            case MathKind::Matrix: {
                const size_t cols = static_cast<size_t>(std::max(1, n.cols));
                std::string o = "{matrix{";
                for (size_t k = 0; k < n.cells.size(); ++k) {
                    if (k > 0) o += k % cols == 0 ? " ## " : " # ";
                    const bool right = n.cells_pair_align && (k % cols) % 2 == 0;
                    o += (right ? "alignr " : n.cells_left_align ? "alignl " : "") + Group({n.cells[k]});
                }
                return Scripts(o + "}}", n);
            }
            case MathKind::Phantom: return Scripts("{phantom" + Group(n.children) + "}", n);
        }
        return "";
    }
};

bool NeedsLayout(const MathNode &n) {
    switch (n.kind) {
        case MathKind::Frac:
        case MathKind::Matrix:
        case MathKind::Accent: return true;
        case MathKind::Sqrt: {
            // √x reads fine as text; √(x² + y²) does not.
            const MathNode *r = n.children.empty() ? nullptr : &n.children[0];
            while (r && r->kind == MathKind::Row && r->children.size() == 1 && !HasScripts(*r)) r = &r->children[0];
            if (!r || r->kind != MathKind::Text || HasScripts(*r)) return true;
            break;
        }
        default: break;
    }
    if (!n.sub.empty() && !n.sup.empty()) return true;
    if (n.big_op && HasScripts(n)) return true;
    // A script that is itself two-dimensional, or longer than a few symbols.
    for (const std::vector<MathNode> *side : {&n.sub, &n.sup})
        for (const MathNode &s : *side)
            if (NeedsLayout(s)) return true;
    for (const MathNode &c : n.children)
        if (NeedsLayout(c)) return true;
    for (const MathNode &c : n.cells)
        if (NeedsLayout(c)) return true;
    return false;
}

// --- OMML -----------------------------------------------------------------------

struct OmmlWriter {
    std::string run_props;

    std::string Run(const std::string &text, MathFace face) {
        std::string sty;
        switch (face) {
            case MathFace::Italic: break;
            case MathFace::Upright:
            case MathFace::Mono:
            case MathFace::Sans: sty = "p"; break;
            case MathFace::Bold: sty = "b"; break;
            case MathFace::BoldItalic: sty = "bi"; break;
        }
        std::string o = "<m:r>";
        if (!sty.empty()) o += "<m:rPr><m:sty m:val=\"" + sty + "\"/></m:rPr>";
        return o + run_props + "<m:t xml:space=\"preserve\">" + XmlEsc(text) + "</m:t></m:r>";
    }

    std::string Arg(const char *tag, const std::vector<MathNode> &kids) {
        return std::string("<m:") + tag + ">" + Items(kids) + "</m:" + tag + ">";
    }

    std::string Scripts(const std::string &base, const MathNode &n) {
        if (!HasScripts(n)) return base;
        if ((display && n.limits_above) || n.limits_always) {
            std::string o = base;
            if (!n.sup.empty()) o = "<m:limUpp><m:e>" + o + "</m:e>" + Arg("lim", n.sup) + "</m:limUpp>";
            if (!n.sub.empty()) o = "<m:limLow><m:e>" + o + "</m:e>" + Arg("lim", n.sub) + "</m:limLow>";
            return o;
        }
        if (!n.sub.empty() && !n.sup.empty())
            return "<m:sSubSup><m:e>" + base + "</m:e>" + Arg("sub", n.sub) + Arg("sup", n.sup) + "</m:sSubSup>";
        if (!n.sub.empty()) return "<m:sSub><m:e>" + base + "</m:e>" + Arg("sub", n.sub) + "</m:sSub>";
        return "<m:sSup><m:e>" + base + "</m:e>" + Arg("sup", n.sup) + "</m:sSup>";
    }

    // A big operator (\sum, \int) as an n-ary: its limits, and as its
    // operand the atom after it.
    std::string Nary(const MathNode &op, const MathNode *operand) {
        std::string pr = "<m:naryPr><m:chr m:val=\"" + XmlEsc(op.text) + "\"/><m:limLoc m:val=\"" +
                         std::string(display && op.limits_above ? "undOvr" : "subSup") + "\"/>";
        if (op.sub.empty()) pr += "<m:subHide m:val=\"1\"/>";
        if (op.sup.empty()) pr += "<m:supHide m:val=\"1\"/>";
        pr += "</m:naryPr>";
        return "<m:nary>" + pr + Arg("sub", op.sub) + Arg("sup", op.sup) + "<m:e>" + (operand ? Node(*operand) : std::string()) +
               "</m:e></m:nary>";
    }

    bool display = false;

    std::string Items(const std::vector<MathNode> &kids) {
        std::string o, pending;
        MathFace pending_face = MathFace::Italic;
        auto flush = [&] {
            if (!pending.empty()) o += Run(pending, pending_face);
            pending.clear();
        };
        for (size_t k = 0; k < kids.size(); ++k) {
            const MathNode &c = kids[k];
            if (c.kind == MathKind::Text && !HasScripts(c) && !c.big_op) {
                if (!pending.empty() && c.face != pending_face) flush();
                pending_face = c.face;
                pending += c.text;
                continue;
            }
            flush();
            if (c.kind == MathKind::Text && c.big_op) {
                const MathNode *operand = nullptr;
                if (k + 1 < kids.size() && kids[k + 1].cls != MathClass::Rel && kids[k + 1].cls != MathClass::Bin &&
                    kids[k + 1].cls != MathClass::Punct && kids[k + 1].cls != MathClass::Close)
                    operand = &kids[++k];
                o += Nary(c, operand);
                continue;
            }
            o += Node(c);
        }
        flush();
        return o;
    }

    std::string Node(const MathNode &n) {
        switch (n.kind) {
            case MathKind::Text: {
                if (n.big_op) return Nary(n, nullptr);
                return Scripts(Run(n.text, n.face), n);
            }
            case MathKind::Row: {
                if (IsWord(n)) {
                    return Scripts(Run(WordText(n), n.children[0].face), n);
                }
                return Scripts(Items(n.children), n);
            }
            case MathKind::Frac:
                return Scripts(std::string("<m:f>") + (n.frac_bar ? "" : "<m:fPr><m:type m:val=\"noBar\"/></m:fPr>") +
                                   Arg("num", {n.children[0]}) + Arg("den", {n.children.size() > 1 ? n.children[1] : MathNode{}}) + "</m:f>",
                               n);
            case MathKind::Sqrt:
                return Scripts("<m:rad><m:radPr><m:degHide m:val=\"1\"/></m:radPr><m:deg/>" + Arg("e", n.children) + "</m:rad>", n);
            case MathKind::Space: {
                const double em = static_cast<double>(n.space_em);
                if (em <= 0.0) return "";
                std::string sp;
                double left = em;
                while (left >= 0.95) {
                    sp += Utf8(0x2003);
                    left -= 1.0;
                }
                if (left >= 0.4) sp += Utf8(0x2002);
                else if (left >= 0.27) sp += Utf8(0x2004);
                else if (left >= 0.1) sp += Utf8(0x2009);
                return sp.empty() ? "" : Run(sp, MathFace::Upright);
            }
            case MathKind::Fenced:
                return Scripts("<m:d><m:dPr><m:begChr m:val=\"" + XmlEsc(n.open_delim) + "\"/><m:endChr m:val=\"" + XmlEsc(n.close_delim) +
                                   "\"/></m:dPr>" + Arg("e", n.children) + "</m:d>",
                               n);
            case MathKind::Accent: {
                if (n.accent_brace)
                    return Scripts("<m:groupChr><m:groupChrPr><m:chr m:val=\"" + Utf8(n.accent_below ? 0x23DF : 0x23DE) + "\"/><m:pos m:val=\"" +
                                       std::string(n.accent_below ? "bot" : "top") + "\"/><m:vertJc m:val=\"" + (n.accent_below ? "top" : "bot") +
                                       "\"/></m:groupChrPr>" + Arg("e", n.children) + "</m:groupChr>",
                                   n);
                if (n.accent.empty() && n.accent_stretch)
                    return Scripts("<m:bar><m:barPr><m:pos m:val=\"" + std::string(n.accent_below ? "bot" : "top") + "\"/></m:barPr>" +
                                       Arg("e", n.children) + "</m:bar>",
                                   n);
                const int comb = CombiningAccent(n.accent, n.accent_below);
                return Scripts("<m:acc><m:accPr><m:chr m:val=\"" + XmlEsc(Utf8(comb ? comb : 0x302)) + "\"/></m:accPr>" + Arg("e", n.children) +
                                   "</m:acc>",
                               n);
            }
            case MathKind::Matrix: {
                const size_t cols = static_cast<size_t>(std::max(1, n.cols));
                std::string o = "<m:m>";
                if (n.cells_pair_align) {
                    o += "<m:mPr><m:mcs>";
                    for (size_t c = 0; c < cols; ++c)
                        o += std::string("<m:mc><m:mcPr><m:count m:val=\"1\"/><m:mcJc m:val=\"") + (c % 2 == 0 ? "right" : "left") +
                             "\"/></m:mcPr></m:mc>";
                    o += "</m:mcs></m:mPr>";
                } else if (n.cells_left_align) {
                    o += "<m:mPr><m:mcs><m:mc><m:mcPr><m:count m:val=\"" + std::to_string(cols) +
                         "\"/><m:mcJc m:val=\"left\"/></m:mcPr></m:mc></m:mcs></m:mPr>";
                }
                for (size_t r = 0; r * cols < n.cells.size(); ++r) {
                    o += "<m:mr>";
                    for (size_t c = 0; c < cols && r * cols + c < n.cells.size(); ++c) o += Arg("e", {n.cells[r * cols + c]});
                    o += "</m:mr>";
                }
                return Scripts(o + "</m:m>", n);
            }
            case MathKind::Phantom: return Scripts("<m:phant>" + Arg("e", n.children) + "</m:phant>", n);
        }
        return "";
    }
};

// --- Text runs --------------------------------------------------------------------

struct TextRunWriter {
    std::vector<MathTextRun> out;

    void Put(const std::string &text, bool italic, bool bold, int script) {
        if (text.empty()) return;
        if (!out.empty() && out.back().italic == italic && out.back().bold == bold && out.back().script == script) {
            out.back().text += text;
            return;
        }
        MathTextRun r;
        r.text = text;
        r.italic = italic;
        r.bold = bold;
        r.script = script;
        out.push_back(std::move(r));
    }

    // Whether `n` reads as one piece written inline (no parentheses needed
    // round it as a fraction's numerator, a root's radicand).
    static bool Simple(const MathNode &n) {
        if (n.kind == MathKind::Text || n.kind == MathKind::Fenced) return true;
        if (n.kind != MathKind::Row) return false;
        if (IsWord(n)) return true;
        int atoms = 0;
        for (const MathNode &c : n.children) {
            if (c.kind == MathKind::Row && c.children.empty()) continue;
            if (c.kind != MathKind::Text || !IsDigits(c.text)) ++atoms;
        }
        if (atoms > 1) return false;
        for (const MathNode &c : n.children)
            if (!Simple(c)) return false;
        return true;
    }

    void Wrapped(const MathNode &n, int script) {
        const bool paren = !Simple(n);
        if (paren) Put("(", false, false, script);
        Node(n, script);
        if (paren) Put(")", false, false, script);
    }

    void Scripts(const MathNode &n, int script) {
        // Nested scripts stay at the one level text can show.
        for (const MathNode &s : n.sub) Node(s, script != 0 ? script : -1);
        for (const MathNode &s : n.sup) Node(s, script != 0 ? script : 1);
    }

    void Row(const std::vector<MathNode> &kids, int script) {
        std::vector<MathNode> terms;
        for (const MathNode &c : kids)
            if (!(c.kind == MathKind::Row && c.children.empty() && !HasScripts(c))) terms.push_back(c);
        const std::vector<MathClass> classes = MathRowClasses(terms);
        for (size_t k = 0; k < terms.size(); ++k) {
            // Air round relations and binary operators, as TeX sets it.
            const bool spaced = script == 0 && (classes[k] == MathClass::Rel || classes[k] == MathClass::Bin);
            if (spaced && k > 0) Put(" ", false, false, script);
            Node(terms[k], script);
            if (spaced && k + 1 < terms.size()) Put(" ", false, false, script);
            if (script == 0 && classes[k] == MathClass::Punct && k + 1 < terms.size()) Put(" ", false, false, script);
        }
    }

    void Node(const MathNode &n, int script) {
        switch (n.kind) {
            case MathKind::Text:
                Put(n.text, n.face == MathFace::Italic || n.face == MathFace::BoldItalic, n.face == MathFace::Bold || n.face == MathFace::BoldItalic,
                    script);
                break;
            case MathKind::Row:
                if (IsWord(n)) {
                    const MathFace f = n.children[0].face;
                    Put(WordText(n), false, f == MathFace::Bold, script);
                    if (n.cls == MathClass::Op && !HasScripts(n) && script == 0) Put(" ", false, false, script);
                } else {
                    Row(n.children, script);
                }
                break;
            case MathKind::Frac:
                Wrapped(n.children[0], script);
                Put("/", false, false, script);
                if (n.children.size() > 1) Wrapped(n.children[1], script);
                break;
            case MathKind::Sqrt:
                Put(Utf8(0x221A), false, false, script);
                if (!n.children.empty()) Wrapped(n.children[0], script);
                break;
            case MathKind::Space:
                if (static_cast<double>(n.space_em) >= 0.25) Put(" ", false, false, script);
                else if (static_cast<double>(n.space_em) >= 0.1) Put(Utf8(0x2009), false, false, script);
                break;
            case MathKind::Fenced:
                Put(n.open_delim, false, false, script);
                for (const MathNode &c : n.children) Node(c, script);
                Put(n.close_delim, false, false, script);
                break;
            case MathKind::Accent: {
                const size_t before = out.size();
                for (const MathNode &c : n.children) Node(c, script);
                // The mark combines with the last character set.
                const int comb = n.accent_brace ? 0 : CombiningAccent(n.accent, n.accent_below);
                if (comb != 0 && out.size() > before) out.back().text += Utf8(comb);
                break;
            }
            case MathKind::Matrix: {
                const size_t cols = static_cast<size_t>(std::max(1, n.cols));
                for (size_t k = 0; k < n.cells.size(); ++k) {
                    if (k > 0) Put(k % cols == 0 ? "; " : ", ", false, false, script);
                    Node(n.cells[k], script);
                }
                break;
            }
            case MathKind::Phantom: break;
        }
        Scripts(n, script);
    }
};

// --- Extent ------------------------------------------------------------------------
//
// Calibrated against LibreOffice Math's own layout of the same MathML (the
// visual area it gives a formula object): a formula object is stretched to
// whatever frame it is given, so the frame has to be the formula's size.

struct Extent {
    // Above and below the maths axis's baseline, and across.
    double w = 0, asc = kAsc, desc = kDesc;
    static constexpr double kAsc = 0.8, kDesc = 0.3;
    double h() const { return asc + desc; }
};

// One glyph's advance, in ems.
double GlyphEm(int cp) {
    if (cp >= 'a' && cp <= 'z') return 0.6;
    if (cp >= 'A' && cp <= 'Z') return 0.72;
    if (cp >= '0' && cp <= '9') return 0.5;
    if (cp >= 0x3B1 && cp <= 0x3C9) return 0.6;
    if (cp >= 0x391 && cp <= 0x3A9) return 0.7;
    if (cp == '|' || cp == 0x2223) return 0.45;
    if (cp == '/') return 0.9;  // (set with an operator's air round it)
    if (cp == '(' || cp == ')' || cp == '[' || cp == ']' || cp == ',' || cp == '.' || cp == ';' || cp == ':') return 0.33;
    return 0.6;
}

Extent Measure(const MathNode &n, bool display, bool script) {
    Extent e;
    auto row = [&](const std::vector<MathNode> &kids) {
        Extent r;
        for (const MathNode &c : kids) {
            const Extent x = Measure(c, display, script);
            r.w += x.w;
            r.asc = std::max(r.asc, x.asc);
            r.desc = std::max(r.desc, x.desc);
        }
        return r;
    };
    switch (n.kind) {
        case MathKind::Text: {
            e.w = 0;
            for (size_t k = 0; k < n.text.size(); ++k)
                if ((static_cast<unsigned char>(n.text[k]) & 0xC0) != 0x80) e.w += GlyphEm(FirstCp(n.text.substr(k)));
            // The air TeX (and LibreOffice) puts round a relation or an operator.
            if (!script && (n.cls == MathClass::Rel || n.cls == MathClass::Bin)) e.w += 0.1;
            if (!script && n.cls == MathClass::Punct) e.w += 0.2;
            if (n.big_op && display) {
                e.w = 1.0;
                e.asc = 1.2;
                e.desc = 0.4;
            }
            break;
        }
        case MathKind::Row: e = row(n.children); break;
        case MathKind::Frac: {
            const Extent a = Measure(n.children[0], display, script),
                         b = n.children.size() > 1 ? Measure(n.children[1], display, script) : Extent{};
            e.w = std::max(a.w, b.w) + 0.2;
            e.asc = a.h() + 0.3;
            e.desc = b.h() - 0.15;
            break;
        }
        case MathKind::Sqrt: e = row(n.children); e.w += 0.6; e.asc += 0.1; break;
        case MathKind::Space: e.w = static_cast<double>(n.space_em); break;
        case MathKind::Fenced:
            e = row(n.children);
            e.w += 0.6;
            // Fences stretched round something taller than a line overshoot
            // it -- save a matrix's, which LibreOffice fits to the rows.
            if (e.h() > 1.3 && (!n.open_delim.empty() || !n.close_delim.empty()) &&
                !(n.children.size() == 1 && n.children[0].kind == MathKind::Matrix)) {
                e.asc += 0.12;
                e.desc += 0.12;
            }
            break;
        case MathKind::Accent:
            e = row(n.children);
            if (n.accent_brace) (n.accent_below ? e.desc : e.asc) += 0.35;
            else e.asc += 0.1;
            break;
        case MathKind::Matrix: {
            const size_t cols = static_cast<size_t>(std::max(1, n.cols));
            std::vector<double> widths(cols, 0.0);
            double h = 0;
            for (size_t r = 0; r * cols < n.cells.size(); ++r) {
                double rh = Extent().h();
                for (size_t c = 0; c < cols && r * cols + c < n.cells.size(); ++c) {
                    const Extent x = Measure(n.cells[r * cols + c], display, script);
                    widths[c] = std::max(widths[c], x.w);
                    rh = std::max(rh, x.h());
                }
                h += rh + 0.15;
            }
            for (double wd : widths) e.w += wd + 0.45;
            // Centred on the axis.
            e.asc = std::max(Extent::kAsc, h / 2 + 0.25);
            e.desc = std::max(Extent::kDesc, h / 2 - 0.25);
            break;
        }
        case MathKind::Phantom: e = row(n.children); break;
    }
    if (HasScripts(n)) {
        const bool limits = (display && n.limits_above) || n.limits_always;
        double sw = 0;
        for (const MathNode &sn : n.sub) {
            const Extent x = Measure(sn, display, true);
            sw = std::max(sw, x.w * 0.7);
            e.desc += limits ? x.h() * 0.55 : 0.15 + (x.h() - Extent().h()) * 0.7;
        }
        for (const MathNode &sn : n.sup) {
            const Extent x = Measure(sn, display, true);
            sw = std::max(sw, x.w * 0.7);
            e.asc += limits ? x.h() * 0.55 : 0.15 + (x.h() - Extent().h()) * 0.7;
        }
        if (limits) e.w = std::max(e.w, sw);
        else e.w += sw;
    }
    return e;
}

}  // namespace

std::string TexToMathMl(const std::string &latex, bool display) {
    MathMlWriter w;
    w.display = display;
    const MathNode root = ParseTexMath(latex);
    std::string body = w.Group({root});
    if (body.rfind("<mrow", 0) != 0) body = "<mrow>" + body + "</mrow>";
    return std::string("<math xmlns=\"http://www.w3.org/1998/Math/MathML\" display=\"") + (display ? "block" : "inline") +
           "\"><semantics>" + body + "<annotation encoding=\"application/x-tex\">" + XmlEsc(latex) + "</annotation></semantics></math>";
}

std::string TexToMathMlBody(const std::string &latex, bool display) {
    MathMlWriter w;
    w.display = display;
    return w.Group({ParseTexMath(latex)});
}

std::string TexToStarMath(const std::string &latex, bool display) {
    StarMathWriter w;
    w.display = display;
    return w.Node(ParseTexMath(latex));
}

std::string TexToLibreOfficeMathMl(const std::string &latex, bool display) {
    std::string body = TexToMathMlBody(latex, display);
    if (body.rfind("<mrow", 0) != 0) body = "<mrow>" + body + "</mrow>";
    return std::string("<math xmlns=\"http://www.w3.org/1998/Math/MathML\" display=\"") + (display ? "block" : "inline") +
           "\"><semantics>" + body + "<annotation encoding=\"StarMath 5.0\">" + XmlEsc(TexToStarMath(latex, display)) +
           "</annotation><annotation encoding=\"application/x-tex\">" + XmlEsc(latex) + "</annotation></semantics></math>";
}

bool TexNeedsLayout(const std::string &latex) { return NeedsLayout(ParseTexMath(latex)); }

std::string TexToOmml(const std::string &latex, const std::string &run_props) {
    OmmlWriter w;
    w.run_props = run_props;
    // (The caller's <m:oMathPara> is what makes it display maths.)
    w.display = true;
    const MathNode root = ParseTexMath(latex);
    return "<m:oMath>" + w.Items({root}) + "</m:oMath>";
}

std::vector<MathTextRun> TexToTextRuns(const std::string &latex) {
    TextRunWriter w;
    w.Node(ParseTexMath(latex), 0);
    return w.out;
}

std::string TexToPlainText(const std::string &latex) {
    std::string t;
    for (const MathTextRun &r : TexToTextRuns(latex)) t += r.text;
    return t;
}

void TexMathExtent(const std::string &latex, bool display, double *width_em, double *height_em, double *ascent_em) {
    const Extent e = Measure(ParseTexMath(latex), display, false);
    if (width_em) *width_em = e.w;
    if (height_em) *height_em = e.h();
    if (ascent_em) *ascent_em = e.asc;
}
