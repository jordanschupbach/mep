#include "sheet_doc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace {

uint64_t CellKey(int row, int col) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(row)) << 32) | static_cast<uint32_t>(col);
}

std::string LowerExt(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}

}  // namespace

const Cell *Sheet::FindCell(int row, int col) const {
    auto it = cells_.find(CellKey(row, col));
    if (it == cells_.end()) return nullptr;
    return &it->second;
}

Cell &Sheet::GetOrCreateCell(int row, int col) {
    max_row = std::max(max_row, row);
    max_col = std::max(max_col, col);
    return cells_[CellKey(row, col)];
}

bool IsCsvPath(const std::string &path) { return LowerExt(path) == "csv"; }
bool IsXlsxPath(const std::string &path) { return LowerExt(path) == "xlsx"; }
bool IsOdsPath(const std::string &path) { return LowerExt(path) == "ods"; }

// ============================================================================
// Value helpers -- shared by the evaluator and by SetCellRaw's literal
// parsing.
// ============================================================================

namespace {

std::string FormatNumber(double n) {
    if (std::isnan(n)) return "NaN";
    if (std::isinf(n)) return n < 0 ? "-Inf" : "Inf";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", n);
    return std::string(buf);
}

bool IsErrorValue(const CellValue &v) { return v.kind == CellKind::Error; }
CellValue MakeErrorValue(SheetError e) {
    CellValue v;
    v.kind = CellKind::Error;
    v.error = e;
    return v;
}
CellValue MakeNumberValue(double n) {
    CellValue v;
    v.kind = CellKind::Number;
    v.number = n;
    return v;
}
CellValue MakeTextValue(const std::string &s) {
    CellValue v;
    v.kind = CellKind::Text;
    v.text = s;
    return v;
}
CellValue MakeBoolValue(bool b) {
    CellValue v;
    v.kind = CellKind::Bool;
    v.boolean = b;
    return v;
}

double ToNumber(const CellValue &v) {
    switch (v.kind) {
        case CellKind::Number: return v.number;
        case CellKind::Bool: return v.boolean ? 1.0 : 0.0;
        case CellKind::Text: {
            if (v.text.empty()) return 0.0;
            const char *start = v.text.c_str();
            char *end = nullptr;
            double n = std::strtod(start, &end);
            return (end == start + v.text.size() && end != start) ? n : 0.0;
        }
        default: return 0.0;
    }
}

std::string ToText(const CellValue &v) {
    switch (v.kind) {
        case CellKind::Text: return v.text;
        case CellKind::Number: return FormatNumber(v.number);
        case CellKind::Bool: return v.boolean ? "TRUE" : "FALSE";
        default: return "";
    }
}

bool ToBoolValue(const CellValue &v) {
    switch (v.kind) {
        case CellKind::Bool: return v.boolean;
        case CellKind::Number: return v.number != 0.0;
        case CellKind::Text: {
            std::string u = v.text;
            std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return std::toupper(c); });
            return u == "TRUE";
        }
        default: return false;
    }
}

// Case-insensitive text compare (matches Excel's own default lookup
// behavior), numeric compare for numbers, exact for booleans; anything
// else (mixed kinds not covered below) is simply unequal.
bool ValuesEqual(const CellValue &a, const CellValue &b) {
    if (a.kind == CellKind::Number && b.kind == CellKind::Number) return a.number == b.number;
    if (a.kind == CellKind::Bool && b.kind == CellKind::Bool) return a.boolean == b.boolean;
    if ((a.kind == CellKind::Text || a.kind == CellKind::Number) &&
        (b.kind == CellKind::Text || b.kind == CellKind::Number)) {
        std::string ta = ToText(a), tb = ToText(b);
        std::transform(ta.begin(), ta.end(), ta.begin(), [](unsigned char c) { return std::toupper(c); });
        std::transform(tb.begin(), tb.end(), tb.begin(), [](unsigned char c) { return std::toupper(c); });
        return ta == tb;
    }
    return false;
}

int CompareValues(const CellValue &a, const CellValue &b) {
    if (a.kind == CellKind::Text || b.kind == CellKind::Text) {
        return ToText(a).compare(ToText(b));
    }
    double an = ToNumber(a), bn = ToNumber(b);
    if (an < bn) return -1;
    if (an > bn) return 1;
    return 0;
}

}  // namespace

std::string FormatCellValue(const CellValue &v) {
    switch (v.kind) {
        case CellKind::Empty: return "";
        case CellKind::Number: return FormatNumber(v.number);
        case CellKind::Text: return v.text;
        case CellKind::Bool: return v.boolean ? "TRUE" : "FALSE";
        case CellKind::Formula: return "";
        case CellKind::Error:
            switch (v.error) {
                case SheetError::DivZero: return "#DIV/0!";
                case SheetError::Ref: return "#REF!";
                case SheetError::Name: return "#NAME?";
                case SheetError::Value: return "#VALUE!";
                case SheetError::NA: return "#N/A";
                case SheetError::Circular: return "#CIRCULAR!";
                case SheetError::Num: return "#NUM!";
                default: return "#ERROR!";
            }
    }
    return "";
}

// ============================================================================
// SetCellRaw
// ============================================================================

namespace {

// A literal's Kind/CellValue from its raw text (no '=' prefix -- that's
// Formula, handled by the caller before this is reached). A literal that
// parses cleanly as a number (the WHOLE string, via strtod + checking
// every byte was consumed) is Number; everything else is Text -- no
// literal booleans, "TRUE"/"FALSE" typed directly into a cell is Text in
// v1 (only a formula's TRUE()/FALSE() literal produces an actual Bool).
CellValue LiteralValueFromText(const std::string &text) {
    CellValue v;
    if (text.empty()) return v;  // Empty
    const char *start = text.c_str();
    char *end = nullptr;
    double num = std::strtod(start, &end);
    if (end == start + text.size() && end != start) return MakeNumberValue(num);
    return MakeTextValue(text);
}

}  // namespace

void SetCellRaw(Workbook &wb, int sheet, int row, int col, const std::string &raw) {
    if (sheet < 0 || sheet >= static_cast<int>(wb.sheets.size())) return;
    Cell &cell = wb.sheets[sheet].GetOrCreateCell(row, col);
    cell.raw = raw;
    cell.ast.reset();
    if (!raw.empty() && raw[0] == '=' && raw.size() > 1) {
        std::string err;
        auto ast = ParseFormula(raw.substr(1), err);
        if (ast) {
            cell.kind = CellKind::Formula;
            cell.ast = ast;
            cell.computed_generation = -1;  // forces EvaluateCell to actually compute it next read
        } else {
            // Unparsable formula text -- stored as a literal Error value,
            // same tolerance convention as a malformed file cell: never
            // crash or refuse the edit, just surface it visibly.
            cell.kind = CellKind::Error;
            cell.cached = MakeErrorValue(SheetError::Name);
        }
    } else {
        cell.kind = CellKind::Empty;
        cell.cached = LiteralValueFromText(raw);
        cell.kind = cell.cached.kind;
    }
    wb.recalc_generation++;
}

// ============================================================================
// Evaluator
// ============================================================================

namespace {

CellValue EvalNode(Workbook &wb, int sheet, const FormulaNode &node);
CellValue CallFunction(Workbook &wb, int sheet, const FormulaNode &node);

int ResolveSheetIndex(const Workbook &wb, const std::string &sheet_name, int default_sheet) {
    if (sheet_name.empty()) return default_sheet;
    for (size_t i = 0; i < wb.sheets.size(); i++) {
        if (wb.sheets[i].name == sheet_name) return static_cast<int>(i);
    }
    return -1;
}

CellValue EvalCellRef(Workbook &wb, int sheet, const FormulaCellRef &ref) {
    int target = ResolveSheetIndex(wb, ref.sheet_name, sheet);
    if (target < 0) return MakeErrorValue(SheetError::Ref);
    return EvaluateCell(wb, target, ref.row, ref.col);
}

// Collects every non-empty value in a range (clamped to the target
// sheet's own max_row/max_col), skipping untouched cells entirely --
// used by aggregate functions (SUM/AVERAGE/...) where position within
// the range doesn't matter. See CollectRangeValuesOrdered below for the
// position-preserving variant MATCH needs.
void CollectRangeValues(Workbook &wb, int sheet, const FormulaNode &node, std::vector<CellValue> &out) {
    int target = ResolveSheetIndex(wb, node.cell.sheet_name, sheet);
    if (target < 0) return;
    Sheet &sh = wb.sheets[target];
    int r0 = std::min(node.cell.row, node.range_end.row), r1 = std::max(node.cell.row, node.range_end.row);
    int c0 = std::min(node.cell.col, node.range_end.col), c1 = std::max(node.cell.col, node.range_end.col);
    r1 = std::min(r1, sh.max_row);
    c1 = std::min(c1, sh.max_col);
    for (int r = r0; r <= r1; r++) {
        for (int c = c0; c <= c1; c++) {
            if (!sh.FindCell(r, c)) continue;  // sparse -- never insert on a read
            out.push_back(EvaluateCell(wb, target, r, c));
        }
    }
}

// Position-preserving variant for MATCH: every logical position in the
// (clamped) range gets an entry -- an Empty placeholder for an untouched
// cell, not skipped -- so the returned index still means "Nth cell in
// the range" the way INDEX/MATCH's row-offset arithmetic expects. Single-
// column or single-row ranges only (v1 scope -- a 2D range's "position"
// for MATCH is otherwise ambiguous); iterates row-major.
void CollectRangeValuesOrdered(Workbook &wb, int sheet, const FormulaNode &node, std::vector<CellValue> &out) {
    int target = ResolveSheetIndex(wb, node.cell.sheet_name, sheet);
    if (target < 0) return;
    Sheet &sh = wb.sheets[target];
    int r0 = std::min(node.cell.row, node.range_end.row), r1 = std::max(node.cell.row, node.range_end.row);
    int c0 = std::min(node.cell.col, node.range_end.col), c1 = std::max(node.cell.col, node.range_end.col);
    r1 = std::min(r1, sh.max_row);
    c1 = std::min(c1, sh.max_col);
    for (int r = r0; r <= r1; r++) {
        for (int c = c0; c <= c1; c++) {
            const Cell *cell = sh.FindCell(r, c);
            if (!cell) {
                out.push_back(CellValue{});
                continue;
            }
            out.push_back(EvaluateCell(wb, target, r, c));
        }
    }
}

// A single argument, expanded to every value it denotes: a Range expands
// to CollectRangeValues (aggregate, order-agnostic); anything else
// evaluates to exactly one value.
void CollectArgValues(Workbook &wb, int sheet, const FormulaNode &arg, std::vector<CellValue> &out) {
    if (arg.kind == FormulaNodeKind::Range) {
        CollectRangeValues(wb, sheet, arg, out);
    } else {
        out.push_back(EvalNode(wb, sheet, arg));
    }
}

CellValue CallFunction(Workbook &wb, int sheet, const FormulaNode &node) {
    const std::string &name = node.text;
    const auto &args = node.args;
    auto arg_value = [&](size_t i) { return EvalNode(wb, sheet, *args[i]); };

    if (name == "SUM" || name == "AVERAGE" || name == "COUNT" || name == "COUNTA" || name == "MIN" ||
        name == "MAX") {
        double total = 0, best = 0;
        int count = 0, counta = 0;
        bool has_best = false;
        for (const auto &a : args) {
            std::vector<CellValue> vals;
            CollectArgValues(wb, sheet, *a, vals);
            for (const CellValue &v : vals) {
                if (IsErrorValue(v)) return v;
                if (v.kind != CellKind::Empty) counta++;
                if (v.kind != CellKind::Number) continue;
                total += v.number;
                count++;
                if (!has_best || (name == "MIN" ? v.number < best : v.number > best)) {
                    best = v.number;
                    has_best = true;
                }
            }
        }
        if (name == "SUM") return MakeNumberValue(total);
        if (name == "COUNT") return MakeNumberValue(count);
        if (name == "COUNTA") return MakeNumberValue(counta);
        if (name == "AVERAGE") return count == 0 ? MakeErrorValue(SheetError::DivZero) : MakeNumberValue(total / count);
        return MakeNumberValue(has_best ? best : 0.0);  // MIN/MAX of nothing -- 0, not an error
    }
    if (name == "IF") {
        if (args.size() < 2) return MakeErrorValue(SheetError::Value);
        CellValue cond = arg_value(0);
        if (IsErrorValue(cond)) return cond;
        if (ToBoolValue(cond)) return arg_value(1);
        return args.size() >= 3 ? arg_value(2) : MakeBoolValue(false);
    }
    if (name == "AND" || name == "OR") {
        bool result = (name == "AND");
        for (const auto &a : args) {
            std::vector<CellValue> vals;
            CollectArgValues(wb, sheet, *a, vals);
            for (const CellValue &v : vals) {
                if (IsErrorValue(v)) return v;
                result = (name == "AND") ? (result && ToBoolValue(v)) : (result || ToBoolValue(v));
            }
        }
        return MakeBoolValue(result);
    }
    if (name == "NOT") {
        if (args.empty()) return MakeErrorValue(SheetError::Value);
        CellValue v = arg_value(0);
        return IsErrorValue(v) ? v : MakeBoolValue(!ToBoolValue(v));
    }
    if (name == "CONCAT" || name == "CONCATENATE") {
        std::string out;
        for (const auto &a : args) {
            std::vector<CellValue> vals;
            CollectArgValues(wb, sheet, *a, vals);
            for (const CellValue &v : vals) {
                if (IsErrorValue(v)) return v;
                out += ToText(v);
            }
        }
        return MakeTextValue(out);
    }
    if (name == "LEN") {
        if (args.empty()) return MakeErrorValue(SheetError::Value);
        CellValue v = arg_value(0);
        return IsErrorValue(v) ? v : MakeNumberValue(static_cast<double>(ToText(v).size()));
    }
    if (name == "LEFT" || name == "RIGHT") {
        if (args.empty()) return MakeErrorValue(SheetError::Value);
        CellValue v = arg_value(0);
        if (IsErrorValue(v)) return v;
        std::string s = ToText(v);
        int n = args.size() > 1 ? static_cast<int>(ToNumber(arg_value(1))) : 1;
        n = std::max(0, std::min(n, static_cast<int>(s.size())));
        return MakeTextValue(name == "LEFT" ? s.substr(0, n) : s.substr(s.size() - n));
    }
    if (name == "MID") {
        if (args.size() < 3) return MakeErrorValue(SheetError::Value);
        CellValue v = arg_value(0);
        if (IsErrorValue(v)) return v;
        std::string s = ToText(v);
        int start = static_cast<int>(ToNumber(arg_value(1))) - 1;
        int count = static_cast<int>(ToNumber(arg_value(2)));
        if (start < 0 || start >= static_cast<int>(s.size()) || count <= 0) return MakeTextValue("");
        count = std::min(count, static_cast<int>(s.size()) - start);
        return MakeTextValue(s.substr(start, count));
    }
    if (name == "UPPER" || name == "LOWER") {
        if (args.empty()) return MakeErrorValue(SheetError::Value);
        CellValue v = arg_value(0);
        if (IsErrorValue(v)) return v;
        std::string s = ToText(v);
        std::transform(s.begin(), s.end(), s.begin(),
                        [&](unsigned char c) { return name == "UPPER" ? std::toupper(c) : std::tolower(c); });
        return MakeTextValue(s);
    }
    if (name == "TRIM") {
        if (args.empty()) return MakeErrorValue(SheetError::Value);
        CellValue v = arg_value(0);
        if (IsErrorValue(v)) return v;
        std::string s = ToText(v);
        size_t a = s.find_first_not_of(' ');
        if (a == std::string::npos) return MakeTextValue("");
        size_t b = s.find_last_not_of(' ');
        return MakeTextValue(s.substr(a, b - a + 1));
    }
    if (name == "ROUND") {
        if (args.empty()) return MakeErrorValue(SheetError::Value);
        CellValue v = arg_value(0);
        if (IsErrorValue(v)) return v;
        int digits = args.size() > 1 ? static_cast<int>(ToNumber(arg_value(1))) : 0;
        double mult = std::pow(10.0, digits);
        return MakeNumberValue(std::round(ToNumber(v) * mult) / mult);
    }
    if (name == "ABS") {
        if (args.empty()) return MakeErrorValue(SheetError::Value);
        CellValue v = arg_value(0);
        return IsErrorValue(v) ? v : MakeNumberValue(std::fabs(ToNumber(v)));
    }
    if (name == "INDEX") {
        if (args.empty() || args[0]->kind != FormulaNodeKind::Range) return MakeErrorValue(SheetError::Ref);
        const FormulaNode &range = *args[0];
        int row_n = args.size() > 1 ? static_cast<int>(ToNumber(arg_value(1))) : 1;
        int col_n = args.size() > 2 ? static_cast<int>(ToNumber(arg_value(2))) : 1;
        int target = ResolveSheetIndex(wb, range.cell.sheet_name, sheet);
        if (target < 0) return MakeErrorValue(SheetError::Ref);
        int r0 = std::min(range.cell.row, range.range_end.row), r1 = std::max(range.cell.row, range.range_end.row);
        int c0 = std::min(range.cell.col, range.range_end.col), c1 = std::max(range.cell.col, range.range_end.col);
        int row = r0 + row_n - 1, col = c0 + col_n - 1;
        if (row < r0 || row > r1 || col < c0 || col > c1) return MakeErrorValue(SheetError::Ref);
        return EvaluateCell(wb, target, row, col);
    }
    if (name == "MATCH") {
        if (args.size() < 2 || args[1]->kind != FormulaNodeKind::Range) return MakeErrorValue(SheetError::NA);
        CellValue lookup = arg_value(0);
        if (IsErrorValue(lookup)) return lookup;
        std::vector<CellValue> vals;
        CollectRangeValuesOrdered(wb, sheet, *args[1], vals);
        // v1 simplification: always an exact match (Excel's match_type
        // 0), no 1/-1 approximate-match modes -- the 3rd MATCH argument,
        // if present, is accepted syntactically but ignored.
        for (size_t i = 0; i < vals.size(); i++) {
            if (ValuesEqual(vals[i], lookup)) return MakeNumberValue(static_cast<double>(i + 1));
        }
        return MakeErrorValue(SheetError::NA);
    }
    if (name == "VLOOKUP") {
        if (args.size() < 3 || args[1]->kind != FormulaNodeKind::Range) return MakeErrorValue(SheetError::NA);
        CellValue lookup = arg_value(0);
        if (IsErrorValue(lookup)) return lookup;
        const FormulaNode &range = *args[1];
        int col_index = static_cast<int>(ToNumber(arg_value(2)));
        int target = ResolveSheetIndex(wb, range.cell.sheet_name, sheet);
        if (target < 0) return MakeErrorValue(SheetError::Ref);
        Sheet &sh = wb.sheets[target];
        int r0 = std::min(range.cell.row, range.range_end.row);
        int r1 = std::min(std::max(range.cell.row, range.range_end.row), sh.max_row);
        int c0 = std::min(range.cell.col, range.range_end.col);
        for (int r = r0; r <= r1; r++) {
            CellValue key = EvaluateCell(wb, target, r, c0);
            if (ValuesEqual(key, lookup)) return EvaluateCell(wb, target, r, c0 + col_index - 1);
        }
        return MakeErrorValue(SheetError::NA);
    }
    return MakeErrorValue(SheetError::Name);  // unknown function
}

CellValue EvalNode(Workbook &wb, int sheet, const FormulaNode &node) {
    switch (node.kind) {
        case FormulaNodeKind::Number: return MakeNumberValue(node.number);
        case FormulaNodeKind::String: return MakeTextValue(node.text);
        case FormulaNodeKind::Bool: return MakeBoolValue(node.boolean);
        case FormulaNodeKind::CellRef: return EvalCellRef(wb, sheet, node.cell);
        case FormulaNodeKind::Range:
            // A bare range where a single value is expected (e.g. "=A1:A5"
            // typed directly, or passed to a function with no special
            // Range handling) resolves to its top-left cell -- close
            // enough to Excel's own "implicit intersection" fallback for
            // v1 without real intersect-with-the-current-row logic.
            return EvalCellRef(wb, sheet, node.cell);
        case FormulaNodeKind::UnaryOp: {
            CellValue v = EvalNode(wb, sheet, *node.lhs);
            if (IsErrorValue(v)) return v;
            return node.op == FormulaOp::Neg ? MakeNumberValue(-ToNumber(v)) : MakeErrorValue(SheetError::Value);
        }
        case FormulaNodeKind::BinaryOp: {
            CellValue l = EvalNode(wb, sheet, *node.lhs);
            if (IsErrorValue(l)) return l;
            CellValue r = EvalNode(wb, sheet, *node.rhs);
            if (IsErrorValue(r)) return r;
            switch (node.op) {
                case FormulaOp::Add: return MakeNumberValue(ToNumber(l) + ToNumber(r));
                case FormulaOp::Sub: return MakeNumberValue(ToNumber(l) - ToNumber(r));
                case FormulaOp::Mul: return MakeNumberValue(ToNumber(l) * ToNumber(r));
                case FormulaOp::Div: {
                    double rn = ToNumber(r);
                    return rn == 0.0 ? MakeErrorValue(SheetError::DivZero) : MakeNumberValue(ToNumber(l) / rn);
                }
                case FormulaOp::Pow: return MakeNumberValue(std::pow(ToNumber(l), ToNumber(r)));
                case FormulaOp::Concat: return MakeTextValue(ToText(l) + ToText(r));
                case FormulaOp::Eq: return MakeBoolValue(ValuesEqual(l, r));
                case FormulaOp::Ne: return MakeBoolValue(!ValuesEqual(l, r));
                case FormulaOp::Lt: return MakeBoolValue(CompareValues(l, r) < 0);
                case FormulaOp::Gt: return MakeBoolValue(CompareValues(l, r) > 0);
                case FormulaOp::Le: return MakeBoolValue(CompareValues(l, r) <= 0);
                case FormulaOp::Ge: return MakeBoolValue(CompareValues(l, r) >= 0);
                default: return MakeErrorValue(SheetError::Value);
            }
        }
        case FormulaNodeKind::FunctionCall: return CallFunction(wb, sheet, node);
    }
    return MakeErrorValue(SheetError::Value);
}

}  // namespace

CellValue EvaluateCell(Workbook &wb, int sheet, int row, int col) {
    if (sheet < 0 || sheet >= static_cast<int>(wb.sheets.size())) return MakeErrorValue(SheetError::Ref);
    Sheet &sh = wb.sheets[sheet];
    // FindCell (const) then const_cast, rather than a public mutable
    // finder: EvaluateCell is the one place in this file that needs
    // write access to an *existing* cell's cache without ever inserting
    // one that isn't there (a truly untouched cell short-circuits to
    // Empty below, no map mutation at all) -- see Sheet::FindCell's own
    // comment on why range-consuming functions depend on that guarantee.
    const Cell *const_cell = sh.FindCell(row, col);
    if (!const_cell) return CellValue{};  // never touched -- nothing to compute or cache
    Cell *cell = const_cast<Cell *>(const_cell);
    if (cell->kind != CellKind::Formula) return cell->cached;  // literal: always fresh, no eval needed
    if (cell->computed_generation == wb.recalc_generation) return cell->cached;
    if (cell->evaluating) {
        // Does NOT stamp computed_generation -- a circular ref shouldn't
        // get permanently cached as circular; the edit that eventually
        // breaks the cycle already bumps recalc_generation on its own.
        return MakeErrorValue(SheetError::Circular);
    }
    cell->evaluating = true;
    CellValue result = EvalNode(wb, sheet, *cell->ast);
    cell->evaluating = false;
    cell->cached = result;
    cell->computed_generation = wb.recalc_generation;
    return result;
}

// ============================================================================
// CSV
// ============================================================================

bool LoadCsvFromMemory(const unsigned char *bytes, size_t len, Workbook &out, std::string &error) {
    (void)error;  // CSV parsing can't structurally fail -- any byte sequence is a valid (if odd) CSV
    out.sheets.clear();
    out.source_format = "csv";
    out.sheets.push_back(Sheet{});
    out.sheets[0].name = "Sheet1";

    std::string text(reinterpret_cast<const char *>(bytes), len);
    size_t i = 0, n = text.size();
    int row = 0, col = 0;
    std::string field;
    bool in_quotes = false;

    auto commit_field = [&]() {
        if (!field.empty()) SetCellRaw(out, 0, row, col, field);
        col++;
        field.clear();
    };
    auto commit_row = [&]() {
        commit_field();
        row++;
        col = 0;
    };

    while (i < n) {
        char c = text[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < n && text[i + 1] == '"') {
                    field.push_back('"');
                    i += 2;
                    continue;
                }
                in_quotes = false;
                i++;
                continue;
            }
            field.push_back(c);
            i++;
            continue;
        }
        if (c == '"' && field.empty()) {
            in_quotes = true;
            i++;
            continue;
        }
        if (c == ',') {
            commit_field();
            i++;
            continue;
        }
        if (c == '\r') {
            i++;
            continue;
        }
        if (c == '\n') {
            commit_row();
            i++;
            continue;
        }
        field.push_back(c);
        i++;
    }
    if (!field.empty() || col > 0) commit_row();  // trailing row with no final newline
    return true;
}

bool SaveCsvToMemory(Workbook &wb, std::string &out, std::string &error) {
    if (wb.sheets.empty()) {
        error = "workbook has no sheets";
        return false;
    }
    Sheet &sh = wb.sheets[0];  // CSV is single-sheet by construction
    auto escape = [](const std::string &s) -> std::string {
        if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
        std::string q = "\"";
        for (char c : s) {
            if (c == '"') q += "\"\"";
            else q.push_back(c);
        }
        q += "\"";
        return q;
    };
    std::ostringstream ss;
    for (int r = 0; r <= sh.max_row; r++) {
        for (int c = 0; c <= sh.max_col; c++) {
            if (c > 0) ss << ",";
            if (sh.FindCell(r, c)) ss << escape(FormatCellValue(EvaluateCell(wb, 0, r, c)));
        }
        ss << "\r\n";
    }
    out = ss.str();
    return true;
}

// ============================================================================
// XLSX / ODS -- stubs for Phase 1 (document model + formula engine + CSV
// + Normal-mode navigation only); real implementations land in Phase 2
// (XLSX read) / Phase 3 (ODS read) / Phase 5 (save-back for both), per
// NVIM_PARITY_PLAN.md's spreadsheet-pane phase. Declared now (not left
// unlinked) so OpenSheetInPlace's IsXlsxPath/IsOdsPath branches, already
// wired up in editor.cpp, fail with a clear status-line message instead
// of a link error.
// ============================================================================

bool LoadXlsxFromMemory(const unsigned char *bytes, size_t len, Workbook &out, std::string &error) {
    (void)bytes;
    (void)len;
    (void)out;
    error = "reading .xlsx isn't implemented yet";
    return false;
}

bool SaveXlsxToMemory(Workbook &wb, const std::vector<unsigned char> &original_bytes, std::vector<unsigned char> &out,
                       std::string &error) {
    (void)wb;
    (void)original_bytes;
    (void)out;
    error = "saving .xlsx isn't implemented yet";
    return false;
}

bool LoadOdsFromMemory(const unsigned char *bytes, size_t len, Workbook &out, std::string &error) {
    (void)bytes;
    (void)len;
    (void)out;
    error = "reading .ods isn't implemented yet";
    return false;
}

bool SaveOdsToMemory(Workbook &wb, const std::vector<unsigned char> &original_bytes, std::vector<unsigned char> &out,
                      std::string &error) {
    (void)wb;
    (void)original_bytes;
    (void)out;
    error = "saving .ods isn't implemented yet";
    return false;
}
