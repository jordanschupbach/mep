// The R reader for mep's own R language server: lexer + recursive-descent
// parser (see r_lsp_parse.h for the shape of what comes out, and why this
// is hand-written rather than a tree-sitter binding).
//
// Two rules drive almost every decision here:
//   - Newlines are significant, but only where R says they are. The lexer
//     emits one Newline token per line unconditionally and the *parser*
//     decides which to ignore: every construct that R continues across
//     lines -- inside `( )` and `[ ]`, after a binary operator, after a
//     comma, after `if (cond)`/`else`/a function header -- steps over
//     them, and nothing else does. That is what makes `x <-\n  1` one
//     statement and `x\n-1` two.
//
//     Suppressing them in the lexer instead (as R's own lexer does)
//     costs recovery: an unclosed `(` would swallow every later newline
//     in the file, so a single typo would leave the rest of the document
//     with no statement boundaries to resynchronize on, and therefore no
//     symbols, folds or completions.
//   - Recovery is per statement. A syntax error reports once, then the
//     parser skips to the next newline/`;`/`}` at the same nesting depth
//     and keeps going, so one broken line still leaves the rest of the
//     file with symbols, folds and completions.

#include "r_lsp_parse.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

/** @brief Reports whether a byte can start an R name (letters, dot, or any non-ASCII byte). */
bool IsNameStart(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalpha(u) != 0 || c == '.' || u >= 0x80;
}

/** @brief Reports whether a byte can continue an R name. */
bool IsNameChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '.' || c == '_' || u >= 0x80;
}

/** @brief Reports whether a byte is an ASCII digit. */
bool IsDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

/** @brief Maps a reserved word to its token kind, or Ident for anything else. */
RTokKind KeywordKind(const std::string &word) {
    if (word == "if") return RTokKind::If;
    if (word == "else") return RTokKind::Else;
    if (word == "for") return RTokKind::For;
    if (word == "while") return RTokKind::While;
    if (word == "repeat") return RTokKind::Repeat;
    if (word == "function") return RTokKind::Function;
    if (word == "break") return RTokKind::Break;
    if (word == "next") return RTokKind::Next;
    if (word == "in") return RTokKind::In;
    if (word == "TRUE") return RTokKind::True;
    if (word == "FALSE") return RTokKind::False;
    if (word == "NULL") return RTokKind::Null;
    if (word == "Inf") return RTokKind::Inf;
    if (word == "NaN") return RTokKind::Nan;
    if (word == "NA" || word == "NA_integer_" || word == "NA_real_" || word == "NA_character_") return RTokKind::Na;
    return RTokKind::Ident;
}

// --- Lexer ------------------------------------------------------------

class Lexer {
public:
    Lexer(const std::vector<std::string> &lines, std::vector<RComment> *comments)
        : lines_(lines), comments_(comments) {}

    /** @brief Runs the scan, returning the whole token stream (always End-terminated). */
    std::vector<RToken> Run() {
        while (li_ < lines_.size()) {
            const std::string &line = lines_[li_];
            if (ci_ >= line.size()) {
                EmitNewline();
                li_++;
                ci_ = 0;
                continue;
            }
            const char c = line[ci_];
            // Every whitespace byte, not just space and tab: R's own
            // lexer skips the form feed some older sources still use as
            // a page break, and treating one as an operator would fail
            // the whole file (found against Matrix's test-tools-1.R).
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                ci_++;
                continue;
            }
            if (c == '#') {
                Comment();
                continue;
            }
            if (c == '"' || c == '\'') {
                String(c);
                continue;
            }
            if ((c == 'r' || c == 'R') && ci_ + 1 < line.size() && (line[ci_ + 1] == '"' || line[ci_ + 1] == '\'')) {
                RawString();
                continue;
            }
            if (c == '`') {
                Backtick();
                continue;
            }
            if (IsDigit(c) || (c == '.' && ci_ + 1 < line.size() && IsDigit(line[ci_ + 1]))) {
                Number();
                continue;
            }
            if (IsNameStart(c) || c == '_') {
                Name();
                continue;
            }
            Punct();
        }
        EmitNewline();
        RToken end;
        end.kind = RTokKind::End;
        end.range = Here(0);
        toks_.push_back(end);
        return toks_;
    }

private:
    const std::vector<std::string> &lines_;
    std::vector<RComment> *comments_;
    std::vector<RToken> toks_;
    size_t li_ = 0;
    size_t ci_ = 0;

    /** @brief Builds a range on the current line spanning `width` bytes from the cursor. */
    RRange Here(size_t width) const {
        RRange r;
        r.line = static_cast<int>(li_);
        r.col = static_cast<int>(ci_);
        r.end_line = r.line;
        r.end_col = static_cast<int>(ci_ + width);
        return r;
    }

    /** @brief Appends a token of the given kind, text and range. */
    void Push(RTokKind kind, std::string text, const RRange &range) {
        RToken t;
        t.kind = kind;
        t.text = std::move(text);
        t.range = range;
        toks_.push_back(t);
    }

    /** @brief Emits a statement-separating Newline token, one per line at most. */
    void EmitNewline() {
        if (toks_.empty()) return;  // leading blank lines separate nothing
        if (toks_.back().kind == RTokKind::Newline) return;
        // Called once more after the last line is consumed, so the line
        // index has to be clamped rather than trusted.
        const size_t at = li_ < lines_.size() ? li_ : lines_.size() - 1;
        RRange r;
        r.line = static_cast<int>(at);
        r.col = static_cast<int>(lines_[at].size());
        r.end_line = r.line;
        r.end_col = r.col;
        Push(RTokKind::Newline, "\n", r);
    }

    /** @brief Consumes a `#` comment to end of line, recording it out of band. */
    void Comment() {
        const std::string &line = lines_[li_];
        RComment cm;
        cm.text = line.substr(ci_);
        cm.range = Here(line.size() - ci_);
        cm.roxygen = cm.text.rfind("#'", 0) == 0;
        if (comments_ != nullptr) comments_->push_back(cm);
        ci_ = line.size();
    }

    /** @brief Consumes a quoted string, which in R may run across lines, decoding its escapes. */
    void String(char quote) {
        const int start_line = static_cast<int>(li_);
        const int start_col = static_cast<int>(ci_);
        ci_++;  // the opening quote
        std::string value;
        bool closed = false;
        while (li_ < lines_.size()) {
            const std::string &line = lines_[li_];
            if (ci_ >= line.size()) {
                // R string literals really do span lines; the newline is
                // part of the value.
                value += '\n';
                li_++;
                ci_ = 0;
                continue;
            }
            const char c = line[ci_];
            if (c == '\\' && ci_ + 1 < line.size()) {
                value += Unescape(line[ci_ + 1]);
                ci_ += 2;
                continue;
            }
            if (c == '\\') {  // a backslash at end of line: line continuation inside the literal
                ci_ = line.size();
                continue;
            }
            ci_++;
            if (c == quote) {
                closed = true;
                break;
            }
            value += c;
        }
        RRange r;
        r.line = start_line;
        r.col = start_col;
        r.end_line = static_cast<int>(li_ < lines_.size() ? li_ : lines_.size() - 1);
        r.end_col = static_cast<int>(ci_);
        RToken t;
        t.kind = RTokKind::Str;
        t.text = value;
        t.range = r;
        t.unterminated = !closed;
        toks_.push_back(t);
    }

    /** @brief Translates one backslash escape to the byte it stands for. */
    static char Unescape(char c) {
        switch (c) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case '0': return '\0';
            default: return c;
        }
    }

    /** @brief Consumes an r"(...)"-style raw string, including its dashed r"---(...)---" form. */
    void RawString() {
        const std::string &line = lines_[li_];
        const int start_line = static_cast<int>(li_);
        const int start_col = static_cast<int>(ci_);
        const char quote = line[ci_ + 1];
        size_t p = ci_ + 2;
        std::string dashes;
        while (p < line.size() && line[p] == '-') {
            dashes += '-';
            p++;
        }
        char open = p < line.size() ? line[p] : '(';
        char close = open == '(' ? ')' : (open == '[' ? ']' : (open == '{' ? '}' : ')'));
        if (open != '(' && open != '[' && open != '{') {
            // Not actually a raw string (`r` followed by a quote but no
            // delimiter): fall back to lexing the name and the string
            // separately, which is what R's own lexer would reject but is
            // the least surprising recovery.
            Name();
            return;
        }
        p++;
        const std::string terminator = std::string(1, close) + dashes + std::string(1, quote);
        std::string value;
        bool closed = false;
        li_ = static_cast<size_t>(start_line);
        ci_ = p;
        while (li_ < lines_.size()) {
            const std::string &cur = lines_[li_];
            if (ci_ >= cur.size()) {
                value += '\n';
                li_++;
                ci_ = 0;
                continue;
            }
            if (cur.compare(ci_, terminator.size(), terminator) == 0) {
                ci_ += terminator.size();
                closed = true;
                break;
            }
            value += cur[ci_];
            ci_++;
        }
        RRange r;
        r.line = start_line;
        r.col = start_col;
        r.end_line = static_cast<int>(li_ < lines_.size() ? li_ : lines_.size() - 1);
        r.end_col = static_cast<int>(ci_);
        RToken t;
        t.kind = RTokKind::Str;
        t.text = value;
        t.range = r;
        t.unterminated = !closed;
        toks_.push_back(t);
    }

    /** @brief Consumes a `backticked name`, which may contain anything but a backtick. */
    void Backtick() {
        const std::string &line = lines_[li_];
        const int start_col = static_cast<int>(ci_);
        size_t p = ci_ + 1;
        std::string value;
        bool closed = false;
        while (p < line.size()) {
            if (line[p] == '`') {
                closed = true;
                p++;
                break;
            }
            value += line[p];
            p++;
        }
        RRange r;
        r.line = static_cast<int>(li_);
        r.col = start_col;
        r.end_line = r.line;
        r.end_col = static_cast<int>(p);
        RToken t;
        t.kind = RTokKind::Ident;
        t.text = value;
        t.range = r;
        t.backticked = true;
        t.unterminated = !closed;
        toks_.push_back(t);
        ci_ = p;
    }

    /** @brief Consumes a numeric literal: decimal, hex, exponent form, with an optional L/i suffix. */
    void Number() {
        const std::string &line = lines_[li_];
        const size_t start = ci_;
        if (line[ci_] == '0' && ci_ + 1 < line.size() && (line[ci_ + 1] == 'x' || line[ci_ + 1] == 'X')) {
            ci_ += 2;
            while (ci_ < line.size() && (std::isxdigit(static_cast<unsigned char>(line[ci_])) != 0)) ci_++;
        } else {
            while (ci_ < line.size() && IsDigit(line[ci_])) ci_++;
            if (ci_ < line.size() && line[ci_] == '.') {
                ci_++;
                while (ci_ < line.size() && IsDigit(line[ci_])) ci_++;
            }
            if (ci_ < line.size() && (line[ci_] == 'e' || line[ci_] == 'E')) {
                size_t p = ci_ + 1;
                if (p < line.size() && (line[p] == '+' || line[p] == '-')) p++;
                if (p < line.size() && IsDigit(line[p])) {
                    ci_ = p;
                    while (ci_ < line.size() && IsDigit(line[ci_])) ci_++;
                }
            }
        }
        if (ci_ < line.size() && (line[ci_] == 'L' || line[ci_] == 'i')) ci_++;
        RRange r;
        r.line = static_cast<int>(li_);
        r.col = static_cast<int>(start);
        r.end_line = r.line;
        r.end_col = static_cast<int>(ci_);
        Push(RTokKind::Num, line.substr(start, ci_ - start), r);
    }

    /** @brief Consumes a name or reserved word, including the `...`/`..1` dot forms. */
    void Name() {
        const std::string &line = lines_[li_];
        const size_t start = ci_;
        while (ci_ < line.size() && IsNameChar(line[ci_])) ci_++;
        if (ci_ == start) ci_++;  // a lone `_`: still one token, never a zero-width one
        const std::string word = line.substr(start, ci_ - start);
        RRange r;
        r.line = static_cast<int>(li_);
        r.col = static_cast<int>(start);
        r.end_line = r.line;
        r.end_col = static_cast<int>(ci_);
        RTokKind kind = KeywordKind(word);
        if (word == "..." || (word.size() > 2 && word[0] == '.' && word[1] == '.' && IsDigit(word[2]))) {
            kind = RTokKind::Dots;
        }
        Push(kind, word, r);
    }

    /** @brief Consumes one operator or punctuation token, longest match first. */
    void Punct() {
        const std::string &line = lines_[li_];
        const char c = line[ci_];
        const auto two = [&](const char *s) { return line.compare(ci_, 2, s) == 0; };
        const auto three = [&](const char *s) { return line.compare(ci_, 3, s) == 0; };

        if (c == '%') {
            SpecialOp();
            return;
        }
        if (c == '\\' && ci_ + 1 < line.size() && line[ci_ + 1] == '(') {
            // The `\(x) x` lambda shorthand: one `function` token, so the
            // parser never needs to know which spelling was used.
            Push(RTokKind::Function, "\\", Here(1));
            ci_++;
            return;
        }
        struct Simple {
            char ch;
            RTokKind kind;
        };
        static const Simple kSimple[] = {
            {'(', RTokKind::LParen}, {')', RTokKind::RParen},   {'{', RTokKind::LBrace},
            {'}', RTokKind::RBrace}, {']', RTokKind::RBracket}, {',', RTokKind::Comma},
            {';', RTokKind::Semi},
        };
        for (const Simple &s : kSimple) {
            if (c != s.ch) continue;
            Push(s.kind, std::string(1, c), Here(1));
            ci_++;
            return;
        }
        if (c == '[') {
            const bool dbl = ci_ + 1 < line.size() && line[ci_ + 1] == '[';
            Push(dbl ? RTokKind::DblLBracket : RTokKind::LBracket, dbl ? "[[" : "[", Here(dbl ? 2 : 1));
            ci_ += dbl ? 2 : 1;
            return;
        }
        static const char *const kThree[] = {"<<-", "->>", ":::"};
        for (const char *op : kThree) {
            if (!three(op)) continue;
            Push(RTokKind::Op, op, Here(3));
            ci_ += 3;
            return;
        }
        // `**` is R's own deprecated spelling of `^`, still accepted by
        // its parser (and still in the wild -- RcppArmadillo's test suite
        // uses it), so it lexes as one operator rather than two `*`.
        static const char *const kTwo[] = {"<-", "->", "<=", ">=", "==", "!=", "&&",
                                           "||", "|>", "::", ":=", "%%", "**"};
        for (const char *op : kTwo) {
            if (!two(op)) continue;
            Push(RTokKind::Op, op, Here(2));
            ci_ += 2;
            return;
        }
        Push(RTokKind::Op, std::string(1, c), Here(1));
        ci_++;
    }

    /** @brief Consumes a `%...%` special operator, tolerating an unterminated one as a single token. */
    void SpecialOp() {
        const std::string &line = lines_[li_];
        const size_t start = ci_;
        size_t p = ci_ + 1;
        while (p < line.size() && line[p] != '%') p++;
        const bool closed = p < line.size();
        if (closed) p++;
        RRange r;
        r.line = static_cast<int>(li_);
        r.col = static_cast<int>(start);
        r.end_line = r.line;
        r.end_col = static_cast<int>(p);
        RToken t;
        t.kind = RTokKind::Op;
        t.text = line.substr(start, p - start);
        t.range = r;
        t.unterminated = !closed;
        toks_.push_back(t);
        ci_ = p;
    }
};

}  // namespace

bool RRangeContains(const RRange &r, int line, int col) {
    if (line < r.line || line > r.end_line) return false;
    if (line == r.line && col < r.col) return false;
    if (line == r.end_line && col >= r.end_col) return false;
    return true;
}

const RNode &RParseResult::at(int index) const {
    static const RNode kEmpty;
    if (index < 0 || static_cast<size_t>(index) >= nodes.size()) return kEmpty;
    return nodes[static_cast<size_t>(index)];
}

std::vector<RToken> RLspTokenize(const std::vector<std::string> &lines, std::vector<RComment> *comments_out) {
    if (lines.empty()) {
        static const std::vector<std::string> kOneEmptyLine{std::string()};
        Lexer lexer(kOneEmptyLine, comments_out);
        return lexer.Run();
    }
    Lexer lexer(lines, comments_out);
    return lexer.Run();
}

namespace {

// R's operator precedence, as ?Syntax lists it, lowest binding first.
// Postfix (`::`, `$`, `@`, `[`, `[[`, a call's `(`) binds tighter than
// anything here and is handled by the postfix loop instead.
constexpr int kPrecHelp = 1;    // ?
constexpr int kPrecEq = 2;      // =  (assignment, not a named argument)
constexpr int kPrecAssign = 3;  // <- <<- :=
constexpr int kPrecRight = 4;   // -> ->>
constexpr int kPrecTilde = 5;   // ~
constexpr int kPrecOr = 6;      // || |
constexpr int kPrecAnd = 7;     // && &
constexpr int kPrecNot = 8;     // ! (unary)
constexpr int kPrecCompare = 9;
constexpr int kPrecAdd = 10;
constexpr int kPrecMul = 11;
constexpr int kPrecSpecial = 12;  // %any% and |>
constexpr int kPrecColon = 13;
constexpr int kPrecUnary = 14;  // unary - +
constexpr int kPrecPower = 15;  // ^

// The precedence an argument's value is parsed at: above `=`, so that the
// `=` in `f(a = 1)` stays a named-argument marker instead of being eaten
// as an assignment operator.
constexpr int kPrecArg = kPrecAssign;

// A document with this many syntax errors is not one more error away from
// being understood; past the cap the parser keeps recovering but stops
// reporting, so a file that is actually some other language does not
// produce a wall of underlines.
constexpr size_t kMaxErrors = 25;

/**
 * @brief Looks up a token's binary precedence.
 * @param tok the operator token to classify
 * @param right_assoc set to whether the operator associates to the right
 * @return the precedence, or -1 when the token is not a binary operator
 */
int BinaryPrec(const RToken &tok, bool *right_assoc) {
    *right_assoc = false;
    if (tok.kind != RTokKind::Op) return -1;
    const std::string &s = tok.text;
    if (s == "?") return kPrecHelp;
    if (s == "=") {
        *right_assoc = true;
        return kPrecEq;
    }
    if (s == "<-" || s == "<<-" || s == ":=") {
        *right_assoc = true;
        return kPrecAssign;
    }
    if (s == "->" || s == "->>") return kPrecRight;
    if (s == "~") return kPrecTilde;
    if (s == "||" || s == "|") return kPrecOr;
    if (s == "&&" || s == "&") return kPrecAnd;
    if (s == "==" || s == "!=" || s == "<" || s == ">" || s == "<=" || s == ">=") return kPrecCompare;
    if (s == "+" || s == "-") return kPrecAdd;
    if (s == "*" || s == "/") return kPrecMul;
    if (s == "|>" || (s.size() >= 2 && s[0] == '%')) return kPrecSpecial;
    if (s == ":") return kPrecColon;
    if (s == "^" || s == "**") {
        *right_assoc = true;
        return kPrecPower;
    }
    return -1;
}

/** @brief Names a token for an error message, in the spelling a person would recognize. */
std::string Describe(const RToken &tok) {
    switch (tok.kind) {
        case RTokKind::End: return "end of file";
        case RTokKind::Newline: return "end of line";
        case RTokKind::Str: return "a string";
        case RTokKind::Num: return "a number";
        case RTokKind::Ident: return "'" + tok.text + "'";
        default: return "'" + tok.text + "'";
    }
}

class Parser {
public:
    explicit Parser(std::vector<RToken> toks) : toks_(std::move(toks)) {}

    /** @brief Parses the whole token stream into top-level expressions. */
    RParseResult Run() {
        while (!At(RTokKind::End)) {
            if (At(RTokKind::Newline) || At(RTokKind::Semi)) {
                Advance();
                continue;
            }
            if (At(RTokKind::RBrace) || At(RTokKind::RParen) || At(RTokKind::RBracket)) {
                const RToken &tok = Cur();
                Error(tok.range, "unmatched-bracket", "Unmatched " + Describe(tok) + ".");
                Advance();
                continue;
            }
            const size_t before = pos_;
            const int stmt = ParseExpr(kPrecHelp);
            if (stmt >= 0) out_.roots.push_back(stmt);
            EndStatement();
            if (pos_ == before) Advance();  // never spin on a token nothing consumes
        }
        return std::move(out_);
    }

private:
    std::vector<RToken> toks_;
    size_t pos_ = 0;
    RParseResult out_;
    int brace_depth_ = 0;
    // >0 while inside `( )` or `[ ]`, where R ignores line breaks. A
    // braced block nested inside one of those resets it to 0 for the
    // block's own statements (NlGuard/BlockGuard below).
    int nl_suppress_ = 0;

    // Raises newline suppression for the extent of a parenthesized or
    // bracketed construct. Scope-based on purpose: several of the parse
    // helpers return early on a missing ')', and suppression must be
    // dropped on those paths too or the rest of the file would lose its
    // statement boundaries.
    class NlGuard {
    public:
        explicit NlGuard(Parser *parser) : parser_(parser) { parser_->nl_suppress_++; }
        NlGuard(const NlGuard &) = delete;
        NlGuard(NlGuard &&) = delete;
        NlGuard &operator=(const NlGuard &) = delete;
        NlGuard &operator=(NlGuard &&) = delete;
        ~NlGuard() { parser_->nl_suppress_--; }

    private:
        Parser *parser_;
    };

    // The inverse, for a `{ ... }` block: inside the braces newlines
    // separate statements again, however deeply nested in calls it is.
    class BlockGuard {
    public:
        explicit BlockGuard(Parser *parser) : parser_(parser), saved_(parser->nl_suppress_) {
            parser_->nl_suppress_ = 0;
        }
        BlockGuard(const BlockGuard &) = delete;
        BlockGuard(BlockGuard &&) = delete;
        BlockGuard &operator=(const BlockGuard &) = delete;
        BlockGuard &operator=(BlockGuard &&) = delete;
        ~BlockGuard() { parser_->nl_suppress_ = saved_; }

    private:
        Parser *parser_;
        int saved_;
    };

    // --- Token access --------------------------------------------------
    //
    // Cur()/Peek()/At() step over newlines while inside a construct R
    // continues across lines (see NlGuard); RawCur() deliberately does
    // not, because error recovery resynchronizes *on* those newlines.

    /** @brief The token at the cursor, past any newline the current construct ignores. */
    const RToken &Cur() {
        Normalize();
        return toks_[pos_];
    }
    /** @brief The token at the cursor, newlines included. */
    const RToken &RawCur() const { return toks_[pos_]; }
    /** @brief The token `ahead` positions past the cursor (clamped to the End token). */
    const RToken &Peek(size_t ahead) {
        Normalize();
        const size_t at = pos_ + ahead;
        return at < toks_.size() ? toks_[at] : toks_.back();
    }
    /** @brief Reports whether the cursor is on a token of the given kind. */
    bool At(RTokKind kind) { return Cur().kind == kind; }
    /** @brief Reports whether the cursor is on a given operator. */
    bool AtOp(const char *text) { return Cur().kind == RTokKind::Op && Cur().text == text; }
    /** @brief Steps the cursor over newlines the enclosing construct ignores. */
    void Normalize() {
        while (nl_suppress_ > 0 && toks_[pos_].kind == RTokKind::Newline && pos_ + 1 < toks_.size()) pos_++;
    }
    /** @brief Consumes and returns the current token. */
    RToken Advance() {
        const RToken tok = Cur();
        if (pos_ + 1 < toks_.size()) pos_++;
        return tok;
    }
    /** @brief Steps over statement-separating newlines (used wherever an expression is known to continue). */
    void SkipNewlines() {
        while (At(RTokKind::Newline)) Advance();
    }
    /** @brief The end of the last consumed token, for closing a node's range. */
    RRange PrevRange() const { return pos_ > 0 ? toks_[pos_ - 1].range : toks_[0].range; }

    // --- Nodes ---------------------------------------------------------

    /** @brief Appends a node to the arena and returns its index. */
    int New(RNodeKind kind, const RRange &range, std::string text = std::string()) {
        RNode node;
        node.kind = kind;
        node.range = range;
        node.text = std::move(text);
        out_.nodes.push_back(std::move(node));
        return static_cast<int>(out_.nodes.size()) - 1;
    }
    /** @brief Borrows a node for mutation (only valid until the next New()). */
    RNode &Node(int index) { return out_.nodes[static_cast<size_t>(index)]; }
    /** @brief Builds the range spanning from one range's start to another's end. */
    static RRange Span(const RRange &from, const RRange &to) {
        RRange r = from;
        r.end_line = to.end_line;
        r.end_col = to.end_col;
        return r;
    }
    /** @brief Closes a node's range at the last consumed token. */
    void Close(int index, const RRange &start) { Node(index).range = Span(start, PrevRange()); }

    // --- Errors --------------------------------------------------------

    /** @brief Records one syntax error, up to the per-document cap. */
    void Error(const RRange &range, const char *code, std::string message) {
        if (out_.errors.size() >= kMaxErrors) return;
        RParseError err;
        err.range = range;
        err.code = code;
        err.message = std::move(message);
        out_.errors.push_back(std::move(err));
    }

    /** @brief Consumes an expected token, reporting and reporting-only (no skip) when it is missing. */
    bool Expect(RTokKind kind, const char *code, const std::string &what) {
        if (At(kind)) {
            Advance();
            return true;
        }
        Error(Cur().range, code, "Expected " + what + " but found " + Describe(Cur()) + ".");
        return false;
    }

    /** @brief Reports whether the raw cursor sits on something that ends a statement. */
    bool AtStatementEnd() const {
        const RTokKind kind = RawCur().kind;
        return kind == RTokKind::End || kind == RTokKind::Newline || kind == RTokKind::Semi ||
               kind == RTokKind::RBrace;
    }

    /** @brief Skips to the next statement boundary after an error, leaving the boundary itself unconsumed. */
    void Sync() {
        while (!AtStatementEnd()) Advance();
    }

    /** @brief Checks that a statement really ended where it did, recovering when it did not. */
    void EndStatement() {
        if (AtStatementEnd()) return;
        Error(RawCur().range, "syntax-error", "Unexpected " + Describe(RawCur()) + " after a complete expression.");
        Sync();
    }

    // --- Expressions ---------------------------------------------------

    /**
     * @brief Parses an expression by precedence climbing.
     * @param min_prec the lowest operator precedence this call may absorb
     * @return the node index, or -1 when nothing could be parsed
     */
    int ParseExpr(int min_prec) {
        int lhs = ParseUnary();
        if (lhs < 0) return -1;
        while (true) {
            bool right = false;
            const int prec = BinaryPrec(Cur(), &right);
            if (prec < 0 || prec < min_prec) break;
            const RToken op = Advance();
            if (op.unterminated) {
                Error(op.range, "unterminated-operator", "The special operator " + op.text + " is missing its '%'.");
            }
            SkipNewlines();  // a trailing operator continues the expression onto the next line
            const int rhs = ParseExpr(right ? prec : prec + 1);
            const RRange start = Node(lhs).range;
            const int node = New(RNodeKind::Binary, start, op.text);
            Node(node).kids.push_back(lhs);
            if (rhs >= 0) {
                Node(node).kids.push_back(rhs);
                Node(node).range = Span(start, Node(rhs).range);
            } else {
                Error(op.range, "missing-operand", "Nothing follows the operator '" + op.text + "'.");
                Node(node).range = Span(start, op.range);
            }
            lhs = node;
        }
        return lhs;
    }

    /** @brief Parses a prefix operator, or falls through to a postfixed primary. */
    int ParseUnary() {
        if (Cur().kind == RTokKind::Op) {
            const std::string &s = Cur().text;
            int operand_prec = -1;
            if (s == "-" || s == "+") {
                operand_prec = kPrecUnary;
            } else if (s == "!") {
                operand_prec = kPrecNot + 1;
            } else if (s == "~") {
                operand_prec = kPrecTilde;
            } else if (s == "?") {
                operand_prec = kPrecHelp;
            }
            if (operand_prec > 0) {
                const RToken op = Advance();
                SkipNewlines();
                const int operand = ParseExpr(operand_prec);
                const int node = New(RNodeKind::Unary, op.range, op.text);
                if (operand >= 0) {
                    Node(node).kids.push_back(operand);
                    Node(node).range = Span(op.range, Node(operand).range);
                } else {
                    Error(op.range, "missing-operand", "Nothing follows the operator '" + op.text + "'.");
                }
                return node;
            }
        }
        const int primary = ParsePrimary();
        if (primary < 0) return -1;
        return ParsePostfix(primary);
    }

    /** @brief Applies every postfix construct that follows an expression: calls, subscripts, `$`, `@`, `::`. */
    int ParsePostfix(int expr) {
        int lhs = expr;
        while (true) {
            if (At(RTokKind::LParen)) {
                const RRange start = Node(lhs).range;
                Advance();
                const NlGuard inside_call(this);
                const int node = New(RNodeKind::Call, start);
                Node(node).kids.push_back(lhs);
                std::vector<RArg> args;
                ParseArgs(RTokKind::RParen, &args);
                Node(node).args = std::move(args);
                if (!Expect(RTokKind::RParen, "unclosed-call", "')' to close the call")) {
                    Close(node, start);
                    return node;
                }
                Close(node, start);
                lhs = node;
                continue;
            }
            if (At(RTokKind::LBracket) || At(RTokKind::DblLBracket)) {
                const bool dbl = At(RTokKind::DblLBracket);
                const RRange start = Node(lhs).range;
                Advance();
                const NlGuard inside_index(this);
                const int node = New(RNodeKind::Index, start, dbl ? "[[" : "[");
                Node(node).kids.push_back(lhs);
                std::vector<RArg> args;
                ParseArgs(RTokKind::RBracket, &args);
                Node(node).args = std::move(args);
                const bool closed = Expect(RTokKind::RBracket, "unclosed-index",
                                           dbl ? "']]' to close the subscript" : "']' to close the subscript");
                if (dbl && closed) Expect(RTokKind::RBracket, "unclosed-index", "a second ']' to close '[['");
                Close(node, start);
                if (!closed) return node;
                lhs = node;
                continue;
            }
            if (AtOp("$") || AtOp("@") || AtOp("::") || AtOp(":::")) {
                const RRange start = Node(lhs).range;
                const RToken op = Advance();
                SkipNewlines();
                int rhs = -1;
                if (At(RTokKind::Ident) || At(RTokKind::Str)) {
                    const RToken name = Advance();
                    rhs = New(name.kind == RTokKind::Str ? RNodeKind::Str : RNodeKind::Ident, name.range, name.text);
                } else if (At(RTokKind::LParen)) {
                    rhs = ParsePrimary();  // x$(dynamic) -- rare but legal
                } else {
                    Error(Cur().range, "syntax-error",
                          "Expected a name after '" + op.text + "' but found " + Describe(Cur()) + ".");
                }
                const int node = New(RNodeKind::Binary, start, op.text);
                Node(node).kids.push_back(lhs);
                if (rhs >= 0) Node(node).kids.push_back(rhs);
                Close(node, start);
                lhs = node;
                continue;
            }
            return lhs;
        }
    }

    /**
     * @brief Parses a comma-separated argument list, including R's empty slots (`x[, 1]`, `switch(a = , 1)`).
     * @param closing the token kind that ends the list (not consumed here)
     * @param out receives one entry per argument, in order
     */
    void ParseArgs(RTokKind closing, std::vector<RArg> *out) {
        SkipNewlines();
        if (At(closing) || At(RTokKind::End)) return;
        while (true) {
            out->push_back(ParseOneArg(closing));
            SkipNewlines();
            if (At(RTokKind::Comma)) {
                Advance();
                SkipNewlines();
                continue;
            }
            break;
        }
    }

    /** @brief Parses one argument: an optional `name =` followed by an optional value. */
    RArg ParseOneArg(RTokKind closing) {
        RArg arg;
        const bool named = (At(RTokKind::Ident) || At(RTokKind::Str) || At(RTokKind::Dots)) &&
                           Peek(1).kind == RTokKind::Op && Peek(1).text == "=";
        if (named) {
            const RToken name = Advance();
            arg.name = name.text;
            arg.name_range = name.range;
            Advance();  // the '='
            SkipNewlines();
        }
        if (At(RTokKind::Comma) || At(closing) || At(RTokKind::End)) {
            arg.value = -1;  // an empty slot: `x[, 1]`, or `switch(a = , b = 1)`
            return arg;
        }
        arg.value = ParseExpr(kPrecArg);
        if (arg.value < 0) Sync();
        return arg;
    }

    /** @brief Parses a literal, name, keyword construct, block or parenthesized expression. */
    int ParsePrimary() {
        const RToken tok = Cur();
        switch (tok.kind) {
            case RTokKind::Num: Advance(); return New(RNodeKind::Num, tok.range, tok.text);
            case RTokKind::Str:
                Advance();
                if (tok.unterminated) {
                    Error(tok.range, "unterminated-string", "This string literal is never closed.");
                }
                return New(RNodeKind::Str, tok.range, tok.text);
            case RTokKind::True:
            case RTokKind::False: Advance(); return New(RNodeKind::Bool, tok.range, tok.text);
            case RTokKind::Null: Advance(); return New(RNodeKind::Null, tok.range, tok.text);
            case RTokKind::Na: Advance(); return New(RNodeKind::Na, tok.range, tok.text);
            case RTokKind::Inf: Advance(); return New(RNodeKind::Inf, tok.range, tok.text);
            case RTokKind::Nan: Advance(); return New(RNodeKind::Nan, tok.range, tok.text);
            case RTokKind::Dots: Advance(); return New(RNodeKind::Dots, tok.range, tok.text);
            case RTokKind::Break: Advance(); return New(RNodeKind::Break, tok.range, tok.text);
            case RTokKind::Next: Advance(); return New(RNodeKind::Next, tok.range, tok.text);
            case RTokKind::Ident:
                Advance();
                if (tok.unterminated) {
                    Error(tok.range, "unterminated-name", "This `backquoted name` is never closed.");
                }
                return New(RNodeKind::Ident, tok.range, tok.text);
            case RTokKind::LParen: return ParseParen();
            case RTokKind::LBrace: return ParseBlock();
            case RTokKind::If: return ParseIf();
            case RTokKind::For: return ParseFor();
            case RTokKind::While: return ParseWhile();
            case RTokKind::Repeat: return ParseRepeat();
            case RTokKind::Function: return ParseFunction();
            case RTokKind::Else:
                Advance();
                Error(tok.range, "dangling-else", "'else' without a matching 'if'.");
                return New(RNodeKind::Error, tok.range, tok.text);
            default: break;
        }
        Error(tok.range, "syntax-error", "Unexpected " + Describe(tok) + " where an expression was expected.");
        return -1;
    }

    /** @brief Parses `( expr )`. */
    int ParseParen() {
        const RRange start = Cur().range;
        Advance();
        const NlGuard inside_paren(this);
        SkipNewlines();
        const int inner = ParseExpr(kPrecHelp);
        SkipNewlines();
        const int node = New(RNodeKind::Paren, start);
        if (inner >= 0) Node(node).kids.push_back(inner);
        Expect(RTokKind::RParen, "unclosed-paren", "')' to close '('");
        Close(node, start);
        return node;
    }

    /** @brief Parses a `{ ... }` block, one statement per newline or `;`. */
    int ParseBlock() {
        const RRange start = Cur().range;
        Advance();
        const BlockGuard newlines_matter_again(this);
        brace_depth_++;
        const int node = New(RNodeKind::Block, start);
        while (!At(RTokKind::RBrace) && !At(RTokKind::End)) {
            if (At(RTokKind::Newline) || At(RTokKind::Semi)) {
                Advance();
                continue;
            }
            const size_t before = pos_;
            const int stmt = ParseExpr(kPrecHelp);
            if (stmt >= 0) Node(node).kids.push_back(stmt);
            EndStatement();
            if (pos_ == before) Advance();
        }
        brace_depth_--;
        Expect(RTokKind::RBrace, "unclosed-brace", "'}' to close '{'");
        Close(node, start);
        return node;
    }

    /** @brief Parses `if (cond) expr` and its optional `else` branch. */
    int ParseIf() {
        const RRange start = Cur().range;
        Advance();
        const int node = New(RNodeKind::If, start);
        if (!Expect(RTokKind::LParen, "syntax-error", "'(' after 'if'")) {
            Close(node, start);
            return node;
        }
        {
            const NlGuard inside_header(this);
            const int cond = ParseExpr(kPrecHelp);
            if (cond >= 0) Node(node).kids.push_back(cond);
            Expect(RTokKind::RParen, "unclosed-paren", "')' to close the 'if' condition");
        }
        SkipNewlines();
        const int then_branch = ParseExpr(kPrecHelp);
        if (then_branch >= 0) Node(node).kids.push_back(then_branch);
        // `else` may sit on its own line inside a block, but at top level
        // R has already ended the statement by then -- so report that
        // rather than silently accepting a file R itself would reject.
        const size_t save = pos_;
        bool crossed_newline = false;
        while (At(RTokKind::Newline)) {
            crossed_newline = true;
            Advance();
        }
        if (At(RTokKind::Else)) {
            const RToken else_tok = Cur();
            if (crossed_newline && brace_depth_ == 0) {
                Error(else_tok.range, "else-at-top-level",
                      "At top level 'else' must be on the same line as the end of the 'if' branch.");
            }
            Advance();
            SkipNewlines();
            const int else_branch = ParseExpr(kPrecHelp);
            if (else_branch >= 0) Node(node).kids.push_back(else_branch);
        } else {
            pos_ = save;  // the newlines belong to the enclosing statement list
        }
        Close(node, start);
        return node;
    }

    /** @brief Parses `for (var in seq) body`. */
    int ParseFor() {
        const RRange start = Cur().range;
        Advance();
        const int node = New(RNodeKind::For, start);
        if (!Expect(RTokKind::LParen, "syntax-error", "'(' after 'for'")) {
            Close(node, start);
            return node;
        }
        {
        const NlGuard inside_header(this);
        // The new node has to exist before `node` is borrowed again:
        // New() can reallocate the arena, which would leave a reference
        // taken across the call dangling.
        int var_node = -1;
        if (At(RTokKind::Ident)) {
            const RToken var = Advance();
            var_node = New(RNodeKind::Ident, var.range, var.text);
        } else {
            Error(Cur().range, "syntax-error", "Expected a loop variable but found " + Describe(Cur()) + ".");
            var_node = New(RNodeKind::Error, Cur().range);
        }
        Node(node).kids.push_back(var_node);
        SkipNewlines();
        Expect(RTokKind::In, "syntax-error", "'in' after the loop variable");
        SkipNewlines();
        const int seq = ParseExpr(kPrecHelp);
        if (seq >= 0) Node(node).kids.push_back(seq);
        Expect(RTokKind::RParen, "unclosed-paren", "')' to close the 'for' header");
        }
        SkipNewlines();
        const int body = ParseExpr(kPrecHelp);
        if (body >= 0) Node(node).kids.push_back(body);
        Close(node, start);
        return node;
    }

    /** @brief Parses `while (cond) body`. */
    int ParseWhile() {
        const RRange start = Cur().range;
        Advance();
        const int node = New(RNodeKind::While, start);
        if (!Expect(RTokKind::LParen, "syntax-error", "'(' after 'while'")) {
            Close(node, start);
            return node;
        }
        {
            const NlGuard inside_header(this);
            const int cond = ParseExpr(kPrecHelp);
            if (cond >= 0) Node(node).kids.push_back(cond);
            Expect(RTokKind::RParen, "unclosed-paren", "')' to close the 'while' condition");
        }
        SkipNewlines();
        const int body = ParseExpr(kPrecHelp);
        if (body >= 0) Node(node).kids.push_back(body);
        Close(node, start);
        return node;
    }

    /** @brief Parses `repeat body`. */
    int ParseRepeat() {
        const RRange start = Cur().range;
        Advance();
        SkipNewlines();
        const int node = New(RNodeKind::Repeat, start);
        const int body = ParseExpr(kPrecHelp);
        if (body >= 0) Node(node).kids.push_back(body);
        Close(node, start);
        return node;
    }

    /** @brief Parses `function(formals) body`, including the `\(x)` shorthand. */
    int ParseFunction() {
        const RRange start = Cur().range;
        const RToken kw = Advance();
        const int node = New(RNodeKind::Function, start, kw.text);
        if (!Expect(RTokKind::LParen, "syntax-error", "'(' after 'function'")) {
            Close(node, start);
            return node;
        }
        std::vector<RArg> formals;
        {
        const NlGuard inside_header(this);
        if (!At(RTokKind::RParen)) {
            while (true) {
                RArg formal;
                if (At(RTokKind::Ident) || At(RTokKind::Dots)) {
                    const RToken name = Advance();
                    formal.name = name.text;
                    formal.name_range = name.range;
                } else if (At(RTokKind::RParen) || At(RTokKind::Comma)) {
                    Error(Cur().range, "syntax-error", "Empty formal argument in the function header.");
                } else {
                    Error(Cur().range, "syntax-error",
                          "Expected a formal argument name but found " + Describe(Cur()) + ".");
                    Sync();
                    break;
                }
                SkipNewlines();
                if (AtOp("=")) {
                    Advance();
                    SkipNewlines();
                    formal.value = ParseExpr(kPrecArg);
                    SkipNewlines();
                }
                formals.push_back(formal);
                if (At(RTokKind::Comma)) {
                    Advance();
                    SkipNewlines();
                    continue;
                }
                break;
            }
        }
        Node(node).args = std::move(formals);
        Expect(RTokKind::RParen, "unclosed-paren", "')' to close the function header");
        }
        SkipNewlines();
        const int body = ParseExpr(kPrecHelp);
        if (body >= 0) {
            Node(node).kids.push_back(body);
        } else {
            Error(PrevRange(), "syntax-error", "This function has no body.");
        }
        Close(node, start);
        return node;
    }
};

}  // namespace

RParseResult RLspParse(const std::vector<std::string> &lines) {
    std::vector<RComment> comments;
    std::vector<RToken> toks = RLspTokenize(lines, &comments);
    Parser parser(std::move(toks));
    RParseResult result = parser.Run();
    result.comments = std::move(comments);
    return result;
}
