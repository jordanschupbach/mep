// mep's GAP language server, analysis half: see gap_lsp.h.
//
// Everything runs on gap_parse.h's reader (GAP's own tokens, grammar and
// first syntax error). On top of the tree: a scope model (a function's
// parameters and locals, closures seeing the enclosing functions' locals,
// everything else global), GAP's own read-time warning "Unbound global
// variable" decided the way GAP decides it (see UnboundGlobals below), and
// the editor features over the same bindings. Completion and signature help
// also work from the token stream, so they keep working inside a statement
// that does not parse yet.
#include "gap_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>

#include "gap_format.h"
#include "gap_lsp_vocab.h"
#include "gap_parse.h"

using gapparse::K;
using gapparse::T;

namespace {

struct Binding {
    std::string name;
    char kind = 'g';     // 'p' parameter, 'l' local, 'g' global
    int decl_tok = -1;   // a parameter's or local's own token
    int func = -1;       // the Function/Lambda node declaring it (0: the file's own `local`)
    int reads = 0, writes = 0, isbound = 0, unbinds = 0;
    int loop_writes = 0;  // of `writes`: as a for loop's variable
};

// One mention of a name. `how`: 'd' declaration (param/local), 'r' read, 'w' assignment, 'b' IsBound
// operand, 'u' Unbind operand, 's' the name as a string (DeclareGlobalFunction("Name")).
struct Occ {
    int tok = -1;
    int binding = -1;
    char how = 'r';
    int func = -1;  // innermost function node, -1 at the top level
};

}  // namespace

class GapLspDocument {
public:
    std::vector<std::string> lines;
    std::string text;
    std::vector<uint32_t> line_start;
    gapparse::Parse parse;
    std::vector<Binding> bindings;
    std::vector<Occ> occs;
    std::unordered_map<std::string, int> globals;     // name -> binding
    std::unordered_map<int, int> tok_occ;             // token -> occurrence
    std::unordered_map<std::string, int> bound_from;  // global -> first token index from which it is bound
    std::map<std::string, std::vector<int>> defs;     // global -> defining tokens (assignment, Declare*, Install*)
    std::unordered_map<int, std::string> func_name;   // function node -> the name it is defined under
    std::set<std::string> fields;                     // record component names
    std::shared_ptr<const std::set<std::string>> context;  // names the rest of the package binds
    bool opaque = false;  // the package binds names we cannot see (a compiled kernel extension)
};

namespace {

using Doc = GapLspDocument;

// ---- vocabulary -----------------------------------------------------------------------

// Short all-lowercase names a package leaves bound at start-up (`g`, `x`, `gens`, `hom`, ... -- loop
// variables of its own init code): GAP itself would not warn about them, but a function using `x` without
// declaring it local is a bug this server should still point out.
bool StrayStartupName(const std::string &name) {
    if (name.size() > 5 || name == "last" || name == "last2" || name == "last3") return false;
    for (char c : name)
        if (!std::islower(static_cast<unsigned char>(c)) && !std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool KnownGlobal(const std::string &name) {
    if (StrayStartupName(name)) return false;
    const char *const *end = kGapGlobals + kGapGlobalsSize;
    const char *const *it = std::lower_bound(kGapGlobals, end, name.c_str(),
                                             [](const char *a, const char *b) { return std::strcmp(a, b) < 0; });
    return it != end && name == *it;
}

// The documented forms of @p name: [first, last) in kGapVocab.
std::pair<const GapVocabEntry *, const GapVocabEntry *> VocabRange(const std::string &name) {
    const GapVocabEntry *end = kGapVocab + kGapVocabSize;
    const GapVocabEntry *lo = std::lower_bound(kGapVocab, end, name.c_str(), [](const GapVocabEntry &e, const char *n) {
        return std::strcmp(e.name, n) < 0;
    });
    const GapVocabEntry *hi = lo;
    while (hi != end && name == hi->name) hi++;
    return {lo, hi};
}

bool Documented(const std::string &name) {
    const auto r = VocabRange(name);
    return r.first != r.second;
}

GapLspKind CompletionKindOf(const char *kind) {
    const std::string k = kind;
    if (k == "Func") return GapLspKind::Function;
    if (k == "Oper" || k == "Meth" || k == "Constr") return GapLspKind::Method;
    if (k == "Attr" || k == "Prop") return GapLspKind::Property;
    if (k == "Filt" || k == "Repr" || k == "Coll" || k == "Fam") return GapLspKind::Class;
    if (k == "InfoClass") return GapLspKind::Constant;
    return GapLspKind::Variable;
}

const char *KindWord(const std::string &k) {
    if (k == "Func") return "function";
    if (k == "Oper") return "operation";
    if (k == "Meth") return "method";
    if (k == "Constr") return "constructor";
    if (k == "Attr") return "attribute";
    if (k == "Prop") return "property";
    if (k == "Filt") return "filter";
    if (k == "Repr") return "representation";
    if (k == "Coll") return "collections category";
    if (k == "Fam") return "family";
    if (k == "InfoClass") return "info class";
    if (k == "Var") return "variable";
    return "name";
}

// Library functions whose first argument (a string) names a global they bind, and the extra names they
// bind with it: an attribute or property also binds `HasName` and `SetName`.
struct Binder {
    const char *callee;
    bool has_set;
    const char *suffix;  // DeclareCategoryCollections("IsX") binds IsXCollection
    GapLspSymbolKind kind;
};

const Binder kBinders[] = {
    {"BindGlobal", false, nullptr, GapLspSymbolKind::Variable},
    {"BindConstant", false, nullptr, GapLspSymbolKind::Constant},
    {"DeclareGlobalFunction", false, nullptr, GapLspSymbolKind::Function},
    {"DeclareGlobalVariable", false, nullptr, GapLspSymbolKind::Variable},
    {"DeclareGlobalName", false, nullptr, GapLspSymbolKind::Variable},
    {"DeclareOperation", false, nullptr, GapLspSymbolKind::Method},
    {"DeclareOperationKernel", false, nullptr, GapLspSymbolKind::Method},
    {"DeclareConstructor", false, nullptr, GapLspSymbolKind::Method},
    {"DeclareConstructorKernel", false, nullptr, GapLspSymbolKind::Method},
    {"DeclareAttribute", true, nullptr, GapLspSymbolKind::Property},
    {"DeclareAttributeKernel", true, nullptr, GapLspSymbolKind::Property},
    {"DeclareProperty", true, nullptr, GapLspSymbolKind::Property},
    {"DeclarePropertyKernel", true, nullptr, GapLspSymbolKind::Property},
    {"DeclareSynonymAttr", true, nullptr, GapLspSymbolKind::Property},
    {"DeclareSynonym", false, nullptr, GapLspSymbolKind::Function},
    {"DeclareFilter", false, nullptr, GapLspSymbolKind::Class},
    {"DeclareCategory", false, nullptr, GapLspSymbolKind::Class},
    {"DeclareCategoryKernel", false, nullptr, GapLspSymbolKind::Class},
    {"DeclareRepresentation", false, nullptr, GapLspSymbolKind::Class},
    {"DeclareRepresentationKernel", false, nullptr, GapLspSymbolKind::Class},
    {"DeclareInfoClass", false, nullptr, GapLspSymbolKind::Constant},
    {"DeclareCategoryCollections", false, "Collection", GapLspSymbolKind::Class},
    {"DeclareCategoryFamily", false, "Family", GapLspSymbolKind::Class},
};

const Binder *FindBinder(const std::string &callee) {
    for (const Binder &b : kBinders)
        if (callee == b.callee) return &b;
    return nullptr;
}

std::vector<std::string> BoundBy(const Binder &b, const std::string &name) {
    std::vector<std::string> out;
    if (b.suffix) {
        // DeclareCategoryCollections("IsGroupElement") binds IsGroupElementCollection.
        out.push_back(name + b.suffix);
        return out;
    }
    out.push_back(name);
    if (b.has_set) {
        out.push_back("Has" + name);
        out.push_back("Set" + name);
    }
    return out;
}

// Functions whose first argument is the global they give a value or a method to.
bool IsInstaller(const std::string &callee) {
    return callee == "InstallGlobalFunction" || callee == "InstallValue" || callee == "InstallFlushableValue" ||
           callee == "InstallMethod" || callee == "InstallOtherMethod" || callee == "InstallImmediateMethod" ||
           callee == "InstallTrueMethod" || callee == "InstallMethodWithRandomSource" ||
           callee == "InstallEarlyMethod" || callee == "InstallSynonym";
}

bool IsKeywordName(const std::string &s) {
    static const char *const kw[] = {"and", "atomic", "break", "continue", "do", "elif", "else", "end", "false",
                                     "fi", "for", "function", "if", "in", "local", "mod", "not", "od", "or",
                                     "readonly", "readwrite", "rec", "repeat", "return", "then", "true", "until",
                                     "while", "quit", "QUIT", "IsBound", "Unbind", "TryNextMethod", "Info",
                                     "Assert"};
    for (const char *k : kw)
        if (s == k) return true;
    return false;
}

bool IsIdentChar(int c) { return std::isalnum(c) || c == '_' || c == '@'; }

bool ValidIdentifier(const std::string &s) {
    if (s.empty() || IsKeywordName(s)) return false;
    bool alpha = false;
    for (char c : s) {
        if (!IsIdentChar(static_cast<unsigned char>(c))) return false;
        if (!std::isdigit(static_cast<unsigned char>(c))) alpha = true;
    }
    return alpha;  // all digits is a number
}

// ---- positions --------------------------------------------------------------------------

const gapparse::Token &Tk(const Doc &d, int i) { return d.parse.Tok(i); }
const gapparse::Node &Nd(const Doc &d, int i) { return d.parse.N(i); }

void PosOf(const Doc &d, uint32_t off, int *line, int *col) {
    const auto it = std::upper_bound(d.line_start.begin(), d.line_start.end(), off);
    const size_t l = static_cast<size_t>(it - d.line_start.begin()) - 1;
    *line = static_cast<int>(l);
    *col = static_cast<int>(off - d.line_start[l]);
}

GapLspRange RangeOf(const Doc &d, uint32_t begin, uint32_t end) {
    GapLspRange r;
    PosOf(d, begin, &r.line, &r.col);
    PosOf(d, end, &r.end_line, &r.end_col);
    return r;
}

GapLspRange TokRange(const Doc &d, int tok) { return RangeOf(d, Tk(d, tok).begin, Tk(d, tok).end); }

// A string token's contents (without its quotes), when it is a plain one-line string.
bool StringValue(const Doc &d, int tok, std::string *out) {
    const std::string_view s = d.parse.Spelling(tok);
    if (s.size() < 2 || s.front() != '"' || s.back() != '"' || s.substr(0, 3) == "\"\"\"") return false;
    const std::string_view in = s.substr(1, s.size() - 2);
    if (in.find('\\') != std::string_view::npos || in.find('\n') != std::string_view::npos) return false;
    *out = std::string(in);
    return true;
}

uint32_t OffsetOf(const Doc &d, int line, int col) {
    if (line < 0) return 0;
    if (static_cast<size_t>(line) >= d.lines.size()) return static_cast<uint32_t>(d.text.size());
    const std::string &l = d.lines[static_cast<size_t>(line)];
    const int c = std::max(0, std::min(col, static_cast<int>(l.size())));
    return d.line_start[static_cast<size_t>(line)] + static_cast<uint32_t>(c);
}

// The token under the cursor: one containing the offset, else one ending at it.
int TokenAt(const Doc &d, uint32_t off) {
    const auto &toks = d.parse.tokens;
    const auto it = std::upper_bound(toks.begin(), toks.end(), off,
                                     [](uint32_t o, const gapparse::Token &t) { return o < t.begin; });
    if (it == toks.begin()) return -1;
    const int i = static_cast<int>(it - toks.begin()) - 1;
    const gapparse::Token &t = toks[static_cast<size_t>(i)];
    if (t.kind == T::Eof) return -1;
    if (off < t.end) return i;
    if (off == t.end) return i;
    return -1;
}

// ---- the scope walk ---------------------------------------------------------------------

class Walker {
public:
    explicit Walker(Doc *d) : d_(d) {}

    void Run() {
        const gapparse::Parse &p = d_->parse;
        if (p.nodes.empty()) return;
        const gapparse::Node &file = p.N(0);
        size_t first = 0;
        scopes_.emplace_back();
        if (!file.kids.empty() && p.N(file.kids[0]).kind == K::Locals) {
            for (int id : p.N(file.kids[0]).kids) Declare(p.N(id).tok, 'l', 0);
            first = 1;
        }
        for (size_t i = first; i < file.kids.size(); i++) {
            const int st = file.kids[i];
            pending_binds_.clear();
            Walk(st, false);
            // A library call that binds a name has run by the time the next statement is read.
            const int after = p.N(st).last + 1;
            for (const std::string &name : pending_binds_) MarkBound(name, after);
        }
        NameFunctions();
    }

private:
    Doc *d_;
    std::vector<std::unordered_map<std::string, int>> scopes_;  // [0]: the file's own locals
    std::vector<int> funcs_;                                    // enclosing function nodes
    std::vector<std::string> pending_binds_;

    const gapparse::Node &N(int i) const { return d_->parse.N(i); }
    const gapparse::Token &Tok(int i) const { return d_->parse.Tok(i); }
    int Func() const { return funcs_.empty() ? -1 : funcs_.back(); }

    void MarkBound(const std::string &name, int from_tok) {
        auto it = d_->bound_from.find(name);
        if (it == d_->bound_from.end() || it->second > from_tok) d_->bound_from[name] = from_tok;
    }

    int AddBinding(const std::string &name, char kind, int decl, int func) {
        Binding b;
        b.name = name;
        b.kind = kind;
        b.decl_tok = decl;
        b.func = func;
        d_->bindings.push_back(b);
        return static_cast<int>(d_->bindings.size()) - 1;
    }

    void AddOcc(int tok, int binding, char how) {
        Occ o;
        o.tok = tok;
        o.binding = binding;
        o.how = how;
        o.func = Func();
        d_->tok_occ[tok] = static_cast<int>(d_->occs.size());
        d_->occs.push_back(o);
        Binding &b = d_->bindings[static_cast<size_t>(binding)];
        if (how == 'r') b.reads++;
        if (how == 'w') b.writes++;
        if (how == 'b') b.isbound++;
        if (how == 'u') b.unbinds++;
    }

    void Declare(int tok, char kind, int func) {
        if (tok < 0) return;
        const std::string &name = Tok(tok).name;
        const int b = AddBinding(name, kind, tok, func);
        scopes_.back()[name] = b;
        AddOcc(tok, b, 'd');
    }

    int Global(const std::string &name) {
        auto it = d_->globals.find(name);
        if (it != d_->globals.end()) return it->second;
        const int b = AddBinding(name, 'g', -1, -1);
        d_->globals[name] = b;
        return b;
    }

    void Ref(int tok, char how) {
        const std::string &name = Tok(tok).name;
        for (size_t i = scopes_.size(); i-- > 0;) {
            auto it = scopes_[i].find(name);
            if (it != scopes_[i].end()) {
                AddOcc(tok, it->second, how);
                return;
            }
        }
        AddOcc(tok, Global(name), how);
        if (how == 'w' && funcs_.empty()) {
            // A top-level assignment binds the name from its target on (GAP reads `f := function() ... f ... end`
            // without a warning).
            MarkBound(name, tok);
            d_->defs[name].push_back(tok);
        }
    }

    void StringName(int tok, const std::string &name) {
        AddOcc(tok, Global(name), 's');
        d_->defs[name].push_back(tok);
    }

    void WalkKids(int n, bool isbound, size_t from = 0) {
        const auto &kids = N(n).kids;
        for (size_t i = from; i < kids.size(); i++) Walk(kids[i], isbound);
    }

    // A record component or option name: an identifier is a name, not a variable; `(expr)` is evaluated.
    void ComponentName(int n, bool isbound) {
        const gapparse::Node &c = N(n);
        if (c.kind == K::Ident) {
            d_->fields.insert(Tok(c.tok).name);
        } else if (c.kind == K::Paren) {
            Walk(n, isbound);
        }
    }

    void Call(int n, bool isbound) {
        const gapparse::Node &c = N(n);
        if (c.kids.empty()) return;
        const int callee = c.kids[0];
        std::string cname;
        if (N(callee).kind == K::Ident) cname = Tok(N(callee).tok).name;
        Walk(callee, isbound);
        for (size_t i = 1; i < c.kids.size(); i++) {
            const int a = c.kids[i];
            const gapparse::Node &an = N(a);
            if (an.kind == K::Option) {
                if (!an.kids.empty()) ComponentName(an.kids[0], isbound);
                for (size_t j = 1; j < an.kids.size(); j++) Walk(an.kids[j], isbound);
                continue;
            }
            if (i == 1 && !cname.empty() && an.kind == K::String) {
                std::string s;
                if (StringValue(*d_, an.tok, &s) && ValidIdentifier(s)) {
                    if (const Binder *b = FindBinder(cname)) {
                        StringName(an.tok, s);
                        if (funcs_.empty())
                            for (const std::string &name : BoundBy(*b, s)) pending_binds_.push_back(name);
                        continue;
                    }
                    if (IsInstaller(cname)) {
                        StringName(an.tok, s);
                        continue;
                    }
                }
            }
            if (i == 3 && cname == "DeclareAutoreadableVariables" && an.kind == K::List) {
                // DeclareAutoreadableVariables(pkg, file, ["A", "B"]) binds A and B.
                for (int e : an.kids) {
                    std::string s;
                    if (N(e).kind == K::String && StringValue(*d_, N(e).tok, &s) && ValidIdentifier(s)) {
                        StringName(N(e).tok, s);
                        if (funcs_.empty()) pending_binds_.push_back(s);
                    }
                }
                continue;
            }
            if (i == 1 && IsInstaller(cname) && an.kind == K::Ident && funcs_.empty()) {
                Walk(a, isbound);
                d_->defs[Tok(an.tok).name].push_back(an.tok);
                continue;
            }
            Walk(a, isbound);
        }
    }

    void Function(int n) {
        const gapparse::Node &f = N(n);
        funcs_.push_back(n);
        scopes_.emplace_back();
        size_t i = 0;
        if (i < f.kids.size() && N(f.kids[i]).kind == K::Params) {
            for (int pr : N(f.kids[i]).kids) Declare(N(pr).tok, 'p', n);
            i++;
        }
        if (f.kind == K::Function && i < f.kids.size() && N(f.kids[i]).kind == K::Locals) {
            for (int id : N(f.kids[i]).kids) Declare(N(id).tok, 'l', n);
            i++;
        }
        for (; i < f.kids.size(); i++) Walk(f.kids[i], false);
        scopes_.pop_back();
        funcs_.pop_back();
    }

    void Walk(int n, bool isbound) {
        if (n < 0) return;
        const gapparse::Node &nd = N(n);
        switch (nd.kind) {
        case K::Ident:
            Ref(nd.tok, isbound ? 'b' : 'r');
            return;
        case K::Function:
        case K::Lambda:
            Function(n);
            return;
        case K::Assign: {
            if (nd.kids.empty()) return;
            const int target = nd.kids[0];
            if (N(target).kind == K::Ident) {
                Ref(N(target).tok, 'w');
            } else {
                Walk(target, false);
            }
            WalkKids(n, false, 1);
            return;
        }
        case K::For: {
            if (nd.kids.empty()) return;
            const int var = nd.kids[0];
            if (N(var).kind == K::Ident) {
                Ref(N(var).tok, 'w');
                const auto oi = d_->tok_occ.find(N(var).tok);
                if (oi != d_->tok_occ.end())
                    d_->bindings[static_cast<size_t>(d_->occs[static_cast<size_t>(oi->second)].binding)].loop_writes++;
            } else {
                Walk(var, false);
            }
            WalkKids(n, false, 1);
            return;
        }
        case K::IsBound:
            WalkKids(n, true);
            return;
        case K::Unbind:
            if (!nd.kids.empty() && N(nd.kids[0]).kind == K::Ident) {
                Ref(N(nd.kids[0]).tok, 'u');
                return;
            }
            WalkKids(n, false);
            return;
        case K::Field:
        case K::CompField:
            if (!nd.kids.empty()) Walk(nd.kids[0], isbound);
            if (nd.kids.size() > 1) ComponentName(nd.kids[1], isbound);
            return;
        case K::RecEntry:
            if (!nd.kids.empty()) ComponentName(nd.kids[0], isbound);
            WalkKids(n, isbound, 1);
            return;
        case K::Call:
            Call(n, isbound);
            return;
        case K::Locals:
            return;
        default:
            WalkKids(n, isbound);
            return;
        }
    }

    // A function's display name: the global or local it is assigned to, or the global an Install* call
    // gives it to.
    void NameFunctions() {
        const gapparse::Parse &p = d_->parse;
        for (size_t i = 0; i < p.nodes.size(); i++) {
            const gapparse::Node &nd = p.nodes[i];
            if (nd.kind == K::Assign && nd.kids.size() == 2) {
                const gapparse::Node &t = N(nd.kids[0]);
                const gapparse::Node &v = N(nd.kids[1]);
                if ((v.kind == K::Function || v.kind == K::Lambda) && t.first >= 0 && t.last >= t.first) {
                    std::string name;
                    for (int k = t.first; k <= t.last; k++) name += std::string(p.Spelling(k));
                    d_->func_name[nd.kids[1]] = name;
                }
            } else if (nd.kind == K::Call && nd.kids.size() >= 3 && N(nd.kids[0]).kind == K::Ident) {
                const std::string callee = Tok(N(nd.kids[0]).tok).name;
                if (!IsInstaller(callee)) continue;
                const gapparse::Node &a = N(nd.kids[1]);
                std::string name;
                if (a.kind == K::Ident) name = Tok(a.tok).name;
                if (a.kind == K::String && !StringValue(*d_, a.tok, &name)) name.clear();
                if (name.empty()) continue;
                for (size_t k = 2; k < nd.kids.size(); k++) {
                    const int f = nd.kids[k];
                    if (N(f).kind == K::Function || N(f).kind == K::Lambda) d_->func_name[f] = name;
                }
            }
        }
    }
};

// ---- token-level scope (for completion inside unfinished code) --------------------------

struct TokFrame {
    std::vector<std::string> names;
    bool lambda = false;
    int depth = 0;  // bracket depth a lambda lives at
};

// The parameter and local names visible at @p off, innermost first, from the tokens alone.
std::vector<std::string> NamesInScope(const Doc &d, uint32_t off) {
    const auto &toks = d.parse.tokens;
    std::vector<TokFrame> st;
    int depth = 0;
    auto pop_lambdas = [&](int dep, bool comma) {
        while (!st.empty() && st.back().lambda && (st.back().depth > dep || (comma && st.back().depth == dep)))
            st.pop_back();
    };
    for (size_t i = 0; i < toks.size() && toks[i].kind != T::Eof && toks[i].end <= off; i++) {
        const gapparse::Token &t = toks[i];
        switch (t.kind) {
        case T::Function: {
            TokFrame f;
            size_t j = i + 1;
            if (j < toks.size() && toks[j].kind == T::LParen) {
                for (j++; j < toks.size() && toks[j].kind != T::RParen && toks[j].kind != T::Eof; j++)
                    if (toks[j].kind == T::Ident) f.names.push_back(toks[j].name);
            }
            if (j + 1 < toks.size() && toks[j + 1].kind == T::Local) {
                for (j += 2; j < toks.size() && toks[j].kind != T::Semi && toks[j].kind != T::Eof; j++)
                    if (toks[j].kind == T::Ident) f.names.push_back(toks[j].name);
            }
            f.depth = depth;
            st.push_back(f);
            break;
        }
        case T::End:
            while (!st.empty() && st.back().lambda) st.pop_back();
            if (!st.empty()) st.pop_back();
            break;
        case T::Arrow: {
            TokFrame f;
            f.lambda = true;
            f.depth = depth;
            if (i > 0 && toks[i - 1].kind == T::Ident) {
                f.names.push_back(toks[i - 1].name);
            } else if (i > 0 && toks[i - 1].kind == T::RBrace) {
                for (size_t j = i - 1; j-- > 0 && toks[j].kind != T::LBrace;)
                    if (toks[j].kind == T::Ident) f.names.push_back(toks[j].name);
            }
            st.push_back(f);
            break;
        }
        case T::LParen: case T::LBracket: case T::LBrace: case T::BangBracket:
            depth++;
            break;
        case T::RParen: case T::RBracket: case T::RBrace:
            depth--;
            pop_lambdas(depth, false);
            break;
        case T::Comma:
            pop_lambdas(depth, true);
            break;
        case T::Semi: case T::DualSemi:
            pop_lambdas(-1, false);
            break;
        default:
            break;
        }
    }
    // File-level `local` (a file read with ReadAsFunction).
    std::vector<std::string> out;
    for (size_t i = st.size(); i-- > 0;)
        for (const std::string &n : st[i].names) out.push_back(n);
    if (!toks.empty() && toks[0].kind == T::Local) {
        for (size_t j = 1; j < toks.size() && toks[j].kind != T::Semi && toks[j].kind != T::Eof; j++)
            if (toks[j].kind == T::Ident) out.push_back(toks[j].name);
    }
    return out;
}

// ---- helpers for hover and signatures ---------------------------------------------------

std::string OneLine(std::string s, size_t max = 100) {
    for (char &c : s)
        if (c == '\n' || c == '\t' || c == '\r') c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    size_t a = s.find_first_not_of(' ');
    s = a == std::string::npos ? std::string() : s.substr(a);
    if (s.size() > max) s = s.substr(0, max - 3) + "...";
    return s;
}

// The `#` comment block right above line @p line (GAP's `##  ...` documentation style), without the marks.
std::string CommentAbove(const Doc &d, int line) {
    std::vector<std::string> got;
    for (int l = line - 1; l >= 0; l--) {
        const std::string &s = d.lines[static_cast<size_t>(l)];
        const size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos || s[a] != '#') break;
        size_t b = a;
        while (b < s.size() && s[b] == '#') b++;
        std::string body = s.substr(b);
        if (!body.empty() && body[0] == ' ') body.erase(0, 1);
        while (!body.empty() && body[0] == ' ') body.erase(0, 1);
        got.push_back(body);
        if (got.size() > 30) break;
    }
    std::reverse(got.begin(), got.end());
    // Drop rule lines (`#####`) and the leading/trailing blanks they leave.
    std::vector<std::string> keep;
    for (const std::string &g : got)
        if (!g.empty() || !keep.empty()) keep.push_back(g);
    while (!keep.empty() && keep.back().empty()) keep.pop_back();
    std::string out;
    for (const std::string &k : keep) out += k + "\n";
    return out;
}

// A function node's parameter list as written: "x, y, rest..."
std::string ParamList(const Doc &d, int fn) {
    const gapparse::Node &f = Nd(d, fn);
    if (f.kids.empty() || Nd(d, f.kids[0]).kind != K::Params) return "";
    std::string out;
    for (int pr : Nd(d, f.kids[0]).kids) {
        const gapparse::Node &p = Nd(d, pr);
        if (!out.empty()) out += ", ";
        if (p.qualifier >= 0) out += Tk(d, p.qualifier).name + " ";
        out += Tk(d, p.tok).name;
        if (p.variadic && Tk(d, p.tok).name != "arg") out += "...";
    }
    return out;
}

// The function node a global is defined as in this document (`f := function ...`,
// InstallGlobalFunction(f, function ...)), or -1.
int DefinedFunction(const Doc &d, const std::string &name) {
    for (const auto &kv : d.func_name)
        if (kv.second == name) return kv.first;
    return -1;
}

// The documented forms of a name, one per distinct (kind, args), library ("ref"/"tut") first.
std::vector<const GapVocabEntry *> Forms(const std::string &name) {
    const auto r = VocabRange(name);
    std::vector<const GapVocabEntry *> out;
    std::set<std::string> seen;
    for (int pass = 0; pass < 2; pass++) {
        for (const GapVocabEntry *e = r.first; e != r.second; e++) {
            const bool ref = std::strcmp(e->book, "ref") == 0 || std::strcmp(e->book, "tut") == 0;
            if ((pass == 0) != ref) continue;
            if (seen.insert(std::string(e->kind) + "(" + e->args).second) out.push_back(e);
        }
    }
    return out;
}

std::string VocabHover(const std::string &name) {
    const std::vector<const GapVocabEntry *> forms = Forms(name);
    if (forms.empty()) return "";
    std::string sig, books;
    std::set<std::string> book_set;
    size_t shown = 0;
    for (const GapVocabEntry *e : forms) {
        book_set.insert(e->book);
        if (shown >= 6) continue;
        std::string args = e->args;
        size_t s = 0;
        while (shown < 6) {
            const size_t bar = args.find('|', s);
            const std::string one = args.substr(s, bar == std::string::npos ? std::string::npos : bar - s);
            const std::string line = std::string(e->kind) == "Var" || std::string(e->kind) == "InfoClass" ||
                                             std::string(e->kind) == "Fam"
                                         ? name
                                         : name + "( " + one + (one.empty() ? ")" : " )");
            if (sig.find(line + "\n") == std::string::npos) {
                sig += line + "\n";
                shown++;
            }
            if (bar == std::string::npos) break;
            s = bar + 1;
        }
    }
    std::string out = "```gap\n" + sig + "```\n\n";
    out += std::string("GAP ") + KindWord(forms[0]->kind);
    for (const std::string &b : book_set) books += (books.empty() ? "" : ", ") + b;
    out += " -- documented in: " + books + " (`?" + name + "`)";
    return out;
}

}  // namespace

// ==== analysis ===========================================================================

std::shared_ptr<const GapLspDocument> GapLspAnalyze(const std::vector<std::string> &lines) {
    return GapLspAnalyze(lines, GapLspOptions{});
}

std::shared_ptr<const GapLspDocument> GapLspAnalyze(const std::vector<std::string> &lines, const GapLspOptions &opts) {
    auto doc = std::make_shared<GapLspDocument>();
    doc->lines = lines;
    if (doc->lines.empty()) doc->lines.emplace_back();
    for (size_t i = 0; i < doc->lines.size(); i++) {
        doc->line_start.push_back(static_cast<uint32_t>(doc->text.size()));
        if (i) {
            doc->line_start.back()++;
            doc->text += '\n';
        }
        doc->text += doc->lines[i];
    }
    doc->parse = gapparse::ParseGap(doc->text);
    doc->context = opts.context;
    doc->opaque = opts.opaque;
    Walker(doc.get()).Run();
    return doc;
}

std::vector<std::string> GapLspTopLevelNames(std::string_view text) {
    std::vector<gapparse::Token> toks;
    std::vector<gapparse::Comment> comments;
    gapparse::Tokenize(text, &toks, &comments);
    std::vector<std::string> out;
    int fdepth = 0;
    for (size_t i = 0; i < toks.size(); i++) {
        const gapparse::Token &t = toks[i];
        if (t.kind == T::Function) fdepth++;
        if (t.kind == T::End && fdepth > 0) fdepth--;
        if (fdepth > 0 || t.kind != T::Ident) continue;
        const bool after_dot = i > 0 && (toks[i - 1].kind == T::Dot || toks[i - 1].kind == T::BangDot);
        if (after_dot) continue;
        if (i + 1 < toks.size() && toks[i + 1].kind == T::Assign) {
            out.push_back(t.name);
            continue;
        }
        if (i > 0 && toks[i - 1].kind == T::For) {
            out.push_back(t.name);
            continue;
        }
        if (t.name == "DeclareAutoreadableVariables" && i + 1 < toks.size() && toks[i + 1].kind == T::LParen) {
            // The strings in its list argument.
            int depth = 0;
            for (size_t j = i + 1; j < toks.size() && toks[j].kind != T::Eof; j++) {
                const T k = toks[j].kind;
                if (k == T::LParen || k == T::LBracket) depth++;
                if (k == T::RParen || k == T::RBracket) {
                    if (--depth == 0) break;
                }
                if (k != T::String || depth != 2) continue;
                const std::string_view s = text.substr(toks[j].begin, toks[j].end - toks[j].begin);
                if (s.size() < 3 || s.front() != '"' || s.back() != '"') continue;
                const std::string name(s.substr(1, s.size() - 2));
                if (ValidIdentifier(name)) out.push_back(name);
            }
            continue;
        }
        if (i + 2 < toks.size() && toks[i + 1].kind == T::LParen && toks[i + 2].kind == T::String) {
            const Binder *b = FindBinder(t.name);
            if (!b) continue;
            const std::string_view s = text.substr(toks[i + 2].begin, toks[i + 2].end - toks[i + 2].begin);
            if (s.size() < 3 || s.front() != '"' || s.back() != '"') continue;
            const std::string name(s.substr(1, s.size() - 2));
            if (!ValidIdentifier(name)) continue;
            for (const std::string &n : BoundBy(*b, name)) out.push_back(n);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ==== diagnostics ========================================================================

namespace {

// GAP's "Syntax warning: Unbound global variable", decided as GAP's reader decides it: inside a function
// (or lambda), a global that is neither read inside `IsBound(...)` nor bound when the function is read.
// Bound when read: a name GAP binds at start-up, one the rest of the package binds (the server passes
// those), one assigned at the top level earlier in reading order (the target of the assignment being read
// counts: `f := function() ... f ... end` is fine), or one a library call (DeclareGlobalFunction, BindGlobal,
// ...) bound in an earlier statement. Names documented anywhere in GAP's manuals count as bound too: a
// package the file loads binds them.
// HPC-GAP's kernel functions: bound in an HPC-GAP build, which GAP's start-up list (a plain build) lacks.
bool HpcGapName(const std::string &name) {
    static const std::set<std::string> names = {
        "AdoptObj", "AdoptSingleObj", "AtThreadExit", "CreateBarrier", "CreateChannel", "CreateSemaphore",
        "CreateSyncVar", "CreateThread", "CurrentThread", "DeserializeNativeString", "DestroyChannel",
        "HaveReadAccess", "HaveWriteAccess", "InspectChannel", "IsPublic", "IsShared", "IsThreadLocal",
        "KillThread", "LOCK", "MakeReadOnlyObj", "MakeReadOnlySingleObj", "MakeThreadLocal", "MigrateObj",
        "MigrateSingleObj", "MultiReceiveChannel", "MultiSendChannel", "NewRegion", "PauseThread", "READ_LOCK",
        "ReceiveAnyChannel", "ReceiveChannel", "RegionOf", "ResumeThread", "SendChannel",
        "SerializeToNativeString", "ShareObj", "ShareSingleObj", "SignalSemaphore", "StartBarrier", "SyncRead",
        "SyncWrite", "ThreadID", "TryReceiveChannel", "TrySendChannel", "TryWaitSemaphore", "UNLOCK",
        "WRITE_LOCK", "WaitBarrier", "WaitSemaphore", "WaitThread"};
    return names.count(name) > 0;
}

bool BoundPlain(const Doc &d, const std::string &name) {
    return KnownGlobal(name) || HpcGapName(name) || Documented(name) || (d.context && d.context->count(name));
}

bool Bound(const Doc &d, const std::string &name) {
    // An escaped operator or keyword name (`\<`, `\mod`): GAP binds those itself. A namespaced name
    // (`Helper@MyPkg`, `Helper@`) belongs to a package's private namespace.
    for (char c : name)
        if (!IsIdentChar(static_cast<unsigned char>(c)) || c == '@') return true;
    if (IsKeywordName(name)) return true;
    if (BoundPlain(d, name)) return true;
    // HasX / SetX come with the attribute or property X, wherever that is declared.
    if (name.size() > 3 && (name.compare(0, 3, "Has") == 0 || name.compare(0, 3, "Set") == 0) &&
        std::isupper(static_cast<unsigned char>(name[3])))
        return BoundPlain(d, name.substr(3));
    return false;
}

void UnboundGlobals(const Doc &d, std::vector<GapLspDiagnostic> *out) {
    std::map<std::string, int> later;  // bound later in the file: the line
    for (const auto &kv : d.bound_from) later[kv.first] = kv.second;
    size_t reported = 0;
    for (const Occ &o : d.occs) {
        if (o.func < 0 || o.binding < 0) continue;
        if (o.how != 'r' && o.how != 'w' && o.how != 'u') continue;
        const Binding &b = d.bindings[static_cast<size_t>(o.binding)];
        if (b.kind != 'g') continue;
        const auto bf = d.bound_from.find(b.name);
        if (bf != d.bound_from.end() && bf->second <= o.tok) continue;
        if (d.opaque || Bound(d, b.name)) continue;
        GapLspDiagnostic g;
        g.range = TokRange(d, o.tok);
        g.code = "unbound-global";
        if (bf != d.bound_from.end()) {
            int line = 0, col = 0;
            PosOf(d, Tk(d, bf->second < static_cast<int>(d.parse.tokens.size()) ? bf->second : o.tok).begin, &line,
                  &col);
            g.severity = GapLspSeverity::Information;
            g.message = "unbound global variable '" + b.name + "' when this function is read: it is bound later, at line " +
                        std::to_string(line + 1) + " (GAP warns; declare it first with DeclareGlobalFunction or move "
                        "the definition up)";
        } else {
            g.severity = GapLspSeverity::Warning;
            g.message = "unbound global variable '" + b.name + "'";
            if (o.how == 'w') g.message += " (assigned in a function but not declared local)";
        }
        out->push_back(g);
        if (++reported >= 500) return;
    }
}

void Locals(const Doc &d, std::vector<GapLspDiagnostic> *out) {
    for (const Binding &b : d.bindings) {
        if (b.kind != 'l' || b.decl_tok < 0) continue;
        GapLspDiagnostic g;
        g.range = TokRange(d, b.decl_tok);
        if (b.reads == 0 && b.writes == 0 && b.isbound == 0 && b.unbinds == 0) {
            g.severity = GapLspSeverity::Hint;
            g.code = "unused-local";
            g.message = "local '" + b.name + "' is never used";
            g.unnecessary = true;
        } else if (b.reads == 0 && b.isbound == 0 && b.writes > b.loop_writes) {
            g.severity = GapLspSeverity::Hint;
            g.code = "unused-local";
            g.message = "local '" + b.name + "' is assigned but never read";
            g.unnecessary = true;
        } else if (b.writes == 0 && b.reads > 0) {
            g.severity = GapLspSeverity::Warning;
            g.code = "unassigned-local";
            g.message = "local '" + b.name + "' is read but never assigned (\"Variable: '" + b.name +
                        "' must have an assigned value\")";
        } else {
            continue;
        }
        out->push_back(g);
    }
}

}  // namespace

std::vector<GapLspDiagnostic> GapLspDiagnostics(const GapLspDocument &doc) {
    std::vector<GapLspDiagnostic> out;
    for (size_t i = 0; i < doc.parse.errors.size(); i++) {
        const gapparse::Error &e = doc.parse.errors[i];
        GapLspDiagnostic g;
        g.code = "syntax-error";
        g.severity = GapLspSeverity::Error;
        g.message = e.message;
        const uint32_t off = std::min(e.offset, static_cast<uint32_t>(doc.text.size()));
        uint32_t end = off;
        const int t = TokenAt(doc, off);
        if (t >= 0 && Tk(doc, t).begin == off) end = Tk(doc, t).end;
        if (end == off && end < doc.text.size() && doc.text[end] != '\n') end++;
        g.range = RangeOf(doc, off, end);
        out.push_back(g);
    }
    Locals(doc, &out);
    UnboundGlobals(doc, &out);
    std::stable_sort(out.begin(), out.end(), [](const GapLspDiagnostic &a, const GapLspDiagnostic &b) {
        return a.range.line != b.range.line ? a.range.line < b.range.line : a.range.col < b.range.col;
    });
    return out;
}

std::string GapLspFormatDiagnostic(const std::string &path, const GapLspDiagnostic &d) {
    const char *sev = d.severity == GapLspSeverity::Error         ? "error"
                      : d.severity == GapLspSeverity::Warning     ? "warning"
                      : d.severity == GapLspSeverity::Information ? "info"
                                                                  : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool GapLspDiagnosticFails(const GapLspDiagnostic &d) {
    return d.severity == GapLspSeverity::Error || d.severity == GapLspSeverity::Warning;
}

// ==== completion =========================================================================

std::vector<GapLspCompletionItem> GapLspCompletions(const GapLspDocument &doc, int line, int col) {
    std::vector<GapLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return out;
    const std::string &cur = doc.lines[static_cast<size_t>(line)];
    col = std::max(0, std::min(col, static_cast<int>(cur.size())));
    int start = col;
    while (start > 0 && IsIdentChar(static_cast<unsigned char>(cur[static_cast<size_t>(start - 1)]))) start--;
    const std::string prefix = cur.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    // Not inside a comment or a string.
    const uint32_t off = OffsetOf(doc, line, start);
    for (const gapparse::Comment &c : doc.parse.comments)
        if (c.begin <= off && off < c.end) return out;
    {
        const int t = TokenAt(doc, off);
        if (t >= 0 && (Tk(doc, t).kind == T::String || Tk(doc, t).kind == T::Char) && Tk(doc, t).begin < off &&
            off < Tk(doc, t).end)
            return out;
    }
    std::set<std::string> seen;
    auto add = [&](const std::string &label, GapLspKind kind, const std::string &detail, const std::string &docs) {
        if (label == prefix && kind != GapLspKind::Keyword) return;
        if (!seen.insert(label).second) return;
        GapLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = docs;
        it.replace_start = start;
        it.replace_end = col;
        out.push_back(it);
    };
    auto matches = [&](const std::string &s) { return s.compare(0, prefix.size(), prefix) == 0; };
    // After `r.` / `r!.`: record components the document uses.
    const bool after_dot = start > 0 && cur[static_cast<size_t>(start - 1)] == '.' &&
                           !(start > 1 && cur[static_cast<size_t>(start - 2)] == '.');
    if (after_dot) {
        for (const std::string &f : doc.fields)
            if (matches(f)) add(f, GapLspKind::Field, "record component", "");
        return out;
    }
    for (const std::string &n : NamesInScope(doc, off))
        if (matches(n)) add(n, GapLspKind::Variable, "local", "");
    static const char *const kw[] = {"function", "local", "return", "if", "then", "elif", "else", "fi", "for",
                                     "in", "do", "od", "while", "repeat", "until", "end", "rec", "and", "or",
                                     "not", "mod", "true", "false", "break", "continue", "IsBound", "Unbind",
                                     "TryNextMethod", "Info", "Assert", "atomic", "readonly", "readwrite", "quit"};
    if (!prefix.empty())
        for (const char *k : kw)
            if (matches(k)) add(k, GapLspKind::Keyword, "keyword", "");
    // Globals this document defines.
    for (const auto &kv : doc.defs) {
        if (!matches(kv.first)) continue;
        const int fn = DefinedFunction(doc, kv.first);
        add(kv.first, fn >= 0 ? GapLspKind::Function : GapLspKind::Variable,
            fn >= 0 ? "function( " + ParamList(doc, fn) + " )" : "defined in this file", "");
    }
    if (prefix.empty()) return out;
    // GAP's documented names, then the rest of its start-up globals.
    size_t added = 0;
    const GapVocabEntry *end = kGapVocab + kGapVocabSize;
    const GapVocabEntry *it = std::lower_bound(kGapVocab, end, prefix.c_str(), [](const GapVocabEntry &e, const char *p) {
        return std::strcmp(e.name, p) < 0;
    });
    for (; it != end && matches(it->name) && added < 200; it++) {
        if (seen.count(it->name)) continue;
        std::string args = it->args;
        const size_t bar = args.find('|');
        if (bar != std::string::npos) args = args.substr(0, bar);
        const std::string kind = it->kind;
        const std::string detail = kind == "Var" || kind == "InfoClass" || kind == "Fam"
                                       ? std::string(KindWord(kind))
                                       : std::string(it->name) + "( " + args + " )";
        add(it->name, CompletionKindOf(it->kind), detail, VocabHover(it->name));
        added++;
    }
    const char *const *g_end = kGapGlobals + kGapGlobalsSize;
    const char *const *g = std::lower_bound(kGapGlobals, g_end, prefix.c_str(),
                                            [](const char *a, const char *b) { return std::strcmp(a, b) < 0; });
    for (; g != g_end && matches(*g) && added < 300; g++) {
        if (seen.count(*g)) continue;
        add(*g, GapLspKind::Variable, "GAP global", "");
        added++;
    }
    return out;
}

// ==== hover ==============================================================================

namespace {

std::string FuncLabel(const Doc &d, int fn) {
    if (fn == 0) return "the file";
    const auto it = d.func_name.find(fn);
    int line = 0, col = 0;
    PosOf(d, Tk(d, Nd(d, fn).first).begin, &line, &col);
    if (it != d.func_name.end()) return "`" + it->second + "`";
    return "the function at line " + std::to_string(line + 1);
}

// The source line a token sits on, trimmed.
std::string LineText(const Doc &d, int tok) {
    int line = 0, col = 0;
    PosOf(d, Tk(d, tok).begin, &line, &col);
    return OneLine(d.lines[static_cast<size_t>(line)], 120);
}

}  // namespace

GapLspHoverInfo GapLspHover(const GapLspDocument &doc, int line, int col) {
    GapLspHoverInfo h;
    const int t = TokenAt(doc, OffsetOf(doc, line, col));
    if (t < 0 || Tk(doc, t).kind != T::Ident) return h;
    const std::string &name = Tk(doc, t).name;
    const auto oi = doc.tok_occ.find(t);
    h.range = TokRange(doc, t);
    if (oi != doc.tok_occ.end()) {
        const Occ &o = doc.occs[static_cast<size_t>(oi->second)];
        const Binding &b = doc.bindings[static_cast<size_t>(o.binding)];
        if (b.kind == 'p' || b.kind == 'l') {
            h.found = true;
            h.text = std::string("```gap\n") + (b.kind == 'p' ? "(parameter) " : "local ") + name + "\n```\n\n" +
                     (b.kind == 'p' ? "Parameter of " : "Local variable of ") + FuncLabel(doc, b.func) + ".";
            return h;
        }
        // A global: what this file says about it, then GAP's manuals.
        std::string text;
        const auto di = doc.defs.find(name);
        if (di != doc.defs.end() && !di->second.empty()) {
            const int fn = DefinedFunction(doc, name);
            const int def = di->second.front();
            int dl = 0, dc = 0;
            PosOf(doc, Tk(doc, def).begin, &dl, &dc);
            text += "```gap\n" + (fn >= 0 ? name + " := function( " + ParamList(doc, fn) + " )" : LineText(doc, def)) +
                    "\n```\n\n";
            const std::string above = CommentAbove(doc, dl);
            if (!above.empty()) text += above + "\n";
            text += "Defined in this file, line " + std::to_string(dl + 1) + ".";
        }
        const std::string vocab = VocabHover(name);
        if (!vocab.empty()) text += (text.empty() ? "" : "\n\n") + vocab;
        if (text.empty() && KnownGlobal(name)) text = "```gap\n" + name + "\n```\n\nA global variable of GAP's library.";
        if (text.empty()) return h;
        h.found = true;
        h.text = text;
        return h;
    }
    // A record component or option name.
    if (doc.fields.count(name)) {
        h.found = true;
        h.text = "Record component `" + name + "`.";
    }
    return h;
}

// ==== signature help =====================================================================

GapLspSignatureHelp GapLspSignatureAt(const GapLspDocument &doc, int line, int col) {
    GapLspSignatureHelp sh;
    const uint32_t off = OffsetOf(doc, line, col);
    const auto &toks = doc.parse.tokens;
    // The innermost unclosed `(` before the cursor, with a name before it.
    int depth = 0, commas = 0;
    int open = -1;
    int i = static_cast<int>(toks.size()) - 1;
    while (i >= 0 && (toks[static_cast<size_t>(i)].kind == T::Eof || toks[static_cast<size_t>(i)].begin >= off)) i--;
    for (; i >= 0; i--) {
        const T k = toks[static_cast<size_t>(i)].kind;
        if (k == T::RParen || k == T::RBracket || k == T::RBrace) {
            depth++;
        } else if (k == T::LParen || k == T::LBracket || k == T::LBrace || k == T::BangBracket) {
            if (depth == 0) {
                if (k == T::LParen) open = i;
                break;
            }
            depth--;
        } else if (k == T::Comma && depth == 0) {
            commas++;
        } else if ((k == T::Semi || k == T::Then || k == T::Do || k == T::End || k == T::Local) && depth == 0) {
            break;
        } else if (k == T::Colon && depth == 0) {
            return sh;  // among the options
        }
    }
    if (open <= 0 || toks[static_cast<size_t>(open - 1)].kind != T::Ident) return sh;
    const std::string name = toks[static_cast<size_t>(open - 1)].name;
    // Split an argument list such as "G, H[, opt]" into its names.
    auto build = [&](const std::string &args) {
        sh.found = true;
        sh.label = name + "( " + args + " )";
        sh.params.clear();
        std::string cur;
        auto flush = [&]() {
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            size_t a = cur.find_first_not_of(' ');
            if (a != std::string::npos && !cur.substr(a).empty()) sh.params.push_back(cur.substr(a));
            cur.clear();
        };
        for (char c : args) {
            if (c == ',' || c == '[' || c == ']') {
                flush();
            } else {
                cur += c;
            }
        }
        flush();
        sh.active_param = std::min(commas, std::max(0, static_cast<int>(sh.params.size()) - 1));
    };
    const int fn = DefinedFunction(doc, name);
    if (fn >= 0) {
        build(ParamList(doc, fn));
        sh.documentation = "Defined in this file.";
        return sh;
    }
    const std::vector<const GapVocabEntry *> forms = Forms(name);
    if (forms.empty()) return sh;
    // The documented form with enough arguments, library forms first.
    std::string best;
    size_t best_n = 0;
    bool have = false;
    for (const GapVocabEntry *e : forms) {
        std::string args = e->args;
        size_t s = 0;
        while (true) {
            const size_t bar = args.find('|', s);
            const std::string one = args.substr(s, bar == std::string::npos ? std::string::npos : bar - s);
            const size_t n = static_cast<size_t>(std::count(one.begin(), one.end(), ',')) + (one.empty() ? 0 : 1);
            if (!have || (best_n <= static_cast<size_t>(commas) && n > best_n)) {
                best = one;
                best_n = n;
                have = true;
            }
            if (bar == std::string::npos) break;
            s = bar + 1;
        }
    }
    build(best);
    sh.documentation = std::string("GAP ") + KindWord(forms[0]->kind) + " (" + forms[0]->book + ")";
    return sh;
}

// ==== navigation =========================================================================

namespace {

const Occ *OccAt(const Doc &d, int line, int col, int *tok_out = nullptr) {
    const int t = TokenAt(d, OffsetOf(d, line, col));
    if (t < 0) return nullptr;
    const auto it = d.tok_occ.find(t);
    if (it == d.tok_occ.end()) return nullptr;
    if (tok_out) *tok_out = t;
    return &d.occs[static_cast<size_t>(it->second)];
}

// The range of an occurrence: a string occurrence is the text inside its quotes.
GapLspRange OccRange(const Doc &d, const Occ &o) {
    const gapparse::Token &t = Tk(d, o.tok);
    if (o.how == 's') return RangeOf(d, t.begin + 1, t.end - 1);
    return RangeOf(d, t.begin, t.end);
}

}  // namespace

std::vector<GapLspLocation> GapLspDefinition(const GapLspDocument &doc, int line, int col) {
    std::vector<GapLspLocation> out;
    const Occ *o = OccAt(doc, line, col);
    if (!o) return out;
    const Binding &b = doc.bindings[static_cast<size_t>(o->binding)];
    if (b.kind != 'g') {
        GapLspLocation l;
        l.range = TokRange(doc, b.decl_tok);
        out.push_back(l);
        return out;
    }
    const auto it = doc.defs.find(b.name);
    if (it == doc.defs.end()) return out;
    for (int t : it->second) {
        const auto oi = doc.tok_occ.find(t);
        GapLspLocation l;
        l.range = oi != doc.tok_occ.end() ? OccRange(doc, doc.occs[static_cast<size_t>(oi->second)]) : TokRange(doc, t);
        out.push_back(l);
    }
    return out;
}

std::vector<GapLspReference> GapLspReferences(const GapLspDocument &doc, int line, int col) {
    std::vector<GapLspReference> out;
    const Occ *o = OccAt(doc, line, col);
    if (!o) return out;
    const Binding &b = doc.bindings[static_cast<size_t>(o->binding)];
    std::set<int> def_toks;
    if (b.kind == 'g') {
        const auto it = doc.defs.find(b.name);
        if (it != doc.defs.end()) def_toks.insert(it->second.begin(), it->second.end());
    }
    for (const Occ &x : doc.occs) {
        if (x.binding != o->binding) continue;
        GapLspReference r;
        r.range = OccRange(doc, x);
        r.is_definition = x.how == 'd' || def_toks.count(x.tok) > 0;
        out.push_back(r);
    }
    return out;
}

GapLspRenameTarget GapLspPrepareRename(const GapLspDocument &doc, int line, int col) {
    GapLspRenameTarget r;
    const Occ *o = OccAt(doc, line, col);
    if (!o) return r;
    const Binding &b = doc.bindings[static_cast<size_t>(o->binding)];
    if (b.kind == 'g' && (doc.defs.find(b.name) == doc.defs.end() || KnownGlobal(b.name))) return r;
    r.found = true;
    r.range = OccRange(doc, *o);
    r.placeholder = b.name;
    return r;
}

std::vector<GapLspTextEdit> GapLspRename(const GapLspDocument &doc, int line, int col, const std::string &new_name,
                                         std::string *error) {
    std::vector<GapLspTextEdit> edits;
    const Occ *o = OccAt(doc, line, col);
    if (!o) {
        *error = "no variable here";
        return edits;
    }
    const Binding &b = doc.bindings[static_cast<size_t>(o->binding)];
    if (b.kind == 'g' && (doc.defs.find(b.name) == doc.defs.end() || KnownGlobal(b.name))) {
        *error = "'" + b.name + "' is not defined in this file";
        return edits;
    }
    if (!ValidIdentifier(new_name)) {
        *error = "'" + new_name + "' is not a GAP identifier";
        return edits;
    }
    if (new_name == b.name) return edits;
    // Refuse a name that would capture or be captured: any other binding of that name where the renamed
    // variable is visible (a local or parameter of the same or an enclosing/nested function, or a global
    // mentioned inside its function).
    for (size_t i = 0; i < doc.bindings.size(); i++) {
        const Binding &x = doc.bindings[i];
        if (x.name != new_name || static_cast<int>(i) == o->binding) continue;
        if (b.kind == 'g' || x.kind == 'g') {
            *error = "'" + new_name + "' is already used in this file";
            return edits;
        }
        // Two locals: fine only when their functions do not nest.
        auto within = [&](int inner, int outer) {
            if (outer == 0) return true;
            const gapparse::Node &on = Nd(doc, outer);
            const gapparse::Node &in = Nd(doc, inner == 0 ? 0 : inner);
            return inner != 0 && in.first >= on.first && in.last <= on.last;
        };
        if (within(x.func, b.func) || within(b.func, x.func)) {
            *error = "'" + new_name + "' is already a variable there";
            return edits;
        }
    }
    for (const Occ &x : doc.occs) {
        if (x.binding != o->binding) continue;
        GapLspTextEdit e;
        e.range = OccRange(doc, x);
        e.new_text = new_name;
        edits.push_back(e);
    }
    return edits;
}

// ==== outline and folding ================================================================

namespace {

void SymbolsIn(const Doc &d, int parent_stmt, std::vector<GapLspSymbol> *out) {
    for (int st : Nd(d, parent_stmt).kids) {
        const gapparse::Node &n = Nd(d, st);
        if (n.first < 0 || n.last < n.first) continue;
        const GapLspRange whole = RangeOf(d, Tk(d, n.first).begin, Tk(d, n.last).end);
        if (n.kind == K::Assign && n.kids.size() == 2 && Nd(d, n.kids[0]).kind == K::Ident) {
            const gapparse::Node &v = Nd(d, n.kids[1]);
            GapLspSymbol s;
            s.name = Tk(d, Nd(d, n.kids[0]).tok).name;
            s.range = whole;
            s.sel_range = TokRange(d, Nd(d, n.kids[0]).tok);
            if (v.kind == K::Function || v.kind == K::Lambda) {
                s.kind = GapLspSymbolKind::Function;
                s.detail = "function( " + ParamList(d, n.kids[1]) + " )";
            } else if (v.kind == K::Rec) {
                s.kind = GapLspSymbolKind::Object;
            } else if (v.kind == K::List || v.kind == K::Range) {
                s.kind = GapLspSymbolKind::Array;
            } else {
                s.kind = GapLspSymbolKind::Variable;
            }
            out->push_back(s);
            continue;
        }
        if ((n.kind == K::CallStat || n.kind == K::ExprStat) && !n.kids.empty() &&
            Nd(d, n.kids[0]).kind == K::Call) {
            const gapparse::Node &c = Nd(d, n.kids[0]);
            if (c.kids.size() < 2 || Nd(d, c.kids[0]).kind != K::Ident) continue;
            const std::string callee = Tk(d, Nd(d, c.kids[0]).tok).name;
            const gapparse::Node &a = Nd(d, c.kids[1]);
            std::string name;
            int sel = a.tok;
            if (a.kind == K::String) {
                if (!StringValue(d, a.tok, &name)) continue;
            } else if (a.kind == K::Ident) {
                name = Tk(d, a.tok).name;
            } else {
                continue;
            }
            GapLspSymbol s;
            s.name = name;
            s.range = whole;
            s.sel_range = TokRange(d, sel);
            if (const Binder *b = FindBinder(callee)) {
                if (a.kind != K::String) continue;
                s.kind = b->kind;
                s.detail = callee;
            } else if (IsInstaller(callee)) {
                const bool method = callee.find("Method") != std::string::npos;
                s.kind = method ? GapLspSymbolKind::Method : GapLspSymbolKind::Function;
                s.detail = callee;
                if (method && c.kids.size() > 2 && Nd(d, c.kids[2]).kind == K::String) {
                    std::string info;
                    if (StringValue(d, Nd(d, c.kids[2]).tok, &info)) s.detail = info;
                }
            } else {
                continue;
            }
            out->push_back(s);
            continue;
        }
        if (n.kind == K::If || n.kind == K::Elif || n.kind == K::Else || n.kind == K::For || n.kind == K::While ||
            n.kind == K::Repeat)
            SymbolsIn(d, st, out);
    }
}

}  // namespace

std::vector<GapLspSymbol> GapLspSymbols(const GapLspDocument &doc) {
    std::vector<GapLspSymbol> out;
    if (doc.parse.nodes.empty()) return out;
    SymbolsIn(doc, 0, &out);
    return out;
}

std::vector<GapLspFold> GapLspFolds(const GapLspDocument &doc) {
    std::vector<GapLspFold> out;
    for (size_t i = 1; i < doc.parse.nodes.size(); i++) {
        const gapparse::Node &n = doc.parse.nodes[i];
        if (n.first < 0 || n.last < n.first) continue;
        switch (n.kind) {
        case K::Function: case K::If: case K::For: case K::While: case K::Repeat: case K::Atomic: case K::Rec:
        case K::List: case K::Elif: case K::Else:
            break;
        default:
            continue;
        }
        const int a = Tk(doc, n.first).line;
        int b = Tk(doc, n.last).line;
        if (n.kind == K::Elif || n.kind == K::Else) {
            // Up to the line before the next branch / `fi`.
            const int next = n.last + 1;
            if (next < static_cast<int>(doc.parse.tokens.size())) b = Tk(doc, next).line - 1;
        } else if (n.kind == K::If && n.split > 0 && n.split < static_cast<int>(n.kids.size())) {
            // The if's own fold ends before its first elif/else.
            const gapparse::Node &br = Nd(doc, n.kids[static_cast<size_t>(n.split)]);
            if (br.first >= 0) b = Tk(doc, br.first).line - 1;
        }
        if (b > a) {
            GapLspFold f;
            f.start_line = a;
            f.end_line = b;
            out.push_back(f);
        }
    }
    // Runs of comment-only lines.
    int run_start = -1, prev = -2;
    auto flush = [&]() {
        if (run_start >= 0 && prev > run_start) {
            GapLspFold f;
            f.start_line = run_start;
            f.end_line = prev;
            f.kind = "comment";
            out.push_back(f);
        }
    };
    for (const gapparse::Comment &c : doc.parse.comments) {
        const std::string &l = doc.lines[static_cast<size_t>(c.line)];
        const bool alone = l.find_first_not_of(" \t") == static_cast<size_t>(c.col);
        if (!alone) continue;
        if (c.line != prev + 1) {
            flush();
            run_start = c.line;
        }
        prev = c.line;
    }
    flush();
    std::sort(out.begin(), out.end(), [](const GapLspFold &x, const GapLspFold &y) {
        return x.start_line != y.start_line ? x.start_line < y.start_line : x.end_line > y.end_line;
    });
    return out;
}

// ==== formatting =========================================================================

std::vector<GapLspTextEdit> GapLspFormat(const std::vector<std::string> &lines, int width, std::string *error) {
    std::vector<GapLspTextEdit> edits;
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    gapfmt::Options o;
    if (width > 0) o.width = width;
    const gapfmt::Result r = gapfmt::Format(text, o);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    // The buffer's lines carry no final newline of their own; compare without it.
    std::string want = r.text;
    if (!want.empty() && want.back() == '\n' && (text.empty() || text.back() != '\n')) want.pop_back();
    if (want == text) return edits;
    std::vector<std::string> b;
    {
        size_t i = 0;
        while (true) {
            const size_t j = want.find('\n', i);
            if (j == std::string::npos) {
                b.push_back(want.substr(i));
                break;
            }
            b.push_back(want.substr(i, j - i));
            i = j + 1;
        }
    }
    // One edit over the lines that changed.
    size_t pre = 0;
    while (pre < lines.size() && pre < b.size() && lines[pre] == b[pre]) pre++;
    size_t suf = 0;
    while (suf < lines.size() - pre && suf < b.size() - pre && lines[lines.size() - 1 - suf] == b[b.size() - 1 - suf])
        suf++;
    GapLspTextEdit e;
    e.range.line = static_cast<int>(pre);
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
        if (pre > 0 && pre == lines.size() && b_end > pre) {
            e.range.line = static_cast<int>(pre) - 1;
            e.range.col = static_cast<int>(lines[pre - 1].size());
            e.new_text = "\n" + e.new_text;
        }
    }
    edits.push_back(e);
    return edits;
}
