// mep's Octave language server, analysis half: see octave_lsp.h.
//
// Everything sits on octave_parse.h, mep's reader for Octave whose errors
// are Octave's own (message and "near line L, column C", checked against
// `__parse_file__` by tools/octave_parse_check.py). On top of the tree:
//
//   - a scope model: one scope per function (a nested function -- one
//     inside a function closed by `end` -- shares the variables its parent
//     also has, as Octave's nested functions do), one per anonymous
//     function (its parameters; everything else is the enclosing scope's),
//     and the script's top level. A name assigned anywhere in a scope is a
//     variable there; any other name is a function call.
//   - Octave's own functions from octave_lsp_vocab.cpp (names and usage
//     lines generated from Octave 11 by tools/gen_octave_vocab.py).
//
// The parser stops at the first error, as Octave does. For the features
// (not the diagnostics) a broken document is re-read with the offending
// line blanked (or an `end` appended when the file just runs out), a few
// times over, so completion and navigation keep working while typing.
#include "octave_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

#include "octave_format.h"
#include "octave_lsp_vocab.h"
#include "octave_parse.h"

using octparse::AstNode;
using octparse::Node;
using octparse::ParseResult;
using octparse::Tok;
using octparse::Token;

namespace {

struct Occurrence {
    int tok = -1;
    int scope = -1;
    bool write = false;
    bool read = false;
    bool command = false;  // the name of a command-syntax call
    bool handle = false;   // @name
};

struct Scope {
    int parent = -1;     // lookups continue here (anonymous / nested functions)
    int fn = -1;         // Function node, -1 for the top level / an anonymous function
    bool anon = false;
    bool nested = false;  // a nested function: shares its parent's variables
    bool dynamic = false;  // eval/assignin/load/...: names can appear that the text does not show
    std::set<std::string> params, outputs, globals, loop_vars, vars;
};

struct FunctionInfo {
    std::string name;
    int node = -1;
    int name_tok = -1;
    int parent = -1;  // index into functions, -1 at file level
    bool method = false;
};

std::string Basename(const std::string &path) {
    std::string p = path;
    const size_t slash = p.find_last_of("/\\");
    if (slash != std::string::npos) p = p.substr(slash + 1);
    if (p.size() > 2 && p.compare(p.size() - 2, 2, ".m") == 0) p.resize(p.size() - 2);
    return p;
}

bool IsWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool IsIdentifier(const std::string &s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!IsWordChar(c)) return false;
    return true;
}

// Calls that read or write variables the text does not name.
bool IsDynamicCall(const std::string &n) {
    static const std::set<std::string> k = {"eval", "evalin", "assignin", "load", "save", "exist", "who",
                                            "whos", "clear", "clearvars", "inputname", "evalc", "import",
                                            "inputParser", "keyboard", "dbstop", "var", "isvarname", "genvarname",
                                            "fieldnames", "struct2vars", "symvar"};
    return k.count(n) > 0;
}

const char *const kKeywords[] = {"break", "case", "catch", "classdef", "continue", "do", "else", "elseif", "end",
                                 "end_try_catch", "end_unwind_protect", "endclassdef", "endenumeration",
                                 "endevents", "endfor", "endfunction", "endif", "endmethods", "endparfor",
                                 "endproperties", "endspmd", "endswitch", "endwhile", "enumeration", "events",
                                 "for", "function", "global", "if", "methods", "otherwise", "parfor",
                                 "persistent", "properties", "return", "spmd", "switch", "try", "until",
                                 "unwind_protect", "unwind_protect_cleanup", "while", "arguments"};

}  // namespace

class OctaveLspDocument {
public:
    std::vector<std::string> lines;
    std::string text;
    std::vector<int> line_start;
    ParseResult parse;     // the text as written: the diagnostics
    std::string ptext;     // the (possibly repaired) text `tree` was read from
    ParseResult tree;      // what the features use
    std::vector<Scope> scopes;
    std::vector<Occurrence> occ;
    std::vector<int> tok_occ;  // token index -> occurrence index (-1)
    std::vector<FunctionInfo> functions;
    std::map<int, int> fn_scope;  // Function node -> scope
    std::vector<int> unreachable;  // first statement nodes after a return/break/continue
    std::vector<std::pair<int, int>> duplicate_cases;  // (case label node, first label node)

    const Token &T(int i) const { return tree.tokens[static_cast<size_t>(i)]; }
    const AstNode &N(int i) const { return tree.nodes[static_cast<size_t>(i)]; }

    OctaveLspRange TokRange(const ParseResult &r, int i) const {
        const Token &t = r.tokens[static_cast<size_t>(i)];
        OctaveLspRange g;
        g.line = t.line;
        g.col = t.col;
        g.end_line = t.line;
        g.end_col = t.col + std::max(0, t.end - t.begin);
        return g;
    }
    OctaveLspRange TokRange(int i) const { return TokRange(tree, i); }
    void PosOf(int off, int *line, int *col) const {
        const auto it = std::upper_bound(line_start.begin(), line_start.end(), off);
        const int l = static_cast<int>(it - line_start.begin()) - 1;
        *line = std::max(0, l);
        *col = off - line_start[static_cast<size_t>(*line)];
    }
    // A node's range from its first real token to its last.
    bool NodeRange(int n, OctaveLspRange *g) const {
        const AstNode &a = N(n);
        int f = a.first, l = a.last;
        if (f < 0 || l < f) return false;
        while (f <= l && T(f).synthetic) f++;
        while (l >= f && (T(l).synthetic || T(l).kind == Tok::Newline)) l--;
        if (f > l) return false;
        PosOf(T(f).begin, &g->line, &g->col);
        PosOf(T(l).end, &g->end_line, &g->end_col);
        return true;
    }
    std::string Name(int tok) const { return T(tok).text; }
    int TokenAt(int line, int col) const {
        for (size_t i = 0; i < tree.tokens.size(); i++) {
            const Token &t = tree.tokens[i];
            if (t.synthetic || t.line != line) continue;
            if (t.kind != Tok::Name && t.kind != Tok::FcnHandle && t.kind != Tok::StructElt) continue;
            const int b = t.col + (t.kind == Tok::FcnHandle ? 1 : 0);
            const int e = t.col + (t.end - t.begin);
            if (col >= b && col <= e) return static_cast<int>(i);
        }
        return -1;
    }
    // Where a name's variable lives, starting from `scope`; -1 when it is not a variable.
    int Resolve(const std::string &name, int scope) const {
        int s = scope;
        int found = -1;
        while (s >= 0) {
            const Scope &sc = scopes[static_cast<size_t>(s)];
            if (sc.vars.count(name)) found = s;
            if (sc.anon) {
                if (found >= 0) return found;
                s = sc.parent;
                continue;
            }
            if (sc.nested) {
                s = sc.parent;  // the outermost scope that has it owns it
                continue;
            }
            break;
        }
        return found;
    }
    const FunctionInfo *FindFunction(const std::string &name) const {
        for (const FunctionInfo &f : functions)
            if (f.name == name) return &f;
        return nullptr;
    }
};

namespace {

class Analyzer {
public:
    explicit Analyzer(OctaveLspDocument &d) : d_(d) {}

    void Run() {
        const ParseResult &r = d_.tree;
        d_.tok_occ.assign(r.tokens.size(), -1);
        d_.scopes.push_back(Scope());
        if (r.root >= 0) Walk(r.root, 0, -1);
        for (size_t i = 0; i < d_.occ.size(); i++) d_.tok_occ[static_cast<size_t>(d_.occ[i].tok)] = static_cast<int>(i);
    }

private:
    OctaveLspDocument &d_;

    const AstNode &N(int i) const { return d_.tree.nodes[static_cast<size_t>(i)]; }
    const Token &T(int i) const { return d_.tree.tokens[static_cast<size_t>(i)]; }

    int NewScope(int parent, int fn, bool anon, bool nested) {
        Scope s;
        s.parent = parent;
        s.fn = fn;
        s.anon = anon;
        s.nested = nested;
        d_.scopes.push_back(s);
        return static_cast<int>(d_.scopes.size()) - 1;
    }
    Scope &S(int s) { return d_.scopes[static_cast<size_t>(s)]; }

    void Add(int tok, int scope, bool write, bool read) {
        if (tok < 0) return;
        Occurrence o;
        o.tok = tok;
        o.scope = scope;
        o.write = write;
        o.read = read;
        if (write) S(scope).vars.insert(T(tok).text);
        d_.occ.push_back(o);
    }

    // An assignment target: names written; indices and a subscripted base are read too.
    void Lhs(int n, int scope, bool loop) {
        if (n < 0) return;
        const AstNode &a = N(n);
        switch (a.kind) {
        case Node::Identifier:
            Add(a.tok, scope, true, false);
            if (loop) S(scope).loop_vars.insert(T(a.tok).text);
            return;
        case Node::Paren:
            if (!a.kids.empty()) Lhs(a.kids[0], scope, loop);
            return;
        case Node::Matrix:
        case Node::Row:
            for (int k : a.kids) Lhs(k, scope, loop);
            return;
        case Node::Index:
        case Node::Field:
        case Node::DynField: {
            int base = n;
            while (N(base).kind == Node::Index || N(base).kind == Node::Field || N(base).kind == Node::DynField ||
                   N(base).kind == Node::Paren) {
                const AstNode &b = N(base);
                for (size_t k = 1; k < b.kids.size(); k++) Walk(b.kids[k], scope, -1);
                if (b.kids.empty()) return;
                base = b.kids[0];
            }
            if (N(base).kind == Node::Identifier) {
                Add(N(base).tok, scope, true, true);  // x(i) = v keeps the rest of x
            } else {
                Walk(base, scope, -1);
            }
            return;
        }
        default:
            Walk(n, scope, -1);
        }
    }

    void Params(int list, int scope, std::set<std::string> *into) {
        if (list < 0) return;
        for (int k : N(list).kids) {
            const AstNode &a = N(k);
            int id = k;
            if (a.kind == Node::DeclElt) {
                id = a.name;
                for (int e : a.kids) Walk(e, scope, -1);
            }
            if (id >= 0 && N(id).kind == Node::Identifier) {
                Add(N(id).tok, scope, true, false);
                if (into) into->insert(T(N(id).tok).text);
            }
        }
    }

    // A statement list; the first statement after a return/break/continue
    // in the same list is unreachable (reported once per list).
    void Body(int list, int scope) {
        if (list < 0) return;
        bool dead = false, reported = false;
        for (int k : N(list).kids) {
            if (dead && !reported && N(k).kind != Node::Function) {
                d_.unreachable.push_back(k);
                reported = true;
            }
            Walk(k, scope, -1);
            if (N(k).kind == Node::Jump) dead = true;
        }
    }

    void Function(int n, int scope, int parent_fn_info, bool parent_has_end) {
        const AstNode &a = N(n);
        const bool nested = parent_fn_info >= 0 && parent_has_end && scope > 0 && !S(scope).anon && S(scope).fn >= 0;
        const int fs = NewScope(nested ? scope : -1, n, false, nested);
        d_.fn_scope[n] = fs;
        FunctionInfo fi;
        fi.node = n;
        if (a.name >= 0 && N(a.name).tok >= 0) {
            fi.name_tok = N(a.name).tok;
            fi.name = T(fi.name_tok).text;
            const size_t dot = fi.name.find('.');
            if (dot != std::string::npos) fi.name = fi.name.substr(dot + 1);
        }
        fi.parent = parent_fn_info;
        fi.method = in_classdef_;
        d_.functions.push_back(fi);
        const int me = static_cast<int>(d_.functions.size()) - 1;
        if (a.kids.size() > 0) Params(a.kids[0], fs, &S(fs).params);
        if (a.kids.size() > 1) Params(a.kids[1], fs, &S(fs).outputs);
        if (a.kids.size() > 2) {
            const int saved = cur_fn_;
            const bool saved_end = cur_fn_end_;
            cur_fn_ = me;
            cur_fn_end_ = a.explicit_end;
            const bool saved_cd = in_classdef_;
            in_classdef_ = false;
            Body(a.kids[2], fs);
            in_classdef_ = saved_cd;
            cur_fn_ = saved;
            cur_fn_end_ = saved_end;
        }
    }
    int cur_fn_ = -1;
    bool cur_fn_end_ = false;
    bool in_classdef_ = false;

    void Walk(int n, int scope, int) {
        if (n < 0) return;
        const AstNode &a = N(n);
        switch (a.kind) {
        case Node::StatementList:
            Body(n, scope);
            return;
        case Node::Function:
            Function(n, scope, cur_fn_, cur_fn_end_);
            return;
        case Node::Classdef: {
            const bool saved = in_classdef_;
            in_classdef_ = true;
            for (int k : a.kids) Walk(k, scope, -1);
            in_classdef_ = saved;
            return;
        }
        case Node::ClassBlock:
            for (int k : a.kids) {
                if (N(k).kind == Node::Function) {
                    in_classdef_ = true;
                    Function(k, scope, -1, false);
                } else if (N(k).kind == Node::Property || N(k).kind == Node::EnumItem) {
                    for (size_t j = 1; j < N(k).kids.size(); j++) Walk(N(k).kids[j], scope, -1);
                }
            }
            return;
        case Node::Property:
        case Node::Attr:
        case Node::Superclass:
        case Node::MethodDecl:
        case Node::EnumItem:
            return;
        case Node::Assign:
            if (a.kids.size() > 1) Walk(a.kids[1], scope, -1);
            if (!a.kids.empty()) {
                const bool op_assign = a.op >= 0 && T(a.op).text != "=";
                if (op_assign && N(a.kids[0]).kind == Node::Identifier) {
                    Add(N(a.kids[0]).tok, scope, true, true);
                } else {
                    Lhs(a.kids[0], scope, false);
                }
            }
            return;
        case Node::For:
            if (a.kids.size() > 1) Walk(a.kids[1], scope, -1);
            if (a.kids.size() > 3) Walk(a.kids[3], scope, -1);
            if (!a.kids.empty()) Lhs(a.kids[0], scope, true);
            if (a.kids.size() > 2) Walk(a.kids[2], scope, -1);
            return;
        case Node::Catch:
            if (a.name >= 0 && N(a.name).tok >= 0) Add(N(a.name).tok, scope, true, false);
            for (int k : a.kids) Walk(k, scope, -1);
            return;
        case Node::Decl: {
            for (int k : a.kids) {
                const AstNode &e = N(k);
                if (e.name >= 0 && N(e.name).tok >= 0) {
                    Add(N(e.name).tok, scope, true, true);
                    S(scope).globals.insert(T(N(e.name).tok).text);
                }
                for (int x : e.kids) Walk(x, scope, -1);
            }
            return;
        }
        case Node::Switch: {
            std::map<std::string, int> seen;
            for (size_t i = 0; i < a.kids.size(); i++) {
                const int k = a.kids[i];
                if (i > 0 && N(k).kind == Node::Case && !N(k).kids.empty()) {
                    std::vector<int> labels;
                    const int lab = N(k).kids[0];
                    if (N(lab).kind == Node::Cell) {
                        for (int r : N(lab).kids)
                            for (int e : N(r).kids) labels.push_back(e);
                    } else {
                        labels.push_back(lab);
                    }
                    for (int e : labels) {
                        const AstNode &l = N(e);
                        if ((l.kind != Node::Number && l.kind != Node::String) || l.tok < 0) continue;
                        const std::string key = (l.kind == Node::Number ? "n:" : "s:") + T(l.tok).text;
                        const auto it = seen.find(key);
                        if (it != seen.end()) {
                            d_.duplicate_cases.emplace_back(e, it->second);
                        } else {
                            seen[key] = e;
                        }
                    }
                }
                Walk(k, scope, -1);
            }
            return;
        }
        case Node::Command: {
            if (a.tok >= 0) {
                Occurrence o;
                o.tok = a.tok;
                o.scope = scope;
                o.read = true;
                o.command = true;
                d_.occ.push_back(o);
                if (IsDynamicCall(T(a.tok).text)) MarkDynamic(scope);
            }
            return;
        }
        case Node::Identifier:
            if (a.tok >= 0) {
                Add(a.tok, scope, false, true);
                if (IsDynamicCall(T(a.tok).text)) MarkDynamic(scope);
            }
            return;
        case Node::FcnHandle:
            if (a.tok >= 0) {
                Occurrence o;
                o.tok = a.tok;
                o.scope = scope;
                o.read = true;
                o.handle = true;
                d_.occ.push_back(o);
            }
            return;
        case Node::AnonFcn: {
            const int as = NewScope(scope, -1, true, false);
            if (!a.kids.empty()) Params(a.kids[0], as, &S(as).params);
            for (size_t k = 1; k < a.kids.size(); k++) Walk(a.kids[k], as, -1);
            return;
        }
        case Node::Field:
            if (!a.kids.empty()) Walk(a.kids[0], scope, -1);
            return;
        default:
            for (int k : a.kids) Walk(k, scope, -1);
        }
    }

    void MarkDynamic(int scope) {
        for (int s = scope; s >= 0; s = S(s).parent) {
            S(s).dynamic = true;
            if (!S(s).anon && !S(s).nested) break;
        }
    }
};

// Re-read a broken text: blank the line of the error, or close what is open.
std::string Repair(const std::string &text, const ParseResult &r) {
    std::string t = text;
    int line = r.error_line - 1;
    if (r.error_tok >= 0 && static_cast<size_t>(r.error_tok) < r.tokens.size()) {
        const Token &tk = r.tokens[static_cast<size_t>(r.error_tok)];
        if (tk.kind == Tok::Eof) {
            // Ran out inside a bracket: no number of `end`s helps, so blank
            // the line that opened it.
            std::vector<int> open;
            for (const Token &b : r.tokens) {
                if (b.kind != Tok::Op || b.synthetic) continue;
                if (b.text == "(" || b.text == "[" || b.text == "{") open.push_back(b.line);
                else if ((b.text == ")" || b.text == "]" || b.text == "}") && !open.empty()) open.pop_back();
            }
            if (open.empty()) {
                t += "\nend\n";
                return t;
            }
            line = open.back();
            int cur = 0;
            size_t i = 0;
            while (i < t.size() && cur < line) {
                if (t[i] == '\n') cur++;
                i++;
            }
            for (; i < t.size() && t[i] != '\n'; i++)
                if (t[i] != ' ' && t[i] != '\t' && t[i] != '\r') t[i] = ' ';
            return t;
        }
        if (!tk.synthetic) line = tk.line;
    }
    int cur = 0;
    size_t i = 0;
    while (i < t.size() && cur < line) {
        if (t[i] == '\n') cur++;
        i++;
    }
    if (i >= t.size()) {
        t += "\nend\n";
        return t;
    }
    bool changed = false;
    for (; i < t.size() && t[i] != '\n'; i++)
        if (t[i] != ' ' && t[i] != '\t' && t[i] != '\r') {
            t[i] = ' ';
            changed = true;
        }
    if (!changed) t += "\nend\n";
    return t;
}

}  // namespace

std::shared_ptr<const OctaveLspDocument> OctaveLspAnalyze(const std::vector<std::string> &lines,
                                                          const std::string &path) {
    auto doc = std::make_shared<OctaveLspDocument>();
    doc->lines = lines;
    for (size_t i = 0; i < lines.size(); i++) {
        doc->line_start.push_back(static_cast<int>(doc->text.size()));
        if (i) {
            doc->text += '\n';
            doc->line_start.back() = static_cast<int>(doc->text.size());
        }
        doc->text += lines[i];
    }
    if (doc->line_start.empty()) doc->line_start.push_back(0);
    octparse::Options opt;
    opt.file_base = Basename(path);
    doc->parse = octparse::Parse(doc->text, opt);
    doc->ptext = doc->text;
    doc->tree = doc->parse;
    for (int attempt = 0; attempt < 12 && !doc->tree.ok(); attempt++) {
        doc->ptext = Repair(doc->ptext, doc->tree);
        doc->tree = octparse::Parse(doc->ptext, opt);
    }
    if (!doc->tree.ok()) {  // nothing worked: the features fall back to the tokens read
        doc->ptext = doc->text;
        doc->tree = doc->parse;
    }
    doc->tree.src = doc->ptext;
    doc->parse.src = doc->text;
    Analyzer(*doc).Run();
    return doc;
}

std::string OctaveLspOctaveCheck(const std::string &text, const std::string &path) {
    octparse::Options opt;
    opt.file_base = Basename(path);
    return octparse::CheckMessage(octparse::Parse(text, opt));
}

// --- Diagnostics ------------------------------------------------------------------

std::vector<OctaveLspDiagnostic> OctaveLspDiagnostics(const OctaveLspDocument &doc) {
    std::vector<OctaveLspDiagnostic> out;
    const ParseResult &p = doc.parse;
    if (!p.ok()) {
        OctaveLspDiagnostic d;
        d.code = "syntax-error";
        d.message = p.error;
        const Token *t = nullptr;
        if (p.error_tok >= 0 && static_cast<size_t>(p.error_tok) < p.tokens.size())
            t = &p.tokens[static_cast<size_t>(p.error_tok)];
        if (t && !t->synthetic && t->kind != Tok::Eof && t->end > t->begin) {
            d.range = doc.TokRange(p, p.error_tok);
        } else {
            const int last = static_cast<int>(doc.lines.size()) - 1;
            d.range.line = std::max(0, std::min(p.error_line - 1, last));
            const int len = static_cast<int>(doc.lines[static_cast<size_t>(d.range.line)].size());
            d.range.col = std::max(0, std::min(p.error_col - 1, len));
            d.range.end_line = d.range.line;
            d.range.end_col = std::min(len, d.range.col + 1);
            if (d.range.end_col <= d.range.col) d.range.col = std::max(0, len - 1), d.range.end_col = len;
        }
        out.push_back(d);
        return out;  // the rest is computed on a repaired tree: do not report it
    }
    // unused-local: assigned in a function, never read. Hints (tagged
    // unnecessary), and only where the function cannot reach its variables
    // by other means (eval, load, assignin, inputname, ...).
    std::map<std::pair<int, std::string>, int> first_write;
    std::set<std::pair<int, std::string>> read;
    for (const Occurrence &o : doc.occ) {
        const std::string &nm = doc.T(o.tok).text;
        const int owner = doc.Resolve(nm, o.scope);
        if (owner < 0) continue;
        const auto key = std::make_pair(owner, nm);
        if (o.read) read.insert(key);
        if (o.write && !first_write.count(key)) first_write[key] = o.tok;
    }
    for (const auto &kv : first_write) {
        const Scope &s = doc.scopes[static_cast<size_t>(kv.first.first)];
        const std::string &nm = kv.first.second;
        if (s.fn < 0 || s.anon || read.count(kv.first)) continue;
        bool dynamic = false;
        for (const Scope &other : doc.scopes)
            if (other.dynamic) {
                // a dynamic nested/anon scope can read its parent's variables
                for (int x = static_cast<int>(&other - &doc.scopes[0]); x >= 0;
                     x = doc.scopes[static_cast<size_t>(x)].parent)
                    if (x == kv.first.first) dynamic = true;
            }
        if (dynamic) continue;
        if (s.params.count(nm) || s.outputs.count(nm) || s.globals.count(nm) || s.loop_vars.count(nm)) continue;
        if (nm == "ans" || nm == "varargout" || nm == "varargin") continue;
        OctaveLspDiagnostic d;
        d.range = doc.TokRange(kv.second);
        d.severity = OctaveLspSeverity::Hint;
        d.code = "unused-local";
        d.message = "'" + nm + "' is assigned but never used";
        d.unnecessary = true;
        out.push_back(d);
    }
    for (int n : doc.unreachable) {
        OctaveLspDiagnostic d;
        if (!doc.NodeRange(n, &d.range)) continue;
        d.severity = OctaveLspSeverity::Hint;
        d.code = "unreachable-code";
        d.message = "unreachable code";
        d.unnecessary = true;
        out.push_back(d);
    }
    for (const auto &dc : doc.duplicate_cases) {
        OctaveLspDiagnostic d;
        if (!doc.NodeRange(dc.first, &d.range)) continue;
        OctaveLspRange first;
        doc.NodeRange(dc.second, &first);
        d.severity = OctaveLspSeverity::Warning;
        d.code = "duplicate-case";
        d.message = "this case label repeats the one on line " + std::to_string(first.line + 1) +
                    "; only the first is ever taken";
        out.push_back(d);
    }
    // `%#ok` (MATLAB's code-analyzer pragma) on the line silences the lints there.
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const OctaveLspDiagnostic &d) {
                                 const size_t ln = static_cast<size_t>(d.range.line);
                                 return ln < doc.lines.size() && doc.lines[ln].find("%#ok") != std::string::npos;
                             }),
              out.end());
    std::sort(out.begin(), out.end(), [](const OctaveLspDiagnostic &a, const OctaveLspDiagnostic &b) {
        return a.range.line != b.range.line ? a.range.line < b.range.line : a.range.col < b.range.col;
    });
    return out;
}

std::string OctaveLspFormatDiagnostic(const std::string &path, const OctaveLspDiagnostic &d) {
    const char *sev = d.severity == OctaveLspSeverity::Error         ? "error"
                      : d.severity == OctaveLspSeverity::Warning     ? "warning"
                      : d.severity == OctaveLspSeverity::Information ? "info"
                                                                     : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool OctaveLspDiagnosticFails(const OctaveLspDiagnostic &d) {
    return d.severity == OctaveLspSeverity::Error || d.severity == OctaveLspSeverity::Warning;
}

// --- Completion / hover / signature help -------------------------------------------

namespace {

// The scope the position is in: the innermost function whose range holds it.
int ScopeAt(const OctaveLspDocument &doc, int line, int col) {
    int best = 0;
    int best_size = 1 << 30;
    for (size_t i = 1; i < doc.scopes.size(); i++) {
        const Scope &s = doc.scopes[i];
        if (s.fn < 0) continue;
        OctaveLspRange g;
        if (!doc.NodeRange(s.fn, &g)) continue;
        const bool after_start = line > g.line || (line == g.line && col >= g.col);
        const bool before_end = line < g.end_line || (line == g.end_line && col <= g.end_col);
        if (!after_start || !before_end) continue;
        const int size = (g.end_line - g.line) * 4096 + (g.end_col - g.col);
        if (size < best_size) best = static_cast<int>(i), best_size = size;
    }
    return best;
}

std::string FunctionSignature(const OctaveLspDocument &doc, const FunctionInfo &f) {
    const AstNode &a = doc.N(f.node);
    auto names = [&](int list) {
        std::vector<std::string> v;
        if (list < 0) return v;
        for (int k : doc.N(list).kids) {
            const AstNode &e = doc.N(k);
            if (e.kind == Node::Tilde) {
                v.push_back("~");
                continue;
            }
            const int id = e.kind == Node::DeclElt ? e.name : k;
            if (id >= 0 && doc.N(id).tok >= 0) v.push_back(doc.T(doc.N(id).tok).text);
        }
        return v;
    };
    const std::vector<std::string> ins = a.kids.size() > 0 ? names(a.kids[0]) : std::vector<std::string>();
    std::vector<std::string> outs = a.kids.size() > 1 ? names(a.kids[1]) : std::vector<std::string>();
    std::string s;
    if (outs.size() == 1) {
        s = outs[0] + " = ";
    } else if (outs.size() > 1) {
        s = "[";
        for (size_t i = 0; i < outs.size(); i++) s += (i ? ", " : "") + outs[i];
        s += "] = ";
    }
    s += f.name + " (";
    for (size_t i = 0; i < ins.size(); i++) s += (i ? ", " : "") + ins[i];
    s += ")";
    return s;
}

std::vector<std::string> SplitUsage(const char *u) {
    std::vector<std::string> v;
    std::string cur;
    for (const char *p = u; *p; p++) {
        if (*p == '\n') {
            v.push_back(cur);
            cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

bool IsKeywordWord(const std::string &w) {
    for (const char *k : kKeywords)
        if (w == k) return true;
    return false;
}

}  // namespace

std::vector<OctaveLspCompletionItem> OctaveLspCompletions(const OctaveLspDocument &doc, int line, int col) {
    std::vector<OctaveLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return out;
    const std::string &cur = doc.lines[static_cast<size_t>(line)];
    col = std::max(0, std::min(col, static_cast<int>(cur.size())));
    int start = col;
    while (start > 0 && IsWordChar(cur[static_cast<size_t>(start - 1)])) start--;
    const std::string prefix = cur.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    // Inside a comment or a string: nothing.
    {
        bool in_sq = false, in_dq = false;
        for (int i = 0; i < start; i++) {
            const char c = cur[static_cast<size_t>(i)];
            if (in_dq) {
                if (c == '\\') i++;
                else if (c == '"') in_dq = false;
                continue;
            }
            if (in_sq) {
                if (c == '\'') in_sq = false;
                continue;
            }
            if (c == '%' || c == '#') return out;
            if (c == '"') in_dq = true;
            if (c == '\'' && (i == 0 || !(IsWordChar(cur[static_cast<size_t>(i - 1)]) ||
                                          std::strchr(")]}.'", cur[static_cast<size_t>(i - 1)]))))
                in_sq = true;
        }
        if (in_sq || in_dq) return out;
    }
    std::set<std::string> seen;
    auto add = [&](const std::string &label, OctaveLspKind kind, const std::string &detail) {
        if (label.compare(0, prefix.size(), prefix) != 0 || !seen.insert(label).second) return;
        OctaveLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.replace_start = start;
        it.replace_end = col;
        out.push_back(it);
    };
    const bool after_dot = start > 0 && cur[static_cast<size_t>(start - 1)] == '.';
    if (after_dot) {  // a field: every field name the document uses
        for (size_t i = 0; i < doc.tree.tokens.size(); i++) {
            const Token &t = doc.tree.tokens[i];
            if (t.kind != Tok::StructElt) continue;
            if (t.line == line && t.col == start) continue;
            add(t.text, OctaveLspKind::Field, "field");
        }
        return out;
    }
    const int scope = ScopeAt(doc, line, col);
    for (const Occurrence &o : doc.occ) {
        if (!o.write) continue;
        const Token &t = doc.T(o.tok);
        if (t.line == line && t.col == start) continue;
        int s = o.scope;
        bool visible = false;
        for (int x = scope; x >= 0; x = doc.scopes[static_cast<size_t>(x)].parent) {
            if (x == s) visible = true;
            const Scope &sc = doc.scopes[static_cast<size_t>(x)];
            if (!sc.anon && !sc.nested) break;
        }
        if (visible) add(t.text, OctaveLspKind::Variable, "variable");
    }
    for (const FunctionInfo &f : doc.functions)
        if (!f.name.empty()) add(f.name, OctaveLspKind::Function, FunctionSignature(doc, f));
    for (const char *k : kKeywords) add(k, OctaveLspKind::Keyword, "keyword");
    if (!prefix.empty()) {
        size_t n = 0;
        const OctaveVocabEntry *v = OctaveVocab(&n);
        for (size_t i = 0; i < n; i++) {
            if (v[i].name[0] == '_' && prefix[0] != '_') continue;
            const std::vector<std::string> us = SplitUsage(v[i].usage);
            add(v[i].name, OctaveLspKind::Function, us.empty() ? std::string("Octave function") : us[0]);
        }
    }
    return out;
}

OctaveLspHoverInfo OctaveLspHover(const OctaveLspDocument &doc, int line, int col) {
    OctaveLspHoverInfo h;
    const int tok = doc.TokenAt(line, col);
    if (tok < 0) return h;
    const Token &t = doc.T(tok);
    h.range = doc.TokRange(tok);
    if (t.kind == Tok::StructElt) {
        h.found = true;
        h.text = "field `" + t.text + "`";
        return h;
    }
    const std::string &nm = t.text;
    const int oi = doc.tok_occ[static_cast<size_t>(tok)];
    if (oi >= 0 && t.kind == Tok::Name) {
        const Occurrence &o = doc.occ[static_cast<size_t>(oi)];
        const int owner = doc.Resolve(nm, o.scope);
        if (owner >= 0 && !o.command) {
            const Scope &s = doc.scopes[static_cast<size_t>(owner)];
            std::string what = "variable";
            if (s.params.count(nm)) what = "parameter";
            else if (s.outputs.count(nm)) what = "output";
            else if (s.globals.count(nm)) what = "global or persistent variable";
            h.found = true;
            h.text = "(" + what + ") `" + nm + "`";
            return h;
        }
    }
    if (const FunctionInfo *f = doc.FindFunction(nm)) {
        h.found = true;
        h.text = "```octave\nfunction " + FunctionSignature(doc, *f) + "\n```\n" +
                 (f->method ? "method in this file" : "function in this file");
        return h;
    }
    if (const OctaveVocabEntry *v = OctaveVocabFind(nm.c_str(), nm.size())) {
        h.found = true;
        std::string body;
        for (const std::string &u : SplitUsage(v->usage)) body += u + "\n";
        h.text = (body.empty() ? std::string() : "```octave\n" + body + "```\n") +
                 (v->builtin ? "Octave built-in function" : "Octave function") + " `" + nm + "` -- `help " + nm +
                 "` for the documentation";
        return h;
    }
    return h;
}

OctaveLspSignatureHelp OctaveLspSignatureAt(const OctaveLspDocument &doc, int line, int col) {
    OctaveLspSignatureHelp sh;
    // The innermost open '(' before the position, on tokens of the tree.
    const auto &toks = doc.tree.tokens;
    std::vector<std::pair<int, int>> stack;  // (open token, commas)
    for (size_t i = 0; i < toks.size(); i++) {
        const Token &t = toks[i];
        if (t.line > line || (t.line == line && t.col >= col)) break;
        if (t.synthetic) continue;
        if (t.kind == Tok::Op && (t.text == "(" || t.text == "[" || t.text == "{")) {
            stack.emplace_back(static_cast<int>(i), 0);
        } else if (t.kind == Tok::Op && (t.text == ")" || t.text == "]" || t.text == "}")) {
            if (!stack.empty()) stack.pop_back();
        } else if (t.kind == Tok::Comma && !stack.empty()) {
            stack.back().second++;
        } else if (t.kind == Tok::Newline || t.kind == Tok::Semi) {
            if (t.kind == Tok::Newline) stack.clear();
        }
    }
    while (!stack.empty() && toks[static_cast<size_t>(stack.back().first)].text != "(") stack.pop_back();
    if (stack.empty() || stack.back().first == 0) return sh;
    const Token &callee = toks[static_cast<size_t>(stack.back().first - 1)];
    if (callee.kind != Tok::Name) return sh;
    const std::string &nm = callee.text;
    std::string label;
    if (const FunctionInfo *f = doc.FindFunction(nm)) {
        label = FunctionSignature(doc, *f);
    } else if (const OctaveVocabEntry *v = OctaveVocabFind(nm.c_str(), nm.size())) {
        // The first usage line with enough parameters for the commas typed.
        const std::vector<std::string> us = SplitUsage(v->usage);
        for (const std::string &u : us) {
            const size_t lp = u.find('(');
            if (lp == std::string::npos) continue;
            const int commas = static_cast<int>(std::count(u.begin() + static_cast<long>(lp), u.end(), ','));
            if (label.empty()) label = u;
            if (commas >= stack.back().second || u.find("...") != std::string::npos) {
                label = u;
                break;
            }
        }
    }
    if (label.empty()) return sh;
    const size_t lp = label.find('(');
    const size_t rp = label.rfind(')');
    if (lp == std::string::npos || rp == std::string::npos || rp < lp) return sh;
    sh.found = true;
    sh.label = label;
    std::string cur;
    for (size_t i = lp + 1; i <= rp; i++) {
        if (i == rp || label[i] == ',') {
            size_t b = cur.find_first_not_of(' ');
            size_t e = cur.find_last_not_of(' ');
            if (b != std::string::npos) sh.params.push_back(cur.substr(b, e - b + 1));
            cur.clear();
        } else {
            cur += label[i];
        }
    }
    sh.active_param = stack.back().second;
    // Past the last parameter: stay on it (it is "..." when more are allowed).
    if (!sh.params.empty() && sh.active_param >= static_cast<int>(sh.params.size()))
        sh.active_param = static_cast<int>(sh.params.size()) - 1;
    return sh;
}

// --- Navigation -------------------------------------------------------------------------

namespace {

// Every token naming the same thing as `tok`: a variable's occurrences in
// its owning scope (and the scopes sharing it), or a file function's name
// and its calls, handles and commands wherever no variable shadows it.
std::vector<int> SameThing(const OctaveLspDocument &doc, int tok, bool *is_var, int *def_tok) {
    std::vector<int> out;
    *is_var = false;
    *def_tok = -1;
    if (tok < 0) return out;
    const Token &t = doc.T(tok);
    if (t.kind != Tok::Name && t.kind != Tok::FcnHandle) return out;
    const std::string &nm = t.text;
    const int oi = doc.tok_occ[static_cast<size_t>(tok)];
    int owner = -1;
    if (oi >= 0 && t.kind == Tok::Name && !doc.occ[static_cast<size_t>(oi)].command)
        owner = doc.Resolve(nm, doc.occ[static_cast<size_t>(oi)].scope);
    if (owner >= 0) {
        *is_var = true;
        for (const Occurrence &o : doc.occ) {
            if (o.command || o.handle || doc.T(o.tok).text != nm) continue;
            if (doc.Resolve(nm, o.scope) != owner) continue;
            out.push_back(o.tok);
            if (o.write && *def_tok < 0) *def_tok = o.tok;
        }
        return out;
    }
    const FunctionInfo *f = doc.FindFunction(nm);
    if (!f) {
        // Is this the function's own name token?
        for (const FunctionInfo &g : doc.functions)
            if (g.name_tok == tok) f = &g;
        if (!f) return out;
    }
    *def_tok = f->name_tok;
    if (f->name_tok >= 0) out.push_back(f->name_tok);
    for (const Occurrence &o : doc.occ) {
        if (doc.T(o.tok).text != nm) continue;
        if (!o.command && !o.handle && doc.Resolve(nm, o.scope) >= 0) continue;
        out.push_back(o.tok);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

OctaveLspRange NameRange(const OctaveLspDocument &doc, int tok) {
    OctaveLspRange g = doc.TokRange(tok);
    const Token &t = doc.T(tok);
    if (t.kind == Tok::FcnHandle) g.col += 1;  // past the '@'
    if (t.kind == Tok::Name && t.end - t.begin > static_cast<int>(t.text.size())) {
        g.end_col = g.col + static_cast<int>(t.text.size());
    }
    // A get.Name / set.Name method's name token spells only "get"/"set".
    return g;
}

}  // namespace

std::vector<OctaveLspLocation> OctaveLspDefinition(const OctaveLspDocument &doc, int line, int col) {
    std::vector<OctaveLspLocation> out;
    bool is_var = false;
    int def = -1;
    SameThing(doc, doc.TokenAt(line, col), &is_var, &def);
    if (def >= 0) {
        OctaveLspLocation l;
        l.range = NameRange(doc, def);
        out.push_back(l);
    }
    return out;
}

std::vector<OctaveLspReference> OctaveLspReferences(const OctaveLspDocument &doc, int line, int col) {
    std::vector<OctaveLspReference> out;
    bool is_var = false;
    int def = -1;
    for (int tok : SameThing(doc, doc.TokenAt(line, col), &is_var, &def)) {
        OctaveLspReference r;
        r.range = NameRange(doc, tok);
        const int oi = doc.tok_occ[static_cast<size_t>(tok)];
        r.is_definition = tok == def || (is_var && oi >= 0 && doc.occ[static_cast<size_t>(oi)].write);
        out.push_back(r);
    }
    return out;
}

OctaveLspRenameTarget OctaveLspPrepareRename(const OctaveLspDocument &doc, int line, int col) {
    OctaveLspRenameTarget r;
    const int tok = doc.TokenAt(line, col);
    bool is_var = false;
    int def = -1;
    if (SameThing(doc, tok, &is_var, &def).empty()) return r;
    r.found = true;
    r.range = NameRange(doc, tok);
    r.placeholder = doc.T(tok).text;
    return r;
}

std::vector<OctaveLspTextEdit> OctaveLspRename(const OctaveLspDocument &doc, int line, int col,
                                               const std::string &new_name, std::string *error) {
    std::vector<OctaveLspTextEdit> out;
    if (!IsIdentifier(new_name) || IsKeywordWord(new_name) || octparse::IsKeyword(new_name)) {
        *error = "'" + new_name + "' is not a valid Octave name";
        return out;
    }
    bool is_var = false;
    int def = -1;
    const std::vector<int> toks = SameThing(doc, doc.TokenAt(line, col), &is_var, &def);
    if (toks.empty()) {
        *error = "nothing to rename here";
        return out;
    }
    if (!doc.parse.ok()) {
        *error = "the file has a syntax error; fix it before renaming";
        return out;
    }
    for (int tok : toks) {
        OctaveLspTextEdit e;
        e.range = NameRange(doc, tok);
        e.new_text = new_name;
        out.push_back(e);
    }
    return out;
}

// --- Outline / folding --------------------------------------------------------------------

std::vector<OctaveLspSymbol> OctaveLspSymbols(const OctaveLspDocument &doc) {
    std::vector<OctaveLspSymbol> out;
    if (doc.tree.root < 0) return out;
    // Walk the tree: classdef, its blocks, functions (with nesting).
    struct Walker {
        const OctaveLspDocument &d;
        std::vector<OctaveLspSymbol> &out;
        void Visit(int n, int parent) {
            if (n < 0) return;
            const AstNode &a = d.N(n);
            if (a.kind == Node::Function) {
                OctaveLspSymbol s;
                if (!d.NodeRange(n, &s.range)) return;
                if (a.name >= 0 && d.N(a.name).tok >= 0) {
                    const int nt = d.N(a.name).tok;
                    s.name = d.T(nt).text;
                    s.sel_range = NameRange(d, nt);
                    if ((s.name == "get" || s.name == "set") && d.N(a.name).first >= 0) {
                        // get.Prop: the identifier node spans `get . Prop`
                        OctaveLspRange g;
                        if (d.NodeRange(a.name, &g)) s.sel_range = g;
                        s.name = s.name + "." + d.T(d.N(a.name).tok).text;
                    }
                } else {
                    s.name = "(function)";
                    s.sel_range = s.range;
                }
                for (const FunctionInfo &f : d.functions)
                    if (f.node == n) s.detail = "function " + FunctionSignature(d, f);
                s.kind = parent >= 0 && out[static_cast<size_t>(parent)].kind == OctaveLspSymbolKind::Class
                             ? OctaveLspSymbolKind::Method
                             : OctaveLspSymbolKind::Function;
                s.parent = parent;
                out.push_back(s);
                const int me = static_cast<int>(out.size()) - 1;
                if (a.kids.size() > 2) Visit(a.kids[2], me);
                return;
            }
            if (a.kind == Node::Classdef) {
                OctaveLspSymbol s;
                if (!d.NodeRange(n, &s.range)) return;
                s.kind = OctaveLspSymbolKind::Class;
                if (a.name >= 0 && d.N(a.name).tok >= 0) {
                    s.name = d.T(d.N(a.name).tok).text;
                    s.sel_range = NameRange(d, d.N(a.name).tok);
                } else {
                    s.name = "(classdef)";
                    s.sel_range = s.range;
                }
                s.detail = "classdef";
                s.parent = parent;
                out.push_back(s);
                const int me = static_cast<int>(out.size()) - 1;
                for (int k : a.kids) Visit(k, me);
                return;
            }
            if (a.kind == Node::ClassBlock) {
                const std::string kw = a.tok >= 0 ? d.T(a.tok).text : "";
                for (int k : a.kids) {
                    const AstNode &e = d.N(k);
                    if (e.kind == Node::Function) {
                        Visit(k, parent);
                    } else if ((e.kind == Node::Property || e.kind == Node::EnumItem) && !e.kids.empty() &&
                               d.N(e.kids[0]).tok >= 0) {
                        OctaveLspSymbol s;
                        const int nt = d.N(e.kids[0]).tok;
                        s.name = d.T(nt).text;
                        s.sel_range = NameRange(d, nt);
                        if (!d.NodeRange(k, &s.range)) s.range = s.sel_range;
                        s.kind = kw == "events"        ? OctaveLspSymbolKind::Key
                                 : kw == "enumeration" ? OctaveLspSymbolKind::Constant
                                                       : OctaveLspSymbolKind::Property;
                        s.detail = kw;
                        s.parent = parent;
                        out.push_back(s);
                    }
                }
                return;
            }
            for (int k : a.kids) Visit(k, parent);
        }
    } w{doc, out};
    w.Visit(doc.tree.root, -1);
    return out;
}

std::vector<OctaveLspFold> OctaveLspFolds(const OctaveLspDocument &doc) {
    std::vector<OctaveLspFold> out;
    for (size_t i = 0; i < doc.tree.nodes.size(); i++) {
        const AstNode &a = doc.tree.nodes[i];
        switch (a.kind) {
        case Node::Function:
        case Node::Classdef:
        case Node::ClassBlock:
        case Node::If:
        case Node::Switch:
        case Node::While:
        case Node::DoUntil:
        case Node::For:
        case Node::Try:
        case Node::Unwind:
        case Node::Spmd:
        case Node::ArgumentsBlock: {
            OctaveLspRange g;
            if (!doc.NodeRange(static_cast<int>(i), &g)) break;
            if (g.end_line > g.line) {
                OctaveLspFold f;
                f.start_line = g.line;
                f.end_line = g.end_line;
                out.push_back(f);
            }
            break;
        }
        default:
            break;
        }
    }
    // Comments: a block comment, or a run of two or more full-line comments.
    const auto &cs = doc.tree.comments;
    for (size_t i = 0; i < cs.size(); i++) {
        if (cs[i].block) {
            if (cs[i].end_line > cs[i].line) out.push_back({cs[i].line, cs[i].end_line, "comment"});
            continue;
        }
        if (!cs[i].full_line) continue;
        size_t j = i;
        while (j + 1 < cs.size() && !cs[j + 1].block && cs[j + 1].full_line && cs[j + 1].line == cs[j].line + 1) j++;
        if (cs[j].line > cs[i].line) out.push_back({cs[i].line, cs[j].line, "comment"});
        i = j;
    }
    std::sort(out.begin(), out.end(), [](const OctaveLspFold &a, const OctaveLspFold &b) {
        return a.start_line != b.start_line ? a.start_line < b.start_line : a.end_line > b.end_line;
    });
    return out;
}

// --- Formatting ------------------------------------------------------------------------------

std::vector<OctaveLspTextEdit> OctaveLspFormat(const std::vector<std::string> &lines, int width, std::string *error) {
    std::vector<OctaveLspTextEdit> edits;
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    octavefmt::Options o;
    if (width > 0) o.width = width;
    const octavefmt::Result r = octavefmt::Format(text, o);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    std::string want = r.text;
    if (!want.empty() && want.back() == '\n' && (text.empty() || text.back() != '\n')) want.pop_back();
    if (want == text) return edits;
    // One edit over the changed lines only.
    std::vector<std::string> nl;
    {
        std::string cur;
        for (char c : want) {
            if (c == '\n') {
                nl.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        nl.push_back(cur);
    }
    size_t top = 0;
    while (top < lines.size() && top < nl.size() && lines[top] == nl[top]) top++;
    size_t bot = 0;
    while (bot < lines.size() - top && bot < nl.size() - top &&
           lines[lines.size() - 1 - bot] == nl[nl.size() - 1 - bot])
        bot++;
    OctaveLspTextEdit e;
    e.range.line = static_cast<int>(top);
    e.range.col = 0;
    const size_t old_end = lines.size() - bot;  // exclusive
    std::string repl;
    for (size_t i = top; i < nl.size() - bot; i++) repl += nl[i] + "\n";
    if (old_end < lines.size()) {
        e.range.end_line = static_cast<int>(old_end);
        e.range.end_col = 0;
    } else {
        e.range.end_line = static_cast<int>(lines.size()) - 1;
        e.range.end_col = static_cast<int>(lines.back().size());
        if (!repl.empty()) repl.pop_back();
        if (top > 0 && top >= lines.size()) {
            // appending after the last line
            e.range.line = static_cast<int>(lines.size()) - 1;
            e.range.col = static_cast<int>(lines.back().size());
            repl = "\n" + repl;
        }
    }
    e.new_text = repl;
    edits.push_back(e);
    return edits;
}
