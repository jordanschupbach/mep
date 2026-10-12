#ifndef MEP_SHEET_VIEW_H
#define MEP_SHEET_VIEW_H

#include <string>
#include <unordered_map>
#include <vector>

#include "sheet_doc.h"

// Deliberately raylib-free, for the same reason sheet_doc.h is: this is
// how a sheet is *shown* -- sizes, zoom, which rows are visible and in
// what order -- as pure data plus integer arithmetic, with no GL context
// and no font metrics anywhere in it. main.cpp's grid renderer and
// Editor::ResizeSheetViewport both compute their geometry by calling the
// functions below and nothing else, which is the whole point of the
// module: those two *must* agree exactly (the same requirement
// kPdfPageGapPx's comment in editor.h describes), and the only way to
// guarantee that is to give them one shared implementation that a unit
// test can pin down (sheet_view_test.cpp).
//
// The division of labour against sheet_doc.h is strict: nothing here
// ever changes a Cell, with the single exception of
// SheetApplyOrderToData, which exists precisely to turn a view into an
// edit and says so in its name. Sorting and filtering are otherwise
// non-destructive -- a Workbook loaded from a file and then sorted six
// ways still saves byte-identically.
//
// The one thing this module cannot do is measure text: autofit needs
// MeasureTextEx, which only exists on the renderer side. So autofit is
// split -- main.cpp measures, SheetSetColWidth stores.

// ============================================================================
// Grid geometry defaults (screen pixels at zoom 1.0)
// ============================================================================
//
// These used to live in editor.h as the *only* grid geometry there was,
// with a comment explaining that fixed-size slots were what let
// ResizeSheetViewport do scroll-follow in raylib-free code with plain
// integer division. Slots are no longer fixed -- per-column widths,
// per-row heights, a zoom factor and filtered-out rows all break that
// arithmetic -- so the reasoning has moved rather than been abandoned:
// scroll-follow is still raylib-free integer arithmetic, it just lives
// here as a running sum instead of a multiplication, and the renderer
// calls the same function instead of reimplementing it.
inline constexpr int kSheetRowHeaderW = 50;
inline constexpr int kSheetColWidth = 90;
inline constexpr int kSheetRowHeight = 22;

// Clamps on user-set sizes. The minima are what keep a column/row
// clickable and resizable after a user has dragged it down to nothing:
// a zero-width column cannot be grabbed again to widen it, which is a
// one-way trip for anyone who overshoots.
inline constexpr int kSheetMinColWidth = 24;
inline constexpr int kSheetMaxColWidth = 1200;
inline constexpr int kSheetMinRowHeight = 12;
inline constexpr int kSheetMaxRowHeight = 400;

// One Alt+arrow press worth of resize, and the keyboard zoom step.
// kSheetZoomStep matches kImageZoomStep/kPdfZoomStep's 1.25 being a bit
// coarse for a grid of text -- a sheet is read at a size, not fitted to
// a page, so it wants finer control.
inline constexpr int kSheetColWidthStep = 10;
inline constexpr int kSheetRowHeightStep = 3;

// Autofit scans at most this many rows of a column, and sizes at most this
// many columns per "fit everything" request. Each scanned cell costs one
// EvaluateCell plus one MeasureTextEx, so a true full-column scan of a
// 100k-row CSV is seconds of work, not milliseconds -- and a frame spent
// measuring is a frame the editor is not drawing. The cap is a sample, and
// the status line says so when it bit, rather than letting a column that
// was fitted to the first few thousand rows look like it was fitted to all
// of them.
inline constexpr int kSheetAutofitScanRows = 4000;
inline constexpr int kSheetAutofitMaxCols = 256;
inline constexpr float kSheetZoomStep = 1.1f;
inline constexpr float kSheetMinZoom = 0.4f;
inline constexpr float kSheetMaxZoom = 4.0f;

// ============================================================================
// Sorting and filtering
// ============================================================================

// SortKind::Auto decides per column from the data (numeric if a majority
// of the column's non-empty cells evaluate to a number), which is what
// makes a single "sort this column" key do the right thing on both a
// name column and a price column without the user having to say which.
enum class SheetSortKind { None, Auto, Text, Number };

struct SheetSort {
    int col = -1;  // -1 == no sort; display order is the sheet's own row order
    SheetSortKind kind = SheetSortKind::Auto;
    bool descending = false;
};

// Row predicates, ANDed together across the whole filter list. Text ops
// compare against the cell's *displayed* text (FormatCellValue of its
// evaluated value, so a formula filters by its result, not its source)
// and are case-insensitive, because a filter the user typed by hand
// should not miss "Boston" because they typed "boston". Greater/Less
// compare numerically when both sides parse as numbers and lexically
// otherwise, so ">2024-01-01" on a text date column still does something
// sensible.
enum class SheetFilterOp { Equals, NotEquals, Contains, NotContains, Greater, Less, Empty, NonEmpty, Regex, OneOf };

struct SheetFilter {
    int col = 0;
    SheetFilterOp op = SheetFilterOp::Contains;
    std::string value;                // every op except OneOf
    std::vector<std::string> values;  // OneOf -- the header menu's value checklist
};

// ============================================================================
// SheetView
// ============================================================================

// Per-*sheet* presentation state. Lives in SheetSession::views, kept
// parallel to Workbook::sheets exactly the way Workbook::xlsx_sheet_paths
// is, so switching sheets in a workbook keeps each sheet's own widths,
// sort and filters instead of sharing one set between them. The pane's
// zoom is deliberately NOT in here -- it belongs to the pane, not the
// sheet, so flipping between sheets must not change the magnification
// (see SheetSession::zoom).
struct SheetView {
    // Sparse per-column/per-row overrides in UNZOOMED pixels -- absent
    // means "the default". Sparse rather than a vector because a user
    // resizes a handful of columns out of however many the file has, and
    // row heights on a 100k-row CSV would otherwise be a 400KB vector
    // holding one interesting value.
    std::unordered_map<int, int> col_w;
    std::unordered_map<int, int> row_h;

    // Rows/columns pinned against the top/left edge while the rest
    // scrolls. Frozen rows are also exempt from sorting and filtering --
    // a header row that sorted itself into the middle of the data would
    // be a bug, not a feature.
    int freeze_rows = 0, freeze_cols = 0;

    SheetSort sort;
    std::vector<SheetFilter> filters;

    // The rendered font's line height in pixels, pushed in once per frame
    // by main.cpp (Editor::SetSheetFontMetrics). The model cannot measure
    // a font -- that needs MeasureTextEx and a GL context -- but it does
    // need the number, because a row shorter than its own text clips it:
    // the editor's global font size (Ctrl+Shift+=) can outgrow the
    // geometric 22px row on its own, with no sheet zoom involved. Zero
    // until the first frame, which simply means no floor is applied yet.
    float font_line_height = 0.0f;

    // ---- Derived. Rebuilt by RebuildSheetOrder; never assigned directly.
    //
    // `order` is the display order: display index -> sheet row. Its first
    // `freeze_rows` entries are always rows 0..freeze_rows-1 in that
    // order (never sorted, never filtered away), which is what lets every
    // consumer -- renderer, cursor motion, scroll-follow, stats, find --
    // treat a display index as a single uniform coordinate instead of
    // special-casing the frozen band. `display_of` is the exact inverse,
    // sized max_row+1, with -1 for a row the filter hid.
    std::vector<int> order;
    std::vector<int> display_of;
    // Set by MarkSheetOrderDirty on anything that could change the order
    // (an edit, a filter, a sort, a freeze change) and cleared by
    // RebuildSheetOrder. Starts true so a never-built view is never read
    // as "no rows".
    bool order_dirty = true;
    // For the "N of M rows" readout. total_rows counts the sheet's used
    // range; visible_rows counts what survived the filters, frozen rows
    // included (they are always visible).
    int total_rows = 0, visible_rows = 0;
};

// Binds a view to the pane's current zoom. Built fresh at each call site
// rather than stored, so a zoom change can never leave a stale copy
// behind; it is two words, so passing it by value costs nothing.
struct SheetGeom {
    const SheetView *view = nullptr;
    float zoom = 1.0f;
};

/**
 * @brief Pairs a view with a zoom factor for the geometry functions below.
 * @param view The sheet's view state.
 * @param zoom The pane's zoom factor; clamped into [kSheetMinZoom, kSheetMaxZoom].
 * @return A SheetGeom referring to `view` (which must outlive it).
 */
SheetGeom MakeSheetGeom(const SheetView &view, float zoom);

/**
 * @brief Clamps a zoom factor into the supported range.
 * @param zoom Requested zoom.
 * @return The zoom clamped to [kSheetMinZoom, kSheetMaxZoom].
 */
float ClampSheetZoom(float zoom);

// ============================================================================
// Geometry
// ============================================================================

/**
 * @brief Returns a column's on-screen width in pixels (override or default, times zoom, clamped).
 * @param g View plus zoom.
 * @param col Zero-based column index.
 * @return Width in screen pixels, never below 1.
 */
int SheetColWidth(const SheetGeom &g, int col);
/**
 * @brief Returns a sheet row's on-screen height in pixels (override or default, times zoom, clamped).
 * @param g View plus zoom.
 * @param row Zero-based *sheet* row index (heights follow the data, so a sorted row keeps its height).
 * @return Height in screen pixels, never below 1.
 */
int SheetRowHeight(const SheetGeom &g, int row);
/**
 * @brief Returns the on-screen height of the row currently shown at a display index.
 * @param g View plus zoom.
 * @param disp Display index into SheetView::order.
 * @return Height in screen pixels; the default height if `disp` is out of range.
 */
int SheetDisplayRowHeight(const SheetGeom &g, int disp);

/**
 * @brief Stores a column width override in unzoomed pixels, clamped to the supported range.
 * @param view View to modify.
 * @param col Zero-based column index.
 * @param px Desired unzoomed width.
 */
void SheetSetColWidth(SheetView &view, int col, int px);
/**
 * @brief Stores a row height override in unzoomed pixels, clamped to the supported range.
 * @param view View to modify.
 * @param row Zero-based sheet row index.
 * @param px Desired unzoomed height.
 */
void SheetSetRowHeight(SheetView &view, int row, int px);
/**
 * @brief Returns a column's stored unzoomed width, or the default when it has no override.
 * @param view View to read.
 * @param col Zero-based column index.
 * @return Unzoomed width in pixels.
 */
int SheetStoredColWidth(const SheetView &view, int col);
/**
 * @brief Returns a row's stored unzoomed height, or the default when it has no override.
 * @param view View to read.
 * @param row Zero-based sheet row index.
 * @return Unzoomed height in pixels.
 */
int SheetStoredRowHeight(const SheetView &view, int row);

// Everything a pane's grid layout is derived from, computed once and
// shared, so the renderer and the editor cannot disagree about where the
// scrollable area starts or how big it is. `grid_h` is the pane height
// already minus the formula bar.
struct SheetPaneMetrics {
    int header_w = 0;      // row-number gutter on the left
    int col_header_h = 0;  // column-letter strip across the top
    int frozen_w = 0;      // pinned columns, just right of the gutter
    int frozen_h = 0;      // pinned rows, just below the column letters
    int avail_w = 0;       // width left for scrolling columns
    int avail_h = 0;       // height left for scrolling rows
};

/**
 * @brief Splits a pane's grid rectangle into gutter, headers, frozen bands and the scrollable area.
 * @param g View plus zoom.
 * @param pane_w Full pane width in pixels.
 * @param grid_h Pane height below the formula bar, in pixels.
 * @return The metrics every other geometry call takes its avail_w/avail_h from.
 */
SheetPaneMetrics SheetMeasurePane(const SheetGeom &g, int pane_w, int grid_h);

/**
 * @brief Returns the x offset of a column relative to the start of the scrollable column area.
 * @param g View plus zoom.
 * @param scroll_col Leftmost scrolling column.
 * @param col Column to locate; may be left of scroll_col, in which case the result is negative.
 * @return Offset in pixels.
 */
int SheetColOffsetX(const SheetGeom &g, int scroll_col, int col);
/**
 * @brief Returns the y offset of a display row relative to the start of the scrollable row area.
 * @param g View plus zoom.
 * @param scroll_disp Topmost scrolling display index.
 * @param disp Display index to locate; may be above scroll_disp, in which case the result is negative.
 * @return Offset in pixels.
 */
int SheetRowOffsetY(const SheetGeom &g, int scroll_disp, int disp);

/**
 * @brief Counts how many columns starting at scroll_col fit in the available width.
 * @param g View plus zoom.
 * @param avail_w Width of the scrollable column area.
 * @param scroll_col Leftmost scrolling column.
 * @return Column count, at least 1 (a column too wide for the pane is still shown, clipped).
 */
int SheetVisibleCols(const SheetGeom &g, int avail_w, int scroll_col);
/**
 * @brief Counts how many display rows starting at scroll_disp fit in the available height.
 * @param g View plus zoom.
 * @param avail_h Height of the scrollable row area.
 * @param scroll_disp Topmost scrolling display index.
 * @return Row count, at least 1.
 */
int SheetVisibleRows(const SheetGeom &g, int avail_h, int scroll_disp);

/**
 * @brief Hit-tests an x offset inside the scrollable column area to a column index.
 * @param g View plus zoom.
 * @param scroll_col Leftmost scrolling column.
 * @param x_rel Offset in pixels from the start of the scrollable column area.
 * @return The column under that offset; scroll_col for a negative offset.
 */
int SheetColAtX(const SheetGeom &g, int scroll_col, int x_rel);
/**
 * @brief Hit-tests a y offset inside the scrollable row area to a display index.
 * @param g View plus zoom.
 * @param scroll_disp Topmost scrolling display index.
 * @param y_rel Offset in pixels from the start of the scrollable row area.
 * @return The display index under that offset, clamped to the last one; scroll_disp for a negative offset.
 */
int SheetDisplayAtY(const SheetGeom &g, int scroll_disp, int y_rel);

/**
 * @brief Finds which column's right-hand edge an x offset is within `tol` pixels of (drag-to-resize).
 * @param g View plus zoom.
 * @param scroll_col Leftmost scrolling column.
 * @param x_rel Offset in pixels from the start of the scrollable column area.
 * @param tol Half-width of the grab zone in pixels.
 * @param avail_w Width of the scrollable column area; edges beyond it are not offered.
 * @return The column whose right edge is nearest within tolerance, or -1.
 */
int SheetColEdgeAtX(const SheetGeom &g, int scroll_col, int x_rel, int tol, int avail_w);
/**
 * @brief Finds which display row's bottom edge a y offset is within `tol` pixels of (drag-to-resize).
 * @param g View plus zoom.
 * @param scroll_disp Topmost scrolling display index.
 * @param y_rel Offset in pixels from the start of the scrollable row area.
 * @param tol Half-height of the grab zone in pixels.
 * @param avail_h Height of the scrollable row area; edges beyond it are not offered.
 * @return The display index whose bottom edge is nearest within tolerance, or -1.
 */
int SheetDisplayEdgeAtY(const SheetGeom &g, int scroll_disp, int y_rel, int tol, int avail_h);

/**
 * @brief Scrolls the minimum amount needed to bring the cursor's cell fully into view.
 * @param g View plus zoom.
 * @param scroll_disp In/out: topmost scrolling display index.
 * @param scroll_col In/out: leftmost scrolling column.
 * @param cursor_disp Display index the cursor is on.
 * @param cursor_col Column the cursor is on.
 * @param avail_w Width of the scrollable column area.
 * @param avail_h Height of the scrollable row area.
 *
 * Idempotent: calling it twice with the same arguments leaves the scroll
 * untouched the second time, which is what lets the renderer call it
 * every frame. Frozen rows/columns are never scrolled away -- the
 * scroll origin is clamped at the first non-frozen row/column.
 */
void SheetFollowCursor(const SheetGeom &g, int *scroll_disp, int *scroll_col, int cursor_disp, int cursor_col,
                       int avail_w, int avail_h);

// ============================================================================
// Display order
// ============================================================================

/**
 * @brief Marks the derived display order stale so the next EnsureSheetOrder rebuilds it.
 * @param view View to invalidate.
 */
void MarkSheetOrderDirty(SheetView &view);
/**
 * @brief Recomputes the display order from the view's filters and sort, unconditionally.
 * @param view View to rebuild; order/display_of/total_rows/visible_rows are replaced.
 * @param wb Workbook holding the sheet; non-const because sorting and filtering evaluate cells.
 * @param sheet Index of the sheet in wb.
 */
void RebuildSheetOrder(SheetView &view, Workbook &wb, int sheet);
/**
 * @brief Rebuilds the display order only if it is marked stale.
 * @param view View to refresh.
 * @param wb Workbook holding the sheet.
 * @param sheet Index of the sheet in wb.
 */
void EnsureSheetOrder(SheetView &view, Workbook &wb, int sheet);

/**
 * @brief Returns how many rows the display order holds (frozen rows included).
 * @param view View to read; must have an up-to-date order.
 * @return The display row count.
 */
int SheetDisplayCount(const SheetView &view);
/**
 * @brief Maps a display index to the sheet row shown there.
 * @param view View to read.
 * @param disp Display index.
 * @return The sheet row, or -1 if `disp` is out of range.
 */
int SheetRowAtDisplay(const SheetView &view, int disp);
/**
 * @brief Maps a sheet row to its display index.
 * @param view View to read.
 * @param row Sheet row.
 * @return The display index, or -1 if the row is filtered out or beyond the used range.
 */
int SheetDisplayOfRow(const SheetView &view, int row);
/**
 * @brief Finds the display index to put the cursor on when its own row is hidden or gone.
 * @param view View to read.
 * @param row The sheet row the cursor was on.
 * @return The display index of that row if visible, else of the nearest visible row at or after it, else the last one; 0 when nothing is visible.
 */
int SheetNearestVisibleDisplay(const SheetView &view, int row);

/**
 * @brief Tests whether a row falls inside a rectangular selection's row span, measured in display order.
 * @param view View supplying the display order.
 * @param anchor_row Sheet row one end of the selection is anchored to.
 * @param cursor_row Sheet row the other end is on.
 * @param row Sheet row to test.
 * @return True if `row` is visible and lies between the two endpoints in display order.
 *
 * Display order, not sheet-row order, is what makes a Visual-mode range
 * over a filtered view mean what it looks like: the rows the user can
 * see between the two corners. Comparing sheet rows instead would sweep
 * in every hidden row that happens to lie numerically between them --
 * and `x` would then clear data that was never on screen.
 */
bool SheetDisplayRangeContains(const SheetView &view, int anchor_row, int cursor_row, int row);

/**
 * @brief Tests one row against a filter list (all filters must pass).
 * @param wb Workbook holding the sheet; non-const because the test evaluates cells.
 * @param sheet Index of the sheet in wb.
 * @param row Zero-based sheet row to test.
 * @param filters Filters to apply; an empty list passes everything.
 * @return True if the row should be visible.
 */
bool SheetRowPasses(Workbook &wb, int sheet, int row, const std::vector<SheetFilter> &filters);

// ============================================================================
// Analysis helpers
// ============================================================================

// What the stats readout reports for a column or a selected range. `sum`
// /`mean`/`min`/`max` describe the numeric cells only (`numeric` of
// them); a column of names reports count/non-empty/distinct and leaves
// the arithmetic at zero rather than inventing a number for it.
struct SheetColumnStats {
    int count = 0;     // cells examined
    int nonempty = 0;  // cells with any content
    int numeric = 0;   // cells whose value is a number
    int distinct = 0;  // distinct non-empty displayed values
    double sum = 0;
    double mean = 0;
    double min = 0;
    double max = 0;
};

/**
 * @brief Computes column statistics over a display-index range, so filtered-out rows are excluded.
 * @param wb Workbook holding the sheet; non-const because it evaluates cells.
 * @param sheet Index of the sheet in wb.
 * @param view View supplying the display order.
 * @param col Column to summarize.
 * @param disp0 First display index in the range, inclusive.
 * @param disp1 Last display index in the range, inclusive; clamped to the display count.
 * @return The statistics for that range.
 */
SheetColumnStats SheetComputeStats(Workbook &wb, int sheet, const SheetView &view, int col, int disp0, int disp1);

// One distinct value in a column, with how many visible rows hold it.
struct SheetValueCount {
    std::string value;
    int count = 0;
};

/**
 * @brief Collects a column's distinct displayed values with frequencies, most frequent first.
 * @param wb Workbook holding the sheet; non-const because it evaluates cells.
 * @param sheet Index of the sheet in wb.
 * @param view View supplying the display order (frozen rows are skipped -- a header is not data).
 * @param col Column to scan.
 * @param value_cap Maximum distinct values to return.
 * @param row_cap Maximum rows to scan.
 * @param truncated Receives true when either cap cut the scan short, so a caller can say so rather than imply the list is complete.
 * @return Up to `value_cap` values ordered by descending count, then by value.
 */
std::vector<SheetValueCount> SheetDistinctValues(Workbook &wb, int sheet, const SheetView &view, int col, int value_cap,
                                                 int row_cap, bool *truncated);

/**
 * @brief Searches cell text in display order and returns the next match after a starting cell.
 * @param wb Workbook holding the sheet; non-const because it evaluates cells.
 * @param sheet Index of the sheet in wb.
 * @param view View supplying the display order.
 * @param query Text to look for as a substring of a cell's displayed value.
 * @param ignore_case Whether to match case-insensitively.
 * @param from_disp Display index to start after (or before, searching backwards).
 * @param from_col Column to start after.
 * @param forward Search direction.
 * @param out_disp Receives the matching display index.
 * @param out_col Receives the matching column.
 * @return True if a match was found; the search wraps around once.
 */
bool SheetFindNext(Workbook &wb, int sheet, const SheetView &view, const std::string &query, bool ignore_case,
                   int from_disp, int from_col, bool forward, int *out_disp, int *out_col);

// What SheetApplyOrderToData did, so the caller can say so precisely --
// "applied" on its own would leave a user with a filter active guessing
// what happened to the rows they could not see.
struct SheetApplyResult {
    bool changed = false;
    int moved_rows = 0;   // rows that ended up somewhere new
    int hidden_rows = 0;  // filtered-out rows appended after the visible ones
    bool moved_formula = false;
};

/**
 * @brief Reports whether applying the current display order would relocate a cell holding a formula.
 * @param wb Workbook holding the sheet.
 * @param sheet Index of the sheet in wb.
 * @param view View supplying the display order.
 * @return True if a formula cell would move.
 *
 * Moving a row does not rewrite the relative references inside its
 * formulas, so a sheet with formulas would be silently corrupted. The
 * caller refuses on this unless the user forces it. A CSV never trips
 * it in practice: SaveCsvToMemory writes evaluated values, so a CSV
 * round-trip has already turned every formula into its result.
 */
bool SheetApplyWouldMoveFormula(const Workbook &wb, int sheet, const SheetView &view);

/**
 * @brief Rewrites the sheet's rows into the current display order, turning the view into data.
 * @param wb Workbook to modify; rows are rewritten through SetCellRaw.
 * @param sheet Index of the sheet in wb.
 * @param view View supplying the display order; its sort is cleared afterwards and its order marked stale.
 * @return What was done; `changed` is false when the order was already the identity.
 *
 * Filtered-out rows are not discarded: they are appended after the
 * visible ones in their original relative order, so the operation never
 * loses data even with a filter active. Frozen rows stay at the top.
 * Row-height overrides travel with the rows they were set on.
 */
SheetApplyResult SheetApplyOrderToData(Workbook &wb, int sheet, SheetView &view);

/**
 * @brief Parses a column argument written as letters ("A", "ab") or a 1-based number ("3").
 * @param text Argument text; surrounding spaces are ignored.
 * @param out Receives the zero-based column index.
 * @return True if the text was a usable column reference.
 */
bool ParseSheetColumnArg(const std::string &text, int *out);

/**
 * @brief Parses a filter operator token ("=", "!=", "~", "!~", ">", "<", "empty", "nonempty", "re").
 * @param text Token text.
 * @param out Receives the operator.
 * @return True if the token names an operator.
 */
bool ParseSheetFilterOp(const std::string &text, SheetFilterOp *out);

/**
 * @brief Renders a short human-readable summary of a view's sort and filters for the status readout.
 * @param view View to describe.
 * @return A string like "sort B desc, 2 filters", or an empty string when neither is set.
 */
std::string SheetViewSummary(const SheetView &view);

#endif
