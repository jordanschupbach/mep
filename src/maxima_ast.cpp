// The Maxima reader: lexer, then a Pratt parser using Maxima's own
// binding powers. See maxima_ast.h for the contract and for why the
// numbers in kOperators are not invented.

#include "maxima_ast.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

// --- Ranges -----------------------------------------------------------

bool MxRangeContains(const MxRange &r, int line, int col) {
    if (line < r.line || line > r.end_line) return false;
    if (line == r.line && col < r.col) return false;
    if (line == r.end_line && col >= r.end_col) return false;
    return true;
}

const MxNode &MxParseResult::at(int index) const {
    static const MxNode kEmpty;
    if (index < 0 || static_cast<size_t>(index) >= nodes.size()) return kEmpty;
    return nodes[static_cast<size_t>(index)];
}

// --- Character classes ------------------------------------------------

bool MxIsNameChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    // Bytes above ASCII are part of a name: Maxima's `alphabetp` accepts
    // whatever the host Lisp calls a letter, which on a Unicode-capable
    // build includes accented names, and a UTF-8 continuation byte must
    // never split an identifier in half here.
    if (u >= 0x80) return true;
    return std::isalnum(u) != 0 || c == '_' || c == '%';
}

bool MxIsNameStart(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 0x80) return true;
    return std::isalpha(u) != 0 || c == '_' || c == '%';
}

// --- Operator table ---------------------------------------------------

namespace {

// Maxima's own lbp/rbp, read out of a live image (`(get '$+ 'lbp)`) and
// written down. Longest spelling first within a shared prefix, since the
// lexer matches greedily: `::=` must beat `::`, which must beat `:`.
const std::vector<MxOperatorInfo> kOperators = {
    {"::=", 180, 20, 0, false},  // macro definition
    {":=", 180, 20, 0, false},   // function definition
    {"::", 180, 20, 0, false},   // assign to the value of the left side
    {":", 180, 20, 0, false},    // assignment
    {"!!", 160, 0, 0, true},     // double factorial
    {"!", 160, 0, 0, true},      // factorial
    {"^^", 140, 139, 0, false},  // non-commutative exponent
    {"**", 140, 139, 0, false},  // exponent, FORTRAN spelling
    {"^", 140, 139, 0, false},   // exponent (right-associative: rbp < lbp)
    {".", 130, 129, 0, false},   // non-commutative product
    {"*", 120, 120, 0, false},
    {"/", 120, 120, 0, false},
    // Prefix `+`/`-` bind at 134 -- tighter than `*` (120), looser than
    // `^` (140), which is why `-x^2` is `-(x^2)` and `-a*b` is `(-a)*b`.
    {"+", 100, 100, 134, false},
    {"-", 100, 100, 134, false},
    {"->", 80, 80, 0, false},  // used by pattern-matching packages
    {"<=", 80, 80, 0, false},
    {">=", 80, 80, 0, false},
    {"#", 80, 80, 0, false},  // not equal
    {"<", 80, 80, 0, false},
    {">", 80, 80, 0, false},
    {"=", 80, 80, 0, false},  // an equation, not an assignment
    {"not", 0, 0, 70, false},
    {"and", 65, 65, 0, false},
    {"or", 60, 60, 0, false},
    {"@", 200, 201, 0, false},  // a defstruct field
};

// `,` is a real infix operator in Maxima -- `integrate(x, x), x = 2` is
// the `ev` shorthand -- but only where it is not separating arguments,
// which the parser arranges by reading argument lists at binding power
// 10 and statements at 0.
constexpr int kCommaLbp = 10;

// What a declaration with no binding powers of its own gets, and what an
// undeclared operator is read at when the parser recovers past one.
constexpr int kUserDefaultBp = 180;

const MxOperatorInfo *FindOp(const std::string &text) {
    for (const MxOperatorInfo &op : kOperators) {
        if (text == op.text) return &op;
    }
    return nullptr;
}

}  // namespace

const MxOperatorInfo *MxFindOperator(const std::string &text) { return FindOp(text); }

const std::vector<MxOperatorInfo> &MxOperatorTable() { return kOperators; }

// --- Lexer ------------------------------------------------------------

namespace {

struct Keyword {
    const char *text;
    MxTokKind kind;
};

// Maxima is case-sensitive, so these exact spellings and no others.
// `and`/`or`/`not` are operators rather than keywords and live in
// kOperators instead.
const Keyword kKeywords[] = {
    {"if", MxTokKind::If},         {"then", MxTokKind::Then}, {"else", MxTokKind::Else},
    {"elseif", MxTokKind::Elseif}, {"for", MxTokKind::For},   {"from", MxTokKind::From},
    {"step", MxTokKind::Step},     {"next", MxTokKind::Next}, {"thru", MxTokKind::Thru},
    {"while", MxTokKind::While},   {"unless", MxTokKind::Unless}, {"do", MxTokKind::Do},
};

class Lexer {
  public:
    Lexer(const std::vector<std::string> &lines, std::vector<MxComment> *comments, std::vector<MxParseError> *errors,
          const std::vector<MxUserOperator> *user_ops)
        : lines_(lines), comments_(comments), errors_(errors), user_ops_(user_ops) {}

    std::vector<MxToken> Run() {
        std::vector<MxToken> out;
        while (true) {
            SkipTrivia();
            if (AtEnd()) break;
            out.push_back(Next());
            if (out.back().kind == MxTokKind::End) break;
        }
        MxToken end;
        end.kind = MxTokKind::End;
        end.range = Here();
        out.push_back(end);
        return out;
    }

  private:
    const std::vector<std::string> &lines_;
    std::vector<MxComment> *comments_;
    std::vector<MxParseError> *errors_;
    const std::vector<MxUserOperator> *user_ops_ = nullptr;
    int line_ = 0;
    int col_ = 0;

    bool AtEnd() const { return line_ >= static_cast<int>(lines_.size()); }

    MxRange Here() {
        Sync();
        MxRange r;
        r.line = r.end_line = line_;
        r.col = r.end_col = col_;
        return r;
    }

    char RawPeek(int ahead) const {
        if (AtEnd()) return '\0';
        const std::string &l = lines_[static_cast<size_t>(line_)];
        const size_t i = static_cast<size_t>(col_) + static_cast<size_t>(ahead);
        if (i >= l.size()) return '\n';  // the newline itself: whitespace, never part of a token
        return l[i];
    }

    void RawAdvance() {
        if (AtEnd()) return;
        const std::string &l = lines_[static_cast<size_t>(line_)];
        if (static_cast<size_t>(col_) >= l.size()) {
            line_++;
            col_ = 0;
        } else {
            col_++;
        }
    }

    // A backslash at end of line, and the line break after it, are
    // deleted before anything else sees them. Maxima does this in its
    // character reader (`backslash-check` in src/commac.lisp), below the
    // tokenizer, so a continuation can fall in the middle of a *number*
    // or a name -- and one of its own test files splits a 300-digit
    // integer across four lines that way.
    void Sync() {
        while (!AtEnd() && RawPeek(0) == '\\' && RawPeek(1) == '\n') {
            RawAdvance();
            RawAdvance();
        }
    }

    char Peek(int ahead = 0) {
        Sync();
        return RawPeek(ahead);
    }

    /** @brief Whether the character `ahead` positions from here ends the line. */
    bool AtLineEnd(int ahead) { return Peek(ahead) == '\n'; }

    void Advance() {
        Sync();
        RawAdvance();
    }

    void Error(const MxRange &range, const char *code, const std::string &message) {
        if (errors_ == nullptr) return;
        MxParseError e;
        e.range = range;
        e.code = code;
        e.message = message;
        errors_->push_back(e);
    }

    // Whitespace and comments, which may nest: Maxima's own
    // `gobble-comment` counts depth, so `/* a /* b */ c */` is one
    // comment and a reader that stopped at the first `*/` would treat the
    // rest of the file as code.
    void SkipTrivia() {
        while (!AtEnd()) {
            const char c = Peek();
            // Form feed and vertical tab are whitespace to Maxima's own
            // reader, and a page break between sections is ordinary
            // house style in the shipped packages (ode2.mac has four).
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                Advance();
                continue;
            }
            // A backslash at end of line joins it to the next one and
            // produces no token of its own: `[ \` then `"a", \` is a
            // two-element list, not a list with two stray names in it.
            if (c == '\\' && AtLineEnd(1)) {
                Advance();
                Advance();
                continue;
            }
            if (c == '/' && Peek(1) == '*') {
                ReadComment();
                continue;
            }
            return;
        }
    }

    void ReadComment() {
        const MxRange start = Here();
        std::string text;
        int depth = 0;
        while (!AtEnd()) {
            const char c = Peek();
            if (c == '/' && Peek(1) == '*') {
                depth++;
                text += "/*";
                Advance();
                Advance();
                continue;
            }
            if (c == '*' && Peek(1) == '/') {
                depth--;
                text += "*/";
                Advance();
                Advance();
                if (depth == 0) break;
                continue;
            }
            text += (c == '\n' ? '\n' : c);
            Advance();
        }
        MxRange range = start;
        range.end_line = line_;
        range.end_col = col_;
        if (depth != 0) {
            Error(range, "unterminated-comment", "This comment is never closed (`*/` is missing).");
        }
        if (comments_ != nullptr) {
            MxComment com;
            com.text = text;
            com.range = range;
            comments_->push_back(com);
        }
    }

    MxToken Make(MxTokKind kind, const MxRange &start, const std::string &text) const {
        MxToken t;
        t.kind = kind;
        t.text = text;
        t.range = start;
        t.range.end_line = line_;
        t.range.end_col = col_;
        return t;
    }

    MxToken Next() {
        const MxRange start = Here();
        const char c = Peek();

        if (c == '"') return ReadString(start);
        if (c == '?') return ReadLispName(start);
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) return ReadNumber(start);
        // A dot that a digit follows is the start of a number, not the
        // non-commutative product: `2.3.4` lexes as `2.3` then `.4`,
        // which is exactly why Maxima rejects it.
        if (c == '.' && std::isdigit(static_cast<unsigned char>(Peek(1))) != 0) return ReadNumber(start);
        if (MxIsNameStart(c) || c == '\\') return ReadName(start);

        switch (c) {
            case '(': Advance(); return Make(MxTokKind::LParen, start, "(");
            case ')': Advance(); return Make(MxTokKind::RParen, start, ")");
            case '[': Advance(); return Make(MxTokKind::LBracket, start, "[");
            case ']': Advance(); return Make(MxTokKind::RBracket, start, "]");
            case '{': Advance(); return Make(MxTokKind::LBrace, start, "{");
            case '}': Advance(); return Make(MxTokKind::RBrace, start, "}");
            case ',': Advance(); return Make(MxTokKind::Comma, start, ",");
            case ';': Advance(); return Make(MxTokKind::Semi, start, ";");
            case '$': Advance(); return Make(MxTokKind::Dollar, start, "$");
            case '\'':
                Advance();
                if (Peek() == '\'') {
                    Advance();
                    return Make(MxTokKind::QuoteEval, start, "''");
                }
                return Make(MxTokKind::Quote, start, "'");
            default: break;
        }

        // `&&` tags the statement that follows with a name. It is not an
        // operator (Maxima gives it a left binding power of -1, the same
        // as a terminator); mread-raw handles it itself, and so does
        // ParseStatement below.
        if (c == '&' && Peek(1) == '&') {
            Advance();
            Advance();
            return Make(MxTokKind::Label, start, "&&");
        }
        // What this file declared for itself beats the built-in table, and
        // longest match wins within each: `@@` is one operator where
        // something said so, and two `@`s otherwise.
        if (user_ops_ != nullptr) {
            const MxUserOperator *best = nullptr;
            for (const MxUserOperator &op : *user_ops_) {
                if (op.text.empty() || MxIsNameStart(op.text[0])) continue;
                if (!MatchesAhead(op.text)) continue;
                if (best == nullptr || op.text.size() > best->text.size()) best = &op;
            }
            if (best != nullptr) {
                const std::string text = best->text;
                for (size_t i = 0; i < text.size(); i++) Advance();
                return Make(MxTokKind::Op, start, text);
            }
        }
        // Operators, longest spelling first.
        for (const MxOperatorInfo &op : kOperators) {
            const std::string text = op.text;
            if (!MxIsNameStart(text[0]) && MatchesAhead(text)) {
                for (size_t i = 0; i < text.size(); i++) Advance();
                return Make(MxTokKind::Op, start, text);
            }
        }

        // Anything else is a character Maxima's reader would not accept
        // here. Emit it as a one-character operator token so the parser
        // can report it in context rather than the lexer guessing.
        Advance();
        return Make(MxTokKind::Op, start, std::string(1, c));
    }

    bool MatchesAhead(const std::string &text) {
        for (size_t i = 0; i < text.size(); i++) {
            if (Peek(static_cast<int>(i)) != text[i]) return false;
        }
        return true;
    }

    MxToken ReadString(const MxRange &start) {
        Advance();  // the opening quote
        std::string value;
        bool closed = false;
        while (!AtEnd()) {
            const char c = Peek();
            if (c == '\\') {
                Advance();
                if (AtEnd()) break;
                // Maxima's `scan-string` takes the next character
                // literally, whatever it is -- there is no \n escape,
                // and a backslash before a newline continues the string.
                value += Peek() == '\n' ? '\n' : Peek();
                Advance();
                continue;
            }
            if (c == '"') {
                Advance();
                closed = true;
                break;
            }
            value += (c == '\n' ? '\n' : c);
            Advance();
        }
        MxToken t = Make(MxTokKind::Str, start, value);
        if (!closed) {
            t.unterminated = true;
            Error(t.range, "unterminated-string", "This string is never closed (a `\"` is missing).");
        }
        return t;
    }

    // `?name` is a symbol in the host Lisp package, `?:name` a Lisp
    // keyword, `?"..."` a symbol whose name is spelled out. None of them
    // is a Maxima name, so they are kept whole and never resolved.
    MxToken ReadLispName(const MxRange &start) {
        std::string text = "?";
        Advance();
        if (Peek() == '"') {
            const MxToken str = ReadString(Here());
            return Make(MxTokKind::Lisp, start, "?\"" + str.text + "\"");
        }
        if (Peek() == ':') {
            text += ':';
            Advance();
        }
        // Maxima's `scan-token` reads at least one character after the
        // `?`, whatever it is, and only then starts testing: `?princ('?
        // in\ )` in the integral-equations package really does name the
        // Lisp symbol `| in |`, space and all.
        bool first = true;
        while (!AtEnd()) {
            const char c = Peek();
            if (!first && !(MxIsNameChar(c) || c == '-' || c == '\\')) break;
            if (c == '\n') break;
            if (c == '\\') {
                Advance();
                if (AtEnd()) break;
            }
            text += Peek();
            Advance();
            first = false;
        }
        return Make(MxTokKind::Lisp, start, text);
    }

    MxToken ReadNumber(const MxRange &start) {
        std::string text;
        while (std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
            text += Peek();
            Advance();
        }
        if (Peek() == '.') {
            text += '.';
            Advance();
            while (std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                text += Peek();
                Advance();
            }
        }
        // Maxima's exponent markers: `e`/`d`/`s`/`l`/`f` are floats of
        // various host precisions and `b` is a bigfloat. All of them take
        // an optional sign and at least one digit.
        const char marker = Peek();
        const bool is_marker = marker == 'e' || marker == 'E' || marker == 'b' || marker == 'B' || marker == 'd' ||
                               marker == 'D' || marker == 's' || marker == 'S' || marker == 'l' || marker == 'L' ||
                               marker == 'f' || marker == 'F';
        if (is_marker) {
            const int sign_at = (Peek(1) == '+' || Peek(1) == '-') ? 1 : 0;
            if (std::isdigit(static_cast<unsigned char>(Peek(1 + sign_at))) != 0) {
                text += marker;
                Advance();
                if (sign_at != 0) {
                    text += Peek();
                    Advance();
                }
                while (std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                    text += Peek();
                    Advance();
                }
            }
        }
        return Make(MxTokKind::Num, start, text);
    }

    MxToken ReadName(const MxRange &start) {
        std::string text;
        while (!AtEnd()) {
            const char c = Peek();
            if (c == '\\') {
                // A backslash makes the next character part of the name,
                // which is how a Maxima name can contain a space or a
                // `+`. The escape itself is not part of the name -- and
                // a backslash at end of line is a continuation, so
                // `ab\` + `cd` is the single name `abcd`.
                if (AtLineEnd(1)) {
                    Advance();
                    Advance();
                    continue;
                }
                Advance();
                if (AtEnd()) break;
                text += Peek();
                Advance();
                continue;
            }
            if (!MxIsNameChar(c)) break;
            text += c;
            Advance();
        }
        for (const Keyword &kw : kKeywords) {
            if (text == kw.text) return Make(kw.kind, start, text);
        }
        if (text == "and" || text == "or" || text == "not") return Make(MxTokKind::Op, start, text);
        return Make(MxTokKind::Ident, start, text);
    }
};

}  // namespace

std::vector<MxToken> MxTokenize(const std::vector<std::string> &lines, std::vector<MxComment> *comments_out,
                                std::vector<MxParseError> *errors_out,
                                const std::vector<MxUserOperator> *user_ops) {
    Lexer lexer(lines, comments_out, errors_out, user_ops);
    return lexer.Run();
}

// --- Operator declarations --------------------------------------------

// Every operator declaration in the Maxima distribution's own share
// tree, with the binding powers each declaration gives it. Small and
// fixed on purpose: these are the spellings real files use without
// declaring them, because the package they load did it for them.
const std::vector<MxUserOperator> &MxPackageOperators() {
    static const std::vector<MxUserOperator> kTable = [] {
        struct Row {
            const char *text;
            const char *package;
            int lbp;
            int rbp;
            int prefix_bp;
            bool postfix;
        };
        static const Row kRows[] = {
            {"~", "vect", 134, 133, 0, false},       {"grad", "vect", 0, 0, 142, false},
            {"div", "vect", 0, 0, 142, false},       {"curl", "vect", 0, 0, 142, false},
            {"laplacian", "vect", 0, 0, 142, false}, {"cross", "vector", 112, 112, 0, false},
            {"dotdel", "vector", 108, 108, 0, false}, {"`", "ezunits", 118, 118, 0, false},
            {"``", "ezunits", 117, 117, 0, false},   {"=>", "defm", 180, 20, 0, false},
            {"=>", "submac", 180, 20, 0, false},     {"@", "diff_form", 200, 201, 0, false},
            {"&", "diff_form", 180, 180, 0, false},  {"|", "diff_form", 180, 180, 0, false},
            {"~", "ex_calc", 180, 180, 0, false},    {"|", "ex_calc", 180, 180, 0, false},
            {"//", "coma", 115, 115, 0, false},      {"/_", "coma", 150, 150, 0, false},
            {"//_", "coma", 20, 0, 0, true},
        };
        std::vector<MxUserOperator> out;
        for (const Row &r : kRows) {
            MxUserOperator op;
            op.text = r.text;
            op.package = r.package;
            op.lbp = r.lbp;
            op.rbp = r.rbp;
            op.prefix_bp = r.prefix_bp;
            op.postfix = r.postfix;
            out.push_back(std::move(op));
        }
        return out;
    }();
    return kTable;
}

std::vector<std::string> MxScanLoadedPackages(const std::vector<MxToken> &tokens) {
    std::vector<std::string> out;
    for (size_t i = 0; i + 2 < tokens.size(); i++) {
        if (tokens[i].kind != MxTokKind::Ident) continue;
        const std::string &fn = tokens[i].text;
        if (fn != "load" && fn != "batch" && fn != "batchload" && fn != "loadfile" && fn != "load_pathname") continue;
        if (tokens[i + 1].kind != MxTokKind::LParen) continue;
        const MxToken &arg = tokens[i + 2];
        if (arg.kind != MxTokKind::Str && arg.kind != MxTokKind::Ident) continue;
        // `load("share/vector/vect.mac")` and `load(vect)` name the same
        // package; what matters is the bare stem.
        std::string name = arg.text;
        const size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
        if (!name.empty()) out.push_back(name);
    }
    return out;
}

std::vector<MxUserOperator> MxScanOperatorDeclarations(const std::vector<MxToken> &tokens) {
    // Maxima's own defaults when a declaration gives no binding powers
    // (see `$infix`/`$prefix`/`$postfix`/`$nary` in src/nparse.lisp):
    // 180 in every direction, which puts a user operator between `:`
    // (180) and `!` (160).
    constexpr int kDefault = 180;
    std::vector<MxUserOperator> out;
    for (size_t i = 0; i + 2 < tokens.size(); i++) {
        if (tokens[i].kind != MxTokKind::Ident) continue;
        const std::string &fn = tokens[i].text;
        const bool infix = fn == "infix";
        const bool nary = fn == "nary";
        const bool prefix = fn == "prefix";
        const bool postfix = fn == "postfix";
        const bool nofix = fn == "nofix";
        const bool matchfix = fn == "matchfix";
        if (!(infix || nary || prefix || postfix || nofix || matchfix)) continue;
        if (tokens[i + 1].kind != MxTokKind::LParen) continue;
        if (tokens[i + 2].kind != MxTokKind::Str || tokens[i + 2].text.empty()) continue;
        MxUserOperator op;
        op.text = tokens[i + 2].text;
        op.range = tokens[i].range;
        op.range.end_line = tokens[i + 2].range.end_line;
        op.range.end_col = tokens[i + 2].range.end_col;
        // The numeric arguments a declaration may carry: `infix("x", lbp,
        // rbp)`, `prefix("x", rbp)`, `postfix("x", lbp)`. Anything that is
        // not a plain number (a `pos` symbol, a computed value) leaves the
        // default in place.
        auto number_at = [&tokens](size_t index, int fallback) {
            if (index + 1 >= tokens.size()) return fallback;
            if (tokens[index].kind != MxTokKind::Comma) return fallback;
            if (tokens[index + 1].kind != MxTokKind::Num) return fallback;
            return std::atoi(tokens[index + 1].text.c_str());
        };
        if (infix || nary) {
            op.lbp = number_at(i + 3, kDefault);
            op.rbp = number_at(i + 5, op.lbp);
        } else if (prefix) {
            op.prefix_bp = number_at(i + 3, kDefault);
        } else if (postfix) {
            op.lbp = number_at(i + 3, kDefault);
            op.postfix = true;
        } else if (nofix) {
            // A nofix operator stands alone as a whole expression, so it
            // has no binding power at all -- it is recorded only so the
            // name is known.
        } else if (matchfix) {
            if (tokens[i + 3].kind == MxTokKind::Comma && tokens[i + 4].kind == MxTokKind::Str) {
                op.close = tokens[i + 4].text;
            }
        }
        out.push_back(std::move(op));
    }
    return out;
}

// --- Parser -----------------------------------------------------------

namespace {

// Thrown past the expression parser to the statement loop, which reports
// the error and resynchronizes at the next terminator. Every throw site
// has already recorded the diagnostic, so this carries nothing.
struct ParseAbort {};

class Parser {
  public:
    Parser(std::vector<MxToken> tokens, MxParseResult *out) : toks_(std::move(tokens)), out_(out) {}

    void Run() {
        while (!AtEnd()) {
            const size_t before = pos_;
            ParseStatement();
            if (pos_ == before) pos_++;  // never spin on a token nothing consumed
        }
    }

  private:
    std::vector<MxToken> toks_;
    MxParseResult *out_;
    size_t pos_ = 0;
    int depth_ = 0;  // expression nesting, capped at kMxMaxNesting
    // Spellings already reported as undeclared, so a file that uses `~`
    // forty times says so once.
    std::vector<std::string> reported_ops_;

    /** @brief Looks up an operator, the file's own declarations first. */
    const MxOperatorInfo *Op(const std::string &text) {
        for (const MxUserOperator &u : out_->user_operators) {
            if (u.text != text) continue;
            scratch_.text = u.text.c_str();
            scratch_.lbp = u.lbp;
            scratch_.rbp = u.rbp != 0 ? u.rbp : u.lbp;
            scratch_.prefix_bp = u.prefix_bp;
            scratch_.postfix = u.postfix;
            return &scratch_;
        }
        return FindOp(text);
    }
    MxOperatorInfo scratch_ = {"", 0, 0, 0, false};

    // An operator-shaped token that nothing declares. Maxima would stop
    // here, and so would this reader -- except that the spelling is
    // almost always one a package the file loads declares for it
    // (`~` in vect, `` ` `` in ezunits), and refusing to read the rest of
    // such a file is the false-positive storm r_lsp.h warns about. So it
    // is read as an ordinary infix operator at the default binding power
    // and reported once, at a severity the analysis half drops to
    // Information.
    bool RecoverAsOperator(const MxToken &t) {
        if (t.kind != MxTokKind::Op) return false;
        if (t.text.empty() || MxIsNameStart(t.text[0])) return false;
        for (const std::string &seen : reported_ops_) {
            if (seen == t.text) return true;
        }
        reported_ops_.push_back(t.text);
        Report(t.range, "undeclared-operator",
               "`" + t.text +
                   "` is not one of Maxima's operators. A package this file loads may declare it with infix(\"" +
                   t.text + "\"); without one, Maxima stops here.");
        return true;
    }

    bool AtEnd() const { return Cur().kind == MxTokKind::End; }
    const MxToken &Cur() const { return toks_[std::min(pos_, toks_.size() - 1)]; }
    const MxToken &Ahead(size_t n) const { return toks_[std::min(pos_ + n, toks_.size() - 1)]; }
    void Bump() {
        if (pos_ + 1 < toks_.size()) pos_++;
    }

    int Add(MxNode node) {
        out_->nodes.push_back(std::move(node));
        return static_cast<int>(out_->nodes.size()) - 1;
    }

    MxRange Span(const MxRange &from, const MxRange &to) const {
        MxRange r = from;
        r.end_line = to.end_line;
        r.end_col = to.end_col;
        return r;
    }

    void Report(const MxRange &range, const char *code, const std::string &message) {
        // One error per statement is enough: after the first, the parser
        // is guessing, and a cascade of derived complaints buries the
        // one the author can act on.
        if (!out_->errors.empty() && out_->errors.back().range.line == range.line &&
            out_->errors.back().range.col == range.col) {
            return;
        }
        MxParseError e;
        e.range = range;
        e.code = code;
        e.message = message;
        out_->errors.push_back(e);
    }

    [[noreturn]] void Fail(const MxRange &range, const char *code, const std::string &message) {
        Report(range, code, message);
        throw ParseAbort{};
    }

    /** @brief Reports whether a token cannot begin an expression, so a prefix operator would have no operand. */
    static bool StartsNothing(const MxToken &t) {
        switch (t.kind) {
            case MxTokKind::End:
            case MxTokKind::Semi:
            case MxTokKind::Dollar:
            case MxTokKind::Comma:
            case MxTokKind::RParen:
            case MxTokKind::RBracket:
            case MxTokKind::RBrace:
            case MxTokKind::Label:
            case MxTokKind::Then:
            case MxTokKind::Else:
            case MxTokKind::Elseif:
            case MxTokKind::Do: return true;
            default: return false;
        }
    }

    /** @brief Describes the current token the way a message can name it. */
    std::string Describe(const MxToken &t) const {
        switch (t.kind) {
            case MxTokKind::End: return "the end of the file";
            case MxTokKind::Str: return "a string";
            case MxTokKind::Num: return "the number `" + t.text + "`";
            default: break;
        }
        return "`" + t.text + "`";
    }

    // --- Statements ---------------------------------------------------

    void ParseStatement() {
        // `:lisp <form>` drops into the host Lisp for the rest of the
        // line. Reading it as Maxima would be wrong in every case, so the
        // line is kept whole and skipped.
        if (Cur().kind == MxTokKind::Op && Cur().text == ":" && Ahead(1).kind == MxTokKind::Ident &&
            (Ahead(1).text == "lisp" || Ahead(1).text == "lisp_quiet")) {
            const MxRange start = Cur().range;
            const int on_line = start.line;
            MxRange end = Ahead(1).range;
            Bump();
            Bump();
            while (!AtEnd() && Cur().range.line == on_line) {
                end = Cur().range;
                Bump();
            }
            MxNode node;
            node.kind = MxNodeKind::LispEsc;
            node.range = Span(start, end);
            node.text = ":lisp";
            MxStatement st;
            st.node = Add(std::move(node));
            st.terminated = true;
            st.range = Span(start, end);
            out_->statements.push_back(st);
            return;
        }

        const MxRange start = Cur().range;
        int node = -1;
        bool aborted = false;
        depth_ = 0;  // a ParseAbort unwinds past the decrements below
        try {
            node = ParseExpr(0);
            // `name && expr;` tags the statement with a name -- Maxima's
            // own mread-raw reads the tag, then goes back for the
            // expression it labels. The simplification package writes
            // every one of its definitions that way.
            while (Cur().kind == MxTokKind::Label) {
                const MxToken amp = Cur();
                if (out_->at(node).kind != MxNodeKind::Ident) {
                    Report(out_->at(node).range, "bad-label",
                           "A `&&` tag has to be a plain name; this is an expression.");
                }
                Bump();
                if (Cur().kind == MxTokKind::Semi || Cur().kind == MxTokKind::Dollar || AtEnd()) break;
                const int labelled = ParseExpr(0);
                MxNode n;
                n.kind = MxNodeKind::Label;
                n.text = out_->at(node).text;
                n.range = Span(out_->at(node).range, out_->at(labelled).range);
                n.kids = {node, labelled};
                node = Add(std::move(n));
                (void)amp;
            }
        } catch (const ParseAbort &) {
            aborted = true;
            SkipToTerminator();
        }

        MxStatement st;
        st.node = node;
        MxRange end = node >= 0 ? out_->at(node).range : start;
        if (Cur().kind == MxTokKind::Semi || Cur().kind == MxTokKind::Dollar) {
            st.terminated = true;
            st.terminator = Cur().kind == MxTokKind::Semi ? ';' : '$';
            end = Cur().range;
            Bump();
        } else if (!aborted && !AtEnd()) {
            // The statement ended somewhere the grammar cannot continue
            // from. Maxima says "<token> is not an infix operator" here,
            // and the cause is almost always the terminator that is not
            // there.
            Report(Cur().range, "syntax-error",
                   "Expected `;` or `$` to end this statement, but found " + Describe(Cur()) + ".");
            SkipToTerminator();
            if (Cur().kind == MxTokKind::Semi || Cur().kind == MxTokKind::Dollar) {
                st.terminated = true;
                st.terminator = Cur().kind == MxTokKind::Semi ? ';' : '$';
                end = Cur().range;
                Bump();
            }
        } else if (!aborted && AtEnd() && node >= 0) {
            Report(end, "missing-terminator",
                   "The file ends without a `;` or `$` after this expression; Maxima reads past the end of it.");
        }
        st.range = Span(start, end);
        if (st.node >= 0 || aborted) out_->statements.push_back(st);
    }

    void SkipToTerminator() {
        int depth = 0;
        while (!AtEnd()) {
            const MxTokKind k = Cur().kind;
            if (k == MxTokKind::LParen || k == MxTokKind::LBracket || k == MxTokKind::LBrace) depth++;
            if (k == MxTokKind::RParen || k == MxTokKind::RBracket || k == MxTokKind::RBrace) depth--;
            if (depth <= 0 && (k == MxTokKind::Semi || k == MxTokKind::Dollar)) return;
            Bump();
        }
    }

    // --- Expressions --------------------------------------------------

    // Pratt: read a prefix form, then keep absorbing infix/postfix
    // operators whose left binding power beats `bp`. The powers are
    // Maxima's own -- see maxima_ast.h.
    int ParseExpr(int bp) {
        if (depth_ >= kMxMaxNesting) {
            Fail(Cur().range, "too-deep",
                 "This expression nests more than " + std::to_string(kMxMaxNesting) +
                     " levels deep; Maxima's own reader would not manage it either.");
        }
        depth_++;
        const int result = ParseExprInner(bp);
        depth_--;
        return result;
    }

    int ParseExprInner(int bp) {
        int left = ParsePrefix();
        while (true) {
            const MxToken &t = Cur();
            if (t.kind == MxTokKind::Comma) {
                if (bp >= kCommaLbp) return left;
                // A comma at statement level is Maxima's `ev` shorthand:
                // `integrate(x, x), x = 2`.
                const MxToken op = t;
                Bump();
                const int right = ParseExpr(kCommaLbp);
                MxNode node;
                node.kind = MxNodeKind::Binary;
                node.text = ",";
                node.range = Span(out_->at(left).range, out_->at(right).range);
                node.kids = {left, right};
                left = Add(std::move(node));
                (void)op;
                continue;
            }
            if (t.kind == MxTokKind::LParen) {
                if (bp >= 200) return left;
                left = ParseCall(left);
                continue;
            }
            if (t.kind == MxTokKind::LBracket) {
                if (bp >= 200) return left;
                left = ParseSubscript(left);
                continue;
            }
            if (t.kind == MxTokKind::Ident) {
                // A word the file declared as an infix operator:
                // `a cross b` after `infix("cross")`. An undeclared word
                // is never treated this way -- that is what a missing
                // terminator looks like, and silently reading it as an
                // operator would hide the error.
                const MxOperatorInfo *word = Op(t.text);
                if (word == nullptr || word->lbp == 0 || word->lbp <= bp) return left;
                const MxToken tok = t;
                Bump();
                if (word->postfix) {
                    MxNode node;
                    node.kind = MxNodeKind::Postfix;
                    node.text = tok.text;
                    node.range = Span(out_->at(left).range, tok.range);
                    node.kids = {left};
                    left = Add(std::move(node));
                    continue;
                }
                const int rhs = ParseExpr(word->rbp);
                MxNode node;
                node.kind = MxNodeKind::Binary;
                node.text = tok.text;
                node.range = Span(out_->at(left).range, out_->at(rhs).range);
                node.kids = {left, rhs};
                left = Add(std::move(node));
                continue;
            }
            if (t.kind != MxTokKind::Op) return left;
            const MxOperatorInfo *op = Op(t.text);
            if (op == nullptr || op->lbp == 0) {
                if (!RecoverAsOperator(t)) return left;
                if (kUserDefaultBp <= bp) return left;
                const MxToken unknown = t;
                Bump();
                const int rhs = ParseExpr(kUserDefaultBp);
                MxNode node;
                node.kind = MxNodeKind::Binary;
                node.text = unknown.text;
                node.range = Span(out_->at(left).range, out_->at(rhs).range);
                node.kids = {left, rhs};
                left = Add(std::move(node));
                continue;
            }
            if (op->lbp <= bp) return left;
            const MxToken tok = t;
            Bump();
            if (op->postfix) {
                MxNode node;
                node.kind = MxNodeKind::Postfix;
                node.text = tok.text;
                node.range = Span(out_->at(left).range, tok.range);
                node.kids = {left};
                left = Add(std::move(node));
                continue;
            }
            const int right = ParseExpr(op->rbp);
            MxNode node;
            node.kind = MxNodeKind::Binary;
            node.text = tok.text;
            node.range = Span(out_->at(left).range, out_->at(right).range);
            node.kids = {left, right};
            left = Add(std::move(node));
        }
    }

    int ParsePrefix() {
        const MxToken t = Cur();
        switch (t.kind) {
            case MxTokKind::Num: {
                Bump();
                MxNode n;
                n.kind = MxNodeKind::Num;
                n.text = t.text;
                n.range = t.range;
                return Add(std::move(n));
            }
            case MxTokKind::Str: {
                Bump();
                MxNode n;
                n.kind = MxNodeKind::Str;
                n.text = t.text;
                n.range = t.range;
                return Add(std::move(n));
            }
            case MxTokKind::Ident: {
                // `prefix("grad")` makes `grad x` a real expression, and
                // the vector-algebra package leans on that.
                const MxOperatorInfo *word = Op(t.text);
                if (word != nullptr && word->prefix_bp != 0 && !StartsNothing(Ahead(1))) {
                    Bump();
                    const int operand = ParseExpr(word->prefix_bp);
                    MxNode n;
                    n.kind = MxNodeKind::Unary;
                    n.text = t.text;
                    n.range = Span(t.range, out_->at(operand).range);
                    n.kids = {operand};
                    return Add(std::move(n));
                }
                Bump();
                MxNode n;
                n.kind = MxNodeKind::Ident;
                n.text = t.text;
                n.range = t.range;
                return Add(std::move(n));
            }
            case MxTokKind::Lisp: {
                Bump();
                MxNode n;
                n.kind = MxNodeKind::Lisp;
                n.text = t.text;
                n.range = t.range;
                return Add(std::move(n));
            }
            case MxTokKind::Quote:
            case MxTokKind::QuoteEval: {
                Bump();
                const int operand = ParseExpr(190);
                MxNode n;
                n.kind = MxNodeKind::Quote;
                n.text = t.text;
                n.range = Span(t.range, out_->at(operand).range);
                n.kids = {operand};
                return Add(std::move(n));
            }
            case MxTokKind::LParen: return ParseParen();
            case MxTokKind::LBracket: return ParseBracketed(MxNodeKind::List, MxTokKind::RBracket, "]");
            case MxTokKind::LBrace: return ParseBracketed(MxNodeKind::Set, MxTokKind::RBrace, "}");
            case MxTokKind::If: return ParseIf();
            case MxTokKind::For:
            case MxTokKind::From:
            case MxTokKind::Step:
            case MxTokKind::Next:
            case MxTokKind::Thru:
            case MxTokKind::While:
            case MxTokKind::Unless:
            case MxTokKind::Do: return ParseDo();
            case MxTokKind::Op: {
                const MxOperatorInfo *op = Op(t.text);
                if (op != nullptr && op->prefix_bp != 0) {
                    Bump();
                    const int operand = ParseExpr(op->prefix_bp);
                    MxNode n;
                    n.kind = MxNodeKind::Unary;
                    n.text = t.text;
                    n.range = Span(t.range, out_->at(operand).range);
                    n.kids = {operand};
                    return Add(std::move(n));
                }
                if (op != nullptr) {
                    Fail(t.range, "syntax-error",
                         "`" + t.text + "` needs a value on its left, but the expression starts here.");
                }
                Fail(t.range, "syntax-error", "`" + t.text + "` is not something Maxima can read here.");
            }
            case MxTokKind::Then:
            case MxTokKind::Else:
            case MxTokKind::Elseif:
                Fail(t.range, "syntax-error", "`" + t.text + "` appears with no `if` for it to belong to.");
            case MxTokKind::RParen:
            case MxTokKind::RBracket:
            case MxTokKind::RBrace:
                Fail(t.range, "unmatched-bracket", "`" + t.text + "` closes something that was never opened.");
            case MxTokKind::Comma:
                Fail(t.range, "syntax-error", "An expression is missing before this comma.");
            case MxTokKind::Label:
                Fail(t.range, "bad-label", "A `&&` tag needs a name in front of it.");
            case MxTokKind::Semi:
            case MxTokKind::Dollar:
                Fail(t.range, "empty-statement",
                     "There is no expression before this `" + t.text + "`; Maxima reads an empty statement as an error.");
            case MxTokKind::End: Fail(t.range, "syntax-error", "The file ends in the middle of an expression.");
        }
        Fail(t.range, "syntax-error", "Unreadable input.");
    }

    // `( a )` is just `a`; `( a, b )` is Maxima's mprogn -- a sequence
    // whose value is the last expression.
    int ParseParen() {
        const MxToken open = Cur();
        Bump();
        MxNode n;
        n.kind = MxNodeKind::Paren;
        if (Cur().kind == MxTokKind::RParen) {
            Fail(Span(open.range, Cur().range), "empty-parens", "`()` is not an expression Maxima can read.");
        }
        while (true) {
            n.kids.push_back(ParseExpr(kCommaLbp));
            if (Cur().kind == MxTokKind::Comma) {
                Bump();
                continue;
            }
            break;
        }
        if (Cur().kind != MxTokKind::RParen) {
            Fail(Cur().range, "unclosed-paren",
                 "Expected `)` to close the `(` opened on line " + std::to_string(open.range.line + 1) + ", but found " +
                     Describe(Cur()) + ".");
        }
        n.range = Span(open.range, Cur().range);
        Bump();
        return Add(std::move(n));
    }

    int ParseBracketed(MxNodeKind kind, MxTokKind closer, const char *closer_text) {
        const MxToken open = Cur();
        Bump();
        MxNode n;
        n.kind = kind;
        if (Cur().kind != closer) {
            while (true) {
                n.kids.push_back(ParseExpr(kCommaLbp));
                if (Cur().kind == MxTokKind::Comma) {
                    Bump();
                    continue;
                }
                break;
            }
        }
        if (Cur().kind != closer) {
            Fail(Cur().range, "unclosed-bracket",
                 std::string("Expected `") + closer_text + "` to close the `" + open.text + "` opened on line " +
                     std::to_string(open.range.line + 1) + ", but found " + Describe(Cur()) + ".");
        }
        n.range = Span(open.range, Cur().range);
        Bump();
        return Add(std::move(n));
    }

    int ParseCall(int callee) {
        const MxToken open = Cur();
        // Maxima's own `def-led |$(|` refuses `number(...)` outright, and
        // the reason to copy that rather than build a call node is that
        // `8(3*x)` is what a dropped `*` or `+` looks like -- the one
        // shape juxtaposition can still be caught at.
        if (out_->at(callee).kind == MxNodeKind::Num) {
            Fail(open.range, "syntax-error",
                 "A number cannot be called. An operator is missing between `" + out_->at(callee).text +
                     "` and this `(`.");
        }
        Bump();
        MxNode n;
        n.kind = MxNodeKind::Call;
        n.kids = {callee};
        if (Cur().kind != MxTokKind::RParen) {
            while (true) {
                MxArg arg;
                arg.value = ParseExpr(kCommaLbp);
                arg.range = out_->at(arg.value).range;
                n.args.push_back(arg);
                if (Cur().kind == MxTokKind::Comma) {
                    Bump();
                    continue;
                }
                break;
            }
        }
        if (Cur().kind != MxTokKind::RParen) {
            Fail(Cur().range, "unclosed-paren",
                 "Expected `)` to close the argument list opened on line " + std::to_string(open.range.line + 1) +
                     ", but found " + Describe(Cur()) + ".");
        }
        n.range = Span(out_->at(callee).range, Cur().range);
        Bump();
        return Add(std::move(n));
    }

    int ParseSubscript(int object) {
        const MxToken open = Cur();
        if (out_->at(object).kind == MxNodeKind::Num) {
            Fail(open.range, "syntax-error",
                 "A number cannot be subscripted. An operator is missing between `" + out_->at(object).text +
                     "` and this `[`.");
        }
        Bump();
        MxNode n;
        n.kind = MxNodeKind::Index;
        n.kids = {object};
        if (Cur().kind == MxTokKind::RBracket) {
            Fail(Span(open.range, Cur().range), "empty-subscript", "A subscript needs at least one index.");
        }
        while (true) {
            MxArg arg;
            arg.value = ParseExpr(kCommaLbp);
            arg.range = out_->at(arg.value).range;
            n.args.push_back(arg);
            if (Cur().kind == MxTokKind::Comma) {
                Bump();
                continue;
            }
            break;
        }
        if (Cur().kind != MxTokKind::RBracket) {
            Fail(Cur().range, "unclosed-bracket",
                 "Expected `]` to close the subscript opened on line " + std::to_string(open.range.line + 1) +
                     ", but found " + Describe(Cur()) + ".");
        }
        n.range = Span(out_->at(object).range, Cur().range);
        Bump();
        return Add(std::move(n));
    }

    // if C then A elseif D then B else E -- the branches are kept flat,
    // one "cond"/"then" pair per level, so a walker does not have to
    // recurse through a chain it did not write.
    int ParseIf() {
        const MxToken open = Cur();
        Bump();
        MxNode n;
        n.kind = MxNodeKind::If;
        MxRange end = open.range;
        while (true) {
            MxArg cond;
            cond.name = "cond";
            cond.value = ParseExpr(45);
            cond.range = out_->at(cond.value).range;
            n.args.push_back(cond);
            if (Cur().kind != MxTokKind::Then) {
                Fail(Cur().range, "missing-then",
                     "Expected `then` after this condition, but found " + Describe(Cur()) + ".");
            }
            Bump();
            MxArg then_arg;
            then_arg.name = "then";
            then_arg.value = ParseExpr(25);
            then_arg.range = out_->at(then_arg.value).range;
            end = then_arg.range;
            n.args.push_back(then_arg);
            if (Cur().kind == MxTokKind::Elseif) {
                Bump();
                continue;
            }
            if (Cur().kind == MxTokKind::Else) {
                Bump();
                MxArg else_arg;
                else_arg.name = "else";
                else_arg.value = ParseExpr(25);
                else_arg.range = out_->at(else_arg.value).range;
                end = else_arg.range;
                n.args.push_back(else_arg);
            }
            break;
        }
        n.range = Span(open.range, end);
        return Add(std::move(n));
    }

    // Every loop is the same construct: a bag of clauses in any order,
    // closed by `do <body>`. `for i: 1` is `for i from 1` -- inside a
    // loop header Maxima reads `:` as `from`, which is why an assignment
    // there is not one.
    int ParseDo() {
        const MxToken open = Cur();
        MxNode n;
        n.kind = MxNodeKind::Do;
        MxRange end = open.range;
        while (true) {
            MxToken clause = Cur();
            std::string name;
            switch (clause.kind) {
                case MxTokKind::For: name = "for"; break;
                case MxTokKind::From: name = "from"; break;
                case MxTokKind::Step: name = "step"; break;
                case MxTokKind::Next: name = "next"; break;
                case MxTokKind::Thru: name = "thru"; break;
                case MxTokKind::While: name = "while"; break;
                case MxTokKind::Unless: name = "unless"; break;
                case MxTokKind::Do: name = "do"; break;
                case MxTokKind::Ident:
                    // `in` is an ordinary name everywhere except right
                    // here, which is why the lexer does not reserve it.
                    if (clause.text == "in") {
                        name = "in";
                        break;
                    }
                    [[fallthrough]];
                case MxTokKind::Op:
                    if (clause.kind == MxTokKind::Op && clause.text == ":") {
                        name = "from";
                        break;
                    }
                    [[fallthrough]];
                default:
                    Fail(clause.range, "missing-do",
                         "Expected another loop clause or `do`, but found " + Describe(clause) + ".");
            }
            Bump();
            MxArg arg;
            arg.name = name;
            arg.range = clause.range;
            // Maxima's own right binding powers per clause: `for` takes a
            // bare name (200), the ranges take expressions (95), the
            // conditions take clauses (45) and the body takes everything
            // down to the terminator (25).
            int rbp = 25;
            if (name == "for") rbp = 200;
            else if (name == "from" || name == "in" || name == "step" || name == "thru") rbp = 95;
            else if (name == "while" || name == "unless" || name == "next") rbp = 45;
            arg.value = ParseExpr(rbp);
            end = out_->at(arg.value).range;
            n.args.push_back(arg);
            if (name == "do") break;
        }
        n.range = Span(open.range, end);
        return Add(std::move(n));
    }
};

}  // namespace

MxParseResult MxParse(const std::vector<std::string> &lines) {
    MxParseResult out;
    // Two passes, because a declaration changes the lexing of everything
    // after it: the first only looks for `infix("@@")` and friends, whose
    // spelling is a string literal and therefore survives being lexed
    // without knowing about it; the second reads the file for real with
    // those operators in the table.
    const std::vector<MxToken> probe = MxTokenize(lines, nullptr, nullptr, nullptr);
    out.user_operators = MxScanOperatorDeclarations(probe);
    // A loaded package's operators join the file's own, with the same
    // standing: `load("vect.mac")` is a declaration of `~`, just written
    // somewhere else.
    const std::vector<std::string> packages = MxScanLoadedPackages(probe);
    for (const MxUserOperator &op : MxPackageOperators()) {
        bool loaded = false;
        for (const std::string &pkg : packages) {
            if (pkg == op.package) loaded = true;
        }
        if (loaded) out.user_operators.push_back(op);
    }
    std::vector<MxToken> tokens =
        MxTokenize(lines, &out.comments, &out.errors, out.user_operators.empty() ? nullptr : &out.user_operators);
    Parser parser(std::move(tokens), &out);
    parser.Run();
    return out;
}
