// Coverage for pdf_links.h/.cpp's PDF Link annotation (spec 12.5.6.5)
// reader: a /Dest-carrying link resolving to a device-pixel rect and
// target page, an /A GoTo action doing the same, a /A URI action
// recovering its URI instead of a page, an annotation with neither
// shape resolving (skipped, not a crash), a non-Link annotation being
// ignored, and a page with no /Annots array at all. Builds small
// synthetic documents by hand, same "compute offsets from actual string
// positions" approach as pdf_outline_test.cpp (whose BuildDoc/XrefLine
// helpers this duplicates rather than shares a header over, for the
// same "small enough, not worth a shared test-utility module" reasoning
// as pdf_outline.cpp's own AppendUtf8 comment).

#include "pdf_links.h"

#include "pdf_document.h"

#include <algorithm>
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

// A 2-page, 612x792pt document whose page 0 carries 4 Link annotations:
// a direct /Dest to page 1, an /A GoTo (also to page 1), an /A URI, and
// one with neither /Dest nor /A (unresolvable, must be skipped) -- plus
// one non-Link annotation (a /Subtype /Text "sticky note") that must be
// ignored entirely regardless of its own /Rect.
std::string BuildLinksDoc() {
    return BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R 4 0 R] /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Annots [5 0 R 6 0 R 7 0 R 8 0 R 9 0 R] >>"},  // page index 0
            {4, "<< /Type /Page /Parent 2 0 R >>"},                                          // page index 1
            {5, "<< /Type /Annot /Subtype /Link /Rect [100 200 150 220] /Dest [4 0 R /Fit] >>"},
            {6, "<< /Type /Annot /Subtype /Link /Rect [100 300 150 320] /A << /S /GoTo /D [4 0 R /Fit] >> >>"},
            {7, "<< /Type /Annot /Subtype /Link /Rect [100 400 150 420] "
                "/A << /S /URI /URI (https://example.com/) >> >>"},
            {8, "<< /Type /Annot /Subtype /Link /Rect [100 500 150 520] >>"},  // no /Dest or /A: unresolvable
            {9, "<< /Type /Annot /Subtype /Text /Rect [0 0 612 792] /Contents (a sticky note, not a link) >>"},
        },
        1);
}

void TestDestAndGoToAndUriLinks() {
    std::string doc = BuildLinksDoc();
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    CHECK(document.PageCount() == 2);

    auto links = pdflinks::GetPageLinks(B(doc), doc.size(), document.Xref(), document, 0, 1.0f);
    // The unresolvable (8) and non-Link (9) annotations are excluded --
    // only the 3 that actually resolve to something jumpable remain.
    CHECK(links.size() == 3);

    const pdflinks::PdfLinkAnnot &dest_link = links[0];
    CHECK(dest_link.target_page == 1);
    CHECK(dest_link.uri.empty());
    // Device-pixel rect at px_per_pt=1.0: PDF's bottom-up [100,200]-[150,220]
    // on a 792pt-tall page flips to top-down y = 792-220=572 .. 792-200=592.
    CHECK(dest_link.x0 == 100.0f && dest_link.x1 == 150.0f);
    CHECK(dest_link.y0 == 572.0f && dest_link.y1 == 592.0f);

    const pdflinks::PdfLinkAnnot &goto_link = links[1];
    CHECK(goto_link.target_page == 1);
    CHECK(goto_link.uri.empty());

    const pdflinks::PdfLinkAnnot &uri_link = links[2];
    CHECK(uri_link.target_page == -1);
    CHECK(uri_link.uri == "https://example.com/");
}

void TestNoAnnotsArray() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},  // no /Annots entry at all
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    auto links = pdflinks::GetPageLinks(B(doc), doc.size(), document.Xref(), document, 0, 1.0f);
    CHECK(links.empty());
}

void TestOutOfRangePage() {
    std::string doc = BuildLinksDoc();
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    auto links = pdflinks::GetPageLinks(B(doc), doc.size(), document.Xref(), document, 99, 1.0f);
    CHECK(links.empty());
}

}  // namespace

int main() {
    TestDestAndGoToAndUriLinks();
    TestNoAnnotsArray();
    TestOutOfRangePage();
    std::printf("pdf_links_test: all checks passed\n");
    return 0;
}
