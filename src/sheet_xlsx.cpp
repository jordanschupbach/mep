#include "office_doc.h"
#include "sheet_doc.h"

// XLSX read/write -- Phase 2 (read) / part of Phase 4 (save-back) of
// NVIM_PARITY_PLAN.md's spreadsheet-pane phase. Same zip+XML approach as
// office_doc.cpp's DOCX support (a .xlsx is a ZIP of XML parts too):
// miniz via office_doc.h's ReadZipEntry/WriteZipReplacingEntries, pugixml
// for the XML itself. Kept in its own translation unit, same reasoning as
// office_odt.cpp staying separate from office_doc.cpp.
//
// Scope matches sheet_doc.h's own documented exclusions -- no merged
// cells, cell styles/number formats (beyond the plain float/string/bool/
// formula distinction), frozen panes, or dates-as-a-real-type (a date/
// time cell's ISO value text loads as a Text literal, not parsed into
// anything date-arithmetic-aware). One real-world XLSX feature IS
// supported: shared formulas (<f t="shared">), since skipping them would
// silently blank out every non-master cell in a filled-down formula
// range -- too common in real files to treat as an acceptable v1 loss the
// way e.g. merged cells are.

#include <cstdlib>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "formula.h"
#include "pugixml.hpp"

namespace {

// Reads every <t> descendant's text within an <si> shared-string entry,
// concatenated -- handles both the simple <si><t>text</t></si> form and
// the rich-text-run form <si><r><t>run1</t></r><r><t>run2</t></r></si>
// (per-run formatting is discarded; sheet cells have no rich-text model).
/**
 * @brief Concatenates every <t> descendant's text within a shared-string <si> entry.
 * @param si The <si> XML node to read.
 * @return The entry's combined text, handling both the simple <si><t>...</t></si> form and the rich-text-run <si><r><t>...</t></r>...</si> form.
 */
std::string CollectSharedStringText(const pugi::xml_node &si) {
    std::string out;
    for (pugi::xml_node child : si.children()) {
        std::string name = child.name();
        if (name == "t") {
            out += child.text().get();
        } else if (name == "r") {
            if (pugi::xml_node t = child.child("t")) out += t.text().get();
        }
    }
    return out;
}

// xl/sharedStrings.xml is optional (a workbook with no text cells at all
// may omit it entirely) -- an empty result here just means every text
// cell must be inlineStr instead, not a load failure.
/**
 * @brief Loads and parses xl/sharedStrings.xml into an index-ordered list of shared-string texts.
 * @param zip_bytes Pointer to the raw .xlsx (zip) file contents.
 * @param zip_len Length in bytes of the buffer pointed to by zip_bytes.
 * @return The shared strings in file order; empty if the part is absent or unparsable (not a load failure -- text cells then use inlineStr instead).
 */
std::vector<std::string> ParseSharedStrings(const unsigned char *zip_bytes, size_t zip_len) {
    std::vector<std::string> out;
    std::vector<unsigned char> xml_bytes;
    if (!ReadZipEntry(zip_bytes, zip_len, "xl/sharedStrings.xml", xml_bytes)) return out;
    pugi::xml_document doc;
    if (!doc.load_buffer(xml_bytes.data(), xml_bytes.size(), pugi::parse_default, pugi::encoding_utf8)) return out;
    for (pugi::xml_node si : doc.child("sst").children("si")) out.push_back(CollectSharedStringText(si));
    return out;
}

struct SheetEntry {
    std::string name;
    std::string path;  // zip entry path, e.g. "xl/worksheets/sheet1.xml"
};

// Parses xl/workbook.xml (sheet name + r:id, in document order) joined
// with xl/_rels/workbook.xml.rels (r:id -> Target path) into an ordered
// sheet list. Tolerant, matching LoadDocxFromMemory's own philosophy: a
// missing rels part or an unmatched r:id falls back to the conventional
// "worksheets/sheetN.xml" path (1-indexed by document order) rather than
// failing the whole load.
/**
 * @brief Parses xl/workbook.xml joined with xl/_rels/workbook.xml.rels into an ordered list of sheet name/zip-path pairs.
 * @param zip_bytes Pointer to the raw .xlsx (zip) file contents.
 * @param zip_len Length in bytes of the buffer pointed to by zip_bytes.
 * @param error Receives a message if xl/workbook.xml is missing, malformed, or has no <sheets>.
 * @return The sheets in document order (empty on failure, with error set); an unmatched r:id or missing rels part falls back to a conventional "worksheets/sheetN.xml" path rather than failing.
 */
std::vector<SheetEntry> ParseWorkbookSheetList(const unsigned char *zip_bytes, size_t zip_len, std::string &error) {
    std::vector<SheetEntry> sheets;
    std::vector<unsigned char> wb_xml;
    if (!ReadZipEntry(zip_bytes, zip_len, "xl/workbook.xml", wb_xml)) {
        error = "not a valid .xlsx (missing xl/workbook.xml)";
        return sheets;
    }
    pugi::xml_document doc;
    pugi::xml_parse_result result =
        doc.load_buffer(wb_xml.data(), wb_xml.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) {
        error = std::string("malformed xl/workbook.xml: ") + result.description();
        return sheets;
    }
    pugi::xml_node sheets_node = doc.child("workbook").child("sheets");
    if (!sheets_node) {
        error = "xl/workbook.xml has no <sheets>";
        return sheets;
    }

    std::unordered_map<std::string, std::string> rid_to_target;
    std::vector<unsigned char> rels_xml;
    if (ReadZipEntry(zip_bytes, zip_len, "xl/_rels/workbook.xml.rels", rels_xml)) {
        pugi::xml_document rels_doc;
        if (rels_doc.load_buffer(rels_xml.data(), rels_xml.size(), pugi::parse_default, pugi::encoding_utf8)) {
            for (pugi::xml_node rel : rels_doc.child("Relationships").children("Relationship")) {
                std::string id = rel.attribute("Id").as_string();
                std::string target = rel.attribute("Target").as_string();
                if (!id.empty() && !target.empty()) rid_to_target[id] = target;
            }
        }
    }

    int fallback_index = 1;
    for (pugi::xml_node sheet_node : sheets_node.children("sheet")) {
        SheetEntry entry;
        entry.name = sheet_node.attribute("name").as_string();
        std::string rid = sheet_node.attribute("r:id").as_string();
        auto it = rid.empty() ? rid_to_target.end() : rid_to_target.find(rid);
        std::string target = (it != rid_to_target.end())
                                  ? it->second
                                  : ("worksheets/sheet" + std::to_string(fallback_index) + ".xml");
        // Targets in workbook.xml.rels are relative to xl/ -- normalize
        // both a "worksheets/sheet1.xml" relative form and a
        // "/xl/worksheets/sheet1.xml" absolute form some producers use.
        if (target.rfind("/xl/", 0) == 0) {
            target = target.substr(1);
        } else if (target.rfind("xl/", 0) != 0) {
            target.insert(0, "xl/");
        }
        entry.path = target;
        if (entry.name.empty()) entry.name = "Sheet" + std::to_string(fallback_index);
        sheets.push_back(std::move(entry));
        fallback_index++;
    }
    return sheets;
}

struct ParsedCellRef {
    int row = -1, col = -1;
};

/**
 * @brief Parses an XLSX cell-reference string (e.g. "B7") into zero-based row/col indices.
 * @param r Cell reference text from a <c r="..."> attribute.
 * @return The parsed row/col, or {-1,-1} if r doesn't parse as a valid cell address.
 */
ParsedCellRef ParseXlsxCellRef(const std::string &r) {
    ParsedCellRef out;
    bool row_abs = false, col_abs = false;
    if (!ParseCellAddress(r, out.row, out.col, row_abs, col_abs)) {
        out.row = -1;
        out.col = -1;
    }
    return out;
}

// One XLSX shared-formula group's master: the cell that carries the
// group's actual formula text (every other member cell's <f t="shared">
// has an `si` back-reference but no text of its own).
struct SharedFormulaMaster {
    int row = 0, col = 0;
    std::string formula;  // native syntax, no leading '='
};

/**
 * @brief Populates one sheet's cells from a parsed worksheet XML document, resolving shared strings and expanding shared formulas.
 * @param doc Parsed worksheet XML document (its <worksheet><sheetData> is read).
 * @param wb Workbook whose sheet at sheet_index is populated.
 * @param sheet_index Index of the destination sheet in wb.sheets.
 * @param shared_strings Index-ordered shared-string table used to resolve t="s" cells.
 */
void ParseXlsxSheetXml(const pugi::xml_document &doc, Workbook &wb, int sheet_index,
                        const std::vector<std::string> &shared_strings) {
    pugi::xml_node sheet_data = doc.child("worksheet").child("sheetData");
    if (!sheet_data) return;

    std::unordered_map<int, SharedFormulaMaster> shared_masters;
    int implicit_row = 0;

    for (pugi::xml_node row_node : sheet_data.children("row")) {
        int row = row_node.attribute("r") ? row_node.attribute("r").as_int() - 1 : implicit_row;
        if (row < 0) row = implicit_row;
        int implicit_col = 0;

        for (pugi::xml_node c_node : row_node.children("c")) {
            std::string r_attr = c_node.attribute("r").as_string();
            int col = implicit_col;
            if (!r_attr.empty()) {
                ParsedCellRef pr = ParseXlsxCellRef(r_attr);
                if (pr.col >= 0) col = pr.col;
            }
            implicit_col = col + 1;

            pugi::xml_node f_node = c_node.child("f");
            pugi::xml_node v_node = c_node.child("v");
            std::string raw;

            if (f_node) {
                std::string ftype = f_node.attribute("t").as_string();
                std::string ftext = f_node.text().get();
                if (ftype == "shared") {
                    std::string si_attr = f_node.attribute("si").as_string();
                    int si = si_attr.empty() ? -1 : std::atoi(si_attr.c_str());
                    if (!ftext.empty()) {
                        if (si >= 0) shared_masters[si] = SharedFormulaMaster{row, col, ftext};
                        raw = "=" + ftext;
                    } else if (si >= 0) {
                        auto it = shared_masters.find(si);
                        if (it != shared_masters.end()) {
                            std::string parse_err;
                            auto ast = ParseFormula(it->second.formula, parse_err);
                            if (ast) {
                                auto shifted = ShiftFormulaRefs(ast, row - it->second.row, col - it->second.col);
                                raw = "=" + SerializeFormula(shifted, /*ods_style=*/false);
                            }
                        }
                        // No master seen yet (out-of-order/malformed) and no
                        // inline text of its own: falls through to the
                        // cached <v> below, same tolerance as any other
                        // unrecognized cell -- never fails the whole load.
                    }
                } else {
                    raw = "=" + ftext;
                }
            }

            if (raw.empty()) {
                std::string t = c_node.attribute("t").as_string();  // "" | "s" | "str" | "inlineStr" | "b" | "e" | "n"
                if (t == "s") {
                    if (v_node) {
                        int idx = std::atoi(v_node.text().get());
                        if (idx >= 0 && idx < static_cast<int>(shared_strings.size()))
                            raw = shared_strings[static_cast<size_t>(idx)];
                    }
                } else if (t == "inlineStr") {
                    if (pugi::xml_node is = c_node.child("is")) raw = CollectSharedStringText(is);
                } else if (t == "b") {
                    // SetCellRaw's own literal parser treats bare "TRUE"/
                    // "FALSE" text as Text, not Bool (see sheet_doc.cpp's
                    // LiteralValueFromText) -- wrapping as a trivial
                    // formula is the only way to get a real Bool CellKind
                    // out of it. Shows as "=TRUE"/"=FALSE" in the formula
                    // bar rather than bare "TRUE" -- a visible v1 quirk,
                    // not a correctness bug (EvaluateCell still returns a
                    // genuine Bool either way).
                    if (v_node) raw = (std::string(v_node.text().get()) == "1") ? "=TRUE" : "=FALSE";
                } else if (t == "str" || t == "e") {
                    // "str": a cached formula-string-result with no <f> of
                    // its own (rare); "e": a raw error token ("#DIV/0!")
                    // with no backing formula. Both stored as plain text --
                    // there's no formula to re-derive a real Error/live
                    // value from.
                    if (v_node) raw = v_node.text().get();
                } else {
                    // "n" or no `t` at all: plain number, verbatim.
                    if (v_node) raw = v_node.text().get();
                }
            }

            if (!raw.empty()) SetCellRaw(wb, sheet_index, row, col, raw);
        }
        implicit_row = row + 1;
    }
}

/**
 * @brief Writes a computed CellValue's cached representation (a <v>, and a type attribute for bool/text/error) onto a <c> XML node.
 * @param c_node The <c> node to write attributes/children onto.
 * @param v The value to write (a formula's evaluated result, or a literal's own value).
 */
void WriteXlsxCachedValue(pugi::xml_node &c_node, const CellValue &v) {
    switch (v.kind) {
        case CellKind::Number:
            c_node.append_child("v").text().set(FormatCellValue(v).c_str());
            break;
        case CellKind::Bool:
            c_node.append_attribute("t").set_value("b");
            c_node.append_child("v").text().set(v.boolean ? "1" : "0");
            break;
        case CellKind::Text:
            c_node.append_attribute("t").set_value("str");
            c_node.append_child("v").text().set(v.text.c_str());
            break;
        case CellKind::Error:
            c_node.append_attribute("t").set_value("e");
            c_node.append_child("v").text().set(FormatCellValue(v).c_str());
            break;
        default:
            break;  // Empty: no <v>, matching a blank computed result
    }
}

// Rebuilds `sheet_data`'s <row>/<c> children from `wb.sheets[sheet_index]`.
// Text cells are written as inlineStr (an XML-inline value directly on the
// cell) rather than shared strings -- avoids also having to grow/rewrite
// xl/sharedStrings.xml's index and count attributes in lockstep on every
// save; a real, if slightly larger-than-necessary, tradeoff Excel/LO both
// read back correctly regardless.
/**
 * @brief Appends one <row>/<c> per non-empty row/cell in a sheet, writing formulas (as inline <f>/<v>), numbers, and text/error literals (as inlineStr).
 * @param wb Workbook whose sheet at sheet_index is serialized (formula cells are evaluated to get their cached value).
 * @param sheet_index Index of the source sheet in wb.sheets.
 * @param sheet_data The <sheetData> XML node to append rows/cells to.
 */
void SerializeXlsxSheetData(Workbook &wb, int sheet_index, pugi::xml_node &sheet_data) {
    const Sheet &sh = wb.sheets[static_cast<size_t>(sheet_index)];
    for (int r = 0; r <= sh.max_row; r++) {
        bool row_has_content = false;
        for (int c = 0; c <= sh.max_col; c++) {
            if (sh.FindCell(r, c)) {
                row_has_content = true;
                break;
            }
        }
        if (!row_has_content) continue;  // sparse: <row> elements needn't be contiguous per the OOXML schema

        pugi::xml_node row_node = sheet_data.append_child("row");
        row_node.append_attribute("r").set_value(std::to_string(r + 1).c_str());

        for (int c = 0; c <= sh.max_col; c++) {
            const Cell *cell = sh.FindCell(r, c);
            if (!cell) continue;
            pugi::xml_node c_node = row_node.append_child("c");
            c_node.append_attribute("r").set_value(CellAddressToString(r, c).c_str());

            if (cell->kind == CellKind::Formula) {
                CellValue v = EvaluateCell(wb, sheet_index, r, c);
                std::string formula_text = cell->raw.substr(1);  // strip leading '='
                c_node.append_child("f").text().set(formula_text.c_str());
                WriteXlsxCachedValue(c_node, v);
            } else if (cell->kind == CellKind::Number) {
                c_node.append_child("v").text().set(FormatCellValue(cell->cached).c_str());
            } else {
                // Text, or the rare Error-kind literal (an unparsable
                // "=..." raw from SetCellRaw's own failure path) -- either
                // way, preserved verbatim as inline display text so
                // nothing is silently dropped on save.
                c_node.append_attribute("t").set_value("inlineStr");
                c_node.append_child("is").append_child("t").text().set(cell->raw.c_str());
            }
        }
    }
}

}  // namespace

bool LoadXlsxFromMemory(const unsigned char *bytes, size_t len, Workbook &out, std::string &error) {
    std::vector<SheetEntry> sheet_list = ParseWorkbookSheetList(bytes, len, error);
    if (sheet_list.empty()) {
        if (error.empty()) error = "xl/workbook.xml declares no sheets";
        return false;
    }
    std::vector<std::string> shared_strings = ParseSharedStrings(bytes, len);

    out.sheets.clear();
    out.xlsx_sheet_paths.clear();
    out.source_format = "xlsx";
    for (const auto &entry : sheet_list) {
        Sheet sh;
        sh.name = entry.name;
        out.sheets.push_back(std::move(sh));
        out.xlsx_sheet_paths.push_back(entry.path);
    }

    for (size_t i = 0; i < sheet_list.size(); i++) {
        std::vector<unsigned char> sheet_xml;
        if (!ReadZipEntry(bytes, len, sheet_list[i].path.c_str(), sheet_xml)) continue;  // tolerant: skip an unreadable sheet part
        pugi::xml_document doc;
        if (!doc.load_buffer(sheet_xml.data(), sheet_xml.size(), pugi::parse_default, pugi::encoding_utf8)) continue;
        ParseXlsxSheetXml(doc, out, static_cast<int>(i), shared_strings);
    }
    return true;
}

bool SaveXlsxToMemory(Workbook &wb, const std::vector<unsigned char> &original_bytes, std::vector<unsigned char> &out,
                       std::string &error) {
    if (wb.sheets.size() != wb.xlsx_sheet_paths.size()) {
        error = "internal error: sheet count doesn't match the workbook's own zip-path list";
        return false;
    }
    std::vector<std::pair<std::string, std::string>> entries;
    for (size_t i = 0; i < wb.sheets.size(); i++) {
        std::vector<unsigned char> sheet_xml;
        if (!ReadZipEntry(original_bytes.data(), original_bytes.size(), wb.xlsx_sheet_paths[i].c_str(), sheet_xml)) {
            error = "missing worksheet part: " + wb.xlsx_sheet_paths[i];
            return false;
        }
        pugi::xml_document doc;
        pugi::xml_parse_result result =
            doc.load_buffer(sheet_xml.data(), sheet_xml.size(), pugi::parse_default, pugi::encoding_utf8);
        if (!result) {
            error = "malformed " + wb.xlsx_sheet_paths[i] + ": " + result.description();
            return false;
        }
        pugi::xml_node worksheet = doc.child("worksheet");
        if (!worksheet) {
            error = wb.xlsx_sheet_paths[i] + " has no <worksheet>";
            return false;
        }

        pugi::xml_node sheet_data = worksheet.child("sheetData");
        if (sheet_data) {
            // Reparse-and-replace-children-in-place (not remove+reinsert
            // the node itself) keeps <sheetData>'s required position
            // between <cols>/<sheetFormatPr> and <mergeCells>/
            // <pageMargins>/etc. correct for free -- same convention as
            // SaveDocxToMemory's own <w:body> child-replacement.
            while (pugi::xml_node child = sheet_data.first_child()) sheet_data.remove_child(child);
        } else {
            pugi::xml_node anchor;
            for (pugi::xml_node child : worksheet.children()) {
                std::string name = child.name();
                if (name == "sheetCalcPr" || name == "sheetProtection" || name == "mergeCells" ||
                    name == "conditionalFormatting" || name == "dataValidations" || name == "hyperlinks" ||
                    name == "pageMargins" || name == "pageSetup" || name == "headerFooter") {
                    anchor = child;
                    break;
                }
            }
            sheet_data =
                anchor ? worksheet.insert_child_before("sheetData", anchor) : worksheet.append_child("sheetData");
        }

        SerializeXlsxSheetData(wb, static_cast<int>(i), sheet_data);

        std::ostringstream ss;
        doc.save(ss, "", pugi::format_raw);
        entries.emplace_back(wb.xlsx_sheet_paths[i], ss.str());
    }
    return WriteZipReplacingEntries(original_bytes.data(), original_bytes.size(), entries, out, error);
}
