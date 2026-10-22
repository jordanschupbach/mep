#pragma once

// mep's own C formatter: the builtin behind `gf` in a .c buffer
// (mep.format_c) and behind mep-c-lsp's textDocument/formatting. No
// clang-format binary, no libclang.
//
// The layout is clang-format's, in LLVM style -- what `clang-format` gives a
// .c file that has no .clang-format of its own: two-space indents, an
// 80-column limit, `int *p`, braces on the line of the statement or function
// that opens them, `case` labels at the switch's own indent, a short function
// (`int f(void) { return 1; }`) kept on one line, continuation lines aligned
// after the open bracket, runs of trailing comments lined up, the
// backslashes of a multi-line #define lined up at the column limit, comments
// that run past the limit reflowed and long strings split.
//
// It gets there the way clang-format does, because it is a port of
// clang-format's own pipeline, restricted to C (see c_format_internal.h):
// the same parser into unwrapped lines, the same token annotation, the same
// penalty-based search for line breaks, the same whitespace pass.
//
// Differences from clang-format, by design:
//   - #include lines are never reordered (clang-format's LLVM style sorts
//     them within each block; the order of includes can matter in C).
//
// It never returns text it has not proof-read: the result is lexed again and
// compared with the input token for token -- only whitespace may differ,
// plus what reflowing a comment or splitting a string literal changes (the
// words of every comment and the bytes of every string stay the same, in
// order), and no token may move into or out of a preprocessor directive.
// When the check fails, `ok` is false and the input is returned unchanged.

#include <cstddef>
#include <string>
#include <string_view>

namespace cfmt {

struct Options {
    unsigned column_limit = 80;   // ColumnLimit
    unsigned indent_width = 2;    // IndentWidth
    unsigned continuation_indent_width = 4;
    bool use_tabs = false;        // UseTab: ForIndentation when true
    unsigned tab_width = 8;
    bool indent_case_labels = false;
    bool verify = true;           // proof-read the result (leave on)
    // Refuse a file whose tokens times #if branch combinations (each of
    // which is a pass over the whole file) exceed this; 0 for no limit.
    size_t max_work = 60'000'000;
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats C source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = Options());

}  // namespace cfmt
