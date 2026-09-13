// Coverage for pdf_outline.h/.cpp's PDF /Outlines (bookmarks) tree
// reader: nesting via depth, /Dest and /A GoTo destination resolution
// (direct-array forms and String-keyed named destinations resolved
// through /Root/Names/Dests's name tree, spec 7.9.6), UTF-16BE title
// decoding (with a surrogate-pair codepoint), tolerance of a legacy
// Name-object destination and of a name-tree lookup miss (page stays
// -1, title still listed), a missing /Outlines dict entirely, and cycle
// protection against a malformed /Next chain. Builds small
// synthetic documents by hand (same "compute offsets from actual string
// positions" approach as pdf_document_test.cpp/pdf_xref_test.cpp)
// rather than reading test/pdf_fixtures/ (gitignored, and none of the 3
// real fixtures happen to have a real /Outlines dict at all, since none
// of tectonic/LibreOffice's default output enables bookmarks/hyperref).

#include "pdf_outline.h"

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

// A 2-page document whose outline tree is:
//   Chapter 1 (page 0)
//     Section 1.1 (page 0, via /A GoTo instead of a direct /Dest)
//   Chapter 2 (page 1)
std::string BuildOutlineDoc() {
    return BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R /Outlines 6 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R 4 0 R] /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},  // page index 0
            {4, "<< /Type /Page /Parent 2 0 R >>"},  // page index 1
            {5, "<< /Title (Section 1.1) /Parent 7 0 R /A << /S /GoTo /D [3 0 R /Fit] >> >>"},
            {6, "<< /Type /Outlines /First 7 0 R /Last 8 0 R /Count 2 >>"},
            {7, "<< /Title (Chapter 1) /Parent 6 0 R /Next 8 0 R /First 5 0 R /Last 5 0 R /Count 1 "
                "/Dest [3 0 R /Fit] >>"},
            {8, "<< /Title (Chapter 2) /Parent 6 0 R /Prev 7 0 R /Dest [4 0 R /Fit] >>"},
        },
        1);
}

void TestBasicNestedOutline() {
    std::string doc = BuildOutlineDoc();
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    CHECK(document.PageCount() == 2);

    auto items = pdfoutline::GetOutline(B(doc), doc.size(), document.Xref(), document);
    CHECK(items.size() == 3);
    CHECK(items[0].title == "Chapter 1" && items[0].depth == 0 && items[0].page == 0);
    CHECK(items[1].title == "Section 1.1" && items[1].depth == 1 && items[1].page == 0);  // via /A GoTo
    CHECK(items[2].title == "Chapter 2" && items[2].depth == 0 && items[2].page == 1);
}

void TestNoOutlinesDict() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},  // no /Outlines entry at all
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    auto items = pdfoutline::GetOutline(B(doc), doc.size(), document.Xref(), document);
    CHECK(items.empty());
}

// "café" as UTF-16BE (00 63 00 61 00 66 00 E9), BOM-prefixed (FE FF), as
// a PDF hex string -- also exercises a supplementary-plane codepoint
// (U+1F600, surrogate pair D83D DE00) appended after it, to confirm
// surrogate-pair decoding produces the correct 4-byte UTF-8 sequence.
void TestUtf16BeTitleWithSurrogatePair() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},
            {4, "<< /Type /Outlines /First 5 0 R /Last 5 0 R /Count 1 >>"},
            {5, "<< /Title <FEFF00630061006600E9D83DDE00> /Parent 4 0 R /Dest [3 0 R /Fit] >>"},
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    auto items = pdfoutline::GetOutline(B(doc), doc.size(), document.Xref(), document);
    CHECK(items.size() == 1);
    CHECK(items[0].title == "caf\xC3\xA9\xF0\x9F\x98\x80");  // "café\xF0\x9F\x98\x80" (U+00E9, U+1F600) in UTF-8
    CHECK(items[0].page == 0);
}

// A legacy Name-object destination (pre-PDF-1.2's own named-destination
// mechanism -- a Catalog-level /Dests dictionary keyed by Name, not the
// /Root/Names/Dests *name tree* keyed by String that ResolveNamedDestination
// actually walks) still lists the bookmark's title; its page can't be
// resolved since there's no such legacy /Dests dict here at all, so it
// stays -1 rather than crashing or being silently dropped. Real-world
// bookmark-generating tools (hyperref included) use the String-keyed
// name tree instead -- see TestNamedDestinationViaNameTree below.
void TestNamedDestinationUnresolved() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},
            {4, "<< /Type /Outlines /First 5 0 R /Last 5 0 R /Count 1 >>"},
            {5, "<< /Title (Somewhere) /Parent 4 0 R /Dest /SomeNamedDest >>"},
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    auto items = pdfoutline::GetOutline(B(doc), doc.size(), document.Xref(), document);
    CHECK(items.size() == 1);
    CHECK(items[0].title == "Somewhere");
    CHECK(items[0].page == -1);
}

// A String-keyed named destination resolved through /Root/Names/Dests's
// name tree (spec 7.9.6) -- exactly what hyperref/xdvipdfmx emits for
// every LaTeX \section/\chapter bookmark by default, making this the
// common case for a tectonic/pdflatex-produced PDF, not the rare one.
// Exercises both an intermediate /Kids-splitting node (rather than the
// Dests root being a leaf itself) and a lookup miss (a name absent from
// the tree, which must still resolve to -1 rather than erroring).
void TestNamedDestinationViaNameTree() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R /Outlines 6 0 R /Names 9 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R 4 0 R] /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},  // page index 0
            {4, "<< /Type /Page /Parent 2 0 R >>"},  // page index 1
            {6, "<< /Type /Outlines /First 7 0 R /Last 8 0 R /Count 2 >>"},
            {7, "<< /Title (Chapter 2) /Parent 6 0 R /Next 8 0 R /Dest (chapter.2) >>"},
            {8, "<< /Title (Nowhere) /Parent 6 0 R /Prev 7 0 R /A << /S /GoTo /D (does.not.exist) >> >>"},
            {9, "<< /Dests 10 0 R >>"},
            {10, "<< /Kids [11 0 R] >>"},
            {11, "<< /Limits [(chapter.2)(chapter.2)] /Names [(chapter.2) [4 0 R /Fit]] >>"},
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    auto items = pdfoutline::GetOutline(B(doc), doc.size(), document.Xref(), document);
    CHECK(items.size() == 2);
    CHECK(items[0].title == "Chapter 2" && items[0].page == 1);  // resolved via the name tree to page object 4
    CHECK(items[1].title == "Nowhere" && items[1].page == -1);   // name tree lookup miss: tolerated, not an error
}

// A malformed circular /Next chain (5 -> 6 -> 5 -> ...) must not hang --
// same cycle-protection convention as pdf_document.cpp's own
// TestCyclicKidsDoesNotHang.
void TestCyclicNextChainDoesNotHang() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},
            {4, "<< /Type /Outlines /First 5 0 R /Last 6 0 R /Count 2 >>"},
            {5, "<< /Title (A) /Parent 4 0 R /Next 6 0 R >>"},
            {6, "<< /Title (B) /Parent 4 0 R /Next 5 0 R >>"},  // cycles back to 5
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    auto items = pdfoutline::GetOutline(B(doc), doc.size(), document.Xref(), document);  // must return, not hang
    CHECK(items.size() == 2);
    CHECK(items[0].title == "A");
    CHECK(items[1].title == "B");
}

// A missing/malformed /Root or /Outlines target (points at something
// that isn't a dict) is tolerated the same "return empty" way as an
// absent /Outlines entry -- no crash, no bogus entries.
void TestMalformedOutlinesTarget() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},
            {4, "42"},  // malformed: /Outlines points at a bare integer, not a dict
        },
        1);
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    auto items = pdfoutline::GetOutline(B(doc), doc.size(), document.Xref(), document);
    CHECK(items.empty());
}

}  // namespace

int main() {
    TestBasicNestedOutline();
    TestNoOutlinesDict();
    TestUtf16BeTitleWithSurrogatePair();
    TestNamedDestinationUnresolved();
    TestNamedDestinationViaNameTree();
    TestCyclicNextChainDoesNotHang();
    TestMalformedOutlinesTarget();
    std::printf("pdf_outline_test: all checks passed\n");
    return 0;
}
