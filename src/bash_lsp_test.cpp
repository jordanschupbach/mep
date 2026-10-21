// Windowless test for bash_parse.cpp and bash_lsp*.cpp -- the parser and
// analysis halves of mep's own Bash language server (`mep-bash-lsp`,
// src/bash_lsp_server.cpp). Drives the pure functions directly: no
// process, no JSON-RPC client, no bash. CHECK(), never assert(): the
// Release build strips assert().
//
// Conventions (lisp_lsp_test.cpp's):
//   - Diagnostics are asserted by code, never by message wording.
//   - Every lint is asserted absent on the idiomatic forms it must stay
//     quiet about, as deliberately as it is asserted present.
//   - The parser is asserted through the shape of what it read, and its
//     syntax errors against what `bash -n` says about the same text.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bash_lsp.h"
#include "bash_parse.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

using Lines = std::vector<std::string>;

Lines L(const std::string &text) {
    Lines out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

int Count(const std::string &text, const std::string &code) {
    int n = 0;
    for (const BashLspDiagnostic &d : BashLspDiagnostics(L(text))) {
        if (d.code == code) n++;
    }
    return n;
}
bool Has(const std::string &text, const std::string &code) { return Count(text, code) > 0; }

const std::string kScript = "#!/bin/bash\n";

std::string FirstError(const std::string &text) {
    const BashParseResult r = BashParse(L(text));
    for (const BashParseError &e : r.errors) {
        if (!e.warning) return std::to_string(e.range.line + 1) + ": " + e.message;
    }
    return "";
}

const BashCommand &FirstCommand(const BashParseResult &r) { return r.program.items.at(0).pipelines.at(0).commands.at(0); }

bool HasLabel(const std::vector<BashLspCompletionItem> &items, const std::string &label) {
    for (const BashLspCompletionItem &it : items) {
        if (it.label == label) return true;
    }
    return false;
}

std::string Formatted(const std::string &text) {
    Lines lines = L(text);
    for (const BashLspTextEdit &e : BashLspFormat(lines)) lines[static_cast<size_t>(e.range.line)] = e.new_text;
    std::string out;
    for (size_t k = 0; k < lines.size(); k++) {
        if (k) out += "\n";
        out += lines[k];
    }
    return out;
}

// --- Parser -----------------------------------------------------------------

void TestParseShapes() {
    {
        const BashParseResult r = BashParse(L("FOO=1 BAR=(a b) cmd -x \"$y\" 'z' >out 2>&1"));
        CHECK(!r.has_syntax_error);
        const BashCommand &c = FirstCommand(r);
        CHECK(c.kind == BashCommandKind::Simple);
        CHECK(c.assignments.size() == 2);
        CHECK(c.assignments[1].is_array && c.assignments[1].elements.size() == 2);
        CHECK(c.words.size() == 4);
        CHECK(c.redirects.size() == 2);
        CHECK(c.redirects[1].fd == "2" && c.redirects[1].op == ">&");
    }
    {
        const BashParseResult r = BashParse(L("if a; then b; elif c; then d; else e; fi"));
        const BashCommand &c = FirstCommand(r);
        CHECK(c.kind == BashCommandKind::If);
        CHECK(c.bodies.size() == 5 && c.has_else);
    }
    {
        const BashParseResult r = BashParse(L("f() { local x=$1; echo \"${x:-none}\"; }"));
        const BashCommand &c = FirstCommand(r);
        CHECK(c.kind == BashCommandKind::Function && c.name == "f");
        CHECK(c.sub.size() == 1 && c.sub[0].kind == BashCommandKind::Group);
    }
    {
        const BashParseResult r = BashParse(L("case $x in\n  a|b) one ;;\n  @(c|d)) two ;&\n  *) ;;\nesac"));
        const BashCommand &c = FirstCommand(r);
        CHECK(c.kind == BashCommandKind::Case);
        CHECK(c.case_items.size() == 3);
        CHECK(c.case_items[0].patterns.size() == 2);
        CHECK(c.case_items[1].terminator == ";&");
        CHECK(c.case_items[1].patterns[0].parts[0].kind == BashPartKind::ExtGlob);
    }
    {
        // Here-documents: the body starts after the line, expansions in an
        // unquoted body are read, a quoted one is literal.
        const BashParseResult r = BashParse(L("cat <<EOF; echo after\nhi $name\nEOF\ncat <<'X'\n$lit\nX"));
        CHECK(!r.has_syntax_error);
        CHECK(r.program.items.size() == 3);
        const BashRedirect &h = FirstCommand(r).redirects[0];
        CHECK(h.heredoc && h.delimiter == "EOF" && !h.heredoc_quoted);
        int params = 0;
        for (const BashWordPart &p : h.body_parts) {
            if (p.kind == BashPartKind::Param && p.name == "name") params++;
        }
        CHECK(params == 1);
        const BashRedirect &q = r.program.items[2].pipelines[0].commands[0].redirects[0];
        CHECK(q.heredoc_quoted && q.body_parts.empty());
    }
    {
        // ${...} operators, nested expansions, arithmetic names.
        const BashParseResult r = BashParse(L("echo ${a:-${b}} ${#c} ${!d} ${e//x/y} $((f + g[i]++)) ${h@Q}"));
        const BashCommand &c = FirstCommand(r);
        CHECK(c.words[1].parts[0].op == ":-" && c.words[1].parts[0].args[0].parts[0].name == "b");
        CHECK(c.words[2].parts[0].length);
        CHECK(c.words[3].parts[0].indirect);
        CHECK(c.words[4].parts[0].op == "//" && c.words[4].parts[0].args.size() == 2);
        const BashArith &a = c.words[5].parts[0].arith;
        CHECK(a.refs.size() == 3 && a.refs[0].name == "f" && a.refs[1].name == "g" && a.refs[1].assigned);
        CHECK(c.words[6].parts[0].op == "@");
    }
    {
        // Line continuations disappear, inside words too.
        const BashParseResult r = BashParse(L("echo a \\\n  b\nec\\\nho c"));
        CHECK(!r.has_syntax_error);
        CHECK(FirstCommand(r).words.size() == 3);
        CHECK(r.program.items.size() == 2);
    }
    {
        // `((` is arithmetic only when it closes with `))`.
        CHECK(FirstCommand(BashParse(L("((x = 1))"))).kind == BashCommandKind::Arith);
        CHECK(FirstCommand(BashParse(L("((echo a); echo b)"))).kind == BashCommandKind::Subshell);
        CHECK(FirstCommand(BashParse(L("[[ -f $x && $y =~ ^a(b|c)$ ]]"))).kind == BashCommandKind::Cond);
    }
    {
        // Pipelines bind tighter than && and ||.
        const BashParseResult r = BashParse(L("a | b | c && d\ne && f"));
        CHECK(r.program.items.size() == 2);
        CHECK(r.program.items[0].pipelines.size() == 2);
        CHECK(r.program.items[0].pipelines[0].commands.size() == 3);
    }
    {
        // bash 5.3's ${ cmd; } and $(...) holding a case statement.
        CHECK(!BashParse(L("x=${ echo hi; }-y")).has_syntax_error);
        CHECK(!BashParse(L("x=$(case $a in b) echo;; esac)")).has_syntax_error);
        CHECK(!BashParse(L("a=( one # comment\n two )")).has_syntax_error);
        CHECK(!BashParse(L("for i do echo $i; done")).has_syntax_error);
        CHECK(!BashParse(L("exec {fd}>&-; exec 3<>file")).has_syntax_error);
        CHECK(!BashParse(L("! time { a; }")).has_syntax_error);
        CHECK(!BashParse(L("eval a=( $x )")).has_syntax_error);
        CHECK(!BashParse(L("echo \"${v/x/$'\\''}\"")).has_syntax_error);
    }
}

void TestParseErrors() {
    // bash -n's wording, on bash -n's line.
    CHECK(FirstError("if true; then\n  echo\n") == "3: syntax error: unexpected end of file from `if' command on line 1");
    CHECK(FirstError("echo hi\nfi") == "2: syntax error near unexpected token `fi'");
    CHECK(FirstError("echo 'open") == "1: unexpected EOF while looking for matching `''");
    CHECK(FirstError("echo \"$(ls)") == "1: unexpected EOF while looking for matching `\"'");
    CHECK(FirstError("a=1 (") == "1: syntax error near unexpected token `('");
    CHECK(FirstError("printf x a=(1)") == "1: syntax error near unexpected token `('");
    CHECK(FirstError("case x in\n  a) echo\nesac x") == "3: syntax error near unexpected token `x'");
    CHECK(FirstError("[[ a b ]]").find("conditional binary operator expected") != std::string::npos);
    CHECK(FirstError("echo >") == "1: syntax error near unexpected token `newline'");
    CHECK(FirstError("{ echo }") == "1: syntax error: unexpected end of file from `{' command on line 1");
    // Not errors: bash reads these.
    CHECK(FirstError("for 1 in a; do :; done").empty());  // an error only when it runs
    CHECK(FirstError("echo ${$x}").empty());              // a bad substitution, at run time
    CHECK(FirstError("echo `if`").empty());               // backquotes are read when run
    // A here-document left open is a warning, as in bash.
    const BashParseResult r = BashParse(L("cat <<EOF\nnever closed"));
    CHECK(!r.has_syntax_error);
    CHECK(r.errors.size() == 1 && r.errors[0].warning && r.errors[0].code == "unterminated-heredoc");
    // After an error the rest is still read, for the outline.
    const BashParseResult r2 = BashParse(L("fi\nf() { :; }"));
    CHECK(r2.has_syntax_error);
    CHECK(r2.program.items.size() == 1 && FirstCommand(r2).kind == BashCommandKind::Function);
}

// --- Diagnostics --------------------------------------------------------------

void TestDiagnostics() {
    CHECK(Has("if true; then", "unexpected-eof"));
    CHECK(Has("echo\nfi", "syntax-error"));
    // After a syntax error only the error is reported.
    CHECK(BashLspDiagnostics(L(kScript + "echo $x\nfi")).size() == 1);

    CHECK(Has(kScript + "local x=1", "local-outside-function"));
    CHECK(!Has("local x=1", "local-outside-function"));  // no #!: may be sourced from a function
    CHECK(!Has("f() { local x=1; echo \"$x\"; }", "local-outside-function"));

    CHECK(Has("for 1 in a; do :; done", "invalid-identifier"));
    CHECK(Has("read -r 1x", "invalid-identifier"));
    CHECK(Has("f() { local 2=x; }", "invalid-identifier"));
    CHECK(!Has("read -r -d '' -a arr", "invalid-identifier"));
    CHECK(!Has("declare -f my-func", "invalid-identifier"));

    CHECK(Has("x = 1", "assignment-spaces"));
    CHECK(Has("x =1", "assignment-spaces"));
    CHECK(!Has("x=1", "assignment-spaces"));
    CHECK(!Has("[ \"$a\" = 1 ]", "assignment-spaces"));
    CHECK(!Has("test x = y", "assignment-spaces"));

    CHECK(Has("[[ $x =~ \"^a+$\" ]]", "quoted-regex"));
    CHECK(!Has("re='^a+$'; [[ $x =~ $re ]]", "quoted-regex"));
    CHECK(!Has("[[ $x =~ ^\"a b\"+$ ]]", "quoted-regex"));  // a quoted piece of a regex is fine

    CHECK(Has("cp $src /tmp", "unquoted-expansion"));
    CHECK(Has("rm $(cat list)", "unquoted-expansion"));
    CHECK(!Has("cp \"$src\" /tmp", "unquoted-expansion"));
    CHECK(!Has("x=$y", "unquoted-expansion"));             // assignments do not split
    CHECK(!Has("local x=$y", "unquoted-expansion"));
    CHECK(!Has("[[ $a == $b ]]", "unquoted-expansion"));    // nor does [[ ]]
    CHECK(!Has("case $x in $y) ;; esac", "unquoted-expansion"));
    CHECK(!Has("echo $# $? $$", "unquoted-expansion"));      // numbers
    CHECK(!Has("cmd ${opt:+--flag}", "unquoted-expansion"));  // meant to vanish
    CHECK(!Has("for f in $list; do :; done", "unquoted-expansion"));  // splitting is the point
    CHECK(!Has("cat <<< $x", "unquoted-expansion"));

    CHECK(Has("cmd $@", "unquoted-at"));
    CHECK(Has("for a in ${arr[@]}; do :; done", "unquoted-at"));
    CHECK(!Has("cmd \"$@\"", "unquoted-at"));
    CHECK(!Has("n=${#arr[@]}", "unquoted-at"));

    CHECK(Has("cd /tmp\nrm -rf x", "cd-unchecked"));
    CHECK(!Has("cd /tmp || exit 1", "cd-unchecked"));
    CHECK(!Has("cd /tmp && make", "cd-unchecked"));
    CHECK(!Has("if cd /tmp; then :; fi", "cd-unchecked"));
    CHECK(!Has("set -e\ncd /tmp", "cd-unchecked"));
    CHECK(!Has("#!/bin/bash -e\ncd /tmp", "cd-unchecked"));

    CHECK(Has("read line", "read-without-r"));
    CHECK(!Has("read -r line", "read-without-r"));
    CHECK(!Has("IFS= read -rd '' x", "read-without-r"));

    CHECK(Has("a=(1 2)\necho $a", "array-as-string"));
    CHECK(!Has("a=(1 2)\necho \"${a[@]}\" ${#a[@]} ${a[0]}", "array-as-string"));
    CHECK(!Has("a=(1 2)\na=x\necho $a", "array-as-string"));  // also a scalar

    CHECK(Has("x=`date`", "backticks"));
    CHECK(!Has("x=$(date)", "backticks"));

    CHECK(Has("#!/bin/sh\n[[ -f x ]]", "bashism"));
    CHECK(Has("#!/usr/bin/env sh\na=(1 2)", "bashism"));
    CHECK(Has("#!/bin/sh\necho ${x//a/b}", "bashism"));
    CHECK(!Has("#!/bin/bash\n[[ -f x ]]", "bashism"));
    CHECK(!Has("#!/bin/sh\n[ -f x ] && echo ${x%%.*}", "bashism"));

    CHECK(Has("exit 0\necho never", "unreachable"));
    CHECK(Has("f() { return 1; echo never; }", "unreachable"));
    CHECK(!Has("[ -f x ] || exit 0\necho reached", "unreachable"));
    CHECK(!Has("if x; then exit; fi\necho reached", "unreachable"));

    CHECK(Has("f() { :; }\nf() { :; }", "duplicate-function"));
    CHECK(!Has("if x; then f() { :; }; else f() { echo; }; fi", "duplicate-function"));

    CHECK(Has("exit 256", "exit-code-range"));
    CHECK(Has("f() { return -1; }", "exit-code-range"));
    CHECK(!Has("exit 255", "exit-code-range"));
    CHECK(!Has("exit $code", "exit-code-range"));

    CHECK(Has("f() { local x=$(cmd); echo \"$x\"; }", "masked-return"));
    CHECK(Has("export y=\"$(cmd)\"", "masked-return"));
    CHECK(!Has("f() { local x; x=$(cmd); echo \"$x\"; }", "masked-return"));

    CHECK(Has("[ -f x && -r x ]", "test-missing-bracket"));
    CHECK(!Has("[ -f x ] && [ -r x ]", "test-missing-bracket"));

    CHECK(Has("cp \"~/x\" .", "literal-tilde"));
    CHECK(!Has("cp ~/\"x y\" .", "literal-tilde"));

    CHECK(Has("for f in $(ls *.txt); do :; done", "for-in-ls"));
    CHECK(!Has("for f in *.txt; do :; done", "for-in-ls"));

    CHECK(Has("cat <<EOF\nnever closed", "unterminated-heredoc"));
}

void TestNameChecks() {
    // unused-variable: a function's local...
    CHECK(Has("f() { local tmp=1; echo hi; }", "unused-variable"));
    CHECK(!Has("f() { local tmp=1; echo \"$tmp\"; }", "unused-variable"));
    CHECK(!Has("f() { local _ignored=1; }", "unused-variable"));
    // `local a b` with no value shields the caller's globals from a callee
    // that assigns them: not unused.
    CHECK(!Has("f() { local cur prev; init_vars; }", "unused-variable"));
    // ...seen by a function it calls (dynamic scoping): not unused.
    CHECK(!Has("g() { echo \"$depth\"; }\nf() { local depth=1; g; }", "unused-variable"));
    // ...and a script's own lower-case globals.
    CHECK(Has(kScript + "count=0\necho hi", "unused-variable"));
    CHECK(!Has(kScript + "count=0\necho $count", "unused-variable"));
    CHECK(!Has(kScript + "LANG=C\necho hi", "unused-variable"));        // ALL_CAPS: the environment's
    CHECK(!Has(kScript + "export opt=1", "unused-variable"));
    CHECK(!Has("count=0\necho hi", "unused-variable"));                 // no #!: a library
    CHECK(!Has(kScript + "count=0\neval \"$cmd\"", "unused-variable"));  // eval can read anything
    CHECK(!Has(kScript + "x=0\n((x++))", "unused-variable"));
    // `read -r a b rest`: rest takes the rest of the line, unused on purpose; b is not.
    CHECK(Count(kScript + "while read -r a b rest; do echo \"$a\"; done", "unused-variable") == 1);
    CHECK(!Has(kScript + "unset OLDVAR", "unused-variable"));

    // undefined-variable: lower-case, never assigned, in a self-contained script.
    CHECK(Has(kScript + "name=x\necho \"$nmae\"", "undefined-variable"));
    CHECK(!Has(kScript + "echo \"$HOME $USER $1 $@\"", "undefined-variable"));
    CHECK(!Has(kScript + "echo \"${opt:-default}\"", "undefined-variable"));
    CHECK(!Has(kScript + "[[ -v maybe ]]", "undefined-variable"));
    CHECK(!Has(kScript + "source ./lib.sh\necho \"$from_lib\"", "undefined-variable"));
    CHECK(!Has(kScript + "f() { echo \"$v\"; }\ng() { local v=1; f; }", "undefined-variable"));
    // Past five unknown names the script is getting them from elsewhere.
    CHECK(!Has(kScript + "echo $a1 $a2 $a3 $a4 $a5 $a6", "undefined-variable"));
}

// --- Completion, hover, signature ------------------------------------------

void TestCompletion() {
    const std::string src = "count=1\ngreet() {\n  local name=$1\n  echo \"$na\"\n}\n$co\nre\nread -\n[[ -f x ]]\nset -o pi\n";
    const Lines lines = L(src);
    // After `$`: the file's variables, the function's locals first.
    std::vector<BashLspCompletionItem> items = BashLspCompletions(lines, 3, 11);
    CHECK(!items.empty() && items[0].label == "name");
    CHECK(items[0].replace_start == 9 && items[0].replace_end == 11);
    items = BashLspCompletions(lines, 5, 3);
    CHECK(HasLabel(items, "count"));
    CHECK(!HasLabel(items, "name"));  // another function's local
    items = BashLspCompletions(L("echo $CO"), 0, 8);
    CHECK(HasLabel(items, "COLUMNS") && HasLabel(items, "COMP_WORDS") && !HasLabel(items, "count"));
    // A command: builtins, keywords, the file's functions.
    items = BashLspCompletions(lines, 6, 2);
    CHECK(HasLabel(items, "read") && HasLabel(items, "readonly") && HasLabel(items, "return"));
    items = BashLspCompletions(L("gr"), 0, 2);
    CHECK(!HasLabel(items, "greet"));
    items = BashLspCompletions(L("greet() { :; }\ngr"), 1, 2);
    CHECK(HasLabel(items, "greet"));
    // A builtin's options.
    items = BashLspCompletions(lines, 7, 6);
    CHECK(HasLabel(items, "-r") && HasLabel(items, "-a") && HasLabel(items, "-e"));
    // Test operators in [[ ]].
    items = BashLspCompletions(L("[[ -"), 0, 4);
    CHECK(HasLabel(items, "-f") && HasLabel(items, "-eq"));
    // set -o names.
    items = BashLspCompletions(lines, 9, 9);
    CHECK(HasLabel(items, "pipefail") && !HasLabel(items, "errexit"));
    // ${...}: names, not $? and friends.
    items = BashLspCompletions(L("x=1\necho ${"), 1, 7);
    CHECK(HasLabel(items, "x") && !HasLabel(items, "?"));
}

void TestHover() {
    const std::string src =
        "# Says hello.\n# Takes a name.\ngreet() {\n  local name=$1\n  echo \"hi ${name:-you}\"\n}\ngreet bob\n"
        "read -r line\n[[ -f x ]]\necho $?\nset -o pipefail\n";
    const Lines lines = L(src);
    BashLspHoverInfo h = BashLspHover(lines, 6, 1);  // greet bob
    CHECK(h.found && h.text.find("function greet name") != std::string::npos);
    CHECK(h.text.find("Says hello.\nTakes a name.") != std::string::npos);
    h = BashLspHover(lines, 4, 15);  // name in ${name:-you}
    CHECK(h.found && h.text.find("local variable name") != std::string::npos);
    h = BashLspHover(lines, 4, 19);  // :- in ${name:-you}
    CHECK(h.found && h.text.find("${name:-word}") != std::string::npos);
    h = BashLspHover(lines, 7, 1);  // read
    CHECK(h.found && h.text.find("read [-Eers]") != std::string::npos);
    h = BashLspHover(lines, 7, 6);  // -r
    CHECK(h.found && h.text.find("read -r") != std::string::npos);
    h = BashLspHover(lines, 8, 3);  // -f
    CHECK(h.found && h.text.find("regular file") != std::string::npos);
    h = BashLspHover(lines, 9, 6);  // $?
    CHECK(h.found && h.text.find("exit status") != std::string::npos);
    h = BashLspHover(lines, 10, 9);  // pipefail
    CHECK(h.found && h.text.find("set -o pipefail") != std::string::npos);
    h = BashLspHover(L("for x in a; do :; done"), 0, 1);
    CHECK(h.found && h.text.find("for name") != std::string::npos);
    h = BashLspHover(L("echo $HOME"), 0, 7);
    CHECK(h.found && h.text.find("home directory") != std::string::npos);
}

void TestSignatureHelp() {
    const Lines lines = L("greet() {\n  local name=$1 greeting=\"${2:-hello}\"\n  echo \"$@\"\n}\ngreet bob \nread -r ");
    BashLspSignature s = BashLspSignatureHelp(lines, 4, 10);
    CHECK(s.found && s.label == "greet name [greeting] [args...]");
    CHECK(s.active_param == 1);
    s = BashLspSignatureHelp(lines, 5, 8);
    CHECK(s.found && s.label.rfind("read [-Eers]", 0) == 0);
    CHECK(s.active_param >= 0 && s.params[static_cast<size_t>(s.active_param)] == "[name ...]");
    CHECK(!BashLspSignatureHelp(L("nosuchcommand x"), 0, 15).found);
}

// --- Outline, folding, navigation, rename -------------------------------------

void TestSymbols() {
    const std::vector<BashLspSymbol> syms =
        BashLspSymbols(L("readonly VERSION=1.2\ncount=0\nouter() {\n  inner() { :; }\n  local x=1\n}\n"));
    CHECK(syms.size() == 4);
    CHECK(syms[0].name == "VERSION" && syms[0].kind == BashLspSymbolKind::Constant);
    CHECK(syms[1].name == "count" && syms[1].kind == BashLspSymbolKind::Variable);
    CHECK(syms[2].name == "outer" && syms[2].kind == BashLspSymbolKind::Function && syms[2].parent == -1);
    CHECK(syms[3].name == "inner" && syms[3].parent == 2);
}

void TestFolds() {
    const std::vector<BashLspFold> folds =
        BashLspFolds(L("# a\n# b\nf() {\n  if x; then\n    y\n  fi\n}\ncat <<EOF\nbody\nEOF\n"));
    bool comment = false, fn = false, block = false, heredoc = false;
    for (const BashLspFold &f : folds) {
        comment = comment || (f.start_line == 0 && f.end_line == 1 && f.kind == "comment");
        fn = fn || (f.start_line == 2 && f.end_line == 6);
        block = block || (f.start_line == 3 && f.end_line == 5);
        heredoc = heredoc || (f.start_line == 7 && f.end_line == 9);  // through the delimiter line
    }
    CHECK(comment && fn && block && heredoc);
}

void TestNavigation() {
    const std::string src = "x=1\nf() {\n  local x=2\n  echo \"$x\"\n}\necho $x\ng() { f; }\n";
    const Lines lines = L(src);
    // The local x in f, and the global x, are different variables.
    BashLspLocation d = BashLspDefinition(lines, 3, 9);
    CHECK(d.found && d.range.line == 2);
    d = BashLspDefinition(lines, 5, 6);
    CHECK(d.found && d.range.line == 0);
    std::vector<BashLspReference> refs = BashLspReferences(lines, 5, 6);
    CHECK(refs.size() == 2 && refs[0].range.line == 0 && refs[0].is_definition && refs[1].range.line == 5);
    refs = BashLspReferences(lines, 2, 8);
    CHECK(refs.size() == 2 && refs[1].range.line == 3);
    // Functions: definition and calls.
    d = BashLspDefinition(lines, 6, 6);
    CHECK(d.found && d.range.line == 1);
    refs = BashLspReferences(lines, 1, 0);
    CHECK(refs.size() == 2);

    // Rename: the local only touches f's x.
    std::string err;
    std::vector<BashLspTextEdit> edits = BashLspRename(lines, 2, 8, "y", &err);
    CHECK(err.empty() && edits.size() == 2);
    edits = BashLspRename(lines, 1, 0, "do_f", &err);
    CHECK(err.empty() && edits.size() == 2);
    edits = BashLspRename(lines, 0, 0, "2bad", &err);
    CHECK(edits.empty() && !err.empty());
    // Not the file's own: refused.
    CHECK(!BashLspPrepareRename(L("echo $HOME"), 0, 7).found);
    CHECK(!BashLspPrepareRename(L("echo $never_set"), 0, 7).found);
    CHECK(BashLspPrepareRename(lines, 0, 0).found);
    // Read by `read`, `for`, arithmetic, ${x:=}: all the same variable.
    refs = BashLspReferences(L("read -r n\nfor i in 1; do :; done\n((n += i))\necho ${n:=0}"), 0, 8);
    CHECK(refs.size() == 3);
}

// --- Formatting -------------------------------------------------------------------

void TestFormat() {
    CHECK(Formatted("if a; then\nb\nfi") == "if a; then\n    b\nfi");
    // The file's own unit: two spaces, from the lines already indented one level.
    CHECK(Formatted("f() {\n  a\n      b\n}") == "f() {\n  a\n  b\n}");
    CHECK(Formatted("f() {\n\ta\n        b\n}") == "f() {\n\ta\n\tb\n}");
    CHECK(Formatted("case $x in\na)\necho\n;;\n*) z ;;\nesac") ==
          "case $x in\n    a)\n        echo\n        ;;\n    *) z ;;\nesac");
    CHECK(Formatted("for i in 1; do\nfor j in 2; do\necho\ndone\ndone") ==
          "for i in 1; do\n    for j in 2; do\n        echo\n    done\ndone");
    // Continuation lines: one level past the command...
    CHECK(Formatted("cmd --a \\\n--b |\nsort") == "cmd --a \\\n    --b |\n    sort");
    // ...unless the file keeps its && chains level, as git's tests do.
    CHECK(Formatted("f() {\n\ta &&\n\tb &&\n\tc\n}") == "f() {\n\ta &&\n\tb &&\n\tc\n}");
    CHECK(BashLspFormat(L("f() {\n\ta &&\n\t\tb &&\n\tc\n}")).size() == 1);
    // A comment goes with the code after it -- or before a closing keyword.
    CHECK(Formatted("if a; then\n# why\nb\n# end\nfi") == "if a; then\n    # why\n    b\n    # end\nfi");
    // Here-document bodies and multi-line strings are not touched.
    CHECK(Formatted("f() {\ncat <<EOF\n  keep\nEOF\necho \"a\n  b\"\n}") ==
          "f() {\n    cat <<EOF\n  keep\nEOF\n    echo \"a\n  b\"\n}");
    // A multi-line $( ... ).
    CHECK(Formatted("x=$(\nfoo\n)") == "x=$(\n    foo\n)");
    // A syntax error: nothing is changed.
    CHECK(BashLspFormat(L("if a; then\nb")).empty());
    // Already formatted: no edits.
    CHECK(BashLspFormat(L("f() {\n    echo\n}")).empty());
}

void TestLintFormat() {
    BashLspDiagnostic d;
    d.range.line = 2;
    d.range.col = 4;
    d.severity = BashLspSeverity::Warning;
    d.code = "x";
    d.message = "m";
    CHECK(BashLspFormatDiagnostic("a.sh", d) == "a.sh:3:5: warning: m [x]");
    CHECK(BashLspDiagnosticFails(d));
    d.severity = BashLspSeverity::Information;
    CHECK(!BashLspDiagnosticFails(d));
}

}  // namespace

int main() {
    TestParseShapes();
    TestParseErrors();
    TestDiagnostics();
    TestNameChecks();
    TestCompletion();
    TestHover();
    TestSignatureHelp();
    TestSymbols();
    TestFolds();
    TestNavigation();
    TestFormat();
    TestLintFormat();
    std::printf("bash lsp tests passed (%d checks)\n", g_checks);
    return 0;
}
