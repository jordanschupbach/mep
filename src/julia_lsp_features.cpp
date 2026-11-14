// mep's Julia language server: hover, completion, signature help,
// navigation, rename, outline, folds and formatting over the analysis in
// julia_lsp.cpp (julia_lsp_internal.h describes the document).
#include <algorithm>
#include <cstring>
#include <set>

#include "julia_format.h"
#include "julia_lsp.h"
#include "julia_lsp_internal.h"
#include "julia_lsp_vocab.h"

using namespace jllsp;

namespace {

const char *const kKeywords[] = {
    "baremodule", "begin", "break", "catch", "const", "continue", "do", "else", "elseif", "end", "export", "false",
    "finally", "for", "function", "global", "if", "import", "let", "local", "macro", "module", "public", "quote",
    "return", "struct", "true", "try", "using", "while", "abstract type", "mutable struct", "primitive type",
    "where", "in", "isa", "outer",
};

bool IsKeyword(std::string_view s) {
    for (const char *k : kKeywords)
        if (s == k) return true;
    return s == "abstract" || s == "mutable" || s == "primitive" || s == "type" || s == "as" || s == "var";
}

bool IdentByte(unsigned char c) { return std::isalnum(c) || c == '_' || c == '!' || c >= 0x80; }

// The occurrence covering byte @p at (or ending right at it).
const Occ *OccAt(const Doc &d, uint32_t at) {
    const Occ *best = nullptr;
    for (const Occ &o : d.occs) {
        if (o.b > at) break;
        if (at < o.e || (at == o.e && !best)) {
            if (at <= o.e) best = &o;
        }
    }
    if (best && (at < best->b || at > best->e)) return nullptr;
    return best;
}

const char *VarKindName(VarKind k) {
    switch (k) {
    case VarKind::Global: return "global";
    case VarKind::Local: return "local variable";
    case VarKind::Argument: return "argument";
    case VarKind::KwArgument: return "keyword argument";
    case VarKind::TypeParam: return "type parameter";
    case VarKind::Function: return "function";
    case VarKind::Macro: return "macro";
    case VarKind::Struct: return "struct";
    case VarKind::AbstractType: return "abstract type";
    case VarKind::PrimitiveType: return "primitive type";
    case VarKind::ModuleName: return "module";
    case VarKind::Const: return "constant";
    case VarKind::Import: return "imported";
    case VarKind::LoopVar: return "loop variable";
    case VarKind::CatchVar: return "exception";
    case VarKind::LetVar: return "let variable";
    }
    return "";
}

JuliaLspKind CompletionKind(VarKind k) {
    switch (k) {
    case VarKind::Function:
    case VarKind::Macro: return JuliaLspKind::Function;
    case VarKind::Struct:
    case VarKind::AbstractType:
    case VarKind::PrimitiveType:
    case VarKind::TypeParam: return JuliaLspKind::Struct;
    case VarKind::ModuleName: return JuliaLspKind::Module;
    case VarKind::Const: return JuliaLspKind::Constant;
    default: return JuliaLspKind::Variable;
    }
}

JuliaLspKind VocabKind(std::string_view kind) {
    if (kind == "function" || kind == "macro") return JuliaLspKind::Function;
    if (kind == "type") return JuliaLspKind::Struct;
    if (kind == "module") return JuliaLspKind::Module;
    return JuliaLspKind::Constant;
}

std::string VocabMarkdown(const JuliaLspApi &a) {
    std::string s = "```julia\n";
    s += *a.signature ? std::string(a.signature) : std::string(a.name);
    s += "\n```\n";
    s += std::string("*") + a.kind + " in `" + a.module + "`*";
    if (*a.doc) {
        s += "\n\n";
        s += a.doc;
    }
    return s;
}

std::string VarMarkdown(const Doc &d, const Var &v) {
    std::string s = "```julia\n";
    std::vector<std::string> sigs;
    for (int def : v.defs) {
        const std::string sig = SignatureText(d, def);
        if (!sig.empty() && std::find(sigs.begin(), sigs.end(), sig) == sigs.end()) sigs.push_back(sig);
    }
    if ((v.kind == VarKind::Function || v.kind == VarKind::Macro || v.kind == VarKind::Struct ||
         v.kind == VarKind::AbstractType || v.kind == VarKind::PrimitiveType) &&
        !sigs.empty()) {
        for (size_t i = 0; i < sigs.size() && i < 8; ++i) s += sigs[i] + "\n";
        if (sigs.size() > 8) s += "# ... " + std::to_string(sigs.size() - 8) + " more methods\n";
    } else {
        s += v.name;
        if (!v.type.empty()) s += "::" + v.type;
        s += "\n";
    }
    s += "```\n*";
    s += VarKindName(v.kind);
    if (v.kind == VarKind::Function && v.defs.size() > 1) s += ", " + std::to_string(v.defs.size()) + " methods";
    s += "*";
    for (int def : v.defs) {
        const std::string doc = DocstringFor(d, def);
        if (!doc.empty()) {
            s += "\n\n" + doc;
            break;
        }
    }
    return s;
}

// The identifier (or @macro) around byte @p at: [b, e).
void WordAround(const Doc &d, uint32_t at, uint32_t *b, uint32_t *e) {
    uint32_t s = at;
    while (s > 0 && IdentByte(static_cast<unsigned char>(d.text[s - 1]))) --s;
    if (s > 0 && d.text[s - 1] == '@') --s;
    uint32_t t = at;
    while (t < d.text.size() && IdentByte(static_cast<unsigned char>(d.text[t]))) ++t;
    *b = s;
    *e = t;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hover

JuliaLspHoverInfo JuliaLspHover(const JuliaLspDocument &doc, int line, int col) {
    const Doc &d = doc.d;
    JuliaLspHoverInfo h;
    const uint32_t at = d.Offset(line, col);
    const Occ *o = OccAt(d, at);
    if (o) {
        h.range = d.Range(o->b, o->e);
        if (o->var >= 0) {
            h.found = true;
            const Var &v = d.vars[static_cast<size_t>(o->var)];
            // `using Package`: the package's own docstring.
            const Package *p = v.kind == VarKind::Import ? PackageNamed(d, v.name) : nullptr;
            h.text = p ? "```julia\nmodule " + p->name + "\n```\n*package at `" + p->file + "`*" +
                             (p->doc.empty() ? std::string() : "\n\n" + p->doc)
                       : VarMarkdown(d, v);
            return h;
        }
        const int mod = d.scopes[static_cast<size_t>(o->scope)].module;
        auto rows = VocabFor(d, mod, o->name);
        if (rows.empty()) rows = VocabAll(d, o->name);
        if (!rows.empty()) {
            h.found = true;
            h.text = VocabMarkdown(*rows.front());
            return h;
        }
        return h;
    }
    // A qualified name (`Base.sum`, `LinearAlgebra.norm`) or a keyword.
    uint32_t b, e;
    WordAround(d, at, &b, &e);
    if (b >= e) return h;
    const std::string word = d.text.substr(b, e - b);
    std::string qual;
    if (b > 0 && d.text[b - 1] == '.') {
        uint32_t q = b - 1;
        while (q > 0 && IdentByte(static_cast<unsigned char>(d.text[q - 1]))) --q;
        qual = d.text.substr(q, b - 1 - q);
    }
    for (const JuliaLspApi *a : VocabAll(d, word)) {
        if (!qual.empty() && qual != a->module) continue;
        h.found = true;
        h.range = d.Range(b, e);
        h.text = VocabMarkdown(*a);
        return h;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Completion

std::vector<JuliaLspCompletionItem> JuliaLspCompletions(const JuliaLspDocument &doc, int line, int col) {
    const Doc &d = doc.d;
    std::vector<JuliaLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= d.lines.size()) return out;
    const std::string &cur = d.lines[static_cast<size_t>(line)];
    col = std::max(0, std::min(col, static_cast<int>(cur.size())));
    int start = col;
    while (start > 0 && IdentByte(static_cast<unsigned char>(cur[static_cast<size_t>(start - 1)]))) start--;
    bool macro = start > 0 && cur[static_cast<size_t>(start - 1)] == '@';
    if (macro) start--;
    const std::string prefix = cur.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    // In a comment or string: nothing.
    const uint32_t at = d.Offset(line, col);
    std::set<std::string> seen;
    auto push = [&](const std::string &label, JuliaLspKind kind, const std::string &detail, const std::string &docs) {
        if (!seen.insert(label).second) return;
        if (label.compare(0, prefix.size(), prefix) != 0) return;
        JuliaLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = docs;
        it.replace_start = start;
        it.replace_end = col;
        out.push_back(std::move(it));
    };
    // `Module.`: that module's names.
    if (!macro && start > 0 && cur[static_cast<size_t>(start - 1)] == '.') {
        int q = start - 1;
        while (q > 0 && IdentByte(static_cast<unsigned char>(cur[static_cast<size_t>(q - 1)]))) q--;
        const std::string mod = cur.substr(static_cast<size_t>(q), static_cast<size_t>(start - 1 - q));
        // A `using`ed package: all its names, exported or not.
        if (const Package *p = PackageNamed(d, mod))
            for (const PackageRow &r : p->rows) push(r.api.name, VocabKind(r.api.kind), r.api.signature, r.api.doc);
        size_t count = 0;
        const JuliaLspApi *api = JuliaLspApiTable(&count);
        for (size_t i = 0; i < count; ++i)
            if (mod == api[i].module) push(api[i].name, VocabKind(api[i].kind), api[i].signature, api[i].doc);
        return out;
    }
    const int scope = ScopeAt(d, at);
    // Variables in scope, innermost first.
    for (int s = scope; s >= 0; s = d.scopes[static_cast<size_t>(s)].parent) {
        for (auto &[name, vi] : d.scopes[static_cast<size_t>(s)].names) {
            const Var &v = d.vars[static_cast<size_t>(vi)];
            if ((name[0] == '@') != macro) continue;
            push(name, CompletionKind(v.kind), VarKindName(v.kind), "");
        }
        if (d.scopes[static_cast<size_t>(s)].kind == ScopeKind::Module) break;
    }
    if (!macro)
        for (const char *k : kKeywords) push(k, JuliaLspKind::Keyword, "keyword", "");
    // Base, Core and `using`ed standard libraries and packages.
    const int mod = d.scopes[static_cast<size_t>(scope)].module;
    for (const auto &p : d.packages) {
        const Scope &m = d.scopes[static_cast<size_t>(mod)];
        if (std::find(m.usings.begin(), m.usings.end(), p->name) == m.usings.end() &&
            std::find(m.unknown_usings.begin(), m.unknown_usings.end(), p->name) == m.unknown_usings.end())
            continue;
        for (const PackageRow &r : p->rows) {
            if (!r.exported || (r.api.name[0] == '@') != macro) continue;
            push(r.api.name, VocabKind(r.api.kind), *r.api.signature ? r.api.signature : r.api.module, r.api.doc);
        }
    }
    size_t count = 0;
    const JuliaLspApi *api = JuliaLspApiTable(&count);
    const std::string_view p = prefix;
    auto lo = std::lower_bound(api, api + count, p,
                               [](const JuliaLspApi &a, std::string_view n) { return std::string_view(a.name) < n; });
    for (; lo != api + count && std::string_view(lo->name).substr(0, p.size()) == p; ++lo) {
        if ((lo->name[0] == '@') != macro) continue;
        if (VocabFor(d, mod, lo->name).empty()) continue;
        push(lo->name, VocabKind(lo->kind), *lo->signature ? lo->signature : lo->module, lo->doc);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Signature help

JuliaLspSignatureHelp JuliaLspSignatureAt(const JuliaLspDocument &doc, int line, int col) {
    const Doc &d = doc.d;
    JuliaLspSignatureHelp sh;
    uint32_t at = d.Offset(line, col);
    // The unmatched `(` before the cursor, skipping strings crudely by bracket count.
    int depth = 0, commas = 0;
    bool semicolon = false;
    uint32_t i = at;
    while (i > 0) {
        const char c = d.text[i - 1];
        if (c == ')' || c == ']' || c == '}') depth++;
        else if (c == '[' || c == '{') {
            if (depth == 0) return sh;
            depth--;
        } else if (c == '(') {
            if (depth == 0) break;
            depth--;
        } else if (c == ',' && depth == 0) {
            commas++;
        } else if (c == ';' && depth == 0) {
            semicolon = true;
        } else if (c == '\n' && i + 2000 < at) {
            return sh;
        }
        --i;
    }
    if (i == 0) return sh;
    uint32_t e = i - 1, b = e;
    while (b > 0 && IdentByte(static_cast<unsigned char>(d.text[b - 1]))) --b;
    if (b > 0 && d.text[b - 1] == '@') --b;
    if (b >= e) return sh;
    const std::string name = d.text.substr(b, e - b);
    std::vector<std::string> sigs;
    std::string docs;
    const int scope = ScopeAt(d, b);
    const int v = Lookup(d, scope, name);
    if (v >= 0) {
        for (int def : d.vars[static_cast<size_t>(v)].defs) {
            std::string s = SignatureText(d, def);
            if (!s.empty()) sigs.push_back(s);
            if (docs.empty()) docs = DocstringFor(d, def);
        }
    }
    if (sigs.empty()) {
        for (const JuliaLspApi *a : VocabAll(d, name)) {
            std::string all = a->signature;
            size_t p = 0;
            while (p <= all.size()) {
                size_t nl = all.find('\n', p);
                if (nl == std::string::npos) nl = all.size();
                std::string s = all.substr(p, nl - p);
                if (s.compare(0, name.size(), name) == 0 && s.size() > name.size() && s[name.size()] == '(')
                    sigs.push_back(s);
                p = nl + 1;
            }
            if (docs.empty()) docs = a->doc;
        }
    }
    if (sigs.empty()) return sh;
    // The first signature with enough positional parameters.
    auto params_of = [](const std::string &s) {
        std::vector<std::string> ps;
        const size_t open = s.find('(');
        if (open == std::string::npos) return ps;
        int dep = 0;
        size_t from = open + 1;
        for (size_t k = open + 1; k < s.size(); ++k) {
            const char c = s[k];
            if (c == '(' || c == '[' || c == '{') dep++;
            else if ((c == ')' || c == ']' || c == '}') && dep > 0) dep--;
            else if ((c == ',' || c == ';' || c == ')') && dep == 0) {
                std::string p = s.substr(from, k - from);
                while (!p.empty() && p.front() == ' ') p.erase(0, 1);
                while (!p.empty() && p.back() == ' ') p.pop_back();
                if (!p.empty()) ps.push_back(p);
                from = k + 1;
                if (c == ')') break;
            }
        }
        return ps;
    };
    std::string chosen = sigs.front();
    for (const std::string &s : sigs)
        if (static_cast<int>(params_of(s).size()) > commas) {
            chosen = s;
            break;
        }
    sh.found = true;
    sh.label = chosen;
    sh.params = params_of(chosen);
    sh.documentation = docs.size() > 1500 ? docs.substr(0, 1500) + "\n..." : docs;
    sh.active_param = semicolon ? static_cast<int>(sh.params.size()) - 1 : commas;
    if (sh.active_param >= static_cast<int>(sh.params.size()))
        sh.active_param = std::max(0, static_cast<int>(sh.params.size()) - 1);
    return sh;
}

// ---------------------------------------------------------------------------
// Navigation

std::vector<JuliaLspLocation> JuliaLspDefinition(const JuliaLspDocument &doc, int line, int col) {
    const Doc &d = doc.d;
    std::vector<JuliaLspLocation> out;
    const Occ *o = OccAt(d, d.Offset(line, col));
    if (!o) return out;
    if (o->var < 0 || d.vars[static_cast<size_t>(o->var)].kind == VarKind::Import) {
        // A name from a `using`ed package: where the package defines it.
        const std::string name = o->var < 0 ? o->name : d.vars[static_cast<size_t>(o->var)].name;
        if (const Package *p = PackageNamed(d, name)) {
            JuliaLspLocation l;
            l.path = p->file;
            out.push_back(l);
            return out;
        }
        for (const auto &p : d.packages)
            for (const PackageRow *r : PackageRows(*p, name)) {
                if (!r->has_location) continue;
                JuliaLspLocation l;
                l.path = p->file;
                l.range = {r->line, r->col, r->line, r->col};
                out.push_back(l);
                return out;
            }
        if (o->var < 0) return out;
    }
    const Var &v = d.vars[static_cast<size_t>(o->var)];
    std::set<uint32_t> seen;
    for (const Occ &x : d.occs) {
        if (x.var != o->var || !(x.def || x.write)) continue;
        // Functions and types: every method; variables: the first assignment.
        if (seen.insert(x.b).second) {
            JuliaLspLocation l;
            l.range = d.Range(x.b, x.e);
            out.push_back(l);
        }
        if (v.kind != VarKind::Function && v.kind != VarKind::Macro) break;
    }
    if (out.empty()) {
        JuliaLspLocation l;
        l.range = d.Range(v.b, v.e);
        out.push_back(l);
    }
    return out;
}

std::vector<JuliaLspReference> JuliaLspReferences(const JuliaLspDocument &doc, int line, int col) {
    const Doc &d = doc.d;
    std::vector<JuliaLspReference> out;
    const Occ *o = OccAt(d, d.Offset(line, col));
    if (!o) return out;
    for (const Occ &x : d.occs) {
        const bool same = o->var >= 0 ? x.var == o->var
                                      : (x.var < 0 && x.name == o->name &&
                                         d.scopes[static_cast<size_t>(x.scope)].module ==
                                             d.scopes[static_cast<size_t>(o->scope)].module);
        if (!same) continue;
        JuliaLspReference r;
        r.range = d.Range(x.b, x.e);
        r.is_definition = x.def || x.write;
        out.push_back(r);
    }
    return out;
}

JuliaLspRenameTarget JuliaLspPrepareRename(const JuliaLspDocument &doc, int line, int col) {
    const Doc &d = doc.d;
    JuliaLspRenameTarget t;
    const Occ *o = OccAt(d, d.Offset(line, col));
    if (!o || o->var < 0) return t;
    const Var &v = d.vars[static_cast<size_t>(o->var)];
    if (v.kind == VarKind::Import || v.name[0] == '@') return t;
    if (d.text.compare(o->b, 4, "var\"") == 0) return t;  // var"..." names: renamed by hand
    t.found = true;
    t.range = d.Range(o->b, o->e);
    t.placeholder = o->name;
    return t;
}

std::vector<JuliaLspTextEdit> JuliaLspRename(const JuliaLspDocument &doc, int line, int col,
                                             const std::string &new_name, std::string *error) {
    const Doc &d = doc.d;
    std::vector<JuliaLspTextEdit> out;
    const JuliaLspRenameTarget t = JuliaLspPrepareRename(doc, line, col);
    if (!t.found) {
        *error = "nothing to rename here (only names this file defines; not imports or macros)";
        return out;
    }
    bool ok = !new_name.empty() && !std::isdigit(static_cast<unsigned char>(new_name[0])) && new_name[0] != '!';
    for (char c : new_name) ok = ok && IdentByte(static_cast<unsigned char>(c));
    if (!ok || IsKeyword(new_name)) {
        *error = "`" + new_name + "` is not a Julia identifier";
        return out;
    }
    const Occ *o = OccAt(d, d.Offset(line, col));
    const Var &v = d.vars[static_cast<size_t>(o->var)];
    if (v.kind == VarKind::KwArgument) {
        *error = "`" + v.name + "` is a keyword argument: callers name it, so renaming it here would break them";
        return out;
    }
    // Refuse a rename that another variable in a scope of a use would capture.
    for (const Occ &x : d.occs) {
        if (x.var != o->var) continue;
        const int other = Lookup(d, x.scope, new_name);
        if (other >= 0 && other != o->var) {
            *error = "`" + new_name + "` is already defined where `" + v.name + "` is used";
            return {};
        }
        JuliaLspTextEdit e;
        e.range = d.Range(x.b, x.e);
        e.new_text = new_name;
        out.push_back(e);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Outline

namespace {

struct SymbolWalker {
    const Doc &d;
    std::vector<JuliaLspSymbol> &out;

    int Add(const std::string &name, const std::string &detail, JuliaLspSymbolKind kind, int node, int name_node,
            int parent) {
        JuliaLspSymbol s;
        s.name = name;
        s.detail = detail;
        s.kind = kind;
        s.range = d.Range(d.N(node).b, d.N(node).e);
        s.sel_range = name_node >= 0 ? d.Range(d.N(name_node).b, d.N(name_node).e) : s.range;
        s.parent = parent;
        out.push_back(std::move(s));
        return static_cast<int>(out.size()) - 1;
    }

    static int SigName(const Doc &d, int sig) {
        while (sig >= 0) {
            const Node &n = d.N(sig);
            if ((n.k == K::Where || n.k == K::DColon) && d.NKids(sig) >= 1) {
                sig = d.Kid(sig, 0);
                continue;
            }
            if (n.k == K::Call && d.NKids(sig) >= 1) return d.Kid(sig, 0);
            if (n.k == K::Ident || n.k == K::Var) return sig;
            return -1;
        }
        return -1;
    }

    void Walk(int node, int parent, bool top) {
        const Node &n = d.N(node);
        switch (n.k) {
        case K::Toplevel:
        case K::Block:
        case K::Doc:
            for (uint32_t k = 0; k < d.NKids(node); ++k) Walk(d.Kid(node, k), parent, top);
            return;
        case K::MacroCall:
            // @inline function ..., @kwdef struct ..., Base.@propagate_inbounds f(x) = ...
            for (uint32_t k = 1; k < d.NKids(node); ++k) {
                const K ck = d.N(d.Kid(node, k)).k;
                if (ck == K::Function || ck == K::Struct || ck == K::Assign || ck == K::Const || ck == K::Macro)
                    Walk(d.Kid(node, k), parent, top);
            }
            return;
        case K::Module: {
            int name = -1, body = -1;
            for (uint32_t k = 0; k < d.NKids(node); ++k) {
                const int c = d.Kid(node, k);
                if ((d.N(c).k == K::Ident) && name < 0) name = c;
                if (d.N(c).k == K::Block) body = c;
            }
            const int me = Add(name >= 0 ? std::string(d.Text(name)) : "module", "module",
                               JuliaLspSymbolKind::Module, node, name, parent);
            if (body >= 0) Walk(body, me, true);
            return;
        }
        case K::Function:
        case K::Macro: {
            if (d.NKids(node) == 0) return;
            const int sig = d.Kid(node, 0);
            const int name = SigName(d, sig);
            if (name < 0) return;  // anonymous
            std::string nm(d.Text(name));
            if (n.k == K::Macro) nm = "@" + nm;
            Add(nm, SignatureText(d, node), n.k == K::Macro ? JuliaLspSymbolKind::Function
                                                           : JuliaLspSymbolKind::Function,
                node, name, parent);
            return;
        }
        case K::Assign: {
            if (d.NKids(node) != 2) return;
            const int lhs = d.Kid(node, 0);
            const K lk = d.N(lhs).k;
            if (lk == K::Call || lk == K::Where || (lk == K::DColon && d.NKids(lhs) == 2 &&
                                                     d.N(d.Kid(lhs, 0)).k == K::Call)) {
                const int name = SigName(d, lhs);
                if (name >= 0) Add(std::string(d.Text(name)), SignatureText(d, node), JuliaLspSymbolKind::Function,
                                   node, name, parent);
                return;
            }
            if (!top) return;
            int nm = lhs;
            if (d.N(nm).k == K::DColon && d.NKids(nm) == 2) nm = d.Kid(nm, 0);
            if (d.N(nm).k == K::Ident) Add(std::string(d.Text(nm)), "", JuliaLspSymbolKind::Variable, node, nm, parent);
            return;
        }
        case K::Const:
            for (uint32_t k = 0; k < d.NKids(node); ++k) {
                const int c = d.Kid(node, k);
                if (d.N(c).k == K::Assign && d.NKids(c) == 2) {
                    int nm = d.Kid(c, 0);
                    if (d.N(nm).k == K::DColon && d.NKids(nm) == 2) nm = d.Kid(nm, 0);
                    if (d.N(nm).k == K::Ident)
                        Add(std::string(d.Text(nm)), "const", JuliaLspSymbolKind::Constant, node, nm, parent);
                }
            }
            return;
        case K::Struct:
        case K::Abstract:
        case K::Primitive: {
            if (d.NKids(node) == 0) return;
            int head = d.Kid(node, 0);
            if (d.N(head).k == K::Subtype && d.NKids(head) == 2) head = d.Kid(head, 0);
            if (d.N(head).k == K::Curly && d.NKids(head) >= 1) head = d.Kid(head, 0);
            if (d.N(head).k != K::Ident) return;
            const int me = Add(std::string(d.Text(head)), SignatureText(d, node),
                               n.k == K::Struct ? JuliaLspSymbolKind::Class : JuliaLspSymbolKind::Class, node, head,
                               parent);
            if (n.k == K::Struct && d.NKids(node) >= 2) {
                const int body = d.Kid(node, 1);
                for (uint32_t k = 0; k < d.NKids(body); ++k) {
                    int c = d.Kid(body, k);
                    int f = c;
                    if (d.N(f).k == K::Assign && d.NKids(f) == 2 && d.N(d.Kid(f, 0)).k != K::Call) f = d.Kid(f, 0);
                    if (d.N(f).k == K::DColon && d.NKids(f) == 2) f = d.Kid(f, 0);
                    if (d.N(f).k == K::Ident) {
                        Add(std::string(d.Text(f)), "", JuliaLspSymbolKind::Field, c, f, me);
                    } else {
                        Walk(c, me, false);
                    }
                }
            }
            return;
        }
        default:
            return;
        }
    }
};

}  // namespace

std::vector<JuliaLspSymbol> JuliaLspSymbols(const JuliaLspDocument &doc) {
    const Doc &d = doc.d;
    std::vector<JuliaLspSymbol> out;
    if (d.root < 0) return out;
    SymbolWalker w{d, out};
    w.Walk(d.root, -1, true);
    return out;
}

// ---------------------------------------------------------------------------
// Folds

std::vector<JuliaLspFold> JuliaLspFolds(const JuliaLspDocument &doc) {
    const Doc &d = doc.d;
    std::vector<JuliaLspFold> out;
    std::set<std::pair<int, int>> seen;
    auto fold = [&](uint32_t b, uint32_t e, bool keep_last, const char *kind) {
        int l0, c0, l1, c1;
        d.Pos(b, &l0, &c0);
        d.Pos(e, &l1, &c1);
        if (keep_last) l1--;
        if (l1 <= l0) return;
        if (!seen.insert({l0, l1}).second) return;
        out.push_back({l0, l1, kind});
    };
    for (const Node &n : d.nodes) {
        switch (n.k) {
        case K::Function:
        case K::Macro:
        case K::Struct:
        case K::Module:
        case K::If:
        case K::For:
        case K::While:
        case K::Let:
        case K::Try:
        case K::Do:
        case K::Quote:
            fold(n.b, n.e, true, "");
            break;
        case K::Block:
            // begin ... end on its own (not a body: those fold with their construct).
            if (n.parent >= 0 && (d.N(n.parent).k == K::Toplevel || d.N(n.parent).k == K::Block) &&
                d.text.compare(n.b, 5, "begin") == 0)
                fold(n.b, n.e, true, "");
            break;
        case K::Call:
        case K::Vect:
        case K::Tuple:
        case K::MacroCall:
        case K::Comprehension:
        case K::String:
        case K::Other:
            if (n.k == K::Other && !(n.e > n.b && (d.text[n.e - 1] == ']' || d.text[n.e - 1] == '}'))) break;
            fold(n.b, n.e, true, "");
            break;
        default:
            break;
        }
    }
    // Comment runs: consecutive `#` lines, and #= ... =# blocks.
    int run_start = -1;
    for (size_t i = 0; i <= d.lines.size(); ++i) {
        bool comment = false;
        if (i < d.lines.size()) {
            const std::string &l = d.lines[i];
            const size_t ns = l.find_first_not_of(" \t");
            comment = ns != std::string::npos && l[ns] == '#' && l.compare(ns, 2, "#=") != 0;
        }
        if (comment && run_start < 0) run_start = static_cast<int>(i);
        if (!comment && run_start >= 0) {
            if (static_cast<int>(i) - 1 > run_start) out.push_back({run_start, static_cast<int>(i) - 1, "comment"});
            run_start = -1;
        }
    }
    for (size_t i = 0; i + 1 < d.text.size(); ++i) {
        if (d.text[i] == '#' && d.text[i + 1] == '=') {
            int depth = 0;
            size_t j = i;
            for (; j + 1 < d.text.size(); ++j) {
                if (d.text[j] == '#' && d.text[j + 1] == '=') {
                    depth++;
                    ++j;
                } else if (d.text[j] == '=' && d.text[j + 1] == '#') {
                    depth--;
                    ++j;
                    if (depth == 0) break;
                }
            }
            // Only real comments: a token, not inside a string.
            const int node = NodeAt(d, static_cast<uint32_t>(i));
            if (node < 0 || d.N(node).k != K::StrLit) fold(static_cast<uint32_t>(i), static_cast<uint32_t>(j), false,
                                                            "comment");
            i = j;
        }
    }
    std::sort(out.begin(), out.end(), [](const JuliaLspFold &a, const JuliaLspFold &b) {
        return a.start_line != b.start_line ? a.start_line < b.start_line : a.end_line > b.end_line;
    });
    return out;
}

// ---------------------------------------------------------------------------
// Formatting

std::vector<JuliaLspTextEdit> JuliaLspFormat(const std::vector<std::string> &lines, int width, std::string *error) {
    (void)width;  // Runic has no options
    std::vector<JuliaLspTextEdit> edits;
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    const juliafmt::Result r = juliafmt::Format(text);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    if (r.text == text) return edits;
    // One edit over the lines that changed.
    std::vector<std::string> next;
    size_t at = 0;
    while (true) {
        size_t nl = r.text.find('\n', at);
        if (nl == std::string::npos) {
            next.push_back(r.text.substr(at));
            break;
        }
        next.push_back(r.text.substr(at, nl - at));
        at = nl + 1;
    }
    size_t pre = 0;
    while (pre < lines.size() && pre < next.size() && lines[pre] == next[pre]) pre++;
    size_t suf = 0;
    while (suf < lines.size() - pre && suf < next.size() - pre &&
           lines[lines.size() - 1 - suf] == next[next.size() - 1 - suf])
        suf++;
    JuliaLspTextEdit e;
    e.range.line = static_cast<int>(pre);
    e.range.col = 0;
    std::string body;
    for (size_t i = pre; i < next.size() - suf; ++i) body += next[i] + "\n";
    if (lines.size() - suf < lines.size()) {
        e.range.end_line = static_cast<int>(lines.size() - suf);
        e.range.end_col = 0;
    } else {
        // Through the end of the document.
        e.range.end_line = static_cast<int>(lines.size()) - 1;
        e.range.end_col = static_cast<int>(lines.back().size());
        if (!body.empty()) body.pop_back();
    }
    e.new_text = body;
    edits.push_back(e);
    return edits;
}
