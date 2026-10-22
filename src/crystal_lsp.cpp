// mep-crystal-lsp's features (crystal_lsp.h) over the analysis in
// crystal_lsp_analysis.h and the standard library table in
// crystal_lsp_stdlib.h.

#include "crystal_lsp.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

#include "crystal_lsp_analysis.h"
#include "crystal_lsp_stdlib.h"
#include "crystal_syntax.h"

using namespace crlsp;

namespace {

// --- the standard library ----------------------------------------------------------------

struct StdIndex {
    std::unordered_map<std::string, const CrystalStdType *> types_by_base;  // "Array" -> Array(T)
    std::unordered_map<std::string, const CrystalStdType *> types_by_name;  // "Array(T)" -> it
    std::unordered_map<std::string, std::pair<size_t, size_t>> methods_by_owner;
    std::unordered_map<std::string, std::vector<const CrystalStdConst *>> consts_by_owner;
    const CrystalStdMethod *methods = nullptr;
    size_t method_count = 0;
    const CrystalStdType *types = nullptr;
    size_t type_count = 0;
};

const StdIndex &Std() {
    static const StdIndex idx = [] {
        StdIndex s;
        s.types = CrystalStdTypes(&s.type_count);
        for (size_t i = 0; i < s.type_count; i++) {
            const CrystalStdType *t = &s.types[i];
            s.types_by_name[t->name] = t;
            const std::string base = BaseType(t->name);
            if (!s.types_by_base.count(base)) s.types_by_base[base] = t;
        }
        s.methods = CrystalStdMethods(&s.method_count);
        size_t i = 0;
        while (i < s.method_count) {
            size_t j = i;
            while (j < s.method_count && std::string(s.methods[j].owner) == s.methods[i].owner) j++;
            s.methods_by_owner[s.methods[i].owner] = {i, j};
            i = j;
        }
        size_t nc = 0;
        const CrystalStdConst *consts = CrystalStdConsts(&nc);
        for (size_t k = 0; k < nc; k++) s.consts_by_owner[consts[k].owner].push_back(&consts[k]);
        return s;
    }();
    return idx;
}

std::vector<std::string> SplitComma(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[' || c == '{') depth++;
        if (c == ')' || c == ']' || c == '}') depth--;
        if (c == ',' && depth == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    for (std::string &x : out) {
        while (!x.empty() && x.front() == ' ') x.erase(x.begin());
        while (!x.empty() && x.back() == ' ') x.pop_back();
    }
    return out;
}

// The stdlib type a (base) name refers to.
const CrystalStdType *StdType(const std::string &name) {
    const StdIndex &s = Std();
    auto it = s.types_by_name.find(name);
    if (it != s.types_by_name.end()) return it->second;
    auto jt = s.types_by_base.find(BaseType(name));
    return jt == s.types_by_base.end() ? nullptr : jt->second;
}

// --- what is known about a method ------------------------------------------------------------

struct MethodInfo {
    std::string name;
    std::string signature;  // "def foo(a : Int32) : String"
    std::string params;     // "a : Int32" (inside the parentheses)
    std::string ret;
    std::string doc;
    std::string owner;
    bool class_method = false;
    bool is_macro = false;
    std::string path;  // where it is defined ("" this file, "<stdlib>")
    CrRange range;
};

std::string ParamsOf(const std::string &signature) {
    const size_t open = signature.find('(');
    if (open == std::string::npos) return "";
    int depth = 0;
    for (size_t i = open; i < signature.size(); i++) {
        if (signature[i] == '(') depth++;
        if (signature[i] == ')' && --depth == 0) return signature.substr(open + 1, i - open - 1);
    }
    return signature.substr(open + 1);
}

MethodInfo FromStd(const CrystalStdMethod &m) {
    MethodInfo mi;
    mi.name = m.name;
    std::string args = m.args;
    mi.signature = std::string(m.kind == 2 ? "macro " : "def ") + (m.kind == 1 ? "self." : "") + m.name + args;
    if (*m.ret) mi.signature += std::string(" : ") + m.ret;
    mi.params = ParamsOf(args);
    mi.ret = m.ret;
    mi.doc = m.doc;
    mi.owner = m.owner;
    mi.class_method = m.kind == 1;
    mi.is_macro = m.kind == 2;
    mi.path = "<stdlib>";
    return mi;
}

MethodInfo FromDef(const Def &d, const std::string &path) {
    MethodInfo mi;
    mi.name = d.name;
    mi.signature = d.signature;
    mi.params = ParamsOf(d.signature);
    mi.ret = d.ret;
    mi.doc = d.doc;
    mi.owner = d.owner;
    mi.class_method = d.class_method;
    mi.is_macro = d.kind == "macro";
    mi.path = path;
    mi.range = d.range;
    return mi;
}

MethodInfo FromProject(const CrProjectDef &d) {
    MethodInfo mi;
    mi.name = d.name;
    mi.signature = d.signature;
    mi.params = ParamsOf(d.signature);
    mi.ret = d.ret;
    mi.doc = d.doc;
    mi.owner = d.owner;
    mi.class_method = d.class_method;
    mi.is_macro = d.kind == "macro";
    mi.path = d.path;
    mi.range = d.range;
    return mi;
}

bool IsMethodKind(const std::string &k) { return k == "def" || k == "macro" || k == "getter" || k == "setter" || k == "fun"; }
bool IsTypeKind(const std::string &k) {
    return k == "class" || k == "struct" || k == "module" || k == "enum" || k == "lib" || k == "alias" ||
           k == "annotation" || k == "union";
}

std::string LastSegment(const std::string &path) {
    const size_t p = path.rfind("::");
    return p == std::string::npos ? path : path.substr(p + 2);
}

// A type's ancestors as far as the file, the project and the stdlib say (nearest first, itself included).
std::vector<std::string> Ancestors(const std::string &type, const Analysis &a, const CrProject *project, int depth = 0) {
    std::vector<std::string> out = {type};
    if (depth > 6 || type.empty()) return out;
    std::string super;
    std::vector<std::string> includes;
    std::string kind;
    for (const Def &d : a.defs) {
        if (IsTypeKind(d.kind) && LastSegment(type) == d.name) {
            kind = d.kind;
            if (!d.superclass.empty()) super = d.superclass;
            includes.insert(includes.end(), d.includes.begin(), d.includes.end());
        }
    }
    if (project) {
        for (const CrProjectDef &d : project->defs) {
            if (IsTypeKind(d.kind) && LastSegment(type) == d.name) {
                if (kind.empty()) kind = d.kind;
                if (super.empty() && !d.superclass.empty()) super = d.superclass;
                includes.insert(includes.end(), d.includes.begin(), d.includes.end());
            }
        }
    }
    for (const std::string &inc : includes) {
        for (const std::string &x : Ancestors(BaseType(inc), a, project, depth + 1)) out.push_back(x);
    }
    if (!super.empty()) {
        for (const std::string &x : Ancestors(BaseType(super), a, project, depth + 1)) out.push_back(x);
    } else if (const CrystalStdType *t = StdType(type)) {
        for (const std::string &x : SplitComma(t->ancestors)) out.push_back(x);
    } else if (kind == "class") {
        out.push_back("Reference");
        out.push_back("Object");
    } else if (kind == "struct") {
        out.push_back("Struct");
        out.push_back("Value");
        out.push_back("Object");
    }
    return out;
}

// The methods of a type (instance or class side), nearest definition first.
std::vector<MethodInfo> MembersOf(const std::string &type_in, bool class_side, const Analysis &a,
                                  const CrProject *project) {
    std::vector<MethodInfo> out;
    std::set<std::string> seen;
    const std::string type = BaseType(type_in);
    const StdIndex &s = Std();
    for (const std::string &anc : Ancestors(type, a, project)) {
        const std::string base = BaseType(anc);
        for (const Def &d : a.defs) {
            if (!IsMethodKind(d.kind) || d.kind == "fun") continue;
            if (BaseType(d.owner) != base && LastSegment(BaseType(d.owner)) != LastSegment(base)) continue;
            if (d.class_method != class_side && d.kind != "macro") continue;
            if (seen.insert(d.name).second) out.push_back(FromDef(d, ""));
        }
        if (project) {
            for (const CrProjectDef &d : project->defs) {
                if (!IsMethodKind(d.kind) || d.kind == "fun") continue;
                if (LastSegment(BaseType(d.owner)) != LastSegment(base)) continue;
                if (d.class_method != class_side && d.kind != "macro") continue;
                if (seen.insert(d.name).second) out.push_back(FromProject(d));
            }
        }
        const CrystalStdType *t = StdType(anc);
        if (!t) continue;
        auto it = s.methods_by_owner.find(t->name);
        if (it == s.methods_by_owner.end()) continue;
        for (size_t i = it->second.first; i < it->second.second; i++) {
            const CrystalStdMethod &m = s.methods[i];
            const bool cls = m.kind == 1;
            if (cls != class_side && m.kind != 2) continue;
            if (seen.insert(m.name).second || true) {
                // Overloads are kept (signature help shows them); completion dedups by name.
                out.push_back(FromStd(m));
            }
        }
    }
    if (class_side && !seen.count("new")) {
        MethodInfo mi;
        mi.name = "new";
        mi.signature = "def self.new";
        out.push_back(mi);
    }
    return out;
}

// --- the cursor's context ----------------------------------------------------------------

bool IdentByte(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80; }

struct CursorWord {
    int start = 0, end = 0;  // byte columns of the identifier being typed
    std::string prefix;
    char before = 0;  // '.', ':', '@' or 0
    int before_col = -1;
};

CursorWord WordAt(const std::string &s, int col) {
    CursorWord w;
    int c = std::min(col, static_cast<int>(s.size()));
    int b = c;
    while (b > 0 && IdentByte(s[static_cast<size_t>(b - 1)])) b--;
    w.start = b;
    int e = c;
    while (e < static_cast<int>(s.size()) && IdentByte(s[static_cast<size_t>(e)])) e++;
    if (e < static_cast<int>(s.size()) && (s[static_cast<size_t>(e)] == '?' || s[static_cast<size_t>(e)] == '!')) e++;
    w.end = e;
    w.prefix = s.substr(static_cast<size_t>(b), static_cast<size_t>(c - b));
    int p = b - 1;
    while (p >= 0 && s[static_cast<size_t>(p)] == '@') {
        w.before = '@';
        w.start = p;
        p--;
    }
    if (w.before != '@' && p >= 0) {
        if (s[static_cast<size_t>(p)] == '.') {
            w.before = '.';
            w.before_col = p;
        } else if (s[static_cast<size_t>(p)] == ':' && p > 0 && s[static_cast<size_t>(p - 1)] == ':') {
            w.before = ':';
            w.before_col = p - 1;
        }
    }
    return w;
}

// The text of the expression ending just before `end` on a line: `a.b(1).c`, `@x`, `Foo::Bar`, `"s"`.
std::string ReceiverText(const std::string &s, int end) {
    int i = end;
    int depth = 0;
    while (i > 0) {
        const char c = s[static_cast<size_t>(i - 1)];
        if (c == ')' || c == ']' || c == '}') {
            depth++;
        } else if (c == '(' || c == '[' || c == '{') {
            if (depth == 0) break;
            depth--;
        } else if (c == '"' && depth == 0) {
            // A string literal: back to its opening quote.
            int j = i - 2;
            while (j >= 0 && s[static_cast<size_t>(j)] != '"') j--;
            i = std::max(j, 0);
            continue;
        } else if (depth == 0 && !(IdentByte(c) || c == '.' || c == ':' || c == '@' || c == '?' || c == '!')) {
            break;
        }
        i--;
    }
    return s.substr(static_cast<size_t>(i), static_cast<size_t>(end - i));
}

// Locals visible at a position: the cursor's scope up through blocks, bound before it.
std::vector<const Local *> VisibleLocals(const Analysis &a, int line, int col) {
    std::vector<const Local *> out;
    std::set<std::string> seen;
    int s = ScopeAt(a, line, col);
    while (s >= 0) {
        const Scope &sc = a.scopes[static_cast<size_t>(s)];
        for (const auto &kv : sc.vars) {
            const Local &l = a.locals[static_cast<size_t>(kv.second)];
            if (Before(line, col, l.def_range.start_line, l.def_range.start_col)) continue;
            if (seen.insert(l.name).second) out.push_back(&l);
        }
        if (sc.kind != ScopeKind::Block && sc.kind != ScopeKind::Proc) break;
        s = sc.parent;
    }
    return out;
}

std::string EnclosingOwner(const Analysis &a, int line, int col) {
    const int s = ScopeAt(a, line, col);
    return a.scopes[static_cast<size_t>(s)].owner;
}

std::string LocalType(const Analysis &a, const Local &l) {
    if (l.declared_type) return TypeText(l.declared_type);
    if (l.first_value) return InferType(a, l.first_value);
    return "";
}

std::string IvarType(const Analysis &a, const std::string &owner, const std::string &ivar) {
    for (const NameOcc &o : a.names) {
        if (o.kind != NameKind::InstanceVar || o.name != ivar || !o.node) continue;
        if (!owner.empty() && o.owner != owner) continue;
        if (o.node->is(NodeKind::TypeDeclaration)) return TypeText(o.node->declared_type);
    }
    for (const Def &d : a.defs) {
        if (d.kind == "getter" && "@" + d.name == ivar && (owner.empty() || d.owner == owner) && !d.ret.empty()) {
            return d.ret;
        }
    }
    for (const NameOcc &o : a.names) {
        if (o.kind != NameKind::InstanceVar || o.name != ivar || !o.node || !o.write) continue;
        if (!owner.empty() && o.owner != owner) continue;
        if (o.node->is(NodeKind::Assign)) {
            const std::string t = InferType(a, o.node->value_node);
            if (!t.empty()) return t;
        }
    }
    return "";
}

struct ReceiverType {
    std::string type;
    bool class_side = false;
};

std::string MethodRet(const std::string &type, const std::string &name, bool class_side, const Analysis &a,
                      const CrProject *project) {
    for (const MethodInfo &m : MembersOf(type, class_side, a, project)) {
        if (m.name != name) continue;
        if (m.ret == "self" || m.ret == "self?") return type + (m.ret == "self?" ? "?" : "");
        if (m.name == "new" && class_side) return type;
        if (!m.ret.empty() && !(m.ret.size() == 1 && std::isupper(static_cast<unsigned char>(m.ret[0])))) return m.ret;
        return "";
    }
    if (name == "new" && class_side) return type;
    return "";
}

ReceiverType ResolveReceiver(const Analysis &a, const std::string &text_in, int line, int col,
                             const CrProject *project) {
    ReceiverType r;
    std::string text = text_in;
    if (text.empty()) return r;
    // Split the chain at top-level dots.
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    bool in_str = false;
    for (char c : text) {
        if (c == '"') in_str = !in_str;
        if (!in_str) {
            if (c == '(' || c == '[' || c == '{') depth++;
            if (c == ')' || c == ']' || c == '}') depth--;
            if (c == '.' && depth == 0) {
                parts.push_back(cur);
                cur.clear();
                continue;
            }
        }
        cur += c;
    }
    parts.push_back(cur);
    auto strip_args = [](std::string s) {
        const size_t p = s.find_first_of("( ");
        if (p != std::string::npos) s = s.substr(0, p);
        return s;
    };
    std::string first = parts[0];
    std::string type;
    bool class_side = false;
    if (first.empty()) return r;
    if (first[0] == '"') {
        type = "String";
    } else if (first[0] == '[') {
        type = "Array";
    } else if (first[0] == '{') {
        type = "Hash";
    } else if (first[0] == ':') {
        type = "Symbol";
    } else if (first[0] == '\'') {
        type = "Char";
    } else if (std::isdigit(static_cast<unsigned char>(first[0]))) {
        type = first.find('.') != std::string::npos ? "Float64" : "Int32";
    } else if (std::isupper(static_cast<unsigned char>(first[0]))) {
        type = strip_args(first);
        class_side = true;
    } else if (first[0] == '@') {
        type = IvarType(a, EnclosingOwner(a, line, col), strip_args(first));
    } else {
        const std::string name = strip_args(first);
        if (name == "self") {
            type = EnclosingOwner(a, line, col);
        } else {
            for (const Local *l : VisibleLocals(a, line, col)) {
                if (l->name == name) type = LocalType(a, *l);
            }
            if (type.empty()) {
                const std::string owner = EnclosingOwner(a, line, col);
                type = MethodRet(owner.empty() ? "" : owner, name, false, a, project);
                if (type.empty()) {
                    for (const Def &d : a.defs) {
                        if (d.name == name && !d.ret.empty() && IsMethodKind(d.kind)) type = d.ret;
                    }
                }
                if (type.empty()) {
                    const StdIndex &s = Std();
                    auto it = s.methods_by_owner.find("");
                    if (it != s.methods_by_owner.end()) {
                        for (size_t i = it->second.first; i < it->second.second; i++) {
                            if (s.methods[i].name == name && *s.methods[i].ret) {
                                type = s.methods[i].ret;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    for (size_t i = 1; i < parts.size() && !type.empty(); i++) {
        const std::string m = strip_args(parts[i]);
        if (m == "class") {
            class_side = true;
            continue;
        }
        type = MethodRet(BaseType(type), m, class_side, a, project);
        class_side = false;
    }
    r.type = type;
    r.class_side = class_side;
    return r;
}

const char *const kKeywords[] = {
    "abstract", "alias", "annotation", "as", "as?", "asm", "begin", "break", "case", "class", "def", "do", "else",
    "elsif", "end", "ensure", "enum", "extend", "false", "for", "fun", "if", "in", "include", "instance_sizeof",
    "is_a?", "lib", "macro", "module", "next", "nil", "nil?", "of", "out", "pointerof", "private", "protected",
    "require", "rescue", "responds_to?", "return", "select", "self", "sizeof", "struct", "super", "then", "true",
    "type", "typeof", "uninitialized", "union", "unless", "until", "when", "while", "with", "yield",
};

CrLspKind KindFor(const std::string &k) {
    if (k == "class") return CrLspKind::Class;
    if (k == "struct" || k == "union") return CrLspKind::Struct;
    if (k == "module" || k == "lib") return CrLspKind::Module;
    if (k == "enum") return CrLspKind::Enum;
    if (k == "const") return CrLspKind::Constant;
    if (k == "member") return CrLspKind::EnumMember;
    if (k == "getter" || k == "setter") return CrLspKind::Property;
    if (k == "macro") return CrLspKind::Function;
    if (k == "alias" || k == "annotation") return CrLspKind::Interface;
    return CrLspKind::Method;
}

std::string MethodDoc(const MethodInfo &m) {
    std::string s = "```crystal\n" + m.signature + "\n```";
    if (!m.owner.empty()) s += "\n\n*" + m.owner + "*";
    if (!m.doc.empty()) s += "\n\n" + m.doc;
    return s;
}

}  // namespace

// --- the project ----------------------------------------------------------------------------

void CrProjectAddFile(CrProject *project, const std::string &path, const std::vector<std::string> &lines) {
    project->defs.erase(std::remove_if(project->defs.begin(), project->defs.end(),
                                       [&](const CrProjectDef &d) { return d.path == path; }),
                        project->defs.end());
    std::unique_ptr<Analysis> a = Analyze(lines);
    // Includes per type (from `include M` in its body).
    std::map<std::string, std::vector<std::string>> includes;
    std::function<void(Node *, const std::string &)> walk = [&](Node *n, const std::string &owner) {
        if (!n) return;
        if (n->is(NodeKind::Include) && n->name_node) includes[owner].push_back(TypeText(n->name_node));
        if (n->is(NodeKind::Expressions)) {
            for (Node *e : n->exps) walk(e, owner);
        } else if (n->is(NodeKind::ClassDef) || n->is(NodeKind::ModuleDef)) {
            const std::string t = TypeText(n->name_node);
            walk(n->body, owner.empty() ? t : owner + "::" + t);
        } else if (n->is(NodeKind::VisibilityModifier)) {
            walk(n->exp, owner);
        }
    };
    walk(a->root, "");
    for (const Def &d : a->defs) {
        CrProjectDef p;
        p.kind = d.kind == "member" ? "const" : d.kind;
        p.owner = d.owner;
        p.name = d.name;
        p.signature = d.signature;
        p.doc = d.doc;
        p.ret = d.ret;
        p.superclass = d.superclass;
        p.class_method = d.class_method;
        p.path = path;
        p.range = d.range;
        if (IsTypeKind(d.kind)) {
            const std::string full = d.owner.empty() ? d.name : d.owner + "::" + d.name;
            p.includes = includes[full];
        }
        project->defs.push_back(p);
    }
}

// --- completion -----------------------------------------------------------------------------

std::vector<CrLspCompletionItem> CrLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                  const CrProject *project) {
    std::vector<CrLspCompletionItem> out;
    if (line < 0 || line >= static_cast<int>(lines.size())) return out;
    const std::string &s = lines[static_cast<size_t>(line)];
    const CursorWord w = WordAt(s, col);
    std::unique_ptr<Analysis> a = Analyze(lines);
    std::set<std::string> seen;

    auto add = [&](const std::string &label, CrLspKind kind, const std::string &detail, const std::string &doc,
                   const char *sort) {
        if (label.rfind(w.prefix, 0) != 0) return;
        if (!seen.insert(label).second) return;
        CrLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = doc;
        it.replace_start = w.before == '@' ? w.start : w.start;
        it.replace_end = std::min(col, static_cast<int>(s.size()));
        it.sort_text = std::string(sort) + label;
        out.push_back(it);
    };

    if (w.before == '.') {
        const std::string recv = ReceiverText(s, w.before_col);
        const ReceiverType rt = ResolveReceiver(*a, recv, line, col, project);
        if (!rt.type.empty()) {
            for (const MethodInfo &m : MembersOf(rt.type, rt.class_side, *a, project)) {
                if (!m.name.empty() && !std::isalpha(static_cast<unsigned char>(m.name[0])) && m.name[0] != '_') continue;
                add(m.name, m.is_macro ? CrLspKind::Function : CrLspKind::Method, m.signature, MethodDoc(m),
                    m.path == "<stdlib>" ? "2" : "1");
            }
            return out;
        }
        // Unknown receiver: every method name the file, the project and the stdlib know.
        for (const Def &d : a->defs) {
            if (IsMethodKind(d.kind) && d.kind != "fun") add(d.name, KindFor(d.kind), d.signature, d.doc, "1");
        }
        if (project) {
            for (const CrProjectDef &d : project->defs) {
                if (IsMethodKind(d.kind) && d.kind != "fun") add(d.name, KindFor(d.kind), d.signature, d.doc, "2");
            }
        }
        const StdIndex &st = Std();
        for (size_t i = 0; i < st.method_count; i++) {
            const CrystalStdMethod &m = st.methods[i];
            if (*m.owner && m.kind == 0 && std::isalpha(static_cast<unsigned char>(m.name[0]))) {
                add(m.name, CrLspKind::Method, FromStd(m).signature, m.doc, "3");
            }
        }
        return out;
    }

    if (w.before == ':') {
        // `A::` -- what A contains.
        int b = w.before_col;
        int i = b;
        while (i > 0 && (IdentByte(s[static_cast<size_t>(i - 1)]) || s[static_cast<size_t>(i - 1)] == ':')) i--;
        const std::string container = s.substr(static_cast<size_t>(i), static_cast<size_t>(b - i));
        const std::string last = LastSegment(container);
        for (const Def &d : a->defs) {
            if (LastSegment(d.owner) == last && (IsTypeKind(d.kind) || d.kind == "const" || d.kind == "member")) {
                add(d.name, KindFor(d.kind), d.signature, d.doc, "1");
            }
        }
        if (project) {
            for (const CrProjectDef &d : project->defs) {
                if (LastSegment(d.owner) == last && (IsTypeKind(d.kind) || d.kind == "const")) {
                    add(d.name, KindFor(d.kind), d.signature, d.doc, "2");
                }
            }
        }
        const StdIndex &st = Std();
        for (size_t k = 0; k < st.type_count; k++) {
            const std::string n = st.types[k].name;
            if (n.rfind(container + "::", 0) == 0 && n.find("::", container.size() + 2) == std::string::npos) {
                add(n.substr(container.size() + 2), KindFor(st.types[k].kind), st.types[k].kind, st.types[k].doc, "3");
            }
        }
        if (const CrystalStdType *t = StdType(container)) {
            auto it = st.consts_by_owner.find(t->name);
            if (it != st.consts_by_owner.end()) {
                for (const CrystalStdConst *c : it->second) add(c->name, CrLspKind::Constant, c->value, c->doc, "3");
            }
        }
        return out;
    }

    if (w.before == '@') {
        const std::string owner = EnclosingOwner(*a, line, col);
        const std::string typed = s.substr(static_cast<size_t>(w.start), static_cast<size_t>(col - w.start));
        for (const NameOcc &o : a->names) {
            if ((o.kind == NameKind::InstanceVar || o.kind == NameKind::ClassVar) && o.owner == owner &&
                o.name.rfind(typed, 0) == 0 && o.name != typed) {
                CrLspCompletionItem it;
                it.label = o.name;
                it.insert_text = o.name;
                it.kind = CrLspKind::Field;
                it.detail = IvarType(*a, owner, o.name);
                it.replace_start = w.start;
                it.replace_end = col;
                if (seen.insert(o.name).second) out.push_back(it);
            }
        }
        return out;
    }

    // Plain: locals, the enclosing type's methods, the file's and project's, types, stdlib, keywords.
    for (const Local *l : VisibleLocals(*a, line, col)) {
        add(l->name, CrLspKind::Variable, LocalType(*a, *l), "", "0");
    }
    const std::string owner = EnclosingOwner(*a, line, col);
    if (!owner.empty()) {
        for (const MethodInfo &m : MembersOf(owner, false, *a, project)) {
            if (m.path != "<stdlib>" && !m.name.empty() && (std::isalpha(static_cast<unsigned char>(m.name[0])))) {
                add(m.name, m.is_macro ? CrLspKind::Function : CrLspKind::Method, m.signature, MethodDoc(m), "1");
            }
        }
    }
    for (const Def &d : a->defs) {
        if ((IsMethodKind(d.kind) && d.owner.empty()) || IsTypeKind(d.kind) || d.kind == "const") {
            add(d.name, KindFor(d.kind), d.signature, d.doc, "1");
        }
    }
    if (project) {
        for (const CrProjectDef &d : project->defs) {
            if ((IsMethodKind(d.kind) && d.owner.empty()) || (IsTypeKind(d.kind) && d.owner.empty())) {
                add(d.name, KindFor(d.kind), d.signature, d.doc, "2");
            }
        }
    }
    const StdIndex &st = Std();
    auto it = st.methods_by_owner.find("");
    if (it != st.methods_by_owner.end()) {
        for (size_t i = it->second.first; i < it->second.second; i++) {
            const CrystalStdMethod &m = st.methods[i];
            if (std::isalpha(static_cast<unsigned char>(m.name[0]))) {
                add(m.name, m.kind == 2 ? CrLspKind::Function : CrLspKind::Function, FromStd(m).signature,
                    MethodDoc(FromStd(m)), "3");
            }
        }
    }
    // Object's own macros (getter, property, record...) inside a type.
    if (!w.prefix.empty()) {
        for (size_t k = 0; k < st.type_count; k++) {
            const std::string n = st.types[k].name;
            if (n.find("::") == std::string::npos) {
                add(BaseType(n), KindFor(st.types[k].kind), st.types[k].kind, st.types[k].doc, "4");
            }
        }
    }
    for (const char *kw : kKeywords) add(kw, CrLspKind::Keyword, "keyword", "", "5");
    return out;
}

// --- what is under the cursor --------------------------------------------------------------

namespace {

struct Target {
    enum Kind { None, Local, Name } kind = None;
    int local = -1;
    const NameOcc *name = nullptr;
    CrRange range;
};

Target TargetAt(const Analysis &a, int line, int col) {
    Target t;
    for (const auto &lo : a.local_occs) {
        if (Contains(lo.first, line, col)) {
            t.kind = Target::Local;
            t.local = lo.second;
            t.range = lo.first;
            return t;
        }
    }
    // The smallest name occurrence containing the cursor.
    const NameOcc *best = nullptr;
    for (const NameOcc &o : a.names) {
        if (!Contains(o.range, line, col)) continue;
        if (!best || Before(best->range.start_line, best->range.start_col, o.range.start_line, o.range.start_col)) {
            best = &o;
        }
    }
    if (best) {
        t.kind = Target::Name;
        t.name = best;
        t.range = best->range;
    }
    return t;
}

// The methods a call or def name may refer to.
std::vector<MethodInfo> MethodsFor(const Analysis &a, const NameOcc &o, const CrProject *project) {
    std::vector<MethodInfo> out;
    const Node *n = o.node;
    if (o.kind == NameKind::Call && n && n->obj) {
        std::string type;
        bool class_side = false;
        if (n->obj->is(NodeKind::Path) || n->obj->is(NodeKind::Generic)) {
            type = TypeText(n->obj);
            class_side = true;
        } else {
            type = InferType(a, n->obj);
        }
        if (!type.empty()) {
            for (const MethodInfo &m : MembersOf(type, class_side, a, project)) {
                if (m.name == o.name) out.push_back(m);
            }
            if (!out.empty()) return out;
        }
        // Unknown receiver: every method of that name the file and the project define.
        for (const Def &d : a.defs) {
            if (IsMethodKind(d.kind) && d.name == o.name) out.push_back(FromDef(d, ""));
        }
        if (project) {
            for (const CrProjectDef &d : project->defs) {
                if (IsMethodKind(d.kind) && d.name == o.name) out.push_back(FromProject(d));
            }
        }
        return out;
    }
    // No receiver (or a def's own name): the enclosing type's, then the top level's.
    if (!o.owner.empty()) {
        for (const MethodInfo &m : MembersOf(o.owner, o.kind == NameKind::DefName && n && n->receiver, a, project)) {
            if (m.name == o.name) out.push_back(m);
        }
        if (!out.empty()) return out;
    }
    for (const Def &d : a.defs) {
        if (IsMethodKind(d.kind) && d.name == o.name && (d.owner.empty() || o.kind == NameKind::DefName)) {
            out.push_back(FromDef(d, ""));
        }
    }
    if (project) {
        for (const CrProjectDef &d : project->defs) {
            if (IsMethodKind(d.kind) && d.name == o.name && d.owner.empty()) out.push_back(FromProject(d));
        }
    }
    const StdIndex &st = Std();
    auto it = st.methods_by_owner.find("");
    if (it != st.methods_by_owner.end()) {
        for (size_t i = it->second.first; i < it->second.second; i++) {
            if (st.methods[i].name == o.name) out.push_back(FromStd(st.methods[i]));
        }
    }
    return out;
}

struct TypeInfo {
    bool found = false;
    std::string text;
    std::string path;  // "" this file
    CrRange range;
};

TypeInfo TypeFor(const Analysis &a, const std::string &name, const std::string &owner, const CrProject *project) {
    TypeInfo ti;
    const std::string last = LastSegment(name);
    // The file's: prefer one nested where the name is written.
    const Def *best = nullptr;
    for (const Def &d : a.defs) {
        if ((IsTypeKind(d.kind) || d.kind == "const" || d.kind == "member") && d.name == last) {
            const std::string full = d.owner.empty() ? d.name : d.owner + "::" + d.name;
            if (!best || full == name || (owner.size() && full == owner + "::" + name)) best = &d;
        }
    }
    if (best) {
        ti.found = true;
        ti.text = "```crystal\n" + best->signature + "\n```";
        if (!best->owner.empty()) ti.text += "\n\n*" + best->owner + "*";
        if (!best->doc.empty()) ti.text += "\n\n" + best->doc;
        ti.range = best->range;
        return ti;
    }
    if (project) {
        for (const CrProjectDef &d : project->defs) {
            if ((IsTypeKind(d.kind) || d.kind == "const") && d.name == last) {
                ti.found = true;
                ti.text = "```crystal\n" + d.signature + "\n```";
                if (!d.doc.empty()) ti.text += "\n\n" + d.doc;
                ti.path = d.path;
                ti.range = d.range;
                return ti;
            }
        }
    }
    std::string n = name;
    if (n.rfind("::", 0) == 0) n = n.substr(2);
    if (const CrystalStdType *t = StdType(n)) {
        if (BaseType(t->name) == BaseType(n) || LastSegment(BaseType(t->name)) == last) {
            ti.found = true;
            ti.text = std::string("```crystal\n") + t->kind + " " + t->name;
            if (*t->superclass) ti.text += std::string(" < ") + t->superclass;
            ti.text += "\n```";
            if (*t->doc) ti.text += std::string("\n\n") + t->doc;
            ti.path = "<stdlib>";
            return ti;
        }
    }
    return ti;
}

}  // namespace

// --- hover -------------------------------------------------------------------------------------

CrLspHoverInfo CrLspHover(const std::vector<std::string> &lines, int line, int col, const CrProject *project) {
    CrLspHoverInfo h;
    std::unique_ptr<Analysis> a = Analyze(lines);
    const Target t = TargetAt(*a, line, col);
    if (t.kind == Target::Local) {
        const Local &l = a->locals[static_cast<size_t>(t.local)];
        const std::string type = LocalType(*a, l);
        h.found = true;
        h.range = t.range;
        h.text = "```crystal\n" + l.name + (type.empty() ? "" : " : " + type) + "\n```\n\n" +
                 (l.is_block_param ? "block parameter" : l.is_param ? "parameter" : "local variable");
        return h;
    }
    if (t.kind != Target::Name) return h;
    const NameOcc &o = *t.name;
    h.range = t.range;
    if (o.kind == NameKind::Call || o.kind == NameKind::DefName) {
        std::vector<MethodInfo> ms = MethodsFor(*a, o, project);
        if (ms.empty()) return h;
        h.found = true;
        h.text = "```crystal\n";
        for (size_t i = 0; i < ms.size() && i < 6; i++) h.text += ms[i].signature + "\n";
        h.text += "```";
        if (!ms[0].owner.empty()) h.text += "\n\n*" + ms[0].owner + "*";
        for (const MethodInfo &m : ms) {
            if (!m.doc.empty()) {
                h.text += "\n\n" + m.doc;
                break;
            }
        }
        return h;
    }
    if (o.kind == NameKind::Path) {
        const TypeInfo ti = TypeFor(*a, o.name, o.owner, project);
        if (!ti.found) return h;
        h.found = true;
        h.text = ti.text;
        return h;
    }
    if (o.kind == NameKind::InstanceVar || o.kind == NameKind::ClassVar) {
        const std::string type = IvarType(*a, o.owner, o.name);
        h.found = true;
        h.text = "```crystal\n" + o.name + (type.empty() ? "" : " : " + type) + "\n```\n\n" +
                 (o.kind == NameKind::InstanceVar ? "instance variable" : "class variable") +
                 (o.owner.empty() ? "" : " of " + o.owner);
        return h;
    }
    return h;
}

// --- signature help -------------------------------------------------------------------------

CrLspSignatureHelp CrLspSignatures(const std::vector<std::string> &lines, int line, int col, const CrProject *project) {
    CrLspSignatureHelp sh;
    if (line < 0 || line >= static_cast<int>(lines.size())) return sh;
    // Back to the unclosed '(' of the call the cursor is in.
    int l = line;
    int c = std::min(col, static_cast<int>(lines[static_cast<size_t>(l)].size())) - 1;
    int depth = 0;
    int commas = 0;
    bool found = false;
    for (int guard = 0; guard < 4000 && l >= 0; guard++) {
        const std::string &s = lines[static_cast<size_t>(l)];
        if (c < 0) {
            l--;
            if (l < 0 || line - l > 20) break;
            c = static_cast<int>(lines[static_cast<size_t>(l)].size()) - 1;
            continue;
        }
        const char ch = s[static_cast<size_t>(c)];
        if (ch == ')' || ch == ']' || ch == '}') {
            depth++;
        } else if (ch == '(' || ch == '[' || ch == '{') {
            if (depth == 0) {
                if (ch == '(') found = true;
                break;
            }
            depth--;
        } else if (ch == ',' && depth == 0) {
            commas++;
        }
        c--;
    }
    if (!found || c <= 0) return sh;
    const std::string &s = lines[static_cast<size_t>(l)];
    int e = c;
    int b = e;
    while (b > 0 && (IdentByte(s[static_cast<size_t>(b - 1)]) || s[static_cast<size_t>(b - 1)] == '?' ||
                     s[static_cast<size_t>(b - 1)] == '!')) {
        b--;
    }
    const std::string name = s.substr(static_cast<size_t>(b), static_cast<size_t>(e - b));
    if (name.empty()) return sh;
    std::unique_ptr<Analysis> a = Analyze(lines);
    std::vector<MethodInfo> ms;
    if (b > 0 && s[static_cast<size_t>(b - 1)] == '.') {
        const std::string recv = ReceiverText(s, b - 1);
        const ReceiverType rt = ResolveReceiver(*a, recv, l, b, project);
        if (!rt.type.empty()) {
            for (const MethodInfo &m : MembersOf(rt.type, rt.class_side, *a, project)) {
                if (m.name == name) ms.push_back(m);
            }
            if (name == "new" && rt.class_side) {
                for (const MethodInfo &m : MembersOf(rt.type, false, *a, project)) {
                    if (m.name == "initialize") {
                        MethodInfo n2 = m;
                        n2.signature = "def self.new(" + m.params + ")";
                        ms.push_back(n2);
                    }
                }
            }
        }
    } else {
        NameOcc o;
        o.kind = NameKind::Call;
        o.name = name;
        o.owner = EnclosingOwner(*a, l, b);
        ms = MethodsFor(*a, o, project);
    }
    if (ms.empty()) return sh;
    sh.found = true;
    int best = -1;
    for (const MethodInfo &m : ms) {
        CrLspSignature sig;
        sig.label = m.signature;
        sig.documentation = m.doc;
        for (const std::string &p : SplitComma(m.params)) sig.params.push_back(p);
        if (best < 0 && static_cast<int>(sig.params.size()) > commas) best = static_cast<int>(sh.signatures.size());
        sh.signatures.push_back(sig);
        if (sh.signatures.size() >= 12) break;
    }
    sh.active_signature = best < 0 ? 0 : best;
    sh.active_param = commas;
    return sh;
}

// --- symbols, folds ----------------------------------------------------------------------------

std::vector<CrLspSymbol> CrLspSymbols(const std::vector<std::string> &lines) {
    std::vector<CrLspSymbol> out;
    std::unique_ptr<Analysis> a = Analyze(lines);
    std::map<int, int> index;  // def index -> symbol index
    for (size_t i = 0; i < a->defs.size(); i++) {
        const Def &d = a->defs[i];
        CrLspSymbol sym;
        sym.name = d.kind == "def" && d.class_method ? "self." + d.name : d.name;
        sym.detail = d.signature;
        if (d.kind == "class") sym.kind = CrLspSymbolKind::Class;
        else if (d.kind == "struct" || d.kind == "union") sym.kind = CrLspSymbolKind::Struct;
        else if (d.kind == "module") sym.kind = CrLspSymbolKind::Module;
        else if (d.kind == "enum") sym.kind = CrLspSymbolKind::Enum;
        else if (d.kind == "lib") sym.kind = CrLspSymbolKind::Namespace;
        else if (d.kind == "const") sym.kind = CrLspSymbolKind::Constant;
        else if (d.kind == "member") sym.kind = CrLspSymbolKind::EnumMember;
        else if (d.kind == "getter" || d.kind == "setter") sym.kind = CrLspSymbolKind::Property;
        else if (d.kind == "macro" || d.kind == "fun") sym.kind = CrLspSymbolKind::Function;
        else if (d.kind == "alias" || d.kind == "annotation") sym.kind = CrLspSymbolKind::Interface;
        else if (d.name == "initialize") sym.kind = CrLspSymbolKind::Constructor;
        else if (!d.name.empty() && !std::isalpha(static_cast<unsigned char>(d.name[0])) && d.name[0] != '_') sym.kind = CrLspSymbolKind::Operator;
        else sym.kind = d.owner.empty() ? CrLspSymbolKind::Function : CrLspSymbolKind::Method;
        sym.range = d.full_range;
        sym.sel_range = d.range;
        if (Before(sym.sel_range.start_line, sym.sel_range.start_col, sym.range.start_line, sym.range.start_col) ||
            Before(sym.range.end_line, sym.range.end_col, sym.sel_range.end_line, sym.sel_range.end_col)) {
            sym.range = sym.sel_range;
        }
        auto it = index.find(d.parent);
        sym.parent = it == index.end() ? -1 : it->second;
        index[static_cast<int>(i)] = static_cast<int>(out.size());
        out.push_back(sym);
    }
    return out;
}

std::vector<CrLspFold> CrLspFolds(const std::vector<std::string> &lines) {
    std::vector<CrLspFold> out;
    std::unique_ptr<Analysis> a = Analyze(lines);
    std::set<std::pair<int, int>> seen;
    std::function<void(Node *)> walk = [&](Node *n) {
        if (!n) return;
        switch (n->kind) {
            case NodeKind::Def: case NodeKind::ClassDef: case NodeKind::ModuleDef: case NodeKind::EnumDef:
            case NodeKind::LibDef: case NodeKind::Macro: case NodeKind::Block: case NodeKind::If: case NodeKind::Unless:
            case NodeKind::Case: case NodeKind::Select: case NodeKind::While: case NodeKind::Until:
            case NodeKind::ExceptionHandler: case NodeKind::ArrayLiteral: case NodeKind::HashLiteral:
            case NodeKind::NamedTupleLiteral: case NodeKind::TupleLiteral: case NodeKind::CStructOrUnionDef:
            case NodeKind::ProcLiteral: case NodeKind::StringInterpolation: case NodeKind::When:
            case NodeKind::Call: case NodeKind::Annotation: {
                const CrRange r = a->NodeRange(n);
                if (n->kind == NodeKind::If && n->ternary) break;
                if (r.end_line > r.start_line) {
                    int end = r.end_line;
                    // Leave a closing `end`/`}` line visible.
                    if (end < static_cast<int>(lines.size())) {
                        const std::string &e = lines[static_cast<size_t>(end)];
                        const size_t f = e.find_first_not_of(" \t");
                        if (f != std::string::npos && (e.compare(f, 3, "end") == 0 || e[f] == '}' || e[f] == ']' || e[f] == ')')) {
                            end--;
                        }
                    }
                    if (end > r.start_line && seen.insert({r.start_line, end}).second) {
                        out.push_back(CrLspFold{r.start_line, end, ""});
                    }
                }
                break;
            }
            default:
                break;
        }
        std::vector<Node *> kids = {n->obj, n->exp, n->cond, n->then_, n->else_, n->body, n->target, n->value_node,
                                    n->left, n->right, n->block, n->block_arg, n->def_, n->ensure_, n->default_value};
        for (Node *k : kids) walk(k);
        for (Node *k : n->exps) walk(k);
        for (Node *k : n->exps2) walk(k);
        for (Node *k : n->args) walk(k);
        for (Node *k : n->named_args) walk(k->value_node);
        for (auto &e : n->entries) walk(e.second);
        for (auto &e : n->named_entries) walk(e.second);
    };
    walk(a->root);
    // Runs of comment lines.
    int run = -1;
    for (int i = 0; i <= static_cast<int>(lines.size()); i++) {
        bool comment = false;
        if (i < static_cast<int>(lines.size())) {
            const std::string &s = lines[static_cast<size_t>(i)];
            const size_t f = s.find_first_not_of(" \t");
            comment = f != std::string::npos && s[f] == '#';
        }
        if (comment && run < 0) run = i;
        if (!comment && run >= 0) {
            if (i - 1 > run) out.push_back(CrLspFold{run, i - 1, "comment"});
            run = -1;
        }
    }
    std::sort(out.begin(), out.end(), [](const CrLspFold &x, const CrLspFold &y) {
        return x.start_line < y.start_line || (x.start_line == y.start_line && x.end_line > y.end_line);
    });
    return out;
}

// --- navigation ---------------------------------------------------------------------------------

CrLspLocation CrLspDefinition(const std::vector<std::string> &lines, int line, int col, const CrProject *project) {
    CrLspLocation loc;
    std::unique_ptr<Analysis> a = Analyze(lines);
    const Target t = TargetAt(*a, line, col);
    if (t.kind == Target::Local) {
        loc.found = true;
        loc.range = a->locals[static_cast<size_t>(t.local)].def_range;
        return loc;
    }
    if (t.kind != Target::Name) return loc;
    const NameOcc &o = *t.name;
    if (o.kind == NameKind::Call || o.kind == NameKind::DefName) {
        for (const MethodInfo &m : MethodsFor(*a, o, project)) {
            if (m.path == "<stdlib>") continue;
            loc.found = true;
            loc.path = m.path;
            loc.range = m.range;
            return loc;
        }
        return loc;
    }
    if (o.kind == NameKind::Path) {
        const TypeInfo ti = TypeFor(*a, o.name, o.owner, project);
        if (ti.found && ti.path != "<stdlib>") {
            loc.found = true;
            loc.path = ti.path;
            loc.range = ti.range;
        }
        return loc;
    }
    if (o.kind == NameKind::InstanceVar || o.kind == NameKind::ClassVar) {
        for (const NameOcc &x : a->names) {
            if (x.kind == o.kind && x.name == o.name && x.owner == o.owner && (x.is_def || x.write)) {
                loc.found = true;
                loc.range = x.range;
                return loc;
            }
        }
    }
    return loc;
}

namespace {

std::vector<CrLspReference> RefsOf(const Analysis &a, const Target &t) {
    std::vector<CrLspReference> out;
    if (t.kind == Target::Local) {
        for (const Occurrence &o : a.locals[static_cast<size_t>(t.local)].occs) {
            out.push_back(CrLspReference{o.range, o.def, o.write});
        }
        return out;
    }
    if (t.kind != Target::Name) return out;
    const NameOcc &o = *t.name;
    std::set<std::pair<int, int>> seen;
    for (const NameOcc &x : a.names) {
        bool same = false;
        if (o.kind == NameKind::Call || o.kind == NameKind::DefName) {
            same = (x.kind == NameKind::Call || x.kind == NameKind::DefName) && x.name == o.name;
        } else if (o.kind == NameKind::Path) {
            same = x.kind == NameKind::Path && LastSegment(x.name) == LastSegment(o.name);
        } else {
            same = x.kind == o.kind && x.name == o.name && x.owner == o.owner;
        }
        if (!same) continue;
        // A path's occurrence: just its last segment.
        CrRange r = x.range;
        if (x.kind == NameKind::Path && r.start_line == r.end_line) {
            const std::string seg = LastSegment(o.name);
            r.start_col = std::max(r.start_col, r.end_col - static_cast<int>(seg.size()));
        }
        if (!seen.insert({r.start_line, r.start_col}).second) continue;
        out.push_back(CrLspReference{r, x.is_def, x.write});
    }
    return out;
}

}  // namespace

std::vector<CrLspReference> CrLspReferences(const std::vector<std::string> &lines, int line, int col) {
    std::unique_ptr<Analysis> a = Analyze(lines);
    return RefsOf(*a, TargetAt(*a, line, col));
}

CrLspRenameTarget CrLspPrepareRename(const std::vector<std::string> &lines, int line, int col) {
    CrLspRenameTarget r;
    std::unique_ptr<Analysis> a = Analyze(lines);
    const Target t = TargetAt(*a, line, col);
    if (t.kind == Target::Local) {
        r.found = true;
        r.range = t.range;
        r.placeholder = a->locals[static_cast<size_t>(t.local)].name;
        return r;
    }
    if (t.kind != Target::Name) return r;
    const NameOcc &o = *t.name;
    bool defined_here = false;
    if (o.kind == NameKind::InstanceVar || o.kind == NameKind::ClassVar) {
        defined_here = true;
    } else {
        for (const Def &d : a->defs) {
            if (d.name == LastSegment(o.name) && (o.kind == NameKind::Path ? IsTypeKind(d.kind) || d.kind == "const"
                                                                            : IsMethodKind(d.kind))) {
                defined_here = true;
            }
        }
    }
    if (!defined_here) return r;
    r.found = true;
    r.range = t.range;
    if (o.kind == NameKind::Path && r.range.start_line == r.range.end_line) {
        const std::string seg = LastSegment(o.name);
        r.range.start_col = std::max(r.range.start_col, r.range.end_col - static_cast<int>(seg.size()));
        r.placeholder = seg;
    } else {
        r.placeholder = o.name;
    }
    return r;
}

std::vector<CrLspTextEdit> CrLspRename(const std::vector<std::string> &lines, int line, int col,
                                       const std::string &new_name_in, std::string *error) {
    std::vector<CrLspTextEdit> edits;
    const CrLspRenameTarget target = CrLspPrepareRename(lines, line, col);
    if (!target.found) {
        *error = "nothing to rename here (a local, an instance variable, or a method or type this file defines)";
        return edits;
    }
    std::unique_ptr<Analysis> a = Analyze(lines);
    const Target t = TargetAt(*a, line, col);
    std::string new_name = new_name_in;
    auto ident = [](const std::string &s, bool upper) {
        if (s.empty()) return false;
        const auto c0 = static_cast<unsigned char>(s[0]);
        if (upper ? !std::isupper(c0) : !(std::islower(c0) || c0 == '_')) return false;
        for (size_t i = 1; i < s.size(); i++) {
            const char c = s[i];
            if (!(IdentByte(c) || ((c == '?' || c == '!' || c == '=') && i + 1 == s.size() && !upper))) return false;
        }
        return true;
    };
    bool ok = false;
    if (t.kind == Target::Local) {
        ok = ident(new_name, false) && new_name.back() != '?' && new_name.back() != '!' && new_name.back() != '=';
    } else if (t.name->kind == NameKind::InstanceVar || t.name->kind == NameKind::ClassVar) {
        const std::string sigil = t.name->kind == NameKind::InstanceVar ? "@" : "@@";
        if (new_name.rfind(sigil, 0) != 0) new_name = sigil + new_name;
        ok = ident(new_name.substr(sigil.size()), false);
    } else if (t.name->kind == NameKind::Path) {
        ok = ident(new_name, true);
    } else {
        ok = ident(new_name, false);
    }
    if (!ok) {
        *error = "'" + new_name_in + "' is not a valid name here";
        return edits;
    }
    for (const CrLspReference &r : RefsOf(*a, t)) {
        CrRange range = r.range;
        // An @ivar occurrence covers the sigil; a getter's name (`getter x`) does not: keep the sigil there.
        std::string text = new_name;
        if (t.kind == Target::Name && t.name->kind == NameKind::InstanceVar && range.start_line < static_cast<int>(lines.size())) {
            const std::string &s = lines[static_cast<size_t>(range.start_line)];
            if (range.start_col < static_cast<int>(s.size()) && s[static_cast<size_t>(range.start_col)] != '@') {
                text = new_name.substr(1);
            }
        }
        edits.push_back(CrLspTextEdit{range, text});
    }
    return edits;
}
