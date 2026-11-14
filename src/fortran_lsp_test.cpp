// Windowless test for fortran_lsp.cpp -- the analysis half of mep's own
// Fortran language server (`mep-fortran-lsp`) -- and the reader under it
// (fortran_parse.cpp). Drives the pure functions directly: no process, no
// JSON-RPC client. CHECK(), never assert(): the Release build strips assert().
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "fortran_lsp.h"
#include "fortran_lsp_modules.h"
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


// ---- across files: USE of modules from other files, FORD documentation --------------------------

// A module shaped like the ones rosetta generates: private by default, a type hierarchy with
// bindings and a generic binding, a generic interface, a constructor generic named after its
// type, parameters, FORD `!>` / `!!` docs and Doxygen tags.
const char *kGeom =
    "!> Shapes and fitting for the test suite.\n"
    "!!\n"
    "!! A longer description of the module.\n"
    "module geom\n"
    "  use iso_c_binding, only: c_double, c_int\n"
    "  implicit none\n"
    "  private\n"
    "  public :: shape, circle, fit, color_red, color_green, scale, new_circle\n"
    "\n"
    "  !> Red, as an enum value.\n"
    "  integer(c_int), parameter :: color_red = 0\n"
    "  integer(c_int), parameter :: color_green = 1 !! Green.\n"
    "\n"
    "  !> An abstract shape.\n"
    "  type, abstract :: shape\n"
    "     real(c_double) :: x = 0 !! x position\n"
    "  contains\n"
    "     !> Area of the shape.\n"
    "     procedure(area_iface), deferred :: area\n"
    "     procedure :: move => shape_move\n"
    "  end type shape\n"
    "\n"
    "  !> A circle.\n"
    "  !! Has a radius.\n"
    "  type, extends(shape) :: circle\n"
    "     real(c_double) :: radius !! the radius\n"
    "  contains\n"
    "     procedure :: area => circle_area\n"
    "     procedure :: fit_one => circle_fit1\n"
    "     procedure :: fit_two => circle_fit2\n"
    "     !> Fit the circle.\n"
    "     generic :: fit => fit_one, fit_two\n"
    "  end type circle\n"
    "\n"
    "  abstract interface\n"
    "     function area_iface(self) result(a)\n"
    "       import :: shape, c_double\n"
    "       class(shape), intent(in) :: self\n"
    "       real(c_double) :: a\n"
    "     end function\n"
    "  end interface\n"
    "\n"
    "  !> Fit a model.\n"
    "  interface fit\n"
    "     module procedure fit_1, fit_2\n"
    "  end interface fit\n"
    "\n"
    "  !> Construct a circle.\n"
    "  interface circle\n"
    "     module procedure new_circle\n"
    "  end interface circle\n"
    "\n"
    "contains\n"
    "\n"
    "  !> @brief Fit with one array.\n"
    "  !> @param x the data\n"
    "  !> @param w weights\n"
    "  !> @return the fitted value\n"
    "  function fit_1(x, w) result(r)\n"
    "    real(c_double), intent(in) :: x(:)\n"
    "    real(c_double), intent(in), optional :: w(:)\n"
    "    real(c_double) :: r\n"
    "    r = 0\n"
    "  end function fit_1\n"
    "\n"
    "  !> Fit with a scalar.\n"
    "  subroutine fit_2(x, n, out)\n"
    "    real(c_double), intent(in) :: x !! the scalar\n"
    "    integer, intent(in) :: n !! how many\n"
    "    real(c_double), intent(out) :: out\n"
    "    out = x * n\n"
    "  end subroutine fit_2\n"
    "\n"
    "  !> A circle of radius @p r.\n"
    "  function new_circle(r) result(c)\n"
    "    real(c_double), intent(in) :: r !! radius\n"
    "    type(circle) :: c\n"
    "    c%radius = r\n"
    "  end function\n"
    "\n"
    "  function circle_area(self) result(a)\n"
    "    !! Area of a circle.\n"
    "    class(circle), intent(in) :: self\n"
    "    real(c_double) :: a\n"
    "    a = 3.14159d0 * self%radius**2\n"
    "  end function\n"
    "\n"
    "  subroutine shape_move(self, dx)\n"
    "    !! Move by dx.\n"
    "    class(shape), intent(inout) :: self\n"
    "    real(c_double), intent(in) :: dx\n"
    "    self%x = self%x + dx\n"
    "  end subroutine\n"
    "\n"
    "  !> Fit to an array.\n"
    "  subroutine circle_fit1(self, x)\n"
    "    class(circle), intent(inout) :: self\n"
    "    real(c_double), intent(in) :: x(:)\n"
    "  end subroutine\n"
    "\n"
    "  subroutine circle_fit2(self, x, y)\n"
    "    class(circle), intent(inout) :: self\n"
    "    real(c_double), intent(in) :: x, y\n"
    "  end subroutine\n"
    "\n"
    "  !> Scale things.\n"
    "  subroutine scale(f)\n"
    "    real(c_double), intent(in) :: f\n"
    "  end subroutine\n"
    "\n"
    "  subroutine hidden()\n"
    "  end subroutine\n"
    "end module geom\n";

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("mep-fortran-lsp-test-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string Write(const std::string &rel, const std::string &text) const {
        fs::path p = path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
        return p.string();
    }
    std::string Dir(const std::string &rel) const { return (path / rel).string(); }
};

void SetEnv(const char *name, const std::string &value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

// (line, column) of the @p nth occurrence of @p needle, plus @p off columns.
struct At {
    int line = -1, col = -1;
};
At Find(const std::vector<std::string> &lines, const std::string &needle, int off = 0, int nth = 0) {
    for (size_t i = 0; i < lines.size(); i++) {
        size_t p = lines[i].find(needle);
        while (p != std::string::npos) {
            if (nth-- == 0) return {static_cast<int>(i), static_cast<int>(p) + off};
            p = lines[i].find(needle, p + 1);
        }
    }
    return {};
}

bool Has(const std::string &text, const char *what) { return text.find(what) != std::string::npos; }

bool HasItem(const std::vector<FortranLspCompletionItem> &items, const char *label,
             const FortranLspCompletionItem **out = nullptr) {
    for (const auto &it : items) {
        if (it.label == label) {
            if (out) *out = &it;
            return true;
        }
    }
    return false;
}

void TestModuleScan() {
    auto names = FortranLspScanModuleNames(
        "MODULE Alpha\n  module procedure foo\nend module alpha\n  module beta ! c\nsubmodule (alpha) gamma\n"
        "module function f()\nmodule\n");
    CHECK(names.size() == 2 && names[0] == "alpha" && names[1] == "beta");
    CHECK(FortranSourceName("a.F90") && FortranSourceName("b.f") && FortranSourceName("c.for") &&
          !FortranSourceName("d.c") && !FortranSourceName("mod"));
}

void TestCrossFile() {
    TempDir dir;
    dir.Write("src/geom.f90", kGeom);
    dir.Write("build/geom_copy.f90", "module geom\nend module geom\n");  // build output: not indexed
    const std::string main_text =
        "program main\n"
        "  use geom\n"
        "  use iso_c_binding, only: c_double\n"
        "  implicit none\n"
        "  type(circle) :: c\n"
        "  real(c_double) :: v\n"
        "  c = circle(1.0d0)\n"
        "  v = fit(a, b\n"
        "  call c%move(1.0d0)\n"
        "  v = c%area()\n"
        "  print *, color_red, color_green\n"
        "  call c%fit(\n"
        "  call c%\n"
        "  v = fi\n"
        "  call hidden()\n"
        "end program main\n";
    const std::string main_path = dir.Write("app/main.f90", main_text);
    FortranModuleIndex index;
    index.SetRoot(dir.path.string());
    CHECK(index.workspace_files() == 2);
    auto lines = L(main_text);
    auto doc = index.Analyze(lines, main_path);
    const std::string geom_path = dir.Dir("src/geom.f90");

    // hover: a generic interface, with every specific and the docs as Markdown
    At at = Find(lines, "fit(a", 1);
    auto h = FortranLspHover(*doc, at.line, at.col);
    CHECK(h.found && Has(h.text, "interface fit") && Has(h.text, "function fit_1(x, w) result(r)") &&
          Has(h.text, "subroutine fit_2(x, n, out)") && Has(h.text, "Fit a model.") &&
          Has(h.text, "**Specific procedures**") && Has(h.text, "`fit_1(x, w)`: Fit with one array.") &&
          Has(h.text, "from module `geom`, geom.f90:"));
    // a type, extending another, with its constructor generic
    at = Find(lines, "type(circle)", 6);
    h = FortranLspHover(*doc, at.line, at.col);
    CHECK(h.found && Has(h.text, "type, extends(shape) :: circle") && Has(h.text, "real(c_double) :: radius") &&
          Has(h.text, "generic :: fit => fit_one, fit_two") && Has(h.text, "A circle.\nHas a radius.") &&
          Has(h.text, "**Constructors**") && Has(h.text, "`new_circle(r)`: A circle of radius `r`."));
    // parameters: value and the `!>` above / trailing `!!` docs
    at = Find(lines, "color_green", 2);
    h = FortranLspHover(*doc, at.line, at.col);
    CHECK(h.found && Has(h.text, "integer(c_int), parameter :: color_green = 1") && Has(h.text, "Green."));
    at = Find(lines, "color_red", 2);
    h = FortranLspHover(*doc, at.line, at.col);
    CHECK(h.found && Has(h.text, "color_red = 0") && Has(h.text, "Red, as an enum value."));
    // the module named in USE
    at = Find(lines, "use geom", 5);
    h = FortranLspHover(*doc, at.line, at.col);
    CHECK(h.found && Has(h.text, "module geom") && Has(h.text, "Shapes and fitting for the test suite.") &&
          Has(h.text, "A longer description of the module."));
    // a private name is not visible
    at = Find(lines, "hidden", 2);
    CHECK(!FortranLspHover(*doc, at.line, at.col).found);
    // bindings through obj%: the overriding binding, documented by its procedure
    at = Find(lines, "c%area", 3);
    h = FortranLspHover(*doc, at.line, at.col);
    CHECK(h.found && Has(h.text, "procedure :: area => circle_area") && Has(h.text, "Area of a circle.") &&
          Has(h.text, "type-bound procedure"));
    at = Find(lines, "c%move", 3);
    h = FortranLspHover(*doc, at.line, at.col);  // inherited from shape
    CHECK(h.found && Has(h.text, "procedure :: move => shape_move") && Has(h.text, "Move by dx."));

    // completion: names the module exports (not its private ones), with documentation
    at = Find(lines, "v = fi", 6, 1);
    auto items = FortranLspCompletions(*doc, at.line, at.col);
    const FortranLspCompletionItem *it = nullptr;
    CHECK(HasItem(items, "fit", &it) && Has(it->documentation, "Fit a model.") && it->kind == FortranLspKind::Function);
    lines[static_cast<size_t>(at.line)] = "  v = colo";
    doc = index.Analyze(lines, main_path);
    items = FortranLspCompletions(*doc, at.line, 10);
    CHECK(HasItem(items, "color_red", &it) && it->kind == FortranLspKind::Constant && Has(it->detail, "= 0"));
    CHECK(HasItem(items, "color_green"));
    lines[static_cast<size_t>(at.line)] = "  call hid";
    doc = index.Analyze(lines, main_path);
    CHECK(!HasItem(FortranLspCompletions(*doc, at.line, 10), "hidden"));
    // after obj%: components and bindings, inherited ones included
    at = Find(lines, "call c%", 7, 1);
    items = FortranLspCompletions(*doc, at.line, at.col);
    CHECK(HasItem(items, "radius") && HasItem(items, "x") && HasItem(items, "area") && HasItem(items, "fit") &&
          HasItem(items, "fit_one"));
    CHECK(HasItem(items, "move", &it) && it->kind == FortranLspKind::Method && Has(it->documentation, "Move by dx."));
    CHECK(HasItem(items, "radius", &it) && it->kind == FortranLspKind::Field && Has(it->documentation, "the radius"));
    // after use: the workspace's modules
    auto udoc = index.Analyze(L("program q\n  use ge\nend\n"), main_path);
    CHECK(HasItem(FortranLspCompletions(*udoc, 1, 8), "geom"));

    // signature help: every specific of a generic, the active one by argument count
    at = Find(lines, "fit(a, b", 4);
    auto sh = FortranLspSignatureAt(*doc, at.line, at.col);
    CHECK(sh.found && sh.signatures.size() == 2 && sh.signatures[0].label == "fit(x, w)" &&
          sh.signatures[1].label == "fit(x, n, out)" && sh.active_signature == 0 && sh.active_param == 0);
    CHECK(Has(sh.signatures[0].documentation, "Fit with one array.") && Has(sh.signatures[0].param_docs[1], "weights") &&
          Has(sh.signatures[0].param_docs[1], "optional"));
    sh = FortranLspSignatureAt(*doc, at.line, at.col + 3);  // after `a, b`: still fit_1 (2 args)
    CHECK(sh.found && sh.active_signature == 0 && sh.active_param == 1);
    lines[static_cast<size_t>(at.line)] = "  v = fit(a, b, ";
    doc = index.Analyze(lines, main_path);
    sh = FortranLspSignatureAt(*doc, at.line, static_cast<int>(lines[static_cast<size_t>(at.line)].size()));
    CHECK(sh.found && sh.active_signature == 1 && sh.active_param == 2 && sh.label == "fit(x, n, out)");
    lines[static_cast<size_t>(at.line)] = "  v = fit(a, out=";
    doc = index.Analyze(lines, main_path);
    sh = FortranLspSignatureAt(*doc, at.line, static_cast<int>(lines[static_cast<size_t>(at.line)].size()));
    CHECK(sh.found && sh.active_signature == 1 && sh.active_param == 2);
    // the constructor generic named after the type
    at = Find(lines, "circle(1", 7);
    sh = FortranLspSignatureAt(*doc, at.line, at.col);
    CHECK(sh.found && sh.signatures.size() == 1 && sh.label == "circle(r)" && Has(sh.documentation, "new_circle"));
    // a binding drops the passed object; a generic binding offers each of its bindings
    at = Find(lines, "c%move(", 7);
    sh = FortranLspSignatureAt(*doc, at.line, at.col);
    CHECK(sh.found && sh.label == "move(dx)" && Has(sh.documentation, "Move by dx."));
    at = Find(lines, "c%fit(", 6);
    sh = FortranLspSignatureAt(*doc, at.line, at.col);
    CHECK(sh.found && sh.signatures.size() == 2 && sh.signatures[0].label == "fit(x)" &&
          sh.signatures[1].label == "fit(x, y)" && Has(sh.signatures[0].documentation, "Fit the circle."));

    // definition: into the module's file
    at = Find(lines, "color_red", 2);
    auto defs = FortranLspDefinition(*doc, at.line, at.col);
    CHECK(defs.size() == 1 && defs[0].path == geom_path && defs[0].range.line == 10);
    at = Find(lines, "use geom", 6);
    defs = FortranLspDefinition(*doc, at.line, at.col);
    CHECK(defs.size() == 1 && defs[0].path == geom_path && defs[0].range.line == 3);
    at = Find(lines, "c%area", 3);
    defs = FortranLspDefinition(*doc, at.line, at.col);
    CHECK(defs.size() == 1 && defs[0].path == geom_path && defs[0].range.line == 27);
    at = Find(lines, "type(circle)", 6);
    defs = FortranLspDefinition(*doc, at.line, at.col);
    CHECK(defs.size() == 1 && defs[0].path == geom_path && defs[0].range.line == 24);
    // local names still resolve locally
    at = Find(lines, "c = circle", 0);
    defs = FortranLspDefinition(*doc, at.line, at.col);
    CHECK(defs.size() == 1 && defs[0].path.empty() && defs[0].range.line == 4);
    // the module from another file keeps undeclared quiet, as before
    CHECK(!HasCode(FortranLspDiagnostics(*doc), "undeclared"));
    // every query at every position of both files: nothing may crash
    auto geom = index.Find("geom", main_path);
    CHECK(geom != nullptr && FortranLspDiagnostics(*geom).empty());
    for (const auto &d : {doc, geom}) {
        const auto &ls = d == doc ? lines : L(kGeom);
        for (size_t li = 0; li <= ls.size(); li++) {
            int n = li < ls.size() ? static_cast<int>(ls[li].size()) + 1 : 2;
            for (int c = 0; c <= n; c++) {
                int ln = static_cast<int>(li);
                FortranLspHover(*d, ln, c);
                FortranLspSignatureAt(*d, ln, c);
                FortranLspDefinition(*d, ln, c);
                if (c % 4 == 0) FortranLspCompletions(*d, ln, c);
            }
        }
    }
}

void TestOnlyAndRenames() {
    TempDir dir;
    dir.Write("geom.f90", kGeom);
    const std::string text =
        "subroutine s\n"
        "  use geom, only: f => fit, color_red, circle\n"
        "  use geom, sc => scale\n"
        "  implicit none\n"
        "  call sc(2.0d0)\n"
        "  print *, f(x), colo\n"
        "  call s\n"
        "end subroutine\n";
    const std::string path = dir.Write("s.f90", text);
    FortranModuleIndex index;
    index.SetRoot(dir.path.string());
    auto lines = L(text);
    auto doc = index.Analyze(lines, path);
    // a renamed name hovers as what it names
    At at = Find(lines, "f(x)", 0);
    auto h = FortranLspHover(*doc, at.line, at.col);
    CHECK(h.found && Has(h.text, "interface fit") && Has(h.text, "Fit a model."));
    at = Find(lines, "f => fit", 0);
    h = FortranLspHover(*doc, at.line, at.col);
    CHECK(h.found && Has(h.text, "interface fit"));
    at = Find(lines, "call sc", 6);
    h = FortranLspHover(*doc, at.line, at.col);
    CHECK(h.found && Has(h.text, "subroutine scale(f)") && Has(h.text, "Scale things."));
    auto sh = FortranLspSignatureAt(*doc, at.line, at.col + 3);
    CHECK(sh.found && sh.signatures.size() == 1 && sh.label == "sc(f)");  // called by its local name
    at = Find(lines, "f(x", 2);
    sh = FortranLspSignatureAt(*doc, at.line, at.col);
    CHECK(sh.found && sh.signatures.size() == 2 && sh.label == "f(x, w)");
    // completion: the renames under their local names, not the module's
    at = Find(lines, "colo", 4);
    auto items = FortranLspCompletions(*doc, at.line, at.col);
    CHECK(HasItem(items, "color_red") && HasItem(items, "color_green"));  // the second USE is not ONLY
    lines[4] = "  call s";
    doc = index.Analyze(lines, path);
    items = FortranLspCompletions(*doc, 4, 8);
    const FortranLspCompletionItem *scale = nullptr;
    CHECK(HasItem(items, "sc") && HasItem(items, "shape"));
    CHECK(HasItem(items, "scale", &scale) && !Has(scale->detail, "subroutine"));  // only the intrinsic SCALE
    // ONLY alone: just the listed names
    const std::string only_text =
        "subroutine t\n"
        "  use geom, only: f => fit, color_red\n"
        "  print *, colo\n"
        "  print *, fi\n"
        "end subroutine\n";
    auto olines = L(only_text);
    auto odoc = index.Analyze(olines, dir.Write("t.f90", only_text));
    items = FortranLspCompletions(*odoc, 2, 15);
    const FortranLspCompletionItem *it = nullptr;
    CHECK(HasItem(items, "color_red", &it) && Has(it->documentation, "Red") && !HasItem(items, "color_green"));
    items = FortranLspCompletions(*odoc, 3, 13);
    CHECK(!HasItem(items, "fit"));
    items = FortranLspCompletions(*odoc, 3, 12);  // "f"
    CHECK(HasItem(items, "f", &it) && Has(it->documentation, "Fit a model."));
    // completion inside the ONLY list: what the module exports
    olines[1] = "  use geom, only: f => fit, sc";
    odoc = index.Analyze(olines, dir.Dir("t.f90"));
    items = FortranLspCompletions(*odoc, 1, static_cast<int>(olines[1].size()));
    CHECK(HasItem(items, "scale") && !HasItem(items, "hidden"));
}

void TestFordForms() {
    TempDir dir;
    dir.Write("geom.f90", kGeom);
    const std::string text = "program p\n  use geom\n  call fit(\n  call scale(1d0)\nend\n";
    const std::string path = dir.Write("p.f90", text);
    FortranModuleIndex index;
    auto lines = L(text);
    auto doc = index.Analyze(lines, path);  // no workspace: the file's own directory
    auto h = FortranLspHover(*doc, 2, 8);
    CHECK(h.found);
    // a specific with Doxygen tags: @brief dropped, @param/@return gathered as Markdown
    TempDir d2;
    d2.Write("m.f90",
             "module m\n"
             "contains\n"
             "  !> @brief Fit with one array.\n"
             "  !! @details Uses least squares;\n"
             "  !! see `lsq`.\n"
             "  !> @param[in] x the data\n"
             "  !!   continued\n"
             "  !> @param w weights\n"
             "  !> @return the fitted value\n"
             "  !> @note Slow.\n"
             "  function f1(x, w, z) result(r)\n"
             "    real, intent(in) :: x(:)\n"
             "    real, intent(in), optional :: w(:)\n"
             "    real, intent(in) :: z !! trailing doc of z\n"
             "    real :: r\n"
             "    r = 0\n"
             "  end function f1\n"
             "  subroutine below(a)\n"
             "    !! Documented below,\n"
             "    !! on two lines.\n"
             "    integer, intent(in) :: a\n"
             "    !! below a\n"
             "  end subroutine\n"
             "  integer function nodoc()\n"
             "    nodoc = 1\n"
             "  end function\n"
             "end module m\n");
    const std::string ptext = "program p\n  use m\n  x = f1(a)\n  call below(1)\n  i = nodoc()\nend\n";
    auto plines = L(ptext);
    auto pdoc = index.Analyze(plines, d2.Write("p.f90", ptext));
    h = FortranLspHover(*pdoc, 2, 7);
    CHECK(h.found);
    CHECK(Has(h.text, "```fortran\nfunction f1(x, w, z) result(r)\n    real, intent(in) :: x(:)\n"
                      "    real, intent(in), optional :: w(:)\n    real, intent(in) :: z\n    real :: r\n```"));
    CHECK(Has(h.text, "Fit with one array.\n\nUses least squares;\nsee `lsq`."));
    CHECK(!Has(h.text, "@brief") && !Has(h.text, "@param") && !Has(h.text, "@details"));
    CHECK(Has(h.text, "**Arguments**\n\n- `x`: the data continued\n- `w` *(optional)*: weights\n- `z`: trailing doc of z"));
    CHECK(Has(h.text, "**Returns:** the fitted value"));
    CHECK(Has(h.text, "**Note:** Slow."));
    h = FortranLspHover(*pdoc, 3, 8);
    CHECK(h.found && Has(h.text, "Documented below,\non two lines.") && Has(h.text, "- `a`: below a"));
    h = FortranLspHover(*pdoc, 4, 6);
    CHECK(h.found && Has(h.text, "integer function nodoc()") && Has(h.text, "function from module `m`"));
    // in the same file too
    auto local = FortranLspAnalyze(L("module q\n  !> The answer.\n  integer, parameter :: answer = 42\nend module\n"
                                     "program p\n  use q\n  print *, answer\nend\n"));
    h = FortranLspHover(*local, 6, 12);
    CHECK(h.found && Has(h.text, "integer, parameter :: answer = 42") && Has(h.text, "The answer.") &&
          Has(h.text, "declared on line 3"));
}

void TestSearchPath() {
    TempDir dir;
    dir.Write("lib/a/alpha.f90", "module alpha\n  !> From MEP_FORTRAN_PATH.\n  integer, parameter :: one = 1\nend module\n");
    dir.Write("lib/b/beta.F90", "module beta\n  !> From FFLAGS.\n  integer, parameter :: two = 2\nend module\n");
    dir.Write("lib/c/gamma.f", "      MODULE GAMMA\n!> Fixed form.\n      INTEGER, PARAMETER :: THREE = 3\n      END MODULE\n");
    const std::string text = "program p\n  use alpha\n  use beta\n  use gamma\n  print *, one, two, three\nend\n";
    const std::string path = dir.Write("app/p.f90", text);
    auto lines = L(text);
    SetEnv("MEP_FORTRAN_PATH", "");
    SetEnv("FFLAGS", "");
    SetEnv("FCFLAGS", "");
    FortranModuleIndex index;
    auto doc = index.Analyze(lines, path);
    CHECK(!FortranLspHover(*doc, 4, 12).found);
    SetEnv("MEP_FORTRAN_PATH", "/nonexistent:" + dir.Dir("lib/a"));
    SetEnv("FFLAGS", "-O2 -I" + dir.Dir("lib/b"));
    SetEnv("FCFLAGS", "-g -I " + dir.Dir("lib/c") + " -Wall");
    auto dirs = index.SearchDirs(path);
    CHECK(dirs.size() == 5 && dirs[1] == dir.Dir("lib/a") && dirs[2] == dir.Dir("lib/b") && dirs[3] == dir.Dir("lib/c") &&
          dirs[4] == dir.Dir("app"));
    doc = index.Analyze(lines, path);
    auto h = FortranLspHover(*doc, 4, 12);
    CHECK(h.found && Has(h.text, "From MEP_FORTRAN_PATH.") && Has(h.text, "alpha.f90:3"));
    h = FortranLspHover(*doc, 4, 17);
    CHECK(h.found && Has(h.text, "From FFLAGS."));
    h = FortranLspHover(*doc, 4, 22);
    CHECK(h.found && Has(h.text, "Fixed form."));
    auto defs = FortranLspDefinition(*doc, 4, 12);
    CHECK(defs.size() == 1 && defs[0].path == dir.Dir("lib/a/alpha.f90"));
    // the workspace comes first: a module of the same name there wins
    dir.Write("ws/alpha.f90", "module alpha\n  !> From the workspace.\n  integer, parameter :: one = 1\nend module\n");
    index.SetRoot(dir.Dir("ws"));
    doc = index.Analyze(lines, path);
    h = FortranLspHover(*doc, 4, 12);
    CHECK(h.found && Has(h.text, "From the workspace."));
    // an edited module is re-read (its mtime/size changed); what used it is out of date
    CHECK(!index.Stale(*doc));
    dir.Write("ws/alpha.f90", "module alpha\n  !> Edited.\n  integer, parameter :: one = 11\nend module\n");
    index.Refresh(dir.Dir("ws/alpha.f90"));
    CHECK(index.Stale(*doc));
    doc = index.Analyze(lines, path);
    CHECK(!index.Stale(*doc));
    h = FortranLspHover(*doc, 4, 12);
    CHECK(h.found && Has(h.text, "Edited.") && Has(h.text, "one = 11"));
    // an open file's text answers for its module
    index.SetOpen(dir.Dir("ws/alpha.f90"),
                  index.Analyze(L("module alpha\n  !> Unsaved.\n  integer, parameter :: one = 1\nend module\n"),
                                dir.Dir("ws/alpha.f90")));
    doc = index.Analyze(lines, path);
    CHECK(Has(FortranLspHover(*doc, 4, 12).text, "Unsaved."));
    SetEnv("MEP_FORTRAN_PATH", "");
    SetEnv("FFLAGS", "");
    SetEnv("FCFLAGS", "");
}

// A module the size rosetta generates for a large library: hover must stay fast once parsed.
void TestLargeModulePerformance() {
    TempDir dir;
    const int kProcs = 4000, kGenerics = 1000;
    std::string m = "!> A big generated module.\nmodule big\n  use iso_c_binding\n  implicit none\n  private\n";
    for (int g = 0; g < kGenerics; g++) m += "  public :: gen" + std::to_string(g) + "\n";
    for (int i = 0; i < 2000; i++) {
        m += "  !> Enum value " + std::to_string(i) + ".\n  integer(c_int), parameter, public :: k" + std::to_string(i) +
             " = " + std::to_string(i) + "\n";
    }
    for (int t = 0; t < 300; t++) {
        std::string n = std::to_string(t);
        m += "  !> Type " + n + ".\n  type, public" + (t % 50 ? ", extends(ty" + std::to_string(t - 1) + ")" : std::string()) +
             " :: ty" + n + "\n    real(c_double) :: v" + n + " !! value\n  contains\n    procedure :: m" + n + " => p" +
             std::to_string(t * 2) + "\n  end type\n";
    }
    for (int g = 0; g < kGenerics; g++) {
        std::string n = std::to_string(g);
        m += "  !> Generic " + n + ".\n  interface gen" + n + "\n    module procedure p" + std::to_string(g * 4) + ", p" +
             std::to_string(g * 4 + 1) + ", p" + std::to_string(g * 4 + 2) + ", p" + std::to_string(g * 4 + 3) +
             "\n  end interface\n";
    }
    m += "contains\n";
    for (int i = 0; i < kProcs; i++) {
        std::string n = std::to_string(i);
        m += "  !> Procedure " + n + ".\n  !>\n  !> @param a first\n  !> @param b second\n  subroutine p" + n +
             "(self, a, b)\n    class(*), intent(inout) :: self\n    real(c_double), intent(in) :: a(:)\n"
             "    integer(c_int), intent(in), optional :: b\n    integer :: i\n    do i = 1, size(a)\n"
             "      if (present(b)) then\n        print *, a(i) + b\n      end if\n    end do\n  end subroutine p" + n + "\n";
    }
    m += "end module big\n";
    const std::string mpath = dir.Write("big.f90", m);
    const int module_lines = static_cast<int>(L(m).size());
    const std::string text = "program p\n  use big\n  type(ty299) :: o\n  call gen500(o, x, \n  print *, k1999\n  call o%m250(\n"
                             "  call o%\nend\n";
    const std::string path = dir.Write("p.f90", text);
    auto lines = L(text);
    FortranModuleIndex index;
    index.SetRoot(dir.path.string());
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    auto t0 = Clock::now();
    auto doc = index.Analyze(lines, path);  // parses big.f90
    auto t1 = Clock::now();
    auto h = FortranLspHover(*doc, 3, 9);
    CHECK(h.found && Has(h.text, "interface gen500") && Has(h.text, "Generic 500."));
    // a keystroke: the program is analysed again (the module from the cache), then hovered
    double worst = 0;
    for (int k = 0; k < 5; k++) {
        auto a = Clock::now();
        doc = index.Analyze(lines, path);
        auto hv = FortranLspHover(*doc, 4, 12);
        auto b = Clock::now();
        CHECK(hv.found && Has(hv.text, "Enum value 1999."));
        worst = std::max(worst, ms(b - a));
    }
    auto t2 = Clock::now();
    auto sh = FortranLspSignatureAt(*doc, 3, static_cast<int>(lines[3].size()));
    CHECK(sh.found && sh.signatures.size() == 4 && sh.active_param == 2);
    auto items = FortranLspCompletions(*doc, 6, 9);  // o%: 50 levels of inheritance
    CHECK(items.size() == 100);
    sh = FortranLspSignatureAt(*doc, 5, static_cast<int>(lines[5].size()));  // inherited from ty250
    CHECK(sh.found && sh.label == "m250(a, b)");
    lines[6] = "  call gen1";
    doc = index.Analyze(lines, path);
    items = FortranLspCompletions(*doc, 6, 11);
    CHECK(items.size() >= 111);
    auto t3 = Clock::now();
    // inside the module itself: a use of a dummy near the end, and a generic's specific
    auto big = index.Find("big", path);
    CHECK(big != nullptr);
    auto blines = L(m);
    At in_big = Find(blines, "print *, a(i) + b", 9, kProcs - 1);
    auto t4 = Clock::now();
    auto bh = FortranLspHover(*big, in_big.line, in_big.col);
    At spec = Find(blines, "module procedure p2000", 17);
    auto bh2 = FortranLspHover(*big, spec.line, spec.col);
    auto t5 = Clock::now();
    CHECK(bh.found && Has(bh.text, "real(c_double), intent(in) :: a(:)") && Has(bh.text, "first"));
    CHECK(bh2.found && Has(bh2.text, "subroutine p2000(self, a, b)") && Has(bh2.text, "Procedure 2000."));
    std::printf("fortran_lsp_test: %d-line module: first parse %.0f ms; re-analyse + hover %.1f ms (worst of 5); "
                "signature + 2 completions %.1f ms; 2 hovers inside the module %.1f ms\n",
                module_lines, ms(t1 - t0), worst, ms(t3 - t2), ms(t5 - t4));
    CHECK(worst < 1000 && ms(t5 - t4) < 1000);
    (void)mpath;
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
    TestModuleScan();
    TestCrossFile();
    TestOnlyAndRenames();
    TestFordForms();
    TestSearchPath();
    TestLargeModulePerformance();
    std::printf("fortran_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
