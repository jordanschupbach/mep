#pragma once

// mep's own Go formatter: the builtin behind `gf` in a Go buffer
// (mep.format_go), and textDocument/formatting in mep-go-lsp. It is a port
// of cmd/gofmt (Go 1.26) without flags: go_parse.cpp for go/parser,
// go_printer*.cpp for go/printer and ast.SortImports, go_doc_comment.cpp
// for doc-comment reformatting, go_tabwriter.cpp for text/tabwriter. The
// output is gofmt's byte for byte (`just format-compare go`).
//
// gofmt has no options, so Options::width and Options::indent are ignored;
// they exist only because every mep formatter takes them. Like gofmt
// reading stdin, a fragment that is not a whole file (a list of
// declarations, or of statements) is formatted too, keeping its leading
// and trailing white space and its indentation.

#include <string>
#include <string_view>

namespace gofmt {

struct Options {
    int width = 100;  // ignored (gofmt has no line width)
    int indent = 4;   // ignored (gofmt indents with tabs)
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats Go source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = {});

}  // namespace gofmt
