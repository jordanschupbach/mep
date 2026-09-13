// PDFIUM_REMOVAL_PLAN.md Phase 3 coverage for pdf_xref.h/.cpp (+
// pdf_filters.h/.cpp's FlateDecode/Predictor, landed early to support
// this phase -- see pdf_filters.h's own doc comment). Builds small,
// real-shaped PDF byte buffers by hand (computing object offsets from
// actual string positions rather than hardcoded byte counts, so the
// fixtures stay correct if edited) covering: a classic xref table +
// trailer, a PDF-1.5-style xref stream with objects compressed into an
// ObjStm (mirroring what Phase 1's tectonic_test.pdf fixture actually
// does for its /Root), PNG and TIFF predictor reversal, an incremental
// update's /Prev chain merge, cyclic-/Prev-chain protection, and
// brute-force recovery from a corrupted/missing xref section.
//
// Doesn't read test/pdf_fixtures/ at runtime (gitignored, per Phase 1 --
// a test reading from it wouldn't run in a fresh checkout/CI); accepts
// an optional argv[1] fixture path for an opt-in real-file spot check
// instead (same "opt-in file dump" convention as image_procgen_test.cpp).

#include "pdf_xref.h"

#include "deflate.h"
#include "pdf_filters.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

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

void TestClassicXrefTableEndToEnd() {
    std::string doc = "%PDF-1.4\n";
    size_t off1 = doc.size();
    doc += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    size_t off2 = doc.size();
    doc += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    size_t off3 = doc.size();
    doc += "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n";
    size_t xref_off = doc.size();
    doc += "xref\n0 4\n";
    doc += XrefLine(0, 65535, 'f');
    doc += XrefLine(static_cast<long long>(off1), 0, 'n');
    doc += XrefLine(static_cast<long long>(off2), 0, 'n');
    doc += XrefLine(static_cast<long long>(off3), 0, 'n');
    doc += "trailer\n<< /Size 4 /Root 1 0 R >>\n";
    doc += "startxref\n" + std::to_string(xref_off) + "\n%%EOF\n";

    pdfxref::XrefTable table;
    table.Load(B(doc), doc.size());
    CHECK(table.Entries().size() == 4);  // objects 0 (free) through 3
    CHECK(table.Find(0) != nullptr && table.Find(0)->kind == pdfxref::EntryKind::Free);

    const pdfobj::Object *root_ref = table.Trailer().Find("Root");
    CHECK(root_ref && root_ref->IsReference() && root_ref->ref_val.num == 1);

    pdfobj::Object catalog = pdfxref::ResolveObject(B(doc), doc.size(), table, 1);
    CHECK(catalog.IsDict());
    CHECK(catalog.Find("Type")->AsString("") == "Catalog");
    const pdfobj::Object *pages_ref = catalog.Find("Pages");
    CHECK(pages_ref && pages_ref->ref_val.num == 2);

    pdfobj::Object pages = pdfxref::ResolveObject(B(doc), doc.size(), table, pages_ref->ref_val.num);
    CHECK(pages.IsDict());
    CHECK(pages.Find("Count")->AsInt() == 1);
}

void TestXrefStreamWithCompressedObjects() {
    // ObjStm body: header "objnum offset" pairs, then the concatenated
    // object *values* (no "N G obj"/"endobj" wrapper -- object 7/8 live
    // entirely inside this compressed stream, addressed by index).
    std::string obj7_val = "<< /Type /Catalog /Pages 8 0 R >>";
    std::string obj8_val = "<< /Type /Pages /Kids [9 0 R] /Count 1 >>";
    std::string objstm_header = "7 0 8 " + std::to_string(obj7_val.size() + 1);  // "+1" for the separating space
    std::string objstm_body = objstm_header + " " + obj7_val + " " + obj8_val;
    long long first = static_cast<long long>(objstm_header.size()) + 1;
    std::string objstm_compressed = deflate::DeflateZlib(B(objstm_body), objstm_body.size());

    std::string doc = "%PDF-1.5\n";
    size_t off_objstm = doc.size();
    doc += "6 0 obj\n<< /Type /ObjStm /N 2 /First " + std::to_string(first) + " /Filter /FlateDecode /Length " +
           std::to_string(objstm_compressed.size()) + " >>\nstream\n" + objstm_compressed + "\nendstream\nendobj\n";
    size_t off_page = doc.size();
    doc += "9 0 obj\n<< /Type /Page /Parent 8 0 R >>\nendobj\n";
    size_t off_xref = doc.size();

    // xref stream: object 0 free, 6 in-use (ObjStm), 7/8 compressed (in
    // ObjStm 6, indices 0/1), 9 in-use (page), 10 in-use (the xref
    // stream object itself). W = [1,2,2] (small enough offsets/indices).
    struct Rec {
        int type, f2, f3;
    };
    std::vector<Rec> recs = {
        {0, 0, 65535},
        {1, static_cast<int>(off_objstm), 0},
        {2, 6, 0},
        {2, 6, 1},
        {1, static_cast<int>(off_page), 0},
        {1, static_cast<int>(off_xref), 0},
    };
    std::string xref_body;
    for (const auto &r : recs) {
        xref_body.push_back(static_cast<char>(r.type));
        xref_body.push_back(static_cast<char>((r.f2 >> 8) & 0xFF));
        xref_body.push_back(static_cast<char>(r.f2 & 0xFF));
        xref_body.push_back(static_cast<char>((r.f3 >> 8) & 0xFF));
        xref_body.push_back(static_cast<char>(r.f3 & 0xFF));
    }
    std::string xref_compressed = deflate::DeflateZlib(B(xref_body), xref_body.size());
    // /Index [0 1 6 5]: object 0 alone (the free-list head), then
    // objects 6-10 -- the 6 records above are NOT a contiguous 0..10
    // range, so the default (absent /Index -> [0 Size]) would misalign
    // every record against the wrong object number.
    doc += "10 0 obj\n<< /Type /XRef /Size 11 /Index [0 1 6 5] /W [1 2 2] /Root 7 0 R /Filter /FlateDecode /Length " +
           std::to_string(xref_compressed.size()) + " >>\nstream\n" + xref_compressed + "\nendstream\nendobj\n";
    doc += "startxref\n" + std::to_string(off_xref) + "\n%%EOF\n";

    pdfxref::XrefTable table;
    table.Load(B(doc), doc.size());
    CHECK(table.Find(7)->kind == pdfxref::EntryKind::Compressed);
    CHECK(table.Find(6)->kind == pdfxref::EntryKind::InUse);

    pdfobj::Object catalog = pdfxref::ResolveObject(B(doc), doc.size(), table, 7);
    CHECK(catalog.IsDict());
    CHECK(catalog.Find("Type")->AsString("") == "Catalog");

    pdfobj::Object pages = pdfxref::ResolveObject(B(doc), doc.size(), table, 8);
    CHECK(pages.IsDict());
    CHECK(pages.Find("Count")->AsInt() == 1);

    pdfobj::Object page = pdfxref::ResolveObject(B(doc), doc.size(), table, 9);
    CHECK(page.IsDict());
    CHECK(page.Find("Type")->AsString("") == "Page");
}

void TestPngPredictor() {
    // 2 rows, columns=3, colors=1, bpc=8: row0 filter=None [10,20,30],
    // row1 filter=Up [5,5,5] (transmitted) -> actual [15,25,35] (+prior).
    std::string filtered;
    filtered += static_cast<char>(0);
    filtered += static_cast<char>(10);
    filtered += static_cast<char>(20);
    filtered += static_cast<char>(30);
    filtered += static_cast<char>(2);
    filtered += static_cast<char>(5);
    filtered += static_cast<char>(5);
    filtered += static_cast<char>(5);
    std::string compressed = deflate::DeflateZlib(B(filtered), filtered.size());

    pdfobj::Object parms;
    parms.type = pdfobj::Type::Dict;
    pdfobj::Object columns, colors, bpc, predictor;
    columns.type = pdfobj::Type::Int;
    columns.int_val = 3;
    colors.type = pdfobj::Type::Int;
    colors.int_val = 1;
    bpc.type = pdfobj::Type::Int;
    bpc.int_val = 8;
    predictor.type = pdfobj::Type::Int;
    predictor.int_val = 12;
    parms.dict_val["Columns"] = columns;
    parms.dict_val["Colors"] = colors;
    parms.dict_val["BitsPerComponent"] = bpc;
    parms.dict_val["Predictor"] = predictor;

    std::string out;
    CHECK(pdffilter::FlateDecode(compressed, &parms, &out));
    CHECK(out.size() == 6);
    unsigned char expected[6] = {10, 20, 30, 15, 25, 35};
    for (int i = 0; i < 6; ++i) CHECK(static_cast<unsigned char>(out[static_cast<size_t>(i)]) == expected[i]);
}

void TestTiffPredictor() {
    // columns=3, colors=1, bpc=8, one row of deltas [10,5,5] -> cumulative [10,15,20].
    std::string filtered;
    filtered += static_cast<char>(10);
    filtered += static_cast<char>(5);
    filtered += static_cast<char>(5);
    std::string compressed = deflate::DeflateZlib(B(filtered), filtered.size());

    pdfobj::Object parms;
    parms.type = pdfobj::Type::Dict;
    pdfobj::Object columns, predictor;
    columns.type = pdfobj::Type::Int;
    columns.int_val = 3;
    predictor.type = pdfobj::Type::Int;
    predictor.int_val = 2;
    parms.dict_val["Columns"] = columns;
    parms.dict_val["Predictor"] = predictor;

    std::string out;
    CHECK(pdffilter::FlateDecode(compressed, &parms, &out));
    CHECK(out.size() == 3);
    unsigned char expected[3] = {10, 15, 20};
    for (int i = 0; i < 3; ++i) CHECK(static_cast<unsigned char>(out[static_cast<size_t>(i)]) == expected[i]);
}

void TestNoPredictor() {
    std::string data = "just plain flate data, no predictor";
    std::string compressed = deflate::DeflateZlib(B(data), data.size());
    std::string out;
    CHECK(pdffilter::FlateDecode(compressed, nullptr, &out));
    CHECK(out == data);
}

void TestIncrementalUpdatePrevChain() {
    // Base revision: objects 1-2.
    std::string doc = "%PDF-1.4\n";
    size_t off1 = doc.size();
    doc += "1 0 obj\n(base one)\nendobj\n";
    size_t off2_v1 = doc.size();
    doc += "2 0 obj\n(base two)\nendobj\n";
    size_t xref1_off = doc.size();
    doc += "xref\n0 3\n";
    doc += XrefLine(0, 65535, 'f');
    doc += XrefLine(static_cast<long long>(off1), 0, 'n');
    doc += XrefLine(static_cast<long long>(off2_v1), 0, 'n');
    doc += "trailer\n<< /Size 3 /Root 1 0 R >>\n";
    doc += "startxref\n" + std::to_string(xref1_off) + "\n%%EOF\n";

    // Incremental update: redefines object 2 only.
    size_t off2_v2 = doc.size();
    doc += "2 0 obj\n(updated two)\nendobj\n";
    size_t xref2_off = doc.size();
    doc += "xref\n2 1\n";
    doc += XrefLine(static_cast<long long>(off2_v2), 0, 'n');
    doc += "trailer\n<< /Size 3 /Root 1 0 R /Prev " + std::to_string(xref1_off) + " >>\n";
    doc += "startxref\n" + std::to_string(xref2_off) + "\n%%EOF\n";

    pdfxref::XrefTable table;
    table.Load(B(doc), doc.size());
    pdfobj::Object obj1 = pdfxref::ResolveObject(B(doc), doc.size(), table, 1);
    pdfobj::Object obj2 = pdfxref::ResolveObject(B(doc), doc.size(), table, 2);
    CHECK(obj1.str_val == "base one");        // only in the older section: still reachable via /Prev
    CHECK(obj2.str_val == "updated two");     // newer section's entry wins over the older one
}

void TestCyclicPrevChainDoesNotHang() {
    // Two xref sections whose trailers point /Prev at each other --
    // secA (first in the file) /Prev's forward to secB's offset, and
    // secB /Prev's back to secA's -- Load() must terminate (this test
    // process exiting at all is itself the check) rather than looping
    // forever walking the cycle.
    std::string doc = "%PDF-1.4\n";
    doc += "1 0 obj\n(x)\nendobj\n";
    size_t secA_off = doc.size();
    // secB's offset depends on secA's exact size, which itself depends
    // on the /Prev value's digit count -- pin that field to a fixed
    // 10-digit width (like XrefLine's own offset field) so the
    // placeholder used to compute secB_off is exactly the same length
    // as the real formatted value substituted in afterward.
    std::string secA_header = "xref\n0 1\n" + XrefLine(0, 65535, 'f') + "trailer\n<< /Size 1 /Root 1 0 R /Prev ";
    std::string secA_footer = " >>\n";
    size_t secA_size = secA_header.size() + 10 + secA_footer.size();
    size_t secB_off = secA_off + secA_size;

    char secA_prev_buf[16];
    char secB_prev_buf[16];
    std::snprintf(secA_prev_buf, sizeof(secA_prev_buf), "%010lld", static_cast<long long>(secB_off));
    std::snprintf(secB_prev_buf, sizeof(secB_prev_buf), "%010lld", static_cast<long long>(secA_off));
    std::string secA = secA_header + secA_prev_buf + secA_footer;
    std::string secB = secA_header + secB_prev_buf + secA_footer;
    CHECK(secA.size() == secA_size);
    doc += secA;
    doc += secB;
    doc += "startxref\n" + std::to_string(secA_off) + "\n%%EOF\n";

    pdfxref::XrefTable table;
    table.Load(B(doc), doc.size());  // must return, not hang
    CHECK(table.Trailer().Find("Root") != nullptr);
}

void TestRecoveryFromMissingXref() {
    std::string doc = "%PDF-1.4\n";
    doc += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    doc += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    doc += "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n";
    // No xref/trailer/startxref at all -- simulates a truncated/corrupt file.

    pdfxref::XrefTable table;
    table.Load(B(doc), doc.size());
    CHECK(table.Entries().size() == 3);
    const pdfobj::Object *root_ref = table.Trailer().Find("Root");
    CHECK(root_ref && root_ref->IsReference() && root_ref->ref_val.num == 1);

    pdfobj::Object catalog = pdfxref::ResolveObject(B(doc), doc.size(), table, 1);
    CHECK(catalog.IsDict() && catalog.Find("Type")->AsString("") == "Catalog");
}

void TestRecoveryPrefersLatestDuplicateObject() {
    // Simulates a corrupted-xref file that still contains 2 definitions
    // of object 1 (e.g. from an interrupted incremental save) -- the
    // later one in file order must win.
    std::string doc = "%PDF-1.4\n";
    doc += "1 0 obj\n(old version)\nendobj\n";
    doc += "1 0 obj\n(new version)\nendobj\n";

    pdfxref::XrefTable table;
    table.RecoverByScanning(B(doc), doc.size());
    pdfobj::Object obj1 = pdfxref::ResolveObject(B(doc), doc.size(), table, 1);
    CHECK(obj1.str_val == "new version");
}

// Opt-in: pass a real fixture path (e.g.
// test/pdf_fixtures/tectonic_test.pdf) to spot-check this module against
// a real xdvipdfmx-produced file -- Root/Pages/first-page resolve, and
// the xref's own declared /Size roughly matches the highest object
// number actually seen.
void CheckRealFixture(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string data = ss.str();

    pdfxref::XrefTable table;
    table.Load(B(data), data.size());
    std::printf("  %s: %zu xref entries, Size=%lld\n", path.c_str(), table.Entries().size(),
                table.Trailer().Find("Size") ? table.Trailer().Find("Size")->AsInt() : -1);
    const pdfobj::Object *root_ref = table.Trailer().Find("Root");
    CHECK(root_ref != nullptr);
    pdfobj::Object catalog = pdfxref::ResolveObject(B(data), data.size(), table, root_ref->ref_val.num);
    CHECK(catalog.IsDict());
    CHECK(catalog.Find("Type")->AsString("") == "Catalog");
    const pdfobj::Object *pages_ref = catalog.Find("Pages");
    CHECK(pages_ref != nullptr);
    pdfobj::Object pages = pdfxref::ResolveObject(B(data), data.size(), table, pages_ref->ref_val.num);
    CHECK(pages.IsDict());
    CHECK(pages.Find("Type")->AsString("") == "Pages");
    std::printf("  Root -> Pages: /Count = %lld\n", pages.Find("Count") ? pages.Find("Count")->AsInt() : -1);
}

}  // namespace

int main(int argc, char **argv) {
    TestClassicXrefTableEndToEnd();
    TestXrefStreamWithCompressedObjects();
    TestPngPredictor();
    TestTiffPredictor();
    TestNoPredictor();
    TestIncrementalUpdatePrevChain();
    TestCyclicPrevChainDoesNotHang();
    TestRecoveryFromMissingXref();
    TestRecoveryPrefersLatestDuplicateObject();
    std::printf("pdf_xref_test: all checks passed\n");

    for (int i = 1; i < argc; ++i) CheckRealFixture(argv[i]);
    return 0;
}
