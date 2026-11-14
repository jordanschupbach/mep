// mep's APL formatter: see apl_format.h.
#include "apl_format.h"

#include <cctype>
#include <vector>

#include "apl_lex.h"

namespace aplfmt {

namespace {

using apl::Tok;
using apl::Token;

std::string Upper(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

enum class Frame { Tradfn, Ctl, Brace, Group };

struct Open {
    Frame kind;
    std::string word;  // for Ctl: the opening word, upper-case without ':'
    int indent = 0;    // the opener line's (formatted) indentation
    int body = 0;      // the body's indentation
    int line = 0;      // 1-based line of the opener
};

const char *EndOf(const std::string &w) {
    static const char *const kPairs[][2] = {
        {"IF", "ENDIF"},         {"FOR", "ENDFOR"},           {"WHILE", "ENDWHILE"},   {"REPEAT", "ENDREPEAT"},
        {"SELECT", "ENDSELECT"}, {"TRAP", "ENDTRAP"},         {"WITH", "ENDWITH"},     {"HOLD", "ENDHOLD"},
        {"SECTION", "ENDSECTION"}, {"PROPERTY", "ENDPROPERTY"}, {"DISPOSABLE", "ENDDISPOSABLE"},
        {"NAMESPACE", "ENDNAMESPACE"}, {"CLASS", "ENDCLASS"}, {"INTERFACE", "ENDINTERFACE"},
    };
    for (const auto &p : kPairs) {
        if (w == p[0]) return p[1];
    }
    return nullptr;
}

bool IsMidWord(const std::string &w) {
    return w == "ELSE" || w == "ELSEIF" || w == "ANDIF" || w == "ORIF" || w == "CASE" || w == "CASELIST";
}

bool IsCloser(const std::string &w) { return w == "END" || w == "UNTIL" || (w.size() > 3 && w.compare(0, 3, "END") == 0); }

std::vector<std::string> SplitLines(std::string_view src) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : src) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    lines.push_back(cur);
    return lines;
}

int LeadingWidth(const std::string &line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') n++;
        else if (c == '\t') n += 4 - n % 4;
        else break;
    }
    return n;
}

std::string RTrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\f')) s.pop_back();
    return s;
}

// The comment's text compared without the spaces after its ⍝.
std::string CommentKey(const std::string &c) {
    const std::string lamp = "⍝";
    if (c.compare(0, lamp.size(), lamp) != 0) return c;
    size_t i = lamp.size();
    while (i < c.size() && c[i] == ' ') i++;
    return lamp + RTrim(c.substr(i));
}

// The comment normalised: one space after ⍝ when text starts right after it.
std::string NormalComment(const std::string &c) {
    const std::string lamp = "⍝";
    std::string out = RTrim(c);
    if (out.compare(0, lamp.size(), lamp) != 0 || out.size() == lamp.size()) return out;
    const unsigned char next = static_cast<unsigned char>(out[lamp.size()]);
    if (std::isalnum(next) || next >= 0x80) {
        // ⍝⍝ / ⍝⍵ ... are kept: only plain text gets the space (letters, digits, non-glyph Unicode).
        if (next >= 0x80) {
            const std::string cp = out.substr(lamp.size(), static_cast<size_t>(apl::Utf8Len(next)));
            if (!apl::IsNameStart(cp)) return out;
        }
        out.insert(lamp.size(), " ");
    }
    return out;
}

// One line rebuilt from its tokens: original spacing except around ← and ⋄
// and before a trailing comment.
std::string Rebuild(const std::string &line, const std::vector<Token> &toks) {
    std::string out;
    for (size_t i = 0; i < toks.size(); i++) {
        const Token &t = toks[i];
        if (i > 0) {
            const Token &p = toks[i - 1];
            std::string gap = line.substr(static_cast<size_t>(p.end), static_cast<size_t>(t.col - p.end));
            if (p.kind == Tok::Assign || (t.kind == Tok::Assign)) gap.clear();
            if (p.kind == Tok::Diamond || t.kind == Tok::Diamond) gap = " ";
            if (t.kind == Tok::Comment && gap.empty()) gap = " ";
            if (p.kind == Tok::Assign && t.kind == Tok::Comment) gap = " ";
            if (p.kind == Tok::Assign && t.kind == Tok::Diamond) gap = " ";
            // Tabs inside a line become spaces of the same count.
            for (char &c : gap) {
                if (c == '\t') c = ' ';
            }
            out += gap;
        }
        out += t.kind == Tok::Comment ? NormalComment(t.text) : t.text;
    }
    return out;
}

struct Fail {
    int line;
    std::string message;
};

}  // namespace

Result Format(std::string_view src, const Options &opts) {
    Result r;
    r.text = std::string(src);
    const int unit = opts.indent > 0 ? opts.indent : 4;
    const std::vector<std::string> lines = SplitLines(src);
    std::vector<std::string> out;
    out.reserve(lines.size());
    std::vector<Open> stack;
    auto fail = [&](int line, const std::string &msg) {
        r.ok = false;
        r.text = std::string(src);
        r.error = msg;
        r.error_line = line;
        return r;
    };
    auto top_kind = [&](Frame k) { return !stack.empty() && stack.back().kind == k; };
    auto brace_depth = [&]() {
        int n = 0;
        for (const Open &o : stack) n += o.kind == Frame::Brace;
        return n;
    };

    for (size_t li = 0; li < lines.size(); li++) {
        const int ln = static_cast<int>(li) + 1;
        const std::string &line = lines[li];
        const std::vector<Token> toks = apl::LexLine(line);
        for (const Token &t : toks) {
            if (t.kind == Tok::BadString) return fail(ln, "unterminated string");
        }
        if (toks.empty()) {
            out.push_back(std::string());
            continue;
        }
        if (toks[0].kind == Tok::SysCommand || (toks[0].kind == Tok::Comment && toks[0].text.compare(0, 2, "#!") == 0)) {
            out.push_back(RTrim(line));
            continue;
        }
        const bool continuation = top_kind(Frame::Group);
        // The first code token decides whether the line closes or continues a block.
        const Token *first = nullptr;
        for (const Token &t : toks) {
            if (t.kind != Tok::Comment) {
                first = &t;
                break;
            }
        }
        int indent = stack.empty() ? LeadingWidth(line) : stack.back().body;
        if (first && !continuation) {
            if (first->kind == Tok::Keyword) {
                const std::string w = Upper(first->text.substr(1));
                if ((IsMidWord(w) || IsCloser(w)) && top_kind(Frame::Ctl)) indent = stack.back().indent;
            } else if (first->kind == Tok::RBrace && top_kind(Frame::Brace)) {
                indent = stack.back().indent;
            } else if (first->kind == Tok::Del && top_kind(Frame::Tradfn) && brace_depth() == 0) {
                indent = stack.back().indent;
            }
        }
        if (continuation) indent = LeadingWidth(line);
        // Walk the tokens: open and close frames.
        bool stmt_start = true;
        for (size_t i = 0; i < toks.size(); i++) {
            const Token &t = toks[i];
            const bool at_start = stmt_start;
            stmt_start = false;
            switch (t.kind) {
            case Tok::Diamond:
                stmt_start = true;
                break;
            case Tok::Keyword: {
                if (!at_start || brace_depth() > 0 || top_kind(Frame::Group)) break;
                const std::string w = Upper(t.text.substr(1));
                if (EndOf(w)) {
                    stack.push_back({Frame::Ctl, w, indent, indent + unit, ln});
                } else if (IsCloser(w)) {
                    if (!top_kind(Frame::Ctl)) return fail(ln, t.text + " without a matching opening word");
                    const Open &o = stack.back();
                    if (w == "UNTIL" ? (o.word != "REPEAT" && o.word != "WHILE")
                                     : (w != "END" && EndOf(o.word) != w)) {
                        return fail(ln, t.text + " does not match :" + o.word + " on line " + std::to_string(o.line));
                    }
                    stack.pop_back();
                }
                break;
            }
            case Tok::Name:
                // A label: a statement may follow it.
                if (at_start && i + 1 < toks.size() && toks[i + 1].kind == Tok::Colon && brace_depth() == 0) {
                    i++;
                    stmt_start = true;
                }
                break;
            case Tok::Del:
                if (i == 0 && brace_depth() == 0 && !top_kind(Frame::Group)) {
                    if (top_kind(Frame::Tradfn)) {
                        stack.pop_back();
                    } else {
                        for (const Open &o : stack) {
                            if (o.kind == Frame::Tradfn) {
                                const Open &b = stack.back();
                                return fail(b.line, b.kind == Frame::Ctl ? ":" + b.word + " is not closed before the ∇ on line " + std::to_string(ln)
                                                                         : "unclosed bracket before the ∇ on line " + std::to_string(ln));
                            }
                        }
                        stack.push_back({Frame::Tradfn, "", indent, indent + 2, ln});
                    }
                    i = toks.size();  // the rest of a ∇ line is the header (or ⍫)
                }
                break;
            case Tok::LBrace:
                stack.push_back({Frame::Brace, "", indent, indent + unit, ln});
                stmt_start = true;
                break;
            case Tok::LParen:
            case Tok::LBracket:
                stack.push_back({Frame::Group, "", indent, indent, ln});
                break;
            case Tok::RBrace:
            case Tok::RParen:
            case Tok::RBracket: {
                const Frame want = t.kind == Tok::RBrace ? Frame::Brace : Frame::Group;
                if (!top_kind(want)) return fail(ln, "unbalanced '" + t.text + "'");
                stack.pop_back();
                break;
            }
            default:
                break;
            }
        }
        std::string text = Rebuild(line, toks);
        out.push_back(text.empty() ? text : std::string(static_cast<size_t>(indent), ' ') + text);
    }
    if (!stack.empty()) {
        const Open &o = stack.back();
        const char *what = o.kind == Frame::Tradfn ? "function (∇)" : o.kind == Frame::Brace ? "'{'"
                           : o.kind == Frame::Group ? "bracket" : "control structure";
        // A Dyalog .aplf body has no ∇ and is fine; an open ∇ isn't.
        return fail(o.line, std::string("unclosed ") + what);
    }
    std::string result;
    for (size_t i = 0; i < out.size(); i++) {
        if (i) result += '\n';
        result += out[i];
    }
    // Verify: the same tokens, line by line.
    const std::vector<std::string> after = SplitLines(result);
    if (after.size() != lines.size()) return fail(0, "internal error: the line count changed");
    for (size_t li = 0; li < lines.size(); li++) {
        const std::vector<Token> a = apl::LexLine(lines[li]);
        const std::vector<Token> b = apl::LexLine(after[li]);
        bool same = a.size() == b.size();
        for (size_t k = 0; same && k < a.size(); k++) {
            same = a[k].kind == b[k].kind &&
                   (a[k].kind == Tok::Comment ? CommentKey(a[k].text) == CommentKey(b[k].text)
                    : a[k].kind == Tok::SysCommand ? RTrim(a[k].text) == RTrim(b[k].text)
                                                    : a[k].text == b[k].text);
        }
        if (!same) return fail(static_cast<int>(li) + 1, "formatting would change the code here; left unformatted");
    }
    r.ok = true;
    r.text = result;
    return r;
}

}  // namespace aplfmt
