#!/usr/bin/env bash
# The Zig oracle for `just grammar-diff zig --oracle`: exit 0 when Zig's own
# parser (std.zig.Ast, as `zig fmt` runs it) reads the file without a syntax
# error. Nothing is analysed or compiled: `zig ast-check` would also report
# AstGen errors (an unused local, an undeclared name), which are not syntax.
# Uses $ZIG, else `zig` on PATH (the dev shell has it).
exec "${ZIG:-zig}" fmt --stdin < "$1" > /dev/null 2>&1
