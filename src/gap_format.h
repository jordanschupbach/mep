#pragma once

// mep's own GAP formatter: the builtin behind `gf` in a GAP buffer
// (mep.format_gap), and textDocument/formatting in mep-gap-lsp.
// GAP has no standard formatter, so this one re-indents: block bodies one
// step in, the way GAP's own function printer (`Print(f)`) lays a function
// out, continuation lines one step deeper, trailing blanks dropped. Tokens,
// comments and line breaks are kept exactly (gap_format.cpp).

#include <string>
#include <string_view>

namespace gapfmt {

struct Options {
    int width = 100;  // accepted for the common interface; lines are never re-wrapped
    int indent = 4;   // columns per block level (GAP's printer uses 4)
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats GAP source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = {});

}  // namespace gapfmt
