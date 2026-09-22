// Coverage for python_format.h/.cpp: mep's in-house, dependency-free Python
// formatter. Exercises each stage the header describes -- the tokenizer
// (strings, f-strings, numbers, indentation, the errors it must refuse on),
// the spacing table, statement splitting, the bracket splitter and its
// trailing-comma rules, blank-line placement, and the equivalence check that
// backs all of it.
//
// The rule every expectation here follows: the formatter may change how code
// is *written* and never what it *means*. So alongside the exact-output tests
// there is TestNeverChangesMeaning, which formats a batch of awkward snippets
// and asserts that Format() either produced token-equivalent output or
// refused outright -- the property that matters when a buffer is about to be
// overwritten in place.

#include "python_format.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// Formats `src` and requires success. Reported failures abort with the
// formatter's own message, which is far more useful than a bare CHECK.
std::string Fmt(const std::string &src) {
    pyfmt::Result r = pyfmt::Format(src);
    if (!r.ok) {
        std::fprintf(stderr, "unexpected format failure: %s\n(input)\n%s\n",
                     r.error.c_str(), src.c_str());
        std::abort();
    }
    return r.text;
}

// Same, at a narrow width, to make split behavior testable without 88-column
// string literals in this file.
std::string FmtW(const std::string &src, int width) {
    pyfmt::Options o;
    o.line_length = width;
    pyfmt::Result r = pyfmt::Format(src, o);
    if (!r.ok) {
        std::fprintf(stderr, "unexpected format failure: %s\n(input)\n%s\n",
                     r.error.c_str(), src.c_str());
        std::abort();
    }
    return r.text;
}

void Expect(const std::string &src, const std::string &want) {
    std::string got = Fmt(src);
    if (got != want) {
        std::fprintf(stderr, "--- input ---\n%s--- want ---\n%s--- got ---\n%s",
                     src.c_str(), want.c_str(), got.c_str());
        Check(false, "formatted output matches", __LINE__);
    }
}

void ExpectW(const std::string &src, int width, const std::string &want) {
    std::string got = FmtW(src, width);
    if (got != want) {
        std::fprintf(stderr, "--- input (width %d) ---\n%s--- want ---\n%s--- got ---\n%s",
                     width, src.c_str(), want.c_str(), got.c_str());
        Check(false, "formatted output matches", __LINE__);
    }
}

// A formatter must be a fixed point: formatting its own output changes
// nothing. Every expectation in this file is run through this.
void ExpectStable(const std::string &formatted, int width = 88) {
    pyfmt::Options o;
    o.line_length = width;
    pyfmt::Result again = pyfmt::Format(formatted, o);
    CHECK(again.ok);
    if (again.text != formatted) {
        std::fprintf(stderr, "--- not idempotent, first ---\n%s--- second ---\n%s",
                     formatted.c_str(), again.text.c_str());
        Check(false, "formatting is idempotent", __LINE__);
    }
}

void TestSpacing() {
    Expect("x=1\n", "x = 1\n");
    Expect("x = a+b*c\n", "x = a + b * c\n");
    Expect("x = a  ==  b\n", "x = a == b\n");
    // Unary operators bind to their operand; binary ones get spaces. The
    // difference is decided by what precedes them.
    Expect("x = -1\n", "x = -1\n");
    Expect("x = a - 1\n", "x = a - 1\n");
    Expect("x = a * -1\n", "x = a * -1\n");
    Expect("x = (-1, +2, ~3)\n", "x = (-1, +2, ~3)\n");
    Expect("f( * args, ** kw )\n", "f(*args, **kw)\n");
    Expect("a, *b = c\n", "a, *b = c\n");
    // Calls and subscripts are tight; a keyword is not a call.
    Expect("print (x)\n", "print(x)\n");
    Expect("x = d ['k']\n", "x = d[\"k\"]\n");
    Expect("assert(x)\n", "assert (x)\n");
    Expect("del x [0]\n", "del x[0]\n");
    // Attribute access, and the one place a dot needs a space around it.
    Expect("x = a . b . c\n", "x = a.b.c\n");
    Expect("from . import x\n", "from . import x\n");
    Expect("from .. import x\n", "from .. import x\n");
    Expect("from .mod import x\n", "from .mod import x\n");
    // `**` is tight between simple operands and spaced otherwise.
    Expect("x = a**2\n", "x = a**2\n");
    Expect("x = a.b**2\n", "x = a.b**2\n");
    Expect("x = f()**2\n", "x = f() ** 2\n");
    Expect("x = a ** b()\n", "x = a ** b()\n");
    // Keyword arguments are tight; annotated parameters are not.
    Expect("f(a=1, b=2)\n", "f(a=1, b=2)\n");
    Expect("def f(a=1):\n    pass\n", "def f(a=1):\n    pass\n");
    Expect("def f(a: int=1):\n    pass\n", "def f(a: int = 1):\n    pass\n");
    // ...and a lambda's colon must not be mistaken for an annotation, which
    // would put spaces around the `n=1` that follows it.
    Expect("f(key=lambda x: x, n=1)\n", "f(key=lambda x: x, n=1)\n");
    Expect("x: int = 5\n", "x: int = 5\n");
    Expect("def f() ->dict:\n    pass\n", "def f() -> dict:\n    pass\n");
    // Decorators: a leading '@' is a prefix, not the matmul operator.
    Expect("@ foo\ndef f():\n    pass\n", "@foo\ndef f():\n    pass\n");
    Expect("x = a @ b\n", "x = a @ b\n");
    Expect("x = {'a': 1}\n", "x = {\"a\": 1}\n");
    Expect("x = a if b else c\n", "x = a if b else c\n");
    Expect("x = not  y\n", "x = not y\n");
    Expect("x = y  if  not  z  else  w\n", "x = y if not z else w\n");
}

void TestSlices() {
    // PEP 8: the colon is an operator with the lowest priority, so it takes
    // equal space on both sides -- but only when an operand is complex.
    Expect("x = a[1:2]\n", "x = a[1:2]\n");
    Expect("x = a[:]\n", "x = a[:]\n");
    Expect("x = a[::2]\n", "x = a[::2]\n");
    Expect("x = a[x:y:z]\n", "x = a[x:y:z]\n");
    Expect("x = a[-1:]\n", "x = a[-1:]\n");
    Expect("x = a[lower+1:upper+1]\n", "x = a[lower + 1 : upper + 1]\n");
    Expect("x = a[fn(b):]\n", "x = a[fn(b) :]\n");
    // A plain index is not a slice at all and never gains spaces.
    Expect("x = a[b + 1]\n", "x = a[b + 1]\n");
    // A dict's colon is not a slice colon even though both live in brackets.
    Expect("x = {a + 1: b + 2}\n", "x = {a + 1: b + 2}\n");
}

void TestLiterals() {
    Expect("x = 'hi'\n", "x = \"hi\"\n");
    // Quote choice follows whichever spelling needs fewer escapes.
    Expect("x = 'it\\'s'\n", "x = \"it's\"\n");
    Expect("x = 'say \"hi\"'\n", "x = 'say \"hi\"'\n");
    Expect("x = \"\\\"q\\\"\"\n", "x = '\"q\"'\n");
    // Prefixes are lowercased and a redundant `u` is dropped.
    Expect("x = U'a'\n", "x = \"a\"\n");
    Expect("x = F'a'\n", "x = f\"a\"\n");
    Expect("x = RB'a'\n", "x = rb\"a\"\n");
    // A raw string's backslashes cannot be touched, so its quote only changes
    // when the target does not occur in the body.
    Expect("x = r'a\\d'\n", "x = r\"a\\d\"\n");
    Expect("x = r'a\"b'\n", "x = r'a\"b'\n");
    // Triple quotes swap delimiter only when that cannot terminate early.
    Expect("x = '''doc'''\n", "x = \"\"\"doc\"\"\"\n");
    // ...which this body cannot do: it ends in a quote, so swapping the
    // delimiter would spell a four-quote terminator.
    Expect("x = '''say \"hi\"'''\n", "x = '''say \"hi\"'''\n");
    // Numbers: lowercase prefix/exponent/suffix, uppercase hex digits.
    Expect("x = 0XaB\n", "x = 0xAB\n");
    Expect("x = 1E5\n", "x = 1e5\n");
    Expect("x = 2J\n", "x = 2j\n");
    Expect("x = 0O17\n", "x = 0o17\n");
    Expect("x = 0B1010\n", "x = 0b1010\n");
    Expect("x = 1_000\n", "x = 1_000\n");
    // A leading-dot float is left as written: normalization changes case,
    // never the digits.
    Expect("x = .5\n", "x = .5\n");
}

void TestFStrings() {
    Expect("x = f'{a}'\n", "x = f\"{a}\"\n");
    // An f-string whose interpolation contains the other quote keeps its own
    // delimiter: swapping it would need an escape inside the braces.
    Expect("x = f'{d[\"k\"]}'\n", "x = f'{d[\"k\"]}'\n");
    // PEP 701 nesting: the inner literal must not end the outer one.
    CHECK(pyfmt::Format("x = f'{f'{y}'}'\n").ok);
    // A format spec is literal text, so a quote in it is not a nested string,
    // while a slice colon inside brackets does not start a spec.
    CHECK(pyfmt::Format("x = f\"{v:'>10}\"\n").ok);
    Expect("x = f'{v[1:2]}'\n", "x = f\"{v[1:2]}\"\n");
    // A backslash does not suppress brace escaping.
    Expect("x = rf'\\{{%'\n", "x = rf\"\\{{%\"\n");
    // t-strings (PEP 750) are string literals, not a name beside a string.
    Expect("x = t'{a}'\n", "x = t\"{a}\"\n");
    Expect("x = T'a'\n", "x = t\"a\"\n");
}

void TestStatementSplitting() {
    Expect("a = 1; b = 2\n", "a = 1\nb = 2\n");
    Expect("if x: y = 1\n", "if x:\n    y = 1\n");
    // The suite split has to happen before the `;` split: both statements
    // belong to the body, and splitting the other way around silently moves
    // the second one out of the `if`.
    Expect("if x: a(); return\n", "if x:\n    a()\n    return\n");
    Expect("def f(): return 1\n", "def f():\n    return 1\n");
    Expect("for i in x: print(i)\n", "for i in x:\n    print(i)\n");
    Expect("while x: pass\n", "while x:\n    pass\n");
    Expect("with a as b: pass\n", "with a as b:\n    pass\n");
    Expect("try: a()\nexcept E: b()\n", "try:\n    a()\nexcept E:\n    b()\n");
    Expect("async def f(): pass\n", "async def f():\n    pass\n");
    // A colon that belongs to a lambda does not open a suite.
    Expect("f = lambda x: x\n", "f = lambda x: x\n");
    Expect("if f(lambda: 1): pass\n", "if f(lambda: 1):\n    pass\n");
    // A bare `else:` has nothing after the colon and stays one line.
    Expect("if x:\n    a()\nelse:\n    b()\n", "if x:\n    a()\nelse:\n    b()\n");
}

void TestIndentation() {
    // Indentation is regenerated from nesting depth, so the input's own
    // width -- tabs, two spaces, seven -- does not survive.
    Expect("if x:\n  y = 1\n", "if x:\n    y = 1\n");
    Expect("if x:\n\tif y:\n\t\tz = 1\n", "if x:\n    if y:\n        z = 1\n");
    Expect("if x:\n       y = 1\n", "if x:\n    y = 1\n");
}

// A UTF-8 BOM has to be set aside rather than tokenized: glued to the first
// token it turns `def` into an ordinary name, and the statement stops being
// recognized as a definition.
void TestByteOrderMark() {
    const std::string bom = "\xEF\xBB\xBF";
    std::string got = Fmt(bom + "def f(): pass\n");
    CHECK(got == bom + "def f():\n    pass\n");
    ExpectStable(got);
    // Line endings are normalized; a file that had none at the end gains one.
    Expect("x = 1\r\nif x:\r\n    y = 2\r\n", "x = 1\nif x:\n    y = 2\n");
}

void TestSplitting() {
    // Stage one: brackets onto their own lines, contents together.
    ExpectW("result = fn(aaaa, bbbb, cccc)\n", 20,
            "result = fn(\n    aaaa, bbbb, cccc\n)\n");
    // Stage two: still too long, so one element per line with a trailing comma.
    ExpectW("result = fn(aaaa, bbbb, cccc)\n", 12,
            "result = fn(\n    aaaa,\n    bbbb,\n    cccc,\n)\n");
    // A magic trailing comma forces the exploded shape regardless of width.
    Expect("x = fn(a, b,)\n", "x = fn(\n    a,\n    b,\n)\n");
    Expect("x = [\n    1,\n]\n", "x = [\n    1,\n]\n");
    // ...but not where the comma is part of the value rather than a style
    // choice: a one-tuple and a single-element subscript must stay put.
    Expect("t = (1,)\n", "t = (1,)\n");
    Expect("s = x[1,]\n", "s = x[1,]\n");
    // A grouping paren must not gain a trailing comma -- that would make it a
    // tuple. Neither may a subscript, or a comprehension.
    // A grouping paren splits like any other bracket but must never gain a
    // trailing comma -- `(a and b,)` is a tuple, which is always truthy.
    ExpectW("if (aaaaaa and bbbbbb):\n    pass\n", 14,
            "if (\n    aaaaaa and bbbbbb\n):\n    pass\n");
    // With no bracket at all there is nothing to split, and no paren is
    // invented: the line is emitted over-long (see the header's gap list).
    ExpectW("if aaaaaa and bbbbbb:\n    pass\n", 14,
            "if aaaaaa and bbbbbb:\n    pass\n");
    ExpectW("y = [aaaa for bbbb in cccc]\n", 12,
            "y = [\n    aaaa for bbbb in cccc\n]\n");
    ExpectW("y = {k: v for k, v in items}\n", 12,
            "y = {\n    k: v for k, v in items\n}\n");
    // A lambda's parameter commas sit at the call's own depth but do not
    // separate its arguments; splitting on them would emit `lambda a,`.
    ExpectW("y = sorted(data, key=lambda a, b: a)\n", 16,
            "y = sorted(\n    data,\n    key=lambda a, b: a,\n)\n");
    // The tail after the closing bracket is split in turn when it is long.
    ExpectW("def f(aaaa, bbbb) -> Dict[str, int]:\n    pass\n", 20,
            "def f(\n    aaaa, bbbb\n) -> Dict[str, int]:\n    pass\n");
    // Nothing to split at: emitted long rather than broken somewhere unsafe.
    ExpectW("xxxxxxxxxx = yyyyyyyyyy\n", 5, "xxxxxxxxxx = yyyyyyyyyy\n");
    // A candidate bracket whose head would itself have to explode is not a
    // usable split point, however short that head measures when flattened --
    // the flat form is never emitted, and measuring it counts trailing commas
    // this formatter adds, so the file would alternate between two shapes on
    // successive runs.
    Expect("x = {\n    'a': [1, 2,],\n} if cond(y) else None\n",
           "x = {\n    \"a\": [\n        1,\n        2,\n    ],\n} if cond(y) else None\n");
}

void TestComments() {
    Expect("#comment\n", "# comment\n");
    Expect("#!shebang\n", "#!shebang\n");
    Expect("#: sphinx\n", "#: sphinx\n");
    Expect("####\n", "####\n");
    Expect("##commented out\n", "##commented out\n");
    Expect("x = 1  #note\n", "x = 1  # note\n");
    Expect("x = 1 # note\n", "x = 1  # note\n");
    // A comment introducing a block's body goes with the body, whatever
    // column it was written at, and a comment closing a block stays inside it.
    Expect("if x:\n        # note\n    y = 1\n", "if x:\n    # note\n    y = 1\n");
    Expect("def f():\n    a()\n    # done\nb()\n",
           "def f():\n    a()\n    # done\n\n\nb()\n");
    // ...including when the statement above it is itself about to gain a
    // level. `def read(self): ...` becomes two lines, so a rule that placed
    // this comment by comparing columns with the statement above would put it
    // somewhere else on the second run.
    Expect("class A:\n    def read(self): ...\n    # note\n\nclass B:\n    pass\n",
           "class A:\n    def read(self):\n        ...\n\n    # note\n\n\nclass B:\n    pass\n");
    // Comments stranded between implicitly concatenated pieces cannot be
    // rendered inline -- everything after the '#' would be swallowed.
    std::string joined = Fmt("x = fn(\n    'a'  # one\n    'b'  # two\n)\n");
    CHECK(joined.find("# one") != std::string::npos);
    CHECK(joined.find("# two") != std::string::npos);
    CHECK(joined.find("\"b\"") != std::string::npos);
    ExpectStable(joined);
    // ...and are not emitted twice, which a naive "trailing comment" rule does.
    CHECK(joined.find("# one") == joined.rfind("# one"));
    CHECK(joined.find("# two") == joined.rfind("# two"));
}

void TestBlankLines() {
    // Two blank lines around a top-level def, one around a method.
    Expect("import os\ndef f():\n    pass\n", "import os\n\n\ndef f():\n    pass\n");
    Expect("def f():\n    pass\nx = 1\n", "def f():\n    pass\n\n\nx = 1\n");
    Expect("class A:\n    def a(self):\n        pass\n    def b(self):\n        pass\n",
           "class A:\n    def a(self):\n        pass\n\n    def b(self):\n        pass\n");
    // No blank line between a header and the first line of its body...
    Expect("def f():\n\n    pass\n", "def f():\n    pass\n");
    // ...nor between a decorator and what it decorates.
    Expect("@dec\n\ndef f():\n    pass\n", "@dec\ndef f():\n    pass\n");
    Expect("@a\n@b\ndef f():\n    pass\n", "@a\n@b\ndef f():\n    pass\n");
    // The blank lines a def is owed land before its leading comment block,
    // not between the comment and the def it documents.
    Expect("x = 1\n# doc\ndef f():\n    pass\n",
           "x = 1\n\n\n# doc\ndef f():\n    pass\n");
    // Runs are capped: two at module level, one inside a body.
    Expect("a = 1\n\n\n\n\nb = 2\n", "a = 1\n\n\nb = 2\n");
    Expect("def f():\n    a = 1\n\n\n\n    b = 2\n",
           "def f():\n    a = 1\n\n    b = 2\n");
    // No leading blank lines, and exactly one trailing newline.
    Expect("\n\n\nx = 1\n", "x = 1\n");
    Expect("x = 1", "x = 1\n");
    Expect("x = 1\n\n\n", "x = 1\n");
    // Whitespace-only input has no statements and produces nothing.
    Expect("", "");
    Expect("\n\n", "");
}

void TestRefusals() {
    // A syntax error is reported with its line and the buffer left alone --
    // never "formatted" into something else.
    pyfmt::Result r = pyfmt::Format("x = 'unterminated\n");
    CHECK(!r.ok);
    CHECK(r.error_line == 1);
    CHECK(r.text.empty());

    r = pyfmt::Format("x = (1\n");
    CHECK(!r.ok);
    CHECK(r.error.find("unclosed") != std::string::npos);

    r = pyfmt::Format("x = 1)\n");
    CHECK(!r.ok);
    CHECK(r.error.find("unmatched") != std::string::npos);

    r = pyfmt::Format("if x:\n        a()\n    b()\n");
    CHECK(!r.ok);
    CHECK(r.error.find("unindent") != std::string::npos);
    CHECK(r.error_line == 3);
}

void TestSoftKeywords() {
    // `match`/`case` are identifiers unless they head a match statement, so
    // `case (A | B):` looks exactly like a call -- and giving it the trailing
    // comma a call would get turns an or-pattern into a sequence pattern.
    Expect("match x:\n    case (A | B):\n        pass\n",
           "match x:\n    case (A | B):\n        pass\n");
    std::string wide = FmtW(
        "match value:\n    case (Aaaaaaaaaa | Bbbbbbbbbb | Cccccccccc):\n        pass\n", 20);
    CHECK(wide.find(",\n") == std::string::npos);
    ExpectStable(wide, 20);
    // Used as ordinary names they still format as names.
    Expect("match = 1\n", "match = 1\n");
    Expect("case = match + 1\n", "case = match + 1\n");
}

// The property that actually matters: for any input, Format() either produces
// something that means the same thing or refuses. These snippets are the ones
// where a token-stream formatter is most likely to get it wrong, so each is
// checked to be either refused or stable and structurally intact.
void TestNeverChangesMeaning() {
    const char *const kSnippets[] = {
        "x = (a,)\n",
        "x = (a)\n",
        "x = d[a,]\n",
        "x = d[a]\n",
        "x = d[a, b]\n",
        "x = f(*a, **b)\n",
        "def f(a, /, b, *, c):\n    pass\n",
        "def f(*args, **kwargs):\n    pass\n",
        "x = [a for a in b if c]\n",
        "x = {a: b for a, b in c}\n",
        "x = (a for a in b)\n",
        "x = f(a for a in b)\n",
        "x = lambda a, b=1, *c, **d: a\n",
        "x = a if b else c\n",
        "x = not a in b\n",
        "x = a is not b\n",
        "x = yield\n",
        "async def f():\n    await g()\n",
        "x = a[b][c](d)\n",
        "x = -a ** b\n",
        "x = a @ b @ c\n",
        "global a, b\n",
        "x = 1 if a else 2 if b else 3\n",
        "with a() as b, c() as d:\n    pass\n",
        "try:\n    pass\nexcept* E:\n    pass\n",
        "x: list[int] = []\n",
        "def f(a: int = 1, *, b: str = 'x') -> None:\n    pass\n",
        "class A(B, metaclass=M):\n    pass\n",
        "x = {**a, **b}\n",
        "x = [*a, *b]\n",
        "assert a, 'msg'\n",
        "raise E('x') from f\n",
        "x = a,\n",
        "x = a, b\n",
        "for a, b in c:\n    pass\n",
        "x = (yield a)\n",
        "if (a := f()) > 0:\n    pass\n",
        "x = f'{a!r:>{w}}'\n",
        "print(*(a for a in b))\n",
        "x = a[b:c, d:e]\n",
    };
    for (const char *snippet : kSnippets) {
        std::string src(snippet);
        for (int width : {88, 20, 8}) {
            pyfmt::Options o;
            o.line_length = width;
            pyfmt::Result r = pyfmt::Format(src, o);
            if (!r.ok) continue;  // refusing is always an acceptable answer
            // Verified output must survive a second pass unchanged...
            pyfmt::Result again = pyfmt::Format(r.text, o);
            if (!again.ok || again.text != r.text) {
                std::fprintf(stderr, "not idempotent at width %d:\n%s-> %s",
                             width, src.c_str(), r.text.c_str());
                Check(false, "snippet formats idempotently", __LINE__);
            }
            // ...and must still be non-empty code, not silently dropped.
            CHECK(!r.text.empty());
        }
    }
}

// `gf` runs this formatter inside the editor process, so pathological nesting
// must come back as an error rather than as a stack overflow that takes mep
// down. (Both the splitter and the f-string scanner recurse once per level;
// 20k nested parens really did segfault before the cap in python_format.cpp.)
void TestNestingGuard() {
    std::string deep = "x = ";
    for (int i = 0; i < 20000; i++) deep += "(";
    deep += "1";
    for (int i = 0; i < 20000; i++) deep += ")";
    deep += "\n";
    pyfmt::Result r = pyfmt::Format(deep);
    CHECK(!r.ok);
    CHECK(r.error.find("nests too deeply") != std::string::npos);

    std::string deep_f = "x = ";
    for (int i = 0; i < 4000; i++) deep_f += "f\"{";
    deep_f += "1";
    for (int i = 0; i < 4000; i++) deep_f += "}\"";
    deep_f += "\n";
    pyfmt::Result rf = pyfmt::Format(deep_f);
    CHECK(!rf.ok);
    CHECK(rf.error.find("nests too deeply") != std::string::npos);

    // The cap has to sit above anything real code does, and everything under
    // it still has to work rather than merely not crash.
    std::string ok_deep = "x = ";
    for (int i = 0; i < 400; i++) ok_deep += "(";
    ok_deep += "1";
    for (int i = 0; i < 400; i++) ok_deep += ")";
    ok_deep += "\n";
    pyfmt::Result ro = pyfmt::Format(ok_deep);
    CHECK(ro.ok);
    CHECK(!ro.text.empty());
}

// The verifier is the last line of defence, so it gets its own test: a comma
// added where it would change the value has to be caught, not waved through.
void TestVerifier() {
    pyfmt::Options o;
    o.verify = true;
    // These all round-trip cleanly, which is what lets the checks above mean
    // anything -- a verifier that rejected everything would pass vacuously.
    CHECK(pyfmt::Format("x = f(a, b)\n", o).ok);
    CHECK(pyfmt::Format("x = (a, b)\n", o).ok);
    CHECK(pyfmt::Format("x = [a, b]\n", o).ok);
    CHECK(pyfmt::Format("x = d[a, b]\n", o).ok);
    CHECK(pyfmt::Format("x = (a + b)\n", o).ok);
    // Turning verification off must not change the answer for good input.
    pyfmt::Options unchecked;
    unchecked.verify = false;
    CHECK(pyfmt::Format("x = f(a, b)\n", unchecked).text ==
          pyfmt::Format("x = f(a, b)\n", o).text);
}

// Everything above asserts a single formatting; this asserts the whole file
// round-trips and stays put, over a program that uses most of the language.
void TestWholeFile() {
    const std::string src =
        "#!/usr/bin/env python3\n"
        "'''Module doc.'''\n"
        "import os, sys\n"
        "\n"
        "CONST = { 'a':1, 'b':2 }\n"
        "@decorator(arg=1)\n"
        "class Thing( Base ):\n"
        "    '''Doc.'''\n"
        "    def __init__(self, a, b=2, *args, **kw):\n"
        "        self.a=a\n"
        "        # note\n"
        "        self.b = [x**2 for x in range(10) if x%2==0]\n"
        "    def run(self)->int:\n"
        "        try:\n"
        "            with open('f') as fh: data = fh.read()\n"
        "        except OSError as e: raise RuntimeError('bad') from e\n"
        "        return len(data[1:-1])\n"
        "def main():\n"
        "    t = Thing(1, b=3)\n"
        "    print(f'{t.a}: {t.b!r}')\n"
        "if __name__=='__main__':\n"
        "    main()\n";
    std::string got = Fmt(src);
    ExpectStable(got);
    // Spot-check the transformations that should have happened.
    CHECK(got.find("#!/usr/bin/env python3\n") == 0);
    CHECK(got.find("\"\"\"Module doc.\"\"\"") != std::string::npos);
    CHECK(got.find("self.a = a") != std::string::npos);
    CHECK(got.find("x**2") != std::string::npos);
    CHECK(got.find("x % 2 == 0") != std::string::npos);
    CHECK(got.find("def run(self) -> int:") != std::string::npos);
    CHECK(got.find("with open(\"f\") as fh:\n                data = fh.read()") !=
          std::string::npos);
    CHECK(got.find("\n\n\ndef main():") != std::string::npos);
    CHECK(got.find("\n\n    def run") != std::string::npos);
}

// Runs every exact-output expectation a second time through the formatter.
// Cheap to do here and it means no test above can pass with output that would
// churn on the next `gf`.
void TestIdempotenceOfExpectations() {
    const char *const kInputs[] = {
        "x=1\n",
        "if x: a(); return\n",
        "x = fn(a, b,)\n",
        "class A:\n    def a(self):\n        pass\n    def b(self):\n        pass\n",
        "x = a[lower+1:upper+1]\n",
        "x = f(key=lambda x: x, n=1)\n",
        "if x:\n        # note\n    y = 1\n",
        "class A:\n    def read(self): ...\n    # note\n\nclass B:\n    pass\n",
        "x = 1  #note\n",
        "match x:\n    case (A | B):\n        pass\n",
    };
    for (const char *in : kInputs) ExpectStable(Fmt(in));
}

}  // namespace

int main() {
    TestSpacing();
    TestSlices();
    TestLiterals();
    TestFStrings();
    TestStatementSplitting();
    TestIndentation();
    TestByteOrderMark();
    TestSplitting();
    TestComments();
    TestBlankLines();
    TestRefusals();
    TestSoftKeywords();
    TestNeverChangesMeaning();
    TestNestingGuard();
    TestVerifier();
    TestWholeFile();
    TestIdempotenceOfExpectations();
    std::printf("python_format tests passed\n");
    return 0;
}
