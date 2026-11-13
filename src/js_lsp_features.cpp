// mep's JavaScript language server, analysis half: hover, completion,
// signature help, definition/references/rename, outline and folding. See
// js_lsp.h; the scope model is js_lsp_internal.h.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "js_lsp_internal.h"

using jsparse::Node;
using jsparse::NodeType;
using namespace jslsp;

namespace {

bool IsWordByte(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '$' || u >= 0x80;
}

bool IsFunction(const Node *n) {
    return n && (n->type == NodeType::FunctionDeclaration || n->type == NodeType::FunctionExpression ||
                 n->type == NodeType::ArrowFunctionExpression);
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
    if (out.size() > max) {
        size_t cut = max - 3;
        // Do not cut a UTF-8 sequence in half.
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) cut--;
        out = out.substr(0, cut) + "...";
    }
    return out;
}

// The innermost nodes containing off, outermost first.
std::vector<const Node *> PathAt(const JsLspDocument &doc, uint32_t off) {
    std::vector<const Node *> path;
    const Node *n = doc.parse.program;
    std::vector<Node *> kids;
    while (n) {
        path.push_back(n);
        jsparse::CollectChildren(n, kids);
        const Node *next = nullptr;
        for (const Node *k : kids) {
            if (k->start <= off && off <= k->end && k->end > k->start) next = k;
        }
        n = next;
    }
    return path;
}

bool InCommentOrString(const JsLspDocument &doc, uint32_t off) {
    for (const jsparse::Comment &c : doc.parse.comments) {
        if (c.start < off && (off < c.end || (!c.block && off == c.end))) return true;
        if (c.start >= off) break;
    }
    const std::vector<const Node *> path = PathAt(doc, off);
    if (path.empty()) return false;
    const Node *n = path.back();
    if (n->type == NodeType::Literal && (n->lit == jsparse::LitKind::String || n->lit == jsparse::LitKind::RegExp))
        return off > n->start && off < n->end;
    if (n->type == NodeType::TemplateElement || n->type == NodeType::JSXText) return true;
    return false;
}

std::string ParamsText(const JsLspDocument &doc, const Node *fn) {
    std::string s = "(";
    bool first = true;
    for (const Node *p : fn->params()) {
        if (!p) continue;
        if (!first) s += ", ";
        s += OneLine(doc.Slice(p->start, p->end), 40);
        first = false;
    }
    return s + ")";
}

// TypeScript: the `<T>` of a declaration and the `: R` of a function, as written ("" in JavaScript).
std::string TypeParamsText(const JsLspDocument &doc, const Node *n) {
    const Node *tp = n ? n->typeParameters() : nullptr;
    return tp ? OneLine(doc.Slice(tp->start, tp->end), 60) : std::string();
}

std::string ReturnText(const JsLspDocument &doc, const Node *fn) {
    const Node *rt = fn ? fn->returnType() : nullptr;
    if (!rt) return std::string();
    std::string t = OneLine(doc.Slice(rt->start, rt->end), 60);
    // A TSTypeAnnotation starts at its `:` (or an arrow type's `=>`).
    if (!t.empty() && t[0] == ':') t = t.substr(1);
    while (!t.empty() && t[0] == ' ') t.erase(0, 1);
    return t.empty() ? t : ": " + t;
}

// `: T` of a declared identifier, as written ("" when it has none).
std::string AnnotationText(const JsLspDocument &doc, const Node *id) {
    const Node *a = id ? id->typeAnnotation() : nullptr;
    if (!a) return std::string();
    std::string t = OneLine(doc.Slice(a->start, a->end), 70);
    if (!t.empty() && t[0] == ':') t = t.substr(1);
    while (!t.empty() && t[0] == ' ') t.erase(0, 1);
    return t.empty() ? t : ": " + t;
}

std::string FunctionHead(const JsLspDocument &doc, const Node *fn, const std::string &name) {
    std::string s;
    if (fn->Has(jsparse::kFlagAsync)) s += "async ";
    if (fn->type == NodeType::ArrowFunctionExpression)
        return s + name + TypeParamsText(doc, fn) + ParamsText(doc, fn) + ReturnText(doc, fn) + " => ...";
    s += "function";
    if (fn->Has(jsparse::kFlagGenerator)) s += "*";
    return s + " " + name + TypeParamsText(doc, fn) + ParamsText(doc, fn) + ReturnText(doc, fn);
}

std::string Fence(const JsLspDocument &doc) { return doc.typescript ? "```ts\n" : "```js\n"; }

const char *OriginNote(const Api &a) {
    if (std::strcmp(a.origin, "es") == 0) return "*ECMAScript*";
    if (std::strcmp(a.origin, "dom") == 0) return "*DOM*";
    return "*Node.js*";
}

// "Array.prototype.map" -> "Array.prototype.map" as written in code: "map".
std::string ApiShortName(const Api &a) {
    std::string_view n = a.name;
    const size_t dot = n.rfind('.');
    return std::string(dot == std::string_view::npos ? n : n.substr(dot + 1));
}

std::string ApiHover(const Api &a) {
    std::string out = "```ts\n";
    const std::string sig = a.signature;
    const std::string_view kind = a.kind;
    auto each_line = [&](const std::string &prefix) {
        if (sig.empty()) {
            out += prefix + "(...)\n";
            return;
        }
        size_t i = 0;
        while (i <= sig.size()) {
            size_t nl = sig.find('\n', i);
            if (nl == std::string::npos) nl = sig.size();
            out += prefix + sig.substr(i, nl - i) + "\n";
            i = nl + 1;
        }
    };
    if (kind == "function") {
        each_line(std::string("function ") + a.name);
    } else if (kind == "method") {
        each_line(std::string("(method) ") + a.name);
    } else if (kind == "property" || kind == "variable") {
        out += std::string(kind == "property" ? "(property) " : "var ") + a.name + (sig.empty() ? "" : ": " + sig) + "\n";
    } else if (kind == "class") {
        out += std::string("class ") + a.name + "\n";
    } else {
        out += std::string(a.kind) + " " + a.name + "\n";
    }
    out += "```";
    if (a.doc[0]) out += std::string("\n\n") + a.doc;
    out += std::string("\n\n") + OriginNote(a);
    return out;
}

// The receiver's owner in the vocabulary: "Math" for Math's statics, "Array.prototype" for an
// array, "" when unknown.
std::string OwnerOf(const JsLspDocument &doc, const Node *obj) {
    if (!obj) return std::string();
    if (obj->type == NodeType::Identifier) {
        auto it = doc.id_ref.find(obj);
        const bool local = it != doc.id_ref.end() && it->second->var;
        if (!local && !ApiMembers(obj->str).empty()) return std::string(obj->str);
    }
    if (obj->type == NodeType::MemberExpression && !obj->Has(jsparse::kFlagComputed) && obj->property()) {
        // window.JSON, self.Math
        const std::string inner = OwnerOf(doc, obj->object());
        if (!inner.empty()) {
            const std::string chain = inner + "." + std::string(obj->property()->str);
            if (!ApiMembers(chain).empty()) return chain;
        }
    }
    const std::string t = InferType(doc, obj);
    if (!t.empty()) return t + ".prototype";
    return std::string();
}

// Inherited members: the vocabulary lists each interface's own; walk the usual chains.
std::vector<std::string> OwnerChain(const std::string &owner) {
    std::vector<std::string> out{owner};
    const std::string suffix = ".prototype";
    if (owner.size() <= suffix.size() || owner.compare(owner.size() - suffix.size(), suffix.size(), suffix) != 0)
        return out;
    const std::string t = owner.substr(0, owner.size() - suffix.size());
    static const std::map<std::string, std::vector<std::string>> kParents = {
        {"HTMLElement", {"Element", "Node", "EventTarget"}},
        {"Element", {"Node", "EventTarget"}},
        {"Node", {"EventTarget"}},
        {"Document", {"Node", "EventTarget"}},
        {"Window", {"EventTarget"}},
        {"HTMLInputElement", {"HTMLElement", "Element", "Node", "EventTarget"}},
        {"HTMLCanvasElement", {"HTMLElement", "Element", "Node", "EventTarget"}},
    };
    auto it = kParents.find(t);
    if (it != kParents.end()) {
        for (const std::string &p : it->second) out.push_back(p + suffix);
    } else if (t.rfind("HTML", 0) == 0 && t.size() > 11 && t.compare(t.size() - 7, 7, "Element") == 0) {
        for (const char *p : {"HTMLElement", "Element", "Node", "EventTarget"}) out.push_back(std::string(p) + suffix);
    }
    if (t != "Object") out.push_back("Object.prototype");
    return out;
}

// Every vocabulary owner whose members a receiver has: a global's own ("document.title",
// "Math.max"), then its type's prototype chain.
std::vector<std::string> OwnersOf(const JsLspDocument &doc, const Node *obj) {
    std::vector<std::string> out;
    auto push = [&](const std::string &o) {
        if (!o.empty() && std::find(out.begin(), out.end(), o) == out.end()) out.push_back(o);
    };
    const std::string direct = OwnerOf(doc, obj);
    const std::string suffix = ".prototype";
    if (!direct.empty() && !(direct.size() > suffix.size() &&
                             direct.compare(direct.size() - suffix.size(), suffix.size(), suffix) == 0))
        push(direct);
    const std::string t = InferType(doc, obj);
    if (!t.empty()) {
        for (const std::string &o : OwnerChain(t + ".prototype")) push(o);
    }
    return out;
}

std::vector<std::string> OwnersOfType(const std::string &t) {
    return t.empty() ? std::vector<std::string>() : OwnerChain(t + ".prototype");
}

const Api *FindInOwners(const std::vector<std::string> &owners, std::string_view name) {
    for (const std::string &o : owners) {
        if (const Api *a = FindApi(o + "." + std::string(name))) return a;
    }
    return nullptr;
}

// When the receiver's type is unknown: the prototype member of that name from the most common
// built-in classes, if exactly one family has it.
const Api *GuessMemberApi(std::string_view name) {
    for (const char *cls : {"Array", "String", "Promise", "Map", "Set", "Object", "Number", "Function", "RegExp",
                            "Date", "Element", "HTMLElement", "Node", "EventTarget", "Document"}) {
        if (const Api *a = FindApi(std::string(cls) + ".prototype." + std::string(name))) return a;
    }
    return nullptr;
}

// ---- the file's own members ----

struct MemberDef {
    std::string name;
    const Node *key = nullptr;    // the name's node
    const Node *value = nullptr;  // the value (function for a method)
    const Node *owner = nullptr;  // Property / MethodDefinition / PropertyDefinition / AssignmentExpression
};

void ObjectMembers(const Node *obj, std::vector<MemberDef> &out) {
    if (!obj || obj->type != NodeType::ObjectExpression) return;
    for (const Node *p : obj->properties()) {
        if (!p || p->type != NodeType::Property || p->Has(jsparse::kFlagComputed) || !p->key()) continue;
        const Node *k = p->key();
        std::string name = k->type == NodeType::Identifier ? std::string(k->str)
                           : (k->type == NodeType::Literal && k->lit == jsparse::LitKind::String) ? std::string(k->str2)
                                                                                                  : std::string();
        if (name.empty()) continue;
        out.push_back({name, k, p->value(), p});
    }
}

void ClassMembers(const Node *cls, bool statics, std::vector<MemberDef> &out) {
    if (!cls || !cls->body()) return;
    for (const Node *m : cls->body()->body_list()) {
        if (!m) continue;
        // TypeScript: `constructor(private x: T)` declares a member x.
        if (!statics && m->type == NodeType::MethodDefinition && m->kind == "constructor" && m->value()) {
            for (const Node *pp : m->value()->params()) {
                if (!pp || pp->type != NodeType::TSParameterProperty) continue;
                const Node *id = pp->parameter();
                if (id && id->type == NodeType::AssignmentPattern) id = id->left();
                if (id && id->type == NodeType::Identifier) out.push_back({std::string(id->str), id, nullptr, pp});
            }
        }
        if (m->type != NodeType::MethodDefinition && m->type != NodeType::PropertyDefinition &&
            m->type != NodeType::TSAbstractMethodDefinition && m->type != NodeType::TSAbstractPropertyDefinition &&
            m->type != NodeType::AccessorProperty)
            continue;
        if (m->Has(jsparse::kFlagComputed) || !m->key() || m->kind == "constructor") continue;
        if (m->Has(jsparse::kFlagStatic) != statics) continue;
        const Node *k = m->key();
        std::string name(k->str);
        if (k->type == NodeType::PrivateIdentifier) name = "#" + name;
        if (k->type == NodeType::Literal) name = std::string(k->str2);
        if (!name.empty()) out.push_back({name, k, m->value(), m});
    }
}

// TypeScript: the members an interface or object type literal declares (and its merged
// declarations': every `interface X` of the same variable).
void TypeMembers(const Node *body, std::vector<MemberDef> &out) {
    if (!body) return;
    const jsparse::NodeList &list = body->type == NodeType::TSInterfaceBody ? body->body_list() : body->members();
    for (const Node *m : list) {
        if (!m || (m->type != NodeType::TSPropertySignature && m->type != NodeType::TSMethodSignature)) continue;
        if (m->Has(jsparse::kFlagComputed) || !m->key()) continue;
        const Node *k = m->key();
        std::string name = k->type == NodeType::Identifier ? std::string(k->str)
                           : (k->type == NodeType::Literal && k->lit == jsparse::LitKind::String) ? std::string(k->str2)
                                                                                                  : std::string();
        if (!name.empty()) out.push_back({name, k, nullptr, m});
    }
}

// The declared type of a variable (`x: Foo`, `x: {a: 1}`), as the node of its annotation's type.
const Node *DeclaredType(const Var *v) {
    if (!v || v->ids.empty()) return nullptr;
    const Node *a = v->ids[0]->typeAnnotation();
    return a && a->type == NodeType::TSTypeAnnotation ? a->typeAnnotation() : nullptr;
}

const Node *ClassOfVar(const Var *v) {
    if (!v) return nullptr;
    if (v->kind == VarKind::Class && v->decl) return v->decl;
    if (v->init && v->init->type == NodeType::ClassExpression) return v->init;
    return nullptr;
}

const Node *EnclosingClass(const JsLspDocument &doc, const Node *n) {
    // `this` inside a method (not inside a nested non-arrow function) is the class's instance.
    for (const Node *p = doc.Parent(n); p; p = doc.Parent(p)) {
        if (p->type == NodeType::ClassBody) return doc.Parent(p);
        if (p->type == NodeType::FunctionDeclaration) return nullptr;
        if (p->type == NodeType::FunctionExpression) {
            const Node *pp = doc.Parent(p);
            if (!pp || pp->type != NodeType::MethodDefinition) return nullptr;
        }
    }
    return nullptr;
}

const Var *VarOfExpr(const JsLspDocument &doc, const Node *e) {
    if (!e || e->type != NodeType::Identifier) return nullptr;
    auto it = doc.id_ref.find(e);
    if (it != doc.id_ref.end()) return it->second->var;
    auto dit = doc.decl_var.find(e);  // a declaring identifier
    return dit == doc.decl_var.end() ? nullptr : dit->second;
}

// The members the file gives the value of `obj`: an object literal's keys, a class's members,
// `obj.x = ...` assignments, and for `this` the enclosing class's members and `this.x = `.
std::vector<MemberDef> FileMembers(const JsLspDocument &doc, const Node *obj) {
    std::vector<MemberDef> out;
    if (!obj) return out;
    const Var *v = VarOfExpr(doc, obj);
    const Node *cls = nullptr;
    bool statics = false;
    if (obj->type == NodeType::ThisExpression) {
        cls = EnclosingClass(doc, obj);
    } else if (v) {
        cls = ClassOfVar(v);
        statics = cls != nullptr;
        // TypeScript: `x: Foo` -- the members of the file's class or interface Foo, or of a type literal.
        if (const Node *t = cls ? nullptr : DeclaredType(v)) {
            if (t->type == NodeType::TSTypeLiteral) TypeMembers(t, out);
            if (t->type == NodeType::TSTypeReference && t->typeName() && t->typeName()->type == NodeType::Identifier) {
                auto it = doc.id_ref.find(t->typeName());
                const Var *tv = it == doc.id_ref.end() ? nullptr : it->second->var;
                if (tv && tv->kind == VarKind::Class && tv->decl) {
                    ClassMembers(tv->decl, false, out);
                } else if (tv) {
                    for (const Node *id : tv->ids) {
                        const Node *d = doc.Parent(id);
                        if (d && d->type == NodeType::TSInterfaceDeclaration) TypeMembers(d->body(), out);
                        if (d && d->type == NodeType::TSTypeAliasDeclaration && d->typeAnnotation() &&
                            d->typeAnnotation()->type == NodeType::TSTypeLiteral)
                            TypeMembers(d->typeAnnotation(), out);
                    }
                }
            }
        }
        if (!cls && v->init) {
            const Node *init = v->init;
            if (init->type == NodeType::NewExpression) cls = ClassOfVar(VarOfExpr(doc, init->callee()));
            ObjectMembers(init, out);
        }
    }
    if (cls) ClassMembers(cls, statics, out);
    // Assignments: same variable (or `this` inside the same class), then `.prototype.` ones.
    std::vector<const Node *> stack{doc.parse.program};
    std::vector<Node *> kids;
    while (!stack.empty()) {
        const Node *n = stack.back();
        stack.pop_back();
        if (n->type == NodeType::AssignmentExpression && n->left() &&
            n->left()->type == NodeType::MemberExpression && !n->left()->Has(jsparse::kFlagComputed)) {
            const Node *lo = n->left()->object();
            bool match = false;
            if (v && lo && VarOfExpr(doc, lo) == v) match = true;
            if (!v && obj->type == NodeType::Identifier && lo && lo->type == NodeType::Identifier &&
                lo->str == obj->str && !VarOfExpr(doc, lo))
                match = true;
            if (obj->type == NodeType::ThisExpression && lo && lo->type == NodeType::ThisExpression &&
                EnclosingClass(doc, lo) == cls && cls)
                match = true;
            // C.prototype.m = function ... for an instance of C
            if (lo && lo->type == NodeType::MemberExpression && !lo->Has(jsparse::kFlagComputed) && lo->property() &&
                lo->property()->str == "prototype" && v && v->init && v->init->type == NodeType::NewExpression &&
                lo->object() && v->init->callee() && lo->object()->type == NodeType::Identifier &&
                v->init->callee()->type == NodeType::Identifier && lo->object()->str == v->init->callee()->str)
                match = true;
            if (match && n->left()->property())
                out.push_back({std::string(n->left()->property()->str), n->left()->property(), n->right(), n});
        }
        jsparse::CollectChildren(n, kids);
        for (const Node *k : kids) stack.push_back(k);
    }
    return out;
}

std::string MemberHover(const JsLspDocument &doc, const MemberDef &m) {
    std::string head;
    const Node *o = m.owner;
    if (o && (o->type == NodeType::TSPropertySignature || o->type == NodeType::TSMethodSignature ||
              o->type == NodeType::TSAbstractPropertyDefinition || o->type == NodeType::TSAbstractMethodDefinition ||
              (o->type == NodeType::PropertyDefinition && o->typeAnnotation()) ||
              (o->type == NodeType::MethodDefinition && m.value &&
               m.value->type == NodeType::TSEmptyBodyFunctionExpression))) {
        // TypeScript: the member as declared, `name?: T` / `m<T>(a: A): R`.
        const bool method = o->type == NodeType::TSMethodSignature || o->type == NodeType::TSAbstractMethodDefinition ||
                            o->type == NodeType::MethodDefinition;
        std::string text = OneLine(doc.Slice(o->start, o->end), 120);
        if (!text.empty() && (text.back() == ';' || text.back() == ',')) text.pop_back();
        head = std::string(method ? "(method) " : "(property) ") + text;
    } else if (IsFunction(m.value)) {
        head = FunctionHead(doc, m.value, m.name);
        if (m.owner && m.owner->type == NodeType::MethodDefinition) head = "(method) " + m.name + ParamsText(doc, m.value);
    } else if (m.value) {
        head = "(property) " + m.name + ": " + OneLine(doc.Slice(m.value->start, m.value->end), 60);
    } else {
        head = "(property) " + m.name;
    }
    std::string out = Fence(doc) + head + "\n```";
    const std::string jsdoc = doc.DocCommentFor(m.owner && m.owner->type != NodeType::AssignmentExpression ? m.key : m.owner);
    if (!jsdoc.empty()) out += "\n\n" + jsdoc;
    return out;
}

const char *KindWord(VarKind k) {
    switch (k) {
        case VarKind::Var:
            return "var";
        case VarKind::Let:
            return "let";
        case VarKind::Const:
            return "const";
        case VarKind::Using:
            return "using";
        default:
            return "";
    }
}

// `type_note`: what the declaration files say the value is (`Circle`), "" for the file's own inference.
std::string VarHover(const JsLspDocument &doc, const Var *v, const std::string &type_note = std::string()) {
    const std::string name(v->name);
    std::string head;
    switch (v->kind) {
        case VarKind::Function:
        case VarKind::FunctionName:
            head = v->decl ? FunctionHead(doc, v->decl, name) : "function " + name;
            break;
        case VarKind::Class:
        case VarKind::ClassName: {
            head = std::string(v->decl && v->decl->HasTs(jsparse::kTsAbstract) ? "abstract class " : "class ") + name +
                   TypeParamsText(doc, v->decl);
            if (v->decl && v->decl->superClass())
                head += " extends " + OneLine(doc.Slice(v->decl->superClass()->start, v->decl->superClass()->end), 40);
            if (v->decl && !v->decl->implements().empty()) {
                const jsparse::NodeList &im = v->decl->implements();
                head += " implements " + OneLine(doc.Slice(im[0]->start, im[im.size() - 1]->end), 50);
            }
            break;
        }
        case VarKind::Param: {
            const Node *id = v->ids.empty() ? nullptr : v->ids[0];
            const Node *p = id ? doc.Parent(id) : nullptr;
            const Node *pp = p && p->type == NodeType::AssignmentPattern ? doc.Parent(p) : p;
            if (v->param_property && pp && pp->type == NodeType::TSParameterProperty) {
                // `constructor(private readonly x: T)`
                std::string mods;
                if (!pp->accessibility.empty()) mods += std::string(pp->accessibility) + " ";
                if (pp->HasTs(jsparse::kTsReadonly)) mods += "readonly ";
                head = "(property) " + mods + name + AnnotationText(doc, id);
            } else {
                head = "(parameter) " + name + AnnotationText(doc, id);
            }
            if (p && p->type == NodeType::AssignmentPattern && p->left() == id && p->right())
                head += " = " + OneLine(doc.Slice(p->right()->start, p->right()->end), 40);
            break;
        }
        case VarKind::CatchParam:
            head = "(exception) " + name + AnnotationText(doc, v->ids.empty() ? nullptr : v->ids[0]);
            break;
        case VarKind::Interface: {
            const Node *d = v->decl;
            const Node *body = d ? d->body() : nullptr;
            head = d && body ? OneLine(doc.Slice(d->start, body->start), 100) : "interface " + name;
            if (v->ids.size() > 1) head += "  (+" + std::to_string(v->ids.size() - 1) + " merged)";
            break;
        }
        case VarKind::TypeAlias:
            head = v->decl ? OneLine(doc.Slice(v->decl->start, v->decl->end), 120) : "type " + name;
            if (!head.empty() && head.back() == ';') head.pop_back();
            break;
        case VarKind::Enum: {
            const Node *d = v->decl;
            head = std::string(d && d->HasTs(jsparse::kTsConst) ? "const enum " : "enum ") + name;
            if (d) head = OneLine(doc.Slice(d->start, d->end), 100);
            break;
        }
        case VarKind::EnumMember: {
            const Node *body = v->decl ? doc.Parent(v->decl) : nullptr;
            const Node *en = body && body->type != NodeType::TSEnumDeclaration ? doc.Parent(body) : body;
            const std::string owner = en && en->id() ? std::string(en->id()->str) + "." : std::string();
            head = "(enum member) " + owner + (v->decl ? OneLine(doc.Slice(v->decl->start, v->decl->end), 60) : name);
            break;
        }
        case VarKind::Namespace:
            head = "namespace " + name;
            if (v->decl && v->decl->kind == "module") head = "module " + name;
            break;
        case VarKind::TypeParam: {
            const Node *d = v->decl;
            if (d && d->type == NodeType::TSTypeParameter) {
                const Node *p = doc.Parent(d);
                head = std::string(p && p->type == NodeType::TSInferType ? "(type parameter, inferred) "
                                                                          : "(type parameter) ") +
                       OneLine(doc.Slice(d->start, d->end), 80);
            } else {
                head = "(type parameter) " + name;
            }
            break;
        }
        case VarKind::Arguments:
            head = "arguments";
            break;
        case VarKind::Import: {
            const Node *spec = v->decl;
            if (spec && spec->type == NodeType::TSImportEqualsDeclaration) {
                head = OneLine(doc.Slice(spec->start, spec->end), 100);
                break;
            }
            const Node *decl = spec ? doc.Parent(spec) : nullptr;
            if (decl && decl->HasTs(jsparse::kTsTypeOnly)) {
                const std::string src = decl->source() ? doc.Slice(decl->source()->start, decl->source()->end) : "";
                if (spec->type == NodeType::ImportDefaultSpecifier)
                    head = "import type " + name + " from " + src;
                else if (spec->type == NodeType::ImportNamespaceSpecifier)
                    head = "import type * as " + name + " from " + src;
                else
                    head = "import type { " + doc.Slice(spec->start, spec->end) + " } from " + src;
                break;
            }
            std::string src = decl && decl->source() ? doc.Slice(decl->source()->start, decl->source()->end) : "";
            if (spec && spec->type == NodeType::ImportDefaultSpecifier)
                head = "import " + name + " from " + src;
            else if (spec && spec->type == NodeType::ImportNamespaceSpecifier)
                head = "import * as " + name + " from " + src;
            else
                head = "import { " + (spec ? doc.Slice(spec->start, spec->end) : name) + " } from " + src;
            break;
        }
        default: {
            head = std::string(KindWord(v->kind)) + " " + name + AnnotationText(doc, v->ids.empty() ? nullptr : v->ids[0]);
            if (v->ambient && v->decl) {
                const Node *vd = doc.Parent(v->decl);
                if (vd && vd->HasTs(jsparse::kTsDeclare)) head = "declare " + head;
            }
            if (v->init) {
                if (IsFunction(v->init))
                    head = std::string(KindWord(v->kind)) + " " + name + " = " + FunctionHead(doc, v->init, "").substr(0);
                else
                    head += " = " + OneLine(doc.Slice(v->init->start, v->init->end), 70);
            }
            break;
        }
    }
    std::string out = Fence(doc) + head + "\n```";
    if (v->kind == VarKind::Arguments) out += "\n\nThe arguments the function was called with (array-like).";
    if (!type_note.empty()) {
        out += "\n\n*Type:* `" + type_note + "`";
    } else if (v->init && !IsFunction(v->init) && v->init->type != NodeType::Literal) {
        const std::string t = InferType(doc, v->init);
        if (!t.empty() && t != "Object" && t != "Function") out += "\n\n*Type:* `" + t + "`";
    }
    if (!v->ids.empty()) {
        const bool decl_doc = v->kind == VarKind::Function || v->kind == VarKind::Class ||
                              v->kind == VarKind::Interface || v->kind == VarKind::TypeAlias ||
                              v->kind == VarKind::Enum || v->kind == VarKind::Namespace ||
                              v->kind == VarKind::EnumMember;
        const std::string jsdoc = doc.DocCommentFor(decl_doc && v->decl ? v->decl : v->ids[0]);
        if (!jsdoc.empty()) out += "\n\n" + jsdoc;
        // A parameter's @param line from the function's JSDoc.
        if (v->kind == VarKind::Param && v->decl) {
            const std::string fdoc = doc.DocCommentFor(v->decl);
            const std::string tag = "*@param* `" + name + "`";
            const size_t at = fdoc.find(tag);
            if (at != std::string::npos) {
                size_t end = fdoc.find("\n\n", at);
                out += "\n\n" + fdoc.substr(at, end == std::string::npos ? std::string::npos : end - at);
            }
        }
    }
    int reads = 0, writes = 0;
    for (const Ref *r : v->refs) {
        if (r->read) reads++;
        if (r->write) writes++;
    }
    if (!v->ids.empty() && v->kind != VarKind::Arguments) {
        out += "\n\n" + std::to_string(reads) + " read" + (reads == 1 ? "" : "s");
        if (writes) out += ", " + std::to_string(writes) + " write" + (writes == 1 ? "" : "s");
        if (v->exported) out += " · exported";
    }
    return out;
}

const char *const kKeywords[] = {
    "async",  "await",   "break",   "case",     "catch",  "class",      "const",  "continue", "debugger",
    "default", "delete", "do",      "else",     "export", "extends",    "false",  "finally",  "for",
    "function", "if",    "import",  "in",       "instanceof", "let",    "new",    "null",     "of",
    "return", "static",  "super",   "switch",   "this",   "throw",      "true",   "try",      "typeof",
    "undefined", "var",  "void",    "while",    "yield",
};

JsLspKind CompletionKindOfApi(const Api &a) {
    const std::string_view k = a.kind;
    if (k == "function") return JsLspKind::Function;
    if (k == "method") return JsLspKind::Method;
    if (k == "property") return JsLspKind::Property;
    if (k == "class") return JsLspKind::Class;
    if (k == "interface" || k == "type") return JsLspKind::Interface;
    if (k == "namespace") return JsLspKind::Module;
    return JsLspKind::Variable;
}

JsLspKind CompletionKindOfVar(const Var *v) {
    switch (v->kind) {
        case VarKind::Function:
        case VarKind::FunctionName:
            return JsLspKind::Function;
        case VarKind::Class:
        case VarKind::ClassName:
            return JsLspKind::Class;
        case VarKind::Const:
            return v->init && IsFunction(v->init) ? JsLspKind::Function : JsLspKind::Constant;
        case VarKind::Import:
            return JsLspKind::Module;
        default:
            return v->init && IsFunction(v->init) ? JsLspKind::Function : JsLspKind::Variable;
    }
}

std::string FirstSigLine(const char *sig) {
    std::string s = sig;
    const size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

// The receiver chain text ending just before `dot` ("a.b.c", "this.x", "foo()"), walking back over
// names, dots and balanced brackets.
uint32_t ReceiverStart(const std::string &t, uint32_t dot) {
    uint32_t i = dot;
    while (i > 0 && (t[i - 1] == ' ' || t[i - 1] == '\t')) i--;
    for (;;) {
        if (i == 0) return 0;
        const char c = t[i - 1];
        if (c == '\'' || c == '"' || c == '`') {
            uint32_t j = i - 1;
            while (j > 0 && !(t[j - 1] == c && (j < 2 || t[j - 2] != '\\'))) j--;
            return j > 0 ? j - 1 : i;
        }
        if (c == ')' || c == ']') {
            const char open = c == ')' ? '(' : '[';
            int depth = 0;
            uint32_t j = i;
            while (j > 0) {
                const char d = t[j - 1];
                if (d == c) depth++;
                if (d == open && --depth == 0) break;
                j--;
            }
            if (j == 0) return i;
            i = j - 1;
            // A call or index continues the chain leftwards (`a.b(1)[2]`); a bare bracket is a literal.
            if (i > 0 && (IsWordByte(t[i - 1]) || t[i - 1] == ')' || t[i - 1] == ']')) continue;
            if (i > 1 && t[i - 1] == '.' && t[i - 2] == '?') {
                i -= 2;
                continue;
            }
            return i;
        }
        if (IsWordByte(c)) {
            while (i > 0 && IsWordByte(t[i - 1])) i--;
            if (i > 0 && t[i - 1] == '.' && !(i > 1 && t[i - 2] == '.')) {
                i--;
                if (i > 0 && t[i - 1] == '?') i--;
                continue;
            }
            return i;
        }
        return i;
    }
}

// The node for a receiver: an expression in the tree ending exactly at `end` (the '.' position),
// starting at `start`.
const Node *ExprAt(const JsLspDocument &doc, uint32_t start, uint32_t end) {
    const Node *best = nullptr;
    std::vector<const Node *> stack{doc.parse.program};
    std::vector<Node *> kids;
    while (!stack.empty()) {
        const Node *n = stack.back();
        stack.pop_back();
        if (n->start > end || n->end < start) continue;
        if (n->start == start && n->end == end && n->type != NodeType::ExpressionStatement) {
            if (!best) best = n;
        }
        jsparse::CollectChildren(n, kids);
        for (const Node *k : kids) stack.push_back(k);
    }
    return best;
}

bool IsTsTypeNode(NodeType t) {
    const char *n = jsparse::NodeTypeName(t);
    // TS* type nodes, not the TS declarations/expressions that hold values.
    if (std::strncmp(n, "TS", 2) != 0) return false;
    switch (t) {
        case NodeType::TSAsExpression:
        case NodeType::TSSatisfiesExpression:
        case NodeType::TSNonNullExpression:
        case NodeType::TSTypeAssertion:
        case NodeType::TSInstantiationExpression:
        case NodeType::TSEnumDeclaration:
        case NodeType::TSEnumBody:
        case NodeType::TSEnumMember:
        case NodeType::TSModuleDeclaration:
        case NodeType::TSModuleBlock:
        case NodeType::TSDeclareFunction:
        case NodeType::TSEmptyBodyFunctionExpression:
        case NodeType::TSAbstractMethodDefinition:
        case NodeType::TSAbstractPropertyDefinition:
        case NodeType::TSAbstractAccessorProperty:
        case NodeType::TSParameterProperty:
        case NodeType::TSImportEqualsDeclaration:
        case NodeType::TSExternalModuleReference:
        case NodeType::TSExportAssignment:
        case NodeType::TSNamespaceExportDeclaration:
        case NodeType::TSTypeQuery:  // `typeof x`: x is a value
            return false;
        default:
            return true;
    }
}

// Whether the word starting at `ps` is in a type: by the tree when the text parses there, else
// by what precedes it (`x: `, `): `, `as `, `satisfies `, `implements `, `keyof `, `Foo<`).
bool InTypePosition(const JsLspDocument &doc, uint32_t ps) {
    if (!doc.typescript) return false;
    const std::vector<const Node *> path = PathAt(doc, ps);
    for (size_t i = path.size(); i-- > 0;) {
        const Node *n = path[i];
        if (n->type == NodeType::Identifier || n->type == NodeType::TSQualifiedName) continue;
        if (n->type == NodeType::TSTypeQuery) return false;
        if (n->type == NodeType::TSTypeAnnotation || n->type == NodeType::TSTypeReference ||
            n->type == NodeType::TSTypeParameterInstantiation || n->type == NodeType::TSInterfaceHeritage ||
            n->type == NodeType::TSClassImplements)
            return true;
        if (IsTsTypeNode(n->type) && n->type != NodeType::TSInterfaceBody && n->type != NodeType::TSTypeLiteral &&
            n->type != NodeType::TSTypeParameterDeclaration && n->type != NodeType::TSTypeParameter)
            return true;
        break;
    }
    const std::string &t = doc.text;
    uint32_t i = ps;
    auto skip_space = [&](uint32_t k) {
        while (k > 0 && (t[k - 1] == ' ' || t[k - 1] == '\t')) k--;
        return k;
    };
    i = skip_space(i);
    if (i == 0) return false;
    // The word before: `as`, `satisfies`, `implements`, `keyof`, `is`, `extends` (not a class's).
    {
        uint32_t w = i;
        while (w > 0 && IsWordByte(t[w - 1])) w--;
        const std::string word = t.substr(w, i - w);
        if (word == "as" || word == "satisfies" || word == "implements" || word == "keyof" || word == "is" ||
            word == "infer")
            return true;
        if (word == "extends") {
            const size_t ls = t.rfind('\n', w);
            const std::string line = t.substr(ls == std::string::npos ? 0 : ls + 1, w);
            return line.find("class ") == std::string::npos;
        }
    }
    const char c = t[i - 1];
    if (c == '<') return i >= 2 && IsWordByte(t[i - 2]);  // Foo<|
    if (c == '|' || c == '&') {
        // `type U = A | |`
        const size_t ls = t.rfind('\n', i - 1);
        const std::string line = t.substr(ls == std::string::npos ? 0 : ls + 1, i);
        return line.find("type ") != std::string::npos || line.find(':') != std::string::npos;
    }
    if (c == '=') {
        // `type X<T> = |`
        const size_t ls = t.rfind('\n', i - 1);
        std::string line = t.substr(ls == std::string::npos ? 0 : ls + 1, i - 1 - (ls == std::string::npos ? 0 : ls + 1));
        size_t k = line.find_first_not_of(" \t");
        if (k == std::string::npos) return false;
        line = line.substr(k);
        if (line.rfind("export ", 0) == 0) line = line.substr(7);
        if (line.rfind("declare ", 0) == 0) line = line.substr(8);
        return line.rfind("type ", 0) == 0;
    }
    if (c != ':') return false;
    // `name: |`, `name?: |`, `): |`
    uint32_t j = skip_space(i - 1);
    if (j > 0 && t[j - 1] == ')') return true;
    if (j > 0 && t[j - 1] == '?') j--;
    uint32_t w = j;
    while (w > 0 && IsWordByte(t[w - 1])) w--;
    if (w == j) return false;
    const uint32_t before = skip_space(w);
    {
        uint32_t w2 = before;
        while (w2 > 0 && IsWordByte(t[w2 - 1])) w2--;
        const std::string word = t.substr(w2, before - w2);
        if (word == "let" || word == "const" || word == "var" || word == "readonly" || word == "private" ||
            word == "public" || word == "protected" || word == "static" || word == "declare" || word == "accessor")
            return true;
    }
    // Inside which bracket? `(`: a parameter; `{`: a class body or a type, not an object literal.
    int depth = 0;
    for (uint32_t k = w; k > 0; k--) {
        const char d = t[k - 1];
        if (d == ')' || d == ']' || d == '}') depth++;
        if (d == '(' || d == '[' || d == '{') {
            if (depth > 0) {
                depth--;
                continue;
            }
            if (d == '(') return t.find('?', k) > w;  // not a ternary's `:`
            if (d == '[') return false;
            // `{`: what holds it?
            const std::vector<const Node *> p2 = PathAt(doc, k);
            for (size_t q = p2.size(); q-- > 0;) {
                const NodeType ty = p2[q]->type;
                if (ty == NodeType::ClassBody || ty == NodeType::TSInterfaceBody || ty == NodeType::TSTypeLiteral)
                    return true;
                if (ty == NodeType::ObjectExpression || ty == NodeType::BlockStatement || ty == NodeType::ObjectPattern)
                    return false;
            }
            return false;
        }
    }
    return false;
}

// ---- declaration files (js_lsp_dts.h) ----
//
// Just enough inference for a module a declaration file types: `require("x")` and `import`
// give its exports, `new C(...)` an instance of the class, a call what its (first) signature
// declares it returns, `.p` the declared type of a property. A variable takes the value of its
// initializer, or of the module it destructures; nothing else is tracked (parameters,
// reassignments and loops stay unknown).

struct DtsVal {
    enum K { None, Namespace, Class, Instance, Function, Enum, EnumMember, Method, Property, Variable } k = None;
    const JsDtsModule *mod = nullptr;
    const JsDtsScope *scope = nullptr;    // Namespace: whose exports
    const JsDtsDecl *decl = nullptr;      // the declaration; Method / Property / EnumMember: the owner
    const JsDtsMember *member = nullptr;  // Method / Property / EnumMember
    const JsDtsDecl *self = nullptr;      // Method / Property: the instance's class (what `this` returns)
};

DtsVal DeclVal(const JsDtsModule *m, const JsDtsDecl *d) {
    DtsVal v;
    if (!m || !d) return v;
    v.mod = m;
    v.decl = d;
    switch (d->kind) {
        case JsDtsDecl::Function:
            v.k = DtsVal::Function;
            break;
        case JsDtsDecl::Class:
            v.k = DtsVal::Class;
            break;
        case JsDtsDecl::Enum:
            v.k = DtsVal::Enum;
            break;
        case JsDtsDecl::Variable:
            v.k = DtsVal::Variable;
            break;
        case JsDtsDecl::Namespace:
            v.k = DtsVal::Namespace;
            v.scope = d->ns.get();
            break;
        default:  // an interface or a type alias: no value
            return DtsVal();
    }
    return v;
}

// A value of the declared type `type` (written in `scope`): an instance of the class it names.
DtsVal ValueOfType(const JsDtsModule *m, const JsDtsScope *scope, const std::string &type, const JsDtsDecl *self) {
    DtsVal v;
    if (!m) return v;
    std::string t = type;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t == "this" && self) {
        v.k = DtsVal::Instance;
        v.mod = m;
        v.decl = self;
        return v;
    }
    if (t.rfind("typeof ", 0) == 0) return DeclVal(m, scope ? scope->Find(t.substr(7)) : nullptr);
    if (const JsDtsDecl *c = m->ClassOfType(scope, t)) {
        v.k = DtsVal::Instance;
        v.mod = m;
        v.decl = c;
    }
    return v;
}

// A variable or property stands for a value of its declared type.
DtsVal Resolved(const DtsVal &v) {
    if (v.k == DtsVal::Variable) return ValueOfType(v.mod, v.decl->scope, v.decl->type, nullptr);
    if (v.k == DtsVal::Property) return ValueOfType(v.mod, v.decl->scope, v.member->type, v.self);
    return v;
}

// What `import`/`require` of `spec` gives: its exports, or what `export =` names (`deflt`: the
// default import, which is the whole module for one with no default export, as node gives a
// CommonJS module).
DtsVal ModuleVal(const JsLspDocument &doc, const std::string &spec, bool deflt) {
    DtsVal v;
    const JsDtsModule *m = doc.DtsModule(spec);
    if (!m) return v;
    const JsDtsScope *sc = &m->root;
    auto amb = m->ambient.find(spec);
    if (amb != m->ambient.end()) sc = amb->second.get();
    if (deflt) {
        if (const JsDtsDecl *d = sc->Export("default")) return DeclVal(m, d);
    }
    if (const JsDtsDecl *eq = sc->ExportEquals()) {
        const DtsVal e = DeclVal(m, eq);
        if (e.k != DtsVal::None) return e;
    }
    v.k = DtsVal::Namespace;
    v.mod = m;
    v.scope = sc;
    return v;
}

DtsVal MemberOf(const DtsVal &base0, const std::string &name) {
    const DtsVal base = Resolved(base0);
    DtsVal v;
    if (base.k == DtsVal::None || name.empty()) return v;
    const JsDtsModule *m = base.mod;
    auto from_ns = [&](const JsDtsDecl *d) {
        return d && d->ns ? DeclVal(m, d->ns->Export(name)) : DtsVal();
    };
    auto member = [&](const JsDtsDecl *cls, bool statics) {
        const JsDtsDecl *owner = nullptr;
        const JsDtsMember *mm = m->Member(cls, name, statics, &owner);
        DtsVal r;
        if (!mm) return r;
        r.k = mm->kind == JsDtsMember::Method ? DtsVal::Method : DtsVal::Property;
        r.mod = m;
        r.decl = owner;
        r.member = mm;
        r.self = statics ? nullptr : cls;
        return r;
    };
    switch (base.k) {
        case DtsVal::Namespace:
            return DeclVal(m, base.scope ? base.scope->Export(name) : nullptr);
        case DtsVal::Class:
            v = member(base.decl, true);
            return v.k != DtsVal::None ? v : from_ns(base.decl);
        case DtsVal::Instance:
            return member(base.decl, false);
        case DtsVal::Function:
            return from_ns(base.decl);
        case DtsVal::Enum:
            for (const JsDtsMember &mm : base.decl->members) {
                if (mm.kind != JsDtsMember::EnumMember || mm.name != name) continue;
                v.k = DtsVal::EnumMember;
                v.mod = m;
                v.decl = base.decl;
                v.member = &mm;
                return v;
            }
            return from_ns(base.decl);
        default:
            return v;
    }
}

// What calling (`new`: constructing) a value gives.
DtsVal CallOf(const DtsVal &callee0, bool is_new) {
    const DtsVal callee = Resolved(callee0);
    DtsVal v;
    if (is_new) {
        if (callee.k != DtsVal::Class) return v;
        v.k = DtsVal::Instance;
        v.mod = callee.mod;
        v.decl = callee.decl;
        return v;
    }
    if (callee.k == DtsVal::Function && !callee.decl->sigs.empty())
        return ValueOfType(callee.mod, callee.decl->scope, callee.decl->sigs[0].ret, nullptr);
    if (callee.k == DtsVal::Method && !callee.member->sigs.empty())
        return ValueOfType(callee.mod, callee.decl->scope, callee.member->sigs[0].ret, callee.self);
    return v;
}

DtsVal ExprVal(const JsLspDocument &doc, const Node *e, int depth);

std::string KeyName(const Node *k) {
    if (!k) return std::string();
    if (k->type == NodeType::Identifier) return std::string(k->str);
    if (k->type == NodeType::Literal && k->lit == jsparse::LitKind::String) return std::string(k->str2);
    return std::string();
}

std::string StringLiteral(const Node *n) {
    return n && n->type == NodeType::Literal && n->lit == jsparse::LitKind::String ? std::string(n->str2)
                                                                                  : std::string();
}

DtsVal VarVal(const JsLspDocument &doc, const Var *var, int depth) {
    if (!var || depth > 32) return DtsVal();
    if (var->kind == VarKind::Import && var->decl) {
        const Node *spec = var->decl;
        if (spec->type == NodeType::TSImportEqualsDeclaration) {
            // import x = require("y")
            const Node *ref = spec->moduleReference();
            if (ref && ref->type == NodeType::TSExternalModuleReference) {
                const std::string s = StringLiteral(ref->expression());
                if (!s.empty()) return ModuleVal(doc, s, false);
            }
            return DtsVal();
        }
        const Node *decl = doc.Parent(spec);
        const std::string src = decl && decl->type == NodeType::ImportDeclaration ? StringLiteral(decl->source()) : "";
        if (src.empty()) return DtsVal();
        if (spec->type == NodeType::ImportNamespaceSpecifier) return ModuleVal(doc, src, false);
        if (spec->type == NodeType::ImportDefaultSpecifier) return ModuleVal(doc, src, true);
        const std::string name = KeyName(spec->imported());
        if (name == "default") return ModuleVal(doc, src, true);
        return MemberOf(ModuleVal(doc, src, false), name);
    }
    if (var->kind != VarKind::Const && var->kind != VarKind::Let && var->kind != VarKind::Var) return DtsVal();
    if (var->in_pattern && !var->ids.empty() && var->decl && var->decl->type == NodeType::VariableDeclarator) {
        // const { Circle, add: plus } = require("x"): the source's member of the key's name.
        const Node *id = var->ids[0];
        const Node *p = doc.Parent(id);
        if (p && p->type == NodeType::AssignmentPattern) p = doc.Parent(p);
        const Node *pat = p ? doc.Parent(p) : nullptr;
        if (!p || p->type != NodeType::Property || p->Has(jsparse::kFlagComputed) || pat != var->decl->id())
            return DtsVal();
        return MemberOf(ExprVal(doc, var->decl->init(), depth + 1), KeyName(p->key()));
    }
    return ExprVal(doc, var->init, depth + 1);
}

DtsVal ExprVal(const JsLspDocument &doc, const Node *e, int depth) {
    if (!e || depth > 32) return DtsVal();
    switch (e->type) {
        case NodeType::Identifier:
            return VarVal(doc, VarOfExpr(doc, e), depth + 1);
        case NodeType::ParenthesizedExpression:
        case NodeType::ChainExpression:
        case NodeType::TSNonNullExpression:
        case NodeType::TSAsExpression:
        case NodeType::TSSatisfiesExpression:
            return ExprVal(doc, e->expression(), depth + 1);
        case NodeType::CallExpression: {
            const Node *c = e->callee();
            if (c && c->type == NodeType::Identifier && c->str == "require" && !VarOfExpr(doc, c) &&
                e->arguments().size() == 1) {
                const std::string s = StringLiteral(e->arguments()[0]);
                return s.empty() ? DtsVal() : ModuleVal(doc, s, false);
            }
            return CallOf(ExprVal(doc, c, depth + 1), false);
        }
        case NodeType::NewExpression:
            return CallOf(ExprVal(doc, e->callee(), depth + 1), true);
        case NodeType::MemberExpression: {
            const std::string name = e->Has(jsparse::kFlagComputed) ? StringLiteral(e->property())
                                                                    : KeyName(e->property());
            return MemberOf(ExprVal(doc, e->object(), depth + 1), name);
        }
        default:
            return DtsVal();
    }
}

// A receiver written as text (`dm`, `dm.Circle.unit()`, `v.set_x(1)`, `require("x")`), when a
// half-typed line kept it out of the tree: the root by the scope at `off`, then member by member.
// `is_new`: the text follows `new`, which applies to its first call (`new dm.Vec2(1, 2).`).
DtsVal DtsValOfText(const JsLspDocument &doc, uint32_t off, const std::string &recv, bool is_new = false) {
    std::vector<std::string> parts{std::string()}, args{std::string()};
    std::vector<bool> called{false};
    for (size_t i = 0; i < recv.size(); i++) {
        const char ch = recv[i];
        if (ch == '(' || ch == '[') {
            const char close = ch == '(' ? ')' : ']';
            int depth = 0;
            const size_t from = i;
            for (; i < recv.size(); i++) {
                if (recv[i] == ch) depth++;
                if (recv[i] == close && --depth == 0) break;
            }
            if (ch == '[') return DtsVal();  // an index: unknown
            called.back() = true;
            args.back() = recv.substr(from + 1, i > from ? i - from - 1 : 0);
            continue;
        }
        if (ch == '?') continue;
        if (ch == '.') {
            parts.emplace_back();
            args.emplace_back();
            called.push_back(false);
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\n') continue;
        parts.back() += ch;
    }
    const std::string &root = parts[0];
    if (!IsIdentifierName(root)) return DtsVal();
    const Var *var = nullptr;
    for (const Scope *s = doc.ScopeAt(off); s && !var; s = s->parent) var = s->Find(root);
    DtsVal v;
    if (!var && root == "require" && called[0]) {
        std::string a = args[0];
        while (!a.empty() && (a.back() == ' ' || a.back() == ',')) a.pop_back();
        while (!a.empty() && a[0] == ' ') a.erase(0, 1);
        if (a.size() < 2 || (a[0] != '"' && a[0] != '\'') || a.back() != a[0]) return DtsVal();
        v = ModuleVal(doc, a.substr(1, a.size() - 2), false);
    } else {
        v = VarVal(doc, var, 0);
        if (called[0]) {
            v = CallOf(v, is_new);
            is_new = false;
        }
    }
    for (size_t k = 1; k < parts.size() && v.k != DtsVal::None; k++) {
        v = MemberOf(v, parts[k]);
        if (called[k]) {
            v = CallOf(v, is_new);
            is_new = false;
        }
    }
    if (is_new) return DtsVal();  // `new dm.Circle.` is not an instance of anything
    return v;
}

std::string DtsHover(const DtsVal &v) {
    switch (v.k) {
        case DtsVal::Namespace:
            if (v.decl) return JsDtsDeclHover(*v.mod, *v.decl);
            return "```ts\nmodule\n```\n\n" + JsDtsModuleNote(*v.mod, *v.scope);
        case DtsVal::Class:
        case DtsVal::Instance:
        case DtsVal::Function:
        case DtsVal::Enum:
        case DtsVal::Variable:
            return JsDtsDeclHover(*v.mod, *v.decl);
        case DtsVal::Method:
        case DtsVal::Property:
        case DtsVal::EnumMember:
            return JsDtsMemberHover(*v.mod, *v.decl, *v.member);
        default:
            return std::string();
    }
}

using CompletionSink = std::function<void(const std::string &label, JsLspKind kind, const std::string &detail,
                                          const std::string &docs)>;

JsLspKind DtsDeclKind(const JsDtsDecl &d) {
    switch (d.kind) {
        case JsDtsDecl::Function:
            return JsLspKind::Function;
        case JsDtsDecl::Class:
            return JsLspKind::Class;
        case JsDtsDecl::Enum:
            return JsLspKind::Enum;
        case JsDtsDecl::Namespace:
            return JsLspKind::Module;
        case JsDtsDecl::Variable:
            return d.var_kind == "const" ? JsLspKind::Constant : JsLspKind::Variable;
        default:
            return JsLspKind::Interface;
    }
}

std::string DtsDeclDetail(const JsDtsDecl &d) {
    switch (d.kind) {
        case JsDtsDecl::Function:
            return d.sigs.empty() ? "function" : JsDtsSignatureLabel(d.sigs[0], d.name);
        case JsDtsDecl::Class:
            return std::string(d.abstract_ ? "abstract class " : "class ") + d.name +
                   (d.bases.empty() ? "" : " extends " + d.bases[0]);
        case JsDtsDecl::Enum:
            return "enum " + d.name;
        case JsDtsDecl::Namespace:
            return "namespace " + d.name;
        case JsDtsDecl::Variable:
            return d.type;
        default:
            return std::string();
    }
}

// The members a value has by its declarations, for completion after `.`.
void DtsMembers(const DtsVal &v0, const CompletionSink &add) {
    const DtsVal v = Resolved(v0);
    auto exports = [&](const JsDtsScope *sc) {
        if (!sc) return;
        for (const auto &kv : sc->Exports()) {
            const JsDtsDecl &d = *kv.second;
            if (d.kind == JsDtsDecl::Interface || d.kind == JsDtsDecl::TypeAlias) continue;  // types only
            add(kv.first, DtsDeclKind(d), DtsDeclDetail(d), d.doc);
        }
    };
    auto members = [&](const JsDtsDecl *cls, bool statics) {
        for (const auto &om : v.mod->Members(cls, statics)) {
            const JsDtsMember &mm = *om.second;
            if (mm.kind == JsDtsMember::Method) {
                std::string docs;
                for (const JsDtsSignature &s : mm.sigs) {
                    if (docs.empty()) docs = s.doc;
                }
                add(mm.name, JsLspKind::Method,
                    mm.sigs.empty() ? std::string() : JsDtsSignatureLabel(mm.sigs[0], mm.name), docs);
            } else {
                add(mm.name, JsLspKind::Property, mm.type, mm.doc);
            }
        }
    };
    switch (v.k) {
        case DtsVal::Namespace:
            exports(v.scope);
            break;
        case DtsVal::Class:
            members(v.decl, true);
            if (v.decl->ns) exports(v.decl->ns.get());
            break;
        case DtsVal::Instance:
            members(v.decl, false);
            break;
        case DtsVal::Function:
            if (v.decl->ns) exports(v.decl->ns.get());
            break;
        case DtsVal::Enum:
            for (const JsDtsMember &mm : v.decl->members) add(mm.name, JsLspKind::EnumMember, mm.type, mm.doc);
            if (v.decl->ns) exports(v.decl->ns.get());
            break;
        default:
            break;
    }
}

}  // namespace

// ---- hover -----------------------------------------------------------------------------------------------------

JsLspHoverInfo JsLspHover(const JsLspDocument &doc, int line, int col) {
    JsLspHoverInfo info;
    const uint32_t off = doc.Off(line, col);
    const Hit *h = doc.HitAt(off);
    if (!h) return info;
    info.range = doc.Range(h->start, h->end);
    if (h->var) {
        info.found = true;
        // A value a declaration file types: what it declares, with the file's own view.
        const DtsVal dv = VarVal(doc, h->var, 0);
        if (dv.k == DtsVal::None) {
            info.text = VarHover(doc, h->var);
        } else if (dv.k == DtsVal::Namespace && !dv.decl) {
            info.text = VarHover(doc, h->var) + "\n\n" + JsDtsModuleNote(*dv.mod, *dv.scope);
        } else if (dv.k == DtsVal::Instance) {
            info.text = VarHover(doc, h->var, dv.decl->name);
        } else if (h->var->kind == VarKind::Import || h->var->in_pattern) {
            info.text = DtsHover(dv);  // `import { add }`, `const { add } = require(...)`: the declaration itself
        } else {
            info.text = VarHover(doc, h->var) + "\n\n---\n\n" + DtsHover(dv);
        }
        return info;
    }
    if (h->ref) {
        const std::string name(h->node->str);
        if (const Api *a = FindApi(name)) {
            info.found = true;
            info.text = ApiHover(*a);
        } else if (IsHostGlobal(name)) {
            info.found = true;
            info.text = Fence(doc) + "(global) " + name + "\n```";
        } else {
            info.found = true;
            info.text = Fence(doc) + "(global) " + name + "\n```\n\nNot declared in this file.";
        }
        return info;
    }
    if (h->label) {
        info.found = true;
        info.text = Fence(doc) + "(label) " + std::string(h->node->str) + "\n```";
        return info;
    }
    if (h->member) {
        const Node *p = doc.Parent(h->node);
        const std::string name(h->node->str);
        if (p && p->type == NodeType::MemberExpression && p->property() == h->node) {
            for (const MemberDef &m : FileMembers(doc, p->object())) {
                if (m.name == name) {
                    info.found = true;
                    info.text = MemberHover(doc, m);
                    return info;
                }
            }
            const DtsVal dv = MemberOf(ExprVal(doc, p->object(), 0), name);
            if (dv.k != DtsVal::None) {
                info.found = true;
                info.text = DtsHover(dv);
                return info;
            }
            const std::vector<std::string> owners = OwnersOf(doc, p->object());
            const Api *a = owners.empty() ? GuessMemberApi(name) : FindInOwners(owners, name);
            if (a) {
                info.found = true;
                info.text = ApiHover(*a);
            }
            return info;
        }
        // `const { Circle: C } = require("x")`: the key is the module's export.
        if (p && p->type == NodeType::Property && !p->Has(jsparse::kFlagComputed) && p->key() == h->node) {
            const Node *pat = doc.Parent(p);
            const Node *vd = pat && pat->type == NodeType::ObjectPattern ? doc.Parent(pat) : nullptr;
            if (vd && vd->type == NodeType::VariableDeclarator && vd->id() == pat) {
                const DtsVal dv = MemberOf(ExprVal(doc, vd->init(), 0), name);
                if (dv.k != DtsVal::None) {
                    info.found = true;
                    info.text = DtsHover(dv);
                    return info;
                }
            }
        }
        if (p && (p->type == NodeType::Property || p->type == NodeType::MethodDefinition ||
                  p->type == NodeType::PropertyDefinition || p->type == NodeType::TSAbstractMethodDefinition ||
                  p->type == NodeType::TSAbstractPropertyDefinition || p->type == NodeType::AccessorProperty ||
                  p->type == NodeType::TSPropertySignature || p->type == NodeType::TSMethodSignature)) {
            MemberDef m{name, h->node, p->value(), p};
            info.found = true;
            info.text = MemberHover(doc, m);
        }
    }
    return info;
}

// ---- completion ------------------------------------------------------------------------------------------------

std::vector<JsLspCompletionItem> JsLspCompletions(const JsLspDocument &doc, int line, int col) {
    std::vector<JsLspCompletionItem> out;
    const uint32_t off = doc.Off(line, col);
    if (InCommentOrString(doc, off)) return out;
    const std::string &t = doc.text;
    uint32_t ps = off;
    while (ps > 0 && IsWordByte(t[ps - 1])) ps--;
    const std::string prefix = t.substr(ps, off - ps);
    int pl = 0, pc = 0;
    doc.Pos(ps, &pl, &pc);
    std::set<std::string> seen;
    auto add = [&](const std::string &label, JsLspKind kind, const std::string &detail, const std::string &docs) {
        if (label.empty() || !seen.insert(label).second) return;
        if (!prefix.empty()) {
            // Prefix match, case-insensitively; the client narrows further.
            if (label.size() < prefix.size()) return;
            for (size_t i = 0; i < prefix.size(); i++) {
                if (std::tolower(static_cast<unsigned char>(label[i])) !=
                    std::tolower(static_cast<unsigned char>(prefix[i])))
                    return;
            }
        }
        JsLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = docs;
        it.replace_start = pc;
        it.replace_end = col;
        out.push_back(it);
    };

    // -- after a dot: members --
    bool dot = ps > 0 && t[ps - 1] == '.' && !(ps > 1 && t[ps - 2] == '.');
    if (dot) {
        const uint32_t d = ps - 1;
        const uint32_t rs = ReceiverStart(t, d);
        const uint32_t re = (d > 0 && t[d - 1] == '?') ? d - 1 : d;
        std::vector<MemberDef> mems;
        std::vector<std::string> owners;
        const Node *obj = rs < re ? ExprAt(doc, rs, re) : nullptr;
        DtsVal dts;  // what the declaration files say the receiver is
        if (obj) {
            mems = FileMembers(doc, obj);
            owners = OwnersOf(doc, obj);
            dts = ExprVal(doc, obj, 0);
        } else if (rs < re) {
            uint32_t nk = rs;
            while (nk > 0 && (t[nk - 1] == ' ' || t[nk - 1] == '\t')) nk--;
            const bool after_new = nk >= 3 && t.compare(nk - 3, 3, "new") == 0 && (nk < 4 || !IsWordByte(t[nk - 4]));
            dts = DtsValOfText(doc, off, t.substr(rs, re - rs), after_new);
            // A half-typed `foo.` usually broke its statement out of the tree: go by the text.
            const std::string recv = t.substr(rs, re - rs);
            const char c0 = recv[0];
            if (recv == "this") {
                const Node *cls = nullptr;
                std::vector<const Node *> stack{doc.parse.program};
                std::vector<Node *> kids;
                while (!stack.empty()) {
                    const Node *n = stack.back();
                    stack.pop_back();
                    if (!(n->start <= off && off <= n->end)) continue;
                    if (n->type == NodeType::ClassDeclaration || n->type == NodeType::ClassExpression) cls = n;
                    jsparse::CollectChildren(n, kids);
                    for (const Node *k : kids) stack.push_back(k);
                }
                if (cls) {
                    // A `this` inside the class gathers its members and `this.x =` assignments.
                    const Node *any_this = nullptr;
                    stack.assign(1, cls);
                    while (!stack.empty() && !any_this) {
                        const Node *n = stack.back();
                        stack.pop_back();
                        if (n->type == NodeType::ThisExpression && EnclosingClass(doc, n) == cls) any_this = n;
                        jsparse::CollectChildren(n, kids);
                        for (const Node *k : kids) stack.push_back(k);
                    }
                    if (any_this)
                        mems = FileMembers(doc, any_this);
                    else
                        ClassMembers(cls, false, mems);
                }
            } else if (c0 == '\'' || c0 == '"' || c0 == '`') {
                owners = OwnersOfType("String");
            } else if (c0 == '[') {
                owners = OwnersOfType("Array");
            } else if (std::isdigit(static_cast<unsigned char>(c0))) {
                owners = OwnersOfType("Number");
            } else {
                // a, a.b, a.b(), a?.b: the root by scope, then each member by its declared type.
                std::vector<std::string> parts;
                std::vector<bool> called;
                {
                    size_t i = 0;
                    std::string cur;
                    bool call = false;
                    while (i < recv.size()) {
                        const char ch = recv[i];
                        if (ch == '(' || ch == '[') {
                            const char close = ch == '(' ? ')' : ']';
                            int depth = 0;
                            for (; i < recv.size(); i++) {
                                if (recv[i] == ch) depth++;
                                if (recv[i] == close && --depth == 0) break;
                            }
                            if (ch == '(') call = true;
                            else cur += "[]";
                            i++;
                            continue;
                        }
                        if (ch == '.' || ch == '?') {
                            if (ch == '.') {
                                parts.push_back(cur);
                                called.push_back(call);
                                cur.clear();
                                call = false;
                            }
                            i++;
                            continue;
                        }
                        cur += ch;
                        i++;
                    }
                    parts.push_back(cur);
                    called.push_back(call);
                }
                const std::string &root = parts[0];
                if (IsIdentifierName(root)) {
                    const Var *v = nullptr;
                    for (const Scope *s = doc.ScopeAt(off); s && !v; s = s->parent) v = s->Find(root);
                    if (called[0]) {
                        const Api *a = v ? nullptr : FindApi(root);
                        if (a) owners = OwnersOfType(ReturnTypeOf(a->signature));
                    } else if (v && !v->refs.empty()) {
                        mems = FileMembers(doc, v->refs[0]->id);
                        owners = OwnersOf(doc, v->refs[0]->id);
                    } else if (v && !v->ids.empty() && v->ids[0]->typeAnnotation()) {
                        // TypeScript: a declared type (`m: Map<K, V>`, `p: Point`).
                        mems = FileMembers(doc, v->ids[0]);
                        owners = OwnersOf(doc, v->ids[0]);
                    } else if (v) {
                        if (const Node *cls = ClassOfVar(v)) ClassMembers(cls, true, mems);
                        if (v->init) {
                            ObjectMembers(v->init, mems);
                            if (v->init->type == NodeType::NewExpression)
                                ClassMembers(ClassOfVar(VarOfExpr(doc, v->init->callee())), false, mems);
                            owners = OwnersOfType(InferType(doc, v->init));
                        }
                    } else {
                        if (!ApiMembers(root).empty()) owners.push_back(root);
                        const Api *a = FindApi(root);
                        if (a && (std::strcmp(a->kind, "variable") == 0 || std::strcmp(a->kind, "property") == 0)) {
                            for (const std::string &o : OwnersOfType(TypeNameOf(a->signature))) owners.push_back(o);
                        }
                    }
                }
                std::string direct = called[0] ? std::string() : root;
                for (size_t k = 1; k < parts.size() && (!owners.empty() || !mems.empty()); k++) {
                    const std::string &name = parts[k];
                    std::string type;
                    // The file's own member first (an object literal's nested object, a method).
                    const MemberDef *own = nullptr;
                    for (const MemberDef &m : mems) {
                        if (m.name == name) own = &m;
                    }
                    std::vector<MemberDef> next_mems;
                    if (own && own->value) {
                        ObjectMembers(own->value, next_mems);
                        type = InferType(doc, own->value);
                    } else if (const Api *a = FindInOwners(owners, name)) {
                        const std::string_view kind = a->kind;
                        type = called[k] ? ReturnTypeOf(a->signature)
                               : (kind == "property" || kind == "variable") ? TypeNameOf(a->signature)
                                                                            : std::string();
                    }
                    owners.clear();
                    if (!direct.empty() && !called[k] && !ApiMembers(direct + "." + name).empty())
                        owners.push_back(direct + "." + name);
                    direct = direct.empty() || called[k] ? std::string() : direct + "." + name;
                    for (const std::string &o : OwnersOfType(type)) owners.push_back(o);
                    mems = next_mems;
                }
            }
        }
        for (const MemberDef &m : mems)
            add(m.name, IsFunction(m.value) ? JsLspKind::Method : JsLspKind::Property, "", "");
        DtsMembers(dts, add);
        for (const std::string &o : owners) {
            for (const Api *a : ApiMembers(o))
                add(ApiShortName(*a), CompletionKindOfApi(*a), FirstSigLine(a->signature), a->doc);
        }
        // Whatever the receiver: the member names this file uses elsewhere.
        for (const Hit &h : doc.hits) {
            if (h.member && h.node->type == NodeType::Identifier && !(h.start <= off && off <= h.end))
                add(std::string(h.node->str), JsLspKind::Field, "", "");
        }
        return out;
    }

    // -- names in scope, innermost first --
    // TypeScript: a type position (`x: |`, `as |`, `Array<|`) wants types, anywhere else values.
    const bool type_pos = InTypePosition(doc, ps);
    if (type_pos) {
        for (const Scope *s = doc.ScopeAt(off); s; s = s->parent) {
            for (const Var *v : s->vars) {
                if (!(v->ns & kNsType) || v->ids.empty()) continue;
                if (v->ids[0]->start <= off && off <= v->ids[0]->end) continue;
                add(std::string(v->name), v->kind == VarKind::TypeParam ? JsLspKind::Value
                                          : v->kind == VarKind::Class    ? JsLspKind::Class
                                          : v->kind == VarKind::Enum     ? JsLspKind::Struct
                                          : v->kind == VarKind::Namespace || v->kind == VarKind::Import ? JsLspKind::Module
                                                                         : JsLspKind::Interface,
                    v->kind == VarKind::TypeParam ? "type parameter"
                    : v->kind == VarKind::TypeAlias ? "type"
                    : v->kind == VarKind::Interface ? "interface"
                    : v->kind == VarKind::Enum      ? "enum"
                    : v->kind == VarKind::Import    ? "import"
                    : v->kind == VarKind::Namespace ? "namespace"
                                                    : "class",
                    "");
            }
        }
        for (const char *k : {"string", "number", "boolean", "bigint", "symbol", "object", "unknown", "any", "never",
                              "void", "undefined", "null", "keyof", "typeof", "readonly", "infer", "unique", "asserts",
                              "is", "extends"})
            add(k, JsLspKind::Keyword, "", "");
        for (const Api *a : ApiGlobals()) {
            const std::string_view kind = a->kind;
            if (kind != "interface" && kind != "type" && kind != "class") continue;
            if (!prefix.empty() || std::strcmp(a->origin, "es") == 0)
                add(a->name, kind == "class" ? JsLspKind::Class : JsLspKind::Interface, FirstSigLine(a->signature), a->doc);
        }
        return out;
    }
    for (const Scope *s = doc.ScopeAt(off); s; s = s->parent) {
        for (const Var *v : s->vars) {
            // A let/const below the cursor in the same block is not usable yet; a var/function is.
            if (!v->ids.empty() && v->ids[0]->start <= off && off <= v->ids[0]->end) continue;
            if (!(v->ns & kNsValue)) continue;  // an interface, a type alias, a type parameter
            std::string detail;
            if (v->kind == VarKind::Param) detail = "parameter";
            else if (v->kind == VarKind::Import) detail = "import";
            else if (v->kind == VarKind::Function && v->decl) detail = FunctionHead(doc, v->decl, std::string(v->name));
            else if (v->ids.empty()) detail = "";
            else detail = KindWord(v->kind);
            add(std::string(v->name), CompletionKindOfVar(v), detail, "");
        }
    }
    for (std::string_view g : doc.implicit_globals) add(std::string(g), JsLspKind::Variable, "global", "");
    for (const char *k : kKeywords) add(k, JsLspKind::Keyword, "", "");
    if (!prefix.empty()) {
        for (const Api *a : ApiGlobals()) {
            const std::string_view kind = a->kind;
            if (kind == "interface" || kind == "type") continue;
            add(a->name, CompletionKindOfApi(*a), FirstSigLine(a->signature), a->doc);
        }
    }
    // Document words.
    if (!prefix.empty()) {
        size_t i = 0;
        while (i < t.size()) {
            if (!IsWordByte(t[i]) || (i > 0 && IsWordByte(t[i - 1]))) {
                i++;
                continue;
            }
            size_t j = i;
            while (j < t.size() && IsWordByte(t[j])) j++;
            if (j - i >= 3 && !(i <= off && off <= j) && !std::isdigit(static_cast<unsigned char>(t[i])))
                add(t.substr(i, j - i), JsLspKind::Text, "", "");
            i = j;
        }
    }
    return out;
}

// ---- signature help --------------------------------------------------------------------------------------------

namespace {

// Splits "(a: T, b?: U): R" (or "(a, b)") into the parameter substrings and their offsets.
void SplitParams(const std::string &label, size_t open, std::vector<std::string> *params) {
    int depth = 0;
    size_t start = open + 1;
    for (size_t i = open; i < label.size(); i++) {
        const char c = label[i];
        if (c == '(' || c == '[' || c == '{' || c == '<') depth++;
        if (c == ')' || c == ']' || c == '}' || (c == '>' && i > 0 && label[i - 1] != '=')) {
            depth--;
            if (depth == 0 && c == ')') {
                std::string p = label.substr(start, i - start);
                while (!p.empty() && p[0] == ' ') p.erase(0, 1);
                if (!p.empty()) params->push_back(p);
                return;
            }
        }
        if (c == ',' && depth == 1) {
            std::string p = label.substr(start, i - start);
            while (!p.empty() && p[0] == ' ') p.erase(0, 1);
            params->push_back(p);
            start = i + 1;
        }
    }
}

size_t ParamsOpen(const std::string &label, size_t from) {
    // Skip a leading generic list "<T>(...)".
    size_t i = from;
    if (i < label.size() && label[i] == '<') {
        int d = 0;
        for (; i < label.size(); i++) {
            if (label[i] == '<') d++;
            if (label[i] == '>' && label[i - 1] != '=' && --d == 0) {
                i++;
                break;
            }
        }
    }
    return i < label.size() && label[i] == '(' ? i : std::string::npos;
}

}  // namespace

JsLspSignatureHelp JsLspSignatureAt(const JsLspDocument &doc, int line, int col) {
    JsLspSignatureHelp sh;
    const uint32_t off = doc.Off(line, col);
    if (InCommentOrString(doc, off)) return sh;
    const std::string &t = doc.text;
    // Back to the unmatched '(' of the call, counting commas at its depth.
    int depth = 0, commas = 0;
    uint32_t i = off;
    uint32_t open = UINT32_MAX;
    const uint32_t limit = off > 20000 ? off - 20000 : 0;
    while (i > limit) {
        const char c = t[i - 1];
        if (c == ')' || c == ']' || c == '}') depth++;
        else if (c == '(' || c == '[' || c == '{') {
            if (depth == 0) {
                if (c == '(') open = i - 1;
                break;
            }
            depth--;
        } else if (c == ',' && depth == 0) {
            commas++;
        } else if (c == ';' && depth == 0) {
            break;
        } else if (c == '"' || c == '\'' || c == '`') {
            // Skip a string backwards.
            uint32_t j = i - 1;
            while (j > limit && !(t[j - 1] == c && (j < 2 || t[j - 2] != '\\'))) j--;
            if (j > limit) i = j;
        }
        i--;
    }
    if (open == UINT32_MAX) return sh;
    uint32_t ce = open;
    while (ce > 0 && (t[ce - 1] == ' ' || t[ce - 1] == '\t')) ce--;
    const uint32_t cs = ReceiverStart(t, ce);
    if (cs >= ce) return sh;
    const std::string callee = t.substr(cs, ce - cs);
    bool is_new = false;
    {
        uint32_t k = cs;
        while (k > 0 && (t[k - 1] == ' ')) k--;
        is_new = k >= 3 && t.compare(k - 3, 3, "new") == 0 && (k < 4 || !IsWordByte(t[k - 4]));
    }
    // The last name of the callee chain.
    const size_t dotp = callee.rfind('.');
    const std::string last = dotp == std::string::npos ? callee : callee.substr(dotp + 1);
    if (!IsIdentifierName(last) || IsReservedWord(last)) {
        if (last != "eval") return sh;
    }
    const Scope *scope = doc.ScopeAt(open);
    auto fill_from_function = [&](const Node *fn, const std::string &name, const std::string &jsdoc_of) {
        sh.found = true;
        sh.label = name + "(";
        for (uint32_t k = 0; k < fn->params().size(); k++) {
            const Node *p = fn->params()[k];
            if (!p) continue;
            const std::string ptxt = OneLine(doc.Slice(p->start, p->end), 40);
            if (k) sh.label += ", ";
            sh.params.push_back(ptxt);
            sh.label += ptxt;
        }
        sh.label += ")";
        (void)jsdoc_of;
        sh.documentation = doc.DocCommentFor(fn);
        int n = static_cast<int>(sh.params.size());
        sh.active_param = commas;
        if (n > 0 && sh.params.back().rfind("...", 0) == 0 && sh.active_param >= n) sh.active_param = n - 1;
    };
    auto fill_from_api = [&](const Api &a, const std::string &sig_line) {
        const size_t po = ParamsOpen(sig_line, 0);
        if (po == std::string::npos) return false;
        sh.found = true;
        sh.label = ApiShortName(a) + sig_line;
        SplitParams(sh.label, ApiShortName(a).size() + po, &sh.params);
        sh.documentation = a.doc;
        const int n = static_cast<int>(sh.params.size());
        sh.active_param = commas;
        if (n > 0 && sh.params.back().rfind("...", 0) == 0 && sh.active_param >= n) sh.active_param = n - 1;
        return true;
    };
    auto pick_overload = [&](const Api &a) {
        std::string sig = a.signature;
        if (std::strcmp(a.kind, "class") == 0) {
            // "{ new (x: T): C; ... }" -> "(x: T): C"
            const size_t at = sig.find("new (");
            if (at == std::string::npos) return false;
            sig = sig.substr(at + 4);
            int d = 0;
            size_t e = 0;
            for (; e < sig.size(); e++) {
                if (sig[e] == '(') d++;
                if (sig[e] == ')' && --d == 0) break;
            }
            const size_t semi = sig.find(';', e);
            sig = sig.substr(0, semi);
        }
        // The first overload with enough parameters for the cursor.
        std::vector<std::string> lines;
        size_t s = 0;
        while (s <= sig.size()) {
            size_t nl = sig.find('\n', s);
            if (nl == std::string::npos) nl = sig.size();
            lines.push_back(sig.substr(s, nl - s));
            s = nl + 1;
        }
        for (const std::string &l : lines) {
            std::vector<std::string> ps;
            const size_t po = ParamsOpen(l, 0);
            if (po == std::string::npos) continue;
            SplitParams(l, po, &ps);
            const bool rest = !ps.empty() && ps.back().rfind("...", 0) == 0;
            if (static_cast<int>(ps.size()) > commas || rest) return fill_from_api(a, l);
        }
        return !lines.empty() && fill_from_api(a, lines[0]);
    };

    // What a declaration file declares: a module's function, a class's constructors, a method --
    // every overload, the first that can take the argument at the cursor active.
    {
        DtsVal target;
        if (dotp == std::string::npos) {
            const Var *v = nullptr;
            for (const Scope *s = scope; s && !v; s = s->parent) v = s->Find(callee);
            target = VarVal(doc, v, 0);
        } else {
            const std::string recv = callee.substr(0, dotp);
            const Node *obj = ExprAt(doc, cs, cs + static_cast<uint32_t>(recv.size()));
            target = MemberOf(obj ? ExprVal(doc, obj, 0) : DtsValOfText(doc, open, recv), last);
        }
        target = Resolved(target);
        const std::vector<JsDtsSignature> *sigs = nullptr;
        std::string name, fallback_doc;
        if (target.k == DtsVal::Function) {
            sigs = &target.decl->sigs;
            name = target.decl->name;
            fallback_doc = target.decl->doc;
        } else if (target.k == DtsVal::Method) {
            sigs = &target.member->sigs;
            name = target.member->name;
            for (const JsDtsSignature &sg : *sigs) {
                if (fallback_doc.empty()) fallback_doc = sg.doc;
            }
        } else if (target.k == DtsVal::Class) {
            sigs = &target.decl->sigs;
            name = target.decl->name;
            fallback_doc = target.decl->doc;
        }
        if (sigs) {
            static const std::vector<JsDtsSignature> kNoArgs{JsDtsSignature{"", "()", "", ""}};
            if (sigs->empty()) sigs = &kNoArgs;
            sh.found = true;
            int active = -1;
            for (size_t k = 0; k < sigs->size(); k++) {
                const JsDtsSignature &sg = (*sigs)[k];
                JsLspSignatureHelp::Overload o;
                o.label = JsDtsSignatureLabel(sg, name);
                const size_t po = ParamsOpen(o.label, name.size());
                if (po != std::string::npos) SplitParams(o.label, po, &o.params);
                o.documentation = sg.doc;
                if (target.k == DtsVal::Class && !target.decl->doc.empty())
                    o.documentation = target.decl->doc + (sg.doc.empty() ? "" : "\n\n" + sg.doc);
                else if (o.documentation.empty())
                    o.documentation = fallback_doc;
                const bool rest = !o.params.empty() && o.params.back().rfind("...", 0) == 0;
                if (active < 0 && (static_cast<int>(o.params.size()) > commas || rest || (commas == 0 && o.params.empty())))
                    active = static_cast<int>(k);
                sh.overloads.push_back(o);
            }
            if (active < 0) active = 0;
            sh.active_signature = active;
            const JsLspSignatureHelp::Overload &a = sh.overloads[static_cast<size_t>(active)];
            sh.label = a.label;
            sh.params = a.params;
            sh.documentation = a.documentation;
            const int n = static_cast<int>(sh.params.size());
            sh.active_param = commas;
            if (n > 0 && sh.params.back().rfind("...", 0) == 0 && sh.active_param >= n) sh.active_param = n - 1;
            if (sh.overloads.size() == 1) sh.overloads.clear();
            return sh;
        }
    }

    if (dotp == std::string::npos) {
        // A plain name: local function, local class, else the vocabulary.
        const Var *v = nullptr;
        for (const Scope *s = scope; s && !v; s = s->parent) v = s->Find(callee);
        if (v) {
            const Node *fn = nullptr;
            if ((v->kind == VarKind::Function || v->kind == VarKind::FunctionName) && v->decl) fn = v->decl;
            else if (v->init && IsFunction(v->init)) fn = v->init;
            const Node *cls = ClassOfVar(v);
            if (cls && cls->body()) {
                for (const Node *m : cls->body()->body_list()) {
                    if (m && m->type == NodeType::MethodDefinition && m->kind == "constructor") fn = m->value();
                }
                if (!fn) {
                    sh.found = true;
                    sh.label = callee + "()";
                    return sh;
                }
            }
            if (fn) fill_from_function(fn, callee, "");
            return sh;
        }
        if (const Api *a = FindApi(callee)) pick_overload(*a);
        (void)is_new;
        return sh;
    }
    // obj.method(: the file's members first, then the vocabulary.
    const std::string recv = callee.substr(0, dotp);
    const Node *obj = ExprAt(doc, cs, cs + static_cast<uint32_t>(recv.size()));
    if (obj) {
        for (const MemberDef &m : FileMembers(doc, obj)) {
            if (m.name == last && IsFunction(m.value)) {
                fill_from_function(m.value, last, "");
                return sh;
            }
        }
        if (const Api *a = FindInOwners(OwnersOf(doc, obj), last)) {
            pick_overload(*a);
            return sh;
        }
    }
    if (const Api *a = FindApi(recv + "." + last)) {
        pick_overload(*a);
        return sh;
    }
    if (const Api *a = GuessMemberApi(last)) pick_overload(*a);
    return sh;
}

// ---- navigation ------------------------------------------------------------------------------------------------

namespace {

const Node *LabelTarget(const JsLspDocument &doc, const Node *label) {
    for (const Node *p = doc.Parent(label); p; p = doc.Parent(p)) {
        if (p->type == NodeType::LabeledStatement && p->label() && p->label()->str == label->str) return p->label();
        if (IsFunction(p)) break;
    }
    return nullptr;
}

std::vector<const Node *> LabelUses(const JsLspDocument &doc, const Node *decl) {
    std::vector<const Node *> out{decl};
    for (const Hit &h : doc.hits) {
        if (h.label && h.node != decl && LabelTarget(doc, h.node) == decl) out.push_back(h.node);
    }
    return out;
}

}  // namespace

std::vector<JsLspLocation> JsLspDefinition(const JsLspDocument &doc, int line, int col) {
    std::vector<JsLspLocation> out;
    const Hit *h = doc.HitAt(doc.Off(line, col));
    if (!h) return out;
    if (h->var) {
        if (!h->var->ids.empty()) out.push_back({doc.Range(h->var->ids[0]->start, h->var->ids[0]->end)});
        return out;
    }
    if (h->ref) {
        // A global the file assigns somewhere: the first assignment.
        for (const Ref *r : doc.through) {
            if (r->write && r->id->str == h->node->str) {
                out.push_back({doc.Range(r->id->start, r->id->end)});
                break;
            }
        }
        return out;
    }
    if (h->label) {
        if (const Node *t = LabelTarget(doc, h->node)) out.push_back({doc.Range(t->start, t->end)});
        return out;
    }
    if (h->member) {
        const Node *p = doc.Parent(h->node);
        if (p && p->type == NodeType::MemberExpression && p->property() == h->node) {
            for (const MemberDef &m : FileMembers(doc, p->object())) {
                if (m.name == h->node->str && m.key) {
                    out.push_back({doc.Range(m.key->start, m.key->end)});
                    break;
                }
            }
        }
    }
    return out;
}

std::vector<JsLspReference> JsLspReferences(const JsLspDocument &doc, int line, int col) {
    std::vector<JsLspReference> out;
    const Hit *h = doc.HitAt(doc.Off(line, col));
    if (!h) return out;
    if (h->var) {
        for (const Node *id : h->var->ids) out.push_back({doc.Range(id->start, id->end), true, true});
        for (const Ref *r : h->var->refs) out.push_back({doc.Range(r->id->start, r->id->end), false, r->write});
    } else if (h->ref) {
        for (const Ref *r : doc.through) {
            if (r->id->str == h->node->str) out.push_back({doc.Range(r->id->start, r->id->end), false, r->write});
        }
    } else if (h->label) {
        const Node *decl = doc.Parent(h->node) && doc.Parent(h->node)->type == NodeType::LabeledStatement
                               ? h->node
                               : LabelTarget(doc, h->node);
        if (decl) {
            for (const Node *n : LabelUses(doc, decl))
                out.push_back({doc.Range(n->start, n->end), n == decl, false});
        }
    } else if (h->member) {
        // Members are not typed: every use of the same property name in the file.
        for (const Hit &o : doc.hits) {
            if (o.member && o.node->str == h->node->str) {
                const Node *p = doc.Parent(o.node);
                const bool def = p && p->type != NodeType::MemberExpression;
                bool write = false;
                if (p && p->type == NodeType::MemberExpression) {
                    const Node *pp = doc.Parent(p);
                    write = pp && pp->type == NodeType::AssignmentExpression && pp->left() == p;
                }
                out.push_back({doc.Range(o.start, o.end), def, write || def});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const JsLspReference &a, const JsLspReference &b) {
        return a.range.line != b.range.line ? a.range.line < b.range.line : a.range.col < b.range.col;
    });
    return out;
}

// ---- rename ----------------------------------------------------------------------------------------------------

JsLspRenameTarget JsLspPrepareRename(const JsLspDocument &doc, int line, int col) {
    JsLspRenameTarget t;
    const Hit *h = doc.HitAt(doc.Off(line, col));
    if (!h) return t;
    if ((h->var && h->var->kind != VarKind::Arguments && !h->var->ids.empty()) || h->label) {
        t.found = true;
        t.range = doc.Range(h->start, h->end);
        t.placeholder = std::string(h->node->str);
    }
    return t;
}

namespace {

bool IsAncestorOrSelf(const Scope *anc, const Scope *s) {
    for (; s; s = s->parent) {
        if (s == anc) return true;
    }
    return false;
}

}  // namespace

std::vector<JsLspTextEdit> JsLspRename(const JsLspDocument &doc, int line, int col, const std::string &new_name,
                                       std::string *error) {
    std::vector<JsLspTextEdit> edits;
    const Hit *h = doc.HitAt(doc.Off(line, col));
    if (!h || (!h->var && !h->label)) {
        *error = "nothing to rename here (only variables, functions, classes, parameters and labels)";
        return edits;
    }
    if (!IsIdentifierName(new_name) || IsReservedWord(new_name)) {
        *error = "'" + new_name + "' is not a valid JavaScript name";
        return edits;
    }
    if (h->label) {
        const Node *decl = doc.Parent(h->node) && doc.Parent(h->node)->type == NodeType::LabeledStatement
                               ? h->node
                               : LabelTarget(doc, h->node);
        if (!decl) {
            *error = "no label to rename";
            return edits;
        }
        for (const Node *n : LabelUses(doc, decl)) edits.push_back({doc.Range(n->start, n->end), new_name});
        return edits;
    }
    const Var *v = h->var;
    if (v->kind == VarKind::Arguments || v->ids.empty()) {
        *error = "'arguments' cannot be renamed";
        return edits;
    }
    if (new_name == v->name) return edits;
    // 1. Something of that name in the same scope.
    if (const Var *o = v->scope->Find(new_name)) {
        *error = "'" + new_name + "' is already declared in this scope" +
                 (o->ids.empty() ? std::string() : " (line " + std::to_string(doc.LineOf(o->ids[0]->start) + 1) + ")");
        return edits;
    }
    // 2. A use of v that would resolve to a nearer declaration of the new name.
    for (const Ref *r : v->refs) {
        for (const Scope *s = r->from; s && s != v->scope; s = s->parent) {
            if (const Var *o = s->Find(new_name)) {
                *error = "'" + new_name + "' would be captured by the declaration" +
                         (o->ids.empty() ? std::string(" of 'arguments'")
                                         : " on line " + std::to_string(doc.LineOf(o->ids[0]->start) + 1));
                return edits;
            }
        }
    }
    // 3. A use of an outer (or global) `new_name` inside v's scope would now find v.
    for (const auto &up : doc.refs) {
        const Ref *r = up.get();
        if (r->id->str != new_name) continue;
        const bool outer = !r->var || (r->var->scope != v->scope && IsAncestorOrSelf(r->var->scope, v->scope));
        if (outer && IsAncestorOrSelf(v->scope, r->from)) {
            *error = "renaming would hide the outer '" + new_name + "' used on line " +
                     std::to_string(doc.LineOf(r->id->start) + 1);
            return edits;
        }
    }
    const std::string old(v->name);
    for (const Node *id : v->ids) {
        const Node *p = doc.Parent(id);
        std::string text = new_name;
        if (p && p->type == NodeType::Property && p->Has(jsparse::kFlagShorthand)) text = old + ": " + new_name;
        if (p && p->type == NodeType::AssignmentPattern) {
            const Node *pp = doc.Parent(p);
            if (pp && pp->type == NodeType::Property && pp->Has(jsparse::kFlagShorthand)) text = old + ": " + new_name;
        }
        if (p && p->type == NodeType::ImportSpecifier && p->imported() && p->imported()->start == id->start)
            text = old + " as " + new_name;
        edits.push_back({doc.Range(id->start, id->end), text});
    }
    for (const Ref *r : v->refs) {
        std::string text = new_name;
        if (r->shorthand) text = old + ": " + new_name;
        if (r->export_ref) {
            const Node *p = doc.Parent(r->id);
            if (p && p->exported() && p->exported()->start == r->id->start) text = new_name + " as " + old;
        }
        edits.push_back({doc.Range(r->id->start, r->id->end), text});
    }
    std::sort(edits.begin(), edits.end(), [](const JsLspTextEdit &a, const JsLspTextEdit &b) {
        return a.range.line != b.range.line ? a.range.line < b.range.line : a.range.col < b.range.col;
    });
    edits.erase(std::unique(edits.begin(), edits.end(),
                            [](const JsLspTextEdit &a, const JsLspTextEdit &b) {
                                return a.range.line == b.range.line && a.range.col == b.range.col;
                            }),
                edits.end());
    return edits;
}

// ---- outline ---------------------------------------------------------------------------------------------------

namespace {

class Outline {
public:
    explicit Outline(const JsLspDocument &doc) : doc_(doc) {}

    std::vector<JsLspSymbol> Run() {
        if (doc_.parse.program) Statements(doc_.parse.program->body_list(), -1, 0);
        return out_;
    }

private:
    const JsLspDocument &doc_;
    std::vector<JsLspSymbol> out_;

    int Add(const std::string &name, const std::string &detail, JsLspSymbolKind kind, const Node *whole,
            const Node *sel, int parent) {
        JsLspSymbol s;
        s.name = name;
        s.detail = detail;
        s.kind = kind;
        s.range = doc_.Range(whole->start, whole->end);
        s.sel_range = doc_.Range(sel->start, sel->end);
        s.parent = parent;
        out_.push_back(s);
        return static_cast<int>(out_.size()) - 1;
    }

    void Statements(const jsparse::NodeList &list, int parent, int depth) {
        // TypeScript: overload signatures are listed by their implementation (or once, if none).
        std::set<std::string_view> fns;
        for (const Node *s : list) {
            const Node *d = s && (s->type == NodeType::ExportNamedDeclaration ||
                                  s->type == NodeType::ExportDefaultDeclaration)
                                ? s->declaration()
                                : s;
            if (d && d->type == NodeType::FunctionDeclaration && d->id()) fns.insert(d->id()->str);
        }
        std::set<std::string_view> sigs;
        for (const Node *s : list) {
            const Node *d = s && (s->type == NodeType::ExportNamedDeclaration ||
                                  s->type == NodeType::ExportDefaultDeclaration)
                                ? s->declaration()
                                : s;
            if (d && d->type == NodeType::TSDeclareFunction && d->id() &&
                (fns.count(d->id()->str) || !sigs.insert(d->id()->str).second))
                continue;
            Statement(s, parent, depth);
        }
    }

    // TypeScript: an interface's or type literal's members.
    void TypeMembersOutline(const jsparse::NodeList &list, int parent) {
        for (const Node *m : list) {
            if (!m || !m->key() || (m->type != NodeType::TSPropertySignature && m->type != NodeType::TSMethodSignature))
                continue;
            const Node *k = m->key();
            std::string kn = m->Has(jsparse::kFlagComputed) ? "[" + OneLine(doc_.Slice(k->start, k->end), 30) + "]"
                             : k->type == NodeType::Literal ? std::string(k->str2.empty() ? k->str : k->str2)
                                                            : std::string(k->str);
            if (m->type == NodeType::TSMethodSignature)
                Add(kn, ParamsText(doc_, m) + ReturnText(doc_, m), JsLspSymbolKind::Method, m, k, parent);
            else
                Add(kn, AnnotationText(doc_, m), JsLspSymbolKind::Property, m, k, parent);
        }
    }

    void TsDeclaration(const Node *s, int parent, int depth) {
        switch (s->type) {
            case NodeType::TSInterfaceDeclaration: {
                if (!s->id()) return;
                const int me = Add(std::string(s->id()->str), TypeParamsText(doc_, s), JsLspSymbolKind::Interface, s,
                                   s->id(), parent);
                if (s->body()) TypeMembersOutline(s->body()->body_list(), me);
                return;
            }
            case NodeType::TSTypeAliasDeclaration: {
                if (!s->id()) return;
                const int me = Add(std::string(s->id()->str), TypeParamsText(doc_, s), JsLspSymbolKind::Struct, s,
                                   s->id(), parent);
                const Node *t = s->typeAnnotation();
                if (t && t->type == NodeType::TSTypeLiteral) TypeMembersOutline(t->members(), me);
                return;
            }
            case NodeType::TSEnumDeclaration: {
                if (!s->id()) return;
                const int me = Add(std::string(s->id()->str), s->HasTs(jsparse::kTsConst) ? "const" : "",
                                   JsLspSymbolKind::Enum, s, s->id(), parent);
                const jsparse::NodeList &members = s->body() ? s->body()->members() : s->members();
                for (const Node *m : members) {
                    if (!m || !m->id()) continue;
                    const Node *k = m->id();
                    std::string kn = k->type == NodeType::Literal ? std::string(k->str2) : std::string(k->str);
                    Add(kn, m->initializer() ? OneLine(doc_.Slice(m->initializer()->start, m->initializer()->end), 30)
                                             : "",
                        JsLspSymbolKind::EnumMember, m, k, me);
                }
                return;
            }
            case NodeType::TSModuleDeclaration: {
                const Node *id = s->id();
                if (!id) return;
                std::string name = id->type == NodeType::Literal ? doc_.Slice(id->start, id->end)
                                                                 : OneLine(doc_.Slice(id->start, id->end), 60);
                const bool global = s->kind == "global";
                const int me = Add(name, global ? "global" : s->kind == "module" ? "module" : "namespace",
                                   id->type == NodeType::Literal || global ? JsLspSymbolKind::Module
                                                                           : JsLspSymbolKind::Namespace,
                                   s, id, parent);
                const Node *b = s->body();
                while (b && b->type == NodeType::TSModuleDeclaration) b = b->body();
                if (b && b->type == NodeType::TSModuleBlock && depth < 8) Statements(b->body_list(), me, 0);
                return;
            }
            case NodeType::TSDeclareFunction:
                if (s->id())
                    Add(std::string(s->id()->str), TypeParamsText(doc_, s) + ParamsText(doc_, s) + ReturnText(doc_, s),
                        JsLspSymbolKind::Function, s, s->id(), parent);
                return;
            default:
                return;
        }
    }

    void FunctionBody(const Node *fn, int parent, int depth) {
        if (depth > 8 || !fn) return;
        const Node *b = fn->body();
        if (b && b->type == NodeType::BlockStatement) Statements(b->body_list(), parent, depth + 1);
    }

    void Value(const std::string &name, const Node *whole, const Node *sel, const Node *v, bool constant, int parent,
               int depth) {
        if (v && IsFunction(v)) {
            const int me = Add(name, ParamsText(doc_, v), JsLspSymbolKind::Function, whole, sel, parent);
            FunctionBody(v, me, depth);
        } else if (v && v->type == NodeType::ClassExpression) {
            Class(v, name, whole, sel, parent, depth);
        } else if (v && v->type == NodeType::ObjectExpression && depth < 3) {
            const int me = Add(name, "", constant ? JsLspSymbolKind::Constant : JsLspSymbolKind::Variable, whole,
                               sel, parent);
            for (const Node *p : v->properties()) {
                if (!p || p->type != NodeType::Property || p->Has(jsparse::kFlagComputed) || !p->key()) continue;
                const Node *k = p->key();
                std::string kn = k->type == NodeType::Identifier ? std::string(k->str)
                                 : k->type == NodeType::Literal  ? std::string(k->str2.empty() ? k->str : k->str2)
                                                                 : std::string();
                if (kn.empty()) continue;
                if (IsFunction(p->value())) {
                    const int m = Add(kn, ParamsText(doc_, p->value()), JsLspSymbolKind::Method, p, k, me);
                    FunctionBody(p->value(), m, depth + 1);
                } else if (p->value() && p->value()->type == NodeType::ObjectExpression) {
                    Value(kn, p, k, p->value(), false, me, depth + 1);
                } else {
                    Add(kn, "", JsLspSymbolKind::Property, p, k, me);
                }
            }
        } else {
            Add(name, "", constant ? JsLspSymbolKind::Constant : JsLspSymbolKind::Variable, whole, sel, parent);
        }
    }

    void Class(const Node *cls, const std::string &name, const Node *whole, const Node *sel, int parent, int depth) {
        std::string detail;
        if (cls->superClass()) detail = "extends " + OneLine(doc_.Slice(cls->superClass()->start, cls->superClass()->end), 40);
        const int me = Add(name, detail, JsLspSymbolKind::Class, whole, sel, parent);
        if (!cls->body()) return;
        for (const Node *m : cls->body()->body_list()) {
            if (!m || !m->key()) continue;
            const Node *k = m->key();
            std::string kn = m->Has(jsparse::kFlagComputed) ? "[" + OneLine(doc_.Slice(k->start, k->end), 30) + "]"
                             : k->type == NodeType::PrivateIdentifier ? "#" + std::string(k->str)
                             : k->type == NodeType::Literal           ? std::string(k->str2.empty() ? k->str : k->str2)
                                                                      : std::string(k->str);
            if (m->Has(jsparse::kFlagStatic)) kn = "static " + kn;
            // TypeScript: an overload signature is listed by its implementation.
            if (m->value() && m->value()->type == NodeType::TSEmptyBodyFunctionExpression) {
                bool has_impl = false;
                for (const Node *o : cls->body()->body_list()) {
                    if (o && o != m && o->type == NodeType::MethodDefinition && o->key() && o->value() &&
                        o->value()->type != NodeType::TSEmptyBodyFunctionExpression && o->key()->str == k->str)
                        has_impl = true;
                }
                if (has_impl) continue;
            }
            if (m->type == NodeType::MethodDefinition && m->kind == "constructor" && m->value()) {
                // `constructor(private x: T)`: the parameter properties are fields.
                for (const Node *pp : m->value()->params()) {
                    if (!pp || pp->type != NodeType::TSParameterProperty) continue;
                    const Node *id = pp->parameter();
                    if (id && id->type == NodeType::AssignmentPattern) id = id->left();
                    if (id && id->type == NodeType::Identifier)
                        Add(std::string(id->str), AnnotationText(doc_, id), JsLspSymbolKind::Field, pp, id, me);
                }
            }
            if (m->type == NodeType::TSAbstractMethodDefinition) {
                Add(kn, ParamsText(doc_, m->value()) + ReturnText(doc_, m->value()), JsLspSymbolKind::Method, m, k, me);
                continue;
            }
            if (m->type == NodeType::TSAbstractPropertyDefinition || m->type == NodeType::AccessorProperty ||
                m->type == NodeType::TSAbstractAccessorProperty) {
                Add(kn, AnnotationText(doc_, m), JsLspSymbolKind::Field, m, k, me);
                continue;
            }
            if (m->type == NodeType::MethodDefinition) {
                JsLspSymbolKind kind = m->kind == "constructor" ? JsLspSymbolKind::Constructor
                                       : (m->kind == "get" || m->kind == "set") ? JsLspSymbolKind::Property
                                                                               : JsLspSymbolKind::Method;
                std::string d = m->kind == "get"   ? "get"
                                : m->kind == "set" ? "set"
                                                   : ParamsText(doc_, m->value()) + ReturnText(doc_, m->value());
                const int mm = Add(kn, d, kind, m, k, me);
                FunctionBody(m->value(), mm, depth + 1);
            } else if (m->type == NodeType::PropertyDefinition) {
                if (IsFunction(m->value())) {
                    const int mm = Add(kn, ParamsText(doc_, m->value()), JsLspSymbolKind::Method, m, k, me);
                    FunctionBody(m->value(), mm, depth + 1);
                } else {
                    Add(kn, AnnotationText(doc_, m), JsLspSymbolKind::Field, m, k, me);
                }
            }
        }
    }

    static std::string Chain(const Node *n) {
        if (!n) return std::string();
        if (n->type == NodeType::Identifier) return std::string(n->str);
        if (n->type == NodeType::ThisExpression) return "this";
        if (n->type == NodeType::MemberExpression && !n->Has(jsparse::kFlagComputed) && n->property()) {
            const std::string b = Chain(n->object());
            return b.empty() ? b : b + "." + std::string(n->property()->str);
        }
        return std::string();
    }

    void Statement(const Node *s, int parent, int depth) {
        if (!s) return;
        switch (s->type) {
            case NodeType::ExportNamedDeclaration:
            case NodeType::ExportDefaultDeclaration: {
                const Node *d = s->declaration();
                if (!d) return;
                if (d->type == NodeType::TSInterfaceDeclaration || d->type == NodeType::TSTypeAliasDeclaration ||
                    d->type == NodeType::TSEnumDeclaration || d->type == NodeType::TSModuleDeclaration ||
                    d->type == NodeType::TSDeclareFunction) {
                    TsDeclaration(d, parent, depth);
                    return;
                }
                if (d->type == NodeType::FunctionDeclaration || d->type == NodeType::ClassDeclaration ||
                    d->type == NodeType::VariableDeclaration) {
                    if (!d->id() && s->type == NodeType::ExportDefaultDeclaration) {
                        if (d->type == NodeType::FunctionDeclaration) {
                            const int me = Add("default", ParamsText(doc_, d), JsLspSymbolKind::Function, s, s, parent);
                            FunctionBody(d, me, depth);
                        } else if (d->type == NodeType::ClassDeclaration) {
                            Class(d, "default", s, s, parent, depth);
                        }
                        return;
                    }
                    Statement(d, parent, depth);
                } else if (s->type == NodeType::ExportDefaultDeclaration) {
                    Value("default", s, s, d, true, parent, depth);
                }
                return;
            }
            case NodeType::TSInterfaceDeclaration:
            case NodeType::TSTypeAliasDeclaration:
            case NodeType::TSEnumDeclaration:
            case NodeType::TSModuleDeclaration:
            case NodeType::TSDeclareFunction:
                TsDeclaration(s, parent, depth);
                return;
            case NodeType::FunctionDeclaration:
                if (s->id()) {
                    const int me = Add(std::string(s->id()->str), TypeParamsText(doc_, s) + ParamsText(doc_, s) +
                                                                      ReturnText(doc_, s),
                                       JsLspSymbolKind::Function, s, s->id(), parent);
                    FunctionBody(s, me, depth);
                }
                return;
            case NodeType::ClassDeclaration:
                if (s->id()) Class(s, std::string(s->id()->str), s, s->id(), parent, depth);
                return;
            case NodeType::VariableDeclaration:
                for (const Node *d : s->declarations()) {
                    if (!d || !d->id()) continue;
                    if (d->id()->type == NodeType::Identifier) {
                        // Inside functions only the ones holding functions/classes: locals are noise.
                        if (depth > 0 && !(IsFunction(d->init()) || (d->init() && d->init()->type == NodeType::ClassExpression)))
                            continue;
                        Value(std::string(d->id()->str), s->declarations().size() == 1 ? s : d, d->id(), d->init(),
                              s->kind == "const", parent, depth);
                    }
                }
                return;
            case NodeType::ExpressionStatement: {
                const Node *e = s->expression();
                if (!e) return;
                if (e->type == NodeType::AssignmentExpression && e->op == "=" &&
                    e->left()->type == NodeType::MemberExpression) {
                    const std::string name = Chain(e->left());
                    if (name.empty() || name.rfind("this.", 0) == 0) return;
                    const Node *r = e->right();
                    if (depth == 0 || IsFunction(r)) {
                        if (IsFunction(r) || (r && (r->type == NodeType::ObjectExpression || r->type == NodeType::ClassExpression)))
                            Value(name, s, e->left(), r, false, parent, depth);
                    }
                    return;
                }
                // describe('x', () => { it('y', ...) })
                if (e->type == NodeType::CallExpression && e->callee() && e->callee()->type == NodeType::Identifier &&
                    e->arguments().size() >= 2 && e->arguments()[0] &&
                    e->arguments()[0]->type == NodeType::Literal && e->arguments()[0]->lit == jsparse::LitKind::String) {
                    const std::string_view c = e->callee()->str;
                    const bool group = c == "describe" || c == "suite" || c == "context";
                    const bool test = c == "it" || c == "test" || c == "specify";
                    if (!group && !test) return;
                    const Node *fn = e->arguments()[1];
                    const int me = Add(std::string(e->arguments()[0]->str2), std::string(c),
                                       group ? JsLspSymbolKind::Namespace : JsLspSymbolKind::Method, s,
                                       e->arguments()[0], parent);
                    if (group && IsFunction(fn)) FunctionBody(fn, me, depth);
                }
                return;
            }
            default:
                return;
        }
    }
};

}  // namespace

std::vector<JsLspSymbol> JsLspSymbols(const JsLspDocument &doc) {
    Outline o(doc);
    return o.Run();
}

// ---- folding ---------------------------------------------------------------------------------------------------

std::vector<JsLspFold> JsLspFolds(const JsLspDocument &doc) {
    std::vector<JsLspFold> out;
    std::set<int> seen;
    auto fold = [&](uint32_t a, uint32_t b, const char *kind, bool keep_last) {
        const int la = doc.LineOf(a), lb = doc.LineOf(b);
        const int end = keep_last ? lb - 1 : lb;
        if (end <= la) return;
        if (!seen.insert(la).second) return;
        out.push_back({la, end, kind});
    };
    if (doc.parse.program) {
        // Runs of imports.
        const auto &body = doc.parse.program->body_list();
        for (uint32_t i = 0; i < body.size();) {
            if (!body[i] || body[i]->type != NodeType::ImportDeclaration) {
                i++;
                continue;
            }
            uint32_t j = i;
            while (j + 1 < body.size() && body[j + 1] && body[j + 1]->type == NodeType::ImportDeclaration) j++;
            if (j > i) fold(body[i]->start, body[j]->end, "imports", false);
            i = j + 1;
        }
        std::vector<const Node *> stack{doc.parse.program};
        std::vector<Node *> kids;
        std::vector<const Node *> order;
        while (!stack.empty()) {
            const Node *n = stack.back();
            stack.pop_back();
            order.push_back(n);
            jsparse::CollectChildren(n, kids);
            for (size_t k = kids.size(); k-- > 0;) stack.push_back(kids[k]);
        }
        for (const Node *n : order) {
            switch (n->type) {
                case NodeType::BlockStatement:
                case NodeType::ClassBody:
                case NodeType::ObjectExpression:
                case NodeType::ObjectPattern:
                case NodeType::ArrayExpression:
                case NodeType::ArrayPattern:
                case NodeType::StaticBlock:
                case NodeType::JSXElement:
                case NodeType::JSXFragment:
                case NodeType::TSInterfaceBody:
                case NodeType::TSTypeLiteral:
                case NodeType::TSModuleBlock:
                case NodeType::TSEnumBody:
                case NodeType::TSMappedType:
                    fold(n->start, n->end - 1, "", true);
                    break;
                case NodeType::SwitchStatement:
                case NodeType::SwitchCase:
                    fold(n->start, n->end - 1, "", n->type == NodeType::SwitchStatement);
                    break;
                case NodeType::TemplateLiteral:
                    fold(n->start, n->end - 1, "", false);
                    break;
                case NodeType::CallExpression:
                case NodeType::NewExpression:
                    if (!n->arguments().empty() && n->end > 0 && doc.text[n->end - 1] == ')') {
                        const Node *callee = n->callee();
                        fold(callee ? callee->end : n->start, n->end - 1, "", true);
                    }
                    break;
                default:
                    break;
            }
        }
    }
    const auto &cs = doc.parse.comments;
    for (size_t k = 0; k < cs.size();) {
        if (cs[k].block) {
            const int la = doc.LineOf(cs[k].start), lb = doc.LineOf(cs[k].end);
            if (lb > la && seen.insert(la).second) out.push_back({la, lb, "comment"});
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
        if (prev > la && seen.insert(la).second) out.push_back({la, prev, "comment"});
        k = j + 1;
    }
    std::sort(out.begin(), out.end(), [](const JsLspFold &a, const JsLspFold &b) { return a.start_line < b.start_line; });
    return out;
}
