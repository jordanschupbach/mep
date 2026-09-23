// The analysis half of mep's own Maxima language server: an index of what
// a document binds and uses, then the diagnostics and the editor features
// read off it. See maxima_lsp.h for the contract and the scope notes.

#include "maxima_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "maxima_lsp_builtin_names.h"

namespace {

// --- Small helpers ----------------------------------------------------

/** @brief Levenshtein distance, capped: anything past `limit` is reported as `limit + 1`. */
int EditDistance(const std::string &a, const std::string &b, int limit) {
    const size_t n = a.size();
    const size_t m = b.size();
    if (n > m + static_cast<size_t>(limit) || m > n + static_cast<size_t>(limit)) return limit + 1;
    std::vector<int> prev(m + 1);
    std::vector<int> cur(m + 1);
    for (size_t j = 0; j <= m; j++) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; i++) {
        cur[0] = static_cast<int>(i);
        int row_best = cur[0];
        for (size_t j = 1; j <= m; j++) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            row_best = std::min(row_best, cur[j]);
        }
        if (row_best > limit) return limit + 1;
        prev = cur;
    }
    return prev[m];
}

// --- The document index ------------------------------------------------

// What one name in one scope is.
enum class BindKind {
    Global,     // a top-level `x : ...`
    Function,   // `f(x) := ...`
    ArrayFun,   // `f[i] := ...`
    Macro,      // `f(x) ::= ...`
    Param,      // a formal of a function or lambda
    Local,      // a `block([...])` local, or a `local(...)` declaration
    LoopVar,    // the control variable of a loop or of makelist/sum/product
    Operator,   // a spelling introduced by infix("...") and friends
};

struct Binding {
    std::string name;
    BindKind kind = BindKind::Global;
    MxRange range;          // the defining occurrence of the name itself
    MxRange def_range;      // the whole definition, for folding and symbols
    int scope = 0;          // index into Index::scopes
    int reads = 0;          // how many times it was read after being bound
    int writes = 0;         // how many times it was assigned (the definition counts)
    int arity = -1;         // declared parameter count, -1 when it is not a function
    int min_arity = -1;     // the same, minus the parameters that carry a default
    bool variadic = false;  // a `[rest]` formal, so any arity is acceptable
    std::string signature;  // "f(x, y)" for a function, "" otherwise
    // A `block([x : v], ...)` local with an initializer. Maxima binds
    // globals dynamically, so one of these is very often a setting the
    // body's *callees* read rather than an unused name -- which is why
    // `unused-local` only looks at the ones declared bare.
    bool initialized = false;
    int def_node = -1;
};

struct Scope {
    int parent = -1;
    int node = -1;  // the construct that opened it, -1 for the file scope
    std::vector<int> bindings;
};

// One resolved occurrence of a name.
struct Occurrence {
    std::string name;
    MxRange range;
    int binding = -1;  // index into Index::bindings, -1 when nothing binds it
    bool is_write = false;
    bool is_call = false;
    bool quoted = false;  // inside something that does not evaluate its arguments
    int argc = 0;         // arguments written at the call site
};

struct Index {
    std::vector<Scope> scopes;
    std::vector<Binding> bindings;
    std::vector<Occurrence> occurrences;
    std::vector<MaximaLspDiagnostic> diags;
    // Set when the document does something that makes name knowledge
    // unreliable -- see maxima_lsp.h's scope note.
    bool opaque = false;
    std::vector<std::string> loaded_packages;
};

// --- The walker -------------------------------------------------------

// Functions that bind a variable over one of their arguments. The value
// is the index of the argument that names the variable -- `makelist(expr,
// i, 1, 10)` binds argument 1 (`i`) over argument 0 (`expr`).
struct BinderSpec {
    const char *name;
    int var_arg;
    int body_arg;
};
const BinderSpec kBinders[] = {
    {"makelist", 1, 0},    {"create_list", 1, 0}, {"sum", 1, 0},        {"product", 1, 0},
    {"lsum", 1, 0},        {"nusum", 1, 0},       {"unsum", 1, 0},      {"sumcontract", 1, 0},
    {"treillis", 1, 0},    {"genmatrix", 1, 0},
};

/** @brief Finds the binder spec for a call, or null. */
const BinderSpec *FindBinder(const std::string &name) {
    for (const BinderSpec &spec : kBinders) {
        if (name == spec.name) return &spec;
    }
    return nullptr;
}

// Calls that take an expression and do something other than evaluate it:
// translate it to another language, use it as a rewrite-rule template,
// print it. `f(x)` inside one of these is a piece of syntax, so the
// argument count Maxima would insist on at a real call does not apply --
// gentran's own demo file writes `gentran(while f(x) >= 0 do ...)` for an
// `f` defined with four parameters.
const char *const kUnevaluatedArgFunctions[] = {
    "gentran",   "buildq",   "quote",    "tellsimp", "tellsimpafter", "defrule",  "defmatch",
    "let",       "letsimp",  "texput",   "tex",      "grind",         "string",   "fundef",
    "dispfun",   "disprule", "funmake",  "trace",    "untrace",       "timer",    "untimer",
    "dependencies", "romberg", "quad_qags", "quad_qag", "plot2d",     "plot3d",   "draw2d",
    "draw3d",
};

/** @brief Reports whether a call leaves its arguments unevaluated. */
bool TakesUnevaluatedArgs(const std::string &name) {
    for (const char *fn : kUnevaluatedArgFunctions) {
        if (name == fn) return true;
    }
    return false;
}

// Calls that make the document's names unknowable from its own text: a
// package this server has no name list for, a file read at run time, a
// name built out of a string. `undefined`-style reporting switches off
// for a document that does any of them, which is r_lsp.h's own rule and
// for the same reason.
const char *const kOpaqueFunctions[] = {
    "eval_string", "parse_string", "concat",  "sconcat", "symbol",  "funmake", "apply",
    "read",        "readonly",     "readline", "with_stdout", "translate", "compile_file",
};

/** @brief Reports whether a call makes the rest of the document's names unknowable. */
bool IsOpaqueCall(const std::string &name) {
    for (const char *fn : kOpaqueFunctions) {
        if (name == fn) return true;
    }
    return false;
}

class Walker {
  public:
    Walker(const MxParseResult &parse, Index *index) : parse_(parse), index_(index) {
        Scope file;
        index_->scopes.push_back(file);
    }

    void Run() {
        for (const MxStatement &st : parse_.statements) {
            if (st.node < 0) continue;
            WalkStatement(st.node, 0);
        }
        ResolvePending();
    }

  private:
    const MxParseResult &parse_;
    Index *index_;
    // The scope each occurrence was seen in, kept in step with
    // index_->occurrences. A function may be called above its own
    // definition, so any occurrence that resolved to nothing on the way
    // through is looked up again once the whole file has been walked --
    // the same two-pass resolve cpp_lsp.cpp needs, for the same reason.
    std::vector<int> occurrence_scopes_;
    // How deep inside something that does not evaluate its arguments the
    // walk currently is.
    int quote_depth_ = 0;

    const MxNode &At(int node) const { return parse_.at(node); }

    // `(x)` is `x`: Maxima's reader collapses a one-expression
    // parenthesis away, so an assignment or a definition written
    // `(f)(x) := ...` binds `f` like any other.
    int Unwrap(int node) const {
        int current = node;
        while (At(current).kind == MxNodeKind::Paren && At(current).kids.size() == 1) current = At(current).kids[0];
        return current;
    }

    int NewScope(int parent, int node) {
        Scope s;
        s.parent = parent;
        s.node = node;
        index_->scopes.push_back(s);
        return static_cast<int>(index_->scopes.size()) - 1;
    }

    int Bind(const std::string &name, BindKind kind, const MxRange &range, int scope) {
        Binding b;
        b.name = name;
        b.kind = kind;
        b.range = range;
        b.def_range = range;
        b.scope = scope;
        index_->bindings.push_back(b);
        const int id = static_cast<int>(index_->bindings.size()) - 1;
        index_->scopes[static_cast<size_t>(scope)].bindings.push_back(id);
        return id;
    }

    /** @brief Finds a visible binding for `name` from `scope` outwards, or -1. */
    int Lookup(const std::string &name, int scope) const {
        for (int s = scope; s >= 0;) {
            const Scope &sc = index_->scopes[static_cast<size_t>(s)];
            for (auto it = sc.bindings.rbegin(); it != sc.bindings.rend(); ++it) {
                if (index_->bindings[static_cast<size_t>(*it)].name == name) return *it;
            }
            s = sc.parent;
        }
        return -1;
    }

    void Use(const std::string &name, const MxRange &range, int scope, bool is_write, bool is_call, int argc) {
        Occurrence occ;
        occ.name = name;
        occ.range = range;
        occ.is_write = is_write;
        occ.is_call = is_call;
        occ.quoted = quote_depth_ > 0;
        occ.argc = argc;
        occ.binding = Lookup(name, scope);
        if (occ.binding >= 0) {
            Binding &b = index_->bindings[static_cast<size_t>(occ.binding)];
            if (is_write) {
                b.writes++;
            } else {
                b.reads++;
            }
        }
        index_->occurrences.push_back(occ);
        occurrence_scopes_.push_back(scope);
    }

    void ResolvePending() {
        for (size_t i = 0; i < index_->occurrences.size(); i++) {
            Occurrence &occ = index_->occurrences[i];
            if (occ.binding >= 0) continue;
            const int scope = i < occurrence_scopes_.size() ? occurrence_scopes_[i] : 0;
            occ.binding = Lookup(occ.name, scope);
            if (occ.binding < 0) continue;
            Binding &b = index_->bindings[static_cast<size_t>(occ.binding)];
            if (occ.is_write) {
                b.writes++;
            } else {
                b.reads++;
            }
        }
    }

    // --- Statements ---------------------------------------------------

    void WalkStatement(int node, int scope) {
        const MxNode &n = At(node);
        if (n.kind == MxNodeKind::Label) {
            // `name && expr` -- the tag names the statement, it is not a
            // use of anything.
            if (n.kids.size() == 2) WalkStatement(n.kids[1], scope);
            return;
        }
        Walk(node, scope);
    }

    // --- Expressions --------------------------------------------------

    void Walk(int node, int scope) {
        if (node < 0) return;
        const MxNode &n = At(node);
        switch (n.kind) {
            case MxNodeKind::Ident:
                Use(n.text, n.range, scope, false, false, 0);
                return;
            case MxNodeKind::Num:
            case MxNodeKind::Str:
            case MxNodeKind::Lisp:
            case MxNodeKind::LispEsc:
            case MxNodeKind::Error:
                return;
            case MxNodeKind::Quote:
                // `'expr` is the expression as written, not as evaluated:
                // the names in it are symbols, and resolving them would
                // report every `'x` as a use of a variable that the
                // author deliberately did not want evaluated. `''expr`
                // is the opposite and is walked normally.
                if (n.text == "''" && !n.kids.empty()) Walk(n.kids[0], scope);
                return;
            case MxNodeKind::Binary: return WalkBinary(node, scope);
            case MxNodeKind::Call: return WalkCall(node, scope);
            case MxNodeKind::Index: return WalkIndex(node, scope, false);
            case MxNodeKind::Do: return WalkDo(node, scope);
            case MxNodeKind::If:
            case MxNodeKind::List:
            case MxNodeKind::Set:
            case MxNodeKind::Paren:
            case MxNodeKind::Unary:
            case MxNodeKind::Postfix:
            case MxNodeKind::Label:
            default: break;
        }
        for (int kid : n.kids) Walk(kid, scope);
        for (const MxArg &arg : n.args) Walk(arg.value, scope);
    }

    void WalkBinary(int node, int scope) {
        const MxNode &n = At(node);
        if (n.kids.size() != 2) return;
        const int lhs = n.kids[0];
        const int rhs = n.kids[1];
        if (n.text == ":=" || n.text == "::=") {
            WalkDefinition(node, Unwrap(lhs), rhs, scope, n.text == "::=");
            return;
        }
        if (n.text == ":") {
            WalkAssignment(node, Unwrap(lhs), rhs, scope);
            return;
        }
        // `a :: b` assigns to whatever `a` evaluates to, so `a` is read
        // rather than written -- the one assignment operator whose left
        // side is not a binding.
        Walk(lhs, scope);
        Walk(rhs, scope);
    }

    // `f(x) := body`, `f[i] := body`, `x := body`.
    void WalkDefinition(int node, int lhs, int rhs, int scope, bool macro) {
        const MxNode &target = At(lhs);
        if (target.kind == MxNodeKind::Call || target.kind == MxNodeKind::Index) {
            const int name_node = target.kids.empty() ? -1 : target.kids[0];
            const MxNode &name = At(name_node);
            const int body_scope = NewScope(scope, node);
            int arity = 0;
            int min_arity = 0;
            bool variadic = false;
            std::string signature;
            for (const MxArg &arg : target.args) {
                const MxNode &param = At(arg.value);
                std::string param_name;
                bool has_default = false;
                if (param.kind == MxNodeKind::Ident) {
                    param_name = param.text;
                } else if (param.kind == MxNodeKind::List && param.kids.size() == 1 &&
                           At(param.kids[0]).kind == MxNodeKind::Ident) {
                    // `f([L]) := ...` takes any number of arguments and
                    // collects them into the list `L`.
                    param_name = At(param.kids[0]).text;
                    variadic = true;
                } else if (param.kind == MxNodeKind::Binary && param.text == ":" && param.kids.size() == 2 &&
                           At(param.kids[0]).kind == MxNodeKind::Ident) {
                    // `f(x, y : 1) := ...` gives `y` a default, so a call
                    // may leave it out.
                    param_name = At(param.kids[0]).text;
                    has_default = true;
                    Walk(param.kids[1], scope);
                } else {
                    Walk(arg.value, scope);
                }
                if (!param_name.empty()) {
                    if (!signature.empty()) signature += ", ";
                    signature += param_name;
                    const int existing = LookupLocal(param_name, body_scope);
                    if (existing >= 0) {
                        Diag(param.range, MaximaLspSeverity::Warning, "duplicate-parameter",
                             "`" + param_name + "` is already a parameter of this definition.");
                    }
                    MarkShadowed(param_name, scope);
                    Bind(param_name, BindKind::Param, param.range, body_scope);
                    arity++;
                    if (!has_default) min_arity++;
                }
            }
            if (name.kind == MxNodeKind::Ident) {
                // `local(f)` then `f(x) := ...` is the declaration doing
                // its job, not a leftover.
                MarkShadowed(name.text, scope);
                const BindKind kind = macro ? BindKind::Macro
                                            : (target.kind == MxNodeKind::Index ? BindKind::ArrayFun
                                                                                : BindKind::Function);
                const int id = Bind(name.text, kind, name.range, scope);
                Binding &b = index_->bindings[static_cast<size_t>(id)];
                b.def_range = At(node).range;
                b.def_node = node;
                b.arity = arity;
                b.min_arity = min_arity;
                b.variadic = variadic;
                b.signature = name.text + (target.kind == MxNodeKind::Index ? "[" : "(") + signature +
                              (target.kind == MxNodeKind::Index ? "]" : ")");
                b.writes++;
                if (MaximaLspIsProtectedName(name.text)) {
                    Diag(name.range, MaximaLspSeverity::Warning, "redefines-builtin",
                         "`" + name.text + "` is one of Maxima's own names; redefining it changes what the rest of "
                         "the session means by it.");
                }
            } else {
                Walk(name_node, scope);
            }
            Walk(rhs, body_scope);
            return;
        }
        if (target.kind == MxNodeKind::Ident) {
            // `x := expr` defines a function of no arguments, which is
            // legal but almost always a `:` that was mistyped -- the two
            // differ in whether the right side is evaluated now or at
            // every call.
            const int id = Bind(target.text, BindKind::Function, target.range, scope);
            index_->bindings[static_cast<size_t>(id)].arity = 0;
            index_->bindings[static_cast<size_t>(id)].min_arity = 0;
            index_->bindings[static_cast<size_t>(id)].signature = target.text + "()";
            index_->bindings[static_cast<size_t>(id)].def_range = At(node).range;
            index_->bindings[static_cast<size_t>(id)].writes++;
            Walk(rhs, scope);
            return;
        }
        Diag(target.range, MaximaLspSeverity::Error, "bad-definition-lhs",
             "The left side of `:=` has to be a name, `f(args)` or `f[args]`; Maxima refuses any other shape.");
        Walk(lhs, scope);
        Walk(rhs, scope);
    }

    // `x : expr`, `a[i] : expr`.
    void WalkAssignment(int node, int lhs, int rhs, int scope) {
        // The right side is evaluated in the scope the assignment is
        // written in, before the name is bound.
        Walk(rhs, scope);
        const MxNode &target = At(lhs);
        if (target.kind == MxNodeKind::Ident) {
            if (MaximaLspIsProtectedName(target.text)) {
                Diag(target.range, MaximaLspSeverity::Error, "assign-to-protected",
                     "`" + target.text + "` is a Maxima constant; assigning to it fails at run time.");
            }
            const int existing = Lookup(target.text, scope);
            if (existing >= 0) {
                Use(target.text, target.range, scope, true, false, 0);
            } else {
                const int id = Bind(target.text, scope == 0 ? BindKind::Global : BindKind::Local, target.range, scope);
                index_->bindings[static_cast<size_t>(id)].writes++;
                index_->bindings[static_cast<size_t>(id)].def_range = At(node).range;
                index_->bindings[static_cast<size_t>(id)].def_node = node;
            }
            return;
        }
        if (target.kind == MxNodeKind::Index) {
            // `a[i] : v` writes into `a` and reads `i`.
            WalkIndex(lhs, scope, true);
            return;
        }
        if (target.kind == MxNodeKind::List) {
            // `[x1, x2] : assoc(...)` assigns to each name in turn --
            // Maxima's destructuring assignment, and the reason a
            // perfectly used variable would otherwise look unread.
            for (int kid : target.kids) {
                const MxNode &element = At(kid);
                if (element.kind != MxNodeKind::Ident) {
                    Walk(kid, scope);
                    continue;
                }
                if (Lookup(element.text, scope) >= 0) {
                    Use(element.text, element.range, scope, true, false, 0);
                    continue;
                }
                const int id =
                    Bind(element.text, scope == 0 ? BindKind::Global : BindKind::Local, element.range, scope);
                index_->bindings[static_cast<size_t>(id)].writes++;
            }
            return;
        }
        Walk(lhs, scope);
    }

    void WalkIndex(int node, int scope, bool as_write) {
        const MxNode &n = At(node);
        if (!n.kids.empty()) {
            const MxNode &obj = At(n.kids[0]);
            if (obj.kind == MxNodeKind::Ident) {
                const int existing = Lookup(obj.text, scope);
                if (as_write && existing < 0) {
                    const int id =
                        Bind(obj.text, scope == 0 ? BindKind::Global : BindKind::Local, obj.range, scope);
                    index_->bindings[static_cast<size_t>(id)].writes++;
                } else {
                    Use(obj.text, obj.range, scope, as_write, false, 0);
                }
            } else {
                Walk(n.kids[0], scope);
            }
        }
        for (const MxArg &arg : n.args) Walk(arg.value, scope);
    }

    // A loop that rebinds a name the enclosing block already declared
    // (`block([i], ..., for i : 1 thru n do ...)`) is *why* that
    // declaration is there -- it keeps the loop's variable from leaking
    // into the global environment. Declaring it and then looping over it
    // is the idiom, not a leftover, so the loop counts as writing it.
    void MarkShadowed(const std::string &name, int scope) {
        const int existing = Lookup(name, scope);
        if (existing < 0) return;
        index_->bindings[static_cast<size_t>(existing)].writes++;
    }

    /** @brief Finds a binding declared directly in `scope`, or -1. */
    int LookupLocal(const std::string &name, int scope) const {
        if (scope < 0) return -1;
        const Scope &sc = index_->scopes[static_cast<size_t>(scope)];
        for (int id : sc.bindings) {
            if (index_->bindings[static_cast<size_t>(id)].name == name) return id;
        }
        return -1;
    }

    void WalkCall(int node, int scope) {
        const MxNode &n = At(node);
        const int callee_node = n.kids.empty() ? -1 : n.kids[0];
        const MxNode &callee = At(callee_node);
        const std::string name = callee.kind == MxNodeKind::Ident ? callee.text : std::string();

        if (name == "block" || name == "lambda" || name == "buildq") {
            // `buildq([a, b : x], template)` binds names over a template
            // the same way a block binds them over a body -- the only
            // difference is that the template is substituted into rather
            // than evaluated, which is what quote_depth_ records.
            const bool quoting = name == "buildq";
            if (quoting) quote_depth_++;
            WalkBlockLike(node, scope, name == "lambda", quoting);
            if (quoting) quote_depth_--;
            return;
        }
        if (name == "local") {
            // `local(a, b)` inside a block declares those names local to
            // it -- the one declaration form that is a call.
            for (const MxArg &arg : n.args) {
                const MxNode &a = At(arg.value);
                if (a.kind == MxNodeKind::Ident) {
                    MarkShadowed(a.text, scope);
                    Bind(a.text, BindKind::Local, a.range, scope);
                } else {
                    Walk(arg.value, scope);
                }
            }
            Use(name, callee.range, scope, false, true, static_cast<int>(n.args.size()));
            return;
        }
        if (name == "define" && n.args.size() == 2) {
            // `define(f(x), body)` is `f(x) := body` with the body
            // evaluated first -- the same binding, written the other way.
            // `define(funmake('f, vars), body)` is not: the name and the
            // parameter list are both computed, and nothing here can say
            // what they come out as.
            const MxNode &head = At(n.args[0].value);
            const MxNode &head_callee = head.kids.empty() ? At(-1) : At(head.kids[0]);
            const bool computed_head =
                head_callee.kind == MxNodeKind::Ident &&
                (head_callee.text == "funmake" || head_callee.text == "apply" || head_callee.text == "concat" ||
                 head_callee.text == "ev");
            if ((head.kind == MxNodeKind::Call || head.kind == MxNodeKind::Index) && !computed_head) {
                WalkDefinition(node, n.args[0].value, n.args[1].value, scope, false);
                return;
            }
            if (computed_head) index_->opaque = true;
        }
        if (const BinderSpec *spec = FindBinder(name)) {
            WalkBinderCall(node, scope, *spec);
            return;
        }
        if (IsOpaqueCall(name)) index_->opaque = true;

        if (!name.empty()) {
            Use(name, callee.range, scope, false, true, static_cast<int>(n.args.size()));
        } else {
            Walk(callee_node, scope);
        }

        const bool unevaluated = TakesUnevaluatedArgs(name);
        if (unevaluated) quote_depth_++;
        for (const MxArg &arg : n.args) Walk(arg.value, scope);
        if (unevaluated) quote_depth_--;
    }

    // `block([a, b : 1], body...)`, `lambda([x, y], body...)` and
    // `buildq([a, b : x], template)`.
    void WalkBlockLike(int node, int scope, bool lambda, bool reads_outer = false) {
        const MxNode &n = At(node);
        if (n.args.empty()) return;
        const MxNode &first = At(n.args[0].value);
        size_t body_start = 0;
        int body_scope = scope;
        if (first.kind == MxNodeKind::List) {
            body_scope = NewScope(scope, node);
            body_start = 1;
            for (int kid : first.kids) {
                const MxNode &decl = At(kid);
                std::string local_name;
                bool initialized = false;
                MxRange range = decl.range;
                if (decl.kind == MxNodeKind::Ident) {
                    local_name = decl.text;
                    // A bare name in a `buildq` list means "substitute
                    // what this name holds out here", so it is a read of
                    // the enclosing binding as well as a new one.
                    if (reads_outer) Use(local_name, decl.range, scope, false, false, 0);
                } else if (decl.kind == MxNodeKind::Binary && decl.text == ":" && decl.kids.size() == 2 &&
                           At(decl.kids[0]).kind == MxNodeKind::Ident) {
                    local_name = At(decl.kids[0]).text;
                    range = At(decl.kids[0]).range;
                    initialized = true;
                    // The initializer is evaluated outside the block, in
                    // the scope the block itself was written in.
                    Walk(decl.kids[1], scope);
                } else {
                    Walk(kid, scope);
                    continue;
                }
                if (LookupLocal(local_name, body_scope) >= 0) {
                    Diag(range, MaximaLspSeverity::Warning, "duplicate-local",
                         "`" + local_name + "` is declared twice in this " + (lambda ? "lambda" : "block") + ".");
                }
                MarkShadowed(local_name, scope);
                const int id = Bind(local_name, lambda ? BindKind::Param : BindKind::Local, range, body_scope);
                index_->bindings[static_cast<size_t>(id)].initialized = initialized;
            }
        } else if (lambda) {
            // `lambda` without a parameter list is a syntax mistake
            // Maxima catches at run time rather than at read time.
            Diag(At(n.args[0].value).range, MaximaLspSeverity::Warning, "lambda-without-parameters",
                 "`lambda` takes its parameters as a list first: lambda([x], body).");
        }
        bool returned = false;
        for (size_t i = body_start; i < n.args.size(); i++) {
            const int body_node = n.args[i].value;
            if (returned) {
                Diag(At(body_node).range, MaximaLspSeverity::Information, "unreachable-after-return",
                     "`return` above has already left this block, so this is never evaluated.");
                returned = false;  // one report per block is enough
            }
            CheckStatementValue(body_node, i + 1 == n.args.size());
            Walk(body_node, body_scope);
            if (IsReturnCall(body_node)) returned = true;
        }
    }

    /** @brief Reports whether a body expression is a bare `return(...)` call. */
    bool IsReturnCall(int node) const {
        const MxNode &n = At(node);
        if (n.kind != MxNodeKind::Call || n.kids.empty()) return false;
        const MxNode &callee = At(n.kids[0]);
        return callee.kind == MxNodeKind::Ident && callee.text == "return";
    }

    // A body expression whose value is thrown away and which changes
    // nothing: `x = 1` in the middle of a block is the classic Maxima
    // slip, because `=` builds an equation and `:` is the assignment.
    void CheckStatementValue(int node, bool is_last) {
        if (is_last) return;
        const MxNode &n = At(node);
        if (n.kind != MxNodeKind::Binary || n.text != "=") return;
        if (n.kids.empty()) return;
        const MxNode &lhs = At(n.kids[0]);
        if (lhs.kind != MxNodeKind::Ident) return;
        Diag(n.range, MaximaLspSeverity::Information, "equation-as-statement",
             "`=` builds an equation and this one is discarded; assignment is `" + lhs.text + " : ...`.");
    }

    void WalkBinderCall(int node, int scope, const BinderSpec &spec) {
        const MxNode &n = At(node);
        const MxNode &callee = At(n.kids.empty() ? -1 : n.kids[0]);
        if (callee.kind == MxNodeKind::Ident) {
            Use(callee.text, callee.range, scope, false, true, static_cast<int>(n.args.size()));
        }
        const int var_index = spec.var_arg;
        const int body_index = spec.body_arg;
        int body_scope = scope;
        if (static_cast<size_t>(var_index) < n.args.size()) {
            const MxNode &var = At(n.args[static_cast<size_t>(var_index)].value);
            if (var.kind == MxNodeKind::Ident) {
                MarkShadowed(var.text, scope);
                body_scope = NewScope(scope, node);
                Bind(var.text, BindKind::LoopVar, var.range, body_scope);
            }
        }
        for (size_t i = 0; i < n.args.size(); i++) {
            if (static_cast<int>(i) == var_index && body_scope != scope) continue;
            Walk(n.args[i].value, static_cast<int>(i) == body_index ? body_scope : scope);
        }
    }

    void WalkDo(int node, int scope) {
        const MxNode &n = At(node);
        int body_scope = scope;
        // Maxima's own clause-collision table (`def-collisions $do` in
        // src/nparse.lisp): a loop cannot say both where to start and
        // what to iterate over, and cannot step two ways at once.
        std::vector<std::string> seen;
        for (const MxArg &arg : n.args) {
            const std::string &clause = arg.name;
            auto has = [&seen](const char *what) {
                return std::find(seen.begin(), seen.end(), what) != seen.end();
            };
            const char *conflict = nullptr;
            if (clause == "for" && has("for")) conflict = "for";
            if (clause == "in" && (has("in") || has("from") || has("step") || has("next") || has("thru"))) {
                conflict = has("in") ? "in" : (has("from") ? "from" : (has("step") ? "step" : (has("next") ? "next" : "thru")));
            }
            if (clause == "from" && (has("in") || has("from"))) conflict = has("in") ? "in" : "from";
            if (clause == "step" && (has("in") || has("step") || has("next"))) {
                conflict = has("in") ? "in" : (has("step") ? "step" : "next");
            }
            if (clause == "next" && (has("in") || has("step") || has("next"))) {
                conflict = has("in") ? "in" : (has("step") ? "step" : "next");
            }
            if (clause == "thru" && (has("in") || has("thru"))) conflict = has("in") ? "in" : "thru";
            if (conflict != nullptr) {
                Diag(arg.range, MaximaLspSeverity::Error, "loop-clause-conflict",
                     "A loop cannot have both `" + std::string(conflict) + "` and `" + clause + "`.");
            }
            seen.push_back(clause);

            if (clause == "for") {
                const MxNode &var = At(arg.value);
                if (var.kind == MxNodeKind::Ident) {
                    MarkShadowed(var.text, body_scope);
                    body_scope = NewScope(scope, node);
                    Bind(var.text, BindKind::LoopVar, var.range, body_scope);
                    continue;
                }
            }
            Walk(arg.value, body_scope);
        }
    }

    void Diag(const MxRange &range, MaximaLspSeverity severity, const char *code, const std::string &message) {
        MaximaLspDiagnostic d;
        d.range = range;
        d.severity = severity;
        d.code = code;
        d.message = message;
        index_->diags.push_back(d);
    }
};

// --- Built-in name lookup ---------------------------------------------

/** @brief Binary-searches the generated name table. */
const MaximaBuiltinDecl *FindDecl(const std::string &name) {
    size_t lo = 0;
    size_t hi = kMaximaBuiltinDeclCount;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int cmp = name.compare(kMaximaBuiltinDecls[mid].name);
        if (cmp == 0) return &kMaximaBuiltinDecls[mid];
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return nullptr;
}

}  // namespace

bool MaximaLspIsBuiltin(const std::string &name) { return FindDecl(name) != nullptr; }

const MaximaBuiltinDecl *MaximaLspFindBuiltin(const std::string &name) { return FindDecl(name); }

const std::vector<std::string> &MaximaLspBuiltinNameList() {
    static const std::vector<std::string> kNames = [] {
        std::vector<std::string> out;
        out.reserve(kMaximaBuiltinNameCount);
        for (size_t i = 0; i < kMaximaBuiltinNameCount; i++) out.emplace_back(kMaximaBuiltinNames[i]);
        return out;
    }();
    return kNames;
}

bool MaximaLspIsProtectedName(const std::string &name) {
    // The names Maxima refuses to let a program assign to. `%pi : 3` is
    // not a redefinition, it is an error at run time -- and the reason
    // is worth saying out loud, because every other name in the language
    // is assignable.
    static const char *const kProtected[] = {
        "%pi", "%e", "%i", "%phi", "%gamma", "%catalan", "true", "false", "inf", "minf", "und", "ind", "infinity",
        "%", "%%", "all", "done",
    };
    for (const char *p : kProtected) {
        if (name == p) return true;
    }
    return false;
}

std::vector<std::string> MaximaLspSplitSignatures(const std::string &signature) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= signature.size()) {
        const size_t bar = signature.find(" | ", start);
        if (bar == std::string::npos) {
            if (start < signature.size()) out.push_back(signature.substr(start));
            break;
        }
        out.push_back(signature.substr(start, bar - start));
        start = bar + 3;
    }
    return out;
}

std::vector<std::string> MaximaLspSplitParams(const std::string &signature) {
    std::vector<std::string> out;
    const size_t open = signature.find_first_of("([");
    if (open == std::string::npos) return out;
    const size_t close = signature.find_last_of(")]");
    if (close == std::string::npos || close <= open) return out;
    const std::string inner = signature.substr(open + 1, close - open - 1);
    int depth = 0;
    std::string current;
    for (char c : inner) {
        if (c == '(' || c == '[') depth++;
        if (c == ')' || c == ']') depth--;
        if (c == ',' && depth == 0) {
            out.push_back(current);
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty()) out.push_back(current);
    for (std::string &p : out) {
        size_t b = p.find_first_not_of(" \t");
        size_t e = p.find_last_not_of(" \t");
        p = b == std::string::npos ? std::string() : p.substr(b, e - b + 1);
    }
    return out;
}

namespace {

/** @brief Builds the whole index for a parsed document. */
Index BuildIndex(const MxParseResult &parse) {
    Index index;
    Walker walker(parse, &index);
    walker.Run();
    return index;
}

/** @brief The severity a parse error is reported at. */
MaximaLspSeverity SeverityForParseError(const std::string &code) {
    // An operator nothing declared is almost always one a package the
    // file loads brings in, so it is a note rather than a failure -- the
    // same call org_lsp.cpp makes for a `#+begin_src` language it cannot
    // run. Everything else the reader rejects, Maxima rejects too.
    if (code == "undeclared-operator") return MaximaLspSeverity::Information;
    if (code == "missing-terminator") return MaximaLspSeverity::Warning;
    return MaximaLspSeverity::Error;
}

/** @brief Orders diagnostics by position, then by code, so output is stable. */
bool DiagLess(const MaximaLspDiagnostic &a, const MaximaLspDiagnostic &b) {
    if (a.range.line != b.range.line) return a.range.line < b.range.line;
    if (a.range.col != b.range.col) return a.range.col < b.range.col;
    return a.code < b.code;
}

/** @brief The closest documented name to `name` within `limit` edits, or "". */
std::string ClosestBuiltin(const std::string &name, int limit) {
    std::string best;
    int best_distance = limit + 1;
    for (size_t i = 0; i < kMaximaBuiltinNameCount; i++) {
        const char *candidate = kMaximaBuiltinNames[i];
        // A cheap length gate first: the table has nearly three thousand
        // names and the edit distance is the expensive part.
        const size_t len = std::char_traits<char>::length(candidate);
        if (len + static_cast<size_t>(limit) < name.size() || name.size() + static_cast<size_t>(limit) < len) continue;
        const int d = EditDistance(name, candidate, limit);
        if (d < best_distance) {
            best_distance = d;
            best = candidate;
        }
    }
    return best_distance <= limit ? best : std::string();
}

}  // namespace

std::vector<MaximaLspDiagnostic> MaximaLspDiagnostics(const std::vector<std::string> &lines,
                                                      const MaximaLspOptions &opts) {
    const MxParseResult parse = MxParse(lines);
    std::vector<MaximaLspDiagnostic> out;
    for (const MxParseError &e : parse.errors) {
        MaximaLspDiagnostic d;
        d.range = e.range;
        d.severity = SeverityForParseError(e.code);
        d.code = e.code;
        d.message = e.message;
        out.push_back(d);
    }

    Index index = BuildIndex(parse);
    for (const MaximaLspDiagnostic &d : index.diags) out.push_back(d);

    // A file that reads another file, or that builds a name out of a
    // string, can define anything at all; past that point the name
    // checks below would be guessing.
    const std::vector<MxToken> tokens = MxTokenize(lines, nullptr, nullptr, nullptr);
    const std::vector<std::string> packages = MxScanLoadedPackages(tokens);
    bool unknown_package = false;
    for (const std::string &pkg : packages) {
        // A package whose own names this server ships is fine; one it
        // has never heard of may define anything.
        if (!MaximaLspIsBuiltin(pkg)) unknown_package = true;
    }
    const bool syntax_broken = !parse.errors.empty();
    // `kill(f)` takes a definition away again, and the test files in
    // Maxima's own tree define, kill and redefine the same names all the
    // way down. Once a document does that, the definition this server
    // can see is not the one a given call is made against, so the
    // argument-count check has nothing solid to stand on.
    bool kills_definitions = false;
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if (tokens[i].kind == MxTokKind::Ident && tokens[i + 1].kind == MxTokKind::LParen &&
            (tokens[i].text == "kill" || tokens[i].text == "remfunction" || tokens[i].text == "remvalue" ||
             tokens[i].text == "translate" || tokens[i].text == "compile")) {
            kills_definitions = true;
        }
    }

    // --- Unused locals ------------------------------------------------
    for (const Binding &b : index.bindings) {
        // Locals only. An unused *parameter* is not a defect the way an
        // unused local is: a function that has to match a caller's
        // protocol -- an ODE right-hand side that ignores `t`, a
        // comparison predicate that looks at one of its arguments --
        // declares the parameter because the interface has it. Measured
        // against Maxima's own share tree that check produced 288
        // reports and no defects.
        if (b.kind != BindKind::Local) continue;
        if (b.reads > 0) continue;
        if (b.name.empty() || b.name[0] == '_') continue;  // `_x` is the usual "I know" spelling
        // A local that was given a value is not dead code even when the
        // block's own body never reads it: Maxima binds globals
        // dynamically, so `block([ps_scale : [40, 20]], plot2d(...))` is
        // how a caller passes a setting down to a callee. Only a name
        // declared bare and never touched at all is the leftover this
        // check is for.
        if (b.initialized || b.writes > 0) continue;
        MaximaLspDiagnostic d;
        d.range = b.range;
        d.severity = MaximaLspSeverity::Information;
        d.code = "unused-local";
        d.message = "`" + b.name + "` is declared local here and never read.";
        out.push_back(d);
    }

    // --- Call checks --------------------------------------------------
    for (const Occurrence &occ : index.occurrences) {
        if (!occ.is_call) continue;
        if (occ.binding >= 0) {
            const Binding &b = index.bindings[static_cast<size_t>(occ.binding)];
            if (b.arity < 0 || b.variadic || occ.quoted || kills_definitions) continue;
            if (b.kind != BindKind::Function && b.kind != BindKind::Macro) continue;
            if (occ.argc >= b.min_arity && occ.argc <= b.arity) continue;
            MaximaLspDiagnostic d;
            d.range = occ.range;
            d.severity = MaximaLspSeverity::Warning;
            d.code = "wrong-argument-count";
            d.message = "`" + b.signature + "` takes " +
                        (b.min_arity == b.arity ? std::to_string(b.arity)
                                                : std::to_string(b.min_arity) + " to " + std::to_string(b.arity)) +
                        " argument" + (b.arity == 1 ? "" : "s") + ", but this call passes " +
                        std::to_string(occ.argc) + ".";
            out.push_back(d);
            continue;
        }
        if (syntax_broken || index.opaque || unknown_package || occ.quoted) continue;
        // No argument-count check against the manual at all. Its
        // argument lists are a sample rather than the whole set --
        // `subst` has three forms and documents one, `reset` takes any
        // number, `return()` is legal and documented as `return(value)`
        // -- and measured against Maxima's own share tree every report
        // it produced was one of those gaps rather than a defect. What
        // is checked is the calls this file can actually settle: the
        // ones to a function defined in the same file, above.
        if (MaximaLspIsBuiltin(occ.name)) continue;
        // An unknown name is not an error in a computer-algebra system:
        // `f(x)` with no `f` defined is a perfectly good noun expression,
        // and reporting those would bury the file in complaints about
        // the mathematics. Even "it is one edit from a real name" is not
        // enough on its own -- `nfloat`, `sfloat`, `match` and `picture`
        // are all real functions from packages this server ships no name
        // list for, and each one is one edit from a documented name.
        // What is left, and what this reports, is a name used *once*, in
        // a file that loads nothing and builds no names, that is one edit
        // from a core name. A typo is a one-off; a package function is
        // not.
        if (occ.name.size() < 4) continue;
        if (!packages.empty()) continue;
        int uses = 0;
        for (const Occurrence &other : index.occurrences) {
            if (other.name == occ.name) uses++;
        }
        if (uses != 1) continue;
        const std::string closest = ClosestBuiltin(occ.name, 1);
        if (closest.empty() || closest == occ.name) continue;
        const MaximaBuiltinDecl *near = FindDecl(closest);
        if (near == nullptr || near->package[0] != '\0') continue;
        MaximaLspDiagnostic d;
        d.range = occ.range;
        d.severity = MaximaLspSeverity::Information;
        d.code = "unknown-function";
        d.message = "Nothing defines `" + occ.name + "`, so this stays an unevaluated expression. Did you mean `" +
                    closest + "`?";
        out.push_back(d);
    }

    // --- `load()` targets ---------------------------------------------
    if (opts.check_files && !opts.doc_dir.empty()) {
        for (size_t i = 0; i + 2 < tokens.size(); i++) {
            if (tokens[i].kind != MxTokKind::Ident) continue;
            if (tokens[i].text != "load" && tokens[i].text != "batch" && tokens[i].text != "batchload") continue;
            if (tokens[i + 1].kind != MxTokKind::LParen || tokens[i + 2].kind != MxTokKind::Str) continue;
            const std::string target = tokens[i + 2].text;
            // Only a path is checked. A bare `load(draw)` names a package
            // on Maxima's own search path, which is not this document's
            // directory and not something to guess at.
            if (target.find('/') == std::string::npos && target.find('.') == std::string::npos) continue;
            std::error_code ec;
            const std::filesystem::path path =
                target.front() == '/' ? std::filesystem::path(target) : std::filesystem::path(opts.doc_dir) / target;
            if (std::filesystem::exists(path, ec)) continue;
            MaximaLspDiagnostic d;
            d.range = tokens[i + 2].range;
            d.severity = MaximaLspSeverity::Information;
            d.code = "missing-load-target";
            d.message = "No file `" + target + "` next to this one; Maxima will look for it on its own search path.";
            out.push_back(d);
        }
    }

    std::stable_sort(out.begin(), out.end(), DiagLess);
    return out;
}

// --- Position helpers --------------------------------------------------

namespace {

// The name the cursor is on or immediately after, with its byte columns.
struct WordAt {
    bool found = false;
    std::string text;
    int start = 0;
    int end = 0;
};

WordAt NameAt(const std::vector<std::string> &lines, int line, int col) {
    WordAt out;
    if (line < 0 || line >= static_cast<int>(lines.size())) return out;
    const std::string &text = lines[static_cast<size_t>(line)];
    int c = std::min(col, static_cast<int>(text.size()));
    // A cursor just past the end of a name belongs to that name, which is
    // what completion and hover both want.
    if (c > 0 && (c == static_cast<int>(text.size()) || !MxIsNameChar(text[static_cast<size_t>(c)])) &&
        MxIsNameChar(text[static_cast<size_t>(c - 1)])) {
        c--;
    }
    if (c < 0 || c >= static_cast<int>(text.size()) || !MxIsNameChar(text[static_cast<size_t>(c)])) return out;
    int start = c;
    while (start > 0 && MxIsNameChar(text[static_cast<size_t>(start - 1)])) start--;
    int end = c;
    while (end < static_cast<int>(text.size()) && MxIsNameChar(text[static_cast<size_t>(end)])) end++;
    // A name cannot start with a digit -- that would be a number with a
    // suffix, which Maxima reads as two tokens.
    if (!MxIsNameStart(text[static_cast<size_t>(start)])) return out;
    out.found = true;
    out.text = text.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
    out.start = start;
    out.end = end;
    return out;
}

/** @brief The occurrence whose range covers a position, or -1. */
int OccurrenceAt(const Index &index, int line, int col) {
    for (size_t i = 0; i < index.occurrences.size(); i++) {
        if (MxRangeContains(index.occurrences[i].range, line, col)) return static_cast<int>(i);
    }
    return -1;
}

/** @brief The binding whose defining name covers a position, or -1. */
int BindingAt(const Index &index, int line, int col) {
    for (size_t i = 0; i < index.bindings.size(); i++) {
        if (MxRangeContains(index.bindings[i].range, line, col)) return static_cast<int>(i);
    }
    return -1;
}

/** @brief A one-line description of what a binding is. */
std::string DescribeBinding(const Binding &b) {
    switch (b.kind) {
        case BindKind::Function: return b.signature.empty() ? "function" : b.signature;
        case BindKind::ArrayFun: return b.signature.empty() ? "array function" : b.signature + "  (array function)";
        case BindKind::Macro: return b.signature.empty() ? "macro" : b.signature + "  (macro)";
        case BindKind::Param: return "parameter";
        case BindKind::Local: return "block local";
        case BindKind::LoopVar: return "loop variable";
        case BindKind::Operator: return "operator";
        case BindKind::Global: break;
    }
    return "variable";
}

}  // namespace

// --- Hover -------------------------------------------------------------

MaximaLspHoverInfo MaximaLspHover(const std::vector<std::string> &lines, int line, int col) {
    MaximaLspHoverInfo out;
    const WordAt word = NameAt(lines, line, col);
    if (!word.found) {
        // Not on a name: the operator under the cursor is the next best
        // thing to explain, since Maxima's assignment/equality/product
        // operators are exactly what a newcomer gets wrong.
        if (line < 0 || line >= static_cast<int>(lines.size())) return out;
        const std::string &text = lines[static_cast<size_t>(line)];
        for (const MxOperatorInfo &op : MxOperatorTable()) {
            const std::string spelling = op.text;
            if (spelling.empty() || MxIsNameStart(spelling[0])) continue;
            const size_t len = spelling.size();
            for (int start = std::max(0, col - static_cast<int>(len) + 1); start <= col; start++) {
                if (static_cast<size_t>(start) + len > text.size()) continue;
                if (text.compare(static_cast<size_t>(start), len, spelling) != 0) continue;
                const MaximaLspVocabEntry *entry = MaximaLspFindVocab(spelling);
                if (entry == nullptr) continue;
                out.found = true;
                out.text = std::string("`") + entry->name + "` -- " + entry->detail + "\n\n" + entry->doc;
                out.range.line = out.range.end_line = line;
                out.range.col = start;
                out.range.end_col = start + static_cast<int>(len);
                return out;
            }
        }
        return out;
    }

    out.range.line = out.range.end_line = line;
    out.range.col = word.start;
    out.range.end_col = word.end;

    const MxParseResult parse = MxParse(lines);
    const Index index = BuildIndex(parse);
    const int occ = OccurrenceAt(index, line, word.start);
    int binding = occ >= 0 ? index.occurrences[static_cast<size_t>(occ)].binding : -1;
    if (binding < 0) binding = BindingAt(index, line, word.start);
    if (binding >= 0) {
        const Binding &b = index.bindings[static_cast<size_t>(binding)];
        out.found = true;
        out.text = "`" + b.name + "` -- " + DescribeBinding(b) + "\n\nDefined on line " +
                   std::to_string(b.range.line + 1) + " of this file.";
        return out;
    }

    if (const MaximaLspVocabEntry *entry = MaximaLspFindVocab(word.text)) {
        out.found = true;
        out.text = std::string("`") + entry->name + "` -- " + entry->detail + "\n\n" + entry->doc;
        return out;
    }
    if (const MaximaBuiltinDecl *decl = FindDecl(word.text)) {
        out.found = true;
        std::string text = "`";
        text += decl->signature[0] != '\0' ? decl->signature : decl->name;
        text += "`";
        text += std::string(" -- Maxima ") + decl->kind;
        if (decl->package[0] != '\0') {
            text += std::string("\n\nFrom the `") + decl->package + "` package: `load(" + decl->package +
                    ")` before using it.";
        }
        out.text = text;
        return out;
    }
    // An unbound name in Maxima is a symbol, which is a real and useful
    // thing to be -- saying so is more helpful than saying nothing.
    out.found = true;
    out.text = "`" + word.text + "` -- an unbound symbol.\n\nNothing in this file binds it, so Maxima treats it as "
               "an algebraic unknown.";
    return out;
}

// --- Completion --------------------------------------------------------

namespace {

/** @brief Reports whether `prefix` is a prefix of `name`, case-sensitively (Maxima is). */
bool HasPrefix(const std::string &name, const std::string &prefix) {
    return name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
}

void AddCompletion(std::vector<MaximaLspCompletionItem> *out, const WordAt &word, const std::string &label,
                   MaximaLspKind kind, const std::string &detail, const std::string &doc) {
    MaximaLspCompletionItem item;
    item.label = label;
    item.insert_text = label;
    item.kind = kind;
    item.detail = detail;
    item.documentation = doc;
    item.replace_start = word.start;
    item.replace_end = word.end;
    out->push_back(item);
}

}  // namespace

std::vector<MaximaLspCompletionItem> MaximaLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                          const MaximaLspOptions &opts) {
    (void)opts;
    std::vector<MaximaLspCompletionItem> out;
    WordAt word = NameAt(lines, line, col);
    if (!word.found) {
        word.found = true;
        word.start = col;
        word.end = col;
        word.text.clear();
    } else {
        // Complete what is typed *before* the cursor, not the whole word
        // the cursor sits in the middle of.
        word.end = std::max(word.start, col);
        word.text = word.text.substr(0, static_cast<size_t>(word.end - word.start));
    }
    const std::string prefix = word.text;

    const MxParseResult parse = MxParse(lines);
    const Index index = BuildIndex(parse);

    // What this file binds, innermost first: a block local is much more
    // likely to be what is being typed than a name from the manual.
    std::set<std::string> seen;
    for (const Binding &b : index.bindings) {
        if (!HasPrefix(b.name, prefix) || b.name.empty()) continue;
        if (!seen.insert(b.name).second) continue;
        const MaximaLspKind kind = (b.kind == BindKind::Function || b.kind == BindKind::Macro)
                                       ? MaximaLspKind::Function
                                       : (b.kind == BindKind::ArrayFun ? MaximaLspKind::Field
                                                                       : MaximaLspKind::Variable);
        AddCompletion(&out, word, b.name, kind, DescribeBinding(b), "Defined in this file.");
    }
    for (const MxUserOperator &op : parse.user_operators) {
        if (!HasPrefix(op.text, prefix) || !seen.insert(op.text).second) continue;
        AddCompletion(&out, word, op.text, MaximaLspKind::Operator,
                      op.package.empty() ? "operator declared in this file" : "operator from " + op.package, "");
    }
    for (const MaximaLspVocabEntry &e : MaximaLspKeywordVocab()) {
        if (!HasPrefix(e.name, prefix) || !seen.insert(e.name).second) continue;
        AddCompletion(&out, word, e.name, MaximaLspKind::Keyword, e.detail, e.doc);
    }
    for (const MaximaLspVocabEntry &e : MaximaLspConstantVocab()) {
        if (!HasPrefix(e.name, prefix) || !seen.insert(e.name).second) continue;
        AddCompletion(&out, word, e.name, MaximaLspKind::Constant, e.detail, e.doc);
    }
    for (size_t i = 0; i < kMaximaBuiltinDeclCount; i++) {
        const MaximaBuiltinDecl &decl = kMaximaBuiltinDecls[i];
        const std::string name = decl.name;
        if (!HasPrefix(name, prefix) || name.empty()) continue;
        // The manual indexes the operators under their own spellings
        // (`!`, `#`, `:=`); those are not names anybody completes.
        if (!MxIsNameStart(name[0])) continue;
        if (!seen.insert(name).second) continue;
        const MaximaLspVocabEntry *vocab = MaximaLspFindVocab(name);
        std::string detail = decl.signature[0] != '\0' ? MaximaLspSplitSignatures(decl.signature).front() : decl.kind;
        if (decl.package[0] != '\0') detail += "  [" + std::string(decl.package) + "]";
        AddCompletion(&out, word, name,
                      std::string(decl.kind) == "function" ? MaximaLspKind::Function : MaximaLspKind::Variable, detail,
                      vocab != nullptr ? vocab->doc : "");
    }
    return out;
}

// --- Document symbols --------------------------------------------------

namespace {

// A comment that is only rules and a title -- `/* ---- Utilities ---- */`
// -- is a section header in every Maxima source file that has any
// structure at all, and the outline is where that belongs.
bool BannerTitle(const MxComment &comment, std::string *title) {
    std::string inner = comment.text;
    if (inner.size() < 4) return false;
    inner = inner.substr(2, inner.size() - 4);
    std::string collapsed;
    bool has_rule = false;
    int rule_run = 0;
    for (char c : inner) {
        if (c == '-' || c == '=' || c == '*' || c == '_' || c == '#') {
            rule_run++;
            if (rule_run >= 3) has_rule = true;
            continue;
        }
        rule_run = 0;
        if (c == '\n' || c == '\r' || c == '\t') {
            collapsed += ' ';
            continue;
        }
        collapsed += c;
    }
    if (!has_rule) return false;
    const size_t b = collapsed.find_first_not_of(' ');
    const size_t e = collapsed.find_last_not_of(' ');
    if (b == std::string::npos) return false;
    *title = collapsed.substr(b, e - b + 1);
    return !title->empty() && title->size() < 80;
}

}  // namespace

std::vector<MaximaLspSymbol> MaximaLspSymbols(const std::vector<std::string> &lines) {
    const MxParseResult parse = MxParse(lines);
    const Index index = BuildIndex(parse);
    std::vector<MaximaLspSymbol> out;

    struct Pending {
        MxRange range;
        std::string name;
        std::string detail;
        MaximaLspSymbolKind kind;
        MxRange sel;
    };
    std::vector<Pending> items;
    for (const MxComment &c : parse.comments) {
        std::string title;
        if (!BannerTitle(c, &title)) continue;
        items.push_back({c.range, title, "section", MaximaLspSymbolKind::Namespace, c.range});
    }
    for (const Binding &b : index.bindings) {
        if (b.scope != 0) continue;  // the file's own top level only
        MaximaLspSymbolKind kind = MaximaLspSymbolKind::Variable;
        switch (b.kind) {
            case BindKind::Function:
            case BindKind::Macro: kind = MaximaLspSymbolKind::Function; break;
            case BindKind::ArrayFun: kind = MaximaLspSymbolKind::Array; break;
            case BindKind::Operator: kind = MaximaLspSymbolKind::Operator; break;
            default: break;
        }
        items.push_back({b.def_range, b.name, DescribeBinding(b), kind, b.range});
    }
    for (const MxUserOperator &op : parse.user_operators) {
        if (!op.package.empty()) continue;  // brought in by a load, not defined here
        items.push_back({op.range, op.text, "operator", MaximaLspSymbolKind::Operator, op.range});
    }
    std::stable_sort(items.begin(), items.end(), [](const Pending &a, const Pending &b) {
        if (a.range.line != b.range.line) return a.range.line < b.range.line;
        return a.range.col < b.range.col;
    });

    int current_section = -1;
    for (const Pending &item : items) {
        MaximaLspSymbol sym;
        sym.name = item.name;
        sym.detail = item.detail;
        sym.kind = item.kind;
        sym.range = item.range;
        sym.sel_range = item.sel;
        if (item.kind == MaximaLspSymbolKind::Namespace) {
            current_section = static_cast<int>(out.size());
        } else if (current_section >= 0) {
            sym.parent = current_section;
        }
        out.push_back(sym);
    }
    // A section owns everything under it, which is only knowable once the
    // next section starts.
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i].kind != MaximaLspSymbolKind::Namespace) continue;
        int end_line = static_cast<int>(lines.size()) - 1;
        for (size_t j = i + 1; j < out.size(); j++) {
            if (out[j].kind == MaximaLspSymbolKind::Namespace) {
                end_line = out[j].range.line - 1;
                break;
            }
        }
        out[i].range.end_line = std::max(out[i].range.end_line, end_line);
        out[i].range.end_col = 0;
    }
    return out;
}

// --- Folding -----------------------------------------------------------

std::vector<MaximaLspFold> MaximaLspFolds(const std::vector<std::string> &lines) {
    const MxParseResult parse = MxParse(lines);
    std::vector<MaximaLspFold> out;

    // Every statement that spans more than one line, plus the multi-line
    // constructs inside it: a `block` that fills a screen is the thing
    // anyone reading a Maxima file wants collapsed first.
    auto add = [&out](const MxRange &r, const char *kind) {
        if (r.end_line <= r.line) return;
        for (const MaximaLspFold &f : out) {
            if (f.start_line == r.line && f.end_line == r.end_line) return;
        }
        MaximaLspFold fold;
        fold.start_line = r.line;
        fold.end_line = r.end_line;
        fold.kind = kind;
        out.push_back(fold);
    };
    for (const MxStatement &st : parse.statements) add(st.range, "");
    for (const MxNode &n : parse.nodes) {
        switch (n.kind) {
            case MxNodeKind::Call:
            case MxNodeKind::List:
            case MxNodeKind::Set:
            case MxNodeKind::Paren:
            case MxNodeKind::Do:
            case MxNodeKind::If: add(n.range, ""); break;
            default: break;
        }
    }
    for (const MxComment &c : parse.comments) add(c.range, "comment");

    std::stable_sort(out.begin(), out.end(), [](const MaximaLspFold &a, const MaximaLspFold &b) {
        if (a.start_line != b.start_line) return a.start_line < b.start_line;
        return a.end_line > b.end_line;
    });
    return out;
}

// --- Definition --------------------------------------------------------

MaximaLspLocation MaximaLspDefinition(const std::vector<std::string> &lines, int line, int col,
                                      const MaximaLspOptions &opts) {
    MaximaLspLocation out;
    // A `load("helpers.mac")` path resolves to that file, which is the
    // one cross-file jump a Maxima document has.
    const std::vector<MxToken> tokens = MxTokenize(lines, nullptr, nullptr, nullptr);
    for (size_t i = 0; i + 2 < tokens.size(); i++) {
        if (tokens[i].kind != MxTokKind::Ident) continue;
        if (tokens[i].text != "load" && tokens[i].text != "batch" && tokens[i].text != "batchload") continue;
        if (tokens[i + 1].kind != MxTokKind::LParen || tokens[i + 2].kind != MxTokKind::Str) continue;
        if (!MxRangeContains(tokens[i + 2].range, line, col)) continue;
        if (!opts.check_files || opts.doc_dir.empty()) return out;
        const std::string target = tokens[i + 2].text;
        std::error_code ec;
        const std::filesystem::path path =
            !target.empty() && target.front() == '/' ? std::filesystem::path(target)
                                                     : std::filesystem::path(opts.doc_dir) / target;
        if (!std::filesystem::exists(path, ec)) return out;
        out.found = true;
        out.path = path.string();
        return out;
    }

    const WordAt word = NameAt(lines, line, col);
    if (!word.found) return out;
    const MxParseResult parse = MxParse(lines);
    const Index index = BuildIndex(parse);
    const int occ = OccurrenceAt(index, line, word.start);
    int binding = occ >= 0 ? index.occurrences[static_cast<size_t>(occ)].binding : -1;
    if (binding < 0) binding = BindingAt(index, line, word.start);
    if (binding < 0) return out;
    out.found = true;
    out.range = index.bindings[static_cast<size_t>(binding)].range;
    return out;
}

// --- References --------------------------------------------------------

std::vector<MaximaLspReference> MaximaLspReferences(const std::vector<std::string> &lines, int line, int col) {
    std::vector<MaximaLspReference> out;
    const WordAt word = NameAt(lines, line, col);
    if (!word.found) return out;
    const MxParseResult parse = MxParse(lines);
    const Index index = BuildIndex(parse);
    const int occ = OccurrenceAt(index, line, word.start);
    int binding = occ >= 0 ? index.occurrences[static_cast<size_t>(occ)].binding : -1;
    if (binding < 0) binding = BindingAt(index, line, word.start);

    if (binding >= 0) {
        const Binding &b = index.bindings[static_cast<size_t>(binding)];
        MaximaLspReference def;
        def.range = b.range;
        def.is_definition = true;
        def.is_write = true;
        out.push_back(def);
        for (const Occurrence &o : index.occurrences) {
            if (o.binding != binding) continue;
            MaximaLspReference ref;
            ref.range = o.range;
            ref.is_write = o.is_write;
            out.push_back(ref);
        }
    } else {
        // An unbound symbol still has occurrences worth finding: in a
        // computer-algebra file the free variable `x` is the subject of
        // the whole document.
        for (const Occurrence &o : index.occurrences) {
            if (o.name != word.text || o.binding >= 0) continue;
            MaximaLspReference ref;
            ref.range = o.range;
            ref.is_write = o.is_write;
            out.push_back(ref);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const MaximaLspReference &a, const MaximaLspReference &b) {
        if (a.range.line != b.range.line) return a.range.line < b.range.line;
        return a.range.col < b.range.col;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const MaximaLspReference &a, const MaximaLspReference &b) {
                              return a.range.line == b.range.line && a.range.col == b.range.col;
                          }),
              out.end());
    return out;
}

// --- Signature help ----------------------------------------------------

MaximaLspSignature MaximaLspSignatureHelp(const std::vector<std::string> &lines, int line, int col) {
    MaximaLspSignature out;
    const std::vector<MxToken> tokens = MxTokenize(lines, nullptr, nullptr, nullptr);

    // Walk the tokens up to the cursor, tracking the innermost call whose
    // parenthesis is still open and how many commas have gone by in it.
    struct Frame {
        std::string name;
        MxRange name_range;
        int arg = 0;
        bool is_call = false;
    };
    std::vector<Frame> stack;
    for (size_t i = 0; i < tokens.size(); i++) {
        const MxToken &t = tokens[i];
        if (t.range.line > line || (t.range.line == line && t.range.col >= col)) break;
        switch (t.kind) {
            case MxTokKind::LParen: {
                Frame f;
                if (i > 0 && tokens[i - 1].kind == MxTokKind::Ident) {
                    f.name = tokens[i - 1].text;
                    f.name_range = tokens[i - 1].range;
                    f.is_call = true;
                }
                stack.push_back(f);
                break;
            }
            case MxTokKind::LBracket:
            case MxTokKind::LBrace: stack.push_back(Frame{}); break;
            case MxTokKind::RParen:
            case MxTokKind::RBracket:
            case MxTokKind::RBrace:
                if (!stack.empty()) stack.pop_back();
                break;
            case MxTokKind::Comma:
                if (!stack.empty()) stack.back().arg++;
                break;
            case MxTokKind::Semi:
            case MxTokKind::Dollar: stack.clear(); break;
            default: break;
        }
    }
    while (!stack.empty() && !stack.back().is_call) stack.pop_back();
    if (stack.empty()) return out;
    const Frame &frame = stack.back();

    // A function defined in this file beats the manual: its signature is
    // the one that is actually being called.
    const MxParseResult parse = MxParse(lines);
    const Index index = BuildIndex(parse);
    for (const Binding &b : index.bindings) {
        if (b.name != frame.name || b.signature.empty()) continue;
        out.found = true;
        out.label = b.signature;
        out.params = MaximaLspSplitParams(b.signature);
        out.active_param = frame.arg < static_cast<int>(out.params.size()) ? frame.arg : -1;
        out.documentation = "Defined on line " + std::to_string(b.range.line + 1) + " of this file.";
        return out;
    }

    const MaximaBuiltinDecl *decl = FindDecl(frame.name);
    if (decl == nullptr || decl->signature[0] == '\0') return out;
    const std::vector<std::string> forms = MaximaLspSplitSignatures(decl->signature);
    if (forms.empty()) return out;
    // Offer the form that can actually take the argument being typed,
    // since Maxima overloads on arity far more than on type.
    size_t best = 0;
    for (size_t i = 0; i < forms.size(); i++) {
        const std::vector<std::string> params = MaximaLspSplitParams(forms[i]);
        if (static_cast<int>(params.size()) > frame.arg) {
            best = i;
            break;
        }
        best = i;
    }
    out.found = true;
    out.label = forms[best];
    out.params = MaximaLspSplitParams(out.label);
    out.active_param = frame.arg < static_cast<int>(out.params.size()) ? frame.arg : -1;
    for (size_t i = 0; i < forms.size(); i++) {
        if (i != best) out.alternatives.push_back(forms[i]);
    }
    if (const MaximaLspVocabEntry *entry = MaximaLspFindVocab(frame.name)) out.documentation = entry->doc;
    if (decl->package[0] != '\0') {
        if (!out.documentation.empty()) out.documentation += "\n\n";
        out.documentation += std::string("From the `") + decl->package + "` package.";
    }
    return out;
}
