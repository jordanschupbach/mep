#include "cpp_format.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace cppfmt {
namespace {

// How deeply brackets may nest before the lexer refuses the file. This is not
// a style limit, it is a safety one: `gf` runs this formatter *inside the
// editor process* and the splitter recurses once per bracket level, so
// pathological input -- the 20k nested parens that really did segfault mep in
// both the R and the Python formatter before they grew the same cap -- would
// take the editor down with it rather than merely failing to format.
const int kMaxNest = 400;

// How deep the splitter's own recursion may go before it gives up and emits
// a range flat. The bracket cap above already bounds bracket nesting; this
// bounds the other axis, where a range recurses once per operator precedence
// level and then again into each operand.
const int kMaxSplitDepth = 64;

const size_t kNpos = static_cast<size_t>(-1);

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------

enum class Kind : unsigned char {
    Ident,        // identifier that is not a keyword
    Keyword,      // reserved word (IsKeyword below)
    Number,       // pp-number: 1, 0x1p-3, 1'000, 3.5f, 42_km
    String,       // "x", u8"x", R"d(x)d" -- any string literal, prefix included
    Char,         // 'c', L'c'
    HeaderName,   // <stdio.h>, lexed whole after #include so it is not `<` `/`
    Punct,        // everything else
    LineComment,  // // ... to end of line
    BlockComment  // /* ... */, possibly spanning lines
};

// What a `(`/`[`/`{` is *for*. Only the distinctions the spacing table and the
// line splitter actually act on exist here; everything else is Group/Init.
enum : unsigned char {
    kCtxNone = 0,
    kParenControl,  // the ( of if/while/for/switch -- gets a space before
    kParenCatch,    // the ( of catch: spaced like a control one, but it
                    // always declares, so `catch (E &e)` gets the `&` right
    kParenCall,     // foo(...), sizeof(...), a declarator's parameter list
    kParenDecl,     // a parameter list we are confident is a declarator's
    kParenGroup,    // (a + b)
    kParenCast,     // (int)x -- no space after the )
    kParenFnPtr,    // the ( of `GType (*fn)(...)` -- a declarator, spaced
    kBracketSub,    // a[i]
    kBracketLambda, // the [...] of a lambda
    kBracketAttr,   // the [[ of an attribute
    kBraceBlock,    // a compound statement, class/enum/namespace body, ...
    kBraceInit,     // a braced-init-list
    kColonLabel,    // the : of case/default/public/a goto label
    kColonTernary,  // the : of a ?:
    kColonCtorInit, // the : introducing a member initializer list
    kColonBase      // the : introducing a base-class list
};

struct Token {
    Kind kind = Kind::Punct;
    std::string text;
    size_t off = 0;  // byte offset into the source, for slicing raw text
    int line = 1;    // 1-based line the token starts on
    int col = 0;     // 0-based column it starts at
    int end_line = 1;
    int newlines_before = 0;
    bool pp = false;        // part of a preprocessor directive
    bool pp_lead = false;   // the `#` that opens one
    bool starts_line = false;

    // Annotations, all filled by Annotate() and all *cosmetic*: guessing any
    // of them wrong changes spacing, never the token stream (see cpp_format.h).
    bool unary = false;     // prefix operator, binds to what follows
    bool postfix = false;   // ++/-- applied to what precedes
    bool pointer = false;   // */&/&& used as a declarator: `int *p`
    bool tmpl_open = false;
    bool tmpl_close = false;
    bool op_name = false;   // part of an `operator@` name
    unsigned char ctx = kCtxNone;
    size_t match = kNpos;   // bracket partner, both directions

    // Escape hatches the spacing table consults first. The only place in C++
    // where whitespace between two tokens is *semantic* rather than cosmetic
    // is a `#define`'s macro name and the `(` after it -- `#define X(a)` is a
    // function-like macro and `#define X (a)` is an object-like one -- and
    // the re-lex-and-compare check cannot see the difference, because the
    // token stream is identical. So that one case is pinned here from the
    // original source instead of being re-derived.
    bool no_space_before = false;
    bool force_space_before = false;

    // For a `{` with ctx kBraceBlock: what kind of thing it opens, which is
    // all the line builder needs to know to indent case labels, leave
    // namespace bodies alone and keep `} while (...)` on one line.
    unsigned char brace_head = 0;  // 1 switch, 2 class, 3 namespace, 4 do, 5 enum
};

bool IsIdentStart(char c) {
    // Bytes >= 0x80 are let through so a UTF-8 identifier (or, far more
    // likely, a UTF-8 comment that got here some other way) survives as one
    // token rather than being chopped into punctuation.
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$' ||
           static_cast<unsigned char>(c) >= 0x80;
}

bool IsIdentCont(char c) {
    return IsIdentStart(c) || (c >= '0' && c <= '9');
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool IsKeyword(std::string_view s) {
    switch (s.size()) {
        case 2: return s == "do" || s == "if" || s == "or";
        case 3: return s == "and" || s == "asm" || s == "for" || s == "int" || s == "new" ||
                       s == "not" || s == "try" || s == "xor";
        case 4: return s == "auto" || s == "bool" || s == "case" || s == "char" || s == "else" ||
                       s == "enum" || s == "goto" || s == "long" || s == "this" || s == "true" ||
                       s == "void" || s == "compl";
        case 5: return s == "break" || s == "catch" || s == "class" || s == "const" ||
                       s == "false" || s == "float" || s == "short" || s == "throw" ||
                       s == "union" || s == "using" || s == "while" || s == "or_eq";
        case 6: return s == "and_eq" || s == "bitor" || s == "delete" || s == "double" ||
                       s == "export" || s == "extern" || s == "friend" || s == "inline" ||
                       s == "public" || s == "return" || s == "signed" || s == "sizeof" ||
                       s == "static" || s == "struct" || s == "switch" || s == "typeid" ||
                       s == "xor_eq" || s == "not_eq" || s == "concept";
        case 7: return s == "alignas" || s == "alignof" || s == "bitand" || s == "default" ||
                       s == "mutable" || s == "nullptr" || s == "private" || s == "typedef" ||
                       s == "virtual" || s == "wchar_t" || s == "requires";
        case 8: return s == "char8_t" || s == "co_await" || s == "co_yield" || s == "continue" ||
                       s == "decltype" || s == "explicit" || s == "noexcept" || s == "operator" ||
                       s == "register" || s == "template" || s == "typename" || s == "unsigned" ||
                       s == "volatile";
        case 9: return s == "char16_t" || s == "char32_t" || s == "co_return" ||
                       s == "consteval" || s == "constexpr" || s == "constinit" ||
                       s == "namespace" || s == "protected";
        case 10: return s == "const_cast";
        case 12: return s == "dynamic_cast" || s == "static_cast" || s == "thread_local";
        case 13: return s == "static_assert";
        case 16: return s == "reinterpret_cast";
        default: return false;
    }
}

// Keywords that name (part of) a type, and so make a following `*`/`&` a
// declarator rather than a binary operator: `char *p`, `auto &x`.
bool IsTypeKeyword(std::string_view s) {
    return s == "void" || s == "bool" || s == "char" || s == "char8_t" || s == "char16_t" ||
           s == "char32_t" || s == "wchar_t" || s == "short" || s == "int" || s == "long" ||
           s == "signed" || s == "unsigned" || s == "float" || s == "double" || s == "auto" ||
           s == "const" || s == "volatile" || s == "constexpr" || s == "static" ||
           s == "extern" || s == "inline" || s == "mutable" || s == "register" ||
           s == "typename" || s == "struct" || s == "class" || s == "union" || s == "enum" ||
           s == "thread_local" || s == "constinit" || s == "consteval";
}

// Keyword-shaped things that take a `(` with no space before it, the way a
// call does. `if (x)` gets a space; `sizeof(x)` does not.
bool IsFunctionLikeKeyword(std::string_view s) {
    return s == "sizeof" || s == "alignof" || s == "alignas" || s == "decltype" ||
           s == "typeid" || s == "noexcept" || s == "static_assert" || s == "static_cast" ||
           s == "const_cast" || s == "dynamic_cast" || s == "reinterpret_cast" ||
           s == "requires" || s == "__attribute__" || s == "__declspec" || s == "operator";
}

bool IsControlKeyword(std::string_view s) {
    return s == "if" || s == "while" || s == "for" || s == "switch" || s == "catch";
}

// `override`, `final` and the GNU/MSVC spellings are not keywords, but they
// behave like trailing qualifiers everywhere this cares.
bool IsTrailingQualifier(const Token &t) {
    if (t.kind == Kind::Keyword) {
        return t.text == "const" || t.text == "volatile" || t.text == "noexcept" ||
               t.text == "throw" || t.text == "requires";
    }
    return t.kind == Kind::Ident && (t.text == "override" || t.text == "final");
}



bool IsComment(const Token &t) {
    return t.kind == Kind::LineComment || t.kind == Kind::BlockComment;
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

// Punctuators, longest first so the scan is a plain prefix match. Digraphs
// (`<%`, `:>`, `%:`) are deliberately absent: `std::vector<::T>` needs `<` and
// `::` to lex separately, which is also what every compiler since C++11 does.
const char *const kPuncts[] = {
    "<=>", "...", "<<=", ">>=", "->*",
    "::", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "^=", "&=", "|=", ".*", "##",
    "{", "}", "[", "]", "(", ")", "<", ">", ";", ":", "?", ".", "+", "-",
    "*", "/", "%", "^", "&", "|", "~", "!", "=", ",", "#", "@", "\\",
};

class Lexer {
public:
    explicit Lexer(std::string_view src) : s_(src) {}

    bool Run();
    std::vector<Token> &tokens() { return tokens_; }
    const std::string &error() const { return error_; }
    int error_line() const { return error_line_; }

private:
    bool Fail(const std::string &msg) {
        error_ = msg;
        error_line_ = line_;
        return false;
    }
    bool ScanString(bool raw);
    bool ScanChar();
    void ScanNumber();
    bool ScanBlockComment();
    void ScanLineComment();
    // A ud-suffix is part of the literal: `"x"sv` is one token, and putting a
    // space in it would call a different operator (or none at all).
    void ScanUdSuffix() {
        if (i_ < s_.size() && IsIdentStart(s_[i_])) {
            while (i_ < s_.size() && IsIdentCont(s_[i_])) i_++;
        }
    }
    void Push(Kind k, size_t start, int start_line);

    std::string_view s_;
    size_t i_ = 0;
    int line_ = 1;
    size_t line_start_ = 0;
    int pending_newlines_ = 0;
    bool at_line_start_ = true;
    bool in_pp_ = false;
    int pp_token_index_ = 0;   // 0 = the `#`, 1 = the directive name, ...
    bool want_header_ = false; // next `<` is an #include's header-name
    int depth_ = 0;
    std::vector<Token> tokens_;
    std::string error_;
    int error_line_ = 0;
};

void Lexer::Push(Kind k, size_t start, int start_line) {
    Token t;
    t.kind = k;
    t.text.assign(s_.data() + start, i_ - start);
    t.off = start;
    t.col = static_cast<int>(start - line_start_);
    t.line = start_line;
    t.end_line = line_;
    t.newlines_before = pending_newlines_;
    t.starts_line = at_line_start_;
    t.pp = in_pp_;
    t.pp_lead = in_pp_ && pp_token_index_ == 0;
    tokens_.push_back(std::move(t));
    pending_newlines_ = 0;
    at_line_start_ = false;
    if (in_pp_) pp_token_index_++;
}

void Lexer::ScanLineComment() {
    while (i_ < s_.size() && s_[i_] != '\n') {
        // A `//` comment continued with a backslash swallows the next line
        // too. Rare, but getting it wrong would turn the next line of code
        // into something this formatter moves around freely.
        if (s_[i_] == '\\' && i_ + 1 < s_.size() &&
            (s_[i_ + 1] == '\n' || (s_[i_ + 1] == '\r' && i_ + 2 < s_.size() && s_[i_ + 2] == '\n'))) {
            i_ += (s_[i_ + 1] == '\n') ? 2 : 3;
            line_++;
            continue;
        }
        i_++;
    }
}

bool Lexer::ScanBlockComment() {
    i_ += 2;
    while (i_ + 1 < s_.size()) {
        if (s_[i_] == '*' && s_[i_ + 1] == '/') {
            i_ += 2;
            return true;
        }
        if (s_[i_] == '\n') line_++;
        i_++;
    }
    return Fail("unterminated /* comment");
}

bool Lexer::ScanString(bool raw) {
    if (raw) {
        // R"delim( ... )delim" -- the delimiter is up to 16 characters and
        // the body is completely uninterpreted, backslashes included.
        i_++;  // past the opening quote
        size_t dstart = i_;
        while (i_ < s_.size() && s_[i_] != '(' && i_ - dstart <= 16) i_++;
        if (i_ >= s_.size() || s_[i_] != '(') return Fail("malformed raw string literal");
        std::string close = ")";
        close.append(s_.data() + dstart, i_ - dstart);
        close += '"';
        i_++;
        while (i_ < s_.size()) {
            if (s_[i_] == '\n') {
                line_++;
                i_++;
                continue;
            }
            if (s_[i_] == ')' && s_.compare(i_, close.size(), close) == 0) {
                i_ += close.size();
                return true;
            }
            i_++;
        }
        return Fail("unterminated raw string literal");
    }
    i_++;
    while (i_ < s_.size()) {
        char c = s_[i_];
        if (c == '\\' && i_ + 1 < s_.size()) {
            if (s_[i_ + 1] == '\n') line_++;
            i_ += 2;
            continue;
        }
        if (c == '"') {
            i_++;
            return true;
        }
        if (c == '\n') return Fail("unterminated string literal");
        i_++;
    }
    return Fail("unterminated string literal");
}

bool Lexer::ScanChar() {
    i_++;
    while (i_ < s_.size()) {
        char c = s_[i_];
        if (c == '\\' && i_ + 1 < s_.size()) {
            if (s_[i_ + 1] == '\n') line_++;
            i_ += 2;
            continue;
        }
        if (c == '\'') {
            i_++;
            return true;
        }
        if (c == '\n') return Fail("unterminated character literal");
        i_++;
    }
    return Fail("unterminated character literal");
}

void Lexer::ScanNumber() {
    // A preprocessing-number, which is deliberately looser than any real
    // numeric literal: it is whatever the compiler's own lexer would hand the
    // parser, so `0x1p-3`, `1'000'000`, `3.5f` and the user-defined `42_km`
    // all come out as one token without this having to know what they mean.
    if (s_[i_] == '.') i_++;
    while (i_ < s_.size()) {
        char c = s_[i_];
        if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && i_ + 1 < s_.size() &&
            (s_[i_ + 1] == '+' || s_[i_ + 1] == '-')) {
            i_ += 2;
            continue;
        }
        if (c == '\'' && i_ + 1 < s_.size() && IsIdentCont(s_[i_ + 1]) && i_ > 0 &&
            IsIdentCont(s_[i_ - 1])) {
            // A C++14 digit separator, which only ever sits between two
            // digits. Anywhere else a quote starts a character literal.
            i_ += 2;
            continue;
        }
        if (IsIdentCont(c) || c == '.') {
            i_++;
            continue;
        }
        break;
    }
}

bool Lexer::Run() {
    const size_t n = s_.size();
    while (i_ < n) {
        // Whitespace, newlines and line splices between tokens.
        while (i_ < n) {
            char c = s_[i_];
            if (c == '\n') {
                line_++;
                pending_newlines_++;
                at_line_start_ = true;
                i_++;
                line_start_ = i_;
                if (in_pp_) {
                    in_pp_ = false;
                    want_header_ = false;
                }
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                i_++;
                continue;
            }
            if (c == '\\' && i_ + 1 < n &&
                (s_[i_ + 1] == '\n' || (s_[i_ + 1] == '\r' && i_ + 2 < n && s_[i_ + 2] == '\n'))) {
                // Inside a directive this is the ordinary multi-line `#define`
                // and the directive continues. Outside one -- GCC's own
                // <mmintrin.h> does it, so refusing outright is too strict --
                // it splices the two lines together before anything else
                // happens. That only matters when the splice is *tight* on
                // both sides, because then it welds two spellings into one
                // token that this formatter would be free to pull apart
                // again; with whitespace on either side it is whitespace.
                bool tight = i_ > 0 && !std::isspace(static_cast<unsigned char>(s_[i_ - 1]));
                size_t after = i_ + ((s_[i_ + 1] == '\n') ? 2 : 3);
                tight = tight && after < n &&
                        !std::isspace(static_cast<unsigned char>(s_[after]));
                if (!in_pp_ && tight) {
                    return Fail("backslash line continuation joins two tokens");
                }
                i_ = after;
                line_++;
                continue;
            }
            break;
        }
        if (i_ >= n) break;

        const size_t start = i_;
        const int start_line = line_;
        const char c = s_[i_];

        if (c == '/' && i_ + 1 < n && s_[i_ + 1] == '/') {
            ScanLineComment();
            Push(Kind::LineComment, start, start_line);
            continue;
        }
        if (c == '/' && i_ + 1 < n && s_[i_ + 1] == '*') {
            if (!ScanBlockComment()) return false;
            Push(Kind::BlockComment, start, start_line);
            // A directive is a sequence of tokens up to the first newline;
            // a comment that spans one ends it as far as this is concerned.
            if (in_pp_ && line_ != start_line) {
                in_pp_ = false;
                want_header_ = false;
            }
            continue;
        }
        if (c == '#' && at_line_start_ && !in_pp_) {
            in_pp_ = true;
            pp_token_index_ = 0;
        }
        if (want_header_ && c == '<') {
            size_t j = i_ + 1;
            while (j < n && s_[j] != '>' && s_[j] != '\n') j++;
            if (j < n && s_[j] == '>') {
                i_ = j + 1;
                want_header_ = false;
                Push(Kind::HeaderName, start, start_line);
                continue;
            }
        }
        if (IsIdentStart(c)) {
            while (i_ < n && IsIdentCont(s_[i_])) i_++;
            std::string_view word(s_.data() + start, i_ - start);
            bool raw = !word.empty() && word.back() == 'R';
            std::string_view stem = raw ? word.substr(0, word.size() - 1) : word;
            bool str_prefix = stem.empty() || stem == "L" || stem == "u" || stem == "U" ||
                              stem == "u8";
            if (i_ < n && s_[i_] == '"' && str_prefix && word.size() <= 3) {
                if (!ScanString(raw)) return false;
                ScanUdSuffix();
                Push(Kind::String, start, start_line);
                continue;
            }
            if (i_ < n && s_[i_] == '\'' && !raw && str_prefix && word.size() <= 2) {
                if (!ScanChar()) return false;
                ScanUdSuffix();
                Push(Kind::Char, start, start_line);
                continue;
            }
            Kind k = IsKeyword(word) ? Kind::Keyword : Kind::Ident;
            Push(k, start, start_line);
            if (in_pp_ && pp_token_index_ == 2 &&
                (word == "include" || word == "include_next" || word == "import")) {
                want_header_ = true;
            }
            continue;
        }
        if (IsDigit(c) || (c == '.' && i_ + 1 < n && IsDigit(s_[i_ + 1]))) {
            ScanNumber();
            Push(Kind::Number, start, start_line);
            continue;
        }
        if (c == '"') {
            if (!ScanString(false)) return false;
            ScanUdSuffix();
            Push(Kind::String, start, start_line);
            continue;
        }
        if (c == '\'') {
            if (!ScanChar()) return false;
            ScanUdSuffix();
            Push(Kind::Char, start, start_line);
            continue;
        }
        bool matched = false;
        for (const char *p : kPuncts) {
            size_t len = std::char_traits<char>::length(p);
            if (s_.compare(i_, len, p) == 0) {
                i_ += len;
                matched = true;
                break;
            }
        }
        if (!matched) {
            // A byte no C++ lexer would produce (a stray control character).
            // Refusing beats silently dropping it.
            return Fail("unexpected character in source");
        }
        if (s_[start] == '(' || s_[start] == '[' || s_[start] == '{') {
            if (++depth_ > kMaxNest) return Fail("brackets nested too deeply to format");
        } else if (s_[start] == ')' || s_[start] == ']' || s_[start] == '}') {
            if (depth_ > 0) depth_--;
        }
        Push(Kind::Punct, start, start_line);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Annotation
// ---------------------------------------------------------------------------
//
// Everything decided here is cosmetic. C++ cannot be disambiguated from a
// token stream -- `a < b` and `vector<int>`, `a * b` and `Foo *p`, `T{1}` and
// `if (c) {` are each two different things spelled the same way, and telling
// them apart properly needs name lookup, which needs the whole translation
// unit. So this guesses, and the guesses only ever move spaces: whichever way
// they go the same tokens come out in the same order, and Format()'s
// re-lex-and-compare proves it before anything is written back.

// Previous significant token, refusing to look across a preprocessor
// boundary: a `#define`'s tokens are not context for the code before it and
// vice versa.
size_t PrevTok(const std::vector<Token> &t, size_t i) {
    const bool pp = t[i].pp;
    if (pp && t[i].pp_lead) return kNpos;
    while (i > 0) {
        i--;
        if (IsComment(t[i])) continue;
        if (t[i].pp != pp) return kNpos;
        if (pp && t[i].pp_lead && !t[i].pp_lead) return kNpos;
        return i;
    }
    return kNpos;
}

size_t NextTok(const std::vector<Token> &t, size_t i) {
    const bool pp = t[i].pp;
    for (size_t j = i + 1; j < t.size(); j++) {
        if (IsComment(t[j])) continue;
        if (t[j].pp != pp) return kNpos;
        if (t[j].pp_lead) return kNpos;
        return j;
    }
    return kNpos;
}

bool EndsOperand(const Token &x) {
    switch (x.kind) {
        case Kind::Ident:
        case Kind::Number:
        case Kind::String:
        case Kind::Char:
        case Kind::HeaderName:
            return true;
        case Kind::Keyword:
            return x.text == "this" || x.text == "true" || x.text == "false" ||
                   x.text == "nullptr" || IsTypeKeyword(x.text);
        case Kind::Punct:
            if (x.tmpl_close) return true;
            if (x.text == "++" || x.text == "--") return x.postfix;
            // The `)` of `if (c)` ends the head, not a value: what follows is
            // a fresh statement, so `while (x) --n;` decrements rather than
            // post-decrementing the condition.
            if (x.text == ")") return x.ctx != kParenControl && x.ctx != kParenCatch;
            return x.text == "]" || x.text == "}" || x.text == "...";
        default:
            return false;
    }
}

// A token that can legally follow the `>` of a template argument list. Used
// only to make the `<` guess less wrong; a literal right after the `>` is the
// giveaway that this was a comparison all along.
bool CanFollowTemplateClose(const Token &x) {
    if (x.kind == Kind::Number || x.kind == Kind::String || x.kind == Kind::Char) return false;
    if (x.kind == Kind::Ident || x.kind == Kind::Keyword) return true;
    static const char *const ok[] = {"(", ")", "[", "]", "{", "}", ";", ",", ":", "::",
                                     "*",  "&", "&&", "=", "...", ">", ">>", "->", ".", "<"};
    for (const char *p : ok) {
        if (x.text == p) return true;
    }
    return false;
}

// Is the `<` at `i` a template argument list? Scans forward for a plausible
// `>`, refusing on anything that cannot appear between them. The comparison
// operators in the refusal list are what keeps `if (a < b && c > d)` from
// being read as a template -- which is exactly the shape that fools every
// token-level formatter that does not check for it.
bool TryTemplate(const std::vector<Token> &t, size_t i) {
    size_t prev = PrevTok(t, i);
    if (prev == kNpos) return false;
    const Token &p = t[prev];
    if (p.kind == Kind::Keyword && p.text == "template") return true;
    if (p.kind == Kind::Keyword && p.text == "operator") return false;
    if (p.op_name) return false;
    bool head_ok = p.kind == Kind::Ident || p.tmpl_close ||
                   (p.kind == Kind::Keyword &&
                    (p.text == "typename" || p.text == "class" || p.text == "struct" ||
                     p.text == "enum" || p.text == "decltype" || p.text == "requires"));
    if (!head_ok) return false;

    int angle = 1;
    int guard = 0;
    for (size_t j = i + 1; j < t.size() && guard < 512; j++, guard++) {
        const Token &x = t[j];
        if (x.kind == Kind::LineComment) return false;
        if (x.kind == Kind::BlockComment) continue;
        if (x.pp != t[i].pp || x.pp_lead) return false;
        if (x.kind != Kind::Punct) continue;
        const std::string &s = x.text;
        if (s == "(" || s == "[" || s == "{") {
            // Skip a balanced group; anything unbalanced means this was not a
            // template argument list.
            int d = 0;
            size_t k = j;
            for (; k < t.size(); k++) {
                if (t[k].kind != Kind::Punct) continue;
                if (t[k].text == "(" || t[k].text == "[" || t[k].text == "{") d++;
                else if (t[k].text == ")" || t[k].text == "]" || t[k].text == "}") {
                    if (--d == 0) break;
                }
            }
            if (k >= t.size()) return false;
            j = k;
            continue;
        }
        if (s == ")" || s == "]" || s == "}" || s == ";") return false;
        if (s == "&&" || s == "||" || s == "==" || s == "!=" || s == "<=" || s == ">=" ||
            s == "?" || s == ">>=" || s == "<<=") {
            return false;
        }
        if (s == "<") {
            angle++;
            continue;
        }
        if (s == ">") {
            if (--angle == 0) {
                size_t after = NextTok(t, j);
                return after == kNpos || CanFollowTemplateClose(t[after]);
            }
            continue;
        }
        if (s == ">>") {
            // `>>` closes two levels. Seen from the inner `<` of
            // `map<int, vector<int>>` only one of them is ours, and the other
            // belongs to an enclosing list -- which still means ours closes
            // here, so `angle` going negative is not a refusal.
            angle -= 2;
            if (angle <= 0) {
                size_t after = NextTok(t, j);
                return after == kNpos || CanFollowTemplateClose(t[after]);
            }
            continue;
        }
    }
    return false;
}

struct Frame {
    char open = 0;  // '(', '[', '{', '<', or 0 for the file scope
    size_t idx = kNpos;
    unsigned char kind = kCtxNone;

    // True when statements here can be declarations *and only* declarations
    // (file scope, a namespace body, a class body), which is what makes a
    // bare `Name(` there a declarator rather than a call.
    bool decl_only = true;
    bool is_switch = false;
    bool is_class = false;
    bool is_namespace = false;
    bool for_header = false;

    // Per-statement state, reset at every `;`, `{`, `}` and label.
    size_t stmt_start = 0;
    size_t elem_start = 0;  // stmt_start, or just after the last top-level comma
    bool seen_assign = false;
    bool lambda_pending = false;
    int ternary = 0;
    std::string head;  // first structural keyword of the current statement
    // The first class/struct/union/enum/namespace at the statement's top
    // level. Separate from `head` because `typedef struct { ... } X;` gets
    // there first and would otherwise hide the aggregate that actually says
    // what the `{` opens.
    std::string agg;
};

// Do the tokens in [from, to) look like the type and name that precede a
// declarator -- `const std::string &`, `static Foo *`, `int` -- rather than
// the left operand of an expression?
bool DeclPrefix(const std::vector<Token> &t, size_t from, size_t to, int *name_groups,
                bool *has_type_keyword = nullptr) {
    int groups = 0;
    bool after_scope = false;  // the previous token was `::`, so this name continues one
    if (has_type_keyword) *has_type_keyword = false;
    for (size_t j = from; j < to; j++) {
        const Token &x = t[j];
        if (IsComment(x)) continue;
        if (x.tmpl_open) {
            if (x.match == kNpos || x.match >= to) return false;
            j = x.match;
            continue;
        }
        if (x.kind == Kind::Ident || (x.kind == Kind::Keyword && IsTypeKeyword(x.text))) {
            if (x.kind == Kind::Keyword && has_type_keyword) *has_type_keyword = true;
            if (!after_scope) groups++;
            after_scope = false;
            continue;
        }
        if (x.kind == Kind::Punct &&
            (x.text == "::" || x.text == "*" || x.text == "&" || x.text == "&&" ||
             x.text == "..." || x.text == "~" || x.text == "[" || x.text == "]")) {
            after_scope = x.text == "::";  // keeps `a::b` one group
            continue;
        }
        return false;
    }
    if (name_groups) *name_groups = groups;
    return true;
}

// Is the `(` at `i` the parenthesized declarator of a function pointer or a
// reference-to-array -- `GType (*fn)(args)`, `int (&ref)[4]` -- rather than a
// call? The shape is unmistakable: it opens on a `*`/`&`, holds nothing but a
// name, and is immediately followed by another `(` or a `[`.
bool IsFnPtrParen(const std::vector<Token> &t, size_t i) {
    size_t j = NextTok(t, i);
    if (j == kNpos) return false;
    if (t[j].kind != Kind::Punct || (t[j].text != "*" && t[j].text != "&" && t[j].text != "&&")) {
        return false;
    }
    while (j != kNpos && t[j].kind == Kind::Punct &&
           (t[j].text == "*" || t[j].text == "&" || t[j].text == "&&" || t[j].text == "::")) {
        j = NextTok(t, j);
    }
    while (j != kNpos && (t[j].kind == Kind::Ident ||
                          (t[j].kind == Kind::Keyword && IsTypeKeyword(t[j].text)))) {
        j = NextTok(t, j);
    }
    if (j == kNpos || t[j].text != ")") return false;
    size_t after = NextTok(t, j);
    return after != kNpos && (t[after].text == "(" || t[after].text == "[");
}

void MarkOperatorName(std::vector<Token> &t, size_t i) {
    size_t j = NextTok(t, i);
    if (j == kNpos) return;
    Token &a = t[j];
    if (a.kind == Kind::Keyword && (a.text == "new" || a.text == "delete")) {
        size_t k = NextTok(t, j);
        if (k != kNpos && t[k].text == "[") {
            size_t l = NextTok(t, k);
            if (l != kNpos && t[l].text == "]") {
                t[k].op_name = true;
                t[l].op_name = true;
            }
        }
        return;
    }
    if (a.kind != Kind::Punct && a.kind != Kind::String) return;
    a.op_name = true;
    if (a.text == "(" || a.text == "[") {
        size_t k = NextTok(t, j);
        const char *want = a.text == "(" ? ")" : "]";
        if (k != kNpos && t[k].text == want) t[k].op_name = true;
    }
    if (a.kind == Kind::String) {
        // operator""_suffix: the suffix identifier binds to the "".
        size_t k = NextTok(t, j);
        if (k != kNpos && t[k].kind == Kind::Ident) t[k].op_name = true;
    }
}

// Contents that make a `(...)` a C-style cast rather than a grouping paren.
// Deliberately conservative: a type keyword or a trailing `*`/`&` has to be
// in there, so `(Foo)x` keeps its space and only the unmistakable casts lose
// it. Guessing "cast" on a grouping paren would run two operands together
// (`(a)-b`), which reads as a different expression even though it is not one.
bool LooksLikeCast(const std::vector<Token> &t, size_t open, size_t close) {
    if (close <= open + 1) return false;
    bool has_type = false;
    size_t last = kNpos;
    for (size_t j = open + 1; j < close; j++) {
        const Token &x = t[j];
        if (IsComment(x)) continue;
        last = j;
        if (x.tmpl_open) {
            if (x.match == kNpos || x.match >= close) return false;
            j = x.match;
            has_type = true;
            continue;
        }
        if (x.kind == Kind::Keyword) {
            if (!IsTypeKeyword(x.text)) return false;
            has_type = true;
            continue;
        }
        if (x.kind == Kind::Ident) continue;
        if (x.kind == Kind::Punct &&
            (x.text == "*" || x.text == "&" || x.text == "::" || x.text == "[" ||
             x.text == "]")) {
            continue;
        }
        return false;
    }
    if (last != kNpos && (t[last].text == "*" || t[last].text == "&")) has_type = true;
    return has_type;
}

void Annotate(std::vector<Token> &t) {
    std::vector<Frame> st;
    st.push_back(Frame{});  // file scope
    const size_t n = t.size();

    auto reset_stmt = [&](size_t next) {
        Frame &f = st.back();
        f.stmt_start = next;
        f.elem_start = next;
        f.seen_assign = false;
        f.lambda_pending = false;
        f.ternary = 0;
        f.head.clear();
        f.agg.clear();
    };

    for (size_t i = 0; i < n; i++) {
        Token &tk = t[i];
        if (IsComment(tk)) continue;
        if (tk.op_name) {
            // Already claimed as part of an `operator@` name; it is spelling,
            // not structure, so it never opens a bracket frame.
            continue;
        }
        Frame *f = &st.back();
        if (i == 0 || f->stmt_start > i) reset_stmt(i);

        if (tk.pp) {
            // Directives are re-spaced but never restructured, so all they
            // need is the local operator/pointer shape plus the one place
            // where whitespace is load-bearing.
            if (tk.pp_lead) {
                size_t name = NextTok(t, i);
                if (name != kNpos && t[name].kind != Kind::Punct) {
                    size_t after = NextTok(t, name);
                    bool define = t[name].text == "define";
                    if (define && after != kNpos) {
                        size_t mac = after;
                        size_t paren = NextTok(t, mac);
                        if (paren != kNpos && t[paren].text == "(") {
                            bool adjacent = t[mac].off + t[mac].text.size() == t[paren].off;
                            t[paren].no_space_before = adjacent;
                            t[paren].force_space_before = !adjacent;
                            if (adjacent) {
                                // The body begins after the parameter list;
                                // without a space it reads as a call.
                                size_t cl = paren;
                                int d = 0;
                                for (; cl < t.size() && t[cl].pp; cl++) {
                                    if (t[cl].text == "(") d++;
                                    else if (t[cl].text == ")" && --d == 0) break;
                                }
                                if (cl < t.size() && t[cl].text == ")") {
                                    size_t body = NextTok(t, cl);
                                    if (body != kNpos) t[body].force_space_before = true;
                                }
                            }
                        }
                    }
                }
            }
            size_t p = PrevTok(t, i);
            if (tk.kind == Kind::Punct) {
                const std::string &s = tk.text;
                if (s == "*" || s == "&" || s == "&&" || s == "+" || s == "-" || s == "!" ||
                    s == "~" || s == "++" || s == "--") {
                    bool operand = p != kNpos && EndsOperand(t[p]);
                    if (!operand) {
                        tk.unary = true;
                    } else if (s == "++" || s == "--") {
                        tk.postfix = true;
                    }
                }
            }
            continue;
        }

        if (tk.kind == Kind::Keyword && tk.text == "operator") {
            MarkOperatorName(t, i);
        }

        const size_t p = PrevTok(t, i);
        const bool after_operand = p != kNpos && EndsOperand(t[p]);

        if (tk.kind == Kind::Keyword && f->agg.empty() &&
            (tk.text == "class" || tk.text == "struct" || tk.text == "union" ||
             tk.text == "enum" || tk.text == "namespace")) {
            f->agg = tk.text;
        }
        if (tk.kind == Kind::Keyword && f->head.empty()) {
            static const char *const heads[] = {"namespace", "class",  "struct", "union",
                                                "enum",      "extern", "typedef", "using",
                                                "if",        "for",    "while",  "switch",
                                                "do",        "try",    "catch",  "else",
                                                "return",    "case",   "default", "public",
                                                "private",   "protected", "template", "friend",
                                                "static",    "inline", "const",  "explicit",
                                                "virtual",   "constexpr", "throw", "delete",
                                                "new",       "goto",   "break",  "continue"};
            for (const char *h : heads) {
                if (tk.text == h) {
                    // `template <...>` is a prefix; the structural keyword is
                    // whatever comes after it, so do not let it win.
                    if (tk.text != "template") f->head = tk.text;
                    break;
                }
            }
        }

        if (tk.kind != Kind::Punct) continue;
        const std::string &s = tk.text;

        // --- template angle brackets -------------------------------------
        if (s == "<" && TryTemplate(t, i)) {
            tk.tmpl_open = true;
            Frame nf;
            nf.open = '<';
            nf.idx = i;
            nf.decl_only = false;
            nf.stmt_start = i + 1;
            nf.elem_start = i + 1;
            st.push_back(nf);
            continue;
        }
        if ((s == ">" || s == ">>") && st.size() > 1) {
            int want = (s == ">>") ? 2 : 1;
            int have = 0;
            for (size_t k = st.size(); k-- > 1 && have < want;) {
                if (st[k].open != '<') break;
                have++;
            }
            if (have == want) {
                tk.tmpl_close = true;
                // A `template <...>` header ends the statement: what follows
                // is the declaration it introduces, and reading that from its
                // own start is what makes its parameter list a declarator --
                // and so its `&` a reference rather than a bitwise and.
                size_t open_idx = st.back().idx;
                size_t kw = PrevTok(t, open_idx);
                bool header = want == 1 && kw != kNpos && t[kw].kind == Kind::Keyword &&
                              t[kw].text == "template";
                for (int k = 0; k < want; k++) {
                    t[st.back().idx].match = i;
                    tk.match = st.back().idx;  // the innermost `<` it closes
                    st.pop_back();
                }
                f = &st.back();
                if (header) reset_stmt(i + 1);
                continue;
            }
        }

        // --- brackets ----------------------------------------------------
        if (s == "(" || s == "[" || s == "{") {
            Frame nf;
            nf.open = s[0];
            nf.idx = i;
            nf.stmt_start = i + 1;
            nf.elem_start = i + 1;
            nf.decl_only = false;
            if (s == "(") {
                // `operator bool()`, `operator new(size_t)`: the paren belongs
                // to the declarator, not to the keyword spelling the name.
                size_t q = p == kNpos ? kNpos : PrevTok(t, p);
                bool after_op_name =
                    q != kNpos && t[q].kind == Kind::Keyword && t[q].text == "operator";
                if (after_op_name) {
                    tk.ctx = kParenDecl;
                    nf.decl_only = true;
                } else if (p != kNpos && IsFnPtrParen(t, i)) {
                    // `GType (*fn)(args)` and `int (&ref)[4]`: the paren is
                    // part of the declarator, not a call, and reads as one
                    // only with a space in front of it.
                    tk.ctx = kParenFnPtr;
                    nf.decl_only = true;
                } else if (p != kNpos && t[p].text == ")" && t[p].match != kNpos &&
                           t[t[p].match].ctx == kParenFnPtr) {
                    tk.ctx = kParenDecl;  // the parameters of that pointer
                    nf.decl_only = true;
                } else if (p != kNpos && t[p].kind == Kind::Keyword &&
                           IsControlKeyword(t[p].text)) {
                    tk.ctx = t[p].text == "catch" ? kParenCatch : kParenControl;
                    nf.for_header = t[p].text == "for";
                } else if (p != kNpos && t[p].kind == Kind::Keyword &&
                           IsFunctionLikeKeyword(t[p].text)) {
                    tk.ctx = kParenCall;
                } else if (p != kNpos && t[p].kind == Kind::Keyword &&
                           !(t[p].text == "this" || t[p].text == "true" ||
                             t[p].text == "false" || t[p].text == "nullptr")) {
                    tk.ctx = kParenGroup;
                } else if (p != kNpos && (t[p].kind == Kind::Ident || t[p].text == ")" ||
                                          t[p].text == "]" || t[p].tmpl_close || t[p].op_name)) {
                    tk.ctx = kParenCall;
                    if (!f->seen_assign) {
                        int groups = 0;
                        bool declish = t[p].op_name ||
                                       (t[p].kind == Kind::Ident &&
                                        DeclPrefix(t, f->stmt_start, i, &groups) &&
                                        (groups >= 2 || f->decl_only));
                        if (declish) {
                            tk.ctx = kParenDecl;
                            nf.decl_only = true;
                        }
                    }
                } else {
                    tk.ctx = kParenGroup;
                }
            } else if (s == "[") {
                size_t nx = NextTok(t, i);
                if (p != kNpos && t[p].text == "[" && t[p].ctx == kBracketAttr) {
                    tk.ctx = kBracketAttr;
                } else if (nx != kNpos && t[nx].text == "[" && !after_operand) {
                    tk.ctx = kBracketAttr;
                } else if (after_operand) {
                    tk.ctx = kBracketSub;
                } else {
                    tk.ctx = kBracketLambda;
                }
            } else {
                // `{`: block or braced-init-list. See cpp_format.h -- getting
                // this wrong costs layout, never meaning.
                bool block;
                const bool by_lambda = f->lambda_pending;
                if (f->lambda_pending) {
                    block = true;
                    f->lambda_pending = false;
                } else if (p == kNpos) {
                    block = true;
                } else if (t[p].kind == Kind::Punct &&
                           (t[p].text == "=" || t[p].text == "," || t[p].text == "(" ||
                            t[p].text == "[" || t[p].text == "?" ||
                            (t[p].text == "{" && t[p].ctx == kBraceInit))) {
                    block = false;
                } else if (t[p].kind == Kind::Keyword &&
                           (t[p].text == "return" || t[p].text == "new" ||
                            t[p].text == "delete" || t[p].text == "throw" ||
                            t[p].text == "case" || t[p].text == "co_return" ||
                            t[p].text == "co_yield" || t[p].text == "co_await")) {
                    block = false;
                } else if (t[p].text == ")") {
                    size_t op = t[p].match;
                    if (op != kNpos && (t[op].ctx == kParenControl || t[op].ctx == kParenCatch)) {
                        block = true;
                    } else {
                        block = !f->seen_assign;
                    }
                } else if (t[p].text == "]") {
                    size_t op = t[p].match;
                    block = op != kNpos && t[op].ctx == kBracketLambda;
                } else if (t[p].kind == Kind::Keyword) {
                    block = !(t[p].text == "sizeof" || t[p].text == "alignof");
                } else if (t[p].kind == Kind::String) {
                    block = true;  // extern "C" {
                } else if (t[p].kind == Kind::Ident || t[p].tmpl_close || t[p].text == "::") {
                    block = !f->agg.empty() || f->head == "extern";
                } else if (f->open == '(' || f->open == '[' ||
                           (f->open == '{' && f->kind == kBraceInit)) {
                    block = false;
                } else {
                    block = true;
                }
                if ((f->open == '(' || f->open == '[') && !by_lambda) {
                    // Inside an expression nothing but a lambda body is a
                    // block, and a lambda body was already caught above --
                    // either by the pending flag its `[...]` set, or, for a
                    // capture list with no parameter list, right here.
                    if (!(p != kNpos && t[p].text == "]" && t[p].match != kNpos &&
                          t[t[p].match].ctx == kBracketLambda)) {
                        block = false;
                    }
                }
                tk.ctx = block ? kBraceBlock : kBraceInit;
                nf.kind = tk.ctx;
                if (block) {
                    nf.is_switch = f->head == "switch";
                    tk.brace_head = nf.is_switch ? 1 : 0;
                    nf.is_class = f->agg == "class" || f->agg == "struct" ||
                                  f->agg == "union" || f->agg == "enum";
                    nf.is_namespace = f->agg == "namespace" ||
                                      (f->head == "extern" && p != kNpos &&
                                       t[p].kind == Kind::String);
                    nf.decl_only = nf.is_class || nf.is_namespace;
                    if (nf.is_namespace) tk.brace_head = 3;
                    else if (f->agg == "enum") tk.brace_head = 5;
                    else if (nf.is_class) tk.brace_head = 2;
                    else if (f->head == "do") tk.brace_head = 4;
                }
            }
            nf.kind = tk.ctx;
            if (static_cast<int>(st.size()) < kMaxNest) st.push_back(nf);
            continue;
        }

        if (s == ")" || s == "]" || s == "}") {
            char want = s == ")" ? '(' : (s == "]" ? '[' : '{');
            // Drop any template frames that never found their `>`: the guess
            // was wrong, so un-guess it rather than leave the stack skewed.
            while (st.size() > 1 && st.back().open == '<') {
                t[st.back().idx].tmpl_open = false;
                st.pop_back();
            }
            if (st.size() > 1 && st.back().open == want) {
                size_t open = st.back().idx;
                bool lambda = st.back().kind == kBracketLambda;
                t[open].match = i;
                tk.match = open;
                tk.ctx = t[open].ctx;
                st.pop_back();
                f = &st.back();
                if (want == '(' && t[open].ctx == kParenGroup && !after_operand) {
                    size_t before = PrevTok(t, open);
                    size_t after = NextTok(t, i);
                    bool lhs_ok = before == kNpos || !EndsOperand(t[before]);
                    bool rhs_ok = after != kNpos && (EndsOperand(t[after]) ||
                                                     t[after].text == "(" ||
                                                     t[after].text == "~" ||
                                                     t[after].text == "!" ||
                                                     t[after].text == "-" ||
                                                     t[after].text == "+" ||
                                                     t[after].text == "*" ||
                                                     t[after].text == "&" ||
                                                     t[after].text == "++" ||
                                                     t[after].text == "--");
                    if (lhs_ok && rhs_ok && LooksLikeCast(t, open, i)) {
                        t[open].ctx = kParenCast;
                        tk.ctx = kParenCast;
                    }
                }
                if (lambda) f->lambda_pending = true;
                if (want == '{' && t[open].ctx == kBraceBlock) reset_stmt(i + 1);
                if (want == '(' &&
                    (t[open].ctx == kParenControl || t[open].ctx == kParenCatch)) {
                    // The body is a fresh element -- `if (c) Foo *p = q;`
                    // declares -- but the statement's head keyword still has
                    // to survive, because `switch (x) {` reads it to know the
                    // block it is about to open is a switch.
                    f->elem_start = i + 1;
                    f->seen_assign = false;
                    f->lambda_pending = false;
                    f->ternary = 0;
                }
            }
            continue;
        }

        // --- statement structure -----------------------------------------
        if (s == ";") {
            if (!f->for_header) reset_stmt(i + 1);
            continue;
        }
        if (s == ",") {
            f->elem_start = i + 1;
            f->lambda_pending = false;
            continue;
        }
        if (s == "?") {
            f->ternary++;
            continue;
        }
        if (s == ":") {
            if (f->ternary > 0) {
                f->ternary--;
                tk.ctx = kColonTernary;
                continue;
            }
            bool label = false;
            size_t start = f->stmt_start;
            if (f->head == "case" || f->head == "default") {
                label = true;
            } else if ((f->head == "public" || f->head == "private" ||
                        f->head == "protected")) {
                label = true;
            } else if (start < i) {
                // A goto label is a lone identifier followed by the colon.
                size_t only = kNpos;
                bool single = true;
                for (size_t j = start; j < i; j++) {
                    if (IsComment(t[j])) continue;
                    if (only != kNpos) {
                        single = false;
                        break;
                    }
                    only = j;
                }
                label = single && only != kNpos && t[only].kind == Kind::Ident &&
                        f->open == '{' && f->kind == kBraceBlock && !f->is_class;
            }
            if (label) {
                tk.ctx = kColonLabel;
                reset_stmt(i + 1);
                continue;
            }
            if (!f->agg.empty() && f->agg != "namespace") {
                tk.ctx = kColonBase;
            } else if (p != kNpos && (t[p].text == ")" || IsTrailingQualifier(t[p])) &&
                       f->open != '(' && !f->for_header) {
                tk.ctx = kColonCtorInit;
            }
            continue;
        }
        if (s == "=" && !f->seen_assign) {
            f->seen_assign = true;
            continue;
        }

        // --- unary / pointer / postfix -----------------------------------
        if (s == "+" || s == "-" || s == "!" || s == "~") {
            if (!after_operand) tk.unary = true;
            continue;
        }
        if (s == "++" || s == "--") {
            if (after_operand) tk.postfix = true;
            else tk.unary = true;
            continue;
        }
        if (s == "*" || s == "&" || s == "&&") {
            size_t nx = NextTok(t, i);
            if (!after_operand) {
                bool type_prev = p != kNpos && t[p].kind == Kind::Keyword &&
                                 IsTypeKeyword(t[p].text);
                tk.pointer = type_prev;
                tk.unary = !type_prev;
                continue;
            }
            if (p != kNpos && t[p].kind == Kind::Keyword && IsTypeKeyword(t[p].text)) {
                tk.pointer = true;
                continue;
            }
            if (nx != kNpos) {
                const std::string &nt = t[nx].text;
                if (t[nx].kind == Kind::Punct &&
                    (nt == ")" || nt == "," || nt == ";" || nt == "*" || nt == "&" ||
                     nt == "&&" || nt == "..." || nt == "[" || nt == "{")) {
                    tk.pointer = true;
                    continue;
                }
                if (t[nx].tmpl_close) {
                    tk.pointer = true;
                    continue;
                }
                bool name_next = t[nx].kind == Kind::Ident ||
                                 (t[nx].kind == Kind::Keyword &&
                                  (IsTypeKeyword(t[nx].text) || t[nx].text == "operator")) ||
                                 (t[nx].kind == Kind::Punct && t[nx].text == "~");
                if (name_next) {
                    bool decl_here = f->open == '{' ? f->kind == kBraceBlock : false;
                    if (f->open == 0) decl_here = true;
                    if (f->open == '(' &&
                        (t[f->idx].ctx == kParenDecl || t[f->idx].ctx == kParenCatch)) {
                        decl_here = true;
                    }
                    if (f->open == '<') decl_here = true;
                    int groups = 0;
                    bool has_type = false;
                    bool prefix_ok = DeclPrefix(t, f->elem_start, i, &groups, &has_type);
                    // An `if`/`while`/`for` head can declare too, but there
                    // `Name * name` is far more often a multiplication, so it
                    // takes an actual type keyword (`if (const Foo *p = q)`)
                    // to be read as a declarator.
                    if (!decl_here && f->open == '(' && t[f->idx].ctx == kParenControl) {
                        decl_here = has_type;
                    }
                    if (decl_here && !f->seen_assign && prefix_ok && groups >= 1) {
                        tk.pointer = true;
                    }
                }
            }
            continue;
        }
    }

    // Unclosed template guesses at end of file: take them back.
    while (st.size() > 1) {
        if (st.back().open == '<') t[st.back().idx].tmpl_open = false;
        st.pop_back();
    }
}

// ---------------------------------------------------------------------------
// Spacing
// ---------------------------------------------------------------------------

// Columns a rendered string occupies. UTF-8 continuation bytes do not advance
// the cursor; nothing here tries to be clever about double-width characters,
// which in C++ only ever turn up inside string literals and comments.
int Width(const std::string &s) {
    int w = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) w++;
    }
    return w;
}

std::string Pad(int n) { return std::string(n > 0 ? static_cast<size_t>(n) : 0, ' '); }

// Would writing `a` and `b` with nothing between them lex as something other
// than those two tokens? This is the backstop under the whole spacing table:
// every rule below that returns "no space" is checked against it, so a rule
// that is wrong about `:` next to `::`, or about the `>` of one template
// closing against the `>` of another, costs a stray space instead of a
// different program. (Format()'s re-lex-and-compare would catch those too,
// but as a refusal to format -- this turns them into output.)
bool WouldGlue(const Token &ta, const Token &tb) {
    const std::string &a = ta.text;
    const std::string &b = tb.text;
    if (a.empty() || b.empty()) return false;
    const char x = a.back();
    const char y = b.front();
    if (IsIdentCont(x) && IsIdentCont(y)) return true;
    if (x == '.' && IsDigit(y)) return true;
    // A preprocessing-number swallows a following `.`, and a `+`/`-` after an
    // exponent marker: `case 0 ... 5:` must not become `case 0... 5:`.
    if (ta.kind == Kind::Number) {
        if (y == '.') return true;
        if ((y == '+' || y == '-') && (x == 'e' || x == 'E' || x == 'p' || x == 'P')) {
            return true;
        }
    }
    // Two punctuation characters that begin a longer punctuator (or a
    // comment) glue into it.
    static const char *const glue[] = {"::", "->", "++", "--", "<<", ">>", "<=", ">=",
                                       "==", "!=", "&&", "||", "+=", "-=", "*=", "/=",
                                       "%=", "^=", "&=", "|=", ".*", "##", "//", "/*",
                                       "..", "<:", ":>", "%:", "<%", "%>"};
    for (const char *p : glue) {
        if (x == p[0] && y == p[1]) return true;
    }
    return false;
}

// Is there a space between `a` and `b` when they are rendered adjacently?
// Everything this needs is on the two tokens, because Annotate() put it
// there; the table below is the whole house style.
bool SpaceBetween(const Token &a, const Token &b) {
    if (b.no_space_before) return false;
    if (b.force_space_before) return true;

    // --- preprocessor -----------------------------------------------------
    if (a.pp && b.pp) {
        if (a.pp_lead) return false;                       // #include, #define
        if (a.kind == Kind::Punct && a.text == "#") return false;  // #x stringize
        if (a.text == "##" || b.text == "##") return false;
    }

    // --- comments ---------------------------------------------------------
    if (b.kind == Kind::BlockComment) {
        return !(a.text == "(" || a.text == "[" || a.tmpl_open || a.unary || a.pointer ||
                 a.text == "::" || a.text == "." || a.text == "->");
    }
    if (a.kind == Kind::BlockComment) {
        return !(b.text == ")" || b.text == "]" || b.text == "," || b.text == ";" ||
                 b.tmpl_close);
    }

    // --- operator names ---------------------------------------------------
    if (a.kind == Kind::Keyword && a.text == "operator") {
        return !(b.kind == Kind::Punct || b.kind == Kind::String);
    }
    if (a.op_name) return false;

    // --- brackets ---------------------------------------------------------
    if (a.text == "{" && b.text == "}") return false;
    if (b.text == ")" || b.text == "]") return false;
    if (b.tmpl_close) return false;
    if (b.text == "}") return b.ctx != kBraceInit;
    if (a.text == "(" || a.text == "[") return false;
    if (a.tmpl_open) return false;
    if (a.text == "{" && a.ctx == kBraceInit) return false;

    if (b.tmpl_open) return a.kind == Kind::Keyword && a.text == "template";
    if (b.text == "(") {
        if (b.ctx == kParenControl || b.ctx == kParenCatch) return true;
        if (b.ctx == kParenFnPtr) return true;
        if (b.ctx == kParenDecl) return false;
        if (a.kind == Kind::Keyword) {
            if (IsFunctionLikeKeyword(a.text)) return false;
            // `AnyInvocable<void()>`: a type keyword before a parameter list
            // is a function type, not a statement keyword taking a group.
            if (IsTypeKeyword(a.text)) return false;
            return !(a.text == "this" || a.text == "true" || a.text == "false" ||
                     a.text == "nullptr");
        }
        if (a.kind == Kind::Ident || a.text == ")" || a.text == "]" || a.tmpl_close) {
            return false;
        }
        if (a.unary || a.pointer) return false;
        if (a.text == "::" || a.text == "." || a.text == "->" || a.text == ".*" ||
            a.text == "->*") {
            return false;
        }
        return true;
    }
    if (b.text == "[") {
        if (b.ctx == kBracketSub) return false;
        if (b.ctx == kBracketAttr) return a.text != "[";
        if (a.kind == Kind::Keyword && (a.text == "new" || a.text == "delete")) return false;
        if (a.text == "]") return false;
        return true;
    }
    if (b.text == "{") {
        if (b.ctx != kBraceInit) return true;  // a block brace always gets one
        // `Thing t{1, 2}` and `Foo(){...}` bind tight; `= {1, 2}` and
        // `return {1, 2}` do not (Cpp11BracedListStyle).
        return !(a.kind == Kind::Ident || a.tmpl_close || a.text == ")" || a.text == "]" ||
                 (a.kind == Kind::Keyword && IsTypeKeyword(a.text)));
    }

    // --- separators -------------------------------------------------------
    if (b.text == "," || b.text == ";") return false;
    if (b.text == "::" || a.text == "::") return false;
    if (b.text == "." || b.text == "->" || b.text == ".*" || b.text == "->*") return false;
    if (a.text == "." || a.text == "->" || a.text == ".*" || a.text == "->*") return false;
    if (b.text == "...") return false;
    if (a.text == "...") return true;
    if (a.text == ")" && a.ctx == kParenCast) return false;

    // --- operators --------------------------------------------------------
    if (a.unary) return false;
    if (b.postfix) return false;
    if (a.pointer) return false;
    if (b.pointer) return true;
    if (b.text == ":") return b.ctx != kColonLabel;
    if (a.text == "#" && !a.pp) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Unwrapped lines
// ---------------------------------------------------------------------------

struct Line {
    size_t lo = 0, hi = 0;
    int level = 0;
    int blanks = 0;       // blank lines to print before this one
    bool pp = false;      // a preprocessor directive
    bool verbatim = false;  // ... carrying a line continuation: print as-is
    std::string raw;
    bool opens_block = false;
    bool starts_block_close = false;
};

size_t NextCode(const std::vector<Token> &t, size_t i) {
    for (size_t j = i + 1; j < t.size(); j++) {
        if (!IsComment(t[j])) return j;
    }
    return kNpos;
}

class Builder {
public:
    Builder(const std::vector<Token> &t, const Options &o, std::string_view src)
        : t_(t), o_(o), src_(src) {}

    std::vector<Line> Run();

private:
    struct BFrame {
        size_t open = kNpos;
        int level_at_open = 0;  // what level_ goes back to when the block ends
        int close_level = 0;    // indent of the `}` line, and of the `{`'s own line
        bool is_switch = false;
        bool is_class = false;
        bool is_do = false;
        bool is_enum = false;
        bool case_active = false;
    };

    void Begin(size_t i, int level) {
        if (start_ == kNpos) {
            start_ = i;
            cur_level_ = level;
        }
    }
    void End(size_t i) {
        if (start_ == kNpos || i <= start_) return;
        Line l;
        l.lo = start_;
        l.hi = i;
        l.level = cur_level_;
        l.starts_block_close = pending_close_;
        lines_.push_back(l);
        start_ = kNpos;
        pending_close_ = false;
    }
    int LevelFor(size_t i) const;
    void Unwind() {
        if (!body_stack_.empty()) {
            level_ = body_stack_.front();
            body_stack_.clear();
        }
    }

    const std::vector<Token> &t_;
    const Options &o_;
    std::string_view src_;
    std::vector<Line> lines_;
    std::vector<BFrame> bst_;
    // Indents owed to brace-less bodies (`if (c)` with the statement on the
    // next line). They all unwind together at the end of the statement they
    // belong to, which is the next `;` at statement level or the `}` of a
    // block opened inside the body.
    std::vector<int> body_stack_;
    int level_ = 0;
    int expr_ = 0;
    size_t start_ = kNpos;
    int cur_level_ = 0;
    bool pending_close_ = false;
};

int Builder::LevelFor(size_t i) const {
    const Token &tk = t_[i];
    if (expr_ == 0 && !bst_.empty()) {
        const BFrame &f = bst_.back();
        if (f.is_switch && tk.kind == Kind::Keyword &&
            (tk.text == "case" || tk.text == "default")) {
            if (!o_.indent_case_labels) return level_ - 1;
            return f.case_active ? level_ - 1 : level_;
        }
        if (f.is_class && tk.kind == Kind::Keyword &&
            (tk.text == "public" || tk.text == "private" || tk.text == "protected")) {
            size_t nx = NextCode(t_, i);
            if (nx != kNpos && t_[nx].ctx == kColonLabel) return level_ - 1;
        }
    }
    if (tk.kind == Kind::Punct && tk.text == "}" && tk.ctx == kBraceBlock && !bst_.empty() &&
        bst_.back().open == tk.match) {
        return bst_.back().close_level;
    }
    return level_;
}

std::vector<Line> Builder::Run() {
    const size_t n = t_.size();
    for (size_t i = 0; i < n; i++) {
        const Token &tk = t_[i];

        // --- preprocessor directives, always alone on their line ----------
        if (tk.pp_lead) {
            End(i);
            size_t j = i;
            while (j + 1 < n && t_[j + 1].pp && !t_[j + 1].pp_lead) j++;
            Line l;
            l.lo = i;
            l.hi = j + 1;
            l.level = 0;
            l.pp = true;
            size_t from = tk.off;
            size_t to = t_[j].off + t_[j].text.size();
            std::string raw(src_.substr(from, to - from));
            if (raw.find('\n') != std::string::npos) {
                l.verbatim = true;
                l.raw = raw;
            }
            lines_.push_back(l);
            i = j;
            continue;
        }

        if (IsComment(tk)) {
            if (start_ != kNpos) {
                // A comment on a line of its own, at statement level, after
                // something that already looks finished -- the usual cause is
                // a macro invocation with no semicolon (`G_BEGIN_DECLS`) --
                // is not part of that statement. Inside brackets, or after an
                // operator, it is, and stays.
                size_t pv = i > 0 ? i - 1 : kNpos;
                while (pv != kNpos && pv > start_ && IsComment(t_[pv])) pv--;
                bool finished = pv != kNpos && pv >= start_ &&
                                (t_[pv].kind == Kind::Ident || t_[pv].text == ")" ||
                                 t_[pv].text == "]");
                if (expr_ == 0 && tk.newlines_before > 0 && finished) {
                    End(i);
                } else {
                    continue;  // interior or trailing: part of the line
                }
            }
            if (tk.newlines_before == 0 && !lines_.empty() && lines_.back().hi == i &&
                !lines_.back().pp) {
                lines_.back().hi = i + 1;  // trailing comment on the line just closed
                continue;
            }
            size_t nx = NextCode(t_, i);
            bool alone = tk.kind == Kind::LineComment || nx == kNpos ||
                         t_[nx].line > tk.end_line;
            // A comment lines up with the code that follows it -- except a
            // comment just before a `}`, which belongs to the block it is
            // sitting at the end of rather than to the brace.
            int lvl = level_;
            if (nx != kNpos && !(t_[nx].kind == Kind::Punct && t_[nx].text == "}" &&
                                 t_[nx].ctx == kBraceBlock)) {
                lvl = LevelFor(nx);
            }
            Begin(i, lvl);
            if (alone) End(i + 1);
            continue;
        }

        if (tk.kind == Kind::Punct && tk.text == "}" && tk.ctx == kBraceBlock &&
            !bst_.empty() && bst_.back().open == tk.match) {
            End(i);
            BFrame f = bst_.back();
            bst_.pop_back();
            level_ = f.level_at_open;
            Begin(i, f.close_level);
            pending_close_ = true;
            size_t nx = NextCode(t_, i);
            bool join = false;
            if (nx != kNpos) {
                const Token &x = t_[nx];
                const std::string &s = x.text;
                if (x.kind == Kind::Punct &&
                    (s == ";" || s == "," || s == ")" || s == "]" || s == "(" || s == "." ||
                     s == "->")) {
                    join = true;
                } else if (x.kind == Kind::Keyword && (s == "else" || s == "catch")) {
                    join = true;
                } else if (x.kind == Kind::Keyword && s == "while" && f.is_do) {
                    join = true;
                } else if (f.is_class &&
                           (x.kind == Kind::Ident || s == "*" || s == "&" || s == "::" ||
                            s == "[")) {
                    join = true;
                }
            }
            if (!join) {
                End(i + 1);
                Unwind();
            }
            continue;
        }

        if (tk.kind == Kind::Keyword && (tk.text == "else" || tk.text == "do") &&
            start_ == kNpos) {
            size_t nx = NextCode(t_, i);
            bool braced = nx != kNpos && t_[nx].text == "{" && t_[nx].ctx == kBraceBlock;
            bool else_if = tk.text == "else" && nx != kNpos && t_[nx].kind == Kind::Keyword &&
                           t_[nx].text == "if";
            if (!braced && !else_if && nx != kNpos) {
                Begin(i, LevelFor(i));
                End(i + 1);
                body_stack_.push_back(level_);
                level_++;
                continue;
            }
        }

        int lvl = LevelFor(i);
        bool is_case = expr_ == 0 && !bst_.empty() && bst_.back().is_switch &&
                       tk.kind == Kind::Keyword &&
                       (tk.text == "case" || tk.text == "default") && start_ == kNpos;
        Begin(i, lvl);
        if (is_case && o_.indent_case_labels && !bst_.back().case_active) {
            bst_.back().case_active = true;
            level_++;
        } else if (is_case) {
            bst_.back().case_active = true;
        }

        if (tk.kind == Kind::Punct) {
            const std::string &s = tk.text;
            if (s == "{" && tk.ctx == kBraceBlock) {
                End(i + 1);
                lines_.back().opens_block = true;
                BFrame nf;
                nf.open = i;
                nf.level_at_open = level_;
                // Contents are one level in from the line the `{` sits on,
                // not from level_: a `case 1: {` label has already taken its
                // own step in, and indenting again from there doubles it.
                nf.close_level = lines_.back().level;
                nf.is_switch = tk.brace_head == 1;
                nf.is_class = tk.brace_head == 2 || tk.brace_head == 5;
                nf.is_do = tk.brace_head == 4;
                nf.is_enum = tk.brace_head == 5;
                bool no_indent = tk.brace_head == 3 && !o_.indent_namespaces;
                bst_.push_back(nf);
                level_ = nf.close_level + (no_indent ? 0 : 1);
                continue;
            }
            if (s == "(" || s == "[" || (s == "{" && tk.ctx == kBraceInit) || tk.tmpl_open) {
                expr_++;
                continue;
            }
            if (s == ")" || s == "]" || (s == "}" && tk.ctx == kBraceInit) || tk.tmpl_close) {
                if (expr_ > 0) expr_ -= tk.tmpl_close && s == ">>" ? 2 : 1;
                if (expr_ < 0) expr_ = 0;
                if (expr_ == 0 && tk.tmpl_close && tk.match != kNpos) {
                    size_t kw = tk.match > 0 ? tk.match - 1 : kNpos;
                    while (kw != kNpos && IsComment(t_[kw])) kw = kw > 0 ? kw - 1 : kNpos;
                    if (kw != kNpos && t_[kw].kind == Kind::Keyword &&
                        t_[kw].text == "template") {
                        // `template <...>` is its own line, at the same indent
                        // as the declaration it introduces. The join pass puts
                        // it back when that is how it was written and it fits.
                        End(i + 1);
                    }
                    continue;
                }
                if (expr_ == 0 && s == ")" &&
                    (tk.ctx == kParenControl || tk.ctx == kParenCatch)) {
                    // `if (...)`/`for (...)`/`while (...)` whose body is not a
                    // block: the body is its own line, one level in. The join
                    // pass puts it back on this line when that is how it was
                    // written and it fits. A `;` right here is either a
                    // do-while's tail or an empty body, and neither wants a
                    // line to itself.
                    size_t nx = NextCode(t_, i);
                    if (nx != kNpos && t_[nx].text != ";" &&
                        !(t_[nx].text == "{" && t_[nx].ctx == kBraceBlock)) {
                        End(i + 1);
                        body_stack_.push_back(level_);
                        level_++;
                    }
                }
                continue;
            }
            if (expr_ == 0 && s == ";") {
                End(i + 1);
                Unwind();
                continue;
            }
            // In an enum body the commas are what separate the declarations,
            // the way `;` does everywhere else. Without this the whole body
            // is one unwrapped line and the splitter bin-packs the
            // enumerators together, which is not what an enum looks like.
            if (expr_ == 0 && s == "," && !bst_.empty() && bst_.back().is_enum) {
                size_t nx = NextCode(t_, i);
                size_t end = i + 1;
                while (end < n && t_[end].kind == Kind::LineComment &&
                       t_[end].newlines_before == 0) {
                    end++;
                }
                if (nx != kNpos) {
                    End(end);
                    i = end - 1;
                }
                continue;
            }
            if (expr_ == 0 && s == ":" && tk.ctx == kColonLabel) {
                size_t nx = NextCode(t_, i);
                // `case 1: {` keeps the brace where it was written rather
                // than stranding it on a line of its own.
                if (!(nx != kNpos && t_[nx].text == "{" && t_[nx].ctx == kBraceBlock &&
                      t_[nx].line == tk.line)) {
                    End(i + 1);
                }
                continue;
            }
        }
    }
    End(n);

    // Blank lines: at most max_empty_lines between statements, none at the
    // start of a block, before a closing brace, or at the top of the file.
    for (size_t k = 0; k < lines_.size(); k++) {
        Line &l = lines_[k];
        int nl = t_[l.lo].newlines_before - 1;
        if (nl > o_.max_empty_lines) nl = o_.max_empty_lines;
        if (nl < 0) nl = 0;
        if (k == 0) nl = 0;
        l.blanks = nl;
    }
    return lines_;
}


// ---------------------------------------------------------------------------
// Rendering and line splitting
// ---------------------------------------------------------------------------

// Binary operators, lowest binding first. The splitter picks the lowest class
// present at the top level of a range and breaks at every operator in it,
// which is the one thing that reliably reads better than breaking anywhere
// else. clang-format's LLVM preset leaves the operator at the end of the line
// (BreakBeforeBinaryOperators: None) but puts a ternary's `?` and `:` at the
// start of the next one, and so does this.
int OpClass(const Token &x) {
    if (x.kind != Kind::Punct) return -1;
    if (x.unary || x.pointer || x.postfix || x.tmpl_open || x.tmpl_close || x.op_name) return -1;
    const std::string &s = x.text;
    if (s == "?") return 0;
    if (s == ":") return x.ctx == kColonTernary ? 0 : -1;
    if (s == "||") return 1;
    if (s == "&&") return 2;
    if (s == "|") return 3;
    if (s == "^") return 4;
    if (s == "&") return 5;
    if (s == "==" || s == "!=") return 6;
    if (s == "<" || s == ">" || s == "<=" || s == ">=" || s == "<=>") return 7;
    if (s == "<<" || s == ">>") return 8;
    if (s == "+" || s == "-") return 9;
    if (s == "*" || s == "/" || s == "%") return 10;
    return -1;
}

void RTrim(std::string &s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    s.resize(e);
}

class Emitter {
public:
    Emitter(const std::vector<Token> &t, const Options &o) : t_(t), o_(o) {}

    std::string Run(const std::vector<Line> &lines);
    const std::vector<int> &standalone_comment_lines() const {
        return standalone_comment_lines_;
    }

private:
    // Everything that reaches `out_` goes through here, so `out_line_` --
    // which the trailing-comment alignment pass indexes by -- cannot drift on
    // the pieces that carry their own newlines: a verbatim `#define`, a
    // re-indented block comment, a raw string literal.
    void AppendOut(const std::string &s) {
        for (char c : s) {
            if (c == '\n') out_line_++;
        }
        out_ += s;
    }
    void Break(int indent) {
        RTrim(cur_);
        AppendOut(cur_);
        AppendOut("\n");
        cur_ = Pad(indent);
    }
    void Flush() {
        RTrim(cur_);
        AppendOut(cur_);
        AppendOut("\n");
        cur_.clear();
    }
    void Add(const std::string &s) { cur_ += s; }
    int Col() const {
        size_t nl = cur_.rfind('\n');
        return Width(nl == std::string::npos ? cur_ : cur_.substr(nl + 1));
    }
    std::string Sep(size_t j) const;
    bool HasMagicComma(size_t lo, size_t hi) const;
    bool MagicComma(size_t open, size_t close) const;
    std::string Render(size_t lo, size_t hi) const;
    bool HasInteriorLineComment(size_t lo, size_t hi) const;
    bool EndsControlHeader(const Line &l) const;
    size_t JoinSpan(const std::vector<Line> &ls, size_t k) const;
    void EmitLine(const Line &l);
    void EmitRange(size_t lo, size_t hi, int cont, int depth);
    template <typename F>
    void ForEachTop(size_t lo, size_t hi, F fn) const;
    int TopOpClass(size_t lo, size_t hi) const;
    bool SplitBracket(size_t lo, size_t hi, int cont, int depth);
    bool SplitOperator(size_t lo, size_t hi, int cont, int depth);
    bool SplitAssign(size_t lo, size_t hi, int cont, int depth);
    std::string ReindentBlockComment(const std::string &text, int indent) const;

    const std::vector<Token> &t_;
    const Options &o_;
    std::string out_;
    std::string cur_;
    int out_line_ = 1;
    // Output lines that hold a whole comment-only unwrapped line -- a comment
    // written between two statements, as opposed to one the splitter had to
    // break a statement at. Only the former can continue a trailing comment's
    // alignment run; see AlignTrailingComments.
    std::vector<int> standalone_comment_lines_;
};

// The separator between two adjacent tokens on one output line: a trailing
// comment gets its own (wider) gap, everything else asks the spacing table
// and then the glue backstop.
std::string Emitter::Sep(size_t j) const {
    const Token &a = t_[j - 1];
    const Token &b = t_[j];
    // The wider gap is for a trailing `//`, which by construction ends its
    // output line -- everything after it is part of it. A trailing `/* */`
    // gets one space, the way clang-format's SpacesBeforeTrailingComments
    // also only covers line comments. Deciding this from the comment's
    // original position instead would not be a fixed point: a comment pulled
    // up onto a macro line by this very pass would qualify on the next run
    // and gain a space every time.
    if (!IsComment(a) && b.kind == Kind::LineComment) {
        return std::string(static_cast<size_t>(o_.spaces_before_trailing_comment > 0
                                                   ? o_.spaces_before_trailing_comment
                                                   : 1),
                           ' ');
    }
    if (SpaceBetween(a, b) || WouldGlue(a, b)) return " ";
    return "";
}

std::string Emitter::Render(size_t lo, size_t hi) const {
    std::string r;
    for (size_t j = lo; j < hi; j++) {
        if (j > lo) r += Sep(j);
        r += t_[j].text;
    }
    return r;
}

// A braced initializer whose last element carries a comma stays exploded,
// one element per line, however short it is -- clang-format's reading of a
// trailing comma, and black's "magic trailing comma" by another name. It is a
// fixed point because this never adds or removes a comma: what comes out
// exploded goes back in exploded.
bool Emitter::MagicComma(size_t open, size_t close) const {
    if (t_[open].kind != Kind::Punct || t_[open].text != "{") return false;
    if (t_[open].ctx != kBraceInit) return false;
    size_t j = close;
    while (j > open + 1 && IsComment(t_[j - 1])) j--;
    return j > open + 1 && t_[j - 1].kind == Kind::Punct && t_[j - 1].text == ",";
}

bool Emitter::HasMagicComma(size_t lo, size_t hi) const {
    for (size_t j = lo; j < hi; j++) {
        if (t_[j].match != kNpos && t_[j].match < hi && t_[j].match > j &&
            MagicComma(j, t_[j].match)) {
            return true;
        }
    }
    return false;
}

bool Emitter::HasInteriorLineComment(size_t lo, size_t hi) const {
    for (size_t j = lo; j + 1 < hi; j++) {
        if (t_[j].kind == Kind::LineComment) return true;
    }
    return false;
}

// Lines whose next line belongs to them: a braceless `if (c)`/`else`/`do`
// body, or the declaration a `template <...>` header introduces.
bool Emitter::EndsControlHeader(const Line &l) const {
    const Token &last = t_[l.hi - 1];
    if (last.kind == Kind::Punct && last.text == ")" &&
        (last.ctx == kParenControl || last.ctx == kParenCatch)) {
        return true;
    }
    if (last.kind == Kind::Keyword && (last.text == "else" || last.text == "do")) return true;
    if (last.tmpl_close && last.match != kNpos && last.match > 0) {
        size_t kw = last.match - 1;
        while (kw != kNpos && IsComment(t_[kw])) kw = kw > 0 ? kw - 1 : kNpos;
        if (kw != kNpos && t_[kw].kind == Kind::Keyword && t_[kw].text == "template") return true;
    }
    return false;
}

size_t Emitter::JoinSpan(const std::vector<Line> &ls, size_t k) const {
    const Line &L = ls[k];
    if (L.pp) return 1;
    size_t span = 1;
    if (L.opens_block) {
        int depth = 1;
        size_t m = k;
        while (depth > 0) {
            m++;
            if (m >= ls.size() || ls[m].pp) return 1;
            if (ls[m].starts_block_close) depth--;
            if (ls[m].opens_block) depth++;
        }
        span = m - k + 1;
        // An empty body is written `{}` whatever the source did with it:
        // there is nothing in there for a line break to separate, and the
        // two characters cannot push a line over on their own. Empty means
        // the `{` really is this line's last token -- a comment written
        // inside the braces is attached to this line and is not nothing.
        if (span == 2 && ls[k + 1].lo == L.hi && t_[L.hi - 1].text == "{" &&
            t_[ls[k + 1].lo].text == "}") {
            return 2;
        }
    } else if (EndsControlHeader(L)) {
        if (k + 1 >= ls.size() || ls[k + 1].pp) return 1;
        span = 1 + JoinSpan(ls, k + 1);
    } else {
        return 1;
    }
    if (k + span > ls.size()) return 1;
    size_t lo = L.lo;
    size_t hi = ls[k + span - 1].hi;
    if (t_[lo].line != t_[hi - 1].end_line) return 1;
    if (HasInteriorLineComment(lo, hi) || HasMagicComma(lo, hi)) return 1;
    std::string flat = Render(lo, hi);
    if (flat.find('\n') != std::string::npos) return 1;
    if (L.level * o_.indent_width + Width(flat) > o_.column_limit) return 1;
    return span;
}

std::string Emitter::ReindentBlockComment(const std::string &text, int indent) const {
    // Only the standard `/* ... \n * ... \n */` shape is re-indented: every
    // continuation line has to start with a single `*`. Anything else (ASCII
    // art, a `**` banner, a pasted table, commented-out code) is left byte
    // for byte, because moving it would be a guess about what the author was
    // lining it up with.
    //
    // The test looks only at the comment's own shape, never at the column it
    // used to start in. A column test is not a fixed point once this pass has
    // moved the opening `/*`: the second run measures the continuations
    // against a different origin and can flip its answer.
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            parts.push_back(text.substr(pos));
            break;
        }
        parts.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (parts.size() < 2) return text;
    for (size_t k = 1; k < parts.size(); k++) {
        size_t f = parts[k].find_first_not_of(" \t");
        if (f == std::string::npos || parts[k][f] != '*') return text;
        if (f + 1 < parts[k].size() && parts[k][f + 1] == '*') return text;  // a banner
    }
    std::string r = parts[0];
    for (size_t k = 1; k < parts.size(); k++) {
        size_t f = parts[k].find_first_not_of(" \t");
        r += '\n';
        r += Pad(indent + 1);
        r += parts[k].substr(f);
    }
    return r;
}

void Emitter::EmitRange(size_t lo, size_t hi, int cont, int depth) {
    if (lo >= hi) return;
    if (depth > kMaxSplitDepth) {
        Add(Render(lo, hi));
        return;
    }
    if (!HasInteriorLineComment(lo, hi) && !HasMagicComma(lo, hi)) {
        std::string flat = Render(lo, hi);
        if (flat.find('\n') != std::string::npos) {
            Add(flat);
            return;
        }
        if (Col() + Width(flat) <= o_.column_limit) {
            Add(flat);
            return;
        }
    }
    // A member initializer list always breaks before its colon when the line
    // has to break at all: `Foo::Foo(int a)` / `    : a_(a) {`.
    for (size_t j = lo + 1; j < hi; j++) {
        // From lo + 1: the colon has to have something in front of it on this
        // line, or splitting at it is not a split and the recursion below
        // never gets anywhere.
        if (t_[j].kind == Kind::Punct && t_[j].text == ":" && t_[j].ctx == kColonCtorInit) {
            EmitRange(lo, j, cont, depth + 1);
            Break(cont);
            EmitRange(j, hi, cont + o_.continuation_indent, depth + 1);
            return;
        }
    }
    // Breaking after `=` and dropping the whole right-hand side one indent in
    // beats every other split when there is an assignment to break at -- it is
    // what clang-format's penalties come out to, and it keeps the interesting
    // half of the statement whole.
    if (SplitAssign(lo, hi, cont, depth)) return;
    // A low-precedence operator at the top level is a better place to break
    // than any bracket inside it: `a && b` splits at the `&&`, not inside
    // whatever subscript happens to sit furthest right.
    int op = TopOpClass(lo, hi);
    if (op >= 0 && op <= 8 && SplitOperator(lo, hi, cont, depth)) return;
    if (SplitBracket(lo, hi, cont, depth)) return;
    if (op >= 0 && SplitOperator(lo, hi, cont, depth)) return;
    // Nothing to split at. A too-long string literal, a lone identifier, a
    // comment: emit it over the limit rather than mangle it.
    if (HasInteriorLineComment(lo, hi)) {
        // Nothing to split at, but a `//` swallows the rest of its line, so
        // the stream has to be broken at every one of them regardless.
        bool at_start = true;
        for (size_t j = lo; j < hi; j++) {
            if (!at_start) Add(Sep(j));
            Add(t_[j].text);
            at_start = false;
            if (t_[j].kind == Kind::LineComment && j + 1 < hi) {
                Break(cont);
                at_start = true;
            }
        }
        return;
    }
    Add(Render(lo, hi));
}

// Walks the top level of [lo, hi) applying `fn` to every token that is not
// inside a bracket of its own. Everything that has to reason about "the top
// level" -- picking split brackets, finding operators, cutting elements at
// commas -- goes through this so they cannot disagree about where it is.
template <typename F>
void Emitter::ForEachTop(size_t lo, size_t hi, F fn) const {
    int d = 0;
    for (size_t j = lo; j < hi; j++) {
        const Token &x = t_[j];
        if (x.op_name) continue;
        if (x.tmpl_open) {
            d++;
        } else if (x.tmpl_close) {
            d -= (x.text == ">>") ? 2 : 1;
            if (d < 0) d = 0;
        } else if (x.kind == Kind::Punct && (x.text == "(" || x.text == "[" || x.text == "{")) {
            d++;
        } else if (x.kind == Kind::Punct && (x.text == ")" || x.text == "]" || x.text == "}")) {
            d--;
            if (d < 0) d = 0;
        } else if (d == 0) {
            fn(j);
        }
    }
}

int Emitter::TopOpClass(size_t lo, size_t hi) const {
    int best = -1;
    ForEachTop(lo, hi, [&](size_t j) {
        if (j == lo) return;
        int c = OpClass(t_[j]);
        if (c >= 0 && (best < 0 || c < best)) best = c;
    });
    return best;
}

bool Emitter::SplitBracket(size_t lo, size_t hi, int cont, int depth) {
    // Top-level bracket pairs of the range, left to right.
    std::vector<std::pair<size_t, size_t>> pairs;
    int d = 0;
    for (size_t j = lo; j < hi; j++) {
        const Token &x = t_[j];
        bool open = (x.kind == Kind::Punct &&
                     (x.text == "(" || x.text == "[" || x.text == "{")) ||
                    x.tmpl_open;
        if (open && !x.op_name && d == 0 && x.match != kNpos && x.match < hi && x.match > j) {
            pairs.emplace_back(j, x.match);
            j = x.match;
            continue;
        }
        if (x.op_name) continue;
        if (x.tmpl_open) d++;
        else if (x.tmpl_close) d -= (x.text == ">>") ? 2 : 1;
        else if (x.kind == Kind::Punct && (x.text == "(" || x.text == "[" || x.text == "{")) d++;
        else if (x.kind == Kind::Punct && (x.text == ")" || x.text == "]" || x.text == "}")) d--;
        if (d < 0) d = 0;
    }
    if (pairs.empty()) return false;

    // Pick the pair with the most inside it, preferring the rightmost on a
    // tie and never a subscript when anything else is available. Taking the
    // rightmost outright -- python_format's right-hand split, which works for
    // Python because a trailing call is where the weight is -- picks `[i]`
    // out of `foo(bar) || baz[i] == x` here and produces nonsense.
    size_t open = kNpos, close = kNpos;
    int best_score = -1;
    for (const auto &pr : pairs) {
        if (pr.second <= pr.first + 1) continue;
        if (HasInteriorLineComment(lo, pr.first + 1)) continue;
        std::string head = Render(lo, pr.first + 1);
        if (head.find('\n') != std::string::npos) continue;
        if (Col() + Width(head) > o_.column_limit) continue;
        std::string body = Render(pr.first + 1, pr.second);
        int score = Width(body);
        if (t_[pr.first].ctx == kBracketSub) score -= 1000;
        if (MagicComma(pr.first, pr.second)) score += 1000;
        if (score >= best_score) {
            best_score = score;
            open = pr.first;
            close = pr.second;
        }
    }
    if (open == kNpos) return false;

    Add(Render(lo, open + 1));
    const int align = Col();
    const bool magic = MagicComma(open, close);
    const bool forced = magic || HasInteriorLineComment(open + 1, close);

    // Elements, each carrying its own trailing comma.
    std::vector<std::pair<size_t, size_t>> elems;
    size_t es = open + 1;
    ForEachTop(open + 1, close, [&](size_t j) {
        if (t_[j].kind == Kind::Punct && t_[j].text == ",") {
            // A `//` comment written after the comma is that element's, not
            // the next one's -- it is on that line and has to stay there.
            size_t end = j + 1;
            while (end < close && t_[end].kind == Kind::LineComment &&
                   t_[end].newlines_before == 0) {
                end++;
            }
            elems.emplace_back(es, end);
            es = end;
        }
    });
    if (es < close) elems.emplace_back(es, close);
    if (elems.empty()) return false;

    int widest = 0;
    for (const auto &e : elems) {
        std::string f = Render(e.first, e.second);
        if (f.find('\n') == std::string::npos) widest = std::max(widest, Width(f));
    }
    // Align the continuation under the open bracket when there is room for it
    // (clang-format's AlignAfterOpenBracket: Align); otherwise break straight
    // after the bracket and hang everything one continuation indent in.
    const bool aligned = !magic && align + std::min(widest, 24) <= o_.column_limit &&
                         align <= o_.column_limit - 8;
    const int elem_indent = aligned ? align : cont;
    if (!aligned) Break(elem_indent);

    bool at_start = true;
    for (size_t k = 0; k < elems.size(); k++) {
        const auto &e = elems[k];
        if (k == 0 && at_start && aligned) Add(Sep(e.first));
        // Never decide the break from a flat form that will not be emitted:
        // an element carrying its own magic comma, or a `//`, measures short
        // and then explodes anyway.
        bool flatten = !HasInteriorLineComment(e.first, e.second) &&
                       !HasMagicComma(e.first, e.second);
        std::string f = flatten ? Render(e.first, e.second) : std::string();
        bool fits = !f.empty() && f.find('\n') == std::string::npos &&
                    Col() + (at_start ? 0 : 1) + Width(f) <= o_.column_limit;
        if (!at_start && (forced || !fits)) {
            Break(elem_indent);
        } else if (!at_start) {
            Add(Sep(e.first));
        }
        EmitRange(e.first, e.second, elem_indent, depth + 1);
        at_start = false;
        // A `//` comment swallows the rest of its line, so whatever comes
        // next has to start a new one. This is the one break the splitter is
        // not free to skip.
        if (t_[e.second - 1].kind == Kind::LineComment && k + 1 < elems.size()) {
            Break(elem_indent);
            at_start = true;
        }
    }
    // A braced initializer that had to hang puts its `}` back at the
    // statement's own indent; a call's `)` stays with the last argument.
    const bool init_list = t_[open].kind == Kind::Punct && t_[open].text == "{" &&
                           t_[open].ctx == kBraceInit;
    if (t_[close - 1].kind == Kind::LineComment || (!aligned && init_list)) {
        Break(aligned ? elem_indent : cont - o_.continuation_indent);
    } else {
        Add(Sep(close));
    }
    EmitRange(close, hi, cont, depth + 1);
    return true;
}

bool Emitter::SplitOperator(size_t lo, size_t hi, int cont, int depth) {
    const int best = TopOpClass(lo, hi);
    if (best < 0) return false;
    std::vector<size_t> at;
    ForEachTop(lo, hi, [&](size_t j) {
        if (j > lo && OpClass(t_[j]) == best) at.push_back(j);
    });
    if (at.empty()) return false;

    const bool before = best == 0;  // ternary: `?` and `:` start the next line
    std::vector<std::pair<size_t, size_t>> segs;
    size_t s = lo;
    for (size_t p : at) {
        size_t cut = before ? p : p + 1;
        if (cut <= s) continue;
        segs.emplace_back(s, cut);
        s = cut;
    }
    if (s < hi) segs.emplace_back(s, hi);
    if (segs.size() < 2) return false;

    // Bin-pack the operands the same way an argument list is packed, rather
    // than putting one per line: `a || b || c || d` that overruns by six
    // columns should lose one line, not four.
    for (size_t k = 0; k < segs.size(); k++) {
        if (k > 0) {
            bool flatten = !HasInteriorLineComment(segs[k].first, segs[k].second) &&
                           !HasMagicComma(segs[k].first, segs[k].second);
            std::string f = flatten ? Render(segs[k].first, segs[k].second) : std::string();
            // Everything after a `//` is part of it, so a segment that ended
            // in one takes the rest of the line with it whatever it measures.
            bool after_comment = t_[segs[k].first - 1].kind == Kind::LineComment;
            bool fits = !after_comment && !f.empty() && f.find('\n') == std::string::npos &&
                        Col() + 1 + Width(f) <= o_.column_limit;
            if (fits) {
                Add(Sep(segs[k].first));
            } else {
                Break(cont);
            }
        }
        EmitRange(segs[k].first, segs[k].second, cont, depth + 1);
    }
    return true;
}

// Break after the `=` of an assignment and put the right-hand side on its own
// line, one continuation indent in.
bool Emitter::SplitAssign(size_t lo, size_t hi, int cont, int depth) {
    size_t at = kNpos;
    ForEachTop(lo, hi, [&](size_t j) {
        if (at != kNpos || j == lo) return;
        const Token &x = t_[j];
        if (x.kind != Kind::Punct) return;
        const std::string &v = x.text;
        if (v == "=" || v == "+=" || v == "-=" || v == "*=" || v == "/=" || v == "%=" ||
            v == "^=" || v == "&=" || v == "|=" || v == "<<=" || v == ">>=") {
            at = j;
        }
    });
    if (at == kNpos || at + 1 >= hi) return false;
    // `Thing t[] = {...}` keeps its `= {` on the first line and hangs the
    // list under it; stranding the brace on a line of its own is not how a
    // braced initializer is written.
    if (t_[at + 1].kind == Kind::Punct && t_[at + 1].text == "{" &&
        t_[at + 1].ctx == kBraceInit) {
        return false;
    }
    if (HasInteriorLineComment(lo, at + 1)) return false;
    std::string head = Render(lo, at + 1);
    if (head.find('\n') != std::string::npos) return false;
    if (Col() + Width(head) > o_.column_limit) return false;
    EmitRange(lo, at + 1, cont, depth + 1);
    Break(cont);
    EmitRange(at + 1, hi, cont, depth + 1);
    return true;
}

void Emitter::EmitLine(const Line &l) {
    for (int k = 0; k < l.blanks; k++) AppendOut("\n");
    bool all_comments = !l.pp;
    for (size_t j = l.lo; all_comments && j < l.hi; j++) {
        if (!IsComment(t_[j])) all_comments = false;
    }
    if (all_comments) standalone_comment_lines_.push_back(out_line_);
    if (l.pp) {
        if (l.verbatim) {
            std::string raw = l.raw;
            std::string acc;
            size_t pos = 0;
            while (pos <= raw.size()) {
                size_t nl = raw.find('\n', pos);
                std::string part =
                    raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                RTrim(part);
                acc += part;
                if (nl == std::string::npos) break;
                acc += '\n';
                pos = nl + 1;
            }
            AppendOut(acc);
            AppendOut("\n");
            return;
        }
        cur_.clear();
        Add(Render(l.lo, l.hi));
        Flush();
        return;
    }
    const int indent = l.level * o_.indent_width;
    if (l.hi == l.lo + 1 && t_[l.lo].kind == Kind::BlockComment &&
        t_[l.lo].text.find('\n') != std::string::npos) {
        cur_ = Pad(indent);
        Add(ReindentBlockComment(t_[l.lo].text, indent));
        Flush();
        return;
    }
    cur_ = Pad(indent);
    EmitRange(l.lo, l.hi, indent + o_.continuation_indent, 0);
    Flush();
}

std::string Emitter::Run(const std::vector<Line> &lines) {
    for (size_t k = 0; k < lines.size();) {
        size_t span = JoinSpan(lines, k);
        Line m = lines[k];
        m.hi = lines[k + span - 1].hi;
        EmitLine(m);
        k += span;
    }
    return out_;
}

// ---------------------------------------------------------------------------
// Trailing comment alignment
// ---------------------------------------------------------------------------

// Line up a run of trailing `//` comments in one column, the way
// clang-format's AlignTrailingComments does and the way most of the code this
// runs on is written. Done as a post-pass over the finished text rather than
// during emission, because the column a run settles on is the widest line in
// it, which is not known until every line of the run exists.
//
// This only ever changes the run of spaces in front of a comment, so it
// cannot disturb the token stream the verifier is about to check.
//
// Staying a fixed point is the whole difficulty -- it is the trap that cost
// real debugging in python_format too (see its AssignCommentIndents). Three
// things do it:
//
//   * The target column comes from the *code* widths, which this pass never
//     touches, so a second run computes the same column.
//   * Whether a comment-only line joins the run above it is asked as "is it
//     at least as far right as the comment it would be continuing?", never
//     as "is it further right than the statement's indent". The first
//     question survives this pass moving both of them to the same column;
//     the second does not, and reading it that way was what made a comment
//     written *inside* the block a run's first line opened get swept in.
//   * Only a comment that was a whole line of its own is eligible at all. A
//     comment the splitter had to break a statement at sits at a
//     continuation indent this pass did not choose and cannot predict from
//     the input.
//
// Format() still applies it twice, because the first application is the one
// that moves columns and the second is the one that reads what it wrote.
//
// `columns` supplies where each token sat in the *input* -- the only place
// the "this comment hangs off the one above it" signal exists. Passing null
// means "use the text's own columns", which is what that second application
// does.
void AlignTrailingComments(std::string &text, const std::vector<Token> *columns,
                           const std::vector<int> &standalone_comment_lines,
                           const Options &opts) {
    Lexer lx(text);
    if (!lx.Run()) return;  // the verifier will refuse; do not make it worse
    const std::vector<Token> &after = lx.tokens();
    if (columns != nullptr && after.size() != columns->size()) return;
    const std::vector<Token> &before = columns != nullptr ? *columns : after;

    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            if (pos < text.size()) out.push_back(text.substr(pos));
            break;
        }
        out.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }

    // Per output line: the comment that ends it, if any.
    struct Slot {
        bool has = false;
        bool code_before = false;   // something other than the comment is on it
        bool standalone = false;    // a whole comment-only unwrapped line
        size_t cut = 0;            // byte offset of the comment within the line
        int input_col = 0;   // where the comment sat in the source
    };
    std::vector<Slot> slot(out.size());
    int line = 1;
    size_t line_begin = 0;
    for (size_t i = 0; i < after.size(); i++) {
        const Token &t = after[i];
        while (line < t.line && static_cast<size_t>(line) <= out.size()) {
            line_begin += out[static_cast<size_t>(line - 1)].size() + 1;
            line++;
        }
        if (t.pp) continue;
        if (t.kind != Kind::LineComment && t.kind != Kind::BlockComment) continue;
        if (t.end_line != t.line) continue;  // a multi-line /* */ is not a slot
        bool last_on_line = i + 1 >= after.size() || after[i + 1].line > t.line;
        if (!last_on_line) continue;
        size_t idx = static_cast<size_t>(t.line - 1);
        if (idx >= out.size()) continue;
        Slot &s = slot[idx];
        s.has = true;
        s.cut = t.off - line_begin;
        s.input_col = before[i].col;
        s.code_before = i > 0 && after[i - 1].line == t.line;
    }
    for (int ln : standalone_comment_lines) {
        size_t idx = static_cast<size_t>(ln - 1);
        if (idx < slot.size()) slot[idx].standalone = true;
    }

    const int gap = opts.spaces_before_trailing_comment > 0
                        ? opts.spaces_before_trailing_comment
                        : 1;
    size_t k = 0;
    while (k < out.size()) {
        if (!slot[k].has || !slot[k].code_before) {
            k++;
            continue;
        }
        // Extend the run: more code+comment lines, plus comment-only lines
        // that were written hanging to the right of their own statement's
        // indent (a wrapped trailing comment), never one sitting at it.
        size_t end = k + 1;
        while (end < out.size() && slot[end].has) {
            // A comment-only line joins only when it was written hanging to
            // the right of the statement the run started on -- that is what
            // distinguishes a trailing comment that wrapped from an ordinary
            // standalone one, and it is the same test after this pass has
            // moved it, so the answer does not change on a second run.
            // Compared against where the run's *first comment* sat, not
            // against the statement's indent: that is what tells a wrapped
            // trailing comment from a comment written inside the block the
            // run's first line opened, and -- because this pass moves both
            // of them to the same column when they do belong together -- it
            // gives the same answer the second time around.
            if (!slot[end].code_before &&
                (!slot[end].standalone || slot[end].input_col < slot[k].input_col)) {
                break;
            }
            end++;
        }
        if (end - k < 2) {
            k = end;
            continue;
        }
        int target = 0;
        bool ok = true;
        for (size_t m = k; m < end; m++) {
            if (!slot[m].code_before) continue;
            std::string code = out[m].substr(0, slot[m].cut);
            RTrim(code);
            target = std::max(target, Width(code) + gap);
        }
        for (size_t m = k; m < end && ok; m++) {
            int w = Width(out[m].substr(slot[m].cut));
            if (target + w > opts.column_limit) ok = false;
        }
        if (ok) {
            for (size_t m = k; m < end; m++) {
                std::string code = out[m].substr(0, slot[m].cut);
                RTrim(code);
                out[m] = code + Pad(target - Width(code)) + out[m].substr(slot[m].cut);
            }
        }
        k = end;
    }

    std::string joined;
    for (const std::string &l : out) {
        joined += l;
        joined += '\n';
    }
    text = joined;
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

// C++ is the easy case for this: a formatter that only moves whitespace
// cannot change the token stream, so the comparison is exact rather than
// "exact modulo the rewrites we allow" the way python_format's has to be.
// Comment *text* is the single exception -- trailing whitespace is stripped
// and a `*`-prefixed block comment's interior is re-indented, neither of
// which any compiler can see.
std::string SqueezeComment(const std::string &s) {
    std::string r;
    bool sp = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            sp = true;
            continue;
        }
        if (sp && !r.empty()) r += ' ';
        sp = false;
        r += c;
    }
    return r;
}

bool VerifyEquivalent(const std::vector<Token> &before, const std::vector<Token> &after,
                      std::string &why, int &line) {
    size_t i = 0, j = 0;
    while (i < before.size() && j < after.size()) {
        const Token &a = before[i];
        const Token &b = after[j];
        if (a.kind != b.kind) {
            why = "token `" + a.text + "` became `" + b.text + "`";
            line = a.line;
            return false;
        }
        if (IsComment(a)) {
            if (SqueezeComment(a.text) != SqueezeComment(b.text)) {
                why = "a comment changed";
                line = a.line;
                return false;
            }
        } else if (a.text != b.text) {
            why = "token `" + a.text + "` became `" + b.text + "`";
            line = a.line;
            return false;
        }
        // A token drifting onto or off a `#` line is the one way whitespace
        // changes meaning without changing the stream, so the flag is part of
        // the comparison.
        if (a.pp != b.pp) {
            why = "token `" + a.text + "` moved across a preprocessor directive";
            line = a.line;
            return false;
        }
        i++;
        j++;
    }
    if (i < before.size()) {
        why = "token `" + before[i].text + "` was dropped";
        line = before[i].line;
        return false;
    }
    if (j < after.size()) {
        why = "token `" + after[j].text + "` appeared";
        line = 0;
        return false;
    }
    return true;
}

}  // namespace

Result Format(std::string_view source, const Options &opts) {
    Result r;
    // A UTF-8 BOM is not part of the program; left in it would glue onto the
    // first token. It is put back verbatim at the end.
    std::string_view bom;
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB &&
        static_cast<unsigned char>(source[2]) == 0xBF) {
        bom = source.substr(0, 3);
        source.remove_prefix(3);
    }

    Lexer lx(source);
    if (!lx.Run()) {
        r.error = lx.error();
        r.error_line = lx.error_line();
        return r;
    }
    std::vector<Token> toks = lx.tokens();
    if (toks.empty()) {
        r.ok = true;
        r.text = std::string(bom);
        return r;
    }
    std::vector<Token> before = toks;

    Annotate(toks);
    Builder builder(toks, opts, source);
    std::vector<Line> lines = builder.Run();
    Emitter em(toks, opts);
    std::string text = em.Run(lines);
    if (opts.align_trailing_comments) {
        AlignTrailingComments(text, &before, em.standalone_comment_lines(), opts);
        // Applied twice on purpose. Run membership is read from the input's
        // columns, and this pass moves some of them; a second application
        // reads the columns it just wrote, which is exactly what a third one
        // would see, so two is a fixed point where one is not.
        AlignTrailingComments(text, nullptr, em.standalone_comment_lines(), opts);
    }

    if (opts.verify) {
        Lexer check(text);
        if (!check.Run()) {
            r.error = "internal error: formatted output does not lex (" + check.error() + ")";
            r.error_line = check.error_line();
            return r;
        }
        std::string why;
        int line = 0;
        if (!VerifyEquivalent(before, check.tokens(), why, line)) {
            r.error = "internal error: formatting would change this code (" + why + ")";
            r.error_line = line;
            return r;
        }
    }

    r.ok = true;
    r.text = std::string(bom) + text;
    return r;
}

}  // namespace cppfmt
