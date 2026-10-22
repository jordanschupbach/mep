#pragma once

// mep's own Common Lisp formatter: the builtin behind `gf` in a .lisp/
// .lsp/.cl/.asd buffer (mep.format_lisp) and behind mep-lisp-lsp's
// textDocument/formatting. No Emacs, no Lisp image.
//
// There is no cljfmt for Common Lisp; the layout every Lisp programmer's
// tools agree on is Emacs's -- `lisp-mode' with `lisp-indent-function'
// set to `common-lisp-indent-function' (what SLIME and Sly set up) and
// `indent-tabs-mode' off -- so that is the reference, measured against
// Emacs's own indent-region on a corpus (tools/cl_indent_oracle.el):
//
//   - indentation recomputed the way Emacs's `calculate-lisp-indent' and
//     cl-indent.el's `common-lisp-indent-function' compute it, rule for
//     rule: the indentation specs of `defun', `let', `flet', `case',
//     `destructuring-bind', `handler-case', `loop', `tagbody', `do'...,
//     a `def...' head treated like `defun' and a `with-...'/`do-...'
//     head like a body form, package prefixes ignored (`sb-c:defknown'),
//     a call's arguments under the first one, a quoted list's elements
//     under its first, a keyword under the keyword above it -- and the
//     same column caching indent-region does, so a run of lines at one
//     depth lines up with the first of them;
//   - no whitespace just inside parens -- `( foo )' is `(foo)' -- and a
//     close paren alone on its line joins the line before it, unless that
//     line ends in a `;' comment or the parens carry a comment of their
//     own (`  ) ; end of #+sbcl progn' stays);
//   - no trailing whitespace; at most two blank lines in a row; no blank
//     lines at the end of the file, and a final newline;
//   - indentation is spaces: a line kept where it is has its tabs turned
//     into the same columns of spaces.
//
// Never touched: the text of a string, a `|...|' symbol, a `;' comment
// or a `#|...|#' comment, a character literal such as `#\(' or `#\Space',
// and a `#+feature' expression. It never breaks or joins lines otherwise,
// and does not wrap, so it has no width.
//
// Deliberately unlike Emacs's indent-region:
//   - a line that starts inside a `#|...|#' comment keeps its
//     indentation, and so does the line a multi-line `#|' comment starts
//     on (Emacs re-indents a block comment's lines as if they were code);
//   - a line holding only a single-`;' comment keeps its column (Emacs
//     sends it to `comment-column', 40);
//   - a `defmethod's qualifiers are counted in the top-level form that
//     holds it; Emacs finds that form with `beginning-of-defun' from
//     below the line being indented, so in a file not yet indented it
//     can land on a body line still in column 0, then stops with a scan
//     error or indents the body as if it were the lambda list (and
//     differently the second time round);
//   - where Emacs's keyword alignment walks back into the paren
//     (`(@ x (y)' then a line starting `:z') it stops with a scan error
//     and indents nothing more; here that line gets the ordinary column;
//   - the whitespace rules above, which Emacs leaves to the author.
//
// It never returns text it has not checked: the result is read again and
// its tokens compared with the input's; a file that does not read (an
// unclosed paren or string), or a result that differs anywhere but in
// whitespace, is left alone.

#include <map>
#include <string>
#include <string_view>

namespace lispfmt {

struct Options {
    // Indentation specs to add or replace, keyed by lower-case operator
    // name without a package prefix: an integer ("1": that many
    // distinguished arguments, then a body), a cl-indent list
    // ("(4 &lambda &body)", "((&whole 4 &rest (&whole 1 1 2)) &body)"),
    // "defun", one of cl-indent's functions ("lisp-indent-tagbody",
    // "lisp-indent-do", "lisp-indent-defmethod"), or the name of another
    // operator to indent like ("let", "case"). An empty string removes an
    // operator's spec.
    std::map<std::string, std::string> indent;
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats Common Lisp source; on failure `ok` is false and `text` is the input unchanged. */
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
 * @param line 0-based line to indent; text after that line is used only as Emacs would (to look ahead
 *        at a `loop' or a `defmethod')
 * @param options extra indentation specs
 * @return the column, or -1 when the line starts inside a string or a `#|...|#' comment (no change)
 */
int IndentLine(std::string_view text, int line, const Options &options = {});

}  // namespace lispfmt
