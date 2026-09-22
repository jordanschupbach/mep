// The front end of mep's own C language server: lexer, preprocessor
// model and parser. See c_ast.h for the shape of what comes out and for
// the reasoning behind the preprocessor half, which is the part that has
// no counterpart in python_ast.cpp.
//
// Three passes, in this order:
//   1. TokenizeC       -- bytes to tokens, directives and comments,
//                         following backslash line continuations.
//   2. SelectCPreprocessorArms -- decide which arm of every `#if` group
//                         the parser gets to see.
//   3. ParseC          -- recursive descent over the surviving tokens.
//
// The parser recovers rather than stopping at the first error (a file
// being typed into still has to produce symbols, folds and completions
// for the parts that are fine), and it reports at most one error per
// line, because a single missing `;` otherwise cascades into a pile of
// them that buries the one the reader needs.

#include "c_ast.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// --- Character classes ------------------------------------------------

/** @brief Reports whether a byte is C whitespace (a newline included). */
bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

/** @brief Reports whether a byte is a decimal digit. */
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// The reserved words this parser knows. C23's own new keywords that are
// spelled like ordinary identifiers -- `bool`, `true`, `false`,
// `nullptr`, `alignas`, `static_assert`, `thread_local`, `constexpr` --
// are deliberately NOT here: in the C99 and C11 code that makes up most
// of the world they are macros from <stdbool.h>/<stdalign.h>, and code
// predating those headers defines its own (`typedef enum { false, true }
// bool;` is real, and making `bool` a keyword would report it as a
// syntax error). They live in the server's vocabulary instead, where
// being wrong about one costs a missing completion rather than a false
// error. `_Bool`, `_Static_assert` and friends have no such problem and
// are keywords here.
const char *const kKeywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern", "float",
    "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short", "signed", "sizeof",
    "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
    // C99/C11/C23 underscore-capital spellings.
    "_Alignas", "_Alignof", "_Atomic", "_BitInt", "_Bool", "_Complex", "_Decimal128", "_Decimal32", "_Decimal64",
    "_Generic", "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local", "_Float16", "_Float32", "_Float32x",
    "_Float64", "_Float64x", "_Float128",
    // GNU C, which is what real code is actually written in. Leaving
    // these out does not make them go away; it only turns every
    // `__attribute__((packed))` into a syntax error.
    "asm", "typeof", "__asm", "__asm__", "__attribute", "__attribute__", "__alignof", "__alignof__", "__complex__",
    "__const", "__const__", "__extension__", "__float128", "__inline", "__inline__", "__int128", "__label__",
    "__restrict", "__restrict__", "__signed", "__signed__", "__thread", "__typeof", "__typeof__", "__typeof_unqual",
    "__volatile", "__volatile__", "__declspec", "__cdecl", "__stdcall", "__fastcall", "__thiscall", "__vectorcall",
    "__forceinline", "__builtin_va_arg",
    "__builtin_offsetof", "__builtin_types_compatible_p", "__builtin_choose_expr", "__real__", "__imag__",
};

// The subset of the above that can open, or sit inside, a type.
const char *const kTypeKeywords[] = {
    "char", "short", "int", "long", "signed", "unsigned", "float", "double", "void", "struct", "union", "enum",
    "_Bool", "_Complex", "_Imaginary", "_Atomic", "_BitInt", "_Decimal32", "_Decimal64", "_Decimal128", "_Float16",
    "_Float32", "_Float32x", "_Float64", "_Float64x", "_Float128", "typeof", "__typeof", "__typeof__",
    "__typeof_unqual", "__int128", "__float128", "__complex__", "__signed", "__signed__",
};

/** @brief Builds the keyword lookup set once. */
const std::unordered_set<std::string> &KeywordSet() {
    static const std::unordered_set<std::string> kSet(std::begin(kKeywords), std::end(kKeywords));
    return kSet;
}

/** @brief Builds the type-keyword lookup set once. */
const std::unordered_set<std::string> &TypeKeywordSet() {
    static const std::unordered_set<std::string> kSet(std::begin(kTypeKeywords), std::end(kTypeKeywords));
    return kSet;
}

// Qualifiers and storage-class specifiers, i.e. the words that may
// appear in a declaration's specifier list without being the type.
const std::unordered_set<std::string> &QualifierSet() {
    static const std::unordered_set<std::string> kSet = {
        "const",      "volatile", "restrict",   "__restrict", "__restrict__", "__const",  "__const__",
        "__volatile", "__volatile__", "static", "extern",     "auto",         "register", "typedef",
        "inline",     "__inline", "__inline__", "_Noreturn",  "_Thread_local", "__thread", "_Atomic",
        "__extension__", "__forceinline", "__cdecl", "__stdcall", "__fastcall", "__thiscall", "__vectorcall",
    };
    return kSet;
}

// Punctuators, longest first so the scan is a plain prefix match.
const char *const kPunctuators[] = {
    "...", "<<=", ">>=", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "*=", "/=",
    "%=",  "+=",  "-=",  "&=", "^=", "|=", "##", "[",  "]",  "(",  ")",  "{",  "}",  ".",  "&",  "*",
    "+",   "-",   "~",   "!",  "/",  "%",  "<",  ">",  "^",  "|",  "?",  ":",  ";",  "=",  ",",  "#",
};

// --- Spliced source ---------------------------------------------------

// The source with backslash-newline splices removed, plus the map back
// to original byte positions. Doing it this way rather than teaching
// every scanner to step over a splice is what keeps the lexer below
// readable: `int fo\<newline>o(void)` is one identifier token whose
// `text` is "foo" and whose start and end sit on different lines, and no
// scanner has to know that happened.
struct Spliced {
    std::string text;          // the source, splices removed, lines joined by '\n'
    std::vector<int> origin;   // origin[i] = original byte offset of text[i]
    std::vector<int> line_off;  // original byte offset each 0-based line starts at

    /** @brief Converts an index into `text` to a line and byte column in the original source. */
    CPos PosOf(size_t i) const {
        const int off = i < origin.size() ? origin[i] : (origin.empty() ? 0 : origin.back() + 1);
        // Upper bound over line starts: the last line beginning at or
        // before `off` is the line `off` sits on.
        size_t lo = 0;
        size_t hi = line_off.size();
        while (lo + 1 < hi) {
            const size_t mid = (lo + hi) / 2;
            if (line_off[mid] <= off) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        CPos p;
        p.line = static_cast<int>(lo);
        p.col = off - line_off[lo];
        return p;
    }
};

/** @brief Joins the source lines and removes backslash-newline splices, recording where every byte came from. */
Spliced SpliceLines(const std::vector<std::string> &lines) {
    Spliced s;
    s.line_off.reserve(lines.size());
    // Total size up front: every byte plus one newline per line.
    size_t total = 0;
    for (const std::string &l : lines) total += l.size() + 1;
    s.text.reserve(total);
    s.origin.reserve(total);

    int off = 0;
    for (size_t li = 0; li < lines.size(); li++) {
        s.line_off.push_back(off);
        const std::string &l = lines[li];
        // A line ending in `\`, optionally followed by trailing blanks
        // (which GCC accepts with a warning), splices into the next one.
        size_t keep = l.size();
        bool splice = false;
        if (li + 1 < lines.size()) {
            size_t j = l.size();
            while (j > 0 && (l[j - 1] == ' ' || l[j - 1] == '\t' || l[j - 1] == '\r')) j--;
            if (j > 0 && l[j - 1] == '\\') {
                keep = j - 1;
                splice = true;
            }
        }
        for (size_t i = 0; i < keep; i++) {
            s.text += l[i];
            s.origin.push_back(off + static_cast<int>(i));
        }
        if (!splice) {
            s.text += '\n';
            s.origin.push_back(off + static_cast<int>(l.size()));
        }
        off += static_cast<int>(l.size()) + 1;
    }
    if (s.line_off.empty()) s.line_off.push_back(0);
    return s;
}

// --- Lexer ------------------------------------------------------------

/** @brief Reports whether an identifier is one of the string/character literal prefixes. */
bool IsLiteralPrefix(const std::string &word) {
    return word == "L" || word == "u" || word == "U" || word == "u8";
}

// One lexing run. Holds only what the scan needs; everything it produces
// is handed back through the out-parameters TokenizeC was given.
class Lexer {
public:
    Lexer(const Spliced &src, std::vector<CSyntaxError> *errors, std::vector<CComment> *comments,
          std::vector<CDirective> *directives)
        : src_(src), errors_(errors), comments_(comments), directives_(directives) {}

    /** @brief Scans the whole source, returning the token stream (End token included). */
    std::vector<CToken> Run() {
        bool at_line_start = true;
        while (i_ < src_.text.size()) {
            const char c = src_.text[i_];
            if (c == '\n') {
                i_++;
                at_line_start = true;
                if (cur_directive_ >= 0 && i_ > directive_end_) cur_directive_ = -1;
                continue;
            }
            if (IsSpace(c)) {
                i_++;
                continue;
            }
            if (c == '/' && i_ + 1 < src_.text.size() && (src_.text[i_ + 1] == '*' || src_.text[i_ + 1] == '/')) {
                ScanComment(at_line_start);
                at_line_start = false;
                continue;
            }
            if (c == '#' && at_line_start && cur_directive_ < 0) {
                // The rest of the line is lexed as ordinary tokens,
                // tagged with this directive's index, so hover and
                // completion work inside a `#define` body the same way
                // they do anywhere else -- but the `#` itself gets its
                // own kind, so nothing downstream can mistake a
                // directive for a stringize operator.
                OpenDirective();
                const size_t from = i_;
                i_++;
                Emit(CTokKind::Hash, from);
                at_line_start = false;
                continue;
            }
            at_line_start = false;
            ScanToken();
        }
        CToken end;
        end.kind = CTokKind::End;
        end.start = end.end = src_.PosOf(src_.text.size());
        tokens_.push_back(end);
        return std::move(tokens_);
    }

private:
    const Spliced &src_;
    std::vector<CSyntaxError> *errors_;
    std::vector<CComment> *comments_;
    std::vector<CDirective> *directives_;
    std::vector<CToken> tokens_;
    size_t i_ = 0;
    int cur_directive_ = -1;
    size_t directive_end_ = 0;

    /**
     * @brief Records a lexical error over a source range.
     *
     * Never inside a directive line: `#error Illegal name `=`` and
     * `#warning don't` are ordinary English, and the preprocessor never
     * tokenizes the text of either, so neither does this.
     */
    void Error(size_t from, size_t to, const char *code, const std::string &message) {
        if (errors_ == nullptr || cur_directive_ >= 0) return;
        CSyntaxError e;
        e.start = src_.PosOf(from);
        e.end = src_.PosOf(to);
        e.code = code;
        e.message = message;
        errors_->push_back(e);
    }

    /** @brief Appends a token covering [from, i_). */
    void Emit(CTokKind kind, size_t from, bool unterminated = false) {
        CToken t;
        t.kind = kind;
        t.text = src_.text.substr(from, i_ - from);
        t.start = src_.PosOf(from);
        t.end = src_.PosOf(i_);
        t.unterminated = unterminated;
        t.directive = cur_directive_;
        t.active = cur_directive_ < 0;
        tokens_.push_back(t);
    }

    /** @brief Scans a line or block comment, recording it and reporting an unterminated block. */
    void ScanComment(bool own_line) {
        const size_t from = i_;
        CComment c;
        c.own_line = own_line;
        c.block = src_.text[i_ + 1] == '*';
        const CPos start = src_.PosOf(i_);
        c.line = start.line;
        c.col = start.col;
        if (c.block) {
            i_ += 2;
            bool closed = false;
            while (i_ + 1 < src_.text.size()) {
                if (src_.text[i_] == '*' && src_.text[i_ + 1] == '/') {
                    i_ += 2;
                    closed = true;
                    break;
                }
                i_++;
            }
            if (!closed) {
                i_ = src_.text.size();
                Error(from, i_, "unterminated-comment", "Comment is never closed (missing `*/`)");
            }
        } else {
            while (i_ < src_.text.size() && src_.text[i_] != '\n') i_++;
        }
        const CPos end = src_.PosOf(i_);
        c.end_line = end.line;
        c.end_col = end.col;
        c.text = src_.text.substr(from, i_ - from);
        if (comments_ != nullptr) comments_->push_back(c);
    }

    /** @brief Scans one ordinary token: identifier, number, literal or punctuator. */
    void ScanToken() {
        const size_t from = i_;
        const char c = src_.text[i_];
        if (IsCIdentStart(static_cast<unsigned char>(c))) {
            while (i_ < src_.text.size() && IsCIdentChar(static_cast<unsigned char>(src_.text[i_]))) i_++;
            const std::string word = src_.text.substr(from, i_ - from);
            // `u8"..."`, `L'x'`: the prefix belongs to the literal, not
            // to an identifier that happens to sit in front of it.
            if (i_ < src_.text.size() && IsLiteralPrefix(word) && (src_.text[i_] == '"' || src_.text[i_] == '\'')) {
                ScanQuoted(from, src_.text[i_]);
                return;
            }
            Emit(IsCKeyword(word) ? CTokKind::Keyword : CTokKind::Ident, from);
            return;
        }
        if (IsDigit(c) || (c == '.' && i_ + 1 < src_.text.size() && IsDigit(src_.text[i_ + 1]))) {
            ScanNumber(from);
            return;
        }
        if (c == '"' || c == '\'') {
            ScanQuoted(from, c);
            return;
        }
        for (const char *p : kPunctuators) {
            const size_t n = std::strlen(p);
            if (src_.text.compare(i_, n, p) == 0) {
                i_ += n;
                Emit(CTokKind::Punct, from);
                return;
            }
        }
        // Anything left is a byte C has no token for. `$` and `@` show up
        // in generated code and in Objective-C headers; report once and
        // step over it rather than spinning.
        i_++;
        Error(from, i_, "stray-character", std::string("Stray `") + c + "` in program");
    }

    /**
     * @brief Scans a preprocessing number, which is looser than any real numeric literal.
     *
     * `1.0e+5f`, `0x1p-3`, `08`, `1and` are all one pp-number to the
     * lexer; deciding which of them is a valid constant is the parser's
     * business, not the scanner's.
     */
    void ScanNumber(size_t from) {
        i_++;
        while (i_ < src_.text.size()) {
            const char c = src_.text[i_];
            const bool exponent = (c == '+' || c == '-') && i_ > from &&
                                  (src_.text[i_ - 1] == 'e' || src_.text[i_ - 1] == 'E' || src_.text[i_ - 1] == 'p' ||
                                   src_.text[i_ - 1] == 'P');
            if (IsCIdentChar(static_cast<unsigned char>(c)) || c == '.' || exponent) {
                i_++;
                continue;
            }
            break;
        }
        Emit(CTokKind::Number, from);
    }

    /** @brief Scans a string or character literal, ending it at the line's end when the closing quote is missing. */
    void ScanQuoted(size_t from, char quote) {
        i_++;  // the opening quote
        bool closed = false;
        while (i_ < src_.text.size()) {
            const char c = src_.text[i_];
            if (c == '\n') break;
            if (c == '\\' && i_ + 1 < src_.text.size()) {
                i_ += 2;
                continue;
            }
            i_++;
            if (c == quote) {
                closed = true;
                break;
            }
        }
        if (!closed) {
            Error(from, i_, quote == '"' ? "unterminated-string" : "unterminated-char",
                  quote == '"' ? "String literal is never closed" : "Character constant is never closed");
        }
        Emit(quote == '"' ? CTokKind::String : CTokKind::CharLit, from, !closed);
    }

    /** @brief Reads the directive line starting at the current `#`, appending it to the directive list. */
    void OpenDirective() {
        if (directives_ == nullptr) return;
        const size_t nl = src_.text.find('\n', i_);
        directive_end_ = nl == std::string::npos ? src_.text.size() : nl;
        const std::string line = src_.text.substr(i_, directive_end_ - i_);
        CDirective d = ParseDirective(line, src_.PosOf(i_), src_.PosOf(directive_end_).line);
        directives_->push_back(d);
        cur_directive_ = static_cast<int>(directives_->size()) - 1;
    }

    /** @brief Splits one directive line's text into the fields CDirective carries. */
    static CDirective ParseDirective(const std::string &line, CPos at, int end_line) {
        CDirective d;
        d.line = at.line;
        d.col = at.col;
        d.end_line = end_line;
        size_t p = 1;  // past the `#`
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
        const size_t kw_start = p;
        while (p < line.size() && IsCIdentChar(static_cast<unsigned char>(line[p]))) p++;
        d.keyword = line.substr(kw_start, p - kw_start);
        d.kind = KindOfKeyword(d.keyword);
        // A `#` with no directive name but something after it is not the
        // null directive, it is a typo (`#<stdio.h>`).
        if (d.kind == CDirectiveKind::None && line.find_first_not_of(" \t", p) != std::string::npos) {
            d.kind = CDirectiveKind::Unknown;
        }
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
        d.text = line.substr(p);
        while (!d.text.empty() && (d.text.back() == ' ' || d.text.back() == '\t')) d.text.pop_back();

        switch (d.kind) {
            case CDirectiveKind::Include:
            case CDirectiveKind::Embed:
                ReadIncludeTarget(line, p, at.col, &d);
                break;
            case CDirectiveKind::Define:
                ReadDefine(line, p, at, &d);
                break;
            case CDirectiveKind::Undef:
            case CDirectiveKind::Ifdef:
            case CDirectiveKind::Ifndef:
            case CDirectiveKind::ElifDef:
                ReadName(line, p, at, &d);
                break;
            default:
                break;
        }
        return d;
    }

    /** @brief Maps a directive keyword to its kind, with `#elifndef` recorded as a negated `#elifdef`. */
    static CDirectiveKind KindOfKeyword(const std::string &kw) {
        if (kw.empty()) return CDirectiveKind::None;
        if (kw == "include" || kw == "include_next" || kw == "import") return CDirectiveKind::Include;
        if (kw == "define") return CDirectiveKind::Define;
        if (kw == "undef") return CDirectiveKind::Undef;
        if (kw == "if") return CDirectiveKind::If;
        if (kw == "ifdef") return CDirectiveKind::Ifdef;
        if (kw == "ifndef") return CDirectiveKind::Ifndef;
        if (kw == "elif") return CDirectiveKind::Elif;
        if (kw == "elifdef" || kw == "elifndef") return CDirectiveKind::ElifDef;
        if (kw == "else") return CDirectiveKind::Else;
        if (kw == "endif") return CDirectiveKind::Endif;
        if (kw == "line") return CDirectiveKind::Line;
        if (kw == "error") return CDirectiveKind::Error;
        if (kw == "warning") return CDirectiveKind::Warning;
        if (kw == "pragma") return CDirectiveKind::Pragma;
        if (kw == "embed") return CDirectiveKind::Embed;
        // A digit right after the `#` is a line marker (`# 1 "foo.h"`),
        // which preprocessed sources are full of.
        if (IsDigit(kw[0])) return CDirectiveKind::Line;
        return CDirectiveKind::Unknown;
    }

    /** @brief Reads the `<header>` or `"header"` of an `#include`. */
    static void ReadIncludeTarget(const std::string &line, size_t p, int base_col, CDirective *d) {
        if (p >= line.size()) return;
        const char open = line[p];
        if (open != '<' && open != '"') return;  // a macro-expanded include target; nothing to resolve
        const char close = open == '<' ? '>' : '"';
        const size_t end = line.find(close, p + 1);
        if (end == std::string::npos) return;
        d->angled = open == '<';
        d->header = line.substr(p + 1, end - p - 1);
        d->header_col = base_col + static_cast<int>(p);
        d->header_end_col = base_col + static_cast<int>(end) + 1;
    }

    /** @brief Reads an `#undef`/`#ifdef`-style directive's single macro name. */
    static void ReadName(const std::string &line, size_t p, CPos at, CDirective *d) {
        const size_t start = p;
        while (p < line.size() && IsCIdentChar(static_cast<unsigned char>(line[p]))) p++;
        d->name = line.substr(start, p - start);
        d->name_pos.line = at.line;
        d->name_pos.col = at.col + static_cast<int>(start);
        d->name_end_col = at.col + static_cast<int>(p);
        if (d->keyword == "elifndef") d->text = "!defined(" + d->name + ")";
    }

    /** @brief Reads a `#define`'s name, parameter list (when it is function-like) and replacement body. */
    static void ReadDefine(const std::string &line, size_t p, CPos at, CDirective *d) {
        ReadName(line, p, at, d);
        p += d->name.size();
        // Function-like only when the `(` touches the name; `#define A (x)`
        // is an object-like macro whose body happens to start with one.
        if (p < line.size() && line[p] == '(') {
            d->function_like = true;
            p++;
            std::string param;
            for (; p < line.size(); p++) {
                const char c = line[p];
                if (c == ')') {
                    p++;
                    break;
                }
                if (c == ',') {
                    if (!param.empty()) d->params.push_back(param);
                    param.clear();
                    continue;
                }
                if (c == ' ' || c == '\t') continue;
                param += c;
            }
            if (!param.empty()) d->params.push_back(param);
            if (!d->params.empty() && d->params.back() == "...") d->variadic = true;
        }
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
        d->body = line.substr(p);
        while (!d->body.empty() && (d->body.back() == ' ' || d->body.back() == '\t')) d->body.pop_back();
    }
};

}  // namespace

// --- Public lexer entry points ---------------------------------------

bool CPosLess(const CPos &a, const CPos &b) { return a.line != b.line ? a.line < b.line : a.col < b.col; }

bool IsCKeyword(const std::string &name) { return KeywordSet().count(name) > 0; }

bool IsCTypeKeyword(const std::string &name) { return TypeKeywordSet().count(name) > 0; }

bool IsCIdentStart(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}

bool IsCIdentChar(unsigned char c) { return IsCIdentStart(c) || (c >= '0' && c <= '9'); }

std::vector<CToken> TokenizeC(const std::vector<std::string> &lines, std::vector<CSyntaxError> *out_errors,
                              std::vector<CComment> *out_comments, std::vector<CDirective> *out_directives) {
    const Spliced src = SpliceLines(lines);
    Lexer lexer(src, out_errors, out_comments, out_directives);
    return lexer.Run();
}

// --- Preprocessor arm selection --------------------------------------

namespace {

// A tri-state truth value. "Unknown" is the common case and the whole
// reason this exists: `#if HAVE_SYS_TIME_H` is settled by a configure
// script this server will never see, so the honest answer is neither
// true nor false, and the arm gets chosen by shape instead (see
// PickArm).
enum class Tri { False, True, Unknown };

/** @brief Combines two operands the way `||` does, keeping Unknown only when it could still matter. */
Tri OrOf(Tri a, Tri b) {
    if (a == Tri::True || b == Tri::True) return Tri::True;
    if (a == Tri::False && b == Tri::False) return Tri::False;
    return Tri::Unknown;
}

/** @brief Combines two operands the way `&&` does. */
Tri AndOf(Tri a, Tri b) {
    if (a == Tri::False || b == Tri::False) return Tri::False;
    if (a == Tri::True && b == Tri::True) return Tri::True;
    return Tri::Unknown;
}

// What this server knows about the macro state of a file, built from the
// file's own `#define`s and `#undef`s. Nothing here comes from a header:
// a name with no event before the condition being evaluated is Unknown,
// not undefined.
//
// Order matters, and getting that wrong is how a preprocessor model
// breaks the most common construct in C. A header guard is
// `#ifndef FOO_H` followed, *inside the arm*, by `#define FOO_H`. An
// order-blind macro set says FOO_H is defined, so `!defined(FOO_H)` is
// false, so the guard's arm is dead -- and the whole header disappears.
// Only definitions that come earlier in the file than the condition
// count, which is also what a real preprocessor does.
struct MacroEvent {
    size_t at = 0;       // directive index
    bool defined = false;  // a `#define`, as opposed to an `#undef`
    std::string body;    // the replacement text of an object-like macro
    bool function_like = false;
};

struct MacroState {
    std::unordered_map<std::string, std::vector<MacroEvent>> events;
    // The directive index the condition being evaluated sits at; only
    // events before it are in scope.
    size_t limit = 0;

    /** @brief The macro event in effect at `limit` for a name, or nullptr when the file says nothing. */
    const MacroEvent *EventFor(const std::string &name) const {
        const auto it = events.find(name);
        if (it == events.end()) return nullptr;
        const MacroEvent *latest = nullptr;
        for (const MacroEvent &e : it->second) {
            if (e.at >= limit) break;
            latest = &e;
        }
        return latest;
    }
};

// The one identifier a C language server may treat as settled: it is
// compiling C, so `__cplusplus` is not defined. That single fact is what
// keeps the `extern "C" {` prologue of every public header from
// unbalancing the file's braces.
bool IsKnownUndefinedMacro(const std::string &name) { return name == "__cplusplus"; }

struct Val;
Val EvalCCondition(const std::string &text, const MacroState &macros, int depth);

// A minimal constant-expression evaluator over `#if` conditions. It
// carries a value *and* a truth, because `#if (A > 2) && B` needs the
// comparison's operands and not just their truth, and it gives up
// (Unknown) the moment an operand is a name this file does not settle.
struct Val {
    Tri truth = Tri::Unknown;
    long long number = 0;
    bool known = false;  // `number` is meaningful
};

/** @brief Wraps a known integer as an evaluator value. */
Val Known(long long n) {
    Val v;
    v.known = true;
    v.number = n;
    v.truth = n != 0 ? Tri::True : Tri::False;
    return v;
}

/** @brief The evaluator value for an operand this server cannot settle. */
Val UnknownVal() { return Val(); }

// Evaluates one `#if`/`#elif` condition. Recursive descent over the same
// precedence ladder the real preprocessor uses, minus the parts a
// condition cannot contain anyway (no assignment, no sizeof, no casts).
class CondEval {
public:
    CondEval(const std::vector<std::string> &toks, const MacroState &macros, int depth)
        : toks_(toks), macros_(macros), depth_(depth) {}

    /** @brief Evaluates the whole condition, returning Unknown when anything in it is unsettled. */
    Val Run() {
        Val v = Conditional();
        return v;
    }

private:
    const std::vector<std::string> &toks_;
    const MacroState &macros_;
    int depth_;
    size_t i_ = 0;

    /** @brief The token at the cursor, or "" past the end. */
    const std::string &Peek() const {
        static const std::string kEmpty;
        return i_ < toks_.size() ? toks_[i_] : kEmpty;
    }

    /** @brief Consumes the cursor token when it matches, reporting whether it did. */
    bool Eat(const char *s) {
        if (Peek() == s) {
            i_++;
            return true;
        }
        return false;
    }

    /** @brief `a ? b : c`, the top of the ladder. */
    Val Conditional() {
        Val cond = LogicalOr();
        if (!Eat("?")) return cond;
        const Val a = Conditional();
        Val b;
        if (Eat(":")) b = Conditional();
        if (cond.truth == Tri::True) return a;
        if (cond.truth == Tri::False) return b;
        return UnknownVal();
    }

    /** @brief `||`, short-circuiting through Unknown. */
    Val LogicalOr() {
        Val a = LogicalAnd();
        while (Eat("||")) {
            const Val b = LogicalAnd();
            Val r;
            r.truth = OrOf(a.truth, b.truth);
            r.known = r.truth != Tri::Unknown;
            r.number = r.truth == Tri::True ? 1 : 0;
            a = r;
        }
        return a;
    }

    /** @brief `&&`, short-circuiting through Unknown. */
    Val LogicalAnd() {
        Val a = BitOr();
        while (Eat("&&")) {
            const Val b = BitOr();
            Val r;
            r.truth = AndOf(a.truth, b.truth);
            r.known = r.truth != Tri::Unknown;
            r.number = r.truth == Tri::True ? 1 : 0;
            a = r;
        }
        return a;
    }

    /** @brief `|`. */
    Val BitOr() {
        Val a = BitXor();
        while (Peek() == "|") {
            i_++;
            a = Arith(a, BitXor(), '|');
        }
        return a;
    }

    /** @brief `^`. */
    Val BitXor() {
        Val a = BitAnd();
        while (Peek() == "^") {
            i_++;
            a = Arith(a, BitAnd(), '^');
        }
        return a;
    }

    /** @brief `&`. */
    Val BitAnd() {
        Val a = Equality();
        while (Peek() == "&") {
            i_++;
            a = Arith(a, Equality(), '&');
        }
        return a;
    }

    /** @brief `==` and `!=`. */
    Val Equality() {
        Val a = Relational();
        while (Peek() == "==" || Peek() == "!=") {
            const bool eq = Peek() == "==";
            i_++;
            const Val b = Relational();
            a = Compare(a, b, eq ? 'e' : 'n');
        }
        return a;
    }

    /** @brief `<`, `>`, `<=`, `>=`. */
    Val Relational() {
        Val a = Shift();
        while (Peek() == "<" || Peek() == ">" || Peek() == "<=" || Peek() == ">=") {
            const std::string op = Peek();
            i_++;
            const Val b = Shift();
            a = Compare(a, b, op == "<" ? '<' : op == ">" ? '>' : op == "<=" ? 'l' : 'g');
        }
        return a;
    }

    /** @brief `<<` and `>>`. */
    Val Shift() {
        Val a = Additive();
        while (Peek() == "<<" || Peek() == ">>") {
            const bool left = Peek() == "<<";
            i_++;
            a = Arith(a, Additive(), left ? 'L' : 'R');
        }
        return a;
    }

    /** @brief `+` and `-`. */
    Val Additive() {
        Val a = Multiplicative();
        while (Peek() == "+" || Peek() == "-") {
            const bool plus = Peek() == "+";
            i_++;
            a = Arith(a, Multiplicative(), plus ? '+' : '-');
        }
        return a;
    }

    /** @brief `*`, `/` and `%`. */
    Val Multiplicative() {
        Val a = Unary();
        while (Peek() == "*" || Peek() == "/" || Peek() == "%") {
            const char op = Peek()[0];
            i_++;
            a = Arith(a, Unary(), op);
        }
        return a;
    }

    /** @brief `!`, `~`, unary `-` and `+`. */
    Val Unary() {
        if (Eat("!")) {
            const Val v = Unary();
            Val r;
            r.truth = v.truth == Tri::True ? Tri::False : v.truth == Tri::False ? Tri::True : Tri::Unknown;
            r.known = r.truth != Tri::Unknown;
            r.number = r.truth == Tri::True ? 1 : 0;
            return r;
        }
        if (Eat("-")) {
            const Val v = Unary();
            return v.known ? Known(-v.number) : UnknownVal();
        }
        if (Eat("+")) return Unary();
        if (Eat("~")) {
            const Val v = Unary();
            return v.known ? Known(~v.number) : UnknownVal();
        }
        return Primary();
    }

    /** @brief A literal, a `defined(...)`, a parenthesized expression, or an identifier this file may settle. */
    Val Primary() {
        if (Eat("(")) {
            const Val v = Conditional();
            Eat(")");
            return v;
        }
        const std::string tok = Peek();
        if (tok.empty()) return UnknownVal();
        i_++;
        if (tok == "defined") {
            const bool paren = Eat("(");
            const std::string name = Peek();
            if (!name.empty()) i_++;
            if (paren) Eat(")");
            if (IsKnownUndefinedMacro(name)) return Known(0);
            const MacroEvent *event = macros_.EventFor(name);
            if (event == nullptr) return UnknownVal();
            return Known(event->defined ? 1 : 0);
        }
        if (IsDigit(tok[0])) return Known(ParseIntLiteral(tok));
        if (tok[0] == '\'') return Known(ParseCharLiteral(tok));
        if (IsCIdentStart(static_cast<unsigned char>(tok[0]))) {
            if (IsKnownUndefinedMacro(tok)) return Known(0);
            // An object-like macro this file defines can be substituted,
            // once, to a bounded depth. `#define VERSION 3` followed by
            // `#if VERSION > 2` is common enough to be worth it.
            const MacroEvent *event = macros_.EventFor(tok);
            if (event != nullptr && event->defined && !event->function_like && depth_ < 8) {
                return EvalCCondition(event->body, macros_, depth_ + 1);
            }
            // A name a header might define. Unknown, never zero: guessing
            // zero here is exactly how a server ends up parsing the arm a
            // compiler never sees.
            return UnknownVal();
        }
        return UnknownVal();
    }

    /** @brief Applies a binary arithmetic or bitwise operator, propagating Unknown. */
    static Val Arith(const Val &a, const Val &b, char op) {
        if (!a.known || !b.known) return UnknownVal();
        switch (op) {
            case '+': return Known(a.number + b.number);
            case '-': return Known(a.number - b.number);
            case '*': return Known(a.number * b.number);
            case '/': return b.number == 0 ? UnknownVal() : Known(a.number / b.number);
            case '%': return b.number == 0 ? UnknownVal() : Known(a.number % b.number);
            case '&': return Known(a.number & b.number);
            case '|': return Known(a.number | b.number);
            case '^': return Known(a.number ^ b.number);
            case 'L': return b.number < 0 || b.number > 63 ? UnknownVal() : Known(a.number << b.number);
            case 'R': return b.number < 0 || b.number > 63 ? UnknownVal() : Known(a.number >> b.number);
            default: return UnknownVal();
        }
    }

    /** @brief Applies a comparison operator, propagating Unknown. */
    static Val Compare(const Val &a, const Val &b, char op) {
        if (!a.known || !b.known) return UnknownVal();
        switch (op) {
            case 'e': return Known(a.number == b.number ? 1 : 0);
            case 'n': return Known(a.number != b.number ? 1 : 0);
            case '<': return Known(a.number < b.number ? 1 : 0);
            case '>': return Known(a.number > b.number ? 1 : 0);
            case 'l': return Known(a.number <= b.number ? 1 : 0);
            case 'g': return Known(a.number >= b.number ? 1 : 0);
            default: return UnknownVal();
        }
    }

    /** @brief Reads an integer constant in any C base, ignoring its suffix. */
    static long long ParseIntLiteral(const std::string &tok) {
        return std::strtoll(tok.c_str(), nullptr, 0);
    }

    /** @brief Reads a character constant's value, handling the escapes a condition realistically contains. */
    static long long ParseCharLiteral(const std::string &tok) {
        if (tok.size() < 3) return 0;
        if (tok[1] != '\\') return static_cast<unsigned char>(tok[1]);
        switch (tok[2]) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case '0': return 0;
            case '\\': return '\\';
            case '\'': return '\'';
            default: return static_cast<unsigned char>(tok[2]);
        }
    }

};

/** @brief Splits a condition's text into the coarse tokens CondEval walks. */
std::vector<std::string> SplitCondTokens(const std::string &text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (IsSpace(c)) {
            i++;
            continue;
        }
        if (IsCIdentStart(static_cast<unsigned char>(c))) {
            const size_t from = i;
            while (i < text.size() && IsCIdentChar(static_cast<unsigned char>(text[i]))) i++;
            out.push_back(text.substr(from, i - from));
            continue;
        }
        if (IsDigit(c)) {
            const size_t from = i;
            while (i < text.size() && (IsCIdentChar(static_cast<unsigned char>(text[i])) || text[i] == '.')) i++;
            out.push_back(text.substr(from, i - from));
            continue;
        }
        if (c == '\'') {
            const size_t from = i;
            i++;
            while (i < text.size() && text[i] != '\'') {
                i += text[i] == '\\' ? 2u : 1u;
            }
            if (i < text.size()) i++;
            out.push_back(text.substr(from, i - from));
            continue;
        }
        bool matched = false;
        for (const char *p : kPunctuators) {
            const size_t n = std::strlen(p);
            if (text.compare(i, n, p) == 0) {
                out.push_back(p);
                i += n;
                matched = true;
                break;
            }
        }
        if (!matched) i++;
    }
    return out;
}

/** @brief Evaluates a `#if` condition's text against what this file says about its own macros. */
Val EvalCCondition(const std::string &text, const MacroState &macros, int depth) {
    const std::vector<std::string> toks = SplitCondTokens(text);
    if (toks.empty()) return UnknownVal();
    CondEval eval(toks, macros, depth);
    return eval.Run();
}

// One arm of a conditional group, as arm selection sees it: which
// directive opened it, and which token range it owns.
struct Arm {
    int directive = -1;  // the `#if`/`#elif`/`#else` that opens this arm
    size_t first = 0;    // first token index in the arm
    size_t last = 0;     // one past the arm's last token
    Tri truth = Tri::Unknown;
    bool balanced = false;
};

/** @brief Reports whether a token range opens and closes every bracket it touches. */
bool IsBalanced(const std::vector<CToken> &tokens, size_t first, size_t last) {
    int depth = 0;
    for (size_t i = first; i < last && i < tokens.size(); i++) {
        const CToken &t = tokens[i];
        if (!t.active || t.directive >= 0) continue;
        if (t.kind != CTokKind::Punct) continue;
        if (t.text == "{" || t.text == "(" || t.text == "[") depth++;
        if (t.text == "}" || t.text == ")" || t.text == "]") depth--;
        if (depth < 0) return false;
    }
    return depth == 0;
}

}  // namespace

void SelectCPreprocessorArms(std::vector<CToken> &tokens, std::vector<CDirective> &directives,
                             std::vector<CSyntaxError> *out_errors) {
    if (directives.empty()) return;

    // Every `#define` and `#undef` the file contains, in order, and
    // deliberately blind to which arm each sits in: which arm is taken
    // depends on what is defined, which depends on which arm is taken,
    // and that circle has to be cut somewhere. Order is what makes the
    // over-approximation safe -- see MacroState.
    MacroState macros;
    for (size_t i = 0; i < directives.size(); i++) {
        const CDirective &d = directives[i];
        const bool define = d.kind == CDirectiveKind::Define;
        if ((!define && d.kind != CDirectiveKind::Undef) || d.name.empty()) continue;
        MacroEvent event;
        event.at = i;
        event.defined = define;
        event.body = d.body;
        event.function_like = d.function_like;
        macros.events[d.name].push_back(event);
    }

    // Token index of the first and last token belonging to each
    // directive, so an arm's token range can be cut out of the stream.
    const size_t n = directives.size();
    std::vector<size_t> first_tok(n, tokens.size());
    std::vector<size_t> end_tok(n, 0);
    for (size_t i = 0; i < tokens.size(); i++) {
        const int d = tokens[i].directive;
        if (d < 0) continue;
        const size_t du = static_cast<size_t>(d);
        if (du >= n) continue;
        first_tok[du] = std::min(first_tok[du], i);
        end_tok[du] = std::max(end_tok[du], i + 1);
    }

    // A group being collected, innermost last.
    struct Group {
        std::vector<Arm> arms;
        bool has_else = false;
    };
    std::vector<Group> stack;

    /** Closes the innermost group at `endif_index`, choosing and applying one arm. */
    const auto close_group = [&](size_t endif_index) {
        Group g = stack.back();
        stack.pop_back();
        // Each arm runs from just past its own directive line to the
        // start of the next arm's directive (or the `#endif`).
        for (size_t k = 0; k < g.arms.size(); k++) {
            const size_t open = static_cast<size_t>(g.arms[k].directive);
            g.arms[k].first = end_tok[open];
            const size_t next = k + 1 < g.arms.size() ? static_cast<size_t>(g.arms[k + 1].directive) : endif_index;
            g.arms[k].last = first_tok[next];
            g.arms[k].balanced = IsBalanced(tokens, g.arms[k].first, g.arms[k].last);
        }

        // Which arm a compiler would take, as far as this file settles
        // it: the first arm that is not definitively false. Arms before
        // it are dead for certain (`#if 0`), so they never compete.
        int chosen = -1;
        size_t start = 0;
        while (start < g.arms.size() && g.arms[start].truth == Tri::False) start++;
        if (start < g.arms.size() && g.arms[start].truth == Tri::True) {
            chosen = static_cast<int>(start);
        } else if (start < g.arms.size()) {
            // Undecidable. A group with a written `#else` offers a real
            // choice between two spellings of the same thing, and the
            // first one is as good a guess as any -- taking the other
            // because it happens to balance would read the arm a
            // compiler took only half the time.
            //
            // A group with *no* `#else` is different: its one arm is
            // something the file adds under a condition, and whether it
            // balances says which reading leaves the rest of the file
            // intact. That is what makes a lone `#ifdef __cplusplus`
            // wrapping `extern "C" {` contribute nothing instead of one
            // stray brace, since the implicit empty else balances and
            // the arm does not.
            if (g.has_else) {
                chosen = static_cast<int>(start);
            } else {
                for (size_t k = start; k < g.arms.size(); k++) {
                    if (g.arms[k].truth != Tri::False && g.arms[k].balanced) {
                        chosen = static_cast<int>(k);
                        break;
                    }
                }
            }
        }

        for (size_t k = 0; k < g.arms.size(); k++) {
            const bool take = static_cast<int>(k) == chosen;
            const size_t open = static_cast<size_t>(g.arms[k].directive);
            directives[open].taken = take;
            directives[open].group_start = g.arms.front().directive;
            directives[open].matching_end = static_cast<int>(endif_index);
            if (take) continue;
            for (size_t t = g.arms[k].first; t < g.arms[k].last && t < tokens.size(); t++) tokens[t].active = false;
            // Every directive between this arm's opener and the next
            // one is inside it, nested groups included. Marking them by
            // index rather than by the code token that follows them is
            // what makes a `#define` at the very end of a dead arm come
            // out dead.
            const size_t next = k + 1 < g.arms.size() ? static_cast<size_t>(g.arms[k + 1].directive) : endif_index;
            for (size_t j = open + 1; j < next && j < n; j++) directives[j].active = false;
        }
        directives[endif_index].group_start = g.arms.front().directive;
        directives[endif_index].matching_end = static_cast<int>(endif_index);
    };

    for (size_t i = 0; i < n; i++) {
        CDirective &d = directives[i];
        switch (d.kind) {
            case CDirectiveKind::If:
            case CDirectiveKind::Ifdef:
            case CDirectiveKind::Ifndef: {
                Arm arm;
                arm.directive = static_cast<int>(i);
                std::string cond = d.text;
                if (d.kind == CDirectiveKind::Ifdef) cond = "defined(" + d.name + ")";
                if (d.kind == CDirectiveKind::Ifndef) cond = "!defined(" + d.name + ")";
                macros.limit = i;
                arm.truth = EvalCCondition(cond, macros, 0).truth;
                Group g;
                g.arms.push_back(arm);
                stack.push_back(g);
                break;
            }
            case CDirectiveKind::Elif:
            case CDirectiveKind::ElifDef: {
                if (stack.empty()) {
                    if (out_errors != nullptr) {
                        CSyntaxError e;
                        e.start = {d.line, d.col};
                        e.end = {d.line, d.col + 1 + static_cast<int>(d.keyword.size())};
                        e.code = "pp-stray-elif";
                        e.message = "`#" + d.keyword + "` without a matching `#if`";
                        out_errors->push_back(e);
                    }
                    break;
                }
                Arm arm;
                arm.directive = static_cast<int>(i);
                std::string cond = d.text;
                if (d.kind == CDirectiveKind::ElifDef && d.keyword == "elifdef") cond = "defined(" + d.name + ")";
                macros.limit = i;
                arm.truth = EvalCCondition(cond, macros, 0).truth;
                stack.back().arms.push_back(arm);
                break;
            }
            case CDirectiveKind::Else: {
                if (stack.empty()) {
                    if (out_errors != nullptr) {
                        CSyntaxError e;
                        e.start = {d.line, d.col};
                        e.end = {d.line, d.col + 5};
                        e.code = "pp-stray-else";
                        e.message = "`#else` without a matching `#if`";
                        out_errors->push_back(e);
                    }
                    break;
                }
                if (stack.back().has_else && out_errors != nullptr) {
                    CSyntaxError e;
                    e.start = {d.line, d.col};
                    e.end = {d.line, d.col + 5};
                    e.code = "pp-duplicate-else";
                    e.message = "This conditional already has an `#else`";
                    out_errors->push_back(e);
                }
                Arm arm;
                arm.directive = static_cast<int>(i);
                // An `#else` is true exactly when every arm before it is
                // false, which is only settled when all of them are.
                bool all_false = true;
                for (const Arm &a : stack.back().arms) {
                    if (a.truth != Tri::False) all_false = false;
                }
                arm.truth = all_false ? Tri::True : Tri::Unknown;
                stack.back().arms.push_back(arm);
                stack.back().has_else = true;
                break;
            }
            case CDirectiveKind::Endif: {
                if (stack.empty()) {
                    if (out_errors != nullptr) {
                        CSyntaxError e;
                        e.start = {d.line, d.col};
                        e.end = {d.line, d.col + 6};
                        e.code = "pp-stray-endif";
                        e.message = "`#endif` without a matching `#if`";
                        out_errors->push_back(e);
                    }
                    break;
                }
                close_group(i);
                break;
            }
            default:
                break;
        }
    }

    // A group still open at end of file: report it on its `#if` (where
    // the reader can act on it) and take its first arm, so the file
    // still parses into something.
    while (!stack.empty()) {
        const Group g = stack.back();
        const CDirective &open = directives[static_cast<size_t>(g.arms.front().directive)];
        if (out_errors != nullptr) {
            CSyntaxError e;
            e.start = {open.line, open.col};
            e.end = {open.line, open.col + 1 + static_cast<int>(open.keyword.size())};
            e.code = "pp-unterminated-if";
            e.message = "`#" + open.keyword + "` is never closed by an `#endif`";
            out_errors->push_back(e);
        }
        stack.pop_back();
    }

}

// --- Parser -----------------------------------------------------------

namespace {

// How deep the recursive descent may go before it gives up. A file full
// of `((((((...` is not something anyone typed on purpose, but it is
// something an editor is asked to parse while someone is typing, and the
// only acceptable answer to it is a diagnostic rather than a crashed
// language server.
constexpr int kMaxDepth = 150;

// At most this many syntax errors are reported for one file. Past that
// the file is not "a program with mistakes in it" any more -- it is a
// different language, a binary, or a paste that landed sideways -- and
// the hundredth report helps nobody.
constexpr size_t kMaxErrors = 200;

// Everything a declaration's specifier list contributed: the type it
// names, its storage class and qualifier flags, and (for
// `struct S { int x; } s;`) the tag definition itself, which is a real
// declaration in its own right and has to survive into the tree.
struct DeclSpec {
    CType type;
    unsigned flags = 0;
    CNodePtr tag;      // RecordDecl or EnumDecl defined inline, or null
    bool saw_type = false;
    // Whether a `MACRO(...)` group was already read as the type. Only
    // one per declaration: in `RTDECL(void) RTMemTmpFree(void *pv)` the
    // second group is the parameter list, and reading it as a second
    // macro type leaves the trailing `RT_NO_THROW_DEF` as the declared
    // name.
    bool saw_macro_group = false;
    bool empty = true;  // nothing at all was consumed
    CPos start;
    CPos end;
};

// What one declarator parsed to. The type is the declared type,
// shallowly (see CType); `params` is filled only for a declarator whose
// outermost suffix is a parameter list, which is what makes
// `int f(void) { ... }` a function definition and `int (*f)(void) = g;`
// an initialized pointer.
struct DeclaratorInfo {
    std::string name;
    CPos name_pos;
    int name_end_col = 0;
    CType type;
    std::vector<CNodePtr> params;
    bool top_function = false;  // `name(params)`, not `(*name)(params)`
    bool kandr = false;         // the parameter list was an identifier list
    unsigned flags = 0;         // attribute flags found on this declarator
    CPos start;
    CPos end;
};

// The recursive-descent parser. Walks only the tokens
// SelectCPreprocessorArms left active, keeps its own typedef-name table
// (there is no other way to read `A * b;`), and recovers at statement
// and declaration boundaries rather than stopping at the first mistake.
class Parser {
public:
    Parser(const std::vector<CToken> &tokens, std::vector<CSyntaxError> *errors) : all_(tokens), errors_(errors) {
        idx_.reserve(tokens.size());
        for (size_t i = 0; i < tokens.size(); i++) {
            const CToken &t = tokens[i];
            if (t.kind == CTokKind::End) continue;
            if (t.directive >= 0 || !t.active) continue;
            idx_.push_back(i);
        }
        end_pos_ = tokens.empty() ? CPos() : tokens.back().start;
    }

    /** @brief Parses the whole translation unit, always returning a node. */
    CNodePtr Run() {
        CNodePtr unit = Make(CNodeKind::TranslationUnit, Pos());
        while (!AtEnd()) {
            const size_t before = p_;
            CNodePtr decl = ParseExternalDeclaration();
            if (decl) unit->body.push_back(std::move(decl));
            if (p_ == before) p_++;  // never spin on a token nothing accepted
        }
        unit->end = end_pos_;
        return unit;
    }

    /** @brief The typedef names this file introduced, in declaration order. */
    const std::vector<std::string> &typedef_names() const { return typedef_order_; }

private:
    const std::vector<CToken> &all_;
    std::vector<CSyntaxError> *errors_;
    std::vector<size_t> idx_;  // indices into all_ of the tokens the parser walks
    size_t p_ = 0;
    int depth_ = 0;
    int last_error_line_ = -1;
    CPos end_pos_;
    std::unordered_set<std::string> typedefs_;
    std::vector<std::string> typedef_order_;

    // --- Token access -------------------------------------------------

    /** @brief Reports whether every token has been consumed. */
    bool AtEnd() const { return p_ >= idx_.size(); }

    /** @brief The token `k` places ahead of the cursor, or a synthetic End token past the last one. */
    const CToken &At(size_t k) const {
        static const CToken kEnd;
        const size_t q = p_ + k;
        return q < idx_.size() ? all_[idx_[q]] : kEnd;
    }

    /** @brief The token at the cursor. */
    const CToken &Cur() const { return At(0); }

    /** @brief The cursor token's start position, or the end of the file. */
    CPos Pos() const { return AtEnd() ? end_pos_ : Cur().start; }

    /** @brief The end position of the token just consumed. */
    CPos PrevEnd() const { return p_ == 0 ? Pos() : all_[idx_[p_ - 1]].end; }

    /** @brief Reports whether the token `k` ahead is a punctuator spelled `s`. */
    bool IsPunct(const char *s, size_t k = 0) const {
        const CToken &t = At(k);
        return t.kind == CTokKind::Punct && t.text == s;
    }

    /** @brief Reports whether the token `k` ahead is the keyword `s`. */
    bool IsKw(const char *s, size_t k = 0) const {
        const CToken &t = At(k);
        return t.kind == CTokKind::Keyword && t.text == s;
    }

    /** @brief Consumes a punctuator when it is at the cursor, reporting whether it was. */
    bool EatPunct(const char *s) {
        if (!IsPunct(s)) return false;
        p_++;
        return true;
    }

    /** @brief Consumes a keyword when it is at the cursor, reporting whether it was. */
    bool EatKw(const char *s) {
        if (!IsKw(s)) return false;
        p_++;
        return true;
    }

    /** @brief Consumes the expected punctuator, or reports a diagnostic naming it. */
    bool Expect(const char *s, const char *what) {
        if (EatPunct(s)) return true;
        Error(Pos(), Pos(), "expected-token", std::string("Expected `") + s + "` " + what);
        return false;
    }

    // --- Diagnostics --------------------------------------------------

    /**
     * @brief Records a syntax error, at most one per source line.
     *
     * One per line on purpose: a missing `;` makes the next three
     * constructs unreadable too, and three reports for one typo buries
     * the one the reader can act on. CPython's tokenizer takes the same
     * position for the same reason (python_ast.cpp says so at length).
     */
    void Error(const CPos &start, const CPos &end, const char *code, const std::string &message) {
        if (errors_ == nullptr || errors_->size() >= kMaxErrors) return;
        if (start.line == last_error_line_) return;
        last_error_line_ = start.line;
        CSyntaxError e;
        e.start = start;
        e.end = CPosLess(start, end) ? end : CPos{start.line, start.col + 1};
        e.code = code;
        e.message = message;
        errors_->push_back(e);
    }

    /** @brief Reports whether the error budget for this file is used up. */
    bool ErrorBudgetSpent() const { return errors_ != nullptr && errors_->size() >= kMaxErrors; }

    // --- Nodes --------------------------------------------------------

    /** @brief Allocates a node of a kind, starting at a position. */
    static CNodePtr Make(CNodeKind kind, const CPos &start) {
        CNodePtr n = std::make_unique<CNode>();
        n->kind = kind;
        n->start = start;
        n->end = start;
        return n;
    }

    /** @brief Closes a node at the end of the last consumed token. */
    CNodePtr Finish(CNodePtr n) {
        n->end = PrevEnd();
        return n;
    }

    /** @brief Allocates the node an unreadable construct leaves behind. */
    CNodePtr Placeholder(const CPos &start) {
        CNodePtr n = Make(CNodeKind::Placeholder, start);
        n->end = Pos();
        return n;
    }

    // --- Recovery -----------------------------------------------------

    /**
     * @brief Skips forward to the end of the construct the cursor is lost in.
     *
     * Stops after a `;` at the current nesting level, or before a `}`
     * that closes the enclosing block, which are the two places a C
     * reader also picks the thread back up.
     */
    void SyncToStatementEnd() {
        int depth = 0;
        while (!AtEnd()) {
            const CToken &t = Cur();
            if (t.kind == CTokKind::Punct) {
                if (t.text == "{" || t.text == "(" || t.text == "[") depth++;
                if (t.text == ")" || t.text == "]") depth--;
                if (t.text == "}") {
                    if (depth == 0) return;
                    depth--;
                    p_++;
                    continue;
                }
                if (t.text == ";" && depth <= 0) {
                    p_++;
                    return;
                }
            }
            p_++;
        }
    }

    /** @brief Skips a balanced bracket group starting at the cursor's opening bracket. */
    void SkipBalanced() {
        if (AtEnd()) return;
        const std::string open = Cur().text;
        const char *close = open == "(" ? ")" : open == "[" ? "]" : open == "{" ? "}" : nullptr;
        if (close == nullptr) {
            p_++;
            return;
        }
        int depth = 0;
        while (!AtEnd()) {
            const CToken &t = Cur();
            if (t.kind == CTokKind::Punct) {
                if (t.text == open) depth++;
                if (t.text == close) {
                    depth--;
                    if (depth == 0) {
                        p_++;
                        return;
                    }
                }
            }
            p_++;
        }
    }

    // --- Attributes ---------------------------------------------------

    /**
     * @brief Consumes any run of GNU/C23 attributes, `asm` labels and `__extension__` markers.
     *
     * These can appear almost anywhere a declaration can, they carry
     * nothing this server reasons about except `unused`, and refusing to
     * skip them turns every line of real system-header-shaped C into a
     * syntax error.
     */
    unsigned SkipAttributes() {
        unsigned flags = 0;
        for (;;) {
            if (IsKw("__attribute__") || IsKw("__attribute") || IsKw("__declspec")) {
                p_++;
                if (IsPunct("(")) {
                    flags |= AttributeFlagsIn(p_);
                    SkipBalanced();
                }
                continue;
            }
            if (IsKw("__extension__") || IsKw("__forceinline") || IsKw("__cdecl") || IsKw("__stdcall") ||
                IsKw("__fastcall") || IsKw("__thiscall") || IsKw("__vectorcall")) {
                p_++;
                continue;
            }
            // C23 `[[nodiscard]]`. Only a doubled `[` can start one, and
            // a doubled `[` is not valid as two subscripts, so there is
            // no ambiguity with an array declarator.
            if (IsPunct("[") && IsPunct("[", 1)) {
                flags |= AttributeFlagsIn(p_ + 1);
                SkipBalanced();
                continue;
            }
            // An `asm("name")` label on a declarator, which glibc's
            // headers put on nearly every function.
            if ((IsKw("asm") || IsKw("__asm") || IsKw("__asm__")) && IsPunct("(", 1)) {
                p_++;
                SkipBalanced();
                // An assembler label means something outside C names
                // this object, so nothing here may call it unused.
                flags |= kCFlagAttrUnused;
                continue;
            }
            return flags;
        }
    }

    /** @brief Scans an attribute group's contents for the few attribute names this server acts on. */
    unsigned AttributeFlagsIn(size_t open) const {
        unsigned flags = 0;
        int depth = 0;
        for (size_t q = open; q < idx_.size(); q++) {
            const CToken &t = all_[idx_[q]];
            if (t.kind == CTokKind::Punct) {
                if (t.text == "(" || t.text == "[") depth++;
                if (t.text == ")" || t.text == "]") {
                    depth--;
                    if (depth <= 0) break;
                }
                continue;
            }
            if (t.kind != CTokKind::Ident && t.kind != CTokKind::Keyword) continue;
            const std::string &w = t.text;
            if (w == "unused" || w == "__unused__" || w == "used" || w == "__used__" || w == "cleanup" ||
                w == "__cleanup__" || w == "constructor" || w == "__constructor__" || w == "destructor" ||
                w == "__destructor__" || w == "alias" || w == "__alias__" || w == "weak" || w == "__weak__" ||
                w == "maybe_unused" || w == "section" || w == "__section__") {
                flags |= kCFlagAttrUnused;
            }
            if (w == "fallthrough" || w == "__fallthrough__") flags |= kCFlagAttrFallthrough;
        }
        return flags;
    }

    // --- Type helpers -------------------------------------------------

    /** @brief Reports whether a name has been introduced as a typedef earlier in this file. */
    bool IsTypedefName(const std::string &name) const { return typedefs_.count(name) > 0; }

    /** @brief Records a typedef name, so later declarations can read `A * b;` as a declaration. */
    void AddTypedef(const std::string &name) {
        if (name.empty()) return;
        if (typedefs_.insert(name).second) typedef_order_.push_back(name);
    }

    /** @brief Reports whether the token `k` ahead can appear in a declaration's specifier list. */
    bool IsSpecifierToken(size_t k) const {
        const CToken &t = At(k);
        if (t.kind != CTokKind::Keyword) return false;
        return IsCTypeKeyword(t.text) || QualifierSet().count(t.text) > 0 || t.text == "_Alignas" ||
               t.text == "__attribute__" || t.text == "__attribute" || t.text == "__declspec" ||
               t.text == "_Noreturn" || t.text == "_Static_assert";
    }

    /**
     * @brief Reports whether what follows the cursor is a declaration rather than an expression.
     *
     * The one genuinely ambiguous case in C: `A * b;` declares `b` when
     * `A` is a type and multiplies when it is not. When `A` is a typedef
     * this file introduced, that is settled. When it is not -- because it
     * came from a header this server cannot see -- the declaration
     * reading wins anyway, on the grounds that a statement whose whole
     * effect is to multiply two values and discard the result is not
     * something anyone writes, while `size_t *p;` is on every other page.
     */
    bool LooksLikeDeclaration() const {
        if (IsKw("typedef") || IsKw("static") || IsKw("extern") || IsKw("register") || IsKw("auto") ||
            IsKw("_Thread_local") || IsKw("__thread") || IsKw("_Static_assert") || IsKw("inline") ||
            IsKw("__inline") || IsKw("__inline__") || IsKw("_Noreturn") || IsKw("__extension__")) {
            return true;
        }
        if (Cur().kind == CTokKind::Keyword &&
            (IsCTypeKeyword(Cur().text) || QualifierSet().count(Cur().text) > 0)) {
            // `sizeof(int)` and `(struct s *)p` never start a statement,
            // so a type keyword here is a declaration.
            return true;
        }
        if (IsPunct("[") && IsPunct("[", 1)) return true;  // a C23 attribute on a declaration
        if (Cur().kind != CTokKind::Ident) return false;
        // `g_autoptr(GSettings) settings = NULL;` -- a macro that
        // expands to a type, which ParseDeclSpecifiers knows how to read.
        if (IsPunct("(", 1) && MacroTypeEndsAt(p_ + 1)) return true;
        if (IsTypedefName(Cur().text)) {
            // `A;` and `A(x);` are still expressions even when A is a
            // type name, because someone shadowed it with a variable.
            return !IsPunct(";", 1) && !IsPunct("(", 1) && !IsPunct("=", 1) && !IsPunct(".", 1) &&
                   !IsPunct("->", 1) && !IsPunct("[", 1) && !IsPunct(",", 1);
        }
        // An unknown leading identifier: a declaration only if what
        // follows can only be a declarator. `FOO *bar;` and `FOO bar;`
        // qualify; `foo(x);` and `foo->x = 1;` do not. The leading run
        // may be several words long, because macros expand into type
        // position: `g_autofree gchar *dir = ...` is three of them.
        size_t k = 0;
        while ((At(k).kind == CTokKind::Ident && ExtendsTypeWords(k + 1)) ||
               (At(k).kind == CTokKind::Keyword && QualifierSet().count(At(k).text) > 0)) {
            k++;
        }
        if (k == 0) k = 1;
        size_t stars = 0;
        while (IsPunct("*", k)) {
            k++;
            stars++;
            while (At(k).kind == CTokKind::Keyword && QualifierSet().count(At(k).text) > 0) k++;
        }
        if (At(k).kind != CTokKind::Ident) return false;
        const CToken &after = At(k + 1);
        // `DWORD len __attribute__((unused));` -- an attribute right
        // after the declared name.
        if (after.kind == CTokKind::Keyword) {
            return after.text == "__attribute__" || after.text == "__attribute" || after.text == "__asm__" ||
                   QualifierSet().count(after.text) > 0;
        }
        if (after.kind != CTokKind::Punct) return false;
        if (after.text == ";" || after.text == "," || after.text == "=" || after.text == "[") return true;
        // `FOO bar(void);` -- a prototype inside a block, which is rare
        // but legal; only accept it when there was no `*`, since
        // `a * b(c)` is a plausible expression.
        return stars == 0 && after.text == "(";
    }

    /** @brief Reports whether the token `k` ahead continues a run of type words rather than being the declarator. */
    bool ExtendsTypeWords(size_t k) const {
        const CToken &t = At(k);
        if (t.kind == CTokKind::Ident) return true;
        if (t.kind == CTokKind::Punct) return t.text == "*";
        return t.kind == CTokKind::Keyword && (IsCTypeKeyword(t.text) || QualifierSet().count(t.text) > 0);
    }

    // --- Declaration specifiers ---------------------------------------

    /** @brief Parses a declaration's specifier list (storage class, qualifiers, type, inline tag definition). */
    DeclSpec ParseDeclSpecifiers() {
        DeclSpec spec;
        spec.start = Pos();
        std::string spelling;
        const auto add = [&spelling](const std::string &w) {
            if (!spelling.empty()) spelling += ' ';
            spelling += w;
        };
        for (;;) {
            const unsigned attr = SkipAttributes();
            if (attr != 0) {
                spec.flags |= attr;
                spec.empty = false;
                continue;
            }
            const CToken &t = Cur();
            if (t.kind == CTokKind::Keyword) {
                const std::string &w = t.text;
                if (w == "typedef") spec.flags |= kCFlagTypedef;
                if (w == "static") spec.flags |= kCFlagStatic;
                if (w == "extern") spec.flags |= kCFlagExtern;
                if (w == "register") spec.flags |= kCFlagRegister;
                if (w == "inline" || w == "__inline" || w == "__inline__") spec.flags |= kCFlagInline;
                if (w == "_Noreturn") spec.flags |= kCFlagNoreturn;
                if (w == "_Thread_local" || w == "__thread") spec.flags |= kCFlagThreadLocal;
                if (w == "const" || w == "__const" || w == "__const__") spec.flags |= kCFlagConst;
                if (w == "volatile" || w == "__volatile" || w == "__volatile__") spec.flags |= kCFlagVolatile;
                if (w == "_Alignas") {
                    p_++;
                    if (IsPunct("(")) SkipBalanced();
                    spec.empty = false;
                    continue;
                }
                if (w == "struct" || w == "union") {
                    if (spec.saw_type) break;
                    CNodePtr rec = ParseRecord();
                    spec.type.base = rec->str_value + (rec->name.empty() ? "" : " " + rec->name);
                    spec.type.is_struct = true;
                    spec.saw_type = true;
                    spec.empty = false;
                    add(spec.type.base);
                    if (!rec->body.empty() || rec->name.empty()) spec.tag = std::move(rec);
                    continue;
                }
                if (w == "enum") {
                    if (spec.saw_type) break;
                    CNodePtr en = ParseEnum();
                    spec.type.base = "enum" + (en->name.empty() ? std::string() : " " + en->name);
                    spec.type.is_struct = true;
                    spec.saw_type = true;
                    spec.empty = false;
                    add(spec.type.base);
                    spec.tag = std::move(en);
                    continue;
                }
                if (w == "typeof" || w == "__typeof" || w == "__typeof__" || w == "__typeof_unqual") {
                    if (spec.saw_type) break;
                    p_++;
                    if (IsPunct("(")) SkipBalanced();
                    spec.saw_type = true;
                    spec.empty = false;
                    add("typeof");
                    continue;
                }
                if (w == "_Atomic" && IsPunct("(", 1)) {
                    // `_Atomic(int) x;` -- the type is inside the parens.
                    p_++;
                    SkipBalanced();
                    spec.saw_type = true;
                    spec.empty = false;
                    add("_Atomic");
                    continue;
                }
                if (IsCTypeKeyword(w)) {
                    // `long long`, `unsigned char`: a second type keyword
                    // extends the first rather than ending the list.
                    spec.saw_type = true;
                    spec.type.base = spec.type.base.empty() ? w : spec.type.base + " " + w;
                    spec.empty = false;
                    add(w);
                    p_++;
                    continue;
                }
                if (QualifierSet().count(w) > 0) {
                    spec.empty = false;
                    add(w);
                    p_++;
                    continue;
                }
                break;
            }
            // Only before a type is settled. Once `int` has been read,
            // the next `IDENT(` is the declarator -- `int f(void)
            // __THROW;` is a prototype with an attribute macro after it,
            // not a macro type called `f`, and there is no shape that
            // tells those two apart.
            if (t.kind == CTokKind::Ident && !spec.saw_type && !spec.saw_macro_group && IsPunct("(", 1) &&
                MacroTypeEndsAt(p_ + 1)) {
                // A macro that expands to a type, with arguments:
                // `RTDECL(int) f(void)`, `const ElfW(Phdr) *p`. Only the
                // preprocessor can know what it produced, but the shape
                // says where it ends -- a declarator name follows the
                // group -- and reading it as the type is the only
                // reading that leaves a declaration behind.
                add(t.text);
                if (!spec.saw_type) spec.type.base = t.text;
                spec.saw_type = true;
                spec.saw_macro_group = true;
                spec.empty = false;
                p_++;
                SkipBalanced();
                continue;
            }
            if (t.kind == CTokKind::Ident) {
                // An identifier is part of the type only when something
                // after it can still be the declarator; otherwise it
                // *is* the declarator and this specifier list ended (an
                // implicit `int` declaration, or a K&R parameter).
                //
                // A second identifier is allowed even once a type is
                // settled, which is how `long double complex z` and
                // `static NIM_CONST Array a` read: in both, the extra
                // word is a macro that expanded to part of the type.
                // The next token still decides -- three identifiers in a
                // row end with the last one being the declarator.
                const CToken &next = At(1);
                const bool next_extends =
                    next.kind == CTokKind::Ident || (next.kind == CTokKind::Punct && next.text == "*") ||
                    // A qualifier only, never a type keyword: `double x
                    // double y` is a parameter list that lost a comma,
                    // and reading it as one four-word type would be
                    // exactly the wrong answer. `weak hidden const
                    // size_t x` still works, because `const` is a
                    // qualifier.
                    (next.kind == CTokKind::Keyword && QualifierSet().count(next.text) > 0 &&
                     !IsCTypeKeyword(next.text));
                const bool can_be_type =
                    next_extends || (!spec.saw_type && (IsTypedefName(t.text) ||
                                                        (next.kind == CTokKind::Keyword &&
                                                         (IsCTypeKeyword(next.text) ||
                                                          QualifierSet().count(next.text) > 0))));
                if (!can_be_type) break;
                spec.saw_type = true;
                spec.type.base = spec.type.base.empty() ? t.text : spec.type.base + " " + t.text;
                spec.empty = false;
                add(t.text);
                p_++;
                continue;
            }
            break;
        }
        spec.type.is_const = (spec.flags & kCFlagConst) != 0;
        spec.type.is_volatile = (spec.flags & kCFlagVolatile) != 0;
        spec.type.spelling = spelling;
        spec.end = PrevEnd();
        return spec;
    }

    /**
     * @brief Reports whether the parenthesized group at `open` is followed by something that can only be a declarator.
     *
     * The test that makes `RTDECL(int) RTStrNCmp(...)` a declaration and
     * `assert(x)` a call: a name followed by a group followed by another
     * name is a macro-produced type; a name followed by a group followed
     * by anything else is an invocation.
     */
    bool MacroTypeEndsAt(size_t open) const {
        int depth = 0;
        // A group holding nothing but a comma-separated identifier list
        // is what a K&R parameter list looks like (`int old(a, b)`), and
        // that must not be read as a macro type.
        bool plain_identifier_list = true;
        for (size_t q = open; q < idx_.size(); q++) {
            const CToken &t = all_[idx_[q]];
            if (t.kind != CTokKind::Punct) {
                if (depth == 1 && t.kind != CTokKind::Ident) plain_identifier_list = false;
                continue;
            }
            if (t.text == "(") {
                depth++;
                if (depth > 1) plain_identifier_list = false;
                continue;
            }
            if (depth == 1 && t.text != ")" && t.text != ",") plain_identifier_list = false;
            if (t.text == ")") {
                depth--;
                if (depth != 0) continue;
                const CToken &after = q + 1 < idx_.size() ? all_[idx_[q + 1]] : CToken();
                if (after.kind == CTokKind::Ident) return true;
                if (after.kind == CTokKind::Keyword) {
                    // `int old(a, b) int a; ...` is a K&R definition, not
                    // a macro type followed by one. Only a group that
                    // could not be a parameter list wins here.
                    if (plain_identifier_list) return false;
                    return IsCTypeKeyword(after.text) || QualifierSet().count(after.text) > 0;
                }
                return after.kind == CTokKind::Punct && after.text == "*";
            }
        }
        return false;
    }

    /** @brief Parses a `struct`/`union` specifier, with its member list when one is written. */
    CNodePtr ParseRecord() {
        const CPos start = Pos();
        CNodePtr rec = Make(CNodeKind::RecordDecl, start);
        rec->str_value = Cur().text;  // "struct" or "union"
        p_++;
        rec->flags |= SkipAttributes();
        // `struct ATTRIB_GCC_STRUCT __tI128 { ... }`: an attribute macro
        // sits where the tag goes. Only a `{` right after the run of
        // identifiers settles that -- in `struct timespec times[2];` the
        // second identifier is the variable, not the tag.
        {
            size_t run = 0;
            while (At(run).kind == CTokKind::Ident) run++;
            if (run > 1 && IsPunct("{", run)) p_ += run - 1;
        }
        if (Cur().kind == CTokKind::Ident) {
            rec->name = Cur().text;
            rec->name_pos = Cur().start;
            rec->name_end_col = Cur().end.col;
            p_++;
        }
        rec->flags |= SkipAttributes();
        if (EatPunct("{")) {
            ParseMemberList(rec.get());
        } else if (rec->name.empty()) {
            Error(start, PrevEnd(), "expected-tag",
                  "`" + rec->str_value + "` needs a tag name or a `{ ... }` member list");
        }
        rec->flags |= SkipAttributes();
        return Finish(std::move(rec));
    }

    /** @brief Parses a struct or union member list up to its closing brace. */
    void ParseMemberList(CNode *rec) {
        while (!AtEnd() && !IsPunct("}")) {
            const size_t before = p_;
            if (EatPunct(";")) continue;  // a stray `;` between members is harmless
            if (IsKw("_Static_assert")) {
                CNodePtr sa = ParseStaticAssert();
                rec->body.push_back(std::move(sa));
                continue;
            }
            // A member list is as full of macros as a file scope is
            // (`LIST_ENTRY(foo) link;` is a member, `N_NIMCALL_PTR(void,
            // p)(int);` is a whole one). Same shape rule as
            // TryParseMacroDeclaration, same reason.
            // A bare `PyObject_HEAD;` is a macro that expands to
            // members; a bare `ru_inblock;` is a member that lost its
            // type. An upper-case letter is what tells them apart, and
            // being wrong about it costs a missed error rather than an
            // invented one only in the direction people actually write.
            const bool macro_shaped =
                Cur().kind == CTokKind::Ident && Cur().text.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") !=
                                                     std::string::npos;
            if (Cur().kind == CTokKind::Ident && !IsTypedefName(Cur().text) &&
                ((IsPunct(";", 1) && macro_shaped) || IsPunct("(", 1))) {
                const size_t save = p_;
                p_++;
                while (IsPunct("(")) SkipBalanced();
                if (IsPunct(";")) {
                    p_++;
                    continue;
                }
                p_ = save;
            }
            DeclSpec spec = ParseDeclSpecifiers();
            if (spec.tag) rec->body.push_back(std::move(spec.tag));
            if (spec.empty && !IsPunct(";")) {
                Error(Pos(), Pos(), "expected-member", "Expected a struct member declaration");
                SyncToStatementEnd();
                if (p_ == before) p_++;
                continue;
            }
            if (EatPunct(";")) continue;  // an anonymous struct/union member
            for (;;) {
                CNodePtr field = Make(CNodeKind::Field, Pos());
                field->flags = spec.flags;
                // A bit-field may have no declarator at all: `int :3;`.
                if (!IsPunct(":")) {
                    DeclaratorInfo info = ParseDeclarator(spec.type, true);
                    field->name = info.name;
                    field->name_pos = info.name_pos;
                    field->name_end_col = info.name_end_col;
                    field->type = info.type;
                    field->params = std::move(info.params);
                    field->flags |= info.flags;
                }
                if (EatPunct(":")) field->kids.push_back(ParseConditional());
                field->flags |= SkipAttributes();
                rec->body.push_back(Finish(std::move(field)));
                if (EatPunct(",")) continue;
                break;
            }
            if (!EatPunct(";")) {
                Error(Pos(), Pos(), "expected-semicolon", "Expected `;` after this struct member");
                SyncToStatementEnd();
            }
            if (p_ == before) p_++;
        }
        Expect("}", "to close this struct or union");
    }

    /** @brief Parses an `enum` specifier, with its enumerator list when one is written. */
    CNodePtr ParseEnum() {
        const CPos start = Pos();
        CNodePtr en = Make(CNodeKind::EnumDecl, start);
        p_++;  // `enum`
        en->flags |= SkipAttributes();
        if (Cur().kind == CTokKind::Ident) {
            en->name = Cur().text;
            en->name_pos = Cur().start;
            en->name_end_col = Cur().end.col;
            p_++;
        }
        // C23 lets an enum name its underlying type: `enum E : int { ... }`.
        if (EatPunct(":")) {
            while (!AtEnd() && !IsPunct("{") && !IsPunct(";")) p_++;
        }
        en->flags |= SkipAttributes();
        if (EatPunct("{")) {
            while (!AtEnd() && !IsPunct("}")) {
                const size_t before = p_;
                if (Cur().kind != CTokKind::Ident) {
                    Error(Pos(), Pos(), "expected-enumerator", "Expected an enumeration constant");
                    break;
                }
                CNodePtr e = Make(CNodeKind::Enumerator, Pos());
                e->name = Cur().text;
                e->name_pos = Cur().start;
                e->name_end_col = Cur().end.col;
                p_++;
                e->flags |= SkipAttributes();
                if (EatPunct("=")) e->kids.push_back(ParseConditional());
                en->body.push_back(Finish(std::move(e)));
                if (!EatPunct(",")) break;
                if (p_ == before) p_++;
            }
            Expect("}", "to close this enum");
        }
        en->flags |= SkipAttributes();
        return Finish(std::move(en));
    }

    // --- Declarators --------------------------------------------------

    /**
     * @brief Parses a declarator, returning the name it declares and the type it builds on the base type.
     * @param base the type the specifier list named
     * @param abstract_ok whether a declarator with no name at all is allowed (a parameter, a cast)
     */
    DeclaratorInfo ParseDeclarator(CType base, bool abstract_ok) {
        DeclaratorInfo info;
        info.start = Pos();
        info.type = base;
        if (++depth_ > kMaxDepth) {
            depth_--;
            return info;
        }
        while (IsPunct("*")) {
            p_++;
            info.type.pointers++;
            for (;;) {
                if (Cur().kind == CTokKind::Keyword && QualifierSet().count(Cur().text) > 0) {
                    if (Cur().text == "const") info.type.is_const = true;
                    p_++;
                    continue;
                }
                const unsigned attr = SkipAttributes();
                if (attr != 0) {
                    info.flags |= attr;
                    continue;
                }
                break;
            }
        }
        info.flags |= SkipAttributes();

        bool nested = false;
        if (IsPunct("(") && OpensNestedDeclarator(1)) {
            // `int (*fp)(void)`: the inner declarator owns the name, the
            // suffixes out here apply to it.
            p_++;
            nested = true;
            // `void (WINAPI * fn)(LPFILETIME)`: a calling-convention
            // macro in front of the `*`. An identifier immediately
            // followed by `*` is not a declarator in any reading, so
            // skipping it loses nothing.
            while (Cur().kind == CTokKind::Ident && IsPunct("*", 1)) p_++;
            DeclaratorInfo inner = ParseDeclarator(CType(), true);
            info.name = inner.name;
            info.name_pos = inner.name_pos;
            info.name_end_col = inner.name_end_col;
            info.type.pointers += inner.type.pointers;
            info.flags |= inner.flags;
            // `void (*signal(int, void (*)(int)))(int) { ... }`: the
            // inner declarator is the one that took a parameter list, so
            // it is the inner one that says this can be a definition.
            if (inner.top_function) {
                info.top_function = true;
                info.type.is_function = true;
                // `void (*sigset(int sig, void (*h)(int)))(int)`: the
                // parameters that belong to the function being defined
                // are the inner declarator's, not the outer group's.
                info.params = std::move(inner.params);
                info.kandr = inner.kandr;
            }
            // `(name)` with nothing else in the group is only there to
            // keep a macro of the same name from expanding; the
            // parameter list after it still makes this a definition.
            if (inner.type.pointers == 0 && !inner.top_function && !inner.name.empty()) nested = false;
            Expect(")", "to close this declarator");
        } else if (Cur().kind == CTokKind::Ident) {
            info.name = Cur().text;
            info.name_pos = Cur().start;
            info.name_end_col = Cur().end.col;
            p_++;
        } else if (!abstract_ok) {
            Error(Pos(), Pos(), "expected-declarator", "Expected a name here");
        }

        for (bool first = true;; first = false) {
            if (IsPunct("[")) {
                p_++;
                info.type.array_dims++;
                // `[static 3]`, `[const restrict]`, `[*]` are all legal
                // inside an array declarator's brackets.
                while (!AtEnd() && !IsPunct("]")) {
                    if (IsPunct("[") || IsPunct("(")) {
                        SkipBalanced();
                        continue;
                    }
                    p_++;
                }
                Expect("]", "to close this array declarator");
                continue;
            }
            if (IsPunct("(")) {
                p_++;
                // Into a fresh vector: for `void (*f(int a))(int)` the
                // parameters that matter are the inner declarator's,
                // already in `info.params`, and the outer group is the
                // returned function pointer's.
                std::vector<CNodePtr> params;
                const bool kandr = ParseParamList(&params);
                if (first && !nested) {
                    info.top_function = true;
                    info.kandr = kandr;
                    info.type.is_function = true;
                    info.params = std::move(params);
                }
                continue;
            }
            break;
        }
        info.flags |= SkipAttributes();
        SkipTrailingMacros();
        info.type.spelling = SpellType(info.type);
        info.end = PrevEnd();
        depth_--;
        return info;
    }

    /**
     * @brief Skips a run of attribute-shaped macros between a declarator and its `;`, `,`, `=` or body.
     *
     * `int f(void) __THROW __nonnull((1));` and every glibc prototype
     * like it. The run is only taken when it lands on one of those four
     * tokens, so a genuinely missing `;` between two declarations is
     * still reported rather than quietly absorbed.
     */
    void SkipTrailingMacros() {
        if (Cur().kind != CTokKind::Ident) return;
        size_t k = p_;
        while (At(k - p_).kind == CTokKind::Ident) {
            const size_t name = k;
            k++;
            if (At(k - p_).kind == CTokKind::Punct && At(k - p_).text == "(") {
                // Step over the group without committing to it.
                int depth = 0;
                while (k < idx_.size()) {
                    const CToken &t = all_[idx_[k]];
                    if (t.kind == CTokKind::Punct) {
                        if (t.text == "(") depth++;
                        if (t.text == ")") {
                            depth--;
                            if (depth == 0) {
                                k++;
                                break;
                            }
                        }
                    }
                    k++;
                }
            }
            if (k == name) break;
        }
        if (k == p_) return;
        const CToken &after = k < idx_.size() ? all_[idx_[k]] : CToken();
        const bool lands_well = after.kind == CTokKind::Punct &&
                                (after.text == ";" || after.text == "," || after.text == "{" || after.text == "=");
        if (lands_well) p_ = k;
    }

    /**
     * @brief Reports whether the `(` at `k - 1` groups a declarator rather than opening a parameter list.
     *
     * `int (*f)(void)` versus `int (void)`: the first token inside tells
     * them apart, because a parameter list always starts with a type and
     * a grouped declarator never can.
     */
    bool OpensNestedDeclarator(size_t k) const {
        const CToken &t = At(k);
        if (t.kind == CTokKind::Punct) return t.text == "*" || t.text == "(";
        if (t.kind == CTokKind::Ident) return !IsTypedefName(t.text);
        if (t.kind == CTokKind::Keyword) {
            // `int (__attribute__((x)) *f)(void)` and every calling
            // convention spelling. A type keyword here would be a
            // parameter list instead, which is the case this excludes.
            return t.text == "__attribute__" || t.text == "__attribute" ||
                   (QualifierSet().count(t.text) > 0 && !IsCTypeKeyword(t.text));
        }
        return false;
    }

    /**
     * @brief Parses a parameter list up to its `)`.
     * @param out receives one Param node per parameter (a `...` parameter included, named "...")
     * @return true when the list was a K&R identifier list rather than a prototype
     */
    bool ParseParamList(std::vector<CNodePtr> *out) {
        // `f(void)` takes no parameters; `f()` takes an unspecified
        // number, which this server records the same way.
        if (IsKw("void") && IsPunct(")", 1)) {
            p_ += 2;
            return false;
        }
        if (EatPunct(")")) return false;
        bool all_idents = true;
        while (!AtEnd()) {
            const size_t before = p_;
            if (IsPunct(")")) break;
            if (IsPunct("...")) {
                CNodePtr param = Make(CNodeKind::Param, Pos());
                param->name = "...";
                p_++;
                out->push_back(Finish(std::move(param)));
            } else {
                CNodePtr param = Make(CNodeKind::Param, Pos());
                DeclSpec spec = ParseDeclSpecifiers();
                if (spec.tag) param->kids.push_back(std::move(spec.tag));
                if (!spec.empty) all_idents = false;
                if (spec.empty && Cur().kind == CTokKind::Ident) {
                    // A K&R identifier list: `int f(a, b) int a; int b; {`.
                    param->name = Cur().text;
                    param->name_pos = Cur().start;
                    param->name_end_col = Cur().end.col;
                    p_++;
                } else {
                    DeclaratorInfo info = ParseDeclarator(spec.type, true);
                    param->name = info.name;
                    param->name_pos = info.name_pos;
                    param->name_end_col = info.name_end_col;
                    param->type = info.type;
                    param->flags = spec.flags | info.flags;
                    if (!info.name.empty()) all_idents = false;
                }
                out->push_back(Finish(std::move(param)));
            }
            if (EatPunct(",") && p_ != before) continue;
            break;
        }
        Expect(")", "to close this parameter list");
        return all_idents && !out->empty();
    }

    /** @brief Renders a type back to something readable, for hover and for a symbol's detail line. */
    static std::string SpellType(const CType &t) {
        std::string s = t.base.empty() ? std::string("int") : t.base;
        if (t.is_const && s.rfind("const", 0) != 0) s = "const " + s;
        if (t.pointers > 0) {
            s += ' ';
            s.append(static_cast<size_t>(t.pointers), '*');
        }
        for (int i = 0; i < t.array_dims; i++) s += "[]";
        return s;
    }

    // --- External declarations ----------------------------------------

    /** @brief Parses one top-level construct: a declaration, a function definition, or a macro invocation. */
    CNodePtr ParseExternalDeclaration() {
        if (EatPunct(";")) return nullptr;  // a stray `;` at file scope is legal and empty
        if (ErrorBudgetSpent() && !AtEnd()) {
            // Past the error budget, stop trying to make sense of the
            // file and just walk it out; the tree already has everything
            // that was readable.
            p_++;
            return nullptr;
        }
        if (IsKw("_Static_assert") || (Cur().kind == CTokKind::Ident && Cur().text == "static_assert")) {
            return ParseStaticAssert();
        }
        if ((IsKw("asm") || IsKw("__asm") || IsKw("__asm__")) && (IsPunct("(", 1) || At(1).kind == CTokKind::Keyword)) {
            return ParseAsm();
        }
        if (IsPunct("}")) {
            // A brace with nothing to close. Report it and step over it,
            // rather than letting every later construct nest wrong.
            Error(Pos(), Cur().end, "unexpected-brace", "Unmatched `}`");
            p_++;
            return nullptr;
        }
        if (CNodePtr macro = TryParseMacroDeclaration()) return macro;
        return ParseDeclaration(true);
    }

    /**
     * @brief Parses the file-scope shapes that are macro invocations rather than declarations.
     *
     * Real headers are full of these -- `MODULE_LICENSE("GPL");`,
     * `__BEGIN_DECLS`, `PUBLIC int f(void);` -- and none of them is
     * anything a compiler that has not run the preprocessor can call a
     * declaration. Recognizing their *shape* costs three lookaheads and
     * is the difference between a header that outlines cleanly and one
     * that is a wall of red.
     */
    CNodePtr TryParseMacroDeclaration() {
        if (Cur().kind != CTokKind::Ident || IsTypedefName(Cur().text)) return nullptr;
        const CPos start = Pos();
        const std::string name = Cur().text;
        const int name_end = Cur().end.col;

        // `NAME(...)` followed by `;`, end of file, or the start of the
        // next construct: a macro invocation standing in for a
        // declaration.
        if (IsPunct("(", 1)) {
            const size_t save = p_;
            p_++;
            // A macro that stands in for a whole declarator can be
            // followed by a second parenthesized group -- generated code
            // writes `N_NIMCALL(void, f)(int x);` -- so take every
            // adjacent group before deciding.
            while (IsPunct("(")) SkipBalanced();
            if (IsPunct("{")) {
                // `START_TEST(name) { ... }`: a macro that opens a
                // function definition. The name is inside the macro
                // arguments, so there is nothing to declare, but the
                // body is real code and belongs in the tree.
                CNodePtr n = Make(CNodeKind::MacroDecl, start);
                n->name = name;
                n->name_pos = start;
                n->name_end_col = name_end;
                CollectMacroArgumentNames(save + 1, n.get());
                CNodePtr block = ParseCompound();
                n->body = std::move(block->body);
                return Finish(std::move(n));
            }
            if (AtEnd() || IsPunct(";") || IsPunct("}") || StartsDeclaration()) {
                CNodePtr n = Make(CNodeKind::MacroDecl, start);
                n->name = name;
                n->name_pos = start;
                n->name_end_col = name_end;
                CollectMacroArgumentNames(save + 1, n.get());
                EatPunct(";");
                return Finish(std::move(n));
            }
            p_ = save;
            return nullptr;
        }
        // A bare `NAME` in front of something that can only be a
        // declaration (`__BEGIN_DECLS` / `PUBLIC`), or at end of file.
        const CToken &next = At(1);
        const bool next_starts_decl =
            next.kind == CTokKind::Keyword && (IsCTypeKeyword(next.text) || QualifierSet().count(next.text) > 0);
        if (next_starts_decl || next.kind == CTokKind::End) {
            CNodePtr n = Make(CNodeKind::MacroDecl, start);
            n->name = name;
            n->name_pos = start;
            n->name_end_col = name_end;
            p_++;
            return Finish(std::move(n));
        }
        return nullptr;
    }

    /**
     * @brief Records the identifiers inside a macro invocation's arguments as Ident children.
     *
     * `weak_alias(dummy_0, __fork);` is the only mention `dummy_0` has
     * in the file, and without this the analysis half would call it a
     * dead `static` function. The arguments are not parsed -- only a
     * macro knows what they mean -- but the names in them are real
     * mentions and have to count as such.
     */
    void CollectMacroArgumentNames(size_t open, CNode *node) {
        int depth = 0;
        for (size_t q = open; q < idx_.size(); q++) {
            const CToken &t = all_[idx_[q]];
            if (t.kind == CTokKind::Punct) {
                if (t.text == "(") depth++;
                if (t.text == ")") {
                    depth--;
                    if (depth <= 0) return;
                }
                continue;
            }
            if (t.kind != CTokKind::Ident) continue;
            CNodePtr id = Make(CNodeKind::Ident, t.start);
            id->name = t.text;
            id->name_pos = t.start;
            id->name_end_col = t.end.col;
            id->end = t.end;
            node->kids.push_back(std::move(id));
        }
    }

    /** @brief Reports whether the cursor sits on a token that can only begin a declaration. */
    bool StartsDeclaration() const {
        const CToken &t = Cur();
        if (t.kind != CTokKind::Keyword) return false;
        return IsCTypeKeyword(t.text) || QualifierSet().count(t.text) > 0;
    }

    /** @brief Parses `_Static_assert(expr, "message");`, in either of its two spellings. */
    CNodePtr ParseStaticAssert() {
        CNodePtr n = Make(CNodeKind::StaticAssert, Pos());
        p_++;
        if (EatPunct("(")) {
            n->kids.push_back(ParseConditional());
            if (EatPunct(",")) n->kids.push_back(ParseAssignment());
            Expect(")", "to close this assertion");
        }
        EatPunct(";");
        return Finish(std::move(n));
    }

    /** @brief Parses a top-level or statement `asm(...)`, whose contents this server deliberately does not read. */
    CNodePtr ParseAsm() {
        CNodePtr n = Make(CNodeKind::AsmStmt, Pos());
        p_++;
        while (Cur().kind == CTokKind::Keyword && QualifierSet().count(Cur().text) > 0) p_++;
        if (IsKw("goto")) p_++;
        if (IsPunct("(")) SkipBalanced();
        EatPunct(";");
        return Finish(std::move(n));
    }

    /**
     * @brief Parses a declaration, or the function definition a declaration turns into when a `{` follows.
     * @param file_scope whether a function definition may start here
     */
    CNodePtr ParseDeclaration(bool file_scope) {
        const CPos start = Pos();
        DeclSpec spec = ParseDeclSpecifiers();
        CNodePtr decl = Make(CNodeKind::Declaration, start);
        decl->flags = spec.flags;
        decl->str_value = spec.type.spelling;
        decl->type = spec.type;
        CNodePtr tag = std::move(spec.tag);

        if (spec.empty && !spec.saw_type) {
            Error(Pos(), Pos(), "expected-declaration", "Expected a declaration");
            SyncToStatementEnd();
            return Finish(std::move(decl));
        }
        if (EatPunct(";")) {
            // `struct S { ... };` -- the tag definition was the point.
            if (tag) {
                decl->flags |= kCFlagTagOnly;
                decl->kids.push_back(std::move(tag));
            }
            return Finish(std::move(decl));
        }
        if (tag) decl->kids.push_back(std::move(tag));

        // A declarator that is itself a macro invocation:
        // `typedef N_NIMCALL_PTR(void, Fn) (int);` declares `Fn`, but
        // only the preprocessor can see that. Recognized by shape -- a
        // name, a parenthesized group, then a second one -- and left as
        // a MacroDecl rather than reported.
        // `isl_ctx *ISL_FN(H, get)(H *h)` puts the macro after the `*`,
        // so the pointers are stepped over speculatively -- and put back
        // below if this turns out to be an ordinary declarator.
        const size_t before_stars = p_;
        size_t stars = 0;
        while (IsPunct("*", stars)) stars++;
        if (stars > 0 && At(stars).kind == CTokKind::Ident && !IsTypedefName(At(stars).text) &&
            IsPunct("(", stars + 1)) {
            p_ += stars;
        }
        if (Cur().kind == CTokKind::Ident && !IsTypedefName(Cur().text) && IsPunct("(", 1)) {
            const size_t save = before_stars;
            const CPos macro_start = Pos();
            const std::string macro_name = Cur().text;
            const int macro_name_end = Cur().end.col;
            const bool literal_args = GroupHasLiteralArgument(p_ + 1);
            p_++;
            SkipBalanced();
            // A group holding a string or a number is an argument list,
            // not a parameter list: `DECLARE_FSTYPE(t, "vboxsf", f, 0);`
            // is a macro, `static foo(a, b);` is an old-style prototype.
            if (!spec.saw_type && literal_args && IsPunct(";")) {
                p_++;
                CNodePtr n = Make(CNodeKind::MacroDecl, macro_start);
                n->name = macro_name;
                n->name_pos = macro_start;
                n->name_end_col = macro_name_end;
                return Finish(std::move(n));
            }
            if (IsPunct("(")) {
                SkipBalanced();
                if (IsPunct(";") || IsPunct("{")) {
                    CNodePtr n = Make(CNodeKind::MacroDecl, macro_start);
                    n->name = macro_name;
                    n->name_pos = macro_start;
                    n->name_end_col = macro_name_end;
                    if (IsPunct("{")) {
                        CNodePtr block = ParseCompound();
                        n->body = std::move(block->body);
                    } else {
                        p_++;
                    }
                    return Finish(std::move(n));
                }
            }
            p_ = save;
        } else {
            p_ = before_stars;
        }

        bool first = true;
        for (;;) {
            DeclaratorInfo info = ParseDeclarator(spec.type, false);
            CNodePtr d = Make(CNodeKind::Declarator, info.start);
            d->name = info.name;
            d->name_pos = info.name_pos;
            d->name_end_col = info.name_end_col;
            d->type = info.type;
            d->flags = spec.flags | info.flags;
            d->params = std::move(info.params);
            if (info.kandr) d->flags |= kCFlagKandR;
            d->end = info.end;

            if ((spec.flags & kCFlagTypedef) != 0) AddTypedef(info.name);

            // A function definition: the declarator is `name(params)` and
            // a body (or a K&R parameter declaration list) follows.
            if (file_scope && first && info.top_function && (IsPunct("{") || StartsKandRDeclarations(info))) {
                return ParseFunctionBody(std::move(decl), std::move(d), start);
            }
            if (EatPunct("=")) d->kids.push_back(ParseInitializer());
            decl->kids.push_back(Finish(std::move(d)));
            first = false;
            if (EatPunct(",")) continue;
            break;
        }
        if (!EatPunct(";")) {
            Error(Pos(), Pos(), "expected-semicolon", "Expected `;` after this declaration");
            SyncToStatementEnd();
        }
        return Finish(std::move(decl));
    }

    /** @brief Reports whether a parenthesized group holds a string or numeric literal at its top level. */
    bool GroupHasLiteralArgument(size_t open) const {
        int depth = 0;
        for (size_t q = open; q < idx_.size(); q++) {
            const CToken &t = all_[idx_[q]];
            if (t.kind == CTokKind::Punct) {
                if (t.text == "(") depth++;
                if (t.text == ")") {
                    depth--;
                    if (depth == 0) return false;
                }
                continue;
            }
            if (depth == 1 && (t.kind == CTokKind::String || t.kind == CTokKind::Number)) return true;
        }
        return false;
    }

    /** @brief Reports whether a K&R parameter declaration list follows a function declarator. */
    bool StartsKandRDeclarations(const DeclaratorInfo &info) const {
        return info.kandr && (StartsDeclaration() || (Cur().kind == CTokKind::Ident && IsTypedefName(Cur().text)));
    }

    /** @brief Parses a function definition's K&R parameter declarations (when written) and its body. */
    CNodePtr ParseFunctionBody(CNodePtr decl, CNodePtr declarator, const CPos &start) {
        CNodePtr fn = Make(CNodeKind::FunctionDef, start);
        fn->name = declarator->name;
        fn->name_pos = declarator->name_pos;
        fn->name_end_col = declarator->name_end_col;
        fn->type = declarator->type;
        // The declarator's type carries the `*` of `void *f(void)`,
        // which the specifier list never sees. What it must not carry
        // into a *return* type is the "this is a function" flag.
        fn->type.is_function = false;
        fn->str_value = decl->str_value;
        fn->flags = declarator->flags;
        fn->params = std::move(declarator->params);
        if (!decl->kids.empty()) fn->kids.push_back(std::move(decl->kids.front()));

        // K&R parameter declarations, which give the identifier-list
        // parameters their types.
        while (!AtEnd() && !IsPunct("{") && (StartsDeclaration() || LooksLikeDeclaration())) {
            const size_t before = p_;
            CNodePtr kr = ParseDeclaration(false);
            ApplyKandRTypes(fn.get(), kr.get());
            if (p_ == before) break;
        }
        if (IsPunct("{")) {
            CNodePtr body = ParseCompound();
            fn->body = std::move(body->body);
            fn->end = body->end;
            return fn;
        }
        Error(Pos(), Pos(), "expected-function-body", "Expected `{` to open this function's body");
        return Finish(std::move(fn));
    }

    /** @brief Copies the types from a K&R parameter declaration onto the matching identifier-list parameters. */
    static void ApplyKandRTypes(CNode *fn, const CNode *decl) {
        for (const CNodePtr &d : decl->kids) {
            for (CNodePtr &param : fn->params) {
                if (param->name == d->name) {
                    param->type = d->type;
                    param->start = d->start;
                }
            }
        }
    }

    /** @brief Parses an initializer: either a braced list or a single assignment expression. */
    CNodePtr ParseInitializer() {
        if (!IsPunct("{")) return ParseAssignment();
        CNodePtr list = Make(CNodeKind::InitList, Pos());
        p_++;
        if (++depth_ > kMaxDepth) {
            depth_--;
            SkipBalanced();
            return Finish(std::move(list));
        }
        while (!AtEnd() && !IsPunct("}")) {
            const size_t before = p_;
            CNodePtr entry;
            if (IsPunct(".") || IsPunct("[") ||
                (Cur().kind == CTokKind::Ident && IsPunct(":", 1) && !IsPunct(":", 2))) {
                entry = ParseDesignator();
            } else {
                entry = ParseInitializer();
            }
            list->kids.push_back(std::move(entry));
            if (!EatPunct(",")) {
                // `{ TAGKEYS(XK_1, 0) TAGKEYS(XK_2, 1) }`: each macro
                // expands to several comma-separated entries, so there
                // is no comma between them in the source.
                if (!(Cur().kind == CTokKind::Ident && IsPunct("(", 1))) break;
            }
            if (p_ == before) p_++;
        }
        Expect("}", "to close this initializer");
        depth_--;
        return Finish(std::move(list));
    }

    /** @brief Parses a designated initializer entry (`.field = v`, `[i] = v`, and the GNU range form). */
    CNodePtr ParseDesignator() {
        CNodePtr d = Make(CNodeKind::Designator, Pos());
        // GNU's pre-C99 spelling, which the Linux kernel is full of:
        // `owner: THIS_MODULE,` means the same as `.owner = THIS_MODULE`.
        if (Cur().kind == CTokKind::Ident && IsPunct(":", 1)) {
            d->name = Cur().text;
            d->name_pos = Cur().start;
            d->name_end_col = Cur().end.col;
            p_ += 2;
            d->kids.push_back(ParseInitializer());
            return Finish(std::move(d));
        }
        for (;;) {
            if (EatPunct(".")) {
                if (Cur().kind == CTokKind::Ident || Cur().kind == CTokKind::Keyword) {
                    if (d->name.empty()) {
                        d->name = Cur().text;
                        d->name_pos = Cur().start;
                        d->name_end_col = Cur().end.col;
                    }
                    p_++;
                }
                continue;
            }
            if (EatPunct("[")) {
                d->kids.push_back(ParseConditional());
                if (EatPunct("...")) d->kids.push_back(ParseConditional());
                Expect("]", "to close this array designator");
                continue;
            }
            break;
        }
        if (EatPunct("=")) d->kids.push_back(ParseInitializer());
        return Finish(std::move(d));
    }

    // --- Statements ---------------------------------------------------

    /** @brief Parses a `{ ... }` block. */
    CNodePtr ParseCompound() {
        CNodePtr block = Make(CNodeKind::CompoundStmt, Pos());
        if (!EatPunct("{")) return Finish(std::move(block));
        if (++depth_ > kMaxDepth) {
            depth_--;
            Error(block->start, block->start, "nesting-too-deep", "Too deeply nested to analyze");
            p_--;
            SkipBalanced();
            return Finish(std::move(block));
        }
        while (!AtEnd() && !IsPunct("}")) {
            const size_t before = p_;
            CNodePtr item = ParseBlockItem();
            if (item) block->body.push_back(std::move(item));
            if (p_ == before) p_++;
        }
        Expect("}", "to close this block");
        depth_--;
        return Finish(std::move(block));
    }

    /** @brief Parses one item of a block: a declaration or a statement. */
    CNodePtr ParseBlockItem() {
        if (IsKw("_Static_assert") || (Cur().kind == CTokKind::Ident && Cur().text == "static_assert" &&
                                       IsPunct("(", 1))) {
            return ParseStaticAssert();
        }
        // A label always wins over a declaration reading: `done: int x;`
        // is a label followed by a declaration, not a declaration of
        // something called `done`.
        if (Cur().kind == CTokKind::Ident && IsPunct(":", 1)) return ParseStatement();
        if (LooksLikeDeclaration()) {
            CNodePtr stmt = Make(CNodeKind::DeclStmt, Pos());
            stmt->kids.push_back(ParseDeclaration(false));
            return Finish(std::move(stmt));
        }
        return ParseStatement();
    }

    /** @brief Parses one statement. */
    CNodePtr ParseStatement() {
        if (++depth_ > kMaxDepth) {
            depth_--;
            SyncToStatementEnd();
            return nullptr;
        }
        CNodePtr result = ParseStatementInner();
        depth_--;
        return result;
    }

    /** @brief The body of ParseStatement, with the recursion guard applied by its caller. */
    CNodePtr ParseStatementInner() {
        const CPos start = Pos();
        if (IsPunct("{")) return ParseCompound();
        if (IsPunct(";")) {
            CNodePtr n = Make(CNodeKind::EmptyStmt, start);
            p_++;
            return Finish(std::move(n));
        }
        if (IsKw("if")) return ParseIf();
        if (IsKw("while")) return ParseWhile();
        if (IsKw("do")) return ParseDo();
        if (IsKw("for")) return ParseFor();
        if (IsKw("switch")) return ParseSwitch();
        if (IsKw("case")) return ParseCase();
        if (IsKw("default")) {
            CNodePtr n = Make(CNodeKind::DefaultStmt, start);
            p_++;
            Expect(":", "after `default`");
            n->body.push_back(ParseLabeledBody());
            return Finish(std::move(n));
        }
        if (IsKw("return")) {
            CNodePtr n = Make(CNodeKind::ReturnStmt, start);
            p_++;
            if (!IsPunct(";")) n->kids.push_back(ParseExpression());
            ExpectSemicolon("after `return`");
            return Finish(std::move(n));
        }
        if (IsKw("break") || IsKw("continue")) {
            CNodePtr n = Make(IsKw("break") ? CNodeKind::BreakStmt : CNodeKind::ContinueStmt, start);
            p_++;
            ExpectSemicolon("after this statement");
            return Finish(std::move(n));
        }
        if (IsKw("goto")) {
            CNodePtr n = Make(CNodeKind::GotoStmt, start);
            p_++;
            if (Cur().kind == CTokKind::Ident) {
                n->name = Cur().text;
                n->name_pos = Cur().start;
                n->name_end_col = Cur().end.col;
                p_++;
            } else if (IsPunct("*")) {
                // GNU computed goto: `goto *label_address;`.
                p_++;
                n->kids.push_back(ParseExpression());
            }
            ExpectSemicolon("after `goto`");
            return Finish(std::move(n));
        }
        if (IsKw("asm") || IsKw("__asm") || IsKw("__asm__")) return ParseAsm();
        if (IsKw("__label__")) {
            // GNU local label declaration: `__label__ again, done;`.
            while (!AtEnd() && !IsPunct(";")) p_++;
            EatPunct(";");
            return nullptr;
        }
        if (Cur().kind == CTokKind::Ident && IsPunct(":", 1)) {
            CNodePtr n = Make(CNodeKind::LabelStmt, start);
            n->name = Cur().text;
            n->name_pos = Cur().start;
            n->name_end_col = Cur().end.col;
            p_ += 2;
            SkipAttributes();
            n->body.push_back(ParseLabeledBody());
            return Finish(std::move(n));
        }
        CNodePtr n = Make(CNodeKind::ExprStmt, start);
        CNodePtr expr = ParseExpression();
        // `list_for_each_entry(p, head, link) { ... }`: a macro that
        // takes a block. It looks exactly like a call followed by a
        // compound statement, so that is how it is read -- the block's
        // contents are real code and belong in the tree.
        if (expr->kind == CNodeKind::Call && IsPunct("{")) {
            n->kids.push_back(std::move(expr));
            n->body.push_back(ParseCompound());
            return Finish(std::move(n));
        }
        n->kids.push_back(std::move(expr));
        ExpectSemicolon("after this expression");
        return Finish(std::move(n));
    }

    /** @brief Consumes a statement's `;`, reporting its absence without losing the parser's place. */
    void ExpectSemicolon(const char *what) {
        if (EatPunct(";")) return;
        Error(Pos(), Pos(), "expected-semicolon", std::string("Expected `;` ") + what);
        SyncToStatementEnd();
    }

    /**
     * @brief Parses what a label, `case` or `default` labels.
     *
     * C23 finally allows a label at the end of a block and in front of a
     * declaration; before it, neither was legal. Accepting both is the
     * right call for an editor, which has no business reporting a
     * standard-version difference as a syntax error.
     */
    CNodePtr ParseLabeledBody() {
        if (IsPunct("}") || AtEnd()) return nullptr;
        if (IsKw("case") || IsKw("default")) return nullptr;
        return ParseBlockItem();
    }

    /** @brief Parses `if (cond) then [else otherwise]`. */
    CNodePtr ParseIf() {
        CNodePtr n = Make(CNodeKind::IfStmt, Pos());
        p_++;
        if (Expect("(", "after `if`")) {
            n->kids.push_back(ParseExpression());
            Expect(")", "to close this condition");
        } else {
            n->kids.push_back(Placeholder(Pos()));
        }
        n->body.push_back(ParseStatement());
        if (EatKw("else")) n->orelse.push_back(ParseStatement());
        return Finish(std::move(n));
    }

    /** @brief Parses `while (cond) body`. */
    CNodePtr ParseWhile() {
        CNodePtr n = Make(CNodeKind::WhileStmt, Pos());
        p_++;
        if (Expect("(", "after `while`")) {
            n->kids.push_back(ParseExpression());
            Expect(")", "to close this condition");
        } else {
            n->kids.push_back(Placeholder(Pos()));
        }
        n->body.push_back(ParseStatement());
        return Finish(std::move(n));
    }

    /** @brief Parses `do body while (cond);`. */
    CNodePtr ParseDo() {
        CNodePtr n = Make(CNodeKind::DoStmt, Pos());
        p_++;
        n->body.push_back(ParseStatement());
        if (!EatKw("while")) {
            Error(Pos(), Pos(), "expected-while", "Expected `while` to close this `do` loop");
            return Finish(std::move(n));
        }
        if (Expect("(", "after `while`")) {
            n->kids.push_back(ParseExpression());
            Expect(")", "to close this condition");
        }
        ExpectSemicolon("after a `do ... while` loop");
        return Finish(std::move(n));
    }

    /** @brief Parses `for (init; cond; step) body`, with either an expression or a declaration as the init. */
    CNodePtr ParseFor() {
        CNodePtr n = Make(CNodeKind::ForStmt, Pos());
        p_++;
        if (!Expect("(", "after `for`")) return Finish(std::move(n));
        if (IsPunct(";")) {
            n->kids.push_back(Placeholder(Pos()));
            p_++;
        } else if (LooksLikeDeclaration()) {
            CNodePtr stmt = Make(CNodeKind::DeclStmt, Pos());
            stmt->kids.push_back(ParseDeclaration(false));
            n->kids.push_back(Finish(std::move(stmt)));
        } else {
            CNodePtr init = Make(CNodeKind::ExprStmt, Pos());
            init->kids.push_back(ParseExpression());
            n->kids.push_back(Finish(std::move(init)));
            ExpectSemicolon("after this `for` initializer");
        }
        n->kids.push_back(IsPunct(";") ? Placeholder(Pos()) : ParseExpression());
        Expect(";", "after this `for` condition");
        n->kids.push_back(IsPunct(")") ? Placeholder(Pos()) : ParseExpression());
        Expect(")", "to close this `for` header");
        n->body.push_back(ParseStatement());
        return Finish(std::move(n));
    }

    /** @brief Parses `switch (subject) body`. */
    CNodePtr ParseSwitch() {
        CNodePtr n = Make(CNodeKind::SwitchStmt, Pos());
        p_++;
        if (Expect("(", "after `switch`")) {
            n->kids.push_back(ParseExpression());
            Expect(")", "to close this subject");
        } else {
            n->kids.push_back(Placeholder(Pos()));
        }
        n->body.push_back(ParseStatement());
        return Finish(std::move(n));
    }

    /** @brief Parses `case value:`, including the GNU `case lo ... hi:` range form. */
    CNodePtr ParseCase() {
        CNodePtr n = Make(CNodeKind::CaseStmt, Pos());
        p_++;
        n->kids.push_back(ParseConditional());
        if (EatPunct("...")) n->kids.push_back(ParseConditional());
        Expect(":", "after this `case` label");
        n->body.push_back(ParseLabeledBody());
        return Finish(std::move(n));
    }

    // --- Expressions --------------------------------------------------

    /** @brief Parses a full expression, comma operator included. */
    CNodePtr ParseExpression() {
        CNodePtr left = ParseAssignment();
        while (IsPunct(",")) {
            CNodePtr n = Make(CNodeKind::Comma, left->start);
            p_++;
            n->kids.push_back(std::move(left));
            n->kids.push_back(ParseAssignment());
            left = Finish(std::move(n));
        }
        return left;
    }

    /** @brief Parses an assignment expression (the operand of a comma, an argument, an initializer). */
    CNodePtr ParseAssignment() {
        if (++depth_ > kMaxDepth) {
            depth_--;
            const CPos start = Pos();
            SyncToStatementEnd();
            return Placeholder(start);
        }
        CNodePtr left = ParseConditional();
        static const char *const kAssignOps[] = {"=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "^=", "|="};
        for (const char *op : kAssignOps) {
            if (!IsPunct(op)) continue;
            CNodePtr n = Make(CNodeKind::Assign, left->start);
            n->name = op;
            n->name_pos = Pos();
            p_++;
            n->kids.push_back(std::move(left));
            n->kids.push_back(ParseAssignment());
            depth_--;
            return Finish(std::move(n));
        }
        depth_--;
        return left;
    }

    /** @brief Parses `a ? b : c`, including the GNU `a ?: b` form. */
    CNodePtr ParseConditional() {
        CNodePtr cond = ParseBinary(0);
        if (!IsPunct("?")) return cond;
        CNodePtr n = Make(CNodeKind::Conditional, cond->start);
        p_++;
        n->kids.push_back(std::move(cond));
        n->kids.push_back(IsPunct(":") ? Placeholder(Pos()) : ParseExpression());
        Expect(":", "in this conditional expression");
        n->kids.push_back(ParseConditional());
        return Finish(std::move(n));
    }

    /** @brief The binary operator precedence ladder, lowest level first. */
    static int PrecedenceOf(const std::string &op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "|") return 3;
        if (op == "^") return 4;
        if (op == "&") return 5;
        if (op == "==" || op == "!=") return 6;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 7;
        if (op == "<<" || op == ">>") return 8;
        if (op == "+" || op == "-") return 9;
        if (op == "*" || op == "/" || op == "%") return 10;
        return 0;
    }

    /** @brief Precedence climbing over the binary operators, from `min_prec` up. */
    CNodePtr ParseBinary(int min_prec) {
        CNodePtr left = ParseCastExpression();
        for (;;) {
            if (Cur().kind != CTokKind::Punct) break;
            const int prec = PrecedenceOf(Cur().text);
            if (prec == 0 || prec < min_prec) break;
            CNodePtr n = Make(CNodeKind::Binary, left->start);
            n->name = Cur().text;
            n->name_pos = Pos();
            n->name_end_col = Cur().end.col;
            p_++;
            n->kids.push_back(std::move(left));
            n->kids.push_back(ParseBinary(prec + 1));
            left = Finish(std::move(n));
        }
        return left;
    }

    /**
     * @brief Reports whether `( ... )` at the cursor can only be a type name, even one this file never declared.
     *
     * `(NI)8` and `(Length){a, b}` are casts and compound literals whose
     * type came from a header, so IsTypedefName knows nothing about it.
     * Refusing to read them is not neutral -- it turns every one into a
     * syntax error -- so the *shape* decides instead: a group that is an
     * identifier followed by at least one `*` cannot be an expression at
     * all, and a bare `(Name)` counts when what follows it can only be
     * an operand, never a binary operator's right-hand side. `(a) - b`
     * and `(a) * b` therefore stay subtraction and multiplication.
     */
    bool LooksLikeUnknownTypeCast() const {
        size_t k = 1;
        if (At(k).kind != CTokKind::Ident) return false;
        k++;
        while (At(k).kind == CTokKind::Keyword && QualifierSet().count(At(k).text) > 0) k++;
        size_t stars = 0;
        while (IsPunct("*", k)) {
            k++;
            stars++;
            while (At(k).kind == CTokKind::Keyword && QualifierSet().count(At(k).text) > 0) k++;
        }
        size_t brackets = 0;
        while (IsPunct("[", k)) {
            int depth = 0;
            while (p_ + k < idx_.size()) {
                if (IsPunct("[", k)) depth++;
                if (IsPunct("]", k)) {
                    depth--;
                    if (depth == 0) {
                        k++;
                        break;
                    }
                }
                k++;
            }
            brackets++;
        }
        if (!IsPunct(")", k)) return false;
        if (stars > 0) return true;
        // `(size_t [3]){0, 1, 2}`: an array type is only unambiguous in
        // front of a braced initializer, since `(a[3])` on its own is a
        // perfectly good subscript.
        if (brackets > 0) return IsPunct("{", k + 1);
        const CToken &after = At(k + 1);
        switch (after.kind) {
            case CTokKind::Number:
            case CTokKind::String:
            case CTokKind::CharLit:
            case CTokKind::Ident:
                return true;
            case CTokKind::Punct:
                return after.text == "(" || after.text == "{" || after.text == "~" || after.text == "!";
            default:
                return false;
        }
    }

    /** @brief Parses a cast expression, or falls through to a unary one. */
    CNodePtr ParseCastExpression() {
        if (IsPunct("(") && (StartsTypeName(1) || LooksLikeUnknownTypeCast())) {
            const size_t save = p_;
            const CPos start = Pos();
            p_++;
            CNodePtr type = ParseTypeName();
            if (EatPunct(")")) {
                // `(struct s){ ... }` is a compound literal, not a cast.
                if (IsPunct("{")) {
                    CNodePtr lit = Make(CNodeKind::CompoundLiteral, start);
                    lit->kids.push_back(std::move(type));
                    lit->kids.push_back(ParseInitializer());
                    // `(struct s){0}.field` and `(int[]){1, 2}[i]`: a
                    // compound literal is an operand like any other.
                    return ParsePostfixSuffixes(Finish(std::move(lit)));
                }
                // A `)` immediately followed by something that cannot
                // start an operand means this was a parenthesized
                // expression after all, not a cast.
                if (StartsUnaryOperand()) {
                    CNodePtr n = Make(CNodeKind::Cast, start);
                    n->kids.push_back(std::move(type));
                    n->kids.push_back(ParseCastExpression());
                    return Finish(std::move(n));
                }
            }
            p_ = save;
        }
        return ParseUnary();
    }

    /** @brief Reports whether the cursor sits on something a cast can be applied to. */
    bool StartsUnaryOperand() const {
        const CToken &t = Cur();
        switch (t.kind) {
            case CTokKind::Ident:
            case CTokKind::Number:
            case CTokKind::String:
            case CTokKind::CharLit:
                return true;
            case CTokKind::Keyword:
                return t.text == "sizeof" || t.text == "_Alignof" || t.text == "__alignof__" || t.text == "_Generic" ||
                       t.text == "__real__" || t.text == "__imag__" || t.text == "__extension__" ||
                       t.text.rfind("__builtin_", 0) == 0;
            case CTokKind::Punct:
                return t.text == "(" || t.text == "*" || t.text == "&" || t.text == "-" || t.text == "+" ||
                       t.text == "!" || t.text == "~" || t.text == "++" || t.text == "--";
            default:
                return false;
        }
    }

    /** @brief Reports whether the token `k` ahead begins a type name (the contents of a cast or `sizeof`). */
    bool StartsTypeName(size_t k) const {
        const CToken &t = At(k);
        if (t.kind == CTokKind::Keyword) {
            return IsCTypeKeyword(t.text) || QualifierSet().count(t.text) > 0;
        }
        if (t.kind != CTokKind::Ident || !IsTypedefName(t.text)) return false;
        // A typedef name is only a type name here when the rest of the
        // parenthesized group can be one: `(size_t)x` casts, `(size)+1`
        // does not (someone shadowed the typedef).
        const CToken &next = At(k + 1);
        if (next.kind == CTokKind::Punct) {
            return next.text == ")" || next.text == "*" || next.text == "[" || next.text == "(";
        }
        return next.kind == CTokKind::Keyword && QualifierSet().count(next.text) > 0;
    }

    /** @brief Parses a type name: the specifier list plus an abstract declarator. */
    CNodePtr ParseTypeName() {
        CNodePtr n = Make(CNodeKind::TypeName, Pos());
        DeclSpec spec = ParseDeclSpecifiers();
        if (spec.tag) n->kids.push_back(std::move(spec.tag));
        DeclaratorInfo info = ParseDeclarator(spec.type, true);
        n->type = info.type;
        n->name = info.type.spelling;
        return Finish(std::move(n));
    }

    /** @brief Parses a unary expression: prefix operators, `sizeof`, `_Alignof`, and the GNU spellings. */
    CNodePtr ParseUnary() {
        const CPos start = Pos();
        // GNU labels-as-values: `&&done` takes a label's address. The
        // lexer has already made one `&&` token of it, so this cannot be
        // spotted as two `&` operators.
        if (IsPunct("&&") && At(1).kind == CTokKind::Ident) {
            CNodePtr n = Make(CNodeKind::Unary, start);
            n->name = "&&";
            n->str_value = "label";
            p_++;
            n->kids.push_back(MakeIdent());
            return Finish(std::move(n));
        }
        static const char *const kPrefixOps[] = {"++", "--", "&", "*", "+", "-", "~", "!"};
        for (const char *op : kPrefixOps) {
            if (!IsPunct(op)) continue;
            CNodePtr n = Make(CNodeKind::Unary, start);
            n->name = op;
            p_++;
            n->kids.push_back(ParseCastExpression());
            return Finish(std::move(n));
        }
        if (IsKw("sizeof") || IsKw("_Alignof") || IsKw("__alignof__") || IsKw("__alignof")) {
            const bool is_sizeof = IsKw("sizeof");
            p_++;
            if (IsPunct("(") && (StartsTypeName(1) || LooksLikeUnknownTypeCast())) {
                const size_t save = p_;
                p_++;
                CNodePtr type = ParseTypeName();
                if (EatPunct(")")) {
                    // `sizeof(int){0}` would be a compound literal, but
                    // `sizeof (T)` followed by anything else is the type
                    // form.
                    CNodePtr n = Make(is_sizeof ? CNodeKind::SizeOfType : CNodeKind::AlignOf, start);
                    n->kids.push_back(std::move(type));
                    return Finish(std::move(n));
                }
                p_ = save;
            }
            CNodePtr n = Make(CNodeKind::SizeOfExpr, start);
            n->kids.push_back(ParseUnary());
            return Finish(std::move(n));
        }
        if (IsKw("__extension__") || IsKw("__real__") || IsKw("__imag__")) {
            p_++;
            return ParseUnary();
        }
        return ParsePostfix();
    }

    /** @brief Wraps the identifier at the cursor in a node and consumes it. */
    CNodePtr MakeIdent() {
        CNodePtr n = Make(CNodeKind::Ident, Pos());
        n->name = Cur().text;
        n->name_pos = Cur().start;
        n->name_end_col = Cur().end.col;
        p_++;
        return Finish(std::move(n));
    }

    /** @brief Parses a postfix expression: calls, subscripts, member access and the postfix increments. */
    CNodePtr ParsePostfix() { return ParsePostfixSuffixes(ParsePrimary()); }

    /** @brief Applies every postfix suffix that follows an already-parsed operand. */
    CNodePtr ParsePostfixSuffixes(CNodePtr expr) {
        for (;;) {
            if (IsPunct("(")) {
                CNodePtr call = Make(CNodeKind::Call, expr->start);
                call->name = expr->kind == CNodeKind::Ident ? expr->name : std::string();
                call->name_pos = expr->name_pos;
                call->name_end_col = expr->name_end_col;
                p_++;
                call->kids.push_back(std::move(expr));
                if (!IsPunct(")")) {
                    for (;;) {
                        const size_t before = p_;
                        call->kids.push_back(ParseArgument());
                        if (!EatPunct(",")) break;
                        // A trailing comma before the `)`. Not legal in a
                        // function call, but every one of these is a
                        // variadic *macro* invocation, where it is -- and
                        // a server that cannot tell a macro from a
                        // function has no business calling it an error.
                        if (IsPunct(")")) break;
                        if (p_ == before) break;
                    }
                }
                Expect(")", "to close this argument list");
                expr = Finish(std::move(call));
                continue;
            }
            if (IsPunct("[")) {
                CNodePtr idx = Make(CNodeKind::Index, expr->start);
                p_++;
                idx->kids.push_back(std::move(expr));
                idx->kids.push_back(ParseExpression());
                Expect("]", "to close this subscript");
                expr = Finish(std::move(idx));
                continue;
            }
            if (IsPunct(".") || IsPunct("->")) {
                const bool arrow = IsPunct("->");
                CNodePtr m = Make(arrow ? CNodeKind::Arrow : CNodeKind::Member, expr->start);
                p_++;
                if (Cur().kind == CTokKind::Ident || Cur().kind == CTokKind::Keyword) {
                    m->name = Cur().text;
                    m->name_pos = Cur().start;
                    m->name_end_col = Cur().end.col;
                    p_++;
                } else {
                    m->name_pos = PrevEnd();
                    m->name_end_col = PrevEnd().col;
                    // `p->` with the line ending right there is someone
                    // mid-type, and inventing an error for it is the
                    // most annoying thing a server can do. `p-> == 3`
                    // on one line is a real mistake.
                    if (!AtEnd() && Pos().line == PrevEnd().line) {
                        Error(Pos(), Cur().end, "expected-member-name",
                              std::string("Expected a member name after `") + (arrow ? "->" : ".") + "`");
                    }
                }
                m->kids.push_back(std::move(expr));
                expr = Finish(std::move(m));
                continue;
            }
            if (IsPunct("++") || IsPunct("--")) {
                CNodePtr u = Make(CNodeKind::Unary, expr->start);
                u->name = Cur().text;
                u->str_value = "post";
                p_++;
                u->kids.push_back(std::move(expr));
                expr = Finish(std::move(u));
                continue;
            }
            break;
        }
        return expr;
    }

    /**
     * @brief Parses one call argument, which in C is sometimes a type.
     *
     * `va_arg(ap, int)`, `offsetof(struct s, m)` and every
     * `container_of`-shaped macro put a type where the grammar says an
     * expression goes. They are macros, so a compiler never sees this --
     * but an editor does, on every other line of real code, and reading
     * the type is far better than reporting it.
     */
    CNodePtr ParseArgument() {
        // `CLAY(CLAY_ID("x"), { .layout = ... })`: a braced group as an
        // argument, which only a macro can take.
        if (IsPunct("{")) return ParseInitializer();
        if (StartsTypeArgument()) {
            const size_t save = p_;
            const size_t errors_before = errors_ == nullptr ? 0 : errors_->size();
            CNodePtr type = ParseTypeName();
            if (IsPunct(",") || IsPunct(")")) return type;
            // Not a type after all (`sizeof(x) * count` reaches here):
            // rewind, including any complaint the attempt filed, and let
            // the expression parser have it.
            p_ = save;
            if (errors_ != nullptr) errors_->resize(errors_before);
        }
        return ParseAssignment();
    }

    /** @brief Reports whether an argument can only be a type name rather than an expression. */
    bool StartsTypeArgument() const {
        const CToken &t = Cur();
        // `sizeof` and the like start an expression even though they are
        // keywords; every other type keyword here can only be a type.
        if (t.kind == CTokKind::Keyword) {
            return IsCTypeKeyword(t.text) || (QualifierSet().count(t.text) > 0 && t.text != "typedef");
        }
        if (t.kind != CTokKind::Ident) return false;
        // `va_arg(ap, wchar_t *)`: a name this file has no typedef for,
        // but a `*` before the comma settles it -- `a * )` is not an
        // expression in any reading.
        size_t k = 1;
        while (At(k).kind == CTokKind::Keyword && QualifierSet().count(At(k).text) > 0) k++;
        size_t stars = 0;
        while (IsPunct("*", k)) {
            k++;
            stars++;
            while (At(k).kind == CTokKind::Keyword && QualifierSet().count(At(k).text) > 0) k++;
        }
        return stars > 0 && (IsPunct(",", k) || IsPunct(")", k));
    }

    /** @brief Parses a primary expression: a literal, a name, a parenthesized expression, `_Generic`, `({ ... })`. */
    CNodePtr ParsePrimary() {
        const CPos start = Pos();
        const CToken &t = Cur();
        switch (t.kind) {
            case CTokKind::Ident:
                // `PRIu64 "\n"`: a macro that expands to a string
                // literal, which is how every <inttypes.h> format string
                // in the world is written.
                if (At(1).kind == CTokKind::String) return ParseStringLiteral();
                return MakeIdent();
            case CTokKind::Number: {
                CNodePtr n = Make(CNodeKind::Number, start);
                n->name = t.text;
                p_++;
                return Finish(std::move(n));
            }
            case CTokKind::CharLit: {
                CNodePtr n = Make(CNodeKind::CharLit, start);
                n->name = t.text;
                p_++;
                return Finish(std::move(n));
            }
            case CTokKind::String:
                return ParseStringLiteral();
            default:
                break;
        }
        if (IsPunct("(")) {
            // GNU statement expression: `({ int t = a; t; })`.
            if (IsPunct("{", 1)) {
                CNodePtr n = Make(CNodeKind::StmtExpr, start);
                p_++;
                CNodePtr block = ParseCompound();
                n->body = std::move(block->body);
                Expect(")", "to close this statement expression");
                return Finish(std::move(n));
            }
            CNodePtr n = Make(CNodeKind::Paren, start);
            p_++;
            n->kids.push_back(ParseExpression());
            Expect(")", "to close this expression");
            return Finish(std::move(n));
        }
        if (IsKw("_Generic")) return ParseGeneric();
        if (IsKw("__builtin_va_arg") || IsKw("__builtin_offsetof") || IsKw("__builtin_types_compatible_p") ||
            IsKw("__builtin_choose_expr")) {
            CNodePtr n = Make(CNodeKind::Call, start);
            n->name = t.text;
            n->name_pos = t.start;
            n->name_end_col = t.end.col;
            p_++;
            if (IsPunct("(")) {
                p_++;
                while (!AtEnd() && !IsPunct(")")) {
                    const size_t before = p_;
                    n->kids.push_back(ParseArgument());
                    if (!EatPunct(",")) break;
                    if (p_ == before) break;
                }
                Expect(")", "to close this argument list");
            }
            return Finish(std::move(n));
        }
        // Keywords that reach here are a mistake somewhere. Report once
        // and leave a placeholder, so the rest of the statement still
        // parses into something.
        Error(start, Cur().end, "unexpected-token",
              AtEnd() ? "Unexpected end of file" : "Unexpected `" + Cur().text + "` in an expression");
        if (!AtEnd() && !IsPunct(";") && !IsPunct(")") && !IsPunct("}") && !IsPunct(",")) p_++;
        return Placeholder(start);
    }

    /**
     * @brief Parses a run of adjacent string literals as the single literal C says they are.
     *
     * Macro names inside the run are absorbed too (`"%3" PRId64 "\n"`):
     * an identifier next to a string literal is never anything else in
     * C, and treating one as a syntax error would fail on most of the
     * format strings ever written. The decoded text of such a run is
     * only the parts that were literal, which is exactly what the
     * format-argument check wants -- it stops counting conversions once
     * it cannot see the whole format.
     */
    CNodePtr ParseStringLiteral() {
        CNodePtr n = Make(CNodeKind::StrLit, Pos());
        bool macro_part = false;
        for (;;) {
            if (Cur().kind == CTokKind::String) {
                n->name += Cur().text;
                n->str_value += DecodeStringLiteral(Cur().text);
                p_++;
                continue;
            }
            if (Cur().kind == CTokKind::Ident && (At(1).kind == CTokKind::String || !n->name.empty())) {
                n->name += Cur().text;
                macro_part = true;
                p_++;
                // `__stringify(REV)` between two literals is a macro
                // call that produces one.
                if (IsPunct("(")) SkipBalanced();
                continue;
            }
            break;
        }
        if (macro_part) n->flags |= kCFlagAttrUnused;  // "do not read str_value as the whole format"
        return Finish(std::move(n));
    }

    /** @brief Parses a C11 `_Generic(control, type: value, ...)` selection. */
    CNodePtr ParseGeneric() {
        CNodePtr n = Make(CNodeKind::Generic, Pos());
        p_++;
        if (!Expect("(", "after `_Generic`")) return Finish(std::move(n));
        n->kids.push_back(ParseAssignment());
        while (EatPunct(",")) {
            const size_t before = p_;
            CNodePtr assoc = Make(CNodeKind::GenericAssoc, Pos());
            if (EatKw("default")) {
                // no type for the default association
            } else {
                assoc->kids.push_back(ParseTypeName());
            }
            Expect(":", "in this `_Generic` association");
            assoc->kids.push_back(ParseAssignment());
            n->kids.push_back(Finish(std::move(assoc)));
            if (p_ == before) break;
        }
        Expect(")", "to close this `_Generic`");
        return Finish(std::move(n));
    }

    /** @brief Decodes a string literal's escapes into the bytes it stands for. */
    static std::string DecodeStringLiteral(const std::string &raw) {
        const size_t open = raw.find('"');
        if (open == std::string::npos) return std::string();
        std::string out;
        for (size_t i = open + 1; i < raw.size(); i++) {
            const char c = raw[i];
            if (c == '"') break;
            if (c != '\\' || i + 1 >= raw.size()) {
                out += c;
                continue;
            }
            i++;
            switch (raw[i]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '0': out += '\0'; break;
                case 'a': out += '\a'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'v': out += '\v'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case '\'': out += '\''; break;
                default: out += raw[i]; break;
            }
        }
        return out;
    }
};

}  // namespace

namespace {

// --- Lexical and directive validation ---------------------------------

// Every suffix a C numeric constant may carry, lower-cased. Deliberately
// generous: the integer and floating sets, the C23 `wb`/`z` additions,
// the decimal-float and `_FloatN` ones, GCC's imaginary `i`/`j` and
// `q`, and MSVC's sized integer suffixes. A suffix outside this set is
// what a mistyped number looks like (`1,0xac0a` with the comma dropped
// becomes the number `10xac0a`), and reporting those is free.
const std::unordered_set<std::string> &NumberSuffixes() {
    static const std::unordered_set<std::string> kSet = {
        "",    "u",   "l",    "ul",  "lu",  "ll",  "ull", "llu", "lul", "f",   "lf",   "df",  "dd",
        "dl",  "d",   "i",    "j",   "if",  "fi",  "il",  "li",  "ij",  "q",   "iq",   "wb",  "uwb",
        "wbu", "z",   "uz",   "zu",  "f16", "f32", "f64", "f128", "f32x", "f64x", "bf16", "i8", "i16",
        "i32", "i64", "ui8",  "ui16", "ui32", "ui64", "lq", "fl",
        // Not a C suffix: `1b` and `3b` are GNU assembler local-label
        // references, and they reach here through macros like
        // `_ASM_EXTABLE(1b, 3b)` that stringize their arguments.
        "b",
    };
    return kSet;
}

/** @brief Lower-cases an ASCII string. */
std::string Lower(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/**
 * @brief Reports whether a preprocessing number is a valid C constant.
 *
 * The lexer is deliberately loose here (c_ast.h says so: a pp-number is
 * whatever looks like one), which is right for tokenizing and useless
 * for a reader. This is the other half: the mantissa has to be
 * well-formed for its base, and whatever is left over has to be a suffix
 * C actually has.
 */
bool IsValidCNumber(const std::string &text, std::string *out_reason) {
    if (text.empty()) return true;
    size_t i = 0;
    bool any_digit = false;
    bool is_float = false;
    const auto hex_digit = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    if (text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        i = 2;
        while (i < text.size() && (hex_digit(text[i]) || text[i] == '\'')) {
            i++;
            any_digit = true;
        }
        if (i < text.size() && text[i] == '.') {
            i++;
            is_float = true;
            while (i < text.size() && hex_digit(text[i])) {
                i++;
                any_digit = true;
            }
        }
        if (!any_digit) {
            *out_reason = "Hexadecimal constant has no digits";
            return false;
        }
        if (i < text.size() && (text[i] == 'p' || text[i] == 'P')) {
            i++;
            is_float = true;
            if (i < text.size() && (text[i] == '+' || text[i] == '-')) i++;
            size_t exp_digits = 0;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                i++;
                exp_digits++;
            }
            if (exp_digits == 0) {
                *out_reason = "Exponent has no digits";
                return false;
            }
        } else if (is_float) {
            *out_reason = "Hexadecimal floating constant needs a `p` exponent";
            return false;
        }
    } else if (text.size() > 1 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        i = 2;
        while (i < text.size() && (text[i] == '0' || text[i] == '1' || text[i] == '\'')) {
            i++;
            any_digit = true;
        }
        if (!any_digit) {
            *out_reason = "Binary constant has no digits";
            return false;
        }
    } else {
        const bool octal = text[0] == '0';
        size_t bad_octal_digit = std::string::npos;
        while (i < text.size() && ((text[i] >= '0' && text[i] <= '9') || text[i] == '\'')) {
            if (octal && (text[i] == '8' || text[i] == '9') && bad_octal_digit == std::string::npos) {
                bad_octal_digit = i;
            }
            i++;
        }
        if (i < text.size() && text[i] == '.') {
            i++;
            is_float = true;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') i++;
        }
        if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
            const size_t save = i;
            i++;
            if (i < text.size() && (text[i] == '+' || text[i] == '-')) i++;
            size_t exp_digits = 0;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                i++;
                exp_digits++;
            }
            if (exp_digits == 0) {
                i = save;  // not an exponent after all; `1east` is a bad suffix, not a bad exponent
            } else {
                is_float = true;
            }
        }
        if (octal && !is_float && bad_octal_digit != std::string::npos) {
            *out_reason = "Octal constant (it starts with `0`) has the digit `" +
                          std::string(1, text[bad_octal_digit]) + "` in it";
            return false;
        }
    }
    const std::string suffix = Lower(text.substr(i));
    if (NumberSuffixes().count(suffix) > 0) return true;
    *out_reason = "`" + text.substr(i) + "` is not a suffix a number can have";
    return false;
}

/** @brief Reports every numeric literal the lexer accepted that C would not. */
void CheckCNumbers(const std::vector<CToken> &tokens, std::vector<CSyntaxError> *out_errors) {
    for (const CToken &t : tokens) {
        if (t.kind != CTokKind::Number || !t.active) continue;
        std::string reason;
        if (IsValidCNumber(t.text, &reason)) continue;
        CSyntaxError e;
        e.start = t.start;
        e.end = t.end;
        e.code = "invalid-number";
        e.message = reason;
        out_errors->push_back(e);
    }
}

/**
 * @brief Reports the directives that are malformed as directives.
 *
 * Nothing here is about what a directive *means* -- that is arm
 * selection's business, and it deliberately guesses. These are the ones
 * where the line cannot be a directive at all: `#incldue`, `#include
 * stdio.h>`, `#define` with nothing after it.
 */
void CheckCDirectives(const std::vector<CDirective> &directives, std::vector<CSyntaxError> *out_errors) {
    for (const CDirective &d : directives) {
        const auto report = [&](const char *code, const std::string &message, int col, int end_col) {
            CSyntaxError e;
            e.start = {d.line, col};
            e.end = {d.line, end_col};
            e.code = code;
            e.message = message;
            out_errors->push_back(e);
        };
        const int kw_end = d.col + 1 + static_cast<int>(d.keyword.size());
        switch (d.kind) {
            case CDirectiveKind::Unknown:
                report("unknown-directive", "`" + d.keyword + "` is not a preprocessor directive", d.col, kw_end);
                break;
            case CDirectiveKind::Include:
            case CDirectiveKind::Embed:
                // An empty target is a macro-expanded include, which is
                // legal and unreadable; a non-empty one that is not
                // bracketed or quoted is a typo.
                if (d.header.empty() && !d.text.empty() && d.text[0] != '<' && d.text[0] != '"') {
                    report("bad-include", "`#" + d.keyword + "` needs `<header.h>` or `\"header.h\"`", d.col,
                           kw_end);
                } else if (d.header.empty() && !d.text.empty()) {
                    report("bad-include", "This `#" + d.keyword + "` target is never closed", d.col, kw_end);
                } else if (d.text.empty()) {
                    report("bad-include", "`#" + d.keyword + "` needs a header name", d.col, kw_end);
                }
                break;
            case CDirectiveKind::Define:
            case CDirectiveKind::Undef:
            case CDirectiveKind::Ifdef:
            case CDirectiveKind::Ifndef:
                if (d.name.empty()) {
                    report("directive-no-name", "`#" + d.keyword + "` needs a macro name", d.col, kw_end);
                }
                break;
            case CDirectiveKind::If:
            case CDirectiveKind::Elif:
                if (d.text.empty()) {
                    report("directive-no-condition", "`#" + d.keyword + "` needs a condition", d.col, kw_end);
                }
                break;
            default:
                break;
        }
    }
}

}  // namespace

CParseResult ParseC(const std::vector<std::string> &lines) {
    CParseResult result;
    result.tokens = TokenizeC(lines, &result.errors, &result.comments, &result.directives);
    SelectCPreprocessorArms(result.tokens, result.directives, &result.errors);

    // Bracket depth at the start of each line, over the tokens that
    // survived arm selection. Folding and the line-shape checks read it.
    result.depth_at_line.assign(lines.size() + 1, 0);
    {
        int depth = 0;
        size_t line = 0;
        for (const CToken &t : result.tokens) {
            while (line < result.depth_at_line.size() && static_cast<int>(line) <= t.start.line) {
                result.depth_at_line[line] = depth;
                line++;
            }
            if (!t.active || t.directive >= 0 || t.kind != CTokKind::Punct) continue;
            if (t.text == "{" || t.text == "(" || t.text == "[") depth++;
            if (t.text == "}" || t.text == ")" || t.text == "]") depth = depth > 0 ? depth - 1 : 0;
        }
        while (line < result.depth_at_line.size()) {
            result.depth_at_line[line] = depth;
            line++;
        }
    }

    CheckCDirectives(result.directives, &result.errors);
    CheckCNumbers(result.tokens, &result.errors);

    Parser parser(result.tokens, &result.errors);
    result.unit = parser.Run();
    result.typedef_names = parser.typedef_names();

    // Errors come out of three passes that each walk the file in their
    // own order; the client wants one list in document order.
    std::stable_sort(result.errors.begin(), result.errors.end(),
                     [](const CSyntaxError &a, const CSyntaxError &b) { return CPosLess(a.start, b.start); });
    return result;
}
