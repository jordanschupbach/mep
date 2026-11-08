#include "scheme_format.h"

#include <algorithm>
#include <climits>
#include <iterator>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Two halves, the same two as lisp_format.cpp's. The whitespace rules work
// on the gaps between the tokens Guile's reader sees (Lex). The
// indentation is Emacs's, ported function by function from lisp-mode.el
// (`lisp-indent-region', `lisp-indent-calc-next', `calculate-lisp-indent',
// `lisp-indent-specform', `lisp-indent-defform') and scheme.el
// (`scheme-indent-function', `scheme-let-indent', the syntax table and the
// `#;' datum comments its syntax-propertize function marks) over a small
// model of the Emacs primitives they use -- `parse-partial-sexp',
// `forward-sexp', `current-column' -- so that the corners come out the way
// Emacs's do. The engine is lisp_format.cpp's with scheme-mode's syntax
// table and indent function in place of lisp-mode's and cl-indent's.
// Comments name the Emacs function each piece stands for.

namespace schemefmt {
namespace {

// ============================================================================ reading

enum class TK { Open, Close, Atom, String, Prefix, Comment, Block };

struct Tok {
    size_t a = 0, b = 0;
    TK k = TK::Atom;
};

bool IsWhite(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// Guile's delimiters (read.scm's `delimiter?', square brackets on).
bool IsDelimiter(char c) { return IsWhite(c) || c == '(' || c == ')' || c == '[' || c == ']' || c == '"' || c == ';'; }

int LineAt(const std::string &s, size_t off) {
    return 1 + static_cast<int>(std::count(s.begin(), s.begin() + static_cast<long>(std::min(off, s.size())), '\n'));
}

size_t Utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 6) return 2;
    if ((c >> 4) == 14) return 3;
    if ((c >> 3) == 30) return 4;
    return 1;
}

/** @brief The bytes of the character at `k`: a whole UTF-8 sequence, or one byte when it is not one. */
size_t CharLen(const std::string &s, size_t k) {
    const size_t len = Utf8Len(static_cast<unsigned char>(s[k]));
    for (size_t j = 1; j < len; j++) {
        if (k + j >= s.size() || (static_cast<unsigned char>(s[k + j]) & 0xC0) != 0x80) return 1;
    }
    return len;
}

/** @brief Where the string whose body starts at `k` ends: its closing `close`, or the size when unclosed. */
size_t StringEnd(const std::string &s, size_t k, char close) {
    while (k < s.size() && s[k] != close) k += s[k] == '\\' ? 2 : 1;
    return std::min(k, s.size());
}

struct Lexed {
    std::vector<Tok> toks;
    bool ok = false;
    std::string error;
    int error_line = 0;
};

// The `#!' words that are reader directives rather than the start of a
// `#! ... !#' comment. `#!eof' ends the file: what follows is not read.
bool IsDirectiveWord(const std::string &word) {
    static const char *const kWords[] = {"r6rs", "fold-case", "no-fold-case", "curly-infix",
                                         "curly-infix-and-bracket-lists", "eof"};
    for (const char *w : kWords)
        if (word == w) return true;
    return false;
}

bool IsDirective(const std::string &s, size_t i, size_t *end) {
    size_t e = i + 2;
    while (e < s.size() && !IsDelimiter(s[e])) e++;
    if (!IsDirectiveWord(s.substr(i + 2, e - i - 2))) return false;
    *end = e;
    return true;
}

/**
 * @brief Splits Scheme source into the tokens Guile's reader sees; fails on what cannot be balanced.
 * Strings, comments, `#\' characters and `#{...}#' symbols are single tokens, so the whitespace rules
 * never reach inside them.
 */
Lexed Lex(const std::string &s) {
    Lexed r;
    const size_t n = s.size();
    std::vector<std::pair<size_t, char>> opens;
    bool bad = false;
    size_t bad_at = 0;
    const char *bad_msg = "";
    auto fail = [&](const char *msg, size_t at) {
        if (!bad) {
            bad = true;
            bad_msg = msg;
            bad_at = at;
        }
    };
    auto atom_end = [&](size_t k) {
        while (k < n && !IsDelimiter(s[k])) k++;
        return k;
    };
    auto push = [&](size_t a, size_t b, TK k) { r.toks.push_back(Tok{a, b, k}); };
    auto open = [&](size_t a, size_t b, char close) {
        opens.emplace_back(a, close);
        push(a, b, TK::Open);
    };
    size_t i = 0;
    while (i < n && !bad) {
        const char c = s[i];
        if (IsWhite(c)) {
            i++;
        } else if (c == ';') {
            size_t e = s.find('\n', i);
            if (e == std::string::npos) e = n;
            push(i, e, TK::Comment);
            i = e;
        } else if (c == '"' || c == '|') {
            // A string, or an R7RS `|...|' symbol (a string to Emacs too).
            const size_t k = StringEnd(s, i + 1, c);
            if (k >= n) {
                fail("unterminated string", i);
                break;
            }
            push(i, k + 1, TK::String);
            i = k + 1;
        } else if (c == '(' || c == '[') {
            open(i, i + 1, c == '(' ? ')' : ']');
            i++;
        } else if (c == ')' || c == ']') {
            if (opens.empty()) {
                fail("unexpected close paren", i);
                break;
            }
            if (opens.back().second != c) {
                fail("mismatched close paren", i);
                break;
            }
            opens.pop_back();
            push(i, i + 1, TK::Close);
            i++;
        } else if (c == '\'' || c == '`') {
            push(i, i + 1, TK::Prefix);
            i++;
        } else if (c == ',') {
            const size_t e = i + 1 < n && s[i + 1] == '@' ? i + 2 : i + 1;
            push(i, e, TK::Prefix);
            i = e;
        } else if (c == '#' && i + 1 < n) {
            const char d = s[i + 1];
            if (d == '|') {
                int depth = 1;
                size_t k = i + 2;
                while (k < n) {
                    if (s[k] == '|' && k + 1 < n && s[k + 1] == '#') {
                        k += 2;
                        if (--depth == 0) break;
                    } else if (s[k] == '#' && k + 1 < n && s[k + 1] == '|') {
                        k += 2;
                        depth++;
                    } else {
                        k++;
                    }
                }
                if (depth > 0) {
                    fail("unterminated #| comment", i);
                    break;
                }
                push(i, k, TK::Block);
                i = k;
            } else if (d == '!') {
                size_t e = 0;
                if (IsDirective(s, i, &e)) {
                    if (s.compare(i, e - i, "#!eof") == 0 && opens.empty()) {
                        // At the top level the rest of the file is not read: one block, its trailing blank
                        // lines left out. (Inside a list it is the end-of-file object.)
                        const size_t last = s.find_last_not_of(" \t\r\n");
                        push(i, std::max(e, last + 1), TK::Block);
                        i = n;
                        continue;
                    }
                    push(i, e, TK::Atom);
                    i = e;
                    continue;
                }
                const size_t k = s.find("!#", i + 2);
                if (k == std::string::npos) {
                    // Chez's `#!chezscheme', `#!eof': a word, not a comment.
                    push(i, atom_end(i + 2), TK::Atom);
                    i = atom_end(i + 2);
                    continue;
                }
                push(i, k + 2, TK::Block);
                i = k + 2;
            } else if (d == '\\') {
                // A character: one character, then whatever constituents follow (#\space, #\x41).
                size_t k = i + 2;
                if (k >= n) {
                    fail("unterminated character", i);
                    break;
                }
                k = std::min(n, k + CharLen(s, k));
                push(i, atom_end(k), TK::Atom);
                i = atom_end(k);
            } else if (d == '{') {
                // Guile's `#{...}#' symbol; Chez's `#{name unique}' gensym ends at the brace.
                const size_t k = s.find('}', i + 2);
                if (k == std::string::npos) {
                    fail("unterminated #{ symbol", i);
                    break;
                }
                const size_t e = k + 1 < n && s[k + 1] == '#' ? k + 2 : k + 1;
                push(i, e, TK::Atom);
                i = e;
            } else if (d == '[') {
                // Gauche's `#[a-z]' character set: a string to the layout.
                const size_t k = StringEnd(s, i + 2, ']');
                if (k >= n) {
                    fail("unterminated #[ character set", i);
                    break;
                }
                push(i, k + 1, TK::String);
                i = k + 1;
            } else if (d == ';' || d == '\'' || d == '`' || d == '~') {
                push(i, i + 2, TK::Prefix);
                i += 2;
            } else if (d == ',' || d == '$' || d == '+') {
                const size_t e = i + 2 < n && s[i + 2] == '@' ? i + 3 : i + 2;
                push(i, e, TK::Prefix);
                i = e;
            } else if (d == '"' || d == '/') {
                // Gauche's `#"...~(x)"' interpolation and `#/regex/' literal: strings to Emacs.
                const size_t k = StringEnd(s, i + 2, d);
                if (k >= n) {
                    fail("unterminated string", i);
                    break;
                }
                push(i, k + 1, TK::String);
                i = k + 1;
            } else if (d == '(') {
                open(i, i + 2, ')');
                i += 2;
            } else {
                // #t, #:key, #x1F, and the array / bytevector openers #vu8( #u8( #f64( #2(.
                size_t k = i + 1;
                while (k < n && !IsDelimiter(s[k])) k++;
                if (k < n && s[k] == '(') {
                    open(i, k + 1, ')');
                    i = k + 1;
                } else {
                    push(i, k, TK::Atom);
                    i = k;
                }
            }
        } else {
            const size_t e = std::max(atom_end(i), i + 1);
            push(i, e, TK::Atom);
            i = e;
        }
    }
    if (!bad && !opens.empty()) fail("unclosed paren", opens.front().first);
    if (bad) {
        r.error = bad_msg;
        r.error_line = LineAt(s, bad_at);
        return r;
    }
    r.ok = true;
    return r;
}

/** @brief The last token of the datum starting at token `k` (comments skipped); npos when there is none. */
size_t DatumLast(const std::string &s, const std::vector<Tok> &t, size_t k) {
    while (k < t.size() && (t[k].k == TK::Comment || t[k].k == TK::Block)) k++;
    if (k >= t.size()) return std::string::npos;
    switch (t[k].k) {
        case TK::Prefix: {
            if (s.compare(t[k].a, t[k].b - t[k].a, "#;") == 0) {
                // A commented datum, then the datum this prefix applies to.
                const size_t j = DatumLast(s, t, k + 1);
                return j == std::string::npos ? j : DatumLast(s, t, j + 1);
            }
            return DatumLast(s, t, k + 1);
        }
        case TK::Open: {
            int depth = 0;
            for (; k < t.size(); k++) {
                if (t[k].k == TK::Open) depth++;
                if (t[k].k == TK::Close && --depth == 0) return k;
            }
            return std::string::npos;
        }
        case TK::Close: return std::string::npos;
        default: return k;
    }
}

/** @brief Turns each `#;' and the datum it comments out into one Block token: text no rule may touch. */
void MergeDatumComments(const std::string &s, std::vector<Tok> &t) {
    std::vector<Tok> out;
    out.reserve(t.size());
    for (size_t k = 0; k < t.size(); k++) {
        if (t[k].k == TK::Prefix && s.compare(t[k].a, t[k].b - t[k].a, "#;") == 0) {
            const size_t e = DatumLast(s, t, k + 1);
            if (e != std::string::npos) {
                out.push_back(Tok{t[k].a, t[e].b, TK::Block});
                k = e;
                continue;
            }
        }
        out.push_back(t[k]);
    }
    t = std::move(out);
}

std::string RStripBlanks(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

std::string FixLines(std::string_view gap);

/**
 * @brief A multi-line `#!...!#' comment: its lines lose their trailing blanks, as a `;' comment's do
 * (and as Emacs, which reads it as code, strips them).
 */
bool IsBangBlock(const Tok &t, const std::string &text) {
    return t.k == TK::Block && text.compare(0, 2, "#!") == 0 && text.find('\n') != std::string::npos;
}

/** @brief The tokens, for the check: kinds and texts, no whitespace (and none at a comment's end). */
std::vector<std::string> Signature(const std::string &s, const std::vector<Tok> &toks) {
    std::vector<std::string> out;
    out.reserve(toks.size());
    for (const Tok &t : toks) {
        std::string text = s.substr(t.a, t.b - t.a);
        if (t.k == TK::Comment) text = RStripBlanks(text);
        if (IsBangBlock(t, text)) text = FixLines(text);
        out.push_back(std::to_string(static_cast<int>(t.k)) + text);
    }
    return out;
}

// ============================================================================ the gaps

/** @brief A gap holding newlines: no trailing blanks, at most two blank lines, the last line's margin kept. */
std::string FixLines(std::string_view gap) {
    std::vector<std::string> parts;
    size_t p = 0;
    for (;;) {
        const size_t nl = gap.find('\n', p);
        if (nl == std::string_view::npos) {
            parts.emplace_back(gap.substr(p));
            break;
        }
        parts.emplace_back(gap.substr(p, nl - p));
        p = nl + 1;
    }
    std::string out = RStripBlanks(parts[0]);
    int blanks = 0;
    for (size_t k = 1; k + 1 < parts.size(); k++) {
        const std::string line = RStripBlanks(parts[k]);
        if (line.empty()) {
            if (++blanks > 2) continue;
        } else {
            blanks = 0;
        }
        out += '\n';
        out += line;
    }
    out += '\n';
    out += parts.back();
    return out;
}

/** @brief Whether the close parens starting at token `k` are followed on their line by a `;` comment. */
bool ClosersCarryComment(const std::string &s, const std::vector<Tok> &t, size_t k) {
    while (k + 1 < t.size() && t[k + 1].k == TK::Close && s.find('\n', t[k].b) >= t[k + 1].a) k++;
    return k + 1 < t.size() && t[k + 1].k == TK::Comment && s.find('\n', t[k].b) >= t[k + 1].a;
}

/** @brief The whitespace rules, on the gaps between tokens. */
std::string FixGaps(const std::string &s, const std::vector<Tok> &t) {
    std::string out;
    out.reserve(s.size());
    if (t.empty()) return out;
    {
        const std::string_view lead(s.data(), t[0].a);
        out += lead.find('\n') == std::string_view::npos ? std::string(lead) : FixLines(lead);
    }
    for (size_t k = 0; k < t.size(); k++) {
        const std::string text = s.substr(t[k].a, t[k].b - t[k].a);
        out += t[k].k == TK::Comment ? RStripBlanks(text) : IsBangBlock(t[k], text) ? FixLines(text) : text;
        if (k + 1 == t.size()) {
            // The end: blank lines and blanks go, a form feed stays.
            const std::string_view tail(s.data() + t[k].b, s.size() - t[k].b);
            std::string kept = tail.find('\n') == std::string_view::npos ? std::string(tail) : FixLines(tail);
            while (!kept.empty() && (kept.back() == ' ' || kept.back() == '\t' || kept.back() == '\n')) kept.pop_back();
            out += kept;
            break;
        }
        const std::string_view gap(s.data() + t[k].b, t[k + 1].a - t[k].b);
        const bool lines = gap.find('\n') != std::string_view::npos;
        if (t[k + 1].k == TK::Close && t[k].k != TK::Comment && !(lines && ClosersCarryComment(s, t, k + 1))) {
            // `foo )' and a lone `)': the blanks and newlines go (a carriage return stays).
            const size_t keep = gap.find_last_not_of(" \t\n");
            if (keep != std::string_view::npos) {
                const std::string_view head = gap.substr(0, keep + 1);
                out += head.find('\n') == std::string_view::npos ? std::string(head) : FixLines(head);
            }
            continue;
        }
        if (t[k].k == TK::Open && !lines && t[k + 1].k != TK::Comment) {
            // `( foo': the blanks after the paren go.
            const size_t keep = gap.find_first_not_of(" \t");
            if (keep != std::string_view::npos) out += std::string(gap.substr(keep));
            continue;
        }
        out += lines ? FixLines(gap) : std::string(gap);
    }
    out += '\n';
    return out;
}

// ============================================================================ indentation specs

// A `scheme-indent-function' property: `defun', a count of distinguished
// arguments (`lisp-indent-specform'), or `scheme-let-indent'.
struct Method {
    enum Kind { Defun, Spec, Let } kind = Spec;
    int count = 0;
};

using Methods = std::unordered_map<std::string, Method>;

/** @brief scheme.el's `scheme-indent-function' properties, every one (Emacs 30). */
const Methods &BaseMethods() {
    static const Methods m = {
        {"access-components", Method{Method::Spec, 1}},
        {"and-let*", Method{Method::Spec, 1}},
        {"assignment-components", Method{Method::Spec, 1}},
        {"begin", Method{Method::Spec, 0}},
        {"call-with-input-file", Method{Method::Spec, 1}},
        {"call-with-output-file", Method{Method::Spec, 1}},
        {"call-with-port", Method{Method::Spec, 1}},
        {"call-with-values", Method{Method::Spec, 1}},
        {"case", Method{Method::Spec, 1}},
        {"combination-components", Method{Method::Spec, 1}},
        {"comment-components", Method{Method::Spec, 1}},
        {"conditional-components", Method{Method::Spec, 1}},
        {"declaration-components", Method{Method::Spec, 1}},
        {"define-library", Method{Method::Spec, 1}},
        {"define-record-type", Method{Method::Spec, 1}},
        {"define-values", Method{Method::Spec, 1}},
        {"definition-components", Method{Method::Spec, 1}},
        {"delay", Method{Method::Spec, 0}},
        {"delay-components", Method{Method::Spec, 1}},
        {"disjunction-components", Method{Method::Spec, 1}},
        {"do", Method{Method::Spec, 2}},
        {"dynamic-wind", Method{Method::Spec, 3}},
        {"element", Method{Method::Spec, 1}},
        {"eval-when", Method{Method::Spec, 1}},
        {"fluid-let", Method{Method::Spec, 1}},
        {"in-package", Method{Method::Spec, 1}},
        {"in-package-components", Method{Method::Spec, 1}},
        {"lambda", Method{Method::Spec, 1}},
        {"lambda-components", Method{Method::Spec, 1}},
        {"lambda-components*", Method{Method::Spec, 1}},
        {"lambda-components**", Method{Method::Spec, 1}},
        {"let", Method{Method::Let, 0}},
        {"let*", Method{Method::Spec, 1}},
        {"let*-values", Method{Method::Spec, 1}},
        {"let-syntax", Method{Method::Spec, 1}},
        {"let-values", Method{Method::Spec, 1}},
        {"letrec", Method{Method::Spec, 1}},
        {"letrec*", Method{Method::Spec, 1}},
        {"letrec-syntax", Method{Method::Spec, 1}},
        {"library", Method{Method::Spec, 1}},
        {"list-search-negative", Method{Method::Spec, 1}},
        {"list-search-positive", Method{Method::Spec, 1}},
        {"list-transform-negative", Method{Method::Spec, 1}},
        {"list-transform-positive", Method{Method::Spec, 1}},
        {"local-declare", Method{Method::Spec, 1}},
        {"macro", Method{Method::Spec, 1}},
        {"make", Method{Method::Spec, 1}},
        {"make-environment", Method{Method::Spec, 0}},
        {"match", Method{Method::Spec, 1}},
        {"match-lambda", Method{Method::Spec, 0}},
        {"match-lambda*", Method{Method::Spec, 0}},
        {"match-let", Method{Method::Let, 0}},
        {"match-let*", Method{Method::Spec, 1}},
        {"match-letrec", Method{Method::Spec, 1}},
        {"mode", Method{Method::Spec, 1}},
        {"named-lambda", Method{Method::Spec, 1}},
        {"open-block-components", Method{Method::Spec, 1}},
        {"parameterize", Method{Method::Spec, 1}},
        {"pathname-components", Method{Method::Spec, 1}},
        {"procedure-components", Method{Method::Spec, 1}},
        {"receive", Method{Method::Spec, 2}},
        {"root", Method{Method::Spec, 1}},
        {"sequence", Method{Method::Spec, 0}},
        {"sequence-components", Method{Method::Spec, 1}},
        {"style", Method{Method::Spec, 1}},
        {"syntax-case", Method{Method::Spec, 2}},
        {"syntax-rules", Method{Method::Defun, 0}},
        {"syntax-table-define", Method{Method::Spec, 2}},
        {"unassigned?-components", Method{Method::Spec, 1}},
        {"unbound?-components", Method{Method::Spec, 1}},
        {"unless", Method{Method::Spec, 1}},
        {"using-syntax", Method{Method::Spec, 1}},
        {"variable-components", Method{Method::Spec, 1}},
        {"when", Method{Method::Spec, 1}},
        {"with-input-from-file", Method{Method::Spec, 1}},
        {"with-input-from-port", Method{Method::Spec, 1}},
        {"with-input-from-string", Method{Method::Spec, 1}},
        {"with-mode", Method{Method::Spec, 1}},
        {"with-output-to-file", Method{Method::Spec, 1}},
        {"with-output-to-port", Method{Method::Spec, 1}},
        {"with-output-to-string", Method{Method::Spec, 0}},
        {"with-syntax", Method{Method::Spec, 1}},
        {"with-values", Method{Method::Spec, 1}},
        {"\xce""\xbb""", Method{Method::Spec, 1}}
    };
    return m;
}

/** @brief The base table with the caller's specs applied. */
Methods WithOptions(const Options &o) {
    Methods m = BaseMethods();
    for (const auto &[name, spec] : o.indent) {
        if (spec.empty()) {
            m.erase(name);
        } else if (spec == "defun") {
            m[name] = Method{Method::Defun, 0};
        } else if (spec == "let" || spec == "scheme-let-indent") {
            m[name] = Method{Method::Let, 0};
        } else if (std::all_of(spec.begin(), spec.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
                   spec.size() < 6) {
            m[name] = Method{Method::Spec, std::stoi(spec)};
        } else {
            // Another operator's name: indent like it.
            const auto it = BaseMethods().find(spec);
            if (it != BaseMethods().end()) m[name] = it->second;
        }
    }
    return m;
}

// ============================================================================ the Emacs model

// scheme-mode-syntax-table, as far as indentation can tell. The text
// properties scheme-syntax-propertize adds -- `#;' starting a comment that
// ends with its datum, `#/' starting a regexp string -- are applied where
// the scanners meet them.
enum class Syn { Ws, Punct, Word, Symbol, Open, Close, Quote, String, Escape, Comment, EndComment };

Syn Class(unsigned char c) {
    switch (c) {
        case ' ':
        case '\t':
        case '\r':
        case '\f': return Syn::Ws;
        case '\n': return Syn::EndComment;
        case '(':
        case '[':
        case '{': return Syn::Open;
        case ')':
        case ']':
        case '}': return Syn::Close;
        case '"':
        case '|': return Syn::String;
        case '\\': return Syn::Escape;
        case ';': return Syn::Comment;
        case '\'':
        case '`':
        case ',':
        case '@':
        case '#': return Syn::Quote;
        default: break;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80) return Syn::Word;
    return Syn::Symbol;
}

struct UniRange {
    unsigned lo, hi;
    Syn syn;
};

// The characters past ASCII that the standard syntax table (characters.el)
// does not make word constituents, as Emacs 30 has them: punctuation such as
// `…' and the curly quotes, symbol characters such as `→', the
// paired brackets, and the blanks. Generated with `(aref (syntax-table) c)'.
constexpr UniRange kUniSyntax[] = {
    {0xA0, 0xA0, Syn::Ws}, {0xA1, 0xA1, Syn::Punct}, {0xA2, 0xA4, Syn::Symbol}, {0xA6, 0xA6, Syn::Symbol},
    {0xA7, 0xA7, Syn::Punct}, {0xA8, 0xAA, Syn::Symbol}, {0xAB, 0xAB, Syn::Punct}, {0xAC, 0xB1, Syn::Symbol},
    {0xB4, 0xB4, Syn::Symbol}, {0xB6, 0xB8, Syn::Symbol}, {0xBA, 0xBA, Syn::Symbol}, {0xBB, 0xBB, Syn::Punct},
    {0xBC, 0xBE, Syn::Symbol}, {0xBF, 0xBF, Syn::Punct}, {0xD7, 0xD7, Syn::Symbol}, {0xF7, 0xF7, Syn::Symbol},
    {0x2C7, 0x2C7, Syn::Symbol}, {0x2C9, 0x2C9, Syn::Symbol}, {0x2D0, 0x2D0, Syn::Symbol},
    {0x2D8, 0x2DB, Syn::Symbol}, {0x2DD, 0x2DD, Syn::Symbol}, {0x384, 0x385, Syn::Symbol},
    {0x5BE, 0x5BE, Syn::Punct}, {0x5C0, 0x5C0, Syn::Punct}, {0x5C3, 0x5C3, Syn::Punct}, {0x5C6, 0x5C6, Syn::Punct},
    {0x5F3, 0x5F4, Syn::Punct}, {0xE2F, 0xE2F, Syn::Symbol}, {0xE3F, 0xE3F, Syn::Symbol},
    {0xE46, 0xE46, Syn::Symbol}, {0xE4F, 0xE4F, Syn::Symbol}, {0xE5A, 0xE5B, Syn::Symbol},
    {0xEAF, 0xEAF, Syn::Symbol}, {0xEC6, 0xEC6, Syn::Symbol}, {0xF00, 0xF0B, Syn::Punct},
    {0xF0D, 0xF18, Syn::Punct}, {0xF1A, 0xF1F, Syn::Punct}, {0xF34, 0xF34, Syn::Punct}, {0xF36, 0xF36, Syn::Punct},
    {0xF38, 0xF3F, Syn::Punct}, {0xF7F, 0xF7F, Syn::Punct}, {0xF85, 0xF85, Syn::Punct}, {0xFBE, 0xFCF, Syn::Punct},
    {0x1361, 0x1368, Syn::Punct}, {0x1800, 0x180A, Syn::Punct}, {0x2000, 0x200B, Syn::Ws},
    {0x200C, 0x2027, Syn::Punct}, {0x202F, 0x202F, Syn::Ws}, {0x2030, 0x2038, Syn::Punct},
    {0x2039, 0x203A, Syn::Symbol}, {0x203B, 0x2043, Syn::Punct}, {0x2044, 0x2044, Syn::Symbol},
    {0x2045, 0x2045, Syn::Open}, {0x2046, 0x2046, Syn::Close}, {0x2047, 0x2051, Syn::Punct},
    {0x2052, 0x2052, Syn::Symbol}, {0x2053, 0x205E, Syn::Punct}, {0x205F, 0x205F, Syn::Ws},
    {0x207D, 0x207D, Syn::Open}, {0x207E, 0x207E, Syn::Close}, {0x208D, 0x208D, Syn::Open},
    {0x208E, 0x208E, Syn::Close}, {0x20AC, 0x20AC, Syn::Symbol}, {0x2103, 0x2103, Syn::Symbol},
    {0x2109, 0x2109, Syn::Symbol}, {0x2116, 0x2116, Syn::Punct}, {0x2121, 0x2122, Syn::Symbol},
    {0x2153, 0x2154, Syn::Symbol}, {0x215B, 0x215E, Syn::Symbol}, {0x2190, 0x2328, Syn::Symbol},
    {0x2329, 0x2329, Syn::Open}, {0x232A, 0x232A, Syn::Close}, {0x232B, 0x23B3, Syn::Symbol},
    {0x23B4, 0x23B4, Syn::Open}, {0x23B5, 0x23B5, Syn::Close}, {0x23B6, 0x244F, Syn::Symbol},
    {0x2460, 0x246E, Syn::Symbol}, {0x2474, 0x24B5, Syn::Symbol}, {0x2500, 0x254B, Syn::Symbol},
    {0x2592, 0x2592, Syn::Symbol}, {0x25A0, 0x25A1, Syn::Symbol}, {0x25A3, 0x25A9, Syn::Symbol},
    {0x25B2, 0x25B3, Syn::Symbol}, {0x25B6, 0x25B7, Syn::Symbol}, {0x25BC, 0x25BD, Syn::Symbol},
    {0x25C0, 0x25C1, Syn::Symbol}, {0x25C6, 0x25C8, Syn::Symbol}, {0x25CB, 0x25CB, Syn::Symbol},
    {0x25CE, 0x25D1, Syn::Symbol}, {0x25EF, 0x25EF, Syn::Symbol}, {0x2605, 0x2606, Syn::Symbol},
    {0x260E, 0x260F, Syn::Symbol}, {0x261C, 0x261C, Syn::Symbol}, {0x261E, 0x261E, Syn::Symbol},
    {0x2640, 0x2640, Syn::Symbol}, {0x2642, 0x2642, Syn::Symbol}, {0x2660, 0x2661, Syn::Symbol},
    {0x2663, 0x2665, Syn::Symbol}, {0x2667, 0x266A, Syn::Symbol}, {0x266C, 0x266D, Syn::Symbol},
    {0x266F, 0x266F, Syn::Symbol}, {0x2768, 0x2768, Syn::Open}, {0x2769, 0x2769, Syn::Close},
    {0x276A, 0x276A, Syn::Open}, {0x276B, 0x276B, Syn::Close}, {0x276C, 0x276C, Syn::Open},
    {0x276D, 0x276D, Syn::Close}, {0x2770, 0x2770, Syn::Open}, {0x2771, 0x2771, Syn::Close},
    {0x2772, 0x2772, Syn::Open}, {0x2773, 0x2773, Syn::Close}, {0x2774, 0x2774, Syn::Open},
    {0x2775, 0x2775, Syn::Close}, {0x27E6, 0x27E6, Syn::Open}, {0x27E7, 0x27E7, Syn::Close},
    {0x27E8, 0x27E8, Syn::Open}, {0x27E9, 0x27E9, Syn::Close}, {0x27EA, 0x27EA, Syn::Open},
    {0x27EB, 0x27EB, Syn::Close}, {0x2983, 0x2983, Syn::Open}, {0x2984, 0x2984, Syn::Close},
    {0x2985, 0x2985, Syn::Open}, {0x2986, 0x2986, Syn::Close}, {0x2987, 0x2987, Syn::Open},
    {0x2988, 0x2988, Syn::Close}, {0x2989, 0x2989, Syn::Open}, {0x298A, 0x298A, Syn::Close},
    {0x298B, 0x298B, Syn::Open}, {0x298C, 0x298C, Syn::Close}, {0x298D, 0x298D, Syn::Open},
    {0x298E, 0x298E, Syn::Close}, {0x298F, 0x298F, Syn::Open}, {0x2990, 0x2990, Syn::Close},
    {0x2991, 0x2991, Syn::Open}, {0x2992, 0x2992, Syn::Close}, {0x2993, 0x2993, Syn::Open},
    {0x2994, 0x2994, Syn::Close}, {0x2995, 0x2995, Syn::Open}, {0x2996, 0x2996, Syn::Close},
    {0x2997, 0x2997, Syn::Open}, {0x2998, 0x2998, Syn::Close}, {0x29FC, 0x29FC, Syn::Open},
    {0x29FD, 0x29FD, Syn::Close}, {0x2A00, 0x2BFF, Syn::Symbol}, {0x2E00, 0x2E7F, Syn::Punct},
    {0x3000, 0x3000, Syn::Ws}, {0x3001, 0x3003, Syn::Punct}, {0x3008, 0x3008, Syn::Open},
    {0x3009, 0x3009, Syn::Close}, {0x300A, 0x300A, Syn::Open}, {0x300B, 0x300B, Syn::Close},
    {0x300C, 0x300C, Syn::Open}, {0x300D, 0x300D, Syn::Close}, {0x300E, 0x300E, Syn::Open},
    {0x300F, 0x300F, Syn::Close}, {0x3010, 0x3010, Syn::Open}, {0x3011, 0x3011, Syn::Close},
    {0x3012, 0x3013, Syn::Symbol}, {0x3014, 0x3014, Syn::Open}, {0x3015, 0x3015, Syn::Close},
    {0x3016, 0x3016, Syn::Open}, {0x3017, 0x3017, Syn::Close}, {0x3018, 0x3018, Syn::Open},
    {0x3019, 0x3019, Syn::Close}, {0x301A, 0x301A, Syn::Open}, {0x301B, 0x301B, Syn::Close},
    {0x301C, 0x301C, Syn::Symbol}, {0x30FB, 0x30FB, Syn::Punct}, {0x3200, 0x321C, Syn::Symbol},
    {0x3220, 0x3229, Syn::Symbol}, {0x3260, 0x327B, Syn::Symbol}, {0x327E, 0x327F, Syn::Symbol},
    {0x3380, 0x3384, Syn::Symbol}, {0x3388, 0x33CA, Syn::Symbol}, {0x33CF, 0x33D0, Syn::Symbol},
    {0x33D3, 0x33D3, Syn::Symbol}, {0x33D6, 0x33D6, Syn::Symbol}, {0x33D8, 0x33D8, Syn::Symbol},
    {0x33DB, 0x33DD, Syn::Symbol}, {0xAADB, 0xAADF, Syn::Symbol}, {0xFD3E, 0xFD3E, Syn::Open},
    {0xFD3F, 0xFD3F, Syn::Close}, {0xFE35, 0xFE35, Syn::Open}, {0xFE36, 0xFE36, Syn::Close},
    {0xFE37, 0xFE37, Syn::Open}, {0xFE38, 0xFE38, Syn::Close}, {0xFE39, 0xFE39, Syn::Open},
    {0xFE3A, 0xFE3A, Syn::Close}, {0xFE3B, 0xFE3B, Syn::Open}, {0xFE3C, 0xFE3C, Syn::Close},
    {0xFE3D, 0xFE3D, Syn::Open}, {0xFE3E, 0xFE3E, Syn::Close}, {0xFE3F, 0xFE3F, Syn::Open},
    {0xFE40, 0xFE40, Syn::Close}, {0xFE41, 0xFE41, Syn::Open}, {0xFE42, 0xFE42, Syn::Close},
    {0xFE43, 0xFE43, Syn::Open}, {0xFE44, 0xFE44, Syn::Close}, {0xFE59, 0xFE59, Syn::Open},
    {0xFE5A, 0xFE5A, Syn::Close}, {0xFE5B, 0xFE5B, Syn::Open}, {0xFE5C, 0xFE5C, Syn::Close},
    {0xFE5D, 0xFE5D, Syn::Open}, {0xFE5E, 0xFE5E, Syn::Close}, {0xFF01, 0xFF03, Syn::Punct},
    {0xFF04, 0xFF04, Syn::Symbol}, {0xFF05, 0xFF07, Syn::Punct}, {0xFF08, 0xFF08, Syn::Open},
    {0xFF09, 0xFF09, Syn::Close}, {0xFF0A, 0xFF0A, Syn::Punct}, {0xFF0B, 0xFF0B, Syn::Symbol},
    {0xFF0C, 0xFF0F, Syn::Punct}, {0xFF1A, 0xFF1B, Syn::Punct}, {0xFF1C, 0xFF1E, Syn::Symbol},
    {0xFF1F, 0xFF20, Syn::Punct}, {0xFF3B, 0xFF3B, Syn::Open}, {0xFF3C, 0xFF3C, Syn::Symbol},
    {0xFF3D, 0xFF3D, Syn::Close}, {0xFF3E, 0xFF40, Syn::Symbol}, {0xFF5B, 0xFF5B, Syn::Open},
    {0xFF5C, 0xFF5C, Syn::Symbol}, {0xFF5D, 0xFF5D, Syn::Close}, {0xFF5E, 0xFF5E, Syn::Symbol},
    {0xFF5F, 0xFF5F, Syn::Open}, {0xFF60, 0xFF60, Syn::Close}, {0xFF61, 0xFF61, Syn::Punct},
    {0xFF62, 0xFF62, Syn::Open}, {0xFF63, 0xFF63, Syn::Close}, {0xFF64, 0xFF65, Syn::Punct},
    {0xFFE0, 0xFFE3, Syn::Symbol}, {0xFFE5, 0xFFE5, Syn::Symbol}, {0x11660, 0x1166C, Syn::Punct},
    {0x1FB00, 0x1FBCA, Syn::Symbol}, {0x1FBCB, 0x1FBFF, Syn::Punct},
};

Syn UniClass(unsigned cp) {
    const UniRange *b = std::begin(kUniSyntax), *e = std::end(kUniSyntax);
    const UniRange *it = std::upper_bound(b, e, cp, [](unsigned c, const UniRange &r) { return c < r.lo; });
    if (it == b) return Syn::Word;
    --it;
    return cp <= it->hi ? it->syn : Syn::Word;
}

// The East Asian wide ranges Emacs's `char-width' gives two columns.
bool IsWide(unsigned cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
           (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
           (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

// An indentation answer: nothing, a column (which indent-region caches
// for the lines that follow at the same depth), or `(COLUMN START)' (which
// it does not).
struct Ind {
    enum Kind { None, Int, List } kind = None;
    int col = 0;
};

Ind IntInd(int c) { return Ind{Ind::Int, c}; }
Ind ListInd(int c) { return Ind{Ind::List, c}; }

// `parse-partial-sexp's state: the parts indentation reads.
struct PState {
    int depth = 0;
    long inner = -1;       // nth 1: the innermost open paren
    long last = -1;        // nth 2: the start of the last complete sexp at that level
    char instr = 0;        // nth 3: the terminator of the string being read
    int comment = 0;       // nth 4: -1 in a `;' comment, the nesting depth in a `#|' or `#;' one
    long comstr = -1;      // nth 8: where that string or comment started
    long datum_end = -1;   // in a `#;' comment: where it ends (LONG_MAX: never)
    std::vector<long> opens;  // nth 9: every open paren, outermost first
};

struct Stop {
    PState st;
    long pos = 0;
};

constexpr int kNoTarget = INT_MIN;
constexpr int kBodyIndent = 2;  // lisp-body-indent

/**
 * The buffer: the lines already indented (`done`), then the rest of the
 * text as it still is. Indentation only ever looks back at finished lines
 * and forward at unfinished ones, whose leading blanks do not matter to it,
 * exactly as in Emacs, where indent-region changes the buffer as it goes.
 */
class Engine {
  public:
    Engine(const std::string &text, const Methods &m) : rest_(text), methods_(m) {
        line_starts_.push_back(0);
        for (size_t k = 0; k < text.size(); k++) {
            if (text[k] == '\n') line_starts_.push_back(static_cast<long>(k + 1));
        }
    }

    /** @brief `indent-region' over the whole text: the result. */
    std::string IndentRegion();

    /** @brief `lisp-indent-line's column for one line, the text as it stands; -1 inside a string or comment. */
    int IndentOne(int line);

  private:
    std::string done_;
    const std::string &rest_;
    size_t roff_ = 0;
    const Methods &methods_;
    std::vector<long> line_starts_;  // of the original text

    long Size() const { return static_cast<long>(done_.size() + rest_.size() - roff_); }
    unsigned char At(long p) const {
        if (p < 0) return 0;
        const long d = static_cast<long>(done_.size());
        if (p < d) return static_cast<unsigned char>(done_[static_cast<size_t>(p)]);
        const size_t r = static_cast<size_t>(p - d) + roff_;
        return r < rest_.size() ? static_cast<unsigned char>(rest_[r]) : 0;
    }

    long Bol(long p) const {
        while (p > 0 && At(p - 1) != '\n') p--;
        return p;
    }
    long Eol(long p) const {
        const long n = Size();
        while (p < n && At(p) != '\n') p++;
        return p;
    }
    /** @brief `(save-excursion (forward-line 1) (point))'. */
    long NextLine(long p) const {
        const long e = Eol(p);
        return e < Size() ? e + 1 : e;
    }

    /** @brief `current-column': tabs to multiples of 8, control characters as ^X, wide characters as two. */
    int Column(long p) const {
        int col = 0;
        for (long q = Bol(p); q < p;) {
            const unsigned char c = At(q);
            if (c == '\t') {
                col = (col / 8 + 1) * 8;
                q++;
            } else if (c < 0x20 || c == 0x7f) {
                col += 2;
                q++;
            } else if (c < 0x80) {
                col++;
                q++;
            } else {
                const size_t len = Utf8Len(c);
                unsigned cp = len == 2 ? c & 0x1Fu : len == 3 ? c & 0x0Fu : len == 4 ? c & 0x07u : c;
                bool valid = len > 1;
                for (size_t k = 1; k < len; k++) {
                    const unsigned char cc = At(q + static_cast<long>(k));
                    if ((cc & 0xC0) != 0x80) valid = false;
                    cp = (cp << 6) | (cc & 0x3Fu);
                }
                if (!valid) {
                    col += 4;  // a raw byte shows as \351
                    q++;
                    continue;
                }
                col += IsWide(cp) ? 2 : 1;
                q += static_cast<long>(len);
            }
        }
        return col;
    }

    /** @brief The syntax class of the character starting at `p' (a UTF-8 sequence read whole). */
    Syn SynAt(long p) const {
        const unsigned char c = At(p);
        if (c < 0xC0) return Class(c);
        const size_t len = Utf8Len(c);
        unsigned cp = len == 2 ? c & 0x1Fu : len == 3 ? c & 0x0Fu : c & 0x07u;
        for (size_t k = 1; k < len; k++) {
            const unsigned char cc = At(p + static_cast<long>(k));
            if ((cc & 0xC0) != 0x80) return Syn::Word;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        return UniClass(cp);
    }
    /** @brief The start of the character after the one at `p'. */
    long Next(long p) const {
        const unsigned char c = At(p);
        if (c < 0xC0) return p + 1;
        const long len = static_cast<long>(Utf8Len(c));
        for (long k = 1; k < len; k++) {
            if ((At(p + k) & 0xC0) != 0x80) return p + k;
        }
        return p + len;
    }

    long BackwardPrefixChars(long p) const {
        while (p > 0 && Class(At(p - 1)) == Syn::Quote) p--;
        return p;
    }

    long SkipBlanks(long p) const {
        while (p < Size() && (At(p) == ' ' || At(p) == '\t')) p++;
        return p;
    }

    Stop Parse(long from, long to, int target, bool stopbefore, const PState *old, bool stop_string_end = false) const;
    std::optional<long> ForwardSexp(long from) const;
    /** @brief Where a `#;' comment whose datum starts at `p' ends: scheme-syntax-propertize-sexp-comment. */
    long DatumEnd(long p) const {
        const std::optional<long> e = ForwardSexp(p);
        return e ? *e : LONG_MAX;
    }
    /**
     * @brief Where a Guile `#!...!#' comment starting at `p' ends; -1 for a directive (`#!r6rs') or no `!#';
     * after `#!eof' in the first column (the top level, where Guile stops reading), never.
     * Emacs's scheme-mode does not know these (scheme.el says so) and reads the text as code; here it is a
     * comment, as Guile reads it.
     */
    long BangEnd(long p) const {
        const long n = Size();
        long w = p + 2;
        std::string word;
        while (w < n && !IsDelimiter(static_cast<char>(At(w)))) word += static_cast<char>(At(w++));
        if (word == "eof") return p == Bol(p) ? LONG_MAX : -1;  // the rest of the file, at the top level
        if (IsDirectiveWord(word)) return -1;
        for (long q = p + 2; q + 1 < n; q++) {
            if (At(q) == '!' && At(q + 1) == '#') return q + 2;
        }
        return -1;
    }
    /**
     * @brief Where a Guile `#{...}#' symbol starting at `p' ends; -1 when its first brace is not `}#' (Chez's
     * `#{name unique}'). Emacs reads one as a braced list -- and re-indents the lines of a multi-line one,
     * changing the symbol; here it is one token, its lines kept as they are, as a string's are.
     */
    long BraceSymbolEnd(long p) const {
        const long n = Size();
        for (long q = p + 2; q < n; q++) {
            if (At(q) == '}') return At(q + 1) == '#' ? q + 2 : -1;
        }
        return -1;
    }
    long SexpStart(long from, long limit) const { return Parse(from, limit, 0, true, nullptr).pos; }

    Ind Calculate(long ip, PState st) const;
    Ind SchemeIndent(long ip, const PState &st, long last_sexp, int normal) const;
    Ind SpecForm(int count, const PState &st, long ip, int normal) const;
    Ind DefForm(const PState &st) const;

    void CommitLine(Ind indent, const PState &at_start);
};

/** @brief `parse-partial-sexp' (scan_sexps_forward) from `from' to `to', optionally resuming `old'. */
Stop Engine::Parse(long from, long to, int target, bool stopbefore, const PState *old, bool stop_string_end) const {
    Stop r;
    PState &s = r.st;
    if (old) s = *old;
    std::vector<std::pair<long, long>> lv;  // per level: (prev, last)
    lv.emplace_back(-1, -1);
    for (long o : s.opens) {
        lv.back().second = o;
        lv.emplace_back(-1, -1);
    }
    if (old) {
        // Resuming at a line start: the innermost level's last complete sexp, or the string being read.
        lv.back().first = s.last;
        lv.back().second = s.instr ? s.comstr : s.last;
    }
    long p = from;
    auto finish = [&]() {
        s.inner = s.opens.empty() ? -1 : s.opens.back();
        s.last = lv.back().first;
        r.pos = p;
        return r;
    };
    auto stop_here = [&]() {
        r.pos = p;
        s.inner = s.opens.empty() ? -1 : s.opens.back();
        s.last = lv.back().first;
        return r;
    };
    for (;;) {
        if (s.instr) {
            const bool sym = s.instr == '}';  // a `#{...}#' symbol
            while (p < to) {
                const unsigned char c = At(p);
                if (sym ? c == '}' && At(p + 1) == '#' : c == static_cast<unsigned char>(s.instr)) break;
                p += c == '\\' && !sym ? 2 : 1;
            }
            if (p >= to) {
                p = std::min(p, to);
                return finish();
            }
            p += sym ? 2 : 1;
            s.instr = 0;
            lv.back().first = lv.back().second;
            if (stop_string_end) return finish();
            continue;
        }
        if (s.comment > 0 && s.datum_end >= 0) {
            if (s.datum_end > to) {
                p = std::max(p, to);
                return finish();
            }
            p = std::max(p, s.datum_end);
            s.comment = 0;
            s.datum_end = -1;
            continue;
        }
        if (s.comment > 0) {
            while (p < to) {
                if (At(p) == '|' && p + 1 < to && At(p + 1) == '#') {
                    p += 2;
                    if (--s.comment == 0) break;
                } else if (At(p) == '#' && p + 1 < to && At(p + 1) == '|') {
                    p += 2;
                    s.comment++;
                } else {
                    p++;
                }
            }
            if (s.comment > 0) return finish();
            continue;
        }
        if (s.comment < 0) {
            while (p < to && At(p) != '\n') p++;
            if (p >= to) return finish();
            p++;
            s.comment = 0;
            continue;
        }
        if (p >= to) return finish();
        const unsigned char c = At(p);
        if (c == '#' && p + 1 < to) {
            const unsigned char d = At(p + 1);
            if (d == '|') {
                s.comment = 1;
                s.comstr = p;
                p += 2;
                continue;
            }
            const long bang = d == '!' ? BangEnd(p) : -1;
            if (d == ';' || bang >= 0) {
                s.comment = 1;
                s.comstr = p;
                s.datum_end = d == ';' ? DatumEnd(p + 2) : bang;
                p += 2;
                continue;
            }
            if (d == '/' || (d == '{' && BraceSymbolEnd(p) >= 0)) {
                if (stopbefore) return stop_here();
                s.comstr = p;
                lv.back().second = p;
                s.instr = d == '/' ? '/' : '}';
                p += 2;
                continue;
            }
        }
        const Syn cls = SynAt(p);
        switch (cls) {
            case Syn::Ws:
            case Syn::Punct:
            case Syn::EndComment:
            case Syn::Quote: p = Next(p); break;
            case Syn::Comment:
                s.comment = -1;
                s.comstr = p;
                p++;
                break;
            case Syn::Escape:
            case Syn::Word:
            case Syn::Symbol: {
                if (stopbefore) return stop_here();
                lv.back().second = p;
                p = cls == Syn::Escape ? Next(p + 1) : Next(p);
                while (p < to) {
                    const Syn k = SynAt(p);
                    if (k == Syn::Escape) {
                        p = Next(p + 1);
                    } else if (k == Syn::Word || k == Syn::Symbol || k == Syn::Quote) {
                        p = Next(p);
                    } else {
                        break;
                    }
                }
                p = std::min(p, to);
                lv.back().first = lv.back().second;
                break;
            }
            case Syn::Open:
                if (stopbefore) return stop_here();
                s.depth++;
                lv.back().second = p;
                s.opens.push_back(p);
                lv.emplace_back(-1, -1);
                p++;
                if (target == s.depth) return finish();
                break;
            case Syn::Close:
                s.depth--;
                if (lv.size() > 1) {
                    lv.pop_back();
                    s.opens.pop_back();
                }
                lv.back().first = lv.back().second;
                p++;
                if (target == s.depth) return finish();
                break;
            case Syn::String:
                if (stopbefore) return stop_here();
                s.comstr = p;
                lv.back().second = p;
                s.instr = static_cast<char>(c);
                p++;
                break;
        }
    }
}

/** @brief `forward-sexp' (scan_lists): where the next sexp ends; nullopt where Emacs signals a scan error. */
std::optional<long> Engine::ForwardSexp(long p) const {
    const long n = Size();
    int depth = 0;
    auto string_end = [&](long q, unsigned char close) -> std::optional<long> {
        for (;;) {
            if (q >= n) return std::nullopt;
            const unsigned char d = At(q);
            if (d == close) return q + 1;
            q += d == '\\' ? 2 : 1;
        }
    };
    while (p < n) {
        const unsigned char c = At(p);
        if (c == '#' && p + 1 < n && At(p + 1) == '|') {
            int nest = 1;
            p += 2;
            while (p < n && nest > 0) {
                if (At(p) == '|' && p + 1 < n && At(p + 1) == '#') {
                    nest--;
                    p += 2;
                } else if (At(p) == '#' && p + 1 < n && At(p + 1) == '|') {
                    nest++;
                    p += 2;
                } else {
                    p++;
                }
            }
            if (nest > 0) return depth == 0 ? std::optional<long>(n) : std::nullopt;
            continue;
        }
        if (c == '#' && p + 1 < n && (At(p + 1) == ';' || (At(p + 1) == '!' && BangEnd(p) >= 0))) {
            const long e = At(p + 1) == ';' ? DatumEnd(p + 2) : BangEnd(p);
            if (e >= n) return depth == 0 ? std::optional<long>(n) : std::nullopt;
            p = e;
            continue;
        }
        if (c == '#' && p + 1 < n && At(p + 1) == '/') {
            const std::optional<long> e = string_end(p + 2, '/');
            if (!e) return std::nullopt;
            p = *e;
            if (depth == 0) return p;
            continue;
        }
        if (c == '#' && p + 1 < n && At(p + 1) == '{' && BraceSymbolEnd(p) >= 0) {
            p = BraceSymbolEnd(p);
            if (depth == 0) return p;
            continue;
        }
        const Syn cls = SynAt(p);
        p = Next(p);
        switch (cls) {
            case Syn::Escape:
                if (p >= n) return std::nullopt;
                p = Next(p);
                [[fallthrough]];
            case Syn::Word:
            case Syn::Symbol:
                if (depth) break;
                while (p < n) {
                    const Syn k = SynAt(p);
                    if (k == Syn::Escape) {
                        p++;
                        if (p >= n) return std::nullopt;
                        p = Next(p);
                    } else if (k == Syn::Word || k == Syn::Symbol || k == Syn::Quote) {
                        p = Next(p);
                    } else {
                        break;
                    }
                }
                return p;
            case Syn::Comment:
                while (p < n && At(p) != '\n') p++;
                if (p >= n) return depth == 0 ? std::optional<long>(n) : std::nullopt;
                p++;
                break;
            case Syn::Open: depth++; break;
            case Syn::Close:
                depth--;
                if (depth == 0) return p;
                if (depth < 0) return std::nullopt;
                break;
            case Syn::String: {
                const std::optional<long> e = string_end(p, c);
                if (!e) return std::nullopt;
                p = *e;
                if (depth == 0) return p;
                break;
            }
            default: break;
        }
    }
    return depth == 0 ? std::optional<long>(n) : std::nullopt;
}

/** @brief `calculate-lisp-indent' for the line starting at `ip', given the parse state there. */
Ind Engine::Calculate(long ip, PState st) const {
    long last_sexp = -1, containing = -1;
    bool retry = true;
    while (retry && st.depth > 0) {
        retry = false;
        last_sexp = st.last;
        containing = st.inner;
        if (containing < 0) break;
        if (last_sexp >= 0 && last_sexp > containing + 1) {
            // Is there a containing sexp after the last complete one?
            Stop peek = Parse(last_sexp, ip, 0, false, nullptr);
            if (peek.st.inner >= 0) {
                retry = true;
                PState merged = peek.st;
                merged.opens.insert(merged.opens.begin(), st.opens.begin(), st.opens.end());
                st = merged;
            }
        }
    }
    if (st.instr) return Ind{};
    if (retry || containing < 0) return IntInd(0);
    const long after_open = containing + 1;
    const bool ws_after_open = SynAt(after_open) == Syn::Ws;
    if (last_sexp < 0) return IntInd(Column(after_open));  // right after the open paren
    long pt = SexpStart(after_open, last_sexp);
    if (SynAt(pt) == Syn::Open) {
        // The first element is a list: indent under it.
    } else if (NextLine(pt) > last_sexp) {
        // The first line inside the containing sexp: almost certainly a call.
        if (!(pt == last_sexp || ws_after_open)) {
            const std::optional<long> e = ForwardSexp(pt);
            if (e) pt = SexpStart(*e, last_sexp);
        }
        pt = BackwardPrefixChars(pt);
    } else {
        // Under the first sexp on the last sexp's line.
        pt = BackwardPrefixChars(SexpStart(Bol(last_sexp), last_sexp));
    }
    const int normal = Column(pt);
    const Ind got = SchemeIndent(ip, st, last_sexp, normal);
    if (got.kind != Ind::None) return got;
    // A keyword under the keyword above it.
    if (At(SkipBlanks(ip)) == ':') {
        // The starts (prefixes included) of the elements up to the last sexp,
        // which is what backward-sexp steps between.
        std::vector<long> elts;
        for (long q = after_open;;) {
            const long s = SexpStart(q, last_sexp + 1);
            if (s > last_sexp || s >= Size()) break;
            elts.push_back(BackwardPrefixChars(s));
            const std::optional<long> e = ForwardSexp(s);
            if (!e || *e <= s) break;
            q = *e;
        }
        long pt2 = BackwardPrefixChars(last_sexp);
        int i = static_cast<int>(elts.size()) - 1;
        while (i > 0 && elts[static_cast<size_t>(i)] > pt2) i--;
        bool ok = i >= 0;
        while (ok) {
            long q = pt2;
            while (q > 0 && (At(q - 1) == ' ' || At(q - 1) == '\t')) q--;
            if (q == Bol(q) || q == after_open) break;
            if (i <= 0) {
                ok = false;
                break;
            }
            i--;
            pt2 = elts[static_cast<size_t>(i)];
        }
        if (ok && pt2 > SexpStart(after_open, pt2)) {
            if (At(pt2) == ':') return IntInd(Column(pt2));
            if (i > 0) {
                const long prev = elts[static_cast<size_t>(i - 1)];
                if (Bol(pt2) < prev && At(prev) == ':') return IntInd(Column(prev));
            }
        }
    }
    return IntInd(normal);
}

/** @brief `scheme-indent-function': the head's `scheme-indent-function' property, or a `def...' head. */
Ind Engine::SchemeIndent(long ip, const PState &st, long last_sexp, int normal) const {
    long pt = SexpStart(st.inner + 1, last_sexp);
    const Syn k = SynAt(pt);
    if (!(k == Syn::Word || k == Syn::Symbol) || (At(pt) == '#' && At(pt + 1) == '/')) {
        // The car is not a symbol: under the list, or under the first sexp on the last sexp's line.
        if (NextLine(pt) <= last_sexp) pt = SexpStart(Bol(last_sexp), last_sexp);
        return IntInd(Column(BackwardPrefixChars(pt)));
    }
    const std::optional<long> end = ForwardSexp(pt);
    if (!end) return Ind{};
    std::string name;
    for (long q = pt; q < *end; q++) name += static_cast<char>(At(q));
    const auto it = methods_.find(name);
    if (it == methods_.end()) {
        // `(string-match "\\`def" function)', case-fold-search on; more than three characters.
        size_t chars = 0;
        for (char c : name) chars += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
        const bool def = chars > 3 && (name[0] | 0x20) == 'd' && (name[1] | 0x20) == 'e' && (name[2] | 0x20) == 'f';
        return def ? DefForm(st) : Ind{};
    }
    switch (it->second.kind) {
        case Method::Defun: return DefForm(st);
        case Method::Spec: return SpecForm(it->second.count, st, ip, normal);
        case Method::Let: {
            // `scheme-let-indent': a named let has one more distinguished form.
            const unsigned char c = At(SkipBlanks(*end));
            const bool named = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                               (c != 0 && std::strchr("-+*/?!@$%^&_:~", c) != nullptr);
            return SpecForm(named ? 2 : 1, st, ip, normal);
        }
    }
    return Ind{};
}

/** @brief `lisp-indent-specform': `count' distinguished forms at twice the body indent, then a body. */
Ind Engine::SpecForm(int count, const PState &st, long ip, int normal) const {
    const long start = st.inner;
    const int start_col = Column(start);
    const int body = start_col + kBodyIndent;
    const int i = count;
    const std::optional<long> head = ForwardSexp(start + 1);
    if (!head) return Ind{};
    long p = Parse(*head, ip, 1, true, nullptr).pos;
    while (p < ip) {
        count--;
        const std::optional<long> e = ForwardSexp(p);
        if (!e) break;
        p = Parse(*e, ip, 1, true, nullptr).pos;
    }
    if (count > 0) return ListInd(i - count <= 1 ? start_col + 2 * kBodyIndent : normal);
    if ((i == 0 && count == 0) || (count == 0 && body <= normal)) return IntInd(body);
    return IntInd(normal);
}

/** @brief `lisp-indent-defform': a body indent, once the line is past the head's line. */
Ind Engine::DefForm(const PState &st) const {
    if (NextLine(st.inner) > st.last) return IntInd(Column(st.inner) + kBodyIndent);
    return Ind{};
}

/** @brief The first line of the rest (up to and including its newline), with its new margin. */
void Engine::CommitLine(Ind indent, const PState &at_start) {
    size_t e = rest_.find('\n', roff_);
    e = e == std::string::npos ? rest_.size() : e + 1;
    const std::string line = rest_.substr(roff_, e - roff_);
    roff_ = e;
    size_t f = 0;
    while (f < line.size() && (line[f] == ' ' || line[f] == '\t')) f++;
    const std::string body = line.substr(f);
    if (at_start.instr || at_start.comment > 0) {
        done_ += line;  // in a string or a `#|' or `#;' comment: as it is
        return;
    }
    if (body.empty() || body[0] == '\n') {
        done_ += body;  // blank: empty
        return;
    }
    int margin = 0;
    for (size_t k = 0; k < f; k++) margin = line[k] == '\t' ? (margin / 8 + 1) * 8 : margin + 1;
    // Kept where they are: `;;;' and single-`;' comment lines.
    bool keep = indent.kind == Ind::None;
    if (body[0] == ';') keep = keep || body.compare(0, 3, ";;;") == 0 || body.size() < 2 || body[1] != ';';
    const size_t at = done_.size();
    done_ += std::string(static_cast<size_t>(std::max(0, keep ? margin : indent.col)), ' ');
    done_ += body;
    // And the first line of a `#|', `#;' or `#!' comment that goes on past it.
    if (!keep && body.back() == '\n' &&
        (body.compare(0, 2, "#|") == 0 || body.compare(0, 2, "#;") == 0 || body.compare(0, 2, "#!") == 0)) {
        const long from = static_cast<long>(done_.size() - body.size());
        const PState ps = Parse(from, static_cast<long>(done_.size()), kNoTarget, false, nullptr).st;
        if (ps.comment > 0) {
            done_.resize(at);
            done_ += std::string(static_cast<size_t>(margin), ' ');
            done_ += body;
        }
    }
}

/**
 * @brief `indent-region' as scheme-mode has it: no `indent-region-function', so
 * `indent-region-line-by-line' -- `lisp-indent-line' on every non-empty line, each
 * from a full `lisp-ppss' of the text above it as indented so far, no column cache.
 */
std::string Engine::IndentRegion() {
    PState st;  // at the start of the line about to be committed
    long line_start = 0;
    while (roff_ < rest_.size()) {
        CommitLine(st.instr ? Ind{} : Calculate(line_start, st), st);
        const long next = static_cast<long>(done_.size());
        st = Parse(line_start, next, kNoTarget, false, &st).st;
        line_start = next;
    }
    return done_;
}

/** @brief `lisp-indent-line's column for one line (a fresh parse, no cache). */
int Engine::IndentOne(int line) {
    if (line <= 0 || line_starts_.empty()) return 0;
    if (static_cast<size_t>(line) >= line_starts_.size()) line = static_cast<int>(line_starts_.size()) - 1;
    const long ip = line_starts_[static_cast<size_t>(line)];
    const PState st = Parse(0, ip, kNoTarget, false, nullptr).st;
    if (st.instr || st.comment > 0) return -1;
    const Ind got = Calculate(ip, st);
    return got.kind == Ind::None ? -1 : got.col;
}

}  // namespace

Result Format(std::string_view input, const Options &options) {
    Result res;
    res.text = std::string(input);
    const std::string &src = res.text;
    Lexed in = Lex(src);
    if (!in.ok) {
        res.error = in.error;
        res.error_line = in.error_line;
        return res;
    }
    MergeDatumComments(src, in.toks);
    const std::string gaps = FixGaps(src, in.toks);
    const Methods m = options.indent.empty() ? BaseMethods() : WithOptions(options);
    std::string out = Engine(gaps, m).IndentRegion();
    Lexed again = Lex(out);
    if (again.ok) MergeDatumComments(out, again.toks);
    if (!again.ok || Signature(out, again.toks) != Signature(src, in.toks)) {
        res.error = "could not lay this file out without changing it; left as it is";
        return res;
    }
    res.ok = true;
    res.text = std::move(out);
    return res;
}

std::string Reindent(std::string_view text, const Options &options) {
    const std::string s(text);
    const Methods m = options.indent.empty() ? BaseMethods() : WithOptions(options);
    return Engine(s, m).IndentRegion();
}

int IndentLine(std::string_view text, int line, const Options &options) {
    const std::string s(text);
    const Methods m = options.indent.empty() ? BaseMethods() : WithOptions(options);
    return Engine(s, m).IndentOne(line);
}

}  // namespace schemefmt
