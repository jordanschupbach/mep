#ifndef MEP_PDF_RASTER_H
#define MEP_PDF_RASTER_H

#include <string>

// mep's in-house stand-in for `pdftoppm -png -r DPI -singlefile`: one page
// of a PDF file rasterized by mep's own renderer (PdfDoc::RenderPage, the
// PDF pane's pixels) and written as a PNG. This is what turns a compiled
// maths fragment or org-babel `latex` block into the image the editor
// shows, so with it the preview pipeline needs no poppler on PATH
// (plans/TEX_ENGINE_PLAN.md, phase 1; `tex_rasterizer = builtin`).

/**
 * @brief Rasterizes one page of a PDF file to a PNG, as `pdftoppm -png -r dpi -singlefile` would.
 * @param pdf_path The PDF to read.
 * @param png_path Where to write the PNG (overwritten).
 * @param dpi Output resolution; the page's points are scaled by dpi / 72.
 * @param page Zero-based page index.
 * @param err Optional: set to a one-line reason on failure.
 * @return True if the PNG was written.
 */
bool PdfRasterizeToPng(const std::string &pdf_path, const std::string &png_path, double dpi, int page = 0,
                       std::string *err = nullptr);

#endif
