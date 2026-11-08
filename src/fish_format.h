#pragma once

// mep's own Fish formatter: the builtin behind `gf` in a Fish buffer
// (mep.format_fish), and textDocument/formatting in mep-fish-lsp.
// It lays code out the way fish_indent does (fish 4.x), byte for byte --
// `just format-compare fish` measures it -- and refuses (returning the input)
// when the result would not read back as the same words and comments, which
// fish_indent itself gets wrong in a few broken files. Like fish_indent it
// never breaks long lines, so `width` is accepted and ignored.

#include <string>
#include <string_view>

namespace fishfmt {

struct Options {
    int width = 100;
    int indent = 4;  // spaces per level (fish_indent always uses 4)
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats Fish source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = {});

}  // namespace fishfmt
