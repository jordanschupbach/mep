#include "pdf_doc.h"

#include "pdf_content.h"
#include "pdf_document.h"
#include "pdf_outline.h"
#include "pdf_text.h"
#include "pdf_xref.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

bool IsPdfPath(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    // Lowercases a single character for case-insensitive extension comparison.
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == "pdf";
}

// PDFIUM_REMOVAL_PLAN.md Phase 13: PdfDoc::Impl now drives mep's own
// in-house parser/interpreter/rasterizer (pdf_document.h/pdf_content.h/
// pdf_text.h/pdf_xref.h, Phases 2-12) instead of PDFium's FPDF_* API --
// this is now the ONLY implementation, native and Emscripten/wasm alike
// (the old PDFium-backed version had a separate `#if
// defined(__EMSCRIPTEN__)` stub, since PDFium itself is a native-only
// prebuilt shared library CMakeLists.txt never even links for wasm --
// the new engine is pure, portable C++ with no OS/platform dependency
// at all -- confirmed by grepping every module it touches for
// WIN32/__linux__/pthread/std::thread/etc. before writing this, finding
// none -- so wasm gets real PDF support as a side effect of this swap,
// not just a smaller stub).
struct PdfDoc::Impl {
    // Kept alive for the document's whole lifetime: GetPageContent/
    // RenderContentStream/pdftext::Search all re-read directly from
    // these raw bytes on every call, the same lazy-reparse-from-source
    // design this header's own top comment already documented even back
    // when PDFium (which has the identical "doesn't copy the buffer
    // itself" requirement for FPDF_LoadMemDocument) was the backend.
    std::vector<unsigned char> file_data_;
    pdfdoc::PdfDocument document_;
};

PdfDoc::PdfDoc() = default;
PdfDoc::~PdfDoc() = default;
PdfDoc::PdfDoc(PdfDoc &&) noexcept = default;
PdfDoc &PdfDoc::operator=(PdfDoc &&) noexcept = default;

bool PdfDoc::LoadFromMemory(const unsigned char *bytes, size_t len) {
    auto impl = std::make_unique<Impl>();
    impl->file_data_.assign(bytes, bytes + len);
    impl->document_.Load(impl->file_data_.data(), impl->file_data_.size());

    // PDFIUM_REMOVAL_PLAN.md Phase 12: a document that's encrypted but
    // couldn't be unlocked with an empty password reports the same
    // outcome PDFium's own FPDF_ERR_PASSWORD gave -- checked before the
    // generic "no pages" case below since PdfDocument::Load already
    // leaves pages_ empty for this exact situation (both hit
    // PageCount() == 0, but this one has a more specific, useful message).
    if (impl->document_.Xref().IsEncrypted() && !impl->document_.Xref().Encryption()) {
        error_ = "password-protected (encrypted PDFs are not supported)";
        return false;
    }
    if (impl->document_.PageCount() <= 0) {
        error_ = "document has no pages";
        return false;
    }
    impl_ = std::move(impl);
    return true;
}

int PdfDoc::PageCount() const { return impl_ ? impl_->document_.PageCount() : 0; }

double PdfDoc::PageWidthPt(int page_index) const { return impl_ ? impl_->document_.PageWidthPt(page_index) : 0; }

double PdfDoc::PageHeightPt(int page_index) const { return impl_ ? impl_->document_.PageHeightPt(page_index) : 0; }

bool PdfDoc::RenderPage(int page_index, float px_per_pt, std::vector<unsigned char> &out_rgba, int &out_w,
                         int &out_h) {
    if (!impl_) return false;
    const pdfdoc::Page *page = impl_->document_.GetPage(page_index);
    if (!page) return false;

    double scale = static_cast<double>(px_per_pt);
    int w = std::max(1, std::min(8192, static_cast<int>(std::lround(impl_->document_.PageWidthPt(page_index) * scale))));
    int h = std::max(1, std::min(8192, static_cast<int>(std::lround(impl_->document_.PageHeightPt(page_index) * scale))));

    pdfrender::Canvas canvas = pdfrender::Canvas::MakeWhite(w, h);
    pdfrender::Mat2D ctm = pdfrender::PageToDeviceMatrix(page->effective_box[0], page->effective_box[1],
                                                          page->effective_box[2], page->effective_box[3],
                                                          page->rotate, scale);
    std::string content = pdfrender::GetPageContent(impl_->file_data_.data(), impl_->file_data_.size(),
                                                      impl_->document_.Xref(), *page);
    pdfrender::RenderContentStream(content, canvas, ctm, page->resources, impl_->file_data_.data(),
                                    impl_->file_data_.size(), impl_->document_.Xref());

    out_rgba = std::move(canvas.rgba);
    out_w = w;
    out_h = h;
    return true;
}

std::vector<PdfTextMatch> PdfDoc::Search(const std::string &query) const {
    std::vector<PdfTextMatch> results;
    if (!impl_) return results;
    // pdftext::PdfTextMatch/PdfTextRectPt are identically-shaped (same
    // field names/order) but separately-namespaced siblings of this
    // header's own public types -- Phase 11's own deliberate design so
    // this conversion is purely mechanical, not a re-derivation of
    // anything. Kept as an explicit field-by-field copy rather than a
    // reinterpret_cast: distinct aggregate types have no guaranteed
    // layout compatibility in portable C++ even when shaped identically.
    std::vector<pdftext::PdfTextMatch> matches =
        pdftext::Search(impl_->file_data_.data(), impl_->file_data_.size(), impl_->document_, query);
    results.reserve(matches.size());
    for (const pdftext::PdfTextMatch &m : matches) {
        PdfTextMatch out;
        out.page = m.page;
        out.rects_pt.reserve(m.rects_pt.size());
        for (const pdftext::PdfTextRectPt &r : m.rects_pt) out.rects_pt.push_back({r.left, r.top, r.right, r.bottom});
        results.push_back(std::move(out));
    }
    return results;
}

std::vector<PdfHighlightRect> PdfDoc::MatchRectsForPage(int page_index, float px_per_pt,
                                                         const std::vector<PdfTextMatch> &matches) const {
    std::vector<PdfHighlightRect> out;
    if (!impl_) return out;
    std::vector<pdftext::PdfTextMatch> converted;
    converted.reserve(matches.size());
    for (const PdfTextMatch &m : matches) {
        pdftext::PdfTextMatch pm;
        pm.page = m.page;
        pm.rects_pt.reserve(m.rects_pt.size());
        for (const PdfTextRectPt &r : m.rects_pt) pm.rects_pt.push_back({r.left, r.top, r.right, r.bottom});
        converted.push_back(std::move(pm));
    }
    // match_index in the result below indexes back into `converted`,
    // which was built in exactly the same order as `matches` -- so it
    // still correctly indexes into the caller's own `matches` too,
    // preserving this method's documented contract.
    std::vector<pdftext::PdfHighlightRect> hi =
        pdftext::MatchRectsForPage(impl_->document_, page_index, px_per_pt, converted);
    out.reserve(hi.size());
    for (const pdftext::PdfHighlightRect &r : hi) out.push_back({r.match_index, r.x0, r.y0, r.x1, r.y1});
    return out;
}

std::vector<PdfOutlineItem> PdfDoc::Outline() const {
    std::vector<PdfOutlineItem> out;
    if (!impl_) return out;
    std::vector<pdfoutline::OutlineItem> items =
        pdfoutline::GetOutline(impl_->file_data_.data(), impl_->file_data_.size(), impl_->document_.Xref(),
                                impl_->document_);
    out.reserve(items.size());
    for (const pdfoutline::OutlineItem &it : items) out.push_back({it.title, it.page, it.depth});
    return out;
}
