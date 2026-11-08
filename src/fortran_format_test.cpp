// mep's Fortran formatter, driven directly.
//
// `mep-fortran-format-test` runs the checks below;
// `mep-fortran-format-test --format [-w N] [--indent N] [--fixed] FILE...` prints
// each file formatted ('-' reads stdin), for tools/format_compare.py. A file
// ending in .f/.for/.F/.FOR/.ftn is treated as fixed form (and refused).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "fortran_format.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

bool EndsWith(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

int FormatFiles(int argc, char **argv) {
    fortranfmt::Options base;
    bool fixed = false;
    int status = 0;
    for (int k = 2; k < argc; k++) {
        const std::string arg = argv[k];
        if ((arg == "-w" || arg == "--width") && k + 1 < argc) {
            base.width = std::atoi(argv[++k]);
            continue;
        }
        if (arg == "--indent" && k + 1 < argc) {
            base.indent = std::atoi(argv[++k]);
            continue;
        }
        if (arg == "--fixed") {
            fixed = true;
            continue;
        }
        std::stringstream ss;
        if (arg == "-") {
            ss << std::cin.rdbuf();
        } else {
            std::ifstream in(arg, std::ios::binary);
            ss << in.rdbuf();
        }
        fortranfmt::Options opts = base;
        opts.fixed_form = fixed || EndsWith(arg, ".f") || EndsWith(arg, ".for") || EndsWith(arg, ".F") ||
                          EndsWith(arg, ".FOR") || EndsWith(arg, ".ftn") || EndsWith(arg, ".FTN");
        const fortranfmt::Result r = fortranfmt::Format(ss.str(), opts);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", arg.c_str(), r.error_line, r.error.c_str());
            status = 1;
            continue;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}

// Input and fprettify 0.3.7's output for it (default settings), byte for byte.
struct Case {
    const char *in, *want;
};
const Case kCases[] = {
        {"program p\nimplicit none\ninteger::i\ndo i=1,10\nif(i.eq.5)then\nprint*,i\nelse if (i>7) then\nx=a+b*c-d/e\nendif\nenddo\nend program p\n",
         "program p\n   implicit none\n   integer::i\n   do i = 1, 10\n   if (i .eq. 5) then\n      print *, i\n   else if (i > 7) then\n      x = a + b*c - d/e\n   end if\n   end do\nend program p\n"},
        {"module m\ncontains\nsubroutine s(a,b)\nreal,intent(in)::a\nreal::b\nb=a**2\nend subroutine s\nend module m\n",
         "module m\ncontains\n   subroutine s(a, b)\n      real, intent(in)::a\n      real::b\n      b = a**2\n   end subroutine s\nend module m\n"},
        {"x = (/ 1, 2 /) + z\nif(a.eq.b)x=a* b\nprint*,\"hi\"\ncall foo ( a = 1 , b )\nx = a .and. .not. b\n",
         "x = (/1, 2/) + z\nif (a .eq. b) x = a*b\nprint *, \"hi\"\ncall foo(a=1, b)\nx = a .and. .not. b\n"},
        {"integer(4) :: x\nreal(8)::y\ncharacter(len=*), parameter :: s = 'a  b'\n",
         "integer(4) :: x\nreal(8)::y\ncharacter(len=*), parameter :: s = 'a  b'\n"},
        {"call f(a, b, &\n c, d)\nx = g(1, &\n& 2)\ny = 1 + &\n    2\n",
         "call f(a, b, &\n       c, d)\nx = g(1, &\n& 2)\ny = 1 + &\n    2\n"},
        {"select case (k)\ncase (1)\nx = 1\ncase default\nx = 2\nend select\n",
         "select case (k)\ncase (1)\n   x = 1\ncase default\n   x = 2\nend select\n"},
        {"outer: do i = 1, n\ninner:do j=1,n\ncycle outer\nend do inner\nend do outer\n",
         "outer: do i = 1, n\ninner: do j = 1, n\n   cycle outer\nend do inner\nend do outer\n"},
        {"  100 continue\n10 format(a)\n      goto 100\n",
         "100 continue\n10  format(a)\n    goto 100\n"},
        {"x=1; y=2 ; z=3\n",
         "x = 1; y = 2; z = 3\n"},
        {"! comment\n   ! indented comment\nx = 1 ! trailing\n\n\n\ny = 2\n",
         "! comment\n! indented comment\nx = 1 ! trailing\n\ny = 2\n"},
        {"#ifdef FOO\n   x = 1\n#endif\n",
         "#ifdef FOO\n   x = 1\n#endif\n"},
        {"!$ use omp_lib\n!$omp parallel\nx=1\n!$omp end parallel\n",
         "!$ use omp_lib\n!$omp parallel\nx = 1\n!$omp end parallel\n"},
        {"!&<\nx   =   1\n!&>\ny   =   2\nz  =  3 !&\n",
         "!&<\nx   =   1\n!&>\ny = 2\nz  =  3 !&\n"},
        {"where (a > 0)\nb = 1\nelsewhere\nb = 2\nend where\nwhere (a > 0) b = 0\n",
         "where (a > 0)\n   b = 1\nelsewhere\n   b = 2\nend where\nwhere (a > 0) b = 0\n"},
        {"associate (x => y%z)\nblock\nx = 1\nend block\nend associate\n",
         "associate (x => y%z)\n   block\n      x = 1\n   end block\nend associate\n"},
        {"type, extends(base) :: t\ninteger :: i\ncontains\nprocedure :: p\nend type t\n",
         "type, extends(base) :: t\n   integer :: i\ncontains\n   procedure :: p\nend type t\n"},
        {"use iso_c_binding, only : c_int, &\n c_double\n",
         "use iso_c_binding, only: c_int, &\n                         c_double\n"},
        {"interface operator(+)\nmodule procedure add\nend interface\n",
         "interface operator(+)\n   module procedure add\nend interface\n"},
        {"x = 1.0e-5 + 2.d+3 - y\nz = -x + (-1)\n",
         "x = 1.0e-5 + 2.d+3 - y\nz = -x + (-1)\n"},
        {"a = merge(b, c, d) * e / f\n",
         "a = merge(b, c, d)*e/f\n"},
        {"write(*,*) 'a', b\nread (unit, *) c\nallocate(x(10))\nopen(10, file='f')\n",
         "write (*, *) 'a', b\nread (unit, *) c\nallocate (x(10))\nopen (10, file='f')\n"},
        {"namelist /nml/ a, b\ndata x /1.0/\n",
         "namelist /nml/ a, b\ndata x/1.0/\n"},
        {"y = 'it''s' // \"say \"\"hi\"\"\"\n",
         "y = 'it''s'//\"say \"\"hi\"\"\"\n"},
        {"x = 'abc&\n&def'\n",
         "x = 'abc&\n&def'\n"},
        {"real function f(x) result(y)\ny = x\nend function\n",
         "real function f(x) result(y)\n   y = x\nend function\n"},
        {"enum, bind(c)\nenumerator :: red = 1\nend enum\n",
         "enum, bind(c)\n   enumerator :: red = 1\nend enum\n"},
        {"\tx = 1\n",
         "        x = 1\n"},
        {"do while (x < 10)\nx = x + 1\nend do\n",
         "do while (x < 10)\n   x = x + 1\nend do\n"},
        {"forall (i = 1:n) a(i) = 0\nforall (i = 1:n)\na(i) = 1\nend forall\n",
         "forall (i=1:n) a(i) = 0\nforall (i=1:n)\n   a(i) = 1\nend forall\n"},
        {"x = 1\r\ny = 2\r\n",
         "x = 1\ny = 2\n"},
        {"  x = 1\n  y = 2\n",
         "  x = 1\n  y = 2\n"},
        {"if (x) then\n!! doc comment\nx = 2\nend if\n",
         "if (x) then\n!! doc comment\n   x = 2\nend if\n"},
        {"x = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa + 1\n",
         "x = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa + 1\n"},
        {"x = 1 + &   ! comment\n    2\n",
         "x = 1 + &   ! comment\n    2\n"},
        {"submodule (m) sm\ncontains\nmodule procedure p\nend procedure\nend submodule sm\n",
         "submodule(m) sm\ncontains\n   module procedure p\n   end procedure\nend submodule sm\n"},
        {"do 10 i = 1, n\nx = i\n10 continue\n",
         "do 10 i = 1, n\n   x = i\n10 continue\n"},
        {"p => q\nx%y = z % w\n",
         "p => q\nx%y = z%w\n"},
        {"",
         ""},
};

void TestCases() {
    for (const Case &c : kCases) {
        const fortranfmt::Result r = fortranfmt::Format(c.in);
        if (!r.ok || r.text != c.want)
            std::fprintf(stderr, "input:\n%s\ngot (%s):\n%s\nwant:\n%s\n", c.in, r.error.c_str(), r.text.c_str(),
                         c.want);
        CHECK(r.ok && r.text == c.want);
        const fortranfmt::Result again = fortranfmt::Format(r.text);
        if (again.text != r.text) std::fprintf(stderr, "not idempotent:\n%s\n---\n%s\n", r.text.c_str(), again.text.c_str());
        CHECK(again.ok && again.text == r.text);
    }
}

void TestRefusals() {
    // Fixed form is refused, text unchanged.
    fortranfmt::Options fixed;
    fixed.fixed_form = true;
    const std::string f77 = "      program x\nc comment\n      end\n";
    fortranfmt::Result r = fortranfmt::Format(f77, fixed);
    CHECK(!r.ok && r.text == f77 && !r.error.empty());
    // Not UTF-8.
    const std::string bad = "x = 1 ! \xff\n";
    r = fortranfmt::Format(bad);
    CHECK(!r.ok && r.text == bad);
    // fprettify gives up on unpaired format-off directives; so do we.
    const std::string off = "!&>\nx=1\n";
    r = fortranfmt::Format(off);
    CHECK(!r.ok && r.text == off);
    // fprettify loses the middle of a string continued over several lines; that is refused.
    const std::string str = "write(str,'(a)') 'Print &&&&&\n   &&&&&\n   &&&&&'\nend\n";
    r = fortranfmt::Format(str);
    CHECK(!r.ok && r.text == str && r.error_line == 1);
    // ... as is an unterminated string continued to the end of the file (fprettify never returns).
    const std::string eof = "program p\nz = 'abc&\n! c\n";
    r = fortranfmt::Format(eof);
    CHECK(!r.ok && r.text == eof);
    // DEC `8e` exponents: fprettify joins `8e + 48e` into one token; refused.
    const std::string dec = "r4 = 8e + 48e\n";
    r = fortranfmt::Format(dec);
    CHECK(!r.ok && r.text == dec);
    // Empty input stays empty.
    r = fortranfmt::Format("");
    CHECK(r.ok && r.text.empty());
}

void TestHostile() {
    // Deep nesting, long lines and many continuations neither crash nor hang.
    std::string deep = "x = ";
    for (int i = 0; i < 20000; i++) deep += '(';
    deep += "1";
    for (int i = 0; i < 20000; i++) deep += ')';
    deep += "\n";
    fortranfmt::Result r = fortranfmt::Format(deep);
    CHECK(r.ok || r.text == deep);
    std::string unbalanced = "x = f(";
    for (int i = 0; i < 20000; i++) unbalanced += "a, (";
    unbalanced += "\n";
    r = fortranfmt::Format(unbalanced);
    CHECK(r.ok || r.text == unbalanced);
    std::string nested;
    for (int i = 0; i < 3000; i++) nested += "do i = 1, n\n";
    for (int i = 0; i < 3000; i++) nested += "end do\n";
    r = fortranfmt::Format(nested);
    CHECK(r.ok);
    std::string huge(3u << 20, 'a');
    huge = "x = " + huge + "\n";
    r = fortranfmt::Format(huge);
    CHECK(!r.ok && r.text == huge);
    std::string conts = "x = 1";
    for (int i = 0; i < 30000; i++) conts += " + &\n a";
    conts += "\n";
    r = fortranfmt::Format(conts);
    CHECK(!r.ok && r.text == conts);
    std::string quotes(100000, '\'');
    r = fortranfmt::Format(quotes);
    CHECK(r.ok || r.text == quotes);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);
    TestCases();
    TestRefusals();
    TestHostile();
    std::printf("fortran_format_test: %d checks passed\n", g_checks);
    return 0;
}
