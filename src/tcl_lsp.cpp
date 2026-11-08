// mep's Tcl language server, analysis half: see tcl_lsp.h.
//
// The document is parsed with tcl_parse.h (a port of Tcl's own parser), and
// every script Tcl would run is parsed in place: proc bodies, the bodies and
// conditions of if/while/for/foreach/switch/try/catch, namespace eval and
// class definitions, `[...]` substitutions, braced `expr` arguments. A syntax
// error anywhere is reported with Tcl's message, where tclsh points.
//
// Variables are tracked per proc (and per namespace outside procs); the lints
// are calibrated on Tcl's, Tk's and tcllib's sources (help/tcl-lsp.org):
//   syntax-error       Tcl's parser rejects a script or an expression
//   wrong-args         a built-in command or subcommand gets an argument count
//                      tclsh refuses ("wrong # args"), with Tcl's usage line
//   unknown-subcommand an ensemble (string, dict, file, ...) has no such subcommand
//   unused-variable    a proc sets a local variable and never reads it
//   unbraced-expr      an expression with substitutions that is not in braces
//   missing-dollar     a local variable's name where its value is expected
#include "tcl_lsp.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>

#include "tcl_format.h"
#include "tcl_lsp_vocab.h"
#include "tcl_parse.h"

using tclparse::TokType;
using tclparse::Token;

namespace {

constexpr int kMaxDepth = 200;  // nested scripts we follow

struct Span {
    int start = 0, end = 0;
};

struct VarSite {
    int start = 0, end = 0;
    bool def = false;
};

struct VarInfo {
    std::vector<VarSite> sites;
    bool param = false;
    bool linked = false;  // global / variable / upvar: lives elsewhere
    int link_scope = -1;  // the namespace scope a global/variable declaration names
    bool read = false;
    bool other_def = false;  // set by foreach, regexp, scan, lassign...: placeholders are idiomatic there
};

struct Scope {
    enum Kind { Namespace, Proc, Detached } kind = Namespace;
    std::string ns = "::";
    std::string name;  // proc / method
    int body_start = 0, body_end = 0;
    bool dynamic = false;  // eval, info locals, computed names...: no unused-variable lint
    bool loose = false;    // itcl / snit / TclOO methods: instance variables need no declaration
    std::map<std::string, VarInfo> vars;
};

struct Region {
    int start, end, scope;
};

struct ProcInfo {
    std::string qname;    // ::ns::name
    std::string written;  // as in the source
    int name_start = 0, name_end = 0;
    int cmd_start = 0, cmd_end = 0;
    std::string params_label;
    std::vector<std::string> params;
    std::string doc;
    int scope = -1;
    bool method = false;
};

struct CallInfo {
    std::string written, ns;
    int start = 0, end = 0;  // the command word
};

struct CmdRec {
    int start = 0, end = 0;
    std::vector<Span> words;
    std::string name;  // literal command word ("" when computed)
    int scope = 0;
};

struct SymRec {
    std::string name, detail;
    TclLspSymbolKind kind = TclLspSymbolKind::Function;
    int start = 0, end = 0, sel_start = 0, sel_end = 0;
    int parent = -1;
};

struct Diag {
    int start = 0, end = 0;
    TclLspSeverity sev = TclLspSeverity::Error;
    std::string code, message;
    bool unnecessary = false;
};

struct ArityCheck {
    std::string name;  // the command whose shadowing cancels it
    std::string sub;   // "file home": an unknown subcommand
    Diag diag;
};

struct ReportSpan {
    std::string kind;  // script, cmd, expr
    int start, end;
    tclparse::Error error;
};

// --- Vocabulary -------------------------------------------------------------------------

struct Vocab {
    std::unordered_map<std::string, size_t> by_name;              // commands, subcommands ("string length")
    std::unordered_map<std::string, size_t> funcs, vars;          // expr functions, Tcl variables
    std::unordered_map<std::string, std::vector<std::string>> subs;  // ensemble -> subcommands
    const TclLspApi *api = nullptr;
    size_t count = 0;
};

const Vocab &V() {
    static const Vocab v = [] {
        Vocab out;
        out.api = TclLspApiTable(&out.count);
        for (size_t i = 0; i < out.count; i++) {
            const TclLspApi &a = out.api[i];
            const std::string kind = a.kind;
            if (kind == "function") {
                out.funcs[a.name] = i;
            } else if (kind == "variable") {
                out.vars[a.name] = i;
            } else {
                out.by_name[a.name] = i;
                if (kind == "subcommand") {
                    const std::string n = a.name;
                    const size_t sp = n.find(' ');
                    out.subs[n.substr(0, sp)].push_back(n.substr(sp + 1));
                }
            }
        }
        return out;
    }();
    return v;
}

const TclLspApi *Api(const std::string &name) {
    const Vocab &v = V();
    auto it = v.by_name.find(name);
    return it == v.by_name.end() ? nullptr : &v.api[it->second];
}

// The subcommand an ensemble resolves `word` to: exact, or a unique prefix (Tcl's -prefix rule).
std::string ResolveSub(const std::string &ens, const std::string &word, bool *ambiguous = nullptr) {
    const Vocab &v = V();
    auto it = v.subs.find(ens);
    if (it == v.subs.end() || word.empty()) return "";
    std::string found;
    int n = 0;
    for (const std::string &s : it->second) {
        if (s == word) return s;
        if (s.compare(0, word.size(), word) == 0) {
            found = s;
            n++;
        }
    }
    if (ambiguous) *ambiguous = n > 1;
    return n == 1 ? found : "";
}

// Subcommands Tcl 8.7 / 9.0 added: code written for them is not wrong.
bool Tcl9Sub(const std::string &s) {
    static const std::set<std::string> k = {
        "file home", "file tildeexpand", "string insert", "dict getwithdefault", "dict getdef", "info cmdtype",
        "info constant", "info consts", "chan isbinary", "array default", "array for", "namespace which",
        "info object creationid", "info class definitionnamespace", "string is dict", "encoding profiles",
        "zlib",
    };
    return k.count(s) > 0;
}

bool Accepts(const TclLspApi &a, int n) {
    if (a.mask == 0) return true;
    if (n > 30) return a.more;
    return (a.mask >> n) & 1u;
}

std::string CommandTail(const std::string &name) {
    const size_t p = name.rfind("::");
    return p == std::string::npos ? name : name.substr(p + 2);
}

// The builtin a written command name means ("::set" and "set" are set).
std::string BuiltinName(const std::string &written) {
    if (written.compare(0, 2, "::") == 0) {
        const std::string rest = written.substr(2);
        if (rest.find("::") == std::string::npos || rest.compare(0, 4, "oo::") == 0) return rest;
        return "";
    }
    return written;
}

bool IsVarNameChar(char c) { return tclparse::IsBareword(c) || c == ':'; }

// Splits a usage line into its words, keeping `?a b?` groups together.
std::vector<std::string> UsageParams(const std::string &usage, int skip) {
    std::vector<std::string> words;
    std::string cur;
    int depth = 0;
    for (char c : usage) {
        if (c == '?') {
            if (cur.empty() || cur.back() == ' ') depth++;
            else if (depth > 0) depth--;
        }
        if (c == ' ' && depth == 0) {
            if (!cur.empty()) words.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) words.push_back(cur);
    std::vector<std::string> out;
    for (size_t i = static_cast<size_t>(skip); i < words.size(); i++) out.push_back(words[i]);
    return out;
}

}  // namespace

class TclLspDocument {
public:
    std::vector<std::string> lines;
    std::string text;
    std::vector<int> line_starts;
    std::deque<Scope> scopes;  // a deque: references stay valid while scopes are added
    std::vector<Region> regions;
    std::map<std::string, int> ns_scope;  // namespace -> scope index
    std::vector<ProcInfo> procs;
    std::vector<CallInfo> calls;
    std::vector<CmdRec> cmds;
    std::vector<SymRec> syms;
    std::vector<Diag> diags;
    std::vector<ArityCheck> arity;
    std::vector<Span> funcs;  // expr function names
    std::vector<Span> folds;
    std::set<std::string> shadowed;   // command tails a proc / rename / alias redefines
    std::set<std::string> extended;   // ensembles the file reconfigures
    std::vector<ReportSpan> report;
    bool want_report = false;

    void Pos(int off, int *line, int *col) const {
        auto it = std::upper_bound(line_starts.begin(), line_starts.end(), off);
        const size_t li = static_cast<size_t>(it - line_starts.begin()) - 1;
        *line = static_cast<int>(li);
        *col = off - line_starts[li];
    }
    int Off(int line, int col) const {
        if (line < 0) return 0;
        if (static_cast<size_t>(line) >= line_starts.size()) return static_cast<int>(text.size());
        const int ls = line_starts[static_cast<size_t>(line)];
        const int le = static_cast<size_t>(line) + 1 < line_starts.size()
                           ? line_starts[static_cast<size_t>(line) + 1] - 1
                           : static_cast<int>(text.size());
        return std::max(ls, std::min(ls + col, le));
    }
    TclLspRange Range(int start, int end) const {
        TclLspRange r;
        Pos(start, &r.line, &r.col);
        Pos(end, &r.end_line, &r.end_col);
        return r;
    }
    int ScopeAt(int off) const {
        int best = 0, best_len = 1 << 30;
        for (const Region &r : regions) {
            if (off >= r.start && off <= r.end && r.end - r.start < best_len) {
                best = r.scope;
                best_len = r.end - r.start;
            }
        }
        return best;
    }
};

namespace {

// A word of a command, as the analysis needs it.
struct Word {
    size_t tok = 0;
    int start = 0, end = 0;
    bool literal = false;
    std::string value;
    bool braced = false, quoted = false, expand = false;
    int cstart = 0, cend = 0;  // a braced word's content
};

class Analyzer {
public:
    explicit Analyzer(TclLspDocument &doc) : d(doc), t(doc.text) {}

    void Run() {
        Scope g;
        g.kind = Scope::Namespace;
        g.ns = "::";
        g.body_end = static_cast<int>(t.size());
        d.scopes.push_back(g);
        d.ns_scope["::"] = 0;
        d.regions.push_back({0, static_cast<int>(t.size()), 0});
        // Packages that add subcommands to Tcl's ensembles: tcllib's dicttool (and clay, built on it)
        // extend dict; itcl extends info.
        if (t.find("dicttool") != std::string::npos || t.find("clay") != std::string::npos) d.extended.insert("dict");
        if (t.find("itcl") != std::string::npos || t.find("Itcl") != std::string::npos) d.extended.insert("info");
        Script(0, static_cast<int>(t.size()), false, 0, 0, "script");
        Finish();
    }

private:
    TclLspDocument &d;
    const std::string &t;
    int class_ctx_ = 0;     // inside a class definition script: 1 TclOO, 2 itcl/snit
    std::string class_name_;
    std::vector<std::string> class_vars_;
    int sym_parent_ = -1;
    bool plain_def_ = false;

    void AddDiag(int start, int end, TclLspSeverity sev, const std::string &code, const std::string &msg,
                 bool unnecessary = false) {
        Diag g;
        g.start = start;
        g.end = std::max(end, start);
        g.sev = sev;
        g.code = code;
        g.message = msg;
        g.unnecessary = unnecessary;
        d.diags.push_back(g);
    }

    int LineEnd(int off) const {
        const size_t nl = t.find('\n', static_cast<size_t>(std::max(0, off)));
        return nl == std::string::npos ? static_cast<int>(t.size()) : static_cast<int>(nl);
    }

    void SyntaxError(const tclparse::Error &e, int fallback_end) {
        std::string msg = e.message;
        for (char &c : msg)
            if (c == '\n') c = ' ';
        int start = std::min(std::max(0, e.pos), static_cast<int>(t.size()));
        int end = std::min(start + 1, std::min(LineEnd(start), fallback_end));
        if (end <= start) end = std::min(start + 1, static_cast<int>(t.size()));
        for (const Diag &g : d.diags)
            if (g.code == "syntax-error" && g.start == start) return;
        AddDiag(start, end, TclLspSeverity::Error, "syntax-error", msg);
    }

    // --- Scripts and words ---

    void Script(int start, int end, bool nested, int scope, int depth, const char *kind) {
        if (depth > kMaxDepth || start > end) return;
        tclparse::Script s = tclparse::ParseScript(t, start, end, nested);
        if (d.want_report) d.report.push_back({kind, start, end, s.error});
        for (const tclparse::Command &c : s.commands) Command(c, scope, depth);
        if (s.error.failed) SyntaxError(s.error, end);
    }

    std::vector<Word> Words(const tclparse::Command &c) const {
        std::vector<Word> out;
        const std::vector<Token> &tk = c.tokens;
        for (size_t i = 0; i < tk.size(); i += static_cast<size_t>(tk[i].num_components) + 1) {
            const Token &w = tk[i];
            if (w.type != TokType::Word && w.type != TokType::SimpleWord && w.type != TokType::ExpandWord) break;
            Word x;
            x.tok = i;
            x.start = w.start;
            x.end = w.start + w.size;
            x.expand = w.type == TokType::ExpandWord;
            const char c0 = w.size > 0 ? t[static_cast<size_t>(w.start)] : '\0';
            x.braced = !x.expand && c0 == '{';
            x.quoted = !x.expand && c0 == '"';
            if (x.braced) {
                x.cstart = w.start + 1;
                x.cend = w.start + w.size - 1;
            } else if (x.quoted) {
                x.cstart = w.start + 1;
                x.cend = w.start + w.size - 1;
            } else {
                x.cstart = x.start;
                x.cend = x.end;
            }
            x.literal = !x.expand && tclparse::WordLiteral(t, tk, i, &x.value);
            out.push_back(x);
        }
        return out;
    }

    // Variables and [scripts] in tokens [from, to).
    void Substs(const std::vector<Token> &tk, size_t from, size_t to, int scope, int depth) {
        for (size_t i = from; i < to && i < tk.size();) {
            const Token &k = tk[i];
            if (k.type == TokType::Variable) {
                if (i + 1 < tk.size()) {
                    const Token &name = tk[i + 1];
                    VarRef(scope, std::string(t, static_cast<size_t>(name.start), static_cast<size_t>(name.size)),
                           name.start, name.start + name.size);
                }
                Substs(tk, i + 2, i + 1 + static_cast<size_t>(k.num_components), scope, depth);
                i += 1 + static_cast<size_t>(k.num_components);
            } else if (k.type == TokType::Command) {
                Script(k.start + 1, k.start + k.size, true, scope, depth + 1, "cmd");
                i++;
            } else {
                i++;
            }
        }
    }

    void WordSubsts(const tclparse::Command &c, const Word &w, int scope, int depth) {
        Substs(c.tokens, w.tok + 1, w.tok + 1 + static_cast<size_t>(c.tokens[w.tok].num_components), scope, depth);
    }

    void Expr(int start, int end, int scope, int depth) {
        if (depth > kMaxDepth) return;
        tclparse::ExprResult r = tclparse::ParseExpr(t, start, end);
        if (d.want_report) d.report.push_back({"expr", start, end, r.error});
        if (r.error.failed) {
            SyntaxError(r.error, end);
            return;
        }
        Substs(r.tokens, 0, r.tokens.size(), scope, depth + 1);
        for (const auto &f : r.functions) d.funcs.push_back({f.start, f.start + f.size});
    }

    // A script argument: parsed in place when braced, else only its substitutions are followed.
    void ScriptArg(const tclparse::Command &c, const Word &w, int scope, int depth) {
        if (w.braced) {
            Script(w.cstart, w.cend, false, scope, depth + 1, "script");
            AddFold(w.start, w.end);
        } else {
            WordSubsts(c, w, scope, depth);
        }
    }

    void ExprArg(const tclparse::Command &c, const Word &w, int scope, int depth, bool allow_single) {
        if (w.braced) {
            Expr(w.cstart, w.cend, scope, depth + 1);
            return;
        }
        WordSubsts(c, w, scope, depth);
        if (w.literal) {
            // A literal word is the expression itself ("1", "true", "$x" never reaches here).
            Expr(w.cstart, w.cend, scope, depth + 1);
            return;
        }
        // Substitutions in an unbraced expression are substituted twice.
        const Token &tok = c.tokens[w.tok];
        const bool single = tok.num_components == 1 || (tok.num_components >= 1 && w.quoted == false &&
                                                         SingleSubst(c.tokens, w.tok));
        if (allow_single && single) return;  // `if $done`, `if [catch ...]`, `expr $e`
        AddDiag(w.start, w.end, TclLspSeverity::Warning, "unbraced-expr",
                "expression with substitutions is not braced: Tcl substitutes it twice and cannot compile it "
                "(write {...})");
    }

    static bool SingleSubst(const std::vector<Token> &tk, size_t word) {
        const Token &w = tk[word];
        if (word + 1 >= tk.size()) return false;
        const Token &first = tk[word + 1];
        return (first.type == TokType::Variable || first.type == TokType::Command) &&
               first.num_components + 1 == w.num_components;
    }

    void AddFold(int start, int end) {
        int l1 = 0, c1 = 0, l2 = 0, c2 = 0;
        d.Pos(start, &l1, &c1);
        d.Pos(end, &l2, &c2);
        if (l2 > l1) d.folds.push_back({start, end});
    }

    // --- Variables ---

    static bool LocalName(const std::string &n) { return !n.empty() && n.find("::") == std::string::npos; }

    // `a(b)` -> `a` and where it ends
    static std::string BaseName(const std::string &n) {
        const size_t p = n.find('(');
        return p == std::string::npos ? n : n.substr(0, p);
    }

    VarInfo &Var(int scope, const std::string &name) { return d.scopes[static_cast<size_t>(scope)].vars[name]; }

    void VarRef(int scope, const std::string &raw, int start, int end) {
        const std::string name = BaseName(raw);
        if (!LocalName(name)) return;
        VarInfo &v = Var(scope, name);
        v.read = true;
        v.sites.push_back({start, std::min(end, start + static_cast<int>(name.size())), false});
    }

    void VarDef(int scope, const Word &w, bool also_read = false) {
        if (!w.literal) {
            d.scopes[static_cast<size_t>(scope)].dynamic = true;
            return;
        }
        const std::string name = BaseName(w.value);
        if (!LocalName(name)) return;
        VarInfo &v = Var(scope, name);
        if (also_read || w.value.find('(') != std::string::npos) v.read = v.read || also_read;
        // The name's position: the word itself when written plainly.
        const int s = w.braced || w.quoted ? w.cstart : w.start;
        v.sites.push_back({s, s + static_cast<int>(name.size()), true});
        if (also_read) v.read = true;
        if (!also_read && !plain_def_) v.other_def = true;
    }

    void VarUse(int scope, const Word &w) { VarDef(scope, w, true); }

    void VarLinked(int scope, const Word &w, int link_scope) {
        if (!w.literal) return;
        const std::string name = BaseName(CommandTail(w.value));
        if (name.empty()) return;
        VarInfo &v = Var(scope, name);
        v.linked = true;
        if (link_scope >= 0) v.link_scope = link_scope;
        const int s = w.braced || w.quoted ? w.cstart : w.start;
        const int off = static_cast<int>(w.value.size() - name.size());
        if (w.value.find('(') == std::string::npos)
            v.sites.push_back({s + off, s + off + static_cast<int>(name.size()), true});
    }

    void ListDefs(int scope, const Word &w) {
        if (!w.literal) {
            d.scopes[static_cast<size_t>(scope)].dynamic = true;
            return;
        }
        std::vector<tclparse::ListElem> el;
        tclparse::Error e;
        const int s = w.braced || w.quoted ? w.cstart : w.start;
        const int en = w.braced || w.quoted ? w.cend : w.end;
        if (!tclparse::SplitList(t, s, en, &el, &e)) return;
        for (const tclparse::ListElem &x : el) {
            Word v;
            v.literal = true;
            v.value = tclparse::ElementValue(t, x);
            v.start = x.start;
            v.end = x.start + x.size;
            VarDef(scope, v);
        }
    }

    int NamespaceScope(const std::string &ns) {
        auto it = d.ns_scope.find(ns);
        if (it != d.ns_scope.end()) return it->second;
        Scope s;
        s.kind = Scope::Namespace;
        s.ns = ns;
        d.scopes.push_back(s);
        const int idx = static_cast<int>(d.scopes.size()) - 1;
        d.ns_scope[ns] = idx;
        return idx;
    }

    static std::string Qualify(const std::string &ns, const std::string &name) {
        if (name.compare(0, 2, "::") == 0) return name;
        if (ns == "::") return "::" + name;
        return ns + "::" + name;
    }

    std::string CommentAbove(int cmd_start) const {
        // The comment lines right above a command.
        int line = 0, col = 0;
        d.Pos(cmd_start, &line, &col);
        std::vector<std::string> out;
        for (int l = line - 1; l >= 0; l--) {
            const std::string &s = d.lines[static_cast<size_t>(l)];
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
            if (i >= s.size() || s[i] != '#') break;
            size_t j = i + 1;
            while (j < s.size() && s[j] == '#') j++;
            if (j < s.size() && s[j] == ' ') j++;
            out.push_back(s.substr(j));
        }
        std::reverse(out.begin(), out.end());
        std::string r;
        for (const std::string &s : out) r += s + "\n";
        return r;
    }

    // A proc-like definition: name (may be empty for constructors), parameter list, body.
    void DefineProc(const tclparse::Command &c, const Word *name, const Word &params, const Word &body, int scope,
                    int depth, bool method, const std::string &label) {
        const Scope &outer = d.scopes[static_cast<size_t>(scope)];
        Scope ps;
        ps.kind = Scope::Proc;
        ps.ns = outer.ns;
        ps.loose = method && class_ctx_ == 2;
        ps.body_start = body.cstart;
        ps.body_end = body.cend;
        ProcInfo pi;
        pi.method = method;
        if (name && name->literal) {
            pi.written = name->value;
            pi.qname = method ? class_name_ + " " + name->value : Qualify(outer.ns, name->value);
            pi.name_start = name->braced || name->quoted ? name->cstart : name->start;
            pi.name_end = pi.name_start + static_cast<int>(name->value.size());
            ps.name = name->value;
            if (!method) {
                // A proc runs in the namespace its qualified name names.
                const size_t q = pi.qname.rfind("::");
                ps.ns = q == 0 || q == std::string::npos ? "::" : pi.qname.substr(0, q);
                if (ps.ns.compare(0, 7, "::tcl::") == 0) d.extended.insert(CommandTail(ps.ns));
            }
        } else {
            pi.written = label;
            pi.qname = class_name_ + " " + label;
            pi.name_start = c.tokens[0].start;
            pi.name_end = pi.name_start + c.tokens[0].size;
            ps.name = label;
        }
        pi.cmd_start = c.command_start;
        pi.cmd_end = c.command_start + c.command_size;
        pi.doc = CommentAbove(c.command_start);
        d.scopes.push_back(ps);
        const int idx = static_cast<int>(d.scopes.size()) - 1;
        pi.scope = idx;
        if (ps.loose) d.scopes[static_cast<size_t>(idx)].dynamic = true;
        // Parameters.
        if (params.literal) {
            std::vector<tclparse::ListElem> el;
            tclparse::Error e;
            const int s = params.braced || params.quoted ? params.cstart : params.start;
            const int en = params.braced || params.quoted ? params.cend : params.end;
            if (tclparse::SplitList(t, s, en, &el, &e)) {
                for (const tclparse::ListElem &x : el) {
                    std::vector<tclparse::ListElem> sub;
                    tclparse::Error e2;
                    const std::string spec = tclparse::ElementValue(t, x);
                    if (!tclparse::SplitList(t, x.start, x.start + x.size, &sub, &e2) || sub.empty()) continue;
                    const std::string pname = tclparse::ElementValue(t, sub[0]);
                    VarInfo &v = d.scopes[static_cast<size_t>(idx)].vars[pname];
                    v.param = true;
                    v.sites.push_back({sub[0].start, sub[0].start + sub[0].size, true});
                    pi.params.push_back(sub.size() > 1 ? "?" + pname + "?" : pname);
                    if (!pi.params_label.empty()) pi.params_label += ' ';
                    pi.params_label += x.braced ? "{" + spec + "}" : spec;
                }
            }
        } else {
            WordSubsts(c, params, scope, depth);
        }
        // Instance variables of a TclOO class are visible once declared with `variable` there.
        if (method && class_ctx_ == 1) {
            for (const std::string &v : class_vars_) {
                VarInfo &vi = d.scopes[static_cast<size_t>(idx)].vars[v];
                vi.linked = true;
            }
        }
        d.procs.push_back(pi);
        const int pidx = static_cast<int>(d.procs.size()) - 1;
        (void)pidx;
        SymRec sym;
        sym.name = name && name->literal ? name->value : label;
        sym.detail = pi.params_label.empty() ? "" : "{" + pi.params_label + "}";
        sym.kind = method ? TclLspSymbolKind::Method : TclLspSymbolKind::Function;
        sym.start = c.command_start;
        sym.end = c.command_start + c.command_size;
        sym.sel_start = pi.name_start;
        sym.sel_end = pi.name_end;
        sym.parent = sym_parent_;
        d.syms.push_back(sym);
        d.regions.push_back({body.cstart, body.cend, idx});
        if (body.braced) {
            const int save_ctx = class_ctx_;
            const int save_parent = sym_parent_;
            class_ctx_ = 0;
            sym_parent_ = static_cast<int>(d.syms.size()) - 1;
            Script(body.cstart, body.cend, false, idx, depth + 1, "script");
            class_ctx_ = save_ctx;
            sym_parent_ = save_parent;
            AddFold(body.start, body.end);
        } else {
            WordSubsts(c, body, scope, depth);
            d.scopes[static_cast<size_t>(idx)].dynamic = true;
        }
    }

    void Detached(const tclparse::Command &c, const Word &w, int scope, int depth) {
        // A script that runs somewhere else (after, uplevel, bind...): parsed, its variables not ours.
        if (!w.braced) {
            WordSubsts(c, w, scope, depth);
            return;
        }
        Scope s;
        s.kind = Scope::Detached;
        s.ns = d.scopes[static_cast<size_t>(scope)].ns;
        s.dynamic = true;
        s.body_start = w.cstart;
        s.body_end = w.cend;
        d.scopes.push_back(s);
        const int idx = static_cast<int>(d.scopes.size()) - 1;
        d.regions.push_back({w.cstart, w.cend, idx});
        Script(w.cstart, w.cend, false, idx, depth + 1, "script");
        AddFold(w.start, w.end);
    }

    void ClassBody(const tclparse::Command &c, const Word &w, int scope, int depth, int ctx,
                   const std::string &name) {
        if (!w.braced) {
            WordSubsts(c, w, scope, depth);
            return;
        }
        const int save_ctx = class_ctx_;
        const std::string save_name = class_name_;
        const std::vector<std::string> save_vars = class_vars_;
        class_ctx_ = ctx;
        class_name_ = name;
        // TclOO: `variable a b c` in the definition declares instance variables for every method.
        if (ctx == 1) {
            tclparse::Script s = tclparse::ParseScript(t, w.cstart, w.cend, false);
            for (const tclparse::Command &cc : s.commands) {
                std::vector<Word> ws = Words(cc);
                if (ws.size() >= 2 && ws[0].literal && ws[0].value == "variable") {
                    for (size_t i = 1; i < ws.size(); i++) {
                        const std::string &v = ws[i].value;
                        if (ws[i].literal && v != "-set" && v != "-append" && v != "-prepend" && v != "-remove" &&
                            v != "-clear" && v != "--") {
                            class_vars_.push_back(v);
                        } else if (ws[i].expand && ws[i].end - ws[i].start > 5 &&
                                   t[static_cast<size_t>(ws[i].start) + 3] == '{') {
                            // variable -set {*}{a b c}
                            std::vector<tclparse::ListElem> el;
                            tclparse::Error e;
                            if (tclparse::SplitList(t, ws[i].start + 4, ws[i].end - 1, &el, &e))
                                for (const tclparse::ListElem &x : el) class_vars_.push_back(tclparse::ElementValue(t, x));
                        }
                    }
                }
            }
        }
        Script(w.cstart, w.cend, false, scope, depth + 1, "script");
        AddFold(w.start, w.end);
        class_ctx_ = save_ctx;
        class_name_ = save_name;
        class_vars_ = save_vars;
    }

    void Symbol(const std::string &name, const std::string &detail, TclLspSymbolKind kind,
                const tclparse::Command &c, const Word &sel) {
        SymRec s;
        s.name = name;
        s.detail = detail;
        s.kind = kind;
        s.start = c.command_start;
        s.end = c.command_start + c.command_size;
        s.sel_start = sel.start;
        s.sel_end = sel.end;
        s.parent = sym_parent_;
        d.syms.push_back(s);
    }

    // --- Commands ---

    void Command(const tclparse::Command &c, int scope, int depth) {
        std::vector<Word> w = Words(c);
        if (w.empty()) return;
        CmdRec rec;
        rec.start = c.command_start;
        rec.end = c.command_start + c.command_size;
        for (const Word &x : w) rec.words.push_back({x.start, x.end});
        rec.name = w[0].literal ? w[0].value : "";
        rec.scope = scope;
        d.cmds.push_back(rec);
        const bool any_expand = std::any_of(w.begin(), w.end(), [](const Word &x) { return x.expand; });
        std::vector<bool> handled(w.size(), false);
        auto H = [&](size_t i) {
            if (i < handled.size()) handled[i] = true;
        };
        const std::string written = w[0].literal ? w[0].value : "";
        if (!written.empty()) {
            CallInfo ci;
            ci.written = written;
            ci.ns = d.scopes[static_cast<size_t>(scope)].ns;
            ci.start = w[0].start;
            ci.end = w[0].end;
            d.calls.push_back(ci);
        }
        const std::string name = BuiltinName(written);
        Scope &sc = d.scopes[static_cast<size_t>(scope)];
        const size_t n = w.size();
        if (!any_expand) {
            if (name == "proc" && n == 4) {
                H(1), H(2), H(3);
                DefineProc(c, &w[1], w[2], w[3], scope, depth, false, "");
                if (w[1].literal) d.shadowed.insert(CommandTail(w[1].value));
            } else if (class_ctx_ && (name == "method" || name == "typemethod" || name == "proc") && n >= 4) {
                // `method name args ?init? body` (itcl/snit/TclOO)
                H(1);
                for (size_t i = 2; i < n; i++) H(i);
                DefineProc(c, &w[1], w[2], w[n - 1], scope, depth, true, "");
            } else if (class_ctx_ && name == "constructor" && n >= 3) {
                for (size_t i = 1; i < n; i++) H(i);
                DefineProc(c, nullptr, w[1], w[n - 1], scope, depth, true, "constructor");
            } else if (class_ctx_ && (name == "destructor" || name == "typeconstructor") && n == 2) {
                H(1);
                Word noparams;
                noparams.literal = true;
                DefineProc(c, nullptr, noparams, w[1], scope, depth, true, name);
            } else if (name == "if") {
                IfCmd(c, w, scope, depth, handled);
            } else if (name == "while" && n == 3) {
                H(1), H(2);
                ExprArg(c, w[1], scope, depth, true);
                ScriptArg(c, w[2], scope, depth);
            } else if (name == "for" && n == 5) {
                H(1), H(2), H(3), H(4);
                ScriptArg(c, w[1], scope, depth);
                ExprArg(c, w[2], scope, depth, true);
                ScriptArg(c, w[3], scope, depth);
                ScriptArg(c, w[4], scope, depth);
            } else if ((name == "foreach" || name == "lmap") && n >= 4 && n % 2 == 0) {
                for (size_t i = 1; i + 1 < n; i += 2) {
                    H(i);
                    ListDefs(scope, w[i]);
                }
                H(n - 1);
                ScriptArg(c, w[n - 1], scope, depth);
            } else if (name == "expr") {
                for (size_t i = 1; i < n; i++) H(i);
                if (n == 2) {
                    ExprArg(c, w[1], scope, depth, true);  // `expr $e` evaluates a string on purpose
                } else {
                    bool subst = false;
                    for (size_t i = 1; i < n; i++) {
                        WordSubsts(c, w[i], scope, depth);
                        if (!w[i].literal) subst = true;
                    }
                    if (subst)
                        AddDiag(w[1].start, w[n - 1].end, TclLspSeverity::Warning, "unbraced-expr",
                                "expression with substitutions is not braced: Tcl substitutes it twice and cannot "
                                "compile it (write {...})");
                }
            } else if (name == "catch" && n >= 2 && n <= 4) {
                H(1);
                ScriptArg(c, w[1], scope, depth);
                for (size_t i = 2; i < n; i++) {
                    H(i);
                    VarDef(scope, w[i]);
                }
            } else if ((name == "eval" || name == "time") && n >= 2) {
                if (name == "eval") sc.dynamic = true;
                if (n == 2 || name == "time") {
                    H(1);
                    ScriptArg(c, w[1], scope, depth);
                }
            } else if (name == "uplevel" && n >= 2) {
                H(n - 1);
                if (n == 3 && w[1].literal && (w[1].value == "0" || w[1].value == "#0")) sc.dynamic = true;
                if (n == 2 || n == 3) Detached(c, w[n - 1], scope, depth);
                else WordSubsts(c, w[n - 1], scope, depth);
            } else if (name == "namespace" && n >= 2 && w[1].literal) {
                NamespaceCmd(c, w, scope, depth, handled);
            } else if (name == "switch") {
                SwitchCmd(c, w, scope, depth, handled);
            } else if (name == "try") {
                TryCmd(c, w, scope, depth, handled);
            } else if (name == "dict" && n >= 2 && w[1].literal) {
                DictCmd(c, w, scope, depth, handled);
            } else if (name == "apply" && n >= 2 && w[1].braced) {
                H(1);
                std::vector<tclparse::ListElem> el;
                tclparse::Error e;
                if (tclparse::SplitList(t, w[1].cstart, w[1].cend, &el, &e) && (el.size() == 2 || el.size() == 3)) {
                    Word params, body;
                    params.literal = true;
                    params.value = tclparse::ElementValue(t, el[0]);
                    params.braced = el[0].braced;
                    params.start = params.cstart = el[0].start;
                    params.end = params.cend = el[0].start + el[0].size;
                    if (el[1].braced) {
                        body.braced = true;
                        body.cstart = el[1].start;
                        body.cend = el[1].start + el[1].size;
                        body.start = body.cstart - 1;
                        body.end = body.cend + 1;
                        DefineProc(c, nullptr, params, body, scope, depth, false, "apply");
                        d.syms.pop_back();
                        d.procs.pop_back();
                    }
                }
            } else if (name == "after" && n >= 2) {
                if (n == 3 && w[2].braced) {
                    H(2);
                    Detached(c, w[2], scope, depth);
                }
            } else if ((name == "oo::class" || name == "itcl::class" || name == "class") && n >= 3 && w[1].literal &&
                       (name != "class" || class_ctx_ == 0)) {
                if (name == "oo::class" && w[1].value == "create" && n >= 3) {
                    H(2);
                    if (w[2].literal) Symbol(w[2].value, "", TclLspSymbolKind::Class, c, w[2]);
                    if (n == 4) {
                        H(3);
                        const int save = sym_parent_;
                        sym_parent_ = static_cast<int>(d.syms.size()) - 1;
                        ClassBody(c, w[3], scope, depth, 1, w[2].literal ? w[2].value : "");
                        sym_parent_ = save;
                    }
                } else if (name != "oo::class" && n == 3) {
                    H(1), H(2);
                    Symbol(w[1].value, "", TclLspSymbolKind::Class, c, w[1]);
                    const int save = sym_parent_;
                    sym_parent_ = static_cast<int>(d.syms.size()) - 1;
                    ClassBody(c, w[2], scope, depth, 2, w[1].value);
                    sym_parent_ = save;
                }
            } else if ((name == "snit::type" || name == "snit::widget" || name == "snit::widgetadaptor" ||
                        name == "itcl::type") &&
                       n == 3 && w[1].literal) {
                H(1), H(2);
                Symbol(w[1].value, "", TclLspSymbolKind::Class, c, w[1]);
                const int save = sym_parent_;
                sym_parent_ = static_cast<int>(d.syms.size()) - 1;
                ClassBody(c, w[2], scope, depth, 2, w[1].value);
                sym_parent_ = save;
            } else if ((name == "oo::define" || name == "oo::objdefine") && n == 3 && w[1].literal) {
                H(2);
                ClassBody(c, w[2], scope, depth, 1, w[1].value);
            } else if ((name == "oo::define" || name == "oo::objdefine") && n >= 4 && w[2].literal) {
                const std::string kw = w[2].value;
                const std::string save = class_name_;
                const int save_ctx = class_ctx_;
                class_name_ = w[1].literal ? w[1].value : "";
                class_ctx_ = 1;
                if ((kw == "method") && n == 6) {
                    for (size_t i = 3; i < n; i++) H(i);
                    DefineProc(c, &w[3], w[4], w[5], scope, depth, true, "");
                } else if (kw == "constructor" && n == 5) {
                    H(3), H(4);
                    DefineProc(c, nullptr, w[3], w[4], scope, depth, true, "constructor");
                }
                class_name_ = save;
                class_ctx_ = save_ctx;
            } else if (name == "test" && n >= 4 && w[1].literal) {
                TestCmd(c, w, scope, depth, handled);
            } else {
                VarCommands(c, w, name, scope, depth, handled);
            }
        }
        // Everything not handled: substitutions only.
        for (size_t i = 1; i < n; i++)
            if (!handled[i]) WordSubsts(c, w[i], scope, depth);
        if (!w[0].literal) {
            WordSubsts(c, w[0], scope, depth);
            if (sc.kind == Scope::Proc) {
                // `$cmd args` may be anything.
            }
        }
        // Inside a class definition, or a namespace of Tcl's own ensembles (::tcl::dict has its own `for`),
        // a name may mean something else.
        const std::string &cns = d.scopes[static_cast<size_t>(scope)].ns;
        if (!any_expand && !name.empty() && class_ctx_ == 0 && cns.compare(0, 7, "::tcl::") != 0)
            Lints(c, w, name, scope);
    }

    void IfCmd(const tclparse::Command &c, const std::vector<Word> &w, int scope, int depth,
               std::vector<bool> &handled) {
        size_t i = 1;
        const size_t n = w.size();
        std::vector<std::pair<size_t, bool>> plan;  // word, is expression
        while (true) {
            if (i >= n) return;  // malformed: Tcl reports it at run time
            plan.push_back({i, true});
            i++;
            if (i < n && w[i].literal && w[i].value == "then") i++;
            if (i >= n) return;
            plan.push_back({i, false});
            i++;
            if (i >= n) break;
            if (w[i].literal && w[i].value == "elseif") {
                i++;
                continue;
            }
            if (w[i].literal && w[i].value == "else") i++;
            if (i >= n) return;
            plan.push_back({i, false});
            i++;
            if (i != n) return;
            break;
        }
        for (auto [k, is_expr] : plan) {
            handled[k] = true;
            if (is_expr) ExprArg(c, w[k], scope, depth, true);
            else ScriptArg(c, w[k], scope, depth);
        }
    }

    void NamespaceCmd(const tclparse::Command &c, const std::vector<Word> &w, int scope, int depth,
                      std::vector<bool> &handled) {
        const std::string sub = ResolveSub("namespace", w[1].value);
        const size_t n = w.size();
        if (sub == "eval" && n == 4 && w[2].literal) {
            handled[2] = handled[3] = true;
            const std::string ns = Qualify(d.scopes[static_cast<size_t>(scope)].ns == "::" ||
                                                   d.scopes[static_cast<size_t>(scope)].kind != Scope::Proc
                                               ? d.scopes[static_cast<size_t>(scope)].ns
                                               : d.scopes[static_cast<size_t>(scope)].ns,
                                           w[2].value);
            Symbol(w[2].value, "", TclLspSymbolKind::Namespace, c, w[2]);
            const int nsidx = NamespaceScope(ns);
            if (w[3].braced) {
                d.regions.push_back({w[3].cstart, w[3].cend, nsidx});
                const int save = sym_parent_;
                sym_parent_ = static_cast<int>(d.syms.size()) - 1;
                Script(w[3].cstart, w[3].cend, false, nsidx, depth + 1, "script");
                sym_parent_ = save;
                AddFold(w[3].start, w[3].end);
            } else {
                WordSubsts(c, w[3], scope, depth);
            }
        } else if (sub == "ensemble" && n >= 3) {
            for (size_t i = 3; i < n; i++)
                if (w[i].literal && w[i].value.find("::") == std::string::npos) d.extended.insert(w[i].value);
            if (n >= 4 && w[3].literal) d.extended.insert(CommandTail(w[3].value));
        } else if (sub == "upvar" && n >= 4) {
            for (size_t i = 3; i + 1 < n; i += 2) {
                handled[i + 1] = true;
                VarLinked(scope, w[i + 1], -1);
            }
        }
    }

    void SwitchCmd(const tclparse::Command &c, const std::vector<Word> &w, int scope, int depth,
                   std::vector<bool> &handled) {
        size_t i = 1;
        const size_t n = w.size();
        bool regexp = false;
        while (i < n && w[i].literal && !w[i].value.empty() && w[i].value[0] == '-') {
            const std::string o = w[i].value;
            handled[i] = true;
            if (o == "--") {
                i++;
                break;
            }
            if (o == "-regexp") regexp = true;
            if (o == "-matchvar" || o == "-indexvar") {
                if (i + 1 < n) {
                    handled[i + 1] = true;
                    VarDef(scope, w[i + 1]);
                }
                i += 2;
                continue;
            }
            i++;
        }
        (void)regexp;
        if (i >= n) return;
        i++;  // the string
        if (n - i == 1 && w[i].braced) {
            handled[i] = true;
            std::vector<tclparse::ListElem> el;
            tclparse::Error e;
            if (!tclparse::SplitList(t, w[i].cstart, w[i].cend, &el, &e) || el.size() % 2 != 0) return;
            for (size_t k = 1; k < el.size(); k += 2) {
                const std::string body = tclparse::ElementValue(t, el[k]);
                if (body == "-") continue;
                if (el[k].braced || el[k].literal) {
                    Script(el[k].start, el[k].start + el[k].size, false, scope, depth + 1, "script");
                    AddFold(el[k].start - 1, el[k].start + el[k].size + 1);
                }
            }
            AddFold(w[i].start, w[i].end);
        } else if ((n - i) % 2 == 0) {
            for (size_t k = i + 1; k < n; k += 2) {
                if (w[k].literal && w[k].value == "-") continue;
                handled[k] = true;
                ScriptArg(c, w[k], scope, depth);
            }
        }
    }

    void TryCmd(const tclparse::Command &c, const std::vector<Word> &w, int scope, int depth,
                std::vector<bool> &handled) {
        const size_t n = w.size();
        if (n < 2) return;
        handled[1] = true;
        ScriptArg(c, w[1], scope, depth);
        size_t i = 2;
        while (i < n) {
            if (!w[i].literal) return;
            const std::string k = w[i].value;
            if ((k == "on" || k == "trap") && i + 3 < n) {
                handled[i + 2] = handled[i + 3] = true;
                ListDefs(scope, w[i + 2]);
                if (!(w[i + 3].literal && w[i + 3].value == "-")) ScriptArg(c, w[i + 3], scope, depth);
                i += 4;
            } else if (k == "finally" && i + 1 < n) {
                handled[i + 1] = true;
                ScriptArg(c, w[i + 1], scope, depth);
                i += 2;
            } else {
                return;
            }
        }
    }

    void DictCmd(const tclparse::Command &c, const std::vector<Word> &w, int scope, int depth,
                 std::vector<bool> &handled) {
        const std::string sub = ResolveSub("dict", w[1].value);
        const size_t n = w.size();
        if ((sub == "for" || sub == "map") && n == 5) {
            handled[2] = handled[4] = true;
            ListDefs(scope, w[2]);
            ScriptArg(c, w[4], scope, depth);
        } else if (sub == "with" && n >= 4) {
            handled[2] = handled[n - 1] = true;
            VarUse(scope, w[2]);
            d.scopes[static_cast<size_t>(scope)].dynamic = true;
            ScriptArg(c, w[n - 1], scope, depth);
        } else if (sub == "update" && n >= 6 && n % 2 == 0) {
            handled[2] = handled[n - 1] = true;
            VarUse(scope, w[2]);
            for (size_t i = 4; i + 1 < n; i += 2) {
                handled[i] = true;
                VarDef(scope, w[i]);
            }
            ScriptArg(c, w[n - 1], scope, depth);
        } else if ((sub == "set" || sub == "unset" || sub == "lappend" || sub == "append" || sub == "incr") && n >= 3) {
            handled[2] = true;
            VarUse(scope, w[2]);
        } else if (sub == "filter" && n == 6 && w[3].literal && (w[3].value == "script" || w[3].value == "s")) {
            handled[4] = handled[5] = true;
            ListDefs(scope, w[4]);
            ScriptArg(c, w[5], scope, depth);
        }
    }

    void TestCmd(const tclparse::Command &c, const std::vector<Word> &w, int scope, int depth,
                 std::vector<bool> &handled) {
        // tcltest: test name description ?constraints? body result, or with -setup/-body/-cleanup.
        const size_t n = w.size();
        bool options = false;
        for (size_t i = 3; i < n; i++)
            if (w[i].literal && (w[i].value == "-body" || w[i].value == "-setup" || w[i].value == "-cleanup"))
                options = true;
        if (options) {
            for (size_t i = 3; i + 1 < n; i++) {
                if (w[i].literal && (w[i].value == "-body" || w[i].value == "-setup" || w[i].value == "-cleanup")) {
                    handled[i + 1] = true;
                    ScriptArg(c, w[i + 1], scope, depth);
                    i++;
                }
            }
        } else if (n == 5 || n == 6) {
            handled[n - 2] = true;
            ScriptArg(c, w[n - 2], scope, depth);
        }
    }

    // Commands that name variables.
    void VarCommands(const tclparse::Command &c, const std::vector<Word> &w, const std::string &name, int scope,
                     int depth, std::vector<bool> &handled) {
        (void)c, (void)depth;
        const size_t n = w.size();
        Scope &sc = d.scopes[static_cast<size_t>(scope)];
        auto def = [&](size_t i) {
            if (i < n) {
                handled[i] = true;
                VarDef(scope, w[i]);
            }
        };
        auto use = [&](size_t i) {
            if (i < n) {
                handled[i] = true;
                VarUse(scope, w[i]);
            }
        };
        if (name == "set") {
            if (n == 3) {
                plain_def_ = true;
                def(1);
                plain_def_ = false;
            } else if (n == 2) {
                use(1);
            }
        } else if (name == "incr" || name == "append" || name == "lappend" || name == "lset" || name == "unset" ||
                   name == "vwait") {
            if (name == "unset") {
                for (size_t i = 1; i < n; i++)
                    if (!(w[i].literal && w[i].value.compare(0, 1, "-") == 0)) use(i);
            } else if (n >= 2) {
                use(1);
            }
        } else if (name == "global") {
            for (size_t i = 1; i < n; i++) {
                handled[i] = true;
                VarLinked(scope, w[i], 0);
            }
        } else if (name == "variable") {
            if (sc.kind == Scope::Proc) {
                const int nsidx = NamespaceScope(sc.ns);
                if (n == 2) {
                    handled[1] = true;
                    VarLinked(scope, w[1], nsidx);
                } else {
                    for (size_t i = 1; i < n; i += 2) {
                        handled[i] = true;
                        VarLinked(scope, w[i], nsidx);
                    }
                }
            } else {
                for (size_t i = 1; i < n; i += 2) def(i);
                if (n >= 2 && w[1].literal) {
                    Symbol(w[1].value, "", TclLspSymbolKind::Variable, c, w[1]);
                }
            }
        } else if (name == "upvar") {
            size_t i = 1;
            if (n % 2 == 0) i = 2;  // a level
            if (n == 4 && w[1].literal && (w[1].value == "0" || w[1].value == "#0")) {
                // aliases inside this frame
            }
            for (; i + 1 < n; i += 2) {
                handled[i + 1] = true;
                VarLinked(scope, w[i + 1], -1);
            }
        } else if (name == "gets" && n == 3) {
            def(2);
        } else if (name == "scan" && n >= 4) {
            for (size_t i = 3; i < n; i++) def(i);
        } else if (name == "lassign" && n >= 3) {
            for (size_t i = 2; i < n; i++) def(i);
        } else if (name == "regexp" || name == "regsub") {
            size_t i = 1;
            while (i < n && w[i].literal && w[i].value.compare(0, 1, "-") == 0) {
                const bool stop = w[i].value == "--";
                if (w[i].value == "-start") i++;
                i++;
                if (stop) break;
            }
            // exp string vars... (regsub: exp string subSpec ?varName?)
            if (name == "regexp") {
                for (size_t k = i + 2; k < n; k++) def(k);
            } else if (i + 3 < n) {
                def(i + 3);
            }
        } else if (name == "binary" && n >= 5 && w[1].literal && ResolveSub("binary", w[1].value) == "scan") {
            for (size_t i = 4; i < n; i++) def(i);
        } else if (name == "info" && n >= 3 && w[1].literal) {
            const std::string sub = ResolveSub("info", w[1].value);
            if (sub == "exists") use(2);
            if (sub == "locals" || sub == "vars" || sub == "level") sc.dynamic = true;
            if (sub == "default" && n == 5) def(4);
        } else if (name == "info" && n == 2 && w[1].literal) {
            const std::string sub = ResolveSub("info", w[1].value);
            if (sub == "locals" || sub == "vars" || sub == "level") sc.dynamic = true;
        } else if (name == "array" && n >= 3) {
            use(2);
        } else if (name == "trace" && n >= 4 && w[1].literal) {
            const std::string sub = ResolveSub("trace", w[1].value);
            if (sub == "add" || sub == "remove" || sub == "info") {
                if (w[2].literal && w[2].value == "variable") use(3);
            } else if (sub == "variable" || sub == "vdelete" || sub == "vinfo") {
                use(2);
            }
        } else if (name == "subst") {
            sc.dynamic = true;
        } else if (name == "rename" && n == 3) {
            if (w[1].literal) d.shadowed.insert(CommandTail(w[1].value));
            if (w[2].literal) d.shadowed.insert(CommandTail(w[2].value));
        } else if (name == "interp" && n >= 4 && w[1].literal && w[1].value == "alias") {
            if (w[3].literal) d.shadowed.insert(CommandTail(w[3].value));
        } else if (name == "catch" || name == "foreach" || name == "lmap") {
            // malformed: substitutions only
        } else if (name == "chan" || name == "fileevent" || name == "bind" || name == "trace") {
            // callbacks are strings
        }
        // A command word that is a variable's name passed to a proc (`helper resultVar`) counts as a use.
    }

    // --- Lints that need the whole command ---

    void Lints(const tclparse::Command &c, const std::vector<Word> &w, const std::string &name, int scope) {
        (void)c;
        const TclLspApi *a = Api(name);
        if (!a) return;
        const std::string kind = a->kind;
        int nargs = static_cast<int>(w.size()) - 1;
        const TclLspApi *target = a;
        std::string label = name;
        if (kind == "ensemble" && w.size() >= 2 && w[1].literal && V().subs.count(name)) {
            bool ambiguous = false;
            const std::string sub = ResolveSub(name, w[1].value, &ambiguous);
            if (sub.empty()) {
                ArityCheck ac;
                ac.name = name;
                ac.diag.start = w[1].start;
                ac.diag.end = w[1].end;
                ac.diag.sev = TclLspSeverity::Warning;
                ac.diag.code = "unknown-subcommand";
                ac.sub = name + " " + w[1].value;
                std::string list;
                const std::vector<std::string> &subs = V().subs.at(name);
                for (size_t i = 0; i < subs.size(); i++) {
                    if (i) list += i + 1 == subs.size() ? ", or " : ", ";
                    list += subs[i];
                }
                ac.diag.message = "unknown or ambiguous subcommand \"" + w[1].value +
                                  "\": must be " + list;
                d.arity.push_back(ac);
                return;
            }
            target = Api(name + " " + sub);
            if (!target) return;
            nargs--;
            label = name + " " + sub;
        }
        if (!Accepts(*target, nargs)) {
            ArityCheck ac;
            ac.name = name;
            ac.diag.start = w[0].start;
            ac.diag.end = w.back().end;
            ac.diag.sev = TclLspSeverity::Error;
            ac.diag.code = "wrong-args";
            ac.diag.message = std::string("wrong # args: should be \"") + target->usage + "\"";
            d.arity.push_back(ac);
        }
        MissingDollar(w, name, label, scope);
    }

    void MissingDollar(const std::vector<Word> &w, const std::string &name, const std::string &label, int scope) {
        const Scope &sc = d.scopes[static_cast<size_t>(scope)];
        if (sc.kind != Scope::Proc) return;
        // Positions that take a value, never a name.
        std::vector<size_t> pos;
        const size_t n = w.size();
        if (name == "return" && n == 2) pos = {1};
        else if (name == "llength" && n == 2) pos = {1};
        else if (name == "lindex" && n >= 2) pos = {1};
        else if (name == "lrange" && n == 4) pos = {1};
        else if (name == "lsort" && n >= 2) pos = {n - 1};
        else if (name == "join" && n >= 2) pos = {1};
        else if (label == "string length" && n == 3) pos = {2};
        else if (label == "dict get" && n >= 3) pos = {2};
        else if (label == "dict exists" && n >= 3) pos = {2};
        for (size_t i : pos) {
            const Word &x = w[i];
            if (!x.literal || x.braced || x.quoted || x.value.empty()) continue;
            bool bare = true;
            for (char ch : x.value)
                if (!tclparse::IsBareword(ch)) bare = false;
            if (!bare || std::isdigit(static_cast<unsigned char>(x.value[0]))) continue;
            auto it = sc.vars.find(x.value);
            if (it == sc.vars.end()) continue;
            bool before = false;
            for (const VarSite &s : it->second.sites)
                if (s.def && s.start < x.start) before = true;
            if (!before) continue;
            AddDiag(x.start, x.end, TclLspSeverity::Warning, "missing-dollar",
                    "\"" + x.value + "\" is a variable here: did you mean \"$" + x.value + "\"?");
        }
    }

    // --- After the walk ---

    bool MentionedElsewhere(const Scope &sc, const std::string &name, const VarInfo &v) const {
        // Any `$name`, `${name}` or bare `name` word in the body other than the definitions.
        const int s = sc.body_start, e = sc.body_end;
        size_t pos = static_cast<size_t>(s);
        while (true) {
            pos = t.find(name, pos);
            if (pos == std::string::npos || static_cast<int>(pos) >= e) return false;
            const int p = static_cast<int>(pos);
            const int q = p + static_cast<int>(name.size());
            pos++;
            const char before = p > 0 ? t[static_cast<size_t>(p - 1)] : ' ';
            const char after = q < static_cast<int>(t.size()) ? t[static_cast<size_t>(q)] : ' ';
            if (IsVarNameChar(before) && before != ':') continue;
            if (IsVarNameChar(after) && after != ':') continue;
            bool is_def = false;
            for (const VarSite &vs : v.sites)
                if (vs.start == p) is_def = true;
            if (is_def) continue;
            return true;
        }
    }

    void Finish() {
        for (const Scope &sc : d.scopes) {
            if (sc.kind != Scope::Proc || sc.dynamic) continue;
            for (const auto &[name, v] : sc.vars) {
                if (v.param || v.linked || v.read || v.other_def || v.sites.empty()) continue;
                if (MentionedElsewhere(sc, name, v)) continue;
                const VarSite &first = v.sites.front();
                AddDiag(first.start, first.end, TclLspSeverity::Warning, "unused-variable",
                        "variable \"" + name + "\" is set but never read", true);
            }
        }
        for (const ArityCheck &ac : d.arity) {
            if (d.shadowed.count(ac.name)) continue;
            if (d.extended.count(ac.name)) continue;
            if (ac.diag.code == "unknown-subcommand" && Tcl9Sub(ac.sub)) continue;
            d.diags.push_back(ac.diag);
        }
        std::sort(d.diags.begin(), d.diags.end(), [](const Diag &a, const Diag &b) {
            return a.start != b.start ? a.start < b.start : a.code < b.code;
        });
    }
};

// --- Lookups for the features -----------------------------------------------------------

struct VarTarget {
    bool found = false;
    int scope = -1;
    std::string name;
    VarSite site;
};

// The scope that owns a variable seen in `scope` (following global/variable links).
std::pair<int, std::string> Owner(const TclLspDocument &d, int scope, const std::string &name) {
    const Scope &sc = d.scopes[static_cast<size_t>(scope)];
    auto it = sc.vars.find(name);
    if (it != sc.vars.end() && it->second.linked && it->second.link_scope >= 0) return {it->second.link_scope, name};
    return {scope, name};
}

VarTarget VarAt(const TclLspDocument &d, int off) {
    VarTarget out;
    for (size_t si = 0; si < d.scopes.size(); si++) {
        for (const auto &[name, v] : d.scopes[si].vars) {
            for (const VarSite &s : v.sites) {
                if (off >= s.start && off <= s.end) {
                    out.found = true;
                    out.scope = static_cast<int>(si);
                    out.name = name;
                    out.site = s;
                    return out;
                }
            }
        }
    }
    return out;
}

// Every site of the variable (scope, name) everywhere it is visible.
std::vector<VarSite> VarSites(const TclLspDocument &d, int scope, const std::string &name) {
    const auto [owner, oname] = Owner(d, scope, name);
    std::vector<VarSite> out;
    for (size_t si = 0; si < d.scopes.size(); si++) {
        auto it = d.scopes[si].vars.find(oname);
        if (it == d.scopes[si].vars.end()) continue;
        const auto [o2, n2] = Owner(d, static_cast<int>(si), oname);
        if (o2 != owner) continue;
        for (const VarSite &s : it->second.sites) out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const VarSite &a, const VarSite &b) { return a.start < b.start; });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const VarSite &a, const VarSite &b) { return a.start == b.start; }),
              out.end());
    return out;
}

const ProcInfo *ResolveProc(const TclLspDocument &d, const std::string &written, const std::string &ns) {
    std::vector<std::string> cands;
    if (written.compare(0, 2, "::") == 0) {
        cands.push_back(written);
    } else {
        if (ns != "::") cands.push_back(ns + "::" + written);
        cands.push_back("::" + written);
    }
    for (const std::string &q : cands)
        for (const ProcInfo &p : d.procs)
            if (!p.method && p.qname == q) return &p;
    return nullptr;
}

const CallInfo *CallAt(const TclLspDocument &d, int off) {
    for (const CallInfo &c : d.calls)
        if (off >= c.start && off <= c.end) return &c;
    return nullptr;
}

const ProcInfo *ProcDefAt(const TclLspDocument &d, int off) {
    for (const ProcInfo &p : d.procs)
        if (!p.method && off >= p.name_start && off <= p.name_end) return &p;
    return nullptr;
}

const ProcInfo *ProcTargetAt(const TclLspDocument &d, int off) {
    if (const ProcInfo *p = ProcDefAt(d, off)) return p;
    if (const CallInfo *c = CallAt(d, off)) return ResolveProc(d, c->written, c->ns);
    return nullptr;
}

const CmdRec *InnermostCmd(const TclLspDocument &d, int off) {
    const CmdRec *best = nullptr;
    for (const CmdRec &c : d.cmds) {
        if (off >= c.start && off <= c.end && (!best || c.end - c.start <= best->end - best->start)) best = &c;
    }
    return best;
}

std::string ApiHover(const TclLspApi &a) {
    std::string s = "```tcl\n" + std::string(a.usage) + "\n```";
    if (a.doc && *a.doc) s += "\n\n" + std::string(a.doc);
    return s;
}

std::string ProcHover(const ProcInfo &p) {
    std::string s = "```tcl\n" + std::string(p.method ? "method " : "proc ") + p.qname + " {" + p.params_label +
                    "}\n```";
    if (!p.doc.empty()) s += "\n\n" + p.doc;
    return s;
}

bool IsCommandPosition(const std::string &line, int start) {
    int i = start - 1;
    while (i >= 0 && (line[static_cast<size_t>(i)] == ' ' || line[static_cast<size_t>(i)] == '\t')) i--;
    if (i < 0) return true;
    const char c = line[static_cast<size_t>(i)];
    return c == '[' || c == '{' || c == ';';
}

TclLspKind KindOf(const TclLspApi &a) {
    const std::string k = a.kind;
    if (k == "variable") return TclLspKind::Variable;
    if (k == "function") return TclLspKind::Function;
    if (k == "subcommand") return TclLspKind::Method;
    if (k == "ensemble") return TclLspKind::Module;
    return TclLspKind::Function;
}

}  // namespace

std::shared_ptr<const TclLspDocument> TclLspAnalyze(const std::vector<std::string> &lines) {
    auto doc = std::make_shared<TclLspDocument>();
    doc->lines = lines;
    int off = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        doc->line_starts.push_back(off);
        if (i) doc->text += '\n';
        doc->text += lines[i];
        off += static_cast<int>(lines[i].size()) + 1;
    }
    if (doc->line_starts.empty()) doc->line_starts.push_back(0);
    Analyzer(*doc).Run();
    return doc;
}

std::string TclLspParseReport(const std::string &text) {
    TclLspDocument doc;
    doc.text = text;
    size_t i = 0;
    while (true) {
        doc.line_starts.push_back(static_cast<int>(i));
        const size_t nl = text.find('\n', i);
        if (nl == std::string::npos) {
            doc.lines.push_back(text.substr(i));
            break;
        }
        doc.lines.push_back(text.substr(i, nl - i));
        i = nl + 1;
    }
    doc.want_report = true;
    Analyzer(doc).Run();
    std::string out;
    for (const ReportSpan &r : doc.report) {
        out += r.kind + " " + std::to_string(r.start) + " " + std::to_string(r.end);
        if (r.error.failed) {
            std::string m = r.error.message;
            std::string esc;
            for (char c : m) {
                if (c == '\n') esc += "\\n";
                else if (c == '\\') esc += "\\\\";
                else esc += c;
            }
            out += " error " + std::to_string(r.error.pos) + " " + esc;
        } else {
            out += " ok";
        }
        out += "\n";
    }
    return out;
}

std::vector<TclLspDiagnostic> TclLspDiagnostics(const TclLspDocument &doc) {
    std::vector<TclLspDiagnostic> out;
    for (const Diag &g : doc.diags) {
        TclLspDiagnostic d;
        d.range = doc.Range(g.start, g.end);
        d.severity = g.sev;
        d.code = g.code;
        d.message = g.message;
        d.unnecessary = g.unnecessary;
        out.push_back(d);
    }
    return out;
}

std::string TclLspFormatDiagnostic(const std::string &path, const TclLspDiagnostic &d) {
    const char *sev = d.severity == TclLspSeverity::Error         ? "error"
                      : d.severity == TclLspSeverity::Warning     ? "warning"
                      : d.severity == TclLspSeverity::Information ? "info"
                                                                  : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool TclLspDiagnosticFails(const TclLspDiagnostic &d) {
    return d.severity == TclLspSeverity::Error || d.severity == TclLspSeverity::Warning;
}

std::vector<TclLspCompletionItem> TclLspCompletions(const TclLspDocument &doc, int line, int col) {
    std::vector<TclLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return out;
    const std::string &cur = doc.lines[static_cast<size_t>(line)];
    col = std::max(0, std::min(col, static_cast<int>(cur.size())));
    int start = col;
    while (start > 0 && IsVarNameChar(cur[static_cast<size_t>(start - 1)])) start--;
    const std::string prefix = cur.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    std::set<std::string> seen;
    auto add = [&](const std::string &label, TclLspKind kind, const std::string &detail, const std::string &docs) {
        if (label.compare(0, prefix.size(), prefix) != 0 || label == prefix || !seen.insert(label).second) return;
        TclLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = docs;
        it.replace_start = start;
        it.replace_end = col;
        out.push_back(it);
    };
    const int off = doc.Off(line, start);
    const int scope = doc.ScopeAt(off);
    const Vocab &v = V();
    // $name: the variables visible here.
    int dollar = start - 1;
    if (dollar >= 0 && cur[static_cast<size_t>(dollar)] == '{') dollar--;
    if (dollar >= 0 && cur[static_cast<size_t>(dollar)] == '$') {
        const Scope &sc = doc.scopes[static_cast<size_t>(scope)];
        for (const auto &[name, vi] : sc.vars)
            add(name, TclLspKind::Variable, vi.param ? "parameter" : (vi.linked ? "linked" : "local"), "");
        if (sc.kind != Scope::Proc) {
            for (const auto &[name, idx] : v.vars) add(name, TclLspKind::Variable, "Tcl", v.api[idx].doc);
        }
        return out;
    }
    // A subcommand of an ensemble.
    {
        int e = start - 1;
        while (e >= 0 && (cur[static_cast<size_t>(e)] == ' ' || cur[static_cast<size_t>(e)] == '\t')) e--;
        int b = e;
        while (b >= 0 && IsVarNameChar(cur[static_cast<size_t>(b)])) b--;
        if (e > b && e < start - 1) {
            const std::string prev = BuiltinName(cur.substr(static_cast<size_t>(b + 1), static_cast<size_t>(e - b)));
            auto it = v.subs.find(prev);
            if (it != v.subs.end() && IsCommandPosition(cur, b + 1)) {
                for (const std::string &s : it->second) {
                    const TclLspApi *a = Api(prev + " " + s);
                    add(s, TclLspKind::Method, a ? a->usage : "", a ? a->doc : "");
                }
                return out;
            }
        }
    }
    if (IsCommandPosition(cur, start)) {
        for (const ProcInfo &p : doc.procs)
            if (!p.method) add(p.written, TclLspKind::Function, "proc {" + p.params_label + "}", p.doc);
        for (size_t i = 0; i < v.count; i++) {
            const std::string k = v.api[i].kind;
            if (k == "command" || k == "ensemble") add(v.api[i].name, KindOf(v.api[i]), v.api[i].usage, v.api[i].doc);
        }
        return out;
    }
    // Otherwise: expr functions inside expressions, then the document's words.
    for (const auto &[name, idx] : v.funcs) add(name, TclLspKind::Function, v.api[idx].usage, v.api[idx].doc);
    for (size_t li = 0; li < doc.lines.size(); li++) {
        const std::string &l = doc.lines[li];
        for (size_t i = 0; i < l.size();) {
            if (!IsVarNameChar(l[i]) || std::isdigit(static_cast<unsigned char>(l[i]))) {
                i++;
                continue;
            }
            size_t j = i;
            while (j < l.size() && IsVarNameChar(l[j])) j++;
            const bool at_cursor = li == static_cast<size_t>(line) && static_cast<int>(i) == start;
            if (!at_cursor) add(l.substr(i, j - i), TclLspKind::Text, "", "");
            i = j;
        }
    }
    return out;
}

TclLspHoverInfo TclLspHover(const TclLspDocument &doc, int line, int col) {
    TclLspHoverInfo h;
    const int off = doc.Off(line, col);
    if (const ProcInfo *p = ProcDefAt(doc, off)) {
        h.found = true;
        h.text = ProcHover(*p);
        h.range = doc.Range(p->name_start, p->name_end);
        return h;
    }
    const VarTarget vt = VarAt(doc, off);
    if (vt.found) {
        const Scope &sc = doc.scopes[static_cast<size_t>(vt.scope)];
        const VarInfo &vi = sc.vars.at(vt.name);
        std::string what = vi.param ? "parameter" : vi.linked ? "variable (linked)" : sc.kind == Scope::Proc ? "local variable" : "variable";
        h.found = true;
        h.text = "```tcl\n" + what + " " + vt.name + "\n```";
        if (sc.kind == Scope::Proc && !sc.name.empty()) h.text += "\n\nin `" + sc.name + "`";
        auto it = V().vars.find(vt.name);
        if (sc.kind != Scope::Proc && it != V().vars.end()) h.text += "\n\n" + std::string(V().api[it->second].doc);
        h.range = doc.Range(vt.site.start, vt.site.end);
        return h;
    }
    for (const Span &f : doc.funcs) {
        if (off >= f.start && off <= f.end) {
            const std::string name(doc.text, static_cast<size_t>(f.start), static_cast<size_t>(f.end - f.start));
            auto it = V().funcs.find(name);
            if (it == V().funcs.end()) return h;
            h.found = true;
            h.text = ApiHover(V().api[it->second]);
            h.range = doc.Range(f.start, f.end);
            return h;
        }
    }
    const CmdRec *c = InnermostCmd(doc, off);
    if (!c || c->words.empty()) return h;
    for (size_t i = 0; i < c->words.size() && i < 2; i++) {
        const Span &w = c->words[i];
        if (off < w.start || off > w.end) continue;
        if (i == 0) {
            if (const ProcInfo *p = ResolveProc(doc, c->name, doc.scopes[static_cast<size_t>(c->scope)].ns)) {
                h.found = true;
                h.text = ProcHover(*p);
            } else if (const TclLspApi *a = Api(BuiltinName(c->name))) {
                h.found = true;
                h.text = ApiHover(*a);
            }
        } else {
            const std::string ens = BuiltinName(c->name);
            const std::string word(doc.text, static_cast<size_t>(w.start), static_cast<size_t>(w.end - w.start));
            const std::string sub = ResolveSub(ens, word);
            if (const TclLspApi *a = sub.empty() ? nullptr : Api(ens + " " + sub)) {
                h.found = true;
                h.text = ApiHover(*a);
            }
        }
        if (h.found) h.range = doc.Range(w.start, w.end);
        return h;
    }
    return h;
}

TclLspSignatureHelp TclLspSignatureAt(const TclLspDocument &doc, int line, int col) {
    TclLspSignatureHelp out;
    const int off = doc.Off(line, col);
    // The innermost command whose name is not under the cursor.
    const CmdRec *best = nullptr;
    for (const CmdRec &c : doc.cmds) {
        if (c.words.empty() || off <= c.words[0].end || off > c.end + 1) continue;
        if (!best || c.end - c.start <= best->end - best->start) best = &c;
    }
    if (!best) return out;
    int arg = 0;
    for (size_t i = 1; i < best->words.size(); i++)
        if (off > best->words[i - 1].end) arg = static_cast<int>(i) - 1;
    if (off > best->words.back().end) arg = static_cast<int>(best->words.size()) - 1;
    const std::string name = BuiltinName(best->name);
    if (const ProcInfo *p = ResolveProc(doc, best->name, doc.scopes[static_cast<size_t>(best->scope)].ns)) {
        out.found = true;
        out.label = p->written + (p->params_label.empty() ? "" : " " + p->params_label);
        out.params = p->params;
        out.documentation = p->doc;
        out.active_param = arg;
        return out;
    }
    const TclLspApi *a = Api(name);
    if (!a) return out;
    int skip = 1;
    if (std::string(a->kind) == "ensemble" && best->words.size() >= 2) {
        const Span &w = best->words[1];
        const std::string word(doc.text, static_cast<size_t>(w.start), static_cast<size_t>(w.end - w.start));
        const std::string sub = ResolveSub(name, word);
        if (const TclLspApi *s = sub.empty() ? nullptr : Api(name + " " + sub)) {
            a = s;
            skip = 2;
            arg--;
        }
    }
    out.found = true;
    out.label = a->usage;
    out.params = UsageParams(a->usage, skip);
    out.documentation = a->doc;
    out.active_param = std::max(0, arg);
    return out;
}

std::vector<TclLspLocation> TclLspDefinition(const TclLspDocument &doc, int line, int col) {
    std::vector<TclLspLocation> out;
    const int off = doc.Off(line, col);
    const VarTarget vt = VarAt(doc, off);
    if (vt.found) {
        for (const VarSite &s : VarSites(doc, vt.scope, vt.name)) {
            if (s.def) {
                out.push_back({doc.Range(s.start, s.end)});
                break;
            }
        }
        return out;
    }
    if (const ProcInfo *p = ProcTargetAt(doc, off)) out.push_back({doc.Range(p->name_start, p->name_end)});
    return out;
}

std::vector<TclLspReference> TclLspReferences(const TclLspDocument &doc, int line, int col) {
    std::vector<TclLspReference> out;
    const int off = doc.Off(line, col);
    const VarTarget vt = VarAt(doc, off);
    if (vt.found) {
        for (const VarSite &s : VarSites(doc, vt.scope, vt.name)) out.push_back({doc.Range(s.start, s.end), s.def});
        return out;
    }
    const ProcInfo *p = ProcTargetAt(doc, off);
    if (!p) return out;
    out.push_back({doc.Range(p->name_start, p->name_end), true});
    for (const CallInfo &c : doc.calls)
        if (ResolveProc(doc, c.written, c.ns) == p) out.push_back({doc.Range(c.start, c.end), false});
    return out;
}

TclLspRenameTarget TclLspPrepareRename(const TclLspDocument &doc, int line, int col) {
    TclLspRenameTarget r;
    const int off = doc.Off(line, col);
    const VarTarget vt = VarAt(doc, off);
    if (vt.found) {
        r.found = true;
        r.range = doc.Range(vt.site.start, vt.site.end);
        r.placeholder = vt.name;
        return r;
    }
    const ProcInfo *p = ProcTargetAt(doc, off);
    if (!p) return r;
    // The tail of the name under the cursor.
    const CallInfo *c = CallAt(doc, off);
    int s = p->name_start, e = p->name_end;
    if (c && !(off >= p->name_start && off <= p->name_end)) {
        s = c->start;
        e = c->end;
    }
    const std::string written(doc.text, static_cast<size_t>(s), static_cast<size_t>(e - s));
    const size_t tail = written.rfind("::");
    if (tail != std::string::npos) s += static_cast<int>(tail) + 2;
    r.found = true;
    r.range = doc.Range(s, e);
    r.placeholder = CommandTail(p->written);
    return r;
}

std::vector<TclLspTextEdit> TclLspRename(const TclLspDocument &doc, int line, int col, const std::string &new_name,
                                         std::string *error) {
    std::vector<TclLspTextEdit> edits;
    bool ok = !new_name.empty();
    for (char c : new_name)
        if (!tclparse::IsBareword(c)) ok = false;
    if (!ok) {
        *error = "\"" + new_name + "\" is not a plain Tcl name (letters, digits, _)";
        return edits;
    }
    const int off = doc.Off(line, col);
    const VarTarget vt = VarAt(doc, off);
    if (vt.found) {
        for (const VarSite &s : VarSites(doc, vt.scope, vt.name)) edits.push_back({doc.Range(s.start, s.end), new_name});
        return edits;
    }
    const ProcInfo *p = ProcTargetAt(doc, off);
    if (!p) {
        *error = "nothing to rename here";
        return edits;
    }
    auto tail_edit = [&](int s, int e) {
        const std::string written(doc.text, static_cast<size_t>(s), static_cast<size_t>(e - s));
        const size_t tail = written.rfind("::");
        if (tail != std::string::npos) s += static_cast<int>(tail) + 2;
        edits.push_back({doc.Range(s, e), new_name});
    };
    tail_edit(p->name_start, p->name_end);
    for (const CallInfo &c : doc.calls)
        if (ResolveProc(doc, c.written, c.ns) == p) tail_edit(c.start, c.end);
    return edits;
}

std::vector<TclLspSymbol> TclLspSymbols(const TclLspDocument &doc) {
    std::vector<TclLspSymbol> out;
    for (const SymRec &s : doc.syms) {
        TclLspSymbol y;
        y.name = s.name;
        y.detail = s.detail;
        y.kind = s.kind;
        y.range = doc.Range(s.start, s.end);
        y.sel_range = doc.Range(s.sel_start, s.sel_end);
        y.parent = s.parent;
        out.push_back(y);
    }
    return out;
}

std::vector<TclLspFold> TclLspFolds(const TclLspDocument &doc) {
    std::vector<TclLspFold> out;
    std::set<std::pair<int, int>> seen;
    for (const Span &f : doc.folds) {
        int l1 = 0, c1 = 0, l2 = 0, c2 = 0;
        doc.Pos(f.start, &l1, &c1);
        doc.Pos(std::max(f.start, f.end - 1), &l2, &c2);
        const int end = l2 - 1;
        if (end > l1 && seen.insert({l1, end}).second) out.push_back({l1, end, ""});
    }
    // Runs of comment lines.
    int run_start = -1;
    for (size_t i = 0; i <= doc.lines.size(); i++) {
        bool comment = false;
        if (i < doc.lines.size()) {
            const std::string &s = doc.lines[i];
            size_t k = 0;
            while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) k++;
            comment = k < s.size() && s[k] == '#';
        }
        if (comment && run_start < 0) run_start = static_cast<int>(i);
        if (!comment && run_start >= 0) {
            if (static_cast<int>(i) - 1 > run_start) out.push_back({run_start, static_cast<int>(i) - 1, "comment"});
            run_start = -1;
        }
    }
    std::sort(out.begin(), out.end(),
              [](const TclLspFold &a, const TclLspFold &b) { return a.start_line < b.start_line; });
    return out;
}

std::vector<TclLspTextEdit> TclLspFormat(const std::vector<std::string> &lines, int width, std::string *error) {
    std::vector<TclLspTextEdit> edits;
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    tclfmt::Options o;
    if (width > 0) o.width = width;
    const tclfmt::Result r = tclfmt::Format(text, o);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    if (r.text == text) return edits;
    // One edit over the lines that changed.
    std::vector<std::string> b;
    {
        size_t i = 0;
        while (true) {
            const size_t j = r.text.find('\n', i);
            if (j == std::string::npos) {
                b.push_back(r.text.substr(i));
                break;
            }
            b.push_back(r.text.substr(i, j - i));
            i = j + 1;
        }
    }
    size_t pre = 0;
    while (pre < lines.size() && pre < b.size() && lines[pre] == b[pre]) pre++;
    size_t suf = 0;
    while (suf < lines.size() - pre && suf < b.size() - pre && lines[lines.size() - 1 - suf] == b[b.size() - 1 - suf])
        suf++;
    TclLspTextEdit e;
    e.range.line = static_cast<int>(pre);
    e.range.col = 0;
    const size_t a_end = lines.size() - suf;
    const size_t b_end = b.size() - suf;
    if (a_end < lines.size()) {
        e.range.end_line = static_cast<int>(a_end);
        e.range.end_col = 0;
        for (size_t k = pre; k < b_end; k++) e.new_text += b[k] + "\n";
    } else {
        e.range.end_line = static_cast<int>(lines.size()) - 1;
        e.range.end_col = static_cast<int>(lines.back().size());
        for (size_t k = pre; k < b_end; k++) {
            if (k > pre) e.new_text += "\n";
            e.new_text += b[k];
        }
    }
    edits.push_back(e);
    return edits;
}
