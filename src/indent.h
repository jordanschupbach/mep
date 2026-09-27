#pragma once

// Indent helpers for text entering a buffer -- the leading whitespace mep gives
// a line the moment you press Enter (or open one with o/O), the re-alignment of
// a Python else/elif/except/finally clause as it is typed, the re-indentation of
// a pasted block onto the indent of wherever it lands, and the width one
// Backspace/Delete press eats when the cursor is in an indent.
//
// These are deliberately pure string->string functions with no dependency on
// the editor, buffer or Lua (this file includes <string>, <optional> and
// <vector> and nothing else), the same "keep dependencies to an absolute
// minimum" ethos as
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
#include <vector>

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

// Re-alignment for a Python clause that belongs to a block opened further up --
// else/finally, an elif/except with a condition, or a match statement's case --
// as it is completed (mep calls this after a ':' is typed in a py/pyi buffer).
// `lines[row]` is the line being typed on; the lines above it are the context.
// Returns the new leading whitespace for that line, or nullopt to leave it
// untouched -- which is the answer for any non-clause line, a clause already at
// the right column, and every non-Python filetype.
//
// The clause is aligned with the statement it actually pairs with: the nearest
// line above it that opens a block this clause can close (`else` after
// if/elif/for/while/try/except, `elif` after if/elif, `except` after
// try/except, `finally` after try/except/else, `case` after case/match),
// skipping blank and comment lines and skipping anything indented deeper than
// the clause itself -- that is the body being closed, not the opener. Finding
// the opener is what distinguishes this from "dedent one level", which is what
// this used to do and
// which silently mis-paired a clause whenever the line above had *already*
// dedented:
//
//     if a:
//         if b:
//             return 1      <- Enter here dedents (a flow keyword ends the block)
//         else:             <- so this was typed at 4 and dedented again, to 0,
//                              pairing with `if a:` instead of `if b:`
//
// A `case` takes the column of the `case` above it, or -- when it is the first
// one -- sits one level *inside* its own `match`, which is where Python puts it.
// It is also the one clause here that is a soft keyword, so it is only treated
// as a clause at all when a match statement is genuinely open above it: an
// ordinary `case = {1: 2}` elsewhere in a file is left alone rather than
// dedented for looking like one.
//
// It only ever dedents, never indents: a clause typed shallower than its opener
// (a deliberate manual dedent, mid-edit code) is left where it is rather than
// pushed back in. When no matching opener can be found at all -- an unfinished
// buffer, a clause with nothing above it -- every hard-keyword clause falls back
// to one level out, which is the old behaviour and never over-dedents.
std::optional<std::string> ReindentDedentKeyword(const std::vector<std::string> &lines, int row,
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
