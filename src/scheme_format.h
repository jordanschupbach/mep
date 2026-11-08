#pragma once

// mep's own Scheme formatter: the builtin behind `gf` in a Scheme buffer
// (mep.format_scheme) and behind mep-scheme-lsp's textDocument/formatting.
// No Emacs, no Scheme.
//
// The layout every Scheme programmer's tools agree on is Emacs's
// `scheme-mode' -- `lisp-indent-function' set to `scheme-indent-function',
// the `scheme-indent-function' properties scheme.el puts on `let',
// `lambda', `when', `syntax-rules', `do', `dynamic-wind'..., and
// `indent-tabs-mode' off -- so that is the reference, measured against
// Emacs's own indent-region on a corpus (tools/scheme_indent_oracle.el,
// `just format-compare scheme'):
//
//   - indentation recomputed the way Emacs's `calculate-lisp-indent' and
//     `scheme-indent-function' compute it, rule for rule: a head with a
//     count N has N distinguished forms at four columns and a body at two
//     (`lisp-indent-specform'), `let' with a name after it has two, a
//     `def...' head (any case) is indented like a definition, a call's
//     arguments line up under the first one, a list whose car is not a
//     symbol lines up under its first element -- and the same column
//     caching indent-region does, so a run of lines at one depth lines up
//     with the first of them;
//   - square brackets and braces are parens, as in scheme-mode; `#;DATUM'
//     is a comment that ends with its datum, `#|...|#' nests;
//   - no whitespace just inside parens -- `( foo )' is `(foo)' -- and a
//     close paren alone on its line joins the line before it, unless that
//     line ends in a `;' comment or the parens carry a comment of their
//     own;
//   - no trailing whitespace; at most two blank lines in a row; no blank
//     lines at the end of the file, and a final newline;
//   - indentation is spaces: a line kept where it is has its tabs turned
//     into the same columns of spaces.
//
// Never touched: the text of a string, a `|...|' symbol, a `;' comment,
// a `#|...|#' or `#;DATUM' comment, or a character literal such as `#\('.
// It never breaks or joins lines otherwise and does not wrap, so it has
// no width.
//
// Deliberately unlike Emacs's indent-region:
//   - a line that starts inside a `#|...|#' or `#;DATUM' comment keeps its
//     indentation, and so does the line such a multi-line comment starts
//     on (Emacs re-indents a commented-out form as if it were code);
//   - a line holding only a single-`;' comment keeps its column (Emacs
//     sends it to `comment-column', 40);
//   - the whitespace rules above, which Emacs leaves to the author.
//
// It never returns text it has not checked: the result is read again and
// its tokens compared with the input's; a file that does not read (an
// unclosed paren or string), or a result that differs anywhere but in
// whitespace, is left alone.

#include <map>
#include <string>
#include <string_view>

namespace schemefmt {

struct Options {
    // `scheme-indent-function' properties to add or replace, keyed by the
    // operator's exact name: an integer ("1": that many distinguished
    // arguments, then a body), "defun", "let" (scheme-let-indent: one
    // more when a name follows), or the name of another operator to indent
    // like ("lambda", "do"). An empty string removes an operator's spec.
    std::map<std::string, std::string> indent;
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats Scheme source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &options = {});

/**
 * @brief Re-indentation only: Emacs's indent-region with the line rules above, no whitespace rules and no check.
 * @param text any text, balanced or not
 * @return the text with only its lines' leading blanks changed (whitespace-only lines emptied)
 */
std::string Reindent(std::string_view text, const Options &options = {});

/**
 * @brief The column Emacs's TAB (`lisp-indent-line') gives one line, with the text as it stands.
 * @param text the whole buffer, lines separated by '\n'
 * @param line 0-based line to indent
 * @param options extra indentation specs
 * @return the column, or -1 when the line starts inside a string or a comment (no change)
 */
int IndentLine(std::string_view text, int line, const Options &options = {});

}  // namespace schemefmt
