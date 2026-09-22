// The analysis half of mep's own R language server: scope resolution,
// diagnostics, completion, hover, symbols, folding, definition,
// references and signature help, all over the AST r_lsp_parse.h
// produces. See r_lsp.h for the API and for what this deliberately does
// not do.
//
// One rule shapes every check in here: the table in r_lsp_vocab.cpp is
// only ever trusted for *positive* knowledge. A name that is in it gets a
// signature, a hover and argument checking; a name that is not in it is
// simply not annotated, never reported. R programs routinely call things
// this server cannot see -- a package's exports, a `source()`d file, an
// object built by `assign()` -- and a linter that treats "I have not
// heard of it" as "it is wrong" gets switched off within a day.

#include "r_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace {

// --- Tiny string helpers ----------------------------------------------

/** @brief Reports whether a byte can appear in an R name. */
bool IsNameByte(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '.' || c == '_' || u >= 0x80;
}

/** @brief Strips leading and trailing ASCII whitespace. */
std::string Trim(const std::string &s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) e--;
    return s.substr(b, e - b);
}

/** @brief Reports whether `s` starts with `prefix`. */
bool StartsWith(const std::string &s, const std::string &prefix) { return s.rfind(prefix, 0) == 0; }

/**
 * @brief Bounded Levenshtein distance, for "did you mean" suggestions.
 * @param a one word
 * @param b the other word
 * @param limit the largest distance worth distinguishing
 * @return the edit distance, or `limit + 1` once it is certainly above the limit
 */
int EditDistance(const std::string &a, const std::string &b, int limit) {
    const size_t la = a.size();
    const size_t lb = b.size();
    if (la > lb + static_cast<size_t>(limit) || lb > la + static_cast<size_t>(limit)) return limit + 1;
    std::vector<int> prev(lb + 1);
    std::vector<int> cur(lb + 1);
    for (size_t j = 0; j <= lb; j++) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= la; i++) {
        cur[0] = static_cast<int>(i);
        int row_best = cur[0];
        for (size_t j = 1; j <= lb; j++) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            row_best = std::min(row_best, cur[j]);
        }
        if (row_best > limit) return limit + 1;
        prev = cur;
    }
    return prev[lb];
}

/** @brief The one-line source text of a node's first line, trimmed, for hovers and symbol details. */
std::string FirstLineOf(const std::vector<std::string> &lines, const RRange &range) {
    if (range.line < 0 || static_cast<size_t>(range.line) >= lines.size()) return std::string();
    const std::string &line = lines[static_cast<size_t>(range.line)];
    const size_t from = std::min(static_cast<size_t>(range.col), line.size());
    const size_t to = range.end_line == range.line ? std::min(static_cast<size_t>(range.end_col), line.size())
                                                   : line.size();
    return Trim(line.substr(from, to - from));
}

/** @brief Shortens a string to `width` bytes, marking the cut with an ellipsis. */
std::string Ellipsize(const std::string &s, size_t width) {
    if (s.size() <= width) return s;
    return s.substr(0, width - 3) + "...";
}

// --- The scope model --------------------------------------------------

enum class BindKind { Variable, Function, Parameter, Loop };

struct Binding {
    std::string name;
    RRange range;   // where the name is written at its definition
    BindKind kind = BindKind::Variable;
    int value_node = -1;  // the expression assigned, or -1
    int scope = 0;
    bool used = false;      // read anywhere (writes do not count)
    bool reassigned = false;  // written more than once
};

struct Scope {
    int parent = -1;
    int fn_node = -1;  // the Function node this scope belongs to, -1 for the document
    RRange range;      // where this scope's bindings are visible
    std::map<std::string, int> vars;  // name -> index into bindings_
};

// Every name occurrence the resolver saw, in document order. Definition,
// references, hover and document highlight all read this rather than
// re-walking the tree.
struct Use {
    std::string name;
    RRange range;
    int binding = -1;  // index into bindings_, or -1 when unresolved
    int scope = 0;
    bool write = false;
    bool call = false;  // the name was in call position: `f(...)`
};

/** @brief The calls whose arguments are quoted rather than evaluated, and how many leading arguments are real. */
int NseRealArgs(const std::string &callee) {
    // -1 means "no argument is an ordinary expression".
    if (callee == "quote" || callee == "bquote" || callee == "expression" || callee == "substitute" ||
        callee == "missing" || callee == "library" || callee == "require" || callee == "requireNamespace" ||
        callee == "loadNamespace" || callee == "attachNamespace" || callee == "help" || callee == "vignette" ||
        callee == "aes" || callee == "aes_string" || callee == "vars" || callee == "alist") {
        return -1;
    }
    // The first argument is a real object; everything after it is
    // evaluated inside that object, where this server cannot follow.
    if (callee == "with" || callee == "within" || callee == "subset" || callee == "transform" ||
        callee == "attach" || callee == "curve" || callee == "mutate" || callee == "filter" ||
        callee == "summarise" || callee == "summarize" || callee == "arrange" || callee == "select" ||
        callee == "group_by" || callee == "rename" || callee == "count") {
        return 1;
    }
    return -2;  // not an NSE call at all
}

// The analysis of one document: parse, scopes, bindings, uses, and the
// diagnostics found on the way. Built once and shared by every feature,
// because they all need the same three questions answered -- what is
// bound, where, and what does this name refer to.
class Analyzer {
public:
    Analyzer(const std::vector<std::string> &lines, const RLspOptions &opts)
        : lines_(lines), opts_(opts), ast_(RLspParse(lines)) {
        Scope global;
        global.range.line = 0;
        global.range.col = 0;
        global.range.end_line = static_cast<int>(lines.empty() ? 0 : lines.size() - 1);
        global.range.end_col = lines.empty() ? 0 : static_cast<int>(lines.back().size()) + 1;
        scopes_.push_back(global);
        ScanEnvironmentOpacity();
        for (int root : ast_.roots) Collect(root, 0);
        Ctx ctx;
        for (int root : ast_.roots) Visit(root, ctx);
        ReportUnknownNames();
        ReportEqualsAssignments();
        ReportUnused();
    }

    const RParseResult &ast() const { return ast_; }
    const std::vector<Binding> &bindings() const { return bindings_; }
    const std::vector<Scope> &scopes() const { return scopes_; }
    const std::vector<Use> &uses() const { return uses_; }
    std::vector<RLspDiagnostic> &diags() { return diags_; }

    /** @brief The innermost scope whose extent contains a position. */
    int ScopeAt(int line, int col) const {
        int best = 0;  // the document's own scope always contains it
        for (size_t i = 1; i < scopes_.size(); i++) {
            if (!RRangeContains(scopes_[i].range, line, col)) continue;
            // Scopes nest, so of the ones containing the position the
            // innermost is whichever starts last.
            const RRange &chosen = scopes_[static_cast<size_t>(best)].range;
            const RRange &candidate = scopes_[i].range;
            if (candidate.line > chosen.line || (candidate.line == chosen.line && candidate.col >= chosen.col)) {
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    /** @brief Resolves a name from a scope outward, returning the binding index or -1. */
    int Lookup(const std::string &name, int scope) const {
        for (int s = scope; s >= 0; s = scopes_[static_cast<size_t>(s)].parent) {
            const auto it = scopes_[static_cast<size_t>(s)].vars.find(name);
            if (it != scopes_[static_cast<size_t>(s)].vars.end()) return it->second;
        }
        return -1;
    }

    /** @brief The use whose range contains a position, or -1. */
    int UseAt(int line, int col) const {
        for (size_t i = 0; i < uses_.size(); i++) {
            if (RRangeContains(uses_[i].range, line, col)) return static_cast<int>(i);
        }
        return -1;
    }

private:
    struct Ctx {
        int scope = 0;
        bool in_loop = false;
        bool in_function = false;
        bool has_dots = false;
        // Inside a formula or an NSE call's quoted arguments: names still
        // count as reads (so nothing is falsely reported unused), but an
        // unresolved one is never reported.
        bool soft = false;
    };

    const std::vector<std::string> &lines_;
    const RLspOptions &opts_;
    RParseResult ast_;
    std::vector<Scope> scopes_;
    std::vector<Binding> bindings_;
    std::vector<Use> uses_;
    std::vector<RLspDiagnostic> diags_;
    // Conditions already reported as assigning rather than comparing.
    std::set<int> condition_assignments_;
    // One name that resolved to nothing, held back until the whole
    // document has been walked: how many there are is itself evidence
    // about whether any of them mean anything (see ReportUnknownNames).
    struct UnknownName {
        std::string name;
        RRange range;
        bool call = false;
    };
    std::vector<UnknownName> pending_unknown_;
    // The same idea for `=` assignments (see ReportEqualsAssignments).
    std::vector<RLspDiagnostic> pending_equals_;
    int arrow_assignments_ = 0;
    // How many distinct unplaceable names it takes before the document,
    // rather than the names, is what looks wrong.
    static constexpr size_t kUnknownNameLimit = 6;
    // Set when the document attaches something the server cannot see.
    // While it is set, "this name is not defined anywhere" is not a
    // statement this server is entitled to make (see r_lsp.h).
    bool opaque_env_ = false;

    const RNode &N(int index) const { return ast_.at(index); }

    /** @brief Records one diagnostic. */
    void Diag(const RRange &range, RLspSeverity sev, const char *code, std::string message) {
        RLspDiagnostic d;
        d.range = range;
        d.severity = sev;
        d.code = code;
        d.message = std::move(message);
        diags_.push_back(std::move(d));
    }

    // --- Environment opacity -------------------------------------------

    /** @brief Walks the whole tree looking for anything that puts invisible names in scope. */
    void ScanEnvironmentOpacity() {
        for (const RNode &node : ast_.nodes) {
            if (node.kind != RNodeKind::Call || node.kids.empty()) continue;
            // `pkg::fn(...)` counts too: Rcpp::sourceCpp() defines R
            // functions from C++, and reading only bare calls missed it.
            const RNode &callee = N(node.kids[0]);
            std::string name;
            if (callee.kind == RNodeKind::Ident) {
                name = callee.text;
            } else if (callee.kind == RNodeKind::Binary && (callee.text == "::" || callee.text == ":::") &&
                       callee.kids.size() == 2) {
                name = N(callee.kids[1]).text;
            } else {
                continue;
            }
            if (name == "library" || name == "require") {
                // A base package's exports are known; anything else is not.
                const std::string pkg = node.args.empty() ? std::string() : LiteralName(node.args[0].value);
                if (pkg.empty() || !RLspIsBasePackage(pkg)) opaque_env_ = true;
                continue;
            }
            if (name == "source" || name == "sys.source" || name == "source_url" || name == "sourceCpp" ||
                name == "attach" || name == "load" || name == "load_all" || name == "list2env" ||
                name == "environment<-" || name == "eval" || name == "evalq" || name == "get" ||
                name == "get0" || name == "mget" || name == "attachNamespace" || name == "use") {
                opaque_env_ = true;
                continue;
            }
            if (name == "assign" && (node.args.empty() || LiteralName(node.args[0].value).empty())) {
                opaque_env_ = true;  // a name computed at run time
            }
        }
    }

    /** @brief The text of a string literal or bare name argument, or "" when it is neither. */
    std::string LiteralName(int node) const {
        const RNode &n = N(node);
        if (n.kind == RNodeKind::Str || n.kind == RNodeKind::Ident) return n.text;
        return std::string();
    }

    // --- Binding collection --------------------------------------------

    /** @brief Adds a binding to a scope, keeping the first definition of a name as the canonical one. */
    void Bind(const std::string &name, const RRange &range, BindKind kind, int value_node, int scope) {
        if (name.empty()) return;
        Scope &sc = scopes_[static_cast<size_t>(scope)];
        const auto it = sc.vars.find(name);
        if (it != sc.vars.end()) {
            Binding &existing = bindings_[static_cast<size_t>(it->second)];
            existing.reassigned = true;
            if (existing.kind == BindKind::Variable && kind == BindKind::Function) {
                existing.kind = kind;  // `f <- NULL` then `f <- function()` is still a function
                existing.value_node = value_node;
            }
            return;
        }
        Binding binding;
        binding.name = name;
        binding.range = range;
        binding.kind = kind;
        binding.value_node = value_node;
        binding.scope = scope;
        sc.vars[name] = static_cast<int>(bindings_.size());
        bindings_.push_back(std::move(binding));
    }

    /** @brief Binds an assignment's target, when that target is a plain name. */
    void BindTarget(int target, int value, int scope) {
        const RNode &t = N(target);
        if (t.kind != RNodeKind::Ident) return;  // `names(x) <- v` modifies x, it does not bind a new name
        const BindKind kind = N(value).kind == RNodeKind::Function ? BindKind::Function : BindKind::Variable;
        Bind(t.text, t.range, kind, value, scope);
    }

    /** @brief Collects every binding a subtree makes in one scope, without descending into nested functions. */
    void Collect(int index, int scope) {
        if (index < 0) return;
        const RNode &node = N(index);
        switch (node.kind) {
            case RNodeKind::Function: return;  // its body binds into its own scope
            case RNodeKind::Binary: {
                if (node.kids.size() == 2) {
                    if (node.text == "<-" || node.text == "=" || node.text == ":=") {
                        BindTarget(node.kids[0], node.kids[1], scope);
                    } else if (node.text == "->" || node.text == "->>") {
                        BindTarget(node.kids[1], node.kids[0], scope);
                    } else if (node.text == "<<-") {
                        // Assigns to an enclosing binding if there is one,
                        // and creates a global otherwise.
                        const RNode &target = N(node.kids[0]);
                        if (target.kind == RNodeKind::Ident && Lookup(target.text, scope) < 0) {
                            BindTarget(node.kids[0], node.kids[1], 0);
                        }
                    }
                }
                break;
            }
            case RNodeKind::For: {
                if (!node.kids.empty()) {
                    const RNode &var = N(node.kids[0]);
                    if (var.kind == RNodeKind::Ident) Bind(var.text, var.range, BindKind::Loop, -1, scope);
                }
                break;
            }
            case RNodeKind::Call: {
                if (!node.kids.empty() && N(node.kids[0]).kind == RNodeKind::Ident &&
                    N(node.kids[0]).text == "assign" && !node.args.empty()) {
                    const RNode &name_node = N(node.args[0].value);
                    if (name_node.kind == RNodeKind::Str) {
                        Bind(name_node.text, name_node.range, BindKind::Variable,
                             node.args.size() > 1 ? node.args[1].value : -1, scope);
                    }
                }
                break;
            }
            default: break;
        }
        for (int kid : node.kids) Collect(kid, scope);
        for (const RArg &arg : node.args) Collect(arg.value, scope);
    }

    // --- Use resolution and the checks that ride along ------------------

    /** @brief Records one name occurrence, resolving it and reporting an unknown one. */
    void RecordUse(const RNode &node, const Ctx &ctx, bool write, bool call) {
        Use use;
        use.name = node.text;
        use.range = node.range;
        use.scope = ctx.scope;
        use.write = write;
        use.call = call;
        use.binding = Lookup(node.text, ctx.scope);
        if (use.binding >= 0) {
            if (!write) bindings_[static_cast<size_t>(use.binding)].used = true;
        } else if (!write && !ctx.soft) {
            // T and F are ordinary variables that happen to start out
            // holding TRUE and FALSE -- until something rebinds them, at
            // which point every use of them silently changes meaning.
            if (node.text == "T" || node.text == "F") {
                Diag(node.range, RLspSeverity::Warning, "true-false-abbrev",
                     "'" + node.text + "' is a rebindable variable, not a constant. Write '" +
                         (node.text == "T" ? "TRUE" : "FALSE") + "'.");
            } else {
                ReportUnknownName(node, call);
            }
        }
        uses_.push_back(std::move(use));
    }

    /** @brief Reports a name that resolves to nothing the server knows about, where that is safe to say. */
    void ReportUnknownName(const RNode &node, bool call) {
        if (opaque_env_) return;      // something invisible is attached: say nothing
        if (!ast_.ok()) return;       // a broken parse loses bindings, not just syntax
        if (node.text.empty()) return;
        // The whole base distribution, not just the documented subset:
        // diff() has no entry in the curated table and is still real.
        if (RLspIsBaseName(node.text)) return;
        if (RLspFindFunction(node.text) != nullptr) return;
        for (const RLspVocabEntry &entry : RLspConstantVocab()) {
            if (node.text == entry.name) return;
        }
        // Dot-names are how R hides things (.Machine, .GlobalEnv, and
        // every package's own internals); too many of them are real for
        // an unknown one to be worth reporting.
        if (node.text[0] == '.') return;
        // The message (and its "did you mean", which walks the whole
        // base vocabulary) is built later, in ReportUnknownNames: most
        // of these are about to be discarded, and a file with hundreds
        // of them should not pay for a suggestion per name.
        UnknownName unknown;
        unknown.name = node.text;
        unknown.range = node.range;
        unknown.call = call;
        pending_unknown_.push_back(std::move(unknown));
    }

    /**
     * @brief Decides whether the unknown names found are typos worth reporting, or evidence of an unseen environment.
     *
     * A file with one or two names this server cannot place has probably
     * misspelled them. A file with a dozen is a file that runs somewhere
     * this server cannot see -- a shiny `ui.R` evaluated inside the app's
     * own environment, a tinytest file, a knitr chunk, a script sourced
     * into a prepared workspace -- and reporting every line of it helps
     * nobody. The count is the only signal available without evaluating
     * anything, so it is the one used; erring towards silence is the
     * documented direction (see r_lsp.h).
     */
    void ReportUnknownNames() {
        std::set<std::string> distinct;
        for (const UnknownName &unknown : pending_unknown_) distinct.insert(unknown.name);
        if (distinct.size() >= kUnknownNameLimit) {
            opaque_env_ = true;  // and so `unused-variable` says nothing either
            pending_unknown_.clear();
            return;
        }
        for (const UnknownName &unknown : pending_unknown_) {
            std::string message = unknown.call
                                      ? "No function named '" + unknown.name + "' is defined in this file or in base R."
                                      : "'" + unknown.name + "' is not defined in this file or in base R.";
            const std::string suggestion = Suggest(unknown.name);
            if (!suggestion.empty()) message += " Did you mean '" + suggestion + "'?";
            Diag(unknown.range, RLspSeverity::Warning,
                 unknown.call ? "undefined-function" : "undefined-variable", std::move(message));
        }
    }

    /** @brief The closest known name to a misspelling, or "" when nothing is close enough. */
    std::string Suggest(const std::string &name) const {
        // Nothing shorter than three characters: at that length half the
        // vocabulary is one edit away, and "did you mean 'f'?" for `df`
        // is noise rather than help.
        if (name.size() < 3) return std::string();
        const int limit = name.size() <= 4 ? 1 : 2;
        std::string best;
        int best_distance = limit + 1;
        const auto consider = [&](const std::string &candidate) {
            if (candidate.size() < 3) return;
            const int distance = EditDistance(name, candidate, limit);
            if (distance > 0 && distance < best_distance) {
                best_distance = distance;
                best = candidate;
            }
        };
        for (const Binding &binding : bindings_) consider(binding.name);
        for (const std::string &candidate : RLspBaseNameList()) consider(candidate);
        return best_distance <= limit ? best : std::string();
    }

    /** @brief Visits a subtree, resolving names and running every check that needs context. */
    void Visit(int index, const Ctx &ctx) {
        if (index < 0) return;
        const RNode &node = N(index);
        switch (node.kind) {
            case RNodeKind::Ident: RecordUse(node, ctx, false, false); return;
            case RNodeKind::Dots:
                if (ctx.in_function && !ctx.has_dots && node.text == "...") {
                    Diag(node.range, RLspSeverity::Warning, "dots-outside-function",
                         "'...' is used here but this function does not have a '...' formal argument.");
                } else if (!ctx.in_function && node.text == "...") {
                    Diag(node.range, RLspSeverity::Warning, "dots-outside-function",
                         "'...' only means something inside a function.");
                }
                return;
            case RNodeKind::Break:
            case RNodeKind::Next:
                if (!ctx.in_loop) {
                    Diag(node.range, RLspSeverity::Error,
                         node.kind == RNodeKind::Break ? "break-outside-loop" : "next-outside-loop",
                         "'" + node.text + "' is only allowed inside a loop.");
                }
                return;
            case RNodeKind::Function: VisitFunction(index, ctx); return;
            case RNodeKind::Binary: VisitBinary(index, ctx); return;
            case RNodeKind::Call: VisitCall(index, ctx); return;
            case RNodeKind::If: VisitIf(index, ctx); return;
            case RNodeKind::For: {
                Ctx inner = ctx;
                inner.in_loop = true;
                if (node.kids.size() > 1) Visit(node.kids[1], ctx);
                if (node.kids.size() > 2) Visit(node.kids[2], inner);
                // The loop variable is bound by the `for` itself; record
                // the write so references and highlight find it.
                if (!node.kids.empty() && N(node.kids[0]).kind == RNodeKind::Ident) {
                    RecordUse(N(node.kids[0]), ctx, true, false);
                }
                return;
            }
            case RNodeKind::While: {
                Ctx inner = ctx;
                inner.in_loop = true;
                if (!node.kids.empty()) {
                    CheckCondition(node.kids[0], "while");
                    Visit(node.kids[0], ctx);
                }
                if (node.kids.size() > 1) Visit(node.kids[1], inner);
                return;
            }
            case RNodeKind::Repeat: {
                Ctx inner = ctx;
                inner.in_loop = true;
                for (int kid : node.kids) Visit(kid, inner);
                return;
            }
            case RNodeKind::Block: VisitBlock(index, ctx); return;
            default: break;
        }
        for (int kid : node.kids) Visit(kid, ctx);
        for (const RArg &arg : node.args) Visit(arg.value, ctx);
    }

    /** @brief Visits a function literal: opens a scope, binds the formals, then walks the body. */
    void VisitFunction(int index, const Ctx &ctx) {
        const RNode &node = N(index);
        Scope scope;
        scope.parent = ctx.scope;
        scope.fn_node = index;
        scope.range = node.range;
        const int scope_index = static_cast<int>(scopes_.size());
        scopes_.push_back(scope);

        Ctx inner = ctx;
        inner.scope = scope_index;
        inner.in_function = true;
        inner.in_loop = false;
        inner.has_dots = false;

        std::set<std::string> seen;
        for (const RArg &formal : node.args) {
            if (formal.name.empty()) continue;
            if (formal.name == "...") inner.has_dots = true;
            if (!seen.insert(formal.name).second) {
                Diag(formal.name_range, RLspSeverity::Error, "duplicate-parameter",
                     "'" + formal.name + "' is declared twice in this function's arguments.");
                continue;
            }
            Bind(formal.name, formal.name_range, BindKind::Parameter, formal.value, scope_index);
        }
        // Collect the body's own bindings before walking it, so a
        // function may call another defined further down.
        for (int kid : node.kids) Collect(kid, scope_index);
        // Defaults are evaluated inside the new scope, where they can
        // refer to the other formals.
        for (const RArg &formal : node.args) Visit(formal.value, inner);
        for (int kid : node.kids) Visit(kid, inner);
    }

    /** @brief Visits a binary expression, including every form of assignment. */
    void VisitBinary(int index, const Ctx &ctx) {  // NOLINT(misc-no-recursion)
        const RNode &node = N(index);
        if (node.kids.size() < 2) {
            for (int kid : node.kids) Visit(kid, ctx);
            return;
        }
        const int lhs = node.kids[0];
        const int rhs = node.kids[1];
        const std::string &op = node.text;

        if (op == "$" || op == "@") {
            Visit(lhs, ctx);  // the right side is a field name, not a variable
            return;
        }
        if (op == "::" || op == ":::") {
            CheckNamespace(index);
            return;  // neither side is a variable use
        }
        if (op == "~") {
            // A formula's sides are quoted. Names in them still count as
            // reads, so nothing is falsely reported unused.
            Ctx soft = ctx;
            soft.soft = true;
            Visit(lhs, soft);
            Visit(rhs, soft);
            return;
        }
        if (op == "<-" || op == "=" || op == "<<-" || op == ":=" || op == "->" || op == "->>") {
            const bool rightward = op == "->" || op == "->>";
            const int target = rightward ? rhs : lhs;
            const int value = rightward ? lhs : rhs;
            if (op == "<-" || op == "<<-") arrow_assignments_++;
            // A condition that assigns has already been reported as
            // exactly that; the style hint on top of it would be noise.
            if (op == "=" && condition_assignments_.count(index) == 0) {
                RLspDiagnostic diag;
                diag.range = node.range.line == N(target).range.line ? N(target).range : node.range;
                diag.severity = RLspSeverity::Hint;
                diag.code = "equals-assignment";
                diag.message = "'=' assigns here. R's ordinary assignment operator is '<-'.";
                pending_equals_.push_back(std::move(diag));
            }
            Visit(value, ctx);
            const RNode &target_node = N(target);
            if (target_node.kind == RNodeKind::Ident) {
                RecordUse(target_node, ctx, true, false);
                CheckShadowedBase(target_node, value);
            } else if (target_node.kind == RNodeKind::Str) {
                // `"x" <- 1` is legal and binds the name in the string.
            } else {
                // `names(x) <- v`, `x[i] <- v`: the object is read and
                // written both, and every subscript is an ordinary use.
                Visit(target, ctx);
            }
            return;
        }
        if (op == "==" || op == "!=") CheckSpecialComparison(index);
        if (op == ":") CheckSequence(index);
        Visit(lhs, ctx);
        Visit(rhs, ctx);
    }

    /** @brief Checks `pkg::name` against the base packages, whose exports the server does know. */
    void CheckNamespace(int index) {
        const RNode &node = N(index);
        if (node.kids.size() < 2) return;
        const RNode &pkg = N(node.kids[0]);
        const RNode &name = N(node.kids[1]);
        if (pkg.kind != RNodeKind::Ident || name.kind != RNodeKind::Ident) return;
        if (!RLspIsBasePackage(pkg.text)) return;  // any other package's exports are invisible here
        const RLspFunctionEntry *entry = RLspFindFunction(name.text);
        if (entry != nullptr && std::string(entry->package) != pkg.text) {
            Diag(name.range, RLspSeverity::Warning, "wrong-package",
                 "'" + name.text + "' is exported by " + entry->package + ", not by " + pkg.text + ".");
        }
    }

    /** @brief Reports `x == NA` and `x == NULL`, which are never the test the author meant. */
    void CheckSpecialComparison(int index) {
        const RNode &node = N(index);
        for (int side : node.kids) {
            const RNode &operand = N(side);
            if (operand.kind == RNodeKind::Na) {
                Diag(node.range, RLspSeverity::Warning, "na-comparison",
                     "Comparing with NA always gives NA. Use is.na(x) instead.");
                return;
            }
            if (operand.kind == RNodeKind::Null) {
                Diag(node.range, RLspSeverity::Warning, "null-comparison",
                     "Comparing with NULL gives a zero-length result. Use is.null(x) instead.");
                return;
            }
        }
    }

    /** @brief Reports `1:length(x)`, which counts down to 1 when x is empty. */
    void CheckSequence(int index) {
        const RNode &node = N(index);
        if (node.kids.size() < 2) return;
        const RNode &from = N(node.kids[0]);
        const RNode &to = N(node.kids[1]);
        if (from.kind != RNodeKind::Num || from.text != "1") return;
        if (to.kind != RNodeKind::Call || to.kids.empty()) return;
        const RNode &callee = N(to.kids[0]);
        if (callee.kind != RNodeKind::Ident) return;
        std::string replacement;
        if (callee.text == "length") {
            replacement = "seq_along(" + FirstLineOf(lines_, to.args.empty() ? to.range : N(to.args[0].value).range) + ")";
        } else if (callee.text == "nrow" || callee.text == "ncol" || callee.text == "NROW" || callee.text == "NCOL") {
            replacement = "seq_len(" + FirstLineOf(lines_, to.range) + ")";
        } else {
            return;
        }
        Diag(node.range, RLspSeverity::Warning, "seq-length",
             "1:" + FirstLineOf(lines_, to.range) + " counts down to 1 when it is zero. Use " + replacement + ".");
    }

    /** @brief Reports an assignment over a base function people rely on being itself. */
    void CheckShadowedBase(const RNode &target, int value) {
        // Only names whose shadowing actually breaks other code. `df`
        // and `data` are base functions too, but they are also what
        // every second script calls its data frame, and reporting those
        // was pure noise on a real corpus.
        static const char *const kDangerous[] = {"c", "t", "T", "F", "pi"};
        for (const char *name : kDangerous) {
            if (target.text != name) continue;
            // Redefining one as a function is a deliberate act; binding a
            // value to it is usually the accident.
            if (N(value).kind == RNodeKind::Function) return;
            Diag(target.range, RLspSeverity::Hint, "shadow-base",
                 "'" + target.text + "' is also a base R name; assigning to it shadows " +
                     (target.text == "T" || target.text == "F" ? std::string("the logical constant")
                                                               : std::string("base::") + target.text) +
                     " for the rest of this scope.");
            return;
        }
    }

    /** @brief Visits `if`, checking its condition and both branches. */
    void VisitIf(int index, const Ctx &ctx) {
        const RNode &node = N(index);
        if (!node.kids.empty()) {
            CheckCondition(node.kids[0], "if");
            Visit(node.kids[0], ctx);
        }
        for (size_t i = 1; i < node.kids.size(); i++) Visit(node.kids[i], ctx);
    }

    /** @brief Reports the two conditions that are nearly always mistakes: an assignment, and vector logic. */
    void CheckCondition(int index, const char *keyword) {
        const RNode &node = N(index);
        if (node.kind != RNodeKind::Binary) return;
        if (node.text == "=") {
            condition_assignments_.insert(index);
            Diag(node.range, RLspSeverity::Error, "assignment-in-condition",
                 std::string("'=' assigns, it does not compare. Write '==' to test equality in a '") + keyword +
                     "' condition.");
            return;
        }
        if (node.text == "<-" || node.text == "<<-") {
            Diag(node.range, RLspSeverity::Warning, "assignment-in-condition",
                 std::string("This '") + keyword + "' condition assigns rather than compares. Write '==' if a test was meant.");
            return;
        }
        if (node.text == "&" || node.text == "|") {
            Diag(node.range, RLspSeverity::Warning, "vector-logic-in-condition",
                 "'" + node.text + "' works elementwise over whole vectors. In an '" + keyword +
                     "' condition write '" + node.text + node.text + "'.");
        }
    }

    /** @brief Visits a block, reporting code that can never run. */
    void VisitBlock(int index, const Ctx &ctx) {
        const RNode &node = N(index);
        bool stopped = false;
        RRange stopper;
        for (size_t i = 0; i < node.kids.size(); i++) {
            const int stmt = node.kids[i];
            if (stopped) {
                Diag(N(stmt).range, RLspSeverity::Hint, "unreachable-code",
                     "This never runs: the statement at line " + std::to_string(stopper.line + 1) +
                         " always leaves the function.");
                stopped = false;  // one report per block is enough to make the point
            }
            Visit(stmt, ctx);
            const RNode &s = N(stmt);
            if (s.kind == RNodeKind::Call && !s.kids.empty() && N(s.kids[0]).kind == RNodeKind::Ident) {
                const std::string &callee = N(s.kids[0]).text;
                if (callee == "return" || callee == "stop") {
                    stopped = i + 1 < node.kids.size();
                    stopper = s.range;
                }
            }
        }
    }

    /** @brief Visits a call: its callee, its arguments, and every check that depends on which function it is. */
    void VisitCall(int index, const Ctx &ctx) {
        const RNode &node = N(index);
        if (node.kids.empty()) return;
        const RNode &callee = N(node.kids[0]);
        std::string name;
        if (callee.kind == RNodeKind::Ident) {
            name = callee.text;
            RecordUse(callee, ctx, false, true);
        } else if (callee.kind == RNodeKind::Binary && (callee.text == "::" || callee.text == ":::") &&
                   callee.kids.size() == 2) {
            name = N(callee.kids[1]).text;
            Visit(node.kids[0], ctx);
        } else {
            Visit(node.kids[0], ctx);
        }

        CheckCallArguments(index, name);
        CheckKnownCall(index, name, ctx);

        const int real_args = name.empty() ? -2 : NseRealArgs(name);
        for (size_t i = 0; i < node.args.size(); i++) {
            Ctx arg_ctx = ctx;
            if (real_args == -1 || (real_args >= 0 && static_cast<int>(i) >= real_args)) arg_ctx.soft = true;
            Visit(node.args[i].value, arg_ctx);
        }
    }

    /** @brief Checks a call's argument list against itself and, for a known function, against its formals. */
    void CheckCallArguments(int index, const std::string &name) {
        const RNode &node = N(index);
        std::set<std::string> seen;
        size_t positional = 0;
        for (size_t i = 0; i < node.args.size(); i++) {
            const RArg &arg = node.args[i];
            if (arg.name.empty()) {
                positional++;
                // Only a *trailing* empty slot, which is a comma someone
                // forgot to delete. An empty slot in the middle --
                // `cv.tree(fit, , prune.misclass)` -- is the deliberate
                // way to skip an argument and take its default.
                if (arg.value < 0 && !name.empty() && node.args.size() > 1 && i + 1 == node.args.size()) {
                    Diag(node.range, RLspSeverity::Warning, "empty-argument",
                         "The argument list of " + name + "() ends with a stray comma.");
                }
                continue;
            }
            if (!seen.insert(arg.name).second) {
                Diag(arg.name_range, RLspSeverity::Error, "duplicate-argument",
                     "'" + arg.name + "' is passed twice to " + (name.empty() ? std::string("this call") : name + "()") +
                         ".");
            }
        }
        if (name.empty()) return;
        const RLspFunctionEntry *entry = RLspFindFunction(name);
        if (entry == nullptr) return;
        const std::vector<std::string> formals = RLspSplitParams(entry->params);
        std::vector<std::string> formal_names;
        bool has_dots = false;
        for (const std::string &formal : formals) {
            const std::string formal_name = Trim(formal.substr(0, formal.find('=')));
            if (formal_name == "...") {
                has_dots = true;
                continue;
            }
            formal_names.push_back(formal_name);
        }
        // `...` swallows anything, and partial matching means a shortened
        // name is legal too -- so an unknown name is only really unknown
        // when neither applies.
        if (!has_dots) {
            for (const RArg &arg : node.args) {
                if (arg.name.empty()) continue;
                bool matched = false;
                for (const std::string &formal : formal_names) {
                    if (formal == arg.name || StartsWith(formal, arg.name)) {
                        matched = true;
                        break;
                    }
                }
                if (matched) continue;
                std::string message = "'" + arg.name + "' is not an argument of " + name + "().";
                int best_distance = 3;
                std::string best;
                for (const std::string &formal : formal_names) {
                    const int distance = EditDistance(arg.name, formal, 2);
                    if (distance < best_distance) {
                        best_distance = distance;
                        best = formal;
                    }
                }
                if (!best.empty()) message += " Did you mean '" + best + "'?";
                message += " It takes " + JoinNames(formal_names) + ".";
                Diag(arg.name_range, RLspSeverity::Warning, "unknown-argument", std::move(message));
            }
            if (positional > formal_names.size()) {
                Diag(node.range, RLspSeverity::Warning, "too-many-arguments",
                     name + "() takes " + std::to_string(formal_names.size()) + " argument" +
                         (formal_names.size() == 1 ? "" : "s") + " but is given " + std::to_string(positional) +
                         " here.");
            }
        }
    }

    /** @brief Renders a formal-name list as "'a', 'b' and 'c'". */
    static std::string JoinNames(const std::vector<std::string> &names) {
        if (names.empty()) return "no arguments";
        std::string out;
        for (size_t i = 0; i < names.size(); i++) {
            if (i > 0) out += i + 1 == names.size() ? " and " : ", ";
            out += "'" + names[i] + "'";
        }
        return out;
    }

    /** @brief Runs the checks that depend on which base function is being called. */
    void CheckKnownCall(int index, const std::string &name, const Ctx &ctx) {
        const RNode &node = N(index);
        if (name == "library" || name == "require") {
            if (ctx.in_function) {
                Diag(node.range, RLspSeverity::Warning, "library-in-function",
                     name + "() inside a function changes the search path for the whole session. Use pkg::fun() instead.");
            }
            return;
        }
        if (name == "attach") {
            Diag(node.range, RLspSeverity::Warning, "attach-call",
                 "attach() puts a data frame on the search path, where later code silently picks up its columns. "
                 "Use with() or a plain $ instead.");
            return;
        }
        if (name == "return" && !ctx.in_function) {
            Diag(node.range, RLspSeverity::Warning, "return-outside-function",
                 "return() only means something inside a function.");
            return;
        }
        if (name == "source" && !node.args.empty()) CheckSourcePath(node.args[0].value);
        if (name == "setwd") {
            Diag(node.range, RLspSeverity::Hint, "setwd-call",
                 "setwd() makes a script depend on where it was run from. Prefer paths relative to the project.");
        }
        if (name == "sapply") {
            // Not reported as a problem -- it is idiomatic -- but the
            // type-unstable result is worth a nudge where it matters.
        }
    }

    /** @brief Reports a `source()` of a file that is not there. */
    void CheckSourcePath(int arg) {
        if (!opts_.check_files || opts_.doc_dir.empty()) return;
        const RNode &node = N(arg);
        if (node.kind != RNodeKind::Str || node.text.empty()) return;
        std::filesystem::path path(node.text);
        if (path.is_relative()) path = std::filesystem::path(opts_.doc_dir) / path;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) return;
        Diag(node.range, RLspSeverity::Warning, "missing-file",
             "source() names a file that is not there: " + node.text);
    }

    // --- Post-pass ------------------------------------------------------

    /**
     * @brief Decides whether the `=` assignments in this document are slips or the author's own style.
     *
     * `x = 1` is legal R that a good many people write on purpose. A
     * document where `=` is the *only* assignment operator is one of
     * theirs, and hinting at every line of it is exactly the behaviour
     * that gets a linter switched off; a document that uses `<-`
     * throughout and then `=` twice has two slips worth pointing at. So
     * the hint survives only where the arrows outnumber the equals.
     */
    void ReportEqualsAssignments() {
        if (pending_equals_.size() >= static_cast<size_t>(arrow_assignments_)) return;
        for (RLspDiagnostic &diag : pending_equals_) diags_.push_back(std::move(diag));
    }

    /** @brief Reports locals that are assigned inside a function and never read. */
    void ReportUnused() {
        if (opaque_env_ || !ast_.ok()) return;
        for (const Binding &binding : bindings_) {
            if (binding.used) continue;
            if (binding.scope == 0) continue;  // a top-level value is a script's output, not a mistake
            if (binding.kind != BindKind::Variable) continue;  // parameters and loop variables are declarations
            if (binding.name.empty() || binding.name[0] == '.') continue;
            Diag(binding.range, RLspSeverity::Hint, "unused-variable",
                 "'" + binding.name + "' is assigned here and never used.");
        }
    }
};

}  // namespace

std::vector<std::string> RLspSplitParams(const std::string &params) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    bool in_string = false;
    char quote = '\0';
    for (size_t i = 0; i < params.size(); i++) {
        const char c = params[i];
        if (in_string) {
            cur += c;
            if (c == '\\' && i + 1 < params.size()) {
                cur += params[i + 1];
                i++;
            } else if (c == quote) {
                in_string = false;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            in_string = true;
            quote = c;
            cur += c;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') depth++;
        if (c == ')' || c == ']' || c == '}') depth--;
        if (c == ',' && depth == 0) {
            const std::string trimmed = Trim(cur);
            if (!trimmed.empty()) out.push_back(trimmed);
            cur.clear();
            continue;
        }
        cur += c;
    }
    const std::string trimmed = Trim(cur);
    if (!trimmed.empty()) out.push_back(trimmed);
    return out;
}

// --- Diagnostics ------------------------------------------------------

namespace {

/** @brief Maps a parse error's stable code to the severity it is reported at. */
RLspSeverity SeverityOfParseError(const std::string &code) {
    // Every parse error is an error: R would refuse to source the file.
    (void)code;
    return RLspSeverity::Error;
}

/** @brief Reports whether the rest of a line after a column is blank or a comment. */
bool RestOfLineIsBlank(const std::string &line, size_t from) {
    for (size_t i = from; i < line.size(); i++) {
        if (line[i] == '#') return true;
        if (std::isspace(static_cast<unsigned char>(line[i])) == 0) return false;
    }
    return true;
}

/**
 * @brief Reports a `;` that ends a line, which in R separates nothing.
 *
 * Held to the same standard as the `=` hint above: a file that ends
 * every other line with a semicolon was written that way on purpose (or
 * generated), and a hint per line helps nobody. One on a line here and
 * there is a habit from another language, and worth a quiet word.
 */
void CheckTrailingSemicolons(const std::vector<std::string> &lines, std::vector<RLspDiagnostic> *out) {
    const std::vector<RToken> toks = RLspTokenize(lines, nullptr);
    std::vector<RLspDiagnostic> found;
    for (const RToken &tok : toks) {
        if (tok.kind != RTokKind::Semi) continue;
        const size_t at = static_cast<size_t>(tok.range.line);
        if (at >= lines.size()) continue;
        if (!RestOfLineIsBlank(lines[at], static_cast<size_t>(tok.range.end_col))) continue;
        RLspDiagnostic d;
        d.range = tok.range;
        d.severity = RLspSeverity::Hint;
        d.code = "trailing-semicolon";
        d.message = "R does not need a ';' at the end of a line.";
        found.push_back(std::move(d));
    }
    size_t code_lines = 0;
    for (const std::string &line : lines) {
        const std::string trimmed = Trim(line);
        if (!trimmed.empty() && trimmed[0] != '#') code_lines++;
    }
    if (found.size() * 4 > code_lines) return;  // a quarter of the file: deliberate
    for (RLspDiagnostic &d : found) out->push_back(std::move(d));
}

// One roxygen2 comment block, and the definition it documents.
struct RoxygenBlock {
    int first_line = 0;
    int last_line = 0;
    std::vector<RComment> comments;
};

/** @brief Groups consecutive `#'` comment lines into blocks. */
std::vector<RoxygenBlock> RoxygenBlocks(const std::vector<RComment> &comments) {
    std::vector<RoxygenBlock> blocks;
    for (const RComment &comment : comments) {
        if (!comment.roxygen) continue;
        if (!blocks.empty() && blocks.back().last_line + 1 == comment.range.line) {
            blocks.back().last_line = comment.range.line;
            blocks.back().comments.push_back(comment);
            continue;
        }
        RoxygenBlock block;
        block.first_line = comment.range.line;
        block.last_line = comment.range.line;
        block.comments.push_back(comment);
        blocks.push_back(std::move(block));
    }
    return blocks;
}

/** @brief The `@tag` a roxygen line opens, with the column it starts at; empty when the line carries no tag. */
std::string RoxygenTagOf(const std::string &text, size_t *col_out) {
    size_t i = 0;
    while (i < text.size() && (text[i] == '#' || text[i] == '\'')) i++;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) i++;
    if (i >= text.size() || text[i] != '@') return std::string();
    const size_t at = i;
    i++;
    std::string tag;
    while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) != 0 || text[i] == '.')) {
        tag += text[i];
        i++;
    }
    if (col_out != nullptr) *col_out = at;
    return tag;
}

/**
 * @brief The argument names a `@param` line documents.
 * @param text the whole comment line, including its `#'`
 * @return one entry per name -- roxygen2 lets one line document several, as `@param x,y` or `@param x, y`
 */
std::vector<std::string> RoxygenParamNames(const std::string &text) {
    size_t col = 0;
    const std::string tag = RoxygenTagOf(text, &col);
    if (tag.empty()) return {};
    const std::string rest = Trim(text.substr(std::min(col + 1 + tag.size(), text.size())));
    std::vector<std::string> names;
    std::string cur;
    for (size_t i = 0; i <= rest.size(); i++) {
        const char c = i < rest.size() ? rest[i] : ' ';
        if (c == ',') {
            if (!cur.empty()) names.push_back(cur);
            cur.clear();
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (cur.empty()) continue;  // "x, y": the space after the comma is not the end of the list
            names.push_back(cur);
            cur.clear();
            // A name list ends at the first space that does not follow a
            // comma; everything after it is the description.
            const size_t next = rest.find_first_not_of(" \t", i);
            if (next == std::string::npos || rest[next] != ',') break;
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) names.push_back(cur);
    return names;
}

/** @brief Reports whether a roxygen tag is one roxygen2 defines. */
bool IsKnownRoxygenTag(const std::string &tag) {
    for (const RLspVocabEntry &entry : RLspRoxygenVocab()) {
        if (tag == entry.name) return true;
    }
    return false;
}

/** @brief Finds the formals of the function a roxygen block documents, and the name it is assigned to. */
bool DocumentedFunction(const RParseResult &ast, int after_line, std::string *name_out,
                        std::vector<std::string> *formals_out) {
    for (int root : ast.roots) {
        const RNode &node = ast.at(root);
        if (node.range.line != after_line) continue;
        if (node.kind != RNodeKind::Binary) return false;
        if (node.text != "<-" && node.text != "=" && node.text != ":=") return false;
        if (node.kids.size() < 2) return false;
        const RNode &target = ast.at(node.kids[0]);
        const RNode &value = ast.at(node.kids[1]);
        if (value.kind != RNodeKind::Function) return false;
        *name_out = target.text;
        for (const RArg &formal : value.args) formals_out->push_back(formal.name);
        return true;
    }
    return false;
}

/** @brief Checks each roxygen block against the function it documents. */
void CheckRoxygen(const RParseResult &ast, std::vector<RLspDiagnostic> *out) {
    for (const RoxygenBlock &block : RoxygenBlocks(ast.comments)) {
        std::string fn_name;
        std::vector<std::string> formals;
        const bool documents_function = DocumentedFunction(ast, block.last_line + 1, &fn_name, &formals);
        std::set<std::string> documented;
        for (const RComment &comment : block.comments) {
            size_t col = 0;
            const std::string tag = RoxygenTagOf(comment.text, &col);
            if (tag.empty()) continue;
            RRange tag_range = comment.range;
            tag_range.col = static_cast<int>(col);
            tag_range.end_col = static_cast<int>(col + tag.size() + 1);
            if (!IsKnownRoxygenTag(tag)) {
                RLspDiagnostic d;
                d.range = tag_range;
                d.severity = RLspSeverity::Hint;
                d.code = "roxygen-unknown-tag";
                d.message = "roxygen2 does not define a '@" + tag + "' tag.";
                out->push_back(std::move(d));
                continue;
            }
            if (tag != "param" || !documents_function) continue;
            for (const std::string &word : RoxygenParamNames(comment.text)) {
                const bool known = std::find(formals.begin(), formals.end(), word) != formals.end();
                if (!known) {
                    RLspDiagnostic d;
                    d.range = comment.range;
                    d.severity = RLspSeverity::Warning;
                    d.code = "roxygen-unknown-param";
                    d.message = "@param documents '" + word + "', which is not an argument of " + fn_name + "().";
                    out->push_back(std::move(d));
                    continue;
                }
                if (!documented.insert(word).second) {
                    RLspDiagnostic d;
                    d.range = comment.range;
                    d.severity = RLspSeverity::Warning;
                    d.code = "roxygen-duplicate-param";
                    d.message = "'" + word + "' is documented more than once.";
                    out->push_back(std::move(d));
                }
            }
        }
    }
}

}  // namespace

std::vector<RLspDiagnostic> RLspDiagnostics(const std::vector<std::string> &lines, const RLspOptions &opts) {
    Analyzer analyzer(lines, opts);
    std::vector<RLspDiagnostic> out;
    for (const RParseError &error : analyzer.ast().errors) {
        RLspDiagnostic d;
        d.range = error.range;
        d.severity = SeverityOfParseError(error.code);
        d.code = error.code;
        d.message = error.message;
        out.push_back(std::move(d));
    }
    for (RLspDiagnostic &d : analyzer.diags()) out.push_back(std::move(d));
    CheckTrailingSemicolons(lines, &out);
    CheckRoxygen(analyzer.ast(), &out);
    std::stable_sort(out.begin(), out.end(), [](const RLspDiagnostic &a, const RLspDiagnostic &b) {
        if (a.range.line != b.range.line) return a.range.line < b.range.line;
        return a.range.col < b.range.col;
    });
    return out;
}

// --- Cursor context ---------------------------------------------------
//
// Completion and signature help both have to answer "what is being typed,
// and inside what?" for a document that is, by definition, half-written
// and usually not parseable. Both go through the token stream rather than
// the AST for exactly that reason: the lexer never fails, so a call whose
// `)` has not been typed yet is still a call here.

namespace {

// What encloses the cursor, innermost last.
struct OpenBracket {
    enum class Kind { Call, Index, Paren, Brace } kind = Kind::Paren;
    std::string callee;        // for Kind::Call: the function being called
    std::string callee_pkg;    // the `pkg` of a `pkg::fun(` callee
    int arg_index = 0;         // which argument the cursor is in
    bool named_arg = false;    // an `=` has been typed in this argument already
    RRange open;
};

struct CursorContext {
    std::vector<OpenBracket> stack;
    std::string word;      // the R name being typed, possibly empty
    int word_start = 0;    // its start column on the cursor's line
    bool in_comment = false;
    bool roxygen = false;
    std::string roxygen_tag_prefix;  // the letters typed after an `@`, when that is where the cursor is
    bool after_at = false;
    bool in_string = false;
    std::string dollar_object;  // the object before a `$`/`@`, when the cursor follows one
    std::string namespace_pkg;  // the package before a `::`, when the cursor follows one

    /** @brief The innermost enclosing call, or null when the cursor is not in one. */
    const OpenBracket *EnclosingCall() const {
        for (size_t i = stack.size(); i-- > 0;) {
            if (stack[i].kind == OpenBracket::Kind::Call) return &stack[i];
            // A `{` or a subscript in between means the cursor is no
            // longer in the call's own argument list.
            if (stack[i].kind == OpenBracket::Kind::Brace) return nullptr;
        }
        return nullptr;
    }
};

/** @brief Reports whether a token starts at or after a position. */
bool StartsAtOrAfter(const RRange &range, int line, int col) {
    if (range.line != line) return range.line > line;
    return range.col >= col;
}

/** @brief Works out what encloses the cursor, and what name is being typed there. */
CursorContext ContextAt(const std::vector<std::string> &lines, int line, int col) {
    CursorContext ctx;
    std::vector<RComment> comments;
    const std::vector<RToken> toks = RLspTokenize(lines, &comments);

    for (const RComment &comment : comments) {
        if (comment.range.line != line) continue;
        if (col <= comment.range.col) continue;
        ctx.in_comment = true;
        ctx.roxygen = comment.roxygen;
    }

    std::string prev_name;  // the last identifier seen, i.e. a call's callee
    std::string prev_pkg;
    bool prev_was_ns = false;
    for (const RToken &tok : toks) {
        if (StartsAtOrAfter(tok.range, line, col)) break;
        // A string the cursor is inside of: offer nothing rather than
        // completing R names into the middle of a file path.
        if (tok.kind == RTokKind::Str && RRangeContains(tok.range, line, col)) ctx.in_string = true;
        switch (tok.kind) {
            case RTokKind::LParen: {
                OpenBracket open;
                open.kind = prev_name.empty() ? OpenBracket::Kind::Paren : OpenBracket::Kind::Call;
                open.callee = prev_name;
                open.callee_pkg = prev_pkg;
                open.open = tok.range;
                ctx.stack.push_back(open);
                break;
            }
            case RTokKind::LBracket:
            case RTokKind::DblLBracket: {
                OpenBracket open;
                open.kind = OpenBracket::Kind::Index;
                open.open = tok.range;
                ctx.stack.push_back(open);
                break;
            }
            case RTokKind::LBrace: {
                OpenBracket open;
                open.kind = OpenBracket::Kind::Brace;
                open.open = tok.range;
                ctx.stack.push_back(open);
                break;
            }
            case RTokKind::RParen:
            case RTokKind::RBracket:
            case RTokKind::RBrace:
                if (!ctx.stack.empty()) ctx.stack.pop_back();
                break;
            case RTokKind::Comma:
                if (!ctx.stack.empty()) {
                    ctx.stack.back().arg_index++;
                    ctx.stack.back().named_arg = false;
                }
                break;
            case RTokKind::Op:
                if (tok.text == "=" && !ctx.stack.empty()) ctx.stack.back().named_arg = true;
                break;
            default: break;
        }
        if (tok.kind == RTokKind::Ident) {
            if (prev_was_ns) {
                prev_pkg = prev_name;
            } else {
                prev_pkg.clear();
            }
            prev_name = tok.text;
        } else if (tok.kind != RTokKind::Op || (tok.text != "::" && tok.text != ":::")) {
            prev_name.clear();
            prev_pkg.clear();
        }
        prev_was_ns = tok.kind == RTokKind::Op && (tok.text == "::" || tok.text == ":::");
    }

    // The word being typed, read straight off the line: mid-typing, the
    // token stream may not agree with what the cursor is inside of.
    static const std::string kNoLine;
    const std::string &text =
        static_cast<size_t>(line) < lines.size() && line >= 0 ? lines[static_cast<size_t>(line)] : kNoLine;
    const size_t at = std::min(static_cast<size_t>(col), text.size());
    size_t start = at;
    while (start > 0 && IsNameByte(text[start - 1])) start--;
    ctx.word = text.substr(start, at - start);
    ctx.word_start = static_cast<int>(start);

    if (start > 0 && text[start - 1] == '@' && ctx.roxygen) {
        ctx.after_at = true;
        ctx.roxygen_tag_prefix = ctx.word;
    }
    // `obj$fie` / `obj@slo` / `pkg::fun`: what comes before the word says
    // which namespace to complete in.
    if (start > 0 && (text[start - 1] == '$' || (text[start - 1] == '@' && !ctx.roxygen))) {
        size_t obj_end = start - 1;
        size_t obj_start = obj_end;
        while (obj_start > 0 && IsNameByte(text[obj_start - 1])) obj_start--;
        ctx.dollar_object = text.substr(obj_start, obj_end - obj_start);
    }
    if (start >= 2 && text[start - 1] == ':' && text[start - 2] == ':') {
        size_t pkg_end = start - 2;
        if (pkg_end > 0 && text[pkg_end - 1] == ':') pkg_end--;
        size_t pkg_start = pkg_end;
        while (pkg_start > 0 && IsNameByte(text[pkg_start - 1])) pkg_start--;
        ctx.namespace_pkg = text.substr(pkg_start, pkg_end - pkg_start);
    }
    return ctx;
}

// --- Completion -------------------------------------------------------

/** @brief Reports whether a candidate matches the typed prefix, ignoring case. */
bool PrefixMatch(const std::string &candidate, const std::string &prefix) {
    if (prefix.empty()) return true;
    if (candidate.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(candidate[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

// Assembles a completion list, dropping duplicates and anything that does
// not match what has been typed.
class CompletionBuilder {
public:
    CompletionBuilder(const std::string &prefix, int replace_start, int replace_end)
        : prefix_(prefix), replace_start_(replace_start), replace_end_(replace_end) {}

    /** @brief Adds one candidate, unless its label was offered already or does not match the prefix. */
    void Add(const std::string &label, const std::string &insert, RLspKind kind, const std::string &detail,
             const std::string &doc) {
        if (!PrefixMatch(label, prefix_)) return;
        if (!seen_.insert(label).second) return;
        if (items_.size() >= kMaxItems) return;
        RLspCompletionItem item;
        item.label = label;
        item.insert_text = insert;
        item.kind = kind;
        item.detail = detail;
        item.documentation = doc;
        item.replace_start = replace_start_;
        item.replace_end = replace_end_;
        items_.push_back(std::move(item));
    }

    /** @brief The list built so far. */
    std::vector<RLspCompletionItem> Take() { return std::move(items_); }
    /** @brief How many items have been added. */
    size_t size() const { return items_.size(); }
    /**
     * @brief Sorts everything added since a mark shortest-name-first.
     *
     * Applied to the vocabulary groups only: `sub` before
     * `suppressWarnings` is what someone typing "su" is after, while the
     * document's own names are already in the order that matters (the
     * innermost scope first) and are left alone.
     */
    void SortTail(size_t from) {
        if (from >= items_.size()) return;
        std::stable_sort(items_.begin() + static_cast<std::ptrdiff_t>(from), items_.end(),
                         [](const RLspCompletionItem &a, const RLspCompletionItem &b) {
                             if (a.label.size() != b.label.size()) return a.label.size() < b.label.size();
                             return a.label < b.label;
                         });
    }
    /** @brief Whether the list is already at its cap. */
    bool full() const { return items_.size() >= kMaxItems; }

private:
    // A list this long is already past what anyone scrolls; the cap keeps
    // an empty prefix from serializing the whole vocabulary on every
    // keystroke.
    static constexpr size_t kMaxItems = 200;
    std::string prefix_;
    int replace_start_;
    int replace_end_;
    std::set<std::string> seen_;
    std::vector<RLspCompletionItem> items_;
};

/** @brief The signature of a known function, rendered for a completion's right-hand annotation. */
std::string FunctionDetail(const RLspFunctionEntry &entry) {
    return std::string(entry.name) + "(" + entry.params + ")  " + entry.package;
}

/** @brief Adds the field names an object is known to have, from its own assignment and from every `$` use of it. */
void AddKnownFields(const std::vector<std::string> &lines, const std::string &object, int line, int col,
                    CompletionBuilder *out) {
    const RParseResult ast = RLspParse(lines);
    std::vector<std::string> fields;
    for (const RNode &node : ast.nodes) {
        // `obj$field` anywhere in the document, including on the left of
        // an assignment: whatever the author wrote is what the object has.
        if (node.kind == RNodeKind::Binary && (node.text == "$" || node.text == "@") && node.kids.size() == 2) {
            const RNode &lhs = ast.at(node.kids[0]);
            const RNode &rhs = ast.at(node.kids[1]);
            // Not the half-typed name under the cursor: `d$al` must not
            // offer `al` back as a field of `d`.
            if (lhs.kind == RNodeKind::Ident && lhs.text == object && rhs.kind == RNodeKind::Ident &&
                !RRangeContains(rhs.range, line, col) && rhs.range.end_col != col) {
                fields.push_back(rhs.text);
            }
            continue;
        }
        // `obj <- list(a = 1, b = 2)` / data.frame(...) / c(a = 1): the
        // named arguments are the fields.
        if (node.kind != RNodeKind::Binary || node.kids.size() != 2) continue;
        if (node.text != "<-" && node.text != "=" && node.text != ":=") continue;
        const RNode &target = ast.at(node.kids[0]);
        if (target.kind != RNodeKind::Ident || target.text != object) continue;
        const RNode &value = ast.at(node.kids[1]);
        if (value.kind != RNodeKind::Call || value.kids.empty()) continue;
        const RNode &callee = ast.at(value.kids[0]);
        if (callee.kind != RNodeKind::Ident) continue;
        if (callee.text != "list" && callee.text != "data.frame" && callee.text != "c" &&
            callee.text != "setNames" && callee.text != "tibble") {
            continue;
        }
        for (const RArg &arg : value.args) {
            if (!arg.name.empty()) fields.push_back(arg.name);
        }
    }
    for (const std::string &field : fields) {
        out->Add(field, field, RLspKind::Field, object + "$" + field, "A field of " + object + " used in this file.");
    }
}

}  // namespace

std::vector<RLspCompletionItem> RLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                const RLspOptions &opts) {
    const CursorContext ctx = ContextAt(lines, line, col);
    CompletionBuilder out(ctx.word, ctx.word_start, col);

    // Inside a comment there is nothing to complete except a roxygen tag.
    if (ctx.in_comment) {
        if (!ctx.after_at) return {};
        for (const RLspVocabEntry &entry : RLspRoxygenVocab()) {
            out.Add(entry.name, entry.name, RLspKind::Keyword, entry.detail, entry.doc);
        }
        return out.Take();
    }
    if (ctx.in_string) return {};

    // `pkg::` -- only the base distribution's exports are knowable.
    if (!ctx.namespace_pkg.empty()) {
        for (const RLspFunctionEntry &entry : RLspFunctionVocab()) {
            if (ctx.namespace_pkg != entry.package) continue;
            out.Add(entry.name, entry.name, RLspKind::Function, FunctionDetail(entry), entry.doc);
        }
        return out.Take();
    }

    // `obj$` -- the fields this document shows the object having.
    if (!ctx.dollar_object.empty()) {
        AddKnownFields(lines, ctx.dollar_object, line, col, &out);
        return out.Take();
    }

    // Every remaining context needs the document's own bindings, so the
    // analysis is run once here rather than per branch.
    const Analyzer analyzer(lines, opts);
    const OpenBracket *call = ctx.EnclosingCall();
    if (call != nullptr) {
        // `library(` / `require(`: package names.
        if ((call->callee == "library" || call->callee == "require" || call->callee == "requireNamespace" ||
             call->callee == "loadNamespace") &&
            call->arg_index == 0) {
            for (const RLspVocabEntry &entry : RLspPackageVocab()) {
                out.Add(entry.name, entry.name, RLspKind::Module, entry.detail, entry.doc);
            }
            return out.Take();
        }
        // Argument names of the function being called, offered first:
        // that is what the cursor is most likely reaching for.
        if (!call->named_arg) {
            const RLspFunctionEntry *entry = RLspFindFunction(call->callee);
            if (entry != nullptr) {
                for (const std::string &formal : RLspSplitParams(entry->params)) {
                    const std::string name = Trim(formal.substr(0, formal.find('=')));
                    if (name == "...") continue;
                    out.Add(name, name + " = ", RLspKind::Field, formal,
                            "An argument of " + std::string(entry->name) + "().");
                }
            } else {
                // A function defined in this file: its formals are right
                // there in the source.
                const int binding = analyzer.Lookup(call->callee, analyzer.ScopeAt(line, col));
                if (binding >= 0) {
                    const RNode &value = analyzer.ast().at(analyzer.bindings()[static_cast<size_t>(binding)].value_node);
                    if (value.kind == RNodeKind::Function) {
                        for (const RArg &formal : value.args) {
                            if (formal.name.empty() || formal.name == "...") continue;
                            out.Add(formal.name, formal.name + " = ", RLspKind::Field, formal.name,
                                    "An argument of " + call->callee + "().");
                        }
                    }
                }
            }
        }
    }

    // Everything in scope at the cursor, innermost first.
    const int scope = analyzer.ScopeAt(line, col);
    for (int s = scope; s >= 0; s = analyzer.scopes()[static_cast<size_t>(s)].parent) {
        for (const auto &entry : analyzer.scopes()[static_cast<size_t>(s)].vars) {
            const Binding &binding = analyzer.bindings()[static_cast<size_t>(entry.second)];
            const bool is_function = binding.kind == BindKind::Function;
            std::string detail = "defined at line " + std::to_string(binding.range.line + 1);
            if (is_function) {
                const RNode &value = analyzer.ast().at(binding.value_node);
                std::string params;
                for (const RArg &formal : value.args) {
                    if (!params.empty()) params += ", ";
                    params += formal.name;
                }
                detail = binding.name + "(" + params + ")";
            } else if (binding.kind == BindKind::Parameter) {
                detail = "argument";
            }
            out.Add(binding.name, binding.name, is_function ? RLspKind::Function : RLspKind::Variable, detail,
                    is_function ? "Defined in this file." : "Bound in this file.");
        }
    }

    // Then R itself.
    const size_t vocabulary_start = out.size();
    for (const RLspVocabEntry &entry : RLspKeywordVocab()) {
        out.Add(entry.name, entry.name, RLspKind::Keyword, entry.detail, entry.doc);
    }
    for (const RLspVocabEntry &entry : RLspConstantVocab()) {
        // T and F are offered only when explicitly typed out: completing
        // them is the fastest way to write the bug they cause.
        if ((std::string(entry.name) == "T" || std::string(entry.name) == "F") && ctx.word != entry.name) continue;
        out.Add(entry.name, entry.name, RLspKind::Constant, entry.detail, entry.doc);
    }
    for (const RLspFunctionEntry &entry : RLspFunctionVocab()) {
        if (out.full()) break;
        out.Add(entry.name, entry.name, RLspKind::Function, FunctionDetail(entry), entry.doc);
    }
    // Then the rest of the base distribution, which has no documentation
    // here but is still what the author is reaching for.
    for (const std::string &name : RLspBaseNameList()) {
        if (out.full()) break;
        out.Add(name, name, RLspKind::Function, "base R", std::string());
    }
    out.SortTail(vocabulary_start);
    return out.Take();
}

// --- Hover ------------------------------------------------------------

namespace {

/** @brief Finds the token whose range contains a position. */
bool TokenAt(const std::vector<RToken> &toks, int line, int col, RToken *out) {
    for (const RToken &tok : toks) {
        if (tok.kind == RTokKind::Newline || tok.kind == RTokKind::End) continue;
        if (!RRangeContains(tok.range, line, col)) continue;
        *out = tok;
        return true;
    }
    return false;
}

/** @brief Renders a known function's signature. */
std::string SignatureLabel(const RLspFunctionEntry &entry) {
    return std::string(entry.name) + "(" + entry.params + ")";
}

/** @brief Renders a function defined in the document as a signature, defaults included. */
std::string LocalSignature(const RParseResult &ast, const std::vector<std::string> &lines, const std::string &name,
                           const RNode &fn) {
    std::string out = name + "(";
    for (size_t i = 0; i < fn.args.size(); i++) {
        if (i > 0) out += ", ";
        out += fn.args[i].name;
        if (fn.args[i].value >= 0) {
            out += " = " + Ellipsize(FirstLineOf(lines, ast.at(fn.args[i].value).range), 24);
        }
    }
    return out + ")";
}

/** @brief The prose of the roxygen block immediately above a line, with its tags and `#'` markers stripped. */
std::string RoxygenTitleAbove(const RParseResult &ast, int line) {
    std::string title;
    for (int at = line - 1; at >= 0; at--) {
        bool found = false;
        for (const RComment &comment : ast.comments) {
            if (comment.range.line != at || !comment.roxygen) continue;
            found = true;
            std::string text = comment.text;
            size_t i = 0;
            while (i < text.size() && (text[i] == '#' || text[i] == '\'')) i++;
            text = Trim(text.substr(i));
            if (text.empty() || text[0] == '@') break;  // tags end the free prose
            title = text + (title.empty() ? "" : "\n" + title);
        }
        if (!found) break;
    }
    return title;
}

/** @brief Looks a name up in one of the flat vocabulary tables. */
const RLspVocabEntry *FindVocab(const std::vector<RLspVocabEntry> &table, const std::string &name) {
    for (const RLspVocabEntry &entry : table) {
        if (name == entry.name) return &entry;
    }
    return nullptr;
}

/** @brief Describes a binding for a hover: what it is, where it came from, and what it was assigned. */
std::string DescribeBinding(const Analyzer &analyzer, const std::vector<std::string> &lines, const Binding &binding) {
    const RParseResult &ast = analyzer.ast();
    const int def_line = binding.range.line + 1;
    if (binding.kind == BindKind::Function) {
        const RNode &fn = ast.at(binding.value_node);
        std::string text = LocalSignature(ast, lines, binding.name, fn);
        text += "\n\nDefined in this file, line " + std::to_string(def_line) + ".";
        const std::string title = RoxygenTitleAbove(ast, binding.range.line);
        if (!title.empty()) text += "\n\n" + title;
        return text;
    }
    if (binding.kind == BindKind::Parameter) {
        std::string text = binding.name + "\n\nAn argument of the enclosing function.";
        if (binding.value_node >= 0) {
            text += "\nDefault: " + Ellipsize(FirstLineOf(lines, ast.at(binding.value_node).range), 60);
        }
        return text;
    }
    if (binding.kind == BindKind::Loop) {
        return binding.name + "\n\nThe loop variable of the 'for' at line " + std::to_string(def_line) + ".";
    }
    std::string text = binding.name + "\n\nAssigned at line " + std::to_string(def_line);
    if (binding.reassigned) text += " (and reassigned later)";
    text += ".";
    if (binding.value_node >= 0) {
        const std::string value = Ellipsize(FirstLineOf(lines, ast.at(binding.value_node).range), 60);
        if (!value.empty()) text += "\n\n" + binding.name + " <- " + value;
    }
    return text;
}

}  // namespace

RLspHoverInfo RLspHover(const std::vector<std::string> &lines, int line, int col) {
    RLspHoverInfo info;
    std::vector<RComment> comments;
    const std::vector<RToken> toks = RLspTokenize(lines, &comments);

    // A roxygen tag explains itself; ordinary comment prose does not.
    for (const RComment &comment : comments) {
        if (!RRangeContains(comment.range, line, col)) continue;
        if (!comment.roxygen) return info;
        size_t tag_col = 0;
        const std::string tag = RoxygenTagOf(comment.text, &tag_col);
        if (tag.empty()) return info;
        const int start = comment.range.col + static_cast<int>(tag_col);
        const int end = start + static_cast<int>(tag.size()) + 1;
        if (col < start || col >= end) return info;
        const RLspVocabEntry *entry = FindVocab(RLspRoxygenVocab(), tag);
        if (entry == nullptr) return info;
        info.found = true;
        info.text = std::string(entry->detail) + "\n\n" + entry->doc;
        info.range = comment.range;
        info.range.col = start;
        info.range.end_col = end;
        return info;
    }

    RToken tok;
    if (!TokenAt(toks, line, col, &tok)) return info;
    info.range = tok.range;

    switch (tok.kind) {
        case RTokKind::Ident: break;
        case RTokKind::Op: {
            const RLspVocabEntry *entry = FindVocab(RLspOperatorVocab(), tok.text);
            if (entry == nullptr && tok.text.size() > 2 && tok.text[0] == '%') {
                info.found = true;
                info.text = tok.text + "\n\nAn infix operator. In R any `%name%` is an ordinary function, called "
                                       "between its two arguments.";
                return info;
            }
            if (entry == nullptr) return info;
            info.found = true;
            info.text = std::string(entry->name) + "  (" + entry->detail + ")\n\n" + entry->doc;
            return info;
        }
        case RTokKind::Str:
        case RTokKind::Num: return info;
        default: {
            // A reserved word: `if`, `function`, `repeat`, ...
            const RLspVocabEntry *entry = FindVocab(RLspKeywordVocab(), tok.text);
            if (entry == nullptr) entry = FindVocab(RLspConstantVocab(), tok.text);
            if (entry == nullptr) return info;
            info.found = true;
            info.text = std::string(entry->detail) + "\n\n" + entry->doc;
            return info;
        }
    }

    RLspOptions opts;
    opts.check_files = false;
    const Analyzer analyzer(lines, opts);
    const int use = analyzer.UseAt(line, col);
    if (use >= 0) {
        const int binding = analyzer.uses()[static_cast<size_t>(use)].binding;
        if (binding >= 0) {
            info.found = true;
            info.text = DescribeBinding(analyzer, lines, analyzer.bindings()[static_cast<size_t>(binding)]);
            return info;
        }
    }
    const RLspFunctionEntry *fn = RLspFindFunction(tok.text);
    if (fn != nullptr) {
        info.found = true;
        info.text = SignatureLabel(*fn) + "\n\n" + fn->doc + "\n\nPackage: " + fn->package;
        return info;
    }
    const RLspVocabEntry *constant = FindVocab(RLspConstantVocab(), tok.text);
    if (constant != nullptr) {
        info.found = true;
        info.text = std::string(constant->name) + "  (" + constant->detail + ")\n\n" + constant->doc;
    }
    return info;
}

// --- Signature help ---------------------------------------------------

RLspSignature RLspSignatureHelp(const std::vector<std::string> &lines, int line, int col) {
    RLspSignature sig;
    const CursorContext ctx = ContextAt(lines, line, col);
    const OpenBracket *call = ctx.EnclosingCall();
    if (call == nullptr || call->callee.empty()) return sig;

    const RLspFunctionEntry *entry = RLspFindFunction(call->callee);
    if (entry != nullptr && (call->callee_pkg.empty() || call->callee_pkg == entry->package)) {
        sig.found = true;
        sig.label = SignatureLabel(*entry);
        sig.documentation = std::string(entry->doc) + "\n\nPackage: " + entry->package;
        sig.params = RLspSplitParams(entry->params);
    } else {
        RLspOptions opts;
        opts.check_files = false;
        const Analyzer analyzer(lines, opts);
        const int binding = analyzer.Lookup(call->callee, analyzer.ScopeAt(line, col));
        if (binding < 0) return sig;
        const Binding &bound = analyzer.bindings()[static_cast<size_t>(binding)];
        const RNode &fn = analyzer.ast().at(bound.value_node);
        if (fn.kind != RNodeKind::Function) return sig;
        sig.found = true;
        sig.label = LocalSignature(analyzer.ast(), lines, call->callee, fn);
        sig.documentation = RoxygenTitleAbove(analyzer.ast(), bound.range.line);
        for (const RArg &formal : fn.args) {
            std::string param = formal.name;
            if (formal.value >= 0) {
                param += " = " + Ellipsize(FirstLineOf(lines, analyzer.ast().at(formal.value).range), 24);
            }
            sig.params.push_back(param);
        }
    }

    if (sig.params.empty()) return sig;
    sig.active_param = call->arg_index;
    if (sig.active_param >= static_cast<int>(sig.params.size())) {
        // Past the last formal: `...` keeps absorbing arguments, anything
        // else has simply been given too many.
        sig.active_param = sig.params.back() == "..." ? static_cast<int>(sig.params.size()) - 1 : -1;
    }
    return sig;
}

// --- Definition and references ----------------------------------------

RLspLocation RLspDefinition(const std::vector<std::string> &lines, int line, int col, const RLspOptions &opts) {
    RLspLocation loc;
    const Analyzer analyzer(lines, opts);
    const int use = analyzer.UseAt(line, col);
    if (use >= 0) {
        const int binding = analyzer.uses()[static_cast<size_t>(use)].binding;
        if (binding >= 0) {
            loc.found = true;
            loc.range = analyzer.bindings()[static_cast<size_t>(binding)].range;
            return loc;
        }
    }
    // A `source("other.R")` path is a definition too -- of everything in
    // that file.
    for (const RNode &node : analyzer.ast().nodes) {
        if (node.kind != RNodeKind::Call || node.kids.empty() || node.args.empty()) continue;
        const RNode &callee = analyzer.ast().at(node.kids[0]);
        if (callee.kind != RNodeKind::Ident || (callee.text != "source" && callee.text != "sys.source")) continue;
        const RNode &arg = analyzer.ast().at(node.args[0].value);
        if (arg.kind != RNodeKind::Str || !RRangeContains(arg.range, line, col)) continue;
        if (opts.doc_dir.empty()) return loc;
        std::filesystem::path path(arg.text);
        if (path.is_relative()) path = std::filesystem::path(opts.doc_dir) / path;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return loc;
        loc.found = true;
        loc.path = path.string();
        return loc;
    }
    return loc;
}

std::vector<RLspReference> RLspReferences(const std::vector<std::string> &lines, int line, int col) {
    std::vector<RLspReference> out;
    RLspOptions opts;
    opts.check_files = false;
    const Analyzer analyzer(lines, opts);
    const int at = analyzer.UseAt(line, col);
    if (at < 0) return out;
    const Use &target = analyzer.uses()[static_cast<size_t>(at)];

    if (target.binding >= 0) {
        const Binding &binding = analyzer.bindings()[static_cast<size_t>(target.binding)];
        // A formal argument's declaration is not itself a use, so it is
        // added here rather than found in the use list.
        if (binding.kind == BindKind::Parameter) {
            RLspReference ref;
            ref.range = binding.range;
            ref.is_write = true;
            ref.is_definition = true;
            out.push_back(ref);
        }
        for (const Use &use : analyzer.uses()) {
            if (use.binding != target.binding) continue;
            RLspReference ref;
            ref.range = use.range;
            ref.is_write = use.write;
            ref.is_definition = use.range.line == binding.range.line && use.range.col == binding.range.col;
            out.push_back(ref);
        }
        return out;
    }
    // An unresolved name -- a base function, or something from an
    // attached package. Every occurrence of the same name is the same
    // thing as far as this document can tell.
    for (const Use &use : analyzer.uses()) {
        if (use.binding >= 0 || use.name != target.name) continue;
        RLspReference ref;
        ref.range = use.range;
        ref.is_write = use.write;
        out.push_back(ref);
    }
    return out;
}

// --- Document symbols -------------------------------------------------

namespace {

// A `# Title ----` outline header, the convention RStudio established and
// every R editor since has followed. The trailing run of dashes (or
// equals signs, or hashes) is what makes a comment a header; the number
// of leading hashes is its depth.
struct SectionHeader {
    int line = 0;
    int level = 1;
    std::string title;
    RRange range;
};

/** @brief Recognizes a `# Section ----` header comment, returning its title and depth. */
bool ParseSectionComment(const RComment &comment, SectionHeader *out) {
    if (comment.roxygen) return false;
    const std::string &text = comment.text;
    size_t i = 0;
    while (i < text.size() && text[i] == '#') i++;
    const int level = static_cast<int>(i);
    std::string body = Trim(text.substr(i));
    // The header marker: at least four of the same character, at the end.
    size_t end = body.size();
    while (end > 0 && (body[end - 1] == '-' || body[end - 1] == '=' || body[end - 1] == '#')) end--;
    if (body.size() - end < 4) return false;
    const std::string title = Trim(body.substr(0, end));
    if (title.empty()) return false;
    out->line = comment.range.line;
    out->level = level;
    out->title = title;
    out->range = comment.range;
    return true;
}

// A symbol before its parent links are worked out. `id` is the index it
// was created at, which is what a nested symbol's `parent` refers to:
// the list is sorted into document order afterwards, so positions move
// but ids do not.
struct PendingSymbol {
    RLspSymbol symbol;
    int section_level = 0;  // >0 for a section header, which can parent what follows
    int id = -1;
    int parent_id = -1;
};

/** @brief Renders a function's formals as the detail shown beside its name in the outline. */
std::string FormalsDetail(const RNode &fn) {
    std::string params;
    for (size_t i = 0; i < fn.args.size(); i++) {
        if (i > 0) params += ", ";
        params += fn.args[i].name;
    }
    return "(" + params + ")";
}

/** @brief Emits a symbol for one definition, recursing into a function body for the functions nested in it. */
void CollectSymbols(const RParseResult &ast, const std::vector<std::string> &lines, int index, int parent,
                    std::vector<PendingSymbol> *out);

/** @brief Emits child symbols for every function defined inside a function body. */
void CollectNested(const RParseResult &ast, const std::vector<std::string> &lines, int index, int parent,
                   std::vector<PendingSymbol> *out) {
    if (index < 0) return;
    const RNode &node = ast.at(index);
    if (node.kind == RNodeKind::Binary && node.kids.size() == 2 &&
        (node.text == "<-" || node.text == "=" || node.text == ":=") &&
        ast.at(node.kids[0]).kind == RNodeKind::Ident && ast.at(node.kids[1]).kind == RNodeKind::Function) {
        CollectSymbols(ast, lines, index, parent, out);
        return;  // CollectSymbols recurses into the body itself
    }
    for (int kid : node.kids) CollectNested(ast, lines, kid, parent, out);
    for (const RArg &arg : node.args) CollectNested(ast, lines, arg.value, parent, out);
}

/** @brief The string literal argument of an S4 declaration, which is the name it declares. */
std::string S4Name(const RParseResult &ast, const RNode &call, size_t arg) {
    if (call.args.size() <= arg) return std::string();
    const RNode &node = ast.at(call.args[arg].value);
    return node.kind == RNodeKind::Str ? node.text : std::string();
}

void CollectSymbols(const RParseResult &ast, const std::vector<std::string> &lines, int index, int parent,
                    std::vector<PendingSymbol> *out) {
    const RNode &node = ast.at(index);

    // `name <- value`
    if (node.kind == RNodeKind::Binary && node.kids.size() == 2 &&
        (node.text == "<-" || node.text == "=" || node.text == ":=" || node.text == "<<-")) {
        const RNode &target = ast.at(node.kids[0]);
        const RNode &value = ast.at(node.kids[1]);
        if (target.kind != RNodeKind::Ident) return;
        PendingSymbol pending;
        pending.symbol.name = target.text;
        pending.symbol.range = node.range;
        pending.symbol.sel_range = target.range;
        pending.symbol.parent = parent;
        if (value.kind == RNodeKind::Function) {
            pending.symbol.kind = RLspSymbolKind::Function;
            pending.symbol.detail = FormalsDetail(value);
            const int self = static_cast<int>(out->size());
            out->push_back(pending);
            for (int kid : value.kids) CollectNested(ast, lines, kid, self, out);
            return;
        }
        if (value.kind == RNodeKind::Call && !value.kids.empty()) {
            const RNode &callee = ast.at(value.kids[0]);
            const std::string callee_name = callee.kind == RNodeKind::Ident ? callee.text : std::string();
            if (callee_name == "R6Class" || callee_name == "setRefClass" || callee_name == "setClass") {
                pending.symbol.kind = RLspSymbolKind::Class;
                pending.symbol.detail = callee_name + "()";
                out->push_back(pending);
                return;
            }
        }
        pending.symbol.kind = RLspSymbolKind::Variable;
        pending.symbol.detail = Ellipsize(FirstLineOf(lines, value.range), 40);
        out->push_back(pending);
        return;
    }

    // The S4 declarations, which name what they define in a string.
    if (node.kind == RNodeKind::Call && !node.kids.empty()) {
        const RNode &callee = ast.at(node.kids[0]);
        if (callee.kind != RNodeKind::Ident) return;
        const std::string &name = callee.text;
        PendingSymbol pending;
        pending.symbol.range = node.range;
        pending.symbol.sel_range = callee.range;
        pending.symbol.parent = parent;
        if (name == "setClass") {
            pending.symbol.name = S4Name(ast, node, 0);
            pending.symbol.kind = RLspSymbolKind::Class;
            pending.symbol.detail = "S4 class";
        } else if (name == "setGeneric") {
            pending.symbol.name = S4Name(ast, node, 0);
            pending.symbol.kind = RLspSymbolKind::Function;
            pending.symbol.detail = "S4 generic";
        } else if (name == "setMethod") {
            const std::string generic = S4Name(ast, node, 0);
            const std::string signature = S4Name(ast, node, 1);
            pending.symbol.name = signature.empty() ? generic : generic + "," + signature;
            pending.symbol.kind = RLspSymbolKind::Method;
            pending.symbol.detail = "S4 method";
        } else if (name == "setValidity") {
            pending.symbol.name = S4Name(ast, node, 0);
            pending.symbol.kind = RLspSymbolKind::Method;
            pending.symbol.detail = "S4 validity";
        } else {
            return;
        }
        if (pending.symbol.name.empty()) return;
        out->push_back(pending);
    }
}

}  // namespace

std::vector<RLspSymbol> RLspSymbols(const std::vector<std::string> &lines) {
    const RParseResult ast = RLspParse(lines);
    std::vector<PendingSymbol> pending;

    for (const RComment &comment : ast.comments) {
        SectionHeader header;
        if (!ParseSectionComment(comment, &header)) continue;
        PendingSymbol item;
        item.symbol.name = header.title;
        item.symbol.kind = RLspSymbolKind::Namespace;
        item.symbol.detail = "section";
        item.symbol.range = header.range;
        item.symbol.sel_range = header.range;
        item.section_level = header.level;
        pending.push_back(item);
    }
    for (int root : ast.roots) CollectSymbols(ast, lines, root, -1, &pending);

    // Document order, with a stable tie-break so two definitions on one
    // line keep the order they were written in. Parent links are ids at
    // this point, not positions, precisely because this sort moves
    // things around.
    for (size_t i = 0; i < pending.size(); i++) {
        pending[i].id = static_cast<int>(i);
        pending[i].parent_id = pending[i].symbol.parent;
    }
    std::stable_sort(pending.begin(), pending.end(), [](const PendingSymbol &a, const PendingSymbol &b) {
        if (a.symbol.range.line != b.symbol.range.line) return a.symbol.range.line < b.symbol.range.line;
        return a.symbol.range.col < b.symbol.range.col;
    });
    std::vector<int> position_of_id(pending.size(), -1);
    for (size_t i = 0; i < pending.size(); i++) {
        position_of_id[static_cast<size_t>(pending[i].id)] = static_cast<int>(i);
    }

    // A section owns everything after it until a section at its own level
    // or above. A symbol that already has a parent -- a function nested
    // inside another -- keeps it.
    const int last_line = static_cast<int>(lines.empty() ? 0 : lines.size() - 1);
    const int last_col = lines.empty() ? 0 : static_cast<int>(lines.back().size());
    std::vector<int> open;  // positions of the sections currently open
    for (size_t i = 0; i < pending.size(); i++) {
        PendingSymbol &item = pending[i];
        if (item.section_level > 0) {
            while (!open.empty() &&
                   pending[static_cast<size_t>(open.back())].section_level >= item.section_level) {
                pending[static_cast<size_t>(open.back())].symbol.range.end_line = item.symbol.range.line - 1;
                open.pop_back();
            }
            item.symbol.parent = open.empty() ? -1 : open.back();
            item.symbol.range.end_line = last_line;
            item.symbol.range.end_col = last_col;
            open.push_back(static_cast<int>(i));
            continue;
        }
        if (item.parent_id >= 0) {
            item.symbol.parent = position_of_id[static_cast<size_t>(item.parent_id)];
        } else {
            item.symbol.parent = open.empty() ? -1 : open.back();
        }
    }

    std::vector<RLspSymbol> out;
    out.reserve(pending.size());
    for (const PendingSymbol &item : pending) out.push_back(item.symbol);
    return out;
}

// --- Folding ----------------------------------------------------------

namespace {

/** @brief Adds a fold, dropping single-line and duplicate ranges. */
void AddFold(int start, int end, const char *kind, std::vector<RLspFold> *out) {
    if (end <= start) return;
    for (const RLspFold &fold : *out) {
        if (fold.start_line == start && fold.end_line == end) return;
    }
    RLspFold fold;
    fold.start_line = start;
    fold.end_line = end;
    fold.kind = kind;
    out->push_back(fold);
}

/** @brief Walks the tree, folding every braced body and multi-line call. */
void CollectFolds(const RParseResult &ast, int index, std::vector<RLspFold> *out) {
    if (index < 0) return;
    const RNode &node = ast.at(index);
    if (node.kind == RNodeKind::Block || node.kind == RNodeKind::Call || node.kind == RNodeKind::Function ||
        node.kind == RNodeKind::If || node.kind == RNodeKind::For || node.kind == RNodeKind::While ||
        node.kind == RNodeKind::Index) {
        // The closing line stays visible, as every editor's brace folding
        // does: the fold covers the body, not the `}` that ends it.
        AddFold(node.range.line, node.range.end_line - 1, "", out);
    }
    for (int kid : node.kids) CollectFolds(ast, kid, out);
    for (const RArg &arg : node.args) CollectFolds(ast, arg.value, out);
}

}  // namespace

std::vector<RLspFold> RLspFolds(const std::vector<std::string> &lines) {
    const RParseResult ast = RLspParse(lines);
    std::vector<RLspFold> out;
    for (int root : ast.roots) CollectFolds(ast, root, &out);

    // Runs of comment lines fold as one comment region.
    int run_start = -1;
    int run_end = -1;
    for (const RComment &comment : ast.comments) {
        // Only a comment that is the whole line joins a run: a trailing
        // `# note` after code is not a foldable block, and a
        // `# Section ----` header gets a region of its own below rather
        // than being swallowed into the prose under it.
        const std::string &line = lines[static_cast<size_t>(comment.range.line)];
        const bool own_line = Trim(line.substr(0, static_cast<size_t>(comment.range.col))).empty();
        SectionHeader header;
        if (!own_line || ParseSectionComment(comment, &header)) continue;
        if (run_start >= 0 && comment.range.line == run_end + 1) {
            run_end = comment.range.line;
            continue;
        }
        AddFold(run_start, run_end, "comment", &out);
        run_start = comment.range.line;
        run_end = comment.range.line;
    }
    AddFold(run_start, run_end, "comment", &out);

    // `# Section ----` regions, to the line before the next section at the
    // same depth or above.
    std::vector<SectionHeader> sections;
    for (const RComment &comment : ast.comments) {
        SectionHeader header;
        if (ParseSectionComment(comment, &header)) sections.push_back(header);
    }
    const int last_line = static_cast<int>(lines.empty() ? 0 : lines.size() - 1);
    for (size_t i = 0; i < sections.size(); i++) {
        int end = last_line;
        for (size_t j = i + 1; j < sections.size(); j++) {
            if (sections[j].level > sections[i].level) continue;
            end = sections[j].line - 1;
            break;
        }
        AddFold(sections[i].line, end, "", &out);
    }

    std::stable_sort(out.begin(), out.end(), [](const RLspFold &a, const RLspFold &b) {
        if (a.start_line != b.start_line) return a.start_line < b.start_line;
        return a.end_line > b.end_line;
    });
    return out;
}
