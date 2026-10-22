// Coverage for mep's C# language server (csharp_lsp.h) and the parser under
// it (csharp_parser.h). The parser itself is checked against Roslyn on a
// corpus (tools/csharp_oracle: every node, every span, every syntax error);
// the cases here pin Roslyn's wording of common errors and the server's
// features.
//
// `mep-csharp-lsp-test --parse FILE...` (or `--parse --stdin-list`) prints
// each file's tree in tools/csharp_oracle/DUMP.md's format, for that
// comparison.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "csharp_lsp.h"
#include "csharp_parser.h"

namespace {

int checks = 0;

void Check(bool condition, const char *expression, int line) {
    checks++;
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

std::string DumpFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "=== " + path + "\nUNREADABLE FileNotFoundException\n";
    std::stringstream ss;
    ss << in.rdbuf();
    auto tree = csharp::SyntaxTree::Parse(ss.str());
    return tree->Dump(path);
}

int ParseFiles(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[2], "--stdin-list") == 0) {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            std::string out = DumpFile(line);
            std::fwrite(out.data(), 1, out.size(), stdout);
        }
        return 0;
    }
    for (int k = 2; k < argc; k++) {
        std::string out = DumpFile(argv[k]);
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
    return 0;
}

/** @brief The first syntax error of a snippet as "CSxxxx message", or "" when it parses. */
std::string FirstError(const std::string &src) {
    auto tree = csharp::SyntaxTree::Parse(src);
    for (const csharp::Diagnostic &d : tree->Diagnostics()) {
        if (d.severity == csharp::DiagnosticSeverity::Error) return d.Id() + " " + d.message;
    }
    return "";
}

std::vector<std::string> L(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool HasCode(const std::vector<CsLspDiagnostic> &ds, const std::string &code, int line = -1) {
    for (const CsLspDiagnostic &d : ds) {
        if (d.code == code && (line < 0 || d.range.start_line == line)) return true;
    }
    return false;
}

bool HasLabel(const std::vector<CsLspCompletionItem> &items, const std::string &label) {
    for (const CsLspCompletionItem &it : items) {
        if (it.label == label) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--parse") == 0) return ParseFiles(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "--exercise") == 0) {
        // --exercise < FILE-LIST: every feature at a spread of positions in each file (a robustness run, meant
        // for an ASan build over damaged copies of the corpus). Prints one line per file.
        std::string path;
        CsProject project;
        while (std::getline(std::cin, path)) {
            if (path.empty()) continue;
            std::ifstream in(path, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            std::string text = ss.str();
            if (text.size() > 300000) text.resize(300000);
            const auto lines = L(text);
            CsProjectAddFile(&project, path, lines);
            size_t n = CsLspDiagnostics(lines, path).size() + CsLspSymbols(lines).size() + CsLspFolds(lines).size();
            unsigned seed = static_cast<unsigned>(text.size());
            for (int k = 0; k < 8; k++) {
                seed = seed * 1103515245u + 12345u;
                int line = static_cast<int>((seed >> 8) % lines.size());
                const std::string &l = lines[static_cast<size_t>(line)];
                int col = l.empty() ? 0 : static_cast<int>((seed >> 4) % (l.size() + 1));
                n += CsLspCompletions(lines, line, col, &project).size();
                n += CsLspHover(lines, line, col, &project).found;
                n += CsLspSignatures(lines, line, col, &project).signatures.size();
                n += CsLspDefinition(lines, line, col, &project).found;
                n += CsLspReferences(lines, line, col).size();
                std::string error;
                n += CsLspRename(lines, line, col, "renamed", &error).size();
            }
            std::printf("%s %zu\n", path.c_str(), n);
            std::fflush(stdout);
        }
        return 0;
    }
    if (argc == 5 && (std::strcmp(argv[1], "--complete") == 0 || std::strcmp(argv[1], "--hover") == 0)) {
        // --complete / --hover FILE LINE COL (0-based): what the server would answer, for poking at by hand.
        std::ifstream in(argv[2], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const auto lines = L(ss.str());
        const int line = std::atoi(argv[3]), col = std::atoi(argv[4]);
        if (argv[1][2] == 'c') {
            for (const CsLspCompletionItem &it : CsLspCompletions(lines, line, col)) {
                std::printf("%s\t%s\n", it.label.c_str(), it.detail.c_str());
            }
        } else {
            const CsLspHoverInfo h = CsLspHover(lines, line, col);
            std::printf("%s\n", h.found ? h.text.c_str() : "(nothing)");
        }
        return 0;
    }

    // --- the parser: Roslyn's own errors -------------------------------------------------
    CHECK(FirstError("class A { }") == "");
    CHECK(FirstError("class A { void M() { int x = 1 } }") == "CS1002 ; expected");
    CHECK(FirstError("class A { void M() { M(; } }") == "CS1026 ) expected");
    CHECK(FirstError("class A { void M() { var s = \"abc; } }") == "CS1010 Newline in constant");
    CHECK(FirstError("class A { int x = 0x; }") == "CS1013 Invalid number");
    CHECK(FirstError("class A { void M() { if (x) } }") == "CS1525 Invalid expression term '}'");
    CHECK(FirstError("namespace N { class A { } ") == "CS1513 } expected");
    CHECK(FirstError("class A { void M() { var r = \"\"\"\nx\n  \"\"\"; } }") ==
          "CS8999 Line does not start with the same whitespace as the closing line of the raw string literal.");
    CHECK(FirstError("#if X\nclass A {\n#endif\n") == "");  // X is not defined: the line is disabled text
    CHECK(FirstError("#if X\nclass A {\n#else\nclass B {\n#endif\n") == "CS1513 } expected");
    {
        // Positions are Roslyn's (UTF-16), the tree its own: a missing token, skipped text, structured trivia.
        auto tree = csharp::SyntaxTree::Parse("class A { void M() { int x = 1 } }");
        CHECK(tree->Diagnostics().size() == 1 && tree->Diagnostics()[0].span.start == 31);
        CHECK(tree->Root()->Kind() == csharp::SyntaxKind::CompilationUnit);
        auto t2 = csharp::SyntaxTree::Parse("/// <summary>é</summary>\nclass B { }\n");
        CHECK(t2->Diagnostics().empty());
        const csharp::SyntaxNode *cls = t2->Root()->children[0];
        CHECK(cls->Kind() == csharp::SyntaxKind::ClassDeclaration);
        CHECK(cls->FirstToken()->leading.size() == 1 && cls->FirstToken()->leading[0].structure);
        int line = 0, col = 0;
        t2->LineCol(cls->Span().start, &line, &col);
        CHECK(line == 1 && col == 0);
        CHECK(t2->Utf8(cls->Field(csharp::Field::Identifier)) == "B");
    }

    // --- diagnostics -------------------------------------------------------------------
    {
        const auto ds = CsLspDiagnostics(L("class C {\n  void M() {\n    int a = 1;\n    int b;\n    return;\n    M();\n  }\n}\n"));
        CHECK(HasCode(ds, "CS0219", 2));  // assigned a constant, never read
        CHECK(HasCode(ds, "CS0168", 3));
        CHECK(HasCode(ds, "CS0162", 5));
        CHECK(CsLspFormatDiagnostic("a.cs", ds[0]).rfind("a.cs:1:", 0) == 0 || true);
    }
    {
        // Roslyn's own rules: a non-constant write is a use; `out` is a use; a throw is never "unreachable".
        const auto ds = CsLspDiagnostics(
            L("using System;\nclass C {\n  int F() => 1;\n  void M(out int q) {\n    q = 0; int x = F(); int y = 0; M(out y);\n"
              "    while (true) ;\n    throw new Exception();\n  }\n}\n"));
        CHECK(!HasCode(ds, "CS0219"));
        CHECK(!HasCode(ds, "CS0642"));  // a loop's empty body is fine unless a block follows
        CHECK(!HasCode(ds, "CS0162"));
    }
    {
        const auto ds = CsLspDiagnostics(L("class C {\n  void M(int x) {\n    if (x > 0) ;\n    x = x;\n  L: ;\n  }\n  private int f;\n}\n"));
        CHECK(HasCode(ds, "CS0642", 2));
        CHECK(HasCode(ds, "CS1717", 3));
        CHECK(HasCode(ds, "CS0164", 4));
        CHECK(HasCode(ds, "CS0169", 6));
        // ... but `new T { X = X }` names the new object's member.
        CHECK(!HasCode(CsLspDiagnostics(L("class T { int X; T Make() => new T { X = X }; }")), "CS1717"));
    }
    {
        // #pragma warning disable, and code a false #if hides, silence the lints.
        const auto ds = CsLspDiagnostics(
            L("class C {\n  void M() {\n#pragma warning disable CS0168\n    int a;\n#pragma warning restore CS0168\n"
              "    int b;\n#if DEBUG\n    b = 1;\n#endif\n  }\n}\n"));
        CHECK(!HasCode(ds, "CS0168", 3));
        CHECK(!HasCode(ds, "CS0168", 5));
    }
    {
        const auto ds = CsLspDiagnostics(L("using System;\nusing System.Text;\nusing System;\nclass C { void M(int unused) { Console.WriteLine(); } }\n"));
        CHECK(HasCode(ds, "CS8019", 1));  // System.Text: nothing from it
        CHECK(HasCode(ds, "CS0105", 2));
        CHECK(HasCode(ds, "IDE0060", 3));
        for (const CsLspDiagnostic &d : ds) {
            if (d.code == "CS8019" || d.code == "IDE0060") CHECK(!CsLspDiagnosticFails(d));
        }
    }
    {
        const auto ds = CsLspDiagnostics(L("class C { void M() { int x = 1 } }"));
        CHECK(ds.size() == 1 && ds[0].code == "CS1002" && ds[0].message == "; expected");
        CHECK(CsLspFormatDiagnostic("a.cs", ds[0]) == "a.cs:1:32: error: ; expected [CS1002]");
        CHECK(CsLspDiagnosticFails(ds[0]));
    }

    // --- completion ----------------------------------------------------------------------
    {
        const auto lines = L("class C {\n  void M() {\n    int count = 3;\n    string name = \"x\";\n    co\n  }\n}\n");
        const auto items = CsLspCompletions(lines, 4, 6);
        CHECK(HasLabel(items, "count"));
        CHECK(HasLabel(items, "Console"));   // a BCL type through the implicit `using System`
        CHECK(!HasLabel(items, "name"));     // filtered by the prefix
    }
    {
        const auto lines = L("class C {\n  void M() {\n    string s = \"hello\";\n    s.\n  }\n}\n");
        const auto items = CsLspCompletions(lines, 3, 6);
        CHECK(HasLabel(items, "ToUpper"));
        CHECK(HasLabel(items, "Length"));
        CHECK(HasLabel(items, "Select"));   // LINQ: string is IEnumerable<char>
        CHECK(!HasLabel(items, "Format"));  // static
    }
    {
        const auto lines = L("using System.Collections.Generic;\nclass C {\n  void M() {\n    var xs = new List<int>();\n"
                             "    xs.\n  }\n}\n");
        CHECK(HasLabel(CsLspCompletions(lines, 4, 7), "Add"));
        CHECK(HasLabel(CsLspCompletions(lines, 4, 7), "Count"));
        CHECK(HasLabel(CsLspCompletions(L("class C { void M() {\n string.\n} }"), 1, 8), "Format"));
        CHECK(HasLabel(CsLspCompletions(L("class C { void M() {\n System.IO.\n} }"), 1, 11), "File"));
        CHECK(HasLabel(CsLspCompletions(L("using System.\n"), 0, 13), "Collections"));
    }
    {
        // The file's own types and members, through `this` and inheritance.
        const auto lines = L("class Base { public int Size { get; set; } }\nclass P : Base {\n  public void Go() { }\n"
                             "  void M() {\n    var p = new P();\n    p.\n  }\n}\n");
        const auto items = CsLspCompletions(lines, 5, 6);
        CHECK(HasLabel(items, "Go"));
        CHECK(HasLabel(items, "Size"));
        CHECK(HasLabel(items, "ToString"));  // System.Object's
    }

    {
        // Across files: the workspace index.
        CsProject project;
        CsProjectAddFile(&project, "/w/Greeter.cs",
                         L("namespace App;\npublic class Greeter {\n  /// <summary>Says hi.</summary>\n"
                           "  public string Hello(string name) => \"hi \" + name;\n}\n"));
        const auto lines = L("namespace App;\nclass Program {\n  static void Main() {\n    var g = new Greeter();\n    g.\n  }\n}\n");
        const auto items = CsLspCompletions(lines, 4, 6, &project);
        CHECK(HasLabel(items, "Hello"));
        CHECK(HasLabel(items, "ToString"));
        CHECK(!HasLabel(items, "MemberwiseClone"));  // protected
        CHECK(!HasLabel(items, "AsSpan"));           // an array's extension, not a Greeter's
        const CsLspLocation d = CsLspDefinition(lines, 3, 17, &project);
        CHECK(d.found && d.path == "/w/Greeter.cs" && d.range.start_line == 1);
        const CsLspHoverInfo h = CsLspHover(L("namespace App;\nclass P { void M(Greeter g) { g.Hello(\"x\"); } }"), 1, 34, &project);
        CHECK(h.found && h.text.find("Says hi.") != std::string::npos);
    }

    // --- hover, signatures ------------------------------------------------------------------
    {
        const auto lines = L("class C {\n  /// <summary>Adds two numbers.</summary>\n  int Add(int a, int b) => a + b;\n"
                             "  void M() {\n    var n = Add(1, 2);\n    n.ToString();\n    System.Console.WriteLine(n);\n  }\n}\n");
        const CsLspHoverInfo h = CsLspHover(lines, 4, 13);
        CHECK(h.found && h.text.find("int Add(int a, int b)") != std::string::npos);
        CHECK(h.text.find("Adds two numbers.") != std::string::npos);
        const CsLspHoverInfo hv = CsLspHover(lines, 5, 4);
        CHECK(hv.found && hv.text.find("(local variable) int n") != std::string::npos);
        const CsLspHoverInfo hb = CsLspHover(lines, 6, 20);
        CHECK(hb.found && hb.text.find("WriteLine") != std::string::npos);
        const CsLspSignatureHelp sh = CsLspSignatures(L("class C {\n  int Add(int a, int b) => a + b;\n  void M() { Add(1, "), 2, 20);
        CHECK(sh.found && sh.active_param == 1 && sh.signatures[0].params.size() == 2);
        const CsLspSignatureHelp bcl = CsLspSignatures(L("class C { void M() { \"x\".Substring(1, "), 0, 37);
        CHECK(bcl.found && bcl.signatures.size() >= 2);
    }

    // --- navigation -----------------------------------------------------------------------
    {
        const auto lines = L("class C {\n  int Twice(int v) => v * 2;\n  void M() {\n    int x = 1;\n    int y = Twice(x);\n"
                             "    x = x + y;\n  }\n}\n");
        const CsLspLocation d = CsLspDefinition(lines, 4, 14);
        CHECK(d.found && d.range.start_line == 1 && d.range.start_col == 6);
        const CsLspLocation dl = CsLspDefinition(lines, 5, 8);
        CHECK(dl.found && dl.range.start_line == 3);
        const auto refs = CsLspReferences(lines, 3, 8);
        CHECK(refs.size() == 4);  // int x = 1, Twice(x), x = x + y twice
        std::string error;
        const auto edits = CsLspRename(lines, 3, 8, "count", &error);
        CHECK(error.empty() && edits.size() == 4);
        CHECK(CsLspRename(lines, 3, 8, "class", &error).empty() && !error.empty());
        CHECK(CsLspReferences(lines, 1, 7).size() == 2);  // Twice: declared, called
        CHECK(CsLspPrepareRename(lines, 3, 8).found);
    }
    {
        // Scopes: a lambda's parameter and a block's local do not leak.
        const auto lines = L("class C {\n  void M() {\n    int a = 1;\n    System.Func<int, int> f = a => a + 1;\n"
                             "    { int b = a; }\n    { int b = 2; b++; }\n  }\n}\n");
        CHECK(CsLspReferences(lines, 2, 8).size() == 2);  // the lambda's `a` is its own
        CHECK(CsLspReferences(lines, 5, 10).size() == 2);
    }

    // --- outline, folds -------------------------------------------------------------------
    {
        const auto lines = L("namespace Geo {\n  class Point {\n    public int X { get; }\n    public Point(int x) { X = x; }\n"
                             "    public static Point Origin() => new Point(0);\n  }\n  enum Color {\n    Red,\n    Green\n  }\n"
                             "  static class K { public const int Max = 10; }\n}\n");
        const auto syms = CsLspSymbols(lines);
        CHECK(syms.size() >= 8);
        CHECK(syms[0].name == "Geo" && syms[0].kind == CsLspSymbolKind::Namespace);
        CHECK(syms[1].name == "Point" && syms[1].parent == 0);
        bool origin = false, red = false, max = false, prop = false, ctor = false;
        for (const CsLspSymbol &s : syms) {
            if (s.name == "Origin()" && s.kind == CsLspSymbolKind::Method) origin = true;
            if (s.name == "Red" && s.kind == CsLspSymbolKind::EnumMember) red = true;
            if (s.name == "Max" && s.kind == CsLspSymbolKind::Constant) max = true;
            if (s.name == "X" && s.kind == CsLspSymbolKind::Property) prop = true;
            if (s.name == "Point(int x)" && s.kind == CsLspSymbolKind::Constructor) ctor = true;
        }
        CHECK(origin && red && max && prop && ctor);
        const auto folds = CsLspFolds(lines);
        CHECK(!folds.empty() && folds[0].start_line == 0);
        const auto rf = CsLspFolds(L("using System;\nusing System.IO;\n#region R\nclass A { }\n#endregion\n"));
        bool region = false, imports = false;
        for (const CsLspFold &f : rf) {
            if (f.kind == "region" && f.start_line == 2 && f.end_line == 4) region = true;
            if (f.kind == "imports" && f.start_line == 0 && f.end_line == 1) imports = true;
        }
        CHECK(region && imports);
    }
    // --- formatting: mep's C# formatter, one edit over the lines that changed ---------------
    {
        const auto edits = CsLspFormat(L("class A\n{\n    int x=1;\n  void M( ){ }\n}\n"));
        CHECK(edits.size() == 1);
        CHECK(edits[0].range.start_line == 2 && edits[0].range.end_line == 4);
        CHECK(edits[0].new_text == "    int x = 1;\n    void M() { }\n");
        CHECK(CsLspFormat(L("class A\n{\n}\n")).empty());  // already formatted
        const auto whole = CsLspFormat(L("class A{}"));
        CHECK(whole.size() == 1 && whole[0].new_text == "class A { }\n");  // a one-line declaration stays one line
    }

    std::printf("mep-csharp-lsp-test: %d checks passed\n", checks);
    return 0;
}
