#include "clojure_format.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "clojure_reader.h"

// cljfmt's rules (cljfmt.core, 0.16): the whitespace passes work on the
// gaps between tokens; indentation works line by line, top down, on the
// already re-indented text, as cljfmt's zipper walk does.

namespace cljfmt {
namespace {

bool IsWs(char c) { return c == ' ' || c == '\t' || c == ',' || c == '\n' || c == '\r' || c == '\f'; }

std::vector<std::string> SplitLines(const std::string &text) {
    std::vector<std::string> lines;
    size_t p = 0;
    for (;;) {
        const size_t nl = text.find('\n', p);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(p));
            break;
        }
        lines.push_back(text.substr(p, nl - p));
        p = nl + 1;
    }
    return lines;
}

// Offsets <-> (line, col) over a text.
struct Text {
    std::string s;
    std::vector<size_t> starts;
    explicit Text(std::string t) : s(std::move(t)) {
        starts.push_back(0);
        for (size_t k = 0; k < s.size(); k++) {
            if (s[k] == '\n') starts.push_back(k + 1);
        }
    }
    size_t Off(int line, int col) const {
        if (line < 0) return 0;
        if (static_cast<size_t>(line) >= starts.size()) return s.size();
        return std::min(s.size(), starts[static_cast<size_t>(line)] + static_cast<size_t>(col));
    }
    int Line(size_t off) const {
        return static_cast<int>(std::upper_bound(starts.begin(), starts.end(), off) - starts.begin()) - 1;
    }
    int Col(size_t off) const { return static_cast<int>(off - starts[static_cast<size_t>(Line(off))]); }
};

enum class TK { Open, Close, Atom, Prefix, Comment, Discard };

struct Tok {
    size_t a = 0, b = 0;
    TK k = TK::Atom;
    int node = -1;
};

bool IsCollection(CljNodeKind k) {
    switch (k) {
        case CljNodeKind::List:
        case CljNodeKind::Vector:
        case CljNodeKind::Map:
        case CljNodeKind::Set:
        case CljNodeKind::Fn:
        case CljNodeKind::ReadCond:
        case CljNodeKind::ReadCondSplicing:
        case CljNodeKind::NsMap: return true;
        default: return false;
    }
}

struct Tree {
    const CljReadResult &r;
    const Text &t;
    std::vector<Tok> toks;
    std::vector<size_t> visual;  // per node: where it starts, its metadata included

    size_t Start(int k) const { return t.Off(r.at(k).range.line, r.at(k).range.col); }
    size_t End(int k) const { return t.Off(r.at(k).range.end_line, r.at(k).range.end_col); }

    Tree(const CljReadResult &rr, const Text &tt) : r(rr), t(tt) {
        visual.assign(r.nodes.size(), 0);
        for (size_t k = 0; k < r.nodes.size(); k++) visual[k] = Start(static_cast<int>(k));
        for (int top : r.top) Collect(top);
        for (int d : r.discarded) Collect(d);
        for (const CljComment &c : r.comments) {
            toks.push_back(Tok{t.Off(c.range.line, c.range.col), t.Off(c.range.end_line, c.range.end_col), TK::Comment, -1});
        }
        std::sort(toks.begin(), toks.end(), [](const Tok &x, const Tok &y) { return x.a < y.a; });
    }

    void Collect(int node) {
        const CljNode &n = r.at(node);
        size_t vis = Start(node);
        for (int m : n.meta) {
            // The caret before the metadata form (whitespace may follow it).
            size_t c = Start(m);
            while (c > 0 && IsWs(t.s[c - 1])) c--;
            if (c > 0) c--;
            const size_t ca = c > 0 && t.s[c] == '^' && t.s[c - 1] == '#' ? c - 1 : c;
            toks.push_back(Tok{ca, c + 1, TK::Prefix, m});
            vis = std::min(vis, ca);
            Collect(m);
        }
        visual[static_cast<size_t>(node)] = vis;
        const size_t a = Start(node), b = End(node);
        if (IsCollection(n.kind)) {
            size_t oe = a;
            while (oe < b && t.s[oe] != '(' && t.s[oe] != '[' && t.s[oe] != '{') oe++;
            toks.push_back(Tok{a, oe + 1, TK::Open, node});
            for (int kid : n.kids) Collect(kid);
            toks.push_back(Tok{b - 1, b, TK::Close, node});
            return;
        }
        switch (n.kind) {
            case CljNodeKind::Quote:
            case CljNodeKind::SyntaxQuote:
            case CljNodeKind::Unquote:
            case CljNodeKind::UnquoteSplicing:
            case CljNodeKind::Deref:
            case CljNodeKind::Var:
            case CljNodeKind::ReadEval:
            case CljNodeKind::Tagged:
                toks.push_back(Tok{a, a + n.text.size(), TK::Prefix, node});
                for (int kid : n.kids) Collect(kid);
                return;
            default: toks.push_back(Tok{a, b, TK::Atom, node}); return;
        }
    }

    // Splits the gaps' "#_" markers into tokens; false if a gap holds anything else.
    bool Markers() {
        std::vector<Tok> extra;
        size_t prev = 0;
        for (size_t k = 0; k <= toks.size(); k++) {
            const size_t next = k < toks.size() ? toks[k].a : t.s.size();
            for (size_t p = prev; p < next; p++) {
                if (IsWs(t.s[p]) || static_cast<unsigned char>(t.s[p]) < 0x20) continue;
                if (t.s.compare(p, 2, "#_") == 0) {
                    extra.push_back(Tok{p, p + 2, TK::Discard, -1});
                    p++;
                    continue;
                }
                return false;
            }
            if (k < toks.size()) prev = std::max(prev, toks[k].b);
        }
        toks.insert(toks.end(), extra.begin(), extra.end());
        std::sort(toks.begin(), toks.end(), [](const Tok &x, const Tok &y) { return x.a < y.a; });
        return true;
    }
};

// --- the whitespace passes ------------------------------------------------------------

bool EndsElement(TK k) { return k == TK::Atom || k == TK::Close; }
bool StartsElement(TK k) { return k == TK::Open || k == TK::Atom || k == TK::Prefix || k == TK::Discard; }

std::string FixGap(const std::string &gap, const Tok &prev, const Tok &next, const std::string &prev_text,
                   const std::string &next_text, bool top_level) {
    if (prev.k == TK::Open && next.k != TK::Comment) return "";
    // Nothing between a reader macro and its form (`@ (x)` is `@(x)`, `#_`
    // newline `(f)` is `#_(f)`) -- but `~ @x` must not become `~@x`, and a
    // tagged literal's tag keeps its space.
    if ((prev.k == TK::Prefix || prev.k == TK::Discard) && next.k != TK::Comment && prev_text[0] != '#') {
        if (prev_text == "~" && !next_text.empty() && next_text[0] == '@') return " ";
        return "";
    }
    if ((prev.k == TK::Prefix || prev.k == TK::Discard) && next.k != TK::Comment &&
        (prev_text == "#_" || prev_text == "#'" || prev_text == "#=" || prev_text == "#^")) {
        return "";
    }
    if (next.k == TK::Close && prev.k != TK::Comment) return "";
    if (next.k == TK::Close && prev.k == TK::Comment) return "\n";
    if (gap.find('\n') == std::string::npos) {
        if (gap.empty() && EndsElement(prev.k) && StartsElement(next.k)) return " ";
        return gap;
    }
    // Lines: trailing blanks dropped; a run of blank lines is one, and the
    // line after it starts over at column 0 (cljfmt rewrites the run) --
    // except after a top-level comment, where cljfmt keeps them all.
    std::string out;
    std::string seg;
    int newlines = 0;
    for (char c : gap) {
        if (c == '\n') {
            while (!seg.empty() && (seg.back() == ' ' || seg.back() == '\t')) seg.pop_back();
            if (newlines == 0) out += seg;  // a comma left on the line keeps it
            seg.clear();
            newlines++;
            continue;
        }
        seg += c;
    }
    if (newlines > 2 && !(prev.k == TK::Comment && next.k != TK::Comment && top_level)) return out + "\n\n";
    return out + std::string(static_cast<size_t>(newlines), '\n') + seg;
}

// --- indentation ---------------------------------------------------------------------------

struct Rule {
    std::string key;     // a bare symbol name, or a regex when `pattern`
    bool pattern = false;
    std::vector<std::vector<int>> specs;  // {0 /*inner*/, depth, idx or -1} or {1 /*block*/, idx}
    int order_depth = 0;
};

const std::vector<Rule> &Rules() {
    static const std::vector<Rule> rules = [] {
        std::vector<Rule> v;
        auto inner0 = [&](const char *k) { v.push_back(Rule{k, false, {{0, 0, -1}}, 0}); };
        auto block = [&](const char *k, int idx) { v.push_back(Rule{k, false, {{1, idx}}, 0}); };
        for (const char *k : {"alt!", "alt!!", "comment", "cond", "delay", "do", "finally", "future", "go", "thread", "try",
                              "with-out-str"}) {
            block(k, 0);
        }
        for (const char *k : {"binding", "case", "cond->", "cond->>", "defstruct", "doseq", "dotimes", "doto", "extend",
                              "for", "go-loop", "if", "if-let", "if-not", "if-some", "let", "let*", "locking", "loop", "match",
                              "ns", "struct-map", "testing", "when", "when-first", "when-let", "when-not", "when-some",
                              "while", "with-local-vars", "with-open", "with-precision", "with-redefs", "let-routes"}) {
            block(k, 1);
        }
        for (const char *k : {"are", "as->", "catch", "condp"}) block(k, 2);
        for (const char *k : {"bound-fn", "def", "defmacro", "defmethod", "defmulti", "defn", "defn-", "defonce", "deftest",
                              "fdef", "fn", "use-fixtures", "ANY", "DELETE", "GET", "HEAD", "OPTIONS", "PATCH", "POST",
                              "PUT", "context", "defroutes", "rfn"}) {
            inner0(k);
        }
        v.push_back(Rule{"defprotocol", false, {{1, 1}, {0, 1, -1}}, 1});
        v.push_back(Rule{"defrecord", false, {{1, 2}, {0, 1, -1}}, 1});
        v.push_back(Rule{"deftype", false, {{1, 2}, {0, 1, -1}}, 1});
        v.push_back(Rule{"extend-protocol", false, {{1, 1}, {0, 1, -1}}, 1});
        v.push_back(Rule{"extend-type", false, {{1, 1}, {0, 1, -1}}, 1});
        v.push_back(Rule{"letfn", false, {{1, 1}, {0, 2, 0}}, 2});
        v.push_back(Rule{"proxy", false, {{1, 2}, {0, 1, -1}}, 1});
        v.push_back(Rule{"reify", false, {{0, 0, -1}, {0, 1, -1}}, 1});
        v.push_back(Rule{"^def(?!ault)(?!late)(?!er)", true, {{0, 0, -1}}, 0});
        v.push_back(Rule{"^with-", true, {{0, 0, -1}}, 0});
        // cljfmt's order: deeper :inner rules first, symbols before patterns, then by name.
        std::stable_sort(v.begin(), v.end(), [](const Rule &x, const Rule &y) {
            if (x.order_depth != y.order_depth) return x.order_depth > y.order_depth;
            if (x.pattern != y.pattern) return !x.pattern;
            return x.key < y.key;
        });
        return v;
    }();
    return rules;
}

bool KeyMatches(const Rule &rule, const std::string &name) {
    if (!rule.pattern) return rule.key == name;
    // std::regex has no lookahead-free form of these; spell them out.
    if (rule.key == "^with-") return name.compare(0, 5, "with-") == 0;
    return name.compare(0, 3, "def") == 0 && name.compare(0, 7, "default") != 0 && name.compare(0, 7, "deflate") != 0 &&
           name.compare(0, 5, "defer") != 0;
}

class Indenter {
public:
    Indenter(const CljReadResult &r, Text &t, Tree &tree) : r_(r), t_(t), tree_(tree) {
        delta_.assign(t.starts.size(), 0);
        done_.assign(t.starts.size(), false);
        // Each token's innermost enclosing collection, from the openers and closers.
        enclosing_.assign(tree.toks.size(), -1);
        std::vector<int> stack;
        for (size_t k = 0; k < tree.toks.size(); k++) {
            const Tok &tk = tree.toks[k];
            if (tk.k == TK::Close && !stack.empty()) stack.pop_back();
            enclosing_[k] = stack.empty() ? -1 : stack.back();
            if (tk.k == TK::Open) stack.push_back(tk.node);
        }
    }

    // The new indentation of each line, top down; -1 to leave a line alone.
    std::vector<int> Run() {
        std::vector<int> out(t_.starts.size(), -1);
        // The token starting each line, if the line does not continue one.
        std::vector<int> first(t_.starts.size(), -1);
        std::vector<bool> inside(t_.starts.size(), false);
        for (size_t k = 0; k < tree_.toks.size(); k++) {
            const Tok &tk = tree_.toks[k];
            const int la = t_.Line(tk.a), lb = t_.Line(tk.b > tk.a ? tk.b - 1 : tk.a);
            for (int l = la + 1; l <= lb; l++) inside[static_cast<size_t>(l)] = true;
            if (first[static_cast<size_t>(la)] < 0) first[static_cast<size_t>(la)] = static_cast<int>(k);
        }
        for (size_t l = 0; l < t_.starts.size(); l++) {
            const int k = first[l];
            if (k < 0 || inside[l]) {
                done_[l] = true;
                continue;
            }
            const Tok &tk = tree_.toks[static_cast<size_t>(k)];
            // The token must be the line's first non-blank.
            const int col = CharCol(tk.a);
            const std::string &s = t_.s;
            bool blank_before = true;
            for (size_t p = t_.starts[l]; p < tk.a; p++) {
                if (s[p] != ' ' && s[p] != '\t' && s[p] != ',') blank_before = false;
            }
            if (!blank_before || tk.k == TK::Comment || l == 0) {
                done_[l] = true;
                continue;
            }
            const int want = Amount(tk, static_cast<size_t>(k));
            out[l] = want;
            delta_[l] = want - col;
            done_[l] = true;
        }
        return out;
    }

private:
    const CljReadResult &r_;
    Text &t_;
    Tree &tree_;
    std::vector<int> delta_;
    std::vector<bool> done_;
    std::vector<int> enclosing_;

    // The current column of an offset (its line already re-indented when above).
    // Columns count characters, as cljfmt does, not bytes.
    int CharCol(size_t off) const {
        const int l = t_.Line(off);
        int col = 0;
        for (size_t p = t_.starts[static_cast<size_t>(l)]; p < off; p++) {
            if ((static_cast<unsigned char>(t_.s[p]) & 0xC0) != 0x80) col++;
        }
        return col;
    }
    int Col(size_t off) const {
        const int l = t_.Line(off);
        return CharCol(off) + (done_[static_cast<size_t>(l)] ? delta_[static_cast<size_t>(l)] : 0);
    }

    // The collection whose element (or closer) a token starts, with the
    // element's index (kids before it); -1 at the top level.
    void Place(const Tok &tk, size_t k, int *coll, int *index) const {
        *coll = -1;
        *index = 0;
        if (tk.k == TK::Close) {
            *coll = tk.node;
            *index = static_cast<int>(r_.at(tk.node).kids.size());
            return;
        }
        const int best = enclosing_[k];
        if (best < 0) return;
        *coll = best;
        // Kids are in source order: the count of those starting before the token.
        const std::vector<int> &kids = r_.at(best).kids;
        const auto it = std::lower_bound(kids.begin(), kids.end(), tk.a, [&](int kid, size_t a) {
            return tree_.Start(kid) < a;  // (a line starting at metadata's target is that element)
        });
        *index = static_cast<int>(it - kids.begin());
    }

    int OpenLen(int coll) const {
        const size_t a = tree_.Start(coll);
        size_t oe = a;
        while (oe < t_.s.size() && t_.s[oe] != '(' && t_.s[oe] != '[' && t_.s[oe] != '{') oe++;
        return static_cast<int>(oe + 1 - a);
    }

    int Margin(int coll) const { return Col(tree_.Start(coll)); }
    int CollIndent(int coll) const { return Margin(coll) + OpenLen(coll); }
    int Width(int coll) const { return r_.at(coll).kind == CljNodeKind::Fn ? 3 : 2; }

    // Past the first argument, under it -- "it" being whatever comes first
    // after the head, a #_ form included (cljfmt counts arguments without
    // them, but aligns with the next thing after the head).
    int ListIndent(int coll, int index) const {
        const std::vector<int> &kids = r_.at(coll).kids;
        if (index > 1 && kids.size() > 1) {
            size_t at = tree_.visual[static_cast<size_t>(kids[1])];
            const size_t head_end = tree_.End(kids[0]);
            const auto it = std::lower_bound(tree_.toks.begin(), tree_.toks.end(), head_end,
                                             [](const Tok &t, size_t a) { return t.a < a; });
            for (auto j = it; j != tree_.toks.end() && j->a < at; ++j) {
                if (j->k == TK::Discard) {
                    at = j->a;
                    break;
                }
            }
            return Col(at);
        }
        return CollIndent(coll);
    }

    // The name a collection's first form gives it (a #? head: its first symbol).
    std::string HeadName(int coll) const {
        if (coll < 0 || r_.at(coll).kids.empty()) return "";
        int h = r_.at(coll).kids[0];
        if (r_.at(h).kind == CljNodeKind::ReadCond) {
            const std::vector<int> &ck = r_.at(h).kids;
            h = ck.size() > 1 ? ck[1] : -1;
        }
        const CljNode &n = r_.at(h);
        return n.kind == CljNodeKind::Symbol ? n.name : "";
    }

    bool FirstInLine(int node) const {
        const size_t v = tree_.visual[static_cast<size_t>(node)];
        const int l = t_.Line(v);
        for (size_t p = t_.starts[static_cast<size_t>(l)]; p < v; p++) {
            if (t_.s[p] != ' ' && t_.s[p] != '\t' && t_.s[p] != ',') return false;
        }
        return true;
    }

    // A node's container (the collection holding it as an element) and its
    // index there; for the element being indented, (coll, index) are given.
    int Container(int node) const {
        int p = r_.at(node).parent;
        while (p >= 0 && !IsCollection(r_.at(p).kind)) p = r_.at(p).parent;
        return p;
    }
    int IndexIn(int coll, int node) const {
        const std::vector<int> &kids = r_.at(coll).kids;
        for (size_t k = 0; k < kids.size(); k++) {
            if (kids[k] == node) return static_cast<int>(k);
        }
        return -1;
    }

    int Amount(const Tok &tk, size_t k) const {
        int coll = -1, index = 0;
        Place(tk, k, &coll, &index);
        if (coll < 0) return 0;
        const CljNodeKind kind = r_.at(coll).kind;
        if (kind == CljNodeKind::ReadCond || kind == CljNodeKind::ReadCondSplicing) return CollIndent(coll);
        if (kind != CljNodeKind::List && kind != CljNodeKind::Fn) return CollIndent(coll);
        for (const Rule &rule : Rules()) {
            for (const std::vector<int> &spec : rule.specs) {
                int got = -1;
                if (spec[0] == 1) {
                    got = Block(rule, spec[1], coll, index);
                } else {
                    got = Inner(rule, spec[1], spec[2], coll, index);
                }
                if (got >= 0) return got;
            }
        }
        return ListIndent(coll, index);
    }

    int Block(const Rule &rule, int idx, int coll, int index) const {
        if (!KeyMatches(rule, HeadName(coll))) return -1;
        const std::vector<int> &kids = r_.at(coll).kids;
        const bool after_ok = static_cast<size_t>(idx + 1) >= kids.size() || FirstInLine(kids[static_cast<size_t>(idx + 1)]);
        if (after_ok && index > idx) return Margin(coll) + Width(coll);
        return ListIndent(coll, index);
    }

    // :inner at depth: the form `depth` levels up from the element (0: the
    // element itself) must sit in a form headed by the key.
    int Inner(const Rule &rule, int depth, int idx, int coll, int index) const {
        if (index < 1) return -1;  // the head itself
        int container = coll;  // the collection holding the depth-0 form
        int top_node = -1;     // the depth-d form (-1: the element itself)
        for (int d = 0; d < depth; d++) {
            top_node = container;
            container = Container(container);
            if (container < 0) return -1;
        }
        if (!KeyMatches(rule, HeadName(container))) return -1;
        if (idx >= 0 && depth > 0 && IndexIn(container, top_node) != idx + 1) return -1;
        return Margin(coll) + Width(coll);
    }
};

// The input's tokens, for the check: kinds and texts, no whitespace.
std::vector<std::string> Signature(const Tree &tree) {
    std::vector<std::string> out;
    for (const Tok &tk : tree.toks) {
        std::string s = tree.t.s.substr(tk.a, tk.b - tk.a);
        if (tk.k == TK::Comment) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
        }
        if (tk.k == TK::Open) {
            // `#? (` and `#?(` are one opener.
            s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return IsWs(c); }), s.end());
        }
        out.push_back(std::to_string(static_cast<int>(tk.k)) + s);
    }
    return out;
}

}  // namespace

Result Format(std::string_view input) {
    Result res;
    res.text = std::string(input);
    const Text in{std::string(input)};
    const std::vector<std::string> lines = SplitLines(in.s);
    const CljReadResult r = CljRead(lines, false);
    if (!r.errors.empty()) {
        res.error = r.errors[0].message;
        res.error_line = r.errors[0].range.line + 1;
        return res;
    }
    Tree tree(r, in);
    if (!tree.Markers()) {
        res.error = "could not read this file's layout; left as it is";
        return res;
    }

    // 1. The gaps: no whitespace inside brackets, a space between touching
    // forms, no trailing blanks, one blank line at most.
    std::string a;
    {
        const std::vector<Tok> &tk = tree.toks;
        if (tk.empty()) {
            res.ok = true;
            return res;
        }
        const std::string lead = in.s.substr(0, tk[0].a);
        a += lead.find_first_not_of(" \t\n,") == std::string::npos && lead.find('\n') != std::string::npos ? "" : lead;
        int depth = 0;
        for (size_t k = 0; k < tk.size(); k++) {
            const std::string text = in.s.substr(tk[k].a, tk[k].b - tk[k].a);
            a += text;
            if (tk[k].k == TK::Open) depth++;
            if (tk[k].k == TK::Close) depth--;
            if (k + 1 < tk.size()) {
                a += FixGap(in.s.substr(tk[k].b, tk[k + 1].a - tk[k].b), tk[k], tk[k + 1], text,
                            in.s.substr(tk[k + 1].a, tk[k + 1].b - tk[k + 1].a), depth == 0);
            }
        }
        // The end: what followed the last form, its lines' trailing blanks dropped.
        std::string tail = in.s.substr(tk.back().b);
        std::string kept;
        for (char c : tail) {
            if (c == '\n') {
                while (!kept.empty() && (kept.back() == ' ' || kept.back() == '\t')) kept.pop_back();
            }
            kept += c;
        }
        if (kept.find_first_not_of(" \t,") == std::string::npos) kept.clear();  // no newline: nothing
        else if (kept.back() != '\n') kept = kept.substr(0, kept.find_last_of('\n') + 1);
        a += kept;
    }

    // 2. Indentation, top down.
    std::string b;
    {
        Text mid{a};
        const CljReadResult r2 = CljRead(SplitLines(a), false);
        if (!r2.errors.empty()) {
            res.error = "could not lay this file out without changing it; left as it is";
            return res;
        }
        Tree tree2(r2, mid);
        if (!tree2.Markers()) {
            res.error = "could not lay this file out without changing it; left as it is";
            return res;
        }
        Indenter ind(r2, mid, tree2);
        const std::vector<int> want = ind.Run();
        const std::vector<std::string> ls = SplitLines(a);
        for (size_t l = 0; l < ls.size(); l++) {
            std::string line = ls[l];
            if (l < want.size() && want[l] >= 0) {
                size_t f = 0;
                while (f < line.size() && (line[f] == ' ' || line[f] == '\t')) f++;
                line = std::string(static_cast<size_t>(want[l]), ' ') + line.substr(f);
            }
            b += line;
            if (l + 1 < ls.size()) b += '\n';
        }
    }

    // 3. The check: the same tokens, in the same order.
    {
        const Text out{b};
        const CljReadResult r3 = CljRead(SplitLines(b), false);
        Tree t3(r3, out);
        if (!r3.errors.empty() || !t3.Markers() || Signature(t3) != Signature(tree)) {
            res.error = "could not lay this file out without changing it; left as it is";
            return res;
        }
    }
    res.ok = true;
    res.text = b;
    return res;
}

}  // namespace cljfmt
