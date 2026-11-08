// mep's Tcl formatter, driven directly.
//
// `mep-tcl-format-test` runs the checks below;
// `mep-tcl-format-test --format [-w N] [--indent N] FILE...` prints each file
// formatted ('-' reads stdin), for tools/format_compare.py.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "tcl_format.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

int FormatFiles(int argc, char **argv) {
    tclfmt::Options opts;
    int status = 0;
    for (int k = 2; k < argc; k++) {
        const std::string arg = argv[k];
        if ((arg == "-w" || arg == "--width") && k + 1 < argc) {
            opts.width = std::atoi(argv[++k]);
            continue;
        }
        if (arg == "--indent" && k + 1 < argc) {
            const std::string v = argv[++k];
            if (v == "tab") {
                opts.tabs = true;
            } else {
                opts.indent = std::atoi(v.c_str());
            }
            continue;
        }
        std::stringstream ss;
        if (arg == "-") {
            ss << std::cin.rdbuf();
        } else {
            std::ifstream in(arg, std::ios::binary);
            ss << in.rdbuf();
        }
        const tclfmt::Result r = tclfmt::Format(ss.str(), opts);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", arg.c_str(), r.error_line, r.error.c_str());
            status = 1;
            continue;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}

}  // namespace

// Formats `in` and checks the result is `want`, and that formatting it
// again changes nothing.
void Expect(const std::string &in, const std::string &want, const tclfmt::Options &o = {}) {
    const tclfmt::Result r = tclfmt::Format(in, o);
    if (!r.ok || r.text != want) {
        std::fprintf(stderr, "input:\n%s\nwant:\n%s\ngot (ok=%d %s):\n%s\n", in.c_str(), want.c_str(), r.ok ? 1 : 0,
                     r.error.c_str(), r.text.c_str());
    }
    CHECK(r.ok);
    CHECK(r.text == want);
    const tclfmt::Result again = tclfmt::Format(r.text, o);
    CHECK(again.ok);
    CHECK(again.text == r.text);
}

void ExpectRefused(const std::string &in) {
    const tclfmt::Result r = tclfmt::Format(in);
    CHECK(!r.ok);
    CHECK(r.text == in);
    CHECK(!r.error.empty());
}

void Rules() {
    // Script arguments of builtins are reindented; words are single-spaced.
    Expect("proc  foo {a  b} {\nset x 1\n      puts   $x\n}\n", "proc foo {a b} {\n    set x 1\n    puts $x\n}\n");
    // One-line bodies stay on one line; empty ones become {}.
    Expect("if {$a} {  puts hi  } else {   }\n", "if {$a} {puts hi} else {}\n");
    // Expressions get one space around binary operators.
    Expect("expr {$a+1*(2-$b)}\n", "expr {$a + 1 * (2 - $b)}\n");
    Expect("set x [expr {max($a,$b) ? -$a : !$b}]\n", "set x [expr {max($a, $b) ? -$a : !$b}]\n");
    Expect("expr 1+2\n", "expr 1 + 2\n");
    // Multi-line expressions open on their own line.
    Expect("if {$a &&\n  $b} {\nfoo\n}\n", "if {\n    $a &&\n    $b\n} {\n    foo\n}\n");
    // A body given as a bare or quoted word is braced.
    Expect("if 1 foo\n", "if 1 {foo}\n");
    Expect("eval foo bar\n", "eval {foo bar}\n");
    Expect("after 100 set x 1\n", "after 100 {set x 1}\n");
    // proc's argument list is braced and its defaults are lists.
    Expect("proc f a {}\n", "proc f {a} {}\n");
    Expect("proc f {a   {b  1}} {}\n", "proc f {a {b 1}} {}\n");
    // switch arms are scripts, `-` fall-throughs stay.
    Expect("switch $x {\na -\nb {foo}\n  default {bar;baz}\n}\n",
           "switch $x {\n    a -\n    b {foo}\n    default {bar; baz}\n}\n");
    // Blank lines: at most two; none at the start or end.
    Expect("\n\nfoo\n\n\n\n\nbar\n\n\n", "foo\n\n\nbar\n");
    // Commands on one line keep their `;`, comments their ` ;#`.
    Expect("a;b  ;  c\nd   ;# note   \n", "a; b; c\nd ;# note\n");
    // A comment on the opening brace's line stays there.
    Expect("proc f {} { # note\nfoo\n}\n", "proc f {} { # note\n    foo\n}\n");
    // Continuation lines: `\` plus one indent.
    Expect("cmd a \\\n        b \\\n  c\n", "cmd a \\\n    b \\\n    c\n");
    // Command substitutions with several commands on several lines.
    Expect("puts [\nfoo\nbar]\n", "puts [\n    foo\n    bar\n]\n");
    Expect("puts [foo;bar]\n", "puts [foo; bar]\n");
    // Data in braces and quotes is copied verbatim.
    Expect("set l {a\n   b  c}\nset s \"x   y\"\n", "set l {a\n   b  c}\nset s \"x   y\"\n");
    // namespace eval bodies are scripts too.
    Expect("namespace eval ns {\nvariable x\n}\n", "namespace eval ns {\n    variable x\n}\n");
    // tclfmt-disable ... tclfmt-enable is copied as it stands.
    Expect("# tclfmt-disable\nset   a  1\n  # tclfmt-enable\nset   b 2\n",
           "# tclfmt-disable\nset   a  1\n  # tclfmt-enable\nset b 2\n");
    // CRLF reads as LF.
    Expect("set a 1\r\nset b 2\r\n", "set a 1\nset b 2\n");
    // Deliberate departures from tclfmt, which would change meaning:
    // `$a()` keeps its empty index ...
    Expect("puts $a()\n", "puts $a()\n");
    // ... and a comment continued by a backslash onto a blank line keeps it.
    Expect("# a \\\n\nputs b\n", "# a \\\n\nputs b\n");
}

void Options() {
    tclfmt::Options two;
    two.indent = 2;
    Expect("proc f {} {\nfoo\n}\n", "proc f {} {\n  foo\n}\n", two);
    tclfmt::Options tabs;
    tabs.tabs = true;
    Expect("proc f {} {\nfoo\n}\n", "proc f {} {\n\tfoo\n}\n", tabs);
    tclfmt::Options braces;
    braces.spaces_in_braces = true;
    Expect("if {$a} {foo}\n", "if { $a } { foo }\n", braces);
    tclfmt::Options flat;
    flat.indent_namespace_eval = false;
    Expect("namespace eval ns {\n  variable x\n}\n", "namespace eval ns {\nvariable x\n}\n", flat);
    tclfmt::Options blank;
    blank.max_blank_lines = 0;
    Expect("a\n\n\nb\n", "a\nb\n", blank);
}

void Refusals() {
    ExpectRefused("proc f {} {\n  puts x\n");  // unclosed brace
    ExpectRefused("puts \"abc\n");               // unclosed quote
    ExpectRefused("puts [foo\n");                // unclosed bracket
    ExpectRefused("set a {b}c\n");               // junk after a braced word
    ExpectRefused("if {1 +* 2} {}\n");           // not an expression
    ExpectRefused("puts \xff\n");                // not UTF-8
    {
        const tclfmt::Result r = tclfmt::Format("puts a\nproc f {} {\n");
        CHECK(!r.ok);
        CHECK(r.error_line == 2);
    }
    // Pathological nesting refuses instead of overflowing the stack.
    std::string deep;
    for (int k = 0; k < 5000; k++) deep += "[";
    for (int k = 0; k < 5000; k++) deep += "]";
    ExpectRefused("puts " + deep + "\n");
    std::string bodies = "x";
    for (int k = 0; k < 300; k++) bodies = "if 1 {" + bodies + "}";
    ExpectRefused(bodies + "\n");
    std::string parens = "1";
    for (int k = 0; k < 2000; k++) parens = "(" + parens + ")";
    ExpectRefused("expr {" + parens + "}\n");
    // Arguments tclint cannot see into are left alone, not refused.
    Expect("if $cond $body\n", "if $cond $body\n");
    Expect("expr {$}\n", "expr {$}\n");
}

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);
    Rules();
    Options();
    Refusals();
    // Idempotence: formatting formatted text changes nothing.
    const std::string once = tclfmt::Format("x\n").text;
    CHECK(tclfmt::Format(once).text == once);
    std::printf("tcl_format_test: %d checks passed\n", g_checks);
    return 0;
}
