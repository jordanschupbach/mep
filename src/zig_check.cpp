// mep-zig-lsp's semantic checks: a port of the diagnostic half of Zig 0.16's
// lib/std/zig/AstGen.zig (MIT, Copyright (c) Zig contributors; the license is
// reproduced in zig_ast.h). See zig_check.h.
//
// AstGen walks the tree once, lowering it to ZIR; this walks it the same way
// and in the same order, keeping only what decides an error: the scope chain
// (Scope.LocalVal / LocalPtr / Namespace / GenZir / Defer), the result
// location kind of each operand (none / ref / ref_const / discard -- `.ref`
// is what marks a `var` as mutated), and the per-GenZir flags (comptime,
// @TypeOf, inside a defer, the function body). Function names below are
// AstGen's, so each piece can be read against the Zig source.
//
// A hard error (AstGen's `fail*`) throws Fail; it is caught per container
// member exactly where AstGen catches error.AnalysisFail (containerMember),
// so one undeclared name hides the later errors of that declaration, as in
// `zig ast-check`. Soft errors (`append*`) are just recorded.

#include "zig_check.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>

#include "zig_lsp_vocab.h"

namespace zigcheck {

namespace {

using zig::Ast;
using zig::kNone;
using zig::NodeIndex;
using zig::NodeTag;
using zig::TokenIndex;
using zig::TokenTag;

struct Fail {};

enum class Rl { none, ref, ref_const, discard };
enum class Ctx { none, assignment, return_, return_addrof };

struct Ri {
    Rl rl = Rl::none;
    Ctx ctx = Ctx::none;
};

enum class IdCat { local_constant, local_variable, function_parameter, capture, switch_tag_capture };

const char *catName(IdCat c) {
    switch (c) {
        case IdCat::local_constant: return "local constant";
        case IdCat::local_variable: return "local variable";
        case IdCat::function_parameter: return "function parameter";
        case IdCat::capture: return "capture";
        case IdCat::switch_tag_capture: return "switch tag capture";
    }
    return "";
}

enum class ContTarget { none, break_, switch_continue };

// One Scope of AstGen's chain. Kept in a vector (indices stay valid) and
// never freed during a check.
struct S {
    enum class Kind { top, local_val, local_ptr, ns, gen, defer_ } kind = Kind::top;
    int parent = -1;
    // local_val / local_ptr
    std::string name;
    TokenIndex token = 0;
    IdCat cat = IdCat::local_constant;
    TokenIndex used = kNone, discarded = kNone;
    bool used_as_lvalue = false;
    bool maybe_comptime = false;
    int binding = -1;
    // ns
    std::unordered_map<std::string, NodeIndex> decls;
    std::unordered_map<std::string, int> decl_binding;
    NodeIndex node = 0;
    // gen
    TokenIndex label = kNone;
    bool label_used = false;
    int label_binding = -1;
    bool allow_unlabeled = false;
    ContTarget cont = ContTarget::none;
    NodeIndex cur_defer = kNone;
    Ri break_ri;
};

// The GenZir flags that matter, plus the scope it hangs off and the
// Result::scopes entry new names go into.
struct Gz {
    int scope = 0;
    int rscope = -1;
    bool is_comptime = false;
    bool is_typeof = false;
    bool c_import = false;
    NodeIndex any_defer = kNone;
};

bool isPrimitive(std::string_view name) {
    static const char *const kNames[] = {
        "anyerror", "anyframe", "anyopaque", "bool", "c_int", "c_long", "c_longdouble", "c_longlong",
        "c_char", "c_short", "c_uint", "c_ulong", "c_ulonglong", "c_ushort", "comptime_float", "comptime_int",
        "f128", "f16", "f32", "f64", "f80", "false", "i16", "i32", "i64", "i128", "i8", "isize", "noreturn",
        "null", "true", "type", "u16", "u29", "u32", "u64", "u128", "u1", "u8", "undefined", "usize", "void",
    };
    for (const char *n : kNames)
        if (name == n) return true;
    if (name.size() < 2) return false;
    if (name[0] != 'i' && name[0] != 'u') return false;
    for (std::size_t i = 1; i < name.size(); ++i)
        if (name[i] < '0' || name[i] > '9') return false;
    return true;
}

// Fixed-name primitive values/types (primitive_instrs).
bool isPrimitiveInstr(std::string_view name) {
    if (!isPrimitive(name)) return false;
    // isPrimitive also accepts every iN/uN; primitive_instrs only the named
    // ones, the others go through the integer path.
    if (name.size() >= 2 && (name[0] == 'i' || name[0] == 'u') && name[1] >= '0' && name[1] <= '9') return false;
    return true;
}

void appendUtf8(std::string &out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The bytes of a string literal token ("..."), escapes decoded.
std::string parseStrLit(std::string_view lit) {
    std::string out;
    if (lit.size() < 2) return out;
    std::string_view s = lit.substr(1, lit.size() - 2);
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) {
            out += c;
            continue;
        }
        char e = s[++i];
        switch (e) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '\\': out += '\\'; break;
            case '\'': out += '\''; break;
            case '"': out += '"'; break;
            case 'x':
                if (i + 2 < s.size() + 0 && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) {
                    out += static_cast<char>(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
                    i += 2;
                }
                break;
            case 'u': {
                std::size_t j = i + 1;
                if (j < s.size() && s[j] == '{') {
                    std::uint32_t cp = 0;
                    ++j;
                    while (j < s.size() && s[j] != '}') {
                        int h = hexVal(s[j]);
                        if (h < 0) break;
                        cp = cp * 16 + static_cast<std::uint32_t>(h);
                        ++j;
                    }
                    appendUtf8(out, cp);
                    i = j;
                }
                break;
            }
            default: out += e; break;
        }
    }
    return out;
}

class Checker {
public:
    explicit Checker(const Ast &tree) : t(tree) {}

    Result run() {
        S top;
        top.kind = S::Kind::top;
        scopes.push_back(top);
        Gz g;
        g.scope = 0;
        g.is_comptime = true;
        // generate(): the file is a struct.
        Ast::NodeBuffer2 buf;
        (void)buf;
        zig::full::ContainerDecl root = t.containerDeclRoot();
        try {
            for (NodeIndex m : root.ast.members) {
                auto f = t.fullContainerField(m);
                if (f && f->ast.tuple_like) failNode(m, "file cannot be a tuple");
            }
            structDecl(g, 0, root, true);
        } catch (const Fail &) {
        }
        return std::move(res);
    }

private:
    const Ast &t;
    std::vector<S> scopes;
    Result res;
    bool in_fn = false;  // astgen.fn_block != null

    // ---- positions and errors ----------------------------------------------

    std::uint32_t tokStart(TokenIndex tok) const { return t.tokenStart(tok); }
    std::uint32_t tokEnd(TokenIndex tok) const {
        return t.tokenStart(tok) + static_cast<std::uint32_t>(t.tokenSlice(tok).size());
    }
    std::uint32_t nodeMain(NodeIndex n) const { return t.nodeToSpan(n).main; }
    std::uint32_t nodeEnd(NodeIndex n) const { return tokEnd(t.lastToken(n)); }

    Note noteTok(TokenIndex tok, std::string msg) const { return Note{tokStart(tok), std::move(msg)}; }
    Note noteNode(NodeIndex n, std::string msg) const { return Note{nodeMain(n), std::move(msg)}; }

    static std::string codeFor(const std::string &msg) {
        auto starts = [&](const char *p) { return msg.rfind(p, 0) == 0; };
        if (starts("use of undeclared identifier")) return "undeclared-identifier";
        if (starts("unused function parameter")) return "unused-parameter";
        if (starts("unused capture") || starts("unused switch tag capture")) return "unused-capture";
        if (starts("unused local")) return "unused-local";
        if (starts("unused ") && msg.find("label") != std::string::npos) return "unused-label";
        if (starts("pointless discard")) return "pointless-discard";
        if (starts("local variable is never mutated")) return "never-mutated";
        if (msg.find("shadows") != std::string::npos) return "shadowing";
        if (starts("redeclaration")) return "redeclaration";
        if (starts("duplicate")) return "duplicate-name";
        if (starts("invalid builtin function")) return "invalid-builtin";
        if (starts("expected ") && msg.find("argument") != std::string::npos) return "builtin-arity";
        if (msg.find("outside function scope") != std::string::npos) return "outside-function";
        if (starts("unreachable code")) return "unreachable-code";
        if (starts("label not found") || starts("redefinition of label")) return "label";
        if (starts("discard of") || starts("pointer modifier invalid on discard")) return "discard";
        return "astgen";
    }

    void append(std::uint32_t at, std::uint32_t end, std::string msg, std::vector<Note> notes = {}) {
        Diagnostic d;
        d.byte_offset = at;
        d.end_offset = std::max(at, end);
        d.code = codeFor(msg);
        d.message = std::move(msg);
        d.notes = std::move(notes);
        res.diagnostics.push_back(std::move(d));
    }
    void appendTok(TokenIndex tok, std::string msg, std::vector<Note> notes = {}) {
        append(tokStart(tok), tokEnd(tok), std::move(msg), std::move(notes));
    }
    void appendNode(NodeIndex n, std::string msg, std::vector<Note> notes = {}) {
        append(nodeMain(n), nodeEnd(n), std::move(msg), std::move(notes));
    }
    [[noreturn]] void failTok(TokenIndex tok, std::string msg, std::vector<Note> notes = {}) {
        appendTok(tok, std::move(msg), std::move(notes));
        throw Fail{};
    }
    [[noreturn]] void failNode(NodeIndex n, std::string msg, std::vector<Note> notes = {}) {
        appendNode(n, std::move(msg), std::move(notes));
        throw Fail{};
    }

    // ---- names ---------------------------------------------------------------

    // identAsString: `@"name"` and `name` are the same name.
    std::string identName(TokenIndex tok) const {
        std::string_view raw = t.tokenSlice(tok);
        if (!raw.empty() && raw[0] == '@') return parseStrLit(raw.substr(1));
        return std::string(raw);
    }
    bool tokenIdentEql(TokenIndex a, TokenIndex b) const { return identName(a) == identName(b); }

    // ---- scopes and results -------------------------------------------------

    int push(S s) {
        scopes.push_back(std::move(s));
        return static_cast<int>(scopes.size() - 1);
    }
    Gz subBlock(const Gz &g, int parent_scope) {
        S s;
        s.kind = S::Kind::gen;
        s.parent = parent_scope;
        Gz n = g;
        n.scope = push(std::move(s));
        return n;
    }
    Gz subBlock(const Gz &g) { return subBlock(g, g.scope); }

    int openResultScope(int parent, std::uint32_t start, std::uint32_t end, bool is_container, NodeIndex node) {
        Scope s;
        s.start = start;
        s.end = end;
        s.parent = parent;
        s.is_container = is_container;
        s.node = node;
        res.scopes.push_back(std::move(s));
        return static_cast<int>(res.scopes.size() - 1);
    }
    int openResultScope(const Gz &g, NodeIndex node) {
        return openResultScope(g.rscope, t.tokenStart(t.firstToken(node)), nodeEnd(node), false, node);
    }
    int addBinding(int rscope, TokenIndex name_token, BindingKind kind, NodeIndex node) {
        Binding b;
        b.name_token = name_token;
        b.kind = kind;
        b.node = node;
        b.scope = rscope;
        res.bindings.push_back(b);
        int idx = static_cast<int>(res.bindings.size() - 1);
        if (rscope >= 0) res.scopes[static_cast<std::size_t>(rscope)].bindings.push_back(idx);
        return idx;
    }
    void addRef(TokenIndex tok, int binding) {
        if (binding >= 0) res.references.emplace_back(tok, binding);
    }

    static BindingKind kindFor(IdCat c) {
        switch (c) {
            case IdCat::local_constant: return BindingKind::local_const;
            case IdCat::local_variable: return BindingKind::local_var;
            case IdCat::function_parameter: return BindingKind::param;
            default: return BindingKind::capture;
        }
    }

    // A Scope.LocalVal / LocalPtr on top of `parent`.
    int pushLocal(const Gz &g, int parent, bool ptr, TokenIndex name_tok, IdCat cat, bool maybe_comptime,
                  NodeIndex decl_node) {
        S s;
        s.kind = ptr ? S::Kind::local_ptr : S::Kind::local_val;
        s.parent = parent;
        s.name = identName(name_tok);
        s.token = name_tok;
        s.cat = cat;
        s.maybe_comptime = maybe_comptime;
        s.binding = addBinding(g.rscope, name_tok, kindFor(cat), decl_node);
        return push(std::move(s));
    }

    S &sc(int i) { return scopes[static_cast<std::size_t>(i)]; }

    // ---- AstGen: scope checks -----------------------------------------------

    void detectLocalShadowing(int scope, TokenIndex name_token, IdCat id_cat) {
        std::string_view raw = t.tokenSlice(name_token);
        if (raw[0] != '@' && isPrimitive(raw)) {
            failTok(name_token, "name shadows primitive '" + std::string(raw) + "'",
                    {noteTok(name_token, "consider using @\"" + std::string(raw) + "\" to disambiguate")});
        }
        std::string name = identName(name_token);
        bool outer = false;
        for (int i = scope; i >= 0;) {
            S &s = sc(i);
            switch (s.kind) {
                case S::Kind::local_val:
                case S::Kind::local_ptr:
                    if (s.name == name) {
                        if (outer) {
                            failTok(name_token,
                                    std::string(catName(id_cat)) + " '" + name + "' shadows " + catName(s.cat) +
                                        " from outer scope",
                                    {noteTok(s.token, "previous declaration here")});
                        }
                        failTok(name_token, std::string("redeclaration of ") + catName(s.cat) + " '" + name + "'",
                                {noteTok(s.token, "previous declaration here")});
                    }
                    break;
                case S::Kind::ns: {
                    outer = true;
                    auto it = s.decls.find(name);
                    if (it != s.decls.end()) {
                        failTok(name_token, std::string(catName(id_cat)) + " shadows declaration of '" + name + "'",
                                {noteNode(it->second, "declared here")});
                    }
                    break;
                }
                case S::Kind::gen: outer = true; break;
                case S::Kind::defer_: break;
                case S::Kind::top: return;
            }
            i = s.parent;
        }
    }

    void checkUsed(int outer, int inner) {
        for (int i = inner; i != outer && i >= 0;) {
            S &s = sc(i);
            if (s.kind == S::Kind::local_val) {
                if (s.used == kNone && s.discarded == kNone) {
                    appendTok(s.token, std::string("unused ") + catName(s.cat));
                } else if (s.used != kNone && s.discarded != kNone) {
                    appendTok(s.discarded, std::string("pointless discard of ") + catName(s.cat),
                              {noteTok(s.used, "used here")});
                }
            } else if (s.kind == S::Kind::local_ptr) {
                if (s.used == kNone && s.discarded == kNone) {
                    appendTok(s.token, std::string("unused ") + catName(s.cat));
                } else {
                    if (s.used != kNone && s.discarded != kNone) {
                        appendTok(s.discarded, std::string("pointless discard of ") + catName(s.cat),
                                  {noteTok(s.used, "used here")});
                    }
                    if (s.cat == IdCat::local_variable && !s.used_as_lvalue) {
                        appendTok(s.token, "local variable is never mutated",
                                  {noteTok(s.token, "consider using 'const'")});
                    }
                }
            }
            i = s.parent;
        }
    }

    void checkLabelRedefinition(int scope, TokenIndex label) {
        for (int i = scope; i >= 0;) {
            S &s = sc(i);
            if (s.kind == S::Kind::ns || s.kind == S::Kind::top) return;
            if (s.kind == S::Kind::gen && s.label != kNone && tokenIdentEql(label, s.label)) {
                failTok(label, "redefinition of label '" + identName(label) + "'",
                        {noteTok(s.label, "previous definition here")});
            }
            i = s.parent;
        }
    }

    void setLabel(const Gz &g, int gen_scope, TokenIndex label) {
        S &s = sc(gen_scope);
        s.label = label;
        s.label_used = false;
        s.label_binding = addBinding(g.rscope, label, BindingKind::label, 0);
    }

    // ---- identifiers ---------------------------------------------------------

    void identifier(const Gz &g, Ri ri, NodeIndex ident, bool force_comptime) {
        TokenIndex tok = t.nodeMainToken(ident);
        std::string_view raw = t.tokenSlice(tok);
        if (raw == "_") failNode(ident, "'_' used as an identifier without @\"_\" syntax");
        if (raw[0] != '@') {
            if (isPrimitiveInstr(raw)) return;
            if (raw.size() >= 2 && (raw[0] == 'i' || raw[0] == 'u')) {
                bool digits = true;
                for (std::size_t i = 1; i < raw.size(); ++i)
                    if (raw[i] < '0' || raw[i] > '9') digits = false;
                if (raw.size() >= 3 && raw[1] == '0') {
                    failNode(ident, "primitive integer type '" + std::string(raw) + "' has leading zero");
                }
                if (digits) {
                    // parseBitCount: overflow past u16.
                    std::uint32_t v = 0;
                    bool overflow = false;
                    for (std::size_t i = 1; i < raw.size(); ++i) {
                        v = v * 10 + static_cast<std::uint32_t>(raw[i] - '0');
                        if (v > 65535) {
                            overflow = true;
                            break;
                        }
                    }
                    if (overflow) {
                        failNode(ident, "primitive integer type '" + std::string(raw) +
                                            "' exceeds maximum bit width of 65535");
                    }
                    return;
                }
            }
        }
        if (force_comptime) ri.rl = Rl::none;
        localVarRef(g, ri, ident, tok);
    }

    void localVarRef(const Gz &g, Ri ri, NodeIndex ident, TokenIndex tok) {
        std::string name = identName(tok);
        NodeIndex found = kNone;
        int found_binding = -1;
        std::uint32_t namespaces_out = 0;
        NodeIndex capturing_ns = 0;
        for (int i = g.scope; i >= 0;) {
            S &s = sc(i);
            switch (s.kind) {
                case S::Kind::local_val:
                    if (s.name == name) {
                        if (ri.rl == Rl::discard && ri.ctx == Ctx::assignment)
                            s.discarded = tok;
                        else
                            s.used = tok;
                        addRef(tok, s.binding);
                        return;
                    }
                    break;
                case S::Kind::local_ptr:
                    if (s.name == name) {
                        if (ri.rl == Rl::discard && ri.ctx == Ctx::assignment)
                            s.discarded = tok;
                        else
                            s.used = tok;
                        addRef(tok, s.binding);
                        if (!s.maybe_comptime && !g.is_typeof) {
                            if (namespaces_out != 0) {
                                failNode(ident, "mutable '" + name + "' not accessible from here",
                                         {noteTok(s.token, "declared mutable here"),
                                          noteNode(capturing_ns, "crosses namespace boundary here")});
                            } else if (ri.ctx == Ctx::return_addrof) {
                                failNode(ident, "returning address of expired local variable '" + name + "'",
                                         {noteTok(s.token, "declared runtime-known here")});
                            }
                        }
                        if (ri.rl == Rl::ref) s.used_as_lvalue = true;
                        return;
                    }
                    break;
                case S::Kind::ns: {
                    auto it = s.decls.find(name);
                    if (it != s.decls.end()) {
                        if (found != kNone) {
                            failNode(ident, "ambiguous reference",
                                     {noteNode(found, "declared here"), noteNode(it->second, "also declared here")});
                        }
                        found = it->second;
                        auto b = s.decl_binding.find(name);
                        found_binding = b == s.decl_binding.end() ? -1 : b->second;
                    }
                    ++namespaces_out;
                    capturing_ns = s.node;
                    break;
                }
                case S::Kind::gen:
                case S::Kind::defer_: break;
                case S::Kind::top: i = -1; continue;
            }
            i = s.parent;
        }
        if (found == kNone) failNode(ident, "use of undeclared identifier '" + name + "'");
        addRef(tok, found_binding);
    }

    // ---- noreturn ------------------------------------------------------------

    // Whether AstGen's result for this expression is a noreturn instruction
    // (addEnsureResult / refIsNoReturn): return, break, continue,
    // unreachable, @compileError, @panic, @trap.
    bool isNoReturn(NodeIndex n) const {
        while (true) {
            switch (t.nodeTag(n)) {
                case NodeTag::return_:
                case NodeTag::break_:
                case NodeTag::continue_:
                case NodeTag::unreachable_literal: return true;
                case NodeTag::grouped_expression: n = t.nodeData(n).node_and_token().first; continue;
                case NodeTag::builtin_call_two:
                case NodeTag::builtin_call_two_comma:
                case NodeTag::builtin_call:
                case NodeTag::builtin_call_comma: {
                    std::string_view name = t.tokenSlice(t.nodeMainToken(n));
                    return name == "@compileError" || name == "@panic" || name == "@trap";
                }
                default: return false;
            }
        }
    }

    void reachableExpr(const Gz &g, Ri ri, NodeIndex node, NodeIndex reachable_node) {
        expr(g, ri, node);
        if (isNoReturn(node)) {
            appendNode(reachable_node, "unreachable code", {noteNode(node, "control flow is diverted here")});
        }
    }
    void reachableExprComptime(const Gz &g, Ri ri, NodeIndex node, NodeIndex reachable_node, bool comptime) {
        if (comptime)
            comptimeExpr(g, ri, node);
        else
            expr(g, ri, node);
        if (isNoReturn(node)) {
            appendNode(reachable_node, "unreachable code", {noteNode(node, "control flow is diverted here")});
        }
    }

    // ---- comptime ------------------------------------------------------------

    void typeExpr(const Gz &g, NodeIndex n) { comptimeExpr(g, Ri{}, n); }

    void comptimeExpr(const Gz &g, Ri ri, NodeIndex node) {
        if (g.is_comptime) {
            expr(g, ri, node);
            return;
        }
        switch (t.nodeTag(node)) {
            case NodeTag::identifier: identifier(g, ri, node, true); return;
            case NodeTag::number_literal:
            case NodeTag::char_literal:
            case NodeTag::string_literal:
            case NodeTag::multiline_string_literal:
            case NodeTag::enum_literal:
            case NodeTag::error_value:
            case NodeTag::anyframe_literal:
            case NodeTag::error_set_decl:
            case NodeTag::error_union:
            case NodeTag::merge_error_sets:
            case NodeTag::optional_type:
            case NodeTag::anyframe_type:
            case NodeTag::ptr_type_aligned:
            case NodeTag::ptr_type_sentinel:
            case NodeTag::ptr_type:
            case NodeTag::ptr_type_bit_range:
            case NodeTag::array_type:
            case NodeTag::array_type_sentinel:
            case NodeTag::fn_proto_simple:
            case NodeTag::fn_proto_multi:
            case NodeTag::fn_proto_one:
            case NodeTag::fn_proto:
            case NodeTag::container_decl:
            case NodeTag::container_decl_trailing:
            case NodeTag::container_decl_arg:
            case NodeTag::container_decl_arg_trailing:
            case NodeTag::container_decl_two:
            case NodeTag::container_decl_two_trailing:
            case NodeTag::tagged_union:
            case NodeTag::tagged_union_trailing:
            case NodeTag::tagged_union_enum_tag:
            case NodeTag::tagged_union_enum_tag_trailing:
            case NodeTag::tagged_union_two:
            case NodeTag::tagged_union_two_trailing: expr(g, ri, node); return;
            case NodeTag::block_two:
            case NodeTag::block_two_semicolon:
            case NodeTag::block:
            case NodeTag::block_semicolon: {
                TokenIndex lbrace = t.nodeMainToken(node);
                if (t.isTokenPrecededByTags(lbrace, {TokenTag::identifier, TokenTag::colon})) {
                    Ast::NodeBuffer2 buf;
                    auto stmts = *t.blockStatements(buf, node);
                    labeledBlockExpr(g, Ri{Rl::none, ri.ctx}, node, stmts, true);
                    return;
                }
                break;
            }
            default: break;
        }
        Gz c = subBlock(g);
        c.is_comptime = true;
        c.scope = g.scope;  // fullBodyExpr(&block_scope, scope, ...)
        fullBodyExpr(c, Ri{Rl::none, ri.ctx}, node);
    }

    // ---- blocks --------------------------------------------------------------

    void fullBodyExpr(const Gz &g, Ri ri, NodeIndex node) {
        Ast::NodeBuffer2 buf;
        auto stmts = t.blockStatements(buf, node);
        if (!stmts) {
            expr(g, ri, node);
            return;
        }
        TokenIndex lbrace = t.nodeMainToken(node);
        if (t.isTokenPrecededByTags(lbrace, {TokenTag::identifier, TokenTag::colon})) {
            labeledBlockExpr(g, ri, node, *stmts, false);
            return;
        }
        Gz sub = subBlock(g);
        blockExprStmts(sub, node, *stmts);
    }

    void blockExpr(const Gz &g, Ri ri, NodeIndex node, std::span<const NodeIndex> stmts) {
        TokenIndex lbrace = t.nodeMainToken(node);
        if (t.isTokenPrecededByTags(lbrace, {TokenTag::identifier, TokenTag::colon})) {
            labeledBlockExpr(g, ri, node, stmts, false);
            return;
        }
        Gz sub = subBlock(g);
        blockExprStmts(sub, node, stmts);
    }

    void labeledBlockExpr(const Gz &g, Ri ri, NodeIndex node, std::span<const NodeIndex> stmts, bool force_comptime) {
        TokenIndex label = t.nodeMainToken(node) - 2;
        checkLabelRedefinition(g.scope, label);
        Gz b = subBlock(g);
        if (force_comptime) b.is_comptime = true;
        setLabel(g, b.scope, label);
        sc(b.scope).break_ri = ri;
        sc(b.scope).cont = ContTarget::none;
        blockExprStmts(b, node, stmts);
        if (!sc(b.scope).label_used) appendTok(label, "unused block label");
    }

    void blockExprStmts(Gz g, NodeIndex block_node, std::span<const NodeIndex> stmts) {
        if (stmts.empty()) return;
        g.rscope = openResultScope(g, block_node);
        const int parent_scope = g.scope;
        NodeIndex noreturn_src = kNone;
        for (NodeIndex statement : stmts) {
            if (noreturn_src != kNone) {
                appendNode(statement, "unreachable code", {noteNode(noreturn_src, "control flow is diverted here")});
            }
            NodeIndex inner = statement;
            while (true) {
                switch (t.nodeTag(inner)) {
                    case NodeTag::global_var_decl:
                    case NodeTag::local_var_decl:
                    case NodeTag::simple_var_decl:
                    case NodeTag::aligned_var_decl: g.scope = varDecl(g, statement); break;
                    case NodeTag::assign_destructure: g.scope = assignDestructureMaybeDecls(g, statement); break;
                    case NodeTag::defer_: g.scope = deferStmt(g, statement, false); break;
                    case NodeTag::errdefer_: g.scope = deferStmt(g, statement, true); break;
                    case NodeTag::assign: assign(g, statement); break;
                    case NodeTag::assign_shl:
                    case NodeTag::assign_shr:
                    case NodeTag::assign_bit_and:
                    case NodeTag::assign_bit_or:
                    case NodeTag::assign_bit_xor:
                    case NodeTag::assign_div:
                    case NodeTag::assign_sub:
                    case NodeTag::assign_sub_wrap:
                    case NodeTag::assign_mod:
                    case NodeTag::assign_add:
                    case NodeTag::assign_add_wrap:
                    case NodeTag::assign_mul:
                    case NodeTag::assign_mul_wrap: assignOp(g, statement); break;
                    case NodeTag::grouped_expression: inner = t.nodeData(inner).node_and_token().first; continue;
                    case NodeTag::while_simple:
                    case NodeTag::while_cont:
                    case NodeTag::while_: whileExpr(g, Ri{}, inner, *t.fullWhile(inner), true); break;
                    case NodeTag::for_simple:
                    case NodeTag::for_: forExpr(g, Ri{}, inner, *t.fullFor(inner), true); break;
                    default:
                        expr(g, Ri{}, inner);
                        noreturn_src = isNoReturn(inner) ? inner : kNone;
                        break;
                }
                break;
            }
        }
        checkUsed(parent_scope, g.scope);
    }

    // ---- statements ----------------------------------------------------------

    int deferStmt(const Gz &g, NodeIndex node, bool is_err) {
        Gz d = subBlock(g);
        sc(d.scope).cur_defer = node;
        d.any_defer = node;
        int sub_scope = d.scope;
        NodeIndex expr_node;
        if (is_err) {
            auto [payload, e] = t.nodeData(node).opt_token_and_node();
            expr_node = e;
            if (payload) {
                TokenIndex ptok = payload.unwrap();
                if (t.tokenSlice(ptok) == "_") {
                    appendTok(ptok, "discard of error capture; omit it instead");
                } else {
                    d.rscope = openResultScope(g, node);
                    sub_scope = pushLocal(d, d.scope, false, ptok, IdCat::capture, false, 0);
                }
            }
        } else {
            expr_node = t.nodeData(node).node();
        }
        Gz e = d;
        e.scope = sub_scope;
        expr(e, Ri{}, expr_node);
        checkUsed(g.scope, sub_scope);
        S s;
        s.kind = S::Kind::defer_;
        s.parent = g.scope;
        return push(std::move(s));
    }

    int varDecl(const Gz &g, NodeIndex node) {
        zig::full::VarDecl vd = *t.fullVarDecl(node);
        TokenIndex name_token = vd.ast.mut_token + 1;
        std::string_view raw = t.tokenSlice(name_token);
        if (raw == "_") failTok(name_token, "'_' used as an identifier without @\"_\" syntax");
        bool is_const = t.tokenTag(vd.ast.mut_token) == TokenTag::keyword_const;
        detectLocalShadowing(g.scope, name_token, is_const ? IdCat::local_constant : IdCat::local_variable);
        if (!vd.ast.init_node) failNode(node, "variables must be initialized");
        NodeIndex init_node = vd.ast.init_node.unwrap();
        if (vd.ast.addrspace_node) {
            failTok(t.nodeMainToken(vd.ast.addrspace_node.unwrap()),
                    "cannot set address space of local variable '" + std::string(raw) + "'");
        }
        if (vd.ast.section_node) {
            failTok(t.nodeMainToken(vd.ast.section_node.unwrap()),
                    "cannot set section of local variable '" + std::string(raw) + "'");
        }
        if (vd.ast.align_node) expr(g, Ri{}, vd.ast.align_node.unwrap());
        if (is_const) {
            if (vd.comptime_token) {
                appendTok(*vd.comptime_token,
                          "'comptime const' is redundant; instead wrap the initialization expression with 'comptime'");
            }
            bool force_comptime = vd.comptime_token.has_value();
            if (vd.ast.type_node) typeExpr(g, vd.ast.type_node.unwrap());
            reachableExprComptime(g, Ri{}, init_node, node, force_comptime);
            return pushLocal(g, g.scope, false, name_token, IdCat::local_constant, true, node);
        }
        if (vd.comptime_token && g.is_comptime) {
            failTok(*vd.comptime_token, "'comptime var' is redundant in comptime scope");
        }
        bool is_comptime = vd.comptime_token.has_value() || g.is_comptime;
        if (vd.ast.type_node) typeExpr(g, vd.ast.type_node.unwrap());
        reachableExprComptime(g, Ri{}, init_node, node, vd.comptime_token.has_value());
        return pushLocal(g, g.scope, true, name_token, IdCat::local_variable, is_comptime, node);
    }

    void assign(const Gz &g, NodeIndex node) {
        auto [lhs, rhs] = t.nodeData(node).node_and_node();
        if (t.nodeTag(lhs) == NodeTag::identifier && t.tokenSlice(t.nodeMainToken(lhs)) == "_") {
            expr(g, Ri{Rl::discard, Ctx::assignment}, rhs);
            return;
        }
        lvalExpr(g, lhs);
        expr(g, Ri{}, rhs);
    }

    void assignOp(const Gz &g, NodeIndex node) {
        auto [lhs, rhs] = t.nodeData(node).node_and_node();
        lvalExpr(g, lhs);
        expr(g, Ri{}, rhs);
    }

    void assignDestructure(const Gz &g, NodeIndex node) {
        zig::full::AssignDestructure full = t.assignDestructure(node);
        if (full.comptime_token && g.is_comptime) {
            appendTok(*full.comptime_token, "redundant comptime keyword in already comptime scope");
            return;
        }
        Gz inner = g;
        if (full.comptime_token) {
            inner = subBlock(g);
            inner.is_comptime = true;
        }
        for (NodeIndex v : full.ast.variables) {
            if (t.nodeTag(v) == NodeTag::identifier && t.tokenSlice(t.nodeMainToken(v)) == "_") continue;
            lvalExpr(inner, v);
        }
        expr(inner, Ri{}, full.ast.value_expr);
    }

    int assignDestructureMaybeDecls(const Gz &g, NodeIndex node) {
        zig::full::AssignDestructure full = t.assignDestructure(node);
        if (full.comptime_token && g.is_comptime) {
            appendTok(*full.comptime_token, "redundant comptime keyword in already comptime scope");
        }
        bool is_comptime = full.comptime_token.has_value() || g.is_comptime;
        bool any_non_const = false, any_lvalue_expr = false;
        for (NodeIndex v : full.ast.variables) {
            switch (t.nodeTag(v)) {
                case NodeTag::identifier:
                    if (t.tokenSlice(t.nodeMainToken(v)) == "_") {
                        any_non_const = true;
                        continue;
                    }
                    break;
                case NodeTag::global_var_decl:
                case NodeTag::local_var_decl:
                case NodeTag::simple_var_decl:
                case NodeTag::aligned_var_decl: {
                    zig::full::VarDecl vd = *t.fullVarDecl(v);
                    TokenIndex name_token = vd.ast.mut_token + 1;
                    std::string_view raw = t.tokenSlice(name_token);
                    if (raw == "_") failTok(name_token, "'_' used as an identifier without @\"_\" syntax");
                    if (vd.ast.addrspace_node) {
                        failTok(t.nodeMainToken(vd.ast.addrspace_node.unwrap()),
                                "cannot set address space of local variable '" + std::string(raw) + "'");
                    }
                    if (vd.ast.section_node) {
                        failTok(t.nodeMainToken(vd.ast.section_node.unwrap()),
                                "cannot set section of local variable '" + std::string(raw) + "'");
                    }
                    if (t.tokenTag(vd.ast.mut_token) == TokenTag::keyword_var) any_non_const = true;
                    if (vd.ast.align_node) expr(g, Ri{}, vd.ast.align_node.unwrap());
                    if (vd.ast.type_node) typeExpr(g, vd.ast.type_node.unwrap());
                    continue;
                }
                default: break;
            }
            any_non_const = true;
            any_lvalue_expr = true;
        }
        if (full.comptime_token && !any_non_const) {
            appendTok(*full.comptime_token,
                      "'comptime const' is redundant; instead wrap the initialization expression with 'comptime'");
        }
        Gz inner = g;
        if (full.comptime_token) {
            inner = subBlock(g);
            inner.is_comptime = true;
            inner.scope = g.scope;
        }
        if (any_lvalue_expr) {
            for (NodeIndex v : full.ast.variables) {
                switch (t.nodeTag(v)) {
                    case NodeTag::global_var_decl:
                    case NodeTag::local_var_decl:
                    case NodeTag::simple_var_decl:
                    case NodeTag::aligned_var_decl: continue;
                    case NodeTag::identifier:
                        if (t.tokenSlice(t.nodeMainToken(v)) == "_") continue;
                        break;
                    default: break;
                }
                lvalExpr(inner, v);
            }
        }
        reachableExpr(inner, Ri{}, full.ast.value_expr, node);
        int cur = g.scope;
        for (NodeIndex v : full.ast.variables) {
            switch (t.nodeTag(v)) {
                case NodeTag::local_var_decl:
                case NodeTag::simple_var_decl:
                case NodeTag::aligned_var_decl: break;
                default: continue;
            }
            zig::full::VarDecl vd = *t.fullVarDecl(v);
            bool is_const = t.tokenTag(vd.ast.mut_token) == TokenTag::keyword_const;
            TokenIndex name_token = vd.ast.mut_token + 1;
            detectLocalShadowing(cur, name_token, is_const ? IdCat::local_constant : IdCat::local_variable);
            cur = pushLocal(g, cur, true, name_token, is_const ? IdCat::local_constant : IdCat::local_variable,
                            is_const || is_comptime, v);
        }
        return cur;
    }

    void lvalExpr(const Gz &g, NodeIndex node) {
        switch (t.nodeTag(node)) {
            case NodeTag::unwrap_optional:
            case NodeTag::deref:
            case NodeTag::field_access:
            case NodeTag::array_access:
            case NodeTag::identifier:
            case NodeTag::grouped_expression:
            case NodeTag::orelse_: break;
            case NodeTag::builtin_call:
            case NodeTag::builtin_call_comma:
            case NodeTag::builtin_call_two:
            case NodeTag::builtin_call_two_comma: {
                const zig_lsp::BuiltinInfo *info = zig_lsp::findBuiltin(t.tokenSlice(t.nodeMainToken(node)));
                if (info && !info->allows_lvalue) failNode(node, "invalid left-hand side to assignment");
                break;
            }
            default: failNode(node, "invalid left-hand side to assignment");
        }
        expr(g, Ri{Rl::ref, Ctx::none}, node);
    }

    // ---- control flow --------------------------------------------------------

    void ret(const Gz &g, NodeIndex node) {
        if (!in_fn) failNode(node, "'return' outside function scope");
        if (g.any_defer != kNone) {
            failNode(node, "cannot return from defer expression", {noteNode(g.any_defer, "defer expression here")});
        }
        auto operand = t.nodeData(node).opt_node();
        if (!operand) return;
        reachableExpr(g, Ri{Rl::none, Ctx::return_}, operand.unwrap(), node);
    }

    void breakExpr(const Gz &g, NodeIndex node) {
        auto [opt_label, opt_rhs] = t.nodeData(node).opt_token_and_opt_node();
        for (int i = g.scope; i >= 0;) {
            S &s = sc(i);
            if (s.kind == S::Kind::gen) {
                if (s.cur_defer != kNone) {
                    failNode(node, "cannot break out of defer expression",
                             {noteNode(s.cur_defer, "defer expression here")});
                }
                bool target = false;
                if (opt_label) {
                    if (s.label != kNone && tokenIdentEql(s.label, opt_label.unwrap())) {
                        s.label_used = true;
                        addRef(opt_label.unwrap(), s.label_binding);
                        target = true;
                    }
                } else if (s.allow_unlabeled) {
                    target = true;
                }
                if (target) {
                    if (opt_rhs) {
                        Ri bri = s.break_ri;
                        reachableExpr(g, bri, opt_rhs.unwrap(), node);
                    }
                    return;
                }
            } else if (s.kind == S::Kind::ns || s.kind == S::Kind::top) {
                if (opt_label) {
                    failTok(opt_label.unwrap(), "label not found: '" + identName(opt_label.unwrap()) + "'");
                }
                failNode(node, "break expression outside loop");
            }
            i = s.parent;
        }
    }

    void continueExpr(const Gz &g, NodeIndex node) {
        auto [opt_label, opt_rhs] = t.nodeData(node).opt_token_and_opt_node();
        if (!opt_label && opt_rhs) failNode(node, "cannot continue with operand without label");
        for (int i = g.scope; i >= 0;) {
            S &s = sc(i);
            if (s.kind == S::Kind::gen) {
                if (s.cur_defer != kNone) {
                    failNode(node, "cannot continue out of defer expression",
                             {noteNode(s.cur_defer, "defer expression here")});
                }
                if (opt_label) {
                    if (s.label != kNone && tokenIdentEql(s.label, opt_label.unwrap())) {
                        switch (s.cont) {
                            case ContTarget::none:
                                failNode(node, "continue outside of loop or labeled switch expression");
                            case ContTarget::break_:
                                if (opt_rhs) failNode(node, "cannot continue loop with operand");
                                break;
                            case ContTarget::switch_continue:
                                if (!opt_rhs) failNode(node, "cannot continue switch without operand");
                                break;
                        }
                        s.label_used = true;
                        addRef(opt_label.unwrap(), s.label_binding);
                        if (s.cont == ContTarget::switch_continue) reachableExpr(g, Ri{}, opt_rhs.unwrap(), node);
                        return;
                    }
                } else if (s.allow_unlabeled && s.cont == ContTarget::break_) {
                    return;
                }
            } else if (s.kind == S::Kind::ns || s.kind == S::Kind::top) {
                if (opt_label) {
                    failTok(opt_label.unwrap(), "label not found: '" + identName(opt_label.unwrap()) + "'");
                }
                failNode(node, "continue expression outside loop");
            }
            i = s.parent;
        }
    }

    // A capture `|x|` / `|*x|` starting at `payload_token` (the `*` or the
    // name): pushes it on `parent` unless it is `_`. Returns the new scope.
    int capture(const Gz &g, int parent, TokenIndex payload_token, bool allow_discard_ptr_error, NodeIndex body) {
        (void)allow_discard_ptr_error;
        bool is_ref = t.tokenTag(payload_token) == TokenTag::asterisk;
        TokenIndex ident = payload_token + (is_ref ? 1u : 0u);
        if (t.tokenSlice(ident) == "_") {
            if (is_ref) failTok(payload_token, "pointer modifier invalid on discard");
            return parent;
        }
        detectLocalShadowing(parent, ident, IdCat::capture);
        (void)body;
        return pushLocal(g, parent, false, ident, IdCat::capture, false, 0);
    }

    void ifExpr(const Gz &g, Ri ri, NodeIndex node, const zig::full::If &f) {
        (void)node;
        Gz block = subBlock(g);
        bool payload_is_ref = f.payload_token && t.tokenTag(*f.payload_token) == TokenTag::asterisk;
        Ri cond_ri{payload_is_ref ? Rl::ref : Rl::none, Ctx::none};
        if (!f.error_token && !f.payload_token) cond_ri = Ri{};
        expr(block, cond_ri, f.ast.cond_expr);

        Gz then_g = subBlock(g, block.scope);
        then_g.rscope = openResultScope(g, f.ast.then_expr);
        int then_sub = then_g.scope;
        if (f.payload_token) then_sub = capture(then_g, then_g.scope, *f.payload_token, true, f.ast.then_expr);
        Gz tb = then_g;
        tb.scope = then_sub;
        fullBodyExpr(tb, ri, f.ast.then_expr);
        checkUsed(then_g.scope, then_sub);

        if (f.ast.else_expr) {
            NodeIndex else_node = f.ast.else_expr.unwrap();
            Gz else_g = subBlock(g, block.scope);
            else_g.rscope = openResultScope(g, else_node);
            int sub = else_g.scope;
            if (f.error_token) {
                TokenIndex et = *f.error_token;
                if (t.tokenSlice(et) != "_") {
                    detectLocalShadowing(else_g.scope, et, IdCat::capture);
                    sub = pushLocal(else_g, else_g.scope, false, et, IdCat::capture, false, 0);
                }
            }
            Gz eb = else_g;
            eb.scope = sub;
            auto sw = t.fullSwitch(else_node);
            if (sw && f.error_token && sw->label_token && t.nodeTag(sw->ast.condition) == NodeTag::identifier &&
                tokenIdentEql(*f.error_token, t.nodeMainToken(sw->ast.condition))) {
                switchExpr(eb, ri, else_node, *sw, NonErr::peer);
            } else {
                fullBodyExpr(eb, ri, else_node);
            }
            checkUsed(else_g.scope, sub);
        }
    }

    void whileExpr(const Gz &g, Ri ri, NodeIndex node, const zig::full::While &w, bool is_statement) {
        (void)node;
        (void)is_statement;
        if (w.label_token) checkLabelRedefinition(g.scope, *w.label_token);
        bool is_inline = w.inline_token.has_value();
        if (g.is_comptime && is_inline) appendTok(*w.inline_token, "redundant inline keyword in comptime scope");
        Gz loop = subBlock(g);
        Gz cond = subBlock(loop);
        bool payload_is_ref = w.payload_token && t.tokenTag(*w.payload_token) == TokenTag::asterisk;
        Ri cond_ri{payload_is_ref ? Rl::ref : Rl::none, Ctx::none};
        fullBodyExpr(cond, cond_ri, w.ast.cond_expr);

        Gz then_g = subBlock(g, cond.scope);
        then_g.rscope = openResultScope(g, w.ast.then_expr);
        int then_sub = then_g.scope;
        if (w.payload_token) then_sub = capture(then_g, then_g.scope, *w.payload_token, true, w.ast.then_expr);
        Gz cont = subBlock(then_g, then_sub);

        S &ls = sc(loop.scope);
        if (w.label_token) setLabel(g, loop.scope, *w.label_token);
        ls.allow_unlabeled = true;
        ls.cont = ContTarget::break_;
        ls.break_ri = ri;

        if (w.ast.cont_expr) {
            Gz ce = then_g;
            ce.scope = then_sub;
            expr(ce, Ri{}, w.ast.cont_expr.unwrap());
        }
        fullBodyExpr(cont, Ri{}, w.ast.then_expr);
        checkUsed(then_g.scope, then_sub);

        if (w.ast.else_expr) {
            NodeIndex else_node = w.ast.else_expr.unwrap();
            Gz else_g = subBlock(g, cond.scope);
            else_g.rscope = openResultScope(g, else_node);
            int sub = else_g.scope;
            if (w.error_token) {
                TokenIndex et = *w.error_token;
                if (t.tokenSlice(et) != "_") {
                    detectLocalShadowing(else_g.scope, et, IdCat::capture);
                    sub = pushLocal(else_g, else_g.scope, false, et, IdCat::capture, false, 0);
                }
            }
            sc(loop.scope).allow_unlabeled = false;
            sc(loop.scope).cont = ContTarget::none;
            Gz eb = else_g;
            eb.scope = sub;
            fullBodyExpr(eb, ri, else_node);
            checkUsed(else_g.scope, sub);
        }
        if (w.label_token && !sc(loop.scope).label_used) appendTok(*w.label_token, "unused while loop label");
    }

    void forExpr(const Gz &g, Ri ri, NodeIndex node, const zig::full::For &f, bool is_statement) {
        (void)is_statement;
        if (f.label_token) checkLabelRedefinition(g.scope, *f.label_token);
        if (g.is_comptime && f.inline_token) appendTok(*f.inline_token, "redundant inline keyword in comptime scope");
        bool any_len_checks = false;
        {
            TokenIndex capture_token = f.payload_token;
            for (NodeIndex input : f.ast.inputs) {
                bool is_ref = t.tokenTag(capture_token) == TokenTag::asterisk;
                TokenIndex ident = capture_token + (is_ref ? 1u : 0u);
                bool is_discard = t.tokenSlice(ident) == "_";
                if (is_discard && is_ref) failTok(capture_token, "pointer modifier invalid on discard");
                capture_token = ident + 2;
                if (t.nodeTag(input) == NodeTag::for_range) {
                    if (is_ref) failTok(ident, "cannot capture reference to range");
                    auto [start, end] = t.nodeData(input).node_and_opt_node();
                    expr(g, Ri{}, start);
                    if (end) {
                        expr(g, Ri{}, end.unwrap());
                        any_len_checks = true;
                    } else if (is_discard) {
                        appendTok(ident, "discard of unbounded counter");
                    }
                } else {
                    expr(g, Ri{}, input);
                    any_len_checks = true;
                }
            }
        }
        if (!any_len_checks) failNode(node, "unbounded for loop");
        Gz loop = subBlock(g);
        Gz cond = subBlock(loop);
        S &ls = sc(loop.scope);
        if (f.label_token) setLabel(g, loop.scope, *f.label_token);
        ls.allow_unlabeled = true;
        ls.cont = ContTarget::break_;
        ls.break_ri = ri;

        Gz then_g = subBlock(g, cond.scope);
        then_g.rscope = openResultScope(g, f.ast.then_expr);
        int sub = then_g.scope;
        {
            TokenIndex capture_token = f.payload_token;
            for (std::size_t i = 0; i < f.ast.inputs.size(); ++i) {
                bool is_ref = t.tokenTag(capture_token) == TokenTag::asterisk;
                TokenIndex ident = capture_token + (is_ref ? 1u : 0u);
                capture_token = ident + 2;
                if (t.tokenSlice(ident) == "_") continue;
                detectLocalShadowing(sub, ident, IdCat::capture);
                sub = pushLocal(then_g, sub, false, ident, IdCat::capture, false, 0);
            }
        }
        Gz tb = then_g;
        tb.scope = sub;
        fullBodyExpr(tb, Ri{}, f.ast.then_expr);
        checkUsed(then_g.scope, sub);

        if (f.ast.else_expr) {
            NodeIndex else_node = f.ast.else_expr.unwrap();
            Gz else_g = subBlock(g, cond.scope);
            sc(loop.scope).allow_unlabeled = false;
            sc(loop.scope).cont = ContTarget::none;
            fullBodyExpr(else_g, ri, else_node);
        }
        if (f.label_token && !sc(loop.scope).label_used) appendTok(*f.label_token, "unused for loop label");
    }

    enum class NonErr { none, peer, catch_, if_ };

    // switchExpr. For catch_/if_, `node` is the catch / if node; for none
    // and peer, the switch node.
    void switchExpr(const Gz &g, Ri ri, NodeIndex node, const zig::full::Switch &sw, NonErr non_err,
                    const zig::full::If *if_full = nullptr) {
        NodeIndex operand_node = sw.ast.condition;
        TokenIndex err_token = 0;
        if (non_err == NonErr::catch_) {
            operand_node = t.nodeData(node).node_and_node().first;
            err_token = t.nodeMainToken(node) + 2;
        } else if (non_err == NonErr::if_) {
            operand_node = if_full->ast.cond_expr;
            err_token = *if_full->error_token;
        }
        bool is_err_switch = non_err != NonErr::none;
        bool needs_non_err = non_err == NonErr::catch_ || non_err == NonErr::if_;
        enum class Ref { no, yes, yes_const } non_err_is_ref = Ref::no;
        if (non_err == NonErr::catch_) {
            non_err_is_ref = ri.rl == Rl::ref ? Ref::yes : ri.rl == Rl::ref_const ? Ref::yes_const : Ref::no;
        } else if (non_err == NonErr::if_) {
            non_err_is_ref = (if_full->payload_token && t.tokenTag(*if_full->payload_token) == TokenTag::asterisk)
                                 ? Ref::yes
                                 : Ref::no;
        }
        if (sw.label_token) checkLabelRedefinition(g.scope, *sw.label_token);
        if (needs_non_err) {
            if (t.tokenSlice(err_token) == "_") failTok(err_token, "discard of error capture; omit it instead");
            detectLocalShadowing(g.scope, err_token, IdCat::capture);
        }
        bool any_payload_is_ref = false;
        NodeIndex else_case = kNone, underscore = kNone;
        for (NodeIndex case_node : sw.ast.cases) {
            zig::full::SwitchCase c = *t.fullSwitchCase(case_node);
            if (c.payload_token && t.tokenTag(*c.payload_token) == TokenTag::asterisk) any_payload_is_ref = true;
            if (c.ast.values.empty()) {
                if (else_case != kNone) {
                    TokenIndex prev = t.fullSwitchCase(else_case)->ast.arrow_token - 1;
                    failTok(c.ast.arrow_token - 1, "multiple else prongs in switch expression",
                            {noteTok(prev, "previous else prong here")});
                }
                else_case = case_node;
                continue;
            }
            for (NodeIndex val : c.ast.values) {
                NodeTag tag = t.nodeTag(val);
                if (tag == NodeTag::switch_range) continue;
                if (tag == NodeTag::string_literal) failNode(val, "cannot switch on strings");
                if (tag == NodeTag::identifier && t.tokenSlice(t.nodeMainToken(val)) == "_") {
                    if (is_err_switch) {
                        TokenIndex case_src = c.ast.arrow_token - 1;
                        failTok(case_src, "'_' prong is not allowed when switching on errors",
                                {noteTok(case_src, "consider using 'else'")});
                    }
                    if (underscore != kNone) {
                        failNode(val, "multiple '_' prongs in switch expression",
                                 {noteNode(underscore, "previous '_' prong here")});
                    }
                    if (c.inline_token) failNode(val, "cannot inline '_' prong");
                    underscore = val;
                }
            }
        }
        Gz block = subBlock(g);
        Ri operand_ri;
        if (any_payload_is_ref || (needs_non_err && non_err_is_ref == Ref::yes))
            operand_ri.rl = Rl::ref;
        else if (needs_non_err && non_err_is_ref == Ref::yes_const)
            operand_ri.rl = Rl::ref_const;
        if (needs_non_err)
            reachableExpr(g, operand_ri, operand_node, node);
        else
            expr(g, operand_ri, operand_node);

        if (sw.label_token) {
            setLabel(g, block.scope, *sw.label_token);
            sc(block.scope).cont = ContTarget::switch_continue;
        }
        sc(block.scope).break_ri = ri;

        if (non_err == NonErr::if_) {
            Gz scratch = subBlock(g, block.scope);
            scratch.rscope = openResultScope(g, if_full->ast.then_expr);
            int then_sub = scratch.scope;
            if (if_full->payload_token) {
                TokenIndex ident = *if_full->payload_token + (non_err_is_ref != Ref::no ? 1u : 0u);
                if (t.tokenSlice(ident) != "_") {
                    detectLocalShadowing(scratch.scope, ident, IdCat::capture);
                    then_sub = pushLocal(scratch, scratch.scope, false, ident, IdCat::capture, false, 0);
                }
            }
            Gz tb = scratch;
            tb.scope = then_sub;
            fullBodyExpr(tb, sc(block.scope).break_ri, if_full->ast.then_expr);
            checkUsed(scratch.scope, then_sub);
        }

        for (NodeIndex case_node : sw.ast.cases) {
            zig::full::SwitchCase c = *t.fullSwitchCase(case_node);
            Gz scratch = subBlock(g, block.scope);
            // Items, in the outer scope.
            Gz items = scratch;
            items.scope = g.scope;
            for (NodeIndex val : c.ast.values) {
                if (t.nodeTag(val) == NodeTag::switch_range) {
                    auto [a, b] = t.nodeData(val).node_and_node();
                    fullBodyExpr(items, Ri{}, a);
                    fullBodyExpr(items, Ri{}, b);
                } else if (!(t.nodeTag(val) == NodeTag::identifier && t.tokenSlice(t.nodeMainToken(val)) == "_")) {
                    Gz ci = items;
                    ci.is_comptime = true;
                    comptimeItem(ci, val);
                }
            }
            scratch.rscope = openResultScope(g, c.ast.target_expr);
            int switch_scope = scratch.scope;
            if (needs_non_err) switch_scope = pushLocal(scratch, scratch.scope, false, err_token, IdCat::capture, false, 0);
            int body_scope = switch_scope;
            if (c.payload_token) {
                TokenIndex payload_token = *c.payload_token;
                bool capture_is_ref = t.tokenTag(payload_token) == TokenTag::asterisk;
                TokenIndex ident = payload_token + (capture_is_ref ? 1u : 0u);
                bool discarded = t.tokenSlice(ident) == "_";
                int payload_sub = switch_scope;
                if (discarded) {
                    if (capture_is_ref) failTok(payload_token, "pointer modifier invalid on discard");
                } else {
                    detectLocalShadowing(switch_scope, ident, IdCat::capture);
                    payload_sub = pushLocal(scratch, switch_scope, false, ident, IdCat::capture, false, 0);
                }
                if (is_err_switch && capture_is_ref && !discarded) {
                    failTok(ident, "error set cannot be captured by reference");
                }
                if (t.tokenTag(ident + 1) == TokenTag::comma) {
                    TokenIndex tag_token = ident + 2;
                    if (t.tokenSlice(tag_token) == "_") failTok(tag_token, "discard of tag capture; omit it instead");
                    detectLocalShadowing(payload_sub, tag_token, IdCat::switch_tag_capture);
                    if (is_err_switch) failTok(tag_token, "cannot capture tag of error union");
                    body_scope = pushLocal(scratch, payload_sub, false, tag_token, IdCat::switch_tag_capture, false, 0);
                } else if (discarded) {
                    failTok(payload_token, "discard of capture; omit it instead");
                } else {
                    body_scope = payload_sub;
                }
            }
            Gz body = scratch;
            body.scope = body_scope;
            fullBodyExpr(body, sc(block.scope).break_ri, c.ast.target_expr);
            // With an error switch the captured error itself is not checked.
            checkUsed(needs_non_err ? switch_scope : scratch.scope, body_scope);
        }
        if (sw.label_token && !sc(block.scope).label_used) appendTok(*sw.label_token, "unused switch label");
    }

    // A switch item: comptimeExpr in the outer scope.
    void comptimeItem(const Gz &g, NodeIndex val) { expr(g, Ri{}, val); }

    void orelseCatchExpr(const Gz &g, Ri ri, NodeIndex node, std::optional<TokenIndex> payload_token) {
        auto [lhs, rhs] = t.nodeData(node).node_and_node();
        Gz block = subBlock(g);
        Ri operand_ri;
        if (ri.rl == Rl::ref)
            operand_ri.rl = Rl::ref;
        else if (ri.rl == Rl::ref_const)
            operand_ri.rl = Rl::ref_const;
        reachableExpr(block, operand_ri, lhs, rhs);
        Gz else_g = subBlock(block, g.scope);
        int sub = else_g.scope;
        if (payload_token) {
            TokenIndex p = *payload_token;
            if (t.tokenSlice(p) == "_") {
                appendTok(p, "discard of error capture; omit it instead");
            } else {
                detectLocalShadowing(g.scope, p, IdCat::capture);
                else_g.rscope = openResultScope(g, rhs);
                sub = pushLocal(else_g, else_g.scope, false, p, IdCat::capture, false, 0);
            }
        }
        Gz eb = else_g;
        eb.scope = sub;
        auto sw = t.fullSwitch(rhs);
        if (sw && t.nodeTag(node) == NodeTag::catch_ && payload_token && sw->label_token &&
            t.nodeTag(sw->ast.condition) == NodeTag::identifier &&
            tokenIdentEql(*payload_token, t.nodeMainToken(sw->ast.condition))) {
            switchExpr(eb, ri, rhs, *sw, NonErr::peer);
        } else {
            fullBodyExpr(eb, ri, rhs);
        }
        checkUsed(else_g.scope, sub);
    }

    void tryExpr(const Gz &g, Ri ri, NodeIndex node, NodeIndex operand) {
        if (!in_fn) failNode(node, "'try' outside function scope");
        if (g.any_defer != kNone) {
            failNode(node, "'try' not allowed inside defer expression",
                     {noteNode(g.any_defer, "defer expression here")});
        }
        Ri ori;
        if (ri.rl == Rl::ref)
            ori.rl = Rl::ref;
        else if (ri.rl == Rl::ref_const)
            ori.rl = Rl::ref_const;
        expr(g, ori, operand);
    }

    // ---- calls and builtins --------------------------------------------------

    void callExpr(const Gz &g, NodeIndex node) {
        Ast::NodeBuffer1 buf;
        zig::full::Call call = *t.fullCall(buf, node);
        NodeIndex callee = call.ast.fn_expr;
        if (t.nodeTag(callee) == NodeTag::field_access) {
            expr(g, Ri{Rl::ref, Ctx::none}, t.nodeData(callee).node_and_token().first);
        } else if (t.nodeTag(callee) != NodeTag::enum_literal) {
            expr(g, Ri{}, callee);
        }
        for (NodeIndex p : call.ast.params) expr(g, Ri{}, p);
    }

    void builtinCall(const Gz &g, Ri ri, NodeIndex node, std::span<const NodeIndex> params) {
        std::string_view name = t.tokenSlice(t.nodeMainToken(node));
        const zig_lsp::BuiltinInfo *info = zig_lsp::findBuiltin(name);
        if (!info) failNode(node, "invalid builtin function: '" + std::string(name) + "'");
        if (info->param_count >= 0 && static_cast<std::size_t>(info->param_count) != params.size()) {
            std::uint32_t expected = static_cast<std::uint32_t>(info->param_count);
            failNode(node, "expected " + std::to_string(expected) + " argument" + (expected == 1 ? "" : "s") +
                               ", found " + std::to_string(params.size()));
        }
        if (!in_fn && info->illegal_outside_function) {
            failNode(node, "'" + std::string(name) + "' outside function scope");
        }
        if (name == "@import") {
            NodeIndex op = params[0];
            if (t.nodeTag(op) != NodeTag::string_literal) failNode(op, "@import operand must be a string literal");
            std::string s = parseStrLit(t.tokenSlice(t.nodeMainToken(op)));
            if (s.find('\0') != std::string::npos)
                failTok(t.nodeMainToken(op), "import path cannot contain null bytes");
            if (s.empty()) failTok(t.nodeMainToken(op), "import path cannot be empty");
            return;
        }
        if (name == "@field") {
            Ri lri;
            lri.rl = (ri.rl == Rl::ref) ? Rl::ref : Rl::ref_const;
            expr(g, lri, params[0]);
            comptimeExpr(g, Ri{}, params[1]);
            return;
        }
        if (name == "@TypeOf") {
            if (params.empty()) failNode(node, "expected at least 1 argument, found 0");
            Gz ts = subBlock(g);
            ts.is_typeof = true;
            for (NodeIndex p : params) reachableExpr(ts, Ri{}, p, node);
            return;
        }
        if (name == "@cImport") {
            Gz cs = subBlock(g);
            cs.is_comptime = true;
            cs.c_import = true;
            fullBodyExpr(cs, Ri{}, params[0]);
            return;
        }
        if (name == "@cDefine" && !g.c_import) failNode(node, "C define valid only inside C import block");
        for (NodeIndex p : params) expr(g, Ri{}, p);
    }

    // ---- expr ----------------------------------------------------------------

    void expr(const Gz &g, Ri ri, NodeIndex node) {
        const auto d = t.nodeData(node);
        switch (t.nodeTag(node)) {
            case NodeTag::assign: assign(g, node); return;
            case NodeTag::assign_destructure: assignDestructure(g, node); return;
            case NodeTag::assign_shl:
            case NodeTag::assign_shl_sat:
            case NodeTag::assign_shr:
            case NodeTag::assign_bit_and:
            case NodeTag::assign_bit_or:
            case NodeTag::assign_bit_xor:
            case NodeTag::assign_div:
            case NodeTag::assign_sub:
            case NodeTag::assign_sub_wrap:
            case NodeTag::assign_sub_sat:
            case NodeTag::assign_mod:
            case NodeTag::assign_add:
            case NodeTag::assign_add_wrap:
            case NodeTag::assign_add_sat:
            case NodeTag::assign_mul:
            case NodeTag::assign_mul_wrap:
            case NodeTag::assign_mul_sat: assignOp(g, node); return;
            case NodeTag::shl:
            case NodeTag::shr:
            case NodeTag::add:
            case NodeTag::add_wrap:
            case NodeTag::add_sat:
            case NodeTag::sub:
            case NodeTag::sub_wrap:
            case NodeTag::sub_sat:
            case NodeTag::mul:
            case NodeTag::mul_wrap:
            case NodeTag::mul_sat:
            case NodeTag::div:
            case NodeTag::mod:
            case NodeTag::shl_sat:
            case NodeTag::bit_and:
            case NodeTag::bit_or:
            case NodeTag::bit_xor:
            case NodeTag::bang_equal:
            case NodeTag::equal_equal:
            case NodeTag::greater_than:
            case NodeTag::greater_or_equal:
            case NodeTag::less_than:
            case NodeTag::less_or_equal:
            case NodeTag::array_cat:
            case NodeTag::bool_and:
            case NodeTag::bool_or: {
                auto [a, b] = d.node_and_node();
                expr(g, Ri{}, a);
                expr(g, Ri{}, b);
                return;
            }
            case NodeTag::array_mult: {
                auto [a, b] = d.node_and_node();
                expr(g, Ri{}, a);
                comptimeExpr(g, Ri{}, b);
                return;
            }
            case NodeTag::error_union:
            case NodeTag::merge_error_sets: {
                auto [a, b] = d.node_and_node();
                reachableExprComptime(g, Ri{}, a, node, true);
                reachableExprComptime(g, Ri{}, b, node, true);
                return;
            }
            case NodeTag::bool_not:
            case NodeTag::bit_not:
            case NodeTag::negation:
            case NodeTag::negation_wrap: expr(g, Ri{}, d.node()); return;
            case NodeTag::identifier: identifier(g, ri, node, false); return;
            case NodeTag::asm_simple:
            case NodeTag::asm_: asmExpr(g, node); return;
            case NodeTag::string_literal:
            case NodeTag::multiline_string_literal:
            case NodeTag::number_literal:
            case NodeTag::char_literal:
            case NodeTag::unreachable_literal:
            case NodeTag::enum_literal:
            case NodeTag::error_value:
            case NodeTag::anyframe_literal: return;
            case NodeTag::builtin_call_two:
            case NodeTag::builtin_call_two_comma:
            case NodeTag::builtin_call:
            case NodeTag::builtin_call_comma: {
                Ast::NodeBuffer2 buf;
                auto params = *t.builtinCallParams(buf, node);
                builtinCall(g, ri, node, params);
                return;
            }
            case NodeTag::call_one:
            case NodeTag::call_one_comma:
            case NodeTag::call:
            case NodeTag::call_comma: callExpr(g, node); return;
            case NodeTag::return_: ret(g, node); return;
            case NodeTag::field_access: {
                Ri lri;
                lri.rl = (ri.rl == Rl::ref) ? Rl::ref : Rl::ref_const;
                expr(g, lri, d.node_and_token().first);
                return;
            }
            case NodeTag::if_simple:
            case NodeTag::if_: {
                zig::full::If f = *t.fullIf(node);
                if (f.error_token && f.ast.else_expr) {
                    auto sw = t.fullSwitch(f.ast.else_expr.unwrap());
                    if (sw && !sw->label_token && t.nodeTag(sw->ast.condition) == NodeTag::identifier &&
                        tokenIdentEql(*f.error_token, t.nodeMainToken(sw->ast.condition))) {
                        switchExpr(g, ri, node, *sw, NonErr::if_, &f);
                        return;
                    }
                }
                ifExpr(g, ri, node, f);
                return;
            }
            case NodeTag::while_simple:
            case NodeTag::while_cont:
            case NodeTag::while_: whileExpr(g, ri, node, *t.fullWhile(node), false); return;
            case NodeTag::for_simple:
            case NodeTag::for_: forExpr(g, ri, node, *t.fullFor(node), false); return;
            case NodeTag::slice_open:
            case NodeTag::slice:
            case NodeTag::slice_sentinel: {
                zig::full::Slice s = *t.fullSlice(node);
                expr(g, Ri{Rl::ref, Ctx::none}, s.ast.sliced);
                expr(g, Ri{}, s.ast.start);
                if (s.ast.end) expr(g, Ri{}, s.ast.end.unwrap());
                if (s.ast.sentinel) expr(g, Ri{}, s.ast.sentinel.unwrap());
                return;
            }
            case NodeTag::deref: expr(g, Ri{}, d.node()); return;
            case NodeTag::address_of: {
                Ri ori{Rl::ref, ri.ctx == Ctx::return_ ? Ctx::return_addrof : Ctx::none};
                expr(g, ori, d.node());
                return;
            }
            case NodeTag::optional_type: typeExpr(g, d.node()); return;
            case NodeTag::unwrap_optional: {
                Ri lri;
                lri.rl = ri.rl == Rl::ref ? Rl::ref : ri.rl == Rl::ref_const ? Rl::ref_const : Rl::none;
                expr(g, lri, d.node_and_token().first);
                return;
            }
            case NodeTag::block_two:
            case NodeTag::block_two_semicolon:
            case NodeTag::block:
            case NodeTag::block_semicolon: {
                Ast::NodeBuffer2 buf;
                auto stmts = *t.blockStatements(buf, node);
                blockExpr(g, ri, node, stmts);
                return;
            }
            case NodeTag::anyframe_type: typeExpr(g, d.token_and_node().second); return;
            case NodeTag::catch_: {
                TokenIndex catch_token = t.nodeMainToken(node);
                std::optional<TokenIndex> payload;
                if (t.tokenTag(catch_token + 1) == TokenTag::pipe) payload = catch_token + 2;
                if (payload) {
                    auto sw = t.fullSwitch(d.node_and_node().second);
                    if (sw && !sw->label_token && t.nodeTag(sw->ast.condition) == NodeTag::identifier &&
                        tokenIdentEql(*payload, t.nodeMainToken(sw->ast.condition))) {
                        switchExpr(g, ri, node, *sw, NonErr::catch_);
                        return;
                    }
                }
                orelseCatchExpr(g, ri, node, payload);
                return;
            }
            case NodeTag::orelse_: orelseCatchExpr(g, ri, node, std::nullopt); return;
            case NodeTag::ptr_type_aligned:
            case NodeTag::ptr_type_sentinel:
            case NodeTag::ptr_type:
            case NodeTag::ptr_type_bit_range: {
                zig::full::PtrType p = *t.fullPtrType(node);
                typeExpr(g, p.ast.child_type);
                if (p.ast.sentinel) comptimeExpr(g, Ri{}, p.ast.sentinel.unwrap());
                if (p.ast.addrspace_node) comptimeExpr(g, Ri{}, p.ast.addrspace_node.unwrap());
                if (p.ast.align_node) comptimeExpr(g, Ri{}, p.ast.align_node.unwrap());
                if (p.ast.bit_range_start) {
                    comptimeExpr(g, Ri{}, p.ast.bit_range_start.unwrap());
                    comptimeExpr(g, Ri{}, p.ast.bit_range_end.unwrap());
                }
                return;
            }
            case NodeTag::container_decl:
            case NodeTag::container_decl_trailing:
            case NodeTag::container_decl_arg:
            case NodeTag::container_decl_arg_trailing:
            case NodeTag::container_decl_two:
            case NodeTag::container_decl_two_trailing:
            case NodeTag::tagged_union:
            case NodeTag::tagged_union_trailing:
            case NodeTag::tagged_union_enum_tag:
            case NodeTag::tagged_union_enum_tag_trailing:
            case NodeTag::tagged_union_two:
            case NodeTag::tagged_union_two_trailing: {
                Ast::NodeBuffer2 buf;
                containerDecl(g, node, *t.fullContainerDecl(buf, node));
                return;
            }
            case NodeTag::break_: breakExpr(g, node); return;
            case NodeTag::continue_: continueExpr(g, node); return;
            case NodeTag::grouped_expression: expr(g, ri, d.node_and_token().first); return;
            case NodeTag::array_type:
            case NodeTag::array_type_sentinel: {
                zig::full::ArrayType a = *t.fullArrayType(node);
                NodeIndex len = a.ast.elem_count;
                if (t.nodeTag(len) == NodeTag::identifier && t.tokenSlice(t.nodeMainToken(len)) == "_") {
                    failNode(len, "unable to infer array size");
                }
                comptimeExpr(g, Ri{}, len);
                typeExpr(g, a.ast.elem_type);
                if (a.ast.sentinel) comptimeExpr(g, Ri{}, a.ast.sentinel.unwrap());
                return;
            }
            case NodeTag::error_set_decl: errorSetDecl(node); return;
            case NodeTag::array_access: {
                auto [a, b] = d.node_and_node();
                Ri lri;
                lri.rl = ri.rl == Rl::ref ? Rl::ref : Rl::ref_const;
                expr(g, lri, a);
                expr(g, Ri{}, b);
                return;
            }
            case NodeTag::comptime_: {
                if (g.is_comptime) {
                    appendTok(t.nodeMainToken(node), "redundant comptime keyword in already comptime scope");
                }
                NodeIndex body = d.node();
                Ri bri = ri;
                if (t.nodeTag(body) == NodeTag::identifier) {
                    if (g.is_comptime) {
                        expr(g, ri, body);
                    } else {
                        identifier(g, ri, body, true);
                    }
                    return;
                }
                comptimeExpr(g, bri, body);
                return;
            }
            case NodeTag::switch_:
            case NodeTag::switch_comma: switchExpr(g, ri, node, *t.fullSwitch(node), NonErr::none); return;
            case NodeTag::nosuspend_: expr(g, ri, d.node()); return;
            case NodeTag::suspend_: fullBodyExpr(subBlock(g), Ri{}, d.node()); return;
            case NodeTag::resume_: expr(g, Ri{Rl::ref, Ctx::none}, d.node()); return;
            case NodeTag::try_: tryExpr(g, ri, node, d.node()); return;
            case NodeTag::array_init_one:
            case NodeTag::array_init_one_comma:
            case NodeTag::array_init_dot_two:
            case NodeTag::array_init_dot_two_comma:
            case NodeTag::array_init_dot:
            case NodeTag::array_init_dot_comma:
            case NodeTag::array_init:
            case NodeTag::array_init_comma: {
                Ast::NodeBuffer2 buf;
                zig::full::ArrayInit a = *t.fullArrayInit(buf, node);
                if (a.ast.type_expr) arrayInitType(g, a.ast.type_expr.unwrap());
                for (NodeIndex e : a.ast.elements) expr(g, Ri{}, e);
                return;
            }
            case NodeTag::struct_init_one:
            case NodeTag::struct_init_one_comma:
            case NodeTag::struct_init_dot_two:
            case NodeTag::struct_init_dot_two_comma:
            case NodeTag::struct_init_dot:
            case NodeTag::struct_init_dot_comma:
            case NodeTag::struct_init:
            case NodeTag::struct_init_comma: {
                Ast::NodeBuffer2 buf;
                zig::full::StructInit s = *t.fullStructInit(buf, node);
                if (s.ast.type_expr) arrayInitType(g, s.ast.type_expr.unwrap());
                for (NodeIndex e : s.ast.fields) expr(g, Ri{}, e);
                return;
            }
            case NodeTag::fn_proto_simple:
            case NodeTag::fn_proto_multi:
            case NodeTag::fn_proto_one:
            case NodeTag::fn_proto: {
                Ast::NodeBuffer1 buf;
                fnProtoExpr(g, node, *t.fullFnProto(buf, node));
                return;
            }
            default: return;
        }
    }

    // `[_]T{...}` / `T{...}`: the type, with `[_]` allowed.
    void arrayInitType(const Gz &g, NodeIndex type_node) {
        NodeTag tag = t.nodeTag(type_node);
        if (tag == NodeTag::array_type || tag == NodeTag::array_type_sentinel) {
            zig::full::ArrayType a = *t.fullArrayType(type_node);
            NodeIndex len = a.ast.elem_count;
            if (!(t.nodeTag(len) == NodeTag::identifier && t.tokenSlice(t.nodeMainToken(len)) == "_")) {
                comptimeExpr(g, Ri{}, len);
            }
            typeExpr(g, a.ast.elem_type);
            if (a.ast.sentinel) comptimeExpr(g, Ri{}, a.ast.sentinel.unwrap());
            return;
        }
        typeExpr(g, type_node);
    }

    void errorSetDecl(NodeIndex node) {
        auto [lbrace, rbrace] = t.nodeData(node).token_and_token();
        std::unordered_map<std::string, TokenIndex> seen;
        for (TokenIndex i = lbrace + 1; i < rbrace; ++i) {
            if (t.tokenTag(i) != TokenTag::identifier) continue;
            std::string name = identName(i);
            auto [it, inserted] = seen.emplace(name, i);
            if (!inserted) {
                failTok(i, "duplicate error set field '" + name + "'", {noteTok(it->second, "previous declaration here")});
            }
        }
    }

    void asmExpr(const Gz &g, NodeIndex node) {
        zig::full::Asm a = *t.fullAsm(node);
        comptimeExpr(g, Ri{}, a.ast.template_);
        for (NodeIndex out : a.outputs) {
            TokenIndex symbolic_name = t.nodeMainToken(out);
            bool has_arrow = t.tokenTag(symbolic_name + 4) == TokenTag::arrow;
            if (has_arrow) {
                auto ty = t.nodeData(out).opt_node_and_token().first;
                if (ty) typeExpr(g, ty.unwrap());
            } else {
                localVarRef(g, Ri{Rl::ref, Ctx::none}, node, symbolic_name + 4);
            }
        }
        for (NodeIndex in : a.inputs) expr(g, Ri{}, t.nodeData(in).node_and_token().first);
        if (a.ast.clobbers) comptimeExpr(g, Ri{}, a.ast.clobbers.unwrap());
    }

    void fnProtoExpr(const Gz &g, NodeIndex node, const zig::full::FnProto &fp) {
        (void)node;
        if (fp.name_token) failTok(*fp.name_token, "function type cannot have a name");
        if (fp.ast.align_expr) failNode(fp.ast.align_expr.unwrap(), "function type cannot have an alignment");
        if (fp.ast.addrspace_expr) failNode(fp.ast.addrspace_expr.unwrap(), "function type cannot have an addrspace");
        if (fp.ast.section_expr) failNode(fp.ast.section_expr.unwrap(), "function type cannot have a linksection");
        NodeIndex rt = fp.ast.return_type.unwrap();
        TokenIndex maybe_bang = t.firstToken(rt) - 1;
        if (t.tokenTag(maybe_bang) == TokenTag::bang) failTok(maybe_bang, "function type cannot have an inferred error set");
        fnProtoExprInner(g, fp);
    }

    void fnProtoExprInner(const Gz &g, const zig::full::FnProto &fp) {
        Gz b = subBlock(g);
        b.scope = g.scope;
        auto it = fp.iterate(t);
        while (auto p = it.next()) {
            if (p->anytype_ellipsis3) {
                if (t.tokenTag(*p->anytype_ellipsis3) == TokenTag::ellipsis3) break;
                continue;
            }
            Gz pg = b;
            pg.is_comptime = true;
            fullBodyExpr(pg, Ri{}, *p->type_expr);
        }
        Gz c = b;
        c.is_comptime = true;
        if (fp.ast.callconv_expr) comptimeExpr(c, Ri{}, fp.ast.callconv_expr.unwrap());
        comptimeExpr(c, Ri{}, fp.ast.return_type.unwrap());
    }

    // ---- declarations --------------------------------------------------------

    void containerDecl(const Gz &g, NodeIndex node, const zig::full::ContainerDecl &cd) {
        bool prev_in_fn = in_fn;
        in_fn = false;
        struct Restore {
            bool &r;
            bool v;
            ~Restore() { r = v; }
        } restore{in_fn, prev_in_fn};
        switch (t.tokenTag(cd.ast.main_token)) {
            case TokenTag::keyword_struct: structDecl(g, node, cd, false); return;
            case TokenTag::keyword_union: unionDecl(g, node, cd); return;
            case TokenTag::keyword_enum: enumDecl(g, node, cd); return;
            case TokenTag::keyword_opaque: opaqueDecl(g, node, cd); return;
            default: return;
        }
    }

    enum class CKind { struct_, union_, enum_, opaque_ };

    // Scope.Namespace + scanContainer. Returns the namespace scope.
    Gz openNamespace(const Gz &g, NodeIndex node, const zig::full::ContainerDecl &cd, CKind kind, bool is_root) {
        S ns;
        ns.kind = S::Kind::ns;
        ns.parent = g.scope;
        ns.node = node;
        Gz n = g;
        n.scope = push(std::move(ns));
        n.is_comptime = true;
        n.is_typeof = false;
        n.any_defer = kNone;
        std::uint32_t start = is_root ? 0 : t.tokenStart(t.firstToken(node));
        std::uint32_t end = is_root ? static_cast<std::uint32_t>(t.source.size()) : nodeEnd(node);
        n.rscope = openResultScope(g.rscope, start, end, true, node);
        scanContainer(n, cd.ast.members, kind);
        // The block_scope (a GenZir) sits on the namespace.
        Gz b = subBlock(n);
        return b;
    }

    void scanContainer(const Gz &n, std::span<const NodeIndex> members, CKind kind) {
        static const char *const kKindNames[] = {"struct", "union", "enum", "opaque"};
        const char *kind_name = kKindNames[static_cast<int>(kind)];
        S &ns = sc(n.scope);
        bool any_invalid = false;
        bool any_duplicates = false;
        struct Entry {
            std::vector<TokenIndex> toks;
        };
        std::vector<std::string> order, test_order, decltest_order;
        std::unordered_map<std::string, Entry> names, test_names, decltest_names;
        for (NodeIndex m : members) {
            bool is_field = false;
            TokenIndex name_token = 0;
            switch (t.nodeTag(m)) {
                case NodeTag::container_field_init:
                case NodeTag::container_field_align:
                case NodeTag::container_field: {
                    zig::full::ContainerField f = *t.fullContainerField(m);
                    if (kind == CKind::union_ || kind == CKind::enum_) f.convertToNonTupleLike(t);
                    if (f.ast.tuple_like) continue;
                    is_field = true;
                    name_token = f.ast.main_token;
                    addBinding(n.rscope, name_token, BindingKind::field, m);
                    break;
                }
                case NodeTag::global_var_decl:
                case NodeTag::local_var_decl:
                case NodeTag::simple_var_decl:
                case NodeTag::aligned_var_decl: name_token = t.nodeMainToken(m) + 1; break;
                case NodeTag::fn_proto_simple:
                case NodeTag::fn_proto_multi:
                case NodeTag::fn_proto_one:
                case NodeTag::fn_proto:
                case NodeTag::fn_decl: {
                    TokenIndex ident = t.nodeMainToken(m) + 1;
                    if (t.tokenTag(ident) != TokenTag::identifier) {
                        appendNode(m, "missing function name");
                        any_invalid = true;
                        continue;
                    }
                    name_token = ident;
                    break;
                }
                case NodeTag::comptime_: continue;
                case NodeTag::test_decl: {
                    TokenIndex tn = t.nodeMainToken(m) + 1;
                    if (t.tokenTag(tn) == TokenTag::string_literal) {
                        std::string name = parseStrLit(t.tokenSlice(tn));
                        auto &e = test_names[name];
                        if (e.toks.empty()) test_order.push_back(name);
                        else any_duplicates = true;
                        e.toks.push_back(tn);
                    } else if (t.tokenTag(tn) == TokenTag::identifier) {
                        std::string name = identName(tn);
                        auto &e = decltest_names[name];
                        if (e.toks.empty()) decltest_order.push_back(name);
                        else any_duplicates = true;
                        e.toks.push_back(tn);
                    }
                    continue;
                }
                default: continue;
            }
            std::string name = identName(name_token);
            if (!is_field) {
                ns.decls[name] = m;
                if (!ns.decl_binding.count(name)) {
                    BindingKind bk = BindingKind::container_const;
                    NodeTag tag = t.nodeTag(m);
                    if (tag == NodeTag::fn_decl || tag == NodeTag::fn_proto || tag == NodeTag::fn_proto_one ||
                        tag == NodeTag::fn_proto_multi || tag == NodeTag::fn_proto_simple) {
                        bk = BindingKind::function;
                    } else if (t.tokenTag(t.nodeMainToken(m)) == TokenTag::keyword_var) {
                        bk = BindingKind::container_var;
                    }
                    ns.decl_binding[name] = addBinding(n.rscope, name_token, bk, m);
                }
            }
            auto &e = names[name];
            if (!e.toks.empty()) {
                e.toks.push_back(name_token);
                any_duplicates = true;
                continue;
            }
            order.push_back(name);
            e.toks.push_back(name_token);
            if (is_field) continue;
            std::string_view bytes = t.tokenSlice(name_token);
            if (bytes[0] != '@' && isPrimitive(bytes)) {
                appendTok(name_token, "name shadows primitive '" + std::string(bytes) + "'",
                          {noteTok(name_token, "consider using @\"" + std::string(bytes) + "\" to disambiguate")});
                any_invalid = true;
                continue;
            }
            for (int i = ns.parent; i >= 0;) {
                S &s = sc(i);
                if ((s.kind == S::Kind::local_val || s.kind == S::Kind::local_ptr) && s.name == name) {
                    appendTok(name_token,
                              "declaration '" + std::string(bytes) + "' shadows " + catName(s.cat) + " from outer scope",
                              {noteTok(s.token, "previous declaration here")});
                    any_invalid = true;
                    break;
                }
                if (s.kind == S::Kind::top) break;
                i = s.parent;
            }
        }
        if (!any_duplicates) {
            if (any_invalid) throw Fail{};
            return;
        }
        auto report = [&](const std::vector<std::string> &ord, std::unordered_map<std::string, Entry> &map,
                          const char *dup_note, const std::string &msg_prefix, const char *msg_suffix) {
            for (const std::string &name : ord) {
                Entry &e = map[name];
                if (e.toks.size() < 2) continue;
                std::vector<Note> notes;
                for (std::size_t i = 1; i < e.toks.size(); ++i) notes.push_back(noteTok(e.toks[i], dup_note));
                notes.push_back(noteNode(ns.node, std::string(kind_name) + " declared here"));
                appendTok(e.toks[0], msg_prefix + name + msg_suffix, std::move(notes));
            }
        };
        report(order, names, "duplicate name here", std::string("duplicate ") + kind_name + " member name '", "'");
        report(test_order, test_names, "duplicate test here", "duplicate test name '", "'");
        report(decltest_order, decltest_names, "duplicate decltest here", "duplicate decltest '", "'");
        throw Fail{};
    }

    // containerMember: a declaration, its errors contained.
    // Returns the field if `m` is one.
    std::optional<zig::full::ContainerField> containerMember(const Gz &g, NodeIndex m) {
        switch (t.nodeTag(m)) {
            case NodeTag::container_field_init:
            case NodeTag::container_field_align:
            case NodeTag::container_field: return t.fullContainerField(m);
            case NodeTag::fn_proto:
            case NodeTag::fn_proto_multi:
            case NodeTag::fn_proto_one:
            case NodeTag::fn_proto_simple:
            case NodeTag::fn_decl: {
                Ast::NodeBuffer1 buf;
                zig::full::FnProto fp = *t.fullFnProto(buf, m);
                std::optional<NodeIndex> body;
                if (t.nodeTag(m) == NodeTag::fn_decl) body = t.nodeData(m).node_and_node().second;
                try {
                    fnDecl(g, m, body, fp);
                } catch (const Fail &) {
                }
                return std::nullopt;
            }
            case NodeTag::global_var_decl:
            case NodeTag::local_var_decl:
            case NodeTag::simple_var_decl:
            case NodeTag::aligned_var_decl:
                try {
                    globalVarDecl(g, m, *t.fullVarDecl(m));
                } catch (const Fail &) {
                }
                return std::nullopt;
            case NodeTag::comptime_:
                try {
                    Gz c = subBlock(g);
                    c.is_comptime = true;
                    c.any_defer = kNone;
                    fullBodyExpr(c, Ri{}, t.nodeData(m).node());
                } catch (const Fail &) {
                }
                return std::nullopt;
            case NodeTag::test_decl:
                try {
                    testDecl(g, m);
                } catch (const Fail &) {
                }
                return std::nullopt;
            default: return std::nullopt;
        }
    }

    void structDecl(const Gz &g, NodeIndex node, const zig::full::ContainerDecl &cd, bool is_root) {
        if (!is_root) {
            for (NodeIndex m : cd.ast.members) {
                auto f = t.fullContainerField(m);
                if (f && f->ast.tuple_like) {
                    tupleDecl(g, node, cd);
                    return;
                }
            }
        }
        Gz b = openNamespace(g, node, cd, CKind::struct_, is_root);
        bool is_packed = cd.layout_token && t.tokenTag(*cd.layout_token) == TokenTag::keyword_packed;
        bool is_extern = cd.layout_token && t.tokenTag(*cd.layout_token) == TokenTag::keyword_extern;
        if (cd.ast.arg) {
            if (!is_packed) failNode(cd.ast.arg.unwrap(), "non-packed struct does not support backing integer type");
            typeExpr(b, cd.ast.arg.unwrap());
        }
        for (NodeIndex m : cd.ast.members) {
            auto f = containerMember(b, m);
            if (!f) continue;
            if (!f->ast.type_expr) failTok(f->ast.main_token, "struct field missing type");
            typeExpr(b, f->ast.type_expr.unwrap());
            if (f->ast.align_expr) {
                if (is_packed) {
                    failNode(f->ast.align_expr.unwrap(), "unable to override alignment of packed struct fields");
                }
                expr(b, Ri{}, f->ast.align_expr.unwrap());
            }
            if (f->ast.value_expr) expr(b, Ri{}, f->ast.value_expr.unwrap());
            if (f->comptime_token) {
                if (is_packed || is_extern) {
                    failTok(*f->comptime_token, std::string(is_packed ? "packed" : "extern") +
                                                    " struct fields cannot be marked comptime");
                }
                if (!f->ast.value_expr) {
                    failTok(f->ast.main_token, "comptime field without default initialization value");
                }
            }
        }
    }

    void tupleDecl(const Gz &g, NodeIndex node, const zig::full::ContainerDecl &cd) {
        if (cd.layout_token) {
            std::string_view l = t.tokenSlice(*cd.layout_token);
            if (l == "extern" || l == "packed") failNode(node, std::string(l) + " tuples are not supported");
        }
        if (cd.ast.arg) failNode(cd.ast.arg.unwrap(), "tuple does not support backing integer type");
        Gz b = g;
        b.is_comptime = true;
        for (NodeIndex m : cd.ast.members) {
            auto f = t.fullContainerField(m);
            if (!f) {
                NodeIndex tuple_member = 0;
                for (NodeIndex mm : cd.ast.members) {
                    auto ff = t.fullContainerField(mm);
                    if (ff && ff->ast.tuple_like) {
                        tuple_member = mm;
                        break;
                    }
                }
                failNode(m, "tuple declarations are not allowed", {noteNode(tuple_member, "tuple field here")});
            }
            if (!f->ast.tuple_like) failTok(f->ast.main_token, "tuple field has a name");
            if (f->ast.align_expr) failTok(f->ast.main_token, "tuple field has alignment");
            if (f->ast.value_expr && !f->comptime_token) {
                failTok(f->ast.main_token, "non-comptime tuple field has default initialization value");
            }
            if (!f->ast.value_expr && f->comptime_token) {
                failTok(*f->comptime_token, "comptime field without default initialization value");
            }
            typeExpr(b, f->ast.type_expr.unwrap());
            if (f->ast.value_expr) expr(b, Ri{}, f->ast.value_expr.unwrap());
        }
    }

    void unionDecl(const Gz &g, NodeIndex node, const zig::full::ContainerDecl &cd) {
        bool is_packed = cd.layout_token && t.tokenTag(*cd.layout_token) == TokenTag::keyword_packed;
        if (cd.layout_token && t.tokenTag(*cd.layout_token) != TokenTag::keyword_packed && false) {
        }
        Gz b = openNamespace(g, node, cd, CKind::union_, false);
        if (cd.ast.arg) typeExpr(b, cd.ast.arg.unwrap());
        bool auto_enum = cd.ast.enum_token.has_value();
        for (NodeIndex m : cd.ast.members) {
            auto f = containerMember(b, m);
            if (!f) continue;
            f->convertToNonTupleLike(t);
            if (f->ast.tuple_like) failTok(f->ast.main_token, "union field missing name");
            if (f->comptime_token) failTok(*f->comptime_token, "union fields cannot be marked comptime");
            if (f->ast.type_expr) {
                typeExpr(b, f->ast.type_expr.unwrap());
            } else if (!cd.ast.arg && !auto_enum) {
                failNode(m, "union field missing type");
            }
            if (f->ast.align_expr) {
                if (is_packed) {
                    failNode(f->ast.align_expr.unwrap(), "unable to override alignment of packed union fields");
                }
                expr(b, Ri{}, f->ast.align_expr.unwrap());
            }
            if (f->ast.value_expr) expr(b, Ri{}, f->ast.value_expr.unwrap());
        }
    }

    void enumDecl(const Gz &g, NodeIndex node, const zig::full::ContainerDecl &cd) {
        if (cd.layout_token) {
            failTok(*cd.layout_token,
                    "enums do not support 'packed' or 'extern'; instead provide an explicit integer tag type");
        }
        Gz b = openNamespace(g, node, cd, CKind::enum_, false);
        if (cd.ast.arg) typeExpr(b, cd.ast.arg.unwrap());
        for (NodeIndex m : cd.ast.members) {
            auto f = containerMember(b, m);
            if (!f) continue;
            f->convertToNonTupleLike(t);
            if (f->ast.tuple_like) failTok(f->ast.main_token, "enum field missing name");
            if (f->comptime_token) failTok(*f->comptime_token, "enum fields cannot be marked comptime");
            if (f->ast.type_expr) {
                failNode(f->ast.type_expr.unwrap(), "enum fields do not have types",
                         {noteNode(node, "consider 'union(enum)' here to make it a tagged union")});
            }
            if (f->ast.align_expr) failNode(f->ast.align_expr.unwrap(), "enum fields cannot be aligned");
            if (f->ast.value_expr) expr(b, Ri{}, f->ast.value_expr.unwrap());
        }
    }

    void opaqueDecl(const Gz &g, NodeIndex node, const zig::full::ContainerDecl &cd) {
        Gz b = openNamespace(g, node, cd, CKind::opaque_, false);
        if (cd.layout_token) failTok(*cd.layout_token, "opaque types do not support 'packed' or 'extern'");
        for (NodeIndex m : cd.ast.members) {
            if (containerMember(b, m)) failNode(m, "opaque types cannot have fields");
        }
    }

    void globalVarDecl(const Gz &g, NodeIndex node, const zig::full::VarDecl &vd) {
        bool is_mutable = t.tokenTag(vd.ast.mut_token) == TokenTag::keyword_var;
        bool is_extern = vd.extern_export_token && t.tokenTag(*vd.extern_export_token) == TokenTag::keyword_extern;
        if (vd.threadlocal_token && !is_mutable) failTok(*vd.threadlocal_token, "threadlocal variable cannot be constant");
        if (vd.lib_name) {
            std::string s = parseStrLit(t.tokenSlice(*vd.lib_name));
            if (s.find('\0') != std::string::npos) failTok(*vd.lib_name, "library name cannot contain null bytes");
            if (s.empty()) failTok(*vd.lib_name, "library name cannot be empty");
        }
        if (vd.ast.init_node) {
            if (is_extern) failNode(vd.ast.init_node.unwrap(), "extern variables have no initializers");
        } else if (!is_extern) {
            failNode(node, "variables must be initialized");
        }
        if (is_extern && !vd.ast.type_node) failNode(node, "unable to infer variable type");
        Gz c = subBlock(g);
        c.is_comptime = true;
        if (vd.ast.type_node) expr(c, Ri{}, vd.ast.type_node.unwrap());
        if (vd.ast.align_node) expr(c, Ri{}, vd.ast.align_node.unwrap());
        if (vd.ast.section_node) expr(c, Ri{}, vd.ast.section_node.unwrap());
        if (vd.ast.addrspace_node) expr(c, Ri{}, vd.ast.addrspace_node.unwrap());
        if (vd.ast.init_node) expr(c, Ri{}, vd.ast.init_node.unwrap());
    }

    void testDecl(const Gz &g, NodeIndex node) {
        auto [opt_name, body] = t.nodeData(node).opt_token_and_node();
        TokenIndex tn = t.nodeMainToken(node) + 1;
        if (t.tokenTag(tn) == TokenTag::string_literal) {
            std::string s = parseStrLit(t.tokenSlice(tn));
            if (s.find('\0') != std::string::npos) failTok(tn, "test name cannot contain null bytes");
            if (s.empty()) failTok(tn, "empty test name must be omitted");
        } else if (t.tokenTag(tn) == TokenTag::identifier) {
            std::string_view raw = t.tokenSlice(tn);
            if (raw == "_") failTok(tn, "'_' used as an identifier without @\"_\" syntax");
            if (raw[0] != '@' && isPrimitive(raw)) failTok(tn, "cannot test a primitive");
            std::string name = identName(tn);
            NodeIndex found = kNone;
            int found_binding = -1;
            for (int i = g.scope; i >= 0;) {
                S &s = sc(i);
                if ((s.kind == S::Kind::local_val || s.kind == S::Kind::local_ptr) && s.name == name) {
                    s.used = tn;
                    failTok(tn, std::string("cannot test a ") + catName(s.cat),
                            {noteTok(s.token, std::string(catName(s.cat)) + " declared here")});
                }
                if (s.kind == S::Kind::ns) {
                    auto it = s.decls.find(name);
                    if (it != s.decls.end()) {
                        if (found != kNone) {
                            failTok(tn, "ambiguous reference",
                                    {noteNode(found, "declared here"), noteNode(it->second, "also declared here")});
                        }
                        found = it->second;
                        auto b = s.decl_binding.find(name);
                        found_binding = b == s.decl_binding.end() ? -1 : b->second;
                    }
                }
                if (s.kind == S::Kind::top) break;
                i = s.parent;
            }
            if (found == kNone) failTok(tn, "use of undeclared identifier '" + name + "'");
            addRef(tn, found_binding);
        }
        (void)opt_name;
        bool prev = in_fn;
        in_fn = true;
        struct Restore {
            bool &r;
            bool v;
            ~Restore() { r = v; }
        } restore{in_fn, prev};
        Gz f = subBlock(g);
        f.is_comptime = false;
        f.any_defer = kNone;
        f.rscope = openResultScope(g, node);
        fullBodyExpr(f, Ri{}, body);
    }

    void fnDecl(const Gz &g, NodeIndex decl_node, std::optional<NodeIndex> body, const zig::full::FnProto &fp) {
        bool is_extern = fp.extern_export_inline_token &&
                         t.tokenTag(*fp.extern_export_inline_token) == TokenTag::keyword_extern;
        bool has_inline = fp.extern_export_inline_token &&
                          t.tokenTag(*fp.extern_export_inline_token) == TokenTag::keyword_inline;
        if (fp.lib_name) {
            std::string s = parseStrLit(t.tokenSlice(*fp.lib_name));
            if (s.find('\0') != std::string::npos) failTok(*fp.lib_name, "library name cannot contain null bytes");
            if (s.empty()) failTok(*fp.lib_name, "library name cannot be empty");
        }
        if (fp.ast.callconv_expr && has_inline) {
            failNode(fp.ast.callconv_expr.unwrap(), "explicit callconv incompatible with inline keyword");
        }
        NodeIndex return_type = fp.ast.return_type.unwrap();
        TokenIndex maybe_bang = t.firstToken(return_type) - 1;
        bool is_inferred_error = t.tokenTag(maybe_bang) == TokenTag::bang;
        if (!body) {
            if (!is_extern) failTok(fp.ast.fn_token, "non-extern function has no body");
            if (is_inferred_error) failTok(maybe_bang, "function prototype may not have inferred error set");
        }
        Gz type_gz = subBlock(g);
        type_gz.is_comptime = true;
        type_gz.scope = g.scope;
        if (is_extern) {
            fnProtoExprInner(type_gz, fp);
        }
        if (fp.ast.align_expr) expr(type_gz, Ri{}, fp.ast.align_expr.unwrap());
        if (fp.ast.section_expr) expr(type_gz, Ri{}, fp.ast.section_expr.unwrap());
        if (fp.ast.addrspace_expr) expr(type_gz, Ri{}, fp.ast.addrspace_expr.unwrap());
        if (!is_extern) fnDeclInner(type_gz, decl_node, *body, fp);
    }

    void fnDeclInner(const Gz &g, NodeIndex decl_node, NodeIndex body_node, const zig::full::FnProto &fp) {
        Gz decl = subBlock(g);
        decl.is_comptime = true;
        decl.rscope = openResultScope(g, decl_node);
        int params_scope = decl.scope;
        auto it = fp.iterate(t);
        while (auto p = it.next()) {
            bool is_anytype = false;
            if (p->anytype_ellipsis3) {
                if (t.tokenTag(*p->anytype_ellipsis3) == TokenTag::ellipsis3) break;
                is_anytype = true;
            }
            bool named = false;
            if (p->name_token) {
                std::string_view nb = t.tokenSlice(*p->name_token);
                if (nb != "_") {
                    detectLocalShadowing(params_scope, *p->name_token, IdCat::function_parameter);
                    named = true;
                }
            } else {
                if (p->anytype_ellipsis3) failTok(*p->anytype_ellipsis3, "missing parameter name");
                NodeIndex te = *p->type_expr;
                if (t.nodeTag(te) == NodeTag::identifier) {
                    std::string ident(t.tokenSlice(t.nodeMainToken(te)));
                    if (!isPrimitive(ident)) {
                        failNode(te, "missing parameter name or type",
                                 {noteNode(te, "if this is a name, annotate its type: '" + ident + ": T'"),
                                  noteNode(te, "if this is a type, give it a name: 'name: " + ident + "'")});
                    }
                }
                failNode(te, "missing parameter name");
            }
            if (!is_anytype) {
                Gz pg = subBlock(decl);
                pg.scope = params_scope;
                fullBodyExpr(pg, Ri{}, *p->type_expr);
            }
            if (!named) continue;
            params_scope = pushLocal(decl, params_scope, false, *p->name_token, IdCat::function_parameter, false, 0);
        }
        Gz ret_gz = subBlock(decl, params_scope);
        ret_gz.scope = params_scope;
        fullBodyExpr(ret_gz, Ri{}, fp.ast.return_type.unwrap());
        if (fp.ast.callconv_expr) {
            Gz cc = subBlock(decl);
            cc.scope = g.scope;
            expr(cc, Ri{}, fp.ast.callconv_expr.unwrap());
        }
        bool prev = in_fn;
        in_fn = true;
        struct Restore {
            bool &r;
            bool v;
            ~Restore() { r = v; }
        } restore{in_fn, prev};
        Gz body = subBlock(decl, params_scope);
        body.is_comptime = false;
        body.any_defer = kNone;
        fullBodyExpr(body, Ri{}, body_node);
        checkUsed(decl.scope, params_scope);
    }
};

}  // namespace

Result Check(const zig::Ast &tree) {
    Checker c(tree);
    return c.run();
}

}  // namespace zigcheck
