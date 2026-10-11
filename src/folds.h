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
#include <cstdlib>
#include <string>
#include <tuple>
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
 * @brief The text a fold was made over: its first and last line, verbatim.
 *
 * A row number is only meaningful against one particular version of a
 * file, and a fold outlives that version -- it is written to a session
 * file, read back a day later, and in between the file may have been
 * edited by another editor, a formatter, an AI agent in a terminal pane
 * or a `git pull`. Rows alone cannot survive any of that: the fold comes
 * back sitting four lines above its own function, which is the whole of
 * the "I open a file with folds in it and everything is offset" report.
 *
 * The two lines are what lets the fold be *found* again instead
 * (RefindFoldByAnchor), so it either lands back on its own text or is
 * dropped -- never left pointing at somebody else's.
 */
struct FoldAnchor {
    std::string head;  // the fold's start row as it read when the fold was made
    std::string tail;  // its end row
    // The lines immediately outside it, which is what tells two identical
    // folds apart. Real Python repeats itself exactly -- a dozen classes
    // whose `    def __init__(self):` ... `        return None` are
    // byte-for-byte the same -- and for those the only distinguishing text
    // is the `class Thing7:` directly above. Empty when the fold sat at
    // the very top or bottom of the file, or when an older session file
    // recorded no context at all.
    std::string before;
    std::string after;
};

/**
 * @brief A fold plus the text it was made over, as a session file holds it.
 *
 * The pair is the unit the session save writes and the restore reads: the
 * range is what gets applied when the file is unchanged (the usual case,
 * and free), and the anchor is what re-derives the range when it isn't.
 */
struct AnchoredFold {
    Fold fold;
    FoldAnchor anchor;
};

/**
 * @brief Whether a line says enough about itself to find it by.
 * @param line The line to judge.
 * @return true when it has any non-whitespace content.
 *
 * A blank line (or one of pure indentation) matches hundreds of places in
 * a Python file, so matching one proves nothing -- those are the lines
 * the callers below fall back to arithmetic for.
 */
inline bool FoldAnchorUsable(const std::string &line) {
    return line.find_first_not_of(" \t\r\n") != std::string::npos;
}

/**
 * @brief Finds the rows a fold's own text occupies in a (possibly changed) line vector.
 * @param anchor The text the fold was made over.
 * @param want_start The row it used to start on -- the search's centre, since most edits move a fold a little, not a lot.
 * @param want_span end_row - start_row as it used to be; likewise the preferred distance between the two matches.
 * @param lines The text to search.
 * @param start Receives the new start row.
 * @param end Receives the new end row.
 * @return true if the fold's text was found (and `start`/`end` written), false if it is gone.
 *
 * Both the fold's own lines have to match, which already pins the range
 * down where either half alone would be ambiguous. Candidates are then
 * ranked by, in order:
 *
 * 1. How much of the surrounding context (FoldAnchor::before/after) still
 *    matches. This outranks proximity because it is the only thing that
 *    separates genuinely identical folds -- twelve classes with the same
 *    `__init__` body differ *only* in the `class Thing7:` line above, so
 *    a fold five rows from where it was but under the wrong class is the
 *    wrong answer, and the one thirty rows away under the right one is
 *    not. Blank context counts neither for nor against: a blank line
 *    above a `def` says nothing about which `def` it is.
 * 2. Proximity to where the fold already was. Most edits move a fold a
 *    little, not across the file -- and without this an insert of ten
 *    lines could hand the fold to an identical function five hundred
 *    rows further down.
 * 3. Fidelity to its old span, which is how a line added inside the fold
 *    (its end moved down by one, still the same fold) is preferred over
 *    some far-off repeat of the same last line.
 *
 * A blank end line is the one concession: `zf` over a trailing blank row
 * leaves an anchor that cannot be matched, so a matched head plus the old
 * span is accepted there. A blank *start* line is not anchorable at all
 * and the caller is told so (false), to fall back on whatever arithmetic
 * it has.
 */
inline bool RefindFoldByAnchor(const FoldAnchor &anchor, int want_start, int want_span,
                               const std::vector<std::string> &lines, int *start, int *end, int window_lo = 0,
                               int window_hi = -1) {
    if (lines.empty() || want_span <= 0 || !FoldAnchorUsable(anchor.head)) return false;
    // The window a nested fold is confined to: its parent's own new range
    // (RefindFoldsByAnchors). Outside one, the whole file.
    const int file_last = static_cast<int>(lines.size()) - 1;
    const int last = (window_hi < 0 || window_hi > file_last) ? file_last : window_hi;
    const int first = std::clamp(window_lo, 0, last);
    const bool tail_usable = FoldAnchorUsable(anchor.tail);
    /**
     * @brief How many of this candidate's context lines fail to match (0 when it has none to match).
     */
    auto missed_context = [&](int r, int e) {
        int missed = 0;
        if (FoldAnchorUsable(anchor.before) && !(r > 0 && lines[static_cast<size_t>(r - 1)] == anchor.before)) {
            missed++;
        }
        if (FoldAnchorUsable(anchor.after) && !(e < file_last && lines[static_cast<size_t>(e + 1)] == anchor.after)) {
            missed++;
        }
        return missed;
    };
    // Nothing moved this fold: its two lines, and the context either side,
    // are all still exactly where they were. That is the usual answer --
    // an unchanged file on reopen, or an edit somewhere else in the buffer
    // -- and taking it here keeps the usual answer free instead of
    // scanning the file for every fold. The context has to match too, or
    // this would quietly undo what the ranking below is for: in a file of
    // identical blocks, an insert the size of one block leaves the *next*
    // block's text sitting at the fold's old rows, and that is a different
    // block, not this fold.
    if (want_start >= first && want_start + want_span <= last &&
        lines[static_cast<size_t>(want_start)] == anchor.head &&
        (!tail_usable || lines[static_cast<size_t>(want_start + want_span)] == anchor.tail) &&
        missed_context(want_start, want_start + want_span) == 0) {
        *start = want_start;
        *end = want_start + want_span;
        return true;
    }
    int best_start = -1, best_end = -1;
    // (missed context lines, distance from where it was, span error) --
    // compared in that order, so a worse rank on an earlier term is never
    // bought back by a better one on a later term.
    std::tuple<int, int, int> best_cost{0, 0, 0};
    for (int r = first; r <= last; r++) {
        if (lines[static_cast<size_t>(r)] != anchor.head) continue;
        int e = -1;
        if (tail_usable) {
            // The end match closest to the span the fold used to have:
            // a line added inside the fold moves its end down by one, and
            // that is still the same fold.
            for (int c = r + 1; c <= last; c++) {
                if (lines[static_cast<size_t>(c)] != anchor.tail) continue;
                if (e < 0 || std::abs((c - r) - want_span) < std::abs((e - r) - want_span)) e = c;
            }
            if (e < 0) continue;  // its last line is gone: this candidate is not the fold
        } else {
            e = r + want_span;
            if (e > last) continue;
        }
        const std::tuple<int, int, int> cost{missed_context(r, e), std::abs(r - want_start),
                                             std::abs((e - r) - want_span)};
        if (best_start < 0 || cost < best_cost) {
            best_start = r;
            best_end = e;
            best_cost = cost;
        }
    }
    if (best_start < 0) return false;
    *start = best_start;
    *end = best_end;
    return true;
}

/**
 * @brief Reads a fold's anchor -- its own two lines plus the context either side -- out of a line vector.
 * @param lines The text the fold's rows are true of.
 * @param start_row The fold's start row.
 * @param end_row Its (inclusive) end row.
 * @return The anchor; empty if the rows aren't inside `lines`.
 *
 * One definition, used by everything that records an anchor (the session
 * save, the buffer-swap shift), so a fold is always looked for by the
 * same text it was filed under.
 */
inline FoldAnchor FoldAnchorFor(const std::vector<std::string> &lines, int start_row, int end_row) {
    FoldAnchor a;
    const int last = static_cast<int>(lines.size()) - 1;
    if (start_row < 0 || end_row > last || start_row > end_row) return a;
    a.head = lines[static_cast<size_t>(start_row)];
    a.tail = lines[static_cast<size_t>(end_row)];
    if (start_row > 0) a.before = lines[static_cast<size_t>(start_row - 1)];
    if (end_row < last) a.after = lines[static_cast<size_t>(end_row + 1)];
    return a;
}

/**
 * @brief The outermost closed fold hiding a row, if any.
 * @param folds The fold list to consult.
 * @param row The row in question.
 * @param fold_start_row Receives the row the hidden row actually displays on (the fold's summary line).
 * @return true when `row` is inside a closed fold rather than being one's start row.
 *
 * The *outermost* one, not whichever the vector lists first: with nesting
 * they differ, and only the outermost fold's start row is itself on
 * screen, so reporting an inner one hands the caller a row no more
 * visible than the one it asked about. Editor::IsRowHiddenByFold answers
 * for the active buffer through this; the pane fixups need it for a
 * buffer that isn't active.
 */
inline bool RowHiddenByFoldIn(const std::vector<Fold> &folds, int row, int *fold_start_row) {
    const Fold *outermost = nullptr;
    for (const Fold &f : folds) {
        if (!f.closed || row <= f.start_row || row > f.end_row) continue;
        if (outermost == nullptr || f.start_row < outermost->start_row) outermost = &f;
    }
    if (outermost == nullptr) return false;
    if (fold_start_row != nullptr) *fold_start_row = outermost->start_row;
    return true;
}

/**
 * @brief One fold's re-found position, or the news that its text is gone.
 */
struct RefoundFold {
    bool found = false;
    int start_row = 0;
    int end_row = 0;
};

/**
 * @brief Re-finds a whole fold list by its anchors, keeping folds that were nested nested.
 * @param folds The folds as they were before the text changed.
 * @param anchors One anchor per fold, taken from that same text (FoldAnchorsFor).
 * @param lines The text to find them in.
 * @return One result per input fold, in the input's order.
 *
 * Folds cannot be re-found one at a time and then simply collected: each
 * lookup is independent, so an inner fold whose own last line was
 * rewritten can match a copy of that line *below* its parent and come
 * back as a range that starts inside the parent and ends after it. That
 * is a crossing pair, and NormalizeFoldList resolves crossings by
 * widening the outer fold -- so the class the user folded silently grows
 * to cover whatever followed it. "The fold swallowed lines I never
 * selected", arrived at from a new direction.
 *
 * Nesting is known *before* the edit, though, and it is information worth
 * keeping: a fold that was inside another one is still inside it
 * afterwards or it is gone. So parents are resolved first and each child
 * is then looked for only within its parent's new range. A child with
 * nowhere to be found inside its parent is reported missing, which leaves
 * the caller to drop it (or fall back to arithmetic) rather than to place
 * it somewhere it cannot belong.
 */
inline std::vector<RefoundFold> RefindFoldsByAnchors(const std::vector<Fold> &folds,
                                                     const std::vector<FoldAnchor> &anchors,
                                                     const std::vector<std::string> &lines) {
    std::vector<RefoundFold> out(folds.size());
    if (folds.empty()) return out;
    // Outer before inner, so a parent is always resolved before its
    // children -- NormalizeFoldList's own canonical order, recomputed
    // here rather than assumed, since a caller may hand over a list that
    // has not been through it yet.
    std::vector<size_t> order(folds.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (folds[a].start_row != folds[b].start_row) return folds[a].start_row < folds[b].start_row;
        return folds[a].end_row > folds[b].end_row;
    });
    // Each fold's innermost enclosing fold in the *old* text, or none.
    std::vector<size_t> parent(folds.size(), folds.size());
    for (size_t i = 0; i < folds.size(); i++) {
        for (size_t j = 0; j < folds.size(); j++) {
            if (i == j) continue;
            const bool contains = folds[j].start_row <= folds[i].start_row && folds[j].end_row >= folds[i].end_row &&
                                  (folds[j].start_row < folds[i].start_row || folds[j].end_row > folds[i].end_row);
            if (!contains) continue;
            if (parent[i] == folds.size() || folds[j].start_row > folds[parent[i]].start_row ||
                folds[j].end_row < folds[parent[i]].end_row) {
                parent[i] = j;
            }
        }
    }
    for (size_t idx : order) {
        if (idx >= anchors.size()) continue;
        int lo = 0, hi = -1;  // the whole file, unless a parent narrows it
        const size_t p = parent[idx];
        if (p < folds.size() && out[p].found) {
            lo = out[p].start_row;
            hi = out[p].end_row;
        }
        int start = 0, end = 0;
        if (RefindFoldByAnchor(anchors[idx], folds[idx].start_row, folds[idx].end_row - folds[idx].start_row, lines,
                               &start, &end, lo, hi)) {
            out[idx] = {true, start, end};
        }
    }
    return out;
}

/**
 * @brief Takes an anchor for every fold in a list, from the text the folds' rows are true of.
 * @param folds The folds to anchor.
 * @param lines The buffer's lines *before* whatever is about to change them.
 * @return One anchor per fold, in the same order.
 *
 * Four strings per fold and nothing else: this is what lets a caller
 * record where its folds are without copying the whole line vector, which
 * for a ten-thousand-line buffer is the difference between a free call
 * and one worth avoiding.
 */
inline std::vector<FoldAnchor> FoldAnchorsFor(const std::vector<Fold> &folds,
                                              const std::vector<std::string> &lines) {
    std::vector<FoldAnchor> out;
    out.reserve(folds.size());
    for (const Fold &f : folds) out.push_back(FoldAnchorFor(lines, f.start_row, f.end_row));
    return out;
}

/**
 * @brief Moves folds across a replaced run of lines, preferring each fold's own text to arithmetic.
 * @param folds The list to move, in place.
 * @param anchors One anchor per fold, taken before the edit (FoldAnchorsFor).
 * @param at_row The first row of the replaced run.
 * @param removed How many lines the run had.
 * @param inserted How many lines replaced them.
 * @param lines The buffer's lines *after* the replacement.
 *
 * The arithmetic -- the delete, then the insert, the same two steps
 * ShiftFoldList does -- is right whenever the replacement leaves a fold's
 * text where the row maths says it is, and it is all there is to go on
 * when a fold's own lines were part of what got rewritten. What it cannot
 * do is survive a replacement that *covers* a fold: `:Format` rewrites
 * the whole file as one replace, so every fold in it was deleted and
 * nothing the arithmetic produced came back -- which is the other half of
 * "I have to delete all my folds and make them again".
 *
 * So each fold is first looked for by its own text, exactly as a session
 * restore looks for it, and only falls back to the arithmetic when that
 * text is gone.
 */
inline void ShiftFoldListForReplace(std::vector<Fold> &folds, const std::vector<FoldAnchor> &anchors, int at_row,
                                    int removed, int inserted, const std::vector<std::string> &lines) {
    if (folds.empty()) return;
    const int line_count = static_cast<int>(lines.size());
    // As a set, for the nesting reason RefindFoldsByAnchors explains.
    const std::vector<RefoundFold> refound = RefindFoldsByAnchors(folds, anchors, lines);
    std::vector<Fold> out;
    out.reserve(folds.size());
    for (size_t i = 0; i < folds.size(); i++) {
        Fold moved = folds[i];
        if (refound[i].found) {
            moved.start_row = refound[i].start_row;
            moved.end_row = refound[i].end_row;
        } else {
            // The delete and then the insert, in that order and against
            // the post-edit line count -- the same sequence (and the same
            // `inclusive_end` rule for the fold's own last row) the two
            // ShiftFoldList calls this replaces used to perform.
            moved.start_row = ShiftRowForLineEdit(moved.start_row, at_row, -removed, line_count);
            moved.end_row = ShiftRowForLineEdit(moved.end_row, at_row, -removed, line_count, /*inclusive_end=*/true);
            moved.start_row = ShiftRowForLineEdit(moved.start_row, at_row, inserted, line_count);
            moved.end_row = ShiftRowForLineEdit(moved.end_row, at_row, inserted, line_count, /*inclusive_end=*/true);
        }
        // Nothing left of it (see ShiftFoldList's own note: dropped here
        // rather than left for NormalizeFoldList to straighten out into a
        // plausible-looking fold over text it never covered).
        if (moved.start_row < moved.end_row) out.push_back(moved);
    }
    folds.swap(out);
    NormalizeFoldList(folds, line_count);
}

/**
 * @brief Shifts folds after a buffer's whole line vector was swapped for another.
 * @param folds The list to shift, in place.
 * @param before The line vector that was replaced.
 * @param after The line vector now in the buffer.
 *
 * Undo and redo don't present as an (at_row, count) edit -- they replace
 * the text outright -- so each fold's own first and last line are looked
 * for in the new text (RefindFoldByAnchor) and the fold moves to where
 * they are now. That handles the scattered, many-places-at-once rewrite a
 * `:e!` of a reformatted file is, which no single (at_row, count) can
 * describe at all.
 *
 * A fold whose text can't be found that way -- a blank start line, or an
 * undo that rewrote the very line the fold starts on -- falls back to the
 * older rule: everything above the first row the two versions disagree on
 * is untouched, so treat the line-count difference as an insert or delete
 * there. For the contiguous insert or delete an undo almost always is,
 * that is exactly the edit that was undone, run backwards.
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
    // Resolved as a set rather than one at a time, so a fold that was
    // nested inside another is looked for inside its parent's new range
    // (RefindFoldsByAnchors) instead of escaping it.
    const std::vector<RefoundFold> refound = RefindFoldsByAnchors(folds, FoldAnchorsFor(folds, before), after);
    std::vector<Fold> anchored;
    std::vector<Fold> unanchored;
    for (size_t i = 0; i < folds.size(); i++) {
        if (refound[i].found) {
            Fold moved = folds[i];
            moved.start_row = refound[i].start_row;
            moved.end_row = refound[i].end_row;
            anchored.push_back(moved);
            continue;
        }
        unanchored.push_back(folds[i]);
    }
    // The arithmetic fallback, applied only to the folds that needed it --
    // it is the thing that drops a fold whose text the swap deleted
    // outright (the shift leaves it inverted, which ShiftFoldList erases).
    if (!unanchored.empty()) {
        ShiftFoldList(unanchored, FirstDifferingRow(before, after), delta, static_cast<int>(after.size()));
        anchored.insert(anchored.end(), unanchored.begin(), unanchored.end());
    }
    folds.swap(anchored);
    NormalizeFoldList(folds, static_cast<int>(after.size()));
}

#endif  // MEP_FOLDS_H
