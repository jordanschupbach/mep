// Lexer, parser and builtin-command table of mep's Tcl formatter: a port
// of tclint 0.6.2's lexer.py, parser.py, commands/checks.py and
// commands/builtin.py (MIT, Noah Moroze). See tcl_format.cpp.
//
// The structure follows tclint closely, including its quirks, because the
// formatter's output depends on exactly which arguments end up parsed as
// scripts, expressions and lists, and on which inputs are syntax errors.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "tcl_format_internal.h"

namespace tclfmt::detail {

namespace {

// tclint raises CommandArgError (or any other Python exception) while
// checking a command's arguments: the arguments are then left unparsed.
struct ArgError {};

// Recursion caps (DepthError refuses the file). A nested script costs far
// more stack per level than an expression operator (which costs 2), and no
// real code comes near either; tclint itself gives up around 140 levels.
constexpr int kMaxScriptDepth = 200;
constexpr int kMaxExprDepth = 1000;

enum class T {
    Eof,
    BsNl,
    BsSub,
    Newline,
    Semi,
    Ws,
    Quote,
    ArgExp,
    LBrace,
    RBrace,
    Star,
    LBracket,
    RBracket,
    Dollar,
    LParen,
    RParen,
    Hash,
    Alpha,
    Num,
    NsSep,
    Char,
    Contents,
};

size_t Utf8Len(unsigned char b) {
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

bool IsAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// ply lexer of tclint: one token of lookahead, lexed on demand, with the
// exclusive `bracedword` state pushed while reading a braced word.
class Lexer {
  public:
    Lexer(std::string text, Pos start) : text_(std::move(text)), line_(start.line), col_(start.col) { Next(); }
    Lexer(const Lexer &) = delete;
    Lexer &operator=(const Lexer &) = delete;

    T type() const { return type_; }
    std::string_view value() const { return value_; }
    Pos pos() const { return type_ == T::Eof ? Pos{line_, col_} : pos_; }
    bool Is(std::string_view v) const { return type_ != T::Eof && value_ == v; }
    void Push() { braced_.push_back(true); }
    void Pop() {
        if (!braced_.empty()) braced_.pop_back();
    }

    void Next() {
        for (;;) {
            const size_t n = text_.size();
            if (off_ >= n) {
                type_ = T::Eof;
                value_ = {};
                return;
            }
            const bool br = !braced_.empty() && braced_.back();
            const char c = text_[off_];
            size_t len = 1;
            T t = T::Char;
            if (c == '\\') {
                if (off_ + 1 < n && text_[off_ + 1] == '\n') {
                    t = T::BsNl;
                    len = 2;
                } else if (off_ + 1 < n) {
                    t = T::BsSub;
                    len = 1 + Utf8Len(static_cast<unsigned char>(text_[off_ + 1]));
                } else if (br) {
                    // ply's error rule: skip the character.
                    Advance(1);
                    continue;
                } else {
                    t = T::Char;
                }
            } else if (br) {
                if (c == '{') {
                    t = T::LBrace;
                } else if (c == '}') {
                    t = T::RBrace;
                } else {
                    t = T::Contents;
                    len = 0;
                    while (off_ + len < n && text_[off_ + len] != '{' && text_[off_ + len] != '}' &&
                           text_[off_ + len] != '\\')
                        len++;
                }
            } else {
                switch (c) {
                case '\n': t = T::Newline; break;
                case ';': t = T::Semi; break;
                case '\t':
                case '\v':
                case '\f':
                case '\r':
                case ' ':
                    t = T::Ws;
                    len = 0;
                    while (off_ + len < n) {
                        const char d = text_[off_ + len];
                        if (d != '\t' && d != '\v' && d != '\f' && d != '\r' && d != ' ') break;
                        len++;
                    }
                    break;
                case '"': t = T::Quote; break;
                case '{':
                    if (text_.compare(off_, 3, "{*}") == 0) {
                        t = T::ArgExp;
                        len = 3;
                    } else {
                        t = T::LBrace;
                    }
                    break;
                case '}': t = T::RBrace; break;
                case '*': t = T::Star; break;
                case '[': t = T::LBracket; break;
                case ']': t = T::RBracket; break;
                case '$': t = T::Dollar; break;
                case '(': t = T::LParen; break;
                case ')': t = T::RParen; break;
                case '#': t = T::Hash; break;
                default:
                    if (IsAlpha(c)) {
                        t = T::Alpha;
                        len = 0;
                        while (off_ + len < n && IsAlpha(text_[off_ + len])) len++;
                    } else if (IsDigit(c)) {
                        t = T::Num;
                        len = 0;
                        while (off_ + len < n && IsDigit(text_[off_ + len])) len++;
                    } else if (c == ':' && off_ + 1 < n && text_[off_ + 1] == ':') {
                        t = T::NsSep;
                        len = 0;
                        while (off_ + len < n && text_[off_ + len] == ':') len++;
                    } else {
                        t = T::Char;
                        len = Utf8Len(static_cast<unsigned char>(c));
                    }
                }
            }
            if (off_ + len > n) len = n - off_;
            type_ = t;
            pos_ = {line_, col_};
            value_ = std::string_view(text_).substr(off_, len);
            Advance(len);
            return;
        }
    }

    // tclint's Lexer.expect: on a mismatch munch one more token, then raise.
    void Expect(std::initializer_list<T> ok, const std::string &message, Pos at) {
        for (T t : ok)
            if (type_ == t) {
                Next();
                return;
            }
        Next();
        throw SyntaxError{message, at};
    }

  private:
    void Advance(size_t len) {
        for (size_t k = 0; k < len; k++) {
            const unsigned char ch = static_cast<unsigned char>(text_[off_ + k]);
            if (ch == '\n') {
                line_++;
                col_ = 1;
            } else if ((ch & 0xC0) != 0x80) {
                col_++;
            }
        }
        off_ += len;
    }

    std::string text_;
    size_t off_ = 0;
    int line_;
    int col_;
    std::vector<bool> braced_;
    T type_ = T::Eof;
    std::string_view value_;
    Pos pos_;
};

std::string Str(std::string_view v) { return std::string(v); }

Pos ContentsPos(const Node *n) {
    switch (n->kind) {
    case K::BareWord: return n->pos;
    case K::BracedWord:
    case K::QuotedWord: return {n->pos.line, n->pos.col + 1};
    default: return {1, 1};  // tclint's contents_pos is None here: the lexer starts at 1:1
    }
}

// --- number literals (parser.py's _is_int / _is_float_*) ---

bool AllIn(std::string_view s, size_t from, std::string_view set, bool non_empty) {
    if (non_empty && s.size() <= from) return false;
    for (size_t k = from; k < s.size(); k++)
        if (set.find(s[k]) == std::string_view::npos) return false;
    return true;
}

bool IsInt(std::string_view s, bool full) {
    static constexpr std::string_view kDigits = "0123456789";
    static constexpr std::string_view kOct = "01234567";
    static constexpr std::string_view kHex = "0123456789abcdefABCDEF";
    if (s.substr(0, 2) == "0b") return AllIn(s, 2, "01", full);
    if (s.substr(0, 2) == "0o") return AllIn(s, 2, kOct, full);
    if (s.substr(0, 2) == "0x") return AllIn(s, 2, kHex, full);
    if (s.substr(0, 1) == "0") return AllIn(s, 1, kOct, full);
    return AllIn(s, 0, kDigits, full);
}

size_t SkipDigits(std::string_view s, size_t i) {
    while (i < s.size() && IsDigit(s[i])) i++;
    return i;
}

bool IsFloatPrefix(std::string_view s) {
    // \d*\.?\d*([Ee][+-]?)?\d*
    size_t i = SkipDigits(s, 0);
    if (i < s.size() && s[i] == '.') i++;
    i = SkipDigits(s, i);
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
    }
    i = SkipDigits(s, i);
    return i == s.size();
}

std::string Lower(std::string_view s) {
    std::string r(s);
    for (char &c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

bool IsFloatLiteral(std::string_view s) {
    const std::string l = Lower(s);
    if (l == "nan" || l == "inf") return true;
    if (s.empty()) return false;
    // \d*\.?\d*([Ee][+-]?\d+)?
    size_t i = SkipDigits(s, 0);
    if (i < s.size() && s[i] == '.') i++;
    i = SkipDigits(s, i);
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
        const size_t d = SkipDigits(s, i);
        if (d == i) return false;
        i = d;
    }
    return i == s.size();
}

bool IsBoolLiteral(std::string_view s) {
    return s == "false" || s == "no" || s == "off" || s == "true" || s == "yes" || s == "on";
}

bool IsFunction(std::string_view s) {
    static const std::set<std::string, std::less<>> kFns = {
        "abs",  "acos",  "asin", "atan",  "atan2", "bool", "ceil",  "cos",   "cosh", "double", "entier",
        "exp",  "floor", "fmod", "hypot", "int",   "isqrt", "log",  "log10", "max",  "min",    "pow",
        "rand", "round", "sin",  "sinh",  "sqrt",  "srand", "tan",  "tanh",  "wide"};
    return kFns.count(s) != 0;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

// Python's int(str) for the strings tclint feeds it.
bool PyInt(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) a++;
    while (b > a && IsSpace(s[b - 1])) b--;
    if (a < b && (s[a] == '+' || s[a] == '-')) a++;
    if (a == b) return false;
    bool prev_digit = false;
    for (size_t k = a; k < b; k++) {
        if (IsDigit(s[k])) {
            prev_digit = true;
        } else if (s[k] == '_' && prev_digit && k + 1 < b && IsDigit(s[k + 1])) {
            prev_digit = false;
        } else {
            return false;
        }
    }
    return true;
}

// Python's float(str), close enough for timerate's -overhead.
bool PyFloat(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) a++;
    while (b > a && IsSpace(s[b - 1])) b--;
    std::string t = s.substr(a, b - a);
    size_t k = (!t.empty() && (t[0] == '+' || t[0] == '-')) ? 1 : 0;
    const std::string l = Lower(t.substr(k));
    if (l == "inf" || l == "infinity" || l == "nan") return true;
    std::string u;
    for (size_t i = k; i < t.size(); i++) {
        if (t[i] == '_') {
            if (i == k || i + 1 >= t.size() || !IsDigit(t[i - 1]) || !IsDigit(t[i + 1])) return false;
            continue;
        }
        u += t[i];
    }
    return !u.empty() && IsFloatLiteral(u) && u != ".";
}

}  // namespace

const std::string *Contents(const Node *n) {
    static const std::string kEmpty;
    if (!n) return nullptr;
    switch (n->kind) {
    case K::BareWord:
    case K::BracedWord: return &n->value;
    case K::QuotedWord:
        if (n->kids.size() > 1) return nullptr;
        if (n->kids.empty()) return &kEmpty;
        return Contents(n->kids[0]);
    case K::ArgExpansion: return n->kids.empty() ? nullptr : Contents(n->kids[0]);
    default: return nullptr;
    }
}

Node *Parser::New(K kind) {
    arena_.emplace_back();
    Node *n = &arena_.back();
    n->kind = kind;
    return n;
}

struct ParserImpl {
    Parser &p;

    struct Depth {
        int &d;
        Depth(int &x, int cap) : d(x) {
            if (++d > cap) {
                d--;
                throw DepthError{};
            }
        }
        ~Depth() { d--; }
        Depth(const Depth &) = delete;
        Depth &operator=(const Depth &) = delete;
    };

    Node *Make(K kind, Pos pos, Pos end, std::string value = {}) {
        Node *n = p.New(kind);
        n->pos = pos;
        n->end = end;
        n->value = std::move(value);
        return n;
    }

    // parser.py's _Word: words built from literal runs and substitutions.
    struct Word {
        std::vector<Node *> segs;
        std::string cur;
        bool started = false;
        Pos start;
        void AddTok(ParserImpl &, std::string_view v, Pos at) {
            if (!started) {
                started = true;
                start = at;
            }
            cur += v;
        }
        void AddNode(ParserImpl &pi, Node *node) {
            if (!cur.empty()) {
                segs.push_back(pi.Make(K::BareWord, start, node->pos, cur));
                cur.clear();
                started = false;
            }
            segs.push_back(node);
        }
        std::vector<Node *> Resolve(ParserImpl &pi, Pos end) {
            if (!cur.empty()) segs.push_back(pi.Make(K::BareWord, start, end, cur));
            return segs;
        }
    };

    Node *ParseScript(Lexer &ts, bool in_sub) {
        Depth guard(p.depth_, kMaxScriptDepth);
        const Pos pos = ts.pos();
        Node *script = Make(in_sub ? K::CommandSub : K::Script, pos, pos);
        while (ts.type() != T::Eof) {
            if (ts.type() == T::Ws || ts.type() == T::BsNl) {
                ts.Next();
                continue;
            }
            if (ts.type() == T::Hash) {
                script->kids.push_back(ParseComment(ts));
            } else {
                Node *cmd = ParseCommand(ts, in_sub);
                if (cmd) script->kids.push_back(cmd);
            }
            if (in_sub && ts.type() == T::RBracket) return script;
            const Pos at = ts.pos();
            ts.Expect({T::Eof, T::Newline, T::Semi}, "expected newline or semicolon, got " + Str(ts.value()), at);
        }
        if (in_sub) throw SyntaxError{"reached EOF without finding end of command substitution", pos};
        script->end = ts.pos();
        return script;
    }

    Node *ParseComment(Lexer &ts) {
        const Pos pos = ts.pos();
        ts.Next();  // '#'
        std::string value;
        while (ts.type() != T::Newline && ts.type() != T::Eof) {
            value += ts.value();
            ts.Next();
        }
        return Make(K::Comment, pos, ts.pos(), RStrip(value));
    }

    // Python's str.rstrip(): ASCII whitespace plus the Unicode spaces.
    static std::string RStrip(std::string s) {
        for (;;) {
            if (s.empty()) return s;
            const unsigned char c = static_cast<unsigned char>(s.back());
            if (c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F)) {
                s.pop_back();
                continue;
            }
            size_t n = UnicodeSpaceSuffix(s);
            if (n == 0) return s;
            s.resize(s.size() - n);
        }
    }

    static size_t UnicodeSpaceSuffix(const std::string &s) {
        static const char *const kSpaces[] = {"\xC2\x85",     "\xC2\xA0",     "\xE1\x9A\x80", "\xE2\x80\x80",
                                              "\xE2\x80\x81", "\xE2\x80\x82", "\xE2\x80\x83", "\xE2\x80\x84",
                                              "\xE2\x80\x85", "\xE2\x80\x86", "\xE2\x80\x87", "\xE2\x80\x88",
                                              "\xE2\x80\x89", "\xE2\x80\x8A", "\xE2\x80\xA8", "\xE2\x80\xA9",
                                              "\xE2\x80\xAF", "\xE2\x81\x9F", "\xE3\x80\x80"};
        for (const char *sp : kSpaces) {
            const std::string_view v(sp);
            if (s.size() >= v.size() && s.compare(s.size() - v.size(), v.size(), v) == 0) return v.size();
        }
        return 0;
    }

    Node *ParseCommand(Lexer &ts, bool in_sub) {
        const Pos pos = ts.pos();
        Node *routine = ParseWord(ts, in_sub);
        if (!routine) return nullptr;
        std::vector<Node *> args;
        for (;;) {
            if (ts.type() != T::Ws && ts.type() != T::BsNl) break;
            while (ts.type() == T::Ws || ts.type() == T::BsNl) ts.Next();
            Node *w = ParseWord(ts, in_sub);
            if (!w) break;
            args.push_back(w);
        }
        std::vector<Node *> parsed = args;
        if (const std::string *name = Contents(routine)) {
            try {
                if (auto r = CheckCommand(*name, args)) parsed = std::move(*r);
            } catch (const ArgError &) {
                parsed = args;
            }
        }
        Node *cmd = Make(K::Command, pos, parsed.empty() ? routine->end : parsed.back()->end);
        cmd->kids.push_back(routine);
        cmd->kids.insert(cmd->kids.end(), parsed.begin(), parsed.end());
        return cmd;
    }

    Node *ParseWord(Lexer &ts, bool in_sub) {
        if (ts.type() == T::ArgExp) return ParseArgExpansion(ts, in_sub);
        if (ts.type() == T::LBrace) return ParseBracedWord(ts);
        if (ts.type() == T::Quote) return ParseQuotedWord(ts);
        return ParseBareWord(ts, in_sub);
    }

    static bool IsDelim(T t, bool in_sub) {
        return t == T::Ws || t == T::BsNl || t == T::Newline || t == T::Semi || t == T::Eof ||
               (in_sub && t == T::RBracket);
    }

    Node *ParseArgExpansion(Lexer &ts, bool in_sub) {
        const Pos pos = ts.pos();
        ts.Next();
        if (IsDelim(ts.type(), in_sub)) return Make(K::BracedWord, pos, ts.pos(), "*");
        Node *inner = ParseWord(ts, in_sub);
        if (!inner) throw SyntaxError{"unexpected end of word after {*}", pos};
        Node *n = Make(K::ArgExpansion, pos, ts.pos());
        n->kids.push_back(inner);
        return n;
    }

    // A '$' or '[' inside a word ('$' not followed by a name is literal).
    void Substitution(Lexer &ts, Word &w) {
        if (ts.type() == T::Dollar) {
            const std::string v = Str(ts.value());
            const Pos at = ts.pos();
            Node *vs = ParseVarSub(ts);
            if (vs) {
                w.AddNode(*this, vs);
            } else {
                w.AddTok(*this, v, at);
            }
        } else {
            w.AddNode(*this, ParseCommandSub(ts));
        }
    }

    Node *ParseQuotedWord(Lexer &ts) {
        const Pos pos = ts.pos();
        ts.Next();
        Word w;
        while (ts.type() != T::Quote && ts.type() != T::Eof) {
            if (ts.type() == T::Dollar || ts.type() == T::LBracket) {
                Substitution(ts, w);
            } else {
                w.AddTok(*this, ts.value(), ts.pos());
                ts.Next();
            }
        }
        std::vector<Node *> res = w.Resolve(*this, ts.pos());
        ts.Expect({T::Quote}, "reached EOF without finding match for quote", pos);
        Node *n = Make(K::QuotedWord, pos, ts.pos());
        n->kids = std::move(res);
        return n;
    }

    Node *ParseBracedWord(Lexer &ts) {
        const Pos pos = ts.pos();
        ts.Push();
        ts.Next();
        std::string word;
        std::vector<Pos> expected{pos};
        for (;;) {
            const T t = ts.type();
            if (t == T::Eof) throw SyntaxError{"reached EOF without finding match for brace", expected.back()};
            if (t == T::LBrace) {
                expected.push_back(ts.pos());
            } else if (t == T::RBrace) {
                expected.pop_back();
                if (expected.empty()) {
                    ts.Pop();
                    ts.Next();
                    break;
                }
            }
            word += ts.value();
            ts.Next();
        }
        return Make(K::BracedWord, pos, ts.pos(), word);
    }

    Node *ParseBareWord(Lexer &ts, bool in_sub) {
        const Pos pos = ts.pos();
        Word w;
        while (!IsDelim(ts.type(), in_sub)) {
            if (ts.type() == T::Dollar || ts.type() == T::LBracket) {
                Substitution(ts, w);
            } else {
                w.AddTok(*this, ts.value(), ts.pos());
                ts.Next();
            }
        }
        std::vector<Node *> res = w.Resolve(*this, ts.pos());
        if (res.empty()) return nullptr;
        if (res.size() == 1) return res[0];
        Node *n = Make(K::Compound, pos, ts.pos());
        n->kids = std::move(res);
        return n;
    }

    Node *ParseVarSub(Lexer &ts) {
        const Pos pos = ts.pos();
        ts.Next();  // '$'
        std::string var;
        if (ts.type() == T::LBrace) {
            const Pos brace = ts.pos();
            ts.Next();
            while (ts.type() != T::RBrace) {
                if (ts.type() == T::Eof) throw SyntaxError{"reached EOF without finding match for brace", brace};
                var += ts.value();
                ts.Next();
            }
            ts.Next();
            Node *n = Make(K::VarSub, pos, ts.pos(), var);
            n->braced = true;
            return n;
        }
        while (ts.type() == T::Alpha || ts.type() == T::Num || ts.type() == T::NsSep) {
            var += ts.value();
            ts.Next();
        }
        if (var.empty()) return nullptr;
        std::vector<Node *> index;
        bool paren = false;
        if (ts.type() == T::LParen) {
            paren = true;
            const Pos at = ts.pos();
            Word w;
            ts.Next();
            while (ts.type() != T::RParen) {
                if (ts.type() == T::Eof) throw SyntaxError{"reached EOF without finding match for paren", at};
                if (ts.type() == T::Dollar || ts.type() == T::LBracket) {
                    Substitution(ts, w);
                } else {
                    w.AddTok(*this, ts.value(), ts.pos());
                    ts.Next();
                }
            }
            index = w.Resolve(*this, ts.pos());
            ts.Next();
        }
        Node *n = Make(K::VarSub, pos, ts.pos(), var);
        n->paren = paren;
        n->kids = std::move(index);
        return n;
    }

    Node *ParseCommandSub(Lexer &ts) {
        const Pos pos = ts.pos();
        ts.Next();  // '['
        Node *script = ParseScript(ts, true);
        ts.Next();  // ']'
        script->pos = pos;
        script->end = ts.pos();
        return script;
    }

    Node *ParseList(Node *node) {
        if (node->kind == K::List) return node;
        const std::string *c = Contents(node);
        if (!c) throw ArgError{};
        Depth guard(p.depth_, kMaxScriptDepth);
        Lexer ts(*c, ContentsPos(node));
        Node *list = Make(K::List, node->pos, node->end);
        auto delim = [](T t) { return t == T::Ws || t == T::BsNl || t == T::Newline; };
        while (ts.type() != T::Eof) {
            while (delim(ts.type())) ts.Next();
            if (ts.type() == T::Eof) break;
            if (ts.type() == T::LBrace) {
                list->kids.push_back(ParseBracedWord(ts));
            } else if (ts.type() == T::Quote) {
                const Pos qpos = ts.pos();
                ts.Next();
                const Pos bpos = ts.pos();
                std::string contents;
                while (ts.type() != T::Quote && ts.type() != T::Eof) {
                    contents += ts.value();
                    ts.Next();
                }
                Node *word = Make(K::BareWord, bpos, ts.pos(), contents);
                ts.Expect({T::Quote}, "reached EOF without finding match for quote", qpos);
                Node *q = Make(K::QuotedWord, qpos, ts.pos());
                q->kids.push_back(word);
                list->kids.push_back(q);
            } else {
                const Pos at = ts.pos();
                std::string contents;
                while (!delim(ts.type()) && ts.type() != T::Eof) {
                    contents += ts.value();
                    ts.Next();
                }
                list->kids.push_back(Make(K::BareWord, at, ts.pos(), contents));
            }
        }
        return list;
    }

    Node *ParseAt(const std::string &text, Pos start) {
        Lexer ts(text, start);
        return ParseScript(ts, false);
    }

    Node *ParseScriptArg(Node *node) {
        const std::string *c = Contents(node);
        if (!c) throw ArgError{};
        Node *script = ParseAt(*c, ContentsPos(node));
        script->pos = node->pos;
        script->end = node->end;
        return script;
    }

    // --- expressions ---

    static bool IsExprWs(T t) { return t == T::Ws || t == T::BsNl || t == T::Newline; }
    static void SkipExprWs(Lexer &ts) {
        while (IsExprWs(ts.type())) ts.Next();
    }

    Node *ParseExpression(Node *node) {
        const std::string *c = Contents(node);
        if (!c) throw ArgError{};
        Lexer ts(*c, ContentsPos(node));
        Node *contents = Expr(ts);
        const Pos at = ts.pos();
        ts.Expect({T::Eof}, "expected end of expression, got " + Str(ts.value()), at);
        // tclint builds an empty expression node here (and then prints `{}`).
        if (!contents) throw ArgError{};
        Node *n = Make(node->kind == K::BracedWord ? K::BracedExpression : K::Expression, node->pos, node->end);
        n->kids.push_back(contents);
        return n;
    }

    static bool IsColonOrComma(const Lexer &ts) { return ts.Is(":") || ts.Is(","); }

    Node *Expr(Lexer &ts) {
        Depth guard(p.expr_depth_, kMaxExprDepth);
        SkipExprWs(ts);
        Node *op1 = Operand(ts);
        Node *expr = op1;
        if (ts.type() != T::Eof && ts.type() != T::RParen && !IsColonOrComma(ts)) {
            if (ts.Is("?")) {
                Pos start = ts.pos();
                ts.Next();
                Node *q = Make(K::BareWord, start, ts.pos(), "?");
                Node *op2 = Expr(ts);
                if (!ts.Is(":")) {
                    start = ts.pos();
                    ts.Next();
                    throw SyntaxError{"expected ':' to continue ternary expression", start};
                }
                start = ts.pos();
                ts.Next();
                Node *colon = Make(K::BareWord, start, ts.pos(), ":");
                Node *op3 = Expr(ts);
                if (!op1 || !op2 || !op3) throw ArgError{};
                expr = Make(K::TernaryOp, op1->pos, op3->end);
                expr->kids = {op1, q, op2, colon, op3};
            } else {
                Node *op = Operator(ts);
                Node *op2 = Expr(ts);
                if (!op1 || !op2) throw ArgError{};
                expr = Make(K::BinaryOp, op1->pos, op2->end);
                expr->kids = {op1, op, op2};
            }
            if (ts.type() != T::RParen && !IsColonOrComma(ts)) {
                ts.Expect({T::Eof}, "expected end of expression", ts.pos());
            }
        }
        SkipExprWs(ts);
        return expr;
    }

    Node *Operand(Lexer &ts) {
        Depth guard(p.expr_depth_, kMaxExprDepth);
        SkipExprWs(ts);
        Node *n = OperandInner(ts);
        SkipExprWs(ts);
        return n;
    }

    Node *OperandInner(Lexer &ts) {
        if (ts.type() == T::Dollar) return ParseVarSub(ts);
        if (ts.type() == T::Quote) return ParseQuotedWord(ts);
        if (ts.type() == T::LBrace) return ParseBracedWord(ts);
        if (ts.type() == T::LBracket) return ParseCommandSub(ts);
        if (ts.type() == T::LParen) {
            const Pos start = ts.pos();
            ts.Next();
            Node *e = Expr(ts);
            if (!e) throw ArgError{};
            ts.Expect({T::RParen}, "reached EOF without finding match for paren", e->pos);
            Node *n = Make(K::ParenExpression, start, ts.pos());
            n->kids.push_back(e);
            return n;
        }
        if (ts.Is("-") || ts.Is("+") || ts.Is("~") || ts.Is("!")) {
            const Pos at = ts.pos();
            const std::string v = Str(ts.value());
            ts.Next();
            Node *op = Make(K::BareWord, at, ts.pos(), v);
            Node *operand = Operand(ts);
            if (!operand) throw ArgError{};
            Node *n = Make(K::UnaryOp, at, operand->end);
            n->kids = {op, operand};
            return n;
        }
        std::string operand;
        const Pos at = ts.pos();
        while (ts.type() != T::Eof) {
            const std::string next = operand + Str(ts.value());
            if (!IsInt(next, false) && !IsFloatPrefix(next)) break;
            operand = next;
            ts.Next();
        }
        if (!(IsInt(operand, true) || IsFloatLiteral(operand))) {
            while (ts.type() == T::Alpha || ts.type() == T::Num) {
                operand += ts.value();
                ts.Next();
            }
        }
        const bool is_func = IsFunction(operand);
        if (!(IsInt(operand, true) || IsFloatLiteral(operand) || IsBoolLiteral(operand) || is_func)) {
            throw SyntaxError{"invalid bareword in expression: " + operand, at};
        }
        Node *node = Make(K::BareWord, at, ts.pos(), operand);
        if (is_func) node = Function(ts, node);
        return node;
    }

    Node *Operator(Lexer &ts) {
        const Pos pos = ts.pos();
        std::string op;
        if (ts.Is("*") || ts.Is("&") || ts.Is("|")) {
            op = Str(ts.value());
            ts.Next();
            if (ts.Is(op)) {
                op += ts.value();
                ts.Next();
            }
        } else if (ts.Is("<") || ts.Is(">")) {
            op = Str(ts.value());
            ts.Next();
            if (ts.Is(op) || ts.Is("=")) {
                op += ts.value();
                ts.Next();
            }
        } else if (ts.Is("=") || ts.Is("!")) {
            op = Str(ts.value());
            ts.Next();
            if (!ts.Is("=")) throw SyntaxError{"invalid operator in expression: " + op, pos};
            op += ts.value();
            ts.Next();
        } else if (ts.Is("/") || ts.Is("%") || ts.Is("+") || ts.Is("-") || ts.Is("^") || ts.Is("eq") ||
                   ts.Is("ne") || ts.Is("in") || ts.Is("ni")) {
            op = Str(ts.value());
            ts.Next();
        } else {
            throw SyntaxError{"invalid operator in expression: " + Str(ts.value()), pos};
        }
        return Make(K::BareWord, pos, ts.pos(), op);
    }

    Node *Function(Lexer &ts, Node *name) {
        while (ts.type() == T::Ws || ts.type() == T::BsNl) ts.Next();
        ts.Expect({T::LParen}, "expected open paren after function name", name->pos);
        std::vector<Node *> args;
        auto delim = [&] { return ts.type() == T::RParen || ts.type() == T::Eof; };
        if (!delim()) args.push_back(Expr(ts));
        while (!delim()) {
            if (!ts.Is(",")) {
                const Pos start = ts.pos();
                ts.Next();
                throw SyntaxError{"expected comma between function arguments", start};
            }
            ts.Next();
            args.push_back(Expr(ts));
        }
        ts.Expect({T::RParen}, "expected close paren after function arguments", name->pos);
        for (Node *a : args)
            if (!a) throw ArgError{};
        Node *f = Make(K::Function, name->pos, ts.pos());
        f->kids.push_back(name);
        f->kids.insert(f->kids.end(), args.begin(), args.end());
        return f;
    }

    // --- commands/checks.py ---

    // arg_count(): the number of arguments with literal {*} lists expanded.
    // Parsing those lists can raise a syntax error, which fails the file.
    std::pair<size_t, bool> ArgCount(const std::vector<Node *> &args) {
        size_t count = 0;
        bool expansion = false;
        for (Node *a : args) {
            if (a->kind == K::ArgExpansion) {
                if (!Contents(a)) {
                    expansion = true;
                    continue;
                }
                count += ParseList(a)->kids.size();
            } else {
                count++;
            }
        }
        return {count, expansion};
    }

    std::vector<Node *> Eval(const std::vector<Node *> &args) {
        if (args.size() > 1)
            for (Node *a : args)
                if (a->kind == K::QuotedWord || a->kind == K::BracedWord) throw ArgError{};
        std::string script;
        const Node *prev = nullptr;
        for (Node *a : args) {
            const std::string *c = Contents(a);
            if (!c) throw ArgError{};
            if (prev) {
                if (prev->end.line != a->pos.line) {
                    for (int k = 0; k < a->pos.line - prev->end.line; k++) script += "\\\n";
                    if (a->pos.col > 1) script.append(static_cast<size_t>(a->pos.col - 1), ' ');
                } else if (a->pos.col > prev->end.col) {
                    script.append(static_cast<size_t>(a->pos.col - prev->end.col), ' ');
                }
            }
            script += *c;
            prev = a;
        }
        if (args.empty()) throw ArgError{};
        Node *s = ParseAt(script, args[0]->pos);
        s->end = args.back()->end;
        return {s};
    }

    static std::vector<Node *> Slice(const std::vector<Node *> &v, size_t from, size_t to = SIZE_MAX) {
        if (to > v.size()) to = v.size();
        if (from >= to) return {};
        return std::vector<Node *>(v.begin() + static_cast<std::ptrdiff_t>(from),
                                   v.begin() + static_cast<std::ptrdiff_t>(to));
    }

    static std::vector<Node *> Cat(std::vector<Node *> a, const std::vector<Node *> &b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }

    static bool ContentsIs(const Node *n, std::string_view s) {
        const std::string *c = Contents(n);
        return c && *c == s;
    }

    using Args = std::vector<Node *>;
    using Maybe = std::optional<Args>;

    // A leaf of the command table: callables by name; `nothing` leaves do no
    // parsing at all; everything else only runs arg_count().
    Maybe Leaf(const std::string &name, const Args &args) {
        static const std::set<std::string> kNothing = {
            "clock",           "glob",           "global",          "history",     "unset",
            "tcl::tm::roots",  "return",         "after cancel",    "dict create", "dict merge",
            "namespace delete", "namespace export", "namespace forget", "namespace import",
            "namespace ensemble create", "package forget", "tcl::tm::path add", "tcl::tm::path remove"};
        if (kNothing.count(name)) return std::nullopt;
        if (name == "fileevent" || name == "namespace inscope" || name == "package ifneeded") throw ArgError{};
        if (name == "after") return After(args);
        if (name == "after idle") return Eval(args);
        if (name == "apply") return Apply(args);
        if (name == "catch") return Catch(args);
        if (name == "dict filter") return DictFilter(args);
        if (name == "dict for" || name == "dict map") {
            if (args.size() != 3) throw ArgError{};
            return Cat(Slice(args, 0, 2), {ParseScriptArg(args[2])});
        }
        if (name == "dict update") {
            if (args.size() < 4 || args.size() % 2 != 0) throw ArgError{};
            return Cat(Slice(args, 0, args.size() - 1), {ParseScriptArg(args.back())});
        }
        if (name == "dict with") {
            if (args.size() < 2) throw ArgError{};
            return Cat(Slice(args, 0, args.size() - 1), {ParseScriptArg(args.back())});
        }
        if (name == "eval") return Eval(args);
        if (name == "expr") return ExprCmd(args);
        if (name == "for") {
            if (args.size() != 4) throw ArgError{};
            Node *a = ParseScriptArg(args[0]);
            Node *b = ParseExpression(args[1]);
            Node *c = ParseScriptArg(args[2]);
            Node *d = ParseScriptArg(args[3]);
            return Args{a, b, c, d};
        }
        if (name == "foreach") {
            auto [count, exp] = ArgCount(args);
            if ((!exp && count < 3) || (exp && args.empty())) throw ArgError{};
            return Cat(Slice(args, 0, args.size() - 1), {ParseScriptArg(args.back())});
        }
        if (name == "if") return If(args);
        if (name == "interp eval" || name == "namespace eval") {
            if (args.size() < 2) throw ArgError{};
            return Cat(Slice(args, 0, 1), Eval(Slice(args, 1)));
        }
        if (name == "lmap") {
            if (args.size() < 3) throw ArgError{};
            return Cat(Slice(args, 0, args.size() - 1), {ParseScriptArg(args.back())});
        }
        if (name == "namespace code") {
            // tclint keeps only the first argument; keep them all unparsed instead.
            if (args.size() != 1) throw ArgError{};
            return Args{ParseScriptArg(args[0])};
        }
        if (name == "namespace unknown") {
            if (args.size() > 1) throw ArgError{};
            if (args.size() == 1) return Args{ParseScriptArg(args[0])};
            return std::nullopt;
        }
        if (name == "proc") return Proc(args);
        if (name == "switch") return Switch(args);
        if (name == "time") return Time(args);
        if (name == "timerate") return Timerate(args);
        if (name == "try") return Try(args);
        if (name == "while") {
            if (args.size() != 2) throw ArgError{};
            Node *a = ParseExpression(args[0]);
            Node *b = ParseScriptArg(args[1]);
            return Args{a, b};
        }
        ArgCount(args);
        return std::nullopt;
    }

    struct Group {
        const char *name;
        std::vector<std::string> subs;
        bool has_default;
    };

    std::optional<Args> CheckCommand(const std::string &routine, const Args &args) {
        static const std::set<std::string> kLeaves = {
            "append", "apply", "break", "catch", "cd", "clock", "close", "concat", "continue", "coroutine", "eof",
            "error", "eval", "exec", "exit", "expr", "fblocked", "fconfigure", "fcopy", "file", "fileevent",
            "flush", "for", "foreach", "format", "gets", "glob", "global", "history", "if", "incr", "info", "join",
            "lappend", "lassign", "lindex", "linsert", "list", "llength", "lrepeat", "lreplace", "lreverse", "lset",
            "lsort", "lmap", "load", "lrange", "lsearch", "open", "pid", "pkg::create", "pkg_mkIndex", "proc",
            "puts", "pwd", "read", "regexp", "regsub", "rename", "return", "safe", "scan", "seek", "set", "socket",
            "source", "split", "string", "subst", "switch", "tailcall", "tell", "throw", "time", "timerate",
            "tcl::tm::roots", "trace", "try", "unload", "unset", "update", "uplevel", "upvar", "variable", "vwait",
            "while", "yield", "yieldto", "zlib"};
        static const std::vector<Group> kGroups = {
            {"after", {"cancel", "idle", "info"}, true},
            {"array",
             {"anymore", "donesearch", "exists", "get", "names", "nextelement", "set", "size", "startsearch",
              "statistics", "unset"},
             false},
            {"binary", {"decode", "encode", "format", "scan"}, false},
            {"chan",
             {"blocked", "close", "configure", "copy", "create", "eof", "event", "flush", "gets", "names", "pending",
              "pipe", "pop", "postevent", "push", "puts", "read", "seek", "tell", "truncate"},
             false},
            {"dict",
             {"append", "create", "exists", "filter", "for", "get", "incr", "info", "keys", "lappend", "map", "merge",
              "remove", "replace", "set", "size", "unset", "update", "values", "with"},
             false},
            {"encoding", {"convertfrom", "convertto", "dirs", "names", "system"}, false},
            {"interp", {"eval"}, true},
            {"memory",
             {"active", "break_on_malloc", "info", "init", "objs", "onexit", "tag", "trace", "trace_on_at_malloc",
              "validate"},
             false},
            {"namespace",
             {"children", "code", "current", "delete", "eval", "exists", "export", "forget", "import", "inscope",
              "origin", "parent", "path", "qualifiers", "tail", "unknown", "upvar", "which", "ensemble"},
             false},
            {"namespace ensemble", {"create", "configure", "exists"}, false},
            {"package",
             {"forget", "ifneeded", "names", "present", "provide", "require", "unknown", "vcompare", "versions",
              "vsatisfies", "prefer"},
             false},
            {"tcl::prefix", {"all", "longest", "match"}, false},
            {"tcl::tm::path", {"add", "remove", "list"}, false},
        };
        return Check(routine, args, kLeaves, kGroups, true);
    }

    std::optional<Args> Check(const std::string &name, const Args &args, const std::set<std::string> &leaves,
                              const std::vector<Group> &groups, bool top) {
        for (const Group &g : groups) {
            if (name != g.name) continue;
            const std::string *sub = args.empty() ? nullptr : Contents(args[0]);
            if (sub) {
                for (const std::string &s : g.subs) {
                    if (s != *sub) continue;
                    const std::string full = name + " " + s;
                    std::optional<Args> r = Check(full, Slice(args, 1), leaves, groups, false);
                    if (!r) return r;
                    return Cat(Slice(args, 0, 1), *r);
                }
            }
            if (g.has_default) return Leaf(name, args);
            throw ArgError{};
        }
        if (top && !leaves.count(name)) return std::nullopt;  // not a builtin
        return Leaf(name, args);
    }

    Maybe After(const Args &args) {
        Args script;
        if (args.size() > 1) script = Eval(Slice(args, 1));
        return Cat(Slice(args, 0, 1), script);
    }

    Maybe Apply(const Args &args) {
        if (args.empty()) throw ArgError{};
        Node *fn = ParseList(args[0]);
        if (fn->kids.size() < 2 || fn->kids.size() > 3) throw ArgError{};
        Node *copy = Make(K::List, fn->pos, fn->end);
        copy->kids = fn->kids;
        copy->kids[1] = ParseScriptArg(fn->kids[1]);
        return Cat({copy}, Slice(args, 1));
    }

    Maybe Catch(const Args &args) {
        if (args.empty() || args.size() > 3) throw ArgError{};
        return Cat({ParseScriptArg(args[0])}, Slice(args, 1));
    }

    Maybe DictFilter(const Args &args) {
        if (args.size() < 2) throw ArgError{};
        const std::string *t = Contents(args[1]);
        if (!t || (*t != "key" && *t != "script" && *t != "value")) throw ArgError{};
        if (*t != "script") return std::nullopt;
        // tclint drops anything after the script; refuse to parse that instead.
        if (args.size() != 4) throw ArgError{};
        Node *kv = ParseList(args[2]);
        if (kv->kids.size() != 2) throw ArgError{};
        return Args{args[0], args[1], kv, ParseScriptArg(args[3])};
    }

    Maybe ExprCmd(const Args &args) {
        if (args.empty()) throw ArgError{};
        if (args.size() == 1 && Contents(args[0])) return Args{ParseExpression(args[0])};
        std::string contents;
        Pos last = args[0]->pos;
        for (Node *a : args) {
            if (a->kind != K::BareWord) return std::nullopt;
            if (a->pos.line != last.line) {
                if (a->pos.line > last.line) contents.append(static_cast<size_t>(a->pos.line - last.line), '\n');
                if (a->pos.col > 1) contents.append(static_cast<size_t>(a->pos.col - 1), ' ');
            } else if (a->pos.col > last.col) {
                contents.append(static_cast<size_t>(a->pos.col - last.col), ' ');
            }
            contents += a->value;
            last = a->end;
        }
        Node *word = Make(K::BareWord, args[0]->pos, args.back()->end, contents);
        return Args{ParseExpression(word)};
    }

    Maybe If(const Args &args) {
        Args out;
        auto more = [&] { return out.size() < args.size(); };
        if (!more()) throw ArgError{};
        out.push_back(ParseExpression(args[0]));
        if (!more()) throw ArgError{};
        if (ContentsIs(args[out.size()], "then")) out.push_back(args[out.size()]);
        if (!more()) throw ArgError{};
        out.push_back(ParseScriptArg(args[out.size()]));
        while (more() && ContentsIs(args[out.size()], "elseif")) {
            out.push_back(args[out.size()]);
            if (!more()) throw ArgError{};
            out.push_back(ParseExpression(args[out.size()]));
            if (!more()) throw ArgError{};
            if (ContentsIs(args[out.size()], "then")) out.push_back(args[out.size()]);
            if (!more()) throw ArgError{};
            out.push_back(ParseScriptArg(args[out.size()]));
        }
        if (!more()) return out;
        if (ContentsIs(args[out.size()], "else")) out.push_back(args[out.size()]);
        if (!more()) throw ArgError{};
        out.push_back(ParseScriptArg(args[out.size()]));
        if (!more()) return out;
        throw ArgError{};
    }

    Maybe Proc(const Args &args) {
        if (args.size() != 3) throw ArgError{};
        Node *list = ParseList(args[1]);
        Node *copy = Make(K::List, list->pos, list->end);
        copy->kids = list->kids;
        for (Node *&a : copy->kids) {
            if (a->kind == K::BareWord) continue;
            Node *spec = ParseList(a);
            if (spec->kids.size() == 2) {
                a = spec;
            } else if (spec->kids.size() != 1) {
                throw ArgError{};
            }
        }
        return Args{args[0], copy, ParseScriptArg(args[2])};
    }

    Maybe Switch(const Args &args) {
        size_t i = 0;
        bool dashdash = false;
        for (size_t k = 0; k < args.size(); k++)
            if (ContentsIs(args[k], "--")) {
                i = k + 1;
                dashdash = true;
                break;
            }
        if (!dashdash) {
            for (;;) {
                if (i >= args.size()) throw ArgError{};
                const std::string *c = Contents(args[i]);
                if (c && (*c == "-exact" || *c == "-glob" || *c == "-regexp" || *c == "-nocase")) {
                    i += 1;
                } else if (c && (*c == "-matchvar" || *c == "-indexvar")) {
                    i += 2;
                } else {
                    break;
                }
            }
        }
        i += 1;
        Args out = Slice(args, 0, i);
        const bool as_list = args.size() >= 1 && i == args.size() - 1;
        Node *list = nullptr;
        std::vector<Node *> pairs;
        if (as_list) {
            Node *parsed = ParseList(args[i]);
            list = Make(K::List, parsed->pos, parsed->end);
            list->kids = parsed->kids;
            pairs = list->kids;
        } else {
            pairs = Slice(args, i);
        }
        if (pairs.size() % 2 != 0) throw ArgError{};
        for (size_t k = 1; k < pairs.size(); k += 2) {
            if (ContentsIs(pairs[k], "-")) continue;
            pairs[k] = ParseScriptArg(pairs[k]);
        }
        if (as_list) {
            list->kids = pairs;
            out.push_back(list);
        } else {
            out.insert(out.end(), pairs.begin(), pairs.end());
        }
        return out;
    }

    Maybe Time(const Args &args) {
        if (args.empty() || args.size() > 2) throw ArgError{};
        if (args.size() == 2) {
            const std::string *t = Contents(args[1]);
            if (t && !PyInt(*t)) throw ArgError{};
        }
        return Cat({ParseScriptArg(args[0])}, Slice(args, 1));
    }

    Maybe Timerate(const Args &args) {
        size_t k = 0;
        Args out;
        Node *arg = nullptr;
        for (;;) {
            if (k >= args.size()) throw ArgError{};
            arg = args[k++];
            if (ContentsIs(arg, "-direct") || ContentsIs(arg, "-calibrate")) {
                out.push_back(arg);
            } else if (ContentsIs(arg, "-overhead")) {
                out.push_back(arg);
                if (k >= args.size()) throw ArgError{};
                Node *val = args[k++];
                if (const std::string *c = Contents(val))
                    if (!PyFloat(*c)) throw ArgError{};
                out.push_back(val);
            } else {
                break;
            }
        }
        out.push_back(ParseScriptArg(arg));
        Args rest = Slice(args, k);
        if (rest.size() > 2) throw ArgError{};
        for (Node *r : rest) {
            const std::string *c = Contents(r);
            if (!c || !PyInt(*c)) throw ArgError{};
        }
        return Cat(out, rest);
    }

    static void CheckCode(const Node *n) {
        const std::string *c = Contents(n);
        if (!c || PyInt(*c)) return;
        if (*c == "ok" || *c == "error" || *c == "return" || *c == "break" || *c == "continue") return;
        throw ArgError{};
    }

    Maybe Try(const Args &args) {
        size_t k = 0;
        Args out;
        for (;;) {
            if (k >= args.size()) throw ArgError{};
            out.push_back(ParseScriptArg(args[k++]));
            if (k >= args.size()) break;
            Node *arg = args[k++];
            out.push_back(arg);
            if (ContentsIs(arg, "on")) {
                if (k >= args.size()) throw ArgError{};
                Node *code = args[k++];
                CheckCode(code);
                out.push_back(code);
                if (k >= args.size()) throw ArgError{};
                out.push_back(args[k++]);
            } else if (ContentsIs(arg, "trap")) {
                if (k >= args.size()) throw ArgError{};
                out.push_back(args[k++]);
                if (k >= args.size()) throw ArgError{};
                out.push_back(args[k++]);
            } else if (ContentsIs(arg, "finally")) {
                continue;
            } else {
                throw ArgError{};
            }
        }
        return out;
    }
};

Node *Parser::Parse(const std::string &text) {
    ParserImpl pi{*this};
    return pi.ParseAt(text, {1, 1});
}

Node *Parser::ParseAt(const std::string &text, Pos start) {
    ParserImpl pi{*this};
    return pi.ParseAt(text, start);
}

Node *Parser::ParseScriptArg(Node *node) {
    ParserImpl pi{*this};
    return pi.ParseScriptArg(node);
}

Node *Parser::ParseList(Node *node) {
    ParserImpl pi{*this};
    return pi.ParseList(node);
}

Node *Parser::ParseExpression(Node *node) {
    ParserImpl pi{*this};
    return pi.ParseExpression(node);
}

}  // namespace tclfmt::detail
