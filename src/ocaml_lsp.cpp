// mep's OCaml language server, analysis half: see ocaml_lsp.h.
//
// A document is parsed with tree-sitter (ocaml / ocaml_interface), indexed into
// its items (ocaml_lsp_index.cpp), and walked once for its value bindings and
// their scopes (`let ... in`, function parameters, match cases, `for`), its
// value references, and its `open`s (top-level, `let open`, `M.( ... )`).
// Unqualified names resolve to the innermost binding, then the opened modules,
// then Stdlib; qualified names go through the module resolution of
// ocaml_lsp_index.cpp (workspace, stdlib, library directories).

#include "ocaml_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>
#include <sstream>

#include "ocaml_format.h"
#include "ocaml_lsp_internal.h"

using namespace mllsp;

namespace {

struct Binding {
    std::string name;
    OcamlLspRange sel;
    uint32_t def_byte = 0;
    uint32_t scope_start = 0, scope_end = 0;
    bool top_level = false;
    bool let_in = false;
    bool label_pun = false;  // `~x` / `?x` parameter: the name is also the label
    std::string display;     // hover: `let f x y`, `(parameter) x`
    std::vector<std::string> params;  // a let-bound function's parameters
    int refs = 0;
};

struct Ref {
    std::string name;
    OcamlLspRange range;
    uint32_t byte = 0;
    int binding = -1;
    bool label_pun = false;  // `~x` argument
    bool field_pun = false;  // `{ x }` record field
};

struct QRef {  // `M.N.x`
    std::vector<std::string> path;
    std::string name;
    OcamlLspRange range;  // of the value name
    uint32_t byte = 0;
};

struct Open {
    std::vector<std::string> path;
    uint32_t at = 0;              // where the `open` itself is
    uint32_t start = 0, end = 0;  // its scope
};

}  // namespace

class OcamlLspDocument {
public:
    std::vector<std::string> lines;
    std::string path;
    bool interface = false;
    std::unique_ptr<Tree> tree;
    std::vector<size_t> line_starts;
    Env env;
    std::shared_ptr<Module> module;
    std::shared_ptr<const Module> twin;  // foo.mli beside foo.ml
    std::vector<Binding> bindings;
    std::vector<Ref> refs;
    std::vector<QRef> qrefs;
    std::vector<Open> opens;

    const std::string &src() const { return tree->src; }
    Scope scope() const { return Scope{&env, module}; }

    uint32_t Offset(int line, int col) const {
        if (line < 0) return 0;
        if (static_cast<size_t>(line) >= line_starts.size()) return static_cast<uint32_t>(src().size());
        const size_t ls = line_starts[static_cast<size_t>(line)];
        const size_t len = lines[static_cast<size_t>(line)].size();
        return static_cast<uint32_t>(ls + std::min(static_cast<size_t>(std::max(col, 0)), len));
    }
};

namespace {

namespace fs = std::filesystem;

bool IsNull(TSNode n) { return ts_node_is_null(n); }

bool IsIdentType(const std::string &t) {
    return t == "value_name" || t == "value_pattern" || t == "module_name" || t == "module_type_name" ||
           t == "type_constructor" || t == "constructor_name" || t == "field_name" || t == "class_name" ||
           t == "method_name" || t == "label_name";
}

void PatternNodes(TSNode n, std::vector<TSNode> *out) {
    if (IsNull(n)) return;
    const std::string t = Type(n);
    if (t == "value_name" || t == "value_pattern") {
        out->push_back(n);
        return;
    }
    TSNode def = t == "parameter" ? Field(n, "default") : TSNode{};
    const uint32_t count = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < count; i++) {
        TSNode c = ts_node_named_child(n, i);
        const std::string ct = Type(c);
        if (ct == "constructor_path" || ct == "field_path" || ct == "module_path" || ct == "type_constructor_path" ||
            ct.find("_type") != std::string::npos || ct == "attribute" || ct == "item_attribute")
            continue;
        if (!IsNull(def) && ts_node_eq(def, c)) continue;
        PatternNodes(c, out);
    }
}

std::string ModuleNameOf(const std::string &path) {
    if (path.empty()) return std::string();
    std::string n = fs::path(path).stem().string();
    if (!n.empty()) n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
    return n;
}

class Walker {
public:
    explicit Walker(OcamlLspDocument *d) : d_(d), src_(d->src()) {}

    void Run() {
        TSNode root = d_->tree->Root();
        Walk(root);
        Resolve();
    }

private:
    OcamlLspDocument *d_;
    const std::string &src_;

    void AddBinding(TSNode name, uint32_t start, uint32_t end, const std::string &display, bool top, bool let_in) {
        Binding b;
        b.name = std::string(Text(name, src_));
        if (b.name.empty() || b.name == "_") return;
        b.sel = RangeOf(name);
        b.def_byte = ts_node_start_byte(name);
        b.scope_start = start;
        b.scope_end = end;
        b.top_level = top;
        b.let_in = let_in;
        b.display = display;
        d_->bindings.push_back(b);
    }

    void Params(TSNode owner, uint32_t end) {
        const uint32_t count = ts_node_named_child_count(owner);
        for (uint32_t i = 0; i < count; i++) {
            TSNode p = ts_node_named_child(owner, i);
            if (Type(p) != "parameter") continue;
            std::vector<TSNode> names;
            PatternNodes(Field(p, "pattern"), &names);
            const bool labelled = HasToken(p, "~") || HasToken(p, "?");
            TSNode label = ChildOfType(p, "label_name");
            if (names.empty() && !IsNull(label)) names.push_back(label);
            const std::string disp = "(parameter) " + Collapse(Text(p, src_));
            for (TSNode n : names) {
                const size_t before = d_->bindings.size();
                AddBinding(n, ts_node_start_byte(p), end, disp, false, false);
                // `~x`, `?x`, `?(x = 1)`, `~(x : int)`: the variable is also the label.
                if (labelled && d_->bindings.size() > before && (IsNull(label) || ts_node_eq(label, n)))
                    d_->bindings.back().label_pun = true;
            }
        }
    }

    void ValueDefinition(TSNode vd, uint32_t start, uint32_t end, bool top, bool let_in) {
        const uint32_t count = ts_node_named_child_count(vd);
        for (uint32_t i = 0; i < count; i++) {
            TSNode b = ts_node_named_child(vd, i);
            if (Type(b) != "let_binding") continue;
            std::vector<TSNode> names;
            PatternNodes(Field(b, "pattern"), &names);
            TSNode body = Field(b, "body");
            std::string head;
            {
                const uint32_t s = ts_node_start_byte(b);
                const uint32_t e = IsNull(body) ? ts_node_end_byte(b) : ts_node_start_byte(body);
                head = Collapse(std::string_view(src_).substr(s, e - s));
                while (!head.empty() && (head.back() == '=' || head.back() == ' ')) head.pop_back();
            }
            std::vector<std::string> params;
            const uint32_t nc = ts_node_named_child_count(b);
            for (uint32_t k = 0; k < nc; k++) {
                TSNode p = ts_node_named_child(b, k);
                if (Type(p) == "parameter") params.push_back(Collapse(Text(p, src_)));
            }
            for (TSNode n : names) {
                const std::string disp = names.size() == 1 ? "let " + head : "let " + std::string(Text(n, src_));
                const size_t before = d_->bindings.size();
                AddBinding(n, start, end, disp, top, let_in);
                if (names.size() == 1 && d_->bindings.size() > before) d_->bindings.back().params = params;
            }
            Params(b, ts_node_end_byte(b));
        }
    }

    void AddOpen(TSNode path_node, uint32_t start, uint32_t end) {
        Open o;
        o.path = SplitPath(Text(path_node, src_));
        o.at = ts_node_start_byte(path_node);
        o.start = start;
        o.end = end;
        if (!o.path.empty()) d_->opens.push_back(o);
    }

    void Walk(TSNode n) {
        const std::string t = Type(n);
        const uint32_t count = ts_node_named_child_count(n);
        if (t == "compilation_unit" || t == "structure" || t == "signature") {
            for (uint32_t i = 0; i < count; i++) {
                TSNode c = ts_node_named_child(n, i);
                const std::string ct = Type(c);
                if (ct == "value_definition") {
                    const bool rec = HasToken(c, "rec");
                    ValueDefinition(c, rec ? ts_node_start_byte(c) : ts_node_end_byte(c), ts_node_end_byte(n),
                                    t == "compilation_unit", false);
                } else if (ct == "open_module") {
                    TSNode m = Field(c, "module");
                    if (!IsNull(m)) AddOpen(m, ts_node_end_byte(c), ts_node_end_byte(n));
                }
                Walk(c);
            }
            return;
        }
        if (t == "let_expression") {
            TSNode body = Field(n, "body");
            const uint32_t end = ts_node_end_byte(n);
            for (uint32_t i = 0; i < count; i++) {
                TSNode c = ts_node_named_child(n, i);
                const std::string ct = Type(c);
                if (ct == "value_definition") {
                    const bool rec = HasToken(c, "rec");
                    const uint32_t start = rec ? ts_node_start_byte(c)
                                               : (IsNull(body) ? ts_node_end_byte(c) : ts_node_start_byte(body));
                    ValueDefinition(c, start, end, false, true);
                } else if (ct == "open_module") {
                    TSNode m = Field(c, "module");
                    if (!IsNull(m)) AddOpen(m, ts_node_end_byte(c), end);
                }
            }
        } else if (t == "local_open_expression") {
            TSNode m = ChildOfType(n, "module_path");
            if (!IsNull(m)) AddOpen(m, ts_node_end_byte(m), ts_node_end_byte(n));
        } else if (t == "fun_expression" || t == "method_definition" || t == "class_binding" || t == "class_function") {
            Params(n, ts_node_end_byte(n));
        } else if (t == "match_case") {
            TSNode pat = Field(n, "pattern");
            std::vector<TSNode> names;
            PatternNodes(pat, &names);
            for (TSNode nm : names) {
                AddBinding(nm, IsNull(pat) ? ts_node_start_byte(n) : ts_node_end_byte(pat), ts_node_end_byte(n),
                           "(pattern) " + std::string(Text(nm, src_)), false, false);
            }
        } else if (t == "for_expression") {
            TSNode name = Field(n, "name");
            if (!IsNull(name))
                AddBinding(name, ts_node_end_byte(name), ts_node_end_byte(n),
                           "(for) " + std::string(Text(name, src_)) + " : int", false, false);
        } else if (t == "value_path") {
            TSNode name = ChildOfType(n, "value_name");
            TSNode mp = ChildOfType(n, "module_path");
            if (!IsNull(name)) {
                if (IsNull(mp)) {
                    Ref r;
                    r.name = std::string(Text(name, src_));
                    r.range = RangeOf(name);
                    r.byte = ts_node_start_byte(name);
                    d_->refs.push_back(r);
                } else {
                    QRef q;
                    q.path = SplitPath(Text(mp, src_));
                    q.name = std::string(Text(name, src_));
                    q.range = RangeOf(name);
                    q.byte = ts_node_start_byte(name);
                    d_->qrefs.push_back(q);
                }
            }
            return;
        } else if (t == "field_expression" && ts_node_named_child_count(n) == 1) {
            // `{ x; y }`: punned fields read the variables x and y.
            TSNode fp = ts_node_named_child(n, 0);
            TSNode name = Type(fp) == "field_path" ? ChildOfType(fp, "field_name") : TSNode{};
            if (!IsNull(name) && IsNull(ChildOfType(fp, "module_path"))) {
                Ref r;
                r.name = std::string(Text(name, src_));
                r.range = RangeOf(name);
                r.byte = ts_node_start_byte(name);
                r.field_pun = true;  // renaming x keeps the field: `{ x = y }`
                d_->refs.push_back(r);
            }
            return;
        } else if (t == "labeled_argument") {
            TSNode label = ChildOfType(n, "label_name");
            if (!IsNull(label) && ts_node_named_child_count(n) == 1) {
                Ref r;
                r.name = std::string(Text(label, src_));
                r.range = RangeOf(label);
                r.byte = ts_node_start_byte(label);
                r.label_pun = true;
                d_->refs.push_back(r);
                return;
            }
        }
        for (uint32_t i = 0; i < count; i++) Walk(ts_node_named_child(n, i));
    }

    void Resolve() {
        std::map<std::string, std::vector<int>> by_name;
        for (size_t i = 0; i < d_->bindings.size(); i++) by_name[d_->bindings[i].name].push_back(static_cast<int>(i));
        for (Ref &r : d_->refs) {
            auto it = by_name.find(r.name);
            if (it == by_name.end()) continue;
            int best = -1;
            for (int bi : it->second) {
                const Binding &b = d_->bindings[static_cast<size_t>(bi)];
                if (r.byte < b.scope_start || r.byte >= b.scope_end) continue;
                if (best < 0) {
                    best = bi;
                    continue;
                }
                const Binding &cur = d_->bindings[static_cast<size_t>(best)];
                if (b.scope_start > cur.scope_start || (b.scope_start == cur.scope_start && b.def_byte > cur.def_byte))
                    best = bi;
            }
            r.binding = best;
            if (best >= 0) d_->bindings[static_cast<size_t>(best)].refs++;
        }
    }
};

/** `(* CMD: ocaml -I lib ... *)` headers: their -I directories. */
std::vector<std::string> CmdIncludes(const std::vector<std::string> &lines, const std::string &dir) {
    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size() && i < 8; i++) {
        const size_t p = lines[i].find("CMD:");
        if (p == std::string::npos) continue;
        std::istringstream in(lines[i].substr(p + 4));
        std::string w;
        while (in >> w) {
            std::string d;
            if (w == "-I") {
                in >> d;
            } else if (w.size() > 2 && w.compare(0, 2, "-I") == 0) {
                d = w.substr(2);
            }
            if (d.empty()) continue;
            fs::path pd(d);
            if (pd.is_relative() && !dir.empty()) pd = fs::path(dir) / pd;
            out.push_back(pd.lexically_normal().string());
        }
    }
    return out;
}

const Binding *BindingAt(const OcamlLspDocument &doc, const std::string &name, uint32_t byte) {
    const Binding *best = nullptr;
    for (const Binding &b : doc.bindings) {
        if (b.name != name || byte < b.scope_start || byte >= b.scope_end) continue;
        if (!best || b.scope_start > best->scope_start ||
            (b.scope_start == best->scope_start && b.def_byte > best->def_byte))
            best = &b;
    }
    return best;
}

std::shared_ptr<const Module> ResolveAt(const OcamlLspDocument &doc, const std::vector<std::string> &path,
                                        uint32_t byte, int depth = 0);

/** The modules opened at @p byte, innermost / latest first (Stdlib not included). */
std::vector<std::shared_ptr<const Module>> UserOpens(const OcamlLspDocument &doc, uint32_t byte, int depth) {
    std::vector<const Open *> live;
    for (const Open &o : doc.opens) {
        if (byte >= o.start && byte <= o.end) live.push_back(&o);
    }
    std::sort(live.begin(), live.end(), [](const Open *a, const Open *b) { return a->start > b->start; });
    std::vector<std::shared_ptr<const Module>> out;
    if (depth > 8) return out;
    for (const Open *o : live) {
        if (auto m = ResolveAt(doc, o->path, o->at, depth + 1)) out.push_back(m);
    }
    return out;
}

/** The modules opened at @p byte: innermost / latest first, then Stdlib. */
std::vector<std::shared_ptr<const Module>> OpenModules(const OcamlLspDocument &doc, uint32_t byte) {
    std::vector<std::shared_ptr<const Module>> out = UserOpens(doc, byte, 0);
    if (auto s = StdlibModule()) out.push_back(s);
    return out;
}

/** A module path as written at @p byte: the document's own modules, then the opened modules'
 *  submodules (`open Datamunge` makes `Stats` visible), then compilation units. */
std::shared_ptr<const Module> ResolveAt(const OcamlLspDocument &doc, const std::vector<std::string> &path,
                                        uint32_t byte, int depth) {
    if (path.empty()) return nullptr;
    Found f = FindItem(doc.module, path[0], {ItemKind::Module}, doc.scope());
    if (!f.item) {
        for (const auto &m : UserOpens(doc, byte, depth)) {
            f = FindItem(m, path[0], {ItemKind::Module}, doc.scope());
            if (f.item) break;
        }
    }
    if (!f.item) return ResolveModulePath(path, doc.scope());
    auto mod = ModuleOfItem(*f.item, f.module.get(), doc.scope());
    for (size_t i = 1; mod && i < path.size(); i++) {
        Found g = FindItem(mod, path[i], {ItemKind::Module}, doc.scope());
        mod = g.item ? ModuleOfItem(*g.item, g.module.get(), doc.scope()) : nullptr;
    }
    return mod;
}

/** An unqualified name: the document's own items, then the opened modules, then Stdlib. */
Found LookupUnqualified(const OcamlLspDocument &doc, const std::string &name, const std::set<ItemKind> &kinds,
                        uint32_t byte, bool include_local) {
    if (include_local) {
        Found f = FindItem(doc.module, name, kinds, doc.scope());
        if (f.item) return f;
    }
    for (const auto &m : OpenModules(doc, byte)) {
        Found f = FindItem(m, name, kinds, doc.scope());
        if (f.item) return f;
    }
    return Found{};
}

const std::set<ItemKind> kValueKinds = {ItemKind::Value, ItemKind::External};

/** The interface's twin of a top-level implementation item (same name and kind family). */
Found TwinItem(const OcamlLspDocument &doc, const std::string &name, ItemKind kind) {
    if (!doc.twin) return Found{};
    std::set<ItemKind> kinds = {kind};
    if (kind == ItemKind::Value || kind == ItemKind::External) kinds = kValueKinds;
    return FindItem(doc.twin, name, kinds, doc.scope());
}

/** The doc's own item (at any depth) whose name is at @p sel. */
const Item *OwnItemAt(const OcamlLspDocument &doc, const OcamlLspRange &sel) {
    std::function<const Item *(const Module &)> find = [&](const Module &m) -> const Item * {
        for (const Item &it : m.items) {
            if (it.sel.line == sel.line && it.sel.col == sel.col) return &it;
            if (it.members) {
                if (const Item *x = find(*it.members)) return x;
            }
        }
        return nullptr;
    };
    return doc.module ? find(*doc.module) : nullptr;
}

// --- the target under the cursor --------------------------------------------------------

struct Target {
    const Binding *binding = nullptr;
    Found found;
    std::shared_ptr<const Module> module;  // a module reference
    const Item *module_item = nullptr;     // its item, when it has one
    std::shared_ptr<const Module> module_item_owner;
    std::string module_name;
    std::string qualifier;
    OcamlLspRange range;
    std::string name;
    bool ok() const { return binding || found.item || module; }
};

TSNode IdentAt(const OcamlLspDocument &doc, int line, int col) {
    TSNode root = doc.tree->Root();
    const uint32_t off = doc.Offset(line, col);
    for (int back = 0; back <= 1; back++) {
        if (back && off == 0) break;
        const uint32_t o = off - static_cast<uint32_t>(back);
        TSNode n = ts_node_named_descendant_for_byte_range(root, o, o);
        if (IsNull(n)) continue;
        if (IsIdentType(Type(n)) && ts_node_start_byte(n) <= o && o <= ts_node_end_byte(n)) return n;
    }
    return TSNode{};
}

/** The dotted module path from the outermost path ancestor's start to the end of @p n. */
std::vector<std::string> PathUpTo(const OcamlLspDocument &doc, TSNode n) {
    TSNode top = n;
    for (;;) {
        TSNode p = ts_node_parent(top);
        if (IsNull(p)) break;
        const std::string pt = Type(p);
        if (pt == "module_path" || pt == "extended_module_path") {
            top = p;
            continue;
        }
        break;
    }
    const uint32_t s = ts_node_start_byte(top), e = ts_node_end_byte(n);
    return SplitPath(std::string_view(doc.src()).substr(s, e - s));
}

/** The module qualifier of an identifier inside a `*_path` node (`List` of `List.map`), if any. */
TSNode QualifierOf(TSNode n) {
    TSNode p = ts_node_parent(n);
    if (IsNull(p)) return TSNode{};
    const std::string pt = Type(p);
    if (pt == "value_path" || pt == "type_constructor_path" || pt == "constructor_path" || pt == "field_path" ||
        pt == "module_type_path" || pt == "class_path") {
        TSNode mp = ChildOfType(p, "module_path");
        if (IsNull(mp)) mp = ChildOfType(p, "extended_module_path");
        if (!IsNull(mp) && ts_node_end_byte(mp) <= ts_node_start_byte(n)) return mp;
    }
    return TSNode{};
}

void FillModuleTarget(const OcamlLspDocument &doc, const std::vector<std::string> &path, uint32_t byte, Target *t) {
    t->module = ResolveAt(doc, path, byte);
    t->module_name = path.empty() ? std::string() : path.back();
    // Its item, for its doc and its definition site.
    if (path.size() > 1) {
        std::vector<std::string> parent(path.begin(), path.end() - 1);
        auto pm = ResolveAt(doc, parent, byte);
        Found f = FindItem(pm, path.back(), {ItemKind::Module}, doc.scope());
        t->module_item = f.item;
        t->module_item_owner = f.module;
    } else if (!path.empty()) {
        Found f = FindItem(doc.module, path[0], {ItemKind::Module}, doc.scope());
        if (!f.item) {
            for (const auto &m : UserOpens(doc, byte, 0)) {
                f = FindItem(m, path[0], {ItemKind::Module}, doc.scope());
                if (f.item) break;
            }
        }
        t->module_item = f.item;
        t->module_item_owner = f.module;
    }
}

Target TargetAt(const OcamlLspDocument &doc, int line, int col) {
    Target t;
    TSNode n = IdentAt(doc, line, col);
    if (IsNull(n)) return t;
    const std::string type = Type(n);
    const std::string name(Text(n, doc.src()));
    const uint32_t byte = ts_node_start_byte(n);
    t.range = RangeOf(n);
    t.name = name;
    TSNode parent = ts_node_parent(n);
    const std::string pt = Type(parent);

    if (type == "module_name") {
        if (pt == "module_binding") {
            if (const Item *own = OwnItemAt(doc, t.range)) {
                t.module_item = own;
                t.module_item_owner = doc.module;
                t.module = ModuleOfItem(*own, doc.module.get(), doc.scope());
                t.module_name = name;
                if (!t.module) t.module = std::make_shared<Module>();
                return t;
            }
        }
        FillModuleTarget(doc, PathUpTo(doc, n), byte, &t);
        return t;
    }
    if (type == "label_name") {
        if (pt == "labeled_argument" && ts_node_named_child_count(parent) == 1) {
            t.binding = BindingAt(doc, name, byte);
        } else if (pt == "parameter") {
            for (const Binding &b : doc.bindings) {
                if (b.sel.line == t.range.line && b.sel.col == t.range.col) t.binding = &b;
            }
        }
        return t;
    }

    std::set<ItemKind> kinds;
    if (type == "value_name" || type == "value_pattern") kinds = kValueKinds;
    else if (type == "type_constructor") kinds = {ItemKind::Type};
    else if (type == "constructor_name") kinds = {ItemKind::Constructor, ItemKind::Exception};
    else if (type == "field_name") kinds = {ItemKind::Field};
    else if (type == "module_type_name") kinds = {ItemKind::ModuleType};
    else if (type == "class_name") kinds = {ItemKind::Class};
    else if (type == "method_name") kinds = {ItemKind::Method};

    TSNode qual = QualifierOf(n);
    if (!IsNull(qual)) {
        const std::vector<std::string> path = SplitPath(Text(qual, doc.src()));
        auto mod = ResolveAt(doc, path, byte);
        t.found = FindItem(mod, name, kinds, doc.scope());
        std::string q;
        for (const std::string &p : path) q += (q.empty() ? "" : ".") + p;
        t.qualifier = q;
        return t;
    }

    if (type == "value_name" || type == "value_pattern") {
        // A definition site, or a reference.
        for (const Binding &b : doc.bindings) {
            if (b.sel.line == t.range.line && b.sel.col == t.range.col) {
                t.binding = &b;
                break;
            }
        }
        if (!t.binding && pt == "value_path") t.binding = BindingAt(doc, name, byte);
        if (t.binding) {
            if (t.binding->top_level) {
                if (const Item *own = OwnItemAt(doc, t.binding->sel)) t.found = Found{own, doc.module};
            }
            return t;
        }
        if (const Item *own = OwnItemAt(doc, t.range)) {  // `val x` in an .mli, a nested module's `let`
            t.found = Found{own, doc.module};
            return t;
        }
        t.found = LookupUnqualified(doc, name, kinds, byte, false);
        return t;
    }
    if (const Item *own = OwnItemAt(doc, t.range)) {
        t.found = Found{own, doc.module};
        return t;
    }
    if (type == "method_name") return t;
    t.found = LookupUnqualified(doc, name, kinds, byte, true);
    return t;
}

std::string ModuleMarkdown(const Target &t) {
    std::string sig = t.module_item ? t.module_item->signature : "module " + t.module_name;
    std::string out = "```ocaml\n" + sig + "\n```";
    std::string doc;
    if (t.module_item && !t.module_item->doc.empty()) doc = t.module_item->doc;
    else if (t.module && !t.module->doc.empty()) doc = t.module->doc;
    if (t.module && !t.module->path.empty() && (!t.module_item || t.module_item_owner != t.module) &&
        (!t.module_item || !t.module_item->members || t.module_item->members != t.module)) {
        out += "\n\n`" + fs::path(t.module->path).filename().string() + "`";
    }
    const std::string md = OdocToMarkdown(doc);
    if (!md.empty()) out += "\n\n---\n\n" + md;
    return out;
}

/** Hover text of a top-level implementation item, preferring its interface's signature and docs. */
std::string OwnItemMarkdown(const OcamlLspDocument &doc, const Item &item) {
    Found twin = TwinItem(doc, item.name, item.kind);
    if (twin.item) {
        Item merged = *twin.item;
        if (merged.doc.empty()) merged.doc = item.doc;
        return ItemMarkdown(merged, std::string());
    }
    return ItemMarkdown(item, std::string());
}

std::string TargetMarkdown(const OcamlLspDocument &doc, const Target &t) {
    if (t.found.item) {
        if (t.found.module == doc.module) return OwnItemMarkdown(doc, *t.found.item);
        return ItemMarkdown(*t.found.item, t.qualifier);
    }
    if (t.binding) return "```ocaml\n" + t.binding->display + "\n```";
    if (t.module) return ModuleMarkdown(t);
    return std::string();
}

/** For items of an .mli, the matching implementation item (the .ml in the same directory). */
bool ImplementationOf(const Found &f, OcamlLspLocation *loc) {
    if (!f.module || f.module->path.empty() || !f.item) return false;
    const fs::path p(f.module->path);
    if (p.extension() != ".mli") return false;
    const fs::path ml = fs::path(p).replace_extension(".ml");
    std::error_code ec;
    if (!fs::exists(ml, ec)) return false;
    auto impl = LoadFile(ml.string());
    if (!impl) return false;
    std::function<const Item *(const Module &)> find = [&](const Module &m) -> const Item * {
        for (size_t i = m.items.size(); i-- > 0;) {
            const Item &it = m.items[i];
            if (it.name == f.item->name && (it.kind == f.item->kind ||
                                            (f.item->kind == ItemKind::Value && it.kind == ItemKind::External)))
                return &it;
        }
        for (const Item &it : m.items) {
            if (it.members) {
                if (const Item *x = find(*it.members)) return x;
            }
        }
        return nullptr;
    };
    const Item *it = find(*impl);
    if (!it) return false;
    loc->path = ml.string();
    loc->range = it->sel;
    return true;
}

std::string PathFor(const OcamlLspDocument &doc, const std::string &p) {
    if (p.empty() || p == doc.path) return std::string();
    return p;
}

std::vector<OcamlLspLocation> Locate(const OcamlLspDocument &doc, int line, int col, bool declaration) {
    std::vector<OcamlLspLocation> out;
    const Target t = TargetAt(doc, line, col);
    if (!t.ok()) return out;
    OcamlLspLocation loc;
    if (t.found.item) {
        if (t.found.module == doc.module) {
            if (declaration) {
                Found twin = TwinItem(doc, t.found.item->name, t.found.item->kind);
                if (twin.item && twin.module) {
                    loc.path = twin.module->path;
                    loc.range = twin.item->sel;
                    out.push_back(loc);
                    return out;
                }
            }
            loc.range = t.found.item->sel;
            out.push_back(loc);
            return out;
        }
        if (!declaration && ImplementationOf(t.found, &loc)) {
            loc.path = PathFor(doc, loc.path);
            out.push_back(loc);
            return out;
        }
        if (!t.found.module || t.found.module->path.empty()) return out;  // the compiled-in stdlib
        loc.path = PathFor(doc, t.found.module->path);
        loc.range = t.found.item->sel;
        out.push_back(loc);
        return out;
    }
    if (t.binding) {
        loc.range = t.binding->sel;
        out.push_back(loc);
        return out;
    }
    if (t.module) {
        if (t.module_item && t.module_item_owner && t.module_item_owner == doc.module) {
            loc.range = t.module_item->sel;
            out.push_back(loc);
            return out;
        }
        if (t.module_item && t.module_item_owner && !t.module_item_owner->path.empty() && !t.module_item->members) {
            // An alias inside another file (`module List = Stdlib__List`): go to the target file.
        } else if (t.module_item && t.module_item_owner && !t.module_item_owner->path.empty()) {
            if (!declaration && ImplementationOf(Found{t.module_item, t.module_item_owner}, &loc)) {
                loc.path = PathFor(doc, loc.path);
                out.push_back(loc);
                return out;
            }
            loc.path = PathFor(doc, t.module_item_owner->path);
            loc.range = t.module_item->sel;
            out.push_back(loc);
            return out;
        }
        if (t.module->path.empty()) return out;
        std::string p = t.module->path;
        if (!declaration && fs::path(p).extension() == ".mli") {
            std::error_code ec;
            const fs::path ml = fs::path(p).replace_extension(".ml");
            if (fs::exists(ml, ec)) p = ml.string();
        }
        loc.path = PathFor(doc, p);
        out.push_back(loc);
    }
    return out;
}

OcamlLspKind CompletionKind(const Item &it) {
    switch (it.kind) {
    case ItemKind::Value:
        return (!it.param_names.empty() || it.type.find("->") != std::string::npos) ? OcamlLspKind::Function
                                                                                   : OcamlLspKind::Value;
    case ItemKind::External: return OcamlLspKind::Function;
    case ItemKind::Type: return OcamlLspKind::Struct;
    case ItemKind::Constructor: return OcamlLspKind::EnumMember;
    case ItemKind::Field: return OcamlLspKind::Field;
    case ItemKind::Exception: return OcamlLspKind::Event;
    case ItemKind::Module: return OcamlLspKind::Module;
    case ItemKind::ModuleType: return OcamlLspKind::Interface;
    case ItemKind::Class: return OcamlLspKind::Class;
    case ItemKind::Method: return OcamlLspKind::Method;
    }
    return OcamlLspKind::Text;
}

std::string ItemDetail(const Item &it) {
    if (!it.type.empty()) return it.type;
    if (it.kind == ItemKind::Constructor || it.kind == ItemKind::Field) return it.signature + " : " + it.owner_type;
    std::string s = it.signature;
    const size_t nl = s.find('\n');
    if (nl != std::string::npos) s = s.substr(0, nl) + " ...";
    return s;
}

bool StartsWithCI(const std::string &s, const std::string &prefix) {
    if (prefix.size() > s.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\''; }

bool InCommentOrString(const OcamlLspDocument &doc, uint32_t off) {
    TSNode root = doc.tree->Root();
    for (uint32_t o : {off, off > 0 ? off - 1 : off}) {
        TSNode n = ts_node_descendant_for_byte_range(root, o, o);
        for (int k = 0; k < 3 && !IsNull(n); k++) {
            const std::string t = Type(n);
            if ((t == "comment" || t == "string" || t == "quoted_string" || t == "character") &&
                ts_node_start_byte(n) < off && off < ts_node_end_byte(n))
                return true;
            n = ts_node_parent(n);
        }
    }
    return false;
}

struct ValueSig {
    bool found = false;
    std::string label;
    std::vector<std::string> params;
    std::vector<std::string> labels;  // parallel: "" for positional
    std::vector<std::string> param_docs;
    std::string doc;
};

/** `@param name text` of a raw doc, in order. */
std::vector<std::pair<std::string, std::string>> ParamDocs(const std::string &raw) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(raw);
    std::string line;
    bool cont = false;
    while (std::getline(in, line)) {
        size_t s = 0;
        while (s < line.size() && std::isspace(static_cast<unsigned char>(line[s]))) s++;
        if (line.compare(s, 7, "@param ") == 0) {
            std::istringstream ls(line.substr(s + 7));
            std::string name;
            ls >> name;
            std::string rest;
            std::getline(ls, rest);
            out.emplace_back(name, rest);
            cont = true;
        } else if (line.compare(s, 1, "@") == 0) {
            cont = false;
        } else if (cont && s < line.size()) {
            out.back().second += " " + line.substr(s);
        }
    }
    for (auto &kv : out) kv.second = OdocSummary(kv.second);
    return out;
}

ValueSig SigOfItem(const Item &item, const std::string &display_name) {
    ValueSig s;
    if (!item.type.empty()) {
        std::string result;
        const std::vector<ArrowParam> ps = SplitArrows(item.type, &result);
        // The label is rebuilt from the pieces so every parameter is an exact substring.
        std::string label = display_name + " : ";
        for (const ArrowParam &p : ps) {
            s.params.push_back(p.text);
            s.labels.push_back(p.label);
            label += p.text + " -> ";
        }
        label += result;
        s.label = label;
    } else if (!item.param_names.empty()) {
        s.label = display_name;
        for (const std::string &p : item.param_names) {
            s.label += " " + p;
            s.params.push_back(p);
            std::string l;
            if (!p.empty() && (p[0] == '~' || p[0] == '?')) {
                size_t e = 1;
                if (e < p.size() && p[e] == '(') e++;
                const size_t b = e;
                while (e < p.size() && IsIdentChar(p[e])) e++;
                l = p.substr(b, e - b);
            }
            s.labels.push_back(l);
        }
    } else {
        return s;
    }
    s.found = true;
    s.doc = OdocToMarkdown(item.doc);
    // @param docs: labelled parameters by name, the others in order.
    const auto pd = ParamDocs(item.doc);
    std::set<std::string> used;
    s.param_docs.assign(s.params.size(), std::string());
    for (size_t i = 0; i < s.params.size(); i++) {
        if (s.labels[i].empty()) continue;
        for (const auto &kv : pd) {
            if (kv.first == s.labels[i]) {
                s.param_docs[i] = "`" + kv.first + "`: " + kv.second;
                used.insert(kv.first);
            }
        }
    }
    size_t next = 0;
    for (size_t i = 0; i < s.params.size(); i++) {
        if (!s.labels[i].empty()) continue;
        while (next < pd.size() && used.count(pd[next].first)) next++;
        if (next < pd.size()) {
            s.param_docs[i] = "`" + pd[next].first + "`: " + pd[next].second;
            next++;
        }
    }
    return s;
}

/** The signature of a function named @p path (`f`, `List.map`) at @p byte. */
ValueSig SigOfName(const OcamlLspDocument &doc, const std::vector<std::string> &path, uint32_t byte) {
    if (path.empty()) return ValueSig{};
    const std::string &name = path.back();
    std::string display;
    for (const std::string &p : path) display += (display.empty() ? "" : ".") + p;
    if (path.size() == 1) {
        if (const Binding *b = BindingAt(doc, name, byte)) {
            if (b->top_level) {
                Found twin = TwinItem(doc, name, ItemKind::Value);
                if (twin.item) {
                    ValueSig s = SigOfItem(*twin.item, display);
                    if (s.found) return s;
                }
                if (const Item *it = OwnItemAt(doc, b->sel)) {
                    ValueSig s = SigOfItem(*it, display);
                    if (s.found) return s;
                }
            }
            if (b->params.empty()) return ValueSig{};
            Item tmp;
            tmp.param_names = b->params;
            return SigOfItem(tmp, display);
        }
        Found f = LookupUnqualified(doc, name, kValueKinds, byte, true);
        return f.item ? SigOfItem(*f.item, display) : ValueSig{};
    }
    std::vector<std::string> mpath(path.begin(), path.end() - 1);
    auto mod = ResolveAt(doc, mpath, byte);
    Found f = FindItem(mod, name, kValueKinds, doc.scope());
    return f.item ? SigOfItem(*f.item, display) : ValueSig{};
}

std::vector<std::string> SplitText(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

/** One edit covering only the changed lines. */
std::vector<OcamlLspTextEdit> Narrow(const std::vector<std::string> &lines, const std::string &formatted) {
    const std::vector<std::string> out_lines = SplitText(formatted);
    std::vector<OcamlLspTextEdit> edits;
    if (out_lines == lines) return edits;
    size_t a = 0;
    while (a < lines.size() && a < out_lines.size() && lines[a] == out_lines[a]) a++;
    size_t be = lines.size(), bo = out_lines.size();
    while (be > a && bo > a && lines[be - 1] == out_lines[bo - 1]) {
        be--;
        bo--;
    }
    OcamlLspTextEdit e;
    if (be < lines.size()) {
        // Whole lines [a, be) replaced by [a, bo).
        e.range.line = static_cast<int>(a);
        e.range.end_line = static_cast<int>(be);
        for (size_t i = a; i < bo; i++) e.new_text += out_lines[i] + "\n";
    } else {
        // The changed part runs to the end of the document.
        e.range.line = static_cast<int>(a);
        e.range.end_line = static_cast<int>(lines.size()) - 1;
        e.range.end_col = static_cast<int>(lines.back().size());
        for (size_t i = a; i < bo; i++) {
            if (i > a) e.new_text += "\n";
            e.new_text += out_lines[i];
        }
        if (a == lines.size()) {  // only appended lines
            e.range.line = e.range.end_line;
            e.range.col = e.range.end_col;
            e.new_text = "\n" + e.new_text;
        }
    }
    edits.push_back(e);
    return edits;
}

std::string Join(const std::vector<std::string> &lines) {
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    return text;
}

}  // namespace

// --- analysis ---------------------------------------------------------------------------

std::shared_ptr<const OcamlLspDocument> OcamlLspAnalyze(const std::vector<std::string> &lines,
                                                       const std::string &path) {
    auto doc = std::make_shared<OcamlLspDocument>();
    doc->lines = lines.empty() ? std::vector<std::string>{std::string()} : lines;
    doc->path = path;
    doc->interface = path.size() > 4 && path.compare(path.size() - 4, 4, ".mli") == 0;
    std::string src;
    for (size_t i = 0; i < doc->lines.size(); i++) {
        doc->line_starts.push_back(src.size());
        src += doc->lines[i];
        if (i + 1 < doc->lines.size()) src += '\n';
    }
    doc->tree = Parse(std::move(src), doc->interface);
    std::error_code ec;
    doc->env.file = path;
    doc->env.dir = path.empty() ? fs::current_path(ec).string() : fs::path(path).parent_path().string();
    doc->env.include_dirs = CmdIncludes(doc->lines, doc->env.dir);
    doc->module = IndexNode(doc->tree->Root(), doc->src(), doc->interface);
    doc->module->name = ModuleNameOf(path);
    SetPaths(doc->module.get(), path);
    if (!doc->interface && !path.empty()) {
        const fs::path mli = fs::path(path).replace_extension(".mli");
        if (fs::exists(mli, ec)) doc->twin = LoadFile(mli.string());
    }
    Walker(doc.get()).Run();
    return doc;
}

void OcamlLspSetWorkspaceRoot(const std::string &root) { SetWorkspaceRoot(root); }

void OcamlLspResetCaches() { ResetCaches(); }

// --- diagnostics ----------------------------------------------------------------------------

std::vector<OcamlLspDiagnostic> OcamlLspDiagnostics(const OcamlLspDocument &doc) {
    std::vector<OcamlLspDiagnostic> out;
    // Syntax errors.
    std::function<void(TSNode)> walk = [&](TSNode n) {
        if (out.size() >= 50) return;
        if (ts_node_is_missing(n)) {
            OcamlLspDiagnostic d;
            d.range = RangeOf(n);
            d.code = "syntax-error";
            d.message = "Syntax error: missing `" + Type(n) + "`";
            out.push_back(d);
            return;
        }
        if (ts_node_is_error(n)) {
            OcamlLspDiagnostic d;
            d.range = RangeOf(n);
            if (d.range.end_line > d.range.line) {  // keep the squiggle on its first line
                d.range.end_line = d.range.line;
                d.range.end_col = static_cast<int>(doc.lines[static_cast<size_t>(d.range.line)].size());
            }
            d.code = "syntax-error";
            std::string text(Text(n, doc.src()));
            if (text.size() > 30) text = text.substr(0, 27) + "...";
            for (char &c : text) {
                if (c == '\n') c = ' ';
            }
            d.message = "Syntax error" + (text.empty() ? std::string() : " near `" + text + "`");
            out.push_back(d);
            return;
        }
        if (!ts_node_has_error(n)) return;
        const uint32_t count = ts_node_child_count(n);
        for (uint32_t i = 0; i < count; i++) walk(ts_node_child(n, i));
    };
    walk(doc.tree->Root());
    // Lines touched by a syntax error get no further (semantic) diagnostics.
    std::set<int> broken;
    for (const OcamlLspDiagnostic &d : out) {
        for (int l = d.range.line; l <= d.range.end_line; l++) broken.insert(l);
    }

    // Unused `let ... in` variables (OCaml's warning 26).
    for (const Binding &b : doc.bindings) {
        if (!b.let_in || b.refs > 0 || b.name.empty() || b.name[0] == '_') continue;
        OcamlLspDiagnostic d;
        d.range = b.sel;
        d.severity = OcamlLspSeverity::Warning;
        d.code = "unused-variable";
        d.message = "Unused variable " + b.name + ".";
        d.unnecessary = true;
        out.push_back(d);
    }

    // Qualified values: unbound in a module whose members are all known; deprecated ones.
    {
        for (const QRef &q : doc.qrefs) {
            if (broken.count(q.range.line)) continue;
            auto mod = ResolveAt(doc, q.path, q.byte);
            if (!mod) continue;
            Found f = FindItem(mod, q.name, kValueKinds, doc.scope());
            std::string qual;
            for (const std::string &p : q.path) qual += p + ".";
            if (!f.item) {
                if (mod->path.empty() || mod->opaque || !mod->include_types.empty()) continue;
                bool known = true;
                for (const std::string &inc : mod->includes) {
                    auto im = ResolveModulePath(SplitPath(inc), doc.scope());
                    if (!im || im->opaque || im->path.empty()) known = false;
                }
                if (!known) continue;
                OcamlLspDiagnostic d;
                d.range = q.range;
                d.severity = mod->interface ? OcamlLspSeverity::Error : OcamlLspSeverity::Warning;
                d.code = "unbound-value";
                d.message = "Unbound value " + qual + q.name;
                out.push_back(d);
                continue;
            }
            if (f.item->is_deprecated) {
                OcamlLspDiagnostic d;
                d.range = q.range;
                d.severity = OcamlLspSeverity::Warning;
                d.code = "deprecated";
                d.message = qual + q.name + " is deprecated" +
                            (f.item->deprecated.empty() ? std::string(".") : ": " + f.item->deprecated);
                d.deprecated = true;
                out.push_back(d);
            }
        }
        for (const Ref &r : doc.refs) {
            if (r.binding >= 0 || broken.count(r.range.line)) continue;
            Found f = LookupUnqualified(doc, r.name, kValueKinds, r.byte, false);
            if (!f.item || !f.item->is_deprecated) continue;
            OcamlLspDiagnostic d;
            d.range = r.range;
            d.severity = OcamlLspSeverity::Warning;
            d.code = "deprecated";
            d.message = r.name + " is deprecated" +
                        (f.item->deprecated.empty() ? std::string(".") : ": " + f.item->deprecated);
            d.deprecated = true;
            out.push_back(d);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const OcamlLspDiagnostic &a, const OcamlLspDiagnostic &b) {
        return a.range.line != b.range.line ? a.range.line < b.range.line : a.range.col < b.range.col;
    });
    return out;
}

std::string OcamlLspFormatDiagnostic(const std::string &path, const OcamlLspDiagnostic &d) {
    const char *sev = d.severity == OcamlLspSeverity::Error         ? "error"
                      : d.severity == OcamlLspSeverity::Warning     ? "warning"
                      : d.severity == OcamlLspSeverity::Information ? "info"
                                                                     : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool OcamlLspDiagnosticFails(const OcamlLspDiagnostic &d) {
    return d.severity == OcamlLspSeverity::Error || d.severity == OcamlLspSeverity::Warning;
}

// --- completion ------------------------------------------------------------------------------

std::vector<OcamlLspCompletionItem> OcamlLspCompletions(const OcamlLspDocument &doc, int line, int col) {
    std::vector<OcamlLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return out;
    const std::string &cur = doc.lines[static_cast<size_t>(line)];
    col = std::max(0, std::min(col, static_cast<int>(cur.size())));
    const uint32_t off = doc.Offset(line, col);
    if (InCommentOrString(doc, off)) return out;
    int start = col;
    while (start > 0 && IsIdentChar(cur[static_cast<size_t>(start - 1)])) start--;
    const std::string prefix = cur.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    // A module qualifier before it: `Foo.Bar.pre`.
    std::vector<std::string> qual;
    {
        int p = start;
        while (p > 0 && cur[static_cast<size_t>(p - 1)] == '.') {
            const int q = p - 1;
            int b = q;
            while (b > 0 && IsIdentChar(cur[static_cast<size_t>(b - 1)])) b--;
            if (b == q || !std::isupper(static_cast<unsigned char>(cur[static_cast<size_t>(b)]))) break;
            qual.insert(qual.begin(), cur.substr(static_cast<size_t>(b), static_cast<size_t>(q - b)));
            p = b;
        }
    }
    std::set<std::string> seen;
    auto push = [&](const std::string &label, OcamlLspKind kind, const std::string &detail, const std::string &docs) {
        if (out.size() >= 600) return;
        if (!StartsWithCI(label, prefix)) return;
        if (!seen.insert(label).second) return;
        OcamlLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = docs;
        it.replace_start = start;
        it.replace_end = col;
        out.push_back(it);
    };
    auto push_item = [&](const Item &it) { push(it.name, CompletionKind(it), ItemDetail(it), OdocToMarkdown(it.doc)); };
    const bool upper = !prefix.empty() && std::isupper(static_cast<unsigned char>(prefix[0]));

    if (!qual.empty()) {
        auto mod = ResolveAt(doc, qual, off);
        if (!mod) return out;
        std::vector<Found> all = AllItems(mod, doc.scope());
        // Values first, then modules, constructors, the rest.
        auto rank = [](ItemKind k) {
            switch (k) {
            case ItemKind::Value:
            case ItemKind::External: return 0;
            case ItemKind::Module: return 1;
            case ItemKind::Constructor:
            case ItemKind::Exception: return 2;
            default: return 3;
            }
        };
        std::stable_sort(all.begin(), all.end(), [&](const Found &a, const Found &b) {
            if (rank(a.item->kind) != rank(b.item->kind)) return rank(a.item->kind) < rank(b.item->kind);
            return a.item->name < b.item->name;
        });
        for (const Found &f : all) {
            if (f.item->kind == ItemKind::Method) continue;
            push_item(*f.item);
        }
        return out;
    }

    if (!upper) {
        // Bindings in scope, innermost first.
        std::vector<const Binding *> live;
        for (const Binding &b : doc.bindings) {
            if (off >= b.scope_start && off <= b.scope_end && !(b.sel.line == line && b.sel.col == start))
                live.push_back(&b);
        }
        std::sort(live.begin(), live.end(),
                  [](const Binding *a, const Binding *b) { return a->scope_start > b->scope_start; });
        for (const Binding *b : live) {
            std::string detail = b->display;
            const OcamlLspKind kind = b->params.empty() ? OcamlLspKind::Variable : OcamlLspKind::Function;
            std::string docs;
            if (b->top_level) {
                if (const Item *it = OwnItemAt(doc, b->sel)) {
                    Found twin = TwinItem(doc, it->name, it->kind);
                    const Item &src = twin.item ? *twin.item : *it;
                    if (!src.type.empty()) detail = src.type;
                    docs = OdocToMarkdown(src.doc);
                }
            }
            push(b->name, kind, detail, docs);
        }
        for (const Item &it : doc.module->items) {
            if (it.kind == ItemKind::Value || it.kind == ItemKind::External) push_item(it);
        }
    } else {
        for (const Item &it : doc.module->items) {
            if (it.kind == ItemKind::Module || it.kind == ItemKind::Constructor || it.kind == ItemKind::Exception ||
                it.kind == ItemKind::ModuleType)
                push_item(it);
        }
    }
    // Opened modules (Stdlib last).
    for (const auto &m : OpenModules(doc, off)) {
        for (const Found &f : AllItems(m, doc.scope())) {
            const ItemKind k = f.item->kind;
            const bool value = k == ItemKind::Value || k == ItemKind::External;
            const bool capital = k == ItemKind::Module || k == ItemKind::Constructor || k == ItemKind::Exception;
            if ((upper && capital) || (!upper && value)) push_item(*f.item);
        }
    }
    if (upper) {
        for (const std::string &m : KnownModules(doc.env)) push(m, OcamlLspKind::Module, "module", std::string());
        for (const char *c : {"None", "Some", "Ok", "Error", "Not_found", "Invalid_argument", "Failure"})
            push(c, OcamlLspKind::EnumMember, std::string(), std::string());
    } else if (!prefix.empty()) {
        for (const std::string &k : Keywords()) push(k, OcamlLspKind::Keyword, std::string(), std::string());
        for (const char *c : {"true", "false"}) push(c, OcamlLspKind::Constant, "bool", std::string());
    }
    return out;
}

// --- hover / signature help ---------------------------------------------------------------------

OcamlLspHoverInfo OcamlLspHover(const OcamlLspDocument &doc, int line, int col) {
    OcamlLspHoverInfo info;
    const Target t = TargetAt(doc, line, col);
    if (!t.ok()) return info;
    info.text = TargetMarkdown(doc, t);
    if (info.text.empty()) return info;
    info.found = true;
    info.range = t.range;
    return info;
}

OcamlLspSignatureHelp OcamlLspSignatureAt(const OcamlLspDocument &doc, int line, int col) {
    OcamlLspSignatureHelp sh;
    const uint32_t off = doc.Offset(line, col);
    if (InCommentOrString(doc, off)) return sh;
    // Lex the text before the cursor and walk back over the current application.
    const std::string prefix = doc.src().substr(0, off);
    std::vector<ocamlfmt::Token> toks;
    std::string err;
    int err_line = 0;
    if (!ocamlfmt::Lex(prefix, &toks, &err, &err_line)) return sh;
    toks.erase(std::remove_if(toks.begin(), toks.end(),
                              [](const ocamlfmt::Token &t) { return t.kind == ocamlfmt::TokKind::Comment; }),
               toks.end());
    if (toks.empty()) return sh;
    struct Atom {
        size_t first, last;  // token indices
        std::string label;   // `~f:` / `?f:` / punned `~f` before it
    };
    std::vector<Atom> atoms;  // backwards
    int depth = 0;
    size_t atom_end = 0;
    auto is_open = [](std::string_view t) {
        return t == "(" || t == "[" || t == "{" || t == "[|" || t == "begin" || t == "[<" || t == "[>" || t == "{<";
    };
    auto is_close = [](std::string_view t) {
        return t == ")" || t == "]" || t == "}" || t == "|]" || t == "end" || t == ">}";
    };
    size_t k = toks.size();
    while (k-- > 0) {
        const ocamlfmt::Token &t = toks[k];
        const std::string_view tx = t.text;
        if (depth > 0) {
            if (is_close(tx)) {
                depth++;
            } else if (is_open(tx) && --depth == 0) {
                atoms.push_back(Atom{k, atom_end, ""});
            }
            continue;
        }
        if (is_close(tx)) {
            depth = 1;
            atom_end = k;
            continue;
        }
        if (is_open(tx)) break;
        if (t.kind == ocamlfmt::TokKind::Label) {
            std::string l(tx.substr(1));
            const bool colon = !l.empty() && l.back() == ':';
            if (colon) l.pop_back();
            if (colon && !atoms.empty() && atoms.back().label.empty() && atoms.back().first > k) {
                atoms.back().label = l;
            } else {
                atoms.push_back(Atom{k, k, l});  // punned `~f`, or `~f:` being typed
            }
            continue;
        }
        const bool atom_tok = t.kind == ocamlfmt::TokKind::Ident || t.kind == ocamlfmt::TokKind::UIdent ||
                              t.kind == ocamlfmt::TokKind::Number || t.kind == ocamlfmt::TokKind::String ||
                              t.kind == ocamlfmt::TokKind::Char || t.kind == ocamlfmt::TokKind::QString ||
                              t.kind == ocamlfmt::TokKind::Tag || tx == "true" || tx == "false";
        if (atom_tok) {
            // Qualified names: `List.map` is one atom.
            size_t first = k;
            while (first >= 2 && toks[first - 1].text == "." && toks[first - 2].kind == ocamlfmt::TokKind::UIdent)
                first -= 2;
            atoms.push_back(Atom{first, k, ""});
            k = first;
            continue;
        }
        if (tx == "." || tx == "!") continue;
        break;
    }
    if (depth > 0 || atoms.empty()) return sh;
    std::reverse(atoms.begin(), atoms.end());
    const Atom &head = atoms[0];
    if (!head.label.empty()) return sh;  // the head is a (possibly qualified) name
    std::vector<std::string> path;
    for (size_t i = head.first; i <= head.last && i < toks.size(); i++) {
        if (toks[i].kind == ocamlfmt::TokKind::Ident || toks[i].kind == ocamlfmt::TokKind::UIdent)
            path.emplace_back(toks[i].text);
        else if (toks[i].text != ".")
            return sh;
    }
    if (path.empty()) return sh;
    const char c0 = path.back()[0];
    if (!(std::islower(static_cast<unsigned char>(c0)) || c0 == '_')) return sh;
    const ValueSig vs = SigOfName(doc, path, static_cast<uint32_t>(toks[head.first].begin));
    if (!vs.found || vs.params.empty()) return sh;
    // Which argument the cursor is on.
    const ocamlfmt::Token &lt = toks.back();
    const bool touching = lt.end == off;
    const bool after_label = touching && lt.kind == ocamlfmt::TokKind::Label && lt.text.back() == ':';
    const bool typing = touching && !after_label && atoms.size() > 1;
    std::string current_label;
    if (after_label) {
        current_label = std::string(lt.text.substr(1, lt.text.size() - 2));
    } else if (typing && !atoms.back().label.empty()) {
        current_label = atoms.back().label;
    }
    int active = -1;
    if (!current_label.empty()) {
        for (size_t i = 0; i < vs.labels.size(); i++) {
            if (vs.labels[i] == current_label) active = static_cast<int>(i);
        }
    } else {
        size_t positional = 0;
        const size_t complete = atoms.size() - 1 - (typing ? 1 : 0);
        for (size_t i = 1; i <= complete; i++) {
            if (atoms[i].label.empty()) positional++;
        }
        size_t seen_pos = 0;
        for (size_t i = 0; i < vs.labels.size(); i++) {
            if (!vs.labels[i].empty()) continue;
            if (seen_pos == positional) {
                active = static_cast<int>(i);
                break;
            }
            seen_pos++;
        }
        if (active < 0) active = static_cast<int>(vs.params.size()) - 1;
    }
    sh.found = true;
    sh.label = vs.label;
    sh.params = vs.params;
    sh.param_docs = vs.param_docs;
    sh.documentation = vs.doc;
    sh.active_param = std::max(0, active);
    return sh;
}

// --- navigation ----------------------------------------------------------------------------------

std::vector<OcamlLspLocation> OcamlLspDefinition(const OcamlLspDocument &doc, int line, int col) {
    return Locate(doc, line, col, false);
}

std::vector<OcamlLspLocation> OcamlLspDeclaration(const OcamlLspDocument &doc, int line, int col) {
    return Locate(doc, line, col, true);
}

std::vector<OcamlLspReference> OcamlLspReferences(const OcamlLspDocument &doc, int line, int col) {
    std::vector<OcamlLspReference> out;
    const Target t = TargetAt(doc, line, col);
    if (t.binding) {
        out.push_back(OcamlLspReference{t.binding->sel, true});
        const int idx = static_cast<int>(t.binding - doc.bindings.data());
        for (const Ref &r : doc.refs) {
            if (r.binding == idx) out.push_back(OcamlLspReference{r.range, false});
        }
        return out;
    }
    if (t.found.item) {
        if (t.found.module == doc.module) out.push_back(OcamlLspReference{t.found.item->sel, true});
        if (t.found.item->kind == ItemKind::Value || t.found.item->kind == ItemKind::External) {
            for (const QRef &q : doc.qrefs) {
                if (q.name != t.found.item->name) continue;
                Found f = FindItem(ResolveAt(doc, q.path, q.byte), q.name, kValueKinds, doc.scope());
                if (f.item == t.found.item) out.push_back(OcamlLspReference{q.range, false});
            }
            for (const Ref &r : doc.refs) {
                if (r.binding >= 0 || r.name != t.found.item->name) continue;
                Found f = LookupUnqualified(doc, r.name, kValueKinds, r.byte, false);
                if (f.item == t.found.item) out.push_back(OcamlLspReference{r.range, false});
            }
        }
    }
    return out;
}

OcamlLspRenameTarget OcamlLspPrepareRename(const OcamlLspDocument &doc, int line, int col) {
    OcamlLspRenameTarget out;
    const Target t = TargetAt(doc, line, col);
    if (!t.binding || t.binding->label_pun) return out;
    out.found = true;
    out.range = t.range;
    out.placeholder = t.binding->name;
    return out;
}

std::vector<OcamlLspTextEdit> OcamlLspRename(const OcamlLspDocument &doc, int line, int col,
                                               const std::string &new_name, std::string *error) {
    std::vector<OcamlLspTextEdit> edits;
    const Target t = TargetAt(doc, line, col);
    if (!t.binding) {
        *error = "only values bound in this file can be renamed";
        return edits;
    }
    if (t.binding->label_pun) {
        *error = "renaming a labelled parameter would change the function's labels";
        return edits;
    }
    bool valid = !new_name.empty() && (std::islower(static_cast<unsigned char>(new_name[0])) || new_name[0] == '_') &&
                 !IsKeyword(new_name);
    for (char c : new_name) valid = valid && IsIdentChar(c);
    if (!valid) {
        *error = "`" + new_name + "` is not a valid OCaml value name";
        return edits;
    }
    edits.push_back(OcamlLspTextEdit{t.binding->sel, new_name});
    const int idx = static_cast<int>(t.binding - doc.bindings.data());
    for (const Ref &r : doc.refs) {
        if (r.binding != idx) continue;
        // `~x` keeps its label: `~x:new_name`.
        edits.push_back(OcamlLspTextEdit{r.range, r.label_pun   ? r.name + ":" + new_name
                                                  : r.field_pun ? r.name + " = " + new_name
                                                                : new_name});
    }
    return edits;
}

// --- outline / folding -------------------------------------------------------------------------

std::vector<OcamlLspSymbol> OcamlLspSymbols(const OcamlLspDocument &doc) {
    std::vector<OcamlLspSymbol> out;
    std::function<void(const Module &, int)> add = [&](const Module &m, int parent) {
        int last_type = -1;
        std::string last_type_name;
        for (const Item &it : m.items) {
            OcamlLspSymbol s;
            s.name = it.name;
            s.range = it.range;
            s.sel_range = it.sel;
            s.parent = parent;
            switch (it.kind) {
            case ItemKind::Value:
                s.kind = (!it.param_names.empty() || it.type.find("->") != std::string::npos)
                             ? OcamlLspSymbolKind::Function
                             : OcamlLspSymbolKind::Variable;
                s.detail = it.type;
                break;
            case ItemKind::External:
                s.kind = OcamlLspSymbolKind::Function;
                s.detail = it.type;
                break;
            case ItemKind::Type:
                s.kind = it.signature.find('{') != std::string::npos   ? OcamlLspSymbolKind::Struct
                         : it.signature.find('|') != std::string::npos ? OcamlLspSymbolKind::Enum
                                                                       : OcamlLspSymbolKind::TypeParameter;
                break;
            case ItemKind::Constructor: s.kind = OcamlLspSymbolKind::EnumMember; break;
            case ItemKind::Field:
                s.kind = OcamlLspSymbolKind::Field;
                s.detail = it.type;
                break;
            case ItemKind::Exception: s.kind = OcamlLspSymbolKind::Event; break;
            case ItemKind::Module: s.kind = OcamlLspSymbolKind::Module; break;
            case ItemKind::ModuleType: s.kind = OcamlLspSymbolKind::Interface; break;
            case ItemKind::Class: s.kind = OcamlLspSymbolKind::Class; break;
            case ItemKind::Method:
                s.kind = OcamlLspSymbolKind::Method;
                s.detail = it.type;
                break;
            }
            if ((it.kind == ItemKind::Constructor || it.kind == ItemKind::Field) && last_type >= 0 &&
                it.owner_type == last_type_name) {
                s.parent = last_type;
            }
            const int idx = static_cast<int>(out.size());
            out.push_back(s);
            if (it.kind == ItemKind::Type) {
                last_type = idx;
                last_type_name = it.name;
            }
            if (it.members) add(*it.members, idx);
        }
    };
    if (doc.module) add(*doc.module, -1);
    return out;
}

std::vector<OcamlLspFold> OcamlLspFolds(const OcamlLspDocument &doc) {
    std::vector<OcamlLspFold> out;
    static const std::set<std::string> kinds = {
        "value_definition", "type_definition", "module_definition", "module_type_definition", "class_definition",
        "class_type_definition", "structure", "signature", "object_expression", "match_expression",
        "function_expression", "try_expression", "let_expression", "if_expression", "list_expression",
        "array_expression", "record_expression", "record_declaration", "variant_declaration",
        "parenthesized_expression", "fun_expression", "external", "value_specification", "exception_definition",
        "comment"};
    std::map<int, int> best;  // start line -> end line
    std::map<int, bool> is_comment;
    std::function<void(TSNode)> walk = [&](TSNode n) {
        const std::string t = Type(n);
        const TSPoint s = ts_node_start_point(n), e = ts_node_end_point(n);
        if (kinds.count(t) && e.row > s.row) {
            int end = static_cast<int>(e.row);
            const int start = static_cast<int>(s.row);
            if (e.column == 0) end--;
            if (end > start) {
                auto it = best.find(start);
                if (it == best.end() || it->second < end) {
                    best[start] = end;
                    is_comment[start] = t == "comment";
                }
            }
        }
        const uint32_t count = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < count; i++) walk(ts_node_named_child(n, i));
    };
    walk(doc.tree->Root());
    for (const auto &kv : best)
        out.push_back(OcamlLspFold{kv.first, kv.second, is_comment[kv.first] ? "comment" : ""});
    return out;
}

// --- --doc ---------------------------------------------------------------------------------------

std::string OcamlLspDocFor(const OcamlLspDocument &doc, const std::string &name) {
    std::vector<std::string> path = SplitPath(name);
    if (path.empty()) return std::string();
    const uint32_t end = static_cast<uint32_t>(doc.src().size());
    const bool all_modules = std::all_of(path.begin(), path.end(), [](const std::string &p) {
        return !p.empty() && std::isupper(static_cast<unsigned char>(p[0]));
    });
    if (all_modules) {
        Target t;
        FillModuleTarget(doc, path, end, &t);
        if (t.module) {
            std::string md = ModuleMarkdown(t);
            // The module's values, as a quick index.
            std::vector<std::string> values;
            for (const Found &f : AllItems(t.module, doc.scope())) {
                if (f.item->kind == ItemKind::Value || f.item->kind == ItemKind::External)
                    values.push_back(f.item->name);
            }
            if (!values.empty()) {
                std::string list;
                for (size_t i = 0; i < values.size() && i < 120; i++) list += (i ? ", `" : "`") + values[i] + "`";
                if (values.size() > 120) list += ", ...";
                md += "\n\n**Values:** " + list;
            }
            return md;
        }
        // Not a module: perhaps a constructor or an exception (`Not_found`, `Foo.Bar`).
    }
    const std::string last = path.back();
    const std::vector<std::string> mpath(path.begin(), path.end() - 1);
    const std::set<ItemKind> any;
    Found f;
    if (!mpath.empty()) {
        auto mod = ResolveAt(doc, mpath, end);
        f = FindItem(mod, last, kValueKinds, doc.scope());
        if (!f.item) f = FindItem(mod, last, any, doc.scope());
        if (!f.item) return std::string();
        std::string q;
        for (const std::string &p : mpath) q += (q.empty() ? "" : ".") + p;
        return ItemMarkdown(*f.item, q);
    }
    f = LookupUnqualified(doc, last, kValueKinds, end, true);
    if (!f.item) f = LookupUnqualified(doc, last, any, end, true);
    if (!f.item) return std::string();
    if (f.module == doc.module) return OwnItemMarkdown(doc, *f.item);
    return ItemMarkdown(*f.item, std::string());
}

// --- formatting ----------------------------------------------------------------------------------

std::vector<OcamlLspTextEdit> OcamlLspFormat(const std::vector<std::string> &lines, int width, std::string *error) {
    const std::string text = Join(lines);
    ocamlfmt::Options o;
    if (width > 0) o.width = width;
    const ocamlfmt::Result r = ocamlfmt::Format(text, o);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return {};
    }
    if (r.text == text) return {};
    return Narrow(lines, r.text);
}

std::vector<OcamlLspTextEdit> OcamlLspFormatRange(const std::vector<std::string> &lines, int first, int last,
                                                  int width, std::string *error) {
    std::vector<OcamlLspTextEdit> edits;
    const std::string text = Join(lines);
    ocamlfmt::Options o;
    if (width > 0) o.width = width;
    o.break_lines = false;
    const ocamlfmt::Result r = ocamlfmt::Format(text, o);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    const std::vector<std::string> out = SplitText(r.text);
    if (out.size() != lines.size()) {
        *error = "range formatting would change the number of lines";
        return edits;
    }
    first = std::max(0, first);
    last = std::min(last, static_cast<int>(lines.size()) - 1);
    for (int i = first; i <= last; i++) {
        const size_t k = static_cast<size_t>(i);
        if (lines[k] == out[k]) continue;
        OcamlLspTextEdit e;
        e.range.line = i;
        e.range.end_line = i;
        e.range.end_col = static_cast<int>(lines[k].size());
        e.new_text = out[k];
        edits.push_back(e);
    }
    return edits;
}
