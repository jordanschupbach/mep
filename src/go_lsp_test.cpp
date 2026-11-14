// Windowless test for go_lsp.cpp -- the analysis half of mep's own
// Go language server (`mep-go-lsp`). Drives the pure functions
// directly: no process, no JSON-RPC client. CHECK(), never assert(): the
// Release build strips assert().
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <filesystem>
#include <fstream>

#include "go_lsp.h"
#include "go_lsp_pkgdoc.h"
#include "go_parse.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

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

std::vector<GoLspDiagnostic> Diags(const std::string &src) { return GoLspDiagnostics(*GoLspAnalyze(L(src))); }

bool Has(const std::vector<GoLspDiagnostic> &ds, const std::string &code, const std::string &msg, int line = -1) {
    for (const auto &d : ds) {
        if (d.code == code && d.message == msg && (line < 0 || d.range.line == line)) return true;
    }
    return false;
}

void Dump(const std::vector<GoLspDiagnostic> &ds) {
    for (const auto &d : ds) std::fprintf(stderr, "  %d:%d %s [%s]\n", d.range.line, d.range.col, d.message.c_str(), d.code.c_str());
}

// A realistic file that compiles: no diagnostics at all.
const char *kClean = R"(// Package demo shows things.
package demo

import (
	"errors"
	"fmt"
	"strings"
	str "strconv"
)

// Point is a point.
type Point struct {
	X, Y int
	name string
}

type Shape interface {
	Area() float64
}

// Dist returns the squared distance.
func (p *Point) Dist(q Point) int {
	dx, dy := p.X-q.X, p.Y-q.Y
	return dx*dx + dy*dy
}

var ErrBad = errors.New("bad")

func Parse(s string) (Point, error) {
	parts := strings.Split(s, ",")
	if len(parts) != 2 {
		return Point{}, fmt.Errorf("want 2 parts: %w", ErrBad)
	}
	x, err := str.Atoi(parts[0])
	if err != nil {
		return Point{}, err
	}
	y, err := str.Atoi(parts[1])
	if err != nil {
		return Point{}, err
	}
	return Point{X: x, Y: y}, nil
}

func Sum(xs ...int) (total int) {
outer:
	for i, x := range xs {
		switch {
		case x < 0:
			continue outer
		case x == 0:
			fallthrough
		case i > 100:
			break outer
		default:
			total += x
		}
	}
	for {
		select {
		default:
			return
		}
	}
}

func Kind(v any) string {
	switch t := v.(type) {
	case int:
		return fmt.Sprint(t + 1)
	default:
		panic("unknown")
	}
}

func Loop() int {
	for {
	}
}

func Apply(f func(int) int, n int) int {
	g := func(x int) int { return f(x) * 2 }
	m := map[string]Point{"a": {X: 1}}
	_ = m
	var arr [3]int
	arr[0] = n
	return g(arr[0])
}
)";

void TestSyntaxErrors() {
    auto ds = Diags("package main\nfunc main() { x := 1 +; }\n");
    CHECK(!ds.empty());
    CHECK(ds[0].code == "syntax-error" && ds[0].message == "expected operand, found ';'");
    CHECK(ds[0].range.line == 1 && ds[0].range.col == 22);
    // a file with syntax errors gets no use-based checks
    auto ds2 = Diags("package main\nimport \"fmt\"\nfunc main() { x := 1; )\n");
    for (const auto &d : ds2) CHECK(d.code == "syntax-error");
}

void TestClean() {
    auto ds = Diags(kClean);
    if (!ds.empty()) Dump(ds);
    CHECK(ds.empty());
}

void TestUnused() {
    auto ds = Diags("package p\nimport (\n\t\"fmt\"\n\tos2 \"os\"\n\t_ \"embed\"\n)\nfunc f() {\n\ta := 1\n\tb := 2\n\tb++\n"
                    "\tc := 0\n\tc = 3\n\tfor i := range 3 {\n\t}\n\tswitch t := any(1).(type) {\n\tcase int:\n\t}\n"
                    "\tg := 1\n\tgo func() { g = 2 }()\n}\n");
    CHECK(Has(ds, "unused-import", "\"fmt\" imported and not used", 2));
    CHECK(Has(ds, "unused-import", "\"os\" imported as os2 and not used", 3));
    CHECK(Has(ds, "unused-variable", "declared and not used: a", 7));
    CHECK(!Has(ds, "unused-variable", "declared and not used: b"));
    CHECK(Has(ds, "unused-variable", "declared and not used: c", 10));
    CHECK(Has(ds, "unused-variable", "declared and not used: i", 12));
    CHECK(Has(ds, "unused-variable", "t declared and not used", 14));
    CHECK(Has(ds, "unused-variable", "declared and not used: g", 17));
    for (const auto &d : ds) {
        if (d.code == "unused-variable") CHECK(d.unnecessary);
    }
    // a non-standard import whose name we can only guess is not reported while
    // the file uses an unresolved package-like name
    auto ds2 = Diags("package p\nimport \"example.com/go-thing\"\nfunc f() { thingy.Do() }\n");
    CHECK(!Has(ds2, "unused-import", "\"example.com/go-thing\" imported and not used"));
    auto ds3 = Diags("package p\nimport \"example.com/go-thing/v2\"\nfunc f() { thing.Do() }\n");
    CHECK(ds3.empty());
}

void TestControlFlow() {
    auto ds = Diags("package p\nfunc f() int {\n\tif true {\n\t\treturn 1\n\t}\n}\n");
    CHECK(Has(ds, "missing-return", "missing return", 5));
    auto ds2 = Diags("package p\nfunc f() {\n\tbreak\n\tfor {\n\t\tcontinue\n\t}\n\tcontinue\nL:\n\tgoto M\n}\n");
    CHECK(Has(ds2, "misplaced-branch", "break is not in a loop, switch, or select", 2));
    CHECK(Has(ds2, "misplaced-branch", "continue is not in a loop", 6));
    CHECK(Has(ds2, "undefined-label", "label M not defined", 8));
    CHECK(Has(ds2, "unused-label", "label L defined and not used", 7));
    auto ds3 = Diags("package p\nfunc f(x int) {\n\tswitch x {\n\tcase 1:\n\t\tfallthrough\n\t}\n\tfallthrough\n}\n");
    CHECK(Has(ds3, "misplaced-branch", "cannot fallthrough final case in switch", 4));
    CHECK(Has(ds3, "misplaced-branch", "fallthrough statement out of place", 6));
    auto ds4 = Diags("package p\nfunc f() int {\n\treturn 1\n\tprintln()\n}\n");
    CHECK(Has(ds4, "unreachable", "unreachable code", 3));
    auto ds5 = Diags("package p\nfunc f() {\n\tx := 1\n\tx := 2\n\t_ = x\n\ty := 1\n\ty = y\n}\n");
    CHECK(Has(ds5, "no-new-variables", "no new variables on left side of :=", 3));
    CHECK(Has(ds5, "self-assignment", "self-assignment of y to y", 6));
}

void TestStdlibChecks() {
    auto ds = Diags("package p\nimport (\n\t\"fmt\"\n\t\"io/ioutil\"\n)\nfunc f() {\n\tfmt.Printlnx(1)\n\t_, _ = ioutil.ReadAll(nil)\n}\n");
    CHECK(Has(ds, "undefined", "undefined: fmt.Printlnx", 6));
    bool dep = false;
    for (const auto &d : ds) {
        if (d.code == "deprecated" && d.message.rfind("ioutil.ReadAll is deprecated", 0) == 0) dep = d.deprecated;
    }
    CHECK(dep);
}

int LineOf(const std::vector<std::string> &lines, const std::string &needle, int *col) {
    for (size_t i = 0; i < lines.size(); i++) {
        size_t c = lines[i].find(needle);
        if (c != std::string::npos) {
            *col = static_cast<int>(c);
            return static_cast<int>(i);
        }
    }
    *col = -1;
    return -1;
}

void TestHoverAndNavigation() {
    const auto lines = L(kClean);
    const auto doc = GoLspAnalyze(lines);
    int col = 0;
    int line = LineOf(lines, "strings.Split(s", &col);
    auto h = GoLspHover(*doc, line, col + 9);
    CHECK(h.found && h.text.find("func Split(s, sep string) []string") != std::string::npos);
    CHECK(h.text.find("pkg.go.dev/strings#Split") != std::string::npos);
    line = LineOf(lines, "x, err := str.Atoi", &col);
    h = GoLspHover(*doc, line, col);
    CHECK(h.found && h.text.find("var x int") != std::string::npos);  // typed through strconv.Atoi
    line = LineOf(lines, "return Point{X: x", &col);
    h = GoLspHover(*doc, line, col + 8);
    CHECK(h.found && h.text.find("type Point struct") != std::string::npos && h.text.find("Point is a point.") != std::string::npos);
    // definition of Point from its use
    auto defs = GoLspDefinition(*doc, line, col + 8);
    CHECK(defs.size() == 1 && defs[0].range.line == 11);
    // references of `parts`
    line = LineOf(lines, "parts := strings", &col);
    auto refs = GoLspReferences(*doc, line, col);
    CHECK(refs.size() == 4);
    // field access through a method receiver
    line = LineOf(lines, "dx, dy := p.X", &col);
    h = GoLspHover(*doc, line, col + 12);
    CHECK(h.found && h.text.find("field X int") != std::string::npos);
    defs = GoLspDefinition(*doc, line, col + 12);
    CHECK(defs.size() == 1 && defs[0].range.line == 12);
    // rename a local; refuse a package-level name
    std::string err;
    line = LineOf(lines, "parts := strings", &col);
    auto edits = GoLspRename(*doc, line, col, "fields", &err);
    CHECK(edits.size() == 4 && edits[0].new_text == "fields");
    line = LineOf(lines, "func Parse", &col);
    CHECK(GoLspRename(*doc, line, col + 5, "Parse2", &err).empty() && !err.empty());
    CHECK(!GoLspPrepareRename(*doc, line, col + 5).found);
}

bool HasItem(const std::vector<GoLspCompletionItem> &items, const std::string &label) {
    for (const auto &it : items) {
        if (it.label == label) return true;
    }
    return false;
}

void TestCompletion() {
    auto doc = GoLspAnalyze(L("package p\nimport \"strings\"\nfunc f() {\n\tstrings.Spl\n}\n"));
    auto items = GoLspCompletions(*doc, 3, 12);
    CHECK(HasItem(items, "Split") && HasItem(items, "SplitN") && !HasItem(items, "Join"));
    CHECK(items[0].replace_start == 9 && items[0].replace_end == 12);
    // members of a local struct value, and of a standard-library type
    doc = GoLspAnalyze(L("package p\nimport \"strings\"\ntype T struct{ Alpha, Beta int }\nfunc (T) Gamma() {}\n"
                         "func f() {\n\tt := T{}\n\tvar b strings.Builder\n\tb.Wr\n\tt.\n}\n"));
    items = GoLspCompletions(*doc, 8, 3);
    CHECK(HasItem(items, "Alpha") && HasItem(items, "Beta") && HasItem(items, "Gamma"));
    items = GoLspCompletions(*doc, 7, 5);
    CHECK(HasItem(items, "WriteString") && HasItem(items, "WriteByte"));
    // names in scope, builtins, keywords
    doc = GoLspAnalyze(L("package p\nfunc f(count int) {\n\tcounter := 1\n\t_ = counter\n\tco\n}\n"));
    items = GoLspCompletions(*doc, 4, 3);
    CHECK(HasItem(items, "count") && HasItem(items, "counter") && HasItem(items, "copy") && HasItem(items, "continue"));
    // import paths
    doc = GoLspAnalyze(L("package p\nimport (\n\t\"net/ht\"\n)\n"));
    items = GoLspCompletions(*doc, 2, 8);
    CHECK(HasItem(items, "net/http") && HasItem(items, "net/http/httptest"));
}

void TestSignature() {
    auto doc = GoLspAnalyze(L("package p\nimport \"strings\"\nfunc g(a int, b string) {}\nfunc f() {\n"
                              "\tstrings.Replace(\"x\", \n\tg(1, \n}\n"));
    auto sh = GoLspSignatureAt(*doc, 4, 22);
    CHECK(sh.found && sh.label.rfind("Replace(", 0) == 0 && sh.params.size() == 4 && sh.active_param == 1);
    CHECK(sh.params[0] == "s" || sh.params[0].find("s") == 0);
    sh = GoLspSignatureAt(*doc, 5, 6);
    CHECK(sh.found && sh.params.size() == 2 && sh.params[1] == "b string" && sh.active_param == 1);
}

void TestOutline() {
    const auto doc = GoLspAnalyze(L(kClean));
    auto syms = GoLspSymbols(*doc);
    bool point = false, dist = false, field = false;
    for (size_t i = 0; i < syms.size(); i++) {
        if (syms[i].name == "Point" && syms[i].kind == GoLspSymbolKind::Struct) point = true;
        if (syms[i].name == "(*Point).Dist" && syms[i].kind == GoLspSymbolKind::Method) dist = true;
        if (syms[i].name == "name" && syms[i].parent >= 0 && syms[static_cast<size_t>(syms[i].parent)].name == "Point") field = true;
    }
    CHECK(point && dist && field);
    auto folds = GoLspFolds(*doc);
    bool imports = false;
    for (const auto &f : folds) {
        if (f.kind == "imports" && f.start_line == 3) imports = true;
    }
    CHECK(imports && folds.size() > 8);
}

void TestParserLimits() {
    std::string deep = "package p\nvar x = " + std::string(20000, '(') + "1" + std::string(20000, ')') + "\n";
    const goparse::Result r = goparse::ParseFile(deep);
    CHECK(!r.errors.empty() && r.errors[0].msg == "exceeded max nesting depth");
    const auto doc = GoLspAnalyze(L(deep));
    CHECK(!GoLspDiagnostics(*doc).empty());
    // Unicode identifiers and positions in bytes
    const goparse::Result u = goparse::ParseFile("package p\nvar é = 1 +\n");
    CHECK(u.errors.size() == 2 && u.errors[0].line == 2 && u.errors[0].col == 14);  // byte columns: é is 2 bytes
    std::string out;
    CHECK(goparse::Unquote("\"a\\tb\\u00e9\"", &out) && out == "a\tb\xc3\xa9");
    CHECK(goparse::QuoteRune('\n') == "'\\n'" && goparse::QuoteRune(0x201C) == "'\xe2\x80\x9c'");
}

void TestRobustness() {
    // Every query on every prefix of a file must not crash.
    const std::string src = kClean;
    for (size_t n = 0; n <= src.size(); n += 7) {
        const auto lines = L(src.substr(0, n));
        const auto doc = GoLspAnalyze(lines);
        const int last = static_cast<int>(lines.size()) - 1;
        const int col = static_cast<int>(lines.back().size());
        GoLspDiagnostics(*doc);
        GoLspCompletions(*doc, last, col);
        GoLspHover(*doc, last, col);
        GoLspSignatureAt(*doc, last, col);
        GoLspDefinition(*doc, last, col);
        GoLspReferences(*doc, last, col);
        GoLspSymbols(*doc);
        GoLspFolds(*doc);
    }
}


bool Has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

void WriteText(const std::filesystem::path &p, const std::string &text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

void TestPackageDocs() {
    namespace fs = std::filesystem;
    // go.mod: module, require and replace, single lines and blocks, comments and quotes.
    const gopkgdoc::Module m = gopkgdoc::ParseGoMod(
        "module example.com/app // the app\n\ngo 1.22\n\nrequire (\n\texample.com/datamunge v0.0.0\n"
        "\t\"example.com/Ext\" v1.2.0 // indirect\n)\nrequire golang.org/x/text v0.3.0\n"
        "replace example.com/datamunge => ../datamunge\nreplace (\n\told.com/a v1.0.0 => new.com/a v1.1.0\n)\n");
    CHECK(m.ok && m.path == "example.com/app");
    CHECK(m.require.size() == 3 && m.require[1].first == "example.com/Ext" && m.require[2].second == "v0.3.0");
    CHECK(m.replace.size() == 2 && m.replace[0].new_path == "../datamunge" && m.replace[1].old_version == "v1.0.0" &&
          m.replace[1].new_path == "new.com/a" && m.replace[1].new_version == "v1.1.0");
    CHECK(gopkgdoc::EscapeModulePath("github.com/BurntSushi/toml") == "github.com/!burnt!sushi/toml");

    const fs::path root = fs::temp_directory_path() / ("mep-gopkgdoc-test-" + std::to_string(std::rand()));
    WriteText(root / "app/go.mod",
              "module example.com/app\n\nrequire (\n\texample.com/datamunge v0.0.0\n\texample.com/Ext v1.2.0\n)\n"
              "replace example.com/datamunge => ../datamunge\n");
    const std::string main_go =
        "package main\n\nimport (\n\t\"fmt\"\n\n\tdm \"example.com/datamunge\"\n\t\"example.com/Ext/sub\"\n"
        "\t\"example.com/app/internal/util\"\n)\n\n// helper doubles x.\nfunc helper(x int) int { return 2 * x }\n\n"
        "func main() {\n\tf := dm.NewFrame(3)\n\tfmt.Println(f.Rows(), helper(2), sub.Answer, util.Twice(1))\n}\n";
    WriteText(root / "app/main.go", main_go);
    WriteText(root / "app/main_test.go", "package main\n\n// TestOnly is not documentation.\nfunc TestOnly() {}\n");
    WriteText(root / "app/internal/util/util.go", "// Package util helps.\npackage util\n\n// Twice is 2x.\nfunc Twice(x int) int { return 2 * x }\n");
    WriteText(root / "datamunge/go.mod", "module example.com/datamunge\n");
    WriteText(root / "datamunge/frame.go",
              "// Package datamunge wraps the datamunge C++ library.\npackage datamunge\n\n"
              "// Frame is a table of columns.\n//\n// Example:\n//\n//\tf := NewFrame(3)\ntype Frame struct {\n"
              "\t// Name labels the frame.\n\tName string\n\trows int\n}\n\n"
              "// NewFrame makes a frame of n rows.\nfunc NewFrame(n int) *Frame { return &Frame{rows: n} }\n\n"
              "// Rows returns the number of rows.\nfunc (f *Frame) Rows() int { return f.rows }\n\n"
              "// Kinds of join.\nconst (\n\tInner = iota // rows in both\n\t// Outer keeps all rows.\n\tOuter\n)\n\n"
              "func internal() {}\n");
    WriteText(root / "gopath/pkg/mod/example.com/!ext@v1.2.0/sub/sub.go",
              "// Package sub is a dependency.\npackage sub\n\n// Answer is the answer.\nvar Answer = 42\n");
    const std::string app = (root / "app").string();
#if !defined(_WIN32)
    setenv("GOMODCACHE", (root / "gopath/pkg/mod").string().c_str(), 1);
    unsetenv("GOFLAGS");
#endif
    // Import paths: the main module, a local replace, the module cache (escaped), nothing.
    std::string why;
    CHECK(gopkgdoc::ResolveImport("example.com/app/internal/util", app, &why) == app + "/internal/util" &&
          why == "main module");
    CHECK(Has(gopkgdoc::ResolveImport("example.com/datamunge", app, &why), "datamunge") && Has(why, "replace"));
    CHECK(Has(gopkgdoc::ResolveImport("example.com/Ext/sub", app, &why), "!ext@v1.2.0/sub") && why == "module cache");
    CHECK(gopkgdoc::ResolveImport("example.com/nowhere", app).empty());

    // The package: exported names only, tests skipped, docs and positions.
    const gopkgdoc::Package dm = gopkgdoc::ReadPackage((root / "datamunge").string());
    CHECK(dm.ok && dm.name == "datamunge" && dm.doc == "Package datamunge wraps the datamunge C++ library.\n");
    const std::string table = gopkgdoc::ExportsTable(dm);
    CHECK(Has(table, "type\tFrame\ttype Frame struct { ... }\t"));
    CHECK(Has(table, "field\tFrame.Name\tName string\t"));
    CHECK(Has(table, "func\tNewFrame\tfunc NewFrame(n int) *Frame\t") && Has(table, "frame.go:16\n"));
    CHECK(Has(table, "method\tFrame.Rows\tfunc (f *Frame) Rows() int\t"));
    CHECK(Has(table, "const\tInner\tconst Inner = iota\t") && Has(table, "const\tOuter\tconst Outer\t"));
    CHECK(!Has(table, "internal") && !Has(table, "rows"));
    CHECK(!Has(gopkgdoc::ExportsTable(gopkgdoc::ReadPackage(app)), "TestOnly"));

    // Doc, in go doc's forms.
    const std::vector<std::pair<std::string, std::string>> ctx = {{app + "/main.go", main_go}};
    std::string d = gopkgdoc::Doc("dm.NewFrame", app, ctx);
    CHECK(d.find("```go\nfunc NewFrame(n int) *Frame\n```\n\nfunc in package `example.com/datamunge`\n\nNewFrame makes a frame of n rows.") == 0);
    d = gopkgdoc::Doc("dm.Frame", app);
    CHECK(Has(d, "Frame is a table of columns.") && Has(d, "```go\nf := NewFrame(3)\n```"));
    CHECK(Has(d, "```go\nfunc NewFrame(n int) *Frame\nfunc (f *Frame) Rows() int\n```"));
    CHECK(Has(gopkgdoc::Doc("dm.Frame.Rows", app), "Rows returns the number of rows."));
    CHECK(Has(gopkgdoc::Doc("dm.frame.rows", app), "Rows returns the number of rows."));  // case-insensitive
    CHECK(Has(gopkgdoc::Doc("dm.Outer", app), "Outer keeps all rows."));
    CHECK(Has(gopkgdoc::Doc("dm.Inner", app), "rows in both"));
    CHECK(Has(gopkgdoc::Doc("dm.Frame.Name", app), "Name labels the frame."));
    CHECK(Has(gopkgdoc::Doc("sub.Answer", app), "Answer is the answer."));
    CHECK(Has(gopkgdoc::Doc("util.Twice", app), "Twice is 2x."));
    CHECK(Has(gopkgdoc::Doc("helper", app), "helper doubles x.") && Has(gopkgdoc::Doc("helper", app), "package `example.com/app`"));
    CHECK(Has(gopkgdoc::Doc("example.com/datamunge.Frame.Rows", app), "Rows returns"));
    d = gopkgdoc::Doc("dm", app);
    CHECK(Has(d, "package datamunge // import \"example.com/datamunge\"") && Has(d, "const Outer\n"));
    CHECK(gopkgdoc::Doc("dm.Nothing", app).empty() && gopkgdoc::Doc("nothing_at_all", app).empty());
    fs::remove_all(root);
}

// Members of typed values (hover on `x.Method`): types inferred from declarations and documented
// results, as in Rosetta's cgo binding of datamunge (calls return (T, error), a generic Must,
// derived classes embed their bases, a policy subpackage).
void TestMemberDocs() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("mep-gomember-test-" + std::to_string(std::rand()));
    WriteText(root / "app/go.mod", "module app\n\ngo 1.24\n\nrequire datamunge v0.0.0\n\nreplace datamunge => ../datamunge\n");
    WriteText(root / "datamunge/go.mod", "module datamunge\n\ngo 1.24\n");
    WriteText(root / "datamunge/datamunge.go",
              "// Package datamunge is generated.\npackage datamunge\n\n"
              "// Must panics on the error.\nfunc Must[T any](v T, err error) T { return v }\n\n"
              "// Frame is a table.\ntype Frame struct {\n\tref int\n}\n\n"
              "// NewFrame makes a frame.\nfunc NewFrame() (*Frame, error) { return &Frame{}, nil }\n\n"
              "// Select keeps the named columns.\nfunc (f *Frame) Select(columns []string) (*Frame, error) { return f, nil }\n\n"
              "// Nrows counts the rows.\nfunc (f *Frame) Nrows() (int, error) { return 0, nil }\n\n"
              "// Plot is a figure.\ntype Plot struct {\n\tref int\n}\n\n"
              "// SaveSvg writes the figure.\nfunc (p *Plot) SaveSvg(path string) error { return nil }\n\n"
              "// RPlot is an R-style plot.\ntype RPlot struct {\n\tPlot\n}\n\n"
              "// RPlotCreate starts a plot.\nfunc RPlotCreate() (*RPlot, error) { return &RPlot{}, nil }\n\n"
              "// Title sets the title.\nfunc (r *RPlot) Title(value string) (*RPlot, error) { return r, nil }\n");
    WriteText(root / "datamunge/data/data.go",
              "// Package data holds datasets.\npackage data\n\nimport (\n\tdatamunge \"datamunge\"\n)\n\n"
              "// Iris is the iris data.\nfunc Iris() (*datamunge.Frame, error) { return nil, nil }\n");
    const std::string main_go =
        "package main\n\nimport (\n\t\"datamunge\"\n\t\"datamunge/data\"\n)\n\n"
        "func show(p *datamunge.RPlot) {}\n\n"
        "func main() {\n"
        "\tdf, err := datamunge.NewFrame()\n"
        "\tif err != nil {\n\t\tpanic(err)\n\t}\n"
        "\tm := datamunge.Must(datamunge.NewFrame())\n"
        "\tiris, _ := data.Iris()\n"
        "\tplot := datamunge.Must(datamunge.RPlotCreate(\n\t))\n"
        "\tvar q *datamunge.RPlot\n"
        "\tsel := datamunge.Must(m.Select([]string{\"a)\"}))\n"
        "}\n";
    WriteText(root / "app/main.go", main_go);
    const std::string app = (root / "app").string();
    const std::vector<std::pair<std::string, std::string>> ctx = {{app + "/main.go", main_go}};
    auto member = [&](const std::string &expr, const std::string &name) {
        return gopkgdoc::MemberDoc(expr, name, app, ctx, main_go);
    };
    CHECK(Has(member("df", "Select"), "func (f *Frame) Select(columns []string) (*Frame, error)"));  // x, err := pkg.F()
    CHECK(Has(member("df", "Select"), "Select keeps the named columns."));
    CHECK(Has(member("m", "Nrows"), "Nrows counts the rows."));                     // x := pkg.Must(pkg.F())
    CHECK(Has(member("iris", "Nrows"), "Nrows counts the rows."));                  // a subpackage's qualified result
    CHECK(Has(member("plot", "Title"), "Title sets the title."));                   // a call over two lines
    CHECK(Has(member("plot", "SaveSvg"), "SaveSvg writes the figure."));            // promoted from the embedded Plot
    CHECK(Has(member("q", "SaveSvg"), "SaveSvg writes the figure."));               // var q *pkg.T
    CHECK(Has(member("p", "Title"), "Title sets the title."));                      // a parameter
    CHECK(Has(member("sel", "Nrows"), "Nrows counts the rows."));                   // Must(x.M(...)) with ")" in a string
    CHECK(Has(member("datamunge.Must(m.Select([]string{\"x\"}))", "Nrows"), "Nrows")); // a call chain
    CHECK(Has(member("datamunge.Must(plot.Title(\"t\"))", "SaveSvg"), "SaveSvg"));
    CHECK(Has(member("m.Select(nil)", "Select"), "Select keeps"));                  // (T, error) without Must
    CHECK(member("df", "Nothing").empty() && member("unknown", "Select").empty() && member("err", "Select").empty());
    // Parsed packages are cached until a file changes.
    const std::string dm = (root / "datamunge").string();
    CHECK(gopkgdoc::ReadPackage(dm).entries.size() == gopkgdoc::ReadPackage(dm).entries.size());
    WriteText(root / "datamunge/extra.go", "package datamunge\n\n// Extra is new.\nfunc Extra() {}\n");
    CHECK(Has(gopkgdoc::Doc("datamunge.Extra", app), "Extra is new."));
    fs::remove_all(root);
}

}  // namespace

int main() {
    TestSyntaxErrors();
    TestClean();
    TestUnused();
    TestControlFlow();
    TestStdlibChecks();
    TestHoverAndNavigation();
    TestCompletion();
    TestSignature();
    TestOutline();
    TestParserLimits();
    TestRobustness();
    TestPackageDocs();
    TestMemberDocs();
    std::printf("go_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
