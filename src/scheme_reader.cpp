// mep's Scheme reader: see scheme_reader.h. The structure follows the
// decisions of Guile's reader (ice-9/read.scm) one for one -- which
// characters delimit a token, what `#` followed by each character means,
// which error is reported where -- written afresh here; the function names
// echo the reader's own so the two can be read side by side.
#include "scheme_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kEof = -1;

bool IsAsciiDigit(int c) { return c >= '0' && c <= '9'; }

int HexValue(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int DigitValue(int c, int radix) {
    const int v = HexValue(c);
    return v >= 0 && v < radix ? v : -1;
}

bool StartsWithCi(std::string_view s, size_t at, const char *word) {
    for (size_t i = 0; word[i]; i++) {
        if (at + i >= s.size()) return false;
        if (std::tolower(static_cast<unsigned char>(s[at + i])) != word[i]) return false;
    }
    return true;
}

// ---- string->number --------------------------------------------------------
//
// Guile's (numbers.c) reading of a number: an optional radix and exactness
// prefix (each at most once, either order), then a real, a polar `r@a`, or
// a rectangular `a+bi` complex. Only acceptance matters here.

struct NumScan {
    std::string_view s;
    size_t i = 0;
    int radix = 10;
    char exactness = 0;  // 0, 'e' or 'i'
    long bad_exponent = 0;  // an exponent string->number throws on (out of range)
    bool out_of_range = false;
};

// <uinteger R> with Guile's `#` placeholders after the digits.
bool ScanUinteger(NumScan &n, bool *hashes) {
    const size_t start = n.i;
    while (n.i < n.s.size() && DigitValue(static_cast<unsigned char>(n.s[n.i]), n.radix) >= 0) n.i++;
    if (n.i == start) return false;
    while (n.i < n.s.size() && n.s[n.i] == '#') {
        n.i++;
        *hashes = true;
    }
    return true;
}

bool ScanExponent(NumScan &n) {
    if (n.i >= n.s.size()) return true;
    const char c = n.s[n.i];
    if (c == 0 || !std::strchr("eEsSfFdDlL", c)) return true;
    size_t j = n.i + 1;
    if (j < n.s.size() && (n.s[j] == '+' || n.s[j] == '-')) j++;
    const size_t digits = j;
    long value = 0;
    while (j < n.s.size() && IsAsciiDigit(static_cast<unsigned char>(n.s[j]))) {
        if (value < 100000000) value = value * 10 + (n.s[j] - '0');
        j++;
    }
    if (j == digits) return false;
    if (n.s[digits - 1] == '-') value = -value;
    // Guile's string->number throws on an exponent a double cannot hold.
    if ((value > 308 || value < -324) && !n.out_of_range) {
        n.out_of_range = true;
        n.bad_exponent = value;
    }
    n.i = j;
    return true;
}

// <ureal R> without its sign. `inf_ok`: a sign was given, so inf.0/nan.0 may follow.
bool ScanUreal(NumScan &n, bool inf_ok) {
    if (n.i >= n.s.size()) return false;
    if (inf_ok && (StartsWithCi(n.s, n.i, "inf.0") || StartsWithCi(n.s, n.i, "nan.0"))) {
        if (n.exactness == 'e') return false;
        const bool nan = n.s[n.i] == 'n' || n.s[n.i] == 'N';
        n.i += 5;
        if (nan)  // Guile reads `+nan.000` too
            while (n.i < n.s.size() && n.s[n.i] == '0') n.i++;
        return true;
    }
    bool hashes = false;
    if (n.s[n.i] == '.') {
        if (n.radix != 10) return false;
        n.i++;
        const size_t start = n.i;
        while (n.i < n.s.size() && IsAsciiDigit(static_cast<unsigned char>(n.s[n.i]))) n.i++;
        if (n.i == start) return false;
        while (n.i < n.s.size() && n.s[n.i] == '#') n.i++;
        return ScanExponent(n);
    }
    if (!ScanUinteger(n, &hashes)) return false;
    if (n.i >= n.s.size()) return true;
    if (n.s[n.i] == '/') {
        n.i++;
        const size_t den = n.i;
        bool den_hashes = false;
        if (!ScanUinteger(n, &den_hashes)) return false;
        // An exact zero denominator is no number (`1/0`, `#i1/0` alike).
        bool zero = true;
        for (size_t k = den; k < n.i; k++)
            if (n.s[k] != '0' && n.s[k] != '#') zero = false;
        return !zero;
    }
    if (n.radix == 10) {
        if (n.s[n.i] == '.') {
            n.i++;
            if (hashes) {
                while (n.i < n.s.size() && n.s[n.i] == '#') n.i++;
            } else {
                while (n.i < n.s.size() && IsAsciiDigit(static_cast<unsigned char>(n.s[n.i]))) n.i++;
                while (n.i < n.s.size() && n.s[n.i] == '#') n.i++;
            }
        }
        return ScanExponent(n);
    }
    return true;
}

// A signed real; true with n.i past it.
bool ScanReal(NumScan &n, bool *signed_out) {
    bool sign = false;
    if (n.i < n.s.size() && (n.s[n.i] == '+' || n.s[n.i] == '-')) {
        sign = true;
        n.i++;
    }
    if (signed_out) *signed_out = sign;
    return ScanUreal(n, sign);
}

bool ScanNumber(std::string_view s, int radix, long *bad_exponent = nullptr);

bool ScanNumberBody(NumScan &n) {
    const std::string_view s = n.s;
    bool have_radix = false;
    while (n.i + 1 < s.size() && s[n.i] == '#') {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[n.i + 1])));
        if (c == 'b' || c == 'o' || c == 'd' || c == 'x') {
            if (have_radix) return false;
            have_radix = true;
            n.radix = c == 'b' ? 2 : c == 'o' ? 8 : c == 'd' ? 10 : 16;
        } else if (c == 'e' || c == 'i') {
            if (n.exactness) return false;
            n.exactness = c;
        } else {
            return false;
        }
        n.i += 2;
    }
    if (n.i >= s.size()) return false;
    // `+i` / `-i`
    if ((s[n.i] == '+' || s[n.i] == '-') && n.i + 2 == s.size() && (s[n.i + 1] == 'i' || s[n.i + 1] == 'I'))
        return true;
    bool sign = false;
    if (!ScanReal(n, &sign)) return false;
    if (n.i == s.size()) return true;
    const char c = s[n.i];
    if ((c == 'i' || c == 'I') && n.i + 1 == s.size()) return sign;  // `+5i`: a pure imaginary needs a sign
    if (c == '@') {
        n.i++;
        bool s2 = false;
        if (!ScanReal(n, &s2)) return false;
        return n.i == s.size();
    }
    if (c == '+' || c == '-') {
        n.i++;
        if (n.i + 1 == s.size() && (s[n.i] == 'i' || s[n.i] == 'I')) return true;
        if (!ScanUreal(n, true)) return false;
        return n.i + 1 == s.size() && (s[n.i] == 'i' || s[n.i] == 'I');
    }
    return false;
}

bool ScanNumber(std::string_view s, int radix, long *bad_exponent) {
    NumScan n;
    n.s = s;
    n.radix = radix;
    const bool ok = ScanNumberBody(n);
    // string->number throws as soon as it meets the exponent, before it
    // looks at the rest of the token: `1e400x` is an error, not a symbol.
    if (bad_exponent) *bad_exponent = n.out_of_range ? n.bad_exponent : 0;
    return ok;
}

// ---- the reader ---------------------------------------------------------------

struct Pos {
    size_t at = 0;
    int line = 0, col = 0;  // 0-based, byte column
};

class Reader {
public:
    explicit Reader(std::string_view src, SchemeReadResult &out) : s_(src), out_(out) {
        // Guile reads a file in the encoding its `coding:` comment names
        // (`#:guess-encoding`); in a Latin-1 file every byte is a character.
        const std::string_view head = src.substr(0, std::min<size_t>(src.size(), 1000));
        const size_t at = head.find("coding:");
        if (at != std::string_view::npos) {
            size_t i = at + 7;
            while (i < head.size() && (head[i] == ' ' || head[i] == '\t')) i++;
            std::string name;
            while (i < head.size() && (std::isalnum(static_cast<unsigned char>(head[i])) || head[i] == '-' || head[i] == '_'))
                name += static_cast<char>(std::tolower(static_cast<unsigned char>(head[i++])));
            latin1_ = name.rfind("iso-8859", 0) == 0 || name.rfind("latin", 0) == 0 || name.rfind("iso8859", 0) == 0;
        }
    }

    void ReadAll() {
        for (;;) {
            const int ch = NextNonWhitespace();
            if (ch == kEof) break;
            int node = ReadExpr(ch, 0);
            if (node == kStrayClose) continue;
            if (node >= 0) out_.top.push_back(node);
        }
        out_.fold_case = fold_case_;
    }

private:
    static constexpr int kStrayClose = -2;

    std::string_view s_;
    SchemeReadResult &out_;
    size_t pos_ = 0;
    int line_ = 0, col_ = 0;  // byte column
    int gcol_ = 0;            // Guile's port column
    Pos start_;               // where the last character NextNonWhitespace returned starts
    bool fold_case_ = false;
    bool r6rs_escapes_ = false;
    bool hungry_eol_ = false;
    bool square_brackets_ = true;
    bool curly_infix_ = false;
    int neoteric_ = 0;
    bool latin1_ = false;

    // -- the port --
    int Peek() const { return pos_ < s_.size() ? static_cast<unsigned char>(s_[pos_]) : kEof; }
    int PeekAt(size_t k) const { return pos_ + k < s_.size() ? static_cast<unsigned char>(s_[pos_ + k]) : kEof; }
    Pos Here() const { return Pos{pos_, line_, col_}; }
    int Next() {
        if (pos_ >= s_.size()) return kEof;
        const int c = static_cast<unsigned char>(s_[pos_++]);
        if (c == '\n') {
            line_++;
            col_ = 0;
            gcol_ = 0;
        } else {
            col_++;
            if (c == '\r') gcol_ = 0;
            else if (c == '\t') gcol_ = (gcol_ / 8 + 1) * 8;
            else if (latin1_ || (c & 0xC0) != 0x80) gcol_++;  // a UTF-8 continuation byte is part of the character before
        }
        return c;
    }

    bool Delimiter(int c) const {
        switch (c) {
            case '(': case ')': case ';': case '"': case ' ': case '\r': case '\f': case '\n': case '\t':
                return true;
            case '[': case ']':
                return square_brackets_ || curly_infix_;
            case '{': case '}':
                return curly_infix_;
            default:
                return false;
        }
    }

    SchemeRange RangeFrom(const Pos &a) const { return SchemeRange{a.line, a.col, line_, col_}; }

    // -- errors --
    void Error(const Pos &from, const std::string &code, const std::string &message, bool positioned = true) {
        SchemeReadError e;
        e.range = RangeFrom(from);
        if (e.range.end_line == e.range.line && e.range.end_col == e.range.col) e.range.end_col++;
        if (positioned) {
            e.guile_line = line_ + 1;
            e.guile_col = gcol_ + 1;
        }
        e.code = code;
        e.message = message;
        out_.errors.push_back(std::move(e));
    }

    // The rest of a UTF-8 character whose first byte c was just read.
    std::string WholeChar(int c) {
        std::string out(1, static_cast<char>(c));
        if (!latin1_ && c >= 0xC0)
            while ((Peek() & 0xC0) == 0x80) out += static_cast<char>(Next());
        return out;
    }

    static std::string WriteChar(int c, const std::string &whole) {
        if (whole.size() > 1) return "#\\" + whole;
        return WriteChar(c);
    }

    static std::string WriteChar(int c) {
        switch (c) {
            case kEof: return "#<eof>";
            case ' ': return "#\\space";
            case '\n': return "#\\newline";
            case '\t': return "#\\tab";
            case '\r': return "#\\return";
            case '\f': return "#\\page";
            case 0: return "#\\nul";
            default: break;
        }
        return std::string("#\\") + static_cast<char>(c);
    }
    static std::string DisplayChar(int c) { return c == kEof ? "#<eof>" : std::string(1, static_cast<char>(c)); }

    // A string as Guile's `write` prints it (for messages that use ~S).
    static std::string WriteString(const std::string &str) {
        std::string out = "\"";
        for (char c : str) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                case '\r': out += "\\r"; break;
                case '\f': out += "\\f"; break;
                case '\0': out += "\\0"; break;
                default: out += c;
            }
        }
        return out + "\"";
    }

    // A symbol as Guile prints it: in `#{...}#` when it would not read back
    // as that symbol (it reads as a number, starts with a digit or a prefix
    // character, or holds a `#`), brackets and braces as hex escapes.
    static std::string DisplaySymbol(const std::string &name) {
        const unsigned char c0 = name.empty() ? 0 : static_cast<unsigned char>(name[0]);
        const bool extended = name.empty() || SchemeIsNumber(name) || name == "." || IsAsciiDigit(c0) ||
                              std::strchr("'`,\";#", c0) || name.find('#') != std::string::npos;
        if (!extended) return name;
        std::string out = "#{";
        for (char c : name) {
            if (c == '{') out += "\\x7b;";
            else if (c == '}') out += "\\x7d;";
            else if (c == '[') out += "\\x5b;";
            else if (c == ']') out += "\\x5d;";
            else out += c;
        }
        return out + "}#";
    }

    // A datum as Guile's `display` prints it (for messages that use ~a);
    // atoms as written, which is what display prints for all but numbers
    // in another radix.
    std::string Display(int node) const {
        if (node < 0) return "";
        const SchemeNode &n = out_.nodes[static_cast<size_t>(node)];
        switch (n.kind) {
            case SchemeNodeKind::List:
            case SchemeNodeKind::Vector:
            case SchemeNodeKind::Bytevector: {
                std::string out = n.kind == SchemeNodeKind::List ? "(" : n.text;
                for (size_t i = 0; i < n.kids.size(); i++) {
                    if (i) out += n.dotted && i + 1 == n.kids.size() ? " . " : " ";
                    out += Display(n.kids[i]);
                }
                return out + ")";
            }
            case SchemeNodeKind::Prefix: {
                static const char *const kNames[][2] = {
                    {"'", "quote"}, {"`", "quasiquote"}, {",", "unquote"}, {",@", "unquote-splicing"},
                    {"#'", "syntax"}, {"#`", "quasisyntax"}, {"#,", "unsyntax"}, {"#,@", "unsyntax-splicing"},
                    {"#~", "gexp"}, {"#$", "ungexp"}, {"#$@", "ungexp-splicing"}, {"#+", "ungexp-native"},
                    {"#+@", "ungexp-native-splicing"}};
                std::string name = n.text;
                for (const auto &k : kNames)
                    if (n.text == k[0]) name = k[1];
                return "(" + name + (n.kids.empty() ? "" : " " + Display(n.kids[0])) + ")";
            }
            case SchemeNodeKind::String: {
                // display drops the quotes; escapes are rare in these messages.
                return n.text.size() >= 2 ? n.text.substr(1, n.text.size() - 2) : n.text;
            }
            case SchemeNodeKind::Symbol:
                return DisplaySymbol(n.name);
            default:
                return n.text;
        }
    }

    int NewNode(SchemeNodeKind kind, const Pos &from, std::string text = {}) {
        SchemeNode n;
        n.kind = kind;
        n.range = RangeFrom(from);
        n.text = std::move(text);
        out_.nodes.push_back(std::move(n));
        return static_cast<int>(out_.nodes.size()) - 1;
    }
    void Adopt(int parent, int kid) {
        if (kid < 0) return;
        out_.nodes[static_cast<size_t>(kid)].parent = parent;
        out_.nodes[static_cast<size_t>(parent)].kids.push_back(kid);
    }
    void Finish(int node) { out_.nodes[static_cast<size_t>(node)].range.end_line = line_, out_.nodes[static_cast<size_t>(node)].range.end_col = col_; }

    // -- comments and whitespace --
    void Comment(SchemeCommentKind kind, const Pos &from, int datum = -1) {
        SchemeComment c;
        c.kind = kind;
        c.range = RangeFrom(from);
        c.datum = datum;
        out_.comments.push_back(c);
    }

    // After `;`: to the end of the line. Returns the character after the newline.
    int SkipEolComment(const Pos &from) {
        for (;;) {
            if (Peek() == '\n' || Peek() == kEof) {
                Comment(SchemeCommentKind::Line, from);
                Next();
                return Next();
            }
            Next();
        }
    }

    size_t LineStart(size_t at) const {
        while (at > 0 && s_[at - 1] != '\n') at--;
        return at;
    }

    // After `#|`: up to the matching `|#`, nesting.
    bool SkipBlockComment(const Pos &from) {
        int depth = 1;
        for (;;) {
            const int c = Next();
            if (c == kEof) {
                Error(from, "unterminated-comment", "unterminated `#| ... |#' comment");
                return false;
            }
            if (c == '|' && Peek() == '#') {
                Next();
                if (--depth == 0) return true;
            } else if (c == '#' && Peek() == '|') {
                Next();
                depth++;
            }
        }
    }

    // After `#!`: a directive or a comment to `!#`. Returns the character after it.
    int ProcessShebang(const Pos &from) {
        auto directive_char = [](int c) {
            return c != kEof && (c == '-' || std::isalnum(c) || c >= 0x80);
        };
        std::string sym;
        if (directive_char(Peek())) {
            while (directive_char(Peek())) sym += static_cast<char>(Next());
        }
        bool known = true;
        if (sym == "r6rs") {
            fold_case_ = false;
            r6rs_escapes_ = true;
            square_brackets_ = true;
            hungry_eol_ = true;
            out_.r6rs = true;
        } else if (sym == "fold-case") {
            fold_case_ = true;
        } else if (sym == "no-fold-case") {
            fold_case_ = false;
        } else if (sym == "curly-infix") {
            curly_infix_ = true;
        } else if (sym == "curly-infix-and-bracket-lists") {
            curly_infix_ = true;
            square_brackets_ = false;
        } else {
            known = false;
        }
        if (known) {
            Comment(SchemeCommentKind::Directive, from);
            return Next();
        }
        // A SCSH-style block comment, `#! ... !#`.
        int c = Next();
        for (;;) {
            if (c == kEof) {
                Error(from, "unterminated-comment", "unterminated `#! ... !#' comment");
                return kEof;
            }
            if (c == '!') {
                c = Next();
                if (c == '#') {
                    Comment(SchemeCommentKind::Shebang, from);
                    return Next();
                }
                continue;
            }
            c = Next();
        }
    }

    // Guile's next-non-whitespace: skips blanks and every kind of comment;
    // start_ is where the returned character begins.
    int NextNonWhitespace() {
        Pos at = Here();
        int c = Next();
        for (;;) {
            switch (c) {
                case ';':
                    c = SkipEolComment(at);
                    at = Prev(c);
                    continue;
                case '#': {
                    const int p = Peek();
                    if (p == '!') {
                        Next();
                        c = ProcessShebang(at);
                        at = Prev(c);
                        continue;
                    }
                    if (p == ';') {
                        Next();
                        const int datum = ReadSubexpression("#; comment", 0);
                        Comment(SchemeCommentKind::Datum, at, datum >= 0 ? datum : -1);
                        at = Here();
                        c = Next();
                        continue;
                    }
                    if (p == '|') {
                        Next();
                        if (SkipBlockComment(at)) Comment(SchemeCommentKind::Block, at);
                        at = Here();
                        c = Next();
                        continue;
                    }
                    start_ = at;
                    return c;
                }
                case ' ': case '\r': case '\f': case '\n': case '\t':
                    at = Here();
                    c = Next();
                    continue;
                default:
                    start_ = at;
                    return c;
            }
        }
    }

    // The position of the character just returned by Next() (c), for a
    // loop that was handed a character rather than reading it.
    Pos Prev(int c) const {
        if (c == kEof) return Here();
        size_t at = pos_ - 1;
        if (c == '\n') return Pos{at, line_ - 1, static_cast<int>(at - LineStart(at))};
        return Pos{at, line_, col_ - 1};
    }

    // -- tokens --
    std::string ReadToken(int first) {
        std::string out(1, static_cast<char>(first));
        while (Peek() != kEof && !Delimiter(Peek())) out += static_cast<char>(Next());
        return out;
    }

    std::string Fold(std::string s) const {
        if (fold_case_)
            for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    int Symbol(const Pos &from, int first) {
        std::string tok = ReadToken(first);
        const int n = NewNode(SchemeNodeKind::Symbol, from, tok);
        out_.nodes[static_cast<size_t>(n)].name = Fold(tok);
        return n;
    }

    // -- data --
    int ReadSubexpression(const char *what, int depth) {
        const int c = NextNonWhitespace();
        if (c == kEof) {
            Error(Here(), "unexpected-eof", std::string("unexpected end of input while reading ") + what);
            return -1;
        }
        return ReadExpr(c, depth);
    }

    int ReadExpr(int ch, int depth) {
        const Pos from = start_;
        if (depth > kSchemeMaxNesting) {
            Error(from, "too-deep", "nesting too deep");
            // Skip the rest of the input: the tree would not be useful.
            while (Next() != kEof) {
            }
            return -1;
        }
        int node = neoteric_ == 0 ? ReadExprStar(ch, from, depth) : ReadNeoteric(ch, from, depth);
        return node;
    }

    int ReadNeoteric(int ch, const Pos &from, int depth) {
        int expr = ReadExprStar(ch, from, depth);
        while (expr >= 0) {
            const int p = Peek();
            if (p != '(' && p != '[' && p != '{') break;
            Next();
            const int list = NewNode(SchemeNodeKind::List, from);
            Adopt(list, expr);
            ReadParenthesized(list, p == '(' ? ')' : p == '[' ? ']' : '}', depth + 1);
            expr = list;
        }
        return expr;
    }

    int ReadExprStar(int ch, const Pos &from, int depth) {
        switch (ch) {
            case '{':
                if (curly_infix_) {
                    neoteric_++;
                    const int list = NewNode(SchemeNodeKind::List, from);
                    out_.nodes[static_cast<size_t>(list)].open = '{';
                    ReadParenthesized(list, '}', depth + 1);
                    neoteric_--;
                    return list;
                }
                return Symbol(from, ch);
            case '[':
                if (square_brackets_ || curly_infix_) {
                    const int list = NewNode(SchemeNodeKind::List, from);
                    out_.nodes[static_cast<size_t>(list)].open = '[';
                    ReadParenthesized(list, ']', depth + 1);
                    return list;
                }
                return Symbol(from, ch);
            case '(': {
                const int list = NewNode(SchemeNodeKind::List, from);
                ReadParenthesized(list, ')', depth + 1);
                return list;
            }
            case '"':
                return ReadString(from);
            case '\'':
                return PrefixNode(from, "'", "quoted expression", depth);
            case '`':
                return PrefixNode(from, "`", "quasiquoted expression", depth);
            case ',':
                if (Peek() == '@') {
                    Next();
                    return PrefixNode(from, ",@", "subexpression of ,@", depth);
                }
                return PrefixNode(from, ",", "unquoted expression", depth);
            case '#':
                return ReadSharp(from, depth);
            case ')':
                Error(from, "unexpected-close", "unexpected \")\"");
                return kStrayClose;
            case '}':
                if (curly_infix_) {
                    Error(from, "unexpected-close", "unexpected \"}\"");
                    return kStrayClose;
                }
                return Symbol(from, ch);
            case ']':
                if (square_brackets_) {
                    Error(from, "unexpected-close", "unexpected \"]\"");
                    return kStrayClose;
                }
                return Symbol(from, ch);
            default:
                break;
        }
        if (IsAsciiDigit(ch) || ch == '+' || ch == '-' || ch == '.') {
            std::string tok = ReadToken(ch);
            OutOfRange(from, tok);
            if (SchemeIsNumber(tok)) return NewNode(SchemeNodeKind::Number, from, tok);
            const int n = NewNode(SchemeNodeKind::Symbol, from, tok);
            out_.nodes[static_cast<size_t>(n)].name = Fold(tok);
            return n;
        }
        return Symbol(from, ch);
    }

    void OutOfRange(const Pos &from, const std::string &tok) {
        long e = 0;
        if (SchemeNumberOutOfRange(tok, &e))
            Error(from, "bad-number", "Value out of range: " + std::to_string(e), false);
    }

    int PrefixNode(const Pos &from, const char *prefix, const char *what, int depth) {
        const int n = NewNode(SchemeNodeKind::Prefix, from, prefix);
        const int kid = ReadSubexpression(what, depth + 1);
        if (kid == kStrayClose) {
            Finish(n);
            return n;
        }
        Adopt(n, kid);
        Finish(n);
        return n;
    }

    void ReadParenthesized(int list, char rdelim, int depth) {
        const Pos open_at{0, out_.nodes[static_cast<size_t>(list)].range.line, out_.nodes[static_cast<size_t>(list)].range.col};
        for (;;) {
            const int c = NextNonWhitespace();
            if (c == kEof) {
                Error(Here(), "unbalanced-paren",
                      std::string("unexpected end of input while searching for: ") + rdelim);
                out_.errors.back().range = SchemeRange{open_at.line, open_at.col, open_at.line, open_at.col + 1};
                out_.nodes[static_cast<size_t>(list)].closed = false;
                Finish(list);
                return;
            }
            if (c == rdelim) {
                Finish(list);
                return;
            }
            if (c == ')' || ((c == ']') && (square_brackets_ || curly_infix_)) || (c == '}' && curly_infix_)) {
                Error(start_, "mismatched-paren", std::string("mismatched close paren: ") + static_cast<char>(c));
                Finish(list);  // recovery: let it close this list
                return;
            }
            const bool dot = c == '.' && (Peek() == kEof || Delimiter(Peek()));
            const int expr = ReadExpr(c, depth);
            if (expr == kStrayClose) continue;
            if (dot && expr >= 0 && out_.nodes[static_cast<size_t>(expr)].kind == SchemeNodeKind::Symbol &&
                out_.nodes[static_cast<size_t>(expr)].text == ".") {
                // The dot itself is not a datum: drop its node (the last one made).
                if (expr == static_cast<int>(out_.nodes.size()) - 1) out_.nodes.pop_back();
                const int tail = ReadSubexpression("tail of improper list", depth);
                if (tail == kStrayClose) {
                    Finish(list);
                    return;
                }
                Adopt(list, tail);
                out_.nodes[static_cast<size_t>(list)].dotted = tail >= 0;
                const int close = NextNonWhitespace();
                if (close != rdelim) {
                    Error(start_, "missing-close", "missing close paren: " + DisplayChar(close));
                    if (close == kEof) {
                        out_.nodes[static_cast<size_t>(list)].closed = false;
                        Finish(list);
                        return;
                    }
                    // Recovery: read on to the close paren.
                    int e = ReadExpr(close, depth);
                    for (;;) {
                        if (e >= 0) Adopt(list, e);
                        const int c2 = NextNonWhitespace();
                        if (c2 == kEof || c2 == rdelim || c2 == ')' || c2 == ']' || c2 == '}') break;
                        e = ReadExpr(c2, depth);
                    }
                }
                Finish(list);
                return;
            }
            Adopt(list, expr);
        }
    }

    int ReadString(const Pos &from) {
        for (;;) {
            const int c = Next();
            if (c == kEof) {
                Error(from, "unterminated-string", "unexpected end of input while reading string");
                out_.errors.back().range = SchemeRange{from.line, from.col, from.line, from.col + 1};
                return NewNode(SchemeNodeKind::String, from, std::string(s_.substr(from.at, pos_ - from.at)));
            }
            if (c == '"') break;
            if (c != '\\') continue;
            const Pos esc = Prev(c);
            const int e = Next();
            if (e == kEof) {
                Error(from, "unterminated-string", "unexpected end of input while reading string");
                out_.errors.back().range = SchemeRange{from.line, from.col, from.line, from.col + 1};
                return NewNode(SchemeNodeKind::String, from, std::string(s_.substr(from.at, pos_ - from.at)));
            }
            switch (e) {
                case '\n':
                    if (hungry_eol_)
                        while (Peek() == '\t' || Peek() == ' ') Next();
                    break;
                case '"': case '|': case '\\': case '(': case '0': case 'f': case 'n': case 'r': case 't': case 'a':
                case 'v': case 'b':
                    break;
                case 'x':
                    if (r6rs_escapes_) HexEscapeR6rs(esc);
                    else FixedHexEscape(esc, 2);
                    break;
                case 'u':
                    FixedHexEscape(esc, 4);
                    break;
                case 'U':
                    FixedHexEscape(esc, 6);
                    break;
                default:
                    Error(esc, "bad-escape", "invalid character in escape sequence: " + WriteChar(e, WholeChar(e)));
                    break;
            }
        }
        return NewNode(SchemeNodeKind::String, from, std::string(s_.substr(from.at, pos_ - from.at)));
    }

    void FixedHexEscape(const Pos &esc, int len) {
        for (int k = 0; k < len; k++) {
            const int c = Next();
            if (HexValue(c) >= 0) continue;
            if (c == kEof) Error(esc, "bad-escape", "unexpected end of input in character escape sequence");
            else Error(esc, "bad-escape", "invalid character in escape sequence: " + WriteChar(c));
            if (c == '"') {
                // Recovery: the quote still closes the string.
                pos_--, col_--, gcol_--;
            }
            return;
        }
    }

    void HexEscapeR6rs(const Pos &esc) {
        int c = Next();
        if (HexValue(c) < 0) {
            if (c == kEof) Error(esc, "bad-escape", "unexpected end of input in character escape sequence");
            else Error(esc, "bad-escape", "invalid character in escape sequence: " + WriteChar(c));
            if (c == '"') pos_--, col_--, gcol_--;
            return;
        }
        for (;;) {
            c = Next();
            if (HexValue(c) >= 0) continue;
            if (c == ';') return;
            if (c == kEof) Error(esc, "bad-escape", "unexpected end of input in character escape sequence");
            else Error(esc, "bad-escape", "invalid character in escape sequence: " + WriteChar(c));
            if (c == '"') pos_--, col_--, gcol_--;
            return;
        }
    }

    size_t Utf8Length(const std::string &s) const {
        if (latin1_) return s.size();
        size_t n = 0;
        for (char ch : s)
            if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) n++;
        return n;
    }

    int ReadCharacter(const Pos &from) {
        const int c = Next();
        if (c == kEof) {
            Error(from, "bad-character", "unexpected end of input after #\\");
            return NewNode(SchemeNodeKind::Error, from, "#\\");
        }
        if (Delimiter(c)) return NewNode(SchemeNodeKind::Character, from, std::string(s_.substr(from.at, pos_ - from.at)));
        const std::string tok = ReadToken(c);
        const std::string text(s_.substr(from.at, pos_ - from.at));
        const size_t len = Utf8Length(tok);
        if (len == 1) return NewNode(SchemeNodeKind::Character, from, text);
        // A dotted circle (U+25CC) keeping a combining character apart.
        if (len == 2 && tok.size() >= 3 && tok.compare(tok.size() - 3, 3, "\xE2\x97\x8C") == 0)
            return NewNode(SchemeNodeKind::Character, from, text);
        auto all = [&](size_t from_i, int radix) {
            if (from_i >= tok.size()) return false;
            for (size_t i = from_i; i < tok.size(); i++)
                if (DigitValue(static_cast<unsigned char>(tok[i]), radix) < 0) return false;
            return true;
        };
        if (c >= '0' && c <= '7' && all(0, 8)) return NewNode(SchemeNodeKind::Character, from, text);
        if (c == 'x' && all(1, 16)) return NewNode(SchemeNodeKind::Character, from, text);
        static const char *const kNames[] = {
            "space", "newline", "nul", "alarm", "backspace", "tab", "linefeed", "vtab", "page", "return", "esc",
            "delete", "escape", "soh", "stx", "etx", "eot", "enq", "ack", "bel", "bs", "ht", "lf", "vt", "ff",
            "cr", "so", "si", "dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb", "can", "em", "sub", "fs",
            "gs", "rs", "us", "sp", "del", "null", "nl", "np"};
        std::string lower = tok;
        for (char &ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (const char *name : kNames)
            if (lower == name) return NewNode(SchemeNodeKind::Character, from, text);
        Error(from, "bad-character", "unknown character name " + tok);
        return NewNode(SchemeNodeKind::Character, from, text);
    }

    // Guile's maybe-read-boolean-tail: all of the tail, case-insensitively, or none of it.
    void BooleanTail(const char *tail) {
        size_t k = 0;
        while (tail[k] && PeekAt(k) != kEof && std::tolower(PeekAt(k)) == tail[k]) k++;
        if (tail[k] == 0)
            for (size_t i = 0; i < k; i++) Next();
    }

    // #s8( #u8( #f64( #2( #@1( ...: Guile's read-array, for its errors.
    int ReadArray(const Pos &from, int ch, int depth) {
        auto read_decimal = [&](int c, long alt, long *value) {
            long sign = 1;
            if (c == '-') {
                sign = -1;
                c = Next();
            }
            bool any = false;
            long res = 0;
            while (IsAsciiDigit(c)) {
                any = true;
                if (res < 100000000) res = res * 10 + (c - '0');
                c = Next();
            }
            *value = any ? res * sign : alt;
            return c;
        };
        long rank = 1;
        int c = read_decimal(ch, 1, &rank);
        if (rank < 0) {
            Error(from, "bad-array", "array rank must be non-negative");
            return NewNode(SchemeNodeKind::Error, from, "#");
        }
        if (c == kEof) {
            Error(from, "unexpected-eof", "unexpected end of input while reading array");
            return NewNode(SchemeNodeKind::Error, from, "#");
        }
        std::string tag;
        while (c != '(' && c != '@' && c != ':') {
            if (c == kEof) {
                Error(from, "unexpected-eof", "unexpected end of input while reading array");
                return NewNode(SchemeNodeKind::Error, from, "#");
            }
            tag += static_cast<char>(c);
            c = Next();
        }
        long dims = 0;
        while (c == '@' || c == ':') {
            long lbnd = 0, len = -1;
            if (c == '@') c = read_decimal(Next(), 0, &lbnd);
            if (c == ':') {
                c = read_decimal(Next(), 0, &len);
                if (len < 0) {
                    Error(from, "bad-array", "array length must be non-negative");
                    return NewNode(SchemeNodeKind::Error, from, "#");
                }
            }
            if (c == kEof) {
                Error(from, "unexpected-eof", "unexpected end of input while reading array");
                return NewNode(SchemeNodeKind::Error, from, "#");
            }
            dims++;
        }
        if (c != '(') {
            Error(from, "bad-array", "missing '(' in vector or array literal");
            return NewNode(SchemeNodeKind::Error, from, "#");
        }
        const int n = NewNode(SchemeNodeKind::Bytevector, from, std::string(s_.substr(from.at, pos_ - from.at)));
        ReadParenthesized(n, ')', depth + 1);
        const size_t elems = out_.nodes[static_cast<size_t>(n)].kids.size();
        if (!out_.nodes[static_cast<size_t>(n)].closed) return n;
        if (rank == 0 && elems == 0) Error(Here(), "bad-array", "too few elements in array literal, need 1");
        else if (rank == 0 && elems > 1) Error(Here(), "bad-array", "too many elements in array literal, need 1");
        else if (dims > 0 && dims != rank)
            Error(Here(), "bad-array", "the number of shape specifications must match the array rank");
        else {
            static const char *const kTags[] = {"", "vu8", "u8", "s8", "u16", "s16", "u32", "s32", "u64",
                                                "s64", "f32", "f64", "c32", "c64", "b", "a"};
            bool ok = false;
            for (const char *t : kTags) ok = ok || tag == t;
            if (!ok)
                Error(from, "bad-array",
                      "Wrong type argument in position 1 (expecting array type): " + tag, false);
            else if (rank > 0)
                CheckArrayElements(n, static_cast<int>(rank), tag);
        }
        return n;
    }

    // list->typed-array's checks, which Guile reports without a position:
    // a rank-N array's elements nest N lists deep, all of one length per
    // dimension, and a typed vector's elements are numbers of its type.
    void CheckArrayElements(int node, int rank, const std::string &tag) {
        std::vector<size_t> lengths;
        std::string why;
        CheckDimension(node, 0, rank, tag, lengths, why);
        if (!why.empty()) Error(Here(), "bad-array", why, false);
    }

    void CheckDimension(int node, int dim, int rank, const std::string &tag, std::vector<size_t> &lengths,
                        std::string &why) {
        if (!why.empty()) return;
        const SchemeNode &n = out_.nodes[static_cast<size_t>(node)];
        if (n.dotted) {
            why = "Wrong type argument in position 3: " + Display(node);
            return;
        }
        if (static_cast<size_t>(dim) == lengths.size()) lengths.push_back(n.kids.size());
        else if (lengths[static_cast<size_t>(dim)] != n.kids.size()) {
            why = "too many elements for array dimension " + std::to_string(dim) + ", want " +
                  std::to_string(lengths[static_cast<size_t>(dim)]);
            return;
        }
        for (int kid : n.kids) {
            const SchemeNode &k = out_.nodes[static_cast<size_t>(kid)];
            if (dim + 1 < rank) {
                if (k.kind != SchemeNodeKind::List) {
                    why = "Wrong type argument in position 3: " + Display(kid);
                    return;
                }
                CheckDimension(kid, dim + 1, rank, tag, lengths, why);
                if (!why.empty()) return;
            } else if (!ElementFits(k, tag)) {
                why = "Wrong type argument: " + Display(kid);
                return;
            }
        }
    }

    // Whether a datum can be an element of a vector with this type tag.
    static bool ElementFits(const SchemeNode &k, const std::string &tag) {
        if (tag.empty()) return true;
        if (tag == "a") return k.kind == SchemeNodeKind::Character;
        if (tag == "b") return k.kind == SchemeNodeKind::Boolean;
        if (k.kind != SchemeNodeKind::Number) return false;
        if (tag[0] == 'c') return true;
        std::string t = k.text;
        for (char &c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool complex = t.find('i') != std::string::npos && t.find("inf") == std::string::npos &&
                             t.find("#i") == std::string::npos;
        if (tag[0] == 'f') return !complex || t.back() != 'i';
        // An exact integer in range: decimal (or #x/#b/#o/#d) digits, no point or exponent.
        int radix = 10;
        size_t i = 0;
        bool inexact = false;
        while (i + 1 < t.size() && t[i] == '#') {
            const char c = t[i + 1];
            if (c == 'x') radix = 16;
            else if (c == 'b') radix = 2;
            else if (c == 'o') radix = 8;
            else if (c == 'i') inexact = true;
            i += 2;
        }
        if (inexact) return false;
        bool neg = false;
        if (i < t.size() && (t[i] == '+' || t[i] == '-')) neg = t[i++] == '-';
        if (i >= t.size()) return false;
        double v = 0;
        for (; i < t.size(); i++) {
            const int d = DigitValue(static_cast<unsigned char>(t[i]), radix);
            if (d < 0) return false;
            v = v * radix + d;
        }
        if (neg) v = -v;
        const bool is_signed = tag[0] == 's';
        const int bits = std::atoi(tag.c_str() + (tag == "vu8" ? 2 : 1));
        const double span = bits >= 64 ? 1.8446744073709552e19 : static_cast<double>(1ULL << bits);
        if (is_signed) return v >= -span / 2 && v < span / 2;
        return v >= 0 && v < span;
    }

    int ReadSharp(const Pos &from, int depth) {
        const int ch = Next();
        if (ch == kEof) {
            Error(from, "unexpected-eof", "unexpected end of input after #");
            return NewNode(SchemeNodeKind::Error, from, "#");
        }
        switch (ch) {
            // Guix's reader extensions, as (guix gexp) installs them.
            case '~':
                return HashProcPrefix(from, "#~", depth);
            case '$':
            case '+':
                if (Peek() == '@') {
                    Next();
                    return HashProcPrefix(from, ch == '$' ? "#$@" : "#+@", depth);
                }
                return HashProcPrefix(from, ch == '$' ? "#$" : "#+", depth);
            case '.':
                Error(from, "read-eval", "#. read expansion found and read-eval? is #f.", false);
                return NewNode(SchemeNodeKind::Error, from, "#.");
            case '\\':
                return ReadCharacter(from);
            case '(': {
                const int v = NewNode(SchemeNodeKind::Vector, from, "#(");
                ReadParenthesized(v, ')', depth + 1);
                return v;
            }
            case 's': case 'u': case 'c':
                return ReadArray(from, ch, depth);
            case 'f':
                if (Peek() == '3' || Peek() == '6') return ReadArray(from, 'f', depth);
                BooleanTail("alse");
                return NewNode(SchemeNodeKind::Boolean, from, std::string(s_.substr(from.at, pos_ - from.at)));
            case 'v': {
                const char *want = "u8(";
                for (int k = 0; k < 3; k++) {
                    const int c = Next();
                    if (c != want[k]) {
                        Error(from, "bad-bytevector", "invalid bytevector prefix");
                        return NewNode(SchemeNodeKind::Error, from, std::string(s_.substr(from.at, pos_ - from.at)));
                    }
                }
                const int n = NewNode(SchemeNodeKind::Bytevector, from, "#vu8(");
                ReadParenthesized(n, ')', depth + 1);
                if (out_.nodes[static_cast<size_t>(n)].closed) CheckArrayElements(n, 1, "vu8");
                return n;
            }
            case '*': {
                while (Peek() == '0' || Peek() == '1') Next();
                return NewNode(SchemeNodeKind::BitVector, from, std::string(s_.substr(from.at, pos_ - from.at)));
            }
            case 't': case 'T':
                BooleanTail("rue");
                return NewNode(SchemeNodeKind::Boolean, from, std::string(s_.substr(from.at, pos_ - from.at)));
            case 'F':
                BooleanTail("alse");
                return NewNode(SchemeNodeKind::Boolean, from, std::string(s_.substr(from.at, pos_ - from.at)));
            case ':': {
                const int sym = ReadSubexpression("keyword", depth + 1);
                if (sym == kStrayClose) return kStrayClose;
                if (sym < 0) return NewNode(SchemeNodeKind::Error, from, "#:");
                const SchemeNode &sn = out_.nodes[static_cast<size_t>(sym)];
                if (sn.kind != SchemeNodeKind::Symbol) {
                    Error(from, "bad-keyword", "keyword prefix #: not followed by a symbol: " + Display(sym));
                    return sym;
                }
                // The keyword replaces the symbol node.
                SchemeNode &k = out_.nodes[static_cast<size_t>(sym)];
                k.kind = SchemeNodeKind::Keyword;
                k.range.line = from.line;
                k.range.col = from.col;
                k.text = std::string(s_.substr(from.at, pos_ - from.at));
                return sym;
            }
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': case '@':
                return ReadArray(from, ch, depth);
            case 'i': case 'e': case 'b': case 'B': case 'o': case 'O': case 'd': case 'D': case 'x': case 'X':
            case 'I': case 'E': {
                const std::string tok = "#" + ReadToken(ch);
                long e = 0;
                if (SchemeNumberOutOfRange(tok, &e)) OutOfRange(from, tok);
                else if (!SchemeIsNumber(tok)) Error(from, "bad-number", "unknown # object: " + WriteString(tok));
                return NewNode(SchemeNodeKind::Number, from, tok);
            }
            case '{':
                return ReadExtendedSymbol(from);
            case '\'':
                return PrefixNode(from, "#'", "syntax expression", depth);
            case '`':
                return PrefixNode(from, "#`", "quasisyntax expression", depth);
            case ',':
                if (Peek() == '@') {
                    Next();
                    return PrefixNode(from, "#,@", "unsyntax-splicing expression", depth);
                }
                return PrefixNode(from, "#,", "unsyntax expression", depth);
            case 'n': {
                const std::string tok = Fold(ReadToken('n'));
                if (tok != "nil") Error(from, "bad-nil", "unexpected input while reading #nil: " + DisplaySymbol(tok));
                return NewNode(SchemeNodeKind::Nil, from, std::string(s_.substr(from.at, pos_ - from.at)));
            }
            default:
                break;
        }
        std::string chars = "#";
        chars += static_cast<char>(ch);
        if (!latin1_ && ch >= 0xC0)  // the whole character, not its first byte
            while ((Peek() & 0xC0) == 0x80) chars += static_cast<char>(Next());
        Error(from, "unknown-dispatch", "Unknown # object: " + WriteString(chars));
        // Recovery: the rest of the token goes with it.
        while (Peek() != kEof && !Delimiter(Peek())) Next();
        return NewNode(SchemeNodeKind::Error, from, std::string(s_.substr(from.at, pos_ - from.at)));
    }

    // A reader extension: the datum after it is read by a fresh top-level
    // `read`, which returns the end-of-file object (no error) at the end.
    int HashProcPrefix(const Pos &from, const char *prefix, int depth) {
        const int n = NewNode(SchemeNodeKind::Prefix, from, prefix);
        const int c = NextNonWhitespace();
        if (c == kEof) {
            Finish(n);
            return n;
        }
        const int kid = ReadExpr(c, depth + 1);
        if (kid >= 0) Adopt(n, kid);
        Finish(n);
        return n;
    }

    int ReadExtendedSymbol(const Pos &from) {
        std::string name;
        bool saw_brace = false;
        for (;;) {
            int c = Next();
            if (c == kEof) {
                Error(from, "unterminated-symbol", "end of input while reading symbol");
                break;
            }
            if (saw_brace) {
                if (c == '#') break;
                name += '}';
                saw_brace = false;
            }
            if (c == '}') {
                saw_brace = true;
                continue;
            }
            if (c == '\\') {
                c = Next();
                if (c == kEof) {
                    Error(from, "unterminated-symbol", "end of input while reading symbol");
                    break;
                }
                if (c == 'x') {
                    HexEscapeR6rs(from);
                    continue;
                }
            }
            name += static_cast<char>(c);
        }
        const int n = NewNode(SchemeNodeKind::Symbol, from, std::string(s_.substr(from.at, pos_ - from.at)));
        out_.nodes[static_cast<size_t>(n)].name = name;
        return n;
    }
};

}  // namespace

bool SchemeRangeContains(const SchemeRange &r, int line, int col) {
    if (line < r.line || line > r.end_line) return false;
    if (line == r.line && col < r.col) return false;
    if (line == r.end_line && col > r.end_col) return false;
    return true;
}

const SchemeNode &SchemeReadResult::at(int index) const {
    static const SchemeNode kEmpty;
    if (index < 0 || static_cast<size_t>(index) >= nodes.size()) return kEmpty;
    return nodes[static_cast<size_t>(index)];
}

SchemeReadResult SchemeRead(std::string_view text) {
    SchemeReadResult out;
    Reader r(text, out);
    r.ReadAll();
    return out;
}

bool SchemeIsNumber(std::string_view token) { return ScanNumber(token, 10); }

bool SchemeNumberOutOfRange(std::string_view token, long *exponent) {
    long e = 0;
    ScanNumber(token, 10, &e);
    if (e == 0) return false;
    if (exponent) *exponent = e;
    return true;
}
