#include "office_doc.h"
#include "sheet_doc.h"

// ODS read/write -- Phase 3 (read) / part of Phase 4 (save-back) of
// NVIM_PARITY_PLAN.md's spreadsheet-pane phase. Same content.xml-inside-
// a-ZIP shape as office_odt.cpp's ODT support (table:table/table:table-row/
// table:table-cell instead of office:text/text:p), reusing the same
// ReadZipEntry/WriteZipReplacingEntry (office_doc.h) + pugixml pairing.
//
// The one real translation problem ODS has that XLSX doesn't: ODF
// formulas use a different textual syntax entirely (`of:=SUM([.A1:.A10])`,
// `;`-separated arguments, bracketed `[Sheet.Cell]` references) from what
// this engine's own formula.cpp parses. TranslateOdsFormula/
// TranslateOdsBracketRef below convert ODF syntax -> this engine's native
// syntax on load; SerializeFormula(ast, /*ods_style=*/true) (formula.cpp)
// does the reverse on save, so this file never has to re-implement a
// second formula grammar of its own -- it only ever transforms text
// around an actual parsed AST.
//
// Scope matches sheet_doc.h's own documented exclusions -- no merged
// cells, cell styles/number formats, frozen panes, or dates-as-a-real-type
// (a date/time cell's ISO value text loads as a Text literal). A
// non-empty cell inside a `table:number-columns-repeated`/
// `table:number-rows-repeated` run (ODF's compact way to say "this exact
// cell/row repeats N times", almost always used only for *empty* trailing
// padding in real files) is written to the first `min(repeat, 10000)`
// columns/rows and no further -- a real, documented v1 loss for the
// (extremely rare in practice) case of a *non-empty* cell/row genuinely
// marked repeated, guarding against materializing a sparse-map-breaking
// number of cells the way sheet_doc.cpp's own SUM(A1:A1048576) guard does.

#include <cctype>
#include <cstdlib>
#include <sstream>

#include "pugixml.hpp"

namespace {

/**
 * @brief Reports whether a sheet name needs single-quoting to lex correctly in this engine's native formula syntax.
 * @param name Sheet name to check.
 * @return True if name is non-empty and contains a character other than alphanumerics, '_', or '$'.
 */
bool NeedsNativeQuote(const std::string &name) {
    if (name.empty()) return false;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$')) return true;
    }
    return false;
}

// One bracketed ODF reference's inner text (no surrounding `[` `]`) --
// "SheetName.A1" / ".A1" (this sheet) / "$SheetName.$A$1" (ODF's inert
// sheet-absolute `$`) -- translated into this engine's own
// "SheetName!A1" / "A1" syntax. An already-single-quoted sheet name
// ('My Sheet'.A1, from a name containing a space or other special
// character) is valid input to this engine's own lexer verbatim
// (LexQuotedIdent, formula.cpp) and passed through as-is; an unquoted
// name that itself needs quoting to lex correctly natively (same
// characters, just not pre-quoted by this particular producer) gets
// quoted here instead.
/**
 * @brief Translates one bracketed ODF reference's inner text into this engine's native "Sheet!Cell"/"Cell" syntax.
 * @param part ODF reference text with no surrounding brackets, e.g. "SheetName.A1", ".A1", or "$SheetName.$A$1".
 * @return part translated to native syntax, or part unchanged if it has no '.' (malformed).
 */
std::string TranslateOdsRefPart(const std::string &part) {
    size_t dot = part.rfind('.');
    if (dot == std::string::npos) return part;  // malformed -- pass through rather than drop
    std::string sheet = part.substr(0, dot);
    std::string cell = part.substr(dot + 1);
    if (!sheet.empty() && sheet[0] == '$') sheet = sheet.substr(1);
    if (sheet.empty()) return cell;
    if (sheet.front() != '\'' && NeedsNativeQuote(sheet)) sheet = "'" + sheet + "'";
    return sheet + "!" + cell;
}

// A range's end side ("Sheet.A1:.B5" or "Sheet.A1:Sheet.B5") only ever
// needs its bare cell address -- this engine's own range grammar
// (FinishRefOrRange, formula.cpp) always inherits the start ref's sheet
// for the end side, the same convention ODF ranges themselves follow in
// every producer actually observed (the end side's own sheet qualifier,
// when present at all, always matches the start's).
/**
 * @brief Translates one bracketed ODF reference (single cell or range) into this engine's native reference syntax.
 * @param inner ODF reference text with no surrounding brackets, e.g. "Sheet.A1:.B5" or "Sheet.A1:Sheet.B5".
 * @return The translated single-cell reference, or "start:end_cell" for a range (the end side's own sheet qualifier, if any, is dropped since it's always inherited from the start).
 */
std::string TranslateOdsBracketRef(const std::string &inner) {
    size_t colon = inner.find(':');
    if (colon == std::string::npos) return TranslateOdsRefPart(inner);
    std::string start = TranslateOdsRefPart(inner.substr(0, colon));
    std::string end_part = inner.substr(colon + 1);
    size_t end_dot = end_part.rfind('.');
    std::string end_cell = (end_dot == std::string::npos) ? end_part : end_part.substr(end_dot + 1);
    return start + ":" + end_cell;
}

// Strips the "of:=" / "oooc:=" namespace prefix, translates every
// bracketed `[...]` reference, and turns `;` argument separators into
// this engine's `,` -- all in one left-to-right scan that tracks whether
// it's inside a string literal (so a `;` or `[` that happens to appear
// inside a quoted formula string is left untouched). Returns "" (caller
// falls back to the cell's cached value) if `text` has no '=' at all.
/**
 * @brief Converts an ODF formula's raw text ("of:=..."/"oooc:=...") into this engine's native formula syntax.
 * @param text Full formula cell text, including the "of:="/"oooc:=" namespace prefix.
 * @return The translated formula text (no leading '='/prefix), with `;` argument separators replaced by `,` and every bracketed reference translated; "" if text has no '=' at all.
 */
std::string TranslateOdsFormula(const std::string &text) {
    size_t eq = text.find('=');
    if (eq == std::string::npos) return "";
    std::string s = text.substr(eq + 1);

    std::string out;
    bool in_string = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (in_string) {
            out.push_back(c);
            if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
            out.push_back(c);
            continue;
        }
        if (c == ';') {
            out.push_back(',');
            continue;
        }
        if (c == '[') {
            size_t close = s.find(']', i);
            if (close == std::string::npos) break;  // malformed -- stop, return what's translated so far
            out += TranslateOdsBracketRef(s.substr(i + 1, close - i - 1));
            i = close;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// A single <table:table-cell>'s content as this engine's raw cell text
// ("" if genuinely empty). Formula cells always re-derive their value
// live from the translated formula text (never trust the file's own
// cached office:value/text:p for a formula cell -- same "recompute, don't
// trust a stale cache" discipline EvaluateCell itself follows).
/**
 * @brief Extracts a <table:table-cell>'s content as this engine's raw cell text (a formula, a literal value, or paragraph text).
 * @param cell The <table:table-cell> XML node to read.
 * @return The cell's raw text: "=..." if it carries a formula, the literal value/text for a recognized value-type, or the joined <text:p> paragraph text otherwise; "" if genuinely empty.
 */
std::string OdsCellRawText(const pugi::xml_node &cell) {
    if (pugi::xml_attribute formula_attr = cell.attribute("table:formula")) {
        std::string translated = TranslateOdsFormula(formula_attr.as_string());
        if (!translated.empty()) return "=" + translated;
    }

    std::string vtype = cell.attribute("office:value-type").as_string();
    if (vtype == "float" || vtype == "percentage" || vtype == "currency") {
        if (pugi::xml_attribute v = cell.attribute("office:value")) return v.as_string();
    } else if (vtype == "boolean") {
        // See sheet_xlsx.cpp's identical comment on its own "b"-type
        // branch -- SetCellRaw's literal parser never produces a real
        // Bool from bare "TRUE"/"FALSE" text, so this wraps as a trivial
        // formula instead.
        std::string b = cell.attribute("office:boolean-value").as_string();
        return (b == "true") ? "=TRUE" : "=FALSE";
    } else if (vtype == "date") {
        if (pugi::xml_attribute v = cell.attribute("office:date-value")) return v.as_string();
    } else if (vtype == "time") {
        if (pugi::xml_attribute v = cell.attribute("office:time-value")) return v.as_string();
    }

    // "string" value-type, or no recognized value-type at all -- fall
    // back to the cell's own displayed text, joining multiple <text:p>
    // paragraphs (a genuinely multi-line cell) with '\n'.
    std::string text;
    bool first = true;
    for (pugi::xml_node p : cell.children("text:p")) {
        if (!first) text += "\n";
        text += p.text().get();
        first = false;
    }
    return text;
}

/**
 * @brief Populates one sheet's cells from a parsed <table:table>'s rows, expanding repeated (but non-empty) rows/columns.
 * @param table The <table:table> XML node to read.
 * @param wb Workbook whose sheet at sheet_index is populated.
 * @param sheet_index Index of the destination sheet in wb.sheets.
 */
void ParseOdsTable(const pugi::xml_node &table, Workbook &wb, int sheet_index) {
    int row = 0;
    for (pugi::xml_node row_node : table.children("table:table-row")) {
        int row_repeat = std::max(1, row_node.attribute("table:number-rows-repeated").as_int(1));
        int col = 0;
        for (pugi::xml_node cell_node : row_node.children("table:table-cell")) {
            int col_repeat = std::max(1, cell_node.attribute("table:number-columns-repeated").as_int(1));
            std::string raw = OdsCellRawText(cell_node);
            if (!raw.empty()) {
                int fanout = std::min(col_repeat, 10000);
                for (int k = 0; k < fanout; k++) SetCellRaw(wb, sheet_index, row, col + k, raw);
            }
            col += col_repeat;
        }
        row += row_repeat;
    }
}

/**
 * @brief Writes a computed CellValue's cached representation onto a <table:table-cell> XML node.
 * @param cell_node The <table:table-cell> node to write attributes/children onto.
 * @param v The value to write (a formula's evaluated result, or a literal's own value).
 */
void WriteOdsCachedValue(pugi::xml_node &cell_node, const CellValue &v) {
    switch (v.kind) {
        case CellKind::Number:
            cell_node.append_attribute("office:value-type").set_value("float");
            cell_node.append_attribute("office:value").set_value(FormatCellValue(v).c_str());
            cell_node.append_child("text:p").text().set(FormatCellValue(v).c_str());
            break;
        case CellKind::Bool:
            cell_node.append_attribute("office:value-type").set_value("boolean");
            cell_node.append_attribute("office:boolean-value").set_value(v.boolean ? "true" : "false");
            cell_node.append_child("text:p").text().set(v.boolean ? "TRUE" : "FALSE");
            break;
        case CellKind::Text:
            cell_node.append_attribute("office:value-type").set_value("string");
            cell_node.append_child("text:p").text().set(v.text.c_str());
            break;
        case CellKind::Error:
            cell_node.append_attribute("office:value-type").set_value("string");
            cell_node.append_child("text:p").text().set(FormatCellValue(v).c_str());
            break;
        default:
            break;  // Empty: bare <table:table-cell/>, matching a blank computed result
    }
}

/**
 * @brief Appends one <table:table-row>/<table:table-cell> per used cell in a sheet, writing formulas, numbers, and text/error literals.
 * @param wb Workbook whose sheet at sheet_index is serialized (formula cells are evaluated to get their cached value).
 * @param sheet_index Index of the source sheet in wb.sheets.
 * @param table The <table:table> XML node to append rows/cells to.
 */
void SerializeOdsTable(Workbook &wb, int sheet_index, pugi::xml_node &table) {
    Sheet &sh = wb.sheets[static_cast<size_t>(sheet_index)];
    for (int r = 0; r <= sh.max_row; r++) {
        pugi::xml_node row_node = table.append_child("table:table-row");
        for (int c = 0; c <= sh.max_col; c++) {
            const Cell *cell = sh.FindCell(r, c);
            pugi::xml_node cell_node = row_node.append_child("table:table-cell");
            if (!cell || cell->kind == CellKind::Empty) continue;

            if (cell->kind == CellKind::Formula) {
                // cell->ast is always valid here -- a formula that failed
                // to parse gets CellKind::Error instead (SetCellRaw), not
                // Formula, so this branch never sees a null ast.
                std::string ods_formula = "of:=" + SerializeFormula(cell->ast, /*ods_style=*/true);
                cell_node.append_attribute("table:formula").set_value(ods_formula.c_str());
                WriteOdsCachedValue(cell_node, EvaluateCell(wb, sheet_index, r, c));
            } else if (cell->kind == CellKind::Number) {
                cell_node.append_attribute("office:value-type").set_value("float");
                cell_node.append_attribute("office:value").set_value(FormatCellValue(cell->cached).c_str());
                cell_node.append_child("text:p").text().set(FormatCellValue(cell->cached).c_str());
            } else {
                // Text, or the rare Error-kind literal (an unparsable
                // "=..." raw) -- preserved verbatim as plain display text.
                cell_node.append_attribute("office:value-type").set_value("string");
                cell_node.append_child("text:p").text().set(cell->raw.c_str());
            }
        }
    }
}

}  // namespace

bool LoadOdsFromMemory(const unsigned char *bytes, size_t len, Workbook &out, std::string &error) {
    std::vector<unsigned char> content_bytes;
    if (!ReadZipEntry(bytes, len, "content.xml", content_bytes)) {
        error = "not a valid .ods (missing content.xml)";
        return false;
    }
    pugi::xml_document doc;
    pugi::xml_parse_result result =
        doc.load_buffer(content_bytes.data(), content_bytes.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) {
        error = std::string("malformed content.xml: ") + result.description();
        return false;
    }
    pugi::xml_node spreadsheet = doc.child("office:document-content").child("office:body").child("office:spreadsheet");
    if (!spreadsheet) {
        error = "content.xml has no <office:spreadsheet>";
        return false;
    }

    out.sheets.clear();
    out.xlsx_sheet_paths.clear();
    out.source_format = "ods";

    int sheet_index = 0;
    for (pugi::xml_node table : spreadsheet.children("table:table")) {
        Sheet sh;
        sh.name = table.attribute("table:name").as_string();
        if (sh.name.empty()) sh.name = "Sheet" + std::to_string(sheet_index + 1);
        out.sheets.push_back(std::move(sh));
        ParseOdsTable(table, out, sheet_index);
        sheet_index++;
    }
    if (out.sheets.empty()) {
        error = "content.xml has no <table:table>";
        return false;
    }
    return true;
}

bool SaveOdsToMemory(Workbook &wb, const std::vector<unsigned char> &original_bytes, std::vector<unsigned char> &out,
                      std::string &error) {
    std::vector<unsigned char> content_bytes;
    if (!ReadZipEntry(original_bytes.data(), original_bytes.size(), "content.xml", content_bytes)) {
        error = "not a valid .ods (missing content.xml)";
        return false;
    }
    pugi::xml_document doc;
    pugi::xml_parse_result result =
        doc.load_buffer(content_bytes.data(), content_bytes.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) {
        error = std::string("malformed content.xml: ") + result.description();
        return false;
    }
    pugi::xml_node spreadsheet = doc.child("office:document-content").child("office:body").child("office:spreadsheet");
    if (!spreadsheet) {
        error = "content.xml has no <office:spreadsheet>";
        return false;
    }

    std::vector<pugi::xml_node> tables;
    for (pugi::xml_node table : spreadsheet.children("table:table")) tables.push_back(table);
    if (tables.size() != wb.sheets.size()) {
        error = "sheet count changed since load (not supported in v1)";
        return false;
    }

    for (size_t i = 0; i < tables.size(); i++) {
        pugi::xml_node table = tables[i];
        // Removes only <table:table-row> children -- <table:table-column>
        // definitions and any other structural children stay untouched
        // and, since rows always come last in the ODF schema's own content
        // model for table:table, appending fresh rows below still lands
        // in a valid position.
        while (pugi::xml_node child = table.child("table:table-row")) table.remove_child(child);
        SerializeOdsTable(wb, static_cast<int>(i), table);
    }

    std::ostringstream ss;
    doc.save(ss, "", pugi::format_raw);
    return WriteZipReplacingEntry(original_bytes.data(), original_bytes.size(), "content.xml", ss.str(), out, error);
}
