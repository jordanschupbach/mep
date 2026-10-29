// PDFIUM_REMOVAL_PLAN.md Phase 11 coverage for pdf_text.h/.cpp: Search's
// case-insensitive substring matching, its line-merged rect grouping
// (mirroring pdf_doc.h's PdfTextMatch/FPDFText_CountRects contract), and
// MatchRectsForPage's device-pixel conversion. Synthetic documents built
// the same "compute offsets from actual string positions" way as
// pdf_document_test.cpp/pdf_content_test.cpp, using standard-14
// (Helvetica) substitution so no embedded FontFile is needed.
//
// Also accepts optional fixture paths (argv) for a real-file spot check
// -- same opt-in convention as pdf_document_test.cpp -- searching for a
// known query per fixture (checked by hand to actually appear in that
// fixture's own test/pdf_fixtures/oracle/<name>/text.txt, `pdftotext`'s
// independent extraction) and confirming Search finds it.

#include "pdf_text.h"

#include "pdf_document.h"
#include "pdf_xref.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

const unsigned char *B(const std::string &s) { return reinterpret_cast<const unsigned char *>(s.data()); }

std::string XrefLine(long long offset, int gen, char kind) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%010lld %05d %c \n", offset, gen, kind);
    return buf;
}

std::string BuildDoc(const std::vector<std::pair<int, std::string>> &objects, int root_num) {
    std::string doc = "%PDF-1.4\n";
    std::vector<std::pair<int, size_t>> offsets;
    int max_num = 0;
    for (const auto &obj : objects) {
        offsets.emplace_back(obj.first, doc.size());
        doc += std::to_string(obj.first) + " 0 obj\n" + obj.second + "\nendobj\n";
        max_num = std::max(max_num, obj.first);
    }
    size_t xref_off = doc.size();
    doc += "xref\n0 " + std::to_string(max_num + 1) + "\n";
    doc += XrefLine(0, 65535, 'f');
    for (int n = 1; n <= max_num; ++n) {
        auto it = std::find_if(offsets.begin(), offsets.end(), [&](const auto &p) { return p.first == n; });
        doc += it == offsets.end() ? XrefLine(0, 0, 'f') : XrefLine(static_cast<long long>(it->second), 0, 'n');
    }
    doc += "trailer\n<< /Size " + std::to_string(max_num + 1) + " /Root " + std::to_string(root_num) + " 0 R >>\n";
    doc += "startxref\n" + std::to_string(xref_off) + "\n%%EOF\n";
    return doc;
}

// One page, MediaBox 0 0 200 100, /F1 mapped to standard-14 Helvetica,
// content stream given verbatim as object 5's own stream body.
std::string OnePageDoc(const std::string &content) {
    return BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 200 100] "
                "/Resources << /Font << /F1 4 0 R >> >> >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"},
            {5, "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream"},
        },
        1);
}

void TestSearchFindsWordCaseInsensitively() {
    std::string doc = OnePageDoc("BT /F1 24 Tf 10 50 Td (Hello World) Tj ET");
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());

    std::vector<pdftext::PdfTextMatch> lower = pdftext::Search(B(doc), doc.size(), document, "world");
    CHECK(lower.size() == 1);
    CHECK(lower[0].page == 0);
    CHECK(lower[0].rects_pt.size() == 1);
    CHECK(lower[0].rects_pt[0].right > lower[0].rects_pt[0].left);
    CHECK(lower[0].rects_pt[0].top > lower[0].rects_pt[0].bottom);

    std::vector<pdftext::PdfTextMatch> upper = pdftext::Search(B(doc), doc.size(), document, "WORLD");
    CHECK(upper.size() == 1);
    CHECK(upper[0].rects_pt[0].left == lower[0].rects_pt[0].left);
    CHECK(upper[0].rects_pt[0].right == lower[0].rects_pt[0].right);
}

void TestSearchMultiWordQuerySpansWordGap() {
    // No literal space glyph needed for the match to succeed across the
    // word gap -- BuildPageText's horizontal-gap heuristic inserts its
    // own synthetic separator between "Hello" and "World" here since
    // they're two separate Tj runs positioned apart via Td, exactly like
    // TJ-kerned real-world text.
    std::string doc = OnePageDoc("BT /F1 24 Tf 10 50 Td (Hello) Tj 70 0 Td (World) Tj ET");
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());

    std::vector<pdftext::PdfTextMatch> m = pdftext::Search(B(doc), doc.size(), document, "hello world");
    CHECK(m.size() == 1);
    CHECK(m[0].rects_pt.size() == 1);  // still one line -> one merged rect
}

void TestSearchNoMatchReturnsEmpty() {
    std::string doc = OnePageDoc("BT /F1 24 Tf 10 50 Td (Hello World) Tj ET");
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    CHECK(pdftext::Search(B(doc), doc.size(), document, "goodbye").empty());
    CHECK(pdftext::Search(B(doc), doc.size(), document, "").empty());
}

void TestSearchLineWrapProducesTwoRects() {
    // Two lines (T* drops to a new line per the font's own /Leading) --
    // a query spanning both must come back as ONE match with TWO rects,
    // matching PDFium's own "a match can cover more than one rect when
    // it wraps a line" contract.
    std::string content = "BT /F1 24 Tf 30 TL 10 80 Td (first) Tj T* (second) Tj ET";
    std::string doc = OnePageDoc(content);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());

    std::vector<pdftext::PdfTextMatch> m = pdftext::Search(B(doc), doc.size(), document, "first second");
    CHECK(m.size() == 1);
    CHECK(m[0].rects_pt.size() == 2);
    // The 2nd line was placed lower on the page (T* moves down), so its
    // rect's top/bottom must sit strictly below the 1st line's.
    CHECK(m[0].rects_pt[1].top < m[0].rects_pt[0].bottom);
}

// The index the `/` picker searches per keystroke gives the same matches
// as a one-off search, and a context snippet around each one up to the
// limit asked for, with the match's span in it.
void TestSearchIndexContext() {
    std::string doc = OnePageDoc("BT /F1 12 Tf 10 50 Td (alpha beta gamma beta delta) Tj ET");
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    const pdftext::SearchIndex index = pdftext::BuildSearchIndex(B(doc), doc.size(), document);
    CHECK(index.pages.size() == 1);

    std::vector<pdftext::PdfTextMatch> once = pdftext::Search(B(doc), doc.size(), document, "BETA");
    std::vector<pdftext::PdfTextMatch> indexed = pdftext::Search(index, "BETA", 1);
    CHECK(once.size() == 2);
    CHECK(indexed.size() == 2);
    CHECK(indexed[1].rects_pt[0].left == once[1].rects_pt[0].left);

    const pdftext::PdfTextMatch &m = indexed[0];
    CHECK(!m.context.empty());
    CHECK(m.context_hl_end - m.context_hl_start == 4);
    CHECK(m.context.substr(static_cast<size_t>(m.context_hl_start), 4) == "beta");
    CHECK(indexed[1].context.empty());  // past the limit of 1
    CHECK(pdftext::Search(index, "").empty());
}

void TestSearchAcrossMultiplePages() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R 4 0 R] /MediaBox [0 0 200 100] "
                "/Resources << /Font << /F1 5 0 R >> >> >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 6 0 R >>"},
            {4, "<< /Type /Page /Parent 2 0 R /Contents 7 0 R >>"},
            {5, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"},
            {6, "<< /Length 10 >>\nstream\nBT /F1 24 Tf 10 50 Td (Apple) Tj ET\nendstream"},
            {7, "<< /Length 10 >>\nstream\nBT /F1 24 Tf 10 50 Td (Banana) Tj ET\nendstream"},
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    CHECK(document.PageCount() == 2);

    std::vector<pdftext::PdfTextMatch> apple = pdftext::Search(B(doc), doc.size(), document, "apple");
    CHECK(apple.size() == 1 && apple[0].page == 0);
    std::vector<pdftext::PdfTextMatch> banana = pdftext::Search(B(doc), doc.size(), document, "banana");
    CHECK(banana.size() == 1 && banana[0].page == 1);
}

void TestMatchRectsForPageScalesWithPxPerPt() {
    std::string doc = OnePageDoc("BT /F1 24 Tf 10 50 Td (Hello World) Tj ET");
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    std::vector<pdftext::PdfTextMatch> matches = pdftext::Search(B(doc), doc.size(), document, "world");
    CHECK(matches.size() == 1);

    std::vector<pdftext::PdfHighlightRect> at1x = pdftext::MatchRectsForPage(document, 0, 1.0f, matches);
    std::vector<pdftext::PdfHighlightRect> at2x = pdftext::MatchRectsForPage(document, 0, 2.0f, matches);
    CHECK(at1x.size() == 1 && at2x.size() == 1);
    CHECK(at1x[0].match_index == 0 && at2x[0].match_index == 0);
    // 2x render scale -> 2x device-pixel rect size (page has no /Rotate,
    // so this is a plain uniform scale, no rotation-induced axis swap).
    float w1 = at1x[0].x1 - at1x[0].x0;
    float w2 = at2x[0].x1 - at2x[0].x0;
    CHECK(w2 > w1 * 1.9f && w2 < w1 * 2.1f);
    // PDF y-up "top" maps to a SMALLER device y (device is y-down,
    // origin top-left) -- confirms MatchRectsForPage didn't just copy
    // point-space y's sign/order through unchanged.
    CHECK(at1x[0].y0 < at1x[0].y1);
}

void TestExtractPageTextContainsShownWords() {
    std::string doc = OnePageDoc("BT /F1 24 Tf 10 50 Td (Hello World) Tj ET");
    pdfxref::XrefTable table;
    table.Load(B(doc), doc.size());
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    const pdfdoc::Page *page = document.GetPage(0);
    CHECK(page != nullptr);
    std::string text = pdftext::ExtractPageText(B(doc), doc.size(), table, *page);
    CHECK(text.find("Hello") != std::string::npos);
    CHECK(text.find("World") != std::string::npos);
}

// Opt-in real-fixture spot check (pass e.g.
// test/pdf_fixtures/tectonic_test.pdf as argv) -- searches for a query
// known (by hand, from this fixture's own oracle/<name>/text.txt) to
// appear in that fixture, confirming Search actually finds it, with a
// page index/rect that both make sense.
void CheckRealFixture(const std::string &path) {
    // Fixtures that deliberately carry no text layer: a scanned page is
    // a stencil image, so there is nothing for Search to find and
    // nothing here to assert. They exist for the render comparison
    // (tools/pdf_accuracy.py render), not this one.
    if (path.find("scan_bitonal") != std::string::npos) {
        std::printf("  %s: no text layer by design, skipped\n", path.c_str());
        return;
    }
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string data = ss.str();

    pdfdoc::PdfDocument document;
    document.Load(B(data), data.size());

    std::string query;
    if (path.find("jpeg_test") != std::string::npos) {
        query = "plain text before the image";
    } else if (path.find("tectonic_test") != std::string::npos || path.find("libreoffice_test") != std::string::npos) {
        query = "quicksort";
    } else if (path.find("degenerate_text") != std::string::npos) {
        // This fixture's text is deliberately nonsense (its font maps
        // every code to one of three glyphs), so the query is nonsense
        // too -- Search still has to find it, which is the point: the
        // extraction machinery works, the file is lying to it.
        query = "DBBB";
    } else if (path.find("bullets_tagged") != std::string::npos) {
        // Inside the item whose /ActualText spans marker and body, so it
        // only matches when that implausible span is ignored rather than
        // honoured -- otherwise the whole item reads as one bullet.
        query = "sits inside one wide span";
    } else if (path.find("bullets_cm") != std::string::npos) {
        // A bulleted item's own body text, which is the thing that was
        // reaching an AI prompt as nothing but the bullet.
        query = "nested list follows this item";
    } else if (path.find("display_math") != std::string::npos) {
        // Prose beside the equations; the equations themselves are
        // checked by tools/pdf_accuracy.py against the oracle, not here.
        query = "summation with limits";
    } else if (path.find("inline_math") != std::string::npos) {
        query = "observation";
    } else if (path.find("twocolumn_header") != std::string::npos) {
        query = "first column";
    } else if (path.find("ligatures_hyphen") != std::string::npos) {
        // Spans a ligature (ffi) and so only matches if /ToUnicode's
        // multi-character text survived extraction.
        query = "difficulties";
    } else if (path.find("rotated") != std::string::npos) {
        // On the landscape page, so it only matches when the page's
        // /Rotate is accounted for in row grouping.
        query = "landscape";
    } else if (path.find("cid_unicode") != std::string::npos) {
        query = "Unicode engine";
    } else {
        query = "the";
    }

    std::vector<pdftext::PdfTextMatch> matches =
        pdftext::Search(reinterpret_cast<const unsigned char *>(data.data()), data.size(), document, query);
    std::printf("  %s: query \"%s\" -> %zu match(es)\n", path.c_str(), query.c_str(), matches.size());
    CHECK(!matches.empty());
    for (const pdftext::PdfTextMatch &m : matches) {
        CHECK(m.page >= 0 && m.page < document.PageCount());
        CHECK(!m.rects_pt.empty());
        std::vector<pdftext::PdfHighlightRect> hi = pdftext::MatchRectsForPage(document, m.page, 2.0f, {m});
        CHECK(!hi.empty());
        std::printf("    page %d: rect (%.1f,%.1f)-(%.1f,%.1f) pt -> device (%.1f,%.1f)-(%.1f,%.1f)\n", m.page,
                    m.rects_pt[0].left, m.rects_pt[0].bottom, m.rects_pt[0].right, m.rects_pt[0].top,
                    static_cast<double>(hi[0].x0), static_cast<double>(hi[0].y0), static_cast<double>(hi[0].x1),
                    static_cast<double>(hi[0].y1));
    }
}

}  // namespace

// One glyph. The text defaults to empty for the fixtures that care
// where a glyph is and not what it says, which is most of them.
pdftext::GlyphBox Box(double left, double top, double right, double bottom, std::string text = "",
                       std::string actual_text = "") {
    return pdftext::GlyphBox{left, top, right, bottom, std::move(text), std::move(actual_text)};
}

// SelectionRects is a pure function over glyph boxes -- test it directly
// with a synthetic two-line layout (point space, y-up), no PDF needed.
void TestSelectionRects() {
    // Line A at y[90,100], glyphs at x = 0,10,20,30 (widths 10). Line B at
    // y[70,80], glyphs at x = 0,10,20.
    std::vector<pdftext::GlyphBox> g = {
        Box(0, 100, 10, 90), Box(10, 100, 20, 90), Box(20, 100, 30, 90), Box(30, 100, 40, 90),  // line A: 0..3
        Box(0, 80, 10, 70),  Box(10, 80, 20, 70),  Box(20, 80, 30, 70),                          // line B: 4..6
    };
    // Select within line A only, from glyph 1 to glyph 2 (points inside them).
    auto r1 = pdftext::SelectionRects(g, 12, 95, 25, 95);
    CHECK(r1.size() == 1);
    CHECK(r1[0].left == 10 && r1[0].right == 30 && r1[0].top == 100 && r1[0].bottom == 90);
    // Select spanning both lines (anchor in A glyph 2, head in B glyph 1) ->
    // two line rects, split on the vertical-center jump.
    auto r2 = pdftext::SelectionRects(g, 22, 95, 12, 75);
    CHECK(r2.size() == 2);
    // Order is anchor..head by glyph index: line A tail (glyphs 2,3) then line B head (glyph 4,5).
    CHECK(r2[0].top == 100 && r2[0].bottom == 90);   // line A run
    CHECK(r2[1].top == 80 && r2[1].bottom == 70);    // line B run
    // Empty glyphs -> empty selection.
    CHECK(pdftext::SelectionRects({}, 0, 0, 1, 1).empty());
}

// --- PageGlyphRows: the visual lines a keyboard caret moves between ---
//
// Geometry throughout: 10pt text, so glyph boxes are 10 high and the
// page's own median height is 10; body lines sit 13pt apart (ordinary
// leading) and run from x=70 to x=430 (the measure).

// Body text in a content stream that emits the running header LAST --
// the shape that makes a caret following reading order teleport to the
// top of the page. Rows must come back top to bottom regardless.
void TestGlyphRowsAreVisualOrderNotReadingOrder() {
    std::vector<pdftext::GlyphBox> g;
    // Two body lines, reading order first.
    for (int i = 0; i < 8; ++i) g.push_back(Box(70.0 + i * 45, 510, 110.0 + i * 45, 500));  // line at y 500
    for (int i = 0; i < 8; ++i) g.push_back(Box(70.0 + i * 45, 497, 110.0 + i * 45, 487));  // line at y 487
    // ...then the running header, way up the page.
    for (int i = 0; i < 8; ++i) g.push_back(Box(70.0 + i * 45, 710, 110.0 + i * 45, 700));  // header at y 700
    const auto rows = pdftext::PageGlyphRows(g);
    CHECK(rows.size() == 3);
    CHECK(rows[0].top == 710);  // header first, because it is topmost
    CHECK(rows[1].top == 510);
    CHECK(rows[2].top == 497);
    CHECK(rows[0].glyphs.front() == 16);  // ...even though it came last in the stream
    CHECK(rows[1].glyphs.front() == 0);
    CHECK(rows[2].glyphs.front() == 8);
    // Every glyph lands in exactly one row.
    size_t total = 0;
    for (const auto &r : rows) total += r.glyphs.size();
    CHECK(total == g.size());
}

// Two ordinary lines of text do not merge, however close their boxes
// come: neither is a satellite of the other.
void TestGlyphRowsKeepAdjacentTextLinesApart() {
    std::vector<pdftext::GlyphBox> g;
    for (int i = 0; i < 10; ++i) g.push_back(Box(70.0 + i * 36, 510, 106.0 + i * 36, 500));
    for (int i = 0; i < 10; ++i) g.push_back(Box(70.0 + i * 36, 497, 106.0 + i * 36, 487));
    const auto rows = pdftext::PageGlyphRows(g);
    CHECK(rows.size() == 2);
    CHECK(rows[0].glyphs.size() == 10 && rows[1].glyphs.size() == 10);
    CHECK(!rows[0].display_math && !rows[1].display_math);
}

// A subscript belongs to the line it hangs off, not to a line of its
// own -- and that line is still ordinary prose, not an equation.
void TestGlyphRowsFoldSubscriptIntoItsLine() {
    std::vector<pdftext::GlyphBox> g;
    for (int i = 0; i < 10; ++i) g.push_back(Box(70.0 + i * 36, 510, 106.0 + i * 36, 500));
    g.push_back(Box(430, 507, 436, 499));  // a subscript hanging off the end: smaller, baseline 1pt lower
    for (int i = 0; i < 10; ++i) g.push_back(Box(70.0 + i * 36, 497, 106.0 + i * 36, 487));
    const auto rows = pdftext::PageGlyphRows(g);
    CHECK(rows.size() == 2);
    CHECK(rows[0].glyphs.size() == 11);  // the subscript joined the upper line
    CHECK(!rows[0].display_math);
}

// A dedicated equation line: indented, short of the measure, and with
// glyphs on three baselines (the main one, a summation, and indices
// under it). That is what gets flagged.
void TestGlyphRowsFlagDisplayEquation() {
    std::vector<pdftext::GlyphBox> g;
    // Body text above and below, establishing the measure and the
    // page's own text height.
    for (int line = 0; line < 6; ++line) {
        const double y = 600.0 - line * 13;
        for (int i = 0; i < 10; ++i) g.push_back(Box(70.0 + i * 36, y + 10, 106.0 + i * 36, y));
    }
    // The equation, indented to x=150 and stopping at x=320.
    for (int i = 0; i < 8; ++i) g.push_back(Box(150.0 + i * 20, 510, 168.0 + i * 20, 500));   // main baseline
    g.push_back(Box(200, 512, 212, 498));                                                      // a tall summation
    g.push_back(Box(202, 499, 210, 491));                                                      // its index, 9pt lower
    g.push_back(Box(240, 499, 248, 491));                                                      // and another
    const auto rows = pdftext::PageGlyphRows(g);
    const pdftext::GlyphRow *eq = nullptr;
    int flagged = 0;
    for (const auto &r : rows) {
        if (!r.display_math) continue;
        ++flagged;
        eq = &r;
    }
    CHECK(flagged == 1);
    CHECK(eq != nullptr && eq->glyphs.size() == 11);  // the indices folded in with it
}

// The same vertical structure, but running margin to margin as prose
// does: ordinary text with inline maths in it, which the caret must
// keep. This is the case the measure test exists for.
void TestGlyphRowsKeepProseWithInlineMath() {
    std::vector<pdftext::GlyphBox> g;
    for (int line = 0; line < 6; ++line) {
        const double y = 600.0 - line * 13;
        for (int i = 0; i < 10; ++i) g.push_back(Box(70.0 + i * 36, y + 10, 106.0 + i * 36, y));
    }
    for (int i = 0; i < 10; ++i) g.push_back(Box(70.0 + i * 36, 510, 106.0 + i * 36, 500));  // full measure
    g.push_back(Box(200, 512, 212, 498));
    g.push_back(Box(202, 499, 210, 491));
    g.push_back(Box(240, 499, 248, 491));
    const auto rows = pdftext::PageGlyphRows(g);
    for (const auto &r : rows) CHECK(!r.display_math);
}

void TestGlyphRowsEmptyInput() { CHECK(pdftext::PageGlyphRows({}).empty()); }

// --- JoinGlyphText: a selection as copyable, quotable text -----------
//
// Same 10pt geometry as the row tests above: boxes 10 high, body lines
// 13pt apart, a word gap anything wider than 2.5pt.

// Builds one line of glyphs left to right, each `w` wide with `gap`
// between them, so a test says what it means rather than counting
// coordinates.
void PushWord(std::vector<pdftext::GlyphBox> &g, const char *text, double x, double y, double w = 5,
               double gap = 0) {
    for (const char *c = text; *c; ++c) {
        // One box per BYTE would split a multi-byte character, so the
        // caller passes whole pieces instead; this is the ASCII path.
        g.push_back(Box(x, y + 10, x + w, y, std::string(1, *c), ""));
        x += w + gap;
    }
}

std::vector<int> AllOf(const std::vector<pdftext::GlyphBox> &g) {
    std::vector<int> idx(g.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int>(i);
    return idx;
}

void TestJoinGlyphTextWordsAndGaps() {
    std::vector<pdftext::GlyphBox> g;
    PushWord(g, "one", 0, 500);     // x 0..15
    PushWord(g, "two", 20, 500);    // 5pt gap -> a word break
    const std::string out = pdftext::JoinGlyphText(g, AllOf(g));
    CHECK(out == "one two");
    // A gap under a quarter of the glyph height is just letter spacing.
    std::vector<pdftext::GlyphBox> tight;
    PushWord(tight, "one", 0, 500);
    PushWord(tight, "two", 17, 500);  // 2pt gap, under 2.5
    CHECK(pdftext::JoinGlyphText(tight, AllOf(tight)) == "onetwo");
}

void TestJoinGlyphTextLineBreak() {
    std::vector<pdftext::GlyphBox> g;
    PushWord(g, "one", 0, 500);
    PushWord(g, "two", 0, 487);  // next line down
    CHECK(pdftext::JoinGlyphText(g, AllOf(g)) == "one\ntwo");
}

// A word broken over a line end is put back together -- the single
// biggest difference between a quotable passage and raw extraction.
void TestJoinGlyphTextRejoinsHyphenatedWord() {
    std::vector<pdftext::GlyphBox> g;
    PushWord(g, "dia-", 0, 500);
    PushWord(g, "gram", 0, 487);
    CHECK(pdftext::JoinGlyphText(g, AllOf(g)) == "diagram");
    // A real hyphenated compound keeps its hyphen: the next line starts
    // upper-case, so it was not a break mid-word.
    std::vector<pdftext::GlyphBox> compound;
    PushWord(compound, "Anglo-", 0, 500);
    PushWord(compound, "Saxon", 0, 487);
    CHECK(pdftext::JoinGlyphText(compound, AllOf(compound)) == "Anglo-\nSaxon");
}

void TestJoinGlyphTextExpandsLigatures() {
    CHECK(pdftext::ExpandLigatures("signi\xEF\xAC\x81" "es") == "signifies");
    CHECK(pdftext::ExpandLigatures("su\xEF\xAC\x83" "cient") == "sufficient");
    CHECK(pdftext::ExpandLigatures("plain") == "plain");
    std::vector<pdftext::GlyphBox> g;
    g.push_back(Box(0, 510, 5, 500, "\xEF\xAC\x81", ""));  // a single "fi" glyph
    g.push_back(Box(5, 510, 10, 500, "n", ""));
    CHECK(pdftext::JoinGlyphText(g, AllOf(g)) == "fin");
}

// /ActualText replaces the glyphs of its whole sequence, once -- every
// glyph in the run carries the same string.
void TestJoinGlyphTextActualTextReadOnce() {
    std::vector<pdftext::GlyphBox> g;
    g.push_back(Box(0, 510, 5, 500, "1", "one half"));
    g.push_back(Box(5, 510, 10, 500, "/", "one half"));
    g.push_back(Box(10, 510, 15, 500, "2", "one half"));
    CHECK(pdftext::JoinGlyphText(g, AllOf(g)) == "one half");
}

// Only the glyphs asked for, in the order asked for: a visual selection
// hands over row-ordered indices that need not be contiguous.
void TestJoinGlyphTextHonoursIndices() {
    std::vector<pdftext::GlyphBox> g;
    PushWord(g, "abc", 0, 500);
    CHECK(pdftext::JoinGlyphText(g, {1, 2}) == "bc");
    CHECK(pdftext::JoinGlyphText(g, {}).empty());
    // Out-of-range indices are skipped, not read past the end.
    CHECK(pdftext::JoinGlyphText(g, {-1, 0, 99}) == "a");
}

void TestEndsWithLetterHyphen() {
    CHECK(pdftext::EndsWithLetterHyphen("dia-"));
    CHECK(!pdftext::EndsWithLetterHyphen("dia"));
    CHECK(!pdftext::EndsWithLetterHyphen("-"));
    CHECK(!pdftext::EndsWithLetterHyphen(""));
    CHECK(!pdftext::EndsWithLetterHyphen("2-"));  // a number, not a broken word
}

// SelectionGlyphRange is what both the drawn quads and the copied text
// are built from, so they can never disagree about what is selected.
void TestSelectionGlyphRangeMatchesRects() {
    std::vector<pdftext::GlyphBox> g;
    PushWord(g, "abcd", 0, 500, 10);
    const auto range = pdftext::SelectionGlyphRange(g, 12, 505, 35, 505);
    CHECK(range.size() == 3);
    CHECK(range.front() == 1 && range.back() == 3);
    CHECK(pdftext::JoinGlyphText(g, range) == "bcd");
    CHECK(pdftext::SelectionGlyphRange({}, 0, 0, 1, 1).empty());
}

// An /ActualText span is honoured when it is a plausible replacement
// for its glyphs, and ignored when it is not.
//
// The case that matters: real tagged-PDF producers wrap a whole list
// item -- marker and body together -- in one marked-content sequence
// whose /ActualText is just the bullet. Taking that at its word erases
// the item, which is where "I highlighted a bulleted item and the model
// only knew it was bulleted" came from. poppler erases it too.
void TestJoinGlyphTextIgnoresImplausibleActualText() {
    // Twelve glyphs of real text, all claiming an /ActualText of one
    // bullet character. The bullet is a label, not a transcription.
    std::vector<pdftext::GlyphBox> g;
    const char *words[] = {"Se", "cond", " ", "item", " ", "body", " ", "text", " ", "in", "a", "span"};
    double x = 0;
    for (const char *w : words) {
        g.push_back(Box(x, 100, x + 10, 90, w, "\u00b7"));
        x += 10;
    }
    const std::string out = pdftext::JoinGlyphText(g, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
    // The glyphs' own text survives; the bullet does not stand in for it.
    CHECK(out.find("Second") != std::string::npos);
    CHECK(out.find("span") != std::string::npos);
    // ...and that holds for the last glyph of the span too: deciding per
    // glyph rather than per span made the tail look short and honoured
    // it, turning the final word back into the bullet.
    CHECK(out.size() > 20);

    // A short span, which is what /ActualText is actually for (a
    // ligature, a decorative glyph), is still honoured -- same contract
    // TestJoinGlyphTextActualTextReadOnce asserts.
    std::vector<pdftext::GlyphBox> lig = {
        Box(0, 100, 10, 90, "1", "one half"),
        Box(10, 100, 20, 90, "/", "one half"),
        Box(20, 100, 30, 90, "2", "one half"),
    };
    CHECK(pdftext::JoinGlyphText(lig, {0, 1, 2}) == "one half");
}

// A drag must select what was dragged over, not whatever the producer
// emitted in between.
//
// The failing case, which is the normal case on a typeset page: the
// content stream emits the body text first and the running header
// afterwards, so the header's glyphs have HIGHER indices than body text
// below them on the page. Taking the contiguous run of indices between
// two points in the body therefore swept the header in -- on one real
// book, 451 of 485 pages order their content this way.
void TestSelectionGlyphRangeIsVisualNotStreamOrder() {
    // Two body lines (emitted first), then a header ABOVE them on the
    // page (emitted last, so highest indices).
    std::vector<pdftext::GlyphBox> g;
    // Body line 1 at y[90,100]: indices 0..3
    for (int i = 0; i < 4; ++i)
        g.push_back(Box(70 + i * 10, 100, 80 + i * 10, 90, std::string(1, static_cast<char>('a' + i))));
    // Body line 2 at y[70,80]: indices 4..7
    for (int i = 0; i < 4; ++i)
        g.push_back(Box(70 + i * 10, 80, 80 + i * 10, 70, std::string(1, static_cast<char>('e' + i))));
    // The running header at y[130,140] -- above both, emitted last:
    // indices 8..11.
    for (int i = 0; i < 4; ++i)
        g.push_back(Box(70 + i * 10, 140, 80 + i * 10, 130, "H"));

    // Drag from the first body glyph to the last one. In stream order
    // that is 0..7 and happens to be right; the header is outside.
    std::vector<int> r = pdftext::SelectionGlyphRange(g, 72, 95, 112, 75);
    CHECK(r.size() == 8);
    for (int gi : r) CHECK(gi < 8);  // no header

    // Now the case that used to break: drag from the HEADER down into
    // the body. Visually that covers the header and body line 1; in
    // stream order the indices run 0..11, which is the whole page.
    r = pdftext::SelectionGlyphRange(g, 72, 135, 112, 95);
    CHECK(!r.empty());
    // Every selected glyph must be one a reader dragged over: the
    // header, or body line 1. Body line 2 sits below the drag's end.
    for (int gi : r) {
        const pdftext::GlyphBox &b = g[static_cast<size_t>(gi)];
        CHECK(b.bottom >= 90.0);  // header (130) or body line 1 (90)
    }
    // ...and it really did include both of those rows, so this is not
    // passing by selecting nothing.
    bool saw_header = false, saw_body = false;
    for (int gi : r) {
        if (g[static_cast<size_t>(gi)].bottom > 120) saw_header = true;
        if (g[static_cast<size_t>(gi)].bottom < 120) saw_body = true;
    }
    CHECK(saw_header && saw_body);
}

// --- intra-row glyph order, and joining a row as one line -----------
//
// The defect these cover: a row holding more than one baseline used to
// be sorted purely by x, interleaving the baselines, and the text
// assembled from that order broke a line wherever the vertical position
// jumped. A real thesis page's "lim" with its own limit beneath came out
// as "\u2206 / li / t / m / \u21920" -- five lines for one operator.

// "lim" on the main baseline with "dt" centred underneath it, the shape
// of \lim_{\Delta t \to 0}. Sorted by x the two baselines interleave;
// the row must instead read as the operator followed by its limit.
void TestGlyphRowsOrderLimitAfterItsOperator() {
    // Geometry copied from pdflatex's own output for \lim_{\Delta t \to 0}
    // (test/pdf_fixtures/display_math.pdf), rounded: the operator on a
    // 10.36-high baseline at y=339.42, the limit 6.68 below it at 7.57
    // high, and -- the part that matters -- the limit CENTRED under the
    // operator, so it starts to the left of "l" and ends to the right of
    // "m". Invented geometry would not reproduce either the fold that
    // puts them in one row or the x overlap that marks the limit as a
    // separate structure.
    std::vector<pdftext::GlyphBox> g = {
        Box(225.68, 349.78, 228.71, 339.42, "l"),
        Box(228.71, 349.78, 231.74, 339.42, "i"),
        Box(231.74, 349.78, 240.83, 339.42, "m"),
        Box(221.85, 340.31, 228.91, 332.74, "d"),
        Box(228.91, 340.31, 244.67, 332.74, "t"),
    };
    std::vector<pdftext::GlyphRow> rows = pdftext::PageGlyphRows(g);
    CHECK(rows.size() == 1);
    // The three main-baseline glyphs come first, in x order, then the
    // limit's own two -- not d,l,i,t,m as x order alone would give.
    CHECK(rows[0].glyphs.size() == 5);
    CHECK(rows[0].glyphs[0] == 0 && rows[0].glyphs[1] == 1 && rows[0].glyphs[2] == 2);
    CHECK(rows[0].glyphs[3] == 3 && rows[0].glyphs[4] == 4);
    // And the row reads as one line: the operator, then its limit as a
    // separate word, with no line break anywhere.
    const std::string line = pdftext::JoinGlyphRowText(g, rows[0].glyphs);
    CHECK(line.find('\n') == std::string::npos);
    CHECK(line == "lim dt");
}

// An inline subscript must NOT be split off: "x_i" is the single word
// "xi", which is how poppler reads it too. The separating rule is how
// far below the baseline the satellite sits -- a displayed limit a full
// glyph height, an inline subscript about a third of one.
void TestJoinGlyphRowTextKeepsInlineSubscriptAttached() {
    // Again pdflatex's own geometry, from the "S_t" in the same fixture:
    // the base 10.36 high on its baseline, the subscript 7.57 high and
    // only 1.10 below it, sitting immediately to the right of the base
    // rather than under it.
    std::vector<pdftext::GlyphBox> g = {
        Box(263.64, 349.78, 270.33, 339.42, "x"),
        Box(270.32, 345.89, 273.38, 338.32, "i"),
    };
    std::vector<pdftext::GlyphRow> rows = pdftext::PageGlyphRows(g);
    CHECK(rows.size() == 1);
    CHECK(pdftext::JoinGlyphRowText(g, rows[0].glyphs) == "xi");
}

// A row is one line however many baselines it holds, so joining one
// never emits a newline -- that is the whole difference from
// JoinGlyphText, which has to guess.
void TestJoinGlyphRowTextNeverBreaksALine() {
    std::vector<pdftext::GlyphBox> g = {
        Box(0, 100, 10, 90, "a"), Box(10, 84, 20, 76, "b"), Box(20, 100, 30, 90, "c"),
    };
    const std::string line = pdftext::JoinGlyphRowText(g, {0, 1, 2});
    CHECK(line.find('\n') == std::string::npos);
}

// JoinGlyphRows puts exactly one newline between rows, and rejoins a
// word broken over a row end.
void TestJoinGlyphRowsSeparatesRowsAndRejoinsHyphen() {
    std::vector<pdftext::GlyphBox> g = {
        Box(0, 100, 10, 90, "dia"), Box(10, 100, 20, 90, "-"),   // row 0: "dia-"
        Box(0, 80, 10, 70, "gram"),                               // row 1: "gram"
        Box(0, 60, 10, 50, "Next"),                               // row 2
    };
    const std::string joined = pdftext::JoinGlyphRows(g, {{0, 1}, {2}, {3}});
    CHECK(joined == "diagram\nNext");
}

// An accent drawn over a letter becomes one character with it.
//
// A font built without precomposed glyphs -- one Type 1 program holding
// `e` and `acute` and no `eacute` at all -- draws the accent as its own
// glyph positioned over the letter. Read glyph by glyph that is
// "FRE\u00b4DE\u00b4RIC": not a word any search will find, and not what a
// reader copying the name wants.
void TestJoinGlyphRowTextComposesAccents() {
    // Geometry from a real page: the acute sits inside the E's own
    // horizontal span and about 2pt higher. Note the accent is EMITTED
    // first in the content stream, so after the left-to-right sort it
    // lands just after its base.
    std::vector<pdftext::GlyphBox> g = {
        Box(161.12, 720.41, 166.64, 712.84, "F"),
        Box(166.64, 720.41, 172.88, 712.84, "R"),
        Box(172.87, 720.41, 178.63, 712.84, "E"),
        Box(173.63, 722.43, 177.86, 714.86, "\u00b4"),
        Box(178.63, 720.41, 185.09, 712.84, "D"),
    };
    std::vector<pdftext::GlyphRow> rows = pdftext::PageGlyphRows(g);
    CHECK(rows.size() == 1);
    const std::string line = pdftext::JoinGlyphRowText(g, rows[0].glyphs);
    CHECK(line == "FR\u00c9D");

    // A quote or prime that merely FOLLOWS a letter, sitting beside it
    // rather than over it, must be left alone.
    std::vector<pdftext::GlyphBox> q = {
        Box(100, 110, 108, 100, "q"),
        Box(108, 112, 112, 104, "\u00b4"),  // beyond the q's right edge
    };
    std::vector<pdftext::GlyphRow> qrows = pdftext::PageGlyphRows(q);
    CHECK(pdftext::JoinGlyphRowText(q, qrows[0].glyphs) == "q\u00b4");

    // A pair with no single-character form stays as it was.
    std::vector<pdftext::GlyphBox> z = {
        Box(100, 110, 108, 100, "q"),
        Box(101, 112, 107, 104, "\u00b4"),  // over it, but there is no q-acute
    };
    std::vector<pdftext::GlyphRow> zrows = pdftext::PageGlyphRows(z);
    CHECK(pdftext::JoinGlyphRowText(z, zrows[0].glyphs) == "q\u00b4");
}

// A nested list keeps its nesting when copied out.
//
// Which item sits under which is the structure of a list, and it is
// carried only by the indentation. A paragraph's own first-line indent
// is NOT structure, so it must not be reproduced -- otherwise ordinary
// prose comes out with stray spaces down its left edge.
void TestJoinGlyphRowsKeepsListIndentation() {
    std::vector<pdftext::GlyphBox> g;
    // Character-sized boxes, not one wide box holding a whole phrase:
    // the word-gap unit is the larger of the glyphs' height and width,
    // so a single 50pt-wide "glyph" would set the threshold far above
    // any real gap and nothing would ever be spaced.
    auto row_at = [&](double left, double bottom, const std::string &first, const std::string &rest) {
        const int start = static_cast<int>(g.size());
        g.push_back(Box(left, bottom + 10, left + 8, bottom, first));
        double x = left + 16;  // a real gap after the marker
        for (char ch : rest) {
            g.push_back(Box(x, bottom + 10, x + 5, bottom, std::string(1, ch)));
            x += 5;
        }
        std::vector<int> row;
        for (int i = start; i < static_cast<int>(g.size()); ++i) row.push_back(i);
        return row;
    };
    // Two outer items at x=70, one nested at x=90 (just over one step
    // for 10pt text), and an indented paragraph line that is not a list.
    std::vector<std::vector<int>> rows;
    rows.push_back(row_at(70, 200, "\u2022", "outer one"));
    rows.push_back(row_at(90, 185, "\u2013", "nested"));
    rows.push_back(row_at(70, 170, "\u2022", "outer two"));
    rows.push_back(row_at(90, 155, "Indented", "prose line"));

    const std::string out = pdftext::JoinGlyphRows(g, rows);
    CHECK(out.find("\u2022 outer one\n") != std::string::npos);
    // The nested item is indented...
    CHECK(out.find("\n  \u2013 nested\n") != std::string::npos);
    // ...the outer ones are not...
    CHECK(out.find("\n\u2022 outer two\n") != std::string::npos);
    // ...and the indented prose line is left alone, marker-less.
    CHECK(out.find("\nIndented prose line") != std::string::npos);
}

// A font that reports a nonsense glyph height must not have every
// letter spaced apart.
//
// The word-gap test needs the em, and the glyph box's height normally
// is it. A Type 3 font defines its glyph space with /FontMatrix rather
// than the usual 1/1000 em, which the vertical estimate does not
// account for: one real book's glyphs measured 5.5pt wide and 0.114pt
// tall, putting the threshold at 0.023pt so the ordinary 0.24pt gap
// between two letters read as a word break -- "number" came out as
// "numb er", "code" as "co de".
void TestJoinGlyphRowTextSurvivesDegenerateHeights() {
    // Geometry straight off that page: real widths, collapsed heights,
    // and the inter-letter gaps it actually had.
    std::vector<pdftext::GlyphBox> g = {
        Box(111.120, 0.114, 116.640, 0.0, "n"),
        Box(116.400, 0.114, 121.920, 0.0, "u"),
        Box(121.920, 0.114, 130.200, 0.0, "m"),
        Box(129.960, 0.114, 135.480, 0.0, "b"),
        Box(135.720, 0.114, 140.160, 0.0, "e"),   // the 0.24pt gap that split it
        Box(140.160, 0.114, 144.120, 0.0, "r"),
    };
    std::vector<int> all;
    for (int i = 0; i < static_cast<int>(g.size()); ++i) all.push_back(i);
    CHECK(pdftext::JoinGlyphRowText(g, all) == "number");

    // A genuine word space in the same font -- several points, far wider
    // than any letter gap -- must still register.
    std::vector<pdftext::GlyphBox> two = {
        Box(100.0, 0.114, 105.5, 0.0, "a"),
        Box(112.0, 0.114, 117.5, 0.0, "b"),  // 6.5pt apart
    };
    CHECK(pdftext::JoinGlyphRowText(two, {0, 1}) == "a b");
}

// --- rotated pages ---------------------------------------------------
//
// Glyph boxes stay in the page's unrotated point space, because
// annotation quads round-trip through it. On a /Rotate 90 page that
// means text advances along the axis the row grouper would otherwise
// take for the line-advance axis, and a landscape page came back one
// character per line. PageGlyphRows is told the rotation instead.
void TestGlyphRowsHandleRotatedPage() {
    // A /Rotate 90 page: in unrotated point space the line advances
    // along +y at a constant x, and successive lines step along +x.
    // Two lines of three glyphs each.
    std::vector<pdftext::GlyphBox> g = {
        // line 1 at x[100,114], advancing y 200->230
        Box(100, 210, 114, 200, "A"), Box(100, 220, 114, 210, "B"), Box(100, 230, 114, 220, "C"),
        // line 2 at x[120,134], same y range
        Box(120, 210, 134, 200, "D"), Box(120, 220, 134, 210, "E"), Box(120, 230, 134, 220, "F"),
    };
    // Told the page is upright, the grouper buckets by `bottom` -- the
    // axis text advances along here -- and gets this wrong.
    std::vector<pdftext::GlyphRow> upright = pdftext::PageGlyphRows(g, 0);
    CHECK(upright.size() == 3);  // one "row" per advance step: the bug

    // Told the truth, it finds the two real lines, each reading A,B,C.
    std::vector<pdftext::GlyphRow> rows = pdftext::PageGlyphRows(g, 90);
    CHECK(rows.size() == 2);
    CHECK(rows[0].glyphs.size() == 3 && rows[1].glyphs.size() == 3);
    CHECK(pdftext::JoinGlyphRowText(g, rows[0].glyphs, 90) == "ABC");
    CHECK(pdftext::JoinGlyphRowText(g, rows[1].glyphs, 90) == "DEF");
    // Row extents come back in the caller's own (unrotated) space.
    CHECK(rows[0].top == 230 && rows[0].bottom == 200);
}

int main(int argc, char **argv) {
    TestSearchFindsWordCaseInsensitively();
    TestSearchMultiWordQuerySpansWordGap();
    TestSearchNoMatchReturnsEmpty();
    TestSearchLineWrapProducesTwoRects();
    TestSearchAcrossMultiplePages();
    TestSearchIndexContext();
    TestMatchRectsForPageScalesWithPxPerPt();
    TestExtractPageTextContainsShownWords();
    TestSelectionRects();
    TestGlyphRowsAreVisualOrderNotReadingOrder();
    TestGlyphRowsKeepAdjacentTextLinesApart();
    TestGlyphRowsFoldSubscriptIntoItsLine();
    TestGlyphRowsFlagDisplayEquation();
    TestGlyphRowsKeepProseWithInlineMath();
    TestGlyphRowsEmptyInput();
    TestJoinGlyphTextWordsAndGaps();
    TestJoinGlyphTextLineBreak();
    TestJoinGlyphTextRejoinsHyphenatedWord();
    TestJoinGlyphTextExpandsLigatures();
    TestJoinGlyphTextActualTextReadOnce();
    TestJoinGlyphTextHonoursIndices();
    TestEndsWithLetterHyphen();
    TestSelectionGlyphRangeMatchesRects();
    TestGlyphRowsOrderLimitAfterItsOperator();
    TestJoinGlyphRowTextKeepsInlineSubscriptAttached();
    TestJoinGlyphRowTextNeverBreaksALine();
    TestJoinGlyphRowsSeparatesRowsAndRejoinsHyphen();
    TestGlyphRowsHandleRotatedPage();
    TestJoinGlyphRowTextComposesAccents();
    TestJoinGlyphRowsKeepsListIndentation();
    TestJoinGlyphRowTextSurvivesDegenerateHeights();
    TestJoinGlyphTextIgnoresImplausibleActualText();
    TestSelectionGlyphRangeIsVisualNotStreamOrder();
    std::printf("pdf_text_test: all checks passed\n");

    for (int i = 1; i < argc; ++i) CheckRealFixture(argv[i]);
    return 0;
}
