#include <algorithm>
#include <map>
#include <set>
#include <string>

#include "clojure_lsp.h"
#include "clojure_lsp_analysis.h"

// The lints. Each one is calibrated on the corpus of real Clojure in
// build/grammar-corpus/clojure (what clojure-lsp, babashka, clj-kondo,
// polylith, jet and cljfmt ship), against clj-kondo's findings on the same
// files: code that compiles must not be reported for what the compiler
// would refuse.

using namespace clj;

namespace {

CljLspDiagnostic Diag(const CljRange &r, CljLspSeverity sev, const std::string &code, const std::string &msg) {
    CljLspDiagnostic d;
    d.range = r;
    d.severity = sev;
    d.code = code;
    d.message = msg;
    return d;
}

bool InReaderConditional(const Analysis &a, int node) {
    for (int p = a.read.at(node).parent; p >= 0; p = a.read.at(p).parent) {
        const CljNodeKind k = a.read.at(p).kind;
        if (k == CljNodeKind::ReadCond || k == CljNodeKind::ReadCondSplicing) return true;
    }
    // Kids spliced from #?@ have the conditional's vector as parent, which is
    // inside the conditional: covered above.
    return false;
}

// Is `inner` inside the form `outer`?
bool Inside(const CljRange &outer, const CljRange &inner) {
    return !Before(inner, outer) && !Before(CljRange{outer.end_line, outer.end_col, 0, 0},
                                            CljRange{inner.end_line, inner.end_col, 0, 0});
}

struct Arity {
    int fixed = 0;
    bool variadic = false;
};

// "[x y & more]" -> 2 fixed, variadic. Destructuring forms count as one.
bool ParseArglist(const std::string &s, Arity *out) {
    if (s.size() < 2 || s.front() != '[' || s.back() != ']') return false;
    int depth = 0;
    bool in_token = false, amp = false;
    Arity a;
    for (size_t k = 1; k + 1 < s.size(); k++) {
        const char c = s[k];
        if (c == '[' || c == '{' || c == '(') {
            if (depth == 0) {
                if (amp) a.variadic = true;
                else a.fixed++;
                amp = false;
            }
            depth++;
            in_token = false;
            continue;
        }
        if (c == ']' || c == '}' || c == ')') {
            depth--;
            continue;
        }
        if (depth > 0) continue;
        if (c == ' ' || c == ',') {
            in_token = false;
            continue;
        }
        if (in_token) continue;
        in_token = true;
        if (c == '&' && (k + 2 >= s.size() || s[k + 1] == ' ')) {
            amp = true;
            continue;
        }
        if (c == '^') {  // a type hint: the next token is the parameter
            in_token = false;
            while (k + 1 < s.size() && s[k + 1] != ' ') k++;
            continue;
        }
        // A documentation arglist's `xform*` stands for any number of them.
        size_t e = k;
        while (e + 1 < s.size() && s[e + 1] != ' ' && s[e + 1] != ']') e++;
        if (s[e] == '*') {
            a.variadic = true;
            continue;
        }
        if (amp) {
            a.variadic = true;
            amp = false;
        } else {
            a.fixed++;
        }
    }
    *out = a;
    return true;
}

std::string Plural(int n, const char *word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); }

}  // namespace

std::vector<CljLspDiagnostic> CljLspDiagnostics(const std::vector<std::string> &lines, const std::string &path,
                                                const CljProject *project) {
    std::vector<CljLspDiagnostic> out;
    const Analysis a = Analyze(lines, path, project);

    // Reader errors: what the JVM reader would stop at.
    for (const CljReadError &e : a.read.errors) out.push_back(Diag(e.range, CljLspSeverity::Error, e.code, e.message));
    if (a.ext == "edn") return out;  // data: nothing to resolve

    for (const CljLspDiagnostic &d : a.shape) out.push_back(d);

    // A reader conditional in a .clj file: `load` reads .clj without them.
    if (a.ext == "clj") {
        for (size_t k = 0; k < a.read.nodes.size(); k++) {
            const CljNodeKind kind = a.read.nodes[k].kind;
            if (kind == CljNodeKind::ReadCond || kind == CljNodeKind::ReadCondSplicing) {
                out.push_back(Diag(a.read.nodes[k].range, CljLspSeverity::Error, "reader-conditional",
                                   "Conditional read not allowed (reader conditionals belong in .cljc files)"));
                break;
            }
        }
    }

    // The namespace's name against the file's path.
    if (a.ns_name_node >= 0 && !path.empty() && (a.ext == "clj" || a.ext == "cljc" || a.ext == "cljs")) {
        std::string want = a.ns_name;
        for (char &c : want) {
            if (c == '.') c = '/';
            else if (c == '-') c = '_';
        }
        const std::string tail = "/" + want + "." + a.ext;
        const std::string p = "/" + path;
        if (p.size() < tail.size() || p.compare(p.size() - tail.size(), tail.size(), tail) != 0) {
            out.push_back(Diag(a.read.at(a.ns_name_node).range, CljLspSeverity::Warning, "namespace-name-mismatch",
                               "Namespace " + a.ns_name + " does not match the file's path: it would be " +
                                   want + "." + a.ext));
        }
    }

    // --- names -------------------------------------------------------------------
    std::set<std::string> unresolved_names;
    std::vector<const Occurrence *> unresolved;
    for (const Occurrence &o : a.occs) {
        if (!o.checked || o.is_definition || o.target != Target::Unresolved || !a.clj_checks) continue;
        // `a.b.c/x` with a.b.c loaded by some other file works: only an
        // alias (no dot) is certainly missing.
        if (o.alias_unknown && o.ns.find('.') != std::string::npos) continue;
        unresolved.push_back(&o);
        unresolved_names.insert(o.alias_unknown ? o.ns + "/" : o.text);
    }
    // Many names the server cannot place: the file depends on a context it
    // cannot see (a macro library, code `load`ed into another namespace).
    if (unresolved_names.size() <= 5) {
        for (const Occurrence *o : unresolved) {
            if (o->alias_unknown) {
                out.push_back(Diag(o->range, CljLspSeverity::Error, "unresolved-namespace", "No such namespace: " + o->ns));
            } else if (!o->ns.empty() && o->text.find('/') != std::string::npos) {
                out.push_back(Diag(o->range, CljLspSeverity::Error, "unresolved-var", "No such var: " + o->text));
            } else {
                out.push_back(Diag(o->range, CljLspSeverity::Error, "unresolved-symbol",
                                   "Unable to resolve symbol: " + o->text + " in this context"));
            }
        }
    }

    // `ns/x` naming a var its namespace keeps private.
    for (const Occurrence &o : a.occs) {
        if (!o.checked || !o.not_public || !a.clj_checks) continue;
        out.push_back(Diag(o.range, CljLspSeverity::Warning, "private-var",
                           "var: #'" + o.ns + "/" + o.name + " is not public"));
    }

    // A var used above its definition: the compiler resolves names as it
    // reaches them, so this is the same error.
    for (const Occurrence &o : a.occs) {
        if (!o.checked || o.is_definition || o.target != Target::FileVar || !a.clj_checks) continue;
        const FileVar &v = a.vars[static_cast<size_t>(o.var)];
        // The earliest definition of the name.
        int first = -1;
        for (int k : FileVarsNamed(a, v.name)) {
            const FileVar &w = a.vars[static_cast<size_t>(k)];
            if (w.in_comment || w.other_platform) continue;
            if (first < 0 || w.top < a.vars[static_cast<size_t>(first)].top ||
                (w.top == a.vars[static_cast<size_t>(first)].top && Before(w.form_range, a.vars[static_cast<size_t>(first)].form_range))) {
                first = k;
            }
        }
        if (first < 0) continue;
        const FileVar &d = a.vars[static_cast<size_t>(first)];
        const bool later = d.top > o.top || (d.top == o.top && Before(o.range, d.form_range) && !Inside(d.form_range, o.range));
        if (!later || d.kind == "def-macro") continue;
        out.push_back(Diag(o.range, CljLspSeverity::Error, "use-before-definition",
                           "Unable to resolve symbol: " + o.text + " in this context (it is defined below, at line " +
                               std::to_string(d.range.line + 1) + ": move it up, or `declare` it)"));
    }

    // --- argument counts ------------------------------------------------------
    for (const Call &c : a.calls) {
        if (!c.checked || a.own_vocab_ns) continue;
        const Occurrence &h = a.occs[static_cast<size_t>(c.head)];
        // A call whose arguments a #?@ splices in has no fixed count.
        bool spliced = false;
        for (int kid : a.read.at(c.node).kids) {
            if (a.read.at(kid).kind == CljNodeKind::ReadCondSplicing) spliced = true;
        }
        if (spliced) continue;
        std::vector<std::string> arglists;
        std::string who = h.text;
        if (h.target == Target::CoreVar) {
            const CljLspVar *v = CljLspCoreVar(h.ns, h.name);
            if (!v || v->kind != "function" || v->arglists.empty()) continue;
            size_t p = 0;
            while (p < v->arglists.size()) {
                const size_t nl = v->arglists.find('\n', p);
                arglists.push_back(v->arglists.substr(p, nl == std::string::npos ? std::string::npos : nl - p));
                if (nl == std::string::npos) break;
                p = nl + 1;
            }
            who = (h.ns.empty() ? "" : h.ns + "/") + h.name;
        } else if (h.target == Target::FileVar) {
            const FileVar &v = a.vars[static_cast<size_t>(h.var)];
            if (v.kind != "defn" && v.kind != "defn-" && v.kind != "defmacro" && v.kind != "constructor" && v.kind != "def")
                continue;
            // Every definition of the name must agree (a #? may define it twice).
            if (FileVarsNamed(a, v.name).size() != 1 || a.own_vocab_ns || v.arities.empty()) continue;
            for (const auto &[fixed, variadic] : v.arities) {
                std::string al = "[";
                for (int q = 0; q < fixed; q++) al += "p ";
                arglists.push_back(al + (variadic ? "& r]" : "]"));
            }
        } else if (h.target == Target::ProjectVar && project) {
            for (const CljProjectVar &v : project->vars) {
                if (v.ns != h.ns || v.name != h.name || v.kind == "var") continue;
                size_t p = 0;
                while (p < v.arglists.size()) {
                    const size_t nl = v.arglists.find('\n', p);
                    arglists.push_back(v.arglists.substr(p, nl == std::string::npos ? std::string::npos : nl - p));
                    if (nl == std::string::npos) break;
                    p = nl + 1;
                }
            }
        } else {
            continue;
        }
        if (arglists.empty()) continue;
        bool ok = false, parsed = true;
        std::vector<Arity> ars;
        for (const std::string &al : arglists) {
            Arity ar;
            if (!ParseArglist(al, &ar)) {
                parsed = false;
                break;
            }
            ars.push_back(ar);
            if (c.argc == ar.fixed || (ar.variadic && c.argc >= ar.fixed)) ok = true;
        }
        if (!parsed || ok) continue;
        std::string expects;
        for (size_t k = 0; k < ars.size(); k++) {
            if (k) expects += k + 1 == ars.size() ? " or " : ", ";
            expects += std::to_string(ars[k].fixed) + (ars[k].variadic ? "+" : "");
        }
        out.push_back(Diag(h.range, CljLspSeverity::Error, "invalid-arity",
                           who + " is called with " + Plural(c.argc, "arg") + " but expects " + expects));
    }

    // --- unused things ---------------------------------------------------------
    // Symbols in the branches a #?@ splice leaves out (another platform's
    // code): a binding they name is used there.
    std::vector<const CljNode *> elsewhere;
    for (const CljNode &n : a.read.nodes) {
        if (n.kind != CljNodeKind::ReadCondSplicing && n.kind != CljNodeKind::ReadCond) continue;
        for (size_t p = 1; p < n.kids.size(); p += 2) {
            std::vector<int> stack = {n.kids[p]};
            while (!stack.empty()) {
                const int x = stack.back();
                stack.pop_back();
                const CljNode &xn = a.read.at(x);
                if (xn.kind == CljNodeKind::Symbol) elsewhere.push_back(&xn);
                for (int y : xn.kids) stack.push_back(y);
            }
        }
    }
    auto used_elsewhere = [&](const Binding &b) {
        for (const CljNode *n : elsewhere) {
            if (n->text == b.name && n->range.line != b.range.line + 1000000 && CljRangeContains(b.scope, n->range.line, n->range.col) &&
                !(n->range.line == b.range.line && n->range.col == b.range.col)) {
                return true;
            }
        }
        return false;
    };
    // `~'name` in a syntax-quoted template: a macro of the file's that
    // refers to a local of its caller's by name (SCI's fns does this).
    std::set<std::string> by_name;
    for (const CljNode &n : a.read.nodes) {
        if (n.kind != CljNodeKind::Unquote || n.kids.empty()) continue;
        const CljNode &q = a.read.at(n.kids[0]);
        if (q.kind == CljNodeKind::Quote && !q.kids.empty() && a.read.at(q.kids[0]).kind == CljNodeKind::Symbol) {
            by_name.insert(a.read.at(q.kids[0]).text);
        }
    }
    for (const Binding &b : a.bindings) {
        if (b.uses > 0 || b.soft || b.exempt || b.name.empty() || b.name == "&" || b.name == "this") continue;
        if (used_elsewhere(b) || by_name.count(b.name)) continue;
        if (b.kind == "fn-name" || b.kind == "field" || b.kind == "macro") continue;
        out.push_back(Diag(b.range, CljLspSeverity::Warning, "unused-binding", "unused binding " + b.name));
    }
    // A file `load`ed into another namespace (in-ns), or one of Clojure's own:
    // its private vars are its siblings' too.
    const bool shares_privates = a.has_in_ns || a.own_vocab_ns || a.ns_form < 0;
    for (const FileVar &v : a.vars) {
        if (shares_privates || !v.is_private || v.uses > 0 || v.in_comment || v.kind == "declare") continue;
        // Any occurrence of the name counts (another definition's, a #'var).
        bool used = false;
        for (const Occurrence &o : a.occs) {
            if (!o.is_definition && o.target == Target::FileVar && a.vars[static_cast<size_t>(o.var)].name == v.name) used = true;
        }
        if (!used) out.push_back(Diag(v.range, CljLspSeverity::Warning, "unused-private-var", "Unused private var " + a.ns_name + "/" + v.name));
    }
    // Requires: an alias nothing uses, a referred name nothing uses.
    {
        // Every symbol in the file counts, whatever the walk made of it: another
        // platform's #?@ branch, a quoted `'str/join` handed to resolve.
        std::map<std::string, int> alias_uses, name_uses;
        std::set<int> ns_form_nodes;
        if (a.ns_form >= 0) {
            std::vector<int> stack = {a.ns_form};
            while (!stack.empty()) {
                const int x = stack.back();
                stack.pop_back();
                ns_form_nodes.insert(x);
                for (int y : a.read.at(x).kids) stack.push_back(y);
                for (int y : a.read.at(x).meta) stack.push_back(y);
            }
        }
        for (size_t k = 0; k < a.read.nodes.size(); k++) {
            const CljNode &n = a.read.nodes[k];
            if (n.kind != CljNodeKind::Symbol || ns_form_nodes.count(static_cast<int>(k))) continue;
            if (!n.ns.empty()) alias_uses[n.ns]++;
            name_uses[n.text]++;
        }
        for (int k : a.keywords) {
            const CljNode &n = a.read.at(k);
            if (n.auto_resolve && !n.ns.empty()) alias_uses[n.ns]++;
        }
        // Metadata and keywords anywhere (type hints `^alias/Type`, `:require`d specs).
        for (const CljNode &n : a.read.nodes) {
            if (n.kind == CljNodeKind::Keyword && n.auto_resolve && !n.ns.empty()) alias_uses[n.ns]++;
            if (n.kind == CljNodeKind::Symbol && !n.ns.empty()) alias_uses[n.ns] += 0;
        }
        std::map<std::string, int> required;
        for (const Require &rq : a.reqs) {
            // Within one platform: #?(:clj [a :as x] :cljs [a :as x]) is one require.
            if (rq.node >= 0 && InReaderConditional(a, rq.node)) continue;
            if (++required[rq.ns] == 2 && rq.node >= 0) {
                out.push_back(Diag(a.read.at(rq.node).range, CljLspSeverity::Warning, "duplicate-require",
                                   "Duplicate require of " + rq.ns));
            }
            if (rq.side_effect || rq.refer_all || rq.is_use) continue;
            bool used = !rq.alias.empty() && alias_uses[rq.alias] > 0;
            // ClojureScript: `(:require ["react" :as react])`, then `react` itself.
            if (!rq.alias.empty() && a.ext != "clj" && name_uses[rq.alias] > 0) used = true;
            // The full name used as a qualifier counts too.
            if (alias_uses[rq.ns] > 0) used = true;
            int referred_used = 0;
            for (int x : rq.refer_nodes) {
                if (name_uses[a.read.at(x).text] > 0) referred_used++;
            }
            if (referred_used > 0) used = true;
            if (!used && rq.node >= 0) {
                out.push_back(Diag(a.read.at(rq.node).range, CljLspSeverity::Warning, "unused-namespace",
                                   "namespace " + rq.ns + " is required but never used"));
                continue;
            }
            for (int x : rq.refer_nodes) {
                if (name_uses[a.read.at(x).text] == 0) {
                    out.push_back(Diag(a.read.at(x).range, CljLspSeverity::Warning, "unused-referred-var",
                                       "#'" + rq.ns + "/" + a.read.at(x).text + " is referred but never used"));
                }
            }
        }
    }

    // --- redefinitions, docstrings, deprecations -------------------------------
    {
        std::map<std::string, const FileVar *> seen;
        for (const FileVar &v : a.vars) {
            if (v.kind != "def" && v.kind != "defn" && v.kind != "defn-" && v.kind != "defmacro") continue;
            if (v.in_comment || InReaderConditional(a, v.form)) continue;
            const auto it = seen.find(v.name);
            if (it == seen.end()) {
                seen[v.name] = &v;
                continue;
            }
            out.push_back(Diag(v.range, CljLspSeverity::Warning, "redefined-var",
                               "redefined var #'" + a.ns_name + "/" + v.name + " (first defined at line " +
                                   std::to_string(it->second->range.line + 1) + ")"));
        }
    }
    for (const FileVar &v : a.vars) {
        if (v.kind != "defn" && v.kind != "defn-" && v.kind != "defmacro") continue;
        const std::vector<int> &kids = a.read.at(v.form).kids;
        if (kids.size() >= 5 && a.read.at(kids[2]).kind == CljNodeKind::Vector &&
            a.read.at(kids[3]).kind == CljNodeKind::String) {
            out.push_back(Diag(a.read.at(kids[3]).range, CljLspSeverity::Warning, "misplaced-docstring",
                               "Misplaced docstring: it goes before the parameter vector"));
        }
    }
    for (const Occurrence &o : a.occs) {
        if (o.target != Target::CoreVar || o.is_definition || !o.checked) continue;
        const CljLspVar *v = CljLspCoreVar(o.ns, o.name);
        if (v && !v->deprecated.empty()) {
            out.push_back(Diag(o.range, CljLspSeverity::Warning, "deprecated-var",
                               o.ns + "/" + o.name + " is deprecated since " + v->deprecated));
        }
    }

    // --- hints -----------------------------------------------------------------
    for (const Call &c : a.calls) {
        if (!c.checked) continue;
        const Occurrence &h = a.occs[static_cast<size_t>(c.head)];
        if (h.target != Target::CoreVar) continue;
        const std::vector<int> &kids = a.read.at(c.node).kids;
        if (h.ns.empty() && h.name == "do" && c.argc == 1 && a.read.at(c.node).kind == CljNodeKind::List) {
            out.push_back(Diag(h.range, CljLspSeverity::Hint, "redundant-do", "redundant do: it wraps a single form"));
        }
        if (h.name == "not" && h.ns == "clojure.core" && kids.size() == 2) {
            const CljNode &arg = a.read.at(kids[1]);
            if (arg.kind == CljNodeKind::List && !arg.kids.empty() && CljIsSymbol(a.read.at(arg.kids[0]), "empty?")) {
                out.push_back(Diag(a.read.at(c.node).range, CljLspSeverity::Hint, "not-empty",
                                   "(not (empty? x)) is (seq x), the idiom"));
            }
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const CljLspDiagnostic &x, const CljLspDiagnostic &y) {
        return Before(x.range, y.range);
    });
    return out;
}

std::string CljLspFormatDiagnostic(const std::string &path, const CljLspDiagnostic &d) {
    static const char *const names[] = {"", "error", "warning", "info", "hint"};
    return (path == "-" ? std::string("<stdin>") : path) + ":" + std::to_string(d.range.line + 1) + ":" +
           std::to_string(d.range.col + 1) + ": " + names[static_cast<int>(d.severity)] + ": " + d.message + " [" +
           d.code + "]";
}

bool CljLspDiagnosticFails(const CljLspDiagnostic &d) {
    return d.severity == CljLspSeverity::Error || d.severity == CljLspSeverity::Warning;
}
