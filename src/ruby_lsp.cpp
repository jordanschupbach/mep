// mep's Ruby language server, analysis half: see ruby_lsp.h.
//
// The document is parsed with tree-sitter-ruby (ruby_lsp_index.cpp's Parse)
// and indexed together with every `.rb` file it requires (transitively,
// through the load path). Features:
//
//   diagnostics   syntax errors (tree-sitter ERROR/MISSING nodes), and
//                 `require_relative` of a file that does not exist
//   hover         signature, owner and the YARD docs of methods, classes,
//                 modules, constants and attributes; locals with their type
//   completion    after `x.` (methods of x's inferred type), `Foo::`
//                 (members), `@` (instance variables), inside `require '`
//                 (load-path files), and bare names (locals, methods of self,
//                 constants, keywords)
//   signatures    one per YARD `@overload`, parameter docs from `@param`
//   definition    into the required files (or this one)
//   outline       modules, classes, methods, attributes, constants
//
// Types are inferred simply: `Foo.new` is a Foo, a call's type is its YARD
// `@return`, a parameter's is its `@param` type, a local's is its last
// assignment's, block parameters of `each`/`map`/... over `Array<T>` are T.
#include "ruby_lsp.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

#include "ruby_format.h"
#include "ruby_lsp_internal.h"

namespace fs = std::filesystem;
using namespace rblsp;

class RubyLspDocument {
public:
    std::vector<std::string> lines;
    std::vector<size_t> line_start;
    std::string path;
    std::unique_ptr<Tree> tree;
    std::shared_ptr<FileIndex> index;
    World world;
    std::vector<std::string> extra;  // load-path directories of the document

    const std::string &text() const { return tree->text; }
    uint32_t Offset(int line, int col) const {
        if (line < 0) return 0;
        if (static_cast<size_t>(line) >= line_start.size()) return static_cast<uint32_t>(text().size());
        const size_t len = lines[static_cast<size_t>(line)].size();
        const size_t c = static_cast<size_t>(std::max(0, std::min(col, static_cast<int>(len))));
        return static_cast<uint32_t>(line_start[static_cast<size_t>(line)] + c);
    }
};

namespace {

bool IdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

std::string Trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

bool StartsWith(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }

std::string BaseName(const std::string &p) { return fs::path(p).filename().string(); }

// ---------------------------------------------------------------------------
// Lexical context

struct Context {
    std::vector<std::string> nesting;  // enclosing namespaces, outermost first
    std::string self_class;            // "Object" at the top level
    bool self_meta = false;            // inside `def self.x`, `class << self`, or a class body
    TSNode method{};                   // the enclosing def (null at class/top level)
    bool in_method = false;
};

std::string JoinNs(const std::string &owner, const std::string &name) {
    if (StartsWith(name, "::")) return name.substr(2);
    return owner.empty() ? name : owner + "::" + name;
}

Context ContextAt(const RubyLspDocument &d, uint32_t at) {
    Context c;
    c.self_class = "Object";
    TSNode n = d.tree->Root();
    std::string owner;
    bool in_sclass = false;
    while (true) {
        TSNode next{};
        bool found = false;
        const uint32_t k = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < k; i++) {
            TSNode ch = ts_node_named_child(n, i);
            if (ts_node_start_byte(ch) <= at && at <= ts_node_end_byte(ch) &&
                !(at == ts_node_end_byte(ch) && i + 1 < k && ts_node_start_byte(ts_node_named_child(n, i + 1)) == at)) {
                next = ch;
                found = true;
                break;
            }
        }
        if (!found) break;
        n = next;
        const std::string_view ty = NodeType(n);
        if (ty == "module" || ty == "class") {
            TSNode name = Field(n, "name");
            if (!ts_node_is_null(name) && at > ts_node_end_byte(name)) {
                owner = JoinNs(owner, Text(d.text(), name));
                c.nesting.push_back(owner);
                c.self_class = owner;
                c.self_meta = true;  // the class body: self is the class
                c.in_method = false;
                in_sclass = false;
            }
        } else if (ty == "singleton_class") {
            if (Is(Field(n, "value"), "self")) {
                c.self_meta = true;
                in_sclass = true;
            }
        } else if (ty == "method") {
            c.method = n;
            c.in_method = true;
            c.self_meta = in_sclass;
        } else if (ty == "singleton_method") {
            c.method = n;
            c.in_method = true;
            c.self_meta = true;
        }
    }
    if (c.nesting.empty() && !c.in_method) c.self_meta = false;  // `main`, an Object
    return c;
}

// ---------------------------------------------------------------------------
// Locals

enum class LocalKind { Param, Local, BlockParam, Rescue, For };

struct Local {
    std::string name;
    LocalKind kind = LocalKind::Local;
    TSNode def{};    // the identifier
    TSNode value{};  // the assignment's right side (Local)
    TSNode owner{};  // the method (Param), the block (BlockParam) or the rescue clause
};

bool HardScope(TSNode n) {
    const std::string_view t = NodeType(n);
    return t == "method" || t == "singleton_method" || t == "class" || t == "module" || t == "singleton_class" ||
           t == "program";
}
bool SoftScope(TSNode n) {
    const std::string_view t = NodeType(n);
    return t == "block" || t == "do_block" || t == "lambda";
}
bool Contains(TSNode n, uint32_t at) { return ts_node_start_byte(n) <= at && at <= ts_node_end_byte(n); }

TSNode SmallestAt(TSNode root, uint32_t at) { return ts_node_descendant_for_byte_range(root, at, at); }

TSNode HardScopeAt(const RubyLspDocument &d, uint32_t at) {
    for (TSNode n = SmallestAt(d.tree->Root(), at); !ts_node_is_null(n); n = ts_node_parent(n)) {
        if (HardScope(n)) return n;
    }
    return d.tree->Root();
}

void ParamNames(TSNode params, LocalKind kind, TSNode owner, const std::string &src, std::vector<Local> &out) {
    if (ts_node_is_null(params)) return;
    const uint32_t k = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < k; i++) {
        TSNode c = ts_node_named_child(params, i);
        TSNode name{};
        if (Is(c, "identifier")) {
            name = c;
        } else if (Is(c, "destructured_parameter")) {
            ParamNames(c, kind, owner, src, out);
            continue;
        } else {
            name = Field(c, "name");
        }
        if (!Is(name, "identifier")) continue;
        Local l;
        l.name = Text(src, name);
        l.kind = kind;
        l.def = name;
        l.owner = owner;
        out.push_back(l);
    }
}

void AssignTargets(TSNode left, TSNode value, const std::string &src, std::vector<Local> &out) {
    if (Is(left, "identifier")) {
        Local l;
        l.name = Text(src, left);
        l.def = left;
        l.value = value;
        out.push_back(l);
        return;
    }
    if (Is(left, "left_assignment_list") || Is(left, "destructured_left_assignment") || Is(left, "rest_assignment")) {
        const uint32_t k = ts_node_named_child_count(left);
        for (uint32_t i = 0; i < k; i++) AssignTargets(ts_node_named_child(left, i), TSNode{}, src, out);
    }
}

void CollectLocals(TSNode n, uint32_t at, const std::string &src, std::vector<Local> &out, bool top) {
    if (!top && HardScope(n)) return;
    if (!top && SoftScope(n) && !Contains(n, at)) return;
    if (ts_node_start_byte(n) > at) return;
    const std::string_view t = NodeType(n);
    if (t == "assignment" || t == "operator_assignment") {
        TSNode left = Field(n, "left");
        if (ts_node_start_byte(left) < at) AssignTargets(left, t == "assignment" ? Field(n, "right") : TSNode{}, src, out);
    } else if (t == "block" || t == "do_block" || t == "lambda") {
        ParamNames(Field(n, "parameters"), LocalKind::BlockParam, n, src, out);
    } else if (t == "exception_variable") {
        TSNode id = ts_node_named_child(n, 0);
        if (Is(id, "identifier")) {
            Local l;
            l.name = Text(src, id);
            l.kind = LocalKind::Rescue;
            l.def = id;
            l.owner = ts_node_parent(n);
            out.push_back(l);
        }
    } else if (t == "for") {
        TSNode pat = Field(n, "pattern");
        if (Is(pat, "identifier")) {
            Local l;
            l.name = Text(src, pat);
            l.kind = LocalKind::For;
            l.def = pat;
            l.owner = n;
            out.push_back(l);
        } else if (!ts_node_is_null(pat)) {
            AssignTargets(pat, TSNode{}, src, out);
        }
    }
    const uint32_t k = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < k; i++) CollectLocals(ts_node_named_child(n, i), at, src, out, false);
}

// The locals visible at @p at, in the order they are defined.
std::vector<Local> LocalsAt(const RubyLspDocument &d, uint32_t at) {
    std::vector<Local> out;
    TSNode scope = HardScopeAt(d, at);
    if (Is(scope, "method") || Is(scope, "singleton_method")) {
        ParamNames(Field(scope, "parameters"), LocalKind::Param, scope, d.text(), out);
    }
    if (Is(scope, "program")) {
        CollectLocals(scope, at, d.text(), out, true);
    } else {
        const uint32_t k = ts_node_named_child_count(scope);
        for (uint32_t i = 0; i < k; i++) {
            TSNode c = ts_node_named_child(scope, i);
            if (Is(c, "method_parameters")) continue;
            CollectLocals(c, at, d.text(), out, false);
        }
    }
    return out;
}

bool HasLocal(const std::vector<Local> &locals, const std::string &name) {
    for (const Local &l : locals) {
        if (l.name == name) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Types

Type ParseTypeName(const std::string &raw) {
    Type t;
    std::string s = Trim(raw);
    if (s.empty()) return t;
    const size_t lt = s.find_first_of("<{(");
    std::string base = lt == std::string::npos ? s : s.substr(0, lt);
    if (lt != std::string::npos && s[lt] == '<') {
        std::string inner = s.substr(lt + 1);
        if (!inner.empty() && inner.back() == '>') inner.pop_back();
        int depth = 0;
        std::string first;
        for (char c : inner) {
            if (c == '<' || c == '{' || c == '(') depth++;
            if (c == '>' || c == '}' || c == ')') depth--;
            if (c == ',' && depth == 0) break;
            first += c;
        }
        if (Trim(base) == "Class") {
            t.name = Trim(first);
            t.meta = true;
            return t;
        }
        t.elem = Trim(first);
    }
    t.name = Trim(base);
    if (t.name == "Boolean" || t.name == "true" || t.name == "false") t.name = "TrueClass";
    return t;
}

std::string TypeLabel(const Type &t) {
    if (!t.Known()) return std::string();
    std::string s = t.meta ? "Class<" + t.name + ">" : t.name;
    if (!t.elem.empty()) s += "<" + t.elem + ">";
    return s;
}

struct Infer {
    const RubyLspDocument &d;

    std::string ResolveName(const std::string &name, const std::vector<std::string> &nesting) const {
        if (name.empty()) return name;
        const std::string r = d.world.ResolveConst(name, nesting);
        return r.empty() ? name : r;
    }

    Type FromYard(const std::string &types, const Def &def, const Type &recv) const {
        const std::string first = FirstType(types);
        if (first.empty()) return Type();
        if (first == "self" || first == "$self") return recv;
        Type t = ParseTypeName(first);
        if (t.name == "$instance") {
            Type r = recv;
            r.meta = false;
            return r;
        }
        if (t.name == "$elem") return recv.elem.empty() ? Type() : Type{recv.elem, false, ""};
        if (t.elem == "$elem") t.elem = recv.elem;
        std::vector<std::string> nest = def.nesting;
        if (!def.owner.empty() && (nest.empty() || nest.back() != def.owner)) nest.push_back(def.owner);
        t.name = ResolveName(t.name, nest);
        if (!t.elem.empty() && t.elem[0] != '$') t.elem = ResolveName(ParseTypeName(t.elem).name, nest);
        return t;
    }

    // The type a call of @p def on @p recv returns.
    Type Returns(const Def &def, const Type &recv) const {
        if (def.doc) {
            if (!def.doc->returns.empty()) return FromYard(def.doc->returns.front().types, def, recv);
            for (const YardOverload &o : def.doc->overloads) {
                if (o.doc && !o.doc->returns.empty()) return FromYard(o.doc->returns.front().types, def, recv);
            }
        }
        if (!def.return_type.empty()) return FromYard(def.return_type, def, recv);
        return Type();
    }

    // The method definitions a call `name` on @p recv resolves to.
    std::vector<const Def *> Lookup(const Type &recv, const std::string &name) const {
        if (!recv.Known()) return {};
        std::vector<const Def *> defs = d.world.Methods(recv.name, recv.meta, name);
        if (recv.meta && name == "new") {
            // A user-defined `self.new` wins; otherwise `initialize` describes `new`.
            for (const Def *def : defs) {
                if (!def->builtin) return defs;
            }
            std::vector<const Def *> init = d.world.Methods(recv.name, false, "initialize");
            if (!init.empty() && !init.front()->builtin) return init;
        }
        return defs;
    }

    Type SelfType(uint32_t at) const {
        const Context c = ContextAt(d, at);
        return Type{c.self_class, c.self_meta, ""};
    }

    // The index entry of the `def` node @p method (in this document).
    const Def *MethodDefFor(TSNode method) const {
        if (ts_node_is_null(method)) return nullptr;
        const RubyLspRange r = RangeOf(method);
        for (const auto &def : d.index->defs) {
            if (def->kind == DefKind::Method && def->range.line == r.line && def->range.col == r.col) return def.get();
        }
        return nullptr;
    }

    Type LocalType(const Local &l, uint32_t at, int depth) const {
        const std::string &src = d.text();
        switch (l.kind) {
        case LocalKind::Local:
            if (!ts_node_is_null(l.value)) return Expr(src, l.value, ts_node_start_byte(l.value), depth + 1);
            return Type();
        case LocalKind::Param: {
            const Def *m = MethodDefFor(l.owner);
            if (m && m->doc) {
                for (const YardParam &p : m->doc->params) {
                    if (p.name == l.name && !p.types.empty()) return FromYard(p.types, *m, SelfType(at));
                }
            }
            TSNode parent = ts_node_parent(l.def);
            if (Is(parent, "optional_parameter") || Is(parent, "keyword_parameter")) {
                TSNode v = Field(parent, "value");
                if (!ts_node_is_null(v)) return Expr(src, v, ts_node_start_byte(v), depth + 1);
            }
            return Type();
        }
        case LocalKind::BlockParam: {
            // `recv.each do |x|`: the element type, or the method's @yieldparam.
            TSNode block = l.owner;
            TSNode call = ts_node_parent(block);
            if (!Is(call, "call")) return Type();
            TSNode params = Field(block, "parameters");
            int index = 0;
            const uint32_t k = ts_node_named_child_count(params);
            for (uint32_t i = 0; i < k; i++) {
                TSNode p = ts_node_named_child(params, i);
                if (ts_node_start_byte(p) <= ts_node_start_byte(l.def) && ts_node_end_byte(l.def) <= ts_node_end_byte(p)) {
                    index = static_cast<int>(i);
                    break;
                }
            }
            TSNode recv_node = Field(call, "receiver");
            const std::string meth = Text(src, Field(call, "method"));
            const Type recv = ts_node_is_null(recv_node) ? SelfType(ts_node_start_byte(call))
                                                         : Expr(src, recv_node, ts_node_start_byte(call), depth + 1);
            for (const Def *def : Lookup(recv, meth)) {
                if (def->doc && static_cast<size_t>(index) < def->doc->yieldparams.size()) {
                    const std::string &ty = def->doc->yieldparams[static_cast<size_t>(index)].types;
                    if (!ty.empty()) return FromYard(ty, *def, recv);
                }
            }
            static const std::set<std::string> kIter = {
                "each", "map", "collect", "flat_map", "select", "filter", "filter_map", "reject", "find", "detect",
                "each_with_index", "each_with_object", "sort_by", "min_by", "max_by", "group_by", "partition", "sum",
                "count", "any?", "all?", "none?", "take_while", "drop_while", "find_index", "each_entry", "map!",
                "select!", "reject!", "delete_if", "keep_if", "uniq", "sort"};
            if (index == 0 && !recv.elem.empty() && kIter.count(meth)) return Type{recv.elem, false, ""};
            return Type();
        }
        case LocalKind::Rescue: {
            TSNode exc = Field(l.owner, "exceptions");
            if (!ts_node_is_null(exc) && ts_node_named_child_count(exc) > 0) {
                Type t = Expr(src, ts_node_named_child(exc, 0), at, depth + 1);
                t.meta = false;
                return t;
            }
            return Type{"StandardError", false, ""};
        }
        case LocalKind::For:
            return Type();
        }
        return Type();
    }

    // `@name`'s type: its assignment in the enclosing class, or its attribute's YARD type.
    Type IvarType(const std::string &name, uint32_t at, int depth) const {
        const Context c = ContextAt(d, at);
        TSNode cls{};
        for (TSNode n = SmallestAt(d.tree->Root(), at); !ts_node_is_null(n); n = ts_node_parent(n)) {
            if (Is(n, "class") || Is(n, "module")) {
                cls = n;
                break;
            }
        }
        if (!ts_node_is_null(cls) && depth < 10) {
            std::vector<TSNode> assigns;
            std::function<void(TSNode)> walk = [&](TSNode n) {
                if (Is(n, "assignment")) {
                    TSNode left = Field(n, "left");
                    if (Is(left, "instance_variable") && Text(d.text(), left) == name) assigns.push_back(n);
                }
                const uint32_t k = ts_node_named_child_count(n);
                for (uint32_t i = 0; i < k; i++) walk(ts_node_named_child(n, i));
            };
            walk(cls);
            for (TSNode a : assigns) {
                TSNode right = Field(a, "right");
                const Type t = Expr(d.text(), right, ts_node_start_byte(right), depth + 1);
                if (t.Known()) return t;
            }
        }
        const std::string attr = name.size() > 1 ? name.substr(1) : name;
        for (const Def *def : d.world.Methods(c.self_class, false, attr)) {
            if (def->kind == DefKind::Attr) return Returns(*def, Type{c.self_class, false, ""});
        }
        return Type();
    }

    // The type of expression @p n (in @p src: the document or a snippet), with names looked
    // up as seen at @p at in the document.
    Type Expr(const std::string &src, TSNode n, uint32_t at, int depth) const {
        if (ts_node_is_null(n) || depth > 12) return Type();
        const std::string_view t = NodeType(n);
        if (t == "program" || t == "parenthesized_statements" || t == "body_statement" || t == "then" || t == "begin") {
            const uint32_t k = ts_node_named_child_count(n);
            for (uint32_t i = k; i-- > 0;) {
                TSNode c = ts_node_named_child(n, i);
                if (!Is(c, "comment")) return Expr(src, c, at, depth + 1);
            }
            return Type();
        }
        if (t == "string" || t == "chained_string" || t == "heredoc_beginning" || t == "subshell") {
            return Type{"String", false, ""};
        }
        if (t == "integer") return Type{"Integer", false, ""};
        if (t == "float") return Type{"Float", false, ""};
        if (t == "simple_symbol" || t == "delimited_symbol") return Type{"Symbol", false, ""};
        if (t == "regex") return Type{"Regexp", false, ""};
        if (t == "nil") return Type{"NilClass", false, ""};
        if (t == "true") return Type{"TrueClass", false, ""};
        if (t == "false") return Type{"FalseClass", false, ""};
        if (t == "range") return Type{"Range", false, ""};
        if (t == "lambda") return Type{"Proc", false, ""};
        if (t == "hash") return Type{"Hash", false, ""};
        if (t == "string_array") return Type{"Array", false, "String"};
        if (t == "symbol_array") return Type{"Array", false, "Symbol"};
        if (t == "array") {
            Type a{"Array", false, ""};
            if (ts_node_named_child_count(n) > 0) {
                const Type e = Expr(src, ts_node_named_child(n, 0), at, depth + 1);
                if (e.Known() && !e.meta) a.elem = e.name;
            }
            return a;
        }
        if (t == "self") return SelfType(at);
        if (t == "constant" || t == "scope_resolution") {
            const std::string name = Text(src, n);
            const Context c = ContextAt(d, at);
            const std::string fqn = d.world.ResolveConst(name, c.nesting);
            if (fqn.empty()) return Type();
            if (d.world.IsNamespace(fqn)) return Type{fqn, true, ""};
            for (const Def *def : d.world.Constants(fqn)) {
                if (def->doc && !def->doc->returns.empty()) return FromYard(def->doc->returns.front().types, *def, Type());
                if (!def->return_type.empty()) return FromYard(def->return_type, *def, Type());
            }
            return Type();
        }
        if (t == "instance_variable") return IvarType(Text(src, n), at, depth);
        if (t == "identifier") {
            const std::string name = Text(src, n);
            const std::vector<Local> locals = LocalsAt(d, at);
            if (HasLocal(locals, name)) {
                // The latest definition with a known type before `at`.
                for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
                    if (it->name != name) continue;
                    const Type ty = LocalType(*it, at, depth);
                    if (ty.Known()) return ty;
                }
                return Type();
            }
            // A call on self without arguments.
            const Type self = SelfType(at);
            for (const Def *def : Lookup(self, name)) return Returns(*def, self);
            for (const Def *def : d.world.TopLevelMethods()) {
                if (def->name == name) return Returns(*def, self);
            }
            return Type();
        }
        if (t == "call") {
            TSNode recv_node = Field(n, "receiver");
            const std::string meth = Text(src, Field(n, "method"));
            if (meth.empty()) return Type();
            const Type recv = ts_node_is_null(recv_node) ? SelfType(at) : Expr(src, recv_node, at, depth + 1);
            if (!recv.Known()) {
                if (ts_node_is_null(recv_node)) {
                    for (const Def *def : d.world.TopLevelMethods()) {
                        if (def->name == meth) return Returns(*def, recv);
                    }
                }
                return Type();
            }
            if (recv.meta && meth == "new") {
                for (const Def *def : d.world.Methods(recv.name, true, "new")) {
                    if (!def->builtin) {
                        const Type r = Returns(*def, recv);
                        if (r.Known()) return r;
                    }
                }
                return Type{recv.name, false, ""};
            }
            for (const Def *def : Lookup(recv, meth)) return Returns(*def, recv);
            if (ts_node_is_null(recv_node)) {
                for (const Def *def : d.world.TopLevelMethods()) {
                    if (def->name == meth) return Returns(*def, recv);
                }
            }
            return Type();
        }
        if (t == "element_reference") {
            const Type recv = Expr(src, Field(n, "object"), at, depth + 1);
            if (recv.name == "Array" && !recv.elem.empty()) return Type{recv.elem, false, ""};
            if (recv.Known()) {
                for (const Def *def : Lookup(recv, "[]")) return Returns(*def, recv);
            }
            return Type();
        }
        if (t == "binary") {
            const std::string op = Text(src, Field(n, "operator"));
            if (op == "||" || op == "or") return Expr(src, Field(n, "left"), at, depth + 1);
            if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
                return Type{"TrueClass", false, ""};
            }
            if (op == "&&" || op == "and") return Type();
            const Type left = Expr(src, Field(n, "left"), at, depth + 1);
            if (left.Known()) {
                for (const Def *def : Lookup(left, op)) return Returns(*def, left);
                if (left.name == "Integer" || left.name == "Float" || left.name == "String" || left.name == "Array") {
                    return Type{left.name, false, left.elem};
                }
            }
            return Type();
        }
        if (t == "conditional") return Expr(src, Field(n, "consequence"), at, depth + 1);
        if (t == "assignment") return Expr(src, Field(n, "right"), at, depth + 1);
        return Type();
    }
};

// A Ruby expression's text before byte @p end on its line: `foo.bar(1)[2].baz` when
// completing after `baz.`. Stops at whitespace, operators and unbalanced brackets.
std::string ReceiverText(const std::string &line, size_t end) {
    size_t i = end;
    int depth = 0;
    while (i > 0) {
        const char c = line[i - 1];
        if (c == ')' || c == ']' || c == '}') {
            depth++;
            i--;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            if (depth == 0) break;
            depth--;
            i--;
            continue;
        }
        if (c == '"' || c == '\'') {
            const char q = c;
            i--;
            while (i > 0 && line[i - 1] != q) i--;
            if (i > 0) i--;
            continue;
        }
        if (depth > 0) {
            i--;
            continue;
        }
        if (IdentChar(c) || c == '.' || c == '@' || c == '$' || c == '?' || c == '!') {
            i--;
            continue;
        }
        if (c == ':' && i >= 2 && line[i - 2] == ':') {
            i -= 2;
            continue;
        }
        if (c == '&' && i < line.size() && line[i] == '.') {  // `&.`
            i--;
            continue;
        }
        break;
    }
    std::string s = line.substr(i, end - i);
    while (!s.empty() && (s[0] == '.' || s[0] == '?' || s[0] == '!')) s.erase(0, 1);
    return s;
}

// The single expression of a parsed snippet (a null node when it does not parse cleanly).
TSNode SnippetExpr(const Tree &t) {
    TSNode root = t.Root();
    if (ts_node_named_child_count(root) != 1) return TSNode{};
    TSNode e = ts_node_named_child(root, 0);
    if (ts_node_has_error(e)) return TSNode{};
    return e;
}

// ---------------------------------------------------------------------------
// Markdown

std::string ReturnTypes(const Def &d) {
    if (d.doc) {
        if (!d.doc->returns.empty()) return d.doc->returns.front().types;
        for (const YardOverload &o : d.doc->overloads) {
            if (o.doc && !o.doc->returns.empty()) return o.doc->returns.front().types;
        }
    }
    if (!d.return_type.empty() && d.return_type.find('$') == std::string::npos) return d.return_type;
    return std::string();
}

std::string Location(const RubyLspDocument &doc, const Def &d) {
    if (d.builtin) return "Ruby core";
    if (d.path.empty() || d.path == doc.path) return std::string();
    return BaseName(d.path) + ":" + std::to_string(d.range.line + 1);
}

std::string OwnerLabel(const Def &d) { return d.owner.empty() ? std::string("Object") : d.owner; }

// `Frame#filter(column, value = nil)` / `Frame.read(path)` / `Frame.new(...)`.
std::string MethodHead(const Def &d, const std::string &as_name = std::string()) {
    std::string head;
    if (!d.owner.empty()) head = d.owner + (d.singleton || !as_name.empty() ? "." : "#");
    Def copy = d;
    if (!as_name.empty()) copy.name = as_name;
    head += copy.Signature();
    return head;
}

std::string DefMarkdown(const RubyLspDocument &doc, const Def &d, const std::string &as_name = std::string()) {
    std::string code;
    std::string kind;
    switch (d.kind) {
    case DefKind::Module:
        code = "module " + d.fqn;
        kind = "module";
        break;
    case DefKind::Class:
        code = "class " + d.fqn + (d.superclass.empty() ? std::string() : " < " + d.superclass);
        kind = "class";
        break;
    case DefKind::Constant:
        code = d.fqn + (d.value.empty() ? std::string() : " = " + d.value);
        kind = "constant";
        break;
    case DefKind::Attr: {
        code = OwnerLabel(d) + "#" + d.name;
        const std::string rt = ReturnTypes(d);
        if (!rt.empty()) code += " -> " + rt;
        kind = d.attr == "rw" ? "attribute (read/write)" : d.attr == "w" ? "attribute (write)" : "attribute (read)";
        break;
    }
    case DefKind::Alias:
        code = MethodHead(d) + "  # alias of " + d.alias_of;
        kind = "alias";
        break;
    case DefKind::Method: {
        std::vector<std::string> sigs;
        if (d.doc && !d.doc->overloads.empty()) {
            const std::string prefix =
                d.owner.empty() ? std::string() : d.owner + (d.singleton || !as_name.empty() ? "." : "#");
            for (const YardOverload &o : d.doc->overloads) {
                std::string s = o.signature;
                if (!as_name.empty() && StartsWith(s, d.name)) s = as_name + s.substr(d.name.size());
                const std::string rt = o.doc && !o.doc->returns.empty() ? o.doc->returns.front().types : std::string();
                sigs.push_back(prefix + s + (rt.empty() ? std::string() : " -> " + rt));
            }
        } else {
            const std::string rt = ReturnTypes(d);
            sigs.push_back(MethodHead(d, as_name) + (rt.empty() ? std::string() : " -> " + rt));
        }
        for (size_t i = 0; i < sigs.size(); i++) code += (i ? "\n" : "") + sigs[i];
        kind = d.singleton || !as_name.empty() ? "class method" : d.owner.empty() ? "method" : "instance method";
        if (!d.visibility.empty() && as_name.empty()) kind = d.visibility + " " + kind;
        break;
    }
    }
    std::string s = "```ruby\n" + code + "\n```\n*" + kind;
    const std::string loc = Location(doc, d);
    if (!loc.empty()) s += " — " + loc;
    s += "*";
    if (d.doc) {
        const std::string md = YardMarkdown(*d.doc);
        if (!md.empty()) s += "\n\n" + md;
        // Overloads: their own descriptions and tags follow the shared text.
        for (const YardOverload &o : d.doc->overloads) {
            if (!o.doc) continue;
            const std::string om = YardMarkdown(*o.doc);
            if (!om.empty()) s += "\n\n---\n`" + o.signature + "`\n\n" + om;
        }
    }
    return s;
}

std::string LocalMarkdown(const Local &l, const Type &t, const Infer &inf) {
    std::string code = l.name;
    if (t.Known()) code += ": " + TypeLabel(t);
    const char *kind = l.kind == LocalKind::Param        ? "parameter"
                       : l.kind == LocalKind::BlockParam ? "block parameter"
                       : l.kind == LocalKind::Rescue     ? "exception variable"
                                                         : "local variable";
    std::string s = "```ruby\n" + code + "\n```\n*" + kind + "*";
    if (l.kind == LocalKind::Param) {
        const Def *m = inf.MethodDefFor(l.owner);
        if (m && m->doc) {
            for (const YardParam &p : m->doc->params) {
                if (p.name == l.name && !p.text.empty()) s += "\n\n" + p.text;
            }
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// What is at a position

struct Target {
    enum Kind { None, Method, Const, LocalVar, Ivar, Require, Self } kind = None;
    std::vector<const Def *> defs;
    std::string as_name;  // "new" when `initialize` describes `Foo.new`
    Local local;
    Type type;
    TSNode node{};
    std::string require_path;
    std::string name;
};

// User-defined methods named @p name anywhere in the world (when the receiver's type is unknown).
std::vector<const Def *> MethodsNamed(const World &w, const std::string &name) {
    std::vector<const Def *> out;
    std::set<std::string> owners;
    for (const auto &f : w.Files()) {
        if (f->path == "<builtin>") continue;
        for (const auto &d : f->defs) {
            if ((d->kind == DefKind::Method || d->kind == DefKind::Attr || d->kind == DefKind::Alias) &&
                d->name == name && owners.insert(d->owner + (d->singleton ? "." : "#")).second) {
                out.push_back(d.get());
            }
        }
    }
    return out;
}

bool Wordish(TSNode x) {
    const std::string_view t = NodeType(x);
    return t == "identifier" || t == "constant" || t == "instance_variable" || t == "class_variable" ||
           t == "global_variable" || t == "self" || t == "string_content";
}

Target TargetAt(const RubyLspDocument &d, int line, int col) {
    Target tg;
    const uint32_t at = d.Offset(line, col);
    const std::string &src = d.text();
    TSNode n = SmallestAt(d.tree->Root(), at);
    // At the end of a word, the word.
    if (!Wordish(n) && at > 0) {
        TSNode m = SmallestAt(d.tree->Root(), at - 1);
        if (Wordish(m)) n = m;
    }
    if (ts_node_is_null(n)) return tg;
    tg.node = n;
    const Infer inf{d};
    const std::string_view ty = NodeType(n);
    TSNode parent = ts_node_parent(n);
    tg.name = Text(src, n);

    if (ty == "string_content") {
        TSNode str = parent;
        if (!Is(str, "string")) return tg;
        const RubyLspRange sr = RangeOf(str);
        for (const Require &r : d.index->reqs) {
            if (r.range.line == sr.line && r.range.col == sr.col) {
                tg.kind = Target::Require;
                tg.require_path = ResolveRequire(r, d.path, d.extra);
                tg.node = str;
                return tg;
            }
        }
        return tg;
    }
    if (ty == "self") {
        tg.kind = Target::Self;
        tg.type = inf.SelfType(at);
        return tg;
    }
    if (ty == "instance_variable") {
        tg.kind = Target::Ivar;
        tg.type = inf.IvarType(tg.name, at, 0);
        const Context c = ContextAt(d, at);
        for (const Def *def : d.world.Methods(c.self_class, false, tg.name.substr(1))) {
            if (def->kind == DefKind::Attr) tg.defs.push_back(def);
        }
        return tg;
    }
    if (ty == "constant") {
        // The constant path up to this segment: `A::B::C` with the cursor on B is `A::B`.
        std::string path = tg.name;
        TSNode top = n;
        if (Is(parent, "scope_resolution") && ts_node_eq(Field(parent, "name"), n)) {
            path = Text(src, parent);
            top = parent;
        }
        const Context c = ContextAt(d, at);
        const std::string fqn = d.world.ResolveConst(path, c.nesting);
        if (fqn.empty()) return tg;
        tg.kind = Target::Const;
        tg.node = top;
        tg.name = path;
        if (d.world.IsNamespace(fqn)) {
            tg.defs = d.world.Namespace(fqn);
            tg.type = Type{fqn, true, ""};
        } else {
            tg.defs = d.world.Constants(fqn);
        }
        return tg;
    }
    if (ty != "identifier") return tg;

    // A method's own name in its definition.
    if ((Is(parent, "method") || Is(parent, "singleton_method")) && ts_node_eq(Field(parent, "name"), n)) {
        const RubyLspRange r = RangeOf(n);
        for (const auto &def : d.index->defs) {
            if (def->kind == DefKind::Method && def->sel.line == r.line && def->sel.col == r.col) {
                tg.kind = Target::Method;
                tg.defs.push_back(def.get());
                return tg;
            }
        }
        return tg;
    }
    // A call: `recv.name`, `name(...)`.
    if (Is(parent, "call") && ts_node_eq(Field(parent, "method"), n)) {
        TSNode recv_node = Field(parent, "receiver");
        const Type recv = ts_node_is_null(recv_node) ? inf.SelfType(at)
                                                     : inf.Expr(src, recv_node, ts_node_start_byte(parent), 0);
        tg.kind = Target::Method;
        tg.type = recv;
        tg.defs = inf.Lookup(recv, tg.name);
        if (recv.meta && tg.name == "new" && !tg.defs.empty() && tg.defs.front()->name == "initialize") tg.as_name = "new";
        if (tg.defs.empty() && !ts_node_is_null(recv_node) && !recv.Known()) tg.defs = MethodsNamed(d.world, tg.name);
        if (tg.defs.empty() && ts_node_is_null(recv_node)) {
            for (const Def *def : d.world.TopLevelMethods()) {
                if (def->name == tg.name) tg.defs.push_back(def);
            }
        }
        if (tg.defs.empty()) tg.kind = Target::None;
        return tg;
    }
    // Parameters and other locals.
    const std::vector<Local> locals = LocalsAt(d, std::max(at, ts_node_end_byte(n)));
    for (const Local &l : locals) {
        if (l.name != tg.name) continue;
        tg.kind = Target::LocalVar;
        tg.local = l;  // the first definition
        for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
            if (it->name != tg.name || ts_node_start_byte(it->def) > at) continue;
            tg.type = inf.LocalType(*it, at, 0);
            if (tg.type.Known()) break;
        }
        return tg;
    }
    // A bare name: a method of self (or a top-level def).
    const Type self = inf.SelfType(at);
    tg.defs = inf.Lookup(self, tg.name);
    if (tg.defs.empty()) {
        for (const Def *def : d.world.TopLevelMethods()) {
            if (def->name == tg.name) tg.defs.push_back(def);
        }
    }
    if (!tg.defs.empty()) {
        tg.kind = Target::Method;
        tg.type = self;
    }
    return tg;
}

// ---------------------------------------------------------------------------
// Signature help: the call around the cursor

struct CallSite {
    bool found = false;
    std::string receiver;  // its text, "" for self
    std::string name;
    int commas = 0;
    std::string keyword;  // `key:` being typed in the current argument
};

CallSite FindCall(const RubyLspDocument &d, int line, int col) {
    CallSite cs;
    const std::string &src = d.text();
    const uint32_t at = d.Offset(line, col);
    const int first = std::max(0, line - 40);
    uint32_t i = d.Offset(first, 0);
    struct Frame {
        char open;
        uint32_t pos;
        int commas;
        uint32_t arg_start;
    };
    std::vector<Frame> stack;
    while (i < at) {
        const char c = src[i];
        if (c == '#') {
            while (i < at && src[i] != '\n') i++;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            const char q = c;
            i++;
            while (i < at && src[i] != q) {
                if (src[i] == '\\') i++;
                i++;
            }
            i++;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            stack.push_back(Frame{c, i, 0, i + 1});
        } else if (c == ')' || c == ']' || c == '}') {
            if (!stack.empty()) stack.pop_back();
        } else if (c == ',' && !stack.empty()) {
            stack.back().commas++;
            stack.back().arg_start = i + 1;
        }
        i++;
    }
    if (stack.empty() || stack.back().open != '(') return cs;
    const Frame f = stack.back();
    const uint32_t e = f.pos;
    uint32_t b = e;
    while (b > 0 && (IdentChar(src[b - 1]) || src[b - 1] == '?' || src[b - 1] == '!')) b--;
    if (b == e) return cs;
    cs.name = src.substr(b, e - b);
    if (std::isdigit(static_cast<unsigned char>(cs.name[0]))) return cs;
    static const std::set<std::string> kNotCalls = {"if", "unless", "while", "until", "and", "or", "not",
                                                     "return", "when", "in", "elsif", "case", "def"};
    if (kNotCalls.count(cs.name)) return cs;
    // `def name(` is a definition, not a call.
    {
        uint32_t k = b;
        while (k > 0 && (src[k - 1] == ' ' || src[k - 1] == '\t')) k--;
        if (k >= 3 && src.compare(k - 3, 3, "def") == 0 && (k == 3 || !IdentChar(src[k - 4]))) return cs;
        if (k >= 4 && src[k - 1] == '.' && src.compare(k - 5 < k ? k - 5 : 0, 4, "self") == 0) {
            uint32_t j = k - 5;
            while (j > 0 && (src[j - 1] == ' ' || src[j - 1] == '\t')) j--;
            if (j >= 3 && src.compare(j - 3, 3, "def") == 0) return cs;
        }
    }
    const size_t nl = b == 0 ? std::string::npos : src.rfind('\n', b - 1);
    const size_t lb = nl == std::string::npos ? 0 : nl + 1;
    if (b > 0 && src[b - 1] == '.') {
        size_t dot = b - 1;
        if (dot > lb && src[dot - 1] == '&') dot--;
        const std::string line_text = src.substr(lb, dot - lb);
        cs.receiver = ReceiverText(line_text, line_text.size());
        if (cs.receiver.empty()) return cs;
    } else if (b > 1 && src[b - 1] == ':' && src[b - 2] == ':') {
        const std::string line_text = src.substr(lb, b - 2 - lb);
        cs.receiver = ReceiverText(line_text, line_text.size());
    }
    cs.commas = f.commas;
    const std::string arg = Trim(src.substr(f.arg_start, at - f.arg_start));
    size_t k = 0;
    while (k < arg.size() && IdentChar(arg[k])) k++;
    if (k > 0 && k < arg.size() && arg[k] == ':' && (k + 1 >= arg.size() || arg[k + 1] != ':')) {
        cs.keyword = arg.substr(0, k);
    }
    cs.found = true;
    return cs;
}

RubyLspSignature MakeSignature(const std::string &name, const std::vector<Param> &params, const YardDoc *doc) {
    RubyLspSignature s;
    s.label = name + "(";
    for (size_t i = 0; i < params.size(); i++) {
        if (i) s.label += ", ";
        RubyLspSignatureParam p;
        p.label = ParamLabel(params[i]);
        s.label += p.label;
        if (doc) {
            for (const YardParam &yp : doc->params) {
                if (yp.name != params[i].name) continue;
                if (!yp.types.empty()) p.documentation = "`" + yp.types + "`";
                if (!yp.text.empty()) p.documentation += (p.documentation.empty() ? "" : " — ") + yp.text;
            }
            for (const YardOption &o : doc->options) {
                if (o.hash != params[i].name) continue;
                p.documentation += std::string(p.documentation.empty() ? "" : "\n") + "- `" + o.key + "`" +
                                   (o.types.empty() ? "" : " (`" + o.types + "`)") + (o.text.empty() ? "" : " — " + o.text);
            }
        }
        s.params.push_back(p);
    }
    s.label += ")";
    if (doc) s.documentation = YardMarkdown(*doc);
    return s;
}

int ActiveParam(const std::vector<Param> &params, int commas, const std::string &keyword) {
    if (!keyword.empty()) {
        for (size_t i = 0; i < params.size(); i++) {
            if ((params[i].kind == ParamKind::Key || params[i].kind == ParamKind::KeyReq) && params[i].name == keyword) {
                return static_cast<int>(i);
            }
        }
        for (size_t i = 0; i < params.size(); i++) {
            if (params[i].kind == ParamKind::KeyRest) return static_cast<int>(i);
        }
    }
    int positional = 0;
    int rest = -1;
    for (size_t i = 0; i < params.size(); i++) {
        const ParamKind k = params[i].kind;
        if (k == ParamKind::Rest) rest = static_cast<int>(i);
        if (k == ParamKind::Req || k == ParamKind::Opt || k == ParamKind::Rest || k == ParamKind::Destructured) {
            if (positional == commas) return static_cast<int>(i);
            positional++;
        }
    }
    if (rest >= 0) return rest;
    for (size_t i = 0; i < params.size(); i++) {
        if (params[i].kind == ParamKind::Key || params[i].kind == ParamKind::KeyReq ||
            params[i].kind == ParamKind::KeyRest) {
            return static_cast<int>(i);
        }
    }
    return commas;
}

// ---------------------------------------------------------------------------
// Completion helpers

RubyLspKind KindOf(const Def &d) {
    switch (d.kind) {
    case DefKind::Module: return RubyLspKind::Module;
    case DefKind::Class: return RubyLspKind::Class;
    case DefKind::Constant: return RubyLspKind::Constant;
    case DefKind::Attr: return RubyLspKind::Property;
    case DefKind::Alias:
    case DefKind::Method: return d.owner.empty() && !d.builtin ? RubyLspKind::Function : RubyLspKind::Method;
    }
    return RubyLspKind::Text;
}

std::string DetailOf(const Def &d, const std::string &as_name = std::string()) {
    switch (d.kind) {
    case DefKind::Module: return "module " + d.fqn;
    case DefKind::Class: return "class " + d.fqn + (d.superclass.empty() ? std::string() : " < " + d.superclass);
    case DefKind::Constant: return d.value.empty() ? d.fqn : d.fqn + " = " + d.value;
    case DefKind::Attr: {
        const std::string rt = ReturnTypes(d);
        return OwnerLabel(d) + "#" + d.name + (rt.empty() ? std::string() : " -> " + rt);
    }
    default: {
        const std::string rt = ReturnTypes(d);
        Def copy = d;
        if (!as_name.empty()) copy.name = as_name;
        return copy.Signature() + (rt.empty() ? std::string() : " -> " + rt) + "  (" + OwnerLabel(d) + ")";
    }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API

void RubyLspSetWorkspaceRoot(const std::string &root) { SetWorkspaceRoot(root); }
void RubyLspSetLoadPath(const std::vector<std::string> *dirs) { SetLoadPathOverride(dirs); }

std::shared_ptr<const RubyLspDocument> RubyLspAnalyze(const std::vector<std::string> &lines, const std::string &path) {
    auto doc = std::make_shared<RubyLspDocument>();
    doc->lines = lines.empty() ? std::vector<std::string>{std::string()} : lines;
    doc->path = path;
    std::string text;
    for (size_t i = 0; i < doc->lines.size(); i++) {
        doc->line_start.push_back(text.size());
        text += doc->lines[i];
        if (i + 1 < doc->lines.size()) text += '\n';
    }
    doc->tree = Parse(std::move(text));
    doc->index = IndexTree(*doc->tree, path);
    doc->extra = doc->index->load_path;
    doc->world.Add(Builtins());
    doc->world.Add(doc->index);
    // Required files, transitively (breadth first, bounded).
    std::vector<std::shared_ptr<const FileIndex>> queue{doc->index};
    std::set<std::string> seen;
    if (!path.empty()) seen.insert(path);
    size_t qi = 0;
    while (qi < queue.size() && doc->world.Files().size() < 400) {
        const auto f = queue[qi++];
        std::vector<std::string> extra = doc->extra;
        for (const std::string &dir : f->load_path) {
            if (std::find(extra.begin(), extra.end(), dir) == extra.end()) extra.push_back(dir);
        }
        for (const Require &r : f->reqs) {
            const std::string p = ResolveRequire(r, f->path, extra);
            if (p.empty() || !seen.insert(p).second) continue;
            auto idx = IndexFile(p);
            if (!idx) continue;
            doc->world.Add(idx);
            queue.push_back(idx);
        }
    }
    return doc;
}

std::vector<RubyLspDiagnostic> RubyLspDiagnostics(const RubyLspDocument &doc) {
    std::vector<RubyLspDiagnostic> out;
    const std::string &src = doc.text();
    std::function<void(TSNode)> walk = [&](TSNode n) {
        if (out.size() >= 50) return;
        if (ts_node_is_missing(n)) {
            RubyLspDiagnostic d;
            d.range = RangeOf(n);
            d.code = "syntax-error";
            d.message = std::string("syntax error: missing `") + ts_node_type(n) + "`";
            out.push_back(d);
            return;
        }
        if (ts_node_is_error(n)) {
            RubyLspDiagnostic d;
            d.range = RangeOf(n);
            // Point at the first token, not the whole damaged region.
            TSNode first = n;
            while (ts_node_child_count(first) > 0) first = ts_node_child(first, 0);
            std::string tok = Text(src, first);
            const size_t nl = tok.find('\n');
            if (nl != std::string::npos) tok = tok.substr(0, nl);
            if (tok.size() > 20) tok = tok.substr(0, 20) + "...";
            if (d.range.end_line > d.range.line) {
                d.range = RangeOf(first);
                if (d.range.end_line > d.range.line) {
                    d.range.end_line = d.range.line;
                    d.range.end_col = static_cast<int>(doc.lines[static_cast<size_t>(d.range.line)].size());
                }
            }
            d.code = "syntax-error";
            d.message = tok.empty() ? "syntax error" : "syntax error, unexpected `" + tok + "`";
            // A block opener whose damage runs to the end of the file: it was never closed.
            static const std::set<std::string> kOpeners = {"class", "module", "def", "do", "if", "unless",
                                                            "while", "until", "case", "begin", "for"};
            size_t last = src.size();
            while (last > 0 && std::isspace(static_cast<unsigned char>(src[last - 1]))) last--;
            if (kOpeners.count(tok) && ts_node_end_byte(n) >= last) {
                d.message = "syntax error: `" + tok + "` without a matching `end`";
            }
            out.push_back(d);
            return;
        }
        if (!ts_node_has_error(n)) return;
        const uint32_t k = ts_node_child_count(n);
        for (uint32_t i = 0; i < k; i++) walk(ts_node_child(n, i));
    };
    walk(doc.tree->Root());
    // `require_relative` of a file that is not there.
    if (!doc.path.empty()) {
        const fs::path dir = fs::path(doc.path).parent_path();
        for (const Require &r : doc.index->reqs) {
            if (!r.relative || r.conditional) continue;
            const std::string base = (dir / r.name).lexically_normal().string();
            bool exists = false;
            for (const char *ext : {"", ".rb", ".so", ".bundle", ".dll"}) {
                std::error_code ec;
                if (fs::is_regular_file(base + ext, ec)) exists = true;
            }
            if (exists) continue;
            RubyLspDiagnostic d;
            d.range = r.range;
            d.severity = RubyLspSeverity::Warning;
            d.code = "missing-require";
            d.message = "cannot find `" + r.name + "` relative to this file";
            out.push_back(d);
        }
    }
    return out;
}

std::string RubyLspFormatDiagnostic(const std::string &path, const RubyLspDiagnostic &d) {
    const char *sev = d.severity == RubyLspSeverity::Error         ? "error"
                      : d.severity == RubyLspSeverity::Warning     ? "warning"
                      : d.severity == RubyLspSeverity::Information ? "info"
                                                                     : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool RubyLspDiagnosticFails(const RubyLspDiagnostic &d) {
    return d.severity == RubyLspSeverity::Error || d.severity == RubyLspSeverity::Warning;
}

// --- Completion --------------------------------------------------------------------

namespace {
std::vector<RubyLspCompletionItem> Complete(const RubyLspDocument &doc, const std::string &cur, int line, int col);
}  // namespace

std::vector<RubyLspCompletionItem> RubyLspCompletions(const RubyLspDocument &doc, int line, int col) {
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return {};
    const std::string &cur = doc.lines[static_cast<size_t>(line)];
    col = std::max(0, std::min(col, static_cast<int>(cur.size())));
    if (!ts_node_has_error(doc.tree->Root())) return Complete(doc, cur, line, col);
    // While typing, `frame.` or `Foo::` usually breaks the parse of the whole enclosing class.
    // Analyse the document with what is being typed blanked out (same offsets) instead.
    int start = col;
    while (start > 0 && IdentChar(cur[static_cast<size_t>(start - 1)])) start--;
    if (start > 0 && cur[static_cast<size_t>(start - 1)] == '.') {
        start--;
        if (start > 0 && cur[static_cast<size_t>(start - 1)] == '&') start--;
    } else if (start > 1 && cur[static_cast<size_t>(start - 1)] == ':' && cur[static_cast<size_t>(start - 2)] == ':') {
        start -= 2;
    }
    std::vector<std::string> repaired = doc.lines;
    for (int i = start; i < col; i++) repaired[static_cast<size_t>(line)][static_cast<size_t>(i)] = ' ';
    const auto fixed = RubyLspAnalyze(repaired, doc.path);
    return Complete(*fixed, cur, line, col);
}

namespace {
std::vector<RubyLspCompletionItem> Complete(const RubyLspDocument &doc, const std::string &cur, int line, int col) {
    std::vector<RubyLspCompletionItem> out;
    const uint32_t at = doc.Offset(line, col);
    int start = col;
    while (start > 0 && IdentChar(cur[static_cast<size_t>(start - 1)])) start--;
    const std::string prefix = cur.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    std::set<std::string> seen;
    const Infer inf{doc};

    auto add = [&](const std::string &label, RubyLspKind kind, const std::string &detail, const std::string &docmd) {
        if (!StartsWith(label, prefix) || !seen.insert(label).second) return;
        RubyLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = docmd;
        it.replace_start = start;
        it.replace_end = col;
        out.push_back(it);
    };
    auto add_def = [&](const Def &d, const std::string &as_name = std::string()) {
        const std::string label = as_name.empty() ? d.name : as_name;
        if (!StartsWith(label, prefix) || seen.count(label)) return;
        add(label, KindOf(d), DetailOf(d, as_name), DefMarkdown(doc, d, as_name));
    };

    // Inside `require '...'`: files on the load path.
    {
        const std::string before = cur.substr(0, static_cast<size_t>(col));
        for (const char *kw : {"require_relative", "require"}) {
            const size_t k = before.rfind(kw);
            if (k == std::string::npos) continue;
            const std::string rest = before.substr(k + std::string(kw).size());
            size_t p = 0;
            while (p < rest.size() && (rest[p] == ' ' || rest[p] == '(')) p++;
            if (p >= rest.size() || (rest[p] != '\'' && rest[p] != '"')) continue;
            const std::string typed = rest.substr(p + 1);
            if (typed.find_first_of("'\"") != std::string::npos) continue;
            const bool relative = std::string(kw) == "require_relative";
            const size_t slash = typed.rfind('/');
            const std::string dir_part = slash == std::string::npos ? std::string() : typed.substr(0, slash + 1);
            const std::string file_part = slash == std::string::npos ? typed : typed.substr(slash + 1);
            std::vector<std::string> roots;
            if (relative) {
                if (!doc.path.empty()) roots.push_back(fs::path(doc.path).parent_path().string());
            } else {
                roots = LoadPath(doc.path, doc.extra);
            }
            std::set<std::string> names;
            for (const std::string &r : roots) {
                std::error_code ec;
                for (fs::directory_iterator it(fs::path(r) / dir_part, ec), end; !ec && it != end; it.increment(ec)) {
                    const std::string fn = it->path().filename().string();
                    if (fn.empty() || fn[0] == '.') continue;
                    std::error_code ec2;
                    if (it->is_directory(ec2)) {
                        names.insert(fn + "/");
                    } else if (fn.size() > 3 && fn.substr(fn.size() - 3) == ".rb") {
                        const std::string stem = fn.substr(0, fn.size() - 3);
                        if (!(relative && !doc.path.empty() && (fs::path(r) / dir_part / fn) == fs::path(doc.path))) {
                            names.insert(stem);
                        }
                    }
                }
            }
            const int fstart = col - static_cast<int>(file_part.size());
            for (const std::string &nm : names) {
                if (!StartsWith(nm, file_part)) continue;
                RubyLspCompletionItem it;
                it.label = nm;
                it.insert_text = nm;
                it.kind = nm.back() == '/' ? RubyLspKind::Module : RubyLspKind::Text;
                it.detail = relative ? "relative file" : "library";
                it.replace_start = fstart;
                it.replace_end = col;
                out.push_back(it);
            }
            return out;
        }
    }

    const char before = start > 0 ? cur[static_cast<size_t>(start - 1)] : '\0';
    // Instance variables of the class.
    if (before == '@') {
        const int s2 = start - 1 - (start >= 2 && cur[static_cast<size_t>(start - 2)] == '@' ? 1 : 0);
        TSNode cls{};
        for (TSNode n = SmallestAt(doc.tree->Root(), at); !ts_node_is_null(n); n = ts_node_parent(n)) {
            if (Is(n, "class") || Is(n, "module")) {
                cls = n;
                break;
            }
        }
        std::set<std::string> ivars;
        std::function<void(TSNode)> walk = [&](TSNode n) {
            if (Is(n, "instance_variable") || Is(n, "class_variable")) ivars.insert(Text(doc.text(), n));
            const uint32_t k = ts_node_named_child_count(n);
            for (uint32_t i = 0; i < k; i++) walk(ts_node_named_child(n, i));
        };
        walk(ts_node_is_null(cls) ? doc.tree->Root() : cls);
        const std::string typed = cur.substr(static_cast<size_t>(s2), static_cast<size_t>(col - s2));
        for (const std::string &v : ivars) {
            if (!StartsWith(v, typed) || v == typed) continue;
            RubyLspCompletionItem it;
            it.label = v;
            it.insert_text = v;
            it.kind = RubyLspKind::Field;
            it.detail = TypeLabel(inf.IvarType(v, at, 0));
            it.replace_start = s2;
            it.replace_end = col;
            out.push_back(it);
        }
        return out;
    }
    // `recv.` / `recv&.`: the receiver type's methods.
    if (before == '.' && !(start >= 2 && cur[static_cast<size_t>(start - 2)] == '.')) {
        size_t dot = static_cast<size_t>(start - 1);
        if (dot > 0 && cur[dot - 1] == '&') dot--;
        const std::string recv_text = ReceiverText(cur, dot);
        if (recv_text.empty()) return out;
        Type recv;
        if (recv_text == "self") {
            recv = inf.SelfType(at);
        } else {
            auto snippet = Parse(recv_text);
            TSNode e = SnippetExpr(*snippet);
            if (!ts_node_is_null(e)) recv = inf.Expr(snippet->text, e, at, 0);
        }
        if (recv.Known()) {
            const bool self_recv = recv_text == "self";
            if (recv.meta) {
                // `Foo.new`: initialize's parameters and docs.
                const std::vector<const Def *> init = inf.Lookup(recv, "new");
                if (!init.empty()) add_def(*init.front(), init.front()->name == "initialize" ? "new" : std::string());
            }
            for (const Def *d : doc.world.AllMethods(recv.name, recv.meta)) {
                if (!self_recv && d->visibility == "private") continue;
                if (d->name == "initialize" || !IdentChar(d->name[0])) continue;
                add_def(*d);
            }
        } else {
            // Unknown receiver: every public method defined in the files the document sees.
            for (const auto &f : doc.world.Files()) {
                if (f->path == "<builtin>") continue;
                for (const auto &d : f->defs) {
                    if ((d->kind == DefKind::Method || d->kind == DefKind::Attr || d->kind == DefKind::Alias) &&
                        d->visibility != "private" && d->name != "initialize" && !d->owner.empty()) {
                        add_def(*d);
                    }
                }
            }
        }
        return out;
    }
    // `Foo::`: constants and class methods.
    if (before == ':' && start >= 2 && cur[static_cast<size_t>(start - 2)] == ':') {
        const std::string ns_text = ReceiverText(cur, static_cast<size_t>(start - 2));
        const Context c = ContextAt(doc, at);
        const std::string fqn = ns_text.empty() ? std::string() : doc.world.ResolveConst(ns_text, c.nesting);
        if (fqn.empty() && !ns_text.empty()) return out;
        for (const Def *d : doc.world.Members(fqn)) add_def(*d);
        if (!fqn.empty()) {
            for (const Def *d : doc.world.AllMethods(fqn, true)) {
                if (d->builtin || d->visibility == "private") continue;
                add_def(*d);
            }
        }
        return out;
    }
    if (before == ':' || before == '$') return out;  // symbols, globals
    // A bare name: locals, methods of self, constants in scope, keywords.
    const std::vector<Local> locals = LocalsAt(doc, at);
    const uint32_t word_start = doc.Offset(line, start);
    for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
        if (ts_node_start_byte(it->def) == word_start) continue;  // the word being typed
        if (seen.count(it->name)) continue;
        const Type t = inf.LocalType(*it, at, 0);
        add(it->name, RubyLspKind::Variable, TypeLabel(t), LocalMarkdown(*it, t, inf));
    }
    const Context c = ContextAt(doc, at);
    for (const Def *d : doc.world.AllMethods(c.self_class, c.self_meta)) {
        if (d->name == "initialize" || !IdentChar(d->name[0]) || d->name.back() == '=') continue;
        add_def(*d);
    }
    for (const Def *d : doc.world.TopLevelMethods()) add_def(*d);
    for (const Def *d : doc.world.AllMethods("Object", false)) {
        if (IdentChar(d->name[0]) && d->name.back() != '=') add_def(*d);
    }
    for (size_t i = c.nesting.size() + 1; i-- > 0;) {
        for (const Def *d : doc.world.Members(i == 0 ? std::string() : c.nesting[i - 1])) add_def(*d);
    }
    for (const std::string &k : Keywords()) add(k, RubyLspKind::Keyword, "keyword", std::string());
    return out;
}
}  // namespace

// --- Hover --------------------------------------------------------------------------

RubyLspHoverInfo RubyLspHover(const RubyLspDocument &doc, int line, int col) {
    RubyLspHoverInfo h;
    const Target tg = TargetAt(doc, line, col);
    if (tg.kind == Target::None) return h;
    h.range = RangeOf(tg.node);
    const Infer inf{doc};
    switch (tg.kind) {
    case Target::Method:
    case Target::Const: {
        if (tg.defs.empty()) return h;
        std::string s;
        if (tg.kind == Target::Const) {
            // Reopened classes: the documented definition.
            const Def *best = tg.defs.front();
            for (const Def *d : tg.defs) {
                if (d->doc && (!best->doc || best->doc->Empty())) best = d;
            }
            s = DefMarkdown(doc, *best);
        } else {
            std::set<std::string> owners;
            for (const Def *d : tg.defs) {
                if (!owners.insert(d->owner + (d->singleton ? "." : "#")).second) continue;
                if (!s.empty()) s += "\n\n---\n\n";
                s += DefMarkdown(doc, *d, tg.as_name);
                if (owners.size() >= 4) break;
            }
        }
        h.found = true;
        h.text = s;
        return h;
    }
    case Target::LocalVar:
        h.found = true;
        h.text = LocalMarkdown(tg.local, tg.type, inf);
        return h;
    case Target::Ivar: {
        h.found = true;
        std::string code = tg.name;
        if (tg.type.Known()) code += ": " + TypeLabel(tg.type);
        h.text = "```ruby\n" + code + "\n```\n*instance variable*";
        if (!tg.defs.empty() && tg.defs.front()->doc) {
            const std::string md = YardMarkdown(*tg.defs.front()->doc);
            if (!md.empty()) h.text += "\n\n" + md;
        }
        return h;
    }
    case Target::Self:
        h.found = true;
        h.text = "```ruby\nself: " + TypeLabel(tg.type) + "\n```";
        return h;
    case Target::Require:
        h.found = true;
        h.text = tg.require_path.empty() ? "*not found on the load path*" : "`" + tg.require_path + "`";
        return h;
    case Target::None:
        break;
    }
    return h;
}

// --- Signature help ------------------------------------------------------------------

RubyLspSignatureHelp RubyLspSignatureAt(const RubyLspDocument &doc, int line, int col) {
    RubyLspSignatureHelp sh;
    const CallSite cs = FindCall(doc, line, col);
    if (!cs.found) return sh;
    const Infer inf{doc};
    const uint32_t at = doc.Offset(line, col);
    Type recv;
    if (cs.receiver.empty() || cs.receiver == "self") {
        recv = inf.SelfType(at);
    } else {
        auto snippet = Parse(cs.receiver);
        TSNode e = SnippetExpr(*snippet);
        if (!ts_node_is_null(e)) recv = inf.Expr(snippet->text, e, at, 0);
    }
    std::vector<const Def *> defs = inf.Lookup(recv, cs.name);
    std::string as_name;
    if (recv.meta && cs.name == "new" && !defs.empty() && defs.front()->name == "initialize") as_name = "new";
    if (defs.empty() && cs.receiver.empty()) {
        for (const Def *d : doc.world.TopLevelMethods()) {
            if (d->name == cs.name) defs.push_back(d);
        }
    }
    if (defs.empty() && !recv.Known() && !cs.receiver.empty()) defs = MethodsNamed(doc.world, cs.name);
    if (defs.empty()) return sh;
    const Def &d = *defs.front();
    const std::string name = as_name.empty() ? d.name : as_name;
    std::vector<std::vector<Param>> param_lists;
    if (d.doc && !d.doc->overloads.empty()) {
        for (const YardOverload &o : d.doc->overloads) {
            const size_t paren = o.signature.find('(');
            const std::string params_text = paren == std::string::npos ? std::string() : o.signature.substr(paren);
            const std::vector<Param> ps = ParseParamList(params_text);
            RubyLspSignature s = MakeSignature(name, ps, o.doc.get());
            // The shared description first, then the overload's own.
            if (!d.doc->text.empty()) s.documentation = d.doc->text + (s.documentation.empty() ? "" : "\n\n" + s.documentation);
            sh.signatures.push_back(s);
            param_lists.push_back(ps);
        }
    } else {
        sh.signatures.push_back(MakeSignature(name, d.params, d.doc.get()));
        param_lists.push_back(d.params);
    }
    // The first overload that takes this many arguments.
    sh.active_signature = 0;
    for (size_t i = 0; i < param_lists.size(); i++) {
        int positional = 0;
        bool rest = false, keyword = false;
        for (const Param &p : param_lists[i]) {
            if (p.kind == ParamKind::Req || p.kind == ParamKind::Opt || p.kind == ParamKind::Destructured) positional++;
            if (p.kind == ParamKind::Rest) rest = true;
            if (p.kind == ParamKind::Key || p.kind == ParamKind::KeyReq || p.kind == ParamKind::KeyRest) keyword = true;
        }
        if ((cs.keyword.empty() && (cs.commas < positional || rest)) || (!cs.keyword.empty() && keyword)) {
            sh.active_signature = static_cast<int>(i);
            break;
        }
    }
    sh.active_param = ActiveParam(param_lists[static_cast<size_t>(sh.active_signature)], cs.commas, cs.keyword);
    sh.found = true;
    return sh;
}

// --- Navigation ------------------------------------------------------------------------

std::vector<RubyLspLocation> RubyLspDefinition(const RubyLspDocument &doc, int line, int col) {
    std::vector<RubyLspLocation> out;
    const Target tg = TargetAt(doc, line, col);
    auto add = [&](const std::string &path, const RubyLspRange &r) {
        RubyLspLocation loc;
        loc.path = path == doc.path ? std::string() : path;
        loc.range = r;
        for (const RubyLspLocation &o : out) {
            if (o.path == loc.path && o.range.line == r.line && o.range.col == r.col) return;
        }
        out.push_back(loc);
    };
    switch (tg.kind) {
    case Target::Method:
    case Target::Const:
    case Target::Ivar:
        for (const Def *d : tg.defs) {
            if (!d->builtin) add(d->path, d->sel);
        }
        if (tg.kind == Target::Ivar && out.empty()) {
            // The first assignment in the class.
            TSNode cls{};
            for (TSNode n = tg.node; !ts_node_is_null(n); n = ts_node_parent(n)) {
                if (Is(n, "class") || Is(n, "module")) {
                    cls = n;
                    break;
                }
            }
            TSNode first{};
            std::function<void(TSNode)> walk = [&](TSNode n) {
                if (!ts_node_is_null(first)) return;
                if (Is(n, "assignment") && Is(Field(n, "left"), "instance_variable") &&
                    Text(doc.text(), Field(n, "left")) == tg.name) {
                    first = Field(n, "left");
                    return;
                }
                const uint32_t k = ts_node_named_child_count(n);
                for (uint32_t i = 0; i < k; i++) walk(ts_node_named_child(n, i));
            };
            if (!ts_node_is_null(cls)) walk(cls);
            if (!ts_node_is_null(first)) add(doc.path, RangeOf(first));
        }
        break;
    case Target::LocalVar:
        add(doc.path, RangeOf(tg.local.def));
        break;
    case Target::Require:
        if (!tg.require_path.empty()) add(tg.require_path, RubyLspRange{});
        break;
    default:
        break;
    }
    return out;
}

namespace {

// Every identifier naming local @p l in its scope.
std::vector<TSNode> LocalOccurrences(const RubyLspDocument &doc, const Local &l) {
    std::vector<TSNode> out;
    const std::string &src = doc.text();
    // A block parameter lives in its block, anything else in the hard scope.
    TSNode scope = l.kind == LocalKind::BlockParam ? l.owner : HardScopeAt(doc, ts_node_start_byte(l.def));
    std::function<void(TSNode, bool)> walk = [&](TSNode n, bool top) {
        if (!top && HardScope(n)) return;
        if (Is(n, "identifier") && Text(src, n) == l.name) {
            TSNode p = ts_node_parent(n);
            const bool not_local = (Is(p, "call") && ts_node_eq(Field(p, "method"), n)) ||
                                   ((Is(p, "method") || Is(p, "singleton_method")) && ts_node_eq(Field(p, "name"), n));
            if (!not_local) out.push_back(n);
        }
        const uint32_t k = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < k; i++) walk(ts_node_named_child(n, i), false);
    };
    walk(scope, true);
    // An inner block that declares a parameter of that name shadows it.
    std::vector<TSNode> filtered;
    for (TSNode n : out) {
        bool shadowed = false;
        for (TSNode p = ts_node_parent(n); !ts_node_is_null(p) && !ts_node_eq(p, scope); p = ts_node_parent(p)) {
            if (!SoftScope(p) || (l.kind == LocalKind::BlockParam && ts_node_eq(p, l.owner))) continue;
            std::vector<Local> ps;
            ParamNames(Field(p, "parameters"), LocalKind::BlockParam, p, src, ps);
            for (const Local &x : ps) shadowed = shadowed || x.name == l.name;
        }
        if (!shadowed) filtered.push_back(n);
    }
    return filtered;
}

bool IsDefinitionSite(TSNode n) {
    TSNode p = ts_node_parent(n);
    if (Is(p, "assignment") || Is(p, "operator_assignment")) return ts_node_eq(Field(p, "left"), n);
    return Is(p, "method_parameters") || Is(p, "block_parameters") || Is(p, "lambda_parameters") ||
           Is(p, "optional_parameter") || Is(p, "keyword_parameter") || Is(p, "splat_parameter") ||
           Is(p, "hash_splat_parameter") || Is(p, "block_parameter") || Is(p, "exception_variable") ||
           Is(p, "left_assignment_list");
}

}  // namespace

std::vector<RubyLspReference> RubyLspReferences(const RubyLspDocument &doc, int line, int col) {
    std::vector<RubyLspReference> out;
    const Target tg = TargetAt(doc, line, col);
    if (tg.kind == Target::LocalVar) {
        for (TSNode n : LocalOccurrences(doc, tg.local)) {
            RubyLspReference r;
            r.range = RangeOf(n);
            r.is_definition = IsDefinitionSite(n);
            out.push_back(r);
        }
        return out;
    }
    if (tg.kind == Target::None) return out;
    // Methods, constants, instance variables: every occurrence of the word in the document.
    TSNode leaf = tg.node;
    if (Is(leaf, "scope_resolution")) leaf = Field(leaf, "name");
    const std::string_view want = NodeType(leaf);
    const std::string name = Text(doc.text(), leaf);
    if (name.empty()) return out;
    std::function<void(TSNode)> walk = [&](TSNode n) {
        if (NodeType(n) == want && Text(doc.text(), n) == name) {
            RubyLspReference r;
            r.range = RangeOf(n);
            TSNode p = ts_node_parent(n);
            r.is_definition = ((Is(p, "method") || Is(p, "singleton_method") || Is(p, "class") || Is(p, "module")) &&
                               ts_node_eq(Field(p, "name"), n)) ||
                              (Is(p, "assignment") && ts_node_eq(Field(p, "left"), n));
            out.push_back(r);
        }
        const uint32_t k = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < k; i++) walk(ts_node_named_child(n, i));
    };
    walk(doc.tree->Root());
    return out;
}

RubyLspRenameTarget RubyLspPrepareRename(const RubyLspDocument &doc, int line, int col) {
    RubyLspRenameTarget t;
    const Target tg = TargetAt(doc, line, col);
    if (tg.kind != Target::LocalVar) return t;
    t.found = true;
    t.range = RangeOf(tg.node);
    t.placeholder = tg.name;
    return t;
}

std::vector<RubyLspTextEdit> RubyLspRename(const RubyLspDocument &doc, int line, int col, const std::string &new_name,
                                           std::string *error) {
    std::vector<RubyLspTextEdit> edits;
    const Target tg = TargetAt(doc, line, col);
    if (tg.kind != Target::LocalVar) {
        *error = "only local variables and parameters can be renamed";
        return edits;
    }
    bool ok = !new_name.empty() && (std::islower(static_cast<unsigned char>(new_name[0])) || new_name[0] == '_');
    for (char c : new_name) ok = ok && IdentChar(c);
    for (const std::string &k : Keywords()) ok = ok && k != new_name;
    if (!ok) {
        *error = "`" + new_name + "` is not a valid local variable name";
        return edits;
    }
    for (TSNode n : LocalOccurrences(doc, tg.local)) {
        if (Is(ts_node_parent(n), "keyword_parameter")) {
            *error = "keyword parameters are part of the method's interface; rename them by hand";
            return {};
        }
        RubyLspTextEdit e;
        e.range = RangeOf(n);
        e.new_text = new_name;
        edits.push_back(e);
    }
    return edits;
}

// --- Outline / folding -----------------------------------------------------------------------

std::vector<RubyLspSymbol> RubyLspSymbols(const RubyLspDocument &doc) {
    std::vector<RubyLspSymbol> out;
    std::vector<const Def *> defs;
    std::set<std::string> seen;
    for (const auto &d : doc.index->defs) {
        // module_function's private copy, attr_accessor's writer: once.
        const std::string key = std::to_string(d->sel.line) + ":" + std::to_string(d->sel.col) + ":" + d->name + ":" +
                                std::to_string(static_cast<int>(d->kind));
        if (!seen.insert(key).second) continue;
        defs.push_back(d.get());
    }
    std::stable_sort(defs.begin(), defs.end(), [](const Def *a, const Def *b) {
        if (a->range.line != b->range.line) return a->range.line < b->range.line;
        return a->range.col < b->range.col;
    });
    auto inside = [](const RubyLspRange &inner, const RubyLspRange &outer) {
        auto le = [](int l1, int c1, int l2, int c2) { return l1 < l2 || (l1 == l2 && c1 <= c2); };
        return le(outer.line, outer.col, inner.line, inner.col) &&
               le(inner.end_line, inner.end_col, outer.end_line, outer.end_col);
    };
    std::vector<int> ns_stack;
    for (const Def *d : defs) {
        RubyLspSymbol s;
        s.name = d->name;
        s.range = d->range;
        s.sel_range = d->sel;
        switch (d->kind) {
        case DefKind::Module:
            s.kind = RubyLspSymbolKind::Module;
            break;
        case DefKind::Class:
            s.kind = RubyLspSymbolKind::Class;
            if (!d->superclass.empty()) s.detail = "< " + d->superclass;
            break;
        case DefKind::Method:
        case DefKind::Alias: {
            s.kind = d->owner.empty() ? RubyLspSymbolKind::Function : RubyLspSymbolKind::Method;
            if (d->singleton) s.name = "self." + d->name;
            Def copy = *d;
            copy.name.clear();
            s.detail = d->kind == DefKind::Alias ? "alias of " + d->alias_of : copy.Signature();
            break;
        }
        case DefKind::Attr:
            s.kind = RubyLspSymbolKind::Property;
            s.detail = d->attr == "rw" ? "attr_accessor" : d->attr == "w" ? "attr_writer" : "attr_reader";
            break;
        case DefKind::Constant:
            s.kind = RubyLspSymbolKind::Constant;
            s.detail = d->value;
            break;
        }
        while (!ns_stack.empty() && !inside(d->range, out[static_cast<size_t>(ns_stack.back())].range)) ns_stack.pop_back();
        s.parent = ns_stack.empty() ? -1 : ns_stack.back();
        out.push_back(s);
        if (d->kind == DefKind::Module || d->kind == DefKind::Class) ns_stack.push_back(static_cast<int>(out.size()) - 1);
    }
    return out;
}

std::vector<RubyLspFold> RubyLspFolds(const RubyLspDocument &doc) {
    std::vector<RubyLspFold> out;
    static const std::set<std::string> kFold = {
        "module", "class", "singleton_class", "method", "singleton_method", "do_block", "block", "if", "unless",
        "case", "case_match", "while", "until", "for", "begin", "hash", "array", "argument_list", "heredoc_body",
        "lambda", "when", "elsif", "else", "rescue", "ensure", "string_array", "symbol_array"};
    std::set<int> starts;
    std::function<void(TSNode)> walk = [&](TSNode n) {
        const std::string t(NodeType(n));
        const TSPoint s = ts_node_start_point(n), e = ts_node_end_point(n);
        if (kFold.count(t) && e.row > s.row && e.row < doc.lines.size()) {
            int end = static_cast<int>(e.row);
            // Keep a closing `end`/`}`/`]` line visible.
            const std::string lt = Trim(doc.lines[e.row]);
            if (t != "heredoc_body" && !lt.empty() &&
                (StartsWith(lt, "end") || lt[0] == '}' || lt[0] == ']' || lt[0] == ')')) {
                end--;
            }
            if (t == "when" || t == "elsif" || t == "else" || t == "rescue" || t == "ensure") {
                TSNode next = ts_node_next_named_sibling(n);
                if (!ts_node_is_null(next)) end = std::min(end, static_cast<int>(ts_node_start_point(next).row) - 1);
            }
            if (end > static_cast<int>(s.row) && starts.insert(static_cast<int>(s.row)).second) {
                out.push_back(RubyLspFold{static_cast<int>(s.row), end, ""});
            }
        }
        const uint32_t k = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < k; i++) walk(ts_node_named_child(n, i));
    };
    walk(doc.tree->Root());
    // Runs of whole-line comments.
    int run_start = -1;
    for (size_t i = 0; i <= doc.lines.size(); i++) {
        const bool c = i < doc.lines.size() && StartsWith(Trim(doc.lines[i]), "#");
        if (c && run_start < 0) run_start = static_cast<int>(i);
        if (!c && run_start >= 0) {
            if (static_cast<int>(i) - 1 > run_start && !starts.count(run_start)) {
                out.push_back(RubyLspFold{run_start, static_cast<int>(i) - 1, "comment"});
            }
            run_start = -1;
        }
    }
    std::sort(out.begin(), out.end(), [](const RubyLspFold &a, const RubyLspFold &b) { return a.start_line < b.start_line; });
    return out;
}

// --- Documentation by name ---------------------------------------------------------------------

std::string RubyLspDocFor(const RubyLspDocument &doc, const std::string &raw) {
    const std::string name = Trim(raw);
    if (name.empty()) return std::string();
    const World &w = doc.world;
    const Infer inf{doc};
    // `Foo#bar`, `Foo.bar`
    size_t sep = name.rfind('#');
    bool singleton = false;
    if (sep == std::string::npos) {
        sep = name.rfind('.');
        singleton = sep != std::string::npos;
    }
    if (sep != std::string::npos && sep > 0 && sep + 1 < name.size()) {
        const std::string cls = name.substr(0, sep);
        const std::string meth = name.substr(sep + 1);
        std::string fqn = w.ResolveConst(cls, {});
        if (fqn.empty()) fqn = cls;
        std::vector<const Def *> defs = inf.Lookup(Type{fqn, singleton, ""}, meth);
        // An instance method asked for with `.` (or the other way round).
        if (defs.empty()) defs = inf.Lookup(Type{fqn, !singleton, ""}, meth);
        if (defs.empty()) return std::string();
        const std::string as_name = singleton && meth == "new" && defs.front()->name == "initialize" ? "new" : "";
        return DefMarkdown(doc, *defs.front(), as_name);
    }
    if (std::isupper(static_cast<unsigned char>(name[0])) || StartsWith(name, "::")) {
        const std::string fqn = w.ResolveConst(name, {});
        if (fqn.empty()) {
            // `Foo::bar`, a method written with `::`.
            const size_t k = name.rfind("::");
            if (k != std::string::npos && k + 2 < name.size() && std::islower(static_cast<unsigned char>(name[k + 2]))) {
                return RubyLspDocFor(doc, name.substr(0, k) + "." + name.substr(k + 2));
            }
            // A class known by its last segment only.
            for (const auto &f : w.Files()) {
                for (const auto &d : f->defs) {
                    if ((d->kind == DefKind::Class || d->kind == DefKind::Module) && d->name == name) {
                        return RubyLspDocFor(doc, d->fqn);
                    }
                }
            }
            return std::string();
        }
        const std::vector<const Def *> defs = w.IsNamespace(fqn) ? w.Namespace(fqn) : w.Constants(fqn);
        if (defs.empty()) return std::string();
        const Def *best = defs.front();
        for (const Def *d : defs) {
            if (d->doc && (!best->doc || best->doc->Empty())) best = d;
        }
        std::string s = DefMarkdown(doc, *best);
        if (w.IsNamespace(fqn)) {
            std::vector<std::string> names;
            for (const auto &f : w.Files()) {
                for (const auto &d : f->defs) {
                    if ((d->kind == DefKind::Method || d->kind == DefKind::Attr) && d->owner == fqn &&
                        d->visibility != "private" && d->name != "initialize") {
                        const std::string label = (d->singleton ? "." : "#") + d->name;
                        if (std::find(names.begin(), names.end(), label) == names.end()) names.push_back(label);
                    }
                }
            }
            if (!names.empty()) {
                s += "\n\n**Methods**\n";
                for (const std::string &n : names) s += "\n- `" + n + "`";
            }
        }
        return s;
    }
    // A bare method name: user-defined first, then Ruby core.
    std::vector<const Def *> defs;
    for (const Def *d : w.TopLevelMethods()) {
        if (d->name == name) defs.push_back(d);
    }
    for (const Def *d : MethodsNamed(w, name)) {
        if (std::find(defs.begin(), defs.end(), d) == defs.end()) defs.push_back(d);
    }
    if (defs.empty()) {
        for (const auto &d : Builtins()->defs) {
            if (d->kind == DefKind::Method && d->name == name) defs.push_back(d.get());
        }
    }
    if (defs.empty()) return std::string();
    std::string s;
    for (size_t i = 0; i < defs.size() && i < 4; i++) {
        if (i) s += "\n\n---\n\n";
        s += DefMarkdown(doc, *defs[i]);
    }
    return s;
}

// --- Formatting ------------------------------------------------------------------------------

std::vector<RubyLspTextEdit> RubyLspFormat(const std::vector<std::string> &lines, int width, std::string *error) {
    std::vector<RubyLspTextEdit> edits;
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    rubyfmt::Options o;
    if (width > 0) o.width = width;
    const rubyfmt::Result r = rubyfmt::Format(text, o);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    if (r.text == text) return edits;
    std::vector<std::string> nl;
    {
        std::string cur;
        for (char c : r.text) {
            if (c == '\n') {
                nl.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        nl.push_back(cur);
    }
    // One edit over the changed lines: [a, lines.size() - b) becomes nl[a, nl.size() - b).
    size_t a = 0;
    while (a < lines.size() && a < nl.size() && lines[a] == nl[a]) a++;
    size_t b = 0;
    while (b < lines.size() - a && b < nl.size() - a && lines[lines.size() - 1 - b] == nl[nl.size() - 1 - b]) b++;
    RubyLspTextEdit e;
    if (a == lines.size()) {
        // Only appended lines.
        e.range.line = e.range.end_line = static_cast<int>(lines.size()) - 1;
        e.range.col = e.range.end_col = static_cast<int>(lines.back().size());
        for (size_t i = a; i < nl.size(); i++) e.new_text += "\n" + nl[i];
    } else if (b == 0) {
        e.range.line = static_cast<int>(a);
        e.range.end_line = static_cast<int>(lines.size()) - 1;
        e.range.end_col = static_cast<int>(lines.back().size());
        for (size_t i = a; i < nl.size(); i++) e.new_text += nl[i] + (i + 1 < nl.size() ? "\n" : "");
    } else {
        e.range.line = static_cast<int>(a);
        e.range.end_line = static_cast<int>(lines.size() - b);
        for (size_t i = a; i < nl.size() - b; i++) e.new_text += nl[i] + "\n";
    }
    edits.push_back(e);
    return edits;
}
