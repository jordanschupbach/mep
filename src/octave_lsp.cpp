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
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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
    std::string path;  // the file's path (a file:// URI is decoded), "" when unknown
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
    doc->path = OctaveLspUriToPath(path);
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

// --- Help text and the load path --------------------------------------------------

std::string OctaveLspUriToPath(const std::string &uri) {
    if (uri.compare(0, 7, "file://") != 0) return uri;
    std::string p = uri.substr(7);
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

std::string TrimLeft(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

bool IsCommentLine(const std::string &l) {
    const std::string t = TrimLeft(l);
    return !t.empty() && (t[0] == '%' || t[0] == '#');
}
bool IsBlankLine(const std::string &l) { return TrimLeft(l).empty(); }
bool IsBlockOpen(const std::string &l) {
    std::string t = TrimLeft(l);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.pop_back();
    return t == "%{" || t == "#{";
}
bool IsBlockClose(const std::string &l) {
    std::string t = TrimLeft(l);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.pop_back();
    return t == "%}" || t == "#}";
}

// A comment line's text: the leading `%`/`#` run and one space dropped.
std::string CommentText(const std::string &l) {
    std::string t = TrimLeft(l);
    size_t i = 0;
    while (i < t.size() && (t[i] == '%' || t[i] == '#')) i++;
    if (i < t.size() && t[i] == ' ') i++;
    t = t.substr(i);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.pop_back();
    return t;
}

// The comment block starting at (or, backwards, ending at) `at`: line
// comments or one %{ ... %} block. Returns its text lines, empty when there is none.
std::vector<std::string> BlockForward(const std::vector<std::string> &lines, size_t at) {
    std::vector<std::string> out;
    if (at >= lines.size()) return out;
    if (IsBlockOpen(lines[at])) {
        for (size_t i = at + 1; i < lines.size() && !IsBlockClose(lines[i]); i++) out.push_back(lines[i]);
        return out;
    }
    for (size_t i = at; i < lines.size() && IsCommentLine(lines[i]) && !IsBlockOpen(lines[i]); i++)
        out.push_back(CommentText(lines[i]));
    return out;
}
std::vector<std::string> BlockBackward(const std::vector<std::string> &lines, size_t at) {
    std::vector<std::string> out;
    if (at >= lines.size()) return out;
    if (IsBlockClose(lines[at])) {
        size_t i = at;
        while (i > 0 && !IsBlockOpen(lines[i - 1])) i--;
        if (i == 0) return out;
        for (size_t k = i; k < at; k++) out.push_back(lines[k]);
        return out;
    }
    size_t i = at + 1;
    while (i > 0 && IsCommentLine(lines[i - 1]) && !IsBlockClose(lines[i - 1])) i--;
    for (size_t k = i; k <= at; k++) out.push_back(CommentText(lines[k]));
    return out;
}

bool IsCopyright(const std::vector<std::string> &block) {
    for (const std::string &l : block) {
        const std::string t = TrimLeft(l);
        if (t.empty()) continue;
        return t.compare(0, 9, "Copyright") == 0 || t.compare(0, 7, "Author:") == 0 ||
               t.compare(0, 4, "SPDX") == 0;
    }
    return false;
}

std::string Upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The argument of `@cmd{...}` starting at text[open] == '{' (nesting aware); `*end` past the '}'.
std::string BraceArg(const std::string &t, size_t open, size_t *end) {
    int depth = 0;
    std::string arg;
    for (size_t i = open; i < t.size(); i++) {
        const char c = t[i];
        if (c == '@' && i + 1 < t.size() && std::strchr("{}@", t[i + 1])) {
            if (depth > 0) arg += t.substr(i, 2);
            i++;
            continue;
        }
        if (c == '{') {
            if (depth++ > 0) arg += c;
        } else if (c == '}') {
            if (--depth == 0) {
                *end = i + 1;
                return arg;
            }
            arg += c;
        } else {
            arg += c;
        }
    }
    *end = t.size();
    return arg;
}

// Texinfo inline markup to plain text, the way Octave's `help` shows it.
std::string TexinfoInline(const std::string &t) {
    std::string out;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] != '@') {
            out += t[i];
            continue;
        }
        if (i + 1 < t.size() && std::strchr("{}@", t[i + 1])) {
            out += t[i + 1];
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < t.size() && std::isalpha(static_cast<unsigned char>(t[j]))) j++;
        const std::string cmd = t.substr(i + 1, j - i - 1);
        if (j < t.size() && t[j] == '{') {
            size_t end = j;
            const std::string arg = TexinfoInline(BraceArg(t, j, &end));
            if (cmd == "var") out += Upper(arg);
            else if (cmd == "code" || cmd == "samp" || cmd == "command" || cmd == "env" || cmd == "file")
                out += "'" + arg + "'";
            else if (cmd == "seealso") out += "See also: " + arg + ".";
            else if (cmd == "dots") out += "...";
            else if (cmd == "result") out += "=>";
            else if (cmd == "print") out += "-|";
            else if (cmd == "error") out += "error:";
            else out += arg;
            i = end - 1;
        } else {
            i = j - 1;  // unknown bare @cmd: dropped
        }
    }
    return out;
}

// A whole help block that starts with `-*- texinfo -*-`, as plain text.
std::vector<std::string> TexinfoToText(const std::vector<std::string> &block) {
    std::vector<std::string> out;
    bool in_example = false;
    for (size_t i = 0; i < block.size(); i++) {
        const std::string &l = block[i];
        if (l.find("-*- texinfo -*-") != std::string::npos) continue;
        const std::string t = TrimLeft(l);
        if (t.compare(0, 10, "@deftypefn") == 0) {
            // @deftypefn[x] {Category} {RET =} name (args)
            size_t p = t.find(' ');
            std::string rest = p == std::string::npos ? std::string() : TrimLeft(t.substr(p));
            if (!rest.empty() && rest[0] == '{') {  // the category
                size_t end = 0;
                BraceArg(rest, 0, &end);
                rest = TrimLeft(rest.substr(end));
            }
            std::string ret;
            if (!rest.empty() && rest[0] == '{') {
                size_t end = 0;
                ret = TexinfoInline(BraceArg(rest, 0, &end));
                rest = TrimLeft(rest.substr(end));
            }
            out.push_back(" -- " + (ret.empty() ? std::string() : ret + " ") + TexinfoInline(rest));
            continue;
        }
        if (t.compare(0, 4, "@end") == 0) {
            if (t.find("example") != std::string::npos) in_example = false;
            if (t.find("deftypefn") != std::string::npos) continue;
            continue;
        }
        if (t.compare(0, 8, "@example") == 0 || t.compare(0, 8, "@display") == 0 ||
            t.compare(0, 11, "@smallexample") == 0) {
            in_example = true;
            continue;
        }
        if (t.compare(0, 5, "@item") == 0) {
            out.push_back("     * " + TexinfoInline(TrimLeft(t.substr(5))));
            continue;
        }
        if (!t.empty() && t[0] == '@' && t.find('{') == std::string::npos) continue;  // @noindent, @table ...
        out.push_back((in_example ? "          " : "     ") + TexinfoInline(l));
    }
    while (!out.empty() && TrimLeft(out.back()).empty()) out.pop_back();
    return out;
}

std::string JoinHelp(std::vector<std::string> block) {
    while (!block.empty() && TrimLeft(block.front()).empty()) block.erase(block.begin());
    while (!block.empty() && TrimLeft(block.back()).empty()) block.pop_back();
    if (!block.empty() && block.front().find("-*- texinfo -*-") != std::string::npos) block = TexinfoToText(block);
    std::string s;
    for (const std::string &l : block) s += l + "\n";
    return s;
}

std::vector<std::string> ReadLines(const std::string &path, bool *ok) {
    std::ifstream in(path, std::ios::binary);
    *ok = static_cast<bool>(in);
    std::vector<std::string> lines;
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}

bool FileExists(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return static_cast<bool>(in);
}

std::vector<std::string> &ServerSearchPath() {
    static std::vector<std::string> dirs;
    return dirs;
}

std::string DirOf(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return std::string();
    return slash == 0 ? std::string("/") : path.substr(0, slash);
}

}  // namespace

std::string OctaveLspHelpText(const std::vector<std::string> &lines, int function_line, bool first_in_file) {
    if (function_line < 0 || static_cast<size_t>(function_line) >= lines.size()) return std::string();
    const size_t fl = static_cast<size_t>(function_line);
    // Above the function line (blank lines between allowed), skipping copyright blocks.
    size_t i = fl;
    while (i > 0) {
        size_t k = i;
        while (k > 0 && IsBlankLine(lines[k - 1])) k--;
        if (k == 0 || !(IsCommentLine(lines[k - 1]) || IsBlockClose(lines[k - 1]))) break;
        std::vector<std::string> block = BlockBackward(lines, k - 1);
        size_t top = k - 1;
        if (IsBlockClose(lines[k - 1])) {
            while (top > 0 && !IsBlockOpen(lines[top])) top--;
        } else {
            while (top > 0 && IsCommentLine(lines[top - 1]) && !IsBlockClose(lines[top - 1])) top--;
        }
        if (!IsCopyright(block)) {
            const std::string s = JoinHelp(block);
            if (!s.empty()) return s;
        }
        i = top;
    }
    // A function file's leading block (Octave reads it even when code separates them).
    if (first_in_file) {
        for (size_t k = 0; k < fl; k++) {
            if (IsBlankLine(lines[k])) continue;
            if (!(IsCommentLine(lines[k]) || IsBlockOpen(lines[k]))) break;
            std::vector<std::string> block = BlockForward(lines, k);
            if (!IsCopyright(block)) {
                const std::string s = JoinHelp(block);
                if (!s.empty()) return s;
            }
            if (IsBlockOpen(lines[k])) {
                while (k < fl && !IsBlockClose(lines[k])) k++;
            } else {
                while (k + 1 < fl && IsCommentLine(lines[k + 1])) k++;
            }
        }
    }
    // Below the function line (a signature continued with `...` first).
    size_t k = fl;
    while (k + 1 < lines.size() && lines[k].find("...") != std::string::npos && !IsCommentLine(lines[k])) k++;
    k++;
    while (k < lines.size() && IsBlankLine(lines[k])) k++;
    if (k < lines.size() && (IsCommentLine(lines[k]) || IsBlockOpen(lines[k]))) {
        std::vector<std::string> block = BlockForward(lines, k);
        if (!IsCopyright(block)) return JoinHelp(block);
    }
    return std::string();
}

void OctaveLspSetSearchPath(const std::vector<std::string> &dirs) { ServerSearchPath() = dirs; }

std::vector<std::string> OctaveLspSearchPath(const OctaveLspDocument &doc) {
    std::vector<std::string> dirs;
    auto add = [&dirs](const std::string &d) {
        if (!d.empty() && std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
    };
    const std::string own = DirOf(doc.path);
    // addpath('dir') / addpath("dir") / addpath dir with a literal argument.
    for (size_t i = 0; i < doc.tree.tokens.size(); i++) {
        const Token &t = doc.tree.tokens[i];
        if (t.kind != Tok::Name || (t.text != "addpath" && t.text != "path")) continue;
        for (size_t j = i + 1; j < doc.tree.tokens.size() && j < i + 12; j++) {
            const Token &a = doc.tree.tokens[j];
            if (a.kind == Tok::Newline || a.kind == Tok::Semi || (a.kind == Tok::Op && a.text == ")")) break;
            if (a.kind != Tok::DqString && a.kind != Tok::SqString) continue;
            const std::string dir = a.text;
            size_t start = 0;
            while (start <= dir.size()) {  // "a:b" (pathsep) adds both
                size_t sep = dir.find(':', start);
#if defined(_WIN32)
                sep = dir.find(';', start);
#endif
                std::string one = dir.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
                if (!one.empty() && one[0] != '/' && !own.empty()) one = own + "/" + one;
                add(one);
                if (sep == std::string::npos) break;
                start = sep + 1;
            }
        }
    }
    add(own);
    for (const std::string &d : ServerSearchPath()) add(d);
    return dirs;
}

namespace {

// A classdef's outline: methods (with their signature and Static attribute) and properties.
struct ClassMember {
    std::string name, signature;
    int line = 0;
    bool is_static = false;
};
struct ClassOutline {
    bool found = false;
    std::string name;
    std::vector<std::string> supers;
    int line = 0;
    std::vector<ClassMember> methods;
    std::vector<std::string> properties;  // not Hidden
};

// Whether a block's attribute list sets `attr` (Static, Hidden): `(Static)`, `(Static = true)`.
bool BlockHasAttr(const OctaveLspDocument &d, const AstNode &block, const char *attr) {
    for (int k : block.kids) {
        const AstNode &a = d.N(k);
        if (a.kind != Node::Attr || a.kids.empty() || d.N(a.kids[0]).tok < 0) continue;
        if (d.T(d.N(a.kids[0]).tok).text != attr) continue;
        if (a.kids.size() > 1 && d.N(a.kids[1]).tok >= 0 && d.T(d.N(a.kids[1]).tok).text == "false") return false;
        return true;
    }
    return false;
}

ClassOutline OutlineOf(const OctaveLspDocument &d) {
    ClassOutline c;
    if (d.tree.root < 0) return c;
    int cls = -1;
    for (size_t i = 0; i < d.tree.nodes.size() && cls < 0; i++)
        if (d.tree.nodes[i].kind == Node::Classdef) cls = static_cast<int>(i);
    if (cls < 0) return c;
    const AstNode &a = d.N(cls);
    if (a.name < 0 || d.N(a.name).tok < 0) return c;
    c.found = true;
    c.name = d.T(d.N(a.name).tok).text;
    OctaveLspRange g;
    if (d.NodeRange(cls, &g)) c.line = g.line;
    for (int k : a.kids) {
        const AstNode &b = d.N(k);
        if (b.kind == Node::Superclass && b.tok >= 0) c.supers.push_back(d.T(b.tok).text);
        if (b.kind != Node::ClassBlock || b.tok < 0) continue;
        const std::string kw = d.T(b.tok).text;
        const bool is_static = BlockHasAttr(d, b, "Static");
        const bool hidden = BlockHasAttr(d, b, "Hidden") || BlockHasAttr(d, b, "Access");
        for (int e : b.kids) {
            const AstNode &x = d.N(e);
            if (kw == "methods" && x.kind == Node::Function) {
                for (const FunctionInfo &f : d.functions) {
                    if (f.node != e) continue;
                    ClassMember m;
                    m.name = f.name;
                    m.signature = "function " + FunctionSignature(d, f);
                    if (d.NodeRange(e, &g)) m.line = g.line;
                    m.is_static = is_static;
                    c.methods.push_back(m);
                }
            } else if (kw == "properties" && x.kind == Node::Property && !hidden && !x.kids.empty() &&
                       d.N(x.kids[0]).tok >= 0) {
                c.properties.push_back(d.T(d.N(x.kids[0]).tok).text);
            }
        }
    }
    return c;
}

// A file read and analysed once per modification time.
struct CachedFile {
    std::vector<std::string> lines;
    std::shared_ptr<const OctaveLspDocument> doc;
    ClassOutline outline;
};
const CachedFile *Cached(const std::string &path) {
    struct Entry {
        std::filesystem::file_time_type mtime;
        CachedFile file;
    };
    static std::map<std::string, Entry> cache;
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) return nullptr;
    auto it = cache.find(path);
    if (it != cache.end() && it->second.mtime == mtime) return &it->second.file;
    bool ok = false;
    Entry e;
    e.mtime = mtime;
    e.file.lines = ReadLines(path, &ok);
    if (!ok) return nullptr;
    e.file.doc = OctaveLspAnalyze(e.file.lines, path);
    e.file.outline = OutlineOf(*e.file.doc);
    return &(cache[path] = std::move(e)).file;
}

std::vector<std::string> SplitDots(const std::string &s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t d = s.find('.', start);
        out.push_back(s.substr(start, d == std::string::npos ? std::string::npos : d - start));
        if (d == std::string::npos) break;
        start = d + 1;
    }
    return out;
}

// parts[0..n) as a path: +a/+b/c (the last part plain).
std::string PackagePath(const std::vector<std::string> &parts, size_t n) {
    std::string rel;
    for (size_t i = 0; i < n; i++) rel += (i + 1 < n ? "+" : "") + parts[i] + (i + 1 < n ? "/" : "");
    return rel;
}

// Whether help text has a line that starts (after indentation) with `heading`.
bool HasHeading(const std::string &help, const std::string &heading) {
    size_t at = 0;
    while (at < help.size()) {
        const size_t nl = help.find('\n', at);
        const std::string l = TrimLeft(help.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        if (l.compare(0, heading.size(), heading) == 0) return true;
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return false;
}

// The class's help block, then its properties and methods unless the help lists them already.
std::string ClassHelp(const CachedFile &f) {
    const ClassOutline &c = f.outline;
    std::string help = OctaveLspHelpText(f.lines, c.line, true);
    const std::string written = help;
    if (!c.properties.empty() && !HasHeading(written, "Properties")) {
        help += (help.empty() ? "" : "\n") + std::string("Properties:\n");
        for (const std::string &p : c.properties) help += "  " + p + "\n";
    }
    std::string inst, stat;
    for (const ClassMember &m : c.methods) {
        if (m.name == c.name) continue;  // the constructor is the class's own signature
        (m.is_static ? stat : inst) += "  " + m.signature.substr(9) + "\n";
    }
    if (!inst.empty() && !HasHeading(written, "Methods")) help += (help.empty() ? "" : "\n") + std::string("Methods:\n") + inst;
    if (!stat.empty() && !HasHeading(written, "Static methods"))
        help += (help.empty() ? "" : "\n") + std::string("Static methods:\n") + stat;
    return help;
}

}  // namespace

OctaveLspDocLookup OctaveLspFindMember(const std::string &class_name, const std::string &member,
                                       const std::vector<std::string> &dirs, int depth) {
    OctaveLspDocLookup r;
    const OctaveLspDocLookup cls = OctaveLspFindOnPath(class_name, dirs);
    if (!cls.found || cls.kind != "class" || depth > 8) return r;
    const CachedFile *f = Cached(cls.path);
    if (!f) return r;
    for (const ClassMember &m : f->outline.methods) {
        if (m.name != member || m.name == f->outline.name) continue;
        r.found = true;
        r.kind = m.is_static ? "static method" : "method";
        r.path = cls.path;
        r.line = m.line;
        r.signature = m.signature;
        r.help = OctaveLspHelpText(f->lines, m.line, false);
        r.qualified = cls.qualified + "." + member;
        return r;
    }
    for (const std::string &p : f->outline.properties) {
        if (p != member) continue;
        r.found = true;
        r.kind = "property";
        r.path = cls.path;
        r.qualified = cls.qualified + "." + member;
        r.signature = "property " + r.qualified;
        return r;
    }
    // Inherited: the superclasses, in order.
    for (const std::string &s : f->outline.supers) {
        r = OctaveLspFindMember(s, member, dirs, depth + 1);
        if (r.found) return r;
    }
    return OctaveLspDocLookup();
}

std::vector<OctaveLspClassMember> OctaveLspClassMembers(const std::string &class_name,
                                                        const std::vector<std::string> &dirs) {
    std::vector<OctaveLspClassMember> out;
    std::set<std::string> seen;
    std::vector<std::string> todo = {class_name};
    for (size_t i = 0; i < todo.size() && i < 16; i++) {
        const OctaveLspDocLookup cls = OctaveLspFindOnPath(todo[i], dirs);
        if (!cls.found || cls.kind != "class") continue;
        const CachedFile *f = Cached(cls.path);
        if (!f) continue;
        for (const ClassMember &m : f->outline.methods) {
            if (m.name == f->outline.name || !seen.insert(m.name).second) continue;
            out.push_back({m.name, m.signature, m.is_static, false});
        }
        for (const std::string &p : f->outline.properties)
            if (seen.insert(p).second) out.push_back({p, "property", false, true});
        for (const std::string &s : f->outline.supers) todo.push_back(s);
    }
    return out;
}

OctaveLspDocLookup OctaveLspFindInFile(const std::string &name, const std::string &path) {
    OctaveLspDocLookup r;
    const CachedFile *cf = Cached(path);
    if (!cf) return r;
    const std::vector<std::string> &lines = cf->lines;
    const OctaveLspDocument &doc = *cf->doc;
    const std::string stem = Basename(path);
    r.path = path;
    if (cf->outline.found) {
        // A classdef file: the class itself, or one of its methods.
        if (name == stem || name == cf->outline.name) {
            r.found = true;
            r.kind = "class";
            r.line = cf->outline.line;
            r.signature = "classdef " + cf->outline.name;
            for (size_t i = 0; i < cf->outline.supers.size(); i++)
                r.signature += (i ? " & " : " < ") + cf->outline.supers[i];
            r.help = ClassHelp(*cf);
            return r;
        }
        for (const ClassMember &m : cf->outline.methods) {
            if (m.name != name) continue;
            r.found = true;
            r.kind = m.is_static ? "static method" : "method";
            r.line = m.line;
            r.signature = m.signature;
            r.help = OctaveLspHelpText(lines, m.line, false);
            return r;
        }
        return OctaveLspDocLookup();
    }
    const FunctionInfo *f = nullptr;
    for (const FunctionInfo &g : doc.functions)
        if (g.name == name && !f) f = &g;
    if (!f && name == stem && !doc.functions.empty()) f = &doc.functions[0];
    if (!f) {
        // A script file named NAME: its leading comment block.
        if (name != stem) return r;
        r.found = true;
        r.kind = "script";
        size_t k = 0;
        while (k < lines.size() && IsBlankLine(lines[k])) k++;
        while (k < lines.size() && (IsCommentLine(lines[k]) || IsBlockOpen(lines[k]))) {
            std::vector<std::string> block = BlockForward(lines, k);
            if (!IsCopyright(block)) {
                r.help = JoinHelp(block);
                break;
            }
            while (k < lines.size() && IsCommentLine(lines[k])) k++;
            while (k < lines.size() && IsBlankLine(lines[k])) k++;
        }
        return r;
    }
    OctaveLspRange g;
    if (doc.NodeRange(f->node, &g)) r.line = g.line;
    r.found = true;
    r.kind = "function";
    r.signature = "function " + FunctionSignature(doc, *f);
    r.help = OctaveLspHelpText(lines, r.line, f == &doc.functions[0]);
    return r;
}

OctaveLspDocLookup OctaveLspFindOnPath(const std::string &name, const std::vector<std::string> &dirs) {
    const std::vector<std::string> parts = SplitDots(name);
    for (const std::string &p : parts)
        if (!IsIdentifier(p)) return OctaveLspDocLookup();
    const size_t n = parts.size();
    // pkg.fn / pkg.Class -> +pkg/fn.m
    for (const std::string &dir : dirs) {
        const std::string p = dir + "/" + PackagePath(parts, n) + ".m";
        if (FileExists(p)) {
            OctaveLspDocLookup r = OctaveLspFindInFile(parts.back(), p);
            if (r.found) r.qualified = name;
            return r;
        }
    }
    // pkg.Class.method -> a method of +pkg/Class.m (or of its superclasses).
    if (n >= 2) {
        std::string cls;
        for (size_t i = 0; i + 1 < n; i++) cls += (i ? "." : "") + parts[i];
        for (const std::string &dir : dirs) {
            if (!FileExists(dir + "/" + PackagePath(parts, n - 1) + ".m")) continue;
            return OctaveLspFindMember(cls, parts.back(), dirs);
        }
    }
    return OctaveLspDocLookup();
}

std::vector<std::string> OctaveLspPackageNames(const std::vector<std::string> &dirs) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::set<std::string> seen;
    struct Walk {
        std::vector<std::string> &out;
        std::set<std::string> &seen;
        void Dir(const fs::path &dir, const std::string &prefix, int depth) {
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                const std::string fn = it->path().filename().string();
                if (it->is_directory(ec)) {
                    if (fn.size() > 1 && fn[0] == '+' && depth < 6) Dir(it->path(), prefix + fn.substr(1) + ".", depth + 1);
                } else if (!prefix.empty() && fn.size() > 2 && fn.compare(fn.size() - 2, 2, ".m") == 0) {
                    const std::string q = prefix + fn.substr(0, fn.size() - 2);
                    if (seen.insert(q).second) out.push_back(q);
                }
            }
        }
    } w{out, seen};
    for (const std::string &d : dirs) {
        std::error_code ec;
        if (!d.empty() && fs::is_directory(d, ec)) w.Dir(d, "", 0);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> OctaveLspImports(const OctaveLspDocument &doc) {
    std::vector<std::string> out;
    const auto &toks = doc.tree.tokens;
    for (size_t i = 0; i < toks.size(); i++) {
        if (toks[i].kind != Tok::Name || toks[i].text != "import") continue;
        for (size_t j = i + 1; j < toks.size() && toks[j].kind != Tok::Newline && toks[j].kind != Tok::Semi; j++) {
            if (toks[j].kind != Tok::SqString && toks[j].kind != Tok::DqString) continue;
            if (!toks[j].text.empty()) out.push_back(toks[j].text);
        }
    }
    return out;
}

OctaveLspDocLookup OctaveLspResolveName(const std::string &name, const std::vector<std::string> &dirs,
                                        const std::vector<std::string> &imports, std::vector<std::string> *candidates) {
    OctaveLspDocLookup r = OctaveLspFindOnPath(name, dirs);
    if (r.found) return r;
    const std::vector<std::string> parts = SplitDots(name);
    const std::string head = parts[0];
    const std::string rest = name.size() > head.size() ? name.substr(head.size()) : std::string();
    // import pkg.* / import pkg.Name
    for (const std::string &imp : imports) {
        std::string q;
        if (imp.size() > 2 && imp.compare(imp.size() - 2, 2, ".*") == 0) q = imp.substr(0, imp.size() - 2) + "." + head;
        else if (imp.size() > head.size() && imp.compare(imp.size() - head.size() - 1, std::string::npos, "." + head) == 0)
            q = imp;
        if (q.empty()) continue;
        r = OctaveLspFindOnPath(q + rest, dirs);
        if (r.found) return r;
    }
    // The one package member of that name.
    std::vector<std::string> matches;
    for (const std::string &q : OctaveLspPackageNames(dirs)) {
        const size_t dot = q.rfind('.');
        if (q.compare(dot + 1, std::string::npos, head) == 0) matches.push_back(q);
    }
    if (candidates) *candidates = matches;
    if (matches.size() == 1) return OctaveLspFindOnPath(matches[0] + rest, dirs);
    return OctaveLspDocLookup();
}

std::vector<OctaveLspDocLookup> OctaveLspMethodsNamed(const std::string &method, const std::vector<std::string> &dirs,
                                                      size_t limit) {
    std::vector<OctaveLspDocLookup> out;
    for (const std::string &q : OctaveLspPackageNames(dirs)) {
        if (out.size() >= limit) break;
        const OctaveLspDocLookup cls = OctaveLspFindOnPath(q, dirs);
        if (!cls.found || cls.kind != "class") continue;
        const CachedFile *f = Cached(cls.path);
        if (!f) continue;
        for (const ClassMember &m : f->outline.methods) {
            if (m.name != method || m.name == f->outline.name) continue;
            OctaveLspDocLookup r;
            r.found = true;
            r.kind = m.is_static ? "static method" : "method";
            r.path = cls.path;
            r.line = m.line;
            r.signature = m.signature;
            r.help = OctaveLspHelpText(f->lines, m.line, false);
            r.qualified = q + "." + method;
            out.push_back(r);
            break;
        }
    }
    return out;
}

// --- What an expression is: a package, a class, an instance of one, a callable ----

namespace {

struct ExprType {
    enum Kind { None, Package, ClassRef, Instance, Callable } kind = None;
    std::string q;  // the package / class / callable's qualified name
};

// The class a method's or function's help says it returns: Rosetta's
// "Output:\n  r  pkg.Class or []" (or a class-named first word of the next line).
std::string OutputClass(const OctaveLspDocLookup &d, const std::vector<std::string> &dirs) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : d.help + "\n") {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    for (size_t i = 0; i + 1 < lines.size(); i++) {
        const std::string t = TrimLeft(lines[i]);
        if (t.compare(0, 7, "Output:") != 0 && t.compare(0, 8, "Outputs:") != 0) continue;
        std::string words = TrimLeft(lines[i + 1]);
        // "r  datamunge.DataFrame or []": the second word.
        const size_t sp = words.find_first_of(" \t");
        if (sp == std::string::npos) return std::string();
        words = TrimLeft(words.substr(sp));
        const std::string w = words.substr(0, words.find_first_of(" \t,"));
        const OctaveLspDocLookup c = OctaveLspFindOnPath(w, dirs);
        return c.found && c.kind == "class" ? c.qualified : std::string();
    }
    return std::string();
}

class Typer {
public:
    // Types names as of position (line, col) of `d`.
    Typer(const OctaveLspDocument &d, int line, int col) : d_(d), line_(line), col_(col) {
        dirs_ = OctaveLspSearchPath(d);
        imports_ = OctaveLspImports(d);
        for (size_t i = 0; i < d.tree.nodes.size(); i++) {
            const AstNode &a = d.tree.nodes[i];
            if (a.kind != Node::Assign || a.kids.size() < 2) continue;
            const AstNode &lhs = d.N(a.kids[0]);
            if (lhs.kind != Node::Identifier || lhs.tok < 0) continue;
            OctaveLspRange g;
            if (!d.NodeRange(static_cast<int>(i), &g)) continue;
            assigns_.push_back({g.end_line, g.end_col, a.last, d.T(lhs.tok).text, a.kids[1]});
        }
        std::sort(assigns_.begin(), assigns_.end(), [](const Assign &x, const Assign &y) { return x.last < y.last; });
    }

    // The type of node `n` of `e` (`d` itself, or an expression parsed apart and typed at the position).
    ExprType Of(const OctaveLspDocument &e, int n, int guard = 0) {
        ExprType t;
        if (n < 0 || guard > 32) return t;
        const AstNode &a = e.N(n);
        switch (a.kind) {
        case Node::Paren:
            return a.kids.empty() ? t : Of(e, a.kids[0], guard + 1);
        case Node::Identifier: {
            if (a.tok < 0) return t;
            const std::string name = e.T(a.tok).text;
            // The last assignment to it before this point decides.
            for (size_t i = assigns_.size(); i-- > 0;) {
                const Assign &as = assigns_[i];
                if (as.name != name) continue;
                const bool before = &e == &d_ ? as.last < a.tok
                                              : (as.end_line < line_ || (as.end_line == line_ && as.end_col <= col_));
                if (!before) continue;
                return Value(Of(d_, as.rhs, guard + 1));
            }
            return Named(name, true);
        }
        case Node::Field: {
            if (a.kids.empty() || a.tok < 0) return t;
            const std::string member = e.T(a.tok).text;
            const ExprType base = Of(e, a.kids[0], guard + 1);
            if (base.kind == ExprType::Package) return Named(base.q + "." + member, false);
            if (base.kind == ExprType::ClassRef || base.kind == ExprType::Instance) {
                const OctaveLspDocLookup m = OctaveLspFindMember(base.q, member, dirs_);
                if (!m.found || m.kind == "property") return t;
                t.kind = ExprType::Callable;
                t.q = m.qualified;
            }
            return t;
        }
        case Node::Index:
            return a.kids.empty() ? t : Value(Of(e, a.kids[0], guard + 1));
        default:
            return t;
        }
    }

    // What a name, class or callable gives as a value: a class's instance, a callable's output class.
    ExprType Value(const ExprType &f) {
        ExprType t;
        if (f.kind == ExprType::ClassRef || f.kind == ExprType::Instance) {
            t.kind = ExprType::Instance;
            t.q = f.q;
        } else if (f.kind == ExprType::Callable) {
            const std::string c = OutputClass(OctaveLspFindOnPath(f.q, dirs_), dirs_);
            if (!c.empty()) {
                t.kind = ExprType::Instance;
                t.q = c;
            }
        }
        return t;
    }

    // A dotted name: a package directory, a class, a function or method; bare
    // names also through the file's imports.
    ExprType Named(const std::string &q, bool bare) {
        ExprType t;
        std::string pkg;
        for (const std::string &p : SplitDots(q)) pkg += "/+" + p;
        for (const std::string &dir : dirs_) {
            std::error_code ec;
            if (std::filesystem::is_directory(dir + pkg, ec)) {
                t.kind = ExprType::Package;
                t.q = q;
                return t;
            }
        }
        const OctaveLspDocLookup r = bare ? OctaveLspResolveName(q, dirs_, imports_, nullptr) : OctaveLspFindOnPath(q, dirs_);
        if (!r.found) return t;
        t.q = r.qualified.empty() ? q : r.qualified;
        t.kind = r.kind == "class" ? ExprType::ClassRef : ExprType::Callable;
        return t;
    }

    const std::vector<std::string> &dirs() const { return dirs_; }
    const std::vector<std::string> &imports() const { return imports_; }

private:
    struct Assign {
        int end_line, end_col, last;
        std::string name;
        int rhs;
    };
    const OctaveLspDocument &d_;
    int line_, col_;
    std::vector<std::string> dirs_, imports_;
    std::vector<Assign> assigns_;
};

// The Field node whose member name is token `tok`, or -1.
int FieldOfToken(const OctaveLspDocument &d, int tok) {
    for (size_t i = 0; i < d.tree.nodes.size(); i++)
        if (d.tree.nodes[i].kind == Node::Field && d.tree.nodes[i].tok == tok) return static_cast<int>(i);
    return -1;
}

}  // namespace

OctaveLspDocLookup OctaveLspMemberAt(const OctaveLspDocument &doc, int tok, bool *instance_call) {
    OctaveLspDocLookup r;
    if (instance_call) *instance_call = false;
    const int field = FieldOfToken(doc, tok);
    if (field < 0 || doc.N(field).kids.empty()) return r;
    const std::string member = doc.T(tok).text;
    Typer typer(doc, doc.T(tok).line, doc.T(tok).col);
    const ExprType base = typer.Of(doc, doc.N(field).kids[0]);
    if (base.kind == ExprType::Package) {
        r = OctaveLspFindOnPath(base.q + "." + member, typer.dirs());
    } else if (base.kind == ExprType::ClassRef || base.kind == ExprType::Instance) {
        r = OctaveLspFindMember(base.q, member, typer.dirs());
        if (instance_call) *instance_call = base.kind == ExprType::Instance && r.kind == "method";
    } else if (base.kind == ExprType::None) {
        // Unknown receiver: the methods of that name across the classes on the path.
        const std::vector<OctaveLspDocLookup> all = OctaveLspMethodsNamed(member, typer.dirs(), 12);
        if (all.empty()) return r;
        r = all[0];
        if (instance_call) *instance_call = r.kind == "method";
        if (all.size() > 1) {
            std::string others;
            for (size_t i = 1; i < all.size(); i++) others += (i > 1 ? ", " : "") + all[i].qualified;
            r.note = "the receiver's class is unknown: also a method of " + others;
        }
    }
    return r;
}

std::vector<OctaveLspClassMember> OctaveLspMembersOf(const OctaveLspDocument &doc, int line, const std::string &receiver) {
    std::vector<OctaveLspClassMember> out;
    // The receiver text, parsed apart and typed with this document's assignments before `line`.
    const auto expr = OctaveLspAnalyze({"__mep_recv__ = " + receiver + ";"});
    int rhs = -1;
    for (const AstNode &a : expr->tree.nodes)
        if (a.kind == Node::Assign && a.kids.size() > 1 && rhs < 0) rhs = a.kids[1];
    if (rhs < 0) return out;
    Typer typer(doc, line, 0);
    const ExprType t = typer.Of(*expr, rhs);
    if (t.kind == ExprType::Package) {
        const std::string prefix = t.q + ".";
        std::set<std::string> seen;
        for (const std::string &q : OctaveLspPackageNames(typer.dirs())) {
            if (q.compare(0, prefix.size(), prefix) != 0) continue;
            std::string rest = q.substr(prefix.size());
            const size_t dot = rest.find('.');
            const bool sub = dot != std::string::npos;
            if (sub) rest = rest.substr(0, dot);
            if (!seen.insert(rest).second) continue;
            if (sub) {
                out.push_back({rest, "package", false, false, true});
                continue;
            }
            const OctaveLspDocLookup d = OctaveLspFindOnPath(q, typer.dirs());
            out.push_back({rest, d.signature, false, false, true});
        }
    } else if (t.kind == ExprType::ClassRef || t.kind == ExprType::Instance) {
        for (const OctaveLspClassMember &m : OctaveLspClassMembers(t.q, typer.dirs()))
            if (t.kind == ExprType::Instance || m.is_static) out.push_back(m);
    }
    return out;
}

// The type of variable token `tok` as of its position, e.g. "datamunge.DataFrame" ("" when unknown).
std::string OctaveLspVariableClass(const OctaveLspDocument &doc, int tok) {
    for (size_t i = 0; i < doc.tree.nodes.size(); i++) {
        const AstNode &a = doc.tree.nodes[i];
        if (a.kind != Node::Identifier || a.tok != tok) continue;
        Typer typer(doc, doc.T(tok).line, doc.T(tok).col);
        // A write: the value assigned.
        for (const AstNode &b : doc.tree.nodes)
            if (b.kind == Node::Assign && b.kids.size() > 1 && b.kids[0] == static_cast<int>(i)) {
                const ExprType v = typer.Value(typer.Of(doc, b.kids[1]));
                return v.kind == ExprType::Instance ? v.q : std::string();
            }
        const ExprType v = typer.Of(doc, static_cast<int>(i));
        return v.kind == ExprType::Instance ? v.q : std::string();
    }
    return std::string();
}

std::string OctaveLspDocMarkdown(const OctaveLspDocLookup &d) {
    std::string s;
    if (!d.signature.empty()) s += "```octave\n" + d.signature + "\n```\n";
    if (!d.help.empty()) s += (s.empty() ? "" : "\n") + std::string("```text\n") + d.help + "```\n";
    if (!d.note.empty()) s += (s.empty() ? "" : "\n") + std::string("(") + d.note + ")\n";
    std::string where;
    if (!d.kind.empty() && d.kind != "function" && d.kind != "script")
        where = d.kind + (d.qualified.empty() ? std::string() : " `" + d.qualified + "`") + (d.path.empty() ? "" : " -- ");
    if (!d.path.empty()) where += "*" + d.path + "*";
    if (!where.empty()) s += (s.empty() ? "" : "\n") + where;
    return s;
}

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
    if (after_dot) {
        // A package's contents, a class's static methods, an instance's methods and properties.
        int rs = start - 1;
        int depth = 0;
        while (rs > 0) {
            const char c = cur[static_cast<size_t>(rs - 1)];
            if (c == ')') depth++;
            else if (c == '(' && depth > 0) depth--;
            else if (depth == 0 && !(IsWordChar(c) || c == '.')) break;
            rs--;
            // `obj.f (x).`: the call's name may stand apart from its parentheses.
            if (c == '(' && depth == 0)
                while (rs > 0 && cur[static_cast<size_t>(rs - 1)] == ' ') rs--;
        }
        const std::string receiver = cur.substr(static_cast<size_t>(rs), static_cast<size_t>(start - 1 - rs));
        if (!receiver.empty() && receiver.back() != '.') {
            const std::vector<OctaveLspClassMember> ms = OctaveLspMembersOf(doc, line, receiver);
            for (const OctaveLspClassMember &m : ms) {
                OctaveLspKind k = m.is_property ? OctaveLspKind::Property : OctaveLspKind::Method;
                if (m.in_package)
                    k = m.signature == "package"                         ? OctaveLspKind::Module
                        : m.signature.compare(0, 9, "classdef ") == 0 ? OctaveLspKind::Class
                                                                         : OctaveLspKind::Function;
                add(m.name, k, m.signature.compare(0, 9, "function ") == 0 ? m.signature.substr(9) : m.signature);
            }
            if (!ms.empty()) return out;
        }
        // Otherwise a field: every field name the document uses.
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
        const OctaveLspDocLookup m = OctaveLspMemberAt(doc, tok, nullptr);
        h.text = m.found ? OctaveLspDocMarkdown(m) : "field `" + t.text + "`";
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
            const std::string cls = OctaveLspVariableClass(doc, tok);
            if (!cls.empty()) h.text += ": `" + cls + "`";
            return h;
        }
    }
    if (const FunctionInfo *f = doc.FindFunction(nm)) {
        h.found = true;
        h.text = "```octave\nfunction " + FunctionSignature(doc, *f) + "\n```\n";
        OctaveLspRange g;
        if (doc.NodeRange(f->node, &g)) {
            const std::string help = OctaveLspHelpText(doc.lines, g.line, f == &doc.functions[0]);
            if (!help.empty()) h.text += "\n```text\n" + help + "```\n\n";
        }
        h.text += f->method ? "method in this file" : "function in this file";
        return h;
    }
    // A function file on the search path (the user's shadow Octave's own).
    if (t.kind == Tok::Name || t.kind == Tok::FcnHandle) {
        const std::vector<std::string> dirs = OctaveLspSearchPath(doc);
        std::vector<std::string> candidates;
        const OctaveLspDocLookup d = OctaveLspResolveName(nm, dirs, OctaveLspImports(doc), &candidates);
        if (d.found) {
            h.found = true;
            h.text = OctaveLspDocMarkdown(d);
            return h;
        }
        // A package name (`datamunge` of `datamunge.f`).
        for (const std::string &dir : dirs) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir + "/+" + nm, ec)) continue;
            h.found = true;
            h.text = "package `" + nm + "` -- *" + dir + "/+" + nm + "*";
            return h;
        }
        if (candidates.size() > 1 && !OctaveVocabFind(nm.c_str(), nm.size())) {
            h.found = true;
            h.text = "`" + nm + "` is ambiguous:";
            for (const std::string &c : candidates) h.text += " `" + c + "`";
            return h;
        }
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
    const int callee_tok = stack.back().first - 1;
    const Token &callee = toks[static_cast<size_t>(callee_tok)];
    if (callee.kind != Tok::Name && callee.kind != Tok::StructElt) return sh;
    const std::string &nm = callee.text;
    std::string label;
    OctaveLspDocLookup on_path;
    if (callee.kind == Tok::StructElt) {
        // obj.method( / pkg.fn( / pkg.Class.static(: an instance call passes obj itself.
        bool instance = false;
        on_path = OctaveLspMemberAt(doc, callee_tok, &instance);
        if (!on_path.found || on_path.signature.compare(0, 9, "function ") != 0) return sh;
        label = on_path.signature.substr(9);
        if (instance) {
            const size_t lp = label.find('(');
            const size_t first_end = label.find_first_of(",)", lp == std::string::npos ? 0 : lp);
            if (lp != std::string::npos && first_end != std::string::npos)
                label.erase(lp + 1, label[first_end] == ',' ? first_end + 1 - lp : first_end - lp - 1);
            while (lp != std::string::npos && lp + 1 < label.size() && label[lp + 1] == ' ') label.erase(lp + 1, 1);
        }
        if (!on_path.help.empty()) sh.documentation = "```text\n" + on_path.help + "```";
    } else if (const FunctionInfo *f = doc.FindFunction(nm)) {
        label = FunctionSignature(doc, *f);
        OctaveLspRange g;
        if (doc.NodeRange(f->node, &g)) sh.documentation = OctaveLspHelpText(doc.lines, g.line, f == &doc.functions[0]);
        if (!sh.documentation.empty()) sh.documentation = "```text\n" + sh.documentation + "```";
    } else if ((on_path = OctaveLspResolveName(nm, OctaveLspSearchPath(doc), OctaveLspImports(doc), nullptr)).found &&
               on_path.signature.compare(0, 9, "function ") == 0) {
        label = on_path.signature.substr(9);  // past "function "
        if (!on_path.help.empty()) sh.documentation = "```text\n" + on_path.help + "```";
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
