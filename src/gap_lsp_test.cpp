// Windowless test for gap_lsp.cpp -- the analysis half of mep's own
// GAP language server (`mep-gap-lsp`). Drives the pure functions
// directly: no process, no JSON-RPC client. CHECK(), never assert(): the
// Release build strips assert(). The unbound-global cases are the ones
// checked against GAP 4.15's own reader warnings.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "gap_lsp.h"

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

bool HasDiag(const std::vector<GapLspDiagnostic> &ds, const std::string &code, int line, const std::string &part) {
    for (const auto &d : ds)
        if (d.code == code && d.range.line == line && d.message.find(part) != std::string::npos) return true;
    return false;
}

int Count(const std::vector<GapLspDiagnostic> &ds, const std::string &code) {
    int n = 0;
    for (const auto &d : ds)
        if (d.code == code) n++;
    return n;
}

void TestSyntax() {
    // GAP's own first error, worded and placed as GAP reports it.
    const auto doc = GapLspAnalyze(L("x := 1;\ny := ;\nz := 2;"));
    const auto ds = GapLspDiagnostics(*doc);
    CHECK(Count(ds, "syntax-error") == 1);
    CHECK(ds[0].code == "syntax-error" && ds[0].range.line == 1 && ds[0].severity == GapLspSeverity::Error);
    CHECK(GapLspDiagnostics(*GapLspAnalyze(L("f := function(x) return x; end;"))).empty());
}

void TestUnboundGlobals() {
    // The cases checked against GAP 4.15's own "Syntax warning: Unbound global variable".
    const auto doc = GapLspAnalyze(L("f := function(n)\n"
                                     "  local a;\n"
                                     "  yy := 3;\n"
                                     "  a := Lenght(n);\n"
                                     "  return a + Length(n) + zz;\n"
                                     "end;\n"
                                     "h := function() return Foo; end;\n"
                                     "Foo := 1;\n"
                                     "g := function() if IsBound(q1) then Unbind(q2); fi; return f; end;\n"
                                     "k := function() return k; end;\n"
                                     "DeclareGlobalFunction(\"Zap\");\n"
                                     "m := function() return [Zap, Size, \\<]; end;"));
    const auto ds = GapLspDiagnostics(*doc);
    CHECK(HasDiag(ds, "unbound-global", 2, "'yy'"));
    CHECK(HasDiag(ds, "unbound-global", 2, "not declared local"));
    CHECK(HasDiag(ds, "unbound-global", 3, "'Lenght'"));
    CHECK(HasDiag(ds, "unbound-global", 4, "'zz'"));
    CHECK(HasDiag(ds, "unbound-global", 6, "bound later, at line 8"));
    CHECK(HasDiag(ds, "unbound-global", 8, "'q2'"));
    CHECK(!HasDiag(ds, "unbound-global", 8, "'q1'"));  // IsBound's operand is exempt
    CHECK(!HasDiag(ds, "unbound-global", 9, ""));      // the assignment's own target
    CHECK(!HasDiag(ds, "unbound-global", 11, ""));     // declared in an earlier statement; `\<`
    CHECK(Count(ds, "unbound-global") == 5);
    // At the top level nothing is reported: GAP only warns inside functions.
    CHECK(Count(GapLspDiagnostics(*GapLspAnalyze(L("x := undefined1 + 1;"))), "unbound-global") == 0);
    // A name the package binds elsewhere counts as bound.
    GapLspOptions o;
    o.context = std::make_shared<std::set<std::string>>(std::set<std::string>{"PkgHelper"});
    CHECK(Count(GapLspDiagnostics(*GapLspAnalyze(L("f := function() return PkgHelper; end;"), o)), "unbound-global") == 0);
    CHECK(Count(GapLspDiagnostics(*GapLspAnalyze(L("f := function() return PkgHelper; end;"))), "unbound-global") == 1);
    // HasX / SetX come with an attribute X declared elsewhere.
    CHECK(Count(GapLspDiagnostics(*GapLspAnalyze(L("f := function() return [HasPkgHelper, SetPkgHelper]; end;"), o)),
                "unbound-global") == 0);
    o.opaque = true;
    CHECK(Count(GapLspDiagnostics(*GapLspAnalyze(L("f := function() return Nope; end;"), o)), "unbound-global") == 0);
    // A short lowercase name a package leaves bound at start-up is still reported.
    CHECK(Count(GapLspDiagnostics(*GapLspAnalyze(L("f := function() return x; end;"))), "unbound-global") == 1);
}

void TestLocals() {
    const auto doc = GapLspAnalyze(L("f := function(p)\n"
                                     "  local a, b, c, d, i;\n"
                                     "  b := 1;\n"
                                     "  for i in [1..3] do od;\n"
                                     "  d := function() return c + p; end;\n"
                                     "  return d;\n"
                                     "end;"));
    const auto ds = GapLspDiagnostics(*doc);
    CHECK(HasDiag(ds, "unused-local", 1, "'a' is never used"));
    CHECK(HasDiag(ds, "unused-local", 1, "'b' is assigned but never read"));
    CHECK(HasDiag(ds, "unassigned-local", 1, "'c'"));  // read (in a closure) but never assigned
    CHECK(!HasDiag(ds, "unused-local", 1, "'d'") && !HasDiag(ds, "unused-local", 1, "'i'"));
    for (const auto &d : ds)
        if (d.code == "unused-local") CHECK(d.unnecessary && d.severity == GapLspSeverity::Hint);
}

void TestCompletion() {
    // Parameters, locals (also inside a statement that does not parse yet), keywords, file globals,
    // GAP's documented names.
    const auto doc = GapLspAnalyze(L("MyHelper := function(alpha) return 1; end;\n"
                                     "f := function(arg1)\n"
                                     "  local albert;\n"
                                     "  al\n"));
    auto has = [](const std::vector<GapLspCompletionItem> &items, const std::string &label) {
        for (const auto &it : items)
            if (it.label == label) return true;
        return false;
    };
    const auto items = GapLspCompletions(*doc, 3, 4);
    CHECK(has(items, "albert") && !has(items, "alpha"));
    CHECK(items[0].label == "albert" && items[0].replace_start == 2 && items[0].replace_end == 4);
    CHECK(has(GapLspCompletions(*doc, 3, 4), "albert"));
    const auto doc2 = GapLspAnalyze(L("x := MyH;\nMyHelper := 1;\ny := Leng;\nr := rec(field1 := 1);\nr.fi;"));
    CHECK(has(GapLspCompletions(*doc2, 0, 8), "MyHelper"));
    CHECK(has(GapLspCompletions(*doc2, 2, 9), "Length"));
    CHECK(has(GapLspCompletions(*doc2, 4, 4), "field1") && !has(GapLspCompletions(*doc2, 4, 4), "Length"));
    // Nothing inside a comment or a string.
    const auto doc3 = GapLspAnalyze(L("# Leng\ns := \"Leng\";"));
    CHECK(GapLspCompletions(*doc3, 0, 6).empty() && GapLspCompletions(*doc3, 1, 10).empty());
}

void TestHoverSignature() {
    const auto doc = GapLspAnalyze(L("## Doubles x.\n"
                                     "Twice := function(x, y) local z; z := x; return 2*z; end;\n"
                                     "Print(Twice(1, 2), Size(G));"));
    const auto h = GapLspHover(*doc, 2, 7);
    CHECK(h.found && h.text.find("Twice := function( x, y )") != std::string::npos &&
          h.text.find("Doubles x.") != std::string::npos);
    const auto hl = GapLspHover(*doc, 1, 33);
    CHECK(hl.found && hl.text.find("local z") != std::string::npos && hl.text.find("`Twice`") != std::string::npos);
    const auto hs = GapLspHover(*doc, 2, 21);
    CHECK(hs.found && hs.text.find("Size(") != std::string::npos && hs.text.find("attribute") != std::string::npos);
    const auto sh = GapLspSignatureAt(*doc, 2, 15);
    CHECK(sh.found && sh.label == "Twice( x, y )" && sh.active_param == 1 && sh.params.size() == 2);
    const auto sh2 = GapLspSignatureAt(*doc, 2, 24);
    CHECK(sh2.found && sh2.label.rfind("Size(", 0) == 0);
}

void TestNavigation() {
    const auto doc = GapLspAnalyze(L("DeclareGlobalFunction(\"Foo\");\n"
                                     "InstallGlobalFunction(Foo, function(n)\n"
                                     "  local acc;\n"
                                     "  acc := n;\n"
                                     "  return acc + Foo(n - 1);\n"
                                     "end);"));
    // A local: its declaration; all of its uses.
    const auto def = GapLspDefinition(*doc, 4, 10);
    CHECK(def.size() == 1 && def[0].range.line == 2 && def[0].range.col == 8);
    CHECK(GapLspReferences(*doc, 3, 2).size() == 3);
    // A global: the declaration's string and the Install call.
    const auto gdef = GapLspDefinition(*doc, 4, 16);
    CHECK(gdef.size() == 2 && gdef[0].range.line == 0 && gdef[0].range.col == 23 && gdef[1].range.line == 1);
    CHECK(GapLspReferences(*doc, 4, 16).size() == 3);
    // Rename a global defined here: the string's contents too.
    std::string err;
    const auto edits = GapLspRename(*doc, 1, 23, "Bar", &err);
    CHECK(err.empty() && edits.size() == 3);
    CHECK(edits[0].range.line == 0 && edits[0].range.col == 23 && edits[0].range.end_col == 26);
    // Rename a local; refuse a clash and a non-identifier; refuse a library name.
    CHECK(GapLspRename(*doc, 2, 8, "total", &err).size() == 3);
    err.clear();
    CHECK(GapLspRename(*doc, 2, 8, "n", &err).empty() && !err.empty());
    err.clear();
    CHECK(GapLspRename(*doc, 2, 8, "end", &err).empty() && !err.empty());
    const auto lib = GapLspAnalyze(L("x := Size(G);"));
    CHECK(!GapLspPrepareRename(*lib, 0, 6).found);
    CHECK(GapLspPrepareRename(*doc, 2, 9).found && GapLspPrepareRename(*doc, 2, 9).placeholder == "acc");
}

void TestSymbolsFolds() {
    const auto doc = GapLspAnalyze(L("#############\n"
                                     "## header\n"
                                     "DeclareAttribute(\"Weight\", IsObject);\n"
                                     "InstallMethod(Weight, \"for a list\", [IsList],\n"
                                     "function(l)\n"
                                     "  if l = [] then\n"
                                     "    return 0;\n"
                                     "  else\n"
                                     "    return 1;\n"
                                     "  fi;\n"
                                     "end);\n"
                                     "Limit := 10;"));
    const auto syms = GapLspSymbols(*doc);
    CHECK(syms.size() == 3);
    CHECK(syms[0].name == "Weight" && syms[0].kind == GapLspSymbolKind::Property);
    CHECK(syms[1].name == "Weight" && syms[1].kind == GapLspSymbolKind::Method && syms[1].detail == "for a list");
    CHECK(syms[2].name == "Limit" && syms[2].kind == GapLspSymbolKind::Variable);
    const auto folds = GapLspFolds(*doc);
    bool comment = false, func = false, ifb = false, elseb = false;
    for (const auto &f : folds) {
        if (f.kind == "comment" && f.start_line == 0 && f.end_line == 1) comment = true;
        if (f.start_line == 4 && f.end_line == 10) func = true;
        if (f.start_line == 5 && f.end_line == 6) ifb = true;
        if (f.start_line == 7 && f.end_line == 8) elseb = true;
    }
    CHECK(comment && func && ifb && elseb);
}

void TestFormatEdits() {
    std::string err;
    const auto edits = GapLspFormat(L("f := function()\nreturn 1;\nend;"), 100, &err);
    CHECK(err.empty() && edits.size() == 1 && edits[0].range.line == 1 && edits[0].new_text == "    return 1;\n");
    CHECK(GapLspFormat(L("f := function()\n    return 1;\nend;"), 100, &err).empty());
    CHECK(GapLspFormat(L("x := ;"), 100, &err).empty() && !err.empty());
}

void TestTopLevelNames() {
    const auto names = GapLspTopLevelNames("DeclareProperty(\"IsNice\", IsGroup);\nx := 1;\n"
                                           "f := function() local y; y := 2; end;\n"
                                           "DeclareAutoreadableVariables(\"pkg\", \"f.g\", [\"T1\", \"T2\"]);");
    const std::vector<std::string> want = {"HasIsNice", "IsNice", "SetIsNice", "T1", "T2", "f", "x"};
    CHECK(names == want);
}

void TestRobustness() {
    // Every query on every prefix of a file must not crash.
    const std::string src =
        "#############################################################################\n"
        "##\n"
        "#M  Weight( <l> )\n"
        "##\n"
        "DeclareAttribute( \"Weight\", IsList );\n"
        "InstallMethod( Weight, \"for a list\", [ IsList ],\n"
        "    function( l )\n"
        "    local s, x, f;\n"
        "    s := 0;\n"
        "    for x in l do\n"
        "        if IsInt( x ) then s := s + x; elif IsBound( x.w ) then s := s + x!.w; else continue; fi;\n"
        "    od;\n"
        "    f := {a, b} -> a + b;\n"
        "    repeat s := s - 1; until s < 10 or not s mod 2 = 0;\n"
        "    atomic readwrite l do s := Length( l ); od;\n"
        "    Info( InfoWarning, 1, \"done\" ); Assert( 0, s >= 0 );\n"
        "    return rec( total := s, (\"a\") := [ 1 .. 3 ], p := (1,2)(3,4), q := List( l, y -> y^2 : opt ) );\n"
        "    end );\n"
        "g := atomic function( readonly a ) return a[1]{[1]}; end;\n"
        "s := \"\"\"multi\nline\"\"\";\n";
    for (size_t n = 0; n <= src.size(); n++) {
        const auto lines = L(src.substr(0, n));
        const auto doc = GapLspAnalyze(lines);
        const int last = static_cast<int>(lines.size()) - 1;
        const int col = static_cast<int>(lines.back().size());
        GapLspDiagnostics(*doc);
        GapLspCompletions(*doc, last, col);
        GapLspHover(*doc, last, col);
        GapLspSignatureAt(*doc, last, col);
        GapLspSymbols(*doc);
        GapLspFolds(*doc);
        GapLspDefinition(*doc, last, col);
        GapLspReferences(*doc, last, col);
        GapLspPrepareRename(*doc, last, col);
        std::string err;
        GapLspRename(*doc, last, col, "zz", &err);
        GapLspFormat(lines, 100, &err);
        for (int l = 0; l <= last; l++) {
            GapLspHover(*doc, l, 5);
            GapLspCompletions(*doc, l, 3);
        }
    }
    // The whole file is clean apart from nothing: no syntax errors, no false unbound globals.
    const auto full = GapLspDiagnostics(*GapLspAnalyze(L(src)));
    CHECK(Count(full, "syntax-error") == 0 && Count(full, "unbound-global") == 0);
}

}  // namespace

int main() {
    TestSyntax();
    TestUnboundGlobals();
    TestLocals();
    TestCompletion();
    TestHoverSignature();
    TestNavigation();
    TestSymbolsFolds();
    TestFormatEdits();
    TestTopLevelNames();
    TestRobustness();
    std::printf("gap_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
