#include "lisp_format.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Two halves. The whitespace rules work on the gaps between the tokens a
// Common Lisp reader sees (Lex). The indentation is Emacs's, ported
// function by function from lisp-mode.el (`lisp-indent-region',
// `lisp-indent-calc-next', `calculate-lisp-indent') and cl-indent.el
// (`common-lisp-indent-function', `lisp-indent-259' and its helpers) over
// a small model of the Emacs primitives they use -- lisp-mode's syntax
// table, `parse-partial-sexp', `forward-sexp', `current-column' -- so
// that the corners come out the way Emacs's do rather than the way a
// tidier reimplementation would guess them. Comments name the Emacs
// function each piece stands for.

namespace lispfmt {
namespace {

// ============================================================================ reading

enum class TK { Open, Close, Atom, String, Prefix, Comment, Block };

struct Tok {
    size_t a = 0, b = 0;
    TK k = TK::Atom;
};

bool IsClWhitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool IsTerminating(char c) {
    return c == '"' || c == '\'' || c == '(' || c == ')' || c == ',' || c == ';' || c == '`';
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

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

struct Lexed {
    std::vector<Tok> toks;
    bool ok = false;
    std::string error;
    int error_line = 0;
};

/** @brief Splits Common Lisp source into the tokens the reader sees; fails on what does not read. */
Lexed Lex(const std::string &s) {
    Lexed r;
    const size_t n = s.size();
    std::vector<size_t> opens;
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
    // An atom's extent: constituents, `\x' escapes and `|...|' runs.
    auto atom_end = [&](size_t k) {
        while (k < n) {
            const char c = s[k];
            if (c == '\\') {
                k += 2;
                continue;
            }
            if (c == '|') {
                const size_t open = k++;
                while (k < n && s[k] != '|') k += s[k] == '\\' ? 2 : 1;
                if (k >= n) {
                    fail("unterminated |...| symbol", open);
                    return n;
                }
                k++;
                continue;
            }
            if (IsClWhitespace(c) || IsTerminating(c)) break;
            k++;
        }
        return std::min(k, n);
    };
    auto push = [&](size_t a, size_t b, TK k) { r.toks.push_back(Tok{a, b, k}); };
    size_t i = 0;
    while (i < n && !bad) {
        const char c = s[i];
        if (IsClWhitespace(c)) {
            i++;
        } else if (c == ';') {
            size_t e = s.find('\n', i);
            if (e == std::string::npos) e = n;
            push(i, e, TK::Comment);
            i = e;
        } else if (c == '"') {
            size_t k = i + 1;
            while (k < n && s[k] != '"') k += s[k] == '\\' ? 2 : 1;
            if (k >= n) {
                fail("unterminated string", i);
                break;
            }
            push(i, k + 1, TK::String);
            i = k + 1;
        } else if (c == '(') {
            opens.push_back(i);
            push(i, i + 1, TK::Open);
            i++;
        } else if (c == ')') {
            if (opens.empty()) {
                fail("unmatched close paren", i);
                break;
            }
            opens.pop_back();
            push(i, i + 1, TK::Close);
            i++;
        } else if (c == '\'' || c == '`') {
            push(i, i + 1, TK::Prefix);
            i++;
        } else if (c == ',') {
            const size_t e = i + 1 < n && (s[i + 1] == '@' || s[i + 1] == '.') ? i + 2 : i + 1;
            push(i, e, TK::Prefix);
            i = e;
        } else if (c == '#') {
            size_t j = i + 1;
            while (j < n && IsDigit(s[j])) j++;
            if (j >= n) {
                push(i, n, TK::Atom);
                i = n;
                continue;
            }
            const char d = s[j];
            if (d == '|' && j == i + 1) {
                int depth = 1;
                size_t k = j + 1;
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
            } else if (d == '\\') {
                size_t k = j + 1;
                if (k >= n) {
                    fail("unterminated character", i);
                    break;
                }
                k = std::min(n, k + Utf8Len(static_cast<unsigned char>(s[k])));
                while (k < n && !IsClWhitespace(s[k]) && !IsTerminating(s[k])) k++;
                push(i, k, TK::Atom);
                i = k;
            } else if (d == '(') {
                opens.push_back(i);
                push(i, j + 1, TK::Open);
                i = j + 1;
            } else if (d == '\'' || d == '+' || d == '-' || d == '.' || d == '=' || d == 'p' || d == 'P' || d == 'a' ||
                       d == 'A' || d == 's' || d == 'S' || d == 'c' || d == 'C') {
                push(i, j + 1, TK::Prefix);
                i = j + 1;
            } else if (IsClWhitespace(d) || IsTerminating(d)) {
                push(i, j, TK::Atom);
                i = j;
            } else {
                const size_t e = atom_end(j + 1);
                push(i, e, TK::Atom);
                i = e;
            }
        } else {
            const size_t e = atom_end(i);
            push(i, e, TK::Atom);
            i = e;
        }
    }
    if (!bad && !opens.empty()) fail("unclosed paren", opens.front());
    if (bad) {
        r.error = bad_msg;
        r.error_line = LineAt(s, bad_at);
        return r;
    }
    r.ok = true;
    return r;
}

std::string RStripBlanks(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

/** @brief The tokens, for the check: kinds and texts, no whitespace (and none at a comment's end). */
std::vector<std::string> Signature(const std::string &s, const std::vector<Tok> &toks) {
    std::vector<std::string> out;
    out.reserve(toks.size());
    for (const Tok &t : toks) {
        std::string text = s.substr(t.a, t.b - t.a);
        if (t.k == TK::Comment) text = RStripBlanks(text);
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
        out += t[k].k == TK::Comment ? RStripBlanks(text) : text;
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
            continue;  // `foo )' and a lone `)'
        }
        if (t[k].k == TK::Open && !lines && t[k + 1].k != TK::Comment) continue;  // `( foo'
        out += lines ? FixLines(gap) : std::string(gap);
    }
    out += '\n';
    return out;
}

// ============================================================================ indentation specs

/** @brief A cl-indent method: nil, an integer, a symbol, or a list of those. */
struct Node {
    enum Kind { Nil, Int, Sym, List } kind = Nil;
    int n = 0;
    std::string sym;
    std::vector<Node> items;
};

const Node kNil{};

bool ParseNode(const std::string &s, size_t &i, Node &out) {
    while (i < s.size() && IsClWhitespace(s[i])) i++;
    if (i >= s.size()) return false;
    if (s[i] == '(') {
        i++;
        out.kind = Node::List;
        for (;;) {
            while (i < s.size() && IsClWhitespace(s[i])) i++;
            if (i >= s.size()) return false;
            if (s[i] == ')') {
                i++;
                return true;
            }
            Node kid;
            if (!ParseNode(s, i, kid)) return false;
            out.items.push_back(std::move(kid));
        }
    }
    if (s[i] == ')') return false;
    const size_t a = i;
    while (i < s.size() && !IsClWhitespace(s[i]) && s[i] != '(' && s[i] != ')') i++;
    const std::string word = s.substr(a, i - a);
    if (word == "nil") {
        out.kind = Node::Nil;
    } else if (!word.empty() && std::all_of(word.begin(), word.end(), IsDigit)) {
        out.kind = Node::Int;
        out.n = std::stoi(word);
    } else {
        out.kind = Node::Sym;
        out.sym = word;
    }
    return true;
}

std::optional<Node> ParseSpec(const std::string &s) {
    size_t i = 0;
    Node n;
    if (!ParseNode(s, i, n)) return std::nullopt;
    while (i < s.size() && IsClWhitespace(s[i])) i++;
    if (i != s.size()) return std::nullopt;
    return n;
}

const char *const kDefunSpec = "(4 &lambda &body)";

// The `common-lisp-indent-function' properties cl-indent.el sets, and the
// `lisp-indent-function' ones Emacs's own macros declare: cl-indent falls
// back on those for a name it has no spec for, so an Emacs user's CL code
// gets them too (`when-let', `if-let', `save-excursion'...). Dumped from
// `emacs -Q' 30.2 with lisp-mode loaded; `defun' is spelled out.
struct GroupSpec {
    const char *spec;
    std::vector<const char *> names;
};

const std::vector<GroupSpec> &GroupSpecs() {
    static const std::vector<GroupSpec> groups = {
    {"0",
     {"atomic-change-group", "benchmark-progn", "combine-after-change-calls", "delay-mode-hooks", "dont-compile",
      "eval-and-compile", "eval-when-compile", "ignore-errors", "progn", "replace--push-stack", "return",
      "save-current-buffer", "save-excursion", "save-mark-and-excursion", "save-match-data", "save-restriction",
      "save-selected-window", "save-window-excursion", "track-mouse", "while-no-input",
      "with-auto-compression-mode", "with-existing-directory", "with-local-quit",
      "with-minibuffer-completions-window", "with-minibuffer-selected-window", "with-no-warnings",
      "with-silent-modifications", "with-temp-buffer", "with-undo-amalgamate", "without-remote-files",
      "without-restriction"}},
    {"1",
     {"and-let*", "benchmark-run", "benchmark-run-compiled", "block", "catch", "cl-generic-define-generalizer",
      "def-edebug-elem-spec", "def-edebug-spec", "define-generic-mode", "define-ibuffer-sorter", "deftheme",
      "defvar-keymap", "dlet", "easy-mmode-defmap", "easy-mmode-defsyntax", "ert-font-lock-deftest",
      "ert-font-lock-deftest-file", "eval-after-load", "eval-when", "gv-define-expander", "ignore-error",
      "let-alist", "let-when-compile", "letrec", "locally", "macroexp--accumulate",
      "macroexp--with-extended-form-stack", "minibuffer-with-setup-hook", "multiple-value-prog1",
      "oclosure-define", "pcase", "pcase-dolist", "pcase-exhaustive", "pcase-let", "pcase-let*", "prog1",
      "rx-let", "rx-let-eval", "seq-doseq", "throw", "unless", "wallpaper-setter-create", "when", "when-let",
      "when-let*", "while", "while-let", "with-case-table", "with-category-table", "with-coding-priority",
      "with-connection-local-application-variables", "with-current-buffer", "with-delayed-message",
      "with-demoted-errors", "with-environment-variables", "with-eval-after-load", "with-file-modes",
      "with-help-window", "with-locale-environment", "with-memoization", "with-mutex",
      "with-output-to-temp-buffer", "with-selected-frame", "with-selected-window", "with-suppressed-warnings",
      "with-syntax-table", "with-temp-file", "with-temp-message", "with-timeout", "with-window-non-dedicated"}},
    {"2",
     {"bindings--define-key", "cl--define-built-in-type", "cl-defgeneric", "combine-change-calls",
      "comment-with-narrowing", "condition-case", "condition-case-unless-debug", "defadvice", "define-advice",
      "define-ibuffer-filter", "define-ibuffer-op", "defvar-local", "dolist-with-progress-reporter",
      "dotimes-with-progress-reporter", "eldoc--documentation-strategy-defcustom", "ert-deftest",
      "gv-define-setter", "gv-letplace", "if-let", "if-let*", "macroexp-let2*", "named-let", "oclosure-lambda",
      "pcase-defmacro", "prog2", "seq-let", "static-if", "with-restriction", "with-wrapper-hook"}},
    {"3",
     {"macroexp-let2", "oclosure--lambda", "with-current-buffer-window", "with-displayed-buffer-window",
      "with-temp-buffer-window"}},
    {"(4 &lambda &body)",
     {"autoload", "cl-defmethod", "cl-generic-define-context-rewriter", "defalias", "defface", "defgeneric",
      "defgroup", "defimage", "define-abbrev", "define-abbrev-table", "define-alternatives", "define-auto-insert",
      "define-button-type", "define-category", "define-ccl-program", "define-char-code-property",
      "define-charset", "define-charset-internal", "define-coding-system", "define-derived-mode",
      "define-fringe-bitmap", "define-globalized-minor-mode", "define-ibuffer-column", "define-inline",
      "define-iso-single-byte-charset", "define-key-after", "define-keymap", "define-mail-user-agent",
      "define-minor-mode", "define-modify-macro", "define-multisession-variable",
      "define-obsolete-function-alias", "define-obsolete-variable-alias", "define-setf-expander",
      "define-setf-method", "define-short-documentation-group", "define-skeleton",
      "define-translation-hash-table", "define-translation-table", "define-widget", "define-widget-keywords",
      "defmacro", "defmath", "defsubst", "deftype", "defun", "defvaralias", "easy-menu-define",
      "isearch-define-mode-toggle", "keymap-set-after", "pcase-lambda", "rx-define", "transient-append-suffix",
      "transient-insert-suffix", "transient-remove-suffix", "transient-replace-suffix", "use-package",
      "which-key-add-keymap-based-replacements", "which-key-add-major-mode-key-based-replacements"}},
    };
    return groups;
}

const std::vector<std::pair<const char *, const char *>> &SingleSpecs() {
    static const std::vector<std::pair<const char *, const char *>> singles = {
    {":method", "(&lambda &body)"},
    {"case", "(4 &rest (&whole 2 &rest 1))"},
    {"ccase", "(4 &rest (&whole 2 &rest 1))"},
    {"compiler-let", "((&whole 4 &rest (&whole 1 1 2)) &body)"},
    {"cond", "(&rest (&whole 2 &rest 1))"},
    {"ctypecase", "(4 &rest (&whole 2 &rest 1))"},
    {"defclass", "(6 4 (&whole 2 &rest 1) (&whole 2 &rest 1))"},
    {"defconst", "(4 2 2 2)"},
    {"defconstant", "(4 2 2)"},
    {"defcustom", "(4 2 2 2)"},
    {"define-condition", "(6 4 (&whole 2 &rest 1) (&whole 2 &rest 1))"},
    {"defmethod", "lisp-indent-defmethod"},
    {"defpackage", "(4 2)"},
    {"defparameter", "(4 2 2)"},
    {"defsetf", "(4 &lambda 4 &body)"},
    {"defstruct", "((&whole 4 &rest (&whole 2 &rest 1)) &rest (&whole 2 &rest 1))"},
    {"defvar", "(4 2 2)"},
    {"destructuring-bind", "((&whole 6 &rest 1) 4 &body)"},
    {"do", "lisp-indent-do"},
    {"do*", "lisp-indent-do"},
    {"dolist", "((&whole 4 2 1) &body)"},
    {"dotimes", "((&whole 4 2 1) &body)"},
    {"ecase", "(4 &rest (&whole 2 &rest 1))"},
    {"etypecase", "(4 &rest (&whole 2 &rest 1))"},
    {"flet", "((&whole 4 &rest (&whole 1 &lambda &body)) &body)"},
    {"generic-flet", "((&whole 4 &rest (&whole 1 &lambda &body)) &body)"},
    {"generic-labels", "((&whole 4 &rest (&whole 1 &lambda &body)) &body)"},
    {"handler-bind", "((&whole 4 &rest (&whole 1 1 2)) &body)"},
    {"handler-case", "(4 &rest (&whole 2 &lambda &body))"},
    {"if", "(&rest nil)"},
    {"labels", "((&whole 4 &rest (&whole 1 &lambda &body)) &body)"},
    {"lambda", "(&lambda &rest lisp-indent-function-lambda-hack)"},
    {"let", "((&whole 4 &rest (&whole 1 1 2)) &body)"},
    {"let*", "((&whole 4 &rest (&whole 1 1 2)) &body)"},
    {"macrolet", "((&whole 4 &rest (&whole 1 &lambda &body)) &body)"},
    {"multiple-value-bind", "((&whole 6 &rest 1) 4 &body)"},
    {"multiple-value-call", "(4 &body)"},
    {"multiple-value-setf", "(4 2)"},
    {"multiple-value-setq", "(4 2)"},
    {"pprint-logical-block", "(4 2)"},
    {"print-unreadable-object", "((&whole 4 1 &rest 1) &body)"},
    {"prog", "(&lambda &rest lisp-indent-tagbody)"},
    {"prog*", "(&lambda &rest lisp-indent-tagbody)"},
    {"progv", "(4 4 &body)"},
    {"restart-bind", "((&whole 4 &rest (&whole 1 1 2)) &body)"},
    {"restart-case", "(4 &rest (&whole 2 &lambda &body))"},
    {"return-from", "(nil &body)"},
    {"symbol-macrolet", "((&whole 4 &rest (&whole 1 1 2)) &body)"},
    {"tagbody", "lisp-indent-tagbody"},
    {"typecase", "(4 &rest (&whole 2 &rest 1))"},
    {"unwind-protect", "(5 &body)"},
    {"with-accessors", "((&whole 6 &rest 1) 4 &body)"},
    {"with-compilation-unit", "(&lambda &body)"},
    {"with-condition-restarts", "((&whole 6 &rest 1) 4 &body)"},
    {"with-output-to-string", "(4 2)"},
    {"with-slots", "((&whole 6 &rest 1) 4 &body)"},
    {"with-standard-io-syntax", "(2)"},
    };
    return singles;
}

using Methods = std::unordered_map<std::string, Node>;

const Methods &BaseMethods() {
    static const Methods m = [] {
        Methods out;
        for (const GroupSpec &g : GroupSpecs()) {
            const Node n = *ParseSpec(g.spec);
            for (const char *name : g.names) out[name] = n;
        }
        for (const auto &[name, spec] : SingleSpecs()) out[name] = *ParseSpec(spec);
        return out;
    }();
    return m;
}

/** @brief The base table with the caller's specs applied; nullptr-free, built once per call when needed. */
Methods WithOptions(const Options &o) {
    Methods m = BaseMethods();
    for (const auto &[name, spec] : o.indent) {
        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
        if (spec.empty()) {
            m.erase(key);
            continue;
        }
        if (spec == "defun") {
            m[key] = *ParseSpec(kDefunSpec);
            continue;
        }
        std::optional<Node> n = ParseSpec(spec);
        if (!n) continue;
        if (n->kind == Node::Sym && n->sym.rfind("lisp-indent-", 0) != 0) {
            // Another operator's name: indent like it.
            const auto it = BaseMethods().find(n->sym);
            if (it == BaseMethods().end()) continue;
            n = it->second;
        }
        m[key] = *n;
    }
    return m;
}

// ============================================================================ the Emacs model

// lisp-mode-syntax-table, as far as indentation can tell.
enum class Syn { Ws, Word, Symbol, Open, Close, Quote, String, Escape, Comment, EndComment };

Syn Class(unsigned char c) {
    switch (c) {
        case ' ':
        case '\t':
        case '\f': return Syn::Ws;
        case '\n': return Syn::EndComment;
        case '(': return Syn::Open;
        case ')': return Syn::Close;
        case '"':
        case '|': return Syn::String;
        case '\\': return Syn::Escape;
        case ';': return Syn::Comment;
        case '\'':
        case '`':
        case ',':
        case '#': return Syn::Quote;
        default: break;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(static_cast<char>(c)) || c >= 0x80) {
        return Syn::Word;
    }
    return Syn::Symbol;
}

bool IsWordOrSymbol(unsigned char c) {
    const Syn k = Class(c);
    return k == Syn::Word || k == Syn::Symbol;
}

char Lower(unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); }

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
    int comment = 0;       // nth 4: -1 in a `;' comment, the nesting depth in a `#|' one
    long comstr = -1;      // nth 8: where that string or comment started
    std::vector<long> opens;  // nth 9: every open paren, outermost first
};

struct Stop {
    PState st;
    long pos = 0;
};

constexpr int kNoTarget = INT_MIN;

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

    /** @brief `lisp-indent-line's column for one line, the text as it stands; -1 inside a string or `#|'. */
    int IndentOne(int line);

  private:
    std::string done_;
    const std::string &rest_;
    size_t roff_ = 0;
    const Methods &methods_;
    std::vector<long> line_starts_;   // of the original text
    std::vector<long> done_starts_;   // of the finished lines

    long Size() const { return static_cast<long>(done_.size() + rest_.size() - roff_); }
    unsigned char At(long p) const {
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
                unsigned cp = len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : len == 4 ? c & 0x07 : c;
                bool valid = len > 1;
                for (size_t k = 1; k < len; k++) {
                    const unsigned char cc = At(q + static_cast<long>(k));
                    if ((cc & 0xC0) != 0x80) valid = false;
                    cp = (cp << 6) | (cc & 0x3F);
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

    long BackwardPrefixChars(long p) const {
        while (p > 0) {
            const unsigned char c = At(p - 1);
            if (c != '\'' && c != '`' && c != ',' && c != '#' && c != '@') break;
            p--;
        }
        return p;
    }

    long SkipBlanks(long p) const {
        while (p < Size() && (At(p) == ' ' || At(p) == '\t')) p++;
        return p;
    }

    Stop Parse(long from, long to, int target, bool stopbefore, const PState *old, bool stop_string_end = false) const;
    std::optional<long> ForwardSexp(long from) const;
    long SexpStart(long from, long limit) const { return Parse(from, limit, 0, true, nullptr).pos; }

    Ind Calculate(long ip, PState st) const;
    Ind CommonLispIndent(long ip, const PState &st, int normal) const;
    Ind LoopPart(const PState &st) const;
    bool ExtendedLoop(long loop_start) const;

    Ind Lisp259(const Node &method, const std::vector<int> &path, const PState &st, long ip, int sexp_col,
                Ind normal, int level) const;
    Ind CallFunction(const std::string &name, const std::vector<int> &path, const PState &st, long ip, int sexp_col,
                     Ind normal, int level) const;
    Ind Tagbody(const std::vector<int> &path, long ip, int sexp_col, Ind normal, int body) const;
    int LambdaList(long ip, int sexp_col, long cfs) const;
    bool LambdaKeywordAt(long p, long limit, long *end) const;
    const Node *FindMethod(const std::string &name) const {
        const auto it = methods_.find(name);
        return it == methods_.end() ? nullptr : &it->second;
    }

    void CommitLine(Ind indent, const PState &at_start);
    void CommitVerbatim(long upto);
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
    long p = from;
    auto finish = [&]() {
        s.inner = s.opens.empty() ? -1 : s.opens.back();
        s.last = lv.back().first;
        r.pos = p;
        return r;
    };
    for (;;) {
        if (s.instr) {
            while (p < to) {
                const unsigned char c = At(p);
                if (c == static_cast<unsigned char>(s.instr)) break;
                p += c == '\\' ? 2 : 1;
            }
            if (p >= to) {
                p = std::min(p, to);
                return finish();
            }
            p++;
            s.instr = 0;
            lv.back().first = lv.back().second;
            if (stop_string_end) return finish();
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
        if (c == '#' && p + 1 < to && At(p + 1) == '|') {
            s.comment = 1;
            s.comstr = p;
            p += 2;
            continue;
        }
        if (c == '@') {  // the prefix flag: never starts anything
            p++;
            continue;
        }
        switch (Class(c)) {
            case Syn::Ws:
            case Syn::EndComment:
            case Syn::Quote: p++; break;
            case Syn::Comment:
                s.comment = -1;
                s.comstr = p;
                p++;
                break;
            case Syn::Escape:
            case Syn::Word:
            case Syn::Symbol: {
                if (stopbefore) {
                    r.pos = p;
                    s.inner = s.opens.empty() ? -1 : s.opens.back();
                    s.last = lv.back().first;
                    return r;
                }
                lv.back().second = p;
                if (Class(c) == Syn::Escape) p++;
                p++;
                while (p < to) {
                    const Syn k = Class(At(p));
                    if (k == Syn::Escape) {
                        p += 2;
                    } else if (k == Syn::Word || k == Syn::Symbol || k == Syn::Quote) {
                        p++;
                    } else {
                        break;
                    }
                }
                p = std::min(p, to);
                lv.back().first = lv.back().second;
                break;
            }
            case Syn::Open:
                if (stopbefore) {
                    r.pos = p;
                    s.inner = s.opens.empty() ? -1 : s.opens.back();
                    s.last = lv.back().first;
                    return r;
                }
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
                if (stopbefore) {
                    r.pos = p;
                    s.inner = s.opens.empty() ? -1 : s.opens.back();
                    s.last = lv.back().first;
                    return r;
                }
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
        p++;
        if (c == '@') continue;
        switch (Class(c)) {
            case Syn::Escape:
                if (p >= n) return std::nullopt;
                p++;
                [[fallthrough]];
            case Syn::Word:
            case Syn::Symbol:
                if (depth) break;
                while (p < n) {
                    const Syn k = Class(At(p));
                    if (k == Syn::Escape) {
                        p++;
                        if (p >= n) return std::nullopt;
                        p++;
                    } else if (k == Syn::Word || k == Syn::Symbol || k == Syn::Quote) {
                        p++;
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
            case Syn::String:
                for (;;) {
                    if (p >= n) return std::nullopt;
                    const unsigned char d = At(p);
                    if (d == c) break;
                    p += d == '\\' ? 2 : 1;
                }
                p++;
                if (depth == 0) return p;
                break;
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
    const bool ws_after_open = Class(At(after_open)) == Syn::Ws;
    if (last_sexp < 0) return IntInd(Column(after_open));  // right after the open paren
    long pt = SexpStart(after_open, last_sexp);
    if (At(pt) == '(') {
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
    const Ind got = CommonLispIndent(ip, st, normal);
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

/** @brief `common-lisp-indent-function' (and its `-1'): the spec-driven column, or nothing. */
Ind Engine::CommonLispIndent(long ip, const PState &st, int normal_col) const {
    const long c1 = st.inner;
    if (c1 + 4 < Size() && At(c1) == '(' && Lower(At(c1 + 1)) == 'l' && Lower(At(c1 + 2)) == 'o' &&
        Lower(At(c1 + 3)) == 'o' && Lower(At(c1 + 4)) == 'p') {
        return LoopPart(st);
    }
    // common-lisp-indent-function-1
    Ind normal = IntInd(normal_col);
    std::vector<int> path;  // outermost level first
    Ind calculated, tentative;
    const long cfs = st.inner;
    const int sexp_col = Column(cfs);
    int open_index = static_cast<int>(st.opens.size()) - 1;
    long containing = cfs;
    for (int depth = 0; calculated.kind == Ind::None && depth < 3;) {
        long p = Parse(containing + 1, ip, 1, true, nullptr).pos;
        std::string function;
        const Node *method = nullptr;
        if (p < Size() && IsWordOrSymbol(At(p))) {
            const std::optional<long> e = ForwardSexp(p);
            const long end = e ? *e : p;
            for (long q = p; q < end; q++) function += Lower(At(q));
            method = FindMethod(function);
            // The package prefix: `sb-c:defknown' is `defknown'. (A name Emacs
            // has interned keeps its colon; the only one that matters here.)
            if (!method && function != ":default-char") {
                for (size_t k = 0; k + 1 < function.size(); k++) {
                    if (function[k] == ':' && function[k + 1] != ':') {
                        function = function.substr(k + 1);
                        method = FindMethod(function);
                        break;
                    }
                }
            }
        }
        const bool has_function = !function.empty();
        // How far into the containing form is the current form?
        int n = 0;
        if (p < ip) {
            for (;;) {
                const std::optional<long> e = ForwardSexp(p);
                if (!e) break;
                p = *e;
                if (p >= ip) break;
                p = Parse(p, ip, 1, true, nullptr).pos;
                n++;
            }
        }
        path.insert(path.begin(), n);
        bool tentative_defun = false;
        Node with_method;
        if (has_function && !method && path.size() == 1) {
            if (function.compare(0, 3, "def") == 0) {
                tentative_defun = true;
            } else if (function.compare(0, 5, "with-") == 0 || function.compare(0, 8, "without-") == 0 ||
                       function.compare(0, 3, "do-") == 0) {
                with_method = *ParseSpec("(&lambda &body)");
                method = &with_method;
            }
        }
        const unsigned char before = containing > 0 ? At(containing - 1) : 0;
        const unsigned char before2 = containing > 1 ? At(containing - 2) : 0;
        // A backquote substitution: tentatively where it is, and no further up
        // than this level -- but its own spec still applies (that cond
        // clause's test is nil, so cl-indent falls through to the others).
        const bool substitution = before == ',' || (before == '@' && before2 == ',');
        if (substitution) {
            tentative = normal;
            depth = 3;
        }
        if (before == '\'' && before2 != '#') {
            calculated = IntInd(1 + sexp_col);  // a quoted list
        } else if (before == '#') {
            calculated = IntInd(1 + sexp_col);  // a vector
        } else if (!method) {
            if (tentative_defun) {
                tentative = Lisp259(*ParseSpec(kDefunSpec), path, st, ip, sexp_col, normal, open_index);
                normal = tentative;
            }
        } else if (method->kind == Node::Int) {
            const int m = method->n;
            if (path.size() > 1) {
                calculated = normal;
            } else if (path[0] <= m) {
                calculated = ListInd(sexp_col + 4);  // a distinguished form
            } else if (path[0] == m + 1) {
                calculated = IntInd(sexp_col + 2);  // the first body form
            } else {
                calculated = normal;
            }
        } else if (method->kind == Node::Sym) {
            calculated = CallFunction(method->sym, path, st, ip, sexp_col, normal, open_index);
        } else {
            calculated = Lisp259(*method, path, st, ip, sexp_col, normal, open_index);
        }
        if (calculated.kind != Ind::None) break;
        // backward-up-list
        if (depth >= 3) break;
        if (open_index <= 0) break;
        open_index--;
        containing = st.opens[static_cast<size_t>(open_index)];
        depth++;
    }
    return calculated.kind != Ind::None ? calculated : tentative;
}

/** @brief `common-lisp-loop-part-indentation': six past the paren in an extended loop, one in a simple one. */
Ind Engine::LoopPart(const PState &st) const {
    const int loop_col = Column(st.inner);
    return ListInd(loop_col + (ExtendedLoop(st.inner) ? 6 : 1));
}

/** @brief `lisp-extended-loop-p': the form after `loop' starts with a word or a colon. */
bool Engine::ExtendedLoop(long loop_start) const {
    const std::optional<long> e1 = ForwardSexp(loop_start + 1);
    if (!e1) return true;
    const std::optional<long> e2 = ForwardSexp(*e1);
    if (!e2 || *e2 >= Size()) return true;
    const long s = BackwardPrefixChars(SexpStart(*e1, *e2));
    const unsigned char c = At(s);
    return c == ':' || Class(c) == Syn::Word;
}

/** @brief `lisp-indent-259': a cl-indent method applied to a path of element indices, outermost first. */
Ind Engine::Lisp259(const Node &method_in, const std::vector<int> &path, const PState &st, long ip, int sexp_col,
                    Ind normal, int level) const {
    if (method_in.kind != Node::List) return Ind{};
    const std::vector<Node> *m = &method_in.items;
    size_t mi = 0;
    size_t pi = 0;
    while (pi < path.size()) {
        std::optional<int> n = path[pi] - 1;
        pi++;
        const bool last = pi >= path.size();
        bool tail = false;
        while (n) {
            const Node &tem = mi < m->size() ? (*m)[mi] : kNil;
            const bool is_sym = tem.kind == Node::Sym;
            if (tail && tem.kind == Node::Int) return normal;
            if (is_sym && tem.sym == "&body") return (*n == 0 && last) ? IntInd(sexp_col + 2) : normal;
            if (is_sym && tem.sym == "&rest") {
                tail = *n > 0;
                n = 0;
                mi++;
                continue;
            }
            if (*n > 0) {
                n = *n - 1;
                mi++;
                continue;
            }
            if (tem.kind == Node::Nil) return normal.kind == Ind::List ? normal : ListInd(normal.col);
            if (is_sym && tem.sym == "&lambda") {
                if (last) return ListInd(sexp_col + 4);
                if (pi + 1 == path.size()) return ListInd(LambdaList(ip, sexp_col, st.inner));
                return normal;
            }
            if (tem.kind == Node::Int) return last ? ListInd(sexp_col + tem.n) : normal;
            if (is_sym) return CallFunction(tem.sym, path, st, ip, sexp_col, normal, level);
            // (&whole X . rest): descend, or indent the list itself by X.
            if (!last) {
                m = &tem.items;
                mi = 2;
                n.reset();
                continue;
            }
            const Node &w = tem.items.size() > 1 ? tem.items[1] : kNil;
            if (tail) return normal;
            if (w.kind == Node::Nil) return ListInd(normal.col);
            if (w.kind == Node::Int) return ListInd(sexp_col + w.n);
            return CallFunction(w.sym, path, st, ip, sexp_col, normal, level);
        }
    }
    return Ind{};
}

/** @brief The cl-indent functions a method can name: tagbody, do, defmethod and the lambda hack. */
Ind Engine::CallFunction(const std::string &name, const std::vector<int> &path, const PState &st, long ip,
                         int sexp_col, Ind normal, int level) const {
    if (name == "lisp-indent-tagbody") return Tagbody(path, ip, sexp_col, normal, 3);
    if (name == "lisp-indent-do") {
        if (path[0] >= 3) return Tagbody(path, ip, sexp_col, normal, 2);
        static const Node do_method = *ParseSpec("((&whole nil &rest) (&whole nil &rest 1))");
        return Lisp259(do_method, path, st, ip, sexp_col, normal, level);
    }
    if (name == "lisp-indent-defmethod") {
        // The qualifiers of the top-level form (beginning-of-defun's, not
        // necessarily this defmethod's) each count as one more argument.
        // Emacs looks for that form from the end of the method's sexp being
        // indented, which in a file not yet indented can be a line of the
        // method's own body sitting in column 0; the outermost open paren is
        // what it finds in an indented file, and keeps the result the same
        // the second time round.
        int nqual = 0;
        if (path[0] >= 3) {
            long q = st.opens.front() + 1;
            std::optional<long> e = ForwardSexp(q);
            if (e) e = ForwardSexp(*e);
            if (e) {
                q = *e;
                auto skip = [&]() {
                    while (q < Size() && (At(q) == ' ' || At(q) == '\t' || At(q) == '\n')) q++;
                };
                skip();
                while (q < Size() && IsWordOrSymbol(At(q))) {
                    nqual++;
                    const std::optional<long> f = ForwardSexp(q);
                    if (!f) break;
                    q = *f;
                    skip();
                }
            }
        }
        std::string spec = "(4";
        for (int k = 0; k < nqual; k++) spec += " 4";
        spec += " &lambda &body)";
        return Lisp259(*ParseSpec(spec), path, st, ip, sexp_col, normal, level);
    }
    if (name == "lisp-indent-function-lambda-hack") {
        if (path.size() > 1 || path[0] > 3) return normal;
        // Under `function' rather than `lambda' in (function (lambda ...)).
        if (level <= 0) return IntInd(sexp_col + 2);
        long q = st.opens[static_cast<size_t>(level - 1)] + 1;
        const long q0 = q;
        if (q + 5 <= Size() && Lower(At(q)) == 'l' && Lower(At(q + 1)) == 'i' && Lower(At(q + 2)) == 's' &&
            Lower(At(q + 3)) == 'p' && At(q + 4) == ':') {
            q += 5;
            while (q < Size() && At(q) == ':') q++;
        }
        static const char kFunction[] = "function";
        bool match = q + 8 < Size();
        for (int k = 0; match && k < 8; k++) match = Lower(At(q + k)) == kFunction[k];
        if (match) return IntInd(2 - 1 + Column(q0));
        return IntInd(sexp_col + 2);
    }
    return Ind{};
}

/** @brief `lisp-indent-tagbody': tags one column in, statements `body' columns in. */
Ind Engine::Tagbody(const std::vector<int> &path, long ip, int sexp_col, Ind normal, int body) const {
    if (path.size() > 1) return normal;
    const long q = SkipBlanks(ip);
    if (q < Size() && IsWordOrSymbol(At(q))) return ListInd(sexp_col + 1);
    return ListInd(sexp_col + body);
}

/** @brief Matches lisp-indent-lambda-list-keywords-regexp at `p', ending no later than `limit'. */
bool Engine::LambdaKeywordAt(long p, long limit, long *end) const {
    static const char *const kWords[] = {"optional", "rest", "key", "allow-other-keys", "aux",
                                         "whole",    "body", "environment"};
    if (At(p) != '&') return false;
    for (const char *w : kWords) {
        long q = p + 1;
        bool ok = true;
        for (const char *c = w; *c && ok; c++, q++) ok = q < Size() && Lower(At(q)) == *c;
        if (!ok) continue;
        if (q < Size() && (At(q) == ' ' || At(q) == '\t') && q + 1 <= limit) {
            *end = q + 1;
            return true;
        }
        if ((q >= Size() || At(q) == '\n') && q <= limit) {
            *end = q;
            return true;
        }
    }
    return false;
}

/** @brief `lisp-indent-lambda-list': a keyword one past the paren, anything else two past the last keyword. */
int Engine::LambdaList(long ip, int sexp_col, long cfs) const {
    long end = 0;
    const long first = SkipBlanks(ip);
    if (LambdaKeywordAt(first, LONG_MAX, &end)) return sexp_col + 1;
    const long origin = ip - 1;  // the end of the line before
    for (long s = origin; s >= cfs && s >= 0; s--) {
        if (LambdaKeywordAt(s, origin, &end)) return Column(s) + 2;
    }
    return sexp_col + 1;
}

/** @brief The first line of the rest (up to and including its newline), with its new margin. */
void Engine::CommitLine(Ind indent, const PState &at_start) {
    done_starts_.push_back(static_cast<long>(done_.size()));
    size_t e = rest_.find('\n', roff_);
    e = e == std::string::npos ? rest_.size() : e + 1;
    const std::string line = rest_.substr(roff_, e - roff_);
    roff_ = e;
    size_t f = 0;
    while (f < line.size() && (line[f] == ' ' || line[f] == '\t')) f++;
    const std::string body = line.substr(f);
    if (at_start.instr || at_start.comment > 0) {
        done_ += line;  // in a string or a `#|' comment: as it is
        return;
    }
    if (body.empty() || body[0] == '\n') {
        done_ += body;  // blank: empty
        return;
    }
    // Kept where they are: `;;;' and single-`;' comment lines, and the first
    // line of a multi-line `#|' comment. Their margin becomes spaces.
    bool keep = indent.kind == Ind::None;
    if (body[0] == ';') keep = keep || body.compare(0, 3, ";;;") == 0 || body.size() < 2 || body[1] != ';';
    if (body.compare(0, 2, "#|") == 0) {
        int nest = 0;
        size_t k = 0;
        for (; k < body.size() && body[k] != '\n'; k++) {
            if (body.compare(k, 2, "#|") == 0) {
                nest++;
                k++;
            } else if (body.compare(k, 2, "|#") == 0) {
                if (--nest == 0) break;
                k++;
            }
        }
        if (nest > 0) keep = true;
    }
    int col = indent.col;
    if (keep) {
        col = 0;
        for (size_t k = 0; k < f; k++) col = line[k] == '\t' ? (col / 8 + 1) * 8 : col + 1;
    }
    done_ += std::string(static_cast<size_t>(std::max(0, col)), ' ');
    done_ += body;
}

/** @brief Finishes, unchanged, every line starting before `upto' (lines inside a string). */
void Engine::CommitVerbatim(long upto) {
    while (roff_ < rest_.size() && static_cast<long>(done_.size()) < upto) {
        done_starts_.push_back(static_cast<long>(done_.size()));
        size_t e = rest_.find('\n', roff_);
        e = e == std::string::npos ? rest_.size() : e + 1;
        done_ += rest_.substr(roff_, e - roff_);
        roff_ = e;
    }
}

/** @brief `lisp-indent-region': the first line, then `lisp-indent-calc-next' and its column cache for the rest. */
std::string Engine::IndentRegion() {
    PState st;
    CommitLine(Calculate(0, st), st);
    std::vector<int> stack(1, -1);  // cached columns, one per depth; -1 is nil
    long ppss_point = 0;
    for (;;) {
        const int indent_depth = st.depth;
        int depth = indent_depth;
        long eol = Eol(done_starts_.back());
        for (;;) {
            const long last = st.last;
            Stop r = Parse(ppss_point, eol, kNoTarget, false, &st);
            st = r.st;
            if (st.last < 0 && depth == st.depth) st.last = last;
            depth = st.depth;
            ppss_point = eol;
            if (eol < Size() && st.instr) {
                // Skip over newlines within strings.
                const long string_start = st.comstr;
                Stop r2 = Parse(eol, Size(), kNoTarget, false, &st, true);
                st = r2.st;
                st.last = string_start;
                depth = st.depth;
                ppss_point = r2.pos;
                CommitVerbatim(r2.pos);
                eol = Eol(r2.pos);
                if (st.instr) break;  // never closed
                continue;
            }
            break;
        }
        ppss_point = eol;
        const int delta = depth - indent_depth;
        if (delta < 0) {
            stack.resize(static_cast<size_t>(std::max(0, static_cast<int>(stack.size()) + delta)));
        } else {
            stack.insert(stack.end(), static_cast<size_t>(delta), -1);
        }
        if (eol >= Size() || roff_ >= rest_.size()) break;  // no next line
        // The next line starts at eol + 1, the start of the rest.
        Ind indent;
        if (stack.empty()) {
            st = PState{};
            indent = ListInd(0);
        } else if (stack.back() >= 0) {
            indent = IntInd(stack.back());
        } else {
            indent = Calculate(eol + 1, st);
            if (indent.kind == Ind::Int) stack.back() = indent.col;
        }
        CommitLine(indent, st);
    }
    CommitVerbatim(LONG_MAX);
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
    const Lexed in = Lex(src);
    if (!in.ok) {
        res.error = in.error;
        res.error_line = in.error_line;
        return res;
    }
    const std::string gaps = FixGaps(src, in.toks);
    std::string out;
    if (options.indent.empty()) {
        out = Engine(gaps, BaseMethods()).IndentRegion();
    } else {
        const Methods m = WithOptions(options);
        out = Engine(gaps, m).IndentRegion();
    }
    const Lexed again = Lex(out);
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
    if (options.indent.empty()) return Engine(s, BaseMethods()).IndentRegion();
    const Methods m = WithOptions(options);
    return Engine(s, m).IndentRegion();
}

int IndentLine(std::string_view text, int line, const Options &options) {
    const std::string s(text);
    if (options.indent.empty()) return Engine(s, BaseMethods()).IndentOne(line);
    const Methods m = WithOptions(options);
    return Engine(s, m).IndentOne(line);
}

}  // namespace lispfmt
