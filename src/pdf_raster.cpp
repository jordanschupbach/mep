#include "pdf_raster.h"

#include "pdf_content.h"
#include "pdf_document.h"
#include "png_codec.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

// Built on the low-level PDF modules rather than the pdf_doc.h facade (the
// same choice mep-pdf-probe makes), so the mep-tex helper that links this
// pulls in no annotation/outline/a11y machinery. The composition mirrors
// PdfDoc::RenderPage -- effective box, PageToDeviceMatrix, and the 8192px
// ceiling enforced by lowering the scale -- so these pixels are the PDF
// pane's own.
bool PdfRasterizeToPng(const std::string &pdf_path, const std::string &png_path, double dpi, int page,
                       std::string *err) {
    auto fail = [err](const std::string &why) {
        if (err) *err = why;
        return false;
    };
    if (dpi <= 0) return fail("bad resolution");
    std::ifstream in(pdf_path, std::ios::binary);
    if (!in) return fail("cannot open " + pdf_path);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto *bytes = reinterpret_cast<const unsigned char *>(data.data());
    pdfdoc::PdfDocument doc;
    doc.Load(bytes, data.size());
    if (doc.PageCount() == 0) return fail("not a readable PDF: " + pdf_path);
    const pdfdoc::Page *p = doc.GetPage(page);
    if (!p) return fail("no page " + std::to_string(page + 1) + " in " + pdf_path);

    constexpr int kMaxRasterPx = 8192;
    const double w_pt = doc.PageWidthPt(page), h_pt = doc.PageHeightPt(page);
    double scale = dpi / 72.0;
    if (w_pt > 0 && h_pt > 0) scale = std::min(scale, std::min(kMaxRasterPx / w_pt, kMaxRasterPx / h_pt));
    const int w = std::max(1, std::min(kMaxRasterPx, static_cast<int>(std::lround(w_pt * scale))));
    const int h = std::max(1, std::min(kMaxRasterPx, static_cast<int>(std::lround(h_pt * scale))));

    pdfrender::Canvas canvas = pdfrender::Canvas::MakeWhite(w, h);
    const pdfrender::Mat2D ctm = pdfrender::PageToDeviceMatrix(p->effective_box[0], p->effective_box[1],
                                                                p->effective_box[2], p->effective_box[3], p->rotate,
                                                                scale);
    pdfrender::PageContentStatus status;
    const std::string content = pdfrender::GetPageContent(bytes, data.size(), doc.Xref(), *p, &status);
    pdfrender::RenderContentStream(content, canvas, ctm, p->resources, bytes, data.size(), doc.Xref());
    if (status.streams_total > 0 && status.streams_failed >= status.streams_total)
        return fail("page " + std::to_string(page + 1) + " has no decodable content");

    const std::string png = png::Encode(w, h, 4, canvas.rgba.data(), w * 4);
    if (png.empty()) return fail("PNG encode failed");
    std::ofstream out(png_path, std::ios::binary);
    out.write(png.data(), static_cast<std::streamsize>(png.size()));
    if (!out) return fail("cannot write " + png_path);
    return true;
}
