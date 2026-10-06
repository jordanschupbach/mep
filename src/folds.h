#ifndef MEP_FOLDS_H
#define MEP_FOLDS_H

// Fold ranges and the one piece of fold logic with no editor state in it
// (NVIM_PARITY_PLAN.md Part I Phase 5). Split out of editor.h so it can be
// unit-tested windowlessly (mep-fold-test) -- the invariants below are what
// stand between a fold set and the "it broke and I can't open it" symptoms
// described on NormalizeFoldList, and those are worth covering without a
// GL context and a real window in the way.

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

#endif  // MEP_FOLDS_H
