// mep-pdf-bench -- timings for the PDF paths a reader actually waits on.
//
// Why this exists: nothing in the repo timed the PDF stack. The only
// existing hook was MEP_PDF_PROF=1, which prints a line when a frame
// takes over 100ms -- useful for noticing a stall, useless for telling
// whether a change made extraction faster or slower. That matters most
// for the two paths with a user sitting in front of them: rasterizing a
// page as it scrolls into view, and extracting a page's text the moment
// a selection or an AI prompt needs it.
//
// It also guards a specific risk. Text extraction runs the whole
// content-stream interpreter -- the same one that renders -- and
// `ShowText` has no `text_output` guard around its rasterization, so
// extracting a page still rasterizes every glyph into a 1x1 canvas and
// throws it away. Search does that for every page of the document.
// Having the number written down is what makes fixing that provable
// rather than plausible.
//
// Fixtures come from tools/pdf_make_fixtures.sh (gitignored, per this
// repo's convention) or from whatever PDFs are passed as argv, so the
// same binary serves a quick check and a sweep over a real library.
// Results append to bench_results/history.jsonl and are compared by
// tools/bench_report.py, same as every other bench here.

#include "bench_util.h"
#include "pdf_content.h"
#include "pdf_doc.h"
#include "pdf_document.h"
#include "pdf_text.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

const unsigned char *B(const std::string &s) { return reinterpret_cast<const unsigned char *>(s.data()); }

bool ReadFile(const std::string &path, std::string *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

// Which pages to time. Sampled across the document rather than taken
// from the front: front matter is a title page and a contents list,
// which is not what a reader spends their time on, and on a maths book
// it is the only part with no equations.
std::vector<int> SamplePages(int count, int want) {
    std::vector<int> out;
    if (count <= 0) return out;
    if (count <= want) {
        for (int i = 0; i < count; ++i) out.push_back(i);
        return out;
    }
    const double step = static_cast<double>(count) / want;
    for (int i = 0; i < want; ++i) out.push_back(static_cast<int>(static_cast<double>(i) * step));
    return out;
}

struct Doc {
    std::string name;
    std::string data;
    pdfdoc::PdfDocument doc;
};

// --- the scenarios ---------------------------------------------------

// Rasterize a page at a given scale. 2.0 px/pt is the viewer's own
// baseline (editor.cpp pins rendered_scale at 144/72); 4.0 is what
// rendering at the zoom actually shown on a high-density display would
// cost, which is the trade that work has to pay for.
mep::bench::Result RenderPages(Doc &d, const std::vector<int> &pages, double scale, const char *suffix) {
    mep::bench::Timer t;
    long long n = 0;
    for (int pi : pages) {
        const pdfdoc::Page *p = d.doc.GetPage(pi);
        if (!p) continue;
        const int w = static_cast<int>(d.doc.PageWidthPt(pi) * scale);
        const int h = static_cast<int>(d.doc.PageHeightPt(pi) * scale);
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) continue;
        pdfrender::Canvas canvas = pdfrender::Canvas::MakeWhite(w, h);
        pdfrender::Mat2D ctm = pdfrender::PageToDeviceMatrix(p->effective_box[0], p->effective_box[1],
                                                              p->effective_box[2], p->effective_box[3], p->rotate,
                                                              scale);
        std::string content = pdfrender::GetPageContent(B(d.data), d.data.size(), d.doc.Xref(), *p);
        pdfrender::RenderContentStream(content, canvas, ctm, p->resources, B(d.data), d.data.size(), d.doc.Xref());
        ++n;
    }
    return mep::bench::Result{d.name + "/render_" + suffix, n, t.ElapsedMs()};
}

// Re-render pages at a second scale, having already rendered them at a
// first -- which is exactly what changing the zoom does, and the only
// scenario the shared font/glyph cache can help. The cold `render_*`
// scenarios above deliberately do not: each renders a page once, so
// nothing is there to reuse.
//
// This is the number that decides whether the viewer can afford to
// render at the scale actually on screen instead of magnifying a
// fixed-144dpi raster.
//
// Both scenarios run three times and keep the fastest, because whichever
// runs first pays the process's allocator and page-fault warmup -- on
// the first fixture measured that alone made the warm run look slower
// than the cold one it is supposed to beat.
mep::bench::Result ReRenderAtNewScale(Doc &d, const std::vector<int> &pages, double first, double second) {
    double best = 0;
    long long n = 0;
    for (int rep = 0; rep < 3; ++rep) {
        PdfDoc doc;
        doc.LoadFromMemory(B(d.data), d.data.size());
        std::vector<unsigned char> rgba;
        int w = 0, h = 0;
        // Prime the cache at the first scale, exactly as scrolling the
        // page into view would.
        for (int pi : pages) doc.RenderPage(pi, static_cast<float>(first), rgba, w, h);
        mep::bench::Timer t;
        long long got = 0;
        for (int pi : pages)
            if (doc.RenderPage(pi, static_cast<float>(second), rgba, w, h)) ++got;
        const double ms = t.ElapsedMs();
        if (rep == 0 || ms < best) best = ms;
        n = got;
    }
    return mep::bench::Result{d.name + "/rezoom_warm", n, best};
}

// The same re-render with nothing cached, as the control: the gap
// between this and rezoom_warm is what the shared cache buys.
mep::bench::Result ReRenderNoCache(Doc &d, const std::vector<int> &pages, double scale) {
    double best = 0;
    long long n = 0;
    for (int rep = 0; rep < 3; ++rep) {
        mep::bench::Timer t;
        long long got = 0;
        for (int pi : pages) {
            // A fresh PdfDoc per page, so no cache survives between them.
            PdfDoc doc;
            doc.LoadFromMemory(B(d.data), d.data.size());
            std::vector<unsigned char> rgba;
            int w = 0, h = 0;
            if (doc.RenderPage(pi, static_cast<float>(scale), rgba, w, h)) ++got;
        }
        const double ms = t.ElapsedMs();
        if (rep == 0 || ms < best) best = ms;
        n = got;
    }
    return mep::bench::Result{d.name + "/rezoom_cold", n, best};
}

// Pull a page's glyph boxes out. This is what a click-drag, a keyboard
// selection and an AI prompt all wait on, and it runs the full
// interpreter.
mep::bench::Result ExtractGlyphs(Doc &d, const std::vector<int> &pages) {
    mep::bench::Timer t;
    long long n = 0;
    for (int pi : pages) {
        const pdfdoc::Page *p = d.doc.GetPage(pi);
        if (!p) continue;
        n += static_cast<long long>(
            pdftext::PageGlyphBoxes(B(d.data), d.data.size(), d.doc.Xref(), *p).size());
    }
    return mep::bench::Result{d.name + "/extract_glyphs", n, t.ElapsedMs()};
}

// Group glyphs into visual rows and join them into text -- the pure
// geometry half, with no parsing in it, so a change to the row
// algorithm shows up here and not buried in interpreter time.
mep::bench::Result RowsAndJoin(Doc &d, const std::vector<int> &pages) {
    std::vector<std::vector<pdftext::GlyphBox>> cached;
    std::vector<int> rotates;
    for (int pi : pages) {
        const pdfdoc::Page *p = d.doc.GetPage(pi);
        if (!p) continue;
        cached.push_back(pdftext::PageGlyphBoxes(B(d.data), d.data.size(), d.doc.Xref(), *p));
        rotates.push_back(p->rotate);
    }
    mep::bench::Timer t;
    long long n = 0;
    for (size_t i = 0; i < cached.size(); ++i) {
        std::vector<std::vector<int>> rows;
        for (pdftext::GlyphRow &r : pdftext::PageGlyphRows(cached[i], rotates[i])) rows.push_back(std::move(r.glyphs));
        n += static_cast<long long>(pdftext::JoinGlyphRows(cached[i], rows, rotates[i]).size());
    }
    return mep::bench::Result{d.name + "/rows_and_join", n, t.ElapsedMs()};
}

// A document-wide search, which re-extracts every page. Capped, because
// on a 500-page book this alone runs for minutes -- and that it does is
// itself the point.
mep::bench::Result SearchDoc(Doc &d, int max_pages) {
    const int limit = std::min(d.doc.PageCount(), max_pages);
    mep::bench::Timer t;
    long long n = 0;
    for (int pi = 0; pi < limit; ++pi) {
        const pdfdoc::Page *p = d.doc.GetPage(pi);
        if (!p) continue;
        const std::string text = pdftext::ExtractPageText(B(d.data), d.data.size(), d.doc.Xref(), *p);
        n += static_cast<long long>(text.size());
    }
    return mep::bench::Result{d.name + "/search_scan", n, t.ElapsedMs()};
}

}  // namespace

int main(int argc, char **argv) {
    mep::bench::Session session(argc, argv, "pdf_bench");
    const std::string history = "bench_results/history.jsonl";
    std::vector<std::string> paths = session.Rest();
    // Default corpus: whatever tools/pdf_make_fixtures.sh built. Each one
    // targets a different font and layout shape, which is what makes the
    // spread of these numbers meaningful rather than one page measured
    // four ways.
    if (paths.empty()) {
        const std::string dir = "test/pdf_fixtures";
        std::error_code ec;
        for (const auto &e : std::filesystem::directory_iterator(dir, ec))
            if (e.path().extension() == ".pdf") paths.push_back(e.path().string());
        std::sort(paths.begin(), paths.end());
        if (paths.empty()) {
            std::fprintf(stderr,
                         "no fixtures in %s and no files given.\n"
                         "Run tools/pdf_make_fixtures.sh first (just pdf-fixtures), or pass PDF paths.\n",
                         dir.c_str());
        }
    }

    // One entry per document, loaded only when that entry runs (so --list is free).
    for (const std::string &path : paths) {
        const std::string name = std::filesystem::path(path).stem().string();
        session.Add("pdf/" + name, [path, name, history] {
            Doc d;
            d.name = name;
            if (!ReadFile(path, &d.data)) {
                std::fprintf(stderr, "cannot read %s\n", path.c_str());
                return;
            }
            d.doc.Load(B(d.data), d.data.size());
            if (d.doc.PageCount() == 0) {
                std::fprintf(stderr, "cannot parse %s\n", path.c_str());
                return;
            }
            const std::vector<int> pages = SamplePages(d.doc.PageCount(), 4);
            std::printf("== %s (%d pages, timing %zu)\n", d.name.c_str(), d.doc.PageCount(), pages.size());
            std::vector<mep::bench::Result> results;
            results.push_back(RenderPages(d, pages, 2.0, "2x"));
            results.push_back(RenderPages(d, pages, 4.0, "4x"));
            results.push_back(ReRenderAtNewScale(d, pages, 2.0, 2.5));
            results.push_back(ReRenderNoCache(d, pages, 2.5));
            results.push_back(ExtractGlyphs(d, pages));
            results.push_back(RowsAndJoin(d, pages));
            results.push_back(SearchDoc(d, 8));
            for (const mep::bench::Result &r : results) mep::bench::RecordResult("pdf_bench", history, r);
        });
    }
    return session.Run();
}
