#pragma once

// mep's own in-house PDF Link annotation reader -- spec 12.5.6.5. Reads a
// page's /Annots array for /Subtype /Link entries, for the hint system's
// "jump to a hyperlink visible in the pane" targets (HINT_SYSTEM.md) --
// the PDF-viewer counterpart of html_doc.h's <a href> cascade. Independent
// successor work to pdf_outline.h/.cpp, whose destination-resolution logic
// (pdfoutline::ResolveDestPage) this reuses as-is: a Link annotation's own
// /Dest or /A GoTo action is exactly the same shape an outline item's is.

#include "pdf_document.h"
#include "pdf_xref.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pdflinks {

// One Link annotation, already resolved to a device-pixel rect at the
// `px_per_pt` scale the caller rendered/rasterized the page at -- the
// same space pdftext::PdfHighlightRect uses (pdf_text.h), so a caller
// already converting one of those to screen pixels (DrawPane's PDF
// branch, main.cpp) converts one of these identically.
struct PdfLinkAnnot {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    // 0-based target page, or -1 if this link isn't an internal GoTo (an
    // unresolvable/malformed destination, or a URI/other action type --
    // see `uri` below).
    int target_page = -1;
    // Non-empty for a URI action (spec 12.6.4.7); empty for a GoTo (or
    // any other/malformed) action. At most one of target_page>=0 / uri
    // non-empty is meaningful for a given link, never both.
    std::string uri;
};

// Reads every /Subtype /Link annotation on `page_index` (empty if the
// page has no /Annots array, or none of its annotations are links).
// `data`/`len` must be the same buffer `table`/`document` were loaded
// from. Tolerant like the rest of this PDF engine: a malformed /Rect,
// missing action, or unresolvable destination degrades to that one
// annotation being skipped (or listed with target_page==-1 and an empty
// uri), never a failure of the whole read.
std::vector<PdfLinkAnnot> GetPageLinks(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                                        const pdfdoc::PdfDocument &document, int page_index, float px_per_pt);

}  // namespace pdflinks
