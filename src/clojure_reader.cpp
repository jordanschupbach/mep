#include "clojure_reader.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

// Follows clojure.lang.LispReader (Clojure 1.12): the macro table, what
// ends a token, matchSymbol/matchNumber, and each reader macro's checks,
// with their messages. Where the JVM reader throws, this one records the
// error and carries on.

bool CljRangeContains(const CljRange &r, int line, int col) {
    if (line < r.line || line > r.end_line) return false;
    if (line == r.line && col < r.col) return false;
    if (line == r.end_line && col >= r.end_col) return false;
    return true;
}

bool CljRangeTouches(const CljRange &r, int line, int col) {
    return CljRangeContains(r, line, col) || (line == r.end_line && col == r.end_col);
}

const CljNode &CljReadResult::at(int index) const {
    static const CljNode kEmpty;
    if (index < 0 || static_cast<size_t>(index) >= nodes.size()) return kEmpty;
    return nodes[static_cast<size_t>(index)];
}

bool CljIsSymbol(const CljNode &n, const char *name) {
    return n.kind == CljNodeKind::Symbol && n.name == name && (n.ns.empty() || n.ns == "clojure.core");
}

namespace {

// Character.isWhitespace for the ASCII range, plus the comma.
bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' || c == ',' || (c >= 0x1c && c <= 0x1f); }

// The macros that end a token (LispReader.isTerminatingMacro: every macro
// but `#`, `'` and `%`).
bool IsTerminating(char c) {
    switch (c) {
        case '"':
        case ';':
        case '@':
        case '^':
        case '`':
        case '~':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '\\': return true;
        default: return false;
    }
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
// Every reader macro character (LispReader.isMacro), terminating or not.
bool IsMacro(char c) { return IsTerminating(c) || c == '#' || c == '\'' || c == '%'; }
bool IsCloser(char c) { return c == ')' || c == ']' || c == '}'; }

int DigitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

// LispReader.symbolPat, [:]?([\D&&[^/]].*/)?(/|[\D&&[^/]][^/]*), matched
// from `start` to the end of `s`.
bool MatchesSymbolBody(const std::string &s, size_t start) {
    const size_t n = s.size();
    if (start >= n) return false;
    auto name_ok = [&](size_t a) {
        if (a >= n) return false;
        if (s.compare(a, std::string::npos, "/") == 0) return true;
        if (IsDigit(s[a]) || s[a] == '/') return false;
        return s.find('/', a) == std::string::npos;
    };
    if (name_ok(start)) return true;
    // An ns part: starts with a non-digit, non-slash; ends with a `/`.
    if (IsDigit(s[start]) || s[start] == '/') return false;
    for (size_t k = n; k-- > start + 1;) {
        if (s[k - 1] == '/' && name_ok(k)) return true;
        if (s[k] == '/' && k > start && name_ok(k + 1)) return true;
    }
    return false;
}

// The `ns/` group the regex would capture (greedy), for the `:/` check.
std::string SymbolNsGroup(const std::string &s, size_t start) {
    const size_t n = s.size();
    for (size_t k = n; k-- > start;) {
        if (s[k] != '/') continue;
        const size_t a = k + 1;
        const bool name_ok = (a < n && s.compare(a, std::string::npos, "/") == 0) ||
                             (a < n && !IsDigit(s[a]) && s[a] != '/' && s.find('/', a) == std::string::npos);
        if (name_ok && k > start) return s.substr(start, k + 1 - start);
    }
    return "";
}

// LispReader.matchSymbol: does `s` read as a symbol or keyword?
bool MatchSymbol(const std::string &s) {
    size_t start = 0;
    bool ok = false;
    if (!s.empty() && s[0] == ':' && MatchesSymbolBody(s, 1)) {
        start = 1;
        ok = true;
    } else if (MatchesSymbolBody(s, 0)) {
        start = 0;
        ok = true;
    }
    if (!ok) return false;
    const std::string ns = SymbolNsGroup(s, start);
    const std::string name = ns.empty() ? s.substr(start) : s.substr(start + ns.size());
    if (!ns.empty() && ns.size() >= 2 && ns.compare(ns.size() - 2, 2, ":/") == 0) return false;
    if (!name.empty() && name.back() == ':') return false;
    if (s.find("::", 1) != std::string::npos) return false;
    return true;
}

// LispReader.matchNumber: "" if `s` is a valid number, else the error message.
std::string NumberError(const std::string &s) {
    size_t i = 0;
    const size_t n = s.size();
    if (i < n && (s[i] == '+' || s[i] == '-')) i++;
    // intPat
    {
        size_t j = i;
        std::string digits;
        int radix = 10;
        bool matched = false, bad_octalish = false;
        if (j < n && s[j] == '0' && (j + 1 == n || (j + 2 == n && s[j + 1] == 'N'))) {
            return "";
        }
        if (j + 1 < n && s[j] == '0' && (s[j + 1] == 'x' || s[j + 1] == 'X')) {
            size_t k = j + 2;
            while (k < n && DigitValue(s[k]) < 16) k++;
            if (k > j + 2 && (k == n || (k + 1 == n && s[k] == 'N'))) {
                matched = true;
                radix = 16;
                digits = s.substr(j + 2, k - j - 2);
            }
        }
        if (!matched && j < n && s[j] == '0') {
            size_t k = j + 1;
            while (k < n && s[k] >= '0' && s[k] <= '7') k++;
            if (k > j + 1 && (k == n || (k + 1 == n && s[k] == 'N'))) {
                matched = true;
                radix = 8;
                digits = s.substr(j + 1, k - j - 1);
            } else {
                size_t m = j + 1;
                while (m < n && IsDigit(s[m])) m++;
                if (m > j + 1 && (m == n || (m + 1 == n && s[m] == 'N'))) bad_octalish = true;
            }
        }
        if (!matched && !bad_octalish && j < n && s[j] >= '1' && s[j] <= '9') {
            size_t k = j;
            while (k < n && IsDigit(s[k])) k++;
            if (k == n || (k + 1 == n && s[k] == 'N')) {
                matched = true;
                digits = s.substr(j, k - j);
            } else if ((k - j == 1 || k - j == 2) && k < n && (s[k] == 'r' || s[k] == 'R')) {
                size_t m = k + 1;
                while (m < n && (IsDigit(s[m]) || (s[m] >= 'a' && s[m] <= 'z') || (s[m] >= 'A' && s[m] <= 'Z'))) m++;
                if (m > k + 1 && m == n) {
                    // A trailing N is one of the digits here, as the regex reads it.
                    matched = true;
                    radix = std::stoi(s.substr(j, k - j));
                    digits = s.substr(k + 1);
                }
            }
        }
        if (bad_octalish) return "Invalid number: " + s;
        if (matched) {
            if (radix > 36) return "Radix out of range";
            for (char c : digits) {
                if (DigitValue(c) >= radix) {
                    return "For input string: \"" + digits + "\"" + (radix == 10 ? "" : " under radix " + std::to_string(radix));
                }
            }
            return "";
        }
    }
    // floatPat: [-+]?[0-9]+(\.[0-9]*)?([eE][-+]?[0-9]+)?(M)?
    {
        size_t j = i;
        size_t k = j;
        while (k < n && IsDigit(s[k])) k++;
        if (k > j) {
            if (k < n && s[k] == '.') {
                k++;
                while (k < n && IsDigit(s[k])) k++;
            }
            if (k < n && (s[k] == 'e' || s[k] == 'E')) {
                size_t m = k + 1;
                if (m < n && (s[m] == '+' || s[m] == '-')) m++;
                const size_t ds = m;
                while (m < n && IsDigit(s[m])) m++;
                if (m > ds) k = m;
            }
            if (k < n && s[k] == 'M') k++;
            if (k == n) return "";
        }
    }
    // ratioPat: [-+]?[0-9]+/[0-9]+
    {
        size_t j = i;
        size_t k = j;
        while (k < n && IsDigit(s[k])) k++;
        if (k > j && k < n && s[k] == '/') {
            size_t m = k + 1;
            while (m < n && IsDigit(s[m])) m++;
            if (m > k + 1 && m == n) {
                const std::string den = s.substr(k + 1);
                if (den.find_first_not_of('0') == std::string::npos) return "Divide by zero";
                return "";
            }
        }
    }
    return "Invalid number: " + s;
}

constexpr int kEof = -1;
constexpr int kCloser = -2;
constexpr int kNothing = -3;  // a comment or a discarded form: read on

class Reader {
public:
    explicit Reader(const std::vector<std::string> &lines, bool recover_col0) : recover_col0_(recover_col0) {
        for (size_t k = 0; k < lines.size(); k++) {
            starts_.push_back(s_.size());
            s_ += lines[k];
            if (k + 1 < lines.size()) s_ += '\n';
        }
        if (starts_.empty()) starts_.push_back(0);
    }

    CljReadResult Run() {
        for (;;) {
            const int k = Next();
            if (k == kEof) break;
            if (k == kCloser) {
                const char c = s_[i_];
                Error(i_, i_ + 1, "unmatched-delimiter", std::string("Unmatched delimiter: ") + c);
                i_++;
                continue;
            }
            if (k >= 0) res_.top.push_back(k);
        }
        return std::move(res_);
    }

    bool hit_eof_in_collection() const { return eof_in_collection_; }

private:
    std::string s_;
    std::vector<size_t> starts_;
    size_t i_ = 0;
    CljReadResult res_;
    int depth_ = 0;
    int fn_depth_ = 0;
    bool recover_col0_ = false;
    bool eof_in_collection_ = false;
    bool unwinding_ = false;  // closing every open collection at a column-0 `(def...`
    std::vector<char> closers_;
    size_t top_start_ = 0;

    // --- positions -------------------------------------------------------

    bool Eof(size_t at) const { return at >= s_.size(); }
    bool Eof() const { return Eof(i_); }
    char C(size_t ahead = 0) const { return i_ + ahead < s_.size() ? s_[i_ + ahead] : '\0'; }

    void LineCol(size_t off, int *line, int *col) const {
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), off);
        const size_t l = static_cast<size_t>(it - starts_.begin()) - 1;
        *line = static_cast<int>(l);
        *col = static_cast<int>(off - starts_[l]);
    }

    CljRange Range(size_t a, size_t b) const {
        CljRange r;
        LineCol(a, &r.line, &r.col);
        LineCol(b, &r.end_line, &r.end_col);
        return r;
    }

    int LineOf(size_t off) const {
        int l = 0, c = 0;
        LineCol(off, &l, &c);
        return l;
    }

    void Error(size_t a, size_t b, const char *code, const std::string &message) {
        CljReadError e;
        e.range = Range(a, b);
        e.code = code;
        e.message = message;
        res_.errors.push_back(e);
    }

    int New(CljNodeKind kind, size_t a, size_t b, std::string text = std::string()) {
        CljNode n;
        n.kind = kind;
        n.range = Range(a, b);
        n.text = std::move(text);
        res_.nodes.push_back(std::move(n));
        return static_cast<int>(res_.nodes.size()) - 1;
    }

    CljNode &N(int k) { return res_.nodes[static_cast<size_t>(k)]; }

    void Adopt(int parent, int kid) {
        N(kid).parent = parent;
        N(parent).kids.push_back(kid);
    }

    void SetEnd(int k, size_t b) {
        int l = 0, c = 0;
        LineCol(b, &l, &c);
        N(k).range.end_line = l;
        N(k).range.end_col = c;
    }

    size_t StartOff(int k) const {
        const CljRange &r = res_.nodes[static_cast<size_t>(k)].range;
        return starts_[static_cast<size_t>(r.line)] + static_cast<size_t>(r.col);
    }

    void SkipWs() {
        while (!Eof() && IsWs(C())) i_++;
    }

    void Comment() {
        const size_t a = i_;
        while (!Eof() && C() != '\n') i_++;
        CljComment c;
        c.range = Range(a, i_);
        c.text = s_.substr(a, i_ - a);
        res_.comments.push_back(c);
    }

    // The characters of a token, from the current one: the first is taken
    // whatever it is, then up to whitespace or a terminating macro.
    std::string Token() {
        const size_t a = i_;
        if (!Eof()) i_++;
        while (!Eof() && !IsWs(C()) && !IsTerminating(C())) i_++;
        return s_.substr(a, i_ - a);
    }

    // --- forms -----------------------------------------------------------

    // The next form, or kEof / kCloser (the closer not consumed).
    int Next() {
        for (;;) {
            const int k = One();
            if (k == kNothing) continue;
            return k;
        }
    }

    int One() {
        SkipWs();
        if (Eof()) return kEof;
        const size_t a = i_;
        const char c = C();
        switch (c) {
            case ';':
                Comment();
                return kNothing;
            case ')':
            case ']':
            case '}': return kCloser;
            case '(': return Collection(CljNodeKind::List, a, 1, ')');
            case '[': return Collection(CljNodeKind::Vector, a, 1, ']');
            case '{': return Collection(CljNodeKind::Map, a, 1, '}');
            case '"': return String();
            case '\'': return Prefix(CljNodeKind::Quote, 1);
            case '`': return SyntaxQuote();
            case '~': return C(1) == '@' ? Prefix(CljNodeKind::UnquoteSplicing, 2) : Prefix(CljNodeKind::Unquote, 1);
            case '@': return Prefix(CljNodeKind::Deref, 1);
            case '^': return Meta(1);
            case '\\': return Character();
            case '#': return Dispatch();
            default: break;
        }
        if (IsDigit(c) || ((c == '+' || c == '-') && IsDigit(C(1)))) {
            // LispReader.readNumber stops at any macro character, `#`, `'`
            // and `%` included (a symbol's token does not): `3#?(...)` is a
            // 3 and a reader conditional.
            i_++;
            while (!Eof() && !IsWs(C()) && !IsTerminating(C()) && C() != '#' && C() != '\'' && C() != '%') i_++;
            const std::string t = s_.substr(a, i_ - a);
            const std::string err = NumberError(t);
            if (!err.empty()) {
                Error(a, i_, "invalid-number", err);
                return New(CljNodeKind::Error, a, i_, t);
            }
            return New(CljNodeKind::Number, a, i_, t);
        }
        if (c == '%' && fn_depth_ > 0) return Arg();
        return Atom(a, Token());
    }

    int Atom(size_t a, const std::string &t) {
        if (t == "nil") return New(CljNodeKind::Nil, a, i_, t);
        if (t == "true" || t == "false") return New(CljNodeKind::Bool, a, i_, t);
        if (!MatchSymbol(t)) {
            Error(a, i_, "invalid-token", "Invalid token: " + t);
            return New(CljNodeKind::Error, a, i_, t);
        }
        const bool kw = t[0] == ':';
        const int k = New(kw ? CljNodeKind::Keyword : CljNodeKind::Symbol, a, i_, t);
        std::string body = t;
        if (kw) {
            body = t.substr(1);
            if (!body.empty() && body[0] == ':') {
                N(k).auto_resolve = true;
                body = body.substr(1);
            }
        }
        const size_t slash = body.find('/');
        if (slash == std::string::npos || body == "/") {
            N(k).name = body;
        } else {
            N(k).ns = body.substr(0, slash);
            N(k).name = body.substr(slash + 1);
        }
        return k;
    }

    // %, %&, %n inside #(...).
    int Arg() {
        const size_t a = i_;
        const std::string t = Token();
        bool ok = t == "%" || t == "%&";
        if (!ok && t.size() > 1 && IsDigit(t[1])) {
            ok = t.find_first_not_of("0123456789", 1) == std::string::npos;
            if (!ok) {
                Error(a, i_, "invalid-number", "Invalid number: " + t.substr(1));
                return New(CljNodeKind::Error, a, i_, t);
            }
        }
        if (!ok) {
            Error(a, i_, "arg-literal", "arg literal must be %, %& or %integer");
            return New(CljNodeKind::Error, a, i_, t);
        }
        const int k = New(CljNodeKind::Symbol, a, i_, t);
        N(k).name = t;
        return k;
    }

    int String() {
        const size_t a = i_;
        i_++;
        std::string value;
        while (!Eof() && C() != '"') {
            if (C() != '\\') {
                value += C();
                i_++;
                continue;
            }
            const size_t esc = i_;
            i_++;
            if (Eof()) break;
            const char e = C();
            i_++;
            switch (e) {
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case 'n': value += '\n'; break;
                case '\\': value += '\\'; break;
                case '"': value += '"'; break;
                case 'b': value += '\b'; break;
                case 'f': value += '\f'; break;
                case 'u': {
                    // Exactly four hex digits; a digit that is not one stops it.
                    if (Eof() || DigitValue(C()) >= 16) {
                        Error(esc, i_ + (Eof() ? 0 : 1), "string-escape",
                              std::string("Invalid unicode escape: \\u") + (Eof() ? std::string() : std::string(1, C())));
                        break;
                    }
                    int count = 0;
                    unsigned code = 0;
                    bool bad = false;
                    while (count < 4) {
                        if (Eof() || IsWs(C()) || IsMacro(C())) {
                            Error(esc, i_, "string-escape",
                                  "Invalid character length: " + std::to_string(count) + ", should be: 4");
                            bad = true;
                            break;
                        }
                        if (DigitValue(C()) >= 16) {
                            Error(esc, i_ + 1, "string-escape", std::string("Invalid digit: ") + C());
                            bad = true;
                            break;
                        }
                        code = code * 16 + static_cast<unsigned>(DigitValue(C()));
                        i_++;
                        count++;
                    }
                    if (!bad) AppendUtf8(&value, code);
                    break;
                }
                default:
                    if (IsDigit(e)) {
                        // Up to three octal digits, ended by whitespace or a
                        // macro character; anything else is not a digit.
                        if (e > '7') {
                            Error(esc, i_, "string-escape", std::string("Invalid digit: ") + e);
                            break;
                        }
                        unsigned code = static_cast<unsigned>(e - '0');
                        int count = 1;
                        bool bad = false;
                        while (count < 3 && !Eof() && !IsWs(C()) && !IsMacro(C())) {
                            if (C() < '0' || C() > '7') {
                                Error(esc, i_ + 1, "string-escape", std::string("Invalid digit: ") + C());
                                bad = true;
                                break;
                            }
                            code = code * 8 + static_cast<unsigned>(C() - '0');
                            i_++;
                            count++;
                        }
                        if (bad) break;
                        if (code > 0377) {
                            Error(esc, i_, "string-escape", "Octal escape sequence must be in range [0, 377].");
                        }
                        AppendUtf8(&value, code);
                    } else {
                        Error(esc, i_, "string-escape", std::string("Unsupported escape character: \\") + e);
                    }
            }
        }
        if (Eof()) {
            Error(a, a + 1, "eof-string", "EOF while reading string");
            const int k = New(CljNodeKind::String, a, i_, s_.substr(a));
            N(k).name = value;
            return k;
        }
        i_++;
        const int k = New(CljNodeKind::String, a, i_, s_.substr(a, i_ - a));
        N(k).name = value;
        return k;
    }

    static void AppendUtf8(std::string *out, unsigned cp) {
        if (cp < 0x80) {
            *out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            *out += static_cast<char>(0xC0 | (cp >> 6));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            *out += static_cast<char>(0xE0 | (cp >> 12));
            *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    int Character() {
        const size_t a = i_;
        i_++;
        if (Eof()) {
            Error(a, i_, "invalid-character", "EOF while reading character");
            return New(CljNodeKind::Error, a, i_, "\\");
        }
        const std::string t = Token();
        auto fail = [&](const std::string &msg) {
            Error(a, i_, "invalid-character", msg);
            return New(CljNodeKind::Error, a, i_, "\\" + t);
        };
        if (t.size() == 1) return New(CljNodeKind::Character, a, i_, "\\" + t);
        // A multi-byte UTF-8 character is one character.
        const auto lead = static_cast<unsigned char>(t[0]);
        const size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (len > 1 && t.size() == len) return New(CljNodeKind::Character, a, i_, "\\" + t);
        static const char *const names[] = {"newline", "space", "tab", "backspace", "formfeed", "return"};
        for (const char *nm : names) {
            if (t == nm) return New(CljNodeKind::Character, a, i_, "\\" + t);
        }
        if (t[0] == 'u') {
            if (t.size() != 5) return fail("Invalid unicode character: \\" + t);
            unsigned code = 0;
            for (size_t k = 1; k < 5; k++) {
                const int d = DigitValue(t[k]);
                if (d >= 16) return fail(std::string("Invalid digit: ") + t[k]);
                code = code * 16 + static_cast<unsigned>(d);
            }
            if (code >= 0xD800 && code <= 0xDFFF) return fail("Invalid character constant: \\u" + t.substr(1));
            return New(CljNodeKind::Character, a, i_, "\\" + t);
        }
        if (t[0] == 'o') {
            const size_t digits = t.size() - 1;
            if (digits > 3) return fail("Invalid octal escape sequence length: " + std::to_string(digits));
            unsigned code = 0;
            for (size_t k = 1; k < t.size(); k++) {
                const int d = DigitValue(t[k]);
                if (d >= 8) return fail(std::string("Invalid digit: ") + t[k]);
                code = code * 8 + static_cast<unsigned>(d);
            }
            if (code > 0377) return fail("Octal escape sequence must be in range [0, 377].");
            return New(CljNodeKind::Character, a, i_, "\\" + t);
        }
        return fail("Unsupported character: \\" + t);
    }

    // A reader macro that takes the next form: ' ` ~ ~@ @ #' #=.
    int Prefix(CljNodeKind kind, size_t len) {
        const size_t a = i_;
        i_ += len;
        const int k = New(kind, a, i_, s_.substr(a, len));
        const int x = Operand(a, len);
        if (x >= 0) {
            Adopt(k, x);
            SetEnd(k, EndOff(x));
        }
        return k;
    }

    size_t EndOff(int k) const {
        const CljRange &r = res_.nodes[static_cast<size_t>(k)].range;
        return starts_[static_cast<size_t>(r.end_line)] + static_cast<size_t>(r.end_col);
    }

    // The form a reader macro applies to; an error when there is none.
    int Operand(size_t a, size_t len) {
        if (++depth_ > kCljMaxNesting) {
            depth_--;
            Error(a, a + len, "too-deep", "Form nested too deeply");
            return -1;
        }
        const int x = Next();
        depth_--;
        if (x == kEof) {
            Error(a, a + len, "eof", "EOF while reading");
            return -1;
        }
        if (x == kCloser) {
            Error(i_, i_ + 1, "unmatched-delimiter", std::string("Unmatched delimiter: ") + C());
            return -1;
        }
        return x;
    }

    int SyntaxQuote() {
        const size_t a = i_;
        const int k = Prefix(CljNodeKind::SyntaxQuote, 1);
        if (!N(k).kids.empty() && N(N(k).kids[0]).kind == CljNodeKind::UnquoteSplicing) {
            Error(a, EndOff(k), "splice", "splice not in list");
        }
        return k;
    }

    // ^meta form, #^meta form.
    int Meta(size_t len) {
        const size_t a = i_;
        i_ += len;
        const int m = Operand(a, len);
        if (m < 0) return kNothing;
        const CljNodeKind mk = N(m).kind;
        if (mk != CljNodeKind::Symbol && mk != CljNodeKind::Keyword && mk != CljNodeKind::String &&
            mk != CljNodeKind::Map && mk != CljNodeKind::Vector && mk != CljNodeKind::Error) {
            Error(StartOff(m), EndOff(m), "bad-metadata", "Metadata must be Symbol,Keyword,String,Vector or Map");
        }
        const int x = Operand(a, len);
        if (x < 0) return m;  // keep what was read
        switch (N(x).kind) {
            case CljNodeKind::Number:
            case CljNodeKind::String:
            case CljNodeKind::Keyword:
            case CljNodeKind::Character:
            case CljNodeKind::Nil:
            case CljNodeKind::Bool:
            case CljNodeKind::Regex:
            case CljNodeKind::SymbolicValue:
                Error(StartOff(x), EndOff(x), "metadata-target", "Metadata can only be applied to IMetas");
                break;
            default: break;
        }
        N(m).parent = x;
        N(x).meta.insert(N(x).meta.begin(), m);
        return x;
    }

    int Dispatch() {
        const size_t a = i_;
        const char d = C(1);
        switch (d) {
            case '^': return Meta(2);
            case '\'': return Prefix(CljNodeKind::Var, 2);
            case '=': return Prefix(CljNodeKind::ReadEval, 2);
            case '"': return Regex();
            case '(': {
                const bool nested = fn_depth_ > 0;
                if (nested) Error(a, a + 2, "nested-fn", "Nested #()s are not allowed");
                fn_depth_++;
                const int k = Collection(CljNodeKind::Fn, a, 2, ')');
                fn_depth_--;
                return k;
            }
            case '{': return Collection(CljNodeKind::Set, a, 2, '}');
            case '!':
                Comment();
                return kNothing;
            case '<':
                i_ += 2;
                Error(a, i_, "unreadable", "Unreadable form");
                return kNothing;
            case '_': {
                i_ += 2;
                const int x = Operand(a, 2);
                if (x >= 0) res_.discarded.push_back(x);
                return kNothing;
            }
            case '?': return ReadCond();
            case ':': return NsMap();
            case '#': return Symbolic();
            default: break;
        }
        if (Eof(i_ + 1)) {
            i_++;
            Error(a, i_, "eof", "EOF while reading character");
            return kNothing;
        }
        // A tagged literal or a record constructor: #inst "...", #my.Rec{...}
        i_++;
        const int tag = Operand(a, 1);
        if (tag < 0) return kNothing;
        if (N(tag).kind != CljNodeKind::Symbol) {
            Error(StartOff(tag), EndOff(tag), "reader-tag", "Reader tag must be a symbol");
            return tag;
        }
        const int k = New(CljNodeKind::Tagged, a, EndOff(tag), "#" + N(tag).text);
        const int x = Operand(a, EndOff(tag) - a);
        if (x >= 0) {
            Adopt(k, x);
            SetEnd(k, EndOff(x));
            // The two tags clojure.core reads itself check their value.
            const std::string &t = N(tag).text;
            if (N(x).kind != CljNodeKind::String && (t == "uuid" || t == "inst")) {
                Error(StartOff(x), EndOff(x), "reader-tag",
                      t == "uuid" ? "#uuid data reader expected string" : "Instance literal expects a string for its timestamp.");
            }
        }
        return k;
    }

    int Regex() {
        const size_t a = i_;
        i_ += 2;
        while (!Eof() && C() != '"') {
            if (C() == '\\') i_++;
            if (!Eof()) i_++;
        }
        if (Eof()) {
            Error(a, a + 2, "eof-regex", "EOF while reading regex");
            return New(CljNodeKind::Regex, a, i_, s_.substr(a));
        }
        i_++;
        const std::string pattern = s_.substr(a + 2, i_ - 1 - (a + 2));
        const std::string err = PatternError(pattern);
        if (!err.empty()) Error(a, i_, "regex", err);
        return New(CljNodeKind::Regex, a, i_, s_.substr(a, i_ - a));
    }

    // The java.util.regex.Pattern errors a pattern most often has, in its
    // wording: an unclosed group or class, a stray `)`, a quantifier with
    // nothing to repeat.
    static std::string PatternError(const std::string &p) {
        const size_t n = p.size();
        int groups = 0;
        bool in_class = false;
        size_t class_start = 0;
        int class_depth = 0;
        for (size_t k = 0; k < n; k++) {
            const char c = p[k];
            if (c == '\\') {
                if (k + 1 < n && p[k + 1] == 'Q') {  // \Q...\E is literal
                    const size_t e = p.find("\\E", k + 2);
                    if (e == std::string::npos) return "";
                    k = e + 1;
                    continue;
                }
                k++;
                continue;
            }
            if (in_class) {
                if (c == '[') class_depth++;
                if (c == ']' && k > class_start + 1 && !(k == class_start + 2 && p[class_start + 1] == '^')) {
                    if (--class_depth == 0) in_class = false;
                }
                continue;
            }
            switch (c) {
                case '[':
                    in_class = true;
                    class_start = k;
                    class_depth = 1;
                    break;
                case '(': groups++; break;
                case ')':
                    if (groups == 0) return "Unmatched closing ')'" + std::string(k > 0 ? " near index " + std::to_string(k - 1) : "");
                    groups--;
                    break;
                case '*':
                case '+':
                case '?':
                    if (k == 0 || p[k - 1] == '(' || p[k - 1] == '|') {
                        if (c == '?' && k > 0 && p[k - 1] == '(') break;  // (?:...), (?i)
                        return std::string("Dangling meta character '") + c + "' near index " + std::to_string(k);
                    }
                    break;
                default: break;
            }
        }
        if (in_class) return "Unclosed character class near index " + std::to_string(n - 1);
        if (groups > 0) return "Unclosed group near index " + std::to_string(n);
        return "";
    }

    int Symbolic() {
        const size_t a = i_;
        i_ += 2;
        const int x = Operand(a, 2);
        if (x < 0) return kNothing;
        const CljNode &n = N(x);
        if (n.kind != CljNodeKind::Symbol) {
            Error(a, EndOff(x), "symbolic-value", "Invalid token: ##" + n.text);
            return x;
        }
        if (n.text != "Inf" && n.text != "-Inf" && n.text != "NaN") {
            Error(a, EndOff(x), "symbolic-value", "Unknown symbolic value: ##" + n.text);
        }
        const int k = New(CljNodeKind::SymbolicValue, a, EndOff(x), "##" + n.text);
        return k;
    }

    // #?(...) and #?@(...)
    int ReadCond() {
        const size_t a = i_;
        i_ += 2;
        bool splicing = false;
        if (C() == '@') {
            splicing = true;
            i_++;
        }
        while (!Eof() && IsWs(C())) i_++;
        if (Eof()) {
            Error(a, i_, "eof", "EOF while reading character");
            return kNothing;
        }
        if (C() != '(') {
            Error(a, i_, "read-cond", "read-cond body must be a list");
            return kNothing;
        }
        if (splicing && depth_ == 0) {
            Error(a, i_, "read-cond", "Reader conditional splicing not allowed at the top level.");
        }
        const int k = Collection(splicing ? CljNodeKind::ReadCondSplicing : CljNodeKind::ReadCond, a, i_ + 1 - a, ')');
        SetStart(k, a);
        const std::vector<int> kids = N(k).kids;
        // An odd count, unless a branch was taken before the end (the JVM
        // reads what follows a taken branch without checking it).
        bool taken = false;
        for (size_t p = 0; p + 1 < kids.size(); p += 2) {
            const CljNode &f = N(kids[p]);
            if (f.kind != CljNodeKind::Keyword) {
                taken = true;  // already reported
                break;
            }
            if (f.ns.empty() && (f.name == "clj" || f.name == "default")) {
                taken = true;
                break;
            }
        }
        // ...and only when the feature left without a form is one it would
        // take: `#?(:cljs)` reads as nothing.
        const bool dangling_taken = kids.size() % 2 != 0 && N(kids.back()).kind == CljNodeKind::Keyword &&
                                    N(kids.back()).ns.empty() &&
                                    (N(kids.back()).name == "clj" || N(kids.back()).name == "default");
        if (!taken && dangling_taken) {
            Error(a, EndOff(k), "read-cond",
                  "read-cond starting on line " + std::to_string(LineOf(a) + 1) + " requires an even number of forms");
        }
        return k;
    }

    void SetStart(int k, size_t a) {
        int l = 0, c = 0;
        LineCol(a, &l, &c);
        N(k).range.line = l;
        N(k).range.col = c;
    }

    // A #? feature, as it is read: a keyword, not :else or :none. Returns
    // whether checking stops here (a branch taken, or an error reported).
    bool CheckFeature(int cond, int kid) {
        const std::vector<int> &kids = N(cond).kids;
        if ((kids.size() - 1) % 2 != 0) {
            // A form: was its feature the one taken?
            const CljNode &f = N(kids[kids.size() - 2]);
            return f.kind == CljNodeKind::Keyword && f.ns.empty() && (f.name == "clj" || f.name == "default");
        }
        const CljNode &f = N(kid);
        if (f.kind == CljNodeKind::Keyword && f.ns.empty() && (f.name == "else" || f.name == "none")) {
            Error(StartOff(kid), EndOff(kid), "read-cond", "Feature name " + f.text + " is reserved.");
            return true;
        }
        if (f.kind != CljNodeKind::Keyword) {
            std::string printed = Printed(kid);
            const size_t nl = printed.find('\n');
            if (nl != std::string::npos) printed = printed.substr(0, nl);
            Error(StartOff(kid), EndOff(kid), "read-cond", "Feature should be a keyword: " + printed);
            return true;
        }
        return false;
    }

    // #:ns{...}, #::{...}, #::alias{...}
    int NsMap() {
        const size_t a = i_;
        i_ += 2;
        bool autor = false;
        if (C() == ':') {
            autor = true;
            i_++;
        }
        std::string ns;
        bool have_sym = false;
        if (!Eof() && IsWs(C())) {
            if (!autor) {
                Error(a, i_, "ns-map", "Namespaced map must specify a namespace");
                return kNothing;
            }
            SkipWs();
        }
        if (!Eof() && C() != '{') {
            const size_t t0 = i_;
            const std::string t = Token();
            have_sym = true;
            ns = t;
            if (!MatchSymbol(t) || t[0] == ':' || (t.find('/') != std::string::npos && t != "/")) {
                Error(t0, i_, "ns-map", "Namespaced map must specify a valid namespace: " + t);
            }
            SkipWs();
        }
        if (Eof() || C() != '{') {
            Error(a, i_, "ns-map", "Namespaced map must specify a map");
            return kNothing;
        }
        if (!autor && !have_sym) Error(a, i_, "ns-map", "Namespaced map must specify a valid namespace: null");
        const std::string prefix = s_.substr(a, i_ - a);
        const int k = Collection(CljNodeKind::NsMap, i_, 1, '}');
        SetStart(k, a);
        N(k).text = prefix;
        N(k).ns = ns;
        N(k).auto_resolve = autor;
        return k;
    }

    // --- collections -------------------------------------------------------

    // Does an enclosing collection close with `c`?
    bool OuterExpects(char c) const {
        return std::find(closers_.begin(), closers_.end(), c) != closers_.end();
    }

    // A `(def...` or `(ns` at column 0, on a later line: where the closers
    // of an unclosed top-level form belong.
    bool AtColumn0Definition(size_t open) const {
        if (!recover_col0_ || C() != '(') return false;
        if (i_ == 0 || s_[i_ - 1] != '\n') return false;
        if (LineOf(i_) == LineOf(open)) return false;
        if (top_start_ != 0 && s_[top_start_ - 1] != '\n') return false;
        return s_.compare(i_ + 1, 3, "def") == 0 || s_.compare(i_ + 1, 3, "ns ") == 0;
    }

    int Collection(CljNodeKind kind, size_t a, size_t open_len, char close) {
        if (depth_ == 0) top_start_ = a;
        const int k = New(kind, a, a + open_len, s_.substr(a, open_len));
        i_ = a + open_len;
        if (depth_ >= kCljMaxNesting) {
            Error(a, i_, "too-deep", "Form nested too deeply");
            // Skip to the matching closer, counting brackets.
            int nest = 1;
            while (!Eof() && nest > 0) {
                if (C() == '(' || C() == '[' || C() == '{') nest++;
                if (IsCloser(C())) nest--;
                i_++;
            }
            SetEnd(k, i_);
            return k;
        }
        depth_++;
        closers_.push_back(close);
        bool cond_done = false;  // a #? branch was taken, or a feature was wrong: no more checks
        for (;;) {
            if (unwinding_) break;
            SkipWs();
            if (Eof()) {
                eof_in_collection_ = true;
                Error(a, a + open_len, "eof", "EOF while reading, starting at line " + std::to_string(LineOf(a) + 1));
                break;
            }
            const char c = C();
            if (c == close) {
                i_++;
                break;
            }
            if (IsCloser(c)) {
                Error(i_, i_ + 1, "unmatched-delimiter", std::string("Unmatched delimiter: ") + c);
                closers_.pop_back();
                const bool outer = OuterExpects(c);
                closers_.push_back(close);
                if (outer) break;  // this one was never closed; the outer one closes here
                i_++;
                continue;
            }
            if (AtColumn0Definition(a)) {
                Error(a, a + open_len, "eof", "EOF while reading, starting at line " + std::to_string(LineOf(a) + 1));
                unwinding_ = true;
                break;
            }
            const int x = One();
            if (x == kNothing || x == kCloser) continue;
            if (x == kEof) continue;
            Adopt(k, x);
            if ((kind == CljNodeKind::ReadCond || kind == CljNodeKind::ReadCondSplicing) && !cond_done) {
                cond_done = CheckFeature(k, x);
            }
        }
        closers_.pop_back();
        depth_--;
        if (depth_ == 0) unwinding_ = false;
        SetEnd(k, i_);
        if (kind == CljNodeKind::Map || kind == CljNodeKind::NsMap || kind == CljNodeKind::Set) CheckEntries(k);
        return k;
    }

    // The map's even count and every collection's distinct keys, as the JVM
    // reads them (after #? chose a branch).
    void CheckEntries(int k) {
        const CljNodeKind kind = N(k).kind;
        const std::vector<int> kids = CljEffectiveKids(res_, k);
        if (kind != CljNodeKind::Set && kids.size() % 2 != 0) {
            Error(StartOff(k), EndOff(k), "odd-map",
                  kind == CljNodeKind::NsMap ? "Namespaced map literal must contain an even number of forms"
                                             : "Map literal must contain an even number of forms");
            return;
        }
        std::vector<std::string> seen;
        const size_t step = kind == CljNodeKind::Set ? 1 : 2;
        for (size_t p = 0; p < kids.size(); p += step) {
            std::string key = Canonical(kids[p]);
            if (key.empty()) continue;  // not comparable here (an error, a #=)
            if (kind == CljNodeKind::NsMap) key = NsMapKey(k, kids[p], key);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                Error(StartOff(kids[p]), EndOff(kids[p]), "duplicate-key", "Duplicate key: " + Printed(kids[p]));
                continue;
            }
            seen.push_back(key);
        }
    }

    // #:a{:b 1 :_/c 2}: :b is :a/b, :_/c is :c.
    std::string NsMapKey(int map, int key, const std::string &canon) {
        const CljNode &n = N(key);
        if ((n.kind != CljNodeKind::Keyword && n.kind != CljNodeKind::Symbol) || n.auto_resolve) return canon;
        if (n.ns == "_") return canon + "|_";
        if (n.ns.empty()) return canon + "|" + N(map).ns;
        return canon;
    }

    std::string Printed(int k) {
        const CljNode &n = N(k);
        if (n.kind == CljNodeKind::String) return n.name;
        return s_.substr(StartOff(k), EndOff(k) - StartOff(k));
    }

    // A value's identity for equality, "" when it cannot be known here.
    std::string Canonical(int k) {
        const CljNode &n = N(k);
        std::string out;
        switch (n.kind) {
            case CljNodeKind::Error:
            case CljNodeKind::ReadEval:
            case CljNodeKind::Tagged:
            case CljNodeKind::Regex:  // patterns are compared by identity
            case CljNodeKind::Fn: return "";
            case CljNodeKind::Symbol: return "s" + n.ns + "/" + n.name;
            case CljNodeKind::Keyword: return std::string(n.auto_resolve ? "K" : "k") + n.ns + "/" + n.name;
            case CljNodeKind::String: return "\"" + n.name;
            case CljNodeKind::Number: {
                std::string t = n.text;
                if (!t.empty() && t[0] == '+') t = t.substr(1);
                if (!t.empty() && t.back() == 'N') t.pop_back();
                return "n" + t;
            }
            default: break;
        }
        out = std::to_string(static_cast<int>(n.kind)) + "(" + n.text;
        for (int kid : CljEffectiveKids(res_, k)) {
            const std::string c = Canonical(kid);
            if (c.empty()) return "";
            out += c + " ";
        }
        return out + ")";
    }
};

}  // namespace

std::vector<int> CljEffectiveKids(const CljReadResult &r, int node) {
    std::vector<int> out;
    for (int kid : r.at(node).kids) {
        const CljNode &n = r.at(kid);
        if (n.kind != CljNodeKind::ReadCond && n.kind != CljNodeKind::ReadCondSplicing) {
            out.push_back(kid);
            continue;
        }
        int chosen = -1;
        for (size_t p = 0; p + 1 < n.kids.size(); p += 2) {
            const CljNode &f = r.at(n.kids[p]);
            if (f.kind == CljNodeKind::Keyword && f.ns.empty() && (f.name == "clj" || f.name == "default")) {
                chosen = n.kids[p + 1];
                break;
            }
        }
        if (chosen < 0) continue;
        if (n.kind == CljNodeKind::ReadCond) {
            out.push_back(chosen);
        } else {
            for (int x : r.at(chosen).kids) out.push_back(x);
        }
    }
    return out;
}

CljReadResult CljRead(const std::vector<std::string> &lines, bool recover) {
    Reader first(lines, false);
    CljReadResult r = first.Run();
    if (!recover || !first.hit_eof_in_collection()) return r;
    // An unclosed form: read again, closing it at the next `(def` at column 0.
    Reader again(lines, true);
    return again.Run();
}
