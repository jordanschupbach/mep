#pragma once

// mep's own Crystal formatter: the builtin behind `gf` in a .cr buffer
// (mep.format_crystal). No crystal binary.
//
// The layout is `crystal tool format`'s -- there is only one, Crystal's
// formatter has no options -- and it gets there the way the compiler does,
// because it is a port of the compiler's own formatter
// (src/compiler/crystal/tools/formatter.cr, Crystal 1.19), running on mep's
// ports of the compiler's lexer and parser (crystal_syntax.h,
// crystal_parser.h). As in the original, the file is parsed, then the tree
// is visited while a second lexer walks the same source in step with it:
// comments, blank lines, the spelling of literals and where the author
// broke lines come from the token stream, the structure from the tree.
// Then the same post-passes run over the result: heredoc bodies shifted
// with their start, `case ... when x then y` rows, `=>` columns of a hash
// literal and numeric `=` values lined up, runs of trailing comments
// aligned, and the ```crystal blocks of doc comments formatted in turn.
// Macro bodies are formatted as the compiler does it too: code between
// `{% %}` tags that is valid Crystal on its own is re-formatted, the rest
// is left as written.
//
// See crystal_format_internal.h for how the port is laid out.
//
// It never returns text it has not proof-read: the result is parsed again
// and its tree compared with the input's (every node, every name and
// literal, everything but locations), and the text of every comment
// compared in order (the formatter may only change the spacing after the
// `#`). When the check fails, `ok` is false and the input is returned
// unchanged. A file that does not parse gives the compiler's own syntax
// error, word for word, with its line.
//
// Crystal is Apache-2.0 licensed (Copyright 2012-2026 Manas Technology
// Solutions); this is a derived work.

#include <string>
#include <string_view>

namespace crfmt {

struct Options {
    bool verify = true;  // proof-read the result (leave on)
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats Crystal source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = Options());

}  // namespace crfmt
