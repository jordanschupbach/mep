// The analysis behind mep-crystal-lsp (crystal_lsp_analysis.h).

#include "crystal_lsp_analysis.h"

#include <algorithm>
#include <functional>

#include "crystal_parser.h"

namespace crlsp {

using crystal::Location;

// --- positions ------------------------------------------------------------------------

int Analysis::ByteCol(int line1, int col1) const {
    if (line1 < 1 || line1 > static_cast<int>(lines.size())) return std::max(0, col1 - 1);
    const std::string &s = lines[static_cast<size_t>(line1 - 1)];
    int chars = 0;
    size_t i = 0;
    while (i < s.size() && chars < col1 - 1) {
        const auto b = static_cast<unsigned char>(s[i]);
        size_t w = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
        i += w;
        chars++;
    }
    if (chars < col1 - 1) return static_cast<int>(s.size()) + (col1 - 1 - chars);
    return static_cast<int>(std::min(i, s.size()));
}

CrRange Analysis::RangeOf(Location loc, Location end) const {
    CrRange r;
    if (!loc.known()) return r;
    r.start_line = loc.line - 1;
    r.start_col = ByteCol(loc.line, loc.column);
    if (end.known() && (end.line > loc.line || (end.line == loc.line && end.column >= loc.column))) {
        r.end_line = end.line - 1;
        r.end_col = ByteCol(end.line, end.column + 1);
    } else {
        r.end_line = r.start_line;
        r.end_col = r.start_col;
    }
    return r;
}

CrRange Analysis::RangeAt(Location loc, int chars) const {
    CrRange r;
    if (!loc.known()) return r;
    r.start_line = r.end_line = loc.line - 1;
    r.start_col = ByteCol(loc.line, loc.column);
    r.end_col = ByteCol(loc.line, loc.column + chars);
    return r;
}

CrRange Analysis::NodeRange(const Node *n) const { return RangeOf(crystal::Loc(n), crystal::EndLoc(n)); }

bool Before(int l1, int c1, int l2, int c2) { return l1 < l2 || (l1 == l2 && c1 < c2); }

bool Contains(const CrRange &r, int line, int col) {
    return !Before(line, col, r.start_line, r.start_col) && !Before(r.end_line, r.end_col, line, col);
}

int ScopeAt(const Analysis &a, int line, int col) {
    int best = 0;
    for (size_t i = 1; i < a.scopes.size(); i++) {
        if (!Contains(a.scopes[i].range, line, col)) continue;
        // Scopes are created outside-in, so a later containing one is nested deeper.
        best = static_cast<int>(i);
    }
    return best;
}

// --- type text and inference -------------------------------------------------------------

std::string TypeText(const Node *n) {
    if (!n) return "";
    switch (n->kind) {
        case NodeKind::Path: {
            std::string s = n->global ? "::" : "";
            for (size_t i = 0; i < n->names.size(); i++) s += (i ? "::" : "") + n->names[i];
            return s;
        }
        case NodeKind::Generic: {
            const std::string name = TypeText(n->name_node);
            if (n->suffix == crystal::GenericSuffix::Question && !n->exps.empty()) return TypeText(n->exps[0]) + "?";
            if (n->suffix == crystal::GenericSuffix::Asterisk && !n->exps.empty()) return TypeText(n->exps[0]) + "*";
            if (n->suffix == crystal::GenericSuffix::Bracket && n->exps.size() == 2) {
                return TypeText(n->exps[0]) + "[" + TypeText(n->exps[1]) + "]";
            }
            std::string s = name + "(";
            bool first = true;
            for (const Node *t : n->exps) {
                s += (first ? "" : ", ") + TypeText(t);
                first = false;
            }
            for (const Node *na : n->named_args) {
                s += (first ? "" : ", ") + na->name + ": " + TypeText(na->value_node);
                first = false;
            }
            return s + ")";
        }
        case NodeKind::Union: {
            if (n->exps.size() == 2 && n->exps[1]->is(NodeKind::Path) && n->exps[1]->global &&
                n->exps[1]->names.size() == 1 && n->exps[1]->names[0] == "Nil") {
                return TypeText(n->exps[0]) + "?";
            }
            std::string s;
            for (size_t i = 0; i < n->exps.size(); i++) s += (i ? " | " : "") + TypeText(n->exps[i]);
            return s;
        }
        case NodeKind::Metaclass: return TypeText(n->name_node) + ".class";
        case NodeKind::ProcNotation: {
            std::string s;
            for (size_t i = 0; i < n->exps.size(); i++) s += (i ? ", " : "") + TypeText(n->exps[i]);
            s += s.empty() ? "->" : " ->";
            if (n->right) s += " " + TypeText(n->right);
            return s;
        }
        case NodeKind::Self: return "self";
        case NodeKind::Underscore: return "_";
        case NodeKind::NumberLiteral: return n->value;
        case NodeKind::Splat: return "*" + TypeText(n->exp);
        case NodeKind::TypeOf: return "typeof(...)";
        default: return "";
    }
}

std::string BaseType(const std::string &type_in) {
    std::string t = type_in;
    while (!t.empty() && t.back() == '?') t.pop_back();
    const size_t bar = t.find(" | ");
    if (bar != std::string::npos) {
        std::string first = t.substr(0, bar);
        if (first == "Nil" || first == "::Nil") first = t.substr(bar + 3);
        t = first;
    }
    if (t.rfind("::", 0) == 0) t = t.substr(2);
    const size_t paren = t.find('(');
    if (paren != std::string::npos) t = t.substr(0, paren);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}

namespace {

const char *NumberTypeName(crystal::NumberKind k) {
    switch (k) {
        case crystal::NumberKind::I8: return "Int8";
        case crystal::NumberKind::I16: return "Int16";
        case crystal::NumberKind::I32: return "Int32";
        case crystal::NumberKind::I64: return "Int64";
        case crystal::NumberKind::I128: return "Int128";
        case crystal::NumberKind::U8: return "UInt8";
        case crystal::NumberKind::U16: return "UInt16";
        case crystal::NumberKind::U32: return "UInt32";
        case crystal::NumberKind::U64: return "UInt64";
        case crystal::NumberKind::U128: return "UInt128";
        case crystal::NumberKind::F32: return "Float32";
        case crystal::NumberKind::F64: return "Float64";
    }
    return "Int32";
}

int LocalAtNode(const Analysis &a, const Node *n) {
    const CrRange r = a.NodeRange(n);
    for (const auto &lo : a.local_occs) {
        if (lo.first == r) return lo.second;
    }
    return -1;
}

}  // namespace

std::string InferType(const Analysis &a, const Node *n, int depth) {
    if (!n || depth > 8) return "";
    switch (n->kind) {
        case NodeKind::NumberLiteral: return NumberTypeName(n->number_kind);
        case NodeKind::StringLiteral:
        case NodeKind::StringInterpolation:
            return "String";
        case NodeKind::CharLiteral: return "Char";
        case NodeKind::SymbolLiteral: return "Symbol";
        case NodeKind::BoolLiteral: return "Bool";
        case NodeKind::NilLiteral: return "Nil";
        case NodeKind::RegexLiteral: return "Regex";
        case NodeKind::RangeLiteral: return "Range";
        case NodeKind::ProcLiteral: return "Proc";
        case NodeKind::TupleLiteral: return "Tuple";
        case NodeKind::NamedTupleLiteral: return "NamedTuple";
        case NodeKind::ArrayLiteral: {
            if (n->name_node) return TypeText(n->name_node);
            if (n->of) return "Array(" + TypeText(n->of) + ")";
            std::string elem;
            for (const Node *e : n->exps) {
                const std::string t = InferType(a, e, depth + 1);
                if (t.empty() || (!elem.empty() && t != elem)) return "Array";
                elem = t;
            }
            return elem.empty() ? "Array" : "Array(" + elem + ")";
        }
        case NodeKind::HashLiteral: {
            if (n->name_node) return TypeText(n->name_node);
            if (n->has_of) return "Hash(" + TypeText(n->of_key) + ", " + TypeText(n->of_value) + ")";
            if (!n->entries.empty()) {
                const std::string k = InferType(a, n->entries[0].first, depth + 1);
                const std::string v = InferType(a, n->entries[0].second, depth + 1);
                if (!k.empty() && !v.empty()) return "Hash(" + k + ", " + v + ")";
            }
            return "Hash";
        }
        case NodeKind::Cast: return TypeText(n->right);
        case NodeKind::NilableCast: return TypeText(n->right) + "?";
        case NodeKind::Expressions:
            return n->exps.empty() ? "" : InferType(a, n->exps.back(), depth + 1);
        case NodeKind::Var: {
            const int id = LocalAtNode(a, n);
            if (id < 0) return "";
            const Local &l = a.locals[static_cast<size_t>(id)];
            if (l.declared_type) return TypeText(l.declared_type);
            if (l.first_value && l.first_value != n) return InferType(a, l.first_value, depth + 1);
            return "";
        }
        case NodeKind::Call: {
            if (n->name == "new" && n->obj &&
                (n->obj->is(NodeKind::Path) || n->obj->is(NodeKind::Generic))) {
                return TypeText(n->obj);
            }
            std::string owner;
            if (n->obj) {
                owner = BaseType(InferType(a, n->obj, depth + 1));
                if (owner.empty()) return "";
            }
            // The file's own definitions.
            for (const Def &d : a.defs) {
                if ((d.kind == "def" || d.kind == "getter") && d.name == n->name && !d.ret.empty() &&
                    (owner.empty() || BaseType(d.owner) == owner || d.owner == owner)) {
                    return d.ret;
                }
            }
            return "";
        }
        default:
            return "";
    }
}

// --- header text ----------------------------------------------------------------------

std::string HeaderText(const Analysis &a, const Node *n) {
    const Location loc = crystal::Loc(n);
    if (!loc.known()) return "";
    int line = loc.line - 1;
    size_t col = static_cast<size_t>(a.ByteCol(loc.line, loc.column));
    std::string out;
    int depth = 0;
    char quote = 0;
    bool pending_space = false;
    for (int guard = 0; line < static_cast<int>(a.lines.size()) && guard < 40; guard++) {
        const std::string &s = a.lines[static_cast<size_t>(line)];
        for (size_t i = col; i < s.size(); i++) {
            const char c = s[i];
            if (quote) {
                out += c;
                if (c == '\\' && i + 1 < s.size()) {
                    out += s[++i];
                } else if (c == quote) {
                    quote = 0;
                }
                continue;
            }
            if (c == '#' && !(i + 1 < s.size() && s[i + 1] == '{')) break;  // a comment
            if (c == ';' && depth == 0) return out;
            if (c == '"' || c == '\'') quote = c;
            if (c == '(' || c == '[' || c == '{') depth++;
            if (c == ')' || c == ']' || c == '}') depth--;
            if (c == ' ' || c == '\t') {
                pending_space = !out.empty();
                continue;
            }
            if (pending_space) out += ' ';
            pending_space = false;
            out += c;
        }
        if (depth <= 0 && !quote) break;
        line++;
        col = 0;
        pending_space = !out.empty() && out.back() != '(' && out.back() != '[';
    }
    return out;
}

// --- the walk -----------------------------------------------------------------------------

namespace {

class Walker {
public:
    explicit Walker(Analysis &a) : a_(a) {}

    void Run() {
        Scope file;
        file.kind = ScopeKind::File;
        file.range.start_line = 0;
        file.range.end_line = static_cast<int>(a_.lines.size());
        file.range.end_col = 1 << 20;
        a_.scopes.push_back(file);
        cur_ = 0;
        if (a_.root) Visit(a_.root);
    }

private:
    Analysis &a_;
    int cur_ = 0;
    std::vector<std::string> owners_;  // type nesting
    std::vector<int> def_stack_;       // enclosing type defs (indexes in a_.defs)

    std::string Owner() const { return owners_.empty() ? std::string() : owners_.back(); }

    int PushScope(ScopeKind kind, const Node *range_node) {
        Scope s;
        s.kind = kind;
        s.parent = cur_;
        s.owner = Owner();
        s.range = a_.NodeRange(range_node);
        a_.scopes.push_back(s);
        cur_ = static_cast<int>(a_.scopes.size()) - 1;
        return cur_;
    }
    void PopScope(int saved) { cur_ = saved; }

    // A name visible from the current scope: up the chain through blocks and procs,
    // stopping at a def, a type body or the file.
    int Lookup(const std::string &name) const {
        int s = cur_;
        while (s >= 0) {
            const Scope &sc = a_.scopes[static_cast<size_t>(s)];
            auto it = sc.vars.find(name);
            if (it != sc.vars.end()) return it->second;
            if (sc.kind != ScopeKind::Block && sc.kind != ScopeKind::Proc) break;
            s = sc.parent;
        }
        return -1;
    }

    int Bind(const std::string &name, const CrRange &range, bool is_param = false, bool is_block_param = false) {
        Local l;
        l.name = name;
        l.scope = cur_;
        l.def_range = range;
        l.is_param = is_param;
        l.is_block_param = is_block_param;
        a_.locals.push_back(l);
        const int id = static_cast<int>(a_.locals.size()) - 1;
        a_.scopes[static_cast<size_t>(cur_)].vars[name] = id;
        return id;
    }

    void AddOcc(int id, const CrRange &r, bool write, bool def) {
        Local &l = a_.locals[static_cast<size_t>(id)];
        l.occs.push_back(Occurrence{r, write, def});
        if (!write) l.reads++;
        a_.local_occs.push_back({r, id});
    }

    // A local's use (read) or binding (write).
    void VarRef(Node *var, bool write, Node *value = nullptr, Node *declared = nullptr, int bind_kind = 0) {
        if (var->name == "self" || var->name.empty()) return;
        const CrRange r = a_.NodeRange(var);
        int id = Lookup(var->name);
        if (id < 0) {
            id = Bind(var->name, r);
            Local &l = a_.locals[static_cast<size_t>(id)];
            l.first_value = value;
            l.declared_type = declared;
            l.bind_kind = bind_kind;
            AddOcc(id, r, write, true);
            if (!write) {
                // Known to the parser as a local without an assignment we saw
                // (a macro-made one, `$~`): not an unused one.
                a_.locals[static_cast<size_t>(id)].reads++;
            }
            return;
        }
        Local &l = a_.locals[static_cast<size_t>(id)];
        if (!l.declared_type && declared) l.declared_type = declared;
        if (!l.first_value && value) l.first_value = value;
        AddOcc(id, r, write, false);
    }

    void AddName(NameKind kind, const std::string &name, const CrRange &r, Node *node, bool is_def,
                 bool write = false) {
        NameOcc o;
        o.kind = kind;
        o.name = name;
        o.owner = Owner();
        o.range = r;
        o.node = node;
        o.is_def = is_def;
        o.write = write;
        o.scope = cur_;
        a_.names.push_back(o);
    }

    int AddDef(const std::string &kind, const std::string &name, Node *node, const CrRange &name_range) {
        Def d;
        d.kind = kind;
        d.owner = Owner();
        d.name = name;
        d.node = node;
        d.range = name_range;
        d.full_range = node ? a_.NodeRange(node) : name_range;
        if (node && node->has_doc) d.doc = node->doc;
        d.parent = def_stack_.empty() ? -1 : def_stack_.back();
        a_.defs.push_back(d);
        return static_cast<int>(a_.defs.size()) - 1;
    }

    std::string Join(const std::string &owner, const Node *path) const {
        const std::string p = TypeText(path);
        if (path && path->global) return p.substr(2);
        return owner.empty() ? p : owner + "::" + p;
    }

    // The range of an Arg's internal name (after an external name if any).
    CrRange ArgNameRange(const Node *arg) const {
        const Location loc = arg->location;
        if (!loc.known()) return CrRange();
        if (arg->external_name == arg->name || arg->name.empty()) {
            return a_.RangeAt(loc, static_cast<int>(crystal::Utf8Length(arg->name)));
        }
        const int line = loc.line - 1;
        if (line < 0 || line >= static_cast<int>(a_.lines.size())) return a_.RangeAt(loc, 0);
        const std::string &s = a_.lines[static_cast<size_t>(line)];
        size_t from = static_cast<size_t>(a_.ByteCol(loc.line, loc.column));
        size_t p = s.find(arg->name, from + 1);
        while (p != std::string::npos) {
            const bool left_ok = p == 0 || !(std::isalnum(static_cast<unsigned char>(s[p - 1])) || s[p - 1] == '_');
            const size_t e = p + arg->name.size();
            const bool right_ok = e >= s.size() || !(std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_');
            if (left_ok && right_ok) {
                CrRange r;
                r.start_line = r.end_line = line;
                r.start_col = static_cast<int>(p);
                r.end_col = static_cast<int>(e);
                return r;
            }
            p = s.find(arg->name, p + 1);
        }
        return a_.RangeAt(loc, static_cast<int>(crystal::Utf8Length(arg->external_name)));
    }

    void VisitList(const std::vector<Node *> &v) {
        for (Node *n : v) Visit(n);
    }

    void VisitArgs(Node *def, bool is_proc) {
        const bool abstract_or_empty =
            def->is_abstract || !def->body || def->body->is(NodeKind::Nop) ||
            (def->body->is(NodeKind::Expressions) && def->body->exps.empty());
        auto bind_arg = [&](Node *arg, bool block) {
            if (!arg) return;
            if (arg->default_value) Visit(arg->default_value);
            if (arg->name.empty()) return;
            const int id = Bind(arg->name, ArgNameRange(arg), !block || is_proc, false);
            Local &l = a_.locals[static_cast<size_t>(id)];
            l.declared_type = arg->restriction;
            l.abstract_owner = abstract_or_empty;
            if (block) l.is_param = true;
            AddOcc(id, l.def_range, true, true);
        };
        for (Node *arg : def->args) bind_arg(arg, false);
        bind_arg(def->double_splat, false);
        bind_arg(def->block_arg, true);
    }

    // Expressions: also records code after a jump as unreachable.
    void VisitStatements(Node *n) {
        bool jumped = false;
        for (Node *e : n->exps) {
            if (jumped) {
                a_.unreachable_ranges.push_back(a_.NodeRange(e));
                jumped = false;
            }
            Visit(e);
            if (e->is_control_expression()) jumped = true;
            if (e->is(NodeKind::Call) && !e->obj && !e->block && (e->name == "raise" || e->name == "exit" ||
                                                                    e->name == "abort")) {
                jumped = true;
            }
        }
    }

    void Accessors(Node *call) {
        static const char *const kGetters[] = {"getter", "getter?", "getter!", "class_getter", "class_getter?",
                                               "class_getter!", "property", "property?", "property!",
                                               "class_property", "class_property?", "class_property!", "setter",
                                               "class_setter"};
        bool is_accessor = false;
        for (const char *g : kGetters) {
            if (call->name == g) is_accessor = true;
        }
        if (!is_accessor) return;
        const std::string &m = call->name;
        const bool cls = m.rfind("class_", 0) == 0;
        const bool getter = m.find("getter") != std::string::npos || m.find("property") != std::string::npos;
        const bool setter = m.find("setter") != std::string::npos || m.find("property") != std::string::npos;
        const std::string suffix = (!m.empty() && (m.back() == '?' || m.back() == '!')) ? std::string(1, m.back()) : "";
        for (Node *arg : call->args) {
            std::string name;
            Node *name_node = arg;
            std::string ret;
            if (arg->is(NodeKind::TypeDeclaration) && arg->var) {
                name = arg->var->name;
                name_node = arg->var;
                ret = TypeText(arg->declared_type);
            } else if (arg->is(NodeKind::Assign) && arg->target) {
                name = arg->target->name;
                name_node = arg->target;
                ret = InferType(a_, arg->value_node);
            } else if (arg->is(NodeKind::Var) || arg->is(NodeKind::Call)) {
                name = arg->name;
            } else if (arg->is(NodeKind::SymbolLiteral) || arg->is(NodeKind::StringLiteral)) {
                name = arg->value;
            }
            if (name.empty()) continue;
            const CrRange r = a_.NodeRange(name_node);
            if (getter) {
                const int d = AddDef("getter", name + (suffix == "!" ? "" : suffix), arg, r);
                a_.defs[static_cast<size_t>(d)].ret = ret;
                a_.defs[static_cast<size_t>(d)].class_method = cls;
                a_.defs[static_cast<size_t>(d)].signature = m + " " + name + (ret.empty() ? "" : " : " + ret);
                if (suffix == "!") {
                    const int d2 = AddDef("getter", name, arg, r);
                    a_.defs[static_cast<size_t>(d2)].ret = ret;
                    a_.defs[static_cast<size_t>(d2)].signature = a_.defs[static_cast<size_t>(d)].signature;
                }
            }
            if (setter) {
                const int d = AddDef("setter", name + "=", arg, r);
                a_.defs[static_cast<size_t>(d)].class_method = cls;
                a_.defs[static_cast<size_t>(d)].signature = m + " " + name + (ret.empty() ? "" : " : " + ret);
            }
            AddName(NameKind::InstanceVar, (cls ? "@@" : "@") + name, r, arg, true, true);
        }
    }

    void Record(Node *call) {
        if (call->name != "record" || call->obj || call->args.empty() || !call->args[0]->is(NodeKind::Path)) return;
        Node *path = call->args[0];
        const int d = AddDef("struct", path->names.back(), call, a_.NodeRange(path));
        a_.defs[static_cast<size_t>(d)].signature = HeaderText(a_, call);
        a_.defs[static_cast<size_t>(d)].superclass = "Struct";
        AddName(NameKind::Path, Join(Owner(), path), a_.NodeRange(path), path, true);
        owners_.push_back(Join(Owner(), path));
        def_stack_.push_back(d);
        for (size_t i = 1; i < call->args.size(); i++) {
            Node *arg = call->args[i];
            std::string name;
            std::string ret;
            Node *nn = arg;
            if (arg->is(NodeKind::TypeDeclaration) && arg->var) {
                name = arg->var->name;
                nn = arg->var;
                ret = TypeText(arg->declared_type);
            } else if (arg->is(NodeKind::Assign) && arg->target) {
                name = arg->target->name;
                nn = arg->target;
                ret = InferType(a_, arg->value_node);
            } else if (arg->is(NodeKind::Var) || arg->is(NodeKind::Call)) {
                name = arg->name;
            }
            if (name.empty()) continue;
            const int g = AddDef("getter", name, arg, a_.NodeRange(nn));
            a_.defs[static_cast<size_t>(g)].ret = ret;
            a_.defs[static_cast<size_t>(g)].signature = "getter " + name + (ret.empty() ? "" : " : " + ret);
        }
        def_stack_.pop_back();
        owners_.pop_back();
    }

    void TypeDef(Node *n, const std::string &kind) {
        const std::string full = Join(Owner(), n->name_node);
        const int d = AddDef(kind, n->name_node->names.back(), n, a_.NodeRange(n->name_node));
        Def &def = a_.defs[static_cast<size_t>(d)];
        def.signature = HeaderText(a_, n);
        def.is_abstract = n->is_abstract;
        if (n->superclass) def.superclass = TypeText(n->superclass);
        AddName(NameKind::Path, full, a_.NodeRange(n->name_node), n->name_node, true);
        owners_.push_back(full);
        def_stack_.push_back(d);
        const int saved = cur_;
        PushScope(ScopeKind::Type, n);
        if (n->body) Visit(n->body);
        for (Node *m : n->exps) Visit(m);  // enum members
        PopScope(saved);
        def_stack_.pop_back();
        owners_.pop_back();
    }

    void Visit(Node *n) {
        if (!n) return;
        switch (n->kind) {
            case NodeKind::Expressions: VisitStatements(n); return;
            case NodeKind::Var: VarRef(n, false); return;
            case NodeKind::Assign: {
                Visit(n->value_node);
                Node *t = n->target;
                if (t->is(NodeKind::Var)) {
                    VarRef(t, true, n->value_node);
                } else if (t->is(NodeKind::Path)) {
                    const int d = AddDef("const", t->names.back(), n, a_.NodeRange(t));
                    a_.defs[static_cast<size_t>(d)].signature = HeaderText(a_, n);
                    a_.defs[static_cast<size_t>(d)].ret = InferType(a_, n->value_node);
                    AddName(NameKind::Path, Join(Owner(), t), a_.NodeRange(t), t, true);
                } else if (t->is(NodeKind::InstanceVar)) {
                    AddName(NameKind::InstanceVar, t->name, a_.NodeRange(t), n, false, true);
                } else if (t->is(NodeKind::ClassVar)) {
                    AddName(NameKind::ClassVar, t->name, a_.NodeRange(t), n, false, true);
                } else {
                    Visit(t);
                }
                return;
            }
            case NodeKind::OpAssign: {
                Node *t = n->target;
                if (t->is(NodeKind::Var)) {
                    VarRef(t, false);
                } else {
                    Visit(t);
                }
                Visit(n->value_node);
                return;
            }
            case NodeKind::MultiAssign: {
                VisitList(n->exps2);
                for (Node *t : n->exps) {
                    Node *v = t->is(NodeKind::Splat) ? t->exp : t;
                    if (v && v->is(NodeKind::Var)) {
                        VarRef(v, true, nullptr, nullptr, 1);
                    } else {
                        Visit(v);
                    }
                }
                return;
            }
            case NodeKind::TypeDeclaration: {
                Visit(n->value_node);
                if (n->var && n->var->is(NodeKind::Var)) {
                    if (a_.scopes[static_cast<size_t>(cur_)].kind == ScopeKind::Type) {
                        // `x : Int32` in a type body is not a local; the parser made it a
                        // declaration of the type's.
                        return;
                    }
                    VarRef(n->var, true, n->value_node, n->declared_type);
                } else if (n->var && n->var->is(NodeKind::InstanceVar)) {
                    AddName(NameKind::InstanceVar, n->var->name, a_.NodeRange(n->var), n, true, true);
                } else if (n->var && n->var->is(NodeKind::ClassVar)) {
                    AddName(NameKind::ClassVar, n->var->name, a_.NodeRange(n->var), n, true, true);
                }
                return;
            }
            case NodeKind::UninitializedVar:
                if (n->var && n->var->is(NodeKind::Var)) {
                    VarRef(n->var, true, nullptr, n->declared_type);
                } else if (n->var && n->var->is(NodeKind::InstanceVar)) {
                    AddName(NameKind::InstanceVar, n->var->name, a_.NodeRange(n->var), n, true, true);
                }
                return;
            case NodeKind::Out:
                if (n->exp && n->exp->is(NodeKind::Var)) {
                    VarRef(n->exp, true, nullptr, nullptr, 3);
                } else {
                    Visit(n->exp);
                }
                return;
            case NodeKind::InstanceVar:
                AddName(NameKind::InstanceVar, n->name, a_.NodeRange(n), n, false);
                return;
            case NodeKind::ClassVar:
                AddName(NameKind::ClassVar, n->name, a_.NodeRange(n), n, false);
                return;
            case NodeKind::Path:
                AddName(NameKind::Path, TypeText(n), a_.NodeRange(n), n, false);
                return;
            case NodeKind::Call: {
                // An ECR template (Kemal's `render`, invidious's `templated`,
                // `ECR.embed`) is expanded into this scope by a macro and reads
                // its locals: count them used, as for macro code.
                if ((!n->obj && (n->name == "render" || n->name == "templated" || n->name == "ecr" ||
                                 n->name == "render_template")) ||
                    (n->obj && n->obj->is(NodeKind::Path) && !n->obj->names.empty() && n->obj->names[0] == "ECR")) {
                    MarkMacro();
                }
                Visit(n->obj);
                if (n->name_location.known() && n->name_size_ != 0) {
                    const int size = crystal::NameSize(n);
                    if (size > 0) AddName(NameKind::Call, n->name, a_.RangeAt(n->name_location, size), n, false);
                }
                // `getter x = 1`, `record P, x : Int32 = 0`: at the top level or in a type
                // body, an assignment or declaration passed to a call is a macro's argument,
                // not a local.
                int iso = cur_;
                while (a_.scopes[static_cast<size_t>(iso)].kind == ScopeKind::Block ||
                       a_.scopes[static_cast<size_t>(iso)].kind == ScopeKind::Proc) {
                    iso = a_.scopes[static_cast<size_t>(iso)].parent;
                }
                const ScopeKind sk = a_.scopes[static_cast<size_t>(iso)].kind;
                const bool macro_args = !n->obj && (sk == ScopeKind::Type || sk == ScopeKind::File);
                for (Node *arg : n->args) {
                    if (macro_args && ((arg->is(NodeKind::Assign) && arg->target && arg->target->is(NodeKind::Var)) ||
                                       (arg->is(NodeKind::TypeDeclaration) && arg->var &&
                                        arg->var->is(NodeKind::Var)))) {
                        Visit(arg->value_node);
                        continue;
                    }
                    Visit(arg);
                }
                for (Node *na : n->named_args) Visit(na->value_node);
                Visit(n->block_arg);
                Visit(n->block);
                if (!n->obj && a_.scopes[static_cast<size_t>(cur_)].kind == ScopeKind::Type) {
                    Accessors(n);
                }
                if (!n->obj) Record(n);
                return;
            }
            case NodeKind::Block: {
                const int saved = cur_;
                PushScope(ScopeKind::Block, n);
                for (Node *arg : n->exps) {
                    if (arg->name.empty() || arg->name == "_") continue;
                    {
                        const int inner = cur_;
                        cur_ = a_.scopes[static_cast<size_t>(inner)].parent;
                        if (Lookup(arg->name) >= 0) {
                            a_.shadows.push_back({a_.RangeAt(arg->location, static_cast<int>(crystal::Utf8Length(arg->name))),
                                                  arg->name});
                        }
                        cur_ = inner;
                    }
                    const int id = Bind(arg->name, a_.NodeRange(arg).end_col == a_.NodeRange(arg).start_col
                                                       ? a_.RangeAt(arg->location,
                                                                    static_cast<int>(crystal::Utf8Length(arg->name)))
                                                       : a_.NodeRange(arg),
                                        false, true);
                    AddOcc(id, a_.locals[static_cast<size_t>(id)].def_range, true, true);
                }
                for (auto &kv : n->unpacks) BindUnpack(kv.second);
                Visit(n->body);
                PopScope(saved);
                return;
            }
            case NodeKind::ProcLiteral: {
                Node *d = n->def_;
                const int saved = cur_;
                PushScope(ScopeKind::Proc, n);
                VisitArgs(d, true);
                Visit(d->body);
                PopScope(saved);
                return;
            }
            case NodeKind::Def: {
                const bool class_method = n->receiver != nullptr;
                const int d = AddDef("def", n->name, n,
                                     a_.RangeAt(n->name_location, static_cast<int>(crystal::Utf8Length(n->name))));
                Def &def = a_.defs[static_cast<size_t>(d)];
                def.class_method = class_method;
                def.is_abstract = n->is_abstract;
                def.signature = HeaderText(a_, n);
                def.ret = TypeText(n->return_type);
                AddName(NameKind::DefName, n->name, def.range, n, true);
                Visit(n->receiver);
                const int saved = cur_;
                PushScope(ScopeKind::Def, n);
                VisitArgs(n, false);
                // `def initialize(@x)`: the body starts with `@x = x` at the parameter.
                Visit(n->body);
                PopScope(saved);
                return;
            }
            case NodeKind::Macro: {
                const int d = AddDef("macro", n->name, n,
                                     a_.RangeAt(n->name_location, static_cast<int>(crystal::Utf8Length(n->name))));
                a_.defs[static_cast<size_t>(d)].signature = HeaderText(a_, n);
                AddName(NameKind::DefName, n->name, a_.defs[static_cast<size_t>(d)].range, n, true);
                return;  // the body is macro code
            }
            case NodeKind::ClassDef: TypeDef(n, n->is_struct ? "struct" : "class"); return;
            case NodeKind::ModuleDef: TypeDef(n, "module"); return;
            case NodeKind::EnumDef: {
                const std::string full = Join(Owner(), n->name_node);
                const int enum_index = static_cast<int>(a_.defs.size());
                TypeDef(n, "enum");
                owners_.push_back(full);
                for (Node *m : n->exps) {
                    if (m->is(NodeKind::Arg)) {
                        const int d = AddDef("member", m->name, m,
                                             a_.RangeAt(m->location, static_cast<int>(crystal::Utf8Length(m->name))));
                        a_.defs[static_cast<size_t>(d)].parent = enum_index;
                        a_.defs[static_cast<size_t>(d)].signature = m->name;
                    }
                }
                owners_.pop_back();
                return;
            }
            case NodeKind::LibDef: TypeDef(n, "lib"); return;
            case NodeKind::CStructOrUnionDef: {
                const int d = AddDef(n->is_union ? "union" : "struct", n->name, n,
                                     a_.RangeAt(n->location, 0));
                a_.defs[static_cast<size_t>(d)].signature = HeaderText(a_, n);
                owners_.push_back(Owner().empty() ? n->name : Owner() + "::" + n->name);
                def_stack_.push_back(d);
                Visit(n->body);
                def_stack_.pop_back();
                owners_.pop_back();
                return;
            }
            case NodeKind::FunDef: {
                const int d = AddDef("fun", n->name, n,
                                     a_.RangeAt(n->name_location, static_cast<int>(crystal::Utf8Length(n->name))));
                a_.defs[static_cast<size_t>(d)].signature = HeaderText(a_, n);
                a_.defs[static_cast<size_t>(d)].ret = TypeText(n->return_type);
                AddName(NameKind::DefName, n->name, a_.defs[static_cast<size_t>(d)].range, n, true);
                if (n->body) {
                    const int saved = cur_;
                    PushScope(ScopeKind::Def, n);
                    for (Node *arg : n->args) {
                        if (arg->name.empty()) continue;
                        const int id = Bind(arg->name, ArgNameRange(arg), true);
                        a_.locals[static_cast<size_t>(id)].declared_type = arg->restriction;
                        AddOcc(id, a_.locals[static_cast<size_t>(id)].def_range, true, true);
                    }
                    Visit(n->body);
                    PopScope(saved);
                }
                return;
            }
            case NodeKind::Alias: {
                const int d = AddDef("alias", n->name_node->names.back(), n, a_.NodeRange(n->name_node));
                a_.defs[static_cast<size_t>(d)].signature = HeaderText(a_, n);
                AddName(NameKind::Path, Join(Owner(), n->name_node), a_.NodeRange(n->name_node), n->name_node, true);
                Visit(n->value_node);
                return;
            }
            case NodeKind::TypeDef: {
                const int d = AddDef("alias", n->name, n, a_.RangeAt(n->name_location,
                                                                       static_cast<int>(crystal::Utf8Length(n->name))));
                a_.defs[static_cast<size_t>(d)].signature = HeaderText(a_, n);
                return;
            }
            case NodeKind::AnnotationDef: {
                const int d = AddDef("annotation", n->name_node->names.back(), n, a_.NodeRange(n->name_node));
                a_.defs[static_cast<size_t>(d)].signature = HeaderText(a_, n);
                AddName(NameKind::Path, Join(Owner(), n->name_node), a_.NodeRange(n->name_node), n->name_node, true);
                return;
            }
            case NodeKind::Rescue: {
                for (Node *t : n->exps) Visit(t);
                if (n->has_name) {
                    // The name has no node: find it after `rescue`.
                    CrRange r = a_.RangeAt(n->location, 0);
                    if (r.start_line < static_cast<int>(a_.lines.size())) {
                        const std::string &s = a_.lines[static_cast<size_t>(r.start_line)];
                        const size_t p = s.find(n->name, static_cast<size_t>(r.start_col) + 6);
                        if (p != std::string::npos) {
                            r.start_col = static_cast<int>(p);
                            r.end_col = static_cast<int>(p + n->name.size());
                        }
                    }
                    int id = Lookup(n->name);
                    if (id < 0) {
                        id = Bind(n->name, r);
                        a_.locals[static_cast<size_t>(id)].bind_kind = 2;
                    }
                    AddOcc(id, r, true, a_.locals[static_cast<size_t>(id)].occs.empty());
                }
                Visit(n->body);
                return;
            }
            case NodeKind::Include:
            case NodeKind::Extend:
                if (n->kind == NodeKind::Include && !def_stack_.empty() && n->name_node) {
                    a_.defs[static_cast<size_t>(def_stack_.back())].includes.push_back(TypeText(n->name_node));
                }
                Visit(n->name_node);
                return;
            case NodeKind::MacroExpression: case NodeKind::MacroIf: case NodeKind::MacroFor:
            case NodeKind::MacroLiteral: case NodeKind::MacroVar: case NodeKind::MacroVerbatim:
                // Macro code: what it uses is not seen; neither the scope nor the ones it
                // sits in can call a local unused.
                MarkMacro();
                return;
            case NodeKind::Require:
                a_.require_nodes.push_back(n);
                return;
            default:
                break;
        }
        // Everything else: its children, in order.
        ForEachChild(n, [this](Node *c) { Visit(c); });
    }

    void MarkMacro() {
        for (int sc = cur_; sc >= 0; sc = a_.scopes[static_cast<size_t>(sc)].parent) {
            a_.scopes[static_cast<size_t>(sc)].has_macro = true;
            if (a_.scopes[static_cast<size_t>(sc)].kind != ScopeKind::Block &&
                a_.scopes[static_cast<size_t>(sc)].kind != ScopeKind::Proc) {
                break;
            }
        }
    }

    void BindUnpack(Node *n) {
        if (!n) return;
        if (n->is(NodeKind::Expressions)) {
            for (Node *e : n->exps) BindUnpack(e);
        } else if (n->is(NodeKind::Splat)) {
            BindUnpack(n->exp);
        } else if (n->is(NodeKind::Var) && !n->name.empty() && n->name != "_") {
            const CrRange r = a_.RangeAt(n->location, static_cast<int>(crystal::Utf8Length(n->name)));
            const int id = Bind(n->name, r, false, true);
            AddOcc(id, r, true, true);
        }
    }

    static void ForEachChild(Node *n, const std::function<void(Node *)> &f) {
        std::vector<Node *> kids;
        auto add = [&](Node *c) {
            if (c) kids.push_back(c);
        };
        add(n->obj);
        add(n->receiver);
        add(n->cond);
        add(n->target);
        add(n->var);
        add(n->left);
        add(n->exp);
        add(n->value_node);
        add(n->declared_type);
        add(n->of);
        add(n->name_node);
        add(n->superclass);
        add(n->restriction);
        add(n->default_value);
        add(n->return_type);
        add(n->scope);
        add(n->then_);
        add(n->else_);
        add(n->body);
        add(n->ensure_);
        add(n->right);
        add(n->double_splat);
        add(n->def_);
        add(n->of_key);
        add(n->of_value);
        for (Node *c : n->exps) add(c);
        for (Node *c : n->exps2) add(c);
        for (Node *c : n->args) add(c);
        for (Node *c : n->named_args) add(c);
        for (auto &e : n->entries) {
            add(e.first);
            add(e.second);
        }
        for (auto &e : n->named_entries) add(e.second);
        add(n->block_arg);
        add(n->block);
        // Source order, so a local's first binding comes before its uses.
        std::stable_sort(kids.begin(), kids.end(), [](const Node *x, const Node *y) {
            const Location a = crystal::Loc(x), b = crystal::Loc(y);
            if (!a.known() || !b.known()) return false;
            return a.line < b.line || (a.line == b.line && a.column < b.column);
        });
        for (Node *c : kids) f(c);
    }
};

std::string Join(const std::vector<std::string> &lines) {
    std::string s;
    for (size_t i = 0; i < lines.size(); i++) {
        s += lines[i];
        if (i + 1 < lines.size()) s += '\n';
    }
    return s;
}

}  // namespace

std::unique_ptr<Analysis> Analyze(const std::vector<std::string> &lines) {
    auto a = std::make_unique<Analysis>();
    a->lines = lines;
    a->text = Join(lines);

    auto parse = [&](const std::string &text, crystal::Node **root, std::string *err, int *eline, int *ecol,
                     std::vector<std::string> *warnings) {
        try {
            crystal::Parser p(text, a->ast);
            p.set_wants_doc(true);
            *root = p.parse();
            if (warnings) *warnings = p.warnings;
            return true;
        } catch (const crystal::SyntaxError &e) {
            *err = e.what();
            *eline = e.line;
            *ecol = e.column;
            return false;
        }
    };

    std::string err;
    int eline = 0, ecol = 0;
    std::vector<std::string> warnings;
    if (parse(a->text, &a->root, &err, &eline, &ecol, &warnings)) {
        a->ok = true;
    } else {
        a->ok = false;
        const size_t nl = err.find('\n');
        a->error = nl == std::string::npos ? err : err.substr(0, nl);
        a->error_line = eline > 0 ? eline - 1 : 0;
        a->error_col = eline > 0 ? a->ByteCol(eline, ecol) : 0;
        // Recovery: blank the line the error is on (or close what EOF left open) and try again.
        std::vector<std::string> copy = lines;
        int appended = 0;
        for (int attempt = 0; attempt < 40; attempt++) {
            if (eline <= 0) break;
            if (eline > static_cast<int>(copy.size()) || (eline == static_cast<int>(copy.size()) &&
                                                          copy[static_cast<size_t>(eline - 1)].find_first_not_of(
                                                              " \t") == std::string::npos)) {
                if (appended >= 12) break;
                copy.push_back("end");
                appended++;
            } else {
                std::string &l = copy[static_cast<size_t>(eline - 1)];
                if (l.find_first_not_of(" \t") == std::string::npos) {
                    if (appended >= 12) break;
                    copy.push_back("end");
                    appended++;
                } else {
                    l.assign(l.size(), ' ');
                }
            }
            std::string e2;
            if (parse(Join(copy), &a->root, &e2, &eline, &ecol, nullptr)) {
                a->repaired = true;
                break;
            }
            a->root = nullptr;
        }
    }
    for (const std::string &w : warnings) {
        // "line:col: message"
        const size_t c1 = w.find(':');
        const size_t c2 = w.find(": ", c1 == std::string::npos ? 0 : c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        const int wl = std::atoi(w.substr(0, c1).c_str());
        const int wc = std::atoi(w.substr(c1 + 1, c2 - c1 - 1).c_str());
        a->warnings.push_back({a->RangeAt(crystal::Location{wl, wc}, 1), w.substr(c2 + 2)});
    }
    Walker(*a).Run();
    return a;
}

}  // namespace crlsp
