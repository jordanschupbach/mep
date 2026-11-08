// mep's Go language server, analysis half: see go_lsp.h. This file parses
// (go_parse.cpp, go/parser's errors verbatim), resolves names through Go's
// blocks, and runs the checks that one file can decide for certain: the
// compiler's own unused-variable / unused-import / label / branch /
// missing-return errors, vet's unreachable-code and self-assignment, and
// standard-library members that do not exist or are deprecated.
// go_lsp_features.cpp answers the editor's queries over the same model.

#include "go_lsp.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <functional>
#include <set>
#include <unordered_map>

#include "go_format.h"
#include "go_lsp_internal.h"
#include "go_lsp_vocab.h"

using namespace goast;
using golsp::Ref;
using golsp::Scope;
using golsp::Sym;
using golsp::SymKind;

// --- GoLspDocument helpers ------------------------------------------------------

GoLspRange GoLspDocument::RangeOf(Pos a, Pos b) const {
    GoLspRange r;
    int l = 0, c = 0;
    parse.LineCol(a, &l, &c);
    r.line = std::max(0, l - 1);
    r.col = std::max(0, c - 1);
    if (b < a) b = a;
    parse.LineCol(b, &l, &c);
    r.end_line = std::max(0, l - 1);
    r.end_col = std::max(0, c - 1);
    return r;
}

Pos GoLspDocument::PosAt(int line, int col) const {
    if (lines.empty()) return 1;
    line = std::clamp(line, 0, static_cast<int>(lines.size()) - 1);
    size_t off = 0;
    for (int i = 0; i < line; i++) off += lines[static_cast<size_t>(i)].size() + 1;
    col = std::clamp(col, 0, static_cast<int>(lines[static_cast<size_t>(line)].size()));
    return static_cast<Pos>(off) + col + 1;
}

std::string GoLspDocument::Slice(Pos a, Pos b) const {
    const int n = static_cast<int>(text.size());
    int x = std::clamp(a - 1, 0, n), y = std::clamp(b - 1, 0, n);
    if (y < x) return "";
    return text.substr(static_cast<size_t>(x), static_cast<size_t>(y - x));
}

const Ref *GoLspDocument::RefAt(Pos p) const {
    auto it = std::upper_bound(refs.begin(), refs.end(), p, [](Pos q, const Ref &r) { return q < r.pos; });
    // the candidate is the last ref starting at or before p
    while (it != refs.begin()) {
        --it;
        if (p <= it->pos + it->len) return p >= it->pos ? &*it : nullptr;
        if (it->pos + it->len < p) break;
    }
    return nullptr;
}

int GoLspDocument::ScopeAt(Pos p) const {
    int best = file_scope;
    Pos best_start = 0;
    for (size_t i = 2; i < scopes.size(); i++) {
        const Scope &s = scopes[i];
        if (s.start <= p && p <= s.end && s.start >= best_start) {
            best = static_cast<int>(i);
            best_start = s.start;
        }
    }
    return best;
}

int GoLspDocument::Lookup(const std::string &name, Pos p) const {
    for (int s = ScopeAt(p); s >= 0; s = scopes[static_cast<size_t>(s)].parent) {
        auto it = scopes[static_cast<size_t>(s)].names.find(name);
        if (it == scopes[static_cast<size_t>(s)].names.end()) continue;
        const Sym &sym = syms[static_cast<size_t>(it->second)];
        if (sym.local && sym.pos > p) continue;  // declared further down
        return it->second;
    }
    return -1;
}

// --- names ---------------------------------------------------------------------

namespace golsp {

bool IsUniverse(const std::string &name) {
    static const std::set<std::string> kNames = {
        "any", "bool", "byte", "comparable", "complex64", "complex128", "error", "float32", "float64", "int",
        "int8", "int16", "int32", "int64", "rune", "string", "uint", "uint8", "uint16", "uint32", "uint64",
        "uintptr", "true", "false", "iota", "nil", "append", "cap", "clear", "close", "complex", "copy", "delete",
        "imag", "len", "make", "max", "min", "new", "panic", "print", "println", "real", "recover"};
    return kNames.count(name) > 0;
}

std::string StdPackageName(const std::string &path) {
    const GoVocabEntry *e = GoVocabFind(path, "");
    if (!e) return "";
    std::string_view sig = e->signature;
    if (sig.substr(0, 8) == "package ") return std::string(sig.substr(8));
    return "";
}

std::string GuessImportName(const std::string &path, bool *certain) {
    std::string std_name = StdPackageName(path);
    if (!std_name.empty()) {
        *certain = true;
        return std_name;
    }
    *certain = false;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        parts.push_back(path.substr(i, j - i));
        i = j + 1;
    }
    std::string last = parts.empty() ? path : parts.back();
    auto is_major = [](const std::string &s) {
        if (s.size() < 2 || s[0] != 'v') return false;
        return std::all_of(s.begin() + 1, s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    };
    if (is_major(last) && parts.size() >= 2) last = parts[parts.size() - 2];
    if (size_t dot = last.find(".v"); dot != std::string::npos && is_major(last.substr(dot + 1))) last = last.substr(0, dot);
    if (last.rfind("go-", 0) == 0) last = last.substr(3);
    if (last.size() > 3 && last.compare(last.size() - 3, 3, "-go") == 0) last = last.substr(0, last.size() - 3);
    if (last.size() > 3 && last.compare(last.size() - 3, 3, ".go") == 0) last = last.substr(0, last.size() - 3);
    std::string out;
    for (char c : last) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') out += c;
    }
    return out;
}

}  // namespace golsp

namespace {

// --- the resolver ----------------------------------------------------------------

enum class KeyMode { Expr, Field, Unknown };

class Resolver {
public:
    explicit Resolver(GoLspDocument &d) : d_(d) {}
    void Run();

private:
    GoLspDocument &d_;
    int cur_ = 0;
    std::vector<GoLspDiagnostic> &out() { return d_.semantic; }

    struct BranchCtx {
        K kind;
        const Ident *label;
    };
    struct FuncCtx {
        std::map<std::string, int> labels;
        std::vector<BranchCtx> branches;
    };
    std::deque<FuncCtx> funcs_;  // a deque: Stmt holds a pointer to the back while nested literals push
    const Ident *pending_label_ = nullptr;  // the label of the statement being entered
    enum class Fall { None, Ok, Final, TypeSwitch } fall_ = Fall::None;

    void Diag(Pos a, Pos b, const std::string &code, const std::string &msg, GoLspSeverity sev = GoLspSeverity::Error,
              bool unnecessary = false, bool deprecated = false) {
        GoLspDiagnostic g;
        g.range = d_.RangeOf(a, b);
        g.code = code;
        g.message = msg;
        g.severity = sev;
        g.unnecessary = unnecessary;
        g.deprecated = deprecated;
        out().push_back(std::move(g));
    }

    int NewScope(Pos start, Pos end, bool fn = false) {
        Scope s;
        s.parent = cur_;
        s.start = start;
        s.end = end;
        s.function = fn;
        d_.scopes.push_back(std::move(s));
        cur_ = static_cast<int>(d_.scopes.size()) - 1;
        return cur_;
    }
    void PopScope() { cur_ = d_.scopes[static_cast<size_t>(cur_)].parent; }
    bool InFunction() const { return cur_ != d_.package_scope && cur_ != d_.file_scope; }

    void AddRef(Ident *id, int sym, bool write, bool decl, Node *base = nullptr) {
        Ref r;
        r.pos = id->NamePos;
        r.len = static_cast<int>(id->Name.size());
        r.sym = sym;
        r.write = write;
        r.decl = decl;
        r.ident = id;
        r.selector_base = base;
        r.scope = cur_;
        d_.refs.push_back(r);
    }

    int Declare(Ident *id, SymKind kind, Node *decl, Node *type = nullptr, Node *value = nullptr, int vi = -1,
                int scope = -1) {
        if (!id || id->Name == "_" || id->Name.empty()) return -1;
        if (scope < 0) scope = cur_;
        Sym s;
        s.name = id->Name;
        s.kind = kind;
        s.pos = id->NamePos;
        s.decl = decl;
        s.type = type;
        s.value = value;
        s.value_index = vi;
        s.scope = scope;
        s.local = scope != d_.package_scope && scope != d_.file_scope;
        d_.syms.push_back(std::move(s));
        int idx = static_cast<int>(d_.syms.size()) - 1;
        auto &names = d_.scopes[static_cast<size_t>(scope)].names;
        auto it = names.find(id->Name);
        if (it != names.end()) {
            if (!(scope == d_.package_scope && id->Name == "init")) {
                Diag(id->NamePos, id->NamePos + static_cast<int>(id->Name.size()), "redeclared",
                     id->Name + " redeclared in this block");
            }
        } else {
            names[id->Name] = idx;
        }
        AddRef(id, idx, false, true);
        return idx;
    }

    int LookupHere(const std::string &name) const {
        for (int s = cur_; s >= 0; s = d_.scopes[static_cast<size_t>(s)].parent) {
            auto &names = d_.scopes[static_cast<size_t>(s)].names;
            auto it = names.find(name);
            if (it != names.end()) return it->second;
        }
        return -1;
    }

    void Use(Ident *id) {
        int s = LookupHere(id->Name);
        if (s >= 0) d_.syms[static_cast<size_t>(s)].used = true;
        AddRef(id, s, false, false);
    }
    void Write(Ident *id) { AddRef(id, LookupHere(id->Name), true, false); }

    // expressions and types
    void Expr(Node *n);
    void Fields(FieldList *fl);  // types of a field list; names are not declared
    void CompositeElems(CompositeLit *c, Node *type);
    KeyMode ModeOf(Node *type, Node **elem_type, int depth = 0);
    void FuncScope(Node *recv_or_null, FuncType *ft, BlockStmt *body, Pos start, Pos end);
    void DeclareParams(FieldList *fl, SymKind kind);

    // statements
    void Stmts(const std::vector<Node *> &list);
    void Stmt(Node *n);
    void GenDecl(goast::GenDecl *g, bool top);
    void Branch(BranchStmt *b);
    void CollectLabels(Node *n, FuncCtx &f);
    void CheckSelfAssign(AssignStmt *as);

    std::unordered_map<const Ident *, int> ident_sym_;
    int SymOf(const Ident *id) {
        if (ident_sym_.empty()) {
            for (const Ref &r : d_.refs) ident_sym_[r.ident] = r.sym;
        }
        auto it = ident_sym_.find(id);
        return it == ident_sym_.end() ? -1 : it->second;
    }

    void CheckUnused();
    void CheckImports();
    void CheckStdMembers();
};

void Resolver::Fields(FieldList *fl) {
    if (!fl) return;
    for (Field *f : fl->List) Expr(f->Type);
}

KeyMode Resolver::ModeOf(Node *type, Node **elem_type, int depth) {
    *elem_type = nullptr;
    if (!type || depth > 8) return KeyMode::Unknown;
    type = Unparen(type);
    switch (type->kind) {
    case K::MapType: *elem_type = static_cast<MapType *>(type)->Value; return KeyMode::Expr;
    case K::ArrayType: *elem_type = static_cast<ArrayType *>(type)->Elt; return KeyMode::Expr;
    case K::StructType: return KeyMode::Field;
    case K::StarExpr: return ModeOf(static_cast<StarExpr *>(type)->X, elem_type, depth + 1);
    case K::Ident: {
        int s = LookupHere(static_cast<Ident *>(type)->Name);
        if (s >= 0 && d_.syms[static_cast<size_t>(s)].kind == SymKind::Type) {
            return ModeOf(d_.syms[static_cast<size_t>(s)].type, elem_type, depth + 1);
        }
        return KeyMode::Unknown;
    }
    case K::IndexExpr: return ModeOf(static_cast<IndexExpr *>(type)->X, elem_type, depth + 1);
    case K::IndexListExpr: return ModeOf(static_cast<IndexListExpr *>(type)->X, elem_type, depth + 1);
    default: return KeyMode::Unknown;
    }
}

void Resolver::CompositeElems(CompositeLit *c, Node *type) {
    Node *elem = nullptr;
    KeyMode mode = ModeOf(type, &elem);
    for (Node *e : c->Elts) {
        Node *value = e;
        if (auto *kv = As<KeyValueExpr>(e)) {
            value = kv->Value;
            auto *key = As<Ident>(kv->Key);
            if (mode == KeyMode::Field && key) {
                AddRef(key, -1, false, false);
            } else if (mode == KeyMode::Unknown && key) {
                int s = LookupHere(key->Name);
                if (s >= 0) {
                    d_.syms[static_cast<size_t>(s)].used = true;
                    AddRef(key, s, false, false);
                } else {
                    AddRef(key, -1, false, false);
                }
            } else if (auto *kc = As<CompositeLit>(kv->Key); kc && !kc->Type) {
                Node *ktype = nullptr;
                if (auto *m = As<MapType>(type ? Unparen(type) : nullptr)) ktype = m->Key;
                CompositeElems(kc, ktype);
            } else {
                Expr(kv->Key);
            }
        }
        if (auto *vc = As<CompositeLit>(value); vc && !vc->Type) {
            CompositeElems(vc, elem);
        } else if (auto *u = As<UnaryExpr>(value); u && u->Op == Tok::AND && As<CompositeLit>(u->X) &&
                                                   !As<CompositeLit>(u->X)->Type) {
            Node *inner = elem;
            if (auto *st = As<StarExpr>(inner ? Unparen(inner) : nullptr)) inner = st->X;
            CompositeElems(As<CompositeLit>(u->X), inner);
        } else {
            Expr(value);
        }
    }
}

void Resolver::Expr(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case K::Ident: Use(static_cast<Ident *>(n)); return;
    case K::SelectorExpr: {
        auto *s = static_cast<SelectorExpr *>(n);
        Expr(s->X);
        AddRef(s->Sel, -1, false, false, s->X);
        return;
    }
    case K::FuncLit: {
        auto *f = static_cast<FuncLit *>(n);
        FuncScope(nullptr, f->Type, f->Body, NodePos(f), NodeEnd(f));
        return;
    }
    case K::CompositeLit: {
        auto *c = static_cast<CompositeLit *>(n);
        Expr(c->Type);
        CompositeElems(c, c->Type);
        return;
    }
    case K::StructType: Fields(static_cast<StructType *>(n)->Fields); return;
    case K::InterfaceType: {
        auto *it = static_cast<InterfaceType *>(n);
        if (!it->Methods) return;
        for (Field *f : it->Methods->List) {
            if (auto *ft = As<FuncType>(f->Type); ft && !f->Names.empty()) {
                Fields(ft->Params);
                Fields(ft->Results);
            } else {
                Expr(f->Type);
            }
        }
        return;
    }
    case K::FuncType: {
        auto *ft = static_cast<FuncType *>(n);
        Fields(ft->TypeParams);
        Fields(ft->Params);
        Fields(ft->Results);
        return;
    }
    case K::KeyValueExpr: {
        auto *kv = static_cast<KeyValueExpr *>(n);
        Expr(kv->Key);
        Expr(kv->Value);
        return;
    }
    default: ForEachChild(n, [this](Node *c) { Expr(c); }); return;
    }
}

void Resolver::DeclareParams(FieldList *fl, SymKind kind) {
    if (!fl) return;
    for (Field *f : fl->List) {
        for (Ident *id : f->Names) Declare(id, kind, f, f->Type);
    }
}

void Resolver::CollectLabels(Node *n, FuncCtx &f) {
    if (!n || n->kind == K::FuncLit) return;
    if (auto *ls = As<LabeledStmt>(n)) {
        if (ls->Label->Name != "_") {
            auto it = f.labels.find(ls->Label->Name);
            if (it != f.labels.end()) {
                Diag(ls->Label->NamePos, NodeEnd(ls->Label), "redeclared", "label " + ls->Label->Name + " already defined");
            } else {
                Sym s;
                s.name = ls->Label->Name;
                s.kind = SymKind::Label;
                s.pos = ls->Label->NamePos;
                s.decl = ls;
                s.local = true;
                s.scope = cur_;
                d_.syms.push_back(std::move(s));
                int idx = static_cast<int>(d_.syms.size()) - 1;
                f.labels[ls->Label->Name] = idx;
                AddRef(ls->Label, idx, false, true);
            }
        }
    }
    if (IsExpr(n)) return;
    ForEachChild(n, [&](Node *c) { CollectLabels(c, f); });
}

void Resolver::FuncScope(Node *recv, FuncType *ft, BlockStmt *body, Pos start, Pos end) {
    int saved = cur_;
    NewScope(start, end, true);
    auto *rfl = As<FieldList>(recv);
    if (rfl) {
        for (Field *f : rfl->List) {
            // `func (l *List[T]) M()`: the receiver's type arguments declare type parameters.
            Node *t = f->Type;
            if (auto *st = As<StarExpr>(t ? Unparen(t) : nullptr)) t = st->X;
            if (auto *ix = As<IndexExpr>(t)) {
                Expr(ix->X);
                if (auto *id = As<Ident>(ix->Index)) Declare(id, SymKind::TypeParam, f);
            } else if (auto *il = As<IndexListExpr>(t)) {
                Expr(il->X);
                for (Node *e : il->Indices) {
                    if (auto *id = As<Ident>(e)) Declare(id, SymKind::TypeParam, f);
                }
            } else {
                Expr(f->Type);
            }
        }
        DeclareParams(rfl, SymKind::Param);
    }
    if (ft) {
        if (ft->TypeParams) {
            for (Field *f : ft->TypeParams->List) {
                for (Ident *id : f->Names) Declare(id, SymKind::TypeParam, f, f->Type);
            }
            Fields(ft->TypeParams);
        }
        Fields(ft->Params);
        Fields(ft->Results);
        DeclareParams(ft->Params, SymKind::Param);
        DeclareParams(ft->Results, SymKind::Result);
    }
    if (body) {
        funcs_.emplace_back();
        for (Node *s : body->List) CollectLabels(s, funcs_.back());
        Fall saved_fall = fall_;
        fall_ = Fall::None;
        Stmts(body->List);
        fall_ = saved_fall;
        for (auto &[name, idx] : funcs_.back().labels) {
            const Sym &s = d_.syms[static_cast<size_t>(idx)];
            if (!s.used) {
                Diag(s.pos, s.pos + static_cast<int>(s.name.size()), "unused-label", "label " + name + " defined and not used",
                     GoLspSeverity::Error, true);
            }
        }
        funcs_.pop_back();
    }
    cur_ = saved;
}

bool IsTerminating(Node *s, const std::string &label);

bool IsTerminatingList(const std::vector<Node *> &list, const std::string &label) {
    size_t n = list.size();
    while (n > 0 && As<EmptyStmt>(list[n - 1])) n--;
    return n > 0 && IsTerminating(list[n - 1], label);
}

// Whether a `break` inside `body` leaves the statement labelled `label`
// (unlabelled breaks count unless inside a nested for/switch/select).
bool HasBreak(Node *n, const std::string &label, bool implicit) {
    if (!n || IsExpr(n) || n->kind == K::FuncLit) return false;
    if (auto *b = As<BranchStmt>(n)) {
        if (b->Tok_ != Tok::BREAK) return false;
        if (!b->Label) return implicit;
        return b->Label->Name == label;
    }
    bool nested = n->kind == K::ForStmt || n->kind == K::RangeStmt || n->kind == K::SwitchStmt ||
                  n->kind == K::TypeSwitchStmt || n->kind == K::SelectStmt;
    bool found = false;
    ForEachChild(n, [&](Node *c) {
        if (!found && HasBreak(c, label, implicit && !nested)) found = true;
    });
    return found;
}

bool HasBreakIn(Node *body, const std::string &label) {
    bool found = false;
    ForEachChild(body, [&](Node *c) {
        if (!found && HasBreak(c, label, true)) found = true;
    });
    return found;
}

bool IsTerminating(Node *s, const std::string &label) {
    if (!s) return false;
    switch (s->kind) {
    case K::ReturnStmt: return true;
    case K::BranchStmt: {
        Tok t = static_cast<BranchStmt *>(s)->Tok_;
        return t == Tok::GOTO || t == Tok::FALLTHROUGH;
    }
    case K::ExprStmt: {
        auto *call = As<CallExpr>(Unparen(static_cast<ExprStmt *>(s)->X));
        if (!call) return false;
        auto *id = As<Ident>(Unparen(call->Fun));
        return id && id->Name == "panic";
    }
    case K::BlockStmt: return IsTerminatingList(static_cast<BlockStmt *>(s)->List, "");
    case K::IfStmt: {
        auto *i = static_cast<IfStmt *>(s);
        return i->Else && IsTerminating(i->Body, "") && IsTerminating(i->Else, "");
    }
    case K::LabeledStmt: {
        auto *l = static_cast<LabeledStmt *>(s);
        return IsTerminating(l->Stmt, l->Label->Name);
    }
    case K::ForStmt: {
        auto *f = static_cast<ForStmt *>(s);
        return !f->Cond && !HasBreakIn(f->Body, label);
    }
    case K::SwitchStmt:
    case K::TypeSwitchStmt:
    case K::SelectStmt: {
        BlockStmt *body = s->kind == K::SwitchStmt       ? static_cast<SwitchStmt *>(s)->Body
                          : s->kind == K::TypeSwitchStmt ? static_cast<TypeSwitchStmt *>(s)->Body
                                                         : static_cast<SelectStmt *>(s)->Body;
        bool has_default = s->kind == K::SelectStmt;
        for (Node *c : body->List) {
            const std::vector<Node *> *list = nullptr;
            if (auto *cc = As<CaseClause>(c)) {
                if (cc->List.empty()) has_default = true;
                list = &cc->Body;
            } else if (auto *cm = As<CommClause>(c)) {
                list = &cm->Body;
            }
            if (!list) return false;
            if (!IsTerminatingList(*list, "")) {
                size_t n = list->size();
                while (n > 0 && As<EmptyStmt>((*list)[n - 1])) n--;
                auto *b = n > 0 ? As<BranchStmt>((*list)[n - 1]) : nullptr;
                if (!(b && b->Tok_ == Tok::FALLTHROUGH)) return false;
            }
            for (Node *st : *list) {
                if (HasBreak(st, label, true)) return false;
            }
        }
        return has_default;
    }
    default: return false;
    }
}

void Resolver::Stmts(const std::vector<Node *> &list) {
    bool reported = false;
    Fall outer = fall_;
    size_t last = list.size();
    while (last > 0 && As<EmptyStmt>(list[last - 1])) last--;
    for (size_t i = 0; i < list.size(); i++) {
        fall_ = (i + 1 == last) ? outer : Fall::None;
        Stmt(list[i]);
        if (!reported && i + 1 < list.size()) {
            Node *s = list[i];
            auto *b = As<BranchStmt>(s);
            bool stops = IsTerminating(s, "") || (b && (b->Tok_ == Tok::BREAK || b->Tok_ == Tok::CONTINUE));
            Node *next = list[i + 1];
            if (stops && !As<EmptyStmt>(next) && !As<LabeledStmt>(next) && !(b && b->Tok_ == Tok::FALLTHROUGH)) {
                Diag(NodePos(next), NodeEnd(next), "unreachable", "unreachable code", GoLspSeverity::Warning, true);
                reported = true;
            }
        }
    }
    fall_ = outer;
}

void Resolver::CheckSelfAssign(AssignStmt *as) {
    if (as->Tok_ != Tok::ASSIGN || as->Lhs.size() != as->Rhs.size()) return;
    std::function<bool(Node *)> pure = [&](Node *e) -> bool {
        if (As<Ident>(e)) return true;
        if (auto *s = As<SelectorExpr>(e)) return pure(s->X);
        return false;
    };
    for (size_t i = 0; i < as->Lhs.size(); i++) {
        if (!pure(as->Lhs[i]) || !pure(as->Rhs[i])) continue;
        std::string l = d_.SliceNode(as->Lhs[i]), r = d_.SliceNode(as->Rhs[i]);
        if (l == r && l != "_") {
            Diag(NodePos(as->Lhs[i]), NodeEnd(as->Rhs[i]), "self-assignment", "self-assignment of " + r + " to " + l,
                 GoLspSeverity::Warning);
        }
    }
}

void Resolver::Branch(BranchStmt *b) {
    Pos a = b->TokPos, e = NodeEnd(b);
    FuncCtx *f = funcs_.empty() ? nullptr : &funcs_.back();
    if (b->Tok_ == Tok::FALLTHROUGH) {
        if (fall_ == Fall::Final) Diag(a, e, "misplaced-branch", "cannot fallthrough final case in switch");
        else if (fall_ == Fall::TypeSwitch) Diag(a, e, "misplaced-branch", "cannot fallthrough in type switch");
        else if (fall_ != Fall::Ok) Diag(a, e, "misplaced-branch", "fallthrough statement out of place");
        return;
    }
    if (!f) return;
    if (b->Label) {
        auto it = f->labels.find(b->Label->Name);
        if (it == f->labels.end()) {
            AddRef(b->Label, -1, false, false);
            Diag(b->Label->NamePos, NodeEnd(b->Label), "undefined-label", "label " + b->Label->Name + " not defined");
            return;
        }
        d_.syms[static_cast<size_t>(it->second)].used = true;
        AddRef(b->Label, it->second, false, false);
        if (b->Tok_ == Tok::GOTO) return;
        bool ok = false;
        for (auto &c : f->branches) {
            if (c.label && c.label->Name == b->Label->Name) {
                ok = b->Tok_ == Tok::BREAK || c.kind == K::ForStmt || c.kind == K::RangeStmt;
                break;
            }
        }
        if (!ok) {
            Diag(b->Label->NamePos, NodeEnd(b->Label), "misplaced-branch",
                 std::string("invalid ") + (b->Tok_ == Tok::BREAK ? "break" : "continue") + " label " + b->Label->Name);
        }
        return;
    }
    if (b->Tok_ == Tok::BREAK && f->branches.empty()) {
        Diag(a, e, "misplaced-branch", "break is not in a loop, switch, or select");
    } else if (b->Tok_ == Tok::CONTINUE) {
        bool loop = std::any_of(f->branches.begin(), f->branches.end(),
                                [](const BranchCtx &c) { return c.kind == K::ForStmt || c.kind == K::RangeStmt; });
        if (!loop) Diag(a, e, "misplaced-branch", "continue is not in a loop");
    }
}

void Resolver::GenDecl(goast::GenDecl *g, bool top) {
    for (Node *spec : g->Specs) {
        if (auto *vs = As<ValueSpec>(spec)) {
            Expr(vs->Type);
            for (Node *v : vs->Values) Expr(v);
            if (top) continue;  // declared in the first pass
            for (size_t i = 0; i < vs->Names.size(); i++) {
                Node *value = nullptr;
                int vi = -1;
                if (vs->Values.size() == vs->Names.size()) {
                    value = vs->Values[i];
                } else if (vs->Values.size() == 1) {
                    value = vs->Values[0];
                    vi = static_cast<int>(i);
                }
                Declare(vs->Names[i], g->Tok_ == Tok::CONST ? SymKind::Const : SymKind::Var, vs, vs->Type, value, vi);
            }
        } else if (auto *ts = As<TypeSpec>(spec)) {
            if (!top) Declare(ts->Name, SymKind::Type, ts, ts->Type);
            if (ts->TypeParams) {
                int saved = cur_;
                NewScope(NodePos(ts), NodeEnd(ts));
                for (Field *f : ts->TypeParams->List) {
                    for (Ident *id : f->Names) Declare(id, SymKind::TypeParam, f, f->Type);
                }
                Fields(ts->TypeParams);
                Expr(ts->Type);
                cur_ = saved;
            } else {
                Expr(ts->Type);
            }
        }
    }
}

void Resolver::Stmt(Node *n) {
    if (!n) return;
    const Ident *label = pending_label_;
    pending_label_ = nullptr;
    FuncCtx *f = funcs_.empty() ? nullptr : &funcs_.back();
    auto push = [&](K k) {
        if (f) f->branches.push_back({k, label});
    };
    auto pop = [&] {
        if (f) f->branches.pop_back();
    };
    switch (n->kind) {
    case K::DeclStmt:
        if (auto *g = As<goast::GenDecl>(static_cast<DeclStmt *>(n)->Decl)) GenDecl(g, false);
        break;
    case K::LabeledStmt: {
        auto *ls = static_cast<LabeledStmt *>(n);
        pending_label_ = ls->Label;
        Stmt(ls->Stmt);
        pending_label_ = nullptr;
        break;
    }
    case K::ExprStmt: Expr(static_cast<ExprStmt *>(n)->X); break;
    case K::SendStmt: Expr(static_cast<SendStmt *>(n)->Chan); Expr(static_cast<SendStmt *>(n)->Value); break;
    case K::IncDecStmt: Expr(static_cast<IncDecStmt *>(n)->X); break;
    case K::AssignStmt: {
        auto *as = static_cast<AssignStmt *>(n);
        for (Node *r : as->Rhs) Expr(r);
        if (as->Tok_ == Tok::DEFINE) {
            bool any_new = false;
            for (size_t i = 0; i < as->Lhs.size(); i++) {
                auto *id = As<Ident>(as->Lhs[i]);
                if (!id) {
                    Expr(as->Lhs[i]);
                    continue;
                }
                if (id->Name == "_") {
                    AddRef(id, -1, true, false);
                    continue;
                }
                auto &names = d_.scopes[static_cast<size_t>(cur_)].names;
                if (names.count(id->Name)) {
                    Write(id);
                    continue;
                }
                any_new = true;
                Node *value = nullptr;
                int vi = -1;
                if (as->Rhs.size() == as->Lhs.size()) {
                    value = as->Rhs[i];
                } else if (as->Rhs.size() == 1) {
                    value = as->Rhs[0];
                    vi = static_cast<int>(i);
                }
                Declare(id, SymKind::Var, as, nullptr, value, vi);
            }
            if (!any_new) Diag(as->TokPos, as->TokPos + 2, "no-new-variables", "no new variables on left side of :=");
        } else if (as->Tok_ == Tok::ASSIGN) {
            for (Node *l : as->Lhs) {
                if (auto *id = As<Ident>(l)) {
                    if (id->Name == "_") AddRef(id, -1, true, false);
                    else Write(id);
                } else {
                    Expr(l);
                }
            }
            CheckSelfAssign(as);
        } else {
            for (Node *l : as->Lhs) Expr(l);
        }
        break;
    }
    case K::GoStmt: Expr(static_cast<GoStmt *>(n)->Call); break;
    case K::DeferStmt: Expr(static_cast<DeferStmt *>(n)->Call); break;
    case K::ReturnStmt:
        for (Node *r : static_cast<ReturnStmt *>(n)->Results) Expr(r);
        break;
    case K::BranchStmt: Branch(static_cast<BranchStmt *>(n)); break;
    case K::BlockStmt: {
        auto *b = static_cast<BlockStmt *>(n);
        int saved = cur_;
        NewScope(b->Lbrace, NodeEnd(b));
        Stmts(b->List);
        cur_ = saved;
        break;
    }
    case K::IfStmt: {
        auto *i = static_cast<IfStmt *>(n);
        int saved = cur_;
        NewScope(i->If, NodeEnd(i));
        Stmt(i->Init);
        Expr(i->Cond);
        Fall sf = fall_;
        fall_ = Fall::None;
        Stmt(i->Body);
        Stmt(i->Else);
        fall_ = sf;
        cur_ = saved;
        break;
    }
    case K::SwitchStmt:
    case K::TypeSwitchStmt: {
        bool type_switch = n->kind == K::TypeSwitchStmt;
        int saved = cur_;
        NewScope(NodePos(n), NodeEnd(n));
        BlockStmt *body = nullptr;
        int tsym = -1;
        Ident *tid = nullptr;
        if (!type_switch) {
            auto *s = static_cast<SwitchStmt *>(n);
            Stmt(s->Init);
            Expr(s->Tag);
            body = s->Body;
        } else {
            auto *s = static_cast<TypeSwitchStmt *>(n);
            Stmt(s->Init);
            body = s->Body;
            if (auto *as = As<AssignStmt>(s->Assign); as && as->Lhs.size() == 1 && as->Rhs.size() == 1) {
                Expr(as->Rhs[0]);
                tid = As<Ident>(as->Lhs[0]);
                if (tid && tid->Name != "_") {
                    Sym sym;
                    sym.name = tid->Name;
                    sym.kind = SymKind::TypeSwitch;
                    sym.pos = tid->NamePos;
                    sym.decl = as;
                    sym.value = as->Rhs[0];
                    sym.local = true;
                    sym.scope = cur_;
                    d_.syms.push_back(std::move(sym));
                    tsym = static_cast<int>(d_.syms.size()) - 1;
                    AddRef(tid, tsym, false, true);
                }
            } else {
                Stmt(s->Assign);
            }
        }
        push(n->kind);
        Fall sf = fall_;
        for (size_t ci = 0; ci < body->List.size(); ci++) {
            auto *cc = As<CaseClause>(body->List[ci]);
            if (!cc) continue;
            for (Node *e : cc->List) Expr(e);
            int csaved = cur_;
            NewScope(cc->Colon, NodeEnd(cc));
            if (tsym >= 0) {
                d_.scopes[static_cast<size_t>(cur_)].names[tid->Name] = tsym;
                // a single-type case gives the symbol that type
            }
            fall_ = type_switch ? Fall::TypeSwitch : (ci + 1 == body->List.size() ? Fall::Final : Fall::Ok);
            Stmts(cc->Body);
            cur_ = csaved;
        }
        fall_ = sf;
        pop();
        cur_ = saved;
        if (tsym >= 0 && !d_.syms[static_cast<size_t>(tsym)].used && !body->List.empty()) {
            Diag(tid->NamePos, NodeEnd(tid), "unused-variable", tid->Name + " declared and not used", GoLspSeverity::Error,
                 true);
        }
        break;
    }
    case K::SelectStmt: {
        auto *s = static_cast<SelectStmt *>(n);
        push(K::SelectStmt);
        Fall sf = fall_;
        fall_ = Fall::None;
        for (Node *c : s->Body->List) {
            auto *cc = As<CommClause>(c);
            if (!cc) continue;
            int saved = cur_;
            NewScope(cc->Case, NodeEnd(cc));
            Stmt(cc->Comm);
            Stmts(cc->Body);
            cur_ = saved;
        }
        fall_ = sf;
        pop();
        break;
    }
    case K::ForStmt: {
        auto *fs = static_cast<ForStmt *>(n);
        int saved = cur_;
        NewScope(fs->For, NodeEnd(fs));
        Stmt(fs->Init);
        Expr(fs->Cond);
        Stmt(fs->Post);
        push(K::ForStmt);
        Fall sf = fall_;
        fall_ = Fall::None;
        Stmt(fs->Body);
        fall_ = sf;
        pop();
        cur_ = saved;
        break;
    }
    case K::RangeStmt: {
        auto *rs = static_cast<RangeStmt *>(n);
        int saved = cur_;
        Expr(rs->X);
        NewScope(rs->For, NodeEnd(rs));
        Node *kv[2] = {rs->Key, rs->Value};
        for (int k = 0; k < 2; k++) {
            if (!kv[k]) continue;
            auto *id = As<Ident>(kv[k]);
            if (rs->Tok_ == Tok::DEFINE && id) {
                int s = Declare(id, SymKind::Var, rs, nullptr, rs->X);
                if (s >= 0) (k == 0 ? d_.syms[static_cast<size_t>(s)].range_key : d_.syms[static_cast<size_t>(s)].range_value) = true;
                if (s < 0) AddRef(id, -1, true, false);
            } else if (rs->Tok_ == Tok::ASSIGN && id) {
                if (id->Name == "_") AddRef(id, -1, true, false);
                else Write(id);
            } else {
                Expr(kv[k]);
            }
        }
        push(K::RangeStmt);
        Fall sf = fall_;
        fall_ = Fall::None;
        Stmt(rs->Body);
        fall_ = sf;
        pop();
        cur_ = saved;
        break;
    }
    default: break;
    }
}

void Resolver::CheckUnused() {
    for (const Sym &s : d_.syms) {
        if (s.kind != SymKind::Var || !s.local || s.used) continue;
        Diag(s.pos, s.pos + static_cast<int>(s.name.size()), "unused-variable", "declared and not used: " + s.name,
             GoLspSeverity::Error, true);
    }
}

void Resolver::CheckImports() {
    // Unresolved selector bases: a package this file uses under a name we could not guess.
    std::set<std::string> unresolved_bases;
    for (const Ref &r : d_.refs) {
        if (!r.selector_base) continue;
        if (auto *id = As<Ident>(r.selector_base)) {
            int s = SymOf(id);
            if (s < 0 && !golsp::IsUniverse(id->Name)) unresolved_bases.insert(id->Name);
        }
    }
    for (const golsp::ImportInfo &im : d_.imports) {
        if (im.sym < 0) continue;
        const Sym &s = d_.syms[static_cast<size_t>(im.sym)];
        if (s.used) continue;
        bool explicit_name = im.spec->Name != nullptr;
        if (!im.certain && !unresolved_bases.empty()) continue;
        std::string msg = "\"" + im.path + "\" imported";
        if (explicit_name) msg += " as " + im.name;
        msg += " and not used";
        Diag(NodePos(im.spec), NodeEnd(im.spec), "unused-import", msg, GoLspSeverity::Error, true);
    }
}

void Resolver::CheckStdMembers() {
    // `package os_test` may use the hooks os's export_test.go exports to its tests
    std::string tested;
    const std::string &pkg = d_.parse.file->Name ? d_.parse.file->Name->Name : "";
    if (pkg.size() > 5 && pkg.compare(pkg.size() - 5, 5, "_test") == 0) tested = pkg.substr(0, pkg.size() - 5);
    for (const Ref &r : d_.refs) {
        auto *base = As<Ident>(r.selector_base);
        if (!base) continue;
        int s = SymOf(base);
        if (s < 0 || d_.syms[static_cast<size_t>(s)].kind != SymKind::Import) continue;
        const std::string &path = d_.syms[static_cast<size_t>(s)].import_path;
        if (!GoVocabFind(path, "")) continue;
        const std::string &name = r.ident->Name;
        if (name.empty() || name == "_" || !IsExported(name)) continue;
        const GoVocabEntry *e = GoVocabFind(path, name);
        // syscall and runtime differ per platform; the table is one platform's.
        bool platform = path == "syscall" || path.rfind("runtime", 0) == 0 || path == "os/signal" ||
                        path.rfind("internal/", 0) == 0 || path.find("/internal/") != std::string::npos;
        Pos a = r.pos, b = r.pos + r.len;
        if (!e) {
            if (!platform && d_.host_build && golsp::StdPackageName(path) != tested) {
                Diag(NodePos(base), b, "undefined", "undefined: " + base->Name + "." + name);
            }
            continue;
        }
        if (e->deprecated) {
            std::string doc = e->doc;
            std::string msg = base->Name + "." + name + " is deprecated";
            size_t k = doc.find("Deprecated:");
            if (k != std::string::npos) {
                std::string why = doc.substr(k + 11);
                size_t end = why.find("\n\n");
                if (end != std::string::npos) why = why.substr(0, end);
                for (char &c : why) {
                    if (c == '\n') c = ' ';
                }
                while (!why.empty() && why[0] == ' ') why.erase(0, 1);
                if (!why.empty()) msg += ": " + why;
            }
            Diag(a, b, "deprecated", msg, GoLspSeverity::Hint, false, true);
        }
    }
}

void Resolver::Run() {
    // package block, then the file block (imports) inside it
    d_.scopes.clear();
    Scope pkg;
    pkg.start = 0;
    pkg.end = static_cast<Pos>(d_.text.size()) + 2;
    d_.scopes.push_back(pkg);
    Scope file = pkg;
    file.parent = 0;
    d_.scopes.push_back(file);
    d_.package_scope = 0;
    d_.file_scope = 1;
    File *f = d_.parse.file;

    cur_ = 1;
    for (ImportSpec *im : f->Imports) {
        golsp::ImportInfo info;
        info.spec = im;
        std::string path;
        if (!goparse::Unquote(im->Path->Value, &path)) continue;
        info.path = path;
        if (im->Name) {
            info.name = im->Name->Name;
            info.certain = true;
        } else {
            info.name = golsp::GuessImportName(path, &info.certain);
        }
        if (info.name == "_" || info.name == "." || path == "C" || info.name.empty()) {
            if (im->Name) AddRef(im->Name, -1, false, true);
            d_.imports.push_back(info);
            continue;
        }
        Sym s;
        s.name = info.name;
        s.kind = SymKind::Import;
        s.pos = im->Name ? im->Name->NamePos : im->Path->ValuePos;
        s.decl = im;
        s.scope = 1;
        s.import_path = path;
        s.doc = im->Doc;
        d_.syms.push_back(std::move(s));
        info.sym = static_cast<int>(d_.syms.size()) - 1;
        d_.scopes[1].names[info.name] = info.sym;
        if (im->Name) AddRef(im->Name, info.sym, false, true);
        d_.imports.push_back(info);
    }

    // first pass: every top-level name, so order does not matter
    cur_ = 0;
    for (Node *decl : f->Decls) {
        if (auto *fd = As<FuncDecl>(decl)) {
            if (fd->Recv) continue;
            if (fd->Name->Name == "init" || fd->Name->Name == "_") {
                AddRef(fd->Name, -1, false, true);
                continue;
            }
            int s = Declare(fd->Name, SymKind::Func, fd, fd->Type);
            if (s >= 0) d_.syms[static_cast<size_t>(s)].doc = fd->Doc;
        } else if (auto *g = As<goast::GenDecl>(decl)) {
            for (Node *spec : g->Specs) {
                CommentGroup *doc = nullptr;
                if (auto *vs = As<ValueSpec>(spec)) {
                    doc = vs->Doc ? vs->Doc : (g->Specs.size() == 1 ? g->Doc : nullptr);
                    for (size_t i = 0; i < vs->Names.size(); i++) {
                        Node *value = nullptr;
                        int vi = -1;
                        if (vs->Values.size() == vs->Names.size()) {
                            value = vs->Values[i];
                        } else if (vs->Values.size() == 1) {
                            value = vs->Values[0];
                            vi = static_cast<int>(i);
                        }
                        int s = Declare(vs->Names[i], g->Tok_ == Tok::CONST ? SymKind::Const : SymKind::Var, vs, vs->Type,
                                        value, vi);
                        if (s >= 0) d_.syms[static_cast<size_t>(s)].doc = doc ? doc : g->Doc;
                    }
                } else if (auto *ts = As<TypeSpec>(spec)) {
                    doc = ts->Doc ? ts->Doc : (g->Specs.size() == 1 ? g->Doc : nullptr);
                    int s = Declare(ts->Name, SymKind::Type, ts, ts->Type);
                    if (s >= 0) d_.syms[static_cast<size_t>(s)].doc = doc ? doc : g->Doc;
                }
            }
        }
    }
    // methods and struct fields: syms with no scope entry, for hover/navigation
    for (Node *decl : f->Decls) {
        auto *fd = As<FuncDecl>(decl);
        if (!fd || !fd->Recv || fd->Recv->List.empty()) continue;
        Node *t = fd->Recv->List[0]->Type;
        if (auto *st = As<StarExpr>(t ? Unparen(t) : nullptr)) t = st->X;
        if (auto *ix = As<IndexExpr>(t)) t = ix->X;
        if (auto *il = As<IndexListExpr>(t)) t = il->X;
        Sym s;
        s.name = fd->Name->Name;
        s.kind = SymKind::Method;
        s.pos = fd->Name->NamePos;
        s.decl = fd;
        s.type = fd->Type;
        s.scope = -1;
        s.doc = fd->Doc;
        if (auto *id = As<Ident>(t)) s.import_path = id->Name;
        d_.syms.push_back(std::move(s));
        AddRef(fd->Name, static_cast<int>(d_.syms.size()) - 1, false, true);
    }

    cur_ = 1;
    for (Node *decl : f->Decls) {
        cur_ = 1;
        if (auto *fd = As<FuncDecl>(decl)) {
            FuncScope(fd->Recv, fd->Type, fd->Body, NodePos(fd), NodeEnd(fd));
            if (fd->Body && fd->Type->Results && !fd->Type->Results->List.empty() &&
                !IsTerminatingList(fd->Body->List, "")) {
                Diag(fd->Body->Rbrace, fd->Body->Rbrace + 1, "missing-return", "missing return");
            }
        } else if (auto *g = As<goast::GenDecl>(decl)) {
            if (g->Tok_ != Tok::IMPORT) GenDecl(g, true);
        }
    }
    // function literals' missing returns
    std::function<void(Node *)> lits = [&](Node *n) {
        if (!n) return;
        if (auto *fl = As<FuncLit>(n)) {
            if (fl->Body && fl->Type->Results && !fl->Type->Results->List.empty() &&
                !IsTerminatingList(fl->Body->List, "")) {
                Diag(fl->Body->Rbrace, fl->Body->Rbrace + 1, "missing-return", "missing return");
            }
        }
        ForEachChild(n, lits);
    };
    for (Node *decl : f->Decls) lits(decl);

    std::stable_sort(d_.refs.begin(), d_.refs.end(), [](const Ref &a, const Ref &b) { return a.pos < b.pos; });
    CheckUnused();
    CheckImports();
    CheckStdMembers();
}

// --- build constraints ------------------------------------------------------------
// The standard-library table is linux/amd64's (go doc's view), so members are only
// checked in files that build there.

bool HostTag(const std::string &t) {
    static const std::set<std::string> kTrue = {"linux", "amd64", "unix", "gc", "cgo"};
    if (kTrue.count(t)) return true;
    if (t.rfind("go1.", 0) == 0) return true;
    return false;
}

// //go:build expression: ||, &&, !, parentheses, tags.
struct ConstraintEval {
    std::string s;
    size_t i = 0;
    bool ok = true;
    void Skip() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    }
    bool Or() {
        bool v = And();
        for (Skip(); s.compare(i, 2, "||") == 0; Skip()) {
            i += 2;
            v = And() || v;
        }
        return v;
    }
    bool And() {
        bool v = Not();
        for (Skip(); s.compare(i, 2, "&&") == 0; Skip()) {
            i += 2;
            v = Not() && v;
        }
        return v;
    }
    bool Not() {
        Skip();
        if (i < s.size() && s[i] == '!') {
            i++;
            return !Not();
        }
        if (i < s.size() && s[i] == '(') {
            i++;
            bool v = Or();
            Skip();
            if (i < s.size() && s[i] == ')') i++;
            else ok = false;
            return v;
        }
        size_t j = i;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '.')) i++;
        if (i == j) {
            ok = false;
            return true;
        }
        return HostTag(s.substr(j, i - j));
    }
};

bool BuildsOnHost(const File *f, const std::string &path) {
    static const std::set<std::string> kGoos = {"aix", "android", "darwin", "dragonfly", "freebsd", "hurd", "illumos",
                                                "ios", "js", "linux", "nacl", "netbsd", "openbsd", "plan9", "solaris",
                                                "wasip1", "windows", "zos"};
    static const std::set<std::string> kGoarch = {"386", "amd64", "arm", "arm64", "loong64", "mips", "mipsle", "mips64",
                                                  "mips64le", "ppc64", "ppc64le", "riscv64", "s390x", "wasm", "sparc64"};
    // the file name: name_GOOS_GOARCH.go, name_GOOS.go, name_GOARCH.go (each maybe with _test)
    std::string base = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
    if (base.size() > 3 && base.compare(base.size() - 3, 3, ".go") == 0) {
        base = base.substr(0, base.size() - 3);
        std::vector<std::string> parts;
        for (size_t a = 0; a <= base.size();) {
            size_t b = base.find('_', a);
            if (b == std::string::npos) b = base.size();
            parts.push_back(base.substr(a, b - a));
            a = b + 1;
        }
        if (!parts.empty() && parts.back() == "test") parts.pop_back();
        size_t n = parts.size();
        if (n >= 3 && kGoos.count(parts[n - 2]) && kGoarch.count(parts[n - 1])) {
            if (parts[n - 2] != "linux" || parts[n - 1] != "amd64") return false;
        } else if (n >= 2 && kGoos.count(parts[n - 1])) {
            if (parts[n - 1] != "linux") return false;
        } else if (n >= 2 && kGoarch.count(parts[n - 1])) {
            if (parts[n - 1] != "amd64") return false;
        }
    }
    // the //go:build line, among the comments before the package clause
    for (const CommentGroup *g : f->Comments) {
        if (IsValid(f->Package) && NodePos(g) > f->Package) break;
        for (const Comment *c : g->List) {
            if (c->Text.rfind("//go:build ", 0) != 0) continue;
            ConstraintEval e{c->Text.substr(11)};
            bool v = e.Or();
            if (e.ok && !v) return false;
        }
    }
    return true;
}

std::string JoinLines(const std::vector<std::string> &lines) {
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    return text;
}

}  // namespace

std::shared_ptr<const GoLspDocument> GoLspAnalyze(const std::vector<std::string> &lines, const std::string &path) {
    auto doc = std::make_shared<GoLspDocument>();
    doc->lines = lines;
    if (doc->lines.empty()) doc->lines.emplace_back();
    doc->text = JoinLines(doc->lines);
    doc->path = path;
    doc->parse = goparse::ParseFile(doc->text);
    doc->host_build = BuildsOnHost(doc->parse.file, path);
    Resolver r(*doc);
    r.Run();
    if (!doc->parse.errors.empty()) doc->semantic.clear();  // a damaged tree makes every use-based check guess
    return doc;
}

std::vector<GoLspDiagnostic> GoLspDiagnostics(const GoLspDocument &doc) {
    std::vector<GoLspDiagnostic> out;
    for (const goparse::Error &e : doc.parse.errors) {
        GoLspDiagnostic d;
        d.range.line = d.range.end_line = e.line - 1;
        d.range.col = e.col - 1;
        // underline the token at the position
        const std::string &l = doc.lines[static_cast<size_t>(std::clamp(e.line - 1, 0, static_cast<int>(doc.lines.size()) - 1))];
        size_t c = static_cast<size_t>(std::max(0, e.col - 1));
        size_t end = c;
        while (end < l.size() && (std::isalnum(static_cast<unsigned char>(l[end])) || l[end] == '_')) end++;
        if (end == c && c < l.size()) end = c + 1;
        d.range.end_col = static_cast<int>(end);
        d.severity = GoLspSeverity::Error;
        d.code = "syntax-error";
        d.message = e.msg;
        out.push_back(std::move(d));
    }
    out.insert(out.end(), doc.semantic.begin(), doc.semantic.end());
    std::stable_sort(out.begin(), out.end(), [](const GoLspDiagnostic &a, const GoLspDiagnostic &b) {
        return a.range.line != b.range.line ? a.range.line < b.range.line : a.range.col < b.range.col;
    });
    return out;
}

std::string GoLspFormatDiagnostic(const std::string &path, const GoLspDiagnostic &d) {
    const char *sev = d.severity == GoLspSeverity::Error         ? "error"
                      : d.severity == GoLspSeverity::Warning     ? "warning"
                      : d.severity == GoLspSeverity::Information ? "info"
                                                                 : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool GoLspDiagnosticFails(const GoLspDiagnostic &d) {
    return d.severity == GoLspSeverity::Error || d.severity == GoLspSeverity::Warning;
}

std::vector<GoLspTextEdit> GoLspFormat(const std::vector<std::string> &lines, int width, std::string *error) {
    std::vector<GoLspTextEdit> edits;
    const std::string text = JoinLines(lines);
    gofmt::Options o;
    if (width > 0) o.width = width;
    const gofmt::Result r = gofmt::Format(text, o);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    if (r.text == text) return edits;
    // One edit over the lines that changed.
    std::vector<std::string> b;
    for (size_t i = 0;;) {
        const size_t j = r.text.find('\n', i);
        if (j == std::string::npos) {
            b.push_back(r.text.substr(i));
            break;
        }
        b.push_back(r.text.substr(i, j - i));
        i = j + 1;
    }
    size_t pre = 0;
    while (pre < lines.size() && pre < b.size() && lines[pre] == b[pre]) pre++;
    size_t suf = 0;
    while (suf < lines.size() - pre && suf < b.size() - pre && lines[lines.size() - 1 - suf] == b[b.size() - 1 - suf]) suf++;
    GoLspTextEdit e;
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
