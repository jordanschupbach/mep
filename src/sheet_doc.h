#ifndef MEP_SHEET_DOC_H
#define MEP_SHEET_DOC_H

#include "formula.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>


// Deliberately raylib-free (same reasoning as office_doc.h/pdf_doc.h): the
// spreadsheet model, formula engine, and file parsing are pure CPU-side
// data structures, usable/testable without a GL context. main.cpp is the
// only place that turns a Sheet's cells into a rendered grid.
//
// A hand-rolled, intentionally partial spreadsheet model for .xlsx/.ods/
// .csv -- not a general-purpose spreadsheet library. No array formulas,
// named ranges, pivot tables/charts, conditional formatting, cell
// formatting/styles beyond plain numeric display, merged cells, frozen
// panes, iterative circular-calculation modes, volatile functions,
// external-workbook references, dates/date arithmetic, or adding/
// removing sheets/rows/columns. Legacy binary .xls is out of scope
// entirely. See NVIM_PARITY_PLAN.md's spreadsheet-pane phase for the
// full exclusion list and rationale.

enum class CellKind { Empty, Number, Text, Bool, Formula, Error };
enum class SheetError { None, DivZero, Ref, Name, Value, NA, Circular, Num };

// A formula's (or a literal's) resolved value -- what a cell actually
// displays/participates in arithmetic as, independent of the raw text
// that produced it.
struct CellValue {
    CellKind kind = CellKind::Empty;
    double number = 0;
    std::string text;
    bool boolean = false;
    SheetError error = SheetError::None;
};

struct Cell {
    // Exactly what the user typed or the file stored: "42", "hello",
    // "=SUM(A1:A10)". The single source of truth -- `kind`/`ast`/`cached`
    // below are all derived from this and never edited independently of
    // it (see SetCellRaw).
    std::string raw;
    CellKind kind = CellKind::Empty;  // parsed from `raw` (Formula if raw[0] == '=')
    CellValue cached;                 // last-evaluated result; only meaningful if computed_generation is current

    // shared_ptr, not unique_ptr -- deliberately, so Cell (and therefore
    // Sheet/Workbook) stays copy-constructible, which SheetSession's
    // undo/redo full-snapshot convention (editor.h) depends on; the AST
    // is immutable after parsing, so sharing it across a snapshot copy is
    // safe and cheap (no deep copy needed). Only set if kind == Formula.
    std::shared_ptr<const FormulaNode> ast;

    // Freshness stamp against Workbook::recalc_generation -- see
    // EvaluateCell's own comment for the memoization/invalidation model.
    int computed_generation = -1;
    // Cycle-detection flag, set/cleared during EvaluateCell(); a
    // reference to a cell already `evaluating` resolves to
    // SheetError::Circular instead of infinite-recursing.
    bool evaluating = false;
};

struct Sheet {
    std::string name;

    // High-water marks for rendering/navigation bounds (scrollbar extent,
    // gg/G-equivalent grid-corner jumps) -- NOT a dense allocation size.
    // Updated ONLY when a cell is given real content (on file parse, or
    // on SetCellRaw), never as a side effect of a formula/range
    // evaluation merely reading a cell (a SUM(A1:A1048576) must not
    // inflate max_row to over a million). Only ever grow, never shrink on
    // a cell being cleared -- matches Excel/Calc's own "used range"
    // behavior.
    int max_row = 0, max_col = 0;

    // Read-only lookup -- nullptr if the cell has never been given
    // content. Deliberately does NOT insert on a miss (unlike
    // unordered_map::operator[]) -- range-consuming formula functions
    // (SUM/VLOOKUP/etc.) rely on this to stay sparse even when iterating
    // a million-row range; see EvaluateCell's own comment.
    const Cell *FindCell(int row, int col) const;
    // Inserts an empty Cell if absent (bumping max_row/max_col to cover
    // it) and returns a reference -- the only way `cells` should ever
    // grow. Used by SetCellRaw and by file loaders, never by formula
    // evaluation.
    Cell &GetOrCreateCell(int row, int col);

private:
    std::unordered_map<uint64_t, Cell> cells_;  // sparse; key packs (row,col), see sheet_doc.cpp
};

struct Workbook {
    std::vector<Sheet> sheets;
    std::string source_format;  // "xlsx" | "ods" | "csv"
    // Bumped once per edit commit (any cell's raw changing) -- see
    // EvaluateCell's own comment for how this drives cache invalidation
    // without a dependency graph.
    int recalc_generation = 0;

    // xlsx-only: sheets[i]'s zip entry path ("xl/worksheets/sheet1.xml"),
    // parallel to `sheets`, populated by LoadXlsxFromMemory and consumed
    // by SaveXlsxToMemory to know which zip entry to rewrite for each
    // sheet -- v1 never adds/removes sheets, so this 1:1 parallel index
    // relationship is preserved for the Workbook's whole lifetime. Empty
    // for ods/csv workbooks (ODS keeps every sheet in one content.xml;
    // CSV is always single-sheet with no container to speak of).
    std::vector<std::string> xlsx_sheet_paths;
};

// Sets `raw` on the cell at (sheet,row,col), reparsing it into
// kind/ast (a literal number/text/bool, or a Formula if raw starts with
// '='), and bumps wb.recalc_generation so every formula cell in the
// workbook recomputes on its next EvaluateCell call. The one mutation
// entry point every editor action and file-format loader goes through --
// never poke a Cell's raw/kind/ast fields directly.
void SetCellRaw(Workbook &wb, int sheet, int row, int col, const std::string &raw);

// The single mandatory evaluation choke point -- every reference of any
// kind (same-sheet CellRef, Range iteration, Sheet2!A1 cross-sheet refs,
// and lookups inside VLOOKUP/INDEX/MATCH) MUST route through this
// function and never read Cell::cached directly. An unqualified CellRef
// inside a formula resolves against *that formula's own sheet* -- the
// `sheet` parameter here, threaded through the evaluator recursively, not
// whatever sheet the top-level caller started from. This discipline is
// what makes cross-sheet cycle detection (A on Sheet1 -> B on Sheet2 ->
// A on Sheet1) correctly caught by the same per-cell `evaluating` flag as
// a same-sheet cycle, with no separate global dependency graph needed.
//
// Freshness uses Workbook::recalc_generation, not a dependency graph:
// returns cell.cached immediately if cell.computed_generation matches;
// otherwise recomputes (marking `evaluating` during the walk, so a
// circular reference resolves to SheetError::Circular instead of
// infinite recursion), stamps computed_generation, and returns the fresh
// value. This recomputes every cell actually touched by an edit, not
// just the edited cell, without maintaining a dependency graph -- a
// cell's cached value is only provably fresh relative to the whole
// workbook's last edit, not that specific cell's real dependencies,
// which is an accepted v1 simplification for realistic sheet sizes.
CellValue EvaluateCell(Workbook &wb, int sheet, int row, int col);

// Formats a CellValue for display (the grid renderer) and for CSV save
// (which has no separate "formula vs. value" representation) -- trimmed
// general-format numeric text (no trailing zeros), "TRUE"/"FALSE" for
// booleans, the raw text for Text, and a canonical "#DIV/0!"-style token
// for each SheetError.
std::string FormatCellValue(const CellValue &v);

bool IsCsvPath(const std::string &path);
bool IsXlsxPath(const std::string &path);
bool IsOdsPath(const std::string &path);

// ============================================================================
// File I/O -- one pair of functions per format. Load* is tolerant the
// same way LoadDocxFromMemory is: a malformed row/cell is skipped, not
// fatal, and only a fully unreadable container/missing required part
// fails the whole load. Save* takes `Workbook&` (not const) because it
// must call EvaluateCell to get each formula cell's current value to
// write out.
// ============================================================================

bool LoadCsvFromMemory(const unsigned char *bytes, size_t len, Workbook &out, std::string &error);
bool SaveCsvToMemory(Workbook &wb, std::string &out, std::string &error);

bool LoadXlsxFromMemory(const unsigned char *bytes, size_t len, Workbook &out, std::string &error);
bool SaveXlsxToMemory(Workbook &wb, const std::vector<unsigned char> &original_bytes, std::vector<unsigned char> &out,
                       std::string &error);

bool LoadOdsFromMemory(const unsigned char *bytes, size_t len, Workbook &out, std::string &error);
bool SaveOdsToMemory(Workbook &wb, const std::vector<unsigned char> &original_bytes, std::vector<unsigned char> &out,
                      std::string &error);

#endif
