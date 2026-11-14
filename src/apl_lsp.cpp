// mep's APL language server, analysis half: see apl_lsp.h.
//
// One pass over apl_lex.h's tokens, line by line, keeps the stacks APL's
// structure needs -- open tradfn, :Namespace/:Class scripts, Dyalog control
// structures, brackets and braces (dfns) -- and records the definitions,
// folds and diagnostics on the way. Expressions are not parsed further:
// without knowing which names are functions APL can't be, and nothing here
// needs it.
#include "apl_lsp.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "apl_format.h"
#include "apl_lex.h"
#include "apl_lsp_vocab.h"

using apl::Tok;
using apl::Token;

class AplLspDocument {
public:
    std::vector<std::string> lines;
    std::string path;
    std::vector<std::vector<Token>> toks;  // per line
    std::vector<AplDef> defs;
    std::vector<AplLspDiagnostic> diags;
    std::vector<AplLspFold> folds;
    // Lines inside a dfn's braces (for ⍺/⍵ hovers), per dfn: [start, end] positions.
    struct Span {
        int line, col, end_line, end_col;
        int def;  // dfn definition index, or -1 for an anonymous dfn
    };
    std::vector<Span> dfns;
};

namespace {

std::string Upper(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool EndsWith(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string Lower(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string Extension(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
    return Lower(path.substr(dot + 1));
}

std::string Trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

bool IsCode(const Token &t) { return t.kind != Tok::Comment; }

// The line's tokens without its comment.
std::vector<Token> CodeOf(const std::vector<Token> &line) {
    std::vector<Token> out;
    for (const Token &t : line) {
        if (IsCode(t)) out.push_back(t);
    }
    return out;
}

bool CommentOnly(const std::vector<Token> &line) { return line.size() == 1 && line[0].kind == Tok::Comment; }

// The text of a ⍝ comment without the ⍝ and one following space.
std::string CommentText(const Token &t) {
    std::string s = t.text;
    if (s.rfind("⍝", 0) == 0) s = s.substr(std::string("⍝").size());
    else if (s.rfind("#!", 0) == 0) s = s.substr(2);
    if (!s.empty() && s[0] == ' ') s = s.substr(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
    return s;
}

// Joins comment lines as Markdown: lines kept as written (the generator
// writes Markdown), a bare ⍝ is a paragraph break.
std::string DocOfLines(const std::vector<std::string> &lines) {
    std::vector<std::string> l = lines;
    while (!l.empty() && Trim(l.front()).empty()) l.erase(l.begin());
    while (!l.empty() && Trim(l.back()).empty()) l.pop_back();
    std::string out;
    for (size_t i = 0; i < l.size(); i++) {
        if (i) out += '\n';
        out += l[i];
    }
    return out;
}

const char *KindName(AplDefKind k) {
    switch (k) {
    case AplDefKind::Tradfn: return "tradfn";
    case AplDefKind::TradOp: return "operator (tradfn)";
    case AplDefKind::Dfn: return "dfn";
    case AplDefKind::Dop: return "dop (dfn operator)";
    case AplDefKind::Namespace: return "namespace";
    case AplDefKind::Class: return "class";
    case AplDefKind::Variable: return "variable";
    case AplDefKind::Label: return "label";
    }
    return "";
}

bool IsFunctionKind(AplDefKind k) {
    return k == AplDefKind::Tradfn || k == AplDefKind::TradOp || k == AplDefKind::Dfn || k == AplDefKind::Dop;
}

AplLspRange RangeOf(int line, const Token &t) { return AplLspRange{line, t.col, line, t.end}; }

struct CtlEntry {
    std::string word;  // upper-case, without ':' (IF, FOR, ...)
    int line = 0;
    Token tok;
    int def = -1;  // namespace/class definition index
};

// The control words that open a block, and the end word each takes.
const std::map<std::string, std::string> &Openers() {
    static const std::map<std::string, std::string> m = {
        {"IF", "ENDIF"},       {"FOR", "ENDFOR"},           {"WHILE", "ENDWHILE"},
        {"REPEAT", "ENDREPEAT"}, {"SELECT", "ENDSELECT"},   {"TRAP", "ENDTRAP"},
        {"WITH", "ENDWITH"},   {"HOLD", "ENDHOLD"},         {"SECTION", "ENDSECTION"},
        {"PROPERTY", "ENDPROPERTY"}, {"DISPOSABLE", "ENDDISPOSABLE"},
        {"NAMESPACE", "ENDNAMESPACE"}, {"CLASS", "ENDCLASS"}, {"INTERFACE", "ENDINTERFACE"},
    };
    return m;
}

class Analyzer {
public:
    explicit Analyzer(AplLspDocument &d) : d_(d) {}

    void Run() {
        const size_t n = d_.lines.size();
        d_.toks.resize(n);
        for (size_t i = 0; i < n; i++) d_.toks[i] = apl::LexLine(d_.lines[i]);
        const std::string ext = Extension(d_.path);
        int first = 0;
        if (ext == "aplf" || ext == "aplo") first = StartFileFunction();
        for (int li = first; li < static_cast<int>(n); li++) Line(li);
        EndOfDocument();
        CommentFolds();
    }

private:
    AplLspDocument &d_;
    int tradfn_ = -1;     // open tradfn definition
    bool file_function_ = false;
    std::vector<CtlEntry> ctl_;       // control structures and scripts
    struct Open {
        Tok kind;
        int line;
        Token tok;
        int dfn_def;  // for '{': the definition it opens, or -1
    };
    std::vector<Open> brackets_;

    void Diag(int line, const Token &t, AplLspSeverity sev, const std::string &code, const std::string &msg) {
        AplLspDiagnostic g;
        g.range = RangeOf(line, t);
        g.severity = sev;
        g.code = code;
        g.message = msg;
        d_.diags.push_back(g);
    }

    int BraceDepth() const {
        int n = 0;
        for (const Open &o : brackets_) n += o.kind == Tok::LBrace;
        return n;
    }

    // The namespace/class definition the current position is in (or -1).
    int CurrentNamespace() const {
        for (size_t i = ctl_.size(); i-- > 0;) {
            if (ctl_[i].def >= 0) return ctl_[i].def;
        }
        return -1;
    }

    std::string Qualify(const std::string &name) const {
        const int ns = CurrentNamespace();
        return ns >= 0 ? d_.defs[static_cast<size_t>(ns)].qualified + "." + name : name;
    }

    // The ⍝ lines directly below line `li` (stopping at the first non-comment line).
    std::string DocBelow(int li) const {
        std::vector<std::string> out;
        for (int j = li + 1; j < static_cast<int>(d_.toks.size()); j++) {
            if (!CommentOnly(d_.toks[static_cast<size_t>(j)])) break;
            out.push_back(CommentText(d_.toks[static_cast<size_t>(j)][0]));
        }
        return DocOfLines(out);
    }

    // The ⍝ block directly above line `li` (no blank line between).
    std::string DocAbove(int li) const {
        std::vector<std::string> out;
        for (int j = li - 1; j >= 0; j--) {
            if (!CommentOnly(d_.toks[static_cast<size_t>(j)])) break;
            out.insert(out.begin(), CommentText(d_.toks[static_cast<size_t>(j)][0]));
        }
        return DocOfLines(out);
    }

    // A Dyalog .aplf/.aplo file: the first code line is the header of a
    // function that runs to the end of the file (no ∇ around it), unless
    // the file is written with ∇ or as a dfn after all.
    int StartFileFunction() {
        for (int li = 0; li < static_cast<int>(d_.toks.size()); li++) {
            const std::vector<Token> code = CodeOf(d_.toks[static_cast<size_t>(li)]);
            if (code.empty()) continue;
            if (code[0].kind == Tok::Del) return 0;
            for (const Token &t : code) {
                if (t.kind == Tok::LBrace) return 0;
            }
            if (!OpenTradfn(li, code, 0)) return 0;
            file_function_ = true;
            return li + 1;
        }
        return 0;
    }

    // Parses a tradfn header (tokens from `from`) and opens the definition.
    bool OpenTradfn(int li, const std::vector<Token> &code, size_t from) {
        AplDef def;
        def.kind = AplDefKind::Tradfn;
        size_t p = from;
        const size_t n = code.size();
        auto kind = [&](size_t i) { return i < n ? code[i].kind : Tok::Unknown; };
        // result
        if (kind(p) == Tok::Name && kind(p + 1) == Tok::Assign) {
            def.result = code[p].text;
            p += 2;
        } else if (kind(p) == Tok::LBrace && kind(p + 1) == Tok::Name && kind(p + 2) == Tok::RBrace &&
                   kind(p + 3) == Tok::Assign) {
            def.result = code[p + 1].text;
            def.shy_result = true;
            p += 4;
        } else if (kind(p) == Tok::LParen) {
            size_t q = p + 1;
            std::string names;
            while (kind(q) == Tok::Name) names += (names.empty() ? "" : " ") + code[q++].text;
            if (kind(q) == Tok::RParen && kind(q + 1) == Tok::Assign && !names.empty()) {
                def.result = "(" + names + ")";
                p = q + 2;
            }
        }
        struct Item {
            std::string name;
            bool optional = false;
            bool op = false;
            std::string lo, oper, ro;
            Token tok;
        };
        std::vector<Item> items;
        bool bad = false;
        while (p < n && kind(p) != Tok::Semicolon) {
            Item it;
            if (kind(p) == Tok::Name) {
                it.name = code[p].text;
                it.tok = code[p];
                p++;
            } else if (kind(p) == Tok::LBrace && kind(p + 1) == Tok::Name && kind(p + 2) == Tok::RBrace) {
                it.name = code[p + 1].text;
                it.tok = code[p + 1];
                it.optional = true;
                p += 3;
            } else if (kind(p) == Tok::LParen && kind(p + 1) == Tok::Name && kind(p + 2) == Tok::Name &&
                       (kind(p + 3) == Tok::RParen || (kind(p + 3) == Tok::Name && kind(p + 4) == Tok::RParen))) {
                it.op = true;
                it.lo = code[p + 1].text;
                it.oper = code[p + 2].text;
                it.tok = code[p + 2];
                if (kind(p + 3) == Tok::Name) {
                    it.ro = code[p + 3].text;
                    p += 5;
                } else {
                    p += 4;
                }
            } else {
                bad = true;
                break;
            }
            items.push_back(it);
        }
        const Item *name_item = nullptr;
        if (!bad) {
            if (items.size() == 1) {
                name_item = &items[0];
            } else if (items.size() == 2) {
                name_item = &items[0];
                def.right = items[1].name;
            } else if (items.size() == 3) {
                name_item = &items[1];
                def.left = items[0].name;
                def.left_optional = items[0].optional;
                def.right = items[2].name;
            }
        }
        if (!name_item || name_item->optional || (items.size() >= 2 && items.back().op) ||
            (items.size() == 3 && items[0].op)) {
            Token at = from < n ? code[from] : (code.empty() ? Token{} : code.back());
            if (from >= n && !code.empty()) at = code[0];
            Diag(li, at, AplLspSeverity::Error, "syntax-error", "malformed function header");
            // Still open the function so its ∇ closes it.
            def.name = "?";
        } else if (name_item->op) {
            def.kind = AplDefKind::TradOp;
            def.name = name_item->oper;
            def.left_operand = name_item->lo;
            def.right_operand = name_item->ro;
        } else {
            def.name = name_item->name;
        }
        // locals
        while (p < n && kind(p) == Tok::Semicolon) {
            if (kind(p + 1) == Tok::Name || kind(p + 1) == Tok::SysName) {
                def.locals.push_back(code[p + 1].text);
                if (kind(p + 1) == Tok::SysName) CheckSysName(li, code[p + 1]);
                p += 2;
            } else {
                Diag(li, code[p], AplLspSeverity::Error, "syntax-error", "expected a name after ';' in the header");
                p++;
            }
        }
        if (p < n && def.name != "?") {
            Diag(li, code[p], AplLspSeverity::Error, "syntax-error", "unexpected '" + code[p].text + "' in the header");
        }
        // The header text: the line from its first token, without the comment.
        if (from < n) {
            const std::string &line = d_.lines[static_cast<size_t>(li)];
            def.header = Trim(line.substr(static_cast<size_t>(code[from].col),
                                          static_cast<size_t>(code.back().end - code[from].col)));
        }
        def.qualified = Qualify(def.name);
        def.doc = DocBelow(li);
        def.range = AplLspRange{li, from > 0 ? code[0].col : (code.empty() ? 0 : code[0].col), li,
                                static_cast<int>(d_.lines[static_cast<size_t>(li)].size())};
        if (name_item) def.sel_range = RangeOf(li, name_item->tok);
        else def.sel_range = def.range;
        def.parent = CurrentNamespace();
        d_.defs.push_back(def);
        tradfn_ = static_cast<int>(d_.defs.size()) - 1;
        tradfn_ctl_base_ = ctl_.size();
        return name_item != nullptr;
    }
    size_t tradfn_ctl_base_ = 0;

    void CloseTradfn(int li, const Token &del) {
        AplDef &def = d_.defs[static_cast<size_t>(tradfn_)];
        def.range.end_line = li;
        def.range.end_col = del.end;
        UnclosedControl(tradfn_ctl_base_, "the function ends");
        UnclosedBrackets("the function ends");
        if (li > def.range.line) d_.folds.push_back({def.range.line, li, ""});
        tradfn_ = -1;
    }

    void UnclosedControl(size_t base, const std::string &where) {
        while (ctl_.size() > base) {
            const CtlEntry &e = ctl_.back();
            std::string end = ":End";
            if (Openers().count(e.word)) {
                if (const apl::ControlWord *w = apl::FindControlWord(":" + Openers().at(e.word))) end = w->name;
            }
            Diag(e.line, e.tok, AplLspSeverity::Error, "control-structure",
                 e.tok.text + " is not closed before " + where + " (missing " + end + ")");
            ctl_.pop_back();
        }
    }

    void UnclosedBrackets(const std::string &where) {
        for (const Open &o : brackets_) {
            Diag(o.line, o.tok, AplLspSeverity::Error, "unbalanced", "'" + o.tok.text + "' is not closed before " + where);
        }
        brackets_.clear();
    }

    void CheckSysName(int li, const Token &t) {
        if (t.text == "⎕") return;
        if (!apl::FindSystemName(t.text)) {
            Diag(li, t, AplLspSeverity::Warning, "unknown-system-name", "unknown system name " + t.text);
        }
    }

    // A control word at the start of a statement.
    void Keyword(int li, const Token &t, const std::vector<Token> &code, size_t idx) {
        std::string w = Upper(t.text.substr(1));
        if (BraceDepth() > 0) {
            Diag(li, t, AplLspSeverity::Error, "control-structure", t.text + " can't be used inside a dfn");
            return;
        }
        if (!apl::FindControlWord(t.text)) {
            Diag(li, t, AplLspSeverity::Warning, "unknown-keyword", "unknown control word " + t.text);
            return;
        }
        auto top_is = [&](std::initializer_list<const char *> words) {
            if (ctl_.empty()) return false;
            for (const char *x : words) {
                if (ctl_.back().word == x) return true;
            }
            return false;
        };
        if (Openers().count(w)) {
            CtlEntry e;
            e.word = w;
            e.line = li;
            e.tok = t;
            if (w == "NAMESPACE" || w == "CLASS" || w == "INTERFACE") {
                if (tradfn_ >= 0) {
                    Diag(li, t, AplLspSeverity::Error, "control-structure", t.text + " can't be used inside a function");
                }
                AplDef def;
                def.kind = w == "NAMESPACE" ? AplDefKind::Namespace : AplDefKind::Class;
                if (idx + 1 < code.size() && (code[idx + 1].kind == Tok::Name || code[idx + 1].kind == Tok::QualName)) {
                    def.name = code[idx + 1].text;
                    def.sel_range = RangeOf(li, code[idx + 1]);
                } else {
                    // An unnamed :Namespace in a .apln file takes the file's name.
                    std::string base = d_.path;
                    const size_t slash = base.find_last_of("/\\");
                    if (slash != std::string::npos) base = base.substr(slash + 1);
                    const size_t dot = base.find('.');
                    def.name = base.substr(0, dot);
                    if (def.name.empty()) def.name = "#";
                    def.sel_range = RangeOf(li, t);
                }
                def.qualified = Qualify(def.name);
                def.header = Trim(d_.lines[static_cast<size_t>(li)]);
                if (const size_t c = def.header.find("⍝"); c != std::string::npos) def.header = Trim(def.header.substr(0, c));
                def.doc = DocBelow(li);
                def.range = AplLspRange{li, t.col, li, static_cast<int>(d_.lines[static_cast<size_t>(li)].size())};
                def.parent = CurrentNamespace();
                d_.defs.push_back(def);
                e.def = static_cast<int>(d_.defs.size()) - 1;
            } else if (tradfn_ < 0 && w != "SECTION" && w != "PROPERTY" && !file_function_) {
                // Dyalog scripts (.apls) allow control structures at top level; fine.
            }
            ctl_.push_back(e);
            return;
        }
        auto close = [&](const CtlEntry &e) {
            if (li > e.line) d_.folds.push_back({e.line, li - (w == "UNTIL" ? 0 : 0), ""});
            if (e.def >= 0) {
                AplDef &def = d_.defs[static_cast<size_t>(e.def)];
                def.range.end_line = li;
                def.range.end_col = t.end;
            }
        };
        if (w == "END" || w.rfind("END", 0) == 0) {
            const size_t base = tradfn_ >= 0 ? tradfn_ctl_base_ : 0;
            if (ctl_.size() <= base) {
                Diag(li, t, AplLspSeverity::Error, "control-structure", t.text + " without a matching opening word");
                return;
            }
            const CtlEntry e = ctl_.back();
            if (w != "END" && Openers().at(e.word) != w) {
                Diag(li, t, AplLspSeverity::Error, "control-structure",
                     t.text + " does not match " + e.tok.text + " on line " + std::to_string(e.line + 1));
            }
            ctl_.pop_back();
            close(e);
            return;
        }
        if (w == "UNTIL") {
            if (!top_is({"REPEAT", "WHILE"})) {
                Diag(li, t, AplLspSeverity::Error, "control-structure", ":Until outside a :Repeat or :While");
                return;
            }
            const CtlEntry e = ctl_.back();
            ctl_.pop_back();
            close(e);
            return;
        }
        if (w == "ELSEIF" || w == "ANDIF" || w == "ORIF") {
            if (!top_is({"IF", "WHILE", "UNTIL"})) {
                Diag(li, t, AplLspSeverity::Error, "control-structure", t.text + " outside an :If");
            }
            return;
        }
        if (w == "ELSE") {
            if (!top_is({"IF", "SELECT", "TRAP"})) {
                Diag(li, t, AplLspSeverity::Error, "control-structure", ":Else outside an :If, :Select or :Trap");
            }
            return;
        }
        if (w == "CASE" || w == "CASELIST") {
            if (!top_is({"SELECT", "TRAP"})) {
                Diag(li, t, AplLspSeverity::Error, "control-structure", t.text + " outside a :Select or :Trap");
            }
            return;
        }
        if (w == "IN" || w == "INEACH") {
            Diag(li, t, AplLspSeverity::Error, "control-structure", t.text + " outside a :For");
            return;
        }
        if (w == "LEAVE" || w == "CONTINUE") {
            bool in_loop = false;
            for (const CtlEntry &e : ctl_) in_loop |= e.word == "FOR" || e.word == "WHILE" || e.word == "REPEAT";
            if (!in_loop) Diag(li, t, AplLspSeverity::Error, "control-structure", t.text + " outside a loop");
        }
    }

    // Records `name←{` (a dfn or dop) or a top-level `name←value`.
    void Assignment(int li, const std::vector<Token> &code, size_t name_idx, size_t brace_idx_or_npos) {
        const Token &nt = code[name_idx];
        const std::string simple = nt.kind == Tok::QualName ? nt.text.substr(nt.text.find_last_of('.') + 1) : nt.text;
        if (brace_idx_or_npos != std::string::npos) {
            AplDef def;
            def.kind = AplDefKind::Dfn;
            def.name = simple;
            def.qualified = nt.kind == Tok::QualName ? nt.text : Qualify(simple);
            def.sel_range = RangeOf(li, nt);
            def.range = AplLspRange{li, nt.col, li, static_cast<int>(d_.lines[static_cast<size_t>(li)].size())};
            std::string head = d_.lines[static_cast<size_t>(li)].substr(static_cast<size_t>(nt.col));
            if (const size_t c = head.find("⍝"); c != std::string::npos) head = head.substr(0, c);
            def.header = Trim(head);
            // ⍝ lines right after a `name←{` that ends its line; else the block above.
            const bool brace_ends_line = brace_idx_or_npos + 1 == code.size();
            def.doc = brace_ends_line ? DocBelow(li) : std::string();
            if (def.doc.empty()) def.doc = DocAbove(li);
            def.parent = CurrentNamespace();
            d_.defs.push_back(def);
            pending_dfn_ = static_cast<int>(d_.defs.size()) - 1;
            return;
        }
        if (tradfn_ >= 0 || BraceDepth() > 0) return;
        const std::string q = nt.kind == Tok::QualName ? nt.text : Qualify(simple);
        for (const AplDef &d : d_.defs) {
            if (d.qualified == q) return;  // the first assignment defines it
        }
        AplDef def;
        def.kind = AplDefKind::Variable;
        def.name = simple;
        def.qualified = q;
        def.sel_range = RangeOf(li, nt);
        def.range = AplLspRange{li, nt.col, li, static_cast<int>(d_.lines[static_cast<size_t>(li)].size())};
        std::string head = d_.lines[static_cast<size_t>(li)].substr(static_cast<size_t>(nt.col));
        const std::vector<Token> &all = d_.toks[static_cast<size_t>(li)];
        if (!all.empty() && all.back().kind == Tok::Comment) {
            def.doc = CommentText(all.back());
            head = d_.lines[static_cast<size_t>(li)].substr(static_cast<size_t>(nt.col),
                                                            static_cast<size_t>(all.back().col - nt.col));
        } else {
            def.doc = DocAbove(li);
        }
        def.header = Trim(head);
        def.parent = CurrentNamespace();
        d_.defs.push_back(def);
    }
    int pending_dfn_ = -1;

    void Line(int li) {
        const std::vector<Token> &all = d_.toks[static_cast<size_t>(li)];
        const std::vector<Token> code = CodeOf(all);
        bool stmt_start = true;
        for (size_t i = 0; i < code.size(); i++) {
            const Token &t = code[i];
            const bool in_dfn = BraceDepth() > 0;
            const bool in_group = !brackets_.empty() && brackets_.back().kind != Tok::LBrace;
            const bool at_start = stmt_start && !in_group;
            stmt_start = false;
            switch (t.kind) {
            case Tok::SysCommand: {
                const std::string word = Upper(t.text.substr(0, t.text.find_first_of(" \t")));
                if (t.text[0] == ')' && !apl::FindSystemCommand(word)) {
                    Diag(li, Token{t.kind, t.col, t.col + static_cast<int>(word.size()), word}, AplLspSeverity::Warning,
                         "unknown-command", "unknown system command " + word);
                }
                break;
            }
            case Tok::Del:
            case Tok::DelDel:
                if (in_dfn) break;
                if (at_start && t.kind == Tok::Del) {
                    if (tradfn_ >= 0 && !file_function_) {
                        CloseTradfn(li, t);
                        // Anything after the closing ∇ (⍫) is ignored.
                        return;
                    }
                    if (file_function_) {
                        Diag(li, t, AplLspSeverity::Error, "syntax-error", "∇ inside a function file");
                        return;
                    }
                    if (!brackets_.empty()) UnclosedBrackets("the function starts");
                    if (i + 1 >= code.size()) {
                        Diag(li, t, AplLspSeverity::Error, "syntax-error", "∇ without a function header");
                        return;
                    }
                    OpenTradfn(li, code, i + 1);
                    return;
                }
                Diag(li, t, AplLspSeverity::Error, "syntax-error",
                     t.text + " outside a dfn (a tradfn's ∇ must start its line)");
                break;
            case Tok::Keyword:
                if (at_start) Keyword(li, t, code, i);
                break;
            case Tok::Name:
            case Tok::QualName:
                if (at_start && !in_dfn && t.kind == Tok::Name && i + 1 < code.size() && code[i + 1].kind == Tok::Colon) {
                    // A label.
                    if (tradfn_ >= 0) {
                        AplDef def;
                        def.kind = AplDefKind::Label;
                        def.name = t.text;
                        def.qualified = t.text;
                        def.header = t.text + ":";
                        def.sel_range = RangeOf(li, t);
                        def.range = def.sel_range;
                        def.owner = tradfn_;
                        def.parent = d_.defs[static_cast<size_t>(tradfn_)].parent;
                        d_.defs[static_cast<size_t>(tradfn_)].labels.push_back(t.text);
                        d_.defs.push_back(def);
                    }
                    i++;            // the colon
                    stmt_start = true;  // a statement may follow the label
                    break;
                }
                if (at_start && i + 1 < code.size() && code[i + 1].kind == Tok::Assign) {
                    const size_t brace = i + 2 < code.size() && code[i + 2].kind == Tok::LBrace ? i + 2 : std::string::npos;
                    Assignment(li, code, i, brace);
                }
                break;
            case Tok::SysName:
                CheckSysName(li, t);
                break;
            case Tok::Special:
                if (!in_dfn) {
                    Diag(li, t, AplLspSeverity::Warning, "outside-dfn", t.text + " is only defined inside a dfn");
                } else if (t.text == "⍺" && i + 1 < code.size() && code[i + 1].kind == Tok::Assign) {
                    const int dfn = brackets_.back().dfn_def;
                    if (dfn >= 0) d_.defs[static_cast<size_t>(dfn)].left_optional = true;
                } else if (t.text == "⍺⍺" || t.text == "⍵⍵") {
                    // The innermost named dfn using an operand is a dop.
                    for (size_t k = brackets_.size(); k-- > 0;) {
                        if (brackets_[k].kind != Tok::LBrace) continue;
                        if (brackets_[k].dfn_def >= 0) {
                            AplDef &def = d_.defs[static_cast<size_t>(brackets_[k].dfn_def)];
                            def.kind = AplDefKind::Dop;
                            (t.text == "⍺⍺" ? def.left_operand : def.right_operand) = t.text;
                        }
                        break;
                    }
                } else if (t.text == "⍺") {
                    for (size_t k = brackets_.size(); k-- > 0;) {
                        if (brackets_[k].kind != Tok::LBrace) continue;
                        if (brackets_[k].dfn_def >= 0) d_.defs[static_cast<size_t>(brackets_[k].dfn_def)].left = "⍺";
                        break;
                    }
                }
                break;
            case Tok::Colon:
                if (!in_dfn) {
                    Diag(li, t, AplLspSeverity::Error, "syntax-error", "':' outside a dfn guard or label");
                }
                break;
            case Tok::DoubleColon:
                if (!in_dfn) Diag(li, t, AplLspSeverity::Error, "syntax-error", "'::' (error guard) outside a dfn");
                break;
            case Tok::Assign: {
                const bool last = i + 1 >= code.size();
                const Tok next = last ? Tok::Unknown : code[i + 1].kind;
                if (last || next == Tok::Diamond || next == Tok::RParen || next == Tok::RBracket || next == Tok::RBrace) {
                    Diag(li, t, AplLspSeverity::Error, "syntax-error", "missing value after ←");
                }
                if (i == 0 || (at_start && i == 0)) {
                    Diag(li, t, AplLspSeverity::Error, "syntax-error", "missing name before ←");
                }
                break;
            }
            case Tok::Diamond:
                if (!in_group) stmt_start = true;
                break;
            case Tok::LParen:
            case Tok::LBracket:
                brackets_.push_back({t.kind, li, t, -1});
                break;
            case Tok::LBrace: {
                brackets_.push_back({t.kind, li, t, pending_dfn_});
                pending_dfn_ = -1;
                stmt_start = true;
                break;
            }
            case Tok::RParen:
            case Tok::RBracket:
            case Tok::RBrace: {
                const Tok want = t.kind == Tok::RParen ? Tok::LParen : t.kind == Tok::RBracket ? Tok::LBracket : Tok::LBrace;
                if (brackets_.empty()) {
                    Diag(li, t, AplLspSeverity::Error, "unbalanced", "'" + t.text + "' without a matching opening bracket");
                    break;
                }
                const Open o = brackets_.back();
                if (o.kind != want) {
                    Diag(li, t, AplLspSeverity::Error, "unbalanced",
                         "'" + t.text + "' does not match '" + o.tok.text + "' on line " + std::to_string(o.line + 1) +
                             ":" + std::to_string(o.tok.col + 1));
                    // Recover: pop up to the matching opener if there is one.
                    for (size_t k = brackets_.size(); k-- > 0;) {
                        if (brackets_[k].kind == want) {
                            brackets_.resize(k);
                            break;
                        }
                    }
                    break;
                }
                brackets_.pop_back();
                if (t.kind == Tok::RBrace) {
                    d_.dfns.push_back({o.line, o.tok.col, li, t.end, o.dfn_def});
                    if (li > o.line) d_.folds.push_back({o.line, li, ""});
                    if (o.dfn_def >= 0) {
                        AplDef &def = d_.defs[static_cast<size_t>(o.dfn_def)];
                        def.range.end_line = li;
                        def.range.end_col = t.end;
                    }
                }
                break;
            }
            case Tok::Unknown:
                Diag(li, t, AplLspSeverity::Error, "syntax-error", "unexpected character '" + t.text + "'");
                break;
            case Tok::BadString:
                Diag(li, t, AplLspSeverity::Error, "syntax-error", "unterminated string");
                break;
            default:
                break;
            }
        }
        pending_dfn_ = -1;
        // A dfn's `name←{` defines the name only once its brace opens; a
        // `name←{` with the brace on this line is handled above.
    }

    void EndOfDocument() {
        const int last = static_cast<int>(d_.lines.size()) - 1;
        if (tradfn_ >= 0) {
            if (file_function_) {
                AplDef &def = d_.defs[static_cast<size_t>(tradfn_)];
                def.range.end_line = std::max(0, last);
                def.range.end_col = last >= 0 ? static_cast<int>(d_.lines[static_cast<size_t>(last)].size()) : 0;
                UnclosedControl(tradfn_ctl_base_, "the end of the file");
                if (last > def.range.line) d_.folds.push_back({def.range.line, last, ""});
            } else {
                const AplDef &def = d_.defs[static_cast<size_t>(tradfn_)];
                AplLspDiagnostic g;
                g.range = def.sel_range;
                g.severity = AplLspSeverity::Error;
                g.code = "unbalanced";
                g.message = "function " + def.name + " is not closed with ∇";
                d_.diags.push_back(g);
                UnclosedControl(tradfn_ctl_base_, "the end of the file");
            }
            tradfn_ = -1;
        }
        UnclosedBrackets("the end of the file");
        UnclosedControl(0, "the end of the file");
    }

    void CommentFolds() {
        int start = -1;
        const int n = static_cast<int>(d_.toks.size());
        for (int li = 0; li <= n; li++) {
            const bool c = li < n && CommentOnly(d_.toks[static_cast<size_t>(li)]);
            if (c && start < 0) start = li;
            if (!c && start >= 0) {
                if (li - 1 > start) d_.folds.push_back({start, li - 1, "comment"});
                start = -1;
            }
        }
        std::sort(d_.folds.begin(), d_.folds.end(), [](const AplLspFold &a, const AplLspFold &b) {
            return a.start_line != b.start_line ? a.start_line < b.start_line : a.end_line > b.end_line;
        });
    }
};

// --- position helpers ----------------------------------------------------------------------

const Token *TokenAt(const AplLspDocument &doc, int line, int col, bool allow_end = true) {
    if (line < 0 || static_cast<size_t>(line) >= doc.toks.size()) return nullptr;
    const std::vector<Token> &ts = doc.toks[static_cast<size_t>(line)];
    for (const Token &t : ts) {
        if (col >= t.col && col < t.end) return &t;
    }
    if (allow_end) {
        for (const Token &t : ts) {
            if (col == t.end && t.kind != Tok::Comment) return &t;
        }
    }
    return nullptr;
}

bool Contains(const AplLspRange &r, int line, int col) {
    if (line < r.line || line > r.end_line) return false;
    if (line == r.line && col < r.col) return false;
    if (line == r.end_line && col > r.end_col) return false;
    return true;
}

// The tradfn (or dfn) definition containing a position: innermost.
int EnclosingFunction(const AplLspDocument &doc, int line, int col, bool tradfn_only) {
    int best = -1;
    for (size_t i = 0; i < doc.defs.size(); i++) {
        const AplDef &d = doc.defs[i];
        if (!IsFunctionKind(d.kind)) continue;
        if (tradfn_only && d.kind != AplDefKind::Tradfn && d.kind != AplDefKind::TradOp) continue;
        if (!Contains(d.range, line, col)) continue;
        if (best < 0 || d.range.line >= doc.defs[static_cast<size_t>(best)].range.line) best = static_cast<int>(i);
    }
    return best;
}

int EnclosingNamespace(const AplLspDocument &doc, int line, int col) {
    int best = -1;
    for (size_t i = 0; i < doc.defs.size(); i++) {
        const AplDef &d = doc.defs[i];
        if (d.kind != AplDefKind::Namespace && d.kind != AplDefKind::Class) continue;
        if (!Contains(d.range, line, col)) continue;
        best = static_cast<int>(i);
    }
    return best;
}

std::string StripRoot(std::string q) {
    if (q.rfind("##.", 0) == 0) q = q.substr(3);
    else if (q.rfind("#.", 0) == 0) q = q.substr(2);
    return q;
}

// Candidate definitions for a (possibly qualified) name, nearest first.
struct Found {
    const AplDef *def = nullptr;
    const AplLspDocument *doc = nullptr;
};

std::vector<Found> Resolve(const AplLspDocument &doc, const AplLspWorkspace *ws, const std::string &raw, int line,
                           int col) {
    std::vector<Found> out;
    const std::string name = StripRoot(raw);
    const bool qualified = name.find('.') != std::string::npos;
    const int ns = EnclosingNamespace(doc, line, col);
    const std::string ns_prefix = ns >= 0 ? doc.defs[static_cast<size_t>(ns)].qualified + "." : std::string();
    auto matches = [&](const AplDef &d) {
        if (d.kind == AplDefKind::Label) return false;
        if (qualified) return d.qualified == name || EndsWith(d.qualified, "." + name);
        return d.name == name;
    };
    // This document: definitions in the enclosing namespace first.
    std::vector<Found> here, elsewhere;
    for (const AplDef &d : doc.defs) {
        if (!matches(d)) continue;
        if (!qualified && !ns_prefix.empty() && d.qualified == ns_prefix + name) here.push_back({&d, &doc});
        else elsewhere.push_back({&d, &doc});
    }
    out.insert(out.end(), here.begin(), here.end());
    out.insert(out.end(), elsewhere.begin(), elsewhere.end());
    if (ws) {
        std::set<std::string> seen;
        for (const Found &f : out) seen.insert(f.def->qualified);
        for (const auto &other : ws->docs) {
            if (!other || other.get() == &doc || (!doc.path.empty() && other->path == doc.path)) continue;
            for (const AplDef &d : other->defs) {
                if (!matches(d) || d.kind == AplDefKind::Variable) continue;
                // A copy of a definition (an installed binding and its source) counts once.
                if (!seen.insert(d.qualified + "\n" + d.header).second) continue;
                out.push_back({&d, other.get()});
            }
        }
    }
    return out;
}

// The name under the cursor, for a dotted name only up to the segment under the cursor.
bool NameAt(const AplLspDocument &doc, int line, int col, std::string *name, AplLspRange *range) {
    const Token *t = TokenAt(doc, line, col);
    if (!t || (t->kind != Tok::Name && t->kind != Tok::QualName)) return false;
    std::string text = t->text;
    int end = t->end;
    if (t->kind == Tok::QualName) {
        const size_t rel = static_cast<size_t>(std::max(0, col - t->col));
        const size_t dot = text.find('.', rel);
        if (dot != std::string::npos) {
            text = text.substr(0, dot);
            end = t->col + static_cast<int>(dot);
        }
    }
    *name = text;
    *range = AplLspRange{line, t->col, line, end};
    return true;
}

// For a name inside a tradfn: its role there (argument, result, local), or "".
std::string LocalRole(const AplDef &fn, const std::string &name) {
    if (name == fn.result) return "result";
    if (name == fn.left) return fn.left_optional ? "optional left argument" : "left argument";
    if (name == fn.right) return "right argument";
    if (name == fn.left_operand) return "left operand";
    if (name == fn.right_operand) return "right operand";
    for (const std::string &l : fn.locals) {
        if (l == name) return "local";
    }
    return std::string();
}

std::string SpecialDoc(const std::string &s) {
    if (s == "⍺") return "**⍺** *(dfn left argument)*\n\nThe left argument of the dfn; `⍺←value` gives it a default.";
    if (s == "⍵") return "**⍵** *(dfn right argument)*\n\nThe right argument of the dfn.";
    if (s == "⍺⍺") return "**⍺⍺** *(dop left operand)*\n\nThe left operand of the dfn operator.";
    if (s == "⍵⍵") return "**⍵⍵** *(dop right operand)*\n\nThe right operand of the dfn operator.";
    if (s == "λ") return "**λ** *(GNU APL lambda result)*\n\nThe result of a GNU APL lambda.";
    if (s == "χ") return "**χ** *(GNU APL lambda axis)*\n\nThe axis argument of a GNU APL lambda.";
    return std::string();
}

// A tradfn signature: the header without its locals, with its parameters.
AplLspSignatureHelp SignatureOf(const AplDef &d) {
    AplLspSignatureHelp sh;
    sh.found = true;
    if (d.kind == AplDefKind::Tradfn || d.kind == AplDefKind::TradOp) {
        std::string h = d.header;
        if (const size_t semi = h.find(';'); semi != std::string::npos) h = Trim(h.substr(0, semi));
        sh.label = h;
    } else {
        std::string l;
        if (d.left_operand.size() || d.right_operand.size()) {
            l = std::string(d.left.empty() ? "" : (d.left_optional ? "{⍺} " : "⍺ ")) + "(⍺⍺ " + d.name +
                (d.right_operand.empty() ? "" : " ⍵⍵") + ") ⍵";
        } else {
            l = std::string(d.left.empty() ? "" : (d.left_optional ? "{⍺} " : "⍺ ")) + d.name + " ⍵";
        }
        sh.label = l;
    }
    for (const std::string *p : {&d.left, &d.right}) {
        if (p->empty()) continue;
        const std::string key = *p;
        if (sh.label.find(key) != std::string::npos) sh.params.push_back(key);
    }
    sh.documentation = d.doc;
    return sh;
}

void AddUnique(std::vector<AplLspCompletionItem> &out, std::set<std::string> &seen, AplLspCompletionItem it) {
    if (!seen.insert(it.label + "\x01" + it.insert_text).second) return;
    out.push_back(std::move(it));
}

AplLspKind CompletionKindOf(AplDefKind k) {
    switch (k) {
    case AplDefKind::Tradfn:
    case AplDefKind::Dfn: return AplLspKind::Function;
    case AplDefKind::TradOp:
    case AplDefKind::Dop: return AplLspKind::Operator;
    case AplDefKind::Namespace: return AplLspKind::Module;
    case AplDefKind::Class: return AplLspKind::Class;
    default: return AplLspKind::Variable;
    }
}

}  // namespace

// --- public API ------------------------------------------------------------------------------

std::shared_ptr<const AplLspDocument> AplLspAnalyze(const std::vector<std::string> &lines, const std::string &path) {
    auto doc = std::make_shared<AplLspDocument>();
    doc->lines = lines;
    doc->path = path;
    Analyzer(*doc).Run();
    for (AplDef &d : doc->defs) d.path = path;
    return doc;
}

const std::vector<AplDef> &AplLspDefinitions(const AplLspDocument &doc) { return doc.defs; }
const std::string &AplLspPath(const AplLspDocument &doc) { return doc.path; }

std::vector<AplLspDiagnostic> AplLspDiagnostics(const AplLspDocument &doc) {
    std::vector<AplLspDiagnostic> out = doc.diags;
    std::stable_sort(out.begin(), out.end(), [](const AplLspDiagnostic &a, const AplLspDiagnostic &b) {
        return a.range.line != b.range.line ? a.range.line < b.range.line : a.range.col < b.range.col;
    });
    return out;
}

std::string AplLspFormatDiagnostic(const std::string &path, const AplLspDiagnostic &d) {
    const char *sev = d.severity == AplLspSeverity::Error         ? "error"
                      : d.severity == AplLspSeverity::Warning     ? "warning"
                      : d.severity == AplLspSeverity::Information ? "info"
                                                                  : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool AplLspDiagnosticFails(const AplLspDiagnostic &d) {
    return d.severity == AplLspSeverity::Error || d.severity == AplLspSeverity::Warning;
}

std::string AplLspDefMarkdown(const AplDef &d) {
    std::string out = "```apl\n" + (d.header.empty() ? d.name : d.header) + "\n```\n";
    out += "*" + std::string(KindName(d.kind)) + "*";
    if (d.qualified != d.name) out += " `" + d.qualified + "`";
    out += "\n";
    if (!d.doc.empty()) out += "\n" + d.doc + "\n";
    if (!d.path.empty()) {
        std::string base = d.path;
        const size_t slash = base.find_last_of("/\\");
        if (slash != std::string::npos) base = base.substr(slash + 1);
        out += "\n_" + base + ":" + std::to_string(d.sel_range.line + 1) + "_\n";
    }
    return out;
}

std::string AplLspDocFor(const std::string &name_in, const AplLspDocument *doc, const AplLspWorkspace *ws) {
    const std::string name = Trim(name_in);
    if (name.empty()) return std::string();
    if (const apl::Glyph *g = apl::FindGlyph(name)) return apl::GlyphMarkdown(*g);
    if (name.rfind("⎕", 0) == 0) {
        if (const apl::SystemName *s = apl::FindSystemName(name)) return apl::SystemNameMarkdown(*s);
    }
    if (name[0] == ':') {
        if (const apl::ControlWord *w = apl::FindControlWord(name)) return "**" + std::string(w->name) + "**\n\n" + w->doc;
    }
    if (name[0] == ')') {
        if (const apl::SystemCommand *c = apl::FindSystemCommand(name)) {
            return "**" + std::string(c->name) + "** *(system command)*\n\n" + c->doc;
        }
    }
    std::string out;
    std::set<std::string> seen;
    auto add = [&](const AplDef &d) {
        if (d.kind == AplDefKind::Label) return;
        const std::string q = StripRoot(name);
        if (!(d.name == q || d.qualified == q || EndsWith(d.qualified, "." + q))) return;
        if (!seen.insert(d.qualified + "\n" + d.header).second) return;
        if (!out.empty()) out += "\n---\n\n";
        out += AplLspDefMarkdown(d);
    };
    if (doc) {
        for (const AplDef &d : doc->defs) add(d);
    }
    if (ws) {
        for (const auto &o : ws->docs) {
            if (!o) continue;
            for (const AplDef &d : o->defs) add(d);
        }
    }
    if (!out.empty()) return out;
    // A glyph by its completion name (rho -> ⍴), then a ⎕name without its ⎕ (NA -> ⎕NA).
    for (const apl::Glyph &g : apl::Glyphs()) {
        const std::string names = std::string(" ") + g.names + " ";
        if (names.find(" " + Lower(name) + " ") != std::string::npos) return apl::GlyphMarkdown(g);
    }
    if (name.rfind("⎕", 0) != 0) {
        if (const apl::SystemName *s = apl::FindSystemName(name)) return apl::SystemNameMarkdown(*s);
    }
    return out;
}

AplLspHoverInfo AplLspHover(const AplLspDocument &doc, int line, int col, const AplLspWorkspace *ws) {
    AplLspHoverInfo info;
    const Token *t = TokenAt(doc, line, col, false);
    if (!t) return info;
    info.range = RangeOf(line, *t);
    switch (t->kind) {
    case Tok::Primitive:
    case Tok::Operator:
    case Tok::Assign:
    case Tok::Branch:
    case Tok::Diamond:
    case Tok::Zilde:
    case Tok::Del:
    case Tok::Lock:
        if (const apl::Glyph *g = apl::FindGlyph(t->text)) {
            info.found = true;
            info.text = apl::GlyphMarkdown(*g);
        }
        return info;
    case Tok::DelDel:
        info.found = true;
        info.text = "**∇∇** *(dop self-reference)*\n\nInside a dfn operator: the operator itself (recursion).";
        return info;
    case Tok::Special:
        info.found = true;
        info.text = SpecialDoc(t->text);
        return info;
    case Tok::SysName: {
        if (const apl::SystemName *s = apl::FindSystemName(t->text)) {
            info.found = true;
            info.text = apl::SystemNameMarkdown(*s);
        } else if (t->text == "⎕") {
            info.found = true;
            info.text = apl::GlyphMarkdown(*apl::FindGlyph("⎕"));
        }
        return info;
    }
    case Tok::Keyword:
        if (const apl::ControlWord *w = apl::FindControlWord(t->text)) {
            info.found = true;
            info.text = "**" + std::string(w->name) + "** *(control word)*\n\n" + w->doc;
        }
        return info;
    case Tok::SysCommand: {
        const std::string word = Upper(t->text.substr(0, t->text.find_first_of(" \t")));
        if (const apl::SystemCommand *c = apl::FindSystemCommand(word)) {
            info.found = true;
            info.text = "**" + std::string(c->name) + "** *(system command)*\n\n" + c->doc;
        }
        return info;
    }
    case Tok::Name:
    case Tok::QualName: {
        std::string name;
        if (!NameAt(doc, line, col, &name, &info.range)) return info;
        // A name local to the enclosing tradfn.
        const int fn = EnclosingFunction(doc, line, col, true);
        if (fn >= 0 && t->kind == Tok::Name) {
            const AplDef &f = doc.defs[static_cast<size_t>(fn)];
            const std::string role = LocalRole(f, name);
            if (!role.empty()) {
                info.found = true;
                info.text = "```apl\n" + f.header + "\n```\n`" + name + "`: " + role + " of **" + f.name + "**";
                return info;
            }
            for (const std::string &l : f.labels) {
                if (l == name) {
                    info.found = true;
                    info.text = "`" + name + ":` *label in " + f.name + "*";
                    return info;
                }
            }
        }
        const std::vector<Found> found = Resolve(doc, ws, name, line, col);
        if (found.empty()) return info;
        info.found = true;
        std::set<std::string> seen;
        for (const Found &f : found) {
            const std::string md = AplLspDefMarkdown(*f.def);
            if (!seen.insert(md).second) continue;
            if (!info.text.empty()) info.text += "\n---\n\n";
            info.text += md;
            if (seen.size() >= 3) break;
        }
        return info;
    }
    default:
        return info;
    }
}

std::vector<AplLspCompletionItem> AplLspCompletions(const AplLspDocument &doc, int line, int col,
                                                    const AplLspWorkspace *ws) {
    std::vector<AplLspCompletionItem> out;
    std::set<std::string> seen;
    if (line < 0 || static_cast<size_t>(line) >= doc.lines.size()) return out;
    const std::string &cur = doc.lines[static_cast<size_t>(line)];
    col = std::max(0, std::min(col, static_cast<int>(cur.size())));
    // No completion inside a string or a comment.
    for (const Token &t : doc.toks[static_cast<size_t>(line)]) {
        if ((t.kind == Tok::String || t.kind == Tok::BadString) && col > t.col && (col < t.end || t.kind == Tok::BadString)) {
            return out;
        }
        if (t.kind == Tok::Comment && col > t.col) return out;
    }
    // `key: the APL keyboard's glyphs.
    {
        int bt = -1;
        if (col >= 1 && cur[static_cast<size_t>(col - 1)] == '`') bt = col - 1;
        else if (col >= 2 && cur[static_cast<size_t>(col - 2)] == '`' &&
                 static_cast<unsigned char>(cur[static_cast<size_t>(col - 1)]) < 0x80)
            bt = col - 2;
        if (bt >= 0) {
            const char typed = bt == col - 2 ? cur[static_cast<size_t>(col - 1)] : '\0';
            for (const apl::Glyph &g : apl::Glyphs()) {
                if (!g.key[0]) continue;
                if (typed && g.key[0] != typed) continue;
                AplLspCompletionItem it;
                it.label = std::string("`") + g.key + " " + g.glyph;
                it.filter_text = std::string("`") + g.key;
                it.insert_text = g.glyph;
                it.kind = std::string(g.kind) == "operator" ? AplLspKind::Operator : AplLspKind::Function;
                it.detail = std::string(g.monadic[0] ? g.monadic : g.dyadic) +
                            (g.monadic[0] && g.dyadic[0] ? std::string(" / ") + g.dyadic : std::string());
                it.documentation = apl::GlyphMarkdown(g);
                it.replace_start = bt;
                it.replace_end = col;
                AddUnique(out, seen, it);
            }
            return out;
        }
    }
    // The word before the cursor: name chars, dots, and a leading ⎕ or ) or :.
    int start = col;
    while (start > 0) {
        int p = start - 1;
        while (p > 0 && (static_cast<unsigned char>(cur[static_cast<size_t>(p)]) & 0xC0) == 0x80) p--;
        const std::string cp = cur.substr(static_cast<size_t>(p), static_cast<size_t>(start - p));
        if (apl::IsNameChar(cp) || cp == ".") {
            start = p;
            continue;
        }
        break;
    }
    std::string word = cur.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    const std::string quad = "⎕";
    // ⎕names
    if (start >= static_cast<int>(quad.size()) && cur.compare(static_cast<size_t>(start) - quad.size(), quad.size(), quad) == 0 &&
        word.find('.') == std::string::npos) {
        const int qstart = start - static_cast<int>(quad.size());
        for (const apl::SystemName &s : apl::SystemNames()) {
            const std::string bare = std::string(s.name).substr(quad.size());
            if (Upper(bare).rfind(Upper(word), 0) != 0) continue;
            AplLspCompletionItem it;
            it.label = s.name;
            it.insert_text = s.name;
            it.kind = std::string(s.kind) == "variable" ? AplLspKind::Variable
                      : std::string(s.kind) == "constant" ? AplLspKind::Constant
                      : std::string(s.kind) == "namespace" ? AplLspKind::Module
                                                           : AplLspKind::Function;
            it.detail = s.syntax;
            it.documentation = apl::SystemNameMarkdown(s);
            it.replace_start = qstart;
            it.replace_end = col;
            AddUnique(out, seen, it);
        }
        return out;
    }
    // )commands at the start of the line
    {
        const std::string before = Trim(cur.substr(0, static_cast<size_t>(start)));
        if (before == ")") {
            const int cstart = static_cast<int>(cur.find(')'));
            for (const apl::SystemCommand &c : apl::SystemCommands()) {
                if (std::string(c.name).substr(1).rfind(Upper(word), 0) != 0) continue;
                AplLspCompletionItem it;
                it.label = c.name;
                it.insert_text = c.name;
                it.kind = AplLspKind::Keyword;
                it.documentation = c.doc;
                it.replace_start = cstart;
                it.replace_end = col;
                AddUnique(out, seen, it);
            }
            return out;
        }
        // :Keywords at the start of a statement
        if (start > 0 && cur[static_cast<size_t>(start - 1)] == ':') {
            const std::string lead = Trim(cur.substr(0, static_cast<size_t>(start - 1)));
            const bool stmt = lead.empty() || EndsWith(lead, "⋄") || EndsWith(lead, "◊") ||
                              (Upper(lead).find(":FOR") != std::string::npos);
            if (stmt) {
                for (const apl::ControlWord &w : apl::ControlWords()) {
                    if (Upper(std::string(w.name).substr(1)).rfind(Upper(word), 0) != 0) continue;
                    AplLspCompletionItem it;
                    it.label = w.name;
                    it.insert_text = w.name;
                    it.kind = AplLspKind::Keyword;
                    it.documentation = w.doc;
                    it.replace_start = start - 1;
                    it.replace_end = col;
                    AddUnique(out, seen, it);
                }
                return out;
            }
        }
    }
    // Strip a root: #.ns.x
    std::string qword = word;
    int qstart = start;
    if (start >= 1 && cur[static_cast<size_t>(start - 1)] == '#') {
        // `#.` / `##.` prefix: the word starts with '.'
    }
    if (!qword.empty() && qword[0] == '.') {
        qword = qword.substr(1);
        qstart++;
    }
    const size_t dot = qword.rfind('.');
    if (dot != std::string::npos) {
        // ns.member: the members of ns.
        const std::string ns = qword.substr(0, dot);
        const std::string prefix = qword.substr(dot + 1);
        const int rstart = qstart + static_cast<int>(dot) + 1;
        auto offer = [&](const AplDef &d) {
            if (d.kind == AplDefKind::Label) return;
            const std::string q = d.qualified;
            if (!(q.size() > ns.size() + 1 && (q.compare(0, ns.size() + 1, ns + ".") == 0 ||
                                               q.find("." + ns + ".") != std::string::npos)))
                return;
            const std::string rest = q.substr(q.rfind(ns + ".") + ns.size() + 1);
            if (rest.find('.') != std::string::npos) return;  // only direct members
            if (rest.rfind(prefix, 0) != 0) return;
            AplLspCompletionItem it;
            it.label = rest;
            it.insert_text = rest;
            it.kind = CompletionKindOf(d.kind);
            it.detail = d.header;
            it.documentation = AplLspDefMarkdown(d);
            it.replace_start = rstart;
            it.replace_end = col;
            AddUnique(out, seen, it);
        };
        for (const AplDef &d : doc.defs) offer(d);
        if (ws) {
            for (const auto &o : ws->docs) {
                if (o && o.get() != &doc) {
                    for (const AplDef &d : o->defs) offer(d);
                }
            }
        }
        return out;
    }
    // Plain names: this function's locals, then definitions here and in the workspace, then glyph names.
    const std::string prefix = qword;
    const int fn = EnclosingFunction(doc, line, col, true);
    if (fn >= 0) {
        const AplDef &f = doc.defs[static_cast<size_t>(fn)];
        std::vector<std::string> names = f.locals;
        for (const std::string *p : {&f.result, &f.left, &f.right, &f.left_operand, &f.right_operand}) {
            if (!p->empty() && (*p)[0] != '(') names.push_back(*p);
        }
        for (const std::string &l : f.labels) names.push_back(l);
        for (const std::string &n : names) {
            if (n.rfind(prefix, 0) != 0 || n == prefix || n.rfind("⎕", 0) == 0) continue;
            AplLspCompletionItem it;
            it.label = n;
            it.insert_text = n;
            it.kind = AplLspKind::Variable;
            it.detail = LocalRole(f, n).empty() ? "label" : LocalRole(f, n) + " of " + f.name;
            it.replace_start = qstart;
            it.replace_end = col;
            AddUnique(out, seen, it);
        }
    }
    auto offer_def = [&](const AplDef &d, bool here) {
        if (d.kind == AplDefKind::Label) return;
        if (!here && d.kind == AplDefKind::Variable) return;
        if (d.name.rfind(prefix, 0) != 0 || d.name == "?") return;
        // Top-level names, or members of the namespace the cursor is in.
        AplLspCompletionItem it;
        it.label = d.name;
        it.insert_text = d.name;
        if (!here && d.qualified != d.name) {
            // A member of another file's namespace: complete the qualified name.
            it.label = d.qualified;
            it.insert_text = d.qualified;
            it.filter_text = d.name;
        }
        it.kind = CompletionKindOf(d.kind);
        it.detail = d.header;
        it.documentation = AplLspDefMarkdown(d);
        it.replace_start = qstart;
        it.replace_end = col;
        if (it.insert_text == prefix) return;
        AddUnique(out, seen, it);
    };
    for (const AplDef &d : doc.defs) offer_def(d, true);
    if (ws) {
        for (const auto &o : ws->docs) {
            if (!o || o.get() == &doc || (!doc.path.empty() && o->path == doc.path)) continue;
            for (const AplDef &d : o->defs) offer_def(d, false);
        }
    }
    // Glyphs by name (rho -> ⍴), after two letters.
    if (prefix.size() >= 2) {
        const std::string lp = Lower(prefix);
        for (const apl::Glyph &g : apl::Glyphs()) {
            const std::string names = g.names;
            size_t p = 0;
            while (p < names.size()) {
                size_t e = names.find(' ', p);
                if (e == std::string::npos) e = names.size();
                const std::string nm = names.substr(p, e - p);
                p = e + 1;
                if (nm.rfind(lp, 0) != 0) continue;
                AplLspCompletionItem it;
                it.label = nm + " " + g.glyph;
                it.filter_text = nm;
                it.insert_text = g.glyph;
                it.kind = std::string(g.kind) == "operator" ? AplLspKind::Operator : AplLspKind::Function;
                it.detail = std::string(g.glyph) + " " +
                            (g.monadic[0] && g.dyadic[0] ? std::string(g.monadic) + " / " + g.dyadic
                                                         : std::string(g.monadic[0] ? g.monadic : g.dyadic));
                it.documentation = apl::GlyphMarkdown(g);
                it.replace_start = qstart;
                it.replace_end = col;
                AddUnique(out, seen, it);
            }
        }
    }
    return out;
}

AplLspSignatureHelp AplLspSignatureAt(const AplLspDocument &doc, int line, int col, const AplLspWorkspace *ws) {
    AplLspSignatureHelp none;
    if (line < 0 || static_cast<size_t>(line) >= doc.toks.size()) return none;
    const std::vector<Token> &ts = doc.toks[static_cast<size_t>(line)];
    // The statement around the cursor: between diamonds.
    size_t lo = 0, hi = ts.size();
    for (size_t i = 0; i < ts.size(); i++) {
        if (ts[i].kind == Tok::Diamond || ts[i].kind == Tok::Comment) {
            if (ts[i].end <= col) lo = i + 1;
            else if (hi == ts.size()) hi = i;
        }
    }
    auto try_tok = [&](const Token &t, bool before_cursor) -> AplLspSignatureHelp {
        if (t.kind != Tok::Name && t.kind != Tok::QualName) return none;
        for (const Found &f : Resolve(doc, ws, t.text, line, t.col)) {
            if (!IsFunctionKind(f.def->kind)) continue;
            AplLspSignatureHelp sh = SignatureOf(*f.def);
            const std::string &active = before_cursor ? f.def->right : f.def->left;
            sh.active_param = 0;
            for (size_t k = 0; k < sh.params.size(); k++) {
                if (sh.params[k] == active) sh.active_param = static_cast<int>(k);
            }
            return sh;
        }
        return none;
    };
    // Nearest function name left of the cursor (the cursor is in its right argument) ...
    for (size_t i = hi; i-- > lo;) {
        if (ts[i].end > col) continue;
        AplLspSignatureHelp sh = try_tok(ts[i], true);
        if (sh.found) return sh;
    }
    // ... else right of it (the cursor is in its left argument).
    for (size_t i = lo; i < hi; i++) {
        if (ts[i].col < col) continue;
        AplLspSignatureHelp sh = try_tok(ts[i], false);
        if (sh.found) return sh;
    }
    return none;
}

std::vector<AplLspLocation> AplLspDefinition(const AplLspDocument &doc, int line, int col, const AplLspWorkspace *ws) {
    std::vector<AplLspLocation> out;
    std::string name;
    AplLspRange range;
    if (!NameAt(doc, line, col, &name, &range)) return out;
    const int fn = EnclosingFunction(doc, line, col, true);
    if (fn >= 0 && name.find('.') == std::string::npos) {
        const AplDef &f = doc.defs[static_cast<size_t>(fn)];
        for (const AplDef &d : doc.defs) {
            if (d.kind == AplDefKind::Label && d.owner == fn && d.name == name) {
                out.push_back({"", d.sel_range});
                return out;
            }
        }
        if (!LocalRole(f, name).empty()) {
            // The name in the header line.
            for (const Token &t : doc.toks[static_cast<size_t>(f.range.line)]) {
                if ((t.kind == Tok::Name || t.kind == Tok::SysName) && t.text == name) {
                    out.push_back({"", RangeOf(f.range.line, t)});
                    return out;
                }
            }
        }
    }
    for (const Found &f : Resolve(doc, ws, name, line, col)) {
        out.push_back({f.doc == &doc ? std::string() : f.doc->path, f.def->sel_range});
    }
    return out;
}

std::vector<AplLspReference> AplLspReferences(const AplLspDocument &doc, int line, int col) {
    std::vector<AplLspReference> out;
    std::string name;
    AplLspRange range;
    if (!NameAt(doc, line, col, &name, &range) || name.find('.') != std::string::npos) return out;
    std::set<std::pair<int, int>> defs;
    for (const AplDef &d : doc.defs) {
        if (d.name == name) defs.insert({d.sel_range.line, d.sel_range.col});
    }
    for (size_t li = 0; li < doc.toks.size(); li++) {
        for (const Token &t : doc.toks[li]) {
            if (t.kind == Tok::Name && t.text == name) {
                AplLspReference r;
                r.range = RangeOf(static_cast<int>(li), t);
                r.is_definition = defs.count({static_cast<int>(li), t.col}) > 0;
                out.push_back(r);
            } else if (t.kind == Tok::QualName) {
                // ns.name / name.member: the segment that is the name.
                size_t p = 0;
                const std::string &s = t.text;
                while (p <= s.size()) {
                    size_t e = s.find('.', p);
                    if (e == std::string::npos) e = s.size();
                    if (s.compare(p, e - p, name) == 0 && e - p == name.size()) {
                        AplLspReference r;
                        r.range = AplLspRange{static_cast<int>(li), t.col + static_cast<int>(p), static_cast<int>(li),
                                              t.col + static_cast<int>(e)};
                        out.push_back(r);
                    }
                    p = e + 1;
                }
            }
        }
    }
    return out;
}

AplLspRenameTarget AplLspPrepareRename(const AplLspDocument &doc, int line, int col) {
    AplLspRenameTarget t;
    std::string name;
    AplLspRange range;
    if (!NameAt(doc, line, col, &name, &range)) return t;
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) {
        range.col += static_cast<int>(dot) + 1;
        name = name.substr(dot + 1);
    }
    t.found = true;
    t.range = range;
    t.placeholder = name;
    return t;
}

std::vector<AplLspTextEdit> AplLspRename(const AplLspDocument &doc, int line, int col, const std::string &new_name,
                                         std::string *error) {
    std::vector<AplLspTextEdit> edits;
    const AplLspRenameTarget target = AplLspPrepareRename(doc, line, col);
    if (!target.found) {
        *error = "no name here";
        return edits;
    }
    const std::vector<Token> check = apl::LexLine(new_name);
    if (check.size() != 1 || check[0].kind != Tok::Name || check[0].text != new_name) {
        *error = "'" + new_name + "' is not an APL name";
        return edits;
    }
    for (const AplLspReference &r : AplLspReferences(doc, target.range.line, target.range.col)) {
        edits.push_back({r.range, new_name});
    }
    return edits;
}

std::vector<AplLspSymbol> AplLspSymbols(const AplLspDocument &doc) {
    std::vector<AplLspSymbol> out;
    std::map<int, int> index;  // def index -> symbol index
    for (size_t i = 0; i < doc.defs.size(); i++) {
        const AplDef &d = doc.defs[i];
        if (d.kind == AplDefKind::Label) continue;
        AplLspSymbol s;
        s.name = d.name;
        s.detail = d.kind == AplDefKind::Variable ? std::string() : d.header;
        switch (d.kind) {
        case AplDefKind::Tradfn:
        case AplDefKind::Dfn: s.kind = AplLspSymbolKind::Function; break;
        case AplDefKind::TradOp:
        case AplDefKind::Dop: s.kind = AplLspSymbolKind::Operator; break;
        case AplDefKind::Namespace: s.kind = AplLspSymbolKind::Namespace; break;
        case AplDefKind::Class: s.kind = AplLspSymbolKind::Class; break;
        default: s.kind = AplLspSymbolKind::Variable; break;
        }
        s.range = d.range;
        s.sel_range = d.sel_range;
        s.parent = d.parent >= 0 && index.count(d.parent) ? index[d.parent] : -1;
        index[static_cast<int>(i)] = static_cast<int>(out.size());
        out.push_back(s);
    }
    return out;
}

std::vector<AplLspFold> AplLspFolds(const AplLspDocument &doc) { return doc.folds; }

std::vector<AplLspTextEdit> AplLspFormat(const std::vector<std::string> &lines, int width, std::string *error) {
    std::vector<AplLspTextEdit> edits;
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    aplfmt::Options o;
    if (width > 0) o.width = width;
    const aplfmt::Result r = aplfmt::Format(text, o);
    if (!r.ok) {
        *error = r.error_line > 0 ? "line " + std::to_string(r.error_line) + ": " + r.error : r.error;
        return edits;
    }
    if (r.text == text) return edits;
    // One edit over the changed lines only.
    std::vector<std::string> out;
    {
        std::string cur;
        for (char c : r.text) {
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
    size_t bl = lines.size(), bo = out.size();
    while (bl > a && bo > a && lines[bl - 1] == out[bo - 1]) {
        bl--;
        bo--;
    }
    AplLspTextEdit e;
    e.range.line = static_cast<int>(a);
    e.range.col = 0;
    if (bl < lines.size()) {
        e.range.end_line = static_cast<int>(bl);
        e.range.end_col = 0;
        for (size_t i = a; i < bo; i++) e.new_text += out[i] + "\n";
    } else {
        // The change reaches the last line.
        e.range.end_line = static_cast<int>(lines.size()) - 1;
        e.range.end_col = static_cast<int>(lines.back().size());
        for (size_t i = a; i < bo; i++) {
            if (i > a) e.new_text += "\n";
            e.new_text += out[i];
        }
        if (a > 0 && a >= bo) {
            // Lines removed at the end (not expected from this formatter).
            e.range.line = static_cast<int>(a) - 1;
            e.range.col = static_cast<int>(lines[a - 1].size());
        }
    }
    edits.push_back(e);
    return edits;
}
