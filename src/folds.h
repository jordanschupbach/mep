#ifndef MEP_FOLDS_H
#define MEP_FOLDS_H

// Fold ranges and the one piece of fold logic with no editor state in it
// (NVIM_PARITY_PLAN.md Part I Phase 5). Split out of editor.h so it can be
// unit-tested windowlessly (mep-fold-test) -- the invariants below are what
// stand between a fold set and the "it broke and I can't open it" symptoms
// described on NormalizeFoldList, and those are worth covering without a
// GL context and a real window in the way.

#include "line_edit.h"

#include <algorithm>
#include <string>
#include <vector>

// A fold range. `provider` is a free-form tag (e.g. "manual", "org",
// "markdown", "treesitter") so a provider can find-and-replace just its
// own folds without disturbing another's.
struct Fold {
    int start_row = 0;
    int end_row = 0;  // inclusive
    bool closed = true;
    std::string provider = "manual";
};

/**
 * @brief Restores a fold list to the invariants every fold consumer assumes.
 * @param folds The list to repair, in place.
 * @param line_count The buffer's current line count; ranges are clamped into it.
 *
 * Three invariants, and a fold set violating any one of them reads to the
 * user as "the fold broke and I can't open it":
 *
 * 1. Every range lies inside the file. A stale end_row past EOF makes
 *    DrawPane skip the whole rest of the buffer (it jumps the row cursor
 *    to end_row, which then fails the loop bound), so the text below the
 *    fold is simply gone with no summary row to click. Plenty of buffer
 *    mutations never route through Editor::ShiftFoldsForLineEdit --
 *    undo/redo swap the whole line vector, :e! re-reads the file,
 *    mep.buf_set_lines replaces it from Lua, a collaborator's edit lands
 *    in a buffer that isn't active -- so this is not a theoretical case.
 * 2. No two folds share a range. `zf` over the same lines twice used to
 *    stack two identical folds, and since za/zo open exactly one level
 *    per press, the copy underneath kept the rows hidden -- the fold
 *    looked like it refused to open.
 * 3. Folds nest, never cross. DrawPane only collapses a fold at its
 *    *start* row and then skips to its end, so given [10,50] and [20,60]
 *    it draws the first, jumps to 51, and never reaches row 20 -- yet
 *    the cursor logic still calls rows 51-60 hidden. The result is rows
 *    that render as ordinary text but refuse the cursor, belonging to a
 *    fold with no summary line anywhere to open it from. A crossing pair
 *    is merged by widening the earlier fold to cover the later one,
 *    which nests them without losing either range (clipping the inner
 *    one instead can erase a fold the user made by hand).
 *
 * On return the list is sorted by start row ascending, then end row
 * descending -- outer before inner. That ordering is canonical, which is
 * what lets Editor::LayoutFingerprint hash the list directly: a provider
 * rebuilding the same ranges in a different order must not read as a
 * change worth writing a session file for.
 */
inline void NormalizeFoldList(std::vector<Fold> &folds, int line_count) {
    if (folds.empty()) return;
    const int last = std::max(0, line_count - 1);

    // (1) Inside the file, and still worth at least two lines.
    for (Fold &f : folds) {
        if (f.start_row > f.end_row) std::swap(f.start_row, f.end_row);
        f.start_row = std::clamp(f.start_row, 0, last);
        f.end_row = std::clamp(f.end_row, 0, last);
    }
    folds.erase(std::remove_if(folds.begin(), folds.end(), [](const Fold &f) { return f.start_row >= f.end_row; }),
                folds.end());
    if (folds.empty()) return;

    auto by_outer_first = [](const Fold &a, const Fold &b) {
        if (a.start_row != b.start_row) return a.start_row < b.start_row;
        return a.end_row > b.end_row;
    };

    // (3) Nest, never cross. Widening is monotone (an end only ever grows,
    // and never past the last line), so this converges; the pass cap is a
    // backstop, not a budget -- one pass settles anything short of a
    // deliberately pathological chain.
    std::sort(folds.begin(), folds.end(), by_outer_first);
    for (int pass = 0; pass < 4; pass++) {
        bool changed = false;
        for (size_t i = 0; i < folds.size(); i++) {
            for (size_t j = i + 1; j < folds.size(); j++) {
                // Sorted by start, so folds[j].start_row >= folds[i].start_row:
                // they cross exactly when the later one opens inside the
                // earlier and closes after it.
                if (folds[j].start_row <= folds[i].end_row && folds[j].end_row > folds[i].end_row) {
                    folds[i].end_row = folds[j].end_row;
                    changed = true;
                }
            }
        }
        if (!changed) break;
        std::sort(folds.begin(), folds.end(), by_outer_first);
    }

    // (2) One fold per range. Widening above can have made two of them
    // identical, so this runs after it rather than before. The survivor
    // keeps the collapsed state if *either* had it (losing a collapse is
    // the visible half of a merge) and the hand-made provider tag if
    // either was hand-made, so Editor::WorkspaceFoldsJson still saves it
    // whole rather than treating it as a rebuildable provider fold.
    std::vector<Fold> unique;
    unique.reserve(folds.size());
    for (const Fold &f : folds) {
        if (!unique.empty() && unique.back().start_row == f.start_row && unique.back().end_row == f.end_row) {
            unique.back().closed = unique.back().closed || f.closed;
            if (f.provider == "manual") unique.back().provider = "manual";
            continue;
        }
        unique.push_back(f);
    }
    folds.swap(unique);
}

/**
 * @brief Moves every fold boundary to account for lines inserted or removed at `at_row`.
 * @param folds The list to shift, in place.
 * @param at_row The row lines were inserted at / removed from.
 * @param count Lines inserted (positive) or removed (negative).
 * @param line_count The buffer's line count *after* the edit.
 *
 * Applied independently to start_row and end_row, so a range grows or
 * shrinks correctly when the edit lands inside it rather than before it
 * -- inserting a line inside an open block extends end_row without
 * moving start_row, while inserting above the block moves both. A
 * start_row strictly inside a deleted run collapses to `at_row`; an
 * end_row stops at `at_row - 1`, since `at_row` itself is the first line
 * *after* the deleted run and so outside the fold.
 */
inline void ShiftFoldList(std::vector<Fold> &folds, int at_row, int count, int line_count) {
    if (count == 0 || folds.empty()) return;
    for (Fold &f : folds) {
        f.start_row = ShiftRowForLineEdit(f.start_row, at_row, count, line_count);
        // The fold's own last row, so a deletion eating its tail shrinks it
        // to what survived instead of annexing the line below -- see
        // ShiftRowForLineEdit's `inclusive_end`.
        f.end_row = ShiftRowForLineEdit(f.end_row, at_row, count, line_count, /*inclusive_end=*/true);
    }
    // A deletion that ate a fold's tail can leave end_row *below*
    // start_row, which is this shift's way of saying "there is nothing of
    // this fold left". Drop those here rather than letting them reach
    // NormalizeFoldList, whose first repair step swaps an inverted range
    // the right way round -- turning a fold that should be gone into a
    // plausible-looking two-line one over text it never covered.
    folds.erase(std::remove_if(folds.begin(), folds.end(), [](const Fold &f) { return f.start_row >= f.end_row; }),
                folds.end());
    // A deletion landing across two overlapping ranges can also leave them
    // crossing, which NormalizeFoldList settles (along with re-clamping
    // and the <2-line rule).
    NormalizeFoldList(folds, line_count);
}

/**
 * @brief Shifts folds after a buffer's whole line vector was swapped for another.
 * @param folds The list to shift, in place.
 * @param before The line vector that was replaced.
 * @param after The line vector now in the buffer.
 *
 * Undo and redo don't present as an (at_row, count) edit -- they replace
 * the text outright -- so the shift is recovered by finding the first row
 * the two versions disagree on and treating the line-count difference as
 * an insert or delete there. For the contiguous insert or delete an undo
 * almost always is, that is exactly the edit that was undone, run
 * backwards.
 *
 * This is the "folds became offset" bug: deleting four lines shifted the
 * folds up correctly, and undoing it put the four lines back without
 * putting the folds back, so every fold below the deletion sat four rows
 * above its own text from then on -- and the drift compounded with each
 * further undo until the fold set was worth nothing but deleting.
 */
inline void ShiftFoldListForTextSwap(std::vector<Fold> &folds, const std::vector<std::string> &before,
                                     const std::vector<std::string> &after) {
    if (folds.empty()) return;
    const int delta = static_cast<int>(after.size()) - static_cast<int>(before.size());
    if (delta == 0) return;  // same line count: no row moved, whatever the text now says
    // Everything above the first disagreement is untouched, so no fold
    // boundary above it moves -- which is the whole point: an undo that
    // restores four deleted lines must push the folds below them back
    // down by four and leave the ones above exactly where they are.
    ShiftFoldList(folds, FirstDifferingRow(before, after), delta, static_cast<int>(after.size()));
}

#endif  // MEP_FOLDS_H
