// Windowless test for fortran_lsp.cpp -- the analysis half of mep's own
// Fortran language server (`mep-fortran-lsp`) -- and the reader under it
// (fortran_parse.cpp). Drives the pure functions directly: no process, no
// JSON-RPC client. CHECK(), never assert(): the Release build strips assert().
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "fortran_lsp.h"
#include "fortran_parse.h"

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

bool HasCode(const std::vector<FortranLspDiagnostic> &ds, const char *code) {
    for (const auto &d : ds)
        if (d.code == code) return true;
    return false;
}

const FortranLspDiagnostic *First(const std::vector<FortranLspDiagnostic> &ds, const char *code) {
    for (const auto &d : ds)
        if (d.code == code) return &d;
    return nullptr;
}

std::string Verdict(const std::string &text, const char *path = "t.f90") {
    return FortranLspGfortranCheck(text, path);
}

const char *kSample =
    "module shapes\n"                                       // 0
    "  implicit none\n"                                     // 1
    "  integer, parameter :: dp = kind(1.0d0)\n"            // 2
    "  type :: circle\n"                                    // 3
    "    real(dp) :: radius = 1.0_dp\n"                     // 4
    "  end type circle\n"                                   // 5
    "contains\n"                                            // 6
    "  function area(c) result(a)\n"                        // 7
    "    type(circle), intent(in) :: c\n"                   // 8
    "    real(dp) :: a\n"                                   // 9
    "    a = 3.14159_dp * c%radius**2\n"                    // 10
    "  end function area\n"                                 // 11
    "  subroutine show(c, n)\n"                             // 12
    "    type(circle), intent(in) :: c\n"                   // 13
    "    integer, intent(in) :: n\n"                        // 14
    "    integer :: i\n"                                    // 15
    "    do i = 1, n\n"                                     // 16
    "      print *, area(c), sqrt(c%radius)\n"              // 17
    "    end do\n"                                          // 18
    "  end subroutine show\n"                               // 19
    "end module shapes\n";                                  // 20

void TestReader() {
    // free form: units, constructs, labels
    CHECK(Verdict(kSample).empty());
    CHECK(Verdict("program p\n  x = 1\nend program p\n").empty());
    CHECK(Verdict("end\n").empty());  // an empty main program
    CHECK(Verdict("subroutine s(a, *)\n  call t(a, *10)\n10 continue\nend\n").empty());
    // fixed form, with continuation, comments, blank-free keywords
    const char *fixed =
        "C     a comment\n"
        "      PROGRAM MAIN\n"
        "      DOUBLE PRECISION X\n"
        "      DO 10 I = 1,\n"
        "     &  10\n"
        "*        another comment\n"
        "        X = X + I\n"
        "   10 CONTINUE\n"
        "      GOTO20\n"
        "   20 CALLFOO(X)\n"
        "      END\n";
    CHECK(Verdict(fixed, "t.f").empty());
    // the preprocessor model
    CHECK(Verdict("#define N 3\n#if N > 2\nprogram p\n#else\nprogram p(\n#endif\nend program\n", "t.F90").empty());
    // gfortran's messages, where gfortran reports them
    CHECK(Verdict("program p\n  call foo(\nend program\n") == "2: Syntax error in CALL statement at (1)");
    CHECK(Verdict("program p\n  x = 1\n  else\nend program\n") == "3: Unexpected ELSE statement at (1)");
    CHECK(Verdict("program p\n  do i = 1, 2\n  end if\nend program\n") == "3: Expecting END DO statement at (1)");
    CHECK(Verdict("subroutine s\nend subroutine t\n") == "2: Expected label 's' for END SUBROUTINE statement at (1)");
    CHECK(Verdict("module m\n") == "0: Unexpected end of file");
    CHECK(Verdict("program p\n  frobnicate\nend\n") == "2: Unclassifiable statement at (1)");
    CHECK(Verdict("program p\n  if (x > 1 then\nend\n").find("IF") != std::string::npos);
    CHECK(Verdict("module m\ncontains\n  integer :: x\nend module\n") ==
          "3: Unexpected data declaration statement in CONTAINS section at (1)");
    CHECK(Verdict("program p\n  x = 1\n  integer :: y\nend\n") == "3: Unexpected data declaration statement at (1)");
    CHECK(Verdict("program p\n  select case (i)\n  x = 1\n  end select\nend\n").find("CASE") != std::string::npos);
    // statement functions in the specification part are not executable statements
    CHECK(Verdict("program p\n  f(x) = x * 2\n  real :: y\n  y = f(1.0)\nend\n").empty());
    // structure: units and their constructs
    auto src = fortran::Read(L(kSample), fortran::ReadOptions{});
    int units = 0;
    for (const auto &c : src->constructs) {
        if (c.kind == fortran::StmtKind::Function || c.kind == fortran::StmtKind::Subroutine) units++;
    }
    CHECK(units == 2);
    CHECK(fortran::FormForPath("a.f") == fortran::Form::Fixed);
    CHECK(fortran::FormForPath("a.F90") == fortran::Form::Free);
    CHECK(fortran::PreprocessedPath("a.F90"));
    CHECK(!fortran::PreprocessedPath("a.f90"));
}

void TestSyntaxErrors() {
    auto doc = FortranLspAnalyze(L("program p\n  call foo(\nend program\n"));
    auto ds = FortranLspDiagnostics(*doc);
    CHECK(!ds.empty());
    CHECK(ds[0].code == "syntax-error");
    CHECK(ds[0].range.line == 1);
    CHECK(ds[0].message == "Syntax error in CALL statement");
    CHECK(FortranLspDiagnosticFails(ds[0]));
    CHECK(FortranLspFormatDiagnostic("x.f90", ds[0]) == "x.f90:2:11: error: Syntax error in CALL statement [syntax-error]");
    CHECK(FortranLspDiagnostics(*FortranLspAnalyze(L(kSample))).empty());
}

void TestLints() {
    // undeclared names under IMPLICIT NONE
    auto ds = FortranLspDiagnostics(*FortranLspAnalyze(L("subroutine s\n  implicit none\n  real :: a\n  a = b + 1\nend\n")));
    const FortranLspDiagnostic *d = First(ds, "undeclared");
    CHECK(d && d->range.line == 3 && d->range.col == 6);
    // ... but not without it, nor for intrinsics, nor when a module we cannot see is used
    CHECK(!HasCode(FortranLspDiagnostics(*FortranLspAnalyze(L("subroutine s\n  real :: a\n  a = b + 1\nend\n"))), "undeclared"));
    CHECK(!HasCode(FortranLspDiagnostics(*FortranLspAnalyze(L("subroutine s\n  implicit none\n  real :: a\n  a = sqrt(2.0)\nend\n"))),
                   "undeclared"));
    CHECK(!HasCode(FortranLspDiagnostics(*FortranLspAnalyze(L("subroutine s\n  use other\n  implicit none\n  real :: a\n  a = b\nend\n"))),
                   "undeclared"));
    // host association and same-file modules
    CHECK(!HasCode(FortranLspDiagnostics(*FortranLspAnalyze(L(
                       "module m\n  implicit none\n  real :: g\nend module\nprogram p\n  use m\n  implicit none\n  g = 1\n"
                       "contains\n  subroutine t\n    g = 2\n  end subroutine\nend program\n"))),
                   "undeclared"));
    // unused locals
    ds = FortranLspDiagnostics(*FortranLspAnalyze(L("subroutine s(x)\n  real :: x, unused\n  x = 1\nend\n")));
    d = First(ds, "unused-variable");
    CHECK(d && d->range.line == 1 && d->unnecessary);
    CHECK(d && d->message.find("unused") != std::string::npos);
    CHECK(!HasCode(FortranLspDiagnostics(*FortranLspAnalyze(L(kSample))), "unused-variable"));
    // labels referenced but never defined
    ds = FortranLspDiagnostics(*FortranLspAnalyze(L("program p\n  goto 20\n10 continue\nend\n")));
    d = First(ds, "undefined-label");
    CHECK(d && d->range.line == 1);
    CHECK(!HasCode(FortranLspDiagnostics(*FortranLspAnalyze(L("program p\n  goto 10\n10 continue\nend\n"))), "undefined-label"));
    // long free-form lines
    std::string longline = "program p\n  x = 1" + std::string(140, ' ') + "+ 2\nend\n";
    CHECK(HasCode(FortranLspDiagnostics(*FortranLspAnalyze(L(longline))), "line-too-long"));
    std::string longcomment = "program p\n  x = 1 !" + std::string(140, 'c') + "\nend\n";
    CHECK(!HasCode(FortranLspDiagnostics(*FortranLspAnalyze(L(longcomment))), "line-too-long"));
}

void TestCompletion() {
    auto doc = FortranLspAnalyze(L(kSample));
    // locals and host names in scope
    auto items = FortranLspCompletions(*doc, 17, 15);  // "area(" -> at 'a'? use a prefix below
    (void)items;
    auto lines = L(kSample);
    lines[17] = "      print *, ar";
    doc = FortranLspAnalyze(lines);
    items = FortranLspCompletions(*doc, 17, 17);
    bool area = false;
    for (const auto &it : items) area = area || it.label == "area";
    CHECK(area);
    lines[17] = "      print *, c%ra";
    doc = FortranLspAnalyze(lines);
    items = FortranLspCompletions(*doc, 17, 19);
    CHECK(items.size() == 1 && items[0].label == "radius");
    lines[17] = "      x = sqr";
    doc = FortranLspAnalyze(lines);
    items = FortranLspCompletions(*doc, 17, 13);
    bool sqrt = false;
    for (const auto &it : items) sqrt = sqrt || it.label == "sqrt";
    CHECK(sqrt);
    // use: modules
    doc = FortranLspAnalyze(L("program p\n  use iso_\nend\n"));
    items = FortranLspCompletions(*doc, 1, 9);
    bool iso = false;
    for (const auto &it : items) iso = iso || it.label == "iso_c_binding";
    CHECK(iso);
}

void TestHoverAndSignature() {
    auto doc = FortranLspAnalyze(L(kSample));
    auto h = FortranLspHover(*doc, 17, 15);  // area
    CHECK(h.found && h.text.find("function area(c)") != std::string::npos);
    h = FortranLspHover(*doc, 17, 25);  // sqrt
    CHECK(h.found && h.text.find("SQRT") != std::string::npos);
    h = FortranLspHover(*doc, 15, 15);  // i
    CHECK(h.found && h.text.find("integer :: i") != std::string::npos);
    auto lines = L(kSample);
    lines[17] = "      call show(c, ";
    doc = FortranLspAnalyze(lines);
    auto sh = FortranLspSignatureAt(*doc, 17, 19);
    CHECK(sh.found && sh.label == "show(c, n)" && sh.active_param == 1);
    lines[17] = "      x = merge(a, ";
    doc = FortranLspAnalyze(lines);
    sh = FortranLspSignatureAt(*doc, 17, 19);
    CHECK(sh.found && sh.active_param == 1 && sh.params.size() == 3);
}

void TestNavigation() {
    auto doc = FortranLspAnalyze(L(kSample));
    auto defs = FortranLspDefinition(*doc, 17, 15);  // area -> line 7
    CHECK(defs.size() == 1 && defs[0].range.line == 7);
    defs = FortranLspDefinition(*doc, 17, 29);  // c%radius: c -> dummy on line 12
    CHECK(defs.size() == 1 && defs[0].range.line == 12);
    defs = FortranLspDefinition(*doc, 10, 24);  // radius component
    CHECK(defs.size() == 1 && defs[0].range.line == 4);
    auto refs = FortranLspReferences(*doc, 2, 25);  // dp
    CHECK(refs.size() >= 4);
    std::string err;
    auto edits = FortranLspRename(*doc, 15, 15, "k", &err);  // i -> k
    CHECK(err.empty() && edits.size() == 2);
    edits = FortranLspRename(*doc, 15, 15, "n", &err);  // collides
    CHECK(edits.empty() && !err.empty());
    err.clear();
    edits = FortranLspRename(*doc, 15, 15, "9x", &err);
    CHECK(edits.empty() && !err.empty());
    auto t = FortranLspPrepareRename(*doc, 12, 13);  // show
    CHECK(t.found && t.placeholder == "show");
    err.clear();
    edits = FortranLspRename(*doc, 12, 13, "display", &err);
    CHECK(edits.size() == 2);  // the statement and END SUBROUTINE show
}

void TestOutlineAndFolds() {
    auto doc = FortranLspAnalyze(L(kSample));
    auto syms = FortranLspSymbols(*doc);
    bool mod = false, area = false, circle = false, radius = false, dp = false;
    for (const auto &s : syms) {
        mod = mod || (s.name == "shapes" && s.kind == FortranLspSymbolKind::Module);
        area = area || (s.name == "area" && s.kind == FortranLspSymbolKind::Function && s.parent >= 0);
        circle = circle || (s.name == "circle" && s.kind == FortranLspSymbolKind::Class);
        radius = radius || (s.name == "radius" && s.kind == FortranLspSymbolKind::Field);
        dp = dp || (s.name == "dp" && s.kind == FortranLspSymbolKind::Constant);
    }
    CHECK(mod && area && circle && radius && dp);
    auto folds = FortranLspFolds(*doc);
    bool module_fold = false, do_fold = false;
    for (const auto &f : folds) {
        module_fold = module_fold || (f.start_line == 0 && f.end_line == 20);
        do_fold = do_fold || (f.start_line == 16 && f.end_line == 18);
    }
    CHECK(module_fold && do_fold);
    // fixed form folds and outline
    doc = FortranLspAnalyze(L("      SUBROUTINE FOO(X)\n      IF (X .GT. 0) THEN\n        X = 0\n      END IF\n      END\n"), "a.f");
    syms = FortranLspSymbols(*doc);
    CHECK(syms.size() == 1 && syms[0].name == "FOO");
    CHECK(FortranLspDiagnostics(*doc).empty());
}

void TestFormat() {
    std::string err;
    auto edits = FortranLspFormat(L("program p\nx=1\nend program p"), "a.f90", &err);
    CHECK(err.empty() && edits.size() == 1);
    CHECK(edits[0].new_text.find("   x = 1") != std::string::npos);
    edits = FortranLspFormat(L("program p\n   x = 1\nend program p"), "a.f90", &err);
    CHECK(err.empty() && edits.empty());
    edits = FortranLspFormat(L("      X=1\n      END"), "a.f", &err);
    CHECK(edits.empty() && !err.empty());
}

void TestRobustness() {
    // nothing here may crash or hang
    const char *inputs[] = {"", "\n", "(", ")", "end", "end end", "&", "&&&", "'", "\"", "!", "#if", "#endif",
                            "program", "subroutine", "function f(", "do", "if (", "select case", "type", "type(",
                            "x(((((((", "a%b%c%", "call", "use", "use ,", "implicit", "module procedure",
                            "interface operator(", "10", "10 10", ";", ";;", "x = (/", "x = [", "print *,",
                            "      X\n     &Y", "C\n*\n", "#define f(x) x\nf(", "end do", "contains", "else"};
    for (const char *in : inputs) {
        for (const char *path : {"a.f90", "a.f", "a.F90"}) {
            auto doc = FortranLspAnalyze(L(in), path);
            FortranLspDiagnostics(*doc);
            FortranLspSymbols(*doc);
            FortranLspFolds(*doc);
            FortranLspCompletions(*doc, 0, 1);
            FortranLspHover(*doc, 0, 0);
            FortranLspSignatureAt(*doc, 0, 3);
            FortranLspReferences(*doc, 0, 0);
            FortranLspCompletions(*doc, 5, 5);  // past the end
        }
    }
    std::string deep = "x = ";
    for (int i = 0; i < 2000; i++) deep += "(";
    FortranLspDiagnostics(*FortranLspAnalyze(L("program p\n" + deep + "\nend\n")));
    std::string nest = "program p\n";
    for (int i = 0; i < 3000; i++) nest += "do\n";
    FortranLspDiagnostics(*FortranLspAnalyze(L(nest)));
    CHECK(true);
}

}  // namespace

int main() {
    TestReader();
    TestSyntaxErrors();
    TestLints();
    TestCompletion();
    TestHoverAndSignature();
    TestNavigation();
    TestOutlineAndFolds();
    TestFormat();
    TestRobustness();
    std::printf("fortran_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
