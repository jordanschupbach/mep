#ifndef MEP_PDF_DOC_H
#define MEP_PDF_DOC_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// Deliberately raylib-free (same reasoning as image_doc.h): rendering is
// pure CPU-side work (a PDFium bitmap copied out to an RGBA8 buffer), so
// this is usable/testable without a GL context. main.cpp is the only place
// that turns a rendered page's RGBA8 buffer into a raylib Texture2D.
//
// Backed by PDFium (see third_party_licenses/pdfium-LICENSE.txt; vendored
// as a prebuilt shared library via CMake FetchContent, not source -- see
// CMakeLists.txt's `pdfium` target) rather than a hand-rolled parser. An
// earlier hand-rolled implementation (object model, xref/xref-stream
// parsing, a stb_truetype-based content-stream rasterizer) covered vector
// graphics but not text; PDFium was substituted in to get a fully working
// viewer (including text) immediately. It's kept behind this exact same
// interface deliberately, so swapping backends again later -- back to the
// hand-rolled parser, or to something else -- only touches this one file.
// One text-search match, in PDF-point space (page-native units, scale- and
// rotation-independent) -- a match can cover more than one rect when it
// wraps a line (PDFium merges same-line/same-font character boxes for us,
// see FPDFText_CountRects's own doc comment). Converting to device pixels
// for a given render scale is a separate step (PdfDoc::MatchRectsForPage)
// so a search doesn't need re-running just because the user zoomed.
struct PdfTextRectPt {
    double left = 0, top = 0, right = 0, bottom = 0;
};
struct PdfTextMatch {
    int page = 0;
    std::vector<PdfTextRectPt> rects_pt;
};

// A search match's highlight rect already converted to device pixels at a
// specific px_per_pt (same convention as RenderPage) for one page --
// match_index indexes back into whatever std::vector<PdfTextMatch> was
// passed to MatchRectsForPage, so the caller can tell "this is the
// currently-selected match" apart from "just another match" for coloring.
struct PdfHighlightRect {
    int match_index = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

class PdfDoc {
public:
    PdfDoc();
    ~PdfDoc();
    PdfDoc(const PdfDoc &) = delete;
    PdfDoc &operator=(const PdfDoc &) = delete;

    // Parses the PDF's cross-reference table/trailer and flattens its page
    // tree. Copies `bytes` internally (unlike ImageDoc, which decodes once
    // and discards the source -- PdfDoc keeps parsing lazily from the raw
    // file on every RenderPage call, so the caller's buffer isn't assumed
    // to outlive this call). Returns false (and sets Error()) if the file
    // has no readable xref/trailer/page tree; leaves PageCount() == 0.
    bool LoadFromMemory(const unsigned char *bytes, size_t len);

    int PageCount() const;
    double PageWidthPt(int page_index) const;   // 1pt = 1/72in; post-/Rotate
    double PageHeightPt(int page_index) const;

    // Renders page_index into out_rgba (resized to out_w*out_h*4 bytes,
    // opaque white background), scaling PDF points to pixels by px_per_pt
    // (e.g. 2.0 == 144dpi). Tolerant of malformed/unsupported content --
    // individual bad objects/operators are skipped rather than failing the
    // whole page; this only returns false for an out-of-range page_index
    // or a doc that failed to load.
    bool RenderPage(int page_index, float px_per_pt, std::vector<unsigned char> &out_rgba, int &out_w,
                     int &out_h);

    // Case-insensitive substring search (PDFium's own default
    // FPDFText_FindStart flags -- MATCHCASE unset) across every page, in
    // page order. Loads and re-extracts each page's text, so this is
    // O(document size) -- call only when the query text actually changes,
    // not per frame/per keystroke.
    std::vector<PdfTextMatch> Search(const std::string &query) const;

    // Converts every rect (across all of `matches`) belonging to
    // page_index into device pixels at px_per_pt, in one page-load round
    // trip regardless of how many matches/rects that page has -- intended
    // to be called once per page-render event (a page entering the
    // viewer's small rendered-page window, or a rescale), not per frame.
    std::vector<PdfHighlightRect> MatchRectsForPage(int page_index, float px_per_pt,
                                                     const std::vector<PdfTextMatch> &matches) const;

    const std::string &Error() const { return error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};

// Case-insensitive ".pdf" extension check, mirrors IsImagePath.
bool IsPdfPath(const std::string &path);

#endif
