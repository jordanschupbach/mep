#include "office_doc.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#include "miniz.h"
#include "pugixml.hpp"

// ============================================================================
// Span-editing primitives
// ============================================================================

namespace {

// Delete-range clamp: maps a span endpoint through the removal of [a,b).
// x<=a unaffected; a<x<b collapses to a; x>=b shifts left by (b-a).
int ClampThroughDelete(int x, int a, int b) {
    if (x <= a) return x;
    if (x < b) return a;
    return x - (b - a);
}

}  // namespace

void ApplyDeleteToParagraph(DocParagraph &p, int a, int b) {
    a = std::clamp(a, 0, static_cast<int>(p.text.size()));
    b = std::clamp(b, 0, static_cast<int>(p.text.size()));
    if (b <= a) return;
    p.text.erase(p.text.begin() + a, p.text.begin() + b);
    std::vector<DocSpan> out;
    for (auto &s : p.spans) {
        bool was_empty = (s.start == s.end);
        int ns = ClampThroughDelete(s.start, a, b);
        int ne = ClampThroughDelete(s.end, a, b);
        if (ns == ne && !was_empty) continue;
        out.push_back({ns, ne, s.fmt});
    }
    p.spans = std::move(out);
}

void ApplyInsertToParagraph(DocParagraph &p, int col, const std::string &inserted) {
    int L = static_cast<int>(inserted.size());
    if (L == 0) return;
    col = std::clamp(col, 0, static_cast<int>(p.text.size()));
    p.text.insert(p.text.begin() + col, inserted.begin(), inserted.end());
    for (auto &s : p.spans) {
        if (s.end <= col) {
            // Unchanged: entirely before the insertion point.
        } else if (s.start >= col) {
            // Entirely at/after the insertion point: shift both, so a
            // span starting exactly at col ends up starting after the
            // inserted text rather than "growing" to include it.
            s.start += L;
            s.end += L;
        } else {
            // s.start < col < s.end: insertion lands strictly inside --
            // extend. (col==s.end is covered by the first branch above,
            // so no "sticky" trailing extension either.)
            s.end += L;
        }
    }
}

DocParagraph SplitParagraphAt(DocParagraph &p, int col) {
    col = std::clamp(col, 0, static_cast<int>(p.text.size()));
    DocParagraph second;
    second.align = p.align;
    second.heading_level = p.heading_level;
    second.bullet = p.bullet;
    second.text = p.text.substr(static_cast<size_t>(col));
    p.text.resize(static_cast<size_t>(col));

    std::vector<DocSpan> first_spans, second_spans;
    for (auto &s : p.spans) {
        if (s.end <= col) {
            first_spans.push_back(s);
        } else if (s.start >= col) {
            second_spans.push_back({s.start - col, s.end - col, s.fmt});
        } else {
            // Straddles the cut: split into two spans, same format.
            first_spans.push_back({s.start, col, s.fmt});
            second_spans.push_back({0, s.end - col, s.fmt});
        }
    }
    p.spans = std::move(first_spans);
    second.spans = std::move(second_spans);
    return second;
}

void MergeParagraphs(DocParagraph &p, const DocParagraph &next) {
    int base = static_cast<int>(p.text.size());
    p.text += next.text;
    for (auto &s : next.spans) p.spans.push_back({s.start + base, s.end + base, s.fmt});
    // p keeps its own align/heading_level/bullet (matches Word's own merge behavior).
}

void CoalesceSpans(std::vector<DocSpan> &spans) {
    std::sort(spans.begin(), spans.end(), [](const DocSpan &x, const DocSpan &y) { return x.start < y.start; });
    std::vector<DocSpan> out;
    for (auto &s : spans) {
        if (s.start == s.end) continue;
        if (!out.empty() && out.back().fmt == s.fmt && s.start <= out.back().end) {
            out.back().end = std::max(out.back().end, s.end);
        } else {
            out.push_back(s);
        }
    }
    spans = std::move(out);
}

DocFormat FormatAt(const DocParagraph &p, int col) {
    for (auto &s : p.spans) {
        if (col >= s.start && col < s.end) return s.fmt;
    }
    return DocFormat{};
}

void ToggleFormatOverRange(DocParagraph &p, int a, int b, bool DocFormat::*field) {
    a = std::clamp(a, 0, static_cast<int>(p.text.size()));
    b = std::clamp(b, 0, static_cast<int>(p.text.size()));
    if (b <= a) return;

    // Uniformly "on" already? Walk spans left-to-right across [a,b);
    // any gap (implicit default/off) or any overlapping span with the
    // field off makes it "not uniformly on".
    bool all_on = true;
    {
        int cursor = a;
        for (auto &s : p.spans) {
            if (s.end <= cursor) continue;
            if (s.start >= b) break;
            if (s.start > cursor) { all_on = false; break; }
            if (!(s.fmt.*field)) { all_on = false; break; }
            cursor = std::min(s.end, b);
        }
        if (all_on && cursor < b) all_on = false;
    }
    bool new_value = !all_on;

    // Keep everything strictly outside [a,b) untouched (including the
    // non-overlapping remainder of a span that only partially overlaps).
    std::vector<DocSpan> out;
    for (auto &s : p.spans) {
        if (s.end <= a || s.start >= b) { out.push_back(s); continue; }
        if (s.start < a) out.push_back({s.start, a, s.fmt});
        if (s.end > b) out.push_back({b, s.end, s.fmt});
    }

    // Rebuild [a,b) itself by walking the original segmentation (spans +
    // implicit gaps) again, re-emitting each segment with `field` forced
    // to new_value while preserving every OTHER field from whatever was
    // there -- a flat single replacement span would lose e.g. "was
    // bold+italic, toggling italic off should stay bold".
    int cursor = a;
    for (auto &s : p.spans) {
        if (s.end <= a || s.start >= b) continue;
        int seg_s = std::max(s.start, a), seg_e = std::min(s.end, b);
        if (seg_s > cursor) {
            DocFormat gap_fmt;
            gap_fmt.*field = new_value;
            out.push_back({cursor, seg_s, gap_fmt});
        }
        DocFormat f = s.fmt;
        f.*field = new_value;
        out.push_back({seg_s, seg_e, f});
        cursor = seg_e;
    }
    if (cursor < b) {
        DocFormat gap_fmt;
        gap_fmt.*field = new_value;
        out.push_back({cursor, b, gap_fmt});
    }

    std::sort(out.begin(), out.end(), [](const DocSpan &x, const DocSpan &y) { return x.start < y.start; });
    p.spans = std::move(out);
    CoalesceSpans(p.spans);
}

// ============================================================================
// File path helpers
// ============================================================================

namespace {
std::string LowerExt(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}
}  // namespace

bool IsDocxPath(const std::string &path) { return LowerExt(path) == "docx"; }
bool IsOdtPath(const std::string &path) { return LowerExt(path) == "odt"; }

// ============================================================================
// DOCX parsing (word/document.xml via pugixml, word/document.xml's bytes
// extracted from the .docx ZIP container via miniz)
// ============================================================================

bool ReadZipEntry(const unsigned char *zip_bytes, size_t zip_len, const char *entry_name,
                   std::vector<unsigned char> &out) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zip_bytes, zip_len, 0)) return false;
    int idx = mz_zip_reader_locate_file(&zip, entry_name, nullptr, 0);
    if (idx < 0) {
        mz_zip_reader_end(&zip);
        return false;
    }
    size_t size = 0;
    void *data = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(idx), &size, 0);
    mz_zip_reader_end(&zip);
    if (!data) return false;
    out.assign(static_cast<unsigned char *>(data), static_cast<unsigned char *>(data) + size);
    mz_free(data);
    return true;
}

bool WriteZipReplacingEntry(const unsigned char *orig_bytes, size_t orig_len, const char *entry_name,
                             const std::string &new_content, std::vector<unsigned char> &out, std::string &error) {
    mz_zip_archive reader{};
    if (!mz_zip_reader_init_mem(&reader, orig_bytes, orig_len, 0)) {
        error = "not a valid zip archive";
        return false;
    }
    mz_zip_archive writer{};
    if (!mz_zip_writer_init_heap(&writer, 0, 0)) {
        mz_zip_reader_end(&reader);
        error = "failed to initialize zip writer";
        return false;
    }
    mz_uint n = mz_zip_reader_get_num_files(&reader);
    bool ok = true;
    bool replaced = false;
    for (mz_uint i = 0; i < n && ok; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&reader, i, &stat)) {
            ok = false;
            break;
        }
        if (std::strcmp(stat.m_filename, entry_name) == 0) {
            ok = mz_zip_writer_add_mem(&writer, entry_name, new_content.data(), new_content.size(),
                                        MZ_DEFAULT_COMPRESSION);
            replaced = true;
        } else {
            ok = mz_zip_writer_add_from_zip_reader(&writer, &reader, i);
        }
    }
    if (ok && !replaced) {
        ok = mz_zip_writer_add_mem(&writer, entry_name, new_content.data(), new_content.size(), MZ_DEFAULT_COMPRESSION);
    }
    void *heap_data = nullptr;
    size_t heap_size = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&writer, &heap_data, &heap_size);
    mz_zip_writer_end(&writer);
    mz_zip_reader_end(&reader);
    if (!ok) {
        error = "failed to write zip archive";
        if (heap_data) mz_free(heap_data);
        return false;
    }
    out.assign(static_cast<unsigned char *>(heap_data), static_cast<unsigned char *>(heap_data) + heap_size);
    mz_free(heap_data);
    return true;
}

namespace {

// Maps a DOCX heading paragraph style name ("Heading1".."Heading6", any
// casing/spacing DOCX producers commonly use) to a 1-6 level, or 0 if it
// isn't recognized as a heading -- deliberately just name recognition,
// not resolving the full w:basedOn style-cascade (out of v1 scope).
int DocxHeadingLevelFromStyleName(const std::string &style_id) {
    // Common forms: "Heading1", "Heading 1", "heading1".
    std::string s;
    for (char c : style_id)
        if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    const std::string prefix = "heading";
    if (s.rfind(prefix, 0) != 0) return 0;
    std::string digits = s.substr(prefix.size());
    if (digits.empty() || digits.size() > 1 || !std::isdigit(static_cast<unsigned char>(digits[0]))) return 0;
    int level = digits[0] - '0';
    return (level >= 1 && level <= 6) ? level : 0;
}

// True if a <w:b/>/<w:i/>/<w:u/>/<w:strike/> toggle element is "on":
// present with no w:val, or w:val is a recognized true-ish token. OOXML
// booleans can be explicitly disabled (w:val="false"/"0"); anything else
// present is treated as on, matching how these elements are used in
// practice by real producers.
bool OoxmlBoolOn(const pugi::xml_node &toggle_node) {
    if (!toggle_node) return false;
    pugi::xml_attribute val = toggle_node.attribute("w:val");
    if (!val) return true;
    std::string v = val.as_string();
    return !(v == "false" || v == "0" || v == "off");
}

// Appends run text (handling <w:t>, <w:tab/>, <w:br/>) into `text`,
// returning the appended length so the caller can build a DocSpan for it
// if the run carries any formatting.
int AppendDocxRunText(const pugi::xml_node &run, std::string &text) {
    int start = static_cast<int>(text.size());
    for (pugi::xml_node child : run.children()) {
        std::string name = child.name();
        if (name == "w:t") {
            text += child.text().get();
        } else if (name == "w:tab") {
            text.push_back('\t');
        } else if (name == "w:br" || name == "w:cr") {
            text.push_back('\n');
        }
        // Other run children (drawings, footnote refs, etc.) are silently
        // skipped -- out of v1 scope, not a text-loss bug since they carry
        // no plain-text content this model represents.
    }
    return static_cast<int>(text.size()) - start;
}

// Walks a <w:p>'s children collecting run text/spans -- recurses into
// <w:hyperlink> (which wraps <w:r> runs) so hyperlinked text isn't
// silently dropped; hyperlink *behavior* (click/navigate) isn't modeled.
void CollectDocxParagraphRuns(const pugi::xml_node &p_node, DocParagraph &out) {
    for (pugi::xml_node child : p_node.children()) {
        std::string name = child.name();
        if (name == "w:r") {
            pugi::xml_node rpr = child.child("w:rPr");
            DocFormat fmt;
            if (rpr) {
                fmt.bold = OoxmlBoolOn(rpr.child("w:b"));
                fmt.italic = OoxmlBoolOn(rpr.child("w:i"));
                fmt.underline = static_cast<bool>(rpr.child("w:u"));  // presence-only: any w:u val != "none"
                if (fmt.underline) {
                    std::string uval = rpr.child("w:u").attribute("w:val").as_string();
                    fmt.underline = (uval != "none");
                }
                fmt.strike = OoxmlBoolOn(rpr.child("w:strike"));
            }
            int start = static_cast<int>(out.text.size());
            int len = AppendDocxRunText(child, out.text);
            if (len > 0 && (fmt.bold || fmt.italic || fmt.underline || fmt.strike)) {
                out.spans.push_back({start, start + len, fmt});
            }
        } else if (name == "w:hyperlink") {
            CollectDocxParagraphRuns(child, out);  // recurse: hyperlink wraps <w:r> runs
        }
        // Other paragraph children (bookmarks, fields, etc.) skipped.
    }
}

void ParseDocxParagraph(const pugi::xml_node &p_node, DocParagraph &out) {
    pugi::xml_node ppr = p_node.child("w:pPr");
    if (ppr) {
        if (pugi::xml_node pstyle = ppr.child("w:pStyle")) {
            out.heading_level = DocxHeadingLevelFromStyleName(pstyle.attribute("w:val").as_string());
        }
        if (pugi::xml_node jc = ppr.child("w:jc")) {
            std::string v = jc.attribute("w:val").as_string();
            if (v == "center") out.align = DocParagraph::Align::Center;
            else if (v == "right" || v == "end") out.align = DocParagraph::Align::Right;
            else if (v == "both" || v == "distribute") out.align = DocParagraph::Align::Justify;
        }
        if (ppr.child("w:numPr")) out.bullet = true;
    }
    CollectDocxParagraphRuns(p_node, out);
    CoalesceSpans(out.spans);
}

}  // namespace

bool LoadDocxFromMemory(const unsigned char *bytes, size_t len, OfficeDoc &out, std::string &error) {
    std::vector<unsigned char> xml_bytes;
    if (!ReadZipEntry(bytes, len, "word/document.xml", xml_bytes)) {
        error = "not a valid .docx (missing word/document.xml)";
        return false;
    }
    pugi::xml_document doc;
    pugi::xml_parse_result result =
        doc.load_buffer(xml_bytes.data(), xml_bytes.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) {
        error = std::string("malformed word/document.xml: ") + result.description();
        return false;
    }
    pugi::xml_node body = doc.child("w:document").child("w:body");
    if (!body) {
        error = "word/document.xml has no <w:body>";
        return false;
    }
    out.paragraphs.clear();
    out.source_format = "docx";
    for (pugi::xml_node p_node : body.children("w:p")) {
        DocParagraph p;
        ParseDocxParagraph(p_node, p);
        out.paragraphs.push_back(std::move(p));
    }
    if (out.paragraphs.empty()) out.paragraphs.push_back(DocParagraph{});  // never render a zero-paragraph doc
    return true;
}

// ============================================================================
// DOCX save-back
// ============================================================================

namespace {

// Builds `p_node`'s children (<w:pPr>, then a <w:r> per format run) from
// `p`. Walks `p.spans` plus the implicit default-format gaps between them
// -- the same segmentation BuildOfficeFormatRuns (main.cpp) computes for
// rendering, duplicated here in miniature rather than shared across the
// raylib/non-raylib boundary those two files deliberately keep (see
// office_doc.h's own top-of-file comment).
void SerializeDocxParagraph(const DocParagraph &p, pugi::xml_node &p_node) {
    if (p.heading_level > 0 || p.align != DocParagraph::Align::Left) {
        pugi::xml_node ppr = p_node.append_child("w:pPr");
        if (p.heading_level > 0) {
            pugi::xml_node pstyle = ppr.append_child("w:pStyle");
            pstyle.append_attribute("w:val").set_value(("Heading" + std::to_string(p.heading_level)).c_str());
        }
        if (p.align != DocParagraph::Align::Left) {
            pugi::xml_node jc = ppr.append_child("w:jc");
            const char *val = p.align == DocParagraph::Align::Center ? "center"
                              : p.align == DocParagraph::Align::Right ? "right" : "both";
            jc.append_attribute("w:val").set_value(val);
        }
    }

    auto emit_run = [&](int s, int e, const DocFormat &fmt) {
        if (e <= s) return;
        pugi::xml_node r = p_node.append_child("w:r");
        if (fmt.bold || fmt.italic || fmt.underline || fmt.strike) {
            pugi::xml_node rpr = r.append_child("w:rPr");
            if (fmt.bold) rpr.append_child("w:b");
            if (fmt.italic) rpr.append_child("w:i");
            if (fmt.underline) rpr.append_child("w:u").append_attribute("w:val").set_value("single");
            if (fmt.strike) rpr.append_child("w:strike");
        }
        // Splits on embedded '\t'/'\n' -- the reverse of AppendDocxRunText's
        // <w:tab/>/<w:br/> -> character mapping -- so multiple sibling
        // elements share this one run's <w:rPr>, matching how a real
        // producer represents "bold text<tab/>more bold text".
        size_t seg_start = static_cast<size_t>(s);
        for (size_t i = static_cast<size_t>(s); i <= static_cast<size_t>(e); i++) {
            bool at_end = i == static_cast<size_t>(e);
            bool is_tab = !at_end && p.text[i] == '\t';
            bool is_br = !at_end && p.text[i] == '\n';
            if (is_tab || is_br || at_end) {
                if (i > seg_start) {
                    pugi::xml_node t = r.append_child("w:t");
                    t.append_attribute("xml:space").set_value("preserve");
                    t.text().set(p.text.substr(seg_start, i - seg_start).c_str());
                }
                if (is_tab) r.append_child("w:tab");
                else if (is_br) r.append_child("w:br");
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
    // An empty paragraph (p.text empty, no spans) legitimately ends up
    // with zero <w:r> children -- Word tolerates a bare <w:p/>.
}

}  // namespace

bool SaveDocxToMemory(const OfficeDoc &doc, const std::vector<unsigned char> &original_bytes,
                      std::vector<unsigned char> &out, std::string &error) {
    std::vector<unsigned char> xml_bytes;
    if (!ReadZipEntry(original_bytes.data(), original_bytes.size(), "word/document.xml", xml_bytes)) {
        error = "not a valid .docx (missing word/document.xml)";
        return false;
    }
    pugi::xml_document xml;
    pugi::xml_parse_result result =
        xml.load_buffer(xml_bytes.data(), xml_bytes.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) {
        error = std::string("malformed word/document.xml: ") + result.description();
        return false;
    }
    pugi::xml_node body = xml.child("w:document").child("w:body");
    if (!body) {
        error = "word/document.xml has no <w:body>";
        return false;
    }

    // Removes every existing <w:p>/<w:tbl> (a table is dropped -- never
    // represented in OfficeDoc to begin with, see the V1 scope exclusions),
    // remembering the first <w:sectPr> (page setup) as the insertion
    // anchor so freshly-built paragraphs land before it -- OOXML requires
    // w:sectPr, if present, to be w:body's last child.
    pugi::xml_node anchor;
    for (pugi::xml_node child = body.first_child(); child;) {
        pugi::xml_node next = child.next_sibling();
        std::string name = child.name();
        if (name == "w:p" || name == "w:tbl") {
            body.remove_child(child);
        } else if (name == "w:sectPr" && !anchor) {
            anchor = child;
        }
        child = next;
    }
    for (const DocParagraph &p : doc.paragraphs) {
        pugi::xml_node p_node = anchor ? body.insert_child_before("w:p", anchor) : body.append_child("w:p");
        SerializeDocxParagraph(p, p_node);
    }

    std::ostringstream ss;
    xml.save(ss, "", pugi::format_raw);
    return WriteZipReplacingEntry(original_bytes.data(), original_bytes.size(), "word/document.xml", ss.str(), out,
                                   error);
}
