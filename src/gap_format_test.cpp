// mep's GAP formatter, driven directly.
//
// `mep-gap-format-test` runs the checks below;
// `mep-gap-format-test --format [-w N] [--indent N] FILE...` prints each file
// formatted ('-' reads stdin), for tools/format_compare.py;
// `mep-gap-format-test --check FILE...` formats each file and checks that the
// tokens and comments are unchanged and that formatting again changes nothing.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "gap_format.h"
#include "gap_parse.h"

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
    gapfmt::Options opts;
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
        const gapfmt::Result r = gapfmt::Format(ss.str(), opts);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", arg.c_str(), r.error_line, r.error.c_str());
            status = 1;
            continue;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}

std::string Slurp(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Every token's spelling and every comment's text, in order.
std::string Stream(const std::string &crlf) {
    std::string text;  // line ends are normalised to LF
    for (size_t i = 0; i < crlf.size(); i++)
        if (!(crlf[i] == '\r' && i + 1 < crlf.size() && crlf[i + 1] == '\n')) text += crlf[i];
    std::vector<gapparse::Token> toks;
    std::vector<gapparse::Comment> comments;
    gapparse::Tokenize(text, &toks, &comments);
    std::string out;
    for (const auto &t : toks) out.append(text, t.begin, t.end - t.begin) += '\x01';
    for (const auto &c : comments) {
        std::string body = text.substr(c.begin, c.end - c.begin);
        while (!body.empty() && (body.back() == ' ' || body.back() == '\t' || body.back() == '\r')) body.pop_back();
        out += body + '\x02';
    }
    return out;
}

int CheckFiles(int argc, char **argv) {
    int files = 0, refused = 0, changed = 0, bad = 0;
    for (int k = 2; k < argc; k++) {
        const std::string text = Slurp(argv[k]);
        files++;
        const gapfmt::Result r = gapfmt::Format(text);
        if (!r.ok) {
            refused++;
            continue;
        }
        if (r.text != text) changed++;
        const bool same = Stream(r.text) == Stream(text);
        const gapfmt::Result again = gapfmt::Format(r.text);
        const bool idem = again.ok && again.text == r.text;
        if (!same || !idem) {
            bad++;
            std::printf("%s: %s\n", argv[k], !same ? "tokens changed" : "not idempotent");
        }
    }
    std::printf("files %d, refused (syntax errors) %d, reformatted %d, bad %d\n", files, refused, changed, bad);
    return bad ? 1 : 0;
}

std::string F(const std::string &s) { return gapfmt::Format(s).text; }

void TestIndent() {
    CHECK(F("f:=function(x)\nlocal a;\nif x then\na:=1;\nelif a then\na:=2;\nelse\na:=3;\nfi;\nreturn a;\nend;\n") ==
          "f:=function(x)\n    local a;\n    if x then\n        a:=1;\n    elif a then\n        a:=2;\n    else\n"
          "        a:=3;\n    fi;\n    return a;\nend;\n");
    // Loops, repeat, a function inside a call, continuation lines.
    CHECK(F("for i in [1..3] do\n  Print(i);\n    od;\nrepeat\nx:=x+1;\nuntil x>3;\n") ==
          "for i in [1..3] do\n    Print(i);\nod;\nrepeat\n    x:=x+1;\nuntil x>3;\n");
    CHECK(F("InstallMethod(Foo,\n\"for x\",\n[IsInt],\nfunction(x)\nreturn 1;\nend);\n") ==
          "InstallMethod(Foo,\n    \"for x\",\n    [IsInt],\n    function(x)\n        return 1;\n    end);\n");
    CHECK(F("InstallMethod(Foo, \"x\", [IsInt], function(x)\nreturn 1;\nend);\n") ==
          "InstallMethod(Foo, \"x\", [IsInt], function(x)\n    return 1;\nend);\n");
    CHECK(F("x := a +\nb;\ny := 1;\n") == "x := a +\n    b;\ny := 1;\n");
    // Comments follow the code; trailing blanks and trailing blank lines go; a multi-line string stays.
    CHECK(F("if x then\n# note   \nfi;   \n\n\n") == "if x then\n    # note\nfi;\n");
    CHECK(F("f := function()\nreturn \"\"\"a  \n  b\"\"\";\nend;\n") ==
          "f := function()\n    return \"\"\"a  \n  b\"\"\";\nend;\n");
    CHECK(F("if a then\nx:=1;\n# else: b\nelse\nx:=2;\nfi;\n") == "if a then\n    x:=1;\n# else: b\nelse\n    x:=2;\nfi;\n");
    CHECK(F("atomic readwrite x do\ny := 1;\nod;\n") == "atomic readwrite x do\n    y := 1;\nod;\n");
    // A syntax error refuses, with GAP's message and line.
    const gapfmt::Result bad = gapfmt::Format("x := 1;\ny := ;\n");
    CHECK(!bad.ok && bad.error_line == 2 && bad.text == "x := 1;\ny := ;\n");
    gapfmt::Options two;
    two.indent = 2;
    CHECK(gapfmt::Format("f:=function()\nreturn 1;\nend;\n", two).text == "f:=function()\n  return 1;\nend;\n");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "--check") == 0) return CheckFiles(argc, argv);
    TestIndent();
    // Idempotence: formatting formatted text changes nothing.
    const std::string once = gapfmt::Format("f:=function(x)\nif x then\nreturn [1,\n2];\nfi;\nend;\n").text;
    CHECK(gapfmt::Format(once).text == once);
    std::printf("gap_format_test: %d checks passed\n", g_checks);
    return 0;
}
