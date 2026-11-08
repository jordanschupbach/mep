// The checks fish runs over a parsed script before evaluating it: argument
// escapes and `$` uses, command substitutions (recursively), backgrounded
// jobs, decorated statements (pipelines, break/continue, builtin names,
// what can be a command) and arguments after `end` / `}`. See fish_parse.h.
#include <algorithm>

#include "fish_parse.h"

namespace fishparse {

namespace {

using u32 = std::u32string;
using u32v = std::u32string_view;

std::string Truncate16(u32v s) {
    if (s.size() <= 16) return ToUtf8(s);
    return ToUtf8(s.substr(0, 15)) + "\xE2\x80\xA6";
}

// What a special character prints as in a message.
char32_t Printable(char32_t c) {
    switch (c) {
    case kBraceSpace: return ' ';
    case kBraceBegin: return '{';
    case kBraceEnd: return '}';
    case kBraceSep: return ',';
    case kHomeDirectory: return '~';
    case kAnyChar: return '?';
    case kAnyString: case kAnyStringRecursive: return '*';
    case kInternalSeparator: return 0;
    default: return c;
    }
}

void Add(std::vector<ParseError> *errs, size_t start, size_t len, std::string text) {
    errs->push_back(ParseError{start, len, std::move(text)});
}

std::string ErrorForCharacter(char32_t c) {
    switch (c) {
    case '?': return "$? is not the exit status. In fish, please use $status.";
    case '#': return "$# is not supported. In fish, please use 'count $argv'.";
    case '@': return "$@ is not supported. In fish, please use $argv.";
    case '*': return "$* is not supported. In fish, please use $argv.";
    case '$': case kVariableExpand: case kVariableExpandSingle: case kVariableExpandEmpty:
        return "$$ is not the pid. In fish, please use $fish_pid.";
    case kBraceEnd: case '}': case ',': case kBraceSep:
        return "Expected a variable name after this $.";
    default: {
        const char32_t p = Printable(c);
        return "$" + ToUtf8(u32(1, p)) + " is not a valid variable in fish.";
    }
    }
}

void ExpandVariableError(u32v token, size_t global_pos, size_t dollar_pos, std::vector<ParseError> *errs) {
    const bool double_quotes = token[dollar_pos] == kVariableExpandSingle;
    const size_t global_dollar = global_pos + dollar_pos;
    const size_t after = global_dollar + 1;
    const char32_t c = dollar_pos + 1 < token.size() ? token[dollar_pos + 1] : U'\0';
    if (c == kBraceBegin || c == '{') {
        const char32_t closer = c == '{' ? U'}' : kBraceEnd;
        bool looks = false;
        u32v name;
        for (size_t i = dollar_pos + 2; i < token.size(); i++) {
            if (token[i] == closer) {
                name = token.substr(dollar_pos + 2, i - dollar_pos - 2);
                looks = ValidVarName(name);
                break;
            }
        }
        if (looks) {
            if (double_quotes)
                Add(errs, after, 1, "Variables cannot be bracketed. In fish, please use \"$" + Truncate16(name) + "\".");
            else
                Add(errs, after, 1, "Variables cannot be bracketed. In fish, please use {$" + Truncate16(name) + "}.");
        } else {
            Add(errs, after, 1, "${ is not a valid variable in fish.");
        }
    } else if (c == kInternalSeparator || c == '\0') {
        Add(errs, global_dollar, 1, "Expected a variable name after this $.");
    } else {
        char32_t stop = c;
        if (stop == kAnyChar) stop = '?';
        else if (stop == kAnyString || stop == kAnyStringRecursive) stop = '*';
        Add(errs, after, 1, ErrorForCharacter(stop));
    }
}

bool IsVarExpand(char32_t c) { return c == kVariableExpand || c == kVariableExpandSingle; }

// --- arguments ---

bool CheckSubtoken(u32v arg_src, size_t source_start, size_t begin, size_t end, std::vector<ParseError> *errs) {
    u32 unesc;
    if (!Unescape(arg_src.substr(begin, end - begin), true, false, &unesc)) {
        if (arg_src.size() == 2 && arg_src[0] == '\\' &&
            (arg_src[1] == 'c' || arg_src[1] == 'u' || arg_src[1] == 'U' || arg_src[1] == 'x' || arg_src[1] == 'X')) {
            Add(errs, source_start + begin, end - begin, "Incomplete escape sequence '" + ToUtf8(arg_src) + "'");
        } else {
            Add(errs, source_start + begin, end - begin, "Invalid token '" + ToUtf8(arg_src) + "'");
        }
        return true;
    }
    bool errored = false;
    for (size_t idx = 0; idx < unesc.size(); idx++) {
        if (!IsVarExpand(unesc[idx])) continue;
        const char32_t next = idx + 1 < unesc.size() ? unesc[idx + 1] : U'\0';
        if (!IsVarExpand(next) && next != '(' && !ValidVarNameChar(next)) {
            errored = true;
            size_t first = idx;
            while (first > 0 && IsVarExpand(unesc[first - 1])) first--;
            ExpandVariableError(unesc, source_start, first, errs);
        }
    }
    return errored;
}

std::vector<ParseError> DetectParseErrors(u32v src);

bool ArgumentErrors(u32v arg_src, size_t source_start, std::vector<ParseError> *errs) {
    bool error = false;
    size_t cursor = 0, checked = 0;
    bool is_quoted = false;
    for (;;) {
        bool has_dollar = false;
        Cmdsub cs;
        const int r = LocateCmdsub(arg_src, &cursor, false, &is_quoted, &has_dollar, &cs);
        if (r < 0) {
            Add(errs, source_start, 1, "Mismatched parenthesis");
            return true;
        }
        if (r == 0) break;
        error |= CheckSubtoken(arg_src, source_start, checked, cs.start - (has_dollar ? 1 : 0), errs);
        std::vector<ParseError> sub = DetectParseErrors(arg_src.substr(cs.CommandStart(), cs.CommandEnd() - cs.CommandStart()));
        if (!sub.empty()) error = true;
        const size_t off = cs.start + 1 + source_start;
        for (ParseError &e : sub) {
            if (e.start != kNoLocation) e.start += off;
            errs->push_back(std::move(e));
        }
        checked = cs.end;
    }
    error |= CheckSubtoken(arg_src, source_start, checked, arg_src.size(), errs);
    return error;
}

// --- commands ---

bool IsClean(u32v s) {
    if (s.empty()) return true;
    if (s[0] == '~' || s[0] == '%') return false;
    for (char32_t c : s)
        if (c == '$' || c == '*' || c == '?' || c == '\\' || c == '"' || c == '\'' || c == '(' || c == '{' ||
            c == '}' || c == ')')
            return false;
    return true;
}

// Reads an integer like fish's wcstoi_partial; false if there are no digits.
bool ReadInt(u32v s, size_t pos, long long *val, size_t *consumed) {
    size_t i = pos;
    while (i < s.size() && IsWhitespace(s[i])) i++;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        neg = s[i] == '-';
        i++;
    }
    const size_t digits = i;
    long long v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (v < 100000000000LL) v = v * 10 + static_cast<long long>(s[i] - '0');
        i++;
    }
    if (i == digits) return false;
    *val = neg ? -v : v;
    *consumed = i - pos;
    return true;
}

// fish's slice syntax check for a missing variable; returns false with the error position.
bool CheckSlice(u32v in, size_t *bad, bool *zero) {
    auto at = [&](size_t i) { return i < in.size() ? in[i] : U'\0'; };
    size_t pos = 1;
    bool first = true;
    for (;;) {
        while (IsWhitespace(at(pos)) || at(pos) == kInternalSeparator) pos++;
        if (at(pos) == ']') return true;
        if (at(pos) == '\0') {
            *bad = pos;
            *zero = false;
            return false;
        }
        if (!(first && at(pos) == '.' && at(pos + 1) == '.')) {
            long long v = 0;
            size_t used = 0;
            if (!ReadInt(in, pos, &v, &used)) {
                *bad = pos, *zero = false;
                return false;
            }
            if (v == 0) {
                *bad = pos, *zero = true;
                return false;
            }
            pos += used;
            while (IsWhitespace(at(pos))) pos++;
        }
        first = false;
        while (at(pos) == kInternalSeparator) pos++;
        if (at(pos) == '.' && at(pos + 1) == '.') {
            pos += 2;
            while (at(pos) == kInternalSeparator) pos++;
            while (IsWhitespace(at(pos))) pos++;
            if (at(pos) != ']') {
                long long v = 0;
                size_t used = 0;
                if (!ReadInt(in, pos, &v, &used)) {
                    *bad = pos, *zero = false;
                    return false;
                }
                if (v == 0) {
                    *bad = pos, *zero = true;
                    return false;
                }
                pos += used;
                while (IsWhitespace(at(pos))) pos++;
            }
        }
    }
}

// Expanding a command with no variables set and no command substitutions
// allowed: what fish's checker reports. @p out_cmd gets the expanded word.
bool CommandExpandErrors(u32v com, size_t com_start, std::vector<ParseError> *errs, u32 *out_cmd) {
    if (IsClean(com)) {
        *out_cmd = u32(com);
        return false;
    }
    size_t cursor = 0;
    Cmdsub cs;
    bool q = false, d = false;
    (void)q, (void)d;
    const int r = LocateCmdsub(com, &cursor, true, nullptr, nullptr, &cs);
    if (r < 0) return true;
    if (r > 0) {
        Add(errs, com_start + cs.start, cs.end - cs.start,
            "command substitutions not allowed in command position. Try var=(your-cmd) $var ...");
        return true;
    }
    u32 instr;
    if (!Unescape(com, true, true, &instr)) instr.clear();
    size_t last = instr.size();
    bool any_var = false;
    for (;;) {
        size_t i = last;
        bool found = false;
        while (i > 0) {
            i--;
            if (IsVarExpand(instr[i])) {
                found = true;
                break;
            }
        }
        if (!found) break;
        any_var = true;
        size_t stop = i + 1;
        while (stop < instr.size()) {
            if (instr[stop] == kVariableExpandEmpty) {
                stop++;
                break;
            }
            if (!ValidVarNameChar(instr[stop])) break;
            stop++;
        }
        if (stop == i + 1) {
            ExpandVariableError(instr, com_start, i, errs);
            return true;
        }
        if (stop < instr.size() && instr[stop] == '[') {
            size_t bad = 0;
            bool zero = false;
            if (!CheckSlice(u32v(instr).substr(stop), &bad, &zero)) {
                Add(errs, com_start + stop + bad, 0, zero ? "array indices start at 1, not 0." : "Invalid index value");
                return true;
            }
        }
        if (instr[i] == kVariableExpand) break;  // an unset variable: the word expands to nothing
        last = i;
    }
    if (any_var) {
        out_cmd->clear();
        return false;
    }
    u32 plain;
    if (!Unescape(com, false, true, &plain)) plain.clear();
    *out_cmd = plain;
    return false;
}

// --- the tree walk ---

class Checker {
public:
    Checker(const Ast &ast, u32v src, std::vector<ParseError> *errs) : ast_(ast), src_(src), errs_(errs) {}

    void Walk(int n) {
        Visit(n);
        for (int k : ast_[n].kids) Walk(k);
    }

private:
    Range R(int n) const {
        Range r;
        ast_.RangeOf(n, &r);
        return r;
    }
    u32v Text(int n) const {
        Range r;
        if (!ast_.RangeOf(n, &r)) return {};
        return src_.substr(r.start, r.len);
    }
    int Parent(int n) const { return ast_[n].parent; }

    void Visit(int n) {
        const Node &node = ast_[n];
        switch (node.kind) {
        case Kind::JobConjunction: JobConjunction(n); break;
        case Kind::Argument: {
            if (!node.sourced) break;
            ArgumentErrors(src_.substr(node.start, node.len), node.start, errs_);
            break;
        }
        case Kind::JobPipeline:
            if (ast_.Field(n, Role::Bg) >= 0) Backgrounded(n);
            break;
        case Kind::DecoratedStatement: Decorated(n); break;
        case Kind::BlockStatement: case Kind::BraceStatement: case Kind::IfStatement: case Kind::SwitchStatement:
            RedirectionList(n);
            break;
        default: break;
        }
    }

    void JobConjunction(int n) {
        std::vector<int> jobs{ast_.Field(n, Role::Job)};
        std::vector<int> conts;
        for (int c : ast_[ast_.Field(n, Role::Continuations)].kids) {
            conts.push_back(c);
            jobs.push_back(ast_.Field(c, Role::Job));
        }
        for (size_t i = 0; i < conts.size(); i++) {
            if (ast_.Field(jobs[i], Role::Bg) >= 0) {
                const int conj = ast_.Field(conts[i], Role::Conjunction);
                const Range r = R(conj);
                const bool andand = ast_[conj].tok == PTok::AndAnd;
                Add(errs_, r.start, r.len,
                    std::string("The '") + (andand ? "&&" : "||") +
                        "' command can not be used immediately after a backgrounded job");
                return;
            }
        }
    }

    void Backgrounded(int job) {
        Range jr;
        if (!ast_.RangeOf(job, &jr)) return;
        const int jc = Parent(job);
        if (ast_[jc].kind != Kind::JobConjunction) return;
        const int jcp = Parent(jc);
        const Kind pk = ast_[jcp].kind;
        if (pk == Kind::IfClause || pk == Kind::WhileHeader) {
            Add(errs_, jr.start, jr.len, "Backgrounded commands can not be used as conditionals");
        } else if (pk == Kind::JobList) {
            const auto &kids = ast_[jcp].kids;
            const auto it = std::find(kids.begin(), kids.end(), jc);
            if (it != kids.end() && it + 1 != kids.end()) {
                const int deco = ast_.Field(*(it + 1), Role::Decorator);
                if (deco >= 0) {
                    const Range r = R(deco);
                    Add(errs_, r.start, r.len,
                        std::string("The '") + (ast_[deco].kw == Kw::And ? "and" : "or") +
                            "' command can not be used immediately after a backgrounded job");
                }
            }
        }
    }

    void Decorated(int dst) {
        const Range dr = R(dst);
        const int deco_node = ast_.Field(dst, Role::OptDecoration);
        const Kw deco = deco_node >= 0 ? ast_[deco_node].kw : Kw::None;
        bool first_arg_is_help = false;
        for (int item : ast_[ast_.Field(dst, Role::ArgsOrRedirs)].kids) {
            const int e = ast_[item].kids.empty() ? -1 : ast_[item].kids[0];
            if (e >= 0 && ast_[e].kind == Kind::Argument) {
                const u32v t = Text(e);
                first_arg_is_help = t == U"-h" || t == U"--help";
                break;
            }
        }
        const int st = Parent(dst);
        int job = Parent(st);
        while (job >= 0 && ast_[job].kind != Kind::JobPipeline) job = Parent(job);
        enum { None, First, Subsequent } pipe_pos = None;
        if (job >= 0 && !ast_[ast_.Field(job, Role::Continuation)].kids.empty())
            pipe_pos = ast_.Field(job, Role::Statement) == st ? First : Subsequent;
        const bool in_pipeline = pipe_pos != None;
        bool errored = false;
        if (in_pipeline && deco == Kw::Exec) {
            Add(errs_, dr.start, dr.len, "The 'exec' command can not be used in a pipeline");
            errored = true;
        }
        const int cmd_node = ast_.Field(dst, Role::Command);
        const u32v com = Text(cmd_node);
        if (pipe_pos == Subsequent && deco == Kw::None) {
            if (com == U"and" || com == U"or") {
                Add(errs_, dr.start, dr.len, "The '" + ToUtf8(com) + "' command can not be used in a pipeline");
                errored = true;
            }
            if (com == U"time") {
                Add(errs_, dr.start, dr.len, "The 'time' command may only be at the beginning of a pipeline");
                errored = true;
            }
        }
        if (com == U"$status") {
            Add(errs_, dr.start, dr.len, "$status is not valid as a command. See `help language#conditions`");
            errored = true;
        }
        if (com.empty()) return;
        std::vector<ParseError> new_errors;
        u32 command;
        if (CommandExpandErrors(com, R(cmd_node).start, &new_errors, &command)) errored = true;
        static const char32_t *const kNoPipe[] = {U"exec", U"case", U"break", U"return", U"continue"};
        bool forbidden = false;
        for (const char32_t *w : kNoPipe)
            if (command == w) forbidden = true;
        if (!errored && forbidden && in_pipeline) {
            Add(errs_, dr.start, dr.len, "The '" + ToUtf8(command) + "' command can not be used in a pipeline");
            errored = true;
        }
        if (!errored && (command == U"break" || command == U"continue") && !first_arg_is_help) {
            bool found = false;
            for (int a = Parent(dst); a >= 0; a = Parent(a)) {
                if (ast_[a].kind != Kind::BlockStatement) continue;
                const int h = ast_[ast_.Field(a, Role::Header)].kids[0];
                const Kind hk = ast_[h].kind;
                if (hk == Kind::ForHeader || hk == Kind::WhileHeader) {
                    found = true;
                    break;
                }
                if (hk == Kind::FunctionHeader) break;
            }
            if (!found) {
                Add(errs_, dr.start, dr.len,
                    command == U"break" ? "'break' while not inside of loop" : "'continue' while not inside of loop");
                errored = true;
            }
        }
        if (!errored && deco == Kw::Builtin) {
            bool expands = true;
            for (char32_t c : com)
                if (c == '$' || c == '(' || c == '*' || c == '{') expands = false;
            if (expands && !IsBuiltin(com)) {
                Add(errs_, dr.start, dr.len, "Unknown builtin '" + ToUtf8(com) + "'");
                errored = true;
            }
        }
        for (ParseError &e : new_errors) errs_->push_back(std::move(e));
    }

    void RedirectionList(int n) {
        const int list = ast_.Field(n, Role::ArgsOrRedirs);
        if (list < 0) return;
        for (int item : ast_[list].kids) {
            const int e = ast_[item].kids.empty() ? -1 : ast_[item].kids[0];
            if (e >= 0 && ast_[e].kind == Kind::Argument) {
                const Range r = R(e);
                Add(errs_, r.start, r.len,
                    ast_[n].kind == Kind::BraceStatement ? "'}' does not take arguments. Did you forget a ';'?"
                                                         : "'end' does not take arguments. Did you forget a ';'?");
                return;
            }
        }
    }

    const Ast &ast_;
    u32v src_;
    std::vector<ParseError> *errs_;
};

std::vector<ParseError> DetectParseErrors(u32v src) {
    Ast ast = Parse(src, ParseFlags{});
    if (ast.any_error) return ast.parse_errors;
    std::vector<ParseError> errs;
    Checker(ast, src, &errs).Walk(0);
    return errs;
}

}  // namespace

std::vector<ParseError> DetectErrors(std::u32string_view src) { return DetectParseErrors(src); }

}  // namespace fishparse
