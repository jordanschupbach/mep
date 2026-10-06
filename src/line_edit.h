#ifndef MEP_LINE_EDIT_H
#define MEP_LINE_EDIT_H

// The two pieces of arithmetic every "keep this anchored to its text"
// bookkeeper in the editor needs: how one row moves when whole lines are
// inserted or removed, and how to recover that (at_row, count) when all
// you have is the line vector before and after.
//
// Marks (Editor::ShiftMarksForLineEdit) and folds (folds.h's
// ShiftFoldList) are the two consumers; both used to carry their own copy
// of the first rule, and neither had the second at all -- which is why an
// undo left both of them pointing at rows the text had moved out from
// under. Header-only and std-only so it can be unit-tested windowlessly
// (mep-fold-test).
//
// Decorations deliberately do *not* share this: they drop outright when
// their row is deleted rather than collapsing to the deletion point (see
// Editor::ShiftDecorationsForLineEdit), because a stale highlight over
// surviving text is worse than no highlight.

#include <algorithm>
#include <string>
#include <vector>

/**
 * @brief Moves one row to account for whole lines inserted or removed at `at_row`.
 * @param row The row to move.
 * @param at_row The row lines were inserted at / removed from.
 * @param count Lines inserted (positive) or removed (negative).
 * @param line_count The buffer's line count *after* the edit; the result is clamped into it.
 * @return Where that row now is.
 *
 * A row strictly inside a deleted run collapses to `at_row` -- the text
 * it named is gone, and the deletion point is the nearest thing to where
 * it was. Call this *after* the insert/erase, so `line_count` is the new
 * count.
 */
inline int ShiftRowForLineEdit(int row, int at_row, int count, int line_count) {
    if (count > 0) {
        if (row >= at_row) row += count;
    } else {
        const int removed = -count;
        if (row >= at_row + removed) {
            row += count;  // count already negative
        } else if (row >= at_row) {
            row = at_row;
        }
    }
    return std::max(0, std::min(row, line_count - 1));
}

/**
 * @brief The first row at which two versions of a buffer's text disagree.
 * @param before The earlier line vector.
 * @param after The later one.
 * @return The index of the first differing line, or the length of the shorter vector if one is a prefix of the other.
 *
 * Undo, redo and a reload replace the text outright rather than
 * reporting an (at_row, count), so this is how that edit is recovered:
 * everything above the first disagreement is untouched, so nothing
 * anchored above it moves, and the line-count difference is what
 * everything below it moves by.
 */
inline int FirstDifferingRow(const std::vector<std::string> &before, const std::vector<std::string> &after) {
    const size_t common = std::min(before.size(), after.size());
    int row = 0;
    while (row < static_cast<int>(common) && before[static_cast<size_t>(row)] == after[static_cast<size_t>(row)]) {
        row++;
    }
    return row;
}

#endif  // MEP_LINE_EDIT_H
