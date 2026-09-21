// Coverage for pdf_writer.h/.cpp: (1) the value serializer round-trips
// every pdfobj::Type and formats reals/names/strings/dates per spec;
// (2) BuildIncrementalUpdate appends a revision that mep's OWN xref
// reader parses -- the rewritten page's /Annots grows, the new /Highlight
// and /Text objects resolve, the highlight's /AP /N is a Flate-decodable
// Form XObject, the trailer's /Prev chains to the original startxref, and
// pdfannots::GetPageAnnots reads the two new annotations back with
// point-space geometry matching what went in.

#include "pdf_writer.h"

#include "pdf_annots.h"
#include "pdf_document.h"
#include "pdf_filters.h"
#include "pdf_object.h"
#include "pdf_xref.h"

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
bool Near(double a, double b) { return std::fabs(a - b) < 1e-4; }

std::string XrefLine(long long offset, int gen, char kind) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%010lld %05d %c \n", offset, gen, kind);
    return buf;
}

std::string BuildDoc(const std::vector<std::pair<int, std::string>> &objects, int root_num, size_t *xref_off_out) {
    std::string doc = "%PDF-1.4\n";
    std::vector<std::pair<int, size_t>> offsets;
    int max_num = 0;
    for (const auto &obj : objects) {
        offsets.emplace_back(obj.first, doc.size());
        doc += std::to_string(obj.first) + " 0 obj\n" + obj.second + "\nendobj\n";
        max_num = std::max(max_num, obj.first);
    }
    size_t xref_off = doc.size();
    if (xref_off_out) *xref_off_out = xref_off;
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

// -- serializer unit tests -------------------------------------------

void TestFormatReal() {
    CHECK(pdfwrite::FormatReal(0.0) == "0");
    CHECK(pdfwrite::FormatReal(-0.0) == "0");
    CHECK(pdfwrite::FormatReal(1.0) == "1");
    CHECK(pdfwrite::FormatReal(1.5) == "1.5");
    CHECK(pdfwrite::FormatReal(100.25) == "100.25");
    CHECK(pdfwrite::FormatReal(-3.5) == "-3.5");
    // Never exponent notation.
    std::string tiny = pdfwrite::FormatReal(1e-5);
    CHECK(tiny.find('e') == std::string::npos && tiny.find('E') == std::string::npos);
}

void TestEncoders() {
    CHECK(pdfwrite::EncodeName("Normal") == "Normal");
    CHECK(pdfwrite::EncodeName("A B") == "A#20B");
    CHECK(pdfwrite::EncodeName("a/b#c") == "a#2Fb#23c");
    CHECK(pdfwrite::EncodeLiteralString("hi") == "(hi)");
    CHECK(pdfwrite::EncodeLiteralString("a(b)c\\") == "(a\\(b\\)c\\\\)");
    CHECK(pdfwrite::EncodeLiteralString("x\ny") == "(x\\ny)");
    std::string date = pdfwrite::PdfDateString(0);  // epoch, UTC
    CHECK(date == "D:19700101000000+00'00'");
}

// Serialize a value, parse it back, and compare structurally for the
// types the annotation writer emits.
void TestRoundTripValues() {
    pdfobj::Object dict;
    dict.type = pdfobj::Type::Dict;
    pdfobj::Object arr;
    arr.type = pdfobj::Type::Array;
    pdfobj::Object n;
    n.type = pdfobj::Type::Real;
    n.real_val = 12.5;
    arr.array_val.push_back(n);
    pdfobj::Object ref;
    ref.type = pdfobj::Type::Reference;
    ref.ref_val = {7, 0};
    arr.array_val.push_back(ref);
    dict.dict_val["Nums"] = arr;
    pdfobj::Object name;
    name.type = pdfobj::Type::Name;
    name.str_val = "High light";  // exercises name escaping
    dict.dict_val["Kind"] = name;
    pdfobj::Object str;
    str.type = pdfobj::Type::String;
    str.str_val = "a (parenthetical) note";
    dict.dict_val["Contents"] = str;

    std::string text = pdfwrite::SerializeValue(dict);
    size_t pos = 0;
    pdfobj::Object parsed;
    CHECK(pdfobj::ParseObject(B(text), text.size(), pos, &parsed));
    CHECK(parsed.IsDict());
    CHECK(parsed.Find("Kind") && parsed.Find("Kind")->str_val == "High light");
    CHECK(parsed.Find("Contents") && parsed.Find("Contents")->str_val == "a (parenthetical) note");
    const pdfobj::Object *nums = parsed.Find("Nums");
    CHECK(nums && nums->IsArray() && nums->array_val.size() == 2);
    CHECK(std::fabs(nums->array_val[0].AsDouble() - 12.5) < 1e-9);
    CHECK(nums->array_val[1].IsReference() && nums->array_val[1].ref_val.num == 7);
}

// -- incremental-update round trip -----------------------------------

std::string BasePageDoc(size_t *xref_off) {
    return BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},  // no /Annots yet
        },
        1, xref_off);
}

void TestIncrementalAddAnnots() {
    size_t base_xref_off = 0;
    std::string base = BasePageDoc(&base_xref_off);
    pdfdoc::PdfDocument document;
    document.Load(B(base), base.size());
    CHECK(document.PageCount() == 1);

    pdfannots::PdfAnnot hi;
    hi.page = 0;
    hi.kind = pdfannots::Kind::Highlight;
    hi.rect[0] = 100; hi.rect[1] = 720; hi.rect[2] = 200; hi.rect[3] = 740;
    hi.quads.push_back({100, 740, 200, 740, 100, 720, 200, 720});
    hi.color[0] = 1; hi.color[1] = 1; hi.color[2] = 0;
    hi.contents = "great point";
    hi.author = "me";

    pdfannots::PdfAnnot note;
    note.page = 0;
    note.kind = pdfannots::Kind::Text;
    note.rect[0] = 300; note.rect[1] = 700; note.rect[2] = 318; note.rect[3] = 718;
    note.contents = "a marginal note";
    note.icon = "Comment";

    std::string err;
    std::string updated = pdfwrite::BuildIncrementalUpdate(B(base), base.size(), document.Xref(), document,
                                                           {hi, note}, {}, {}, /*now=*/0, &err);
    CHECK(err.empty());
    CHECK(!updated.empty());
    // Original bytes preserved verbatim (incremental update only appends).
    CHECK(updated.compare(0, base.size(), base) == 0);

    // Re-parse the whole updated file with mep's own reader.
    pdfdoc::PdfDocument doc2;
    doc2.Load(B(updated), updated.size());
    CHECK(doc2.PageCount() == 1);

    // Trailer: /Size grew, /Prev chains to the base's startxref.
    const pdfobj::Object &tr = doc2.Xref().Trailer();
    CHECK(tr.Find("Size") && tr.Find("Size")->AsInt() >= 7);
    CHECK(tr.Find("Prev") && static_cast<size_t>(tr.Find("Prev")->AsInt()) == base_xref_off);

    // The page's /Annots now has two entries and resolves at its new offset.
    const pdfdoc::Page *page = doc2.GetPage(0);
    CHECK(page && page->object_num == 3);
    const pdfobj::Object *annots = page->dict.Find("Annots");
    CHECK(annots && annots->IsArray() && annots->array_val.size() == 2);

    // Read them back through the annots reader with point-space geometry.
    auto read = pdfannots::GetPageAnnots(B(updated), updated.size(), doc2.Xref(), doc2, 0);
    CHECK(read.size() == 2);
    const pdfannots::PdfAnnot &rh = read[0];
    CHECK(rh.kind == pdfannots::Kind::Highlight);
    CHECK(rh.quads.size() == 1);
    CHECK(Near(rh.quads[0].x1, 100) && Near(rh.quads[0].y1, 740));
    CHECK(Near(rh.quads[0].x4, 200) && Near(rh.quads[0].y4, 720));
    CHECK(Near(rh.rect[0], 100) && Near(rh.rect[3], 740));
    CHECK(rh.contents == "great point" && rh.author == "me");
    CHECK(rh.modified == "D:19700101000000+00'00'");
    const pdfannots::PdfAnnot &rt = read[1];
    CHECK(rt.kind == pdfannots::Kind::Text);
    CHECK(rt.contents == "a marginal note" && rt.icon == "Comment");

    // The highlight carries a Flate-decodable /AP /N Form XObject.
    pdfobj::Object hobj = pdfxref::ResolveObject(B(updated), updated.size(), doc2.Xref(), rh.src_obj, rh.src_gen);
    const pdfobj::Object *ap = hobj.Find("AP");
    CHECK(ap && ap->IsDict());
    const pdfobj::Object *apn = ap->Find("N");
    CHECK(apn && apn->IsReference());
    pdfobj::Object ap_dict;
    std::string ap_raw, ap_dec;
    CHECK(pdfxref::ResolveStream(B(updated), updated.size(), doc2.Xref(), apn->ref_val.num, apn->ref_val.gen,
                                 &ap_dict, &ap_raw));
    CHECK(pdffilter::DecodeStream(ap_raw, &ap_dict, &ap_dec));
    CHECK(!ap_dec.empty());
    CHECK(ap_dec.find("re") != std::string::npos && ap_dec.find("f") != std::string::npos);
    const pdfobj::Object *st = ap_dict.Find("Subtype");
    CHECK(st && st->str_val == "Form");
}

// Add two annots, then in a SECOND incremental revision edit the highlight's
// text (re-emitted at its own object number) and delete the note (ref removed
// from /Annots) -- both re-parsed by mep's own reader.
void TestIncrementalEditDelete() {
    size_t off = 0;
    std::string base = BasePageDoc(&off);
    pdfdoc::PdfDocument d0;
    d0.Load(B(base), base.size());

    pdfannots::PdfAnnot hi;
    hi.page = 0; hi.kind = pdfannots::Kind::Highlight;
    hi.rect[0] = 100; hi.rect[1] = 720; hi.rect[2] = 200; hi.rect[3] = 740;
    hi.quads.push_back({100, 740, 200, 740, 100, 720, 200, 720});
    hi.contents = "orig";
    pdfannots::PdfAnnot note;
    note.page = 0; note.kind = pdfannots::Kind::Text;
    note.rect[0] = 300; note.rect[1] = 700; note.rect[2] = 318; note.rect[3] = 718;
    note.contents = "to be deleted";

    std::string err;
    std::string v1 = pdfwrite::BuildIncrementalUpdate(B(base), base.size(), d0.Xref(), d0, {hi, note}, {}, {}, 0, &err);
    CHECK(err.empty() && !v1.empty());
    pdfdoc::PdfDocument d1;
    d1.Load(B(v1), v1.size());
    auto a1 = pdfannots::GetPageAnnots(B(v1), v1.size(), d1.Xref(), d1, 0);
    CHECK(a1.size() == 2);
    // Identify highlight vs note object numbers.
    int h_obj = 0, n_obj = 0;
    pdfannots::PdfAnnot h_full;
    for (const auto &a : a1) {
        if (a.kind == pdfannots::Kind::Highlight) { h_obj = a.src_obj; h_full = a; }
        else n_obj = a.src_obj;
    }
    CHECK(h_obj > 0 && n_obj > 0);

    // Edit the highlight's contents (re-emit at its own object number);
    // delete the note.
    pdfannots::PdfAnnot edit = h_full;   // keeps geometry/colour/provenance
    edit.contents = "edited note";
    edit.from_file = true;               // (already true from the reader)
    std::vector<pdfwrite::AnnotDelete> dels = {{0, n_obj}};
    std::string v2 = pdfwrite::BuildIncrementalUpdate(B(v1), v1.size(), d1.Xref(), d1, {}, {edit}, dels, 0, &err);
    CHECK(err.empty() && !v2.empty());
    CHECK(v2.compare(0, v1.size(), v1) == 0);  // v1 preserved verbatim

    pdfdoc::PdfDocument d2;
    d2.Load(B(v2), v2.size());
    const pdfdoc::Page *p = d2.GetPage(0);
    const pdfobj::Object *annots = p->dict.Find("Annots");
    CHECK(annots && annots->IsArray() && annots->array_val.size() == 1);  // note ref removed
    auto a2 = pdfannots::GetPageAnnots(B(v2), v2.size(), d2.Xref(), d2, 0);
    CHECK(a2.size() == 1);
    CHECK(a2[0].kind == pdfannots::Kind::Highlight);
    CHECK(a2[0].src_obj == h_obj);          // edited in place (same object number)
    CHECK(a2[0].contents == "edited note"); // new text
    CHECK(Near(a2[0].quads[0].x1, 100));    // geometry preserved
}

void TestEncryptedRefused() {
    // A minimal doc whose trailer declares /Encrypt -> XrefTable::IsEncrypted().
    size_t off = 0;
    std::string base = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 612 792] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},
            {4, "<< /Filter /Standard /V 1 /R 2 /O <00> /U <00> /P -4 >>"},
        },
        1, &off);
    // Inject /Encrypt into the trailer (BuildDoc doesn't add one).
    std::string needle = "/Root 1 0 R";
    auto p = base.find(needle);
    base.replace(p, needle.size(), "/Root 1 0 R /Encrypt 4 0 R /ID [<00><00>]");
    pdfdoc::PdfDocument document;
    document.Load(B(base), base.size());

    pdfannots::PdfAnnot note;
    note.page = 0;
    note.kind = pdfannots::Kind::Text;
    note.contents = "x";
    std::string err;
    std::string updated = pdfwrite::BuildIncrementalUpdate(B(base), base.size(), document.Xref(), document, {note}, {},
                                                           {}, 0, &err);
    CHECK(updated.empty());
    CHECK(!err.empty());
}

}  // namespace

int main() {
    TestFormatReal();
    TestEncoders();
    TestRoundTripValues();
    TestIncrementalAddAnnots();
    TestIncrementalEditDelete();
    TestEncryptedRefused();
    std::printf("pdf_writer_test: all checks passed\n");
    return 0;
}
