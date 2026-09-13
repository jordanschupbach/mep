#include "pdf_outline.h"

#include <algorithm>
#include <cstdint>

namespace pdfoutline {

namespace {

// Appends `cp`'s UTF-8 encoding to `out` -- same small helper shape as
// pdf_font.cpp's own AppendUtf8 (duplicated rather than shared: both are
// a handful of lines, and this codebase's own established convention
// throughout the PDF engine is that anonymous-namespace-local helpers
// this small stay local rather than growing a shared-utility header for
// them -- see pdf_font.cpp's own CMap tokenizer, which duplicated ideas
// from pdf_content.cpp's tokenizer the same way).
void AppendUtf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Decodes a /Title string: UTF-16BE (spec 7.9.2.2, signaled by a leading
// U+FEFF byte-order-mark 0xFE 0xFF) if present, including surrogate
// pairs for supplementary-plane titles; otherwise PDFDocEncoded, treated
// here as already-ASCII-compatible bytes -- a Scoping simplification
// (true PDFDocEncoding remaps a handful of codepoints above 0x7F to
// symbols/accented Latin characters that differ from raw Latin-1), but
// the overwhelming majority of real bookmark titles are plain ASCII, and
// this degrades to "a few wrong bytes in a title," never a crash or a
// dropped bookmark.
std::string DecodeTitle(const std::string &raw) {
    if (raw.size() < 2 || static_cast<unsigned char>(raw[0]) != 0xFE || static_cast<unsigned char>(raw[1]) != 0xFF) {
        return raw;
    }
    std::string out;
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
        uint32_t cp = (static_cast<uint32_t>(static_cast<unsigned char>(raw[i])) << 8) |
                      static_cast<uint32_t>(static_cast<unsigned char>(raw[i + 1]));
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < raw.size()) {
            uint32_t lo = (static_cast<uint32_t>(static_cast<unsigned char>(raw[i + 2])) << 8) |
                          static_cast<uint32_t>(static_cast<unsigned char>(raw[i + 3]));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        AppendUtf8(out, cp);
    }
    return out;
}

// Depth-first search of one /Root/Names/Dests name-tree node (spec
// 7.9.6) for `name`, writing the matching value to `*out` and returning
// true on success. An intermediate node has /Kids (array of refs to more
// nodes); a leaf has /Names (a flat [name1 value1 name2 value2 ...]
// array). Every real name tree is supposed to keep each node's /Limits
// sorted so a lookup could binary-search/prune by range, but this walks
// every kid unconditionally instead: outline-sized name trees (a few
// hundred entries at most, one per section/figure/table a document
// cross-references) make that optimization not worth the extra code, and
// a tree that violates its own /Limits invariant still resolves
// correctly this way instead of silently missing the entry. `depth` caps
// recursion the same way WalkOutline's own does, against a
// pathological/corrupt /Kids cycle.
bool FindNamedDestRec(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                       const pdfobj::Object &node, const std::string &name, int depth, pdfobj::Object *out) {
    if (depth > 64) return false;
    if (const pdfobj::Object *kids = node.Find("Kids")) {
        if (!kids->IsArray()) return false;
        for (const pdfobj::Object &kid_ref : kids->array_val) {
            if (!kid_ref.IsReference()) continue;
            pdfobj::Object kid = pdfxref::ResolveObject(data, len, table, kid_ref.ref_val.num, kid_ref.ref_val.gen);
            if (!kid.IsDict()) continue;
            if (FindNamedDestRec(data, len, table, kid, name, depth + 1, out)) return true;
        }
        return false;
    }
    const pdfobj::Object *names = node.Find("Names");
    if (!names || !names->IsArray()) return false;
    const std::vector<pdfobj::Object> &arr = names->array_val;
    for (size_t i = 0; i + 1 < arr.size(); i += 2) {
        if (!arr[i].IsString() || arr[i].str_val != name) continue;
        const pdfobj::Object &val = arr[i + 1];
        *out = val.IsReference() ? pdfxref::ResolveObject(data, len, table, val.ref_val.num, val.ref_val.gen) : val;
        return true;
    }
    return false;
}

// Resolves a named destination (spec 7.9.6) -- the string naming it is
// looked up in /Root/Names/Dests's name tree, whose matching leaf value
// is either a destination array directly or a dict wrapping one in its
// own /D key. Returns false if there's no Dests tree at all, or `name`
// isn't in it.
bool ResolveNamedDestination(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                              const std::string &name, pdfobj::Object *out) {
    const pdfobj::Object *root_ref = table.Trailer().Find("Root");
    if (!root_ref || !root_ref->IsReference()) return false;
    pdfobj::Object catalog = pdfxref::ResolveObject(data, len, table, root_ref->ref_val.num, root_ref->ref_val.gen);
    if (!catalog.IsDict()) return false;
    const pdfobj::Object *names_ref = catalog.Find("Names");
    if (!names_ref) return false;
    pdfobj::Object names_dict = names_ref->IsReference()
                                     ? pdfxref::ResolveObject(data, len, table, names_ref->ref_val.num, names_ref->ref_val.gen)
                                     : *names_ref;
    if (!names_dict.IsDict()) return false;
    const pdfobj::Object *dests_ref = names_dict.Find("Dests");
    if (!dests_ref) return false;
    pdfobj::Object dests_root = dests_ref->IsReference()
                                     ? pdfxref::ResolveObject(data, len, table, dests_ref->ref_val.num, dests_ref->ref_val.gen)
                                     : *dests_ref;
    if (!dests_root.IsDict()) return false;
    return FindNamedDestRec(data, len, table, dests_root, name, 0, out);
}

// Resolves a /Dest value (or an /A action dict's own /D) to a 0-based
// page index -- see pdf_outline.h's own doc comment on exactly which
// shapes this handles: a direct destination array (straight off /Dest,
// or an /A GoTo action's own /D, either possibly one more indirect
// reference away), or a named destination (a String naming an entry in
// /Root/Names/Dests) resolved via ResolveNamedDestination above.
int ResolveDestPage(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                     const pdfdoc::PdfDocument &document, const pdfobj::Object &dest_or_action) {
    pdfobj::Object storage_a, storage_b, storage_c;
    const pdfobj::Object *dest = &dest_or_action;
    if (dest->IsReference()) {
        storage_a = pdfxref::ResolveObject(data, len, table, dest->ref_val.num, dest->ref_val.gen);
        dest = &storage_a;
    }
    if (dest->IsDict()) {  // an /A action dict -- only /S /GoTo's own /D is meaningful here
        const pdfobj::Object *d = dest->Find("D");
        if (!d) return -1;
        if (d->IsReference()) {
            storage_b = pdfxref::ResolveObject(data, len, table, d->ref_val.num, d->ref_val.gen);
            dest = &storage_b;
        } else {
            dest = d;
        }
    }
    if (dest->IsString()) {  // named destination (spec 7.9.6)
        pdfobj::Object named;
        if (!ResolveNamedDestination(data, len, table, dest->str_val, &named)) return -1;
        storage_c = std::move(named);
        dest = &storage_c;
        if (dest->IsDict()) {  // name-tree leaf wrapping the array in its own /D, rather than being the array itself
            const pdfobj::Object *d = dest->Find("D");
            if (!d) return -1;
            dest = d;
        }
    }
    if (!dest->IsArray() || dest->array_val.empty()) return -1;  // malformed: unresolved
    const pdfobj::Object &page_ref = dest->array_val[0];
    if (!page_ref.IsReference()) return -1;
    return document.PageIndexForObjectNum(page_ref.ref_val.num);
}

// Walks one sibling chain (starting at `first_ref`) and each one's own
// children, depth-first, appending flattened entries to `out`. `visited`
// guards against a malformed circular /Next or /First chain the same
// way pdf_document.cpp's own FlattenPageTree does; `depth` is also
// capped (a real outline is rarely more than 5-6 levels deep -- 64 is a
// generous ceiling against a pathological/corrupt file, not a real
// limit) since, unlike the cycle guard, an ever-increasing chain of
// distinct object numbers isn't caught by `visited` alone and could
// otherwise recurse arbitrarily deep.
void WalkOutline(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                  const pdfdoc::PdfDocument &document, pdfobj::Ref first_ref, int depth, std::vector<int> &visited,
                  std::vector<OutlineItem> *out) {
    if (depth > 64) return;
    pdfobj::Ref ref = first_ref;
    while (ref.num != 0) {
        if (std::find(visited.begin(), visited.end(), ref.num) != visited.end()) return;
        visited.push_back(ref.num);
        if (visited.size() > 100000) return;  // pathological outline size: bail rather than hang

        pdfobj::Object item = pdfxref::ResolveObject(data, len, table, ref.num, ref.gen);
        if (!item.IsDict()) return;

        OutlineItem out_item;
        const pdfobj::Object *title = item.Find("Title");
        out_item.title = title && title->IsString() ? DecodeTitle(title->str_val) : "(untitled)";
        out_item.depth = depth;
        if (const pdfobj::Object *dest = item.Find("Dest")) {
            out_item.page = ResolveDestPage(data, len, table, document, *dest);
        } else if (const pdfobj::Object *action = item.Find("A")) {
            out_item.page = ResolveDestPage(data, len, table, document, *action);
        }
        out->push_back(std::move(out_item));

        if (const pdfobj::Object *first = item.Find("First")) {
            if (first->IsReference()) WalkOutline(data, len, table, document, first->ref_val, depth + 1, visited, out);
        }

        const pdfobj::Object *next = item.Find("Next");
        if (!next || !next->IsReference()) return;
        ref = next->ref_val;
    }
}

}  // namespace

std::vector<OutlineItem> GetOutline(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                                     const pdfdoc::PdfDocument &document) {
    std::vector<OutlineItem> out;
    const pdfobj::Object *root_ref = table.Trailer().Find("Root");
    if (!root_ref || !root_ref->IsReference()) return out;
    pdfobj::Object catalog = pdfxref::ResolveObject(data, len, table, root_ref->ref_val.num, root_ref->ref_val.gen);
    if (!catalog.IsDict()) return out;

    const pdfobj::Object *outlines_ref = catalog.Find("Outlines");
    if (!outlines_ref || !outlines_ref->IsReference()) return out;
    pdfobj::Object outlines =
        pdfxref::ResolveObject(data, len, table, outlines_ref->ref_val.num, outlines_ref->ref_val.gen);
    if (!outlines.IsDict()) return out;

    const pdfobj::Object *first = outlines.Find("First");
    if (!first || !first->IsReference()) return out;

    std::vector<int> visited;
    WalkOutline(data, len, table, document, first->ref_val, 0, visited, &out);
    return out;
}

}  // namespace pdfoutline
