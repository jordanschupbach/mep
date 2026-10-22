#pragma once

// mep's own C# formatter: the builtin behind `gf` in a .cs buffer
// (mep.format_csharp) and mep-csharp-lsp's textDocument/formatting. No .NET.
//
// The layout is `dotnet format whitespace`'s with no .editorconfig -- Roslyn's
// formatting engine with the default C# options: 4-space indentation, Allman
// braces, its spacing and new-line rules; it fixes whitespace and indentation
// and never re-wraps long lines. It gets there the way Roslyn does, because
// it is a port of Roslyn's engine (Microsoft.CodeAnalysis.Formatting and the
// C# formatting rules, at the commit the .NET 10 SDK's dotnet-format was
// built from) running on mep's port of Roslyn's parser (csharp_parser.h),
// whose trees are Roslyn's own: token-pair space and new-line operations,
// indentation blocks, anchors and suppressions collected from the tree,
// applied by the same engine with the same interval trees, then the trivia
// between tokens (comments, doc comments, directives) re-indented by the
// same trivia formatter. See csharp_format_internal.h for the layout.
//
// Like dotnet format, a file with syntax errors is still formatted (the
// engine leaves the space next to a missing token alone); a byte-order mark
// is kept; new lines the formatter inserts are Options::new_line ("\n", as
// dotnet format on Linux; mep passes the buffer's own line ending).
//
// It never returns text it has not proof-read: the result is parsed again
// and must give the same tree -- every node and token, the same syntax
// errors, the same comments and directives up to whitespace. When the check
// fails, `ok` is false and the input is returned unchanged.
//
// Roslyn is MIT-licensed: Copyright (c) .NET Foundation and Contributors
// (https://github.com/dotnet/roslyn/blob/main/License.txt). This formatter is
// a derived work.

#include <string>
#include <string_view>

namespace csfmt {

struct Options {
    int indent_size = 4;       // indent_size
    int tab_size = 4;          // tab_width
    bool use_tabs = false;     // indent_style = tab
    std::string new_line = "\n";
    bool verify = true;        // proof-read the result (leave on)
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats C# source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = Options());

}  // namespace csfmt
