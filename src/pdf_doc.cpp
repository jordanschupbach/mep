#include "pdf_doc.h"

#include "pdf_content.h"
#include "pdf_document.h"
#include "pdf_links.h"
#include "pdf_outline.h"
#include "pdf_text.h"
#include "pdf_writer.h"
#include "pdf_xref.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
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
    // Point-space glyph boxes for the page a text selection is currently
    // being dragged on, extracted once and reused across the drag (mutable
    // so the const SelectionQuads can populate it on demand).
    mutable int glyph_cache_page_ = -1;
    mutable std::vector<pdftext::GlyphBox> glyph_cache_;
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
                         int &out_h, std::string *out_warning) {
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
    pdfrender::PageContentStatus status;
    std::string content = pdfrender::GetPageContent(impl_->file_data_.data(), impl_->file_data_.size(),
                                                      impl_->document_.Xref(), *page, &status);
    pdfrender::RenderContentStream(content, canvas, ctm, page->resources, impl_->file_data_.data(),
                                    impl_->file_data_.size(), impl_->document_.Xref());

    // A page whose /Contents stream(s) couldn't be resolved/decoded rendered
    // blank (all failed) or partial (some failed) with no other signal --
    // report it so the caller can warn instead of showing a silent white page.
    if (out_warning && status.streams_failed > 0) {
        bool all = status.streams_failed >= status.streams_total;
        *out_warning = "page " + std::to_string(page_index + 1) + " rendered " +
                       (all ? "blank" : "partially") + ": " + std::to_string(status.streams_failed) + " of " +
                       std::to_string(status.streams_total) +
                       " content stream(s) could not be decoded (unsupported or corrupt PDF feature)";
    }

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

std::vector<PdfLinkAnnot> PdfDoc::PageLinks(int page_index, float px_per_pt) const {
    std::vector<PdfLinkAnnot> out;
    if (!impl_) return out;
    std::vector<pdflinks::PdfLinkAnnot> links = pdflinks::GetPageLinks(
        impl_->file_data_.data(), impl_->file_data_.size(), impl_->document_.Xref(), impl_->document_, page_index,
        px_per_pt);
    out.reserve(links.size());
    for (const pdflinks::PdfLinkAnnot &l : links) out.push_back({l.x0, l.y0, l.x1, l.y1, l.target_page, l.uri});
    return out;
}

std::vector<pdfannots::PdfAnnot> PdfDoc::PageAnnots(int page_index) const {
    if (!impl_) return {};
    return pdfannots::GetPageAnnots(impl_->file_data_.data(), impl_->file_data_.size(), impl_->document_.Xref(),
                                    impl_->document_, page_index);
}

std::vector<PdfAnnotDraw> PdfDoc::AnnotDrawForPage(int page_index, float px_per_pt,
                                                   const std::vector<pdfannots::PdfAnnot> &pending,
                                                   const std::vector<pdfannots::PdfAnnot> &edits,
                                                   const std::vector<pdfwrite::AnnotDelete> &deletes) const {
    std::vector<PdfAnnotDraw> out;
    if (!impl_) return out;
    const pdfdoc::Page *page = impl_->document_.GetPage(page_index);
    if (!page) return out;
    pdfrender::Mat2D m = pdfrender::PageToDeviceMatrix(page->effective_box[0], page->effective_box[1],
                                                       page->effective_box[2], page->effective_box[3], page->rotate,
                                                       static_cast<double>(px_per_pt));

    // Transforms a point-space axis-aligned box (two opposite corners)
    // into a device-pixel PdfAnnotRect (normalized so x0<=x1, y0<=y1).
    auto to_device = [&](double px0, double py0, double px1, double py1) {
        double ax, ay, bx, by;
        pdfrender::Transform(m, px0, py0, &ax, &ay);
        pdfrender::Transform(m, px1, py1, &bx, &by);
        PdfAnnotRect r;
        r.x0 = static_cast<float>(std::min(ax, bx));
        r.x1 = static_cast<float>(std::max(ax, bx));
        r.y0 = static_cast<float>(std::min(ay, by));
        r.y1 = static_cast<float>(std::max(ay, by));
        return r;
    };

    auto emit = [&](const pdfannots::PdfAnnot &a, bool from_file, int pending_index) {
        PdfAnnotDraw d;
        d.kind = a.kind == pdfannots::Kind::Highlight ? 0 : 1;
        d.r = static_cast<float>(a.color[0]);
        d.g = static_cast<float>(a.color[1]);
        d.b = static_cast<float>(a.color[2]);
        d.a = static_cast<float>(a.opacity);
        d.contents = a.contents;
        d.from_file = from_file;
        d.pending_index = pending_index;
        d.src_obj = a.src_obj;
        d.src_gen = a.src_gen;
        d.page = a.page;
        if (a.kind == pdfannots::Kind::Highlight) {
            for (const pdfannots::Quad &q : a.quads) {
                double minx = std::min({q.x1, q.x2, q.x3, q.x4});
                double maxx = std::max({q.x1, q.x2, q.x3, q.x4});
                double miny = std::min({q.y1, q.y2, q.y3, q.y4});
                double maxy = std::max({q.y1, q.y2, q.y3, q.y4});
                d.rects.push_back(to_device(minx, miny, maxx, maxy));
            }
        }
        // Icon/hit rect from /Rect (also the note's device-pixel marker).
        PdfAnnotRect rr = to_device(a.rect[0], a.rect[1], a.rect[2], a.rect[3]);
        if (a.kind == pdfannots::Kind::Text) d.rects.push_back(rr);
        d.bx0 = rr.x0; d.by0 = rr.y0; d.bx1 = rr.x1; d.by1 = rr.y1;
        // For a highlight, widen the bbox to cover all quad rects.
        for (const PdfAnnotRect &r : d.rects) {
            d.bx0 = std::min(d.bx0, r.x0);
            d.by0 = std::min(d.by0, r.y0);
            d.bx1 = std::max(d.bx1, r.x1);
            d.by1 = std::max(d.by1, r.y1);
        }
        out.push_back(std::move(d));
    };

    for (const pdfannots::PdfAnnot &a : PageAnnots(page_index)) {
        // Skip file annotations scheduled for deletion.
        bool deleted = false;
        for (const pdfwrite::AnnotDelete &d : deletes) {
            if (d.page == page_index && d.obj_num == a.src_obj) { deleted = true; break; }
        }
        if (deleted) continue;
        // Apply an unsaved edit (new contents/colour) if one targets this obj.
        pdfannots::PdfAnnot shown = a;
        for (const pdfannots::PdfAnnot &e : edits) {
            if (e.src_obj == a.src_obj && e.page == page_index) { shown = e; break; }
        }
        emit(shown, /*from_file=*/true, -1);
    }
    for (size_t i = 0; i < pending.size(); ++i) {
        if (pending[i].page == page_index) emit(pending[i], /*from_file=*/false, static_cast<int>(i));
    }
    return out;
}

bool PdfDoc::DevicePxToPoint(int page_index, float px_per_pt, double dx, double dy, double *out_px,
                             double *out_py) const {
    if (!impl_) return false;
    const pdfdoc::Page *page = impl_->document_.GetPage(page_index);
    if (!page) return false;
    pdfrender::Mat2D m = pdfrender::PageToDeviceMatrix(page->effective_box[0], page->effective_box[1],
                                                       page->effective_box[2], page->effective_box[3], page->rotate,
                                                       static_cast<double>(px_per_pt));
    // Forward: dx = a*x + c*y + e ; dy = b*x + d*y + f. Invert the 2x2.
    double det = m.a * m.d - m.c * m.b;
    if (std::fabs(det) < 1e-12) return false;
    double ox = dx - m.e, oy = dy - m.f;
    *out_px = (m.d * ox - m.c * oy) / det;
    *out_py = (-m.b * ox + m.a * oy) / det;
    return true;
}

std::vector<pdfannots::Quad> PdfDoc::SelectionQuads(int page_index, float px_per_pt, double dax, double day,
                                                    double dbx, double dby) const {
    std::vector<pdfannots::Quad> out;
    if (!impl_) return out;
    double ax, ay, bx, by;
    if (!DevicePxToPoint(page_index, px_per_pt, dax, day, &ax, &ay)) return out;
    if (!DevicePxToPoint(page_index, px_per_pt, dbx, dby, &bx, &by)) return out;
    if (impl_->glyph_cache_page_ != page_index) {
        const pdfdoc::Page *page = impl_->document_.GetPage(page_index);
        if (!page) return out;
        impl_->glyph_cache_ = pdftext::PageGlyphBoxes(impl_->file_data_.data(), impl_->file_data_.size(),
                                                      impl_->document_.Xref(), *page);
        impl_->glyph_cache_page_ = page_index;
    }
    for (const pdftext::PdfTextRectPt &r : pdftext::SelectionRects(impl_->glyph_cache_, ax, ay, bx, by)) {
        pdfannots::Quad q;
        q.x1 = r.left;  q.y1 = r.top;    q.x2 = r.right; q.y2 = r.top;
        q.x3 = r.left;  q.y3 = r.bottom; q.x4 = r.right; q.y4 = r.bottom;
        out.push_back(q);
    }
    return out;
}

std::vector<PdfGlyphBox> PdfDoc::PageGlyphs(int page_index) const {
    std::vector<PdfGlyphBox> out;
    if (!impl_) return out;
    if (impl_->glyph_cache_page_ != page_index) {
        const pdfdoc::Page *page = impl_->document_.GetPage(page_index);
        if (!page) return out;
        impl_->glyph_cache_ = pdftext::PageGlyphBoxes(impl_->file_data_.data(), impl_->file_data_.size(),
                                                      impl_->document_.Xref(), *page);
        impl_->glyph_cache_page_ = page_index;
    }
    out.reserve(impl_->glyph_cache_.size());
    for (const pdftext::GlyphBox &g : impl_->glyph_cache_) out.push_back({g.left, g.top, g.right, g.bottom});
    return out;
}

std::vector<pdfannots::Quad> PdfDoc::SelectionQuadsForGlyphs(int page_index, int gi_a, int gi_b) const {
    std::vector<pdfannots::Quad> out;
    if (!impl_) return out;
    if (impl_->glyph_cache_page_ != page_index) {
        const pdfdoc::Page *page = impl_->document_.GetPage(page_index);
        if (!page) return out;
        impl_->glyph_cache_ = pdftext::PageGlyphBoxes(impl_->file_data_.data(), impl_->file_data_.size(),
                                                      impl_->document_.Xref(), *page);
        impl_->glyph_cache_page_ = page_index;
    }
    const auto &g = impl_->glyph_cache_;
    if (g.empty()) return out;
    int lo = std::clamp(std::min(gi_a, gi_b), 0, static_cast<int>(g.size()) - 1);
    int hi = std::clamp(std::max(gi_a, gi_b), 0, static_cast<int>(g.size()) - 1);
    // Pass the two glyph centres so SelectionRects picks exactly [lo..hi].
    double ax = (g[static_cast<size_t>(lo)].left + g[static_cast<size_t>(lo)].right) / 2.0;
    double ay = (g[static_cast<size_t>(lo)].top + g[static_cast<size_t>(lo)].bottom) / 2.0;
    double bx = (g[static_cast<size_t>(hi)].left + g[static_cast<size_t>(hi)].right) / 2.0;
    double by = (g[static_cast<size_t>(hi)].top + g[static_cast<size_t>(hi)].bottom) / 2.0;
    for (const pdftext::PdfTextRectPt &r : pdftext::SelectionRects(g, ax, ay, bx, by)) {
        pdfannots::Quad q;
        q.x1 = r.left;  q.y1 = r.top;    q.x2 = r.right; q.y2 = r.top;
        q.x3 = r.left;  q.y3 = r.bottom; q.x4 = r.right; q.y4 = r.bottom;
        out.push_back(q);
    }
    return out;
}

std::vector<PdfAnnotRect> PdfDoc::QuadsToDeviceRects(int page_index, float px_per_pt,
                                                     const std::vector<pdfannots::Quad> &quads) const {
    std::vector<PdfAnnotRect> out;
    if (!impl_) return out;
    const pdfdoc::Page *page = impl_->document_.GetPage(page_index);
    if (!page) return out;
    pdfrender::Mat2D m = pdfrender::PageToDeviceMatrix(page->effective_box[0], page->effective_box[1],
                                                       page->effective_box[2], page->effective_box[3], page->rotate,
                                                       static_cast<double>(px_per_pt));
    for (const pdfannots::Quad &q : quads) {
        double minx = std::min({q.x1, q.x2, q.x3, q.x4});
        double maxx = std::max({q.x1, q.x2, q.x3, q.x4});
        double miny = std::min({q.y1, q.y2, q.y3, q.y4});
        double maxy = std::max({q.y1, q.y2, q.y3, q.y4});
        double ax, ay, bx, by;
        pdfrender::Transform(m, minx, miny, &ax, &ay);
        pdfrender::Transform(m, maxx, maxy, &bx, &by);
        PdfAnnotRect r;
        r.x0 = static_cast<float>(std::min(ax, bx));
        r.x1 = static_cast<float>(std::max(ax, bx));
        r.y0 = static_cast<float>(std::min(ay, by));
        r.y1 = static_cast<float>(std::max(ay, by));
        out.push_back(r);
    }
    return out;
}

std::string PdfDoc::BytesWithAnnotChanges(const std::vector<pdfannots::PdfAnnot> &adds,
                                          const std::vector<pdfannots::PdfAnnot> &edits,
                                          const std::vector<pdfwrite::AnnotDelete> &deletes) {
    if (!impl_) {
        error_ = "no document loaded";
        return "";
    }
    std::string err;
    std::string bytes = pdfwrite::BuildIncrementalUpdate(impl_->file_data_.data(), impl_->file_data_.size(),
                                                         impl_->document_.Xref(), impl_->document_, adds, edits,
                                                         deletes, std::time(nullptr), &err);
    if (bytes.empty()) error_ = err;  // e.g. "cannot add annotations to an encrypted PDF"
    return bytes;
}
