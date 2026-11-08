// The analysis half of mep's own Fortran language server: diagnostics,
// completion, hover, signature help, definition/references/rename,
// outline, folds and formatting over fortran_parse.h's reader. See
// fortran_lsp.h.
#include "fortran_lsp.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "fortran_format.h"
#include "fortran_lsp_vocab.h"
#include "fortran_parse.h"

namespace fp = fortran;

namespace {

enum class SymKind { Variable, Parameter, Dummy, Result, Function, Subroutine, Module, Type, Interface, Namelist,
                     Common, Use, Associate, Component, Binding, Program, Entry };

struct Symbol {
    std::string name;      // lower
    std::string spelling;  // as declared
    SymKind kind = SymKind::Variable;
    int line = 0, col = 0, end_col = 0;  // declaration
    int scope = -1;                       // scope it belongs to
    int unit = -1;                        // for procedures/modules/types: the scope they open (or -1)
    std::string type;                     // "real(dp)", "type(foo)"
    uint32_t attrs = 0;
    bool array = false;
    std::vector<std::string> dummies;  // procedures: argument names
    std::string from;                  // Use: module name
    std::string detail;
    bool declared = false;  // has a type declaration (vs implicit/attribute-only)
    bool in_common = false;
    bool in_namelist = false;
    bool equivalenced = false;
};

struct Scope {
    int construct = -1;  // fortran::Construct index
    int parent = -1;     // host scope (-1 for none: file level / interface bodies)
    fp::StmtKind kind = fp::StmtKind::Unknown;
    std::string name;
    std::map<std::string, int> names;  // -> symbol index
    std::vector<std::string> uses;     // modules USEd (all, lower)
    bool opaque_use = false;           // USE of a module not in this file without ONLY
    bool implicit_none = false;
    bool has_implicit = false;  // an IMPLICIT statement other than NONE
    std::set<std::string> labels;
    int first_line = 0, last_line = 0;
    bool is_interface_body = false;
};

}  // namespace

class FortranLspDocument {
public:
    std::vector<std::string> lines;
    std::string path;
    std::shared_ptr<fp::Source> src;
    std::vector<Symbol> syms;
    std::vector<Scope> scopes;          // [0] = file / global
    std::vector<int> scope_of_stmt;     // statement -> scope
    // every reference resolved: (stmt, ref index) -> symbol (or -1)
    struct Ref {
        int line = 0, col = 0, end_col = 0;
        int sym = -1;
        int stmt = -1;
        fp::RefCtx ctx = fp::RefCtx::Read;
        std::string name;
    };
    std::vector<Ref> refs;
    std::vector<FortranLspDiagnostic> diags;
    std::map<std::string, int> modules;  // module name -> scope
    std::map<std::string, int> types;    // derived type name -> symbol (any scope; first wins)
    std::map<int, std::vector<int>> components;  // type symbol -> component symbols
};

namespace {

std::string Lower(std::string s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool IsNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool IsUnit(fp::StmtKind k) {
    using K = fp::StmtKind;
    return k == K::Program || k == K::Module || k == K::Submodule || k == K::Subroutine || k == K::Function ||
           k == K::BlockData || k == K::ModuleProcedureUnit;
}

const char *KindWord(SymKind k) {
    switch (k) {
        case SymKind::Variable: return "variable";
        case SymKind::Parameter: return "parameter";
        case SymKind::Dummy: return "dummy argument";
        case SymKind::Result: return "function result";
        case SymKind::Function: return "function";
        case SymKind::Subroutine: return "subroutine";
        case SymKind::Module: return "module";
        case SymKind::Type: return "derived type";
        case SymKind::Interface: return "generic interface";
        case SymKind::Namelist: return "namelist group";
        case SymKind::Common: return "common block";
        case SymKind::Use: return "use-associated name";
        case SymKind::Associate: return "associate name";
        case SymKind::Component: return "component";
        case SymKind::Binding: return "type-bound procedure";
        case SymKind::Program: return "program";
        case SymKind::Entry: return "entry";
    }
    return "";
}

// ---- building the model ------------------------------------------------------------------

class Builder {
public:
    explicit Builder(FortranLspDocument *d) : d_(d) {}
    void Run();

private:
    int AddSym(int scope, const std::string &name, const std::string &spelling, SymKind kind, fp::Pos pos,
               fp::Pos end) {
        Scope &sc = d_->scopes[static_cast<size_t>(scope)];
        auto it = sc.names.find(name);
        if (it != sc.names.end()) return it->second;
        Symbol s;
        s.name = name;
        s.spelling = spelling.empty() ? name : spelling;
        s.kind = kind;
        s.line = pos.line;
        s.col = pos.col;
        s.end_col = end.line == pos.line ? end.col : pos.col + static_cast<int>(name.size());
        s.scope = scope;
        int id = static_cast<int>(d_->syms.size());
        d_->syms.push_back(s);
        sc.names[name] = id;
        return id;
    }
    int ScopeOfConstruct(int c) const {
        auto it = scope_of_construct_.find(c);
        return it == scope_of_construct_.end() ? -1 : it->second;
    }
    void Declare(int si, int scope);
    void Resolve();
    int Lookup(int scope, const std::string &name, int depth = 0) const;
    void Lints();

    FortranLspDocument *d_;
    std::map<int, int> scope_of_construct_;
};

void Builder::Run() {
    fp::Source &src = *d_->src;
    Scope file;
    file.construct = 0;
    file.first_line = 0;
    file.last_line = static_cast<int>(d_->lines.size());
    d_->scopes.push_back(file);
    scope_of_construct_[0] = 0;
    // scopes: program units, interface bodies, derived types (for components), BLOCK constructs
    for (size_t ci = 1; ci < src.constructs.size(); ci++) {
        const fp::Construct &c = src.constructs[ci];
        if (!IsUnit(c.kind) && c.kind != fp::StmtKind::Block) continue;
        Scope sc;
        sc.construct = static_cast<int>(ci);
        sc.kind = c.kind;
        // host: the nearest enclosing construct that has a scope
        int p = c.parent;
        bool in_interface = false;
        while (p > 0 && ScopeOfConstruct(p) < 0) {
            if (src.constructs[static_cast<size_t>(p)].kind == fp::StmtKind::Interface) in_interface = true;
            p = src.constructs[static_cast<size_t>(p)].parent;
        }
        sc.parent = in_interface ? -1 : (p >= 0 ? ScopeOfConstruct(p) : 0);
        sc.is_interface_body = in_interface;
        // a separate module procedure takes its arguments from an interface elsewhere
        if (c.kind == fp::StmtKind::ModuleProcedureUnit) sc.opaque_use = true;
        {
            const fp::Stmt &os = src.stmts[static_cast<size_t>(c.open)];
            if (os.unit.prefix.find("module") != std::string::npos) sc.opaque_use = true;
        }
        const fp::Stmt &open = src.stmts[static_cast<size_t>(c.open)];
        sc.name = open.unit.name;
        sc.first_line = open.pos.line;
        sc.last_line = c.close >= 0 ? src.stmts[static_cast<size_t>(c.close)].end.line
                                    : static_cast<int>(d_->lines.size());
        int id = static_cast<int>(d_->scopes.size());
        d_->scopes.push_back(sc);
        scope_of_construct_[static_cast<int>(ci)] = id;
        if (c.kind == fp::StmtKind::Module || c.kind == fp::StmtKind::Submodule) d_->modules[sc.name] = id;
    }
    d_->scope_of_stmt.assign(src.stmts.size(), 0);
    for (size_t si = 0; si < src.stmts.size(); si++) {
        int c = src.construct_of.size() > si ? src.construct_of[si] : 0;
        while (c > 0 && ScopeOfConstruct(c) < 0) c = src.constructs[static_cast<size_t>(c)].parent;
        d_->scope_of_stmt[si] = c >= 0 ? ScopeOfConstruct(c) : 0;
    }
    for (size_t si = 0; si < src.stmts.size(); si++) Declare(static_cast<int>(si), d_->scope_of_stmt[si]);
    // implicit none / opaque use are inherited from hosts
    for (size_t i = 1; i < d_->scopes.size(); i++) {
        Scope &sc = d_->scopes[i];
        if (sc.parent > 0) {
            const Scope &host = d_->scopes[static_cast<size_t>(sc.parent)];
            if (!sc.has_implicit && host.implicit_none) sc.implicit_none = true;
            if (host.opaque_use) sc.opaque_use = true;
        }
    }
    Resolve();
    Lints();
}

void Builder::Declare(int si, int scope) {
    fp::Source &src = *d_->src;
    const fp::Stmt &st = src.stmts[static_cast<size_t>(si)];
    Scope &sc = d_->scopes[static_cast<size_t>(scope)];
    using K = fp::StmtKind;
    if (!st.label.empty()) {
        std::string l = st.label;
        size_t z = 0;
        while (z + 1 < l.size() && l[z] == '0') z++;
        sc.labels.insert(l.substr(z));
    }
    int cons = src.construct_of.size() > static_cast<size_t>(si) ? src.construct_of[static_cast<size_t>(si)] : 0;
    const fp::Construct *in = cons > 0 ? &src.constructs[static_cast<size_t>(cons)] : nullptr;
    // the unit statement itself: declared in the host, dummies in its own scope
    if (IsUnit(st.kind) && in && in->open == si) {
        int own = scope;
        int host = sc.parent >= 0 ? sc.parent : 0;
        if (sc.is_interface_body) {
            // declared in the scope holding the interface block
            int c = in->parent;
            while (c > 0 && ScopeOfConstruct(c) < 0) c = src.constructs[static_cast<size_t>(c)].parent;
            host = c >= 0 ? ScopeOfConstruct(c) : 0;
        }
        SymKind k = st.kind == K::Function ? SymKind::Function
                    : st.kind == K::Subroutine ? SymKind::Subroutine
                    : st.kind == K::Program ? SymKind::Program
                    : st.kind == K::ModuleProcedureUnit ? SymKind::Function
                                                        : SymKind::Module;
        if (!st.unit.name.empty()) {
            int s = AddSym(host, st.unit.name, st.unit.spelling, k, st.unit.name_pos, st.unit.name_end);
            Symbol &sym = d_->syms[static_cast<size_t>(s)];
            sym.unit = own;
            sym.type = st.unit.type;
            for (const fp::Entity &e : st.unit.dummies) sym.dummies.push_back(e.spelling);
            if (st.kind == K::ModuleProcedureUnit) sym.detail = "module procedure";
        }
        for (const fp::Entity &e : st.unit.dummies) {
            if (e.name == "*") continue;
            AddSym(own, e.name, e.spelling, SymKind::Dummy, e.pos, e.end);
        }
        if (st.kind == K::Function) {
            const std::string &r = st.unit.result.empty() ? st.unit.name : st.unit.result;
            fp::Pos p = st.unit.name_pos, e = st.unit.name_end;
            for (const fp::Entity &en : st.entities) {
                if (en.name == r) {
                    p = en.pos;
                    e = en.end;
                }
            }
            int s = AddSym(own, r, r == st.unit.name ? st.unit.spelling : r, SymKind::Result, p, e);
            d_->syms[static_cast<size_t>(s)].type = st.unit.type;
            if (!st.unit.type.empty()) d_->syms[static_cast<size_t>(s)].declared = true;
        }
        return;
    }
    switch (st.kind) {
        case K::Use: {
            sc.uses.push_back(st.group);
            bool known = d_->modules.count(st.group) > 0;
            bool intrinsic = st.group == "iso_c_binding" || st.group == "iso_fortran_env" ||
                             st.group == "ieee_arithmetic" || st.group == "ieee_exceptions" ||
                             st.group == "ieee_features" || st.group == "omp_lib" || st.group == "omp_lib_kinds" ||
                             st.group == "openacc" || st.group == "mpi" || st.group == "mpi_f08";
            if (!st.use_only && !known) {
                if (intrinsic && (st.group.compare(0, 3, "iso") == 0 || st.group.compare(0, 4, "ieee") == 0)) {
                    // names come from the vocabulary
                } else {
                    sc.opaque_use = true;
                }
            }
            for (const fp::Entity &e : st.entities) {
                int s = AddSym(scope, e.name, e.spelling, SymKind::Use, e.pos, e.end);
                d_->syms[static_cast<size_t>(s)].from = st.group + (e.rename.empty() ? "" : ":" + e.rename);
            }
            return;
        }
        case K::Implicit:
            if (st.implicit_none) sc.implicit_none = true;
            else sc.has_implicit = true;
            return;
        case K::TypeDecl:
        case K::Procedure: {
            bool in_type = in && in->kind == K::DerivedType;
            for (const fp::Entity &e : st.entities) {
                if (in_type) {
                    // component of the type opened by in->open
                    const fp::Stmt &ts = src.stmts[static_cast<size_t>(in->open)];
                    auto ti = sc.names.find(ts.unit.name);
                    if (ti == sc.names.end()) continue;
                    Symbol c;
                    c.name = e.name;
                    c.spelling = e.spelling;
                    c.kind = st.kind == K::Procedure ? SymKind::Binding : SymKind::Component;
                    c.line = e.pos.line;
                    c.col = e.pos.col;
                    c.end_col = e.end.col;
                    c.scope = -1;
                    c.type = st.type;
                    c.attrs = st.attrs;
                    c.array = e.array;
                    c.declared = true;
                    d_->components[ti->second].push_back(static_cast<int>(d_->syms.size()));
                    d_->syms.push_back(c);
                    continue;
                }
                SymKind k = (st.attrs & fp::kAttrParameter) ? SymKind::Parameter : SymKind::Variable;
                if (st.kind == K::Procedure || st.type.compare(0, 9, "procedure") == 0) k = SymKind::Function;
                int s = AddSym(scope, e.name, e.spelling, k, e.pos, e.end);
                Symbol &sym = d_->syms[static_cast<size_t>(s)];
                if (sym.kind == SymKind::Variable && k == SymKind::Parameter) sym.kind = k;
                sym.type = st.type;
                sym.attrs |= st.attrs;
                sym.array = sym.array || e.array || (st.attrs & fp::kAttrDimension);
                sym.declared = true;
                if (st.attrs & fp::kAttrExternal && sym.kind == SymKind::Variable) sym.kind = SymKind::Function;
            }
            return;
        }
        case K::Attribute:
        case K::Common:
            for (const fp::Entity &e : st.entities) {
                int s = AddSym(scope, e.name, e.spelling, SymKind::Variable, e.pos, e.end);
                Symbol &sym = d_->syms[static_cast<size_t>(s)];
                if (st.kind == K::Common) sym.in_common = true;
                if (e.array) sym.array = true;
                std::string w = st.toks.empty() ? "" : st.toks[st.first].lower;
                if (w == "external") {
                    if (sym.kind == SymKind::Variable) sym.kind = SymKind::Function;
                    sym.attrs |= fp::kAttrExternal;
                }
                if (w == "intrinsic") sym.attrs |= fp::kAttrIntrinsic;
                if (w == "parameter") sym.kind = SymKind::Parameter;
            }
            if (st.kind == K::Common && !st.group.empty()) {
                // the block name is not a local name: no symbol
            }
            return;
        case K::Parameter:
            for (const fp::Entity &e : st.entities) {
                int s = AddSym(scope, e.name, e.spelling, SymKind::Parameter, e.pos, e.end);
                d_->syms[static_cast<size_t>(s)].kind = SymKind::Parameter;
            }
            return;
        case K::Enumerator:
            for (const fp::Entity &e : st.entities) {
                int s = AddSym(scope, e.name, e.spelling, SymKind::Parameter, e.pos, e.end);
                d_->syms[static_cast<size_t>(s)].type = "integer (enumerator)";
                d_->syms[static_cast<size_t>(s)].declared = true;
            }
            return;
        case K::Namelist:
            if (!st.group.empty() && !st.toks.empty()) {
                for (size_t i = st.first; i + 1 < st.toks.size(); i++) {
                    if (st.toks[i].text == "/" && st.toks[i + 1].kind == fp::TokKind::Name) {
                        AddSym(scope, st.toks[i + 1].lower, st.toks[i + 1].text, SymKind::Namelist,
                               st.toks[i + 1].pos, st.toks[i + 1].end);
                        i += 2;
                    }
                }
            }
            return;
        case K::DerivedType: {
            int s = AddSym(scope, st.unit.name, st.unit.spelling, SymKind::Type, st.unit.name_pos, st.unit.name_end);
            if (!d_->types.count(st.unit.name)) d_->types[st.unit.name] = s;
            return;
        }
        case K::Interface:
            if (!st.unit.name.empty() && !st.generic_spec) {
                AddSym(scope, st.unit.name, st.unit.spelling, SymKind::Interface, st.unit.name_pos,
                       st.unit.name_end);
            }
            return;
        case K::Generic:
            if (in && in->kind == K::DerivedType && !st.unit.name.empty()) {
                const fp::Stmt &ts = src.stmts[static_cast<size_t>(in->open)];
                auto ti = sc.names.find(ts.unit.name);
                if (ti != sc.names.end()) {
                    Symbol c;
                    c.name = st.unit.name;
                    c.spelling = st.unit.name;
                    c.kind = SymKind::Binding;
                    c.line = st.unit.name_pos.line;
                    c.col = st.unit.name_pos.col;
                    c.end_col = st.unit.name_end.col;
                    c.detail = "generic binding";
                    d_->components[ti->second].push_back(static_cast<int>(d_->syms.size()));
                    d_->syms.push_back(c);
                }
            }
            return;
        case K::Entry:
            if (!st.unit.name.empty()) {
                AddSym(sc.parent >= 0 ? sc.parent : 0, st.unit.name, st.unit.spelling, SymKind::Entry,
                       st.unit.name_pos, st.unit.name_end);
            }
            return;
        case K::StatementFunction:
            if (!st.toks.empty()) {
                const fp::Token &t = st.toks[st.first];
                int s = AddSym(scope, t.lower, t.text, SymKind::Function, t.pos, t.end);
                d_->syms[static_cast<size_t>(s)].detail = "statement function";
                for (size_t i = st.first + 2; i < st.toks.size() && st.toks[i].text != ")"; i++) {
                    if (st.toks[i].kind == fp::TokKind::Name) d_->syms[static_cast<size_t>(s)].dummies.push_back(st.toks[i].text);
                }
            }
            return;
        case K::Associate:
        case K::SelectType:
        case K::SelectRank:
            for (const fp::Entity &e : st.entities) AddSym(scope, e.name, e.spelling, SymKind::Associate, e.pos, e.end);
            return;
        case K::Equivalence:
            for (const fp::NameRef &r : st.refs) {
                auto it = sc.names.find(r.name);
                if (it != sc.names.end()) d_->syms[static_cast<size_t>(it->second)].equivalenced = true;
            }
            return;
        default:
            return;
    }
}

int Builder::Lookup(int scope, const std::string &name, int depth) const {
    if (depth > 64) return -1;
    for (int s = scope; s >= 0; s = d_->scopes[static_cast<size_t>(s)].parent) {
        const Scope &sc = d_->scopes[static_cast<size_t>(s)];
        auto it = sc.names.find(name);
        if (it != sc.names.end()) {
            const Symbol &sym = d_->syms[static_cast<size_t>(it->second)];
            // a USE-only name from a module in this file: follow it
            if (sym.kind == SymKind::Use) {
                std::string mod = sym.from.substr(0, sym.from.find(':'));
                std::string orig = sym.from.find(':') == std::string::npos ? name : sym.from.substr(sym.from.find(':') + 1);
                auto m = d_->modules.find(mod);
                if (m != d_->modules.end()) {
                    int r = Lookup(m->second, orig, depth + 1);
                    if (r >= 0) return r;
                }
            }
            return it->second;
        }
        // modules USEd without ONLY
        for (const std::string &u : sc.uses) {
            auto m = d_->modules.find(u);
            if (m == d_->modules.end() || m->second == s) continue;
            const Scope &ms = d_->scopes[static_cast<size_t>(m->second)];
            auto mi = ms.names.find(name);
            if (mi != ms.names.end()) return mi->second;
            // what that module USEs in turn
            if (!ms.uses.empty()) {
                int r = Lookup(m->second, name, depth + 1);
                if (r >= 0 && d_->syms[static_cast<size_t>(r)].scope != 0) return r;
            }
        }
        // a submodule sees its parent module
        if (sc.kind == fp::StmtKind::Submodule && sc.construct > 0) {
            const fp::Stmt &open = d_->src->stmts[static_cast<size_t>(d_->src->constructs[static_cast<size_t>(sc.construct)].open)];
            auto m = d_->modules.find(open.unit.parent);
            if (m != d_->modules.end() && m->second != s) {
                int r = Lookup(m->second, name, depth + 1);
                if (r >= 0) return r;
            }
        }
        if (sc.parent < 0 && s != 0) {
            // interface bodies: no host association, but global names are still visible
            auto g = d_->scopes[0].names.find(name);
            return g == d_->scopes[0].names.end() ? -1 : g->second;
        }
    }
    return -1;
}

void Builder::Resolve() {
    fp::Source &src = *d_->src;
    for (size_t si = 0; si < src.stmts.size(); si++) {
        const fp::Stmt &st = src.stmts[si];
        int scope = d_->scope_of_stmt[si];
        std::vector<int> resolved(st.refs.size(), -1);
        for (size_t ri = 0; ri < st.refs.size(); ri++) {
            const fp::NameRef &r = st.refs[ri];
            FortranLspDocument::Ref out;
            out.line = r.pos.line;
            out.col = r.pos.col;
            out.end_col = r.end.line == r.pos.line ? r.end.col : r.pos.col + static_cast<int>(r.name.size());
            out.stmt = static_cast<int>(si);
            out.ctx = r.ctx;
            out.name = r.name;
            switch (r.ctx) {
                case fp::RefCtx::Keyword:
                case fp::RefCtx::Label:
                case fp::RefCtx::Construct:
                    break;
                case fp::RefCtx::Module: {
                    auto m = d_->modules.find(r.name);
                    if (m != d_->modules.end()) {
                        auto g = d_->scopes[0].names.find(r.name);
                        if (g != d_->scopes[0].names.end()) out.sym = g->second;
                    }
                    break;
                }
                case fp::RefCtx::Component: {
                    // a%b: the base's type, if a derived type of this file
                    if (r.base >= 0 && static_cast<size_t>(r.base) < resolved.size() && resolved[static_cast<size_t>(r.base)] >= 0) {
                        const Symbol &b = d_->syms[static_cast<size_t>(resolved[static_cast<size_t>(r.base)])];
                        std::string t = b.type;
                        size_t lp = t.find('(');
                        if (lp != std::string::npos && (t.compare(0, 4, "type") == 0 || t.compare(0, 5, "class") == 0)) {
                            std::string tn = t.substr(lp + 1, t.find(')') - lp - 1);
                            auto ti = d_->types.find(tn);
                            if (ti != d_->types.end()) {
                                for (int c : d_->components[ti->second]) {
                                    if (d_->syms[static_cast<size_t>(c)].name == r.name) out.sym = c;
                                }
                            }
                        }
                    }
                    break;
                }
                case fp::RefCtx::Type: {
                    int s = Lookup(scope, r.name);
                    if (s >= 0) out.sym = s;
                    else if (d_->types.count(r.name)) out.sym = d_->types[r.name];
                    break;
                }
                default:
                    out.sym = Lookup(scope, r.name);
                    break;
            }
            resolved[ri] = out.sym;
            d_->refs.push_back(out);
        }
    }
}

bool IntrinsicName(const std::string &n) {
    const FortranVocabEntry *e = FortranVocabFind(n.c_str(), n.size());
    if (e) return true;
    // Fortran 2018 coarray inquiries gfortran knows but its manual does not list
    static const char *const kMore[] = {"failed_images", "stopped_images", "image_status", "get_team",
                                        "team_number", "coshape", nullptr};
    for (const char *const *p = kMore; *p; p++) {
        if (n == *p) return true;
    }
    return false;
}

void Builder::Lints() {
    fp::Source &src = *d_->src;
    auto add = [&](int line, int col, int end_col, FortranLspSeverity sev, const std::string &code,
                   const std::string &msg) {
        FortranLspDiagnostic d;
        d.range.line = d.range.end_line = line;
        d.range.col = col;
        d.range.end_col = std::max(end_col, col + 1);
        d.severity = sev;
        d.code = code;
        d.message = msg;
        d_->diags.push_back(d);
        return &d_->diags.back();
    };
    for (const fp::Diag &dg : src.diags) {
        FortranLspDiagnostic d;
        d.range.line = dg.pos.line;
        d.range.col = dg.pos.col;
        d.range.end_line = dg.end.line;
        d.range.end_col = dg.end.col;
        if (d.range.end_line < d.range.line || (d.range.end_line == d.range.line && d.range.end_col <= d.range.col)) {
            d.range.end_line = d.range.line;
            d.range.end_col = d.range.col + 1;
        }
        d.severity = static_cast<FortranLspSeverity>(dg.severity);
        d.code = dg.code.empty() ? "syntax-error" : dg.code;
        std::string m = dg.message;
        size_t at = m.find(" at (1)");
        if (at != std::string::npos) m.erase(at, 7);
        d.message = m;
        d_->diags.push_back(d);
    }
    bool syntax_errors = !src.diags.empty();
    if (syntax_errors) return;  // a broken file's structure is not trustworthy for lints
    bool blind = src.unresolved_include || !src.macros.empty();

    // undefined labels, per program unit
    std::map<int, std::set<std::string>> labels_of_unit;  // scope -> labels defined in it or its BLOCKs
    for (size_t si = 0; si < src.stmts.size(); si++) {
        const fp::Stmt &st = src.stmts[si];
        if (st.label.empty()) continue;
        int sc = d_->scope_of_stmt[si];
        while (sc > 0 && d_->scopes[static_cast<size_t>(sc)].kind == fp::StmtKind::Block)
            sc = d_->scopes[static_cast<size_t>(sc)].parent;
        std::string l = st.label;
        size_t z = 0;
        while (z + 1 < l.size() && l[z] == '0') z++;
        labels_of_unit[sc].insert(l.substr(z));
    }
    std::map<int, std::set<std::string>> used_labels;
    for (size_t si = 0; si < src.stmts.size() && !blind; si++) {
        const fp::Stmt &st = src.stmts[si];
        int sc = d_->scope_of_stmt[si];
        while (sc > 0 && d_->scopes[static_cast<size_t>(sc)].kind == fp::StmtKind::Block)
            sc = d_->scopes[static_cast<size_t>(sc)].parent;
        for (size_t i = 0; i < st.label_refs.size(); i++) {
            used_labels[sc].insert(st.label_refs[i]);
            if (!labels_of_unit[sc].count(st.label_refs[i])) {
                fp::Pos p = st.label_ref_pos[i];
                add(p.line, p.col, p.col + static_cast<int>(st.label_refs[i].size()), FortranLspSeverity::Error,
                    "undefined-label", "Label " + st.label_refs[i] + " is never defined");
            }
        }
    }
    if (blind) return;

    // undeclared names under IMPLICIT NONE
    for (const FortranLspDocument::Ref &r : d_->refs) {
        if (r.sym >= 0) continue;
        if (r.ctx != fp::RefCtx::Read && r.ctx != fp::RefCtx::Write && r.ctx != fp::RefCtx::Type) continue;
        int scope = d_->scope_of_stmt[static_cast<size_t>(r.stmt)];
        const Scope &sc = d_->scopes[static_cast<size_t>(scope)];
        if (!sc.implicit_none || sc.opaque_use || scope == 0) continue;
        const fp::Stmt &st = src.stmts[static_cast<size_t>(r.stmt)];
        using K = fp::StmtKind;
        if (st.kind == K::Forall || st.kind == K::ForallStmt || st.kind == K::Do || st.kind == K::Data ||
            st.kind == K::Io || st.kind == K::Print || st.kind == K::Unknown || st.kind == K::Generic ||
            st.kind == K::Final || st.kind == K::Procedure || st.kind == K::TypeDecl || st.kind == K::Import ||
            st.kind == K::Namelist || st.kind == K::Allocate || st.kind == K::Interface || st.kind == K::Call ||
            st.kind == K::Attribute || st.kind == K::Common || st.kind == K::Equivalence || st.kind == K::TypeGuard ||
            st.kind == K::RankGuard || st.kind == K::Sync || st.kind == K::Lock || st.kind == K::Event ||
            st.kind == K::FailImage || st.kind == K::FormTeam || st.kind == K::Entry || st.kind == K::Parameter)
            continue;  // implied DOs, inline index types, keyword-ish names
        if (r.ctx == fp::RefCtx::Type) continue;
        if (IntrinsicName(r.name)) continue;
        // a name used with arguments may be an external function: implicit none
        // still requires its type, but an interface from an unseen module may give it
        bool args = false;
        for (const fp::NameRef &nr : st.refs) {
            if (nr.pos == fp::Pos{r.line, r.col}) args = nr.has_args;
        }
        if (args && !sc.uses.empty()) continue;
        if (d_->scopes[0].names.count(r.name)) continue;
        add(r.line, r.col, r.end_col, FortranLspSeverity::Error, "undeclared",
            "Symbol '" + r.name + "' has no IMPLICIT type");
    }

    // unused local variables of subprograms
    std::vector<int> uses(d_->syms.size(), 0);
    for (const FortranLspDocument::Ref &r : d_->refs) {
        if (r.sym >= 0) uses[static_cast<size_t>(r.sym)]++;
    }
    // names read anywhere (by name), for symbols the resolver may have missed
    std::set<std::string> any_ref;
    for (const FortranLspDocument::Ref &r : d_->refs) any_ref.insert(r.name);
    for (size_t i = 0; i < d_->syms.size(); i++) {
        const Symbol &s = d_->syms[i];
        if (s.kind != SymKind::Variable || s.scope <= 0 || uses[i] > 0) continue;
        const Scope &sc = d_->scopes[static_cast<size_t>(s.scope)];
        if (sc.kind != fp::StmtKind::Subroutine && sc.kind != fp::StmtKind::Function &&
            sc.kind != fp::StmtKind::Program && sc.kind != fp::StmtKind::ModuleProcedureUnit)
            continue;
        if (sc.is_interface_body || s.in_common || s.equivalenced || !s.declared) continue;
        if (s.attrs & (fp::kAttrSave | fp::kAttrTarget | fp::kAttrVolatile | fp::kAttrBind | fp::kAttrExternal |
                       fp::kAttrIntrinsic | fp::kAttrPublic | fp::kAttrPrivate))
            continue;
        if (any_ref.count(s.name)) continue;
        FortranLspDiagnostic *d = add(s.line, s.col, s.end_col, FortranLspSeverity::Warning, "unused-variable",
                                      "Unused variable '" + s.name + "' declared");
        d->unnecessary = true;
    }

    // free-form lines longer than 132 characters (gfortran: "Line truncated", an error by default)
    if (src.form == fp::Form::Free) {
        std::set<int> inactive(src.inactive_lines.begin(), src.inactive_lines.end());
        for (size_t li = 0; li < d_->lines.size(); li++) {
            const std::string &l = d_->lines[li];
            if (l.size() <= 132 || inactive.count(static_cast<int>(li))) continue;
            size_t k = 0;
            while (k < l.size() && (l[k] == ' ' || l[k] == '\t')) k++;
            if (k < l.size() && (l[k] == '!' || l[k] == '#')) continue;
            // only code past column 132 counts (a trailing comment is fine)
            bool code_after = false;
            bool in_s = false, in_d = false;
            for (size_t j = 0; j < l.size(); j++) {
                char c = l[j];
                if (in_s) {
                    if (c == '\'') in_s = false;
                } else if (in_d) {
                    if (c == '"') in_d = false;
                } else if (c == '\'') {
                    in_s = true;
                } else if (c == '"') {
                    in_d = true;
                } else if (c == '!') {
                    break;
                }
                if (j >= 132 && c != ' ' && c != '\t') {
                    code_after = true;
                    break;
                }
            }
            if (!code_after) continue;
            add(static_cast<int>(li), 132, static_cast<int>(l.size()), FortranLspSeverity::Warning, "line-too-long",
                "Line longer than 132 characters (gfortran truncates it; -ffree-line-length-none allows it)");
        }
    }
    std::stable_sort(d_->diags.begin(), d_->diags.end(), [](const FortranLspDiagnostic &a, const FortranLspDiagnostic &b) {
        return a.range.line != b.range.line ? a.range.line < b.range.line : a.range.col < b.range.col;
    });
}

// ---- position helpers ----------------------------------------------------------------------

// The symbol named at (line, col): a resolved reference or a declaration.
int SymbolAt(const FortranLspDocument &doc, int line, int col, FortranLspRange *range) {
    for (const auto &r : doc.refs) {
        if (r.line == line && col >= r.col && col <= r.end_col && r.sym >= 0) {
            if (range) *range = {r.line, r.col, r.line, r.end_col};
            return r.sym;
        }
    }
    for (size_t i = 0; i < doc.syms.size(); i++) {
        const Symbol &s = doc.syms[i];
        if (s.line == line && col >= s.col && col <= s.end_col) {
            if (range) *range = {s.line, s.col, s.line, s.end_col};
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string WordAt(const std::string &l, int col, int *start, int *end) {
    int n = static_cast<int>(l.size());
    int a = std::min(col, n), b = std::min(col, n);
    while (a > 0 && IsNameChar(l[static_cast<size_t>(a - 1)])) a--;
    while (b < n && IsNameChar(l[static_cast<size_t>(b)])) b++;
    if (start) *start = a;
    if (end) *end = b;
    return l.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
}

int ScopeAtLine(const FortranLspDocument &doc, int line) {
    int best = 0;
    for (size_t i = 1; i < doc.scopes.size(); i++) {
        const Scope &s = doc.scopes[i];
        if (line >= s.first_line && line <= s.last_line) {
            const Scope &b = doc.scopes[static_cast<size_t>(best)];
            if (best == 0 || s.first_line >= b.first_line) best = static_cast<int>(i);
        }
    }
    return best;
}

std::string SymbolDetail(const FortranLspDocument &doc, const Symbol &s) {
    std::string out;
    switch (s.kind) {
        case SymKind::Function:
        case SymKind::Subroutine: {
            out = (s.type.empty() ? "" : s.type + " ") + std::string(s.kind == SymKind::Function ? "function " : "subroutine ") +
                  s.spelling + "(";
            for (size_t i = 0; i < s.dummies.size(); i++) out += (i ? ", " : "") + s.dummies[i];
            out += ")";
            if (!s.detail.empty()) out += "  ! " + s.detail;
            return out;
        }
        case SymKind::Use:
            return s.spelling + "  ! from module " + s.from.substr(0, s.from.find(':'));
        default:
            break;
    }
    out = s.type.empty() ? std::string(KindWord(s.kind)) : s.type;
    static const struct {
        uint32_t bit;
        const char *w;
    } kA[] = {{fp::kAttrParameter, "parameter"}, {fp::kAttrAllocatable, "allocatable"}, {fp::kAttrPointer, "pointer"},
              {fp::kAttrTarget, "target"},       {fp::kAttrOptional, "optional"},       {fp::kAttrSave, "save"},
              {fp::kAttrValue, "value"},         {fp::kAttrProtected, "protected"},     {fp::kAttrContiguous, "contiguous"}};
    for (const auto &a : kA) {
        if (s.attrs & a.bit) out += std::string(", ") + a.w;
    }
    if ((s.attrs & fp::kAttrIntentIn) && (s.attrs & fp::kAttrIntentOut)) out += ", intent(inout)";
    else if (s.attrs & fp::kAttrIntentIn) out += ", intent(in)";
    else if (s.attrs & fp::kAttrIntentOut) out += ", intent(out)";
    out += " :: " + s.spelling;
    if (s.array) out += "(...)";
    (void)doc;
    return out;
}

FortranLspCompletionItem Item(const std::string &label, FortranLspKind kind, const std::string &detail,
                              const std::string &doc, int a, int b) {
    FortranLspCompletionItem it;
    it.label = label;
    it.insert_text = label;
    it.kind = kind;
    it.detail = detail;
    it.documentation = doc;
    it.replace_start = a;
    it.replace_end = b;
    return it;
}

const char *const kKeywords[] = {
    "program", "end", "module", "submodule", "subroutine", "function", "contains", "use", "only", "implicit",
    "none", "integer", "real", "complex", "logical", "character", "double precision", "type", "class",
    "procedure", "interface", "abstract", "parameter", "allocatable", "pointer", "target", "dimension",
    "intent", "in", "out", "inout", "optional", "save", "public", "private", "protected", "value", "volatile",
    "external", "intrinsic", "if", "then", "else", "elseif", "endif", "do", "while", "concurrent", "enddo",
    "select", "case", "default", "where", "elsewhere", "forall", "associate", "block", "critical", "cycle",
    "exit", "return", "stop", "error stop", "call", "allocate", "deallocate", "nullify", "print", "read",
    "write", "open", "close", "inquire", "format", "go to", "continue", "result", "recursive", "pure",
    "elemental", "impure", "bind", "extends", "deferred", "nopass", "pass", "generic", "final", "sequence",
    "import", "kind", "len", "data", "common", "namelist", "equivalence", "entry", "include", "enum",
    "enumerator", "rank", "is", "contiguous", "asynchronous", "codimension", "sync all", "lock", "unlock"};

}  // namespace

// ---- public API --------------------------------------------------------------------------------

std::shared_ptr<const FortranLspDocument> FortranLspAnalyze(const std::vector<std::string> &lines,
                                                            const std::string &path) {
    auto doc = std::make_shared<FortranLspDocument>();
    doc->lines = lines;
    doc->path = path;
    fp::ReadOptions o;
    o.form = fp::FormForPath(path);
    o.preprocess = fp::PreprocessedPath(path);
    doc->src = fp::Read(lines, o);
    Builder b(doc.get());
    b.Run();
    return doc;
}

std::vector<FortranLspDiagnostic> FortranLspDiagnostics(const FortranLspDocument &doc) { return doc.diags; }

std::string FortranLspGfortranCheck(const std::string &text, const std::string &path) {
    fp::ReadOptions o;
    o.form = fp::FormForPath(path);
    o.preprocess = fp::PreprocessedPath(path);
    auto src = fp::Read(fp::SplitLines(fp::DecodeSource(text)), o);
    if (src->diags.empty()) return "";
    const fp::Diag &d = src->diags.front();
    if (d.message.compare(0, 22, "Unexpected end of file") == 0) return "0: " + d.message;
    return std::to_string(d.pos.line + 1) + ": " + d.message;
}

std::string FortranLspFormatDiagnostic(const std::string &path, const FortranLspDiagnostic &d) {
    const char *sev = d.severity == FortranLspSeverity::Error         ? "error"
                      : d.severity == FortranLspSeverity::Warning     ? "warning"
                      : d.severity == FortranLspSeverity::Information ? "info"
                                                                      : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool FortranLspDiagnosticFails(const FortranLspDiagnostic &d) {
    return d.severity == FortranLspSeverity::Error || d.severity == FortranLspSeverity::Warning;
}

std::vector<FortranLspCompletionItem> FortranLspCompletions(const FortranLspDocument &doc, int line, int col) {
    std::vector<FortranLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return out;
    const std::string &l = doc.lines[static_cast<size_t>(line)];
    int a = 0, b = 0;
    std::string word = WordAt(l, col, &a, &b);
    b = col;
    word = l.substr(static_cast<size_t>(a), static_cast<size_t>(std::max(0, col - a)));
    std::string lw = Lower(word);
    std::set<std::string> seen;
    auto want = [&](const std::string &name) { return Lower(name).compare(0, lw.size(), lw) == 0 && seen.insert(Lower(name)).second; };
    // components after `%`
    int p = a - 1;
    while (p >= 0 && (l[static_cast<size_t>(p)] == ' ')) p--;
    if (p >= 0 && l[static_cast<size_t>(p)] == '%') {
        int q = p - 1;
        while (q >= 0 && l[static_cast<size_t>(q)] == ' ') q--;
        if (q >= 0 && l[static_cast<size_t>(q)] == ')') {
            int depth = 0;
            for (; q >= 0; q--) {
                if (l[static_cast<size_t>(q)] == ')') depth++;
                else if (l[static_cast<size_t>(q)] == '(' && --depth == 0) break;
            }
            q--;
        }
        int bs = 0;
        std::string base = q >= 0 ? WordAt(l, q, &bs, nullptr) : "";
        int sym = -1;
        for (const auto &r : doc.refs) {
            if (r.line == line && r.col == bs && r.sym >= 0) sym = r.sym;
        }
        if (sym < 0) {
            int sc = ScopeAtLine(doc, line);
            for (int s = sc; s >= 0 && sym < 0; s = doc.scopes[static_cast<size_t>(s)].parent) {
                auto it = doc.scopes[static_cast<size_t>(s)].names.find(Lower(base));
                if (it != doc.scopes[static_cast<size_t>(s)].names.end()) sym = it->second;
            }
        }
        if (sym >= 0) {
            const std::string &t = doc.syms[static_cast<size_t>(sym)].type;
            size_t lp = t.find('(');
            if (lp != std::string::npos) {
                auto ti = doc.types.find(t.substr(lp + 1, t.find(')') - lp - 1));
                if (ti != doc.types.end()) {
                    auto ci = doc.components.find(ti->second);
                    if (ci != doc.components.end()) {
                        for (int c : ci->second) {
                            const Symbol &cs = doc.syms[static_cast<size_t>(c)];
                            if (!want(cs.spelling)) continue;
                            out.push_back(Item(cs.spelling, cs.kind == SymKind::Binding ? FortranLspKind::Method : FortranLspKind::Field,
                                               SymbolDetail(doc, cs), "", a, b));
                        }
                    }
                }
            }
        }
        return out;
    }
    // after `use`: module names
    std::string before = Lower(l.substr(0, static_cast<size_t>(a)));
    size_t k = before.find_first_not_of(" \t");
    if (k != std::string::npos && before.compare(k, 3, "use") == 0 && before.find(',') == std::string::npos) {
        for (const auto &m : doc.modules) {
            if (want(m.first)) out.push_back(Item(m.first, FortranLspKind::Module, "module", "", a, b));
        }
        size_t n = 0;
        const FortranVocabEntry *v = FortranVocab(&n);
        for (size_t i = 0; i < n; i++) {
            if (v[i].kind == FortranVocabKind::Module && want(v[i].name))
                out.push_back(Item(v[i].name, FortranLspKind::Module, "intrinsic module", v[i].summary, a, b));
        }
        return out;
    }
    // names in scope
    int sc = ScopeAtLine(doc, line);
    for (int s = sc; s >= 0; s = doc.scopes[static_cast<size_t>(s)].parent) {
        for (const auto &kv : doc.scopes[static_cast<size_t>(s)].names) {
            const Symbol &sym = doc.syms[static_cast<size_t>(kv.second)];
            if (!want(sym.spelling)) continue;
            FortranLspKind kind = sym.kind == SymKind::Function || sym.kind == SymKind::Subroutine ? FortranLspKind::Function
                                  : sym.kind == SymKind::Module ? FortranLspKind::Module
                                  : sym.kind == SymKind::Type ? FortranLspKind::Struct
                                  : sym.kind == SymKind::Parameter ? FortranLspKind::Constant
                                                                   : FortranLspKind::Variable;
            out.push_back(Item(sym.spelling, kind, SymbolDetail(doc, sym), "", a, b));
        }
        if (s == 0) break;
    }
    for (const auto &kv : doc.scopes[0].names) {
        const Symbol &sym = doc.syms[static_cast<size_t>(kv.second)];
        if (want(sym.spelling)) out.push_back(Item(sym.spelling, FortranLspKind::Function, SymbolDetail(doc, sym), "", a, b));
    }
    // modules USEd from this file
    for (int s = sc; s > 0; s = doc.scopes[static_cast<size_t>(s)].parent) {
        for (const std::string &u : doc.scopes[static_cast<size_t>(s)].uses) {
            auto m = doc.modules.find(u);
            if (m == doc.modules.end()) continue;
            for (const auto &kv : doc.scopes[static_cast<size_t>(m->second)].names) {
                const Symbol &sym = doc.syms[static_cast<size_t>(kv.second)];
                if (want(sym.spelling)) out.push_back(Item(sym.spelling, FortranLspKind::Variable, SymbolDetail(doc, sym), "", a, b));
            }
        }
    }
    // intrinsics and keywords
    size_t n = 0;
    const FortranVocabEntry *v = FortranVocab(&n);
    for (size_t i = 0; i < n && !lw.empty(); i++) {
        if (v[i].kind == FortranVocabKind::Module || v[i].kind == FortranVocabKind::Keyword) continue;
        if (!want(v[i].name)) continue;
        FortranLspKind kind = v[i].kind == FortranVocabKind::Constant ? FortranLspKind::Constant
                              : v[i].kind == FortranVocabKind::Type ? FortranLspKind::Struct
                                                                    : FortranLspKind::Function;
        std::string sig = v[i].signature;
        out.push_back(Item(v[i].name, kind, sig.substr(0, sig.find('\n')), v[i].summary, a, b));
    }
    for (const char *kw : kKeywords) {
        if (!lw.empty() && want(kw)) out.push_back(Item(kw, FortranLspKind::Keyword, "keyword", "", a, b));
    }
    return out;
}

FortranLspHoverInfo FortranLspHover(const FortranLspDocument &doc, int line, int col) {
    FortranLspHoverInfo h;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return h;
    FortranLspRange range;
    int sym = SymbolAt(doc, line, col, &range);
    if (sym >= 0) {
        const Symbol &s = doc.syms[static_cast<size_t>(sym)];
        h.found = true;
        h.range = range;
        h.text = "```fortran\n" + SymbolDetail(doc, s) + "\n```\n" + KindWord(s.kind) + ", declared on line " +
                 std::to_string(s.line + 1);
        return h;
    }
    int a = 0, b = 0;
    std::string w = Lower(WordAt(doc.lines[static_cast<size_t>(line)], col, &a, &b));
    if (w.empty()) return h;
    const FortranVocabEntry *e = FortranVocabFind(w.c_str(), w.size());
    if (!e) return h;
    h.found = true;
    h.range = {line, a, line, b};
    std::string sig = e->signature;
    h.text = (sig.empty() ? "" : "```fortran\n" + sig + "\n```\n") + e->summary;
    std::string meta = e->klass;
    if (*e->module) meta += std::string(meta.empty() ? "" : ", ") + "module " + e->module;
    if (*e->standard) meta += std::string(meta.empty() ? "" : " -- ") + e->standard;
    if (!meta.empty()) h.text += "\n\n*" + meta + "*";
    return h;
}

FortranLspSignatureHelp FortranLspSignatureAt(const FortranLspDocument &doc, int line, int col) {
    FortranLspSignatureHelp sh;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return sh;
    const std::string &l = doc.lines[static_cast<size_t>(line)];
    // the innermost unclosed `name(` before the cursor on this line
    int depth = 0, commas = 0;
    int i = std::min(col, static_cast<int>(l.size())) - 1;
    for (; i >= 0; i--) {
        char c = l[static_cast<size_t>(i)];
        if (c == ')') depth++;
        else if (c == '(') {
            if (depth == 0) break;
            depth--;
        } else if (c == ',' && depth == 0) {
            commas++;
        }
    }
    if (i <= 0) return sh;
    int j = i - 1;
    while (j >= 0 && l[static_cast<size_t>(j)] == ' ') j--;
    if (j < 0) return sh;
    int a = 0;
    std::string name = Lower(WordAt(l, j, &a, nullptr));
    if (name.empty()) return sh;
    // a procedure of this file
    int sc = ScopeAtLine(doc, line);
    int sym = -1;
    for (int s = sc; s >= 0 && sym < 0; s = doc.scopes[static_cast<size_t>(s)].parent) {
        auto it = doc.scopes[static_cast<size_t>(s)].names.find(name);
        if (it != doc.scopes[static_cast<size_t>(s)].names.end()) sym = it->second;
        if (s == 0) break;
    }
    if (sym < 0) {
        auto it = doc.scopes[0].names.find(name);
        if (it != doc.scopes[0].names.end()) sym = it->second;
    }
    if (sym >= 0 && (doc.syms[static_cast<size_t>(sym)].kind == SymKind::Function ||
                     doc.syms[static_cast<size_t>(sym)].kind == SymKind::Subroutine)) {
        const Symbol &s = doc.syms[static_cast<size_t>(sym)];
        sh.found = true;
        sh.label = s.spelling + "(";
        for (size_t k = 0; k < s.dummies.size(); k++) {
            if (k) sh.label += ", ";
            sh.label += s.dummies[k];
            sh.params.push_back(s.dummies[k]);
        }
        sh.label += ")";
        sh.active_param = std::min(commas, std::max(0, static_cast<int>(sh.params.size()) - 1));
        sh.documentation = KindWord(s.kind);
        return sh;
    }
    const FortranVocabEntry *e = FortranVocabFind(name.c_str(), name.size());
    if (!e || !*e->signature) return sh;
    std::string sig = e->signature;
    sig = sig.substr(0, sig.find('\n'));
    sh.found = true;
    sh.label = sig;
    size_t lp = sig.find('('), rp = sig.rfind(')');
    if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
        std::string inner = sig.substr(lp + 1, rp - lp - 1);
        size_t start = 0;
        for (size_t k = 0; k <= inner.size(); k++) {
            if (k == inner.size() || inner[k] == ',') {
                std::string pr = inner.substr(start, k - start);
                // trim blanks and brackets
                while (!pr.empty() && (pr.front() == ' ' || pr.front() == '[')) pr.erase(0, 1);
                while (!pr.empty() && (pr.back() == ' ' || pr.back() == ']')) pr.pop_back();
                if (!pr.empty()) sh.params.push_back(pr);
                start = k + 1;
            }
        }
    }
    sh.active_param = std::min(commas, std::max(0, static_cast<int>(sh.params.size()) - 1));
    sh.documentation = e->summary;
    return sh;
}

std::vector<FortranLspLocation> FortranLspDefinition(const FortranLspDocument &doc, int line, int col) {
    std::vector<FortranLspLocation> out;
    int sym = SymbolAt(doc, line, col, nullptr);
    if (sym < 0) return out;
    const Symbol &s = doc.syms[static_cast<size_t>(sym)];
    FortranLspLocation loc;
    loc.range = {s.line, s.col, s.line, s.end_col};
    out.push_back(loc);
    return out;
}

std::vector<FortranLspReference> FortranLspReferences(const FortranLspDocument &doc, int line, int col) {
    std::vector<FortranLspReference> out;
    int sym = SymbolAt(doc, line, col, nullptr);
    if (sym < 0) return out;
    const Symbol &s = doc.syms[static_cast<size_t>(sym)];
    FortranLspReference def;
    def.range = {s.line, s.col, s.line, s.end_col};
    def.is_definition = true;
    out.push_back(def);
    std::set<std::pair<int, int>> seen{{s.line, s.col}};
    for (const auto &r : doc.refs) {
        if (r.sym != sym || !seen.insert({r.line, r.col}).second) continue;
        FortranLspReference ref;
        ref.range = {r.line, r.col, r.line, r.end_col};
        out.push_back(ref);
    }
    // END SUBROUTINE name / END TYPE name etc. name the same thing
    if (s.unit >= 0 || s.kind == SymKind::Type) {
        for (const auto &r : doc.refs) {
            if (r.ctx == fp::RefCtx::Construct && r.name == s.name && seen.insert({r.line, r.col}).second) {
                FortranLspReference ref;
                ref.range = {r.line, r.col, r.line, r.end_col};
                out.push_back(ref);
            }
        }
    }
    std::sort(out.begin() + 1, out.end(), [](const FortranLspReference &a, const FortranLspReference &b) {
        return a.range.line != b.range.line ? a.range.line < b.range.line : a.range.col < b.range.col;
    });
    return out;
}

FortranLspRenameTarget FortranLspPrepareRename(const FortranLspDocument &doc, int line, int col) {
    FortranLspRenameTarget t;
    FortranLspRange range;
    int sym = SymbolAt(doc, line, col, &range);
    if (sym < 0) return t;
    const Symbol &s = doc.syms[static_cast<size_t>(sym)];
    if (s.kind == SymKind::Use) return t;  // the name belongs to a module this file does not hold
    t.found = true;
    t.range = range;
    t.placeholder = doc.lines[static_cast<size_t>(range.line)].substr(static_cast<size_t>(range.col),
                                                                       static_cast<size_t>(range.end_col - range.col));
    return t;
}

std::vector<FortranLspTextEdit> FortranLspRename(const FortranLspDocument &doc, int line, int col,
                                                 const std::string &new_name, std::string *error) {
    std::vector<FortranLspTextEdit> edits;
    if (!FortranLspPrepareRename(doc, line, col).found) {
        *error = "nothing to rename here";
        return edits;
    }
    bool ok = !new_name.empty() && new_name.size() <= 63 &&
              ((new_name[0] >= 'a' && new_name[0] <= 'z') || (new_name[0] >= 'A' && new_name[0] <= 'Z'));
    for (char c : new_name) ok = ok && IsNameChar(c);
    if (!ok) {
        *error = "'" + new_name + "' is not a Fortran name";
        return edits;
    }
    int sym = SymbolAt(doc, line, col, nullptr);
    const Symbol &s = doc.syms[static_cast<size_t>(sym)];
    // refuse a rename that collides with another name in the same scope
    if (s.scope >= 0) {
        const Scope &sc = doc.scopes[static_cast<size_t>(s.scope)];
        auto it = sc.names.find(Lower(new_name));
        if (it != sc.names.end() && it->second != sym) {
            *error = "'" + new_name + "' is already declared here";
            return edits;
        }
    }
    for (const FortranLspReference &r : FortranLspReferences(doc, line, col)) {
        FortranLspTextEdit e;
        e.range = r.range;
        e.new_text = new_name;
        edits.push_back(e);
    }
    return edits;
}

std::vector<FortranLspSymbol> FortranLspSymbols(const FortranLspDocument &doc) {
    std::vector<FortranLspSymbol> out;
    const fp::Source &src = *doc.src;
    std::map<int, int> out_of_construct;  // construct -> outline index
    for (size_t ci = 1; ci < src.constructs.size(); ci++) {
        const fp::Construct &c = src.constructs[ci];
        using K = fp::StmtKind;
        if (!IsUnit(c.kind) && c.kind != K::DerivedType && c.kind != K::Interface) continue;
        const fp::Stmt &open = src.stmts[static_cast<size_t>(c.open)];
        if (c.kind == K::Interface && open.unit.name.empty()) continue;
        FortranLspSymbol s;
        s.name = open.unit.name.empty() ? (c.kind == K::Program ? "main program" : "block data") : open.unit.spelling;
        if (s.name.empty()) s.name = open.unit.name;
        s.kind = c.kind == K::Module || c.kind == K::Submodule ? FortranLspSymbolKind::Module
                 : c.kind == K::Program ? FortranLspSymbolKind::Namespace
                 : c.kind == K::DerivedType ? FortranLspSymbolKind::Class
                 : c.kind == K::Interface ? FortranLspSymbolKind::Namespace
                                          : FortranLspSymbolKind::Function;
        s.detail = c.kind == K::Subroutine ? "subroutine" : c.kind == K::Function ? "function"
                   : c.kind == K::Module ? "module" : c.kind == K::Submodule ? "submodule"
                   : c.kind == K::DerivedType ? "type" : c.kind == K::Interface ? "interface"
                   : c.kind == K::Program ? "program" : "";
        int end_line = c.close >= 0 ? src.stmts[static_cast<size_t>(c.close)].end.line : static_cast<int>(doc.lines.size()) - 1;
        int end_col = c.close >= 0 ? src.stmts[static_cast<size_t>(c.close)].end.col : 0;
        s.range = {open.pos.line, 0, end_line, end_col};
        if (!open.unit.name.empty()) s.sel_range = {open.unit.name_pos.line, open.unit.name_pos.col, open.unit.name_end.line, open.unit.name_end.col};
        else s.sel_range = {open.pos.line, open.pos.col, open.pos.line, open.end.line == open.pos.line ? open.end.col : open.pos.col + 1};
        int p = c.parent;
        while (p > 0 && !out_of_construct.count(p)) p = src.constructs[static_cast<size_t>(p)].parent;
        s.parent = p > 0 ? out_of_construct[p] : -1;
        out_of_construct[static_cast<int>(ci)] = static_cast<int>(out.size());
        out.push_back(s);
    }
    // module-level variables/parameters, and components of types
    for (size_t i = 0; i < doc.syms.size(); i++) {
        const Symbol &sym = doc.syms[i];
        int parent = -1;
        if (sym.scope > 0) {
            const Scope &sc = doc.scopes[static_cast<size_t>(sym.scope)];
            if (sc.kind != fp::StmtKind::Module && sc.kind != fp::StmtKind::Submodule) continue;
            if (sym.kind != SymKind::Variable && sym.kind != SymKind::Parameter) continue;
            parent = out_of_construct.count(sc.construct) ? out_of_construct[sc.construct] : -1;
        } else {
            continue;
        }
        FortranLspSymbol s;
        s.name = sym.spelling;
        s.detail = sym.type;
        s.kind = sym.kind == SymKind::Parameter ? FortranLspSymbolKind::Constant : FortranLspSymbolKind::Variable;
        s.range = s.sel_range = {sym.line, sym.col, sym.line, sym.end_col};
        s.parent = parent;
        out.push_back(s);
    }
    for (const auto &kv : doc.components) {
        const Symbol &ts = doc.syms[static_cast<size_t>(kv.first)];
        int parent = -1;
        for (size_t k = 0; k < out.size(); k++) {
            if (out[k].kind == FortranLspSymbolKind::Class && out[k].sel_range.line == ts.line && out[k].sel_range.col == ts.col)
                parent = static_cast<int>(k);
        }
        for (int c : kv.second) {
            const Symbol &cs = doc.syms[static_cast<size_t>(c)];
            FortranLspSymbol s;
            s.name = cs.spelling;
            s.detail = cs.type;
            s.kind = cs.kind == SymKind::Binding ? FortranLspSymbolKind::Method : FortranLspSymbolKind::Field;
            s.range = s.sel_range = {cs.line, cs.col, cs.line, cs.end_col};
            s.parent = parent;
            out.push_back(s);
        }
    }
    return out;
}

std::vector<FortranLspFold> FortranLspFolds(const FortranLspDocument &doc) {
    std::vector<FortranLspFold> out;
    const fp::Source &src = *doc.src;
    for (size_t ci = 1; ci < src.constructs.size(); ci++) {
        const fp::Construct &c = src.constructs[ci];
        if (c.close < 0 || c.open < 0) continue;
        int a = src.stmts[static_cast<size_t>(c.open)].pos.line;
        int b = src.stmts[static_cast<size_t>(c.close)].pos.line;
        if (b > a) out.push_back({a, b, ""});
    }
    // runs of comment lines
    int start = -1, last = -1;
    for (const fp::Comment &cm : src.comments) {
        if (cm.pos.line == last + 1 && start >= 0) {
            last = cm.pos.line;
            continue;
        }
        if (start >= 0 && last > start) out.push_back({start, last, "comment"});
        start = last = cm.pos.line;
    }
    if (start >= 0 && last > start) out.push_back({start, last, "comment"});
    std::sort(out.begin(), out.end(), [](const FortranLspFold &x, const FortranLspFold &y) {
        return x.start_line != y.start_line ? x.start_line < y.start_line : x.end_line > y.end_line;
    });
    return out;
}

std::vector<FortranLspTextEdit> FortranLspFormat(const std::vector<std::string> &lines, const std::string &path,
                                                 std::string *error) {
    std::vector<FortranLspTextEdit> edits;
    if (fp::FormForPath(path) == fp::Form::Fixed) {
        *error = "fixed-form source is not formatted (fprettify's rules are for free form)";
        return edits;
    }
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    text += '\n';
    const fortranfmt::Result r = fortranfmt::Format(text, fortranfmt::Options{});
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    std::string formatted = r.text;
    if (!formatted.empty() && formatted.back() == '\n') formatted.pop_back();
    text.pop_back();
    if (formatted == text) return edits;
    std::vector<std::string> b = fp::SplitLines(formatted);
    size_t pre = 0;
    while (pre < lines.size() && pre < b.size() && lines[pre] == b[pre]) pre++;
    size_t suf = 0;
    while (suf < lines.size() - pre && suf < b.size() - pre && lines[lines.size() - 1 - suf] == b[b.size() - 1 - suf])
        suf++;
    FortranLspTextEdit e;
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
