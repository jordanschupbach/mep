#pragma once

// Auto-indent helpers for typing in a buffer -- the leading whitespace mep
// gives a line the moment you press Enter (or open one with o/O), and the
// re-alignment of a Python else/elif/except/finally clause as it is typed.
//
// These are deliberately pure string->string functions with no dependency on
// the editor, buffer or Lua (this file includes <string> and <optional> and
// nothing else), the same "keep dependencies to an absolute minimum" ethos as
// python_format.h and spell.cpp -- so the whole indent policy is unit-testable
// in isolation (see indent_test.cpp) and the editor call sites stay a couple of
// lines each.
//
// mep has no per-filetype indent configuration and no tree-sitter indents.scm:
// indentation is a fixed 4-space soft tab everywhere (kShift in editor.cpp).
// So `filetype` here is only consulted to switch on the one language with
// offside-rule structure worth reacting to as you type -- Python (extension
// "py"/"pyi", as returned by LspFiletype). Every other filetype gets the
// language-agnostic behaviour: a new line simply inherits the current line's
// leading whitespace.

#include <optional>
#include <string>

namespace mepindent {

// Columns per indent level. Matches editor.cpp's kShift (the >/< shiftwidth and
// the insert-mode Tab expansion), so auto-indent lines up with everything else.
inline constexpr const char *kShift = "    ";

// The leading whitespace for the line created by pressing Enter, given the text
// of the current line up to the cursor (`line_before_cursor`) and the buffer's
// filetype.
//
//   - Base (every filetype): the leading run of spaces/tabs of the current
//     line, so the new line lines up under it.
//   - Python: one shift deeper if the statement opens a block (its code, with a
//     trailing comment and whitespace stripped, ends in ':'); one shift
//     shallower (floored at column 0) if the statement is a flow keyword that
//     ends the block -- return/pass/break/continue/raise. The two are mutually
//     exclusive.
std::string ComputeNewlineIndent(const std::string &line_before_cursor,
                                 const std::string &filetype);

// Re-alignment for a Python clause that belongs one level out from the block
// body -- else/finally, or an elif/except with a condition -- as it is completed
// (mep calls this after a ':' is typed in a py/pyi buffer). Returns the new
// leading whitespace for `current_line` (its existing indent minus one shift,
// floored at column 0), or nullopt to leave the line untouched -- which is the
// answer for any non-clause line, an already-column-0 clause, and every
// non-Python filetype. Single-level only: it dedents one shift rather than
// hunting the matching opener's column, which is right for the common case and
// never over-dedents.
std::optional<std::string> ReindentDedentKeyword(const std::string &current_line,
                                                 const std::string &filetype);

}  // namespace mepindent
