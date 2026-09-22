// mep's own Python language server, front end: tokenizer and parser (see
// python_ast.h for the scope statement and position conventions).
//
// The tokenizer is the part with the Python-specific difficulty in it.
// Python's lexical structure is not line-oriented the way it looks: a
// logical line ends at a newline only when no bracket is open and no
// backslash continues it, indentation is significant but only at the
// start of a logical line, and a triple-quoted string swallows newlines
// whole. So the scanner runs over a (line, column) cursor across the
// whole file rather than line by line, and synthesizes the three tokens
// that have no text of their own -- NEWLINE, INDENT, DEDENT -- exactly
// where CPython's own tokenizer does.
//
// The parser is ordinary recursive descent with precedence climbing for
// binary operators. Its one unusual obligation is error recovery: this
// runs on a file while it is being typed, so "stop at the first error"
// would mean losing every symbol below a half-written line. Instead a
// failed statement is reported, skipped (with its indented block, if it
// had started one), and parsing resumes at the next logical line.

#include "python_ast.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace {

// --- Character classes ------------------------------------------------

/** @brief Reports whether a byte is an ASCII decimal digit. */
bool IsDigit(unsigned char c) { return c >= '0' && c <= '9'; }

/** @brief Reports whether a byte is an ASCII hex digit. */
bool IsHexDigit(unsigned char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// The reserved words, in one place. Soft keywords (`match`, `case`,
// `type`, `_`) are deliberately absent: they are ordinary identifiers
// everywhere except one grammatical position each, and treating them as
// keywords would break `match = re.match(...)`, which is real code.
const std::set<std::string> &KeywordSet() {
    static const std::set<std::string> kKeywords = {
        "False", "None",   "True",  "and",      "as",       "assert", "async", "await",    "break",
        "class", "continue", "def", "del",      "elif",     "else",   "except", "finally", "for",
        "from",  "global", "if",    "import",   "in",       "is",     "lambda", "nonlocal", "not",
        "or",    "pass",   "raise", "return",   "try",      "while",  "with",   "yield",
    };
    return kKeywords;
}

// Multi-byte operators, longest first: the scanner tries them in this
// order, so `**=` is never mis-read as `**` followed by `=`.
const char *const kOperators[] = {
    "**=", "//=", ">>=", "<<=", "...", "!=", ">=", "<=", "==", "->", ":=", "+=", "-=", "*=",
    "/=",  "%=",  "&=",  "|=",  "^=",  "@=", "**", "//", "<<", ">>", "<>",
};

// Single-character operators and delimiters.
const char *const kSingleOps = "+-*/%@&|^~<>()[]{},:.;=";

// --- Lexer ------------------------------------------------------------

// A cursor over the whole file. `Cur()` returns '\n' at the end of every
// line and '\0' past the last one, so a scanner that walks off the end of
// a line (a triple-quoted string, say) sees a terminator rather than
// silently wrapping.
class Lexer {
public:
    Lexer(const std::vector<std::string> &lines, std::vector<PySyntaxError> *errors,
          std::vector<PyComment> *comments, std::vector<int> *depth, bool recover = false)
        : lines_(lines), errors_(errors), comments_(comments), depth_(depth), recover_(recover) {}

    /** @brief Runs the scan, returning the full token stream (always End-terminated). */
    std::vector<PyToken> Run();

private:
    const std::vector<std::string> &lines_;
    std::vector<PySyntaxError> *errors_;
    std::vector<PyComment> *comments_;
    std::vector<int> *depth_;
    // Second-pass flag: a file that ended with a bracket still open is
    // re-tokenized with the force-close rule below, so one unfinished
    // `f(` while typing does not swallow the whole rest of the file into
    // a single logical line (and with it every symbol, fold and
    // diagnostic under the cursor). Off for the first pass, so a *valid*
    // file -- where a continuation line may legally sit at any
    // indentation -- is never subject to the heuristic.
    bool recover_ = false;
    std::vector<PyToken> out_;

    int li_ = 0;  // current line
    int ci_ = 0;  // current byte column
    // Open brackets, so a closer can name its opener's position in the
    // "unclosed bracket" report and a mismatch can say what it expected.
    struct OpenBracket {
        char ch = '(';
        PyPos pos;
    };
    std::vector<OpenBracket> brackets_;
    // One entry per open indentation level, holding both tab expansions
    // CPython compares (see Indent()): 8 columns per tab, and 1 per tab.
    struct IndentLevel {
        int w8 = 0;
        int w1 = 0;
    };
    std::vector<IndentLevel> indents_{IndentLevel{}};

    /** @brief The byte under the cursor: '\n' at end of line, '\0' past end of file. */
    char Cur() const {
        if (li_ >= static_cast<int>(lines_.size())) return '\0';
        const std::string &line = lines_[static_cast<size_t>(li_)];
        if (ci_ >= static_cast<int>(line.size())) return '\n';
        return line[static_cast<size_t>(ci_)];
    }

    /** @brief The byte `n` positions ahead on the current line, or '\0' past its end. */
    char Peek(int n) const {
        if (li_ >= static_cast<int>(lines_.size())) return '\0';
        const std::string &line = lines_[static_cast<size_t>(li_)];
        const int at = ci_ + n;
        if (at < 0 || at >= static_cast<int>(line.size())) return '\0';
        return line[static_cast<size_t>(at)];
    }

    /** @brief Advances one byte, stepping to the next line when the newline itself is consumed. */
    void Adv() {
        if (li_ >= static_cast<int>(lines_.size())) return;
        if (ci_ >= static_cast<int>(lines_[static_cast<size_t>(li_)].size())) {
            li_++;
            ci_ = 0;
        } else {
            ci_++;
        }
    }

    /** @brief Reports whether the cursor is past the last line. */
    bool AtEof() const { return li_ >= static_cast<int>(lines_.size()); }

    /** @brief The current position. */
    PyPos Here() const { return PyPos{li_, ci_}; }

    /** @brief Records a lexical error. */
    void Error(PyPos start, PyPos end, const std::string &code, const std::string &message) {
        if (errors_ != nullptr) errors_->push_back(PySyntaxError{start, end, code, message});
    }

    /** @brief Appends a token with an explicit span. */
    void Emit(PyTokKind kind, const std::string &text, PyPos start, PyPos end, const std::string &prefix = "",
              bool unterminated = false) {
        PyToken t;
        t.kind = kind;
        t.text = text;
        t.start = start;
        t.end = end;
        t.str_prefix = prefix;
        t.unterminated = unterminated;
        out_.push_back(std::move(t));
    }

    /** @brief Reports whether a line looks like the start of a new statement rather than a continuation. */
    bool LooksLikeNewStatement(int line) const;

    void HandleLineStart();
    void ScanToken();
    void ScanString(const std::string &prefix, PyPos start);
    bool ScanStringBody(std::string &text, bool raw, bool formatted);
    static std::string LastIdentRun(const std::string &text);
    void ScanNumber();
    void ScanNameOrString();
};

/** @brief Reports whether a prefix of letters is a legal string-literal prefix (`r`, `bR`, `fr`, ...). */
bool IsStringPrefix(const std::string &raw) {
    if (raw.empty() || raw.size() > 2) return false;
    std::string lower;
    for (char c : raw) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::set<std::string> kPrefixes = {"r",  "b",  "u",  "f",  "t",  "rb", "br",
                                                    "fr", "rf", "tr", "rt"};
    return kPrefixes.count(lower) > 0;
}

void Lexer::HandleLineStart() {
    // Measure the indentation twice, the way CPython does: once counting
    // a tab as advancing to the next multiple of 8, once counting it as
    // one column. Two lines whose indentation compares differently under
    // the two rules are ambiguous, which is exactly the TabError case --
    // it is not about *containing* both tabs and spaces (plenty of legal
    // code does), it is about two indentation levels whose order depends
    // on how wide a tab is.
    int w8 = 0, w1 = 0;
    while (true) {
        const char c = Cur();
        if (c == ' ') {
            w8++;
            w1++;
        } else if (c == '\t') {
            w8 = (w8 / 8 + 1) * 8;
            w1++;
        } else if (c == '\f') {
            w8 = 0;
            w1 = 0;
        } else {
            break;
        }
        Adv();
    }
    const char c = Cur();
    // A blank line, or one holding nothing but a comment, has no
    // indentation as far as the grammar is concerned: it produces no
    // NEWLINE and never opens or closes a block.
    if (c == '\n' || c == '\0') return;
    if (c == '#') return;

    const IndentLevel top = indents_.back();
    if (w8 > top.w8) {
        if (w1 <= top.w1) {
            Error(PyPos{li_, 0}, Here(), "tab-error", "Inconsistent use of tabs and spaces in indentation");
        }
        indents_.push_back(IndentLevel{w8, w1});
        Emit(PyTokKind::Indent, std::string(), PyPos{li_, 0}, Here());
        return;
    }
    while (indents_.size() > 1 && w8 < indents_.back().w8) {
        indents_.pop_back();
        Emit(PyTokKind::Dedent, std::string(), PyPos{li_, 0}, PyPos{li_, 0});
    }
    if (w8 != indents_.back().w8) {
        Error(PyPos{li_, 0}, Here(), "bad-dedent", "Unindent does not match any outer indentation level");
        // Take the line's own width as the current level anyway, so one
        // misaligned line does not cascade into an error on every line
        // under it.
        indents_.back() = IndentLevel{w8, w1};
    } else if (w8 == indents_.back().w8 && w1 != indents_.back().w1) {
        Error(PyPos{li_, 0}, Here(), "tab-error", "Inconsistent use of tabs and spaces in indentation");
        indents_.back().w1 = w1;
    }
}

bool Lexer::LooksLikeNewStatement(int line) const {
    if (brackets_.empty()) return false;
    // Skip blank and comment-only lines to whatever comes next.
    int at = line;
    while (at < static_cast<int>(lines_.size())) {
        const std::string &text = lines_[static_cast<size_t>(at)];
        size_t i = 0;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) i++;
        if (i < text.size() && text[i] != '#') break;
        at++;
    }
    if (at >= static_cast<int>(lines_.size())) return false;
    const std::string &text = lines_[static_cast<size_t>(at)];
    size_t i = 0;
    int indent = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
        indent = text[i] == '\t' ? (indent / 8 + 1) * 8 : indent + 1;
        i++;
    }
    // The opener's own line: a continuation is normally indented past it.
    const std::string &opener_line = lines_[static_cast<size_t>(brackets_.front().pos.line)];
    int opener_indent = 0;
    for (size_t k = 0; k < opener_line.size() && (opener_line[k] == ' ' || opener_line[k] == '\t'); k++) {
        opener_indent = opener_line[k] == '\t' ? (opener_indent / 8 + 1) * 8 : opener_indent + 1;
    }
    if (indent > opener_indent) return false;
    if (i < text.size() && text[i] == '@') return true;
    size_t end = i;
    while (end < text.size() && IsPythonIdentChar(static_cast<unsigned char>(text[end]))) end++;
    const std::string word = text.substr(i, end - i);
    // Only the keywords that can *begin* a statement and can never begin
    // (or continue) an expression -- `not`, `lambda`, `None`, `await`
    // and friends are deliberately absent.
    static const std::set<std::string> kStatementStarts = {
        "assert", "async", "break",  "class",  "continue", "def",    "del",    "elif",   "else",
        "except", "finally", "for", "from",   "global",   "if",     "import", "nonlocal",
        "pass",   "raise", "return", "try",    "while",    "with",
    };
    return kStatementStarts.count(word) > 0;
}

void Lexer::ScanString(const std::string &prefix, PyPos start) {
    std::string lower;
    for (char c : prefix) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string text = prefix;
    const bool triple = Peek(1) == Cur() && Peek(2) == Cur();
    const bool closed = ScanStringBody(text, lower.find('r') != std::string::npos,
                                       lower.find('f') != std::string::npos || lower.find('t') != std::string::npos);
    if (!closed) {
        Error(start, Here(), "unterminated-string",
              triple ? "Unterminated triple-quoted string literal" : "Unterminated string literal");
    }
    Emit(PyTokKind::String, text, start, Here(), lower, !closed);
}

bool Lexer::ScanStringBody(std::string &text, bool raw, bool formatted) {
    const char quote = Cur();
    const bool triple = Peek(1) == quote && Peek(2) == quote;
    const int quote_len = triple ? 3 : 1;
    for (int k = 0; k < quote_len; k++) {
        text += quote;
        Adv();
    }
    // Brace depth inside an f-string (PEP 701): within a replacement
    // field a quote of the *same* kind opens a nested literal rather than
    // closing this one, which is why this scanner cannot simply look for
    // the next matching quote. `{{`/`}}` at depth 0 are escaped braces
    // and change nothing.
    int braces = 0;
    while (!AtEof()) {
        const char c = Cur();
        if (c == '\0') break;
        if (c == '\n' && !triple) break;  // a single-quoted string may not cross a line
        if (c == '\\') {
            // A backslash escapes the next byte, newline included (that
            // is how a single-quoted string legally continues). In a raw
            // string it stays literal, but it still hides the quote
            // behind it from the terminator scan.
            text += c;
            Adv();
            if (Cur() == '\n' && !raw) {
                text += '\n';
                Adv();
                continue;
            }
            if (AtEof() || Cur() == '\0') break;
            text += Cur();
            Adv();
            continue;
        }
        if (formatted && (c == '{' || c == '}')) {
            if (braces == 0 && Peek(1) == c) {  // `{{` / `}}`
                text += c;
                text += c;
                Adv();
                Adv();
                continue;
            }
            braces += c == '{' ? 1 : (braces > 0 ? -1 : 0);
            text += c;
            Adv();
            continue;
        }
        if (formatted && braces > 0 && (c == '"' || c == '\'')) {
            // A nested literal inside a replacement field. Recurse so
            // that it, too, may be an f-string with its own nesting.
            const std::string tail = LastIdentRun(text);
            ScanStringBody(text, tail.find('r') != std::string::npos,
                           tail.find('f') != std::string::npos || tail.find('t') != std::string::npos);
            continue;
        }
        if (c == quote && braces == 0) {
            if (!triple) {
                text += c;
                Adv();
                return true;
            }
            if (Peek(1) == quote && Peek(2) == quote) {
                for (int k = 0; k < 3; k++) {
                    text += quote;
                    Adv();
                }
                return true;
            }
        }
        text += c;
        Adv();
    }
    return false;
}

/** @brief The identifier characters immediately before the end of `text`, lowercased (a nested literal's prefix). */
std::string Lexer::LastIdentRun(const std::string &text) {
    size_t i = text.size();
    while (i > 0 && IsPythonIdentChar(static_cast<unsigned char>(text[i - 1]))) i--;
    std::string out = text.substr(i);
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void Lexer::ScanNumber() {
    const PyPos start = Here();
    std::string text;
    bool valid = true;
    if (Cur() == '0' && (Peek(1) == 'x' || Peek(1) == 'X' || Peek(1) == 'o' || Peek(1) == 'O' || Peek(1) == 'b' ||
                         Peek(1) == 'B')) {
        const char base = static_cast<char>(std::tolower(static_cast<unsigned char>(Peek(1))));
        text += Cur();
        Adv();
        text += Cur();
        Adv();
        int digits = 0;
        while (true) {
            const unsigned char c = static_cast<unsigned char>(Cur());
            const bool ok = c == '_' || (base == 'x' && IsHexDigit(c)) || (base == 'o' && c >= '0' && c <= '7') ||
                            (base == 'b' && (c == '0' || c == '1'));
            if (!ok) break;
            if (c != '_') digits++;
            text += static_cast<char>(c);
            Adv();
        }
        if (digits == 0) valid = false;
        // A trailing letter/digit means the literal ran into an
        // identifier: `0x1g`, `0b12`. `l`/`L` gets its own message below.
        if (IsPythonIdentChar(static_cast<unsigned char>(Cur())) || IsDigit(static_cast<unsigned char>(Cur()))) {
            valid = false;
        }
    } else {
        bool seen_dot = false, seen_exp = false;
        while (true) {
            const unsigned char c = static_cast<unsigned char>(Cur());
            if (IsDigit(c) || c == '_') {
                text += static_cast<char>(c);
                Adv();
                continue;
            }
            if (c == '.' && !seen_dot && !seen_exp) {
                seen_dot = true;
                text += static_cast<char>(c);
                Adv();
                continue;
            }
            if ((c == 'e' || c == 'E') && !seen_exp && !text.empty() &&
                (IsDigit(static_cast<unsigned char>(Peek(1))) || ((Peek(1) == '+' || Peek(1) == '-') &&
                                                                  IsDigit(static_cast<unsigned char>(Peek(2)))))) {
                seen_exp = true;
                text += static_cast<char>(c);
                Adv();
                text += Cur();
                Adv();
                continue;
            }
            break;
        }
        if (Cur() == 'j' || Cur() == 'J') {
            text += Cur();
            Adv();
        } else if (Cur() == 'l' || Cur() == 'L') {
            // Python 2's long suffix: worth naming, since the file it
            // comes from is usually Python 2 throughout.
            text += Cur();
            Adv();
            Error(start, Here(), "python2-long", "Python 2 long literal suffix; Python 3 integers are unbounded");
            Emit(PyTokKind::Number, text, start, Here());
            return;
        }
        if (IsPythonIdentChar(static_cast<unsigned char>(Cur()))) valid = false;
    }
    if (!valid) {
        // Swallow the rest of the run so the parser sees one bad token
        // rather than a number followed by a stray name.
        while (IsPythonIdentChar(static_cast<unsigned char>(Cur()))) {
            text += Cur();
            Adv();
        }
        Error(start, Here(), "invalid-number", "Invalid numeric literal '" + text + "'");
    }
    Emit(PyTokKind::Number, text, start, Here());
}

void Lexer::ScanNameOrString() {
    const PyPos start = Here();
    std::string text;
    while (IsPythonIdentChar(static_cast<unsigned char>(Cur()))) {
        text += Cur();
        Adv();
    }
    // `f"..."`, `rb'...'`: an identifier immediately followed by a quote
    // is a string prefix, not a name.
    if ((Cur() == '"' || Cur() == '\'') && IsStringPrefix(text)) {
        ScanString(text, start);
        return;
    }
    Emit(KeywordSet().count(text) > 0 ? PyTokKind::Keyword : PyTokKind::Name, text, start, Here());
}

void Lexer::ScanToken() {
    const char c = Cur();
    if (c == '"' || c == '\'') {
        ScanString(std::string(), Here());
        return;
    }
    if (IsDigit(static_cast<unsigned char>(c)) ||
        (c == '.' && IsDigit(static_cast<unsigned char>(Peek(1))))) {
        ScanNumber();
        return;
    }
    if (IsPythonIdentStart(static_cast<unsigned char>(c))) {
        ScanNameOrString();
        return;
    }
    const PyPos start = Here();
    const std::string &line = lines_[static_cast<size_t>(li_)];
    for (const char *op : kOperators) {
        const size_t n = std::char_traits<char>::length(op);
        if (line.compare(static_cast<size_t>(ci_), n, op) == 0) {
            for (size_t k = 0; k < n; k++) Adv();
            if (std::string(op) == "<>") {
                Error(start, Here(), "python2-ne", "Python 2 inequality operator; use '!='");
                Emit(PyTokKind::Op, "!=", start, Here());
                return;
            }
            Emit(PyTokKind::Op, op, start, Here());
            return;
        }
    }
    if (std::char_traits<char>::find(kSingleOps, std::char_traits<char>::length(kSingleOps), c) != nullptr) {
        if (c == '(' || c == '[' || c == '{') brackets_.push_back(OpenBracket{c, start});
        if (c == ')' || c == ']' || c == '}') {
            const char want = c == ')' ? '(' : (c == ']' ? '[' : '{');
            if (brackets_.empty()) {
                Error(start, PyPos{li_, ci_ + 1}, "unmatched-bracket", std::string("Unmatched '") + c + "'");
            } else if (brackets_.back().ch != want) {
                Error(start, PyPos{li_, ci_ + 1}, "mismatched-bracket",
                      std::string("Closing '") + c + "' does not match opening '" + brackets_.back().ch + "'");
                brackets_.pop_back();
            } else {
                brackets_.pop_back();
            }
        }
        Adv();
        Emit(PyTokKind::Op, std::string(1, c), start, Here());
        return;
    }
    // Anything left is not a Python token at all: `$`, `?`, a stray
    // backtick (Python 2's repr quotes), a lone backslash mid-line.
    Adv();
    Error(start, Here(), "invalid-character", std::string("Invalid character '") + c + "' in source");
    Emit(PyTokKind::Op, std::string(1, c), start, Here());
}

std::vector<PyToken> Lexer::Run() {
    bool line_start = true;
    if (depth_ != nullptr) depth_->assign(lines_.size(), 0);
    while (!AtEof()) {
        if (depth_ != nullptr && ci_ == 0 && li_ < static_cast<int>(lines_.size())) {
            (*depth_)[static_cast<size_t>(li_)] = static_cast<int>(brackets_.size());
        }
        if (line_start) {
            HandleLineStart();
            line_start = false;
            if (AtEof()) break;
        }
        const char c = Cur();
        if (c == ' ' || c == '\t' || c == '\f') {
            Adv();
            continue;
        }
        if (c == '#') {
            const int start_col = ci_;
            const std::string &line = lines_[static_cast<size_t>(li_)];
            if (comments_ != nullptr) {
                PyComment cm;
                cm.line = li_;
                cm.col = start_col;
                cm.text = line.substr(static_cast<size_t>(start_col));
                cm.own_line = true;
                for (int k = 0; k < start_col; k++) {
                    const char p = line[static_cast<size_t>(k)];
                    if (p != ' ' && p != '\t') {
                        cm.own_line = false;
                        break;
                    }
                }
                comments_->push_back(cm);
            }
            ci_ = static_cast<int>(line.size());
            continue;
        }
        if (c == '\n') {
            const PyPos at = Here();
            Adv();
            if (recover_ && !brackets_.empty() && LooksLikeNewStatement(li_)) {
                // The next line starts a statement at an indentation the
                // open bracket cannot plausibly contain: treat the
                // bracket as abandoned, report it where it was opened,
                // and end the logical line here.
                for (const OpenBracket &open : brackets_) {
                    Error(open.pos, PyPos{open.pos.line, open.pos.col + 1}, "unclosed-bracket",
                          std::string("'") + open.ch + "' was never closed");
                }
                brackets_.clear();
            }
            if (brackets_.empty()) {
                // A logical line ends here unless it is empty -- a blank
                // line inside a suite must not close it.
                if (!out_.empty() && out_.back().kind != PyTokKind::Newline && out_.back().kind != PyTokKind::Indent &&
                    out_.back().kind != PyTokKind::Dedent) {
                    Emit(PyTokKind::Newline, std::string(), at, at);
                }
                line_start = true;
            } else if (depth_ != nullptr && li_ < static_cast<int>(lines_.size())) {
                (*depth_)[static_cast<size_t>(li_)] = static_cast<int>(brackets_.size());
            }
            continue;
        }
        if (c == '\\' && Peek(1) == '\0') {
            // Explicit line joining: the backslash and the newline both
            // disappear, and the next line is *not* a line start.
            Adv();
            Adv();
            continue;
        }
        ScanToken();
    }
    const PyPos eof{static_cast<int>(lines_.size()) - 1,
                    lines_.empty() ? 0 : static_cast<int>(lines_.back().size())};
    for (const OpenBracket &b : brackets_) {
        Error(b.pos, PyPos{b.pos.line, b.pos.col + 1}, "unclosed-bracket",
              std::string("'") + b.ch + "' was never closed");
    }
    if (!out_.empty() && out_.back().kind != PyTokKind::Newline) Emit(PyTokKind::Newline, std::string(), eof, eof);
    while (indents_.size() > 1) {
        indents_.pop_back();
        Emit(PyTokKind::Dedent, std::string(), eof, eof);
    }
    Emit(PyTokKind::End, std::string(), eof, eof);
    return std::move(out_);
}

}  // namespace

bool IsPythonKeyword(const std::string &name) { return KeywordSet().count(name) > 0; }

bool IsPythonIdentStart(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

bool IsPythonIdentChar(unsigned char c) { return IsPythonIdentStart(c) || IsDigit(c); }

std::vector<PyToken> TokenizePython(const std::vector<std::string> &lines, std::vector<PySyntaxError> *out_errors,
                                    std::vector<PyComment> *out_comments, std::vector<int> *out_depth) {
    static const std::vector<std::string> kEmptyDoc{std::string()};
    const std::vector<std::string> &src = lines.empty() ? kEmptyDoc : lines;
    Lexer lexer(src, out_errors, out_comments, out_depth);
    std::vector<PyToken> tokens = lexer.Run();
    bool unclosed = false;
    if (out_errors != nullptr) {
        for (const PySyntaxError &e : *out_errors) unclosed = unclosed || e.code == "unclosed-bracket";
    }
    if (!unclosed) return tokens;
    // Re-tokenize in recovery mode (see Lexer::recover_): the report of
    // the unclosed bracket is kept, but the rest of the file gets its
    // line structure back.
    std::vector<PySyntaxError> recovered_errors;
    if (out_comments != nullptr) out_comments->clear();
    Lexer second(src, &recovered_errors, out_comments, out_depth, true);
    std::vector<PyToken> recovered = second.Run();
    *out_errors = std::move(recovered_errors);
    return recovered;
}

// --- Parser -----------------------------------------------------------

namespace {

/** @brief Allocates a node of a kind, with its start position (end is filled in by the caller). */
PyNodePtr MakeNode(PyNodeKind kind, PyPos start) {
    PyNodePtr n(new PyNode());
    n->kind = kind;
    n->start = start;
    n->end = start;
    return n;
}

/** @brief Shifts a subtree parsed from a detached snippet back onto the real document's coordinates. */
void ShiftNode(PyNode *n, int base_line, int base_col) {
    if (n == nullptr) return;
    if (n->start.line == 0) n->start.col += base_col;
    if (n->end.line == 0) n->end.col += base_col;
    if (n->name_pos.line == 0) n->name_pos.col += base_col;
    n->start.line += base_line;
    n->end.line += base_line;
    n->name_pos.line += base_line;
    for (std::vector<PyNodePtr> *vec :
         {&n->kids, &n->targets, &n->body, &n->orelse, &n->finalbody, &n->handlers, &n->decorators, &n->params}) {
        for (PyNodePtr &child : *vec) ShiftNode(child.get(), base_line, base_col);
    }
}

// Binary operator precedence, loosest first. Each row is one level; the
// parser climbs them with a plain loop rather than one function per
// level, so adding a level is a table edit.
const std::vector<std::vector<std::string>> &BinaryLevels() {
    static const std::vector<std::vector<std::string>> kLevels = {
        {"|"}, {"^"}, {"&"}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "//", "%", "@"},
    };
    return kLevels;
}

// A parser over one token stream. Never throws: every failure is a
// recorded PySyntaxError plus a Placeholder node, which is what lets a
// half-typed file still produce a usable tree.
class Parser {
public:
    Parser(const std::vector<PyToken> &tokens, std::vector<PySyntaxError> *errors)
        : toks_(tokens), errors_(errors) {}

    /** @brief Parses the whole token stream as a module. */
    PyNodePtr ParseModule() {
        PyNodePtr mod = MakeNode(PyNodeKind::Module, PyPos{0, 0});
        SkipNewlines();
        while (!AtEnd()) {
            if (Cur().kind == PyTokKind::Dedent || Cur().kind == PyTokKind::Indent) {
                // Stray indentation at module level: the tokenizer has
                // already reported it, so just resynchronize.
                if (Cur().kind == PyTokKind::Indent) {
                    Error(Cur(), "unexpected-indent", "Unexpected indentation");
                    SkipBlock();
                } else {
                    Advance();
                }
                continue;
            }
            ParseStatementInto(mod->body);
            SkipNewlines();
        }
        if (!mod->body.empty()) mod->end = mod->body.back()->end;
        return mod;
    }

    /** @brief Parses the token stream as a single expression (used for f-string interpolations). */
    PyNodePtr ParseSingleExpression() {
        // `f"{ x }"`: the snippet's own leading space reads as an INDENT,
        // which is meaningless for a detached expression.
        while (Cur().kind == PyTokKind::Indent) Advance();
        if (AtEnd() || Cur().kind == PyTokKind::Newline) return MakeNode(PyNodeKind::Placeholder, Here());
        PyNodePtr e = ParseTestListStarExpr();
        return e;
    }

private:
    const std::vector<PyToken> &toks_;
    std::vector<PySyntaxError> *errors_;
    size_t i_ = 0;
    int depth_ = 0;          // expression recursion guard
    bool reported_deep_ = false;

    static const int kMaxDepth = 150;

    /** @brief The current token. */
    const PyToken &Cur() const { return toks_[i_ < toks_.size() ? i_ : toks_.size() - 1]; }

    /** @brief The token `n` positions ahead (clamped to the End token). */
    const PyToken &Ahead(size_t n) const {
        const size_t at = i_ + n;
        return toks_[at < toks_.size() ? at : toks_.size() - 1];
    }

    /** @brief The current token's start position. */
    PyPos Here() const { return Cur().start; }

    /** @brief The position just past the previously consumed token (a node's natural end). */
    PyPos PrevEnd() const { return i_ > 0 ? toks_[i_ - 1].end : PyPos{0, 0}; }

    /** @brief Reports whether the stream is exhausted. */
    bool AtEnd() const { return Cur().kind == PyTokKind::End; }

    /** @brief Consumes one token. */
    void Advance() {
        if (i_ + 1 < toks_.size()) i_++;
    }

    bool AtOp(const char *op) const { return Cur().kind == PyTokKind::Op && Cur().text == op; }
    bool AtKw(const char *kw) const { return Cur().kind == PyTokKind::Keyword && Cur().text == kw; }
    bool AtName(const char *name) const { return Cur().kind == PyTokKind::Name && Cur().text == name; }

    /** @brief Consumes the current token if it is this operator. */
    bool AcceptOp(const char *op) {
        if (!AtOp(op)) return false;
        Advance();
        return true;
    }

    /** @brief Consumes the current token if it is this keyword. */
    bool AcceptKw(const char *kw) {
        if (!AtKw(kw)) return false;
        Advance();
        return true;
    }

    /** @brief Records a syntax error against a token. */
    void Error(const PyToken &tok, const std::string &code, const std::string &message) {
        if (errors_ == nullptr) return;
        // One error per line. Recovery can re-enter the same spot, and a
        // single mistake cascades into several complaints about the
        // tokens after it -- CPython itself reports only the first.
        for (const PySyntaxError &e : *errors_) {
            if (e.start.line == tok.start.line) return;
        }
        PyPos end = tok.end;
        if (end.line == tok.start.line && end.col <= tok.start.col) end.col = tok.start.col + 1;
        errors_->push_back(PySyntaxError{tok.start, end, code, message});
    }

    /** @brief Records a syntax error against an explicit span. */
    void ErrorAt(PyPos start, PyPos end, const std::string &code, const std::string &message) {
        if (errors_ == nullptr) return;
        for (const PySyntaxError &e : *errors_) {
            if (e.start.line == start.line) return;
        }
        if (end.line == start.line && end.col <= start.col) end.col = start.col + 1;
        errors_->push_back(PySyntaxError{start, end, code, message});
    }

    /** @brief Names the current token the way an error message should ("end of line", "'else'", ...). */
    std::string Describe() const {
        switch (Cur().kind) {
            case PyTokKind::End: return "end of file";
            case PyTokKind::Newline: return "end of line";
            case PyTokKind::Indent: return "an indented block";
            case PyTokKind::Dedent: return "the end of this block";
            default: return "'" + Cur().text + "'";
        }
    }

    /** @brief Consumes an expected operator, reporting (but not consuming) anything else. */
    bool ExpectOp(const char *op) {
        if (AcceptOp(op)) return true;
        Error(Cur(), "expected-token", std::string("Expected '") + op + "', found " + Describe());
        return false;
    }

    /** @brief Consumes an expected keyword, reporting (but not consuming) anything else. */
    bool ExpectKw(const char *kw) {
        if (AcceptKw(kw)) return true;
        Error(Cur(), "expected-token", std::string("Expected '") + kw + "', found " + Describe());
        return false;
    }

    /** @brief Consumes and returns an expected identifier, or "" (reporting) if there is none. */
    std::string ExpectName() {
        if (Cur().kind == PyTokKind::Name) {
            const std::string name = Cur().text;
            Advance();
            return name;
        }
        Error(Cur(), "expected-name", "Expected a name, found " + Describe());
        return std::string();
    }

    /** @brief Consumes the end of a logical line, reporting anything left over on it. */
    void ExpectNewline() {
        if (Cur().kind == PyTokKind::Newline) {
            Advance();
            return;
        }
        if (Cur().kind == PyTokKind::End || Cur().kind == PyTokKind::Dedent) return;
        Error(Cur(), "unexpected-token", "Unexpected " + Describe() + " after end of statement");
        RecoverLine();
    }

    /** @brief Skips blank logical lines. */
    void SkipNewlines() {
        while (Cur().kind == PyTokKind::Newline) Advance();
    }

    /** @brief Skips to just past the next end of logical line. */
    void RecoverLine() {
        while (!AtEnd() && Cur().kind != PyTokKind::Newline) {
            if (Cur().kind == PyTokKind::Indent || Cur().kind == PyTokKind::Dedent) return;
            Advance();
        }
        if (Cur().kind == PyTokKind::Newline) Advance();
    }

    /** @brief Skips a whole INDENT..DEDENT block, brackets of indentation balanced. */
    void SkipBlock() {
        if (Cur().kind != PyTokKind::Indent) return;
        int level = 0;
        do {
            if (Cur().kind == PyTokKind::Indent) level++;
            if (Cur().kind == PyTokKind::Dedent) level--;
            Advance();
        } while (!AtEnd() && level > 0);
    }

    /** @brief Recovers from a failed statement: drop the rest of its line, and its block if it opened one. */
    void RecoverStatement() {
        RecoverLine();
        SkipNewlines();
        if (Cur().kind == PyTokKind::Indent) SkipBlock();
    }

    // --- Suites -------------------------------------------------------

    /** @brief Sets a compound statement's end to the last line of its own suites. */
    static void FinishCompound(PyNode *n) {
        // Deliberately ignores the node's current end, which the caller
        // set from the last consumed token -- and the last token of a
        // block is its DEDENT, which the tokenizer emits at the start of
        // the *next* statement's line. A `class` whose range reached onto
        // the following line would fold and outline one line too far.
        bool any = false;
        PyPos end = n->start;
        for (const std::vector<PyNodePtr> *vec : {&n->body, &n->orelse, &n->finalbody, &n->handlers}) {
            if (vec->empty()) continue;
            const PyPos &last = vec->back()->end;
            if (!any || last.line > end.line || (last.line == end.line && last.col > end.col)) end = last;
            any = true;
        }
        if (any) n->end = end;
    }

    /** @brief Parses `: <suite>`, either an indented block or the statements after the colon on one line. */
    void ParseSuite(std::vector<PyNodePtr> &body) {
        if (!ExpectOp(":")) {
            RecoverStatement();
            return;
        }
        if (Cur().kind == PyTokKind::Newline) {
            Advance();
            SkipNewlines();
            if (Cur().kind != PyTokKind::Indent) {
                Error(Cur(), "expected-block", "Expected an indented block");
                return;
            }
            Advance();
            while (!AtEnd() && Cur().kind != PyTokKind::Dedent) {
                if (Cur().kind == PyTokKind::Newline) {
                    Advance();
                    continue;
                }
                if (Cur().kind == PyTokKind::Indent) {
                    Error(Cur(), "unexpected-indent", "Unexpected indentation");
                    SkipBlock();
                    continue;
                }
                const size_t before = i_;
                ParseStatementInto(body);
                if (i_ == before) Advance();  // never spin on a token nothing consumes
            }
            if (Cur().kind == PyTokKind::Dedent) Advance();
            return;
        }
        // `if x: return 1` -- simple statements, on this line only.
        ParseSimpleLineInto(body);
    }

    // --- Statements ---------------------------------------------------

    /** @brief Parses one statement (or one `;`-joined line of them) and appends it to `out`. */
    void ParseStatementInto(std::vector<PyNodePtr> &out) {
        const size_t before = i_;
        if (AtOp("@")) {
            ParseDecorated(out);
        } else if (AtKw("if")) {
            out.push_back(ParseIf());
        } else if (AtKw("while")) {
            out.push_back(ParseWhile());
        } else if (AtKw("for")) {
            out.push_back(ParseFor(false));
        } else if (AtKw("try")) {
            out.push_back(ParseTry());
        } else if (AtKw("with")) {
            out.push_back(ParseWith(false));
        } else if (AtKw("def")) {
            out.push_back(ParseFunctionDef(false));
        } else if (AtKw("class")) {
            out.push_back(ParseClassDef());
        } else if (AtKw("async")) {
            const PyPos start = Here();
            Advance();
            if (AtKw("def")) {
                PyNodePtr fn = ParseFunctionDef(true);
                fn->start = start;
                out.push_back(std::move(fn));
            } else if (AtKw("for")) {
                PyNodePtr fr = ParseFor(true);
                fr->start = start;
                out.push_back(std::move(fr));
            } else if (AtKw("with")) {
                PyNodePtr wi = ParseWith(true);
                wi->start = start;
                out.push_back(std::move(wi));
            } else {
                Error(Cur(), "bad-async", "'async' must be followed by 'def', 'for' or 'with'");
                RecoverStatement();
            }
        } else if (IsMatchStatement()) {
            out.push_back(ParseMatch());
        } else if (IsTypeAliasStatement()) {
            out.push_back(ParseTypeAlias());
        } else {
            ParseSimpleLineInto(out);
        }
        if (i_ == before) Advance();
    }

    /** @brief Parses a `@decorator` run followed by the `def`/`class` it decorates. */
    void ParseDecorated(std::vector<PyNodePtr> &out) {
        std::vector<PyNodePtr> decorators;
        while (AtOp("@")) {
            const PyPos start = Here();
            Advance();
            PyNodePtr expr = ParseNamedTest();
            expr->start = start;
            decorators.push_back(std::move(expr));
            ExpectNewline();
            SkipNewlines();
        }
        bool is_async = false;
        if (AtKw("async") && Ahead(1).kind == PyTokKind::Keyword && Ahead(1).text == "def") {
            is_async = true;
            Advance();
        }
        PyNodePtr target;
        if (AtKw("def")) {
            target = ParseFunctionDef(is_async);
        } else if (AtKw("class")) {
            target = ParseClassDef();
        } else {
            Error(Cur(), "bad-decorator", "Expected a 'def' or 'class' after a decorator");
            RecoverStatement();
            return;
        }
        if (!decorators.empty()) target->start = decorators.front()->start;
        target->decorators = std::move(decorators);
        out.push_back(std::move(target));
    }

    /** @brief Parses `if`/`elif`/`else`, each `elif` becoming a nested If in the previous `orelse`. */
    PyNodePtr ParseIf() {
        const PyPos start = Here();
        Advance();  // 'if' / 'elif'
        PyNodePtr node = MakeNode(PyNodeKind::If, start);
        node->kids.push_back(ParseNamedTest());
        ParseSuite(node->body);
        SkipNewlines();
        if (AtKw("elif")) {
            node->orelse.push_back(ParseIf());
        } else if (AtKw("else")) {
            Advance();
            ParseSuite(node->orelse);
        }
        node->end = PrevEnd();
        FinishCompound(node.get());
        return node;
    }

    /** @brief Parses `while ...:` with its optional `else`. */
    PyNodePtr ParseWhile() {
        const PyPos start = Here();
        Advance();
        PyNodePtr node = MakeNode(PyNodeKind::While, start);
        node->kids.push_back(ParseNamedTest());
        ParseSuite(node->body);
        SkipNewlines();
        if (AtKw("else")) {
            Advance();
            ParseSuite(node->orelse);
        }
        node->end = PrevEnd();
        FinishCompound(node.get());
        return node;
    }

    /** @brief Parses `for targets in iterable:` with its optional `else`. */
    PyNodePtr ParseFor(bool is_async) {
        const PyPos start = Here();
        Advance();
        PyNodePtr node = MakeNode(PyNodeKind::For, start);
        node->is_async = is_async;
        PyNodePtr targets = ParseTargetList("in");
        node->targets.push_back(std::move(targets));
        ExpectKw("in");
        node->kids.push_back(ParseTestListStarExpr());
        ParseSuite(node->body);
        SkipNewlines();
        if (AtKw("else")) {
            Advance();
            ParseSuite(node->orelse);
        }
        node->end = PrevEnd();
        FinishCompound(node.get());
        return node;
    }

    /** @brief Parses `try`/`except`/`except*`/`else`/`finally`. */
    PyNodePtr ParseTry() {
        const PyPos start = Here();
        Advance();
        PyNodePtr node = MakeNode(PyNodeKind::Try, start);
        ParseSuite(node->body);
        SkipNewlines();
        bool saw_bare = false;
        while (AtKw("except")) {
            const PyPos hstart = Here();
            Advance();
            PyNodePtr handler = MakeNode(PyNodeKind::ExceptHandler, hstart);
            handler->is_async = AcceptOp("*");  // `except*` (exception groups)
            if (!AtOp(":")) {
                if (saw_bare) {
                    Error(Cur(), "except-after-bare", "A bare 'except:' must be the last except clause");
                }
                handler->kids.push_back(ParseTest());
                if (AtOp(",")) {
                    // Python 3.14 (PEP 758) allows an unparenthesized
                    // list of exception types here, which is spelled
                    // exactly like Python 2's `except ValueError, err:`.
                    // They are told apart by what the last item looks
                    // like: one bare lowercase name is a Python 2
                    // capture, anything else (a second class, a dotted
                    // name, a capitalized one) is a type list.
                    std::vector<PyNodePtr> extra;
                    while (AcceptOp(",")) {
                        if (AtOp(":") || AtLineEnd()) break;
                        extra.push_back(ParseTest());
                    }
                    if (extra.size() == 1 && extra[0]->kind == PyNodeKind::Name && !extra[0]->name.empty() &&
                        std::islower(static_cast<unsigned char>(extra[0]->name[0])) != 0) {
                        ErrorAt(extra[0]->start, extra[0]->end, "python2-except",
                                "Python 2 except syntax; use 'except " + handler->kids[0]->name + " as " +
                                    extra[0]->name + ":'");
                        handler->name = extra[0]->name;
                        handler->str_value = extra[0]->name;
                    } else {
                        for (PyNodePtr &item : extra) handler->kids.push_back(std::move(item));
                    }
                }
                if (AcceptKw("as")) {
                    handler->name = ExpectName();
                    handler->str_value = handler->name;
                }
            } else {
                saw_bare = true;
            }
            ParseSuite(handler->body);
            handler->end = PrevEnd();
            FinishCompound(handler.get());
            node->handlers.push_back(std::move(handler));
            SkipNewlines();
        }
        if (AtKw("else")) {
            Advance();
            ParseSuite(node->orelse);
            SkipNewlines();
        }
        if (AtKw("finally")) {
            Advance();
            ParseSuite(node->finalbody);
        }
        if (node->handlers.empty() && node->finalbody.empty()) {
            Error(Cur(), "try-without-except", "'try' needs an 'except' or a 'finally' clause");
        }
        node->end = PrevEnd();
        FinishCompound(node.get());
        return node;
    }

    /** @brief Parses `with a as b, c:` (parenthesized item lists included). */
    PyNodePtr ParseWith(bool is_async) {
        const PyPos start = Here();
        Advance();
        PyNodePtr node = MakeNode(PyNodeKind::With, start);
        node->is_async = is_async;
        // PEP 617 allows the whole item list to be parenthesized, and
        // that is spelled like a parenthesized expression until an `as`
        // or a trailing comma turns up inside it -- so the shape is
        // decided by a lookahead rather than by backtracking.
        if (AtOp("(") && LooksLikeParenthesizedWithItems()) {
            Advance();
            while (!AtOp(")") && !AtEnd() && Cur().kind != PyTokKind::Newline) {
                node->kids.push_back(ParseWithItem());
                if (!AcceptOp(",")) break;
            }
            ExpectOp(")");
        } else {
            do {
                node->kids.push_back(ParseWithItem());
            } while (AcceptOp(","));
        }
        ParseSuite(node->body);
        node->end = PrevEnd();
        FinishCompound(node.get());
        return node;
    }

    /** @brief Reports whether the `(` at the cursor opens a with-item list rather than one expression. */
    bool LooksLikeParenthesizedWithItems() const {
        int level = 0;
        for (size_t k = 0; i_ + k < toks_.size(); k++) {
            const PyToken &t = toks_[i_ + k];
            if (t.kind == PyTokKind::Newline || t.kind == PyTokKind::End) return false;
            if (t.kind == PyTokKind::Keyword && t.text == "as" && level == 1) return true;
            if (t.kind != PyTokKind::Op) continue;
            if (t.text == "(" || t.text == "[" || t.text == "{") {
                level++;
            } else if (t.text == ")" || t.text == "]" || t.text == "}") {
                level--;
                if (level == 0) {
                    // `with (a, b,):` -- a trailing comma is only legal
                    // in an item list, never in a tuple this position
                    // would accept.
                    const PyToken &prev = toks_[i_ + k - 1];
                    return prev.kind == PyTokKind::Op && prev.text == ",";
                }
            }
        }
        return false;
    }

    /** @brief Parses one `expr [as target]` context-manager item. */
    PyNodePtr ParseWithItem() {
        const PyPos start = Here();
        PyNodePtr item = MakeNode(PyNodeKind::WithItem, start);
        item->kids.push_back(ParseTest());
        if (AcceptKw("as")) item->targets.push_back(ParseTarget());
        item->end = PrevEnd();
        return item;
    }

    /** @brief Parses `def name(params) -> ann:`. */
    PyNodePtr ParseFunctionDef(bool is_async) {
        const PyPos start = Here();
        Advance();  // 'def'
        PyNodePtr node = MakeNode(PyNodeKind::FunctionDef, start);
        node->is_async = is_async;
        node->name_pos = Here();
        node->name = ExpectName();
        node->end = PrevEnd();
        FinishCompound(node.get());
        SkipTypeParams();
        if (AtOp("(")) {
            ParseParams(node->params);
        } else {
            Error(Cur(), "expected-token", "Expected '(' after the function name");
        }
        if (AcceptOp("->")) node->kids.push_back(ParseTest());
        ParseSuite(node->body);
        node->end = PrevEnd();
        FinishCompound(node.get());
        return node;
    }

    /** @brief Parses `class Name(bases):`. */
    PyNodePtr ParseClassDef() {
        const PyPos start = Here();
        Advance();
        PyNodePtr node = MakeNode(PyNodeKind::ClassDef, start);
        node->name_pos = Here();
        node->name = ExpectName();
        node->end = PrevEnd();
        FinishCompound(node.get());
        SkipTypeParams();
        if (AtOp("(")) {
            PyNodePtr call = ParseCallArguments(MakeNode(PyNodeKind::Placeholder, Here()));
            // kids[0] is the placeholder callee; the rest are the bases
            // and keyword arguments (`metaclass=`).
            for (size_t k = 1; k < call->kids.size(); k++) node->kids.push_back(std::move(call->kids[k]));
        }
        ParseSuite(node->body);
        node->end = PrevEnd();
        FinishCompound(node.get());
        return node;
    }

    /** @brief Skips a PEP 695 bracketed type-parameter list, which binds names this server does not track. */
    void SkipTypeParams() {
        if (!AtOp("[")) return;
        int level = 0;
        do {
            if (AtOp("[")) level++;
            if (AtOp("]")) level--;
            Advance();
        } while (!AtEnd() && level > 0);
    }

    /** @brief Reports whether the current token starts a `match` statement rather than using `match` as a name. */
    bool IsMatchStatement() const {
        if (!AtName("match")) return false;
        // The subject expression can be anything, so the test is
        // structural: scan this logical line for a ':' at bracket depth 0
        // ending it. `match = 1` and `match(x)` both fail that test.
        if (Ahead(1).kind == PyTokKind::Op &&
            (Ahead(1).text == "=" || Ahead(1).text == "." || Ahead(1).text == "," || Ahead(1).text == ")" ||
             Ahead(1).text == "]" || Ahead(1).text == ":")) {
            return false;
        }
        int level = 0;
        for (size_t k = 1; i_ + k < toks_.size(); k++) {
            const PyToken &t = toks_[i_ + k];
            if (t.kind == PyTokKind::Newline || t.kind == PyTokKind::End) return false;
            if (t.kind != PyTokKind::Op) continue;
            if (t.text == "(" || t.text == "[" || t.text == "{") level++;
            if (t.text == ")" || t.text == "]" || t.text == "}") level--;
            if (t.text == "=" && level == 0) return false;
            if (t.text == ":" && level == 0) {
                return toks_[i_ + k + 1].kind == PyTokKind::Newline;
            }
        }
        return false;
    }

    /** @brief Parses `match subject:` and its `case` clauses. */
    PyNodePtr ParseMatch() {
        const PyPos start = Here();
        Advance();  // 'match'
        PyNodePtr node = MakeNode(PyNodeKind::Match, start);
        node->kids.push_back(ParseTestListStarExpr());
        if (!ExpectOp(":")) {
            RecoverStatement();
            node->end = PrevEnd();
        FinishCompound(node.get());
            return node;
        }
        ExpectNewline();
        SkipNewlines();
        if (Cur().kind != PyTokKind::Indent) {
            Error(Cur(), "expected-block", "Expected an indented block of 'case' clauses");
            node->end = PrevEnd();
        FinishCompound(node.get());
            return node;
        }
        Advance();
        while (!AtEnd() && Cur().kind != PyTokKind::Dedent) {
            if (Cur().kind == PyTokKind::Newline) {
                Advance();
                continue;
            }
            if (!AtName("case")) {
                Error(Cur(), "expected-case", "Expected a 'case' clause");
                RecoverStatement();
                continue;
            }
            const PyPos cstart = Here();
            Advance();
            PyNodePtr case_node = MakeNode(PyNodeKind::Case, cstart);
            ParsePattern(case_node.get());
            if (AtKw("if")) {
                Advance();
                case_node->kids.push_back(ParseNamedTest());
            }
            ParseSuite(case_node->body);
            case_node->end = PrevEnd();
            FinishCompound(case_node.get());
        FinishCompound(node.get());
            node->body.push_back(std::move(case_node));
            SkipNewlines();
        }
        if (Cur().kind == PyTokKind::Dedent) Advance();
        node->end = PrevEnd();
        FinishCompound(node.get());
        return node;
    }

    /** @brief Parses a `case` pattern, collecting its capture names as targets and its other names as loads. */
    void ParsePattern(PyNode *case_node) {
        // Patterns are close enough to expressions to reuse the
        // expression parser for their shape; what differs is meaning, so
        // the *names* are classified here: a bare name (not dotted, not
        // called) captures, everything else is a value being matched.
        while (true) {
            // ParseOrTest, not ParseTest: a pattern is never a
            // conditional expression, and `case x if guard:` would
            // otherwise parse the guard as the `if` of a ternary and then
            // demand an `else`.
            PyNodePtr pat = ParseOrTest();
            if (AcceptKw("as")) {
                PyNodePtr cap = MakeNode(PyNodeKind::Name, Here());
                cap->name = ExpectName();
                cap->end = PrevEnd();
                case_node->targets.push_back(std::move(cap));
            }
            CollectPatternCaptures(pat.get(), case_node);
            case_node->kids.insert(case_node->kids.begin(), std::move(pat));
            if (!AcceptOp(",")) break;
            if (AtOp(":")) break;
        }
    }

    /** @brief Walks a parsed pattern, moving its bare names (the captures) into the case's target list. */
    void CollectPatternCaptures(PyNode *pat, PyNode *case_node) {
        if (pat == nullptr) return;
        if (pat->kind == PyNodeKind::Name) {
            if (pat->name != "_") {
                PyNodePtr cap = MakeNode(PyNodeKind::Name, pat->start);
                cap->name = pat->name;
                cap->end = pat->end;
                case_node->targets.push_back(std::move(cap));
                pat->kind = PyNodeKind::Placeholder;  // no longer a load
            }
            return;
        }
        if (pat->kind == PyNodeKind::Call) {
            // `Point(x=px, y=py)`: the callee is a value, the keyword
            // values are sub-patterns.
            for (size_t k = 1; k < pat->kids.size(); k++) {
                PyNode *arg = pat->kids[k].get();
                CollectPatternCaptures(arg->kind == PyNodeKind::Keyword && !arg->kids.empty() ? arg->kids[0].get() : arg,
                                       case_node);
            }
            return;
        }
        if (pat->kind == PyNodeKind::List || pat->kind == PyNodeKind::Tuple || pat->kind == PyNodeKind::Set ||
            pat->kind == PyNodeKind::Starred) {
            for (PyNodePtr &kid : pat->kids) CollectPatternCaptures(kid.get(), case_node);
            return;
        }
        if (pat->kind == PyNodeKind::Dict) {
            for (size_t k = 1; k < pat->kids.size(); k += 2) CollectPatternCaptures(pat->kids[k].get(), case_node);
            return;
        }
        if (pat->kind == PyNodeKind::BinOp && pat->name == "|") {
            for (PyNodePtr &kid : pat->kids) CollectPatternCaptures(kid.get(), case_node);
        }
    }

    /** @brief Reports whether this is a PEP 695 `type X = ...` alias rather than a use of the name `type`. */
    bool IsTypeAliasStatement() const {
        if (!AtName("type")) return false;
        if (Ahead(1).kind != PyTokKind::Name) return false;
        const PyToken &after = Ahead(2);
        if (after.kind == PyTokKind::Op && after.text == "=") return true;
        return after.kind == PyTokKind::Op && after.text == "[";
    }

    /** @brief Parses `type Alias = <expr>`. */
    PyNodePtr ParseTypeAlias() {
        const PyPos start = Here();
        Advance();  // 'type'
        PyNodePtr node = MakeNode(PyNodeKind::TypeAlias, start);
        node->name = ExpectName();
        node->end = PrevEnd();
        SkipTypeParams();
        if (ExpectOp("=")) node->kids.push_back(ParseTest());
        node->end = PrevEnd();
        ExpectNewline();
        return node;
    }

    // --- Simple statements --------------------------------------------

    /** @brief Parses a whole logical line of `;`-separated simple statements. */
    void ParseSimpleLineInto(std::vector<PyNodePtr> &out) {
        while (true) {
            const size_t before = i_;
            PyNodePtr stmt = ParseSimpleStatement();
            if (stmt) out.push_back(std::move(stmt));
            if (i_ == before) {
                RecoverLine();
                return;
            }
            if (AcceptOp(";")) {
                if (Cur().kind == PyTokKind::Newline || Cur().kind == PyTokKind::End) break;
                continue;
            }
            break;
        }
        ExpectNewline();
    }

    /** @brief Parses one simple statement (no trailing newline handling). */
    PyNodePtr ParseSimpleStatement() {
        const PyPos start = Here();
        if (AtKw("pass")) {
            Advance();
            return Finish(MakeNode(PyNodeKind::Pass, start));
        }
        if (AtKw("break")) {
            Advance();
            return Finish(MakeNode(PyNodeKind::Break, start));
        }
        if (AtKw("continue")) {
            Advance();
            return Finish(MakeNode(PyNodeKind::Continue, start));
        }
        if (AtKw("return")) {
            Advance();
            PyNodePtr node = MakeNode(PyNodeKind::Return, start);
            if (!AtLineEnd()) node->kids.push_back(ParseTestListStarExpr());
            return Finish(std::move(node));
        }
        if (AtKw("raise")) {
            Advance();
            PyNodePtr node = MakeNode(PyNodeKind::Raise, start);
            if (!AtLineEnd()) {
                node->kids.push_back(ParseTest());
                if (AcceptKw("from")) node->kids.push_back(ParseTest());
            }
            return Finish(std::move(node));
        }
        if (AtKw("global") || AtKw("nonlocal")) {
            const bool is_global = Cur().text == "global";
            Advance();
            PyNodePtr node = MakeNode(is_global ? PyNodeKind::Global : PyNodeKind::Nonlocal, start);
            do {
                PyNodePtr name = MakeNode(PyNodeKind::Name, Here());
                name->name = ExpectName();
                name->end = PrevEnd();
                node->kids.push_back(std::move(name));
            } while (AcceptOp(","));
            return Finish(std::move(node));
        }
        if (AtKw("del")) {
            Advance();
            PyNodePtr node = MakeNode(PyNodeKind::Delete, start);
            do {
                node->targets.push_back(ParseTarget());
            } while (AcceptOp(",") && !AtLineEnd());
            return Finish(std::move(node));
        }
        if (AtKw("assert")) {
            Advance();
            PyNodePtr node = MakeNode(PyNodeKind::Assert, start);
            node->kids.push_back(ParseTest());
            if (AcceptOp(",")) node->kids.push_back(ParseTest());
            return Finish(std::move(node));
        }
        if (AtKw("import")) return ParseImport();
        if (AtKw("from")) return ParseImportFrom();
        if (AtName("print") && LooksLikePython2Call()) {
            Error(Cur(), "python2-print", "Python 2 print statement; use print(...)");
            RecoverLine();
            return nullptr;
        }
        if (AtName("exec") && LooksLikePython2Call()) {
            Error(Cur(), "python2-exec", "Python 2 exec statement; use exec(...)");
            RecoverLine();
            return nullptr;
        }
        return ParseExprStatement();
    }

    /** @brief Reports whether a bare name is followed by something only a Python 2 statement could be. */
    bool LooksLikePython2Call() const {
        const PyToken &next = Ahead(1);
        if (next.kind == PyTokKind::String || next.kind == PyTokKind::Number || next.kind == PyTokKind::Name) {
            return true;
        }
        if (next.kind == PyTokKind::Keyword) {
            return next.text == "not" || next.text == "None" || next.text == "True" || next.text == "False" ||
                   next.text == "lambda";
        }
        if (next.kind == PyTokKind::Op) return next.text == ">>";  // `print >>sys.stderr, x`
        return false;
    }

    /** @brief Fills in a node's end position from the last consumed token. */
    PyNodePtr Finish(PyNodePtr node) {
        node->end = PrevEnd();
        return node;
    }

    /** @brief Reports whether the current token ends the statement. */
    bool AtLineEnd() const {
        return Cur().kind == PyTokKind::Newline || Cur().kind == PyTokKind::End ||
               Cur().kind == PyTokKind::Dedent || AtOp(";");
    }

    /** @brief Parses `import a.b as c, d`. */
    PyNodePtr ParseImport() {
        const PyPos start = Here();
        Advance();
        PyNodePtr node = MakeNode(PyNodeKind::Import, start);
        do {
            const PyPos astart = Here();
            PyNodePtr alias = MakeNode(PyNodeKind::Alias, astart);
            alias->name_pos = astart;
            std::string dotted = ExpectName();
            while (AtOp(".") && Ahead(1).kind == PyTokKind::Name) {
                Advance();
                dotted += "." + Cur().text;
                Advance();
            }
            alias->name = dotted;
            if (AcceptKw("as")) alias->str_value = ExpectName();
            alias->end = PrevEnd();
            node->kids.push_back(std::move(alias));
        } while (AcceptOp(","));
        return Finish(std::move(node));
    }

    /** @brief Parses `from .pkg.mod import a as b, c` (and `import *`). */
    PyNodePtr ParseImportFrom() {
        const PyPos start = Here();
        Advance();
        PyNodePtr node = MakeNode(PyNodeKind::ImportFrom, start);
        std::string module;
        while (AtOp(".") || AtOp("...")) {
            module += Cur().text;
            Advance();
        }
        if (!AtKw("import")) {
            module += ExpectName();
            while (AtOp(".") && Ahead(1).kind == PyTokKind::Name) {
                Advance();
                module += "." + Cur().text;
                Advance();
            }
        }
        node->name = module;
        ExpectKw("import");
        if (AcceptOp("*")) {
            PyNodePtr alias = MakeNode(PyNodeKind::Alias, PrevEnd());
            alias->name = "*";
            alias->end = PrevEnd();
            node->kids.push_back(std::move(alias));
            return Finish(std::move(node));
        }
        const bool parenthesized = AcceptOp("(");
        do {
            if (parenthesized && AtOp(")")) break;  // a trailing comma before the ')'
            const PyPos astart = Here();
            PyNodePtr alias = MakeNode(PyNodeKind::Alias, astart);
            alias->name_pos = astart;
            alias->name = ExpectName();
            if (AcceptKw("as")) alias->str_value = ExpectName();
            alias->end = PrevEnd();
            node->kids.push_back(std::move(alias));
        } while (AcceptOp(","));
        if (parenthesized) ExpectOp(")");
        return Finish(std::move(node));
    }

    /** @brief Parses an expression statement: a bare expression, an assignment, or an annotated/augmented one. */
    PyNodePtr ParseExprStatement() {
        const PyPos start = Here();
        PyNodePtr first = ParseTestListStarExpr();
        if (AtOp(":") && !AtLineEnd()) {
            Advance();
            PyNodePtr node = MakeNode(PyNodeKind::AnnAssign, start);
            node->targets.push_back(std::move(first));
            node->kids.push_back(ParseTest());
            if (AcceptOp("=")) node->kids.push_back(ParseTestListStarExpr());
            return Finish(std::move(node));
        }
        static const char *const kAugOps[] = {"+=", "-=", "*=", "/=", "//=", "%=", "@=", "&=",
                                              "|=", "^=", ">>=", "<<=", "**="};
        for (const char *op : kAugOps) {
            if (!AtOp(op)) continue;
            PyNodePtr node = MakeNode(PyNodeKind::AugAssign, start);
            node->name = op;
            Advance();
            node->targets.push_back(std::move(first));
            node->kids.push_back(AtKw("yield") ? ParseYield() : ParseTestListStarExpr());
            return Finish(std::move(node));
        }
        if (AtOp("=")) {
            PyNodePtr node = MakeNode(PyNodeKind::Assign, start);
            node->targets.push_back(std::move(first));
            PyNodePtr value;
            while (AcceptOp("=")) {
                value = AtKw("yield") ? ParseYield() : ParseTestListStarExpr();
                if (AtOp("=")) {
                    node->targets.push_back(std::move(value));  // `a = b = c`: every but the last is a target
                }
            }
            if (value) node->kids.push_back(std::move(value));
            return Finish(std::move(node));
        }
        PyNodePtr node = MakeNode(PyNodeKind::ExprStmt, start);
        node->kids.push_back(std::move(first));
        return Finish(std::move(node));
    }

    // --- Targets ------------------------------------------------------

    /** @brief Parses one assignment target (a name, attribute, subscript, star-target or bracketed list). */
    PyNodePtr ParseTarget() {
        if (AtOp("*")) {
            const PyPos start = Here();
            Advance();
            PyNodePtr node = MakeNode(PyNodeKind::Starred, start);
            node->name = "*";
            node->kids.push_back(ParseTarget());
            node->end = PrevEnd();
            return node;
        }
        return ParseAtomTrailers();
    }

    /** @brief Parses a comma-separated target list, stopping before `stop_kw`; one target stays un-tupled. */
    PyNodePtr ParseTargetList(const char *stop_kw) {
        const PyPos start = Here();
        std::vector<PyNodePtr> items;
        while (true) {
            if (Cur().kind == PyTokKind::Keyword && Cur().text == stop_kw) break;
            items.push_back(ParseTarget());
            if (!AcceptOp(",")) break;
            if (Cur().kind == PyTokKind::Keyword && Cur().text == stop_kw) break;
            if (AtLineEnd() || AtOp(":")) break;
        }
        if (items.size() == 1) return std::move(items[0]);
        PyNodePtr tup = MakeNode(PyNodeKind::Tuple, start);
        tup->kids = std::move(items);
        tup->end = PrevEnd();
        return tup;
    }

    // --- Expressions --------------------------------------------------

    // Every expression entry point goes through Guard(), which is what
    // keeps a pathological input (`((((((...`, a thousand unary minuses)
    // from recursing into a stack overflow inside an editor's own
    // process.
    struct DepthGuard {
        Parser *owner;
        explicit DepthGuard(Parser *p) : owner(p) { owner->depth_++; }
        ~DepthGuard() { owner->depth_--; }
        DepthGuard(const DepthGuard &) = delete;
        DepthGuard(DepthGuard &&) = delete;
        DepthGuard &operator=(const DepthGuard &) = delete;
        DepthGuard &operator=(DepthGuard &&) = delete;
    };

    /** @brief Reports whether the recursion limit has been hit, recording one error the first time. */
    bool TooDeep() {
        if (depth_ < kMaxDepth) return false;
        if (!reported_deep_) {
            reported_deep_ = true;
            Error(Cur(), "too-deep", "Expression nests too deeply to analyze");
        }
        return true;
    }

    /** @brief Parses a comma-separated expression list, building a Tuple when there is more than one item. */
    PyNodePtr ParseTestListStarExpr() {
        const PyPos start = Here();
        PyNodePtr first = ParseStarOrNamed();
        if (!AtOp(",")) return first;
        PyNodePtr tup = MakeNode(PyNodeKind::Tuple, start);
        tup->kids.push_back(std::move(first));
        while (AcceptOp(",")) {
            if (AtLineEnd() || AtOp("=") || AtOp(")") || AtOp("]") || AtOp("}") || AtOp(":")) break;
            tup->kids.push_back(ParseStarOrNamed());
        }
        tup->end = PrevEnd();
        return tup;
    }

    /** @brief Parses `*expr`, `**expr` or a walrus-capable test. */
    PyNodePtr ParseStarOrNamed() {
        if (AtOp("*") || AtOp("**")) {
            const PyPos start = Here();
            const std::string star = Cur().text;
            Advance();
            PyNodePtr node = MakeNode(PyNodeKind::Starred, start);
            node->name = star;
            node->kids.push_back(ParseTest());
            node->end = PrevEnd();
            return node;
        }
        return ParseNamedTest();
    }

    /** @brief Parses a test, optionally a walrus assignment (`n := f()`). */
    PyNodePtr ParseNamedTest() {
        const PyPos start = Here();
        if (Cur().kind == PyTokKind::Name && Ahead(1).kind == PyTokKind::Op && Ahead(1).text == ":=") {
            PyNodePtr target = MakeNode(PyNodeKind::Name, start);
            target->name = Cur().text;
            target->end = Cur().end;
            Advance();
            Advance();
            PyNodePtr node = MakeNode(PyNodeKind::NamedExpr, start);
            node->targets.push_back(std::move(target));
            node->kids.push_back(ParseTest());
            node->end = PrevEnd();
            return node;
        }
        return ParseTest();
    }

    /** @brief Parses a conditional expression, a lambda, or anything looser below them. */
    PyNodePtr ParseTest() {
        DepthGuard guard(this);
        if (TooDeep()) {
            PyNodePtr bad = MakeNode(PyNodeKind::Placeholder, Here());
            RecoverLine();
            return bad;
        }
        if (AtKw("lambda")) return ParseLambda();
        if (AtKw("yield")) return ParseYield();
        const PyPos start = Here();
        PyNodePtr value = ParseOrTest();
        if (!AtKw("if")) return value;
        Advance();
        PyNodePtr node = MakeNode(PyNodeKind::IfExp, start);
        node->kids.push_back(std::move(value));
        node->kids.push_back(ParseOrTest());
        if (ExpectKw("else")) node->kids.push_back(ParseTest());
        node->end = PrevEnd();
        return node;
    }

    /** @brief Parses `lambda params: body`. */
    PyNodePtr ParseLambda() {
        const PyPos start = Here();
        Advance();
        PyNodePtr node = MakeNode(PyNodeKind::Lambda, start);
        while (!AtOp(":") && !AtLineEnd()) {
            PyNodePtr param = MakeNode(PyNodeKind::Param, Here());
            if (AtOp("/")) {
                param->str_value = "/";  // end of positional-only parameters
                Advance();
                param->end = PrevEnd();
                node->params.push_back(std::move(param));
                if (!AcceptOp(",")) break;
                continue;
            }
            if (AtOp("*") || AtOp("**")) {
                param->str_value = Cur().text;
                Advance();
                if (Cur().kind != PyTokKind::Name) {
                    // A bare `*` separating keyword-only parameters.
                    param->end = PrevEnd();
                    node->params.push_back(std::move(param));
                    if (!AcceptOp(",")) break;
                    continue;
                }
            }
            param->name_pos = Here();
            param->name = ExpectName();
            if (AcceptOp("=")) param->kids.push_back(ParseTest());
            param->end = PrevEnd();
            node->params.push_back(std::move(param));
            if (!AcceptOp(",")) break;
        }
        if (ExpectOp(":")) node->kids.push_back(ParseTest());
        node->end = PrevEnd();
        return node;
    }

    /** @brief Parses `yield`/`yield from`. */
    PyNodePtr ParseYield() {
        const PyPos start = Here();
        Advance();
        if (AcceptKw("from")) {
            PyNodePtr node = MakeNode(PyNodeKind::YieldFrom, start);
            node->kids.push_back(ParseTest());
            node->end = PrevEnd();
            return node;
        }
        PyNodePtr node = MakeNode(PyNodeKind::Yield, start);
        if (!AtLineEnd() && !AtOp(")") && !AtOp("]") && !AtOp("}") && !AtOp(",")) {
            node->kids.push_back(ParseTestListStarExpr());
        }
        node->end = PrevEnd();
        return node;
    }

    /** @brief Parses `a or b`. */
    PyNodePtr ParseOrTest() {
        const PyPos start = Here();
        PyNodePtr left = ParseAndTest();
        if (!AtKw("or")) return left;
        PyNodePtr node = MakeNode(PyNodeKind::BoolOp, start);
        node->name = "or";
        node->kids.push_back(std::move(left));
        while (AcceptKw("or")) node->kids.push_back(ParseAndTest());
        node->end = PrevEnd();
        return node;
    }

    /** @brief Parses `a and b`. */
    PyNodePtr ParseAndTest() {
        const PyPos start = Here();
        PyNodePtr left = ParseNotTest();
        if (!AtKw("and")) return left;
        PyNodePtr node = MakeNode(PyNodeKind::BoolOp, start);
        node->name = "and";
        node->kids.push_back(std::move(left));
        while (AcceptKw("and")) node->kids.push_back(ParseNotTest());
        node->end = PrevEnd();
        return node;
    }

    /** @brief Parses `not a`. */
    PyNodePtr ParseNotTest() {
        if (!AtKw("not")) return ParseComparison();
        const PyPos start = Here();
        Advance();
        DepthGuard guard(this);
        if (TooDeep()) return MakeNode(PyNodeKind::Placeholder, start);
        PyNodePtr node = MakeNode(PyNodeKind::UnaryOp, start);
        node->name = "not";
        node->kids.push_back(ParseNotTest());
        node->end = PrevEnd();
        return node;
    }

    /** @brief Parses a comparison chain (`a < b <= c`, `x is not None`, `k not in d`). */
    PyNodePtr ParseComparison() {
        const PyPos start = Here();
        PyNodePtr left = ParseBinary(0);
        std::string ops;
        PyNodePtr node;
        while (true) {
            std::string op;
            if (Cur().kind == PyTokKind::Op &&
                (Cur().text == "<" || Cur().text == ">" || Cur().text == "==" || Cur().text == ">=" ||
                 Cur().text == "<=" || Cur().text == "!=")) {
                op = Cur().text;
                Advance();
            } else if (AtKw("in")) {
                op = "in";
                Advance();
            } else if (AtKw("not") && Ahead(1).kind == PyTokKind::Keyword && Ahead(1).text == "in") {
                op = "not in";
                Advance();
                Advance();
            } else if (AtKw("is")) {
                op = "is";
                Advance();
                if (AcceptKw("not")) op = "is not";
            } else {
                break;
            }
            if (!node) {
                node = MakeNode(PyNodeKind::Compare, start);
                node->kids.push_back(std::move(left));
            }
            ops += (ops.empty() ? "" : " ") + op;
            node->kids.push_back(ParseBinary(0));
        }
        if (!node) return left;
        node->name = ops;
        node->end = PrevEnd();
        return node;
    }

    /** @brief Parses one precedence level of binary operators, climbing to tighter levels first. */
    PyNodePtr ParseBinary(size_t level) {
        if (level >= BinaryLevels().size()) return ParseFactor();
        DepthGuard guard(this);
        if (TooDeep()) return MakeNode(PyNodeKind::Placeholder, Here());
        const PyPos start = Here();
        PyNodePtr left = ParseBinary(level + 1);
        while (Cur().kind == PyTokKind::Op) {
            const std::vector<std::string> &ops = BinaryLevels()[level];
            if (std::find(ops.begin(), ops.end(), Cur().text) == ops.end()) break;
            const std::string op = Cur().text;
            Advance();
            PyNodePtr node = MakeNode(PyNodeKind::BinOp, start);
            node->name = op;
            node->kids.push_back(std::move(left));
            node->kids.push_back(ParseBinary(level + 1));
            node->end = PrevEnd();
            left = std::move(node);
        }
        return left;
    }

    /** @brief Parses unary `+`/`-`/`~` and `await`. */
    PyNodePtr ParseFactor() {
        if (AtOp("+") || AtOp("-") || AtOp("~")) {
            const PyPos start = Here();
            const std::string op = Cur().text;
            Advance();
            DepthGuard guard(this);
            if (TooDeep()) return MakeNode(PyNodeKind::Placeholder, start);
            PyNodePtr node = MakeNode(PyNodeKind::UnaryOp, start);
            node->name = op;
            node->kids.push_back(ParseFactor());
            node->end = PrevEnd();
            return node;
        }
        if (AtKw("await")) {
            const PyPos start = Here();
            Advance();
            PyNodePtr node = MakeNode(PyNodeKind::Await, start);
            node->kids.push_back(ParseFactor());
            node->end = PrevEnd();
            return node;
        }
        return ParsePower();
    }

    /** @brief Parses `a ** b`, which binds tighter than unary minus on its left and looser on its right. */
    PyNodePtr ParsePower() {
        const PyPos start = Here();
        PyNodePtr base = ParseAtomTrailers();
        if (!AtOp("**")) return base;
        Advance();
        PyNodePtr node = MakeNode(PyNodeKind::BinOp, start);
        node->name = "**";
        node->kids.push_back(std::move(base));
        node->kids.push_back(ParseFactor());
        node->end = PrevEnd();
        return node;
    }

    /** @brief Parses an atom followed by any run of `.name`, `(...)` and `[...]` trailers. */
    PyNodePtr ParseAtomTrailers() {
        DepthGuard guard(this);
        if (TooDeep()) return MakeNode(PyNodeKind::Placeholder, Here());
        PyNodePtr node = ParseAtom();
        while (true) {
            if (AtOp(".")) {
                const PyPos start = node->start;
                Advance();
                PyNodePtr attr = MakeNode(PyNodeKind::Attribute, start);
                attr->kids.push_back(std::move(node));
                // The attribute's own span is what hover and go-to
                // definition hit-test against, so it is recorded
                // separately from the whole expression's span.
                attr->name_pos = Cur().start;
                attr->name = Cur().kind == PyTokKind::Name || Cur().kind == PyTokKind::Keyword ? Cur().text : "";
                if (attr->name.empty()) {
                    Error(Cur(), "expected-name", "Expected an attribute name after '.'");
                } else {
                    Advance();
                }
                attr->end = PrevEnd();
                node = std::move(attr);
                continue;
            }
            if (AtOp("(")) {
                node = ParseCallArguments(std::move(node));
                continue;
            }
            if (AtOp("[")) {
                const PyPos start = node->start;
                Advance();
                PyNodePtr sub = MakeNode(PyNodeKind::Subscript, start);
                sub->kids.push_back(std::move(node));
                sub->kids.push_back(ParseSubscriptIndex());
                ExpectOp("]");
                sub->end = PrevEnd();
                node = std::move(sub);
                continue;
            }
            break;
        }
        return node;
    }

    /** @brief Parses whatever sits between `[` and `]` in a subscript, slices included. */
    PyNodePtr ParseSubscriptIndex() {
        const PyPos start = Here();
        std::vector<PyNodePtr> items;
        while (!AtOp("]") && !AtEnd() && Cur().kind != PyTokKind::Newline) {
            items.push_back(ParseSliceItem());
            if (!AcceptOp(",")) break;
        }
        if (items.size() == 1) return std::move(items[0]);
        PyNodePtr tup = MakeNode(PyNodeKind::Tuple, start);
        tup->kids = std::move(items);
        tup->end = PrevEnd();
        return tup;
    }

    /** @brief Parses one subscript element, which may be a `lower:upper:step` slice with omitted parts. */
    PyNodePtr ParseSliceItem() {
        const PyPos start = Here();
        PyNodePtr first;
        if (!AtOp(":")) {
            first = ParseStarOrNamed();
            if (!AtOp(":")) return first;
        }
        PyNodePtr slice = MakeNode(PyNodeKind::Slice, start);
        slice->kids.push_back(first ? std::move(first) : MakeNode(PyNodeKind::Placeholder, start));
        while (AcceptOp(":")) {
            if (AtOp("]") || AtOp(",") || AtOp(":")) {
                slice->kids.push_back(MakeNode(PyNodeKind::Placeholder, Here()));
                continue;
            }
            slice->kids.push_back(ParseTest());
        }
        slice->end = PrevEnd();
        return slice;
    }

    /** @brief Parses a call's argument list onto an already-parsed callee. */
    PyNodePtr ParseCallArguments(PyNodePtr callee) {
        const PyPos start = callee->start;
        PyNodePtr node = MakeNode(PyNodeKind::Call, start);
        node->kids.push_back(std::move(callee));
        ExpectOp("(");
        while (!AtOp(")") && !AtEnd()) {
            if (Cur().kind == PyTokKind::Newline || Cur().kind == PyTokKind::Dedent) break;
            if (AtOp("*") || AtOp("**")) {
                const PyPos astart = Here();
                const std::string star = Cur().text;
                Advance();
                PyNodePtr arg = MakeNode(PyNodeKind::Starred, astart);
                arg->name = star;
                arg->kids.push_back(ParseTest());
                arg->end = PrevEnd();
                node->kids.push_back(std::move(arg));
            } else if (Cur().kind == PyTokKind::Name && Ahead(1).kind == PyTokKind::Op && Ahead(1).text == "=") {
                const PyPos astart = Here();
                PyNodePtr arg = MakeNode(PyNodeKind::Keyword, astart);
                arg->name = Cur().text;
                Advance();
                Advance();
                arg->kids.push_back(ParseTest());
                arg->end = PrevEnd();
                node->kids.push_back(std::move(arg));
            } else {
                PyNodePtr arg = ParseNamedTest();
                if (AtKw("for") || (AtKw("async") && Ahead(1).kind == PyTokKind::Keyword && Ahead(1).text == "for")) {
                    // `sum(x * x for x in xs)`: a bare generator argument.
                    PyNodePtr gen = MakeNode(PyNodeKind::GeneratorExp, arg->start);
                    gen->kids.push_back(std::move(arg));
                    ParseComprehensionClauses(gen.get());
                    gen->end = PrevEnd();
                    arg = std::move(gen);
                }
                node->kids.push_back(std::move(arg));
            }
            if (!AcceptOp(",")) break;
        }
        ExpectOp(")");
        node->end = PrevEnd();
        return node;
    }

    /** @brief Parses the `for ... in ... [if ...]` clauses that turn a display into a comprehension. */
    void ParseComprehensionClauses(PyNode *owner) {
        while (true) {
            bool is_async = false;
            if (AtKw("async") && Ahead(1).kind == PyTokKind::Keyword && Ahead(1).text == "for") {
                is_async = true;
                Advance();
            }
            if (!AtKw("for")) break;
            const PyPos start = Here();
            Advance();
            PyNodePtr comp = MakeNode(PyNodeKind::Comprehension, start);
            comp->is_async = is_async;
            comp->targets.push_back(ParseTargetList("in"));
            ExpectKw("in");
            comp->kids.push_back(ParseOrTest());
            while (AtKw("if")) {
                Advance();
                comp->kids.push_back(ParseNamedTest());
            }
            comp->end = PrevEnd();
            owner->kids.push_back(std::move(comp));
            if (!AtKw("for") && !AtKw("async")) break;
        }
    }

    /** @brief Parses a function's parenthesized parameter list, annotations and defaults included. */
    void ParseParams(std::vector<PyNodePtr> &out) {
        ExpectOp("(");
        while (!AtOp(")") && !AtEnd()) {
            if (Cur().kind == PyTokKind::Newline || Cur().kind == PyTokKind::Dedent) break;
            const PyPos start = Here();
            PyNodePtr param = MakeNode(PyNodeKind::Param, start);
            if (AtOp("/")) {
                param->str_value = "/";  // end of positional-only parameters
                Advance();
                param->end = PrevEnd();
                out.push_back(std::move(param));
                if (!AcceptOp(",")) break;
                continue;
            }
            if (AtOp("*") || AtOp("**")) {
                param->str_value = Cur().text;
                Advance();
                if (Cur().kind != PyTokKind::Name) {
                    // A bare `*` separating keyword-only parameters.
                    param->end = PrevEnd();
                    out.push_back(std::move(param));
                    if (!AcceptOp(",")) break;
                    continue;
                }
            }
            param->name_pos = Here();
            param->name = ExpectName();
            if (AcceptOp(":")) param->kids.push_back(ParseTest());
            if (AcceptOp("=")) {
                if (param->kids.empty()) param->kids.push_back(MakeNode(PyNodeKind::Placeholder, Here()));
                param->kids.push_back(ParseTest());
            }
            param->end = PrevEnd();
            out.push_back(std::move(param));
            if (!AcceptOp(",")) break;
        }
        ExpectOp(")");
    }

    // --- Atoms --------------------------------------------------------

    /** @brief Parses a primary expression: a name, a literal, or a bracketed display/comprehension. */
    PyNodePtr ParseAtom() {
        const PyPos start = Here();
        if (Cur().kind == PyTokKind::Name) {
            PyNodePtr node = MakeNode(PyNodeKind::Name, start);
            node->name = Cur().text;
            node->name_pos = start;
            node->end = Cur().end;
            Advance();
            return node;
        }
        if (Cur().kind == PyTokKind::Number) {
            PyNodePtr node = MakeNode(PyNodeKind::Number, start);
            node->name = Cur().text;
            node->end = Cur().end;
            Advance();
            return node;
        }
        if (Cur().kind == PyTokKind::String) return ParseStringRun();
        if (AtKw("None") || AtKw("True") || AtKw("False")) {
            PyNodePtr node = MakeNode(PyNodeKind::Constant, start);
            node->name = Cur().text;
            node->end = Cur().end;
            Advance();
            return node;
        }
        if (AtOp("...")) {
            PyNodePtr node = MakeNode(PyNodeKind::Constant, start);
            node->name = "...";
            node->end = Cur().end;
            Advance();
            return node;
        }
        if (AtKw("lambda")) return ParseLambda();
        if (AtOp("(")) return ParseParenthesized();
        if (AtOp("[")) return ParseListDisplay();
        if (AtOp("{")) return ParseBraceDisplay();
        // Nothing here can start an expression. Consume it unless it is a
        // structural token the statement loops need to see, so the parser
        // always makes progress.
        Error(Cur(), "unexpected-token", "Unexpected " + Describe() + " in expression");
        PyNodePtr bad = MakeNode(PyNodeKind::Placeholder, start);
        if (Cur().kind != PyTokKind::Newline && Cur().kind != PyTokKind::Dedent && Cur().kind != PyTokKind::End) {
            Advance();
        }
        bad->end = PrevEnd();
        return bad;
    }

    /** @brief Parses `(...)`: an empty tuple, a parenthesized expression, a tuple, a generator or a yield. */
    PyNodePtr ParseParenthesized() {
        const PyPos start = Here();
        Advance();
        if (AtOp(")")) {
            PyNodePtr tup = MakeNode(PyNodeKind::Tuple, start);
            Advance();
            tup->end = PrevEnd();
            return tup;
        }
        if (AtKw("yield")) {
            PyNodePtr y = ParseYield();
            ExpectOp(")");
            return y;
        }
        PyNodePtr first = ParseStarOrNamed();
        if (AtKw("for") || (AtKw("async") && Ahead(1).kind == PyTokKind::Keyword && Ahead(1).text == "for")) {
            PyNodePtr gen = MakeNode(PyNodeKind::GeneratorExp, start);
            gen->kids.push_back(std::move(first));
            ParseComprehensionClauses(gen.get());
            ExpectOp(")");
            gen->end = PrevEnd();
            return gen;
        }
        if (AtOp(",")) {
            PyNodePtr tup = MakeNode(PyNodeKind::Tuple, start);
            tup->kids.push_back(std::move(first));
            while (AcceptOp(",")) {
                if (AtOp(")")) break;
                tup->kids.push_back(ParseStarOrNamed());
            }
            ExpectOp(")");
            tup->end = PrevEnd();
            return tup;
        }
        ExpectOp(")");
        // A parenthesized expression keeps the parentheses' span, so
        // hover and selection ranges cover what the reader sees.
        first->start = start;
        first->end = PrevEnd();
        return first;
    }

    /** @brief Parses `[...]`: a list display or a list comprehension. */
    PyNodePtr ParseListDisplay() {
        const PyPos start = Here();
        Advance();
        PyNodePtr node = MakeNode(PyNodeKind::List, start);
        if (AtOp("]")) {
            Advance();
            node->end = PrevEnd();
            return node;
        }
        PyNodePtr first = ParseStarOrNamed();
        if (AtKw("for") || (AtKw("async") && Ahead(1).kind == PyTokKind::Keyword && Ahead(1).text == "for")) {
            PyNodePtr comp = MakeNode(PyNodeKind::ListComp, start);
            comp->kids.push_back(std::move(first));
            ParseComprehensionClauses(comp.get());
            ExpectOp("]");
            comp->end = PrevEnd();
            return comp;
        }
        node->kids.push_back(std::move(first));
        while (AcceptOp(",")) {
            if (AtOp("]")) break;
            node->kids.push_back(ParseStarOrNamed());
        }
        ExpectOp("]");
        node->end = PrevEnd();
        return node;
    }

    /** @brief Parses `{...}`: a dict or set display, or either one's comprehension. */
    PyNodePtr ParseBraceDisplay() {
        const PyPos start = Here();
        Advance();
        if (AtOp("}")) {
            PyNodePtr node = MakeNode(PyNodeKind::Dict, start);
            Advance();
            node->end = PrevEnd();
            return node;
        }
        // `{**a, **b}` is a dict merge; `{*a, *b}` is a set union.
        if (AtOp("**")) {
            PyNodePtr node = MakeNode(PyNodeKind::Dict, start);
            while (true) {
                if (AcceptOp("**")) {
                    node->kids.push_back(MakeNode(PyNodeKind::Placeholder, Here()));
                    node->kids.push_back(ParseOrTest());
                } else {
                    node->kids.push_back(ParseTest());
                    ExpectOp(":");
                    node->kids.push_back(ParseTest());
                }
                if (!AcceptOp(",")) break;
                if (AtOp("}")) break;
            }
            ExpectOp("}");
            node->end = PrevEnd();
            return node;
        }
        PyNodePtr first = ParseStarOrNamed();
        if (AtOp(":")) {
            Advance();
            PyNodePtr value = ParseTest();
            if (AtKw("for") || (AtKw("async") && Ahead(1).kind == PyTokKind::Keyword && Ahead(1).text == "for")) {
                PyNodePtr comp = MakeNode(PyNodeKind::DictComp, start);
                comp->kids.push_back(std::move(first));
                comp->kids.push_back(std::move(value));
                ParseComprehensionClauses(comp.get());
                ExpectOp("}");
                comp->end = PrevEnd();
                return comp;
            }
            PyNodePtr node = MakeNode(PyNodeKind::Dict, start);
            node->kids.push_back(std::move(first));
            node->kids.push_back(std::move(value));
            while (AcceptOp(",")) {
                if (AtOp("}")) break;
                if (AcceptOp("**")) {
                    node->kids.push_back(MakeNode(PyNodeKind::Placeholder, Here()));
                    node->kids.push_back(ParseOrTest());
                    continue;
                }
                node->kids.push_back(ParseTest());
                ExpectOp(":");
                node->kids.push_back(ParseTest());
            }
            ExpectOp("}");
            node->end = PrevEnd();
            return node;
        }
        if (AtKw("for") || (AtKw("async") && Ahead(1).kind == PyTokKind::Keyword && Ahead(1).text == "for")) {
            PyNodePtr comp = MakeNode(PyNodeKind::SetComp, start);
            comp->kids.push_back(std::move(first));
            ParseComprehensionClauses(comp.get());
            ExpectOp("}");
            comp->end = PrevEnd();
            return comp;
        }
        PyNodePtr node = MakeNode(PyNodeKind::Set, start);
        node->kids.push_back(std::move(first));
        while (AcceptOp(",")) {
            if (AtOp("}")) break;
            node->kids.push_back(ParseStarOrNamed());
        }
        ExpectOp("}");
        node->end = PrevEnd();
        return node;
    }

    /** @brief Parses a run of adjacent string literals, which Python concatenates into one value. */
    PyNodePtr ParseStringRun() {
        const PyPos start = Here();
        PyNodePtr node = MakeNode(PyNodeKind::Str, start);
        std::string raw;
        std::string value;
        bool any_fstring = false;
        while (Cur().kind == PyTokKind::String) {
            const PyToken tok = Cur();
            raw += tok.text;
            value += DecodeStringToken(tok);
            if (tok.str_prefix.find('f') != std::string::npos) {
                any_fstring = true;
                ParseFStringInterpolations(tok, node.get());
            }
            node->end = tok.end;
            Advance();
        }
        node->kind = any_fstring ? PyNodeKind::FString : PyNodeKind::Str;
        node->name = raw;
        node->str_value = value;
        return node;
    }

    /** @brief Parses each `{...}` of an f-string as a real expression, positioned inside the literal. */
    void ParseFStringInterpolations(const PyToken &tok, PyNode *out);

    /** @brief Strips a string token's prefix, quotes and escapes, for docstrings and hover text. */
    static std::string DecodeStringToken(const PyToken &tok);
};

// --- String literal internals -----------------------------------------

/** @brief Locates a string token's content: the offset past its prefix and opening quotes, and the quote length. */
void StringContentSpan(const PyToken &tok, size_t *begin, size_t *end, int *quote_len) {
    const std::string &s = tok.text;
    size_t q = 0;
    while (q < s.size() && s[q] != '"' && s[q] != '\'') q++;
    if (q >= s.size()) {
        *begin = s.size();
        *end = s.size();
        *quote_len = 0;
        return;
    }
    const char quote = s[q];
    int len = 1;
    if (q + 2 < s.size() && s[q + 1] == quote && s[q + 2] == quote) len = 3;
    *quote_len = len;
    *begin = q + static_cast<size_t>(len);
    size_t stop = s.size();
    if (!tok.unterminated && stop >= *begin + static_cast<size_t>(len)) stop -= static_cast<size_t>(len);
    *end = stop < *begin ? *begin : stop;
}

std::string Parser::DecodeStringToken(const PyToken &tok) {
    size_t begin = 0, end = 0;
    int quote_len = 0;
    StringContentSpan(tok, &begin, &end, &quote_len);
    std::string content = tok.text.substr(begin, end - begin);
    if (tok.str_prefix.find('r') != std::string::npos) return content;
    // Only the escapes a reader of a docstring or a hover popup would
    // notice: the point is legible text, not a faithful bytes object.
    std::string out;
    out.reserve(content.size());
    for (size_t i = 0; i < content.size(); i++) {
        if (content[i] != '\\' || i + 1 >= content.size()) {
            out += content[i];
            continue;
        }
        const char esc = content[++i];
        switch (esc) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0': out += '\0'; break;
            case '\\': out += '\\'; break;
            case '\'': out += '\''; break;
            case '"': out += '"'; break;
            case '\n': break;  // a line continuation inside the literal
            default:
                out += '\\';
                out += esc;
                break;
        }
    }
    return out;
}

void Parser::ParseFStringInterpolations(const PyToken &tok, PyNode *out) {
    size_t begin = 0, end = 0;
    int quote_len = 0;
    StringContentSpan(tok, &begin, &end, &quote_len);
    const std::string &s = tok.text;
    int line = tok.start.line;
    int col = tok.start.col;
    // Walk to the content, keeping the position in step (a triple-quoted
    // f-string's opening quotes never cross a line, but its content does).
    for (size_t i = 0; i < begin && i < s.size(); i++) {
        if (s[i] == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
    }
    for (size_t i = begin; i < end; i++) {
        const char c = s[i];
        if (c == '\n') {
            line++;
            col = 0;
            continue;
        }
        if ((c == '{' && i + 1 < end && s[i + 1] == '{') || (c == '}' && i + 1 < end && s[i + 1] == '}')) {
            col += 2;
            i++;
            continue;
        }
        if (c != '{') {
            col++;
            continue;
        }
        // An interpolation. Scan to whichever comes first: the closing
        // brace, the `!r` conversion, the `:` format spec, or the `=`
        // debug suffix -- everything after those is not an expression.
        col++;
        i++;
        const int expr_line = line;
        const int expr_col = col;
        const size_t expr_start = i;
        int depth = 0;
        char in_quote = '\0';
        for (; i < end; i++) {
            const char d = s[i];
            if (d == '\n') {
                line++;
                col = 0;
                continue;
            }
            col++;
            if (in_quote != '\0') {
                if (d == '\\') {
                    i++;
                    col++;
                } else if (d == in_quote) {
                    in_quote = '\0';
                }
                continue;
            }
            if (d == '\'' || d == '"') {
                in_quote = d;
                continue;
            }
            if (d == '(' || d == '[' || d == '{') {
                depth++;
                continue;
            }
            if (d == ')' || d == ']') {
                depth--;
                continue;
            }
            if (d == '}') {
                if (depth == 0) break;
                depth--;
                continue;
            }
            if (depth > 0) continue;
            if (d == '!' && i + 1 < end && s[i + 1] != '=') break;
            if (d == ':') break;
            if (d == '=' && i + 1 < end && (s[i + 1] == '}' || s[i + 1] == '!' || s[i + 1] == ':')) break;
        }
        const size_t expr_end = i;
        // Skip whatever is left of this interpolation (conversion,
        // format spec, closing brace), keeping the position in step.
        while (i < end && s[i] != '}') {
            if (s[i] == '\n') {
                line++;
                col = 0;
            } else {
                col++;
            }
            i++;
        }
        if (expr_end <= expr_start) continue;
        const std::string expr_text = s.substr(expr_start, expr_end - expr_start);
        if (expr_text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        std::vector<std::string> sub_lines;
        std::string cur;
        for (char ch : expr_text) {
            if (ch == '\n') {
                sub_lines.push_back(cur);
                cur.clear();
            } else {
                cur += ch;
            }
        }
        sub_lines.push_back(cur);
        std::vector<PySyntaxError> sub_errors;
        const std::vector<PyToken> sub_tokens = TokenizePython(sub_lines, &sub_errors, nullptr, nullptr);
        Parser sub(sub_tokens, &sub_errors);
        PyNodePtr expr = sub.ParseSingleExpression();
        ShiftNode(expr.get(), expr_line, expr_col);
        if (errors_ != nullptr) {
            for (PySyntaxError err : sub_errors) {
                if (err.start.line == 0) err.start.col += expr_col;
                if (err.end.line == 0) err.end.col += expr_col;
                err.start.line += expr_line;
                err.end.line += expr_line;
                errors_->push_back(err);
            }
        }
        out->kids.push_back(std::move(expr));
    }
}

}  // namespace

PyParseResult ParsePython(const std::vector<std::string> &lines) {
    static const std::vector<std::string> kEmptyDoc{std::string()};
    const std::vector<std::string> &src = lines.empty() ? kEmptyDoc : lines;
    PyParseResult result;
    result.tokens = TokenizePython(src, &result.errors, &result.comments, &result.depth_at_line);
    Parser parser(result.tokens, &result.errors);
    result.module = parser.ParseModule();
    std::stable_sort(result.errors.begin(), result.errors.end(),
                     [](const PySyntaxError &a, const PySyntaxError &b) {
                         if (a.start.line != b.start.line) return a.start.line < b.start.line;
                         return a.start.col < b.start.col;
                     });
    // One report per position: the tokenizer and the parser both have
    // something to say about, say, an unterminated string, and a stack of
    // squiggles on one character is noise.
    std::vector<PySyntaxError> unique;
    for (const PySyntaxError &e : result.errors) {
        if (!unique.empty() && unique.back().start.line == e.start.line && unique.back().start.col == e.start.col) {
            continue;
        }
        unique.push_back(e);
    }
    result.errors = std::move(unique);
    return result;
}
