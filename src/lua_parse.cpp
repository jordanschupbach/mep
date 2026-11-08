// The Lua 5.4 parser behind mep-lua-lsp: see lua_parse.h. The structure
// follows Lua's llex.c and lparser.c function for function (the names are
// theirs), so a reader can hold the two side by side; what is added is the
// tree, the name resolution and the recovery after an error.
#include "lua_parse.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace luaparse {

namespace {

// ---- tokens (llex.h's RESERVED, in its order) ------------------------------

enum : int {
    TK_AND = 256, TK_BREAK, TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION, TK_GOTO, TK_IF,
    TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT, TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
    TK_IDIV, TK_CONCAT, TK_DOTS, TK_EQ, TK_GE, TK_LE, TK_NE, TK_SHL, TK_SHR, TK_DBCOLON, TK_EOS,
    TK_FLT, TK_INT, TK_NAME, TK_STRING,
    TK_LEXERR,  // mep's: a lexical error, raised when the parser reaches it
};
constexpr int kFirstReserved = 256;
constexpr int kNumReserved = TK_WHILE - TK_AND + 1;
constexpr int kEOZ = -1;

const char *const kTokens[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil",
    "not", "or", "repeat", "return", "then", "true", "until", "while", "//", "..", "...", "==", ">=", "<=", "~=",
    "<<", ">>", "::", "<eof>", "<number>", "<integer>", "<name>", "<string>",
};

bool LIsAlpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool LIsDigit(int c) { return c >= '0' && c <= '9'; }
bool LIsAlnum(int c) { return LIsAlpha(c) || LIsDigit(c); }
bool LIsXDigit(int c) { return LIsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool LIsSpace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
bool LIsPrint(int c) { return c >= 32 && c < 127; }
int HexValue(int c) { return LIsDigit(c) ? c - '0' : (c | 0x20) - 'a' + 10; }

std::string Token2Str(int token) {
    if (token < kFirstReserved) {
        if (token >= 0 && LIsPrint(token)) return std::string("'") + static_cast<char>(token) + "'";
        return "'<\\" + std::to_string(token) + ">'";
    }
    const char *s = kTokens[token - kFirstReserved];
    if (token < TK_EOS) return std::string("'") + s + "'";
    return s;
}

struct Token {
    int type = TK_EOS;
    int start = 0, end = 0;  // byte range in the source
    int line = 0;            // line where it starts
    int end_line = 0;        // the scanner's line once it has read the token (ls->linenumber)
    std::string near_text;   // NAME/STRING/numbers: how the scanner's buffer shows it
    std::string value;       // NAME: the name; STRING: the value
    // TK_LEXERR
    std::string err;
    int err_line = 0;
    std::string err_code;
};

// ---- luaO_str2num -----------------------------------------------------------

bool Str2Int(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && LIsSpace(static_cast<unsigned char>(s[i]))) i++;
    bool neg = false;
    if (i < s.size() && s[i] == '-') {
        neg = true;
        i++;
    } else if (i < s.size() && s[i] == '+') {
        i++;
    }
    bool empty = true;
    if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        i += 2;
        while (i < s.size() && LIsXDigit(static_cast<unsigned char>(s[i]))) {
            i++;
            empty = false;
        }
    } else {
        const uint64_t maxby10 = static_cast<uint64_t>(INT64_MAX) / 10;
        const uint64_t maxlastd = static_cast<uint64_t>(INT64_MAX) % 10;
        uint64_t a = 0;
        while (i < s.size() && LIsDigit(static_cast<unsigned char>(s[i]))) {
            const uint64_t d = static_cast<uint64_t>(s[i] - '0');
            if (a >= maxby10 && (a > maxby10 || d > maxlastd + (neg ? 1 : 0))) return false;
            a = a * 10 + d;
            i++;
            empty = false;
        }
    }
    while (i < s.size() && LIsSpace(static_cast<unsigned char>(s[i]))) i++;
    return !empty && i == s.size();
}

bool Str2Num(const std::string &s) {
    if (Str2Int(s)) return true;
    if (s.find_first_of("nN") != std::string::npos) return false;
    const char *b = s.c_str();
    char *endp = nullptr;
    errno = 0;
    (void)std::strtod(b, &endp);
    if (endp == b) return false;
    while (*endp && LIsSpace(static_cast<unsigned char>(*endp))) endp++;
    return *endp == '\0';
}

// ---- the scanner (llex.c) ---------------------------------------------------

struct LexFail {
    std::string msg;
    int code_token;  // the token for "near", 0 for none
};

class Lexer {
public:
    Lexer(std::string_view src, size_t start, std::vector<Comment> *comments)
        : s_(src), p_(start), comments_(comments) {
        cur_ = p_ < s_.size() ? static_cast<unsigned char>(s_[p_]) : kEOZ;
    }

    std::vector<Token> Run() {
        std::vector<Token> out;
        for (;;) {
            Token t;
            buff_.clear();
            try {
                t.type = Llex(&t);
            } catch (const LexFail &f) {
                t.type = TK_LEXERR;
                t.err_line = line_;
                t.err = f.msg;
                if (f.code_token) t.err += " near " + TxtToken(f.code_token);
                t.end = static_cast<int>(p_);
                if (t.start >= t.end) t.end = t.start + 1;
                t.end_line = line_;
                out.push_back(t);
                // Carry on after the rest of the line, so an editor still gets the tokens after it.
                while (cur_ != kEOZ && cur_ != '\n' && cur_ != '\r') Next();
                continue;
            }
            t.end = static_cast<int>(p_);
            t.end_line = line_;
            if (t.type == TK_NAME || t.type == TK_STRING || t.type == TK_FLT || t.type == TK_INT) {
                const size_t z = buff_.find('\0');
                t.near_text = z == std::string::npos ? buff_ : buff_.substr(0, z);
            }
            out.push_back(t);
            if (t.type == TK_EOS) break;
        }
        return out;
    }

private:
    std::string_view s_;
    size_t p_;
    int cur_;
    int line_ = 1;
    std::string buff_;
    std::vector<Comment> *comments_;

    void Next() {
        p_++;
        cur_ = p_ < s_.size() ? static_cast<unsigned char>(s_[p_]) : kEOZ;
    }
    void Save(int c) { buff_ += static_cast<char>(c); }
    void SaveAndNext() {
        Save(cur_);
        Next();
    }
    bool CurrIsNewline() const { return cur_ == '\n' || cur_ == '\r'; }

    std::string TxtToken(int token) {
        switch (token) {
            case TK_NAME:
            case TK_STRING:
            case TK_FLT:
            case TK_INT: {
                const size_t z = buff_.find('\0');
                return "'" + (z == std::string::npos ? buff_ : buff_.substr(0, z)) + "'";
            }
            default:
                return Token2Str(token);
        }
    }
    [[noreturn]] void LexError(const std::string &msg, int token) { throw LexFail{msg, token}; }

    void IncLineNumber() {
        const int old = cur_;
        Next();
        if (CurrIsNewline() && cur_ != old) Next();
        ++line_;
    }

    bool CheckNext1(int c) {
        if (cur_ == c) {
            Next();
            return true;
        }
        return false;
    }
    bool CheckNext2(const char *set) {
        if (cur_ == set[0] || cur_ == set[1]) {
            SaveAndNext();
            return true;
        }
        return false;
    }

    int ReadNumeral() {
        const char *expo = "Ee";
        const int first = cur_;
        SaveAndNext();
        if (first == '0' && CheckNext2("xX")) expo = "Pp";
        for (;;) {
            if (CheckNext2(expo)) {
                CheckNext2("-+");
            } else if (LIsXDigit(cur_) || cur_ == '.') {
                SaveAndNext();
            } else {
                break;
            }
        }
        if (LIsAlpha(cur_)) SaveAndNext();  // force an error
        Save('\0');
        const std::string num = buff_.substr(0, buff_.size() - 1);
        if (!Str2Num(num)) LexError("malformed number", TK_FLT);
        return Str2Int(num) ? TK_INT : TK_FLT;
    }

    size_t SkipSep() {
        size_t count = 0;
        const int s = cur_;
        SaveAndNext();
        while (cur_ == '=') {
            SaveAndNext();
            count++;
        }
        return cur_ == s ? count + 2 : count == 0 ? 1 : 0;
    }

    void ReadLongString(Token *tok, size_t sep, int start_off) {
        const int line = line_;
        SaveAndNext();  // skip 2nd '['
        if (CurrIsNewline()) IncLineNumber();
        for (;;) {
            switch (cur_) {
                case kEOZ: {
                    const char *what = tok ? "string" : "comment";
                    if (!tok) comments_->push_back({start_off, static_cast<int>(p_), true, line});
                    LexError(std::string("unfinished long ") + what + " (starting at line " + std::to_string(line) + ")",
                             TK_EOS);
                }
                case ']':
                    if (SkipSep() == sep) {
                        SaveAndNext();
                        goto endloop;
                    }
                    break;
                case '\n':
                case '\r':
                    Save('\n');
                    IncLineNumber();
                    if (!tok) buff_.clear();
                    break;
                default:
                    if (tok) {
                        SaveAndNext();
                    } else {
                        Next();
                    }
            }
        }
    endloop:
        if (tok) tok->value = buff_.substr(sep, buff_.size() - 2 * sep);
    }

    void EscCheck(bool c, const char *msg) {
        if (!c) {
            if (cur_ != kEOZ) SaveAndNext();
            LexError(msg, TK_STRING);
        }
    }
    int GetHexa() {
        SaveAndNext();
        EscCheck(LIsXDigit(cur_), "hexadecimal digit expected");
        return HexValue(cur_);
    }
    int ReadHexaEsc() {
        int r = GetHexa();
        r = (r << 4) + GetHexa();
        buff_.resize(buff_.size() - 2);
        return r;
    }
    unsigned long ReadUtf8Esc() {
        unsigned long r;
        size_t i = 4;
        SaveAndNext();  // skip 'u'
        EscCheck(cur_ == '{', "missing '{'");
        r = static_cast<unsigned long>(GetHexa());
        while (SaveAndNext(), LIsXDigit(cur_)) {
            i++;
            EscCheck(r <= (0x7FFFFFFFu >> 4), "UTF-8 value too large");
            r = (r << 4) + static_cast<unsigned long>(HexValue(cur_));
        }
        EscCheck(cur_ == '}', "missing '}'");
        Next();
        buff_.resize(buff_.size() - i);
        return r;
    }
    void Utf8Esc() {
        unsigned long x = ReadUtf8Esc();
        // luaO_utf8esc
        char buf[8];
        int n = 1;
        if (x < 0x80) {
            buf[7] = static_cast<char>(x);
        } else {
            unsigned int mfb = 0x3f;
            do {
                buf[8 - (n++)] = static_cast<char>(0x80 | (x & 0x3f));
                x >>= 6;
                mfb >>= 1;
            } while (x > mfb);
            buf[8 - n] = static_cast<char>((~mfb << 1) | x);
        }
        for (; n > 0; n--) Save(static_cast<unsigned char>(buf[8 - n]));
    }
    int ReadDecEsc() {
        int i;
        int r = 0;
        for (i = 0; i < 3 && LIsDigit(cur_); i++) {
            r = 10 * r + cur_ - '0';
            SaveAndNext();
        }
        EscCheck(r <= 255, "decimal escape too large");
        buff_.resize(buff_.size() - static_cast<size_t>(i));
        return r;
    }

    void ReadString(int del, Token *tok) {
        SaveAndNext();
        while (cur_ != del) {
            switch (cur_) {
                case kEOZ:
                    LexError("unfinished string", TK_EOS);
                case '\n':
                case '\r':
                    LexError("unfinished string", TK_STRING);
                case '\\': {
                    // 0: read the char and save c; 1: save c (already past it); 2: save nothing
                    int c = 0;
                    int action = 0;
                    SaveAndNext();  // keep '\\' for error messages
                    switch (cur_) {
                        case 'a': c = '\a'; break;
                        case 'b': c = '\b'; break;
                        case 'f': c = '\f'; break;
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'v': c = '\v'; break;
                        case 'x': c = ReadHexaEsc(); break;
                        case 'u':
                            Utf8Esc();
                            action = 2;
                            break;
                        case '\n':
                        case '\r':
                            IncLineNumber();
                            c = '\n';
                            action = 1;
                            break;
                        case '\\':
                        case '"':
                        case '\'':
                            c = cur_;
                            break;
                        case kEOZ:
                            action = 2;
                            break;
                        case 'z': {
                            buff_.pop_back();
                            Next();
                            while (LIsSpace(cur_)) {
                                if (CurrIsNewline()) {
                                    IncLineNumber();
                                } else {
                                    Next();
                                }
                            }
                            action = 2;
                            break;
                        }
                        default:
                            EscCheck(LIsDigit(cur_), "invalid escape sequence");
                            c = ReadDecEsc();
                            action = 1;
                            break;
                    }
                    if (action == 0) Next();
                    if (action <= 1) {
                        buff_.pop_back();  // remove '\\'
                        Save(c);
                    }
                    break;
                }
                default:
                    SaveAndNext();
            }
        }
        SaveAndNext();
        tok->value = buff_.substr(1, buff_.size() - 2);
    }

    int Llex(Token *t) {
        for (;;) {
            t->start = static_cast<int>(p_);
            t->line = line_;
            switch (cur_) {
                case '\n':
                case '\r':
                    IncLineNumber();
                    break;
                case ' ':
                case '\f':
                case '\t':
                case '\v':
                    Next();
                    break;
                case '-': {
                    Next();
                    if (cur_ != '-') return '-';
                    const int cstart = t->start, cline = line_;
                    Next();
                    if (cur_ == '[') {
                        const size_t sep = SkipSep();
                        buff_.clear();
                        if (sep >= 2) {
                            ReadLongString(nullptr, sep, cstart);
                            buff_.clear();
                            comments_->push_back({cstart, static_cast<int>(p_), true, cline});
                            break;
                        }
                    }
                    while (!CurrIsNewline() && cur_ != kEOZ) Next();
                    comments_->push_back({cstart, static_cast<int>(p_), false, cline});
                    break;
                }
                case '[': {
                    const size_t sep = SkipSep();
                    if (sep >= 2) {
                        ReadLongString(t, sep, t->start);
                        return TK_STRING;
                    }
                    if (sep == 0) LexError("invalid long string delimiter", TK_STRING);
                    return '[';
                }
                case '=':
                    Next();
                    return CheckNext1('=') ? TK_EQ : '=';
                case '<':
                    Next();
                    if (CheckNext1('=')) return TK_LE;
                    if (CheckNext1('<')) return TK_SHL;
                    return '<';
                case '>':
                    Next();
                    if (CheckNext1('=')) return TK_GE;
                    if (CheckNext1('>')) return TK_SHR;
                    return '>';
                case '/':
                    Next();
                    return CheckNext1('/') ? TK_IDIV : '/';
                case '~':
                    Next();
                    return CheckNext1('=') ? TK_NE : '~';
                case ':':
                    Next();
                    return CheckNext1(':') ? TK_DBCOLON : ':';
                case '"':
                case '\'':
                    ReadString(cur_, t);
                    return TK_STRING;
                case '.':
                    SaveAndNext();
                    if (CheckNext1('.')) return CheckNext1('.') ? TK_DOTS : TK_CONCAT;
                    if (!LIsDigit(cur_)) return '.';
                    return ReadNumeral();
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                    return ReadNumeral();
                case kEOZ:
                    return TK_EOS;
                default: {
                    if (LIsAlpha(cur_)) {
                        do {
                            SaveAndNext();
                        } while (LIsAlnum(cur_));
                        t->value = buff_;
                        for (int i = 0; i < kNumReserved; i++) {
                            if (buff_ == kTokens[i]) return kFirstReserved + i;
                        }
                        return TK_NAME;
                    }
                    const int c = cur_;
                    Next();
                    return c;
                }
            }
        }
    }
};

// ---- the parser (lparser.c) ---------------------------------------------------

struct ParseFail {};

constexpr int kMaxVars = 200;
constexpr int kMaxUpval = 255;
constexpr int kMaxCCalls = 200;

struct VarSlot {
    Var *var = nullptr;  // null for "(for state)"
    std::string name;
    bool ctc = false;    // compile-time constant (not a register, never an upvalue)
    bool readonly = false;
};

struct Upval {
    std::string name;
    Var *var = nullptr;
    bool readonly = false;
};

struct LabelDesc {
    std::string name;
    int line = 0;
    int nactvar = 0;
    int start = 0, end = 0;  // where it is written (for the underline)
};

struct BlockCnt {
    BlockCnt *previous = nullptr;
    int nactvar = 0;
    size_t firstlabel = 0, firstgoto = 0;
    bool isloop = false;
};

struct FuncState {
    FuncState *prev = nullptr;
    FuncBody *f = nullptr;     // null for the main chunk
    int linedefined = 0;
    bool is_vararg = false;
    size_t firstlocal = 0;     // index into actvar_
    size_t firstlabel = 0;
    int nactvar = 0;
    std::vector<Upval> upvalues;
    BlockCnt *bl = nullptr;
};

struct Resolved {
    enum K { Void, Local, Const, Upval } k = Void;
    Var *var = nullptr;
    bool readonly = false;
};

class Parser {
public:
    Parser(std::vector<Token> toks, ParseResult *r) : toks_(std::move(toks)), r_(r) {}

    void MainFunc() {
        FuncState fs;
        BlockCnt bl;
        OpenFunc(&fs, &bl);
        fs.is_vararg = true;
        fs.upvalues.push_back({"_ENV", nullptr, false});
        try {
            NextRaw();
            if (t().type == TK_LEXERR) RaiseLex(t());
            Statlist(&r_->chunk);
            r_->chunk.start = toks_.empty() ? 0 : toks_.front().start;
            r_->chunk.end = t().start;
            Check(TK_EOS);
            CloseFunc();
        } catch (const ParseFail &) {
            // Only reachable before any statement list could catch it (or from the final checks).
        }
    }

private:
    std::vector<Token> toks_;
    size_t idx_ = 0;
    bool started_ = false;
    bool la_active_ = false;
    int prev_end_ = 0;
    ParseResult *r_;
    FuncState *fs_ = nullptr;
    std::vector<VarSlot> actvar_;
    std::vector<LabelDesc> labels_, gotos_;
    int level_ = 1;

    const Token &t() const { return toks_[idx_]; }
    const Token &Peek() const { return toks_[std::min(idx_ + 1, toks_.size() - 1)]; }
    int LineNow() const { return la_active_ ? Peek().end_line : t().end_line; }

    // ---- errors ----
    void Record(const std::string &msg, const std::string &code, int start, int end, int line) {
        if (r_->has_error) return;
        Error e;
        e.line = line;
        e.start = start;
        e.end = std::max(end, start);
        e.message = msg;
        e.code = code;
        r_->errors.push_back(e);
        r_->has_error = true;
        r_->first_error_offset = start;
    }
    [[noreturn]] void SyntaxError(const std::string &msg) {
        std::string near;
        switch (t().type) {
            case TK_NAME:
            case TK_STRING:
            case TK_FLT:
            case TK_INT:
                near = "'" + t().near_text + "'";
                break;
            default:
                near = Token2Str(t().type);
        }
        Record(msg + " near " + near, "syntax-error", t().start, t().type == TK_EOS ? t().start : t().end, LineNow());
        throw ParseFail{};
    }
    [[noreturn]] void SemError(const std::string &msg, const std::string &code, int start, int end) {
        Record(msg, code, start, end, LineNow());
        throw ParseFail{};
    }
    [[noreturn]] void RaiseLex(const Token &tok) {
        Record(tok.err, "syntax-error", tok.start, tok.end, tok.err_line);
        throw ParseFail{};
    }
    [[noreturn]] void ErrorExpected(int token) { SyntaxError(Token2Str(token) + " expected"); }
    [[noreturn]] void ErrorLimit(int limit, const char *what) {
        const std::string where =
            fs_->linedefined == 0 ? "main function" : "function at line " + std::to_string(fs_->linedefined);
        SyntaxError("too many " + std::string(what) + " (limit is " + std::to_string(limit) + ") in " + where);
    }
    void EnterLevel() {
        if (++level_ >= kMaxCCalls) {
            Record("C stack overflow", "limit", t().start, t().end, 0);
            throw ParseFail{};
        }
    }
    void LeaveLevel() { level_--; }
    struct LevelGuard {
        Parser *p;
        explicit LevelGuard(Parser *pp) : p(pp) { p->EnterLevel(); }
        ~LevelGuard() { p->LeaveLevel(); }
        LevelGuard(const LevelGuard &) = delete;
        LevelGuard &operator=(const LevelGuard &) = delete;
    };

    // ---- token stream ----
    void NextRaw() {
        if (started_) {
            prev_end_ = t().end;
            if (idx_ + 1 < toks_.size()) idx_++;
        }
        started_ = true;
        la_active_ = false;
    }
    void Next() {
        NextRaw();
        if (t().type == TK_LEXERR) RaiseLex(t());
    }
    int Lookahead() {
        if (Peek().type == TK_LEXERR) RaiseLex(Peek());
        la_active_ = true;
        return Peek().type;
    }
    bool TestNext(int c) {
        if (t().type == c) {
            Next();
            return true;
        }
        return false;
    }
    void Check(int c) {
        if (t().type != c) ErrorExpected(c);
    }
    void CheckNext(int c) {
        Check(c);
        Next();
    }
    void CheckMatch(int what, int who, int where) {
        if (!TestNext(what)) {
            if (where == LineNow()) ErrorExpected(what);
            SyntaxError(Token2Str(what) + " expected (to close " + Token2Str(who) + " at line " +
                        std::to_string(where) + ")");
        }
    }
    std::string StrCheckName(int *start = nullptr, int *end = nullptr) {
        Check(TK_NAME);
        std::string n = t().value;
        if (start) *start = t().start;
        if (end) *end = t().end;
        Next();
        return n;
    }
    bool BlockFollow(bool withuntil) const {
        switch (t().type) {
            case TK_ELSE:
            case TK_ELSEIF:
            case TK_END:
            case TK_EOS:
                return true;
            case TK_UNTIL:
                return withuntil;
            default:
                return false;
        }
    }

    // ---- tree ----
    Node *NewNode(Kind k, int start) {
        r_->node_arena.push_back(std::make_unique<Node>());
        Node *n = r_->node_arena.back().get();
        n->kind = k;
        n->start = start;
        n->end = start;
        n->line = t().line;
        return n;
    }
    Node *Finish(Node *n) {
        n->end = std::max(n->start, prev_end_);
        return n;
    }

    // ---- variables ----
    Var *NewLocalVar(const std::string &name, int start, int end, Var::KindT kind, Node *decl) {
        if (static_cast<int>(actvar_.size()) + 1 - static_cast<int>(fs_->firstlocal) > kMaxVars)
            ErrorLimit(kMaxVars, "local variables");
        r_->var_arena.push_back(std::make_unique<Var>());
        Var *v = r_->var_arena.back().get();
        v->name = name;
        v->start = start;
        v->end = end;
        v->kind = kind;
        v->decl = decl;
        v->owner = fs_->f;
        // An outer variable of the same name visible here.
        Resolved outer = SingleVarAux(fs_, name, true, /*probe=*/true);
        v->shadows = outer.var;
        r_->vars.push_back(v);
        actvar_.push_back({v, name, false, false});
        return v;
    }
    void NewLocalVarLiteral(const char *name) {
        if (static_cast<int>(actvar_.size()) + 1 - static_cast<int>(fs_->firstlocal) > kMaxVars)
            ErrorLimit(kMaxVars, "local variables");
        actvar_.push_back({nullptr, name, false, false});
    }
    void AdjustLocalVars(int nvars) {
        for (int i = 0; i < nvars; i++) {
            VarSlot &s = actvar_[fs_->firstlocal + static_cast<size_t>(fs_->nactvar)];
            if (s.var) s.var->scope_start = t().start;
            fs_->nactvar++;
        }
    }
    void RemoveVars(int tolevel) {
        while (fs_->nactvar > tolevel) {
            fs_->nactvar--;
            VarSlot &s = actvar_[fs_->firstlocal + static_cast<size_t>(fs_->nactvar)];
            if (s.var) s.var->scope_end = t().start;
        }
        actvar_.resize(fs_->firstlocal + static_cast<size_t>(fs_->nactvar));
    }

    // singlevaraux. `probe` resolves without side effects (no upvalue is created).
    Resolved SingleVarAux(FuncState *fs, const std::string &n, bool base, bool probe = false) {
        Resolved res;
        if (!fs) return res;
        for (int i = fs->nactvar - 1; i >= 0; i--) {
            const VarSlot &s = actvar_[fs->firstlocal + static_cast<size_t>(i)];
            if (s.name == n) {
                res.k = s.ctc ? Resolved::Const : Resolved::Local;
                res.var = s.var;
                res.readonly = s.readonly;
                return res;
            }
        }
        for (const Upval &u : fs->upvalues) {
            if (u.name == n) {
                res.k = Resolved::Upval;
                res.var = u.var;
                res.readonly = u.readonly;
                return res;
            }
        }
        res = SingleVarAux(fs->prev, n, false, probe);
        if (res.k == Resolved::Local || res.k == Resolved::Upval) {
            if (!probe) {
                if (static_cast<int>(fs->upvalues.size()) + 1 > kMaxUpval) {
                    FuncState *saved = fs_;
                    fs_ = fs;
                    ErrorLimit(kMaxUpval, "upvalues");
                    fs_ = saved;
                }
                fs->upvalues.push_back({n, res.var, res.readonly});
            }
            res.k = Resolved::Upval;
        }
        (void)base;
        return res;
    }

    Node *SingleVar() {
        Node *n = NewNode(Kind::Name, t().start);
        n->line = t().line;
        int s = 0, e = 0;
        n->text = StrCheckName(&s, &e);
        n->name_start = s;
        n->name_end = e;
        Finish(n);
        Resolved res = SingleVarAux(fs_, n->text, true);
        if (res.k == Resolved::Void) {
            Resolved env = SingleVarAux(fs_, "_ENV", true);
            n->var = nullptr;
            n->custom_env = env.var != nullptr;
            r_->globals.push_back(n);
        } else {
            n->var = res.var;
            if (res.var) {
                res.var->reads.push_back(n);
                if (res.var->owner != fs_->f) res.var->upvalue_uses.push_back(n);
            }
        }
        // Remember constness for check_readonly.
        if (res.readonly) readonly_.push_back(n);
        return n;
    }
    std::vector<Node *> readonly_;
    bool IsReadonly(const Node *n) const {
        for (const Node *x : readonly_) {
            if (x == n) return true;
        }
        return false;
    }
    void MarkWrite(Node *n) {
        if (n->kind != Kind::Name) return;
        n->is_write = true;
        if (n->var) {
            auto &rd = n->var->reads;
            for (size_t i = 0; i < rd.size(); i++) {
                if (rd[i] == n) {
                    rd.erase(rd.begin() + static_cast<long>(i));
                    break;
                }
            }
            n->var->writes.push_back(n);
        }
    }
    void CheckReadonly(Node *v) {
        if (v->kind == Kind::Name && IsReadonly(v))
            SemError("attempt to assign to const variable '" + v->text + "'", "const-assign", v->start, v->end);
    }

    // ---- labels and gotos ----
    void NewLabelEntry(std::vector<LabelDesc> *l, const std::string &name, int line, int start, int end) {
        LabelDesc d;
        d.name = name;
        d.line = line;
        d.nactvar = fs_->nactvar;
        d.start = start;
        d.end = end;
        l->push_back(d);
    }
    LabelDesc *FindLabel(const std::string &name) {
        for (size_t i = fs_->firstlabel; i < labels_.size(); i++) {
            if (labels_[i].name == name) return &labels_[i];
        }
        return nullptr;
    }
    void SolveGotos(const LabelDesc &lb) {
        size_t i = fs_->bl->firstgoto;
        while (i < gotos_.size()) {
            if (gotos_[i].name == lb.name) {
                const LabelDesc gt = gotos_[i];
                if (gt.nactvar < lb.nactvar) {
                    const VarSlot &s = actvar_[fs_->firstlocal + static_cast<size_t>(gt.nactvar)];
                    SemError("<goto " + gt.name + "> at line " + std::to_string(gt.line) +
                                 " jumps into the scope of local '" + s.name + "'",
                             "goto", gt.start, gt.end);
                }
                gotos_.erase(gotos_.begin() + static_cast<long>(i));
            } else {
                i++;
            }
        }
    }
    void CreateLabel(const std::string &name, int line, bool last, int start, int end) {
        NewLabelEntry(&labels_, name, line, start, end);
        if (last) labels_.back().nactvar = fs_->bl->nactvar;
        const LabelDesc lb = labels_.back();
        SolveGotos(lb);
    }
    void MoveGotosOut(BlockCnt *bl) {
        for (size_t i = bl->firstgoto; i < gotos_.size(); i++) gotos_[i].nactvar = bl->nactvar;
    }
    void EnterBlock(BlockCnt *bl, bool isloop) {
        bl->isloop = isloop;
        bl->nactvar = fs_->nactvar;
        bl->firstlabel = labels_.size();
        bl->firstgoto = gotos_.size();
        bl->previous = fs_->bl;
        fs_->bl = bl;
    }
    void LeaveBlock() {
        BlockCnt *bl = fs_->bl;
        RemoveVars(bl->nactvar);
        if (bl->isloop) CreateLabel("break", 0, false, 0, 0);
        labels_.resize(bl->firstlabel);
        fs_->bl = bl->previous;
        if (bl->previous) {
            MoveGotosOut(bl);
        } else if (bl->firstgoto < gotos_.size()) {
            const LabelDesc gt = gotos_[bl->firstgoto];
            if (gt.name == "break")
                SemError("break outside loop at line " + std::to_string(gt.line), "goto", gt.start, gt.end);
            SemError("no visible label '" + gt.name + "' for <goto> at line " + std::to_string(gt.line), "goto",
                     gt.start, gt.end);
        }
    }

    void OpenFunc(FuncState *fs, BlockCnt *bl) {
        fs->prev = fs_;
        fs_ = fs;
        fs->firstlocal = actvar_.size();
        fs->firstlabel = labels_.size();
        fs->bl = nullptr;
        EnterBlock(bl, false);
    }
    void CloseFunc() {
        LeaveBlock();
        fs_ = fs_->prev;
    }

    // ---- statements ----
    void Statlist(Block *blk) {
        if (blk->start == 0 && blk->stats.empty()) blk->start = t().start;
        while (!BlockFollow(true)) {
            if (t().type == TK_RETURN) {
                Guarded(blk);
                blk->end = prev_end_;
                return;
            }
            Guarded(blk);
        }
        blk->end = std::max(blk->start, prev_end_);
    }

    // statement(), with the recovery: a failed statement is replaced by an Error node over what was skipped.
    void Guarded(Block *blk) {
        FuncState *fs = fs_;
        BlockCnt *bl = fs->bl;
        const int nact = fs->nactvar;
        const size_t nvars = actvar_.size(), nlabels = labels_.size(), ngotos = gotos_.size();
        const size_t nup = fs->upvalues.size();
        const int lvl = level_;
        const size_t at = idx_;
        const int start = t().start;
        try {
            Node *s = Statement();
            if (s) blk->stats.push_back(s);
            for (Node *x : pending_after_label_) blk->stats.push_back(x);
            pending_after_label_.clear();
            return;
        } catch (const ParseFail &) {
        }
        pending_after_label_.clear();
        // Back to the state before the statement.
        fs_ = fs;
        fs->bl = bl;
        while (fs->nactvar > nact) {
            fs->nactvar--;
            VarSlot &sl = actvar_[fs->firstlocal + static_cast<size_t>(fs->nactvar)];
            if (sl.var) sl.var->scope_end = t().start;
        }
        fs->nactvar = nact;
        actvar_.resize(nvars);
        labels_.resize(std::min(labels_.size(), nlabels));
        gotos_.resize(std::min(gotos_.size(), ngotos));
        fs->upvalues.resize(std::min(fs->upvalues.size(), nup));
        level_ = lvl;
        la_active_ = false;
        // Skip to something that can start a statement or end the block.
        if (idx_ == at) NextRaw();
        while (!IsSync()) NextRaw();
        Node *e = NewNode(Kind::Error, start);
        e->end = std::max(start, prev_end_);
        blk->stats.push_back(e);
    }
    bool IsSync() const {
        switch (t().type) {
            case TK_EOS:
            case TK_END:
            case TK_ELSE:
            case TK_ELSEIF:
            case TK_UNTIL:
            case TK_LOCAL:
            case TK_FUNCTION:
            case TK_IF:
            case TK_FOR:
            case TK_WHILE:
            case TK_REPEAT:
            case TK_RETURN:
            case TK_DO:
            case TK_GOTO:
            case TK_BREAK:
            case TK_DBCOLON:
            case ';':
                return true;
            default:
                return false;
        }
    }

    void Block_(Block *blk) {
        BlockCnt bl;
        EnterBlock(&bl, false);
        Statlist(blk);
        LeaveBlock();
    }

    Node *Statement() {
        const int line = LineNow();
        LevelGuard guard(this);
        Node *s = nullptr;
        const int start = t().start;
        switch (t().type) {
            case ';':
                s = NewNode(Kind::Empty, start);
                Next();
                break;
            case TK_IF:
                s = IfStat(line);
                break;
            case TK_WHILE:
                s = WhileStat(line);
                break;
            case TK_DO:
                s = NewNode(Kind::Do, start);
                Next();
                Block_(&s->block);
                CheckMatch(TK_END, TK_DO, line);
                break;
            case TK_FOR:
                s = ForStat(line);
                break;
            case TK_REPEAT:
                s = RepeatStat(line);
                break;
            case TK_FUNCTION:
                s = FuncStat(line);
                break;
            case TK_LOCAL:
                Next();
                if (TestNext(TK_FUNCTION)) {
                    s = LocalFunc(start);
                } else {
                    s = LocalStat(start);
                }
                break;
            case TK_DBCOLON: {
                Next();
                s = NewNode(Kind::Label, start);
                int ns = 0, ne = 0;
                s->text = StrCheckName(&ns, &ne);
                s->name_start = ns;
                s->name_end = ne;
                LabelStat(s, line);
                break;
            }
            case TK_RETURN:
                Next();
                s = RetStat(start);
                break;
            case TK_BREAK:
                s = NewNode(Kind::Break, start);
                BreakStat(start);
                break;
            case TK_GOTO:
                Next();
                s = NewNode(Kind::Goto, start);
                GotoStat(s);
                break;
            default:
                s = ExprStat();
                break;
        }
        return s ? Finish(s) : nullptr;
    }

    Node *IfStat(int line) {
        Node *s = NewNode(Kind::If, t().start);
        TestThenBlock(s);
        while (t().type == TK_ELSEIF) TestThenBlock(s);
        if (t().type == TK_ELSE) {
            IfClause c;
            c.kw = t().start;
            Next();
            s->clauses.push_back(c);
            Block_(&s->clauses.back().body);
        }
        CheckMatch(TK_END, TK_IF, line);
        return s;
    }
    void TestThenBlock(Node *s) {
        IfClause c;
        c.kw = t().start;
        Next();
        c.cond = Expr();
        CheckNext(TK_THEN);
        s->clauses.push_back(c);
        Block *body = &s->clauses.back().body;
        BlockCnt bl;
        if (t().type == TK_BREAK) {
            const int line = LineNow();
            const int bstart = t().start, bend = t().end;
            Node *br = NewNode(Kind::Break, bstart);
            Next();
            br->end = bend;
            body->start = bstart;
            body->stats.push_back(br);
            EnterBlock(&bl, false);  // before the goto, as lparser.c does
            NewLabelEntry(&gotos_, "break", line, bstart, bend);
            while (t().type == ';') {
                Node *e = NewNode(Kind::Empty, t().start);
                Next();
                body->stats.push_back(Finish(e));
            }
            if (BlockFollow(false)) {
                body->end = prev_end_;
                LeaveBlock();
                return;
            }
        } else {
            EnterBlock(&bl, false);
        }
        Statlist(body);
        LeaveBlock();
    }

    Node *WhileStat(int line) {
        Node *s = NewNode(Kind::While, t().start);
        Next();
        s->b = Expr();
        BlockCnt bl;
        EnterBlock(&bl, true);
        CheckNext(TK_DO);
        Block_(&s->block);
        CheckMatch(TK_END, TK_WHILE, line);
        LeaveBlock();
        return s;
    }

    Node *RepeatStat(int line) {
        Node *s = NewNode(Kind::Repeat, t().start);
        BlockCnt bl1, bl2;
        EnterBlock(&bl1, true);
        EnterBlock(&bl2, false);
        Next();
        Statlist(&s->block);
        CheckMatch(TK_UNTIL, TK_REPEAT, line);
        s->b = Expr();
        LeaveBlock();
        LeaveBlock();
        return s;
    }

    Node *ForStat(int line) {
        BlockCnt bl;
        EnterBlock(&bl, true);
        const int start = t().start;
        Next();
        int ns = 0, ne = 0;
        const std::string varname = StrCheckName(&ns, &ne);
        Node *s = nullptr;
        switch (t().type) {
            case '=':
                s = NewNode(Kind::NumFor, start);
                ForNum(s, varname, ns, ne, line);
                break;
            case ',':
            case TK_IN:
                s = NewNode(Kind::GenFor, start);
                ForList(s, varname, ns, ne);
                break;
            default:
                SyntaxError("'=' or 'in' expected");
        }
        CheckMatch(TK_END, TK_FOR, line);
        LeaveBlock();
        return s;
    }
    void ForBody(Node *s, int nvars) {
        CheckNext(TK_DO);
        BlockCnt bl;
        EnterBlock(&bl, false);
        AdjustLocalVars(nvars);
        Block_(&s->block);
        LeaveBlock();
    }
    void ForNum(Node *s, const std::string &varname, int ns, int ne, int line) {
        (void)line;
        NewLocalVarLiteral("(for state)");
        NewLocalVarLiteral("(for state)");
        NewLocalVarLiteral("(for state)");
        s->vars.push_back(NewLocalVar(varname, ns, ne, Var::ForVar, s));
        CheckNext('=');
        s->a = Expr();
        CheckNext(',');
        s->b = Expr();
        if (TestNext(',')) s->c = Expr();
        AdjustLocalVars(3);
        ForBody(s, 1);
    }
    void ForList(Node *s, const std::string &indexname, int ns, int ne) {
        int nvars = 5;
        NewLocalVarLiteral("(for state)");
        NewLocalVarLiteral("(for state)");
        NewLocalVarLiteral("(for state)");
        NewLocalVarLiteral("(for state)");
        s->vars.push_back(NewLocalVar(indexname, ns, ne, Var::ForVar, s));
        while (TestNext(',')) {
            int a = 0, b = 0;
            const std::string n = StrCheckName(&a, &b);
            s->vars.push_back(NewLocalVar(n, a, b, Var::ForVar, s));
            nvars++;
        }
        CheckNext(TK_IN);
        ExpList(&s->list);
        AdjustLocalVars(4);
        ForBody(s, nvars - 4);
    }

    Node *FuncStat(int line) {
        Node *s = NewNode(Kind::FunctionStat, t().start);
        Next();
        // funcname -> NAME {fieldsel} [':' NAME]
        Node *v = SingleVar();
        s->targets.push_back(v);
        Node *obj = v;
        while (t().type == '.') obj = FieldSel(obj, &s->targets);
        if (t().type == ':') {
            s->method = true;
            obj = FieldSel(obj, &s->targets);
        }
        if (s->targets.size() == 1) MarkWrite(v);
        const Node *last = s->targets.back();
        s->func = Body(s->method, line, s, last->kind == Kind::Field ? last->name_start : last->start,
                       last->kind == Kind::Field ? last->name_end : last->end);
        CheckReadonly(v);
        if (s->targets.size() > 1) {
            // Only a plain name is assignable-checked; fields never are.
        }
        return s;
    }

    Node *FieldSel(Node *obj, std::vector<Node *> *path) {
        Node *f = NewNode(Kind::Field, obj->start);
        f->a = obj;
        Next();  // skip '.' or ':'
        int ns = 0, ne = 0;
        f->text = StrCheckName(&ns, &ne);
        f->name_start = ns;
        f->name_end = ne;
        Finish(f);
        if (path) path->push_back(f);
        return f;
    }

    Node *LocalFunc(int start) {
        Node *s = NewNode(Kind::LocalFunction, start);
        int ns = 0, ne = 0;
        const std::string name = StrCheckName(&ns, &ne);
        Var *v = NewLocalVar(name, ns, ne, Var::LocalFunction, s);
        s->vars.push_back(v);
        AdjustLocalVars(1);
        s->func = Body(false, LineNow(), s, ns, ne);
        return s;
    }

    Var::Attrib GetLocalAttribute() {
        if (TestNext('<')) {
            const int as = t().start;
            const std::string attr = StrCheckName();
            const int ae = t().end;
            CheckNext('>');
            if (attr == "const") return Var::Const;
            if (attr == "close") return Var::Close;
            SemError("unknown attribute '" + attr + "'", "attribute", as, ae);
        }
        return Var::None;
    }

    static bool IsConstExpr(const Node *e) {
        if (!e) return false;
        switch (e->kind) {
            case Kind::Nil:
            case Kind::True:
            case Kind::False:
            case Kind::Number:
            case Kind::String:
                return true;
            default:
                return false;
        }
    }

    Node *LocalStat(int start) {
        Node *s = NewNode(Kind::Local, start);
        int toclose = -1;
        int nvars = 0;
        Var *last = nullptr;
        do {
            int ns = 0, ne = 0;
            const std::string name = StrCheckName(&ns, &ne);
            last = NewLocalVar(name, ns, ne, Var::Local, s);
            last->attrib = GetLocalAttribute();
            actvar_.back().readonly = last->attrib != Var::None;
            s->vars.push_back(last);
            if (last->attrib == Var::Close) {
                if (toclose != -1)
                    SemError("multiple to-be-closed variables in local list", "attribute", ns, ne);
                toclose = fs_->nactvar + nvars;
            }
            nvars++;
        } while (TestNext(','));
        int nexps = 0;
        if (TestNext('=')) nexps = ExpList(&s->list);
        if (nvars == nexps && last->attrib == Var::Const && IsConstExpr(s->list.back())) {
            actvar_.back().ctc = true;
        }
        AdjustLocalVars(nvars);
        return s;
    }

    void LabelStat(Node *s, int line) {
        CheckNext(TK_DBCOLON);
        std::vector<Node *> followers;
        while (t().type == ';' || t().type == TK_DBCOLON) {
            Node *x = Statement();
            std::vector<Node *> xs = std::move(pending_after_label_);
            pending_after_label_.clear();
            if (x) followers.push_back(x);
            followers.insert(followers.end(), xs.begin(), xs.end());
        }
        if (LabelDesc *lb = FindLabel(s->text)) {
            SemError("label '" + s->text + "' already defined on line " + std::to_string(lb->line), "label",
                     s->name_start, s->name_end);
        }
        CreateLabel(s->text, line, BlockFollow(false), s->name_start, s->name_end);
        pending_after_label_ = std::move(followers);
    }
    std::vector<Node *> pending_after_label_;  // labels/`;` read inside labelstat (kept out of the tree)

    void BreakStat(int start) {
        const int line = LineNow();
        const int end = t().end;
        Next();
        NewLabelEntry(&gotos_, "break", line, start, end);
    }

    void GotoStat(Node *s) {
        const int line = LineNow();
        int ns = 0, ne = 0;
        s->text = StrCheckName(&ns, &ne);
        s->name_start = ns;
        s->name_end = ne;
        if (!FindLabel(s->text)) NewLabelEntry(&gotos_, s->text, line, s->start, ne);
    }

    Node *RetStat(int start) {
        Node *s = NewNode(Kind::Return, start);
        if (!(BlockFollow(true) || t().type == ';')) ExpList(&s->list);
        if (TestNext(';')) s->semicolon = true;
        return s;
    }

    static bool IsVar(const Node *v) {
        return v && (v->kind == Kind::Name || v->kind == Kind::Index || v->kind == Kind::Field);
    }

    Node *ExprStat() {
        const int start = t().start;
        Node *v = SuffixedExp();
        if (t().type == '=' || t().type == ',') {
            Node *s = NewNode(Kind::Assign, start);
            s->targets.push_back(v);
            RestAssign(s, v);
            for (Node *x : s->targets) MarkWrite(x);
            return s;
        }
        if (v->kind != Kind::Call) SyntaxError("syntax error");
        Node *s = NewNode(Kind::CallStat, start);
        s->a = v;
        return s;
    }
    void RestAssign(Node *s, Node *lh) {
        if (!IsVar(lh)) SyntaxError("syntax error");
        CheckReadonly(lh);
        if (TestNext(',')) {
            Node *nv = SuffixedExp();
            s->targets.push_back(nv);
            LevelGuard guard(this);
            RestAssign(s, nv);
        } else {
            CheckNext('=');
            ExpList(&s->list);
        }
    }

    // ---- functions ----
    FuncBody *Body(bool ismethod, int line, Node *decl, int self_start, int self_end) {
        r_->func_arena.push_back(std::make_unique<FuncBody>());
        FuncBody *f = r_->func_arena.back().get();
        f->line = line;
        f->is_method = ismethod;
        FuncState nfs;
        BlockCnt bl;
        nfs.f = f;
        nfs.linedefined = line;
        OpenFunc(&nfs, &bl);
        f->open_paren = t().start;
        CheckNext('(');
        if (ismethod) {
            Var *self = NewLocalVar("self", self_start, self_end, Var::Self, decl);
            f->params.push_back(self);
            AdjustLocalVars(1);
        }
        ParList(f, decl);
        f->close_paren = t().start;
        CheckNext(')');
        Statlist(&f->body);
        f->last_line = LineNow();
        f->end_kw = t().start;
        CheckMatch(TK_END, TK_FUNCTION, line);
        CloseFunc();
        return f;
    }
    void ParList(FuncBody *f, Node *decl) {
        int nparams = 0;
        if (t().type != ')') {
            do {
                switch (t().type) {
                    case TK_NAME: {
                        int ns = 0, ne = 0;
                        const std::string n = StrCheckName(&ns, &ne);
                        f->params.push_back(NewLocalVar(n, ns, ne, Var::Param, decl));
                        nparams++;
                        break;
                    }
                    case TK_DOTS:
                        Next();
                        f->vararg = true;
                        break;
                    default:
                        SyntaxError("<name> or '...' expected");
                }
            } while (!f->vararg && TestNext(','));
        }
        AdjustLocalVars(nparams);
        fs_->is_vararg = f->vararg;
    }

    int ExpList(std::vector<Node *> *out) {
        int n = 1;
        out->push_back(Expr());
        while (TestNext(',')) {
            out->push_back(Expr());
            n++;
        }
        return n;
    }

    void FuncArgs(Node *call) {
        const int line = LineNow();
        switch (t().type) {
            case '(': {
                call->args = ArgStyle::Parens;
                call->open = t().start;
                Next();
                if (t().type != ')') ExpList(&call->list);
                call->close = t().start;
                CheckMatch(')', '(', line);
                break;
            }
            case '{': {
                call->args = ArgStyle::Table;
                call->list.push_back(Constructor());
                break;
            }
            case TK_STRING: {
                call->args = ArgStyle::String;
                call->list.push_back(StringNode());
                break;
            }
            default:
                SyntaxError("function arguments expected");
        }
        Finish(call);
    }

    Node *StringNode() {
        Node *n = NewNode(Kind::String, t().start);
        n->text = std::string(src_view_.substr(static_cast<size_t>(t().start),
                                               static_cast<size_t>(t().end - t().start)));
        n->value = t().value;
        Next();
        return Finish(n);
    }

  public:
    std::string_view src_view_;

  private:
    Node *PrimaryExp() {
        switch (t().type) {
            case '(': {
                const int line = LineNow();
                Node *p = NewNode(Kind::Paren, t().start);
                p->open = t().start;
                Next();
                p->a = Expr();
                p->close = t().start;
                CheckMatch(')', '(', line);
                return Finish(p);
            }
            case TK_NAME:
                return SingleVar();
            default:
                SyntaxError("unexpected symbol");
        }
    }

    Node *SuffixedExp() {
        Node *v = PrimaryExp();
        for (;;) {
            switch (t().type) {
                case '.':
                    v = FieldSel(v, nullptr);
                    break;
                case '[': {
                    Node *ix = NewNode(Kind::Index, v->start);
                    ix->a = v;
                    ix->open = t().start;
                    ix->b = YIndex(&ix->close);
                    v = Finish(ix);
                    break;
                }
                case ':': {
                    Node *c = NewNode(Kind::Call, v->start);
                    c->a = v;
                    c->method = true;
                    Next();
                    int ns = 0, ne = 0;
                    c->text = StrCheckName(&ns, &ne);
                    c->name_start = ns;
                    c->name_end = ne;
                    FuncArgs(c);
                    v = c;
                    break;
                }
                case '(':
                case TK_STRING:
                case '{': {
                    Node *c = NewNode(Kind::Call, v->start);
                    c->a = v;
                    FuncArgs(c);
                    v = c;
                    break;
                }
                default:
                    return v;
            }
        }
    }

    Node *YIndex(int *close) {
        Next();  // skip '['
        Node *e = Expr();
        *close = t().start;
        CheckNext(']');
        return e;
    }

    Node *Constructor() {
        const int line = LineNow();
        Node *tb = NewNode(Kind::Table, t().start);
        tb->open = t().start;
        CheckNext('{');
        do {
            if (t().type == '}') break;
            Field(tb);
        } while (TestSep(tb));
        tb->close = t().start;
        CheckMatch('}', '{', line);
        return Finish(tb);
    }
    bool TestSep(Node *tb) {
        if (t().type == ',' || t().type == ';') {
            if (!tb->fields.empty()) tb->fields.back().sep = static_cast<char>(t().type);
            Next();
            return true;
        }
        return false;
    }
    void Field(Node *tb) {
        TableField f;
        f.start = t().start;
        switch (t().type) {
            case TK_NAME:
                if (Lookahead() != '=') {
                    f.kind = TableField::Positional;
                    f.value = Expr();
                } else {
                    f.kind = TableField::Named;
                    f.name = t().value;
                    f.name_start = t().start;
                    f.name_end = t().end;
                    Next();
                    CheckNext('=');
                    f.value = Expr();
                }
                break;
            case '[': {
                f.kind = TableField::Keyed;
                int close = 0;
                f.key = YIndex(&close);
                CheckNext('=');
                f.value = Expr();
                break;
            }
            default:
                f.kind = TableField::Positional;
                f.value = Expr();
                break;
        }
        f.end = prev_end_;
        tb->fields.push_back(f);
    }

    Node *SimpleExp() {
        const int start = t().start;
        Node *n = nullptr;
        switch (t().type) {
            case TK_FLT:
            case TK_INT:
                n = NewNode(Kind::Number, start);
                n->text = std::string(src_view_.substr(static_cast<size_t>(t().start),
                                                       static_cast<size_t>(t().end - t().start)));
                break;
            case TK_STRING:
                return StringNode();
            case TK_NIL:
                n = NewNode(Kind::Nil, start);
                break;
            case TK_TRUE:
                n = NewNode(Kind::True, start);
                break;
            case TK_FALSE:
                n = NewNode(Kind::False, start);
                break;
            case TK_DOTS:
                if (!fs_->is_vararg) SyntaxError("cannot use '...' outside a vararg function");
                n = NewNode(Kind::Vararg, start);
                break;
            case '{':
                return Constructor();
            case TK_FUNCTION: {
                n = NewNode(Kind::Function, start);
                Next();
                n->func = Body(false, LineNow(), n, start, start + 8);
                return Finish(n);
            }
            default:
                return SuffixedExp();
        }
        Next();
        return Finish(n);
    }

    static int UnOpr(int op) {
        switch (op) {
            case TK_NOT:
            case '-':
            case '~':
            case '#':
                return 1;
            default:
                return 0;
        }
    }
    // Left and right priorities (lparser.c's table), 0 for "not a binary operator".
    static void BinPrio(int op, int *left, int *right, const char **text) {
        *left = *right = 0;
        switch (op) {
            case '+': *left = *right = 10; *text = "+"; break;
            case '-': *left = *right = 10; *text = "-"; break;
            case '*': *left = *right = 11; *text = "*"; break;
            case '%': *left = *right = 11; *text = "%"; break;
            case '^': *left = 14; *right = 13; *text = "^"; break;
            case '/': *left = *right = 11; *text = "/"; break;
            case TK_IDIV: *left = *right = 11; *text = "//"; break;
            case '&': *left = *right = 6; *text = "&"; break;
            case '|': *left = *right = 4; *text = "|"; break;
            case '~': *left = *right = 5; *text = "~"; break;
            case TK_SHL: *left = *right = 7; *text = "<<"; break;
            case TK_SHR: *left = *right = 7; *text = ">>"; break;
            case TK_CONCAT: *left = 9; *right = 8; *text = ".."; break;
            case TK_NE: *left = *right = 3; *text = "~="; break;
            case TK_EQ: *left = *right = 3; *text = "=="; break;
            case '<': *left = *right = 3; *text = "<"; break;
            case TK_LE: *left = *right = 3; *text = "<="; break;
            case '>': *left = *right = 3; *text = ">"; break;
            case TK_GE: *left = *right = 3; *text = ">="; break;
            case TK_AND: *left = *right = 2; *text = "and"; break;
            case TK_OR: *left = *right = 1; *text = "or"; break;
            default: break;
        }
    }

    // subexpr; returns the first operator it did not take (via *pending).
    Node *SubExpr(int limit, int *pending) {
        LevelGuard guard(this);
        Node *v;
        const int start = t().start;
        if (UnOpr(t().type)) {
            Node *u = NewNode(Kind::Unary, start);
            const int op = t().type;
            u->text = op == TK_NOT ? "not" : std::string(1, static_cast<char>(op));
            Next();
            int dummy = 0;
            u->a = SubExpr(12, &dummy);
            v = Finish(u);
            // The operator subexpr returned is re-read below from the current token.
        } else {
            v = SimpleExp();
        }
        int left = 0, right = 0;
        const char *text = "";
        BinPrio(t().type, &left, &right, &text);
        while (left > 0 && left > limit) {
            Node *b = NewNode(Kind::Binary, v->start);
            b->text = text;
            b->a = v;
            Next();
            int nextop = 0;
            b->b = SubExpr(right, &nextop);
            v = Finish(b);
            BinPrio(t().type, &left, &right, &text);
        }
        *pending = left;
        return v;
    }
    Node *Expr() {
        int pending = 0;
        return SubExpr(0, &pending);
    }
};

}  // namespace

bool IsReserved(std::string_view s) {
    for (int i = 0; i < kNumReserved; i++) {
        if (s == kTokens[i]) return true;
    }
    return false;
}

std::unique_ptr<ParseResult> Parse(std::string_view src) {
    auto r = std::make_unique<ParseResult>();
    size_t start = 0;
    if (src.size() >= 3 && static_cast<unsigned char>(src[0]) == 0xEF && static_cast<unsigned char>(src[1]) == 0xBB &&
        static_cast<unsigned char>(src[2]) == 0xBF)
        start = 3;
    if (start < src.size() && src[start] == '#') {
        size_t e = start;
        while (e < src.size() && src[e] != '\n') e++;
        r->comments.push_back({static_cast<int>(start), static_cast<int>(e), false, 1});
        r->shebang_end = static_cast<int>(e);
        start = e;
    }
    Lexer lx(src, start, &r->comments);
    std::vector<Token> toks = lx.Run();
    Parser p(std::move(toks), r.get());
    p.src_view_ = src;
    p.MainFunc();
    return r;
}

std::string FormatError(const std::string &chunkname, const Error &e) {
    if (e.line <= 0) return e.message;
    // luaO_chunkid: "@name" fits in LUA_IDSIZE (60) bytes, or "..." and the tail that fits.
    std::string id = chunkname;
    if (id.size() + 1 > 60) id = "..." + id.substr(id.size() - 56);
    return id + ":" + std::to_string(e.line) + ": " + e.message;
}

}  // namespace luaparse
