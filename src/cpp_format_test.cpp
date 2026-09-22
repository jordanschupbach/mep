// Coverage for cpp_format.h/.cpp: mep's in-house, dependency-free C++
// formatter. Exercises each stage the header describes -- the lexer (raw
// strings, ud-suffixes, pp-numbers, the line splices it has to refuse on),
// the spacing table and the pointer/template/brace guesses that feed it,
// unwrapped-line splitting at `;`/`{`/`}`/labels, indentation of namespaces,
// class bodies and switch labels, the bracket/operator/assignment splitter,
// preprocessor handling, comments, blank lines, and the token-equivalence
// check that backs all of it.
//
// The rule every expectation here follows: the formatter may change how code
// is *written* and never what it *means*. So alongside the exact-output tests
// there is TestNeverChangesMeaning, which formats a batch of awkward snippets
// and asserts that Format() either produced token-equivalent output or
// refused outright -- the property that matters when a buffer is about to be
// overwritten in place.

#include "cpp_format.h"

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

cppfmt::Options Opts(int width) {
    cppfmt::Options o;
    o.column_limit = width;
    return o;
}

// Formats `src` and requires success. Reported failures abort with the
// formatter's own message, which is far more useful than a bare CHECK.
std::string FmtW(const std::string &src, int width) {
    cppfmt::Result r = cppfmt::Format(src, Opts(width));
    if (!r.ok) {
        std::fprintf(stderr, "unexpected format failure: %s\n(input)\n%s\n", r.error.c_str(),
                     src.c_str());
        std::abort();
    }
    return r.text;
}

// A formatter must be a fixed point: formatting its own output changes
// nothing. Every expectation in this file is run through this, at the same
// width as the first pass -- a narrow-width case looks non-idempotent if the
// second pass is allowed a wider one.
void ExpectStable(const std::string &formatted, int width) {
    cppfmt::Result again = cppfmt::Format(formatted, Opts(width));
    CHECK(again.ok);
    if (again.text != formatted) {
        std::fprintf(stderr, "--- not idempotent, first ---\n%s--- second ---\n%s",
                     formatted.c_str(), again.text.c_str());
        Check(false, "formatting is idempotent", __LINE__);
    }
}

void ExpectW(const std::string &src, int width, const std::string &want) {
    std::string got = FmtW(src, width);
    if (got != want) {
        std::fprintf(stderr, "--- input (width %d) ---\n%s--- want ---\n%s--- got ---\n%s", width,
                     src.c_str(), want.c_str(), got.c_str());
        Check(false, "formatted output matches", __LINE__);
    }
    ExpectStable(got, width);
}

void Expect(const std::string &src, const std::string &want) { ExpectW(src, 100, want); }

// ---------------------------------------------------------------------------

void TestSpacing() {
    Expect("int x=1;\n", "int x = 1;\n");
    Expect("x = a+b*c;\n", "x = a + b * c;\n");
    Expect("x   =   a  ==  b;\n", "x = a == b;\n");
    // Unary operators bind to their operand; binary ones get spaces, and
    // which is which is decided by what precedes them.
    Expect("x = -1;\n", "x = -1;\n");
    Expect("x = a - 1;\n", "x = a - 1;\n");
    Expect("x = a * -1;\n", "x = a * -1;\n");
    Expect("x = !a && ~b;\n", "x = !a && ~b;\n");
    Expect("i++; ++i; --i; i--;\n", "i++;\n++i;\n--i;\ni--;\n");
    // A call is tight, a control statement is not (SpaceBeforeParens:
    // ControlStatements), and `sizeof` is spelled like a call.
    Expect("foo (1,2);\n", "foo(1, 2);\n");
    Expect("if(x){y();}\n", "if (x) { y(); }\n");
    Expect("if(x)\n{\ny();\n}\n", "if (x) {\n    y();\n}\n");
    Expect("x = sizeof (int);\n", "x = sizeof(int);\n");
    Expect("return (a);\n", "return (a);\n");
    // Member access, scope resolution and subscripts never take a space.
    Expect("x = a . b -> c :: d;\n", "x = a.b->c::d;\n");
    Expect("x = v [ i ] [ j ];\n", "x = v[i][j];\n");
    Expect("x = & a;\n", "x = &a;\n");
    // Pointers and references hug the name (PointerAlignment: Right).
    Expect("int * p;\n", "int *p;\n");
    Expect("char * * q;\n", "char **q;\n");
    Expect("const char * const r = s;\n", "const char *const r = s;\n");
    Expect("void f(const std::string & s, int & n);\n",
           "void f(const std::string &s, int &n);\n");
    Expect("auto && x = y;\n", "auto &&x = y;\n");
    // ... but the same tokens between two operands are binary operators.
    Expect("x = a * b;\n", "x = a * b;\n");
    Expect("x = a & b;\n", "x = a & b;\n");
    Expect("void g() { f(a * b, c & d); }\n", "void g() { f(a * b, c & d); }\n");
    // At file scope the very same tokens really are a declaration -- C++'s
    // most vexing parse -- and come out spaced as one.
    Expect("f(a * b, c & d);\n", "f(a *b, c &d);\n");
}

void TestTemplatesAndOperators() {
    Expect("std::vector < int > v;\n", "std::vector<int> v;\n");
    Expect("template < typename T > void f(T t);\n", "template <typename T> void f(T t);\n");
    Expect("template <typename T>\nvoid f(T t);\n", "template <typename T>\nvoid f(T t);\n");
    Expect("x = std::map<int, std::vector<int>>();\n", "x = std::map<int, std::vector<int>>();\n");
    // A `>` from the source's own `> >` stays two tokens: merging them is a
    // legal change under C++11 and a silent one under C++03, and this never
    // makes silent changes.
    Expect("A<B<C> > x;\n", "A<B<C> > x;\n");
    // The shape that fools every token-level formatter that does not look for
    // it: a comparison chain that contains a plausible `<` ... `>` pair.
    Expect("if (a < b && c > d) {}\n", "if (a < b && c > d) {}\n");
    Expect("x = a < b;\n", "x = a < b;\n");
    // An `operator@` name is spelling, not structure.
    Expect("bool operator == (const T & o) const;\n", "bool operator==(const T &o) const;\n");
    Expect("T & operator [] (int i);\n", "T &operator[](int i);\n");
    Expect("void operator () ();\n", "void operator()();\n");
    Expect("operator bool () const;\n", "operator bool() const;\n");
}

void TestBracesAndBlocks() {
    // A braced-init-list binds to what it initializes; a block brace does not.
    Expect("Thing t {1, 2};\n", "Thing t{1, 2};\n");
    Expect("int a[] = { 1,2,3 };\n", "int a[] = {1, 2, 3};\n");
    Expect("return { 1, 2 };\n", "return {1, 2};\n");
    Expect("std::vector<int> v = { };\n", "std::vector<int> v = {};\n");
    Expect("void f(){}\n", "void f() {}\n");
    Expect("struct S{int a;};\n", "struct S { int a; };\n");
    Expect("struct S{\nint a;\n};\n", "struct S {\n    int a;\n};\n");
    // `}` keeps company with what belongs to it.
    Expect("if (a) {\nb();\n} else {\nc();\n}\n",
           "if (a) {\n    b();\n} else {\n    c();\n}\n");
    Expect("do {\nx();\n} while (y);\n", "do {\n    x();\n} while (y);\n");
    Expect("try {\na();\n} catch (const E &e) {\nb();\n}\n",
           "try {\n    a();\n} catch (const E &e) {\n    b();\n}\n");
    Expect("struct S {\nint a;\n} s;\n", "struct S {\n    int a;\n} s;\n");
    // A lambda body is a block even though it sits inside a call.
    Expect("f([&](int x) {\nreturn x;\n});\n", "f([&](int x) {\n    return x;\n});\n");
    Expect("auto g = [=] { return 1; };\n", "auto g = [=] { return 1; };\n");
}

void TestEnums() {
    // In an enum body the commas separate declarations the way `;` does
    // elsewhere; without that the whole body is one line and the splitter
    // packs the enumerators together, which is not what an enum looks like.
    Expect("enum E {\nA = 1,\nB = 2,\n\n// note\nC  // last\n};\n",
           "enum E {\n    A = 1,\n    B = 2,\n\n    // note\n    C  // last\n};\n");
    Expect("enum class E : unsigned char { A = 1, B };\n",
           "enum class E : unsigned char { A = 1, B };\n");
    Expect("enum { A = (1 << 2), B };\n", "enum { A = (1 << 2), B };\n");
    // A class body's commas are not separators: `int a, b;` is one
    // declaration and stays one line.
    Expect("struct S {\nint a, b;\n};\n", "struct S {\n    int a, b;\n};\n");
}

void TestIndentation() {
    // Namespace bodies are not indented (NamespaceIndentation: None).
    Expect("namespace a {\nnamespace b {\nint x;\n}\n}\n",
           "namespace a {\nnamespace b {\nint x;\n}\n}\n");
    Expect("extern \"C\" {\nint x;\n}\n", "extern \"C\" {\nint x;\n}\n");
    // Access specifiers sit flush with the class (AccessModifierOffset: -4).
    Expect("class C {\npublic:\nint a;\nprivate:\nint b;\n};\n",
           "class C {\npublic:\n    int a;\nprivate:\n    int b;\n};\n");
    // Case labels take a level, their statements one more.
    Expect("switch (x) {\ncase 1:\na();\nbreak;\ndefault:\nb();\n}\n",
           "switch (x) {\n    case 1:\n        a();\n        break;\n    default:\n        b();\n}\n");
    // A braced case keeps its brace on the label's line and does not indent
    // twice for it.
    Expect("switch (x) {\ncase 1: {\na();\n}\n}\n",
           "switch (x) {\n    case 1: {\n        a();\n    }\n}\n");
    cppfmt::Options flat = Opts(100);
    flat.indent_case_labels = false;
    cppfmt::Result r = cppfmt::Format("switch (x) {\ncase 1:\na();\n}\n", flat);
    CHECK(r.ok);
    CHECK(r.text == "switch (x) {\ncase 1:\n    a();\n}\n");
    // A goto label keeps the surrounding indent; a ternary colon is not one.
    Expect("void f() {\ngoto done;\ndone:\nreturn;\n}\n",
           "void f() {\n    goto done;\n    done:\n    return;\n}\n");
    Expect("x = a ? b : c;\n", "x = a ? b : c;\n");
    // Input indentation is only ever read for structure, never copied.
    Expect("\t\t\tint f() {\n\t\t\t\t\treturn 1;\n\t\t\t}\n", "int f() {\n    return 1;\n}\n");
}

void TestBracelessBodies() {
    // A body the source put on the next line stays there, one level in.
    Expect("if (x)\ny();\n", "if (x)\n    y();\n");
    Expect("for (;;)\nx();\n", "for (;;)\n    x();\n");
    Expect("if (x)\ny();\nelse\nz();\n", "if (x)\n    y();\nelse\n    z();\n");
    Expect("if (a)\nif (b)\nc();\nd();\n", "if (a)\n    if (b)\n        c();\nd();\n");
    // A body the source put on the same line stays there if it fits. Nothing
    // is ever *joined* that the input had apart, which is what makes both
    // directions a fixed point.
    Expect("if (x) y();\n", "if (x) y();\n");
    Expect("while (x) --n;\n", "while (x) --n;\n");
    ExpectW("if (x) some_function_with_a_long_name(argument_one, argument_two);\n", 40,
            "if (x)\n    some_function_with_a_long_name(\n        argument_one, argument_two);\n");
    // An empty body is not a body.
    Expect("while (Spin());\n", "while (Spin());\n");
}

void TestMagicComma() {
    // A braced initializer whose last element carries a comma stays
    // exploded, one element per line, however short it is -- clang-format's
    // reading of a trailing comma. Nothing here ever adds or removes one, so
    // what comes out exploded goes back in exploded.
    Expect("int a[] = {1, 2, 3,};\n", "int a[] = {\n    1,\n    2,\n    3,\n};\n");
    Expect("int a[] = {1, 2, 3};\n", "int a[] = {1, 2, 3};\n");
    // A call is not a braced list; a trailing comma there is not even legal,
    // and a nested list keeps its own answer.
    Expect("S s = {{1, 2}, {3, 4}};\n", "S s = {{1, 2}, {3, 4}};\n");
}

void TestTrailingCommentAlignment() {
    Expect("int aaa = 1;  // one\nint b = 2;  // two\n",
           "int aaa = 1;  // one\nint b = 2;    // two\n");
    // A blank line, or a line with no comment, ends the run.
    Expect("int aaa = 1;  // one\nint b = 2;\nint c = 3;  // three\n",
           "int aaa = 1;  // one\nint b = 2;\nint c = 3;  // three\n");
    // A comment written hanging to the right continues the trailing comment
    // above it; one written at the statement's own indent does not.
    Expect("int aaa = 1;  // one\n              // more\nint b = 2;\n",
           "int aaa = 1;  // one\n              // more\nint b = 2;\n");
    Expect("int aaa = 1;  // one\n// standalone\nint b = 2;\n",
           "int aaa = 1;  // one\n// standalone\nint b = 2;\n");
    // A comment the *splitter* had to break a statement at sits at a
    // continuation indent this pass did not choose, so it never joins a run:
    // letting it in is what made a second pass disagree with the first.
    ExpectW("int f() { g(aaa);  // why\nreturn 1; }\n", 30,
            "int f() {\n    g(aaa);  // why\n    return 1;\n}\n");
    // A comment written inside the block the run's first line opened is not
    // a continuation of that line's trailing comment, and the test that says
    // so -- "is it at least as far right as the comment above it?" -- is the
    // same one after this pass has moved them.
    Expect("void f() {\n    try {  // lead\n        // second\n        g();\n    } catch (...) {}\n}\n",
           "void f() {\n    try {  // lead\n        // second\n        g();\n    } catch (...) {}\n}\n");
    cppfmt::Options off = Opts(100);
    off.align_trailing_comments = false;
    cppfmt::Result r = cppfmt::Format("int aaa = 1;  // one\nint b = 2;  // two\n", off);
    CHECK(r.ok);
    CHECK(r.text == "int aaa = 1;  // one\nint b = 2;  // two\n");
}

void TestSplitting() {
    // Continuation lines align under the open bracket and bin-pack
    // (AlignAfterOpenBracket: Align, BinPackArguments: true).
    ExpectW("void f(int aaaa, int bbbb, int cccc, int dddd);\n", 30,
            "void f(int aaaa, int bbbb,\n       int cccc, int dddd);\n");
    // With no comma to break at, the lowest-precedence operator wins, and
    // the operands bin-pack the same way arguments do.
    ExpectW("if (aaaaaaa && bbbbbbb && ccccccc) {}\n", 30,
            "if (aaaaaaa && bbbbbbb &&\n    ccccccc) {}\n");
    // Breaking after `=` beats splitting whatever is on the right of it.
    ExpectW("int result = compute(alpha, beta);\n", 30,
            "int result =\n    compute(alpha, beta);\n");
    // A member initializer list breaks before its colon.
    ExpectW("Foo::Foo(int a) : a_(a), b_(0) {}\n", 30,
            "Foo::Foo(int a)\n    : a_(a), b_(0) {}\n");
    // A subscript is the worst place to break and is only used as a last
    // resort, never in preference to the call it sits inside.
    ExpectW("bool ok = check(alphabet[index]) || other;\n", 40,
            "bool ok =\n    check(alphabet[index]) || other;\n");
    // A ternary puts `?` and `:` at the start of the next line.
    ExpectW("int v = condition ? first_value : second_value;\n", 30,
            "int v =\n    condition ? first_value\n    : second_value;\n");
    // Nothing to split at: emitted long rather than mangled.
    ExpectW("const char *s = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\";\n", 20,
            "const char *s =\n    \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\";\n");
}

void TestPreprocessor() {
    Expect("#   include   <stdio.h>\n", "#include <stdio.h>\n");
    Expect("#include \"a/b.h\"\n", "#include \"a/b.h\"\n");
    Expect("#pragma  once\n", "#pragma once\n");
    // `#define X(a)` is a function-like macro and `#define X (a)` is not.
    // No amount of re-lexing can tell those apart afterwards, so the one
    // space that decides it is carried over from the source verbatim.
    Expect("#define X(a) a\n", "#define X(a) a\n");
    Expect("#define X (a)\n", "#define X (a)\n");
    Expect("#define CAT(a,b) a##b\n", "#define CAT(a, b) a##b\n");
    Expect("#define STR(a) #a\n", "#define STR(a) #a\n");
    // Directives go flush left, even inside a function (IndentPPDirectives:
    // None), and a continued one is emitted exactly as written.
    Expect("void f() {\n#if X\ng();\n#endif\n}\n", "void f() {\n#if X\n    g();\n#endif\n}\n");
    Expect("#define M(x) \\\n    do { f(x); } while (0)\n",
           "#define M(x) \\\n    do { f(x); } while (0)\n");
    Expect("#if defined(A)&&!defined(B)\n#endif\n", "#if defined(A) && !defined(B)\n#endif\n");
}

void TestComments() {
    // A trailing comment gets the configured gap no matter what it had.
    Expect("int x = 1; // note\n", "int x = 1;  // note\n");
    Expect("int x = 1;          // note\n", "int x = 1;  // note\n");
    Expect("// lead\nint x;\n", "// lead\nint x;\n");
    // A comment interior to a bracket forces the bracket open: everything
    // after a `//` on its line belongs to the comment.
    Expect("f(a,  // why\n  b);\n", "f(a,  // why\n  b);\n");
    // A block comment whose every continuation line starts with `*` is
    // re-indented with the code; anything else is left byte for byte.
    Expect("void f() {\n/* a\n * b\n */\ng();\n}\n",
           "void f() {\n    /* a\n     * b\n     */\n    g();\n}\n");
    Expect("void f() {\n/* a\nb\n*/\ng();\n}\n", "void f() {\n    /* a\nb\n*/\n    g();\n}\n");
    // A `**` banner is art, not a doc comment, and is never re-indented.
    Expect("/* x\n** y\n*/\nint a;\n", "/* x\n** y\n*/\nint a;\n");
    // The shape test looks only at the comment itself, never at the column
    // it used to start in -- a column test stops being a fixed point as soon
    // as this pass has moved the opening `/*`.
    Expect("void f() {\n\t\t/* a\n\t\t * b\n\t\t */\n\tg();\n}\n",
           "void f() {\n    /* a\n     * b\n     */\n    g();\n}\n");
    Expect("int /*n*/ x;\n", "int /*n*/ x;\n");
    // Trailing whitespace inside a comment is the one text change allowed.
    Expect("// note   \nint x;\n", "// note\nint x;\n");
}

void TestBlankLines() {
    Expect("int a;\n\n\n\n\nint b;\n", "int a;\n\nint b;\n");
    Expect("\n\n\nint a;\n", "int a;\n");
    Expect("int a;\n\n\n", "int a;\n");
    // Blank lines the author put at the edges of a block are theirs to keep
    // (clang-format's LLVM preset keeps them too).
    Expect("void f() {\n\n    g();\n\n}\n", "void f() {\n\n    g();\n\n}\n");
    cppfmt::Options two = Opts(100);
    two.max_empty_lines = 2;
    cppfmt::Result r = cppfmt::Format("int a;\n\n\n\nint b;\n", two);
    CHECK(r.ok);
    CHECK(r.text == "int a;\n\n\nint b;\n");
}

void TestLexer() {
    Expect("auto s = R\"d( raw  \"x\" )d\";\n", "auto s = R\"d( raw  \"x\" )d\";\n");
    Expect("auto s = u8\"x\" ;\n", "auto s = u8\"x\";\n");
    // A ud-suffix is part of the literal: a space in it calls a different
    // operator, or none.
    Expect("auto s = \"x\"sv;\n", "auto s = \"x\"sv;\n");
    Expect("auto n = 1'000'000;\n", "auto n = 1'000'000;\n");
    Expect("auto n = 0x1p-3 + 1.5e+10;\n", "auto n = 0x1p-3 + 1.5e+10;\n");
    Expect("char c = '\\'';\n", "char c = '\\'';\n");
    // A preprocessing-number swallows a `.` that follows it, so the GNU case
    // range keeps its space.
    Expect("switch (x) {\ncase 0 ... 5:\nbreak;\n}\n",
           "switch (x) {\n    case 0 ... 5:\n        break;\n}\n");
    // A UTF-8 BOM is not part of the program and comes back untouched.
    Expect("\xEF\xBB\xBFint  x;\n", "\xEF\xBB\xBFint x;\n");
    Expect("", "");
    Expect("\n\n", "");
}

void TestRefusals() {
    struct Case {
        const char *src;
        const char *why;
    };
    const Case kCases[] = {
        {"auto s = \"unterminated;\n", "unterminated string"},
        {"char c = 'x;\n", "unterminated character"},
        {"/* never closed\n", "unterminated block comment"},
        // A line splice that welds two spellings together cannot be
        // reproduced by a formatter that is free to move tokens.
        {"int ab\\\ncd = 1;\n", "token-welding line splice"},
    };
    for (const Case &c : kCases) {
        cppfmt::Result r = cppfmt::Format(c.src);
        if (r.ok) {
            std::fprintf(stderr, "expected a refusal (%s) for:\n%s", c.why, c.src);
            Check(false, "Format refuses", __LINE__);
        }
        CHECK(!r.error.empty());
        CHECK(r.text.empty());
    }
    // The same splice with whitespace on either side is just whitespace.
    cppfmt::Result ok = cppfmt::Format("int f(int a, \\\n      int b);\n");
    CHECK(ok.ok);
    CHECK(ok.text == "int f(int a, int b);\n");
}

void TestNestingGuard() {
    // `gf` runs this inside the editor, so pathological nesting has to be an
    // error rather than a stack overflow. (20k nested parens really did
    // segfault mep before the same cap went into python_format.cpp.)
    std::string deep = "int x = ";
    deep.append(20000, '(');
    deep += "1";
    deep.append(20000, ')');
    deep += ";\n";
    cppfmt::Result r = cppfmt::Format(deep);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

// Re-lexes `text` and returns its tokens as one flat, whitespace-free string.
// Deliberately a *second*, dumber implementation than the one inside
// cpp_format.cpp: if the two ever disagree this test fails, which is the
// right outcome.
std::string Shape(const std::string &text) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            i++;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') i++;
            out += "<//>";
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) i++;
            i += 2;
            out += "</*>";
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            out += c;
            i++;
            while (i < text.size() && text[i] != q) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    out += text[i];
                    i++;
                }
                out += text[i];
                i++;
            }
            out += q;
            i++;
            continue;
        }
        // Everything else is copied a byte at a time with a marker between
        // runs of identifier characters, so `a b` and `ab` differ here.
        bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_';
        if (word) {
            out += '|';
            while (i < text.size()) {
                char d = text[i];
                bool w = (d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
                         (d >= '0' && d <= '9') || d == '_';
                if (!w) break;
                out += d;
                i++;
            }
            out += '|';
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

void TestNeverChangesMeaning() {
    const char *const kInputs[] = {
        "int x=1;",
        "a = b<c , d>(e);",
        "if (a<b && c>d) f();",
        "x = *p++ + -q;",
        "int *p = &a, **q = &p;",
        "void f(int(*g)(int), int (&h)[3]);",
        "auto l = [x = 1, &y](auto &&...args) -> int { return x; };",
        "struct A : B, virtual C { A() : B(), C() {} };",
        "enum class E : unsigned char { A = 1, B };",
        "#define F(a,b) a##b\nint F(x,y);",
        "#define G (1)\nint z = G;",
        "template <class... T> void f(T &&...t) { g(std::forward<T>(t)...); }",
        "x = y ? *a : &b;",
        "switch (c) { case 'a': case 'b': break; default: break; }",
        "int a[] = {[0] = 1, [2] = 3};",
        "auto s = R\"(a\"b)\" \"c\"_x;",
        "using T = int (*)(char, ...);",
        "co_return co_await f();",
        "void f() noexcept(noexcept(g())) &&;",
        "x = a<<2 >> 1;",
        "MACRO_NO_SEMICOLON\nint after;",
        "if (a) { } else if (b) { } else { }",
        "class C { int b : 3; int c : 5; };",
        "x = (int)y; z = (Foo *)w; v = (a)+b;",
        "for (auto &[k, v] : m) use(k, v);",
        "f(/*a=*/1, /*b=*/2);",
        "int x;  // trailing\nint y;",
        "static_assert(sizeof(int) == 4, \"no\");",
        "x = a ->* b; y = c .* d;",
        "namespace n = ::a::b;",
        "throw std::runtime_error(\"x\");",
        "label: while (1) { continue; }",
        "int i = 0 ... ;",
        "void f() try { g(); } catch (...) { }",
        "A<B<C> >::type v;",
        "x = new int[10]; delete[] p;",
        "operator\"\"_km(unsigned long long);",
        "if (x) /* why */ y();",
        "a = b, c = d;",
        "int f() { return {}; }",
    };
    for (const char *in : kInputs) {
        std::string src(in);
        if (src.empty() || src.back() != '\n') src += '\n';
        for (int width : {100, 40, 20}) {
            cppfmt::Result r = cppfmt::Format(src, Opts(width));
            if (!r.ok) continue;  // a refusal is always an acceptable answer
            if (Shape(src) != Shape(r.text)) {
                std::fprintf(stderr, "--- meaning changed (width %d) ---\n%s--- became ---\n%s",
                             width, src.c_str(), r.text.c_str());
                Check(false, "token shape is preserved", __LINE__);
            }
            ExpectStable(r.text, width);
        }
    }
}

void TestVerifier() {
    // With verification off the formatter still has to produce its own output
    // unchanged; the point of the flag is that it is the *only* thing between
    // a bug in the spacing table and a corrupted buffer.
    cppfmt::Options raw = Opts(100);
    raw.verify = false;
    cppfmt::Result a = cppfmt::Format("int  x = 1 ;\n", raw);
    CHECK(a.ok);
    CHECK(a.text == "int x = 1;\n");
    cppfmt::Result b = cppfmt::Format("int  x = 1 ;\n");
    CHECK(b.ok);
    CHECK(b.text == a.text);
}

void TestWholeFile() {
    const std::string src =
        "#include <vector>\n"
        "\n"
        "namespace demo {\n"
        "// A widget.\n"
        "class Widget : public Base {\n"
        "public:\n"
        "Widget(int a,int b):a_(a),b_(b){}\n"
        "int Get() const { return a_; }\n"
        "private:\n"
        "int a_=0,b_=0;\n"
        "};\n"
        "\n"
        "int Sum(const std::vector<int>&v){\n"
        "int total=0;\n"
        "for(size_t i=0;i<v.size();i++)total+=v[i];\n"
        "return total;\n"
        "}\n"
        "}  // namespace demo\n";
    const std::string want =
        "#include <vector>\n"
        "\n"
        "namespace demo {\n"
        "// A widget.\n"
        "class Widget : public Base {\n"
        "public:\n"
        "    Widget(int a, int b) : a_(a), b_(b) {}\n"
        "    int Get() const { return a_; }\n"
        "private:\n"
        "    int a_ = 0, b_ = 0;\n"
        "};\n"
        "\n"
        "int Sum(const std::vector<int> &v) {\n"
        "    int total = 0;\n"
        "    for (size_t i = 0; i < v.size(); i++) total += v[i];\n"
        "    return total;\n"
        "}\n"
        "}  // namespace demo\n";
    Expect(src, want);
}

}  // namespace

int main() {
    TestSpacing();
    TestTemplatesAndOperators();
    TestBracesAndBlocks();
    TestEnums();
    TestMagicComma();
    TestTrailingCommentAlignment();
    TestIndentation();
    TestBracelessBodies();
    TestSplitting();
    TestPreprocessor();
    TestComments();
    TestBlankLines();
    TestLexer();
    TestRefusals();
    TestNestingGuard();
    TestNeverChangesMeaning();
    TestVerifier();
    TestWholeFile();
    std::printf("cpp_format tests passed\n");
    return 0;
}
