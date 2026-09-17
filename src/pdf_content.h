#pragma once

// mep's own in-house PDF content-stream interpreter -- see
// PDFIUM_REMOVAL_PLAN.md Phase 7 (graphics state & paths) onward
// (Phase 8 adds images, Phase 9-10 add text). Renders a page's content
// stream(s) onto an RGBA8 canvas: graphics-state stack (q/Q/cm/gs),
// path construction (m/l/c/v/y/re/h) and painting (S/s/f/F/f*/B/B*/b/
// b*/n) via gfx/rasterizer.h, clipping (W/W*), device/Indexed/
// Separation-DeviceN color (g/G/rg/RG/k/K/cs/CS/sc/SC/scn/SCN), and
// Form XObjects (Do, recursing into this same interpreter with a
// concatenated matrix and the Form's own /BBox as an additional clip --
// not in this plan's original phase breakdown, added here since it's a
// small, cheap extension of the same machinery and Form XObjects are
// extremely common in real PDFs, e.g. hyperref link annotation
// appearance streams). Image XObjects (`/Subtype /Image`) are a no-op
// until Phase 8.

#include "pdf_document.h"
#include "pdf_object.h"
#include "pdf_xref.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pdfrender {

// A 2D affine transform, PDF's own [a b c d e f] convention:
//   [x' y' 1] = [x y 1] * [a b 0; c d 0; e f 1]
struct Mat2D {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

// Returns the matrix equivalent to applying `first`, then `second`
// (matches PDF's own `cm` operator semantics: `cm`'s operand matrix is
// applied first, then the prior CTM).
Mat2D Multiply(const Mat2D &first, const Mat2D &second);

void Transform(const Mat2D &m, double x, double y, double *out_x, double *out_y);

// The page-to-device matrix for a page whose effective (CropBox-
// clipped) box is [llx,lly,urx,ury] (PDF points) and whose /Rotate is
// `rotate` (0/90/180/270, already normalized -- see pdf_document.h),
// scaling to device pixels at `px_per_pt`. Matches PDFium's own
// FPDF_GetPageWidthF/HeightF behavior (width/height already swapped for
// 90/270), not poppler/pdfinfo's convention of reporting raw MediaBox
// dimensions separately from rotation -- see PDFIUM_REMOVAL_PLAN.md
// Phase 5's own note on this distinction.
Mat2D PageToDeviceMatrix(double llx, double lly, double urx, double ury, int rotate, double px_per_pt);

// An RGBA8 (row-major, width*4 bytes/row) render target, opaque white
// background -- matches pdf_doc.h's RenderPage contract.
struct Canvas {
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;

    static Canvas MakeWhite(int width, int height);
};

// One rendered glyph's Unicode text + bounding rect, in whatever
// coordinate space `initial_ctm` established for the interpreter run
// that produced it (PDFIUM_REMOVAL_PLAN.md Phase 11's text extraction --
// pass an identity `initial_ctm` to ExtractContentStreamText to get
// plain, unscaled PDF user-space/point coordinates suited for search).
// `utf8_text` can hold more than one character for a single glyph (a
// ligature glyph's /ToUnicode text is typically multiple characters,
// e.g. "fi") -- all of it shares this one rect, since it came from one
// rendered glyph. The rect uses an isotropic per-axis approximation of
// the glyph's own render matrix, same Scoping decision as glyph
// rendering itself (see pdf_content.cpp's ShowText): exact for
// unrotated pages/text, approximate otherwise.
struct TextGlyph {
    double left = 0, bottom = 0, right = 0, top = 0;
    std::string utf8_text;
};

// Diagnostic tally GetPageContent optionally fills in, so a caller can
// tell a legitimately empty page (no /Contents, or a genuinely empty
// content stream) apart from one that rendered blank because content it
// *should* have had couldn't be resolved/decoded -- an unsupported
// filter, a corrupt/mislocated stream, etc. `streams_failed > 0` is the
// "this page is missing content" signal RenderPage surfaces to the user.
struct PageContentStatus {
    int streams_total = 0;   // stream references /Contents pointed at
    int streams_failed = 0;  // of those, how many failed to resolve or filter-decode
};

// Reads and fully filter-decodes a page's /Contents (a single stream
// reference, or an array of them concatenated with a separating space
// per spec 7.8.2) via pdf_xref.h + pdf_filters.h. Returns an empty
// string (not a failure signal, matching this codebase's tolerant
// convention) if /Contents is missing or every stream in it fails to
// resolve/decode. When `out_status` is non-null it receives the
// resolve/decode tally (see PageContentStatus) for blank-page diagnostics.
std::string GetPageContent(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                            const pdfdoc::Page &page, PageContentStatus *out_status = nullptr);

// Executes one already-decoded content stream against `canvas`,
// starting from `initial_ctm` (typically PageToDeviceMatrix's result,
// or a Form XObject's concatenated matrix for a recursive call) and
// `resources` (the page's own resolved /Resources dict, or a Form
// XObject's own if it has one) for color-space/XObject lookups.
// `doc_data`/`doc_len`/`table` resolve indirect references reached
// while walking Resources (an indirect /ColorSpace entry, a Form/Image
// XObject's stream, etc.). Tolerant of malformed/unsupported content:
// individual bad operators/operands/streams are skipped rather than
// aborting the whole page, matching pdf_doc.h's own documented
// RenderPage contract ("individual bad objects/operators are skipped
// rather than failing the whole page").
void RenderContentStream(const std::string &content, Canvas &canvas, const Mat2D &initial_ctm,
                          const pdfobj::Object &resources, const unsigned char *doc_data, size_t doc_len,
                          const pdfxref::XrefTable &table);

// Runs the exact same interpreter as RenderContentStream (rendering
// still happens into `canvas` -- pass a small throwaway one, e.g.
// Canvas::MakeWhite(1, 1), if only the text is wanted) but additionally
// appends one TextGlyph per glyph shown to `out_glyphs`
// (PDFIUM_REMOVAL_PLAN.md Phase 11). Pass an identity `initial_ctm` to
// get plain PDF user-space/point coordinates (unrotated, unscaled --
// the convention pdf_doc.h's own PdfTextMatch/Search contract expects);
// pass the same matrix RenderContentStream would use for a given
// px_per_pt to get device-pixel-space glyph positions instead.
void ExtractContentStreamText(const std::string &content, Canvas &canvas, const Mat2D &initial_ctm,
                               const pdfobj::Object &resources, const unsigned char *doc_data, size_t doc_len,
                               const pdfxref::XrefTable &table, std::vector<TextGlyph> *out_glyphs);

}  // namespace pdfrender
