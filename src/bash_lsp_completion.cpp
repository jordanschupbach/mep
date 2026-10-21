// mep-bash-lsp: completion, hover and signature help.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

#include "bash_lsp.h"
#include "bash_lsp_index.h"

using bash_lsp::BuildIndex;
using bash_lsp::FunctionDef;
using bash_lsp::Index;
using bash_lsp::Occurrence;
using bash_lsp::OccKind;
using bash_lsp::PlainWord;

namespace {

bool IsNameChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// Characters of a shell word, for finding the token under the cursor.
bool IsWordChar(char c) { return c != 0 && std::strchr(" \t;&|()<>\"'`$={}", c) == nullptr; }

bool StartsWith(const std::string &s, const std::string &prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

const std::string &LineAt(const std::vector<std::string> &lines, int line) {
    static const std::string kEmpty;
    return line >= 0 && static_cast<size_t>(line) < lines.size() ? lines[static_cast<size_t>(line)] : kEmpty;
}

// The tokens of a line's text before the cursor, split the way the shell
// would at the top level (good enough for "which command am I in").
struct LineContext {
    std::vector<std::string> words;  // the current command's words before the token being typed
    std::string token;               // the token being typed
    int token_start = 0;
    bool command_position = true;    // the token is the command name
    bool in_test = false;            // inside [[ ]] or [ ]
};

LineContext ContextAt(const std::string &line, int col) {
    LineContext ctx;
    const size_t end = std::min(static_cast<size_t>(std::max(col, 0)), line.size());
    size_t start = end;
    while (start > 0 && IsWordChar(line[start - 1])) start--;
    ctx.token = line.substr(start, end - start);
    ctx.token_start = static_cast<int>(start);
    // Words since the last command separator.
    std::vector<std::string> words;
    std::string cur;
    char quote = 0;
    bool test = false;
    auto flush = [&]() {
        if (!cur.empty()) words.push_back(cur);
        cur.clear();
    };
    for (size_t k = 0; k < start; k++) {
        const char c = line[k];
        if (quote) {
            if (c == quote) quote = 0;
            cur += c;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            cur += c;
            continue;
        }
        if (c == '#' && cur.empty()) break;  // a comment
        if (c == ' ' || c == '\t') {
            flush();
            continue;
        }
        if (std::strchr(";&|(){`", c) != nullptr) {
            flush();
            if (c == '(' && k > 0 && line[k - 1] == '$') words.clear();
            words.clear();
            continue;
        }
        cur += c;
    }
    flush();
    // Reserved words that start a new command.
    static const char *const starters[] = {"then", "do", "else", "elif", "if", "while", "until", "!", "time", "{"};
    size_t first = 0;
    while (first < words.size()) {
        bool starter = false;
        for (const char *s : starters) {
            if (words[first] == s) starter = true;
        }
        if (!starter) break;
        first++;
    }
    ctx.words.assign(words.begin() + static_cast<long>(first), words.end());
    for (const std::string &w : ctx.words) {
        if (w == "[[" || w == "[" || w == "test") test = true;
        if (w == "]]" || w == "]") test = false;
    }
    ctx.in_test = test;
    ctx.command_position = ctx.words.empty();
    return ctx;
}

// --- Executables on PATH --------------------------------------------------

std::vector<std::string> Executables(const std::vector<std::string> &dirs, const std::string &prefix) {
    struct Cached {
        std::chrono::steady_clock::time_point at;
        std::vector<std::string> names;
    };
    static std::mutex mu;
    static std::map<std::string, Cached> cache;
    const auto now = std::chrono::steady_clock::now();
    std::set<std::string> out;
    std::lock_guard<std::mutex> lock(mu);
    for (const std::string &dir : dirs) {
        if (dir.empty()) continue;
        Cached &c = cache[dir];
        if (c.names.empty() || now - c.at > std::chrono::seconds(30)) {
            c.names.clear();
            c.at = now;
            std::error_code ec;
            for (std::filesystem::directory_iterator it(dir, ec), last; !ec && it != last; it.increment(ec)) {
                std::error_code ec2;
                const auto st = it->status(ec2);
                if (ec2 || !std::filesystem::is_regular_file(st)) continue;
                using P = std::filesystem::perms;
                if ((st.permissions() & (P::owner_exec | P::group_exec | P::others_exec)) == P::none) continue;
                c.names.push_back(it->path().filename().string());
            }
        }
        for (const std::string &n : c.names) {
            if (StartsWith(n, prefix)) out.insert(n);
        }
    }
    return std::vector<std::string>(out.begin(), out.end());
}

// --- The file's functions -------------------------------------------------

// The comment block just above a definition, without its `#`s.
std::string CommentAbove(const Index &ix, int line) {
    std::vector<std::string> block;
    for (int k = line - 1; k >= 0; k--) {
        const std::string &s = LineAt(ix.lines, k);
        const int f = bash_lsp::FirstNonBlank(s);
        if (f < 0 || s[static_cast<size_t>(f)] != '#' || (k == 0 && ix.has_shebang)) break;
        std::string t = s.substr(static_cast<size_t>(f) + 1);
        if (!t.empty() && t[0] == ' ') t.erase(0, 1);
        block.push_back(t);
    }
    std::reverse(block.begin(), block.end());
    std::string out;
    for (const std::string &b : block) {
        if (!out.empty()) out += "\n";
        out += b;
    }
    return out;
}

// A function's positional parameters, named where its body names them:
// `local name=$1`, `x="${2:-default}"`. "$@" makes a trailing "args...".
struct Params {
    std::vector<std::string> names;  // index 0 is $1
    bool rest = false;
};

class ParamFinder : public BashVisitor {
public:
    Params p;
    void Note(int n, const std::string &name) {
        if (n < 1 || n > 9) return;
        if (static_cast<int>(p.names.size()) < n) p.names.resize(static_cast<size_t>(n));
        std::string &slot = p.names[static_cast<size_t>(n - 1)];
        if (slot.empty() || slot[0] == '$') slot = name;
    }
    bool Command(const BashCommand &c, const BashCommand *) override {
        for (const BashAssignment &a : c.assignments) {
            // name=$1, name="$1", name=${1:-x}, name="${1:-x}"
            const std::vector<BashWordPart> *parts = &a.value.parts;
            if (parts->size() == 1 && (*parts)[0].kind == BashPartKind::DoubleQuoted) parts = &(*parts)[0].parts;
            if (parts->size() == 1 && (*parts)[0].kind == BashPartKind::Param) {
                const std::string &n = (*parts)[0].name;
                if (n.size() == 1 && n[0] >= '1' && n[0] <= '9') {
                    std::string name = a.name;
                    if ((*parts)[0].op == ":-" || (*parts)[0].op == "-") name = "[" + name + "]";
                    Note(n[0] - '0', name);
                }
            }
        }
        return true;
    }
    void Part(const BashWordPart &part, const BashCommand *) override {
        if (part.kind != BashPartKind::Param) return;
        if (part.name == "@" || part.name == "*") p.rest = true;
        if (part.name.size() == 1 && part.name[0] >= '1' && part.name[0] <= '9') {
            Note(part.name[0] - '0', "$" + part.name);
        }
    }
};

Params ParamsOf(const BashCommand &fn) {
    ParamFinder pf;
    for (const BashCommand &body : fn.sub) {
        BashList l;
        BashAndOr ao;
        BashPipeline pl;
        pl.commands.push_back(body);
        ao.pipelines.push_back(pl);
        l.items.push_back(ao);
        BashWalk(l, pf);
    }
    for (size_t k = 0; k < pf.p.names.size(); k++) {
        if (pf.p.names[k].empty()) pf.p.names[k] = "$" + std::to_string(k + 1);
    }
    return pf.p;
}

std::string FunctionSignature(const Index &ix, const FunctionDef &d, std::vector<std::string> *params) {
    const Params p = ParamsOf(*d.cmd);
    std::string label = d.name;
    for (const std::string &n : p.names) {
        label += " " + n;
        if (params) params->push_back(n);
    }
    if (p.rest) {
        label += " [args...]";
        if (params) params->push_back("[args...]");
    }
    (void)ix;
    return label;
}

const FunctionDef *FindFunction(const Index &ix, const std::string &name) {
    for (const FunctionDef &d : ix.functions) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

// --- The parameter expansion under the cursor -----------------------------

class ParamAt : public BashVisitor {
public:
    ParamAt(int line, int col) : line_(line), col_(col) {}
    const BashWordPart *found = nullptr;
    void Part(const BashWordPart &p, const BashCommand *) override {
        if (p.kind != BashPartKind::Param || !p.braced || !BashRangeContains(p.range, line_, col_)) return;
        if (!found || (p.range.end_line - p.range.line) * 10000 + (p.range.end_col - p.range.col) <
                          (found->range.end_line - found->range.line) * 10000 + (found->range.end_col - found->range.col)) {
            found = &p;
        }
    }

private:
    int line_, col_;
};

std::string Describe(const BashLspVocabEntry &e) {
    std::string s;
    if (e.synopsis && *e.synopsis) s = std::string(e.synopsis) + "\n\n";
    return s + e.doc;
}

BashRange TokenRange(int line, int a, int b) {
    BashRange r;
    r.line = r.end_line = line;
    r.col = a;
    r.end_col = b;
    return r;
}

}  // namespace

// --- Completion -----------------------------------------------------------

std::vector<BashLspCompletionItem> BashLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                      const std::vector<std::string> &path_dirs) {
    std::vector<BashLspCompletionItem> out;
    const std::string &text = LineAt(lines, line);
    const int c = std::min(col, static_cast<int>(text.size()));
    // After `$` or `${`: a variable.
    int s = c;
    while (s > 0 && IsNameChar(text[static_cast<size_t>(s - 1)])) s--;
    const std::string prefix = text.substr(static_cast<size_t>(s), static_cast<size_t>(c - s));
    int d = s;
    if (d > 0 && (text[static_cast<size_t>(d - 1)] == '!' || text[static_cast<size_t>(d - 1)] == '#') && d > 1 &&
        text[static_cast<size_t>(d - 2)] == '{') {
        d--;
    }
    const bool braced = d > 0 && text[static_cast<size_t>(d - 1)] == '{' && d > 1 && text[static_cast<size_t>(d - 2)] == '$';
    const bool dollar = braced || (d > 0 && text[static_cast<size_t>(d - 1)] == '$');
    const auto ix = BuildIndex(lines);
    auto add = [&](const std::string &label, BashLspKind kind, const std::string &detail, const std::string &doc,
                   int a, int b) {
        BashLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = doc;
        it.replace_start = a;
        it.replace_end = b;
        out.push_back(it);
    };
    if (dollar) {
        std::set<std::string> seen;
        // The function the cursor is in: its locals first.
        const BashCommand *here = nullptr;
        for (const FunctionDef &fd : ix->functions) {
            if (BashRangeContains(fd.cmd->range, line, c)) here = fd.cmd;
        }
        for (int pass = 0; pass < 2; pass++) {
            for (const Occurrence &o : ix->vars) {
                if (o.kind == OccKind::Read || seen.count(o.name) || !StartsWith(o.name, prefix)) continue;
                if (pass == 0 && (o.scope == nullptr || o.scope != here)) continue;
                if (pass == 1 && o.scope != nullptr && o.scope != here) continue;
                seen.insert(o.name);
                add(o.name, BashLspKind::Variable, o.scope ? "local" : "variable",
                    "assigned on line " + std::to_string(o.range.line + 1), s, c);
            }
        }
        for (const BashLspVocabEntry &e : BashLspVariableVocab()) {
            const std::string n = e.name;
            if (seen.count(n) || !StartsWith(n, prefix)) continue;
            if (!IsNameChar(n[0]) && !prefix.empty()) continue;
            if (!IsNameChar(n[0]) && braced) continue;
            seen.insert(n);
            add(n, BashLspKind::Constant, "shell variable", e.doc, s, c);
        }
        return out;
    }

    LineContext ctx = ContextAt(text, c);
    const int ts = ctx.token_start;
    // `set -o name`, `shopt -s name`.
    if (!ctx.words.empty() && ctx.words[0] == "set" && !ctx.words.empty() &&
        (ctx.words.back() == "-o" || ctx.words.back() == "+o")) {
        for (const BashLspVocabEntry &e : BashLspSetOptionVocab()) {
            if (StartsWith(e.name, ctx.token)) add(e.name, BashLspKind::Constant, "set -o", e.doc, ts, c);
        }
        return out;
    }
    if (!ctx.words.empty() && ctx.words[0] == "shopt" && (ctx.token.empty() || ctx.token[0] != '-')) {
        for (const BashLspVocabEntry &e : BashLspShoptVocab()) {
            if (StartsWith(e.name, ctx.token)) add(e.name, BashLspKind::Constant, "shopt", e.doc, ts, c);
        }
        return out;
    }
    if (!ctx.token.empty() && ctx.token[0] == '-') {
        if (ctx.in_test) {
            for (const BashLspVocabEntry &e : BashLspTestOperatorVocab()) {
                if (e.name[0] == '-' && StartsWith(e.name, ctx.token)) {
                    add(e.name, BashLspKind::Operator, e.synopsis, e.doc, ts, c);
                }
            }
            return out;
        }
        if (!ctx.words.empty()) {
            for (const BashLspOption &o : BashLspBuiltinOptions()) {
                if (ctx.words[0] != o.builtin || !StartsWith(o.option, ctx.token)) continue;
                add(o.option, BashLspKind::Constant, *o.arg ? std::string(o.option) + " " + o.arg : "", o.doc, ts, c);
            }
            return out;
        }
    }
    if (!ctx.command_position) return out;
    // A command: keywords, builtins, the file's functions, executables.
    std::set<std::string> seen;
    for (const BashLspVocabEntry &e : BashLspKeywordVocab()) {
        if (!StartsWith(e.name, ctx.token) || !IsNameChar(e.name[0])) continue;
        seen.insert(e.name);
        add(e.name, BashLspKind::Keyword, "keyword", Describe(e), ts, c);
    }
    for (const FunctionDef &fd : ix->functions) {
        if (seen.count(fd.name) || !StartsWith(fd.name, ctx.token)) continue;
        seen.insert(fd.name);
        add(fd.name, BashLspKind::Function, FunctionSignature(*ix, fd, nullptr), CommentAbove(*ix, fd.cmd->range.line),
            ts, c);
    }
    for (const BashLspVocabEntry &e : BashLspBuiltinVocab()) {
        if (seen.count(e.name) || !StartsWith(e.name, ctx.token)) continue;
        seen.insert(e.name);
        add(e.name, BashLspKind::Function, e.synopsis, e.doc, ts, c);
    }
    if (!ctx.token.empty() && !path_dirs.empty()) {
        int count = 0;
        for (const std::string &x : Executables(path_dirs, ctx.token)) {
            if (seen.count(x)) continue;
            if (++count > 200) break;
            seen.insert(x);
            add(x, BashLspKind::File, "command", "", ts, c);
        }
    }
    return out;
}

// --- Hover ----------------------------------------------------------------

BashLspHoverInfo BashLspHover(const std::vector<std::string> &lines, int line, int col) {
    BashLspHoverInfo h;
    const auto ix = BuildIndex(lines);
    const std::string &text = LineAt(ix->lines, line);
    if (col < 0 || static_cast<size_t>(col) > text.size()) return h;

    // An operator inside ${...}.
    ParamAt pa(line, col);
    BashWalk(ix->parse.program, pa);
    const BashWordPart *param = pa.found;
    const bool on_name = param && BashRangeTouches(param->name_range, line, col) && !param->name.empty();

    // A variable of this file, or one bash provides.
    if (const Occurrence *o = ix->VarAt(line, col)) {
        std::string s = (o->scope ? "local variable " : "variable ") + o->name;
        const auto info = ix->info.find({o->scope, o->name});
        if (info != ix->info.end()) {
            std::string attrs;
            if (info->second.array) attrs += " array";
            if (info->second.integer) attrs += " integer";
            if (info->second.readonly) attrs += " read-only";
            if (info->second.exported) attrs += " exported";
            if (info->second.nameref) attrs += " name-reference";
            if (!attrs.empty()) s += " (" + attrs.substr(1) + ")";
        }
        if (o->scope) {
            for (const FunctionDef &fd : ix->functions) {
                if (fd.cmd == o->scope) s += ", in function " + fd.name;
            }
        }
        const Occurrence *def = nullptr;
        for (const Occurrence &x : ix->vars) {
            if (x.name == o->name && x.scope == o->scope && x.kind != OccKind::Read) {
                if (!def || (def->kind != OccKind::Declare && x.kind == OccKind::Declare)) def = &x;
                if (def->kind == OccKind::Declare) break;
            }
        }
        if (def) {
            std::string src = LineAt(ix->lines, def->range.line);
            const int f = bash_lsp::FirstNonBlank(src);
            if (f > 0) src = src.substr(static_cast<size_t>(f));
            s += "\n\nline " + std::to_string(def->range.line + 1) + ": " + src;
        } else if (const BashLspVocabEntry *e = BashLspFind(BashLspVariableVocab(), o->name)) {
            s = "shell variable " + o->name + "\n\n" + e->doc;
        } else {
            s += "\n\nnot assigned in this file";
        }
        h.found = true;
        h.text = s;
        h.range = o->range;
        return h;
    }
    // A special parameter: $?, $@, ${#}, ...
    if (param && on_name && !BashIsName(param->name)) {
        const BashLspVocabEntry *e = BashLspFind(BashLspVariableVocab(), param->name);
        if (!e && param->name.size() > 0 && param->name[0] >= '1' && param->name[0] <= '9') {
            h.found = true;
            h.text = "positional parameter $" + param->name;
            h.range = param->name_range;
            return h;
        }
        if (e) {
            h.found = true;
            h.text = "$" + param->name + "\n\n" + e->doc;
            h.range = param->name_range;
            return h;
        }
    }
    if (param && !on_name) {
        std::string key = param->op;
        if (param->length) key = "length";
        if (param->indirect) key = param->op == "!*" || param->op == "!@" ? "prefix" : "indirect";
        if (param->indirect && param->has_subscript && (param->subscript == "@" || param->subscript == "*")) key = "keys";
        if (const BashLspVocabEntry *e = BashLspFind(BashLspExpansionVocab(), key)) {
            h.found = true;
            h.text = Describe(*e);
            h.range = param->range;
            return h;
        }
    }
    // Unbraced $?, $1 ...
    if (col > 0 || !text.empty()) {
        int a = col;
        if (static_cast<size_t>(a) < text.size() && text[static_cast<size_t>(a)] == '$') a++;
        if (a > 0 && text[static_cast<size_t>(a - 1)] == '$' && static_cast<size_t>(a) < text.size()) {
            const std::string n(1, text[static_cast<size_t>(a)]);
            if (const BashLspVocabEntry *e = BashLspFind(BashLspVariableVocab(), n)) {
                if (!IsNameChar(n[0]) || (n[0] >= '0' && n[0] <= '9')) {
                    h.found = true;
                    h.text = "$" + n + "\n\n" + e->doc;
                    h.range = TokenRange(line, a - 1, a + 1);
                    return h;
                }
            }
        }
    }
    // A function of this file.
    if (const Occurrence *o = ix->FuncAt(line, col)) {
        if (const FunctionDef *fd = FindFunction(*ix, o->name)) {
            std::string s = "function " + FunctionSignature(*ix, *fd, nullptr) + "\n\ndefined on line " +
                            std::to_string(fd->name_range.line + 1);
            const std::string doc = CommentAbove(*ix, fd->cmd->range.line);
            if (!doc.empty()) s += "\n\n" + doc;
            h.found = true;
            h.text = s;
            h.range = o->range;
            return h;
        }
    }
    // The word under the cursor: a builtin, keyword, test operator, option.
    int a = col, b = col;
    auto op_char = [](char ch) { return IsWordChar(ch) || ch == '=' || ch == '<' || ch == '>' || ch == '!' || ch == '{' || ch == '}'; };
    while (a > 0 && op_char(text[static_cast<size_t>(a - 1)])) a--;
    while (static_cast<size_t>(b) < text.size() && op_char(text[static_cast<size_t>(b)])) b++;
    if (a == b) return h;
    const std::string word = text.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
    const LineContext ctx = ContextAt(text, a);
    const BashRange r = TokenRange(line, a, b);
    if (ctx.in_test || (!ctx.words.empty() && (ctx.words[0] == "test" || ctx.words[0] == "["))) {
        if (const BashLspVocabEntry *e = BashLspFind(BashLspTestOperatorVocab(), word)) {
            h.found = true;
            h.text = Describe(*e);
            h.range = r;
            return h;
        }
    }
    if (!ctx.words.empty()) {
        if (ctx.words[0] == "set" && ctx.words.back() == "-o") {
            if (const BashLspVocabEntry *e = BashLspFind(BashLspSetOptionVocab(), word)) {
                h.found = true;
                h.text = "set -o " + word + "\n\n" + e->doc;
                h.range = r;
                return h;
            }
        }
        if (ctx.words[0] == "shopt") {
            if (const BashLspVocabEntry *e = BashLspFind(BashLspShoptVocab(), word)) {
                h.found = true;
                h.text = "shopt " + word + "\n\n" + e->doc;
                h.range = r;
                return h;
            }
        }
        if (word.size() >= 2 && word[0] == '-') {
            for (const BashLspOption &o : BashLspBuiltinOptions()) {
                if (ctx.words[0] == o.builtin && word == o.option) {
                    h.found = true;
                    h.text = std::string(o.builtin) + " " + o.option + (*o.arg ? std::string(" ") + o.arg : "") +
                             "\n\n" + o.doc;
                    h.range = r;
                    return h;
                }
            }
        }
    }
    if (const BashLspVocabEntry *e = BashLspFind(BashLspKeywordVocab(), word)) {
        if (ctx.command_position || word == "then" || word == "do" || word == "in" || word == "esac" ||
            word == "fi" || word == "done" || word == "]]" || word == "}") {
            h.found = true;
            h.text = Describe(*e);
            h.range = r;
            return h;
        }
    }
    if (ctx.command_position) {
        if (const BashLspVocabEntry *e = BashLspFind(BashLspBuiltinVocab(), word)) {
            h.found = true;
            h.text = Describe(*e);
            h.range = r;
            return h;
        }
    }
    return h;
}

// --- Signature help -------------------------------------------------------

BashLspSignature BashLspSignatureHelp(const std::vector<std::string> &lines, int line, int col) {
    BashLspSignature sig;
    const auto ix = BuildIndex(lines);
    const std::string &text = LineAt(ix->lines, line);
    const LineContext ctx = ContextAt(text, std::min(col, static_cast<int>(text.size())));
    if (ctx.words.empty()) return sig;
    const std::string &cmd = ctx.words[0];
    const int arg_index = static_cast<int>(ctx.words.size()) - 1;  // 0 = the first argument
    if (const FunctionDef *fd = FindFunction(*ix, cmd)) {
        sig.label = FunctionSignature(*ix, *fd, &sig.params);
        sig.documentation = CommentAbove(*ix, fd->cmd->range.line);
        sig.found = true;
        if (!sig.params.empty()) {
            sig.active_param = std::min(arg_index, static_cast<int>(sig.params.size()) - 1);
            if (arg_index >= static_cast<int>(sig.params.size()) && sig.params.back() != "[args...]") sig.active_param = -1;
        }
        return sig;
    }
    const BashLspVocabEntry *e = BashLspFind(BashLspBuiltinVocab(), cmd);
    if (!e) return sig;
    std::string label = e->synopsis;
    const size_t or_pos = label.find(" or ");
    if (or_pos != std::string::npos) label = label.substr(0, or_pos);
    sig.found = true;
    sig.label = label;
    sig.documentation = e->doc;
    // The synopsis's items: [bracketed groups] and plain words.
    size_t p = label.find(' ');
    while (p != std::string::npos && p < label.size()) {
        while (p < label.size() && label[p] == ' ') p++;
        if (p >= label.size()) break;
        size_t q = p;
        if (label[p] == '[') {
            int depth = 0;
            while (q < label.size()) {
                if (label[q] == '[') depth++;
                if (label[q] == ']' && --depth == 0) {
                    q++;
                    break;
                }
                q++;
            }
        } else {
            while (q < label.size() && label[q] != ' ') q++;
        }
        sig.params.push_back(label.substr(p, q - p));
        p = q;
    }
    // The active item: the option being typed, else the first item that is
    // not an option group past the arguments typed.
    const std::string &tok = ctx.token;
    if (!tok.empty() && tok[0] == '-' && tok.size() >= 2) {
        for (size_t k = 0; k < sig.params.size(); k++) {
            const std::string &pr = sig.params[k];
            if (pr.find(tok) != std::string::npos ||
                (pr.rfind("[-", 0) == 0 && pr.find(tok[1]) != std::string::npos && pr.find(' ') == std::string::npos)) {
                sig.active_param = static_cast<int>(k);
                break;
            }
        }
    } else {
        int positional = 0;
        for (size_t k = 1; k < ctx.words.size(); k++) {
            if (ctx.words[k].empty() || ctx.words[k][0] != '-') positional++;
        }
        int seen = 0;
        for (size_t k = 0; k < sig.params.size(); k++) {
            if (sig.params[k].rfind("[-", 0) == 0) continue;
            if (seen == positional || sig.params[k].find("...") != std::string::npos) {
                sig.active_param = static_cast<int>(k);
                break;
            }
            seen++;
        }
    }
    return sig;
}
