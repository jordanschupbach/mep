#include "python_format.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace pyfmt {
namespace {

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------

// How deeply brackets (and nested f-string replacement fields) may nest before
// the tokenizer refuses the file. This is not a style limit, it is a safety
// one: `gf` runs this formatter *inside the editor process*, and both the
// splitter (EmitExpr <-> RightHandSplit) and the f-string scanner recurse once
// per level, so pathological input -- 20k nested parens, which really did
// segfault before this cap -- would take mep down with it rather than merely
// failing to format. Refusing above a depth no real source reaches turns that
// crash into an ordinary error message.
const int kMaxNest = 1000;

enum class Kind { Name, Keyword, Number, String, Op, Comment };

struct Token {
    Kind kind = Kind::Name;
    std::string text;
    int line = 0;  // 1-based source line the token started on

    // Filled in by the render pass, not the tokenizer: "this +/-/~/*/** is a
    // prefix operator, so it binds tight to what follows". Kept on the token
    // because both the renderer and the splitter need it and recomputing it
    // requires the left context.
    bool unary = false;

    // Set only on the flattened stream the verifier compares, marking the
    // first token of each logical line. The soft-keyword rule below needs it.
    bool stmt_start = false;
};

// `match` and `case` are soft keywords: they are ordinary identifiers unless
// they start a match statement. That makes `case (A | B):` look exactly like a
// call to a function named `case`, and treating it as one is not a cosmetic
// mistake -- an exploded call gets a trailing comma, and `case (A | B,):` is a
// *sequence* pattern rather than an or-pattern, which silently changes what
// the code matches. So a bracket opening directly after a statement-initial
// `match`/`case` is never treated as applied to a value. The cost is that a
// bare `match(x)` call statement does not gain a trailing comma when it is
// exploded; the benefit is that the formatter and its verifier agree exactly,
// and neither can be talked into rewriting a pattern.
bool IsSoftKeywordHeadName(const std::string &s) {
    return s == "match" || s == "case";
}

bool IsIdentStart(char c) {
    // Any byte >= 0x80 is let through so UTF-8 identifiers (PEP 3131) survive
    // as single Name tokens rather than being chopped into operators.
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

bool IsIdentCont(char c) {
    return IsIdentStart(c) || std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool IsKeyword(std::string_view s) {
    // The hard keywords only. `match`/`case`/`type` are soft -- they are legal
    // identifiers -- so they stay Name tokens; see the header's note on why
    // match statements are left alone.
    static const char *const kKeywords[] = {
        "False", "None",   "True",  "and",    "as",       "assert", "async",
        "await", "break",  "class", "continue", "def",    "del",    "elif",
        "else",  "except", "finally", "for",  "from",     "global", "if",
        "import", "in",    "is",    "lambda", "nonlocal", "not",    "or",
        "pass",  "raise",  "return", "try",   "while",    "with",   "yield",
    };
    for (const char *k : kKeywords) {
        if (s == k) return true;
    }
    return false;
}

// `None`/`True`/`False`/`...` are keywords (or operators) that are *values*:
// an operator after one of them is binary, not unary. Every other keyword
// leaves the parser expecting an operand, so `return -1` / `yield -x` /
// `not -y` all see a unary minus.
bool IsValueKeyword(std::string_view s) {
    return s == "None" || s == "True" || s == "False";
}

bool IsOpener(std::string_view s) { return s == "(" || s == "[" || s == "{"; }
bool IsCloser(std::string_view s) { return s == ")" || s == "]" || s == "}"; }

char MatchingOpener(char closer) {
    if (closer == ')') return '(';
    if (closer == ']') return '[';
    return '{';
}

// A string literal's prefix is up to two letters from this set. Checked
// against the actual character *after* the letters being a quote, so a plain
// identifier like `rb_tree` is never mistaken for a prefixed string.
bool IsStringPrefixChar(char c) {
    switch (c) {
        case 'r': case 'R': case 'b': case 'B':
        case 'u': case 'U': case 'f': case 'F':
        // `t"..."` is a PEP 750 template string (3.14). Without this it
        // tokenizes as the name `t` followed by a string, and the renderer
        // puts a space between them -- which is a syntax error.
        case 't': case 'T':
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Logical lines
// ---------------------------------------------------------------------------

// One line of the file as the formatter thinks about it: either a statement
// (possibly spanning many physical lines via brackets or backslashes) or a
// comment sitting on a line of its own. Blank lines are not items -- they are
// counted into the next item's `blank_before`, because what the emitter
// decides is "how many blank lines go before this", not "where were they".
struct LogicalLine {
    int indent = 0;             // nesting level, not columns
    std::vector<Token> tokens;  // a lone Comment token for a comment line
    int blank_before = 0;       // blank source lines immediately above
    int first_line = 1;         // 1-based source line it starts on
    int comment_col = 0;        // source column, comment lines only

    bool IsComment() const {
        return tokens.size() == 1 && tokens[0].kind == Kind::Comment;
    }
};

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

class Tokenizer {
   public:
    explicit Tokenizer(std::string_view src) : src_(src) {}

    bool Run();

    std::vector<LogicalLine> &&lines() { return std::move(lines_); }
    const std::string &error() const { return error_; }
    int error_line() const { return error_line_; }

   private:
    bool Fail(const std::string &msg) {
        if (error_.empty()) {
            error_ = msg;
            error_line_ = line_;
        }
        return false;
    }

    bool AtEnd() const { return pos_ >= src_.size(); }
    char Cur() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }
    char At(size_t off) const {
        return pos_ + off < src_.size() ? src_[pos_ + off] : '\0';
    }

    // Consumes the indentation at the current physical line start and returns
    // its width in columns. Tabs advance to the next multiple of 8, which is
    // what CPython's tokenizer does; only the *comparison* between lines
    // matters here, since the output's indentation is regenerated from the
    // nesting level rather than copied.
    int ScanIndentColumns();

    bool ScanString(Token &out, int depth = 0);
    void ScanNumber(Token &out);
    bool ScanStatement(LogicalLine &line);

    std::string_view src_;
    size_t pos_ = 0;
    int line_ = 1;
    std::vector<int> indent_stack_{0};
    std::vector<LogicalLine> lines_;
    int pending_blanks_ = 0;
    std::string error_;
    int error_line_ = 0;
};

int Tokenizer::ScanIndentColumns() {
    int col = 0;
    while (!AtEnd()) {
        if (Cur() == ' ') {
            col += 1;
        } else if (Cur() == '\t') {
            col = (col / 8 + 1) * 8;
        } else if (Cur() == '\f') {
            // A form feed resets the column, again matching CPython. Rare, but
            // it appears in older stdlib-style sources as a page separator.
            col = 0;
        } else {
            break;
        }
        pos_++;
    }
    return col;
}

// Scans one string literal starting at its prefix (or quote). Returns false
// with an error set if it is unterminated, which is the single most important
// thing for this tokenizer to get right: a missed closing quote would make
// every following token garbage, and the formatter would happily reflow it.
//
// f-strings and t-strings are scanned per PEP 701, which is what makes
// `f"{d["k"]}"` and `f'{f'{x}'}'` -- legal since 3.12 -- come out as one token
// instead of derailing the whole file. Inside a `{...}` replacement field a
// quote starts a *nested* literal rather than ending this one, so the scanner
// tracks the field nesting and recurses for each nested literal; after a `:`
// at the field's own level the rest is a format spec, where quotes are
// literal text again (which is what keeps `f"{x:'>10}"` from running away,
// while `f"{x[1:2]}"`'s slice colon is correctly ignored because it is inside
// brackets).
bool Tokenizer::ScanString(Token &out, int depth) {
    if (depth > kMaxNest) return Fail("f-string nests too deeply");
    size_t start = pos_;
    int start_line = line_;
    while (IsStringPrefixChar(Cur())) pos_++;
    // Only interpolation matters to the scan: a backslash behaves the same in
    // raw and non-raw literals as far as *finding the end* goes (see below).
    bool interp = false;
    for (size_t i = start; i < pos_; i++) {
        char c = src_[i];
        if (c == 'f' || c == 'F' || c == 't' || c == 'T') interp = true;
    }
    char quote = Cur();
    bool triple = At(1) == quote && At(2) == quote;
    pos_ += triple ? size_t{3} : size_t{1};

    // One entry per open `{` replacement field: whether it has reached its
    // format spec, and how deep its expression is in ()/[] brackets.
    struct Field {
        bool spec = false;
        int bracket = 0;
    };
    std::vector<Field> fields;

    while (true) {
        if (AtEnd()) {
            line_ = start_line;
            return Fail("unterminated string literal");
        }
        char c = src_[pos_];
        if (c == '\n') {
            // A newline is only allowed inside a single-quoted literal when it
            // is inside a replacement field (PEP 701).
            if (!triple && fields.empty()) {
                line_ = start_line;
                return Fail("unterminated string literal");
            }
            line_++;
            pos_++;
            continue;
        }
        if (fields.empty()) {
            // Ordinary literal text. A backslash consumes the next byte only
            // when that byte is one whose meaning it actually changes: a
            // quote, another backslash, or a newline. This holds in raw
            // literals too -- even there a backslash stops the next character
            // from closing the string (`r"\""` is a valid two-character
            // string), it just stays in the value; getting that wrong ends a
            // regex like r'[^\w\n"\']' at the escaped quote and derails the
            // rest of the file. It must NOT consume anything else, because a
            // backslash does not suppress an f-string's brace handling:
            // rf'\{{%' is a literal backslash followed by an *escaped brace*,
            // so eating the first '{' here would leave the second one opening
            // a replacement field that never closes.
            char nxt = At(1);
            if (c == '\\' && (nxt == '"' || nxt == '\'' || nxt == '\\' || nxt == '\n')) {
                if (nxt == '\n') line_++;
                pos_ += 2;
                continue;
            }
            if (interp && c == '{') {
                if (At(1) == '{') {  // `{{` is an escaped brace
                    pos_ += 2;
                    continue;
                }
                fields.push_back(Field());
                pos_++;
                continue;
            }
            if (interp && c == '}' && At(1) == '}') {
                pos_ += 2;
                continue;
            }
            if (c == quote) {
                if (!triple) {
                    pos_++;
                    break;
                }
                if (At(1) == quote && At(2) == quote) {
                    pos_ += 3;
                    break;
                }
            }
            pos_++;
            continue;
        }
        Field &f = fields.back();
        if (f.spec) {
            // Format-spec text: only a nested `{...}` is code here.
            if (c == '{') {
                fields.push_back(Field());
                pos_++;
                continue;
            }
            if (c == '}') {
                fields.pop_back();
                pos_++;
                continue;
            }
            pos_++;
            continue;
        }
        // Inside a replacement field's expression.
        if (c == '"' || c == '\'' || IsStringPrefixChar(c)) {
            size_t look = pos_;
            while (look < src_.size() && IsStringPrefixChar(src_[look]) &&
                   look - pos_ < 2) {
                look++;
            }
            if (look < src_.size() && (src_[look] == '"' || src_[look] == '\'')) {
                Token nested;
                if (!ScanString(nested, depth + 1)) return false;
                continue;
            }
        }
        if (c == '(' || c == '[') {
            f.bracket++;
        } else if (c == ')' || c == ']') {
            f.bracket--;
        } else if (c == '{') {
            fields.push_back(Field());
            pos_++;
            continue;
        } else if (c == '}') {
            fields.pop_back();
            pos_++;
            continue;
        } else if (c == ':' && f.bracket == 0) {
            f.spec = true;
        }
        pos_++;
    }
    out.kind = Kind::String;
    out.text.assign(src_.substr(start, pos_ - start));
    out.line = start_line;
    return true;
}

void Tokenizer::ScanNumber(Token &out) {
    size_t start = pos_;
    out.line = line_;
    if (Cur() == '0' && (At(1) == 'x' || At(1) == 'X' || At(1) == 'o' ||
                         At(1) == 'O' || At(1) == 'b' || At(1) == 'B')) {
        pos_ += 2;
        while (std::isalnum(static_cast<unsigned char>(Cur())) != 0 || Cur() == '_') pos_++;
    } else {
        bool seen_dot = false;
        while (!AtEnd()) {
            char c = Cur();
            if (std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '_') {
                pos_++;
            } else if (c == '.' && !seen_dot) {
                seen_dot = true;
                pos_++;
            } else if ((c == 'e' || c == 'E') &&
                       (std::isdigit(static_cast<unsigned char>(At(1))) != 0 ||
                        ((At(1) == '+' || At(1) == '-') &&
                         std::isdigit(static_cast<unsigned char>(At(2))) != 0))) {
                // Only an exponent when digits really follow, so the `e` in
                // `1.e` (an attribute access on a float, or a syntax error)
                // is not swallowed into the number.
                pos_ += 2;
            } else if (c == 'j' || c == 'J') {
                pos_++;
                break;
            } else {
                break;
            }
        }
    }
    out.kind = Kind::Number;
    out.text.assign(src_.substr(start, pos_ - start));
}

// The operator table, longest-match-first: every multi-character operator has
// to be tried before its own prefix or `**=` would tokenize as `**` then `=`.
const char *const kOperators[] = {
    "**=", "//=", ">>=", "<<=", "...",
    "!=", ">=", "<=", "==", "->", ":=", "+=", "-=", "*=", "/=", "%=",
    "&=", "|=", "^=", "@=", "**", "//", "<<", ">>",
    "+", "-", "*", "/", "%", "@", "&", "|", "^", "~", "<", ">",
    "(", ")", "[", "]", "{", "}", ",", ":", ".", ";", "=",
};

// Scans one logical line: tokens up to the newline that ends it, where
// "ends it" means a newline at bracket depth 0 that is not preceded by a
// backslash. Newlines inside brackets (implicit joining) and escaped newlines
// are consumed as whitespace, which is exactly what makes this a *logical*
// line and why a reflowed call can be re-split however the width demands.
bool Tokenizer::ScanStatement(LogicalLine &line) {
    std::vector<char> brackets;
    while (true) {
        // Inter-token whitespace. Newlines are only whitespace while a
        // bracket is open; at depth 0 they terminate the line below.
        while (!AtEnd()) {
            char c = Cur();
            if (c == ' ' || c == '\t' || c == '\f' || c == '\r') {
                pos_++;
            } else if (c == '\\' && At(1) == '\n') {
                pos_ += 2;
                line_++;
            } else if (c == '\\' && At(1) == '\r' && At(2) == '\n') {
                pos_ += 3;
                line_++;
            } else if (c == '\n' && !brackets.empty()) {
                pos_++;
                line_++;
            } else {
                break;
            }
        }
        if (AtEnd()) break;
        char c = Cur();
        if (c == '\n') {
            if (brackets.empty()) {
                pos_++;
                line_++;
                break;
            }
            continue;  // handled by the whitespace loop above
        }
        if (c == '#') {
            Token t;
            t.kind = Kind::Comment;
            t.line = line_;
            size_t start = pos_;
            while (!AtEnd() && Cur() != '\n') pos_++;
            t.text.assign(src_.substr(start, pos_ - start));
            // Strip a \r that a CRLF file leaves glued to the comment body.
            while (!t.text.empty() && t.text.back() == '\r') t.text.pop_back();
            line.tokens.push_back(t);
            if (brackets.empty()) {
                // A trailing comment at depth 0 ends the logical line with it.
                if (!AtEnd()) {
                    pos_++;
                    line_++;
                }
                break;
            }
            continue;
        }
        Token t;
        if (IsStringPrefixChar(c)) {
            // Only a string if a quote actually follows the prefix letters --
            // otherwise it is an identifier that happens to start with `r`.
            size_t look = pos_;
            while (look < src_.size() && IsStringPrefixChar(src_[look]) &&
                   look - pos_ < 2) {
                look++;
            }
            if (look < src_.size() && (src_[look] == '"' || src_[look] == '\'')) {
                if (!ScanString(t)) return false;
                line.tokens.push_back(t);
                continue;
            }
        }
        if (c == '"' || c == '\'') {
            if (!ScanString(t)) return false;
            line.tokens.push_back(t);
            continue;
        }
        if (IsIdentStart(c)) {
            size_t start = pos_;
            while (!AtEnd() && IsIdentCont(Cur())) pos_++;
            t.text.assign(src_.substr(start, pos_ - start));
            t.kind = IsKeyword(t.text) ? Kind::Keyword : Kind::Name;
            t.line = line_;
            line.tokens.push_back(t);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
            (c == '.' && std::isdigit(static_cast<unsigned char>(At(1))) != 0 &&
             At(1) != '\0' && !(At(1) == '.' ))) {
            // `.5` is a float; `...` is the ellipsis operator and is matched
            // by the table below, which is tried in longest-first order.
            ScanNumber(t);
            line.tokens.push_back(t);
            continue;
        }
        bool matched = false;
        for (const char *op : kOperators) {
            size_t n = std::char_traits<char>::length(op);
            if (src_.compare(pos_, n, op) == 0) {
                t.kind = Kind::Op;
                t.text.assign(op, n);
                t.line = line_;
                if (IsOpener(t.text)) {
                    brackets.push_back(t.text[0]);
                    if (brackets.size() > static_cast<size_t>(kMaxNest)) {
                        return Fail("expression nests too deeply");
                    }
                } else if (IsCloser(t.text)) {
                    if (brackets.empty() ||
                        brackets.back() != MatchingOpener(t.text[0])) {
                        return Fail(std::string("unmatched '") + t.text + "'");
                    }
                    brackets.pop_back();
                }
                pos_ += n;
                line.tokens.push_back(t);
                matched = true;
                break;
            }
        }
        if (!matched) {
            return Fail(std::string("unexpected character '") + c + "'");
        }
    }
    if (!brackets.empty()) {
        return Fail(std::string("unclosed '") + brackets.back() + "'");
    }
    return true;
}

bool Tokenizer::Run() {
    while (!AtEnd()) {
        int col = ScanIndentColumns();
        if (AtEnd()) break;
        if (Cur() == '\n' || (Cur() == '\r' && At(1) == '\n')) {
            pos_ += Cur() == '\r' ? size_t{2} : size_t{1};
            line_++;
            pending_blanks_++;
            continue;
        }
        if (Cur() == '\r') {  // lone CR, treat as a line end
            pos_++;
            line_++;
            pending_blanks_++;
            continue;
        }
        if (Cur() == '#') {
            // A comment-only line does not participate in the indent stack
            // (CPython's tokenizer skips it entirely), so its level is decided
            // later, from its column and its neighbours -- see AssignComment-
            // Indents. Here only the raw column is recorded.
            LogicalLine cl;
            cl.blank_before = pending_blanks_;
            cl.first_line = line_;
            cl.comment_col = col;
            Token t;
            t.kind = Kind::Comment;
            t.line = line_;
            size_t start = pos_;
            while (!AtEnd() && Cur() != '\n') pos_++;
            t.text.assign(src_.substr(start, pos_ - start));
            while (!t.text.empty() && t.text.back() == '\r') t.text.pop_back();
            cl.tokens.push_back(t);
            lines_.push_back(std::move(cl));
            pending_blanks_ = 0;
            if (!AtEnd()) {
                pos_++;
                line_++;
            }
            continue;
        }
        // A real statement: reconcile the indent stack first.
        if (col > indent_stack_.back()) {
            indent_stack_.push_back(col);
        } else if (col < indent_stack_.back()) {
            while (indent_stack_.size() > 1 && col < indent_stack_.back()) {
                indent_stack_.pop_back();
            }
            if (col != indent_stack_.back()) {
                return Fail("unindent does not match any outer indentation level");
            }
        }
        LogicalLine sl;
        sl.blank_before = pending_blanks_;
        sl.first_line = line_;
        sl.indent = static_cast<int>(indent_stack_.size()) - 1;
        sl.comment_col = col;
        pending_blanks_ = 0;
        if (!ScanStatement(sl)) return false;
        if (!sl.tokens.empty()) lines_.push_back(std::move(sl));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Literal + comment normalization
// ---------------------------------------------------------------------------

// Splits a string token into its prefix letters and the rest.
size_t StringPrefixLen(const std::string &s) {
    size_t p = 0;
    while (p < s.size() && p < 2 && IsStringPrefixChar(s[p])) p++;
    return p;
}

std::string NormalizeStringPrefix(const std::string &prefix) {
    std::string out;
    for (char c : prefix) {
        // `u''` is a no-op left over from 2/3 straddling code; black drops it.
        // Everything else is lowercased, so `F`/`RB` become `f`/`rb`.
        if (c == 'u' || c == 'U') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// One element of a single-quoted string's body: either a plain character or a
// two-character backslash escape. Splitting the body this way is what lets the
// quote choice be costed and the body rebuilt without ever miscounting a
// backslash (`"\\\\"` contains no quote at all, but a naive scan sees one).
struct BodyUnit {
    bool escape = false;
    char c = '\0';  // the character, or the escaped character when escape
};

std::vector<BodyUnit> SplitBody(const std::string &body, bool raw) {
    std::vector<BodyUnit> units;
    for (size_t i = 0; i < body.size(); i++) {
        if (!raw && body[i] == '\\' && i + 1 < body.size()) {
            BodyUnit u;
            u.escape = true;
            u.c = body[i + 1];
            units.push_back(u);
            i++;
        } else {
            BodyUnit u;
            u.c = body[i];
            units.push_back(u);
        }
    }
    return units;
}

std::string NormalizeString(const std::string &tok) {
    size_t p = StringPrefixLen(tok);
    std::string prefix = tok.substr(0, p);
    std::string newprefix = NormalizeStringPrefix(prefix);
    bool raw = false;
    bool fstring = false;  // f- or t-string: has `{...}` interpolation
    for (char c : prefix) {
        if (c == 'r' || c == 'R') raw = true;
        if (c == 'f' || c == 'F' || c == 't' || c == 'T') fstring = true;
    }
    if (p + 1 >= tok.size()) return tok;  // malformed; leave alone
    char quote = tok[p];
    bool triple = tok.size() >= p + 6 && tok[p + 1] == quote && tok[p + 2] == quote;
    size_t qlen = triple ? 3 : 1;
    if (tok.size() < p + 2 * qlen) return tok;
    std::string body = tok.substr(p + qlen, tok.size() - p - 2 * qlen);

    if (triple) {
        // Only the delimiter is swapped, never the body: a triple-quoted
        // string is usually a docstring whose interior whitespace and escapes
        // are load-bearing. `'''` becomes `"""` only when that cannot create
        // a premature terminator (a `"""` inside, or a body ending in `"`).
        if (quote == '\'' && body.find("\"\"\"") == std::string::npos &&
            (body.empty() || body.back() != '"')) {
            quote = '"';
        }
        return newprefix + std::string(3, quote) + body + std::string(3, quote);
    }

    std::vector<BodyUnit> units = SplitBody(body, raw);
    // How many backslash escapes each candidate delimiter would cost: every
    // occurrence of that character in the string's *content*, however it is
    // spelled in the source right now.
    int need_double = 0;
    int need_single = 0;
    for (const BodyUnit &u : units) {
        if (u.c == '"') need_double++;
        if (u.c == '\'') need_single++;
    }
    char target = need_double <= need_single ? '"' : '\'';
    if (raw) {
        // A raw string's backslashes cannot be added or removed, so the quote
        // can only change when the target does not occur in the body at all.
        bool has_target = false;
        for (const BodyUnit &u : units) {
            if (u.c == target) has_target = true;
        }
        if (has_target) target = quote;
        std::string out = newprefix;
        out.push_back(target);
        out += body;
        out.push_back(target);
        return out;
    }
    if (fstring) {
        // Escaping a quote is illegal inside an f-string's `{...}` expression
        // (before 3.12), and this formatter does not parse into the braces to
        // find out whether a given quote is inside one. So an f-string only
        // changes delimiter when the new one needs no escaping anywhere.
        int need_target = target == '"' ? need_double : need_single;
        if (need_target > 0) target = quote;
    }
    std::string out = newprefix;
    out.push_back(target);
    for (const BodyUnit &u : units) {
        if (u.c == target) {
            out.push_back('\\');
            out.push_back(u.c);
        } else if (u.escape && (u.c == '"' || u.c == '\'')) {
            // The other quote no longer needs its backslash.
            out.push_back(u.c);
        } else if (u.escape) {
            out.push_back('\\');
            out.push_back(u.c);
        } else {
            out.push_back(u.c);
        }
    }
    out.push_back(target);
    return out;
}

std::string NormalizeNumber(const std::string &tok) {
    std::string s = tok;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        // Lowercase `0x`, uppercase the digits: `0XabcDEF` -> `0xABCDEF`.
        std::string out = "0x";
        for (size_t i = 2; i < s.size(); i++) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(s[i]))));
        }
        return out;
    }
    // Everything else is uniformly lowercased, which covers the `0O`/`0B`
    // prefixes, a `1E5` exponent and a `2J` imaginary suffix in one rule.
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Black's comment rule: strip trailing whitespace, then ensure a space after
// the leading `#` unless the first body character is one that conventionally
// means "this is not prose" -- `!` (shebang), `:` (Sphinx `#:`), `#` (a
// `#####` banner or `##` commented-out code) or an apostrophe.
std::string NormalizeComment(const std::string &tok, bool first_line_of_file) {
    std::string s = tok;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    if (s.size() < 2) return s;
    if (first_line_of_file && s[1] == '!') return s;
    char c = s[1];
    if (c == ' ' || c == '!' || c == ':' || c == '#' || c == '\'') return s;
    return "#" + std::string(" ") + s.substr(1);
}

// ---------------------------------------------------------------------------
// Statement analysis: bracket roles, unary operators, inter-token spacing
// ---------------------------------------------------------------------------

// Everything the renderer and the splitter need about one statement's token
// vector, computed once up front. The key design point is that spacing is
// resolved *here*, for the whole statement, and stored per token -- so once a
// too-long statement is chopped into bracket-delimited pieces, each piece is
// rendered by simply concatenating its tokens with their own precomputed
// spacing. Re-deriving spacing per piece would lose the surrounding context
// (an element of a call needs to know it is inside a call for `a=1` to stay
// tight) and is exactly the kind of thing that makes token-based formatters
// inconsistent between the flat and the exploded rendering of the same code.
struct Analysis {
    std::vector<size_t> match;       // bracket index <-> its partner
    std::vector<int> sp;             // spaces to emit before token i
    std::vector<bool> slice_colon;   // ':' that is a subscript slice separator
    std::vector<bool> slice_spaced;  // ...and that slice wants spaces around it
    std::vector<bool> kwarg_eq;      // '=' rendered without surrounding spaces
    std::vector<bool> tight_power;   // '**' rendered without surrounding spaces
    std::vector<bool> def_paren;     // '(' introducing def/class parameters
    std::vector<bool> call_paren;    // '(' or '[' that is a call/subscript
};

bool IsUnaryContext(const std::vector<Token> &t, size_t i) {
    if (i == 0) return true;
    const Token &pv = t[i - 1];
    if (pv.kind == Kind::Op) {
        // A closing bracket or an ellipsis completes a value, so what follows
        // is a binary operator; every other operator leaves an operand owing.
        return !IsCloser(pv.text) && pv.text != "...";
    }
    if (pv.kind == Kind::Keyword) return !IsValueKeyword(pv.text);
    return false;  // after a Name/Number/String
}

// "Simple" in black's sense for the `**` spacing rule: a plain name, a number,
// or a dotted attribute chain rooted in a name -- but not a call or a
// subscript, which is why `x**2` and `a.b**2` stay tight while `f()**2`
// becomes `f() ** 2`. `dir` walks left (-1) from the operand's last token or
// right (+1) from its first.
bool IsSimplePowerOperand(const std::vector<Token> &t, size_t idx, int dir) {
    size_t i = idx;
    if (dir > 0) {
        // Skip a unary sign: `x**-1` is still tight.
        while (i < t.size() && t[i].kind == Kind::Op &&
               (t[i].text == "-" || t[i].text == "+" || t[i].text == "~")) {
            i++;
        }
        if (i >= t.size()) return false;
        if (t[i].kind == Kind::Number) {
            return i + 1 >= t.size() || (t[i + 1].text != "(" && t[i + 1].text != "[");
        }
        if (t[i].kind != Kind::Name) return false;
        while (i + 2 < t.size() && t[i + 1].text == "." && t[i + 2].kind == Kind::Name) {
            i += 2;
        }
        return i + 1 >= t.size() || (t[i + 1].text != "(" && t[i + 1].text != "[");
    }
    if (t[i].kind == Kind::Number) {
        return true;
    }
    if (t[i].kind != Kind::Name) return false;
    while (i >= 2 && t[i - 1].text == "." && t[i - 2].kind == Kind::Name) {
        i -= 2;
    }
    // If a dot still precedes the chain's root, the root was a call or a
    // subscript (`f().attr`), which is not simple.
    return !(i >= 1 && t[i - 1].text == ".");
}

// Is the token run [lo, hi) a "simple" slice operand -- empty, a bare
// name/number/string/None, or one of those behind a unary sign? Anything
// arithmetic makes the whole subscript's colons take spaces, per PEP 8's
// "treat the colon as the operator with the lowest priority".
bool IsSimpleSliceOperand(const std::vector<Token> &t, size_t lo, size_t hi) {
    if (lo >= hi) return true;
    size_t i = lo;
    if (t[i].kind == Kind::Op &&
        (t[i].text == "-" || t[i].text == "+" || t[i].text == "~")) {
        i++;
    }
    if (i >= hi) return false;
    bool atom = t[i].kind == Kind::Name || t[i].kind == Kind::Number ||
                t[i].kind == Kind::String ||
                (t[i].kind == Kind::Keyword && IsValueKeyword(t[i].text));
    return atom && i + 1 == hi;
}

// One open bracket (plus a synthetic frame for the statement's own top level)
// as the spacing walk sees it.
struct Frame {
    char open = '\0';
    bool call = false;           // '('/'[' directly applied to a value
    bool def = false;            // '(' holding def/class parameters
    bool subscript = false;      // '[' applied to a value
    bool arg_annotated = false;  // current element carries a `: annotation`
    int lambda_pending = 0;      // `lambda`s here still owing their ':'
};

Analysis Analyze(std::vector<Token> &t) {
    Analysis a;
    size_t n = t.size();
    const size_t kNone = static_cast<size_t>(-1);
    a.match.assign(n, kNone);
    a.sp.assign(n, 1);
    a.slice_colon.assign(n, false);
    a.slice_spaced.assign(n, false);
    a.kwarg_eq.assign(n, false);
    a.tight_power.assign(n, false);
    a.def_paren.assign(n, false);
    a.call_paren.assign(n, false);
    std::vector<int> depth(n, 0);

    // --- brackets ----------------------------------------------------------
    {
        std::vector<size_t> stack;
        for (size_t i = 0; i < n; i++) {
            if (t[i].kind == Kind::Op && IsCloser(t[i].text) && !stack.empty()) {
                size_t j = stack.back();
                stack.pop_back();
                a.match[i] = j;
                a.match[j] = i;
            }
            depth[i] = static_cast<int>(stack.size());
            if (t[i].kind == Kind::Op && IsOpener(t[i].text)) stack.push_back(i);
        }
    }

    // A `def`/`class` header's parameter list, whose `=` defaults are tight
    // but whose annotated ones are not. `async def` puts the keyword first.
    bool header = false;
    if (!t.empty() && t[0].kind == Kind::Keyword) {
        header = t[0].text == "def" || t[0].text == "class" ||
                 (t[0].text == "async" && n > 1 && t[1].text == "def");
    }
    bool header_paren_seen = false;
    for (size_t i = 0; i < n; i++) {
        if (t[i].kind != Kind::Op || !IsOpener(t[i].text)) continue;
        bool applied = i > 0 && (t[i - 1].kind == Kind::Name ||
                                 t[i - 1].kind == Kind::Number ||
                                 t[i - 1].kind == Kind::String ||
                                 (t[i - 1].kind == Kind::Op && IsCloser(t[i - 1].text)));
        if (i == 1 && t[0].kind == Kind::Name && IsSoftKeywordHeadName(t[0].text)) {
            applied = false;
        }
        if (t[i].text == "(" || t[i].text == "[") a.call_paren[i] = applied;
        if (header && t[i].text == "(" && depth[i] == 0 && !header_paren_seen) {
            a.def_paren[i] = true;
            header_paren_seen = true;
        }
    }

    // --- slice colons ------------------------------------------------------
    for (size_t i = 0; i < n; i++) {
        if (!(t[i].kind == Kind::Op && t[i].text == "[" && a.call_paren[i])) continue;
        size_t close = a.match[i];
        if (close == kNone) continue;
        // Boundaries at this subscript's own level: the colons that make it a
        // slice, and the commas of a multi-axis subscript.
        std::vector<size_t> colons;
        std::vector<size_t> bounds;
        bounds.push_back(i);
        for (size_t j = i + 1; j < close; j++) {
            if (depth[j] != depth[i] + 1 || t[j].kind != Kind::Op) continue;
            if (t[j].text == ":") {
                colons.push_back(j);
                bounds.push_back(j);
            } else if (t[j].text == ",") {
                bounds.push_back(j);
            }
        }
        if (colons.empty()) continue;  // an ordinary index, not a slice
        bounds.push_back(close);
        bool simple = true;
        for (size_t b = 0; b + 1 < bounds.size(); b++) {
            if (!IsSimpleSliceOperand(t, bounds[b] + 1, bounds[b + 1])) simple = false;
        }
        for (size_t c : colons) {
            a.slice_colon[c] = true;
            a.slice_spaced[c] = !simple;
        }
    }

    // --- unary operators, spacing -----------------------------------------
    std::vector<Frame> fr;
    fr.push_back(Frame());
    for (size_t i = 0; i < n; i++) {
        Token &cu = t[i];
        // Decorators: the leading '@' is a prefix, not the matmul operator.
        bool decorator_at = i == 0 && cu.kind == Kind::Op && cu.text == "@";
        if (cu.kind == Kind::Op &&
            (cu.text == "+" || cu.text == "-" || cu.text == "~" ||
             cu.text == "*" || cu.text == "**")) {
            cu.unary = IsUnaryContext(t, i);
        }
        if (decorator_at) cu.unary = true;
        if (cu.kind == Kind::Op && cu.text == "**" && !cu.unary) {
            a.tight_power[i] = IsSimplePowerOperand(t, i - 1, -1) &&
                               i + 1 < n && IsSimplePowerOperand(t, i + 1, 1);
        }
        if (cu.kind == Kind::Op && cu.text == "=") {
            Frame &f = fr.back();
            a.kwarg_eq[i] = f.open == '(' && (f.call || f.def) && !f.arg_annotated;
        }

        // Spacing relative to the previous token.
        if (i == 0) {
            a.sp[i] = 0;
        } else {
            const Token &pv = t[i - 1];
            int sp = 1;
            if (cu.kind == Kind::Comment) {
                sp = 2;  // PEP 8's two spaces before an inline comment
            } else if (cu.kind == Kind::Op && (cu.text == "," || cu.text == ";")) {
                sp = 0;
            } else if (cu.kind == Kind::Op && IsCloser(cu.text)) {
                sp = 0;
            } else if (pv.kind == Kind::Op && IsOpener(pv.text)) {
                sp = 0;
            } else if (pv.unary) {
                sp = 0;
            } else if (cu.kind == Kind::Op && cu.text == ".") {
                // `from . import x` needs the space; `a.b` must not have one.
                sp = pv.kind == Kind::Keyword ? 1 : 0;
            } else if (pv.kind == Kind::Op && pv.text == ".") {
                sp = cu.kind == Kind::Keyword ? 1 : 0;
            } else if (cu.kind == Kind::Op && cu.text == ":") {
                sp = a.slice_colon[i] && a.slice_spaced[i] ? 1 : 0;
            } else if (pv.kind == Kind::Op && pv.text == ":") {
                sp = a.slice_colon[i - 1] && !a.slice_spaced[i - 1] ? 0 : 1;
            } else if (cu.kind == Kind::Op && cu.text == "=" && a.kwarg_eq[i]) {
                sp = 0;
            } else if (pv.kind == Kind::Op && pv.text == "=" && a.kwarg_eq[i - 1]) {
                sp = 0;
            } else if (cu.kind == Kind::Op && cu.text == "**" && a.tight_power[i]) {
                sp = 0;
            } else if (pv.kind == Kind::Op && pv.text == "**" && a.tight_power[i - 1]) {
                sp = 0;
            } else if (cu.kind == Kind::Op &&
                       (cu.text == "(" || cu.text == "[")) {
                // Tight against the thing being called or subscripted, spaced
                // otherwise. This reads the classification made above rather
                // than re-deriving it from the previous token, so the
                // soft-keyword override reaches spacing too: `case (A | B):`
                // keeps its space, because that paren is a pattern group and
                // not a call to something named `case`.
                sp = a.call_paren[i] ? 0 : 1;
            } else if (cu.kind == Kind::Op && cu.text == "{") {
                sp = (pv.kind == Kind::Name || pv.kind == Kind::Number ||
                      pv.kind == Kind::String ||
                      (pv.kind == Kind::Op && IsCloser(pv.text)))
                         ? 0
                         : 1;
            }
            a.sp[i] = sp;
        }

        // Frame bookkeeping, after this token's own spacing is settled.
        if (cu.kind == Kind::Op && IsOpener(cu.text)) {
            Frame f;
            f.open = cu.text[0];
            f.call = a.call_paren[i];
            f.def = a.def_paren[i];
            f.subscript = cu.text == "[" && a.call_paren[i];
            fr.push_back(f);
        } else if (cu.kind == Kind::Op && IsCloser(cu.text)) {
            if (fr.size() > 1) fr.pop_back();
        } else if (cu.kind == Kind::Keyword && cu.text == "lambda") {
            fr.back().lambda_pending++;
        } else if (cu.kind == Kind::Op && cu.text == ",") {
            fr.back().arg_annotated = false;
        } else if (cu.kind == Kind::Op && cu.text == ":") {
            // A lambda's colon is not an annotation, so it must not make the
            // following `=` in `f(key=lambda x: x, n=1)` grow spaces.
            if (fr.back().lambda_pending > 0) {
                fr.back().lambda_pending--;
            } else if (fr.back().def) {
                fr.back().arg_annotated = true;
            }
        }
    }
    return a;
}

// ---------------------------------------------------------------------------
// Emitter
// ---------------------------------------------------------------------------

const size_t kNpos = static_cast<size_t>(-1);

// One comma-separated item inside a bracket, plus the comments that belong
// with it. `lo`/`hi` bound the code tokens only -- a comment that appeared in
// the middle of an element is pushed out to `post` rather than left inline,
// which moves it to the end of the item but can never break the code.
struct Element {
    size_t lo = 0;
    size_t hi = 0;  // exclusive; lo == hi for a comment-only tail
    std::vector<size_t> pre;   // comments on their own lines before it
    std::vector<size_t> post;  // first is inline after the comma, rest follow
    bool has_code = false;
};

class Emitter {
   public:
    Emitter(const std::vector<Token> &toks, const Analysis &an, const Options &opts)
        : t_(toks), a_(an), opts_(opts) {}

    std::vector<std::string> &out() { return out_; }
    bool last_opens_block() const { return last_opens_block_; }

    // Emits the statement spanning [lo, hi), splitting `;`-joined statements
    // and one-line compound bodies (`if x: y`) into separate lines first.
    void EmitStatement(size_t lo, size_t hi, int indent);

   private:
    std::string Render(size_t lo, size_t hi) const;
    int Cols(int indent) const { return indent * opts_.indent_width; }
    void Push(int indent, const std::string &s) {
        out_.push_back(std::string(static_cast<size_t>(Cols(indent)), ' ') + s);
    }
    std::vector<size_t> TopCommas(size_t lo, size_t hi) const;
    bool HasTopFor(size_t lo, size_t hi) const;
    bool IsMagicComma(size_t open) const;
    bool ForcedSplit(size_t lo, size_t hi) const;
    size_t FirstTopLevelColon(size_t lo, size_t hi) const;
    std::vector<Element> SplitElements(size_t lo, size_t hi) const;
    bool CanAddTrailingComma(size_t open, size_t close, bool body_has_comma) const;
    void EmitSimple(size_t lo, size_t hi, int indent);
    void EmitExpr(size_t lo, size_t hi, int indent, const std::string &suffix);
    void RightHandSplit(size_t lo, size_t hi, int indent, const std::string &suffix);

    const std::vector<Token> &t_;
    const Analysis &a_;
    const Options &opts_;
    std::vector<std::string> out_;
    bool last_opens_block_ = false;
};

std::string Emitter::Render(size_t lo, size_t hi) const {
    std::string s;
    for (size_t i = lo; i < hi; i++) {
        if (i > lo) s.append(static_cast<size_t>(a_.sp[i]), ' ');
        s += t_[i].text;
    }
    return s;
}

// Commas in [lo, hi) that separate this bracket's own elements. Two things
// make a comma *not* one of those and both matter: a nested bracket, and an
// unparenthesized `lambda`, whose parameter commas sit at the enclosing
// bracket's depth (`f(lambda a, b: a)` has one element, not two). Splitting on
// a lambda's comma would emit `lambda a,` on a line of its own, which is not
// merely ugly but a syntax error -- exactly the class of mistake the header's
// note about a token-stream formatter's limits is about, caught here rather
// than left to the verifier.
std::vector<size_t> Emitter::TopCommas(size_t lo, size_t hi) const {
    std::vector<size_t> commas;
    int depth = 0;
    int lambdas = 0;
    for (size_t i = lo; i < hi; i++) {
        const Token &tk = t_[i];
        if (tk.kind == Kind::Keyword && tk.text == "lambda" && depth == 0) {
            lambdas++;
        } else if (tk.kind == Kind::Op) {
            if (IsOpener(tk.text)) {
                depth++;
            } else if (IsCloser(tk.text)) {
                depth--;
            } else if (depth == 0 && tk.text == ":" && lambdas > 0) {
                lambdas--;
            } else if (depth == 0 && tk.text == "," && lambdas == 0) {
                commas.push_back(i);
            }
        }
    }
    return commas;
}

// A `for` at this bracket's own depth means the body is a comprehension or a
// generator expression. Neither may gain a trailing comma, and neither may be
// split on its commas -- `{k: v for k, v in items}` would become
// `{k: v for k,` / `v in items}`.
bool Emitter::HasTopFor(size_t lo, size_t hi) const {
    int depth = 0;
    for (size_t i = lo; i < hi; i++) {
        if (t_[i].kind == Kind::Op && IsOpener(t_[i].text)) depth++;
        else if (t_[i].kind == Kind::Op && IsCloser(t_[i].text)) depth--;
        else if (t_[i].kind == Kind::Keyword && t_[i].text == "for" && depth == 0) return true;
    }
    return false;
}

// black's "magic trailing comma": a comma the author left before a closing
// bracket is read as "keep this exploded", so the bracket is split even when
// it would fit on one line. The two exceptions are the ones where the comma is
// not stylistic but part of the value -- a one-tuple `(a,)` and a
// single-element subscript `x[a,]` -- which must stay exactly as they are.
bool Emitter::IsMagicComma(size_t open) const {
    size_t close = a_.match[open];
    if (close == kNpos) return false;
    if (close <= open + 1) return false;
    if (!(t_[close - 1].kind == Kind::Op && t_[close - 1].text == ",")) return false;
    bool single_axis = TopCommas(open + 1, close).size() == 1;
    bool grouping_paren = t_[open].text == "(" && !a_.call_paren[open] && !a_.def_paren[open];
    bool subscript = t_[open].text == "[" && a_.call_paren[open];
    if ((grouping_paren || subscript) && single_axis) return false;
    return true;
}

// Does anything in [lo, hi) demand a multi-line rendering regardless of width?
// A magic trailing comma in any bracket, or a comment that would otherwise end
// up inline in the middle of the expression.
bool Emitter::ForcedSplit(size_t lo, size_t hi) const {
    for (size_t i = lo; i < hi; i++) {
        if (t_[i].kind == Kind::Comment) return true;
        if (t_[i].kind == Kind::Op && IsOpener(t_[i].text)) {
            size_t close = a_.match[i];
            if (close != kNpos && close < hi && IsMagicComma(i)) return true;
        }
    }
    return false;
}

std::vector<Element> Emitter::SplitElements(size_t lo, size_t hi) const {
    std::vector<Element> els;
    Element cur;
    cur.lo = lo;
    cur.hi = lo;
    int depth = 0;
    int lambdas = 0;
    int last_comma_line = -1;
    bool closed_any = false;
    for (size_t i = lo; i < hi; i++) {
        const Token &tk = t_[i];
        if (tk.kind == Kind::Comment) {
            if (!cur.has_code && closed_any && tk.line == last_comma_line &&
                !els.empty()) {
                // Same source line as the comma just consumed: it trails the
                // element that comma closed.
                els.back().post.push_back(i);
            } else if (!cur.has_code) {
                cur.pre.push_back(i);
            } else {
                cur.post.push_back(i);
            }
            continue;
        }
        if (tk.kind == Kind::Keyword && tk.text == "lambda" && depth == 0) {
            lambdas++;
        } else if (tk.kind == Kind::Op) {
            if (IsOpener(tk.text)) {
                depth++;
            } else if (IsCloser(tk.text)) {
                depth--;
            } else if (tk.text == ":" && depth == 0 && lambdas > 0) {
                lambdas--;
            } else if (tk.text == "," && depth == 0 && lambdas == 0) {
                els.push_back(cur);
                closed_any = true;
                last_comma_line = tk.line;
                cur = Element();
                cur.lo = i + 1;
                cur.hi = i + 1;
                continue;
            }
        }
        if (!cur.has_code) {
            cur.lo = i;
            cur.has_code = true;
        } else if (!cur.post.empty()) {
            // Code after what looked like a trailing comment means the comment
            // was *interior* to this element after all. It stays inside
            // [lo, hi) and EmitExpr's stranded-comment path renders it; keeping
            // it here as well would emit it a second time.
            cur.post.clear();
        }
        cur.hi = i + 1;
    }
    if (cur.has_code || !cur.pre.empty() || !cur.post.empty()) els.push_back(cur);
    return els;
}

bool Emitter::CanAddTrailingComma(size_t open, size_t close, bool body_has_comma) const {
    // A comprehension or generator has no comma to be trailing: `[x for x in y,]`
    // and `f(x for x in y,)` are both syntax errors.
    if (HasTopFor(open + 1, close)) return false;
    // A bare `*` separator in a def's parameter list cannot be followed by a
    // comma-and-close.
    if (close >= 2 && t_[close - 1].kind == Kind::Op && t_[close - 1].text == "*") return false;
    if (body_has_comma) return true;  // already a comma context; adding is a no-op
    char open_ch = t_[open].text[0];
    if (open_ch == '(') return a_.call_paren[open] || a_.def_paren[open];
    if (open_ch == '[') return !a_.call_paren[open];  // a literal list, not a subscript
    return true;                                      // '{' set/dict literal
}

// The first `:` at bracket depth 0 that introduces a suite, skipping any that
// belongs to a `lambda` at the same level.
size_t Emitter::FirstTopLevelColon(size_t lo, size_t hi) const {
    int depth = 0;
    int lambdas = 0;
    for (size_t i = lo; i < hi; i++) {
        const Token &tk = t_[i];
        if (tk.kind == Kind::Keyword && tk.text == "lambda" && depth == 0) {
            lambdas++;
        } else if (tk.kind == Kind::Op) {
            if (IsOpener(tk.text)) depth++;
            else if (IsCloser(tk.text)) depth--;
            else if (tk.text == ":" && depth == 0) {
                if (lambdas > 0) lambdas--;
                else return i;
            }
        }
    }
    return kNpos;
}

void Emitter::EmitStatement(size_t lo, size_t hi, int indent) {
    if (lo >= hi) return;
    // The suite split has to happen *before* the `;` split, not after: in
    // `if x: a; b` both `a` and `b` belong to the body, so splitting on the
    // semicolon first would emit `b` at the header's own indent and quietly
    // move it out of the `if`.
    if (t_[lo].kind == Kind::Keyword) {
        static const char *const kSuite[] = {"if",  "elif",    "else",  "for",
                                             "while", "with",  "try",   "except",
                                             "finally", "def", "class", "async"};
        bool compound = false;
        for (const char *k : kSuite) {
            if (t_[lo].text == k) compound = true;
        }
        if (compound) {
            size_t colon = FirstTopLevelColon(lo, hi);
            // Only split when something other than a trailing comment follows
            // the colon -- `else:` and `def f():  # note` stay one line.
            if (colon != kNpos && colon + 1 < hi &&
                !(colon + 2 == hi && t_[colon + 1].kind == Kind::Comment)) {
                EmitExpr(lo, colon + 1, indent, "");
                EmitStatement(colon + 1, hi, indent + 1);
                return;
            }
        }
    }
    // `a = 1; b = 2` is two statements that happen to share a line; black
    // gives each its own, and so does this.
    int depth = 0;
    size_t seg_start = lo;
    for (size_t i = lo; i < hi; i++) {
        if (t_[i].kind != Kind::Op) continue;
        if (IsOpener(t_[i].text)) depth++;
        else if (IsCloser(t_[i].text)) depth--;
        else if (t_[i].text == ";" && depth == 0) {
            if (i > seg_start) EmitSimple(seg_start, i, indent);
            seg_start = i + 1;
        }
    }
    if (seg_start < hi) {
        EmitSimple(seg_start, hi, indent);
    } else if (seg_start == lo) {
        EmitSimple(lo, hi, indent);
    }
}

void Emitter::EmitSimple(size_t lo, size_t hi, int indent) {
    if (lo >= hi) return;
    EmitExpr(lo, hi, indent, "");
    // Whether the *last* line emitted opened a block, which is what the blank
    // line tracker needs to know (a split one-liner ends with its body, not
    // with the header).
    size_t last = hi;
    while (last > lo && t_[last - 1].kind == Kind::Comment) last--;
    last_opens_block_ = last > lo && t_[last - 1].kind == Kind::Op &&
                        t_[last - 1].text == ":";
}

// Emits [lo, hi) at `indent`, on one line if it fits and nothing forces a
// split, otherwise by splitting at its rightmost bracket. `suffix` is text
// appended to the final physical line (an inline comment, or the comma a
// caller wants after an exploded element).
void Emitter::EmitExpr(size_t lo, size_t hi, int indent, const std::string &suffix) {
    if (lo >= hi) return;
    // A trailing comment is carried along rather than measured as part of the
    // expression's own bracket structure. `suffix` (the comma an exploded
    // element is owed) has to land *before* it -- put a comment first and the
    // comma ends up inside it, i.e. deleted.
    size_t code_hi = hi;
    while (code_hi > lo && t_[code_hi - 1].kind == Kind::Comment) code_hi--;
    std::string tail_suffix = suffix;
    for (size_t i = code_hi; i < hi; i++) tail_suffix += "  " + t_[i].text;
    if (code_hi <= lo) {
        // Nothing but comments (a dangling comment inside a bracket).
        for (size_t i = lo; i < hi; i++) Push(indent, t_[i].text);
        return;
    }
    std::string flat = Render(lo, code_hi);
    bool forced = ForcedSplit(lo, code_hi);
    if (!forced &&
        Cols(indent) + static_cast<int>(flat.size() + tail_suffix.size()) <=
            opts_.line_length) {
        Push(indent, flat + tail_suffix);
        return;
    }
    RightHandSplit(lo, code_hi, indent, tail_suffix);
}

void Emitter::RightHandSplit(size_t lo, size_t hi, int indent,
                             const std::string &suffix) {
    // Which bracket pair to split at. The preference is the rightmost one
    // that is wholly inside this range and has a body worth moving, because
    // that is what produces black's familiar shape: the call's arguments
    // explode while everything leading up to `(` stays on the first line.
    //
    // But rightmost alone is not enough. `def f(a, b) -> Dict[str, int]:`
    // ends in a subscript, and splitting there leaves the head
    // `def f(a, b) -> Dict[` still over the limit -- so the split bought
    // nothing and the parameter list, the thing actually worth exploding,
    // never gets its turn. So candidates are tried right to left and the
    // first whose *head* fits wins; if none fits, the leftmost is used, since
    // it has the shortest head and is the best of a bad set.
    std::vector<size_t> candidates;
    {
        int depth = 0;
        for (size_t i = lo; i < hi; i++) {
            if (t_[i].kind != Kind::Op) continue;
            if (IsOpener(t_[i].text)) {
                if (depth == 0) {
                    size_t c = a_.match[i];
                    if (c != kNpos && c < hi && c > i + 1) candidates.push_back(i);
                }
                depth++;
            } else if (IsCloser(t_[i].text)) {
                // A tail range (`) -> Dict[str, int]:`) opens on a closer whose
                // partner is outside the range; it must not drive depth
                // negative, or every later bracket here looks nested.
                if (depth > 0) depth--;
            }
        }
    }
    size_t open = kNpos;
    for (size_t k = candidates.size(); k-- > 0;) {
        if (Cols(indent) + static_cast<int>(Render(lo, candidates[k] + 1).size()) <=
            opts_.line_length) {
            open = candidates[k];
            break;
        }
    }
    if (open == kNpos && !candidates.empty()) open = candidates.front();
    if (open == kNpos) {
        // Nothing to split at. If comments are stranded *inside* the range --
        // the classic case is implicitly concatenated string pieces each with
        // its own trailing note -- they cannot be rendered inline, because
        // everything after the first '#' would be swallowed into that comment
        // and the rest of the expression would be lost. Break the line at each
        // one instead. Such a comment is always inside a bracket (a '#' at
        // statement level ends the logical line), so the break is safe.
        std::vector<size_t> comments;
        for (size_t i = lo; i < hi; i++) {
            if (t_[i].kind == Kind::Comment) comments.push_back(i);
        }
        if (comments.empty()) {
            // Genuinely nothing to do: emit over-long rather than invent a
            // break that would change what the code means.
            Push(indent, Render(lo, hi) + suffix);
            return;
        }
        size_t seg = lo;
        for (size_t idx : comments) {
            if (idx > seg) {
                EmitExpr(seg, idx, indent, "  " + t_[idx].text);
            } else {
                Push(indent, t_[idx].text);
            }
            seg = idx + 1;
        }
        if (seg < hi) {
            EmitExpr(seg, hi, indent, suffix);
        } else if (!suffix.empty() && !out_.empty()) {
            out_.back() += suffix;
        }
        return;
    }
    size_t close = a_.match[open];

    EmitExpr(lo, open + 1, indent, "");

    bool body_has_comma = !TopCommas(open + 1, close).empty();
    std::vector<Element> els = SplitElements(open + 1, close);
    // A comprehension body is one element however many commas it contains.
    bool comma_split = !HasTopFor(open + 1, close) &&
                       (body_has_comma || CanAddTrailingComma(open, close, body_has_comma));
    if (!comma_split) {
        // A single grouping-paren body (`if (\n    a and b\n):`): indent it one
        // level and let it split further on its own terms, with no comma added.
        EmitExpr(open + 1, close, indent + 1, "");
    } else if (!IsMagicComma(open) && !ForcedSplit(open + 1, close) &&
               Cols(indent + 1) + static_cast<int>(Render(open + 1, close).size()) <=
                   opts_.line_length) {
        // (A magic trailing comma on *this* bracket is a request for the
        // one-per-line shape specifically, so it skips this stage. ForcedSplit
        // only sees the brackets nested inside the body, not this one's own
        // trailing comma.)
        // black's two-stage split: once the brackets are on their own lines the
        // contents often fit on a single line between them, and that is the
        // preferred shape -- one argument per line (below) is the fallback for
        // when they still do not. No trailing comma is added in this form,
        // because adding one would force the exploded shape on the next run.
        Push(indent + 1, Render(open + 1, close));
    } else {
        bool add_trailing = CanAddTrailingComma(open, close, body_has_comma);
        for (size_t e = 0; e < els.size(); e++) {
            const Element &el = els[e];
            for (size_t c : el.pre) Push(indent + 1, t_[c].text);
            if (!el.has_code) continue;
            bool last = true;
            for (size_t k = e + 1; k < els.size(); k++) {
                if (els[k].has_code) last = false;
            }
            std::string comma = (!last || add_trailing) ? "," : "";
            std::string inline_comment;
            if (!el.post.empty()) inline_comment = "  " + t_[el.post[0]].text;
            EmitExpr(el.lo, el.hi, indent + 1, comma + inline_comment);
            for (size_t k = 1; k < el.post.size(); k++) {
                Push(indent + 1, t_[el.post[k]].text);
            }
        }
    }

    // The tail (`)`, plus whatever follows it -- `) -> Dict[str, int]:`) is
    // emitted at the original indent and may itself need splitting.
    EmitExpr(close, hi, indent, suffix);
}

// ---------------------------------------------------------------------------
// File assembly
// ---------------------------------------------------------------------------

bool IsDecoratorItem(const LogicalLine &l) {
    return !l.tokens.empty() && l.tokens[0].kind == Kind::Op && l.tokens[0].text == "@";
}

bool IsDefOrClassItem(const LogicalLine &l) {
    if (l.tokens.empty() || l.tokens[0].kind != Kind::Keyword) return false;
    const std::string &k = l.tokens[0].text;
    if (k == "def" || k == "class") return true;
    return k == "async" && l.tokens.size() > 1 && l.tokens[1].text == "def";
}

bool IsDefLikeItem(const LogicalLine &l) {
    return !l.IsComment() && (IsDefOrClassItem(l) || IsDecoratorItem(l));
}

// A comment line does not take part in the indent stack, so its nesting level
// has to be inferred. The rule that gets the two cases people actually care
// about right: a comment indented at least as far as the statement below it
// belongs to that statement's block (so a comment introducing the first line
// of a body goes inside the body), and one indented past the statement below
// but not past the statement above stays with the block above (so a comment
// closing out a function body is not yanked to module level by the next
// top-level def).
void AssignCommentIndents(std::vector<LogicalLine> &lines) {
    for (size_t i = 0; i < lines.size(); i++) {
        if (!lines[i].IsComment()) continue;
        bool have_prev = false, have_next = false;
        int prev_indent = 0, prev_col = 0, next_indent = 0;
        for (size_t j = i; j-- > 0;) {
            if (!lines[j].IsComment()) {
                have_prev = true;
                prev_indent = lines[j].indent;
                prev_col = lines[j].comment_col;
                break;
            }
        }
        for (size_t j = i + 1; j < lines.size(); j++) {
            if (!lines[j].IsComment()) {
                have_next = true;
                next_indent = lines[j].indent;
                break;
            }
        }
        int col = lines[i].comment_col;
        if (!have_next) {
            // Trailing comments at end of file stay with the block above.
            lines[i].indent = have_prev ? prev_indent : 0;
        } else if (!have_prev) {
            lines[i].indent = next_indent;
        } else if (next_indent >= prev_indent) {
            // Either the block just opened (the comment introduces its body,
            // whatever column it was written at) or nothing changed.
            lines[i].indent = next_indent;
        } else {
            // A dedent: the comment can belong either to the block that is
            // ending or to what follows it, and its own column decides. Only
            // those two levels are candidates, which is what makes this
            // stable -- re-running on the output re-derives the same answer,
            // because the emitted column is exactly the chosen level's.
            lines[i].indent = col >= prev_col ? prev_indent : next_indent;
        }
    }
}

// ---------------------------------------------------------------------------
// Output verification
// ---------------------------------------------------------------------------

// A flat, comparable view of a token stream: what the code *is*, with
// everything this formatter is allowed to change stripped out. Comments are
// dropped (they move), and `;` is dropped (a `;`-joined line is split into
// separate statements, which removes the token).
std::vector<Token> Significant(const std::vector<LogicalLine> &lines) {
    std::vector<Token> out;
    for (const LogicalLine &l : lines) {
        bool first = true;
        for (const Token &tk : l.tokens) {
            if (tk.kind == Kind::Comment) continue;
            if (tk.kind == Kind::Op && tk.text == ";") continue;
            Token n = tk;
            n.stmt_start = first;
            first = false;
            if (n.kind == Kind::String) n.text = NormalizeString(n.text);
            if (n.kind == Kind::Number) n.text = NormalizeNumber(n.text);
            out.push_back(n);
        }
    }
    return out;
}

// Is `after[j]` a comma that the splitter is allowed to have added -- i.e. one
// sitting directly before a closing bracket whose partner makes it inert? The
// test mirrors CanAddTrailingComma, and mirroring it *here*, independently, is
// the point: if the two ever disagree the verifier refuses rather than lets a
// meaning-changing comma through. A grouping paren is excluded because
// `(a + b,)` is a tuple, and a subscript because `x[a,]` is not `x[a]`.
bool IsInertTrailingComma(const std::vector<Token> &after, size_t j) {
    if (!(after[j].kind == Kind::Op && after[j].text == ",")) return false;
    if (j + 1 >= after.size()) return false;
    if (!(after[j + 1].kind == Kind::Op && IsCloser(after[j + 1].text))) return false;
    // Walk back to the matching opener, starting *at* the closer so it is the
    // one that opens the count.
    int depth = 0;
    size_t open = kNpos;
    for (size_t k = j + 1;; k--) {
        if (after[k].kind == Kind::Op) {
            if (IsCloser(after[k].text)) {
                depth++;
            } else if (IsOpener(after[k].text)) {
                depth--;
                if (depth == 0) {
                    open = k;
                    break;
                }
            }
        }
        if (k == 0) break;
    }
    if (open == kNpos) return false;
    bool applied = open > 0 && (after[open - 1].kind == Kind::Name ||
                                after[open - 1].kind == Kind::Number ||
                                after[open - 1].kind == Kind::String ||
                                (after[open - 1].kind == Kind::Op &&
                                 IsCloser(after[open - 1].text)));
    if (open > 0 && after[open - 1].kind == Kind::Name &&
        after[open - 1].stmt_start && IsSoftKeywordHeadName(after[open - 1].text)) {
        applied = false;
    }
    char ch = after[open].text[0];
    // If the bracket already holds another separator comma it is a tuple, an
    // argument list or a multi-axis subscript either way, so one more before
    // the closer cannot change the value -- this is the same first rule
    // CanAddTrailingComma applies. A `lambda`'s parameter commas sit at this
    // same depth without making the bracket a tuple, so they do not count.
    int d = 0;
    int lambdas = 0;
    for (size_t k = open + 1; k < j; k++) {
        const Token &tk = after[k];
        if (tk.kind == Kind::Keyword && tk.text == "lambda" && d == 0) {
            lambdas++;
        } else if (tk.kind == Kind::Op) {
            if (IsOpener(tk.text)) d++;
            else if (IsCloser(tk.text)) d--;
            else if (d == 0 && tk.text == ":" && lambdas > 0) lambdas--;
            else if (d == 0 && tk.text == "," && lambdas == 0) return true;
        }
    }
    if (ch == '(') return applied;   // a call's arg list, not a parenthesized expr
    if (ch == '[') return !applied;  // a list literal, not a subscript
    return true;                     // '{' set/dict literal
}

// Compares the formatted output's tokens against the input's. The one
// tolerated difference is a comma the splitter *added* before a closing
// bracket, and only where that comma cannot change the value (see
// IsInertTrailingComma). The tolerance is deliberately one-directional, so a
// comma the formatter wrongly *dropped* -- which would silently turn `(1,)`
// into `(1)` -- still fails the check.
bool VerifyEquivalent(const std::vector<Token> &before, const std::vector<Token> &after,
                      std::string &why) {
    size_t i = 0, j = 0;
    while (i < before.size() && j < after.size()) {
        if (before[i].kind == after[j].kind && before[i].text == after[j].text) {
            i++;
            j++;
            continue;
        }
        if (IsInertTrailingComma(after, j)) {
            j++;
            continue;
        }
        why = "token " + std::to_string(i) + ": expected '" + before[i].text +
              "', produced '" + after[j].text + "'";
        return false;
    }
    while (j < after.size() && IsInertTrailingComma(after, j)) j++;
    if (i != before.size()) {
        why = "output ended early, missing '" + before[i].text + "'";
        return false;
    }
    if (j != after.size()) {
        why = "output has extra '" + after[j].text + "'";
        return false;
    }
    return true;
}

std::string RenderFile(std::vector<LogicalLine> &lines, const Options &opts) {
    size_t n = lines.size();
    std::vector<bool> def_like(n, false);
    std::vector<bool> suppress(n, false);
    for (size_t i = 0; i < n; i++) {
        if (!IsDefLikeItem(lines[i])) continue;
        // Back-to-back decorators, and the def under them, are one unit: the
        // surrounding blank lines belong before the first decorator only.
        size_t p = i;
        bool prev_is_decorator = false;
        while (p-- > 0) {
            if (lines[p].IsComment()) continue;
            prev_is_decorator = IsDecoratorItem(lines[p]);
            break;
        }
        if (prev_is_decorator) {
            suppress[i] = true;
            continue;
        }
        // Hoist the requirement onto a comment block sitting directly above,
        // so the blank lines land before the comment rather than between the
        // comment and the def it documents.
        size_t j = i;
        while (j > 0 && lines[j - 1].IsComment() &&
               lines[j - 1].indent == lines[i].indent) {
            j--;
        }
        def_like[j] = true;
        if (j != i) suppress[i] = true;
    }

    std::vector<std::string> out;
    bool any = false;
    bool prev_block_opener = false;
    bool prev_decorator = false;
    int prev_indent = 0;
    std::vector<int> def_depths;
    for (size_t i = 0; i < n; i++) {
        LogicalLine &item = lines[i];
        int cap = item.indent > 0 ? 1 : 2;
        int before = std::min(item.blank_before, cap);
        if (!any) {
            before = 0;
        } else if (prev_decorator) {
            // A decorator and what it decorates are one unit -- and so is a
            // comment sitting between them, which is why `prev_decorator`
            // survives comment items rather than being cleared by them.
            before = 0;
        } else if (prev_block_opener && item.indent > prev_indent) {
            // Never a blank line between `def f():` and the first line of its
            // body, however the source had it.
            before = 0;
        } else {
            // Leaving one or more def/class bodies: the surrounding blank
            // lines are owed on the way *out* as well as on the way in.
            while (!def_depths.empty() && def_depths.back() >= item.indent) {
                int d = def_depths.back();
                def_depths.pop_back();
                before = std::max(before, d == 0 ? 2 : 1);
            }
            if (def_like[i] && !suppress[i]) {
                before = std::max(before, item.indent == 0 ? 2 : 1);
            }
        }
        for (int b = 0; b < before; b++) out.push_back(std::string());

        if (item.IsComment()) {
            out.push_back(
                std::string(static_cast<size_t>(item.indent * opts.indent_width), ' ') +
                item.tokens[0].text);
            prev_block_opener = false;
        } else {
            Analysis an = Analyze(item.tokens);
            Emitter em(item.tokens, an, opts);
            em.EmitStatement(0, item.tokens.size(), item.indent);
            for (const std::string &s : em.out()) out.push_back(s);
            prev_block_opener = em.last_opens_block();
            prev_decorator = IsDecoratorItem(item);
            if (IsDefOrClassItem(item)) def_depths.push_back(item.indent);
        }
        prev_indent = item.indent;
        any = true;
    }

    std::string text;
    for (const std::string &s : out) {
        text += s;
        text += '\n';
    }
    return text;
}

}  // namespace

Result Format(std::string_view source, const Options &opts) {
    Result r;
    // A UTF-8 BOM is not part of the program. Left in, it would glue onto the
    // first token -- `\xef\xbb\xbfdef` becomes a Name rather than the `def`
    // keyword, and the statement stops being recognizable as a definition. It
    // is put back verbatim at the end.
    std::string_view bom;
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB &&
        static_cast<unsigned char>(source[2]) == 0xBF) {
        bom = source.substr(0, 3);
        source.remove_prefix(3);
    }
    Tokenizer tz(source);
    if (!tz.Run()) {
        r.error = tz.error();
        r.error_line = tz.error_line();
        return r;
    }
    std::vector<LogicalLine> lines = tz.lines();
    if (lines.empty()) {
        r.ok = true;
        r.text = std::string(bom);  // nothing but whitespace/blank lines
        return r;
    }
    AssignCommentIndents(lines);

    std::vector<Token> before_tokens = Significant(lines);

    // Literal normalization happens once, before anything measures a line:
    // `'x'` and `"x"` are the same width here but a longer replacement would
    // otherwise be measured at its old length and overrun.
    for (LogicalLine &l : lines) {
        for (Token &tk : l.tokens) {
            if (tk.kind == Kind::String && opts.normalize_strings) {
                tk.text = NormalizeString(tk.text);
            } else if (tk.kind == Kind::Number && opts.normalize_numbers) {
                tk.text = NormalizeNumber(tk.text);
            } else if (tk.kind == Kind::Comment) {
                tk.text = NormalizeComment(tk.text, tk.line == 1);
            }
        }
    }

    std::string text = RenderFile(lines, opts);

    if (opts.verify) {
        Tokenizer check(text);
        if (!check.Run()) {
            r.error = "internal error: formatted output does not tokenize (" +
                      check.error() + ")";
            return r;
        }
        std::vector<LogicalLine> after_lines = check.lines();
        std::vector<Token> after_tokens = Significant(after_lines);
        std::string why;
        if (!VerifyEquivalent(before_tokens, after_tokens, why)) {
            r.error = "internal error: formatting would change this code (" + why + ")";
            return r;
        }
    }

    r.ok = true;
    r.text = std::string(bom) + text;
    return r;
}

}  // namespace pyfmt
