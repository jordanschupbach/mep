// Windowless test for sheet_view.cpp: the display-order model (filter +
// sort + frozen header rows), the geometry both Editor::ResizeSheetViewport
// and main.cpp's grid renderer read, the column stats, find-in-grid, and
// the apply-order-to-data rewrite. CHECK(), never assert(): the Release
// build strips assert() entirely.
//
// Also the first test of any kind for the spreadsheet model -- sheet_doc.cpp
// and formula.cpp had none -- so a few cases here pin behaviour this file
// depends on rather than behaviour it owns (CSV load, evaluated-value
// formatting, CSV save round trips).
#include "sheet_view.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

/**
 * @brief Loads CSV text into a single-sheet Workbook the way opening a .csv does.
 * @param csv The file contents.
 * @return The parsed workbook.
 */
Workbook LoadCsv(const std::string &csv) {
    Workbook wb;
    std::string error;
    const bool ok = LoadCsvFromMemory(reinterpret_cast<const unsigned char *>(csv.data()), csv.size(), wb, error);
    CHECK(ok);
    CHECK(wb.sheets.size() == 1);
    return wb;
}

/**
 * @brief Returns a cell's displayed text, as the grid and the sort/filter code see it.
 * @param wb Workbook to read.
 * @param row Zero-based row.
 * @param col Zero-based column.
 * @return The formatted value.
 */
std::string Text(Workbook &wb, int row, int col) { return FormatCellValue(EvaluateCell(wb, 0, row, col)); }

/**
 * @brief Collects a column's displayed values in display order.
 * @param wb Workbook to read.
 * @param view View supplying the order.
 * @param col Column to read.
 * @return One string per display row.
 */
std::vector<std::string> ColumnInDisplayOrder(Workbook &wb, const SheetView &view, int col) {
    std::vector<std::string> out;
    for (int d = 0; d < SheetDisplayCount(view); d++) out.push_back(Text(wb, SheetRowAtDisplay(view, d), col));
    return out;
}

// name,qty,note -- deliberately mixed: repeated sort keys (for stability),
// a blank, text in a numeric column, and values to filter on.
const char *kFruit =
    "name,qty,note\n"
    "cherry,10,red\n"
    "apple,2,green\n"
    "Banana,2,yellow\n"
    "date,,brown\n"
    "elderberry,n/a,purple\n"
    "fig,30,green\n";

}  // namespace

// ============================================================================
// Fixture sanity -- what the rest of the file assumes about CSV loading
// ============================================================================

void TestFixtureShape() {
    Workbook wb = LoadCsv(kFruit);
    const Sheet &sh = wb.sheets[0];
    CHECK(sh.max_row == 6);  // header + 6 data rows, zero-based high-water mark
    CHECK(sh.max_col == 2);
    CHECK(Text(wb, 0, 0) == "name");
    CHECK(Text(wb, 1, 1) == "10");
    // LoadCsvFromMemory never stores an empty field, so a blank cell is a
    // nullptr rather than an empty Cell. Everything below has to treat
    // that as Empty, not as "skip this row".
    CHECK(sh.FindCell(4, 1) == nullptr);
    CHECK(Text(wb, 4, 1).empty());
}

// ============================================================================
// Sorting
// ============================================================================

void TestSortTextAscDesc() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;  // the header row is not data
    v.sort = SheetSort{0, SheetSortKind::Text, false};
    RebuildSheetOrder(v, wb, 0);
    std::vector<std::string> names = ColumnInDisplayOrder(wb, v, 0);
    // Case-insensitive: "Banana" sorts between "apple" and "cherry", not
    // ahead of both the way a byte comparison would put it.
    CHECK(names.size() == 7);
    CHECK(names[0] == "name");  // frozen, still first
    CHECK(names[1] == "apple");
    CHECK(names[2] == "Banana");
    CHECK(names[3] == "cherry");
    CHECK(names[4] == "date");
    CHECK(names[5] == "elderberry");
    CHECK(names[6] == "fig");

    v.sort.descending = true;
    RebuildSheetOrder(v, wb, 0);
    names = ColumnInDisplayOrder(wb, v, 0);
    CHECK(names[0] == "name");
    CHECK(names[1] == "fig");
    CHECK(names[6] == "apple");
}

void TestSortNumericAndBlanksLast() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;
    v.sort = SheetSort{1, SheetSortKind::Number, false};
    RebuildSheetOrder(v, wb, 0);
    std::vector<std::string> qty = ColumnInDisplayOrder(wb, v, 1);
    CHECK(qty[0] == "qty");
    CHECK(qty[1] == "2");
    CHECK(qty[2] == "2");
    CHECK(qty[3] == "10");  // numeric, not the "10" < "2" a text sort gives
    CHECK(qty[4] == "30");
    CHECK(qty[5] == "n/a");  // non-numeric text sorts after every number
    CHECK(qty[6].empty());   // blanks last

    v.sort.descending = true;
    RebuildSheetOrder(v, wb, 0);
    qty = ColumnInDisplayOrder(wb, v, 1);
    CHECK(qty[1] == "n/a");
    CHECK(qty[2] == "30");
    CHECK(qty[3] == "10");
    // Blanks stay at the BOTTOM when the sort reverses. Reversing them
    // with everything else would fill the first screen of a descending
    // sort with empty rows, which is never what was being looked for.
    CHECK(qty[6].empty());
}

void TestSortTextVsNumericDiffer() {
    Workbook wb = LoadCsv("a\n2\n10\n1\n");
    SheetView v;
    v.sort = SheetSort{0, SheetSortKind::Number, false};
    RebuildSheetOrder(v, wb, 0);
    std::vector<std::string> got = ColumnInDisplayOrder(wb, v, 0);
    CHECK(got[0] == "1");
    CHECK(got[1] == "2");
    CHECK(got[2] == "10");
    CHECK(got[3] == "a");

    v.sort.kind = SheetSortKind::Text;
    RebuildSheetOrder(v, wb, 0);
    got = ColumnInDisplayOrder(wb, v, 0);
    CHECK(got[0] == "1");
    CHECK(got[1] == "10");  // lexical: "10" before "2"
    CHECK(got[2] == "2");
    CHECK(got[3] == "a");
}

void TestSortAutoDetect() {
    // A majority-numeric column sorts numerically even with a stray
    // non-number in it -- a real price column has an "n/a" or two, and
    // falling back to string order for the whole column because of one
    // of them is worse than sorting it after the numbers.
    Workbook wb = LoadCsv("v\n10\n9\nn/a\n2\n");
    SheetView v;
    v.freeze_rows = 1;
    v.sort = SheetSort{0, SheetSortKind::Auto, false};
    RebuildSheetOrder(v, wb, 0);
    std::vector<std::string> got = ColumnInDisplayOrder(wb, v, 0);
    CHECK(got[1] == "2");
    CHECK(got[2] == "9");
    CHECK(got[3] == "10");
    CHECK(got[4] == "n/a");

    // A majority-text column sorts as text.
    Workbook wb2 = LoadCsv("v\npear\nplum\n7\nfig\n");
    SheetView v2;
    v2.freeze_rows = 1;
    v2.sort = SheetSort{0, SheetSortKind::Auto, false};
    RebuildSheetOrder(v2, wb2, 0);
    std::vector<std::string> got2 = ColumnInDisplayOrder(wb2, v2, 0);
    CHECK(got2[1] == "7");  // "7" < "fig" lexically
    CHECK(got2[2] == "fig");
    CHECK(got2[3] == "pear");
    CHECK(got2[4] == "plum");
}

void TestSortIsStable() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;
    v.sort = SheetSort{1, SheetSortKind::Number, false};
    RebuildSheetOrder(v, wb, 0);
    // Two rows tie at qty 2: apple (sheet row 2) and Banana (row 3).
    // stable_sort must keep them in that order, so sorting is repeatable
    // and sorting by one column then another gives a predictable result.
    CHECK(SheetRowAtDisplay(v, 1) == 2);
    CHECK(SheetRowAtDisplay(v, 2) == 3);

    // Descending must not reverse ties either -- only the key order.
    v.sort.descending = true;
    RebuildSheetOrder(v, wb, 0);
    int d_apple = SheetDisplayOfRow(v, 2);
    int d_banana = SheetDisplayOfRow(v, 3);
    CHECK(d_apple + 1 == d_banana);
}

void TestSortByEvaluatedValue() {
    // A formula sorts by its result, which is what the grid shows.
    Workbook wb = LoadCsv("v\n5\n1\n");
    SetCellRaw(wb, 0, 3, 0, "=1+1");
    SheetView v;
    v.freeze_rows = 1;
    v.sort = SheetSort{0, SheetSortKind::Number, false};
    RebuildSheetOrder(v, wb, 0);
    std::vector<std::string> got = ColumnInDisplayOrder(wb, v, 0);
    CHECK(got[1] == "1");
    CHECK(got[2] == "2");  // the formula, as its value -- not as "=1+1"
    CHECK(got[3] == "5");
}

// ============================================================================
// Filtering
// ============================================================================

void TestFilterOps() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;

    auto only = [&](SheetFilterOp op, int col, const std::string &arg) {
        v.filters.clear();
        SheetFilter f;
        f.col = col;
        f.op = op;
        f.value = arg;
        v.filters.push_back(f);
        RebuildSheetOrder(v, wb, 0);
        return ColumnInDisplayOrder(wb, v, 0);
    };

    // Case-insensitive throughout: a filter the user typed by hand should
    // not miss "Banana" because they typed "banana".
    std::vector<std::string> got = only(SheetFilterOp::Equals, 0, "banana");
    CHECK(got.size() == 2);  // the frozen header plus the match
    CHECK(got[1] == "Banana");

    got = only(SheetFilterOp::Contains, 2, "green");
    CHECK(got.size() == 3);
    CHECK(got[1] == "apple");
    CHECK(got[2] == "fig");

    got = only(SheetFilterOp::NotContains, 2, "green");
    CHECK(got.size() == 5);

    got = only(SheetFilterOp::NotEquals, 0, "fig");
    CHECK(got.size() == 6);

    // Empty/NonEmpty against a cell LoadCsvFromMemory never stored.
    got = only(SheetFilterOp::Empty, 1, "");
    CHECK(got.size() == 2);
    CHECK(got[1] == "date");

    got = only(SheetFilterOp::NonEmpty, 1, "");
    CHECK(got.size() == 6);

    // A numeric threshold is a numeric question: "n/a" and the blank do
    // NOT come back, even though "n/a" > "5" as text.
    got = only(SheetFilterOp::Greater, 1, "5");
    CHECK(got.size() == 3);
    CHECK(got[1] == "cherry");  // 10
    CHECK(got[2] == "fig");     // 30

    got = only(SheetFilterOp::Less, 1, "5");
    CHECK(got.size() == 3);  // the two 2s, and neither "n/a" nor the blank

    // A non-numeric threshold is a lexical question, so a text column
    // still filters instead of silently matching nothing.
    got = only(SheetFilterOp::Greater, 0, "date");
    CHECK(got.size() == 3);
    CHECK(got[1] == "elderberry");
    CHECK(got[2] == "fig");

    got = only(SheetFilterOp::Regex, 0, "^[ab]");
    CHECK(got.size() == 3);
    CHECK(got[1] == "apple");
    CHECK(got[2] == "Banana");

    // A pattern that cannot compile must reject everything rather than
    // crash or silently match everything.
    got = only(SheetFilterOp::Regex, 0, "([unclosed");
    CHECK(got.size() == 1);  // header only

    v.filters.clear();
    SheetFilter one_of;
    one_of.col = 0;
    one_of.op = SheetFilterOp::OneOf;
    one_of.values = {"fig", "DATE"};
    v.filters.push_back(one_of);
    RebuildSheetOrder(v, wb, 0);
    got = ColumnInDisplayOrder(wb, v, 0);
    CHECK(got.size() == 3);
    CHECK(got[1] == "date");
    CHECK(got[2] == "fig");
}

void TestFiltersAndTogether() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;
    SheetFilter green;
    green.col = 2;
    green.op = SheetFilterOp::Contains;
    green.value = "green";
    SheetFilter big;
    big.col = 1;
    big.op = SheetFilterOp::Greater;
    big.value = "5";
    v.filters.push_back(green);
    v.filters.push_back(big);
    RebuildSheetOrder(v, wb, 0);
    std::vector<std::string> got = ColumnInDisplayOrder(wb, v, 0);
    CHECK(got.size() == 2);
    CHECK(got[1] == "fig");  // green AND qty > 5
    CHECK(v.visible_rows == 2);
    CHECK(v.total_rows == 7);
}

void TestFilterComposedWithSort() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;
    SheetFilter nonblank;
    nonblank.col = 1;
    nonblank.op = SheetFilterOp::NonEmpty;
    v.filters.push_back(nonblank);
    v.sort = SheetSort{1, SheetSortKind::Number, true};
    RebuildSheetOrder(v, wb, 0);
    std::vector<std::string> qty = ColumnInDisplayOrder(wb, v, 1);
    CHECK(qty.size() == 6);
    CHECK(qty[0] == "qty");
    CHECK(qty[1] == "n/a");
    CHECK(qty[2] == "30");
    CHECK(qty[5] == "2");
}

void TestFrozenRowsNeverFiltered() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;
    SheetFilter impossible;
    impossible.col = 0;
    impossible.op = SheetFilterOp::Equals;
    impossible.value = "nothing matches this";
    v.filters.push_back(impossible);
    RebuildSheetOrder(v, wb, 0);
    // The header row survives a filter that matches nothing: it is not
    // data, so it is not subject to the predicate.
    CHECK(SheetDisplayCount(v) == 1);
    CHECK(SheetRowAtDisplay(v, 0) == 0);
    CHECK(v.visible_rows == 1);
}

// ============================================================================
// Display-order mapping
// ============================================================================

void TestOrderMappingIsBijective() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;
    SheetFilter f;
    f.col = 2;
    f.op = SheetFilterOp::Contains;
    f.value = "e";  // red, green, yellow, purple -- not brown
    v.filters.push_back(f);
    v.sort = SheetSort{0, SheetSortKind::Text, false};
    RebuildSheetOrder(v, wb, 0);

    for (int d = 0; d < SheetDisplayCount(v); d++) {
        const int row = SheetRowAtDisplay(v, d);
        CHECK(row >= 0);
        CHECK(SheetDisplayOfRow(v, row) == d);
    }
    // "date" (brown) is filtered out and must map to nothing.
    CHECK(SheetDisplayOfRow(v, 4) == -1);
    CHECK(SheetRowAtDisplay(v, SheetDisplayCount(v)) == -1);
    CHECK(SheetRowAtDisplay(v, -1) == -1);
    CHECK(SheetDisplayOfRow(v, 999) == -1);
}

void TestIdentityOrderWhenUnsortedUnfiltered() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    RebuildSheetOrder(v, wb, 0);
    CHECK(SheetDisplayCount(v) == 7);
    for (int d = 0; d < 7; d++) CHECK(SheetRowAtDisplay(v, d) == d);
    CHECK(v.visible_rows == v.total_rows);
}

void TestNearestVisibleDisplay() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;
    SheetFilter f;
    f.col = 2;
    f.op = SheetFilterOp::Contains;
    f.value = "green";  // keeps rows 2 (apple) and 6 (fig)
    v.filters.push_back(f);
    RebuildSheetOrder(v, wb, 0);
    CHECK(SheetDisplayCount(v) == 3);

    // A visible row maps to itself.
    CHECK(SheetNearestVisibleDisplay(v, 2) == SheetDisplayOfRow(v, 2));
    // A hidden row lands on the next visible row *after* it, so the
    // cursor keeps moving the way the user was already going.
    CHECK(SheetRowAtDisplay(v, SheetNearestVisibleDisplay(v, 3)) == 6);
    // Nothing after it -- fall back to the previous visible row.
    CHECK(SheetRowAtDisplay(v, SheetNearestVisibleDisplay(v, 5)) == 6);
}

void TestVisualRangeUsesDisplayOrder() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    SheetFilter f;
    f.col = 2;
    f.op = SheetFilterOp::Contains;
    f.value = "green";  // rows 2 and 6 only
    v.filters.push_back(f);
    RebuildSheetOrder(v, wb, 0);

    // A selection from apple (row 2) to fig (row 6) covers exactly the
    // two visible rows -- NOT rows 3, 4 and 5, which are hidden. Sheet-row
    // arithmetic would sweep those in, and `x` would clear data that was
    // never on screen.
    CHECK(SheetDisplayRangeContains(v, 2, 6, 2));
    CHECK(SheetDisplayRangeContains(v, 2, 6, 6));
    CHECK(!SheetDisplayRangeContains(v, 2, 6, 3));
    CHECK(!SheetDisplayRangeContains(v, 2, 6, 4));
    CHECK(!SheetDisplayRangeContains(v, 2, 6, 5));
    // Order of the endpoints must not matter.
    CHECK(SheetDisplayRangeContains(v, 6, 2, 2));
}

// ============================================================================
// Geometry
// ============================================================================

void TestWidthHeightClamps() {
    SheetView v;
    CHECK(SheetStoredColWidth(v, 0) == kSheetColWidth);
    SheetSetColWidth(v, 0, 5);
    CHECK(SheetStoredColWidth(v, 0) == kSheetMinColWidth);
    SheetSetColWidth(v, 0, 99999);
    CHECK(SheetStoredColWidth(v, 0) == kSheetMaxColWidth);
    SheetSetColWidth(v, 0, 140);
    CHECK(SheetStoredColWidth(v, 0) == 140);

    SheetSetRowHeight(v, 3, 1);
    CHECK(SheetStoredRowHeight(v, 3) == kSheetMinRowHeight);
    SheetSetRowHeight(v, 3, 60);
    CHECK(SheetStoredRowHeight(v, 3) == 60);
    // A negative index is ignored rather than stored and later read back
    // for a cell that cannot exist.
    SheetSetColWidth(v, -1, 100);
    CHECK(v.col_w.find(-1) == v.col_w.end());
}

void TestZoomScalesGeometry() {
    SheetView v;
    SheetGeom g1 = MakeSheetGeom(v, 1.0f);
    SheetGeom g2 = MakeSheetGeom(v, 2.0f);
    CHECK(SheetColWidth(g1, 0) == kSheetColWidth);
    CHECK(SheetColWidth(g2, 0) == kSheetColWidth * 2);
    CHECK(SheetRowHeight(g2, 0) == kSheetRowHeight * 2);
    // Overrides scale too, so zooming a resized column keeps its
    // proportion instead of snapping back to the default.
    SheetSetColWidth(v, 0, 140);
    CHECK(SheetColWidth(g2, 0) == 280);
    // Out-of-range zoom is clamped, never applied raw.
    CHECK(ClampSheetZoom(100.0f) == kSheetMaxZoom);
    CHECK(ClampSheetZoom(0.0f) == kSheetMinZoom);
    CHECK(MakeSheetGeom(v, 99.0f).zoom == kSheetMaxZoom);
}

void TestFontFloorRaisesRowHeight() {
    SheetView v;
    // A large editor font with no sheet zoom: the geometric 22px row
    // would clip the glyphs, so the measured line height floors it.
    v.font_line_height = 40.0f;
    SheetGeom g = MakeSheetGeom(v, 1.0f);
    CHECK(SheetRowHeight(g, 0) == 44);
    // The floor is in rendered pixels, so it is not scaled a second time
    // by the zoom -- but a zoom big enough to exceed it still wins.
    SheetGeom g3 = MakeSheetGeom(v, 3.0f);
    CHECK(SheetRowHeight(g3, 0) == kSheetRowHeight * 3);
    // The stored (unzoomed) value stays free of the floor, so a
    // round trip through the setter cannot creep upwards.
    CHECK(SheetStoredRowHeight(v, 0) == kSheetRowHeight);
}

void TestOffsetsAndHitTestRoundTrip() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    RebuildSheetOrder(v, wb, 0);
    SheetSetColWidth(v, 1, 200);
    SheetSetRowHeight(v, 2, 60);
    SheetGeom g = MakeSheetGeom(v, 1.0f);

    CHECK(SheetColOffsetX(g, 0, 0) == 0);
    CHECK(SheetColOffsetX(g, 0, 1) == kSheetColWidth);
    CHECK(SheetColOffsetX(g, 0, 2) == kSheetColWidth + 200);
    // Left of the scroll origin reads back negative, which is what lets
    // the renderer draw a partially-scrolled column without a special case.
    CHECK(SheetColOffsetX(g, 1, 0) == -kSheetColWidth);

    for (int c = 0; c < 4; c++) {
        CHECK(SheetColAtX(g, 0, SheetColOffsetX(g, 0, c)) == c);
        CHECK(SheetColAtX(g, 0, SheetColOffsetX(g, 0, c) + 1) == c);
    }
    for (int d = 0; d < SheetDisplayCount(v); d++) {
        CHECK(SheetDisplayAtY(g, 0, SheetRowOffsetY(g, 0, d)) == d);
    }
    // Row heights follow the SHEET row, so display index 2 is the row
    // that was made 60px tall.
    CHECK(SheetDisplayRowHeight(g, 2) == 60);
    CHECK(SheetRowOffsetY(g, 0, 3) == kSheetRowHeight * 2 + 60);
}

void TestVisibleCountsMatchOldArithmeticWhenUniform() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    RebuildSheetOrder(v, wb, 0);
    SheetGeom g = MakeSheetGeom(v, 1.0f);
    // With no overrides and zoom 1 the counts must agree with the
    // integer division the fixed-slot renderer used, so the refactor is
    // provably behaviour-preserving for an untouched sheet.
    for (int avail = 1; avail < 400; avail += 7) {
        const int rows = SheetVisibleRows(g, avail, 0);
        const int expect = std::max(1, (avail + kSheetRowHeight - 1) / kSheetRowHeight);
        CHECK(rows == expect);
        const int cols = SheetVisibleCols(g, avail, 0);
        const int expect_c = std::max(1, (avail + kSheetColWidth - 1) / kSheetColWidth);
        CHECK(cols == expect_c);
    }
}

void TestVisibleRowsWithTallRow() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    RebuildSheetOrder(v, wb, 0);
    SheetSetRowHeight(v, 1, 200);
    SheetGeom g = MakeSheetGeom(v, 1.0f);
    // 300px of viewport holds the 200px row plus five 22px rows, not the
    // 13 rows a count-based estimate would claim.
    CHECK(SheetVisibleRows(g, 300, 1) == 6);
}

void TestEdgeHitTest() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    RebuildSheetOrder(v, wb, 0);
    SheetGeom g = MakeSheetGeom(v, 1.0f);
    // The grab zone straddles the boundary, so the pointer finds the edge
    // from either side -- a zone only on the right would make a column's
    // left-hand neighbour unresizable at its own right edge.
    CHECK(SheetColEdgeAtX(g, 0, kSheetColWidth, 4, 500) == 0);
    CHECK(SheetColEdgeAtX(g, 0, kSheetColWidth - 3, 4, 500) == 0);
    CHECK(SheetColEdgeAtX(g, 0, kSheetColWidth + 3, 4, 500) == 0);
    CHECK(SheetColEdgeAtX(g, 0, kSheetColWidth / 2, 4, 500) == -1);
    CHECK(SheetColEdgeAtX(g, 0, kSheetColWidth * 2, 4, 500) == 1);
    // Beyond the visible area there is no edge to grab.
    CHECK(SheetColEdgeAtX(g, 0, 5000, 4, 500) == -1);
    CHECK(SheetDisplayEdgeAtY(g, 0, kSheetRowHeight, 4, 300) == 0);
    CHECK(SheetDisplayEdgeAtY(g, 0, kSheetRowHeight / 2, 4, 300) == -1);
}

void TestMeasurePane() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    RebuildSheetOrder(v, wb, 0);
    SheetGeom g = MakeSheetGeom(v, 1.0f);
    SheetPaneMetrics m = SheetMeasurePane(g, 800, 400);
    CHECK(m.header_w == kSheetRowHeaderW);
    CHECK(m.col_header_h == kSheetRowHeight);
    CHECK(m.frozen_w == 0);
    CHECK(m.frozen_h == 0);
    CHECK(m.avail_w == 800 - kSheetRowHeaderW);
    CHECK(m.avail_h == 400 - kSheetRowHeight);

    v.freeze_rows = 1;
    v.freeze_cols = 1;
    RebuildSheetOrder(v, wb, 0);
    m = SheetMeasurePane(g, 800, 400);
    CHECK(m.frozen_w == kSheetColWidth);
    CHECK(m.frozen_h == kSheetRowHeight);
    CHECK(m.avail_w == 800 - kSheetRowHeaderW - kSheetColWidth);
    CHECK(m.avail_h == 400 - kSheetRowHeight - kSheetRowHeight);

    // A freeze bigger than the pane must still leave something to scroll
    // in, or scroll-follow has nothing to work with and the rest of the
    // data becomes unreachable.
    v.freeze_rows = 50;
    v.freeze_cols = 50;
    RebuildSheetOrder(v, wb, 0);
    m = SheetMeasurePane(g, 300, 200);
    CHECK(m.avail_w > 0);
    CHECK(m.avail_h > 0);
}

void TestFollowCursor() {
    Workbook wb = LoadCsv("a,b,c,d,e,f,g,h,i,j,k,l\n1,2,3,4,5,6,7,8,9,10,11,12\n");
    for (int r = 2; r < 200; r++) SetCellRaw(wb, 0, r, 0, std::to_string(r));
    SheetView v;
    RebuildSheetOrder(v, wb, 0);
    SheetGeom g = MakeSheetGeom(v, 1.0f);
    const int avail_h = 10 * kSheetRowHeight;
    const int avail_w = 3 * kSheetColWidth;

    int sd = 0, sc = 0;
    // A cursor already on screen must not move the scroll at all.
    SheetFollowCursor(g, &sd, &sc, 3, 1, avail_w, avail_h);
    CHECK(sd == 0);
    CHECK(sc == 0);

    // Jumping to the bottom brings the cursor onto the last visible row,
    // scrolling the minimum needed.
    SheetFollowCursor(g, &sd, &sc, 50, 0, avail_w, avail_h);
    CHECK(sd == 41);
    const int settled = sd;
    // Idempotent: the renderer calls this every frame.
    SheetFollowCursor(g, &sd, &sc, 50, 0, avail_w, avail_h);
    CHECK(sd == settled);

    // Back up, and the window follows the other way.
    SheetFollowCursor(g, &sd, &sc, 5, 0, avail_w, avail_h);
    CHECK(sd == 5);

    // Columns behave the same way.
    SheetFollowCursor(g, &sd, &sc, 5, 11, avail_w, avail_h);
    CHECK(sc == 9);
    SheetFollowCursor(g, &sd, &sc, 5, 0, avail_w, avail_h);
    CHECK(sc == 0);

    // A row taller than the viewport still gets shown rather than
    // scrolling forever looking for a fit.
    SheetSetRowHeight(v, 60, 400);
    SheetFollowCursor(g, &sd, &sc, 60, 0, avail_w, avail_h);
    CHECK(sd == 60);
}

void TestFollowCursorRespectsFreeze() {
    Workbook wb = LoadCsv("h1,h2,h3\n");
    for (int r = 1; r < 100; r++) SetCellRaw(wb, 0, r, 0, std::to_string(r));
    SheetView v;
    v.freeze_rows = 1;
    v.freeze_cols = 1;
    RebuildSheetOrder(v, wb, 0);
    SheetGeom g = MakeSheetGeom(v, 1.0f);
    int sd = 0, sc = 0;
    SheetFollowCursor(g, &sd, &sc, 0, 0, 200, 200);
    // The scroll origin can never enter the frozen band: those rows and
    // columns are drawn pinned, so scrolling them would show them twice.
    CHECK(sd == 1);
    CHECK(sc == 1);
    // A cursor inside the frozen band is always on screen -- nothing to
    // follow, and in particular no scrolling back to the top.
    SheetFollowCursor(g, &sd, &sc, 40, 0, 200, 200);
    const int after = sd;
    SheetFollowCursor(g, &sd, &sc, 0, 0, 200, 200);
    CHECK(sd == after);
}

// ============================================================================
// Stats, distinct values, find
// ============================================================================

void TestStats() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;
    RebuildSheetOrder(v, wb, 0);
    SheetColumnStats st = SheetComputeStats(wb, 0, v, 1, 1, SheetDisplayCount(v) - 1);
    CHECK(st.count == 6);
    CHECK(st.nonempty == 5);       // one blank
    CHECK(st.numeric == 4);        // 10, 2, 2, 30 -- "n/a" is not
    CHECK(st.distinct == 4);       // 10, 2, n/a, 30
    CHECK(st.sum == 44.0);
    CHECK(st.mean == 11.0);        // mean of the NUMERIC cells only
    CHECK(st.min == 2.0);
    CHECK(st.max == 30.0);

    // A text column reports the counts and leaves the arithmetic alone
    // rather than inventing a number for it.
    SheetColumnStats text = SheetComputeStats(wb, 0, v, 0, 1, SheetDisplayCount(v) - 1);
    CHECK(text.nonempty == 6);
    CHECK(text.numeric == 0);
    CHECK(text.sum == 0.0);
    CHECK(text.mean == 0.0);  // no division by zero

    // Over a filtered view, the stats describe what is on screen.
    SheetFilter f;
    f.col = 2;
    f.op = SheetFilterOp::Contains;
    f.value = "green";
    v.filters.push_back(f);
    RebuildSheetOrder(v, wb, 0);
    SheetColumnStats filtered = SheetComputeStats(wb, 0, v, 1, 1, SheetDisplayCount(v) - 1);
    CHECK(filtered.count == 2);
    CHECK(filtered.sum == 32.0);  // apple 2 + fig 30

    // A sub-range of the display order, as Visual mode selects.
    SheetColumnStats one = SheetComputeStats(wb, 0, v, 1, 1, 1);
    CHECK(one.count == 1);
    CHECK(one.sum == 2.0);
    // An inverted or out-of-bounds range is normalized, not read out of
    // bounds.
    SheetColumnStats flipped = SheetComputeStats(wb, 0, v, 1, 99, -5);
    CHECK(flipped.count == SheetDisplayCount(v));
}

void TestDistinctValues() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;
    RebuildSheetOrder(v, wb, 0);
    bool truncated = true;
    std::vector<SheetValueCount> vals = SheetDistinctValues(wb, 0, v, 2, 200, 0, &truncated);
    CHECK(!truncated);
    CHECK(vals.size() == 5);  // green twice, plus red/yellow/brown/purple
    CHECK(vals[0].value == "green");
    CHECK(vals[0].count == 2);
    // Ties are broken by value so the checklist is stable across rebuilds
    // (an unordered_map walk is not).
    CHECK(vals[1].value == "brown");

    // The value cap reports itself rather than implying the list is whole.
    vals = SheetDistinctValues(wb, 0, v, 2, 2, 0, &truncated);
    CHECK(truncated);
    CHECK(vals.size() == 2);

    // So does the row cap.
    vals = SheetDistinctValues(wb, 0, v, 2, 200, 2, &truncated);
    CHECK(truncated);

    // The header row is not a value: the frozen band is skipped.
    vals = SheetDistinctValues(wb, 0, v, 0, 200, 0, &truncated);
    for (const SheetValueCount &vc : vals) CHECK(vc.value != "name");
}

void TestFindInGrid() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    RebuildSheetOrder(v, wb, 0);
    int d = -1, c = -1;
    CHECK(SheetFindNext(wb, 0, v, "green", true, 0, 0, true, &d, &c));
    CHECK(SheetRowAtDisplay(v, d) == 2);
    CHECK(c == 2);
    // `n` continues from the previous match and finds the second one.
    CHECK(SheetFindNext(wb, 0, v, "green", true, d, c, true, &d, &c));
    CHECK(SheetRowAtDisplay(v, d) == 6);
    // And wraps back around to the first.
    CHECK(SheetFindNext(wb, 0, v, "green", true, d, c, true, &d, &c));
    CHECK(SheetRowAtDisplay(v, d) == 2);
    // Backwards.
    CHECK(SheetFindNext(wb, 0, v, "green", true, d, c, false, &d, &c));
    CHECK(SheetRowAtDisplay(v, d) == 6);

    // Case sensitivity is honoured in both directions.
    CHECK(SheetFindNext(wb, 0, v, "BANANA", true, 0, 0, true, &d, &c));
    CHECK(!SheetFindNext(wb, 0, v, "BANANA", false, 0, 0, true, &d, &c));
    CHECK(SheetFindNext(wb, 0, v, "Banana", false, 0, 0, true, &d, &c));
    CHECK(!SheetFindNext(wb, 0, v, "no such value", true, 0, 0, true, &d, &c));
    CHECK(!SheetFindNext(wb, 0, v, "", true, 0, 0, true, &d, &c));

    // A hidden row's contents are not findable -- which is the point of
    // filtering, and avoids jumping the cursor to an invisible cell.
    SheetFilter f;
    f.col = 2;
    f.op = SheetFilterOp::Contains;
    f.value = "green";
    v.filters.push_back(f);
    RebuildSheetOrder(v, wb, 0);
    CHECK(!SheetFindNext(wb, 0, v, "cherry", true, 0, 0, true, &d, &c));
    CHECK(SheetFindNext(wb, 0, v, "fig", true, 0, 0, true, &d, &c));
}

// ============================================================================
// Apply order to data
// ============================================================================

void TestSortDoesNotTouchTheFile() {
    Workbook plain = LoadCsv(kFruit);
    std::string before, error;
    CHECK(SaveCsvToMemory(plain, before, error));

    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;
    v.sort = SheetSort{0, SheetSortKind::Text, true};
    SheetFilter f;
    f.col = 2;
    f.op = SheetFilterOp::Contains;
    f.value = "green";
    v.filters.push_back(f);
    RebuildSheetOrder(v, wb, 0);
    CHECK(SheetDisplayCount(v) == 3);

    std::string after;
    CHECK(SaveCsvToMemory(wb, after, error));
    // The whole non-destructive guarantee, pinned: sorted six ways and
    // filtered down to three rows, the file still saves byte-identically.
    CHECK(after == before);
}

void TestApplyOrderToData() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;
    v.sort = SheetSort{0, SheetSortKind::Text, false};
    RebuildSheetOrder(v, wb, 0);
    const std::vector<std::string> wanted = ColumnInDisplayOrder(wb, v, 0);

    SheetApplyResult res = SheetApplyOrderToData(wb, 0, v);
    CHECK(res.changed);
    CHECK(res.hidden_rows == 0);
    CHECK(res.moved_rows > 0);
    CHECK(!res.moved_formula);
    // The sort is consumed: the data carries the order now, so a live
    // sort spec would be a claim about something that already happened.
    CHECK(v.sort.col == -1);

    RebuildSheetOrder(v, wb, 0);
    const std::vector<std::string> got = ColumnInDisplayOrder(wb, v, 0);
    CHECK(got == wanted);
    // The header row stayed put.
    CHECK(Text(wb, 0, 0) == "name");
    // Whole rows moved together, not just the sorted column.
    CHECK(Text(wb, 1, 0) == "apple");
    CHECK(Text(wb, 1, 1) == "2");
    CHECK(Text(wb, 1, 2) == "green");

    // Applying again changes nothing.
    SheetApplyResult again = SheetApplyOrderToData(wb, 0, v);
    CHECK(!again.changed);
}

void TestApplyKeepsFilteredRows() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    v.freeze_rows = 1;
    SheetFilter f;
    f.col = 2;
    f.op = SheetFilterOp::Contains;
    f.value = "green";
    v.filters.push_back(f);
    v.sort = SheetSort{0, SheetSortKind::Text, false};
    RebuildSheetOrder(v, wb, 0);

    SheetApplyResult res = SheetApplyOrderToData(wb, 0, v);
    CHECK(res.changed);
    CHECK(res.hidden_rows == 4);
    // Hidden rows are appended, never dropped: applying a view is not a
    // delete, and this is only reversible through undo.
    CHECK(Text(wb, 0, 0) == "name");
    CHECK(Text(wb, 1, 0) == "apple");
    CHECK(Text(wb, 2, 0) == "fig");
    std::vector<std::string> tail;
    for (int r = 3; r <= wb.sheets[0].max_row; r++) tail.push_back(Text(wb, r, 0));
    CHECK(tail.size() == 4);
    CHECK(tail[0] == "cherry");  // the hidden rows keep their own order
    CHECK(tail[1] == "Banana");
    CHECK(tail[2] == "date");
    CHECK(tail[3] == "elderberry");
}

void TestApplyMovesRowHeightsAndRefusesOnFormulas() {
    Workbook wb = LoadCsv("v\n3\n1\n2\n");
    SheetView v;
    v.freeze_rows = 1;
    SheetSetRowHeight(v, 1, 70);  // the row holding "3"
    v.sort = SheetSort{0, SheetSortKind::Number, false};
    RebuildSheetOrder(v, wb, 0);
    CHECK(SheetApplyOrderToData(wb, 0, v).changed);
    // "3" ended up last, and the height it was given went with it --
    // otherwise a row made tall to read a long value leaves its height
    // behind on whatever sorts into its old position.
    CHECK(Text(wb, 3, 0) == "3");
    CHECK(SheetStoredRowHeight(v, 3) == 70);
    CHECK(SheetStoredRowHeight(v, 1) == kSheetRowHeight);

    // A formula that would be relocated is detectable, so the caller can
    // refuse: moving a row does not rewrite the relative references in it.
    Workbook fwb = LoadCsv("v,calc\n3,x\n1,y\n");
    SetCellRaw(fwb, 0, 1, 1, "=A2*2");
    SheetView fv;
    fv.freeze_rows = 1;
    fv.sort = SheetSort{0, SheetSortKind::Number, false};
    RebuildSheetOrder(fv, fwb, 0);
    CHECK(SheetApplyWouldMoveFormula(fwb, 0, fv));
    // Not sorting it means nothing moves, so there is nothing to break.
    SheetView plain;
    plain.freeze_rows = 1;
    RebuildSheetOrder(plain, fwb, 0);
    CHECK(!SheetApplyWouldMoveFormula(fwb, 0, plain));
}

// ============================================================================
// Argument parsing and the status summary
// ============================================================================

void TestArgParsing() {
    int col = -1;
    CHECK(ParseSheetColumnArg("A", &col) && col == 0);
    CHECK(ParseSheetColumnArg("b", &col) && col == 1);
    CHECK(ParseSheetColumnArg(" AB ", &col) && col == 27);
    // A 1-based number is accepted too, because a CSV's columns are as
    // often counted as lettered.
    CHECK(ParseSheetColumnArg("1", &col) && col == 0);
    CHECK(ParseSheetColumnArg("12", &col) && col == 11);
    CHECK(!ParseSheetColumnArg("0", &col));
    CHECK(!ParseSheetColumnArg("A1", &col));
    CHECK(!ParseSheetColumnArg("", &col));
    CHECK(!ParseSheetColumnArg("  ", &col));

    SheetFilterOp op = SheetFilterOp::Contains;
    CHECK(ParseSheetFilterOp("=", &op) && op == SheetFilterOp::Equals);
    CHECK(ParseSheetFilterOp("!=", &op) && op == SheetFilterOp::NotEquals);
    CHECK(ParseSheetFilterOp("~", &op) && op == SheetFilterOp::Contains);
    CHECK(ParseSheetFilterOp("!~", &op) && op == SheetFilterOp::NotContains);
    CHECK(ParseSheetFilterOp(">", &op) && op == SheetFilterOp::Greater);
    CHECK(ParseSheetFilterOp("<", &op) && op == SheetFilterOp::Less);
    CHECK(ParseSheetFilterOp("empty", &op) && op == SheetFilterOp::Empty);
    CHECK(ParseSheetFilterOp("nonempty", &op) && op == SheetFilterOp::NonEmpty);
    CHECK(ParseSheetFilterOp("re", &op) && op == SheetFilterOp::Regex);
    CHECK(!ParseSheetFilterOp("nonsense", &op));
}

void TestViewSummary() {
    SheetView v;
    CHECK(SheetViewSummary(v).empty());
    v.sort = SheetSort{1, SheetSortKind::Auto, true};
    CHECK(SheetViewSummary(v) == "sort B desc");
    SheetFilter f;
    f.col = 0;
    v.filters.push_back(f);
    CHECK(SheetViewSummary(v) == "sort B desc, 1 filter");
    v.filters.push_back(f);
    CHECK(SheetViewSummary(v) == "sort B desc, 2 filters");
    v.sort = SheetSort{};
    CHECK(SheetViewSummary(v) == "2 filters");
}

// ============================================================================
// Degenerate input
// ============================================================================

void TestEmptyAndOutOfRange() {
    Workbook wb;
    SheetView v;
    // A workbook with no sheets at all must not be read out of bounds.
    RebuildSheetOrder(v, wb, 0);
    CHECK(SheetDisplayCount(v) == 0);
    CHECK(SheetRowAtDisplay(v, 0) == -1);
    CHECK(SheetNearestVisibleDisplay(v, 5) == 0);
    SheetGeom g = MakeSheetGeom(v, 1.0f);
    int sd = 0, sc = 0;
    SheetFollowCursor(g, &sd, &sc, 0, 0, 100, 100);
    CHECK(sd == 0);
    SheetColumnStats st = SheetComputeStats(wb, 0, v, 0, 0, 10);
    CHECK(st.count == 0);
    int d = 0, c = 0;
    CHECK(!SheetFindNext(wb, 0, v, "x", true, 0, 0, true, &d, &c));
    CHECK(!SheetApplyOrderToData(wb, 0, v).changed);
    CHECK(!SheetApplyWouldMoveFormula(wb, 0, v));

    // A null view is tolerated by the geometry functions so a renderer
    // that has not resolved a session yet cannot crash on them.
    SheetGeom empty;
    CHECK(SheetColWidth(empty, 0) == kSheetColWidth);
    CHECK(SheetRowHeight(empty, 0) == kSheetRowHeight);
    CHECK(SheetVisibleRows(empty, 100, 0) >= 1);

    // An out-of-range sheet index is a no-op, not a crash.
    Workbook one = LoadCsv("a\n1\n");
    SheetView v2;
    RebuildSheetOrder(v2, one, 7);
    CHECK(SheetDisplayCount(v2) == 0);
    RebuildSheetOrder(v2, one, -1);
    CHECK(SheetDisplayCount(v2) == 0);
}

void TestDirtyFlag() {
    Workbook wb = LoadCsv(kFruit);
    SheetView v;
    // A fresh view starts stale, so a never-built order is never mistaken
    // for "this sheet has no rows".
    CHECK(v.order_dirty);
    EnsureSheetOrder(v, wb, 0);
    CHECK(!v.order_dirty);
    CHECK(SheetDisplayCount(v) == 7);
    // A second Ensure is free.
    EnsureSheetOrder(v, wb, 0);
    CHECK(SheetDisplayCount(v) == 7);
    MarkSheetOrderDirty(v);
    CHECK(v.order_dirty);
    // Growth: typing into a brand-new row is picked up by a rebuild.
    SetCellRaw(wb, 0, 20, 0, "new");
    EnsureSheetOrder(v, wb, 0);
    CHECK(SheetDisplayCount(v) == 21);
    CHECK(SheetRowAtDisplay(v, 20) == 20);
}

int main() {
    TestFixtureShape();

    TestSortTextAscDesc();
    TestSortNumericAndBlanksLast();
    TestSortTextVsNumericDiffer();
    TestSortAutoDetect();
    TestSortIsStable();
    TestSortByEvaluatedValue();

    TestFilterOps();
    TestFiltersAndTogether();
    TestFilterComposedWithSort();
    TestFrozenRowsNeverFiltered();

    TestOrderMappingIsBijective();
    TestIdentityOrderWhenUnsortedUnfiltered();
    TestNearestVisibleDisplay();
    TestVisualRangeUsesDisplayOrder();

    TestWidthHeightClamps();
    TestZoomScalesGeometry();
    TestFontFloorRaisesRowHeight();
    TestOffsetsAndHitTestRoundTrip();
    TestVisibleCountsMatchOldArithmeticWhenUniform();
    TestVisibleRowsWithTallRow();
    TestEdgeHitTest();
    TestMeasurePane();
    TestFollowCursor();
    TestFollowCursorRespectsFreeze();

    TestStats();
    TestDistinctValues();
    TestFindInGrid();

    TestSortDoesNotTouchTheFile();
    TestApplyOrderToData();
    TestApplyKeepsFilteredRows();
    TestApplyMovesRowHeightsAndRefusesOnFormulas();

    TestArgParsing();
    TestViewSummary();

    TestEmptyAndOutOfRange();
    TestDirtyFlag();

    std::printf("sheet_view_test: all checks passed\n");
    return 0;
}
