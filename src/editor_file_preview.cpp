// File preview float: the small box beside the file tree showing what the
// row under its cursor is -- a text file's first lines (sent from Lua,
// syntax-highlighted there), an image, or a PDF's first page. This file
// classifies paths and decodes pictures; DrawFilePreviewFloat (main.cpp)
// draws the box; kBuiltinFileTree decides what to show and when.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "editor.h"
#include "gfx/platform.h"
#include "image_doc.h"
#include "pdf_doc.h"

namespace {

// The longest side a preview picture is kept at. The float itself is a
// few hundred pixels at most; this leaves room for a HiDPI screen.
constexpr int kPreviewMaxSide = 768;
// How long the cursor has to rest on a picture's row before it is decoded.
constexpr double kPreviewDecodeDelay = 0.12;
// Files bigger than these are not decoded for a preview at all.
constexpr std::uintmax_t kPreviewMaxImageBytes = 64ull << 20;
constexpr std::uintmax_t kPreviewMaxPdfBytes = 256ull << 20;
constexpr size_t kPreviewCacheSize = 16;

long long FileMtime(const std::string &path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<long long>(t.time_since_epoch().count());
}

bool ReadWholeFile(const std::string &path, std::vector<unsigned char> *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Box-filter downscale of RGBA8 so the longer side is at most `max_side`
// (each output pixel averages the source pixels it covers, so fine detail
// such as text on a PDF page stays readable instead of aliasing away).
void DownscaleRgba(std::vector<unsigned char> &rgba, int &w, int &h, int max_side) {
    if (w <= 0 || h <= 0 || std::max(w, h) <= max_side) return;
    const double scale = static_cast<double>(max_side) / static_cast<double>(std::max(w, h));
    const int nw = std::max(1, static_cast<int>(std::lround(static_cast<double>(w) * scale)));
    const int nh = std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * scale)));
    std::vector<unsigned char> out(static_cast<size_t>(nw) * static_cast<size_t>(nh) * 4);
    for (int y = 0; y < nh; y++) {
        const int sy0 = y * h / nh;
        const int sy1 = std::max(sy0 + 1, (y + 1) * h / nh);
        for (int x = 0; x < nw; x++) {
            const int sx0 = x * w / nw;
            const int sx1 = std::max(sx0 + 1, (x + 1) * w / nw);
            unsigned sum[4] = {0, 0, 0, 0};
            for (int sy = sy0; sy < sy1; sy++) {
                const unsigned char *p = &rgba[(static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx0)) * 4];
                for (int sx = sx0; sx < sx1; sx++, p += 4) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                }
            }
            const unsigned n = static_cast<unsigned>((sx1 - sx0) * (sy1 - sy0));
            unsigned char *q = &out[(static_cast<size_t>(y) * static_cast<size_t>(nw) + static_cast<size_t>(x)) * 4];
            for (int c = 0; c < 4; c++) q[c] = static_cast<unsigned char>(sum[c] / n);
        }
    }
    rgba = std::move(out);
    w = nw;
    h = nh;
}

std::string HumanSize(std::uintmax_t bytes) {
    char buf[32];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < (1ull << 20)) {
        std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < (1ull << 30)) {
        std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / 1048576.0);
    } else {
        std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(bytes) / 1073741824.0);
    }
    return buf;
}

}  // namespace

std::string Editor::FilePreviewKind(const std::string &path) {
    std::error_code ec;
    const auto st = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(st)) return "missing";
    if (std::filesystem::is_directory(st)) return "dir";
    if (IsPdfPath(path)) return "pdf";
    if (IsViewerImagePath(path)) return "image";
    // A NUL byte in the first few KB is the usual "this is not text" test.
    std::ifstream in(path, std::ios::binary);
    if (!in) return "missing";
    char buf[8192];
    in.read(buf, sizeof buf);
    const std::streamsize n = in.gcount();
    for (std::streamsize i = 0; i < n; i++) {
        if (buf[i] == '\0') return "binary";
    }
    return "text";
}

void Editor::SetFilePreviewText(int anchor_buffer, const std::string &title, const std::string &text,
                                std::vector<PickerHlSpan> spans) {
    file_preview_.kind = FilePreview::Kind::Text;
    file_preview_.anchor_buffer = anchor_buffer;
    file_preview_.title = title;
    file_preview_.text = text;
    file_preview_.spans = std::move(spans);
    file_preview_.pending_path.clear();
    file_preview_.rgba.clear();
    file_preview_.w = file_preview_.h = 0;
    file_preview_.info.clear();
}

void Editor::SetFilePreviewVisual(int anchor_buffer, const std::string &title, const std::string &path) {
    file_preview_.kind = FilePreview::Kind::Image;
    file_preview_.anchor_buffer = anchor_buffer;
    file_preview_.title = title;
    file_preview_.text.clear();
    file_preview_.spans.clear();
    file_preview_.rgba.clear();
    file_preview_.w = file_preview_.h = 0;
    file_preview_.info.clear();
    file_preview_.generation++;
    // Already decoded (moving back over a row): show it straight away.
    const long long mtime = FileMtime(path);
    for (size_t i = 0; i < file_preview_cache_.size(); i++) {
        FilePreviewCacheEntry &e = file_preview_cache_[i];
        if (e.path != path || e.mtime != mtime) continue;
        file_preview_.rgba = e.rgba;
        file_preview_.w = e.w;
        file_preview_.h = e.h;
        file_preview_.info = e.info;
        file_preview_.pending_path.clear();
        // Most recently used goes last.
        std::rotate(file_preview_cache_.begin() + static_cast<std::ptrdiff_t>(i),
                    file_preview_cache_.begin() + static_cast<std::ptrdiff_t>(i) + 1, file_preview_cache_.end());
        return;
    }
    file_preview_.pending_path = path;
    file_preview_.requested_at = gfx::GetTime();
}

void Editor::ClearFilePreview() {
    file_preview_.kind = FilePreview::Kind::None;
    file_preview_.anchor_buffer = -1;
    file_preview_.pending_path.clear();
    file_preview_.rgba.clear();
    file_preview_.text.clear();
    file_preview_.spans.clear();
    file_preview_.w = file_preview_.h = 0;
    file_preview_.generation++;
}

void Editor::TickFilePreview() {
    if (file_preview_.kind != FilePreview::Kind::Image || file_preview_.pending_path.empty()) return;
    if (gfx::GetTime() - file_preview_.requested_at < kPreviewDecodeDelay) return;
    const std::string path = file_preview_.pending_path;
    file_preview_.pending_path.clear();

    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    std::string info;
#if defined(__EMSCRIPTEN__)
    info = "(no picture preview in the web build)";
#else
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    std::vector<unsigned char> bytes;
    if (IsPdfPath(path)) {
        if (ec || size > kPreviewMaxPdfBytes) {
            info = "PDF, " + HumanSize(size) + " (too large to preview)";
        } else if (!ReadWholeFile(path, &bytes)) {
            info = "(cannot open)";
        } else {
            PdfDoc doc;
            if (!doc.LoadFromMemory(bytes.data(), bytes.size()) || doc.PageCount() <= 0) {
                info = "PDF (cannot read: " + doc.Error() + ")";
            } else {
                // Rendered straight at roughly preview size rather than at
                // full resolution and shrunk: a big page at print scale is
                // most of the cost of a preview.
                const double long_pt = std::max(doc.PageWidthPt(0), doc.PageHeightPt(0));
                const float px_per_pt = long_pt > 0 ? static_cast<float>(kPreviewMaxSide / long_pt) : 1.0f;
                if (doc.RenderPage(0, px_per_pt, rgba, w, h)) {
                    const int pages = doc.PageCount();
                    info = "PDF, " + std::to_string(pages) + (pages == 1 ? " page" : " pages");
                } else {
                    info = "PDF (cannot render the first page)";
                }
            }
        }
    } else {
        if (ec || size > kPreviewMaxImageBytes) {
            info = HumanSize(size) + " (too large to preview)";
        } else if (!ReadWholeFile(path, &bytes)) {
            info = "(cannot open)";
        } else {
            ImageDoc doc;
            std::string error;
            if (DecodeViewerImage(path, bytes.data(), bytes.size(), doc, &error) && doc.Pixels() != nullptr) {
                w = doc.Width();
                h = doc.Height();
                const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
                rgba.assign(doc.Pixels(), doc.Pixels() + n);
                info = IsSvgPath(path) ? "SVG" : std::to_string(w) + "x" + std::to_string(h);
            } else {
                info = "(cannot decode: " + error + ")";
            }
        }
    }
    DownscaleRgba(rgba, w, h, kPreviewMaxSide);
#endif

    {
        file_preview_.rgba = rgba;
        file_preview_.w = w;
        file_preview_.h = h;
        file_preview_.info = info;
        file_preview_.generation++;
    }
    if (!rgba.empty()) {
        if (file_preview_cache_.size() >= kPreviewCacheSize) file_preview_cache_.erase(file_preview_cache_.begin());
        file_preview_cache_.push_back({path, FileMtime(path), std::move(rgba), w, h, info});
    }
}
