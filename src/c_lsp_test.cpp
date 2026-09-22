// Windowless test for c_lsp.cpp and c_ast.cpp -- the analysis and
// parsing halves of mep's own C language server (`mep-c-lsp`,
// src/c_lsp_server.cpp). Drives the pure functions directly: no process,
// no JSON-RPC client, no GL context. CHECK(), never assert(): the
// Release build strips assert() entirely.
//
// Three conventions, the first two borrowed from org_lsp_test.cpp and
// python_lsp_test.cpp because they are what make a linter's test suite
// worth having, and the third forced by C:
//   - Diagnostics are asserted by CLspDiagnostic::code, never by message
//     wording, so the messages stay free to improve.
//   - Every check asserts the *absence* of a diagnostic on the
//     legal-but-suspicious shapes it must stay quiet about, as
//     deliberately as it asserts the true reports. A linter that cries
//     wolf gets switched off, and then its true reports go unseen too.
//   - Every preprocessor-shaped construct is tested for what it does to
//     the *parse*, not only to the diagnostics. A server that reads the
//     wrong arm of an `#if` does not produce a wrong warning; it
//     produces a wrong outline, wrong completions and wrong
//     go-to-definition, all silently.
#include "c_lsp.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "c_ast.h"

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

/** @brief Options that never touch the filesystem, so the suite answers the same in any directory. */
CLspOptions NoFiles() {
    CLspOptions opts;
    opts.check_files = false;
    opts.file_name = "test.c";
    return opts;
}

std::vector<CLspDiagnostic> Lint(const Lines &lines) { return CLspDiagnostics(lines, NoFiles()); }

/** @brief Counts diagnostics carrying a given code. */
size_t CountCode(const std::vector<CLspDiagnostic> &diags, const std::string &code) {
    size_t n = 0;
    for (const CLspDiagnostic &d : diags) {
        if (d.code == code) n++;
    }
    return n;
}

/** @brief Reports whether any diagnostic carries a given code. */
bool HasCode(const std::vector<CLspDiagnostic> &diags, const std::string &code) {
    return CountCode(diags, code) > 0;
}

/** @brief Reports whether linting these lines produces a given code. */
bool Reports(const Lines &lines, const std::string &code) { return HasCode(Lint(lines), code); }

// Returned by value, not by reference: GCC's -Wdangling-reference cannot
// tell that the reference points into `diags` rather than at the string
// literal parameter, and a copy of a diagnostic costs nothing.
/** @brief Returns the first diagnostic carrying a given code, aborting the test if there is none. */
CLspDiagnostic FindCode(const std::vector<CLspDiagnostic> &diags, const std::string &code) {
    for (const CLspDiagnostic &d : diags) {
        if (d.code == code) return d;
    }
    Check(false, code.c_str(), __LINE__);
    return diags[0];  // unreachable: Check aborts
}

/** @brief Reports whether a completion list offers a label. */
bool Offers(const std::vector<CLspCompletionItem> &items, const std::string &label) {
    for (const CLspCompletionItem &i : items) {
        if (i.label == label) return true;
    }
    return false;
}

/** @brief The completions at a position, with no filesystem access. */
std::vector<CLspCompletionItem> Complete(const Lines &lines, int line, int col) {
    return CLspCompletions(lines, line, col, NoFiles());
}

/** @brief Reports whether a symbol list holds a name. */
bool HasSymbol(const std::vector<CLspSymbol> &symbols, const std::string &name) {
    for (const CLspSymbol &s : symbols) {
        if (s.name == name) return true;
    }
    return false;
}

/** @brief Returns the symbol with a given name, aborting the test if there is none. */
CLspSymbol FindSymbol(const std::vector<CLspSymbol> &symbols, const std::string &name) {
    for (const CLspSymbol &s : symbols) {
        if (s.name == name) return s;
    }
    Check(false, name.c_str(), __LINE__);
    return symbols[0];  // unreachable
}

/** @brief Reports whether a parse produced no syntax errors at all. */
bool Parses(const Lines &lines) { return ParseC(lines).errors.empty(); }

/** @brief Finds a top-level function definition by name, or nullptr. */
const CNode *FindFunction(const CParseResult &parse, const std::string &name) {
    for (const CNodePtr &node : parse.unit->body) {
        if (node->kind == CNodeKind::FunctionDef && node->name == name) return node.get();
    }
    return nullptr;
}

// --- Lexer ------------------------------------------------------------

void TestTokenizerBasics() {
    const Lines lines = {"int x = 0x1f; /* c */ // trailing"};
    std::vector<CSyntaxError> errors;
    std::vector<CComment> comments;
    std::vector<CDirective> directives;
    const std::vector<CToken> tokens = TokenizeC(lines, &errors, &comments, &directives);
    CHECK(errors.empty());
    CHECK(comments.size() == 2);
    CHECK(comments[0].block);
    CHECK(!comments[1].block);
    // int x = 0x1f ; plus End
    CHECK(tokens.size() == 6);
    CHECK(tokens[0].kind == CTokKind::Keyword);
    CHECK(tokens[1].kind == CTokKind::Ident);
    CHECK(tokens[3].kind == CTokKind::Number);
    CHECK(tokens[3].text == "0x1f");
    CHECK(tokens.back().kind == CTokKind::End);
}

void TestLineSplice() {
    // A splice makes one identifier out of two lines, and the token's
    // start and end land on the lines it really came from.
    const Lines lines = {"int fo\\", "o = 1;"};
    const CParseResult parse = ParseC(lines);
    CHECK(parse.errors.empty());
    bool found = false;
    for (const CToken &t : parse.tokens) {
        if (t.kind != CTokKind::Ident || t.text != "foo") continue;
        found = true;
        CHECK(t.start.line == 0);
        CHECK(t.end.line == 1);
    }
    CHECK(found);
}

void TestUnterminatedLiterals() {
    CHECK(Reports({"char *s = \"oops;"}, "unterminated-string"));
    CHECK(Reports({"char c = 'a;"}, "unterminated-char"));
    CHECK(Reports({"/* never closed", "int x;"}, "unterminated-comment"));
    // A `//` comment swallowing the next line through a splice is legal.
    CHECK(Parses({"// spliced \\", "still a comment", "int x;"}));
}

void TestNumberValidation() {
    CHECK(Parses({"int a = 0x1p-3 + 1e5f + 0b1011 + 077 + 10ULL;"}));
    CHECK(Reports({"int a = 10xac0a;"}, "invalid-number"));
    CHECK(Reports({"int a = 019;"}, "invalid-number"));
    CHECK(Reports({"double d = 1e;"}, "invalid-number"));
    // C23 digit separators and the sized suffixes stay silent.
    CHECK(!Reports({"int a = 100;", "long b = 1L;", "unsigned c = 1u;"}, "invalid-number"));
}

void TestStrayCharacter() {
    CHECK(Reports({"int x = 1 ` 2;"}, "stray-character"));
    // Not inside a directive: `#error don't` is ordinary English.
    CHECK(!Reports({"#error don't do this", "int x;"}, "unterminated-char"));
    CHECK(!Reports({"#warning `=` is not allowed", "int x;"}, "stray-character"));
}

// --- Preprocessor -----------------------------------------------------

void TestConditionalArmSelection() {
    // `#if 0` is settled by the file itself, so the dead arm contributes
    // nothing at all.
    const CParseResult zero = ParseC({"#if 0", "int dead(void);", "#else", "int live(void);", "#endif"});
    CHECK(zero.errors.empty());
    const std::vector<CLspSymbol> symbols =
        CLspSymbols({"#if 0", "int dead(void);", "#else", "int live(void);", "#endif"});
    CHECK(HasSymbol(symbols, "live"));
    CHECK(!HasSymbol(symbols, "dead"));
}

void TestCplusplusGuardIsFalse() {
    // The `extern "C" {` prologue every public header opens with must
    // not unbalance the file: `__cplusplus` is false in a C server by
    // construction.
    const Lines lines = {"#ifdef __cplusplus", "extern \"C\" {", "#endif", "int f(void);",
                         "#ifdef __cplusplus", "}", "#endif"};
    CHECK(Parses(lines));
    CHECK(HasSymbol(CLspSymbols(lines), "f"));
}

void TestUnbalancedArmLosesToBalancedOne() {
    // Neither arm is settled, so shape decides: the one that leaves
    // every brace it opened closed wins.
    // With no `#else`, the arm that leaves the braces balanced is the
    // one a reader means: here the condition adds a whole function, and
    // the file around it is complete without one.
    CHECK(Parses({"#ifdef MAYBE", "int extra(void) { return 1; }", "#endif", "int f(void) { return 0; }"}));
    // An arm that opens a brace it never closes loses to the implicit
    // empty else, which is what keeps `extern \"C\" {` from unbalancing a
    // header.
    CHECK(Parses({"#ifdef MAYBE", "struct wrapper {", "#endif", "int f(void) { return 0; }"}));
    // With a written `#else` the first arm wins, because both spell the
    // same construct and the shape says nothing about which.
    const Lines either = {"#if MAYBE", "int f(void) {", "#else", "int f(void) {", "#endif", "  return 1;", "}"};
    CHECK(Parses(either));
}

void TestHeaderGuardArmIsTaken() {
    const Lines lines = {"#ifndef FOO_H", "#define FOO_H", "int f(void);", "#endif"};
    CHECK(Parses(lines));
    CHECK(HasSymbol(CLspSymbols(lines), "f"));
}

void TestFileDefinedMacroSettlesCondition() {
    const Lines lines = {"#define VERSION 3", "#if VERSION > 2", "int modern(void);", "#else",
                         "int ancient(void);", "#endif"};
    const std::vector<CLspSymbol> symbols = CLspSymbols(lines);
    CHECK(HasSymbol(symbols, "modern"));
    CHECK(!HasSymbol(symbols, "ancient"));
}

void TestPreprocessorStructureErrors() {
    CHECK(Reports({"#endif"}, "pp-stray-endif"));
    CHECK(Reports({"#else"}, "pp-stray-else"));
    CHECK(Reports({"#ifdef A", "int x;"}, "pp-unterminated-if"));
    CHECK(Reports({"#ifdef A", "#else", "#else", "#endif"}, "pp-duplicate-else"));
    CHECK(Reports({"#incldue <stdio.h>"}, "unknown-directive"));
    CHECK(Reports({"#include stdio.h>"}, "bad-include"));
    CHECK(Reports({"#define"}, "directive-no-name"));
    CHECK(Reports({"#if", "#endif"}, "directive-no-condition"));
    // A `#` alone is the null directive, which is legal and means
    // nothing.
    CHECK(!Reports({"#", "int x;"}, "unknown-directive"));
}

void TestMacroRedefinition() {
    CHECK(Reports({"#define N 1", "#define N 2"}, "macro-redefined"));
    // Identical text is how two headers guard the same constant.
    CHECK(!Reports({"#define N 1", "#define N 1"}, "macro-redefined"));
    // An `#undef` in between makes it deliberate.
    CHECK(!Reports({"#define N 1", "#undef N", "#define N 2"}, "macro-redefined"));
    // Definitions in different arms of one conditional are not a
    // redefinition; a compiler only ever sees one of them.
    CHECK(!Reports({"#ifdef A", "#define N 1", "#else", "#define N 2", "#endif"}, "macro-redefined"));
}

// --- Parser -----------------------------------------------------------

void TestDeclarationShapes() {
    CHECK(Parses({"int a, *b, c[3], (*d)(void), **e;"}));
    CHECK(Parses({"static const unsigned long long x = 1ULL;"}));
    CHECK(Parses({"typedef struct node { struct node *next; int v; } node_t;"}));
    CHECK(Parses({"enum color { RED, GREEN = 4, BLUE };"}));
    CHECK(Parses({"union u { int i; float f; };"}));
    CHECK(Parses({"struct bits { unsigned a : 3; unsigned : 5; unsigned b : 1; };"}));
    CHECK(Parses({"extern int errno;"}));
    CHECK(Parses({"void (*signal(int, void (*)(int)))(int);"}));
    CHECK(Parses({"int arr[3][4] = {{1,2,3,4},{0},{0}};"}));
    CHECK(Parses({"struct p { int x, y; };", "struct p q = { .y = 1, .x = 2 };"}));
}

void TestFunctionShapes() {
    const CParseResult parse = ParseC({"int add(int a, int b) { return a + b; }"});
    CHECK(parse.errors.empty());
    const CNode *fn = FindFunction(parse, "add");
    CHECK(fn != nullptr);
    CHECK(fn->params.size() == 2);
    CHECK(fn->params[0]->name == "a");
    CHECK(fn->type.base == "int");

    // A function returning a pointer is not a function returning void.
    const CParseResult ptr = ParseC({"void *alloc(unsigned n) { return 0; }"});
    const CNode *alloc = FindFunction(ptr, "alloc");
    CHECK(alloc != nullptr);
    CHECK(!alloc->type.IsVoid());
    CHECK(alloc->type.pointers == 1);

    // A function returning a function pointer keeps its own parameters.
    const CParseResult sig = ParseC({"void (*handler(int sig, int mode))(int) { return 0; }"});
    const CNode *handler = FindFunction(sig, "handler");
    CHECK(handler != nullptr);
    CHECK(handler->params.size() == 2);
    CHECK(handler->params[0]->name == "sig");
}

void TestKandRDefinition() {
    const CParseResult parse = ParseC({"int old(a, b)", "int a;", "char *b;", "{", "  return a;", "}"});
    CHECK(parse.errors.empty());
    const CNode *fn = FindFunction(parse, "old");
    CHECK(fn != nullptr);
    CHECK(fn->params.size() == 2);
    CHECK(fn->params[0]->name == "a");
}

void TestGnuExtensions() {
    CHECK(Parses({"int f(void) __attribute__((unused));"}));
    CHECK(Parses({"__attribute__((packed)) struct s { int x; };"}));
    CHECK(Parses({"int f(void) { return ({ int t = 1; t + 1; }); }"}));
    CHECK(Parses({"int f(int x) { switch (x) { case 1 ... 5: return 1; } return 0; }"}));
    CHECK(Parses({"void f(void) { __asm__ volatile (\"nop\" : : : \"memory\"); }"}));
    CHECK(Parses({"typedef __typeof__(1) my_int;"}));
    CHECK(Parses({"int f(int x) { return _Generic(x, int: 1, default: 0); }"}));
    CHECK(Parses({"_Static_assert(sizeof(int) >= 2, \"int is too small\");"}));
    CHECK(Parses({"int f(void) { void *p = &&done; goto *p; done: return 0; }"}));
}

void TestCastsAndCompoundLiterals() {
    // A cast to a type from a header this server never saw is still a
    // cast; refusing to read it would be a syntax error on every line.
    CHECK(Parses({"int f(void *p) { return *(NI *)p; }"}));
    CHECK(Parses({"long f(void) { return ((NI)8); }"}));
    CHECK(Parses({"struct s { int a; };", "int f(void) { return (struct s){1}.a; }"}));
    CHECK(Parses({"int f(int i) { return (int[]){1, 2, 3}[i]; }"}));
    CHECK(Parses({"unsigned f(void *p) { return (unsigned)(long)p; }"}));
    // But a subtraction is still a subtraction.
    const CParseResult parse = ParseC({"int f(int a, int b) { return (a) - b; }"});
    CHECK(parse.errors.empty());
    CHECK(!Reports({"int f(int a, int b) { return (a) - b; }"}, "undeclared-name"));
}

void TestMacroShapedConstructs() {
    // A file-scope macro invocation, with and without a semicolon.
    CHECK(Parses({"MODULE_LICENSE(\"GPL\");", "int f(void);"}));
    CHECK(Parses({"__BEGIN_DECLS", "int f(void);", "__END_DECLS"}));
    // A macro that produces the return type.
    const CParseResult parse = ParseC({"RTDECL(int) RTStrCmp(const char *a, const char *b) { return 0; }"});
    CHECK(parse.errors.empty());
    CHECK(FindFunction(parse, "RTStrCmp") != nullptr);
    // A trailing attribute macro before the `;`.
    CHECK(Parses({"int f(void) __THROW __nonnull((1));"}));
    // A macro that takes a block.
    CHECK(Parses({"void f(void *h) { list_for_each(p, h) { use(p); } }"}));
    // A macro that expands to struct members.
    CHECK(Parses({"struct o { PyObject_HEAD; int x; };"}));
    // A type produced by a macro with arguments.
    CHECK(Parses({"void f(void) { g_autoptr(GSettings) s = NULL; }"}));
    // A trailing comma in a variadic macro invocation.
    CHECK(Parses({"void f(void) { ADVANCE_MAP('a', 1, 'b', 2,); }"}));
    // A string literal split by a macro.
    CHECK(Parses({"#include <inttypes.h>", "void f(long x) { printf(\"%\" PRId64 \"\\n\", x); }"}));
}

void TestTypedefAmbiguity() {
    // With a typedef in scope, `A * b;` declares; without one, the
    // declaration reading still wins, because a statement whose whole
    // effect is to multiply and discard is not something anyone writes.
    const CParseResult parse = ParseC({"typedef int A;", "void f(void) { A * b; b = 0; }"});
    CHECK(parse.errors.empty());
    CHECK(!HasCode(Lint({"typedef int A;", "void f(void) { A * b; b = 0; }"}), "statement-no-effect"));
    CHECK(!HasCode(Lint({"void f(void) { size_t * b; b = 0; }"}), "statement-no-effect"));
}

void TestErrorRecovery() {
    // One error per line, and the rest of the file still parses into
    // something a reader can navigate.
    const Lines lines = {"int f(void) {", "  int x = 1", "  return x;", "}", "int g(void) { return 2; }"};
    const CParseResult parse = ParseC(lines);
    CHECK(!parse.errors.empty());
    CHECK(FindFunction(parse, "g") != nullptr);
    CHECK(HasSymbol(CLspSymbols(lines), "g"));

    // A half-typed call does not swallow the rest of the file.
    const Lines typing = {"void f(void) {", "  g(1,", "}", "int later(void) { return 0; }"};
    CHECK(HasSymbol(CLspSymbols(typing), "later"));
}

void TestRecursionCap() {
    // A wall of open parentheses is not a program, and the only
    // acceptable answer to it is a diagnostic rather than a crash.
    std::string deep = "int x = ";
    for (int i = 0; i < 400; i++) deep += "(";
    deep += "1";
    const CParseResult parse = ParseC({deep});
    CHECK(!parse.errors.empty());
    CHECK(parse.errors.size() <= 200);
}

void TestSyntaxErrorMessages() {
    CHECK(Reports({"int f(void) { return 1 }"}, "expected-semicolon"));
    CHECK(Reports({"int f(void) { g(1, 2; }"}, "expected-token"));
    CHECK(Reports({"int f(void) {"}, "expected-token"));
    CHECK(Reports({"}"}, "unexpected-brace"));
    CHECK(Reports({"struct * p;"}, "expected-tag"));
    CHECK(Reports({"struct s { int a; };", "int f(struct s *p) { return p. == 3; }"}, "expected-member-name"));
    // But `p->` at the end of a line is someone mid-type, not a mistake.
    CHECK(!Reports({"struct s { int a; };", "int f(struct s *p) { return p->", "a; }"}, "expected-member-name"));
}

// --- Scope diagnostics ------------------------------------------------

void TestUndeclaredName() {
    CHECK(Reports({"int f(void) { return nope; }"}, "undeclared-name"));
    CHECK(!Reports({"int f(void) { int ok = 1; return ok; }"}, "undeclared-name"));
    // Standard library names are known without being declared here.
    CHECK(!Reports({"#include <string.h>", "unsigned long f(const char *s) { return strlen(s); }"},
                   "undeclared-name"));
    // A header this server has no table for switches the check off.
    CHECK(!Reports({"#include <mystery/thing.h>", "int f(void) { return nope; }"}, "undeclared-name"));
    // So does a syntax error: mid-edit is exactly when an invented
    // "undeclared" is worst.
    CHECK(!Reports({"int f(void) { int x = ; return nope; }"}, "undeclared-name"));
    // And so does a fileful of unplaceable names, which is what a
    // platform header this server has never heard of looks like from
    // inside.
    CHECK(!Reports({"int f(void) {", "  return a1 + b2 + c3 + d4 + e5 + f6 + g7;", "}"}, "undeclared-name"));
    // Names in the implementation's own namespace were declared by the
    // implementation, wherever that is.
    CHECK(!Reports({"int f(void) { return __sync_fetch_and_add(0, 1); }"}, "undeclared-name"));
    CHECK(!Reports({"int f(void) { return __builtin_expect(1, 1); }"}, "undeclared-name"));
    // A macro this file defines is a declaration as far as a reader is
    // concerned.
    CHECK(!Reports({"#define LIMIT 10", "int f(void) { return LIMIT; }"}, "undeclared-name"));
}

void TestUnusedVariable() {
    CHECK(Reports({"int f(void) { int unused = 1; return 0; }"}, "unused-variable"));
    CHECK(!Reports({"int f(void) { int used = 1; return used; }"}, "unused-variable"));
    // A parameter is an interface, not a mistake.
    CHECK(!Reports({"int f(int ignored) { return 0; }"}, "unused-variable"));
    // The author already said not to ask.
    CHECK(!Reports({"int f(void) { int x __attribute__((unused)) = 1; return 0; }"}, "unused-variable"));
    // A name mentioned inside a `#if` arm this server did not take is
    // used by code it cannot see.
    CHECK(!Reports({"int f(void) {", "  int maybe = 1;", "#ifdef DEBUG", "  return maybe;", "#endif",
                    "  return 0;", "}"},
                   "unused-variable"));
    // A local typedef is not an object, so "never read" says nothing.
    CHECK(!Reports({"int f(void) { typedef int local_t; local_t x = 1; return x; }"}, "unused-variable"));
}

void TestUnusedStatic() {
    CHECK(Reports({"static int helper(void) { return 1; }", "int main(void) { return 0; }"},
                  "unused-static-function"));
    CHECK(!Reports({"static int helper(void) { return 1; }", "int main(void) { return helper(); }"},
                   "unused-static-function"));
    // External linkage means another file may call it.
    CHECK(!Reports({"int helper(void) { return 1; }"}, "unused-static-function"));
    // A mention inside a macro invocation counts.
    CHECK(!Reports({"static int helper(void) { return 1; }", "weak_alias(helper, other);"},
                   "unused-static-function"));
    CHECK(Reports({"static int counter = 0;", "int main(void) { return 0; }"}, "unused-static-variable"));
    // A header declares things for other files to use.
    {
        CLspOptions opts = NoFiles();
        opts.file_name = "thing.h";
        const Lines lines = {"static int helper(void) { return 1; }"};
        CHECK(!HasCode(CLspDiagnostics(lines, opts), "unused-static-function"));
    }
}

void TestRedefinition() {
    CHECK(Reports({"int f(void) { return 1; }", "int f(void) { return 2; }"}, "redefinition"));
    // A prototype followed by its definition is the normal shape of a C
    // file.
    CHECK(!Reports({"int f(void);", "int f(void) { return 1; }"}, "redefinition"));
    // Tentative definitions: C allows any number of them.
    CHECK(!Reports({"static int x;", "static int x;"}, "redefinition"));
    CHECK(Reports({"static int x = 1;", "static int x = 2;"}, "redefinition"));
}

void TestLabels() {
    CHECK(Reports({"int f(void) { goto nowhere; return 0; }"}, "undefined-label"));
    CHECK(!Reports({"int f(void) { goto done; done: return 0; }"}, "undefined-label"));
    CHECK(Reports({"int f(void) { unused: return 0; }"}, "unused-label"));
    CHECK(!Reports({"int f(void) { goto done; done: return 0; }"}, "unused-label"));
}

void TestDuplicates() {
    CHECK(Reports({"struct s { int a; int a; };"}, "duplicate-member"));
    CHECK(!Reports({"struct s { int a; };", "struct t { int a; };"}, "duplicate-member"));
    CHECK(Reports({"enum e { A, B, A };"}, "duplicate-enumerator"));
    CHECK(Reports({"int f(int a, int a) { return a; }"}, "duplicate-parameter"));
    CHECK(Reports({"int f(int x) { switch (x) { case 1: return 1; case 1: return 2; } return 0; }"},
                  "duplicate-case"));
    CHECK(!Reports({"int f(int x) { switch (x) { case 1: return 1; case 2: return 2; } return 0; }"},
                   "duplicate-case"));
    CHECK(Reports({"int f(int x) { switch (x) { default: return 1; default: return 2; } }"},
                  "duplicate-default"));
    // A nested switch has its own labels.
    CHECK(!Reports({"int f(int x, int y) {", "  switch (x) {", "  case 1:", "    switch (y) { case 1: return 1; }",
                    "    return 0;", "  }", "  return 0;", "}"},
                   "duplicate-case"));
}

void TestShadowing() {
    CHECK(Reports({"int f(void) { int x = 1; { int x = 2; return x; } }"}, "shadowed-variable"));
    // Shadowing a file-scope name is how C is written.
    CHECK(!Reports({"int x;", "int f(void) { int x = 1; return x; }"}, "shadowed-variable"));
}

// --- Idiom diagnostics ------------------------------------------------

void TestAssignInCondition() {
    CHECK(Reports({"int f(int x) { if (x = 0) return 1; return 0; }"}, "assign-in-condition"));
    // The convention C programmers actually use.
    CHECK(!Reports({"int f(int x) { if ((x = 0)) return 1; return 0; }"}, "assign-in-condition"));
    CHECK(!Reports({"int f(int x) { if (x == 0) return 1; return 0; }"}, "assign-in-condition"));
    CHECK(Reports({"int f(int c) { while (c = 1) return 1; return 0; }"}, "assign-in-condition"));
    CHECK(!Reports({"#include <stdio.h>", "int f(void) { int c; while ((c = getchar()) != EOF) return c; return 0; }"},
                   "assign-in-condition"));
}

void TestBitwisePrecedence() {
    CHECK(Reports({"int f(int a, int b, int c) { return a & b == c; }"}, "bitwise-precedence"));
    CHECK(Reports({"int f(int a, int b, int c) { return a | b < c; }"}, "bitwise-precedence"));
    CHECK(!Reports({"int f(int a, int b, int c) { return (a & b) == c; }"}, "bitwise-precedence"));
    CHECK(!Reports({"int f(int a, int b) { return a & b; }"}, "bitwise-precedence"));
}

void TestEmptyIfBody() {
    CHECK(Reports({"int f(int x) { if (x); return 0; }"}, "empty-if-body"));
    // A `while (...) ;` is a real busy-wait idiom.
    CHECK(!Reports({"int f(volatile int *p) { while (*p) ; return 0; }"}, "empty-if-body"));
    CHECK(!Reports({"int f(int x) { if (x) return 1; return 0; }"}, "empty-if-body"));
}

void TestStatementHasEffect() {
    CHECK(Reports({"int f(int a, int b) { a + b; return 0; }"}, "statement-no-effect"));
    CHECK(!Reports({"int f(int a) { a++; return a; }"}, "statement-no-effect"));
    // `(void)x;` is how C says "I know, and I mean it".
    CHECK(!Reports({"int f(int a) { (void)a; return 0; }"}, "statement-no-effect"));
    // A bare name that resolves to nothing is a macro that expanded to
    // statements.
    CHECK(!Reports({"int f(void) { Py_BEGIN_ALLOW_THREADS; return 0; }"}, "statement-no-effect"));
    CHECK(Reports({"int f(int a) { a; return 0; }"}, "statement-no-effect"));
}

void TestSelfAssignment() {
    CHECK(Reports({"int f(int a) { a = a; return a; }"}, "self-assignment"));
    CHECK(Reports({"struct s { int x; };", "void f(struct s *p) { p->x = p->x; }"}, "self-assignment"));
    CHECK(!Reports({"int f(int a, int b) { a = b; return a; }"}, "self-assignment"));
}

void TestUnreachableCode() {
    CHECK(Reports({"int f(void) { return 1; return 2; }"}, "unreachable-code"));
    // A `switch` body is full of statements after a `break`, and every
    // one of them is reached through a label.
    CHECK(!Reports({"int f(int x) {", "  switch (x) {", "  case 1: x = 1; break;", "  case 2: x = 2; break;",
                    "  }", "  return x;", "}"},
                   "unreachable-code"));
    CHECK(!Reports({"int f(int x) { if (x) return 1; return 2; }"}, "unreachable-code"));
    // A declaration after a `return` does nothing at all, and a label
    // after one is jumped to.
    CHECK(!Reports({"int f(void) { return 1; int x; }"}, "unreachable-code"));
    CHECK(!Reports({"int f(int x) { if (x) goto done; return 0; done: return 1; }"}, "unreachable-code"));
}

void TestReturnChecks() {
    CHECK(Reports({"void f(void) { return 1; }"}, "return-value-in-void"));
    CHECK(!Reports({"void *f(void) { return 0; }"}, "return-value-in-void"));
    CHECK(Reports({"int f(void) { return; }"}, "return-no-value"));
    CHECK(Reports({"int f(int x) { if (x) x++; }"}, "missing-return"));
    CHECK(!Reports({"int f(int x) { return x; }"}, "missing-return"));
    CHECK(!Reports({"int main(void) { }"}, "missing-return"));
    CHECK(!Reports({"#include <stdlib.h>", "int f(void) { exit(1); }"}, "missing-return"));
    CHECK(!Reports({"_Noreturn int f(void) { for (;;) ; }"}, "missing-return"));
    // A return type that came out of a macro is not one this server can
    // read, so it says nothing about it.
    CHECK(!Reports({"RTDECL(int) f(void) { g(); }"}, "missing-return"));
    CHECK(!Reports({"DECLINLINE(void) f(void) { return 1; }"}, "return-value-in-void"));
}

void TestCharacterConstants() {
    CHECK(Reports({"int c = 'ab';"}, "multi-character-constant"));
    CHECK(!Reports({"int c = 'a';"}, "multi-character-constant"));
    // One escape is one character, however long it is written.
    CHECK(!Reports({"int c = '\\n';"}, "multi-character-constant"));
    CHECK(!Reports({"int c = '\\x20';"}, "multi-character-constant"));
    CHECK(!Reports({"int c = '\\033';"}, "multi-character-constant"));
    CHECK(!Reports({"int c = U'\\u2510';"}, "multi-character-constant"));
}

void TestFormatArgumentCount() {
    CHECK(Reports({"#include <stdio.h>", "void f(int a) { printf(\"%d %d\\n\", a); }"}, "format-argument-count"));
    CHECK(!Reports({"#include <stdio.h>", "void f(int a, int b) { printf(\"%d %d\\n\", a, b); }"},
                   "format-argument-count"));
    CHECK(!Reports({"#include <stdio.h>", "void f(void) { printf(\"100%%\\n\"); }"}, "format-argument-count"));
    // `*` asks for one more argument when printing.
    CHECK(!Reports({"#include <stdio.h>", "void f(int w, int a) { printf(\"%*d\", w, a); }"},
                   "format-argument-count"));
    // ... and for one fewer when scanning, where it suppresses the
    // assignment instead.
    CHECK(!Reports({"#include <stdio.h>", "void f(char *s) { sscanf(s, \"%*d %d\", s); }"},
                   "format-argument-count"));
    // A scanset is one conversion, not a handful.
    CHECK(!Reports({"#include <stdio.h>", "void f(char *s, char *o) { sscanf(s, \"%[^\\n]\", o); }"},
                   "format-argument-count"));
    // The format string's own position differs per function.
    CHECK(Reports({"#include <stdio.h>", "void f(char *b, int a) { snprintf(b, 10, \"%d %d\", a); }"},
                   "format-argument-count"));
    // A format built out of macros is not one this check can count.
    CHECK(!Reports({"#include <inttypes.h>", "void f(long a) { printf(\"%\" PRId64 \" %d\\n\", a); }"},
                   "format-argument-count"));
    // Positional conversions mean the count is not the conversion count.
    CHECK(!Reports({"#include <stdio.h>", "void f(int a) { printf(\"%2$d %1$d\", a, a); }"},
                   "format-argument-count"));
    // Nothing is said about a `vprintf`-family call at all.
    CHECK(!Reports({"#include <stdarg.h>", "#include <stdio.h>",
                    "void f(const char *fmt, va_list ap) { vprintf(fmt, ap); }"},
                   "format-argument-count"));
}

void TestNoSuchMember() {
    CHECK(Reports({"struct s { int a; };", "int f(struct s *p) { return p->b; }"}, "no-such-member"));
    CHECK(!Reports({"struct s { int a; };", "int f(struct s *p) { return p->a; }"}, "no-such-member"));
    CHECK(!Reports({"typedef struct { int a; } s_t;", "int f(s_t *p) { return p->a; }"}, "no-such-member"));
    // An anonymous member lifts its own fields into the outer struct,
    // and this server does not follow that, so it stays quiet.
    CHECK(!Reports({"struct s { union { int a; }; };", "int f(struct s *p) { return p->a; }"}, "no-such-member"));
    // A struct this server never saw the definition of settles nothing.
    CHECK(!Reports({"int f(struct unknown *p) { return p->whatever; }"}, "no-such-member"));
}

void TestLineLength() {
    CLspOptions opts = NoFiles();
    opts.max_line_length = 20;
    const Lines lines = {"int x;", "int this_identifier_is_definitely_too_long_for_twenty_columns;"};
    const std::vector<CLspDiagnostic> diags = CLspDiagnostics(lines, opts);
    CHECK(CountCode(diags, "line-too-long") == 1);
    CHECK(FindCode(diags, "line-too-long").line == 1);
    // Off unless a caller asks for it.
    CHECK(!HasCode(CLspDiagnostics(lines, NoFiles()), "line-too-long"));
}

void TestRealisticFileIsSilent() {
    // The bar every one of the checks above has to clear together: an
    // ordinary, correct C file produces nothing at all. A linter that
    // cries wolf on this gets switched off, and then its true reports go
    // unseen too.
    const Lines lines = {
        "#include <stdio.h>",
        "#include <stdlib.h>",
        "#include <string.h>",
        "",
        "#define MAX_LINE 4096",
        "",
        "struct counter {",
        "    char *name;",
        "    unsigned long hits;",
        "};",
        "",
        "static struct counter *counter_new(const char *name)",
        "{",
        "    struct counter *c = malloc(sizeof *c);",
        "    if (c == NULL) {",
        "        return NULL;",
        "    }",
        "    c->name = strdup(name);",
        "    c->hits = 0;",
        "    return c;",
        "}",
        "",
        "static void counter_free(struct counter *c)",
        "{",
        "    if (c == NULL) {",
        "        return;",
        "    }",
        "    free(c->name);",
        "    free(c);",
        "}",
        "",
        "int main(int argc, char **argv)",
        "{",
        "    char line[MAX_LINE];",
        "    struct counter *c = counter_new(argc > 1 ? argv[1] : \"stdin\");",
        "    if (c == NULL) {",
        "        fprintf(stderr, \"out of memory\\n\");",
        "        return EXIT_FAILURE;",
        "    }",
        "    while (fgets(line, (int)sizeof line, stdin) != NULL) {",
        "        size_t len = strlen(line);",
        "        if (len > 0 && line[len - 1] == '\\n') {",
        "            line[len - 1] = '\\0';",
        "        }",
        "        c->hits++;",
        "    }",
        "    printf(\"%s: %lu\\n\", c->name, c->hits);",
        "    counter_free(c);",
        "    return EXIT_SUCCESS;",
        "}",
    };
    const std::vector<CLspDiagnostic> diags = Lint(lines);
    if (!diags.empty()) {
        for (const CLspDiagnostic &d : diags) {
            std::fprintf(stderr, "unexpected: %d:%d [%s] %s\n", d.line + 1, d.col_start + 1, d.code.c_str(),
                         d.message.c_str());
        }
    }
    CHECK(diags.empty());
}

void TestRealisticHeaderIsSilent() {
    const Lines lines = {
        "#ifndef COUNTER_H",
        "#define COUNTER_H",
        "",
        "#include <stddef.h>",
        "",
        "#ifdef __cplusplus",
        "extern \"C\" {",
        "#endif",
        "",
        "struct counter;",
        "",
        "struct counter *counter_new(const char *name);",
        "void counter_free(struct counter *c);",
        "size_t counter_hits(const struct counter *c);",
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
        "#endif  /* COUNTER_H */",
    };
    CLspOptions opts = NoFiles();
    opts.file_name = "counter.h";
    const std::vector<CLspDiagnostic> diags = CLspDiagnostics(lines, opts);
    if (!diags.empty()) {
        for (const CLspDiagnostic &d : diags) {
            std::fprintf(stderr, "unexpected: %d:%d [%s] %s\n", d.line + 1, d.col_start + 1, d.code.c_str(),
                         d.message.c_str());
        }
    }
    CHECK(diags.empty());
}

// --- Included headers -------------------------------------------------

/**
 * @brief The one test that touches the filesystem, because the feature under it does.
 *
 * `#include "sibling.h"` is how a C project is held together, and a
 * server that cannot follow one calls every name in it undeclared. This
 * writes a real header into a temporary directory, points the options at
 * it, and checks the four things that follow from resolving it.
 */
void TestResolvedInclude() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "mep-c-lsp-test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) return;  // a machine with no writable temp directory is not one to fail over
    {
        std::ofstream header(dir / "sibling.h");
        header << "#ifndef SIBLING_H\n#define SIBLING_H\n";
        header << "#define SIBLING_LIMIT 12\n";
        header << "typedef struct sibling { int depth; } sibling_t;\n";
        header << "int sibling_add(int a, int b);\n";
        header << "#endif\n";
    }
    CLspOptions opts;
    opts.check_files = true;
    opts.doc_dir = dir.string();
    opts.file_name = "user.c";
    const Lines lines = {"#include \"sibling.h\"", "int f(sibling_t *s) {",
                         "    return sibling_add(s->depth, SIBLING_LIMIT);", "}"};

    const std::vector<CLspDiagnostic> diags = CLspDiagnostics(lines, opts);
    for (const CLspDiagnostic &d : diags) {
        std::fprintf(stderr, "unexpected: %d:%d [%s] %s\n", d.line + 1, d.col_start + 1, d.code.c_str(),
                     d.message.c_str());
    }
    CHECK(diags.empty());

    // A header that is not there is a different matter, and is said so
    // -- but only when the search path is evidently right for this
    // project, which one resolving include is the evidence for.
    const Lines missing = {"#include \"sibling.h\"", "#include \"nowhere.h\"", "int f(void) { return 0; }"};
    CHECK(HasCode(CLspDiagnostics(missing, opts), "include-not-found"));
    // With nothing at all resolving, the headers live somewhere a build
    // system knows about and this server does not; saying so once per
    // include would help nobody.
    const Lines all_missing = {"#include \"nowhere.h\"", "#include \"elsewhere.h\"",
                               "int f(void) { return 0; }"};
    CHECK(!HasCode(CLspDiagnostics(all_missing, opts), "include-not-found"));

    // Go to definition crosses into the header.
    const CLspLocation loc = CLspDefinition(lines, 2, 15, opts);
    CHECK(loc.found);
    CHECK(loc.path == fs::weakly_canonical(dir / "sibling.h", ec).string());
    CHECK(loc.line == 4);

    // ... and so does completion, hover and member resolution.
    const std::vector<CLspCompletionItem> items = CLspCompletions(lines, 2, 17, opts);
    CHECK(Offers(items, "sibling_add"));
    const std::vector<CLspCompletionItem> members = CLspCompletions(lines, 2, 27, opts);
    CHECK(Offers(members, "depth"));

    // The `#include` line itself jumps to the file.
    const CLspLocation header_jump = CLspDefinition(lines, 0, 12, opts);
    CHECK(header_jump.found);
    CHECK(!header_jump.path.empty());

    fs::remove_all(dir, ec);
}

// --- Completion -------------------------------------------------------

void TestCompletionScope() {
    const Lines lines = {"static int global_thing = 1;", "int f(int param) {", "  int local = 0;", "  lo", "}"};
    const std::vector<CLspCompletionItem> items = Complete(lines, 3, 4);
    CHECK(Offers(items, "local"));
    CHECK(!Offers(items, "param"));  // filtered by the typed prefix
    const std::vector<CLspCompletionItem> all = Complete(lines, 3, 2);
    CHECK(Offers(all, "param"));
    CHECK(Offers(all, "global_thing"));
    CHECK(Offers(all, "f"));
}

void TestCompletionKeywordsAndStandardNames() {
    const std::vector<CLspCompletionItem> items = Complete({"int f(void) { ret", "}"}, 0, 17);
    CHECK(Offers(items, "return"));
    const std::vector<CLspCompletionItem> std_names = Complete({"#include <string.h>", "void f(void) { strl", "}"}, 1, 19);
    CHECK(Offers(std_names, "strlen"));
}

void TestCompletionMembers() {
    const Lines lines = {"struct point { int x; int y; };", "int f(struct point *p) {", "  return p->", "}"};
    const std::vector<CLspCompletionItem> items = Complete(lines, 2, 12);
    CHECK(Offers(items, "x"));
    CHECK(Offers(items, "y"));
    CHECK(!Offers(items, "f"));  // only members, once a `->` narrowed it

    const Lines dotted = {"struct point { int x; int y; };", "int f(struct point p) {", "  return p.y;", "}"};
    const std::vector<CLspCompletionItem> dot_items = Complete(dotted, 2, 11);
    CHECK(Offers(dot_items, "y"));

    // Through a typedef, and through a chain.
    const Lines chain = {"struct inner { int deep; };", "typedef struct outer { struct inner *in; } outer_t;",
                         "int f(outer_t *o) { return o->in->", "; }"};
    const std::vector<CLspCompletionItem> chain_items = Complete(chain, 2, 34);
    CHECK(Offers(chain_items, "deep"));
}

void TestCompletionDirectives() {
    const std::vector<CLspCompletionItem> items = Complete({"#inc"}, 0, 4);
    CHECK(Offers(items, "include"));
    const std::vector<CLspCompletionItem> headers = Complete({"#include <std"}, 0, 13);
    CHECK(Offers(headers, "stdio.h"));
    CHECK(Offers(headers, "stdlib.h"));
    CHECK(!Offers(headers, "string.h"));  // filtered by the typed prefix
}

void TestCompletionTags() {
    const Lines lines = {"struct point { int x; };", "void f(void) { struct po", "}"};
    const std::vector<CLspCompletionItem> items = Complete(lines, 1, 24);
    CHECK(Offers(items, "point"));
}

void TestNoCompletionInStringsAndComments() {
    CHECK(Complete({"const char *s = \"ret\";"}, 0, 20).empty());
    CHECK(Complete({"// ret"}, 0, 6).empty());
}

// --- Hover ------------------------------------------------------------

void TestHover() {
    const Lines lines = {"/** doc */", "static int counter = 0;", "int f(void) { return counter; }"};
    const CLspHoverInfo info = CLspHover(lines, 2, 22);
    CHECK(info.found);
    CHECK(info.text.find("counter") != std::string::npos);

    const CLspHoverInfo keyword = CLspHover({"static int x;"}, 0, 2);
    CHECK(keyword.found);
    CHECK(keyword.text.find("storage class") != std::string::npos);

    const CLspHoverInfo macro = CLspHover({"#define LIMIT 10", "int x = LIMIT;"}, 1, 9);
    CHECK(macro.found);
    CHECK(macro.text.find("#define LIMIT 10") != std::string::npos);

    const CLspHoverInfo std_name = CLspHover({"#include <string.h>", "void f(char *a) { strlen(a); }"}, 1, 19);
    CHECK(std_name.found);
    CHECK(std_name.text.find("strlen") != std::string::npos);
    CHECK(std_name.text.find("string.h") != std::string::npos);

    const CLspHoverInfo member =
        CLspHover({"struct s { unsigned long hits; };", "void f(struct s *p) { p->hits++; }"}, 1, 26);
    CHECK(member.found);
    CHECK(member.text.find("hits") != std::string::npos);

    const CLspHoverInfo header = CLspHover({"#include <stdio.h>"}, 0, 12);
    CHECK(header.found);
    CHECK(header.text.find("stdio.h") != std::string::npos);

    // Nothing to say about a position that is not on a word at all.
    CHECK(!CLspHover({"int x;"}, 0, 6).found);
}

// --- Symbols, folding, definition, references, signature --------------

void TestSymbols() {
    const Lines lines = {"#define LIMIT 10",
                         "typedef struct point { int x; int y; } point_t;",
                         "enum color { RED, GREEN };",
                         "static int counter;",
                         "int add(int a, int b) { return a + b; }"};
    const std::vector<CLspSymbol> symbols = CLspSymbols(lines);
    CHECK(HasSymbol(symbols, "LIMIT"));
    CHECK(HasSymbol(symbols, "point"));
    CHECK(HasSymbol(symbols, "x"));
    CHECK(HasSymbol(symbols, "color"));
    CHECK(HasSymbol(symbols, "RED"));
    CHECK(HasSymbol(symbols, "counter"));
    CHECK(HasSymbol(symbols, "add"));
    CHECK(FindSymbol(symbols, "add").kind == 12);
    CHECK(FindSymbol(symbols, "x").parent >= 0);
    CHECK(symbols[static_cast<size_t>(FindSymbol(symbols, "x").parent)].name == "point");
    CHECK(FindSymbol(symbols, "add").detail.find("int add(int a, int b)") != std::string::npos);

    // A typedef of an anonymous struct outlines under the typedef's own
    // name, which is the only name anyone will look for.
    const std::vector<CLspSymbol> anon = CLspSymbols({"typedef struct { int a; } thing_t;"});
    CHECK(HasSymbol(anon, "thing_t"));
    CHECK(HasSymbol(anon, "a"));
}

void TestFolds() {
    const Lines lines = {"#include <stdio.h>", "#include <stdlib.h>", "", "int f(void)", "{",
                         "    if (1) {", "        return 1;", "    }", "    return 0;", "}"};
    const std::vector<CLspFold> folds = CLspFolds(lines);
    bool imports = false;
    bool body = false;
    for (const CLspFold &f : folds) {
        if (f.kind == "imports" && f.start_line == 0 && f.end_line == 1) imports = true;
        if (f.kind.empty() && f.start_line == 4) body = true;
    }
    CHECK(imports);
    CHECK(body);

    const std::vector<CLspFold> conditional = CLspFolds({"#ifdef A", "int x;", "#endif"});
    bool group = false;
    for (const CLspFold &f : conditional) {
        if (f.start_line == 0 && f.end_line == 1) group = true;
    }
    CHECK(group);

    const std::vector<CLspFold> comments = CLspFolds({"/* one", "   two */", "int x;"});
    bool comment = false;
    for (const CLspFold &f : comments) {
        if (f.kind == "comment" && f.start_line == 0 && f.end_line == 1) comment = true;
    }
    CHECK(comment);
}

void TestDefinition() {
    const Lines lines = {"static int helper(int a) { return a; }", "int f(void) { return helper(1); }"};
    const CLspLocation loc = CLspDefinition(lines, 1, 21, NoFiles());
    CHECK(loc.found);
    CHECK(loc.line == 0);
    CHECK(loc.path.empty());

    const CLspLocation macro = CLspDefinition({"#define LIMIT 10", "int x = LIMIT;"}, 1, 9, NoFiles());
    CHECK(macro.found);
    CHECK(macro.line == 0);

    const CLspLocation member =
        CLspDefinition({"struct s { int hits; };", "void f(struct s *p) { p->hits++; }"}, 1, 26, NoFiles());
    CHECK(member.found);
    CHECK(member.line == 0);

    CHECK(!CLspDefinition({"int x;"}, 0, 6, NoFiles()).found);
}

void TestReferences() {
    const Lines lines = {"int f(void) {", "  int count = 0;", "  count++;", "  return count;", "}",
                         "int g(void) { int count = 1; return count; }"};
    const CLspReferenceSet refs = CLspReferences(lines, 1, 7);
    CHECK(refs.found);
    CHECK(refs.name == "count");
    // A local is renamed only inside its own function, so `g`'s own
    // `count` must not be in the list.
    CHECK(refs.refs.size() == 3);
    CHECK(refs.refs.front().line == 1);
    CHECK(refs.refs.front().is_write);
    CHECK(refs.rename_blocked_reason.empty());

    const CLspReferenceSet keyword = CLspReferences({"static int x;"}, 0, 2);
    CHECK(!keyword.rename_blocked_reason.empty());

    const CLspReferenceSet std_name =
        CLspReferences({"#include <string.h>", "void f(char *a) { strlen(a); }"}, 1, 20);
    CHECK(!std_name.rename_blocked_reason.empty());
}

void TestSignatureHelp() {
    const Lines lines = {"int add(int a, int b);", "int f(void) { return add(1, ", "}"};
    const CLspSignature sig = CLspSignatureHelp(lines, 1, 28);
    CHECK(sig.found);
    CHECK(sig.label.find("add") != std::string::npos);
    CHECK(sig.params.size() == 2);
    CHECK(sig.active_param == 1);

    const Lines std_call = {"#include <string.h>", "void f(void *d, const void *s) { memcpy(d, s, "};
    const CLspSignature std_sig = CLspSignatureHelp(std_call, 1, 45);
    CHECK(std_sig.found);
    CHECK(std_sig.label.find("memcpy") != std::string::npos);
    CHECK(std_sig.params.size() == 3);
    CHECK(std_sig.active_param == 2);

    const Lines macro = {"#define MIN(a, b) ((a) < (b) ? (a) : (b))", "int f(void) { return MIN(1, "};
    const CLspSignature macro_sig = CLspSignatureHelp(macro, 1, 28);
    CHECK(macro_sig.found);
    CHECK(macro_sig.params.size() == 2);

    CHECK(!CLspSignatureHelp({"int x = 1;"}, 0, 9).found);
}

// --- Vocabulary -------------------------------------------------------

void TestVocabulary() {
    CHECK(!CLspKeywordVocab().empty());
    CHECK(!CLspDirectiveVocab().empty());
    CHECK(!CLspHeaderVocab().empty());
    CHECK(CLspKnowsHeader("stdio.h"));
    CHECK(CLspKnowsHeader("sys/socket.h"));
    CHECK(!CLspKnowsHeader("mystery/thing.h"));
    CHECK(CLspHeaderOf("printf") == "stdio.h");
    CHECK(CLspHeaderOf("malloc") == "stdlib.h");
    CHECK(CLspHeaderOf("size_t") == "stddef.h");
    CHECK(CLspHeaderOf("not_a_standard_name_at_all").empty());
    const CLspVocabEntry *printf_entry = CLspLookupStandardName("printf");
    CHECK(printf_entry != nullptr);
    // The signature comes from the real header, so it has the real
    // parameters in it.
    CHECK(std::string(printf_entry->detail).find("const char *format") != std::string::npos);
    CHECK(std::string(printf_entry->doc).find("stdout") != std::string::npos);
    const std::vector<CLspVocabEntry> *members = CLspHeaderMembers("stdio.h");
    CHECK(members != nullptr);
    CHECK(members->size() > 20);
    CHECK(CLspHeaderMembers("mystery/thing.h") == nullptr);
}

void TestKeywordPredicates() {
    CHECK(IsCKeyword("int"));
    CHECK(IsCKeyword("_Static_assert"));
    CHECK(IsCKeyword("__attribute__"));
    // Deliberately not keywords: they are macros in most real code, and
    // `typedef enum { false, true } bool;` has to keep parsing.
    CHECK(!IsCKeyword("bool"));
    CHECK(!IsCKeyword("true"));
    CHECK(!IsCKeyword("static_assert"));
    CHECK(Parses({"typedef enum { false, true } bool;", "bool b = true;"}));
    CHECK(IsCTypeKeyword("unsigned"));
    CHECK(!IsCTypeKeyword("return"));
    CHECK(IsCIdentStart('_'));
    CHECK(!IsCIdentStart('1'));
    CHECK(IsCIdentChar('1'));
}

}  // namespace

/**
 * @brief Runs every C language server check, aborting on the first failure.
 * @return 0 when every check passed
 */
int main() {
    TestTokenizerBasics();
    TestLineSplice();
    TestUnterminatedLiterals();
    TestNumberValidation();
    TestStrayCharacter();

    TestConditionalArmSelection();
    TestCplusplusGuardIsFalse();
    TestUnbalancedArmLosesToBalancedOne();
    TestHeaderGuardArmIsTaken();
    TestFileDefinedMacroSettlesCondition();
    TestPreprocessorStructureErrors();
    TestMacroRedefinition();

    TestDeclarationShapes();
    TestFunctionShapes();
    TestKandRDefinition();
    TestGnuExtensions();
    TestCastsAndCompoundLiterals();
    TestMacroShapedConstructs();
    TestTypedefAmbiguity();
    TestErrorRecovery();
    TestRecursionCap();
    TestSyntaxErrorMessages();

    TestUndeclaredName();
    TestUnusedVariable();
    TestUnusedStatic();
    TestRedefinition();
    TestLabels();
    TestDuplicates();
    TestShadowing();

    TestAssignInCondition();
    TestBitwisePrecedence();
    TestEmptyIfBody();
    TestStatementHasEffect();
    TestSelfAssignment();
    TestUnreachableCode();
    TestReturnChecks();
    TestCharacterConstants();
    TestFormatArgumentCount();
    TestNoSuchMember();
    TestLineLength();
    TestRealisticFileIsSilent();
    TestRealisticHeaderIsSilent();

    TestResolvedInclude();
    TestCompletionScope();
    TestCompletionKeywordsAndStandardNames();
    TestCompletionMembers();
    TestCompletionDirectives();
    TestCompletionTags();
    TestNoCompletionInStringsAndComments();
    TestHover();
    TestSymbols();
    TestFolds();
    TestDefinition();
    TestReferences();
    TestSignatureHelp();
    TestVocabulary();
    TestKeywordPredicates();

    std::printf("c_lsp_test: all checks passed\n");
    return 0;
}
