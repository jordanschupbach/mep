#include "clojure_lsp.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>

#include "clojure_lsp_analysis.h"
#include "clojure_lsp_core_names.h"

using namespace clj;

// --- Vocabulary ------------------------------------------------------------------

namespace {

struct Vocab {
    std::vector<CljLspVar> vars;
    std::map<std::pair<std::string, std::string>, size_t> index;
    std::map<std::string, std::vector<size_t>> by_ns;
};

const Vocab &TheVocab() {
    static const Vocab v = [] {
        Vocab out;
        size_t n = 0;
        const CljCoreName *names = CljCoreNames(&n);
        out.vars.reserve(n);
        for (size_t k = 0; k < n; k++) {
            CljLspVar x;
            x.ns = names[k].ns;
            x.name = names[k].name;
            x.kind = names[k].kind;
            x.arglists = names[k].arglists;
            x.doc = names[k].doc;
            x.added = names[k].added;
            x.deprecated = names[k].deprecated;
            x.is_private = names[k].is_private;
            out.index[{x.ns, x.name}] = out.vars.size();
            out.by_ns[x.ns].push_back(out.vars.size());
            out.vars.push_back(std::move(x));
        }
        return out;
    }();
    return v;
}

}  // namespace

const CljLspVar *CljLspCoreVar(const std::string &ns, const std::string &name) {
    const Vocab &v = TheVocab();
    const auto it = v.index.find({ns, name});
    return it == v.index.end() ? nullptr : &v.vars[it->second];
}

std::vector<const CljLspVar *> CljLspNamespaceVars(const std::string &ns) {
    std::vector<const CljLspVar *> out;
    const Vocab &v = TheVocab();
    const auto it = v.by_ns.find(ns);
    if (it == v.by_ns.end() || ns.empty()) return out;
    for (size_t k : it->second) {
        if (!v.vars[k].is_private) out.push_back(&v.vars[k]);
    }
    return out;
}

// --- The project ---------------------------------------------------------------------

void CljProjectAddFile(CljProject *project, const std::string &path, const std::vector<std::string> &lines) {
    project->vars.erase(std::remove_if(project->vars.begin(), project->vars.end(),
                                       [&](const CljProjectVar &v) { return v.path == path; }),
                        project->vars.end());
    for (auto it = project->ns_paths.begin(); it != project->ns_paths.end();) {
        it = it->second == path ? project->ns_paths.erase(it) : std::next(it);
    }
    const Analysis a = Analyze(lines, path, nullptr);
    if (a.ns_form < 0) return;
    project->ns_paths[a.ns_name] = path;
    for (const FileVar &v : a.vars) {
        if (v.in_comment || v.kind == "declare") continue;
        CljProjectVar p;
        p.ns = a.ns_name;
        p.name = v.name;
        p.kind = v.kind == "defmacro" || v.kind == "def-macro" ? "macro"
                 : (v.kind == "def" || v.kind == "defonce" || v.kind == "defprotocol" || v.kind == "defrecord" ||
                    v.kind == "deftype" || v.kind == "definterface" || v.kind == "defstruct")
                     ? (v.arglists.empty() ? "var" : "function")
                     : "function";
        for (size_t k = 0; k < v.arglists.size(); k++) p.arglists += (k ? "\n" : "") + v.arglists[k];
        p.doc = v.doc;
        p.path = path;
        p.range = v.range;
        p.is_private = v.is_private;
        project->vars.push_back(p);
    }
}

// --- Shared helpers ----------------------------------------------------------------------

namespace {

bool SymbolChar(char c) {
    return !(c == ' ' || c == '\t' || c == ',' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
             c == '"' || c == ';' || c == '\'' || c == '@' || c == '^' || c == '`' || c == '~' || c == '\\');
}

// The symbol-ish text ending at the cursor: [start, col).
int PrefixStart(const std::string &line, int col) {
    int s = std::min(col, static_cast<int>(line.size()));
    while (s > 0 && SymbolChar(line[static_cast<size_t>(s - 1)])) s--;
    return s;
}

std::vector<std::string> SplitArglists(const std::string &s) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p < s.size()) {
        const size_t nl = s.find('\n', p);
        out.push_back(s.substr(p, nl == std::string::npos ? std::string::npos : nl - p));
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    return out;
}

// "(name a b & more)" for each arglist.
std::string Usage(const std::string &name, const std::vector<std::string> &arglists) {
    std::string out;
    for (const std::string &al : arglists) {
        if (!out.empty()) out += "\n";
        std::string inner = al;
        if (inner.size() >= 2 && inner.front() == '[' && inner.back() == ']') inner = inner.substr(1, inner.size() - 2);
        out += "(" + name + (inner.empty() ? "" : " " + inner) + ")";
    }
    return out;
}

const CljProjectVar *FindProjectVar(const CljProject *project, const std::string &ns, const std::string &name) {
    if (!project) return nullptr;
    for (const CljProjectVar &v : project->vars) {
        if (v.ns == ns && v.name == name) return &v;
    }
    return nullptr;
}

std::string KindWord(const std::string &kind) {
    if (kind == "defmacro" || kind == "macro") return "macro";
    if (kind == "defn" || kind == "defn-" || kind == "function" || kind == "definline") return "function";
    if (kind == "special") return "special form";
    if (kind == "defmulti") return "multimethod";
    if (kind == "defprotocol") return "protocol";
    if (kind == "protocol-method") return "protocol method";
    if (kind == "defrecord") return "record";
    if (kind == "deftype") return "type";
    if (kind == "constructor") return "constructor";
    if (kind == "deftest") return "test";
    return "var";
}

}  // namespace

// --- Completion -----------------------------------------------------------------------------

std::vector<CljLspCompletionItem> CljLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                    const std::string &path, const CljProject *project) {
    std::vector<CljLspCompletionItem> out;
    if (line < 0 || line >= static_cast<int>(lines.size())) return out;
    const std::string &text = lines[static_cast<size_t>(line)];
    const int start = PrefixStart(text, col);
    const std::string prefix = text.substr(static_cast<size_t>(start), static_cast<size_t>(std::max(0, col - start)));
    const Analysis a = Analyze(lines, path, project);
    std::set<std::string> seen;
    auto add = [&](const std::string &label, CljLspKind kind, const std::string &detail, const std::string &doc,
                   const std::string &sort) {
        if (label.compare(0, prefix.size(), prefix) != 0 || !seen.insert(label).second) return;
        CljLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = doc;
        it.replace_start = start;
        it.replace_end = col;
        it.sort_text = sort + label;
        out.push_back(it);
    };
    // Keywords: the ones the file uses, and the ones forms take.
    if (!prefix.empty() && prefix[0] == ':') {
        for (int k : a.keywords) add(a.read.at(k).text, CljLspKind::Keyword, "", "", "1");
        for (const CljNode &n : a.read.nodes) {
            if (n.kind == CljNodeKind::Keyword) add(n.text, CljLspKind::Keyword, "", "", "1");
        }
        for (const char *k : {":keys", ":as", ":or", ":strs", ":syms", ":require", ":refer", ":import", ":refer-clojure",
                              ":exclude", ":only", ":rename", ":pre", ":post", ":doc", ":private", ":dynamic", ":tag",
                              ":else", ":default", ":clj", ":cljs", ":let", ":when", ":while"}) {
            add(k, CljLspKind::Keyword, "", "", "2");
        }
        return out;
    }
    // alias/name: the namespace's vars.
    const size_t slash = prefix.find('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string qual = prefix.substr(0, slash);
        const auto al = a.aliases.find(qual);
        const std::string ns = al != a.aliases.end() ? al->second : qual;
        for (const CljLspVar *v : CljLspNamespaceVars(ns)) {
            add(qual + "/" + v->name, v->kind == "macro" ? CljLspKind::Keyword : CljLspKind::Function,
                Usage(v->name, SplitArglists(v->arglists)), v->doc, "1");
        }
        if (project) {
            for (const CljProjectVar &v : project->vars) {
                if (v.ns != ns || v.is_private) continue;
                add(qual + "/" + v.name, v.kind == "var" ? CljLspKind::Variable : CljLspKind::Function,
                    Usage(v.name, SplitArglists(v.arglists)), v.doc, "1");
            }
        }
        if (ns == a.ns_name) {
            for (const FileVar &v : a.vars) add(qual + "/" + v.name, CljLspKind::Function, "", v.doc, "1");
        }
        return out;
    }
    // Inside (:require [...]): namespace names.
    const int list = ListAt(a, line, col);
    if (list >= 0 && a.ns_form >= 0) {
        const CljRange &nr = a.read.at(a.ns_form).range;
        if (CljRangeContains(nr, line, col)) {
            const int head = a.read.at(list).kids.empty() ? -1 : a.read.at(list).kids[0];
            const CljNode &h = a.read.at(head);
            if (h.kind == CljNodeKind::Keyword && (h.name == "require" || h.name == "use")) {
                std::set<std::string> nss;
                size_t n = 0;
                const CljCoreName *names = CljCoreNames(&n);
                for (size_t k = 0; k < n; k++) {
                    if (names[k].ns[0]) nss.insert(names[k].ns);
                }
                if (project) {
                    for (const auto &[ns, p] : project->ns_paths) nss.insert(ns);
                }
                for (const std::string &ns : nss) add(ns, CljLspKind::Module, "namespace", "", "1");
                return out;
            }
        }
    }
    // Locals, innermost first; the file's vars; referred names; clojure.core.
    int rank = 0;
    for (int b : LocalsAt(a, line, col)) {
        const Binding &bd = a.bindings[static_cast<size_t>(b)];
        char key[8];
        std::snprintf(key, sizeof key, "0%03d", std::min(rank++, 999));
        add(bd.name, CljLspKind::Variable, "local (" + bd.kind + ")", "", key);
    }
    for (const FileVar &v : a.vars) {
        const bool fn = v.kind == "defn" || v.kind == "defn-" || v.kind == "defmacro" || v.kind == "defmulti" ||
                        v.kind == "protocol-method" || v.kind == "constructor";
        add(v.name, fn ? CljLspKind::Function : CljLspKind::Variable,
            v.arglists.empty() ? KindWord(v.kind) : Usage(v.name, v.arglists), v.doc, "1");
    }
    for (const auto &[name, ns] : a.refers) {
        const CljLspVar *v = CljLspCoreVar(ns, name);
        const CljProjectVar *pv = FindProjectVar(project, ns, name);
        add(name, CljLspKind::Function, ns, v ? v->doc : pv ? pv->doc : "", "2");
    }
    size_t n = 0;
    const CljCoreName *names = CljCoreNames(&n);
    for (size_t k = 0; k < n; k++) {
        const std::string ns = names[k].ns;
        if ((!ns.empty() && ns != "clojure.core") || names[k].is_private) continue;
        if (a.core_excluded.count(names[k].name)) continue;
        const std::string kind = names[k].kind;
        add(names[k].name, kind == "special" || kind == "macro" ? CljLspKind::Keyword : kind == "function" ? CljLspKind::Function : CljLspKind::Variable,
            kind == "special" ? "special form" : Usage(names[k].name, SplitArglists(names[k].arglists)), names[k].doc, "3");
    }
    // A few interop and binding names.
    if (!prefix.empty() && prefix[0] == '&') add("&", CljLspKind::Operator, "rest", "", "4");
    return out;
}

// --- Hover ----------------------------------------------------------------------------------------

CljLspHoverInfo CljLspHover(const std::vector<std::string> &lines, int line, int col, const std::string &path,
                            const CljProject *project) {
    CljLspHoverInfo h;
    const Analysis a = Analyze(lines, path, project);
    const int oi = OccurrenceAt(a, line, col);
    if (oi < 0) {
        // A keyword: what namespace it is in.
        for (int k : a.keywords) {
            const CljNode &n = a.read.at(k);
            if (!CljRangeTouches(n.range, line, col)) continue;
            std::string ns = n.ns;
            if (n.auto_resolve) {
                const auto al = a.aliases.find(n.ns);
                ns = n.ns.empty() ? a.ns_name : al != a.aliases.end() ? al->second : n.ns;
            }
            h.found = true;
            h.range = n.range;
            h.text = "keyword `:" + (ns.empty() ? n.name : ns + "/" + n.name) + "`";
            return h;
        }
        return h;
    }
    const Occurrence &o = a.occs[static_cast<size_t>(oi)];
    h.range = o.range;
    auto code = [](const std::string &s) { return "```clojure\n" + s + "\n```"; };
    switch (o.target) {
        case Target::Local: {
            const Binding &b = a.bindings[static_cast<size_t>(o.binding)];
            h.found = true;
            h.text = code(b.name) + "\nlocal (" + b.kind + "), bound at line " + std::to_string(b.range.line + 1);
            return h;
        }
        case Target::FileVar: {
            const FileVar &v = a.vars[static_cast<size_t>(o.var)];
            h.found = true;
            h.text = code(v.arglists.empty() ? a.ns_name + "/" + v.name : Usage(v.name, v.arglists)) + "\n" +
                     KindWord(v.kind) + (v.is_private ? ", private" : "") + " in `" + a.ns_name + "`" +
                     (v.doc.empty() ? "" : "\n\n" + v.doc);
            return h;
        }
        case Target::CoreVar: {
            const CljLspVar *v = CljLspCoreVar(o.ns, o.name);
            if (!v) return h;
            h.found = true;
            const std::vector<std::string> al = SplitArglists(v->arglists);
            std::string usage;
            if (v->kind == "special") {
                usage = v->arglists.empty() ? v->name : v->arglists;
            } else {
                usage = al.empty() ? (o.ns + "/" + o.name) : Usage(v->name, al);
            }
            h.text = code(usage) + "\n" + KindWord(v->kind) + (o.ns.empty() ? "" : " in `" + o.ns + "`") +
                     (v->added.empty() ? "" : ", since " + v->added) +
                     (v->deprecated.empty() ? "" : " -- **deprecated since " + v->deprecated + "**") +
                     (v->doc.empty() ? "" : "\n\n" + v->doc);
            return h;
        }
        case Target::ProjectVar: {
            const CljProjectVar *v = FindProjectVar(project, o.ns, o.name);
            if (!v) return h;
            h.found = true;
            const std::vector<std::string> al = SplitArglists(v->arglists);
            h.text = code(al.empty() ? o.ns + "/" + o.name : Usage(v->name, al)) + "\n" + KindWord(v->kind) + " in `" +
                     o.ns + "`" + (v->doc.empty() ? "" : "\n\n" + v->doc);
            return h;
        }
        case Target::Class:
            h.found = true;
            h.text = "Java " + std::string(o.text[0] == '.' ? "method or field `" : "class or member `") + o.text + "`";
            return h;
        case Target::OtherVar:
            h.found = true;
            h.text = "var `" + o.ns + "/" + o.name + "` (namespace not indexed)";
            return h;
        default: return h;
    }
}

// --- Signature help ---------------------------------------------------------------------------------

CljLspSignatureHelp CljLspSignatures(const std::vector<std::string> &lines, int line, int col, const std::string &path,
                                     const CljProject *project) {
    CljLspSignatureHelp out;
    const Analysis a = Analyze(lines, path, project);
    const int list = ListAt(a, line, col);
    if (list < 0) return out;
    const CljNode &ln = a.read.at(list);
    if (ln.kids.empty() || a.read.at(ln.kids[0]).kind != CljNodeKind::Symbol) return out;
    // The head's occurrence.
    const int head = ln.kids[0];
    const Occurrence *o = nullptr;
    for (const Occurrence &x : a.occs) {
        if (x.node == head) o = &x;
    }
    if (!o) return out;
    std::vector<std::string> arglists;
    std::string name = a.read.at(head).text, doc;
    if (o->target == Target::FileVar) {
        arglists = a.vars[static_cast<size_t>(o->var)].arglists;
        doc = a.vars[static_cast<size_t>(o->var)].doc;
    } else if (o->target == Target::CoreVar) {
        const CljLspVar *v = CljLspCoreVar(o->ns, o->name);
        if (v && v->kind != "special") {
            arglists = SplitArglists(v->arglists);
            doc = v->doc;
        }
    } else if (o->target == Target::ProjectVar) {
        if (const CljProjectVar *v = FindProjectVar(project, o->ns, o->name)) {
            arglists = SplitArglists(v->arglists);
            doc = v->doc;
        }
    }
    if (arglists.empty()) return out;
    // Which argument the cursor is on: the forms that end before it.
    int arg = 0;
    for (size_t k = 1; k < ln.kids.size(); k++) {
        const CljRange &r = a.read.at(ln.kids[k]).range;
        if (r.end_line < line || (r.end_line == line && r.end_col < col)) arg++;
        else break;
    }
    out.found = true;
    out.documentation = doc;
    out.active_param = arg;
    int best = -1;
    for (size_t k = 0; k < arglists.size(); k++) {
        CljLspSignature sig;
        std::string inner = arglists[k];
        if (inner.size() >= 2 && inner.front() == '[') inner = inner.substr(1, inner.size() - 2);
        sig.label = "(" + name + (inner.empty() ? "" : " " + inner) + ")";
        // Parameters: the top-level tokens of the arglist.
        int depth = 0;
        std::string tok;
        int fixed = 0;
        bool variadic = false;
        auto flush = [&] {
            if (tok.empty()) return;
            if (tok == "&") variadic = true;
            else {
                sig.params.push_back(tok);
                if (!variadic) fixed++;
            }
            tok.clear();
        };
        for (char c : inner) {
            if (depth == 0 && (c == ' ' || c == ',')) {
                flush();
                continue;
            }
            if (c == '[' || c == '{' || c == '(') depth++;
            if (c == ']' || c == '}' || c == ')') depth--;
            tok += c;
        }
        flush();
        if (best < 0 && (arg < fixed || variadic)) best = static_cast<int>(k);
        out.signatures.push_back(sig);
    }
    out.active_signature = best < 0 ? static_cast<int>(arglists.size()) - 1 : best;
    const CljLspSignature &chosen = out.signatures[static_cast<size_t>(out.active_signature)];
    if (out.active_param >= static_cast<int>(chosen.params.size()) && !chosen.params.empty()) {
        out.active_param = static_cast<int>(chosen.params.size()) - 1;  // the rest parameter
    }
    return out;
}

// --- Symbols and folds ----------------------------------------------------------------------------------

std::vector<CljLspSymbol> CljLspSymbols(const std::vector<std::string> &lines) {
    std::vector<CljLspSymbol> out;
    const Analysis a = Analyze(lines, "", nullptr);
    if (a.ns_form >= 0) {
        CljLspSymbol s;
        s.name = a.ns_name;
        s.kind = CljLspSymbolKind::Namespace;
        s.range = a.read.at(a.ns_form).range;
        s.sel_range = a.read.at(a.ns_name_node).range;
        out.push_back(s);
    }
    std::map<std::string, int> parents;
    for (const FileVar &v : a.vars) {
        if (v.kind == "constructor" || v.kind == "declare") continue;
        CljLspSymbol s;
        s.name = v.name;
        s.range = v.form_range;
        s.sel_range = v.range;
        s.detail = v.arglists.empty() ? KindWord(v.kind) : v.arglists.front();
        if (v.kind == "defn" || v.kind == "defn-" || v.kind == "defmulti" || v.kind == "definline" || v.kind == "deftest") {
            s.kind = CljLspSymbolKind::Function;
        } else if (v.kind == "defmacro") {
            s.kind = CljLspSymbolKind::Function;
            s.detail = "macro";
        } else if (v.kind == "defprotocol" || v.kind == "definterface") {
            s.kind = CljLspSymbolKind::Interface;
        } else if (v.kind == "defrecord") {
            s.kind = CljLspSymbolKind::Struct;
        } else if (v.kind == "deftype") {
            s.kind = CljLspSymbolKind::Class;
        } else if (v.kind == "protocol-method") {
            s.kind = CljLspSymbolKind::Method;
            const auto it = parents.find(v.parent);
            if (it != parents.end()) s.parent = it->second;
        } else {
            s.kind = CljLspSymbolKind::Variable;
        }
        if (v.kind == "defprotocol") parents[v.name] = static_cast<int>(out.size());
        out.push_back(s);
    }
    return out;
}

std::vector<CljLspFold> CljLspFolds(const std::vector<std::string> &lines) {
    std::vector<CljLspFold> out;
    const CljReadResult r = CljRead(lines);
    for (const CljNode &n : r.nodes) {
        switch (n.kind) {
            case CljNodeKind::List:
            case CljNodeKind::Vector:
            case CljNodeKind::Map:
            case CljNodeKind::Set:
            case CljNodeKind::Fn:
            case CljNodeKind::ReadCond:
            case CljNodeKind::ReadCondSplicing:
            case CljNodeKind::NsMap:
            case CljNodeKind::String:
                if (n.range.end_line > n.range.line) out.push_back(CljLspFold{n.range.line, n.range.end_line, ""});
                break;
            default: break;
        }
    }
    // Runs of comment lines.
    int run_start = -1, prev = -2;
    for (const CljComment &c : r.comments) {
        if (c.range.line == prev + 1 && run_start >= 0) {
            prev = c.range.line;
            continue;
        }
        if (run_start >= 0 && prev > run_start) out.push_back(CljLspFold{run_start, prev, "comment"});
        run_start = prev = c.range.line;
    }
    if (run_start >= 0 && prev > run_start) out.push_back(CljLspFold{run_start, prev, "comment"});
    std::sort(out.begin(), out.end(), [](const CljLspFold &x, const CljLspFold &y) {
        return x.start_line != y.start_line ? x.start_line < y.start_line : x.end_line > y.end_line;
    });
    return out;
}

// --- Navigation and rename ------------------------------------------------------------------------------

namespace {

// The occurrences that name the same thing as occurrence `oi`.
std::vector<int> Same(const Analysis &a, int oi) {
    std::vector<int> out;
    const Occurrence &o = a.occs[static_cast<size_t>(oi)];
    for (size_t k = 0; k < a.occs.size(); k++) {
        const Occurrence &x = a.occs[k];
        bool same = false;
        if (o.target == Target::Local) same = x.target == Target::Local && x.binding == o.binding;
        else if (o.target == Target::FileVar)
            same = x.target == Target::FileVar && a.vars[static_cast<size_t>(x.var)].name == a.vars[static_cast<size_t>(o.var)].name;
        else if (o.target == Target::CoreVar || o.target == Target::ProjectVar || o.target == Target::OtherVar)
            same = x.target == o.target && x.ns == o.ns && x.name == o.name;
        if (same) out.push_back(static_cast<int>(k));
    }
    return out;
}

}  // namespace

CljLspLocation CljLspDefinition(const std::vector<std::string> &lines, int line, int col, const std::string &path,
                                const CljProject *project) {
    CljLspLocation loc;
    const Analysis a = Analyze(lines, path, project);
    const int oi = OccurrenceAt(a, line, col);
    if (oi < 0) return loc;
    const Occurrence &o = a.occs[static_cast<size_t>(oi)];
    if (o.target == Target::Local) {
        loc.found = true;
        loc.range = a.bindings[static_cast<size_t>(o.binding)].range;
    } else if (o.target == Target::FileVar) {
        const std::vector<int> defs = FileVarsNamed(a, a.vars[static_cast<size_t>(o.var)].name);
        // The first real definition (a declare only promises one).
        int pick = defs.empty() ? o.var : defs.front();
        for (int d : defs) {
            if (!a.vars[static_cast<size_t>(d)].declared_only) {
                pick = d;
                break;
            }
        }
        loc.found = true;
        loc.range = a.vars[static_cast<size_t>(pick)].range;
    } else if (o.target == Target::ProjectVar) {
        if (const CljProjectVar *v = FindProjectVar(project, o.ns, o.name)) {
            loc.found = true;
            loc.path = v->path;
            loc.range = v->range;
        }
    }
    return loc;
}

std::vector<CljLspReference> CljLspReferences(const std::vector<std::string> &lines, int line, int col,
                                              const std::string &path) {
    std::vector<CljLspReference> out;
    const Analysis a = Analyze(lines, path, nullptr);
    const int oi = OccurrenceAt(a, line, col);
    if (oi < 0) return out;
    for (int k : Same(a, oi)) {
        const Occurrence &x = a.occs[static_cast<size_t>(k)];
        out.push_back(CljLspReference{x.range, x.is_definition});
    }
    return out;
}

CljLspRenameTarget CljLspPrepareRename(const std::vector<std::string> &lines, int line, int col, const std::string &path) {
    CljLspRenameTarget t;
    const Analysis a = Analyze(lines, path, nullptr);
    const int oi = OccurrenceAt(a, line, col);
    if (oi < 0) return t;
    const Occurrence &o = a.occs[static_cast<size_t>(oi)];
    if (o.target != Target::Local && o.target != Target::FileVar) return t;
    t.found = true;
    t.range = o.range;
    t.placeholder = o.target == Target::Local ? a.bindings[static_cast<size_t>(o.binding)].name : o.text;
    return t;
}

std::vector<CljLspTextEdit> CljLspRename(const std::vector<std::string> &lines, int line, int col,
                                         const std::string &new_name, std::string *error, const std::string &path) {
    std::vector<CljLspTextEdit> out;
    std::string scratch;
    std::string *why = error ? error : &scratch;
    const Analysis a = Analyze(lines, path, nullptr);
    const int oi = OccurrenceAt(a, line, col);
    if (oi < 0) {
        *why = "nothing to rename here";
        return out;
    }
    const Occurrence &o = a.occs[static_cast<size_t>(oi)];
    if (o.target != Target::Local && o.target != Target::FileVar) {
        *why = "`" + o.text + "` is not this file's: only locals and the file's own vars can be renamed";
        return out;
    }
    // A new name must read as a plain symbol.
    const CljReadResult r = CljRead({new_name});
    if (new_name.empty() || r.top.size() != 1 || !r.errors.empty() || r.at(r.top[0]).kind != CljNodeKind::Symbol ||
        new_name.find('/') != std::string::npos) {
        *why = "`" + new_name + "` is not a valid symbol name";
        return out;
    }
    for (int k : Same(a, oi)) {
        const Occurrence &x = a.occs[static_cast<size_t>(k)];
        CljLspTextEdit e;
        e.range = x.range;
        // ns-qualified uses of a file var keep their qualifier.
        const size_t slash = x.text.find('/');
        if (o.target == Target::FileVar && slash != std::string::npos && x.text != "/") {
            e.range.col += static_cast<int>(slash) + 1;
        }
        // :keys [a/b] destructuring: the name part only.
        if (o.target == Target::Local && x.text != a.read.at(x.node).text) {
            const std::string &full = a.read.at(x.node).text;
            const size_t s = full.find('/');
            if (s != std::string::npos) e.range.col += static_cast<int>(s) + 1;
        }
        e.new_text = new_name;
        out.push_back(e);
    }
    return out;
}
