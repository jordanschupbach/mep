#include "office_doc.h"
#include "image_doc.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pugixml.hpp"

// ODT parsing (content.xml + styles.xml via pugixml, both extracted from
// the .odt ZIP container via miniz -- see ReadZipEntry, shared with
// office_doc.cpp's DOCX parser). Mirrors LoadDocxFromMemory's tolerance
// convention (skip bad paragraphs/styles rather than fail the whole
// load) but the schema shape differs enough (explicit heading levels,
// real style *sheets* referenced by name rather than DOCX's flat
// per-run toggle elements) that it's its own file per the plan.

namespace {

// One resolved ODT style's fields, each with a has_* flag so a name-only
// partial override (e.g. a <text:span> style that only sets bold) doesn't
// clobber fields it never mentioned when composed with an enclosing
// format. `parent` is style:parent-style-name -- resolved one level only
// (not a full cascade -- v1 scope, same simplification DOCX's heading-
// name recognition makes for w:basedOn).
struct OdtStyle {
    bool has_bold = false, bold = false;
    bool has_italic = false, italic = false;
    bool has_underline = false, underline = false;
    bool has_strike = false, strike = false;
    bool has_super = false, is_super = false, is_sub = false;
    bool has_font_family = false;
    OfficeFontFamily font_family = OfficeFontFamily::Sans;
    bool has_font_size = false;
    float font_size_pt = 0.0f;
    bool has_color = false;
    unsigned char color_r = 0, color_g = 0, color_b = 0;
    bool has_highlight = false;
    unsigned char highlight_r = 255, highlight_g = 255, highlight_b = 0;
    bool has_align = false;
    DocParagraph::Align align = DocParagraph::Align::Left;
    std::string parent;
};

// Same name-heuristic DOCX's own DocxFontFamilyFromName uses (office_doc.cpp)
// -- ODF's style:font-name is typically already a human-readable family
// name (not an indirect id needing font-face-decls resolution), so the
// same substring match applies.
/**
 * @brief Maps an ODF font-name/font-family value to one of the 3 embedded logical families.
 * @param name The font name, e.g. from style:font-name or fo:font-family.
 * @return Serif/Mono if `name` matches a recognized family or metric-compatible substitute; Sans otherwise.
 */
OfficeFontFamily OdtFontFamilyFromName(const std::string &name) {
    std::string lower;
    for (char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower.find("serif") != std::string::npos || lower.find("times") != std::string::npos ||
        lower.find("georgia") != std::string::npos || lower.find("cambria") != std::string::npos ||
        lower.find("garamond") != std::string::npos) {
        return OfficeFontFamily::Serif;
    }
    if (lower.find("mono") != std::string::npos || lower.find("courier") != std::string::npos ||
        lower.find("consolas") != std::string::npos) {
        return OfficeFontFamily::Mono;
    }
    return OfficeFontFamily::Sans;
}

// "#RRGGBB" -> true + rgb out-params; anything else (a named color, an
// empty/"transparent" background) returns false, matching this file's own
// existing "silently ignore what isn't a plain hex value" tolerance.
/**
 * @brief Parses a "#RRGGBB" ODF color string into RGB byte out-params.
 * @param v The color string to parse.
 * @param r Receives the red component on success.
 * @param g Receives the green component on success.
 * @param b Receives the blue component on success.
 * @return True if `v` is a valid "#RRGGBB" hex string; false otherwise (out-params untouched).
 */
bool ParseOdtHexColor(const std::string &v, unsigned char *r, unsigned char *g, unsigned char *b) {
    if (v.size() != 7 || v[0] != '#') return false;
    unsigned int rgb = 0;
    if (std::sscanf(v.c_str() + 1, "%x", &rgb) != 1) return false;
    *r = static_cast<unsigned char>((rgb >> 16) & 0xff);
    *g = static_cast<unsigned char>((rgb >> 8) & 0xff);
    *b = static_cast<unsigned char>(rgb & 0xff);
    return true;
}

using StyleMap = std::unordered_map<std::string, OdtStyle>;

/**
 * @brief Reads a <style:style>'s text/paragraph properties and parent-style-name into an OdtStyle.
 * @param style_node The <style:style> node to read.
 * @param s The style struct to populate (only fields the node actually sets are marked has_*).
 */
void ParseStyleNode(const pugi::xml_node &style_node, OdtStyle &s) {
    if (pugi::xml_node tp = style_node.child("style:text-properties")) {
        if (pugi::xml_attribute a = tp.attribute("fo:font-weight")) {
            s.has_bold = true;
            s.bold = (std::string(a.as_string()) == "bold");
        }
        if (pugi::xml_attribute a = tp.attribute("fo:font-style")) {
            s.has_italic = true;
            s.italic = (std::string(a.as_string()) == "italic");
        }
        if (pugi::xml_attribute a = tp.attribute("style:text-underline-style")) {
            s.has_underline = true;
            s.underline = (std::string(a.as_string()) != "none");
        }
        if (pugi::xml_attribute a = tp.attribute("style:text-line-through-style")) {
            s.has_strike = true;
            s.strike = (std::string(a.as_string()) != "none");
        }
        if (pugi::xml_attribute a = tp.attribute("style:text-position")) {
            // e.g. "super 58%" / "sub 58%" / "0% 100%" -- only the leading
            // keyword matters here (the percentage is a size scale real
            // office suites apply; mep's own renderer picks its own fixed
            // scale for superscript/subscript runs, same as it already
            // picks its own fixed heading-size scale rather than reading
            // one from the file).
            std::string v = a.as_string();
            s.has_super = true;
            s.is_super = v.rfind("super", 0) == 0;
            s.is_sub = v.rfind("sub", 0) == 0;
        }
        if (pugi::xml_attribute a = tp.attribute("style:font-name")) {
            s.has_font_family = true;
            s.font_family = OdtFontFamilyFromName(a.as_string());
        } else if (pugi::xml_attribute a2 = tp.attribute("fo:font-family")) {
            s.has_font_family = true;
            s.font_family = OdtFontFamilyFromName(a2.as_string());
        }
        if (pugi::xml_attribute a = tp.attribute("fo:font-size")) {
            std::string v = a.as_string();
            float pt = 0.0f;
            if (std::sscanf(v.c_str(), "%f", &pt) == 1 && pt > 0.0f) {
                s.has_font_size = true;
                s.font_size_pt = pt;
            }
        }
        if (pugi::xml_attribute a = tp.attribute("fo:color")) {
            unsigned char r, g, b;
            if (ParseOdtHexColor(a.as_string(), &r, &g, &b)) {
                s.has_color = true;
                s.color_r = r; s.color_g = g; s.color_b = b;
            }
        }
        if (pugi::xml_attribute a = tp.attribute("fo:background-color")) {
            unsigned char r, g, b;
            if (ParseOdtHexColor(a.as_string(), &r, &g, &b)) {
                s.has_highlight = true;
                s.highlight_r = r; s.highlight_g = g; s.highlight_b = b;
            }
        }
    }
    if (pugi::xml_node pp = style_node.child("style:paragraph-properties")) {
        if (pugi::xml_attribute a = pp.attribute("fo:text-align")) {
            s.has_align = true;
            std::string v = a.as_string();
            if (v == "center") s.align = DocParagraph::Align::Center;
            else if (v == "end" || v == "right") s.align = DocParagraph::Align::Right;
            else if (v == "justify") s.align = DocParagraph::Align::Justify;
            else s.align = DocParagraph::Align::Left;
        }
    }
    if (pugi::xml_attribute parent = style_node.attribute("style:parent-style-name")) {
        s.parent = parent.as_string();
    }
}

// Collects every <style:style style:name="..."> under `container` (an
// <office:automatic-styles> or <office:styles> node) into `out` -- called
// twice (content.xml's automatic-styles, then styles.xml's named
// styles/automatic-styles) into the same map; the two namespaces don't
// collide in practice (LibreOffice/Word-exported automatic names like
// "P1"/"T1" vs. named styles like "Standard"/"Heading_20_1").
/**
 * @brief Collects every <style:style style:name="..."> child of `container` into a name -> OdtStyle map.
 * @param container The <office:automatic-styles> or <office:styles> node to scan.
 * @param out The map to insert parsed styles into (merged with any existing entries).
 */
void CollectStyles(const pugi::xml_node &container, StyleMap &out) {
    for (pugi::xml_node style_node : container.children("style:style")) {
        pugi::xml_attribute name_attr = style_node.attribute("style:name");
        if (!name_attr) continue;
        OdtStyle s;
        ParseStyleNode(style_node, s);
        out[name_attr.as_string()] = s;
    }
}

// Resolves a style name against the map, folding in one level of
// style:parent-style-name for any field the named style itself didn't
// set (not a full cascade -- see OdtStyle's own comment).
/**
 * @brief Looks up a style by name and folds in one level of its parent style for any unset fields.
 * @param styles The name -> OdtStyle map to look up in.
 * @param name The style name to resolve (returns a default OdtStyle if empty or not found).
 * @return The resolved style, with fields the named style didn't set filled in from its parent (one level only).
 */
OdtStyle ResolveStyle(const StyleMap &styles, const std::string &name) {
    if (name.empty()) return OdtStyle{};
    auto it = styles.find(name);
    if (it == styles.end()) return OdtStyle{};
    OdtStyle result = it->second;
    if (!result.parent.empty()) {
        auto pit = styles.find(result.parent);
        if (pit != styles.end()) {
            const OdtStyle &parent = pit->second;
            if (!result.has_bold && parent.has_bold) { result.has_bold = true; result.bold = parent.bold; }
            if (!result.has_italic && parent.has_italic) { result.has_italic = true; result.italic = parent.italic; }
            if (!result.has_underline && parent.has_underline) {
                result.has_underline = true;
                result.underline = parent.underline;
            }
            if (!result.has_strike && parent.has_strike) { result.has_strike = true; result.strike = parent.strike; }
            if (!result.has_super && parent.has_super) {
                result.has_super = true;
                result.is_super = parent.is_super;
                result.is_sub = parent.is_sub;
            }
            if (!result.has_font_family && parent.has_font_family) {
                result.has_font_family = true;
                result.font_family = parent.font_family;
            }
            if (!result.has_font_size && parent.has_font_size) {
                result.has_font_size = true;
                result.font_size_pt = parent.font_size_pt;
            }
            if (!result.has_color && parent.has_color) {
                result.has_color = true;
                result.color_r = parent.color_r; result.color_g = parent.color_g; result.color_b = parent.color_b;
            }
            if (!result.has_highlight && parent.has_highlight) {
                result.has_highlight = true;
                result.highlight_r = parent.highlight_r; result.highlight_g = parent.highlight_g;
                result.highlight_b = parent.highlight_b;
            }
            if (!result.has_align && parent.has_align) { result.has_align = true; result.align = parent.align; }
        }
    }
    return result;
}

// Walks a paragraph/heading's inline content in document order (mixed
// text + element children, unlike DOCX's leaf-only <w:t>), recursing into
// <text:span> (composing its style on top of `base_fmt` -- only fields
// the span's own style sets override) and <text:a> (hyperlink text,
// preserved but not clickable in v1, same convention as DOCX's
// <w:hyperlink> recursion), mapping <text:tab/>/<text:line-break/> to
// embedded '\t'/'\n' and <text:s text:c="N"/> (explicit preserved
// space run) to N literal spaces.
/**
 * @brief Walks a paragraph's mixed text/element children, appending text and building DocSpans for formatted runs.
 * @param node The node whose children to walk (a <text:p>/<text:h>, or a nested <text:span>/<text:a>).
 * @param styles The resolved style map, used to look up <text:span> formatting.
 * @param base_fmt The format inherited from the enclosing context, composed with any span-level overrides.
 * @param out The paragraph to append text/spans onto.
 */
void CollectOdtInline(const pugi::xml_node &node, const StyleMap &styles, DocFormat base_fmt, DocParagraph &out) {
    for (pugi::xml_node child : node.children()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            int start = static_cast<int>(out.text.size());
            out.text += child.value();
            int len = static_cast<int>(out.text.size()) - start;
            if (len > 0 && base_fmt != DocFormat{}) {
                out.spans.push_back({start, start + len, base_fmt});
            }
            continue;
        }
        std::string name = child.name();
        if (name == "text:span") {
            DocFormat fmt = base_fmt;
            if (pugi::xml_attribute sn = child.attribute("text:style-name")) {
                OdtStyle s = ResolveStyle(styles, sn.as_string());
                if (s.has_bold) fmt.bold = s.bold;
                if (s.has_italic) fmt.italic = s.italic;
                if (s.has_underline) fmt.underline = s.underline;
                if (s.has_strike) fmt.strike = s.strike;
                if (s.has_super) { fmt.superscript = s.is_super; fmt.subscript = s.is_sub; }
                if (s.has_font_family) fmt.font_family = s.font_family;
                if (s.has_font_size) fmt.font_size_pt = s.font_size_pt;
                if (s.has_color) { fmt.has_color = true; fmt.color_r = s.color_r; fmt.color_g = s.color_g; fmt.color_b = s.color_b; }
                if (s.has_highlight) {
                    fmt.has_highlight = true;
                    fmt.highlight_r = s.highlight_r; fmt.highlight_g = s.highlight_g; fmt.highlight_b = s.highlight_b;
                }
            }
            CollectOdtInline(child, styles, fmt, out);
        } else if (name == "text:a") {
            CollectOdtInline(child, styles, base_fmt, out);
        } else if (name == "text:tab") {
            out.text.push_back('\t');
        } else if (name == "text:line-break") {
            out.text.push_back('\n');
        } else if (name == "text:s") {
            int count = std::max(1, child.attribute("text:c").as_int(1));
            out.text.append(static_cast<size_t>(count), ' ');
        }
        // Other inline content (bookmarks, notes, changes, frames/images)
        // silently skipped -- v1 scope, matches DOCX's own "skip what
        // carries no plain-text content this model represents" rule.
    }
}

/**
 * @brief Parses one <text:p>/<text:h> into a DocParagraph, resolving its paragraph style and inline runs.
 * @param p_node The <text:p> or <text:h> node to parse.
 * @param styles The resolved style map.
 * @param in_list Whether this paragraph is inside a <text:list> (marks it as a bullet paragraph).
 * @return The parsed paragraph.
 */
DocParagraph ParseOdtParagraph(const pugi::xml_node &p_node, const StyleMap &styles, bool in_list) {
    DocParagraph out;
    std::string style_name;
    if (pugi::xml_attribute sn = p_node.attribute("text:style-name")) style_name = sn.as_string();
    OdtStyle pstyle = ResolveStyle(styles, style_name);
    if (pstyle.has_align) out.align = pstyle.align;

    std::string tag = p_node.name();
    if (tag == "text:h") {
        out.heading_level = std::clamp(p_node.attribute("text:outline-level").as_int(1), 1, 6);
    }
    out.list_kind = in_list ? DocParagraph::ListKind::Bullet : DocParagraph::ListKind::None;

    DocFormat base_fmt;
    base_fmt.bold = pstyle.has_bold && pstyle.bold;
    base_fmt.italic = pstyle.has_italic && pstyle.italic;
    base_fmt.underline = pstyle.has_underline && pstyle.underline;
    base_fmt.strike = pstyle.has_strike && pstyle.strike;
    if (pstyle.has_super) { base_fmt.superscript = pstyle.is_super; base_fmt.subscript = pstyle.is_sub; }
    if (pstyle.has_font_family) base_fmt.font_family = pstyle.font_family;
    if (pstyle.has_font_size) base_fmt.font_size_pt = pstyle.font_size_pt;
    if (pstyle.has_color) {
        base_fmt.has_color = true;
        base_fmt.color_r = pstyle.color_r; base_fmt.color_g = pstyle.color_g; base_fmt.color_b = pstyle.color_b;
    }
    if (pstyle.has_highlight) {
        base_fmt.has_highlight = true;
        base_fmt.highlight_r = pstyle.highlight_r; base_fmt.highlight_g = pstyle.highlight_g;
        base_fmt.highlight_b = pstyle.highlight_b;
    }
    CollectOdtInline(p_node, styles, base_fmt, out);
    CoalesceSpans(out.spans);
    return out;
}

// A <table:table-cell>'s plain text: CollectOdtInline already knows how to
// flatten <text:span>/<text:tab>/<text:line-break>/etc. into a flat string
// (that's exactly what a DocParagraph's own `text` field is) -- reused here
// via a scratch DocParagraph per <text:p>, discarding its spans (DocTable
// cells carry no per-run formatting, matching this file's own scope note),
// joining multiple paragraphs within one cell with '\n'.
/**
 * @brief Extracts a <table:table-cell>'s plain text, joining multiple paragraphs with '\n'.
 * @param cell_node The <table:table-cell> node to read.
 * @param styles The resolved style map (needed by CollectOdtInline, though spans are discarded here).
 * @return The cell's flattened plain text.
 */
std::string OdtCellText(const pugi::xml_node &cell_node, const StyleMap &styles) {
    std::string text;
    bool first_p = true;
    for (pugi::xml_node p : cell_node.children("text:p")) {
        if (!first_p) text += "\n";
        first_p = false;
        DocParagraph scratch;
        CollectOdtInline(p, styles, DocFormat{}, scratch);
        text += scratch.text;
    }
    return text;
}

// Parses a <table:table> into a DocTable. Column count is taken from the
// widest <table:table-row> (not the <table:table-column> declarations,
// which use table:number-columns-repeated and can disagree with actual
// cell counts) -- shorter rows are padded with empty cells. A repeated
// cell (<table:table-cell table:number-columns-repeated="N"/>, ODF's way
// of saying "N identical empty cells in a row") is expanded to N entries,
// same simplification DOCX's own w:gridCol-vs-actual-w:tc mismatch handling
// makes.
/**
 * @brief Parses a <table:table> into a DocTable, expanding repeated cells and padding shorter rows.
 * @param table_node The <table:table> node to parse.
 * @param styles The resolved style map, passed through to cell text extraction.
 * @return The parsed table, sized rows x (widest row's column count).
 */
DocTable ParseOdtTable(const pugi::xml_node &table_node, const StyleMap &styles) {
    std::vector<std::vector<std::string>> rows;
    int max_cols = 0;
    for (pugi::xml_node tr : table_node.children("table:table-row")) {
        std::vector<std::string> row;
        for (pugi::xml_node tc : tr.children("table:table-cell")) {
            int repeat = std::max(1, tc.attribute("table:number-columns-repeated").as_int(1));
            std::string text = OdtCellText(tc, styles);
            for (int i = 0; i < repeat; i++) row.push_back(text);
        }
        max_cols = std::max(max_cols, static_cast<int>(row.size()));
        rows.push_back(std::move(row));
    }
    DocTable t;
    t.rows = static_cast<int>(rows.size());
    t.cols = std::max(1, max_cols);
    t.cells.assign(static_cast<size_t>(t.rows) * static_cast<size_t>(t.cols), std::string());
    for (int r = 0; r < t.rows; r++) {
        for (int c = 0; c < static_cast<int>(rows[static_cast<size_t>(r)].size()) && c < t.cols; c++) {
            t.Cell(r, c) = rows[static_cast<size_t>(r)][static_cast<size_t>(c)];
        }
    }
    return t;
}

// Recursively searches a <text:p>/<text:h> for an embedded image's
// <draw:image xlink:href="Pictures/..."/> (inside a <draw:frame>) and
// returns its href -- already a package-root-relative zip path in ODF
// (unlike DOCX's indirection through a relationship id), so no separate
// resolution step is needed.
/**
 * @brief Recursively searches a node's descendants for a <draw:image xlink:href="..."/> and returns its href.
 * @param node The node to search within (typically a <text:p>/<text:h>).
 * @param href Receives the image's package-relative href if found.
 * @return True if an image was found (`href` set); false otherwise.
 */
bool FindOdtImageHref(const pugi::xml_node &node, std::string &href) {
    for (pugi::xml_node child : node.children()) {
        if (std::string(child.name()) == "draw:image") {
            if (pugi::xml_attribute h = child.attribute("xlink:href")) {
                href = h.as_string();
                return true;
            }
        }
        if (FindOdtImageHref(child, href)) return true;
    }
    return false;
}

// Walks <office:text> (and recursively <text:list>/<text:list-item>,
// v1: bullet-or-not only, no real numbering -- same simplification as
// DOCX's <w:numPr> handling) collecting one DocParagraph per <text:p>/
// <text:h>, plus one DocTable (with a fresh empty anchor paragraph, same
// convention LoadDocxFromMemory's <w:tbl> handling uses) per
// <table:table>. An embedded image lives *inside* one of the paragraph's
// own children (<draw:frame><draw:image .../></draw:frame>), so unlike a
// table it needs no synthetic anchor -- the paragraph it's already part of
// becomes the anchor via DocParagraph::image_ref, same as DOCX's inline
// <w:drawing> handling. Sections are still skipped, not recursed into --
// out of v1 scope, and (unlike a raw ZIP-entry failure) skipping a single
// unsupported element is exactly the per-element tolerance this parser is
// meant to have.
/**
 * @brief Recursively walks a body/list container collecting paragraphs, tables, and inline images.
 * @param container The node whose children to walk (<office:text>, <text:list>, or <text:list-item>).
 * @param styles The resolved style map.
 * @param in_list Whether the current position is inside a <text:list> (marks paragraphs as bullets).
 * @param zip_bytes Pointer to the .odt file's raw bytes, used to resolve embedded image parts.
 * @param zip_len Length of `zip_bytes` in bytes.
 * @param out Receives one DocParagraph per <text:p>/<text:h> (plus a table anchor per <table:table>).
 * @param tables Receives one DocTable per <table:table> encountered.
 * @param images Receives one DocImage per resolvable embedded image encountered.
 */
void CollectOdtBodyParagraphs(const pugi::xml_node &container, const StyleMap &styles, bool in_list,
                               const unsigned char *zip_bytes, size_t zip_len, std::vector<DocParagraph> &out,
                               std::vector<DocTable> &tables, std::vector<DocImage> &images) {
    for (pugi::xml_node child : container.children()) {
        std::string name = child.name();
        if (name == "text:p" || name == "text:h") {
            DocParagraph p = ParseOdtParagraph(child, styles, in_list);
            std::string href;
            if (FindOdtImageHref(child, href) && !href.empty()) {
                std::string zip_path = (href[0] == '/') ? href.substr(1) : href;
                std::vector<unsigned char> img_bytes;
                if (ReadZipEntry(zip_bytes, zip_len, zip_path.c_str(), img_bytes)) {
                    ImageDoc probe;
                    if (probe.LoadFromMemory(img_bytes.data(), img_bytes.size())) {
                        DocImage img;
                        img.bytes.assign(img_bytes.begin(), img_bytes.end());
                        img.natural_w = probe.Width();
                        img.natural_h = probe.Height();
                        images.push_back(std::move(img));
                        p.image_ref = static_cast<int>(images.size()) - 1;
                    }
                }
            }
            out.push_back(std::move(p));
        } else if (name == "text:list") {
            CollectOdtBodyParagraphs(child, styles, true, zip_bytes, zip_len, out, tables, images);
        } else if (name == "text:list-item") {
            CollectOdtBodyParagraphs(child, styles, in_list, zip_bytes, zip_len, out, tables, images);
        } else if (name == "table:table") {
            tables.push_back(ParseOdtTable(child, styles));
            DocParagraph anchor;
            anchor.table_ref = static_cast<int>(tables.size()) - 1;
            out.push_back(std::move(anchor));
        }
        // Other body children (sections, TOC) skipped.
    }
}

}  // namespace

bool LoadOdtFromMemory(const unsigned char *bytes, size_t len, OfficeDoc &out, std::string &error) {
    std::vector<unsigned char> content_bytes;
    if (!ReadZipEntry(bytes, len, "content.xml", content_bytes)) {
        error = "not a valid .odt (missing content.xml)";
        return false;
    }
    pugi::xml_document content_doc;
    pugi::xml_parse_result result =
        content_doc.load_buffer(content_bytes.data(), content_bytes.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) {
        error = std::string("malformed content.xml: ") + result.description();
        return false;
    }

    // Merges content.xml's <office:automatic-styles> (direct/local
    // formatting -- most real-world formatting lives here) with
    // styles.xml's <office:styles> (named styles, referenced as
    // style:parent-style-name from the automatic ones) into one lookup
    // table. styles.xml is optional -- its absence just means named-style
    // fallback resolution silently finds nothing, not a load failure.
    StyleMap styles;
    if (pugi::xml_node auto_styles = content_doc.child("office:document-content").child("office:automatic-styles")) {
        CollectStyles(auto_styles, styles);
    }
    std::vector<unsigned char> styles_bytes;
    if (ReadZipEntry(bytes, len, "styles.xml", styles_bytes)) {
        pugi::xml_document styles_doc;
        if (styles_doc.load_buffer(styles_bytes.data(), styles_bytes.size(), pugi::parse_default, pugi::encoding_utf8)) {
            pugi::xml_node doc_styles = styles_doc.child("office:document-styles");
            if (pugi::xml_node office_styles = doc_styles.child("office:styles")) CollectStyles(office_styles, styles);
            if (pugi::xml_node auto2 = doc_styles.child("office:automatic-styles")) CollectStyles(auto2, styles);
        }
    }

    pugi::xml_node body = content_doc.child("office:document-content").child("office:body").child("office:text");
    if (!body) {
        error = "content.xml has no <office:text> body";
        return false;
    }
    out.paragraphs.clear();
    out.tables.clear();
    out.images.clear();
    out.source_format = "odt";
    CollectOdtBodyParagraphs(body, styles, false, bytes, len, out.paragraphs, out.tables, out.images);
    if (out.paragraphs.empty()) out.paragraphs.push_back(DocParagraph{});  // never render a zero-paragraph doc
    return true;
}

// ============================================================================
// ODT save-back
// ============================================================================

namespace {

// Unlike DOCX's inline w:rPr toggles, ODF formatting is always a named
// style reference -- these two helpers get-or-create one
// <style:style style:family="text"|"paragraph"> per distinct format/
// alignment combination actually used (cached by a key string -- widened
// from a packed-bitfield int once DocFormat grew fields (font family/size/
// color) an int can't cleanly encode), so a document with many runs of the
// same formatting doesn't grow one style per run.
/**
 * @brief Builds a string key from a DocFormat's fields, for deduplicating generated text styles.
 * @param fmt The format to encode.
 * @return A compact string uniquely encoding the fields relevant to an ODF text style.
 */
std::string DocFormatCacheKey(const DocFormat &fmt) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d%d%d%d%d%d%d%d%.1f%d%d%d%d%d%d", fmt.bold, fmt.italic, fmt.underline,
                  fmt.strike, fmt.superscript, fmt.subscript, fmt.math, static_cast<int>(fmt.font_family),
                  static_cast<double>(fmt.font_size_pt), fmt.has_color, fmt.color_r, fmt.color_g, fmt.has_highlight,
                  fmt.highlight_r, fmt.highlight_g);
    return std::string(buf);
}

/**
 * @brief Looks up (or creates and registers) a <style:style style:family="text"> for a given DocFormat.
 * @param auto_styles The <office:automatic-styles> node to append a new style onto, if needed.
 * @param cache Format-key -> style-name cache, checked first and updated on creation.
 * @param fmt The format to represent as a text style (returns "" for a default/empty format).
 * @param counter Running counter used to generate a unique style name, incremented on creation.
 * @return The style name to reference via text:style-name, or "" if `fmt` is the default format.
 */
std::string GetOrCreateTextStyle(pugi::xml_node &auto_styles, std::unordered_map<std::string, std::string> &cache,
                                  const DocFormat &fmt, int &counter) {
    if (fmt == DocFormat{}) return "";
    std::string key = DocFormatCacheKey(fmt);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::string name = "MepT" + std::to_string(++counter);
    pugi::xml_node style = auto_styles.append_child("style:style");
    style.append_attribute("style:name").set_value(name.c_str());
    style.append_attribute("style:family").set_value("text");
    pugi::xml_node tp = style.append_child("style:text-properties");
    if (fmt.font_family != OfficeFontFamily::Sans) {
        const char *fam = fmt.font_family == OfficeFontFamily::Serif ? "Liberation Serif" : "Liberation Mono";
        tp.append_attribute("style:font-name").set_value(fam);
        tp.append_attribute("fo:font-family").set_value(fam);
    }
    if (fmt.bold) tp.append_attribute("fo:font-weight").set_value("bold");
    if (fmt.italic) tp.append_attribute("fo:font-style").set_value("italic");
    if (fmt.underline) tp.append_attribute("style:text-underline-style").set_value("solid");
    if (fmt.strike) tp.append_attribute("style:text-line-through-style").set_value("solid");
    if (fmt.superscript) tp.append_attribute("style:text-position").set_value("super 58%");
    else if (fmt.subscript) tp.append_attribute("style:text-position").set_value("sub 58%");
    if (fmt.font_size_pt > 0.0f) {
        char sz[16];
        std::snprintf(sz, sizeof(sz), "%.1fpt", static_cast<double>(fmt.font_size_pt));
        tp.append_attribute("fo:font-size").set_value(sz);
    }
    if (fmt.has_color) {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", fmt.color_r, fmt.color_g, fmt.color_b);
        tp.append_attribute("fo:color").set_value(hex);
    }
    if (fmt.has_highlight) {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", fmt.highlight_r, fmt.highlight_g, fmt.highlight_b);
        tp.append_attribute("fo:background-color").set_value(hex);
    }
    cache[key] = name;
    return name;
}

/**
 * @brief Looks up (or creates and registers) a <style:style style:family="paragraph"> for a given alignment.
 * @param auto_styles The <office:automatic-styles> node to append a new style onto, if needed.
 * @param cache Alignment -> style-name cache, checked first and updated on creation.
 * @param align The alignment to represent as a paragraph style (returns "" for Left, ODF's default).
 * @param counter Running counter used to generate a unique style name, incremented on creation.
 * @return The style name to reference via text:style-name, or "" if `align` is Left.
 */
std::string GetOrCreateParaStyle(pugi::xml_node &auto_styles, std::unordered_map<int, std::string> &cache,
                                  DocParagraph::Align align, int &counter) {
    if (align == DocParagraph::Align::Left) return "";
    int key = static_cast<int>(align);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::string name = "MepP" + std::to_string(++counter);
    pugi::xml_node style = auto_styles.append_child("style:style");
    style.append_attribute("style:name").set_value(name.c_str());
    style.append_attribute("style:family").set_value("paragraph");
    pugi::xml_node pp = style.append_child("style:paragraph-properties");
    const char *val =
        align == DocParagraph::Align::Center ? "center" : align == DocParagraph::Align::Right ? "end" : "justify";
    pp.append_attribute("fo:text-align").set_value(val);
    cache[key] = name;
    return name;
}

// Builds `p_node`'s attributes/children from `p`: an alignment automatic
// style referenced via text:style-name (bullets are dropped on save --
// see SaveOdtToMemory's own comment), then a <text:span>-wrapped (or bare,
// for the default format) segment per format run, splitting embedded
// '\t'/'\n' into <text:tab/>/<text:line-break/> siblings -- the reverse of
// CollectOdtInline's mapping.
/**
 * @brief Builds a <text:p>/<text:h> node's alignment attribute and format-run children from a DocParagraph.
 * @param p The paragraph to serialize.
 * @param auto_styles The <office:automatic-styles> node passed through to style get-or-create helpers.
 * @param text_style_cache Format-key -> style-name cache for text styles.
 * @param text_counter Running counter for generating unique text style names.
 * @param para_style_cache Alignment -> style-name cache for paragraph styles.
 * @param para_counter Running counter for generating unique paragraph style names.
 * @param p_node The <text:p> or <text:h> XML node to append attributes/children onto.
 */
void SerializeOdtParagraph(const DocParagraph &p, pugi::xml_node &auto_styles,
                            std::unordered_map<std::string, std::string> &text_style_cache, int &text_counter,
                            std::unordered_map<int, std::string> &para_style_cache, int &para_counter,
                            pugi::xml_node &p_node) {
    std::string pstyle = GetOrCreateParaStyle(auto_styles, para_style_cache, p.align, para_counter);
    if (!pstyle.empty()) p_node.append_attribute("text:style-name").set_value(pstyle.c_str());

    /**
     * @brief Appends one run covering [s,e) of `p.text` as a <text:span> (or bare text), with tab/break splitting.
     * @param s Start offset into `p.text`.
     * @param e End offset into `p.text` (no-op if e<=s).
     * @param fmt The format to render as this run's text style, if non-default.
     */
    auto emit_run = [&](int s, int e, const DocFormat &fmt) {
        if (e <= s) return;
        std::string tstyle = GetOrCreateTextStyle(auto_styles, text_style_cache, fmt, text_counter);
        pugi::xml_node parent = p_node;
        if (!tstyle.empty()) {
            parent = p_node.append_child("text:span");
            parent.append_attribute("text:style-name").set_value(tstyle.c_str());
        }
        size_t seg_start = static_cast<size_t>(s);
        for (size_t i = static_cast<size_t>(s); i <= static_cast<size_t>(e); i++) {
            bool at_end = i == static_cast<size_t>(e);
            bool is_tab = !at_end && p.text[i] == '\t';
            bool is_br = !at_end && p.text[i] == '\n';
            if (is_tab || is_br || at_end) {
                if (i > seg_start) {
                    parent.append_child(pugi::node_pcdata).set_value(p.text.substr(seg_start, i - seg_start).c_str());
                }
                if (is_tab) parent.append_child("text:tab");
                else if (is_br) parent.append_child("text:line-break");
                seg_start = i + 1;
            }
        }
    };

    int pos = 0;
    for (const DocSpan &sp : p.spans) {
        if (sp.start > pos) emit_run(pos, sp.start, DocFormat{});
        emit_run(sp.start, sp.end, sp.fmt);
        pos = std::max(pos, sp.end);
    }
    if (pos < static_cast<int>(p.text.size())) emit_run(pos, static_cast<int>(p.text.size()), DocFormat{});
}

// Builds a <table:table> from a DocTable, applying `cell_style_name` (a
// single shared style with visible borders, registered once by the caller
// -- see SaveOdtToMemory -- rather than per-cell, since DocTable has no
// per-cell style data of its own to preserve) to every cell so the grid
// mep itself always draws is visible when the file is reopened elsewhere
// too. Each cell gets exactly one <text:p> (ODF tolerates an empty one),
// with the same '\t'/'\n'-segmented plain-text emission SerializeOdtParagraph
// uses for a run's own text, but with no <text:span> since DocTable cells
// carry no formatting.
/**
 * @brief Builds a <table:table> node from a DocTable, applying a shared bordered cell style to every cell.
 * @param t The table to serialize.
 * @param table_node The <table:table> XML node to append children onto.
 * @param table_name The value for the table's table:name attribute.
 * @param cell_style_name The pre-registered table-cell style name to apply to every cell.
 */
void SerializeOdtTable(const DocTable &t, pugi::xml_node &table_node, const std::string &table_name,
                       const std::string &cell_style_name) {
    table_node.append_attribute("table:name").set_value(table_name.c_str());
    for (int c = 0; c < t.cols; c++) table_node.append_child("table:table-column");
    for (int r = 0; r < t.rows; r++) {
        pugi::xml_node tr = table_node.append_child("table:table-row");
        for (int c = 0; c < t.cols; c++) {
            pugi::xml_node tc = tr.append_child("table:table-cell");
            tc.append_attribute("table:style-name").set_value(cell_style_name.c_str());
            tc.append_attribute("office:value-type").set_value("string");
            pugi::xml_node p_node = tc.append_child("text:p");
            const std::string &txt = t.Cell(r, c);
            size_t seg_start = 0;
            for (size_t i = 0; i <= txt.size(); i++) {
                bool at_end = i == txt.size();
                bool is_tab = !at_end && txt[i] == '\t';
                bool is_br = !at_end && txt[i] == '\n';
                if (is_tab || is_br || at_end) {
                    if (i > seg_start) {
                        p_node.append_child(pugi::node_pcdata).set_value(txt.substr(seg_start, i - seg_start).c_str());
                    }
                    if (is_tab) p_node.append_child("text:tab");
                    else if (is_br) p_node.append_child("text:line-break");
                    seg_start = i + 1;
                }
            }
        }
    }
}

// Builds a <draw:frame><draw:image .../></draw:frame> inside `p_node` (the
// paragraph it's anchored to) referencing `href` (already the full
// package-root-relative zip path, e.g. "Pictures/mepimage1.png"). Width/
// height are converted px -> cm at a 96 DPI assumption, capped to 16cm
// wide (~6.3in, matching SerializeDocxDrawing's own 6in cap) -- the same
// "no per-image size model, always natural-size-capped-to-content-width"
// simplification DocImage's own scope note describes. text:anchor-type
// "as-char" flows the frame inline with the paragraph's own text, mirroring
// how DOCX's wp:inline anchors a drawing.
/**
 * @brief Builds a <draw:frame><draw:image .../></draw:frame> anchored inline inside a paragraph node.
 * @param img The image whose natural dimensions size the frame (converted px -> cm, capped to 16cm wide).
 * @param p_node The paragraph node to append the frame onto.
 * @param href The package-root-relative zip path to the image part (e.g. "Pictures/mepimage1.png").
 * @param frame_index Used to generate a unique draw:name for the frame.
 */
void SerializeOdtDrawFrame(const DocImage &img, pugi::xml_node &p_node, const std::string &href, int frame_index) {
    constexpr double kCmPerPx = 2.54 / 96.0;
    constexpr double kMaxWidthCm = 16.0;
    double w = static_cast<double>(img.natural_w) * kCmPerPx;
    double h = static_cast<double>(img.natural_h) * kCmPerPx;
    if (w > kMaxWidthCm && w > 0.0) {
        h = h * kMaxWidthCm / w;
        w = kMaxWidthCm;
    }
    char w_buf[32], h_buf[32];
    std::snprintf(w_buf, sizeof(w_buf), "%.3fcm", std::max(0.01, w));
    std::snprintf(h_buf, sizeof(h_buf), "%.3fcm", std::max(0.01, h));

    pugi::xml_node frame = p_node.append_child("draw:frame");
    frame.append_attribute("draw:name").set_value(("Image" + std::to_string(frame_index)).c_str());
    frame.append_attribute("svg:width").set_value(w_buf);
    frame.append_attribute("svg:height").set_value(h_buf);
    frame.append_attribute("text:anchor-type").set_value("as-char");
    pugi::xml_node image_node = frame.append_child("draw:image");
    image_node.append_attribute("xlink:href").set_value(href.c_str());
    image_node.append_attribute("xlink:type").set_value("simple");
    image_node.append_attribute("xlink:show").set_value("embed");
    image_node.append_attribute("xlink:actuate").set_value("onLoad");
}

}  // namespace

bool SaveOdtToMemory(const OfficeDoc &doc, const std::vector<unsigned char> &original_bytes,
                     std::vector<unsigned char> &out, std::string &error) {
    std::vector<unsigned char> xml_bytes;
    if (!ReadZipEntry(original_bytes.data(), original_bytes.size(), "content.xml", xml_bytes)) {
        error = "not a valid .odt (missing content.xml)";
        return false;
    }
    pugi::xml_document xml;
    pugi::xml_parse_result result =
        xml.load_buffer(xml_bytes.data(), xml_bytes.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) {
        error = std::string("malformed content.xml: ") + result.description();
        return false;
    }
    pugi::xml_node doc_content = xml.child("office:document-content");
    if (!doc_content) {
        error = "content.xml has no <office:document-content>";
        return false;
    }
    pugi::xml_node body_el = doc_content.child("office:body");
    if (!body_el) {
        error = "content.xml has no <office:body>";
        return false;
    }
    pugi::xml_node text_body = body_el.child("office:text");
    if (!text_body) {
        error = "content.xml has no <office:text>";
        return false;
    }
    pugi::xml_node auto_styles = doc_content.child("office:automatic-styles");
    if (!auto_styles) auto_styles = doc_content.insert_child_before("office:automatic-styles", body_el);

    // Removes every existing paragraph/heading/list/table child -- a
    // bulleted list's <text:list> wrapping is dropped here (v1 doesn't
    // track a list-style definition to regenerate it, so a round-tripped
    // bullet paragraph becomes a plain paragraph, a known, documented loss
    // rather than emitting a <text:list> ODF can't resolve a style for).
    for (pugi::xml_node child = text_body.first_child(); child;) {
        pugi::xml_node next = child.next_sibling();
        std::string name = child.name();
        if (name == "text:p" || name == "text:h" || name == "text:list" || name == "table:table") {
            text_body.remove_child(child);
        }
        child = next;
    }

    // One shared bordered cell style for every table in the document (see
    // SerializeOdtTable's own comment) -- registered once here rather than
    // per-table/per-cell since DocTable carries no per-cell style to
    // preserve. Only added when the document actually has a table, so a
    // table-less document's automatic-styles stays untouched.
    std::string table_cell_style_name;
    if (!doc.tables.empty()) {
        table_cell_style_name = "MepTableCellBordered";
        pugi::xml_node cell_style = auto_styles.append_child("style:style");
        cell_style.append_attribute("style:name").set_value(table_cell_style_name.c_str());
        cell_style.append_attribute("style:family").set_value("table-cell");
        pugi::xml_node tc_props = cell_style.append_child("style:table-cell-properties");
        tc_props.append_attribute("fo:border").set_value("0.5pt solid #000000");
    }

    // Every image in doc.images gets a fresh Pictures/ part this save, in
    // index order, "mep"-prefixed for the same collision-avoidance reason
    // SaveDocxToMemory's word/media/ naming is -- see its own comment for
    // why a pre-existing (unchanged) image is simply re-added rather than
    // reusing its original part.
    struct NewImage {
        std::string href, filename;
        const DocImage *img;
    };
    std::vector<NewImage> new_images;
    new_images.reserve(doc.images.size());
    for (size_t i = 0; i < doc.images.size(); i++) {
        std::string ext = SniffImageExtension(doc.images[i].bytes);
        std::string filename = "mepimage" + std::to_string(i + 1) + "." + ext;
        new_images.push_back({"Pictures/" + filename, filename, &doc.images[i]});
    }

    std::unordered_map<std::string, std::string> text_style_cache;
    std::unordered_map<int, std::string> para_style_cache;
    int text_counter = 0, para_counter = 0, table_counter = 0, image_counter = 0;
    for (const DocParagraph &p : doc.paragraphs) {
        pugi::xml_node p_node = text_body.append_child(p.heading_level > 0 ? "text:h" : "text:p");
        if (p.heading_level > 0) {
            p_node.append_attribute("text:outline-level").set_value(std::clamp(p.heading_level, 1, 6));
        }
        SerializeOdtParagraph(p, auto_styles, text_style_cache, text_counter, para_style_cache, para_counter, p_node);
        if (p.table_ref >= 0 && p.table_ref < static_cast<int>(doc.tables.size())) {
            pugi::xml_node table_node = text_body.append_child("table:table");
            std::string table_name = "Table" + std::to_string(++table_counter);
            SerializeOdtTable(doc.tables[static_cast<size_t>(p.table_ref)], table_node, table_name,
                              table_cell_style_name);
        }
        if (p.image_ref >= 0 && p.image_ref < static_cast<int>(new_images.size())) {
            const NewImage &ni = new_images[static_cast<size_t>(p.image_ref)];
            SerializeOdtDrawFrame(*ni.img, p_node, ni.href, ++image_counter);
        }
    }

    std::ostringstream ss;
    xml.save(ss, "", pugi::format_raw);
    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("content.xml", ss.str());

    if (!new_images.empty()) {
        // META-INF/manifest.xml: every part in an ODF package (besides the
        // package root's own "/" entry) must be declared here with its
        // media type -- add one <manifest:file-entry> per new Pictures/
        // part.
        std::vector<unsigned char> manifest_bytes;
        bool have_manifest =
            ReadZipEntry(original_bytes.data(), original_bytes.size(), "META-INF/manifest.xml", manifest_bytes);
        pugi::xml_document manifest_doc;
        pugi::xml_node manifest_root;
        if (have_manifest && manifest_doc.load_buffer(manifest_bytes.data(), manifest_bytes.size(),
                                                       pugi::parse_default, pugi::encoding_utf8)) {
            manifest_root = manifest_doc.child("manifest:manifest");
        }
        if (manifest_root) {
            for (const NewImage &ni : new_images) {
                pugi::xml_node entry = manifest_root.append_child("manifest:file-entry");
                entry.append_attribute("manifest:full-path").set_value(ni.href.c_str());
                std::string ext = ni.filename.substr(ni.filename.find_last_of('.') + 1);
                entry.append_attribute("manifest:media-type").set_value(MimeForImageExt(ext).c_str());
            }
            std::ostringstream mss;
            manifest_doc.save(mss, "", pugi::format_raw);
            entries.emplace_back("META-INF/manifest.xml", mss.str());
        }

        for (const NewImage &ni : new_images) {
            entries.emplace_back(ni.href, ni.img->bytes);
        }
    }

    return WriteZipReplacingEntries(original_bytes.data(), original_bytes.size(), entries, out, error);
}
