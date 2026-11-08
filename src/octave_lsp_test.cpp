// Windowless test for octave_lsp.cpp -- the analysis half of mep's own
// Octave language server (`mep-octave-lsp`). Drives the pure functions
// directly: no process, no JSON-RPC client. CHECK(), never assert(): the
// Release build strips assert().
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "octave_lsp.h"

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

std::vector<OctaveLspDiagnostic> Diags(const std::string &text, const std::string &path = std::string()) {
    return OctaveLspDiagnostics(*OctaveLspAnalyze(L(text), path));
}

int CountCode(const std::vector<OctaveLspDiagnostic> &ds, const std::string &code) {
    return static_cast<int>(std::count_if(ds.begin(), ds.end(), [&](const OctaveLspDiagnostic &d) { return d.code == code; }));
}

bool HasLabel(const std::vector<OctaveLspCompletionItem> &items, const std::string &label) {
    for (const auto &it : items)
        if (it.label == label) return true;
    return false;
}

void TestSyntaxError() {
    const auto ds = Diags("function r = f(x)\n  r = x +;\nend\n");
    CHECK(ds.size() == 1);
    CHECK(ds[0].code == "syntax-error");
    CHECK(ds[0].severity == OctaveLspSeverity::Error);
    CHECK(ds[0].range.line == 1);
    CHECK(OctaveLspDiagnosticFails(ds[0]));
    CHECK(OctaveLspOctaveCheck("x = 1;\ny = (2;\n", "s.m") != "ok");
    CHECK(OctaveLspOctaveCheck("x = 1;\ny = (2);\n", "s.m") == "ok");
    // Checked against Octave 11.3's __parse_file__: `arguments` outside an
    // arguments block is never a command word; an open method at the end of a
    // classdef file is a function body open; command/variable columns carry the
    // lexer's unput-comma drift.
    CHECK(OctaveLspOctaveCheck("arguments are given,\n", "s.m") == "syntax error near line 1, column 13");
    CHECK(OctaveLspOctaveCheck("arguments = 3;\n", "s.m") == "ok");
    CHECK(OctaveLspOctaveCheck("classdef c1\n  methods\n    function f(o)\n      x = 1;\n", "c1.m") ==
          "function body open at end of input near line 5, column 0");
    CHECK(OctaveLspOctaveCheck("function f\nex = 1;\ny = [1 2];ex ey\n", "f.m") ==
          "ex: invalid use of symbol as both variable and command near line 3, column 11");
    // Octave-only syntax parses.
    CHECK(Diags("x = 0;\ndo\n  x++;\nuntil x > 3\nif x != 4, disp(x); endif\n# comment\n").empty());
    CHECK(Diags("unwind_protect\n  a = 1;\nunwind_protect_cleanup\n  disp(a);\nend_unwind_protect\n").empty());
    // A file running out mid-block is an error.
    CHECK(CountCode(Diags("if true\n  x = 1;\n"), "syntax-error") == 1);
}

void TestUnusedLocal() {
    const auto ds = Diags("function r = f(x)\n  y = 3;\n  r = x;\nend\n");
    CHECK(CountCode(ds, "unused-local") == 1);
    CHECK(ds[0].range.line == 1 && ds[0].range.col == 2);
    CHECK(ds[0].severity == OctaveLspSeverity::Hint && ds[0].unnecessary);
    CHECK(!OctaveLspDiagnosticFails(ds[0]));
    // Parameters, outputs, loop variables, globals and read names are not flagged.
    CHECK(CountCode(Diags("function r = f(x, unused)\n  global g\n  g = 1;\n  for k = 1:3\n  end\n  r = x;\nend\n"),
                    "unused-local") == 0);
    // eval can read anything: no hints in that function.
    CHECK(CountCode(Diags("function f()\n  y = 3;\n  eval('disp(y)');\nend\n"), "unused-local") == 0);
    // A nested function reading its parent's variable.
    CHECK(CountCode(Diags("function f()\n  y = 3;\n  g();\n  function g()\n    disp(y);\n  end\nend\n"),
                    "unused-local") == 0);
    // `%#ok` silences it.
    CHECK(CountCode(Diags("function f()\n  [a, b] = size(1); %#ok<ASGLU>\n  disp(b);\nend\n"), "unused-local") == 0);
    // Scripts are not checked (their variables live on in the workspace).
    CHECK(CountCode(Diags("y = 3;\n"), "unused-local") == 0);
}

void TestUnreachableAndDuplicateCase() {
    const auto ds = Diags("function f()\n  return;\n  disp(1);\nend\n");
    CHECK(CountCode(ds, "unreachable-code") == 1);
    const auto dc = Diags("switch x\n  case 1\n    a = 1;\n  case 1\n    a = 2;\nend\n");
    CHECK(CountCode(dc, "duplicate-case") == 1);
    for (const auto &d : dc)
        if (d.code == "duplicate-case") {
            CHECK(d.range.line == 3);
            CHECK(d.severity == OctaveLspSeverity::Warning);
            CHECK(d.message.find("line 2") != std::string::npos);
        }
    CHECK(CountCode(Diags("switch x\n  case 1\n  case 2\nend\n"), "duplicate-case") == 0);
    const std::string s = OctaveLspFormatDiagnostic("a.m", dc.back());
    CHECK(s.rfind("a.m:4:", 0) == 0 && s.find("[duplicate-case]") != std::string::npos);
}

void TestCompletion() {
    const auto doc = OctaveLspAnalyze(L("alpha = 1;\nbeta = 2;\nal"));
    const auto items = OctaveLspCompletions(*doc, 2, 2);
    CHECK(HasLabel(items, "alpha"));
    CHECK(!HasLabel(items, "beta"));
    for (const auto &it : items) CHECK(it.replace_start == 0 && it.replace_end == 2);
    // Octave's own functions and keywords.
    const auto d2 = OctaveLspAnalyze(L("x = linsp"));
    CHECK(HasLabel(OctaveLspCompletions(*d2, 0, 9), "linspace"));
    const auto d3 = OctaveLspAnalyze(L("endf"));
    CHECK(HasLabel(OctaveLspCompletions(*d3, 0, 4), "endfunction"));
    // Fields after a dot.
    const auto d4 = OctaveLspAnalyze(L("s.width = 1;\ns.height = 2;\ns.w"));
    const auto f = OctaveLspCompletions(*d4, 2, 3);
    CHECK(HasLabel(f, "width") && !HasLabel(f, "height"));
    // Nothing inside comments or strings.
    const auto d5 = OctaveLspAnalyze(L("alpha = 1;\n% al"));
    CHECK(OctaveLspCompletions(*d5, 1, 4).empty());
    const auto d6 = OctaveLspAnalyze(L("alpha = 1;\nx = 'al"));
    CHECK(OctaveLspCompletions(*d6, 1, 7).empty());
    // A function's locals are not visible in another function.
    const auto d7 = OctaveLspAnalyze(L("function f()\n  secret = 1;\n  disp(secret);\nend\nfunction g()\n  sec\nend\n"));
    CHECK(!HasLabel(OctaveLspCompletions(*d7, 5, 5), "secret"));
    // ... and completion still works in a broken document.
    const auto d8 = OctaveLspAnalyze(L("function f()\n  counter = 1;\n  x = (\n  cou\nend\n"));
    CHECK(HasLabel(OctaveLspCompletions(*d8, 3, 5), "counter"));
}

void TestHoverAndSignature() {
    const auto doc = OctaveLspAnalyze(L("function r = add2(a, b)\n  r = a + b;\nend\nx = add2(1, zeros(2));\n"));
    const auto h = OctaveLspHover(*doc, 3, 5);
    CHECK(h.found && h.text.find("add2") != std::string::npos);
    const auto hz = OctaveLspHover(*doc, 3, 13);
    CHECK(hz.found && hz.text.find("zeros") != std::string::npos);
    const auto sh = OctaveLspSignatureAt(*doc, 3, 12);
    CHECK(sh.found && sh.label.find("add2") != std::string::npos);
    CHECK(sh.params.size() == 2 && sh.active_param == 1);
    const auto sz = OctaveLspSignatureAt(*doc, 3, 18);
    CHECK(sz.found && sz.label.find("zeros") != std::string::npos);
}

void TestNavigation() {
    const std::string src = "function r = f(x)\n  total = x;\n  total = total + 1;\n  r = g(total);\nend\n"
                            "function y = g(v)\n  y = v;\nend\n";
    const auto doc = OctaveLspAnalyze(L(src));
    const auto def = OctaveLspDefinition(*doc, 2, 11);
    CHECK(def.size() == 1 && def[0].range.line == 1 && def[0].range.col == 2);
    const auto gdef = OctaveLspDefinition(*doc, 3, 7);
    CHECK(gdef.size() == 1 && gdef[0].range.line == 5);
    const auto refs = OctaveLspReferences(*doc, 1, 3);
    CHECK(refs.size() == 4);
    const auto pr = OctaveLspPrepareRename(*doc, 1, 3);
    CHECK(pr.found && pr.placeholder == "total");
    std::string err;
    const auto edits = OctaveLspRename(*doc, 1, 3, "sum1", &err);
    CHECK(edits.size() == 4 && err.empty());
    for (const auto &e : edits) CHECK(e.new_text == "sum1");
    // x in g is a different variable from x in f.
    CHECK(OctaveLspReferences(*doc, 0, 15).size() == 2);
    // Renaming to a non-identifier fails.
    std::string err2;
    CHECK(OctaveLspRename(*doc, 1, 3, "1bad", &err2).empty() && !err2.empty());
    // Builtins cannot be renamed.
    const auto d2 = OctaveLspAnalyze(L("x = zeros(3);\n"));
    CHECK(!OctaveLspPrepareRename(*d2, 0, 5).found);
}

void TestSymbolsAndFolds() {
    const std::string src = "classdef Point\n  properties\n    x = 0;\n  end\n  methods\n"
                            "    function obj = Point(x)\n      obj.x = x;\n    end\n  end\nend\n";
    const auto doc = OctaveLspAnalyze(L(src), "Point.m");
    CHECK(Diags(src, "Point.m").empty());
    const auto syms = OctaveLspSymbols(*doc);
    bool cls = false, prop = false, meth = false;
    for (const auto &s : syms) {
        if (s.name == "Point" && s.kind == OctaveLspSymbolKind::Class) cls = true;
        if (s.name == "x" && s.kind == OctaveLspSymbolKind::Property) prop = true;
        if (s.name == "Point" && s.kind == OctaveLspSymbolKind::Method && s.parent >= 0) meth = true;
    }
    CHECK(cls && prop && meth);
    // A classdef in a file of another name is Octave's error.
    CHECK(CountCode(Diags(src, "Other.m"), "syntax-error") == 1);
    const auto folds = OctaveLspFolds(*doc);
    bool whole = false;
    for (const auto &f : folds)
        if (f.start_line == 0 && f.end_line >= 8) whole = true;
    CHECK(whole);
    const auto d2 = OctaveLspAnalyze(L("%{\nblock\ncomment\n%}\nx = 1;\n"));
    bool cf = false;
    for (const auto &f : OctaveLspFolds(*d2))
        if (f.kind == "comment" && f.start_line == 0) cf = true;
    CHECK(cf);
    const auto fsyms = OctaveLspSymbols(*OctaveLspAnalyze(L("function a()\nend\nfunction b()\nend\n")));
    CHECK(fsyms.size() == 2 && fsyms[0].kind == OctaveLspSymbolKind::Function);
}

void TestRobustness() {
    // Every query on every prefix of a file must not crash.
    const std::string src =
        "## -*- texinfo -*-\nfunction [m, s] = stats(x, varargin)\n  if nargin < 1\n    print_usage();\n  endif\n"
        "  m = mean(x(:));\n  s = struct('a', {1, 2});\n  f = @(t) t.^2 + m;\n  c = {1, 'two', [3 4]'};\n"
        "  switch class(x)\n    case {'double', 'single'}\n      s.k = f(2);\n    otherwise\n      error(\"bad: %s\\n\", "
        "class(x));\n  endswitch\n  do\n    m--;\n  until m < 0\n  hold on\n  try\n    q = x{end};\n  catch err\n"
        "    disp(err.message);\n  end_try_catch\n%{\nblock\n%}\nendfunction\n";
    for (size_t n = 0; n <= src.size(); n++) {
        const auto lines = L(src.substr(0, n));
        const auto doc = OctaveLspAnalyze(lines);
        const int last = static_cast<int>(lines.size()) - 1;
        const int col = static_cast<int>(lines.back().size());
        OctaveLspDiagnostics(*doc);
        OctaveLspCompletions(*doc, last, col);
        OctaveLspHover(*doc, last, col);
        OctaveLspSignatureAt(*doc, last, col);
        OctaveLspDefinition(*doc, last, col);
        OctaveLspReferences(*doc, last, col);
        OctaveLspPrepareRename(*doc, last, col);
        OctaveLspSymbols(*doc);
        OctaveLspFolds(*doc);
    }
    CHECK(Diags(src).empty() || CountCode(Diags(src), "syntax-error") == 0);
    // Out-of-range positions.
    const auto doc = OctaveLspAnalyze(L("x = 1;"));
    OctaveLspCompletions(*doc, 5, 99);
    OctaveLspHover(*doc, -1, -1);
    OctaveLspSignatureAt(*doc, 9, 0);
    OctaveLspDefinition(*doc, 0, 99);
}

}  // namespace

int main() {
    TestSyntaxError();
    TestUnusedLocal();
    TestUnreachableAndDuplicateCase();
    TestCompletion();
    TestHoverAndSignature();
    TestNavigation();
    TestSymbolsAndFolds();
    TestRobustness();
    std::printf("octave_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
