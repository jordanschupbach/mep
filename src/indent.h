#pragma once

// Indent helpers for text entering a buffer -- the leading whitespace mep gives
// a line the moment you press Enter (or open one with o/O), the re-alignment of
// a Python else/elif/except/finally clause as it is typed, the re-indentation of
// a pasted block onto the indent of wherever it lands, and the width one
// Backspace/Delete press eats when the cursor is in an indent.
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

// --- Pasting -------------------------------------------------------------

// The leading run of spaces/tabs of `line`. Exposed because the editor's paste
// path needs exactly the same notion of "this line's indent" that the helpers
// here use internally.
std::string LeadingWhitespace(const std::string &line);

// The display width of a leading-whitespace run, tabs advanced to the next
// multiple of kShift's width -- so a tab-indented source and a space-indented
// one can be compared level for level.
int IndentWidth(const std::string &whitespace);

// Re-indents a block of text on its way into the buffer (a paste) so it keeps
// its own internal shape but sits at `target_indent` instead of wherever it was
// copied from. Single-line text is returned untouched -- there is no shape to
// keep -- so this only ever rewrites a genuinely multi-line paste.
//
//   - The block's baseline is the indent of its first non-blank line, expanded
//     to display columns. Every line is shifted by the same amount (its own
//     indent minus that baseline, floored at column 0), so nesting inside the
//     block survives exactly, whether the source used tabs, spaces, or both.
//   - `target_indent` is prefixed to every line, i.e. the whole block lands
//     where it is being pasted. When `indent_first_line` is false the first
//     line is only stripped, not prefixed: it is being spliced onto text
//     that's already on the line (a charwise paste, or an Insert-mode one at
//     the cursor), and that text supplies its indent.
//   - Relative indent is emitted as tabs when `target_indent` is itself all
//     tabs (a tab-indented file), spaces otherwise, so a paste doesn't mix
//     the two. Blank lines are emitted empty rather than padded with the
//     target indent -- except a trailing partial line, which keeps the indent
//     so typing continues where the block left off.
std::string ReindentPastedText(const std::string &text, const std::string &target_indent,
                               bool indent_first_line);

// Where a multi-line paste should actually be spliced into `line`, given the
// byte column `col` the caller would otherwise have used (a charwise p/P's own
// insertion point, or the Insert-mode cursor). It is `col` itself everywhere
// except inside the line's leading whitespace, which is moved forward to the
// end of that whitespace.
//
// This is what keeps a multi-line paste from landing a column or two in from
// the margin: a cursor sitting anywhere in a line's indent -- column 0 of an
// indented line is the everyday case, a mouse click or `0` or arrowing down a
// column away -- would otherwise splice the block's first line *inside* the
// indent, at `col`, while `target_indent` put every line after it at the line's
// real indent. Landing the whole block at that same indent is both what
// `:set pasteindent` means and the only answer that keeps the block's own
// shape: the leading whitespace `col` was pointing into is not a position in
// the text, it is the indentation the block is being pasted at.
//
// Single-line pastes must not go through this: a word pasted at column 0 of an
// indented line belongs at column 0 (ReindentPastedText leaves single-line text
// alone for the same reason).
int PasteSpliceCol(const std::string &line, int col);

// --- Backspace/Delete over an indent -------------------------------------

// How many bytes an Insert-mode Backspace at byte `col` of `line` should delete
// for the press to eat one whole indent level rather than a single space --
// mep's indent is a soft tab (kShift spaces), and removing it should cost one
// press, not four. 0 means "nothing special here": the caller deletes one
// character as usual.
//
// Non-zero only while the cursor sits in the line's leading whitespace with a
// space directly behind it; the answer is then the run of spaces back to the
// previous kShift-column tab stop (a literal '\t' is already a single press, so
// it is left to the caller). Text anywhere before the cursor -- including a
// single character -- means an ordinary Backspace: mid-line spaces between
// words are not indentation.
int IndentBackspaceWidth(const std::string &line, int col);

// The forward counterpart, for Delete: the run of spaces from `col` up to the
// next tab stop, when `col` is inside the line's leading whitespace and lands
// on a space. Same 0 = "delete one character as usual" convention.
int IndentDeleteWidth(const std::string &line, int col);

}  // namespace mepindent
