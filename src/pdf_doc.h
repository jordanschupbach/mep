#ifndef MEP_PDF_DOC_H
#define MEP_PDF_DOC_H

#include "pdf_annots.h"
#include "pdf_writer.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// Deliberately raylib-free (same reasoning as image_doc.h): rendering is
// pure CPU-side work (a rendered page's RGBA8 buffer, produced by mep's
// own pdf_content.h content-stream interpreter), so this is usable/
// testable without a GL context. main.cpp is the only place that turns a
// rendered page's RGBA8 buffer into a raylib Texture2D.
//
// Backed by mep's own in-house PDF parser/interpreter/rasterizer
// (pdf_object.h/pdf_xref.h/pdf_crypt.h/pdf_filters.h/pdf_document.h/
// pdf_content.h/pdf_font.h/pdf_encodings.h/pdf_text.h, plus gfx::tt/
// gfx::cff for glyph outlines) rather than a third-party library. This
// was originally backed by PDFium (vendored as a prebuilt shared
// library, not source), substituted in to get a fully working viewer
// (including text) immediately after an even earlier hand-rolled
// implementation covered vector graphics but not text -- see
// PDFIUM_REMOVAL_PLAN.md for that full history and the 14-phase
// from-scratch replacement that made this swap possible. Kept behind
// this exact same interface deliberately, so swapping backends again
// later would again only touch this one file.
// One text-search match, in PDF-point space (page-native units, scale- and
// rotation-independent) -- a match can cover more than one rect when it
// wraps a line (pdf_text.h's own Search merges same-line character boxes
// for us, mirroring the original PDFium-backed contract's own
// FPDFText_CountRects behavior exactly -- see that header's own doc
// comment). Converting to device pixels for a given render scale is a
// separate step (PdfDoc::MatchRectsForPage) so a search doesn't need
// re-running just because the user zoomed.
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

// One entry of the document's own outline (bookmarks) tree -- spec
// 12.3.3, what a PDF viewer's own "bookmarks"/"table of contents" panel
// shows -- flattened out of the tree with a `depth` field rather than
// returned as a real nested structure, since the only consumer (mep's
// Structure sidebar, main.cpp's kBuiltinStructure) already expects
// exactly this "flat list + depth" shape for every other filetype it
// supports (Treesitter symbols, LaTeX sections). See pdf_outline.h for
// the full contract (which destination shapes resolve to a real `page`,
// which don't).
struct PdfOutlineItem {
    std::string title;
    int page = -1;  // 0-based target page index, or -1 if unresolvable (e.g. a named destination)
    int depth = 0;
};

// One Link annotation (spec 12.5.6.5) on a page, already converted to
// device pixels for a given render scale -- same "point-space vs.
// device-pixel-space is a separate conversion step" split as
// PdfTextMatch/PdfHighlightRect above, except a link's own /Rect is
// already in the page's coordinate space (no separate point-space form
// exists to search-then-convert like text matches do), so PageLinks
// below does both steps in one call. See pdf_links.h for exactly which
// action shapes resolve to `target_page` vs. `uri` vs. neither.
struct PdfLinkAnnot {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int target_page = -1;  // 0-based, or -1 if this isn't an internal GoTo (see `uri`)
    std::string uri;       // non-empty for a URI action; empty otherwise
};

// A markup annotation (highlight or sticky note) converted to device
// pixels for a given render scale, ready for the viewer to draw over a
// rasterized page -- the drawable counterpart of the point-space
// pdfannots::PdfAnnot the reader/writer use. Same point-space->device
// split as PdfHighlightRect: AnnotDrawForPage does the conversion so
// main.cpp's draw code never touches the PDF stack.
struct PdfAnnotRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
// One glyph's bounding box in PDF-point space (y-up, top >= bottom), for
// keyboard-caret navigation / visual selection.
struct PdfGlyphBox {
    double left = 0, top = 0, right = 0, bottom = 0;
};
struct PdfAnnotDraw {
    int kind = 0;                        // 0 = highlight, 1 = note
    std::vector<PdfAnnotRect> rects;     // highlight: one device-px rect per quad; note: one icon rect
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;  // overall device-px bbox (the click/hover hit region)
    float r = 1, g = 1, b = 0, a = 0.4f; // colour + opacity
    std::string contents;                // note body / highlight comment (UTF-8)
    bool from_file = true;               // false for a not-yet-saved annotation created this session
    int pending_index = -1;              // index into the `pending` list passed to AnnotDrawForPage (from_file==false), else -1
    int src_obj = 0, src_gen = 0;        // originating file object (from_file==true), for edit/delete; 0 for session annots
    int page = 0;                        // 0-based page this annotation is on (for edit/delete routing)
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
     * @param out_warning Optional output: set to a human-readable message when the page rendered
     *        with missing content (one or more /Contents streams couldn't be resolved/decoded, so
     *        the page is blank or partial); left untouched when the page rendered its content fully.
     *        Lets the caller surface a diagnostic instead of a silently-blank page.
     * @return true on success; false if page_index is out of range or the document failed to load.
     */
    bool RenderPage(int page_index, float px_per_pt, std::vector<unsigned char> &out_rgba, int &out_w,
                     int &out_h, std::string *out_warning = nullptr);

    // Case-insensitive substring search (pdf_text.h's own ASCII-fold
    // matching -- ordinary 'A'-'Z' lowering, applied to both query and
    // extracted text -- see that header's own doc comment for why that's
    // close enough to PDFium's original MATCHCASE-unset default for this
    // codebase's needs) across every page, in page order. Loads and
    // re-extracts each page's text, so this is O(document size) -- call
    // only when the query text actually changes, not per frame/per
    // keystroke.
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

    // The document's own outline (bookmarks) tree, flattened with a
    // depth field -- empty if it has no /Outlines dict at all (most
    // PDFs don't; only ones from tools like LaTeX's hyperref package, or
    // "Save As PDF" from Word/LibreOffice with heading styles, typically
    // do). See pdf_outline.h for exactly which destination shapes
    // resolve to a real page vs. leave `page` at -1.
    /**
     * @brief Returns the document's own outline/bookmarks tree, flattened with a depth field.
     * @return Outline entries in document order; empty if the document has no /Outlines dict, or isn't loaded.
     */
    std::vector<PdfOutlineItem> Outline() const;

    // Every /Subtype /Link annotation on page_index, converted to device
    // pixels at px_per_pt (same convention as RenderPage/MatchRectsForPage)
    // -- empty if the page has no links or isn't loaded. See pdf_links.h
    // for exactly which action shapes resolve to a real target_page/uri.
    /**
     * @brief Returns every Link annotation on one page, converted to device pixels for a given render scale.
     * @param page_index Zero-based page index to read links from.
     * @param px_per_pt Scale factor from PDF points to device pixels, matching RenderPage's convention.
     * @return Link rects/targets on page_index; empty if it has none or the document isn't loaded.
     */
    std::vector<PdfLinkAnnot> PageLinks(int page_index, float px_per_pt) const;

    // Every /Highlight and /Text (sticky-note) markup annotation on
    // page_index, in the page's own unrotated POINT space (unlike
    // PageLinks, which returns device pixels) so the geometry round-trips
    // losslessly through BytesWithAddedAnnots. Empty if the page has none
    // or the document isn't loaded. See pdf_annots.h for the model.
    /**
     * @brief Returns the /Highlight and /Text annotations on one page, in point space.
     * @param page_index Zero-based page index to read annotations from.
     * @return Markup annotations on page_index; empty if it has none or the document isn't loaded.
     */
    std::vector<pdfannots::PdfAnnot> PageAnnots(int page_index) const;

    // Device-pixel draw data for every markup annotation on page_index at
    // px_per_pt: the page's own /Highlight and /Text annotations plus any
    // in `pending` that target this page (annotations created this session
    // but not yet written to the file). `pending` entries come back with
    // from_file==false and pending_index set to their position in
    // `pending`, so the viewer can hit-test/select them. Empty if the page
    // has no annotations or the document isn't loaded.
    /**
     * @brief Returns device-pixel draw data for a page's markup annotations (file + pending), for a given render scale.
     * @param page_index Zero-based page index.
     * @param px_per_pt Scale factor from PDF points to device pixels, matching RenderPage's convention.
     * @param pending Session-created annotations (point space) not yet saved; those on this page are included.
     * @param edits Unsaved edits to file annotations (matched by src_obj) -- their new contents/colour is applied live.
     * @param deletes File annotations scheduled for deletion (by page + object number) -- excluded from the result.
     * @return Drawable annotation overlays in device pixels.
     */
    std::vector<PdfAnnotDraw> AnnotDrawForPage(int page_index, float px_per_pt,
                                               const std::vector<pdfannots::PdfAnnot> &pending,
                                               const std::vector<pdfannots::PdfAnnot> &edits,
                                               const std::vector<pdfwrite::AnnotDelete> &deletes) const;

    // --- text selection (click-drag highlighting) ---

    // Inverse of the page-to-device transform: maps a device-pixel
    // position on page_index (at px_per_pt) back to that page's own point
    // space. Returns false if nothing is loaded or the page is missing.
    /**
     * @brief Maps a device-pixel position on a page back to the page's point space.
     */
    bool DevicePxToPoint(int page_index, float px_per_pt, double dx, double dy, double *out_px, double *out_py) const;

    // Text-selection quads (point space) for a click-drag on page_index
    // between two device-pixel points at px_per_pt (anchor and head, in
    // either order): the union of same-line glyph boxes between the glyphs
    // nearest each end. The page's glyphs are extracted once and cached, so
    // repeated calls during a drag are cheap. Empty if the page has no text.
    /**
     * @brief Returns the point-space selection quads for a device-pixel click-drag on a page.
     */
    std::vector<pdfannots::Quad> SelectionQuads(int page_index, float px_per_pt, double dax, double day, double dbx,
                                                double dby) const;

    // Per-page glyph boxes in reading order, POINT space (y-up, top>=bottom)
    // -- the geometry a keyboard caret navigates and a visual selection is
    // built from. Empty if the page has no text or nothing is loaded.
    /**
     * @brief Returns a page's glyph boxes in reading order (point space).
     */
    std::vector<PdfGlyphBox> PageGlyphs(int page_index) const;

    // Point-space selection quads spanning glyphs [min(gi_a,gi_b) ..
    // max(gi_a,gi_b)] on page_index (reusing the same line-grouping as text
    // search/mouse-selection) -- for a keyboard visual selection.
    /**
     * @brief Returns point-space selection quads spanning a glyph-index range on a page.
     */
    std::vector<pdfannots::Quad> SelectionQuadsForGlyphs(int page_index, int gi_a, int gi_b) const;

    // Converts point-space quads on page_index to device-pixel rects at
    // px_per_pt (one rect per quad bounding box) -- for drawing a live
    // selection overlay before it becomes a saved highlight.
    /**
     * @brief Converts point-space quads to device-pixel rects for a page at a given scale.
     */
    std::vector<PdfAnnotRect> QuadsToDeviceRects(int page_index, float px_per_pt,
                                                 const std::vector<pdfannots::Quad> &quads) const;

    // Produces the full bytes of this PDF with `annots` added to their
    // pages' /Annots as a single appended incremental-update revision
    // (spec 7.5.6) -- the original bytes are preserved verbatim. Pure: the
    // caller writes the result to disk (and typically re-LoadFromMemory's
    // it so the just-added annotations become ordinary existing ones).
    // Returns an empty string with Error() set on refusal (e.g. an
    // encrypted document) or if nothing is loaded.
    /**
     * @brief Returns this PDF's bytes with annotation additions, edits, and deletions applied as one incremental-update revision.
     * @param adds Markup annotations (point space, from_file==false) to append as new objects; each carries its 0-based target page.
     * @param edits Existing file annotations (from_file==true, src_obj>0) re-emitted at their own object number with new contents/colour.
     * @param deletes Annotations to remove from their page's /Annots (by page + object number).
     * @return The new full PDF bytes, or an empty string with Error() set on failure/refusal.
     */
    std::string BytesWithAnnotChanges(const std::vector<pdfannots::PdfAnnot> &adds,
                                      const std::vector<pdfannots::PdfAnnot> &edits,
                                      const std::vector<pdfwrite::AnnotDelete> &deletes);

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
