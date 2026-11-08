// mep's JavaScript language server, analysis half: the diagnostics. See
// js_lsp.h; the scope model is js_lsp_internal.h, hover/completion/
// navigation are js_lsp_features.cpp.
//
// The codes are ESLint's rule names, and the semantics follow ESLint's rule
// documentation (MIT), narrowed where real code showed the rule to be an
// opinion rather than a bug finder -- see help/js-lsp.org's calibration
// section for each narrowing and the corpus numbers behind it.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "js_lsp_internal.h"

using jsparse::Node;
using jsparse::NodeType;
using namespace jslsp;

namespace {

bool IsFunction(const Node *n) {
    return n && (n->type == NodeType::FunctionDeclaration || n->type == NodeType::FunctionExpression ||
                 n->type == NodeType::ArrowFunctionExpression);
}

bool Contains(const Node *outer, const Node *inner) {
    return outer && inner && outer->start <= inner->start && inner->end <= outer->end;
}

// JavaScript's ToString for a number used as a property key (the common cases).
std::string NumberKey(const Node *lit) {
    const double v = lit->num;
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
        return buf;
    }
    return std::string(lit->str);
}

// The name a non-computed property key stands for, or "" for a computed one.
bool KeyName(const Node *prop, std::string *out) {
    if (prop->Has(jsparse::kFlagComputed)) return false;
    const Node *k = prop->key();
    if (!k) return false;
    if (k->type == NodeType::Identifier) {
        *out = std::string(k->str);
        return true;
    }
    if (k->type == NodeType::PrivateIdentifier) {
        *out = "#" + std::string(k->str);
        return true;
    }
    if (k->type == NodeType::Literal) {
        if (k->lit == jsparse::LitKind::String) {
            *out = std::string(k->str2);
            return true;
        }
        if (k->lit == jsparse::LitKind::Number) {
            *out = NumberKey(k);
            return true;
        }
        if (k->lit == jsparse::LitKind::BigInt) {
            *out = std::string(k->str2);
            return true;
        }
        if (k->lit == jsparse::LitKind::Null) {
            *out = "null";
            return true;
        }
        if (k->lit == jsparse::LitKind::Bool) {
            *out = k->Has(jsparse::kFlagBoolValue) ? "true" : "false";
            return true;
        }
    }
    return false;
}

bool IsGlobalNaN(const JsLspDocument &doc, const Node *n) {
    if (!n) return false;
    if (n->type == NodeType::Identifier && n->str == "NaN") {
        auto it = doc.id_ref.find(n);
        return it != doc.id_ref.end() && !it->second->var;
    }
    if (n->type == NodeType::MemberExpression && !n->Has(jsparse::kFlagComputed) && n->object() &&
        n->object()->type == NodeType::Identifier && n->object()->str == "Number" && n->property() &&
        n->property()->str == "NaN")
        return true;
    return false;
}

bool Terminates(const Node *s) {
    if (!s) return false;
    switch (s->type) {
        case NodeType::ReturnStatement:
        case NodeType::ThrowStatement:
        case NodeType::BreakStatement:
        case NodeType::ContinueStatement:
            return true;
        case NodeType::BlockStatement:
            for (const Node *k : s->body_list()) {
                if (Terminates(k)) return true;
            }
            return false;
        case NodeType::IfStatement:
            return s->alternate() && Terminates(s->consequent()) && Terminates(s->alternate());
        case NodeType::TryStatement: {
            if (s->finalizer() && Terminates(s->finalizer())) return true;
            const Node *h = s->handler();
            return Terminates(s->block()) && (!h || Terminates(h->body()));
        }
        default:
            return false;
    }
}

// Statements that are fine after a return: hoisted declarations with no effect where they stand.
bool Inert(const Node *s) {
    if (!s) return true;
    if (s->type == NodeType::FunctionDeclaration || s->type == NodeType::EmptyStatement) return true;
    // TypeScript declarations that emit no code.
    if (s->type == NodeType::TSInterfaceDeclaration || s->type == NodeType::TSTypeAliasDeclaration ||
        s->type == NodeType::TSDeclareFunction || s->HasTs(jsparse::kTsDeclare) ||
        (s->type == NodeType::TSEnumDeclaration && s->HasTs(jsparse::kTsConst)))
        return true;
    if (s->type == NodeType::TSModuleDeclaration) return true;  // hoisted like a function, when it is code at all
    if (s->type == NodeType::VariableDeclaration && s->kind == "var") {
        for (const Node *d : s->declarations()) {
            if (d && d->init()) return false;
        }
        return true;
    }
    return false;
}

bool HasCommentIn(const JsLspDocument &doc, uint32_t a, uint32_t b) {
    for (const jsparse::Comment &c : doc.parse.comments) {
        if (c.start >= a && c.end <= b) return true;
        if (c.start >= b) break;
    }
    return false;
}

// A member chain made of names only: a, a.b, this.a.b.
bool PlainChain(const Node *n) {
    while (n && n->type == NodeType::MemberExpression) {
        if (n->Has(jsparse::kFlagComputed) || n->Has(jsparse::kFlagOptional)) return false;
        n = n->object();
    }
    return n && (n->type == NodeType::Identifier || n->type == NodeType::ThisExpression);
}

const std::set<std::string> &TypeofValues() {
    static const std::set<std::string> s = {"undefined", "object", "boolean", "number",
                                            "string",    "function", "symbol", "bigint"};
    return s;
}

// Is this function's parameter list its own, or dictated by whoever calls it (a callback, a
// method, a handler stored somewhere)? Own: a declaration or a variable's initializer that the
// file only ever calls.
bool OwnSignature(const JsLspDocument &doc, const Node *fn) {
    const Var *named = nullptr;
    if (fn->type == NodeType::FunctionDeclaration) {
        if (!fn->id()) return false;
        auto it = doc.decl_var.find(fn->id());
        named = it == doc.decl_var.end() ? nullptr : it->second;
    } else {
        const Node *p = doc.Parent(fn);
        if (!p || p->type != NodeType::VariableDeclarator || p->init() != fn || !p->id() ||
            p->id()->type != NodeType::Identifier)
            return false;
        auto it = doc.decl_var.find(p->id());
        named = it == doc.decl_var.end() ? nullptr : it->second;
    }
    if (!named || named->exported) return false;
    for (const Ref *r : named->refs) {
        const Node *p = doc.Parent(r->id);
        if (!p) return false;
        if ((p->type == NodeType::CallExpression || p->type == NodeType::NewExpression) && p->callee() == r->id)
            continue;
        return false;  // passed, stored or exported: an interface
    }
    return true;
}

class Linter {
public:
    explicit Linter(const JsLspDocument &doc) : doc_(doc) {}

    std::vector<JsLspDiagnostic> Run() {
        const jsparse::ParseResult &pr = doc_.parse;
        if (!doc_.lint) return out_;
        if (!pr.errors.empty()) {
            const jsparse::Error &e = pr.errors[0];
            uint32_t a = e.offset, b = e.offset;
            while (b < doc_.text.size() && IsWordByte(doc_.text[b])) b++;
            if (b == a && a < doc_.text.size() && doc_.text[a] != '\n') b = a + 1;
            Add(a, b, JsLspSeverity::Error, "syntax-error", e.message);
        }
        for (const auto &sc : doc_.scopes) {
            if (sc->kind == ScopeKind::Function && sc->node) fn_scope_[sc->node] = sc.get();
        }
        broken_ = pr.has_error;
        cut_ = broken_ ? pr.first_error_offset : UINT32_MAX;
        if (!pr.program) return out_;
        Variables();
        Walk();
        std::stable_sort(out_.begin(), out_.end(), [](const JsLspDiagnostic &a, const JsLspDiagnostic &b) {
            return a.range.line != b.range.line ? a.range.line < b.range.line : a.range.col < b.range.col;
        });
        return out_;
    }

private:
    const JsLspDocument &doc_;
    std::vector<JsLspDiagnostic> out_;
    bool broken_ = false;
    uint32_t cut_ = UINT32_MAX;
    std::unordered_map<const Node *, const Scope *> fn_scope_;

    static bool IsWordByte(char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return std::isalnum(u) || c == '_' || c == '$' || u >= 0x80;
    }

    JsLspDiagnostic *Add(uint32_t a, uint32_t b, JsLspSeverity sev, const std::string &code,
                         const std::string &msg) {
        JsLspDiagnostic d;
        d.range = doc_.Range(a, std::max(a, b));
        d.severity = sev;
        d.code = code;
        d.message = msg;
        out_.push_back(d);
        return &out_.back();
    }

    bool Suppressed(uint32_t off, const std::string &code) const {
        auto has = [&](const std::vector<std::string> &rules) {
            return rules.empty() || std::find(rules.begin(), rules.end(), code) != rules.end();
        };
        auto it = doc_.directives.lines.find(doc_.LineOf(off));
        if (it != doc_.directives.lines.end()) {
            for (const auto &rules : it->second) {
                if (has(rules)) return true;
            }
        }
        for (const Directives::Region &r : doc_.directives.regions) {
            if (r.from <= off && off < r.to && has(r.rules)) return true;
        }
        return false;
    }

    JsLspDiagnostic *Lint(uint32_t a, uint32_t b, JsLspSeverity sev, const std::string &code,
                          const std::string &msg) {
        // On a repaired tree only what lies wholly before the first error can be trusted.
        if (a > cut_ || (cut_ != UINT32_MAX && b > cut_)) return nullptr;
        if (Suppressed(a, code)) return nullptr;
        return Add(a, b, sev, code, msg);
    }

    JsLspDiagnostic *LintNode(const Node *n, JsLspSeverity sev, const std::string &code, const std::string &msg) {
        return Lint(n->start, n->end, sev, code, msg);
    }

    // ---- variables ----

    bool ScopeDynamic(const Scope *s) const {
        for (; s; s = s->parent) {
            if (s->dynamic) return true;
        }
        return false;
    }

    // A read that counts as a use: not `x++;`, and not from inside the variable's own
    // definition (a function calling itself, a class naming itself).
    bool CountsAsUse(const Var *v, const Ref *r) const {
        if (!r->read || r->for_itself) return false;
        const Node *self = nullptr;
        if (v->kind == VarKind::Function || v->kind == VarKind::Class || v->kind == VarKind::Interface ||
            v->kind == VarKind::TypeAlias || v->kind == VarKind::Enum || v->kind == VarKind::Namespace)
            self = v->decl;
        // A merged declaration (overloads, `interface A` + `namespace A`): a use inside any of them
        // is the name's own.
        if (v->ids.size() > 1 && doc_.typescript) {
            for (const Node *id : v->ids) {
                const Node *d = DeclOf(id);
                if (d && d != v->decl && Contains(d, r->id) && !IsFunction(d)) return false;
            }
        }
        if (v->init && (IsFunction(v->init) || v->init->type == NodeType::ClassExpression)) self = v->init;
        if (self && Contains(self, r->id)) return false;
        return true;
    }

    // The declaration a declaring identifier names (for the self-use test of merged declarations).
    const Node *DeclOf(const Node *id) const {
        const Node *p = doc_.Parent(id);
        if (!p) return nullptr;
        switch (p->type) {
            case NodeType::TSInterfaceDeclaration:
            case NodeType::TSTypeAliasDeclaration:
            case NodeType::TSEnumDeclaration:
            case NodeType::TSModuleDeclaration:
            case NodeType::ClassDeclaration:
            case NodeType::TSDeclareFunction:
                return p->id() == id ? p : nullptr;
            default:
                return nullptr;
        }
    }

    // Used, and how: 0 unused, 1 used, 2 only in types (`typeof x`) -- a value only used as a type.
    int Usage(const Var *v) const {
        bool in_type = false;
        for (const Ref *r : v->refs) {
            if (!CountsAsUse(v, r)) continue;
            if (!r->in_type || (v->ns & kNsType)) return 1;
            in_type = true;
        }
        if (in_type) return Used(v, true) ? 1 : 2;
        return Used(v, true) ? 1 : 0;
    }

    bool Used(const Var *v, bool loop_only = false) const {
        if (!loop_only) {
            for (const Ref *r : v->refs) {
                if (CountsAsUse(v, r)) return true;
            }
        }
        // `for (const k in o) return true;` tests for a key: ESLint counts that as a use.
        const Node *d = v->decl;
        const Node *decl = d && d->type == NodeType::VariableDeclarator ? doc_.Parent(d) : nullptr;
        const Node *loop = decl ? doc_.Parent(decl) : nullptr;
        if (loop && (loop->type == NodeType::ForInStatement || loop->type == NodeType::ForOfStatement) &&
            loop->left() == decl) {
            const Node *b = loop->body();
            if (b && b->type == NodeType::BlockStatement) b = b->body_list().empty() ? nullptr : b->body_list()[0];
            if (b && b->type == NodeType::ReturnStatement) return true;
        }
        return false;
    }

    std::set<std::string> PragmaNames() const {
        // `/** @jsx h */` names the JSX factory; classic-runtime JSX uses React.
        std::set<std::string> out;
        if (doc_.has_jsx) out.insert("React");
        for (const jsparse::Comment &c : doc_.parse.comments) {
            for (const char *tag : {"@jsx ", "@jsxFrag "}) {
                const size_t at = c.value.find(tag);
                if (at == std::string_view::npos) continue;
                size_t i = at + std::strlen(tag);
                while (i < c.value.size() && c.value[i] == ' ') i++;
                size_t j = i;
                while (j < c.value.size() && IsWordByte(c.value[j])) j++;
                if (j > i) out.insert(std::string(c.value.substr(i, j - i)));
            }
        }
        return out;
    }

    void Variables() {
        const std::set<std::string> pragmas = PragmaNames();
        const bool script = doc_.Global() && doc_.Global()->kind == ScopeKind::Global;
        for (const auto &up : doc_.vars) {
            const Var *v = up.get();
            if (v->ids.empty()) continue;
            const Node *id = v->ids[0];
            const std::string name(v->name);
            // -- assignments to things that cannot be assigned --
            for (const Ref *r : v->refs) {
                if (!r->write) continue;
                switch (v->kind) {
                    case VarKind::Const:
                    case VarKind::Using:
                        LintNode(r->id, JsLspSeverity::Error, "no-const-assign", "'" + name + "' is constant.");
                        break;
                    case VarKind::Import:
                        LintNode(r->id, JsLspSeverity::Error, "no-import-assign", "'" + name + "' is read-only.");
                        break;
                    case VarKind::Function:
                        LintNode(r->id, JsLspSeverity::Warning, "no-func-assign", "'" + name + "' is a function.");
                        break;
                    case VarKind::Class:
                        LintNode(r->id, JsLspSeverity::Warning, "no-class-assign", "'" + name + "' is a class.");
                        break;
                    default:
                        break;
                }
            }
            // -- redeclaration --
            if (v->redeclared &&
                (v->kind == VarKind::Var || v->kind == VarKind::Function || v->kind == VarKind::Param)) {
                for (size_t i = 1; i < v->ids.size(); i++)
                    LintNode(v->ids[i], JsLspSeverity::Warning, "no-redeclare", "'" + name + "' is already defined.");
            }
            // -- shadowing, inside one function only --
            Shadow(v);
            // -- unused --
            if (broken_) continue;
            if (v->kind == VarKind::Arguments || v->kind == VarKind::FunctionName || v->kind == VarKind::ClassName)
                continue;
            if (v->exported || name.empty() || name[0] == '_' || doc_.directives.exported.count(name) ||
                pragmas.count(name))
                continue;
            // TypeScript: declarations of things that exist elsewhere, a parameter that is also a
            // member, an enum member (a property of the enum), a mapped type's key.
            if (v->ambient || v->param_property || v->no_unused || v->kind == VarKind::EnumMember) continue;
            if (v->rest_sibling) continue;
            if (ScopeDynamic(v->scope)) continue;
            // A browser script's top-level names are globals other scripts (and the page) use.
            if (script && v->scope == doc_.Global() && !doc_.commonjs) continue;
            const int usage = Usage(v);
            if (usage == 1) continue;
            bool assigned = v->has_value;
            for (const Ref *r : v->refs) assigned = assigned || r->write;
            const bool valued = v->kind == VarKind::Var || v->kind == VarKind::Let || v->kind == VarKind::Const ||
                                v->kind == VarKind::Using || v->kind == VarKind::CatchParam;
            const std::string msg = "'" + name + "' is " + (assigned && valued ? "assigned a value" : "defined") +
                                    (usage == 2 ? " but only used as a type." : " but never used.");
            if (v->kind == VarKind::TypeParam) {
                if (JsLspDiagnostic *d = LintNode(id, JsLspSeverity::Hint, "no-unused-vars", msg))
                    d->unnecessary = true;
                continue;
            }
            if (v->kind == VarKind::Param) {
                if (!UnusedParamReportable(v)) continue;
                if (JsLspDiagnostic *d = LintNode(id, JsLspSeverity::Hint, "no-unused-vars", msg))
                    d->unnecessary = true;
                continue;
            }
            if (v->kind == VarKind::CatchParam) {
                if (JsLspDiagnostic *d = LintNode(id, JsLspSeverity::Hint, "no-unused-vars", msg))
                    d->unnecessary = true;
                continue;
            }
            // Where ESLint reports it: the last assignment in the variable's own function (so an
            // eslint-disable-next-line above that assignment applies), else the declaration.
            const Node *at = id;
            const Scope *home = v->scope;
            while (home && !home->IsFunctionLike()) home = home->parent;
            for (const Node *other : v->ids) {
                const Node *p = doc_.Parent(other);
                if (p && p->type == NodeType::VariableDeclarator && p->id() == other && p->init() &&
                    other->start > at->start)
                    at = other;
            }
            for (const Ref *r : v->refs) {
                if (!r->write) continue;
                const Scope *f = r->from;
                while (f && !f->IsFunctionLike()) f = f->parent;
                if (f == home && r->id->start > at->start) at = r->id;
            }
            if (JsLspDiagnostic *d = LintNode(at, JsLspSeverity::Warning, "no-unused-vars", msg))
                d->unnecessary = true;
        }
        Undefined();
    }

    bool UnusedParamReportable(const Var *v) const {
        const Node *fn = v->decl;
        if (!fn) return false;
        // A setter's parameter is required by the syntax.
        const Node *p = doc_.Parent(fn);
        if (p && (p->type == NodeType::MethodDefinition || p->type == NodeType::Property) && p->kind == "set")
            return false;
        if (!OwnSignature(doc_, fn)) return false;
        // An overloaded function's implementation serves every overload's parameter list.
        if (fn->id()) {
            auto it = doc_.decl_var.find(fn->id());
            if (it != doc_.decl_var.end() && it->second->overloaded) return false;
        }
        // after-used (ESLint's default): a parameter is kept for its position when a later one is
        // used. A name destructured out of a parameter has no position to keep.
        if (v->in_pattern) return true;
        for (const Var *o : v->scope->vars) {
            if (o->kind != VarKind::Param || o == v || o->ids.empty()) continue;
            if (o->ids[0]->start > v->ids[0]->start && !o->refs.empty()) return false;
        }
        return true;
    }

    void Shadow(const Var *v) {
        if (v->kind == VarKind::Arguments || v->kind == VarKind::FunctionName || v->kind == VarKind::ClassName ||
            v->kind == VarKind::Param || v->kind == VarKind::EnumMember || v->ids.empty() || !(v->ns & kNsValue))
            return;
        const Scope *s = v->scope;
        if (s->IsFunctionLike()) return;
        for (const Scope *up = s->parent; up; up = up->parent) {
            if (const Var *o = up->Find(v->name)) {
                if (o->kind == VarKind::Arguments || o->kind == VarKind::FunctionName ||
                    o->kind == VarKind::ClassName || o->ids.empty() || !(o->ns & kNsValue))
                    break;
                if (o->ids[0]->start >= v->ids[0]->start) break;  // declared later: not shadowed yet
                // `const x = x` style re-reads of the outer one are deliberate.
                LintNode(v->ids[0], JsLspSeverity::Information, "no-shadow",
                         "'" + std::string(v->name) + "' is already declared in the upper scope on line " +
                             std::to_string(doc_.LineOf(o->ids[0]->start) + 1) + ".");
                break;
            }
            if (up->IsFunctionLike()) break;
        }
    }

    void Undefined() {
        // TypeScript: off, as typescript-eslint recommends -- tsc checks every name, with the
        // project's lib and @types, which a single-file analysis cannot see.
        if (broken_ || doc_.uses_with_or_eval || doc_.typescript) return;
        std::map<std::string, std::string> suggestion;
        // A name the file uses again and again is a global it expects (node's internalBinding,
        // a page's library); a typo happens once or twice.
        std::map<std::string_view, int> uses;
        for (const Ref *r : doc_.through) uses[r->id->str]++;
        for (const Ref *r : doc_.through) {
            // Strict code (a module, a class, "use strict") throws on assignment to an undeclared
            // name: that one is reported whether or not it looks like a typo.
            if (r->write && !r->read && r->from->strict && !ScopeDynamic(r->from)) {
                const std::string name(r->id->str);
                if (!IsHostGlobal(name) && !FindApi(name) && !doc_.directives.globals.count(name))
                    LintNode(r->id, JsLspSeverity::Warning, "no-undef",
                             "'" + name + "' is not defined (assigning to it throws in strict mode).");
                continue;
            }
            if (uses[r->id->str] >= 3) continue;
            if (r->type_of || !r->read) continue;
            const std::string name(r->id->str);
            if (IsHostGlobal(name) || FindApi(name) || doc_.directives.globals.count(name) ||
                doc_.implicit_globals.count(r->id->str))
                continue;
            if (ScopeDynamic(r->from)) continue;
            auto it = suggestion.find(name);
            if (it == suggestion.end()) it = suggestion.emplace(name, Suggest(r)).first;
            if (it->second.empty()) continue;
            LintNode(r->id, JsLspSeverity::Warning, "no-undef",
                     "'" + name + "' is not defined. Did you mean '" + it->second + "'?");
        }
    }

    // A known name `r` is probably a misspelling of, or "".
    std::string Suggest(const Ref *r) const {
        const std::string_view name = r->id->str;
        if (name.size() < 4) return std::string();
        const int cap = name.size() <= 6 ? 1 : 2;
        std::string best;
        int best_d = cap + 1;
        auto consider = [&](std::string_view c) {
            if (c == name || c.size() < 3) return;
            const int d = EditDistance(name, c, cap);
            if (d < best_d) {
                best_d = d;
                best = std::string(c);
            }
        };
        for (const Scope *s = r->from; s; s = s->parent) {
            for (const Var *v : s->vars) {
                if (!v->ids.empty()) consider(v->name);
            }
        }
        for (std::string_view g : doc_.implicit_globals) consider(g);
        for (const Api *a : ApiGlobals()) {
            // The DOM's thousands of interface names are not values anyone misspells; its
            // functions and variables (document, window, fetch) are.
            if (std::strcmp(a->origin, "dom") == 0 &&
                (std::strcmp(a->kind, "interface") == 0 || std::strcmp(a->kind, "type") == 0))
                continue;
            if (std::strcmp(a->kind, "type") == 0 || std::strcmp(a->kind, "interface") == 0) continue;
            consider(a->name);
        }
        return best;
    }

    // ---- the tree ----

    void Walk() {
        std::vector<const Node *> stack{doc_.parse.program};
        std::vector<Node *> kids;
        while (!stack.empty()) {
            const Node *n = stack.back();
            stack.pop_back();
            Check(n);
            jsparse::CollectChildren(n, kids);
            for (size_t i = kids.size(); i-- > 0;) stack.push_back(kids[i]);
        }
    }

    void Check(const Node *n) {
        switch (n->type) {
            case NodeType::Program:
            case NodeType::StaticBlock:
                Unreachable(n->body_list());
                break;
            case NodeType::BlockStatement:
                Unreachable(n->body_list());
                EmptyBlock(n);
                break;
            case NodeType::SwitchCase:
                Unreachable(n->consequent_list());
                break;
            case NodeType::SwitchStatement:
                Switch(n);
                break;
            case NodeType::ObjectExpression:
                DupeKeys(n);
                break;
            case NodeType::ClassBody:
                DupeClassMembers(n);
                break;
            case NodeType::BinaryExpression:
                Binary(n);
                break;
            case NodeType::AssignmentExpression:
                if (n->op == "=" || n->op == "&&=" || n->op == "||=" || n->op == "?\?=") SelfAssign(n->left(), n->right());
                break;
            case NodeType::FunctionDeclaration:
            case NodeType::FunctionExpression:
            case NodeType::ArrowFunctionExpression:
                DupeArgs(n);
                break;
            default:
                break;
        }
    }

    void Unreachable(const jsparse::NodeList &list) {
        for (uint32_t i = 0; i < list.size(); i++) {
            if (!Terminates(list[i])) continue;
            uint32_t first = UINT32_MAX, last = 0;
            for (uint32_t j = i + 1; j < list.size(); j++) {
                if (Inert(list[j])) continue;
                if (first == UINT32_MAX) first = list[j]->start;
                last = list[j]->end;
            }
            if (first != UINT32_MAX) {
                if (JsLspDiagnostic *d = Lint(first, last, JsLspSeverity::Warning, "no-unreachable", "Unreachable code."))
                    d->unnecessary = true;
            }
            return;
        }
    }

    void EmptyBlock(const Node *n) {
        if (!n->body_list().empty()) return;
        const Node *p = doc_.Parent(n);
        if (IsFunction(p)) return;
        if (p && p->type == NodeType::CatchClause) {
            // ESLint reports an empty catch too; the try it belongs to is the anchor.
        }
        if (HasCommentIn(doc_, n->start, n->end)) return;
        LintNode(n, JsLspSeverity::Information, "no-empty", "Empty block statement.");
    }

    void Switch(const Node *n) {
        if (n->cases().empty() && !HasCommentIn(doc_, n->start, n->end))
            LintNode(n, JsLspSeverity::Information, "no-empty", "Empty switch statement.");
        if (IsGlobalNaN(doc_, n->discriminant()))
            LintNode(n->discriminant(), JsLspSeverity::Warning, "use-isnan",
                     "'switch(NaN)' can never match a case clause. Use Number.isNaN instead of the switch.");
        std::vector<const Node *> seen;
        for (const Node *c : n->cases()) {
            if (!c || !c->test()) continue;
            if (IsGlobalNaN(doc_, c->test()))
                LintNode(c->test(), JsLspSeverity::Warning, "use-isnan",
                         "'case NaN' can never match. Use Number.isNaN before the switch.");
            bool dup = false;
            for (const Node *s : seen) {
                if (SameNode(s, c->test(), true)) dup = true;
            }
            if (dup)
                Lint(c->start, c->test()->end, JsLspSeverity::Warning, "no-duplicate-case", "Duplicate case label.");
            else
                seen.push_back(c->test());
        }
    }

    void DupeKeys(const Node *n) {
        struct Seen {
            bool init = false, get = false, set = false;
        };
        std::map<std::string, Seen> seen;
        for (const Node *p : n->properties()) {
            if (!p || p->type != NodeType::Property) continue;
            std::string key;
            if (!KeyName(p, &key)) continue;
            if (key == "__proto__" && !p->Has(jsparse::kFlagShorthand) && !p->Has(jsparse::kFlagMethod)) continue;
            Seen &s = seen[key];
            bool dup = false;
            if (p->kind == "get") {
                dup = s.init || s.get;
                s.get = true;
            } else if (p->kind == "set") {
                dup = s.init || s.set;
                s.set = true;
            } else {
                dup = s.init || s.get || s.set;
                s.init = true;
            }
            if (dup) LintNode(p->key(), JsLspSeverity::Warning, "no-dupe-keys", "Duplicate key '" + key + "'.");
        }
    }

    void DupeClassMembers(const Node *n) {
        struct Seen {
            bool init = false, get = false, set = false;
        };
        std::map<std::string, Seen> seen;
        for (const Node *m : n->body_list()) {
            if (!m || (m->type != NodeType::MethodDefinition && m->type != NodeType::PropertyDefinition)) continue;
            if (m->type == NodeType::MethodDefinition && m->kind == "constructor") continue;
            // TypeScript: overload signatures and `declare` fields describe a member, they are not one.
            if (m->value() && m->value()->type == NodeType::TSEmptyBodyFunctionExpression) continue;
            if (m->HasTs(jsparse::kTsDeclare)) continue;
            std::string key;
            if (!KeyName(m, &key)) continue;
            Seen &s = seen[(m->Has(jsparse::kFlagStatic) ? "static " : "") + key];
            bool dup = false;
            if (m->kind == "get") {
                dup = s.init || s.get;
                s.get = true;
            } else if (m->kind == "set") {
                dup = s.init || s.set;
                s.set = true;
            } else {
                dup = s.init || s.get || s.set;
                s.init = true;
            }
            if (dup)
                LintNode(m->key(), JsLspSeverity::Warning, "no-dupe-class-members", "Duplicate name '" + key + "'.");
        }
    }

    void Binary(const Node *n) {
        const std::string_view op = n->op;
        const bool compare = op == "==" || op == "===" || op == "!=" || op == "!==" || op == "<" || op == ">" ||
                             op == "<=" || op == ">=";
        if (compare && (IsGlobalNaN(doc_, n->left()) || IsGlobalNaN(doc_, n->right())))
            LintNode(n, JsLspSeverity::Warning, "use-isnan", "Use the isNaN function to compare with NaN.");
        // `x !== x` is the classic NaN test; only the comparisons that can never be useful.
        if (compare && op != "!==" && op != "!=" && SameNode(n->left(), n->right()) && PlainChain(n->left()))
            LintNode(n, JsLspSeverity::Warning, "no-self-compare", "Comparing to itself is potentially pointless.");
        if (op == "==" || op == "===" || op == "!=" || op == "!==") {
            for (int side = 0; side < 2; side++) {
                const Node *t = side ? n->right() : n->left();
                const Node *v = side ? n->left() : n->right();
                if (!t || t->type != NodeType::UnaryExpression || t->op != "typeof" || !v) continue;
                if (v->type == NodeType::Literal && v->lit == jsparse::LitKind::String &&
                    !TypeofValues().count(std::string(v->str2)))
                    LintNode(v, JsLspSeverity::Warning, "valid-typeof", "Invalid typeof comparison value.");
            }
        }
        if ((op == "in" || op == "instanceof") && n->left() && n->left()->type == NodeType::UnaryExpression &&
            n->left()->op == "!" && n->left()->start == n->start)
            LintNode(n->left(), JsLspSeverity::Warning, "no-unsafe-negation",
                     "Unexpected negating the left operand of '" + std::string(op) + "' operator.");
    }

    void SelfAssign(const Node *left, const Node *right) {
        if (!left || !right) return;
        if (left->type == NodeType::Identifier && right->type == NodeType::Identifier) {
            if (left->str == right->str)
                LintNode(right, JsLspSeverity::Warning, "no-self-assign",
                         "'" + std::string(right->str) + "' is assigned to itself.");
            return;
        }
        if (left->type == NodeType::MemberExpression && PlainChain(left) && PlainChain(right) &&
            SameNode(left, right)) {
            LintNode(right, JsLspSeverity::Warning, "no-self-assign",
                     "'" + doc_.Slice(right->start, right->end) + "' is assigned to itself.");
            return;
        }
        if (left->type == NodeType::ArrayPattern && right->type == NodeType::ArrayExpression) {
            const uint32_t n = std::min(left->elements().size(), right->elements().size());
            for (uint32_t i = 0; i < n; i++) {
                const Node *l = left->elements()[i], *r = right->elements()[i];
                if (!l || !r || r->type == NodeType::SpreadElement) break;
                SelfAssign(l, r);
            }
        }
    }

    void DupeArgs(const Node *fn) {
        auto it = fn_scope_.find(fn);
        if (it != fn_scope_.end() && it->second->strict) return;
        std::set<std::string_view> seen;
        for (const Node *p : fn->params()) {
            if (!p || p->type != NodeType::Identifier) return;  // not simple: the parser rejects duplicates
        }
        for (const Node *p : fn->params()) {
            if (!seen.insert(p->str).second)
                LintNode(p, JsLspSeverity::Warning, "no-dupe-args", "Duplicate param '" + std::string(p->str) + "'.");
        }
    }
};

}  // namespace

std::vector<JsLspDiagnostic> JsLspDiagnostics(const JsLspDocument &doc) {
    Linter l(doc);
    return l.Run();
}

std::string JsLspFormatDiagnostic(const std::string &path, const JsLspDiagnostic &d) {
    const char *sev = d.severity == JsLspSeverity::Error         ? "error"
                      : d.severity == JsLspSeverity::Warning     ? "warning"
                      : d.severity == JsLspSeverity::Information ? "info"
                                                                 : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool JsLspDiagnosticFails(const JsLspDiagnostic &d) {
    return d.severity == JsLspSeverity::Error || d.severity == JsLspSeverity::Warning;
}
