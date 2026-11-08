// mep's Zig formatter, driven directly.
//
// `mep-zig-format-test` runs the checks below;
// `mep-zig-format-test --format [-w N] [--indent N] [--zon|--auto] FILE...`
// prints each file formatted ('-' reads stdin; Zig mode by default, as
// `zig fmt --stdin` reads), for tools/format_compare.py. The expected
// outputs below are `zig fmt` 0.16's.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "zig_format.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

int FormatFiles(int argc, char **argv) {
    zigfmt::Options opts;
    opts.mode = zigfmt::Mode::zig;
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
        if (arg == "--zon" || arg == "--auto" || arg == "--zig") {
            opts.mode = arg == "--zon" ? zigfmt::Mode::zon : arg == "--auto" ? zigfmt::Mode::auto_ : zigfmt::Mode::zig;
            continue;
        }
        std::stringstream ss;
        if (arg == "-") {
            ss << std::cin.rdbuf();
        } else {
            std::ifstream in(arg, std::ios::binary);
            ss << in.rdbuf();
        }
        const zigfmt::Result r = zigfmt::Format(ss.str(), opts);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", arg.c_str(), r.error_line, r.error.c_str());
            status = 1;
            continue;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}


std::string Fmt(const std::string &src, zigfmt::Mode mode = zigfmt::Mode::zig) {
    zigfmt::Options opts;
    opts.mode = mode;
    const zigfmt::Result r = zigfmt::Format(src, opts);
    if (!r.ok) return "<refused: " + r.error + ">";
    return r.text;
}

/// Formats `src`, checks the expected text, and that formatting it again is a no-op.
bool Same(const std::string &src, const std::string &want, zigfmt::Mode mode = zigfmt::Mode::zig) {
    const std::string got = Fmt(src, mode);
    if (got != want) {
        std::fprintf(stderr, "--- input\n%s--- want\n%s--- got\n%s---\n", src.c_str(), want.c_str(), got.c_str());
        return false;
    }
    return Fmt(got, mode) == got;
}

void RunChecks() {
    // Spacing, indentation, a trailing comma keeps an init one per line.
    CHECK(Same("const   std=@import(\"std\");\npub fn main( ) void{\n_=std;\n}\n",
               "const std = @import(\"std\");\npub fn main() void {\n    _ = std;\n}\n"));
    CHECK(Same("const x = .{.a=1,.b=2};\n", "const x = .{ .a = 1, .b = 2 };\n"));
    CHECK(Same("const x = .{.a=1,.b=2,};\n", "const x = .{\n    .a = 1,\n    .b = 2,\n};\n"));
    // Array init columns line up.
    CHECK(Same("const a = .{1,2,3,\n44,5,6,\n};\n", "const a = .{\n    1,  2, 3,\n    44, 5, 6,\n};\n"));
    // Comments survive, trailing whitespace goes.
    CHECK(Same("const x = 1; // hi   \n\n\n\n// two\nconst y = 2;\n",
               "const x = 1; // hi\n\n// two\nconst y = 2;\n"));
    // zig fmt: off/on leaves the region alone.
    CHECK(Same("// zig fmt: off\nconst   x  =  1;\n// zig fmt: on\nconst   y  =  2;\n",
               "// zig fmt: off\nconst   x  =  1;\n// zig fmt: on\nconst y = 2;\n"));
    // Quoted identifiers: unquoted when they can be, primitives and _ kept where they shadow.
    CHECK(Same("const @\"foo\" = @\"u8\";\nconst x = @\"_\";\n", "const foo = @\"u8\";\nconst x = @\"_\";\n"));
    CHECK(Same("const e = enum { @\"a\", @\"_\" };\nconst s = struct { @\"_\": u8 };\n",
               "const e = enum { a, @\"_\" };\nconst s = struct { _: u8 };\n"));
    CHECK(Same("const @\"a\\x62\" = 1;\n", "const ab = 1;\n"));
    // Nested cast builtins are put in canonical order.
    CHECK(Same("const p = @alignCast(@ptrCast(x));\n", "const p = @ptrCast(@alignCast(x));\n"));
    // callconv(.@"inline") on a declaration becomes `inline`.
    CHECK(Same("fn f() callconv(.@\"inline\") void {}\n", "inline fn f() void {}\n"));
    // A UTF-8 BOM and CRs are dropped.
    CHECK(Same("\xef\xbb\xbf" "const x = 1;\r\n", "const x = 1;\n"));
    // ZON, explicitly and as the fallback of auto mode.
    CHECK(Same(".{.a=1,.b=.{2,3}}\n", ".{ .a = 1, .b = .{ 2, 3 } }\n", zigfmt::Mode::zon));
    CHECK(Same(".{.a=1,.b=.{2,3}}\n", ".{ .a = 1, .b = .{ 2, 3 } }\n", zigfmt::Mode::auto_));
    CHECK(Same(".{.a=1}\n", ".{ .a = 1 }\n"));  // zig fmt --stdin reads it as Zig, too
    // Empty input stays empty.
    CHECK(Same("", ""));

    // Refusals: a syntax error reports Zig's first error and returns the input.
    {
        const std::string bad = "const x = 1;\nfn f( {\n";
        zigfmt::Options opts;
        opts.mode = zigfmt::Mode::zig;
        const zigfmt::Result r = zigfmt::Format(bad, opts);
        CHECK(!r.ok);
        CHECK(r.text == bad);
        CHECK(r.error_line == 2);
        CHECK(!r.error.empty());
    }
    {
        // zig fmt prints `noinline inline fn`, which does not parse: refused.
        const std::string src = "noinline fn foo() callconv(.@\"inline\") void {}\n";
        const zigfmt::Result r = zigfmt::Format(src);
        CHECK(!r.ok);
        CHECK(r.text == src);
        CHECK(r.error.find("self-check") != std::string::npos);
    }
    {
        // UTF-16 is not read.
        const std::string src = std::string("\xff\xfe", 2) + std::string("c\0", 2);
        CHECK(!zigfmt::Format(src).ok);
    }
    {
        // Nesting past the parser's cap is a parse error, not a crash.
        std::string deep = "const x = ";
        for (int k = 0; k < 20000; k++) deep += '(';
        deep += '1';
        for (int k = 0; k < 20000; k++) deep += ')';
        deep += ";\n";
        CHECK(!zigfmt::Format(deep).ok);
        std::string deep_ok = "const x = ";
        for (int k = 0; k < 200; k++) deep_ok += '(';
        deep_ok += '1';
        for (int k = 0; k < 200; k++) deep_ok += ')';
        deep_ok += ";\n";
        CHECK(Same(deep_ok, deep_ok));
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);
    RunChecks();
    std::printf("zig_format_test: %d checks passed\n", g_checks);
    return 0;
}
