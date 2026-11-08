#pragma once

// mep's own Octave formatter: the builtin behind `gf` in an Octave buffer
// (mep.format_octave), and textDocument/formatting in mep-octave-lsp. The
// layout is MISS_HIT's `mh_style --fix`, over mep's Octave reader; see
// octave_format.cpp for what it does and where it differs, and
// `just format-compare octave` for the byte-for-byte comparison.

#include <string>
#include <string_view>

namespace octavefmt {

struct Options {
    int width = 100;  // unused: MISS_HIT's layout never re-wraps lines
    int indent = 4;   // mh_style's tab_width
    // End every statement with ';' as mh_style does. Off by default: it
    // changes what Octave prints, so a ',' that ends a statement is removed
    // instead of turned into ';'.
    bool add_semicolons = false;
    // Format again until nothing changes: mh_style itself is not idempotent.
    bool converge = true;
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats Octave source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = {});

}  // namespace octavefmt
