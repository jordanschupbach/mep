// PDFIUM_REMOVAL_PLAN.md Phase 5 coverage for pdf_document.h/.cpp's
// page-tree flattening: inheritance of /Resources/MediaBox/Rotate down
// a /Pages tree, /Rotate-based width/height swapping, cycle protection,
// and tolerance of a malformed/missing /Root or /Pages. Builds small
// synthetic documents by hand (same "compute offsets from actual string
// positions" approach as pdf_xref_test.cpp) rather than reading
// test/pdf_fixtures/ at runtime (gitignored, per Phase 1).
//
// Also accepts optional fixture paths (argv) for a real-file spot check
// -- same opt-in convention as pdf_xref_test.cpp -- to confirm
// PageCount/PageWidthPt/PageHeightPt match `pdfinfo`'s independently
// reported values for Phase 1's 3 real fixtures.

#include "pdf_document.h"

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

// Builds a minimal classic-xref PDF with a hand-specified body (the
// object definitions between the header and the xref table) and
// computes the xref table itself from the actual byte offsets of every
// "N 0 obj" occurrence found in that body -- avoids hardcoding offsets
// while staying a plain, easy-to-read classic table (matching
// pdf_xref_test.cpp's own approach).
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

void TestBasicPageCountAndSize() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},                                 // inherits MediaBox
            {4, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] >>"},          // overrides it
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    CHECK(document.PageCount() == 2);
    CHECK(document.PageWidthPt(0) == 612 && document.PageHeightPt(0) == 792);
    CHECK(document.PageWidthPt(1) == 200 && document.PageHeightPt(1) == 400);
    CHECK(document.PageWidthPt(2) == 0 && document.PageHeightPt(2) == 0);  // out of range: tolerant zero
}

void TestNestedInheritanceAndRotate() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R 6 0 R] /MediaBox [0 0 100 200] >>"},
            {3, "<< /Type /Pages /Kids [4 0 R 5 0 R] /Rotate 90 >>"},  // sub-tree: inherits MediaBox, adds Rotate
            {4, "<< /Type /Page /Parent 3 0 R >>"},                   // inherits both MediaBox (100x200) and Rotate 90
            {5, "<< /Type /Page /Parent 3 0 R /Rotate 0 >>"},         // overrides Rotate back to 0
            {6, "<< /Type /Page /Parent 2 0 R >>"},                   // sibling subtree: no Rotate at all
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    CHECK(document.PageCount() == 3);
    // Page 4 (object 4): Rotate 90 -> width/height swapped relative to the 100x200 MediaBox.
    CHECK(document.PageWidthPt(0) == 200 && document.PageHeightPt(0) == 100);
    // Page 5 (object 5): Rotate overridden back to 0 -> not swapped.
    CHECK(document.PageWidthPt(1) == 100 && document.PageHeightPt(1) == 200);
    // Page 6 (object 6): never inherited Rotate 90 (different branch) -> not swapped.
    CHECK(document.PageWidthPt(2) == 100 && document.PageHeightPt(2) == 200);
}

void TestCropBoxClipsMediaBox() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /MediaBox [0 0 600 800] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},                                 // no CropBox: effective = MediaBox
            {4, "<< /Type /Page /Parent 2 0 R /CropBox [50 50 250 350] >>"},          // smaller than MediaBox
            {5, "<< /Type /Page /Parent 2 0 R /CropBox [-100 -100 900 900] >>"},      // exceeds MediaBox: clamped
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    CHECK(document.PageCount() == 3);
    CHECK(document.PageWidthPt(0) == 600 && document.PageHeightPt(0) == 800);  // no CropBox at all
    CHECK(document.PageWidthPt(1) == 200 && document.PageHeightPt(1) == 300);  // 250-50, 350-50
    CHECK(document.PageWidthPt(2) == 600 && document.PageHeightPt(2) == 800);  // clamped back to MediaBox
}

void TestCropBoxInheritedThenOverridden() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 600 800] /CropBox [0 0 300 400] >>"},
            {3, "<< /Type /Pages /Kids [4 0 R 5 0 R] >>"},  // no own CropBox: inherits [0 0 300 400]
            {4, "<< /Type /Page /Parent 3 0 R >>"},         // inherits inherited CropBox
            {5, "<< /Type /Page /Parent 3 0 R /CropBox [0 0 100 150] >>"},  // overrides it
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    CHECK(document.PageCount() == 2);
    CHECK(document.PageWidthPt(0) == 300 && document.PageHeightPt(0) == 400);
    CHECK(document.PageWidthPt(1) == 100 && document.PageHeightPt(1) == 150);
}

void TestResourcesInheritance() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R 4 0 R] /Resources << /Font << /F1 5 0 R >> >> >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},                                    // inherits Resources
            {4, "<< /Type /Page /Parent 2 0 R /Resources << /Font << /F2 6 0 R >> >> >>"},  // own Resources
            {5, "<< /Type /Font /BaseFont /Helvetica >>"},
            {6, "<< /Type /Font /BaseFont /Times-Roman >>"},
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    CHECK(document.PageCount() == 2);
    const pdfdoc::Page *page0 = document.GetPage(0);
    const pdfdoc::Page *page1 = document.GetPage(1);
    CHECK(page0->resources.IsDict());
    const pdfobj::Object *fonts0 = page0->resources.Find("Font");
    CHECK(fonts0 && fonts0->Find("F1") != nullptr);
    const pdfobj::Object *fonts1 = page1->resources.Find("Font");
    CHECK(fonts1 && fonts1->Find("F2") != nullptr && fonts1->Find("F1") == nullptr);
}

void TestResourcesAsIndirectReference() {
    // Real-world PDFs overwhelmingly put /Resources behind its own
    // indirect reference rather than embedding it inline (confirmed via
    // a real fixture during Phase 8 verification: xdvipdfmx's own
    // jpeg_test.pdf does exactly this) -- catches a real bug found
    // there: FlattenPageTree originally stored the raw (unresolved)
    // Reference object in Page::resources instead of dereferencing it,
    // so `resources.IsDict()` silently came back false for every real
    // PDF shaped this way, breaking every Resources-dependent operator
    // (color space lookups, XObject Do) without any error.
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Resources 4 0 R >>"},
            {4, "<< /Font << /F1 5 0 R >> >>"},
            {5, "<< /Type /Font /BaseFont /Helvetica >>"},
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    const pdfdoc::Page *page = document.GetPage(0);
    CHECK(page->resources.IsDict());
    const pdfobj::Object *fonts = page->resources.Find("Font");
    CHECK(fonts && fonts->Find("F1") != nullptr);
}

void TestRotateNormalization() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /MediaBox [0 0 100 200] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Rotate 450 >>"},   // 450 mod 360 = 90
            {4, "<< /Type /Page /Parent 2 0 R /Rotate -90 >>"},   // -90 -> 270
            {5, "<< /Type /Page /Parent 2 0 R /Rotate 180 >>"},
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    CHECK(document.PageWidthPt(0) == 200 && document.PageHeightPt(0) == 100);  // 450 -> 90: swapped
    CHECK(document.PageWidthPt(1) == 200 && document.PageHeightPt(1) == 100);  // -90 -> 270: swapped
    CHECK(document.PageWidthPt(2) == 100 && document.PageHeightPt(2) == 200);  // 180: not swapped
}

void TestCyclicKidsDoesNotHang() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] >>"},
            {3, "<< /Type /Pages /Kids [2 0 R] >>"},  // cycle: 3's child is 2, an ancestor
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());  // must return, not hang
    CHECK(document.PageCount() == 0);   // the cycle never reaches a real leaf /Page
}

void TestMissingOrMalformedRoot() {
    {
        // No /Root at all in the trailer.
        std::string doc = "%PDF-1.4\n1 0 obj\n<< /Foo /Bar >>\nendobj\n";
        doc += "xref\n0 2\n" + XrefLine(0, 65535, 'f') + XrefLine(9, 0, 'n');
        doc += "trailer\n<< /Size 2 >>\nstartxref\n" + std::to_string(doc.find("xref\n0 2")) + "\n%%EOF\n";
        pdfdoc::PdfDocument document;
        document.Load(B(doc), doc.size());
        CHECK(document.PageCount() == 0);
    }
    {
        // /Root present but the Catalog has no /Pages entry.
        std::string doc = BuildDoc({{1, "<< /Type /Catalog >>"}}, 1);
        pdfdoc::PdfDocument document;
        document.Load(B(doc), doc.size());
        CHECK(document.PageCount() == 0);
    }
    {
        // A /Kids entry that resolves to something that isn't a dict at
        // all (e.g. a stray integer) -- skipped, not a crash.
        std::string doc = BuildDoc(
            {
                {1, "<< /Type /Catalog /Pages 2 0 R >>"},
                {2, "<< /Type /Pages /Kids [3 0 R 4 0 R] >>"},
                {3, "42"},  // malformed: not a dict
                {4, "<< /Type /Page /Parent 2 0 R >>"},
            },
            1);
        pdfdoc::PdfDocument document;
        document.Load(B(doc), doc.size());
        CHECK(document.PageCount() == 1);
    }
}

// Opt-in real-fixture spot check (pass e.g.
// test/pdf_fixtures/tectonic_test.pdf as argv) -- confirms PageCount/
// PageWidthPt/PageHeightPt match `pdfinfo`'s independently reported
// values for a real xdvipdfmx/LibreOffice-produced file.
void CheckRealFixture(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string data = ss.str();

    pdfdoc::PdfDocument document;
    document.Load(B(data), data.size());
    std::printf("  %s: %d pages, page 0 = %.3f x %.3f pt\n", path.c_str(), document.PageCount(),
                document.PageWidthPt(0), document.PageHeightPt(0));
}

}  // namespace

int main(int argc, char **argv) {
    TestBasicPageCountAndSize();
    TestNestedInheritanceAndRotate();
    TestCropBoxClipsMediaBox();
    TestCropBoxInheritedThenOverridden();
    TestResourcesInheritance();
    TestResourcesAsIndirectReference();
    TestRotateNormalization();
    TestCyclicKidsDoesNotHang();
    TestMissingOrMalformedRoot();
    std::printf("pdf_document_test: all checks passed\n");

    for (int i = 1; i < argc; ++i) CheckRealFixture(argv[i]);
    return 0;
}
