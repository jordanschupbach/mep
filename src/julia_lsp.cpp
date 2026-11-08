// mep's Julia language server, analysis half: see julia_lsp.h.
//
// The reader is mep's port of JuliaSyntax (julia_parse.h), so syntax errors
// are Julia's own (Meta.parseall's), message and position. Over its tree:
//
//   * a scope analysis after Julia's rules (julia_lsp_internal.h): modules
//     are global scopes that see Base (and what they `using`), functions,
//     `->`, `do`, macros and struct bodies are hard local scopes, loops,
//     `try`, `let` and comprehensions soft ones; an assignment in a local
//     scope makes a new local unless an enclosing local scope already has
//     the name -- decided per scope before any read is resolved, since a
//     later assignment makes a name local for the whole scope;
//   * the errors Julia raises when *lowering* a file (duplicate argument
//     names, `break` outside a loop, `local`/`global` conflicts, duplicate
//     struct fields or keyword arguments, local `const`), with lowering's
//     messages, checked against Meta.lower over the corpus
//     (tools/julia_lower_check.jl);
//   * lints calibrated on Julia's own base/stdlib and registry packages
//     (`just lsp-lint-corpus julia`): unused locals, unused arguments (a
//     hint: methods implement an interface), unused type parameters (Julia
//     warns about those itself), soft-scope ambiguity, `== nothing`,
//     unreachable code, and undefined names only where the file can tell.
#include "julia_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <unordered_map>

#include "julia_lsp_internal.h"
#include "julia_lsp_vocab.h"

namespace jllsp {

// ---------------------------------------------------------------------------
// Positions

JuliaLspRange Doc::Range(uint32_t b, uint32_t e) const {
    JuliaLspRange r;
    Pos(b, &r.line, &r.col);
    Pos(e, &r.end_line, &r.end_col);
    return r;
}

void Doc::Pos(uint32_t byte, int *line, int *col) const {
    if (byte > text.size()) byte = static_cast<uint32_t>(text.size());
    auto it = std::upper_bound(line_starts.begin(), line_starts.end(), byte);
    const size_t li = static_cast<size_t>(it - line_starts.begin()) - 1;
    *line = static_cast<int>(li);
    *col = static_cast<int>(byte - line_starts[li]);
}

uint32_t Doc::Offset(int line, int col) const {
    if (line < 0) return 0;
    if (static_cast<size_t>(line) >= line_starts.size()) return static_cast<uint32_t>(text.size());
    const uint32_t start = line_starts[static_cast<size_t>(line)];
    const uint32_t len = static_cast<uint32_t>(lines[static_cast<size_t>(line)].size());
    return start + std::min(static_cast<uint32_t>(std::max(col, 0)), len);
}

namespace {

// ---------------------------------------------------------------------------
// The syntax view

K KindOf(std::string_view name) {
    static const std::unordered_map<std::string_view, K> table = {
        {"Identifier", K::Ident}, {"MacroName", K::MacroName}, {"StringMacroName", K::StrMacroName},
        {"CmdMacroName", K::StrMacroName}, {"var", K::Var}, {"call", K::Call}, {"dotcall", K::DotCall},
        {"macrocall", K::MacroCall}, {"function", K::Function}, {"macro", K::Macro}, {"struct", K::Struct},
        {"abstract", K::Abstract}, {"primitive", K::Primitive}, {"module", K::Module}, {"let", K::Let},
        {"for", K::For}, {"while", K::While}, {"if", K::If}, {"elseif", K::Elseif}, {"else", K::Else},
        {"try", K::Try}, {"catch", K::Catch}, {"finally", K::Finally}, {"block", K::Block}, {"=", K::Assign},
        {"+=", K::OpAssign}, {"-=", K::OpAssign}, {"*=", K::OpAssign}, {"/=", K::OpAssign}, {"//=", K::OpAssign},
        {"\\=", K::OpAssign}, {"^=", K::OpAssign}, {"\xc3\xb7=", K::OpAssign}, {"%=", K::OpAssign},
        {"<<=", K::OpAssign}, {">>=", K::OpAssign}, {">>>=", K::OpAssign}, {"|=", K::OpAssign},
        {"&=", K::OpAssign}, {"\xe2\x8a\xbb=", K::OpAssign}, {"$=", K::OpAssign}, {".=", K::DotAssign},
        {"local", K::Local}, {"global", K::Global}, {"const", K::Const}, {"return", K::Return},
        {"break", K::Break}, {"continue", K::Continue}, {"tuple", K::Tuple}, {"parameters", K::Parameters},
        {"->", K::Arrow}, {"do", K::Do}, {"quote", K::Quote}, {"$", K::Interp}, {".", K::Dot}, {"::", K::DColon},
        {"where", K::Where}, {"curly", K::Curly}, {"braces", K::Braces}, {"comprehension", K::Comprehension},
        {"typed_comprehension", K::Comprehension}, {"generator", K::Generator}, {"filter", K::Filter},
        {"cartesian_iterator", K::CartIter}, {"using", K::Using}, {"import", K::Import}, {"export", K::Export},
        {"public", K::Public}, {"importpath", K::ImportPath}, {":", K::Colon}, {"as", K::As}, {"...", K::Splat},
        {"ref", K::Ref}, {"string", K::String}, {"String", K::StrLit}, {"parens", K::Parens}, {"error", K::Error},
        {"toplevel", K::Toplevel}, {"doc", K::Doc}, {"<:", K::Subtype}, {">:", K::Supertype}, {"outer", K::Outer},
        {"inert", K::Inert}, {"false", K::False}, {"true", K::True}, {"?", K::Ternary}, {"&&", K::AndAnd},
        {"||", K::OrOr}, {"vect", K::Vect}, {"Integer", K::Integer}, {"BinInt", K::Integer},
        {"HexInt", K::Integer}, {"OctInt", K::Integer}, {"Float", K::Float}, {"Float32", K::Float},
        {"Char", K::Char},
    };
    auto it = table.find(name);
    return it == table.end() ? K::Other : it->second;
}

void BuildSyntax(Doc &d) {
    // The green tree without trivia: JuliaSyntax's SyntaxNode (an error
    // node is kept even when it is trivia, as SyntaxNode keeps it).
    const jlparse::Tree &g = d.parse.tree;
    d.nodes.clear();
    d.kids.clear();
    if (g.empty()) return;
    std::vector<std::vector<uint32_t>> kidlists;
    struct Frame {
        uint32_t green;
        int parent;
    };
    std::vector<Frame> stack;
    stack.push_back({0, -1});
    std::vector<uint32_t> kept;
    while (!stack.empty()) {
        const Frame f = stack.back();
        stack.pop_back();
        const jlparse::Node &gn = g[f.green];
        Node n;
        n.kind = gn.kind;
        n.flags = gn.flags;
        n.b = gn.offset;
        n.e = gn.offset + gn.span;
        n.parent = f.parent;
        n.leaf = gn.leaf;
        n.k = KindOf(jlparse::KindName(gn.kind));
        if (n.k == K::Assign && (gn.flags & jlparse::DOTOP_FLAG)) n.k = K::DotAssign;  // `x .= y`
        const int id = static_cast<int>(d.nodes.size());
        d.nodes.push_back(n);
        kidlists.emplace_back();
        if (f.parent >= 0) kidlists[static_cast<size_t>(f.parent)].push_back(static_cast<uint32_t>(id));
        kept.clear();
        g.ForEachChild(f.green, [&](uint32_t c) {
            const jlparse::Node &cn = g[c];
            const bool trivia = jlparse::is_trivia(cn.flags) || jlparse::is_whitespace(cn.kind);
            if (!trivia || jlparse::is_error(cn.kind)) kept.push_back(c);
        });
        // Pushed in reverse so they come off the stack in order.
        for (auto it = kept.rbegin(); it != kept.rend(); ++it) stack.push_back({*it, id});
    }
    for (size_t i = 0; i < d.nodes.size(); ++i) {
        d.nodes[i].kid0 = static_cast<uint32_t>(d.kids.size());
        d.nodes[i].nkid = static_cast<uint32_t>(kidlists[i].size());
        d.kids.insert(d.kids.end(), kidlists[i].begin(), kidlists[i].end());
    }
    d.root = 0;
}

bool IsIdent(const Doc &d, int i) { return i >= 0 && (d.N(i).k == K::Ident || d.N(i).k == K::Var); }

// ---------------------------------------------------------------------------
// Scope analysis

struct Analyzer {
    Doc &d;
    explicit Analyzer(Doc &doc) : d(doc) {}

    int NewScope(ScopeKind kind, int parent, int node) {
        Scope s;
        s.kind = kind;
        s.parent = parent;
        s.node = node;
        if (node >= 0) {
            s.b = d.N(node).b;
            s.e = d.N(node).e;
        } else {
            s.e = static_cast<uint32_t>(d.text.size());
        }
        const int id = static_cast<int>(d.scopes.size());
        s.module = kind == ScopeKind::Module ? id : d.scopes[static_cast<size_t>(parent)].module;
        d.scopes.push_back(std::move(s));
        return id;
    }
    Scope &S(int i) { return d.scopes[static_cast<size_t>(i)]; }

    int NewVar(const std::string &name, VarKind kind, int scope, int node, int def_node) {
        Var v;
        v.name = name;
        v.kind = kind;
        v.scope = scope;
        if (node >= 0) {
            v.b = d.N(node).b;
            v.e = d.N(node).e;
        }
        v.def_node = def_node;
        v.underscore = !name.empty() && name[0] == '_';
        if (def_node >= 0) v.defs.push_back(def_node);
        const int id = static_cast<int>(d.vars.size());
        d.vars.push_back(std::move(v));
        S(scope).names[name] = id;
        return id;
    }
    Var &V(int i) { return d.vars[static_cast<size_t>(i)]; }

    void Occur(int node, const std::string &name, int var, bool def, bool write, int scope) {
        Occ o;
        o.b = d.N(node).b;
        o.e = d.N(node).e;
        o.var = var;
        o.def = def;
        o.write = write;
        o.scope = scope;
        o.name = name;
        d.occs.push_back(std::move(o));
        if (var >= 0) {
            if (def || write) V(var).writes++;
            else V(var).reads++;
        }
    }

    // ---- the local-scope rule ------------------------------------------------

    bool IsLocalScope(int s) { return S(s).kind != ScopeKind::Module; }

    // A name an enclosing local scope (up to the module) already binds.
    int EnclosingLocal(int scope, const std::string &name) {
        for (int s = S(scope).parent; s >= 0 && IsLocalScope(s); s = S(s).parent) {
            auto it = S(s).names.find(name);
            if (it != S(s).names.end()) return it->second;
        }
        return -1;
    }

    // Binds @p name, assigned in @p scope: the variable it refers to.
    int Assign(int scope, int node, const std::string &name, VarKind kind, int def_node) {
        auto it = S(scope).names.find(name);
        if (it != S(scope).names.end()) {
            if (def_node >= 0) V(it->second).defs.push_back(def_node);
            return it->second;
        }
        if (IsLocalScope(scope)) {
            const int outer = EnclosingLocal(scope, name);
            if (outer >= 0) return outer;
        }
        if (!IsLocalScope(scope) && kind == VarKind::Local) kind = VarKind::Global;
        return NewVar(name, kind, scope, node, def_node);
    }

    // ---- reading ------------------------------------------------------------

    void Read(int node, int scope) {
        if (node < 0) return;
        const Node &n = d.N(node);
        switch (n.k) {
        case K::Ident:
        case K::Var: {
            const std::string name = IdentName(d, node);
            if (name.empty()) return;
            pending_reads.push_back({node, scope, name});
            return;
        }
        case K::Quote:
            Quoted(node, scope);
            return;
        case K::Inert:
            return;
        case K::MacroName:
        case K::StrMacroName:
            MacroRef(node, scope);
            return;
        case K::Dot:
            // a.b: `b` is a field name, not a variable.
            if (d.NKids(node) >= 1) Read(d.Kid(node, 0), scope);
            for (uint32_t k = 1; k < d.NKids(node); ++k) {
                const int c = d.Kid(node, k);
                if (d.N(c).k == K::Quote || d.N(c).k == K::Inert) {
                    // A macro name `A.@m` stays a macro reference.
                    if (d.NKids(c) == 1 && d.N(d.Kid(c, 0)).k == K::MacroName) continue;
                    continue;
                }
                if (d.N(c).k == K::Ident || d.N(c).k == K::MacroName) continue;
                Read(c, scope);
            }
            return;
        case K::Call:
        case K::DotCall:
            CallArgs(node, scope, false);
            return;
        case K::MacroCall:
            MacroCallNode(node, scope, false);
            return;
        case K::Tuple:
        case K::Parens:
            // Named tuple keys `(a = 1, b = 2)` and `(; a = 1)` are not variables.
            for (uint32_t k = 0; k < d.NKids(node); ++k) {
                const int c = d.Kid(node, k);
                if (d.N(c).k == K::Assign && d.N(node).k == K::Tuple) {
                    if (d.NKids(c) == 2) Read(d.Kid(c, 1), scope);
                } else if (d.N(c).k == K::Parameters) {
                    for (uint32_t j = 0; j < d.NKids(c); ++j) {
                        const int p = d.Kid(c, j);
                        if (d.N(p).k == K::Assign && d.NKids(p) == 2) Read(d.Kid(p, 1), scope);
                        else Read(p, scope);
                    }
                } else {
                    Statement(c, scope);
                }
            }
            return;
        default:
            break;
        }
        if (n.leaf) return;
        Statement(node, scope);
    }

    void MacroRef(int node, int scope) {
        std::string name(d.Text(node));
        if (name.empty()) return;
        if (name[0] != '@') name = "@" + name;
        pending_macros.push_back({node, scope, name});
    }

    void ReadKids(int node, int scope, uint32_t from = 0) {
        for (uint32_t k = from; k < d.NKids(node); ++k) Read(d.Kid(node, k), scope);
    }

    // Arguments of a call: `k = v` keywords are names, not assignments.
    void CallArgs(int node, int scope, bool) {
        std::set<std::string> kws;
        for (uint32_t k = 0; k < d.NKids(node); ++k) {
            const int c = d.Kid(node, k);
            const Node &cn = d.N(c);
            if (k > 0 && cn.k == K::Assign && d.NKids(c) == 2) {
                KeywordArg(node, c, scope, kws);
            } else if (cn.k == K::Parameters) {
                for (uint32_t j = 0; j < d.NKids(c); ++j) {
                    const int p = d.Kid(c, j);
                    if (d.N(p).k == K::Assign && d.NKids(p) == 2) KeywordArg(node, p, scope, kws);
                    else if (IsIdent(d, p)) {
                        // f(; x) passes x as `x = x`.
                        Read(p, scope);
                        NoteKeyword(node, p, IdentName(d, p), kws);
                    } else {
                        Read(p, scope);
                    }
                }
            } else {
                Read(c, scope);
            }
        }
    }
    void KeywordArg(int call, int assign, int scope, std::set<std::string> &kws) {
        const int key = d.Kid(assign, 0);
        if (IsIdent(d, key)) NoteKeyword(call, key, IdentName(d, key), kws);
        else Read(key, scope);
        Read(d.Kid(assign, 1), scope);
    }
    void NoteKeyword(int call, int key, const std::string &name, std::set<std::string> &kws) {
        if (!kws.insert(name).second && d.N(call).k == K::Call && d.NKids(call) > 0) {
            const int callee = d.Kid(call, 0);
            std::string fname = IsIdent(d, callee) ? IdentName(d, callee) : std::string(d.Text(callee));
            Lowering(key, "keyword argument \"" + name + "\" repeated in call to \"" + fname + "\"",
                     "duplicate-keyword-argument");
        }
    }

    // `:(...)`/`quote ... end`: only `$(...)` inside is evaluated here.
    void Quoted(int node, int scope) {
        if (d.N(node).k == K::Interp) {
            ReadKids(node, scope);
            return;
        }
        // A nested quote raises the level; keep it simple: interpolations at any depth read.
        for (uint32_t k = 0; k < d.NKids(node); ++k) Quoted(d.Kid(node, k), scope);
    }

    // ---- macro calls ---------------------------------------------------------

    static bool MacroIs(std::string_view name, std::initializer_list<const char *> list) {
        if (!name.empty() && name[0] == '@') name.remove_prefix(1);
        // `Base.@kwdef` -> kwdef
        const size_t dot = name.rfind('.');
        if (dot != std::string_view::npos) name.remove_prefix(dot + 1);
        if (!name.empty() && name[0] == '@') name.remove_prefix(1);
        for (const char *m : list)
            if (name == m) return true;
        return false;
    }

    void MacroCallNode(int node, int scope, bool statement) {
        if (d.NKids(node) == 0) return;
        const int head = d.Kid(node, 0);
        std::string mname(d.Text(head));
        Read(head, scope);  // the macro itself (MacroName or `A.@m`)
        macro_depth++;
        if (MacroIs(mname, {"eval", "evalpoly_unused", "generated_unused"})) {
            if (MacroIs(mname, {"eval"})) {
                S(S(scope).module).opaque = true;
                MarkEval(scope);
            }
        }
        if (MacroIs(mname, {"locals"})) MarkEval(scope);
        if (MacroIs(mname, {"enum"})) {
            // @enum T a b c  /  @enum T begin a; b = 2 end: every name is a global.
            for (uint32_t k = 1; k < d.NKids(node); ++k) DefineEnum(d.Kid(node, k), scope);
            macro_depth--;
            return;
        }
        if (MacroIs(mname, {"goto", "label"})) {
            macro_depth--;
            return;
        }
        if (d.N(head).k == K::StrMacroName || std::string_view(jlparse::KindName(d.N(head).kind)) == "core_@cmd") {
            // `cmd $x`, styled"{link=$link}", L"$x": the macro interpolates itself;
            // a `$name` in its text counts as a use (never a reference).
            for (uint32_t k = 1; k < d.NKids(node); ++k) DollarNames(d.Text(d.Kid(node, k)), scope);
        }
        for (uint32_t k = 1; k < d.NKids(node); ++k) {
            const int c = d.Kid(node, k);
            // A bare name argument at statement level may be defined by the macro
            // (`@variables x y`, `@syms a`); remembered, never reported.
            if (statement && IsIdent(d, c)) maybe_defined.insert(IdentName(d, c));
            if (statement && d.N(c).k == K::Call && d.NKids(c) > 0 && IsIdent(d, d.Kid(c, 0)))
                maybe_defined.insert(IdentName(d, d.Kid(c, 0)));
            Statement(c, scope);
        }
        macro_depth--;
    }

    void DollarNames(std::string_view t, int scope) {
        auto ident_start = [](unsigned char c) { return c == '_' || std::isalpha(c) || c >= 0x80; };
        auto ident_char = [&](unsigned char c) { return ident_start(c) || std::isdigit(c) || c == '!'; };
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] != '$' || i + 1 >= t.size()) continue;
            size_t j = i + 1;
            if (t[j] == '(') {
                // $(expr): every identifier inside counts.
                int depth = 0;
                size_t k = j;
                for (; k < t.size(); ++k) {
                    if (t[k] == '(') depth++;
                    else if (t[k] == ')' && --depth == 0) break;
                }
                for (size_t a = j + 1; a < k && a < t.size();) {
                    if (ident_start(static_cast<unsigned char>(t[a])) &&
                        !(a > 0 && (ident_char(static_cast<unsigned char>(t[a - 1])) || t[a - 1] == '.'))) {
                        size_t b = a;
                        while (b < k && ident_char(static_cast<unsigned char>(t[b]))) ++b;
                        soft_reads.push_back({scope, std::string(t.substr(a, b - a))});
                        a = b;
                    } else {
                        ++a;
                    }
                }
                i = k;
            } else if (ident_start(static_cast<unsigned char>(t[j]))) {
                size_t b = j;
                while (b < t.size() && ident_char(static_cast<unsigned char>(t[b]))) ++b;
                soft_reads.push_back({scope, std::string(t.substr(j, b - j))});
                i = b - 1;
            }
        }
    }

    void DefineEnum(int node, int scope) {
        const Node &n = d.N(node);
        if (IsIdent(d, node)) {
            const std::string name = IdentName(d, node);
            const int v = Assign(S(scope).module, node, name, VarKind::Const, node);
            Occur(node, name, v, true, true, scope);
        } else if (n.k == K::DColon && d.NKids(node) == 2) {
            DefineEnum(d.Kid(node, 0), scope);
            Read(d.Kid(node, 1), scope);
        } else if (n.k == K::Assign && d.NKids(node) == 2) {
            DefineEnum(d.Kid(node, 0), scope);
            Read(d.Kid(node, 1), scope);
        } else if (n.k == K::Block) {
            for (uint32_t k = 0; k < d.NKids(node); ++k) DefineEnum(d.Kid(node, k), scope);
        }
    }

    void MarkEval(int scope) {
        for (int s = scope; s >= 0; s = S(s).parent) {
            S(s).has_eval = true;
            if (S(s).kind == ScopeKind::Module) break;
        }
    }

    // ---- statements ---------------------------------------------------------

    void Block(int node, int scope) {
        for (uint32_t k = 0; k < d.NKids(node); ++k) Statement(d.Kid(node, k), scope);
    }

    void Statement(int node, int scope) {
        if (node < 0) return;
        const Node &n = d.N(node);
        switch (n.k) {
        case K::Toplevel:
        case K::Block:
            Block(node, scope);
            return;
        case K::Doc:
            // A docstring then its object.
            for (uint32_t k = 0; k < d.NKids(node); ++k) Statement(d.Kid(node, k), scope);
            return;
        case K::Function:
            FunctionDef(node, scope);
            return;
        case K::Macro:
            MacroDef(node, scope);
            return;
        case K::Arrow:
            ArrowDef(node, scope);
            return;
        case K::Do:
            DoDef(node, scope);
            return;
        case K::Struct:
            StructDef(node, scope);
            return;
        case K::Abstract:
        case K::Primitive:
            TypeDecl(node, scope, n.k == K::Abstract ? VarKind::AbstractType : VarKind::PrimitiveType);
            return;
        case K::Module:
            ModuleDef(node, scope);
            return;
        case K::Assign:
            AssignNode(node, scope, VarKind::Local);
            return;
        case K::OpAssign:
        case K::DotAssign:
            // x += 1 reads and writes x; x .= y mutates x (a read).
            if (d.NKids(node) >= 2) {
                const int lhs = d.Kid(node, 0);
                if (n.k == K::OpAssign && IsIdent(d, lhs)) {
                    const std::string name = IdentName(d, lhs);
                    pending_reads.push_back({lhs, scope, name});
                    const int v = Assign(scope, lhs, name, VarKind::Local, -1);
                    Occur(lhs, name, v, false, true, scope);
                } else {
                    Read(lhs, scope);
                }
                for (uint32_t k = 1; k < d.NKids(node); ++k) Read(d.Kid(node, k), scope);
            }
            return;
        case K::Local:
        case K::Global:
            Declaration(node, scope, n.k == K::Global);
            return;
        case K::Const:
            if (IsLocalScope(scope) && !InStructBody(scope) &&
                !(d.NKids(node) == 1 && d.N(d.Kid(node, 0)).k == K::Global) && !ConstOfDeclaredGlobal(node, scope))
                Lowering(node, "unsupported `const` declaration on local variable", "local-const");
            for (uint32_t k = 0; k < d.NKids(node); ++k) {
                const int c = d.Kid(node, k);
                if (d.N(c).k == K::Assign) AssignNode(c, scope, VarKind::Const);
                else if (d.N(c).k == K::Global || d.N(c).k == K::Local) Declaration(c, scope, d.N(c).k == K::Global);
                else Read(c, scope);
            }
            return;
        case K::Let:
            LetDef(node, scope);
            return;
        case K::For:
            ForDef(node, scope);
            return;
        case K::While:
            if (d.NKids(node) >= 1) Read(d.Kid(node, 0), scope);
            if (d.NKids(node) >= 2) {
                const int s = NewScope(ScopeKind::Soft, scope, d.Kid(node, 1));
                loop_depth++;
                Body(d.Kid(node, 1), s);
                loop_depth--;
            }
            return;
        case K::Try:
            TryDef(node, scope);
            return;
        case K::If:
        case K::Elseif:
            for (uint32_t k = 0; k < d.NKids(node); ++k) {
                const int c = d.Kid(node, k);
                if (k == 0) Read(c, scope);
                else Statement(c, scope);
            }
            return;
        case K::Else:
            Block(node, scope);
            return;
        case K::Break:
        case K::Continue:
            if (loop_depth == 0 && macro_depth == 0 && quote_depth == 0)
                Lowering(node, "break or continue outside loop", "break-outside-loop");
            return;
        case K::Return:
            ReadKids(node, scope);
            return;
        case K::Using:
        case K::Import:
            ImportDef(node, scope, n.k == K::Using);
            return;
        case K::Export:
        case K::Public:
            for (uint32_t k = 0; k < d.NKids(node); ++k) {
                const int c = d.Kid(node, k);
                if (IsIdent(d, c)) exports.push_back({c, S(scope).module, IdentName(d, c)});
            }
            return;
        case K::MacroCall:
            MacroCallNode(node, scope, true);
            return;
        case K::Comprehension:
        case K::Generator:
            GeneratorDef(node, scope);
            return;
        case K::Quote:
            Quoted(node, scope);
            return;
        case K::Error:
            ReadKids(node, scope);
            return;
        case K::Tuple:
            Read(node, scope);  // `(a = 1, b = 2)` is a named tuple
            return;
        case K::Call:
            // eval(...) / include(...) make a module's names unknowable.
            if (d.NKids(node) > 0 && IsIdent(d, d.Kid(node, 0))) {
                const std::string f = IdentName(d, d.Kid(node, 0));
                if (f == "include" || f == "eval" || f == "include_dependency") {
                    S(S(scope).module).opaque = true;
                    if (f == "eval") MarkEval(scope);
                }
            }
            CallArgs(node, scope, true);
            return;
        default:
            if (n.k == K::Ident || n.k == K::Var) {
                Read(node, scope);
                return;
            }
            if (n.leaf) return;  // literals, operators, keywords
            // Expressions: every child is read (generators inside open their own scope).
            for (uint32_t k = 0; k < d.NKids(node); ++k) {
                const int c = d.Kid(node, k);
                const K ck = d.N(c).k;
                if (ck == K::Comprehension || ck == K::Generator || ck == K::Arrow || ck == K::Function ||
                    ck == K::Do || ck == K::Let || ck == K::MacroCall || ck == K::Quote || ck == K::Assign ||
                    ck == K::Block || ck == K::If || ck == K::For || ck == K::While || ck == K::Try ||
                    ck == K::OpAssign || ck == K::Local || ck == K::Global || ck == K::Const || ck == K::Return ||
                    ck == K::Break || ck == K::Continue)
                    Statement(c, scope);
                else
                    Read(c, scope);
            }
            return;
        }
    }

    // `global x; const x = 1` in a local scope is a global constant.
    bool ConstOfDeclaredGlobal(int node, int scope) {
        if (d.NKids(node) != 1 || d.N(d.Kid(node, 0)).k != K::Assign || d.NKids(d.Kid(node, 0)) != 2) return false;
        int lhs = d.Kid(d.Kid(node, 0), 0);
        if (d.N(lhs).k == K::DColon && d.NKids(lhs) == 2) lhs = d.Kid(lhs, 0);
        if (!IsIdent(d, lhs)) return false;
        auto it = declared_global.find(scope);
        return it != declared_global.end() && it->second.count(IdentName(d, lhs));
    }

    bool InStructBody(int scope) { return S(scope).kind == ScopeKind::Struct; }

    // A scope's body: its assignments are collected first (they make names
    // local for the whole scope), then the statements are walked.
    void Body(int node, int scope) {
        Prebind(node, scope);
        Statement(node, scope);
    }

    // The local-scope rule, decided before any read: every name assigned
    // directly in this scope (not in a nested scope) becomes local here
    // unless an enclosing local scope has it, or it is declared global.
    void Prebind(int node, int scope) {
        if (!IsLocalScope(scope)) return;
        std::vector<std::pair<int, std::string>> assigned;
        std::set<std::string> globals;
        CollectAssigned(node, assigned, globals);
        for (auto &[n, name] : assigned) {
            if (globals.count(name)) continue;
            if (S(scope).names.count(name)) continue;
            if (EnclosingLocal(scope, name) >= 0) continue;
            int parent = S(scope).parent;
            const bool toplevel_soft = S(scope).kind == ScopeKind::Soft && parent >= 0 && !IsLocalScope(parent);
            if (toplevel_soft && macro_depth == 0 && quote_depth == 0 && !UnderMacro(n, node)) {
                // Assigning a global's name in a top-level loop: Julia makes a
                // new local and warns that it is ambiguous (outside the REPL).
                toplevel_soft_assigns.push_back({n, scope, name});
            }
            NewVar(name, VarKind::Local, scope, n, -1);
        }
    }

    // Is `node` inside a macro call argument below `top`?
    bool UnderMacro(int node, int top) {
        for (int p = d.N(node).parent; p >= 0 && p != top; p = d.N(p).parent)
            if (d.N(p).k == K::MacroCall) return true;
        return false;
    }

    void CollectAssigned(int node, std::vector<std::pair<int, std::string>> &out, std::set<std::string> &globals) {
        if (node < 0) return;
        const Node &n = d.N(node);
        switch (n.k) {
        // Nested scopes keep their own assignments.
        case K::Function:
        case K::Macro:
        case K::Arrow:
        case K::Do:
        case K::Struct:
        case K::Module:
        case K::Let:
        case K::For:
        case K::While:
        case K::Try:
        case K::Comprehension:
        case K::Generator:
        case K::Quote:
            if (n.k == K::Function) {
                // A named local function binds its name here.
                int sig = d.NKids(node) > 0 ? d.Kid(node, 0) : -1;
                const int name = FunctionNameNode(sig);
                if (IsIdent(d, name)) out.push_back({name, IdentName(d, name)});
            }
            return;
        case K::Global:
            for (uint32_t k = 0; k < d.NKids(node); ++k) {
                int c = d.Kid(node, k);
                if (d.N(c).k == K::Assign && d.NKids(c) > 0) c = d.Kid(c, 0);
                if (d.N(c).k == K::DColon && d.NKids(c) > 0) c = d.Kid(c, 0);
                if (IsIdent(d, c)) globals.insert(IdentName(d, c));
            }
            return;
        case K::Assign:
            if (d.NKids(node) == 2) {
                const int lhs = d.Kid(node, 0);
                if (d.N(lhs).k == K::Call || (d.N(lhs).k == K::Where) ||
                    (d.N(lhs).k == K::DColon && d.NKids(lhs) == 2 && d.N(d.Kid(lhs, 0)).k == K::Call)) {
                    const int name = FunctionNameNode(lhs);
                    if (IsIdent(d, name)) out.push_back({name, IdentName(d, name)});
                } else {
                    LhsNames(lhs, out);
                }
                CollectAssigned(d.Kid(node, 1), out, globals);
            }
            return;
        case K::OpAssign:
            if (d.NKids(node) >= 1 && IsIdent(d, d.Kid(node, 0)))
                out.push_back({d.Kid(node, 0), IdentName(d, d.Kid(node, 0))});
            for (uint32_t k = 1; k < d.NKids(node); ++k) CollectAssigned(d.Kid(node, k), out, globals);
            return;
        case K::Call:
        case K::DotCall:
            // f(k = v): keywords, not assignments.
            for (uint32_t k = 0; k < d.NKids(node); ++k) {
                const int c = d.Kid(node, k);
                if (d.N(c).k == K::Assign && d.NKids(c) == 2) CollectAssigned(d.Kid(c, 1), out, globals);
                else if (d.N(c).k != K::Parameters) CollectAssigned(c, out, globals);
            }
            return;
        case K::Tuple:
            for (uint32_t k = 0; k < d.NKids(node); ++k) {
                const int c = d.Kid(node, k);
                if (d.N(c).k == K::Assign && d.NKids(c) == 2) CollectAssigned(d.Kid(c, 1), out, globals);
                else if (d.N(c).k != K::Parameters) CollectAssigned(c, out, globals);
            }
            return;
        case K::Local:
            // Declared local here: handled by Declaration.
            return;
        default:
            for (uint32_t k = 0; k < d.NKids(node); ++k) CollectAssigned(d.Kid(node, k), out, globals);
            return;
        }
    }

    // The names an assignment's left side binds: x, x::T, (a, b), (; a, b), a... .
    void LhsNames(int lhs, std::vector<std::pair<int, std::string>> &out) {
        const Node &n = d.N(lhs);
        if (IsIdent(d, lhs)) {
            out.push_back({lhs, IdentName(d, lhs)});
        } else if (n.k == K::DColon && d.NKids(lhs) == 2) {
            LhsNames(d.Kid(lhs, 0), out);
        } else if (n.k == K::Tuple || n.k == K::Parameters || n.k == K::Parens) {
            for (uint32_t k = 0; k < d.NKids(lhs); ++k) LhsNames(d.Kid(lhs, k), out);
        } else if (n.k == K::Splat && d.NKids(lhs) == 1) {
            LhsNames(d.Kid(lhs, 0), out);
        } else if (n.k == K::Curly && d.NKids(lhs) >= 1) {
            LhsNames(d.Kid(lhs, 0), out);  // `const Alias{T} = Union{...}`
        }
    }

    // ---- assignments ----------------------------------------------------------

    void AssignNode(int node, int scope, VarKind kind) {
        if (d.NKids(node) != 2) {
            ReadKids(node, scope);
            return;
        }
        const int lhs = d.Kid(node, 0);
        const int rhs = d.Kid(node, 1);
        const K lk = d.N(lhs).k;
        // f(x) = body, f(x)::T = body, f(x) where T = body: a method.
        if (lk == K::Call || lk == K::Where ||
            (lk == K::DColon && d.NKids(lhs) == 2 &&
             (d.N(d.Kid(lhs, 0)).k == K::Call || d.N(d.Kid(lhs, 0)).k == K::Where))) {
            Method(node, lhs, rhs, scope, false);
            return;
        }
        Read(rhs, scope);
        Bind(lhs, scope, kind, node, false);
    }

    // Binds the names on an assignment's left side; reads what it indexes.
    void Bind(int lhs, int scope, VarKind kind, int def_node, bool destructured) {
        const Node &n = d.N(lhs);
        if (IsIdent(d, lhs)) {
            const std::string name = IdentName(d, lhs);
            const int v = Assign(scope, lhs, name, kind, def_node);
            if (destructured) V(v).destructured = true;
            if (macro_depth > 0 && V(v).writes == 0 && V(v).reads == 0) V(v).from_macro = true;
            Occur(lhs, name, v, false, true, scope);
        } else if (n.k == K::DColon && d.NKids(lhs) == 2) {
            Bind(d.Kid(lhs, 0), scope, kind, def_node, destructured);
            Read(d.Kid(lhs, 1), scope);
            if (IsIdent(d, d.Kid(lhs, 0))) {
                const int v = Lookup(d, scope, IdentName(d, d.Kid(lhs, 0)));
                if (v >= 0) V(v).type = std::string(d.Text(d.Kid(lhs, 1)));
            }
        } else if (n.k == K::Tuple || n.k == K::Parens) {
            for (uint32_t k = 0; k < d.NKids(lhs); ++k) Bind(d.Kid(lhs, k), scope, kind, def_node, true);
        } else if (n.k == K::Parameters) {
            for (uint32_t k = 0; k < d.NKids(lhs); ++k) Bind(d.Kid(lhs, k), scope, kind, def_node, true);
        } else if (n.k == K::Splat && d.NKids(lhs) == 1) {
            Bind(d.Kid(lhs, 0), scope, kind, def_node, destructured);
        } else if (n.k == K::Curly && d.NKids(lhs) >= 1 && IsIdent(d, d.Kid(lhs, 0))) {
            // `const Alias{T} = Union{A{T}, B{T}}`: T is the alias's own parameter.
            Bind(d.Kid(lhs, 0), scope, kind, def_node, destructured);
        } else if (n.k == K::Const || n.k == K::Global || n.k == K::Local) {
            Declaration(lhs, scope, n.k == K::Global);
        } else {
            // x[i] = v, a.b = v, x' = ...: reads.
            Read(lhs, scope);
        }
    }

    void Declaration(int node, int scope, bool global) {
        for (uint32_t k = 0; k < d.NKids(node); ++k) {
            int c = d.Kid(node, k);
            int rhs = -1;
            if (d.N(c).k == K::Assign && d.NKids(c) == 2) {
                rhs = d.Kid(c, 1);
                if (d.N(d.Kid(c, 0)).k == K::Call || d.N(d.Kid(c, 0)).k == K::Where) {
                    // global f(x) = ... / local f(x) = ...
                    Method(c, d.Kid(c, 0), rhs, scope, false, global ? S(scope).module : -1);
                    continue;
                }
                c = d.Kid(c, 0);
            }
            if (rhs >= 0) Read(rhs, scope);
            std::vector<std::pair<int, std::string>> names;
            LhsNames(c, names);
            if (d.N(c).k == K::DColon && d.NKids(c) == 2) Read(d.Kid(c, 1), scope);
            for (auto &[nn, name] : names) {
                if (global) {
                    if (IsLocalScope(scope)) {
                        auto it = S(scope).names.find(name);
                        if (it != S(scope).names.end() && V(it->second).kind == VarKind::Argument)
                            Lowering(nn, "misplaced \"global\" declaration", "global-argument");
                        else if (it != S(scope).names.end() && declared_local.count(it->second))
                            Lowering(nn, "variable \"" + name + "\" declared both local and global",
                                     "local-global-conflict");
                    }
                    const int mod = S(scope).module;
                    int v;
                    auto git = S(mod).names.find(name);
                    if (git != S(mod).names.end()) v = git->second;
                    else v = NewVar(name, VarKind::Global, mod, nn, rhs >= 0 ? node : -1);
                    if (IsLocalScope(scope)) {
                        S(scope).names[name] = v;
                        declared_global[scope].insert(name);
                    }
                    Occur(nn, name, v, rhs < 0, rhs >= 0, scope);
                } else {
                    if (IsLocalScope(scope) && declared_global[scope].count(name))
                        Lowering(nn, "variable \"" + name + "\" declared both local and global",
                                 "local-global-conflict");
                    auto it = S(scope).names.find(name);
                    int v;
                    if (it != S(scope).names.end() && V(it->second).kind == VarKind::Argument &&
                        S(scope).kind == ScopeKind::Function) {
                        Lowering(nn, "local variable name \"" + name + "\" conflicts with an argument",
                                 "local-argument-conflict");
                        v = it->second;
                    } else if (it != S(scope).names.end() && V(it->second).scope == scope) {
                        v = it->second;
                    } else {
                        v = NewVar(name, IsLocalScope(scope) ? VarKind::Local : VarKind::Global, scope, nn, -1);
                    }
                    declared_local.insert(v);
                    Occur(nn, name, v, rhs < 0, rhs >= 0, scope);
                }
            }
        }
    }

    // ---- definitions ----------------------------------------------------------

    // The node naming a method, given its signature (`f`, `A.f`, `f{T}`):
    // the Ident for a plain name, else -1.
    int FunctionNameNode(int sig) {
        while (sig >= 0) {
            const Node &n = d.N(sig);
            if (n.k == K::Where || n.k == K::DColon) {
                if (d.NKids(sig) == 0) return -1;
                if (n.k == K::DColon && d.NKids(sig) == 1) return -1;
                sig = d.Kid(sig, 0);
                continue;
            }
            if (n.k == K::Call) {
                if (d.NKids(sig) == 0) return -1;
                const int callee = d.Kid(sig, 0);
                if (IsIdent(d, callee)) return callee;
                return -1;
            }
            if (IsIdent(d, sig)) return sig;
            return -1;
        }
        return -1;
    }

    // `function sig body end`, `function f end`, `function (x) ... end`.
    void FunctionDef(int node, int scope) {
        if (d.NKids(node) == 0) return;
        const int sig = d.Kid(node, 0);
        const int body = d.NKids(node) >= 2 ? d.Kid(node, 1) : -1;
        if (IsIdent(d, sig)) {
            // function f end: a generic function with no methods.
            const std::string name = IdentName(d, sig);
            const int v = Assign(scope, sig, name, VarKind::Function, node);
            Occur(sig, name, v, true, true, scope);
            return;
        }
        Method(node, sig, body, scope, true);
    }

    // A method: binds its name in @p scope, its arguments and static
    // parameters in a new function scope, and walks the body there.
    // @p name_scope: where the method's name binds (`global f(x) = ...` in a
    // let binds it in the module, while the body still sees the let's names).
    void Method(int def, int sig, int body, int scope, bool long_form, int name_scope = -1) {
        (void)long_form;
        const int fs = NewScope(ScopeKind::Function, scope, def);
        // Peel `where` and `::T` from the signature.
        std::vector<int> wheres;
        int rettype = -1;
        int s = sig;
        while (s >= 0) {
            const Node &n = d.N(s);
            if (n.k == K::Where && d.NKids(s) >= 1) {
                for (uint32_t k = 1; k < d.NKids(s); ++k) wheres.push_back(d.Kid(s, k));
                s = d.Kid(s, 0);
            } else if (n.k == K::DColon && d.NKids(s) == 2 &&
                       (d.N(d.Kid(s, 0)).k == K::Call || d.N(d.Kid(s, 0)).k == K::Where ||
                        d.N(d.Kid(s, 0)).k == K::Tuple)) {
                rettype = d.Kid(s, 1);
                s = d.Kid(s, 0);
            } else {
                break;
            }
        }
        // Static parameters (outermost `where` binds first).
        std::vector<int> tparams;
        for (auto it = wheres.rbegin(); it != wheres.rend(); ++it) TypeParams(*it, fs, scope, tparams);
        std::vector<int> args;
        if (s >= 0 && d.N(s).k == K::Call && d.NKids(s) >= 1) {
            const int callee = d.Kid(s, 0);
            if (IsIdent(d, callee)) {
                const std::string name = IdentName(d, callee);
                int target = name_scope >= 0 ? name_scope : scope;
                const int v = Assign(target, callee, name, VarKind::Function, def);
                if (V(v).kind == VarKind::Local || V(v).kind == VarKind::Global) {
                    // first seen as a method
                    if (V(v).defs.size() <= 1) V(v).kind = VarKind::Function;
                }
                Occur(callee, name, v, true, true, scope);
            } else if (d.N(callee).k == K::DColon) {
                // (f::Foo)(x) = ...: a callable object; `f` is an argument.
                if (d.NKids(callee) == 2) {
                    Argument(d.Kid(callee, 0), fs, args, VarKind::Argument);
                    Read(d.Kid(callee, 1), fs);
                } else if (d.NKids(callee) == 1) {
                    Read(d.Kid(callee, 0), fs);
                }
            } else if (d.N(callee).k == K::Curly) {
                // S{T}(x) where T: a constructor of S.
                ReadKids(callee, fs);
            } else {
                Read(callee, scope);  // Base.show, operators
            }
            for (uint32_t k = 1; k < d.NKids(s); ++k) Argument(d.Kid(s, k), fs, args, VarKind::Argument);
        } else if (s >= 0 && (d.N(s).k == K::Tuple || d.N(s).k == K::Parens)) {
            for (uint32_t k = 0; k < d.NKids(s); ++k) Argument(d.Kid(s, k), fs, args, VarKind::Argument);
        } else if (s >= 0 && d.N(s).k == K::DotCall) {
            ReadKids(s, scope);
        } else if (s >= 0) {
            Argument(s, fs, args, VarKind::Argument);
        }
        if (rettype >= 0) Read(rettype, fs);
        // Julia warns about a static parameter no argument type mentions.
        if (macro_depth == 0) CheckTypeParams(tparams, s, wheres);
        DuplicateArgs(args);
        if (body >= 0) {
            const int saved_loop = loop_depth;
            loop_depth = 0;
            Body(body, fs);
            loop_depth = saved_loop;
        }
        functions.push_back({def, fs, args});
    }

    void TypeParams(int node, int fs, int outer, std::vector<int> &out) {
        const Node &n = d.N(node);
        if (n.k == K::Braces) {
            for (uint32_t k = 0; k < d.NKids(node); ++k) TypeParams(d.Kid(node, k), fs, outer, out);
            return;
        }
        if ((n.k == K::Subtype || n.k == K::Supertype) && d.NKids(node) == 2 && IsIdent(d, d.Kid(node, 0))) {
            Read(d.Kid(node, 1), fs);
            TypeParams(d.Kid(node, 0), fs, outer, out);
            return;
        }
        // `lb <: T <: ub` parses as a comparison
        if (n.k == K::Other && d.NKids(node) == 5 && IsIdent(d, d.Kid(node, 2))) {
            Read(d.Kid(node, 0), fs);
            Read(d.Kid(node, 4), fs);
            TypeParams(d.Kid(node, 2), fs, outer, out);
            return;
        }
        if (IsIdent(d, node)) {
            const std::string name = IdentName(d, node);
            const int v = NewVar(name, VarKind::TypeParam, fs, node, -1);
            Occur(node, name, v, true, true, fs);
            out.push_back(v);
            return;
        }
        Read(node, fs);
    }

    // Julia warns about a static parameter that neither the signature (the
    // argument types, or the callee `(s::S{T})(x)` / `S{T}(x)`) nor another
    // parameter's bound (`where {T, A<:AbstractArray{T}}`) mentions.
    void CheckTypeParams(const std::vector<int> &tparams, int sig, const std::vector<int> &wheres) {
        if (tparams.empty() || sig < 0) return;
        std::set<std::string> mentioned;
        CollectIdents(sig, mentioned);
        for (int w : wheres) BoundIdents(w, mentioned);
        for (int v : tparams)
            if (!mentioned.count(V(v).name)) unused_tparams.push_back(v);
    }

    void BoundIdents(int node, std::set<std::string> &out) {
        const Node &n = d.N(node);
        if (n.k == K::Braces) {
            for (uint32_t k = 0; k < d.NKids(node); ++k) BoundIdents(d.Kid(node, k), out);
        } else if ((n.k == K::Subtype || n.k == K::Supertype) && d.NKids(node) == 2) {
            CollectIdents(d.Kid(node, 1), out);
        } else if (n.k == K::Other && d.NKids(node) == 5) {
            CollectIdents(d.Kid(node, 0), out);
            CollectIdents(d.Kid(node, 4), out);
        }
    }

    void CollectIdents(int node, std::set<std::string> &out) {
        if (IsIdent(d, node)) {
            out.insert(IdentName(d, node));
            return;
        }
        for (uint32_t k = 0; k < d.NKids(node); ++k) CollectIdents(d.Kid(node, k), out);
    }

    void DuplicateArgs(const std::vector<int> &args) {
        std::set<std::string> seen;
        for (int v : args) {
            const std::string &name = V(v).name;
            if (name == "_" || name.empty()) continue;
            if (!seen.insert(name).second) {
                Lowering(-1, "function argument name not unique: \"" + name + "\"", "duplicate-argument", V(v).b,
                         V(v).e);
            }
        }
    }

    // One parameter: x, x::T, ::T, x = default, x..., (a, b), @nospecialize(x), (; k).
    void Argument(int node, int fs, std::vector<int> &args, VarKind kind) {
        const Node &n = d.N(node);
        if (IsIdent(d, node)) {
            const std::string name = IdentName(d, node);
            int v;
            auto it = S(fs).names.find(name);
            if (it != S(fs).names.end() && V(it->second).scope == fs && V(it->second).kind != VarKind::TypeParam) {
                // A repeated name: a second variable, so the duplicate is reported.
                Var copy = V(it->second);
                v = static_cast<int>(d.vars.size());
                copy.b = n.b;
                copy.e = n.e;
                d.vars.push_back(copy);
            } else {
                v = NewVar(name, kind, fs, node, -1);
            }
            Occur(node, name, v, true, true, fs);
            args.push_back(v);
            return;
        }
        switch (n.k) {
        case K::DColon:
            if (d.NKids(node) == 2) {
                Argument(d.Kid(node, 0), fs, args, kind);
                Read(d.Kid(node, 1), fs);
                if (IsIdent(d, d.Kid(node, 0)) && !args.empty())
                    V(args.back()).type = std::string(d.Text(d.Kid(node, 1)));
            } else {
                ReadKids(node, fs);
            }
            return;
        case K::Assign:
            if (d.NKids(node) == 2) {
                Read(d.Kid(node, 1), fs);
                Argument(d.Kid(node, 0), fs, args, kind);
            }
            return;
        case K::Splat:
            if (d.NKids(node) == 1) {
                const size_t before = args.size();
                Argument(d.Kid(node, 0), fs, args, kind);
                if (args.size() > before) V(args.back()).splat = true;
            }
            return;
        case K::Parameters:
            for (uint32_t k = 0; k < d.NKids(node); ++k) Argument(d.Kid(node, k), fs, args, VarKind::KwArgument);
            return;
        case K::Tuple:
        case K::Parens:
            for (uint32_t k = 0; k < d.NKids(node); ++k) Argument(d.Kid(node, k), fs, args, kind);
            return;
        case K::MacroCall:
            // @nospecialize(x), @specialize x
            if (d.NKids(node) >= 1) Read(d.Kid(node, 0), fs);
            for (uint32_t k = 1; k < d.NKids(node); ++k) Argument(d.Kid(node, k), fs, args, kind);
            return;
        case K::Interp:
            ReadKids(node, fs);
            return;
        default:
            Read(node, fs);
            return;
        }
    }

    void MacroDef(int node, int scope) {
        if (d.NKids(node) == 0) return;
        const int sig = d.Kid(node, 0);
        const int body = d.NKids(node) >= 2 ? d.Kid(node, 1) : -1;
        const int ms = NewScope(ScopeKind::Macro, scope, node);
        std::vector<int> args;
        if (d.N(sig).k == K::Call && d.NKids(sig) >= 1) {
            const int callee = d.Kid(sig, 0);
            if (IsIdent(d, callee)) {
                const std::string name = "@" + IdentName(d, callee);
                const int v = Assign(S(scope).module, callee, name, VarKind::Macro, node);
                V(v).kind = VarKind::Macro;
                Occur(callee, name, v, true, true, scope);
            }
            for (uint32_t k = 1; k < d.NKids(sig); ++k) Argument(d.Kid(sig, k), ms, args, VarKind::Argument);
        }
        DuplicateArgs(args);
        if (body >= 0) Body(body, ms);
        functions.push_back({node, ms, args});
    }

    void ArrowDef(int node, int scope) {
        if (d.NKids(node) != 2) {
            ReadKids(node, scope);
            return;
        }
        const int fs = NewScope(ScopeKind::Function, scope, node);
        std::vector<int> args;
        int sig = d.Kid(node, 0);
        std::vector<int> tparams;
        while (d.N(sig).k == K::Where && d.NKids(sig) >= 1) {
            for (uint32_t k = 1; k < d.NKids(sig); ++k) TypeParams(d.Kid(sig, k), fs, scope, tparams);
            sig = d.Kid(sig, 0);
        }
        if (d.N(sig).k == K::DColon && d.NKids(sig) == 2 && d.N(d.Kid(sig, 0)).k == K::Tuple) {
            Read(d.Kid(sig, 1), fs);
            sig = d.Kid(sig, 0);
        }
        Argument(sig, fs, args, VarKind::Argument);
        DuplicateArgs(args);
        const int saved_loop = loop_depth;
        loop_depth = 0;
        Body(d.Kid(node, 1), fs);
        loop_depth = saved_loop;
        lambdas.push_back({node, fs, args});
    }

    // f(x) do a, b ... end: the call is read here; the block is a function of a, b.
    void DoDef(int node, int scope) {
        if (d.NKids(node) == 0) return;
        Read(d.Kid(node, 0), scope);
        const int fs = NewScope(ScopeKind::Function, scope, node);
        std::vector<int> args;
        if (d.NKids(node) >= 2) Argument(d.Kid(node, 1), fs, args, VarKind::Argument);
        DuplicateArgs(args);
        const int saved_loop = loop_depth;
        loop_depth = 0;
        if (d.NKids(node) >= 3) Body(d.Kid(node, 2), fs);
        loop_depth = saved_loop;
        lambdas.push_back({node, fs, args});
    }

    void StructDef(int node, int scope) {
        if (d.NKids(node) == 0) return;
        int head = d.Kid(node, 0);
        const int ss = NewScope(ScopeKind::Struct, scope, node);
        std::vector<int> tparams;
        if ((d.N(head).k == K::Subtype) && d.NKids(head) == 2) {
            Read(d.Kid(head, 1), ss);
            head = d.Kid(head, 0);
        }
        if (d.N(head).k == K::Curly && d.NKids(head) >= 1) {
            for (uint32_t k = 1; k < d.NKids(head); ++k) TypeParams(d.Kid(head, k), ss, scope, tparams);
            head = d.Kid(head, 0);
        }
        // The supertype was read before the parameters existed; re-read is harmless.
        if (IsIdent(d, head)) {
            const std::string name = IdentName(d, head);
            const int v = Assign(S(scope).module == scope ? scope : scope, head, name, VarKind::Struct, node);
            V(v).kind = VarKind::Struct;
            Occur(head, name, v, true, true, scope);
        }
        if (d.NKids(node) < 2) return;
        const int body = d.Kid(node, 1);
        // Fields first: `x`, `x::T`, `x::T = default` (@kwdef); methods are constructors.
        std::set<std::string> fields;
        for (uint32_t k = 0; k < d.NKids(body); ++k) {
            int c = d.Kid(body, k);
            int field = -1;
            if (IsIdent(d, c)) field = c;
            else if (d.N(c).k == K::DColon && d.NKids(c) == 2 && IsIdent(d, d.Kid(c, 0))) {
                field = d.Kid(c, 0);
                Read(d.Kid(c, 1), ss);
            } else if (d.N(c).k == K::Assign && d.NKids(c) == 2 && d.N(d.Kid(c, 0)).k != K::Call &&
                       d.N(d.Kid(c, 0)).k != K::Where &&
                       !(d.N(d.Kid(c, 0)).k == K::DColon && d.NKids(d.Kid(c, 0)) == 2 &&
                         d.N(d.Kid(d.Kid(c, 0), 0)).k == K::Call)) {
                // @kwdef default
                int f = d.Kid(c, 0);
                if (d.N(f).k == K::DColon && d.NKids(f) == 2) {
                    Read(d.Kid(f, 1), ss);
                    f = d.Kid(f, 0);
                }
                if (IsIdent(d, f)) field = f;
                Read(d.Kid(c, 1), ss);
            } else if (d.N(c).k == K::Const && d.NKids(c) == 1) {
                // Julia 1.8 `const` fields in a mutable struct.
                int f = d.Kid(c, 0);
                if (d.N(f).k == K::DColon && d.NKids(f) == 2) {
                    Read(d.Kid(f, 1), ss);
                    f = d.Kid(f, 0);
                }
                if (IsIdent(d, f)) field = f;
            } else {
                Statement(c, ss);
                continue;
            }
            if (field >= 0) {
                const std::string fname = IdentName(d, field);
                if (!fields.insert(fname).second)
                    Lowering(field, "duplicate field name: \"" + fname + "\" is not unique", "duplicate-field");
                struct_fields.push_back({field, ss});
            }
        }
    }

    void TypeDecl(int node, int scope, VarKind kind) {
        if (d.NKids(node) == 0) return;
        int head = d.Kid(node, 0);
        const int ts = NewScope(ScopeKind::Struct, scope, node);
        std::vector<int> tparams;
        if (d.N(head).k == K::Subtype && d.NKids(head) == 2) {
            Read(d.Kid(head, 1), ts);
            head = d.Kid(head, 0);
        }
        if (d.N(head).k == K::Curly && d.NKids(head) >= 1) {
            for (uint32_t k = 1; k < d.NKids(head); ++k) TypeParams(d.Kid(head, k), ts, scope, tparams);
            head = d.Kid(head, 0);
        }
        if (IsIdent(d, head)) {
            const std::string name = IdentName(d, head);
            const int v = Assign(scope, head, name, kind, node);
            V(v).kind = kind;
            Occur(head, name, v, true, true, scope);
        }
        for (uint32_t k = 1; k < d.NKids(node); ++k) Read(d.Kid(node, k), scope);  // primitive's bits
    }

    void ModuleDef(int node, int scope) {
        // (module name block); a baremodule has BARE_MODULE_FLAG.
        uint32_t k = 0;
        int name = -1, body = -1;
        for (; k < d.NKids(node); ++k) {
            const int c = d.Kid(node, k);
            if (IsIdent(d, c) && name < 0) name = c;
            else if (d.N(c).k == K::Block) body = c;
        }
        if (name >= 0) {
            const std::string n = IdentName(d, name);
            const int v = Assign(S(scope).module, name, n, VarKind::ModuleName, node);
            V(v).kind = VarKind::ModuleName;
            Occur(name, n, v, true, true, scope);
        }
        const int ms = NewScope(ScopeKind::Module, scope, node);
        S(ms).bare = (d.N(node).flags & jlparse::BARE_MODULE_FLAG) != 0;
        if (name >= 0) {
            // A module sees its own name.
            const std::string n = IdentName(d, name);
            NewVar(n, VarKind::ModuleName, ms, name, node);
        }
        if (body >= 0) {
            const int saved_loop = loop_depth;
            loop_depth = 0;
            Statement(body, ms);
            loop_depth = saved_loop;
        }
    }

    void LetDef(int node, int scope) {
        if (d.NKids(node) == 0) return;
        const int binds = d.Kid(node, 0);
        const int ls = NewScope(ScopeKind::Let, scope, node);
        // Each binding: its right side sees the bindings before it, not itself.
        std::vector<int> list;
        if (d.N(binds).k == K::Block) {
            for (uint32_t k = 0; k < d.NKids(binds); ++k) list.push_back(d.Kid(binds, k));
        } else {
            list.push_back(binds);
        }
        for (int b : list) {
            if (d.N(b).k == K::Assign && d.NKids(b) == 2) {
                const int lhs = d.Kid(b, 0);
                if (d.N(lhs).k == K::Call) {
                    Method(b, lhs, d.Kid(b, 1), ls, false);
                    continue;
                }
                // Read in the let scope before the name exists there.
                ReadBefore(d.Kid(b, 1), ls);
                std::vector<std::pair<int, std::string>> names;
                LhsNames(lhs, names);
                const bool parts = !IsIdent(d, lhs) && d.N(lhs).k != K::DColon;
                for (auto &[nn, name] : names) {
                    const int v = NewVar(name, VarKind::LetVar, ls, nn, b);
                    if (parts) V(v).destructured = true;
                    if (macro_depth > 0) V(v).from_macro = true;  // `@m let x = 1 ... end`
                    Occur(nn, name, v, false, true, ls);
                }
                if (d.N(lhs).k == K::DColon && d.NKids(lhs) == 2) Read(d.Kid(lhs, 1), ls);
            } else if (IsIdent(d, b)) {
                const std::string name = IdentName(d, b);
                // `let x` makes a fresh x, `let x = x` copies the outer one.
                const int v = NewVar(name, VarKind::LetVar, ls, b, b);
                V(v).destructured = true;  // a declaration, never reported unused
                Occur(b, name, v, true, false, ls);
            } else {
                Read(b, ls);
            }
        }
        if (d.NKids(node) >= 2) Body(d.Kid(node, 1), ls);
    }

    // Reads resolved in @p scope as it is now (a let binding's right side).
    void ReadBefore(int node, int scope) {
        const size_t mark = pending_reads.size();
        Read(node, scope);
        // Resolve immediately so later bindings of the same name don't capture it.
        for (size_t i = mark; i < pending_reads.size(); ++i) ResolveRead(pending_reads[i]);
        pending_reads.resize(mark);
    }

    void ForDef(int node, int scope) {
        if (d.NKids(node) == 0) return;
        const int spec = d.Kid(node, 0);
        const int fs = NewScope(ScopeKind::Soft, scope, node);
        std::vector<int> specs;
        if (d.N(spec).k == K::CartIter || d.N(spec).k == K::Block) {
            for (uint32_t k = 0; k < d.NKids(spec); ++k) specs.push_back(d.Kid(spec, k));
        } else {
            specs.push_back(spec);
        }
        for (int s : specs) IterationSpec(s, scope, fs, VarKind::LoopVar);
        if (d.NKids(node) >= 2) {
            loop_depth++;
            Body(d.Kid(node, 1), fs);
            loop_depth--;
        }
    }

    // i = r, i in r, i ∈ r, (a, b) in r, outer i in r.
    void IterationSpec(int s, int outer, int inner, VarKind kind) {
        const Node &n = d.N(s);
        int lhs = -1, rhs = -1;
        if ((n.k == K::Assign || n.k == K::Other) && d.NKids(s) == 2) {
            lhs = d.Kid(s, 0);
            rhs = d.Kid(s, 1);
        } else if (n.k == K::Other && d.NKids(s) == 3) {
            // (call-i i in r)
            lhs = d.Kid(s, 0);
            rhs = d.Kid(s, 2);
        }
        if (lhs < 0) {
            Read(s, inner);
            return;
        }
        Read(rhs, outer);
        if (d.N(lhs).k == K::Outer && d.NKids(lhs) == 1) {
            Read(d.Kid(lhs, 0), inner);
            return;
        }
        std::vector<std::pair<int, std::string>> names;
        LhsNames(lhs, names);
        for (auto &[nn, name] : names) {
            const int v = NewVar(name, kind, inner, nn, s);
            if (names.size() > 1) V(v).destructured = true;
            Occur(nn, name, v, false, true, inner);
        }
        if (d.N(lhs).k == K::DColon && d.NKids(lhs) == 2) Read(d.Kid(lhs, 1), inner);
    }

    void TryDef(int node, int scope) {
        for (uint32_t k = 0; k < d.NKids(node); ++k) {
            const int c = d.Kid(node, k);
            const Node &cn = d.N(c);
            if (cn.k == K::Block) {
                const int ts = NewScope(ScopeKind::Soft, scope, c);
                Body(c, ts);
            } else if (cn.k == K::Catch) {
                const int cs = NewScope(ScopeKind::Soft, scope, c);
                uint32_t j = 0;
                if (d.NKids(c) >= 1 && IsIdent(d, d.Kid(c, 0))) {
                    const int e = d.Kid(c, 0);
                    const std::string name = IdentName(d, e);
                    const int v = NewVar(name, VarKind::CatchVar, cs, e, c);
                    Occur(e, name, v, true, true, cs);
                    j = 1;
                } else if (d.NKids(c) >= 1 && d.N(d.Kid(c, 0)).k == K::False) {
                    j = 1;
                }
                for (; j < d.NKids(c); ++j) Body(d.Kid(c, j), cs);
            } else if (cn.k == K::Finally || cn.k == K::Else) {
                const int fs = NewScope(ScopeKind::Soft, scope, c);
                for (uint32_t j = 0; j < d.NKids(c); ++j) Body(d.Kid(c, j), fs);
            } else {
                Read(c, scope);
            }
        }
    }

    // [body for x in xs if cond], (body for x in xs), T[...]
    void GeneratorDef(int node, int scope) {
        const Node &n = d.N(node);
        if (n.k == K::Comprehension) {
            for (uint32_t k = 0; k < d.NKids(node); ++k) {
                const int c = d.Kid(node, k);
                if (d.N(c).k == K::Generator) GeneratorDef(c, scope);
                else Read(c, scope);
            }
            return;
        }
        if (d.NKids(node) == 0) return;
        const int gs = NewScope(ScopeKind::Soft, scope, node);
        // (generator body spec... ) / (generator body (filter spec... cond))
        for (uint32_t k = 1; k < d.NKids(node); ++k) {
            const int c = d.Kid(node, k);
            if (d.N(c).k == K::Filter && d.NKids(c) >= 1) {
                for (uint32_t j = 0; j + 1 < d.NKids(c); ++j) IterationSpec(d.Kid(c, j), gs, gs, VarKind::LoopVar);
                Read(d.Kid(c, d.NKids(c) - 1), gs);
            } else if (d.N(c).k == K::CartIter) {
                for (uint32_t j = 0; j < d.NKids(c); ++j) IterationSpec(d.Kid(c, j), gs, gs, VarKind::LoopVar);
            } else if (d.N(c).k == K::Generator) {
                GeneratorDef(c, gs);
            } else {
                IterationSpec(c, gs, gs, VarKind::LoopVar);
            }
        }
        Read(d.Kid(node, 0), gs);
    }

    // using A, B.C / using A: b, c as d / import A.b as c / import ..A
    void ImportDef(int node, int scope, bool is_using) {
        const int mod = S(scope).module;
        for (uint32_t k = 0; k < d.NKids(node); ++k) {
            const int c = d.Kid(node, k);
            if (d.N(c).k == K::Colon) {
                // (: (importpath A) names...)
                for (uint32_t j = 1; j < d.NKids(c); ++j) ImportName(d.Kid(c, j), scope, mod);
                continue;
            }
            if (d.N(c).k == K::ImportPath || d.N(c).k == K::As) {
                const std::string last = ImportName(c, scope, mod);
                if (is_using && d.N(c).k == K::ImportPath) {
                    // `using X`: X's exports come in. Known for the standard library.
                    std::string path = PathText(c);
                    if (path == "Base" || path == "Core") continue;
                    size_t count = 0;
                    const JuliaLspApi *api = JuliaLspApiTable(&count);
                    bool known = false;
                    for (size_t i = 0; i < count && !known; ++i) known = path == api[i].module;
                    if (known) S(mod).usings.push_back(path);
                    else S(mod).unknown_usings.push_back(path);
                    (void)last;
                }
            }
        }
    }

    std::string PathText(int path) {
        std::string s;
        for (uint32_t k = 0; k < d.NKids(path); ++k) {
            const int c = d.Kid(path, k);
            if (!s.empty() && s.back() != '.') s += '.';
            s += std::string(d.Text(c));
        }
        return s;
    }

    // Binds the name an import brings in (the path's last part, or `as`'s).
    std::string ImportName(int node, int scope, int mod) {
        int name = -1;
        if (d.N(node).k == K::As && d.NKids(node) == 2) {
            name = d.Kid(node, 1);
        } else if (d.N(node).k == K::ImportPath && d.NKids(node) >= 1) {
            name = d.Kid(node, d.NKids(node) - 1);
        } else if (IsIdent(d, node) || d.N(node).k == K::MacroName) {
            name = node;
        }
        if (name < 0) return "";
        std::string n = d.N(name).k == K::MacroName ? std::string(d.Text(name)) : IdentName(d, name);
        if (d.N(name).k == K::MacroName && (n.empty() || n[0] != '@')) n = "@" + n;
        if (n.empty() || n == ".") return "";
        int v;
        auto it = S(mod).names.find(n);
        if (it != S(mod).names.end()) v = it->second;
        else v = NewVar(n, VarKind::Import, mod, name, node);
        Occur(name, n, v, true, true, scope);
        return n;
    }

    // ---- resolution --------------------------------------------------------------

    struct PendingRead {
        int node, scope;
        std::string name;
    };
    std::vector<PendingRead> pending_reads, pending_macros;
    std::vector<std::pair<int, std::string>> soft_reads;  // scope, name

    void ResolveRead(const PendingRead &r) {
        const int v = Lookup(d, r.scope, r.name);
        Occur(r.node, r.name, v, false, false, r.scope);
    }

    // ---- diagnostics ----------------------------------------------------------------

    void Lowering(int node, const std::string &msg, const std::string &code, uint32_t b = 0, uint32_t e = 0) {
        // Inside a macro call or a quote the code may never be lowered as written.
        if (macro_depth > 0 || quote_depth > 0) return;
        if (node >= 0) {
            b = d.N(node).b;
            e = d.N(node).e;
        }
        JuliaLspDiagnostic g;
        g.range = d.Range(b, e);
        g.severity = JuliaLspSeverity::Error;
        g.code = code;
        g.message = msg;
        lowering.push_back(std::move(g));
    }

    struct Fn {
        int def, scope;
        std::vector<int> args;
    };
    std::vector<Fn> functions, lambdas;
    std::vector<int> unused_tparams;  // vars
    struct Export {
        int node, module;
        std::string name;
    };
    std::vector<Export> exports;
    std::vector<PendingRead> toplevel_soft_assigns;
    std::vector<std::pair<int, int>> struct_fields;
    std::vector<JuliaLspDiagnostic> lowering;
    std::set<std::string> maybe_defined;
    std::map<int, std::set<std::string>> declared_global;
    std::set<int> declared_local;
    int loop_depth = 0, macro_depth = 0, quote_depth = 0;

    void Run() {
        const int top = NewScope(ScopeKind::Module, -1, -1);
        (void)top;
        if (d.root >= 0) Statement(d.root, 0);
        for (const PendingRead &r : pending_reads) ResolveRead(r);
        for (const auto &[scope, name] : soft_reads) {
            const int v = Lookup(d, scope, name);
            if (v >= 0) V(v).reads++;
        }
        for (const PendingRead &r : pending_macros) {
            const int v = Lookup(d, r.scope, r.name);
            Occ o;
            o.b = d.N(r.node).b;
            o.e = d.N(r.node).e;
            o.var = v;
            o.scope = r.scope;
            o.name = r.name;
            d.occs.push_back(o);
            if (v >= 0) V(v).reads++;
        }
        std::sort(d.occs.begin(), d.occs.end(), [](const Occ &a, const Occ &b) { return a.b < b.b; });
    }
};

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

int EditDistance(std::string_view a, std::string_view b, int cap) {
    const size_t n = a.size(), m = b.size();
    if (static_cast<int>(n > m ? n - m : m - n) > cap) return cap + 1;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i);
        int best = cur[0];
        for (size_t j = 1; j <= m; ++j) {
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
            best = std::min(best, cur[j]);
        }
        if (best > cap) return cap + 1;
        std::swap(prev, cur);
    }
    return prev[m];
}

const std::set<std::string, std::less<>> &Builtins() {
    // Names every module sees that aren't exported bindings: syntax-level
    // and lowering-level names.
    static const std::set<std::string, std::less<>> s = {
        "nothing", "missing", "true", "false", "ccall", "cglobal", "llvmcall", "Base", "Core", "Main", "new",
        "__source__", "__module__", "end", "begin", "isdefined", "Vararg", "Union", "Any", "Tuple", "NamedTuple",
        "var", "include", "eval", "ans", "err", "splatnew", "__init__", "Expr", "Symbol", "@__dot__", "@__MODULE__",
        "@__FILE__", "@__LINE__", "@__DIR__", "@doc", "@cmd", "@int128_str", "@uint128_str", "@big_str",
        "@generated", "@goto", "@label",
    };
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared helpers (julia_lsp_internal.h)

std::string IdentName(const Doc &d, int node) {
    if (node < 0) return "";
    const Node &n = d.N(node);
    if (n.k == K::Var) {
        // (var x): the identifier inside var"..."
        if (d.NKids(node) >= 1) return std::string(d.Text(d.Kid(node, 0)));
        return "";
    }
    return std::string(d.Text(n));
}

int ScopeAt(const Doc &d, uint32_t at) {
    int best = 0;
    for (size_t i = 0; i < d.scopes.size(); ++i) {
        const Scope &s = d.scopes[i];
        if (s.b <= at && at <= s.e) {
            const Scope &b = d.scopes[static_cast<size_t>(best)];
            if (s.e - s.b <= b.e - b.b) best = static_cast<int>(i);
        }
    }
    return best;
}

int NodeAt(const Doc &d, uint32_t at) {
    int best = -1;
    for (size_t i = 0; i < d.nodes.size(); ++i) {
        const Node &n = d.nodes[i];
        if (n.b <= at && at < n.e) best = static_cast<int>(i);  // preorder: deeper comes later
    }
    return best;
}

int Lookup(const Doc &d, int scope, std::string_view name) {
    for (int s = scope; s >= 0; s = d.scopes[static_cast<size_t>(s)].parent) {
        const Scope &sc = d.scopes[static_cast<size_t>(s)];
        auto it = sc.names.find(name);
        if (it != sc.names.end()) return it->second;
        // A module doesn't see its parent module's names.
        if (sc.kind == ScopeKind::Module) break;
    }
    return -1;
}

std::vector<const JuliaLspApi *> VocabAll(std::string_view name) {
    size_t count = 0;
    const JuliaLspApi *api = JuliaLspApiTable(&count);
    std::vector<const JuliaLspApi *> out;
    auto lo = std::lower_bound(api, api + count, name,
                               [](const JuliaLspApi &a, std::string_view n) { return std::string_view(a.name) < n; });
    for (; lo != api + count && name == lo->name; ++lo) out.push_back(lo);
    return out;
}

std::vector<const JuliaLspApi *> VocabFor(const Doc &d, int module, std::string_view name) {
    std::vector<const JuliaLspApi *> out;
    const Scope &m = d.scopes[static_cast<size_t>(module)];
    for (const JuliaLspApi *a : VocabAll(name)) {
        const std::string_view mod = a->module;
        if (mod == "Core" || (mod == "Base" && !m.bare) ||
            std::find(m.usings.begin(), m.usings.end(), mod) != m.usings.end())
            out.push_back(a);
    }
    return out;
}

std::string DocstringFor(const Doc &d, int def) {
    for (int p = def; p >= 0; p = d.N(p).parent) {
        const Node &n = d.N(p);
        if (n.k == K::Doc && d.NKids(p) >= 1) {
            const int s = d.Kid(p, 0);
            if (d.N(s).k != K::String) return "";
            std::string out;
            for (uint32_t k = 0; k < d.NKids(s); ++k) {
                const int c = d.Kid(s, k);
                if (d.N(c).k == K::StrLit) out += std::string(d.Text(c));
                else out += "$(" + std::string(d.Text(c)) + ")";
            }
            // Triple-quoted strings: drop the common indentation.
            std::vector<std::string> ls;
            size_t at = 0;
            while (at <= out.size()) {
                size_t nl = out.find('\n', at);
                if (nl == std::string::npos) nl = out.size();
                ls.push_back(out.substr(at, nl - at));
                at = nl + 1;
            }
            size_t indent = std::string::npos;
            for (size_t i = 1; i < ls.size(); ++i) {
                const std::string &l = ls[i];
                const size_t ns = l.find_first_not_of(" \t");
                if (ns == std::string::npos) continue;
                indent = std::min(indent, ns);
            }
            std::string res;
            for (size_t i = 0; i < ls.size(); ++i) {
                std::string l = ls[i];
                if (i > 0 && indent != std::string::npos && l.size() >= indent) l = l.substr(indent);
                if (i) res += '\n';
                res += l;
            }
            while (!res.empty() && (res.back() == '\n' || res.back() == ' ')) res.pop_back();
            while (!res.empty() && res.front() == '\n') res.erase(0, 1);
            return res;
        }
        // Only through wrappers that keep the definition the documented object.
        if (p != def && n.k != K::MacroCall && n.k != K::Doc && n.k != K::Const && n.k != K::Global) {
            if (n.k != K::Function && n.k != K::Assign && n.k != K::Struct && n.k != K::Macro) return "";
        }
        if (p != def && n.k == K::Block) return "";
    }
    return "";
}

std::string SignatureText(const Doc &d, int def) {
    if (def < 0) return "";
    const Node &n = d.N(def);
    int sig = -1;
    if ((n.k == K::Function || n.k == K::Macro || n.k == K::Assign) && d.NKids(def) >= 1) sig = d.Kid(def, 0);
    else if (n.k == K::Struct || n.k == K::Abstract || n.k == K::Primitive) sig = d.NKids(def) ? d.Kid(def, 0) : -1;
    if (sig < 0) return "";
    std::string s(d.Text(sig));
    // One line, runs of blanks squeezed.
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == '\n' || c == '\t' || c == ' ' || c == '\r') {
            space = true;
            continue;
        }
        if (space && !out.empty() && out.back() != '(' && c != ')' && c != ',') out += ' ';
        space = false;
        out += c;
    }
    if (n.k == K::Macro) out = "@" + out;
    if (n.k == K::Struct) out = ((n.flags & jlparse::MUTABLE_FLAG) ? "mutable struct " : "struct ") + out;
    if (n.k == K::Abstract) out = "abstract type " + out;
    if (n.k == K::Primitive) out = "primitive type " + out;
    return out;
}

}  // namespace jllsp

using namespace jllsp;

std::shared_ptr<const JuliaLspDocument> JuliaLspAnalyze(const std::vector<std::string> &lines) {
    auto doc = std::make_shared<JuliaLspDocument>();
    Doc &d = doc->d;
    d.lines = lines;
    for (size_t i = 0; i < lines.size(); ++i) {
        d.line_starts.push_back(static_cast<uint32_t>(d.text.size()));
        d.text += lines[i];
        if (i + 1 < lines.size()) d.text += '\n';
    }
    if (d.line_starts.empty()) d.line_starts.push_back(0);
    d.parse = jlparse::Parse(d.text);
    BuildSyntax(d);

    // Syntax errors and warnings: JuliaSyntax's own.
    for (const jlparse::Diagnostic &g : d.parse.diagnostics) {
        JuliaLspDiagnostic x;
        // JuliaSyntax's 1-based inclusive byte range -> 0-based half-open.
        const uint32_t b = static_cast<uint32_t>(std::max<int64_t>(g.first_byte - 1, 0));
        const uint32_t e = static_cast<uint32_t>(std::max<int64_t>(g.last_byte, 0));
        x.range = d.Range(b, std::max(e, b));
        x.severity = g.level == jlparse::Level::Error ? JuliaLspSeverity::Error : JuliaLspSeverity::Warning;
        x.code = g.level == jlparse::Level::Error ? "syntax-error" : "syntax-warning";
        x.message = g.message;
        if (x.severity == JuliaLspSeverity::Error) d.syntax_errors = true;
        d.diags.push_back(std::move(x));
    }

    Analyzer a(d);
    a.Run();
    if (d.syntax_errors) return doc;  // the tree is partial: no lints over it

    for (JuliaLspDiagnostic &g : a.lowering) d.diags.push_back(std::move(g));

    auto add = [&](uint32_t b, uint32_t e, JuliaLspSeverity sev, const char *code, std::string msg,
                   bool unnecessary = false) {
        JuliaLspDiagnostic g;
        g.range = d.Range(b, e);
        g.severity = sev;
        g.code = code;
        g.message = std::move(msg);
        g.unnecessary = unnecessary;
        d.diags.push_back(std::move(g));
    };

    // Unused locals and arguments.
    for (const Var &v : d.vars) {
        if (v.underscore || v.reads > 0 || v.from_macro) continue;
        const Scope &s = d.scopes[static_cast<size_t>(v.scope)];
        if (s.has_eval || s.kind == ScopeKind::Module) continue;
        if (v.kind == VarKind::Local || v.kind == VarKind::LetVar) {
            if (v.destructured) continue;
            add(v.b, v.e, JuliaLspSeverity::Warning, "unused-local",
                "local variable `" + v.name + "` is assigned but never used", true);
        }
    }
    auto unused_args = [&](const std::vector<Analyzer::Fn> &fns, bool lambda) {
        for (const Analyzer::Fn &f : fns) {
            const Scope &s = d.scopes[static_cast<size_t>(f.scope)];
            if (s.has_eval) continue;
            for (int vi : f.args) {
                const Var &v = d.vars[static_cast<size_t>(vi)];
                if (v.underscore || v.reads > 0 || v.name.empty()) continue;
                // `::T` arguments are there for dispatch, `kw...` is a catch-all.
                if (lambda || !v.type.empty() || v.splat) continue;
                add(v.b, v.e, JuliaLspSeverity::Hint, "unused-argument", "argument `" + v.name + "` is never used",
                    true);
            }
        }
    };
    unused_args(a.functions, false);
    unused_args(a.lambdas, true);

    for (int vi : a.unused_tparams) {
        const Var &v = d.vars[static_cast<size_t>(vi)];
        add(v.b, v.e, JuliaLspSeverity::Warning, "unused-type-parameter",
            "method definition declares type variable " + v.name + " but does not use it");
    }

    // Undefined names: resolved nowhere in the file, not Base/Core or a
    // `using`ed standard library, not plausibly made by a macro -- and only
    // where the module's names are knowable.
    std::set<std::string> reported;
    for (const Occ &o : d.occs) {
        if (o.var >= 0 || o.def || o.write) continue;
        const std::string &name = o.name;
        if (Builtins().count(name) || a.maybe_defined.count(name)) continue;
        const int mod = d.scopes[static_cast<size_t>(o.scope)].module;
        const Scope &m = d.scopes[static_cast<size_t>(mod)];
        if (!VocabFor(d, mod, name).empty()) continue;
        if (name[0] == '@') continue;  // macros come from anywhere
        // Near misses only: a name a letter or two from one in scope.
        std::string best;
        int best_d = 3;
        auto consider = [&](const std::string &cand) {
            if (cand == name || cand.size() < 3) return;
            const int dist = EditDistance(name, cand, 2);
            if (dist < best_d || (dist == best_d && cand < best)) {
                best_d = dist;
                best = cand;
            }
        };
        for (int s = o.scope; s >= 0; s = d.scopes[static_cast<size_t>(s)].parent) {
            const bool local = d.scopes[static_cast<size_t>(s)].kind != ScopeKind::Module;
            for (auto &[n, v] : d.scopes[static_cast<size_t>(s)].names) consider(n);
            if (!local) break;
        }
        if (best.empty() || name.size() < 3 || (best_d > 1 && name.size() < 6)) continue;
        if (EqualsNoCase(name, best)) continue;  // `Model` vs `model`: a type and its instance
        // Knowable: an explicit `module` with no include(), eval or package
        // `using`. A file's top level may be include()d from anywhere.
        const bool knowable = m.parent >= 0 && !m.opaque && m.unknown_usings.empty();
        if (!knowable) continue;
        if (!reported.insert(name + "@" + std::to_string(o.b)).second) continue;
        add(o.b, o.e, JuliaLspSeverity::Warning, "undefined-name",
            "`" + name + "` is not defined; did you mean `" + best + "`?");
    }

    // Soft-scope ambiguity: a top-level loop assigning a global's name.
    for (const auto &r : a.toplevel_soft_assigns) {
        const int mod = d.scopes[static_cast<size_t>(r.scope)].module;
        const auto &names = d.scopes[static_cast<size_t>(mod)].names;
        auto it = names.find(r.name);
        if (it == names.end()) continue;
        const Var &g = d.vars[static_cast<size_t>(it->second)];
        if (g.kind != VarKind::Global && g.kind != VarKind::Const) continue;
        add(d.N(r.node).b, d.N(r.node).e, JuliaLspSeverity::Warning, "soft-scope",
            "assignment to `" + r.name +
                "` in soft scope is ambiguous because a global variable by the same name exists: `" + r.name +
                "` will be treated as a new local. Disambiguate by using `local " + r.name +
                "` to suppress this warning or `global " + r.name + "` to assign to the existing global variable.");
    }

    // `x == nothing`: === (or isnothing) is what's meant.
    for (size_t i = 0; i < d.nodes.size(); ++i) {
        const Node &n = d.nodes[i];
        if (n.k != K::Call || n.nkid != 3 || !(n.flags & jlparse::INFIX_FLAG)) continue;
        const int op = d.Kid(static_cast<int>(i), 1);
        const std::string_view ops = d.Text(op);
        if (ops != "==" && ops != "!=") continue;
        const int l = d.Kid(static_cast<int>(i), 0), r = d.Kid(static_cast<int>(i), 2);
        if (d.Text(l) == "nothing" || d.Text(r) == "nothing") {
            add(n.b, n.e, JuliaLspSeverity::Information, "compare-nothing",
                std::string("comparison with `nothing`: use `") + (ops == "==" ? "===" : "!==") +
                    "` or `isnothing` (`==` may call a user-defined method)");
        }
    }

    // Unreachable code after return/throw/break/continue in the same block.
    for (size_t i = 0; i < d.nodes.size(); ++i) {
        const Node &n = d.nodes[i];
        if (n.k != K::Block) continue;
        for (uint32_t k = 0; k + 1 < n.nkid; ++k) {
            const int c = d.Kid(static_cast<int>(i), k);
            const K ck = d.N(c).k;
            bool stops = ck == K::Return || ck == K::Break || ck == K::Continue;
            if (ck == K::Call && d.NKids(c) >= 1 && d.Text(d.Kid(c, 0)) == "throw") stops = true;
            if (!stops) continue;
            const int next = d.Kid(static_cast<int>(i), k + 1);
            const int last = d.Kid(static_cast<int>(i), n.nkid - 1);
            // `@goto` can jump to a later `@label`; quoted code is data.
            bool label = false;
            for (uint32_t j = k + 1; j < n.nkid; ++j) {
                const int s = d.Kid(static_cast<int>(i), j);
                if (d.N(s).k == K::MacroCall && d.NKids(s) > 0 && d.Text(d.Kid(s, 0)) == "@label") label = true;
            }
            bool quoted = false;
            for (int p = n.parent; p >= 0; p = d.N(p).parent)
                if (d.N(p).k == K::Quote) quoted = true;
            if (label || quoted) break;
            add(d.N(next).b, d.N(last).e, JuliaLspSeverity::Hint, "unreachable", "unreachable code", true);
            break;
        }
    }

    std::stable_sort(d.diags.begin(), d.diags.end(), [](const JuliaLspDiagnostic &x, const JuliaLspDiagnostic &y) {
        return std::tie(x.range.line, x.range.col) < std::tie(y.range.line, y.range.col);
    });
    return doc;
}

std::vector<JuliaLspDiagnostic> JuliaLspDiagnostics(const JuliaLspDocument &doc) { return doc.d.diags; }

std::string JuliaLspJuliaCheck(const std::string &text) {
    const jlparse::ParseResult r = jlparse::Parse(text);
    for (const jlparse::Diagnostic &g : r.diagnostics) {
        if (g.level != jlparse::Level::Error) continue;
        const jlparse::LineCol lc = jlparse::SourceIndex(text).Location(g.first_byte);
        return std::to_string(lc.line) + ":" + std::to_string(lc.column) + ": " + g.message;
    }
    return "";
}

std::string JuliaLspFormatDiagnostic(const std::string &path, const JuliaLspDiagnostic &d) {
    const char *sev = d.severity == JuliaLspSeverity::Error         ? "error"
                      : d.severity == JuliaLspSeverity::Warning     ? "warning"
                      : d.severity == JuliaLspSeverity::Information ? "info"
                                                                     : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool JuliaLspDiagnosticFails(const JuliaLspDiagnostic &d) {
    return d.severity == JuliaLspSeverity::Error || d.severity == JuliaLspSeverity::Warning;
}
