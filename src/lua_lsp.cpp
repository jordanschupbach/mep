// mep's Lua language server, analysis half: see lua_lsp.h.
//
// Everything sits on lua_parse.h, a port of Lua 5.4's own lexer and parser:
// its first error is luac's, and it resolves every name the way the
// compiler does (local, upvalue, else a global through _ENV). On top of
// that tree:
//
//   - lints in the spirit of luacheck (unused locals and arguments, values
//     assigned and never read, shadowing, unbalanced assignments, duplicate
//     table keys, unreachable code, empty branches, string.format argument
//     counts, names from other Lua versions), and an undefined-global check
//     that stays silent when the file plainly runs in a host it cannot see;
//   - hover, completion and signature help from the reference manual and
//     mep's own API (lua_lsp_vocab.cpp, generated), plus the file's own
//     functions and the members it assigns to its tables;
//   - definition, references and rename by real scope, document symbols and
//     folding ranges.
#include "lua_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

#include "lua_format.h"
#include "lua_lsp_vocab.h"
#include "lua_parse.h"

using luaparse::Block;
using luaparse::FuncBody;
using luaparse::Kind;
using luaparse::Node;
using luaparse::TableField;
using luaparse::Var;

namespace {

// ---- vocabulary ------------------------------------------------------------------------------------------------

const std::unordered_map<std::string, const LuaLspApi *> &ApiIndex() {
    static const std::unordered_map<std::string, const LuaLspApi *> index = [] {
        std::unordered_map<std::string, const LuaLspApi *> m;
        size_t n = 0;
        const LuaLspApi *t = LuaLspApiTable(&n);
        for (size_t i = 0; i < n; i++) m.emplace(t[i].name, &t[i]);
        return m;
    }();
    return index;
}

const LuaLspApi *FindApi(const std::string &name) {
    const auto &m = ApiIndex();
    auto it = m.find(name);
    return it == m.end() ? nullptr : it->second;
}

// Globals of the hosts Lua is usually embedded in. A file that uses them is
// not wrong, only written for a host this server does not model.
const std::set<std::string> &HostGlobals() {
    static const std::set<std::string> s = {
        // standalone lua
        "arg",
        // LuaJIT
        "jit", "bit", "ffi",
        // Neovim
        "vim",
        // LÖVE
        "love",
        // awesome wm
        "awesome", "client", "screen", "root", "tag", "mouse", "mousegrabber", "keygrabber", "selection",
        // Hammerspoon
        "hs", "spoon",
        // OpenResty / nginx, Redis scripting
        "ngx", "ndk", "redis", "KEYS", "ARGV", "cjson",
        // busted / luassert
        "describe", "it", "insulate", "expose", "before_each", "after_each", "setup", "teardown", "lazy_setup",
        "lazy_teardown", "strict_setup", "strict_teardown", "pending", "spy", "stub", "mock", "finally",
        "randomize",
        // wireshark, conky, mpv, minetest, roblox (a few unmistakable roots)
        "mp", "minetest", "core", "game", "workspace", "script",
        // LÖVR
        "lovr", "vec2", "vec3", "vec4", "quat", "mat4", "Vec2", "Vec3", "Vec4", "Quat", "Mat4",
    };
    return s;
}

bool IsKnownGlobal(const std::string &name) {
    if (name == "_ENV") return true;
    const LuaLspApi *a = FindApi(name);
    if (a && std::strcmp(a->source, "old") != 0) return true;
    return HostGlobals().count(name) > 0;
}

const char *const kKeywords[] = {"and",  "break", "do",     "else", "elseif", "end",   "false", "for",
                                 "function", "goto", "if", "in",   "local",  "nil",   "not",   "or",
                                 "repeat",   "return", "then", "true", "until", "while"};

bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool IsIdentifier(const std::string &s) {
    if (s.empty() || !IsIdentStart(s[0])) return false;
    for (char c : s) {
        if (!IsWordChar(c)) return false;
    }
    return !luaparse::IsReserved(s);
}

// ---- tree walking ----------------------------------------------------------------------------------------------

struct Ctx {
    Node *parent = nullptr;
    FuncBody *func = nullptr;  // innermost enclosing function (null: main chunk)
};

using Visitor = std::function<void(Node *, const Ctx &)>;

void WalkNode(Node *n, const Ctx &ctx, const Visitor &f);

void WalkBlock(const Block &b, const Ctx &ctx, const Visitor &f) {
    for (Node *s : b.stats) WalkNode(s, ctx, f);
}

void WalkNode(Node *n, const Ctx &ctx, const Visitor &f) {
    if (!n) return;
    f(n, ctx);
    Ctx c;
    c.parent = n;
    c.func = ctx.func;
    switch (n->kind) {
        case Kind::Binary:
        case Kind::Index:
            WalkNode(n->a, c, f);
            WalkNode(n->b, c, f);
            break;
        case Kind::Unary:
        case Kind::Paren:
        case Kind::Field:
        case Kind::CallStat:
            WalkNode(n->a, c, f);
            break;
        case Kind::Call:
            WalkNode(n->a, c, f);
            for (Node *x : n->list) WalkNode(x, c, f);
            break;
        case Kind::Table:
            for (const TableField &tf : n->fields) {
                WalkNode(tf.key, c, f);
                WalkNode(tf.value, c, f);
            }
            break;
        case Kind::Function:
        case Kind::LocalFunction:
            if (n->func) {
                Ctx fc = c;
                fc.func = n->func;
                WalkBlock(n->func->body, fc, f);
            }
            break;
        case Kind::FunctionStat:
            if (!n->targets.empty()) WalkNode(n->targets.back(), c, f);
            if (n->func) {
                Ctx fc = c;
                fc.func = n->func;
                WalkBlock(n->func->body, fc, f);
            }
            break;
        case Kind::Local:
        case Kind::Return:
            for (Node *x : n->list) WalkNode(x, c, f);
            break;
        case Kind::Assign:
            for (Node *x : n->targets) WalkNode(x, c, f);
            for (Node *x : n->list) WalkNode(x, c, f);
            break;
        case Kind::Do:
            WalkBlock(n->block, c, f);
            break;
        case Kind::While:
            WalkNode(n->b, c, f);
            WalkBlock(n->block, c, f);
            break;
        case Kind::Repeat:
            WalkBlock(n->block, c, f);
            WalkNode(n->b, c, f);
            break;
        case Kind::If:
            for (const luaparse::IfClause &cl : n->clauses) {
                WalkNode(cl.cond, c, f);
                WalkBlock(cl.body, c, f);
            }
            break;
        case Kind::NumFor:
            WalkNode(n->a, c, f);
            WalkNode(n->b, c, f);
            WalkNode(n->c, c, f);
            WalkBlock(n->block, c, f);
            break;
        case Kind::GenFor:
            for (Node *x : n->list) WalkNode(x, c, f);
            WalkBlock(n->block, c, f);
            break;
        default:
            break;
    }
}

// Every block in the tree, with the statement that owns it.
void ForEachBlock(const Block &root, const std::function<void(const Block &, Node *owner, const char *what)> &f) {
    f(root, nullptr, "chunk");
    Ctx ctx;
    WalkBlock(root, ctx, [&](Node *n, const Ctx &) {
        switch (n->kind) {
            case Kind::Function:
            case Kind::LocalFunction:
            case Kind::FunctionStat:
                if (n->func) f(n->func->body, n, "function");
                break;
            case Kind::Do:
                f(n->block, n, "do");
                break;
            case Kind::While:
                f(n->block, n, "while");
                break;
            case Kind::Repeat:
                f(n->block, n, "repeat");
                break;
            case Kind::NumFor:
            case Kind::GenFor:
                f(n->block, n, "for");
                break;
            case Kind::If:
                for (size_t i = 0; i < n->clauses.size(); i++)
                    f(n->clauses[i].body, n, i == 0 ? "if" : n->clauses[i].cond ? "elseif" : "else");
                break;
            default:
                break;
        }
    });
}

std::string OneLine(const std::string &s, size_t max = 80) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == '\n' || c == '\t' || c == ' ' || c == '\r') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    if (out.size() > max) out = out.substr(0, max - 3) + "...";
    return out;
}

}  // namespace

// ---- the document ----------------------------------------------------------------------------------------------

struct LuaHit {
    int start = 0, end = 0;
    Node *node = nullptr;  // Name / Field / method Call / Goto / Label
    Var *var = nullptr;    // a declaration (node is null) or a Name's local
};

struct LuaMemberDef {
    std::string key;          // "#<var id>.f.g" for a local root, "name.f.g" for a global root
    int name_start = 0, name_end = 0;
    Node *value = nullptr;    // the assigned value (a Function for a function definition)
    FuncBody *func = nullptr;
    bool method = false;
    int line = 0;
};

class LuaLspDocument {
public:
    std::vector<std::string> lines;
    std::string text;
    std::vector<int> line_starts;
    std::unique_ptr<luaparse::ParseResult> parse;
    std::vector<LuaHit> hits;  // sorted by start
    std::unordered_map<const Node *, Ctx> ctx;
    std::unordered_map<const Var *, int> var_id;
    std::vector<LuaMemberDef> members;
    std::map<std::string, Node *> global_defs;  // first assignment of each global
    std::set<std::string> luacheck_globals;      // -- luacheck: globals a b
    std::set<int> ignored_lines;                 // -- luacheck: ignore (on that line)
    bool ignore_all = false;

    void Pos(int off, int *line, int *col) const {
        off = std::max(0, std::min(off, static_cast<int>(text.size())));
        auto it = std::upper_bound(line_starts.begin(), line_starts.end(), off);
        const int l = static_cast<int>(it - line_starts.begin()) - 1;
        *line = l;
        *col = off - line_starts[static_cast<size_t>(l)];
    }
    int Off(int line, int col) const {
        if (line < 0) return 0;
        if (static_cast<size_t>(line) >= line_starts.size()) return static_cast<int>(text.size());
        const int ls = line_starts[static_cast<size_t>(line)];
        const int len = static_cast<int>(lines[static_cast<size_t>(line)].size());
        return ls + std::max(0, std::min(col, len));
    }
    LuaLspRange Range(int a, int b) const {
        LuaLspRange r;
        Pos(a, &r.line, &r.col);
        Pos(b, &r.end_line, &r.end_col);
        return r;
    }
    int LineOf(int off) const {
        int l = 0, c = 0;
        Pos(off, &l, &c);
        return l;
    }
    std::string Slice(int a, int b) const {
        a = std::max(0, a);
        b = std::min(b, static_cast<int>(text.size()));
        return b > a ? text.substr(static_cast<size_t>(a), static_cast<size_t>(b - a)) : std::string();
    }
    const LuaHit *HitAt(int off) const {
        const LuaHit *best = nullptr;
        for (const LuaHit &h : hits) {
            if (h.start > off) break;
            if (off <= h.end && (!best || h.start >= best->start)) best = &h;
        }
        return best;
    }
    std::string VarKey(const Var *v) const {
        auto it = var_id.find(v);
        return "#" + std::to_string(it == var_id.end() ? -1 : it->second);
    }
    int ScopeEnd(const Var *v) const { return v->scope_end > 0 ? v->scope_end : static_cast<int>(text.size()); }
};

namespace {

// "a.b.c" for a chain of fields over a name: the root's key (a local's "#id"
// or a global's name) and the path after it. False for anything else.
bool ChainKey(const LuaLspDocument &doc, const Node *n, std::string *key) {
    std::vector<std::string> parts;
    while (n && n->kind == Kind::Field) {
        parts.push_back(n->text);
        n = n->a;
    }
    if (!n || n->kind != Kind::Name) return false;
    std::string k = n->var ? doc.VarKey(n->var) : n->custom_env ? std::string() : n->text;
    if (k.empty()) return false;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) k += "." + *it;
    *key = k;
    return true;
}

// The same for display: "M.f" whatever M is.
std::string ChainText(const Node *n) {
    if (!n) return std::string();
    if (n->kind == Kind::Name) return n->text;
    if (n->kind == Kind::Field) {
        const std::string base = ChainText(n->a);
        return base.empty() ? std::string() : base + "." + n->text;
    }
    return std::string();
}

std::string ParamList(const FuncBody *f) {
    std::string s = "(";
    bool first = true;
    for (const Var *p : f->params) {
        if (p->kind == Var::Self) continue;
        if (!first) s += ", ";
        s += p->name;
        first = false;
    }
    if (f->vararg) s += first ? "..." : ", ...";
    return s + ")";
}

void RecordTableMembers(LuaLspDocument &doc, const std::string &key, const Node *table, int depth) {
    if (!table || table->kind != Kind::Table || depth > 4) return;
    for (const TableField &tf : table->fields) {
        if (tf.kind != TableField::Named) continue;
        LuaMemberDef m;
        m.key = key + "." + tf.name;
        m.name_start = tf.name_start;
        m.name_end = tf.name_end;
        m.value = tf.value;
        m.func = tf.value && tf.value->kind == Kind::Function ? tf.value->func : nullptr;
        m.line = doc.LineOf(tf.name_start);
        doc.members.push_back(m);
        RecordTableMembers(doc, m.key, tf.value, depth + 1);
    }
}

void Index(LuaLspDocument &doc) {
    luaparse::ParseResult &pr = *doc.parse;
    for (size_t i = 0; i < pr.vars.size(); i++) {
        doc.var_id[pr.vars[i]] = static_cast<int>(i);
        doc.hits.push_back({pr.vars[i]->start, pr.vars[i]->end, nullptr, pr.vars[i]});
    }
    Ctx root;
    WalkBlock(pr.chunk, root, [&](Node *n, const Ctx &c) {
        doc.ctx[n] = c;
        switch (n->kind) {
            case Kind::Name:
                doc.hits.push_back({n->name_start, n->name_end, n, n->var});
                if (!n->var && n->is_write && !n->custom_env && !doc.global_defs.count(n->text))
                    doc.global_defs[n->text] = n;
                break;
            case Kind::Field:
            case Kind::Goto:
            case Kind::Label:
                doc.hits.push_back({n->name_start, n->name_end, n, nullptr});
                break;
            case Kind::Call:
                if (n->method) doc.hits.push_back({n->name_start, n->name_end, n, nullptr});
                break;
            case Kind::FunctionStat:
                if (n->targets.size() > 1) {
                    std::string key;
                    if (ChainKey(doc, n->targets.back(), &key)) {
                        LuaMemberDef m;
                        m.key = key;
                        m.name_start = n->targets.back()->name_start;
                        m.name_end = n->targets.back()->name_end;
                        m.value = n;
                        m.func = n->func;
                        m.method = n->method;
                        m.line = n->line;
                        doc.members.push_back(m);
                    }
                }
                break;
            case Kind::Assign:
                for (size_t i = 0; i < n->targets.size(); i++) {
                    const Node *t = n->targets[i];
                    Node *v = i < n->list.size() ? n->list[i] : nullptr;
                    std::string key;
                    if (t->kind == Kind::Field && ChainKey(doc, t, &key)) {
                        LuaMemberDef m;
                        m.key = key;
                        m.name_start = t->name_start;
                        m.name_end = t->name_end;
                        m.value = v;
                        m.func = v && v->kind == Kind::Function ? v->func : nullptr;
                        m.line = doc.LineOf(t->name_start);
                        doc.members.push_back(m);
                        RecordTableMembers(doc, key, v, 0);
                        // _G.x = ... defines the global x.
                        if (t->a && t->a->kind == Kind::Name && !t->a->var && t->a->text == "_G" &&
                            !doc.global_defs.count(t->text))
                            doc.global_defs[t->text] = const_cast<Node *>(t);
                    } else if (t->kind == Kind::Name && ChainKey(doc, t, &key)) {
                        RecordTableMembers(doc, key, v, 0);
                    }
                }
                break;
            case Kind::Local:
                for (size_t i = 0; i < n->vars.size() && i < n->list.size(); i++)
                    RecordTableMembers(doc, doc.VarKey(n->vars[i]), n->list[i], 0);
                break;
            default:
                break;
        }
    });
    std::sort(doc.hits.begin(), doc.hits.end(),
              [](const LuaHit &a, const LuaHit &b) { return a.start < b.start; });
    // luacheck inline options.
    for (const luaparse::Comment &cm : pr.comments) {
        const std::string body = doc.Slice(cm.start, cm.end);
        const size_t at = body.find("luacheck:");
        if (at == std::string::npos) continue;
        std::string rest = body.substr(at + 9);
        size_t i = 0;
        while (i < rest.size() && std::isspace(static_cast<unsigned char>(rest[i]))) i++;
        std::string word;
        while (i < rest.size() && IsWordChar(rest[i])) word += rest[i++];
        std::vector<std::string> names;
        while (i < rest.size()) {
            while (i < rest.size() && !IsWordChar(rest[i])) i++;
            std::string w;
            while (i < rest.size() && IsWordChar(rest[i])) w += rest[i++];
            if (!w.empty()) names.push_back(w);
        }
        if (word == "globals" || word == "read_globals" || word == "new_globals" || word == "new_read_globals") {
            doc.luacheck_globals.insert(names.begin(), names.end());
        } else if (word == "ignore" || word == "no") {
            // On a line of its own at the top: the whole file; after code: that line.
            const int l = doc.LineOf(cm.start);
            const std::string &line = doc.lines[static_cast<size_t>(l)];
            const size_t first = line.find_first_not_of(" \t");
            const bool own_line = first != std::string::npos &&
                                  doc.line_starts[static_cast<size_t>(l)] + static_cast<int>(first) == cm.start;
            if (own_line && names.empty())
                doc.ignore_all = true;
            else
                doc.ignored_lines.insert(l);
        }
    }
}

}  // namespace

std::shared_ptr<const LuaLspDocument> LuaLspAnalyze(const std::vector<std::string> &lines) {
    auto doc = std::make_shared<LuaLspDocument>();
    doc->lines = lines;
    if (doc->lines.empty()) doc->lines.push_back(std::string());
    for (size_t i = 0; i < doc->lines.size(); i++) {
        doc->line_starts.push_back(static_cast<int>(doc->text.size()));
        if (i) {
            doc->line_starts.back()++;
            doc->text += '\n';
        }
        doc->text += doc->lines[i];
    }
    doc->parse = luaparse::Parse(doc->text);
    Index(*doc);
    return doc;
}

// ---- diagnostics -----------------------------------------------------------------------------------------------

namespace {

int FormatSpecifiers(const std::string &fmt, bool *ok) {
    int n = 0;
    *ok = true;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') continue;
        i++;
        if (i < fmt.size() && fmt[i] == '%') continue;
        while (i < fmt.size() && std::strchr("-+ #0", fmt[i])) i++;
        while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) i++;
        if (i < fmt.size() && fmt[i] == '.') {
            i++;
            while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) i++;
        }
        if (i >= fmt.size() || !std::strchr("cdiouxXaAfeEgGqsp", fmt[i])) {
            *ok = false;
            return 0;
        }
        n++;
    }
    return n;
}

const Node *InitializerOf(const Var *v) {
    if (!v->decl) return nullptr;
    if (v->decl->kind == Kind::Local) {
        for (size_t i = 0; i < v->decl->vars.size(); i++) {
            if (v->decl->vars[i] == v) return i < v->decl->list.size() ? v->decl->list[i] : nullptr;
        }
    }
    return nullptr;
}

bool MultiValued(const Node *e) { return e && (e->kind == Kind::Call || e->kind == Kind::Vararg); }

bool BlockHasComment(const LuaLspDocument &doc, int a, int b) {
    for (const luaparse::Comment &c : doc.parse->comments) {
        if (c.start >= a && c.start < b) return true;
    }
    return false;
}

const char *VarWhat(const Var *v) {
    switch (v->kind) {
        case Var::Param:
            return "argument";
        case Var::ForVar:
            return "loop variable";
        case Var::LocalFunction:
            return "function";
        case Var::Self:
            return "self";
        default:
            return "variable";
    }
}


// Optimal string alignment distance, capped.
int EditDistance(const std::string &a, const std::string &b, int cap) {
    const size_t n = a.size(), m = b.size();
    if ((n > m ? n - m : m - n) > static_cast<size_t>(cap)) return cap + 1;
    std::vector<std::vector<int>> d(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = 0; i <= n; i++) d[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= m; j++) d[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; i++) {
        for (size_t j = 1; j <= m; j++) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
        }
    }
    return d[n][m];
}

// A known name the unknown global `g` is probably a misspelling of, or "".
std::string SuggestName(const LuaLspDocument &doc, const Node *g) {
    const std::string &name = g->text;
    if (name.size() < 3) return std::string();
    const int cap = name.size() <= 5 ? 1 : 2;
    std::string best;
    int best_d = cap + 1;
    auto consider = [&](const std::string &c) {
        if (c == name || c.size() < 3) return;
        // Case-only differences are a different name in a case-sensitive language, but still a typo.
        const int dist = EditDistance(name, c, cap);
        if (dist < best_d) {
            best_d = dist;
            best = c;
        }
    };
    for (const Var *v : doc.parse->vars) {
        if (v->scope_start <= g->start && g->start <= doc.ScopeEnd(v)) consider(v->name);
    }
    for (const auto &kv : doc.global_defs) consider(kv.first);
    size_t count = 0;
    const LuaLspApi *table = LuaLspApiTable(&count);
    for (size_t i = 0; i < count; i++) {
        if (std::strcmp(table[i].source, "old") == 0 || std::strchr(table[i].name, '.') || std::strchr(table[i].name, ':'))
            continue;
        consider(table[i].name);
    }
    return best;
}

}  // namespace

std::vector<LuaLspDiagnostic> LuaLspDiagnostics(const LuaLspDocument &doc) {
    std::vector<LuaLspDiagnostic> out;
    const luaparse::ParseResult &pr = *doc.parse;
    auto add = [&](int a, int b, LuaLspSeverity sev, const std::string &code, const std::string &msg) {
        LuaLspDiagnostic d;
        d.range = doc.Range(a, std::max(a, b));
        d.severity = sev;
        d.code = code;
        d.message = msg;
        out.push_back(d);
        return &out.back();
    };
    if (!pr.errors.empty()) {
        const luaparse::Error &e = pr.errors[0];
        int a = e.start, b = e.end;
        if (e.line <= 0 && pr.first_error_offset >= 0) a = b = pr.first_error_offset;
        if (b <= a && a < static_cast<int>(doc.text.size())) b = a + 1;
        add(a, b, LuaLspSeverity::Error, e.code.empty() ? "syntax-error" : e.code, e.message);
    }
    if (doc.ignore_all) return out;
    const bool broken = pr.has_error;
    const int cut = broken && pr.first_error_offset >= 0 ? pr.first_error_offset : static_cast<int>(doc.text.size());
    auto lint = [&](int a, int b, LuaLspSeverity sev, const std::string &code,
                    const std::string &msg) -> LuaLspDiagnostic * {
        if (a > cut) return nullptr;
        if (doc.ignored_lines.count(doc.LineOf(a))) return nullptr;
        return add(a, b, sev, code, msg);
    };

    // -- variables --
    for (const Var *v : pr.vars) {
        if (broken) break;  // a repaired tree loses uses: no unused-* on it
        if (v->name.empty() || v->name[0] == '_' || v->kind == Var::Self || v->attrib == Var::Close) continue;
        const bool read = !v->reads.empty();
        if (!read && v->writes.empty()) {
            if (v->kind == Var::Param) {
                // A callback, a method or a table's function implements an interface: its
                // parameters are dictated by the caller. Only a plain local or global function
                // is reported, and only for an argument it could drop (none after it is used).
                const Node *d = v->decl;
                bool own_signature = false;
                if (d && d->kind == Kind::LocalFunction) own_signature = true;
                if (d && d->kind == Kind::FunctionStat && d->targets.size() == 1) own_signature = true;
                if (d && d->kind == Kind::Function) {
                    auto it = doc.ctx.find(d);
                    own_signature = it != doc.ctx.end() && it->second.parent && it->second.parent->kind == Kind::Local;
                }
                if (!own_signature) continue;
                const FuncBody *f = nullptr;
                if (d && d->func) f = d->func;
                bool later_used = false;
                if (f) {
                    bool after = false;
                    for (const Var *p : f->params) {
                        if (p == v) {
                            after = true;
                            continue;
                        }
                        if (after && (!p->reads.empty() || !p->writes.empty())) later_used = true;
                    }
                    if (f->vararg) {
                        // `...` used after it: the argument is positional padding.
                        int a = f->open_paren, b = f->end_kw;
                        if (doc.Slice(a, b).find("...", static_cast<size_t>(f->close_paren - a)) != std::string::npos)
                            later_used = true;
                    }
                }
                if (later_used) continue;
                LuaLspDiagnostic *d2 = lint(v->start, v->end, LuaLspSeverity::Hint, "unused-argument",
                                            "unused argument '" + v->name + "'");
                if (d2) d2->unnecessary = true;
                continue;
            }
            const bool loop = v->kind == Var::ForVar;
            LuaLspDiagnostic *d2 = lint(v->start, v->end, loop ? LuaLspSeverity::Hint : LuaLspSeverity::Warning,
                                        loop ? "unused-loop-variable" : "unused-local",
                                        std::string("unused ") + VarWhat(v) + " '" + v->name + "'");
            if (d2) d2->unnecessary = true;
            continue;
        }
        if (!read && !v->writes.empty() && v->kind != Var::Param) {
            lint(v->start, v->end, LuaLspSeverity::Warning, "unused-value",
                 std::string(VarWhat(v)) + " '" + v->name + "' is assigned but never read");
        }
    }
    for (const Var *v : pr.vars) {
        const Var *s = v->shadows;
        if (!s || v->name.empty() || v->name[0] == '_' || v->kind == Var::Self) continue;
        // `local x = x or default`, `local print = print`: deliberate.
        bool reads_outer = false;
        if (v->kind == Var::Local && v->decl) {
            for (const Node *r : s->reads) {
                if (r->start >= v->decl->start && r->start < v->scope_start) reads_outer = true;
            }
        }
        if (reads_outer) continue;
        if (s->owner != v->owner) continue;  // a parameter or local over an outer function's name: ordinary
        const bool same_scope = s->owner == v->owner && v->decl && s->decl && v->decl->kind != Kind::Function &&
                                doc.ctx.count(v->decl) && doc.ctx.count(s->decl) &&
                                doc.ctx.at(v->decl).parent == doc.ctx.at(s->decl).parent && s->kind == Var::Local &&
                                v->kind == Var::Local;
        const int sl = doc.LineOf(s->start) + 1;
        lint(v->start, v->end, same_scope ? LuaLspSeverity::Hint : LuaLspSeverity::Information,
             same_scope ? "redefined-local" : "shadowed-local",
             std::string(same_scope ? "redefinition of " : "shadowing ") + VarWhat(s) + " '" + s->name +
                 "' from line " + std::to_string(sl));
    }

    // -- globals --
    if (!broken) {
        bool env_tricks = false;
        for (const Node *g : pr.globals) {
            if (g->custom_env || g->text == "setfenv" || g->text == "module" || g->text == "_ENV" ||
                g->text == "getfenv")
                env_tricks = true;
        }
        for (const Var *v : pr.vars) {
            if (v->name == "_ENV") env_tricks = true;
        }
        if (doc.text.find("setmetatable(_G") != std::string::npos ||
            doc.text.find("setmetatable (_G") != std::string::npos)
            env_tricks = true;
        // An unknown global is only reported when it looks like a typo of a name that is
        // known here: a global read can come from any file or host this server cannot see.
        std::map<std::string, std::string> suggestion;  // "" = no suggestion
        for (const Node *g : pr.globals) {
            if (env_tricks || g->is_write || g->custom_env) continue;
            if (IsKnownGlobal(g->text) || doc.global_defs.count(g->text) || doc.luacheck_globals.count(g->text) ||
                FindApi(g->text))
                continue;
            auto it = suggestion.find(g->text);
            if (it == suggestion.end()) it = suggestion.emplace(g->text, SuggestName(doc, g)).first;
            if (it->second.empty()) continue;
            lint(g->name_start, g->name_end, LuaLspSeverity::Warning, "undefined-global",
                 "undefined global '" + g->text + "'; did you mean '" + it->second + "'?");
        }
        // Names from other Lua versions, unless guarded (`table.unpack or unpack`) or the file
        // checks which Lua it runs on.
        const bool version_aware = doc.text.find("_VERSION") != std::string::npos ||
                                   doc.text.find("jit") != std::string::npos;
        // A name tested anywhere (`unpack or table.unpack`, `if setfenv then`, `type(f) == ...`)
        // marks the file as written for several versions: none of its uses is reported.
        std::set<std::string> tested;
        auto is_test = [&](const Node *g) {
            auto it = doc.ctx.find(g);
            const Node *child = g;
            const Node *p = it == doc.ctx.end() ? nullptr : it->second.parent;
            while (p) {
                if (p->kind == Kind::Binary && (p->text == "or" || p->text == "and" || p->text == "==" || p->text == "~="))
                    return true;
                if (p->kind == Kind::Unary && p->text == "not") return true;
                if (p->kind == Kind::If || p->kind == Kind::While) return true;
                if (p->kind == Kind::Call && p->a && p->a->kind == Kind::Name && p->a->text == "type" && p->a != child)
                    return true;
                const bool climb = p->kind == Kind::Paren || p->kind == Kind::Field || p->kind == Kind::Binary ||
                                   (p->kind == Kind::Call && p->a == child);
                if (!climb) return false;
                child = p;
                auto pit = doc.ctx.find(p);
                p = pit == doc.ctx.end() ? nullptr : pit->second.parent;
            }
            return false;
        };
        for (const Node *g : pr.globals) {
            if (!g->is_write && FindApi(g->text) && is_test(g)) tested.insert(g->text);
        }
        for (const Node *g : pr.globals) {
            if (g->is_write || g->custom_env || doc.global_defs.count(g->text) || tested.count(g->text)) continue;
            const LuaLspApi *old = FindApi(g->text);
            if (!old || std::strcmp(old->source, "old") != 0) continue;
            if (version_aware) continue;
            LuaLspDiagnostic *d = lint(g->name_start, g->name_end, LuaLspSeverity::Information, "deprecated-global",
                                       "'" + g->text + "' is not in Lua 5.4 (" + old->doc + ")");
            if (d) d->deprecated = true;
        }
        // Setting a global in a module (a file that returns its table).
        bool is_module = false;
        if (!pr.chunk.stats.empty() && pr.chunk.stats.back()->kind == Kind::Return &&
            pr.chunk.stats.back()->list.size() == 1) {
            const Node *r = pr.chunk.stats.back()->list[0];
            if (r->kind == Kind::Table) is_module = true;
            if (r->kind == Kind::Name && r->var) {
                const Node *init = InitializerOf(r->var);
                is_module = init && init->kind == Kind::Table;
            }
        }
        if (is_module) {
            for (const Node *g : pr.globals) {
                if (!g->is_write || g->custom_env || IsKnownGlobal(g->text) || doc.luacheck_globals.count(g->text))
                    continue;
                auto it = doc.ctx.find(g);
                if (it != doc.ctx.end() && it->second.func) continue;  // inside a function: maybe on purpose
                lint(g->name_start, g->name_end, LuaLspSeverity::Warning, "global-in-module",
                     "setting global '" + g->text + "' in a module (did you mean `local " + g->text + "`?)");
            }
        }
    }

    // -- statements and expressions --
    Ctx root;
    WalkBlock(pr.chunk, root, [&](Node *n, const Ctx &) {
        switch (n->kind) {
            case Kind::Local:
            case Kind::Assign: {
                const size_t nvars = n->kind == Kind::Local ? n->vars.size() : n->targets.size();
                const size_t nexp = n->list.size();
                const bool lone_nil = nexp == 1 && n->list[0]->kind == Kind::Nil;
                if (nexp > 0 && nexp != nvars && !lone_nil && !(nexp < nvars && MultiValued(n->list.back()))) {
                    if (nexp > nvars)
                        lint(n->list[nvars]->start, n->list.back()->end, LuaLspSeverity::Warning,
                             "unbalanced-assignment",
                             "value assignment: " + std::to_string(nvars) + " variable" + (nvars == 1 ? "" : "s") +
                                 " but " + std::to_string(nexp) + " values; the extra values are discarded");
                    else
                        lint(n->start, n->list.back()->end, LuaLspSeverity::Warning, "unbalanced-assignment",
                             "value assignment: " + std::to_string(nvars) + " variables but " +
                                 std::to_string(nexp) + " value" + (nexp == 1 ? "" : "s") +
                                 "; the rest are set to nil");
                }
                if (n->kind == Kind::Assign && nvars == nexp) {
                    for (size_t i = 0; i < nvars; i++) {
                        const Node *t = n->targets[i], *v = n->list[i];
                        if (t->kind == Kind::Name && v->kind == Kind::Name && t->text == v->text &&
                            t->var == v->var)
                            lint(t->start, v->end, LuaLspSeverity::Warning, "self-assignment",
                                 "'" + t->text + "' is assigned to itself");
                    }
                }
                break;
            }
            case Kind::Table: {
                std::map<std::string, const TableField *> seen;
                long long next_index = 1;
                for (const TableField &tf : n->fields) {
                    std::string key;
                    int ka = tf.start, kb = tf.start;
                    if (tf.kind == TableField::Named) {
                        key = "s:" + tf.name;
                        ka = tf.name_start;
                        kb = tf.name_end;
                    } else if (tf.kind == TableField::Keyed && tf.key && tf.key->kind == Kind::String) {
                        key = "s:" + tf.key->value;
                        ka = tf.key->start;
                        kb = tf.key->end;
                    } else if (tf.kind == TableField::Keyed && tf.key && tf.key->kind == Kind::Number) {
                        const std::string &t = tf.key->text;
                        if (!t.empty() && t.find_first_not_of("0123456789") == std::string::npos && t.size() < 15)
                            key = "n:" + std::to_string(std::stoll(t));
                        ka = tf.key->start;
                        kb = tf.key->end;
                    } else if (tf.kind == TableField::Positional) {
                        key = "n:" + std::to_string(next_index++);
                        continue;  // positional keys only collide with explicit ones; track but never report
                    }
                    if (key.empty()) continue;
                    auto it = seen.find(key);
                    if (it != seen.end()) {
                        lint(ka, kb, LuaLspSeverity::Warning, "duplicate-key",
                             "duplicate key " + doc.Slice(ka, kb) + " in table (first on line " +
                                 std::to_string(doc.LineOf(it->second->start) + 1) + ")");
                    } else {
                        seen[key] = &tf;
                    }
                }
                break;
            }
            case Kind::Call: {
                // string.format("...", args) and ("..."):format(args)
                const Node *fmt = nullptr;
                size_t first_arg = 0;
                if (!n->method && n->a && n->a->kind == Kind::Field && n->a->text == "format" && n->a->a &&
                    n->a->a->kind == Kind::Name && !n->a->a->var && n->a->a->text == "string" && !n->list.empty()) {
                    fmt = n->list[0];
                    first_arg = 1;
                } else if (n->method && n->text == "format" && n->a &&
                           (n->a->kind == Kind::String ||
                            (n->a->kind == Kind::Paren && n->a->a && n->a->a->kind == Kind::String))) {
                    fmt = n->a->kind == Kind::String ? n->a : n->a->a;
                    first_arg = 0;
                }
                if (fmt && fmt->kind == Kind::String) {
                    bool ok = true;
                    const int want = FormatSpecifiers(fmt->value, &ok);
                    const int have = static_cast<int>(n->list.size() - first_arg);
                    const bool open_ended = have > 0 && MultiValued(n->list.back());
                    if (ok && (have > want || (have < want && !open_ended))) {
                        lint(n->start, n->end, LuaLspSeverity::Warning, "format-arguments",
                             "format string takes " + std::to_string(want) + " argument" + (want == 1 ? "" : "s") +
                                 " but is given " + std::to_string(have));
                    }
                }
                break;
            }
            default:
                break;
        }
    });

    ForEachBlock(pr.chunk, [&](const Block &b, Node *owner, const char *what) {
        // Unreachable: anything after break/goto/return in the same block.
        for (size_t i = 0; i + 1 < b.stats.size(); i++) {
            const Node *s = b.stats[i];
            if (s->kind != Kind::Break && s->kind != Kind::Goto && s->kind != Kind::Return) continue;
            size_t j = i + 1;
            while (j < b.stats.size() && (b.stats[j]->kind == Kind::Empty || b.stats[j]->kind == Kind::Label)) {
                if (b.stats[j]->kind == Kind::Label) {
                    j = b.stats.size();  // a label makes what follows reachable
                    break;
                }
                j++;
            }
            if (j < b.stats.size()) {
                LuaLspDiagnostic *d = lint(b.stats[j]->start, b.stats.back()->end, LuaLspSeverity::Warning,
                                           "unreachable-code", "unreachable code");
                if (d) d->unnecessary = true;
            }
            break;
        }
        // Empty branches (a comment inside counts as content: `-- nothing to do`).
        if (owner && b.stats.empty() && (std::strcmp(what, "if") == 0 || std::strcmp(what, "elseif") == 0 ||
                                         std::strcmp(what, "else") == 0 || std::strcmp(what, "do") == 0)) {
            int a = owner->start, e = owner->end;
            if (owner->kind == Kind::If) {
                for (size_t i = 0; i < owner->clauses.size(); i++) {
                    if (&owner->clauses[i].body != &b) continue;
                    a = owner->clauses[i].kw;
                    e = i + 1 < owner->clauses.size() ? owner->clauses[i + 1].kw : owner->end;
                }
            }
            if (!BlockHasComment(doc, a, e))
                lint(a, a + static_cast<int>(std::strlen(what)), LuaLspSeverity::Information, "empty-block",
                     std::string("empty ") + what + " block");
        }
    });

    // Unused labels: no goto names them in the same function.
    {
        std::map<const FuncBody *, std::set<std::string>> gotos;
        std::vector<std::pair<const Node *, const FuncBody *>> labels;
        WalkBlock(pr.chunk, root, [&](Node *n, const Ctx &c) {
            if (n->kind == Kind::Goto) gotos[c.func].insert(n->text);
            if (n->kind == Kind::Label) labels.push_back({n, c.func});
        });
        if (!broken) {
            for (const auto &l : labels) {
                if (gotos[l.second].count(l.first->text)) continue;
                LuaLspDiagnostic *d = lint(l.first->name_start, l.first->name_end, LuaLspSeverity::Hint,
                                           "unused-label", "unused label '" + l.first->text + "'");
                if (d) d->unnecessary = true;
            }
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const LuaLspDiagnostic &a, const LuaLspDiagnostic &b) {
        return a.range.line != b.range.line ? a.range.line < b.range.line : a.range.col < b.range.col;
    });
    return out;
}

std::string LuaLspFormatDiagnostic(const std::string &path, const LuaLspDiagnostic &d) {
    const char *sev = d.severity == LuaLspSeverity::Error         ? "error"
                      : d.severity == LuaLspSeverity::Warning     ? "warning"
                      : d.severity == LuaLspSeverity::Information ? "info"
                                                                  : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool LuaLspDiagnosticFails(const LuaLspDiagnostic &d) {
    return d.severity == LuaLspSeverity::Error || d.severity == LuaLspSeverity::Warning;
}

// ---- hover / completion / signatures ---------------------------------------------------------------------------

namespace {

std::string ApiSignatureLabel(const LuaLspApi &a) {
    std::string sig = a.signature;
    const size_t arrow = sig.find(" -> ");
    if (arrow != std::string::npos) sig = sig.substr(0, arrow);
    std::string name = a.name;
    return name + sig;
}

std::string ApiHover(const LuaLspApi &a) {
    std::string out = "```lua\n";
    if (std::strcmp(a.kind, "function") == 0) {
        out += std::string("function ") + a.name + (a.signature[0] ? a.signature : "(...)");
    } else if (std::strcmp(a.kind, "table") == 0) {
        out += std::string(a.name) + ": table";
    } else {
        out += std::string(a.name) + ": " + a.kind;
    }
    out += "\n```";
    if (std::strcmp(a.source, "old") == 0) {
        out += "\n\nNot in Lua 5.4: ";
        out += a.doc;
        return out;
    }
    if (a.doc[0]) out += std::string("\n\n") + a.doc;
    if (std::strcmp(a.source, "std") == 0) out += "\n\n*Lua 5.4 standard library*";
    return out;
}

// The `--` comments directly above a line, as markdown.
std::string DocCommentAbove(const LuaLspDocument &doc, int line) {
    std::vector<std::string> got;
    int want = line - 1;
    const auto &cs = doc.parse->comments;
    for (size_t k = cs.size(); k-- > 0;) {
        int cl = 0, cc = 0, el = 0, ec = 0;
        doc.Pos(cs[k].start, &cl, &cc);
        doc.Pos(cs[k].end, &el, &ec);
        if (el > want) continue;
        if (el < want) break;
        // Only a comment alone on its line.
        const std::string &l = doc.lines[static_cast<size_t>(cl)];
        if (l.find_first_not_of(" \t") != static_cast<size_t>(cc)) break;
        std::string body = doc.Slice(cs[k].start, cs[k].end);
        if (cs[k].block) {
            const size_t open = body.find('[', 3);
            const size_t close = body.rfind(']', body.size() >= 2 ? body.size() - 2 : 0);
            if (open != std::string::npos && close != std::string::npos && close > open) {
                size_t s = body.find('[', open + 1);
                body = body.substr(s + 1, close - s - 1);
                while (!body.empty() && body.back() == '=') body.pop_back();
            }
            got.push_back(body);
        } else {
            size_t i = 0;
            while (i < body.size() && body[i] == '-') i++;
            if (i < body.size() && body[i] == ' ') i++;
            got.push_back(body.substr(i));
        }
        want = cl - 1;
    }
    std::reverse(got.begin(), got.end());
    std::string out;
    for (const std::string &s : got) {
        if (s.find("luacheck:") != std::string::npos) continue;
        if (!out.empty()) out += "\n";
        out += s;
    }
    return out;
}

const FuncBody *FunctionOfVar(const Var *v) {
    if (v->kind == Var::LocalFunction && v->decl) return v->decl->func;
    const Node *init = InitializerOf(v);
    return init && init->kind == Kind::Function ? init->func : nullptr;
}

std::string VarHover(const LuaLspDocument &doc, const Var *v) {
    std::string head;
    const FuncBody *fn = FunctionOfVar(v);
    std::string attrib = v->attrib == Var::Const ? " <const>" : v->attrib == Var::Close ? " <close>" : "";
    switch (v->kind) {
        case Var::Param:
            head = "(parameter) " + v->name;
            break;
        case Var::Self:
            head = "(self) self";
            break;
        case Var::ForVar:
            head = "(loop variable) " + v->name;
            break;
        default:
            if (fn) {
                head = "local function " + v->name + ParamList(fn);
            } else {
                head = "local " + v->name + attrib;
                const Node *init = InitializerOf(v);
                if (init) {
                    const std::string s = OneLine(doc.Slice(init->start, init->end), 60);
                    head += " = " + s;
                }
            }
    }
    std::string out = "```lua\n" + head + "\n```";
    const int dl = doc.LineOf(v->start);
    std::string comment = DocCommentAbove(doc, v->decl && v->kind != Var::Param ? doc.LineOf(v->decl->start) : dl);
    if (!comment.empty()) out += "\n\n" + comment;
    std::string uses = std::to_string(v->reads.size()) + " read" + (v->reads.size() == 1 ? "" : "s");
    if (!v->writes.empty())
        uses += ", " + std::to_string(v->writes.size()) + " assignment" + (v->writes.size() == 1 ? "" : "s");
    out += "\n\n*declared on line " + std::to_string(dl + 1) + "; " + uses + "*";
    return out;
}

const LuaMemberDef *FindMember(const LuaLspDocument &doc, const std::string &key) {
    for (const LuaMemberDef &m : doc.members) {
        if (m.key == key) return &m;
    }
    return nullptr;
}

// A member defined in this file whose last component is `name` (for a method call on an unknown object).
const LuaMemberDef *FindMemberByName(const LuaLspDocument &doc, const std::string &name, bool methods_only) {
    const LuaMemberDef *best = nullptr;
    int count = 0;
    for (const LuaMemberDef &m : doc.members) {
        const size_t dot = m.key.rfind('.');
        if (dot == std::string::npos || m.key.compare(dot + 1, std::string::npos, name) != 0) continue;
        if (methods_only && !m.func) continue;
        if (!best) best = &m;
        count++;
    }
    return count >= 1 ? best : nullptr;
}

std::string MemberHover(const LuaLspDocument &doc, const LuaMemberDef &m, const std::string &display) {
    std::string out = "```lua\n";
    if (m.func)
        out += "function " + display + ParamList(m.func);
    else if (m.value)
        out += display + " = " + OneLine(doc.Slice(m.value->start, m.value->end), 60);
    else
        out += display;
    out += "\n```";
    const std::string c = DocCommentAbove(doc, m.value && m.value->kind == Kind::FunctionStat
                                                    ? doc.LineOf(m.value->start)
                                                    : m.line);
    if (!c.empty()) out += "\n\n" + c;
    out += "\n\n*defined on line " + std::to_string(m.line + 1) + "*";
    return out;
}

// The library entry for a method call `obj:name()` on a value of unknown type.
const LuaLspApi *MethodApi(const std::string &name, bool string_receiver) {
    if (const LuaLspApi *a = FindApi("string." + name)) return a;
    if (!string_receiver) {
        if (const LuaLspApi *a = FindApi("file:" + name)) return a;
    }
    return nullptr;
}

std::string GlobalDefHover(const LuaLspDocument &doc, const std::string &name, const Node *def) {
    std::string out = "```lua\n";
    const Node *stat = nullptr;
    auto it = doc.ctx.find(def);
    if (it != doc.ctx.end()) stat = it->second.parent;
    const FuncBody *fn = nullptr;
    if (stat && stat->kind == Kind::FunctionStat) fn = stat->func;
    if (stat && stat->kind == Kind::Assign) {
        for (size_t i = 0; i < stat->targets.size() && i < stat->list.size(); i++) {
            if (stat->targets[i] == def && stat->list[i]->kind == Kind::Function) fn = stat->list[i]->func;
        }
    }
    out += fn ? "function " + name + ParamList(fn) : "(global) " + name;
    out += "\n```";
    const std::string c = DocCommentAbove(doc, stat ? doc.LineOf(stat->start) : doc.LineOf(def->start));
    if (!c.empty()) out += "\n\n" + c;
    out += "\n\n*global, first set on line " + std::to_string(doc.LineOf(def->start) + 1) + "*";
    return out;
}

}  // namespace

LuaLspHoverInfo LuaLspHover(const LuaLspDocument &doc, int line, int col) {
    LuaLspHoverInfo info;
    const int off = doc.Off(line, col);
    const LuaHit *h = doc.HitAt(off);
    if (!h) return info;
    info.range = doc.Range(h->start, h->end);
    if (!h->node && h->var) {
        info.found = true;
        info.text = VarHover(doc, h->var);
        return info;
    }
    const Node *n = h->node;
    if (n->kind == Kind::Name) {
        if (n->var) {
            info.found = true;
            info.text = VarHover(doc, n->var);
            return info;
        }
        auto def = doc.global_defs.find(n->text);
        if (def != doc.global_defs.end()) {
            info.found = true;
            info.text = GlobalDefHover(doc, n->text, def->second);
            return info;
        }
        if (const LuaLspApi *a = FindApi(n->text)) {
            info.found = true;
            info.text = ApiHover(*a);
            return info;
        }
        info.found = true;
        info.text = "```lua\n(global) " + n->text + "\n```" +
                    (HostGlobals().count(n->text) ? "\n\n*provided by the host program*" : "");
        return info;
    }
    if (n->kind == Kind::Field) {
        std::string key;
        if (ChainKey(doc, n, &key)) {
            if (const LuaMemberDef *m = FindMember(doc, key)) {
                info.found = true;
                info.text = MemberHover(doc, *m, ChainText(n));
                return info;
            }
            if (key[0] != '#') {
                if (const LuaLspApi *a = FindApi(key)) {
                    info.found = true;
                    info.text = ApiHover(*a);
                    return info;
                }
            }
        }
        return info;
    }
    if (n->kind == Kind::Call && n->method) {
        std::string key;
        if (ChainKey(doc, n->a, &key)) {
            if (const LuaMemberDef *m = FindMember(doc, key + "." + n->text)) {
                info.found = true;
                info.text = MemberHover(doc, *m, ChainText(n->a) + ":" + n->text);
                return info;
            }
        }
        const bool str = n->a && (n->a->kind == Kind::String ||
                                  (n->a->kind == Kind::Paren && n->a->a && n->a->a->kind == Kind::String));
        if (const LuaMemberDef *m = str ? nullptr : FindMemberByName(doc, n->text, true)) {
            info.found = true;
            info.text = MemberHover(doc, *m, "?:" + n->text);
            return info;
        }
        if (const LuaLspApi *a = MethodApi(n->text, str)) {
            info.found = true;
            info.text = ApiHover(*a);
            if (!str) info.text += "\n\n*(if the receiver is a " + std::string(a->name[0] == 'f' ? "file" : "string") + ")*";
            return info;
        }
        return info;
    }
    if (n->kind == Kind::Label || n->kind == Kind::Goto) {
        info.found = true;
        info.text = "```lua\n::" + n->text + "::\n```";
        return info;
    }
    return info;
}

namespace {

LuaLspKind KindForApi(const LuaLspApi &a) {
    if (std::strcmp(a.kind, "function") == 0) return std::strchr(a.name, ':') ? LuaLspKind::Method : LuaLspKind::Function;
    if (std::strcmp(a.kind, "table") == 0) return LuaLspKind::Module;
    if (std::strcmp(a.kind, "number") == 0 || std::strcmp(a.kind, "string") == 0) return LuaLspKind::Constant;
    return LuaLspKind::Value;
}

bool InCommentOrString(const LuaLspDocument &doc, int off) {
    for (const luaparse::Comment &c : doc.parse->comments) {
        if (off > c.start && off <= c.end) return true;
    }
    // A string literal: scan the line up to the cursor for an open quote.
    int l = 0, c = 0;
    doc.Pos(off, &l, &c);
    const std::string &s = doc.lines[static_cast<size_t>(l)];
    char q = 0;
    for (int i = 0; i < c && i < static_cast<int>(s.size()); i++) {
        const char ch = s[static_cast<size_t>(i)];
        if (q) {
            if (ch == '\\') i++;
            else if (ch == q) q = 0;
        } else if (ch == '"' || ch == '\'') {
            q = ch;
        } else if (ch == '-' && i + 1 < static_cast<int>(s.size()) && s[static_cast<size_t>(i) + 1] == '-') {
            return true;
        }
    }
    return q != 0;
}

}  // namespace

std::vector<LuaLspCompletionItem> LuaLspCompletions(const LuaLspDocument &doc, int line, int col) {
    std::vector<LuaLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return out;
    const std::string &cur = doc.lines[static_cast<size_t>(line)];
    col = std::max(0, std::min(col, static_cast<int>(cur.size())));
    const int off = doc.Off(line, col);
    if (InCommentOrString(doc, off)) return out;
    int start = col;
    while (start > 0 && IsWordChar(cur[static_cast<size_t>(start - 1)])) start--;
    const std::string prefix = cur.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    std::set<std::string> seen;
    auto push = [&](const std::string &label, LuaLspKind kind, const std::string &detail, const std::string &docs) {
        if (label.compare(0, prefix.size(), prefix) != 0) return;
        if (!seen.insert(label).second) return;
        LuaLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = docs;
        it.replace_start = start;
        it.replace_end = col;
        out.push_back(it);
    };

    // Member access: `a.b.` / `a:`
    if (start > 0 && (cur[static_cast<size_t>(start - 1)] == '.' || cur[static_cast<size_t>(start - 1)] == ':') &&
        !(start > 1 && cur[static_cast<size_t>(start - 2)] == '.')) {
        const bool colon = cur[static_cast<size_t>(start - 1)] == ':';
        int b = start - 1;
        std::vector<std::string> parts;
        while (b > 0) {
            int e = b;
            int s = e;
            while (s > 0 && IsWordChar(cur[static_cast<size_t>(s - 1)])) s--;
            if (s == e || !IsIdentStart(cur[static_cast<size_t>(s)])) break;
            parts.push_back(cur.substr(static_cast<size_t>(s), static_cast<size_t>(e - s)));
            b = s;
            if (b > 0 && cur[static_cast<size_t>(b - 1)] == '.') {
                b--;
                continue;
            }
            break;
        }
        std::reverse(parts.begin(), parts.end());
        std::string disp;
        for (const std::string &p : parts) disp += (disp.empty() ? "" : ".") + p;
        // The root: a local in scope, else a global.
        std::string key;
        if (!parts.empty()) {
            const Var *best = nullptr;
            for (const Var *v : doc.parse->vars) {
                if (v->name == parts[0] && v->scope_start <= off && off <= doc.ScopeEnd(v) &&
                    (!best || v->start > best->start))
                    best = v;
            }
            key = best ? doc.VarKey(best) : parts[0];
            for (size_t i = 1; i < parts.size(); i++) key += "." + parts[i];
        }
        // Members this file assigns.
        if (!key.empty()) {
            for (const LuaMemberDef &m : doc.members) {
                if (m.key.size() <= key.size() + 1 || m.key.compare(0, key.size(), key) != 0 || m.key[key.size()] != '.')
                    continue;
                const std::string rest = m.key.substr(key.size() + 1);
                if (rest.find('.') != std::string::npos) continue;
                if (colon && !m.func) continue;
                push(rest, m.func ? (m.method ? LuaLspKind::Method : LuaLspKind::Function) : LuaLspKind::Field,
                     m.func ? disp + (m.method ? ":" : ".") + rest + ParamList(m.func) : std::string(), std::string());
            }
        }
        // Library members.
        size_t count = 0;
        const LuaLspApi *table = LuaLspApiTable(&count);
        auto add_lib = [&](const std::string &lib, const char *sep) {
            const std::string pre = lib + sep;
            for (size_t i = 0; i < count; i++) {
                const LuaLspApi &a = table[i];
                if (std::strcmp(a.source, "old") == 0) continue;
                if (std::strncmp(a.name, pre.c_str(), pre.size()) != 0) continue;
                const std::string rest = a.name + pre.size();
                if (rest.find('.') != std::string::npos) continue;
                push(rest, KindForApi(a), ApiSignatureLabel(a), a.doc);
            }
        };
        if (!key.empty() && key[0] != '#') add_lib(key, ".");
        if (colon) {
            // Methods of a string (any value could be one) and, unless a literal, a file.
            add_lib("string", ".");
            add_lib("file", ":");
        }
        return out;
    }
    if (start > 0 && cur[static_cast<size_t>(start - 1)] == '.') return out;  // `..` concatenation: nothing special

    // Locals in scope, innermost first.
    std::vector<const Var *> vis;
    for (const Var *v : doc.parse->vars) {
        if (v->scope_start <= off && off <= doc.ScopeEnd(v)) vis.push_back(v);
    }
    std::sort(vis.begin(), vis.end(), [](const Var *a, const Var *b) { return a->start > b->start; });
    for (const Var *v : vis) {
        const FuncBody *fn = FunctionOfVar(v);
        push(v->name, fn ? LuaLspKind::Function : LuaLspKind::Variable,
             fn ? "local function " + v->name + ParamList(fn) : std::string("local ") + VarWhat(v), std::string());
    }
    for (const auto &kv : doc.global_defs) push(kv.first, LuaLspKind::Variable, "global", std::string());
    for (const char *kw : kKeywords) push(kw, LuaLspKind::Keyword, std::string(), std::string());
    size_t count = 0;
    const LuaLspApi *table = LuaLspApiTable(&count);
    for (size_t i = 0; i < count; i++) {
        const LuaLspApi &a = table[i];
        if (std::strcmp(a.source, "old") == 0 || std::strchr(a.name, '.') || std::strchr(a.name, ':')) continue;
        push(a.name, KindForApi(a), ApiSignatureLabel(a), a.doc);
    }
    // Words elsewhere in the file (fields, names in strings) as a last resort.
    if (!prefix.empty()) {
        for (size_t li = 0; li < doc.lines.size(); li++) {
            const std::string &l = doc.lines[li];
            for (size_t i = 0; i < l.size();) {
                if (!IsIdentStart(l[i]) || (i > 0 && IsWordChar(l[i - 1]))) {
                    i++;
                    continue;
                }
                size_t j = i;
                while (j < l.size() && IsWordChar(l[j])) j++;
                const bool at_cursor = li == static_cast<size_t>(line) && static_cast<int>(i) == start;
                const std::string w = l.substr(i, j - i);
                if (!at_cursor && w != prefix && !luaparse::IsReserved(w)) push(w, LuaLspKind::Text, std::string(), std::string());
                i = j;
            }
        }
    }
    return out;
}

namespace {

// Parameter names out of a signature such as "(s [, i [, j]])" or "(f, ...)".
std::vector<std::string> SigParams(const std::string &sig) {
    std::vector<std::string> out;
    const size_t close = sig.rfind(')');
    for (size_t i = 1; i < sig.size() && i < close; i++) {
        if (sig.compare(i, 3, "...") == 0) {
            out.push_back("...");
            i += 2;
            continue;
        }
        if (IsIdentStart(sig[i])) {
            size_t j = i;
            while (j < sig.size() && IsWordChar(sig[j])) j++;
            out.push_back(sig.substr(i, j - i));
            i = j - 1;
            continue;
        }
        if (sig[i] == '{' || sig[i] == '"' || sig[i] == '\'') {
            // literal option lists such as ("collect") stop being parameter names
            const char closer = sig[i] == '{' ? '}' : sig[i];
            const size_t j = sig.find(closer, i + 1);
            if (j == std::string::npos) break;
            out.push_back(sig.substr(i, j - i + 1));
            i = j;
        }
    }
    return out;
}

}  // namespace

LuaLspSignatureHelp LuaLspSignatureAt(const LuaLspDocument &doc, int line, int col) {
    LuaLspSignatureHelp sh;
    const int off = doc.Off(line, col);
    if (InCommentOrString(doc, off)) return sh;
    // Scan forward from a few lines back, tracking open brackets, to find the call the cursor is in.
    const int from = doc.line_starts[static_cast<size_t>(std::max(0, line - 40))];
    struct Open {
        char c;
        int pos;
        int commas;
    };
    std::vector<Open> stack;
    const std::string &t = doc.text;
    for (int i = from; i < off; i++) {
        const char c = t[static_cast<size_t>(i)];
        if (c == '"' || c == '\'') {
            int j = i + 1;
            while (j < off && t[static_cast<size_t>(j)] != c && t[static_cast<size_t>(j)] != '\n') {
                if (t[static_cast<size_t>(j)] == '\\') j++;
                j++;
            }
            i = j;
            continue;
        }
        if (c == '-' && i + 1 < off && t[static_cast<size_t>(i) + 1] == '-') {
            while (i < off && t[static_cast<size_t>(i)] != '\n') i++;
            continue;
        }
        if (c == '(' || c == '{' || c == '[') stack.push_back({c, i, 0});
        else if (c == ')' || c == '}' || c == ']') {
            const char want = c == ')' ? '(' : c == '}' ? '{' : '[';
            if (!stack.empty() && stack.back().c == want) stack.pop_back();
        } else if (c == ',' && !stack.empty()) {
            stack.back().commas++;
        }
    }
    if (stack.empty() || stack.back().c != '(') return sh;
    const Open &o = stack.back();
    // The callee: a chain like a.b:c right before '('.
    int e = o.pos;
    while (e > 0 && (t[static_cast<size_t>(e - 1)] == ' ' || t[static_cast<size_t>(e - 1)] == '\t')) e--;
    int s = e;
    while (s > 0 && (IsWordChar(t[static_cast<size_t>(s - 1)]) || t[static_cast<size_t>(s - 1)] == '.' ||
                     t[static_cast<size_t>(s - 1)] == ':'))
        s--;
    std::string callee = t.substr(static_cast<size_t>(s), static_cast<size_t>(e - s));
    if (callee.empty() || !IsIdentStart(callee[0]) || luaparse::IsReserved(callee)) return sh;
    sh.active_param = o.commas;
    const size_t colon = callee.find(':');
    auto from_func = [&](const std::string &label_name, const FuncBody *f, const std::string &docs) {
        sh.found = true;
        sh.label = label_name + ParamList(f);
        for (const Var *p : f->params) {
            if (p->kind != Var::Self) sh.params.push_back(p->name);
        }
        if (f->vararg) sh.params.push_back("...");
        sh.documentation = docs;
    };
    auto from_api = [&](const LuaLspApi &a) {
        sh.found = true;
        sh.label = ApiSignatureLabel(a);
        sh.params = SigParams(a.signature);
        sh.documentation = a.doc;
    };
    // Root resolution for a dotted chain.
    std::string root = callee.substr(0, callee.find_first_of(".:"));
    const Var *rv = nullptr;
    for (const Var *v : doc.parse->vars) {
        if (v->name == root && v->scope_start <= off && off <= doc.ScopeEnd(v) && (!rv || v->start > rv->start))
            rv = v;
    }
    std::string key = (rv ? doc.VarKey(rv) : root) + callee.substr(root.size());
    for (char &c : key) {
        if (c == ':') c = '.';
    }
    if (colon == std::string::npos && callee.find('.') == std::string::npos) {
        if (rv) {
            if (const FuncBody *f = FunctionOfVar(rv)) from_func(callee, f, DocCommentAbove(doc, doc.LineOf(rv->decl->start)));
            return sh;
        }
        auto def = doc.global_defs.find(callee);
        if (def != doc.global_defs.end()) {
            auto it = doc.ctx.find(def->second);
            const Node *stat = it == doc.ctx.end() ? nullptr : it->second.parent;
            if (stat && stat->kind == Kind::FunctionStat && stat->func) {
                from_func(callee, stat->func, DocCommentAbove(doc, doc.LineOf(stat->start)));
                return sh;
            }
            if (stat && stat->kind == Kind::Assign) {
                for (size_t i = 0; i < stat->targets.size() && i < stat->list.size(); i++) {
                    if (stat->targets[i] == def->second && stat->list[i]->kind == Kind::Function) {
                        from_func(callee, stat->list[i]->func, std::string());
                        return sh;
                    }
                }
            }
        }
        if (const LuaLspApi *a = FindApi(callee)) from_api(*a);
        return sh;
    }
    if (const LuaMemberDef *m = FindMember(doc, key)) {
        if (m->func) from_func(callee, m->func, DocCommentAbove(doc, m->line));
        return sh;
    }
    if (!rv && colon == std::string::npos) {
        if (const LuaLspApi *a = FindApi(callee)) from_api(*a);
        return sh;
    }
    if (colon != std::string::npos) {
        const std::string method = callee.substr(colon + 1);
        if (const LuaMemberDef *m = FindMemberByName(doc, method, true)) {
            from_func(callee, m->func, std::string());
            return sh;
        }
        if (const LuaLspApi *a = MethodApi(method, false)) {
            from_api(*a);
            // The receiver is the first argument of string.x: drop it.
            if (std::strncmp(a->name, "string.", 7) == 0 && !sh.params.empty()) {
                sh.params.erase(sh.params.begin());
                sh.label = callee + "(";
                for (size_t i = 0; i < sh.params.size(); i++) sh.label += (i ? ", " : "") + sh.params[i];
                sh.label += ")";
            }
        }
    }
    return sh;
}

// ---- navigation ------------------------------------------------------------------------------------------------

namespace {

struct Target {
    const Var *var = nullptr;
    std::string global;        // a global name
    const LuaMemberDef *member = nullptr;
    const Node *label = nullptr;  // a Label (from a goto or itself)
    std::string member_key;
};

const Node *FindLabelFor(const LuaLspDocument &doc, const Node *gt) {
    auto it = doc.ctx.find(gt);
    const FuncBody *f = it == doc.ctx.end() ? nullptr : it->second.func;
    const Node *best = nullptr;
    Ctx root;
    WalkBlock(doc.parse->chunk, root, [&](Node *n, const Ctx &c) {
        if (n->kind == Kind::Label && c.func == f && n->text == gt->text) {
            // Prefer the nearest label at or before... any is fine; goto may jump forward.
            if (!best) best = n;
        }
    });
    return best;
}

Target TargetAt(const LuaLspDocument &doc, int off) {
    Target t;
    const LuaHit *h = doc.HitAt(off);
    if (!h) return t;
    if (h->var) {
        t.var = h->var;
        return t;
    }
    const Node *n = h->node;
    if (!n) return t;
    switch (n->kind) {
        case Kind::Name:
            if (!n->custom_env) t.global = n->text;
            break;
        case Kind::Field:
            if (ChainKey(doc, n, &t.member_key)) t.member = FindMember(doc, t.member_key);
            break;
        case Kind::Call:
            if (n->method && ChainKey(doc, n->a, &t.member_key)) {
                t.member_key += "." + n->text;
                t.member = FindMember(doc, t.member_key);
            }
            break;
        case Kind::Goto:
            t.label = FindLabelFor(doc, n);
            break;
        case Kind::Label:
            t.label = n;
            break;
        default:
            break;
    }
    return t;
}

}  // namespace

std::vector<LuaLspLocation> LuaLspDefinition(const LuaLspDocument &doc, int line, int col) {
    std::vector<LuaLspLocation> out;
    const Target t = TargetAt(doc, doc.Off(line, col));
    if (t.var) {
        out.push_back({doc.Range(t.var->start, t.var->end)});
    } else if (t.member) {
        out.push_back({doc.Range(t.member->name_start, t.member->name_end)});
    } else if (t.label) {
        out.push_back({doc.Range(t.label->name_start, t.label->name_end)});
    } else if (!t.global.empty()) {
        auto it = doc.global_defs.find(t.global);
        if (it != doc.global_defs.end()) out.push_back({doc.Range(it->second->name_start, it->second->name_end)});
    }
    return out;
}

std::vector<LuaLspReference> LuaLspReferences(const LuaLspDocument &doc, int line, int col) {
    std::vector<LuaLspReference> out;
    const Target t = TargetAt(doc, doc.Off(line, col));
    if (t.var) {
        out.push_back({doc.Range(t.var->start, t.var->end), true});
        std::vector<const Node *> uses(t.var->reads.begin(), t.var->reads.end());
        uses.insert(uses.end(), t.var->writes.begin(), t.var->writes.end());
        std::sort(uses.begin(), uses.end(), [](const Node *a, const Node *b) { return a->start < b->start; });
        for (const Node *u : uses) out.push_back({doc.Range(u->name_start, u->name_end), false});
        return out;
    }
    if (t.label) {
        out.push_back({doc.Range(t.label->name_start, t.label->name_end), true});
        auto it = doc.ctx.find(t.label);
        const FuncBody *f = it == doc.ctx.end() ? nullptr : it->second.func;
        Ctx root;
        WalkBlock(doc.parse->chunk, root, [&](Node *n, const Ctx &c) {
            if (n->kind == Kind::Goto && c.func == f && n->text == t.label->text)
                out.push_back({doc.Range(n->name_start, n->name_end), false});
        });
        return out;
    }
    if (!t.member_key.empty()) {
        for (const LuaHit &h : doc.hits) {
            if (!h.node) continue;
            std::string k;
            if (h.node->kind == Kind::Field && ChainKey(doc, h.node, &k) && k == t.member_key)
                out.push_back({doc.Range(h.start, h.end), t.member && t.member->name_start == h.start});
            else if (h.node->kind == Kind::Call && h.node->method && ChainKey(doc, h.node->a, &k) &&
                     k + "." + h.node->text == t.member_key)
                out.push_back({doc.Range(h.start, h.end), false});
        }
        // Table-constructor definitions are not hits.
        if (t.member && std::none_of(out.begin(), out.end(), [](const LuaLspReference &r) { return r.is_definition; }))
            out.insert(out.begin(), {doc.Range(t.member->name_start, t.member->name_end), true});
        return out;
    }
    if (!t.global.empty()) {
        auto def = doc.global_defs.find(t.global);
        for (const Node *g : doc.parse->globals) {
            if (g->text != t.global || g->custom_env) continue;
            out.push_back({doc.Range(g->name_start, g->name_end), def != doc.global_defs.end() && def->second == g});
        }
    }
    return out;
}

LuaLspRenameTarget LuaLspPrepareRename(const LuaLspDocument &doc, int line, int col) {
    LuaLspRenameTarget r;
    const int off = doc.Off(line, col);
    const Target t = TargetAt(doc, off);
    const LuaHit *h = doc.HitAt(off);
    if (!h) return r;
    if ((t.var && t.var->kind != Var::Self) || t.label) {
        r.found = true;
        r.range = doc.Range(h->start, h->end);
        r.placeholder = doc.Slice(h->start, h->end);
    }
    return r;
}

std::vector<LuaLspTextEdit> LuaLspRename(const LuaLspDocument &doc, int line, int col, const std::string &new_name,
                                         std::string *error) {
    std::vector<LuaLspTextEdit> edits;
    const Target t = TargetAt(doc, doc.Off(line, col));
    if (!IsIdentifier(new_name)) {
        *error = "'" + new_name + "' is not a Lua name";
        return edits;
    }
    if (t.label) {
        for (const LuaLspReference &ref : LuaLspReferences(doc, line, col)) edits.push_back({ref.range, new_name});
        return edits;
    }
    if (!t.var || t.var->kind == Var::Self) {
        *error = "only a local variable, parameter or label can be renamed";
        return edits;
    }
    const Var *v = t.var;
    // Every use must still mean v under the new name: no other variable called new_name may be
    // visible at a use, and no global read of new_name may fall inside v's scope (it would be captured).
    std::vector<const Node *> uses(v->reads.begin(), v->reads.end());
    uses.insert(uses.end(), v->writes.begin(), v->writes.end());
    for (const Var *o : doc.parse->vars) {
        if (o == v || o->name != new_name) continue;
        for (const Node *u : uses) {
            if (o->scope_start <= u->start && u->start <= doc.ScopeEnd(o) && o->start > v->start) {
                *error = "'" + new_name + "' declared on line " + std::to_string(doc.LineOf(o->start) + 1) +
                         " would capture a use of '" + v->name + "'";
                return edits;
            }
        }
        for (const Node *r : o->reads) {
            if (v->scope_start <= r->start && r->start <= doc.ScopeEnd(v) && v->start > o->start) {
                *error = "renaming would hide '" + new_name + "' used on line " + std::to_string(doc.LineOf(r->start) + 1);
                return edits;
            }
        }
    }
    for (const Node *g : doc.parse->globals) {
        if (g->text == new_name && v->scope_start <= g->start && g->start <= doc.ScopeEnd(v)) {
            *error = "global '" + new_name + "' used on line " + std::to_string(doc.LineOf(g->start) + 1) +
                     " would become this local";
            return edits;
        }
    }
    edits.push_back({doc.Range(v->start, v->end), new_name});
    for (const Node *u : uses) edits.push_back({doc.Range(u->name_start, u->name_end), new_name});
    return edits;
}

// ---- outline / folding -----------------------------------------------------------------------------------------

std::vector<LuaLspSymbol> LuaLspSymbols(const LuaLspDocument &doc) {
    std::vector<LuaLspSymbol> out;
    std::function<void(const Block &, int, bool)> block;
    auto add = [&](const std::string &name, const std::string &detail, LuaLspSymbolKind kind, int a, int b, int sa,
                   int sb, int parent) {
        LuaLspSymbol s;
        s.name = name;
        s.detail = detail;
        s.kind = kind;
        s.range = doc.Range(a, b);
        s.sel_range = doc.Range(sa, sb);
        s.parent = parent;
        out.push_back(s);
        return static_cast<int>(out.size()) - 1;
    };
    std::function<void(const Node *, int)> table_fields = [&](const Node *tb, int parent) {
        if (!tb || tb->kind != Kind::Table) return;
        for (const TableField &tf : tb->fields) {
            if (tf.kind != TableField::Named || !tf.value) continue;
            const bool fn = tf.value->kind == Kind::Function;
            const int idx = add(tf.name, fn ? ParamList(tf.value->func) : std::string(),
                                fn ? LuaLspSymbolKind::Function
                                   : tf.value->kind == Kind::Table ? LuaLspSymbolKind::Object : LuaLspSymbolKind::Field,
                                tf.start, tf.value->end, tf.name_start, tf.name_end, parent);
            if (fn) block(tf.value->func->body, idx, false);
            else table_fields(tf.value, idx);
        }
    };
    block = [&](const Block &b, int parent, bool top) {
        for (const Node *s : b.stats) {
            switch (s->kind) {
                case Kind::FunctionStat: {
                    if (!s->func || s->targets.empty()) break;
                    std::string name = ChainText(s->targets.back());
                    if (s->method) {
                        const size_t dot = name.rfind('.');
                        if (dot != std::string::npos) name[dot] = ':';
                    }
                    const Node *last = s->targets.back();
                    const int ns = last->kind == Kind::Field ? last->name_start : last->start;
                    const int ne = last->kind == Kind::Field ? last->name_end : last->end;
                    const int idx = add(name, ParamList(s->func),
                                        s->method ? LuaLspSymbolKind::Method : LuaLspSymbolKind::Function, s->start,
                                        s->end, ns, ne, parent);
                    block(s->func->body, idx, false);
                    break;
                }
                case Kind::LocalFunction: {
                    if (!s->func || s->vars.empty()) break;
                    const int idx = add(s->vars[0]->name, ParamList(s->func), LuaLspSymbolKind::Function, s->start,
                                        s->end, s->vars[0]->start, s->vars[0]->end, parent);
                    block(s->func->body, idx, false);
                    break;
                }
                case Kind::Local:
                    for (size_t i = 0; i < s->vars.size(); i++) {
                        const Var *v = s->vars[i];
                        const Node *init = i < s->list.size() ? s->list[i] : nullptr;
                        const bool fn = init && init->kind == Kind::Function;
                        if (!top && !fn && !(init && init->kind == Kind::Table)) continue;
                        const int idx = add(v->name, fn ? ParamList(init->func) : std::string(),
                                            fn ? LuaLspSymbolKind::Function
                                               : v->attrib == Var::Const ? LuaLspSymbolKind::Constant
                                               : init && init->kind == Kind::Table ? LuaLspSymbolKind::Object
                                                                                    : LuaLspSymbolKind::Variable,
                                            s->start, init ? init->end : v->end, v->start, v->end, parent);
                        if (fn) block(init->func->body, idx, false);
                        else table_fields(init, idx);
                    }
                    break;
                case Kind::Assign:
                    for (size_t i = 0; i < s->targets.size() && i < s->list.size(); i++) {
                        const Node *tg = s->targets[i];
                        const Node *init = s->list[i];
                        const bool fn = init->kind == Kind::Function;
                        if (!fn && !(top && (init->kind == Kind::Table || tg->kind == Kind::Name))) continue;
                        const std::string name = ChainText(tg);
                        if (name.empty()) continue;
                        const int ns = tg->kind == Kind::Field ? tg->name_start : tg->start;
                        const int ne = tg->kind == Kind::Field ? tg->name_end : tg->end;
                        const int idx = add(name, fn ? ParamList(init->func) : std::string(),
                                            fn ? LuaLspSymbolKind::Function
                                               : init->kind == Kind::Table ? LuaLspSymbolKind::Object
                                                                           : LuaLspSymbolKind::Variable,
                                            s->start, init->end, ns, ne, parent);
                        if (fn) block(init->func->body, idx, false);
                        else table_fields(init, idx);
                    }
                    break;
                case Kind::Do:
                    block(s->block, parent, top);
                    break;
                default:
                    break;
            }
        }
    };
    block(doc.parse->chunk, -1, true);
    return out;
}

std::vector<LuaLspFold> LuaLspFolds(const LuaLspDocument &doc) {
    std::vector<LuaLspFold> out;
    std::set<std::pair<int, int>> seen;
    auto fold = [&](int a, int b, const char *kind) {
        const int la = doc.LineOf(a), lb = doc.LineOf(b);
        // Keep the closing line (`end`, `}`) visible.
        if (lb - 1 <= la) return;
        if (!seen.insert({la, lb - 1}).second) return;
        out.push_back({la, lb - 1, kind});
    };
    Ctx root;
    WalkBlock(doc.parse->chunk, root, [&](Node *n, const Ctx &) {
        switch (n->kind) {
            case Kind::Function:
            case Kind::FunctionStat:
            case Kind::LocalFunction:
                if (n->func && n->func->end_kw >= 0) fold(n->start, n->func->end_kw, "");
                break;
            case Kind::Do:
            case Kind::While:
            case Kind::NumFor:
            case Kind::GenFor:
            case Kind::Repeat:
                fold(n->start, n->end - 1, "");
                break;
            case Kind::If:
                for (size_t i = 0; i < n->clauses.size(); i++) {
                    const int e = i + 1 < n->clauses.size() ? n->clauses[i + 1].kw : n->end - 1;
                    fold(n->clauses[i].kw, e, "");
                }
                break;
            case Kind::Table:
                if (n->open >= 0 && n->close >= 0) fold(n->open, n->close, "");
                break;
            case Kind::Call:
                if (n->args == luaparse::ArgStyle::Parens && n->open >= 0 && n->close >= 0) fold(n->open, n->close, "");
                break;
            case Kind::String:
                if (n->text.size() > 1 && n->text[0] == '[') fold(n->start, n->end, "");
                break;
            default:
                break;
        }
    });
    const auto &cs = doc.parse->comments;
    for (size_t k = 0; k < cs.size();) {
        if (cs[k].block) {
            const int la = doc.LineOf(cs[k].start), lb = doc.LineOf(cs[k].end);
            if (lb > la && seen.insert({la, lb}).second) out.push_back({la, lb, "comment"});
            k++;
            continue;
        }
        size_t j = k;
        const int la = doc.LineOf(cs[k].start);
        int prev = la;
        while (j + 1 < cs.size() && !cs[j + 1].block) {
            const int l2 = doc.LineOf(cs[j + 1].start);
            if (l2 != prev + 1) break;
            prev = l2;
            j++;
        }
        if (prev > la && seen.insert({la, prev}).second) out.push_back({la, prev, "comment"});
        k = j + 1;
    }
    std::sort(out.begin(), out.end(), [](const LuaLspFold &a, const LuaLspFold &b) {
        return a.start_line != b.start_line ? a.start_line < b.start_line : a.end_line > b.end_line;
    });
    return out;
}

// ---- formatting ------------------------------------------------------------------------------------------------

std::vector<LuaLspTextEdit> LuaLspFormat(const std::vector<std::string> &lines, int width, std::string *error) {
    std::vector<LuaLspTextEdit> edits;
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    luafmt::Options o;
    if (width > 0) o.width = width;
    const luafmt::Result r = luafmt::Format(text, o);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    // The formatter always ends the file with a newline; the lines here
    // carry one only as an empty last line.
    std::string out = r.text;
    if (!out.empty() && out.back() == '\n' && (text.empty() || text.back() != '\n')) out.pop_back();
    if (out == text) return edits;
    // One edit over the lines that changed.
    std::vector<std::string> b;
    {
        size_t i = 0;
        while (true) {
            const size_t j = out.find('\n', i);
            if (j == std::string::npos) {
                b.push_back(out.substr(i));
                break;
            }
            b.push_back(out.substr(i, j - i));
            i = j + 1;
        }
    }
    size_t pre = 0;
    while (pre < lines.size() && pre < b.size() && lines[pre] == b[pre]) pre++;
    size_t suf = 0;
    while (suf < lines.size() - pre && suf < b.size() - pre && lines[lines.size() - 1 - suf] == b[b.size() - 1 - suf])
        suf++;
    LuaLspTextEdit e;
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

std::string LuaLspLuacCheck(const std::string &text, const std::string &chunkname) {
    const auto r = luaparse::Parse(text);
    if (r->errors.empty()) return std::string();
    return luaparse::FormatError(chunkname, r->errors[0]);
}
