// mep's Nix language server, analysis half: see nix_lsp.h.
#include "nix_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <unordered_map>
#include <utility>

#include "nix_format.h"
#include "nix_parse.h"

using nixparse::AttrDef;
using nixparse::AttrName;
using nixparse::Binding;
using nixparse::Node;
using nixparse::NodeKind;

// ---- builtins -------------------------------------------------------------------

namespace {
std::map<std::string, std::string> &BuiltinDocs() {
    static std::map<std::string, std::string> docs;
    return docs;
}
}  // namespace

const NixLspBuiltin *NixLspFindBuiltin(const std::string &name) {
    static const std::unordered_map<std::string, const NixLspBuiltin *> index = [] {
        std::unordered_map<std::string, const NixLspBuiltin *> m;
        size_t n = 0;
        const NixLspBuiltin *t = NixLspBuiltinTable(&n);
        for (size_t k = 0; k < n; k++) m.emplace(t[k].name, &t[k]);
        return m;
    }();
    const auto it = index.find(name);
    return it == index.end() ? nullptr : it->second;
}

void NixLspSetBuiltinDocs(std::map<std::string, std::string> docs) { BuiltinDocs() = std::move(docs); }

const std::string &NixLspBuiltinDoc(const std::string &name) {
    static const std::string kEmpty;
    const auto it = BuiltinDocs().find(name);
    return it == BuiltinDocs().end() ? kEmpty : it->second;
}

// ---- the analysed document ------------------------------------------------------------

namespace {

struct Def;
struct Scope;

struct Use {
    Node *var = nullptr;      // null for an implicit use (an operator's __sub, <path>'s __findFile)
    int start = 0, end = 0;
    int nix_offset = 0;
    Def *def = nullptr;       // null: dynamic (from a `with`) or undefined
    bool dynamic = false;
    bool inherit_use = false; // the Var of `inherit x;`
    Scope *scope = nullptr;   // where the use is
};

struct Def {
    enum Kind { Let, Rec, Arg, Formal, Builtin };
    Kind kind = Let;
    std::string name;
    int name_start = 0, name_end = 0;
    std::vector<std::pair<int, int>> decl_sites;
    Node *value = nullptr;    // let/rec: the attribute's value; formal: its default
    Node *owner = nullptr;    // the Let / Attrs / Lambda node
    int binding_start = 0;    // for doc comments
    AttrDef::Kind attr_kind = AttrDef::Plain;
    std::vector<int> uses;
    const NixLspBuiltin *builtin = nullptr;
    Scope *scope = nullptr;
};

struct Scope {
    enum Kind { Let, Rec, Lambda, With, Global };
    Kind kind = Let;
    std::unordered_map<std::string, Def *> vars;
    Node *node = nullptr;   // the Let / Attrs / Lambda / With
    Scope *up = nullptr;
    int start = 0, end = 0; // where its names are visible
    bool with_used = false;
};

}  // namespace

class NixLspDocument {
public:
    std::string text;
    std::vector<int> line_starts;
    std::unique_ptr<nixparse::ParseResult> parse;
    std::vector<std::unique_ptr<Scope>> scopes;
    std::vector<std::unique_ptr<Def>> defs;
    std::vector<Use> uses;
    std::unordered_map<std::string, Def *> builtin_defs;
    std::vector<Node *> uris;
    std::vector<Node *> withs;
    std::vector<Node *> lets;
    std::vector<Node *> rec_sets;
    std::unordered_map<const Node *, Scope *> scope_of;  // Let / rec Attrs / Lambda / With -> its scope
    std::unordered_map<const Node *, int> use_of_var;    // Var -> index into uses
    std::vector<std::pair<int, int>> undefined;          // (nix offset, use index)
    Scope *global = nullptr;

    NixLspRange Range(int a, int b) const {
        NixLspRange r;
        Pos(a, &r.line, &r.col);
        Pos(std::max(a, b), &r.end_line, &r.end_col);
        return r;
    }
    void Pos(int off, int *line, int *col) const {
        off = std::max(0, std::min(off, static_cast<int>(text.size())));
        const auto it = std::upper_bound(line_starts.begin(), line_starts.end(), off);
        const int ln = static_cast<int>(it - line_starts.begin()) - 1;
        *line = ln;
        *col = off - line_starts[static_cast<size_t>(ln)];
    }
    int Offset(int line, int col) const {
        if (line < 0) return 0;
        if (static_cast<size_t>(line) >= line_starts.size()) return static_cast<int>(text.size());
        const int start = line_starts[static_cast<size_t>(line)];
        const int next = static_cast<size_t>(line) + 1 < line_starts.size()
                             ? line_starts[static_cast<size_t>(line) + 1] - 1
                             : static_cast<int>(text.size());
        return std::min(start + std::max(0, col), next);
    }
    std::string Slice(int a, int b) const {
        a = std::max(0, std::min(a, static_cast<int>(text.size())));
        b = std::max(a, std::min(b, static_cast<int>(text.size())));
        return text.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
    }
};

namespace {

// ---- resolution (Nix's bindVars) --------------------------------------------------------------

class Resolver {
public:
    explicit Resolver(NixLspDocument *doc) : d_(doc) {}

    void Run() {
        auto g = std::make_unique<Scope>();
        g->kind = Scope::Global;
        g->start = 0;
        g->end = static_cast<int>(d_->text.size());
        d_->global = g.get();
        size_t n = 0;
        const NixLspBuiltin *t = NixLspBuiltinTable(&n);
        for (size_t k = 0; k < n; k++) {
            for (int dunder = 0; dunder < 2; dunder++) {
                if (dunder ? !t[k].dunder : !t[k].bare) continue;
                auto def = std::make_unique<Def>();
                def->kind = Def::Builtin;
                def->name = (dunder ? "__" : "") + std::string(t[k].name);
                def->builtin = &t[k];
                def->scope = g.get();
                g->vars[def->name] = def.get();
                d_->builtin_defs[def->name] = def.get();
                d_->defs.push_back(std::move(def));
            }
        }
        d_->scopes.push_back(std::move(g));
        Visit(d_->parse->root, d_->global);
    }

private:
    NixLspDocument *d_;

    Scope *NewScope(Scope::Kind kind, Node *node, Scope *up, int start, int end) {
        auto s = std::make_unique<Scope>();
        s->kind = kind;
        s->node = node;
        s->up = up;
        s->start = start;
        s->end = end;
        Scope *p = s.get();
        d_->scopes.push_back(std::move(s));
        if (node) d_->scope_of[node] = p;
        return p;
    }

    Def *NewDef(Def::Kind kind, const std::string &name, int a, int b, Scope *scope, Node *owner) {
        auto def = std::make_unique<Def>();
        def->kind = kind;
        def->name = name;
        def->name_start = a;
        def->name_end = b;
        def->scope = scope;
        def->owner = owner;
        Def *p = def.get();
        d_->defs.push_back(std::move(def));
        if (!name.empty()) scope->vars.emplace(name, p);
        return p;
    }

    // The let / rec bindings of `n` become definitions in `scope`.
    void BindAttrs(Node *n, Scope *scope, Def::Kind kind) {
        std::unordered_map<std::string, std::vector<std::pair<int, int>>> sites;
        for (const Binding &b : n->bindings) {
            if (b.kind == Binding::Assign) {
                if (!b.path.empty() && !b.path[0].dynamic && !b.path[0].missing)
                    sites[b.path[0].name].emplace_back(b.path[0].start, b.path[0].end);
            } else {
                for (const AttrName &a : b.path) {
                    if (!a.missing) sites[a.name].emplace_back(a.start, a.end);
                }
            }
        }
        for (const AttrDef &a : n->attrs) {
            Def *def = NewDef(kind, a.name, a.name_start, a.name_end, scope, n);
            def->value = a.kind == AttrDef::Plain ? a.value : nullptr;
            def->attr_kind = a.kind;
            def->binding_start = a.pos;
            const auto it = sites.find(a.name);
            if (it != sites.end()) {
                def->decl_sites = it->second;
                def->name_start = it->second[0].first;
                def->name_end = it->second[0].second;
            } else {
                def->decl_sites.emplace_back(a.name_start, a.name_end);
            }
        }
    }

    void Lookup(const std::string &name, Node *var, int start, int end, int nix_offset, Scope *env,
                bool inherit_use) {
        Use u;
        u.var = var;
        u.start = start;
        u.end = end;
        u.nix_offset = nix_offset;
        u.inherit_use = inherit_use;
        u.scope = env;
        bool any_with = false;
        for (Scope *s = env; s; s = s->up) {
            if (s->kind == Scope::With) {
                any_with = true;
                continue;
            }
            const auto it = s->vars.find(name);
            if (it != s->vars.end()) {
                u.def = it->second;
                break;
            }
        }
        if (!u.def && any_with) {
            u.dynamic = true;
            for (Scope *s = env; s; s = s->up) {
                if (s->kind == Scope::With) s->with_used = true;
            }
        }
        const int idx = static_cast<int>(d_->uses.size());
        if (u.def) u.def->uses.push_back(idx);
        if (!u.def && !u.dynamic && var) d_->undefined.emplace_back(nix_offset, idx);
        if (var) d_->use_of_var[var] = idx;
        d_->uses.push_back(u);
    }

    void Implicit(const char *name, Scope *env) { Lookup(name, nullptr, 0, 0, 0, env, false); }

    void VisitAttrsValues(Node *n, Scope *inner, Scope *outer) {
        for (Node *f : n->inherit_from) Visit(f, inner);
        for (AttrDef &a : n->attrs) {
            if (a.kind == AttrDef::Plain) {
                Visit(a.value, inner);
            } else if (a.kind == AttrDef::Inherited && a.value) {
                if (!a.value->missing)
                    Lookup(a.value->name, a.value, a.value->start, a.value->end, a.value->start, outer, true);
            }
        }
        for (nixparse::DynamicAttrDef &dd : n->dynamic_attrs) {
            Visit(dd.name_expr, inner);
            Visit(dd.value, inner);
        }
    }

    void Visit(Node *n, Scope *env) {
        if (!n) return;
        switch (n->kind) {
            case NodeKind::Var:
                if (!n->missing) Lookup(n->name, n, n->start, n->end, n->nix_pos >= 0 ? n->nix_pos : n->start, env, false);
                return;
            case NodeKind::Attrs:
                if (n->recursive) {
                    Scope *s = NewScope(Scope::Rec, n, env, n->start, n->end);
                    BindAttrs(n, s, Def::Rec);
                    if (!n->old_let) d_->rec_sets.push_back(n);
                    VisitAttrsValues(n, s, env);
                } else {
                    VisitAttrsValues(n, env, env);
                }
                return;
            case NodeKind::Let: {
                Scope *s = NewScope(Scope::Let, n, env, n->start, n->end);
                BindAttrs(n, s, Def::Let);
                d_->lets.push_back(n);
                VisitAttrsValues(n, s, env);
                Visit(n->body, s);
                return;
            }
            case NodeKind::Lambda: {
                Scope *s = NewScope(Scope::Lambda, n, env, n->start, n->end);
                if (!n->arg.empty()) NewDef(Def::Arg, n->arg, n->arg_start, n->arg_end, s, n);
                for (nixparse::Formal &f : n->formals) {
                    if (f.name.empty() || s->vars.count(f.name)) continue;
                    Def *def = NewDef(Def::Formal, f.name, f.start, f.end, s, n);
                    def->value = f.def;
                    def->binding_start = f.start;
                    def->decl_sites.emplace_back(f.start, f.end);
                }
                for (Def *def : CollectArgDefs(s)) {
                    if (def->decl_sites.empty()) def->decl_sites.emplace_back(def->name_start, def->name_end);
                }
                for (nixparse::Formal &f : n->formals) Visit(f.def, s);
                Visit(n->body, s);
                return;
            }
            case NodeKind::With: {
                Visit(n->kids.empty() ? nullptr : n->kids[0], env);
                const int body_start = n->body ? n->body->start : n->end;
                Scope *s = NewScope(Scope::With, n, env, body_start, n->end);
                d_->withs.push_back(n);
                Visit(n->body, s);
                return;
            }
            case NodeKind::Select:
                Visit(n->e, env);
                Visit(n->def, env);
                for (AttrName &a : n->path) Visit(a.expr, env);
                return;
            case NodeKind::HasAttr:
                Visit(n->e, env);
                for (AttrName &a : n->path) Visit(a.expr, env);
                return;
            case NodeKind::SPath:
                Implicit("__findFile", env);
                Implicit("__nixPath", env);
                return;
            case NodeKind::Uri:
                d_->uris.push_back(n);
                return;
            case NodeKind::Negate:
                Implicit("__sub", env);
                break;
            case NodeKind::BinOp: {
                const std::string &op = n->text;
                if (op == "-") Implicit("__sub", env);
                if (op == "*") Implicit("__mul", env);
                if (op == "/") Implicit("__div", env);
                if (op == "<" || op == ">" || op == "<=" || op == ">=") Implicit("__lessThan", env);
                break;
            }
            default:
                break;
        }
        for (Node *k : n->kids) Visit(k, env);
        Visit(n->body, env);
    }

    std::vector<Def *> CollectArgDefs(Scope *s) {
        std::vector<Def *> out;
        for (auto &kv : s->vars) out.push_back(kv.second);
        return out;
    }
};

std::string JoinLines(const std::vector<std::string> &lines) {
    std::string s;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) s += '\n';
        s += lines[i];
    }
    return s;
}

// ---- helpers over the tree -----------------------------------------------------------------------

template <class F>
void Walk(Node *n, const F &f) {
    if (!n) return;
    if (!f(n)) return;
    for (Node *k : n->kids) Walk(k, f);
    Walk(n->body, f);
    Walk(n->e, f);
    Walk(n->def, f);
    for (AttrName &a : n->path) Walk(a.expr, f);
    for (Node *k : n->inherit_from) Walk(k, f);
    for (AttrDef &a : n->attrs) {
        if (a.value) Walk(a.value, f);
    }
    for (auto &dd : n->dynamic_attrs) {
        Walk(dd.name_expr, f);
        Walk(dd.value, f);
    }
    for (nixparse::Formal &fm : n->formals) Walk(fm.def, f);
}

bool IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\'' || c == '-';
}

std::string OneLine(const std::string &s, size_t max = 80) {
    std::string out;
    bool more = false;
    for (char c : s) {
        if (c == '\n') {
            more = true;
            break;
        }
        out += c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    if (out.size() > max) {
        out.resize(max);
        more = true;
    }
    if (more) out += " …";
    return out;
}

const Def *DefAtUse(const NixLspDocument &doc, const Node *var) {
    const auto it = doc.use_of_var.find(var);
    if (it == doc.use_of_var.end()) return nullptr;
    return doc.uses[static_cast<size_t>(it->second)].def;
}

// The innermost Var node at `off` (inclusive of its end, so a cursor just after a name finds it).
Node *VarAt(const NixLspDocument &doc, int off) {
    Node *best = nullptr;
    Walk(doc.parse->root, [&](Node *n) {
        if (n->kind == NodeKind::Var && !n->missing && n->start <= off && off <= n->end) {
            if (!best || n->end - n->start <= best->end - best->start) best = n;
        }
        return true;
    });
    return best;
}

// A definition whose declaration site covers `off`.
Def *DefDeclAt(const NixLspDocument &doc, int off) {
    for (const auto &d : doc.defs) {
        if (d->kind == Def::Builtin) continue;
        for (const auto &s : d->decl_sites) {
            if (s.first <= off && off <= s.second) return d.get();
        }
        if (d->name_start <= off && off <= d->name_end && d->name_end > d->name_start) return d.get();
    }
    return nullptr;
}

const Def *TargetAt(const NixLspDocument &doc, int off) {
    if (Node *v = VarAt(doc, off)) {
        if (const Def *d = DefAtUse(doc, v)) return d;
    }
    return DefDeclAt(doc, off);
}

// The attribute set `e` statically is, when this file says so: a set literal, a variable bound to one, a
// selection from one, a `let`/`with` whose body is one.
Node *StaticAttrs(const NixLspDocument &doc, Node *e, int depth = 0) {
    if (!e || depth > 24) return nullptr;
    switch (e->kind) {
        case NodeKind::Attrs:
            return e->old_let ? nullptr : e;
        case NodeKind::Var: {
            const Def *d = DefAtUse(doc, e);
            if (d && (d->kind == Def::Let || d->kind == Def::Rec) && d->value) return StaticAttrs(doc, d->value, depth + 1);
            return nullptr;
        }
        case NodeKind::Select: {
            Node *base = StaticAttrs(doc, e->e, depth + 1);
            for (const AttrName &a : e->path) {
                if (!base || a.dynamic) return nullptr;
                Node *next = nullptr;
                for (AttrDef &ad : base->attrs) {
                    if (ad.name == a.name && ad.kind == AttrDef::Plain) next = StaticAttrs(doc, ad.value, depth + 1);
                }
                base = next;
            }
            return base;
        }
        case NodeKind::Let:
        case NodeKind::With:
        case NodeKind::Assert:
            return StaticAttrs(doc, e->body, depth + 1);
        default:
            return nullptr;
    }
}

bool InCommentOrString(const NixLspDocument &doc, int off) {
    for (const nixparse::Comment &c : doc.parse->comments) {
        if (c.start < off && off <= c.end) {
            if (!c.block) return true;
            if (off < c.end) return true;
        }
    }
    bool in_str = false;
    Walk(doc.parse->root, [&](Node *n) {
        if (n->end < off || n->start > off) return false;
        if ((n->kind == NodeKind::String || n->kind == NodeKind::IndString) && n->start < off && off < n->end) {
            bool in_interp = false;
            for (Node *k : n->kids) {
                if (k->start <= off && off <= k->end) in_interp = true;
            }
            if (!in_interp) in_str = true;
        }
        return true;
    });
    return in_str;
}

std::string DocCommentBefore(const NixLspDocument &doc, int start) {
    const auto &cs = doc.parse->comments;
    int at = start;
    std::vector<const nixparse::Comment *> run;
    for (size_t k = cs.size(); k-- > 0;) {
        const nixparse::Comment &c = cs[k];
        if (c.end > at) continue;
        const std::string between = doc.Slice(c.end, at);
        if (between.find_first_not_of(" \t\r\n") != std::string::npos) break;
        if (std::count(between.begin(), between.end(), '\n') > 1) break;
        run.push_back(&c);
        at = c.start;
        if (c.block) break;
    }
    std::string out;
    for (size_t k = run.size(); k-- > 0;) {
        std::string t = doc.Slice(run[k]->start, run[k]->end);
        if (run[k]->block) {
            t = t.substr(run[k]->doc ? 3 : 2, t.size() - (run[k]->doc ? 3 : 2) - 2);
        } else {
            t = t.substr(1);
            if (!t.empty() && t[0] == ' ') t = t.substr(1);
        }
        if (!out.empty()) out += '\n';
        out += t;
    }
    // Trim common indentation of block comments.
    while (!out.empty() && (out.front() == '\n' || out.front() == ' ')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

std::string BuiltinSignature(const NixLspBuiltin &b) {
    std::string s = std::string("builtins.") + b.name;
    if (*b.args) s += std::string(" ") + b.args;
    return s;
}

std::string BuiltinHover(const NixLspBuiltin &b) {
    std::string out = "```nix\n" + BuiltinSignature(b) + "\n```\n";
    std::string also;
    if (b.bare) also = std::string("`") + b.name + "`";
    if (b.dunder) also += (also.empty() ? "`__" : ", `__") + std::string(b.name) + "`";
    out += std::string("*builtin ") + b.kind + "*";
    if (!also.empty()) out += " — in scope everywhere as " + also;
    if (*b.experimental) out += std::string(" — needs experimental feature `") + b.experimental + "`";
    out += "\n";
    const std::string &docs = NixLspBuiltinDoc(b.name);
    if (!docs.empty()) out += "\n" + docs + "\n";
    return out;
}

std::string DefHover(const NixLspDocument &doc, const Def &d) {
    if (d.kind == Def::Builtin) return BuiltinHover(*d.builtin);
    int line = 0, col = 0;
    doc.Pos(d.name_start, &line, &col);
    std::string out = "```nix\n";
    switch (d.kind) {
        case Def::Let:
        case Def::Rec:
            if (d.attr_kind == AttrDef::Plain && d.value) {
                out += d.name + " = " + OneLine(doc.Slice(d.value->start, d.value->end)) + "\n";
            } else {
                out += "inherit " + d.name + ";\n";
            }
            out += "```\n";
            out += std::string(d.kind == Def::Let ? "`let` binding" : "`rec` attribute") + ", line " +
                   std::to_string(line + 1) + "\n";
            break;
        case Def::Formal:
            out += "{ " + d.name + (d.value ? " ? " + OneLine(doc.Slice(d.value->start, d.value->end), 60) : "") +
                   ", ... }:\n```\nnamed argument of the function at line " + std::to_string(line + 1) + "\n";
            break;
        case Def::Arg:
            out += d.name + ":\n```\nargument of the function at line " + std::to_string(line + 1) + "\n";
            break;
        default:
            break;
    }
    const std::string comment = DocCommentBefore(doc, d.binding_start ? d.binding_start : d.name_start);
    if (!comment.empty()) out += "\n" + comment + "\n";
    return out;
}

}  // namespace

// ---- analysis entry point --------------------------------------------------------------------------

std::shared_ptr<const NixLspDocument> NixLspAnalyze(const std::vector<std::string> &lines, const NixLspOptions &opts) {
    auto doc = std::make_shared<NixLspDocument>();
    doc->text = JoinLines(lines);
    doc->line_starts.push_back(0);
    for (size_t i = 0; i < doc->text.size(); i++) {
        if (doc->text[i] == '\n') doc->line_starts.push_back(static_cast<int>(i + 1));
    }
    nixparse::Options po;
    po.origin = opts.origin;
    po.pipe_operators = opts.pipe_operators;
    doc->parse = nixparse::Parse(doc->text, po);
    Resolver(doc.get()).Run();
    return doc;
}

NixLspCheck NixLspNixCheck(const std::string &text, const NixLspOptions &opts) {
    NixLspCheck out;
    auto doc = std::make_shared<NixLspDocument>();
    doc->text = text;
    doc->line_starts.push_back(0);
    nixparse::Options po;
    po.origin = opts.origin;
    po.pipe_operators = opts.pipe_operators;
    doc->parse = nixparse::Parse(doc->text, po);
    if (!doc->parse->errors.empty()) {
        out.parse_error = true;
        out.offset = doc->parse->errors[0].nix_offset;
        out.message = doc->parse->errors[0].message;
        return out;
    }
    Resolver(doc.get()).Run();
    for (const auto &u : doc->undefined) {
        const Use &use = doc->uses[static_cast<size_t>(u.second)];
        out.undefined.emplace_back(u.first, use.var ? use.var->name : std::string());
    }
    return out;
}

// ---- diagnostics ---------------------------------------------------------------------------------------

std::vector<NixLspDiagnostic> NixLspDiagnostics(const NixLspDocument &doc) {
    std::vector<NixLspDiagnostic> out;
    const nixparse::ParseResult &p = *doc.parse;
    const int cut = p.has_syntax_error ? p.syntax_error_offset : 1 << 30;
    auto add = [&](int a, int b, NixLspSeverity sev, const std::string &code, const std::string &msg) {
        NixLspDiagnostic d;
        d.range = doc.Range(a, b);
        d.severity = sev;
        d.code = code;
        d.message = msg;
        out.push_back(d);
        return &out.back();
    };
    for (const nixparse::Diagnostic &e : p.errors) {
        if (!e.syntax && e.start >= cut) continue;
        add(e.start, e.end, NixLspSeverity::Error, e.code, e.message);
    }
    for (const nixparse::Diagnostic &w : p.warnings) {
        if (w.start < cut) add(w.start, w.end, NixLspSeverity::Warning, w.code, w.message);
    }
    if (p.recovered) {
        for (const auto &u : doc.undefined) {
            const Use &use = doc.uses[static_cast<size_t>(u.second)];
            if (use.start >= cut) continue;
            add(use.start, use.end, NixLspSeverity::Error, "undefined-variable",
                "undefined variable '" + use.var->name + "'");
        }
    }
    if (p.has_syntax_error) return out;

    for (const auto &dp : doc.defs) {
        const Def &d = *dp;
        if (!d.uses.empty() || d.name.empty() || d.name[0] == '_') continue;
        if (d.kind == Def::Let) {
            NixLspDiagnostic *x = add(d.name_start, d.name_end, NixLspSeverity::Warning, "unused-let",
                                      "unused let binding '" + d.name + "'");
            x->unnecessary = true;
        } else if (d.kind == Def::Formal || d.kind == Def::Arg) {
            NixLspDiagnostic *x = add(d.name_start, d.name_end, NixLspSeverity::Hint, "unused-argument",
                                      "unused function argument '" + d.name + "'");
            x->unnecessary = true;
        }
    }
    for (Node *w : doc.withs) {
        const auto it = doc.scope_of.find(w);
        if (it == doc.scope_of.end() || it->second->with_used) continue;
        const int end = w->kids.empty() || !w->kids[0] ? w->start + 4 : w->kids[0]->end;
        NixLspDiagnostic *x = add(w->start, end, NixLspSeverity::Warning, "unused-with",
                                  "nothing in this `with` is used: every name in its body is bound elsewhere");
        x->unnecessary = true;
    }
    for (Node *l : doc.lets) {
        if (l->attrs.empty() && l->dynamic_attrs.empty()) {
            NixLspDiagnostic *x = add(l->start, l->start + 3, NixLspSeverity::Hint, "empty-let", "empty let");
            x->unnecessary = true;
        }
    }
    for (Node *r : doc.rec_sets) {
        const auto it = doc.scope_of.find(r);
        if (it == doc.scope_of.end() || r->attrs.empty()) continue;
        bool used = false;
        for (const auto &kv : it->second->vars) {
            if (!kv.second->uses.empty()) used = true;
        }
        if (!used) {
            NixLspDiagnostic *x = add(r->start, r->start + 3, NixLspSeverity::Hint, "redundant-rec",
                                      "`rec` is unnecessary: no attribute refers to another");
            x->unnecessary = true;
        }
    }
    Walk(p.root, [&](Node *n) {
        if (n->kind == NodeKind::Attrs && n->old_let) {
            NixLspDiagnostic *x = add(n->start, n->start + 3, NixLspSeverity::Warning, "deprecated-let",
                                      "`let { ...; body = ...; }` is deprecated: use `let ... in ...`");
            x->deprecated = true;
        }
        return true;
    });
    for (Node *u : doc.uris) {
        NixLspDiagnostic *x = add(u->start, u->end, NixLspSeverity::Warning, "url-literal",
                                  "unquoted URL literal (RFC 45): write it as a string, \"" + u->text + "\"");
        x->deprecated = true;
    }
    std::stable_sort(out.begin(), out.end(), [](const NixLspDiagnostic &a, const NixLspDiagnostic &b) {
        if (a.range.line != b.range.line) return a.range.line < b.range.line;
        return a.range.col < b.range.col;
    });
    return out;
}

std::string NixLspFormatDiagnostic(const std::string &path, const NixLspDiagnostic &d) {
    const char *sev = d.severity == NixLspSeverity::Error         ? "error"
                      : d.severity == NixLspSeverity::Warning     ? "warning"
                      : d.severity == NixLspSeverity::Information ? "info"
                                                                  : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool NixLspDiagnosticFails(const NixLspDiagnostic &d) {
    return d.severity == NixLspSeverity::Error || d.severity == NixLspSeverity::Warning;
}

// ---- completion -------------------------------------------------------------------------------------------

std::vector<NixLspCompletionItem> NixLspCompletions(const NixLspDocument &doc, int line, int col) {
    std::vector<NixLspCompletionItem> out;
    const int off = doc.Offset(line, col);
    if (InCommentOrString(doc, off)) return out;
    int ws = off;
    while (ws > 0 && IsWordChar(doc.text[static_cast<size_t>(ws - 1)])) ws--;
    int line0 = 0, start_col = 0;
    doc.Pos(ws, &line0, &start_col);
    std::set<std::string> seen;
    auto item = [&](const std::string &label, NixLspKind kind, const std::string &detail, const std::string &docs) {
        if (!seen.insert(label).second) return;
        NixLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = docs;
        it.replace_start = start_col;
        it.replace_end = col;
        out.push_back(it);
    };

    // `x.` -- the attributes of x.
    if (ws > 0 && doc.text[static_cast<size_t>(ws - 1)] == '.') {
        Node *sel = nullptr;
        size_t elem = 0;
        Walk(doc.parse->root, [&](Node *n) {
            if (n->kind == NodeKind::Select || n->kind == NodeKind::HasAttr) {
                for (size_t k = 0; k < n->path.size(); k++) {
                    if (n->path[k].start <= ws && ws <= n->path[k].end) {
                        sel = n;
                        elem = k;
                    }
                }
            }
            return true;
        });
        if (!sel) return out;
        Node *base = sel->e;
        const bool is_builtins = base && base->kind == NodeKind::Var && base->name == "builtins" && elem == 0 &&
                                 DefAtUse(doc, base) && DefAtUse(doc, base)->kind == Def::Builtin;
        if (is_builtins) {
            size_t n = 0;
            const NixLspBuiltin *t = NixLspBuiltinTable(&n);
            for (size_t k = 0; k < n; k++) {
                if (!t[k].member) continue;
                item(t[k].name, std::strcmp(t[k].kind, "function") == 0 ? NixLspKind::Function : NixLspKind::Constant,
                     BuiltinSignature(t[k]), NixLspBuiltinDoc(t[k].name));
            }
            return out;
        }
        Node *attrs = StaticAttrs(doc, base);
        for (size_t k = 0; attrs && k < elem; k++) {
            Node *next = nullptr;
            if (sel->path[k].dynamic) {
                attrs = nullptr;
                break;
            }
            for (AttrDef &ad : attrs->attrs) {
                if (ad.name == sel->path[k].name && ad.kind == AttrDef::Plain) next = StaticAttrs(doc, ad.value);
            }
            attrs = next;
        }
        if (attrs) {
            for (AttrDef &ad : attrs->attrs) {
                const std::string preview =
                    ad.kind == AttrDef::Plain && ad.value ? OneLine(doc.Slice(ad.value->start, ad.value->end), 60)
                                                          : std::string("inherited");
                item(ad.name,
                     ad.value && ad.value->kind == NodeKind::Lambda ? NixLspKind::Function
                     : ad.value && ad.value->kind == NodeKind::Attrs ? NixLspKind::Module
                                                                     : NixLspKind::Field,
                     preview, "");
            }
        }
        return out;
    }

    // Names in scope, innermost first.
    std::vector<const Scope *> scopes;
    for (const auto &s : doc.scopes) {
        if (s->kind == Scope::Global) continue;
        if (s->start <= off && off <= s->end) scopes.push_back(s.get());
    }
    std::sort(scopes.begin(), scopes.end(),
              [](const Scope *a, const Scope *b) { return a->end - a->start < b->end - b->start; });
    for (const Scope *s : scopes) {
        if (s->kind == Scope::With) {
            Node *w = s->node;
            if (Node *attrs = StaticAttrs(doc, w->kids.empty() ? nullptr : w->kids[0])) {
                for (AttrDef &ad : attrs->attrs) item(ad.name, NixLspKind::Field, "from `with`", "");
            }
            continue;
        }
        std::vector<const Def *> defs;
        for (const auto &kv : s->vars) defs.push_back(kv.second);
        std::sort(defs.begin(), defs.end(), [](const Def *a, const Def *b) { return a->name < b->name; });
        for (const Def *d : defs) {
            const char *what = d->kind == Def::Let ? "let" : d->kind == Def::Rec ? "rec attribute" : "argument";
            NixLspKind kind = d->value && d->value->kind == NodeKind::Lambda ? NixLspKind::Function
                                                                             : NixLspKind::Variable;
            item(d->name, kind, what, "");
        }
    }
    item("builtins", NixLspKind::Module, "builtins", "");
    size_t n = 0;
    const NixLspBuiltin *t = NixLspBuiltinTable(&n);
    for (size_t k = 0; k < n; k++) {
        if (!t[k].bare || std::strcmp(t[k].name, "builtins") == 0) continue;
        item(t[k].name, std::strcmp(t[k].kind, "function") == 0 ? NixLspKind::Function : NixLspKind::Constant,
             BuiltinSignature(t[k]), NixLspBuiltinDoc(t[k].name));
    }
    for (const char *kw : {"if", "then", "else", "let", "in", "with", "assert", "rec", "inherit", "or"})
        item(kw, NixLspKind::Keyword, "keyword", "");
    return out;
}

// ---- hover ----------------------------------------------------------------------------------------------------

NixLspHoverInfo NixLspHover(const NixLspDocument &doc, int line, int col) {
    NixLspHoverInfo h;
    const int off = doc.Offset(line, col);
    if (Node *v = VarAt(doc, off)) {
        const auto it = doc.use_of_var.find(v);
        h.range = doc.Range(v->start, v->end);
        if (it != doc.use_of_var.end()) {
            const Use &u = doc.uses[static_cast<size_t>(it->second)];
            if (u.def) {
                h.found = true;
                h.text = DefHover(doc, *u.def);
                return h;
            }
            if (u.dynamic) {
                std::string s = "```nix\n" + v->name + "\n```\nfrom ";
                bool first = true;
                for (Scope *sc = u.scope; sc; sc = sc->up) {
                    if (sc->kind != Scope::With) continue;
                    Node *w = sc->node;
                    Node *we = w->kids.empty() ? nullptr : w->kids[0];
                    if (!first) s += ", else ";
                    first = false;
                    s += "`with " + (we ? OneLine(doc.Slice(we->start, we->end), 50) : std::string()) + ";`";
                    if (Node *attrs = StaticAttrs(doc, we)) {
                        for (AttrDef &ad : attrs->attrs) {
                            if (ad.name == v->name && ad.value)
                                s += " (= `" + OneLine(doc.Slice(ad.value->start, ad.value->end), 60) + "`)";
                        }
                    }
                }
                h.found = true;
                h.text = s + "\n";
                return h;
            }
            h.found = true;
            h.text = "```nix\n" + v->name + "\n```\nundefined variable\n";
            return h;
        }
    }
    // An attribute in a selection: builtins.x, or a key of a set this file writes.
    Node *sel = nullptr;
    size_t elem = 0;
    Walk(doc.parse->root, [&](Node *n) {
        if (n->kind == NodeKind::Select || n->kind == NodeKind::HasAttr) {
            for (size_t k = 0; k < n->path.size(); k++) {
                if (!n->path[k].dynamic && n->path[k].start <= off && off <= n->path[k].end) {
                    sel = n;
                    elem = k;
                }
            }
        }
        return true;
    });
    if (sel) {
        const AttrName &a = sel->path[elem];
        h.range = doc.Range(a.start, a.end);
        Node *base = sel->e;
        if (elem == 0 && base && base->kind == NodeKind::Var && base->name == "builtins") {
            if (const NixLspBuiltin *b = NixLspFindBuiltin(a.name)) {
                h.found = true;
                h.text = BuiltinHover(*b);
                return h;
            }
        }
        Node *attrs = StaticAttrs(doc, base);
        for (size_t k = 0; attrs && k < elem; k++) {
            Node *next = nullptr;
            for (AttrDef &ad : attrs->attrs) {
                if (ad.name == sel->path[k].name && ad.kind == AttrDef::Plain) next = StaticAttrs(doc, ad.value);
            }
            attrs = next;
        }
        if (attrs) {
            for (AttrDef &ad : attrs->attrs) {
                if (ad.name != a.name) continue;
                h.found = true;
                h.text = "```nix\n" + ad.name + " = " +
                         (ad.value ? OneLine(doc.Slice(ad.value->start, ad.value->end)) : std::string("…")) +
                         "\n```\n";
                const std::string c = DocCommentBefore(doc, ad.pos);
                if (!c.empty()) h.text += "\n" + c + "\n";
                return h;
            }
        }
    }
    if (Def *d = DefDeclAt(doc, off)) {
        h.found = true;
        h.text = DefHover(doc, *d);
        h.range = doc.Range(d->name_start, d->name_end);
    }
    return h;
}

// ---- signature help ----------------------------------------------------------------------------------------------

NixLspSignatureHelp NixLspSignatureAt(const NixLspDocument &doc, int line, int col) {
    NixLspSignatureHelp sh;
    const int off = doc.Offset(line, col);
    int p = off;
    while (p > 0 && std::isspace(static_cast<unsigned char>(doc.text[static_cast<size_t>(p - 1)]))) p--;
    // The innermost call containing the cursor, or ending right before it.
    Node *call = nullptr;
    Walk(doc.parse->root, [&](Node *n) {
        if (n->kind == NodeKind::Call && n->start < off && (off <= n->end || n->end == p)) {
            if (!call || n->end - n->start <= call->end - call->start) call = n;
        }
        return true;
    });
    Node *head = nullptr;
    int count = 0;
    if (call) {
        head = call->kids[0];
        for (size_t k = 1; k < call->kids.size(); k++) {
            Node *a = call->kids[k];
            if (a->end <= off && !(a->start <= off && off < a->end)) count++;
            if (a->start <= off && off <= a->end && off < call->end) {
                count = static_cast<int>(k) - 1;
                break;
            }
        }
        if (call->end == p && p < off) count = static_cast<int>(call->kids.size()) - 1;
    } else if (p < off) {
        Walk(doc.parse->root, [&](Node *n) {
            if ((n->kind == NodeKind::Var || n->kind == NodeKind::Select) && n->end == p) head = n;
            return true;
        });
    }
    if (!head) return sh;

    const NixLspBuiltin *b = nullptr;
    const Def *def = nullptr;
    std::string shown;
    if (head->kind == NodeKind::Var) {
        def = DefAtUse(doc, head);
        if (def && def->kind == Def::Builtin) b = def->builtin;
        shown = head->name;
    } else if (head->kind == NodeKind::Select && head->e && head->e->kind == NodeKind::Var &&
               head->e->name == "builtins" && head->path.size() == 1 && !head->path[0].dynamic) {
        b = NixLspFindBuiltin(head->path[0].name);
    }
    if (b) {
        sh.label = BuiltinSignature(*b);
        const std::string args = b->args;
        size_t i = 0;
        while (i < args.size()) {
            size_t j = args.find(' ', i);
            if (j == std::string::npos) j = args.size();
            sh.params.push_back(args.substr(i, j - i));
            i = j + 1;
        }
        sh.documentation = NixLspBuiltinDoc(b->name);
    } else if (def && (def->kind == Def::Let || def->kind == Def::Rec) && def->value &&
               def->value->kind == NodeKind::Lambda) {
        sh.label = shown + " = ";
        for (Node *l = def->value; l && l->kind == NodeKind::Lambda; l = l->body) {
            std::string param;
            if (l->has_formals) {
                param = "{ ";
                for (size_t k = 0; k < l->formals.size(); k++) {
                    if (k) param += ", ";
                    param += l->formals[k].name;
                    if (l->formals[k].def) param += " ? …";
                }
                if (l->ellipsis) param += l->formals.empty() ? "..." : ", ...";
                param += " }";
                if (!l->arg.empty()) param = l->arg + " @ " + param;
            } else {
                param = l->arg;
            }
            sh.params.push_back(param);
            sh.label += param + ": ";
        }
        sh.label += "…";
        sh.documentation = DocCommentBefore(doc, def->binding_start);
    } else {
        return sh;
    }
    if (count >= static_cast<int>(sh.params.size())) return sh;
    sh.found = true;
    sh.active_param = count;
    return sh;
}

// ---- navigation -----------------------------------------------------------------------------------------------------

std::vector<NixLspLocation> NixLspDefinition(const NixLspDocument &doc, int line, int col) {
    std::vector<NixLspLocation> out;
    const int off = doc.Offset(line, col);
    if (Node *v = VarAt(doc, off)) {
        const auto it = doc.use_of_var.find(v);
        if (it != doc.use_of_var.end()) {
            const Use &u = doc.uses[static_cast<size_t>(it->second)];
            if (u.def && u.def->kind != Def::Builtin) {
                out.push_back({doc.Range(u.def->name_start, u.def->name_end)});
                return out;
            }
            if (u.dynamic) {
                for (Scope *sc = u.scope; sc; sc = sc->up) {
                    if (sc->kind != Scope::With) continue;
                    Node *w = sc->node;
                    if (Node *attrs = StaticAttrs(doc, w->kids.empty() ? nullptr : w->kids[0])) {
                        for (AttrDef &ad : attrs->attrs) {
                            if (ad.name == v->name) {
                                out.push_back({doc.Range(ad.name_start, ad.name_end)});
                                return out;
                            }
                        }
                    }
                }
            }
            return out;
        }
    }
    Walk(doc.parse->root, [&](Node *n) {
        if (!out.empty()) return false;
        if (n->kind != NodeKind::Select && n->kind != NodeKind::HasAttr) return true;
        for (size_t k = 0; k < n->path.size(); k++) {
            const AttrName &a = n->path[k];
            if (a.dynamic || off < a.start || off > a.end) continue;
            Node *attrs = StaticAttrs(doc, n->e);
            for (size_t j = 0; attrs && j < k; j++) {
                Node *next = nullptr;
                for (AttrDef &ad : attrs->attrs) {
                    if (ad.name == n->path[j].name && ad.kind == AttrDef::Plain) next = StaticAttrs(doc, ad.value);
                }
                attrs = next;
            }
            if (attrs) {
                for (AttrDef &ad : attrs->attrs) {
                    if (ad.name == a.name) out.push_back({doc.Range(ad.name_start, ad.name_end)});
                }
            }
        }
        return true;
    });
    return out;
}

std::vector<NixLspReference> NixLspReferences(const NixLspDocument &doc, int line, int col) {
    std::vector<NixLspReference> out;
    const Def *d = TargetAt(doc, doc.Offset(line, col));
    if (!d) return out;
    std::set<std::pair<int, int>> seen;
    for (const auto &s : d->decl_sites) {
        if (seen.insert(s).second) out.push_back({doc.Range(s.first, s.second), true});
    }
    if (d->kind == Def::Builtin) {
        // A builtin has no declaration here; its uses are still worth highlighting.
    }
    for (int ui : d->uses) {
        const Use &u = doc.uses[static_cast<size_t>(ui)];
        if (!u.var) continue;
        if (seen.insert({u.start, u.end}).second) out.push_back({doc.Range(u.start, u.end), false});
    }
    return out;
}

NixLspRenameTarget NixLspPrepareRename(const NixLspDocument &doc, int line, int col) {
    NixLspRenameTarget t;
    const int off = doc.Offset(line, col);
    const Def *d = TargetAt(doc, off);
    if (!d || d->kind == Def::Builtin) return t;
    // The occurrence under the cursor.
    for (const auto &s : d->decl_sites) {
        if (s.first <= off && off <= s.second) {
            t.found = true;
            t.range = doc.Range(s.first, s.second);
        }
    }
    for (int ui : d->uses) {
        const Use &u = doc.uses[static_cast<size_t>(ui)];
        if (u.var && u.start <= off && off <= u.end) {
            t.found = true;
            t.range = doc.Range(u.start, u.end);
        }
    }
    t.placeholder = d->name;
    return t;
}

std::vector<NixLspTextEdit> NixLspRename(const NixLspDocument &doc, int line, int col, const std::string &new_name,
                                         std::string *error) {
    std::vector<NixLspTextEdit> edits;
    const Def *d = TargetAt(doc, doc.Offset(line, col));
    if (!d) {
        *error = "no binding here to rename";
        return edits;
    }
    if (d->kind == Def::Builtin) {
        *error = "'" + d->name + "' is a builtin";
        return edits;
    }
    if (!nixparse::IsNixIdentifier(new_name)) {
        *error = "'" + new_name + "' is not a Nix identifier";
        return edits;
    }
    if (new_name == d->name) return edits;
    if (d->kind == Def::Formal) {
        *error = "'" + d->name + "' is a named argument: callers pass it by name, so renaming it here would break them";
        return edits;
    }
    if (d->attr_kind != AttrDef::Plain) {
        *error = "'" + d->name + "' is bound by `inherit`, which takes its name from the attribute it inherits";
        return edits;
    }
    for (int ui : d->uses) {
        const Use &u = doc.uses[static_cast<size_t>(ui)];
        if (u.inherit_use) {
            int l = 0, c = 0;
            doc.Pos(u.start, &l, &c);
            *error = "'" + d->name + "' is inherited by name at line " + std::to_string(l + 1) +
                     "; renaming it would rename that attribute too";
            return edits;
        }
        if (!u.var) {
            *error = "'" + d->name + "' is used implicitly (by an operator or a <path>)";
            return edits;
        }
        // Would the new name be captured by a closer binding at this use?
        for (Scope *s = u.scope; s && s != d->scope; s = s->up) {
            if (s->kind != Scope::With && s->vars.count(new_name)) {
                *error = "'" + new_name + "' is already bound where '" + d->name + "' is used";
                return edits;
            }
        }
    }
    if (d->scope->vars.count(new_name)) {
        *error = "'" + new_name + "' is already bound here";
        return edits;
    }
    // A name that today comes from a `with` (or is undefined) would start resolving to the renamed binding.
    for (const Use &u : doc.uses) {
        if (!u.var || u.def || u.var->name != new_name) continue;
        for (Scope *s = u.scope; s; s = s->up) {
            if (s == d->scope) {
                *error = "'" + new_name + "' is used inside this scope already (from a `with`)";
                return edits;
            }
        }
    }
    std::set<std::pair<int, int>> seen;
    auto edit = [&](int a, int b) {
        if (!seen.insert({a, b}).second) return;
        edits.push_back({doc.Range(a, b), new_name});
    };
    for (const auto &s : d->decl_sites) edit(s.first, s.second);
    for (int ui : d->uses) edit(doc.uses[static_cast<size_t>(ui)].start, doc.uses[static_cast<size_t>(ui)].end);
    return edits;
}

// ---- outline / folding ------------------------------------------------------------------------------------------------

namespace {

NixLspSymbolKind KindOf(const Node *v) {
    if (!v) return NixLspSymbolKind::Variable;
    switch (v->kind) {
        case NodeKind::Lambda: return NixLspSymbolKind::Function;
        case NodeKind::Attrs: return NixLspSymbolKind::Object;
        case NodeKind::List: return NixLspSymbolKind::Array;
        case NodeKind::String:
        case NodeKind::IndString: return NixLspSymbolKind::String;
        case NodeKind::Int:
        case NodeKind::Float: return NixLspSymbolKind::Number;
        case NodeKind::Path:
        case NodeKind::SPath: return NixLspSymbolKind::File;
        case NodeKind::Var:
            if (v->name == "true" || v->name == "false") return NixLspSymbolKind::Boolean;
            if (v->name == "null") return NixLspSymbolKind::Null;
            return NixLspSymbolKind::Variable;
        default: return NixLspSymbolKind::Variable;
    }
}

class Outliner {
public:
    Outliner(const NixLspDocument &doc, std::vector<NixLspSymbol> *out) : doc_(doc), out_(out) {}

    void Expr(Node *n, int parent, int depth) {
        if (!n || depth > 64 || out_->size() > 5000) return;
        switch (n->kind) {
            case NodeKind::Lambda:
                Expr(n->body, parent, depth + 1);
                return;
            case NodeKind::Let:
                Bindings(n, parent, depth, true);
                Expr(n->body, parent, depth + 1);
                return;
            case NodeKind::Attrs:
                Bindings(n, parent, depth, false);
                return;
            case NodeKind::With:
            case NodeKind::Assert:
                Expr(n->body, parent, depth + 1);
                return;
            case NodeKind::Call:
                // mkDerivation { ... } / callPackage ./x { ... } / mkDerivation (finalAttrs: { ... })
                for (size_t k = 1; k < n->kids.size(); k++) {
                    Node *a = n->kids[k];
                    if (a->kind == NodeKind::Attrs || a->kind == NodeKind::Lambda) Expr(a, parent, depth + 1);
                }
                return;
            case NodeKind::BinOp:
                if (n->text == "//") {
                    Expr(n->kids[0], parent, depth + 1);
                    Expr(n->kids[1], parent, depth + 1);
                }
                return;
            default:
                return;
        }
    }

private:
    const NixLspDocument &doc_;
    std::vector<NixLspSymbol> *out_;

    int Add(const std::string &name, const std::string &detail, NixLspSymbolKind kind, int a, int b, int sa, int sb,
            int parent) {
        NixLspSymbol s;
        s.name = name;
        s.detail = detail;
        s.kind = kind;
        s.range = doc_.Range(a, b);
        s.sel_range = doc_.Range(sa, sb);
        s.parent = parent;
        out_->push_back(s);
        return static_cast<int>(out_->size()) - 1;
    }

    void Bindings(Node *n, int parent, int depth, bool is_let) {
        for (const Binding &b : n->bindings) {
            if (b.kind == Binding::Assign) {
                if (b.path.empty()) continue;
                std::string name;
                for (size_t k = 0; k < b.path.size(); k++) {
                    if (k) name += '.';
                    name += b.path[k].dynamic ? doc_.Slice(b.path[k].start, b.path[k].end) : b.path[k].name;
                }
                if (name.empty()) continue;
                const int me = Add(name, is_let ? "let" : "", is_let ? NixLspSymbolKind::Variable : KindOf(b.value),
                                   b.start, b.end, b.path.front().start, b.path.back().end, parent);
                if (is_let && b.value && b.value->kind == NodeKind::Lambda)
                    (*out_)[static_cast<size_t>(me)].kind = NixLspSymbolKind::Function;
                if (b.value) {
                    Node *v = b.value;
                    while (v && v->kind == NodeKind::Lambda) v = v->body;
                    if (v && (v->kind == NodeKind::Attrs || v->kind == NodeKind::Let || v->kind == NodeKind::Call ||
                              (v->kind == NodeKind::BinOp && v->text == "//")))
                        Expr(v, me, depth + 1);
                }
            } else {
                const std::string detail = b.kind == Binding::Inherit
                                               ? "inherit"
                                               : "inherit (" + OneLine(doc_.Slice(b.from->start, b.from->end), 40) + ")";
                for (const AttrName &a : b.path) {
                    if (a.missing || a.name.empty()) continue;
                    Add(a.name, detail, is_let ? NixLspSymbolKind::Variable : NixLspSymbolKind::Field, a.start, a.end,
                        a.start, a.end, parent);
                }
            }
        }
    }
};

}  // namespace

std::vector<NixLspSymbol> NixLspSymbols(const NixLspDocument &doc) {
    std::vector<NixLspSymbol> out;
    Outliner(doc, &out).Expr(doc.parse->root, -1, 0);
    return out;
}

std::vector<NixLspFold> NixLspFolds(const NixLspDocument &doc) {
    std::vector<NixLspFold> out;
    std::set<std::pair<int, int>> seen;
    auto fold = [&](int a, int b, const char *kind) {
        int la = 0, lb = 0, c = 0;
        doc.Pos(a, &la, &c);
        doc.Pos(b, &lb, &c);
        if (lb - 1 <= la) return;
        if (!seen.insert({la, lb - 1}).second) return;
        out.push_back({la, lb - 1, kind});
    };
    Walk(doc.parse->root, [&](Node *n) {
        switch (n->kind) {
            case NodeKind::Attrs:
                if (n->open >= 0 && n->close >= 0) fold(n->open, n->close, "");
                break;
            case NodeKind::Let:
                if (n->open >= 0 && n->close >= 0) fold(n->open, n->close, "");
                break;
            case NodeKind::List:
                fold(n->start, n->end - 1, "");
                break;
            case NodeKind::IndString:
                fold(n->start, n->end - 1, "");
                break;
            case NodeKind::Lambda:
                if (n->has_formals) fold(n->formals_start, n->formals_end - 1, "");
                break;
            default:
                break;
        }
        return true;
    });
    // Comments: a multi-line block comment, or a run of line comments.
    const auto &cs = doc.parse->comments;
    for (size_t k = 0; k < cs.size();) {
        if (cs[k].block) {
            int la = 0, lb = 0, c = 0;
            doc.Pos(cs[k].start, &la, &c);
            doc.Pos(cs[k].end, &lb, &c);
            if (lb > la && seen.insert({la, lb}).second) out.push_back({la, lb, "comment"});
            k++;
            continue;
        }
        size_t j = k;
        int la = 0, c = 0, prev = 0;
        doc.Pos(cs[k].start, &la, &c);
        prev = la;
        while (j + 1 < cs.size() && !cs[j + 1].block) {
            int l2 = 0;
            doc.Pos(cs[j + 1].start, &l2, &c);
            if (l2 != prev + 1) break;
            prev = l2;
            j++;
        }
        if (prev > la && seen.insert({la, prev}).second) out.push_back({la, prev, "comment"});
        k = j + 1;
    }
    std::sort(out.begin(), out.end(), [](const NixLspFold &a, const NixLspFold &b) {
        return a.start_line != b.start_line ? a.start_line < b.start_line : a.end_line > b.end_line;
    });
    return out;
}

// ---- formatting ----------------------------------------------------------------------------------------------------

std::vector<NixLspTextEdit> NixLspFormat(const std::vector<std::string> &lines, int width, std::string *error) {
    std::vector<NixLspTextEdit> edits;
    const std::string text = JoinLines(lines);
    nixfmt::Options o;
    if (width > 0) o.width = width;
    const nixfmt::Result r = nixfmt::Format(text, o);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    if (r.text == text) return edits;
    // One edit over the lines that changed.
    std::vector<std::string> b;
    {
        size_t i = 0;
        while (true) {
            const size_t j = r.text.find('\n', i);
            if (j == std::string::npos) {
                b.push_back(r.text.substr(i));
                break;
            }
            b.push_back(r.text.substr(i, j - i));
            i = j + 1;
        }
    }
    size_t pre = 0;
    while (pre < lines.size() && pre < b.size() && lines[pre] == b[pre]) pre++;
    size_t suf = 0;
    while (suf < lines.size() - pre && suf < b.size() - pre &&
           lines[lines.size() - 1 - suf] == b[b.size() - 1 - suf])
        suf++;
    NixLspTextEdit e;
    e.range.line = static_cast<int>(pre);
    e.range.col = 0;
    const size_t a_end = lines.size() - suf;  // exclusive
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
