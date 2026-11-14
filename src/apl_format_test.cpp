// mep's APL formatter, driven directly.
//
// `mep-apl-format-test` runs the checks below;
// `mep-apl-format-test --format [--indent N] FILE...` prints each file
// formatted ('-' reads stdin), for tools/format_compare.py and by hand.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "apl_format.h"

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
    aplfmt::Options opts;
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
        const aplfmt::Result r = aplfmt::Format(ss.str(), opts);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", arg.c_str(), r.error_line, r.error.c_str());
            status = 1;
            continue;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}

// Formats `in`, expects `want`, and checks that formatting again changes nothing.
void Expect(const std::string &in, const std::string &want, int line, int indent = 4) {
    aplfmt::Options o;
    o.indent = indent;
    const aplfmt::Result r = aplfmt::Format(in, o);
    if (!r.ok || r.text != want) {
        std::fprintf(stderr, "--- input ---\n%s\n--- want ---\n%s\n--- got (%s) ---\n%s\n", in.c_str(), want.c_str(),
                     r.ok ? "ok" : r.error.c_str(), r.text.c_str());
    }
    Check(r.ok && r.text == want, "formatted as expected", line);
    const aplfmt::Result again = aplfmt::Format(r.text, o);
    Check(again.ok && again.text == r.text, "idempotent", line);
}
#define EXPECT(in, want) Expect((in), (want), __LINE__)

void ExpectRefused(const std::string &in, const std::string &reason, int line) {
    const aplfmt::Result r = aplfmt::Format(in);
    if (r.ok || r.error.find(reason) == std::string::npos) {
        std::fprintf(stderr, "--- input ---\n%s\n--- error: %s (want %s)\n", in.c_str(), r.error.c_str(), reason.c_str());
    }
    Check(!r.ok && r.text == in && r.error.find(reason) != std::string::npos, "refused", line);
}
#define REFUSED(in, reason) ExpectRefused((in), (reason), __LINE__)

void TestSpacing() {
    // No spaces around ←, one around ⋄, one before a trailing comment, one after ⍝.
    EXPECT("x ← 1", "x←1");
    EXPECT("x+←  1 ⋄y←2⋄  z←3", "x+←1 ⋄ y←2 ⋄ z←3");
    // Only the gaps next to ← change: `x f←1` must never become `xf←1`.
    EXPECT("x f ← 1", "x f←1");
    EXPECT("x←1⍝note", "x←1 ⍝ note");
    EXPECT("⍝comment", "⍝ comment");
    EXPECT("⍝   indented doc code", "⍝   indented doc code");
    EXPECT("⍝⍝ banner ⍝⍝", "⍝⍝ banner ⍝⍝");
    EXPECT("⍝", "⍝");
    EXPECT("⍝∇ example", "⍝∇ example");
    // Strings are never touched, nor are their spaces.
    EXPECT("s ← 'a ← b ⋄ c⍝d'", "s←'a ← b ⋄ c⍝d'");
    EXPECT("s←\"gnu ← \\\" x\"", "s←\"gnu ← \\\" x\"");
    // Trailing whitespace goes; blank lines stay blank.
    EXPECT("x←1   \n   \ny←2\t", "x←1\n\ny←2");
    // Spacing elsewhere is kept: 1 2 3, f g, (a b).
    EXPECT("r ←  1  2   3 +  f  g (a  b)", "r←1  2   3 +  f  g (a  b)");
    // A diamond in brackets: still one space.
    EXPECT("x←(1⋄2)", "x←(1 ⋄ 2)");
}

void TestControlStructures() {
    EXPECT(":If x>0\ny←1\n:ElseIf x<0\ny←¯1\n:Else\ny←0\n:EndIf",
           ":If x>0\n    y←1\n:ElseIf x<0\n    y←¯1\n:Else\n    y←0\n:EndIf");
    EXPECT(":For i :In ⍳3\n:If i=2\n:Continue\n:EndIf\ns+←i\n:EndFor",
           ":For i :In ⍳3\n    :If i=2\n        :Continue\n    :EndIf\n    s+←i\n:EndFor");
    EXPECT(":Repeat\nx+←1\n:Until x>5", ":Repeat\n    x+←1\n:Until x>5");
    EXPECT(":Select c\n:Case 1\na\n:CaseList 2 3\nb\n:Else\nd\n:EndSelect",
           ":Select c\n:Case 1\n    a\n:CaseList 2 3\n    b\n:Else\n    d\n:EndSelect");
    EXPECT(":while x\nx-←1\n:end", ":while x\n    x-←1\n:end");
    EXPECT(":Trap 0\nf x\n:Else\n⎕←'failed'\n:EndTrap", ":Trap 0\n    f x\n:Else\n    ⎕←'failed'\n:EndTrap");
    // One-line structures stay one line.
    EXPECT(":If x ⋄ y ⋄ :EndIf\nz", ":If x ⋄ y ⋄ :EndIf\nz");
    // The opener's own indentation is kept at top level; the body follows it.
    EXPECT("  :If x\ny\n  :EndIf", "  :If x\n      y\n  :EndIf");
    // A comment line in a body is indented with it.
    EXPECT(":If x\n⍝ why\ny\n:EndIf", ":If x\n    ⍝ why\n    y\n:EndIf");
    // Indent width.
    Expect(":If x\ny\n:EndIf", ":If x\n  y\n:EndIf", __LINE__, 2);
}

void TestTradfnsAndDfns() {
    EXPECT("∇Z←A PLUS B;T\n⍝ Adds.\nT←A\n:If T>0\nZ←T+B\n:EndIf\n∇",
           "∇Z←A PLUS B;T\n  ⍝ Adds.\n  T←A\n  :If T>0\n      Z←T+B\n  :EndIf\n∇");
    // Labels keep their line.
    EXPECT("∇ R←Loop N\nR←0\nL1:R←R+1\n→(R<N)/L1\n∇", "∇ R←Loop N\n  R←0\n  L1:R←R+1\n  →(R<N)/L1\n∇");
    // Multi-line dfns: body one level in, the closing brace with its line.
    EXPECT("mean←{\n⍝ the mean\n(+/⍵)÷≢⍵\n}", "mean←{\n    ⍝ the mean\n    (+/⍵)÷≢⍵\n}");
    EXPECT("f←{\n⍵≤1:1\ng←{\n⍵+1\n}\ng ⍵\n}", "f←{\n    ⍵≤1:1\n    g←{\n        ⍵+1\n    }\n    g ⍵\n}");
    // One-line dfns are left alone.
    EXPECT("fact←{⍵≤1:1 ⋄ ⍵×∇ ⍵-1}", "fact←{⍵≤1:1 ⋄ ⍵×∇ ⍵-1}");
    // A namespace script with a tradfn and a dfn.
    EXPECT(":Namespace dm\n⍝ docs\n∇ R←Add Y\nR←Y+1\n∇\nMean←{\n(+/⍵)÷≢⍵\n}\n:EndNamespace",
           ":Namespace dm\n    ⍝ docs\n    ∇ R←Add Y\n      R←Y+1\n    ∇\n    Mean←{\n        (+/⍵)÷≢⍵\n    }\n"
           ":EndNamespace");
    // ∇⍫ closes too.
    EXPECT("∇ X\n1\n∇⍫", "∇ X\n  1\n∇⍫");
}

void TestKeptAsIs() {
    // )commands, ]user commands, a #! line.
    EXPECT("#!/usr/bin/apl --script\n  )LOAD  ws  \n]box on", "#!/usr/bin/apl --script\n  )LOAD  ws\n]box on");
    // Lines continuing a multi-line ( ) keep their indentation.
    EXPECT(":If x\nv←(1 2\n        3 4)\n:EndIf", ":If x\n    v←(1 2\n        3 4)\n:EndIf");
    // A Dyalog .aplf body (no ∇) at top level keeps its indentation.
    EXPECT(" r←Foo y\n r←2×y", " r←Foo y\n r←2×y");
}

void TestRefusals() {
    REFUSED("x←'open", "unterminated string");
    REFUSED(":If x\ny", "unclosed control structure");
    REFUSED("y\n:EndIf", "without a matching opening word");
    REFUSED(":If x\n:EndFor", "does not match");
    REFUSED("f←{\n⍵", "unclosed '{'");
    REFUSED("x)", "unbalanced");
    REFUSED("∇ F\n:If 1\n∇", "not closed before the ∇");
    REFUSED("∇ F\n1", "unclosed function");
}

void TestTokensPreserved() {
    // A larger script, formatted, must lex to the same tokens: the formatter
    // checks it itself and would refuse otherwise.
    const std::string src =
        "⍝ A GNU APL script\n"
        "⎕IO ← 0\n"
        "∇Z←A PLUS B\n"
        "Z←A+B\n"
        "∇\n"
        "+/ ⍳ 10 ⋄ 1 2 PLUS 3 4\n"
        "M ← 3 3 ⍴ ⍳ 9 ⋄ M[1;] ← 0 ⋄ ∘.×⍨ ⍳5\n"
        "x ← ¯1.5E¯3J2 ⍝comment\n"
        "]box on\n";
    const aplfmt::Result r = aplfmt::Format(src);
    CHECK(r.ok);
    CHECK(r.text.find("⎕IO←0\n") != std::string::npos);
    CHECK(r.text.find("  Z←A+B\n") != std::string::npos);
    CHECK(r.text.find("M←3 3 ⍴ ⍳ 9 ⋄ M[1;]←0 ⋄ ∘.×⍨ ⍳5") != std::string::npos);
    const aplfmt::Result again = aplfmt::Format(r.text);
    CHECK(again.ok && again.text == r.text);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);
    TestSpacing();
    TestControlStructures();
    TestTradfnsAndDfns();
    TestKeptAsIs();
    TestRefusals();
    TestTokensPreserved();
    std::printf("apl_format_test: %d checks passed\n", g_checks);
    return 0;
}
