// mep-bash-lsp: outline, folding, definition, references, rename and
// formatting. Diagnostics live in bash_lsp_diagnostics.cpp; completion,
// hover and signature help in bash_lsp_completion.cpp.
#include "bash_lsp.h"

#include "bash_format.h"

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

// mep's Bash formatter (bash_format.h), the same one `gf` runs: one edit
// replacing the lines that changed.
std::vector<BashLspTextEdit> BashLspFormat(const std::vector<std::string> &lines, const std::string &default_indent) {
    std::string text;
    for (const std::string &l : lines) text += l + "\n";
    bashfmt::Options opt;
    if (!default_indent.empty()) opt.default_indent = default_indent;
    const bashfmt::Result r = bashfmt::Format(text, opt);
    std::vector<BashLspTextEdit> out;
    if (!r.ok || r.text == text) return out;
    std::vector<std::string> got;
    size_t p = 0;
    while (p < r.text.size()) {
        const size_t nl = r.text.find('\n', p);
        got.push_back(r.text.substr(p, nl == std::string::npos ? std::string::npos : nl - p));
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    size_t head = 0;
    while (head < lines.size() && head < got.size() && lines[head] == got[head]) head++;
    size_t tail = 0;
    while (tail < lines.size() - head && tail < got.size() - head &&
           lines[lines.size() - 1 - tail] == got[got.size() - 1 - tail]) {
        tail++;
    }
    BashLspTextEdit e;
    e.range.line = static_cast<int>(head);
    e.range.col = 0;
    e.range.end_line = static_cast<int>(lines.size() - tail);
    e.range.end_col = 0;
    for (size_t k = head; k < got.size() - tail; k++) e.new_text += got[k] + "\n";
    out.push_back(e);
    return out;
}
