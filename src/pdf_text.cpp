#include "pdf_text.h"

#include "pdf_content.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>

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
    for (const pdfrender::TextGlyph &g : glyphs)
        out.push_back(GlyphBox{g.left, g.top, g.right, g.bottom, g.utf8_text, g.actual_text});
    return out;
}

std::string ExpandLigatures(const std::string &s) {
    static const char *const kLetters[] = {"ff", "fi", "fl", "ffi", "ffl"};
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c0 = static_cast<unsigned char>(s[i]);
        if (i + 2 < s.size() && c0 == 0xEF && static_cast<unsigned char>(s[i + 1]) == 0xAC &&
            static_cast<unsigned char>(s[i + 2]) >= 0x80 && static_cast<unsigned char>(s[i + 2]) <= 0x84) {
            out += kLetters[static_cast<unsigned char>(s[i + 2]) - 0x80];
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool EndsWithLetterHyphen(const std::string &s) {
    return s.size() >= 2 && s.back() == '-' && std::isalpha(static_cast<unsigned char>(s[s.size() - 2])) != 0;
}

std::string JoinGlyphText(const std::vector<GlyphBox> &glyphs, const std::vector<int> &indices) {
    std::string text;
    bool have_prev = false;
    GlyphBox prev;
    std::string last_actual;
    for (int gi : indices) {
        if (gi < 0 || gi >= static_cast<int>(glyphs.size())) continue;
        const GlyphBox &g = glyphs[static_cast<size_t>(gi)];
        // A sequence with /ActualText reads as that text, once -- every
        // glyph in the sequence carries the same string.
        const std::string piece = ExpandLigatures(g.actual_text.empty() ? g.text : g.actual_text);
        const bool repeat = !g.actual_text.empty() && have_prev && g.actual_text == last_actual;
        last_actual = g.actual_text;
        if (have_prev) {
            const double height = std::max(g.top - g.bottom, 1e-6);
            const double prev_height = std::max(prev.top - prev.bottom, 1e-6);
            const double centre = (g.top + g.bottom) / 2, prev_centre = (prev.top + prev.bottom) / 2;
            const bool new_line = std::fabs(centre - prev_centre) > 0.5 * std::max(height, prev_height);
            const bool gap = !new_line && g.left - prev.right > 0.25 * std::max(height, prev_height);
            if (new_line && EndsWithLetterHyphen(text) && !piece.empty() &&
                std::islower(static_cast<unsigned char>(piece[0]))) {
                text.pop_back();  // "investiga-" + "tor" is one word
            } else if (new_line) {
                if (!text.empty() && text.back() == ' ') text.pop_back();
                text += '\n';
            } else if (gap && !text.empty() && text.back() != ' ' && text.back() != '\n') {
                text += ' ';
            }
        }
        if (!repeat) text += piece;
        prev = g;
        have_prev = true;
    }
    return text;
}

std::vector<GlyphRow> PageGlyphRows(const std::vector<GlyphBox> &glyphs) {
    std::vector<GlyphRow> rows;
    if (glyphs.empty()) return rows;

    // The page's dominant text size, which every threshold below is a
    // fraction of. Median rather than mean so a page's running header,
    // footnotes and footer (all smaller) don't drag it.
    std::vector<double> heights;
    heights.reserve(glyphs.size());
    for (const GlyphBox &g : glyphs) heights.push_back(std::max(g.top - g.bottom, 1e-6));
    std::nth_element(heights.begin(), heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2), heights.end());
    const double h_med = heights[heights.size() / 2];

    // Step 1: bucket glyphs by baseline. Glyphs set on the same line of
    // type share a baseline exactly, so this needs only enough slack to
    // absorb a producer's rounding.
    struct Group {
        double base = 0;
        std::vector<int> glyphs;
    };
    std::vector<int> order(glyphs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int l, int r) { return glyphs[static_cast<size_t>(l)].bottom > glyphs[static_cast<size_t>(r)].bottom; });
    std::vector<Group> groups;
    for (int gi : order) {
        const double base = glyphs[static_cast<size_t>(gi)].bottom;
        if (!groups.empty() && groups.back().base - base <= 0.25 * h_med) {
            groups.back().glyphs.push_back(gi);
        } else {
            groups.push_back(Group{base, {gi}});
        }
    }

    // Step 2: fold satellites in. A sub/superscript, or the summation
    // sign it hangs off, is a *small* group close to a bigger one; two
    // lines of running text are two big groups a full line pitch apart.
    // Folding smallest-first into the nearest group at least its own
    // size, and only within 0.8 of the dominant text height, keeps those
    // two cases apart: leading is never that tight (a line pitch runs
    // ~1.3x the glyph box height even in dense typesetting), while a
    // subscript drops well under it.
    //
    // Smallest group first each round, so a sub-subscript settles onto
    // its subscript before that subscript settles onto the main line.
    // `host_must_be_bigger` is what stops two full text lines from
    // collapsing into each other: neither is a satellite of the other.
    auto fold = [&](double max_dist, bool host_must_be_bigger, size_t satellite_max) {
        for (;;) {
            std::vector<size_t> by_size(groups.size());
            for (size_t i = 0; i < by_size.size(); ++i) by_size[i] = i;
            std::stable_sort(by_size.begin(), by_size.end(),
                             [&](size_t l, size_t r) { return groups[l].glyphs.size() < groups[r].glyphs.size(); });
            bool merged = false;
            for (size_t from : by_size) {
                if (groups[from].glyphs.size() > satellite_max) continue;
                size_t to = groups.size();
                double best = max_dist;
                for (size_t j = 0; j < groups.size(); ++j) {
                    if (j == from) continue;
                    if (host_must_be_bigger && groups[j].glyphs.size() < groups[from].glyphs.size()) continue;
                    if (!host_must_be_bigger && groups[j].glyphs.size() <= satellite_max) continue;
                    const double d = std::fabs(groups[j].base - groups[from].base);
                    if (d > best) continue;
                    // A tie (equidistant neighbours above and below) goes
                    // to the bigger of the two, so the result doesn't
                    // depend on which was seen first.
                    if (d < best || to == groups.size() || groups[j].glyphs.size() > groups[to].glyphs.size()) {
                        best = d;
                        to = j;
                    }
                }
                if (to == groups.size()) continue;
                groups[to].glyphs.insert(groups[to].glyphs.end(), groups[from].glyphs.begin(), groups[from].glyphs.end());
                groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(from));
                merged = true;
                break;
            }
            if (!merged) return;
        }
    };
    fold(0.8 * h_med, true, std::numeric_limits<size_t>::max());
    // A lone leftover -- the index under a summation that ended up
    // further from its row than anything else is from anything -- joins
    // the nearest real row rather than becoming a row of its own that
    // the caret would stop on for one keystroke.
    fold(1.3 * h_med, false, 2);

    // Step 3: order, and classify.
    rows.reserve(groups.size());
    for (Group &g : groups) {
        std::sort(g.glyphs.begin(), g.glyphs.end(),
                  [&](int l, int r) { return glyphs[static_cast<size_t>(l)].left < glyphs[static_cast<size_t>(r)].left; });
        GlyphRow row;
        row.glyphs = std::move(g.glyphs);
        row.top = glyphs[static_cast<size_t>(row.glyphs.front())].top;
        row.bottom = glyphs[static_cast<size_t>(row.glyphs.front())].bottom;
        for (int gi : row.glyphs) {
            row.top = std::max(row.top, glyphs[static_cast<size_t>(gi)].top);
            row.bottom = std::min(row.bottom, glyphs[static_cast<size_t>(gi)].bottom);
        }
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const GlyphRow &l, const GlyphRow &r) { return l.top > r.top; });

    // The text block's own margins: the row start and row end shared by
    // more rows than any other, which on a page of justified prose is
    // the measure itself.
    // The value the most entries agree on, to within `tol`. Ties keep
    // the lowest, which only matters for a page too sparse to have a
    // real margin anyway.
    auto modal = [](std::vector<double> &vals, double tol) {
        if (vals.empty()) return 0.0;
        std::sort(vals.begin(), vals.end());
        double best = vals.front();
        size_t best_n = 0;
        for (size_t i = 0; i < vals.size();) {
            size_t j = i;
            while (j < vals.size() && vals[j] - vals[i] <= tol) ++j;
            if (j - i > best_n) {
                best_n = j - i;
                best = vals[i];
            }
            i = j;
        }
        return best;
    };
    std::vector<double> lefts, rights;
    lefts.reserve(rows.size());
    rights.reserve(rows.size());
    for (const GlyphRow &r : rows) {
        lefts.push_back(glyphs[static_cast<size_t>(r.glyphs.front())].left);
        double right = 0;
        for (int gi : r.glyphs) right = std::max(right, glyphs[static_cast<size_t>(gi)].right);
        rights.push_back(right);
    }
    // A point of slack on the margins: justified text lines up exactly,
    // but a producer's rounding can wobble the odd row. `modal` sorts
    // what it is given, so it gets copies -- `lefts`/`rights` stay
    // parallel to `rows` for the per-row test below.
    std::vector<double> lefts_sorted = lefts, rights_sorted = rights;
    const double margin_l = modal(lefts_sorted, 1.0);
    const double margin_r = modal(rights_sorted, 1.0);

    for (size_t i = 0; i < rows.size(); ++i) {
        GlyphRow &row = rows[i];
        // How far off the row's own dominant baseline its furthest glyph
        // sits. Running text with a subscript lands around 0.35 of the
        // text height; a displayed equation, with indices under big
        // operators, is comfortably past 0.7.
        std::vector<double> bases;
        bases.reserve(row.glyphs.size());
        for (int gi : row.glyphs) bases.push_back(glyphs[static_cast<size_t>(gi)].bottom);
        // Glyphs set on one line of type share a baseline exactly, so
        // this wants to be essentially an exact count, not a cluster --
        // slack here would let a superscript's own baseline merge with
        // the main one and hide the very thing being looked for.
        const double dom = modal(bases, 0.05 * h_med);
        double spread = 0;
        int off = 0;
        for (double b : bases) {
            spread = std::max(spread, std::fabs(b - dom));
            if (std::fabs(b - dom) > 0.3 * h_med) ++off;
        }
        // ...and whether the row is set apart from the text block at all.
        // A line of prose that happens to carry an inline fraction still
        // runs margin to margin; a displayed equation is indented,
        // centred, or both. Requiring this is what keeps the handful of
        // very formula-dense sentences in a maths book navigable.
        const double left = lefts[i];
        const bool full_measure = left <= margin_l + 0.5 * h_med && rights[i] >= margin_r - 0.5 * h_med;
        row.display_math = spread > 0.6 * h_med && off * 25 >= static_cast<int>(row.glyphs.size()) && !full_measure;
    }
    return rows;
}

std::vector<int> SelectionGlyphRange(const std::vector<GlyphBox> &glyphs, double ax, double ay, double bx,
                                     double by) {
    std::vector<int> out;
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
    out.reserve(hi - lo + 1);
    for (size_t i = lo; i <= hi; ++i) out.push_back(static_cast<int>(i));
    return out;
}

std::vector<PdfTextRectPt> SelectionRects(const std::vector<GlyphBox> &glyphs, double ax, double ay, double bx,
                                          double by) {
    std::vector<PdfTextRectPt> out;
    const std::vector<int> range = SelectionGlyphRange(glyphs, ax, ay, bx, by);
    if (range.empty()) return out;
    const size_t lo = static_cast<size_t>(range.front()), hi = static_cast<size_t>(range.back());

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
