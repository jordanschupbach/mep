#include "pdf_doc.h"

#include <algorithm>
#include <cctype>
#include <cmath>

bool IsPdfPath(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == "pdf";
}

#if defined(__EMSCRIPTEN__)

// PDFium (see below) is vendored as a native shared library via CMake
// FetchContent -- CMakeLists.txt guards that whole fetch behind
// `if(NOT EMSCRIPTEN)`, so there's nothing to link against here. Wiring
// PDFium into the wasm build is possible in principle (pdfium-binaries
// does publish a `pdfium-wasm` package), but reliably side-module-linking
// it requires pinning to the exact Emscripten SDK version it was built
// with, which this project doesn't currently track -- left as a known gap
// rather than risking a fragile, untested integration. PDF viewing is
// native-only for now; the web build reports a clear error instead of
// silently showing a blank pane.
struct PdfDoc::Impl {};
PdfDoc::PdfDoc() = default;
PdfDoc::~PdfDoc() = default;
bool PdfDoc::LoadFromMemory(const unsigned char *, size_t) {
    error_ = "PDF viewing is not available in the web build";
    return false;
}
int PdfDoc::PageCount() const { return 0; }
double PdfDoc::PageWidthPt(int) const { return 0; }
double PdfDoc::PageHeightPt(int) const { return 0; }
bool PdfDoc::RenderPage(int, float, std::vector<unsigned char> &, int &, int &) { return false; }
std::vector<PdfTextMatch> PdfDoc::Search(const std::string &) const { return {}; }
std::vector<PdfHighlightRect> PdfDoc::MatchRectsForPage(int, float, const std::vector<PdfTextMatch> &) const {
    return {};
}

#else

#include "fpdf_text.h"
#include "fpdfview.h"

namespace {

// PDFium is a C library with process-global init/teardown. Init lazily on
// first use, once; never torn down explicitly -- process exit reclaims it,
// the same way this editor never bothers unloading raylib's GL context on
// exit either.
void EnsurePdfiumInitialized() {
    static bool initialized = [] {
        FPDF_InitLibrary();
        return true;
    }();
    (void)initialized;
}

const char *PdfiumErrorString(unsigned long code) {
    switch (code) {
        case FPDF_ERR_SUCCESS: return "no error";
        case FPDF_ERR_FILE: return "file not found or could not be opened";
        case FPDF_ERR_FORMAT: return "not a valid PDF, or the file is corrupted";
        case FPDF_ERR_PASSWORD: return "password-protected (encrypted PDFs are not supported)";
        case FPDF_ERR_SECURITY: return "unsupported security scheme";
        case FPDF_ERR_PAGE: return "page not found or content error";
        default: return "unknown error";
    }
}

// FPDF_WIDESTRING (search queries) is UTF-16LE, null-terminated. Search
// queries are typed interactively, so a straightforward decode (not a
// hardened one) is fine -- malformed UTF-8 bytes are just skipped.
std::vector<unsigned short> Utf8ToUtf16(const std::string &s) {
    std::vector<unsigned short> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c0 = static_cast<unsigned char>(s[i]);
        unsigned int cp;
        int len;
        if ((c0 & 0x80) == 0) { cp = c0; len = 1; }
        else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; len = 2; }
        else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; len = 3; }
        else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; len = 4; }
        else { i++; continue; }
        if (i + static_cast<size_t>(len) > s.size()) break;
        bool valid = true;
        for (int k = 1; k < len; k++) {
            unsigned char ck = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((ck & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (ck & 0x3F);
        }
        if (!valid) { i++; continue; }
        i += static_cast<size_t>(len);
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<unsigned short>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<unsigned short>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<unsigned short>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return out;
}

}  // namespace

struct PdfDoc::Impl {
    // FPDF_LoadMemDocument does not copy the buffer -- it reads from it
    // lazily for the document's whole lifetime, so it must be kept alive
    // here (same reasoning PdfDoc::LoadFromMemory's own doc comment gives
    // for copying the caller's bytes in the first place).
    std::vector<unsigned char> file_data_;
    FPDF_DOCUMENT doc_ = nullptr;

    ~Impl() {
        if (doc_) FPDF_CloseDocument(doc_);
    }
};

PdfDoc::PdfDoc() = default;
PdfDoc::~PdfDoc() = default;

bool PdfDoc::LoadFromMemory(const unsigned char *bytes, size_t len) {
    EnsurePdfiumInitialized();
    auto impl = std::make_unique<Impl>();
    impl->file_data_.assign(bytes, bytes + len);
    impl->doc_ = FPDF_LoadMemDocument(impl->file_data_.data(), static_cast<int>(impl->file_data_.size()), nullptr);
    if (!impl->doc_) {
        error_ = PdfiumErrorString(FPDF_GetLastError());
        return false;
    }
    if (FPDF_GetPageCount(impl->doc_) <= 0) {
        error_ = "document has no pages";
        return false;
    }
    impl_ = std::move(impl);
    return true;
}

int PdfDoc::PageCount() const { return impl_ ? FPDF_GetPageCount(impl_->doc_) : 0; }

double PdfDoc::PageWidthPt(int page_index) const {
    if (!impl_) return 0;
    FPDF_PAGE page = FPDF_LoadPage(impl_->doc_, page_index);
    if (!page) return 0;
    double w = FPDF_GetPageWidthF(page);
    FPDF_ClosePage(page);
    return w;
}

double PdfDoc::PageHeightPt(int page_index) const {
    if (!impl_) return 0;
    FPDF_PAGE page = FPDF_LoadPage(impl_->doc_, page_index);
    if (!page) return 0;
    double h = FPDF_GetPageHeightF(page);
    FPDF_ClosePage(page);
    return h;
}

bool PdfDoc::RenderPage(int page_index, float px_per_pt, std::vector<unsigned char> &out_rgba, int &out_w,
                         int &out_h) {
    if (!impl_ || page_index < 0 || page_index >= FPDF_GetPageCount(impl_->doc_)) return false;
    FPDF_PAGE page = FPDF_LoadPage(impl_->doc_, page_index);
    if (!page) return false;

    // FPDF_GetPageWidthF/HeightF already reflect the page's own /Rotate, so
    // no separate rotation-swap math is needed here (unlike the hand-rolled
    // renderer this replaced) -- rotate=0 below means "no *additional*
    // rotation on top of that."
    int w = std::max(1, std::min(8192, static_cast<int>(std::lround(FPDF_GetPageWidthF(page) * px_per_pt))));
    int h = std::max(1, std::min(8192, static_cast<int>(std::lround(FPDF_GetPageHeightF(page) * px_per_pt))));

    FPDF_BITMAP bitmap = FPDFBitmap_Create(w, h, /*alpha=*/0);  // BGRx, opaque
    if (!bitmap) {
        FPDF_ClosePage(page);
        return false;
    }
    FPDFBitmap_FillRect(bitmap, 0, 0, w, h, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, /*rotate=*/0, FPDF_ANNOT);

    const unsigned char *src = static_cast<const unsigned char *>(FPDFBitmap_GetBuffer(bitmap));
    int stride = FPDFBitmap_GetStride(bitmap);
    out_rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; y++) {
        const unsigned char *row = src + static_cast<size_t>(y) * static_cast<size_t>(stride);
        unsigned char *dst_row = out_rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        for (int x = 0; x < w; x++) {
            // PDFium's byte order is BGRx/BGRA; raylib's R8G8B8A8 wants RGBA.
            dst_row[x * 4 + 0] = row[x * 4 + 2];
            dst_row[x * 4 + 1] = row[x * 4 + 1];
            dst_row[x * 4 + 2] = row[x * 4 + 0];
            dst_row[x * 4 + 3] = 255;
        }
    }

    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);
    out_w = w;
    out_h = h;
    return true;
}

std::vector<PdfTextMatch> PdfDoc::Search(const std::string &query) const {
    std::vector<PdfTextMatch> results;
    if (!impl_ || query.empty()) return results;
    std::vector<unsigned short> wide = Utf8ToUtf16(query);
    if (wide.empty()) return results;
    wide.push_back(0);

    int page_count = FPDF_GetPageCount(impl_->doc_);
    for (int p = 0; p < page_count; p++) {
        FPDF_PAGE page = FPDF_LoadPage(impl_->doc_, p);
        if (!page) continue;
        FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
        if (tp) {
            // flags=0: MATCHCASE unset -> case-insensitive (PDFium's own
            // default, per fpdf_text.h's comment on FPDF_MATCHCASE),
            // matching typical "Ctrl-F in a PDF reader" search behavior.
            FPDF_SCHHANDLE sh = FPDFText_FindStart(tp, wide.data(), 0, 0);
            if (sh) {
                while (FPDFText_FindNext(sh)) {
                    int idx = FPDFText_GetSchResultIndex(sh);
                    int cnt = FPDFText_GetSchCount(sh);
                    if (cnt <= 0) continue;
                    PdfTextMatch m;
                    m.page = p;
                    int nrects = FPDFText_CountRects(tp, idx, cnt);
                    for (int r = 0; r < nrects; r++) {
                        double l, t, rr, b;
                        if (FPDFText_GetRect(tp, r, &l, &t, &rr, &b)) m.rects_pt.push_back({l, t, rr, b});
                    }
                    results.push_back(std::move(m));
                }
                FPDFText_FindClose(sh);
            }
            FPDFText_ClosePage(tp);
        }
        FPDF_ClosePage(page);
    }
    return results;
}

std::vector<PdfHighlightRect> PdfDoc::MatchRectsForPage(int page_index, float px_per_pt,
                                                         const std::vector<PdfTextMatch> &matches) const {
    std::vector<PdfHighlightRect> out;
    if (!impl_) return out;
    FPDF_PAGE page = FPDF_LoadPage(impl_->doc_, page_index);
    if (!page) return out;
    int w = std::max(1, static_cast<int>(std::lround(FPDF_GetPageWidthF(page) * px_per_pt)));
    int h = std::max(1, static_cast<int>(std::lround(FPDF_GetPageHeightF(page) * px_per_pt)));
    for (size_t mi = 0; mi < matches.size(); mi++) {
        if (matches[mi].page != page_index) continue;
        for (const PdfTextRectPt &r : matches[mi].rects_pt) {
            int dx0, dy0, dx1, dy1;
            // FPDF_PageToDevice (not a hand-derived transform) so highlight
            // placement matches RenderPage's actual rasterization exactly,
            // /Rotate included -- same start_x/start_y/size_x/size_y/rotate
            // convention as the FPDF_RenderPageBitmap call in RenderPage.
            if (!FPDF_PageToDevice(page, 0, 0, w, h, 0, r.left, r.top, &dx0, &dy0)) continue;
            if (!FPDF_PageToDevice(page, 0, 0, w, h, 0, r.right, r.bottom, &dx1, &dy1)) continue;
            PdfHighlightRect hr;
            hr.match_index = static_cast<int>(mi);
            hr.x0 = static_cast<float>(std::min(dx0, dx1));
            hr.x1 = static_cast<float>(std::max(dx0, dx1));
            hr.y0 = static_cast<float>(std::min(dy0, dy1));
            hr.y1 = static_cast<float>(std::max(dy0, dy1));
            out.push_back(hr);
        }
    }
    FPDF_ClosePage(page);
    return out;
}

#endif  // __EMSCRIPTEN__
