// mep's Scheme language server, analysis half: see scheme_lsp.h.
//
// The document is read by scheme_reader.h -- a reader that makes Guile's
// decisions, so its errors are Guile's own (same message, same
// line:column; tools/scheme_read_check.py holds it to that) -- and the
// tree is then walked once with a model of the core special forms (the
// define family, lambda/lambda*/case-lambda, the let family, do, receive,
// guard, case, syntax-rules/syntax-case, quasiquote, ice-9 match, records,
// Guile's module forms and R7RS/R6RS libraries) to build scopes, bindings
// and references. Everything else is a call: its operands are code.
//
// The lints, each calibrated on a corpus of Guile, Guix, Chibi, Chez and
// Gauche sources (`just lsp-lint-corpus scheme`):
//   - the reader's errors (unbalanced parentheses, bad `#` syntax, ...);
//   - duplicate-binding: a name bound twice by one let/letrec/do/lambda
//     list, or defined twice in one body -- Guile rejects both;
//   - unused-variable / unused-definition: a let-family or internal-define
//     binding nothing reads (`_`-prefixed names are exempt); an unused
//     parameter is only a hint;
//   - wrong-arity: a call of a procedure the file defines, or of one the
//     modules it imports export (scheme_lsp_names.h, generated from a
//     running Guile), with an argument count no definition accepts. Only
//     checked when every module the file imports is in that table, so a
//     name another module could be shadowing is never judged.
#include "scheme_lsp.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>

#include "scheme_format.h"
#include "scheme_lsp_names.h"
#include "scheme_reader.h"

namespace {

using NK = SchemeNodeKind;

bool PosLess(int l1, int c1, int l2, int c2) { return l1 < l2 || (l1 == l2 && c1 < c2); }

SchemeLspRange ToLsp(const SchemeRange &r) { return {r.line, r.col, r.end_line, r.end_col}; }

// --- The vocabulary ------------------------------------------------------

struct Vocab {
    std::unordered_map<std::string, std::vector<const SchemeVocabEntry *>> by_name;
    std::set<std::string> modules;
};

const Vocab &TheVocab() {
    static const Vocab v = [] {
        Vocab out;
        for (const SchemeVocabEntry *e = SchemeVocabBegin(); e != SchemeVocabEnd(); ++e) {
            out.by_name[e->name].push_back(e);
            out.modules.insert(e->module);
        }
        return out;
    }();
    return v;
}

// --- The document index ----------------------------------------------------

enum class BK {
    Global,         // a top-level define of a value
    Function,       // a top-level procedure definition
    Macro,          // define-syntax, define-macro, let-syntax
    Record,         // a record type name
    Lexical,        // let, let*, letrec, do, named-let variables, and-let*
    Destructured,   // let-values, receive, define-values formals
    Param,          // a lambda / define parameter
    Internal,       // an internal define
    Pattern,        // a syntax-rules / syntax-case / match pattern variable
    LoopName,       // a named let's name
};

struct Arity {
    bool known = false;
    std::vector<std::pair<int, int>> clauses;  // (min, max); max -1: any number more

    bool Accepts(int n) const {
        for (const auto &c : clauses)
            if (n >= c.first && (c.second < 0 || n <= c.second)) return true;
        return false;
    }
};

// A string literal's value: quotes dropped, escapes decoded.
std::string SchemeStringValue(const std::string &lit) {
    std::string out;
    size_t i = lit.size() >= 2 && lit[0] == '"' ? 1 : 0;
    const size_t end = lit.size() >= 2 && lit.back() == '"' ? lit.size() - 1 : lit.size();
    for (; i < end; i++) {
        if (lit[i] != '\\' || i + 1 >= end) {
            out += lit[i];
            continue;
        }
        const char c = lit[++i];
        if (c == 'n') out += '\n';
        else if (c == 't') out += '\t';
        else if (c == 'r') out += '\r';
        else if (c == '\n') {  // a line continuation: the next line's leading blanks go too
            while (i + 1 < end && (lit[i + 1] == ' ' || lit[i + 1] == '\t')) i++;
        } else out += c;
    }
    return out;
}

struct Binding {
    std::string name;
    BK kind = BK::Lexical;
    int node = -1;  // the naming symbol
    SchemeRange range;
    SchemeRange def_range;  // the whole defining form
    int scope = 0;
    int reads = 0;
    int def_count = 1;
    bool assigned = false;
    bool exported = false;
    Arity arity;
    std::string sig;     // "(f a b . rest)" for a procedure
    std::string origin;  // "let", "parameter of f", ...
    std::string doc;     // a procedure's docstring: (define (f x) "doc" body...)
};

struct Scope {
    int parent = -1;
    SchemeRange range;
    std::vector<int> bindings;
    bool opaque = false;  // code the file does not show runs here (include, the-environment)
};

struct Occ {
    int node = -1;
    SchemeRange range;
    int binding = -1;
    bool def = false;
};

enum class Mode { Code, Template };

// `_`-prefixed names are meant unused; Gauche's C-in-S-expression `x::int` are C variables.
bool IsIgnoredName(const std::string &n) { return n.empty() || n[0] == '_' || n.find("::") != std::string::npos; }

}  // namespace

class SchemeLspDocument {
public:
    std::string path;  // the file's path (a file:// URI decoded), "" when unknown
    std::vector<std::string> lines;
    SchemeReadResult read;
    std::vector<Scope> scopes;
    std::vector<Binding> bindings;
    std::vector<Occ> occs;
    std::map<int, int> occ_of_node;     // symbol node -> occ
    std::map<int, int> def_binding;     // naming symbol node -> binding
    std::vector<SchemeLspDiagnostic> diags;
    // Imports: the vocab modules visible, and whether anything else is.
    bool guile_visible = true;
    bool unknown_imports = false;
    // The file says what it imports (define-module, use-modules, import, a
    // library): only then are calls of imported names judged -- a file
    // that declares nothing may be another Scheme's, or included by one.
    bool declares_imports = false;
    std::vector<std::string> modules;  // in import order
    std::vector<std::string> other_modules;  // imported modules the vocabulary does not know (the user's)
    std::vector<int> export_nodes;     // names a module form exports
};

namespace {

class Analyzer {
public:
    explicit Analyzer(SchemeLspDocument *d) : d_(*d), r_(d->read) {}

    void Run() {
        Scope top;
        top.range.end_line = static_cast<int>(d_.lines.size());
        d_.scopes.push_back(top);
        ScanImports(r_.top);
        WalkBody(r_.top, 0, 0, true);
        for (int n : d_.export_nodes) {
            const SchemeNode &s = N(n);
            const int b = Lookup(0, s.name);
            if (b < 0) continue;
            d_.bindings[static_cast<size_t>(b)].exported = true;
            AddOcc(n, b, false);
        }
        Unused();
    }

private:
    SchemeLspDocument &d_;
    const SchemeReadResult &r_;
    int in_macro_use_ = 0;  // inside the operands of a macro the file defines: no arity checks
    int in_cond_expand_ = 0;  // declaring cond-expand clauses: one name, one binding

    const SchemeNode &N(int i) const { return r_.nodes[static_cast<size_t>(i)]; }
    bool IsSym(int i) const { return i >= 0 && N(i).kind == NK::Symbol; }
    bool IsList(int i) const { return i >= 0 && N(i).kind == NK::List; }
    // #:name, or the old `:name` spelling module forms still accept.
    bool IsKeyword(int i, const char *k) const {
        if (i < 0) return false;
        const SchemeNode &n = N(i);
        if (n.kind == NK::Keyword) return n.name == k;
        return n.kind == NK::Symbol && n.name.size() > 1 && n.name[0] == ':' && n.name.compare(1, std::string::npos, k) == 0;
    }
    int Kid(int n, size_t k) const {
        const SchemeNode &x = N(n);
        return k < x.kids.size() ? x.kids[k] : -1;
    }
    size_t NKids(int n) const { return N(n).kids.size(); }
    std::string HeadName(int n) const {
        if (!IsList(n) || N(n).kids.empty()) return {};
        const int h = N(n).kids[0];
        if (!IsSym(h)) return {};
        // GOOPS imported with `#:prefix goops:` (as Rosetta's generated modules do):
        // (goops:define-class ...) is define-class.
        const std::string &name = N(h).name;
        const size_t colon = name.rfind(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < name.size()) {
            const std::string base = name.substr(colon + 1);
            if (base == "define-class" || base == "define-generic" || base == "define-accessor" ||
                base == "define-method")
                return base;
        }
        return name;
    }

    // The `;` comment lines directly above a form (no blank line between), without
    // their semicolons: the documentation of a define-class or define-generic,
    // which take no docstring.
    std::string LeadingComment(int form) const {
        const int first = N(form).range.line;
        std::vector<std::string> text;
        for (int l = first - 1; l >= 0 && l < static_cast<int>(d_.lines.size()); l--) {
            const std::string &line = d_.lines[static_cast<size_t>(l)];
            size_t i = line.find_first_not_of(" \t");
            if (i == std::string::npos || line[i] != ';') break;
            while (i < line.size() && line[i] == ';') i++;
            if (i < line.size() && line[i] == ' ') i++;
            text.push_back(line.substr(i));
        }
        std::string out;
        for (auto it = text.rbegin(); it != text.rend(); ++it) out += (out.empty() ? "" : "\n") + *it;
        return out;
    }

    // Past the reader's first error the tree is a guess (a recovered list
    // swallows what follows), so the lints stop there.
    bool AfterReadError(int line, int col) const {
        if (r_.errors.empty()) return false;
        const SchemeRange &e = r_.errors[0].range;
        return !PosLess(line, col, e.line, e.col);
    }

    void Diag(const SchemeRange &r, SchemeLspSeverity s, const std::string &code, const std::string &msg,
              bool unnecessary = false) {
        if (AfterReadError(r.line, r.col)) return;
        SchemeLspDiagnostic dg;
        dg.range = ToLsp(r);
        dg.severity = s;
        dg.code = code;
        dg.message = msg;
        dg.unnecessary = unnecessary;
        d_.diags.push_back(dg);
    }

    std::string Text(const SchemeRange &r) const {
        std::string out;
        for (int l = r.line; l <= r.end_line && l < static_cast<int>(d_.lines.size()); l++) {
            const std::string &s = d_.lines[static_cast<size_t>(l)];
            const size_t a = l == r.line ? static_cast<size_t>(std::min<int>(r.col, static_cast<int>(s.size()))) : 0;
            const size_t b = l == r.end_line ? static_cast<size_t>(std::min<int>(r.end_col, static_cast<int>(s.size())))
                                             : s.size();
            if (l != r.line) out += ' ';
            if (b > a) out += s.substr(a, b - a);
        }
        std::string squeezed;
        bool space = false;
        for (char c : out) {
            if (c == ' ' || c == '\t') {
                space = true;
                continue;
            }
            if (space && !squeezed.empty() && squeezed.back() != '(') squeezed += ' ';
            space = false;
            squeezed += c;
        }
        return squeezed;
    }

    // --- Scopes and bindings ---

    int NewScope(int parent, const SchemeRange &r) {
        Scope s;
        s.parent = parent;
        s.range = r;
        d_.scopes.push_back(s);
        return static_cast<int>(d_.scopes.size()) - 1;
    }

    int Lookup(int scope, const std::string &name) const {
        for (int s = scope; s >= 0; s = d_.scopes[static_cast<size_t>(s)].parent) {
            const std::vector<int> &bs = d_.scopes[static_cast<size_t>(s)].bindings;
            for (size_t i = bs.size(); i-- > 0;)
                if (d_.bindings[static_cast<size_t>(bs[i])].name == name) return bs[i];
        }
        return -1;
    }

    int Bind(int node, int scope, BK kind, const SchemeRange &def_range, const std::string &origin) {
        Binding b;
        b.name = N(node).name;
        b.kind = kind;
        b.node = node;
        b.range = N(node).range;
        b.def_range = def_range;
        b.scope = scope;
        b.origin = origin;
        d_.bindings.push_back(b);
        const int id = static_cast<int>(d_.bindings.size()) - 1;
        d_.scopes[static_cast<size_t>(scope)].bindings.push_back(id);
        d_.def_binding[node] = id;
        AddOcc(node, id, true);
        return id;
    }

    void AddOcc(int node, int binding, bool def) {
        if (d_.occ_of_node.count(node)) return;
        Occ o;
        o.node = node;
        o.range = N(node).range;
        o.binding = binding;
        o.def = def;
        d_.occs.push_back(o);
        d_.occ_of_node[node] = static_cast<int>(d_.occs.size()) - 1;
    }

    // A reference to a symbol in code (or, leniently, in a syntax template).
    int Ref(int node, int scope, bool write = false) {
        const SchemeNode &s = N(node);
        const int b = Lookup(scope, s.name);
        if (b < 0) return -1;
        Binding &bd = d_.bindings[static_cast<size_t>(b)];
        if (bd.node == node) return b;
        if (write) {
            bd.assigned = true;
        } else {
            bd.reads++;
        }
        AddOcc(node, b, false);
        return b;
    }

    // --- Formals ---

    struct FormalsInfo {
        Arity arity;
        std::vector<int> names;  // the bound symbols, in order
    };

    // Reads a formals list without binding: (a b . c), args, (a #:optional (b 1) #:key c #:rest r).
    FormalsInfo Formals(int node, size_t from, bool specializers) const {
        FormalsInfo fi;
        fi.arity.known = true;
        if (node < 0) return fi;
        if (IsSym(node)) {
            fi.names.push_back(node);
            fi.arity.clauses.push_back({0, -1});
            return fi;
        }
        if (!IsList(node)) {
            fi.arity.known = false;
            return fi;
        }
        const SchemeNode &l = N(node);
        int required = 0, optional = 0;
        bool rest = false, key = false;
        enum { Req, Opt, Key, Rest } state = Req;
        for (size_t k = from; k < l.kids.size(); k++) {
            const int x = l.kids[k];
            const bool tail = l.dotted && k + 1 == l.kids.size();
            const SchemeNode &n = N(x);
            if (n.kind == NK::Keyword || (n.kind == NK::Symbol && n.name.size() > 1 && n.name[0] == ':')) {
                const std::string kw = n.kind == NK::Keyword ? n.name : n.name.substr(1);
                if (n.kind == NK::Symbol && kw != "optional" && kw != "key" && kw != "rest" && kw != "allow-other-keys") {
                    fi.arity.known = false;
                    continue;
                }
                if (kw == "optional") state = Opt;
                else if (kw == "key") state = Key, key = true;
                else if (kw == "rest") state = Rest, rest = true;
                else if (kw == "allow-other-keys") key = true;
                else fi.arity.known = false;
                continue;
            }
            int name = -1;
            if (n.kind == NK::Symbol) {
                name = x;
            } else if (n.kind == NK::List && !n.kids.empty() && IsSym(n.kids[0]) &&
                       (state == Opt || state == Key || specializers)) {
                name = n.kids[0];
            } else {
                fi.arity.known = false;
                continue;
            }
            fi.names.push_back(name);
            if (tail) {
                rest = true;
                continue;
            }
            switch (state) {
                case Req: required++; break;
                case Opt: optional++; break;
                case Key: break;
                case Rest: break;
            }
        }
        fi.arity.clauses.push_back({required, rest || key ? -1 : required + optional});
        return fi;
    }

    // Binds a formals list in `scope`, walking default values as code.
    void BindFormals(int node, size_t from, int scope, BK kind, const std::string &origin, bool specializers,
                     const char *form) {
        if (node < 0) return;
        if (IsSym(node)) {
            Bind(node, scope, kind, N(node).range, origin);
            return;
        }
        if (!IsList(node)) return;
        const SchemeNode &l = N(node);
        bool defaults = false;
        std::vector<int> names;
        for (size_t k = from; k < l.kids.size(); k++) {
            const int x = l.kids[k];
            const SchemeNode &n = N(x);
            if (n.kind == NK::Keyword || (n.kind == NK::Symbol && (n.name == ":optional" || n.name == ":key" ||
                                                                    n.name == ":rest" || n.name == ":allow-other-keys"))) {
                if (n.name == "optional" || n.name == "key" || n.name == ":optional" || n.name == ":key") defaults = true;
                continue;
            }
            if (n.kind == NK::Symbol) {
                names.push_back(x);
                Bind(x, scope, kind, n.range, origin);
            } else if (n.kind == NK::List && !n.kids.empty() && IsSym(n.kids[0]) && (defaults || specializers)) {
                for (size_t j = 1; j < n.kids.size(); j++) Walk(n.kids[j], scope, Mode::Code);
                names.push_back(n.kids[0]);
                Bind(n.kids[0], scope, kind, n.range, origin);
            }
        }
        Duplicates(names, form, "duplicate identifier in argument list");
    }

    void Duplicates(const std::vector<int> &names, const char *form, const char *what) {
        std::set<std::string> seen;
        for (int n : names) {
            const std::string &nm = N(n).name;
            if (!seen.insert(nm).second)
                Diag(N(n).range, SchemeLspSeverity::Error, "duplicate-binding",
                     std::string(form) + ": " + what + ": " + nm);
        }
    }

    // --- Imports ---

    std::string ModuleKey(int spec) const {
        // (srfi srfi-1), (rnrs base (6)), R7RS (srfi 1).
        if (!IsList(spec)) return {};
        std::vector<std::string> parts;
        for (int k : N(spec).kids) {
            const SchemeNode &n = N(k);
            if (n.kind == NK::Symbol) parts.push_back(n.name);
            else if (n.kind == NK::Number && parts.size() == 1 && parts[0] == "srfi") parts.push_back("srfi-" + n.text);
            else if (n.kind == NK::List || n.kind == NK::Number) continue;
            else return {};
        }
        std::string key;
        for (const std::string &p : parts) key += (key.empty() ? "" : " ") + p;
        return key;
    }

    void ImportModule(const std::string &key) {
        if (key.empty()) {
            d_.unknown_imports = true;
            return;
        }
        if (!TheVocab().modules.count(key)) {
            d_.unknown_imports = true;
            if (std::find(d_.other_modules.begin(), d_.other_modules.end(), key) == d_.other_modules.end())
                d_.other_modules.push_back(key);
            return;
        }
        if (std::find(d_.modules.begin(), d_.modules.end(), key) == d_.modules.end()) d_.modules.push_back(key);
    }

    // A Guile use-modules spec: (a b) or ((a b) #:select (...) #:prefix p: ...).
    void UseModuleSpec(int spec) {
        if (!IsList(spec) || N(spec).kids.empty()) return;
        const int first = N(spec).kids[0];
        if (IsList(first)) {
            bool renamed = false;
            for (size_t k = 1; k < NKids(spec); k++)
                if (IsKeyword(Kid(spec, k), "prefix") || IsKeyword(Kid(spec, k), "renamer")) renamed = true;
            if (!renamed) ImportModule(ModuleKey(first));
            return;
        }
        ImportModule(ModuleKey(spec));
    }

    // An R7RS / R6RS import set: (scheme base), (only SET id...), (prefix SET p), ...
    void ImportSet(int set) {
        if (!IsList(set) || N(set).kids.empty()) {
            d_.unknown_imports = true;  // Chez's (import module-name)
            return;
        }
        const std::string h = HeadName(set);
        if ((h == "only" || h == "except") && NKids(set) >= 2 && IsList(Kid(set, 1))) {
            ImportSet(Kid(set, 1));
            return;
        }
        if ((h == "prefix" || h == "rename") && NKids(set) >= 2 && IsList(Kid(set, 1))) {
            if (h == "rename") d_.unknown_imports = true;
            return;
        }
        if (h == "for" && NKids(set) >= 2) {  // R6RS (for SET phase...)
            ImportSet(Kid(set, 1));
            return;
        }
        ImportModule(ModuleKey(set));
    }

    void ScanImports(const std::vector<int> &forms) {
        for (int f : forms) {
            const std::string h = HeadName(f);
            if (h.empty()) continue;
            if (h == "define-module" || h == "use-modules" || h == "import" || h == "define-library" || h == "library")
                d_.declares_imports = true;
            if (h == "define-module") {
                if (!IsList(Kid(f, 1))) d_.unknown_imports = true;  // Gauche's (define-module name ...)
                for (size_t k = 2; k + 1 < NKids(f); k++) {
                    const int kw = Kid(f, k);
                    if (IsKeyword(kw, "use-module") || IsKeyword(kw, "autoload")) {
                        if (IsKeyword(kw, "use-module")) UseModuleSpec(Kid(f, k + 1));
                        else ImportModule(ModuleKey(Kid(f, k + 1)));
                    } else if (IsKeyword(kw, "export") || IsKeyword(kw, "export-syntax") || IsKeyword(kw, "replace") ||
                               IsKeyword(kw, "re-export") || IsKeyword(kw, "export!")) {
                        ExportList(Kid(f, k + 1), 0);
                    } else if (IsKeyword(kw, "pure")) {
                        d_.guile_visible = false;
                    }
                }
                for (size_t k = 2; k < NKids(f); k++)
                    if (IsKeyword(Kid(f, k), "pure")) d_.guile_visible = false;
            } else if (h == "use-modules") {
                for (size_t k = 1; k < NKids(f); k++) UseModuleSpec(Kid(f, k));
            } else if (h == "import") {
                for (size_t k = 1; k < NKids(f); k++) ImportSet(Kid(f, k));
            } else if (h == "define-library" || h == "library") {
                d_.guile_visible = false;
                LibraryDecls(f, 2, true);
            } else if (h == "begin" || h == "eval-when" || h == "cond-expand") {
                std::vector<int> inner;
                for (size_t k = 1; k < NKids(f); k++) {
                    if (h == "cond-expand") {
                        if (IsList(Kid(f, k)))
                            for (size_t j = 1; j < NKids(Kid(f, k)); j++) inner.push_back(Kid(Kid(f, k), j));
                    } else {
                        inner.push_back(Kid(f, k));
                    }
                }
                ScanImports(inner);
            } else if (h == "select-module" || h == "use" || h == "require" || h == "require-extension") {
                d_.unknown_imports = true;
            }
        }
    }

    void LibraryDecls(int lib, size_t from, bool imports) {
        for (size_t k = from; k < NKids(lib); k++) {
            const int decl = Kid(lib, k);
            const std::string h = HeadName(decl);
            if (h == "import" && imports) {
                for (size_t j = 1; j < NKids(decl); j++) ImportSet(Kid(decl, j));
            } else if (h == "export" && imports) {
                ExportList(decl, 1);
            } else if (h == "cond-expand") {
                for (size_t j = 1; j < NKids(decl); j++)
                    if (IsList(Kid(decl, j))) LibraryDecls(Kid(decl, j), 1, imports);
            } else if (h == "include-library-declarations") {
                d_.unknown_imports = true;
            }
        }
    }

    void ExportList(int list, size_t from) {
        if (!IsList(list)) return;
        for (size_t k = from; k < NKids(list); k++) {
            const int x = Kid(list, k);
            if (IsSym(x)) {
                d_.export_nodes.push_back(x);
            } else if (IsList(x) && NKids(x) >= 2) {
                // (rename a b) in R7RS/R6RS, (a . b) in Guile.
                const int first = Kid(x, 0);
                if (IsSym(first) && N(first).name == "rename" && IsSym(Kid(x, 1))) d_.export_nodes.push_back(Kid(x, 1));
                else if (IsSym(first)) d_.export_nodes.push_back(first);
            }
        }
    }

    // --- Definitions ---

    struct DefTarget {
        int name = -1;          // the defined symbol
        int formals = -1;       // the innermost head list (f a b), -1 for (define f ...)
        std::vector<int> outer; // curried outer head lists, innermost first excluded
    };

    DefTarget Target(int form) const {
        DefTarget t;
        int x = Kid(form, 1);
        if (IsSym(x)) {
            t.name = x;
            return t;
        }
        std::vector<int> chain;
        while (IsList(x) && !N(x).kids.empty()) {
            chain.push_back(x);
            x = N(x).kids[0];
        }
        if (!IsSym(x)) return t;
        t.name = x;
        t.formals = chain.back();
        chain.pop_back();
        t.outer = chain;
        return t;
    }

    static bool IsDefine(const std::string &h) {
        return h == "define" || h == "define-public" || h == "define*" || h == "define*-public" ||
               h == "define-inlinable" || h == "define-integrable" || h == "define-once" || h == "define-constant";
    }
    static bool IsMacroDefine(const std::string &h) {
        return h == "define-syntax" || h == "define-syntax-rule" || h == "define-macro" || h == "defmacro" ||
               h == "define-syntax-parameter" || h == "define-macro*";
    }
    static bool IsLambda(const std::string &h) { return h == "lambda" || h == "lambda*" || h == "\xce\xbb"; }

    // The arity and argument list of the value a define binds, when it is a lambda.
    void ValueShape(int value, Binding *b) const {
        const std::string h = HeadName(value);
        if (IsLambda(h) && NKids(value) >= 2) {
            const FormalsInfo fi = Formals(Kid(value, 1), 0, false);
            b->arity = fi.arity;
            b->kind = b->kind == BK::Global ? BK::Function : b->kind;
            const std::string ft = IsSym(Kid(value, 1)) ? ". " + N(Kid(value, 1)).name : Text(N(Kid(value, 1)).range);
            if (IsSym(Kid(value, 1))) b->sig = "(" + b->name + " " + ft + ")";
            else b->sig = "(" + b->name + (ft.size() > 2 ? " " + ft.substr(1) : ")");
        } else if ((h == "case-lambda" || h == "case-lambda*") && NKids(value) >= 2) {
            b->arity.known = true;
            b->kind = b->kind == BK::Global ? BK::Function : b->kind;
            for (size_t k = 1; k < NKids(value); k++) {
                const int clause = Kid(value, k);
                if (!IsList(clause) || NKids(clause) == 0) {
                    b->arity.known = false;
                    continue;
                }
                const FormalsInfo fi = Formals(Kid(clause, 0), 0, false);
                if (!fi.arity.known) b->arity.known = false;
                for (const auto &c : fi.arity.clauses) b->arity.clauses.push_back(c);
            }
        }
    }

    void DeclareBinding(int name, int form, int scope, BK kind, const std::string &origin,
                        std::vector<int> *body_names) {
        if (!IsSym(name)) return;
        if (scope == 0 || in_cond_expand_ > 0) {
            int prev = -1;
            for (int b : d_.scopes[static_cast<size_t>(scope)].bindings)
                if (d_.bindings[static_cast<size_t>(b)].name == N(name).name) prev = b;
            if (prev >= 0) {
                Binding &b = d_.bindings[static_cast<size_t>(prev)];
                b.def_count++;
                d_.def_binding[name] = prev;
                AddOcc(name, prev, true);
                return;
            }
        }
        const int id = Bind(name, scope, kind, N(form).range, origin);
        if (body_names) body_names->push_back(name);
        (void)id;
    }

    // Creates the bindings a body form introduces, before the body is walked
    // (a body's definitions are visible throughout it).
    void Declare(int form, int scope, bool top, std::vector<int> *body_names) {
        const std::string h = HeadName(form);
        if (h.empty()) return;
        if (!top && Lookup(scope, h) >= 0 && d_.bindings[static_cast<size_t>(Lookup(scope, h))].scope != 0) return;
        const BK var = top ? BK::Global : BK::Internal;
        if (h == "begin") {
            for (size_t k = 1; k < NKids(form); k++) Declare(Kid(form, k), scope, top, body_names);
        } else if (h == "eval-when") {
            for (size_t k = 2; k < NKids(form); k++) Declare(Kid(form, k), scope, top, body_names);
        } else if (h == "cond-expand") {
            // Only one clause is ever expanded: they may define the same names.
            in_cond_expand_++;
            for (size_t k = 1; k < NKids(form); k++)
                if (IsList(Kid(form, k)))
                    for (size_t j = 1; j < NKids(Kid(form, k)); j++) Declare(Kid(Kid(form, k), j), scope, top, nullptr);
            in_cond_expand_--;
        } else if (h == "define-library" || h == "library") {
            for (size_t k = 2; k < NKids(form); k++) {
                const int decl = Kid(form, k);
                if (h == "library") {
                    const std::string dh = HeadName(decl);
                    if (dh != "export" && dh != "import") Declare(decl, scope, top, body_names);
                } else if (HeadName(decl) == "begin") {
                    Declare(decl, scope, top, body_names);
                }
            }
        } else if (IsDefine(h)) {
            const DefTarget t = Target(form);
            if (t.name < 0) return;
            const bool proc = t.formals >= 0;
            DeclareBinding(t.name, form, scope, proc ? (top ? BK::Function : BK::Internal) : var,
                           top ? "" : "internal definition", body_names);
            auto it = d_.def_binding.find(t.name);
            if (it == d_.def_binding.end()) return;
            Binding &b = d_.bindings[static_cast<size_t>(it->second)];
            if (b.node != t.name) return;
            if (proc) {
                const FormalsInfo fi = Formals(t.formals, 1, false);
                b.arity = fi.arity;
                b.sig = Text(N(t.formals).range);
                // A docstring: a string first in a body that has more after it.
                if (NKids(form) >= 4 && N(Kid(form, 2)).kind == NK::String) b.doc = SchemeStringValue(N(Kid(form, 2)).text);
            } else {
                ValueShape(Kid(form, 2), &b);
                const int value = Kid(form, 2);
                if (IsLambda(HeadName(value)) && NKids(value) >= 4 && N(Kid(value, 2)).kind == NK::String)
                    b.doc = SchemeStringValue(N(Kid(value, 2)).text);
            }
        } else if (IsMacroDefine(h)) {
            int name = Kid(form, 1);
            if (IsList(name) && NKids(name) > 0) name = Kid(name, 0);
            DeclareBinding(name, form, scope, BK::Macro, "macro", nullptr);
            // (define-syntax-rule (name args) "doc" template)
            auto it = d_.def_binding.find(name);
            if (h == "define-syntax-rule" && it != d_.def_binding.end() && NKids(form) >= 4 &&
                N(Kid(form, 2)).kind == NK::String) {
                Binding &b = d_.bindings[static_cast<size_t>(it->second)];
                if (b.node == name) {
                    b.doc = SchemeStringValue(N(Kid(form, 2)).text);
                    if (IsList(Kid(form, 1))) b.sig = Text(N(Kid(form, 1)).range);
                }
            }
        } else if (h == "define-values") {
            const FormalsInfo fi = Formals(Kid(form, 1), 0, false);
            for (int n : fi.names) DeclareBinding(n, form, scope, top ? BK::Global : BK::Destructured, "define-values", nullptr);
        } else if (h == "define-record-type*") {
            DeclareGuixRecord(form, scope);
        } else if (h == "define-record-type") {
            DeclareRecord(form, scope);
        } else if (h == "define-condition-type") {
            // (define-condition-type name super pred (field accessor)...)
            DeclareBinding(Kid(form, 1), form, scope, BK::Record, "condition type", nullptr);
            DeclareFn(Kid(form, 3), form, scope, 1, 1);
            for (size_t k = 4; k < NKids(form); k++)
                if (IsList(Kid(form, k))) DeclareFn(Kid(Kid(form, k), 1), form, scope, 1, 1);
        } else if (h == "define-class") {
            DeclareBinding(Kid(form, 1), form, scope, BK::Record, "class", nullptr);
            DocumentFromComments(Kid(form, 1), form);
            for (size_t k = 3; k < NKids(form); k++) {
                const int slot = Kid(form, k);
                if (!IsList(slot)) continue;
                for (size_t j = 1; j + 1 < NKids(slot); j++)
                    if (IsKeyword(Kid(slot, j), "accessor") || IsKeyword(Kid(slot, j), "getter") ||
                        IsKeyword(Kid(slot, j), "setter"))
                        DeclareBinding(Kid(slot, j + 1), form, scope, BK::Global, "slot accessor", nullptr);
            }
        } else if (h == "define-generic" || h == "define-accessor") {
            DeclareBinding(Kid(form, 1), form, scope, BK::Global, "generic", nullptr);
            DocumentFromComments(Kid(form, 1), form);
        } else if (h == "define-method") {
            int head = Kid(form, 1);
            if (IsList(head) && NKids(head) > 0) {
                int name = Kid(head, 0);
                if (IsList(name) && HeadName(name) == "setter") name = Kid(name, 1);
                if (IsSym(name)) {
                    const int prev = Lookup(scope, N(name).name);
                    if (prev >= 0) AddOcc(name, prev, true);
                    else DeclareBinding(name, form, scope, BK::Global, "generic", nullptr);
                    if (prev >= 0) d_.bindings[static_cast<size_t>(prev)].arity.known = false;
                }
            }
        }
    }

    // The comments above a define-class / define-generic document the name it defines
    // (unless an earlier definition of the name already has documentation).
    void DocumentFromComments(int name, int form) {
        if (!IsSym(name)) return;
        auto it = d_.def_binding.find(name);
        if (it == d_.def_binding.end()) return;
        Binding &b = d_.bindings[static_cast<size_t>(it->second)];
        if (b.doc.empty()) b.doc = LeadingComment(form);
    }

    void DeclareFn(int name, int form, int scope, int min, int max, const std::string &origin = "record") {
        if (!IsSym(name)) return;
        const bool fresh = Lookup(scope, N(name).name) < 0 || scope != 0;
        DeclareBinding(name, form, scope, BK::Function, origin, nullptr);
        if (!fresh) return;
        Binding &b = d_.bindings.back();
        if (b.node != name) return;
        if (min >= 0) {
            b.arity.known = true;
            b.arity.clauses.push_back({min, max});
        }
    }

    // Guix's (define-record-type* <type> syntax-ctor ctor pred [#:option x]... (field accessor option...)...)
    void DeclareGuixRecord(int form, int scope) {
        DeclareBinding(Kid(form, 1), form, scope, BK::Record, "record type", nullptr);
        DeclareBinding(Kid(form, 2), form, scope, BK::Macro, "record syntax", nullptr);
        DeclareFn(Kid(form, 3), form, scope, -1, -1, "record constructor");
        DeclareFn(Kid(form, 4), form, scope, 1, 1, "record predicate");
        for (size_t k = 5; k < NKids(form); k++) {
            const int x = Kid(form, k);
            if (N(x).kind == NK::Keyword) {
                if (IsSym(Kid(form, k + 1))) DeclareBinding(Kid(form, k + 1), form, scope, BK::Macro, "record option", nullptr);
                k++;
                continue;
            }
            if (IsList(x)) DeclareFn(Kid(x, 1), form, scope, 1, 1, "record accessor");
        }
    }

    void DeclareRecord(int form, int scope) {
        const int spec = Kid(form, 1);
        const int second = Kid(form, 2);
        static const std::set<std::string> kR6Clauses = {"fields", "parent", "protocol", "sealed", "opaque",
                                                         "nongenerative", "parent-rtd"};
        const bool r6 = second < 0 || (IsList(second) && kR6Clauses.count(HeadName(second)));
        if (r6) {
            // (define-record-type name clause...) or ((name ctor pred) clause...).
            std::string base;
            if (IsSym(spec)) {
                base = N(spec).name;
                DeclareBinding(spec, form, scope, BK::Record, "record type", nullptr);
            } else if (IsList(spec) && NKids(spec) >= 1) {
                if (IsSym(Kid(spec, 0))) base = N(Kid(spec, 0)).name;
                DeclareBinding(Kid(spec, 0), form, scope, BK::Record, "record type", nullptr);
                DeclareFn(Kid(spec, 1), form, scope, -1, -1);
                DeclareFn(Kid(spec, 2), form, scope, 1, 1);
            }
            for (size_t k = 2; k < NKids(form); k++) {
                const int clause = Kid(form, k);
                if (HeadName(clause) != "fields") continue;
                for (size_t j = 1; j < NKids(clause); j++) {
                    const int f = Kid(clause, j);
                    if (!IsList(f)) continue;
                    const std::string mut = HeadName(f);
                    if (mut == "immutable" || mut == "mutable") {
                        DeclareFn(Kid(f, 2), form, scope, 1, 1);
                        if (mut == "mutable") DeclareFn(Kid(f, 3), form, scope, 2, 2);
                    }
                }
            }
            (void)base;
            return;
        }
        // SRFI 9 / R7RS: (define-record-type <name> (ctor field...) pred (field accessor [modifier])...)
        int type = spec;
        if (IsList(spec) && NKids(spec) > 0) type = Kid(spec, 0);  // Guile: (<name> #:opaque? ...)
        DeclareBinding(type, form, scope, BK::Record, "record type", nullptr);
        if (IsList(second) && NKids(second) > 0) {
            const int n = static_cast<int>(NKids(second)) - 1;
            DeclareFn(Kid(second, 0), form, scope, n, n, "record constructor");
        } else if (IsSym(second)) {
            DeclareFn(second, form, scope, -1, -1, "record constructor");
        }
        DeclareFn(Kid(form, 3), form, scope, 1, 1, "record predicate");
        for (size_t k = 4; k < NKids(form); k++) {
            const int f = Kid(form, k);
            if (!IsList(f)) continue;
            DeclareFn(Kid(f, 1), form, scope, 1, 1, "record accessor");
            DeclareFn(Kid(f, 2), form, scope, 2, 2, "record modifier");
        }
    }

    // --- Walking ---

    void WalkBody(const std::vector<int> &forms, size_t from, int scope, bool top) {
        std::vector<int> names;
        for (size_t k = from; k < forms.size(); k++) Declare(forms[k], scope, top, top ? nullptr : &names);
        if (!top) Duplicates(names, "body", "duplicate definition");
        for (size_t k = from; k < forms.size(); k++) Walk(forms[k], scope, Mode::Code);
    }

    void WalkKids(int n, size_t from, int scope, Mode m) {
        for (size_t k = from; k < NKids(n); k++) Walk(Kid(n, k), scope, m);
    }

    void Walk(int n, int scope, Mode m) {
        if (n < 0) return;
        const SchemeNode &x = N(n);
        switch (x.kind) {
            case NK::Symbol:
                if (x.name != "..." && x.name != "_") Ref(n, scope);
                return;
            case NK::List:
                if (m == Mode::Template) WalkKids(n, 0, scope, m);
                else WalkForm(n, scope);
                return;
            case NK::Vector:
                if (m == Mode::Template) WalkKids(n, 0, scope, m);
                return;
            case NK::Prefix:
                WalkPrefix(n, scope, m);
                return;
            default:
                return;
        }
    }

    void WalkPrefix(int n, int scope, Mode m) {
        const SchemeNode &x = N(n);
        const int operand = x.kids.empty() ? -1 : x.kids[0];
        const std::string &p = x.text;
        if (m == Mode::Template) {
            if (p == "#," || p == "#,@") Walk(operand, scope, Mode::Code);
            else Walk(operand, scope, Mode::Template);
            return;
        }
        if (p == "'") return;
        if (p == "`") Quasi(operand, scope, 1, false);
        else if (p == "#~") Quasi(operand, scope, 1, true);
        else if (p == "#'" || p == "#`") Walk(operand, scope, Mode::Template);
        else Walk(operand, scope, Mode::Code);  // , ,@ #$ ... outside a template
    }

    // A quasiquote (or Guix gexp) template: only the unquoted parts are code.
    void Quasi(int n, int scope, int depth, bool gexp) {
        if (n < 0) return;
        const SchemeNode &x = N(n);
        if (x.kind == NK::Prefix) {
            const int operand = x.kids.empty() ? -1 : x.kids[0];
            const std::string &p = x.text;
            const bool up = gexp ? p == "#~" : p == "`";
            const bool down = gexp ? (p == "#$" || p == "#$@" || p == "#+" || p == "#+@") : (p == "," || p == ",@");
            if (down && depth == 1) Walk(operand, scope, Mode::Code);
            else Quasi(operand, scope, depth + (up ? 1 : down ? -1 : 0), gexp);
            return;
        }
        if (x.kind == NK::List || x.kind == NK::Vector) {
            const std::string h = x.kind == NK::List ? HeadName(n) : std::string();
            if (x.kind == NK::List && x.kids.size() == 2) {
                const bool down = gexp ? (h == "ungexp" || h == "ungexp-splicing" || h == "ungexp-native" ||
                                          h == "ungexp-native-splicing")
                                       : (h == "unquote" || h == "unquote-splicing");
                const bool up = gexp ? h == "gexp" : h == "quasiquote";
                if (down && depth == 1) {
                    Walk(x.kids[1], scope, Mode::Code);
                    return;
                }
                if (up || down) {
                    Quasi(x.kids[1], scope, depth + (up ? 1 : -1), gexp);
                    return;
                }
            }
            for (int k : x.kids) Quasi(k, scope, depth, gexp);
        }
    }

    // A pattern variable binder for syntax-rules / syntax-case / with-syntax.
    void SyntaxPattern(int n, int scope, const std::set<std::string> &literals, const std::string &ellipsis,
                       std::vector<int> *bound) {
        if (n < 0) return;
        const SchemeNode &x = N(n);
        if (x.kind == NK::Symbol) {
            if (x.name == "_" || x.name == ellipsis || literals.count(x.name)) return;
            for (int b : *bound)
                if (N(b).name == x.name) return;
            bound->push_back(n);
            Bind(n, scope, BK::Pattern, x.range, "pattern variable");
        } else if (x.kind == NK::List || x.kind == NK::Vector) {
            for (int k : x.kids) SyntaxPattern(k, scope, literals, ellipsis, bound);
        } else if (x.kind == NK::Prefix && !x.kids.empty()) {
            SyntaxPattern(x.kids[0], scope, literals, ellipsis, bound);
        }
    }

    std::set<std::string> Literals(int list) const {
        std::set<std::string> out;
        if (IsList(list))
            for (int k : N(list).kids)
                if (IsSym(k)) out.insert(N(k).name);
        return out;
    }

    void SyntaxRules(int form, int scope) {
        size_t at = 1;
        std::string ellipsis = "...";
        if (IsSym(Kid(form, 1))) {
            ellipsis = N(Kid(form, 1)).name;
            at = 2;
        }
        const std::set<std::string> lits = Literals(Kid(form, at));
        for (size_t k = at + 1; k < NKids(form); k++) {
            const int clause = Kid(form, k);
            if (!IsList(clause) || NKids(clause) < 1) continue;
            const int s = NewScope(scope, N(clause).range);
            std::vector<int> bound;
            const int pat = Kid(clause, 0);
            if (IsList(pat)) {
                for (size_t j = 1; j < NKids(pat); j++) SyntaxPattern(Kid(pat, j), s, lits, ellipsis, &bound);
            }
            WalkKids(clause, 1, s, Mode::Template);
        }
    }

    void SyntaxCase(int form, int scope) {
        Walk(Kid(form, 1), scope, Mode::Code);
        size_t at = 2;
        std::string ellipsis = "...";
        if (IsSym(Kid(form, 2)) && NKids(form) > 3 && IsList(Kid(form, 3)) && !IsList(Kid(Kid(form, 3), 0))) {
            ellipsis = N(Kid(form, 2)).name;
            at = 3;
        }
        const std::set<std::string> lits = Literals(Kid(form, at));
        for (size_t k = at + 1; k < NKids(form); k++) {
            const int clause = Kid(form, k);
            if (!IsList(clause) || NKids(clause) < 1) continue;
            const int s = NewScope(scope, N(clause).range);
            std::vector<int> bound;
            SyntaxPattern(Kid(clause, 0), s, lits, ellipsis, &bound);
            WalkKids(clause, 1, s, Mode::Code);
        }
    }

    // ice-9 match patterns.
    void MatchPattern(int n, int scope, std::vector<int> *bound) {
        if (n < 0) return;
        const SchemeNode &x = N(n);
        if (x.kind == NK::Symbol) {
            const std::string &nm = x.name;
            if (nm == "_" || nm == "..." || nm == "___" || nm.rfind("..", 0) == 0 || nm.rfind("__", 0) == 0) return;
            for (int b : *bound)
                if (N(b).name == nm) {
                    Ref(n, scope);
                    return;
                }
            bound->push_back(n);
            Bind(n, scope, BK::Pattern, x.range, "pattern variable");
            return;
        }
        if (x.kind == NK::Prefix) {
            if (x.text == "`" && !x.kids.empty()) MatchQuasi(x.kids[0], scope, bound);
            return;  // 'datum
        }
        if (x.kind == NK::Vector) {
            for (int k : x.kids) MatchPattern(k, scope, bound);
            return;
        }
        if (x.kind != NK::List || x.kids.empty()) return;
        const std::string h = HeadName(n);
        if (h == "quote") return;
        if (h == "quasiquote") {
            MatchQuasi(Kid(n, 1), scope, bound);
        } else if (h == "?") {
            Walk(Kid(n, 1), scope, Mode::Code);
            for (size_t k = 2; k < x.kids.size(); k++) MatchPattern(x.kids[k], scope, bound);
        } else if (h == "=") {
            Walk(Kid(n, 1), scope, Mode::Code);
            for (size_t k = 2; k < x.kids.size(); k++) MatchPattern(x.kids[k], scope, bound);
        } else if (h == "$" || h == "struct" || h == "object") {
            if (IsSym(Kid(n, 1))) Ref(Kid(n, 1), scope);
            for (size_t k = 2; k < x.kids.size(); k++) MatchPattern(x.kids[k], scope, bound);
        } else if (h == "and" || h == "or" || h == "not") {
            for (size_t k = 1; k < x.kids.size(); k++) MatchPattern(x.kids[k], scope, bound);
        } else if (h == "set!" || h == "get!") {
            MatchPattern(Kid(n, 1), scope, bound);
        } else {
            for (int k : x.kids) MatchPattern(k, scope, bound);
        }
    }

    void MatchQuasi(int n, int scope, std::vector<int> *bound) {
        if (n < 0) return;
        const SchemeNode &x = N(n);
        if (x.kind == NK::Prefix) {
            if ((x.text == "," || x.text == ",@") && !x.kids.empty()) MatchPattern(x.kids[0], scope, bound);
            return;
        }
        if (x.kind == NK::List || x.kind == NK::Vector) {
            const std::string h = HeadName(n);
            if ((h == "unquote" || h == "unquote-splicing") && x.kids.size() == 2) {
                MatchPattern(x.kids[1], scope, bound);
                return;
            }
            for (int k : x.kids) MatchQuasi(k, scope, bound);
        }
    }

    void MatchClause(int clause, int scope) {
        if (!IsList(clause) || NKids(clause) == 0) return;
        const int s = NewScope(scope, N(clause).range);
        std::vector<int> bound;
        MatchPattern(Kid(clause, 0), s, &bound);
        size_t from = 1;
        const int esc = Kid(clause, 1);
        if (IsList(esc) && HeadName(esc) == "=>" && IsSym(Kid(esc, 1))) {
            Bind(Kid(esc, 1), s, BK::Pattern, N(esc).range, "match failure continuation");
            from = 2;
        }
        WalkBody(N(clause).kids, from, s, false);
    }

    void Lambda(int form, size_t formals_at, int scope, const std::string &origin, const char *what) {
        const int s = NewScope(scope, N(form).range);
        BindFormals(Kid(form, formals_at), 0, s, BK::Param, origin, false, what);
        WalkBody(N(form).kids, formals_at + 1, s, false);
    }

    // let / letrec / letrec* / let* / named let / do.
    void Let(int form, int scope, const std::string &h) {
        size_t at = 1;
        int loop_scope = scope;
        if (h == "let" && IsSym(Kid(form, 1))) {
            loop_scope = NewScope(scope, N(form).range);
            Bind(Kid(form, 1), loop_scope, BK::LoopName, N(form).range, "named let");
            Binding &lb = d_.bindings.back();
            lb.arity.known = true;
            const int n = IsList(Kid(form, 2)) ? static_cast<int>(NKids(Kid(form, 2))) : 0;
            lb.arity.clauses.push_back({n, n});
            std::string sig = "(" + lb.name;
            if (IsList(Kid(form, 2)))
                for (int b : N(Kid(form, 2)).kids)
                    if (IsList(b) && IsSym(Kid(b, 0))) sig += " " + N(Kid(b, 0)).name;
            lb.sig = sig + ")";
            at = 2;
        }
        const int bindings = Kid(form, at);
        const bool rec = h == "letrec" || h == "letrec*";
        const bool seq = h == "let*" || h == "and-let*";
        const int s = NewScope(loop_scope, N(form).range);
        const BK kind = loop_scope != scope ? BK::Param : BK::Lexical;
        const std::string origin = loop_scope != scope ? "named let " + N(Kid(form, 1)).name : h;
        std::vector<int> names;
        if (IsList(bindings)) {
            if (rec)
                for (int b : N(bindings).kids)
                    if (IsList(b) && IsSym(Kid(b, 0))) {
                        names.push_back(Kid(b, 0));
                        Bind(Kid(b, 0), s, kind, N(b).range, h);
                    }
            for (int b : N(bindings).kids) {
                if (IsSym(b) && h == "and-let*") {
                    Ref(b, s);
                    continue;
                }
                if (!IsList(b)) continue;
                const int name = Kid(b, 0);
                if (!IsSym(name)) {
                    Walk(name, seq ? s : scope, Mode::Code);  // and-let*'s (expr) clause
                    continue;
                }
                if (rec) {
                    WalkKids(b, 1, s, Mode::Code);
                    continue;
                }
                WalkKids(b, 1, seq ? s : loop_scope, Mode::Code);
                names.push_back(name);
                const int id = Bind(name, s, kind, N(b).range, origin);
                Binding &nb = d_.bindings[static_cast<size_t>(id)];
                if (!rec) ValueShape(Kid(b, 1), &nb);
                if (h == "and-let*") nb.exported = true;  // its value is a test, whether read or not
            }
        }
        // `(let ((x e) ...) body ...)` is a template (a nanopass pattern, a
        // macro's output), not a let.
        bool templ = false;
        for (size_t k = at; k < NKids(form); k++)
            if (IsSym(Kid(form, k)) && N(Kid(form, k)).name == "...") templ = true;
        if (IsList(bindings))
            for (int b : N(bindings).kids)
                if (IsSym(b) && N(b).name == "...") templ = true;
        if (templ) {
            for (int n : names) d_.bindings[static_cast<size_t>(d_.def_binding[n])].exported = true;
        } else if (!seq) {
            Duplicates(names, h.c_str(), "duplicate bound variable");
        }
        WalkBody(N(form).kids, at + 1, s, false);
    }

    void Do(int form, int scope) {
        const int specs = Kid(form, 1);
        const int s = NewScope(scope, N(form).range);
        std::vector<int> names;
        if (IsList(specs)) {
            for (int b : N(specs).kids)
                if (IsList(b)) Walk(Kid(b, 1), scope, Mode::Code);
            for (int b : N(specs).kids)
                if (IsList(b) && IsSym(Kid(b, 0))) {
                    names.push_back(Kid(b, 0));
                    Bind(Kid(b, 0), s, BK::Lexical, N(b).range, "do");
                }
            for (int b : N(specs).kids)
                if (IsList(b)) WalkKids(b, 2, s, Mode::Code);
        }
        Duplicates(names, "do", "duplicate identifier in argument list");
        if (IsList(Kid(form, 2))) WalkKids(Kid(form, 2), 0, s, Mode::Code);
        WalkKids(form, 3, s, Mode::Code);
    }

    void LetValues(int form, int scope, bool seq) {
        const int s = NewScope(scope, N(form).range);
        if (IsList(Kid(form, 1))) {
            for (int b : N(Kid(form, 1)).kids) {
                if (!IsList(b)) continue;
                WalkKids(b, 1, seq ? s : scope, Mode::Code);
                BindFormals(Kid(b, 0), 0, s, BK::Destructured, seq ? "let*-values" : "let-values", false,
                            seq ? "let*-values" : "let-values");
            }
        }
        WalkBody(N(form).kids, 2, s, false);
    }

    void Call(int form, int scope) {
        const SchemeNode &f = N(form);
        const int head = f.kids[0];
        const int b = Ref(head, scope);
        const int argc = static_cast<int>(f.kids.size()) - 1;
        if (b < 0) {
            const std::string &hn = N(head).name;
            if (hn == "include" || hn == "include-ci" || hn == "include-from-path" || hn == "the-environment" ||
                hn == "load" || hn == "primitive-load" || hn == "local-eval")
                for (int s = scope; s >= 0; s = d_.scopes[static_cast<size_t>(s)].parent)
                    d_.scopes[static_cast<size_t>(s)].opaque = true;
        }
        bool keywords = false;
        for (size_t k = 1; k < f.kids.size(); k++)
            if (N(f.kids[k]).kind == NK::Keyword) keywords = true;
        // A reader error inside the call (another Scheme's `#"..."` or
        // `#/re/`) leaves its operands miscounted.
        bool damaged = false;
        for (const SchemeReadError &e : r_.errors)
            if (SchemeRangeContains(f.range, e.range.line, e.range.col)) damaged = true;
        if (!f.dotted && !damaged) CheckArity(head, b, argc, keywords);
        // The operands of a macro need not be code (Guix's record
        // constructors, with-cps's `$`, set-field's field paths): inside one,
        // calls are not judged.
        const bool macro = !OperandsAreCode(head, b);
        if (macro) in_macro_use_++;
        WalkKids(form, 1, scope, Mode::Code);
        if (macro) in_macro_use_--;
    }

    static std::string ArityText(const Arity &a) {
        std::vector<std::pair<int, int>> cs = a.clauses;
        std::sort(cs.begin(), cs.end());
        cs.erase(std::unique(cs.begin(), cs.end()), cs.end());
        auto one = [](const std::pair<int, int> &c) {
            if (c.second < 0) return "at least " + std::to_string(c.first);
            if (c.first == c.second) return std::to_string(c.first);
            if (c.second == c.first + 1) return std::to_string(c.first) + " or " + std::to_string(c.second);
            return std::to_string(c.first) + " to " + std::to_string(c.second);
        };
        std::string out;
        for (size_t i = 0; i < cs.size(); i++) {
            if (i) out += i + 1 == cs.size() ? " or " : ", ";
            out += one(cs[i]);
        }
        const bool single = cs.size() == 1 && cs[0].first == 1 && (cs[0].second == 1 || cs[0].second < 0);
        return out + (single ? " argument" : " arguments");
    }

    // Whether a call's operands are expressions: a procedure's are, and so
    // are those of the core forms that take expressions; a macro's or an
    // unknown name's may be anything.
    bool OperandsAreCode(int head, int b) const {
        if (b >= 0) return d_.bindings[static_cast<size_t>(b)].kind != BK::Macro;
        const std::string &name = N(head).name;
        static const std::set<std::string> kCodeSyntax = {
            "if", "when", "unless", "cond", "else", "=>", "and", "or", "begin", "delay", "delay-force", "make-promise", "lazy",
            "assert", "cons-stream", "while", "false-if-exception", "with-mutex", "with-throw-handler",
            "unwind-protect", "dynamic-wind", "define-inlinable", "case-lambda", "test-assert", "test-equal",
            "test-eqv", "test-eq", "test-approximate", "test-error", "test-group", "test-begin", "test-end",
            "include", "include-ci", "the-environment", "assq-ref", "start-stack", "with-continuation-barrier",
            "%", "reset", "shift", "call-with-prompt", "future", "touch", "λ",
        };
        if (kCodeSyntax.count(name)) return true;
        const auto it = TheVocab().by_name.find(name);
        if (it == TheVocab().by_name.end()) return false;
        for (const SchemeVocabEntry *e : it->second)
            if (e->kind == SchemeVocabKind::Syntax) return false;
        return true;
    }

    void CheckArity(int head, int b, int argc, bool keywords) {
        if (in_macro_use_ > 0) return;
        const std::string &name = N(head).name;
        Arity a;
        if (b >= 0) {
            const Binding &bd = d_.bindings[static_cast<size_t>(b)];
            if (!bd.arity.known || bd.assigned || bd.def_count > 1 || bd.arity.clauses.empty()) return;
            a = bd.arity;
        } else {
            if (!d_.declares_imports || d_.unknown_imports || keywords) return;  // a keyword operand may be a #:key argument
            const auto it = TheVocab().by_name.find(name);
            if (it == TheVocab().by_name.end()) return;
            bool any = false;
            for (const SchemeVocabEntry *e : it->second) {
                const bool visible = (d_.guile_visible && std::string_view(e->module) == "guile") ||
                                     std::find(d_.modules.begin(), d_.modules.end(), e->module) != d_.modules.end();
                if (!visible) continue;
                if (e->kind != SchemeVocabKind::Procedure || e->min_args < 0 || e->max_args == -2) return;
                a.clauses.push_back({e->min_args, e->max_args});
                any = true;
            }
            if (!any) return;
            a.known = true;
        }
        if (a.Accepts(argc)) return;
        Diag(N(head).range, SchemeLspSeverity::Warning, "wrong-arity",
             name + " takes " + ArityText(a) + ", given " + std::to_string(argc));
    }

    void WalkForm(int form, int scope) {
        const SchemeNode &f = N(form);
        if (f.kids.empty()) return;
        const int head = f.kids[0];
        if (!IsSym(head)) {
            WalkKids(form, 0, scope, Mode::Code);
            return;
        }
        const std::string &h = N(head).name;
        const int local = Lookup(scope, h);
        if (local >= 0 && d_.bindings[static_cast<size_t>(local)].scope != 0) {
            Call(form, scope);
            return;
        }
        if (local >= 0) Ref(head, scope);
        if (h == "quote" || h == "@" || h == "@@" || h == "define-module" || h == "use-modules" || h == "import" ||
            h == "export" || h == "export!" || h == "export-syntax" || h == "re-export" || h == "re-export-syntax" ||
            h == "select-module" || h == "define-peg-pattern" || h == "define-peg-string-patterns") {
            if (h == "export" || h == "export!" || h == "export-syntax" || h == "re-export")
                ExportList(form, 1);
            return;
        }
        if (h == "quasiquote") {
            Quasi(Kid(form, 1), scope, 1, false);
        } else if (h == "gexp") {
            Quasi(Kid(form, 1), scope, 1, true);
        } else if (h == "syntax" || h == "quasisyntax") {
            Walk(Kid(form, 1), scope, Mode::Template);
        } else if (IsDefine(h)) {
            WalkDefine(form, scope);
        } else if (h == "define-syntax" || h == "define-syntax-parameter" || h == "let-syntax-parameter") {
            WalkKids(form, 2, scope, Mode::Code);
        } else if (h == "define-syntax-rule") {
            const int pat = Kid(form, 1);
            const int s = NewScope(scope, f.range);
            std::vector<int> bound;
            if (IsList(pat))
                for (size_t k = 1; k < NKids(pat); k++) SyntaxPattern(Kid(pat, k), s, {}, "...", &bound);
            WalkKids(form, 2, s, Mode::Template);
        } else if (h == "define-macro" || h == "define-macro*") {
            const int pat = Kid(form, 1);
            if (IsList(pat)) {
                const int s = NewScope(scope, f.range);
                BindFormals(pat, 1, s, BK::Param, "macro parameter", false, "define-macro");
                WalkBody(f.kids, 2, s, false);
            } else {
                WalkKids(form, 2, scope, Mode::Code);
            }
        } else if (h == "defmacro") {
            const int s = NewScope(scope, f.range);
            BindFormals(Kid(form, 2), 0, s, BK::Param, "macro parameter", false, "defmacro");
            WalkBody(f.kids, 3, s, false);
        } else if (h == "define-values") {
            WalkKids(form, 2, scope, Mode::Code);
        } else if (h == "define-record-type*") {
            for (size_t k = 5; k < NKids(form); k++) {
                const int x = Kid(form, k);
                if (!IsList(x)) continue;
                for (size_t j = 2; j < NKids(x); j++)
                    if (IsList(Kid(x, j))) WalkKids(Kid(x, j), 1, scope, Mode::Code);  // (default expr), (sanitize proc)
            }
        } else if (h == "define-record-type" || h == "define-condition-type" ||
                   h == "define-generic" || h == "define-accessor") {
            if (h == "define-condition-type") Walk(Kid(form, 2), scope, Mode::Code);
            // Record clauses are names, not code; (protocol expr) / (parent p) are code in R6RS.
            for (size_t k = 2; k < NKids(form); k++) {
                const std::string ch = HeadName(Kid(form, k));
                if (ch == "protocol" || ch == "parent" || ch == "parent-rtd") WalkKids(Kid(form, k), 1, scope, Mode::Code);
            }
        } else if (h == "define-class") {
            if (IsList(Kid(form, 2))) WalkKids(Kid(form, 2), 0, scope, Mode::Code);
            for (size_t k = 3; k < NKids(form); k++) {
                const int slot = Kid(form, k);
                if (!IsList(slot)) {
                    Walk(slot, scope, Mode::Code);
                    continue;
                }
                for (size_t j = 1; j + 1 < NKids(slot); j += 2) {
                    const int kw = Kid(slot, j);
                    if (IsKeyword(kw, "accessor") || IsKeyword(kw, "getter") || IsKeyword(kw, "setter")) continue;
                    Walk(Kid(slot, j + 1), scope, Mode::Code);
                }
            }
        } else if (h == "define-method") {
            const int head2 = Kid(form, 1);
            const int s = NewScope(scope, f.range);
            if (IsList(head2)) BindFormals(head2, 1, s, BK::Param, "method parameter", true, "define-method");
            WalkBody(f.kids, 2, s, false);
        } else if (IsLambda(h)) {
            Lambda(form, 1, scope, "parameter", h == "lambda*" ? "lambda*" : "lambda");
        } else if (h == "case-lambda" || h == "case-lambda*") {
            for (size_t k = 1; k < f.kids.size(); k++)
                if (IsList(Kid(form, k))) Lambda(Kid(form, k), 0, scope, "parameter", "lambda");
        } else if (h == "let" || h == "let*" || h == "letrec" || h == "letrec*" || h == "and-let*") {
            Let(form, scope, h);
        } else if (h == "do") {
            Do(form, scope);
        } else if (h == "let-values" || h == "let*-values") {
            LetValues(form, scope, h == "let*-values");
        } else if (h == "receive") {
            Walk(Kid(form, 2), scope, Mode::Code);
            const int s = NewScope(scope, f.range);
            BindFormals(Kid(form, 1), 0, s, BK::Destructured, "receive", false, "receive");
            WalkBody(f.kids, 3, s, false);
        } else if (h == "let-optional" || h == "let-optional*" || h == "let-keywords" || h == "let-keywords*") {
            Walk(Kid(form, 1), scope, Mode::Code);
            const size_t at = h.rfind("let-keywords", 0) == 0 ? 3 : 2;
            if (at == 3) Walk(Kid(form, 2), scope, Mode::Code);
            const int s = NewScope(scope, f.range);
            if (IsList(Kid(form, at)))
                for (int b : N(Kid(form, at)).kids) {
                    const int name = IsList(b) ? Kid(b, 0) : b;
                    if (IsList(b)) WalkKids(b, 1, s, Mode::Code);
                    if (IsSym(name)) Bind(name, s, BK::Destructured, N(b).range, h);
                }
            WalkBody(f.kids, at + 1, s, false);
        } else if (h == "let-syntax" || h == "letrec-syntax") {
            const int s = NewScope(scope, f.range);
            if (IsList(Kid(form, 1))) {
                for (int b : N(Kid(form, 1)).kids)
                    if (IsList(b) && IsSym(Kid(b, 0))) Bind(Kid(b, 0), s, BK::Macro, N(b).range, h);
                for (int b : N(Kid(form, 1)).kids)
                    if (IsList(b)) WalkKids(b, 1, h == "letrec-syntax" ? s : scope, Mode::Code);
            }
            WalkBody(f.kids, 2, s, false);
        } else if (h == "syntax-rules") {
            SyntaxRules(form, scope);
        } else if (h == "syntax-case") {
            SyntaxCase(form, scope);
        } else if (h == "with-syntax") {
            const int s = NewScope(scope, f.range);
            std::vector<int> bound;
            if (IsList(Kid(form, 1)))
                for (int b : N(Kid(form, 1)).kids)
                    if (IsList(b)) {
                        WalkKids(b, 1, scope, Mode::Code);
                        SyntaxPattern(Kid(b, 0), s, {}, "...", &bound);
                    }
            WalkBody(f.kids, 2, s, false);
        } else if (h == "match" || h == "match-lambda" || h == "match-lambda*") {
            const size_t at = h == "match" ? 2 : 1;
            if (h == "match") Walk(Kid(form, 1), scope, Mode::Code);
            for (size_t k = at; k < f.kids.size(); k++) MatchClause(Kid(form, k), scope);
        } else if (h == "match-let" || h == "match-let*" || h == "match-letrec") {
            if (IsSym(Kid(form, 1))) {  // named match-let
                Call(form, scope);
                return;
            }
            const int s = NewScope(scope, f.range);
            std::vector<int> bound;
            if (IsList(Kid(form, 1)))
                for (int b : N(Kid(form, 1)).kids)
                    if (IsList(b)) {
                        WalkKids(b, 1, h == "match-let" ? scope : s, Mode::Code);
                        MatchPattern(Kid(b, 0), s, &bound);
                    }
            WalkBody(f.kids, 2, s, false);
        } else if (h == "parameterize" || h == "with-fluids" || h == "with-fluids*" || h == "fluid-let" ||
                   h == "syntax-parameterize" || h == "with-parameters") {
            // ((param value) ...) body: both halves of each pair are expressions.
            if (IsList(Kid(form, 1)))
                for (int b : N(Kid(form, 1)).kids) {
                    if (IsList(b)) WalkKids(b, 0, scope, Mode::Code);
                    else Walk(b, scope, Mode::Code);
                }
            WalkBody(f.kids, 2, NewScope(scope, f.range), false);
        } else if (h == "guard") {
            const int spec = Kid(form, 1);
            const int s = NewScope(scope, IsList(spec) ? N(spec).range : f.range);
            if (IsList(spec) && IsSym(Kid(spec, 0))) {
                Bind(Kid(spec, 0), s, BK::Param, N(spec).range, "guard");
                WalkKids(spec, 1, s, Mode::Code);
            }
            WalkBody(f.kids, 2, NewScope(scope, f.range), false);
        } else if (h == "case") {
            Walk(Kid(form, 1), scope, Mode::Code);
            for (size_t k = 2; k < f.kids.size(); k++) {
                const int clause = Kid(form, k);
                if (IsList(clause)) WalkKids(clause, 1, scope, Mode::Code);
            }
        } else if (h == "define-library") {
            for (size_t k = 2; k < f.kids.size(); k++) {
                const int decl = Kid(form, k);
                if (HeadName(decl) == "begin") WalkKids(decl, 1, scope, Mode::Code);
            }
        } else if (h == "library") {
            for (size_t k = 2; k < f.kids.size(); k++) {
                const std::string dh = HeadName(Kid(form, k));
                if (dh != "export" && dh != "import") Walk(Kid(form, k), scope, Mode::Code);
            }
        } else if (h == "cond-expand") {
            for (size_t k = 1; k < f.kids.size(); k++)
                if (IsList(Kid(form, k))) WalkKids(Kid(form, k), 1, scope, Mode::Code);  // declared by the body
        } else if (h == "eval-when") {
            WalkKids(form, 2, scope, Mode::Code);
        } else if (h == "set!") {
            if (IsSym(Kid(form, 1))) Ref(Kid(form, 1), scope, true);
            else Walk(Kid(form, 1), scope, Mode::Code);
            WalkKids(form, 2, scope, Mode::Code);
        } else if (h == "begin" && scope != 0) {
            WalkKids(form, 1, scope, Mode::Code);
        } else {
            Call(form, scope);
        }
    }

    void WalkDefine(int form, int scope) {
        const DefTarget t = Target(form);
        if (t.name < 0) {
            WalkKids(form, 1, scope, Mode::Code);
            return;
        }
        if (!d_.def_binding.count(t.name)) {
            // A define where definitions are not expected -- an operand of a
            // macro (boot-9's define-values*, a compile-time-case clause):
            // bind it here, but what it is for is the macro's business.
            Declare(form, scope, scope == 0, nullptr);
            const auto it = d_.def_binding.find(t.name);
            if (it != d_.def_binding.end()) d_.bindings[static_cast<size_t>(it->second)].exported = true;
        }
        if (t.formals < 0) {
            WalkKids(form, 2, scope, Mode::Code);
            return;
        }
        const int s = NewScope(scope, N(form).range);
        const std::string origin = "parameter of " + N(t.name).name;
        const char *what = HeadName(form) == "define*" || HeadName(form) == "define*-public" ? "lambda*" : "lambda";
        BindFormals(t.formals, 1, s, BK::Param, origin, false, what);
        for (size_t i = t.outer.size(); i-- > 0;) BindFormals(t.outer[i], 1, s, BK::Param, origin, false, what);
        WalkBody(N(form).kids, 2, s, false);
    }

    void Unused() {
        for (const Binding &b : d_.bindings) {
            if (b.reads > 0 || b.exported || IsIgnoredName(b.name)) continue;
            const Scope &sc = d_.scopes[static_cast<size_t>(b.scope)];
            if (sc.opaque || AfterReadError(sc.range.end_line, sc.range.end_col)) continue;
            switch (b.kind) {
                case BK::Lexical:
                    Diag(b.range, SchemeLspSeverity::Warning, "unused-variable",
                         "unused variable " + b.name + (b.assigned ? " (only assigned)" : ""), true);
                    break;
                case BK::Internal:
                    Diag(b.range, SchemeLspSeverity::Warning, "unused-definition",
                         "unused local definition " + b.name, true);
                    break;
                case BK::Param:
                    Diag(b.range, SchemeLspSeverity::Hint, "unused-parameter", "unused parameter " + b.name, true);
                    break;
                case BK::Destructured:
                    if (b.scope != 0)
                        Diag(b.range, SchemeLspSeverity::Hint, "unused-destructured", "unused variable " + b.name, true);
                    break;
                default:
                    break;
            }
        }
    }
};

// --- Queries -------------------------------------------------------------------

bool IsSymbolChar(char c) {
    if (std::isspace(static_cast<unsigned char>(c))) return false;
    return c != '(' && c != ')' && c != '[' && c != ']' && c != '"' && c != ';' && c != '\'' && c != '`' &&
           c != ',';
}

int SymbolNodeAt(const SchemeLspDocument &doc, int line, int col) {
    int best = -1;
    for (size_t i = 0; i < doc.read.nodes.size(); i++) {
        const SchemeNode &n = doc.read.nodes[i];
        if (n.kind != NK::Symbol) continue;
        if (SchemeRangeContains(n.range, line, col)) {
            best = static_cast<int>(i);
            if (PosLess(line, col, n.range.end_line, n.range.end_col)) return best;  // strictly inside wins
        }
    }
    return best;
}

int BindingAt(const SchemeLspDocument &doc, int line, int col) {
    const int n = SymbolNodeAt(doc, line, col);
    if (n < 0) return -1;
    const auto it = doc.occ_of_node.find(n);
    if (it == doc.occ_of_node.end()) return -1;
    return doc.occs[static_cast<size_t>(it->second)].binding;
}

std::vector<const SchemeVocabEntry *> VisibleVocab(const SchemeLspDocument &doc, const std::string &name) {
    std::vector<const SchemeVocabEntry *> out;
    const auto it = TheVocab().by_name.find(name);
    if (it == TheVocab().by_name.end()) return out;
    // Imported modules first, in import order, then (guile).
    for (const std::string &m : doc.modules)
        for (const SchemeVocabEntry *e : it->second)
            if (m == e->module) out.push_back(e);
    if (doc.guile_visible)
        for (const SchemeVocabEntry *e : it->second)
            if (std::string_view(e->module) == "guile") out.push_back(e);
    return out;
}

// Every entry of a name, for hover when the file imports nothing that has it.
std::vector<const SchemeVocabEntry *> AnyVocab(const std::string &name) {
    const auto it = TheVocab().by_name.find(name);
    if (it == TheVocab().by_name.end()) return {};
    return it->second;
}

const char *KindWord(SchemeVocabKind k) {
    switch (k) {
        case SchemeVocabKind::Syntax: return "syntax";
        case SchemeVocabKind::Procedure: return "procedure";
        case SchemeVocabKind::Variable: return "variable";
    }
    return "";
}

std::string BindingTitle(const Binding &b) {
    switch (b.kind) {
        case BK::Global: return b.origin == "generic" || b.origin == "slot accessor" ? b.origin : "variable";
        case BK::Function: return b.origin.empty() ? "procedure" : b.origin;
        case BK::Macro: return "macro";
        case BK::Record: return b.origin.empty() ? "record type" : b.origin;
        case BK::Lexical: return "local variable (" + b.origin + ")";
        case BK::Destructured: return "local variable (" + b.origin + ")";
        case BK::Param: return b.origin;
        case BK::Internal: return b.sig.empty() ? "internal definition" : "internal procedure";
        case BK::Pattern: return b.origin;
        case BK::LoopName: return "named let loop";
    }
    return "";
}

std::string VocabHover(const std::vector<const SchemeVocabEntry *> &es) {
    std::string sig, kind, mods;
    std::set<std::string> seen;
    for (const SchemeVocabEntry *e : es) {
        if (sig.empty() && e->signature[0]) sig = e->signature;
        if (kind.empty()) kind = KindWord(e->kind);
        if (seen.insert(e->module).second && seen.size() <= 6) mods += std::string(mods.empty() ? "" : ", ") + "(" + e->module + ")";
    }
    std::string out = "```scheme\n" + (sig.empty() ? std::string(es[0]->name) : sig) + "\n```\n\n*" + kind + "*";
    if (!mods.empty()) out += " — " + mods;
    return out;
}

// The innermost list whose parentheses enclose (line, col).
int EnclosingList(const SchemeLspDocument &doc, int line, int col) {
    int best = -1;
    for (size_t i = 0; i < doc.read.nodes.size(); i++) {
        const SchemeNode &n = doc.read.nodes[i];
        if (n.kind != NK::List) continue;
        if (!PosLess(n.range.line, n.range.col, line, col)) continue;
        if (n.closed && !PosLess(line, col, n.range.end_line, n.range.end_col)) continue;
        if (best < 0 || PosLess(doc.read.nodes[static_cast<size_t>(best)].range.line,
                                doc.read.nodes[static_cast<size_t>(best)].range.col, n.range.line, n.range.col))
            best = static_cast<int>(i);
    }
    return best;
}

std::vector<std::string> SplitSignature(const std::string &sig) {
    // "(name a b #:optional c . rest)" -> {a, b, c, rest}
    std::vector<std::string> out;
    if (sig.size() < 2 || sig[0] != '(') return out;
    size_t i = 1;
    int depth = 0;
    bool first = true;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) {
            if (!first && cur != "." && cur[0] != '#') out.push_back(cur);
            first = false;
            cur.clear();
        }
    };
    for (; i + 1 < sig.size() || (i < sig.size() && sig[i] != ')'); i++) {
        const char c = sig[i];
        if (c == '(' || c == '[') depth++;
        if (c == ')' || c == ']') {
            if (depth == 0) break;
            depth--;
        }
        if (c == ' ' && depth == 0) {
            flush();
            continue;
        }
        cur += c;
    }
    flush();
    return out;
}

}  // namespace

std::shared_ptr<const SchemeLspDocument> SchemeLspAnalyze(const std::vector<std::string> &lines,
                                                          const std::string &path) {
    auto doc = std::make_shared<SchemeLspDocument>();
    doc->path = SchemeLspUriToPath(path);
    doc->lines = lines;
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    doc->read = SchemeRead(text);
    Analyzer(doc.get()).Run();
    return doc;
}

std::vector<SchemeLspDiagnostic> SchemeLspDiagnostics(const SchemeLspDocument &doc) {
    std::vector<SchemeLspDiagnostic> out;
    for (const SchemeReadError &e : doc.read.errors) {
        SchemeLspDiagnostic d;
        d.range = ToLsp(e.range);
        d.severity = SchemeLspSeverity::Error;
        d.code = e.code;
        d.message = e.message;
        out.push_back(d);
    }
    for (const SchemeLspDiagnostic &d : doc.diags) out.push_back(d);
    std::stable_sort(out.begin(), out.end(), [](const SchemeLspDiagnostic &a, const SchemeLspDiagnostic &b) {
        return PosLess(a.range.line, a.range.col, b.range.line, b.range.col);
    });
    return out;
}

std::string SchemeLspFormatDiagnostic(const std::string &path, const SchemeLspDiagnostic &d) {
    const char *sev = d.severity == SchemeLspSeverity::Error         ? "error"
                      : d.severity == SchemeLspSeverity::Warning     ? "warning"
                      : d.severity == SchemeLspSeverity::Information ? "info"
                                                                     : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool SchemeLspDiagnosticFails(const SchemeLspDiagnostic &d) {
    return d.severity == SchemeLspSeverity::Error || d.severity == SchemeLspSeverity::Warning;
}

std::vector<SchemeLspCompletionItem> SchemeLspCompletions(const SchemeLspDocument &doc, int line, int col) {
    std::vector<SchemeLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return out;
    const std::string &cur = doc.lines[static_cast<size_t>(line)];
    col = std::max(0, std::min(col, static_cast<int>(cur.size())));
    int start = col;
    while (start > 0 && IsSymbolChar(cur[static_cast<size_t>(start - 1)])) start--;
    if (start < col && cur[static_cast<size_t>(start)] == '#') return out;  // #t, #\x, #:key
    const std::string prefix = cur.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    std::set<std::string> seen;
    auto add = [&](const std::string &label, SchemeLspKind kind, const std::string &detail) {
        if (label.compare(0, prefix.size(), prefix) != 0 || !seen.insert(label).second) return;
        SchemeLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.replace_start = start;
        it.replace_end = col;
        out.push_back(it);
    };
    // File bindings visible here: those of every scope enclosing the cursor.
    for (const Scope &s : doc.scopes) {
        const bool inside = &s == &doc.scopes[0] ||
                            (SchemeRangeContains(s.range, line, col) &&
                             PosLess(s.range.line, s.range.col, line, col));
        if (!inside) continue;
        for (int bi : s.bindings) {
            const Binding &b = doc.bindings[static_cast<size_t>(bi)];
            if (b.range.line == line && b.range.col == start) continue;  // the word being typed
            const SchemeLspKind k = b.kind == BK::Function || b.kind == BK::LoopName || !b.sig.empty()
                                        ? SchemeLspKind::Function
                                    : b.kind == BK::Macro  ? SchemeLspKind::Keyword
                                    : b.kind == BK::Record ? SchemeLspKind::Struct
                                                           : SchemeLspKind::Variable;
            add(b.name, k, b.sig.empty() ? BindingTitle(b) : b.sig);
        }
    }
    if (prefix.empty()) return out;
    std::vector<std::string> mods = doc.modules;
    if (doc.guile_visible || mods.empty()) mods.push_back("guile");
    for (const SchemeVocabEntry *e = SchemeVocabBegin(); e != SchemeVocabEnd(); ++e) {
        if (std::find(mods.begin(), mods.end(), e->module) == mods.end()) continue;
        const SchemeLspKind k = e->kind == SchemeVocabKind::Syntax      ? SchemeLspKind::Keyword
                                : e->kind == SchemeVocabKind::Procedure ? SchemeLspKind::Function
                                                                        : SchemeLspKind::Variable;
        add(e->name, k, e->signature[0] ? std::string(e->signature) : "(" + std::string(e->module) + ")");
    }
    return out;
}

SchemeLspHoverInfo SchemeLspHover(const SchemeLspDocument &doc, int line, int col) {
    SchemeLspHoverInfo h;
    const int n = SymbolNodeAt(doc, line, col);
    if (n < 0) return h;
    const SchemeNode &sym = doc.read.nodes[static_cast<size_t>(n)];
    h.range = ToLsp(sym.range);
    const int b = BindingAt(doc, line, col);
    if (b >= 0) {
        const Binding &bd = doc.bindings[static_cast<size_t>(b)];
        h.found = true;
        h.text = "```scheme\n" + (bd.sig.empty() ? bd.name : bd.sig) + "\n```\n\n";
        if (!bd.doc.empty()) h.text += bd.doc + "\n\n";
        h.text += "*" + BindingTitle(bd) + "*";
        if (bd.scope == 0) h.text += " — line " + std::to_string(bd.range.line + 1);
        return h;
    }
    std::vector<const SchemeVocabEntry *> es = VisibleVocab(doc, sym.name);
    if (es.empty()) {
        // A definition of a module the file uses, or of a file it loads.
        const SchemeLspDefDoc dd = SchemeLspFindDefinition(sym.name, SchemeLspSearchFiles(doc));
        if (dd.found) {
            h.found = true;
            h.text = SchemeLspDefMarkdown(dd);
            return h;
        }
        es = AnyVocab(sym.name);
    }
    if (es.empty()) return h;
    h.found = true;
    h.text = VocabHover(es);
    return h;
}

SchemeLspSignatureHelp SchemeLspSignatureAt(const SchemeLspDocument &doc, int line, int col) {
    SchemeLspSignatureHelp sh;
    int list = EnclosingList(doc, line, col);
    for (int guard = 0; list >= 0 && guard < 64; guard++) {
        const SchemeNode &l = doc.read.nodes[static_cast<size_t>(list)];
        if (l.kids.empty()) return sh;
        const SchemeNode &head = doc.read.nodes[static_cast<size_t>(l.kids[0])];
        if (head.kind != NK::Symbol || SchemeRangeContains(head.range, line, col)) return sh;
        std::string sig;
        const auto it = doc.occ_of_node.find(l.kids[0]);
        if (it != doc.occ_of_node.end()) {
            const Binding &b = doc.bindings[static_cast<size_t>(doc.occs[static_cast<size_t>(it->second)].binding)];
            sig = b.sig;
            if (sig.empty()) return sh;
            sh.documentation = b.doc;
        } else {
            for (const SchemeVocabEntry *e : VisibleVocab(doc, head.name))
                if (e->signature[0]) {
                    sig = e->signature;
                    break;
                }
            if (sig.empty()) {
                const SchemeLspDefDoc dd = SchemeLspFindDefinition(head.name, SchemeLspSearchFiles(doc));
                if (dd.found) {
                    sig = dd.sig;
                    sh.documentation = dd.doc;
                }
            }
        }
        if (sig.empty()) return sh;
        sh.found = true;
        sh.label = sig;
        sh.params = SplitSignature(sig);
        int active = 0;
        for (size_t k = 1; k < l.kids.size(); k++) {
            const SchemeRange &r = doc.read.nodes[static_cast<size_t>(l.kids[k])].range;
            if (PosLess(r.end_line, r.end_col, line, col)) active++;
        }
        sh.active_param = sh.params.empty() ? 0 : std::min(active, static_cast<int>(sh.params.size()) - 1);
        const bool rest = sig.find(" . ") != std::string::npos;
        if (!rest && active >= static_cast<int>(sh.params.size())) sh.active_param = active;
        return sh;
    }
    return sh;
}

std::vector<SchemeLspLocation> SchemeLspDefinition(const SchemeLspDocument &doc, int line, int col) {
    const int b = BindingAt(doc, line, col);
    if (b < 0) return {};
    SchemeLspLocation loc;
    loc.range = ToLsp(doc.bindings[static_cast<size_t>(b)].range);
    return {loc};
}

std::vector<SchemeLspReference> SchemeLspReferences(const SchemeLspDocument &doc, int line, int col) {
    std::vector<SchemeLspReference> out;
    const int b = BindingAt(doc, line, col);
    if (b < 0) return out;
    for (const Occ &o : doc.occs)
        if (o.binding == b) out.push_back({ToLsp(o.range), o.def});
    std::sort(out.begin(), out.end(), [](const SchemeLspReference &a, const SchemeLspReference &c) {
        return PosLess(a.range.line, a.range.col, c.range.line, c.range.col);
    });
    return out;
}

SchemeLspRenameTarget SchemeLspPrepareRename(const SchemeLspDocument &doc, int line, int col) {
    SchemeLspRenameTarget t;
    const int b = BindingAt(doc, line, col);
    if (b < 0) return t;
    const int n = SymbolNodeAt(doc, line, col);
    const SchemeNode &sym = doc.read.nodes[static_cast<size_t>(n)];
    // Only a name written plainly can be renamed by replacing its text.
    if (sym.range.line != sym.range.end_line ||
        doc.lines[static_cast<size_t>(sym.range.line)].substr(static_cast<size_t>(sym.range.col),
                                                              static_cast<size_t>(sym.range.end_col - sym.range.col)) !=
            sym.name)
        return t;
    t.found = true;
    t.range = ToLsp(sym.range);
    t.placeholder = sym.name;
    return t;
}

std::vector<SchemeLspTextEdit> SchemeLspRename(const SchemeLspDocument &doc, int line, int col,
                                               const std::string &new_name, std::string *error) {
    std::vector<SchemeLspTextEdit> edits;
    if (!SchemeLspPrepareRename(doc, line, col).found) {
        *error = "no renameable symbol here";
        return edits;
    }
    bool ok = !new_name.empty() && new_name[0] != '#' && !SchemeIsNumber(new_name);
    for (char c : new_name)
        if (!IsSymbolChar(c) || c == '|') ok = false;
    if (ok) {
        const SchemeReadResult r = SchemeRead(new_name);
        ok = r.ok() && r.top.size() == 1 && r.nodes[static_cast<size_t>(r.top[0])].kind == NK::Symbol &&
             r.nodes[static_cast<size_t>(r.top[0])].name == new_name;
    }
    if (!ok) {
        *error = "'" + new_name + "' is not a plain Scheme symbol";
        return edits;
    }
    for (const SchemeLspReference &ref : SchemeLspReferences(doc, line, col)) {
        SchemeLspTextEdit e;
        e.range = ref.range;
        e.new_text = new_name;
        edits.push_back(e);
    }
    return edits;
}

namespace {

void SymbolsOf(const SchemeLspDocument &doc, int form, int parent, std::vector<SchemeLspSymbol> *out) {
    const SchemeReadResult &r = doc.read;
    auto node = [&](int i) -> const SchemeNode & { return r.nodes[static_cast<size_t>(i)]; };
    const SchemeNode &f = node(form);
    if (f.kind != NK::List || f.kids.empty() || node(f.kids[0]).kind != NK::Symbol) return;
    const std::string &h = node(f.kids[0]).name;
    auto kid = [&](size_t k) { return k < f.kids.size() ? f.kids[k] : -1; };
    auto add = [&](int name_node, SchemeLspSymbolKind kind, const std::string &detail, int par) {
        SchemeLspSymbol s;
        s.name = name_node >= 0 && node(name_node).kind == NK::Symbol ? node(name_node).name : std::string("?");
        s.kind = kind;
        s.detail = detail;
        s.range = ToLsp(f.range);
        s.sel_range = name_node >= 0 ? ToLsp(node(name_node).range) : ToLsp(f.range);
        s.parent = par;
        out->push_back(s);
        return static_cast<int>(out->size()) - 1;
    };
    if (h == "begin" || h == "eval-when" || h == "cond-expand") {
        for (size_t k = 1; k < f.kids.size(); k++) {
            if (h == "cond-expand") {
                const SchemeNode &c = node(f.kids[k]);
                if (c.kind == NK::List)
                    for (size_t j = 1; j < c.kids.size(); j++) SymbolsOf(doc, c.kids[j], parent, out);
            } else {
                SymbolsOf(doc, f.kids[k], parent, out);
            }
        }
        return;
    }
    if (h == "define-module" || h == "define-library" || h == "library") {
        SchemeLspSymbol s;
        const int name = kid(1);
        if (name >= 0) {
            const SchemeRange &nr = node(name).range;
            std::string t;
            for (int l = nr.line; l <= nr.end_line; l++) {
                const std::string &ln = doc.lines[static_cast<size_t>(l)];
                const int a = l == nr.line ? nr.col : 0;
                const int b = l == nr.end_line ? nr.end_col : static_cast<int>(ln.size());
                if (l != nr.line) t += ' ';
                if (b > a && a <= static_cast<int>(ln.size()))
                    t += ln.substr(static_cast<size_t>(a), static_cast<size_t>(std::min<int>(b, static_cast<int>(ln.size())) - a));
            }
            s.name = t;
            s.sel_range = ToLsp(nr);
        } else {
            s.name = h;
            s.sel_range = ToLsp(f.range);
        }
        s.kind = SchemeLspSymbolKind::Module;
        s.detail = h;
        s.range = ToLsp(f.range);
        s.parent = parent;
        out->push_back(s);
        const int me = static_cast<int>(out->size()) - 1;
        if (h != "define-module")
            for (size_t k = 2; k < f.kids.size(); k++) {
                const SchemeNode &decl = node(f.kids[k]);
                if (h == "define-library") {
                    if (decl.kind == NK::List && !decl.kids.empty() && node(decl.kids[0]).kind == NK::Symbol &&
                        node(decl.kids[0]).name == "begin")
                        for (size_t j = 1; j < decl.kids.size(); j++) SymbolsOf(doc, decl.kids[j], me, out);
                } else {
                    SymbolsOf(doc, f.kids[k], me, out);
                }
            }
        return;
    }
    const bool define = h == "define" || h == "define-public" || h == "define*" || h == "define*-public" ||
                        h == "define-inlinable" || h == "define-integrable" || h == "define-once" ||
                        h == "define-constant" || h == "define-generic" || h == "define-accessor";
    if (define) {
        int name = kid(1);
        bool proc = false;
        while (name >= 0 && node(name).kind == NK::List && !node(name).kids.empty()) {
            name = node(name).kids[0];
            proc = true;
        }
        if (name < 0 || node(name).kind != NK::Symbol) return;
        if (!proc && kid(2) >= 0 && node(kid(2)).kind == NK::List && !node(kid(2)).kids.empty()) {
            const SchemeNode &vh = node(node(kid(2)).kids[0]);
            if (vh.kind == NK::Symbol && (vh.name == "lambda" || vh.name == "lambda*" || vh.name == "case-lambda"))
                proc = true;
        }
        add(name, proc ? SchemeLspSymbolKind::Function : SchemeLspSymbolKind::Variable, h, parent);
        return;
    }
    if (h == "define-method") {
        int name = kid(1);
        if (name >= 0 && node(name).kind == NK::List && !node(name).kids.empty()) name = node(name).kids[0];
        if (name >= 0 && node(name).kind == NK::Symbol) add(name, SchemeLspSymbolKind::Method, h, parent);
        return;
    }
    if (h == "define-syntax" || h == "define-syntax-rule" || h == "define-macro" || h == "defmacro" ||
        h == "define-syntax-parameter") {
        int name = kid(1);
        if (name >= 0 && node(name).kind == NK::List && !node(name).kids.empty()) name = node(name).kids[0];
        if (name >= 0 && node(name).kind == NK::Symbol) add(name, SchemeLspSymbolKind::Function, "macro", parent);
        return;
    }
    if (h == "define-values") {
        const int formals = kid(1);
        if (formals >= 0 && node(formals).kind == NK::List)
            for (int k : node(formals).kids)
                if (node(k).kind == NK::Symbol) add(k, SchemeLspSymbolKind::Variable, h, parent);
        return;
    }
    if (h == "define-record-type" || h == "define-class" || h == "define-condition-type") {
        int name = kid(1);
        if (name >= 0 && node(name).kind == NK::List && !node(name).kids.empty()) name = node(name).kids[0];
        if (name < 0 || node(name).kind != NK::Symbol) return;
        const int me = add(name, SchemeLspSymbolKind::Class, h, parent);
        if (h == "define-record-type") {
            for (size_t k = 4; k < f.kids.size(); k++) {
                const SchemeNode &fld = node(f.kids[k]);
                if (fld.kind == NK::List && !fld.kids.empty() && node(fld.kids[0]).kind == NK::Symbol) {
                    SchemeLspSymbol s;
                    s.name = node(fld.kids[0]).name;
                    s.kind = SchemeLspSymbolKind::Field;
                    s.range = ToLsp(fld.range);
                    s.sel_range = ToLsp(node(fld.kids[0]).range);
                    s.parent = me;
                    out->push_back(s);
                }
            }
        }
    }
}

}  // namespace

std::vector<SchemeLspSymbol> SchemeLspSymbols(const SchemeLspDocument &doc) {
    std::vector<SchemeLspSymbol> out;
    for (int f : doc.read.top) SymbolsOf(doc, f, -1, &out);
    return out;
}

std::vector<SchemeLspFold> SchemeLspFolds(const SchemeLspDocument &doc) {
    std::vector<SchemeLspFold> out;
    std::set<int> starts;
    for (const SchemeNode &n : doc.read.nodes) {
        if (n.kind != NK::List && n.kind != NK::Vector && n.kind != NK::String) continue;
        if (n.range.end_line <= n.range.line) continue;
        if (!starts.insert(n.range.line).second) continue;
        out.push_back({n.range.line, n.range.end_line, ""});
    }
    // Comment folds: block comments, and runs of two or more line comments.
    int run_start = -1, run_end = -1;
    auto flush = [&] {
        if (run_start >= 0 && run_end > run_start && starts.insert(run_start).second)
            out.push_back({run_start, run_end, "comment"});
        run_start = run_end = -1;
    };
    for (const SchemeComment &c : doc.read.comments) {
        if (c.kind == SchemeCommentKind::Line) {
            if (run_start >= 0 && c.range.line == run_end + 1) {
                run_end = c.range.line;
            } else {
                flush();
                run_start = run_end = c.range.line;
            }
            continue;
        }
        flush();
        if (c.kind == SchemeCommentKind::Block && c.range.end_line > c.range.line && starts.insert(c.range.line).second)
            out.push_back({c.range.line, c.range.end_line, "comment"});
    }
    flush();
    std::sort(out.begin(), out.end(),
              [](const SchemeLspFold &a, const SchemeLspFold &b) { return a.start_line < b.start_line; });
    return out;
}

std::vector<SchemeLspTextEdit> SchemeLspFormat(const std::vector<std::string> &lines, int width, std::string *error) {
    std::vector<SchemeLspTextEdit> edits;
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    (void)width;  // the layout never wraps
    const schemefmt::Result r = schemefmt::Format(text);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    // The lines of a buffer carry no final newline unless the last one is empty.
    std::string got = r.text;
    if (!text.empty() && text.back() != '\n' && !got.empty() && got.back() == '\n') got.pop_back();
    if (got == text) return edits;
    // One edit over the changed lines.
    std::vector<std::string> out;
    {
        std::string cur;
        for (char c : got) {
            if (c == '\n') {
                out.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        out.push_back(cur);
    }
    size_t a = 0;
    while (a < lines.size() && a < out.size() && lines[a] == out[a]) a++;
    size_t el = lines.size(), eo = out.size();
    while (el > a && eo > a && lines[el - 1] == out[eo - 1]) el--, eo--;
    SchemeLspTextEdit e;
    e.range.line = static_cast<int>(a);
    e.range.col = 0;
    if (el < lines.size()) {
        e.range.end_line = static_cast<int>(el);
        e.range.end_col = 0;
        for (size_t i = a; i < eo; i++) e.new_text += out[i] + "\n";
    } else {
        // The change reaches the last line: replace through its end.
        e.range.end_line = static_cast<int>(lines.size()) - 1;
        e.range.end_col = static_cast<int>(lines.back().size());
        if (a > 0 && a == lines.size()) {
            // Only lines added at the end.
            e.range.line = static_cast<int>(lines.size()) - 1;
            e.range.col = static_cast<int>(lines.back().size());
            for (size_t i = a; i < eo; i++) e.new_text += "\n" + out[i];
        } else {
            for (size_t i = a; i < eo; i++) e.new_text += (i > a ? "\n" : "") + out[i];
        }
    }
    edits.push_back(e);
    return edits;
}

// --- Definitions of other files ------------------------------------------------------

std::string SchemeLspUriToPath(const std::string &uri) {
    if (uri.compare(0, 7, "file://") != 0) return uri;
    const std::string p = uri.substr(7);
    std::string out;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '%' && i + 2 < p.size() && std::isxdigit(static_cast<unsigned char>(p[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(p[i + 2]))) {
            out += static_cast<char>(std::stoi(p.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += p[i];
        }
    }
#if defined(_WIN32)
    if (out.size() > 2 && out[0] == '/' && out[2] == ':') out.erase(0, 1);
#endif
    return out;
}

namespace {

std::vector<std::string> &ServerSearchPath() {
    static std::vector<std::string> dirs;
    return dirs;
}

std::string DirOf(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return std::string();
    return slash == 0 ? std::string("/") : path.substr(0, slash);
}

std::string JoinPath(const std::string &dir, const std::string &rel) {
    if (rel.empty() || rel[0] == '/' || dir.empty()) return rel;
    return dir + "/" + rel;
}

// The top-level definitions of one file, analysed once per modification time.
const std::vector<SchemeLspDefDoc> &DefsOf(const std::string &path) {
    struct Entry {
        std::filesystem::file_time_type mtime;
        std::vector<SchemeLspDefDoc> defs;
    };
    static std::map<std::string, Entry> cache;
    static const std::vector<SchemeLspDefDoc> none;
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) return none;
    auto it = cache.find(path);
    if (it != cache.end() && it->second.mtime == mtime) return it->second.defs;
    std::ifstream in(path, std::ios::binary);
    if (!in) return none;
    std::vector<std::string> lines;
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    const auto doc = SchemeLspAnalyze(lines, path);
    Entry e;
    e.mtime = mtime;
    for (int bi : doc->scopes[0].bindings) {
        const Binding &b = doc->bindings[static_cast<size_t>(bi)];
        SchemeLspDefDoc d;
        d.found = true;
        d.name = b.name;
        d.sig = b.sig;
        d.doc = b.doc;
        d.kind = BindingTitle(b);
        d.path = path;
        d.line = b.range.line;
        d.exported = b.exported;
        e.defs.push_back(d);
    }
    return (cache[path] = std::move(e)).defs;
}

// The string literal of `(head "x")` / `(head (dirname (current-filename)))` forms, by line scan.
void LiteralForms(const std::vector<std::string> &lines, const std::string &head, const std::string &own,
                  std::vector<std::string> *out) {
    const std::string open = "(" + head + " ";
    for (const std::string &line : lines) {
        size_t at = line.find(open);
        while (at != std::string::npos) {
            size_t i = at + open.size();
            while (i < line.size() && line[i] == ' ') i++;
            if (i < line.size() && line[i] == '"') {
                const size_t e = line.find('"', i + 1);
                if (e != std::string::npos) out->push_back(JoinPath(own, line.substr(i + 1, e - i - 1)));
            } else if (line.compare(i, 29, "(dirname (current-filename)))") == 0 && !own.empty()) {
                out->push_back(own);
            }
            at = line.find(open, at + 1);
        }
    }
}

}  // namespace

void SchemeLspSetSearchPath(const std::vector<std::string> &dirs) { ServerSearchPath() = dirs; }

std::string SchemeLspModuleFile(const std::string &module, const std::vector<std::string> &dirs) {
    // "a b c" or "(a b c)" -> a/b/c.scm (or .sls / .ss / .sld).
    std::string rel;
    std::string cur;
    for (char c : module + " ") {
        if (c == '(' || c == ')') continue;
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) rel += (rel.empty() ? "" : "/") + cur;
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (rel.empty()) return std::string();
    for (const std::string &d : dirs) {
        for (const char *ext : {".scm", ".sls", ".sld", ".ss"}) {
            const std::string p = JoinPath(d, rel + ext);
            std::error_code ec;
            if (std::filesystem::is_regular_file(p, ec)) return p;
        }
    }
    return std::string();
}

std::vector<std::string> SchemeLspSearchFiles(const SchemeLspDocument &doc) {
    const std::string own = DirOf(doc.path);
    std::vector<std::string> dirs, files;
    LiteralForms(doc.lines, "add-to-load-path", own, &dirs);
    if (!own.empty()) dirs.push_back(own);
    for (const std::string &d : ServerSearchPath()) dirs.push_back(d);
    for (const std::string &m : doc.other_modules) {
        const std::string f = SchemeLspModuleFile(m, dirs);
        if (!f.empty() && f != doc.path && std::find(files.begin(), files.end(), f) == files.end()) files.push_back(f);
    }
    std::vector<std::string> loaded;
    LiteralForms(doc.lines, "load", own, &loaded);
    LiteralForms(doc.lines, "include", own, &loaded);
    LiteralForms(doc.lines, "primitive-load", own, &loaded);
    for (const std::string &f : loaded)
        if (std::find(files.begin(), files.end(), f) == files.end()) files.push_back(f);
    return files;
}

SchemeLspDefDoc SchemeLspFindDefinition(const std::string &name, const std::vector<std::string> &files) {
    for (const std::string &f : files)
        for (const SchemeLspDefDoc &d : DefsOf(f))
            if (d.name == name) return d;
    return SchemeLspDefDoc();
}

std::string SchemeLspDefMarkdown(const SchemeLspDefDoc &d) {
    std::string s = "```scheme\n" + (d.sig.empty() ? d.name : d.sig) + "\n```\n\n";
    if (!d.doc.empty()) s += d.doc + "\n\n";
    s += "*" + d.kind + "* — " + d.path + ":" + std::to_string(d.line + 1);
    return s;
}

std::string SchemeLspVocabDoc(const std::string &name) {
    std::vector<const SchemeVocabEntry *> es;
    for (const SchemeVocabEntry *e = SchemeVocabBegin(); e != SchemeVocabEnd(); ++e)
        if (name == e->name) es.push_back(e);
    if (es.empty()) return std::string();
    std::string out = VocabHover(es);
    size_t at;
    while ((at = out.find("```scheme\n")) != std::string::npos) out.erase(at, 10);
    while ((at = out.find("```\n")) != std::string::npos) out.erase(at, 4);
    return out + "\n";
}
