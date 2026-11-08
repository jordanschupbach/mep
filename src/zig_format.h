#pragma once

// mep's own Zig formatter: the builtin behind `gf` in a Zig buffer
// (mep.format_zig), and textDocument/formatting in mep-zig-lsp.
//
// A port of Zig 0.16's lib/std/zig/Ast/Render.zig -- the code `zig fmt` runs
// -- over the port of Zig's own parser (zig_ast.h), so the output is `zig
// fmt`'s byte for byte (checked over a corpus with `just format-compare zig`).
// `zig fmt` has no options: Options::width and Options::indent are accepted
// for the shared formatter interface and ignored (the indent is always 4).
//
// Never returns what it has not checked: the result is parsed again and its
// tokens and comments compared with the input's, modulo exactly the rewrites
// `zig fmt` makes; any difference is a refusal (ok = false, input unchanged).

#include <string>
#include <string_view>

namespace zigfmt {

/// Which grammar to read the text with: Zig source, ZON (`.zon`, one
/// expression), or Zig falling back to ZON when only ZON parses.
enum class Mode { auto_, zig, zon };

struct Options {
    int width = 100;  // ignored: zig fmt has no line width
    int indent = 4;   // ignored: zig fmt always indents by 4
    Mode mode = Mode::auto_;
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats Zig source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = {});

}  // namespace zigfmt
