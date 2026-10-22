#include "clojure_lsp_analysis.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>

namespace clj {

bool Before(const CljRange &a, const CljRange &b) {
    return a.line != b.line ? a.line < b.line : a.col < b.col;
}

std::string Extension(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    const std::string base = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return "clj";
    std::string e = base.substr(dot + 1);
    for (char &c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string NodeText(const Analysis &a, int node) {
    const CljRange &r = a.read.at(node).range;
    if (r.line < 0 || r.line >= static_cast<int>(a.lines.size())) return "";
    std::string out;
    for (int l = r.line; l <= r.end_line && l < static_cast<int>(a.lines.size()); l++) {
        const std::string &s = a.lines[static_cast<size_t>(l)];
        const size_t from = l == r.line ? static_cast<size_t>(r.col) : 0;
        const size_t to = l == r.end_line ? static_cast<size_t>(r.end_col) : s.size();
        if (from <= s.size()) out += s.substr(from, std::min(to, s.size()) - from);
        if (l != r.end_line) out += '\n';
    }
    return out;
}

namespace {

bool Contains(const CljRange &outer, const CljRange &inner) {
    return !Before(inner, outer) &&
           (inner.end_line != outer.end_line ? inner.end_line < outer.end_line : inner.end_col <= outer.end_col);
}

bool IsSym(const CljNode &n) { return n.kind == CljNodeKind::Symbol; }

// clojure.core's defining forms, by what they define.
const std::set<std::string> &DefForms() {
    static const std::set<std::string> s = {"def",      "defn",        "defn-",      "defmacro", "defonce",
                                            "definline", "defmulti",   "defprotocol", "defrecord", "deftype",
                                            "definterface", "declare", "defstruct",  "deftest"};
    return s;
}

class Walker {
public:
    explicit Walker(Analysis &a) : a_(a), r_(a.read) {}

    void Run() {
        Namespace();
        Definitions();
        const Platform p = a_.ext == "cljs" ? Platform::Cljs : a_.ext == "cljc" ? Platform::Both : Platform::Clj;
        for (size_t k = 0; k < r_.top.size(); k++) {
            top_ = static_cast<int>(k);
            platform_ = p;
            Form(r_.top[k]);
        }
        for (int d : r_.discarded) (void)d;
    }

private:
    Analysis &a_;
    const CljReadResult &r_;
    struct Scope {
        std::map<std::string, int> names;
    };
    std::vector<Scope> scopes_;
    int top_ = -1;
    int quote_ = 0;         // inside ' (data)
    int syntax_quote_ = 0;  // inside ` (a template)
    int comment_ = 0;       // inside (comment ...)
    int fn_ = 0;            // inside a fn body
    int unknown_ = 0;       // inside an unknown macro's call
    Platform platform_ = Platform::Clj;

    const CljNode &N(int k) const { return r_.at(k); }
    std::vector<int> Kids(int k) const { return CljEffectiveKidsAll(k); }

    // A collection's forms, reader conditionals spliced in all branches
    // (the analysis looks at every platform's code).
    // A collection's forms, a #?@ spliced as the JVM reads it: its :clj
    // (else :default, else first) branch. The other branches are another
    // platform's code; diagnostics look at their names separately.
    std::vector<int> CljEffectiveKidsAll(int k) const {
        std::vector<int> out;
        for (int kid : N(k).kids) {
            const CljNode &n = N(kid);
            if (n.kind == CljNodeKind::ReadCondSplicing) {
                int chosen = -1;
                for (size_t p = 0; p + 1 < n.kids.size(); p += 2) {
                    const CljNode &f = N(n.kids[p]);
                    if (chosen < 0) chosen = n.kids[p + 1];
                    if (f.kind == CljNodeKind::Keyword && (f.name == "clj" || f.name == "default")) {
                        chosen = n.kids[p + 1];
                        break;
                    }
                }
                if (chosen >= 0) {
                    for (int x : N(chosen).kids) out.push_back(x);
                }
                continue;
            }
            out.push_back(kid);
        }
        return out;
    }

    // The forms of a reader conditional's branches (a splicing one's
    // spliced), every platform's.
    std::vector<int> Branches(int k) const {
        std::vector<int> out;
        const CljNode &n = N(k);
        for (size_t p = 1; p < n.kids.size(); p += 2) {
            if (n.kind == CljNodeKind::ReadCondSplicing) {
                for (int x : N(n.kids[p]).kids) out.push_back(x);
            } else {
                out.push_back(n.kids[p]);
            }
        }
        return out;
    }

    // At a structural position (a parameter vector, a name): a reader
    // conditional stands for its :clj (else :default, else first) branch.
    int Struct(int k) const {
        const CljNode &n = N(k);
        if (n.kind != CljNodeKind::ReadCond) return k;
        int first = -1;
        for (size_t p = 0; p + 1 < n.kids.size(); p += 2) {
            const CljNode &f = N(n.kids[p]);
            if (first < 0) first = n.kids[p + 1];
            if (f.kind == CljNodeKind::Keyword && (f.name == "clj" || f.name == "default")) return n.kids[p + 1];
        }
        return first < 0 ? k : first;
    }

    // Inside a call to a macro this server cannot see into (a library's,
    // or one whose namespace it does not know), a name may be one the macro
    // binds (`(->Node (eval ctx bindings))`): not checked there.
    bool Checked() const {
        return quote_ == 0 && syntax_quote_ == 0 && comment_ == 0 && unknown_ == 0 && platform_ != Platform::Cljs;
    }

    // Is a node in a branch of a reader conditional for another platform
    // than the JVM's (:cljs, :cljr...)? Spliced forms lose the walk's
    // platform, so this asks the tree.
    bool OtherPlatform(int node) const {
        int cur = node;
        for (int p = N(node).parent; p >= 0; cur = p, p = N(p).parent) {
            const CljNode &pn = N(p);
            if (pn.kind != CljNodeKind::ReadCond && pn.kind != CljNodeKind::ReadCondSplicing) continue;
            for (size_t q = 1; q < pn.kids.size(); q += 2) {
                if (pn.kids[q] != cur) continue;
                const CljNode &f = N(pn.kids[q - 1]);
                if (f.kind == CljNodeKind::Keyword && f.name != "clj" && f.name != "default" && f.name != "bb") return true;
            }
        }
        return false;
    }

    void Shape(int node, const std::string &code, const std::string &msg,
               CljLspSeverity sev = CljLspSeverity::Error) {
        if (comment_ > 0 || quote_ > 0 || syntax_quote_ > 0) return;
        CljLspDiagnostic d;
        d.range = N(node).range;
        d.severity = sev;
        d.code = code;
        d.message = msg;
        a_.shape.push_back(d);
    }

    // --- the namespace ------------------------------------------------------

    static std::string Unquoted(const CljReadResult &r, int k) {
        const CljNode &n = r.at(k);
        if (n.kind == CljNodeKind::Quote && !n.kids.empty()) return Unquoted(r, n.kids[0]);
        if (n.kind == CljNodeKind::Symbol) return n.ns.empty() ? n.name : n.ns + "/" + n.name;
        return "";
    }

    int Unquote(int k) const {
        while (N(k).kind == CljNodeKind::Quote && !N(k).kids.empty()) k = N(k).kids[0];
        return k;
    }

    // One libspec of :require/:use: `foo.bar`, `[foo.bar :as b :refer [x]]`,
    // a prefix list `(foo bar baz)`, a string (ClojureScript's npm names).
    void Libspec(int k, bool use, const std::string &prefix) {
        k = Unquote(k);
        const CljNode &n = N(k);
        if (n.kind == CljNodeKind::ReadCond || n.kind == CljNodeKind::ReadCondSplicing) {
            for (int b : Branches(k)) Libspec(b, use, prefix);
            return;
        }
        if (n.kind == CljNodeKind::Symbol || n.kind == CljNodeKind::String) {
            Require rq;
            rq.ns = prefix.empty() ? (n.kind == CljNodeKind::String ? n.name : n.text) : prefix + "." + n.text;
            rq.node = k;
            rq.is_use = use;
            rq.refer_all = use;
            rq.side_effect = !use;
            if (use) a_.refer_all.insert(rq.ns);
            a_.reqs.push_back(rq);
            return;
        }
        const std::vector<int> kids = Kids(k);
        if (kids.empty()) return;
        if (n.kind == CljNodeKind::List) {  // a prefix list
            const std::string pre = N(kids[0]).text;
            for (size_t p = 1; p < kids.size(); p++) Libspec(kids[p], use, prefix.empty() ? pre : prefix + "." + pre);
            return;
        }
        if (n.kind != CljNodeKind::Vector) return;
        const CljNode &head = N(Struct(kids[0]));
        if (head.kind != CljNodeKind::Symbol && head.kind != CljNodeKind::String) return;
        // [prefix [a :as x] b] -- a vector holding libspecs (an older prefix form)
        if (kids.size() > 1 && (N(kids[1]).kind == CljNodeKind::Vector || N(kids[1]).kind == CljNodeKind::Symbol)) {
            bool all_specs = true;
            for (size_t p = 1; p < kids.size(); p++) {
                if (N(kids[p]).kind == CljNodeKind::Keyword) all_specs = false;
            }
            if (all_specs) {
                for (size_t p = 1; p < kids.size(); p++) Libspec(kids[p], use, prefix.empty() ? head.text : prefix + "." + head.text);
                return;
            }
        }
        Require rq;
        rq.ns = head.kind == CljNodeKind::String ? head.name : (prefix.empty() ? head.text : prefix + "." + head.text);
        rq.node = Struct(kids[0]);
        rq.is_use = use;
        bool any = false, only = false;
        for (size_t p = 1; p + 1 < kids.size(); p += 2) {
            const CljNode &opt = N(Struct(kids[p]));
            const int val = Struct(kids[p + 1]);
            if (opt.kind != CljNodeKind::Keyword) continue;
            if (opt.name == "as" || opt.name == "as-alias") {
                if (IsSym(N(val))) {
                    rq.alias = N(val).text;
                    rq.alias_node = val;
                    a_.aliases[rq.alias] = rq.ns;
                    any = true;
                }
            } else if (opt.name == "refer" || opt.name == "only" || opt.name == "refer-macros") {
                any = true;
                if (opt.name == "only") only = true;
                if (N(val).kind == CljNodeKind::Keyword && N(val).name == "all") {
                    rq.refer_all = true;
                    a_.refer_all.insert(rq.ns);
                } else {
                    for (int x : Kids(val)) {
                        if (!IsSym(N(x))) continue;
                        rq.refer_nodes.push_back(x);
                        a_.refers[N(x).text] = rq.ns;
                    }
                }
            } else if (opt.name == "rename") {
                const std::vector<int> m = Kids(val);
                for (size_t q = 0; q + 1 < m.size(); q += 2) {
                    if (IsSym(N(m[q])) && IsSym(N(m[q + 1]))) a_.refers[N(m[q + 1]).text] = rq.ns;
                }
            } else {
                any = true;  // :include-macros, :default...
            }
        }
        // :use refers the whole namespace unless :only says which names.
        if (use && !only) {
            rq.refer_all = true;
            a_.refer_all.insert(rq.ns);
        }
        rq.side_effect = !any && !use;
        a_.reqs.push_back(rq);
    }

    void Import(int k) {
        k = Unquote(k);
        const CljNode &n = N(k);
        if (n.kind == CljNodeKind::Symbol) {
            const size_t dot = n.text.find_last_of('.');
            a_.imports.insert(dot == std::string::npos ? n.text : n.text.substr(dot + 1));
            return;
        }
        if (n.kind != CljNodeKind::List && n.kind != CljNodeKind::Vector) return;
        const std::vector<int> kids = Kids(k);
        for (size_t p = 1; p < kids.size(); p++) {
            if (IsSym(N(kids[p]))) a_.imports.insert(N(kids[p]).text);
        }
    }

    void ReferClojure(const std::vector<int> &args) {
        for (size_t p = 0; p + 1 < args.size(); p += 2) {
            const CljNode &opt = N(args[p]);
            const int val = Unquote(args[p + 1]);
            if (opt.kind != CljNodeKind::Keyword) continue;
            if (opt.name == "exclude") {
                for (int x : Kids(val)) {
                    if (IsSym(N(x))) a_.core_excluded.insert(N(x).text);
                }
            } else if (opt.name == "only") {
                a_.core_only = true;
                for (int x : Kids(val)) {
                    if (IsSym(N(x))) a_.core_only_names.insert(N(x).text);
                }
            } else if (opt.name == "rename") {
                const std::vector<int> m = Kids(val);
                for (size_t q = 0; q + 1 < m.size(); q += 2) {
                    if (IsSym(N(m[q])) && IsSym(N(m[q + 1]))) {
                        a_.renames[N(m[q + 1]).text] = N(m[q]).text;
                        a_.core_excluded.insert(N(m[q]).text);
                    }
                }
            }
        }
    }

    void Namespace() {
        // Top-level forms, a top-level #?(...)'s branches included
        // (`#?(:clj (alias 'timbre 'taoensso.timbre))`).
        std::vector<int> tops;
        for (int t : r_.top) {
            if (N(t).kind == CljNodeKind::ReadCond || N(t).kind == CljNodeKind::ReadCondSplicing) {
                for (int b : Branches(t)) tops.push_back(b);
            } else {
                tops.push_back(t);
            }
        }
        for (int t : tops) {
            const CljNode &n = N(t);
            if (n.kind != CljNodeKind::List) continue;
            const std::vector<int> kids = Kids(t);
            if (kids.empty() || !IsSym(N(kids[0]))) continue;
            const std::string head = N(kids[0]).text;
            if (head == "ns" && a_.ns_form < 0 && kids.size() > 1 && IsSym(N(kids[1]))) {
                a_.ns_form = t;
                a_.ns_name_node = kids[1];
                a_.ns_name = N(kids[1]).text;
                std::vector<int> clauses;
                for (size_t p = 2; p < kids.size(); p++) {
                    if (N(kids[p]).kind == CljNodeKind::ReadCond) {
                        for (int b : Branches(kids[p])) clauses.push_back(b);
                    } else {
                        clauses.push_back(kids[p]);
                    }
                }
                for (int clause : clauses) {
                    const CljNode &c = N(clause);
                    if (c.kind != CljNodeKind::List && c.kind != CljNodeKind::Vector) continue;  // [:require ...] works too
                    const std::vector<int> ck = Kids(clause);
                    if (ck.empty() || N(ck[0]).kind != CljNodeKind::Keyword) continue;
                    const std::string what = N(ck[0]).name;
                    const std::vector<int> args(ck.begin() + 1, ck.end());
                    if (what == "require" || what == "require-macros") {
                        for (int x : args) {
                            if (N(x).kind != CljNodeKind::Keyword) Libspec(x, false, "");
                        }
                    } else if (what == "use" || what == "use-macros") {
                        for (int x : args) {
                            if (N(x).kind != CljNodeKind::Keyword) Libspec(x, true, "");
                        }
                    } else if (what == "import") {
                        for (int x : args) Import(x);
                    } else if (what == "refer-clojure") {
                        ReferClojure(args);
                    }
                }
                continue;
            }
            // The REPL-style forms a script uses instead.
            const std::vector<int> args(kids.begin() + 1, kids.end());
            if (head == "require" || head == "use") {
                for (int x : args) {
                    if (N(x).kind != CljNodeKind::Keyword) Libspec(x, head == "use", "");
                }
            } else if (head == "import") {
                for (int x : args) Import(x);
            } else if (head == "refer-clojure") {
                ReferClojure(args);
            } else if (head == "alias" && args.size() == 2) {
                a_.aliases[Unquoted(r_, args[0])] = Unquoted(r_, args[1]);
            } else if (head == "in-ns" && !args.empty()) {
                const std::string target = Unquoted(r_, args[0]);
                if (target != a_.ns_name) a_.has_in_ns = true;
            } else if (head == "refer" && !args.empty()) {
                a_.refer_all.insert(Unquoted(r_, args[0]));
            }
        }
    }

    // --- definitions -----------------------------------------------------------

    // `(defn ^:private name "doc" {:attr} [params] body)`: the parts.
    void AddVar(int form, int name_node, const std::string &kind, const std::string &parent = "",
                const std::string &name = "") {
        FileVar v;
        v.name = name.empty() ? N(name_node).text : name;
        v.kind = kind;
        v.name_node = name_node;
        v.form = form;
        v.top = top_;
        v.range = N(name_node).range;
        v.form_range = N(form).range;
        v.parent = parent;
        v.in_comment = comment_ > 0;
        v.other_platform = OtherPlatform(form) || OtherPlatform(name_node);
        v.declared_only = kind == "declare";
        for (int m : N(name_node).meta) {
            const CljNode &mn = N(m);
            if (mn.kind == CljNodeKind::Keyword && mn.name == "private") v.is_private = true;
            if (mn.kind == CljNodeKind::Map) {
                const std::vector<int> mk = Kids(m);
                for (size_t q = 0; q + 1 < mk.size(); q += 2) {
                    if (N(mk[q]).kind == CljNodeKind::Keyword && N(mk[q]).name == "private" &&
                        N(mk[q + 1]).kind == CljNodeKind::Bool && N(mk[q + 1]).text == "true") {
                        v.is_private = true;
                    }
                }
            }
        }
        if (kind == "defn-") v.is_private = true;
        // Docstring and arglists.
        const std::vector<int> kids = Kids(form);
        size_t p = 2;
        if (p < kids.size() && N(Struct(kids[p])).kind == CljNodeKind::String && kind != "def" ) {
            if (p + 1 < kids.size() || kind == "defmulti") v.doc = N(kids[p]).name;
            p++;
        }
        if (kind == "def" || kind == "defonce") {
            if (kids.size() == 4 && N(kids[2]).kind == CljNodeKind::String) v.doc = N(kids[2]).name;
            if (kids.size() >= 3) {
                // (def f (fn [x] ...)) reads as a function.
                const int init = kids.back();
                const std::vector<int> ik = Kids(init);
                if (N(init).kind == CljNodeKind::List && !ik.empty() && IsSym(N(ik[0])) &&
                    (N(ik[0]).text == "fn" || N(ik[0]).text == "fn*")) {
                    size_t q = 1;
                    if (q < ik.size() && IsSym(N(ik[q]))) q++;
                    Arglists(ik, q, &v.arglists, &v.arities);
                }
            }
        }
        if (p < kids.size() && N(kids[p]).kind == CljNodeKind::Map && kind != "def" && kind != "defmulti") p++;
        if (kind == "defn" || kind == "defn-" || kind == "defmacro" || kind == "definline") Arglists(kids, p, &v.arglists, &v.arities);
        // {:arglists '([x])} in the attribute map wins, as in Clojure.
        for (size_t q = 2; q < kids.size() && q < 4; q++) {
            if (N(kids[q]).kind == CljNodeKind::Map) ArglistsMeta(kids[q], &v.arglists);
        }
        for (int m : N(name_node).meta) {
            if (N(m).kind == CljNodeKind::Map) ArglistsMeta(m, &v.arglists);
        }
        a_.var_index.emplace(v.name, static_cast<int>(a_.vars.size()));
        a_.vars.push_back(v);
    }

    void Arglists(const std::vector<int> &kids, size_t p, std::vector<std::string> *out,
                  std::vector<std::pair<int, bool>> *arities = nullptr) {
        if (p >= kids.size()) return;
        if (N(Struct(kids[p])).kind == CljNodeKind::Vector) {
            out->push_back(Collapse(NodeText(a_, Struct(kids[p]))));
            if (arities) arities->push_back(ArityOf(Struct(kids[p])));
            return;
        }
        for (; p < kids.size(); p++) {
            const int arity = Struct(kids[p]);
            const std::vector<int> ar = Kids(arity);
            if (N(arity).kind == CljNodeKind::List && !ar.empty() && N(Struct(ar[0])).kind == CljNodeKind::Vector) {
                out->push_back(Collapse(NodeText(a_, Struct(ar[0]))));
                if (arities) arities->push_back(ArityOf(Struct(ar[0])));
            }
        }
    }

    // A parameter vector's count: its forms up to `&` (a #? standing for
    // its :clj branch), and whether a rest parameter follows.
    std::pair<int, bool> ArityOf(int vec) const {
        int fixed = 0;
        bool variadic = false;
        for (int x : Kids(vec)) {
            const int y = Struct(x);
            if (N(y).kind == CljNodeKind::ReadCond) continue;
            if (IsSym(N(y)) && N(y).text == "&") {
                variadic = true;
                break;
            }
            fixed++;
        }
        return {fixed, variadic};
    }

    void ArglistsMeta(int map, std::vector<std::string> *out) {
        const std::vector<int> mk = Kids(map);
        for (size_t q = 0; q + 1 < mk.size(); q += 2) {
            if (N(mk[q]).kind != CljNodeKind::Keyword || N(mk[q]).name != "arglists") continue;
            int v = Unquote(mk[q + 1]);
            if (N(v).kind != CljNodeKind::List && N(v).kind != CljNodeKind::Vector) continue;
            std::vector<std::string> got;
            for (int x : Kids(v)) {
                if (N(x).kind == CljNodeKind::Vector) got.push_back(Collapse(NodeText(a_, x)));
            }
            if (!got.empty()) *out = got;
        }
    }

    static std::string FirstLine(const std::string &s) {
        const size_t nl = s.find('\n');
        std::string t = s.substr(0, nl);
        if (t.size() > 40) t = t.substr(0, 40) + "...";
        return t + (nl != std::string::npos && t.size() <= 40 ? " ..." : "");
    }

    static std::string Collapse(const std::string &s) {
        std::string out;
        bool space = false;
        for (char c : s) {
            if (c == ' ' || c == '\n' || c == '\t' || c == ',') {
                space = true;
                continue;
            }
            if (space && !out.empty() && out.back() != '[' && out.back() != '{' && c != ']' && c != '}') out += ' ';
            space = false;
            out += c;
        }
        return out;
    }

    // Every definition in the file, wherever it is (a def inside a defn is
    // interned when the defn is compiled).
    void Definitions() {
        for (size_t k = 0; k < r_.top.size(); k++) {
            top_ = static_cast<int>(k);
            comment_ = 0;
            top_level_ = true;
            Defs(r_.top[k]);
        }
        top_level_ = false;
        top_ = -1;
        comment_ = 0;
    }

    void Defs(int k) {
        const CljNode &n = N(k);
        if (n.kind == CljNodeKind::Quote || n.kind == CljNodeKind::SyntaxQuote) return;
        if (n.kind == CljNodeKind::List) {
            const std::vector<int> kids = Kids(k);
            if (!kids.empty() && IsSym(N(kids[0]))) {
                const CljNode &h = N(kids[0]);
                const std::string head = h.ns.empty() || h.ns == "clojure.core" ? h.name : "";
                const bool comment = head == "comment";
                if (comment) comment_++;
                DefForm(k, kids, head, h);
                // A top-level do's forms are top-level forms too.
                const bool saved = top_level_;
                top_level_ = saved && head == "do";
                for (int x : kids) Defs(x);
                top_level_ = saved;
                if (comment) comment_--;
                return;
            }
        }
        // A top-level #?(...)'s branches are top-level forms too.
        const bool saved = top_level_;
        top_level_ = saved && (n.kind == CljNodeKind::ReadCond || n.kind == CljNodeKind::ReadCondSplicing);
        for (int x : n.kids) Defs(x);
        top_level_ = saved;
    }

    bool top_level_ = false;  // Defs() is at a top-level form (or in a top-level do)

    bool FileMacro(const std::string &name) const {
        const auto range = a_.var_index.equal_range(name);
        for (auto it = range.first; it != range.second; ++it) {
            if (a_.vars[static_cast<size_t>(it->second)].kind == "defmacro") return true;
        }
        return false;
    }

    static bool Importer(const std::string &hn) {
        return hn.find("import") != std::string::npos || hn.find("export") != std::string::npos ||
               hn.find("alias") != std::string::npos || hn.find("copy") != std::string::npos;
    }

    void DefImporter(int k, const std::vector<int> &kids, const CljNode &h) {
        if (PublicCore(h.name)) return;
        std::function<void(int)> names = [&](int x) {
            const CljNode &xn = N(x);
            if (xn.kind == CljNodeKind::Symbol && !xn.ns.empty()) {
                AddVar(k, x, "def-macro", "", xn.name);
            } else if (xn.kind == CljNodeKind::Symbol && xn.ns.empty() && x == kids[1]) {
                AddVar(k, x, "def-macro");
            } else if (xn.kind == CljNodeKind::Vector || xn.kind == CljNodeKind::List) {
                // [ns a b] names a and b; [ns/a ns/b] names a and b too.
                const std::vector<int> vk = Kids(x);
                for (size_t q = 0; q < vk.size(); q++) {
                    if (!IsSym(N(vk[q]))) continue;
                    if (!N(vk[q]).ns.empty()) AddVar(k, vk[q], "def-macro", "", N(vk[q]).name);
                    else if (q > 0) AddVar(k, vk[q], "def-macro");
                }
            } else if (xn.kind == CljNodeKind::ReadCondSplicing || xn.kind == CljNodeKind::ReadCond) {
                for (int b : Branches(x)) names(b);
            }
        };
        for (size_t p = 1; p < kids.size(); p++) names(kids[p]);
    }

    void DefForm(int k, const std::vector<int> &kids, const std::string &head, const CljNode &h) {
        if (kids.size() < 2) return;
        // A head that is not clojure.core's: a library's macro, maybe.
        const bool foreign = !(h.ns.empty() || h.ns == "clojure.core") || !PublicCore(h.name);
        const int name = Struct(kids[1]);
        const bool named = IsSym(N(name)) && N(name).ns.empty();
        if (head == "declare") {
            for (size_t p = 1; p < kids.size(); p++) {
                if (IsSym(N(kids[p]))) AddVar(k, kids[p], "declare");
            }
            return;
        }
        if (!named) {
            // `(export-api ns/a ns/b)`: no plain name first, names all the same.
            if (top_level_ && foreign && Importer(h.name)) DefImporter(k, kids, h);
            return;
        }
        if (head == "def" || head == "defn" || head == "defn-" || head == "defmacro" || head == "defonce" ||
            head == "definline" || head == "defmulti" || head == "defstruct" || head == "deftest") {
            AddVar(k, name, head);
            return;
        }
        if (head == "defprotocol") {
            AddVar(k, name, "defprotocol");
            for (size_t p = 2; p < kids.size(); p++) {
                const std::vector<int> sig = Kids(kids[p]);
                if (N(kids[p]).kind != CljNodeKind::List || sig.empty() || !IsSym(N(sig[0]))) continue;
                AddVar(kids[p], sig[0], "protocol-method", N(name).text);
                FileVar &v = a_.vars.back();
                for (size_t q = 1; q < sig.size(); q++) {
                    if (N(sig[q]).kind == CljNodeKind::Vector) v.arglists.push_back(Collapse(NodeText(a_, sig[q])));
                    if (N(sig[q]).kind == CljNodeKind::String) v.doc = N(sig[q]).name;
                }
            }
            return;
        }
        if (head == "defrecord" || head == "deftype" || head == "definterface") {
            AddVar(k, name, head);
            // ->Name (and map->Name): constructors the macro defines.
            if (head != "definterface") {
                AddVar(k, name, "constructor", N(name).text, "->" + N(name).text);
                if (kids.size() > 2 && N(kids[2]).kind == CljNodeKind::Vector) {
                    a_.vars.back().arglists.push_back(Collapse(NodeText(a_, kids[2])));
                    a_.vars.back().arities.push_back(ArityOf(kids[2]));
                }
                if (head == "defrecord") {
                    AddVar(k, name, "constructor", N(name).text, "map->" + N(name).text);
                    a_.vars.back().arglists = {"[m]"};
                    a_.vars.back().arities = {{1, false}};
                }
            }
            return;
        }
        // A library's def-macro (`defroutes`, `defstate`, `s/defn`): a head
        // whose name starts with "def" and a symbol after it.
        if (head.empty() || DefForms().count(head) == 0) {
            if (h.name.compare(0, 3, "def") == 0 && h.name != "default" && h.name != "defer" && h.name != "deftype*" &&
                h.name != "defmethod") {
                AddVar(k, name, "def-macro");
                return;
            }
        }
        // At the top level, an unknown macro given a new name first
        // (`(copy-var name other/name)`) defines it; one given qualified
        // names (`(import-vars [ns a b])`, `(export-api ns/a)`) defines theirs.
        if (top_level_ && foreign &&
            (!a_.var_index.count(h.name) || FileMacro(h.name))) {
            if (Importer(h.name)) {
                DefImporter(k, kids, h);
                return;
            }
            if (!a_.var_index.count(N(name).text) && !PublicCore(N(name).text) && h.ns.empty() == false) {
                AddVar(k, name, "def-macro");
            } else if (!a_.var_index.count(N(name).text) && !PublicCore(N(name).text) &&
                       !a_.refers.count(h.name)) {
                AddVar(k, name, "def-macro");
            }
        }
    }

    // --- scopes ------------------------------------------------------------------

    void Push() { scopes_.emplace_back(); }
    void Pop() { scopes_.pop_back(); }

    int Bind(int node, const std::string &kind, const CljRange &scope, bool soft = false) {
        const CljNode &n = N(node);
        Binding b;
        b.name = n.text;
        b.node = node;
        b.range = n.range;
        b.scope = scope;
        b.kind = kind;
        b.soft = soft;
        b.exempt = !n.text.empty() && n.text[0] == '_';
        const int idx = static_cast<int>(a_.bindings.size());
        a_.bindings.push_back(b);
        if (scopes_.empty()) Push();
        scopes_.back().names[n.text] = idx;
        // The binding position is an occurrence too (for highlights, rename).
        Occurrence o;
        o.text = n.text;
        o.node = node;
        o.range = n.range;
        o.target = Target::Local;
        o.binding = idx;
        o.is_definition = true;
        o.top = top_;
        o.checked = false;
        a_.occs.push_back(o);
        return idx;
    }

    int Lookup(const std::string &name) const {
        for (size_t k = scopes_.size(); k-- > 0;) {
            const auto it = scopes_[k].names.find(name);
            if (it != scopes_[k].names.end()) return it->second;
        }
        return -1;  // -2 is &form/&env
    }

    // A binding form: a symbol, or a vector/map destructuring.
    void Destructure(int k, const std::string &kind, const CljRange &scope, bool exempt = false) {
        const CljNode &n = N(k);
        switch (n.kind) {
            case CljNodeKind::Symbol: {
                if (n.text == "&" || !n.ns.empty()) return;
                const int b = Bind(k, kind, scope);
                if (exempt) a_.bindings[static_cast<size_t>(b)].exempt = true;
                return;
            }
            case CljNodeKind::Vector: {
                const std::vector<int> kids = Kids(k);
                for (size_t p = 0; p < kids.size(); p++) {
                    const CljNode &x = N(kids[p]);
                    if (x.kind == CljNodeKind::Keyword && x.name == "as" && p + 1 < kids.size()) {
                        Destructure(kids[++p], kind, scope, exempt);
                        continue;
                    }
                    Destructure(kids[p], kind, scope, exempt);
                }
                return;
            }
            case CljNodeKind::Map: {
                const std::vector<int> kids = Kids(k);
                int or_map = -1;
                for (size_t p = 0; p + 1 < kids.size(); p += 2) {
                    const CljNode &key = N(kids[p]);
                    const int val = kids[p + 1];
                    if (key.kind == CljNodeKind::Keyword) {
                        if (key.name == "keys" || key.name == "strs" || key.name == "syms") {
                            for (int x : Kids(val)) {
                                const CljNode &xn = N(x);
                                if (xn.kind != CljNodeKind::Symbol && xn.kind != CljNodeKind::Keyword) continue;
                                // :keys [a/b] binds b; the binding is the symbol's name part.
                                Binding b;
                                const int idx = BindName(x, xn.name, kind, scope);
                                (void)b;
                                a_.bindings[static_cast<size_t>(idx)].destructured_key = true;
                                if (exempt) a_.bindings[static_cast<size_t>(idx)].exempt = true;
                            }
                            continue;
                        }
                        if (key.name == "as") {
                            Destructure(val, kind, scope, exempt);
                            continue;
                        }
                        if (key.name == "or") {
                            or_map = val;
                            continue;
                        }
                    }
                    // {name :key}: the key is a binding form, the value a lookup key.
                    Destructure(kids[p], kind, scope, exempt);
                    Form(val);
                }
                if (or_map >= 0) {
                    const std::vector<int> ok = Kids(or_map);
                    for (size_t p = 0; p + 1 < ok.size(); p += 2) Form(ok[p + 1]);
                }
                return;
            }
            case CljNodeKind::ReadCond:
            case CljNodeKind::ReadCondSplicing:
                for (int b : Branches(k)) Destructure(b, kind, scope, exempt);
                return;
            default: return;
        }
    }

    // A binding whose name is not the node's whole text (`:keys [a/b]`).
    int BindName(int node, const std::string &name, const std::string &kind, const CljRange &scope) {
        const int idx = Bind(node, kind, scope);
        a_.bindings[static_cast<size_t>(idx)].name = name;
        a_.occs.back().text = name;
        scopes_.back().names.erase(N(node).text);
        scopes_.back().names[name] = idx;
        return idx;
    }

    // --- resolution ------------------------------------------------------------------

    void Symbol(int k) {
        const CljNode &n = N(k);
        if (quote_ > 0) return;
        Occurrence o;
        o.text = n.text;
        o.node = k;
        o.range = n.range;
        o.top = top_;
        o.in_comment = comment_ > 0;
        o.checked = Checked() && !OtherPlatform(k);
        Resolve(n, &o);
        if (o.target == Target::Local) a_.bindings[static_cast<size_t>(o.binding)].uses++;
        if (o.target == Target::FileVar) a_.vars[static_cast<size_t>(o.var)].uses++;
        a_.occs.push_back(o);
    }

    static bool LooksLikeClass(const std::string &s) {
        if (s.empty()) return false;
        if (std::isupper(static_cast<unsigned char>(s[0]))) return true;
        const size_t dot = s.find_last_of('.');
        return dot != std::string::npos && dot + 1 < s.size() && std::isupper(static_cast<unsigned char>(s[dot + 1]));
    }

    bool KnownNamespace(const std::string &ns) const {
        if (ns == a_.ns_name || ns == "clojure.core") return true;
        if (!CljLspNamespaceVars(ns).empty()) return true;
        for (const Require &r : a_.reqs) {
            if (r.ns == ns) return true;
        }
        return a_.project && a_.project->ns_paths.count(ns) > 0;
    }

    // A var `name` in namespace `ns`, wherever it is known.
    void ResolveIn(const std::string &ns, const std::string &name, Occurrence *o) {
        o->ns = ns;
        o->name = name;
        if (ns == a_.ns_name) {
            const auto it = a_.var_index.find(name);
            if (it != a_.var_index.end()) {
                o->target = Target::FileVar;
                o->var = it->second;
                return;
            }
        }
        if (!CljLspNamespaceVars(ns).empty() || ns == "clojure.core") {
            const CljLspVar *v = CljLspCoreVar(ns, name);
            o->target = v ? Target::CoreVar : Target::Unresolved;
            if (v && v->is_private) o->not_public = true;
            return;
        }
        if (a_.project && a_.project->ns_paths.count(ns)) {
            for (const CljProjectVar &v : a_.project->vars) {
                if (v.ns == ns && v.name == name) {
                    o->target = Target::ProjectVar;
                    return;
                }
            }
            o->target = Target::Unresolved;
            return;
        }
        o->target = Target::OtherVar;
    }

    static bool PublicCore(const std::string &name) {
        const CljLspVar *v = CljLspCoreVar("clojure.core", name);
        return v && !v->is_private;
    }

    // The compiler's special forms: no var can take their names over.
    static bool IsSpecialForm(const std::string &s) {
        if (CljLspCoreVar("", s)) return true;
        static const std::set<std::string> starred = {"fn*", "let*", "loop*", "letfn*", "case*", "reify*",
                                                      "deftype*", "import*", "clojure.core/import*"};
        return starred.count(s) > 0;
    }

    void Resolve(const CljNode &n, Occurrence *o) {
        const std::string &text = n.text;
        if (text == "&" || text == "_") {
            o->target = Target::Special;
            return;
        }
        if (n.ns.empty() && IsSpecialForm(text) && Lookup(text) == -1) {
            o->target = CljLspCoreVar("", text) ? Target::CoreVar : Target::Special;
            o->ns = "";
            o->name = text;
            return;
        }
        if (!n.ns.empty()) {
            std::string ns = n.ns;
            const auto al = a_.aliases.find(ns);
            if (al != a_.aliases.end()) {
                ResolveIn(al->second, n.name, o);
                return;
            }
            if (KnownNamespace(ns)) {
                ResolveIn(ns, n.name, o);
                return;
            }
            if (LooksLikeClass(ns) || ns == "js") {
                o->target = Target::Class;
                o->ns = ns;
                return;
            }
            o->target = Target::Unresolved;
            o->alias_unknown = true;
            o->ns = ns;
            o->name = n.name;
            return;
        }
        const int b = Lookup(text);
        if (b == -2) {  // &form, &env
            o->target = Target::Special;
            return;
        }
        if (b >= 0) {
            o->target = Target::Local;
            o->binding = b;
            return;
        }
        if (text[0] == '%' && (fn_literal_ > 0 || (text == "%" && fn_ > 0))) {
            o->target = Target::Special;
            return;
        }
        auto fv = a_.var_index.find(text);
        if (fv != a_.var_index.end() && !OtherPlatform(o->node)) {
            // From JVM code, a name only a :cljs branch defines is not defined.
            bool here = false;
            for (auto it = a_.var_index.equal_range(text).first; it != a_.var_index.equal_range(text).second; ++it) {
                if (!a_.vars[static_cast<size_t>(it->second)].other_platform) {
                    here = true;
                    fv = it;
                    break;
                }
            }
            if (!here) fv = a_.var_index.end();
        }
        if (fv != a_.var_index.end()) {
            // Above its first definition, a name the file takes over from
            // clojure.core (or a referred namespace) still means that one.
            int first_top = -1;
            for (auto it = a_.var_index.equal_range(text).first; it != a_.var_index.equal_range(text).second; ++it) {
                const FileVar &v = a_.vars[static_cast<size_t>(it->second)];
                if (!v.in_comment && !v.other_platform && (first_top < 0 || v.top < first_top)) first_top = v.top;
            }
            const bool core_visible = a_.core_excluded.count(text) == 0 && (!a_.core_only || a_.core_only_names.count(text));
            const bool earlier_meaning = first_top > top_ && top_ >= 0 &&
                                         ((core_visible && PublicCore(text)) || a_.refers.count(text) ||
                                          LooksLikeClass(text));  // java.lang's classes are imported everywhere
            if (!earlier_meaning) {
                o->target = Target::FileVar;
                o->var = fv->second;
                o->ns = a_.ns_name;
                o->name = text;
                return;
            }
        }
        const auto rf = a_.refers.find(text);
        if (rf != a_.refers.end()) {
            ResolveIn(rf->second, text, o);
            if (o->target == Target::Unresolved) o->target = Target::OtherVar;
            return;
        }
        const auto rn = a_.renames.find(text);
        if (rn != a_.renames.end()) {
            ResolveIn("clojure.core", rn->second, o);
            return;
        }
        if (CljLspCoreVar("", text)) {
            o->target = Target::CoreVar;
            o->ns = "";
            o->name = text;
            return;
        }
        const bool core_visible = a_.core_excluded.count(text) == 0 && (!a_.core_only || a_.core_only_names.count(text));
        if (core_visible && PublicCore(text)) {
            o->target = Target::CoreVar;
            o->ns = "clojure.core";
            o->name = text;
            return;
        }
        // Java interop: a class, Class., .method, .-field
        if (LooksLikeClass(text) || text[0] == '.' || text.back() == '.' || a_.imports.count(text) ||
            text.find('.') != std::string::npos) {
            o->target = Target::Class;
            o->ns = text;
            return;
        }
        for (const std::string &ns : a_.refer_all) {
            ResolveIn(ns, text, o);
            if (o->target != Target::Unresolved) {
                if (o->target == Target::OtherVar && !a_.refers.count(text)) o->ns = ns;
                return;
            }
        }
        o->target = Target::Unresolved;
        o->name = text;
    }

    // What an occurrence's head names: a clojure.core form this walk knows,
    // or "" for anything else.
    std::string CoreHead(int head) {
        const CljNode &h = N(head);
        if (!IsSym(h)) return "";
        if (!h.ns.empty()) {
            const auto al = a_.aliases.find(h.ns);
            const std::string ns = al == a_.aliases.end() ? h.ns : al->second;
            return ns == "clojure.core" ? h.name : "";
        }
        // A special form at the head of a list is the special form, even
        // with a local of that name in scope (the compiler checks first).
        if (CljLspCoreVar("", h.text)) return h.text;
        if (Lookup(h.text) != -1) return "";
        if (CoreSource() && CljLspCoreVar("clojure.core", h.text)) return h.text;
        if (CljLspCoreVar("", h.text)) return h.text;  // a special form, whatever the file defines
        if (h.text == "fn*" || h.text == "let*" || h.text == "loop*" || h.text == "letfn*") return h.text;
        if (a_.var_index.count(h.text)) return "";
        if (a_.refers.count(h.text)) return "";
        if (a_.renames.count(h.text)) return a_.renames.at(h.text);
        if (CljLspCoreVar("", h.text)) return h.text;
        if (a_.core_excluded.count(h.text)) return "";
        if (PublicCore(h.text)) return h.text;
        return "";
    }

    // clojure.core's own source defines its forms: they are still its forms.
    bool CoreSource() const { return a_.ns_name == "clojure.core";
    }

    // --- the walk ----------------------------------------------------------------------

    int fn_literal_ = 0;

    void Form(int k) {
        const CljNode &n = N(k);
        switch (n.kind) {
            case CljNodeKind::Symbol: Symbol(k); return;
            case CljNodeKind::Keyword: a_.keywords.push_back(k); return;
            case CljNodeKind::List: List(k); return;
            case CljNodeKind::Quote:
                quote_++;
                for (int x : n.kids) Form(x);
                quote_--;
                return;
            case CljNodeKind::SyntaxQuote:
                syntax_quote_++;
                for (int x : n.kids) Form(x);
                syntax_quote_--;
                return;
            case CljNodeKind::Unquote:
            case CljNodeKind::UnquoteSplicing: {
                // ~ unwraps one level of syntax-quote (`(`(~'~x)) keeps one).
                const int sq = syntax_quote_;
                const int q = quote_;
                if (syntax_quote_ > 0) syntax_quote_--;
                if (syntax_quote_ == 0) quote_ = 0;
                for (int x : n.kids) Form(x);
                syntax_quote_ = sq;
                quote_ = q;
                return;
            }
            case CljNodeKind::Fn: {
                // #(f %) is a list read as a fn's body.
                Push();
                fn_literal_++;
                fn_++;
                List(k);
                fn_--;
                fn_literal_--;
                Pop();
                return;
            }
            case CljNodeKind::ReadCond:
            case CljNodeKind::ReadCondSplicing: {
                const Platform saved = platform_;
                for (size_t p = 0; p + 1 < n.kids.size(); p += 2) {
                    const CljNode &f = N(n.kids[p]);
                    if (f.kind == CljNodeKind::Keyword) {
                        if (f.name == "cljs") platform_ = Platform::Cljs;
                        else if (f.name == "clj" || f.name == "default" || f.name == "bb") platform_ = saved == Platform::Cljs ? Platform::Cljs : Platform::Clj;
                        else platform_ = Platform::Cljs;  // :cljr, :lpy, ...: not checked
                    }
                    if (n.kind == CljNodeKind::ReadCondSplicing) {
                        for (int x : N(n.kids[p + 1]).kids) Form(x);
                    } else {
                        Form(n.kids[p + 1]);
                    }
                }
                platform_ = saved;
                return;
            }
            case CljNodeKind::Var: {
                // #'ns/name may name a private var: the vocabulary has only
                // the public ones.
                const size_t first = a_.occs.size();
                for (int x : n.kids) Form(x);
                for (size_t q = first; q < a_.occs.size(); q++) {
                    if (a_.occs[q].target == Target::Unresolved && !a_.occs[q].alias_unknown && !a_.occs[q].ns.empty()) {
                        a_.occs[q].checked = false;
                    }
                    a_.occs[q].not_public = false;
                }
                return;
            }
            case CljNodeKind::ReadEval: return;
            case CljNodeKind::Vector:
            case CljNodeKind::Map:
            case CljNodeKind::Set:
                // A collection literal's metadata is evaluated with it.
                for (int m : n.meta) {
                    if (N(m).kind == CljNodeKind::Map) Form(m);
                }
                for (int x : Kids(k)) Form(x);
                return;
            default:
                for (int x : Kids(k)) Form(x);
                return;
        }
    }

    // A call: argument count for the checks, after a threading macro's insert.
    void Call(int list, const std::vector<int> &kids, int inserted) {
        if (kids.empty() || !IsSym(N(kids[0]))) return;
        // The head's occurrence is the last one recorded for that node.
        for (size_t k = a_.occs.size(); k-- > 0;) {
            if (a_.occs[k].node == kids[0]) {
                ::clj::Call c;
                c.node = list;
                c.head = static_cast<int>(k);
                c.argc = static_cast<int>(kids.size()) - 1 + inserted;
                c.checked = Checked();
                a_.calls.push_back(c);
                return;
            }
        }
    }

    int insert_ = 0;  // a threading macro inserts this many arguments into the next call

    void List(int k) {
        const std::vector<int> kids = Kids(k);
        const int inserted = insert_;
        insert_ = 0;
        if (kids.empty()) return;
        if (k == a_.ns_form) return;  // read by Namespace(); evaluated before the file's own names exist
        const int h = kids[0];
        if (!IsSym(N(h))) {
            for (int x : kids) Form(x);
            return;
        }
        const std::string core = quote_ == 0 ? CoreHead(h) : "";
        // (catch Class e body) in a library's try macro binds e all the same.
        if (quote_ == 0 && syntax_quote_ == 0 && core.empty() && N(h).text == "catch" && Lookup("catch") == -1 &&
            !a_.var_index.count("catch") && kids.size() >= 3 && IsSym(N(kids[2]))) {
            Symbol(h);
            a_.occs.back().target = Target::Special;
            Form(kids[1]);
            Push();
            Bind(kids[2], "catch", N(k).range);
            for (size_t p = 3; p < kids.size(); p++) Form(kids[p]);
            Pop();
            return;
        }
        // clojure.test's `is` knows (thrown? C body) and (thrown-with-msg? C re body).
        if (quote_ == 0 && core.empty() && (N(h).text == "thrown?" || N(h).text == "thrown-with-msg?") &&
            Lookup(N(h).text) == -1 && !a_.var_index.count(N(h).text)) {
            Symbol(h);
            a_.occs.back().target = Target::Special;
            for (size_t p = 1; p < kids.size(); p++) Form(kids[p]);
            return;
        }
        if (quote_ == 0 && syntax_quote_ == 0 && Special(k, kids, core, inserted)) return;
        // A call.
        Symbol(h);
        const Occurrence &head = a_.occs.back();
        const bool maybe_macro = MaybeMacro(head);
        if (maybe_macro && quote_ == 0) {
            unknown_++;
            Push();
            SoftBind(kids);
            for (size_t p = 1; p < kids.size(); p++) Form(kids[p]);
            Pop();
            unknown_--;
        } else {
            for (size_t p = 1; p < kids.size(); p++) Form(kids[p]);
        }
        if (quote_ == 0 && syntax_quote_ == 0) Call(k, kids, inserted);
    }

    bool MaybeMacro(const Occurrence &o) const {
        switch (o.target) {
            case Target::Unresolved:
            case Target::OtherVar: return true;
            case Target::FileVar: {
                const std::string &kind = a_.vars[static_cast<size_t>(o.var)].kind;
                return kind == "defmacro" || kind == "def-macro";
            }
            case Target::ProjectVar:
                if (a_.project) {
                    for (const CljProjectVar &v : a_.project->vars) {
                        if (v.ns == o.ns && v.name == o.name) return v.kind == "macro";
                    }
                }
                return true;
            default: return false;
        }
    }

    // An unknown macro's vectors: the names it may bind.
    void SoftBind(const std::vector<int> &kids) {
        std::function<void(int)> collect = [&](int x) {
            const CljNode &n = N(x);
            if (n.kind == CljNodeKind::Symbol) {
                if (n.ns.empty() && n.text != "&" && Lookup(n.text) < 0 && !a_.var_index.count(n.text) &&
                    !PublicCore(n.text) && !CljLspCoreVar("", n.text)) {
                    const int b = Bind(x, "macro", N(kids[0]).range, true);
                    a_.bindings[static_cast<size_t>(b)].soft = true;
                    a_.occs.pop_back();  // not a definition site we know
                }
                return;
            }
            if (n.kind == CljNodeKind::Vector || n.kind == CljNodeKind::Map) {
                for (int y : Kids(x)) collect(y);
            }
        };
        for (size_t p = 1; p < kids.size(); p++) {
            if (N(kids[p]).kind == CljNodeKind::Vector) collect(kids[p]);
        }
    }

    // --- clojure.core's forms ------------------------------------------------------------

    CljRange Rest(int form, int from_node) const {
        CljRange r = N(form).range;
        const CljRange &f = N(from_node).range;
        r.line = f.end_line;
        r.col = f.end_col;
        return r;
    }

    bool Special(int k, const std::vector<int> &kids, const std::string &core, int inserted) {
        if (core.empty()) return false;
        const int h = kids[0];
        auto head_occ = [&]() { Symbol(h); };
        const size_t n = kids.size();
        if (core == "quote") {
            head_occ();
            quote_++;
            for (size_t p = 1; p < n; p++) Form(kids[p]);
            quote_--;
            return true;
        }
        if (core == "comment") {
            head_occ();
            a_.comment_forms.push_back(k);
            comment_++;
            for (size_t p = 1; p < n; p++) Form(kids[p]);
            comment_--;
            return true;
        }
        if (core == "ns" || core == "gen-class" || core == "gen-interface") {
            head_occ();
            if (core == "ns") {
                // Only metadata and references: the names were taken in Namespace().
                for (size_t p = 2; p < n; p++) {
                    const std::vector<int> ck = Kids(kids[p]);
                    if (N(kids[p]).kind != CljNodeKind::List || ck.empty()) continue;
                    // :refer'd names and :as aliases are definitions of a sort: record them for navigation.
                }
            }
            return true;
        }
        if (core == "def" || core == "defonce") {
            head_occ();
            if (core == "def") {
                if (n < 2) Shape(k, "def-shape", "Too few arguments to def");
                else if (n > 4) Shape(k, "def-shape", "Too many arguments to def");
            }
            if (n >= 2 && IsSym(N(Struct(kids[1])))) Definition(Struct(kids[1]));
            if (fn_ > 0 && comment_ == 0 && unknown_ == 0) Shape(k, "inline-def", "`def` inside a function body redefines a var each call -- use `let`", CljLspSeverity::Warning);
            for (size_t p = 2; p < n; p++) Form(kids[p]);
            return true;
        }
        if (core == "defn" || core == "defn-" || core == "defmacro" || core == "definline") {
            head_occ();
            if (n < 2 || !IsSym(N(Struct(kids[1])))) {
                if (n >= 2) Shape(kids[1], "defn-shape", "First argument to " + core + " must be a symbol");
                return true;
            }
            Definition(Struct(kids[1]));
            size_t p = 2;
            if (p < n && N(Struct(kids[p])).kind == CljNodeKind::String) {
                // A string after the params is a body, not the docstring.
                p++;
            }
            if (p < n && N(Struct(kids[p])).kind == CljNodeKind::Map) {
                Form(kids[p]);
                p++;
            }
            if (p >= n) {
                Shape(kids[1], "defn-shape", "Parameter declaration missing");
                return true;
            }
            FnTail(k, kids, p, core == "defmacro", "param");
            return true;
        }
        if (core == "fn" || core == "fn*" || core == "bound-fn") {
            head_occ();
            Push();
            size_t p = 1;
            if (p < n && IsSym(N(kids[p]))) {
                const int b = Bind(kids[p], "fn-name", N(k).range);
                a_.bindings[static_cast<size_t>(b)].exempt = true;
                p++;
            }
            if (p >= n) {
                Shape(k, "fn-shape", "Parameter declaration missing");
            } else if (N(Struct(kids[p])).kind != CljNodeKind::Vector && N(Struct(kids[p])).kind != CljNodeKind::List) {
                Shape(kids[p], "fn-shape", "Parameter declaration " + FirstLine(NodeText(a_, kids[p])) + " should be a vector");
            } else {
                FnTail(k, kids, p, false, "param");
            }
            Pop();
            (void)inserted;
            return true;
        }
        if (core == "let" || core == "loop" || core == "let*" || core == "loop*" || core == "with-open" ||
            core == "with-local-vars" || core == "dotimes" || core == "when-let" || core == "if-let" ||
            core == "when-some" || core == "if-some" || core == "when-first" || core == "with-redefs" ||
            core == "binding") {
            head_occ();
            const bool binds_locals = core != "with-redefs" && core != "binding";
            if (n < 2 || N(Struct(kids[1])).kind != CljNodeKind::Vector) {
                Shape(n < 2 ? k : kids[1], "binding-shape", core + " requires a vector for its binding");
                for (size_t p = 1; p < n; p++) Form(kids[p]);
                return true;
            }
            // A #? binding vector: every platform's bindings are in scope (for
            // the analysis; the checks below look at the JVM's).
            std::vector<int> bv = Kids(Struct(kids[1]));
            if (N(kids[1]).kind == CljNodeKind::ReadCond) {
                for (int b : Branches(kids[1])) {
                    if (b == Struct(kids[1]) || N(b).kind != CljNodeKind::Vector) continue;
                    for (int x : Kids(b)) bv.push_back(x);
                }
            }
            if (Kids(Struct(kids[1])).size() % 2 != 0) {
                Shape(kids[1], "binding-shape", core + " requires an even number of forms in binding vector");
            }
            const bool one_pair = core == "when-let" || core == "if-let" || core == "when-some" || core == "if-some" ||
                                  core == "when-first" || core == "dotimes";
            if (one_pair && Kids(Struct(kids[1])).size() != 2 && Kids(Struct(kids[1])).size() % 2 == 0) {
                Shape(kids[1], "binding-shape", core + " requires exactly 2 forms in binding vector");
            }
            if ((core == "if-let" || core == "if-some") && (n < 3 || n > 4)) {
                Shape(k, "if-shape", n < 3 ? "Too few arguments to " + core : "Too many arguments to " + core);
            }
            Push();
            const CljRange scope = N(k).range;
            for (size_t p = 0; p + 1 < bv.size(); p += 2) {
                Form(bv[p + 1]);
                if (binds_locals) {
                    const bool exempt = core == "with-open" ? false : false;
                    Destructure(bv[p], core == "loop" || core == "loop*" ? "loop" : "let", scope, exempt);
                } else {
                    Form(bv[p]);  // binding / with-redefs: the names are vars
                }
            }
            // if-let's else branch does not see the binding; close enough to walk it inside.
            for (size_t p = 2; p < n; p++) Form(kids[p]);
            Pop();
            return true;
        }
        if (core == "letfn" || core == "letfn*") {
            head_occ();
            Push();
            if (n >= 2) {
                const std::vector<int> fns = Kids(kids[1]);
                for (int f : fns) {
                    const std::vector<int> fk = Kids(f);
                    if (!fk.empty() && IsSym(N(fk[0]))) Bind(fk[0], "letfn", N(k).range);
                }
                for (int f : fns) {
                    const std::vector<int> fk = Kids(f);
                    if (fk.size() >= 2) {
                        Push();
                        FnTail(f, fk, 1, false, "param");
                        Pop();
                    }
                }
            }
            for (size_t p = 2; p < n; p++) Form(kids[p]);
            Pop();
            return true;
        }
        if (core == "for" || core == "doseq") {
            head_occ();
            if (n < 2 || N(kids[1]).kind != CljNodeKind::Vector) {
                Shape(n < 2 ? k : kids[1], "binding-shape", core + " requires a vector for its binding");
                for (size_t p = 1; p < n; p++) Form(kids[p]);
                return true;
            }
            Push();
            Comprehension(kids[1], N(k).range, core);
            for (size_t p = 2; p < n; p++) Form(kids[p]);
            Pop();
            return true;
        }
        if (core == "if") {
            head_occ();
            if (n < 3) Shape(k, "if-shape", "Too few arguments to if");
            else if (n > 4) Shape(k, "if-shape", "Too many arguments to if");
            for (size_t p = 1; p < n; p++) Form(kids[p]);
            return true;
        }
        if (core == "case") {
            head_occ();
            if (n >= 2) Form(kids[1]);
            std::vector<std::string> seen;
            for (size_t p = 2; p < n; p += 2) {
                if (p + 1 >= n) {  // the default
                    Form(kids[p]);
                    break;
                }
                // Test constants are not evaluated: a list is a set of alternatives.
                std::vector<int> tests;
                if (N(kids[p]).kind == CljNodeKind::List) {
                    tests = Kids(kids[p]);
                } else {
                    tests.push_back(kids[p]);
                }
                for (int t : tests) {
                    const std::string key = NodeText(a_, t);
                    if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                        Shape(t, "duplicate-case-test", "Duplicate case test constant: " + key);
                    }
                    seen.push_back(key);
                    if (N(t).kind == CljNodeKind::Keyword) a_.keywords.push_back(t);
                }
                Form(kids[p + 1]);
            }
            return true;
        }
        if (core == "try") {
            head_occ();
            // A catch clause may sit in a reader conditional.
            std::vector<int> clauses;
            for (size_t p = 1; p < n; p++) {
                if (N(kids[p]).kind == CljNodeKind::ReadCond || N(kids[p]).kind == CljNodeKind::ReadCondSplicing) {
                    bool catches = false;
                    for (int b : Branches(kids[p])) {
                        const std::vector<int> bk = Kids(b);
                        if (N(b).kind == CljNodeKind::List && !bk.empty() && IsSym(N(bk[0])) &&
                            (N(bk[0]).text == "catch" || N(bk[0]).text == "finally")) catches = true;
                    }
                    if (catches) {
                        for (int b : Branches(kids[p])) clauses.push_back(b);
                        continue;
                    }
                }
                clauses.push_back(kids[p]);
            }
            for (size_t p = 0; p < clauses.size(); p++) {
                const std::vector<int> &kids_ = clauses;
                (void)kids_;
                const std::vector<int> ck = Kids(clauses[p]);
                if (N(clauses[p]).kind == CljNodeKind::List && !ck.empty() && IsSym(N(ck[0])) &&
                    (N(ck[0]).text == "catch" || N(ck[0]).text == "finally")) {
                    Symbol(ck[0]);
                    a_.occs.back().target = Target::Special;
                    if (N(ck[0]).text == "catch") {
                        if (ck.size() > 1) Form(ck[1]);  // the class
                        Push();
                        if (ck.size() > 2 && IsSym(N(ck[2]))) {
                            const int b = Bind(ck[2], "catch", N(clauses[p]).range);
                            (void)b;
                        }
                        for (size_t q = 3; q < ck.size(); q++) Form(ck[q]);
                        Pop();
                    } else {
                        for (size_t q = 1; q < ck.size(); q++) Form(ck[q]);
                    }
                    continue;
                }
                Form(clauses[p]);
            }
            return true;
        }
        if (core == "areduce" || core == "amap") {
            // (areduce a idx ret init expr), (amap a idx ret expr)
            head_occ();
            if (n >= 2) Form(kids[1]);
            Push();
            const CljRange scope = N(k).range;
            if (n >= 3 && IsSym(N(kids[2]))) Bind(kids[2], "let", scope);
            if (core == "areduce" && n >= 5) Form(kids[4]);
            if (n >= 4 && IsSym(N(kids[3]))) Bind(kids[3], "let", scope);
            for (size_t p = core == "areduce" ? 5 : 4; p < n; p++) Form(kids[p]);
            Pop();
            return true;
        }
        if (core == "as->") {
            head_occ();
            // As a step of a threading form, the expression is implicit.
            const size_t name_at = inserted > 0 ? 1 : 2;
            if (inserted == 0 && n >= 2) Form(kids[1]);
            Push();
            if (n > name_at && IsSym(N(kids[name_at]))) {
                const int b = Bind(kids[name_at], "let", N(k).range);
                a_.bindings[static_cast<size_t>(b)].exempt = true;
            }
            for (size_t p = name_at + 1; p < n; p++) Form(kids[p]);
            Pop();
            return true;
        }
        if (core == "->" || core == "->>" || core == "some->" || core == "some->>" || core == "doto" ||
            core == "cond->" || core == "cond->>") {
            head_occ();
            // A step of another threading form gets its expression from it:
            // (-> x (cond-> test (assoc :k v))).
            const size_t first_step = inserted > 0 ? 1 : 2;
            if (inserted == 0 && n >= 2) Form(kids[1]);
            const bool cond = core == "cond->" || core == "cond->>";
            for (size_t p = first_step; p < n; p++) {
                if (cond && (p - first_step) % 2 == 0) {  // a test
                    Form(kids[p]);
                    continue;
                }
                const CljNode &step = N(kids[p]);
                if (step.kind == CljNodeKind::Symbol) {
                    // (-> x f) is (f x)
                    Symbol(kids[p]);
                    ::clj::Call c;
                    c.node = kids[p];
                    c.head = static_cast<int>(a_.occs.size()) - 1;
                    c.argc = 1;
                    c.checked = Checked();
                    a_.calls.push_back(c);
                    continue;
                }
                if (step.kind == CljNodeKind::List) insert_ = 1;
                Form(kids[p]);
                insert_ = 0;
            }
            return true;
        }
        if (core == "proxy-super") {
            head_occ();
            for (size_t p = 2; p < n; p++) Form(kids[p]);  // the first is a method name
            return true;
        }
        if (core == "var") {
            head_occ();
            const size_t first = a_.occs.size();
            for (size_t p = 1; p < n; p++) Form(kids[p]);
            for (size_t q = first; q < a_.occs.size(); q++) {
                if (a_.occs[q].target == Target::Unresolved && !a_.occs[q].alias_unknown && !a_.occs[q].ns.empty()) {
                    a_.occs[q].checked = false;
                }
                a_.occs[q].not_public = false;  // (var ns/private) is how a private var is reached
            }
            return true;
        }
        if (core == "." || core == ".." || core == "new" || core == "set!") {
            head_occ();
            // (. obj method args), (.. obj (m1) m2), (new Class args)
            for (size_t p = 1; p < n; p++) {
                const CljNode &x = N(kids[p]);
                // The member name: (. obj member args), and every symbol of (.. obj m1 m2).
                if (x.kind == CljNodeKind::Symbol && ((core == "." && p == 2) || (core == ".." && p >= 2))) continue;
                if (p == 1 && core == "new") {
                    Symbol(kids[p]);
                    a_.occs.back().target = Target::Class;
                    continue;
                }
                if (x.kind == CljNodeKind::List && core != "set!" && (p == 2 || core == "..")) {  // (. obj (method args))
                    const std::vector<int> mk = Kids(kids[p]);
                    for (size_t q = 1; q < mk.size(); q++) Form(mk[q]);
                    continue;
                }
                Form(kids[p]);
            }
            return true;
        }
        if (core == "defprotocol" || core == "definterface") {
            head_occ();
            if (n >= 2 && IsSym(N(Struct(kids[1])))) Definition(Struct(kids[1]));
            for (size_t p = 2; p < n; p++) {
                const std::vector<int> sig = Kids(kids[p]);
                if (N(kids[p]).kind == CljNodeKind::List && !sig.empty() && IsSym(N(sig[0])) && core == "defprotocol") {
                    Definition(sig[0]);
                }
            }
            return true;
        }
        if (core == "defrecord" || core == "deftype") {
            head_occ();
            if (n >= 2 && IsSym(N(Struct(kids[1])))) Definition(Struct(kids[1]));
            Push();
            if (n >= 3 && (N(kids[2]).kind == CljNodeKind::Vector || N(kids[2]).kind == CljNodeKind::ReadCond)) {
                std::vector<int> fields;
                if (N(kids[2]).kind == CljNodeKind::ReadCond) {
                    for (int b : Branches(kids[2])) {
                        for (int f : Kids(b)) fields.push_back(f);
                    }
                } else {
                    fields = Kids(kids[2]);
                }
                // A field may be a #? of its own (`#?(:clj ^:volatile-mutable root :cljs ^:mutable root)`).
                std::vector<int> flat;
                for (int f : fields) {
                    if (N(f).kind == CljNodeKind::ReadCond) {
                        for (int b : Branches(f)) flat.push_back(b);
                    } else {
                        flat.push_back(f);
                    }
                }
                for (int f : flat) {
                    if (!IsSym(N(f))) continue;
                    const int b = Bind(f, "field", N(k).range);
                    a_.bindings[static_cast<size_t>(b)].exempt = true;
                }
            }
            Impls(kids, 3);
            Pop();
            return true;
        }
        if (core == "reify" || core == "extend-type" || core == "extend-protocol" || core == "specify" ||
            core == "specify!") {
            head_occ();
            if (core != "reify" && n >= 2) Form(kids[1]);
            Impls(kids, core == "reify" ? 1 : 2);
            return true;
        }
        if (core == "proxy") {
            head_occ();
            if (n >= 2) Form(kids[1]);
            if (n >= 3) Form(kids[2]);
            Push();
            // Every proxy method has an implicit `this`.
            for (size_t p = 3; p < n; p++) {
                const std::vector<int> mk = Kids(kids[p]);
                if (N(kids[p]).kind != CljNodeKind::List || mk.empty()) {
                    Form(kids[p]);
                    continue;
                }
                Push();
                scopes_.back().names["this"] = -2;
                FnTail(kids[p], mk, 1, false, "impl");
                Pop();
            }
            Pop();
            return true;
        }
        if (core == "defmulti") {
            head_occ();
            if (n >= 2 && IsSym(N(Struct(kids[1])))) Definition(Struct(kids[1]));
            for (size_t p = 2; p < n; p++) Form(kids[p]);
            return true;
        }
        if (core == "defmethod") {
            head_occ();
            if (n >= 2) Form(kids[1]);
            if (n >= 3) Form(kids[2]);
            if (n >= 4) {
                Push();
                size_t p = 3;
                if (IsSym(N(kids[p]))) {
                    Bind(kids[p], "fn-name", N(k).range);
                    a_.bindings.back().exempt = true;
                    p++;
                }
                if (p < n) FnTail(k, kids, p, false, "impl");
                Pop();
            }
            return true;
        }
        if (core == "declare") {
            head_occ();
            for (size_t p = 1; p < n; p++) {
                if (IsSym(N(kids[p]))) Definition(kids[p]);
            }
            return true;
        }
        if (core == "deftest") {
            head_occ();
            if (n >= 2 && IsSym(N(Struct(kids[1])))) Definition(Struct(kids[1]));
            for (size_t p = 2; p < n; p++) Form(kids[p]);
            return true;
        }
        if (core == "defstruct") {
            head_occ();
            if (n >= 2 && IsSym(N(Struct(kids[1])))) Definition(Struct(kids[1]));
            return true;
        }
        return false;
    }

    // A def's name: an occurrence that is the definition.
    void Definition(int name) {
        const CljNode &n = N(name);
        Occurrence o;
        o.text = n.text;
        o.node = name;
        o.range = n.range;
        o.top = top_;
        o.is_definition = true;
        o.checked = false;
        o.ns = a_.ns_name;
        o.name = n.text;
        // The var this defines: the one recorded for this node.
        for (size_t k = 0; k < a_.vars.size(); k++) {
            if (a_.vars[k].name_node == name && a_.vars[k].name == n.text) {
                o.target = Target::FileVar;
                o.var = static_cast<int>(k);
                break;
            }
        }
        a_.occs.push_back(o);
    }

    // [params] body, or ([params] body)+ -- from kids[p].
    void FnTail(int form, const std::vector<int> &kids, size_t p, bool macro, const std::string &kind) {
        const int saved_fn = fn_;
        fn_++;
        if (p < kids.size() && N(Struct(kids[p])).kind == CljNodeKind::Vector) {
            Arity(form, kids, p, macro, kind);
        } else {
            for (; p < kids.size(); p++) {
                const int arity = Struct(kids[p]);
                const std::vector<int> ar = Kids(arity);
                if (N(arity).kind == CljNodeKind::List && !ar.empty() && N(Struct(ar[0])).kind == CljNodeKind::Vector) {
                    // Every branch's arity, when they differ by platform.
                    if (arity != kids[p]) {
                        for (int b : Branches(kids[p])) {
                            const std::vector<int> bk = Kids(b);
                            if (N(b).kind == CljNodeKind::List && !bk.empty()) Arity(b, bk, 0, macro, kind);
                        }
                    } else {
                        Arity(kids[p], ar, 0, macro, kind);
                    }
                } else if (N(arity).kind == CljNodeKind::Map) {
                    Form(kids[p]);  // a trailing attr-map
                } else {
                    Shape(kids[p], "fn-shape", "Parameter declaration " + FirstLine(NodeText(a_, kids[p])) + " should be a vector");
                    Form(kids[p]);
                }
            }
        }
        fn_ = saved_fn;
    }

    void Arity(int form, const std::vector<int> &kids, size_t p, bool macro, const std::string &kind) {
        Push();
        const CljRange scope = N(form).range;
        if (macro) {
            // &form and &env: every macro has them.
            scopes_.back().names["&form"] = -2;
            scopes_.back().names["&env"] = -2;
        }
        Destructure(kids[p], kind, scope, kind == "impl");
        for (size_t q = p + 1; q < kids.size(); q++) Form(kids[q]);
        Pop();
    }

    // Protocol/interface implementations: (method [this args] body), with
    // names (protocols, classes) between them.
    void Impls(const std::vector<int> &in, size_t from) {
        std::vector<int> kids(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(std::min(from, in.size())));
        for (size_t p = from; p < in.size(); p++) {
            if (N(in[p]).kind == CljNodeKind::ReadCond) {
                for (int b : Branches(in[p])) kids.push_back(b);
            } else {
                kids.push_back(in[p]);
            }
        }
        for (size_t p = from; p < kids.size(); p++) {
            const CljNode &x = N(kids[p]);
            if (x.kind == CljNodeKind::Symbol) {
                Symbol(kids[p]);
                Occurrence &o = a_.occs.back();
                if (o.target == Target::Unresolved) o.target = Target::Class;
                continue;
            }
            if (x.kind == CljNodeKind::Keyword) {  // deftype options: :load-ns true
                if (p + 1 < kids.size()) p++;
                continue;
            }
            const std::vector<int> mk = Kids(kids[p]);
            if (x.kind != CljNodeKind::List || mk.empty()) {
                Form(kids[p]);
                continue;
            }
            // (Class/forName "[B") names a type: a method has a parameter
            // vector (or arities) after its name.
            {
                const int second = mk.size() > 1 ? Struct(mk[1]) : -1;
                const bool method = second >= 0 && (N(second).kind == CljNodeKind::Vector ||
                                                    (N(second).kind == CljNodeKind::List && !Kids(second).empty() &&
                                                     N(Struct(Kids(second)[0])).kind == CljNodeKind::Vector));
                if (!method) {
                    Form(kids[p]);
                    continue;
                }
            }
            // The method name is the protocol's: an occurrence of its var when it is this file's.
            if (IsSym(N(mk[0]))) {
                const auto it = a_.var_index.find(N(mk[0]).text);
                if (it != a_.var_index.end() && a_.vars[static_cast<size_t>(it->second)].kind == "protocol-method") {
                    Occurrence o;
                    o.text = N(mk[0]).text;
                    o.node = mk[0];
                    o.range = N(mk[0]).range;
                    o.target = Target::FileVar;
                    o.var = it->second;
                    o.top = top_;
                    o.checked = false;
                    a_.occs.push_back(o);
                    a_.vars[static_cast<size_t>(it->second)].uses++;
                }
            }
            Push();
            FnTail(kids[p], mk, 1, false, "impl");
            Pop();
        }
    }

    // for/doseq bindings: name coll pairs, and :let/:when/:while.
    void Comprehension(int vec, const CljRange &scope, const std::string &core) {
        const std::vector<int> bv = Kids(vec);
        if (bv.size() % 2 != 0) Shape(vec, "binding-shape", core + " requires an even number of forms in binding vector");
        for (size_t p = 0; p + 1 < bv.size(); p += 2) {
            const CljNode &x = N(bv[p]);
            if (x.kind == CljNodeKind::Keyword) {
                if (x.name == "let") {
                    const std::vector<int> lv = Kids(bv[p + 1]);
                    for (size_t q = 0; q + 1 < lv.size(); q += 2) {
                        Form(lv[q + 1]);
                        Destructure(lv[q], "let", scope);
                    }
                } else {
                    Form(bv[p + 1]);  // :when, :while
                }
                continue;
            }
            Form(bv[p + 1]);
            Destructure(bv[p], "for", scope);
        }
    }
};

}  // namespace

Analysis Analyze(const std::vector<std::string> &lines, const std::string &path, const CljProject *project) {
    Analysis a;
    a.lines = lines;
    a.path = path;
    a.ext = Extension(path);
    a.project = project;
    a.read = CljRead(lines);
    Walker w(a);
    w.Run();
    // The name checks need a file that is its own context: not one `load`ed
    // into another namespace, not Clojure's own sources (clojure.core is
    // bootstrapped from Java), not a Leiningen project.clj.
    if (a.has_in_ns) a.clj_checks = false;
    if (!CljLspNamespaceVars(a.ns_name).empty() || a.ns_name == "clojure.core") {
        a.clj_checks = false;
        a.own_vocab_ns = true;
    }
    {
        const size_t slash = path.find_last_of('/');
        const std::string base = path.substr(slash == std::string::npos ? 0 : slash + 1);
        if (base == "project.clj" || base.compare(0, 13, "data_readers.") == 0) a.clj_checks = false;
        // A file holding only maps and vectors (cljfmt's indent tables) is read as data.
        if (a.ns_form < 0 && !a.read.top.empty()) {
            bool data = true;
            for (int t : a.read.top) {
                const CljNodeKind k = a.read.at(t).kind;
                if (k != CljNodeKind::Map && k != CljNodeKind::Vector && k != CljNodeKind::Set) data = false;
            }
            if (data) a.clj_checks = false;
        }
    }
    for (const std::string &ns : a.refer_all) {
        if (CljLspNamespaceVars(ns).empty() && !(project && project->ns_paths.count(ns))) a.clj_checks = false;
    }
    return a;
}

int OccurrenceAt(const Analysis &a, int line, int col) {
    int best = -1;
    for (size_t k = 0; k < a.occs.size(); k++) {
        if (CljRangeTouches(a.occs[k].range, line, col)) {
            best = static_cast<int>(k);
            if (CljRangeContains(a.occs[k].range, line, col)) return best;
        }
    }
    return best;
}

int ListAt(const Analysis &a, int line, int col) {
    int best = -1;
    for (size_t k = 0; k < a.read.nodes.size(); k++) {
        const CljNode &n = a.read.nodes[k];
        if (n.kind != CljNodeKind::List && n.kind != CljNodeKind::Fn) continue;
        CljRange inner = n.range;
        inner.col += 1;  // after the opening paren
        // An unclosed list (typing at the end of the file) contains its end too.
        const bool at_open_end = line == n.range.end_line && col == n.range.end_col &&
                                 NodeText(a, static_cast<int>(k)).back() != ')';
        if ((!CljRangeContains(n.range, line, col) && !at_open_end) || (line == n.range.line && col == n.range.col)) continue;
        if (best < 0 || Contains(a.read.nodes[static_cast<size_t>(best)].range, n.range)) best = static_cast<int>(k);
    }
    return best;
}

std::vector<int> LocalsAt(const Analysis &a, int line, int col) {
    std::vector<int> out;
    for (size_t k = a.bindings.size(); k-- > 0;) {
        const Binding &b = a.bindings[k];
        if (b.soft) continue;
        if (!CljRangeContains(b.scope, line, col)) continue;
        if (Before(CljRange{line, col, line, col}, CljRange{b.range.end_line, b.range.end_col, 0, 0})) continue;
        bool shadowed = false;
        for (int o : out) {
            if (a.bindings[static_cast<size_t>(o)].name == b.name) shadowed = true;
        }
        if (!shadowed) out.push_back(static_cast<int>(k));
    }
    return out;
}

std::vector<int> FileVarsNamed(const Analysis &a, const std::string &name) {
    std::vector<int> out;
    const auto range = a.var_index.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) out.push_back(it->second);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace clj
