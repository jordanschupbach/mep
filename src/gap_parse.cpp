// GAP reader: see gap_parse.h. Written from GAP's documentation (the
// reference manual's chapter 4, "The Programming Language") and from GAP
// 4.15's observable behaviour -- which input it accepts, and the message,
// line and column of its first syntax error -- measured with
// tools/gap_parse_check.py. No GAP source is used.
#include "gap_parse.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gapparse {

namespace {

struct KeywordEntry {
    const char *spelling;
    T kind;
};

const KeywordEntry kKeywords[] = {
    {"and", T::And},         {"atomic", T::Atomic},     {"break", T::Break},       {"continue", T::Continue},
    {"do", T::Do},           {"elif", T::Elif},         {"else", T::Else},         {"end", T::End},
    {"false", T::False},     {"fi", T::Fi},             {"for", T::For},           {"function", T::Function},
    {"if", T::If},           {"in", T::In},             {"local", T::Local},       {"mod", T::Mod},
    {"not", T::Not},         {"od", T::Od},             {"or", T::Or},             {"readonly", T::Readonly},
    {"readwrite", T::Readwrite}, {"rec", T::Rec},       {"repeat", T::Repeat},     {"return", T::Return},
    {"then", T::Then},       {"true", T::True},         {"until", T::Until},       {"while", T::While},
    {"quit", T::Quit},       {"QUIT", T::QQuit},        {"IsBound", T::IsBound},   {"Unbind", T::Unbind},
    {"TryNextMethod", T::TryNextMethod}, {"Info", T::Info}, {"Assert", T::Assert},
};

bool IsAlpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsDigit(int c) { return c >= '0' && c <= '9'; }
bool IsOctal(int c) { return c >= '0' && c <= '7'; }
bool IsHex(int c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
// A character that continues an identifier (a backslash starts an escape).
bool IsIdentChar(int c) { return IsAlpha(c) || IsDigit(c) || c == '_' || c == '@' || c == '\\'; }

// ---- Scanner ------------------------------------------------------------------------

class Scanner {
public:
    explicit Scanner(std::string_view text) : s_(text) {
        line_starts_.push_back(0);
        for (size_t i = 0; i < s_.size(); i++)
            if (s_[i] == '\n') line_starts_.push_back(static_cast<uint32_t>(i + 1));
    }

    int LineOf(size_t off) const {
        auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), static_cast<uint32_t>(off));
        return static_cast<int>(it - line_starts_.begin()) - 1;
    }
    int ColOf(size_t off) const {
        return static_cast<int>(off - line_starts_[static_cast<size_t>(LineOf(off))]);
    }

    // The next token from `pos`. `after_dot`: the previous token was `.` or `!.`, so a run of digits is a
    // record component (an integer, or an identifier when letters follow), never a float.
    Token Next(bool after_dot, std::vector<Comment> *comments) {
        pos_ = SkipCont(pos_);
        bool newline = false;
        for (;;) {
            const int c = Cur();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f') {
                Adv();
            } else if (c == '\n') {
                newline = true;
                Adv();
            } else if (c == '#') {
                // A comment runs to the end of its line; a backslash before the newline does not continue it.
                Comment cm;
                cm.begin = static_cast<uint32_t>(pos_);
                cm.pragma = pos_ + 1 < s_.size() && s_[pos_ + 1] == '%';
                size_t e = pos_;
                while (e < s_.size() && s_[e] != '\n') e++;
                cm.end = static_cast<uint32_t>(e);
                cm.line = LineOf(cm.begin);
                cm.col = ColOf(cm.begin);
                if (comments) comments->push_back(cm);
                pos_ = SkipCont(e);
            } else {
                break;
            }
        }
        Token t;
        t.newline_before = newline;
        t.begin = static_cast<uint32_t>(pos_);
        t.line = LineOf(pos_);
        t.col = ColOf(pos_);
        const int c = Cur();
        if (c < 0) {
            t.kind = T::Eof;
        } else if (IsDigit(c)) {
            ScanNumber(&t, after_dot);
        } else if (IsAlpha(c) || c == '_' || c == '@' || c == '\\') {
            ScanIdent(&t);
        } else if (c == '"') {
            ScanString(&t);
        } else if (c == '\'') {
            ScanChar(&t);
        } else {
            ScanPunct(&t);
        }
        t.end = static_cast<uint32_t>(pos_);
        return t;
    }

    // Re-reads the token at @p begin (a `.` where an expression starts) as a float: `.5`.
    void RescanFloat(Token *t) {
        pos_ = t->begin;
        t->error.clear();
        t->kind = T::Float;
        Adv();  // the '.'
        bool digits = false;
        while (IsDigit(Cur())) {
            digits = true;
            Adv();
        }
        if (!digits) {
            SetError(t, "Badly formed number: need a digit before or after the decimal point");
        } else {
            FloatTail(t);
        }
        t->end = static_cast<uint32_t>(pos_);
    }

    size_t Pos() const { return pos_; }

private:
    std::string_view s_;
    size_t pos_ = 0;
    std::vector<uint32_t> line_starts_;

    // A backslash-newline (or backslash-CR-LF) is deleted wherever GAP reads characters, except inside a comment.
    size_t SkipCont(size_t p) const {
        while (p < s_.size() && s_[p] == '\\') {
            if (p + 1 < s_.size() && s_[p + 1] == '\n') {
                p += 2;
            } else if (p + 2 < s_.size() && s_[p + 1] == '\r' && s_[p + 2] == '\n') {
                p += 3;
            } else {
                break;
            }
        }
        return p;
    }
    int Cur() const { return pos_ < s_.size() ? static_cast<unsigned char>(s_[pos_]) : -1; }
    int Peek(int k) const {
        size_t p = pos_;
        for (int i = 0; i < k && p < s_.size(); i++) p = SkipCont(p + 1);
        return p < s_.size() ? static_cast<unsigned char>(s_[p]) : -1;
    }
    void Adv() {
        if (pos_ < s_.size()) pos_ = SkipCont(pos_ + 1);
    }
    void SetError(Token *t, const char *msg) {
        if (!t->error.empty()) return;
        t->error = msg;
        // GAP reports a scanner error at the line it has reached.
        t->line = LineOf(std::min(pos_, s_.size()));
        t->col = t->line == LineOf(t->begin) ? ColOf(t->begin) : -1;
    }

    // Identifier characters (with backslash escapes) from pos_; appends the name.
    void IdentRun(std::string *name, bool *escaped) {
        for (;;) {
            const int c = Cur();
            if (c == '\\') {
                Adv();
                const int e = Cur();
                if (e < 0) break;
                name->push_back(static_cast<char>(e));
                *escaped = true;
                Adv();
            } else if (c >= 0 && IsIdentChar(c)) {
                name->push_back(static_cast<char>(c));
                Adv();
            } else {
                break;
            }
        }
    }

    void FinishIdent(Token *t, bool escaped) {
        t->kind = T::Ident;
        if (!escaped) {
            for (const KeywordEntry &k : kKeywords) {
                if (t->name == k.spelling) {
                    t->kind = k.kind;
                    break;
                }
            }
        }
        if (t->name.size() > 1023) SetError(t, "Identifiers in GAP must consist of at most 1023 characters.");
    }

    void ScanIdent(Token *t) {
        bool escaped = false;
        IdentRun(&t->name, &escaped);
        FinishIdent(t, escaped);
    }

    // After a float's digits: an exponent, one marker letter, then `_` and an optional letter; a further
    // identifier character makes it a badly formed number.
    void FloatTail(Token *t) {
        int c = Cur();
        if (c == 'e' || c == 'E' || c == 'd' || c == 'D' || c == 'q' || c == 'Q') {
            Adv();
            if (Cur() == '+' || Cur() == '-') Adv();
            if (!IsDigit(Cur())) {
                SetError(t, "Badly formed number: need at least one digit in the exponent");
                return;
            }
            while (IsDigit(Cur())) Adv();
        }
        if (IsAlpha(Cur())) Adv();
        if (Cur() == '_') {
            Adv();
            if (IsAlpha(Cur())) Adv();
        }
        c = Cur();
        if (c >= 0 && IsIdentChar(c)) SetError(t, "Badly formed number");
    }

    void ScanNumber(Token *t, bool after_dot) {
        std::string digits;
        while (IsDigit(Cur())) {
            digits.push_back(static_cast<char>(Cur()));
            Adv();
        }
        const int c = Cur();
        if (c >= 0 && IsIdentChar(c)) {
            // Digits then letters: an identifier (`2x`, `1e5`).
            t->name = digits;
            bool escaped = false;
            IdentRun(&t->name, &escaped);
            FinishIdent(t, escaped);
            return;
        }
        if (!after_dot && c == '.' && Peek(1) != '.') {
            t->kind = T::Float;
            Adv();
            while (IsDigit(Cur())) Adv();
            FloatTail(t);
            return;
        }
        t->kind = T::Int;
    }

    // One character of a string or character literal after a backslash (pos_ at the backslash).
    void Escape(Token *t) {
        Adv();
        const int e = Cur();
        if (e < 0) return;
        if (e == '0' && Peek(1) == 'x') {
            Adv();
            Adv();
            for (int i = 0; i < 2; i++) {
                if (!IsHex(Cur())) {
                    SetError(t, "Expecting hexadecimal digit");
                    return;
                }
                Adv();
            }
            return;
        }
        if (IsOctal(e)) {
            Adv();
            if (!IsOctal(Cur())) {
                SetError(t, e == '0' ? "Expecting hexadecimal escape, or two more octal digits"
                                     : "Expecting octal digit");
                return;
            }
            Adv();
            if (!IsOctal(Cur())) {
                SetError(t, "Expecting octal digit");
                return;
            }
            Adv();
            return;
        }
        Adv();
    }

    void ScanString(Token *t) {
        t->kind = T::String;
        if (Peek(1) == '"' && Peek(2) == '"') {
            Adv();
            Adv();
            Adv();
            for (;;) {
                if (Cur() < 0) {
                    SetError(t, "String must end with \"\"\" before end of file");
                    t->col = -1;
                    return;
                }
                if (Cur() == '"' && Peek(1) == '"' && Peek(2) == '"') {
                    Adv();
                    Adv();
                    Adv();
                    return;
                }
                Adv();
            }
        }
        Adv();
        for (;;) {
            const int c = Cur();
            if (c < 0) {
                SetError(t, "String must end with \" before end of file");
                return;
            }
            if (c == '"') {
                Adv();
                return;
            }
            if (c == '\n') {
                SetError(t, "String must not include <newline>");
                return;
            }
            if (c == '\\') {
                Escape(t);
                if (!t->error.empty()) return;
                continue;
            }
            Adv();
        }
    }

    void ScanChar(Token *t) {
        t->kind = T::Char;
        Adv();
        const int c = Cur();
        if (c == '\n') {
            SetError(t, "Character literal must not include <newline>");
            return;
        }
        if (c == '\\') {
            Escape(t);
            if (!t->error.empty()) return;
        } else if (c >= 0) {
            Adv();
        }
        if (Cur() != '\'') {
            SetError(t, "Missing single quote in character constant");
            return;
        }
        Adv();
    }

    void ScanPunct(Token *t) {
        const int c = Cur();
        const int n = Peek(1);
        auto one = [&](T k) {
            t->kind = k;
            Adv();
        };
        auto two = [&](T k) {
            t->kind = k;
            Adv();
            Adv();
        };
        switch (c) {
        case ':': n == '=' ? two(T::Assign) : one(T::Colon); break;
        case '<': n == '=' ? two(T::Le) : n == '>' ? two(T::Ne) : one(T::Lt); break;
        case '>': n == '=' ? two(T::Ge) : one(T::Gt); break;
        case '=': one(T::Eq); break;
        case '+': one(T::Plus); break;
        case '-': n == '>' ? two(T::Arrow) : one(T::Minus); break;
        case '*': one(T::Star); break;
        case '/': one(T::Slash); break;
        case '^': one(T::Caret); break;
        case '~': one(T::Tilde); break;
        case ',': one(T::Comma); break;
        case '(': one(T::LParen); break;
        case ')': one(T::RParen); break;
        case '[': one(T::LBracket); break;
        case ']': one(T::RBracket); break;
        case '{': one(T::LBrace); break;
        case '}': one(T::RBrace); break;
        case ';': n == ';' ? two(T::DualSemi) : one(T::Semi); break;
        case '!':
            if (n == '.') {
                two(T::BangDot);
            } else if (n == '[') {
                two(T::BangBracket);
            } else {
                one(T::Illegal);
            }
            break;
        case '.':
            if (n == '.') {
                if (Peek(2) == '.') {
                    t->kind = T::DotDotDot;
                    Adv();
                    Adv();
                    Adv();
                } else {
                    two(T::DotDot);
                }
            } else {
                one(T::Dot);
            }
            break;
        case '?':
            // `?topic`: the rest of the line is the help request.
            t->kind = T::Help;
            while (pos_ < s_.size() && s_[pos_] != '\n') pos_++;
            break;
        default: one(T::Illegal); break;
        }
    }
};

// ---- Parser -------------------------------------------------------------------------

struct Abort {};

// Token kinds GAP lets start a statement (its STATBEGIN set: some of these are then refused with
// "statement expected").
bool InStatBegin(T k) {
    switch (k) {
    case T::Ident: case T::Unbind: case T::IsBound: case T::TryNextMethod: case T::Info: case T::Assert:
    case T::True: case T::False: case T::Tilde: case T::Char: case T::String:
    case T::If: case T::For: case T::While: case T::Repeat: case T::Break: case T::Continue: case T::Return:
    case T::Atomic: case T::Quit: case T::QQuit: case T::Help: case T::Semi: case T::DualSemi:
        return true;
    default:
        return false;
    }
}

bool IsRelOp(T k) {
    return k == T::Eq || k == T::Ne || k == T::Lt || k == T::Le || k == T::Gt || k == T::Ge || k == T::In;
}

class Parser {
public:
    Parser(Parse *out, const Options &opt) : p_(out), opt_(opt), sc_(out->text) {}

    void Run() {
        NewNode(K::File);
        Advance();
        if (opt_.as_function) {
            RunAsFunction();
        } else {
            RunTopLevel();
        }
        Node &file = p_->nodes[0];
        file.first = 0;
        file.last = static_cast<int>(p_->tokens.size()) - 1;
        // Scan whatever is left (after an abandoned statement) so the token list covers the file.
        while (Cur().kind != T::Eof) {
            try {
                Advance();
            } catch (Abort &) {
            }
        }
    }

private:
    Parse *p_;
    Options opt_;
    Scanner sc_;
    int cur_ = -1;        // index of the current (lookahead) token
    int tilde_ok_ = 0;    // inside a list or record literal: `~` allowed
    int loops_ = 0;       // enclosing loops in the current function
    int depth_ = 0;       // recursion depth
    bool tilde_seen_ = false;

    const Token &Cur() const { return p_->tokens[static_cast<size_t>(cur_)]; }
    T Kind() const { return Cur().kind; }
    Node &Nd(int i) { return p_->nodes[static_cast<size_t>(i)]; }

    void Advance() {
        if (cur_ >= 0 && Cur().kind == T::Eof) return;
        const bool after_dot = cur_ >= 0 && (Cur().kind == T::Dot || Cur().kind == T::BangDot);
        p_->tokens.push_back(sc_.Next(after_dot, &p_->comments));
        cur_ = static_cast<int>(p_->tokens.size()) - 1;
        if (!Cur().error.empty()) Fail(Cur().error, true);
    }

    // The current token is a `.` where an atom starts: it begins a float.
    void RescanFloatHere() {
        Token &t = p_->tokens[static_cast<size_t>(cur_)];
        // Comments after the dot were scanned with it; drop them (they are rescanned).
        while (!p_->comments.empty() && p_->comments.back().begin > t.begin) p_->comments.pop_back();
        sc_.RescanFloat(&t);
        if (!t.error.empty()) Fail(t.error, true);
    }

    [[noreturn]] void Fail(const std::string &msg, bool scanner = false) {
        const Token &t = Cur();
        Error e;
        e.message = msg;
        e.offset = t.begin;
        e.at_eof = t.kind == T::Eof;
        if (scanner) {
            e.line = t.line;
            e.col = t.col;
        } else {
            // GAP reports the line its scanner has reached: the end of the current token.
            const size_t end = t.kind == T::Eof ? p_->text.size() : t.end;
            e.line = sc_.LineOf(end);
            e.col = (t.kind == T::Eof || sc_.LineOf(t.begin) != e.line) ? -1 : t.col;
        }
        p_->errors.push_back(e);
        throw Abort{};
    }

    void Match(T kind, const char *what) {
        if (Kind() != kind && !(kind == T::Semi && Kind() == T::DualSemi)) Fail(std::string(what) + " expected");
        Advance();
    }

    int NewNode(K kind, int tok = -1) {
        Node n;
        n.kind = kind;
        n.tok = tok;
        n.first = tok >= 0 ? tok : cur_;
        p_->nodes.push_back(n);
        return static_cast<int>(p_->nodes.size()) - 1;
    }
    // Closes node @p n: its last token is the one before the current one.
    int Close(int n) {
        Node &nd = Nd(n);
        nd.last = std::max(nd.first, cur_ - 1);
        return n;
    }
    void Add(int parent, int child) { Nd(parent).kids.push_back(child); }

    struct DepthGuard {
        Parser *p;
        explicit DepthGuard(Parser *pp) : p(pp) {
            if (++p->depth_ > kMaxNesting) p->Fail("nesting too deep");
        }
        ~DepthGuard() { p->depth_--; }
    };

    // ---- top level ----

    void RunAsFunction() {
        int file = 0;
        try {
            if (Kind() == T::Local) Add(file, ReadLocals({}));
            loops_ = 0;
            ReadStats(file);
            if (Kind() != T::Eof) Fail("<end-of-file> expected");
        } catch (Abort &) {
        }
    }

    void RunTopLevel() {
        bool first = true;
        while (Kind() != T::Eof) {
            const int start = cur_;
            const size_t nodes_before = p_->nodes.size();
            try {
                const int st = ReadCommand(first);
                if (st >= 0) Add(0, st);
            } catch (Abort &) {
                p_->nodes.resize(std::max(nodes_before, static_cast<size_t>(1)));
                Recover(start);
            }
            first = false;
        }
    }

    // After an error: skip to the `;` that ends the broken top-level command.
    void Recover(int start) {
        int depth = 0;
        for (int i = start; i < cur_; i++) depth += DepthDelta(p_->tokens[static_cast<size_t>(i)].kind);
        for (;;) {
            if (Kind() == T::Eof) return;
            const T k = Kind();
            depth += DepthDelta(k);
            if (depth < 0) depth = 0;
            try {
                Advance();
            } catch (Abort &) {
            }
            if ((k == T::Semi || k == T::DualSemi) && depth == 0) return;
        }
    }
    static int DepthDelta(T k) {
        switch (k) {
        case T::Function: case T::If: case T::For: case T::While: case T::Repeat: case T::LParen:
        case T::LBracket: case T::LBrace: case T::BangBracket:
            return 1;
        case T::End: case T::Fi: case T::Od: case T::Until: case T::RParen: case T::RBracket: case T::RBrace:
            return -1;
        case T::Atomic: return 0;  // `atomic function` opens with its `function`; an atomic block with `do`
        case T::Do: return 0;
        default: return 0;
        }
    }

    // One command of a file GAP reads: a statement, or an expression (whose first atom may turn out to be an
    // assignment's target), `quit;`, or a `?` help line.
    int ReadCommand(bool first) {
        DepthGuard g(this);
        switch (Kind()) {
        case T::Help: {
            const int n = NewNode(K::Help, cur_);
            Advance();
            return Close(n);
        }
        case T::Quit:
        case T::QQuit: {
            const int n = NewNode(K::Quit, cur_);
            Advance();
            Match(T::Semi, ";");
            return Close(n);
        }
        case T::Local:
            if (first) {
                const int n = ReadLocals({});
                return n;
            }
            break;
        case T::If: case T::For: case T::While: case T::Repeat: case T::Break: case T::Continue:
        case T::Return: case T::Unbind: case T::Info: case T::Assert: case T::TryNextMethod: case T::Semi:
            return ReadStatementTerminated();
        case T::Atomic:
            if (PeekIsFunction()) break;
            return ReadStatementTerminated();
        default:
            break;
        }
        // An expression, or an assignment / procedure call starting with an identifier.
        const int start = cur_;
        int st = -1;
        const int e = ReadExpr('x', &st);
        int n;
        if (st >= 0) {
            n = st;
        } else {
            n = NewNode(K::ExprStat, start);
            Add(n, e);
            Close(n);
            // Only a call written as a reference (`f(x)`, `r.f(x)`) is a procedure call statement; `(f)(x)` and
            // `function() ... end()` are expressions, which only a file's top level allows.
            if (Nd(e).kind == K::Call && p_->tokens[static_cast<size_t>(start)].kind == T::Ident) {
                Nd(n).kind = K::CallStat;
            } else {
                p_->top_exprs.push_back(n);
            }
        }
        Match(T::Semi, ";");
        Nd(n).last = cur_ - 1;
        return n;
    }

    bool PeekIsFunction() {
        // The scanner reads one token ahead only; look at the source after `atomic`.
        size_t p = Cur().end;
        const std::string_view s = p_->text;
        for (;;) {
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r' || s[p] == '\f')) p++;
            if (p < s.size() && s[p] == '#') {
                while (p < s.size() && s[p] != '\n') p++;
                continue;
            }
            break;
        }
        return s.compare(p, 8, "function") == 0 && (p + 8 >= s.size() || !IsIdentChar(static_cast<unsigned char>(s[p + 8])));
    }

    int ReadStatementTerminated() {
        const int n = ReadStatement();
        if (Nd(n).kind != K::Empty) {
            Match(T::Semi, ";");
            Nd(n).last = cur_ - 1;
        }
        return n;
    }

    // ---- statements ----

    // Statements while the current token may start one, each with its `;`, into @p parent.
    void ReadStats(int parent) {
        while (InStatBegin(Kind())) {
            Add(parent, ReadStatementTerminated());
        }
    }

    int ReadStatement() {
        DepthGuard g(this);
        switch (Kind()) {
        case T::Ident: return ReadCallVarAss('s');
        case T::Unbind: return ReadUnbind();
        case T::Info: return ReadInfo();
        case T::Assert: return ReadAssert();
        case T::TryNextMethod: return ReadTryNext();
        case T::If: return ReadIf();
        case T::For: return ReadFor();
        case T::While: return ReadWhile();
        case T::Repeat: return ReadRepeat();
        case T::Atomic:
            if (PeekIsFunction()) {
                // GAP takes `atomic function ... end;` here as a statement.
                const int n = NewNode(K::ExprStat, cur_);
                Add(n, ReadExpr('r'));
                return Close(n);
            }
            return ReadAtomic();
        case T::Break:
        case T::Continue: {
            if (loops_ == 0)
                Fail(Kind() == T::Break ? "'break' statement not enclosed in a loop"
                                        : "'continue' statement not enclosed in a loop");
            const int n = NewNode(Kind() == T::Break ? K::Break : K::Continue, cur_);
            Advance();
            return Close(n);
        }
        case T::Return: {
            const int n = NewNode(K::Return, cur_);
            Advance();
            if (Kind() != T::Semi && Kind() != T::DualSemi) Add(n, ReadExpr('r'));
            return Close(n);
        }
        case T::Semi: {
            const int n = NewNode(K::Empty, cur_);
            Advance();
            return Close(n);
        }
        case T::Quit: Fail("'quit;' cannot be used in this context");
        case T::QQuit: Fail("'QUIT;' cannot be used in this context");
        case T::Help: Fail("'?' cannot be used in this context");
        default: Fail("statement expected");
        }
    }

    int ReadIf() {
        const int n = NewNode(K::If, cur_);
        Advance();
        Add(n, ReadExpr('r'));
        Match(T::Then, "then");
        ReadStats(n);
        Nd(n).split = static_cast<int>(Nd(n).kids.size());
        while (Kind() == T::Elif) {
            const int e = NewNode(K::Elif, cur_);
            Advance();
            Add(e, ReadExpr('r'));
            Match(T::Then, "then");
            ReadStats(e);
            Add(n, Close(e));
        }
        if (Kind() == T::Else) {
            const int e = NewNode(K::Else, cur_);
            Advance();
            ReadStats(e);
            Add(n, Close(e));
        }
        Match(T::Fi, "while parsing an 'if' statement: statement or 'fi'");
        return Close(n);
    }

    int ReadFor() {
        const int n = NewNode(K::For, cur_);
        Advance();
        if (Kind() != T::Ident) Fail("Identifier expected");
        Add(n, Close(NewNode(K::Ident, cur_)));
        Advance();
        Match(T::In, "in");
        Add(n, ReadExpr('r'));
        Match(T::Do, "do");
        Nd(n).split = static_cast<int>(Nd(n).kids.size());
        loops_++;
        ReadStats(n);
        loops_--;
        Match(T::Od, "while parsing a 'for' loop: statement or 'od'");
        return Close(n);
    }

    int ReadWhile() {
        const int n = NewNode(K::While, cur_);
        Advance();
        Add(n, ReadExpr('r'));
        Match(T::Do, "do");
        loops_++;
        ReadStats(n);
        loops_--;
        Match(T::Od, "while parsing a 'while' loop: statement or 'od'");
        return Close(n);
    }

    int ReadRepeat() {
        const int n = NewNode(K::Repeat, cur_);
        Advance();
        loops_++;
        ReadStats(n);
        loops_--;
        Match(T::Until, "while parsing a 'repeat' loop: statement or 'until'");
        Add(n, ReadExpr('r'));
        return Close(n);
    }

    int ReadAtomic() {
        const int n = NewNode(K::Atomic, cur_);
        Advance();
        for (;;) {
            const int q = NewNode(K::QualExpr, cur_);
            if (Kind() == T::Readonly || Kind() == T::Readwrite) {
                Nd(q).tok = cur_;
                Advance();
            }
            Add(q, ReadExpr('r'));
            Add(n, Close(q));
            if (Kind() != T::Comma) break;
            Advance();
        }
        Match(T::Do, "do");
        Nd(n).split = static_cast<int>(Nd(n).kids.size());
        ReadStats(n);
        Match(T::Od, "while parsing an atomic block: statement or 'od'");
        return Close(n);
    }

    int ReadUnbind() {
        const int n = NewNode(K::Unbind, cur_);
        Advance();
        Match(T::LParen, "(");
        if (Kind() != T::Ident) Fail("Identifier expected");
        Add(n, ReadCallVarAss('u'));
        if (Kind() != T::RParen) Fail("'Unbind': argument should be followed by ')'");
        Advance();
        return Close(n);
    }

    int ReadInfo() {
        const int n = NewNode(K::Info, cur_);
        Advance();
        Match(T::LParen, "(");
        Add(n, ReadExpr('r'));
        Match(T::Comma, ",");
        Add(n, ReadExpr('r'));
        while (Kind() == T::Comma) {
            Advance();
            Add(n, ReadExpr('r'));
        }
        Match(T::RParen, ")");
        return Close(n);
    }

    int ReadAssert() {
        const int n = NewNode(K::Assert, cur_);
        Advance();
        Match(T::LParen, "(");
        Add(n, ReadExpr('r'));
        Match(T::Comma, ",");
        Add(n, ReadExpr('r'));
        if (Kind() == T::Comma) {
            Advance();
            Add(n, ReadExpr('r'));
        }
        Match(T::RParen, ")");
        return Close(n);
    }

    int ReadTryNext() {
        const int n = NewNode(K::TryNext, cur_);
        Advance();
        Match(T::LParen, "(");
        Match(T::RParen, ")");
        return Close(n);
    }

    // ---- references, selectors, assignments, calls ----

    // An identifier and its selectors. Modes: 's' a statement (assignment or procedure call), 'x' a top-level
    // command (an assignment, or the first atom of an expression), 'r' an expression, 'i' IsBound's operand,
    // 'u' Unbind's. *@p stmt receives an Assign node when one was read in mode 'x'.
    int ReadCallVarAss(char mode, int *stmt = nullptr) {
        DepthGuard g(this);
        const int id = Close(NewNode(K::Ident, cur_));
        Advance();
        if (Kind() == T::Arrow) {
            if (mode == 's' || mode == 'i' || mode == 'u') Fail("Function literal in impossible context");
            return ReadLambdaSingle(id);
        }
        bool was_call = false;
        const int ref = ReadSelectors(id, &was_call);
        if (mode == 's') {
            if (Kind() == T::Assign && !was_call) return ReadAssignment(ref);
            if (was_call) {
                const int n = NewNode(K::CallStat, Nd(ref).first);
                Add(n, ref);
                return Close(n);
            }
            Fail("found an expression when a statement was expected");
        }
        if (mode == 'x' && Kind() == T::Assign && !was_call) {
            const int a = ReadAssignment(ref);
            if (stmt) *stmt = a;
            return a;
        }
        if (mode == 'i' && was_call) Fail("Illegal operand for 'IsBound'");
        if (mode == 'u') {
            // Unbind checks its closing parenthesis first.
            if (Kind() != T::RParen) Fail("'Unbind': argument should be followed by ')'");
            if (was_call) Fail("Illegal operand for 'Unbind'");
        }
        return ref;
    }

    int ReadAssignment(int target) {
        const int n = NewNode(K::Assign, cur_);
        Nd(n).first = Nd(target).first;
        Add(n, target);
        Advance();
        Add(n, ReadExpr('r'));
        return Close(n);
    }

    bool AtSelector() const {
        const T k = Kind();
        return k == T::LBracket || k == T::LBrace || k == T::BangBracket || k == T::Dot || k == T::BangDot ||
               k == T::LParen;
    }

    int ReadSelectors(int obj, bool *was_call) {
        *was_call = false;
        while (AtSelector()) {
            DepthGuard g(this);
            const T k = Kind();
            const int first = Nd(obj).first;
            if (k == T::LBracket) {
                const int n = NewNode(K::Index, cur_);
                Nd(n).first = first;
                Add(n, obj);
                Advance();
                Add(n, ReadExpr('r'));
                int count = 1;
                while (Kind() == T::Comma) {
                    Advance();
                    Add(n, ReadExpr('r'));
                    count++;
                }
                if (count > 2) Fail("'[]' only supports 1 or 2 indices");
                Match(T::RBracket, "]");
                obj = Close(n);
                *was_call = false;
            } else if (k == T::LBrace) {
                const int n = NewNode(K::SubList, cur_);
                Nd(n).first = first;
                Add(n, obj);
                Advance();
                Add(n, ReadExpr('r'));
                Match(T::RBrace, "}");
                obj = Close(n);
                *was_call = false;
            } else if (k == T::BangBracket) {
                const int n = NewNode(K::PosIndex, cur_);
                Nd(n).first = first;
                Add(n, obj);
                Advance();
                Add(n, ReadExpr('r'));
                Match(T::RBracket, "]");
                obj = Close(n);
                *was_call = false;
            } else if (k == T::Dot || k == T::BangDot) {
                const int n = NewNode(k == T::Dot ? K::Field : K::CompField, cur_);
                Nd(n).first = first;
                Add(n, obj);
                Advance();
                if (Kind() == T::Ident) {
                    Add(n, Close(NewNode(K::Ident, cur_)));
                    Advance();
                } else if (Kind() == T::Int) {
                    Add(n, Close(NewNode(K::Int, cur_)));
                    Advance();
                } else if (Kind() == T::LParen) {
                    const int pn = NewNode(K::Paren, cur_);
                    Advance();
                    Add(pn, ReadExpr('r'));
                    Match(T::RParen, ")");
                    Add(n, Close(pn));
                } else {
                    Fail("Record component name expected");
                }
                obj = Close(n);
                *was_call = false;
            } else {
                const int n = NewNode(K::Call, cur_);
                Nd(n).first = first;
                Add(n, obj);
                Advance();
                if (Kind() != T::RParen && Kind() != T::Colon) {
                    Add(n, ReadExpr('r'));
                    while (Kind() == T::Comma) {
                        Advance();
                        Add(n, ReadExpr('r'));
                    }
                }
                if (Kind() == T::Colon) {
                    Advance();
                    if (Kind() != T::RParen) {
                        Add(n, ReadOption());
                        while (Kind() == T::Comma) {
                            Advance();
                            Add(n, ReadOption());
                        }
                    }
                }
                Match(T::RParen, ")");
                obj = Close(n);
                *was_call = true;
            }
        }
        return obj;
    }

    int ReadOption() {
        const int n = NewNode(K::Option, cur_);
        if (Kind() == T::Ident) {
            Add(n, Close(NewNode(K::Ident, cur_)));
            Advance();
        } else if (Kind() == T::LParen) {
            const int pn = NewNode(K::Paren, cur_);
            Advance();
            Add(pn, ReadExpr('r'));
            Match(T::RParen, ")");
            Add(n, Close(pn));
        } else {
            Fail("Identifier expected");
        }
        if (Kind() == T::Assign) {
            Advance();
            Add(n, ReadExpr('r'));
        }
        return Close(n);
    }

    // ---- expressions ----

    int ReadExpr(char mode, int *stmt = nullptr) {
        DepthGuard g(this);
        int left = ReadAnd(mode, stmt);
        if (stmt && *stmt >= 0) return left;
        while (Kind() == T::Or) {
            left = Binary(left, [&] { return ReadAnd('r', nullptr); });
        }
        return left;
    }

    template <class F>
    int Binary(int left, F right) {
        const int n = NewNode(K::Binary, cur_);
        Nd(n).first = Nd(left).first;
        Add(n, left);
        Advance();
        Add(n, right());
        return Close(n);
    }

    int ReadAnd(char mode, int *stmt) {
        int left = ReadRel(mode, stmt);
        if (stmt && *stmt >= 0) return left;
        while (Kind() == T::And) {
            left = Binary(left, [&] { return ReadRel('r', nullptr); });
        }
        return left;
    }

    int ReadRel(char mode, int *stmt) {
        std::vector<int> nots;
        while (Kind() == T::Not) {
            nots.push_back(cur_);
            Advance();
            mode = 'r';
        }
        int left = ReadAri(mode, nots.empty() ? stmt : nullptr);
        if (stmt && *stmt >= 0) return left;
        if (IsRelOp(Kind())) {
            left = Binary(left, [&] { return ReadAri('r', nullptr); });
        }
        for (size_t i = nots.size(); i-- > 0;) {
            const int n = NewNode(K::Not, nots[i]);
            Add(n, left);
            left = Close(n);
        }
        return left;
    }

    int ReadAri(char mode, int *stmt) {
        int left = ReadTerm(mode, stmt);
        if (stmt && *stmt >= 0) return left;
        while (Kind() == T::Plus || Kind() == T::Minus) {
            left = Binary(left, [&] { return ReadTerm('r', nullptr); });
        }
        return left;
    }

    int ReadTerm(char mode, int *stmt) {
        int left = ReadFactor(mode, stmt);
        if (stmt && *stmt >= 0) return left;
        while (Kind() == T::Star || Kind() == T::Slash || Kind() == T::Mod) {
            left = Binary(left, [&] { return ReadFactor('r', nullptr); });
        }
        return left;
    }

    int ReadFactor(char mode, int *stmt) {
        DepthGuard g(this);
        int sign = -1;
        if (Kind() == T::Plus || Kind() == T::Minus) {
            sign = cur_;
            Advance();
            mode = 'r';
            stmt = nullptr;
        }
        int left = ReadAtom(mode, stmt);
        if (stmt && *stmt >= 0) return left;
        if (Kind() == T::Caret) {
            const int n = NewNode(K::Binary, cur_);
            Nd(n).first = Nd(left).first;
            Add(n, left);
            Advance();
            int sign2 = -1;
            if (Kind() == T::Plus || Kind() == T::Minus) {
                sign2 = cur_;
                Advance();
            }
            int right = ReadAtom('r', nullptr);
            if (sign2 >= 0) {
                const int u = NewNode(K::Unary, sign2);
                Add(u, right);
                right = Close(u);
            }
            Add(n, right);
            left = Close(n);
            if (Kind() == T::Caret) Fail("'^' is not associative");
        }
        if (sign >= 0) {
            const int u = NewNode(K::Unary, sign);
            Add(u, left);
            left = Close(u);
        }
        return left;
    }

    int Literal(K kind) {
        const int n = Close(NewNode(kind, cur_));
        Advance();
        return n;
    }

    int ReadAtom(char mode, int *stmt) {
        DepthGuard g(this);
        bool was_call = false;
        switch (Kind()) {
        case T::Ident: return ReadCallVarAss(mode == 'x' ? 'x' : 'r', stmt);
        case T::IsBound: {
            const int n = NewNode(K::IsBound, cur_);
            Advance();
            Match(T::LParen, "(");
            if (Kind() != T::Ident) Fail("Identifier expected");
            Add(n, ReadCallVarAss('i'));
            Match(T::RParen, ")");
            return ReadSelectors(Close(n), &was_call);
        }
        case T::Function: return ReadSelectors(ReadFunction(false), &was_call);
        case T::Atomic: {
            const int at = cur_;
            Advance();
            if (Kind() != T::Function) Fail("function expected");
            const int f = ReadFunction(true);
            Nd(f).first = at;
            return ReadSelectors(f, &was_call);
        }
        case T::Int: return ReadSelectors(Literal(K::Int), &was_call);
        case T::Float: return ReadSelectors(Literal(K::Float), &was_call);
        case T::String: return ReadSelectors(Literal(K::String), &was_call);
        case T::Char: return ReadSelectors(Literal(K::Char), &was_call);
        case T::True: return ReadSelectors(Literal(K::True), &was_call);
        case T::False: return ReadSelectors(Literal(K::False), &was_call);
        case T::Tilde:
            if (tilde_ok_ == 0) Fail("'~' not allowed here");
            tilde_seen_ = true;
            return ReadSelectors(Literal(K::Tilde), &was_call);
        case T::LBracket: return ReadSelectors(ReadList(), &was_call);
        case T::Rec: return ReadSelectors(ReadRec(), &was_call);
        case T::LBrace: return ReadLambdaMulti();
        case T::LParen: return ReadParenOrPerm(&was_call);
        case T::Dot:
            RescanFloatHere();
            return ReadSelectors(Literal(K::Float), &was_call);
        case T::Arrow: case T::BangDot: case T::BangBracket: case T::Help: case T::For: case T::If:
        case T::Repeat: case T::While:
            Fail("literal expected");
        default: Fail("expression expected");
        }
    }

    int ReadParenOrPerm(bool *was_call) {
        const int open = cur_;
        Advance();
        if (Kind() == T::RParen) {
            const int n = NewNode(K::IdPerm, open);
            Advance();
            return Close(n);
        }
        const int e = ReadExpr('r');
        if (Kind() == T::Comma) {
            const int perm = NewNode(K::Perm, open);
            int cyc = NewNode(K::Cycle, open);
            Add(cyc, e);
            while (Kind() == T::Comma) {
                Advance();
                Add(cyc, ReadExpr('r'));
            }
            Match(T::RParen, ")");
            Add(perm, Close(cyc));
            while (Kind() == T::LParen) {
                cyc = NewNode(K::Cycle, cur_);
                Advance();
                // A later cycle may have one point: `(1,2)(3)`.
                Add(cyc, ReadExpr('r'));
                while (Kind() == T::Comma) {
                    Advance();
                    Add(cyc, ReadExpr('r'));
                }
                Match(T::RParen, ")");
                Add(perm, Close(cyc));
            }
            return Close(perm);
        }
        Match(T::RParen, ")");
        const int n = NewNode(K::Paren, open);
        Add(n, e);
        return ReadSelectors(Close(n), was_call);
    }

    // `~` stands for the outermost list or record literal being built; it may not be used while that literal
    // turns out to be a range. tilde_seen_ records a `~` since the outermost literal began.
    int ReadList() {
        const int n = NewNode(K::List, cur_);
        Advance();
        const bool outermost = tilde_ok_ == 0;
        if (outermost) tilde_seen_ = false;
        tilde_ok_++;
        int count = 0;
        bool holes = false;
        bool range = false;
        while (Kind() != T::RBracket) {
            if (Kind() == T::Comma) {
                Add(n, Close(NewNode(K::Hole, cur_)));
                holes = true;
            } else {
                Add(n, ReadExpr('r'));
            }
            count++;
            if (Kind() == T::Comma) {
                Advance();
                continue;
            }
            if (Kind() == T::DotDot) {
                if (holes) Fail("Must have no unbound entries in range");
                if (count > 2) Fail("Must have at most 2 entries before '..'");
                Advance();
                Add(n, ReadExpr('r'));
                range = true;
                if (outermost && tilde_seen_) Fail("Sorry, '~' not allowed in range");
                break;
            }
            if (Kind() == T::DotDotDot) Fail("Only two dots in a range");
            break;
        }
        Match(T::RBracket, "]");
        tilde_ok_--;
        if (range) Nd(n).kind = K::Range;
        return Close(n);
    }

    int ReadRec() {
        const int n = NewNode(K::Rec, cur_);
        Advance();
        Match(T::LParen, "(");
        if (tilde_ok_ == 0) tilde_seen_ = false;
        tilde_ok_++;
        if (Kind() == T::Comma) Advance();  // GAP allows one comma before the first component
        while (Kind() != T::RParen) {
            const int e = NewNode(K::RecEntry, cur_);
            if (Kind() == T::Ident) {
                Add(e, Close(NewNode(K::Ident, cur_)));
                Advance();
            } else if (Kind() == T::Int) {
                Add(e, Close(NewNode(K::Int, cur_)));
                Advance();
            } else if (Kind() == T::LParen) {
                const int pn = NewNode(K::Paren, cur_);
                Advance();
                Add(pn, ReadExpr('r'));
                Match(T::RParen, ")");
                Add(e, Close(pn));
            } else {
                Fail("Identifier expected");
            }
            Match(T::Assign, ":=");
            Add(e, ReadExpr('r'));
            Add(n, Close(e));
            if (Kind() != T::Comma) break;
            Advance();
        }
        Match(T::RParen, ")");
        tilde_ok_--;
        return Close(n);
    }

    // Parameter names: `a, b, c...`; qualifiers only for an atomic function.
    void ReadParamList(int params, T close, bool atomic, std::vector<std::string> *names) {
        if (Kind() == close) return;
        for (;;) {
            const int pr = NewNode(K::Param, cur_);
            if (Kind() == T::Readonly || Kind() == T::Readwrite) {
                if (!atomic)
                    Fail(Kind() == T::Readonly ? "'readonly' argument of non-atomic function"
                                               : "'readwrite' argument of non-atomic function");
                Nd(pr).qualifier = cur_;
                Advance();
            }
            if (Kind() != T::Ident) Fail("identifier expected");
            if (std::find(names->begin(), names->end(), Cur().name) != names->end())
                Fail("Name used for two arguments");
            names->push_back(Cur().name);
            Nd(pr).tok = cur_;
            Advance();
            if (Kind() == T::DotDotDot) {
                Nd(pr).variadic = true;
                Nd(params).variadic = true;
                Advance();
                Add(params, Close(pr));
                if (Kind() == T::Comma) Fail("Only final argument can be variadic");
                break;
            }
            if (Kind() == T::DotDot) Fail("Three dots required for variadic argument list");
            Add(params, Close(pr));
            if (Kind() != T::Comma) break;
            Advance();
        }
        // The classic variadic form: one parameter called `arg`.
        if (Nd(params).kids.size() == 1 && names->size() == 1 && (*names)[0] == "arg") {
            Nd(Nd(params).kids[0]).variadic = true;
            Nd(params).variadic = true;
        }
    }

    int ReadLocals(const std::vector<std::string> &params) {
        const int n = NewNode(K::Locals, cur_);
        Advance();
        std::vector<std::string> names;
        for (;;) {
            if (Kind() != T::Ident) Fail("identifier expected");
            if (std::find(params.begin(), params.end(), Cur().name) != params.end())
                Fail("Name used for argument and local");
            if (std::find(names.begin(), names.end(), Cur().name) != names.end()) Fail("Name used for two locals");
            names.push_back(Cur().name);
            Add(n, Close(NewNode(K::Ident, cur_)));
            Advance();
            if (Kind() != T::Comma) break;
            Advance();
        }
        Match(T::Semi, ";");
        return Close(n);
    }

    int ReadFunction(bool atomic) {
        DepthGuard g(this);
        const int n = NewNode(K::Function, cur_);
        Nd(n).atomic = atomic;
        Advance();
        Match(T::LParen, "(");
        const int params = NewNode(K::Params, cur_ - 1);
        std::vector<std::string> names;
        ReadParamList(params, T::RParen, atomic, &names);
        Match(T::RParen, ")");
        Add(n, Close(params));
        if (Kind() == T::Local) Add(n, ReadLocals(names));
        Nd(n).split = static_cast<int>(Nd(n).kids.size());
        const int saved_loops = loops_;
        loops_ = 0;
        ReadStats(n);
        loops_ = saved_loops;
        Match(T::End, "while parsing a function: statement or 'end'");
        return Close(n);
    }

    int ReadLambdaSingle(int id) {
        const int n = NewNode(K::Lambda, Nd(id).first);
        const int params = NewNode(K::Params, Nd(id).first);
        const int pr = NewNode(K::Param, Nd(id).tok);
        Nd(pr).last = Nd(id).tok;
        Add(params, pr);
        Nd(params).last = Nd(id).tok;
        Add(n, params);
        Advance();  // ->
        const int saved_loops = loops_;
        loops_ = 0;
        Add(n, ReadExpr('r'));
        loops_ = saved_loops;
        return Close(n);
    }

    int ReadLambdaMulti() {
        const int n = NewNode(K::Lambda, cur_);
        const int params = NewNode(K::Params, cur_);
        Advance();
        std::vector<std::string> names;
        ReadParamList(params, T::RBrace, false, &names);
        Match(T::RBrace, "}");
        Add(n, Close(params));
        Match(T::Arrow, "->");
        const int saved_loops = loops_;
        loops_ = 0;
        Add(n, ReadExpr('r'));
        loops_ = saved_loops;
        return Close(n);
    }
};

}  // namespace

const char *TokenName(T kind) {
    switch (kind) {
    case T::Eof: return "<end-of-file>";
    case T::Ident: return "identifier";
    case T::Int: return "integer";
    case T::Float: return "float";
    case T::String: return "string";
    case T::Char: return "character";
    case T::Assign: return ":=";
    case T::Colon: return ":";
    case T::Eq: return "=";
    case T::Ne: return "<>";
    case T::Lt: return "<";
    case T::Le: return "<=";
    case T::Gt: return ">";
    case T::Ge: return ">=";
    case T::Plus: return "+";
    case T::Minus: return "-";
    case T::Star: return "*";
    case T::Slash: return "/";
    case T::Caret: return "^";
    case T::Tilde: return "~";
    case T::Dot: return ".";
    case T::DotDot: return "..";
    case T::DotDotDot: return "...";
    case T::BangDot: return "!.";
    case T::BangBracket: return "![";
    case T::Arrow: return "->";
    case T::LParen: return "(";
    case T::RParen: return ")";
    case T::LBracket: return "[";
    case T::RBracket: return "]";
    case T::LBrace: return "{";
    case T::RBrace: return "}";
    case T::Comma: return ",";
    case T::Semi: return ";";
    case T::DualSemi: return ";;";
    case T::Help: return "?";
    case T::Illegal: return "illegal character";
    default:
        for (const KeywordEntry &k : kKeywords)
            if (k.kind == kind) return k.spelling;
        return "?";
    }
}

bool IsKeyword(T kind) { return kind >= T::And && kind <= T::Assert; }

const char *KindName(K kind) {
    static const char *const names[] = {
        "File", "Assign", "CallStat", "ExprStat", "If", "Elif", "Else", "While", "Repeat", "For", "Atomic",
        "QualExpr", "Break", "Continue", "Return", "Empty", "Unbind", "Info", "Assert", "TryNext", "Quit", "Help",
        "Locals", "Ident", "Int", "Float", "String", "Char", "True", "False", "Tilde", "Binary", "Unary", "Not",
        "Call", "Option", "Index", "SubList", "PosIndex", "PosSubList", "Field", "CompField", "List", "Hole",
        "Range", "Rec", "RecEntry", "Perm", "Cycle", "IdPerm", "Paren", "Function", "Lambda", "Params", "Param",
        "IsBound",
    };
    const size_t i = static_cast<size_t>(kind);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
}

void Tokenize(std::string_view text, std::vector<Token> *tokens, std::vector<Comment> *comments) {
    Scanner sc(text);
    bool after_dot = false;
    for (;;) {
        Token t = sc.Next(after_dot, comments);
        after_dot = t.kind == T::Dot || t.kind == T::BangDot;
        const bool eof = t.kind == T::Eof;
        tokens->push_back(std::move(t));
        if (eof) break;
    }
}

Parse ParseGap(std::string_view text, const Options &opt) {
    Parse p;
    p.text = text;
    Parser parser(&p, opt);
    try {
        parser.Run();
    } catch (...) {
        // A scanner error after the parse ended (while completing the token list) is not reported.
    }
    if (p.tokens.empty() || p.tokens.back().kind != T::Eof) {
        // Complete the token list after an abandoned scan.
        std::vector<Token> rest;
        std::vector<Comment> comments;
        Tokenize(text, &rest, &comments);
        p.tokens = std::move(rest);
        p.comments = std::move(comments);
    }
    return p;
}

std::string GapCheck(std::string_view text, bool as_function) {
    Options o;
    o.as_function = as_function;
    const Parse p = ParseGap(text, o);
    if (p.errors.empty()) return "ok";
    const Error &e = p.errors[0];
    return std::to_string(e.line + 1) + ":" + std::to_string(e.col + 1) + ": " + e.message;
}

}  // namespace gapparse
