// mep's Go language server: the editor's queries over the analysed
// document (go_lsp.cpp builds it) -- completion, hover, signature help,
// definition, references, rename, outline and folding -- plus the
// best-effort typing they share. Types come from what is written: declared
// types, composite literals, conversions, and the results of functions
// declared in this file or in the standard library (whose signatures in
// go_lsp_vocab_data.cpp are parsed with the same parser on demand). There
// is no inference across files and no generics instantiation.

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>

#include "go_lsp.h"
#include "go_lsp_internal.h"
#include "go_lsp_vocab.h"

using namespace goast;
using golsp::Member;
using golsp::Ref;
using golsp::Sym;
using golsp::SymKind;
using golsp::TypeRef;

namespace {

// Standard-library signatures parsed into trees, kept for the process's life.
struct Parsed {
    goparse::Result r;
};
const File *ParseSnippet(const std::string &src) {
    static std::map<std::string, std::unique_ptr<Parsed>> cache;
    auto it = cache.find(src);
    if (it == cache.end()) {
        auto p = std::make_unique<Parsed>();
        p->r = goparse::ParseFile("package p\n" + src + "\n");
        it = cache.emplace(src, std::move(p)).first;
    }
    return it->second->r.errors.empty() ? it->second->r.file : nullptr;
}

// The declaration in a vocab entry's signature.
Node *VocabDecl(const GoVocabEntry *e) {
    std::string sig = e->signature;
    if (e->kind == GoVocabKind::Field) sig = "var " + sig;
    const File *f = ParseSnippet(sig);
    if (!f || f->Decls.empty()) return nullptr;
    return f->Decls[0];
}

FuncType *VocabFuncType(const GoVocabEntry *e) {
    if (!e || (e->kind != GoVocabKind::Func && e->kind != GoVocabKind::Method)) return nullptr;
    auto *fd = As<FuncDecl>(VocabDecl(e));
    return fd ? fd->Type : nullptr;
}

Node *VocabVarType(const GoVocabEntry *e) {
    auto *g = As<GenDecl>(VocabDecl(e));
    if (!g || g->Specs.empty()) return nullptr;
    auto *vs = As<ValueSpec>(g->Specs[0]);
    return vs ? vs->Type : nullptr;
}

Node *VocabTypeSpecType(const GoVocabEntry *e) {
    auto *g = As<GenDecl>(VocabDecl(e));
    if (!g || g->Specs.empty()) return nullptr;
    auto *ts = As<TypeSpec>(g->Specs[0]);
    return ts ? ts->Type : nullptr;
}

// A package name used inside a standard-library signature (`io.Reader`) to its path.
std::string StdPathForName(const std::string &name) {
    if (GoVocabFind(name, "")) return name;
    static std::map<std::string, std::string> memo;
    auto it = memo.find(name);
    if (it != memo.end()) return it->second;
    std::string found;
    for (std::string_view p : GoVocabPackages()) {
        if (golsp::StdPackageName(std::string(p)) == name && p.find("internal") == std::string_view::npos) {
            found = std::string(p);
            break;
        }
    }
    memo[name] = found;
    return found;
}

int SymOfIdent(const GoLspDocument &doc, const Ident *id) {
    const Ref *r = doc.RefAt(id->NamePos);
    if (r && r->ident == id) return r->sym;
    return doc.Lookup(id->Name, id->NamePos);
}

// A named type: declared in this file (sym), or in the standard library (path + name).
struct Named {
    int sym = -1;
    std::string path, name;
    Node *literal = nullptr;  // an unnamed struct/interface type
    std::string literal_pkg;
    bool ok() const { return sym >= 0 || !path.empty() || literal; }
};

Named NamedOf(const GoLspDocument &doc, TypeRef t, int depth = 0) {
    Named n;
    if (!t.ok() || depth > 12) return n;
    Node *e = Unparen(t.expr);
    if (auto *s = As<StarExpr>(e)) return NamedOf(doc, {s->X, t.pkg}, depth + 1);
    if (auto *ix = As<IndexExpr>(e)) return NamedOf(doc, {ix->X, t.pkg}, depth + 1);
    if (auto *il = As<IndexListExpr>(e)) return NamedOf(doc, {il->X, t.pkg}, depth + 1);
    if (auto *id = As<Ident>(e)) {
        if (t.pkg.empty()) {
            int s = SymOfIdent(doc, id);
            if (s >= 0 && doc.syms[static_cast<size_t>(s)].kind == SymKind::Type) n.sym = s;
        } else if (GoVocabFind(t.pkg, id->Name)) {
            n.path = t.pkg;
            n.name = id->Name;
        }
        return n;
    }
    if (auto *sel = As<SelectorExpr>(e)) {
        auto *x = As<Ident>(sel->X);
        if (!x) return n;
        std::string path;
        if (t.pkg.empty()) {
            int s = SymOfIdent(doc, x);
            if (s >= 0 && doc.syms[static_cast<size_t>(s)].kind == SymKind::Import) path = doc.syms[static_cast<size_t>(s)].import_path;
        } else {
            path = StdPathForName(x->Name);
        }
        if (!path.empty() && GoVocabFind(path, sel->Sel->Name)) {
            n.path = path;
            n.name = sel->Sel->Name;
        }
        return n;
    }
    if (e->kind == K::StructType || e->kind == K::InterfaceType) {
        n.literal = e;
        n.literal_pkg = t.pkg;
    }
    return n;
}

// The type's definition (the right-hand side of `type T ...`), to look through to slices, maps, ...
TypeRef Underlying(const GoLspDocument &doc, TypeRef t, int depth = 0) {
    if (!t.ok() || depth > 12) return {};
    Node *e = Unparen(t.expr);
    if (auto *s = As<StarExpr>(e)) return Underlying(doc, {s->X, t.pkg}, depth + 1);
    Named n = NamedOf(doc, {e, t.pkg});
    if (n.sym >= 0) return Underlying(doc, {doc.syms[static_cast<size_t>(n.sym)].type, ""}, depth + 1);
    if (!n.path.empty()) {
        const GoVocabEntry *v = GoVocabFind(n.path, n.name);
        Node *rhs = v && v->kind == GoVocabKind::Type ? VocabTypeSpecType(v) : nullptr;
        if (rhs && !As<StructType>(rhs)) return Underlying(doc, {rhs, n.path}, depth + 1);
        return {e, t.pkg};
    }
    return {e, t.pkg};
}

TypeRef ResultOf(FuncType *ft, const std::string &pkg, int index) {
    if (!ft || !ft->Results) return {};
    int k = 0;
    for (Field *f : ft->Results->List) {
        int n = std::max<int>(1, static_cast<int>(f->Names.size()));
        if (index < k + n) return {f->Type, pkg};
        k += n;
    }
    return {};
}

struct FuncRef {
    FuncType *ft = nullptr;
    std::string pkg;
};

FuncRef FuncOf(const GoLspDocument &doc, Node *fun, Pos at, int depth);

}  // namespace

namespace golsp {

std::vector<Member> MembersOf(const GoLspDocument &doc, const TypeRef &t) {
    std::vector<Member> out;
    std::set<std::string> seen;
    std::function<void(const TypeRef &, int)> add = [&](const TypeRef &tr, int depth) {
        if (depth > 4) return;
        Named n = NamedOf(doc, tr);
        Node *body = nullptr;
        std::string body_pkg;
        if (n.sym >= 0) {
            const std::string &tname = doc.syms[static_cast<size_t>(n.sym)].name;
            for (size_t i = 0; i < doc.syms.size(); i++) {
                const Sym &s = doc.syms[i];
                if (s.kind != SymKind::Method || s.import_path != tname || !seen.insert(s.name).second) continue;
                Member m;
                m.name = s.name;
                m.method = true;
                m.pos = s.pos;
                m.detail = DeclText(doc, s);
                m.doc = CommentGroupText(s.doc);
                m.type = {s.type, ""};
                out.push_back(std::move(m));
            }
            body = doc.syms[static_cast<size_t>(n.sym)].type;
            body = body ? Unparen(body) : nullptr;
        } else if (!n.path.empty()) {
            for (const GoVocabEntry *e : GoVocabTypeMembers(n.path, n.name)) {
                std::string mname = std::string(e->name).substr(n.name.size() + 1);
                if (!seen.insert(mname).second) continue;
                Member m;
                m.name = mname;
                m.method = e->kind == GoVocabKind::Method;
                m.detail = e->signature;
                m.doc = e->doc;
                m.deprecated = e->deprecated;
                if (m.method) {
                    m.type = {VocabFuncType(e), n.path};
                } else {
                    m.type = {VocabVarType(e), n.path};
                }
                out.push_back(std::move(m));
            }
            const GoVocabEntry *te = GoVocabFind(n.path, n.name);
            Node *rhs = te ? VocabTypeSpecType(te) : nullptr;
            if (auto *it = As<InterfaceType>(rhs)) {
                body = it;
                body_pkg = n.path;
            }
        } else if (n.literal) {
            body = n.literal;
            body_pkg = n.literal_pkg;
        }
        if (auto *st = As<StructType>(body)) {
            for (Field *f : st->Fields->List) {
                if (f->Names.empty()) {
                    // embedded: its own name is a field, and its members are promoted
                    Node *et = f->Type;
                    if (auto *s = As<StarExpr>(et)) et = s->X;
                    std::string ename;
                    if (auto *id = As<Ident>(et)) ename = id->Name;
                    if (auto *sel = As<SelectorExpr>(et)) ename = sel->Sel->Name;
                    if (!ename.empty() && seen.insert(ename).second) {
                        Member m;
                        m.name = ename;
                        m.detail = ename + " " + doc.SliceNode(f->Type);
                        m.pos = body_pkg.empty() ? NodePos(f->Type) : NoPos;
                        m.type = {f->Type, body_pkg};
                        m.doc = CommentGroupText(f->Doc ? f->Doc : f->Comment);
                        out.push_back(std::move(m));
                    }
                    add({f->Type, body_pkg}, depth + 1);
                    continue;
                }
                for (Ident *id : f->Names) {
                    if (!seen.insert(id->Name).second) continue;
                    Member m;
                    m.name = id->Name;
                    m.detail = body_pkg.empty() ? "field " + id->Name + " " + doc.SliceNode(f->Type) : id->Name;
                    m.pos = body_pkg.empty() ? id->NamePos : NoPos;
                    m.type = {f->Type, body_pkg};
                    m.doc = CommentGroupText(f->Doc ? f->Doc : f->Comment);
                    out.push_back(std::move(m));
                }
            }
        } else if (auto *it = As<InterfaceType>(body)) {
            for (Field *f : it->Methods->List) {
                if (f->Names.empty()) {
                    add({f->Type, body_pkg}, depth + 1);
                    continue;
                }
                for (Ident *id : f->Names) {
                    if (!seen.insert(id->Name).second) continue;
                    Member m;
                    m.name = id->Name;
                    m.method = true;
                    // FuncType source without `func`: "(x int) error"
                    m.detail = body_pkg.empty() ? "func " + id->Name + doc.SliceNode(f->Type) : id->Name;
                    m.pos = body_pkg.empty() ? id->NamePos : NoPos;
                    m.type = {f->Type, body_pkg};
                    m.doc = CommentGroupText(f->Doc ? f->Doc : f->Comment);
                    out.push_back(std::move(m));
                }
            }
        }
    };
    add(t, 0);
    return out;
}

TypeRef TypeOf(const GoLspDocument &doc, Node *e, Pos at, int depth) {
    if (!e || depth > 16) return {};
    e = Unparen(e);
    switch (e->kind) {
    case K::Ident: {
        auto *id = static_cast<Ident *>(e);
        int s = SymOfIdent(doc, id);
        if (s < 0) return {};
        const Sym &sym = doc.syms[static_cast<size_t>(s)];
        switch (sym.kind) {
        case SymKind::Var: case SymKind::Const: case SymKind::Param: case SymKind::Result: case SymKind::TypeSwitch: {
            if (sym.type) return {sym.type, ""};
            if (!sym.value) return {};
            if (sym.range_key || sym.range_value) {
                TypeRef x = Underlying(doc, TypeOf(doc, sym.value, at, depth + 1));
                if (!x.ok()) return {};
                Node *u = Unparen(x.expr);
                if (auto *m = As<MapType>(u)) return {sym.range_key ? m->Key : m->Value, x.pkg};
                if (auto *a = As<ArrayType>(u); a && sym.range_value) return {a->Elt, x.pkg};
                if (auto *c = As<ChanType>(u); c && sym.range_key) return {c->Value, x.pkg};
                return {};
            }
            if (sym.kind == SymKind::TypeSwitch) return {};
            if (sym.value_index >= 0) {
                auto *call = As<CallExpr>(Unparen(sym.value));
                if (!call) return {};
                FuncRef f = FuncOf(doc, call->Fun, at, depth + 1);
                return ResultOf(f.ft, f.pkg, sym.value_index);
            }
            return TypeOf(doc, sym.value, at, depth + 1);
        }
        default: return {};
        }
    }
    case K::SelectorExpr: {
        auto *sel = static_cast<SelectorExpr *>(e);
        if (auto *x = As<Ident>(sel->X)) {
            int s = SymOfIdent(doc, x);
            if (s >= 0 && doc.syms[static_cast<size_t>(s)].kind == SymKind::Import) {
                const GoVocabEntry *v = GoVocabFind(doc.syms[static_cast<size_t>(s)].import_path, sel->Sel->Name);
                if (v && v->kind == GoVocabKind::Var) return {VocabVarType(v), v->pkg};
                return {};
            }
        }
        TypeRef base = TypeOf(doc, sel->X, at, depth + 1);
        for (const Member &m : MembersOf(doc, base)) {
            if (m.name == sel->Sel->Name && !m.method) return m.type;
        }
        return {};
    }
    case K::CallExpr: {
        auto *call = static_cast<CallExpr *>(e);
        Node *fun = Unparen(call->Fun);
        if (auto *id = As<Ident>(fun)) {
            int s = SymOfIdent(doc, id);
            if (s >= 0 && doc.syms[static_cast<size_t>(s)].kind == SymKind::Type) return {fun, ""};
            if (s < 0) {
                if ((id->Name == "new") && !call->Args.empty()) return {call->Args[0], ""};
                if ((id->Name == "make") && !call->Args.empty()) return {call->Args[0], ""};
                if (id->Name == "append" && !call->Args.empty()) return TypeOf(doc, call->Args[0], at, depth + 1);
            }
        }
        if (As<ArrayType>(fun) || As<MapType>(fun) || As<ChanType>(fun) || As<FuncType>(fun)) return {fun, ""};
        if (auto *sel = As<SelectorExpr>(fun)) {
            Named n = NamedOf(doc, {sel, ""});
            if (!n.path.empty()) {
                const GoVocabEntry *v = GoVocabFind(n.path, n.name);
                if (v && v->kind == GoVocabKind::Type) return {sel, ""};  // a conversion
            }
        }
        FuncRef f = FuncOf(doc, fun, at, depth + 1);
        return ResultOf(f.ft, f.pkg, 0);
    }
    case K::CompositeLit: {
        auto *c = static_cast<CompositeLit *>(e);
        return c->Type ? TypeRef{c->Type, ""} : TypeRef{};
    }
    case K::UnaryExpr: {
        auto *u = static_cast<UnaryExpr *>(e);
        if (u->Op == Tok::AND) return TypeOf(doc, u->X, at, depth + 1);
        if (u->Op == Tok::ARROW) {
            TypeRef c = Underlying(doc, TypeOf(doc, u->X, at, depth + 1));
            if (auto *ct = As<ChanType>(c.expr)) return {ct->Value, c.pkg};
        }
        return {};
    }
    case K::StarExpr: {
        TypeRef p = TypeOf(doc, static_cast<StarExpr *>(e)->X, at, depth + 1);
        if (auto *s = As<StarExpr>(p.expr ? Unparen(p.expr) : nullptr)) return {s->X, p.pkg};
        return p;
    }
    case K::IndexExpr: {
        auto *ix = static_cast<IndexExpr *>(e);
        TypeRef x = Underlying(doc, TypeOf(doc, ix->X, at, depth + 1));
        if (!x.ok()) return {};
        if (auto *m = As<MapType>(x.expr)) return {m->Value, x.pkg};
        if (auto *a = As<ArrayType>(x.expr)) return {a->Elt, x.pkg};
        return {};
    }
    case K::SliceExpr: return TypeOf(doc, static_cast<SliceExpr *>(e)->X, at, depth + 1);
    case K::TypeAssertExpr: {
        auto *ta = static_cast<TypeAssertExpr *>(e);
        return ta->Type ? TypeRef{ta->Type, ""} : TypeRef{};
    }
    case K::FuncLit: return {static_cast<FuncLit *>(e)->Type, ""};
    default: return {};
    }
}

std::string DeclText(const GoLspDocument &doc, const Sym &s) {
    auto first_lines = [](std::string t, int max_lines) {
        int n = 0;
        for (size_t i = 0; i < t.size(); i++) {
            if (t[i] == '\n' && ++n >= max_lines) return t.substr(0, i) + "\n\t// ...";
        }
        return t;
    };
    switch (s.kind) {
    case SymKind::Func:
    case SymKind::Method: {
        auto *fd = As<FuncDecl>(s.decl);
        if (!fd) return s.name;
        Pos end = fd->Body ? fd->Body->Lbrace : NodeEnd(fd);
        std::string t = doc.Slice(NodePos(fd->Type), end);
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
        return t;
    }
    case SymKind::Type: {
        auto *ts = As<TypeSpec>(s.decl);
        return ts ? first_lines("type " + doc.SliceNode(ts), 20) : "type " + s.name;
    }
    case SymKind::Var:
    case SymKind::Const: {
        std::string kw = s.kind == SymKind::Const ? "const " : "var ";
        if (s.type) return kw + s.name + " " + doc.SliceNode(s.type);
        if (s.value && s.value_index < 0 && !s.range_key && !s.range_value) {
            std::string v = first_lines(doc.SliceNode(s.value), 1);
            return kw + s.name + " = " + v;
        }
        return kw + s.name;
    }
    case SymKind::Param:
    case SymKind::Result: return "var " + s.name + (s.type ? " " + doc.SliceNode(s.type) : "");
    case SymKind::TypeParam: return "type " + s.name + (s.type ? " " + doc.SliceNode(s.type) : "");
    case SymKind::Import: return "package " + s.name + " (\"" + s.import_path + "\")";
    case SymKind::Label: return "label " + s.name;
    case SymKind::TypeSwitch: return "var " + s.name;
    case SymKind::Field: return "field " + s.name;
    }
    return s.name;
}

}  // namespace golsp

namespace {

FuncRef FuncOf(const GoLspDocument &doc, Node *fun, Pos at, int depth) {
    FuncRef out;
    if (!fun || depth > 16) return out;
    fun = Unparen(fun);
    if (auto *ix = As<IndexExpr>(fun)) fun = ix->X;  // an explicit instantiation
    if (auto *il = As<IndexListExpr>(fun)) fun = il->X;
    if (auto *id = As<Ident>(fun)) {
        int s = SymOfIdent(doc, id);
        if (s >= 0) {
            const Sym &sym = doc.syms[static_cast<size_t>(s)];
            if (sym.kind == SymKind::Func) return {As<FuncType>(sym.type), ""};
            TypeRef t = golsp::TypeOf(doc, id, at, depth + 1);
            TypeRef u = Underlying(doc, t);
            if (auto *ft = As<FuncType>(u.expr)) return {ft, u.pkg};
        }
        return out;
    }
    if (auto *sel = As<SelectorExpr>(fun)) {
        if (auto *x = As<Ident>(sel->X)) {
            int s = SymOfIdent(doc, x);
            if (s >= 0 && doc.syms[static_cast<size_t>(s)].kind == SymKind::Import) {
                const GoVocabEntry *v = GoVocabFind(doc.syms[static_cast<size_t>(s)].import_path, sel->Sel->Name);
                if (v && v->kind == GoVocabKind::Func) return {VocabFuncType(v), v->pkg};
                if (v && v->kind == GoVocabKind::Var) {
                    TypeRef u = Underlying(doc, {VocabVarType(v), v->pkg});
                    if (auto *ft = As<FuncType>(u.expr)) return {ft, u.pkg};
                }
                return out;
            }
        }
        TypeRef base = golsp::TypeOf(doc, sel->X, at, depth + 1);
        for (const Member &m : golsp::MembersOf(doc, base)) {
            if (m.name != sel->Sel->Name) continue;
            if (auto *ft = As<FuncType>(m.type.expr)) return {ft, m.type.pkg};
            TypeRef u = Underlying(doc, m.type);
            if (auto *ft = As<FuncType>(u.expr)) return {ft, u.pkg};
        }
        return out;
    }
    if (auto *fl = As<FuncLit>(fun)) return {fl->Type, ""};
    return out;
}

// A type expression as Go source. Names from a standard-library signature get
// their package's name (`Response` read from net/http is `http.Response`).
std::string RenderType(const GoLspDocument &doc, Node *e, const std::string &pkg, int depth = 0) {
    if (!e || depth > 20) return "";
    auto sub = [&](Node *x) { return RenderType(doc, x, pkg, depth + 1); };
    if (pkg.empty()) return doc.SliceNode(e);
    switch (e->kind) {
    case K::Ident: {
        const std::string &n = static_cast<Ident *>(e)->Name;
        if (golsp::IsUniverse(n) || !IsExported(n)) return n;
        return golsp::StdPackageName(pkg) + "." + n;
    }
    case K::SelectorExpr: return sub(static_cast<SelectorExpr *>(e)->X) == "" ? "" :
                                 static_cast<Ident *>(static_cast<SelectorExpr *>(e)->X)->Name + "." +
                                     static_cast<SelectorExpr *>(e)->Sel->Name;
    case K::StarExpr: return "*" + sub(static_cast<StarExpr *>(e)->X);
    case K::ParenExpr: return "(" + sub(static_cast<ParenExpr *>(e)->X) + ")";
    case K::Ellipsis: return "..." + sub(static_cast<Ellipsis *>(e)->Elt);
    case K::ArrayType: {
        auto *a = static_cast<ArrayType *>(e);
        std::string len;
        if (auto *bl = As<BasicLit>(a->Len)) len = bl->Value;
        else if (a->Len) len = "...";
        return "[" + len + "]" + sub(a->Elt);
    }
    case K::MapType: return "map[" + sub(static_cast<MapType *>(e)->Key) + "]" + sub(static_cast<MapType *>(e)->Value);
    case K::ChanType: {
        auto *c = static_cast<ChanType *>(e);
        std::string head = c->Dir == RECV ? "<-chan " : c->Dir == SEND ? "chan<- " : "chan ";
        return head + sub(c->Value);
    }
    case K::StructType: return "struct{...}";
    case K::InterfaceType: return static_cast<InterfaceType *>(e)->Methods->List.empty() ? "interface{}" : "interface{...}";
    case K::FuncType: {
        auto *f = static_cast<FuncType *>(e);
        auto list = [&](FieldList *fl) {
            std::string out;
            if (!fl) return out;
            for (Field *fld : fl->List) {
                int n = std::max<int>(1, static_cast<int>(fld->Names.size()));
                for (int k = 0; k < n; k++) {
                    if (!out.empty()) out += ", ";
                    out += sub(fld->Type);
                }
            }
            return out;
        };
        std::string r = "func(" + list(f->Params) + ")";
        std::string res = list(f->Results);
        if (!res.empty()) r += res.find(',') != std::string::npos ? " (" + res + ")" : " " + res;
        return r;
    }
    case K::IndexExpr: return sub(static_cast<IndexExpr *>(e)->X) + "[" + sub(static_cast<IndexExpr *>(e)->Index) + "]";
    default: return "";
    }
}

bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || (c & 0x80); }

std::string Markdown(const std::string &decl, const std::string &doc, const std::string &link = "") {
    std::string out = "```go\n" + decl + "\n```";
    if (!doc.empty()) out += "\n\n" + doc;
    if (!link.empty()) out += "\n\n" + link;
    return out;
}

std::string PkgLink(const std::string &path, const std::string &name) {
    std::string anchor = name.empty() ? "" : "#" + name;
    std::string label = name.empty() ? path : path + "." + name;
    return "[`" + label + "` on pkg.go.dev](https://pkg.go.dev/" + path + anchor + ")";
}

std::string VocabHover(const GoVocabEntry *e) {
    std::string sig = e->signature;
    std::string doc = e->doc;
    std::string link = std::string(e->pkg) == "builtin" ? "" : PkgLink(e->pkg, e->name);
    return Markdown(sig, doc, link);
}

GoLspKind CompletionKind(const Sym &s) {
    switch (s.kind) {
    case SymKind::Func: return GoLspKind::Function;
    case SymKind::Method: return GoLspKind::Method;
    case SymKind::Type: {
        auto *ts = As<TypeSpec>(s.decl);
        if (ts && As<StructType>(ts->Type)) return GoLspKind::Struct;
        if (ts && As<InterfaceType>(ts->Type)) return GoLspKind::Interface;
        return GoLspKind::Class;
    }
    case SymKind::Const: return GoLspKind::Constant;
    case SymKind::Import: return GoLspKind::Module;
    case SymKind::TypeParam: return GoLspKind::TypeParameter;
    case SymKind::Field: return GoLspKind::Field;
    default: return GoLspKind::Variable;
    }
}

GoLspKind VocabKind(GoVocabKind k) {
    switch (k) {
    case GoVocabKind::Func: return GoLspKind::Function;
    case GoVocabKind::Method: return GoLspKind::Method;
    case GoVocabKind::Type: return GoLspKind::Class;
    case GoVocabKind::Const: return GoLspKind::Constant;
    case GoVocabKind::Var: return GoLspKind::Variable;
    case GoVocabKind::Field: return GoLspKind::Field;
    default: return GoLspKind::Module;
    }
}

const char *const kKeywords[] = {"break", "case", "chan", "const", "continue", "default", "defer", "else",
                                 "fallthrough", "for", "func", "go", "goto", "if", "import", "interface", "map",
                                 "package", "range", "return", "select", "struct", "switch", "type", "var"};

// The members visible after `base.` where `base` is the reference ending just before the dot.
std::vector<Member> MembersAfter(const GoLspDocument &doc, const Ref *r) {
    if (!r) return {};
    if (r->selector_base) {
        TypeRef base = golsp::TypeOf(doc, r->selector_base, r->pos);
        if (auto *x = As<Ident>(r->selector_base)) {
            int s = SymOfIdent(doc, x);
            if (s >= 0 && doc.syms[static_cast<size_t>(s)].kind == SymKind::Import) {
                const GoVocabEntry *v = GoVocabFind(doc.syms[static_cast<size_t>(s)].import_path, r->ident->Name);
                if (v && v->kind == GoVocabKind::Var) return golsp::MembersOf(doc, {VocabVarType(v), v->pkg});
                return {};
            }
        }
        for (const Member &m : golsp::MembersOf(doc, base)) {
            if (m.name == r->ident->Name && !m.method) return golsp::MembersOf(doc, m.type);
        }
        return {};
    }
    return golsp::MembersOf(doc, golsp::TypeOf(doc, r->ident, r->pos));
}

// The declaring position of what a reference names: the symbol, or a field/method found through the base's type.
Pos DeclPosOf(const GoLspDocument &doc, const Ref &r) {
    if (r.sym >= 0) return doc.syms[static_cast<size_t>(r.sym)].pos;
    if (r.selector_base) {
        TypeRef base = golsp::TypeOf(doc, r.selector_base, r.pos);
        for (const Member &m : golsp::MembersOf(doc, base)) {
            if (m.name == r.ident->Name) return m.pos;
        }
    }
    return NoPos;
}

}  // namespace

// --- completion -------------------------------------------------------------------

std::vector<GoLspCompletionItem> GoLspCompletions(const GoLspDocument &doc, int line, int col) {
    std::vector<GoLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return out;
    const std::string &cur = doc.lines[static_cast<size_t>(line)];
    col = std::clamp(col, 0, static_cast<int>(cur.size()));
    int start = col;
    while (start > 0 && IsIdentChar(cur[static_cast<size_t>(start - 1)])) start--;
    const std::string prefix = cur.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    const Pos at = doc.PosAt(line, col);
    auto add = [&](const std::string &label, GoLspKind kind, const std::string &detail, const std::string &docs,
                   int rs = -1) {
        if (label.compare(0, prefix.size(), prefix) != 0 && rs < 0) return;
        GoLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = docs;
        it.replace_start = rs >= 0 ? rs : start;
        it.replace_end = col;
        out.push_back(std::move(it));
    };

    // an import path
    {
        int q = col;
        while (q > 0 && cur[static_cast<size_t>(q - 1)] != '"' && cur[static_cast<size_t>(q - 1)] != ' ' &&
               cur[static_cast<size_t>(q - 1)] != '\t') {
            q--;
        }
        bool in_import = false;
        for (Node *d : doc.parse.file->Decls) {
            auto *g = As<GenDecl>(d);
            if (g && g->Tok_ == Tok::IMPORT && NodePos(g) <= at && at <= NodeEnd(g) + 1) in_import = true;
        }
        std::string head = cur.substr(0, static_cast<size_t>(std::max(0, q - 1)));
        size_t nonblank = head.find_first_not_of(" \t");
        bool looks = nonblank == std::string::npos || head.compare(nonblank, 6, "import") == 0 ||
                     std::all_of(head.begin() + static_cast<long>(nonblank), head.end(),
                                 [](char c) { return IsIdentChar(c) || c == ' ' || c == '.' || c == '\t'; });
        if (q > 0 && cur[static_cast<size_t>(q - 1)] == '"' && (in_import || (looks && cur.find("import") != std::string::npos))) {
            std::string pp = cur.substr(static_cast<size_t>(q), static_cast<size_t>(col - q));
            for (std::string_view p : GoVocabPackages()) {
                if (p.compare(0, pp.size(), pp) != 0 || p.find("internal") != std::string_view::npos) continue;
                const GoVocabEntry *e = GoVocabFind(p, "");
                GoLspCompletionItem it;
                it.label = std::string(p);
                it.insert_text = it.label;
                it.kind = GoLspKind::Module;
                it.detail = e ? e->signature : "";
                it.documentation = e ? e->doc : "";
                it.replace_start = q;
                it.replace_end = col;
                out.push_back(std::move(it));
            }
            return out;
        }
    }

    // after `x.`
    if (start > 0 && cur[static_cast<size_t>(start - 1)] == '.') {
        int bend = start - 1;
        int bstart = bend;
        while (bstart > 0 && IsIdentChar(cur[static_cast<size_t>(bstart - 1)])) bstart--;
        std::string base = cur.substr(static_cast<size_t>(bstart), static_cast<size_t>(bend - bstart));
        if (base.empty() || std::isdigit(static_cast<unsigned char>(base[0]))) return out;
        int s = doc.Lookup(base, at);
        if (s >= 0 && doc.syms[static_cast<size_t>(s)].kind == SymKind::Import) {
            const std::string &path = doc.syms[static_cast<size_t>(s)].import_path;
            for (const GoVocabEntry *e : GoVocabMembers(path)) add(e->name, VocabKind(e->kind), e->signature, e->doc);
            return out;
        }
        const Ref *r = doc.RefAt(doc.PosAt(line, bend));
        if (r && r->ident && r->ident->Name != base) r = nullptr;
        std::vector<Member> members;
        if (r) {
            members = MembersAfter(doc, r);
        } else if (s >= 0) {
            const Sym &sym = doc.syms[static_cast<size_t>(s)];
            if (sym.type) members = golsp::MembersOf(doc, {sym.type, ""});
            else if (sym.value && sym.value_index < 0) members = golsp::MembersOf(doc, golsp::TypeOf(doc, sym.value, at));
        }
        for (const Member &m : members) add(m.name, m.method ? GoLspKind::Method : GoLspKind::Field, m.detail, m.doc);
        return out;
    }

    // a name in scope
    std::set<std::string> seen;
    for (int sc = doc.ScopeAt(at); sc >= 0; sc = doc.scopes[static_cast<size_t>(sc)].parent) {
        for (const auto &[name, idx] : doc.scopes[static_cast<size_t>(sc)].names) {
            const Sym &s = doc.syms[static_cast<size_t>(idx)];
            if (s.local && s.pos > at) continue;
            if (s.pos <= at && at <= s.pos + static_cast<int>(s.name.size())) continue;  // the word being typed
            if (!seen.insert(name).second) continue;
            add(name, CompletionKind(s), golsp::DeclText(doc, s), CommentGroupText(s.doc));
        }
    }
    if (!prefix.empty()) {
        for (const GoVocabEntry *e : GoVocabMembers("builtin")) {
            if (seen.count(e->name)) continue;
            seen.insert(e->name);
            add(e->name, VocabKind(e->kind), e->signature, e->doc);
        }
        for (const char *k : kKeywords) {
            if (!seen.count(k)) add(k, GoLspKind::Keyword, "", "");
        }
        // standard-library packages not imported yet: the name, with the path as detail
        for (std::string_view p : GoVocabPackages()) {
            if (p.find("internal") != std::string_view::npos || p == "builtin") continue;
            std::string name = golsp::StdPackageName(std::string(p));
            if (name.empty() || name == "main" || seen.count(name) || name.compare(0, prefix.size(), prefix) != 0) continue;
            seen.insert(name);
            add(name, GoLspKind::Module, "\"" + std::string(p) + "\" (not imported)", "");
        }
    }
    return out;
}

// --- hover ------------------------------------------------------------------------

GoLspHoverInfo GoLspHover(const GoLspDocument &doc, int line, int col) {
    GoLspHoverInfo info;
    const Pos at = doc.PosAt(line, col);
    // an import path
    for (const golsp::ImportInfo &im : doc.imports) {
        BasicLit *p = im.spec->Path;
        if (p->ValuePos <= at && at < NodeEnd(p)) {
            const GoVocabEntry *e = GoVocabFind(im.path, "");
            std::string text = Markdown("package " + im.name + " // import \"" + im.path + "\"", e ? e->doc : "",
                                        e ? PkgLink(im.path, "") : "");
            info.found = true;
            info.text = text;
            info.range = doc.RangeOfNode(p);
            return info;
        }
    }
    const Ref *r = doc.RefAt(at);
    if (!r || !r->ident) return info;
    info.range = doc.RangeOf(r->pos, r->pos + r->len);
    const std::string &name = r->ident->Name;
    if (r->sym >= 0) {
        const Sym &s = doc.syms[static_cast<size_t>(r->sym)];
        std::string decl = golsp::DeclText(doc, s);
        std::string docs = CommentGroupText(s.doc);
        std::string link;
        if (s.kind == SymKind::Import) {
            if (const GoVocabEntry *e = GoVocabFind(s.import_path, "")) {
                docs = e->doc;
                link = PkgLink(s.import_path, "");
            }
        }
        if ((s.kind == SymKind::Var || s.kind == SymKind::Const) && !s.type) {
            TypeRef t = golsp::TypeOf(doc, r->ident, r->pos);
            if (t.ok()) {
                std::string ts = RenderType(doc, t.expr, t.pkg);
                if (!ts.empty()) decl = (s.kind == SymKind::Const ? "const " : "var ") + s.name + " " + ts;
            }
        }
        info.found = true;
        info.text = Markdown(decl, docs, link);
        return info;
    }
    if (r->selector_base) {
        if (auto *x = As<Ident>(r->selector_base)) {
            int s = SymOfIdent(doc, x);
            if (s >= 0 && doc.syms[static_cast<size_t>(s)].kind == SymKind::Import) {
                if (const GoVocabEntry *e = GoVocabFind(doc.syms[static_cast<size_t>(s)].import_path, name)) {
                    info.found = true;
                    info.text = VocabHover(e);
                }
                return info;
            }
        }
        TypeRef base = golsp::TypeOf(doc, r->selector_base, r->pos);
        Named n = NamedOf(doc, base);
        for (const Member &m : golsp::MembersOf(doc, base)) {
            if (m.name != name) continue;
            info.found = true;
            std::string link;
            if (!n.path.empty()) link = PkgLink(n.path, n.name + "." + name);
            info.text = Markdown(m.detail, m.doc, link);
            return info;
        }
        return info;
    }
    if (golsp::IsUniverse(name)) {
        if (const GoVocabEntry *e = GoVocabFind("builtin", name)) {
            info.found = true;
            info.text = VocabHover(e);
        } else {
            info.found = true;
            info.text = Markdown("type " + name, "A predeclared type.");
        }
    }
    return info;
}

// --- signature help -------------------------------------------------------------------

GoLspSignatureHelp GoLspSignatureAt(const GoLspDocument &doc, int line, int col) {
    GoLspSignatureHelp sh;
    const Pos at = doc.PosAt(line, col);
    std::vector<goparse::Token> toks = goparse::Scan(std::string_view(doc.text).substr(0, static_cast<size_t>(at - 1)));
    // innermost unclosed '('
    int depth = 0, commas = 0;
    int open = -1;
    for (int i = static_cast<int>(toks.size()) - 1; i >= 0; i--) {
        Tok t = toks[static_cast<size_t>(i)].tok;
        if (t == Tok::RPAREN || t == Tok::RBRACK || t == Tok::RBRACE) {
            depth++;
        } else if (t == Tok::LPAREN || t == Tok::LBRACK || t == Tok::LBRACE) {
            if (depth == 0) {
                if (t == Tok::LPAREN) open = i;
                break;
            }
            depth--;
        } else if (t == Tok::COMMA && depth == 0) {
            commas++;
        } else if (t == Tok::SEMICOLON && depth == 0 && toks[static_cast<size_t>(i)].lit == ";") {
            break;
        }
    }
    if (open <= 0) return sh;
    // the callee: IDENT (. IDENT)*
    std::vector<std::string> chain;
    int i = open - 1;
    while (i >= 0 && toks[static_cast<size_t>(i)].tok == Tok::IDENT) {
        chain.insert(chain.begin(), toks[static_cast<size_t>(i)].lit);
        if (i >= 1 && toks[static_cast<size_t>(i - 1)].tok == Tok::PERIOD) {
            i -= 2;
        } else {
            break;
        }
    }
    if (chain.empty()) return sh;
    std::string sig, docs, fname = chain.back();
    Pos callee_pos = toks[static_cast<size_t>(open - 1)].pos;
    int s = doc.Lookup(chain[0], callee_pos);
    if (chain.size() == 1) {
        if (s >= 0) {
            const Sym &sym = doc.syms[static_cast<size_t>(s)];
            if (sym.kind == SymKind::Func) {
                sig = golsp::DeclText(doc, sym);
                docs = CommentGroupText(sym.doc);
            } else if (sym.kind == SymKind::Var || sym.kind == SymKind::Param) {
                TypeRef t = sym.type ? TypeRef{sym.type, ""}
                                     : (sym.value && sym.value_index < 0 ? golsp::TypeOf(doc, sym.value, callee_pos) : TypeRef{});
                t = Underlying(doc, t);
                if (auto *ft = As<FuncType>(t.expr)) sig = "func " + fname + doc.SliceNode(ft).substr(4);
            }
        } else if (const GoVocabEntry *e = GoVocabFind("builtin", fname)) {
            sig = e->signature;
            docs = e->doc;
        }
    } else if (s >= 0 && doc.syms[static_cast<size_t>(s)].kind == SymKind::Import && chain.size() == 2) {
        if (const GoVocabEntry *e = GoVocabFind(doc.syms[static_cast<size_t>(s)].import_path, fname)) {
            sig = e->signature;
            docs = e->doc;
            if (e->kind == GoVocabKind::Type) sig = "func " + fname + "(x)";  // a conversion
        }
    } else if (s >= 0) {
        // x.y.M: walk the members
        const Sym &sym = doc.syms[static_cast<size_t>(s)];
        TypeRef t = sym.type ? TypeRef{sym.type, ""}
                             : (sym.value && sym.value_index < 0 ? golsp::TypeOf(doc, sym.value, callee_pos) : TypeRef{});
        if (!sym.type && !sym.value) {
            const Ref *r = doc.RefAt(toks[static_cast<size_t>(i)].pos);
            if (r && r->ident) t = golsp::TypeOf(doc, r->ident, r->pos);
        }
        if (sym.range_key || sym.range_value || sym.value_index >= 0) {
            const Ref *r = doc.RefAt(toks[static_cast<size_t>(i)].pos);
            if (r && r->ident) t = golsp::TypeOf(doc, r->ident, r->pos);
        }
        for (size_t k = 1; k < chain.size() && t.ok(); k++) {
            bool last = k + 1 == chain.size();
            TypeRef next;
            for (const Member &m : golsp::MembersOf(doc, t)) {
                if (m.name != chain[k]) continue;
                if (last) {
                    sig = m.detail;
                    docs = m.doc;
                } else {
                    next = m.type;
                }
                break;
            }
            t = next;
        }
    }
    if (sig.empty()) return sh;
    // label: `Name(params) results`, dropping `func` and any receiver
    std::string label = sig;
    size_t name_at = label.find(fname + "(");
    if (name_at == std::string::npos) name_at = label.find(fname + "[");
    if (name_at != std::string::npos) label = label.substr(name_at);
    size_t lp = label.find('(');
    if (label.find('[') != std::string::npos && label.find('[') < lp) {
        // type parameters first: skip the matching ']'
        int d = 0;
        for (size_t k = label.find('['); k < label.size(); k++) {
            if (label[k] == '[') d++;
            if (label[k] == ']' && --d == 0) {
                lp = label.find('(', k);
                break;
            }
        }
    }
    if (lp == std::string::npos) return sh;
    sh.found = true;
    sh.label = label;
    sh.documentation = docs;
    int d = 0;
    size_t pstart = lp + 1;
    for (size_t k = lp; k < label.size(); k++) {
        char c = label[k];
        if (c == '(' || c == '[' || c == '{') {
            d++;
        } else if (c == ')' || c == ']' || c == '}') {
            d--;
            if (d == 0) {
                std::string p = label.substr(pstart, k - pstart);
                if (p.find_first_not_of(" ") != std::string::npos) sh.params.push_back(p);
                break;
            }
        } else if (c == ',' && d == 1) {
            sh.params.push_back(label.substr(pstart, k - pstart));
            pstart = k + 2 <= label.size() ? k + 2 : k + 1;
        }
    }
    for (std::string &p : sh.params) {
        while (!p.empty() && p[0] == ' ') p.erase(0, 1);
    }
    int np = static_cast<int>(sh.params.size());
    sh.active_param = np == 0 ? 0 : std::min(commas, np - 1);
    return sh;
}

// --- navigation ------------------------------------------------------------------------

std::vector<GoLspLocation> GoLspDefinition(const GoLspDocument &doc, int line, int col) {
    std::vector<GoLspLocation> out;
    const Ref *r = doc.RefAt(doc.PosAt(line, col));
    if (!r) return out;
    Pos p = DeclPosOf(doc, *r);
    if (!IsValid(p)) return out;
    GoLspLocation loc;
    int len = r->sym >= 0 ? static_cast<int>(doc.syms[static_cast<size_t>(r->sym)].name.size()) : r->len;
    if (r->sym >= 0 && doc.syms[static_cast<size_t>(r->sym)].kind == SymKind::Import) {
        auto *im = As<ImportSpec>(doc.syms[static_cast<size_t>(r->sym)].decl);
        loc.range = im ? doc.RangeOfNode(im) : doc.RangeOf(p, p + len);
    } else {
        loc.range = doc.RangeOf(p, p + len);
    }
    out.push_back(loc);
    return out;
}

std::vector<GoLspReference> GoLspReferences(const GoLspDocument &doc, int line, int col) {
    std::vector<GoLspReference> out;
    const Ref *r = doc.RefAt(doc.PosAt(line, col));
    if (!r) return out;
    if (r->sym >= 0) {
        for (const Ref &q : doc.refs) {
            if (q.sym != r->sym) continue;
            GoLspReference ref;
            ref.range = doc.RangeOf(q.pos, q.pos + q.len);
            ref.is_definition = q.decl;
            out.push_back(ref);
        }
        // a method or field declared here is also reached through selectors
        const Sym &s = doc.syms[static_cast<size_t>(r->sym)];
        if (s.kind != SymKind::Method) return out;
    }
    Pos target = DeclPosOf(doc, *r);
    if (!IsValid(target)) return out;
    if (r->sym < 0) {
        GoLspReference def;
        def.range = doc.RangeOf(target, target + r->len);
        def.is_definition = true;
        out.push_back(def);
    }
    for (const Ref &q : doc.refs) {
        if (!q.selector_base || q.ident->Name != r->ident->Name) continue;
        if (DeclPosOf(doc, q) != target) continue;
        GoLspReference ref;
        ref.range = doc.RangeOf(q.pos, q.pos + q.len);
        out.push_back(ref);
    }
    return out;
}

namespace {

bool Renamable(const Sym &s) {
    if (!s.local) return false;
    switch (s.kind) {
    case SymKind::Var: case SymKind::Const: case SymKind::Type: case SymKind::Param: case SymKind::Result:
    case SymKind::Label: case SymKind::TypeParam: case SymKind::TypeSwitch:
        return true;
    default: return false;
    }
}

}  // namespace

GoLspRenameTarget GoLspPrepareRename(const GoLspDocument &doc, int line, int col) {
    GoLspRenameTarget t;
    const Ref *r = doc.RefAt(doc.PosAt(line, col));
    if (!r || r->sym < 0 || !Renamable(doc.syms[static_cast<size_t>(r->sym)])) return t;
    t.found = true;
    t.range = doc.RangeOf(r->pos, r->pos + r->len);
    t.placeholder = r->ident->Name;
    return t;
}

std::vector<GoLspTextEdit> GoLspRename(const GoLspDocument &doc, int line, int col, const std::string &new_name,
                                       std::string *error) {
    std::vector<GoLspTextEdit> edits;
    const Ref *r = doc.RefAt(doc.PosAt(line, col));
    if (!r || r->sym < 0) {
        *error = "no symbol to rename here";
        return edits;
    }
    const Sym &s = doc.syms[static_cast<size_t>(r->sym)];
    if (!Renamable(s)) {
        *error = s.local ? "only local names can be renamed" :
                           s.name + " is visible in the whole package; renaming it in one file would miss the others";
        return edits;
    }
    if (!IsIdentifier(new_name)) {
        *error = "\"" + new_name + "\" is not a valid Go identifier";
        return edits;
    }
    if (s.scope >= 0) {
        const auto &names = doc.scopes[static_cast<size_t>(s.scope)].names;
        if (names.count(new_name)) {
            *error = new_name + " is already declared in this block";
            return edits;
        }
    }
    for (const Ref &q : doc.refs) {
        if (q.sym != r->sym) continue;
        // a use that would now find a different declaration first
        if (!q.decl && q.scope >= 0) {
            for (int sc = q.scope; sc >= 0 && sc != s.scope; sc = doc.scopes[static_cast<size_t>(sc)].parent) {
                if (doc.scopes[static_cast<size_t>(sc)].names.count(new_name)) {
                    *error = "renaming would make a use at line " + std::to_string(doc.parse.Line(q.pos)) + " refer to another " +
                             new_name;
                    return {};
                }
            }
        }
        GoLspTextEdit e;
        e.range = doc.RangeOf(q.pos, q.pos + q.len);
        e.new_text = new_name;
        edits.push_back(std::move(e));
    }
    return edits;
}

// --- outline / folding ------------------------------------------------------------------------

std::vector<GoLspSymbol> GoLspSymbols(const GoLspDocument &doc) {
    std::vector<GoLspSymbol> out;
    auto push = [&](const std::string &name, const std::string &detail, GoLspSymbolKind kind, const Node *whole,
                    const Ident *id, int parent) {
        GoLspSymbol s;
        s.name = name;
        s.detail = detail;
        s.kind = kind;
        s.range = doc.RangeOfNode(whole);
        s.sel_range = id ? doc.RangeOfNode(id) : s.range;
        s.parent = parent;
        out.push_back(std::move(s));
        return static_cast<int>(out.size()) - 1;
    };
    auto members = [&](Node *type, int parent) {
        if (auto *st = As<StructType>(type)) {
            for (Field *f : st->Fields->List) {
                if (f->Names.empty()) {
                    push(doc.SliceNode(f->Type), "", GoLspSymbolKind::Field, f, nullptr, parent);
                }
                for (Ident *id : f->Names) push(id->Name, doc.SliceNode(f->Type), GoLspSymbolKind::Field, f, id, parent);
            }
        } else if (auto *it = As<InterfaceType>(type)) {
            for (Field *f : it->Methods->List) {
                if (f->Names.empty()) {
                    push(doc.SliceNode(f->Type), "", GoLspSymbolKind::Interface, f, nullptr, parent);
                }
                for (Ident *id : f->Names) push(id->Name, doc.SliceNode(f->Type), GoLspSymbolKind::Method, f, id, parent);
            }
        }
    };
    for (Node *d : doc.parse.file->Decls) {
        if (auto *fd = As<FuncDecl>(d)) {
            std::string name = fd->Name->Name;
            GoLspSymbolKind kind = GoLspSymbolKind::Function;
            if (fd->Recv && !fd->Recv->List.empty()) {
                Node *t = fd->Recv->List[0]->Type;
                bool ptr = false;
                if (auto *st = As<StarExpr>(t)) {
                    ptr = true;
                    t = st->X;
                }
                if (auto *ix = As<IndexExpr>(t)) t = ix->X;
                if (auto *il = As<IndexListExpr>(t)) t = il->X;
                std::string recv = t ? doc.SliceNode(t) : "?";
                name = ptr ? "(*" + recv + ")." + name : "(" + recv + ")." + name;
                kind = GoLspSymbolKind::Method;
            }
            std::string sig = doc.SliceNode(fd->Type);
            size_t lp = sig.find('(', 4);
            if (fd->Type->TypeParams) lp = sig.find('(', static_cast<size_t>(NodeEnd(fd->Type->TypeParams) - NodePos(fd->Type)));
            std::string detail = lp != std::string::npos ? sig.substr(lp) : sig;
            if (fd->Recv) {
                // the receiver list is before the name in the source; the type's own params follow it
                detail = doc.Slice(fd->Type->Params ? NodePos(fd->Type->Params) : NodePos(fd->Name), NodeEnd(fd->Type));
            }
            push(name, detail, kind, fd, fd->Name, -1);
        } else if (auto *g = As<GenDecl>(d)) {
            for (Node *spec : g->Specs) {
                if (auto *ts = As<TypeSpec>(spec)) {
                    GoLspSymbolKind kind = As<StructType>(ts->Type)      ? GoLspSymbolKind::Struct
                                           : As<InterfaceType>(ts->Type) ? GoLspSymbolKind::Interface
                                                                         : GoLspSymbolKind::Class;
                    std::string detail = As<StructType>(ts->Type) ? "struct" : As<InterfaceType>(ts->Type) ? "interface"
                                                                                                             : doc.SliceNode(ts->Type);
                    if (detail.size() > 60) detail = detail.substr(0, 57) + "...";
                    int p = push(ts->Name->Name, detail, kind, ts, ts->Name, -1);
                    members(ts->Type, p);
                } else if (auto *vs = As<ValueSpec>(spec)) {
                    for (Ident *id : vs->Names) {
                        if (id->Name == "_") continue;
                        push(id->Name, vs->Type ? doc.SliceNode(vs->Type) : "",
                             g->Tok_ == Tok::CONST ? GoLspSymbolKind::Constant : GoLspSymbolKind::Variable,
                             g->Specs.size() == 1 ? static_cast<Node *>(g) : vs, id, -1);
                    }
                }
            }
        }
    }
    return out;
}

std::vector<GoLspFold> GoLspFolds(const GoLspDocument &doc) {
    std::vector<GoLspFold> out;
    std::set<int> starts;
    auto add = [&](Pos a, Pos b, const std::string &kind) {
        if (!IsValid(a) || !IsValid(b)) return;
        int la = doc.parse.Line(a), lb = doc.parse.Line(b);
        if (lb - 1 <= la) return;
        if (!starts.insert(la).second) return;
        GoLspFold f;
        f.start_line = la - 1;
        f.end_line = lb - 2;  // keep the closing line visible
        f.kind = kind;
        out.push_back(f);
    };
    std::function<void(Node *)> walk = [&](Node *n) {
        if (!n) return;
        switch (n->kind) {
        case K::BlockStmt: add(static_cast<BlockStmt *>(n)->Lbrace, static_cast<BlockStmt *>(n)->Rbrace, ""); break;
        case K::CompositeLit: add(static_cast<CompositeLit *>(n)->Lbrace, static_cast<CompositeLit *>(n)->Rbrace, ""); break;
        case K::FieldList: add(static_cast<FieldList *>(n)->Opening, static_cast<FieldList *>(n)->Closing, ""); break;
        case K::CallExpr: add(static_cast<CallExpr *>(n)->Lparen, static_cast<CallExpr *>(n)->Rparen, ""); break;
        case K::GenDecl: {
            auto *g = static_cast<GenDecl *>(n);
            add(g->Lparen, g->Rparen, g->Tok_ == Tok::IMPORT ? "imports" : "");
            break;
        }
        case K::CaseClause: {
            auto *c = static_cast<CaseClause *>(n);
            if (!c->Body.empty()) add(c->Colon, NodeEnd(c->Body.back()) + 1, "");
            break;
        }
        case K::CommClause: {
            auto *c = static_cast<CommClause *>(n);
            if (!c->Body.empty()) add(c->Colon, NodeEnd(c->Body.back()) + 1, "");
            break;
        }
        default: break;
        }
        ForEachChild(n, walk);
    };
    for (Node *d : doc.parse.file->Decls) walk(d);
    for (CommentGroup *g : doc.parse.file->Comments) {
        int la = doc.parse.Line(NodePos(g)), lb = doc.parse.Line(NodeEnd(g) - 1);
        if (lb > la && starts.insert(la).second) {
            GoLspFold f;
            f.start_line = la - 1;
            f.end_line = lb - 1;
            f.kind = "comment";
            out.push_back(f);
        }
    }
    std::sort(out.begin(), out.end(), [](const GoLspFold &a, const GoLspFold &b) { return a.start_line < b.start_line; });
    return out;
}
