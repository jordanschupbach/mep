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

int main(int argc, char **argv) {
    TestSearchFindsWordCaseInsensitively();
    TestSearchMultiWordQuerySpansWordGap();
    TestSearchNoMatchReturnsEmpty();
    TestSearchLineWrapProducesTwoRects();
    TestSearchAcrossMultiplePages();
    TestMatchRectsForPageScalesWithPxPerPt();
    TestExtractPageTextContainsShownWords();
    std::printf("pdf_text_test: all checks passed\n");

    for (int i = 1; i < argc; ++i) CheckRealFixture(argv[i]);
    return 0;
}
