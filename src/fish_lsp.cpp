// mep's Fish language server, analysis half: see fish_lsp.h.
//
// The document is read by fish_parse.h (fish's own tokens, tree and first
// error) and then indexed: functions (with their argument names and
// description), variable definitions (set / read / for / function -a / -V /
// argparse) and uses, and command words. Variables follow fish's scoping in
// the way a reader of the file sees it: inside a function a name is that
// function's own when the function defines it locally (set -l / -f, for,
// read, --argument-names, or a plain `set` of a name the file never sets
// globally); everything else is the file's global.
#include "fish_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

#include "fish_format.h"
#include "fish_lsp_vocab.h"
#include "fish_parse.h"

namespace fp = fishparse;

namespace {

using u32 = std::u32string;
using u32v = std::u32string_view;


enum class DefKind { Set, For, Read, Arg, Inherit, Argparse };

struct FuncInfo {
    std::string name;
    size_t name_start = 0, name_len = 0;  // characters
    size_t start = 0, end = 0;            // the whole block
    std::string desc;
    std::vector<std::string> args;
    int parent = -1;
};

struct VarOcc {
    std::string name;
    size_t start = 0, len = 0;
    bool def = false;
    DefKind kind = DefKind::Set;
    bool global_flag = false;  // set -g / -U
    bool local_flag = false;   // set -l / -f
    bool renamable = true;
    int func = -1;  // innermost enclosing function
    int key = -2;   // resolved scope: a function index or -1 (the file)
};

struct CmdOcc {
    std::string name;
    size_t start = 0, len = 0;
    int func = -1;
    size_t stmt_start = 0;  // the statement's first character
};

// A simple command: its word and argument texts, for lints and signature help.
struct Stmt {
    std::string cmd;
    size_t cmd_start = 0, cmd_len = 0;
    std::vector<std::string> args;  // unescaped where plain
    std::vector<std::pair<size_t, size_t>> arg_ranges;
    std::vector<bool> arg_plain;
    size_t end = 0;
};

const char *const kKeywords[] = {"and", "begin", "break", "builtin", "case", "command", "continue", "else",
                                 "end", "exec", "for", "function", "if", "in", "not", "or", "return",
                                 "switch", "time", "while"};

const char *const kWellKnownVars[] = {
    "argv", "status", "pipestatus", "fish_pid", "last_pid", "version", "FISH_VERSION", "hostname", "history",
    "PWD", "HOME", "USER", "PATH", "SHLVL", "CMD_DURATION", "COLUMNS", "LINES", "TERM", "LANG", "umask",
    "fish_function_path", "fish_complete_path", "fish_user_paths", "fish_greeting", "fish_key_bindings",
    "fish_bind_mode", "fish_color_normal", "fish_color_command", "fish_color_error", "fish_history",
    "fish_trace", "status_generation", "fish_kill_signal", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "__fish_config_dir",
    "__fish_data_dir", "fish_emoji_width", "fish_ambiguous_width", "fish_handle_reflow", "fish_prompt_pwd_dir_length",
};

const char *const kReadOnlyVars[] = {"FISH_VERSION", "PWD", "SHLVL", "_", "fish_kill_signal", "fish_killring",
                                     "fish_pid", "history", "hostname", "pipestatus", "status",
                                     "status_generation", "version"};

const char *const kReservedWords[] = {"[", "_", "and", "argparse", "begin", "break", "builtin", "case", "command",
                                      "continue", "else", "end", "eval", "exec", "for", "function", "if", "not",
                                      "or", "read", "return", "set", "status", "string", "switch", "test", "time",
                                      "while"};

bool InList(const std::string &s, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s == list[i]) return true;
    return false;
}
template <size_t N>
bool InList(const std::string &s, const char *const (&list)[N]) {
    return InList(s, list, N);
}

std::string U8(u32v s) { return fp::ToUtf8(s); }

// The plain text of a word when it has no expansions (quotes and escapes resolved), else "".
std::string PlainWord(u32v raw, bool *plain) {
    *plain = false;
    for (char32_t c : raw)
        if (c == '$' || c == '(' || c == '*' || c == '{' || c == '~') return "";
    u32 out;
    if (!fp::Unescape(raw, false, false, &out)) return "";
    *plain = true;
    return U8(out);
}

bool IsNameByte(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

}  // namespace

class FishLspDocument {
public:
    std::vector<std::string> lines;
    std::string text;
    fp::Source src;
    std::vector<size_t> line_start;  // in characters
    fp::Ast ast;
    std::vector<fp::ParseError> errors;
    std::vector<FuncInfo> funcs;
    std::vector<VarOcc> vars;
    std::vector<CmdOcc> cmds;
    std::vector<Stmt> stmts;
    std::vector<FishLspDiagnostic> lints;
    struct Block {
        size_t start, end;
    };
    std::vector<Block> blocks;      // multi-line candidates for folding
    std::vector<Block> case_items;  // `case` arms

    size_t Off(int line, int col) const {
        if (line < 0) return 0;
        if (static_cast<size_t>(line) >= line_start.size()) return src.text.size();
        const size_t ls = line_start[static_cast<size_t>(line)];
        const size_t target = src.Byte(ls) + static_cast<size_t>(std::max(col, 0));
        size_t o = ls;
        while (o < src.text.size() && src.text[o] != '\n' && src.Byte(o) < target) o++;
        return o;
    }
    void Pos(size_t off, int *line, int *col) const {
        off = std::min(off, src.text.size());
        const auto it = std::upper_bound(line_start.begin(), line_start.end(), off);
        const size_t l = static_cast<size_t>(it - line_start.begin()) - 1;
        *line = static_cast<int>(l);
        *col = static_cast<int>(src.Byte(off) - src.Byte(line_start[l]));
    }
    FishLspRange RangeOf(size_t start, size_t len) const {
        FishLspRange r;
        Pos(start, &r.line, &r.col);
        Pos(start + len, &r.end_line, &r.end_col);
        return r;
    }
    u32v Text(size_t start, size_t len) const { return u32v(src.text).substr(start, len); }
};

namespace {

// --- indexing ---------------------------------------------------------------------------

class Indexer {
public:
    explicit Indexer(FishLspDocument &d) : d_(d) {}

    void IndexTree(const fp::Ast &ast, size_t base, int func, int depth) {
        if (depth > 40) return;
        Walk(ast, 0, base, func, depth);
    }

private:
    void Walk(const fp::Ast &ast, int n, size_t base, int func, int depth) {
        const fp::Node &node = ast[n];
        int inner = func;
        switch (node.kind) {
        case fp::Kind::BlockStatement: {
            const int h = ast[ast.Field(n, fp::Role::Header)].kids[0];
            if (ast[h].kind == fp::Kind::FunctionHeader) inner = FunctionDef(ast, n, h, base, func);
            else if (ast[h].kind == fp::Kind::ForHeader) ForDef(ast, h, base, func);
            BlockRange(ast, n, base);
            break;
        }
        case fp::Kind::IfStatement: case fp::Kind::SwitchStatement: case fp::Kind::BraceStatement:
            BlockRange(ast, n, base);
            break;
        case fp::Kind::CaseItem: {
            fp::Range r;
            ast.RangeOf(n, &r);
            if (r.len) d_.case_items.push_back({base + r.start, base + r.End()});
            break;
        }
        case fp::Kind::DecoratedStatement: Decorated(ast, n, base, func); break;
        case fp::Kind::Argument: case fp::Kind::Token:
            if (node.sourced && node.len && (node.kind == fp::Kind::Argument || node.tok == fp::PTok::String))
                ScanWord(ast, n, base, func, depth);
            break;
        default: break;
        }
        for (int k : node.kids) {
            // A function's own name belongs to the outer scope; the rest of it to the function.
            Walk(ast, k, base, inner, depth);
        }
    }

    void BlockRange(const fp::Ast &ast, int n, size_t base) {
        fp::Range r;
        ast.RangeOf(n, &r);
        if (r.len) d_.blocks.push_back({base + r.start, base + r.End()});
    }

    // `$name` uses in a word, and the command substitutions inside it.
    void ScanWord(const fp::Ast &ast, int n, size_t base, int func, int depth) {
        (void)ast;
        const fp::Node &node = ast[n];
        const size_t start = base + node.start;
        const u32v t = d_.Text(start, node.len);
        std::vector<std::pair<size_t, size_t>> subs;
        size_t cursor = 0;
        bool quoted = false;
        for (;;) {
            bool has_dollar = false;
            fp::Cmdsub cs;
            if (fp::LocateCmdsub(t, &cursor, true, &quoted, &has_dollar, &cs) != 1) break;
            subs.push_back({cs.start, cs.end});
            const size_t cstart = cs.CommandStart(), clen = cs.CommandEnd() - cs.CommandStart();
            fp::ParseFlags f;
            f.continue_after_error = true;
            f.leave_unterminated = true;
            const fp::Ast sub = fp::Parse(t.substr(cstart, clen), f);
            IndexTree(sub, start + cstart, func, depth + 1);
            if (cs.end <= cs.start) break;
        }
        enum { Unq, Single, Double } mode = Unq;
        size_t sub_i = 0;
        for (size_t i = 0; i < t.size();) {
            while (sub_i < subs.size() && subs[sub_i].second <= i) sub_i++;
            if (sub_i < subs.size() && i >= subs[sub_i].first) {
                i = subs[sub_i].second;
                continue;
            }
            const char32_t c = t[i];
            if (mode == Single) {
                if (c == '\\') i += 2;
                else {
                    if (c == '\'') mode = Unq;
                    i++;
                }
                continue;
            }
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == '\'' && mode == Unq) mode = Single;
            else if (c == '"') mode = mode == Double ? Unq : Double;
            else if (c == '$') {
                size_t j = i + 1;
                while (j < t.size() && t[j] == '$') j++;
                size_t k = j;
                while (k < t.size() && fp::ValidVarNameChar(t[k])) k++;
                if (k > j) {
                    VarOcc v;
                    v.name = U8(t.substr(j, k - j));
                    v.start = start + j;
                    v.len = k - j;
                    v.func = func;
                    d_.vars.push_back(v);
                }
                i = std::max(k, i + 1);
                continue;
            }
            i++;
        }
    }

    // A name word (`foo` or `foo[1]`) as a variable definition or use.
    void NameWord(size_t start, u32v raw, int func, bool def, DefKind kind, bool g, bool l, bool renamable = true) {
        size_t k = 0;
        while (k < raw.size() && fp::ValidVarNameChar(raw[k])) k++;
        if (k == 0 || (k < raw.size() && raw[k] != '[')) return;
        VarOcc v;
        v.name = U8(raw.substr(0, k));
        v.start = start;
        v.len = k;
        v.def = def;
        v.kind = kind;
        v.global_flag = g;
        v.local_flag = l;
        v.func = func;
        v.renamable = renamable;
        d_.vars.push_back(v);
    }

    struct Word {
        size_t start = 0, len = 0;
        u32v raw;
        std::string plain;
        bool is_plain = false;
    };

    std::vector<Word> ArgWords(const fp::Ast &ast, int list, size_t base) {
        std::vector<Word> out;
        if (list < 0) return out;
        for (int item : ast[list].kids) {
            int a = item;
            if (ast[item].kind == fp::Kind::ArgumentOrRedirection) {
                a = ast[item].kids.empty() ? -1 : ast[item].kids[0];
                if (a < 0 || ast[a].kind != fp::Kind::Argument) continue;
            }
            const fp::Node &an = ast[a];
            if (!an.sourced || an.len == 0) continue;
            Word w;
            w.start = base + an.start;
            w.len = an.len;
            w.raw = d_.Text(w.start, w.len);
            w.plain = PlainWord(w.raw, &w.is_plain);
            out.push_back(w);
        }
        return out;
    }

    void ForDef(const fp::Ast &ast, int h, size_t base, int func) {
        const int v = ast.Field(h, fp::Role::VarName);
        if (v < 0 || !ast[v].sourced) return;
        NameWord(base + ast[v].start, d_.Text(base + ast[v].start, ast[v].len), func, true, DefKind::For, false,
                 true);
    }

    int FunctionDef(const fp::Ast &ast, int block, int h, size_t base, int outer) {
        const int first = ast.Field(h, fp::Role::FirstArg);
        FuncInfo f;
        f.parent = outer;
        fp::Range br;
        ast.RangeOf(block, &br);
        if (br.len == 0) {
            const int kw = ast.Field(h, fp::Role::Kw);
            br.start = ast[kw].start;
            br.len = ast[kw].len;
            // An unclosed function runs to the end of its text.
            fp::Range all;
            ast.RangeOf(0, &all);
            for (int k : ast[block].kids) {
                fp::Range kr;
                if (ast.RangeOf(k, &kr) && kr.len && kr.End() > br.End()) br.len = kr.End() - br.start;
            }
        }
        f.start = base + br.start;
        f.end = base + br.End();
        if (first >= 0 && ast[first].sourced && ast[first].len) {
            f.name_start = base + ast[first].start;
            f.name_len = ast[first].len;
            bool plain = false;
            f.name = PlainWord(d_.Text(f.name_start, f.name_len), &plain);
            if (!plain) f.name.clear();
        }
        const int idx = static_cast<int>(d_.funcs.size());
        std::vector<Word> words = ArgWords(ast, ast.Field(h, fp::Role::Args), base);
        // Options: -a/--argument-names NAMES..., -d DESC, -V NAME, -v VAR, and the rest take one value.
        enum { None, ArgNames, Desc, Inherit, OnVar, Skip } want = None;
        for (const Word &w : words) {
            const std::string &a = w.plain;
            const bool opt = w.is_plain && a.size() > 1 && a[0] == '-';
            if (opt) {
                std::string name = a, val;
                const size_t eq = a.find('=');
                if (a.compare(0, 2, "--") == 0 && eq != std::string::npos) {
                    name = a.substr(0, eq);
                    val = a.substr(eq + 1);
                }
                want = None;
                if (name == "-a" || name == "--argument-names" || name == "--argument") want = ArgNames;
                else if (name == "-d" || name == "--description") want = Desc;
                else if (name == "-V" || name == "--inherit-variable") want = Inherit;
                else if (name == "-v" || name == "--on-variable") want = OnVar;
                else if (name == "-w" || name == "--wraps" || name == "-e" || name == "--on-event" || name == "-j" ||
                         name == "--on-job-exit" || name == "-p" || name == "--on-process-exit" || name == "-s" ||
                         name == "--on-signal")
                    want = Skip;
                else if (name.size() > 2 && name[1] != '-') {
                    // -ad...: bundled; a value-taking letter last takes the next word
                    const char last = name.back();
                    want = last == 'a' ? ArgNames : last == 'd' ? Desc : last == 'V' ? Inherit : last == 'v' ? OnVar
                         : std::strchr("wejps", last) ? Skip : None;
                }
                if (!val.empty()) {
                    if (want == Desc) f.desc = val;
                    if (want != ArgNames) want = None;
                }
                continue;
            }
            switch (want) {
            case ArgNames:
                f.args.push_back(w.plain);
                NameWord(w.start, w.raw, idx, true, DefKind::Arg, false, true);
                break;
            case Desc:
                f.desc = w.plain;
                want = None;
                break;
            case Inherit:
                NameWord(w.start, w.raw, idx, true, DefKind::Inherit, false, true);
                want = None;
                break;
            case OnVar:
                NameWord(w.start, w.raw, -1, false, DefKind::Set, false, false);
                want = None;
                break;
            case Skip: want = None; break;
            case None: break;
            }
        }
        d_.funcs.push_back(f);
        return idx;
    }

    void Decorated(const fp::Ast &ast, int n, size_t base, int func) {
        const int cmd = ast.Field(n, fp::Role::Command);
        if (cmd < 0 || !ast[cmd].sourced || ast[cmd].len == 0) return;
        Stmt st;
        st.cmd_start = base + ast[cmd].start;
        st.cmd_len = ast[cmd].len;
        bool plain = false;
        st.cmd = PlainWord(d_.Text(st.cmd_start, st.cmd_len), &plain);
        fp::Range sr;
        ast.RangeOf(n, &sr);
        st.end = base + sr.End();
        std::vector<Word> words = ArgWords(ast, ast.Field(n, fp::Role::ArgsOrRedirs), base);
        for (const Word &w : words) {
            st.args.push_back(w.plain);
            st.arg_ranges.push_back({w.start, w.len});
            st.arg_plain.push_back(w.is_plain);
        }
        if (plain) {
            CmdOcc c;
            c.name = st.cmd;
            c.start = st.cmd_start;
            c.len = st.cmd_len;
            c.func = func;
            c.stmt_start = base + sr.start;
            d_.cmds.push_back(c);
        }
        d_.stmts.push_back(st);
        if (!plain) return;
        if (st.cmd == "set") SetCmd(words, func);
        else if (st.cmd == "read") ReadCmd(words, func);
        else if (st.cmd == "argparse") ArgparseCmd(words, func);
    }

    void SetCmd(const std::vector<Word> &w, int func) {
        bool g = false, l = false, erase = false, query = false, list = false;
        size_t i = 0;
        for (; i < w.size(); i++) {
            const std::string &a = w[i].plain;
            if (!w[i].is_plain) break;
            if (a == "--") {
                i++;
                break;
            }
            if (a.size() < 2 || a[0] != '-') break;
            if (a[1] == '-') {
                if (a == "--global" || a == "--universal") g = true;
                else if (a == "--local" || a == "--function") l = true;
                else if (a == "--erase") erase = true;
                else if (a == "--query") query = true;
                else if (a == "--names" || a == "--show" || a == "--help" || a == "--long") list = true;
                continue;
            }
            for (size_t k = 1; k < a.size(); k++) {
                switch (a[k]) {
                case 'g': case 'U': g = true; break;
                case 'l': case 'f': l = true; break;
                case 'e': erase = true; break;
                case 'q': query = true; break;
                case 'n': case 'S': case 'h': case 'L': list = true; break;
                default: break;
                }
            }
        }
        if (list || i >= w.size()) return;
        if (erase || query) {
            for (; i < w.size(); i++) NameWord(w[i].start, w[i].raw, func, false, DefKind::Set, g, l);
            return;
        }
        NameWord(w[i].start, w[i].raw, func, true, DefKind::Set, g, l);
    }

    void ReadCmd(const std::vector<Word> &w, int func) {
        bool g = false, l = false;
        bool skip_next = false;
        bool opts_done = false;
        for (const Word &x : w) {
            if (skip_next) {
                skip_next = false;
                continue;
            }
            const std::string &a = x.plain;
            if (!opts_done && x.is_plain && a == "--") {
                opts_done = true;
                continue;
            }
            if (!opts_done && x.is_plain && a.size() > 1 && a[0] == '-') {
                if (a[1] == '-') {
                    const std::string name = a.substr(0, a.find('='));
                    if (name == "--global" || name == "--universal") g = true;
                    if (name == "--local" || name == "--function") l = true;
                    if (a.find('=') == std::string::npos &&
                        (name == "--prompt" || name == "--prompt-str" || name == "--right-prompt" ||
                         name == "--command" || name == "--nchars" || name == "--delimiter"))
                        skip_next = true;
                    continue;
                }
                for (size_t k = 1; k < a.size(); k++) {
                    const char c = a[k];
                    if (c == 'g' || c == 'U') g = true;
                    if (c == 'l' || c == 'f') l = true;
                    if (std::strchr("pPRcnd", c)) {
                        if (k + 1 == a.size()) skip_next = true;
                        break;
                    }
                }
                continue;
            }
            NameWord(x.start, x.raw, func, true, DefKind::Read, g, l || !g);
        }
    }

    void ArgparseCmd(const std::vector<Word> &w, int func) {
        size_t i = 0;
        bool skip_next = false;
        for (; i < w.size(); i++) {
            const std::string &a = w[i].plain;
            if (a == "--") break;
            if (skip_next) {
                skip_next = false;
                continue;
            }
            if (!w[i].is_plain) continue;
            if (a.size() > 1 && a[0] == '-') {
                const std::string name = a.substr(0, a.find('='));
                if (a.find('=') == std::string::npos &&
                    (name == "-n" || name == "--name" || name == "-x" || name == "--exclusive" || name == "-N" ||
                     name == "--min-args" || name == "-X" || name == "--max-args"))
                    skip_next = true;
                continue;
            }
            // A spec: [s/][long][=?|=+|=][!validation] or #-long
            std::string spec = a.substr(0, a.find_first_of("=!"));
            if (!spec.empty() && spec[0] == '#') continue;
            std::vector<std::string> names;
            const size_t slash = spec.find('/');
            if (slash != std::string::npos) {
                names.push_back(spec.substr(0, slash));
                names.push_back(spec.substr(slash + 1));
            } else if (spec.size() > 2 && spec[1] == '-') {
                names.push_back(spec.substr(2));
            } else {
                names.push_back(spec);
            }
            for (std::string nm : names) {
                if (nm.empty()) continue;
                std::replace(nm.begin(), nm.end(), '-', '_');
                VarOcc v;
                v.name = "_flag_" + nm;
                v.start = w[i].start;
                v.len = w[i].len;
                v.def = true;
                v.kind = DefKind::Argparse;
                v.local_flag = true;
                v.func = func;
                v.renamable = false;
                d_.vars.push_back(v);
            }
        }
    }

    FishLspDocument &d_;
};

// Which scope each variable occurrence belongs to.
void ResolveScopes(FishLspDocument &d) {
    std::set<std::string> globals;
    for (const VarOcc &v : d.vars)
        if (v.def && (v.func < 0 || v.global_flag)) globals.insert(v.name);
    std::set<std::pair<int, std::string>> owned;
    for (const VarOcc &v : d.vars) {
        if (!v.def || v.func < 0 || v.global_flag) continue;
        const bool local = v.kind != DefKind::Set || v.local_flag || !globals.count(v.name);
        if (local) owned.insert({v.func, v.name});
    }
    for (VarOcc &v : d.vars) {
        if (v.func >= 0 && v.global_flag && v.def) {
            v.key = -1;
            continue;
        }
        v.key = v.func >= 0 && owned.count({v.func, v.name}) ? v.func : -1;
    }
}

// --- lints --------------------------------------------------------------------------------

void AddLint(FishLspDocument &d, size_t start, size_t len, FishLspSeverity sev, const char *code,
             const std::string &msg, bool unnecessary = false) {
    FishLspDiagnostic diag;
    diag.range = d.RangeOf(start, std::max<size_t>(len, 1));
    diag.severity = sev;
    diag.code = code;
    diag.message = msg;
    diag.unnecessary = unnecessary;
    d.lints.push_back(diag);
}

const char *const kStringSubs[] = {"collect", "escape", "join", "join0", "length", "lower", "match", "pad",
                                   "repeat", "replace", "shorten", "split", "split0", "sub", "trim", "unescape",
                                   "upper"};
const char *const kPathSubs[] = {"basename", "change-extension", "dirname", "extension", "filter", "is",
                                 "mtime", "normalize", "resolve", "sort"};
const char *const kStatusSubs[] = {
    "basename", "build-info", "buildinfo", "current-command", "current-commandline", "current-filename",
    "current-function", "current-line-number", "dirname", "features", "filename", "fish-path", "function",
    "get-file", "help-sections", "is-block", "is-breakpoint", "is-command-substitution", "is-full-job-control", "is-interactive",
    "is-interactive-job-control", "is-interactive-read", "is-login", "is-no-job-control", "job-control",
    "language", "line", "line-number", "list-files", "print-stack-trace", "stack-trace", "terminal", "terminal-os",
    "test-feature", "test-terminal-feature"};

void Lints(FishLspDocument &d) {
    for (const FuncInfo &f : d.funcs) {
        if (!f.name.empty() && InList(f.name, kReservedWords))
            AddLint(d, f.name_start, f.name_len, FishLspSeverity::Error, "reserved-function-name",
                    "function: " + f.name + ": cannot use reserved keyword as function name");
    }
    for (const Stmt &s : d.stmts) {
        if (s.cmd == "set") {
            // The defined name: the first non-option word.
            for (size_t i = 0; i < s.args.size(); i++) {
                const std::string &a = s.args[i];
                if (!s.arg_plain[i]) break;
                if (a.size() > 1 && a[0] == '-') {
                    if (a == "-q" || a == "--query" || a == "-n" || a == "--names" || a == "-S" || a == "--show" ||
                        a.find('q') != std::string::npos)
                        break;
                    continue;
                }
                const std::string name = a.substr(0, a.find('['));
                if (InList(name, kReadOnlyVars))
                    AddLint(d, s.arg_ranges[i].first, name.size(), FishLspSeverity::Error, "read-only-variable",
                            "set: Tried to change the read-only variable '" + name + "'");
                break;
            }
        }
        const char *const *subs = nullptr;
        size_t nsubs = 0;
        if (s.cmd == "string") subs = kStringSubs, nsubs = sizeof(kStringSubs) / sizeof(kStringSubs[0]);
        if (s.cmd == "path") subs = kPathSubs, nsubs = sizeof(kPathSubs) / sizeof(kPathSubs[0]);
        if (s.cmd == "status") subs = kStatusSubs, nsubs = sizeof(kStatusSubs) / sizeof(kStatusSubs[0]);
        if (subs && !s.args.empty() && s.arg_plain[0] && !s.args[0].empty() && s.args[0][0] != '-' &&
            !InList(s.args[0], subs, nsubs))
            AddLint(d, s.arg_ranges[0].first, s.arg_ranges[0].second, FishLspSeverity::Error, "unknown-subcommand",
                    s.cmd + " " + s.args[0] + ": invalid subcommand");
        if (s.cmd == "[" && !s.args.empty() && s.arg_plain.back() && s.args.back() != "]" &&
            !(s.args.size() == 1 && (s.args[0] == "-h" || s.args[0] == "--help")))
            AddLint(d, s.cmd_start, s.cmd_len, FishLspSeverity::Error, "unclosed-test",
                    "[: the last argument must be ']'");
    }
    // Statements after return / exit / break / continue in the same list never run.
    for (size_t n = 0; n < d.ast.nodes.size(); n++) {
        const fp::Node &node = d.ast.nodes[n];
        if (node.kind != fp::Kind::JobList || node.kids.size() < 2) continue;
        for (size_t i = 0; i + 1 < node.kids.size(); i++) {
            const int jc = node.kids[i];
            if (d.ast.Field(jc, fp::Role::Decorator) >= 0) continue;
            if (!d.ast[d.ast.Field(jc, fp::Role::Continuations)].kids.empty()) continue;
            const int job = d.ast.Field(jc, fp::Role::Job);
            if (d.ast.Field(job, fp::Role::Bg) >= 0) continue;
            if (d.ast.Field(job, fp::Role::Time) >= 0) continue;
            if (!d.ast[d.ast.Field(job, fp::Role::Continuation)].kids.empty()) continue;
            if (!d.ast[d.ast.Field(job, fp::Role::Variables)].kids.empty()) continue;
            const int st = d.ast.Field(job, fp::Role::Statement);
            const int dec = d.ast[st].kids.empty() ? -1 : d.ast[st].kids[0];
            if (dec < 0 || d.ast[dec].kind != fp::Kind::DecoratedStatement) continue;
            if (d.ast.Field(dec, fp::Role::OptDecoration) >= 0) continue;
            const int cmd = d.ast.Field(dec, fp::Role::Command);
            if (!d.ast[cmd].sourced) continue;
            const u32v c = d.Text(d.ast[cmd].start, d.ast[cmd].len);
            if (c != U"return" && c != U"exit" && c != U"break" && c != U"continue") continue;
            bool help = false;
            for (int item : d.ast[d.ast.Field(dec, fp::Role::ArgsOrRedirs)].kids) {
                const int a = d.ast[item].kids.empty() ? -1 : d.ast[item].kids[0];
                if (a >= 0 && d.ast[a].kind == fp::Kind::Argument) {
                    const u32v t = d.Text(d.ast[a].start, d.ast[a].len);
                    help = t == U"-h" || t == U"--help";
                    break;
                }
            }
            if (help) continue;
            fp::Range first, last;
            if (!d.ast.RangeOf(node.kids[i + 1], &first) || !d.ast.RangeOf(node.kids.back(), &last)) break;
            if (first.len == 0) break;
            AddLint(d, first.start, last.End() - first.start, FishLspSeverity::Hint, "unreachable-code",
                    "Unreachable code: nothing runs after '" + U8(c) + "'", true);
            break;
        }
    }
}

// --- helpers for queries -----------------------------------------------------------------

const VarOcc *VarAt(const FishLspDocument &d, size_t off) {
    for (const VarOcc &v : d.vars)
        if (off >= v.start && off <= v.start + v.len) return &v;
    return nullptr;
}

int FuncByName(const FishLspDocument &d, const std::string &name) {
    for (size_t i = 0; i < d.funcs.size(); i++)
        if (d.funcs[i].name == name) return static_cast<int>(i);
    return -1;
}

// The function name under the cursor: a definition's name or a command word naming a function.
bool FuncNameAt(const FishLspDocument &d, size_t off, std::string *name, size_t *start, size_t *len) {
    for (const FuncInfo &f : d.funcs)
        if (!f.name.empty() && off >= f.name_start && off <= f.name_start + f.name_len) {
            *name = f.name, *start = f.name_start, *len = f.name_len;
            return true;
        }
    for (const CmdOcc &c : d.cmds)
        if (off >= c.start && off <= c.start + c.len && FuncByName(d, c.name) >= 0) {
            *name = c.name, *start = c.start, *len = c.len;
            return true;
        }
    return false;
}

const CmdOcc *CmdAt(const FishLspDocument &d, size_t off) {
    for (const CmdOcc &c : d.cmds)
        if (off >= c.start && off <= c.start + c.len) return &c;
    return nullptr;
}

int InnermostFunc(const FishLspDocument &d, size_t off) {
    int best = -1;
    for (size_t i = 0; i < d.funcs.size(); i++) {
        const FuncInfo &f = d.funcs[i];
        if (off > f.start && off <= f.end && (best < 0 || f.start >= d.funcs[static_cast<size_t>(best)].start))
            best = static_cast<int>(i);
    }
    return best;
}

std::string FunctionSignature(const FuncInfo &f) {
    std::string s = f.name;
    for (const std::string &a : f.args) s += " " + a;
    return s;
}

std::string VocabMarkdown(const FishVocabEntry &e) {
    std::string md = "```fish\n";
    md += *e.synopsis ? e.synopsis : e.name;
    md += "\n```\n";
    md += e.builtin ? "fish builtin" : "function shipped with fish";
    md += " \xC2\xB7 `help ";
    md += e.name;
    md += "`";
    return md;
}

}  // namespace

std::shared_ptr<const FishLspDocument> FishLspAnalyze(const std::vector<std::string> &lines) {
    auto doc = std::make_shared<FishLspDocument>();
    FishLspDocument &d = *doc;
    d.lines = lines;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) d.text += '\n';
        d.text += lines[i];
    }
    d.src = fp::Source::FromUtf8(d.text);
    d.line_start.push_back(0);
    for (size_t i = 0; i < d.src.text.size(); i++)
        if (d.src.text[i] == '\n') d.line_start.push_back(i + 1);
    d.errors = fp::DetectErrors(d.src.text);
    fp::ParseFlags f;
    f.continue_after_error = true;
    f.include_comments = true;
    f.leave_unterminated = true;
    d.ast = fp::Parse(d.src.text, f);
    Indexer ix(d);
    ix.IndexTree(d.ast, 0, -1, 0);
    ResolveScopes(d);
    Lints(d);
    return doc;
}

std::vector<FishLspDiagnostic> FishLspDiagnostics(const FishLspDocument &doc) {
    std::vector<FishLspDiagnostic> out;
    if (!doc.errors.empty()) {
        const fp::ParseError &e = doc.errors.front();
        FishLspDiagnostic diag;
        if (e.start == fp::kNoLocation || e.start >= doc.src.text.size()) {
            const size_t end = doc.src.text.size();
            diag.range = doc.RangeOf(end, 0);
        } else {
            size_t len = std::max<size_t>(e.len, 1);
            // One line at most.
            size_t stop = e.start;
            while (stop < e.start + len && stop < doc.src.text.size() && doc.src.text[stop] != '\n') stop++;
            if (stop == e.start && stop < doc.src.text.size()) stop++;
            diag.range = doc.RangeOf(e.start, stop - e.start);
        }
        diag.severity = FishLspSeverity::Error;
        diag.code = "syntax-error";
        diag.message = e.text;
        out.push_back(diag);
    }
    for (const FishLspDiagnostic &l : doc.lints) out.push_back(l);
    return out;
}

std::string FishLspFishCheck(const std::string &text, const std::string &path) {
    std::string body = text;
    if (body.compare(0, 3, "\xEF\xBB\xBF") == 0) body.erase(0, 3);  // fish swallows a BOM
    const fp::Source src = fp::Source::FromUtf8(body);
    const std::vector<fp::ParseError> errs = fp::DetectErrors(src.text);
    if (errs.empty()) return "";
    const fp::ParseError &e = errs.front();
    if (e.start == fp::kNoLocation || e.start > src.text.size()) return path + ": " + e.text;
    const size_t line =
        1 + static_cast<size_t>(std::count(src.text.begin(), src.text.begin() + static_cast<long>(e.start), U'\n'));
    return path + " (line " + std::to_string(line) + "): " + e.text;
}

std::string FishLspFormatDiagnostic(const std::string &path, const FishLspDiagnostic &d) {
    const char *sev = d.severity == FishLspSeverity::Error         ? "error"
                      : d.severity == FishLspSeverity::Warning     ? "warning"
                      : d.severity == FishLspSeverity::Information ? "info"
                                                                   : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool FishLspDiagnosticFails(const FishLspDiagnostic &d) {
    return d.severity == FishLspSeverity::Error || d.severity == FishLspSeverity::Warning;
}

// --- completion -----------------------------------------------------------------------------

namespace {

// The words of the current statement before the cursor, on this line (rough, for completion).
std::vector<std::string> WordsBefore(const std::string &line, size_t col, bool *at_command) {
    std::vector<std::string> words;
    std::string cur;
    bool in_s = false, in_d = false;
    *at_command = true;
    auto push = [&]() {
        if (!cur.empty()) words.push_back(cur);
        cur.clear();
    };
    for (size_t i = 0; i < col && i < line.size(); i++) {
        const char c = line[i];
        if (in_s) {
            if (c == '\'') in_s = false;
            cur += c;
            continue;
        }
        if (in_d) {
            if (c == '"') in_d = false;
            cur += c;
            continue;
        }
        if (c == '\'') in_s = true;
        if (c == '"') in_d = true;
        if (c == ' ' || c == '\t') {
            push();
        } else if (c == ';' || c == '|' || c == '&' || c == '(' || c == ')' || c == '{' || c == '}') {
            push();
            words.clear();
        } else if (c == '#' && cur.empty()) {
            words.clear();
            words.push_back("#");
            return words;
        } else {
            cur += c;
        }
    }
    // Drop prefix words that keep the next word at command position.
    static const char *const kPrefix[] = {"and", "or", "not", "!", "if", "while", "else", "begin", "command",
                                          "builtin", "exec", "time"};
    size_t k = 0;
    while (k < words.size() && (InList(words[k], kPrefix) || words[k].find('=') != std::string::npos)) k++;
    words.erase(words.begin(), words.begin() + static_cast<long>(k));
    *at_command = words.empty();
    return words;
}

}  // namespace

std::vector<FishLspCompletionItem> FishLspCompletions(const FishLspDocument &doc, int line, int col) {
    std::vector<FishLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return out;
    const std::string &cur = doc.lines[static_cast<size_t>(line)];
    col = std::max(0, std::min(col, static_cast<int>(cur.size())));
    const size_t ucol = static_cast<size_t>(col);
    size_t start = ucol;
    while (start > 0 && (IsNameByte(cur[start - 1]) || cur[start - 1] == '-' || cur[start - 1] == '.' ||
                         static_cast<unsigned char>(cur[start - 1]) >= 0x80))
        start--;
    std::string prefix = cur.substr(start, ucol - start);
    std::set<std::string> seen;
    auto add = [&](const std::string &label, FishLspKind kind, const std::string &detail, const std::string &docs,
                   size_t from) {
        if (label.compare(0, ucol - from, cur.substr(from, ucol - from)) != 0) return;
        if (!seen.insert(label).second) return;
        FishLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = docs;
        it.replace_start = static_cast<int>(from);
        it.replace_end = col;
        out.push_back(it);
    };
    // `$name`: variables in scope.
    size_t vstart = ucol;
    while (vstart > 0 && IsNameByte(cur[vstart - 1])) vstart--;
    if (vstart > 0 && cur[vstart - 1] == '$') {
        const size_t off = doc.Off(line, col);
        const int f = InnermostFunc(doc, off);
        for (const VarOcc &v : doc.vars)
            if (v.def && (v.key == -1 || v.key == f))
                add(v.name, FishLspKind::Variable, v.key == -1 ? "variable" : "local variable", "", vstart);
        if (f >= 0) add("argv", FishLspKind::Variable, "the function's arguments", "", vstart);
        for (const char *w : kWellKnownVars) add(w, FishLspKind::Variable, "fish variable", "", vstart);
        return out;
    }
    bool at_command = false;
    const std::vector<std::string> words = WordsBefore(cur, start, &at_command);
    if (!words.empty() && words[0] == "#") return out;
    if (at_command) {
        for (const char *k : kKeywords) add(k, FishLspKind::Keyword, "keyword", "", start);
        for (const FuncInfo &f : doc.funcs)
            if (!f.name.empty()) add(f.name, FishLspKind::Function, FunctionSignature(f), f.desc, start);
        size_t n = 0;
        const FishVocabEntry *vocab = FishVocab(&n);
        for (size_t i = 0; i < n; i++)
            add(vocab[i].name, vocab[i].builtin ? FishLspKind::Keyword : FishLspKind::Function,
                vocab[i].builtin ? "builtin" : "function", vocab[i].synopsis, start);
        return out;
    }
    const std::string &cmd = words[0];
    if (!prefix.empty() && prefix[0] == '-') {
        const FishVocabEntry *e = FishVocabFind(cmd.data(), cmd.size());
        if (e) {
            const std::string opts = e->options;
            size_t i = 0;
            while (i < opts.size()) {
                size_t j = opts.find(' ', i);
                if (j == std::string::npos) j = opts.size();
                add(opts.substr(i, j - i), FishLspKind::Property, std::string(cmd) + " option", "", start);
                i = j + 1;
            }
        }
        return out;
    }
    if (cmd == "set" || cmd == "read" || cmd == "functions" || cmd == "type") {
        const size_t off = doc.Off(line, col);
        const int f = InnermostFunc(doc, off);
        if (cmd == "set" || cmd == "read") {
            for (const VarOcc &v : doc.vars)
                if (v.def && (v.key == -1 || v.key == f)) add(v.name, FishLspKind::Variable, "variable", "", start);
        } else {
            for (const FuncInfo &fn : doc.funcs)
                if (!fn.name.empty()) add(fn.name, FishLspKind::Function, FunctionSignature(fn), fn.desc, start);
        }
    }
    if (cmd == "string" || cmd == "path" || cmd == "status") {
        if (words.size() == 1) {
            const char *const *subs = cmd == "string" ? kStringSubs : cmd == "path" ? kPathSubs : kStatusSubs;
            const size_t ns = cmd == "string"  ? sizeof(kStringSubs) / sizeof(kStringSubs[0])
                              : cmd == "path" ? sizeof(kPathSubs) / sizeof(kPathSubs[0])
                                              : sizeof(kStatusSubs) / sizeof(kStatusSubs[0]);
            for (size_t i = 0; i < ns; i++) add(subs[i], FishLspKind::Method, cmd + " subcommand", "", start);
        }
    }
    return out;
}

// --- hover / signature ---------------------------------------------------------------------

FishLspHoverInfo FishLspHover(const FishLspDocument &doc, int line, int col) {
    FishLspHoverInfo h;
    const size_t off = doc.Off(line, col);
    if (const VarOcc *v = VarAt(doc, off)) {
        h.found = true;
        h.range = doc.RangeOf(v->start, v->len);
        std::string md = "```fish\n$" + v->name + "\n```\n";
        const VarOcc *def = nullptr;
        for (const VarOcc &o : doc.vars)
            if (o.def && o.name == v->name && o.key == v->key) {
                def = &o;
                break;
            }
        if (def) {
            int dl = 0, dc = 0;
            doc.Pos(def->start, &dl, &dc);
            const std::string &src_line = doc.lines[static_cast<size_t>(dl)];
            size_t a = src_line.find_first_not_of(" \t");
            md += (v->key >= 0 ? "local to `" + doc.funcs[static_cast<size_t>(v->key)].name + "`" : std::string("global")) +
                  ", set on line " + std::to_string(dl + 1) + ":\n```fish\n" +
                  (a == std::string::npos ? src_line : src_line.substr(a)) + "\n```";
        } else if (InList(v->name, kWellKnownVars) || v->name == "argv") {
            md += "a variable fish sets";
        } else {
            md += "not set in this file";
        }
        h.text = md;
        return h;
    }
    std::string name;
    size_t s = 0, len = 0;
    if (FuncNameAt(doc, off, &name, &s, &len)) {
        const FuncInfo &f = doc.funcs[static_cast<size_t>(FuncByName(doc, name))];
        h.found = true;
        h.range = doc.RangeOf(s, len);
        int dl = 0, dc = 0;
        doc.Pos(f.name_start, &dl, &dc);
        h.text = "```fish\nfunction " + FunctionSignature(f) + "\n```\n";
        if (!f.desc.empty()) h.text += f.desc + "\n\n";
        h.text += "defined on line " + std::to_string(dl + 1);
        return h;
    }
    if (const CmdOcc *c = CmdAt(doc, off)) {
        if (const FishVocabEntry *e = FishVocabFind(c->name.data(), c->name.size())) {
            h.found = true;
            h.range = doc.RangeOf(c->start, c->len);
            h.text = VocabMarkdown(*e);
            return h;
        }
    }
    // Keywords (if / for / while ... are leaves of block statements, not commands).
    size_t ws = off, we = off;
    while (ws > 0 && IsNameByte(static_cast<char>(doc.src.text[ws - 1] < 0x80 ? doc.src.text[ws - 1] : 0))) ws--;
    while (we < doc.src.text.size() && IsNameByte(static_cast<char>(doc.src.text[we] < 0x80 ? doc.src.text[we] : 0)))
        we++;
    if (we > ws) {
        const std::string word = U8(doc.Text(ws, we - ws));
        if (InList(word, kKeywords)) {
            if (const FishVocabEntry *e = FishVocabFind(word.data(), word.size())) {
                h.found = true;
                h.range = doc.RangeOf(ws, we - ws);
                h.text = VocabMarkdown(*e);
            }
        }
    }
    return h;
}

FishLspSignatureHelp FishLspSignatureAt(const FishLspDocument &doc, int line, int col) {
    FishLspSignatureHelp sh;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return sh;
    const std::string &cur = doc.lines[static_cast<size_t>(line)];
    const size_t ucol = static_cast<size_t>(std::max(0, std::min(col, static_cast<int>(cur.size()))));
    bool at_command = false;
    const std::vector<std::string> words = WordsBefore(cur, ucol, &at_command);
    if (words.empty() || words[0] == "#") return sh;
    const std::string &cmd = words[0];
    const bool typing = ucol > 0 && cur[ucol - 1] != ' ' && cur[ucol - 1] != '\t';
    const int arg_index = static_cast<int>(words.size()) - 1 - (typing ? 1 : 0);  // 0-based argument
    const int fi = FuncByName(doc, cmd);
    if (fi >= 0) {
        const FuncInfo &f = doc.funcs[static_cast<size_t>(fi)];
        sh.found = true;
        sh.label = FunctionSignature(f);
        sh.params = f.args;
        sh.documentation = f.desc;
        sh.active_param = std::max(0, std::min(arg_index, static_cast<int>(f.args.size()) - 1));
        return sh;
    }
    const FishVocabEntry *e = FishVocabFind(cmd.data(), cmd.size());
    if (!e || !*e->synopsis) return sh;
    // Pick the synopsis line for the subcommand being typed, if the command has them.
    std::string syn = e->synopsis;
    std::vector<std::string> lines;
    size_t i = 0;
    while (i <= syn.size()) {
        size_t j = syn.find('\n', i);
        if (j == std::string::npos) j = syn.size();
        lines.push_back(syn.substr(i, j - i));
        i = j + 1;
    }
    std::string label = lines[0];
    if (words.size() >= 2) {
        for (const std::string &l : lines)
            if (l.compare(0, cmd.size() + 1 + words[1].size() + 1, cmd + " " + words[1] + " ") == 0) {
                label = l;
                break;
            }
    }
    sh.found = true;
    sh.label = label;
    // Parameters: the top-level items after the command word(s).
    size_t p = label.find(' ');
    int depth = 0;
    std::string item;
    for (size_t k = p == std::string::npos ? label.size() : p + 1; k <= label.size(); k++) {
        const char c = k < label.size() ? label[k] : ' ';
        if (c == '[' || c == '(') depth++;
        if (c == ']' || c == ')') depth--;
        if (c == ' ' && depth == 0) {
            if (!item.empty()) sh.params.push_back(item);
            item.clear();
        } else {
            item += c;
        }
    }
    if (!sh.params.empty()) sh.active_param = std::max(0, std::min(arg_index, static_cast<int>(sh.params.size()) - 1));
    sh.documentation = e->builtin ? "fish builtin" : "function shipped with fish";
    return sh;
}

// --- navigation ------------------------------------------------------------------------------

std::vector<FishLspLocation> FishLspDefinition(const FishLspDocument &doc, int line, int col) {
    std::vector<FishLspLocation> out;
    const size_t off = doc.Off(line, col);
    if (const VarOcc *v = VarAt(doc, off)) {
        for (const VarOcc &o : doc.vars)
            if (o.def && o.name == v->name && o.key == v->key) out.push_back({doc.RangeOf(o.start, o.len)});
        return out;
    }
    std::string name;
    size_t s = 0, len = 0;
    if (FuncNameAt(doc, off, &name, &s, &len)) {
        for (const FuncInfo &f : doc.funcs)
            if (f.name == name) out.push_back({doc.RangeOf(f.name_start, f.name_len)});
    }
    return out;
}

std::vector<FishLspReference> FishLspReferences(const FishLspDocument &doc, int line, int col) {
    std::vector<FishLspReference> out;
    const size_t off = doc.Off(line, col);
    if (const VarOcc *v = VarAt(doc, off)) {
        for (const VarOcc &o : doc.vars)
            if (o.name == v->name && o.key == v->key) out.push_back({doc.RangeOf(o.start, o.len), o.def});
        return out;
    }
    std::string name;
    size_t s = 0, len = 0;
    if (FuncNameAt(doc, off, &name, &s, &len)) {
        for (const FuncInfo &f : doc.funcs)
            if (f.name == name) out.push_back({doc.RangeOf(f.name_start, f.name_len), true});
        for (const CmdOcc &c : doc.cmds)
            if (c.name == name) out.push_back({doc.RangeOf(c.start, c.len), false});
    }
    return out;
}

FishLspRenameTarget FishLspPrepareRename(const FishLspDocument &doc, int line, int col) {
    FishLspRenameTarget t;
    const size_t off = doc.Off(line, col);
    if (const VarOcc *v = VarAt(doc, off)) {
        if (InList(v->name, kWellKnownVars) || v->name == "argv") return t;
        bool any_def = false;
        for (const VarOcc &o : doc.vars) {
            if (o.name != v->name || o.key != v->key) continue;
            if (!o.renamable) return t;
            any_def = any_def || o.def;
        }
        if (!any_def) return t;
        t.found = true;
        t.range = doc.RangeOf(v->start, v->len);
        t.placeholder = v->name;
        return t;
    }
    std::string name;
    size_t s = 0, len = 0;
    if (FuncNameAt(doc, off, &name, &s, &len)) {
        // Only names written plainly can be renamed by replacing their text.
        for (const FuncInfo &f : doc.funcs)
            if (f.name == name && U8(doc.Text(f.name_start, f.name_len)) != name) return t;
        t.found = true;
        t.range = doc.RangeOf(s, len);
        t.placeholder = name;
    }
    return t;
}

std::vector<FishLspTextEdit> FishLspRename(const FishLspDocument &doc, int line, int col, const std::string &new_name,
                                           std::string *error) {
    std::vector<FishLspTextEdit> edits;
    const FishLspRenameTarget t = FishLspPrepareRename(doc, line, col);
    if (!t.found) {
        *error = "nothing to rename here";
        return edits;
    }
    const size_t off = doc.Off(line, col);
    if (const VarOcc *v = VarAt(doc, off)) {
        const fp::Source ns = fp::Source::FromUtf8(new_name);
        if (!fp::ValidVarName(ns.text)) {
            *error = "'" + new_name + "' is not a valid variable name";
            return edits;
        }
        for (const VarOcc &o : doc.vars)
            if (o.name == v->name && o.key == v->key) edits.push_back({doc.RangeOf(o.start, o.len), new_name});
        return edits;
    }
    if (new_name.empty() || new_name[0] == '-' || new_name.find_first_of("/ \t\n'\"$()[]{};|&<>*?\\#") != std::string::npos ||
        InList(new_name, kReservedWords)) {
        *error = "'" + new_name + "' is not a valid function name";
        return edits;
    }
    for (const FishLspReference &r : FishLspReferences(doc, line, col)) edits.push_back({r.range, new_name});
    return edits;
}

// --- outline / folding ------------------------------------------------------------------------

std::vector<FishLspSymbol> FishLspSymbols(const FishLspDocument &doc) {
    std::vector<FishLspSymbol> out;
    std::vector<int> sym_of(doc.funcs.size(), -1);
    for (size_t i = 0; i < doc.funcs.size(); i++) {
        const FuncInfo &f = doc.funcs[i];
        if (f.name.empty()) continue;
        FishLspSymbol s;
        s.name = f.name;
        s.detail = f.desc;
        s.kind = FishLspSymbolKind::Function;
        s.range = doc.RangeOf(f.start, f.end - f.start);
        s.sel_range = doc.RangeOf(f.name_start, f.name_len);
        int p = f.parent;
        while (p >= 0 && sym_of[static_cast<size_t>(p)] < 0) p = doc.funcs[static_cast<size_t>(p)].parent;
        s.parent = p >= 0 ? sym_of[static_cast<size_t>(p)] : -1;
        sym_of[i] = static_cast<int>(out.size());
        out.push_back(s);
    }
    std::set<std::string> seen;
    for (const VarOcc &v : doc.vars) {
        if (!v.def || v.func >= 0 || v.kind != DefKind::Set) continue;
        if (!seen.insert(v.name).second) continue;
        FishLspSymbol s;
        s.name = v.name;
        s.kind = FishLspSymbolKind::Variable;
        s.range = doc.RangeOf(v.start, v.len);
        s.sel_range = s.range;
        out.push_back(s);
    }
    for (const Stmt &st : doc.stmts) {
        if ((st.cmd == "abbr" || st.cmd == "alias") && !st.args.empty()) {
            size_t i = 0;
            while (i < st.args.size() && st.arg_plain[i] && st.args[i].size() > 1 && st.args[i][0] == '-') i++;
            if (i >= st.args.size() || !st.arg_plain[i]) continue;
            std::string name = st.args[i].substr(0, st.args[i].find('='));
            if (name.empty()) continue;
            FishLspSymbol s;
            s.name = name;
            s.detail = st.cmd;
            s.kind = st.cmd == "abbr" ? FishLspSymbolKind::Constant : FishLspSymbolKind::Function;
            s.range = doc.RangeOf(st.cmd_start, st.end - st.cmd_start);
            s.sel_range = doc.RangeOf(st.arg_ranges[i].first, name.size());
            out.push_back(s);
        }
    }
    return out;
}

std::vector<FishLspFold> FishLspFolds(const FishLspDocument &doc) {
    std::vector<FishLspFold> out;
    std::set<std::pair<int, int>> seen;
    auto fold = [&](size_t a, size_t b, const char *kind) {
        int la = 0, lb = 0, c = 0;
        doc.Pos(a, &la, &c);
        doc.Pos(b, &lb, &c);
        if (lb - 1 <= la) return;
        if (!seen.insert({la, lb - 1}).second) return;
        out.push_back({la, lb - 1, kind});
    };
    for (const auto &b : doc.blocks) fold(b.start, b.end, "");
    for (const auto &b : doc.case_items) {
        int la = 0, lb = 0, c = 0;
        doc.Pos(b.start, &la, &c);
        doc.Pos(b.end, &lb, &c);
        if (lb > la && seen.insert({la, lb}).second) out.push_back({la, lb, ""});
    }
    // Runs of whole-line comments.
    int run_start = -1, prev = -2;
    auto close_run = [&]() {
        if (run_start >= 0 && prev > run_start && seen.insert({run_start, prev}).second)
            out.push_back({run_start, prev, "comment"});
    };
    for (const fp::Range &c : doc.ast.comments) {
        int l = 0, col = 0;
        doc.Pos(c.start, &l, &col);
        const std::string &text = doc.lines[static_cast<size_t>(l)];
        if (text.find_first_not_of(" \t") != static_cast<size_t>(col)) continue;
        if (l != prev + 1) {
            close_run();
            run_start = l;
        }
        prev = l;
    }
    close_run();
    std::sort(out.begin(), out.end(), [](const FishLspFold &a, const FishLspFold &b) {
        return a.start_line != b.start_line ? a.start_line < b.start_line : a.end_line > b.end_line;
    });
    return out;
}

// --- formatting -------------------------------------------------------------------------------

std::vector<FishLspTextEdit> FishLspFormat(const std::vector<std::string> &lines, int width, std::string *error) {
    std::vector<FishLspTextEdit> edits;
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    fishfmt::Options o;
    if (width > 0) o.width = width;
    const fishfmt::Result r = fishfmt::Format(text, o);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    // The editor's lines carry no final newline; fish_indent's output ends with one.
    std::string formatted = r.text;
    if (!formatted.empty() && formatted.back() == '\n') formatted.pop_back();
    if (formatted == text) return edits;
    std::vector<std::string> b;
    {
        size_t i = 0;
        while (true) {
            const size_t j = formatted.find('\n', i);
            if (j == std::string::npos) {
                b.push_back(formatted.substr(i));
                break;
            }
            b.push_back(formatted.substr(i, j - i));
            i = j + 1;
        }
    }
    size_t pre = 0;
    while (pre < lines.size() && pre < b.size() && lines[pre] == b[pre]) pre++;
    size_t suf = 0;
    while (suf < lines.size() - pre && suf < b.size() - pre && lines[lines.size() - 1 - suf] == b[b.size() - 1 - suf])
        suf++;
    FishLspTextEdit e;
    e.range.line = static_cast<int>(pre);
    e.range.col = 0;
    const size_t a_end = lines.size() - suf;
    const size_t b_end = b.size() - suf;
    if (a_end < lines.size()) {
        e.range.end_line = static_cast<int>(a_end);
        e.range.end_col = 0;
        for (size_t k = pre; k < b_end; k++) e.new_text += b[k] + "\n";
    } else {
        e.range.end_line = static_cast<int>(lines.size()) - 1;
        e.range.end_col = static_cast<int>(lines.back().size());
        for (size_t k = pre; k < b_end; k++) {
            if (k > pre) e.new_text += "\n";
            e.new_text += b[k];
        }
        if (pre > 0 && pre == lines.size() && b_end > pre) {
            e.range.line = static_cast<int>(pre) - 1;
            e.range.col = static_cast<int>(lines[pre - 1].size());
            e.new_text = "\n" + e.new_text;
        }
    }
    edits.push_back(e);
    return edits;
}
