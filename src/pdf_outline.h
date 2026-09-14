#pragma once

// mep's own in-house PDF outline (bookmarks) tree reader -- spec 12.3.3.
// Independent successor work to PDFIUM_REMOVAL_PLAN.md's own 14 phases
// (which fully replaced PDFium before this file existed): reads the
// `/Root /Outlines` dictionary tree a PDF viewer's own "bookmarks"/
// "table of contents" panel shows, for mep's Structure sidebar
// (kBuiltinStructure, main.cpp) to display for a PDF-backed buffer the
// same way it already shows LaTeX sections/code symbols for others.

#include "pdf_document.h"
#include "pdf_object.h"
#include "pdf_xref.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pdfoutline {

// Resolves a /Dest value (or an /A action dict's own /D) to a 0-based
// page index -- shared with pdf_links.cpp (spec 12.5.6.5 Link
// annotations use exactly the same /Dest-or-/A shapes an outline item's
// own destination does, see this header's own GetOutline comment for
// which ones). Returns -1 for anything unresolvable (a malformed
// destination, or a named one whose name isn't actually in
// /Root/Names/Dests) rather than failing outright, matching every other
// tolerant fallback in this PDF engine.
int ResolveDestPage(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                     const pdfdoc::PdfDocument &document, const pdfobj::Object &dest_or_action);

// One bookmark entry, flattened out of the tree with a `depth` field --
// the same "flat list + depth, not a real nested structure" convention
// pdf_content.h's own TextGlyph/pdf_text.h's PdfTextMatch siblings use,
// chosen for the identical reason: the consuming side (here, main.cpp's
// generic structure-sidebar rendering, which already expects exactly
// this shape for LaTeX/Treesitter items) never needs to walk a real
// tree, just indent by depth.
struct OutlineItem {
    std::string title;
    // 0-based target page index, or -1 if this bookmark's destination
    // couldn't be resolved to a page (a malformed/unresolvable
    // destination, or a named destination whose name isn't actually in
    // /Root/Names/Dests -- see GetOutline's own doc comment) -- still
    // listed with its title, just not click-to-jump-able.
    int page = -1;
    int depth = 0;
};

// Reads the document's own outline tree (empty if it has no /Outlines
// dict at all -- most PDFs don't; only ones from tools like LaTeX's
// hyperref package, or "Save As PDF" from Word/LibreOffice with heading
// styles, typically do). `data`/`len` must be the same buffer `table`
// was loaded from; `document` supplies the object-number -> page-index
// lookup (PdfDocument::PageIndexForObjectNum) a destination's page
// reference needs resolving against.
//
// Destination resolution handles both shapes real bookmark-generating
// tools emit: a /Dest array (or an /A GoTo action's own /D array) whose
// first element is an indirect reference to a Page object, and a named
// destination (a /Dest or /A-/D given as a String, resolved through the
// /Root/Names/Dests name tree, spec 7.9.6) -- hyperref's own default
// output for LaTeX's \section/\chapter/etc. bookmarks uses exactly this
// named-destination shape, so this is the common case for a
// tectonic/pdflatex-produced PDF, not a rare one. Anything else
// (a malformed destination, or a named one whose name isn't actually in
// the Dests tree) is tolerated the same way this whole PDF engine
// tolerates anything else it doesn't fully resolve -- the bookmark still
// appears, with page -1.
std::vector<OutlineItem> GetOutline(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                                     const pdfdoc::PdfDocument &document);

}  // namespace pdfoutline
