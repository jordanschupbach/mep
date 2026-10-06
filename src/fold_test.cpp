// mep-fold-test: windowless unit tests for folds.h -- NormalizeFoldList's
// three invariants (what stands between a fold set and the two symptoms
// this file exists to keep fixed: a fold that refuses to open, and a fold
// whose collapse swallows the whole rest of the buffer), the shift that
// keeps a fold on its own text across an edit, and line_edit.h's two
// shared helpers underneath both -- shared, because Editor's mark
// shifters are built on the same two and drifted in exactly the same
// ways before they were.
//
// Links nothing but folds.h (header-only, std-only), so it runs anywhere:
// no raylib, no display, no editor. CHECK(), never assert(): the Release
// build strips assert() (see agent_rpc_test.cpp's own comment on exactly
// this bug).

#include "folds.h"
#include "line_edit.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
/**
 * @brief Prints a CHECK-failure message (with file/line) to stderr and aborts the process.
 */
void CheckFailed(const char *expression, const char *file, int line) {
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, file, line);
    std::abort();
}

/**
 * @brief Builds a fold for the tests below.
 */
Fold F(int start, int end, bool closed = true, const char *provider = "manual") {
    Fold f;
    f.start_row = start;
    f.end_row = end;
    f.closed = closed;
    f.provider = provider;
    return f;
}

/**
 * @brief True when some fold in `folds` covers exactly this range.
 */
bool Has(const std::vector<Fold> &folds, int start, int end) {
    for (const Fold &f : folds) {
        if (f.start_row == start && f.end_row == end) return true;
    }
    return false;
}

/**
 * @brief True when no row is hidden by a fold that DrawPane would never reach the start of.
 *
 * DrawPane collapses a fold only when the row loop arrives at its start
 * row, and then jumps straight to its end row. So a fold whose start row
 * is itself hidden inside *another* closed fold that ends before it does
 * -- a crossing pair -- has no summary line anywhere on screen, and the
 * rows it hides can neither be reached nor revealed. This is that
 * condition, stated directly.
 */
bool EveryClosedFoldIsReachable(const std::vector<Fold> &folds) {
    for (const Fold &inner : folds) {
        if (!inner.closed) continue;
        for (const Fold &outer : folds) {
            if (&outer == &inner || !outer.closed) continue;
            const bool start_hidden = inner.start_row > outer.start_row && inner.start_row <= outer.end_row;
            if (start_hidden && inner.end_row > outer.end_row) return false;
        }
    }
    return true;
}
}  // namespace

#define CHECK(cond) ((cond) ? (void)0 : CheckFailed(#cond, __FILE__, __LINE__))

int main() {
    // --- Invariant 1: ranges stay inside the file ---------------------------
    // The undo/:e!/buf_set_lines case: the line vector was replaced under
    // the folds. An end_row past EOF is the one that makes DrawPane skip
    // every row after the fold's start, so the rest of the file vanishes.
    {
        std::vector<Fold> folds = {F(5, 200)};
        NormalizeFoldList(folds, 50);
        CHECK(folds.size() == 1);
        CHECK(folds[0].start_row == 5);
        CHECK(folds[0].end_row == 49);
    }
    // Shrunk past the fold entirely: nothing left to hide, so it goes.
    {
        std::vector<Fold> folds = {F(80, 200)};
        NormalizeFoldList(folds, 50);
        CHECK(folds.empty());
    }
    // A range that survived an edit as a single line isn't a fold anymore
    // -- the same rule Editor::CreateFold applies when one is first made.
    // A *reversed* one is, though: that is just the range written
    // backwards, which CreateFold also accepts by swapping the ends.
    {
        std::vector<Fold> folds = {F(10, 10), F(7, 6), F(3, 9)};
        NormalizeFoldList(folds, 100);
        CHECK(folds.size() == 2);
        CHECK(Has(folds, 3, 9) && Has(folds, 6, 7));
        CHECK(!Has(folds, 10, 10));
    }
    // Negative rows (a deletion that ran off the top) are clamped, not kept.
    {
        std::vector<Fold> folds = {F(-4, 8)};
        NormalizeFoldList(folds, 100);
        CHECK(folds.size() == 1);
        CHECK(folds[0].start_row == 0 && folds[0].end_row == 8);
    }
    // A one-line file can hold no fold at all.
    {
        std::vector<Fold> folds = {F(0, 5)};
        NormalizeFoldList(folds, 1);
        CHECK(folds.empty());
    }

    // --- Invariant 2: one fold per range ------------------------------------
    // zf over the same lines twice. Two identical folds meant za/zo, which
    // open exactly one level per press, left the copy underneath closed --
    // the fold read as refusing to open.
    {
        std::vector<Fold> folds = {F(10, 20), F(10, 20)};
        NormalizeFoldList(folds, 100);
        CHECK(folds.size() == 1);
        CHECK(folds[0].closed);
    }
    // The survivor keeps the collapse if either copy had it...
    {
        std::vector<Fold> folds = {F(10, 20, /*closed=*/false), F(10, 20, /*closed=*/true)};
        NormalizeFoldList(folds, 100);
        CHECK(folds.size() == 1);
        CHECK(folds[0].closed);
    }
    // ...and the hand-made tag if either was hand-made, so the session
    // file still saves it whole rather than as a rebuildable provider fold.
    {
        std::vector<Fold> folds = {F(10, 20, true, "treesitter"), F(10, 20, true, "manual")};
        NormalizeFoldList(folds, 100);
        CHECK(folds.size() == 1);
        CHECK(folds[0].provider == "manual");
    }
    // Nesting is not duplication: distinct ranges all survive.
    {
        std::vector<Fold> folds = {F(0, 99), F(10, 20), F(12, 15)};
        NormalizeFoldList(folds, 100);
        CHECK(folds.size() == 3);
        CHECK(Has(folds, 0, 99) && Has(folds, 10, 20) && Has(folds, 12, 15));
    }

    // --- Invariant 3: folds nest, they never cross --------------------------
    // The headline case: fold 10-50, then fold 20-60. DrawPane draws the
    // first, jumps to 51, and never reaches row 20 -- so rows 51-60 render
    // as ordinary text that the cursor refuses to land on, with no summary
    // line anywhere to open the second fold from.
    {
        std::vector<Fold> folds = {F(10, 50), F(20, 60)};
        CHECK(!EveryClosedFoldIsReachable(folds));  // the bug, before the fix
        NormalizeFoldList(folds, 100);
        CHECK(EveryClosedFoldIsReachable(folds));
        // Widened, not clipped: neither range's content is lost.
        CHECK(Has(folds, 10, 60));
        CHECK(Has(folds, 20, 60));
    }
    // A chain of crossings settles in one call, not one call per link.
    {
        std::vector<Fold> folds = {F(0, 10), F(5, 20), F(15, 30), F(25, 40)};
        NormalizeFoldList(folds, 100);
        CHECK(EveryClosedFoldIsReachable(folds));
        CHECK(folds.size() == 4);
        CHECK(folds[0].start_row == 0 && folds[0].end_row == 40);
    }
    // Already-nested input is left exactly as it was.
    {
        std::vector<Fold> folds = {F(0, 40), F(5, 20), F(8, 12)};
        NormalizeFoldList(folds, 100);
        CHECK(folds.size() == 3);
        CHECK(Has(folds, 0, 40) && Has(folds, 5, 20) && Has(folds, 8, 12));
    }
    // So is a disjoint set -- adjacent folds are not crossing folds.
    {
        std::vector<Fold> folds = {F(0, 10), F(11, 20), F(21, 30)};
        NormalizeFoldList(folds, 100);
        CHECK(folds.size() == 3);
        CHECK(Has(folds, 0, 10) && Has(folds, 11, 20) && Has(folds, 21, 30));
    }
    // Crossing *and* running off the end: clamping happens first, so the
    // merge works against ranges that actually exist.
    {
        std::vector<Fold> folds = {F(10, 50), F(20, 500)};
        NormalizeFoldList(folds, 60);
        CHECK(EveryClosedFoldIsReachable(folds));
        for (const Fold &f : folds) CHECK(f.end_row < 60);
    }
    // An *open* fold crossing a closed one still gets merged: it can be
    // closed later (za on its start row), and the invariant has to hold
    // then too, not just at the moment it was built.
    {
        std::vector<Fold> folds = {F(10, 50, /*closed=*/true), F(20, 60, /*closed=*/false)};
        NormalizeFoldList(folds, 100);
        for (Fold &f : folds) f.closed = true;
        CHECK(EveryClosedFoldIsReachable(folds));
    }

    // --- Canonical ordering -------------------------------------------------
    // Editor::LayoutFingerprint hashes the list in order, so two providers
    // that built the same ranges in different orders must normalize to the
    // same sequence -- otherwise every rebuild reads as a change worth
    // writing a session file for.
    {
        std::vector<Fold> a = {F(12, 15), F(0, 99), F(10, 20)};
        std::vector<Fold> b = {F(10, 20), F(12, 15), F(0, 99)};
        NormalizeFoldList(a, 100);
        NormalizeFoldList(b, 100);
        CHECK(a.size() == b.size());
        for (size_t i = 0; i < a.size(); i++) {
            CHECK(a[i].start_row == b[i].start_row);
            CHECK(a[i].end_row == b[i].end_row);
        }
        // Outer before inner, so a walk can rely on seeing a fold before
        // anything nested in it.
        CHECK(a[0].start_row == 0 && a[0].end_row == 99);
    }
    // Two folds sharing a start row sort widest first -- the one DrawPane
    // picks to draw.
    {
        std::vector<Fold> folds = {F(5, 9), F(5, 30)};
        NormalizeFoldList(folds, 100);
        CHECK(folds.size() == 2);
        CHECK(folds[0].end_row == 30);
        CHECK(folds[1].end_row == 9);
    }

    // --- Idempotence --------------------------------------------------------
    // It runs once per frame on any buffer whose line count moved, so a
    // second pass over its own output must change nothing.
    {
        std::vector<Fold> folds = {F(10, 50), F(20, 60), F(20, 60), F(5, 500), F(3, 3)};
        NormalizeFoldList(folds, 80);
        std::vector<Fold> once = folds;
        NormalizeFoldList(folds, 80);
        CHECK(folds.size() == once.size());
        for (size_t i = 0; i < folds.size(); i++) {
            CHECK(folds[i].start_row == once[i].start_row);
            CHECK(folds[i].end_row == once[i].end_row);
            CHECK(folds[i].closed == once[i].closed);
        }
        CHECK(EveryClosedFoldIsReachable(folds));
    }
    // The empty list is the common case (most buffers have no folds).
    {
        std::vector<Fold> folds;
        NormalizeFoldList(folds, 100);
        CHECK(folds.empty());
    }

    // --- ShiftFoldList: a fold rides the text it was made over -------------
    // Lines inserted above a fold push the whole range down; lines
    // inserted inside it extend end_row and leave start_row put.
    {
        std::vector<Fold> folds = {F(20, 30)};
        ShiftFoldList(folds, 9, 4, 104);  // 4 lines inserted at row 9
        CHECK(Has(folds, 24, 34));
        folds = {F(20, 30)};
        ShiftFoldList(folds, 25, 4, 104);  // 4 lines inserted inside it
        CHECK(Has(folds, 20, 34));
        folds = {F(20, 30)};
        ShiftFoldList(folds, 40, 4, 104);  // below it: nothing moves
        CHECK(Has(folds, 20, 30));
    }
    // Deletions, including one that eats a boundary: a row inside the
    // deleted run collapses to the row the deletion started at.
    {
        std::vector<Fold> folds = {F(20, 30)};
        ShiftFoldList(folds, 9, -4, 96);
        CHECK(Has(folds, 16, 26));
        folds = {F(20, 30)};
        ShiftFoldList(folds, 25, -20, 80);  // swallows end_row and then some
        CHECK(Has(folds, 20, 25));
        folds = {F(20, 30)};
        ShiftFoldList(folds, 18, -20, 80);  // swallows the whole fold
        CHECK(folds.empty());
    }

    // --- ShiftFoldListForTextSwap: undo and redo ---------------------------
    // The reported bug, in miniature. Deleting four lines shifts the folds
    // up (above); undoing has to put them back, and nothing but the two
    // line vectors is available to work that out from.
    {
        std::vector<std::string> full;
        for (int i = 1; i <= 60; i++) full.push_back("line " + std::to_string(i));
        std::vector<std::string> cut = full;
        cut.erase(cut.begin() + 9, cut.begin() + 13);  // :10,13d

        std::vector<Fold> folds = {F(20, 30)};
        ShiftFoldList(folds, 9, -4, static_cast<int>(cut.size()));
        CHECK(Has(folds, 16, 26));  // the delete, as the editor already handled it
        ShiftFoldListForTextSwap(folds, cut, full);
        CHECK(Has(folds, 20, 30));  // the undo puts it back exactly
        ShiftFoldListForTextSwap(folds, full, cut);
        CHECK(Has(folds, 16, 26));  // and the redo takes it away again
    }
    // A swap that changes no line count moves nothing, however different
    // the text is -- an in-place edit never moved a row.
    {
        std::vector<std::string> a = {"one", "two", "three", "four"};
        std::vector<std::string> b = {"ONE", "TWO", "THREE", "FOUR"};
        std::vector<Fold> folds = {F(1, 3)};
        ShiftFoldListForTextSwap(folds, a, b);
        CHECK(Has(folds, 1, 3));
    }
    // A change entirely *below* a fold leaves it alone: the first
    // disagreement is past the fold, so the shift starts past it too.
    {
        std::vector<std::string> a, b;
        for (int i = 0; i < 40; i++) { a.push_back("l" + std::to_string(i)); b.push_back("l" + std::to_string(i)); }
        b.insert(b.begin() + 35, 3, "new");
        std::vector<Fold> folds = {F(5, 15)};
        ShiftFoldListForTextSwap(folds, a, b);
        CHECK(Has(folds, 5, 15));
    }

    // --- line_edit.h: the rule marks and folds share --------------------
    // One row, one edit. A row at or below an insertion point moves down
    // by it; one above does not move at all.
    {
        CHECK(ShiftRowForLineEdit(20, 9, 4, 104) == 24);
        CHECK(ShiftRowForLineEdit(9, 9, 4, 104) == 13);
        CHECK(ShiftRowForLineEdit(8, 9, 4, 104) == 8);
    }
    // A deletion: below the run, move up by it; strictly inside it,
    // collapse to the deletion point -- the text that row named is gone,
    // and where it was is the nearest honest answer. Above it, stay.
    {
        CHECK(ShiftRowForLineEdit(20, 9, -4, 96) == 16);
        CHECK(ShiftRowForLineEdit(11, 9, -4, 96) == 9);   // inside the deleted run
        CHECK(ShiftRowForLineEdit(13, 9, -4, 96) == 9);   // first surviving row
        CHECK(ShiftRowForLineEdit(8, 9, -4, 96) == 8);
    }
    // Never out of the buffer, whichever way the edit went.
    {
        CHECK(ShiftRowForLineEdit(95, 0, 4, 10) == 9);
        CHECK(ShiftRowForLineEdit(5, 0, -90, 1) == 0);
    }
    // FirstDifferingRow: where an undo's edit actually starts.
    {
        std::vector<std::string> a = {"x", "y", "z"};
        CHECK(FirstDifferingRow(a, a) == 3);                                   // identical
        CHECK(FirstDifferingRow(a, {"x", "y"}) == 2);                          // one is a prefix
        CHECK(FirstDifferingRow(a, {"x", "Q", "z"}) == 1);                     // differs in the middle
        CHECK(FirstDifferingRow(a, {"Q", "y", "z"}) == 0);                     // differs at the top
        CHECK(FirstDifferingRow(a, {}) == 0);                                  // emptied
        CHECK(FirstDifferingRow({}, a) == 0);
    }

    std::printf("mep-fold-test: all checks passed\n");
    return 0;
}
