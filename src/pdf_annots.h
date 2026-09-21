#pragma once

// mep's own in-house reader/model for PDF markup annotations -- text
// highlights (spec 12.5.6.10) and text (sticky-note) annotations (spec
// 12.5.6.4). A sibling of pdf_links.h (which reads /Subtype /Link for the
// hint system): this reads /Subtype /Highlight and /Subtype /Text into a
// shared point-space model that the viewer renders and the pdf_writer.h
// incremental-update writer round-trips back into the file's /Annots.
//
// Unlike pdflinks::GetPageLinks, geometry here stays in the page's own
// unrotated user-space POINTS (not device pixels): highlight quads and
// note rects are stored exactly as they appear in the file so a
// read->edit->write cycle is lossless, and the viewer converts to device
// pixels at draw time (same split search highlights use, pdf_text.h).

#include "pdf_document.h"
#include "pdf_xref.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pdfannots {

enum class Kind { Highlight, Text };

// One highlight quadrilateral, in point space, in the spec's /QuadPoints
// ordering (Table 179): (x1,y1)=upper-left, (x2,y2)=upper-right,
// (x3,y3)=lower-left, (x4,y4)=lower-right -- top edge first, then the
// bottom edge (NOT a consistent winding order). Preserved verbatim so the
// writer can emit it back unchanged.
struct Quad {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0, x3 = 0, y3 = 0, x4 = 0, y4 = 0;
};

// A markup annotation, point space. The same struct is produced by the
// reader (from_file == true), created interactively by the editor, and
// consumed by the writer.
struct PdfAnnot {
    int page = 0;                       // 0-based page index
    Kind kind = Kind::Highlight;
    double rect[4] = {0, 0, 0, 0};      // /Rect, point space, unrotated (x0,y0,x1,y1); for a highlight, the union bbox of `quads`
    std::vector<Quad> quads;            // /QuadPoints, one per text line -- Highlight only
    double color[3] = {1, 1, 0};        // /C as RGB 0..1 (default yellow)
    double opacity = 0.4;               // /CA (constant alpha)
    std::string contents;               // /Contents (note body / highlight comment), UTF-8
    std::string author;                 // /T (annotation author)
    std::string modified;               // /M date string, "" -> writer stamps "now"
    bool open = false;                  // /Open (Text note popup initially open) -- Text only
    std::string icon = "Note";          // /Name icon (Text only): Note/Comment/Help/...

    // Editor/session provenance -- not serialized by the writer. Set by the
    // reader for annotations already in the file so the editor can tell
    // them apart from ones created this session (and, later, edit/delete
    // them by rewriting their object).
    bool from_file = false;
    int src_obj = 0, src_gen = 0;
};

// Reads every /Highlight and /Text annotation on `page_index` into the
// point-space model above (empty if the page has no /Annots, or none are
// markup annotations). `data`/`len` must be the same buffer `table`/
// `document` were loaded from. Tolerant like the rest of this engine: a
// malformed /Rect or /QuadPoints degrades to skipping that one
// annotation, never a failure of the whole read.
std::vector<PdfAnnot> GetPageAnnots(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                                    const pdfdoc::PdfDocument &document, int page_index);

}  // namespace pdfannots
