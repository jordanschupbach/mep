// The Lua formatter's lexer and parser (see lua_format_cst.h). The token
// rules and tree shapes follow full-moon 2.0, the parser StyLua 2.3.1
// formats with, since StyLua's layout depends on them: whitespace tokens are
// a run of spaces/tabs plus at most one newline, trailing trivia stops after
// the first newline, `break` is a block's last statement, binary operators
// nest by precedence climbing.
#include "lua_format_cst.h"

#include <cstdint>

#include "lua_format_walk.h"

namespace luafmt::cst {

void AppendTokenText(std::string &out, const Token &t) {
    switch (t.type) {
    case TT::Eof:
        return;
    case TT::SingleLineComment:
        out += "--";
        out += t.text;
        return;
    case TT::MultiLineComment: {
        const std::string eq(static_cast<size_t>(t.blocks), '=');
        out += "--[" + eq + "[";
        out += t.text;
        out += "]" + eq + "]";
        return;
    }
    case TT::String:
        if (t.quote == '[') {
            const std::string eq(static_cast<size_t>(t.blocks), '=');
            out += "[" + eq + "[";
            out += t.text;
            out += "]" + eq + "]";
        } else {
            out += t.quote;
            out += t.text;
            out += t.quote;
        }
        return;
    default:
        out += t.text;
        return;
    }
}

std::string TokenText(const Token &t) {
    std::string s;
    AppendTokenText(s, t);
    return s;
}

// ---- lexer --------------------------------------------------------------------

namespace {

bool IsIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHex(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

const char *const kKeywords[] = {"and",   "break", "do",     "else", "elseif", "end",   "false", "for",
                                 "function", "goto", "if",   "in",   "local",  "nil",   "not",   "or",
                                 "repeat", "return", "then", "true", "until",  "while"};

bool IsKeyword(std::string_view s) {
    for (const char *k : kKeywords)
        if (s == k) return true;
    return false;
}

class Lexer {
  public:
    explicit Lexer(std::string_view s) : s_(s) {}

    // Lexes one token at pos_; false on error (err_ set).
    bool Next(Token &t) {
        const size_t start = pos_;
        t = Token{};
        t.start = start;
        if (pos_ >= s_.size()) {
            t.type = TT::Eof;
            t.end = pos_;
            return true;
        }
        const char c = s_[pos_];
        if (IsIdentStart(c)) {
            size_t e = pos_ + 1;
            while (e < s_.size() && (IsIdentStart(s_[e]) || IsDigit(s_[e]))) e++;
            t.text = std::string(s_.substr(pos_, e - pos_));
            t.type = IsKeyword(t.text) ? TT::Symbol : TT::Identifier;
            pos_ = e;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            size_t e = pos_ + 1;
            while (e < s_.size()) {
                const char d = s_[e];
                if (d == ' ' || d == '\t') {
                    e++;
                } else if (d == '\n') {
                    e++;
                    break;
                } else if (d == '\r' && e + 1 < s_.size() && s_[e + 1] == '\n') {
                    e += 2;
                    break;
                } else {
                    break;
                }
            }
            t.type = TT::Whitespace;
            t.text = std::string(s_.substr(pos_, e - pos_));
            pos_ = e;
        } else if (c == '\n') {
            t.type = TT::Whitespace;
            t.text = "\n";
            pos_++;
        } else if (IsDigit(c) || (c == '.' && pos_ + 1 < s_.size() && IsDigit(s_[pos_ + 1]))) {
            if (!ReadNumber(t)) return false;
        } else if (c == '"' || c == '\'') {
            if (!ReadString(t, c)) return false;
        } else if (c == '[' && pos_ + 1 < s_.size() && (s_[pos_ + 1] == '[' || s_[pos_ + 1] == '=')) {
            size_t p = pos_ + 1;
            int blocks = 0;
            while (p < s_.size() && s_[p] == '=') {
                blocks++;
                p++;
            }
            if (p < s_.size() && s_[p] == '[') {
                std::string body;
                if (!ReadLongBody(p + 1, blocks, body, p)) return Fail("unfinished long string", start);
                t.type = TT::String;
                t.quote = '[';
                t.blocks = blocks;
                t.text = std::move(body);
                pos_ = p;
            } else {
                t.type = TT::Symbol;
                t.text = "[";
                pos_++;
            }
        } else if (c == '-' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '-') {
            size_t p = pos_ + 2;
            if (p < s_.size() && s_[p] == '[') {
                size_t q = p + 1;
                int blocks = 0;
                while (q < s_.size() && s_[q] == '=') {
                    blocks++;
                    q++;
                }
                if (q < s_.size() && s_[q] == '[') {
                    std::string body;
                    size_t endp = 0;
                    if (!ReadLongBody(q + 1, blocks, body, endp)) return Fail("unfinished long comment", start);
                    t.type = TT::MultiLineComment;
                    t.blocks = blocks;
                    t.text = std::move(body);
                    pos_ = endp;
                    t.end = pos_;
                    return true;
                }
            }
            size_t e = p;
            while (e < s_.size() && s_[e] != '\n') e++;
            t.type = TT::SingleLineComment;
            t.text = std::string(s_.substr(p, e - p));
            pos_ = e;
        } else {
            static const char *const kSyms[] = {"...", "..", "==", "~=", "<=", ">=", "<<", ">>", "//", "::",
                                                "+",   "-",  "*",  "/",  "%",  "^",  "#",  "&",  "~",  "|",
                                                "<",   ">",  "=",  "(",  ")",  "{",  "}",  "[",  "]",  ";",
                                                ":",   ",",  "."};
            bool found = false;
            for (const char *sym : kSyms) {
                const std::string_view sv(sym);
                if (s_.substr(pos_, sv.size()) == sv) {
                    t.type = TT::Symbol;
                    t.text = std::string(sv);
                    pos_ += sv.size();
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (pos_ == 0 && c == '#' && s_.size() > 1 && s_[1] == '!') {
                    // handled by the caller
                }
                return Fail(std::string("unexpected character '") + c + "'", start);
            }
        }
        t.end = pos_;
        return true;
    }

    bool ReadNumber(Token &t) {
        const size_t start = pos_;
        size_t p = pos_;
        if (s_[p] == '0' && p + 1 < s_.size() && (s_[p + 1] == 'x' || s_[p + 1] == 'X')) {
            p += 2;
            bool dec = false;
            size_t digits = 0;
            while (p < s_.size()) {
                const char d = s_[p];
                if (IsHex(d)) {
                    p++;
                    digits++;
                } else if (d == '.') {
                    if (dec) return Fail("malformed number", start);
                    dec = true;
                    p++;
                } else if (d == 'p' || d == 'P') {
                    if (digits == 0 && !dec) return Fail("malformed number", start);
                    p++;
                    if (p < s_.size() && (s_[p] == '+' || s_[p] == '-')) p++;
                    if (p >= s_.size() || !IsDigit(s_[p])) return Fail("malformed number", start);
                    while (p < s_.size() && IsDigit(s_[p])) p++;
                    break;
                } else {
                    break;
                }
            }
            if (p - start == 2) return Fail("malformed number", start);
        } else {
            bool dec = false;
            while (p < s_.size()) {
                const char d = s_[p];
                if (IsDigit(d)) {
                    p++;
                } else if (d == '.') {
                    if (dec) return Fail("malformed number", start);
                    dec = true;
                    p++;
                } else if (d == 'e' || d == 'E') {
                    p++;
                    if (p < s_.size() && (s_[p] == '+' || s_[p] == '-')) p++;
                    if (p >= s_.size() || !IsDigit(s_[p])) return Fail("malformed number", start);
                    while (p < s_.size() && IsDigit(s_[p])) p++;
                    break;
                } else {
                    break;
                }
            }
        }
        t.type = TT::Number;
        t.text = std::string(s_.substr(start, p - start));
        pos_ = p;
        return true;
    }

    bool ReadString(Token &t, char quote) {
        const size_t start = pos_;
        size_t p = pos_ + 1;
        bool escape = false, z = false;
        for (;;) {
            if (p >= s_.size()) return Fail("unfinished string", start);
            const char c = s_[p];
            if (escape) {
                escape = false;
                z = true;
                p++;
                continue;
            }
            if (c == '\\') {
                escape = true;
                p++;
                continue;
            }
            if (c == '\n' || c == '\r') {
                if (!z) return Fail("unfinished string", start);
                z = false;
                p++;
                continue;
            }
            if (c == quote) break;
            p++;
        }
        t.type = TT::String;
        t.quote = quote;
        t.text = std::string(s_.substr(start + 1, p - start - 1));
        pos_ = p + 1;
        return true;
    }

    // Reads a long bracket body starting at p (just after the opening `[`);
    // on success `end` is just past the closing bracket.
    bool ReadLongBody(size_t p, int blocks, std::string &body, size_t &end) {
        const std::string close = "]" + std::string(static_cast<size_t>(blocks), '=') + "]";
        const size_t f = s_.find(close, p);
        if (f == std::string_view::npos) return false;
        body = std::string(s_.substr(p, f - p));
        end = f + close.size();
        return true;
    }

    bool Fail(const std::string &msg, size_t at) {
        err_.message = msg;
        err_.line = LineOf(at);
        return false;
    }

    int LineOf(size_t at) const {
        int line = 1;
        for (size_t i = 0; i < at && i < s_.size(); i++)
            if (s_[i] == '\n') line++;
        return line;
    }

    std::string_view s_;
    size_t pos_ = 0;
    ParseError err_;
};

}  // namespace

bool Lex(std::string_view src, std::vector<Token> &out, ParseError &err) {
    Lexer lx(src);
    if (src.size() >= 2 && src[0] == '#' && src[1] == '!') {
        size_t e = 0;
        while (e < src.size() && src[e] != '\n') e++;
        Token sh;
        sh.type = TT::Shebang;
        sh.text = std::string(src.substr(0, e));
        sh.start = 0;
        sh.end = e;
        out.push_back(std::move(sh));
        lx.pos_ = e;
    }
    for (;;) {
        Token t;
        if (!lx.Next(t)) {
            err = lx.err_;
            return false;
        }
        const bool eof = t.type == TT::Eof;
        out.push_back(std::move(t));
        if (eof) return true;
    }
}

// ---- token references -------------------------------------------------------------

namespace {

// Groups the raw token stream into TokenRefs with full-moon's trivia rules.
std::vector<TokenRef> Attach(std::vector<Token> &toks) {
    std::vector<TokenRef> refs;
    size_t i = 0;
    while (i < toks.size()) {
        TokenRef r;
        while (i < toks.size() && toks[i].IsTrivia()) r.leading.push_back(std::move(toks[i++]));
        if (i >= toks.size()) break;
        r.tok = std::move(toks[i++]);
        const bool eof = r.tok.type == TT::Eof;
        if (!eof) {
            while (i < toks.size() && toks[i].IsTrivia() && toks[i].type != TT::Shebang) {
                const bool nl = toks[i].type == TT::Whitespace && toks[i].text.find('\n') != std::string::npos;
                r.trailing.push_back(std::move(toks[i++]));
                if (nl) break;
            }
        }
        refs.push_back(std::move(r));
        if (eof) break;
    }
    return refs;
}

int BinPrec(const Token &t) {
    if (t.type != TT::Symbol) return -1;
    const std::string &s = t.text;
    if (s == "^") return 12;
    if (s == "%" || s == "/" || s == "*" || s == "//") return 10;
    if (s == "-" || s == "+") return 9;
    if (s == "..") return 8;
    if (s == "<<" || s == ">>") return 7;
    if (s == "&") return 6;
    if (s == "~") return 5;
    if (s == "|") return 4;
    if (s == ">" || s == ">=" || s == "<" || s == "<=" || s == "~=" || s == "==") return 3;
    if (s == "and") return 2;
    if (s == "or") return 1;
    return -1;
}

bool IsRightAssoc(const Token &t) { return t.type == TT::Symbol && (t.text == "^" || t.text == ".."); }

constexpr int kUnaryPrec = 11;

// Deeper nesting than this (counting operator chains, whose trees are as
// deep as they are long) is refused rather than risking the stack: the
// formatter recurses through the tree several frames per level.
constexpr int kMaxDepth = 400;

struct ParseFail {};

class Parser {
  public:
    Parser(std::vector<TokenRef> refs, std::string_view src) : r_(std::move(refs)), src_(src) {}

    const TokenRef &Cur() const { return r_[i_]; }
    bool Is(std::string_view sym) const { return r_[i_].tok.IsSymbol(sym); }
    bool IsIdent() const { return r_[i_].tok.type == TT::Identifier; }
    bool PeekIs(std::string_view sym) const { return i_ + 1 < r_.size() && r_[i_ + 1].tok.IsSymbol(sym); }
    TokenRef Take() { return r_[i_++]; }
    TokenRef Expect(std::string_view sym) {
        if (!Is(sym)) Error("expected `" + std::string(sym) + "`");
        return Take();
    }
    TokenRef ExpectName() {
        if (!IsIdent()) Error("expected a name");
        return Take();
    }
    std::optional<TokenRef> TakeIf(std::string_view sym) {
        if (Is(sym)) return Take();
        return std::nullopt;
    }

    [[noreturn]] void Error(const std::string &msg) {
        err.message = msg + " near `" + TokenText(Cur().tok) + "`";
        int line = 1;
        const size_t at = Cur().tok.start;
        for (size_t k = 0; k < at && k < src_.size(); k++)
            if (src_[k] == '\n') line++;
        err.line = line;
        throw ParseFail{};
    }

    struct DepthGuard {
        Parser &p;
        explicit DepthGuard(Parser &pp) : p(pp) {
            if (++p.depth_ > kMaxDepth) p.Error("too deeply nested");
        }
        ~DepthGuard() { p.depth_--; }
        DepthGuard(const DepthGuard &) = delete;
        DepthGuard &operator=(const DepthGuard &) = delete;
    };

    Block ParseBlock() {
        DepthGuard guard(*this);
        Block b;
        for (;;) {
            if (Is("return")) {
                LastStmt ls;
                ls.token = Take();
                if (StartsExpr()) ls.returns = ParseExprList();
                auto semi = TakeIf(";");
                b.last = std::pair<LastStmt, std::optional<TokenRef>>(std::move(ls), std::move(semi));
                return b;
            }
            if (Is("break")) {
                LastStmt ls;
                ls.is_break = true;
                ls.token = Take();
                auto semi = TakeIf(";");
                b.last = std::pair<LastStmt, std::optional<TokenRef>>(std::move(ls), std::move(semi));
                return b;
            }
            std::optional<Stmt> s = ParseStmt();
            if (!s) return b;
            StmtSemi ss;
            ss.stmt = std::move(*s);
            ss.semi = TakeIf(";");
            b.stmts.push_back(std::move(ss));
        }
    }

    bool StartsExpr() const {
        const Token &t = Cur().tok;
        if (t.type == TT::Number || t.type == TT::String || t.type == TT::Identifier) return true;
        if (t.type != TT::Symbol) return false;
        const std::string &s = t.text;
        return s == "function" || s == "true" || s == "false" || s == "nil" || s == "..." || s == "(" || s == "{" ||
               s == "-" || s == "not" || s == "#" || s == "~";
    }

    std::optional<Stmt> ParseStmt() {
        Stmt s;
        if (Is("local")) {
            s.local_token = Take();
            if (Is("function")) {
                s.kind = SK::LocalFunction;
                s.function_token = Take();
                s.name = ExpectName();
                s.body = ParseFuncBody();
                return s;
            }
            s.kind = SK::Local;
            for (;;) {
                TokenRef n = ExpectName();
                std::optional<Attribute> attr;
                if (Is("<")) {
                    Attribute a;
                    a.brackets.open = Take();
                    a.name = ExpectName();
                    a.brackets.close = Expect(">");
                    attr = std::move(a);
                }
                s.attributes.push_back(std::move(attr));
                if (Is(",")) {
                    TokenRef c = Take();
                    s.names.push(std::move(n), std::move(c));
                } else {
                    s.names.push(std::move(n));
                    break;
                }
            }
            bool any = false;
            for (const auto &a : s.attributes) any = any || a.has_value();
            if (!any) s.attributes.clear();
            if (Is("=")) {
                s.equal = Take();
                s.exprs = ParseExprList();
            }
            return s;
        }
        if (Is("do")) {
            s.kind = SK::Do;
            s.first = Take();
            s.block = ParseBlock();
            s.end_token = Expect("end");
            return s;
        }
        if (Is("while")) {
            s.kind = SK::While;
            s.first = Take();
            s.cond = ParseExpr();
            s.do_token = Expect("do");
            s.block = ParseBlock();
            s.end_token = Expect("end");
            return s;
        }
        if (Is("repeat")) {
            s.kind = SK::Repeat;
            s.first = Take();
            s.block = ParseBlock();
            s.end_token = Expect("until");
            s.cond = ParseExpr();
            return s;
        }
        if (Is("if")) {
            s.kind = SK::If;
            s.first = Take();
            s.cond = ParseExpr();
            s.do_token = Expect("then");
            s.block = ParseBlock();
            while (Is("elseif")) {
                ElseIf ei;
                ei.elseif_token = Take();
                ei.cond = ParseExpr();
                ei.then_token = Expect("then");
                ei.block = ParseBlock();
                s.elseifs.push_back(std::move(ei));
            }
            if (Is("else")) {
                s.else_token = Take();
                s.else_block = ParseBlock();
            }
            s.end_token = Expect("end");
            return s;
        }
        if (Is("for")) {
            s.first = Take();
            TokenRef n = ExpectName();
            if (Is("=")) {
                s.kind = SK::NumericFor;
                s.index_var = std::move(n);
                s.equal = Take();
                s.cond = ParseExpr();
                s.comma1 = Expect(",");
                s.limit = ParseExpr();
                if (Is(",")) {
                    s.comma2 = Take();
                    s.step = ParseExpr();
                }
                s.do_token = Expect("do");
                s.block = ParseBlock();
                s.end_token = Expect("end");
                return s;
            }
            s.kind = SK::GenericFor;
            for (;;) {
                if (Is(",")) {
                    TokenRef c = Take();
                    s.names.push(std::move(n), std::move(c));
                    n = ExpectName();
                } else {
                    s.names.push(std::move(n));
                    break;
                }
            }
            s.in_token = Expect("in");
            s.exprs = ParseExprList();
            s.do_token = Expect("do");
            s.block = ParseBlock();
            s.end_token = Expect("end");
            return s;
        }
        if (Is("function")) {
            s.kind = SK::FunctionDecl;
            s.first = Take();
            TokenRef n = ExpectName();
            for (;;) {
                if (Is(".")) {
                    TokenRef d = Take();
                    s.fname.names.push(std::move(n), std::move(d));
                    n = ExpectName();
                } else {
                    s.fname.names.push(std::move(n));
                    break;
                }
            }
            if (Is(":")) {
                TokenRef c = Take();
                TokenRef m = ExpectName();
                s.fname.method = std::make_pair(std::move(c), std::move(m));
            }
            s.body = ParseFuncBody();
            return s;
        }
        if (Is("goto") && i_ + 1 < r_.size() && r_[i_ + 1].tok.type == TT::Identifier) {
            s.kind = SK::Goto;
            s.first = Take();
            s.name = ExpectName();
            return s;
        }
        if (Is("::")) {
            s.kind = SK::Label;
            s.first = Take();
            s.name = ExpectName();
            s.end_token = Expect("::");
            return s;
        }
        if (IsIdent() || Is("(")) {
            FunctionCall pc = ParsePrefixSuffixes();
            if (Is("=") || Is(",")) {
                s.kind = SK::Assignment;
                Var v = ToVar(std::move(pc));
                for (;;) {
                    if (Is(",")) {
                        TokenRef c = Take();
                        s.vars.push(std::move(v), std::move(c));
                        if (!(IsIdent() || Is("("))) Error("expected a variable");
                        v = ToVar(ParsePrefixSuffixes());
                    } else {
                        s.vars.push(std::move(v));
                        break;
                    }
                }
                s.equal = Expect("=");
                s.exprs = ParseExprList();
                return s;
            }
            if (pc.suffixes.empty() || !pc.suffixes.back().IsCall()) Error("expected a function call or assignment");
            s.kind = SK::Call;
            s.call = std::move(pc);
            return s;
        }
        return std::nullopt;
    }

    Var ToVar(FunctionCall pc) {
        Var v;
        if (pc.suffixes.empty()) {
            if (!pc.prefix.is_name) Error("cannot assign to a parenthesized expression");
            v.is_name = true;
            v.name = std::move(pc.prefix.name);
            return v;
        }
        if (pc.suffixes.back().IsCall()) Error("cannot assign to a function call");
        v.is_name = false;
        v.expr = std::move(pc);
        return v;
    }

    Box<FunctionBody> ParseFuncBody() {
        FunctionBody b;
        b.parens.open = Expect("(");
        if (!Is(")")) {
            for (;;) {
                TokenRef p;
                if (Is("...")) {
                    p = Take();
                    b.params.push(std::move(p));
                    break;
                }
                p = ExpectName();
                if (Is(",")) {
                    TokenRef c = Take();
                    b.params.push(std::move(p), std::move(c));
                } else {
                    b.params.push(std::move(p));
                    break;
                }
            }
        }
        b.parens.close = Expect(")");
        b.block = ParseBlock();
        b.end_token = Expect("end");
        return b;
    }

    Punctuated<Expr> ParseExprList() {
        Punctuated<Expr> l;
        for (;;) {
            Expr e = ParseExpr();
            if (Is(",")) {
                TokenRef c = Take();
                l.push(std::move(e), std::move(c));
            } else {
                l.push(std::move(e));
                return l;
            }
        }
    }

    FunctionCall ParsePrefixSuffixes() {
        FunctionCall c;
        if (Is("(")) {
            Expr p;
            p.kind = EK::Paren;
            p.contained.open = Take();
            p.rhs = ParseExpr();
            p.contained.close = Expect(")");
            c.prefix.is_name = false;
            c.prefix.expr = std::move(p);
        } else {
            c.prefix.is_name = true;
            c.prefix.name = ExpectName();
        }
        for (;;) {
            Suffix s;
            if (Is(".")) {
                s.kind = Suffix::kIndex;
                s.index.is_dot = true;
                s.index.dot = Take();
                s.index.name = ExpectName();
            } else if (Is("[")) {
                s.kind = Suffix::kIndex;
                s.index.is_dot = false;
                s.index.brackets.open = Take();
                s.index.expr = ParseExpr();
                s.index.brackets.close = Expect("]");
            } else if (Is(":")) {
                s.kind = Suffix::kMethod;
                s.colon = Take();
                s.method_name = ExpectName();
                if (!ParseArgs(s.args)) Error("expected function arguments");
            } else if (Is("(") || Is("{") || Cur().tok.type == TT::String) {
                s.kind = Suffix::kAnon;
                ParseArgs(s.args);
            } else {
                break;
            }
            c.suffixes.push_back(std::move(s));
        }
        return c;
    }

    bool ParseArgs(FunctionArgs &a) {
        if (Is("(")) {
            a.kind = FunctionArgs::Parens;
            a.parens.open = Take();
            Punctuated<Expr> args;
            if (!Is(")")) args = ParseExprList();
            a.args = std::move(args);
            a.parens.close = Expect(")");
            return true;
        }
        if (Is("{")) {
            a.kind = FunctionArgs::Table;
            a.table = ParseTable();
            return true;
        }
        if (Cur().tok.type == TT::String) {
            a.kind = FunctionArgs::String;
            a.str = Take();
            return true;
        }
        return false;
    }

    TableConstructor ParseTable() {
        TableConstructor t;
        t.braces.open = Expect("{");
        while (!Is("}")) {
            Field f;
            if (Is("[")) {
                f.kind = Field::ExprKey;
                f.brackets.open = Take();
                f.key = ParseExpr();
                f.brackets.close = Expect("]");
                f.equal = Expect("=");
                f.value = ParseExpr();
            } else if (IsIdent() && PeekIs("=")) {
                f.kind = Field::NameKey;
                f.name = Take();
                f.equal = Take();
                f.value = ParseExpr();
            } else {
                f.kind = Field::NoKey;
                f.value = ParseExpr();
            }
            if (Is(",") || Is(";")) {
                TokenRef sep = Take();
                t.fields.push(std::move(f), std::move(sep));
            } else {
                t.fields.push(std::move(f));
                break;
            }
        }
        t.braces.close = Expect("}");
        return t;
    }

    Expr ParsePrimary() {
        DepthGuard guard(*this);
        const Token &t = Cur().tok;
        Expr e;
        if (t.type == TT::Number) {
            e.kind = EK::Number;
            e.tok = Take();
            return e;
        }
        if (t.type == TT::String) {
            e.kind = EK::String;
            e.tok = Take();
            return e;
        }
        if (t.type == TT::Symbol) {
            const std::string &s = t.text;
            if (s == "function") {
                AnonFn f;
                f.function_token = Take();
                f.body = ParseFuncBody();
                e.kind = EK::Function;
                e.func = std::move(f);
                return e;
            }
            if (s == "true" || s == "false" || s == "nil" || s == "...") {
                e.kind = EK::Symbol;
                e.tok = Take();
                return e;
            }
            if (s == "{") {
                e.kind = EK::Table;
                e.table = ParseTable();
                return e;
            }
            if (s == "-" || s == "not" || s == "#" || s == "~") {
                e.kind = EK::Unary;
                e.tok = Take();
                Expr operand = ParsePrimary();
                e.rhs = ParseExprPrec(std::move(operand), kUnaryPrec);
                return e;
            }
        }
        if (t.type == TT::Identifier || t.IsSymbol("(")) {
            FunctionCall c = ParsePrefixSuffixes();
            if (c.suffixes.empty()) {
                if (c.prefix.is_name) {
                    e.kind = EK::Var;
                    Var v;
                    v.is_name = true;
                    v.name = std::move(c.prefix.name);
                    e.var = std::move(v);
                    return e;
                }
                return std::move(*c.prefix.expr);
            }
            if (c.suffixes.back().IsCall()) {
                e.kind = EK::Call;
                e.call = std::move(c);
                return e;
            }
            e.kind = EK::Var;
            Var v;
            v.is_name = false;
            v.expr = std::move(c);
            e.var = std::move(v);
            return e;
        }
        Error("expected an expression");
    }

    Expr ParseExprPrec(Expr lhs, int prec) {
        DepthGuard guard(*this);
        int chain = 0;
        for (;;) {
            if (depth_ + chain > kMaxDepth) Error("too deeply nested");
            const int p = BinPrec(Cur().tok);
            if (p < 0 || p < prec) return lhs;
            TokenRef op = Take();
            Expr rhs = ParsePrimary();
            for (;;) {
                const int np = BinPrec(Cur().tok);
                if (np < 0) break;
                int search = 0;
                if (np > p) {
                    search = p + 1;
                } else if (IsRightAssoc(Cur().tok) && np == p) {
                    search = p;
                } else {
                    break;
                }
                rhs = ParseExprPrec(std::move(rhs), search);
            }
            Expr b;
            b.kind = EK::Binary;
            b.lhs = std::move(lhs);
            b.tok = std::move(op);
            b.rhs = std::move(rhs);
            lhs = std::move(b);
            chain++;
        }
    }

    Expr ParseExpr() { return ParseExprPrec(ParsePrimary(), 0); }

    std::vector<TokenRef> r_;
    size_t i_ = 0;
    std::string_view src_;
    ParseError err;
    int depth_ = 0;
};

}  // namespace

bool Parse(std::string_view src, Ast &out, ParseError &err) {
    std::vector<Token> toks;
    if (!Lex(src, toks, err)) return false;
    Parser p(Attach(toks), src);
    try {
        out.block = p.ParseBlock();
        if (p.Cur().tok.type != TT::Eof) p.Error("unexpected token");
        out.eof = p.Cur();
    } catch (const ParseFail &) {
        err = p.err;
        return false;
    }
    return true;
}

// ---- printing -----------------------------------------------------------------

namespace {

struct Printer {
    std::string out;
    bool operator()(const TokenRef &t) {
        for (const auto &x : t.leading) AppendTokenText(out, x);
        AppendTokenText(out, t.tok);
        for (const auto &x : t.trailing) AppendTokenText(out, x);
        return false;
    }
};

template <class T>
std::string PrintAny(const T &x) {
    Printer p;
    Walk(x, p, false);
    return std::move(p.out);
}

}  // namespace

std::string Print(const TokenRef &t) { return PrintAny(t); }
std::string Print(const Expr &e) { return PrintAny(e); }
std::string Print(const Block &b) { return PrintAny(b); }
std::string Print(const Stmt &s) { return PrintAny(s); }
std::string Print(const LastStmt &s) { return PrintAny(s); }
std::string Print(const Suffix &s) { return PrintAny(s); }
std::string Print(const Prefix &p) { return PrintAny(p); }
std::string Print(const Var &v) { return PrintAny(v); }
std::string Print(const FunctionCall &c) { return PrintAny(c); }
std::string Print(const Field &f) { return PrintAny(f); }
std::string Print(const TableConstructor &t) { return PrintAny(t); }
std::string Print(const FunctionArgs &a) { return PrintAny(a); }
std::string Print(const FunctionBody &b) { return PrintAny(b); }
std::string Print(const Attribute &a) { return PrintAny(a); }
std::string Print(const FunctionName &n) { return PrintAny(n); }
std::string Print(const Punctuated<Expr> &p) { return PrintAny(p); }
std::string Print(const Punctuated<Var> &p) { return PrintAny(p); }
std::string Print(const Punctuated<TokenRef> &p) { return PrintAny(p); }
std::string Print(const ContainedSpan &c) { return PrintAny(c); }
std::string Print(const Ast &a) { return PrintAny(a.block) + PrintAny(a.eof); }

// ---- first / last tokens ------------------------------------------------------------

TokenRef *FirstTok(Expr &e) {
    switch (e.kind) {
    case EK::Binary:
        return FirstTok(*e.lhs);
    case EK::Paren:
        return &e.contained.open;
    case EK::Unary:
    case EK::Number:
    case EK::String:
    case EK::Symbol:
        return &e.tok;
    case EK::Function:
        return &e.func->function_token;
    case EK::Call:
        return FirstTok(*e.call);
    case EK::Table:
        return &e.table->braces.open;
    case EK::Var:
        return FirstTok(*e.var);
    }
    return nullptr;
}

TokenRef *LastTok(Expr &e) {
    switch (e.kind) {
    case EK::Binary:
    case EK::Unary:
        return LastTok(*e.rhs);
    case EK::Paren:
        return &e.contained.close;
    case EK::Number:
    case EK::String:
    case EK::Symbol:
        return &e.tok;
    case EK::Function:
        return &e.func->body->end_token;
    case EK::Call:
        return LastTok(*e.call);
    case EK::Table:
        return &e.table->braces.close;
    case EK::Var:
        return LastTok(*e.var);
    }
    return nullptr;
}

TokenRef *FirstTok(Prefix &p) { return p.is_name ? &p.name : FirstTok(*p.expr); }
TokenRef *LastTok(Prefix &p) { return p.is_name ? &p.name : LastTok(*p.expr); }

TokenRef *FirstTok(FunctionArgs &a) {
    switch (a.kind) {
    case FunctionArgs::Parens:
        return &a.parens.open;
    case FunctionArgs::String:
        return &a.str;
    case FunctionArgs::Table:
        return &a.table->braces.open;
    }
    return nullptr;
}
TokenRef *LastTok(FunctionArgs &a) {
    switch (a.kind) {
    case FunctionArgs::Parens:
        return &a.parens.close;
    case FunctionArgs::String:
        return &a.str;
    case FunctionArgs::Table:
        return &a.table->braces.close;
    }
    return nullptr;
}

TokenRef *FirstTok(Suffix &s) {
    switch (s.kind) {
    case Suffix::kAnon:
        return FirstTok(s.args);
    case Suffix::kMethod:
        return &s.colon;
    case Suffix::kIndex:
        return s.index.is_dot ? &s.index.dot : &s.index.brackets.open;
    }
    return nullptr;
}
TokenRef *LastTok(Suffix &s) {
    switch (s.kind) {
    case Suffix::kAnon:
    case Suffix::kMethod:
        return LastTok(s.args);
    case Suffix::kIndex:
        return s.index.is_dot ? &s.index.name : &s.index.brackets.close;
    }
    return nullptr;
}

TokenRef *FirstTok(FunctionCall &c) { return FirstTok(c.prefix); }
TokenRef *LastTok(FunctionCall &c) { return c.suffixes.empty() ? LastTok(c.prefix) : LastTok(c.suffixes.back()); }
TokenRef *FirstTok(Var &v) { return v.is_name ? &v.name : FirstTok(*v.expr); }
TokenRef *LastTok(Var &v) { return v.is_name ? &v.name : LastTok(*v.expr); }

TokenRef *FirstTok(Field &f) {
    switch (f.kind) {
    case Field::ExprKey:
        return &f.brackets.open;
    case Field::NameKey:
        return &f.name;
    case Field::NoKey:
        return FirstTok(*f.value);
    }
    return nullptr;
}
TokenRef *LastTok(Field &f) { return LastTok(*f.value); }
TokenRef *FirstTok(TableConstructor &t) { return &t.braces.open; }
TokenRef *LastTok(TableConstructor &t) { return &t.braces.close; }
TokenRef *FirstTok(FunctionBody &b) { return &b.parens.open; }
TokenRef *LastTok(FunctionBody &b) { return &b.end_token; }
TokenRef *FirstTok(Attribute &a) { return &a.brackets.open; }
TokenRef *LastTok(Attribute &a) { return &a.brackets.close; }
TokenRef *FirstTok(FunctionName &n) { return FirstTok(n.names); }
TokenRef *LastTok(FunctionName &n) { return n.method ? &n.method->second : LastTok(n.names); }
TokenRef *FirstTok(LastStmt &s) { return &s.token; }
TokenRef *LastTok(LastStmt &s) { return s.returns.empty() ? &s.token : LastTok(s.returns); }

TokenRef *FirstTok(Stmt &s) {
    switch (s.kind) {
    case SK::Assignment:
        return FirstTok(s.vars);
    case SK::Local:
    case SK::LocalFunction:
        return &s.local_token;
    case SK::Call:
        return FirstTok(*s.call);
    default:
        return &s.first;
    }
}

TokenRef *LastTok(Stmt &s) {
    switch (s.kind) {
    case SK::Assignment:
        return LastTok(s.exprs);
    case SK::Local:
        if (!s.exprs.empty()) return LastTok(s.exprs);
        if (!s.attributes.empty() && s.attributes.back()) return LastTok(*s.attributes.back());
        return LastTok(s.names);
    case SK::Call:
        return LastTok(*s.call);
    case SK::Repeat:
        return LastTok(*s.cond);
    case SK::FunctionDecl:
    case SK::LocalFunction:
        return &s.body->end_token;
    case SK::Goto:
        return &s.name;
    default:
        return &s.end_token;
    }
}

}  // namespace luafmt::cst
