// mep-bash-lsp's document index: see bash_lsp_index.h.
#include "bash_lsp_index.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "bash_lsp.h"

namespace bash_lsp {

namespace {

bool RangeSmaller(const BashRange &a, const BashRange &b) {
    if (a.end_line - a.line != b.end_line - b.line) return a.end_line - a.line < b.end_line - b.line;
    return a.end_col - a.col < b.end_col - b.col;
}

// The option letters of a word like `-rx` / `+x`, or "" when it is not one.
std::string OptionLetters(const BashWord &w) {
    std::string t;
    if (!PlainWord(w, &t) || t.size() < 2 || (t[0] != '-' && t[0] != '+') || t == "--") return "";
    return t.substr(1);
}

bool Before(const BashRange &a, const BashRange &b) {
    if (a.line != b.line) return a.line < b.line;
    return a.col < b.col;
}

class Collector : public BashVisitor {
public:
    explicit Collector(Index *ix) : ix_(ix) {}

    struct InfoUpdate {
        size_t occ;
        VarInfo flags;
    };
    std::vector<InfoUpdate> updates;
    std::set<std::string> assoc;  // declared -A

    bool Command(const BashCommand &c, const BashCommand *fn) override {
        // exec {fd}>file: bash stores the descriptor in fd.
        for (const BashRedirect &r : c.redirects) {
            if (r.fd.size() > 2 && r.fd[0] == '{') {
                BashRange nr = r.fd_range;
                nr.col += 1;
                nr.end_col -= 1;
                AddVar(r.fd.substr(1, r.fd.size() - 2), nr, OccKind::Write, fn, VarInfo{}).reads_too = true;
            }
        }
        switch (c.kind) {
            case BashCommandKind::Function: {
                Occurrence o;
                o.name = c.name;
                o.range = c.name_range;
                o.kind = OccKind::Declare;
                o.fn = fn;
                ix_->funcs.push_back(o);
                FunctionDef d;
                d.name = c.name;
                d.name_range = c.name_range;
                d.cmd = &c;
                d.parent = fn;
                ix_->functions.push_back(d);
                break;
            }
            case BashCommandKind::For:
            case BashCommandKind::Select:
                if (BashIsName(c.var)) AddVar(c.var, c.var_range, OccKind::Write, fn, VarInfo{});
                break;
            case BashCommandKind::Coproc:
                if (BashIsName(c.name)) {
                    VarInfo vi;
                    vi.array = true;
                    AddVar(c.name, c.name_range, OccKind::Write, fn, vi);
                }
                break;
            case BashCommandKind::Cond:
                for (size_t k = 0; k + 1 < c.cond.size(); k++) {
                    if (c.cond[k].kind == BashCondTerm::UnaryOp &&
                        (c.cond[k].word.text == "-v" || c.cond[k].word.text == "-R")) {
                        TestVOperand(c.cond[k + 1].word, fn);
                    }
                }
                break;
            case BashCommandKind::Simple:
                Simple(c, fn);
                break;
            default:
                break;
        }
        return true;
    }

    // Subscripts of an array, by name: if the array turns out to be
    // associative, the names in them are keys, not variables.
    std::vector<std::pair<std::string, BashRange>> subscripts;

    void Part(const BashWordPart &p, const BashCommand *fn) override {
        if (p.kind == BashPartKind::SingleQuoted || p.kind == BashPartKind::Literal) StringRefs(p.text);
        if (p.kind != BashPartKind::Param) return;
        if (p.has_subscript) subscripts.emplace_back(p.name, p.subscript_arith.range);
        if (p.indirect) {
            if (p.op == "!*" || p.op == "!@") {
                ix_->indirect_prefix = true;
                ix_->indirect_names.push_back(p.name);
                return;
            }
            if (!p.has_subscript) ix_->indirect_any = true;
        }
        if (!BashIsName(p.name)) return;
        const bool assigns = p.op == "=" || p.op == ":=";
        Occurrence &o = AddVar(p.name, p.name_range, assigns ? OccKind::Write : OccKind::Read, fn, VarInfo{});
        o.quoted = p.quoted;
        o.reads_too = assigns;
        o.guarded = p.op == "-" || p.op == ":-" || p.op == "+" || p.op == ":+" || p.op == "?" || p.op == ":?" ||
                    assigns;
        if (assigns) updates.back().flags.scalar = true;
    }

    void ArithRef(const BashArithRef &r, const BashCommand *fn) override {
        Occurrence &o = AddVar(r.name, r.range, r.assigned ? OccKind::Write : OccKind::Read, fn, VarInfo{});
        o.in_arith = true;
        // `x += 1`, `x++`: a write that reads the old value. A plain
        // `x = 1` reads nothing, but arithmetic assignment is rare enough
        // that counting it as a read costs nothing.
        o.reads_too = r.assigned;
        if (r.assigned) updates.back().flags.scalar = true;
    }

private:
    Index *ix_;

    void StringRefs(const std::string &t) {
        for (size_t k = 0; k + 1 < t.size(); k++) {
            if (t[k] != '$') continue;
            size_t a = k + 1;
            if (t[a] == '{') a++;
            if (a < t.size() && t[a] == '!') a++;
            size_t b = a;
            while (b < t.size() && (std::isalnum(static_cast<unsigned char>(t[b])) || t[b] == '_')) b++;
            if (b > a && !std::isdigit(static_cast<unsigned char>(t[a]))) ix_->string_refs.insert(t.substr(a, b - a));
        }
    }

    Occurrence &AddVar(const std::string &name, const BashRange &range, OccKind kind, const BashCommand *fn,
                       const VarInfo &flags, bool renameable = true) {
        Occurrence o;
        o.name = name;
        o.range = range;
        o.kind = kind;
        o.fn = fn;
        o.renameable = renameable;
        ix_->vars.push_back(o);
        updates.push_back(InfoUpdate{ix_->vars.size() - 1, flags});
        return ix_->vars.back();
    }

    void AddFuncRef(const BashWord &w, const BashCommand *fn) {
        std::string name;
        if (!PlainWord(w, &name)) return;
        Occurrence o;
        o.name = name;
        o.range = w.range;
        o.kind = OccKind::Read;
        o.fn = fn;
        ix_->funcs.push_back(o);
    }

    // The operand of -v: a name, possibly with a subscript, possibly quoted.
    void TestVOperand(const BashWord &w, const BashCommand *fn) {
        std::string text;
        if (!BashWordLiteral(w, &text)) return;
        const size_t b = text.find('[');
        const std::string name = b == std::string::npos ? text : text.substr(0, b);
        if (!BashIsName(name)) return;
        BashRange r = w.range;
        std::string plain;
        const bool literal = PlainWord(w, &plain);
        if (literal) {
            r.end_line = r.line;
            r.end_col = r.col + static_cast<int>(name.size());
        }
        AddVar(name, r, OccKind::Read, fn, VarInfo{}, literal).guarded = true;
    }

    // A word naming a variable as an argument (`read name`, `unset name`).
    Occurrence *NameArg(const BashWord &w, OccKind kind, const BashCommand *fn, VarInfo flags) {
        if (kind == OccKind::Write && !flags.array) flags.scalar = true;
        std::string text;
        if (!BashWordLiteral(w, &text)) return nullptr;
        const size_t b = text.find('[');
        const std::string name = b == std::string::npos ? text : text.substr(0, b);
        if (!BashIsName(name)) return nullptr;
        std::string plain;
        const bool literal = PlainWord(w, &plain);
        BashRange r = w.range;
        if (literal) {
            r.end_line = r.line;
            r.end_col = r.col + static_cast<int>(name.size());
        }
        return &AddVar(name, r, kind, fn, flags, literal);
    }

    void Simple(const BashCommand &c, const BashCommand *fn) {
        for (const BashAssignment &a : c.assignments) {
            if (a.has_subscript) subscripts.emplace_back(a.name, a.subscript_arith.range);
        }
        for (const BashAssignment &a : c.assignments) {
            if (a.declaration) continue;
            VarInfo vi;
            vi.array = a.is_array || a.has_subscript;
            vi.scalar = !vi.array;
            Occurrence &o = AddVar(a.name, a.name_range, OccKind::Write, fn, vi);
            o.reads_too = a.append;
        }
        if (c.words.empty()) return;
        std::string cmd;
        if (!PlainWord(c.words[0], &cmd)) return;
        AddFuncRef(c.words[0], fn);
        const std::vector<BashWord> &w = c.words;
        if (cmd == "local" || cmd == "declare" || cmd == "typeset" || cmd == "export" || cmd == "readonly") {
            Declaration(c, fn, cmd);
        } else if (cmd == "read") {
            Read(c, fn);
        } else if (cmd == "mapfile" || cmd == "readarray") {
            Mapfile(c, fn);
        } else if (cmd == "getopts") {
            if (w.size() >= 3) NameArg(w[2], OccKind::Write, fn, VarInfo{});
        } else if (cmd == "printf") {
            std::string o;
            if (w.size() >= 3 && PlainWord(w[1], &o) && o == "-v") NameArg(w[2], OccKind::Write, fn, VarInfo{});
        } else if (cmd == "wait") {
            for (size_t k = 1; k + 1 < w.size(); k++) {
                if (OptionLetters(w[k]).find('p') != std::string::npos) NameArg(w[k + 1], OccKind::Write, fn, VarInfo{});
            }
        } else if (cmd == "unset") {
            bool functions = false;
            for (size_t k = 1; k < w.size(); k++) {
                const std::string letters = OptionLetters(w[k]);
                if (!letters.empty()) {
                    if (letters.find('f') != std::string::npos) functions = true;
                    continue;
                }
                if (functions) {
                    AddFuncRef(w[k], fn);
                } else if (Occurrence *o = NameArg(w[k], OccKind::Write, fn, VarInfo{})) {
                    o->reads_too = true;  // not a value: it does not make the name "assigned but unused"
                }
            }
        } else if (cmd == "eval") {
            ix_->uses_eval = true;
        } else if (cmd == "source" || cmd == ".") {
            ix_->sources_files = true;
        } else if (cmd == "set") {
            for (size_t k = 1; k < w.size(); k++) {
                std::string t, next;
                if (!PlainWord(w[k], &t)) continue;
                if (t.size() > 1 && t[0] == '-' && t[1] != '-' && t.find('e') != std::string::npos &&
                    t.find('o') == std::string::npos) {
                    ix_->set_e = true;
                }
                if (t.size() > 1 && t[0] == '-' && t.back() == 'o' && k + 1 < w.size() && PlainWord(w[k + 1], &next) &&
                    next == "errexit") {
                    ix_->set_e = true;
                }
            }
        } else if (cmd == "complete" || cmd == "compgen" || cmd == "compopt") {
            for (size_t k = 1; k + 1 < w.size(); k++) {
                std::string t;
                if (PlainWord(w[k], &t) && t == "-F") AddFuncRef(w[k + 1], fn);
            }
        } else if (cmd == "trap") {
            if (w.size() >= 2) {
                std::string t;
                if (PlainWord(w[1], &t) && BashIsName(t)) {
                    AddFuncRef(w[1], fn);
                } else {
                    ix_->uses_eval = true;  // a string of code
                }
            }
        } else if (cmd == "test" || cmd == "[") {
            for (size_t k = 1; k + 1 < w.size(); k++) {
                std::string t;
                if (PlainWord(w[k], &t) && t == "-v") TestVOperand(w[k + 1], fn);
            }
        } else if (cmd == "bash" || cmd == "sh" || cmd == "su" || cmd == "ssh") {
            ix_->uses_eval = true;
        } else if (cmd == "let") {
            // Each argument is an arithmetic expression; the names in
            // a literal one are read (and assigned when followed by `=`).
            for (size_t k = 1; k < w.size(); k++) {
                std::string t;
                if (!BashWordLiteral(w[k], &t)) continue;
                size_t p = 0;
                while (p < t.size()) {
                    if ((t[p] >= 'a' && t[p] <= 'z') || (t[p] >= 'A' && t[p] <= 'Z') || t[p] == '_') {
                        size_t q = p;
                        while (q < t.size() && (std::isalnum(static_cast<unsigned char>(t[q])) || t[q] == '_')) q++;
                        const std::string name = t.substr(p, q - p);
                        bool assigned = q < t.size() && t[q] == '=' && (q + 1 >= t.size() || t[q + 1] != '=');
                        if (q + 1 < t.size() && std::strchr("+-*/%", t[q]) != nullptr && t[q + 1] == '=') assigned = true;
                        if (q + 1 < t.size() && ((t[q] == '+' && t[q + 1] == '+') || (t[q] == '-' && t[q + 1] == '-'))) {
                            assigned = true;
                        }
                        BashRange r = w[k].range;
                        std::string unused;
                        const bool exact = PlainWord(w[k], &unused);
                        if (exact) {
                            r.col += static_cast<int>(p);
                            r.end_line = r.line;
                            r.end_col = r.col + static_cast<int>(q - p);
                        }
                        Occurrence &o = AddVar(name, r, assigned ? OccKind::Write : OccKind::Read, fn, VarInfo{}, exact);
                        o.in_arith = true;
                        o.reads_too = assigned;
                        p = q;
                    } else if (std::isdigit(static_cast<unsigned char>(t[p]))) {
                        while (p < t.size() && (std::isalnum(static_cast<unsigned char>(t[p])) || t[p] == '#')) p++;
                    } else {
                        p++;
                    }
                }
            }
        }
    }

    void Declaration(const BashCommand &c, const BashCommand *fn, const std::string &cmd) {
        bool local = cmd == "local" || ((cmd == "declare" || cmd == "typeset") && fn != nullptr);
        VarInfo flags;
        flags.exported = cmd == "export";
        flags.readonly = cmd == "readonly";
        bool functions = false;
        bool print = false;
        bool assoc_pending = false;
        const std::vector<BashWord> &w = c.words;
        for (size_t k = 1; k < w.size(); k++) {
            const std::string letters = OptionLetters(w[k]);
            if (letters.empty()) continue;
            const bool on = PlainWordText(w[k])[0] == '-';
            for (char ch : letters) {
                if (ch == 'g') local = !on ? local : false;
                if (ch == 'a' || ch == 'A') flags.array = on;
                if (ch == 'A' && on) assoc_pending = true;
                if (ch == 'r') flags.readonly = on;
                if (ch == 'x') flags.exported = on;
                if (ch == 'n') flags.nameref = on;
                if (ch == 'i') flags.integer = on;
                if (ch == 'f' || ch == 'F') functions = true;
                if (ch == 'p') print = true;
            }
        }
        if (local && fn == nullptr) local = false;  // `local` at top level: an error the linter reports
        if (assoc_pending) {
            for (const BashAssignment &a : c.assignments) {
                if (a.declaration) assoc.insert(a.name);
            }
            for (size_t k = 1; k < w.size(); k++) {
                if (BashIsName(w[k].text)) assoc.insert(w[k].text);
            }
        }
        for (const BashAssignment &a : c.assignments) {
            if (!a.declaration) continue;
            VarInfo vi = flags;
            if (a.is_array || a.has_subscript) vi.array = true;
            if (!vi.array) vi.scalar = true;
            // `local x=1` declares and assigns: a write, as `x=1` is.
            Occurrence &o = AddVar(a.name, a.name_range, OccKind::Write, fn, vi);
            o.reads_too = a.append;
            if (local) ix_->locals[fn].insert(a.name);
            if (flags.nameref) {
                std::string target;
                if (PlainWord(a.value, &target) && BashIsName(target)) {
                    ix_->indirect_names.push_back(target);
                    AddVar(target, a.value.range, OccKind::Read, fn, VarInfo{});
                }
            }
        }
        for (size_t k = 1; k < w.size(); k++) {
            if (!OptionLetters(w[k]).empty()) continue;
            // Assignment words are in c.assignments; their parts here are
            // their values', so look at the raw text.
            const std::string &t = w[k].text;
            if (t.find('=') != std::string::npos) continue;
            std::string plain;
            if (!PlainWord(w[k], &plain)) continue;
            if (functions) {
                AddFuncRef(w[k], fn);
                continue;
            }
            if (print || !BashIsName(t)) continue;
            AddVar(t, w[k].range, OccKind::Declare, fn, flags);
            if (local) ix_->locals[fn].insert(t);
        }
    }

    static std::string PlainWordText(const BashWord &w) {
        std::string t;
        PlainWord(w, &t);
        return t.empty() ? std::string(" ") : t;
    }

    void Read(const BashCommand &c, const BashCommand *fn) {
        const std::vector<BashWord> &w = c.words;
        bool names = false;
        for (size_t k = 1; k < w.size(); k++) {
            std::string t;
            const bool plain = PlainWord(w[k], &t);
            if (!names && plain && t == "--") {
                names = true;
                continue;
            }
            if (!names && plain && t.size() > 1 && t[0] == '-') {
                for (size_t j = 1; j < t.size(); j++) {
                    if (std::strchr("adinNptu", t[j]) == nullptr) continue;
                    if (j + 1 == t.size() && k + 1 < w.size()) {
                        if (t[j] == 'a') {
                            VarInfo vi;
                            vi.array = true;
                            NameArg(w[k + 1], OccKind::Write, fn, vi);
                        }
                        k++;
                    }
                    break;
                }
                continue;
            }
            names = true;
            Occurrence *o = NameArg(w[k], OccKind::Write, fn, VarInfo{});
            // The last name takes the rest of the line: `read -r a b rest`
            // names it to keep it out of b, not to use it.
            if (o && k + 1 == w.size()) o->reads_too = true;
        }
    }

    void Mapfile(const BashCommand &c, const BashCommand *fn) {
        const std::vector<BashWord> &w = c.words;
        for (size_t k = 1; k < w.size(); k++) {
            std::string t;
            const bool plain = PlainWord(w[k], &t);
            if (plain && t.size() > 1 && t[0] == '-') {
                const char last = t.back();
                if (std::strchr("dnOsuCc", last) != nullptr && k + 1 < w.size()) {
                    if (last == 'C') AddFuncRef(w[k + 1], fn);
                    k++;
                }
                continue;
            }
            VarInfo vi;
            vi.array = true;
            NameArg(w[k], OccKind::Write, fn, vi);
        }
    }
};

// Finds, for every function definition, whether it runs unconditionally.
class ConditionalFinder : public CommandWalker {
public:
    explicit ConditionalFinder(Index *ix) : ix_(ix) {}
    void Visit(const BashCommand &cmd, const BashCommand *, bool conditional, const BashAndOr *,
               const BashPipeline *) override {
        if (cmd.kind != BashCommandKind::Function) return;
        for (FunctionDef &d : ix_->functions) {
            if (d.cmd == &cmd) d.conditional = conditional;
        }
    }

private:
    Index *ix_;
};

void WalkL(const BashList &list, CommandWalker &w, const BashCommand *fn, bool cond);

void WalkC(const BashCommand &c, CommandWalker &w, const BashCommand *fn, bool cond, const BashAndOr *ao,
           const BashPipeline *pl) {
    w.Visit(c, fn, cond, ao, pl);
    const bool branches = c.kind == BashCommandKind::If || c.kind == BashCommandKind::While ||
                          c.kind == BashCommandKind::Until || c.kind == BashCommandKind::For ||
                          c.kind == BashCommandKind::Select || c.kind == BashCommandKind::ForArith ||
                          c.kind == BashCommandKind::Case;
    for (const auto &b : c.bodies) {
        if (b) WalkL(*b, w, fn, cond || branches);
    }
    for (const BashCaseItem &ci : c.case_items) {
        if (ci.body) WalkL(*ci.body, w, fn, true);
    }
    if (c.kind == BashCommandKind::Function) {
        for (const BashCommand &s : c.sub) WalkC(s, w, &c, false, nullptr, nullptr);
    } else {
        for (const BashCommand &s : c.sub) WalkC(s, w, fn, cond, nullptr, nullptr);
    }
}

void WalkL(const BashList &list, CommandWalker &w, const BashCommand *fn, bool cond) {
    for (const BashAndOr &ao : list.items) {
        const bool c2 = cond || ao.pipelines.size() > 1;
        for (const BashPipeline &pl : ao.pipelines) {
            for (const BashCommand &c : pl.commands) WalkC(c, w, fn, c2 || pl.commands.size() > 1, &ao, &pl);
        }
    }
}

}  // namespace

void WalkCommands(const BashList &list, CommandWalker &w) { WalkL(list, w, nullptr, false); }

bool PlainWord(const BashWord &w, std::string *out) {
    if (w.parts.size() != 1 || w.parts[0].kind != BashPartKind::Literal) return false;
    const std::string &t = w.parts[0].text;
    if (t.find_first_of("\\'\"") != std::string::npos) return false;
    *out = t;
    return true;
}

int FirstNonBlank(const std::string &line) {
    for (size_t k = 0; k < line.size(); k++) {
        if (line[k] != ' ' && line[k] != '\t') return static_cast<int>(k);
    }
    return -1;
}

bool IsKnownShellVariable(const std::string &name) {
    if (BashLspFind(BashLspVariableVocab(), name)) return true;
    static const char *const env[] = {"USER",   "LOGNAME", "TERM",    "EDITOR", "VISUAL",    "PAGER",
                                      "DISPLAY", "MAIL",   "SHELL",   "TZ",     "COLORTERM", "SSH_AUTH_SOCK",
                                      "LD_LIBRARY_PATH", "MANPATH", "INFOPATH", "PYTHONPATH", "CC", "CXX",
                                      "CFLAGS", "LDFLAGS", "MAKEFLAGS", "HISTCONTROL", "HISTFILESIZE",
                                      "HISTIGNORE", "HISTTIMEFORMAT", "INPUTRC", "FIGNORE", "FUNCNEST",
                                      "TIMEFORMAT", "auto_resume", "histchars", "POSIXLY_CORRECT",
                                      "EXECIGNORE", "READLINE_POINT", "READLINE_MARK", "COMP_KEY",
                                      "COMP_TYPE", "COMP_WORDBREAKS", "COPROC", "ENV", "EMACS", "INSIDE_EMACS",
                                      "NO_COLOR", "CI", "GITHUB_ACTIONS"};
    for (const char *e : env) {
        if (name == e) return true;
    }
    return name.rfind("LC_", 0) == 0 || name.rfind("XDG_", 0) == 0 || name.rfind("BASH_", 0) == 0;
}

const Occurrence *Index::VarAt(int line, int col) const {
    const Occurrence *best = nullptr;
    for (const Occurrence &o : vars) {
        if (!BashRangeTouches(o.range, line, col)) continue;
        if (!best || RangeSmaller(o.range, best->range)) best = &o;
    }
    return best;
}

const Occurrence *Index::FuncAt(int line, int col) const {
    const Occurrence *best = nullptr;
    for (const Occurrence &o : funcs) {
        if (!BashRangeTouches(o.range, line, col)) continue;
        if (!best || RangeSmaller(o.range, best->range)) best = &o;
    }
    return best;
}

bool Index::DefinesFunction(const std::string &name) const {
    for (const FunctionDef &d : functions) {
        if (d.name == name) return true;
    }
    return false;
}

std::unique_ptr<Index> BuildIndex(const std::vector<std::string> &lines) {
    auto ix = std::make_unique<Index>();
    ix->lines = lines.empty() ? std::vector<std::string>{std::string()} : lines;
    ix->parse = BashParse(ix->lines);

    const std::string &first = ix->lines[0];
    if (first.rfind("#!", 0) == 0) {
        ix->has_shebang = true;
        const bool env = first.find("/env") != std::string::npos;
        std::string interp = first.substr(2);
        // The interpreter's base name, and its flags.
        size_t p = interp.find_first_not_of(" \t");
        interp = p == std::string::npos ? "" : interp.substr(p);
        std::string prog = interp.substr(0, interp.find_first_of(" \t"));
        std::string rest = interp.size() > prog.size() ? interp.substr(prog.size()) : "";
        if (env) {
            const size_t q = rest.find_first_not_of(" \t");
            rest = q == std::string::npos ? "" : rest.substr(q);
            if (rest.rfind("-S", 0) == 0) rest = rest.substr(2);
            const size_t r = rest.find_first_not_of(" \t");
            rest = r == std::string::npos ? "" : rest.substr(r);
            prog = rest.substr(0, rest.find_first_of(" \t"));
            rest = rest.size() > prog.size() ? rest.substr(prog.size()) : "";
        }
        const std::string base = prog.substr(prog.rfind('/') == std::string::npos ? 0 : prog.rfind('/') + 1);
        ix->posix_sh = base == "sh" || base == "dash" || base == "ash" || base == "posh";
        // Flags on the interpreter line: `#!/bin/bash -eu`.
        size_t k = 0;
        while (k < rest.size()) {
            while (k < rest.size() && (rest[k] == ' ' || rest[k] == '\t')) k++;
            const size_t e = rest.find_first_of(" \t", k);
            const std::string tok = rest.substr(k, e == std::string::npos ? std::string::npos : e - k);
            if (tok.size() > 1 && tok[0] == '-' && tok[1] != '-' && tok.find('e') != std::string::npos) ix->set_e = true;
            if (e == std::string::npos) break;
            k = e;
        }
    }

    Collector collector(ix.get());
    BashWalk(ix->parse.program, collector);

    // The keys of associative arrays are strings: drop the "names" read in them.
    std::vector<BashRange> keys;
    for (const auto &[name, range] : collector.subscripts) {
        if (collector.assoc.count(name)) keys.push_back(range);
    }
    if (!keys.empty()) {
        std::vector<bool> drop(ix->vars.size(), false);
        for (size_t k = 0; k < ix->vars.size(); k++) {
            const Occurrence &o = ix->vars[k];
            if (!o.in_arith) continue;
            for (const BashRange &r : keys) {
                if (BashRangeContains(r, o.range.line, o.range.col)) drop[k] = true;
            }
        }
        std::vector<Occurrence> kept;
        std::vector<Collector::InfoUpdate> updates;
        std::vector<size_t> remap(ix->vars.size(), 0);
        for (size_t k = 0; k < ix->vars.size(); k++) {
            if (drop[k]) continue;
            remap[k] = kept.size();
            kept.push_back(ix->vars[k]);
        }
        for (const Collector::InfoUpdate &u : collector.updates) {
            if (!drop[u.occ]) updates.push_back(Collector::InfoUpdate{remap[u.occ], u.flags});
        }
        ix->vars = std::move(kept);
        collector.updates = std::move(updates);
    }

    // Scopes, now that every function's locals are known.
    for (Occurrence &o : ix->vars) {
        o.scope = nullptr;
        if (o.fn) {
            const auto it = ix->locals.find(o.fn);
            if (it != ix->locals.end() && it->second.count(o.name)) o.scope = o.fn;
        }
    }
    for (const Collector::InfoUpdate &u : collector.updates) {
        const Occurrence &o = ix->vars[u.occ];
        VarInfo &vi = ix->info[{o.scope, o.name}];
        vi.array = vi.array || u.flags.array;
        vi.scalar = vi.scalar || u.flags.scalar;
        vi.readonly = vi.readonly || u.flags.readonly;
        vi.exported = vi.exported || u.flags.exported;
        vi.nameref = vi.nameref || u.flags.nameref;
        vi.integer = vi.integer || u.flags.integer;
    }

    // Calls only count for functions the file defines.
    std::vector<Occurrence> funcs;
    for (const Occurrence &o : ix->funcs) {
        if (o.kind == OccKind::Declare || ix->DefinesFunction(o.name)) funcs.push_back(o);
    }
    ix->funcs = std::move(funcs);

    std::stable_sort(ix->vars.begin(), ix->vars.end(),
                     [](const Occurrence &a, const Occurrence &b) { return Before(a.range, b.range); });
    std::stable_sort(ix->funcs.begin(), ix->funcs.end(),
                     [](const Occurrence &a, const Occurrence &b) { return Before(a.range, b.range); });

    ConditionalFinder finder(ix.get());
    WalkCommands(ix->parse.program, finder);
    return ix;
}

}  // namespace bash_lsp
