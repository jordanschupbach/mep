#pragma once

// mep's own Fortran formatter: the builtin behind `gf` in a free-form Fortran
// buffer, and textDocument/formatting in mep's Fortran language server.
//
// It lays code out the way fprettify 0.3.7 does at its default settings
// (indent 3, whitespace level 2, no case changes, line length 132), byte for
// byte -- `just format-compare fortran` measures it. The rules were rebuilt
// from fprettify's documented and observed behaviour; no code was taken from
// it. Like fprettify it never re-wraps lines: an over-long line is pushed
// left to end at the limit, or left exactly as written when even that does
// not fit.
//
// It refuses (ok = false, text = the input) rather than return something it
// has not checked: fixed-form source (fprettify mangles it), input that is not
// UTF-8, the cases where fprettify itself gives up (bad `!&<`/`!&>` pairs,
// unbalanced continuations it cannot map back), and any result whose tokens,
// re-read by its own free-form lexer, differ from the input's.

#include <string>
#include <string_view>

namespace fortranfmt {

struct Options {
    int width = 132;          // fprettify --line-length (0 means 1024, as in fprettify)
    int indent = 3;           // spaces per scope level
    bool fixed_form = false;  // .f/.for/.F source: refused
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats free-form Fortran; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = {});

}  // namespace fortranfmt
