#include "sheet_view.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

#include "formula.h"
#include "regex.h"

namespace {

/**
 * @brief Lowercases the ASCII letters in a string, leaving every other byte alone.
 * @param s Input text.
 * @return The folded copy.
 *
 * ASCII-only on purpose: this is the same fold :set ignorecase applies to
 * buffer search (Editor::CiFind), so a sheet filter and a buffer search
 * agree about what "case-insensitive" means, and neither pretends to do
 * Unicode case folding it hasn't got the tables for.
 */
std::string LowerAscii(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        unsigned char u = static_cast<unsigned char>(c);
        c = static_cast<char>(std::tolower(u));
    }
    return out;
}

/**
 * @brief Returns a cell's displayed text -- what the grid shows and what filters/sorts compare.
 * @param wb Workbook holding the sheet.
 * @param sheet Index of the sheet in wb.
 * @param row Zero-based row.
 * @param col Zero-based column.
 * @return The formatted value, empty for a cell that was never given content.
 *
 * Deliberately the *evaluated* value rather than Cell::raw, so a formula
 * sorts and filters by its result the way it reads on screen. Routing
 * through EvaluateCell also keeps the "every read goes through the one
 * choke point" rule sheet_doc.h sets out, and never inserts a cell, so
 * scanning a sparse sheet stays sparse.
 */
std::string CellText(Workbook &wb, int sheet, int row, int col) {
    return FormatCellValue(EvaluateCell(wb, sheet, row, col));
}

/**
 * @brief Parses a whole string as a number.
 * @param text Text to parse.
 * @param out Receives the value on success.
 * @return True only if the entire (trimmed) string was consumed as a number.
 */
bool ParseWholeNumber(const std::string &text, double *out) {
    size_t a = text.find_first_not_of(" \t");
    if (a == std::string::npos) return false;
    size_t z = text.find_last_not_of(" \t");
    std::string trimmed = text.substr(a, z - a + 1);
    const char *begin = trimmed.c_str();
    char *end = nullptr;
    double v = std::strtod(begin, &end);
    if (end == begin || *end != '\0') return false;
    if (out) *out = v;
    return true;
}

// A row's sort key, computed once per row rather than per comparison --
// a comparator that called EvaluateCell would evaluate the same cell
// O(log n) times, which on a sorted-by-formula column is the difference
// between instant and noticeable.
struct SortKey {
    bool empty = true;
    bool numeric = false;
    double number = 0;
    std::string folded;  // lower-cased displayed text
};

/**
 * @brief Orders two non-empty sort keys.
 * @param a First key.
 * @param b Second key.
 * @param numeric_mode Whether the column is being sorted as numbers.
 * @return Negative if a sorts first, positive if b does, 0 if they tie.
 *
 * In numeric mode a number always sorts ahead of text (Excel's own
 * convention for a mixed column) and text ties are broken lexically, so
 * a column of mostly-prices with a stray "n/a" still sorts usefully
 * instead of falling back to string order for everything.
 */
int CompareSortKeys(const SortKey &a, const SortKey &b, bool numeric_mode) {
    if (numeric_mode) {
        if (a.numeric && b.numeric) {
            if (a.number < b.number) return -1;
            if (a.number > b.number) return 1;
            return 0;
        }
        if (a.numeric != b.numeric) return a.numeric ? -1 : 1;
    }
    return a.folded.compare(b.folded);
}

/**
 * @brief Tests one cell's text against one filter.
 * @param text The cell's displayed text.
 * @param f The filter to apply.
 * @param rx Pre-compiled pattern for SheetFilterOp::Regex, or nullptr to compile on demand.
 * @return True if the cell passes.
 */
bool TextPasses(const std::string &text, const SheetFilter &f, const mep_regex::Regex *rx) {
    const std::string folded = LowerAscii(text);
    const std::string needle = LowerAscii(f.value);
    switch (f.op) {
        case SheetFilterOp::Empty: return text.empty();
        case SheetFilterOp::NonEmpty: return !text.empty();
        case SheetFilterOp::Equals: return folded == needle;
        case SheetFilterOp::NotEquals: return folded != needle;
        case SheetFilterOp::Contains: return folded.find(needle) != std::string::npos;
        case SheetFilterOp::NotContains: return folded.find(needle) == std::string::npos;
        case SheetFilterOp::Greater:
        case SheetFilterOp::Less: {
            // The *filter value* decides which comparison is meant. A
            // numeric threshold (">5") is a numeric question, so a cell
            // that is not a number cannot satisfy it -- comparing "n/a"
            // to "5" as text would pass it, since 'n' > '5', and a
            // column of prices filtered to ">5" would come back with its
            // blanks and its "n/a"s in it. A non-numeric threshold
            // (">2024-03") is a lexical question and compares as text,
            // so a text date column still filters usefully.
            double want_num = 0;
            if (ParseWholeNumber(f.value, &want_num)) {
                double cell_num = 0;
                if (!ParseWholeNumber(text, &cell_num)) return false;
                return f.op == SheetFilterOp::Greater ? cell_num > want_num : cell_num < want_num;
            }
            const int c = folded.compare(needle);
            return f.op == SheetFilterOp::Greater ? c > 0 : c < 0;
        }
        case SheetFilterOp::Regex: {
            if (rx) return rx->ok() && rx->PartialMatch(text);
            mep_regex::Regex local(f.value, /*ignore_case=*/true);
            return local.ok() && local.PartialMatch(text);
        }
        case SheetFilterOp::OneOf: {
            for (const std::string &v : f.values) {
                if (LowerAscii(v) == folded) return true;
            }
            return false;
        }
    }
    return true;
}

}  // namespace

SheetGeom MakeSheetGeom(const SheetView &view, float zoom) {
    SheetGeom g;
    g.view = &view;
    g.zoom = ClampSheetZoom(zoom);
    return g;
}

float ClampSheetZoom(float zoom) { return std::min(kSheetMaxZoom, std::max(kSheetMinZoom, zoom)); }

// ============================================================================
// Geometry
// ============================================================================

namespace {

/**
 * @brief Scales an unzoomed pixel size by a zoom factor.
 * @param px Unzoomed size.
 * @param zoom Zoom factor.
 * @return The scaled size, never below 1 (a zero-size slot cannot be clicked or grabbed again).
 */
int Scaled(int px, float zoom) {
    const double v = static_cast<double>(px) * static_cast<double>(zoom);
    return std::max(1, static_cast<int>(std::lround(v)));
}

}  // namespace

int SheetStoredColWidth(const SheetView &view, int col) {
    auto it = view.col_w.find(col);
    return it == view.col_w.end() ? kSheetColWidth : it->second;
}

int SheetStoredRowHeight(const SheetView &view, int row) {
    // The stored, unzoomed value only -- deliberately without the font
    // floor SheetRowHeight applies, so round-tripping a height through
    // SheetSetRowHeight(SheetStoredRowHeight(...)) cannot creep upwards
    // every time the font happens to be large.
    auto it = view.row_h.find(row);
    return it == view.row_h.end() ? kSheetRowHeight : it->second;
}

int SheetColWidth(const SheetGeom &g, int col) {
    const int base = g.view ? SheetStoredColWidth(*g.view, col) : kSheetColWidth;
    return Scaled(base, g.zoom);
}

int SheetRowHeight(const SheetGeom &g, int row) {
    if (!g.view) return Scaled(kSheetRowHeight, g.zoom);
    // The override/default is unzoomed and scales; the font floor does
    // not, because font_line_height is measured at the already-zoomed
    // font size the renderer is about to draw with. Scaling it here would
    // apply the zoom twice.
    auto it = g.view->row_h.find(row);
    const int base = it == g.view->row_h.end() ? kSheetRowHeight : it->second;
    int px = Scaled(base, g.zoom);
    if (g.view->font_line_height > 0.0f) {
        px = std::max(px, static_cast<int>(std::ceil(static_cast<double>(g.view->font_line_height))) + 4);
    }
    return px;
}

int SheetDisplayRowHeight(const SheetGeom &g, int disp) {
    if (!g.view) return Scaled(kSheetRowHeight, g.zoom);
    const int row = SheetRowAtDisplay(*g.view, disp);
    return SheetRowHeight(g, row < 0 ? 0 : row);
}

void SheetSetColWidth(SheetView &view, int col, int px) {
    if (col < 0) return;
    view.col_w[col] = std::min(kSheetMaxColWidth, std::max(kSheetMinColWidth, px));
}

void SheetSetRowHeight(SheetView &view, int row, int px) {
    if (row < 0) return;
    view.row_h[row] = std::min(kSheetMaxRowHeight, std::max(kSheetMinRowHeight, px));
}

SheetPaneMetrics SheetMeasurePane(const SheetGeom &g, int pane_w, int grid_h) {
    SheetPaneMetrics m;
    m.header_w = Scaled(kSheetRowHeaderW, g.zoom);
    m.col_header_h = Scaled(kSheetRowHeight, g.zoom);

    const int freeze_cols = g.view ? std::max(0, g.view->freeze_cols) : 0;
    const int freeze_rows = g.view ? std::max(0, g.view->freeze_rows) : 0;

    // The frozen bands are capped at roughly half the pane: a user who
    // freezes more columns than fit would otherwise leave no scrolling
    // area at all and no way to see the rest of the data -- and with
    // avail_w at 0 the scroll-follow arithmetic has nothing to work
    // with. Capping degrades gracefully instead (the band clips).
    const int body_w = std::max(0, pane_w - m.header_w);
    const int body_h = std::max(0, grid_h - m.col_header_h);
    for (int c = 0; c < freeze_cols; c++) {
        const int w = SheetColWidth(g, c);
        if (m.frozen_w + w > body_w / 2) break;
        m.frozen_w += w;
    }
    for (int d = 0; d < freeze_rows; d++) {
        const int h = SheetDisplayRowHeight(g, d);
        if (m.frozen_h + h > body_h / 2) break;
        m.frozen_h += h;
    }

    m.avail_w = std::max(0, body_w - m.frozen_w);
    m.avail_h = std::max(0, body_h - m.frozen_h);
    return m;
}

int SheetColOffsetX(const SheetGeom &g, int scroll_col, int col) {
    if (col == scroll_col) return 0;
    int x = 0;
    if (col > scroll_col) {
        for (int c = scroll_col; c < col; c++) x += SheetColWidth(g, c);
        return x;
    }
    for (int c = col; c < scroll_col; c++) x -= SheetColWidth(g, c);
    return x;
}

int SheetRowOffsetY(const SheetGeom &g, int scroll_disp, int disp) {
    if (disp == scroll_disp) return 0;
    int y = 0;
    if (disp > scroll_disp) {
        for (int d = scroll_disp; d < disp; d++) y += SheetDisplayRowHeight(g, d);
        return y;
    }
    for (int d = disp; d < scroll_disp; d++) y -= SheetDisplayRowHeight(g, d);
    return y;
}

int SheetVisibleCols(const SheetGeom &g, int avail_w, int scroll_col) {
    int n = 0, x = 0;
    // Counts the trailing partially-visible column too: the renderer
    // clips it and drawing half a column is how a grid shows there is
    // more to the right.
    while (x < avail_w) {
        x += SheetColWidth(g, scroll_col + n);
        n++;
        if (n > 4096) break;  // runaway guard; no pane is this wide
    }
    return std::max(1, n);
}

int SheetVisibleRows(const SheetGeom &g, int avail_h, int scroll_disp) {
    int n = 0, y = 0;
    while (y < avail_h) {
        y += SheetDisplayRowHeight(g, scroll_disp + n);
        n++;
        if (n > 8192) break;  // runaway guard
    }
    return std::max(1, n);
}

int SheetColAtX(const SheetGeom &g, int scroll_col, int x_rel) {
    if (x_rel <= 0) return scroll_col;
    int x = 0;
    for (int c = scroll_col; c < scroll_col + 4096; c++) {
        x += SheetColWidth(g, c);
        if (x_rel < x) return c;
    }
    return scroll_col;
}

int SheetDisplayAtY(const SheetGeom &g, int scroll_disp, int y_rel) {
    const int count = g.view ? SheetDisplayCount(*g.view) : 0;
    if (y_rel <= 0) return scroll_disp;
    int y = 0;
    for (int d = scroll_disp; d < count; d++) {
        y += SheetDisplayRowHeight(g, d);
        if (y_rel < y) return d;
    }
    return std::max(scroll_disp, count - 1);
}

int SheetColEdgeAtX(const SheetGeom &g, int scroll_col, int x_rel, int tol, int avail_w) {
    int x = 0;
    for (int c = scroll_col; c < scroll_col + 4096; c++) {
        x += SheetColWidth(g, c);
        if (x > avail_w + tol) break;
        if (std::abs(x_rel - x) <= tol) return c;
    }
    return -1;
}

int SheetDisplayEdgeAtY(const SheetGeom &g, int scroll_disp, int y_rel, int tol, int avail_h) {
    const int count = g.view ? SheetDisplayCount(*g.view) : 0;
    int y = 0;
    for (int d = scroll_disp; d < count; d++) {
        y += SheetDisplayRowHeight(g, d);
        if (y > avail_h + tol) break;
        if (std::abs(y_rel - y) <= tol) return d;
    }
    return -1;
}

void SheetFollowCursor(const SheetGeom &g, int *scroll_disp, int *scroll_col, int cursor_disp, int cursor_col,
                       int avail_w, int avail_h) {
    if (!scroll_disp || !scroll_col) return;
    const int freeze_cols = g.view ? std::max(0, g.view->freeze_cols) : 0;
    const int freeze_rows = g.view ? std::max(0, g.view->freeze_rows) : 0;

    // The scroll origin can never move into the frozen bands: those
    // rows/columns are drawn pinned, so scrolling them would show them
    // twice.
    *scroll_col = std::max(*scroll_col, freeze_cols);
    *scroll_disp = std::max(*scroll_disp, freeze_rows);

    // A cursor inside a frozen band is always on screen -- nothing to
    // follow on that axis.
    if (cursor_col >= freeze_cols) {
        if (cursor_col < *scroll_col) *scroll_col = cursor_col;
        // Walking *backwards* from the cursor to find the leftmost
        // column that still leaves it fully visible costs O(visible
        // columns). The obvious forward loop ("while it doesn't fit,
        // scroll one more") is O(distance) iterations of an O(distance)
        // offset sum -- quadratic, and a `$` or `G` jump on a wide/tall
        // sheet would stall a frame for it.
        int first = cursor_col;
        int acc = SheetColWidth(g, cursor_col);
        while (first > freeze_cols) {
            const int w = SheetColWidth(g, first - 1);
            if (acc + w > avail_w) break;
            acc += w;
            first--;
        }
        if (*scroll_col < first) *scroll_col = first;
    }

    if (cursor_disp >= freeze_rows) {
        if (cursor_disp < *scroll_disp) *scroll_disp = cursor_disp;
        int first = cursor_disp;
        int acc = SheetDisplayRowHeight(g, cursor_disp);
        while (first > freeze_rows) {
            const int h = SheetDisplayRowHeight(g, first - 1);
            if (acc + h > avail_h) break;
            acc += h;
            first--;
        }
        if (*scroll_disp < first) *scroll_disp = first;
    }
}

// ============================================================================
// Display order
// ============================================================================

void MarkSheetOrderDirty(SheetView &view) { view.order_dirty = true; }

void EnsureSheetOrder(SheetView &view, Workbook &wb, int sheet) {
    if (view.order_dirty) RebuildSheetOrder(view, wb, sheet);
}

void RebuildSheetOrder(SheetView &view, Workbook &wb, int sheet) {
    view.order.clear();
    view.display_of.clear();
    view.total_rows = 0;
    view.visible_rows = 0;
    view.order_dirty = false;
    if (sheet < 0 || static_cast<size_t>(sheet) >= wb.sheets.size()) return;

    const int rows = wb.sheets[static_cast<size_t>(sheet)].max_row + 1;
    view.total_rows = rows;
    view.display_of.assign(static_cast<size_t>(rows), -1);

    const int freeze = std::min(std::max(0, view.freeze_rows), rows);
    // The frozen band leads the display order unconditionally: it is
    // neither sorted nor filtered, which is what makes "freeze the
    // header row" also mean "the header row is not data".
    for (int r = 0; r < freeze; r++) view.order.push_back(r);

    // Regexes are compiled once for the whole scan, not once per row --
    // compiling a pattern per row turns a filter on a 100k-row CSV into
    // 100k parser runs.
    std::vector<std::unique_ptr<mep_regex::Regex>> compiled(view.filters.size());
    for (size_t i = 0; i < view.filters.size(); i++) {
        if (view.filters[i].op == SheetFilterOp::Regex) {
            compiled[i] = std::make_unique<mep_regex::Regex>(view.filters[i].value, /*ignore_case=*/true);
        }
    }

    std::vector<int> candidates;
    candidates.reserve(static_cast<size_t>(std::max(0, rows - freeze)));
    for (int r = freeze; r < rows; r++) {
        bool keep = true;
        for (size_t i = 0; i < view.filters.size() && keep; i++) {
            const SheetFilter &f = view.filters[i];
            keep = TextPasses(CellText(wb, sheet, r, f.col), f, compiled[i].get());
        }
        if (keep) candidates.push_back(r);
    }

    if (view.sort.col >= 0 && view.sort.kind != SheetSortKind::None && !candidates.empty()) {
        std::vector<SortKey> keys(candidates.size());
        int numeric_count = 0, nonempty_count = 0;
        for (size_t i = 0; i < candidates.size(); i++) {
            const std::string text = CellText(wb, sheet, candidates[i], view.sort.col);
            SortKey &k = keys[i];
            k.empty = text.empty();
            if (!k.empty) {
                nonempty_count++;
                k.numeric = ParseWholeNumber(text, &k.number);
                if (k.numeric) numeric_count++;
                k.folded = LowerAscii(text);
            }
        }
        bool numeric_mode = view.sort.kind == SheetSortKind::Number;
        if (view.sort.kind == SheetSortKind::Auto) {
            // A simple majority, so one stray "n/a" in a price column
            // does not flip the whole column to string order.
            numeric_mode = nonempty_count > 0 && numeric_count * 2 >= nonempty_count;
        }
        const bool descending = view.sort.descending;

        std::vector<size_t> perm(candidates.size());
        for (size_t i = 0; i < perm.size(); i++) perm[i] = i;
        // stable_sort, not sort: equal keys must keep their original row
        // order, so sorting by one column twice is idempotent and
        // sorting by column A then column B gives a predictable
        // A-within-B result instead of shuffling ties each time.
        std::stable_sort(perm.begin(), perm.end(), [&](size_t a, size_t b) {
            const SortKey &ka = keys[a];
            const SortKey &kb = keys[b];
            // Blanks sink to the bottom in BOTH directions. Reversing
            // them with the sort would fill the first screen of a
            // descending sort with empty rows, which is never what the
            // user wanted to look at.
            if (ka.empty != kb.empty) return !ka.empty;
            if (ka.empty) return false;
            const int c = CompareSortKeys(ka, kb, numeric_mode);
            if (c == 0) return false;
            return descending ? c > 0 : c < 0;
        });
        for (size_t i : perm) view.order.push_back(candidates[i]);
    } else {
        for (int r : candidates) view.order.push_back(r);
    }

    for (size_t d = 0; d < view.order.size(); d++) {
        const int row = view.order[d];
        if (row >= 0 && static_cast<size_t>(row) < view.display_of.size()) {
            view.display_of[static_cast<size_t>(row)] = static_cast<int>(d);
        }
    }
    view.visible_rows = static_cast<int>(view.order.size());
}

int SheetDisplayCount(const SheetView &view) { return static_cast<int>(view.order.size()); }

int SheetRowAtDisplay(const SheetView &view, int disp) {
    if (disp < 0 || static_cast<size_t>(disp) >= view.order.size()) return -1;
    return view.order[static_cast<size_t>(disp)];
}

int SheetDisplayOfRow(const SheetView &view, int row) {
    if (row < 0 || static_cast<size_t>(row) >= view.display_of.size()) return -1;
    return view.display_of[static_cast<size_t>(row)];
}

int SheetNearestVisibleDisplay(const SheetView &view, int row) {
    const int count = SheetDisplayCount(view);
    if (count <= 0) return 0;
    const int direct = SheetDisplayOfRow(view, row);
    if (direct >= 0) return direct;
    // The row the cursor was on is hidden (a new filter) or gone. Land on
    // the next visible row *after* it so the cursor keeps moving the way
    // the user was already going, and only fall back to the last row when
    // there is nothing after it.
    for (int r = row + 1; r < static_cast<int>(view.display_of.size()); r++) {
        const int d = SheetDisplayOfRow(view, r);
        if (d >= 0) return d;
    }
    for (int r = std::min(row, static_cast<int>(view.display_of.size())) - 1; r >= 0; r--) {
        const int d = SheetDisplayOfRow(view, r);
        if (d >= 0) return d;
    }
    return count - 1;
}

bool SheetDisplayRangeContains(const SheetView &view, int anchor_row, int cursor_row, int row) {
    const int a = SheetDisplayOfRow(view, anchor_row);
    const int c = SheetDisplayOfRow(view, cursor_row);
    const int d = SheetDisplayOfRow(view, row);
    if (d < 0) return false;  // hidden rows are never part of a selection
    // An endpoint that is itself hidden (the filter changed under the
    // selection) falls back to its nearest visible neighbour rather than
    // collapsing the whole range to nothing.
    const int lo = a < 0 ? SheetNearestVisibleDisplay(view, anchor_row) : a;
    const int hi = c < 0 ? SheetNearestVisibleDisplay(view, cursor_row) : c;
    return d >= std::min(lo, hi) && d <= std::max(lo, hi);
}

bool SheetRowPasses(Workbook &wb, int sheet, int row, const std::vector<SheetFilter> &filters) {
    for (const SheetFilter &f : filters) {
        if (!TextPasses(CellText(wb, sheet, row, f.col), f, nullptr)) return false;
    }
    return true;
}

// ============================================================================
// Analysis helpers
// ============================================================================

SheetColumnStats SheetComputeStats(Workbook &wb, int sheet, const SheetView &view, int col, int disp0, int disp1) {
    SheetColumnStats st;
    const int count = SheetDisplayCount(view);
    if (count <= 0 || col < 0) return st;
    const int lo = std::max(0, std::min(disp0, disp1));
    const int hi = std::min(count - 1, std::max(disp0, disp1));
    std::unordered_set<std::string> distinct;
    bool have_extremes = false;
    for (int d = lo; d <= hi; d++) {
        const int row = SheetRowAtDisplay(view, d);
        if (row < 0) continue;
        st.count++;
        const std::string text = CellText(wb, sheet, row, col);
        if (text.empty()) continue;
        st.nonempty++;
        distinct.insert(text);
        double num = 0;
        if (ParseWholeNumber(text, &num)) {
            st.numeric++;
            st.sum += num;
            if (!have_extremes) {
                st.min = st.max = num;
                have_extremes = true;
            } else {
                st.min = std::min(st.min, num);
                st.max = std::max(st.max, num);
            }
        }
    }
    st.distinct = static_cast<int>(distinct.size());
    if (st.numeric > 0) st.mean = st.sum / static_cast<double>(st.numeric);
    return st;
}

std::vector<SheetValueCount> SheetDistinctValues(Workbook &wb, int sheet, const SheetView &view, int col, int value_cap,
                                                 int row_cap, bool *truncated) {
    std::vector<SheetValueCount> out;
    if (truncated) *truncated = false;
    if (col < 0) return out;
    const int count = SheetDisplayCount(view);
    const int freeze = std::max(0, view.freeze_rows);
    std::unordered_map<std::string, int> tally;
    int scanned = 0;
    for (int d = freeze; d < count; d++) {
        if (row_cap > 0 && scanned >= row_cap) {
            if (truncated) *truncated = true;
            break;
        }
        const int row = SheetRowAtDisplay(view, d);
        if (row < 0) continue;
        scanned++;
        const std::string text = CellText(wb, sheet, row, col);
        if (text.empty()) continue;
        tally[text]++;
    }
    out.reserve(tally.size());
    for (const auto &kv : tally) out.push_back(SheetValueCount{kv.first, kv.second});
    // Most frequent first, ties broken by value, so the checklist is
    // deterministic across rebuilds (an unordered_map walk is not).
    std::sort(out.begin(), out.end(), [](const SheetValueCount &a, const SheetValueCount &b) {
        if (a.count != b.count) return a.count > b.count;
        return a.value < b.value;
    });
    if (value_cap > 0 && static_cast<int>(out.size()) > value_cap) {
        out.resize(static_cast<size_t>(value_cap));
        if (truncated) *truncated = true;
    }
    return out;
}

bool SheetFindNext(Workbook &wb, int sheet, const SheetView &view, const std::string &query, bool ignore_case,
                   int from_disp, int from_col, bool forward, int *out_disp, int *out_col) {
    if (query.empty()) return false;
    if (sheet < 0 || static_cast<size_t>(sheet) >= wb.sheets.size()) return false;
    const int count = SheetDisplayCount(view);
    const int cols = wb.sheets[static_cast<size_t>(sheet)].max_col + 1;
    if (count <= 0 || cols <= 0) return false;
    const std::string needle = ignore_case ? LowerAscii(query) : query;

    // One flat index over the display grid, so "next match" wraps off the
    // end of a row into the next row and off the end of the sheet back to
    // the start -- the same shape as `n` in a text buffer.
    const long long total = static_cast<long long>(count) * static_cast<long long>(cols);
    long long start = static_cast<long long>(std::max(0, std::min(from_disp, count - 1))) * cols +
                      std::max(0, std::min(from_col, cols - 1));
    for (long long step = 1; step <= total; step++) {
        long long at = forward ? start + step : start - step;
        at = ((at % total) + total) % total;
        const int d = static_cast<int>(at / cols);
        const int c = static_cast<int>(at % cols);
        const int row = SheetRowAtDisplay(view, d);
        if (row < 0) continue;
        std::string text = CellText(wb, sheet, row, c);
        if (ignore_case) text = LowerAscii(text);
        if (text.find(needle) != std::string::npos) {
            if (out_disp) *out_disp = d;
            if (out_col) *out_col = c;
            return true;
        }
    }
    return false;
}

namespace {

/**
 * @brief Builds the row permutation SheetApplyOrderToData would write: visible rows in display order, then hidden rows.
 * @param view View supplying the display order.
 * @param rows The sheet's row count (max_row + 1).
 * @param hidden_count Receives how many rows were appended because a filter hid them.
 * @return target[i] == the sheet row that ends up at row i.
 */
std::vector<int> BuildApplyTarget(const SheetView &view, int rows, int *hidden_count) {
    std::vector<int> target;
    target.reserve(static_cast<size_t>(std::max(0, rows)));
    std::vector<char> taken(static_cast<size_t>(std::max(0, rows)), 0);
    for (int d = 0; d < SheetDisplayCount(view); d++) {
        const int row = SheetRowAtDisplay(view, d);
        if (row < 0 || row >= rows || taken[static_cast<size_t>(row)]) continue;
        taken[static_cast<size_t>(row)] = 1;
        target.push_back(row);
    }
    int hidden = 0;
    for (int r = 0; r < rows; r++) {
        if (!taken[static_cast<size_t>(r)]) {
            target.push_back(r);
            hidden++;
        }
    }
    if (hidden_count) *hidden_count = hidden;
    return target;
}

}  // namespace

bool SheetApplyWouldMoveFormula(const Workbook &wb, int sheet, const SheetView &view) {
    if (sheet < 0 || static_cast<size_t>(sheet) >= wb.sheets.size()) return false;
    const Sheet &sh = wb.sheets[static_cast<size_t>(sheet)];
    const int rows = sh.max_row + 1;
    const int cols = sh.max_col + 1;
    const std::vector<int> target = BuildApplyTarget(view, rows, nullptr);
    for (size_t i = 0; i < target.size(); i++) {
        // A row that is not moving cannot be broken by the move.
        if (target[i] == static_cast<int>(i)) continue;
        for (int c = 0; c < cols; c++) {
            const Cell *cell = sh.FindCell(target[i], c);
            if (cell && cell->kind == CellKind::Formula) return true;
        }
    }
    return false;
}

SheetApplyResult SheetApplyOrderToData(Workbook &wb, int sheet, SheetView &view) {
    SheetApplyResult res;
    if (sheet < 0 || static_cast<size_t>(sheet) >= wb.sheets.size()) return res;
    const int rows = wb.sheets[static_cast<size_t>(sheet)].max_row + 1;
    const int cols = wb.sheets[static_cast<size_t>(sheet)].max_col + 1;
    if (rows <= 0 || cols <= 0) return res;

    // Visible rows in display order, then the rows a filter is hiding, in
    // their original relative order. Appending rather than dropping them
    // is the difference between "apply this view" and "delete the data I
    // filtered out" -- the second is never what a sort-and-save is asking
    // for, and this operation is otherwise reversible only through undo.
    const std::vector<int> target = BuildApplyTarget(view, rows, &res.hidden_rows);
    for (size_t i = 0; i < target.size(); i++) {
        if (target[i] != static_cast<int>(i)) res.moved_rows++;
    }
    if (res.moved_rows == 0) return res;
    res.moved_formula = SheetApplyWouldMoveFormula(wb, sheet, view);

    // Snapshot every raw first: writing row by row in place would
    // overwrite source rows that later target positions still need.
    std::vector<std::vector<std::string>> snapshot(target.size());
    for (size_t i = 0; i < target.size(); i++) {
        snapshot[i].resize(static_cast<size_t>(cols));
        const Sheet &sh = wb.sheets[static_cast<size_t>(sheet)];
        for (int c = 0; c < cols; c++) {
            const Cell *cell = sh.FindCell(target[i], c);
            if (cell) snapshot[i][static_cast<size_t>(c)] = cell->raw;
        }
    }
    for (size_t i = 0; i < snapshot.size(); i++) {
        for (int c = 0; c < cols; c++) {
            SetCellRaw(wb, sheet, static_cast<int>(i), c, snapshot[i][static_cast<size_t>(c)]);
        }
    }

    // Row heights are keyed by sheet row, so they have to travel with the
    // data they were set for -- otherwise a row the user made tall to
    // read a long value leaves its height behind on whatever sorts into
    // its old position.
    if (!view.row_h.empty()) {
        std::unordered_map<int, int> moved;
        for (size_t i = 0; i < target.size(); i++) {
            auto it = view.row_h.find(target[i]);
            if (it != view.row_h.end()) moved[static_cast<int>(i)] = it->second;
        }
        view.row_h = std::move(moved);
    }

    // The data now carries the order, so the view goes back to the
    // identity. Filters stay: the rows they hide are simply at the bottom
    // now, which is exactly what the user is still looking at.
    view.sort = SheetSort{};
    MarkSheetOrderDirty(view);
    res.changed = true;
    return res;
}

bool ParseSheetColumnArg(const std::string &text, int *out) {
    size_t a = text.find_first_not_of(" \t");
    if (a == std::string::npos) return false;
    size_t z = text.find_last_not_of(" \t");
    const std::string arg = text.substr(a, z - a + 1);
    bool all_alpha = true, all_digit = true;
    for (char ch : arg) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (!std::isalpha(u)) all_alpha = false;
        if (!std::isdigit(u)) all_digit = false;
    }
    if (all_alpha) {
        // Reuses the formula parser's own A1-notation column mapping
        // (formula.h), so ":MepSheetSort AB" and "=SUM(AB1:AB9)" can
        // never disagree about which column AB is -- upper-cased first,
        // because that function only accepts 'A'..'Z' (it parses formula
        // text, which is canonicalized by the time it gets there) and a
        // command line is typed by hand.
        std::string upper = arg;
        for (char &ch : upper) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        const int col = ColumnLettersToIndex(upper);
        if (col < 0) return false;
        if (out) *out = col;
        return true;
    }
    if (all_digit) {
        const long v = std::strtol(arg.c_str(), nullptr, 10);
        if (v < 1) return false;
        if (out) *out = static_cast<int>(v - 1);
        return true;
    }
    return false;
}

bool ParseSheetFilterOp(const std::string &text, SheetFilterOp *out) {
    SheetFilterOp op = SheetFilterOp::Contains;
    if (text == "=" || text == "==") {
        op = SheetFilterOp::Equals;
    } else if (text == "!=" || text == "<>") {
        op = SheetFilterOp::NotEquals;
    } else if (text == "~") {
        op = SheetFilterOp::Contains;
    } else if (text == "!~") {
        op = SheetFilterOp::NotContains;
    } else if (text == ">") {
        op = SheetFilterOp::Greater;
    } else if (text == "<") {
        op = SheetFilterOp::Less;
    } else if (text == "empty") {
        op = SheetFilterOp::Empty;
    } else if (text == "nonempty") {
        op = SheetFilterOp::NonEmpty;
    } else if (text == "re") {
        op = SheetFilterOp::Regex;
    } else {
        return false;
    }
    if (out) *out = op;
    return true;
}

std::string SheetViewSummary(const SheetView &view) {
    std::string out;
    if (view.sort.col >= 0 && view.sort.kind != SheetSortKind::None) {
        out = "sort " + ColumnIndexToLetters(view.sort.col) + (view.sort.descending ? " desc" : " asc");
    }
    if (!view.filters.empty()) {
        if (!out.empty()) out += ", ";
        out += std::to_string(view.filters.size());
        out += view.filters.size() == 1 ? " filter" : " filters";
    }
    return out;
}
