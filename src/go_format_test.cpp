// mep's Go formatter, driven directly.
//
// `mep-go-format-test` runs the checks below (expected outputs are gofmt
// 1.26.5's); `mep-go-format-test --format [-w N] [--indent N] FILE...`
// prints each file formatted ('-' reads stdin, which, as with gofmt, also
// accepts a fragment), for tools/format_compare.py.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "go_format.h"

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
    gofmt::Options opts;
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
        const gofmt::Result r = gofmt::Format(ss.str(), opts);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", arg.c_str(), r.error_line, r.error.c_str());
            status = 1;
            continue;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}

// Formats `in`, requires `want`, and requires formatting `want` to change nothing.
bool Same(const std::string &in, const std::string &want) {
    const gofmt::Result r = gofmt::Format(in);
    if (!r.ok || r.text != want) {
        std::fprintf(stderr, "--- input\n%s--- got (%s)\n%s--- want\n%s", in.c_str(), r.error.c_str(),
                     r.text.c_str(), want.c_str());
        return false;
    }
    const gofmt::Result again = gofmt::Format(want);
    return again.ok && again.text == want;
}

void TestBasics() {
    CHECK(Same("package  p\nfunc f( a,b int )int{return a+b*2}\n",
               "package p\n\nfunc f(a, b int) int { return a + b*2 }\n"));
    // Binary expression spacing follows precedence (Normal vs Compact mode).
    CHECK(Same("package p\n\nvar x = a*b + c\nvar y = f(a + b, c * d)\n",
               "package p\n\nvar x = a*b + c\nvar y = f(a+b, c*d)\n"));
    // Number normalization.
    CHECK(Same("package p\n\nconst x = 0X1F + 1E3 + 0O7 + 0B1 + 0012i\n",
               "package p\n\nconst x = 0x1F + 1e3 + 0o7 + 0b1 + 12i\n"));
    // Redundant parens around if/for headers go.
    CHECK(Same("package p\n\nfunc f() {\n\tif (x) {\n\t}\n}\n", "package p\n\nfunc f() {\n\tif x {\n\t}\n}\n"));
}

void TestAlignment() {
    CHECK(Same("package p\n\ntype T struct {\n\tA int // a\n\tBcd string `json:\"b\"`\n\tc, d float64\n}\n",
               "package p\n\ntype T struct {\n\tA    int    // a\n\tBcd  string `json:\"b\"`\n"
               "\tc, d float64\n}\n"));
    CHECK(Same("package p\n\nconst (\n\tX = 1\n\tYzz int = 2 // why\n)\n",
               "package p\n\nconst (\n\tX       = 1\n\tYzz int = 2 // why\n)\n"));
    CHECK(Same("package p\n\nvar m = map[string]int{\n\t\"a\": 1,\n\t\"bbbb\": 2,\n}\n",
               "package p\n\nvar m = map[string]int{\n\t\"a\":    1,\n\t\"bbbb\": 2,\n}\n"));
}

void TestComments() {
    // Doc comments are reformatted (go/doc/comment): code blocks get a tab,
    // lists a canonical marker, and a heading keeps its blank lines.
    CHECK(Same("package p\n\n// F does things.\n//   code()\n// - one\n// - two\nfunc F() {}\n",
               "package p\n\n// F does things.\n//\n//\tcode()\n//\n// - one\n// - two\nfunc F() {}\n"));
    // Trailing comments line up; a /* */ comment inside an expression stays.
    CHECK(Same("package p\n\nvar (\n\ta = 1 // one\n\tbbb = 22 // two\n)\n\nvar c = f(/* x */ 1)\n",
               "package p\n\nvar (\n\ta   = 1  // one\n\tbbb = 22 // two\n)\n\nvar c = f( /* x */ 1)\n"));
    // //go:build moves above the package doc and // +build is kept in sync.
    CHECK(Same("// +build linux darwin\n\npackage p\n",
               "//go:build linux || darwin\n// +build linux darwin\n\npackage p\n"));
}

void TestImports() {
    CHECK(Same("package p\n\nimport (\n\t\"os\"\n\t\"fmt\"\n\t\"fmt\"\n\n\tb \"bytes\"\n\t\"a/z\"\n)\n",
               "package p\n\nimport (\n\t\"fmt\"\n\t\"os\"\n\n\t\"a/z\"\n\tb \"bytes\"\n)\n"));
}

void TestFragments() {
    // Like gofmt on stdin: declarations or statements without a package clause.
    CHECK(Same("func f(){g( 1)}\n", "func f() { g(1) }\n"));
    CHECK(Same("x:=1\nif x>0{y( )}\n", "x := 1\nif x > 0 {\n\ty()\n}\n"));
    CHECK(Same("\t\tx:=1\n", "\t\tx := 1\n"));
}

void TestErrors() {
    const gofmt::Result r = gofmt::Format("package p\n\nfunc f() {\n\tx :=\n}\n");
    CHECK(!r.ok);
    CHECK(r.error_line == 5);
    CHECK(r.error.find("expected operand") != std::string::npos);
    CHECK(r.text == "package p\n\nfunc f() {\n\tx :=\n}\n");
    // 20k nested parentheses: the parser's nesting cap refuses it, no crash.
    std::string deep = "package p\n\nvar x = " + std::string(20000, '(') + "1" + std::string(20000, ')') + "\n";
    const gofmt::Result d = gofmt::Format(deep);
    CHECK(!d.ok);
    // A long left-deep sum is not nesting for the parser; the printer's own
    // recursion cap refuses it rather than overflowing the stack.
    std::string sum = "package p\n\nvar x = a";
    for (int i = 0; i < 20000; i++) sum += "+a";
    sum += "\n";
    CHECK(!gofmt::Format(sum).ok);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);
    TestBasics();
    TestAlignment();
    TestComments();
    TestImports();
    TestFragments();
    TestErrors();
    std::printf("go_format_test: %d checks passed\n", g_checks);
    return 0;
}
