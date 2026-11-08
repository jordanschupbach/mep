// The Fortran reader, part 1: source form, the preprocessor model,
// statement assembly and tokens (fortran_parse.h). Part 2, the statement
// parser and the construct structure, is fortran_parse_stmt.cpp.
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <set>

#include "fortran_parse.h"
#include "fortran_parse_internal.h"

namespace fortran {

namespace {

bool IsLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsNameChar(char c) { return IsLetter(c) || IsDigit(c) || c == '_' || c == '$'; }
bool IsBlank(char c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r'; }

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string Suffix(std::string_view path) {
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return "";
    return std::string(path.substr(dot + 1));
}

}  // namespace

Form FormForPath(std::string_view path) {
    std::string s = Lower(Suffix(path));
    return (s == "f" || s == "for" || s == "ftn" || s == "fpp" || s == "f77") ? Form::Fixed : Form::Free;
}

bool PreprocessedPath(std::string_view path) {
    std::string s = Suffix(path);
    if (s.empty()) return false;
    if (Lower(s) == "fpp") return true;
    return std::isupper(static_cast<unsigned char>(s[0])) != 0;
}

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
    lines.push_back(cur);
    return lines;
}

std::string DecodeSource(std::string_view b) {
    auto u8 = [](std::string *out, uint32_t c) {
        if (c < 0x80) {
            *out += static_cast<char>(c);
        } else if (c < 0x800) {
            *out += static_cast<char>(0xC0 | (c >> 6));
            *out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            *out += static_cast<char>(0xE0 | (c >> 12));
            *out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            *out += static_cast<char>(0xF0 | (c >> 18));
            *out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            *out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (c & 0x3F));
        }
    };
    auto byte = [&](size_t i) { return static_cast<uint32_t>(static_cast<unsigned char>(b[i])); };
    if (b.size() >= 3 && byte(0) == 0xEF && byte(1) == 0xBB && byte(2) == 0xBF) return std::string(b.substr(3));
    int width = 0;
    bool big = false;
    if (b.size() >= 4 && byte(0) == 0 && byte(1) == 0 && byte(2) == 0xFE && byte(3) == 0xFF) {
        width = 4;
        big = true;
    } else if (b.size() >= 4 && byte(0) == 0xFF && byte(1) == 0xFE && byte(2) == 0 && byte(3) == 0) {
        width = 4;
    } else if (b.size() >= 2 && byte(0) == 0xFE && byte(1) == 0xFF) {
        width = 2;
        big = true;
    } else if (b.size() >= 2 && byte(0) == 0xFF && byte(1) == 0xFE) {
        width = 2;
    }
    if (!width) return std::string(b);
    std::string out;
    const size_t w = static_cast<size_t>(width);
    for (size_t i = w; i + w <= b.size(); i += w) {
        uint32_t c = 0;
        for (size_t k = 0; k < w; k++) c |= byte(i + k) << (8 * (big ? w - 1 - k : k));
        if (width == 2 && c >= 0xD800 && c < 0xDC00 && i + 2 * w <= b.size()) {
            uint32_t lo = 0;
            for (size_t k = 0; k < w; k++) lo |= byte(i + w + k) << (8 * (big ? w - 1 - k : k));
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += w;
            }
        }
        if (c > 0x10FFFF) c = '?';
        u8(&out, c);
    }
    return out;
}

const char *StmtKindName(StmtKind k) {
    switch (k) {
        case StmtKind::Program: return "PROGRAM";
        case StmtKind::Module: return "MODULE";
        case StmtKind::Submodule: return "SUBMODULE";
        case StmtKind::Subroutine: return "SUBROUTINE";
        case StmtKind::Function: return "FUNCTION";
        case StmtKind::BlockData: return "BLOCK DATA";
        case StmtKind::ModuleProcedureUnit: return "PROCEDURE";
        case StmtKind::DerivedType: return "TYPE";
        case StmtKind::Interface: return "INTERFACE";
        case StmtKind::Enum: return "ENUM";
        case StmtKind::If: return "IF";
        case StmtKind::Do: return "DO";
        case StmtKind::SelectCase: return "SELECT";
        case StmtKind::SelectType: return "SELECT";
        case StmtKind::SelectRank: return "SELECT";
        case StmtKind::Where: return "WHERE";
        case StmtKind::Forall: return "FORALL";
        case StmtKind::Associate: return "ASSOCIATE";
        case StmtKind::Block: return "BLOCK";
        case StmtKind::Critical: return "CRITICAL";
        case StmtKind::ChangeTeam: return "TEAM";
        default: return "";
    }
}

// ---- preprocessor model --------------------------------------------------------

namespace {

struct Macro {
    bool function = false;
    std::vector<std::string> params;
    std::string body;
};

class Preprocessor {
public:
    explicit Preprocessor(Source *src) : src_(src) {
        Define("__GFORTRAN__", "1");
        Define("__GNUC__", "15");
        Define("__GNUC_MINOR__", "3");
        Define("__GNUC_PATCHLEVEL__", "0");
        Define("__linux__", "1");
        Define("__linux", "1");
        Define("__unix__", "1");
        Define("__unix", "1");
        Define("__x86_64__", "1");
        Define("__ELF__", "1");
        Define("__GFC_INT_16__", "1");
        Define("__GFC_REAL_10__", "1");
        Define("__GFC_REAL_16__", "1");
        Define("__LINE__", "0");
        Define("__FILE__", "\"\"");
        Define("__DATE__", "\"\"");
        Define("__TIME__", "\"\"");
    }

    void Define(const std::string &name, const std::string &body) {
        Macro m;
        m.body = body;
        macros_[name] = m;
    }

    const std::map<std::string, Macro> &macros() const { return macros_; }

    // Processes the directive `text` (from '#', continuations joined) at
    // `line`; returns false when the line was not a directive.
    void Directive(const std::string &text, int line, int end_line) {
        size_t i = 1;
        while (i < text.size() && IsBlank(text[i])) i++;
        size_t ns = i;
        while (i < text.size() && IsNameChar(text[i])) i++;
        std::string name = text.substr(ns, i - ns);
        std::string rest = i < text.size() ? text.substr(i) : "";
        // A trailing /* comment */ or // comment is not part of the directive.
        StripComment(&rest);
        fortran::Directive d;
        d.line = line;
        d.name = name;
        d.text = text;
        d.active = Active();
        src_->directives.push_back(d);
        if (name == "if" || name == "ifdef" || name == "ifndef") {
            Frame f;
            f.parent_active = Active();
            if (!f.parent_active) {
                f.active = false;
                f.taken = true;
            } else {
                bool certain = true;
                bool v = false;
                if (name == "if") {
                    v = Eval(rest, &certain);
                } else {
                    std::string id = FirstName(rest);
                    bool def = macros_.count(id) != 0;
                    v = name == "ifdef" ? def : !def;
                    // A macro the file does not define could come from -D.
                    certain = def || predefined_.count(id) != 0;
                }
                f.active = v;
                f.taken = v;
                f.certain = certain;
            }
            stack_.push_back(f);
        } else if (name == "elif" || name == "elifdef" || name == "elifndef") {
            if (stack_.empty()) {
                Error(line, "#elif without #if");
                return;
            }
            Frame &f = stack_.back();
            if (!f.parent_active || f.taken) {
                f.active = false;
            } else {
                bool certain = true;
                bool v;
                if (name == "elif") {
                    v = Eval(rest, &certain);
                } else {
                    bool def = macros_.count(FirstName(rest)) != 0;
                    v = name == "elifdef" ? def : !def;
                }
                f.active = v;
                f.taken = v;
                f.certain = f.certain && certain;
            }
        } else if (name == "else") {
            if (stack_.empty()) {
                Error(line, "#else without #if");
                return;
            }
            Frame &f = stack_.back();
            f.active = f.parent_active && !f.taken;
            f.taken = true;
        } else if (name == "endif") {
            if (stack_.empty()) {
                Error(line, "#endif without #if");
                return;
            }
            stack_.pop_back();
        } else if (!Active()) {
            return;
        } else if (name == "define") {
            DefineDirective(rest);
        } else if (name == "undef") {
            macros_.erase(FirstName(rest));
        } else if (name == "include") {
            src_->unresolved_include = true;
        } else if (name == "error") {
            bool certain = true;
            for (const Frame &f : stack_) certain = certain && f.certain;
            if (certain) {
                std::string msg = rest;
                while (!msg.empty() && IsBlank(msg.front())) msg.erase(msg.begin());
                Diag dg;
                dg.pos = {line, 0};
                dg.end = {end_line, 0};
                dg.message = "#error " + msg;
                dg.code = "preprocessor-error";
                src_->diags.push_back(dg);
            }
        }
        // #warning, #pragma, #line, #ident and the rest change nothing here.
    }

    bool Active() const { return stack_.empty() || stack_.back().active; }

    void Finish(int nlines) {
        if (!stack_.empty()) Error(nlines > 0 ? nlines - 1 : 0, "unterminated #if");
    }

    // Expands the file's macros in a token list (object-like and
    // function-like), keeping each expansion at the macro name's place.
    void Expand(std::vector<Token> *toks, int depth = 0) const {
        if (macros_.size() <= predefined_.size() && !HasLineMacros(*toks)) return;
        std::vector<Token> out;
        for (size_t i = 0; i < toks->size(); i++) {
            const Token &t = (*toks)[i];
            auto it = t.kind == TokKind::Name ? macros_.find(t.text) : macros_.end();
            if (it == macros_.end() || depth > 32) {
                out.push_back(t);
                continue;
            }
            const Macro &m = it->second;
            std::string body = m.body;
            if (t.text == "__LINE__") body = std::to_string(t.pos.line + 1);
            size_t next = i + 1;
            if (m.function) {
                if (next >= toks->size() || (*toks)[next].text != "(") {
                    out.push_back(t);
                    continue;
                }
                // Collect the arguments as spellings.
                std::vector<std::string> args(1);
                int depth_p = 0;
                size_t j = next + 1;
                for (; j < toks->size(); j++) {
                    const std::string &s = (*toks)[j].text;
                    if (s == "(" || s == "(/" || s == "[") depth_p++;
                    if (s == ")" || s == "/)" || s == "]") {
                        if (depth_p == 0) break;
                        depth_p--;
                    }
                    if (s == "," && depth_p == 0) {
                        args.emplace_back();
                        continue;
                    }
                    if (!args.back().empty()) args.back() += ' ';
                    args.back() += s;
                }
                if (j >= toks->size()) {
                    out.push_back(t);
                    continue;
                }
                body = Substitute(m, args);
                next = j + 1;
            }
            std::vector<Token> sub = TokenizeText(body, t.pos, t.end);
            Expand(&sub, depth + 1);
            out.insert(out.end(), sub.begin(), sub.end());
            i = next - 1;
        }
        *toks = std::move(out);
    }

    std::map<std::string, std::string> UserMacros() const {
        std::map<std::string, std::string> out;
        for (const auto &kv : macros_) {
            if (!predefined_.count(kv.first)) out[kv.first] = kv.second.body;
        }
        return out;
    }

    void SealPredefined() {
        for (const auto &kv : macros_) predefined_.insert(kv.first);
    }

private:
    struct Frame {
        bool parent_active = true, active = true, taken = false, certain = true;
    };

    static bool HasLineMacros(const std::vector<Token> &toks) {
        for (const Token &t : toks) {
            if (t.kind == TokKind::Name && (t.text == "__LINE__" || t.text == "__FILE__")) return true;
        }
        return false;
    }

    static void StripComment(std::string *s) {
        bool q1 = false, q2 = false;
        for (size_t i = 0; i < s->size(); i++) {
            char c = (*s)[i];
            if (c == '\'' && !q2) q1 = !q1;
            if (c == '"' && !q1) q2 = !q2;
            if (q1 || q2) continue;
            if (c == '/' && i + 1 < s->size() && (*s)[i + 1] == '*') {
                size_t e = s->find("*/", i + 2);
                s->erase(i, e == std::string::npos ? std::string::npos : e + 2 - i);
                s->insert(i, " ");
            } else if (c == '/' && i + 1 < s->size() && (*s)[i + 1] == '/') {
                s->erase(i);
                break;
            }
        }
    }

    static std::string FirstName(const std::string &s) {
        size_t i = 0;
        while (i < s.size() && !IsNameChar(s[i])) i++;
        size_t b = i;
        while (i < s.size() && IsNameChar(s[i])) i++;
        return s.substr(b, i - b);
    }

    void DefineDirective(const std::string &rest) {
        size_t i = 0;
        while (i < rest.size() && IsBlank(rest[i])) i++;
        size_t b = i;
        while (i < rest.size() && IsNameChar(rest[i])) i++;
        std::string name = rest.substr(b, i - b);
        if (name.empty()) return;
        Macro m;
        if (i < rest.size() && rest[i] == '(') {
            m.function = true;
            i++;
            std::string cur;
            while (i < rest.size() && rest[i] != ')') {
                if (rest[i] == ',') {
                    m.params.push_back(cur);
                    cur.clear();
                } else if (!IsBlank(rest[i])) {
                    cur += rest[i];
                }
                i++;
            }
            if (!cur.empty()) m.params.push_back(cur);
            if (i < rest.size()) i++;
        }
        while (i < rest.size() && IsBlank(rest[i])) i++;
        m.body = rest.substr(i);
        while (!m.body.empty() && IsBlank(m.body.back())) m.body.pop_back();
        macros_[name] = m;
    }

    static std::string Substitute(const Macro &m, const std::vector<std::string> &args) {
        std::string out;
        const std::string &b = m.body;
        for (size_t i = 0; i < b.size();) {
            if (IsLetter(b[i]) || b[i] == '_') {
                size_t s = i;
                while (i < b.size() && IsNameChar(b[i])) i++;
                std::string id = b.substr(s, i - s);
                bool done = false;
                for (size_t k = 0; k < m.params.size(); k++) {
                    if (m.params[k] == id || (m.params[k] == "..." && id == "__VA_ARGS__")) {
                        if (m.params[k] == "...") {
                            for (size_t a = k; a < args.size(); a++) {
                                if (a > k) out += ", ";
                                out += args[a];
                            }
                        } else if (k < args.size()) {
                            out += args[k];
                        }
                        done = true;
                        break;
                    }
                }
                if (!done) out += id;
            } else if (b[i] == '#' && i + 1 < b.size() && b[i + 1] == '#') {
                while (!out.empty() && IsBlank(out.back())) out.pop_back();
                i += 2;
                while (i < b.size() && IsBlank(b[i])) i++;
            } else {
                out += b[i++];
            }
        }
        return out;
    }

    // #if expression evaluation (C preprocessor arithmetic on long long).
    struct Expr {
        const Preprocessor *pp;
        std::string s;
        size_t i = 0;
        bool *certain;
        int depth = 0;

        void Ws() {
            while (i < s.size() && IsBlank(s[i])) i++;
        }
        bool Eat(const char *op) {
            Ws();
            size_t n = std::char_traits<char>::length(op);
            if (s.compare(i, n, op) == 0) {
                // don't split && / || / << / >> / == / <= ...
                if (n == 1 && i + 1 < s.size()) {
                    char c = op[0], d = s[i + 1];
                    if ((c == '&' && d == '&') || (c == '|' && d == '|') || (c == '<' && (d == '<' || d == '=')) ||
                        (c == '>' && (d == '>' || d == '=')) || (c == '!' && d == '=') || (c == '=' && d == '='))
                        return false;
                }
                i += n;
                return true;
            }
            return false;
        }
        long long Primary() {
            Ws();
            if (++depth > 200) return 0;
            if (Eat("(")) {
                long long v = Cond();
                Eat(")");
                depth--;
                return v;
            }
            if (Eat("!")) return !Primary();
            if (Eat("~")) return ~Primary();
            if (Eat("-")) return -Primary();
            if (Eat("+")) return Primary();
            if (i < s.size() && IsDigit(s[i])) {
                size_t b = i;
                while (i < s.size() && (IsNameChar(s[i]))) i++;
                return std::strtoll(s.substr(b, i - b).c_str(), nullptr, 0);
            }
            if (i < s.size() && s[i] == '\'') {
                i++;
                long long v = i < s.size() ? s[i] : 0;
                while (i < s.size() && s[i] != '\'') i++;
                if (i < s.size()) i++;
                return v;
            }
            if (i < s.size() && (IsLetter(s[i]) || s[i] == '_')) {
                size_t b = i;
                while (i < s.size() && IsNameChar(s[i])) i++;
                std::string id = s.substr(b, i - b);
                if (id == "defined") {
                    Ws();
                    bool paren = Eat("(");
                    Ws();
                    size_t nb = i;
                    while (i < s.size() && IsNameChar(s[i])) i++;
                    std::string n = s.substr(nb, i - nb);
                    if (paren) Eat(")");
                    bool def = pp->macros_.count(n) != 0;
                    if (!def && !pp->predefined_.count(n)) *certain = false;
                    return def ? 1 : 0;
                }
                auto it = pp->macros_.find(id);
                if (it == pp->macros_.end()) {
                    *certain = false;
                    // A function-like use of an unknown name: skip its arguments.
                    Ws();
                    if (i < s.size() && s[i] == '(') {
                        int d = 0;
                        for (; i < s.size(); i++) {
                            if (s[i] == '(') d++;
                            if (s[i] == ')' && --d == 0) {
                                i++;
                                break;
                            }
                        }
                    }
                    return 0;
                }
                if (depth > 100) return 0;
                Expr sub{pp, it->second.body, 0, certain, depth + 1};
                return sub.body_value();
            }
            return 0;
        }
        long long body_value() {
            if (s.empty()) return 0;
            return Cond();
        }
        long long Unary() { return Primary(); }
        long long Mul() {
            long long v = Unary();
            for (;;) {
                if (Eat("*")) v *= Unary();
                else if (Eat("/")) {
                    long long r = Unary();
                    v = r ? v / r : 0;
                } else if (Eat("%")) {
                    long long r = Unary();
                    v = r ? v % r : 0;
                } else return v;
            }
        }
        long long Add() {
            long long v = Mul();
            for (;;) {
                if (Eat("+")) v += Mul();
                else if (Eat("-")) v -= Mul();
                else return v;
            }
        }
        long long Shift() {
            long long v = Add();
            for (;;) {
                if (Eat("<<")) v = v << (Add() & 63);
                else if (Eat(">>")) v = v >> (Add() & 63);
                else return v;
            }
        }
        long long Rel() {
            long long v = Shift();
            for (;;) {
                if (Eat("<=")) v = v <= Shift();
                else if (Eat(">=")) v = v >= Shift();
                else if (Eat("<")) v = v < Shift();
                else if (Eat(">")) v = v > Shift();
                else return v;
            }
        }
        long long Eq() {
            long long v = Rel();
            for (;;) {
                if (Eat("==")) v = v == Rel();
                else if (Eat("!=")) v = v != Rel();
                else return v;
            }
        }
        long long BitAnd() {
            long long v = Eq();
            while (Eat("&")) v &= Eq();
            return v;
        }
        long long BitXor() {
            long long v = BitAnd();
            while (Eat("^")) v ^= BitAnd();
            return v;
        }
        long long BitOr() {
            long long v = BitXor();
            while (Eat("|")) v |= BitXor();
            return v;
        }
        long long And() {
            long long v = BitOr();
            while (Eat("&&")) {
                long long r = BitOr();
                v = v && r;
            }
            return v;
        }
        long long Or() {
            long long v = And();
            while (Eat("||")) {
                long long r = And();
                v = v || r;
            }
            return v;
        }
        long long Cond() {
            long long c = Or();
            if (Eat("?")) {
                long long a = Cond();
                Eat(":");
                long long b = Cond();
                return c ? a : b;
            }
            return c;
        }
    };

    bool Eval(const std::string &e, bool *certain) const {
        Expr x{this, e, 0, certain, 0};
        return x.Cond() != 0;
    }

    void Error(int line, const std::string &msg) {
        Diag d;
        d.pos = {line, 0};
        d.end = {line, 0};
        d.message = msg;
        d.code = "preprocessor-error";
        src_->diags.push_back(d);
    }

    Source *src_;
    std::map<std::string, Macro> macros_;
    std::set<std::string> predefined_;
    std::vector<Frame> stack_;
};

// ---- statement assembly ----------------------------------------------------------

// A statement's characters, with each one's position.
struct RawStmt {
    std::string text;
    std::vector<Pos> pos;
    std::string label;
    Pos label_pos;
    bool unterminated_string = false;
    Pos string_pos;
};

class Assembler {
public:
    Assembler(Source *src, const ReadOptions &opts) : src_(src), opts_(opts) {}

    std::vector<RawStmt> stmts;

    void Comment(int line, int col, const std::string &text) {
        fortran::Comment c;
        c.pos = {line, col};
        c.end = {line, col + static_cast<int>(text.size())};
        c.text = text;
        src_->comments.push_back(c);
    }

    void Finish() {
        if (!cur_.text.empty() || !cur_.label.empty()) {
            // Trim trailing blanks.
            while (!cur_.text.empty() && IsBlank(cur_.text.back())) {
                cur_.text.pop_back();
                cur_.pos.pop_back();
            }
            bool blank = true;
            for (char c : cur_.text) blank = blank && IsBlank(c);
            if (!blank || !cur_.label.empty()) stmts.push_back(cur_);
        }
        cur_ = RawStmt();
        in_string_ = 0;
    }

    void Add(char c, int line, int col) {
        if (cur_.text.empty() && IsBlank(c)) return;
        cur_.text += c;
        cur_.pos.push_back({line, col});
    }

    // ---- free form ----
    void FreeLine(const std::string &l, int line) {
        size_t i = 0;
        size_t n = l.size();
        if (opts_.free_line_length > 0) CheckLength(l, line);
        if (continuing_) {
            while (i < n && IsBlank(l[i])) i++;
            if (i >= n) return;  // blank line between continuation lines
            if (l[i] == '!' && !in_string_) {
                Comment(line, static_cast<int>(i), l.substr(i));
                return;
            }
            if (l[i] == '&') {
                i++;
            } else if (in_string_) {
                i = 0;  // character context goes on from column 1
            } else {
                // no leading `&`: the statement goes on from column 1, so the
                // blanks before the text still separate (`ENDDO&` / `  LOOP`)
                if (i > 0) Add(' ', line, 0);
            }
            continuing_ = false;
        } else {
            while (i < n && IsBlank(l[i])) i++;
            if (i >= n) return;
            if (l[i] == '!') {
                Comment(line, static_cast<int>(i), l.substr(i));
                return;
            }
            StartLabel(l, &i, line);
        }
        for (; i < n; i++) {
            char c = l[i];
            if (in_string_) {
                if (c == in_string_) {
                    if (i + 1 < n && l[i + 1] == in_string_) {
                        Add(c, line, static_cast<int>(i));
                        Add(c, line, static_cast<int>(i + 1));
                        i++;
                        continue;
                    }
                    in_string_ = 0;
                    Add(c, line, static_cast<int>(i));
                    continue;
                }
                if (c == '&' && RestBlank(l, i + 1)) {
                    continuing_ = true;
                    return;
                }
                Add(c, line, static_cast<int>(i));
                continue;
            }
            if (c == '!') {
                Comment(line, static_cast<int>(i), l.substr(i));
                break;
            }
            if (c == '\'' || c == '"') {
                in_string_ = c;
                cur_.string_pos = {line, static_cast<int>(i)};
                Add(c, line, static_cast<int>(i));
                continue;
            }
            if (c == ';') {
                Finish();
                // A label may follow a semicolon? No: only at line start.
                while (i + 1 < n && IsBlank(l[i + 1])) i++;
                continue;
            }
            if (c == '&') {
                size_t j = i + 1;
                while (j < n && IsBlank(l[j])) j++;
                if (j >= n || l[j] == '!') {
                    if (j < n) Comment(line, static_cast<int>(j), l.substr(j));
                    continuing_ = true;
                    return;
                }
            }
            Add(c, line, static_cast<int>(i));
        }
        if (in_string_) {
            cur_.unterminated_string = true;
            in_string_ = 0;
        }
        Finish();
    }

    // ---- fixed form ----
    void FixedLine(const std::string &l0, int line) {
        std::string l = l0;
        size_t n = l.size();
        if (n == 0) return;
        char c0 = l[0];
        if (c0 == 'c' || c0 == 'C' || c0 == '*' || c0 == 'd' || c0 == 'D' || c0 == '!') {
            if (!in_string_ || true) {
                Comment(line, 0, l);
                return;
            }
        }
        size_t first = 0;
        while (first < n && IsBlank(l[first])) first++;
        if (first >= n) return;  // blank line
        if (l[first] == '!' && first != 5) {
            Comment(line, static_cast<int>(first), l.substr(first));
            return;
        }
        bool cont = false;
        size_t text = 6;
        std::string label;
        Pos label_pos;
        if (c0 == '\t') {
            if (n > 1 && l[1] >= '1' && l[1] <= '9') {
                cont = true;
                text = 2;
            } else {
                text = 1;
            }
        } else {
            // label field: columns 1-5 (a tab ends it early)
            size_t k = 0;
            for (; k < 5 && k < n; k++) {
                char c = l[k];
                if (c == '\t') {
                    text = k + 1;
                    break;
                }
                if (IsDigit(c)) {
                    if (label.empty()) label_pos = {line, static_cast<int>(k)};
                    label += c;
                } else if (!IsBlank(c)) {
                    Diag d;
                    d.pos = {line, static_cast<int>(k)};
                    d.end = {line, static_cast<int>(k + 1)};
                    d.message = "Non-numeric character in statement label";
                    d.code = "syntax-error";
                    src_->diags.push_back(d);
                    break;
                }
            }
            if (text == 6 && n > 5 && l[5] != ' ' && l[5] != '0' && l[5] != '\t') cont = true;
            if (text == 6 && n > 5 && l[5] == '\t') text = 6;
        }
        size_t limit = opts_.fixed_line_length > 0 ? static_cast<size_t>(opts_.fixed_line_length) : n;
        if (cont) {
            if (cur_.text.empty() && stmts.empty() && cur_.label.empty()) {
                // continuation of nothing: gfortran warns and reads it as a statement
            }
        } else {
            if (in_string_) cur_.unterminated_string = true;
            Finish();
            if (!label.empty()) {
                // strip leading zeros the way labels compare
                size_t z = 0;
                while (z + 1 < label.size() && label[z] == '0') z++;
                cur_.label = label.substr(z);
                cur_.label_pos = label_pos;
            }
        }
        size_t end = std::min(n, limit);
        for (size_t i = text; i < end; i++) {
            char c = l[i];
            if (in_string_) {
                if (c == in_string_) {
                    if (i + 1 < end && l[i + 1] == in_string_) {
                        Add(c, line, static_cast<int>(i));
                        Add(c, line, static_cast<int>(i + 1));
                        i++;
                        continue;
                    }
                    in_string_ = 0;
                }
                Add(c, line, static_cast<int>(i));
                continue;
            }
            if (c == '!') {
                Comment(line, static_cast<int>(i), l.substr(i));
                break;
            }
            if (c == '\'' || c == '"') {
                in_string_ = c;
                cur_.string_pos = {line, static_cast<int>(i)};
                Add(c, line, static_cast<int>(i));
                continue;
            }
            if (c == ';') {
                Finish();
                continue;
            }
            Add(c, line, static_cast<int>(i));
        }
        if (in_string_ && opts_.fixed_line_length > 0) {
            // A character context is padded to the end of the field.
            for (size_t i = end; i < limit; i++) Add(' ', line, static_cast<int>(i));
        }
    }

    void End() {
        if (in_string_) cur_.unterminated_string = true;
        Finish();
    }

private:
    static bool RestBlank(const std::string &l, size_t i) {
        for (; i < l.size(); i++) {
            if (!IsBlank(l[i])) return false;
        }
        return true;
    }

    void StartLabel(const std::string &l, size_t *i, int line) {
        size_t j = *i;
        while (j < l.size() && IsDigit(l[j])) j++;
        if (j > *i && (j >= l.size() || IsBlank(l[j]) || l[j] == ';' || l[j] == '!')) {
            std::string lab = l.substr(*i, j - *i);
            size_t z = 0;
            while (z + 1 < lab.size() && lab[z] == '0') z++;
            cur_.label = lab.substr(z);
            cur_.label_pos = {line, static_cast<int>(*i)};
            if (j - *i > 5) {
                Diag d;
                d.pos = cur_.label_pos;
                d.end = {line, static_cast<int>(j)};
                d.message = "Too many digits in statement label";
                d.code = "syntax-error";
                src_->diags.push_back(d);
            }
            *i = j;
        }
    }

    void CheckLength(const std::string &l, int line) {
        if (static_cast<int>(l.size()) <= opts_.free_line_length) return;
        // Only code past the limit counts (gfortran: a comment may run on).
        bool q1 = false, q2 = false;
        for (size_t i = 0; i < l.size(); i++) {
            char c = l[i];
            if (c == '\'' && !q2) q1 = !q1;
            else if (c == '"' && !q1) q2 = !q2;
            else if (c == '!' && !q1 && !q2) return;
            if (static_cast<int>(i) >= opts_.free_line_length && !IsBlank(c)) {
                Diag d;
                d.pos = {line, opts_.free_line_length};
                d.end = {line, static_cast<int>(l.size())};
                d.message = "Line truncated at column " + std::to_string(opts_.free_line_length);
                d.code = "line-truncated";
                d.severity = 2;
                src_->diags.push_back(d);
                return;
            }
        }
    }

    Source *src_;
    const ReadOptions &opts_;
    RawStmt cur_;
    char in_string_ = 0;
    bool continuing_ = false;
};

}  // namespace

// ---- tokens ------------------------------------------------------------------------

namespace {

bool IsDotOperatorWord(const std::string &w) {
    static const char *const kOps[] = {"eq", "ne", "lt", "le", "gt", "ge", "and", "or", "not", "eqv", "neqv",
                                       "true", "false", "xor", "nil"};
    for (const char *o : kOps) {
        if (w == o) return true;
    }
    return false;
}

}  // namespace

std::vector<Token> Tokenize(const std::string &text, const std::vector<Pos> &pos, std::vector<Diag> *diags) {
    std::vector<Token> out;
    size_t n = text.size();
    auto P = [&](size_t i) -> Pos {
        if (pos.empty()) return Pos{};
        return i < pos.size() ? pos[i] : Pos{pos.back().line, pos.back().col + 1};
    };
    auto End = [&](size_t i) -> Pos {  // position just after char i-1
        if (pos.empty()) return Pos{};
        if (i == 0) return P(0);
        Pos p = P(i - 1);
        p.col++;
        return p;
    };
    auto push = [&](TokKind k, size_t b, size_t e) {
        Token t;
        t.kind = k;
        t.text = text.substr(b, e - b);
        t.lower = (k == TokKind::Name || k == TokKind::DotOp || k == TokKind::Logical) ? Lower(t.text) : t.text;
        if (k == TokKind::DotOp) t.lower = t.lower.substr(1, t.lower.size() - 2);
        t.pos = P(b);
        t.end = End(e);
        out.push_back(t);
    };
    auto hollerith_ok = [&]() {
        if (out.empty()) return true;
        const std::string &p = out.back().text;
        return p == "(" || p == "," || p == "/" || p == "=" || p == "*" || p == "(/" || p == "[";
    };
    size_t i = 0;
    while (i < n) {
        char c = text[i];
        if (IsBlank(c)) {
            i++;
            continue;
        }
        size_t b = i;
        if (IsLetter(c)) {
            while (i < n && IsNameChar(text[i])) i++;
            // kind-prefixed string: c_char_'x'
            if (i < n && (text[i] == '\'' || text[i] == '"') && text[i - 1] == '_') {
                char q = text[i++];
                while (i < n) {
                    if (text[i] == q) {
                        if (i + 1 < n && text[i + 1] == q) {
                            i += 2;
                            continue;
                        }
                        i++;
                        break;
                    }
                    i++;
                }
                push(TokKind::String, b, i);
                continue;
            }
            // BOZ: b'101' o'7' z'ff' x'ff'
            if (i == b + 1 && i < n && (text[i] == '\'' || text[i] == '"')) {
                char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (l == 'b' || l == 'o' || l == 'z' || l == 'x') {
                    char q = text[i++];
                    while (i < n && text[i] != q) i++;
                    if (i < n) i++;
                    push(TokKind::Boz, b, i);
                    continue;
                }
            }
            push(TokKind::Name, b, i);
            continue;
        }
        if (IsDigit(c) || (c == '.' && i + 1 < n && IsDigit(text[i + 1]))) {
            bool real = false;
            while (i < n && IsDigit(text[i])) i++;
            // Hollerith: 5Hhello
            if (i < n && (text[i] == 'h' || text[i] == 'H') && i > b && hollerith_ok()) {
                long len = std::strtol(text.substr(b, i - b).c_str(), nullptr, 10);
                size_t e = i + 1 + static_cast<size_t>(std::max(0L, len));
                if (len > 0 && e <= n) {
                    push(TokKind::Hollerith, b, e);
                    i = e;
                    continue;
                }
            }
            if (i < n && text[i] == '.') {
                // 1.eq.2 is 1 .eq. 2; 1.e5 / 1.d0 are reals; 1. is a real
                size_t j = i + 1;
                while (j < n && IsLetter(text[j])) j++;
                bool dotop = j > i + 1 && j < n && text[j] == '.' && (IsDotOperatorWord(Lower(text.substr(i + 1, j - i - 1))) ||
                                                                       !(std::tolower(text[i + 1]) == 'e' || std::tolower(text[i + 1]) == 'd' || std::tolower(text[i + 1]) == 'q') ||
                                                                       j - i - 1 > 1);
                if (!dotop) {
                    real = true;
                    i++;
                    while (i < n && IsDigit(text[i])) i++;
                }
            }
            if (i < n && (text[i] == 'e' || text[i] == 'E' || text[i] == 'd' || text[i] == 'D' || text[i] == 'q' ||
                          text[i] == 'Q')) {
                size_t j = i + 1;
                if (j < n && (text[j] == '+' || text[j] == '-')) j++;
                if (j < n && IsDigit(text[j])) {
                    real = true;
                    i = j;
                    while (i < n && IsDigit(text[i])) i++;
                }
            }
            // kind: 1_8, 1.0_dp
            if (i + 1 < n && text[i] == '_' && (IsNameChar(text[i + 1]))) {
                size_t j = i + 1;
                while (j < n && IsNameChar(text[j])) j++;
                if (j < n && (text[j] == '\'' || text[j] == '"') && !real) {
                    // 1_'abc'
                    char q = text[j];
                    i = j + 1;
                    while (i < n) {
                        if (text[i] == q) {
                            if (i + 1 < n && text[i + 1] == q) {
                                i += 2;
                                continue;
                            }
                            i++;
                            break;
                        }
                        i++;
                    }
                    push(TokKind::String, b, i);
                    continue;
                }
                i = j;
            } else if (i + 1 < n && text[i] == '_' && (text[i + 1] == '\'' || text[i + 1] == '"') && !real) {
                char q = text[i + 1];
                i += 2;
                while (i < n) {
                    if (text[i] == q) {
                        if (i + 1 < n && text[i + 1] == q) {
                            i += 2;
                            continue;
                        }
                        i++;
                        break;
                    }
                    i++;
                }
                push(TokKind::String, b, i);
                continue;
            }
            push(real ? TokKind::Real : TokKind::Int, b, i);
            continue;
        }
        if (c == '.') {
            size_t j = i + 1;
            while (j < n && IsLetter(text[j])) j++;
            if (j > i + 1 && j < n && text[j] == '.') {
                std::string w = Lower(text.substr(i + 1, j - i - 1));
                i = j + 1;
                if (w == "true" || w == "false") {
                    if (i + 1 < n && text[i] == '_' && IsNameChar(text[i + 1])) {
                        i++;
                        while (i < n && IsNameChar(text[i])) i++;
                    }
                    push(TokKind::Logical, b, i);
                } else {
                    push(TokKind::DotOp, b, i);
                }
                continue;
            }
            i++;
            push(TokKind::Op, b, i);
            continue;
        }
        if (c == '\'' || c == '"') {
            char q = c;
            i++;
            bool closed = false;
            while (i < n) {
                if (text[i] == q) {
                    if (i + 1 < n && text[i + 1] == q) {
                        i += 2;
                        continue;
                    }
                    i++;
                    closed = true;
                    break;
                }
                i++;
            }
            (void)closed;
            // 'ff'x suffix BOZ
            if (i < n && std::strchr("bBoOzZxX", text[i]) && (i + 1 >= n || !IsNameChar(text[i + 1]))) {
                bool hex = true;
                for (size_t k = b + 1; k + 1 < i; k++) hex = hex && std::isxdigit(static_cast<unsigned char>(text[k]));
                if (hex && i > b + 2) {
                    i++;
                    push(TokKind::Boz, b, i);
                    continue;
                }
            }
            push(TokKind::String, b, i);
            continue;
        }
        auto two = [&](const char *op) { return i + 1 < n && text[i] == op[0] && text[i + 1] == op[1]; };
        if (two("**") || two("//") || two("==") || two("/=") || two("<=") || two(">=") || two("=>") || two("::")) {
            // `(//)` and `(/=)` in a generic spec; `a//b`
            i += 2;
            push(TokKind::Op, b, i);
            continue;
        }
        if (two("(/")) {
            char after = i + 2 < n ? text[i + 2] : '\0';
            size_t k = i + 2;
            while (k < n && IsBlank(text[k])) k++;
            char nb = k < n ? text[k] : '\0';
            if (after != '/' && after != '=' && nb != ')') {
                i += 2;
                push(TokKind::Op, b, i);
                continue;
            }
        }
        if (two("/)") && !(out.size() && out.back().text == "(")) {
            i += 2;
            push(TokKind::Op, b, i);
            continue;
        }
        i++;
        push(TokKind::Op, b, i);
    }
    (void)diags;
    return out;
}

std::vector<Token> TokenizeText(const std::string &text, Pos at, Pos end) {
    std::vector<Pos> pos(text.size(), at);
    std::vector<Token> toks = Tokenize(text, pos, nullptr);
    for (Token &t : toks) {
        t.pos = at;
        t.end = end;
    }
    return toks;
}

// ---- Read --------------------------------------------------------------------------

std::shared_ptr<Source> Read(const std::vector<std::string> &lines, const ReadOptions &opts) {
    auto src = std::make_shared<Source>();
    src->form = opts.form;
    Preprocessor pp(src.get());
    pp.SealPredefined();
    Assembler as(src.get(), opts);
    bool any_directive = false;
    for (const std::string &l : lines) {
        size_t k = 0;
        while (k < l.size() && IsBlank(l[k])) k++;
        if (k < l.size() && l[k] == '#') {
            any_directive = true;
            break;
        }
    }
    bool preprocess = opts.preprocess || any_directive;
    int nlines = static_cast<int>(lines.size());
    for (int li = 0; li < nlines; li++) {
        const std::string &l = lines[static_cast<size_t>(li)];
        size_t k = 0;
        while (k < l.size() && IsBlank(l[k])) k++;
        if (k < l.size() && l[k] == '#' && preprocess) {
            // A directive; backslash-newline continues it.
            std::string text = l.substr(k);
            int end = li;
            while (!text.empty() && text.back() == '\\' && end + 1 < nlines) {
                text.pop_back();
                end++;
                text += lines[static_cast<size_t>(end)];
            }
            pp.Directive(text, li, end);
            for (int x = li + 1; x <= end; x++) {
                if (!pp.Active()) src->inactive_lines.push_back(x);
            }
            li = end;
            continue;
        }
        if (!pp.Active()) {
            src->inactive_lines.push_back(li);
            continue;
        }
        // INCLUDE 'file' lines (not a statement: the reader doesn't open files).
        // gfortran takes one starting in any column, fixed form included.
        {
            size_t j = 0;
            while (j < l.size() && IsBlank(l[j])) j++;
            bool comment_line = opts.form == Form::Fixed && !l.empty() &&
                                (l[0] == 'c' || l[0] == 'C' || l[0] == '*' || l[0] == 'd' || l[0] == 'D') &&
                                !(l.size() >= 7 && Lower(l.substr(0, 7)) == "include");
            if (!comment_line && l.size() >= j + 7 && Lower(l.substr(j, 7)) == "include") {
                size_t q = j + 7;
                while (q < l.size() && IsBlank(l[q])) q++;
                if (q < l.size() && (l[q] == '\'' || l[q] == '"')) {
                    const char quote = l[q];
                    size_t e = l.find(quote, q + 1);
                    size_t r = e == std::string::npos ? l.size() : e + 1;
                    while (r < l.size() && IsBlank(l[r])) r++;
                    if (e != std::string::npos && (r >= l.size() || l[r] == '!')) {
                        src->unresolved_include = true;
                        if (r < l.size()) {
                            fortran::Comment cm;
                            cm.pos = {li, static_cast<int>(r)};
                            cm.end = {li, static_cast<int>(l.size())};
                            cm.text = l.substr(r);
                            src->comments.push_back(cm);
                        }
                        continue;  // the whole line is the INCLUDE
                    }
                }
            }
        }
        if (opts.form == Form::Fixed) {
            as.FixedLine(l, li);
        } else {
            as.FreeLine(l, li);
        }
    }
    as.End();
    pp.Finish(nlines);
    src->macros = pp.UserMacros();

    // Tokens and statements.
    for (RawStmt &r : as.stmts) {
        Stmt st;
        st.label = r.label;
        st.label_pos = r.label_pos;
        st.toks = Tokenize(r.text, r.pos, &src->diags);
        pp.Expand(&st.toks);
        if (r.unterminated_string) {
            Diag d;
            d.pos = r.string_pos;
            d.end = r.pos.empty() ? r.string_pos : Pos{r.pos.back().line, r.pos.back().col + 1};
            d.message = "Unterminated character constant beginning at (1)";
            d.code = "syntax-error";
            src->diags.push_back(d);
        }
        if (!r.pos.empty()) {
            st.pos = r.pos.front();
            st.end = {r.pos.back().line, r.pos.back().col + 1};
        } else {
            st.pos = st.end = r.label_pos;
        }
        // A macro whose body holds `;` expands to several statements.
        bool semi = false;
        for (const Token &t : st.toks) semi = semi || (t.kind == TokKind::Op && t.text == ";");
        if (!semi) {
            src->stmts.push_back(std::move(st));
            continue;
        }
        std::vector<Token> all = std::move(st.toks);
        st.toks.clear();
        bool first = true;
        auto flush = [&]() {
            if (st.toks.empty()) return;
            Stmt part = st;
            if (!first) part.label.clear();
            part.pos = part.toks.front().pos;
            part.end = part.toks.back().end;
            src->stmts.push_back(std::move(part));
            st.toks.clear();
            first = false;
        };
        for (Token &t : all) {
            if (t.kind == TokKind::Op && t.text == ";") {
                flush();
            } else {
                st.toks.push_back(std::move(t));
            }
        }
        flush();
    }
    ParseStatements(src.get());
    return src;
}

}  // namespace fortran
