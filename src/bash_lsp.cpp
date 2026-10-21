// mep-bash-lsp: outline, folding, definition, references, rename and
// formatting. Diagnostics live in bash_lsp_diagnostics.cpp; completion,
// hover and signature help in bash_lsp_completion.cpp.
#include "bash_lsp.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "bash_lsp_index.h"

using bash_lsp::BuildIndex;
using bash_lsp::FirstNonBlank;
using bash_lsp::FunctionDef;
using bash_lsp::Index;
using bash_lsp::Occurrence;
using bash_lsp::OccKind;

namespace {

bool SameVar(const Occurrence &a, const Occurrence &b) { return a.name == b.name && a.scope == b.scope; }

// The occurrence under the cursor: a variable, else a function.
struct Target {
    const Occurrence *occ = nullptr;
    bool function = false;
};

Target TargetAt(const Index &ix, int line, int col) {
    Target t;
    if (const Occurrence *v = ix.VarAt(line, col)) {
        t.occ = v;
        return t;
    }
    if (const Occurrence *f = ix.FuncAt(line, col)) {
        t.occ = f;
        t.function = true;
    }
    return t;
}

std::vector<const Occurrence *> Occurrences(const Index &ix, const Target &t) {
    std::vector<const Occurrence *> out;
    if (!t.occ) return out;
    if (t.function) {
        for (const Occurrence &o : ix.funcs) {
            if (o.name == t.occ->name) out.push_back(&o);
        }
    } else {
        for (const Occurrence &o : ix.vars) {
            if (SameVar(o, *t.occ)) out.push_back(&o);
        }
    }
    return out;
}

// The definition among a name's occurrences: a declaration (local x,
// declare x) first, else the first assignment.
const Occurrence *DefinitionOf(const std::vector<const Occurrence *> &occs) {
    for (const Occurrence *o : occs) {
        if (o->kind == OccKind::Declare) return o;
    }
    for (const Occurrence *o : occs) {
        if (o->kind == OccKind::Write) return o;
    }
    return nullptr;
}

bool ValidFunctionName(const std::string &s) {
    if (s.empty() || BashIsReservedWord(s)) return false;
    for (char c : s) {
        if (std::strchr(" \t\n;&|()<>'\"\\$`=/", c) != nullptr) return false;
    }
    return !(s[0] >= '0' && s[0] <= '9');
}

}  // namespace

// --- Document symbols ----------------------------------------------------

std::vector<BashLspSymbol> BashLspSymbols(const std::vector<std::string> &lines) {
    const auto ix = BuildIndex(lines);
    struct Item {
        BashRange at;
        BashLspSymbol sym;
        const BashCommand *cmd = nullptr;
        const BashCommand *parent = nullptr;
    };
    std::vector<Item> items;
    for (const FunctionDef &d : ix->functions) {
        Item it;
        it.at = d.cmd->range;
        it.sym.name = d.name;
        it.sym.detail = "function";
        it.sym.kind = BashLspSymbolKind::Function;
        it.sym.range = d.cmd->range;
        it.sym.sel_range = d.name_range;
        it.cmd = d.cmd;
        it.parent = d.parent;
        items.push_back(it);
    }
    std::set<std::string> seen;
    for (const Occurrence &o : ix->vars) {
        if (o.fn || o.kind == OccKind::Read || !o.renameable || seen.count(o.name)) continue;
        seen.insert(o.name);
        const auto info = ix->info.find({nullptr, o.name});
        Item it;
        it.at = o.range;
        it.sym.name = o.name;
        it.sym.kind = info != ix->info.end() && info->second.readonly ? BashLspSymbolKind::Constant
                                                                       : BashLspSymbolKind::Variable;
        const std::string &text = ix->lines[static_cast<size_t>(o.range.line)];
        std::string detail = text.substr(static_cast<size_t>(o.range.col));
        const size_t eq = detail.find('=');
        detail = eq == std::string::npos ? "" : detail.substr(eq + 1);
        if (detail.size() > 40) detail = detail.substr(0, 37) + "...";
        if (info != ix->info.end() && info->second.array) detail = detail.empty() ? "array" : detail;
        it.sym.detail = detail;
        it.sym.range = o.range;
        it.sym.range.col = 0;
        it.sym.range.end_line = o.range.line;
        it.sym.range.end_col = static_cast<int>(text.size());
        it.sym.sel_range = o.range;
        items.push_back(it);
    }
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        if (a.at.line != b.at.line) return a.at.line < b.at.line;
        return a.at.col < b.at.col;
    });
    std::vector<BashLspSymbol> out;
    std::map<const BashCommand *, int> index_of;
    for (Item &it : items) {
        if (it.parent) {
            const auto p = index_of.find(it.parent);
            if (p != index_of.end()) it.sym.parent = p->second;
        }
        if (it.cmd) index_of[it.cmd] = static_cast<int>(out.size());
        out.push_back(it.sym);
    }
    return out;
}

// --- Folding -------------------------------------------------------------

namespace {

class FoldCollector : public BashVisitor {
public:
    std::map<int, int> folds;  // start line -> end line

    void Add(int a, int b) {
        if (b <= a) return;
        auto it = folds.find(a);
        if (it == folds.end() || it->second < b) folds[a] = b;
    }

    bool Command(const BashCommand &c, const BashCommand *) override {
        switch (c.kind) {
            case BashCommandKind::Simple:
                for (const BashAssignment &a : c.assignments) {
                    if (a.is_array) Add(a.range.line, a.range.end_line);
                }
                break;
            case BashCommandKind::Case:
                Add(c.range.line, c.range.end_line);
                for (const BashCaseItem &ci : c.case_items) Add(ci.range.line, ci.range.end_line);
                break;
            default:
                Add(c.range.line, c.range.end_line);
                break;
        }
        for (const BashRedirect &r : c.redirects) {
            if (r.heredoc) Add(r.range.line, r.unterminated ? r.body_range.end_line : r.body_range.end_line);
        }
        return true;
    }

    void Part(const BashWordPart &p, const BashCommand *) override {
        if (p.kind == BashPartKind::CommandSub || p.kind == BashPartKind::ProcessSub) Add(p.range.line, p.range.end_line);
    }
};

}  // namespace

std::vector<BashLspFold> BashLspFolds(const std::vector<std::string> &lines) {
    const auto ix = BuildIndex(lines);
    FoldCollector fc;
    BashWalk(ix->parse.program, fc);
    std::vector<BashLspFold> out;
    for (const auto &[a, b] : fc.folds) out.push_back(BashLspFold{a, b, ""});
    // Runs of comment lines (not the interpreter line).
    int run_start = -1;
    for (size_t k = 0; k <= ix->lines.size(); k++) {
        bool comment = false;
        if (k < ix->lines.size()) {
            const int f = FirstNonBlank(ix->lines[k]);
            comment = f >= 0 && ix->lines[k][static_cast<size_t>(f)] == '#' && !(k == 0 && ix->has_shebang);
        }
        if (comment && run_start < 0) run_start = static_cast<int>(k);
        if (!comment && run_start >= 0) {
            if (static_cast<int>(k) - 1 > run_start) out.push_back(BashLspFold{run_start, static_cast<int>(k) - 1, "comment"});
            run_start = -1;
        }
    }
    std::sort(out.begin(), out.end(), [](const BashLspFold &a, const BashLspFold &b) {
        return a.start_line != b.start_line ? a.start_line < b.start_line : a.end_line > b.end_line;
    });
    return out;
}

// --- Definition / references / rename -----------------------------------

BashLspLocation BashLspDefinition(const std::vector<std::string> &lines, int line, int col) {
    const auto ix = BuildIndex(lines);
    BashLspLocation loc;
    const Target t = TargetAt(*ix, line, col);
    if (!t.occ) return loc;
    if (t.function) {
        for (const FunctionDef &d : ix->functions) {
            if (d.name == t.occ->name) {
                loc.found = true;
                loc.range = d.name_range;
                return loc;
            }
        }
        return loc;
    }
    if (const Occurrence *d = DefinitionOf(Occurrences(*ix, t))) {
        loc.found = true;
        loc.range = d->range;
    }
    return loc;
}

std::vector<BashLspReference> BashLspReferences(const std::vector<std::string> &lines, int line, int col) {
    const auto ix = BuildIndex(lines);
    std::vector<BashLspReference> out;
    const Target t = TargetAt(*ix, line, col);
    const std::vector<const Occurrence *> occs = Occurrences(*ix, t);
    const Occurrence *def = t.function ? nullptr : DefinitionOf(occs);
    for (const Occurrence *o : occs) {
        BashLspReference r;
        r.range = o->range;
        r.is_write = o->kind != OccKind::Read;
        r.is_definition = t.function ? o->kind == OccKind::Declare : o == def;
        out.push_back(r);
    }
    return out;
}

namespace {

// Can the target be renamed? Sets *why when not.
bool Renameable(const Index &ix, const Target &t, std::string *why) {
    if (!t.occ) {
        *why = "nothing to rename here";
        return false;
    }
    const std::vector<const Occurrence *> occs = Occurrences(ix, t);
    if (t.function) {
        if (!ix.DefinesFunction(t.occ->name)) {
            *why = "`" + t.occ->name + "' is not a function this file defines";
            return false;
        }
    } else {
        if (bash_lsp::IsKnownShellVariable(t.occ->name)) {
            *why = "`" + t.occ->name + "' is a variable bash or the environment provides";
            return false;
        }
        if (!DefinitionOf(occs)) {
            *why = "`" + t.occ->name + "' is never assigned in this file";
            return false;
        }
    }
    for (const Occurrence *o : occs) {
        if (!o->renameable) {
            *why = "a use of `" + t.occ->name + "' on line " + std::to_string(o->range.line + 1) +
                   " is not written out as a plain name";
            return false;
        }
    }
    return true;
}

}  // namespace

BashLspRenameTarget BashLspPrepareRename(const std::vector<std::string> &lines, int line, int col) {
    const auto ix = BuildIndex(lines);
    BashLspRenameTarget out;
    const Target t = TargetAt(*ix, line, col);
    std::string why;
    if (!Renameable(*ix, t, &why)) return out;
    out.found = true;
    out.range = t.occ->range;
    out.placeholder = t.occ->name;
    return out;
}

std::vector<BashLspTextEdit> BashLspRename(const std::vector<std::string> &lines, int line, int col,
                                           const std::string &new_name, std::string *error) {
    const auto ix = BuildIndex(lines);
    std::vector<BashLspTextEdit> out;
    std::string scratch;
    std::string *why = error ? error : &scratch;
    const Target t = TargetAt(*ix, line, col);
    if (!Renameable(*ix, t, why)) return out;
    if (t.function ? !ValidFunctionName(new_name) : !BashIsName(new_name)) {
        *why = "`" + new_name + "' is not a valid " + (t.function ? "function" : "variable") + " name";
        return out;
    }
    for (const Occurrence *o : Occurrences(*ix, t)) out.push_back(BashLspTextEdit{o->range, new_name});
    return out;
}

// --- Formatting ------------------------------------------------------------

namespace {

class IndentCollector : public BashVisitor {
public:
    std::vector<BashRange> regions;   // +1 for lines starting inside
    std::vector<std::pair<int, int>> protect;  // line spans to leave alone
    // Continuations, kept apart because files differ on them: bash-completion
    // indents a pipeline after `&&` one level, git's tests keep an `&&` chain
    // flat. Each with the line it continues.
    struct Continuation {
        BashRange range;
        int from_line;
        bool pipe;  // after `|` (else after `&&`/`||`)
    };
    std::vector<Continuation> continuations;

    // A block body goes in a level -- unless it starts on the same line as
    // the keyword or brace that opens it (`(cd x &&` newline `make)`), when
    // its other lines continue that line, and the continuation rules apply.
    const BashCommand *current_ = nullptr;
    void Body(const std::shared_ptr<BashList> &b) {
        if (!b || b->items.empty()) return;
        if (current_) {
            int opener_line = current_->range.line;
            for (const BashRange &k : current_->keywords) {
                if (k.line < b->range.line || (k.line == b->range.line && k.col < b->range.col)) opener_line = k.line;
            }
            if (opener_line == b->range.line) return;
        }
        regions.push_back(b->range);
    }

    // A pipeline after `&&`/`||`, or a command after `|`, that starts on a
    // later line continues its list: it, and everything in it, goes one
    // level in.
    void Continuations(const BashList &list) {
        for (const BashAndOr &ao : list.items) {
            for (size_t k = 1; k < ao.pipelines.size(); k++) {
                if (ao.pipelines[k].range.line > ao.range.line) {
                    continuations.push_back(Continuation{ao.pipelines[k].range, ao.range.line, false});
                }
            }
            for (const BashPipeline &pl : ao.pipelines) {
                for (size_t k = 1; k < pl.commands.size(); k++) {
                    if (pl.commands[k].range.line > pl.range.line) {
                        continuations.push_back(Continuation{pl.commands[k].range, pl.range.line, true});
                    }
                }
            }
        }
    }
    void Lists(const BashCommand &c) {
        for (const auto &b : c.bodies) {
            if (b) Continuations(*b);
        }
        for (const BashCaseItem &ci : c.case_items) {
            if (ci.body) Continuations(*ci.body);
        }
    }

    bool Command(const BashCommand &c, const BashCommand *) override {
        Lists(c);
        current_ = &c;
        switch (c.kind) {
            case BashCommandKind::If:
                for (size_t k = 0; k < c.bodies.size(); k++) {
                    const bool else_body = c.has_else && k + 1 == c.bodies.size();
                    if (k % 2 == 1 || else_body) Body(c.bodies[k]);
                }
                break;
            case BashCommandKind::While:
            case BashCommandKind::Until:
                if (c.bodies.size() > 1) Body(c.bodies[1]);
                break;
            case BashCommandKind::For:
            case BashCommandKind::Select:
            case BashCommandKind::ForArith:
            case BashCommandKind::Group:
            case BashCommandKind::Subshell:
                if (!c.bodies.empty()) Body(c.bodies[0]);
                break;
            case BashCommandKind::Case:
                for (const BashCaseItem &ci : c.case_items) {
                    regions.push_back(ci.range);
                    // `pat) cmd &&` newline `cmd2 ;;`: a body starting on the
                    // pattern's line is continued, not indented as a block.
                    if (ci.body && !ci.body->items.empty() && ci.body->range.line > ci.range.line) Body(ci.body);
                }
                break;
            case BashCommandKind::Cond:
            case BashCommandKind::Arith:
                // `[[ a &&` newline `b ]]`: the lines after the first go in a level.
                if (c.range.end_line > c.range.line) {
                    BashRange r = c.range;
                    r.line += 1;
                    r.col = 0;
                    regions.push_back(r);
                }
                break;
            case BashCommandKind::Simple:
                for (const BashAssignment &a : c.assignments) {
                    if (a.is_array && !a.elements.empty()) {
                        BashRange r = a.elements.front().range;
                        r.end_line = a.elements.back().range.end_line;
                        r.end_col = a.elements.back().range.end_col;
                        regions.push_back(r);
                    }
                }
                break;
            default:
                break;
        }
        for (const BashRedirect &r : c.redirects) {
            // The body's range ends at the start of the delimiter line, so
            // this covers the body and the delimiter.
            if (r.heredoc) protect.emplace_back(r.body_range.line, r.body_range.end_line);
        }
        return true;
    }

    void Part(const BashWordPart &p, const BashCommand *) override {
        if (p.body) Continuations(*p.body);
        switch (p.kind) {
            case BashPartKind::CommandSub:
            case BashPartKind::ProcessSub:
                if (!p.backquoted && p.body && !p.body->items.empty() && p.body->range.line > p.range.line) {
                    regions.push_back(p.body->range);
                }
                break;
            case BashPartKind::DoubleQuoted:
            case BashPartKind::SingleQuoted:
            case BashPartKind::AnsiC:
            case BashPartKind::Locale:
            case BashPartKind::Literal:
                // A string running over lines: what follows its first line
                // is the string's content.
                if (p.range.end_line > p.range.line) protect.emplace_back(p.range.line + 1, p.range.end_line);
                break;
            default:
                break;
        }
        if (p.backquoted && p.range.end_line > p.range.line) protect.emplace_back(p.range.line + 1, p.range.end_line);
    }
};

bool StartsWithWord(const std::string &s, size_t at, const char *word) {
    const size_t n = std::strlen(word);
    if (s.compare(at, n, word) != 0) return false;
    if (at + n >= s.size()) return true;
    const char c = s[at + n];
    return std::strchr(" \t;&|()<>", c) != nullptr;
}

bool IsCloserLine(const std::string &s, int at) {
    const size_t a = static_cast<size_t>(at);
    // Not else/elif: a comment before them introduces that branch.
    for (const char *w : {"fi", "done", "esac", "}"}) {
        if (StartsWithWord(s, a, w)) return true;
    }
    return s.compare(a, 1, ")") == 0;  // `;;` is at the item body's level: a comment before it goes there too
}

// Does a line end with a backslash continuing its command onto the next?
// (Continuations after `|`, `&&` and `||` come from the tree.)
bool ContinuesNext(const std::string &s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    if (e == 0 || s[e - 1] != '\\') return false;
    // An even run of backslashes is escaped backslashes, not a continuation.
    size_t n = 0;
    while (n < e && s[e - 1 - n] == '\\') n++;
    if (n % 2 == 0) return false;
    // `cmd | \`: the operator continues the list; the tree has that one.
    size_t p = e - 1;
    while (p > 0 && (s[p - 1] == ' ' || s[p - 1] == '\t')) p--;
    if (p > 0 && s[p - 1] == '|') return false;
    return !(p > 1 && s[p - 1] == '&' && s[p - 2] == '&');
}

}  // namespace

namespace {

// One round of re-indentation; the conventions it reads off the file (the
// indent unit, how continuations are indented) can shift once its own
// output is what it reads, so BashLspFormat runs it to a fixed point.
std::vector<BashLspTextEdit> FormatOnce(const std::vector<std::string> &lines) {
    std::vector<BashLspTextEdit> out;
    const auto ix = BuildIndex(lines);
    if (ix->parse.has_syntax_error) return out;
    const std::vector<std::string> &ls = ix->lines;
    IndentCollector ic;
    ic.Continuations(ix->parse.program);
    BashWalk(ix->parse.program, ic);
    // The file's own convention for continuations: indented a level past the
    // line they continue, or level with it -- by how its lines are now.
    {
        auto indent_of = [&](int line) {
            const std::string &t = ls[static_cast<size_t>(line)];
            int w = 0;
            for (char ch : t) {
                if (ch == ' ') {
                    w++;
                } else if (ch == '\t') {
                    w += 8 - w % 8;
                } else {
                    break;
                }
            }
            return w;
        };
        int flat[2] = {0, 0}, deeper[2] = {0, 0};
        for (const IndentCollector::Continuation &c : ic.continuations) {
            const int a = indent_of(c.from_line), b = indent_of(c.range.line);
            if (b == a) flat[c.pipe]++;
            if (b > a) deeper[c.pipe]++;
        }
        for (const IndentCollector::Continuation &c : ic.continuations) {
            if (flat[c.pipe] > deeper[c.pipe] && flat[c.pipe] >= 2) continue;  // one is not yet a convention
            ic.regions.push_back(c.range);
        }
    }
    std::vector<bool> fixed(ls.size(), false);
    for (const auto &[a, b] : ic.protect) {
        for (int k = std::max(a, 0); k <= b && k < static_cast<int>(ls.size()); k++) fixed[static_cast<size_t>(k)] = true;
    }
    const int n = static_cast<int>(ls.size());
    std::vector<int> depth(ls.size(), -1);
    std::vector<int> base(ls.size(), -1);  // without a continuation's extra level
    for (int k = 0; k < n; k++) {
        const int f = FirstNonBlank(ls[static_cast<size_t>(k)]);
        if (f < 0 || fixed[static_cast<size_t>(k)]) continue;
        int d = 0;
        for (const BashRange &r : ic.regions) {
            if (BashRangeContains(r, k, f)) d++;
        }
        const std::string &s = ls[static_cast<size_t>(k)];
        if (s.compare(static_cast<size_t>(f), 2, ";;") == 0 || s.compare(static_cast<size_t>(f), 2, ";&") == 0) d++;
        base[static_cast<size_t>(k)] = d;
        if (k > 0 && !fixed[static_cast<size_t>(k - 1)] && ContinuesNext(ls[static_cast<size_t>(k - 1)]) &&
            ls[static_cast<size_t>(k - 1)][static_cast<size_t>(std::max(0, FirstNonBlank(ls[static_cast<size_t>(k - 1)])))] != '#') {
            d++;
        }
        depth[static_cast<size_t>(k)] = d;
    }
    // A comment line takes the depth of the code after it -- unless that
    // closes a block, when it belongs with the code before it.
    for (int k = 0; k < n; k++) {
        const int f = FirstNonBlank(ls[static_cast<size_t>(k)]);
        if (f < 0 || fixed[static_cast<size_t>(k)] || ls[static_cast<size_t>(k)][static_cast<size_t>(f)] != '#') continue;
        if (k == 0 && ix->has_shebang) continue;
        int next = k + 1;
        while (next < n && (FirstNonBlank(ls[static_cast<size_t>(next)]) < 0 ||
                            (ls[static_cast<size_t>(next)][static_cast<size_t>(FirstNonBlank(ls[static_cast<size_t>(next)]))] == '#' &&
                             !fixed[static_cast<size_t>(next)]))) {
            next++;
        }
        int prev = k - 1;
        while (prev >= 0 && (FirstNonBlank(ls[static_cast<size_t>(prev)]) < 0 ||
                             ls[static_cast<size_t>(prev)][static_cast<size_t>(FirstNonBlank(ls[static_cast<size_t>(prev)]))] == '#' ||
                             fixed[static_cast<size_t>(prev)])) {
            prev--;
        }
        const bool next_closes = next < n && IsCloserLine(ls[static_cast<size_t>(next)], FirstNonBlank(ls[static_cast<size_t>(next)]));
        if (next >= n) {
            depth[static_cast<size_t>(k)] = 0;  // after the last command: top level
        } else if (!next_closes && base[static_cast<size_t>(next)] >= 0) {
            depth[static_cast<size_t>(k)] = base[static_cast<size_t>(next)];
        } else if (prev >= 0 && base[static_cast<size_t>(prev)] >= 0) {
            depth[static_cast<size_t>(k)] = base[static_cast<size_t>(prev)];
        }
    }
    // The indent unit: tabs if the file indents with tabs, else the width
    // its first-level lines use most, else four spaces.
    int tab_lines = 0, space_lines = 0;
    char first_indent = 0;
    std::map<int, int> widths;
    for (int k = 0; k < n; k++) {
        const std::string &s = ls[static_cast<size_t>(k)];
        if (fixed[static_cast<size_t>(k)] || s.empty()) continue;
        if (s[0] == '\t') tab_lines++;
        if (s[0] == ' ') space_lines++;
        if (!first_indent && (s[0] == '\t' || s[0] == ' ') && FirstNonBlank(s) >= 0) first_indent = s[0];
        if (depth[static_cast<size_t>(k)] == 1 && s[0] == ' ') {
            const int f = FirstNonBlank(s);
            if (f > 0 && f <= 8) widths[f]++;
        }
    }
    std::string unit = "    ";
    if (tab_lines > space_lines || (tab_lines == space_lines && first_indent == '\t')) {
        unit = "\t";
    } else if (!widths.empty()) {
        int best = 4, count = 0;
        for (const auto &[w, c] : widths) {
            if (c > count) {
                best = w;
                count = c;
            }
        }
        unit = std::string(static_cast<size_t>(best), ' ');
    }
    for (int k = 0; k < n; k++) {
        const std::string &s = ls[static_cast<size_t>(k)];
        if (fixed[static_cast<size_t>(k)]) continue;
        const int f = FirstNonBlank(s);
        std::string want;
        if (f < 0) {
            if (s.empty()) continue;
            want = "";  // a line of only blanks
        } else {
            if (depth[static_cast<size_t>(k)] < 0) continue;
            for (int d = 0; d < depth[static_cast<size_t>(k)]; d++) want += unit;
            want += s.substr(static_cast<size_t>(f));
        }
        if (want == s) continue;
        BashLspTextEdit e;
        e.range.line = k;
        e.range.col = 0;
        e.range.end_line = k;
        e.range.end_col = static_cast<int>(s.size());
        e.new_text = want;
        out.push_back(e);
    }
    return out;
}

}  // namespace

std::vector<BashLspTextEdit> BashLspFormat(const std::vector<std::string> &lines) {
    std::vector<std::string> cur = lines;
    for (int round = 0; round < 4; round++) {
        const std::vector<BashLspTextEdit> edits = FormatOnce(cur);
        if (edits.empty()) break;
        for (const BashLspTextEdit &e : edits) cur[static_cast<size_t>(e.range.line)] = e.new_text;
    }
    std::vector<BashLspTextEdit> out;
    for (size_t k = 0; k < lines.size() && k < cur.size(); k++) {
        if (cur[k] == lines[k]) continue;
        BashLspTextEdit e;
        e.range.line = e.range.end_line = static_cast<int>(k);
        e.range.col = 0;
        e.range.end_col = static_cast<int>(lines[k].size());
        e.new_text = cur[k];
        out.push_back(e);
    }
    return out;
}
