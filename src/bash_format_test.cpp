// Coverage for bash_format.h/.cpp: mep's own Bash formatter. Each case is
// an input/expected-output pair, checked once for the layout and once fed
// back through Format() to prove the result is a fixed point.
//
// `mep-bash-format-test --format [--tabs|--spaces N] [--case-indent] FILE...`
// prints each file formatted instead (or the reason it was left alone, on
// stderr), which is how the formatter is compared with shfmt on a corpus.

#include "bash_format.h"

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

std::string Fmt(const std::string &src, const bashfmt::Options &opt) {
    bashfmt::Result r = bashfmt::Format(src, opt);
    if (!r.ok) {
        std::fprintf(stderr, "FORMAT FAILED (line %d): %s\nsource:\n%s\n", r.error_line, r.error.c_str(), src.c_str());
        std::abort();
    }
    return r.text;
}

void Same(const std::string &src, const std::string &want, bashfmt::Options opt = bashfmt::Options()) {
    checks++;
    const std::string got = Fmt(src, opt);
    if (got != want) {
        std::fprintf(stderr, "MISMATCH\n--- input ---\n%s--- want ---\n%s--- got ---\n%s", src.c_str(), want.c_str(),
                     got.c_str());
        std::abort();
    }
    const std::string again = Fmt(got, opt);
    if (again != got) {
        std::fprintf(stderr, "NOT IDEMPOTENT\n--- first ---\n%s--- second ---\n%s", got.c_str(), again.c_str());
        std::abort();
    }
}

void Unchanged(const std::string &src) { Same(src, src); }

void Refused(const std::string &src, int line) {
    checks++;
    bashfmt::Result r = bashfmt::Format(src);
    CHECK(!r.ok);
    CHECK(r.text == src);
    CHECK(r.error_line == line);
}

int FormatFiles(int argc, char **argv) {
    bashfmt::Options opt;
    int status = 0;
    for (int k = 2; k < argc; k++) {
        if (std::strcmp(argv[k], "--tabs") == 0) {
            opt.indent = "\t";
            continue;
        }
        if (std::strcmp(argv[k], "--spaces") == 0 && k + 1 < argc) {
            opt.indent = std::string(static_cast<size_t>(std::atoi(argv[++k])), ' ');
            continue;
        }
        if (std::strcmp(argv[k], "--case-indent") == 0) {
            opt.case_indent = 1;
            continue;
        }
        if (std::strcmp(argv[k], "--unchecked") == 0) {
            opt.unchecked = true;
            continue;
        }
        if (std::strcmp(argv[k], "--no-case-indent") == 0) {
            opt.case_indent = 0;
            continue;
        }
        std::ifstream in(argv[k], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const bashfmt::Result r = bashfmt::Format(ss.str(), opt);
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

    bashfmt::Options tabs;
    tabs.indent = "\t";

    // --- blocks ----------------------------------------------------------------
    Same("if [ \"$x\" = 1 ]\nthen\n  echo yes;\nelif [[  $y == 2  ]];then echo two\nelse\n    echo no # trailing\nfi\n",
         "if [ \"$x\" = 1 ]; then\n  echo yes\nelif [[ $y == 2 ]]; then\n  echo two\nelse\n  echo no # trailing\nfi\n");
    Same("if true; then echo a; else echo b; fi\n", "if true; then echo a; else echo b; fi\n");
    Same("for f in a b c\ndo\necho $f>out.txt 2>&1\ndone\n", "for f in a b c; do\n\techo $f >out.txt 2>&1\ndone\n", tabs);
    Unchanged("while read -r line; do echo \"$line\"; done <file\n");
    Same("x=1;y=2\n", "x=1\ny=2\n");
    Same("{ a; b; } >f\n", "{\n\ta\n\tb\n} >f\n", tabs);
    Same("(cd x &&\n  make)\n", "(cd x &&\n\tmake)\n", tabs);
    Same("f() { echo one\n  echo two; }\n", "f() {\n\techo one\n\techo two\n}\n", tabs);

    // --- functions ---------------------------------------------------------------
    Same("bar () { echo bar; }\n", "bar() { echo bar; }\n");
    Same("baz()\n{\n  true\n}\n", "baz() {\n  true\n}\n");
    Unchanged("function foo {\n  :\n}\n");
    Unchanged("function f() {\n  :\n}\n");

    // --- case ----------------------------------------------------------------
    Same("case $x in\n  a|b ) echo ab;;\n  c)\n    echo c\n    ;;\n  *) ;;\nesac\n",
         "case $x in\n  a | b) echo ab ;;\n  c)\n    echo c\n    ;;\n  *) ;;\nesac\n");
    Same("case $x in\n(a) x;;\nb) y\nesac\n", "case $x in\na) x ;;\nb) y ;;\nesac\n");
    Same("case $x in\n * )\nesac\n", "case $x in\n *) ;;\nesac\n");  // the file indents items by one space
    Unchanged("case $x in\n1)\n  ;;\nesac\n");
    Same("case $x in\n*) pkgs+=(\n  a\n  ) ;;\nesac\n", "case $x in\n*) pkgs+=(\n\ta\n) ;;\nesac\n", tabs);
    Same("case \"$e\" in '#'*) continue;; esac\n", "case \"$e\" in '#'*) continue ;; esac\n");
    Same("case $x in\n--a|\\\n--b) :;;\nesac\n", "case $x in\n--a | \\\n\t--b) : ;;\nesac\n", tabs);
    Same("@name@ () { :; }\n", "@name@ () { :; }\n");
    Unchanged("case x in\n(esac) echo esac ;;\nesac\n");

    // --- lists and continuations -----------------------------------------------
    Same("cmd1 &&\ncmd2 ||\n  cmd3\n", "cmd1 &&\n\tcmd2 ||\n\tcmd3\n", tabs);
    Same("cmd \\\n  && other\n", "cmd &&\n\tother\n", tabs);
    Same("cmd4 \\\n  --flag \\\n    arg\n", "cmd4 \\\n\t--flag \\\n\targ\n", tabs);
    Same("a | b|c\n", "a | b | c\n");
    Same("foo &&   # why\n  bar\n", "foo && # why\n\tbar\n", tabs);
    Same("foo &&\n  bar \\\n  baz\n", "foo &&\n\tbar \\\n\t\tbaz\n", tabs);
    Same("for i in \\\n  a \\\n  b; do\n  x\ndone\n", "for i in \\\n\ta \\\n\tb; do\n\tx\ndone\n", tabs);
    Same("if test -n x # why\nthen\n  :\nfi\n", "if test -n x; then # why\n  :\nfi\n");
    Same("[[ a \\\n  || b ]]\n", "[[ a ||\n\tb ]]\n", tabs);
    Unchanged("! &\ntime\n");

    // --- words -----------------------------------------------------------------
    Same("local a=$( echo hi )\n", "local a=$(echo hi)\n");
    Same("echo `date`   # c2\n", "echo $(date) # c2\n");
    Same("x=`printf '%s\\\\n' \\$HOME`\n", "x=$(printf '%s\\n' $HOME)\n");  // the command the escapes stand for
    Unchanged("x=`echo \\`date\\``\n");  // nested: kept
    Unchanged("x=\"`echo \\\"q\\\"`\"\n");                // \" in quoted backquotes: kept
    Unchanged("echo \\\"$`echo x`\n");                       // $` would become $$(
    Same("v=$( (sub) )\n", "v=$( (sub))\n");
    Same("v=$(\n  echo nested\n  echo more\n)\n", "v=$(\n\techo nested\n\techo more\n)\n", tabs);
    Same("cmd 2> /dev/null 1>&2 &> f <<< \"str\"\n", "cmd 2>/dev/null 1>&2 &>f <<<\"str\"\n");
    Unchanged("echo >&2 \"msg\"\n");                          // after the name: stays
    Same("cmd a >o \\\n  b\n", "cmd a \\\n\tb >o\n", tabs);    // later: after the last word
    Same("cat <<EOF >out x\nbody\nEOF\n", "cat x <<EOF >out\nbody\nEOF\n");  // order among themselves kept
    Same("( x ) \\\n  >>f\n", "(x) \\\n\t>>f\n", tabs);
    Unchanged("diff <(a) <(b) > >(tee x) < <(c)\n");
    Same("arr=( one two\n  three )\n", "arr=(one two\n\tthree)\n", tabs);
    Same("arr=(\n  a # first\n  b\n)\n", "arr=(\n\ta # first\n\tb\n)\n", tabs);
    Unchanged("m[a-b]=1\n");

    // --- arithmetic ------------------------------------------------------------
    Same("echo $(( 2*(3+4) )) $((-x)) $(( a<<2 )) $((x=y?1:0)) $(( ${#a[@]} - 1 ))\n",
         "echo $((2 * (3 + 4))) $((-x)) $((a << 2)) $((x = y ? 1 : 0)) $((${#a[@]} - 1))\n");
    Same("(( i += 2 ))\n((i++))\n", "((i += 2))\n((i++))\n");
    Same("for ((i=0;i<3;i++)); do echo $i; done\n", "for ((i = 0; i < 3; i++)); do echo $i; done\n");
    Same("for ((;;)); do :; done\n", "for (( ; ; )); do :; done\n");
    Same("echo $(( - -x )) $(( a - -b ))\n", "echo $((- -x)) $((a - -b))\n");

    // --- [[ ]] -----------------------------------------------------------------
    Same("[[ ( a ) && ! -f b ]]\n", "[[ (a) && ! -f b ]]\n");
    Same("if [[ a ]] &&\n   [[ b ]]; then\n  :\nfi\n", "if [[ a ]] &&\n\t[[ b ]]; then\n\t:\nfi\n", tabs);

    // --- comments and blank lines ------------------------------------------------
    Same("a=1   # one\nbb=22 # two\nccc=3      # three\n", "a=1   # one\nbb=22 # two\nccc=3 # three\n");
    Same("x=1 # c\nlong_name=2\nyy=3 # d\n", "x=1 # c\nlong_name=2\nyy=3 # d\n");
    Same("#!/bin/bash\n# header\n\n\nx=1\n", "#!/bin/bash\n# header\n\nx=1\n");
    Same("if true; then\n\n  echo a\n\n\n  echo b\n\nfi\n", "if true; then\n\n  echo a\n\n  echo b\n\nfi\n");
    Same("if x; then # c\n  :\nelse # d\n  :\nfi\n", "if x; then # c\n  :\nelse # d\n  :\nfi\n");
    Same("case $1 in\n    -h|--help)  # help\n        usage ;;\n    # comment between\n    *) die ;;\nesac\n",
         "case $1 in\n    -h | --help) # help\n        usage\n        ;;\n    # comment between\n    *) die ;;\nesac\n");

    // --- here-documents ----------------------------------------------------------
    Unchanged("cat <<EOF\n  keep   this\nEOF\n");
    Same("cat <<EOF | other\nbody\nEOF\n", "cat <<EOF | other\nbody\nEOF\n");
    Same("if x; then\ncat << 'EOF'\n  $raw\nEOF\nfi\n", "if x; then\n\tcat <<'EOF'\n  $raw\nEOF\nfi\n", tabs);
    Same("f() {\n\tcat <<-EOF\n\tbody\n\t\tmore\n\tEOF\n}\n", "f() {\n\tcat <<-EOF\n\t\tbody\n\t\t\tmore\n\tEOF\n}\n", tabs);
    Unchanged("f() {\n  cat <<-EOF\n\tbody\n\tEOF\n}\n");  // spaces: a <<- body is left as it is

    // --- left alone ----------------------------------------------------------------
    Refused("if x; then\n", 1);
    Refused("cat <<EOF\nno end\n", 1);

    std::printf("mep-bash-format-test: %d checks passed\n", checks);
    return 0;
}
