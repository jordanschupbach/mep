#pragma once

// mep's own Tcl formatter: the builtin behind `gf` in a Tcl buffer
// (mep.format_tcl), and textDocument/formatting in mep-tcl-lsp.
// A port of tclfmt (tclint 0.6.2, MIT), checked byte for byte against it
// with `just format-compare tcl`; see tcl_format.cpp.

#include <string>
#include <string_view>

namespace tclfmt {

// Defaults are tclfmt's own (no tclint.toml).
struct Options {
    int width = 100;  // unused: tclfmt never wraps lines (kept for the callers)
    int indent = 4;   // spaces per level (--indent N)
    bool tabs = false;  // indent with one tab per level (--indent tab)
    int max_blank_lines = 2;  // --max-blank-lines
    bool spaces_in_braces = false;  // --spaces-in-braces: `{ x }` for one-line bodies
    bool indent_namespace_eval = true;  // --no-indent-namespace-eval turns it off
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats Tcl source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = {});

}  // namespace tclfmt
