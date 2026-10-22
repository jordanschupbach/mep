#ifndef MEP_LISP_INDENT_H
#define MEP_LISP_INDENT_H

#include <string>
#include <vector>

// Standard Common Lisp indentation -- Emacs's, as lisp-mode with
// common-lisp-indent-function computes it -- over plain lines, for mep's
// Enter-key auto-indent (LispIndentForLine) and for re-indenting a whole
// document without the formatter's whitespace rules (LispReindent).
//
// Both are thin entry points into lisp_format.cpp, which holds the one
// copy of the rules (lisp_format.h lists them): what Enter gives a new
// line is what `gf' and mep-lisp-lsp's formatting give it. Neither needs
// the Lisp reader or the COMMON-LISP name table, and both take any text
// -- the text above the cursor is usually incomplete, an open `(defun'
// with no close paren being the normal state of a line being typed.
//
// Columns are byte columns for ASCII margins, as everywhere else in mep's
// LSP code (a tab in a line above counts to the next multiple of eight,
// as Emacs counts it).

/**
 * @brief The column a line should be indented to, as Emacs's TAB would indent it.
 * @param lines the document's text, one entry per line (no trailing newlines)
 * @param line the 0-based line to indent; lines after it are never read, and its own text counts only as it
 *             does to Emacs (a line starting with `:key` or `&key` lines up differently)
 * @return the column to indent to: 0 at top level; the previous line's indentation inside a string or `#|` comment
 */
int LispIndentForLine(const std::vector<std::string> &lines, int line);

/**
 * @brief Re-indents a whole document with LispIndentForLine's rules, touching nothing but leading whitespace.
 * @param lines the document's text, one entry per line
 * @return the re-indented lines, same count. Lines that start inside a string or a block comment are returned
 *         unchanged; `;;;` and single-`;` comment lines, and the line a multi-line `#|` comment starts on, keep
 *         their column (a tab in their margin becoming spaces); whitespace-only lines become empty.
 */
std::vector<std::string> LispReindent(const std::vector<std::string> &lines);

#endif  // MEP_LISP_INDENT_H
