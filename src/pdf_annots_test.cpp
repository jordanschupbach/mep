// Coverage for pdf_annots.h/.cpp's markup-annotation reader: a /Highlight
// with QuadPoints/colour/contents and a /Text sticky note, read back into
// the point-space model (geometry NOT device-transformed, unlike the Link
// reader), a non-markup annotation ignored, and a page with no /Annots.
// Same "compute offsets from actual string positions" BuildDoc helper as
// pdf_links_test.cpp / pdf_outline_test.cpp.

#include "pdf_annots.h"

#include "pdf_document.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }

// Page 0 carries a /Highlight (2 quads, yellow, a comment + author) and a
// /Text sticky note (red, /Name /Comment, /Open true), plus one /Link that
// the markup reader must ignore.
std::string BuildAnnotsDoc() {
    return BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Annots [4 0 R 5 0 R 6 0 R] >>"},
            {4, "<< /Type /Annot /Subtype /Highlight /Rect [100 700 260 740] "
                "/QuadPoints [100 740 200 740 100 720 200 720  100 720 260 720 100 700 260 700] "
                "/C [1 1 0] /CA 0.4 /Contents (great point) /T (reader) >>"},
            {5, "<< /Type /Annot /Subtype /Text /Rect [300 700 318 718] /Contents (a marginal note) "
                "/Name /Comment /Open true /C [1 0 0] >>"},
            {6, "<< /Type /Annot /Subtype /Link /Rect [0 0 10 10] /A << /S /URI /URI (x) >> >>"},
        },
        1);
}

void TestReadHighlightAndText() {
    std::string doc = BuildAnnotsDoc();
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    CHECK(document.PageCount() == 1);

    auto annots = pdfannots::GetPageAnnots(B(doc), doc.size(), document.Xref(), document, 0);
    CHECK(annots.size() == 2);  // Link (obj 6) ignored

    const pdfannots::PdfAnnot &h = annots[0];
    CHECK(h.kind == pdfannots::Kind::Highlight);
    CHECK(h.from_file && h.src_obj == 4);
    CHECK(h.quads.size() == 2);
    // First quad, spec order UL,UR,LL,LR -- preserved verbatim, point space.
    CHECK(Near(h.quads[0].x1, 100) && Near(h.quads[0].y1, 740));
    CHECK(Near(h.quads[0].x2, 200) && Near(h.quads[0].y2, 740));
    CHECK(Near(h.quads[0].x3, 100) && Near(h.quads[0].y3, 720));
    CHECK(Near(h.quads[0].x4, 200) && Near(h.quads[0].y4, 720));
    CHECK(Near(h.rect[0], 100) && Near(h.rect[1], 700) && Near(h.rect[2], 260) && Near(h.rect[3], 740));
    CHECK(Near(h.color[0], 1) && Near(h.color[1], 1) && Near(h.color[2], 0));
    CHECK(Near(h.opacity, 0.4));
    CHECK(h.contents == "great point");
    CHECK(h.author == "reader");

    const pdfannots::PdfAnnot &t = annots[1];
    CHECK(t.kind == pdfannots::Kind::Text);
    CHECK(t.from_file && t.src_obj == 5);
    CHECK(t.contents == "a marginal note");
    CHECK(t.icon == "Comment");
    CHECK(t.open == true);
    CHECK(Near(t.color[0], 1) && Near(t.color[1], 0) && Near(t.color[2], 0));
    // Point space, NOT device-flipped (contrast pdf_links_test).
    CHECK(Near(t.rect[0], 300) && Near(t.rect[1], 700) && Near(t.rect[2], 318) && Near(t.rect[3], 718));
}

void TestNoAnnots() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    auto annots = pdfannots::GetPageAnnots(B(doc), doc.size(), document.Xref(), document, 0);
    CHECK(annots.empty());
}

}  // namespace

int main() {
    TestReadHighlightAndText();
    TestNoAnnots();
    std::printf("pdf_annots_test: all checks passed\n");
    return 0;
}
