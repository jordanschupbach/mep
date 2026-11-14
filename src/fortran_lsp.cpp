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
    // for documentation and cross-file queries
    int stmt = -1;             // the statement that declares it (its FORD docs sit around it)
    bool placeholder = false;  // made by an attribute statement (PUBLIC :: x) before its declaration
    std::vector<std::string> specifics;  // generic interface / generic binding / type constructor
    bool constructor = false;  // a type that also names a generic interface
    std::string extends;       // type: parent type
    std::string target;        // binding: the procedure it binds (or the interface of a deferred one)
    std::string pass;          // binding: "" passes the first dummy, "-" is NOPASS, else PASS(name)
    int owner = -1;            // component/binding: the type symbol
    int decl_line = -1, decl_end = -1;  // where its type declaration names it (a dummy's is not its line)
    std::string prefix;        // procedure prefix (pure, elemental, module, ...)
    std::string result;        // function RESULT name, when not the function's own
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
    struct Use {
        std::string module;
        bool only = false;
        std::set<std::string> renamed;  // module names renamed away (local => name): not visible as name
    };
    std::vector<Use> use_list;
    bool default_private = false;  // a bare PRIVATE statement in a module
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
    // modules from other files, as the module source resolved them (USE name -> analysis)
    std::map<std::string, std::shared_ptr<const FortranLspDocument>> imports;
    std::vector<std::string> known_modules;  // what the module source can resolve
    std::map<int, std::string> trailing_docs;  // line -> its trailing `!!`/`!<` comment
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

// The binding a PROCEDURE statement in a type's CONTAINS part makes of entity @p e:
// its target (name => target, or the interface of PROCEDURE(iface)) and how it passes the object.
void BindingDetails(const fp::Stmt &st, const fp::Entity &e, Symbol *c) {
    const std::vector<fp::Token> &t = st.toks;
    size_t dc = t.size();
    for (size_t i = st.first; i < t.size(); i++) {
        if (t[i].text == "::") {
            dc = i;
            break;
        }
    }
    size_t attrs_end = dc == t.size() ? st.first + 1 : dc;
    for (size_t i = st.first + 1; i < attrs_end && i < t.size(); i++) {
        if (t[i].kind != fp::TokKind::Name) continue;
        if (t[i].lower == "nopass") {
            c->pass = "-";
        } else if (t[i].lower == "pass") {
            if (i + 2 < attrs_end && t[i + 1].text == "(" && t[i + 2].kind == fp::TokKind::Name) c->pass = t[i + 2].lower;
        } else if (t[i].lower == "deferred") {
            c->detail = "deferred";
        }
    }
    if (t.size() > st.first + 3 && t[st.first].lower == "procedure" && t[st.first + 1].text == "(" &&
        t[st.first + 2].kind == fp::TokKind::Name)
        c->target = t[st.first + 2].lower;
    for (size_t i = dc == t.size() ? st.first : dc; i < t.size(); i++) {
        if (t[i].pos == e.pos) {
            if (i + 2 < t.size() && t[i + 1].text == "=>" && t[i + 2].kind == fp::TokKind::Name) c->target = t[i + 2].lower;
            break;
        }
    }
    if (c->target.empty()) c->target = e.name;
}

// ---- building the model ------------------------------------------------------------------

class Builder {
public:
    explicit Builder(FortranLspDocument *d) : d_(d) {}
    void Run();

private:
    int AddSym(int scope, const std::string &name, const std::string &spelling, SymKind kind, fp::Pos pos,
               fp::Pos end, bool placeholder = false) {
        Scope &sc = d_->scopes[static_cast<size_t>(scope)];
        auto it = sc.names.find(name);
        if (it != sc.names.end()) {
            Symbol &old = d_->syms[static_cast<size_t>(it->second)];
            if (old.placeholder && !placeholder) {
                // PUBLIC :: x (or a PROCEDURE in an interface) came first: this is the declaration
                old.placeholder = false;
                old.kind = kind;
                old.spelling = spelling.empty() ? name : spelling;
                old.line = pos.line;
                old.col = pos.col;
                old.end_col = end.line == pos.line ? end.col : pos.col + static_cast<int>(name.size());
                old.stmt = cur_stmt_;
            }
            return it->second;
        }
        Symbol s;
        s.placeholder = placeholder;
        s.stmt = cur_stmt_;
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

    void AddSpecifics(int scope, int interface_construct, const std::vector<std::string> &names);

    FortranLspDocument *d_;
    std::map<int, int> scope_of_construct_;
    int cur_stmt_ = -1;

public:
    FortranLspModuleSource *modules_ = nullptr;
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
    for (size_t si = 0; si < src.stmts.size(); si++) {
        cur_stmt_ = static_cast<int>(si);
        Declare(static_cast<int>(si), d_->scope_of_stmt[si]);
    }
    cur_stmt_ = -1;
    // trailing `!!` / `!<` comments: FORD (and Doxygen) documentation of the statement they end
    for (const fp::Comment &cm : src.comments) {
        if (cm.text.size() < 2 || cm.text[0] != '!' || (cm.text[1] != '!' && cm.text[1] != '<')) continue;
        if (static_cast<size_t>(cm.pos.line) >= d_->lines.size()) continue;
        const std::string &l = d_->lines[static_cast<size_t>(cm.pos.line)];
        size_t first = l.find_first_not_of(" \t");
        if (first != std::string::npos && static_cast<int>(first) < cm.pos.col) d_->trailing_docs[cm.pos.line] = cm.text;
    }
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
            sym.prefix = st.unit.prefix;
            if (st.kind == K::Function && st.unit.result != st.unit.name) sym.result = st.unit.result;
            for (const fp::Entity &e : st.unit.dummies) sym.dummies.push_back(e.spelling);
            if (st.kind == K::ModuleProcedureUnit) sym.detail = "module procedure";
            // an interface body of a named generic interface is one of its specifics
            if (sc.is_interface_body && in->parent > 0 &&
                src.constructs[static_cast<size_t>(in->parent)].kind == K::Interface)
                AddSpecifics(host, in->parent, {st.unit.name});
        }
        for (const fp::Entity &e : st.unit.dummies) {
            if (e.name == "*") continue;
            int d = AddSym(own, e.name, e.spelling, SymKind::Dummy, e.pos, e.end);
            // its documentation is at its type declaration, not at the procedure statement
            if (d_->syms[static_cast<size_t>(d)].stmt == si) d_->syms[static_cast<size_t>(d)].stmt = -1;
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
            if (d_->syms[static_cast<size_t>(s)].stmt == si) d_->syms[static_cast<size_t>(s)].stmt = -1;
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
            // a module from another file: the module source finds and analyses it
            if (!known && !st.use_intrinsic && modules_ && !d_->imports.count(st.group) &&
                !(intrinsic && st.group != "mpi" && st.group != "mpi_f08")) {
                std::shared_ptr<const FortranLspDocument> m = modules_->Module(st.group);
                if (m) d_->imports[st.group] = m;
            }
            {
                Scope::Use u;
                u.module = st.group;
                u.only = st.use_only;
                for (const fp::Entity &e : st.entities) {
                    if (!e.rename.empty()) u.renamed.insert(e.rename);
                }
                sc.use_list.push_back(u);
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
            bool in_interface = in && in->kind == K::Interface;
            if (in_interface && st.kind == K::Procedure) {
                // PROCEDURE :: a, b inside a generic interface: its specifics
                std::vector<std::string> names;
                for (const fp::Entity &e : st.entities) names.push_back(e.name);
                AddSpecifics(scope, cons, names);
            }
            for (const fp::Entity &e : st.entities) {
                if (in_type) {
                    // component of the type opened by in->open
                    const fp::Stmt &ts = src.stmts[static_cast<size_t>(in->open)];
                    auto ti = sc.names.find(ts.unit.name);
                    if (ti == sc.names.end()) continue;
                    Symbol c;
                    c.name = e.name;
                    c.spelling = e.spelling;
                    bool after_contains = in->contains >= 0 && si > in->contains;
                    c.kind = st.kind == K::Procedure || (after_contains && st.type.compare(0, 9, "procedure") == 0)
                                 ? SymKind::Binding
                                 : SymKind::Component;
                    c.line = e.pos.line;
                    c.col = e.pos.col;
                    c.end_col = e.end.col;
                    c.scope = -1;
                    c.type = st.type;
                    c.attrs = st.attrs;
                    c.array = e.array;
                    c.declared = true;
                    c.stmt = si;
                    c.owner = ti->second;
                    if (c.kind == SymKind::Binding) BindingDetails(st, e, &c);
                    d_->components[ti->second].push_back(static_cast<int>(d_->syms.size()));
                    d_->syms.push_back(c);
                    continue;
                }
                SymKind k = (st.attrs & fp::kAttrParameter) ? SymKind::Parameter : SymKind::Variable;
                if (st.kind == K::Procedure || st.type.compare(0, 9, "procedure") == 0) k = SymKind::Function;
                int s = AddSym(scope, e.name, e.spelling, k, e.pos, e.end, in_interface && st.kind == K::Procedure);
                Symbol &sym = d_->syms[static_cast<size_t>(s)];
                // a dummy or result typed here: its documentation is here
                if (sym.stmt < 0 && (sym.kind == SymKind::Dummy || sym.kind == SymKind::Result)) {
                    sym.stmt = si;
                    sym.decl_line = e.pos.line;
                    sym.decl_end = e.end.line == e.pos.line ? e.end.col : e.pos.col + static_cast<int>(e.name.size());
                }
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
        case K::Common: {
            std::string kw = st.toks.empty() ? "" : st.toks[st.first].lower;
            if (st.kind == K::Attribute && st.entities.empty() && in && in->kind == K::Module &&
                (kw == "private" || kw == "public"))
                sc.default_private = kw == "private";
            for (const fp::Entity &e : st.entities) {
                int s = AddSym(scope, e.name, e.spelling, SymKind::Variable, e.pos, e.end, st.kind == K::Attribute);
                Symbol &sym = d_->syms[static_cast<size_t>(s)];
                if (st.kind == K::Common) sym.in_common = true;
                if (e.array) sym.array = true;
                std::string w = kw;
                if (w == "public") sym.attrs |= fp::kAttrPublic;
                if (w == "private") sym.attrs |= fp::kAttrPrivate;
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
        }
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
            Symbol &sym = d_->syms[static_cast<size_t>(s)];
            if (sym.kind == SymKind::Interface) {
                // the generic interface of the same name came first: it is the type's constructor
                sym.kind = SymKind::Type;
                sym.constructor = true;
                sym.spelling = st.unit.spelling;
                sym.line = st.unit.name_pos.line;
                sym.col = st.unit.name_pos.col;
                sym.end_col = st.unit.name_end.col;
                sym.stmt = si;
            }
            for (const fp::NameRef &r : st.refs) {
                if (r.ctx == fp::RefCtx::Type) sym.extends = r.name;
            }
            if (st.abstract) sym.detail = "abstract";
            if (!d_->types.count(st.unit.name)) d_->types[st.unit.name] = s;
            return;
        }
        case K::Interface:
            if (!st.unit.name.empty() && !st.generic_spec) {
                int s = AddSym(scope, st.unit.name, st.unit.spelling, SymKind::Interface, st.unit.name_pos,
                               st.unit.name_end);
                if (d_->syms[static_cast<size_t>(s)].kind == SymKind::Type) d_->syms[static_cast<size_t>(s)].constructor = true;
            }
            return;
        case K::ModuleProcedureList:
            if (in && in->kind == K::Interface) {
                std::vector<std::string> names;
                for (const fp::Entity &e : st.entities) names.push_back(e.name);
                AddSpecifics(scope, cons, names);
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
                    if (st.unit.name_pos.line >= 0 && static_cast<size_t>(st.unit.name_pos.line) < d_->lines.size()) {
                        const std::string &l = d_->lines[static_cast<size_t>(st.unit.name_pos.line)];
                        if (st.unit.name_end.col <= static_cast<int>(l.size()))
                            c.spelling = l.substr(static_cast<size_t>(st.unit.name_pos.col),
                                                  static_cast<size_t>(st.unit.name_end.col - st.unit.name_pos.col));
                    }
                    c.kind = SymKind::Binding;
                    c.line = st.unit.name_pos.line;
                    c.col = st.unit.name_pos.col;
                    c.end_col = st.unit.name_end.col;
                    c.detail = "generic binding";
                    c.stmt = si;
                    c.owner = ti->second;
                    for (const fp::NameRef &r : st.refs) {
                        if (r.ctx == fp::RefCtx::Generic) c.specifics.push_back(r.name);
                    }
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

void Builder::AddSpecifics(int scope, int interface_construct, const std::vector<std::string> &names) {
    const fp::Source &src = *d_->src;
    if (interface_construct <= 0) return;
    const fp::Stmt &is = src.stmts[static_cast<size_t>(src.constructs[static_cast<size_t>(interface_construct)].open)];
    if (is.unit.name.empty() || is.generic_spec) return;
    Scope &sc = d_->scopes[static_cast<size_t>(scope)];
    auto it = sc.names.find(is.unit.name);
    if (it == sc.names.end()) return;
    Symbol &g = d_->syms[static_cast<size_t>(it->second)];
    for (const std::string &n : names) {
        if (std::find(g.specifics.begin(), g.specifics.end(), n) == g.specifics.end()) g.specifics.push_back(n);
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
    if ((s.attrs & fp::kAttrIntentIn) && (s.attrs & fp::kAttrIntentOut)) out += ", intent(inout)";
    else if (s.attrs & fp::kAttrIntentIn) out += ", intent(in)";
    else if (s.attrs & fp::kAttrIntentOut) out += ", intent(out)";
    static const struct {
        uint32_t bit;
        const char *w;
    } kA[] = {{fp::kAttrParameter, "parameter"}, {fp::kAttrAllocatable, "allocatable"}, {fp::kAttrPointer, "pointer"},
              {fp::kAttrTarget, "target"},       {fp::kAttrOptional, "optional"},       {fp::kAttrSave, "save"},
              {fp::kAttrValue, "value"},         {fp::kAttrProtected, "protected"},     {fp::kAttrContiguous, "contiguous"}};
    for (const auto &a : kA) {
        if (s.attrs & a.bit) out += std::string(", ") + a.w;
    }
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

// ---- across files: targets, lookups, FORD documentation ------------------------------------------

// A symbol in some analysis: this document's, or that of a module it imports.
struct Target {
    const FortranLspDocument *doc = nullptr;
    int sym = -1;
    bool ok() const { return doc && sym >= 0; }
    const Symbol &s() const { return doc->syms[static_cast<size_t>(sym)]; }
};

constexpr int kMaxDepth = 16;         // USE chains
constexpr int kMaxInheritance = 128;  // EXTENDS chains

// The analysis holding module @p m as @p doc sees it (its own, or an import), and the module's scope there.
const FortranLspDocument *ModuleDoc(const FortranLspDocument &doc, const std::string &m, int *scope) {
    auto it = doc.modules.find(m);
    if (it != doc.modules.end()) {
        *scope = it->second;
        return &doc;
    }
    auto im = doc.imports.find(m);
    if (im == doc.imports.end() || !im->second) return nullptr;
    auto jt = im->second->modules.find(m);
    if (jt == im->second->modules.end()) return nullptr;
    *scope = jt->second;
    return im->second.get();
}

bool Exported(const FortranLspDocument &doc, const Symbol &s) {
    if (s.attrs & fp::kAttrPrivate) return false;
    if (s.attrs & fp::kAttrPublic) return true;
    return s.scope < 0 || !doc.scopes[static_cast<size_t>(s.scope)].default_private;
}

Target ExportOf(const FortranLspDocument &doc, const std::string &mod, const std::string &name, int depth);

// A USE-associated name: the entity it names in its module (itself when that is out of sight).
Target FollowUse(const FortranLspDocument &doc, int sym, int depth) {
    const Symbol &s = doc.syms[static_cast<size_t>(sym)];
    if (s.kind != SymKind::Use) return {&doc, sym};
    size_t colon = s.from.find(':');
    std::string mod = s.from.substr(0, colon);
    std::string orig = colon == std::string::npos ? s.name : s.from.substr(colon + 1);
    Target t = ExportOf(doc, mod, orig, depth + 1);
    return t.ok() ? t : Target{&doc, sym};
}

// What module @p mod (as @p doc sees it) makes available as @p name: its own public
// entities and those it USEs in turn, private ones excluded.
Target ExportOf(const FortranLspDocument &doc, const std::string &mod, const std::string &name, int depth) {
    if (depth > kMaxDepth) return {};
    int ms = -1;
    const FortranLspDocument *md = ModuleDoc(doc, mod, &ms);
    if (!md) return {};
    const Scope &sc = md->scopes[static_cast<size_t>(ms)];
    Target found;
    auto it = sc.names.find(name);
    if (it != sc.names.end()) {
        const Symbol &s = md->syms[static_cast<size_t>(it->second)];
        if (!Exported(*md, s)) return {};
        if (!s.placeholder) return FollowUse(*md, it->second, depth);
        found = {md, it->second};  // PUBLIC :: name, of something the module USEs
    }
    if (found.ok() || !sc.default_private) {
        for (const Scope::Use &u : sc.use_list) {
            if (u.only || u.renamed.count(name)) continue;
            Target t = ExportOf(*md, u.module, name, depth + 1);
            if (t.ok()) return t;
        }
    }
    return found;
}

// @p name as seen from @p scope of @p doc: host association, USE (of a module in this file or
// another), a submodule's parent, the file's global names.
Target LookupIn(const FortranLspDocument &doc, int scope, const std::string &name, int depth = 0) {
    if (depth > kMaxDepth) return {};
    for (int s = scope; s >= 0 && static_cast<size_t>(s) < doc.scopes.size();
         s = doc.scopes[static_cast<size_t>(s)].parent) {
        const Scope &sc = doc.scopes[static_cast<size_t>(s)];
        Target local;
        auto it = sc.names.find(name);
        if (it != sc.names.end()) {
            const Symbol &sym = doc.syms[static_cast<size_t>(it->second)];
            if (sym.kind == SymKind::Use) return FollowUse(doc, it->second, depth);
            local = {&doc, it->second};
            if (!sym.placeholder) return local;
        }
        for (const Scope::Use &u : sc.use_list) {
            if (u.only || u.renamed.count(name)) continue;
            Target t = ExportOf(doc, u.module, name, depth + 1);
            if (t.ok()) return t;
        }
        if (local.ok()) return local;
        if (sc.kind == fp::StmtKind::Submodule && sc.construct > 0) {
            const fp::Stmt &open =
                doc.src->stmts[static_cast<size_t>(doc.src->constructs[static_cast<size_t>(sc.construct)].open)];
            int ms = -1;
            const FortranLspDocument *md = ModuleDoc(doc, open.unit.parent, &ms);
            if (md && !(md == &doc && ms == s)) {
                Target t = LookupIn(*md, ms, name, depth + 1);
                if (t.ok()) return t;
            }
        }
        if (sc.parent < 0 && s != 0) {
            auto g = doc.scopes[0].names.find(name);
            return g == doc.scopes[0].names.end() ? Target{} : Target{&doc, g->second};
        }
    }
    return {};
}

// The names module @p mod exports, as @p doc sees it.
void ExportsOf(const FortranLspDocument &doc, const std::string &mod, int depth, std::set<std::string> *seen,
               const std::set<std::string> &hidden, std::vector<std::pair<std::string, Target>> *out) {
    if (depth > kMaxDepth) return;
    int ms = -1;
    const FortranLspDocument *md = ModuleDoc(doc, mod, &ms);
    if (!md) return;
    const Scope &sc = md->scopes[static_cast<size_t>(ms)];
    for (const auto &kv : sc.names) {
        const Symbol &s = md->syms[static_cast<size_t>(kv.second)];
        if (hidden.count(kv.first) || !Exported(*md, s) || seen->count(kv.first)) continue;
        Target t = s.placeholder ? ExportOf(doc, mod, kv.first, depth) : FollowUse(*md, kv.second, depth);
        if (!t.ok()) continue;
        seen->insert(kv.first);
        out->push_back({kv.first, t});
    }
    if (sc.default_private) return;
    for (const Scope::Use &u : sc.use_list) {
        if (u.only) continue;
        std::set<std::string> h = hidden;
        h.insert(u.renamed.begin(), u.renamed.end());
        ExportsOf(*md, u.module, depth + 1, seen, h, out);
    }
}

// The scope a symbol's names are looked up from (a component's: its type's).
int ScopeOfSym(const Target &t) {
    const Symbol &s = t.s();
    if (s.scope >= 0) return s.scope;
    if (s.owner >= 0) return t.doc->syms[static_cast<size_t>(s.owner)].scope;
    return 0;
}

// type(foo) / class(foo(k=4)) -> foo; "" for intrinsic types and class(*).
std::string TypeNameOf(const std::string &t) {
    size_t lp;
    if (t.compare(0, 5, "type(") == 0) lp = 4;
    else if (t.compare(0, 6, "class(") == 0) lp = 5;
    else return "";
    size_t a = lp + 1;
    while (a < t.size() && t[a] == ' ') a++;
    size_t b = a;
    while (b < t.size() && IsNameChar(t[b])) b++;
    return Lower(t.substr(a, b - a));
}

Target FindType(const FortranLspDocument &doc, int scope, const std::string &name) {
    if (name.empty()) return {};
    Target t = LookupIn(doc, scope, name);
    if (t.ok() && t.s().kind == SymKind::Type) return t;
    auto it = doc.types.find(name);
    if (it != doc.types.end()) return {&doc, it->second};
    for (const auto &im : doc.imports) {
        if (!im.second) continue;
        auto jt = im.second->types.find(name);
        if (jt != im.second->types.end()) return {im.second.get(), jt->second};
    }
    return {};
}

// A type's components and bindings, its own first, then those it inherits through EXTENDS
// (a binding overridden here hides the parent's).
void Members(const Target &type, std::vector<Target> *out, std::set<std::string> *seen, int depth) {
    if (!type.ok() || depth > kMaxInheritance) return;
    auto it = type.doc->components.find(type.sym);
    if (it != type.doc->components.end()) {
        for (int c : it->second) {
            if (seen->insert(type.doc->syms[static_cast<size_t>(c)].name).second) out->push_back({type.doc, c});
        }
    }
    const Symbol &ts = type.s();
    if (!ts.extends.empty()) {
        Target p = FindType(*type.doc, ScopeOfSym(type), ts.extends);
        if (p.ok() && !(p.doc == type.doc && p.sym == type.sym)) Members(p, out, seen, depth + 1);
    }
}

std::vector<Target> MembersOf(const Target &type) {
    std::vector<Target> out;
    std::set<std::string> seen;
    Members(type, &out, &seen, 0);
    return out;
}

Target MemberOf(const Target &type, const std::string &name) {
    for (const Target &m : MembersOf(type)) {
        if (m.s().name == name) return m;
    }
    return {};
}

// The derived type of a variable, component or function result.
Target TypeOf(const Target &v) {
    if (!v.ok()) return {};
    const Symbol &s = v.s();
    std::string tn = TypeNameOf(s.type);
    if (tn.empty() && s.kind == SymKind::Function && s.unit >= 0) {
        const Scope &u = v.doc->scopes[static_cast<size_t>(s.unit)];
        auto r = u.names.find(s.result.empty() ? s.name : s.result);
        if (r != u.names.end()) tn = TypeNameOf(v.doc->syms[static_cast<size_t>(r->second)].type);
    }
    return FindType(*v.doc, ScopeOfSym(v), tn);
}

// The names of `a%b(i)%c` ending at the `%` at byte @p pct, base first.
std::vector<std::string> ChainBefore(const std::string &l, int pct) {
    std::vector<std::string> names;
    int q = pct - 1;
    for (int guard = 0; guard < 64; guard++) {
        while (q >= 0 && l[static_cast<size_t>(q)] == ' ') q--;
        if (q >= 0 && l[static_cast<size_t>(q)] == ')') {
            int depth = 0;
            for (; q >= 0; q--) {
                if (l[static_cast<size_t>(q)] == ')') depth++;
                else if (l[static_cast<size_t>(q)] == '(' && --depth == 0) break;
            }
            q--;
            while (q >= 0 && l[static_cast<size_t>(q)] == ' ') q--;
        }
        int e = q + 1;
        while (q >= 0 && IsNameChar(l[static_cast<size_t>(q)])) q--;
        if (e - (q + 1) <= 0) return {};
        names.push_back(Lower(l.substr(static_cast<size_t>(q + 1), static_cast<size_t>(e - q - 1))));
        int r = q;
        while (r >= 0 && l[static_cast<size_t>(r)] == ' ') r--;
        if (r >= 0 && l[static_cast<size_t>(r)] == '%') {
            q = r - 1;
            continue;
        }
        break;
    }
    std::reverse(names.begin(), names.end());
    return names;
}

Target ResolveChain(const FortranLspDocument &doc, int line, const std::vector<std::string> &names) {
    if (names.empty()) return {};
    Target t = LookupIn(doc, ScopeAtLine(doc, line), names[0]);
    for (size_t i = 1; i < names.size() && t.ok(); i++) t = MemberOf(TypeOf(t), names[i]);
    return t;
}

// Whether byte @p col of @p l is inside a `!` comment.
bool InComment(const std::string &l, int col) {
    char q = 0;
    for (int i = 0; i < col && static_cast<size_t>(i) < l.size(); i++) {
        char c = l[static_cast<size_t>(i)];
        if (q) {
            if (c == q) q = 0;
        } else if (c == '\'' || c == '"') {
            q = c;
        } else if (c == '!') {
            return true;
        }
    }
    return false;
}

// The entity named at (line, col): a local declaration or reference, a module in USE, a
// component through a%b, or a name from a USEd module in another file.
Target TargetAt(const FortranLspDocument &doc, int line, int col, FortranLspRange *range) {
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return {};
    const std::string &l = doc.lines[static_cast<size_t>(line)];
    for (const auto &r : doc.refs) {
        if (r.line != line || col < r.col || col > r.end_col || r.ctx != fp::RefCtx::Module || r.sym >= 0) continue;
        auto im = doc.imports.find(r.name);
        if (im == doc.imports.end() || !im->second) continue;
        auto g = im->second->scopes[0].names.find(r.name);
        if (g == im->second->scopes[0].names.end()) continue;
        if (range) *range = {r.line, r.col, r.line, r.end_col};
        return {im->second.get(), g->second};
    }
    int sym = SymbolAt(doc, line, col, range);
    if (sym >= 0) return FollowUse(doc, sym, 0);
    int a = 0, b = 0;
    std::string w = Lower(WordAt(l, col, &a, &b));
    if (w.empty() || (w[0] >= '0' && w[0] <= '9') || InComment(l, a)) return {};
    if (range) *range = {line, a, line, b};
    int p = a - 1;
    while (p >= 0 && l[static_cast<size_t>(p)] == ' ') p--;
    if (p >= 0 && l[static_cast<size_t>(p)] == '%') return MemberOf(TypeOf(ResolveChain(doc, line, ChainBefore(l, p))), w);
    return LookupIn(doc, ScopeAtLine(doc, line), w);
}

// ---- FORD documentation ----

// A full-line comment opening with !<mark> (`!>` or `!!`); @p text gets what follows the marker.
bool DocLine(const std::string &l, char mark, std::string *text) {
    size_t k = l.find_first_not_of(" \t");
    if (k == std::string::npos || l.size() < k + 2 || l[k] != '!' || l[k + 1] != mark) return false;
    if (text) {
        std::string t = l.substr(k + 2);
        if (!t.empty() && t[0] == ' ') t.erase(0, 1);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\r' || t.back() == '\t')) t.pop_back();
        *text = t;
    }
    return true;
}

// The documentation lines of a symbol, markers stripped: the `!>` block directly above its
// statement (Doxygen's `!!` continuation lines included), then a trailing `!!`/`!<` on it and the
// `!!` lines directly below it.
std::vector<std::string> RawDoc(const FortranLspDocument &doc, const Symbol &s) {
    std::vector<std::string> out;
    if (s.stmt < 0 || static_cast<size_t>(s.stmt) >= doc.src->stmts.size()) return out;
    const fp::Stmt &st = doc.src->stmts[static_cast<size_t>(s.stmt)];
    int first = std::min(st.pos.line, st.label.empty() ? st.pos.line : st.label_pos.line);
    int last = std::max(st.end.line, first);
    int n = static_cast<int>(doc.lines.size());
    if (first < 0 || first >= n) return out;
    int i = first - 1;
    while (i >= 0 && (DocLine(doc.lines[static_cast<size_t>(i)], '>', nullptr) ||
                      DocLine(doc.lines[static_cast<size_t>(i)], '!', nullptr)))
        i--;
    int top = -1;
    for (int k = i + 1; k < first; k++) {
        if (DocLine(doc.lines[static_cast<size_t>(k)], '>', nullptr)) {
            top = k;  // leading `!!` lines belong to the statement above
            break;
        }
    }
    for (int k = top; k >= 0 && k < first; k++) {
        std::string t;
        if (!DocLine(doc.lines[static_cast<size_t>(k)], '>', &t)) DocLine(doc.lines[static_cast<size_t>(k)], '!', &t);
        out.push_back(t);
    }
    std::vector<std::string> after;
    // trailing: on the entity's own line when the statement spans several
    auto tr = doc.trailing_docs.end();
    int own = s.decl_line >= 0 ? s.decl_line : s.line;
    if (own >= first && own <= last && last > first) {
        tr = doc.trailing_docs.find(own);
    } else {
        for (int k = first; k <= last && tr == doc.trailing_docs.end(); k++) tr = doc.trailing_docs.find(k);
    }
    if (tr != doc.trailing_docs.end()) {
        std::string t = tr->second.substr(2);
        if (!t.empty() && t[0] == ' ') t.erase(0, 1);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\r')) t.pop_back();
        after.push_back(t);
    }
    for (int k = last + 1; k < n && k <= last + 200; k++) {
        std::string t;
        if (!DocLine(doc.lines[static_cast<size_t>(k)], '!', &t)) break;
        after.push_back(t);
    }
    if (!out.empty() && !after.empty()) out.push_back("");
    out.insert(out.end(), after.begin(), after.end());
    while (!out.empty() && out.back().empty()) out.pop_back();
    while (!out.empty() && out.front().empty()) out.erase(out.begin());
    return out;
}

std::string Trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

// `@p name` / `\c name` / `@a name` -> `name`
std::string InlineTags(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if ((s[i] == '@' || s[i] == '\\') && i + 2 < s.size() && (s[i + 1] == 'p' || s[i + 1] == 'c' || s[i + 1] == 'a') &&
            s[i + 2] == ' ' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '(')) {
            size_t a = i + 3;
            size_t b = a;
            while (b < s.size() && IsNameChar(s[b])) b++;
            if (b > a) {
                out += "`" + s.substr(a, b - a) + "`";
                i = b - 1;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

struct ParsedDoc {
    std::string body;                                         // markdown
    std::vector<std::pair<std::string, std::string>> params;  // lower-case name, text
    std::string returns;
    bool empty() const { return body.empty() && params.empty() && returns.empty(); }
};

// FORD documentation is Markdown already; Doxygen's tags (as converted docs carry them) become
// Markdown too: @brief/@details are dropped, @param/@return gathered, @note & co. labelled.
ParsedDoc ParseDoc(const std::vector<std::string> &raw) {
    ParsedDoc d;
    std::vector<std::string> body;
    enum { kBody, kParam, kReturn } mode = kBody;
    static const std::pair<const char *, const char *> kLabels[] = {
        {"note", "Note"},       {"warning", "Warning"},   {"attention", "Attention"}, {"see", "See also"},
        {"sa", "See also"},     {"deprecated", "Deprecated"}, {"since", "Since"},     {"todo", "To do"},
        {"throws", "Throws"},   {"throw", "Throws"},      {"exception", "Throws"},    {"pre", "Precondition"},
        {"post", "Postcondition"}, {"author", "Author"},  {"version", "Version"},     {"bug", "Bug"},
        {"remark", "Remark"},   {"remarks", "Remark"},    {"example", "Example"}};
    auto paragraph = [&]() {
        if (!body.empty() && !body.back().empty()) body.push_back("");
    };
    for (const std::string &line : raw) {
        std::string t = Trim(line);
        if (t.size() > 1 && (t[0] == '@' || t[0] == '\\') && ((t[1] >= 'a' && t[1] <= 'z') || (t[1] >= 'A' && t[1] <= 'Z'))) {
            size_t e = 1;
            while (e < t.size() && IsNameChar(t[e])) e++;
            std::string tag = Lower(t.substr(1, e - 1));
            std::string rest = t.substr(e);
            if (tag == "param" || tag == "tparam" || tag == "arg") {
                rest = Trim(rest);
                if (!rest.empty() && rest[0] == '[') {
                    size_t c = rest.find(']');
                    rest = Trim(c == std::string::npos ? "" : rest.substr(c + 1));
                }
                size_t ne = 0;
                while (ne < rest.size() && IsNameChar(rest[ne])) ne++;
                if (ne > 0) {
                    d.params.push_back({Lower(rest.substr(0, ne)), InlineTags(Trim(rest.substr(ne)))});
                    mode = kParam;
                    continue;
                }
            } else if (tag == "return" || tag == "returns" || tag == "result" || tag == "retval") {
                d.returns = InlineTags(Trim(rest));
                mode = kReturn;
                continue;
            } else if (tag == "brief" || tag == "short" || tag == "summary") {
                if (!Trim(rest).empty()) body.push_back(InlineTags(Trim(rest)));
                mode = kBody;
                continue;
            } else if (tag == "details") {
                paragraph();
                if (!Trim(rest).empty()) body.push_back(InlineTags(Trim(rest)));
                mode = kBody;
                continue;
            } else if (tag == "code") {
                body.push_back("```fortran");
                mode = kBody;
                continue;
            } else if (tag == "endcode") {
                body.push_back("```");
                continue;
            } else {
                const char *label = nullptr;
                for (const auto &kl : kLabels) {
                    if (tag == kl.first) label = kl.second;
                }
                if (label) {
                    paragraph();
                    body.push_back(std::string("**") + label + ":** " + InlineTags(Trim(rest)));
                    mode = kBody;
                    continue;
                }
            }
        }
        if (mode != kBody) {
            if (t.empty()) {
                mode = kBody;
                continue;
            }
            std::string &to = mode == kParam ? d.params.back().second : d.returns;
            to += (to.empty() ? "" : " ") + InlineTags(t);
            continue;
        }
        body.push_back(InlineTags(line));
    }
    while (!body.empty() && Trim(body.back()).empty()) body.pop_back();
    while (!body.empty() && Trim(body.front()).empty()) body.erase(body.begin());
    for (size_t i = 0; i < body.size(); i++) d.body += (i ? "\n" : "") + body[i];
    return d;
}

ParsedDoc DocOf(const Target &t) { return ParseDoc(RawDoc(*t.doc, t.s())); }

// The first paragraph, on one line.
std::string Summary(const std::string &body) {
    std::string out;
    size_t i = 0;
    while (i < body.size()) {
        size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        std::string l = Trim(body.substr(i, e - i));
        if (l.empty()) break;
        if (l.compare(0, 3, "```") == 0) break;
        out += (out.empty() ? "" : " ") + l;
        i = e + 1;
    }
    return out;
}

// Dimensions and initialiser written after an entity's name: `x(:, n) = 3`.
void EntityTail(const FortranLspDocument &doc, const Symbol &s, std::string *dims, std::string *init) {
    int line = s.decl_line >= 0 ? s.decl_line : s.line;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return;
    const std::string &l = doc.lines[static_cast<size_t>(line)];
    size_t k = static_cast<size_t>(std::max(0, s.decl_line >= 0 ? s.decl_end : s.end_col));
    if (k > l.size()) return;
    auto blanks = [&]() {
        while (k < l.size() && (l[k] == ' ' || l[k] == '\t')) k++;
    };
    blanks();
    for (char open : {'(', '['}) {
        if (k < l.size() && l[k] == open) {
            char close = open == '(' ? ')' : ']';
            int depth = 0;
            size_t a = k;
            for (; k < l.size(); k++) {
                if (l[k] == open) depth++;
                else if (l[k] == close && --depth == 0) break;
            }
            if (k >= l.size()) return;
            if (open == '(') *dims = l.substr(a, k - a + 1);
            k++;
            blanks();
        }
    }
    if (k < l.size() && l[k] == '*') return;  // character*len
    if (k + 1 < l.size() && l[k] == '=' && l[k + 1] != '=') {
        bool arrow = l[k + 1] == '>';
        k += arrow ? 2 : 1;
        size_t a = k;
        int depth = 0;
        char q = 0;
        for (; k < l.size(); k++) {
            char c = l[k];
            if (q) {
                if (c == q) q = 0;
                continue;
            }
            if (c == '\'' || c == '"') q = c;
            else if (c == '(' || c == '[') depth++;
            else if (c == ')' || c == ']') depth--;
            else if ((c == ',' && depth == 0) || c == '!' || c == '&') break;
        }
        std::string v = Trim(l.substr(a, k - a));
        if (k < l.size() && l[k] == '&') v += " ...";
        if (v.size() > 120) v = v.substr(0, 117) + "...";
        if (!v.empty()) *init = (arrow ? "=> " : "= ") + v;
    }
}

// `real(dp), intent(in), optional :: x(:)`, `integer, parameter :: red = 1`
std::string DeclLine(const Target &t) {
    const Symbol &s = t.s();
    std::string out = SymbolDetail(*t.doc, s);
    if (s.kind == SymKind::Function || s.kind == SymKind::Subroutine || s.kind == SymKind::Use) return out;
    std::string dims, init;
    EntityTail(*t.doc, s, &dims, &init);
    if (!dims.empty() && out.size() >= 5 && out.compare(out.size() - 5, 5, "(...)") == 0) {
        out.replace(out.size() - 5, 5, dims);
    } else if (!dims.empty() && !s.array) {
        out += dims;
    }
    if (!init.empty() && s.kind != SymKind::Dummy) out += " " + init;
    return out;
}

std::string ProcHeader(const Target &t) {
    const Symbol &s = t.s();
    if (s.detail == "module procedure") return "module procedure " + s.spelling;
    std::string h = s.prefix.empty() ? "" : s.prefix + " ";
    if (!s.type.empty() && s.kind == SymKind::Function) h += s.type + " ";
    h += s.kind == SymKind::Subroutine ? "subroutine " : s.kind == SymKind::Entry ? "entry " : "function ";
    h += s.spelling + "(";
    for (size_t i = 0; i < s.dummies.size(); i++) h += (i ? ", " : "") + s.dummies[i];
    h += ")";
    if (!s.result.empty()) h += " result(" + s.result + ")";
    return h;
}

Target DummyOf(const Target &proc, const std::string &name) {
    const Symbol &s = proc.s();
    if (s.unit < 0) return {};
    const Scope &u = proc.doc->scopes[static_cast<size_t>(s.unit)];
    auto it = u.names.find(Lower(name));
    return it == u.names.end() ? Target{} : Target{proc.doc, it->second};
}

// The procedure statement and its dummies' declarations.
std::string ProcDecl(const Target &t) {
    const Symbol &s = t.s();
    std::string out = ProcHeader(t);
    if (s.unit < 0) return out;
    int shown = 0;
    for (const std::string &d : s.dummies) {
        Target dt = DummyOf(t, d);
        if (!dt.ok() || !dt.s().declared) continue;
        if (++shown > 40) {
            out += "\n    ! ...";
            break;
        }
        out += "\n    " + DeclLine(dt);
    }
    if (s.kind == SymKind::Function && s.type.empty()) {
        Target r = DummyOf(t, s.result.empty() ? s.name : s.result);
        if (r.ok() && r.s().declared) out += "\n    " + DeclLine(r);
    }
    return out;
}

std::string BindingLine(const Target &t) {
    const Symbol &s = t.s();
    if (s.detail == "generic binding") {
        std::string out = "generic :: " + s.spelling + " => ";
        for (size_t i = 0; i < s.specifics.size(); i++) out += (i ? ", " : "") + s.specifics[i];
        return out;
    }
    std::string out = s.type.compare(0, 10, "procedure(") == 0 ? s.type : "procedure";
    if (s.pass == "-") out += ", nopass";
    else if (!s.pass.empty()) out += ", pass(" + s.pass + ")";
    if (s.detail == "deferred") out += ", deferred";
    out += " :: " + s.spelling;
    if (s.detail != "deferred" && s.target != s.name && !s.target.empty()) out += " => " + s.target;
    return out;
}

std::string TypeBlock(const Target &t) {
    const Symbol &s = t.s();
    std::string out = "type";
    if (s.detail == "abstract") out += ", abstract";
    if (!s.extends.empty()) out += ", extends(" + s.extends + ")";
    out += " :: " + s.spelling;
    std::vector<Target> comps, binds;
    auto it = t.doc->components.find(t.sym);
    if (it != t.doc->components.end()) {
        for (int c : it->second) {
            Target ct{t.doc, c};
            (ct.s().kind == SymKind::Binding ? binds : comps).push_back(ct);
        }
    }
    int shown = 0;
    for (const Target &c : comps) {
        if (++shown > 40) break;
        out += "\n    " + DeclLine(c);
    }
    if (!binds.empty()) out += "\ncontains";
    for (const Target &b : binds) {
        if (++shown > 40) break;
        out += "\n    " + BindingLine(b);
    }
    if (shown > 40) out += "\n    ! ... " + std::to_string(comps.size() + binds.size() - 40) + " more";
    out += "\nend type " + s.spelling;
    return out;
}

// The procedure a generic's specific names, or a binding binds.
Target ProcNamed(const Target &from, const std::string &name) {
    Target p = LookupIn(*from.doc, ScopeOfSym(from), name);
    return p;
}

// The dummy a binding passes the object as ("" for none).
std::string PassedDummy(const Target &binding, const Target &proc) {
    const std::string &pass = binding.s().pass;
    if (pass == "-") return "";
    if (!pass.empty()) return pass;
    return proc.ok() && !proc.s().dummies.empty() ? Lower(proc.s().dummies[0]) : "";
}

std::string Where(const Target &t, const FortranLspDocument &from) {
    const Symbol &s = t.s();
    std::string what = s.kind == SymKind::Function && s.detail == "statement function" ? "statement function" : KindWord(s.kind);
    if (s.kind == SymKind::Binding && s.detail == "generic binding") what = "generic type-bound procedure";
    if (t.doc == &from) return what + ", declared on line " + std::to_string(s.line + 1);
    std::string mod;
    for (int sc = ScopeOfSym(t); sc > 0; sc = t.doc->scopes[static_cast<size_t>(sc)].parent) {
        const Scope &scope = t.doc->scopes[static_cast<size_t>(sc)];
        if (scope.kind == fp::StmtKind::Module || scope.kind == fp::StmtKind::Submodule) {
            mod = scope.name;
            break;
        }
    }
    std::string file = t.doc->path.substr(t.doc->path.find_last_of('/') == std::string::npos ? 0 : t.doc->path.find_last_of('/') + 1);
    std::string out = what;
    if (!mod.empty() && s.kind != SymKind::Module) out += " from module `" + mod + "`";
    return out + ", " + file + ":" + std::to_string(s.line + 1);
}

// The arguments documented by @param lines or by docs on the dummies' declarations.
std::string ArgumentDocs(const Target &proc, const ParsedDoc &pd, const std::string &skip) {
    const Symbol &s = proc.s();
    std::string out;
    for (const std::string &d : s.dummies) {
        std::string ld = Lower(d);
        if (ld == skip || ld == "*") continue;
        std::string desc;
        for (const auto &p : pd.params) {
            if (p.first == ld) desc = p.second;
        }
        Target dt = DummyOf(proc, d);
        if (desc.empty() && dt.ok()) desc = ParseDoc(RawDoc(*dt.doc, dt.s())).body;
        bool optional = dt.ok() && (dt.s().attrs & fp::kAttrOptional);
        if (desc.empty() && !optional) continue;
        for (char &c : desc) {
            if (c == '\n') c = ' ';
        }
        out += "\n- `" + d + "`" + (optional ? " *(optional)*" : "") + (desc.empty() ? "" : ": " + desc);
    }
    std::string ret = pd.returns;
    if (ret.empty() && s.kind == SymKind::Function) {
        Target r = DummyOf(proc, s.result.empty() ? s.name : s.result);
        if (r.ok() && r.sym != proc.sym) ret = Summary(ParseDoc(RawDoc(*r.doc, r.s())).body);
    }
    std::string res;
    if (!out.empty()) res = "**Arguments**\n" + out;
    if (!ret.empty()) res += std::string(res.empty() ? "" : "\n\n") + "**Returns:** " + ret;
    return res;
}

std::string Join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + "\n\n" + b;
}

bool IsProc(const Target &t) {
    return t.ok() && (t.s().kind == SymKind::Function || t.s().kind == SymKind::Subroutine || t.s().kind == SymKind::Entry);
}

// The documentation of a symbol, as Markdown: its own text, then its arguments; a binding
// without docs of its own shows its procedure's; a generic lists its specifics.
std::string DocsOf(const Target &t, bool list_specifics = true) {
    if (!t.ok()) return "";
    const Symbol &s = t.s();
    ParsedDoc pd = DocOf(t);
    switch (s.kind) {
        case SymKind::Function:
        case SymKind::Subroutine:
        case SymKind::Entry:
            return Join(pd.body, ArgumentDocs(t, pd, ""));
        case SymKind::Binding: {
            if (s.detail == "generic binding") {
                std::string out = pd.body;
                if (!list_specifics || s.owner < 0) return out;
                std::string list;
                Target owner{t.doc, s.owner};
                for (const std::string &spec : s.specifics) {
                    Target b = MemberOf(owner, spec);
                    Target p = b.ok() ? ProcNamed(b, b.s().target) : Target{};
                    std::string sum = b.ok() ? Summary(DocOf(b).body) : "";
                    if (sum.empty() && p.ok()) sum = Summary(DocOf(p).body);
                    list += "\n- `" + spec + "`" + (sum.empty() ? "" : ": " + sum);
                }
                return Join(out, list.empty() ? "" : "**Specific bindings**\n" + list);
            }
            Target p = ProcNamed(t, s.target);
            if (!IsProc(p)) return pd.body;
            ParsedDoc ppd = DocOf(p);
            std::string body = pd.body.empty() ? ppd.body : pd.body;
            return Join(body, ArgumentDocs(p, pd.params.empty() && pd.returns.empty() ? ppd : pd, PassedDummy(t, p)));
        }
        case SymKind::Interface:
        case SymKind::Type: {
            std::string out = pd.body;
            if (!list_specifics || s.specifics.empty()) return out;
            std::string list;
            for (const std::string &spec : s.specifics) {
                Target p = ProcNamed(t, spec);
                std::string head = IsProc(p) ? p.s().spelling + "(" : spec;
                if (IsProc(p)) {
                    for (size_t i = 0; i < p.s().dummies.size(); i++) head += (i ? ", " : "") + p.s().dummies[i];
                    head += ")";
                }
                std::string sum = IsProc(p) ? Summary(DocOf(p).body) : "";
                list += "\n- `" + head + "`" + (sum.empty() ? "" : ": " + sum);
            }
            return Join(out, (s.kind == SymKind::Type ? "**Constructors**\n" : "**Specific procedures**\n") + list);
        }
        case SymKind::Dummy: {
            if (!pd.body.empty() || s.scope <= 0) return pd.body;
            // documented by an @param of its procedure
            const Scope &unit = t.doc->scopes[static_cast<size_t>(s.scope)];
            if (unit.parent < 0) return pd.body;
            auto it = t.doc->scopes[static_cast<size_t>(unit.parent)].names.find(unit.name);
            if (it == t.doc->scopes[static_cast<size_t>(unit.parent)].names.end()) return pd.body;
            for (const auto &p : DocOf({t.doc, it->second}).params) {
                if (p.first == s.name) return p.second;
            }
            return pd.body;
        }
        default:
            return pd.body;
    }
}

// The declaration shown in a hover's code block.
std::string DeclText(const Target &t) {
    const Symbol &s = t.s();
    switch (s.kind) {
        case SymKind::Function:
        case SymKind::Subroutine:
        case SymKind::Entry:
            return s.unit >= 0 || !s.dummies.empty() || s.kind == SymKind::Entry ? ProcDecl(t) : DeclLine(t);
        case SymKind::Interface: {
            std::string out = "interface " + s.spelling;
            size_t shown = 0;
            for (const std::string &spec : s.specifics) {
                if (++shown > 50) {
                    out += "\n    ! ... " + std::to_string(s.specifics.size() - 50) + " more";
                    break;
                }
                Target p = ProcNamed(t, spec);
                out += "\n    " + (IsProc(p) ? ProcHeader(p) : "module procedure " + spec);
            }
            return out + "\nend interface " + s.spelling;
        }
        case SymKind::Type:
            return TypeBlock(t);
        case SymKind::Binding: {
            std::string out = BindingLine(t);
            if (s.detail == "generic binding") return out;
            Target p = ProcNamed(t, s.target);
            if (IsProc(p)) out += "\n\n" + ProcDecl(p);
            return out;
        }
        case SymKind::Module:
            return "module " + s.spelling;
        case SymKind::Program:
            return "program " + s.spelling;
        default:
            return DeclLine(t);
    }
}

// A one-line description, for completion details.
std::string OneLine(const Target &t) {
    const Symbol &s = t.s();
    switch (s.kind) {
        case SymKind::Function:
        case SymKind::Subroutine:
        case SymKind::Entry:
            return s.unit >= 0 || !s.dummies.empty() ? ProcHeader(t) : DeclLine(t);
        case SymKind::Interface:
            return "interface " + s.spelling + " (" + std::to_string(s.specifics.size()) + " specific" +
                   (s.specifics.size() == 1 ? "" : "s") + ")";
        case SymKind::Type:
            return "type" + std::string(s.extends.empty() ? "" : ", extends(" + s.extends + ")") + " :: " + s.spelling;
        case SymKind::Binding:
            return BindingLine(t);
        case SymKind::Module:
            return "module " + s.spelling;
        default:
            return DeclLine(t);
    }
}

FortranLspKind ItemKind(const Symbol &s) {
    switch (s.kind) {
        case SymKind::Function:
        case SymKind::Subroutine:
        case SymKind::Entry:
        case SymKind::Interface:
            return FortranLspKind::Function;
        case SymKind::Module: return FortranLspKind::Module;
        case SymKind::Type: return FortranLspKind::Struct;
        case SymKind::Parameter: return FortranLspKind::Constant;
        case SymKind::Binding: return FortranLspKind::Method;
        case SymKind::Component: return FortranLspKind::Field;
        default: return FortranLspKind::Variable;
    }
}

FortranLspCompletionItem TargetItem(const std::string &label, const Target &t, int a, int b) {
    return Item(label, ItemKind(t.s()), OneLine(t), DocsOf(t, false), a, b);
}

// One signature of a procedure, called as @p name; @p skip is the dummy a binding passes.
FortranLspSignature SigOf(const Target &proc, const std::string &name, const std::string &skip, const std::string &note,
                          const ParsedDoc *override_doc = nullptr) {
    FortranLspSignature sig;
    const Symbol &s = proc.s();
    ParsedDoc pd = override_doc && !override_doc->empty() ? *override_doc : DocOf(proc);
    sig.label = name + "(";
    bool any = false;
    for (const std::string &d : s.dummies) {
        std::string ld = Lower(d);
        if (ld == skip || d == "*") continue;
        if (any) sig.label += ", ";
        any = true;
        sig.label += d;
        sig.params.push_back(d);
        Target dt = DummyOf(proc, d);
        std::string desc;
        for (const auto &p : pd.params) {
            if (p.first == ld) desc = p.second;
        }
        if (desc.empty() && dt.ok()) desc = ParseDoc(RawDoc(*dt.doc, dt.s())).body;
        std::string pdoc = dt.ok() && dt.s().declared ? "`" + DeclLine(dt) + "`" : "";
        sig.param_docs.push_back(Join(pdoc, desc));
    }
    sig.label += ")";
    std::string docs = Join(pd.body, ArgumentDocs(proc, pd, skip));
    if (docs.empty()) docs = KindWord(s.kind);
    sig.documentation = Join(note, docs);
    return sig;
}

// The components a structure constructor takes, parents' first.
void ConstructorComponents(const Target &type, std::vector<Target> *out, int depth) {
    if (!type.ok() || depth > kMaxInheritance) return;
    const Symbol &ts = type.s();
    if (!ts.extends.empty()) {
        Target p = FindType(*type.doc, ScopeOfSym(type), ts.extends);
        if (p.ok() && !(p.doc == type.doc && p.sym == type.sym)) ConstructorComponents(p, out, depth + 1);
    }
    auto it = type.doc->components.find(type.sym);
    if (it == type.doc->components.end()) return;
    for (int c : it->second) {
        if (type.doc->syms[static_cast<size_t>(c)].kind == SymKind::Component) out->push_back({type.doc, c});
    }
}

// Every signature a call of @p t, written as @p called (a rename's local name), can have;
// @p via is the type of the object for a binding.
std::vector<FortranLspSignature> SignaturesOf(const Target &t, const Target &via, const std::string &called) {
    std::vector<FortranLspSignature> out;
    if (!t.ok()) return out;
    const Symbol &s = t.s();
    const std::string &name = called.empty() ? s.spelling : called;
    if (IsProc(t)) {
        out.push_back(SigOf(t, name, "", ""));
    } else if (s.kind == SymKind::Interface || (s.kind == SymKind::Type && !s.specifics.empty())) {
        // each specific's own documentation, after the generic's summary
        std::string summary = s.kind == SymKind::Interface ? Summary(DocOf(t).body) : "";
        for (const std::string &spec : s.specifics) {
            Target p = ProcNamed(t, spec);
            if (IsProc(p))
                out.push_back(SigOf(p, name, "", Join(summary, "*specific procedure `" + p.s().spelling + "`*")));
        }
    } else if (s.kind == SymKind::Type) {
        FortranLspSignature sig;
        sig.label = name + "(";
        std::vector<Target> comps;
        ConstructorComponents(t, &comps, 0);
        for (size_t i = 0; i < comps.size(); i++) {
            if (i) sig.label += ", ";
            sig.label += comps[i].s().spelling;
            sig.params.push_back(comps[i].s().spelling);
            sig.param_docs.push_back(Join("`" + DeclLine(comps[i]) + "`", DocOf(comps[i]).body));
        }
        sig.label += ")";
        sig.documentation = Join("*structure constructor*", DocOf(t).body);
        out.push_back(sig);
    } else if (s.kind == SymKind::Binding) {
        Target type = via.ok() ? via : Target{t.doc, s.owner};
        if (s.detail == "generic binding") {
            std::string summary = Summary(DocOf(t).body);
            for (const std::string &spec : s.specifics) {
                Target b = MemberOf(type, spec);
                if (!b.ok()) continue;
                Target p = ProcNamed(b, b.s().target);
                if (!IsProc(p)) continue;
                ParsedDoc bd = DocOf(b);
                out.push_back(SigOf(p, name, PassedDummy(b, p), Join(summary, "*binding `" + b.s().spelling + "`*"), &bd));
            }
        } else {
            Target p = ProcNamed(t, s.target);
            ParsedDoc bd = DocOf(t);
            if (IsProc(p)) out.push_back(SigOf(p, name, PassedDummy(t, p), "", &bd));
        }
    }
    return out;
}

}  // namespace

// ---- public API --------------------------------------------------------------------------------

std::shared_ptr<const FortranLspDocument> FortranLspAnalyze(const std::vector<std::string> &lines,
                                                            const std::string &path,
                                                            FortranLspModuleSource *modules) {
    auto doc = std::make_shared<FortranLspDocument>();
    doc->lines = lines;
    doc->path = path;
    fp::ReadOptions o;
    o.form = fp::FormForPath(path);
    o.preprocess = fp::PreprocessedPath(path);
    doc->src = fp::Read(lines, o);
    Builder b(doc.get());
    b.modules_ = modules;
    b.Run();
    if (modules) doc->known_modules = modules->ModuleNames();
    return doc;
}

std::shared_ptr<const FortranLspDocument> FortranLspAnalyze(const std::vector<std::string> &lines,
                                                            const std::string &path) {
    return FortranLspAnalyze(lines, path, nullptr);
}

const std::string &FortranLspPath(const FortranLspDocument &doc) { return doc.path; }

std::vector<std::string> FortranLspModulesDefined(const FortranLspDocument &doc) {
    std::vector<std::string> out;
    for (const auto &m : doc.modules) out.push_back(m.first);
    return out;
}

std::vector<std::pair<std::string, std::shared_ptr<const FortranLspDocument>>> FortranLspImports(
    const FortranLspDocument &doc) {
    return {doc.imports.begin(), doc.imports.end()};
}

std::vector<std::string> FortranLspScanModuleNames(const std::string &text) {
    std::vector<std::string> out;
    size_t i = 0, n = text.size();
    while (i < n) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = n;
        size_t k = i;
        while (k < e && (text[k] == ' ' || text[k] == '\t')) k++;
        if (e - k > 7 && (text[k] | 0x20) == 'm' && Lower(text.substr(k, 6)) == "module" &&
            (text[k + 6] == ' ' || text[k + 6] == '\t')) {
            size_t a = k + 6;
            while (a < e && (text[a] == ' ' || text[a] == '\t')) a++;
            size_t b = a;
            while (b < e && IsNameChar(text[b])) b++;
            size_t r = b;
            while (r < e && (text[r] == ' ' || text[r] == '\t' || text[r] == '\r')) r++;
            std::string name = Lower(text.substr(a, b - a));
            if (b > a && (r == e || text[r] == '!') && name != "procedure" && !(name[0] >= '0' && name[0] <= '9') &&
                std::find(out.begin(), out.end(), name) == out.end())
                out.push_back(name);
        }
        i = e + 1;
    }
    return out;
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
    // components and bindings after `%`, inherited ones included
    int p = a - 1;
    while (p >= 0 && (l[static_cast<size_t>(p)] == ' ')) p--;
    if (p >= 0 && l[static_cast<size_t>(p)] == '%') {
        Target type = TypeOf(ResolveChain(doc, line, ChainBefore(l, p)));
        for (const Target &m : MembersOf(type)) {
            if (!want(m.s().spelling)) continue;
            out.push_back(TargetItem(m.s().spelling, m, a, b));
        }
        return out;
    }
    // after `use`: module names
    std::string before = Lower(l.substr(0, static_cast<size_t>(a)));
    size_t k = before.find_first_not_of(" \t");
    bool use_line = k != std::string::npos && before.compare(k, 3, "use") == 0 &&
                    (before.size() == k + 3 || !IsNameChar(before[k + 3]));
    if (use_line && before.find(',') != std::string::npos) {
        // use m, only: x -- what m exports
        size_t m0 = k + 3;
        while (m0 < before.size() && (before[m0] == ' ' || before[m0] == ':')) m0++;
        size_t m1 = m0;
        while (m1 < before.size() && IsNameChar(before[m1])) m1++;
        std::string mod = before.substr(m0, m1 - m0);
        size_t arrow = before.rfind("=>");
        size_t comma = before.rfind(',');
        if (arrow != std::string::npos && arrow > comma) {
            // local => |: the module's name
        } else if (!mod.empty()) {
            std::set<std::string> seen_exports;
            std::vector<std::pair<std::string, Target>> exports;
            ExportsOf(doc, mod, 0, &seen_exports, {}, &exports);
            for (const auto &e : exports) {
                if (want(e.second.s().spelling)) out.push_back(TargetItem(e.second.s().spelling, e.second, a, b));
            }
            if (!exports.empty()) return out;
        }
    }
    if (use_line && before.find(',') == std::string::npos) {
        for (const auto &m : doc.modules) {
            if (want(m.first)) out.push_back(Item(m.first, FortranLspKind::Module, "module", "", a, b));
        }
        for (const std::string &m : doc.known_modules) {
            if (want(m)) out.push_back(Item(m, FortranLspKind::Module, "module", "", a, b));
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
            if (sym.kind == SymKind::Use) {
                // a name from USE ..., ONLY: (or a rename): what it names in its module
                Target t = FollowUse(doc, kv.second, 0);
                if (t.sym != kv.second || t.doc != &doc) {
                    FortranLspCompletionItem it = TargetItem(sym.spelling, t, a, b);
                    out.push_back(it);
                    continue;
                }
            }
            FortranLspKind kind = sym.kind == SymKind::Function || sym.kind == SymKind::Subroutine ? FortranLspKind::Function
                                  : sym.kind == SymKind::Module ? FortranLspKind::Module
                                  : sym.kind == SymKind::Type ? FortranLspKind::Struct
                                  : sym.kind == SymKind::Parameter ? FortranLspKind::Constant
                                  : sym.kind == SymKind::Interface ? FortranLspKind::Function
                                                                   : FortranLspKind::Variable;
            out.push_back(Item(sym.spelling, kind, SymbolDetail(doc, sym), DocsOf({&doc, kv.second}, false), a, b));
        }
        // modules from other files USEd here without ONLY: everything they export
        for (const Scope::Use &u : doc.scopes[static_cast<size_t>(s)].use_list) {
            if (u.only || doc.modules.count(u.module) || !doc.imports.count(u.module)) continue;
            std::vector<std::pair<std::string, Target>> exports;
            std::set<std::string> seen_exports;
            ExportsOf(doc, u.module, 0, &seen_exports, u.renamed, &exports);
            for (const auto &e : exports) {
                if (want(e.second.s().spelling)) out.push_back(TargetItem(e.second.s().spelling, e.second, a, b));
            }
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
    Target t = TargetAt(doc, line, col, &range);
    if (t.ok()) {
        h.found = true;
        h.range = range;
        h.text = "```fortran\n" + DeclText(t) + "\n```\n" + Join(DocsOf(t), Where(t, doc));
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
    // the argument being written: `name = ...` picks that dummy
    std::string keyword;
    {
        int arg_start = i + 1;
        int d = 0;
        for (int q = std::min(col, static_cast<int>(l.size())) - 1; q > i; q--) {
            char c = l[static_cast<size_t>(q)];
            if (c == ')') d++;
            else if (c == '(') d--;
            else if (c == ',' && d == 0) {
                arg_start = q + 1;
                break;
            }
        }
        std::string arg = l.substr(static_cast<size_t>(arg_start), static_cast<size_t>(std::max(0, col - arg_start)));
        size_t k0 = arg.find_first_not_of(" \t");
        if (k0 != std::string::npos && IsNameChar(arg[k0])) {
            size_t k1 = k0;
            while (k1 < arg.size() && IsNameChar(arg[k1])) k1++;
            size_t k2 = k1;
            while (k2 < arg.size() && arg[k2] == ' ') k2++;
            if (k2 < arg.size() && arg[k2] == '=' && (k2 + 1 >= arg.size() || arg[k2 + 1] != '='))
                keyword = Lower(arg.substr(k0, k1 - k0));
        }
    }
    // a procedure, generic, type or binding of this file or a USEd module
    Target t, via;
    {
        int p = a - 1;
        while (p >= 0 && l[static_cast<size_t>(p)] == ' ') p--;
        if (p >= 0 && l[static_cast<size_t>(p)] == '%') {
            via = TypeOf(ResolveChain(doc, line, ChainBefore(l, p)));
            t = MemberOf(via, name);
        } else {
            t = LookupIn(doc, ScopeAtLine(doc, line), name);
        }
    }
    std::vector<FortranLspSignature> sigs = SignaturesOf(t, via, l.substr(static_cast<size_t>(a), name.size()));
    if (!sigs.empty()) {
        auto index_of = [](const FortranLspSignature &sg, const std::string &kw) {
            for (size_t k = 0; k < sg.params.size(); k++) {
                if (Lower(sg.params[k]) == kw) return static_cast<int>(k);
            }
            return -1;
        };
        int active = -1;
        for (size_t k = 0; k < sigs.size() && active < 0; k++) {
            if (keyword.empty() ? static_cast<int>(sigs[k].params.size()) > commas : index_of(sigs[k], keyword) >= 0)
                active = static_cast<int>(k);
        }
        if (active < 0) active = 0;
        const FortranLspSignature &sg = sigs[static_cast<size_t>(active)];
        sh.found = true;
        sh.signatures = sigs;
        sh.active_signature = active;
        sh.label = sg.label;
        sh.params = sg.params;
        sh.documentation = sg.documentation;
        int kw = keyword.empty() ? -1 : index_of(sg, keyword);
        sh.active_param = kw >= 0 ? kw : std::min(commas, std::max(0, static_cast<int>(sh.params.size()) - 1));
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
    FortranLspSignature sg;
    sg.label = sh.label;
    sg.params = sh.params;
    sg.documentation = sh.documentation;
    sh.signatures.push_back(sg);
    return sh;
}

std::vector<FortranLspLocation> FortranLspDefinition(const FortranLspDocument &doc, int line, int col) {
    std::vector<FortranLspLocation> out;
    Target t = TargetAt(doc, line, col, nullptr);
    if (!t.ok()) return out;
    const Symbol &s = t.s();
    FortranLspLocation loc;
    loc.range = {s.line, s.col, s.line, s.end_col};
    if (t.doc != &doc) loc.path = t.doc->path;
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
