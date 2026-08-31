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
    /**
     * @brief Constructs an empty, unloaded PdfDoc.
     */
    PdfDoc();
    /**
     * @brief Releases the underlying PDF document (and any backend resources it owns), if one was loaded.
     */
    ~PdfDoc();
    /**
     * @brief Deleted: PdfDoc owns non-copyable backend document state, so copying is disallowed.
     */
    PdfDoc(const PdfDoc &) = delete;
    /**
     * @brief Deleted: PdfDoc owns non-copyable backend document state, so copy-assignment is disallowed.
     */
    PdfDoc &operator=(const PdfDoc &) = delete;
    /**
     * @brief Move-constructs from another PdfDoc, transferring ownership of its backend state.
     */
    PdfDoc(PdfDoc &&) noexcept;
    /**
     * @brief Move-assigns from another PdfDoc, transferring ownership of its backend state.
     */
    PdfDoc &operator=(PdfDoc &&) noexcept;

    // Parses the PDF's cross-reference table/trailer and flattens its page
    // tree. Copies `bytes` internally (unlike ImageDoc, which decodes once
    // and discards the source -- PdfDoc keeps parsing lazily from the raw
    // file on every RenderPage call, so the caller's buffer isn't assumed
    // to outlive this call). Returns false (and sets Error()) if the file
    // has no readable xref/trailer/page tree; leaves PageCount() == 0.
    /**
     * @brief Parses a PDF file from an in-memory byte buffer and loads its page tree.
     * @param bytes Pointer to the raw PDF file bytes.
     * @param len Length of the buffer pointed to by bytes, in bytes.
     * @return true on success; false (with Error() set) if the file has no readable xref/trailer/page tree.
     */
    bool LoadFromMemory(const unsigned char *bytes, size_t len);

    /**
     * @brief Returns the number of pages in the loaded document.
     * @return Page count, or 0 if no document is loaded.
     */
    int PageCount() const;
    /**
     * @brief Returns a page's width in PDF points, after accounting for the page's /Rotate.
     * @param page_index Zero-based page index.
     * @return Width in points (1pt = 1/72in), or 0 if no document is loaded or the page can't be opened.
     */
    double PageWidthPt(int page_index) const;   // 1pt = 1/72in; post-/Rotate
    /**
     * @brief Returns a page's height in PDF points, after accounting for the page's /Rotate.
     * @param page_index Zero-based page index.
     * @return Height in points (1pt = 1/72in), or 0 if no document is loaded or the page can't be opened.
     */
    double PageHeightPt(int page_index) const;

    // Renders page_index into out_rgba (resized to out_w*out_h*4 bytes,
    // opaque white background), scaling PDF points to pixels by px_per_pt
    // (e.g. 2.0 == 144dpi). Tolerant of malformed/unsupported content --
    // individual bad objects/operators are skipped rather than failing the
    // whole page; this only returns false for an out-of-range page_index
    // or a doc that failed to load.
    /**
     * @brief Rasterizes one page of the loaded document into an RGBA8 pixel buffer.
     * @param page_index Zero-based page index to render.
     * @param px_per_pt Scale factor from PDF points to output pixels (e.g. 2.0 == 144dpi).
     * @param out_rgba Output buffer, resized to out_w*out_h*4 bytes and filled with the rendered page over an opaque white background.
     * @param out_w Output: rendered width in pixels.
     * @param out_h Output: rendered height in pixels.
     * @return true on success; false if page_index is out of range or the document failed to load.
     */
    bool RenderPage(int page_index, float px_per_pt, std::vector<unsigned char> &out_rgba, int &out_w,
                     int &out_h);

    // Case-insensitive substring search (PDFium's own default
    // FPDFText_FindStart flags -- MATCHCASE unset) across every page, in
    // page order. Loads and re-extracts each page's text, so this is
    // O(document size) -- call only when the query text actually changes,
    // not per frame/per keystroke.
    /**
     * @brief Performs a case-insensitive substring search for query text across every page.
     * @param query The text to search for.
     * @return All matches found, in page order, each with its highlight rects in PDF-point space.
     */
    std::vector<PdfTextMatch> Search(const std::string &query) const;

    // Converts every rect (across all of `matches`) belonging to
    // page_index into device pixels at px_per_pt, in one page-load round
    // trip regardless of how many matches/rects that page has -- intended
    // to be called once per page-render event (a page entering the
    // viewer's small rendered-page window, or a rescale), not per frame.
    /**
     * @brief Converts the point-space rects of every match on one page into device pixels for a given render scale.
     * @param page_index Zero-based page index whose matches should be converted.
     * @param px_per_pt Scale factor from PDF points to device pixels, matching the convention used by RenderPage.
     * @param matches Search results (as returned by Search) to filter down to page_index and convert.
     * @return Highlight rects in device-pixel space for page_index, each tagged with its index into matches.
     */
    std::vector<PdfHighlightRect> MatchRectsForPage(int page_index, float px_per_pt,
                                                     const std::vector<PdfTextMatch> &matches) const;

    /**
     * @brief Returns the error message from the most recent failed LoadFromMemory call.
     * @return Human-readable error description, or an empty string if the last load succeeded.
     */
    const std::string &Error() const { return error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};

// Case-insensitive ".pdf" extension check, mirrors IsImagePath.
/**
 * @brief Checks whether a path's extension is ".pdf" (case-insensitive).
 * @param path File path (or bare filename) to check.
 * @return true if the extension is "pdf" (any case); false otherwise.
 */
bool IsPdfPath(const std::string &path);

#endif
