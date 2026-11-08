// The Nix parser: see nix_parse.h. Lexer rules are libexpr/lexer.l's (Nix
// 2.31), the grammar's tables are tools/nix_lalr.py's (identical to bison's
// for parser.y), and the reduce actions make the checks parser.y's actions
// and ParserState make, at the same moments.
#include "nix_parse.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "nix_parse_tables.h"

namespace nixparse {

namespace {

// ---- positions -------------------------------------------------------------

std::string NixPosText(std::string_view src, const std::string &origin, int offset) {
    int line = 0, col = 0;
    NixLineCol(src, offset, &line, &col);
    return origin + ":" + std::to_string(line) + ":" + std::to_string(col);
}

// ---- the lexer ---------------------------------------------------------------

struct Token {
    int tok = T_END;
    int begin = 0, end = 0;          // Nix's location (yylloc)
    int src_start = 0, src_end = 0;  // the text the token covers (PATH_END / end of file: empty)
    std::string text;                // ID, STR (unescaped), IND_STR, numbers, paths, URI
    bool has_indent = false;         // IND_STR from the main rule (stripIndentation looks at these)
    std::string error, error_code;   // a lexer error Nix raises when this token is read
    int error_offset = 0;
    bool synthetic = false;          // inserted by error recovery
};

bool IsPathChar(unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == '+';
}
bool IsIdStart(unsigned char c) { return std::isalpha(c) || c == '_'; }
bool IsIdChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '\'' || c == '-'; }
bool IsUriChar(unsigned char c) {
    return std::isalnum(c) || std::strchr("%/?:@&=+$,-_.!~*'", c) != nullptr;
}

const char *const kKeywords[] = {"if", "then", "else", "assert", "with", "let", "in", "rec", "inherit", "or"};
const int kKeywordTok[] = {T_IF, T_THEN, T_ELSE, T_ASSERT, T_WITH, T_LET, T_IN_KW, T_REC, T_INHERIT, T_OR_KW};

// Escapes in "..." strings (lexer.l's unescapeStr): \n \r \t, \X = X; a
// raw CR or CR/LF becomes LF.
std::string Unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        char c = s[i++];
        if (c == '\\' && i < s.size()) {
            c = s[i++];
            out += c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
        } else if (c == '\r') {
            out += '\n';
            if (i < s.size() && s[i] == '\n') i++;
        } else {
            out += c;
        }
    }
    return out;
}

class Lexer {
public:
    Lexer(std::string_view src, const Options &opts, std::vector<Comment> *comments)
        : s_(src), opts_(opts), comments_(comments) {}

    std::vector<Token> Run() {
        std::vector<Token> out;
        while (true) {
            Token t;
            if (!Next(&t)) continue;
            out.push_back(std::move(t));
            if (out.back().tok == T_END) break;
        }
        return out;
    }

private:
    enum State : unsigned char { INITIAL, DEFAULT, STRING, IND_STRING, INPATH, INPATH_SLASH, PATH_START };

    std::string_view s_;
    const Options &opts_;
    std::vector<Comment> *comments_;
    size_t p_ = 0;
    std::vector<State> stack_{INITIAL};
    // yylloc and its stash (ParserLocation::stash/unstash).
    int begin_ = 0, end_ = 0, stash_begin_ = 0, stash_end_ = 0;

    State Top() const { return stack_.back(); }
    void Push(State s) { stack_.push_back(s); }
    void Pop() {
        if (stack_.size() > 1) stack_.pop_back();
    }
    void Adjust(size_t len) {  // YY_USER_ACTION: adjustLoc
        stash_begin_ = begin_;
        stash_end_ = end_;
        begin_ = end_;
        end_ += static_cast<int>(len);
    }
    void Unstash() {
        begin_ = stash_begin_;
        end_ = stash_end_;
    }
    unsigned char At(size_t i) const { return i < s_.size() ? static_cast<unsigned char>(s_[i]) : 0; }
    bool AtEnd(size_t i) const { return i >= s_.size(); }

    void Emit(Token *t, int tok, size_t len) {
        Adjust(len);
        t->tok = tok;
        t->begin = begin_;
        t->end = end_;
        t->src_start = static_cast<int>(p_);
        t->src_end = static_cast<int>(p_ + len);
        t->text.assign(s_.substr(p_, len));
        p_ += len;
    }
    void EmitEnd(Token *t) {  // end of input: yylloc is left as it is
        t->tok = T_END;
        t->begin = begin_;
        t->end = end_;
        t->src_start = t->src_end = static_cast<int>(std::min(p_, s_.size()));
    }

    // ---- pattern lengths at p (0 = no match) ----
    size_t PathCharRun(size_t i) const {
        size_t n = 0;
        while (!AtEnd(i + n) && IsPathChar(At(i + n))) n++;
        return n;
    }
    // {PATH_CHAR}*(\/{PATH_CHAR}+)+\/?   (prefix = 0: the leading run is required to be empty)
    size_t MatchPath(size_t i, bool allow_prefix = true) const {
        size_t n = allow_prefix ? PathCharRun(i) : 0;
        int segs = 0;
        while (At(i + n) == '/' && !AtEnd(i + n)) {
            size_t r = PathCharRun(i + n + 1);
            if (r == 0) break;
            n += 1 + r;
            segs++;
        }
        if (segs == 0) return 0;
        if (At(i + n) == '/' && !AtEnd(i + n)) n++;
        return n;
    }
    // {PATH_CHAR}*\/
    size_t MatchPathSeg(size_t i) const {
        size_t n = PathCharRun(i);
        return At(i + n) == '/' && !AtEnd(i + n) ? n + 1 : 0;
    }
    size_t MatchHPath(size_t i) const {  // \~(\/{PATH_CHAR}+)+\/?
        if (At(i) != '~') return 0;
        size_t n = MatchPath(i + 1, false);
        return n ? n + 1 : 0;
    }
    size_t MatchSPath(size_t i) const {  // \<{PATH_CHAR}+(\/{PATH_CHAR}+)*\>
        if (At(i) != '<') return 0;
        size_t n = 1, r = PathCharRun(i + 1);
        if (!r) return 0;
        n += r;
        while (At(i + n) == '/' && !AtEnd(i + n)) {
            size_t r2 = PathCharRun(i + n + 1);
            if (!r2) break;
            n += 1 + r2;
        }
        return At(i + n) == '>' && !AtEnd(i + n) ? n + 1 : 0;
    }
    size_t MatchUri(size_t i) const {
        if (AtEnd(i) || !std::isalpha(At(i))) return 0;
        size_t n = 1;
        while (!AtEnd(i + n) && (std::isalnum(At(i + n)) || At(i + n) == '+' || At(i + n) == '-' || At(i + n) == '.'))
            n++;
        if (At(i + n) != ':' || AtEnd(i + n)) return 0;
        n++;
        size_t r = 0;
        while (!AtEnd(i + n + r) && IsUriChar(At(i + n + r))) r++;
        return r ? n + r : 0;
    }
    size_t MatchId(size_t i) const {
        if (AtEnd(i) || !IsIdStart(At(i))) return 0;
        size_t n = 1;
        while (!AtEnd(i + n) && IsIdChar(At(i + n))) n++;
        return n;
    }
    size_t MatchInt(size_t i) const {
        size_t n = 0;
        while (!AtEnd(i + n) && std::isdigit(At(i + n))) n++;
        return n;
    }
    // (([1-9][0-9]*\.[0-9]*)|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?
    size_t MatchFloat(size_t i) const {
        size_t best = 0;
        auto digits = [&](size_t k) {
            size_t n = 0;
            while (!AtEnd(k + n) && std::isdigit(At(k + n))) n++;
            return n;
        };
        if (!AtEnd(i) && At(i) >= '1' && At(i) <= '9') {
            size_t n = 1 + digits(i + 1);
            if (At(i + n) == '.' && !AtEnd(i + n)) best = n + 1 + digits(i + n + 1);
        }
        {
            size_t n = At(i) == '0' && !AtEnd(i) ? 1 : 0;
            for (size_t z : {n, size_t(0)}) {
                if (At(i + z) == '.' && !AtEnd(i + z)) {
                    size_t d = digits(i + z + 1);
                    if (d) best = std::max(best, z + 1 + d);
                }
            }
        }
        if (!best) return 0;
        if ((At(i + best) == 'e' || At(i + best) == 'E') && !AtEnd(i + best)) {
            size_t k = best + 1;
            if (At(i + k) == '+' || At(i + k) == '-') k++;
            size_t d = digits(i + k);
            if (d) best = k + d;
        }
        return best;
    }
    size_t MatchBlockComment(size_t i) const {  // \/\*([^*]|\*+[^*/])*\*+\/
        if (At(i) != '/' || At(i + 1) != '*' || AtEnd(i + 1)) return 0;
        size_t k = i + 2;
        while (!AtEnd(k)) {
            if (At(k) == '*') {
                size_t j = k;
                while (!AtEnd(j) && At(j) == '*') j++;
                if (AtEnd(j)) return 0;
                if (At(j) == '/') return j + 1 - i;
                k = j + 1;
            } else {
                k++;
            }
        }
        return 0;
    }

    // One step of yylex. Returns true when *t is a token for the parser.
    bool Next(Token *t) {
        const State st = Top();
        if (st == STRING) return LexString(t);
        if (st == IND_STRING) return LexIndString(t);
        if (st == INPATH || st == INPATH_SLASH) return LexInPath(t);
        if (st == PATH_START) {
            Pop();
            Push(INPATH_SLASH);
            if (At(p_) == '~') {
                Emit(t, T_HPATH, 2);
            } else {
                Emit(t, T_PATH, MatchPathSeg(p_));
            }
            return true;
        }
        if (AtEnd(p_)) {
            EmitEnd(t);
            return true;
        }
        // Every DEFAULT rule's match length; the longest wins, the earliest rule on a tie.
        struct Cand {
            size_t len;
            int rule;
            int tok;
        };
        Cand best{0, 1 << 30, 0};
        auto offer = [&](size_t len, int rule, int tok) {
            if (len > best.len || (len == best.len && len > 0 && rule < best.rule)) best = {len, rule, tok};
        };
        const size_t id = MatchId(p_);
        for (size_t k = 0; k < sizeof(kKeywords) / sizeof(*kKeywords); k++) {
            const size_t n = std::strlen(kKeywords[k]);
            if (s_.substr(p_, n) == kKeywords[k]) offer(n, 0, kKeywordTok[k]);
        }
        if (s_.substr(p_, 3) == "...") offer(3, 1, T_ELLIPSIS);
        static const struct {
            const char *op;
            int tok;
        } kOps[] = {{"==", T_EQ}, {"!=", T_NEQ}, {"<=", T_LEQ}, {">=", T_GEQ}, {"&&", T_AND}, {"||", T_OR},
                    {"->", T_IMPL}, {"//", T_UPDATE}, {"++", T_CONCAT}, {"<|", T_PIPE_FROM}, {"|>", T_PIPE_INTO}};
        for (const auto &op : kOps) {
            if (s_.substr(p_, 2) == op.op) offer(2, 2, op.tok);
        }
        offer(id, 3, T_ID);
        offer(MatchInt(p_), 4, T_INT_LIT);
        offer(MatchFloat(p_), 5, T_FLOAT_LIT);
        if (s_.substr(p_, 2) == "${") offer(2, 6, T_DOLLAR_CURLY);
        if (At(p_) == '}') offer(1, 7, T_RBRACE);
        if (At(p_) == '{') offer(1, 8, T_LBRACE);
        if (At(p_) == '"') offer(1, 9, T_QUOTE);
        if (s_.substr(p_, 2) == "''") {
            size_t n = 2;
            while (At(p_ + n) == ' ' && !AtEnd(p_ + n)) n++;
            offer(At(p_ + n) == '\n' && !AtEnd(p_ + n) ? n + 1 : 2, 10, T_IND_STRING_OPEN);
        }
        {
            const size_t seg = MatchPathSeg(p_);
            if (seg && s_.substr(p_ + seg, 2) == "${") offer(seg + 2, 11, -1);
            if (s_.substr(p_, 4) == "~/${") offer(4, 11, -1);
        }
        offer(MatchPath(p_), 12, T_PATH);
        offer(MatchHPath(p_), 13, T_HPATH);
        offer(MatchSPath(p_), 14, T_SPATH);
        offer(MatchUri(p_), 15, T_URI);
        offer(MatchBlockComment(p_), 16, -2);
        {
            size_t n = 0;
            while (!AtEnd(p_ + n) && std::strchr(" \t\r\n", At(p_ + n)) && At(p_ + n) != 0) n++;
            offer(n, 17, -3);
        }
        if (At(p_) == '#') {
            size_t n = 1;
            while (!AtEnd(p_ + n) && At(p_ + n) != '\r' && At(p_ + n) != '\n') n++;
            offer(n, 18, -4);
        }
        offer(1, 20, -5);  // {ANY}

        switch (best.rule) {
            case 6:
                Push(DEFAULT);
                Emit(t, T_DOLLAR_CURLY, 2);
                return true;
            case 7:
                if (Top() != INITIAL) Pop();
                Emit(t, T_RBRACE, 1);
                return true;
            case 8:
                Push(DEFAULT);
                Emit(t, T_LBRACE, 1);
                return true;
            case 9:
                Push(STRING);
                Emit(t, T_QUOTE, 1);
                return true;
            case 10:
                Push(IND_STRING);
                Emit(t, T_IND_STRING_OPEN, best.len);
                return true;
            case 11:  // {PATH_SEG}\$\{ | {HPATH_START}\$\{ -- yyless(0): only the state changes
                Adjust(best.len);
                Unstash();
                Push(PATH_START);
                return false;
            case 12:
            case 13:
                Push(s_[p_ + best.len - 1] == '/' ? INPATH_SLASH : INPATH);
                Emit(t, best.tok, best.len);
                return true;
            case 16:
            case 18: {
                Comment c;
                c.start = static_cast<int>(p_);
                c.end = static_cast<int>(p_ + best.len);
                c.block = best.rule == 16;
                c.doc = c.block && best.len >= 5 && s_[p_ + 2] == '*' && s_[p_ + 3] != '/' && s_[p_ + 3] != '*';
                comments_->push_back(c);
                Adjust(best.len);
                p_ += best.len;
                return false;
            }
            case 17:
                Adjust(best.len);
                p_ += best.len;
                return false;
            case 20: {
                const unsigned char c = At(p_);
                int tok = T_UNDEF;
                switch (c) {
                    case 0: tok = T_END; break;  // a NUL byte reads as the end of input
                    case ':': tok = T_COLON; break;
                    case '@': tok = T_AT; break;
                    case ';': tok = T_SEMI; break;
                    case '!': tok = T_BANG; break;
                    case '.': tok = T_DOT; break;
                    case '(': tok = T_LPAREN; break;
                    case ')': tok = T_RPAREN; break;
                    case '[': tok = T_LBRACK; break;
                    case ']': tok = T_RBRACK; break;
                    case '=': tok = T_ASSIGN; break;
                    case ',': tok = T_COMMA; break;
                    case '<': tok = T_LT; break;
                    case '>': tok = T_GT; break;
                    case '+': tok = T_PLUS; break;
                    case '-': tok = T_MINUS; break;
                    case '*': tok = T_STAR; break;
                    case '/': tok = T_SLASH; break;
                    case '?': tok = T_QUESTION; break;
                    default: break;
                }
                Emit(t, tok, 1);
                if (tok == T_END) p_ = s_.size();  // nothing after it is read
                return true;
            }
            default:
                break;
        }
        Emit(t, best.tok, best.len);
        if (best.tok == T_INT_LIT) {
            // string2Int<int64_t>: digits only, must fit.
            const std::string &d = t->text;
            bool fits = d.size() < 19;
            if (!fits) {
                size_t k = 0;
                while (k + 1 < d.size() && d[k] == '0') k++;
                const std::string v = d.substr(k);
                fits = v.size() < 19 || (v.size() == 19 && v <= "9223372036854775807");
            }
            if (!fits) SetError(t, "invalid integer '" + d + "'", "invalid-integer", t->begin);
        } else if (best.tok == T_FLOAT_LIT) {
            errno = 0;
            (void)std::strtod(t->text.c_str(), nullptr);
            if (errno != 0) SetError(t, "invalid float '" + t->text + "'", "invalid-float", t->begin);
        } else if ((best.tok == T_PIPE_FROM || best.tok == T_PIPE_INTO) && !opts_.pipe_operators) {
            SetError(t,
                     "experimental Nix feature 'pipe-operators' is disabled; add '--extra-experimental-features "
                     "pipe-operators' to enable it",
                     "experimental-feature", t->begin);
        }
        return true;
    }

    static void SetError(Token *t, const std::string &msg, const char *code, int offset) {
        t->error = msg;
        t->error_code = code;
        t->error_offset = offset;
    }

    bool LexString(Token *t) {
        if (AtEnd(p_)) {
            EmitEnd(t);
            return true;
        }
        // ([^\$\"\\]|\$[^\{\"\\]|\\{ANY}|\$\\{ANY})+ -- its pieces are determined by the next characters,
        // so the greedy scan is the longest match.
        size_t n = 0;
        while (!AtEnd(p_ + n)) {
            const unsigned char c = At(p_ + n);
            if (c == '"') break;
            if (c == '\\') {
                if (AtEnd(p_ + n + 1)) break;
                n += 2;
            } else if (c == '$') {
                if (AtEnd(p_ + n + 1)) break;
                const unsigned char d = At(p_ + n + 1);
                if (d == '{' || d == '"') break;
                if (d == '\\') {
                    if (AtEnd(p_ + n + 2)) break;
                    n += 3;
                } else {
                    n += 2;
                }
            } else {
                n++;
            }
        }
        // The trailing-context rule: ...\$/\" takes a final `$` before the closing quote.
        if (At(p_ + n) == '$' && !AtEnd(p_ + n + 1) && At(p_ + n + 1) == '"') n++;
        if (n > 0) {
            Emit(t, T_STR, n);
            t->text = Unescape(t->text);
            return true;
        }
        if (s_.substr(p_, 2) == "${") {
            Push(DEFAULT);
            Emit(t, T_DOLLAR_CURLY, 2);
            return true;
        }
        if (At(p_) == '"') {
            Pop();
            Emit(t, T_QUOTE, 1);
            return true;
        }
        // \$|\\|\$\\ -- only at the end of input; returns EOF.
        size_t len = (At(p_) == '$' && At(p_ + 1) == '\\' && !AtEnd(p_ + 1)) ? 2 : 1;
        Emit(t, T_END, len);
        p_ = s_.size();
        return true;
    }

    bool LexIndString(Token *t) {
        if (AtEnd(p_)) {
            EmitEnd(t);
            return true;
        }
        // ([^\$\']|\$[^\{\']|\'[^\'\$])+
        size_t n = 0;
        while (!AtEnd(p_ + n)) {
            const unsigned char c = At(p_ + n);
            if (c == '$' || c == '\'') {
                if (AtEnd(p_ + n + 1)) break;
                const unsigned char d = At(p_ + n + 1);
                if (c == '$' && (d == '{' || d == '\'')) break;
                if (c == '\'' && (d == '\'' || d == '$')) break;
                n += 2;
            } else {
                n++;
            }
        }
        if (n > 0) {
            Emit(t, T_IND_STR, n);
            t->has_indent = true;
            return true;
        }
        const std::string_view rest = s_.substr(p_);
        if (rest.substr(0, 3) == "''$") {
            Emit(t, T_IND_STR, 3);
            t->text = "$";
            return true;
        }
        if (rest.substr(0, 3) == "'''") {
            Emit(t, T_IND_STR, 3);
            t->text = "''";
            return true;
        }
        if (rest.size() >= 4 && rest.substr(0, 3) == "''\\") {
            Emit(t, T_IND_STR, 4);
            t->text = Unescape(std::string_view(t->text).substr(2));
            return true;
        }
        if (rest.substr(0, 2) == "${") {
            Push(DEFAULT);
            Emit(t, T_DOLLAR_CURLY, 2);
            return true;
        }
        if (rest.substr(0, 2) == "''") {
            Pop();
            Emit(t, T_IND_STRING_CLOSE, 2);
            return true;
        }
        if (rest[0] == '$') {
            Emit(t, T_IND_STR, 1);
            t->text = "$";
            return true;
        }
        Emit(t, T_IND_STR, 1);  // a lone '
        t->text = "'";
        return true;
    }

    bool LexInPath(Token *t) {
        const bool slash = Top() == INPATH_SLASH;
        if (!AtEnd(p_) && s_.substr(p_, 2) == "${") {
            Pop();
            Push(INPATH);
            Push(DEFAULT);
            Emit(t, T_DOLLAR_CURLY, 2);
            return true;
        }
        if (!AtEnd(p_)) {
            // {PATH}|{PATH_SEG}|{PATH_CHAR}+
            const size_t n = std::max({MatchPath(p_), MatchPathSeg(p_), PathCharRun(p_)});
            if (n > 0) {
                Pop();
                Push(s_[p_ + n - 1] == '/' ? INPATH_SLASH : INPATH);
                Emit(t, T_STR, n);
                return true;
            }
        }
        // Anything else ends the path.
        if (!AtEnd(p_)) Adjust(1);
        if (slash) {
            // "path has a trailing slash", raised at CUR_POS; carry on as if the path had ended.
            const int at = begin_;
            Unstash();
            Pop();
            t->tok = T_PATH_END;
            t->begin = begin_;
            t->end = end_;
            t->src_start = t->src_end = static_cast<int>(p_);
            SetError(t, "path has a trailing slash", "trailing-slash", at);
            return true;
        }
        Unstash();
        Pop();
        t->tok = T_PATH_END;
        t->begin = begin_;
        t->end = end_;
        t->src_start = t->src_end = static_cast<int>(p_);
        return true;
    }
};

// ---- the parser --------------------------------------------------------------

struct StrPart {
    int pos = 0;
    Node *e = nullptr;  // an interpolation; otherwise a literal piece
    std::string text;
    bool has_indent = false;
};

struct Value {
    Node *e = nullptr;
    std::vector<AttrName> names;
    std::vector<StrPart> parts;
    std::vector<Formal> formals;
    bool ellipsis = false;
    std::string text;
    bool has_indent = false;
    int tok_start = 0, tok_end = 0;
    bool missing = false;
};

struct Loc {
    int begin = 0, end = 0;
};

bool g_tables_ready = false;
bool g_has_explicit[kNumStates];

void InitTables() {
    if (g_tables_ready) return;
    for (int s = 0; s < kNumStates; s++) {
        g_has_explicit[s] = false;
        for (int t = 0; t < kNumTokens; t++) {
            if (kAction[s][t] != 0) g_has_explicit[s] = true;
        }
    }
    g_tables_ready = true;
}

// The parser-only run of the tables over a token sequence, for error
// recovery: how many of `toks` are shifted (all of them when it accepts).
int Simulate(std::vector<int> stack, const std::vector<int> &toks) {
    size_t i = 0;
    for (int guard = 0; guard < 100000; guard++) {
        const int s = stack.back();
        if (s == kFinalState) return static_cast<int>(toks.size());
        int act;
        if (!g_has_explicit[s]) {
            if (kDefaultReduce[s] < 0) return static_cast<int>(i);
            act = -kDefaultReduce[s] - 1;
        } else {
            if (i >= toks.size()) return static_cast<int>(i);
            act = kAction[s][toks[i]];
            if (act == 0) {
                if (kDefaultReduce[s] < 0) return static_cast<int>(i);
                act = -kDefaultReduce[s] - 1;
            }
        }
        if (act == kExplicitError) return static_cast<int>(i);
        if (act > 0) {
            stack.push_back(act - 1);
            i++;
            continue;
        }
        const int rule = -act - 1;
        stack.resize(stack.size() - static_cast<size_t>(kRuleLen[rule]));
        const int g = kGoto[stack.back()][kRuleLhs[rule]];
        if (g < 0) return static_cast<int>(i);
        stack.push_back(g);
    }
    return static_cast<int>(i);
}

class Parser {
public:
    Parser(std::string_view src, const Options &opts, ParseResult *out) : src_(src), opts_(opts), out_(out) {}

    void Run() {
        InitTables();
        Lexer lexer(src_, opts_, &out_->comments);
        toks_ = lexer.Run();
        Prepredefined();
        Drive();
    }

private:
    std::string_view src_;
    const Options &opts_;
    ParseResult *out_;
    std::vector<Token> toks_;
    size_t next_ = 0;                 // the next token of toks_ to read
    std::deque<Token> inserted_;      // tokens error recovery inserted, read before toks_[next_]
    std::vector<int> states_;
    std::vector<Value> values_;
    std::vector<Loc> locs_;
    std::unordered_map<std::string, int> symbol_rank_;  // stands in for Nix's symbol numbers
    int repairs_ = 0;

    // ---- nodes ----
    Node *New(NodeKind k, int start, int end) {
        out_->arena.push_back(std::make_unique<Node>());
        Node *n = out_->arena.back().get();
        n->kind = k;
        n->start = start;
        n->end = end;
        return n;
    }

    // ---- errors ----
    void Error(int nix_offset, int start, int end, const std::string &code, const std::string &message,
               bool syntax = false) {
        Diagnostic d;
        d.nix_offset = nix_offset;
        d.start = start;
        d.end = std::max(start, end);
        d.code = code;
        d.message = message;
        d.syntax = syntax;
        out_->errors.push_back(std::move(d));
    }

    // Symbols Nix creates before parsing (builtins, the AST's own) compare
    // lower than any the file introduces; among the file's, first use wins.
    void Prepredefined() {
        static const char *const kPre[] = {
            "abort", "add", "all", "any", "attrNames", "attrValues", "baseNameOf", "bitAnd", "bitOr", "bitXor",
            "builtins", "catAttrs", "ceil", "compareVersions", "concatLists", "concatMap", "concatStringsSep",
            "currentSystem", "currentTime", "deepSeq", "derivation", "dirOf", "div", "elem", "elemAt", "false",
            "fetchGit", "fetchTarball", "fetchurl", "filter", "floor", "foldl'", "fromJSON", "fromTOML",
            "functionArgs", "genList", "getAttr", "getEnv", "groupBy", "hasAttr", "hashString", "head", "import",
            "isAttrs", "isBool", "isFloat", "isFunction", "isInt", "isList", "isNull", "isPath", "isString",
            "length", "lessThan", "listToAttrs", "map", "mapAttrs", "match", "mul", "null", "outPath", "path",
            "pathExists", "placeholder", "readFile", "removeAttrs", "replaceStrings", "seq", "sort", "split",
            "stringLength", "sub", "substring", "tail", "throw", "toFile", "toJSON", "toString", "trace", "true",
            "tryEval", "typeOf", "name", "value", "type", "body", "or", "args", "meta", "system", "version",
            "drvPath", "outputs", "out", "file", "line", "column"};
        int k = 0;
        for (const char *s : kPre) symbol_rank_.emplace(s, k++);
        for (const Token &t : toks_) {
            if (t.tok == T_ID) symbol_rank_.emplace(t.text, 1000000 + t.begin);
        }
    }
    int SymbolRank(const std::string &s) const {
        const auto it = symbol_rank_.find(s);
        return it == symbol_rank_.end() ? 2000000000 : it->second;
    }

    // ---- attribute sets (ParserState::addAttr) ----
    static int FindAttr(Node *attrs, const std::string &name) {
        for (size_t k = 0; k < attrs->attrs.size(); k++) {
            if (attrs->attrs[k].name == name) return static_cast<int>(k);
        }
        return -1;
    }
    // Nix's dynamic_cast<ExprAttrs *>: a set written with braces or made by a
    // path -- not `let { }`, which Nix turns into a selection of `body`.
    static bool IsAttrs(const Node *n) { return n && n->kind == NodeKind::Attrs && !n->old_let; }

    std::string ShowPath(const std::vector<std::string> &path) const {
        std::string s;
        for (size_t k = 0; k < path.size(); k++) {
            if (k) s += '.';
            s += path[k];
        }
        return s;
    }
    void DupAttr(const std::vector<std::string> &path, int pos, int prev_pos, int start, int end) {
        Error(pos, start, end, "duplicate-attribute",
              "attribute '" + ShowPath(path) + "' already defined at " + NixPosText(src_, opts_.origin, prev_pos));
    }

    void AddAttr(Node *attrs, const std::vector<AttrName> &path, int pos, Node *e) {
        Node *cur = attrs;
        std::vector<std::string> shown;
        for (size_t i = 0; i + 1 < path.size(); i++) {
            const AttrName &a = path[i];
            Node *nested = nullptr;
            if (!a.dynamic) {
                shown.push_back(a.name);
                const int j = FindAttr(cur, a.name);
                if (j >= 0) {
                    nested = IsAttrs(cur->attrs[static_cast<size_t>(j)].value) ? cur->attrs[static_cast<size_t>(j)].value
                                                                              : nullptr;
                    if (!nested) {
                        DupAttr(shown, pos, cur->attrs[static_cast<size_t>(j)].pos, pos, a.end);
                        return;
                    }
                } else {
                    nested = New(NodeKind::Attrs, pos, path.back().end);
                    nested->implicit = true;
                    AttrDef d;
                    d.name = a.name;
                    d.value = nested;
                    d.pos = pos;
                    d.name_start = a.start;
                    d.name_end = a.end;
                    cur->attrs.push_back(d);
                }
            } else {
                shown.push_back("\"${...}\"");
                nested = New(NodeKind::Attrs, pos, path.back().end);
                nested->implicit = true;
                cur->dynamic_attrs.push_back({a.expr, nested, pos});
            }
            cur = nested;
        }
        const AttrName &last = path.back();
        if (!last.dynamic) {
            shown.push_back(last.name);
            AttrDef d;
            d.name = last.name;
            d.value = e;
            d.pos = pos;
            d.name_start = last.start;
            d.name_end = last.end;
            AddAttrDef(cur, shown, d, pos, last.end);
        } else {
            cur->dynamic_attrs.push_back({last.expr, e, pos});
        }
    }

    void AddAttrDef(Node *attrs, std::vector<std::string> &shown, const AttrDef &def, int err_start, int err_end) {
        const int j = FindAttr(attrs, def.name);
        if (j < 0) {
            attrs->attrs.push_back(def);
            return;
        }
        Node *ae = IsAttrs(def.value) ? def.value : nullptr;
        Node *jattrs = IsAttrs(attrs->attrs[static_cast<size_t>(j)].value) ? attrs->attrs[static_cast<size_t>(j)].value
                                                                          : nullptr;
        if (ae && jattrs && ae != jattrs) {
            std::vector<AttrDef> moved = std::move(ae->attrs);
            ae->attrs.clear();
            const int shift = static_cast<int>(jattrs->inherit_from.size());
            for (AttrDef &ad : moved) {
                if (ad.kind == AttrDef::InheritedFrom) ad.from += shift;
                shown.push_back(ad.name);
                AddAttrDef(jattrs, shown, ad, ad.name_start, ad.name_end);
                shown.pop_back();
            }
            for (DynamicAttrDef &d : ae->dynamic_attrs) jattrs->dynamic_attrs.push_back(d);
            ae->dynamic_attrs.clear();
            for (Node *f : ae->inherit_from) jattrs->inherit_from.push_back(f);
            ae->inherit_from.clear();
            return;
        }
        DupAttr(shown, def.pos, attrs->attrs[static_cast<size_t>(j)].pos, err_start, err_end);
    }

    void InheritInto(Node *attrs, const std::vector<AttrName> &names, AttrDef::Kind kind, int from) {
        for (const AttrName &n : names) {
            const int j = FindAttr(attrs, n.name);
            if (j >= 0) {
                Error(n.start, n.start, n.end, "duplicate-attribute",
                      "attribute '" + n.name + "' already defined at " +
                          NixPosText(src_, opts_.origin, attrs->attrs[static_cast<size_t>(j)].pos));
                continue;  // std::map::emplace keeps the first
            }
            AttrDef d;
            d.name = n.name;
            d.kind = kind;
            d.pos = n.start;
            d.name_start = n.start;
            d.name_end = n.end;
            d.from = from;
            if (kind == AttrDef::Inherited) {
                Node *v = New(NodeKind::Var, n.start, n.end);
                v->name = n.name;
                v->missing = n.missing;
                d.value = v;
            }
            attrs->attrs.push_back(d);
        }
    }

    // ParserState::validateFormals
    void ValidateFormals(const std::vector<Formal> &formals, int pos, const std::string &arg, int arg_start,
                         int arg_end) {
        std::vector<const Formal *> sorted;
        for (const Formal &f : formals) sorted.push_back(&f);
        std::stable_sort(sorted.begin(), sorted.end(), [&](const Formal *a, const Formal *b) {
            const int ra = SymbolRank(a->name), rb = SymbolRank(b->name);
            if (ra != rb) return ra < rb;
            return a->start < b->start;
        });
        const Formal *dup = nullptr;
        for (size_t i = 0; i + 1 < sorted.size(); i++) {
            if (sorted[i]->name != sorted[i + 1]->name || sorted[i]->name.empty()) continue;
            const Formal *cand = sorted[i + 1];
            if (!dup || SymbolRank(cand->name) < SymbolRank(dup->name) ||
                (SymbolRank(cand->name) == SymbolRank(dup->name) && cand->start < dup->start))
                dup = cand;
        }
        if (dup) {
            Error(dup->start, dup->start, dup->end, "duplicate-formal",
                  "duplicate formal function argument '" + dup->name + "'");
            return;
        }
        if (!arg.empty()) {
            for (const Formal &f : formals) {
                if (f.name == arg) {
                    Error(pos, arg_start, arg_end, "duplicate-formal",
                          "duplicate formal function argument '" + arg + "'");
                    return;
                }
            }
        }
    }

    void WarnIfCursedOr(Node *e) {
        if (!e || e->kind != NodeKind::Call || e->cursed_or_end < 0) return;
        Diagnostic d;
        d.start = d.nix_offset = e->start;
        d.end = e->cursed_or_end;
        d.code = "cursed-or";
        d.message = "This expression uses `or` as an identifier in a way that will change in a future Nix "
                    "release. Wrap this entire expression in parentheses to preserve its current meaning: (" +
                    std::string(src_.substr(static_cast<size_t>(e->start),
                                            static_cast<size_t>(e->cursed_or_end - e->start))) +
                    ")";
        out_->warnings.push_back(std::move(d));
    }

    // ---- strings ----
    Node *StringNode(NodeKind kind, std::vector<StrPart> &&parts, int start, int end) {
        Node *n = New(kind, start, end);
        bool all_static = true;
        std::string value;
        for (StrPart &p : parts) {
            if (p.e) {
                all_static = false;
                n->kids.push_back(p.e);
            } else {
                value += p.text;
            }
        }
        n->str_static = all_static;
        if (all_static) n->str_value = value;
        return n;
    }

    // ParserState::stripIndentation: only whether the result is one literal, and its value.
    Node *IndStringNode(std::vector<StrPart> &&es, int start, int end) {
        Node *n = New(NodeKind::IndString, start, end);
        for (const StrPart &p : es) {
            if (p.e) n->kids.push_back(p.e);
        }
        if (es.empty()) {
            n->str_static = true;
            return n;
        }
        bool at_start = true;
        size_t min_indent = 1000000, cur_indent = 0;
        for (const StrPart &p : es) {
            if (p.e || !p.has_indent) {
                if (at_start) {
                    at_start = false;
                    min_indent = std::min(min_indent, cur_indent);
                }
                continue;
            }
            for (char c : p.text) {
                if (at_start) {
                    if (c == ' ') {
                        cur_indent++;
                    } else if (c == '\n') {
                        cur_indent = 0;
                    } else {
                        at_start = false;
                        min_indent = std::min(min_indent, cur_indent);
                    }
                } else if (c == '\n') {
                    at_start = true;
                    cur_indent = 0;
                }
            }
        }
        std::vector<std::pair<bool, std::string>> es2;  // (is_expr, text)
        at_start = true;
        size_t cur_dropped = 0, left = es.size();
        for (const StrPart &p : es) {
            if (p.e) {
                at_start = false;
                cur_dropped = 0;
                es2.emplace_back(true, std::string());
                left--;
                continue;
            }
            std::string s2;
            for (char c : p.text) {
                if (at_start) {
                    if (c == ' ') {
                        if (cur_dropped++ >= min_indent) s2 += c;
                    } else if (c == '\n') {
                        cur_dropped = 0;
                        s2 += c;
                    } else {
                        at_start = false;
                        cur_dropped = 0;
                        s2 += c;
                    }
                } else {
                    s2 += c;
                    if (c == '\n') at_start = true;
                }
            }
            if (left == 1) {
                const size_t q = s2.find_last_of('\n');
                if (q != std::string::npos && s2.find_first_not_of(' ', q + 1) == std::string::npos) s2.resize(q + 1);
            }
            if (!s2.empty()) es2.emplace_back(false, std::move(s2));
            left--;
        }
        if (es2.empty()) {
            n->str_static = true;
        } else if (es2.size() == 1 && !es2[0].first) {
            n->str_static = true;
            n->str_value = es2[0].second;
        }
        return n;
    }

    static bool StaticString(const Node *e, std::string *value) {
        if (!e) return false;
        if ((e->kind == NodeKind::String || e->kind == NodeKind::IndString) && e->str_static) {
            *value = e->str_value;
            return true;
        }
        if (e->kind == NodeKind::Uri) {
            *value = e->text;
            return true;
        }
        return false;
    }

    AttrName NameFromStringAttr(Value &v, const Loc &loc) {
        AttrName a;
        a.start = loc.begin;
        a.end = loc.end;
        std::string s;
        if (StaticString(v.e, &s)) {
            a.name = s;
        } else {
            a.dynamic = true;
            a.expr = v.e;
        }
        return a;
    }

    // ---- reductions ----
    Value Reduce(int rule, Value *v, const Loc *l, const Loc &here) {
        Value r;
        const int cur = here.begin;
        auto op = [&](const char *text) {
            Node *n = New(NodeKind::BinOp, here.begin, here.end);
            n->text = text;
            n->kids = {v[0].e, v[2].e};
            r.e = n;
        };
        switch (rule) {
            case R_Start:
                out_->root = v[0].e;
                r.e = v[0].e;
                break;
            case R_Expr:
            case R_FunctionIf:
            case R_IfPipeFrom:
            case R_IfPipeInto:
            case R_IfOp:
            case R_OpApp:
            case R_Binds:
                r = std::move(v[0]);
                break;
            case R_LambdaId: {
                Node *n = New(NodeKind::Lambda, here.begin, here.end);
                n->arg = v[0].text;
                n->arg_start = v[0].tok_start;
                n->arg_end = v[0].tok_end;
                n->body = v[2].e;
                r.e = n;
                break;
            }
            case R_LambdaFormals:
            case R_LambdaFormalsAt:
            case R_LambdaAtFormals: {
                Node *n = New(NodeKind::Lambda, here.begin, here.end);
                Value &fs = rule == R_LambdaAtFormals ? v[2] : v[0];
                const Loc &fl = rule == R_LambdaAtFormals ? l[2] : l[0];
                n->has_formals = true;
                n->formals = fs.formals;
                n->ellipsis = fs.ellipsis;
                n->formals_start = fl.begin;
                n->formals_end = fl.end;
                if (rule != R_LambdaFormals) {
                    Value &id = rule == R_LambdaAtFormals ? v[0] : v[2];
                    n->arg = id.text;
                    n->arg_start = id.tok_start;
                    n->arg_end = id.tok_end;
                }
                n->body = rule == R_LambdaFormals ? v[2].e : v[4].e;
                ValidateFormals(n->formals, cur, n->arg, n->arg_start, n->arg_end);
                r.e = n;
                break;
            }
            case R_Assert:
            case R_With: {
                Node *n = New(rule == R_Assert ? NodeKind::Assert : NodeKind::With, here.begin, here.end);
                n->kids = {v[1].e};
                n->body = v[3].e;
                r.e = n;
                break;
            }
            case R_Let: {
                Node *b = v[1].e;
                if (!b->dynamic_attrs.empty())
                    Error(cur, l[0].begin, l[0].end, "dynamic-let", "dynamic attributes not allowed in let");
                Node *n = New(NodeKind::Let, here.begin, here.end);
                n->recursive = true;
                n->attrs = std::move(b->attrs);
                n->dynamic_attrs = std::move(b->dynamic_attrs);
                n->inherit_from = std::move(b->inherit_from);
                n->bindings = std::move(b->bindings);
                n->open = l[0].begin;
                n->close = l[2].begin;
                n->body = v[3].e;
                r.e = n;
                break;
            }
            case R_If: {
                Node *n = New(NodeKind::If, here.begin, here.end);
                n->kids = {v[1].e, v[3].e, v[5].e};
                r.e = n;
                break;
            }
            case R_PipeFromChain:
            case R_PipeFrom:
            case R_PipeIntoChain:
            case R_PipeInto: {
                Node *n = New(NodeKind::Pipe, here.begin, here.end);
                n->text = (rule == R_PipeFromChain || rule == R_PipeFrom) ? "<|" : "|>";
                n->kids = {v[0].e, v[2].e};
                r.e = n;
                break;
            }
            case R_Not:
            case R_Negate: {
                Node *n = New(rule == R_Not ? NodeKind::Not : NodeKind::Negate, here.begin, here.end);
                n->kids = {v[1].e};
                r.e = n;
                break;
            }
            case R_OpEq: op("=="); break;
            case R_OpNeq: op("!="); break;
            case R_OpLt: op("<"); break;
            case R_OpLeq: op("<="); break;
            case R_OpGt: op(">"); break;
            case R_OpGeq: op(">="); break;
            case R_OpAnd: op("&&"); break;
            case R_OpOr: op("||"); break;
            case R_OpImpl: op("->"); break;
            case R_OpUpdate: op("//"); break;
            case R_OpAdd: op("+"); break;
            case R_OpSub: op("-"); break;
            case R_OpMul: op("*"); break;
            case R_OpDiv: op("/"); break;
            case R_OpConcat: op("++"); break;
            case R_OpHasAttr: {
                Node *n = New(NodeKind::HasAttr, here.begin, here.end);
                n->e = v[0].e;
                n->path = std::move(v[2].names);
                r.e = n;
                break;
            }
            case R_App: {
                // makeCall: an application chain is one call with several arguments.
                Node *f = v[0].e;
                if (f->kind == NodeKind::Call) {
                    f->kids.push_back(v[1].e);
                    f->end = here.end;
                    r.e = f;
                } else {
                    Node *n = New(NodeKind::Call, here.begin, here.end);
                    n->kids = {f, v[1].e};
                    r.e = n;
                }
                WarnIfCursedOr(v[1].e);
                break;
            }
            case R_AppSelect:
                r = std::move(v[0]);
                if (r.e && r.e->kind == NodeKind::Call) r.e->cursed_or_end = -1;
                break;
            case R_Select:
            case R_SelectOr: {
                Node *n = New(NodeKind::Select, here.begin, here.end);
                n->e = v[0].e;
                n->path = std::move(v[2].names);
                if (rule == R_SelectOr) {
                    n->def = v[4].e;
                    WarnIfCursedOr(v[4].e);
                }
                r.e = n;
                break;
            }
            case R_CursedOr: {
                Node *orv = New(NodeKind::Var, l[1].begin, l[1].end);
                orv->name = "or";
                orv->text = "cursed";  // its Nix position is the call's start
                orv->nix_pos = here.begin;
                Node *n = New(NodeKind::Call, here.begin, here.end);
                n->kids = {v[0].e, orv};
                n->cursed_or_end = here.end;
                r.e = n;
                break;
            }
            case R_SelectSimple:
                r = std::move(v[0]);
                break;
            case R_Var: {
                if (v[0].text == "__curPos" && !v[0].missing) {
                    r.e = New(NodeKind::CurPos, here.begin, here.end);
                } else {
                    Node *n = New(NodeKind::Var, here.begin, here.end);
                    n->name = v[0].text;
                    n->missing = v[0].missing;
                    r.e = n;
                }
                break;
            }
            case R_Int:
            case R_Float: {
                Node *n = New(rule == R_Int ? NodeKind::Int : NodeKind::Float, here.begin, here.end);
                n->text = v[0].text;
                r.e = n;
                break;
            }
            case R_String:
                r.e = v[1].e;
                r.e->start = here.begin;
                r.e->end = here.end;
                break;
            case R_IndString:
                r.e = IndStringNode(std::move(v[1].parts), here.begin, here.end);
                break;
            case R_Path:
            case R_PathInterp: {
                Node *n = New(NodeKind::Path, here.begin, here.end);
                n->text = v[0].text;
                if (rule == R_PathInterp) {
                    for (StrPart &p : v[1].parts) {
                        if (p.e) n->kids.push_back(p.e);
                    }
                }
                r.e = n;
                break;
            }
            case R_SPath: {
                Node *n = New(NodeKind::SPath, here.begin, here.end);
                n->text = v[0].text.size() >= 2 ? v[0].text.substr(1, v[0].text.size() - 2) : v[0].text;
                r.e = n;
                break;
            }
            case R_Uri: {
                Node *n = New(NodeKind::Uri, here.begin, here.end);
                n->text = v[0].text;
                r.e = n;
                break;
            }
            case R_Paren:
                r = std::move(v[1]);
                break;
            case R_OldLet:
            case R_RecSet:
            case R_Set: {
                Node *n = v[rule == R_Set ? 1 : 2].e;
                n->start = here.begin;
                n->end = here.end;
                n->open = l[rule == R_Set ? 0 : 1].begin;
                n->close = l[rule == R_Set ? 2 : 3].begin;
                if (rule != R_Set) n->recursive = true;
                if (rule == R_OldLet) n->old_let = true;
                r.e = n;
                break;
            }
            case R_EmptySet: {
                Node *n = New(NodeKind::Attrs, here.begin, here.end);
                n->open = l[0].begin;
                n->close = l[1].begin;
                r.e = n;
                break;
            }
            case R_List:
                r.e = v[1].e;
                r.e->start = here.begin;
                r.e->end = here.end;
                break;
            case R_StrPartsStr: {
                StrPart p;
                p.pos = l[0].begin;
                p.text = v[0].text;
                std::vector<StrPart> ps{p};
                r.e = StringNode(NodeKind::String, std::move(ps), here.begin, here.end);
                break;
            }
            case R_StrPartsInterp:
                r.e = StringNode(NodeKind::String, std::move(v[0].parts), here.begin, here.end);
                break;
            case R_StrPartsEmpty:
                r.e = StringNode(NodeKind::String, {}, here.begin, here.end);
                break;
            case R_InterpStr: {
                r = std::move(v[0]);
                StrPart p;
                p.pos = l[1].begin;
                p.text = v[1].text;
                r.parts.push_back(p);
                break;
            }
            case R_InterpExpr: {
                r = std::move(v[0]);
                StrPart p;
                p.pos = l[1].begin;
                p.e = v[2].e;
                r.parts.push_back(p);
                break;
            }
            case R_InterpFirstExpr: {
                StrPart p;
                p.pos = l[0].begin;
                p.e = v[1].e;
                r.parts.push_back(p);
                break;
            }
            case R_InterpStrExpr: {
                StrPart a, b;
                a.pos = l[0].begin;
                a.text = v[0].text;
                b.pos = l[1].begin;
                b.e = v[2].e;
                r.parts = {a, b};
                break;
            }
            case R_PathStart:
            case R_HPathStart:
                r.text = v[0].text;
                break;
            case R_IndPartsStr: {
                r = std::move(v[0]);
                StrPart p;
                p.pos = l[1].begin;
                p.text = v[1].text;
                p.has_indent = v[1].has_indent;
                r.parts.push_back(p);
                break;
            }
            case R_IndPartsExpr: {
                r = std::move(v[0]);
                StrPart p;
                p.pos = l[1].begin;
                p.e = v[2].e;
                r.parts.push_back(p);
                break;
            }
            case R_IndPartsEmpty:
                break;
            case R_BindsEmpty:
                r.e = New(NodeKind::Attrs, here.begin, here.end);
                break;
            case R_BindAssign:
            case R_BindFirst: {
                const int k = rule == R_BindAssign ? 1 : 0;
                Node *attrs = rule == R_BindAssign ? v[0].e : New(NodeKind::Attrs, here.begin, here.end);
                Binding b;
                b.kind = Binding::Assign;
                b.path = v[k].names;
                b.value = v[k + 2].e;
                b.start = l[k].begin;
                b.end = l[k + 3].end;
                attrs->bindings.push_back(b);
                if (!v[k].names.empty()) AddAttr(attrs, v[k].names, l[k].begin, v[k + 2].e);
                r.e = attrs;
                break;
            }
            case R_BindInherit: {
                Node *attrs = v[0].e;
                Binding b;
                b.kind = Binding::Inherit;
                b.path = v[2].names;
                b.start = l[1].begin;
                b.end = l[3].end;
                attrs->bindings.push_back(b);
                InheritInto(attrs, v[2].names, AttrDef::Inherited, -1);
                r.e = attrs;
                break;
            }
            case R_BindInheritFrom: {
                Node *attrs = v[0].e;
                Binding b;
                b.kind = Binding::InheritFrom;
                b.path = v[5].names;
                b.from = v[3].e;
                b.start = l[1].begin;
                b.end = l[6].end;
                attrs->bindings.push_back(b);
                attrs->inherit_from.push_back(v[3].e);
                InheritInto(attrs, v[5].names, AttrDef::InheritedFrom,
                            static_cast<int>(attrs->inherit_from.size()) - 1);
                r.e = attrs;
                break;
            }
            case R_AttrsAttr: {
                r = std::move(v[0]);
                AttrName a;
                a.name = v[1].text;
                a.start = l[1].begin;
                a.end = l[1].end;
                a.missing = v[1].missing;
                r.names.push_back(a);
                break;
            }
            case R_AttrsString: {
                r = std::move(v[0]);
                AttrName a = NameFromStringAttr(v[1], l[1]);
                if (a.dynamic) {
                    Error(l[1].begin, l[1].begin, l[1].end, "dynamic-inherit",
                          "dynamic attributes not allowed in inherit");
                } else {
                    r.names.push_back(a);
                }
                break;
            }
            case R_AttrsEmpty:
            case R_ListEmpty:
                if (rule == R_ListEmpty) r.e = New(NodeKind::List, here.begin, here.end);
                break;
            case R_PathAttr:
            case R_PathString: {
                r = std::move(v[0]);
                if (rule == R_PathAttr) {
                    AttrName a;
                    a.name = v[2].text;
                    a.start = l[2].begin;
                    a.end = l[2].end;
                    a.missing = v[2].missing;
                    r.names.push_back(a);
                } else {
                    r.names.push_back(NameFromStringAttr(v[2], l[2]));
                }
                break;
            }
            case R_PathFirstAttr: {
                AttrName a;
                a.name = v[0].text;
                a.start = l[0].begin;
                a.end = l[0].end;
                a.missing = v[0].missing;
                r.names.push_back(a);
                break;
            }
            case R_PathFirstString:
                r.names.push_back(NameFromStringAttr(v[0], l[0]));
                break;
            case R_AttrId:
                r = std::move(v[0]);
                break;
            case R_AttrOr:
                r = std::move(v[0]);
                r.text = "or";
                break;
            case R_StringAttr:
                r.e = v[1].e;
                r.e->start = here.begin;
                r.e->end = here.end;
                break;
            case R_DynamicAttr:
                r.e = v[1].e;
                break;
            case R_ListElem:
                r = std::move(v[0]);
                r.e->kids.push_back(v[1].e);
                WarnIfCursedOr(v[1].e);
                break;
            case R_FormalsEllipsis:
                r = std::move(v[1]);
                r.ellipsis = true;
                break;
            case R_OnlyEllipsis:
                r.ellipsis = true;
                break;
            case R_FormalsComma:
            case R_FormalsPlain:
                r = std::move(v[1]);
                r.ellipsis = false;
                break;
            case R_FormalsEmpty:
                break;
            case R_FormalsMore:
                r = std::move(v[0]);
                r.formals.push_back(v[2].formals[0]);
                break;
            case R_FormalsOne:
                r = std::move(v[0]);
                break;
            case R_Formal:
            case R_FormalDefault: {
                Formal f;
                f.name = v[0].text;
                f.start = v[0].tok_start;
                f.end = v[0].tok_end;
                if (rule == R_FormalDefault) f.def = v[2].e;
                r.formals.push_back(f);
                break;
            }
            default:
                break;
        }
        return r;
    }

    // ---- tokens ----
    const Token &Peek() const { return inserted_.empty() ? toks_[next_] : inserted_.front(); }
    Token Take() {
        if (!inserted_.empty()) {
            Token t = std::move(inserted_.front());
            inserted_.pop_front();
            return t;
        }
        Token t = toks_[next_];
        if (t.tok != T_END) next_++;
        return t;
    }

    std::vector<int> Upcoming(size_t n) const {
        std::vector<int> out;
        for (const Token &t : inserted_) {
            if (out.size() >= n) return out;
            out.push_back(t.tok);
        }
        for (size_t k = next_; k < toks_.size() && out.size() < n; k++) {
            out.push_back(toks_[k].tok);
            if (toks_[k].tok == T_END) break;
        }
        return out;
    }

    Token Synthetic(int tok) const {
        Token t;
        t.tok = tok;
        t.synthetic = true;
        const Token &at = Peek();
        const int pos = locs_.empty() ? 0 : locs_.back().end;
        t.begin = t.end = pos;
        t.src_start = t.src_end = at.src_start;
        if (tok == T_IND_STR || tok == T_STR) t.has_indent = false;
        return t;
    }

    // ---- error messages (bison's yysyntax_error) ----
    void SyntaxError(int state, const Token &la) {
        std::string msg = "syntax error, unexpected ";
        msg += kTokenNames[la.tok];
        std::vector<int> expected;
        for (int t = 0; t < kNumTokens; t++) {
            if (t == T_ERROR) continue;
            const int a = kAction[state][t];
            if (a != 0 && a != kExplicitError) expected.push_back(t);
        }
        if (!expected.empty() && expected.size() <= 4) {
            msg += ", expecting ";
            for (size_t k = 0; k < expected.size(); k++) {
                if (k) msg += " or ";
                msg += kTokenNames[expected[k]];
            }
        }
        const bool eof = msg.rfind("syntax error, unexpected end of file", 0) == 0;
        const int at = eof ? la.end : la.begin;
        out_->has_syntax_error = true;
        out_->syntax_error_offset = la.src_start;
        Error(at, la.src_start, std::max(la.src_end, la.src_start + (la.tok == T_END ? 0 : 1)), "syntax-error", msg,
              true);
    }

    // ---- error recovery: insert or skip a token, checked against what follows ----
    bool Recover(const Token &la) {
        if (++repairs_ > 200) return false;
        static const int kPrefer[] = {T_SEMI, T_RBRACE, T_RPAREN, T_RBRACK, T_IN_KW, T_THEN, T_ELSE, T_ID,
                                      T_ASSIGN, T_COLON, T_QUOTE, T_IND_STRING_CLOSE, T_PATH_END, T_COMMA};
        std::vector<int> order(std::begin(kPrefer), std::end(kPrefer));
        for (int t = 3; t < kNumTokens; t++) {
            if (std::find(order.begin(), order.end(), t) == order.end() && t != T_NOT && t != T_NEGATE) order.push_back(t);
        }
        if (la.tok == T_END) {
            // Close whatever is open: the shortest run of insertions that lets the input end.
            struct Item {
                std::vector<int> stack;
                std::vector<int> seq;
            };
            std::vector<Item> frontier{{states_, {}}};
            for (int depth = 0; depth < 12 && !frontier.empty(); depth++) {
                std::vector<Item> next;
                for (const Item &it : frontier) {
                    for (int t : order) {
                        std::vector<int> seq = it.seq;
                        seq.push_back(t);
                        std::vector<int> probe = seq;
                        probe.push_back(T_END);
                        if (Simulate(states_, probe) == static_cast<int>(probe.size())) {
                            for (auto x = seq.rbegin(); x != seq.rend(); ++x) inserted_.push_front(Synthetic(*x));
                            return true;
                        }
                        if (Simulate(states_, seq) == static_cast<int>(seq.size()) && next.size() < 400)
                            next.push_back({states_, seq});
                    }
                }
                frontier = std::move(next);
            }
            return false;
        }
        for (int need : {3, 2, 1}) {
            const std::vector<int> ahead = Upcoming(static_cast<size_t>(need));
            for (int t : order) {
                std::vector<int> probe{t};
                probe.insert(probe.end(), ahead.begin(), ahead.end());
                if (Simulate(states_, probe) == static_cast<int>(probe.size())) {
                    inserted_.push_front(Synthetic(t));
                    return true;
                }
            }
            // Skip the token instead.
            std::vector<int> rest = Upcoming(static_cast<size_t>(need) + 1);
            if (!rest.empty() && rest[0] != T_END) {
                rest.erase(rest.begin());
                if (!rest.empty() && Simulate(states_, rest) == static_cast<int>(rest.size())) {
                    Take();
                    return true;
                }
            }
        }
        // Nothing fits: drop the token and try again at the next one.
        if (Peek().tok == T_END) return false;
        Take();
        return true;
    }

    void GiveUp() {
        out_->recovered = false;
        Node *n = New(NodeKind::Error, 0, static_cast<int>(src_.size()));
        for (Value &v : values_) {
            if (v.e) n->kids.push_back(v.e);
            for (StrPart &p : v.parts) {
                if (p.e) n->kids.push_back(p.e);
            }
            for (Formal &f : v.formals) {
                if (f.def) n->kids.push_back(f.def);
            }
            for (AttrName &a : v.names) {
                if (a.expr) n->kids.push_back(a.expr);
            }
        }
        out_->root = n;
    }

    void Drive() {
        states_ = {0};
        values_.emplace_back();
        locs_.push_back({0, 0});
        bool have = false;
        Token la;
        bool reported = false;
        while (true) {
            const int s = states_.back();
            if (s == kFinalState) return;
            int act;
            if (!g_has_explicit[s]) {
                act = kDefaultReduce[s] >= 0 ? -kDefaultReduce[s] - 1 : kExplicitError;
            } else {
                if (!have) {
                    la = Take();
                    have = true;
                    if (!la.error.empty()) {
                        Error(la.error_offset, la.src_start, std::max(la.src_end, la.src_start + 1), la.error_code,
                              la.error);
                        if (la.error_code == "trailing-slash") {
                            out_->errors.back().start = la.src_start - 1;
                            out_->errors.back().end = la.src_start;
                        }
                    }
                }
                act = kAction[s][la.tok];
                if (act == 0) act = kDefaultReduce[s] >= 0 ? -kDefaultReduce[s] - 1 : kExplicitError;
            }
            if (act == kExplicitError) {
                if (!reported) {
                    SyntaxError(s, la);
                    reported = true;
                }
                // Put the lookahead back (its lexer error is already recorded) and repair.
                la.error.clear();
                inserted_.push_front(la);
                have = false;
                if (!Recover(inserted_.front())) {
                    GiveUp();
                    return;
                }
                continue;
            }
            if (act > 0) {
                Value v;
                v.text = la.text;
                v.has_indent = la.has_indent;
                v.tok_start = la.src_start;
                v.tok_end = la.src_end;
                v.missing = la.synthetic;
                states_.push_back(act - 1);
                values_.push_back(std::move(v));
                locs_.push_back({la.begin, la.end});
                have = false;
                continue;
            }
            const int rule = -act - 1;
            const size_t len = static_cast<size_t>(kRuleLen[rule]);
            const size_t base = values_.size() - len;
            Loc here;
            if (len > 0) {
                here.begin = locs_[base].begin;
                here.end = locs_.back().end;
            } else {
                here.begin = here.end = locs_.back().end;
            }
            Value r = Reduce(rule, values_.data() + base, locs_.data() + base, here);
            states_.resize(states_.size() - len);
            values_.resize(base);
            locs_.resize(base);
            const int g = kGoto[states_.back()][kRuleLhs[rule]];
            if (g < 0) {
                GiveUp();
                return;
            }
            states_.push_back(g);
            values_.push_back(std::move(r));
            locs_.push_back(here);
        }
    }
};

}  // namespace

void NixLineCol(std::string_view src, int offset, int *line, int *col) {
    int ln = 1;
    size_t start = 0;
    const size_t off = static_cast<size_t>(std::max(0, offset));
    for (size_t i = 0; i < src.size() && i < off; i++) {
        if (src[i] == '\r') {
            if (i + 1 < src.size() && src[i + 1] == '\n') {
                if (i + 1 >= off) break;  // inside a CRLF: still on this line
                i++;
            }
            ln++;
            start = i + 1;
        } else if (src[i] == '\n') {
            ln++;
            start = i + 1;
        }
    }
    *line = ln;
    *col = static_cast<int>(off - start) + 1;
}

bool IsNixIdentifier(std::string_view s) {
    if (s.empty() || !IsIdStart(static_cast<unsigned char>(s[0]))) return false;
    for (char c : s) {
        if (!IsIdChar(static_cast<unsigned char>(c))) return false;
    }
    for (const char *k : kKeywords) {
        if (s == k) return false;
    }
    return true;
}

std::unique_ptr<ParseResult> Parse(std::string_view src, const Options &opts) {
    auto out = std::make_unique<ParseResult>();
    Parser p(src, opts, out.get());
    p.Run();
    if (!out->root) {
        out->arena.push_back(std::make_unique<Node>());
        out->root = out->arena.back().get();
        out->root->kind = NodeKind::Error;
        out->root->end = static_cast<int>(src.size());
        out->recovered = false;
    }
    return out;
}

}  // namespace nixparse
