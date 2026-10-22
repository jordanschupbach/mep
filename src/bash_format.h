#pragma once

// mep's own Bash formatter: the builtin behind `gf` in a .sh/.bash buffer
// (mep.format_bash) and behind mep-bash-lsp's textDocument/formatting. No
// shfmt, nothing spawned.
//
// It reads the script with bash_parse.h -- the parser the Bash language
// server uses, checked against `bash -n` -- and prints the tree back in
// shfmt's layout, because that is what a formatted shell script is
// expected to look like:
//
//   - `then` and `do` on the line that opens the block (`if x; then`), a
//     body one level in, one statement per line;
//   - a block written on one line with a single statement in each body
//     stays on one line (`[[ -f x ]] && { echo y; }`, `bar() { :; }`);
//   - single spaces between words and around `|`, `&&`, `||`, `;;`,
//     `name()` with no space, `[[ a ]]` with one; redirections tight to
//     their target (`>file`, `2>&1`, `<<<"$x"`);
//   - arithmetic re-spaced, binary operators spaced and unary ones tight
//     (`$((a + 1))`, `((i++))`, `for ((i = 0; i < n; i++))`), `$[...]` as
//     `$((...))`;
//   - `$(...)` for backquotes, when the backquotes hold no backslash (with
//     one, the escapes would have to be rewritten and the text is kept);
//   - case patterns level with `case` (shfmt's default; a file that
//     indents them keeps them indented), `pat) cmd ;;` on one line when it
//     was written that way, the last item given its `;;`;
//   - a line broken after `&&`, `||` or `|`, or with a backslash inside a
//     command, stays broken there, the continuation one level in;
//   - comments where they were, a run of trailing comments aligned, at
//     most one blank line kept between statements;
//   - here-document bodies byte for byte.
//
// Where it stops short of shfmt, on purpose:
//
//   - The indent unit is the file's own (tabs, or the width its blocks
//     use), not always a tab; tabs for a file with no indentation yet.
//   - Array subscripts are left as written: shfmt re-spaces `a[i+1]` as
//     arithmetic, which turns the key of an associative array `m[a-b]`
//     into a different key.
//   - `<<-` here-document bodies are not re-indented.
//   - A `$(...)` inside an arithmetic expression is not re-formatted.
//
// Like mep's other formatters it never returns text it has not checked:
// the result is parsed again and compared with the input, command by
// command and word by word (whitespace inside arithmetic aside), with
// every comment and here-document body; any difference, or a script that
// does not parse, leaves the text alone and says why.

#include <string>
#include <string_view>

namespace bashfmt {

struct Options {
    // The indent unit: "\t", or a run of spaces. Empty: the file's own.
    std::string indent;
    // The unit for a file with no indented code to read one from.
    std::string default_indent = "\t";
    // Case patterns a level inside `case`: 1 yes, 0 no, -1 as the file does
    // (no, when it has no case statements).
    int case_indent = -1;
    // Return the layout even when the check fails (for debugging the
    // formatter itself; `ok` still reports the failure).
    bool unchecked = false;
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats a Bash script; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opt = Options());

}  // namespace bashfmt
