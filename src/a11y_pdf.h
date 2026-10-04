#pragma once
// A PDF as a screen reader meets it (a11y_doc.h): its structure tree
// (pdf_struct.h) with each element's text read out of the pages' marked
// content (pdf_content.h's TextGlyph::mcid), figures and formulas as their
// /Alt, artifacts (page numbers, rules) left out. A PDF with no structure
// tree comes back with `tagged` false and its text as extracted, a
// paragraph per block of lines, page by page -- what a screen reader is
// left with for such a file.

#include "a11y_doc.h"
#include "pdf_document.h"

#include <cstddef>

namespace a11y {

// `data`/`len` must be the buffer `document` was loaded from. Reads every
// page's content once: O(document size).
Document FromPdf(const unsigned char *data, size_t len, const pdfdoc::PdfDocument &document);

}  // namespace a11y
