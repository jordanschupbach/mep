#pragma once

// mep's own in-house document-wide text search -- see
// PDFIUM_REMOVAL_PLAN.md Phase 11. Builds on pdf_content.h's
// ExtractContentStreamText (Phase 11's other half, living alongside the
// content-stream interpreter since it reuses that exact same class) to
// turn a page's glyph-by-glyph Unicode text into a searchable per-page
// string, then reproduces pdf_doc.h's PdfDoc::Search/MatchRectsForPage
// contract exactly -- same struct field names/order in
// PdfTextRectPt/PdfTextMatch/PdfHighlightRect (just namespaced here to
// avoid colliding with pdf_doc.h's own globals while both exist side by
// side pre-Phase-13) -- so Phase 13's eventual swap-in is a mechanical
// rename, not a rewrite.
//
// Scoping decision -- word/line-break reconstruction: a PDF content
// stream carries no explicit word- or line-break markers (a "new line"
// is just the next Tj/TJ run positioned lower on the page via its own
// Td/TD/Tm, and inter-word spacing is often just a wider glyph-position
// gap, not always a literal space character). This module infers both
// from glyph geometry alone: consecutive glyphs whose vertical centers
// differ by more than half either one's height start a new line (also
// where PdfTextMatch's own rects split -- matching FPDFText_CountRects'
// "merges same-line character boxes" behavior, which by construction
// never merges across a line); consecutive same-line glyphs separated by
// a horizontal gap wider than a quarter of either one's height get a
// synthetic space inserted so a multi-word query can still find them.
// Approximate by nature (a real space character already existing at that
// same spot naturally reproduces the same behavior via the geometry
// check, so this rarely double-inserts); verified against pdftotext's
// own extraction across every Phase 1 fixture, not expected to be
// byte-exact.

#include "pdf_document.h"
#include "pdf_xref.h"

#include <string>
#include <vector>

namespace pdftext {

// One text-search match's highlight rect, in PDF-point space -- see
// pdf_doc.h's own PdfTextRectPt doc comment for the exact convention
// (page-native units, scale-/rotation-independent; top/bottom use PDF's
// own y-up sense, so top >= bottom).
struct PdfTextRectPt {
    double left = 0, top = 0, right = 0, bottom = 0;
};

struct PdfTextMatch {
    int page = 0;
    std::vector<PdfTextRectPt> rects_pt;
};

struct PdfHighlightRect {
    int match_index = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Plain concatenated text for one page, in reading order -- exposed
// mainly for verification (diffing against `pdftotext`'s own extraction
// of the same fixture); Search below builds and discards the same text
// internally per page, it doesn't call this.
std::string ExtractPageText(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                             const pdfdoc::Page &page);

// Case-insensitive (ASCII-fold) substring search across every page of
// `doc`, in page order, non-overlapping matches per page (matches
// PDFium's own FPDFText_FindNext convention of advancing past each
// found match). `doc_data`/`doc_len` must be the same buffer `doc` was
// loaded from.
std::vector<PdfTextMatch> Search(const unsigned char *doc_data, size_t doc_len, const pdfdoc::PdfDocument &doc,
                                  const std::string &query);

// Converts every rect (across all of `matches`) belonging to page_index
// into device pixels at px_per_pt, via the same PageToDeviceMatrix
// RenderPage itself would use -- mirrors PdfDoc::MatchRectsForPage.
std::vector<PdfHighlightRect> MatchRectsForPage(const pdfdoc::PdfDocument &doc, int page_index, float px_per_pt,
                                                 const std::vector<PdfTextMatch> &matches);

}  // namespace pdftext
