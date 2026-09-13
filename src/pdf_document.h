#pragma once

// mep's own in-house PDF document model: xref + trailer (pdf_xref.h)
// flattened into a linear page list -- see PDFIUM_REMOVAL_PLAN.md
// Phase 5. This is the object Phase 7+'s content-stream rendering and
// Phase 13's final pdf_doc.cpp swap-in build on; Phase 5 itself only
// adds what's needed to answer PageCount/PageWidthPt/PageHeightPt
// against pdf_doc.h's existing documented contract.

#include "pdf_object.h"
#include "pdf_xref.h"

#include <cstddef>
#include <vector>

namespace pdfdoc {

// The inheritable page attributes (spec 7.7.3.4: /Resources, /MediaBox,
// /CropBox, /Rotate) threaded down a /Pages tree -- bundled into one
// struct so FlattenPageTree's recursion doesn't need a growing parameter
// list every time another inheritable key is added.
struct InheritedAttrs {
    pdfobj::Object resources;    // Null if none found anywhere in the ancestor chain
    double media_box[4] = {0, 0, 612, 792};
    double crop_box[4] = {0, 0, 612, 792};
    bool has_crop_box = false;  // distinguishes "no /CropBox anywhere yet" from "defaults to media_box's own value"
    int rotate = 0;
};

// One flattened page, with every inheritable attribute already resolved
// down from its ancestor /Pages nodes -- callers never need to walk back
// up the tree.
struct Page {
    int object_num = 0;   // the Page dict's own object number (0 if unresolvable, e.g. a malformed tree)
    pdfobj::Object dict;  // the page's own (unmerged) dict, as parsed

    // Own /Resources if present, else the nearest ancestor's -- Null
    // (IsNull()) if none was found anywhere in the ancestor chain
    // (a genuinely empty resource dict is a rare but legal edge case
    // Phase 7+'s content-stream interpreter will need to tolerate).
    pdfobj::Object resources;

    // llx, lly, urx, ury, in default user-space points, BEFORE /Rotate
    // is applied. Defaults to US Letter [0 0 612 792] if no /MediaBox is
    // found anywhere in the ancestor chain (tolerant fallback; a
    // conforming file always has one somewhere, but this codebase's
    // convention is to degrade gracefully rather than fail the whole
    // document over it).
    double media_box[4] = {0, 0, 612, 792};

    // The *effective* box PageWidthPt/HeightPt measure: /CropBox if one
    // was found anywhere in the ancestor chain (intersected with
    // media_box, since spec 7.7.3.3 says CropBox never extends the
    // page beyond its physical MediaBox -- matches PDFium's own real
    // CPDF_Page box computation, confirmed via
    // `pdf_doc.cpp`'s comment that FPDF_GetPageWidthF/HeightF "already
    // reflect the page's own /Rotate", which in turn is computed from
    // this same effective box, not raw MediaBox), else equal to
    // media_box.
    double effective_box[4] = {0, 0, 612, 792};

    // Normalized to one of 0/90/180/270 (spec requires a multiple of
    // 90; a negative or non-multiple value from a nonconforming
    // producer is folded into that range rather than rejected).
    int rotate = 0;
};

class PdfDocument {
public:
    // Parses the xref/trailer (pdfxref::XrefTable::Load) and flattens
    // the page tree starting at the trailer's /Root -> /Pages. `data`
    // must outlive this object -- unlike PdfDoc's own public contract
    // (which copies bytes internally, see pdf_doc.h), this is an
    // internal-only class within pdf_doc.cpp's future implementation.
    void Load(const unsigned char *data, size_t len);

    int PageCount() const { return static_cast<int>(pages_.size()); }
    // Returns nullptr for an out-of-range index.
    const Page *GetPage(int index) const;

    // Width/height in PDF points, after accounting for the page's
    // effective (CropBox-clipped) box and /Rotate (90/270 swap width
    // and height) -- matches pdf_doc.h's PageWidthPt/PageHeightPt
    // contract exactly. Returns 0 for an out-of-range index.
    double PageWidthPt(int index) const;
    double PageHeightPt(int index) const;

    const pdfxref::XrefTable &Xref() const { return table_; }

    // Returns the 0-based index of the page whose own Page dict has this
    // exact object number (Page::object_num, set during page-tree
    // flattening), or -1 if no page matches -- used to resolve a PDF
    // outline (bookmarks) entry's destination page reference
    // (PDFIUM_REMOVAL_PLAN.md's own successor work, pdf_outline.h) back
    // to a page index. A plain linear scan: page counts are small and
    // this only runs a handful of times per document open, not per frame.
    int PageIndexForObjectNum(int object_num) const;

private:
    pdfxref::XrefTable table_;
    std::vector<Page> pages_;

    // Resolves the page-tree node at `ref` (a /Kids entry, or the
    // initial /Pages reference) and recurses: a /Type /Pages node (or
    // one with /Kids but no /Type, tolerated the same way) descends
    // into its /Kids; anything else is treated as a leaf /Page.
    // `visited` guards against a malformed circular /Kids chain.
    void FlattenPageTree(const unsigned char *data, size_t len, pdfobj::Ref ref, const InheritedAttrs &inherited,
                          std::vector<int> &visited);
};

}  // namespace pdfdoc
