#include "office_doc.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>

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
    bool has_align = false;
    DocParagraph::Align align = DocParagraph::Align::Left;
    std::string parent;
};

using StyleMap = std::unordered_map<std::string, OdtStyle>;

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
void CollectOdtInline(const pugi::xml_node &node, const StyleMap &styles, DocFormat base_fmt, DocParagraph &out) {
    for (pugi::xml_node child : node.children()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            int start = static_cast<int>(out.text.size());
            out.text += child.value();
            int len = static_cast<int>(out.text.size()) - start;
            if (len > 0 && (base_fmt.bold || base_fmt.italic || base_fmt.underline || base_fmt.strike)) {
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
    out.bullet = in_list;

    DocFormat base_fmt;
    base_fmt.bold = pstyle.has_bold && pstyle.bold;
    base_fmt.italic = pstyle.has_italic && pstyle.italic;
    base_fmt.underline = pstyle.has_underline && pstyle.underline;
    base_fmt.strike = pstyle.has_strike && pstyle.strike;
    CollectOdtInline(p_node, styles, base_fmt, out);
    CoalesceSpans(out.spans);
    return out;
}

// Walks <office:text> (and recursively <text:list>/<text:list-item>,
// v1: bullet-or-not only, no real numbering -- same simplification as
// DOCX's <w:numPr> handling) collecting one DocParagraph per <text:p>/
// <text:h>. Tables/sections/frames are skipped, not recursed into --
// out of v1 scope, and (unlike a raw ZIP-entry failure) skipping a
// single unsupported element is exactly the per-element tolerance this
// parser is meant to have, mirroring how a DOCX table/image is skipped
// while its surrounding paragraphs still render.
void CollectOdtBodyParagraphs(const pugi::xml_node &container, const StyleMap &styles, bool in_list,
                               std::vector<DocParagraph> &out) {
    for (pugi::xml_node child : container.children()) {
        std::string name = child.name();
        if (name == "text:p" || name == "text:h") {
            out.push_back(ParseOdtParagraph(child, styles, in_list));
        } else if (name == "text:list") {
            CollectOdtBodyParagraphs(child, styles, true, out);
        } else if (name == "text:list-item") {
            CollectOdtBodyParagraphs(child, styles, in_list, out);
        }
        // Other body children (tables, sections, frames, TOC) skipped.
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
    out.source_format = "odt";
    CollectOdtBodyParagraphs(body, styles, false, out.paragraphs);
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
// alignment combination actually used (cached by a small packed-bitfield
// key), so a document with many runs of the same formatting doesn't grow
// one style per run.
std::string GetOrCreateTextStyle(pugi::xml_node &auto_styles, std::unordered_map<int, std::string> &cache,
                                  const DocFormat &fmt, int &counter) {
    int key = (fmt.bold ? 1 : 0) | (fmt.italic ? 2 : 0) | (fmt.underline ? 4 : 0) | (fmt.strike ? 8 : 0);
    if (key == 0) return "";
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::string name = "MepT" + std::to_string(++counter);
    pugi::xml_node style = auto_styles.append_child("style:style");
    style.append_attribute("style:name").set_value(name.c_str());
    style.append_attribute("style:family").set_value("text");
    pugi::xml_node tp = style.append_child("style:text-properties");
    if (fmt.bold) tp.append_attribute("fo:font-weight").set_value("bold");
    if (fmt.italic) tp.append_attribute("fo:font-style").set_value("italic");
    if (fmt.underline) tp.append_attribute("style:text-underline-style").set_value("solid");
    if (fmt.strike) tp.append_attribute("style:text-line-through-style").set_value("solid");
    cache[key] = name;
    return name;
}

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
void SerializeOdtParagraph(const DocParagraph &p, pugi::xml_node &auto_styles,
                            std::unordered_map<int, std::string> &text_style_cache, int &text_counter,
                            std::unordered_map<int, std::string> &para_style_cache, int &para_counter,
                            pugi::xml_node &p_node) {
    std::string pstyle = GetOrCreateParaStyle(auto_styles, para_style_cache, p.align, para_counter);
    if (!pstyle.empty()) p_node.append_attribute("text:style-name").set_value(pstyle.c_str());

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

    // Removes every existing paragraph/heading/list/table child (a table
    // is dropped -- never represented in OfficeDoc to begin with, see the
    // V1 scope exclusions; a bulleted list's <text:list> wrapping is also
    // dropped here -- v1 doesn't track a list-style definition to
    // regenerate it, so a round-tripped bullet paragraph becomes a plain
    // paragraph, a known, documented loss rather than emitting a
    // <text:list> ODF can't resolve a style for).
    for (pugi::xml_node child = text_body.first_child(); child;) {
        pugi::xml_node next = child.next_sibling();
        std::string name = child.name();
        if (name == "text:p" || name == "text:h" || name == "text:list" || name == "table:table") {
            text_body.remove_child(child);
        }
        child = next;
    }

    std::unordered_map<int, std::string> text_style_cache, para_style_cache;
    int text_counter = 0, para_counter = 0;
    for (const DocParagraph &p : doc.paragraphs) {
        pugi::xml_node p_node = text_body.append_child(p.heading_level > 0 ? "text:h" : "text:p");
        if (p.heading_level > 0) {
            p_node.append_attribute("text:outline-level").set_value(std::clamp(p.heading_level, 1, 6));
        }
        SerializeOdtParagraph(p, auto_styles, text_style_cache, text_counter, para_style_cache, para_counter, p_node);
    }

    std::ostringstream ss;
    xml.save(ss, "", pugi::format_raw);
    return WriteZipReplacingEntry(original_bytes.data(), original_bytes.size(), "content.xml", ss.str(), out, error);
}
