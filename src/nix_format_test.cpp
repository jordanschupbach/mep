// Coverage for nix_format.h: mep's own Nix formatter (nixfmt's layout).
// Each case is an input/expected pair at nixfmt's test width of 80,
// checked once for the layout and once fed back through Format() to prove
// it is a fixed point. The expected outputs are nixfmt 1.3.1's.
//
// `mep-nix-format-test --format [-w N] [--indent N] [--strict] FILE...`
// prints each file formatted instead (the reason a file was left alone on
// stderr), which is how the formatter is compared with nixfmt on a corpus.

#include "nix_format.h"
#include "nix_format_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
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

nixfmt::Options W80() {
    nixfmt::Options o;
    o.width = 80;
    return o;
}

void Same(const std::string &src, const std::string &want, const nixfmt::Options &opts = W80()) {
    checks++;
    const nixfmt::Result r = nixfmt::Format(src, opts);
    if (!r.ok) {
        std::fprintf(stderr, "FORMAT FAILED (line %d): %s\nsource:\n%s\n", r.error_line, r.error.c_str(), src.c_str());
        std::abort();
    }
    if (r.text != want) {
        std::fprintf(stderr, "MISMATCH\n--- input ---\n%s--- want ---\n%s--- got ---\n%s", src.c_str(), want.c_str(),
                     r.text.c_str());
        std::abort();
    }
    const nixfmt::Result again = nixfmt::Format(r.text, opts);
    if (!again.ok || again.text != r.text) {
        std::fprintf(stderr, "NOT IDEMPOTENT\n--- first ---\n%s--- second ---\n%s", r.text.c_str(), again.text.c_str());
        std::abort();
    }
}

void Unchanged(const std::string &src) { Same(src, src); }

int FormatFiles(int argc, char **argv) {
    nixfmt::Options opts;
    int status = 0;
    for (int k = 2; k < argc; k++) {
        const std::string arg = argv[k];
        if ((arg == "-w" || arg == "--width") && k + 1 < argc) {
            opts.width = std::atoi(argv[++k]);
            continue;
        }
        if (arg == "--indent" && k + 1 < argc) {
            opts.indent = std::atoi(argv[++k]);
            continue;
        }
        if (arg == "--ir") {
            opts.dump_ir = true;
            continue;
        }
        if (arg == "--no-check") {
            opts.check = false;
            continue;
        }
        if (arg == "--strict") {
            opts.strict = true;
            continue;
        }
        std::stringstream ss;
        if (arg == "-") {
            ss << std::cin.rdbuf();
        } else {
            std::ifstream in(arg, std::ios::binary);
            ss << in.rdbuf();
        }
        const nixfmt::Result r = nixfmt::Format(ss.str(), opts);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", arg.c_str(), r.error_line, r.error.c_str());
            status = 1;
            continue;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}
}  // namespace

// `--depth FILE...`: the tree depth of each file, for checking the nesting cap against real code.
static int Depths(int argc, char **argv) {
    for (int k = 2; k < argc; k++) {
        std::ifstream in(argv[k], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const nixfmt::detail::ParseResult r = nixfmt::detail::ParseFile(ss.str(), false);
        std::printf("%d %s\n", r.ok ? nixfmt::detail::MaxDepth(r.file) : -1, argv[k]);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "--depth") == 0) return Depths(argc, argv);

    Same("{pkgs,lib,...}:\n{ a=1; b = [1 2 3]; c = \"x\"; }",
         "{ pkgs, lib, ... }:\n{\n  a = 1;\n  b = [\n    1\n    2\n    3\n  ];\n  c = \"x\";\n}\n");
    Same("{ stdenv, fetchurl }:\nstdenv.mkDerivation { pname = \"hello\"; version = \"2.12\"; src = fetchurl { url = \"mirror://gnu/hello/hello-2.12.tar.gz\"; sha256 = \"abc\"; }; }",
         "{ stdenv, fetchurl }:\nstdenv.mkDerivation {\n  pname = \"hello\";\n  version = \"2.12\";\n  src = fetchurl {\n    url = \"mirror://gnu/hello/hello-2.12.tar.gz\";\n    sha256 = \"abc\";\n  };\n}\n");
    Same("let a = 1; b = 2; in a + b",
         "let\n  a = 1;\n  b = 2;\nin\na + b\n");
    Same("if x then y else if z then w else v",
         "if x then\n  y\nelse if z then\n  w\nelse\n  v\n");
    Same("{ buildInputs = [ a b ] ++ lib.optionals stdenv.isLinux [ c d ] ++ [ e ]; }",
         "{\n  buildInputs = [\n    a\n    b\n  ]\n  ++ lib.optionals stdenv.isLinux [\n    c\n    d\n  ]\n  ++ [ e ];\n}\n");
    Same("f: x: y: f x y",
         "f: x: y:\nf x y\n");
    Same("{ a ? 1, b, ... }@args: a",
         "{\n  a ? 1,\n  b,\n  ...\n}@args:\na\n");
    Same("with pkgs; [ hello git ]",
         "with pkgs;\n[\n  hello\n  git\n]\n");
    Same("assert x != null; x",
         "assert x != null;\nx\n");
    Same("{ x = ''\n    line one\n      line two\n  ''; }",
         "{\n  x = ''\n    line one\n      line two\n  '';\n}\n");
    Same("{ s = \"a ${b} c\"; t = ''x''; }",
         "{\n  s = \"a ${b} c\";\n  t = \"x\";\n}\n");
    Same("{\n  # comment\n  a = 1; # trailing\n\n\n  b = 2;\n}",
         "{\n  # comment\n  a = 1; # trailing\n\n  b = 2;\n}\n");
    Same("{ inherit a b; inherit (lib) mkIf mkOption types optional; }",
         "{\n  inherit a b;\n  inherit (lib)\n    mkIf\n    mkOption\n    types\n    optional\n    ;\n}\n");
    Same("x: { a = x.y or 3; b = x ? y; c = !x; d = -1; }",
         "x: {\n  a = x.y or 3;\n  b = x ? y;\n  c = !x;\n  d = -1;\n}\n");
    Same("[ (f x) (g y) ]",
         "[\n  (f x)\n  (g y)\n]\n");
    Same("{ a.b.c = 1; \"d e\" = 2; ${x} = 3; }",
         "{\n  a.b.c = 1;\n  \"d e\" = 2;\n  ${x} = 3;\n}\n");
    Same("rec { a = 1; b = a; }",
         "rec {\n  a = 1;\n  b = a;\n}\n");
    Same("{ veryLongAttributeNameNumberOne = someFunction argumentNumberOne argumentNumberTwo argumentNumberThree; }",
         "{\n  veryLongAttributeNameNumberOne =\n    someFunction argumentNumberOne argumentNumberTwo\n      argumentNumberThree;\n}\n");
    Same("/* block\n   comment */\n{ }",
         "/*\n  block\n  comment\n*/\n{ }\n");
    Same("{ x = a // { b = 1; }; }",
         "{\n  x = a // {\n    b = 1;\n  };\n}\n");

    // Already formatted input comes back as it is.
    Unchanged("{ pkgs, ... }:\n{\n  environment.systemPackages = with pkgs; [ git ];\n}\n");

    // --strict: the layout stops depending on where the input broke lines.
    Same("x: [\n  1\n]", "x: [\n  1\n]\n");
    {
        nixfmt::Options strict = W80();
        strict.strict = true;
        Same("x: [\n  1\n]", "x: [ 1 ]\n", strict);
        Same("f {\n  a = 1;\n}", "f { a = 1; }\n", strict);
    }
    {
        nixfmt::Options four = W80();
        four.indent = 4;
        Same("{ a = { b = 1; }; }", "{\n    a = {\n        b = 1;\n    };\n}\n", four);
    }

    // What does not parse is left alone, with the line of the problem.
    {
        checks++;
        const std::string bad = "{\n  a = 1;\n  b = ;\n}\n";
        const nixfmt::Result r = nixfmt::Format(bad);
        CHECK(!r.ok);
        CHECK(r.text == bad);
        CHECK(r.error_line == 3);
        CHECK(!r.error.empty());
    }
    {
        checks++;
        const nixfmt::Result r = nixfmt::Format("{ a = \"unterminated; }");
        CHECK(!r.ok);
        CHECK(r.text == "{ a = \"unterminated; }");
    }
    // A tree too deep for the recursive printer is refused, not crashed on.
    {
        checks++;
        const std::string neg = std::string(5000, '!') + "x";
        std::string app = "f";
        for (int k = 0; k < 5000; k++) app += " x";
        for (const std::string &deep : {neg, app}) {
            const nixfmt::Result r = nixfmt::Format(deep);
            CHECK(!r.ok);
            CHECK(r.text == deep);
        }
        // nixpkgs nests at most ~260 deep, well inside the cap.
        CHECK(nixfmt::Format(std::string(500, '!') + "x").ok);
    }

    std::printf("mep-nix-format-test: %d checks passed\n", checks);
    return 0;
}
