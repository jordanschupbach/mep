// mep's Fish formatter, driven directly.
//
// `mep-fish-format-test` runs the checks below;
// `mep-fish-format-test --format [-w N] [--indent N] FILE...` prints each file
// formatted ('-' reads stdin), for tools/format_compare.py.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "fish_format.h"

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
    fishfmt::Options opts;
    int status = 0;
    for (int k = 2; k < argc; k++) {
        const std::string arg = argv[k];
        if ((arg == "-w" || arg == "--width") && k + 1 < argc) {
            opts.width = std::atoi(argv[++k]);
            continue;
        }
        if (arg == "--indent" && k + 1 < argc) {
            opts.indent = std::atoi(argv[++k]);
            continue;
        }
        std::stringstream ss;
        if (arg == "-") {
            ss << std::cin.rdbuf();
        } else {
            std::ifstream in(arg, std::ios::binary);
            ss << in.rdbuf();
        }
        const fishfmt::Result r = fishfmt::Format(ss.str(), opts);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", arg.c_str(), r.error_line, r.error.c_str());
            status = 1;
            continue;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}

// Input and fish_indent's output for it (fish 4.8), byte for byte.
struct Case {
    const char *in, *want;
};
const Case kCases[] = {
        {"if true\necho  hi\nend",
         "if true\n    echo hi\nend\n"},
        {"function f -a x\nset -l y $x;   echo $y\nend\n",
         "function f -a x\n    set -l y $x\n    echo $y\nend\n"},
        {"if test -n x; and test -z y\necho a\nelse if false\necho b\nelse\necho c\nend",
         "if test -n x; and test -z y\n    echo a\nelse if false\n    echo b\nelse\n    echo c\nend\n"},
        {"while read -l line; or true\necho $line\nend",
         "while read -l line; or true\n    echo $line\nend\n"},
        {"switch $x\ncase 'a*' b\necho one\ncase '*'\necho two\nend",
         "switch $x\n    case 'a*' b\n        echo one\n    case '*'\n        echo two\nend\n"},
        {"for i in (seq 3); echo $i; end",
         "for i in (seq 3)\necho $i\nend\n"},
        {"begin; echo a; end",
         "begin\necho a\nend\n"},
        {"{ echo a; echo b; }",
         "{ echo a; echo b; }\n"},
        {"{\necho a\n}",
         "{\n    echo a\n}\n"},
        {"echo 'hi'  \"there\"  'a b'",
         "echo hi there 'a b'\n"},
        {"echo a >  out 2> /dev/null",
         "echo a >out 2>/dev/null\n"},
        {"echo a | \\\n  cat",
         "echo a | cat\n"},
        {"echo a \\\n  b \\\n  c",
         "echo a \\\n    b \\\n    c\n"},
        {"set x (\necho a\necho b\n)",
         "set x (\necho a\necho b\n)\n"},
        {"echo \"$(\necho a\n)\"",
         "echo \"$(\necho a\n)\"\n"},
        {"\n\n# a comment\necho a   # trailing\n\n\n\necho b\n\n\n",
         "# a comment\necho a # trailing\n\necho b\n"},
        {"echo a; and echo b",
         "echo a; and echo b\n"},
        {"not true; or false",
         "not true; or false\n"},
        {"echo a &&\necho b ||\necho c",
         "echo a &&\n    echo b ||\n    echo c\n"},
        {"echo a |\n# note\ncat",
         "echo a |\n    # note\n    cat\n"},
        {"echo 'multi\nline string'\n",
         "echo 'multi\nline string'\n"},
        {"a=1 b=2 \\\n  echo $a",
         "a=1 b=2 \\\n    echo $a\n"},
        {"time sleep 1",
         "time sleep 1\n"},
        {"echo ;;; echo b",
         "echo\necho b\n"},
        {"",
         "\n"},
};

void TestCases() {
    for (const Case &c : kCases) {
        const fishfmt::Result r = fishfmt::Format(c.in);
        if (!r.ok || r.text != c.want)
            std::fprintf(stderr, "input:\n%s\ngot:\n%s\nwant:\n%s\n", c.in, r.text.c_str(), c.want);
        CHECK(r.ok && r.text == c.want);
        // Idempotent -- except, as in fish_indent, a one-line block: its indents come from the
        // input, which had no lines to indent, so the next pass indents the lines it made.
        if (std::strchr(c.in, '\n') || !std::strstr(c.in, "end")) CHECK(fishfmt::Format(r.text).text == r.text);
    }
}

void TestRefusals() {
    // fish_indent would end an unterminated string with a newline it never had: refused.
    fishfmt::Result r = fishfmt::Format("echo 'abc");
    CHECK(!r.ok && r.text == "echo 'abc" && r.error_line == 1);
    // Nesting deeper than the reader goes is refused, not crashed on.
    std::string deep;
    for (int i = 0; i < 3000; i++) deep += "begin\n";
    r = fishfmt::Format(deep);
    CHECK(!r.ok && r.text == deep);
    // Broken input is still formatted when its words survive.
    r = fishfmt::Format("if true\necho a\n");
    CHECK(r.ok && r.text == "if true\n    echo a\n");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);
    TestCases();
    TestRefusals();
    std::printf("fish_format_test: %d checks passed\n", g_checks);
    return 0;
}
