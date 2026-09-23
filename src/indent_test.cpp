// Coverage for indent.h/.cpp: mep's auto-indent policy. Pure string->string
// helpers, so every case is an exact input->output assertion with no editor,
// buffer or display in the loop. The two helpers are:
//   ComputeNewlineIndent  -- the leading whitespace a new line (Enter / o / O)
//                            gets, inheriting the current line and, in Python,
//                            reacting to a block-opening ':' or a flow keyword.
//   ReindentDedentKeyword -- the re-alignment of a Python else/elif/except/
//                            finally clause the moment its ':' is typed.

#include "indent.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace {
// Asserts ComputeNewlineIndent(line, ft) == want, printing all three on a
// mismatch so a failure reads like a diff rather than a bare boolean.
void ExpectNewline(const std::string &line, const std::string &ft, const std::string &want) {
    std::string got = mepindent::ComputeNewlineIndent(line, ft);
    if (got != want) {
        std::fprintf(stderr,
                     "ComputeNewlineIndent(\"%s\", \"%s\") = \"%s\", want \"%s\"\n",
                     line.c_str(), ft.c_str(), got.c_str(), want.c_str());
        std::abort();
    }
}

void TestInheritAllFiletypes() {
    // The base behaviour for every filetype: copy the current line's indent.
    ExpectNewline("    foo", "py", "    ");
    ExpectNewline("    foo", "cpp", "    ");
    ExpectNewline("    foo", "txt", "    ");
    ExpectNewline("foo", "py", "");
    ExpectNewline("", "py", "");
    ExpectNewline("        x = 1", "rs", "        ");
    // Tabs are preserved verbatim, not normalised to spaces.
    ExpectNewline("\t\tfoo", "go", "\t\t");
    ExpectNewline("\tif (x) {", "c", "\t");  // no ':' logic outside Python
    // A C/JS line ending in ':' (label, ternary, object key) gets NO bonus --
    // colon logic is Python-only.
    ExpectNewline("  case 1:", "cpp", "  ");
    ExpectNewline("  default:", "js", "  ");
}

void TestPythonColonOpensBlock() {
    ExpectNewline("def f():", "py", "    ");
    ExpectNewline("    if x:", "py", "        ");
    ExpectNewline("        for i in xs:", "py", "            ");
    ExpectNewline("class A:", "pyi", "    ");
    // Trailing whitespace after the ':' still counts as a block opener.
    ExpectNewline("def f():   ", "py", "    ");
    // A trailing comment is stripped before looking for the ':'.
    ExpectNewline("def f():  # ctor", "py", "    ");
    // A ':' that is only inside a comment does NOT open a block.
    ExpectNewline("x = 1  # note:", "py", "");
    ExpectNewline("    y = 2  # a: b", "py", "    ");
    // A ':' inside a string literal is not a block opener either.
    ExpectNewline("s = \"a:\"", "py", "");
    // Mid-line split (cursor after '('): last code char is '(', not ':'.
    ExpectNewline("def f(", "py", "");
    // Dict/annotation colon mid-line, cursor past it, line ends on ':' -> this
    // is the honest limitation: a line ending in ':' is treated as an opener.
    // Assert the common true-positive rather than pretend we disambiguate it.
    ExpectNewline("d = {", "py", "");
}

void TestPythonFlowKeywordDedents() {
    ExpectNewline("    return 1", "py", "");
    ExpectNewline("        return", "py", "    ");
    ExpectNewline("        pass", "py", "    ");
    ExpectNewline("        break", "py", "    ");
    ExpectNewline("        continue", "py", "    ");
    ExpectNewline("        raise ValueError()", "py", "    ");
    // Floor at column 0.
    ExpectNewline("return", "py", "");
    // Whole-word only: `returns`/`passing` are ordinary names, no dedent.
    ExpectNewline("    returns = 1", "py", "    ");
    ExpectNewline("    passing = True", "py", "    ");
    // Not Python: flow keywords are just words, inherit only.
    ExpectNewline("    return 1;", "cpp", "    ");
}

// ---- ReindentDedentKeyword ----

void ExpectReindent(const std::string &line, const std::string &ft,
                    const std::optional<std::string> &want) {
    std::optional<std::string> got = mepindent::ReindentDedentKeyword(line, ft);
    bool ok = (got.has_value() == want.has_value()) && (!got || *got == *want);
    if (!ok) {
        std::fprintf(stderr, "ReindentDedentKeyword(\"%s\", \"%s\") = %s, want %s\n",
                     line.c_str(), ft.c_str(),
                     got ? ("\"" + *got + "\"").c_str() : "nullopt",
                     want ? ("\"" + *want + "\"").c_str() : "nullopt");
        std::abort();
    }
}

void TestDedentClauseRealign() {
    ExpectReindent("        else:", "py", std::string("    "));
    ExpectReindent("    elif x:", "py", std::string(""));
    ExpectReindent("    except Foo:", "py", std::string(""));
    ExpectReindent("    except* Foo:", "py", std::string(""));
    ExpectReindent("    finally:", "py", std::string(""));
    ExpectReindent("            else:", "pyi", std::string("        "));
    // Tab-indented clause: one tab is one level.
    ExpectReindent("\t\telse:", "py", std::string("\t"));
    // Already at column 0: nothing to dedent.
    ExpectReindent("else:", "py", std::nullopt);
    // Not a dedent clause.
    ExpectReindent("    return", "py", std::nullopt);
    ExpectReindent("    if x:", "py", std::nullopt);
    // Whole-word only: `elsewhere`/`elifetime` are names, not clauses.
    ExpectReindent("    elsewhere = 1", "py", std::nullopt);
    ExpectReindent("    exceptional = 1", "py", std::nullopt);
    // Not Python.
    ExpectReindent("    else:", "cpp", std::nullopt);
    ExpectReindent("    default:", "cpp", std::nullopt);
}

}  // namespace

int main() {
    TestInheritAllFiletypes();
    TestPythonColonOpensBlock();
    TestPythonFlowKeywordDedents();
    TestDedentClauseRealign();
    std::printf("indent tests passed\n");
    return 0;
}
