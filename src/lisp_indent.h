#ifndef MEP_LISP_INDENT_H
#define MEP_LISP_INDENT_H

#include <string>
#include <vector>

// Standard Common Lisp indentation, as Emacs/SLIME's lisp-mode does it,
// over plain text. Two callers: mep's Enter-key auto-indent asks for one
// line's column (LispIndentForLine), and mep's own Common Lisp language
// server re-indents a whole document for textDocument/formatting
// (LispReindent).
//
// Deliberately its own translation unit with no dependency on the reader
// (lisp_reader.h) or the generated COMMON-LISP name table: the editor
// links this file alone, and the text above the cursor is usually
// incomplete -- an open `(defun` with no close paren is the normal state
// of a line being typed -- so this counts open forms with a small
// character scanner rather than parsing anything.
//
// The rules, in order:
//   - A line outside every form is at column 0.
//   - A line inside a string or a `#|...|#` comment is not code: the
//     formatter leaves it alone, and LispIndentForLine answers with the
//     previous line's indentation (what typing on in a docstring wants).
//   - Inside a form whose operator has distinguished arguments -- `defun`
//     (2: name, lambda list), `let`/`when`/`unless`/`lambda`/`flet`/
//     `labels`/`dolist`/`handler-case`/... (1), `destructuring-bind`/
//     `multiple-value-bind`/`with-slots`/`do` (2), any `def...`,
//     `with-...` or `do-...` macro (1) -- a distinguished argument on its
//     own line is indented 4 past the open paren and every body form 2.
//   - Inside an ordinary call, arguments align with the first argument
//     when it is on the operator's line, and otherwise with the operator
//     itself (1 past the open paren).
//   - A list whose first element is not a symbol (`((a 1) (b 2))`, a
//     `cond` clause), a quoted list, a vector, a lambda list or a binding
//     list aligns its elements with its first element.
//   - flet/labels/macrolet definitions and handler-case/restart-case
//     clauses indent their bodies like a defun: 2 past their own paren.
//
// Columns are byte columns, as everywhere else in mep's LSP code.

/**
 * @brief The column a line should be indented to, given only the lines above it.
 * @param lines the document's text, one entry per line (no trailing newlines)
 * @param line the 0-based line to indent; lines at and after it are never read except for that
 *             line's own current indentation, used when it starts inside a string or block comment
 * @return the column to indent to: 0 at top level; the previous line's indentation inside a string or `#|` comment
 */
int LispIndentForLine(const std::vector<std::string> &lines, int line);

/**
 * @brief Re-indents a whole document with LispIndentForLine's rules, touching nothing but leading whitespace.
 * @param lines the document's text, one entry per line
 * @return the re-indented lines, same count. Lines that start inside a string or a block comment, and
 *         lines that start with a `;;;` or single-`;` comment, are returned unchanged; whitespace-only lines
 *         become empty.
 */
std::vector<std::string> LispReindent(const std::vector<std::string> &lines);

/**
 * @brief The number of distinguished arguments an operator takes for indentation, or -1 for an ordinary call.
 * @param upper_name the operator's canonical (upper-case) name with no package prefix, e.g. "DESTRUCTURING-BIND"
 */
int LispIndentDistinguishedArgs(const std::string &upper_name);

#endif  // MEP_LISP_INDENT_H
