// Coverage for c_format.h: mep's own C formatter, a port of clang-format
// in its LLVM style. Every expected layout below is clang-format's own
// (`clang-format --style="{BasedOnStyle: LLVM, SortIncludes: Never}"`,
// with the matching UseTab/ColumnLimit/IndentCaseLabels keys for the
// option cases), and each one is fed back through Format() to prove it is
// a fixed point.
//
// `mep-c-format-test --format FILE...` prints each file formatted instead
// (the reason a file was left alone on stderr), which is how the formatter
// is compared with clang-format on a corpus.

#include "c_format.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

int checks = 0;

void Same(const std::string &src, const std::string &want, const cfmt::Options &opts = cfmt::Options()) {
    checks++;
    const cfmt::Result r = cfmt::Format(src, opts);
    if (!r.ok) {
        std::fprintf(stderr, "FORMAT FAILED (line %d): %s\nsource:\n%s\n", r.error_line, r.error.c_str(), src.c_str());
        std::abort();
    }
    if (r.text != want) {
        std::fprintf(stderr, "MISMATCH\n--- input ---\n%s--- want ---\n%s--- got ---\n%s", src.c_str(), want.c_str(),
                     r.text.c_str());
        std::abort();
    }
    const cfmt::Result again = cfmt::Format(r.text, opts);
    if (!again.ok || again.text != r.text) {
        std::fprintf(stderr, "NOT IDEMPOTENT\n--- first ---\n%s--- second ---\n%s", r.text.c_str(), again.text.c_str());
        std::abort();
    }
}

int FormatFiles(int argc, char **argv) {
    int status = 0;
    for (int k = 2; k < argc; k++) {
        std::ifstream in(argv[k], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const cfmt::Result r = cfmt::Format(ss.str());
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", argv[k], r.error_line, r.error.c_str());
            status = 1;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}
}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);

    // --- layouts, checked against clang-format ---------------------------------------
    // Indentation and braces.
    Same("int main(void){\n"
         "int x=1;\n"
         "if(x){\n"
         "return x;}\n"
         "else return 0;\n"
         "}\n",
         "int main(void) {\n"
         "  int x = 1;\n"
         "  if (x) {\n"
         "    return x;\n"
         "  } else\n"
         "    return 0;\n"
         "}\n");
    // Pointers bind to the name.
    Same("int* p;\n"
         "char *const* q;\n"
         "void f(int*a,char * b);\n",
         "int *p;\n"
         "char *const *q;\n"
         "void f(int *a, char *b);\n");
    // A short function stays on one line.
    Same("int one(void)\n"
         "{\n"
         "  return 1;\n"
         "}\n",
         "int one(void) { return 1; }\n");
    // Case labels at the switch's indent.
    Same("void f(int x){switch(x){case 1: g(); break;\n"
         "case 2:{h();}\n"
         "default: break;}}\n",
         "void f(int x) {\n"
         "  switch (x) {\n"
         "  case 1:\n"
         "    g();\n"
         "    break;\n"
         "  case 2: {\n"
         "    h();\n"
         "  }\n"
         "  default:\n"
         "    break;\n"
         "  }\n"
         "}\n");
    // A long call aligns after the open paren.
    Same("void f(void){ result = some_function_name(first_argument_value, second_argument_value, third_argument_value); }\n",
         "void f(void) {\n"
         "  result = some_function_name(first_argument_value, second_argument_value,\n"
         "                              third_argument_value);\n"
         "}\n");
    // Trailing comments line up.
    Same("int a; // first\n"
         "int bbbbbbbb; // second\n"
         "int cc;  // third\n",
         "int a;        // first\n"
         "int bbbbbbbb; // second\n"
         "int cc;       // third\n");
    // A #define's backslashes line up at the limit.
    Same("#define SWAP(a, b) do { \\\n"
         "  int t = a; \\\n"
         "  a = b; \\\n"
         "  b = t; \\\n"
         "} while (0)\n",
         "#define SWAP(a, b)                                                             \\\n"
         "  do {                                                                         \\\n"
         "    int t = a;                                                                 \\\n"
         "    a = b;                                                                     \\\n"
         "    b = t;                                                                     \\\n"
         "  } while (0)\n");
    // A comment past the limit is reflowed.
    Same("// This is a very long comment that goes on and on past the eighty column limit for sure.\n"
         "int x;\n",
         "// This is a very long comment that goes on and on past the eighty column limit\n"
         "// for sure.\n"
         "int x;\n");
    // A long string is split.
    Same("const char *s = \"this string literal is far too long to fit within the eighty column limit\";\n",
         "const char *s =\n"
         "    \"this string literal is far too long to fit within the eighty column limit\";\n");
    // Every #if branch is laid out.
    Same("#if A\n"
         "int f(void){return 1;}\n"
         "#else\n"
         "int f(void){\n"
         "return 2;\n"
         "}\n"
         "#endif\n",
         "#if A\n"
         "int f(void) { return 1; }\n"
         "#else\n"
         "int f(void) { return 2; }\n"
         "#endif\n");
    // Struct and array initializers.
    Same("struct point p = {1,2};\n"
         "int a[] = {1, 2, 3,};\n"
         "static struct S s = {.name = \"x\", .value = 42, .next = NULL, .flags = FLAG_ONE | FLAG_TWO};\n",
         "struct point p = {1, 2};\n"
         "int a[] = {\n"
         "    1,\n"
         "    2,\n"
         "    3,\n"
         "};\n"
         "static struct S s = {\n"
         "    .name = \"x\", .value = 42, .next = NULL, .flags = FLAG_ONE | FLAG_TWO};\n");
    // Enums.
    Same("enum color {RED, GREEN,\n"
         "BLUE};\n"
         "enum { A = 1, B = 2 };\n",
         "enum color { RED, GREEN, BLUE };\n"
         "enum { A = 1, B = 2 };\n");
    // Includes keep their order.
    Same("#include <stdlib.h>\n"
         "#include <stdio.h>\n"
         "#include \"b.h\"\n"
         "#include \"a.h\"\n",
         "#include <stdlib.h>\n"
         "#include <stdio.h>\n"
         "#include \"b.h\"\n"
         "#include \"a.h\"\n");
    // Clang-format off regions are left alone.
    Same("// clang-format off\n"
         "int   x   =   1;\n"
         "// clang-format on\n"
         "int   y   =   2;\n",
         "// clang-format off\n"
         "int   x   =   1;\n"
         "// clang-format on\n"
         "int y = 2;\n");
    // << in a macro breaks before the operator.
    Same("#define MI_HINT_MAX ((uintptr_t)30 << 40) // wrap after 30TiB (area after 32TiB is used for huge OS pag\n",
         "#define MI_HINT_MAX                                                            \\\n"
         "  ((uintptr_t)30                                                               \\\n"
         "   << 40) // wrap after 30TiB (area after 32TiB is used for huge OS pag\n");
    // A reflowed comment may start a line with *.
    Same("/* int PyObject_GetOptionalAttr(PyObject *obj, PyObject *attr_name, PyObject **result); */\n"
         "int x;\n",
         "/* int PyObject_GetOptionalAttr(PyObject *obj, PyObject *attr_name, PyObject\n"
         " * **result); */\n"
         "int x;\n");
    // Blank lines collapse to one.
    Same("int a;\n"
         "\n"
         "\n"
         "\n"
         "int b;\n",
         "int a;\n"
         "\n"
         "int b;\n");
    // A for loop and a do-while.
    Same("void f(void){for(int i=0;i<n;i++){g(i);}\n"
         "do{h();}while(x);}\n",
         "void f(void) {\n"
         "  for (int i = 0; i < n; i++) {\n"
         "    g(i);\n"
         "  }\n"
         "  do {\n"
         "    h();\n"
         "  } while (x);\n"
         "}\n");
    // Function pointers and casts.
    Same("int (*fp)(int, char *);\n"
         "void f(void){ x = (int)y; p = (char *)malloc(n * sizeof(*p)); }\n",
         "int (*fp)(int, char *);\n"
         "void f(void) {\n"
         "  x = (int)y;\n"
         "  p = (char *)malloc(n * sizeof(*p));\n"
         "}\n");
    // K&R-style long declarations break.
    Same("static PyObject *builtin_function_with_a_long_name(PyObject *module, PyObject *const *args, Py_ssize_t nargs);\n",
         "static PyObject *builtin_function_with_a_long_name(PyObject *module,\n"
         "                                                   PyObject *const *args,\n"
         "                                                   Py_ssize_t nargs);\n");

    // Already formatted, and nothing at all.
    Same("int x;\n", "int x;\n");
    Same("", "");

    // --- options ----------------------------------------------------------------------
    const std::string opt_src = "void f(int x){switch(x){case 1: if(x){g(x, aaaaaaaaaaaaaaaaaaaaaaaa, bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb, cccccccccccccccccccc);}}}\n";
    {
        cfmt::Options tabs;  // UseTab: ForIndentation, IndentWidth 8, TabWidth 8
        tabs.use_tabs = true;
        tabs.indent_width = 8;
        tabs.tab_width = 8;
        Same(opt_src,
             "void f(int x) {\n"
             "\tswitch (x) {\n"
             "\tcase 1:\n"
             "\t\tif (x) {\n"
             "\t\t\tg(x, aaaaaaaaaaaaaaaaaaaaaaaa,\n"
             "\t\t\t  bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb, cccccccccccccccccccc);\n"
             "\t\t}\n"
             "\t}\n"
             "}\n",
             tabs);
    }
    {
        cfmt::Options wide;  // ColumnLimit: 100
        wide.column_limit = 100;
        Same(opt_src,
             "void f(int x) {\n"
             "  switch (x) {\n"
             "  case 1:\n"
             "    if (x) {\n"
             "      g(x, aaaaaaaaaaaaaaaaaaaaaaaa, bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb, cccccccccccccccccccc);\n"
             "    }\n"
             "  }\n"
             "}\n",
             wide);
    }
    {
        cfmt::Options cases;  // IndentCaseLabels: true, IndentWidth: 4
        cases.indent_case_labels = true;
        cases.indent_width = 4;
        Same(opt_src,
             "void f(int x) {\n"
             "    switch (x) {\n"
             "        case 1:\n"
             "            if (x) {\n"
             "                g(x, aaaaaaaaaaaaaaaaaaaaaaaa, bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb,\n"
             "                  cccccccccccccccccccc);\n"
             "            }\n"
             "    }\n"
             "}\n",
             cases);
    }
    // CRLF input keeps CRLF line ends.
    Same("int main(void){\r\n"
             "return 0;\r\n"
             "}\r\n", "int main(void) { return 0; }\r\n");

    // --- refusing ---------------------------------------------------------------------
    {
        // Each #if combination is a pass over the whole file; past the
        // budget the file is refused up front and returned unchanged.
        const std::string src = "#if A\nint  a;\n#if B\nint  b;\n#else\nint  c;\n#endif\n#endif\n";
        cfmt::Options tight;
        tight.max_work = 10;
        const cfmt::Result r = cfmt::Format(src, tight);
        CHECK(!r.ok);
        CHECK(r.text == src);
        CHECK(r.error.find("#if branch combinations") != std::string::npos);
        checks++;
        const cfmt::Result fine = cfmt::Format(src);
        CHECK(fine.ok);
        CHECK(fine.text == "#if A\nint a;\n#if B\nint b;\n#else\nint c;\n#endif\n#endif\n");
        checks++;
    }

    std::printf("c_format_test: %d checks passed\n", checks);
    return 0;
}
