#include "office_doc.h"
#include "image_doc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <unordered_map>

#include "miniz.h"
#include "pugixml.hpp"

// ============================================================================
// Span-editing primitives
// ============================================================================

namespace {

// Delete-range clamp: maps a span endpoint through the removal of [a,b).
// x<=a unaffected; a<x<b collapses to a; x>=b shifts left by (b-a).
/**
 * @brief Maps one span endpoint through the removal of a deleted range.
 * @param x The endpoint to map.
 * @param a Start of the deleted range.
 * @param b End of the deleted range.
 * @return `x` if x<=a; `a` if a<x<b; `x-(b-a)` if x>=b.
 */
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
    second.list_kind = p.list_kind;
    // A table/image anchor stays with whichever half still ends at the
    // paragraph's own end (always the second half, since an anchor renders
    // *after* the paragraph's text -- splitting mid-text never moves what
    // comes after the whole paragraph); the first half is a genuinely new
    // paragraph boundary with nothing anchored to it yet.
    second.table_ref = p.table_ref;
    second.image_ref = p.image_ref;
    p.table_ref = -1;
    p.image_ref = -1;
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
    // p keeps its own align/heading_level/list_kind (matches Word's own
    // merge behavior). `next`'s own table/image anchor (if any) carries
    // over only when p doesn't already have one of its own -- the rare
    // case of merging two paragraphs that both anchor something loses
    // `next`'s anchor rather than either one silently overwriting the
    // other's index into OfficeDoc::tables/images.
    if (p.table_ref < 0) p.table_ref = next.table_ref;
    if (p.image_ref < 0) p.image_ref = next.image_ref;
}

void CoalesceSpans(std::vector<DocSpan> &spans) {
    // Comparator: orders spans by ascending start offset.
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

    // Comparator: orders the rebuilt segments by ascending start offset.
    std::sort(out.begin(), out.end(), [](const DocSpan &x, const DocSpan &y) { return x.start < y.start; });
    p.spans = std::move(out);
    CoalesceSpans(p.spans);
}

void SetFormatFieldOverRange(DocParagraph &p, int a, int b, const std::function<void(DocFormat &)> &apply) {
    a = std::clamp(a, 0, static_cast<int>(p.text.size()));
    b = std::clamp(b, 0, static_cast<int>(p.text.size()));
    if (b <= a) return;

    // Same "keep everything strictly outside [a,b) untouched, clip a span
    // that only partially overlaps" shape as ToggleFormatOverRange, minus
    // the "detect uniform on/off first" step that only makes sense for a
    // boolean.
    std::vector<DocSpan> out;
    for (auto &s : p.spans) {
        if (s.end <= a || s.start >= b) { out.push_back(s); continue; }
        if (s.start < a) out.push_back({s.start, a, s.fmt});
        if (s.end > b) out.push_back({b, s.end, s.fmt});
    }

    int cursor = a;
    for (auto &s : p.spans) {
        if (s.end <= a || s.start >= b) continue;
        int seg_s = std::max(s.start, a), seg_e = std::min(s.end, b);
        if (seg_s > cursor) {
            DocFormat gap_fmt;
            apply(gap_fmt);
            out.push_back({cursor, seg_s, gap_fmt});
        }
        DocFormat f = s.fmt;
        apply(f);
        out.push_back({seg_s, seg_e, f});
        cursor = seg_e;
    }
    if (cursor < b) {
        DocFormat gap_fmt;
        apply(gap_fmt);
        out.push_back({cursor, b, gap_fmt});
    }

    // Comparator: orders the rebuilt segments by ascending start offset.
    std::sort(out.begin(), out.end(), [](const DocSpan &x, const DocSpan &y) { return x.start < y.start; });
    p.spans = std::move(out);
    CoalesceSpans(p.spans);
}

void SetParagraphAlignment(std::vector<DocParagraph> &paragraphs, int first, int last, DocParagraph::Align align) {
    first = std::clamp(first, 0, static_cast<int>(paragraphs.size()) - 1);
    last = std::clamp(last, 0, static_cast<int>(paragraphs.size()) - 1);
    if (first > last) std::swap(first, last);
    for (int i = first; i <= last; i++) paragraphs[static_cast<size_t>(i)].align = align;
}

void SetParagraphListKind(std::vector<DocParagraph> &paragraphs, int first, int last, DocParagraph::ListKind kind) {
    first = std::clamp(first, 0, static_cast<int>(paragraphs.size()) - 1);
    last = std::clamp(last, 0, static_cast<int>(paragraphs.size()) - 1);
    if (first > last) std::swap(first, last);
    for (int i = first; i <= last; i++) paragraphs[static_cast<size_t>(i)].list_kind = kind;
}

// ============================================================================
// File path helpers
// ============================================================================

namespace {
/**
 * @brief Extracts and lowercases a file path's extension (text after the last '.').
 * @param path The file path to inspect.
 * @return The lowercased extension, or "" if `path` has no '.'.
 */
std::string LowerExt(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    // Lowercases each character of the extension in place.
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}
}  // namespace

bool IsDocxPath(const std::string &path) { return LowerExt(path) == "docx"; }
bool IsOdtPath(const std::string &path) { return LowerExt(path) == "odt"; }

std::string SniffImageExtension(const std::string &bytes) {
    /**
     * @brief Checks whether `bytes` begins with the given magic-number byte sequence.
     * @param sig The signature bytes to match at the start of `bytes`.
     * @return True if `bytes` is at least as long as `sig` and matches it byte-for-byte.
     */
    auto starts_with = [&](std::initializer_list<unsigned char> sig) {
        if (bytes.size() < sig.size()) return false;
        size_t i = 0;
        for (unsigned char b : sig) {
            if (static_cast<unsigned char>(bytes[i++]) != b) return false;
        }
        return true;
    };
    if (starts_with({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A})) return "png";
    if (starts_with({0xFF, 0xD8, 0xFF})) return "jpeg";
    if (bytes.size() >= 4 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == '8') return "gif";
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return "bmp";
    return "png";
}

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

bool WriteZipReplacingEntries(const unsigned char *orig_bytes, size_t orig_len,
                               const std::vector<std::pair<std::string, std::string>> &entries,
                               std::vector<unsigned char> &out, std::string &error) {
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
    std::vector<bool> replaced(entries.size(), false);
    bool ok = true;
    for (mz_uint i = 0; i < n && ok; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&reader, i, &stat)) {
            ok = false;
            break;
        }
        int match = -1;
        for (size_t j = 0; j < entries.size(); j++) {
            if (entries[j].first == stat.m_filename) {
                match = static_cast<int>(j);
                break;
            }
        }
        if (match >= 0) {
            ok = mz_zip_writer_add_mem(&writer, entries[static_cast<size_t>(match)].first.c_str(), entries[static_cast<size_t>(match)].second.data(),
                                        entries[static_cast<size_t>(match)].second.size(), MZ_DEFAULT_COMPRESSION);
            replaced[static_cast<size_t>(match)] = true;
        } else {
            ok = mz_zip_writer_add_from_zip_reader(&writer, &reader, i);
        }
    }
    for (size_t j = 0; ok && j < entries.size(); j++) {
        if (replaced[j]) continue;
        ok = mz_zip_writer_add_mem(&writer, entries[j].first.c_str(), entries[j].second.data(),
                                    entries[j].second.size(), MZ_DEFAULT_COMPRESSION);
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
/**
 * @brief Maps a DOCX heading paragraph style id to a 1-6 heading level.
 * @param style_id The style id, e.g. "Heading1"/"Heading 1"/"heading1".
 * @return 1-6 if `style_id` is recognized as a heading style; 0 otherwise.
 */
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
/**
 * @brief Determines whether an OOXML boolean toggle element (e.g. <w:b/>) is "on".
 * @param toggle_node The toggle element node (may be null/empty).
 * @return False if the node is absent; otherwise true unless w:val is "false"/"0"/"off".
 */
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
/**
 * @brief Appends one <w:r> run's text (including <w:tab/>/<w:br/> as '\t'/'\n') onto `text`.
 * @param run The <w:r> run node to read.
 * @param text The string to append onto.
 * @return The number of characters appended.
 */
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

// Maps a DOCX <w:rFonts w:ascii="..."> value to one of the 3 embedded
// families -- exact-name match against Liberation's own family names,
// plus the handful of metric-compatible substitutes those names are
// commonly used as drop-in replacements for (Arial/Helvetica -> Sans,
// Times New Roman/Times/Georgia/Cambria -> Serif, Courier New/Consolas ->
// Mono) so a document authored in real Word still gets a *sensible*
// mep-side family rather than silently falling back to Sans for every
// font name it doesn't recognize verbatim.
/**
 * @brief Maps a DOCX <w:rFonts> ascii font name to one of the 3 embedded logical families.
 * @param name The font name as it appears in <w:rFonts w:ascii="...">.
 * @return Serif/Mono if `name` matches a recognized family or metric-compatible substitute; Sans otherwise.
 */
OfficeFontFamily DocxFontFamilyFromName(const std::string &name) {
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

// Walks a <w:p>'s children collecting run text/spans -- recurses into
// <w:hyperlink> (which wraps <w:r> runs) so hyperlinked text isn't
// silently dropped; hyperlink *behavior* (click/navigate) isn't modeled.
/**
 * @brief Walks a <w:p>'s run/hyperlink children, appending text and building DocSpans for formatted runs.
 * @param p_node The <w:p> paragraph node to walk.
 * @param out The paragraph to append text/spans onto.
 */
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
                if (pugi::xml_node va = rpr.child("w:vertAlign")) {
                    std::string v = va.attribute("w:val").as_string();
                    fmt.superscript = (v == "superscript");
                    fmt.subscript = (v == "subscript");
                }
                if (pugi::xml_node rfonts = rpr.child("w:rFonts")) {
                    pugi::xml_attribute ascii = rfonts.attribute("w:ascii");
                    if (ascii) fmt.font_family = DocxFontFamilyFromName(ascii.as_string());
                }
                if (pugi::xml_node sz = rpr.child("w:sz")) {
                    // w:sz is in half-points.
                    fmt.font_size_pt = static_cast<float>(sz.attribute("w:val").as_int()) / 2.0f;
                }
                if (pugi::xml_node color = rpr.child("w:color")) {
                    std::string v = color.attribute("w:val").as_string();
                    unsigned int rgb = 0;
                    if (v.size() == 6 && v != "auto" && std::sscanf(v.c_str(), "%x", &rgb) == 1) {
                        fmt.has_color = true;
                        fmt.color_r = static_cast<unsigned char>((rgb >> 16) & 0xff);
                        fmt.color_g = static_cast<unsigned char>((rgb >> 8) & 0xff);
                        fmt.color_b = static_cast<unsigned char>(rgb & 0xff);
                    }
                }
                if (pugi::xml_node shd = rpr.child("w:shd")) {
                    std::string v = shd.attribute("w:fill").as_string();
                    unsigned int rgb = 0;
                    if (v.size() == 6 && v != "auto" && std::sscanf(v.c_str(), "%x", &rgb) == 1) {
                        fmt.has_highlight = true;
                        fmt.highlight_r = static_cast<unsigned char>((rgb >> 16) & 0xff);
                        fmt.highlight_g = static_cast<unsigned char>((rgb >> 8) & 0xff);
                        fmt.highlight_b = static_cast<unsigned char>(rgb & 0xff);
                    }
                }
            }
            int start = static_cast<int>(out.text.size());
            int len = AppendDocxRunText(child, out.text);
            if (len > 0 && fmt != DocFormat{}) {
                out.spans.push_back({start, start + len, fmt});
            }
        } else if (name == "w:hyperlink") {
            CollectDocxParagraphRuns(child, out);  // recurse: hyperlink wraps <w:r> runs
        }
        // Other paragraph children (bookmarks, fields, etc.) skipped.
    }
}

// Extracts one <w:tc> cell's plain text: concatenates every <w:r>/<w:t> run
// across the cell's <w:p> children (a cell is required by OOXML to hold at
// least one paragraph), joining multiple paragraphs with '\n' -- DocTable
// cells are a single flat string (no per-paragraph structure), matching
// this file's own "plain text only" scope note for tables. <w:tab>/<w:br>
// map to '\t'/'\n' the same way CollectDocxParagraphRuns does for ordinary
// paragraph text; run-level formatting (bold/color/etc.) inside a cell is
// dropped, also matching DocTable's documented scope.
/**
 * @brief Extracts a <w:tc> table cell's plain text, joining multiple paragraphs with '\n'.
 * @param tc_node The <w:tc> cell node to read.
 * @return The cell's flattened plain text.
 */
std::string DocxCellText(const pugi::xml_node &tc_node) {
    std::string text;
    bool first_p = true;
    for (pugi::xml_node p : tc_node.children("w:p")) {
        if (!first_p) text += "\n";
        first_p = false;
        for (pugi::xml_node r : p.children("w:r")) {
            for (pugi::xml_node t_node : r.children("w:t")) text += t_node.text().get();
            if (r.child("w:tab")) text += "\t";
            if (r.child("w:br")) text += "\n";
        }
    }
    return text;
}

// Parses a <w:tbl> into a DocTable. Column count is taken from the widest
// <w:tr> (not <w:tblGrid>, which can disagree with actual cell counts in
// the wild) -- shorter rows are padded with empty cells.
/**
 * @brief Parses a <w:tbl> into a DocTable, padding shorter rows with empty cells.
 * @param tbl_node The <w:tbl> table node to parse.
 * @return The parsed table, sized rows x (widest row's column count).
 */
DocTable ParseDocxTable(const pugi::xml_node &tbl_node) {
    std::vector<std::vector<std::string>> rows;
    int max_cols = 0;
    for (pugi::xml_node tr : tbl_node.children("w:tr")) {
        std::vector<std::string> row;
        for (pugi::xml_node tc : tr.children("w:tc")) row.push_back(DocxCellText(tc));
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

// Recursively searches a <w:p> for an embedded inline image's
// <a:blip r:embed="rIdN"/> (inside <w:r><w:drawing>...) and returns its
// relationship id. Plain tag-name string matching, not real namespace
// resolution -- consistent with how every other w:/a:/pic: element in this
// file is matched (pugixml here is never configured namespace-aware).
/**
 * @brief Recursively searches a node's descendants for an <a:blip r:embed="..."/> and returns its relationship id.
 * @param node The node to search within (typically a <w:p>).
 * @param rel_id Receives the relationship id if found.
 * @return True if a blip was found (`rel_id` set); false otherwise.
 */
bool FindDocxBlipRelId(const pugi::xml_node &node, std::string &rel_id) {
    for (pugi::xml_node child : node.children()) {
        if (std::string(child.name()) == "a:blip") {
            if (pugi::xml_attribute embed = child.attribute("r:embed")) {
                rel_id = embed.as_string();
                return true;
            }
        }
        if (FindDocxBlipRelId(child, rel_id)) return true;
    }
    return false;
}

// Loads word/_rels/document.xml.rels (if present -- a docx with no
// relationships at all, e.g. one that never had an image/hyperlink, simply
// yields an empty map) into an Id -> Target lookup, used to resolve an
// <a:blip r:embed="rIdN"/> to its word/media/... zip entry path.
/**
 * @brief Loads word/_rels/document.xml.rels (if present) into an Id -> Target lookup map.
 * @param bytes Pointer to the .docx file's raw bytes.
 * @param len Length of `bytes` in bytes.
 * @return The relationship id -> target map, empty if the rels part is missing or unreadable.
 */
std::unordered_map<std::string, std::string> LoadDocxRelationships(const unsigned char *bytes, size_t len) {
    std::unordered_map<std::string, std::string> out;
    std::vector<unsigned char> rel_bytes;
    if (!ReadZipEntry(bytes, len, "word/_rels/document.xml.rels", rel_bytes)) return out;
    pugi::xml_document rel_doc;
    if (!rel_doc.load_buffer(rel_bytes.data(), rel_bytes.size(), pugi::parse_default, pugi::encoding_utf8)) return out;
    for (pugi::xml_node rel : rel_doc.child("Relationships").children("Relationship")) {
        std::string id = rel.attribute("Id").as_string();
        if (!id.empty()) out[id] = rel.attribute("Target").as_string();
    }
    return out;
}

/**
 * @brief Parses a <w:p>'s paragraph properties (heading/alignment/list) and run text/spans into `out`.
 * @param p_node The <w:p> paragraph node to parse.
 * @param out The paragraph to populate.
 */
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
        // v1: any <w:numPr> reads as a bullet list -- distinguishing
        // bullet vs. numbered would need resolving numbering.xml's
        // abstractNum format (out of scope, same simplification this file
        // already documents for w:basedOn style cascades). Authoring a
        // *new* Numbered list from inside mep still works (SetOfficeListKind/
        // ListKind::Numbered) -- it just doesn't survive a save/reload
        // round-trip as "numbered" specifically (see SerializeDocxParagraph's
        // own comment on why list membership isn't re-emitted on save at
        // all yet).
        if (ppr.child("w:numPr")) out.list_kind = DocParagraph::ListKind::Bullet;
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
    out.tables.clear();
    out.images.clear();
    out.source_format = "docx";
    std::unordered_map<std::string, std::string> rels = LoadDocxRelationships(bytes, len);
    // Walks every direct <w:body> child in document order (not just <w:p>)
    // so a <w:tbl>'s position relative to surrounding paragraphs is
    // preserved -- a table isn't itself a paragraph, so it gets its own
    // fresh empty anchor paragraph pointing at it (DocParagraph::table_ref),
    // matching how Editor::InsertOfficeTablePrompt anchors a freshly
    // inserted table to the (usually empty) paragraph the cursor was on.
    // An inline image lives *inside* one of the paragraph's own <w:r>
    // elements (<w:r><w:drawing>...<a:blip r:embed="rIdN"/>...), so unlike
    // a table it needs no synthetic anchor -- the paragraph it's already
    // part of becomes the anchor via DocParagraph::image_ref.
    for (pugi::xml_node child : body.children()) {
        std::string name = child.name();
        if (name == "w:p") {
            DocParagraph p;
            ParseDocxParagraph(child, p);
            std::string rel_id;
            if (FindDocxBlipRelId(child, rel_id)) {
                auto rel_it = rels.find(rel_id);
                if (rel_it != rels.end()) {
                    std::string target = rel_it->second;
                    std::string media_path = (!target.empty() && target[0] == '/') ? target.substr(1) : "word/" + target;
                    std::vector<unsigned char> img_bytes;
                    if (ReadZipEntry(bytes, len, media_path.c_str(), img_bytes)) {
                        ImageDoc probe;
                        if (probe.LoadFromMemory(img_bytes.data(), img_bytes.size())) {
                            DocImage img;
                            img.bytes.assign(img_bytes.begin(), img_bytes.end());
                            img.natural_w = probe.Width();
                            img.natural_h = probe.Height();
                            out.images.push_back(std::move(img));
                            p.image_ref = static_cast<int>(out.images.size()) - 1;
                        }
                    }
                }
            }
            out.paragraphs.push_back(std::move(p));
        } else if (name == "w:tbl") {
            out.tables.push_back(ParseDocxTable(child));
            DocParagraph anchor;
            anchor.table_ref = static_cast<int>(out.tables.size()) - 1;
            out.paragraphs.push_back(std::move(anchor));
        }
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
/**
 * @brief Builds a <w:p> node's paragraph properties and format-run children from a DocParagraph.
 * @param p The paragraph to serialize.
 * @param p_node The <w:p> XML node to append children onto.
 */
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

    /**
     * @brief Appends one <w:r> run covering [s,e) of `p.text`, with `fmt`'s properties and tab/break splitting.
     * @param s Start offset into `p.text`.
     * @param e End offset into `p.text` (no-op if e<=s).
     * @param fmt The format to render as this run's <w:rPr>.
     */
    auto emit_run = [&](int s, int e, const DocFormat &fmt) {
        if (e <= s) return;
        pugi::xml_node r = p_node.append_child("w:r");
        if (fmt != DocFormat{}) {
            pugi::xml_node rpr = r.append_child("w:rPr");
            if (fmt.font_family != OfficeFontFamily::Sans) {
                const char *fam = fmt.font_family == OfficeFontFamily::Serif ? "Liberation Serif" : "Liberation Mono";
                pugi::xml_node rfonts = rpr.append_child("w:rFonts");
                rfonts.append_attribute("w:ascii").set_value(fam);
                rfonts.append_attribute("w:hAnsi").set_value(fam);
            }
            if (fmt.bold) rpr.append_child("w:b");
            if (fmt.italic) rpr.append_child("w:i");
            if (fmt.underline) rpr.append_child("w:u").append_attribute("w:val").set_value("single");
            if (fmt.strike) rpr.append_child("w:strike");
            if (fmt.superscript) rpr.append_child("w:vertAlign").append_attribute("w:val").set_value("superscript");
            else if (fmt.subscript) rpr.append_child("w:vertAlign").append_attribute("w:val").set_value("subscript");
            if (fmt.font_size_pt > 0.0f) {
                // w:sz is in half-points.
                int half_pt = std::max(1, static_cast<int>(std::lround(fmt.font_size_pt * 2.0f)));
                rpr.append_child("w:sz").append_attribute("w:val").set_value(half_pt);
            }
            if (fmt.has_color) {
                char hex[7];
                std::snprintf(hex, sizeof(hex), "%02X%02X%02X", fmt.color_r, fmt.color_g, fmt.color_b);
                rpr.append_child("w:color").append_attribute("w:val").set_value(hex);
            }
            if (fmt.has_highlight) {
                char hex[7];
                std::snprintf(hex, sizeof(hex), "%02X%02X%02X", fmt.highlight_r, fmt.highlight_g, fmt.highlight_b);
                pugi::xml_node shd = rpr.append_child("w:shd");
                shd.append_attribute("w:val").set_value("clear");
                shd.append_attribute("w:color").set_value("auto");
                shd.append_attribute("w:fill").set_value(hex);
            }
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

// Builds a <w:tbl> from a DocTable: explicit single-line borders on every
// edge (so the grid mep itself always draws is visible when the file is
// reopened elsewhere too, rather than depending on a table style being
// present) and equal-width columns via a fixed <w:tblGrid> -- DocTable has
// no per-column width model (office_doc.h's own scope note), so this is a
// deliberate v1 simplification, not a preserved original width. Every cell
// gets exactly one <w:p> (OOXML requires at least one), with a run per
// non-empty '\t'/'\n'-delimited segment, mirroring emit_run's own
// segmentation above but without per-run <w:rPr> since DocTable cells carry
// no formatting.
/**
 * @brief Builds a <w:tbl> node from a DocTable, with visible borders and equal-width columns.
 * @param t The table to serialize.
 * @param tbl_node The <w:tbl> XML node to append children onto.
 */
void SerializeDocxTable(const DocTable &t, pugi::xml_node &tbl_node) {
    pugi::xml_node tbl_pr = tbl_node.append_child("w:tblPr");
    pugi::xml_node borders = tbl_pr.append_child("w:tblBorders");
    for (const char *edge : {"w:top", "w:left", "w:bottom", "w:right", "w:insideH", "w:insideV"}) {
        pugi::xml_node b = borders.append_child(edge);
        b.append_attribute("w:val").set_value("single");
        b.append_attribute("w:sz").set_value(4);
        b.append_attribute("w:space").set_value(0);
        b.append_attribute("w:color").set_value("000000");
    }
    constexpr int kColWidthTwips = 2000;
    pugi::xml_node grid = tbl_node.append_child("w:tblGrid");
    for (int c = 0; c < t.cols; c++) {
        grid.append_child("w:gridCol").append_attribute("w:w").set_value(kColWidthTwips);
    }
    for (int r = 0; r < t.rows; r++) {
        pugi::xml_node tr = tbl_node.append_child("w:tr");
        for (int c = 0; c < t.cols; c++) {
            pugi::xml_node tc = tr.append_child("w:tc");
            pugi::xml_node tc_pr = tc.append_child("w:tcPr");
            pugi::xml_node tc_w = tc_pr.append_child("w:tcW");
            tc_w.append_attribute("w:w").set_value(kColWidthTwips);
            tc_w.append_attribute("w:type").set_value("dxa");
            pugi::xml_node p_node = tc.append_child("w:p");
            const std::string &txt = t.Cell(r, c);
            if (txt.empty()) continue;
            pugi::xml_node run = p_node.append_child("w:r");
            size_t seg_start = 0;
            for (size_t i = 0; i <= txt.size(); i++) {
                bool at_end = i == txt.size();
                bool is_tab = !at_end && txt[i] == '\t';
                bool is_br = !at_end && txt[i] == '\n';
                if (is_tab || is_br || at_end) {
                    if (i > seg_start) {
                        pugi::xml_node t_node = run.append_child("w:t");
                        t_node.append_attribute("xml:space").set_value("preserve");
                        t_node.text().set(txt.substr(seg_start, i - seg_start).c_str());
                    }
                    if (is_tab) run.append_child("w:tab");
                    else if (is_br) run.append_child("w:br");
                    seg_start = i + 1;
                }
            }
        }
    }
}

// Builds a minimal-but-valid inline <w:drawing> (WordprocessingML's
// wp:inline shape, wrapping a:graphic/a:graphicData/pic:pic) referencing
// `rel_id` inside `r_node` (a fresh <w:r> the caller already appended to
// the paragraph). Every namespace this needs (wp/a/pic/r) is declared
// directly on the elements that introduce them rather than relying on the
// document root already declaring them -- self-contained regardless of
// what the original document.xml's root happened to declare. Width/height
// are converted px -> EMU at a 96 DPI assumption (the same implicit
// assumption DocImage::natural_w/h's pixel dimensions carry everywhere
// else in this codebase) and capped to 6 inches wide, matching main.cpp's
// own render-time "cap to the pane's content width" behavior.
/**
 * @brief Builds a minimal inline <w:drawing> referencing an image relationship inside a run node.
 * @param img The image whose natural dimensions size the drawing (converted px -> EMU, capped to 6in wide).
 * @param r_node The <w:r> run node to append the drawing onto.
 * @param rel_id The relationship id (<a:blip r:embed="...">) pointing at the image part.
 */
void SerializeDocxDrawing(const DocImage &img, pugi::xml_node &r_node, const std::string &rel_id) {
    constexpr long long kEmuPerPx = 9525;
    constexpr long long kMaxWidthEmu = 5486400;  // 6 inches
    long long w = static_cast<long long>(img.natural_w) * kEmuPerPx;
    long long h = static_cast<long long>(img.natural_h) * kEmuPerPx;
    if (w > kMaxWidthEmu && w > 0) {
        h = h * kMaxWidthEmu / w;
        w = kMaxWidthEmu;
    }
    std::string w_str = std::to_string(std::max(1LL, w)), h_str = std::to_string(std::max(1LL, h));

    pugi::xml_node drawing = r_node.append_child("w:drawing");
    pugi::xml_node inline_node = drawing.append_child("wp:inline");
    inline_node.append_attribute("xmlns:wp")
        .set_value("http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing");
    inline_node.append_attribute("distT").set_value("0");
    inline_node.append_attribute("distB").set_value("0");
    inline_node.append_attribute("distL").set_value("0");
    inline_node.append_attribute("distR").set_value("0");
    pugi::xml_node extent = inline_node.append_child("wp:extent");
    extent.append_attribute("cx").set_value(w_str.c_str());
    extent.append_attribute("cy").set_value(h_str.c_str());
    pugi::xml_node doc_pr = inline_node.append_child("wp:docPr");
    doc_pr.append_attribute("id").set_value("1");
    doc_pr.append_attribute("name").set_value("Picture");
    pugi::xml_node graphic = inline_node.append_child("a:graphic");
    graphic.append_attribute("xmlns:a").set_value("http://schemas.openxmlformats.org/drawingml/2006/main");
    pugi::xml_node graphic_data = graphic.append_child("a:graphicData");
    graphic_data.append_attribute("uri").set_value("http://schemas.openxmlformats.org/drawingml/2006/picture");
    pugi::xml_node pic = graphic_data.append_child("pic:pic");
    pic.append_attribute("xmlns:pic").set_value("http://schemas.openxmlformats.org/drawingml/2006/picture");
    pugi::xml_node nv_pic_pr = pic.append_child("pic:nvPicPr");
    pugi::xml_node cnv_pr = nv_pic_pr.append_child("pic:cNvPr");
    cnv_pr.append_attribute("id").set_value("0");
    cnv_pr.append_attribute("name").set_value("Picture");
    nv_pic_pr.append_child("pic:cNvPicPr");
    pugi::xml_node blip_fill = pic.append_child("pic:blipFill");
    pugi::xml_node blip = blip_fill.append_child("a:blip");
    blip.append_attribute("xmlns:r").set_value("http://schemas.openxmlformats.org/officeDocument/2006/relationships");
    blip.append_attribute("r:embed").set_value(rel_id.c_str());
    blip_fill.append_child("a:stretch").append_child("a:fillRect");
    pugi::xml_node sp_pr = pic.append_child("pic:spPr");
    pugi::xml_node xfrm = sp_pr.append_child("a:xfrm");
    pugi::xml_node off = xfrm.append_child("a:off");
    off.append_attribute("x").set_value("0");
    off.append_attribute("y").set_value("0");
    pugi::xml_node ext = xfrm.append_child("a:ext");
    ext.append_attribute("cx").set_value(w_str.c_str());
    ext.append_attribute("cy").set_value(h_str.c_str());
    pugi::xml_node prst_geom = sp_pr.append_child("a:prstGeom");
    prst_geom.append_attribute("prst").set_value("rect");
    prst_geom.append_child("a:avLst");
}

}  // namespace

std::string MimeForImageExt(const std::string &ext) {
    if (ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "bmp") return "image/bmp";
    return "image/png";
}

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

    // Removes every existing <w:p>/<w:tbl>, remembering the first <w:sectPr>
    // (page setup) as the insertion anchor so freshly-built paragraphs/
    // tables land before it -- OOXML requires w:sectPr, if present, to be
    // w:body's last child.
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
    // Every image in doc.images gets a fresh word/media/ part + relationship
    // this save, in index order -- rel_id/filename are namespaced with a
    // "mep" prefix specifically so they can never collide with whatever
    // Word-generated rIds/media names the original document.xml.rels
    // already has, sidestepping the need to scan for the highest existing
    // rId. A pre-existing (round-tripped, unchanged) image gets re-added as
    // a new part too rather than reusing its original one -- simpler than
    // tracking per-image provenance, at the cost of leaving the original
    // now-unreferenced media part+relationship behind as harmless (if
    // slightly wasteful) dead weight in the saved file, a documented v1
    // simplification.
    struct NewImage {
        std::string rel_id, media_filename;
        const DocImage *img;
    };
    std::vector<NewImage> new_images;
    new_images.reserve(doc.images.size());
    for (size_t i = 0; i < doc.images.size(); i++) {
        std::string ext = SniffImageExtension(doc.images[i].bytes);
        new_images.push_back({"rIdMep" + std::to_string(i + 1), "mepimage" + std::to_string(i + 1) + "." + ext,
                              &doc.images[i]});
    }

    for (const DocParagraph &p : doc.paragraphs) {
        pugi::xml_node p_node = anchor ? body.insert_child_before("w:p", anchor) : body.append_child("w:p");
        SerializeDocxParagraph(p, p_node);
        if (p.table_ref >= 0 && p.table_ref < static_cast<int>(doc.tables.size())) {
            pugi::xml_node tbl_node = anchor ? body.insert_child_before("w:tbl", anchor) : body.append_child("w:tbl");
            SerializeDocxTable(doc.tables[static_cast<size_t>(p.table_ref)], tbl_node);
        }
        if (p.image_ref >= 0 && p.image_ref < static_cast<int>(new_images.size())) {
            const NewImage &ni = new_images[static_cast<size_t>(p.image_ref)];
            pugi::xml_node r_node = p_node.append_child("w:r");
            SerializeDocxDrawing(*ni.img, r_node, ni.rel_id);
        }
    }

    std::ostringstream ss;
    xml.save(ss, "", pugi::format_raw);
    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("word/document.xml", ss.str());

    if (!new_images.empty()) {
        // word/_rels/document.xml.rels: load the original (if the document
        // never had any relationships at all -- no hyperlinks, no styles
        // reference, vanishingly rare but possible -- build a fresh minimal
        // one) and append one <Relationship> per new image.
        std::vector<unsigned char> rels_bytes;
        bool have_rels =
            ReadZipEntry(original_bytes.data(), original_bytes.size(), "word/_rels/document.xml.rels", rels_bytes);
        pugi::xml_document rels_doc;
        pugi::xml_node rels_root;
        if (have_rels &&
            rels_doc.load_buffer(rels_bytes.data(), rels_bytes.size(), pugi::parse_default, pugi::encoding_utf8)) {
            rels_root = rels_doc.child("Relationships");
        }
        if (!rels_root) {
            rels_doc.reset();
            rels_root = rels_doc.append_child("Relationships");
            rels_root.append_attribute("xmlns").set_value("http://schemas.openxmlformats.org/package/2006/relationships");
        }
        for (const NewImage &ni : new_images) {
            pugi::xml_node rel = rels_root.append_child("Relationship");
            rel.append_attribute("Id").set_value(ni.rel_id.c_str());
            rel.append_attribute("Type").set_value(
                "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image");
            rel.append_attribute("Target").set_value(("media/" + ni.media_filename).c_str());
        }
        std::ostringstream rss;
        rels_doc.save(rss, "", pugi::format_raw);
        entries.emplace_back("word/_rels/document.xml.rels", rss.str());

        // [Content_Types].xml: add a <Default Extension="..."> for any
        // image extension not already declared (a docx that never had an
        // image before commonly lacks these entirely).
        std::vector<unsigned char> ct_bytes;
        bool have_ct = ReadZipEntry(original_bytes.data(), original_bytes.size(), "[Content_Types].xml", ct_bytes);
        pugi::xml_document ct_doc;
        pugi::xml_node types_root;
        if (have_ct &&
            ct_doc.load_buffer(ct_bytes.data(), ct_bytes.size(), pugi::parse_default, pugi::encoding_utf8)) {
            types_root = ct_doc.child("Types");
        }
        if (types_root) {
            std::vector<std::string> seen_exts;
            for (const NewImage &ni : new_images) {
                std::string ext = ni.media_filename.substr(ni.media_filename.find_last_of('.') + 1);
                if (std::find(seen_exts.begin(), seen_exts.end(), ext) != seen_exts.end()) continue;
                seen_exts.push_back(ext);
                bool found = false;
                for (pugi::xml_node d : types_root.children("Default")) {
                    if (std::string(d.attribute("Extension").as_string()) == ext) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    pugi::xml_node d = types_root.append_child("Default");
                    d.append_attribute("Extension").set_value(ext.c_str());
                    d.append_attribute("ContentType").set_value(MimeForImageExt(ext).c_str());
                }
            }
            std::ostringstream css;
            ct_doc.save(css, "", pugi::format_raw);
            entries.emplace_back("[Content_Types].xml", css.str());
        }

        for (const NewImage &ni : new_images) {
            entries.emplace_back("word/media/" + ni.media_filename, ni.img->bytes);
        }
    }

    return WriteZipReplacingEntries(original_bytes.data(), original_bytes.size(), entries, out, error);
}
