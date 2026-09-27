// Coverage for indent.h/.cpp: mep's auto-indent policy. Pure string->string
// helpers, so every case is an exact input->output assertion with no editor,
// buffer or display in the loop. The two helpers are:
//   ComputeNewlineIndent  -- the leading whitespace a new line (Enter / o / O)
//                            gets, inheriting the current line and, in Python,
//                            reacting to a block-opening ':' or a flow keyword.
//   ReindentDedentKeyword -- the re-alignment of a Python else/elif/except/
//                            finally/case clause the moment its ':' is typed,
//                            onto the column of the statement it actually pairs
//                            with (found in the lines above it).
//   ReindentPastedText    -- a pasted block re-aligned onto the indent of
//                            wherever it lands, its own shape kept.
//   PasteSpliceCol        -- where a multi-line paste is actually spliced in,
//                            which is never inside the line's own indent.
//   IndentBackspaceWidth  -- how much whitespace one Backspace press eats when
//   IndentDeleteWidth        the cursor is in an indent (a whole soft tab).

#include "indent.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

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

// The clause being typed is the last line of `lines`; everything before it is
// the context it is matched against.
void ExpectReindent(const std::vector<std::string> &lines, const std::string &ft,
                    const std::optional<std::string> &want) {
    const int row = static_cast<int>(lines.size()) - 1;
    std::optional<std::string> got = mepindent::ReindentDedentKeyword(lines, row, ft);
    bool ok = (got.has_value() == want.has_value()) && (!got || *got == *want);
    if (!ok) {
        std::string ctx;
        for (const std::string &l : lines) ctx += "  |" + l + "\n";
        std::fprintf(stderr, "ReindentDedentKeyword(..., \"%s\") = %s, want %s\n%s", ft.c_str(),
                     got ? ("\"" + *got + "\"").c_str() : "nullopt",
                     want ? ("\"" + *want + "\"").c_str() : "nullopt", ctx.c_str());
        std::abort();
    }
}

// A clause with no context above it at all -- the fallback path, one level out.
void ExpectReindentBare(const std::string &line, const std::string &ft,
                        const std::optional<std::string> &want) {
    ExpectReindent({line}, ft, want);
}

void TestDedentClauseRealign() {
    ExpectReindentBare("        else:", "py", std::string("    "));
    ExpectReindentBare("    elif x:", "py", std::string(""));
    ExpectReindentBare("    except Foo:", "py", std::string(""));
    ExpectReindentBare("    except* Foo:", "py", std::string(""));
    ExpectReindentBare("    finally:", "py", std::string(""));
    ExpectReindentBare("            else:", "pyi", std::string("        "));
    // Tab-indented clause: one tab is one level.
    ExpectReindentBare("\t\telse:", "py", std::string("\t"));
    // Already at column 0: nothing to dedent.
    ExpectReindentBare("else:", "py", std::nullopt);
    // Not a dedent clause.
    ExpectReindentBare("    return", "py", std::nullopt);
    ExpectReindentBare("    if x:", "py", std::nullopt);
    // Whole-word only: `elsewhere`/`elifetime` are names, not clauses.
    ExpectReindentBare("    elsewhere = 1", "py", std::nullopt);
    ExpectReindentBare("    exceptional = 1", "py", std::nullopt);
    // Not Python.
    ExpectReindentBare("    else:", "cpp", std::nullopt);
    ExpectReindentBare("    default:", "cpp", std::nullopt);
}

void TestDedentClauseMatchesItsOpener() {
    // The regression: a nested `if` whose body ends in a flow keyword. Enter
    // after `return 1` already dedents to the inner `if`'s own column, so the
    // `else:` typed there is *already* right -- dedenting it again (which is all
    // this used to do) paired it with the outer `if` instead.
    ExpectReindent({"if a:", "    if b:", "        return 1", "    else:"}, "py", std::nullopt);
    // Typed from the body's own column instead, it dedents to the inner `if`.
    ExpectReindent({"if a:", "    if b:", "        x = 1", "        else:"}, "py",
                   std::string("    "));
    // Three deep, same story.
    ExpectReindent({"if a:", "    if b:", "        if c:", "            return 1", "        else:"},
                   "py", std::nullopt);
    ExpectReindent(
        {"if a:", "    if b:", "        if c:", "            x = 1", "            else:"}, "py",
        std::string("        "));
    // The single-level case every editor gets right, still right.
    ExpectReindent({"if b:", "    x = 1", "    else:"}, "py", std::string(""));

    // Blank lines and comments between the body and the clause decide nothing.
    ExpectReindent({"if a:", "    if b:", "        x = 1", "", "        # why", "        else:"},
                   "py", std::string("    "));

    // Each clause matches only what can own it. A `for`/`while`/`try` can own an
    // `else`; an `elif` can not be owned by a `for`.
    ExpectReindent({"for i in xs:", "    if b:", "        break", "    else:"}, "py", std::nullopt);
    ExpectReindent({"if a:", "    for i in xs:", "        x = 1", "        else:"}, "py",
                   std::string("    "));
    ExpectReindent({"if a:", "    while b:", "        x = 1", "        else:"}, "py",
                   std::string("    "));
    ExpectReindent({"if a:", "    try:", "        return 1", "    except E:"}, "py", std::nullopt);
    ExpectReindent({"if a:", "    try:", "        x = 1", "    except E:", "        y = 2",
                    "        finally:"},
                   "py", std::string("    "));
    // `elif` skips past a `for` that cannot own it and finds the `if`.
    ExpectReindent({"if a:", "    for i in xs:", "        x = 1", "        elif b:"}, "py",
                   std::string(""));

    // A clause already sitting on its opener's column is left alone, however
    // deep the nesting goes.
    ExpectReindent({"def f():", "    if a:", "        if b:", "            x = 1", "        else:"},
                   "py", std::nullopt);
    // An opener deeper than the clause is inside the block being closed, so it
    // cannot own it: here both inner `if`s are skipped and the `else` lands on
    // the one at 4. (Column 5 only because a clause that already sits on a real
    // opener's column needs no change -- this is the mid-edit shape.)
    ExpectReindent({"if a:", "    if b:", "        if c:", "            x = 1", "     else:"}, "py",
                   std::string("    "));

    // Only ever dedents: a clause already shallower than its own opener (a
    // deliberate manual dedent, or code still being moved around) is left where
    // it is rather than pushed back in.
    ExpectReindent({"def f():", "    if b:", "        x = 1", "    else:"}, "py", std::nullopt);

    // Tabs: the opener's own indentation is what the clause takes, so a
    // tab-indented file stays tab-indented.
    ExpectReindent({"if a:", "\tif b:", "\t\tx = 1", "\t\telse:"}, "py", std::string("\t"));
}

void TestDedentCaseClause() {
    // A later `case` aligns with the one above it, not with a level count: the
    // body it closes is one level in, and typing `case` from there dedents.
    ExpectReindent({"match x:", "    case 1:", "        pass", "        case 2:"}, "py",
                   std::string("    "));
    // Typed from the `case` column already (Enter after a flow keyword in the
    // body, say), it is left alone.
    ExpectReindent({"match x:", "    case 1:", "        return 1", "    case 2:"}, "py",
                   std::nullopt);
    // The *first* case sits one level inside its `match`, so a case typed there
    // is already right -- this is the rule that keeps `match x:` Enter `case 1:`
    // from being dedented straight back out onto the `match`.
    ExpectReindent({"match x:", "    case 1:"}, "py", std::nullopt);
    ExpectReindent({"def f():", "    match x:", "        case 1:"}, "py", std::nullopt);
    // Over-indented with no sibling yet: back onto the match's own body column.
    ExpectReindent({"match x:", "        case 1:"}, "py", std::string("    "));
    ExpectReindent({"def f():", "    match x:", "            case 1:"}, "py",
                   std::string("        "));
    // A nested match: the inner one's cases belong to it, not to the outer.
    ExpectReindent({"match x:", "    case 1:", "        match y:", "            case 2:",
                    "                pass", "                case 3:"},
                   "py", std::string("            "));
    // Guards and patterns are just more of the line -- `case` is still the word
    // that decides.
    ExpectReindent({"match x:", "    case Point(y=0):", "        pass", "        case _ if z:"},
                   "py", std::string("    "));
    // Tabs: the sibling's own indentation, and the match's own plus one tab.
    ExpectReindent({"match x:", "\tcase 1:", "\t\tpass", "\t\tcase 2:"}, "py", std::string("\t"));
    ExpectReindent({"match x:", "\t\tcase 1:"}, "py", std::string("\t"));

    // `case` is a soft keyword. With no match statement open above it, a line
    // that merely starts with the word `case` is an ordinary statement -- and
    // its ':' must not dedent it. (`case = {1: 2}` and `case: int = 1` both
    // reach here, since the ':' is what fires this.)
    ExpectReindent({"def f():", "    case = {1: 2}"}, "py", std::nullopt);
    ExpectReindent({"def f():", "    case: int = 1"}, "py", std::nullopt);
    ExpectReindentBare("    case 1:", "py", std::nullopt);
    // Even inside an `if`, with no match in sight.
    ExpectReindent({"if a:", "    x = 1", "    case = {1: 2}"}, "py", std::nullopt);
    // A `match` used as a name opens nothing, so the case below it is left alone
    // rather than pulled onto a column derived from an assignment.
    ExpectReindent({"def f():", "    match = 1", "    case = {2: 3}"}, "py", std::nullopt);
    // Not Python.
    ExpectReindent({"match x:", "    case 1:", "        pass", "        case 2:"}, "cpp",
                   std::nullopt);
}

// ---- ReindentPastedText ----

void ExpectPaste(const std::string &text, const std::string &target, bool indent_first,
                 const std::string &want) {
    std::string got = mepindent::ReindentPastedText(text, target, indent_first);
    if (got != want) {
        std::fprintf(stderr,
                     "ReindentPastedText(\"%s\", \"%s\", %s) = \"%s\", want \"%s\"\n",
                     text.c_str(), target.c_str(), indent_first ? "true" : "false",
                     got.c_str(), want.c_str());
        std::abort();
    }
}

void TestPasteKeepsShape() {
    // The everyday case: a block copied at column 0 pasted into a function body.
    // Every line gains the target indent, the nesting inside it is untouched.
    ExpectPaste("if x:\n    y = 1\n", "    ", true,
                "    if x:\n        y = 1\n");
    // The same block copied *with* its own indentation: the baseline is the
    // first non-blank line's, so the result is identical -- this is the whole
    // point, pasting is insensitive to how deep the source happened to be.
    ExpectPaste("        if x:\n            y = 1\n", "    ", true,
                "    if x:\n        y = 1\n");
    // Pasting at column 0 strips the source indent rather than keeping it.
    ExpectPaste("        if x:\n            y = 1\n", "", true,
                "if x:\n    y = 1\n");
    // A line shallower than the first is floored at the target, never pulled
    // left of it (a copy that starts mid-block).
    ExpectPaste("    y = 1\nz = 2\n", "        ", true,
                "        y = 1\n        z = 2\n");
    // Single line: nothing to re-align, returned byte for byte.
    ExpectPaste("    y = 1", "        ", true, "    y = 1");
    ExpectPaste("", "    ", true, "");
    // Nothing but blank lines: no indentation to measure, so left alone.
    ExpectPaste("\n\n", "    ", true, "\n\n");
}

void TestPasteFirstLineSplice() {
    // indent_first=false: the first line is spliced onto what is already on the
    // line (an Insert-mode paste at the cursor, or a charwise p), so it is
    // stripped but not re-prefixed -- the text before the cursor is its indent.
    ExpectPaste("def f():\n    return 1\n", "    ", false,
                "def f():\n        return 1\n");
    ExpectPaste("    def f():\n        return 1\n", "    ", false,
                "def f():\n        return 1\n");
}

void TestPasteBlankLines() {
    // Interior blank lines stay empty instead of collecting the target indent.
    ExpectPaste("a = 1\n\nb = 2\n", "    ", true, "    a = 1\n\n    b = 2\n");
    ExpectPaste("a = 1\n   \nb = 2\n", "    ", true, "    a = 1\n\n    b = 2\n");
    // A trailing partial line of whitespace is where the cursor lands and
    // typing continues, so it keeps the indent it is due.
    ExpectPaste("if x:\n    ", "    ", true, "    if x:\n        ");
    // ... but a trailing *newline* is preserved as one, not padded.
    ExpectPaste("if x:\n    \n", "    ", true, "    if x:\n\n");
}

void TestPasteTabs() {
    // Tab-indented source, space-indented target: tabs are measured as four
    // columns each and re-emitted as the target's spaces.
    ExpectPaste("if x:\n\ty = 1\n", "    ", true, "    if x:\n        y = 1\n");
    ExpectPaste("\tif x:\n\t\ty = 1\n", "", true, "if x:\n    y = 1\n");
    // Space-indented source, tab-indented target: the relative levels come out
    // as tabs, so the pasted block matches the file it lands in.
    ExpectPaste("if x:\n    y = 1\n", "\t", true, "\tif x:\n\t\ty = 1\n");
    // A target that mixes the two counts as spaces -- only an all-tab indent
    // means "this file uses tabs".
    ExpectPaste("if x:\n    y = 1\n", "\t ", true, "\t if x:\n\t     y = 1\n");
    // Interior tabs (alignment inside the line) are not touched, only leading.
    ExpectPaste("a\t= 1\n    b\t= 2\n", "", true, "a\t= 1\n    b\t= 2\n");
}

// ---- IndentBackspaceWidth / IndentDeleteWidth ----

void ExpectBackspace(const std::string &line, int col, int want) {
    int got = mepindent::IndentBackspaceWidth(line, col);
    if (got != want) {
        std::fprintf(stderr, "IndentBackspaceWidth(\"%s\", %d) = %d, want %d\n",
                     line.c_str(), col, got, want);
        std::abort();
    }
}

void ExpectDelete(const std::string &line, int col, int want) {
    int got = mepindent::IndentDeleteWidth(line, col);
    if (got != want) {
        std::fprintf(stderr, "IndentDeleteWidth(\"%s\", %d) = %d, want %d\n",
                     line.c_str(), col, got, want);
        std::abort();
    }
}

void TestBackspaceEatsWholeIndent() {
    // One press per indent level, at every depth.
    ExpectBackspace("    x", 4, 4);
    ExpectBackspace("        x", 8, 4);
    ExpectBackspace("        ", 8, 4);
    // Mid-level: back to the previous tab stop, not a whole level past it.
    ExpectBackspace("      x", 6, 2);
    ExpectBackspace("     x", 5, 1);
    // Fewer spaces than a level: all of them (there is no stop to stop at).
    ExpectBackspace("  x", 2, 2);
    ExpectBackspace(" x", 1, 1);
    // A literal tab is already a single press -- left to the ordinary path.
    ExpectBackspace("\tx", 1, 0);
    ExpectBackspace("\t    x", 5, 4);  // spaces after a tab: still one level
    // Text before the cursor means ordinary Backspace: the spaces between
    // words are not indentation.
    ExpectBackspace("x    y", 5, 0);
    ExpectBackspace("    x ", 6, 0);
    ExpectBackspace("    xy", 6, 0);
    // Start of line, and a column past the end: nothing special.
    ExpectBackspace("    x", 0, 0);
    ExpectBackspace("    x", 99, 0);
    ExpectBackspace("", 0, 0);
}

void TestDeleteEatsWholeIndent() {
    // Forward Delete from column 0 takes the whole first level.
    ExpectDelete("    x", 0, 4);
    ExpectDelete("        x", 0, 4);
    ExpectDelete("        x", 4, 4);
    // Up to the next stop only.
    ExpectDelete("        x", 2, 2);
    ExpectDelete("      x", 4, 2);
    // A short indent: the spaces that are there, stopping at the text.
    ExpectDelete("  x", 0, 2);
    // On a tab, or on text: ordinary Delete.
    ExpectDelete("\tx", 0, 0);
    ExpectDelete("    x", 4, 0);
    // Past the indent (text before the cursor): ordinary Delete, so a space
    // between words still goes one at a time.
    ExpectDelete("x    y", 1, 0);
    ExpectDelete("", 0, 0);
}

// ---- PasteSpliceCol ----

void ExpectSplice(const std::string &line, int col, int want) {
    int got = mepindent::PasteSpliceCol(line, col);
    if (got != want) {
        std::fprintf(stderr, "PasteSpliceCol(\"%s\", %d) = %d, want %d\n", line.c_str(), col,
                     got, want);
        std::abort();
    }
}

void TestPasteSpliceColLeavesTheIndent() {
    // The regression this exists for: a cursor inside a line's indent -- column
    // 0 of an indented line above all, which is where a mouse click, `0`, or
    // arrowing down a column leaves it. `p` splices after the cursor's own
    // character, i.e. at column 1, and the block used to arrive one space in
    // from the margin with every line under it at the line's real indent.
    ExpectSplice("    x = 1", 1, 4);
    ExpectSplice("    x = 1", 0, 4);
    ExpectSplice("    x = 1", 2, 4);
    ExpectSplice("    x = 1", 3, 4);
    // At the end of the indent, or anywhere in the text, the caller's own
    // column is already right and is returned untouched.
    ExpectSplice("    x = 1", 4, 4);
    ExpectSplice("    x = 1", 5, 5);
    ExpectSplice("    x = 1", 9, 9);
    // A blank-but-indented line -- the other everyday paste target -- lands at
    // its own indent rather than one space into it.
    ExpectSplice("        ", 1, 8);
    ExpectSplice("        ", 8, 8);
    // Nothing to leave: an unindented line, an empty one, a tab indent.
    ExpectSplice("x = 1", 0, 0);
    ExpectSplice("x = 1", 3, 3);
    ExpectSplice("", 0, 0);
    ExpectSplice("\t\tx", 1, 2);
    // A negative column (nothing should ever pass one) clamps rather than
    // indexing backwards.
    ExpectSplice("    x", -1, 0);
}

}  // namespace

int main() {
    TestInheritAllFiletypes();
    TestPythonColonOpensBlock();
    TestPythonFlowKeywordDedents();
    TestDedentClauseRealign();
    TestDedentClauseMatchesItsOpener();
    TestDedentCaseClause();
    TestPasteKeepsShape();
    TestPasteFirstLineSplice();
    TestPasteBlankLines();
    TestPasteTabs();
    TestPasteSpliceColLeavesTheIndent();
    TestBackspaceEatsWholeIndent();
    TestDeleteEatsWholeIndent();
    std::printf("indent tests passed\n");
    return 0;
}
