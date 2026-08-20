#ifndef MEP_FORMULA_H
#define MEP_FORMULA_H

#include <memory>
#include <string>
#include <vector>

// Pure syntax: tokenizes and parses a spreadsheet formula string into an
// AST. Deliberately has no notion of a Workbook/Sheet/Cell -- evaluation
// (which does need those) lives in sheet_doc.h/.cpp, which includes this
// header and walks the AST it hands back. Kept separate so the grammar/
// parser is testable in isolation and so this header doesn't need to
// know about half of Workbook just to describe a formula's shape.
//
// Mirrors regex.h/regex.cpp's general shape (recursive-descent, a hand-
// rolled engine with no external dependency, heavy why-focused comments)
// but NOT its encapsulation style: Regex deliberately hides its AST
// (Node/Parser are private, never handed to callers). FormulaNode is
// genuinely public on purpose -- sheet_doc.h's Cell::ast caches and owns
// a parsed formula outside the parser itself (so it's only reparsed when
// a cell's raw text actually changes), unlike anything Regex needs to
// support.

// Deliberate v1 simplification vs. real Excel: unary minus binds LOWER
// than '^' here (standard math convention: "-2^2" parses as "-(2^2)" ==
// -4), whereas Excel's own operator precedence has unary minus bind
// TIGHTER than '^' ("-2^2" == 4 in real Excel). Formulas that rely on
// this specific interaction are rare and this is called out explicitly
// rather than silently diverging.
enum class FormulaOp {
    Add, Sub, Mul, Div, Pow, Concat,
    Eq, Ne, Lt, Gt, Le, Ge,
    Neg,  // unary minus
};

enum class FormulaNodeKind { Number, String, Bool, CellRef, Range, BinaryOp, UnaryOp, FunctionCall };

// A single cell coordinate, 0-indexed. `sheet_name` is set (non-empty)
// only for a sheet-qualified reference ("Sheet2!A1") -- resolving a name
// to a sheet index needs a Workbook, which this header doesn't have, so
// that resolution happens at evaluation time in sheet_doc.cpp. An
// unqualified reference always means "whatever sheet the enclosing
// formula itself lives on", also resolved at evaluation time (threaded
// through as a parameter, never baked into the AST -- see sheet_doc.h's
// EvaluateCell for why this matters for cross-sheet cycle detection).
struct FormulaCellRef {
    std::string sheet_name;
    int row = 0, col = 0;
    // $ prefix -- parsed and preserved but inert in v1 (nothing here
    // fills/copies a formula across cells, the one thing $ vs. relative
    // addressing would otherwise affect).
    bool row_abs = false, col_abs = false;
};

struct FormulaNode {
    FormulaNodeKind kind;

    double number = 0;   // Number
    std::string text;    // String literal, or FunctionCall's function name (upper-cased)
    bool boolean = false;  // Bool

    FormulaCellRef cell;       // CellRef, and Range's start
    FormulaCellRef range_end;  // Range's end

    FormulaOp op = FormulaOp::Add;  // BinaryOp/UnaryOp
    std::shared_ptr<const FormulaNode> lhs, rhs;  // BinaryOp; UnaryOp's operand is `lhs`

    std::vector<std::shared_ptr<const FormulaNode>> args;  // FunctionCall
};

// Parses `text` (WITHOUT a leading '=' -- callers strip it, since some
// callers, e.g. xlsx's <f> element, never had one to begin with) into an
// AST. Returns nullptr and sets `error` on a syntax error; never throws.
// A successful parse is guaranteed to contain no null child nodes
// anywhere in the tree -- the evaluator can walk it without null-checking
// every node.
std::shared_ptr<const FormulaNode> ParseFormula(const std::string &text, std::string &error);

// Column-letter <-> 0-indexed-column helpers ("A" <-> 0, "Z" <-> 25, "AA"
// <-> 26, ...) and a full "A1"-style ref <-> (row,col) pair, shared by
// every format's cell-address parsing (xlsx's r="B12" attribute, ods's
// [.B12] formula syntax, and the grid renderer's own column-letter
// header) so the base-26 arithmetic exists exactly once.
int ColumnLettersToIndex(const std::string &letters);
std::string ColumnIndexToLetters(int col);
// Parses "B12" or "$B$12" -> row=11,col=1 (0-indexed) + the $ flags.
// Returns false if `text` isn't a syntactically valid cell reference.
bool ParseCellAddress(const std::string &text, int &row, int &col, bool &row_abs, bool &col_abs);
std::string CellAddressToString(int row, int col);

#endif
