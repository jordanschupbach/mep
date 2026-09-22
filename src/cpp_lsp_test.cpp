// Windowless test for cpp_lsp.cpp, cpp_lsp_diagnostics.cpp,
// cpp_lsp_features.cpp and the front end under them (cpp_ast.cpp,
// cpp_parse.cpp) -- the analysis and parsing halves of mep's own C++
// language server (`mep-cpp-lsp`, src/cpp_lsp_server.cpp). Drives the
// pure functions directly: no process, no JSON-RPC client, no GL
// context. CHECK(), never assert(): the Release build strips assert()
// entirely.
//
// Three conventions, all borrowed from python_lsp_test.cpp because they
// are what make a linter's test suite worth having:
//   - Diagnostics are asserted by CppLspDiagnostic::code, never by
//     message wording, so the messages stay free to improve.
//   - Every check asserts the *absence* of a diagnostic on the
//     legal-but-suspicious shapes it must stay quiet about, as
//     deliberately as it asserts the true reports. A linter that cries
//     wolf gets switched off, and then its true reports go unseen too.
//   - The parser's job is tested as "this real-shaped code produces no
//     syntax errors at all", because for a C++ front end without a
//     compiler that is the whole ball game: one bad guess about `<` or
//     about an unknown macro and a correct file lights up red.

#include "cpp_lsp.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cpp_ast.h"

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

/** @brief Options that never touch the filesystem, so the suite answers the same in any directory. */
CppLspOptions NoFiles() {
    CppLspOptions opts;
    opts.check_files = false;
    opts.file_name = "test.cpp";
    return opts;
}

std::vector<CppLspDiagnostic> Lint(const Lines &lines) { return CppLspDiagnostics(lines, NoFiles()); }

/** @brief Counts diagnostics carrying a given code. */
size_t CountCode(const std::vector<CppLspDiagnostic> &diags, const std::string &code) {
    size_t n = 0;
    for (const CppLspDiagnostic &d : diags) {
        if (d.code == code) n++;
    }
    return n;
}

bool HasCode(const std::vector<CppLspDiagnostic> &diags, const std::string &code) {
    return CountCode(diags, code) > 0;
}

bool Reports(const Lines &lines, const std::string &code) { return HasCode(Lint(lines), code); }

/** @brief Reports whether the parser found any syntax problem at all. */
bool ParsesCleanly(const Lines &lines) {
    const CppParseResult result = ParseCpp(lines);
    if (!result.errors.empty()) {
        std::fprintf(stderr, "  unexpected: %s at line %d: %s\n", result.errors.front().code.c_str(),
                     result.errors.front().start.line + 1, result.errors.front().message.c_str());
    }
    return result.errors.empty();
}

/** @brief Reports whether any diagnostic at all was produced (used for the "stays quiet" checks). */
bool Silent(const Lines &lines) {
    const std::vector<CppLspDiagnostic> diags = Lint(lines);
    for (const CppLspDiagnostic &d : diags) {
        std::fprintf(stderr, "  unexpected diagnostic: %s at %d:%d %s\n", d.code.c_str(), d.line + 1,
                     d.col_start + 1, d.message.c_str());
    }
    return diags.empty();
}

// --- Tokenizer --------------------------------------------------------

void TestTokenizer() {
    {
        // A raw string may contain anything, including quotes, braces and
        // newlines, and ends only at its own delimiter.
        const Lines lines = {"const char *sql = R\"sql(SELECT \"x\" FROM t WHERE a > 1)sql\";",
                             "const char *json = R\"({\"a\": 1})\";"};
        CHECK(ParsesCleanly(lines));
        std::vector<CppSyntaxError> errors;
        const std::vector<CppToken> tokens = TokenizeCpp(lines, &errors, nullptr);
        CHECK(errors.empty());
        bool found_raw = false;
        for (const CppToken &token : tokens) {
            if (token.kind == CppTokKind::String && token.text.find("SELECT") != std::string::npos) found_raw = true;
        }
        CHECK(found_raw);
    }
    {
        // A raw string spanning lines, and a `//` inside it that is not a
        // comment.
        const Lines lines = {"const char *s = R\"(line one", "// not a comment", ")\";", "int after = 1;"};
        CHECK(ParsesCleanly(lines));
        std::vector<CppComment> comments;
        TokenizeCpp(lines, nullptr, &comments);
        CHECK(comments.empty());
    }
    {
        // Digit separators, every literal suffix, and a character literal
        // that a naive scanner would read as the start of a separator.
        const Lines lines = {"int a = 1'000'000;", "double b = 1.5e-3;", "auto c = 0xFFu;",
                             "auto d = 0b1010'1010;", "char e = '\\'';", "auto f = 42ms;",
                             "auto g = \"text\"sv;"};
        CHECK(ParsesCleanly(lines));
    }
    {
        // A backslash-newline splice inside a string literal and inside a
        // `//` comment: both continue onto the next line.
        const Lines lines = {"const char *s = \"one\\", "two\";", "// a comment continued \\", "still comment",
                             "int after = 1;"};
        const CppParseResult result = ParseCpp(lines);
        CHECK(result.errors.empty());
        CHECK(result.comments.size() == 1);
    }
    {
        std::vector<CppSyntaxError> errors;
        TokenizeCpp({"const char *s = \"never closed;"}, &errors, nullptr);
        CHECK(errors.size() == 1);
        CHECK(errors.front().code == "unterminated-string");
    }
    {
        std::vector<CppSyntaxError> errors;
        TokenizeCpp({"/* never closed", "more text"}, &errors, nullptr);
        CHECK(errors.size() == 1);
        CHECK(errors.front().code == "unterminated-comment");
    }
    {
        // The alternative tokens are the operators they spell.
        const Lines lines = {"bool f(bool a, bool b) { return a and b or not a; }"};
        CHECK(ParsesCleanly(lines));
    }
}

// --- Preprocessor -----------------------------------------------------

void TestPreprocessor() {
    {
        // `__cplusplus` is predefined, so the modern branch is the live
        // one and the legacy branch never reaches the parser.
        const Lines lines = {"#if __cplusplus >= 201703L", "int modern = 1;", "#else",
                             "this line is not even C++", "#endif"};
        CHECK(ParsesCleanly(lines));
        const CppParseResult result = ParseCpp(lines);
        CHECK(result.line_active.size() == lines.size());
        CHECK(result.line_active[1]);
        CHECK(!result.line_active[3]);
    }
    {
        // A macro nothing defines is 0, and the file still parses because
        // only the live branch is handed to the parser.
        const Lines lines = {"#ifdef MEP_FEATURE_X", "int only_with_x = 1;", "#endif", "int always = 2;"};
        CHECK(ParsesCleanly(lines));
        const CppParseResult result = ParseCpp(lines);
        CHECK(!result.line_active[1]);
    }
    {
        // Object-like and function-like expansion, including `#` and `##`.
        const Lines lines = {"#define SIZE 8",
                             "#define JOIN(a, b) a##b",
                             "#define NAME(x) #x",
                             "int buffer[SIZE];",
                             "int JOIN(pre, fix) = 1;",
                             "const char *n = NAME(hello);"};
        CHECK(ParsesCleanly(lines));
        const CppParseResult result = ParseCpp(lines);
        bool found_joined = false;
        bool found_string = false;
        for (const CppToken &token : result.tokens) {
            if (token.kind == CppTokKind::Ident && token.text == "prefix") found_joined = true;
            if (token.kind == CppTokKind::String && token.text == "\"hello\"") found_string = true;
        }
        CHECK(found_joined);
        CHECK(found_string);
    }
    {
        // A variadic macro, with the GNU comma-deletion extension.
        const Lines lines = {"#define LOG(fmt, ...) Print(fmt, ##__VA_ARGS__)", "void f() { LOG(\"x\"); }"};
        CHECK(ParsesCleanly(lines));
    }
    {
        // A macro that opens a brace is expanded, so the file still
        // balances.
        const Lines lines = {"#define BEGIN_NS namespace mep {", "#define END_NS }", "BEGIN_NS", "int x = 1;",
                             "END_NS"};
        CHECK(ParsesCleanly(lines));
    }
    {
        const Lines lines = {"#if 1", "int x = 1;"};
        CHECK(Reports(lines, "pp-unterminated-conditional"));
    }
    {
        CHECK(Reports({"#endif"}, "pp-stray-conditional"));
    }
    {
        const Lines lines = {"#define X 1", "#define X 2", "int y = X;"};
        CHECK(Reports(lines, "pp-macro-redefined"));
    }
    {
        // Redefining a macro with the same body is how a header written
        // twice behaves, and is not a mistake.
        const Lines lines = {"#define X 1", "#define X 1", "int y = X;"};
        CHECK(!Reports(lines, "pp-macro-redefined"));
    }
    {
        // `#error` behind a macro this server cannot know about stays
        // quiet; one it can evaluate for certain is reported.
        CHECK(!Reports({"#ifndef HAVE_ZLIB", "#error \"needs zlib\"", "#endif"}, "pp-error"));
        CHECK(Reports({"#define A 1", "#if A", "#error \"A must not be set\"", "#endif"}, "pp-error"));
    }
}

// --- Parser coverage --------------------------------------------------

void TestParserCoverage() {
    // Every one of these is real C++ that a person writes, and every one
    // of them is a shape an earlier version of this parser got wrong.
    CHECK(ParsesCleanly({"template <class T, class U = std::pair<T, T>>", "struct Holder { T value; };"}));
    CHECK(ParsesCleanly({"template <class... Args>", "void call(Args &&...args) { f(std::forward<Args>(args)...); }"}));
    CHECK(ParsesCleanly({"auto lambda = [&, x = 1](int n) mutable noexcept -> int { return n + x; };"}));
    CHECK(ParsesCleanly({"void f() { for (const auto &[key, value] : map) { use(key, value); } }"}));
    CHECK(ParsesCleanly({"void f() { if (auto it = m.find(k); it != m.end()) { use(it); } }"}));
    CHECK(ParsesCleanly({"class A { public: virtual ~A() = default; A(const A &) = delete; };"}));
    CHECK(ParsesCleanly({"struct P { int x = 0; P() : x(1) {} P(int v) : x(v) {} };"}));
    CHECK(ParsesCleanly({"void (*callback)(int, const char *) = nullptr;"}));
    CHECK(ParsesCleanly({"int Class::*member_pointer = &Class::field;"}));
    CHECK(ParsesCleanly({"size_t const count = other;"}));
    CHECK(ParsesCleanly({"std::vector<int> v(static_cast<size_t>(n) * 2, 0);"}));
    CHECK(ParsesCleanly({"void f() { auto *p = new Node(next, &value); delete p; }"}));
    CHECK(ParsesCleanly({"void f() { ::new (buffer) Widget(std::move(other)); }"}));
    CHECK(ParsesCleanly({"template <class T> concept Addable = requires (T a, T b) { a + b; };"}));
    CHECK(ParsesCleanly({"template <class T> requires Addable<T> T twice(T v) { return v + v; }"}));
    CHECK(ParsesCleanly({"struct S { int a : 3; unsigned b : 5; };"}));
    CHECK(ParsesCleanly({"enum class Color : unsigned char { Red = 1, Green, Blue };"}));
    CHECK(ParsesCleanly({"extern \"C\" { int c_function(const char *name); }"}));
    CHECK(ParsesCleanly({"namespace a::b::c { int x = 1; }"}));
    CHECK(ParsesCleanly({"inline namespace v1 { int x = 1; }"}));
    CHECK(ParsesCleanly({"using Callback = int (*)(const char *, void *);"}));
    CHECK(ParsesCleanly({"void f() { switch (k) { case 1: [[fallthrough]]; case 2: g(); break; default: break; } }"}));
    CHECK(ParsesCleanly({"[[nodiscard]] static inline constexpr int Answer() noexcept { return 42; }"}));
    CHECK(ParsesCleanly({"void f() try { g(); } catch (const std::exception &e) { h(e); }"}));
    CHECK(ParsesCleanly({"struct Base { virtual int f() const = 0; };",
                         "struct Derived final : public Base { int f() const override { return 1; } };"}));
    CHECK(ParsesCleanly({"auto f() -> decltype(auto) { return x; }"}));
    CHECK(ParsesCleanly({"void f() { co_await task(); co_return; }"}));
    CHECK(ParsesCleanly({"int x = a < b && c > d;"}));
    CHECK(ParsesCleanly({"void f() { if (month < 1 || month > 12) return; }"}));
    CHECK(ParsesCleanly({"std::map<std::string, std::vector<int>> table;"}));
    CHECK(ParsesCleanly({"void f(const std::map<std::string, std::string> &macros, int depth = 0);"}));
    CHECK(ParsesCleanly({"struct A { A &operator=(A &&other) noexcept; bool operator==(const A &) const; };"}));
    CHECK(ParsesCleanly({"A::operator bool() const { return valid_; }"}));
    CHECK(ParsesCleanly({"void f() { x = (Type *)ptr; y = (unsigned char)value; z = (T)~mask; }"}));
    CHECK(ParsesCleanly({"struct timeval tv { 0, 20000 };"}));
    CHECK(ParsesCleanly({"void f() { auto n = size_t{8} * 1024; }"}));
    CHECK(ParsesCleanly({"template <class T> auto sum(T... values) { return (values + ...); }"}));
    CHECK(ParsesCleanly({"void f() { std::sort(v.begin(), v.end(), [](int a, int b) { return a < b; }); }"}));
    CHECK(ParsesCleanly({"MEP_EXPORT void api_function(int x);"}));
    CHECK(ParsesCleanly({"class MEP_API Widget { public: Widget(); };"}));
    CHECK(ParsesCleanly({"namespace std GLIBCXX_VISIBILITY(default) { int x; }"}));
    CHECK(ParsesCleanly({"void f(const Locale &loc UNUSED_PARAM) { }"}));
    CHECK(ParsesCleanly({"TEST_F(FixtureName, DoesTheThing) { EXPECT_EQ(1, 2); }"}));
    CHECK(ParsesCleanly({"void f() { Q_UNUSED(x) return; }"}));
    CHECK(ParsesCleanly({"struct S { int values[4] = {1, 2, 3, 4}; };"}));
    CHECK(ParsesCleanly({"Point p = {.x = 1, .y = 2};"}));
    CHECK(ParsesCleanly({"void f() { label: goto label; }"}));
    CHECK(ParsesCleanly({"int f(int (*compare)(const void *, const void *));"}));
    CHECK(ParsesCleanly({"void swap(int (&a)[4], int (&b)[4]);"}));
    CHECK(ParsesCleanly({"template <class T> class Outer { class Inner { void f(); }; };"}));
    CHECK(ParsesCleanly({"static_assert(sizeof(int) == 4, \"unexpected int size\");",
                         "void f() { static_assert(true); }"}));
}

// --- Syntax diagnostics -----------------------------------------------

void TestSyntaxDiagnostics() {
    CHECK(Reports({"int f() { return 1 }"}, "expected-semicolon"));
    CHECK(Reports({"void f( { }"}, "expected-paren"));
    CHECK(Reports({"class C { int x; "}, "expected-brace"));
    CHECK(Reports({"int x = `backtick`;"}, "stray-character"));
    {
        // One syntax error per line, not a pile: a single missing
        // semicolon must not light up the whole file.
        const Lines lines = {"int a = 1", "int b = 2;", "int c = 3;", "int d = 4;"};
        const std::vector<CppLspDiagnostic> diags = Lint(lines);
        CHECK(CountCode(diags, "expected-semicolon") == 1);
    }
    {
        // A file still being typed into keeps its symbols: the `if` above
        // the unfinished line is still in the outline.
        const Lines lines = {"void ready() { }", "void typing() { auto x = compute(", "void after() { }"};
        const std::vector<CppLspSymbol> symbols = CppLspSymbols(lines);
        bool found_ready = false;
        for (const CppLspSymbol &symbol : symbols) {
            if (symbol.name == "ready") found_ready = true;
        }
        CHECK(found_ready);
    }
}

// --- Binding hygiene --------------------------------------------------

void TestUnusedBindings() {
    CHECK(Reports({"void f() { int unused_local = 3; }"}, "unused-variable"));
    CHECK(!Reports({"void f() { int used = 3; g(used); }"}, "unused-variable"));
    CHECK(!Reports({"void f() { int counter = 0; counter++; use(counter); }"}, "unused-variable"));
    // A read-modify-write is a read: a loop guard must not look unread.
    CHECK(!Reports({"void f() { int guard = 0; while (guard++ < 10) { } }"}, "unused-variable"));
    // The index of a subscript is read even when the subscript is
    // assigned to.
    CHECK(!Reports({"void f(int *out) { int idx = 2; out[idx] = 1; }"}, "unused-variable"));
    // A class-typed local may be doing its work in its destructor.
    CHECK(!Reports({"void f() { std::lock_guard<std::mutex> lock(mutex_); use(); }"}, "unused-variable"));
    // Taking its address is a use.
    CHECK(!Reports({"void f() { int value = 0; g(&value); }"}, "unused-variable"));
    CHECK(!Reports({"void f() { [[maybe_unused]] int value = 0; }"}, "unused-variable"));
    // A parameter nobody reads is an interface, not a mistake.
    CHECK(!Reports({"void f(int unused_parameter) { }"}, "unused-variable"));
    // A caught exception nobody reads is idiomatic.
    CHECK(!Reports({"void f() { try { g(); } catch (const Error &e) { } }"}, "unused-variable"));

    CHECK(Reports({"namespace {", "int Helper() { return 1; }", "}", "int main() { return 0; }"},
                  "unused-function"));
    CHECK(!Reports({"namespace {", "int Helper() { return 1; }", "}", "int main() { return Helper(); }"},
                   "unused-function"));
    // A function used only by another function defined *above* it: the
    // second resolution pass is what makes this quiet.
    CHECK(!Reports({"namespace {", "int Caller() { return Helper(); }", "int Helper() { return 1; }", "}",
                    "int main() { return Caller(); }"},
                   "unused-function"));
    // An overload set: the check steps aside rather than guessing which
    // overload the call resolved to.
    CHECK(!Reports({"namespace {", "int Convert(int v) { return v; }", "int Convert(double v) { return 1; }", "}",
                    "int main() { return Convert(1); }"},
                   "unused-function"));
    // Not static: another translation unit may call it.
    CHECK(!Reports({"int Exported() { return 1; }", "int main() { return 0; }"}, "unused-function"));
    CHECK(!Reports({"int main() { return 0; }"}, "unused-function"));
}

void TestShadowing() {
    CHECK(Reports({"void f() { int value = 1; { int value = 2; use(value); } use(value); }"},
                  "shadowed-declaration"));
    // Sequential blocks are not nested: the same name twice is fine.
    CHECK(!Reports({"void f() { { int v = 1; use(v); } { int v = 2; use(v); } }"}, "shadowed-declaration"));
    // A local with the same name as a member or a global is a house style.
    CHECK(!Reports({"int value = 0;", "void f() { int value = 1; use(value); }"}, "shadowed-declaration"));
    // A declaration further down the function does not shadow anything
    // above it -- a lambda parameter is not shadowed by what comes after.
    CHECK(!Reports({"void f() { auto cb = [](int v) { return v; }; int v = 1; use(cb, v); }"},
                   "shadowed-declaration"));
}

// --- Local idioms -----------------------------------------------------

void TestIdiomDiagnostics() {
    CHECK(Reports({"void f() { if (x = compute()) { g(); } }"}, "assignment-in-condition"));
    // The extra parentheses are how a person says they meant it.
    CHECK(!Reports({"void f() { if ((x = compute())) { g(); } }"}, "assignment-in-condition"));
    CHECK(!Reports({"void f() { if (x == compute()) { g(); } }"}, "assignment-in-condition"));

    CHECK(Reports({"void f() { if (ready); { g(); } }"}, "empty-body"));
    CHECK(!Reports({"void f() { if (ready) { } }"}, "empty-body"));
    CHECK(!Reports({"void f() { for (; done();); }"}, "empty-body"));

    CHECK(Reports({"void f() { count = count; }"}, "self-assignment"));
    CHECK(!Reports({"void f() { count = other.count; }"}, "self-assignment"));

    CHECK(Reports({"void f() { if (a == 1) { g(); } else if (a == 1) { h(); } }"}, "duplicate-condition"));
    CHECK(!Reports({"void f() { if (a == 1) { g(); } else if (a == 2) { h(); } }"}, "duplicate-condition"));
    // Two calls are not known to be the same thing twice.
    CHECK(!Reports({"void f() { if (next()) { g(); } else if (next()) { h(); } }"}, "duplicate-condition"));

    CHECK(Reports({"int f() { return 1; g(); }"}, "unreachable-code"));
    CHECK(!Reports({"int f() { return 1; }"}, "unreachable-code"));
    // A label or a case after a `return` is reachable from elsewhere.
    CHECK(!Reports({"int f() { switch (k) { case 1: return 1; case 2: return 2; } return 0; }"},
                   "unreachable-code"));

    CHECK(Reports({"void f() { a == b; }"}, "expression-has-no-effect"));
    CHECK(!Reports({"void f() { a = b; }"}, "expression-has-no-effect"));
    CHECK(!Reports({"void f() { compute() == expected; }"}, "expression-has-no-effect"));
    CHECK(!Reports({"void f() { out << a << b; }"}, "expression-has-no-effect"));
}

// --- Class-shaped mistakes --------------------------------------------

void TestClassDiagnostics() {
    CHECK(Reports({"class Base { public: virtual void f(); void g(); };"}, "non-virtual-destructor"));
    CHECK(!Reports({"class Base { public: virtual ~Base(); virtual void f(); };"}, "non-virtual-destructor"));
    CHECK(!Reports({"class Base { public: virtual ~Base() = default; virtual void f() = 0; };"},
                   "non-virtual-destructor"));
    // A class with no virtual functions needs no virtual destructor.
    CHECK(!Reports({"class Value { public: void f(); };"}, "non-virtual-destructor"));
    // A derived class inherits its base's destructor, which this server
    // cannot see -- so it says nothing.
    CHECK(!Reports({"class D : public B { public: virtual void f(); };"}, "non-virtual-destructor"));
    // A protected destructor is the other correct answer.
    CHECK(!Reports({"class Base { public: virtual void f(); protected: ~Base(); };"}, "non-virtual-destructor"));

    CHECK(Reports({"struct S {", "  int a = 0;", "  int b = 0;", "  S() : b(1), a(2) {}", "};"},
                  "member-init-order"));
    CHECK(!Reports({"struct S {", "  int a = 0;", "  int b = 0;", "  S() : a(1), b(2) {}", "};"},
                   "member-init-order"));
    // A base class initializer comes first and is not a field.
    CHECK(!Reports({"struct S : Base {", "  int a = 0;", "  S() : Base(), a(1) {}", "};"}, "member-init-order"));
}

// --- Name checks ------------------------------------------------------

void TestNameDiagnostics() {
    {
        // A self-contained file: the check is on, and the typo is caught.
        const Lines lines = {"int Helper(int x) { return x; }", "int main() { return Helpr(1); }"};
        CHECK(Reports(lines, "undefined-name"));
        const std::vector<CppLspDiagnostic> diags = Lint(lines);
        bool suggested = false;
        for (const CppLspDiagnostic &d : diags) {
            if (d.code == "undefined-name" && d.message.find("Helper") != std::string::npos) suggested = true;
        }
        CHECK(suggested);
    }
    // Standard names are known without reading a header.
    CHECK(!Reports({"#include <vector>", "std::vector<int> make() { return std::vector<int>(); }"},
                   "undefined-name"));
    // A header this server could not read switches the check off: every
    // name in the file could have come from it.
    CHECK(!Reports({"#include \"somewhere/unknown.h\"", "int main() { return Mystery(1); }"}, "undefined-name"));
    // So does `using namespace` of a namespace this file does not define.
    CHECK(!Reports({"using namespace elsewhere;", "int main() { return Mystery(1); }"}, "undefined-name"));
    // And so does a file that does not parse cleanly: mid-edit is exactly
    // when an invented "undefined name" is worst.
    CHECK(!Reports({"int main() { auto x = Mystery(", "}"}, "undefined-name"));
    // Six unplaceable names is what a file built on something invisible
    // looks like from the inside; the check stands down rather than
    // reporting them one by one.
    CHECK(!Reports({"int main() { return A1() + A2() + A3() + A4() + A5() + A6() + A7(); }"}, "undefined-name"));
    // A macro is a name too.
    CHECK(!Reports({"#define VALUE 3", "int main() { return VALUE; }"}, "undefined-name"));
    // A member function's body sees members declared below it.
    CHECK(!Reports({"struct S { int f() { return field_; } int field_ = 0; };", "int main() { S s; return s.f(); }"},
                   "undefined-name"));

    {
        const Lines lines = {"struct Point { int x = 0; int y = 0; };",
                             "int main() { Point p; return p.z; }"};
        CHECK(Reports(lines, "no-such-member"));
    }
    CHECK(!Reports({"struct Point { int x = 0; int y = 0; };", "int main() { Point p; return p.x; }"},
                   "no-such-member"));
    // A class with a base this server cannot see may have the member.
    CHECK(!Reports({"struct Point : Unknown { int x = 0; };", "int main() { Point p; return p.z; }"},
                   "no-such-member"));
    // A type from outside this file: nothing is known about its members.
    CHECK(!Reports({"int main() { Mystery m; return m.anything; }"}, "no-such-member"));

    CHECK(Reports({"int add(int a, int b) { return a + b; }", "int main() { return add(1); }"},
                  "wrong-argument-count"));
    CHECK(!Reports({"int add(int a, int b) { return a + b; }", "int main() { return add(1, 2); }"},
                   "wrong-argument-count"));
    CHECK(!Reports({"int add(int a, int b = 2) { return a + b; }", "int main() { return add(1); }"},
                   "wrong-argument-count"));
    CHECK(!Reports({"int now(void);", "int main() { return now(); }"}, "wrong-argument-count"));
    CHECK(!Reports({"int sum(int count, ...);", "int main() { return sum(2, 1, 1); }"}, "wrong-argument-count"));
    // An overload set: say nothing.
    CHECK(!Reports({"int f(int a) { return a; }", "int f(int a, int b) { return a + b; }",
                    "int main() { return f(1, 2); }"},
                   "wrong-argument-count"));
    // A template's parameter list is not something to count against.
    CHECK(!Reports({"template <class... T> int f(T... v) { return 0; }", "int main() { return f(1, 2, 3); }"},
                   "wrong-argument-count"));
}

// --- Header-shaped checks ---------------------------------------------

void TestHeaderChecks() {
    CppLspOptions opts = NoFiles();
    opts.file_name = "widget.h";
    {
        const Lines lines = {"class Widget { public: void f(); };"};
        CHECK(HasCode(CppLspDiagnostics(lines, opts), "missing-include-guard"));
    }
    {
        const Lines lines = {"#pragma once", "class Widget { public: void f(); };"};
        CHECK(!HasCode(CppLspDiagnostics(lines, opts), "missing-include-guard"));
    }
    {
        const Lines lines = {"#ifndef WIDGET_H", "#define WIDGET_H", "class Widget { public: void f(); };",
                             "#endif"};
        CHECK(!HasCode(CppLspDiagnostics(lines, opts), "missing-include-guard"));
    }
    {
        const Lines lines = {"#pragma once", "using namespace std;", "class Widget { };"};
        CHECK(HasCode(CppLspDiagnostics(lines, opts), "using-namespace-in-header"));
    }
    {
        // In a .cpp file, `using namespace` is the author's own business.
        const Lines lines = {"using namespace std;", "class Widget { };"};
        CHECK(!HasCode(CppLspDiagnostics(lines, NoFiles()), "using-namespace-in-header"));
    }
    {
        CppLspOptions length = NoFiles();
        length.max_line_length = 20;
        const Lines lines = {"int x = 1;", "int this_line_is_far_too_long_for_the_limit = 2;"};
        CHECK(CountCode(CppLspDiagnostics(lines, length), "line-too-long") == 1);
        CHECK(!HasCode(CppLspDiagnostics(lines, NoFiles()), "line-too-long"));
    }
    {
        // Calibrated to the document: `NULL` is only worth mentioning in
        // a file that uses `nullptr` elsewhere.
        const Lines mixed = {"void *a = nullptr;", "void *b = NULL;"};
        CHECK(HasCode(CppLspDiagnostics(mixed, NoFiles()), "prefer-nullptr"));
        const Lines old_style = {"void *a = NULL;", "void *b = NULL;"};
        CHECK(!HasCode(CppLspDiagnostics(old_style, NoFiles()), "prefer-nullptr"));
    }
}

// --- Completion -------------------------------------------------------

void TestCompletion() {
    {
        // Locals first, and the prefix filters the list.
        const Lines lines = {"void f() {", "  int counter = 0;", "  int count_max = 10;", "  cou", "}"};
        const std::vector<CppLspCompletionItem> items = CppLspCompletions(lines, 3, 5, NoFiles());
        bool has_counter = false;
        bool has_count_max = false;
        for (const CppLspCompletionItem &item : items) {
            if (item.label == "counter") has_counter = true;
            if (item.label == "count_max") has_count_max = true;
            CHECK(item.label.rfind("cou", 0) == 0);
            CHECK(item.insert_text.find('\n') == std::string::npos);
            CHECK(item.replace_start == 2);
            CHECK(item.replace_end == 5);
        }
        CHECK(has_counter);
        CHECK(has_count_max);
    }
    {
        // Members of a class defined in this file, after a `.`.
        const Lines lines = {"struct Point { int x = 0; int y = 0; void Move(int dx, int dy); };",
                             "void f() { Point p; p. }"};
        const std::vector<CppLspCompletionItem> items = CppLspCompletions(lines, 1, 22, NoFiles());
        bool has_x = false;
        bool has_move = false;
        for (const CppLspCompletionItem &item : items) {
            if (item.label == "x") has_x = true;
            if (item.label == "Move") has_move = true;
        }
        CHECK(has_x);
        CHECK(has_move);
    }
    {
        // Through a pointer, and through a base class.
        const Lines lines = {"struct Base { int shared = 0; };", "struct Derived : Base { int own = 0; };",
                             "void f(Derived *d) { d->}"};
        const std::vector<CppLspCompletionItem> items = CppLspCompletions(lines, 2, 24, NoFiles());
        bool has_shared = false;
        bool has_own = false;
        for (const CppLspCompletionItem &item : items) {
            if (item.label == "shared") has_shared = true;
            if (item.label == "own") has_own = true;
        }
        CHECK(has_shared);
        CHECK(has_own);
    }
    {
        // A standard type answers from the built-in vocabulary.
        const Lines lines = {"#include <string>", "void f() { std::string s; s.pu }"};
        const std::vector<CppLspCompletionItem> items = CppLspCompletions(lines, 1, 30, NoFiles());
        bool has_push_back = false;
        for (const CppLspCompletionItem &item : items) {
            if (item.label == "push_back") has_push_back = true;
        }
        CHECK(has_push_back);
    }
    {
        const Lines lines = {"namespace config { int width = 0; int height = 0; }", "int f() { return config::w }"};
        const std::vector<CppLspCompletionItem> items = CppLspCompletions(lines, 1, 26, NoFiles());
        CHECK(!items.empty());
        CHECK(items.front().label == "width");
    }
    {
        // After a `#`, the directives.
        const std::vector<CppLspCompletionItem> items = CppLspCompletions({"#inc"}, 0, 4, NoFiles());
        CHECK(!items.empty());
        CHECK(items.front().label == "include");
    }
    {
        // Inside an angled include, the standard headers.
        const std::vector<CppLspCompletionItem> items = CppLspCompletions({"#include <vec"}, 0, 13, NoFiles());
        CHECK(!items.empty());
        CHECK(items.front().label == "vector");
    }
    {
        // Nothing inside a string or a comment.
        CHECK(CppLspCompletions({"const char *s = \"text here\";"}, 0, 22, NoFiles()).empty());
        CHECK(CppLspCompletions({"// a comment about x"}, 0, 18, NoFiles()).empty());
    }
    {
        // Keywords are offered where a name is.
        const std::vector<CppLspCompletionItem> items = CppLspCompletions({"void f() { ret }"}, 0, 14, NoFiles());
        bool has_return = false;
        for (const CppLspCompletionItem &item : items) {
            if (item.label == "return") has_return = true;
        }
        CHECK(has_return);
    }
}

// --- Hover ------------------------------------------------------------

void TestHover() {
    {
        const Lines lines = {"/// The number of columns the pane shows.", "int column_count = 80;",
                             "int f() { return column_count; }"};
        const CppLspHoverInfo info = CppLspHover(lines, 2, 19, NoFiles());
        CHECK(info.found);
        CHECK(info.text.find("column_count") != std::string::npos);
        CHECK(info.text.find("The number of columns") != std::string::npos);
        CHECK(info.line == 2);
    }
    {
        const Lines lines = {"/** Draws one pane. */", "void DrawPane(int index, bool active);",
                             "void f() { DrawPane(0, true); }"};
        const CppLspHoverInfo info = CppLspHover(lines, 2, 12, NoFiles());
        CHECK(info.found);
        CHECK(info.text.find("DrawPane(int index, bool active)") != std::string::npos);
        CHECK(info.text.find("Draws one pane.") != std::string::npos);
    }
    {
        const CppLspHoverInfo info = CppLspHover({"constexpr int x = 1;"}, 0, 2, NoFiles());
        CHECK(info.found);
        CHECK(info.text.find("compile time") != std::string::npos);
    }
    {
        const Lines lines = {"#define MAX_PANES 8", "int panes[MAX_PANES];"};
        const CppLspHoverInfo info = CppLspHover(lines, 1, 12, NoFiles());
        CHECK(info.found);
        CHECK(info.text.find("#define MAX_PANES 8") != std::string::npos);
    }
    {
        const CppLspHoverInfo info = CppLspHover({"#include <vector>"}, 0, 12, NoFiles());
        CHECK(info.found);
        CHECK(info.text.find("<vector>") != std::string::npos);
    }
    {
        // Nothing inside a string, and nothing on whitespace. (A cursor
        // just *past* a word still means that word, which is why the
        // whitespace case needs two spaces to be about whitespace.)
        CHECK(!CppLspHover({"const char *s = \"vector\";"}, 0, 19, NoFiles()).found);
        CHECK(!CppLspHover({"int x = 1;  "}, 0, 12, NoFiles()).found);
    }
}

// --- Symbols ----------------------------------------------------------

void TestSymbols() {
    const Lines lines = {"#define LIMIT 4",
                         "namespace mep {",
                         "enum class Mode { Read, Write };",
                         "struct Pane {",
                         "  int width = 0;",
                         "  void Draw(int index);",
                         "};",
                         "void Pane::Draw(int index) { }",
                         "int global_count = 0;",
                         "}"};
    const std::vector<CppLspSymbol> symbols = CppLspSymbols(lines);
    bool has_namespace = false;
    bool has_enum = false;
    bool has_enumerator = false;
    bool has_struct = false;
    bool has_field = false;
    bool has_method = false;
    bool has_macro = false;
    int pane_index = -1;
    for (size_t i = 0; i < symbols.size(); i++) {
        const CppLspSymbol &symbol = symbols[i];
        if (symbol.name == "mep" && symbol.kind == 3) has_namespace = true;
        if (symbol.name == "Mode" && symbol.kind == 10) has_enum = true;
        if (symbol.name == "Read" && symbol.kind == 22) has_enumerator = true;
        if (symbol.name == "Pane" && symbol.kind == 23) {
            has_struct = true;
            pane_index = static_cast<int>(i);
        }
        if (symbol.name == "width" && symbol.kind == 8) has_field = true;
        if (symbol.name == "Draw") has_method = true;
        if (symbol.name == "LIMIT") has_macro = true;
    }
    CHECK(has_namespace);
    CHECK(has_enum);
    CHECK(has_enumerator);
    CHECK(has_struct);
    CHECK(has_field);
    CHECK(has_method);
    CHECK(has_macro);
    // The field is nested under the struct, not left at the top level.
    bool field_nested = false;
    for (const CppLspSymbol &symbol : symbols) {
        if (symbol.name == "width" && symbol.parent == pane_index) field_nested = true;
    }
    CHECK(field_nested);
    CHECK(CppLspSymbols({""}).empty());
}

// --- Folding ----------------------------------------------------------

void TestFolding() {
    {
        const Lines lines = {"void f() {", "  int x = 1;", "  if (x) {", "    g();", "  }", "}"};
        const std::vector<CppLspFold> folds = CppLspFolds(lines);
        bool has_function = false;
        bool has_if = false;
        for (const CppLspFold &fold : folds) {
            if (fold.start_line == 0 && fold.end_line == 5) has_function = true;
            if (fold.start_line == 2 && fold.end_line == 4) has_if = true;
        }
        CHECK(has_function);
        CHECK(has_if);
    }
    {
        const Lines lines = {"// one", "// two", "// three", "int x = 1;"};
        const std::vector<CppLspFold> folds = CppLspFolds(lines);
        bool has_comment = false;
        for (const CppLspFold &fold : folds) {
            if (fold.kind == "comment" && fold.start_line == 0 && fold.end_line == 2) has_comment = true;
        }
        CHECK(has_comment);
    }
    {
        const Lines lines = {"#include <a>", "#include <b>", "#include <c>", "int x = 1;"};
        const std::vector<CppLspFold> folds = CppLspFolds(lines);
        bool has_imports = false;
        for (const CppLspFold &fold : folds) {
            if (fold.kind == "imports" && fold.start_line == 0 && fold.end_line == 2) has_imports = true;
        }
        CHECK(has_imports);
    }
    {
        const Lines lines = {"#if defined(A)", "int x = 1;", "#else", "int x = 2;", "#endif"};
        const std::vector<CppLspFold> folds = CppLspFolds(lines);
        bool has_region = false;
        for (const CppLspFold &fold : folds) {
            if (fold.kind == "region" && fold.start_line == 0 && fold.end_line == 4) has_region = true;
        }
        CHECK(has_region);
    }
}

// --- Definition -------------------------------------------------------

void TestDefinition() {
    {
        const Lines lines = {"int Helper(int x) { return x; }", "int main() { return Helper(1); }"};
        const CppLspLocation location = CppLspDefinition(lines, 1, 20, NoFiles());
        CHECK(location.found);
        CHECK(location.path.empty());
        CHECK(location.line == 0);
    }
    {
        // A declaration and a definition: the definition wins.
        const Lines lines = {"void Draw(int index);", "void f() { Draw(1); }", "void Draw(int index) { }"};
        const CppLspLocation location = CppLspDefinition(lines, 1, 12, NoFiles());
        CHECK(location.found);
        CHECK(location.line == 2);
    }
    {
        const Lines lines = {"void f() { int local = 1; use(local); }"};
        const CppLspLocation location = CppLspDefinition(lines, 0, 30, NoFiles());
        CHECK(location.found);
        CHECK(location.col == 15);
    }
    {
        const Lines lines = {"#define LIMIT 4", "int a[LIMIT];"};
        const CppLspLocation location = CppLspDefinition(lines, 1, 7, NoFiles());
        CHECK(location.found);
        CHECK(location.line == 0);
    }
    CHECK(!CppLspDefinition({"int x = 1;  "}, 0, 12, NoFiles()).found);
}

// --- References -------------------------------------------------------

void TestReferences() {
    {
        const Lines lines = {"void f() {", "  int count = 0;", "  count = count + 1;", "  use(count);", "}"};
        const CppLspReferenceSet refs = CppLspReferences(lines, 1, 6);
        CHECK(refs.found);
        CHECK(refs.name == "count");
        CHECK(refs.refs.size() == 4);
        CHECK(refs.refs.front().line == 1);
        CHECK(refs.refs.front().is_write);
        CHECK(refs.rename_blocked_reason.empty());
    }
    {
        // Two locals of the same name in sibling scopes are two different
        // things, and renaming one must not touch the other.
        const Lines lines = {"void f() { { int v = 1; use(v); } { int v = 2; use(v); } }"};
        const CppLspReferenceSet refs = CppLspReferences(lines, 0, 17);
        CHECK(refs.found);
        CHECK(refs.refs.size() == 2);
    }
    {
        const Lines lines = {"#define LIMIT 4", "int a[LIMIT];", "int b[LIMIT];"};
        const CppLspReferenceSet refs = CppLspReferences(lines, 1, 7);
        CHECK(refs.found);
        CHECK(refs.refs.size() == 3);
    }
    CHECK(!CppLspReferences({"int x = 1;  "}, 0, 12).found);
}

// --- Signature help ---------------------------------------------------

void TestSignatureHelp() {
    {
        const Lines lines = {"void Connect(const char *host, int port, bool secure);",
                             "void f() { Connect(\"localhost\", 80, true); }"};
        const CppLspSignature first = CppLspSignatureHelp(lines, 1, 19);
        CHECK(first.found);
        CHECK(first.label.find("Connect(") != std::string::npos);
        CHECK(first.params.size() == 3);
        CHECK(first.active_param == 0);
        const CppLspSignature second = CppLspSignatureHelp(lines, 1, 33);
        CHECK(second.found);
        CHECK(second.active_param == 1);
        const CppLspSignature third = CppLspSignatureHelp(lines, 1, 37);
        CHECK(third.found);
        CHECK(third.active_param == 2);
    }
    {
        // A nested call: the signature is the inner one.
        const Lines lines = {"int Outer(int a);", "int Inner(int b, int c);", "void f() { Outer(Inner(1, 2)); }"};
        const CppLspSignature signature = CppLspSignatureHelp(lines, 2, 26);
        CHECK(signature.found);
        CHECK(signature.label.find("Inner") != std::string::npos);
        CHECK(signature.active_param == 1);
    }
    {
        const Lines lines = {"/// Connects to a host.", "void Connect(const char *host);",
                             "void f() { Connect(\"x\"); }"};
        const CppLspSignature signature = CppLspSignatureHelp(lines, 2, 19);
        CHECK(signature.found);
        CHECK(signature.documentation.find("Connects to a host.") != std::string::npos);
    }
    CHECK(!CppLspSignatureHelp({"int x = 1;"}, 0, 8).found);
}

// --- Vocabulary -------------------------------------------------------

void TestVocabulary() {
    CHECK(!CppLspKeywordVocab().empty());
    CHECK(!CppLspStdVocab().empty());
    CHECK(!CppLspHeaderVocab().empty());
    CHECK(!CppLspDirectiveVocab().empty());
    // Generated from the real headers: these are the names a person
    // types, and none of them may go missing.
    CHECK(CppLspIsKnownName("vector"));
    CHECK(CppLspIsKnownName("string"));
    CHECK(CppLspIsKnownName("unique_ptr"));
    CHECK(CppLspIsKnownName("size_t"));
    CHECK(CppLspIsKnownName("printf"));
    CHECK(CppLspIsKnownName("transform"));
    CHECK(CppLspIsKnownName("nullptr"));
    CHECK(!CppLspIsKnownName("definitely_not_a_standard_name"));
    CHECK(CppLspHeaderForName("vector") == "vector");
    CHECK(CppLspHeaderForName("sort") == "algorithm");
    CHECK(CppLspTypeMembers("std::string") != nullptr);
    CHECK(CppLspTypeMembers("vector") != nullptr);
    CHECK(CppLspTypeMembers("NotAStandardType") == nullptr);
    // Every curated entry has both a signature and a sentence: a table
    // with half its cells empty is worse than no table.
    for (const CppLspVocabEntry &entry : CppLspStdVocab()) {
        CHECK(entry.name != nullptr && entry.name[0] != '\0');
        CHECK(entry.detail != nullptr && entry.detail[0] != '\0');
        CHECK(entry.doc != nullptr && entry.doc[0] != '\0');
    }
    for (const CppLspVocabEntry &entry : CppLspKeywordVocab()) {
        CHECK(entry.detail != nullptr && entry.detail[0] != '\0');
        CHECK(entry.doc != nullptr && entry.doc[0] != '\0');
    }
}

// --- Robustness -------------------------------------------------------

void TestRobustnessAndOddDocuments() {
    CHECK(Lint({""}).empty());
    CHECK(CppLspSymbols({""}).empty());
    CHECK(CppLspFolds({""}).empty());
    CHECK(!CppLspHover({""}, 0, 0, NoFiles()).found);
    CHECK(CppLspCompletions({""}, 0, 0, NoFiles()).size() > 0);  // keywords, at least
    {
        // Deep nesting must truncate, not overflow the stack.
        std::string deep;
        for (int i = 0; i < 5000; i++) deep += "(";
        deep += "1";
        for (int i = 0; i < 5000; i++) deep += ")";
        const Lines lines = {"int x = " + deep + ";"};
        const CppParseResult result = ParseCpp(lines);
        CHECK(result.truncated);
    }
    {
        // Deeply nested braces, the same way.
        std::string open;
        std::string close;
        for (int i = 0; i < 2000; i++) {
            open += "{ ";
            close += "} ";
        }
        const Lines lines = {"void f() " + open + close};
        const CppParseResult result = ParseCpp(lines);
        CHECK(result.truncated);
    }
    {
        // A file that ends in the middle of everything.
        const Lines lines = {"#if 1", "class C {", "  void f() {", "    if (x) {"};
        const CppParseResult result = ParseCpp(lines);
        CHECK(result.unit != nullptr);
    }
    {
        // A recursive macro expands once and stops.
        const Lines lines = {"#define A B", "#define B A", "int x = A;"};
        CHECK(ParseCpp(lines).unit != nullptr);
    }
    {
        // Positions are byte columns, so a non-ASCII comment above a
        // declaration does not shift the columns on the line below it.
        const Lines lines = {"// \xc3\xa9\xc3\xa9\xc3\xa9 comment", "int unused_local_here = 1;",
                             "void f() { int unused = 1; }"};
        const std::vector<CppLspDiagnostic> diags = Lint(lines);
        for (const CppLspDiagnostic &d : diags) {
            CHECK(d.col_start >= 0);
            CHECK(d.line >= 0 && static_cast<size_t>(d.line) < lines.size());
        }
    }
}

// --- A realistic file stays quiet -------------------------------------

void TestRealisticFileIsQuiet() {
    // The whole point, in one test: ordinary, well-written C++ produces
    // nothing at all. Every line here is a shape that made an earlier
    // version of this server speak up when it should not have.
    const Lines lines = {
        "#include <algorithm>",
        "#include <string>",
        "#include <vector>",
        "",
        "namespace mep {",
        "namespace {",
        "",
        "/// Returns the index of the widest entry, or -1 when there is none.",
        "int WidestIndex(const std::vector<std::string> &entries) {",
        "    int best = -1;",
        "    size_t best_width = 0;",
        "    for (size_t i = 0; i < entries.size(); i++) {",
        "        if (entries[i].size() > best_width) {",
        "            best_width = entries[i].size();",
        "            best = static_cast<int>(i);",
        "        }",
        "    }",
        "    return best;",
        "}",
        "",
        "}  // namespace",
        "",
        "class Table {",
        "public:",
        "    explicit Table(std::vector<std::string> rows) : rows_(std::move(rows)) {}",
        "    virtual ~Table() = default;",
        "",
        "    /// The widest row, or an empty string.",
        "    std::string Widest() const {",
        "        const int index = WidestIndex(rows_);",
        "        if (index < 0) return std::string();",
        "        return rows_[static_cast<size_t>(index)];",
        "    }",
        "",
        "    void Sort() { std::sort(rows_.begin(), rows_.end()); }",
        "    size_t Count() const { return rows_.size(); }",
        "",
        "private:",
        "    std::vector<std::string> rows_;",
        "};",
        "",
        "int Run(const std::vector<std::string> &input) {",
        "    Table table(input);",
        "    table.Sort();",
        "    const std::string widest = table.Widest();",
        "    return widest.empty() ? 0 : static_cast<int>(table.Count());",
        "}",
        "",
        "}  // namespace mep",
    };
    CHECK(ParsesCleanly(lines));
    CHECK(Silent(lines));
}

}  // namespace

int main() {
    TestTokenizer();
    TestPreprocessor();
    TestParserCoverage();
    TestSyntaxDiagnostics();
    TestUnusedBindings();
    TestShadowing();
    TestIdiomDiagnostics();
    TestClassDiagnostics();
    TestNameDiagnostics();
    TestHeaderChecks();
    TestCompletion();
    TestHover();
    TestSymbols();
    TestFolding();
    TestDefinition();
    TestReferences();
    TestSignatureHelp();
    TestVocabulary();
    TestRobustnessAndOddDocuments();
    TestRealisticFileIsQuiet();
    std::printf("mep-cpp-lsp-test: all checks passed\n");
    return 0;
}
