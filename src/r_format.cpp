#include "r_format.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rfmt {
namespace {

// ---------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------

enum class T {
    Eof,
    Comment,
    Ident,   // identifiers, keywords and `backtick names` alike
    Num,
    Str,
    Op,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBrack,
    LBrack2,  // `[[`
    RBrack,
    Comma,
    Semi,
};

struct Tok {
    T kind = T::Eof;
    std::string text;
    int line = 1;
    // Line breaks between the previous token and this one. 0 means "same
    // line" -- the whole of R's newline sensitivity (and comment
    // attachment) is decided from this one number.
    int newlines = 0;
    // Fully blank lines in that gap (newlines - 1, floored at 0), i.e.
    // what the printer reproduces as a blank line.
    int blanks = 0;
};

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHexDigit(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
// R identifiers: letters, digits, '.' and '_', not starting with a digit
// (or with '_'). Bytes >= 0x80 are accepted so a UTF-8 identifier (R
// allows them) survives untouched.
bool IsIdentStart(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c >= 0x80;
}
bool IsIdentChar(unsigned char c) {
    return IsIdentStart(c) || IsDigit(static_cast<char>(c)) || c == '_';
}

// The multi-character operators, longest first so the scan below can take
// the first match. `**` is R's deprecated synonym for `^` and is lexed as
// one token (it is normalized to `^` when printed, which is what R's own
// parser does to it).
const char *const kMultiOps[] = {":::", "<<-", "->>", "::", "<-", "->", "<=",
                                 ">=",  "==",  "!=",  "&&", "||", "|>", "**"};

struct Lexer {
    const std::string &src;
    size_t i = 0;
    int line = 1;
    std::string error;
    int error_line = 0;

    explicit Lexer(const std::string &s) : src(s) {}

    char Cur() const { return i < src.size() ? src[i] : '\0'; }
    char At(size_t off) const { return i + off < src.size() ? src[i + off] : '\0'; }

    void Fail(const std::string &msg) {
        if (error.empty()) {
            error = msg;
            error_line = line;
        }
    }

    // A quoted string, including R's raw strings: r"(...)", R"[...]",
    // r"---(...)---". Returns the literal's full source text (delimiters
    // included) -- normalization happens at print time, not here.
    std::string ScanString() {
        size_t start = i;
        char q = Cur();
        if (q == 'r' || q == 'R') {
            // Raw string: r<quote><dashes><open> ... <close><dashes><quote>.
            size_t j = i + 1;
            char quote = j < src.size() ? src[j] : '\0';
            if (quote != '"' && quote != '\'') return std::string();  // not a raw string
            j++;
            size_t dashes = 0;
            while (j + dashes < src.size() && src[j + dashes] == '-') dashes++;
            j += dashes;
            char open = j < src.size() ? src[j] : '\0';
            char close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}' : '\0';
            if (close == '\0') return std::string();
            j++;
            std::string terminator(1, close);
            terminator.append(dashes, '-');
            terminator.push_back(quote);
            size_t end = src.find(terminator, j);
            if (end == std::string::npos) {
                Fail("unterminated raw string");
                i = src.size();
                return src.substr(start);
            }
            for (size_t k = start; k < end + terminator.size(); k++)
                if (src[k] == '\n') line++;
            i = end + terminator.size();
            return src.substr(start, i - start);
        }
        i++;  // opening quote
        while (i < src.size()) {
            char c = src[i];
            if (c == '\\') {
                if (src[i + 1] == '\n') line++;
                i += 2;
                continue;
            }
            if (c == q) {
                i++;
                return src.substr(start, i - start);
            }
            if (c == '\n') line++;
            i++;
        }
        Fail("unterminated string");
        return src.substr(start);
    }

    std::vector<Tok> Run() {
        std::vector<Tok> toks;
        int pending_newlines = 0;
        auto push = [&](T kind, std::string text, int tok_line) {
            Tok t;
            t.kind = kind;
            t.text = std::move(text);
            t.line = tok_line;
            t.newlines = pending_newlines;
            t.blanks = pending_newlines > 1 ? pending_newlines - 1 : 0;
            pending_newlines = 0;
            toks.push_back(std::move(t));
        };
        // The first token is at the top of the file, not after a line
        // break, but leading blank lines in the file are still dropped by
        // starting `pending_newlines` at 0.
        while (i < src.size()) {
            char c = src[i];
            if (c == '\n') {
                pending_newlines++;
                line++;
                i++;
                continue;
            }
            if (IsSpace(c)) {
                i++;
                continue;
            }
            int tok_line = line;
            if (c == '#') {
                size_t start = i;
                while (i < src.size() && src[i] != '\n') i++;
                push(T::Comment, src.substr(start, i - start), tok_line);
                continue;
            }
            if (c == '"' || c == '\'') {
                push(T::Str, ScanString(), tok_line);
                continue;
            }
            // A raw string only when the quote really follows the r/R --
            // otherwise this is an ordinary identifier starting with r.
            if ((c == 'r' || c == 'R') && (At(1) == '"' || At(1) == '\'')) {
                std::string s = ScanString();
                if (!s.empty()) {
                    push(T::Str, std::move(s), tok_line);
                    continue;
                }
            }
            if (IsDigit(c) || (c == '.' && IsDigit(At(1)))) {
                size_t start = i;
                if (c == '0' && (At(1) == 'x' || At(1) == 'X')) {
                    i += 2;
                    while (IsHexDigit(Cur()) || Cur() == '.') i++;
                    if (Cur() == 'p' || Cur() == 'P') {
                        i++;
                        if (Cur() == '+' || Cur() == '-') i++;
                        while (IsDigit(Cur())) i++;
                    }
                } else {
                    while (IsDigit(Cur()) || Cur() == '.') i++;
                    if (Cur() == 'e' || Cur() == 'E') {
                        i++;
                        if (Cur() == '+' || Cur() == '-') i++;
                        while (IsDigit(Cur())) i++;
                    }
                }
                if (Cur() == 'L' || Cur() == 'i') i++;
                push(T::Num, src.substr(start, i - start), tok_line);
                continue;
            }
            if (c == '`') {
                size_t start = i;
                i++;
                while (i < src.size() && src[i] != '`') {
                    if (src[i] == '\\') i++;
                    if (i < src.size() && src[i] == '\n') line++;
                    i++;
                }
                if (i >= src.size()) {
                    Fail("unterminated backtick name");
                } else {
                    i++;
                }
                push(T::Ident, src.substr(start, i - start), tok_line);
                continue;
            }
            if (IsIdentStart(static_cast<unsigned char>(c))) {
                size_t start = i;
                while (i < src.size() && IsIdentChar(static_cast<unsigned char>(src[i]))) i++;
                push(T::Ident, src.substr(start, i - start), tok_line);
                continue;
            }
            // %any% -- R's user-definable infix operators (%%, %/%, %in%,
            // %*%, and magrittr's %>%). An unterminated one is an error
            // rather than a run to end of file.
            if (c == '%') {
                size_t start = i;
                size_t j = i + 1;
                while (j < src.size() && src[j] != '%' && src[j] != '\n') j++;
                if (j >= src.size() || src[j] != '%') {
                    Fail("unterminated %operator%");
                    i++;
                    push(T::Op, "%", tok_line);
                    continue;
                }
                i = j + 1;
                push(T::Op, src.substr(start, i - start), tok_line);
                continue;
            }
            switch (c) {
                case '(': i++; push(T::LParen, "(", tok_line); continue;
                case ')': i++; push(T::RParen, ")", tok_line); continue;
                case '{': i++; push(T::LBrace, "{", tok_line); continue;
                case '}': i++; push(T::RBrace, "}", tok_line); continue;
                case ']': i++; push(T::RBrack, "]", tok_line); continue;
                case ',': i++; push(T::Comma, ",", tok_line); continue;
                case ';': i++; push(T::Semi, ";", tok_line); continue;
                case '[':
                    if (At(1) == '[') {
                        i += 2;
                        push(T::LBrack2, "[[", tok_line);
                    } else {
                        i++;
                        push(T::LBrack, "[", tok_line);
                    }
                    continue;
                default: break;
            }
            bool matched = false;
            for (const char *op : kMultiOps) {
                size_t n = std::char_traits<char>::length(op);
                if (src.compare(i, n, op) == 0) {
                    i += n;
                    push(T::Op, op, tok_line);
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
            if (std::string("+-*/^<>!&|~?:=@$\\").find(c) != std::string::npos) {
                i++;
                push(T::Op, std::string(1, c), tok_line);
                continue;
            }
            Fail(std::string("unexpected character '") + c + "'");
            i++;
        }
        Tok eof;
        eof.kind = T::Eof;
        eof.line = line;
        eof.newlines = pending_newlines;
        eof.blanks = pending_newlines > 1 ? pending_newlines - 1 : 0;
        toks.push_back(eof);
        return toks;
    }
};

// ---------------------------------------------------------------------
// AST
// ---------------------------------------------------------------------

enum class NK {
    Leaf,     // identifier, number, string literal, break, next
    Paren,    // ( expr )
    Block,    // { stmts }
    Unary,    // text = operator, kids[0] = operand
    Binary,   // text = operator, kids[0] = lhs, kids[1] = rhs
    Call,     // kids[0] = callee, args
    Index,    // kids[0] = object, args, bracket2 = `[[`
    Func,     // text = "function" or "\\", args = parameters, kids[0] = body
    If,       // kids = cond, then, [else]
    For,      // kids = var, seq, body
    While,    // kids = cond, body
    Repeat,   // kids = body
};

// A comment, with the blank lines that preceded it so the printer can
// put them back.
struct Cmt {
    std::string text;
    int blanks = 0;
};

struct Node;
using P = std::unique_ptr<Node>;

// One entry in an argument list (a call's, an index's, or a function
// definition's parameters). `value` is null for R's missing argument --
// the empty slot in `x[, 1]` / `x[1, ]`, which is meaningful, not noise.
struct Arg {
    P name;  // null unless `name = value`
    P value;
    std::vector<Cmt> leading;
    std::string trailing;
};

struct Node {
    NK kind = NK::Leaf;
    std::string text;
    std::vector<P> kids;
    std::vector<Arg> args;
    // Comments on their own line(s) immediately before this node.
    std::vector<Cmt> leading;
    // A comment on the same line as this node's last token (statement and
    // argument positions only -- elsewhere a comment becomes the `leading`
    // of whatever follows it).
    std::string trailing;
    // Comments sitting just before a closing delimiter with no expression
    // after them: the `# done` in `f(\n  a,\n  # done\n)`.
    std::vector<Cmt> tail;
    // A comment written on the same line as this node's *operator*,
    // between it and the right-hand operand: the `# why` in
    // `total <- a + # why` / `x |> # step one`. Binary nodes only.
    std::string op_trailing;
    // Blank lines between this node's last leading comment (or, with no
    // comments, whatever came before) and its own first token.
    int blanks = 0;
    // Blank lines before the whole statement -- set only for nodes in
    // statement position, where reproducing them is the point.
    int stmt_blanks = 0;
    bool bracket2 = false;
};

P MakeNode(NK kind, std::string text = std::string()) {
    P n(new Node());
    n->kind = kind;
    n->text = std::move(text);
    return n;
}

// ---------------------------------------------------------------------
// Operator table (?Syntax, lowest precedence first)
// ---------------------------------------------------------------------

// 0 = not a binary operator. The numbers themselves only matter relative
// to each other and to the unary precedences in ParseUnary below.
int BinPrec(const std::string &op) {
    if (op == "?") return 1;
    if (op == "=") return 2;
    if (op == "<-" || op == "<<-") return 3;
    if (op == "->" || op == "->>") return 4;
    if (op == "~") return 5;
    if (op == "|" || op == "||") return 6;
    if (op == "&" || op == "&&") return 7;
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") return 9;
    if (op == "+" || op == "-") return 10;
    if (op == "*" || op == "/") return 11;
    if (op == "|>") return 12;
    if (op.size() >= 2 && op.front() == '%' && op.back() == '%') return 12;
    if (op == ":") return 13;
    if (op == "^" || op == "**") return 15;
    return 0;
}

bool RightAssoc(const std::string &op) {
    return op == "=" || op == "<-" || op == "<<-" || op == "^" || op == "**" || op == "~";
}

// Operators printed without surrounding spaces (tidyverse style).
bool TightOp(const std::string &op) {
    return op == "^" || op == "**" || op == ":" || op == "::" || op == ":::" || op == "$" || op == "@";
}

// Statement-position `=` is R's other assignment operator and becomes
// `<-` (tidyverse style, and what air does). A named argument's `=` is a
// different thing entirely and is never touched -- it is not a statement,
// so it never reaches here.
void NormalizeStatementAssign(Node *n) {
    if (n->kind != NK::Binary) return;
    if (n->text != "=") return;
    n->text = "<-";
    NormalizeStatementAssign(n->kids[1].get());
}

// A statement's leading comments are collected by the first expression
// parsed inside it, which for `# note\nx <- 1` is the `x` leaf rather
// than the assignment. Left there they would be comments buried *inside*
// the statement, which forces it to break across lines; hoisting them to
// the statement itself lets it stay on one line with the comments above
// it, which is where they were written.
void HoistLeadingComments(Node *stmt) {
    Node *inner = stmt;
    while (inner->kind == NK::Binary) inner = inner->kids[0].get();
    if (inner == stmt || inner->leading.empty()) return;
    stmt->leading = std::move(inner->leading);
    inner->leading.clear();
    stmt->blanks = inner->blanks;
}

// ---------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------

// How deeply expressions may nest before the parser gives up. The
// parser, the printer and even the AST's own destructor all recurse on
// tree depth, and this formatter runs *inside* the editor -- a
// pathological file must come back as an error, not as a stack overflow
// that takes the whole process with it. Real R never comes close (R's
// own parser has comparable limits); the crash threshold measured here
// is an order of magnitude above this.
const int kMaxNest = 1000;

struct Parser {
    std::vector<Tok> toks;
    size_t p = 0;
    // 0 while newlines are statement separators, >0 inside (), [] or [[]]
    // where R ignores them entirely. `{}` resets it (its body is a
    // statement list again), which is what the save/restore in ParseBlock
    // is for.
    int depth = 0;
    std::vector<Cmt> pending;
    std::string error;
    int error_line = 0;
    // Current expression nesting, maintained by NestGuard below.
    int nest = 0;

    struct NestGuard {
        Parser *owner;
        explicit NestGuard(Parser *o) : owner(o) { owner->nest++; }
        ~NestGuard() { owner->nest--; }
        NestGuard(const NestGuard &) = delete;
        NestGuard &operator=(const NestGuard &) = delete;
        NestGuard(NestGuard &&) = delete;
        NestGuard &operator=(NestGuard &&) = delete;
    };

    const Tok &Cur() const { return toks[p]; }
    bool Failed() const { return !error.empty(); }

    void Fail(const std::string &msg) {
        if (error.empty()) {
            error = msg;
            error_line = Cur().line;
        }
    }

    // Moves every comment at the cursor into `pending`, where the next
    // expression/argument/statement will pick it up as its leading
    // comments. Call sites that can own a *trailing* comment check for
    // one (a comment with newlines == 0) before calling this.
    void Trivia() {
        while (Cur().kind == T::Comment) {
            pending.push_back(Cmt{Cur().text, Cur().blanks});
            p++;
        }
    }

    std::vector<Cmt> TakePending() {
        std::vector<Cmt> out;
        out.swap(pending);
        return out;
    }

    // The nth token from the cursor, skipping comments -- lookahead for
    // "is this `name =`?" decisions, which comments must not disturb.
    const Tok &PeekSig(int n) const {
        size_t i = p;
        int seen = 0;
        while (i < toks.size()) {
            if (toks[i].kind != T::Comment) {
                if (seen == n) return toks[i];
                seen++;
            }
            i++;
        }
        return toks.back();
    }

    bool Expect(T kind, const char *what) {
        Trivia();
        if (Cur().kind == kind) {
            p++;
            return true;
        }
        Fail(std::string("expected ") + what);
        return false;
    }

    bool IsKeyword(const char *kw) const {
        return Cur().kind == T::Ident && Cur().text == kw;
    }

    P ParseExpr(int min_prec) {
        P lhs = ParseUnary();
        if (!lhs) return nullptr;
        // Every operator in a left-associative run deepens the tree by
        // one, without any matching recursion here to be caught by the
        // NestGuard in ParseUnary -- so the run's own length counts too.
        int chain = 0;
        for (;;) {
            // Statement boundary: at depth 0 a line break ends the
            // expression, so `a\n-b` is two statements while `a -\nb` (the
            // operator already consumed) is one.
            if (depth == 0 && Cur().newlines > 0) break;
            if (Cur().kind != T::Op) break;
            std::string op = Cur().text;
            int prec = BinPrec(op);
            if (prec == 0 || prec < min_prec) break;
            if (nest + ++chain > kMaxNest) {
                Fail("expression nested too deeply");
                return nullptr;
            }
            p++;
            // `a + # why` -- the comment belongs to the operator it
            // follows, not to the operand on the next line.
            std::string op_trailing;
            if (Cur().kind == T::Comment && Cur().newlines == 0) {
                op_trailing = Cur().text;
                p++;
            }
            P rhs = ParseExpr(RightAssoc(op) ? prec : prec + 1);
            if (!rhs) return nullptr;
            P bin = MakeNode(NK::Binary, op == "**" ? "^" : op);
            bin->op_trailing = op_trailing;
            bin->kids.push_back(std::move(lhs));
            bin->kids.push_back(std::move(rhs));
            lhs = std::move(bin);
        }
        return lhs;
    }

    P ParseUnary() {
        NestGuard guard(this);
        if (nest > kMaxNest) {
            Fail("expression nested too deeply");
            return nullptr;
        }
        Trivia();
        std::vector<Cmt> lead = TakePending();
        int blanks = Cur().blanks;
        P node;
        if (Cur().kind == T::Op &&
            (Cur().text == "-" || Cur().text == "+" || Cur().text == "!" || Cur().text == "~" ||
             Cur().text == "?")) {
            std::string op = Cur().text;
            // Unary +/- bind tighter than every binary operator except ^;
            // ! sits just above the comparisons; ~ and ? are the two
            // lowest-precedence prefixes.
            int prec = (op == "-" || op == "+") ? 14 : op == "!" ? 8 : op == "~" ? 5 : 1;
            p++;
            P operand = ParseExpr(prec);
            if (!operand) return nullptr;
            node = MakeNode(NK::Unary, op);
            node->kids.push_back(std::move(operand));
        } else {
            node = ParsePrimary();
            if (!node) return nullptr;
        }
        node->leading = std::move(lead);
        node->blanks = blanks;
        return node;
    }

    P ParsePrimary() {
        const Tok &t = Cur();
        switch (t.kind) {
            case T::Num:
            case T::Str: {
                P n = MakeNode(NK::Leaf, t.text);
                p++;
                return ParsePostfix(std::move(n));
            }
            case T::Ident: {
                if (t.text == "if") return ParseIf();
                if (t.text == "for") return ParseFor();
                if (t.text == "while") return ParseWhile();
                if (t.text == "repeat") return ParseRepeat();
                if (t.text == "function") return ParseFunction("function");
                P n = MakeNode(NK::Leaf, t.text);
                p++;
                return ParsePostfix(std::move(n));
            }
            case T::LParen: {
                p++;
                depth++;
                P inner = ParseExpr(1);
                if (!inner) return nullptr;
                P n = MakeNode(NK::Paren);
                n->kids.push_back(std::move(inner));
                Trivia();
                n->tail = TakePending();
                depth--;
                if (!Expect(T::RParen, "')'")) return nullptr;
                return ParsePostfix(std::move(n));
            }
            case T::LBrace: {
                // `{...}` is an ordinary expression in R, and calling or
                // indexing one directly is legal -- base R's own
                // plotmath demo writes `expression({}(x, y))`.
                P b = ParseBlock();
                if (!b) return nullptr;
                return ParsePostfix(std::move(b));
            }
            case T::Op:
                // R's lambda shorthand: \(x) x + 1.
                if (t.text == "\\") return ParseFunction("\\");
                break;
            default:
                break;
        }
        Fail("unexpected '" + (t.kind == T::Eof ? std::string("end of file") : t.text) + "'");
        return nullptr;
    }

    // Calls, indexing and the namespace/extraction operators, which bind
    // tighter than everything else and chain left to right.
    P ParsePostfix(P base) {
        for (;;) {
            if (Failed()) return nullptr;
            // Same statement-boundary rule as the binary loop: at depth 0,
            // `f\n(x)` is two statements, not a call.
            if (depth == 0 && Cur().newlines > 0) break;
            if (Cur().kind == T::LParen) {
                p++;
                P call = MakeNode(NK::Call);
                call->kids.push_back(std::move(base));
                call->args = ParseArgs(T::RParen, call->tail);
                if (!Expect(T::RParen, "')'")) return nullptr;
                base = std::move(call);
                continue;
            }
            if (Cur().kind == T::LBrack || Cur().kind == T::LBrack2) {
                bool dbl = Cur().kind == T::LBrack2;
                p++;
                P idx = MakeNode(NK::Index);
                idx->bracket2 = dbl;
                idx->kids.push_back(std::move(base));
                idx->args = ParseArgs(T::RBrack, idx->tail);
                if (!Expect(T::RBrack, "']'")) return nullptr;
                if (dbl && !Expect(T::RBrack, "']]'")) return nullptr;
                base = std::move(idx);
                continue;
            }
            if (Cur().kind == T::Op &&
                (Cur().text == "$" || Cur().text == "@" || Cur().text == "::" || Cur().text == ":::")) {
                std::string op = Cur().text;
                p++;
                Trivia();
                if (Cur().kind != T::Ident && Cur().kind != T::Str) {
                    Fail("expected a name after '" + op + "'");
                    return nullptr;
                }
                P rhs = MakeNode(NK::Leaf, Cur().text);
                p++;
                P bin = MakeNode(NK::Binary, op);
                bin->kids.push_back(std::move(base));
                bin->kids.push_back(std::move(rhs));
                base = std::move(bin);
                continue;
            }
            break;
        }
        return base;
    }

    std::vector<Arg> ParseArgs(T close, std::vector<Cmt> &tail) {
        std::vector<Arg> args;
        depth++;
        bool after_comma = false;
        for (;;) {
            if (Failed()) break;
            Trivia();
            if (Cur().kind == close || Cur().kind == T::Eof ||
                (close == T::RBrack && Cur().kind == T::RBrace)) {
                // `f(a,)` / `x[1, ]`: the slot after the last comma is a
                // real (missing) argument, and for `[` it changes what the
                // code means, so it is kept rather than dropped.
                if (after_comma) {
                    Arg a;
                    a.leading = TakePending();
                    args.push_back(std::move(a));
                }
                break;
            }
            Arg a;
            a.leading = TakePending();
            // `name = value`, where the name may be an identifier, a
            // string, or `...`. A lone `=` is the only thing this can be
            // -- `==` is a different token.
            if ((Cur().kind == T::Ident || Cur().kind == T::Str) && PeekSig(1).kind == T::Op &&
                PeekSig(1).text == "=") {
                a.name = MakeNode(NK::Leaf, Cur().text);
                p++;
                Trivia();
                p++;  // '='
            }
            Trivia();
            if (Cur().kind != close && Cur().kind != T::Comma) {
                a.value = ParseExpr(1);
                if (!a.value) break;
            }
            if (Cur().kind == T::Comment && Cur().newlines == 0) {
                a.trailing = Cur().text;
                p++;
            }
            after_comma = Cur().kind == T::Comma;
            if (after_comma) {
                p++;
                if (a.trailing.empty() && Cur().kind == T::Comment && Cur().newlines == 0) {
                    a.trailing = Cur().text;
                    p++;
                }
                args.push_back(std::move(a));
                continue;
            }
            args.push_back(std::move(a));
            break;
        }
        Trivia();
        tail = TakePending();
        depth--;
        return args;
    }

    // The statements of a `{}` body or of the whole file.
    std::vector<P> ParseStatements(T stop, std::vector<Cmt> &tail) {
        std::vector<P> stmts;
        for (;;) {
            if (Failed()) break;
            for (;;) {
                Trivia();
                if (Cur().kind == T::Semi) {
                    p++;
                    continue;
                }
                break;
            }
            if (Cur().kind == stop || Cur().kind == T::Eof) break;
            int pre = pending.empty() ? Cur().blanks : pending.front().blanks;
            P e = ParseExpr(1);
            if (!e) break;
            e->stmt_blanks = pre;
            if (Cur().kind == T::Comment && Cur().newlines == 0) {
                e->trailing = Cur().text;
                p++;
            }
            Node *stmt = stmts.emplace_back(std::move(e)).get();
            HoistLeadingComments(stmt);
            NormalizeStatementAssign(stmt);
        }
        Trivia();
        tail = TakePending();
        return stmts;
    }

    P ParseBlock() {
        p++;  // '{'
        int saved = depth;
        depth = 0;
        P n = MakeNode(NK::Block);
        n->kids = ParseStatements(T::RBrace, n->tail);
        depth = saved;
        if (!Expect(T::RBrace, "'}'")) return nullptr;
        return n;
    }

    P ParseIf() {
        p++;  // 'if'
        if (!Expect(T::LParen, "'(' after 'if'")) return nullptr;
        depth++;
        P cond = ParseExpr(1);
        if (!cond) return nullptr;
        Trivia();
        depth--;
        if (!Expect(T::RParen, "')'")) return nullptr;
        P then = ParseExpr(1);
        if (!then) return nullptr;
        P n = MakeNode(NK::If);
        n->kids.push_back(std::move(cond));
        n->kids.push_back(std::move(then));
        // `else` may sit on the next line (inside a block, which is where
        // R allows it); the comments between are kept as the else
        // branch's own leading comments by the usual mechanism.
        size_t save = p;
        std::vector<Cmt> saved_pending = pending;
        Trivia();
        if (IsKeyword("else")) {
            p++;
            P alt = ParseExpr(1);
            if (!alt) return nullptr;
            n->kids.push_back(std::move(alt));
        } else {
            p = save;
            pending = saved_pending;
        }
        return n;
    }

    P ParseFor() {
        p++;  // 'for'
        if (!Expect(T::LParen, "'(' after 'for'")) return nullptr;
        depth++;
        Trivia();
        if (Cur().kind != T::Ident) {
            Fail("expected a loop variable");
            return nullptr;
        }
        P var = MakeNode(NK::Leaf, Cur().text);
        p++;
        Trivia();
        if (!IsKeyword("in")) {
            Fail("expected 'in'");
            return nullptr;
        }
        p++;
        P seq = ParseExpr(1);
        if (!seq) return nullptr;
        Trivia();
        depth--;
        if (!Expect(T::RParen, "')'")) return nullptr;
        P body = ParseExpr(1);
        if (!body) return nullptr;
        P n = MakeNode(NK::For);
        n->kids.push_back(std::move(var));
        n->kids.push_back(std::move(seq));
        n->kids.push_back(std::move(body));
        return n;
    }

    P ParseWhile() {
        p++;  // 'while'
        if (!Expect(T::LParen, "'(' after 'while'")) return nullptr;
        depth++;
        P cond = ParseExpr(1);
        if (!cond) return nullptr;
        Trivia();
        depth--;
        if (!Expect(T::RParen, "')'")) return nullptr;
        P body = ParseExpr(1);
        if (!body) return nullptr;
        P n = MakeNode(NK::While);
        n->kids.push_back(std::move(cond));
        n->kids.push_back(std::move(body));
        return n;
    }

    P ParseRepeat() {
        p++;  // 'repeat'
        P body = ParseExpr(1);
        if (!body) return nullptr;
        P n = MakeNode(NK::Repeat);
        n->kids.push_back(std::move(body));
        return n;
    }

    P ParseFunction(const char *kw) {
        p++;  // 'function' / '\'
        if (!Expect(T::LParen, "'(' after 'function'")) return nullptr;
        P n = MakeNode(NK::Func, kw);
        n->args = ParseArgs(T::RParen, n->tail);
        if (!Expect(T::RParen, "')'")) return nullptr;
        P body = ParseExpr(1);
        if (!body) return nullptr;
        n->kids.push_back(std::move(body));
        return n;
    }
};

// ---------------------------------------------------------------------
// Token-level normalization
// ---------------------------------------------------------------------

std::string RTrim(const std::string &s) {
    size_t e = s.size();
    while (e > 0 && IsSpace(s[e - 1])) e--;
    return s.substr(0, e);
}

// Display width in characters, not bytes: a UTF-8 continuation byte does
// not advance the column, so an identifier or string with non-ASCII text
// is measured the way it looks rather than the way it is stored.
int Width(const std::string &s) {
    int w = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) w++;
    return w;
}

// `#comment` -> `# comment`, trailing whitespace dropped. Roxygen (`#'`)
// and shebang (`#!`) lines keep their exact prefix -- both are read by
// other tools that care about the character right after the hashes -- and
// a run of hashes with nothing after it (`####`, a section rule) is left
// alone too.
std::string NormComment(const std::string &raw) {
    std::string s = RTrim(raw);
    size_t hashes = 0;
    while (hashes < s.size() && s[hashes] == '#') hashes++;
    if (hashes >= s.size()) return s;
    char next = s[hashes];
    if (next == ' ' || next == '\t' || next == '\'' || next == '!') return s;
    return s.substr(0, hashes) + " " + s.substr(hashes);
}

bool IsRawStringToken(const std::string &s) {
    return s.size() >= 2 && (s[0] == 'r' || s[0] == 'R') && (s[1] == '"' || s[1] == '\'');
}

// Single-quoted literals become double-quoted (tidyverse style), except
// when that would mean escaping a `"` the source did not escape -- then
// the original quoting is already the better one and is kept. Raw strings
// are never touched.
std::string NormString(const std::string &s) {
    if (s.size() < 2 || s[0] != '\'' || IsRawStringToken(s)) return s;
    std::string body = s.substr(1, s.size() - 2);
    std::string out = "\"";
    for (size_t i = 0; i < body.size(); i++) {
        if (body[i] == '\\' && i + 1 < body.size()) {
            // \' only needs escaping inside single quotes.
            if (body[i + 1] == '\'') {
                out += '\'';
            } else {
                out += body[i];
                out += body[i + 1];
            }
            i++;
            continue;
        }
        if (body[i] == '"') return s;  // would need new escapes -- leave as is
        out += body[i];
    }
    out += '"';
    return out;
}

std::string NormLeaf(const std::string &s) {
    if (!s.empty() && (s[0] == '"' || s[0] == '\'' || IsRawStringToken(s))) return NormString(s);
    return s;
}

// ---------------------------------------------------------------------
// Printer
// ---------------------------------------------------------------------

struct Printer {
    Options opt;
    std::string out;   // finished lines, each already newline-terminated
    std::string line;  // the line being built, indentation included
    // Probe mode renders a construct on one line to find out whether it
    // fits. Anything that cannot be written without a line break (a
    // block, a comment) sets `broke`, and the caller then knows to use
    // its multi-line layout instead. The non-probe path reuses the
    // probe's own string verbatim when it fits, so the two layouts can
    // never disagree about spacing.
    bool probe = false;
    bool broke = false;

    void PushLine() {
        out += RTrim(line);
        out += '\n';
        line.clear();
    }
    // Moves to `indent` for what comes next, reusing the current line
    // when nothing has been written on it -- an argument list whose last
    // entry is R's missing argument (`x[1, ]`) leaves exactly that empty
    // line behind, and the closing bracket belongs on it, not under it.
    void SetIndent(int indent) {
        if (Width(line) == 0 || RTrim(line).empty()) {
            if (probe) return;
            line.assign(static_cast<size_t>(indent), ' ');
            return;
        }
        NewLine(indent);
    }

    void NewLine(int indent) {
        if (probe) {
            broke = true;
            return;
        }
        PushLine();
        line.assign(static_cast<size_t>(indent), ' ');
    }
    int Col() const { return Width(line); }
    bool Fits(const std::string &s) const { return Col() + Width(s) <= opt.width; }

    // Renders `fn` into a fresh probe printer; false if it could not be
    // done on a single line.
    template <class F>
    bool ProbeInto(F &&fn, std::string &result) const {
        Printer pr;
        pr.opt = opt;
        pr.probe = true;
        fn(pr);
        if (pr.broke) return false;
        result = pr.line;
        return true;
    }

    bool Flat(const Node *n, std::string &result) const {
        return ProbeInto([n](Printer &pr) { pr.RenderKind(n, 0); }, result);
    }

    void EmitLeading(const Node *n, int indent) {
        for (size_t i = 0; i < n->leading.size(); i++) {
            if (Width(line) > indent) NewLine(indent);
            // The current line is blank here, so pushing it *is* the
            // blank line the author had between the two comments.
            if (i > 0 && n->leading[i].blanks > 0) NewLine(indent);
            line += NormComment(n->leading[i].text);
            NewLine(indent);
        }
        if (!n->leading.empty() && n->blanks > 0) NewLine(indent);
    }

    void RenderNode(const Node *n, int indent) {
        if (!n->leading.empty()) {
            if (probe) {
                broke = true;
                return;
            }
            EmitLeading(n, indent);
        }
        RenderKind(n, indent);
    }

    void RenderKind(const Node *n, int indent) {
        if (broke) return;
        switch (n->kind) {
            case NK::Leaf: line += NormLeaf(n->text); return;
            case NK::Paren: RenderParen(n, indent); return;
            case NK::Block: RenderBlock(n, indent); return;
            case NK::Unary: RenderUnary(n, indent); return;
            case NK::Binary: RenderBinary(n, indent); return;
            case NK::Call: RenderCall(n, indent); return;
            case NK::Index: RenderIndex(n, indent); return;
            case NK::Func: RenderFunc(n, indent); return;
            case NK::If: RenderIf(n, indent); return;
            case NK::For: RenderFor(n, indent); return;
            case NK::While: RenderWhile(n, indent); return;
            case NK::Repeat: RenderRepeat(n, indent); return;
        }
    }

    void RenderUnary(const Node *n, int indent) {
        line += n->text;
        RenderNode(n->kids[0].get(), indent);
    }

    void RenderParen(const Node *n, int indent) {
        if (!probe) {
            std::string f;
            if (Flat(n, f) && Fits(f)) {
                line += f;
                return;
            }
        }
        if (!n->tail.empty() && probe) {
            broke = true;
            return;
        }
        line += "(";
        RenderNode(n->kids[0].get(), indent);
        if (!n->tail.empty()) {
            for (const Cmt &c : n->tail) {
                NewLine(indent + opt.indent);
                line += NormComment(c.text);
            }
            NewLine(indent);
        }
        line += ")";
    }

    // Statement list of a `{}` body or of the whole file. `indent` is the
    // statements' own indentation.
    void RenderStatements(const std::vector<P> &stmts, const std::vector<Cmt> &tail, int indent) {
        for (size_t i = 0; i < stmts.size(); i++) {
            const Node *s = stmts[i].get();
            // Nothing to flush at the very top of the file, where `line`
            // is still empty -- only inside a block, where it holds the
            // `{` this body belongs to.
            if (!line.empty()) PushLine();
            // A blank line the author put between two statements is kept
            // (just the one, however many there were); one before the
            // first statement of a block is dropped.
            int pre = s->leading.empty() ? s->stmt_blanks : s->leading.front().blanks;
            if (i > 0 && pre > 0) out += '\n';
            line.assign(static_cast<size_t>(indent), ' ');
            RenderNode(s, indent);
            if (!s->trailing.empty()) line += " " + NormComment(s->trailing);
        }
        for (size_t i = 0; i < tail.size(); i++) {
            if (!line.empty()) PushLine();
            if ((!stmts.empty() || i > 0) && tail[i].blanks > 0) out += '\n';
            line.assign(static_cast<size_t>(indent), ' ');
            line += NormComment(tail[i].text);
        }
    }

    void RenderBlock(const Node *n, int indent) {
        // An empty block is the one that stays on its line: `function() {}`
        // reads better than a `{` with nothing but a `}` under it.
        if (n->kids.empty() && n->tail.empty()) {
            line += "{}";
            return;
        }
        // Otherwise a block is never written on one line: `{ x }` becomes
        // a real indented body, which is what every R style guide asks
        // for and what makes the `} else {` shape below work.
        if (probe) {
            broke = true;
            return;
        }
        line += "{";
        RenderStatements(n->kids, n->tail, indent + opt.indent);
        NewLine(indent);
        line += "}";
    }

    // --- argument lists (calls, indexing, function parameters) --------

    void RenderArgFlat(const Arg &a) {
        if (!a.leading.empty() || !a.trailing.empty()) {
            broke = true;
            return;
        }
        if (a.name) {
            line += NormLeaf(a.name->text);
            line += " = ";
        }
        if (a.value) RenderNode(a.value.get(), 0);
    }

    bool FlatArgList(const std::vector<Arg> &args, const std::vector<Cmt> &tail, const char *open,
                     const char *close, std::string &result) const {
        return ProbeInto(
            [&](Printer &pr) {
                if (!tail.empty()) {
                    pr.broke = true;
                    return;
                }
                pr.line += open;
                for (size_t i = 0; i < args.size(); i++) {
                    if (i > 0) pr.line += ", ";
                    pr.RenderArgFlat(args[i]);
                    if (pr.broke) return;
                }
                pr.line += close;
            },
            result);
    }

    void RenderArgList(const std::vector<Arg> &args, const std::vector<Cmt> &tail, int indent,
                       const char *open, const char *close) {
        if (probe) {
            if (!tail.empty()) {
                broke = true;
                return;
            }
            line += open;
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) line += ", ";
                RenderArgFlat(args[i]);
                if (broke) return;
            }
            line += close;
            return;
        }
        // One argument per line, closing delimiter back at the caller's
        // own indentation. No trailing comma: R does not allow one.
        line += open;
        int inner = indent + opt.indent;
        for (size_t i = 0; i < args.size(); i++) {
            NewLine(inner);
            const Arg &a = args[i];
            for (const Cmt &c : a.leading) {
                line += NormComment(c.text);
                NewLine(inner);
            }
            if (a.name) {
                line += NormLeaf(a.name->text);
                line += " = ";
            }
            if (a.value) RenderNode(a.value.get(), inner);
            if (i + 1 < args.size()) line += ",";
            if (!a.trailing.empty()) line += " " + NormComment(a.trailing);
        }
        for (const Cmt &c : tail) {
            NewLine(inner);
            line += NormComment(c.text);
        }
        SetIndent(indent);
        line += close;
    }

    // --- calls, indexing, definitions --------------------------------

    // True for a node whose own layout can absorb the overflow, so an
    // assignment keeps it on the `x <- ` line and lets it break inside
    // itself rather than pushing the whole value down a line.
    static bool Breakable(const Node *n) {
        if (n->kind == NK::Leaf) return false;
        if (n->kind == NK::Unary) return Breakable(n->kids[0].get());
        return true;
    }

    // The opening line of a value that can be "hugged": a brace block, or
    // a function definition with one. Returns false for anything else.
    bool HugHead(const Node *v, std::string &s) const {
        if (!v->leading.empty()) return false;
        if (v->kind == NK::Block) {
            s = "{";
            return true;
        }
        if (v->kind == NK::Func && !v->kids.empty() && v->kids[0]->kind == NK::Block) {
            std::string head;
            if (!FlatArgList(v->args, v->tail, v->text == "\\" ? "\\(" : "function(", ")", head))
                return false;
            s = head + " {";
            return true;
        }
        return false;
    }

    // `test_that("name", {` / `map(xs, function(x) {`: when the last
    // argument is a block (or a function with one) it stays on the call's
    // own line and only its body is indented, instead of the whole
    // argument list exploding one-per-line.
    bool TryHug(const Node *n, int indent) {
        if (n->args.empty() || !n->tail.empty()) return false;
        const Arg &last = n->args.back();
        if (!last.value || !last.leading.empty() || !last.trailing.empty()) return false;
        std::string hug;
        if (!HugHead(last.value.get(), hug)) return false;
        std::string head;
        bool ok = ProbeInto(
            [&](Printer &pr) {
                pr.RenderKind(n->kids[0].get(), 0);
                pr.line += "(";
                for (size_t i = 0; i + 1 < n->args.size(); i++) {
                    pr.RenderArgFlat(n->args[i]);
                    if (pr.broke) return;
                    pr.line += ", ";
                }
                if (last.name) {
                    pr.line += NormLeaf(last.name->text);
                    pr.line += " = ";
                }
            },
            head);
        if (!ok) return false;
        if (Col() + Width(head) + Width(hug) > opt.width) return false;
        line += head;
        RenderNode(last.value.get(), indent);
        line += ")";
        return true;
    }

    void RenderCall(const Node *n, int indent) {
        if (!probe) {
            std::string f;
            if (Flat(n, f) && Fits(f)) {
                line += f;
                return;
            }
            if (TryHug(n, indent)) return;
        }
        RenderNode(n->kids[0].get(), indent);
        RenderArgList(n->args, n->tail, indent, "(", ")");
    }

    void RenderIndex(const Node *n, int indent) {
        if (!probe) {
            std::string f;
            if (Flat(n, f) && Fits(f)) {
                line += f;
                return;
            }
        }
        RenderNode(n->kids[0].get(), indent);
        RenderArgList(n->args, n->tail, indent, n->bracket2 ? "[[" : "[", n->bracket2 ? "]]" : "]");
    }

    void RenderFunc(const Node *n, int indent) {
        const char *open = n->text == "\\" ? "\\(" : "function(";
        if (probe) {
            RenderArgList(n->args, n->tail, 0, open, ")");
            if (broke) return;
            line += " ";
            RenderNode(n->kids[0].get(), 0);
            return;
        }
        std::string f;
        if (Flat(n, f) && Fits(f)) {
            line += f;
            return;
        }
        // A block body is the usual reason the flat form did not fit, and
        // the parameter list itself is generally still fine on one line.
        std::string head;
        if (FlatArgList(n->args, n->tail, open, ")", head) && Fits(head)) {
            line += head;
            line += " ";
            RenderNode(n->kids[0].get(), indent);
            return;
        }
        RenderArgList(n->args, n->tail, indent, open, ")");
        line += " ";
        RenderNode(n->kids[0].get(), indent);
    }

    // --- control flow -------------------------------------------------

    // `if (`/`while (` plus a condition plus `) `, broken inside the
    // parentheses when the condition is too wide to sit on the line.
    // `extra` is what will follow on the same line (1 for a `{`).
    void RenderHeader(const char *open, const Node *cond, int indent, int extra) {
        std::string c;
        if (Flat(cond, c) && Col() + Width(open) + Width(c) + 2 + extra <= opt.width) {
            line += open;
            line += c;
            line += ") ";
            return;
        }
        line += open;
        NewLine(indent + opt.indent);
        RenderNode(cond, indent + opt.indent);
        NewLine(indent);
        line += ") ";
    }

    void RenderIf(const Node *n, int indent) {
        if (probe) {
            line += "if (";
            RenderNode(n->kids[0].get(), 0);
            line += ") ";
            RenderNode(n->kids[1].get(), 0);
            if (n->kids.size() > 2) {
                line += " else ";
                RenderNode(n->kids[2].get(), 0);
            }
            return;
        }
        std::string f;
        if (Flat(n, f) && Fits(f)) {
            line += f;
            return;
        }
        RenderHeader("if (", n->kids[0].get(), indent, n->kids[1]->kind == NK::Block ? 1 : 0);
        RenderNode(n->kids[1].get(), indent);
        if (n->kids.size() > 2) {
            line += " else ";
            RenderNode(n->kids[2].get(), indent);
        }
    }

    void RenderFor(const Node *n, int indent) {
        if (probe) {
            line += "for (";
            line += n->kids[0]->text;
            line += " in ";
            RenderNode(n->kids[1].get(), 0);
            line += ") ";
            RenderNode(n->kids[2].get(), 0);
            return;
        }
        std::string f;
        if (Flat(n, f) && Fits(f)) {
            line += f;
            return;
        }
        std::string seq;
        std::string head = "for (" + n->kids[0]->text + " in ";
        int extra = n->kids[2]->kind == NK::Block ? 1 : 0;
        if (Flat(n->kids[1].get(), seq) && Col() + Width(head) + Width(seq) + 2 + extra <= opt.width) {
            line += head + seq + ") ";
        } else {
            line += head;
            NewLine(indent + opt.indent);
            RenderNode(n->kids[1].get(), indent + opt.indent);
            NewLine(indent);
            line += ") ";
        }
        RenderNode(n->kids[2].get(), indent);
    }

    void RenderWhile(const Node *n, int indent) {
        if (probe) {
            line += "while (";
            RenderNode(n->kids[0].get(), 0);
            line += ") ";
            RenderNode(n->kids[1].get(), 0);
            return;
        }
        std::string f;
        if (Flat(n, f) && Fits(f)) {
            line += f;
            return;
        }
        RenderHeader("while (", n->kids[0].get(), indent, n->kids[1]->kind == NK::Block ? 1 : 0);
        RenderNode(n->kids[1].get(), indent);
    }

    void RenderRepeat(const Node *n, int indent) {
        line += "repeat ";
        RenderNode(n->kids[0].get(), probe ? 0 : indent);
    }

    // --- operators ----------------------------------------------------

    void RenderBinary(const Node *n, int indent) {
        const std::string &op = n->text;
        if (probe) {
            if (!n->op_trailing.empty()) {
                broke = true;
                return;
            }
            RenderNode(n->kids[0].get(), 0);
            if (broke) return;
            if (TightOp(op)) {
                line += op;
            } else {
                line += " ";
                line += op;
                line += " ";
            }
            RenderNode(n->kids[1].get(), 0);
            return;
        }
        std::string f;
        if (Flat(n, f) && Fits(f)) {
            line += f;
            return;
        }
        // Assignment: the value keeps the `x <- ` line and breaks inside
        // itself (`x <- f(`...), which is what a call, a block or a
        // pipeline wants. Only a value with nowhere to break goes below.
        if (op == "<-" || op == "<<-" || op == "=") {
            RenderNode(n->kids[0].get(), indent);
            line += " " + op;
            const Node *rhs = n->kids[1].get();
            if (!n->op_trailing.empty()) {
                line += " " + NormComment(n->op_trailing);
                NewLine(indent + opt.indent);
                RenderNode(rhs, indent + opt.indent);
                return;
            }
            line += " ";
            if (Breakable(rhs)) {
                RenderNode(rhs, indent);
            } else {
                NewLine(indent + opt.indent);
                RenderNode(rhs, indent + opt.indent);
            }
            return;
        }
        // Everything else breaks as a chain: the whole run of same-
        // precedence operators is flattened so a pipeline or a ggplot `+`
        // stack lines its steps up at one indentation instead of nesting
        // them one level deeper each time.
        int prec = BinPrec(op);
        std::vector<std::string> ops;
        std::vector<std::string> op_comments;
        std::vector<const Node *> rights;
        const Node *cur = n;
        const Node *first = nullptr;
        for (;;) {
            ops.insert(ops.begin(), cur->text);
            op_comments.insert(op_comments.begin(), cur->op_trailing);
            rights.insert(rights.begin(), cur->kids[1].get());
            const Node *l = cur->kids[0].get();
            if (l->kind == NK::Binary && BinPrec(l->text) == prec && !RightAssoc(l->text) &&
                l->leading.empty()) {
                cur = l;
                continue;
            }
            first = l;
            break;
        }
        RenderNode(first, indent);
        for (size_t i = 0; i < ops.size(); i++) {
            if (!TightOp(ops[i])) line += " ";
            line += ops[i];
            if (!op_comments[i].empty()) line += " " + NormComment(op_comments[i]);
            NewLine(indent + opt.indent);
            RenderNode(rights[i], indent + opt.indent);
        }
    }
};

// ---------------------------------------------------------------------
// Self-check: the formatted text must carry the same tokens as the input
// ---------------------------------------------------------------------

// A string literal's *value*, so the single- to double-quote rewrite
// above compares equal. Raw strings are passed through untouched (the
// formatter never rewrites them either).
std::string StringValue(const std::string &s) {
    if (IsRawStringToken(s) || s.size() < 2) return s;
    std::string body = s.substr(1, s.size() - 2);
    std::string v;
    for (size_t i = 0; i < body.size(); i++) {
        if (body[i] == '\\' && i + 1 < body.size()) {
            char c = body[i + 1];
            if (c == '\'' || c == '"') {
                v += c;
            } else {
                v += '\\';
                v += c;
            }
            i++;
            continue;
        }
        v += body[i];
    }
    return v;
}

// Every token that carries meaning, in a form the formatter's own
// deliberate rewrites normalize away: quoting style, `**` for `^`,
// statement `=` for `<-`, and the space after a `#`. Semicolons are
// dropped because the formatter turns them into line breaks. Anything
// else differing between input and output is a bug in this file, and
// Format() then returns the input untouched rather than the output.
//
// (The `=` -> `<-` mapping is deliberately blind to context, which does
// cost a little checking power -- it would not catch a named argument
// wrongly turned into an assignment -- but the rewrite is real and has
// to be allowed for somewhere.)
std::vector<std::string> Signature(const std::string &text) {
    Lexer lx(text);
    std::vector<Tok> toks = lx.Run();
    std::vector<std::string> sig;
    sig.reserve(toks.size());
    for (const Tok &t : toks) {
        switch (t.kind) {
            case T::Eof:
            case T::Semi:
                break;
            case T::Comment:
                sig.push_back("C" + NormComment(t.text));
                break;
            case T::Str:
                sig.push_back("S" + StringValue(t.text));
                break;
            case T::Op: {
                std::string o = t.text == "**" ? "^" : t.text;
                if (o == "=") o = "<-";
                sig.push_back("O" + o);
                break;
            }
            default:
                sig.push_back("T" + t.text);
                break;
        }
    }
    return sig;
}

}  // namespace

Result Format(const std::string &src, const Options &opts) {
    Result r;
    r.text = src;
    Lexer lx(src);
    std::vector<Tok> toks = lx.Run();
    if (!lx.error.empty()) {
        r.error = lx.error;
        r.error_line = lx.error_line;
        return r;
    }
    Parser ps;
    ps.toks = std::move(toks);
    std::vector<Cmt> tail;
    std::vector<P> stmts = ps.ParseStatements(T::Eof, tail);
    if (ps.Failed()) {
        r.error = ps.error;
        r.error_line = ps.error_line;
        return r;
    }
    if (ps.Cur().kind != T::Eof) {
        r.error = "unexpected '" + ps.Cur().text + "'";
        r.error_line = ps.Cur().line;
        return r;
    }
    Printer pr;
    pr.opt = opts;
    if (pr.opt.width < 20) pr.opt.width = 20;
    if (pr.opt.indent < 1) pr.opt.indent = 1;
    pr.RenderStatements(stmts, tail, 0);
    if (!pr.line.empty()) pr.PushLine();
    std::string text = pr.out;
    // Exactly one trailing newline, and none of the blank lines a final
    // comment or statement may have left behind.
    while (!text.empty() && (text.back() == '\n' || IsSpace(text.back()))) text.pop_back();
    if (!text.empty()) text += '\n';

    if (Signature(src) != Signature(text)) {
        r.error = "internal check failed (formatting would have changed the code)";
        r.error_line = 0;
        return r;
    }
    r.ok = true;
    r.text = text;
    return r;
}

}  // namespace rfmt
