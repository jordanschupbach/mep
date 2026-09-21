#include "pdf_text.h"

#include "pdf_content.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pdftext {

namespace {

// ASCII-only case fold, applied to both haystack and query before a
// plain byte-wise substring search: since every UTF-8 continuation/lead
// byte for a non-ASCII codepoint is >= 0x80, it can never fall in
// 'A'-'Z' and so is never touched here -- multi-byte sequences pass
// through unchanged and byte offsets into the folded string still line
// up 1:1 with the original.
std::string AsciiLower(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// One page's glyphs, flattened into a searchable string -- see
// pdf_text.h's own top comment for the line/word-gap heuristics used to
// decide where synthetic (rect-less) separators get inserted.
struct PageTextBuild {
    std::string text;
    std::vector<int> char_glyph;       // parallel to `text` (one entry per BYTE): originating glyph index, or -1 for a synthetic separator
    std::vector<bool> glyph_new_line;  // parallel to `glyphs`: true if this glyph starts a new line-group (both text-separator and rect-merge boundary)
};

PageTextBuild BuildPageText(const std::vector<pdfrender::TextGlyph> &glyphs) {
    PageTextBuild b;
    b.glyph_new_line.assign(glyphs.size(), false);
    if (glyphs.empty()) return b;
    b.glyph_new_line[0] = true;

    for (size_t i = 0; i < glyphs.size(); ++i) {
        const pdfrender::TextGlyph &g = glyphs[i];
        double height = std::max(g.top - g.bottom, 1e-6);

        if (i > 0) {
            const pdfrender::TextGlyph &prev = glyphs[i - 1];
            double prev_height = std::max(prev.top - prev.bottom, 1e-6);
            double center = (g.top + g.bottom) / 2.0;
            double prev_center = (prev.top + prev.bottom) / 2.0;
            double line_thresh = 0.5 * std::max(height, prev_height);
            bool new_line = std::fabs(center - prev_center) > line_thresh;
            bool word_gap = false;
            if (!new_line) {
                double gap = g.left - prev.right;
                double word_thresh = 0.25 * std::max(height, prev_height);
                word_gap = gap > word_thresh;
            }
            b.glyph_new_line[i] = new_line;
            if ((new_line || word_gap) && (b.text.empty() || b.text.back() != ' ')) {
                b.text.push_back(' ');
                b.char_glyph.push_back(-1);
            }
        }

        for (char c : g.utf8_text) {
            b.text.push_back(c);
            b.char_glyph.push_back(static_cast<int>(i));
        }
    }
    return b;
}

PageTextBuild ExtractPageBuild(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                                const pdfdoc::Page &page, std::vector<pdfrender::TextGlyph> *out_glyphs) {
    std::string content = pdfrender::GetPageContent(doc_data, doc_len, table, page);
    if (content.empty()) return {};
    pdfrender::Canvas canvas = pdfrender::Canvas::MakeWhite(1, 1);
    pdfrender::ExtractContentStreamText(content, canvas, pdfrender::Mat2D{}, page.resources, doc_data, doc_len,
                                         table, out_glyphs);
    return BuildPageText(*out_glyphs);
}

}  // namespace

std::string ExtractPageText(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                             const pdfdoc::Page &page) {
    std::vector<pdfrender::TextGlyph> glyphs;
    return ExtractPageBuild(doc_data, doc_len, table, page, &glyphs).text;
}

std::vector<PdfTextMatch> Search(const unsigned char *doc_data, size_t doc_len, const pdfdoc::PdfDocument &doc,
                                  const std::string &query) {
    std::vector<PdfTextMatch> results;
    if (query.empty()) return results;
    std::string query_lower = AsciiLower(query);

    for (int p = 0; p < doc.PageCount(); ++p) {
        const pdfdoc::Page *page = doc.GetPage(p);
        if (!page) continue;
        std::vector<pdfrender::TextGlyph> glyphs;
        PageTextBuild build = ExtractPageBuild(doc_data, doc_len, doc.Xref(), *page, &glyphs);
        if (build.text.empty()) continue;
        std::string haystack_lower = AsciiLower(build.text);

        size_t pos = 0;
        while ((pos = haystack_lower.find(query_lower, pos)) != std::string::npos) {
            size_t end = pos + query_lower.size();

            std::vector<int> covered;
            for (size_t i = pos; i < end; ++i) {
                int gi = build.char_glyph[i];
                if (gi < 0) continue;
                if (covered.empty() || covered.back() != gi) covered.push_back(gi);
            }

            if (!covered.empty()) {
                PdfTextMatch m;
                m.page = p;
                size_t k = 0;
                while (k < covered.size()) {
                    size_t j = k + 1;
                    const pdfrender::TextGlyph &g0 = glyphs[static_cast<size_t>(covered[k])];
                    double left = g0.left, right = g0.right, top = g0.top, bottom = g0.bottom;
                    while (j < covered.size() && !build.glyph_new_line[static_cast<size_t>(covered[j])]) {
                        const pdfrender::TextGlyph &g = glyphs[static_cast<size_t>(covered[j])];
                        left = std::min(left, g.left);
                        right = std::max(right, g.right);
                        top = std::max(top, g.top);
                        bottom = std::min(bottom, g.bottom);
                        ++j;
                    }
                    m.rects_pt.push_back(PdfTextRectPt{left, top, right, bottom});
                    k = j;
                }
                results.push_back(std::move(m));
            }

            pos = end > pos ? end : pos + 1;
        }
    }
    return results;
}

std::vector<GlyphBox> PageGlyphBoxes(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                                     const pdfdoc::Page &page) {
    std::vector<GlyphBox> out;
    std::string content = pdfrender::GetPageContent(doc_data, doc_len, table, page);
    if (content.empty()) return out;
    pdfrender::Canvas canvas = pdfrender::Canvas::MakeWhite(1, 1);
    std::vector<pdfrender::TextGlyph> glyphs;
    pdfrender::ExtractContentStreamText(content, canvas, pdfrender::Mat2D{}, page.resources, doc_data, doc_len, table,
                                         &glyphs);
    out.reserve(glyphs.size());
    for (const pdfrender::TextGlyph &g : glyphs) out.push_back(GlyphBox{g.left, g.top, g.right, g.bottom});
    return out;
}

std::vector<PdfTextRectPt> SelectionRects(const std::vector<GlyphBox> &glyphs, double ax, double ay, double bx,
                                          double by) {
    std::vector<PdfTextRectPt> out;
    if (glyphs.empty()) return out;
    // Distance^2 from a point to a glyph box (0 if inside), y-up boxes.
    auto dist2 = [](const GlyphBox &g, double px, double py) {
        double dx = std::max({g.left - px, 0.0, px - g.right});
        double dy = std::max({g.bottom - py, 0.0, py - g.top});
        return dx * dx + dy * dy;
    };
    auto nearest = [&](double px, double py) {
        size_t best = 0;
        double best_d = dist2(glyphs[0], px, py);
        for (size_t i = 1; i < glyphs.size(); ++i) {
            double d = dist2(glyphs[i], px, py);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        return best;
    };
    size_t ai = nearest(ax, ay), bi = nearest(bx, by);
    size_t lo = std::min(ai, bi), hi = std::max(ai, bi);

    // Group the selected run into per-line rects, splitting on the same
    // vertical-center jump Search/BuildPageText use.
    PdfTextRectPt cur;
    bool have = false;
    double prev_center = 0, prev_height = 0;
    for (size_t i = lo; i <= hi; ++i) {
        const GlyphBox &g = glyphs[i];
        double height = std::max(g.top - g.bottom, 1e-6);
        double center = (g.top + g.bottom) / 2.0;
        bool new_line = have && std::fabs(center - prev_center) > 0.5 * std::max(height, prev_height);
        if (!have || new_line) {
            if (have) out.push_back(cur);
            cur = PdfTextRectPt{g.left, g.top, g.right, g.bottom};
            have = true;
        } else {
            cur.left = std::min(cur.left, g.left);
            cur.right = std::max(cur.right, g.right);
            cur.top = std::max(cur.top, g.top);
            cur.bottom = std::min(cur.bottom, g.bottom);
        }
        prev_center = center;
        prev_height = height;
    }
    if (have) out.push_back(cur);
    return out;
}

std::vector<PdfHighlightRect> MatchRectsForPage(const pdfdoc::PdfDocument &doc, int page_index, float px_per_pt,
                                                 const std::vector<PdfTextMatch> &matches) {
    std::vector<PdfHighlightRect> out;
    const pdfdoc::Page *page = doc.GetPage(page_index);
    if (!page) return out;
    pdfrender::Mat2D m = pdfrender::PageToDeviceMatrix(page->effective_box[0], page->effective_box[1],
                                                        page->effective_box[2], page->effective_box[3], page->rotate,
                                                        static_cast<double>(px_per_pt));
    for (size_t mi = 0; mi < matches.size(); ++mi) {
        if (matches[mi].page != page_index) continue;
        for (const PdfTextRectPt &r : matches[mi].rects_pt) {
            double x0, y0, x1, y1;
            pdfrender::Transform(m, r.left, r.top, &x0, &y0);
            pdfrender::Transform(m, r.right, r.bottom, &x1, &y1);
            PdfHighlightRect hr;
            hr.match_index = static_cast<int>(mi);
            hr.x0 = static_cast<float>(std::min(x0, x1));
            hr.x1 = static_cast<float>(std::max(x0, x1));
            hr.y0 = static_cast<float>(std::min(y0, y1));
            hr.y1 = static_cast<float>(std::max(y0, y1));
            out.push_back(hr);
        }
    }
    return out;
}

}  // namespace pdftext
