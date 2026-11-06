#pragma once

// mep's own Nix formatter: the builtin behind `gf` in a .nix buffer
// (mep.format_nix). No nixfmt binary, no Haskell.
//
// The layout is nixfmt's -- the official Nix style (RFC 166) that nixpkgs
// is formatted with. It is a port of nixfmt 1.3.1's pipeline, piece for
// piece: its lexer and parser (including how comments attach to tokens),
// its pretty-printer, and its greedy layout engine with priority groups.
// The goal is byte-identical output, and it is checked against nixfmt
// itself (see nix_format_test.cpp and help/formatting.org).
//
//   - width 100 and two-space indentation by default, like nixfmt;
//   - `strict` is nixfmt --strict: the layout no longer depends on where
//     the input broke its lines (nixfmt keeps a list or set multi-line if
//     its brackets were on different lines, for example).
//
// It never returns text it has not checked: the result is parsed again
// and compared with the input's tree, ignoring only layout, and every
// word of every comment must still be there. A file that does not parse,
// or a result that fails either check, is left alone.

#include <string>
#include <string_view>

namespace nixfmt {

struct Options {
    int width = 100;
    int indent = 2;
    bool strict = false;
    bool check = true;  // false skips the self-check (for debugging the formatter)
    bool dump_ir = false;  // debugging: `text` is the layout document, as `nixfmt --ir` prints it
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats Nix source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = {});

}  // namespace nixfmt
