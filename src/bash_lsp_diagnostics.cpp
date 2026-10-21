// mep-bash-lsp's diagnostics: bash's own syntax errors (bash_parse.h,
// which agrees with `bash -n`), then a set of checks for code that runs
// but does something other than what it says. Each check is written to
// stay quiet on the idiomatic forms of what it looks for; the codes and
// what each one means are listed in help/bash-lsp.org.
#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "bash_lsp.h"
#include "bash_lsp_index.h"

using bash_lsp::Index;
using bash_lsp::Occurrence;
using bash_lsp::OccKind;
using bash_lsp::PlainWord;

namespace {

BashLspDiagnostic Diag(const BashRange &r, BashLspSeverity sev, const char *code, const std::string &message) {
    BashLspDiagnostic d;
    d.range = r;
    d.severity = sev;
    d.code = code;
    d.message = message;
    return d;
}

bool AllCaps(const std::string &s) {
    bool letter = false;
    for (char c : s) {
        if (c >= 'a' && c <= 'z') return false;
        if (c >= 'A' && c <= 'Z') letter = true;
    }
    return letter;
}

std::string CommandName(const BashCommand &c) {
    std::string name;
    if (c.kind == BashCommandKind::Simple && !c.words.empty()) PlainWord(c.words[0], &name);
    return name;
}

// Where a word sits, for the quoting checks.
enum class WordContext {
    Argument,     // an argument of a simple command: split and globbed
    ForItem,      // a `for ... in` word: splitting there is usually the point
    Redirect,     // a redirection target
    NoSplit,      // assignments, [[ ]], case words, here-strings: no splitting
};

class Linter {
public:
    Linter(const Index &ix, std::vector<BashLspDiagnostic> *out) : ix_(ix), out_(out) {}

    void Run() {
        Lists(ix_.parse.program, nullptr, false);
        Names();
        Functions();
    }

private:
    const Index &ix_;
    std::vector<BashLspDiagnostic> *out_;
    int bashisms_ = 0;
    int in_funsub_ = 0;  // inside ${ ...; }, which runs like a function body

    void Add(const BashRange &r, BashLspSeverity sev, const char *code, const std::string &message) {
        out_->push_back(Diag(r, sev, code, message));
    }

    void Bashism(const BashRange &r, const std::string &what) {
        if (!ix_.posix_sh || bashisms_ >= 50) return;
        bashisms_++;
        Add(r, BashLspSeverity::Warning, "bashism",
            what + " is a bash feature, and this script runs with a POSIX sh (its #! line)");
    }

    // --- Lists ---------------------------------------------------------

    void Lists(const BashList &list, const BashCommand *fn, bool condition) {
        Unreachable(list);
        for (const BashAndOr &ao : list.items) {
            for (size_t p = 0; p < ao.pipelines.size(); p++) {
                const BashPipeline &pl = ao.pipelines[p];
                // A command whose failure is handled: in a condition, or
                // followed by && / ||, or negated.
                const bool checked = condition || p + 1 < ao.pipelines.size() || pl.negated;
                if (pl.ops.size() > 0) {
                    for (size_t k = 0; k < pl.ops.size(); k++) {
                        if (pl.ops[k] == "|&") Bashism(pl.range, "`|&`");
                    }
                }
                for (const BashCommand &c : pl.commands) Command(c, fn, checked && pl.commands.size() == 1);
            }
        }
    }

    // Commands after an unconditional exit/return in the same list never run.
    void Unreachable(const BashList &list) {
        for (size_t k = 0; k + 1 < list.items.size(); k++) {
            const BashAndOr &ao = list.items[k];
            if (ao.pipelines.size() != 1 || ao.pipelines[0].commands.size() != 1 || ao.terminator == "&") continue;
            const BashCommand &c = ao.pipelines[0].commands[0];
            const std::string name = CommandName(c);
            if (name != "exit" && name != "return") continue;
            const BashAndOr &next = list.items[k + 1];
            // A function defined after `exit` is still never defined; but a
            // lone label-like `:` or a comment-only tail is not worth a word.
            BashRange r = next.range;
            r.end_line = list.items.back().range.end_line;
            r.end_col = list.items.back().range.end_col;
            Add(r, BashLspSeverity::Hint, "unreachable", "this never runs: the `" + name + "` before it always leaves");
            return;
        }
    }

    // --- Commands ----------------------------------------------------------

    void Command(const BashCommand &c, const BashCommand *fn, bool checked) {
        for (const BashRedirect &r : c.redirects) Redirect(r, fn);
        switch (c.kind) {
            case BashCommandKind::Simple:
                Simple(c, fn, checked);
                break;
            case BashCommandKind::If:
                for (size_t k = 0; k < c.bodies.size(); k++) {
                    const bool else_body = c.has_else && k + 1 == c.bodies.size();
                    if (c.bodies[k]) Lists(*c.bodies[k], fn, k % 2 == 0 && !else_body);
                }
                break;
            case BashCommandKind::While:
            case BashCommandKind::Until:
                for (size_t k = 0; k < c.bodies.size(); k++) {
                    if (c.bodies[k]) Lists(*c.bodies[k], fn, k == 0);
                }
                break;
            case BashCommandKind::For:
            case BashCommandKind::Select:
                if (!BashIsName(c.var)) {
                    Add(c.var_range, BashLspSeverity::Error, "invalid-identifier",
                        "`" + c.var + "' is not a valid variable name");
                }
                if (c.kind == BashCommandKind::Select) Bashism(c.keywords.front(), "`select`");
                for (const BashWord &w : c.items) {
                    Word(w, WordContext::ForItem, fn);
                    ForInLs(w);
                }
                for (const auto &b : c.bodies) {
                    if (b) Lists(*b, fn, false);
                }
                break;
            case BashCommandKind::ForArith:
                Bashism(c.keywords.front(), "`for ((...))`");
                for (const BashArith &a : c.ariths) Arith(a, fn);
                for (const auto &b : c.bodies) {
                    if (b) Lists(*b, fn, false);
                }
                break;
            case BashCommandKind::Arith:
                Bashism(c.keywords.front(), "`((...))`");
                for (const BashArith &a : c.ariths) Arith(a, fn);
                break;
            case BashCommandKind::Case:
                Word(c.subject, WordContext::NoSplit, fn);
                for (const BashCaseItem &ci : c.case_items) {
                    for (const BashWord &w : ci.patterns) Word(w, WordContext::NoSplit, fn);
                    if (ci.body) Lists(*ci.body, fn, false);
                }
                break;
            case BashCommandKind::Cond:
                Bashism(c.keywords.front(), "`[[ ]]`");
                Cond(c, fn);
                break;
            case BashCommandKind::Function:
                if (c.function_keyword) Bashism(c.keywords.front(), "the `function` keyword");
                for (const BashCommand &s : c.sub) Command(s, &c, false);
                break;
            case BashCommandKind::Group:
            case BashCommandKind::Subshell:
                for (const auto &b : c.bodies) {
                    if (b) Lists(*b, fn, checked);
                }
                break;
            case BashCommandKind::Coproc:
                Bashism(c.keywords.front(), "`coproc`");
                for (const BashCommand &s : c.sub) Command(s, fn, false);
                break;
        }
    }

    void Simple(const BashCommand &c, const BashCommand *fn, bool checked) {
        for (const BashAssignment &a : c.assignments) {
            if (a.is_array && !a.declaration) Bashism(a.range, "an array");
            Word(a.value, WordContext::NoSplit, fn);
            for (const BashWord &w : a.elements) Word(w, WordContext::Argument, fn);
            if (a.has_subscript) Arith(a.subscript_arith, fn);
        }
        if (c.words.empty()) return;
        const std::string name = CommandName(c);
        const bool declaration =
            name == "local" || name == "declare" || name == "typeset" || name == "export" || name == "readonly";
        Word(c.words[0], WordContext::NoSplit, fn);
        for (size_t k = 1; k < c.words.size(); k++) {
            // `local x=$1`: an assignment word, which is not split.
            const std::string &t = c.words[k].text;
            const size_t eq = t.find('=');
            const bool assignment = declaration && eq != std::string::npos &&
                                    BashIsName(t.substr(0, t.find_first_of("[+=")));
            Word(c.words[k], assignment ? WordContext::NoSplit : WordContext::Argument, fn);
        }
        if (name.empty()) return;

        // `x = 1`: a command named x, not an assignment.
        if (BashIsName(name) && c.words.size() >= 2 && c.words.size() <= 3 && !ix_.DefinesFunction(name) &&
            !BashLspFind(BashLspBuiltinVocab(), name) && !BashLspFind(BashLspKeywordVocab(), name)) {
            std::string second;
            const bool shape = PlainWord(c.words[1], &second) &&
                               ((c.words.size() == 3 && second == "=") ||
                                (c.words.size() == 2 && second.size() > 1 && second[0] == '=' &&
                                 second.find_first_not_of('=') != std::string::npos));
            if (shape && c.assignments.empty()) {
                Add(c.words[0].range, BashLspSeverity::Warning, "assignment-spaces",
                    "this runs a command named `" + name + "`; an assignment has no spaces around `=` (" + name +
                        "=value)");
            }
        }
        if (name == "local" && fn == nullptr && ix_.has_shebang && !in_funsub_) {
            Add(c.words[0].range, BashLspSeverity::Error, "local-outside-function",
                "`local` can only be used in a function");
        }
        if (name == "local" || name == "declare" || name == "typeset" || name == "export" || name == "readonly") {
            Declaration(c, name);
        }
        if (name == "read") Read(c);
        if (name == "cd" && !checked && !ix_.set_e) {
            Add(c.words[0].range, BashLspSeverity::Information, "cd-unchecked",
                "if this cd fails, the commands after it run in the wrong directory: `cd ... || exit` (or "
                "`|| return`)");
        }
        if ((name == "exit" || name == "return") && c.words.size() >= 2) {
            std::string v;
            if (PlainWord(c.words[1], &v) && !v.empty()) {
                bool numeric = true;
                for (size_t k = v[0] == '-' ? 1 : 0; k < v.size(); k++) numeric = numeric && v[k] >= '0' && v[k] <= '9';
                if (numeric && v != "-" && (v[0] == '-' || v.size() > 3 || std::stoi(v) > 255)) {
                    int mod = 0;
                    for (size_t k = v[0] == '-' ? 1 : 0; k < v.size(); k++) mod = (mod * 10 + (v[k] - '0')) % 256;
                    if (v[0] == '-') mod = (256 - mod) % 256;
                    Add(c.words[1].range, BashLspSeverity::Warning, "exit-code-range",
                        "an exit status is 0 to 255; " + v + " wraps around to " + std::to_string(mod));
                }
            }
        }
        if (name == "source") Bashism(c.words[0].range, "`source` (POSIX sh has `.`)");
        if (name == "let" || name == "declare" || name == "typeset" || name == "mapfile" || name == "readarray") {
            Bashism(c.words[0].range, "`" + name + "`");
        }
        if ((name == "[" || name == "test") && ix_.posix_sh) {
            for (size_t k = 1; k < c.words.size(); k++) {
                std::string t;
                if (PlainWord(c.words[k], &t) && t == "==") Bashism(c.words[k].range, "`==` in test (POSIX has `=`)");
            }
        }
        if (name == "[") {
            std::string last;
            if (!PlainWord(c.words.back(), &last) || last != "]") {
                Add(c.words[0].range, BashLspSeverity::Error, "test-missing-bracket",
                    "this `[` has no closing `]` -- a `]` must be the last argument (`&&` and `||` end the "
                    "command: use them between two `[ ]`, or `[[ ]]`)");
            }
        }
    }

    void Declaration(const BashCommand &c, const std::string &name) {
        bool functions = false;
        for (size_t k = 1; k < c.words.size(); k++) {
            std::string t;
            if (PlainWord(c.words[k], &t) && t.size() > 1 && (t[0] == '-' || t[0] == '+') &&
                t.find_first_of("fF") != std::string::npos) {
                functions = true;
            }
        }
        for (size_t k = 1; k < c.words.size() && !functions; k++) {
            // The raw text: an assignment word's parts are its value's.
            const std::string &t = c.words[k].text;
            if (t.empty() || t[0] == '-' || t[0] == '+') continue;
            const std::string var = t.substr(0, t.find_first_of("=[+"));
            if (var.find_first_of("$`'\"\\@") != std::string::npos) continue;  // made at run time, or a template
            if (!BashIsName(var) && !(name == "export" && t.find('=') == std::string::npos && var.empty())) {
                Add(c.words[k].range, BashLspSeverity::Error, "invalid-identifier",
                    "`" + t + "' is not a valid variable name");
            }
        }
        for (const BashAssignment &a : c.assignments) {
            if (!a.declaration || name == "readonly") continue;
            for (const BashWordPart &p : a.value.parts) {
                if (p.kind == BashPartKind::CommandSub ||
                    (p.kind == BashPartKind::DoubleQuoted &&
                     std::any_of(p.parts.begin(), p.parts.end(),
                                 [](const BashWordPart &q) { return q.kind == BashPartKind::CommandSub; }))) {
                    Add(a.name_range, BashLspSeverity::Information, "masked-return",
                        "`" + name + "` succeeds even when the command substitution fails, hiding its status; "
                        "declare first, then assign: `" + name + " " + a.name + "; " + a.name + "=$(...)`");
                    break;
                }
            }
        }
    }

    void Read(const BashCommand &c) {
        bool raw = false;
        for (size_t k = 1; k < c.words.size(); k++) {
            std::string t;
            if (!PlainWord(c.words[k], &t) || t.size() < 2 || t[0] != '-') continue;
            if (t.find('r') != std::string::npos) raw = true;
            // Options that take an argument end the cluster.
            for (size_t j = 1; j < t.size(); j++) {
                if (std::strchr("adinNptu", t[j]) != nullptr) {
                    if (j + 1 == t.size()) k++;
                    break;
                }
            }
        }
        if (!raw) {
            Add(c.words[0].range, BashLspSeverity::Information, "read-without-r",
                "without -r, read treats backslashes as escapes and drops them; `read -r` keeps the input as is");
        }
        for (size_t k = 1; k < c.words.size(); k++) {
            std::string t;
            if (!PlainWord(c.words[k], &t) || t.empty() || t[0] == '-') continue;
            if (k > 0) {
                std::string prev;
                if (PlainWord(c.words[k - 1], &prev) && prev.size() >= 2 && prev[0] == '-' &&
                    std::strchr("dinNptu", prev.back()) != nullptr) {
                    continue;  // an option's argument
                }
            }
            const std::string var = t.substr(0, t.find('['));
            if (!BashIsName(var)) {
                Add(c.words[k].range, BashLspSeverity::Error, "invalid-identifier",
                    "`" + t + "' is not a valid variable name");
            }
        }
    }

    void Redirect(const BashRedirect &r, const BashCommand *fn) {
        if (r.op == "<<<") Bashism(r.range, "a here-string `<<<`");
        if (r.op == "&>" || r.op == "&>>") Bashism(r.range, "`" + r.op + "` (POSIX: >file 2>&1)");
        if (!r.heredoc) Word(r.target, r.op == "<<<" ? WordContext::NoSplit : WordContext::Redirect, fn);
        for (const BashWordPart &p : r.body_parts) Part(p, WordContext::NoSplit, fn, true);
    }

    void Cond(const BashCommand &c, const BashCommand *fn) {
        for (size_t k = 0; k < c.cond.size(); k++) {
            const BashCondTerm &t = c.cond[k];
            if (t.kind != BashCondTerm::Word) continue;
            Word(t.word, WordContext::NoSplit, fn);
            if (k > 0 && c.cond[k - 1].kind == BashCondTerm::BinaryOp && c.cond[k - 1].word.text == "=~") {
                const std::vector<BashWordPart> &ps = t.word.parts;
                // Only when the quoted text looks like a regex: `=~ "::"` is a
                // substring test, and meant as one.
                const bool regexy = ps.size() == 1 && ps[0].text.find_first_of("^$.*+?()[]{}|\\") != std::string::npos &&
                                    ps[0].text.substr(1, ps[0].text.size() - 2).find_first_of("^$.*+?()[]{}|\\") !=
                                        std::string::npos;
                if (regexy && (ps[0].kind == BashPartKind::DoubleQuoted || ps[0].kind == BashPartKind::SingleQuoted)) {
                    Add(t.word.range, BashLspSeverity::Warning, "quoted-regex",
                        "a quoted right side of =~ matches as a plain string, not a regex; put the regex in a "
                        "variable and use it unquoted: re='...'; [[ $x =~ $re ]]");
                }
            }
        }
    }

    void Arith(const BashArith &a, const BashCommand *fn) {
        for (const BashWordPart &p : a.parts) Part(p, WordContext::NoSplit, fn, false);
    }

    // `for f in $(ls ...)`: breaks on names with spaces; a glob does it right.
    void ForInLs(const BashWord &w) {
        for (const BashWordPart &p : w.parts) {
            if (p.kind != BashPartKind::CommandSub || !p.body || p.body->items.size() != 1) continue;
            const BashAndOr &ao = p.body->items[0];
            if (ao.pipelines.empty() || ao.pipelines[0].commands.empty()) continue;
            if (CommandName(ao.pipelines[0].commands[0]) == "ls") {
                Add(p.range, BashLspSeverity::Information, "for-in-ls",
                    "iterating over ls output splits file names with spaces; use a glob: for f in dir/*");
            }
        }
    }

    // --- Words ---------------------------------------------------------------

    void Word(const BashWord &w, WordContext ctx, const BashCommand *fn) {
        for (const BashWordPart &p : w.parts) Part(p, ctx, fn, false);
    }

    void Part(const BashWordPart &p, WordContext ctx, const BashCommand *fn, bool in_heredoc) {
        switch (p.kind) {
            case BashPartKind::Param:
                Param(p, ctx, fn, in_heredoc);
                break;
            case BashPartKind::DoubleQuoted:
            case BashPartKind::Locale:
                if (p.kind == BashPartKind::Locale) Bashism(p.range, "$\"...\" locale quoting");
                if (!p.parts.empty() && p.parts[0].kind == BashPartKind::Literal && p.parts[0].text.rfind("~/", 0) == 0) {
                    Add(p.range, BashLspSeverity::Warning, "literal-tilde",
                        "~ is not expanded inside quotes; use \"$HOME\" (or leave the ~ outside the quotes: ~/\"...\")");
                }
                for (const BashWordPart &q : p.parts) Part(q, WordContext::NoSplit, fn, in_heredoc);
                break;
            case BashPartKind::CommandSub:
                if (p.backquoted) {
                    Add(p.range, BashLspSeverity::Hint, "backticks",
                        "`...` is the legacy form of $(...), which nests and quotes more simply");
                }
                if (!p.quoted && !in_heredoc && (ctx == WordContext::Argument || ctx == WordContext::Redirect)) {
                    Unquoted(p, "$(...)");
                }
                if (p.text.rfind("${", 0) == 0) {
                    Bashism(p.range, "${ ...; } command substitution");
                    in_funsub_++;
                    if (p.body) Lists(*p.body, fn, false);
                    in_funsub_--;
                } else if (p.body) {
                    Lists(*p.body, fn, false);
                }
                break;
            case BashPartKind::ProcessSub:
                Bashism(p.range, "process substitution");
                if (p.body) Lists(*p.body, fn, false);
                break;
            case BashPartKind::ArithSub:
                Arith(p.arith, fn);
                break;
            case BashPartKind::AnsiC:
                Bashism(p.range, "$'...' quoting");
                break;
            case BashPartKind::ExtGlob:
                Bashism(p.range, "an extglob pattern");
                for (const BashWordPart &q : p.parts) Part(q, WordContext::NoSplit, fn, in_heredoc);
                break;
            default:
                break;
        }
    }

    void Param(const BashWordPart &p, WordContext ctx, const BashCommand *fn, bool in_heredoc) {
        for (const BashWord &a : p.args) Word(a, WordContext::NoSplit, fn);
        if (p.has_subscript) Arith(p.subscript_arith, fn);
        if (p.indirect) Bashism(p.range, "${!name} indirection");
        if (!p.op.empty() && p.op != "-" && p.op != ":-" && p.op != "=" && p.op != ":=" && p.op != "+" &&
            p.op != ":+" && p.op != "?" && p.op != ":?" && p.op != "#" && p.op != "##" && p.op != "%" &&
            p.op != "%%" && p.op[0] != '?' && p.op[0] != '!') {
            Bashism(p.range, "${name" + p.op + "...}");
        }
        if (p.has_subscript) Bashism(p.range, "an array subscript");
        if (in_heredoc || p.quoted) return;
        const bool at = p.name == "@" || (p.has_subscript && p.subscript == "@");
        if (at && (ctx == WordContext::Argument || ctx == WordContext::ForItem)) {
            if (!p.length && !p.indirect) {
                Add(p.range, BashLspSeverity::Warning, "unquoted-at",
                    "unquoted, " + p.text + " re-splits every element on whitespace; \"" + p.text +
                        "\" passes them on as they are");
            }
            return;
        }
        // An array named without a subscript is its first element.
        if (BashIsName(p.name) && !p.has_subscript && !p.length && !p.indirect && p.op.empty()) {
            const BashCommand *scope = nullptr;
            if (fn) {
                const auto it = ix_.locals.find(fn);
                if (it != ix_.locals.end() && it->second.count(p.name)) scope = fn;
            }
            const auto info = ix_.info.find({scope, p.name});
            if (info != ix_.info.end() && info->second.array && !info->second.scalar) {
                Add(p.range, BashLspSeverity::Warning, "array-as-string",
                    "`" + p.name + "` is an array; " + p.text + " is only its first element (\"${" + p.name +
                        "[@]}\" is all of them)");
            }
        }
        if (ctx != WordContext::Argument && ctx != WordContext::Redirect) return;
        if (p.length || p.name == "#" || p.name == "?" || p.name == "$" || p.name == "!" || p.name == "-") return;
        if (p.op == ":+" || p.op == "+") return;  // ${x:+--flag}: usually meant to vanish when empty
        Unquoted(p, p.text);
    }

    void Unquoted(const BashWordPart &p, const std::string &what) {
        Add(p.range, BashLspSeverity::Information, "unquoted-expansion",
            "unquoted, " + what + " is split on whitespace and glob-expanded; quote it unless that is wanted");
    }

    // --- Names -----------------------------------------------------------------

    void Names() {
        // Reads, per scope and name.
        std::set<std::pair<const BashCommand *, std::string>> read;
        std::set<std::string> written_anywhere;
        for (const Occurrence &o : ix_.vars) {
            if (o.kind == OccKind::Read || o.reads_too) read.insert({o.scope, o.name});
            if (o.kind != OccKind::Read) written_anywhere.insert(o.name);
        }
        auto indirect = [&](const std::string &name) {
            for (const std::string &n : ix_.indirect_names) {
                if (name == n || (ix_.indirect_prefix && name.rfind(n, 0) == 0)) return true;
            }
            return false;
        };
        const bool global_unknowable = ix_.uses_eval || ix_.sources_files || ix_.indirect_any || ix_.indirect_prefix;

        // unused-variable: a function's locals, and a script's own globals --
        // only names given a value somewhere: a bare `local x` shields the
        // caller's x from a callee that assigns it.
        std::set<std::pair<const BashCommand *, std::string>> valued;
        for (const Occurrence &o : ix_.vars) {
            if (o.kind == OccKind::Write) valued.insert({o.scope, o.name});
        }
        std::set<std::pair<const BashCommand *, std::string>> reported;
        for (const Occurrence &o : ix_.vars) {
            if (o.kind == OccKind::Read || !o.renameable || o.name == "_" || o.name[0] == '_') continue;
            const std::pair<const BashCommand *, std::string> key{o.scope, o.name};
            if (read.count(key) || reported.count(key) || !valued.count(key) || indirect(o.name) ||
                ix_.string_refs.count(o.name)) {
                continue;
            }
            const auto info = ix_.info.find(key);
            if (info != ix_.info.end() && (info->second.exported || info->second.nameref)) continue;
            if (o.scope) {
                if (ix_.uses_eval || ix_.indirect_any || bash_lsp::IsKnownShellVariable(o.name)) continue;
                // A local is visible to the functions it calls (dynamic
                // scoping): if any read anywhere uses the name, keep quiet.
                bool used_elsewhere = false;
                for (const Occurrence &r : ix_.vars) {
                    if (r.name == o.name && (r.kind == OccKind::Read || r.reads_too)) used_elsewhere = true;
                }
                if (used_elsewhere) continue;
            } else {
                if (global_unknowable || !ix_.has_shebang || AllCaps(o.name) || bash_lsp::IsKnownShellVariable(o.name)) {
                    continue;
                }
            }
            reported.insert(key);
            Add(o.range, BashLspSeverity::Warning, "unused-variable",
                "`" + o.name + "` is assigned but never used" +
                    (o.scope ? std::string(" in this function") : std::string()));
        }

        // undefined-variable: a lower-case name read but never assigned,
        // in a script that is self-contained. Past a handful of them the
        // script is getting its variables from somewhere else.
        if (global_unknowable || !ix_.has_shebang) return;
        // A nixpkgs builder reads its derivation's attributes ($out, $src,
        // $stdenv, ...) from the environment.
        for (const char *nix : {"out", "stdenv", "NIX_BUILD_TOP"}) {
            bool reads = false;
            for (const Occurrence &o : ix_.vars) reads = reads || (o.name == nix && o.kind == OccKind::Read);
            if (reads && !written_anywhere.count(nix)) return;
        }
        std::map<std::string, const Occurrence *> undefined;
        for (const Occurrence &o : ix_.vars) {
            if (o.kind != OccKind::Read || o.guarded || written_anywhere.count(o.name)) continue;
            if (AllCaps(o.name) || bash_lsp::IsKnownShellVariable(o.name) || indirect(o.name)) continue;
            if (!undefined.count(o.name)) undefined[o.name] = &o;
        }
        if (undefined.size() > 5) return;
        for (const auto &[name, o] : undefined) {
            Add(o->range, BashLspSeverity::Warning, "undefined-variable",
                "`" + name + "` is never assigned in this file (a typo?)");
        }
    }

    void Functions() {
        std::map<std::pair<const BashCommand *, std::string>, const bash_lsp::FunctionDef *> first;
        for (const bash_lsp::FunctionDef &d : ix_.functions) {
            if (d.conditional) continue;
            const auto key = std::make_pair(d.parent, d.name);
            const auto it = first.find(key);
            if (it == first.end()) {
                first[key] = &d;
                continue;
            }
            // A test suite redefines a helper between uses; a redefinition is
            // only suspect when the first definition is never called before it.
            const BashRange &a = it->second->cmd->range;
            bool called = false;
            for (const Occurrence &o : ix_.funcs) {
                if (o.name != d.name || o.kind != OccKind::Read) continue;
                const bool after_first = o.range.line > a.end_line || (o.range.line == a.end_line && o.range.col >= a.end_col);
                const bool before_second = o.range.line < d.cmd->range.line ||
                                           (o.range.line == d.cmd->range.line && o.range.col < d.cmd->range.col);
                if (after_first && before_second) called = true;
            }
            it->second = &d;
            if (called) continue;
            Add(d.name_range, BashLspSeverity::Information, "duplicate-function",
                "`" + d.name + "` is already defined on line " + std::to_string(it->second->name_range.line + 1) +
                    "; this definition replaces it");
        }
    }
};

}  // namespace

std::vector<BashLspDiagnostic> BashLspDiagnostics(const std::vector<std::string> &lines) {
    std::vector<BashLspDiagnostic> out;
    const auto ix = bash_lsp::BuildIndex(lines);
    for (const BashParseError &e : ix->parse.errors) {
        out.push_back(Diag(e.range, e.warning ? BashLspSeverity::Warning : BashLspSeverity::Error, e.code.c_str(),
                           e.message));
    }
    // After a syntax error the tree past it is a guess: report only the error.
    if (!ix->parse.has_syntax_error) {
        Linter lint(*ix, &out);
        lint.Run();
    }
    std::stable_sort(out.begin(), out.end(), [](const BashLspDiagnostic &a, const BashLspDiagnostic &b) {
        if (a.range.line != b.range.line) return a.range.line < b.range.line;
        return a.range.col < b.range.col;
    });
    return out;
}

std::string BashLspFormatDiagnostic(const std::string &path, const BashLspDiagnostic &d) {
    static const char *const kNames[] = {"", "error", "warning", "info", "hint"};
    const std::string shown = path == "-" ? "<stdin>" : path;
    return shown + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " +
           kNames[static_cast<int>(d.severity)] + ": " + d.message + " [" + d.code + "]";
}

bool BashLspDiagnosticFails(const BashLspDiagnostic &d) {
    return d.severity == BashLspSeverity::Error || d.severity == BashLspSeverity::Warning;
}
