// PDFIUM_REMOVAL_PLAN.md Phase 10 coverage for pdf_font.h/.cpp's font-
// dict wiring. A `BuildDoc`-style helper (same "compute offsets from
// actual string positions" approach as pdf_xref_test.cpp/
// pdf_document_test.cpp/pdf_content_test.cpp) builds synthetic simple
// and composite font dicts to test /Widths, /W, /Differences,
// /CIDToGIDMap, and standard-14 substitution in isolation. Real
// embedded-font behavior (actual glyph outlines resolving correctly
// through a real CFF/TrueType program) is verified separately via
// pdf_content_test.cpp's end-to-end text-rendering tests against real
// fixture fonts, since that needs the full RenderContentStream pipeline
// to be meaningful (a bitmap in isolation proves little without seeing
// it drawn at the right place).

#include "pdf_font.h"

#include "gfx/cff.h"
#include "gfx/type1.h"
#include "pdf_encodings.h"

#include <algorithm>
#include <cstring>
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

pdfobj::Object LoadFontDict(const std::string &doc, pdfxref::XrefTable *table, int font_obj_num) {
    table->Load(B(doc), doc.size());
    return pdfxref::ResolveObject(B(doc), doc.size(), *table, font_obj_num);
}

void TestStandard14SubstitutionResolvesGlyphs() {
    // No FontFile at all -> standard-14 (Helvetica) substitution via
    // mep's own vendored Liberation Sans.
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [] >>"},
            {3, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /FirstChar 65 /LastChar 65 /Widths [700] >>"},
        },
        1);
    pdfxref::XrefTable table;
    pdfobj::Object font_dict = LoadFontDict(doc, &table, 3);
    CHECK(font_dict.IsDict());

    pdffont::PdfFont font;
    font.Load(B(doc), doc.size(), table, font_dict);
    CHECK(font.BytesPerCode() == 1);
    CHECK(font.GetWidth('A') == 700);  // from /Widths, not a substitute-derived fallback

    int w, h, xoff, yoff;
    unsigned char *bmp = font.GetGlyphBitmap('A', 20.0f, 20.0f, &w, &h, &xoff, &yoff);
    CHECK(bmp != nullptr);  // Liberation Sans has a real 'A' glyph
    CHECK(w > 0 && h > 0);
    font.FreeGlyphBitmap(bmp);
}

void TestStandard14BoldItalicSelection() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [] >>"},
            {3, "<< /Type /Font /Subtype /Type1 /BaseFont /Times-BoldItalic >>"},
            {4, "<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>"},
        },
        1);
    pdfxref::XrefTable table;
    // Both should resolve to SOME glyph for 'A' (Liberation Serif Bold
    // Italic / Liberation Mono Regular respectively) -- the exact
    // substitute chosen isn't independently observable through this
    // API, but a successful, differently-shaped bitmap for each
    // confirms distinct font data actually loaded (same font bytes
    // would still both "work"; the real point is neither silently
    // fails to resolve any glyph engine at all).
    for (int obj_num : {3, 4}) {
        pdfobj::Object font_dict = LoadFontDict(doc, &table, obj_num);
        pdffont::PdfFont font;
        font.Load(B(doc), doc.size(), table, font_dict);
        int w, h, xoff, yoff;
        unsigned char *bmp = font.GetGlyphBitmap('A', 20.0f, 20.0f, &w, &h, &xoff, &yoff);
        CHECK(bmp != nullptr);
        font.FreeGlyphBitmap(bmp);
    }
}

void TestSymbolFontSkipsGlyphsButKeepsWidths() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [] >>"},
            {3, "<< /Type /Font /Subtype /Type1 /BaseFont /Symbol /FirstChar 97 /LastChar 97 /Widths [500] >>"},
        },
        1);
    pdfxref::XrefTable table;
    pdfobj::Object font_dict = LoadFontDict(doc, &table, 3);
    pdffont::PdfFont font;
    font.Load(B(doc), doc.size(), table, font_dict);
    CHECK(font.GetWidth(97) == 500);  // width tracked even with no glyph substitute
    int w, h, xoff, yoff;
    unsigned char *bmp = font.GetGlyphBitmap(97, 20.0f, 20.0f, &w, &h, &xoff, &yoff);
    CHECK(bmp == nullptr);  // no Liberation equivalent: glyph skipped, not drawn
}

void TestMissingWidthFallback() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [] >>"},
            {3, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /FirstChar 65 /LastChar 66 /Widths [700 0] "
                "/FontDescriptor 4 0 R >>"},
            {4, "<< /Type /FontDescriptor /MissingWidth 250 >>"},
        },
        1);
    pdfxref::XrefTable table;
    pdfobj::Object font_dict = LoadFontDict(doc, &table, 3);
    pdffont::PdfFont font;
    font.Load(B(doc), doc.size(), table, font_dict);
    CHECK(font.GetWidth('A') == 700);   // in range, from /Widths
    CHECK(font.GetWidth('Z') == 250);   // outside FirstChar..LastChar: MissingWidth via FontDescriptor (a real bug this phase fixed -- see PDFIUM_REMOVAL_PLAN.md)
}

void TestDifferencesOverridesBaseEncoding() {
    // WinAnsiEncoding base, with code 65 ('A') remapped to /space via
    // Differences -- confirms Differences actually overrides, not just
    // parses without effect.
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [] >>"},
            {3, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding 4 0 R >>"},
            {4, "<< /BaseEncoding /WinAnsiEncoding /Differences [65 /space] >>"},
        },
        1);
    pdfxref::XrefTable table;
    pdfobj::Object font_dict = LoadFontDict(doc, &table, 3);
    pdffont::PdfFont font;
    font.Load(B(doc), doc.size(), table, font_dict);
    int w, h, xoff, yoff;
    // Code 65 now maps to /space (an empty outline) -> nullptr, not the
    // 'A' glyph WinAnsiEncoding alone would have given it.
    unsigned char *bmp = font.GetGlyphBitmap(65, 20.0f, 20.0f, &w, &h, &xoff, &yoff);
    CHECK(bmp == nullptr);
    if (bmp) font.FreeGlyphBitmap(bmp);
    // Code 66 ('B'), untouched by Differences, still resolves normally.
    bmp = font.GetGlyphBitmap(66, 20.0f, 20.0f, &w, &h, &xoff, &yoff);
    CHECK(bmp != nullptr);
    font.FreeGlyphBitmap(bmp);
}

void TestCompositeFontWArrayBothForms() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [] >>"},
            {3, "<< /Type /Font /Subtype /Type0 /Encoding /Identity-H /DescendantFonts [4 0 R] >>"},
            {4, "<< /Type /Font /Subtype /CIDFontType0 /DW 1000 /W [10 [200 300] 20 25 400] >>"},
        },
        1);
    pdfxref::XrefTable table;
    pdfobj::Object font_dict = LoadFontDict(doc, &table, 3);
    pdffont::PdfFont font;
    font.Load(B(doc), doc.size(), table, font_dict);
    CHECK(font.BytesPerCode() == 2);
    CHECK(!font.WordSpacingApplies(32));  // never applies to a 2-byte composite font, even for the ASCII-32 CID value
    CHECK(font.GetWidth(10) == 200);      // format 1: c [w1 w2 ...] grouped individual widths
    CHECK(font.GetWidth(11) == 300);
    CHECK(font.GetWidth(20) == 400);      // format 2: c_first c_last w uniform range
    CHECK(font.GetWidth(25) == 400);
    CHECK(font.GetWidth(999) == 1000);    // outside W entirely: falls back to DW
}

void TestCompositeFontDefaultWidthWithNoW() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [] >>"},
            {3, "<< /Type /Font /Subtype /Type0 /Encoding /Identity-H /DescendantFonts [4 0 R] >>"},
            {4, "<< /Type /Font /Subtype /CIDFontType0 >>"},
        },
        1);
    pdfxref::XrefTable table;
    pdfobj::Object font_dict = LoadFontDict(doc, &table, 3);
    pdffont::PdfFont font;
    font.Load(B(doc), doc.size(), table, font_dict);
    CHECK(font.GetWidth(5) == 1000);  // spec default DW when absent
}

}  // namespace

void TestType3FontLoadsCharProcsAndScalesWidths() {
    std::string proc = "100 0 d0 0 0 100 100 re f";
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [] >>"},
            {3, "<< /Type /Font /Subtype /Type3 /FontMatrix [0.01 0 0 0.01 0 0] /CharProcs 4 0 R "
                "/Encoding << /Differences [65 /sq] >> /FirstChar 65 /LastChar 66 /Widths [50 80] /Resources 6 0 R >>"},
            {4, "<< /sq 5 0 R >>"},
            {5, "<< /Length " + std::to_string(proc.size()) + " >>\nstream\n" + proc + "\nendstream"},
            {6, "<< /ProcSet [/PDF] >>"},
        },
        1);
    pdfxref::XrefTable table;
    pdfobj::Object font_dict = LoadFontDict(doc, &table, 3);
    pdffont::PdfFont font;
    font.Load(B(doc), doc.size(), table, font_dict);
    CHECK(font.IsType3());
    CHECK(font.BytesPerCode() == 1);
    // /Widths are in glyph space: 50 * FontMatrix.a (0.01) = 0.5 em = 500/1000.
    CHECK(font.GetWidth(65) == 500);
    CHECK(font.GetWidth(66) == 800);
    CHECK(font.Type3FontMatrix()[0] == 0.01);
    CHECK(font.Type3Resources().IsDict());
    std::string content;
    CHECK(font.Type3CharProc(65, &content));
    CHECK(content == proc);
    CHECK(!font.Type3CharProc(66, &content));  // no /Differences name -> no glyph procedure
    int w, h, xoff, yoff;
    CHECK(font.GetGlyphBitmap(65, 20.0f, 20.0f, &w, &h, &xoff, &yoff) == nullptr);  // no outline engine
    CHECK(font.GetUnicodeText(65).empty());  // "sq" is no AGL name; nothing to extract
}

// The three copies of StandardEncoding (pdf_encodings.h's name table,
// gfx/cff.cpp's code -> SID table, gfx/type1.cpp's code -> name table)
// must agree, or a seac/"Builtin"-encoded glyph resolves differently
// depending on which engine happens to draw it.
void TestStandardEncodingTablesAgree() {
    for (int c = 0; c < 256; ++c) {
        const char *name = pdfenc::EncodingName(pdfenc::Base::kStandard, c);
        int sid = gfx::cff::StandardEncodingSid(c);
        const char *t1_name = gfx::t1::StandardEncodingName(c);
        CHECK((name == nullptr) == (sid == 0));
        CHECK((name == nullptr) == (t1_name == nullptr));
        if (!name) continue;
        CHECK(std::strcmp(pdfenc::CffStandardString(sid), name) == 0);
        CHECK(std::strcmp(t1_name, name) == 0);
    }
}

// -- Minimal Type 1 font program builder (same scheme as gfx/type1_test.cpp) --
std::string T1Num(int v) {
    std::string s;
    s.push_back(static_cast<char>(255));
    s.push_back(static_cast<char>((v >> 24) & 0xFF));
    s.push_back(static_cast<char>((v >> 16) & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
    return s;
}
std::string T1Op(int op) { return std::string(1, static_cast<char>(op)); }
std::string T1Encrypt(const std::string &plain, uint16_t r) {
    std::string in(4, 'X');
    in += plain;
    std::string out;
    for (char ch : in) {
        unsigned char p = static_cast<unsigned char>(ch);
        unsigned char c = static_cast<unsigned char>(p ^ (r >> 8));
        r = static_cast<uint16_t>((c + r) * 52845 + 22719);
        out.push_back(static_cast<char>(c));
    }
    return out;
}
std::string BuildType1Program() {
    // Glyph "box": sidebearing 0, width 600, a 100x100 square; glyph
    // "eacute" (a math-style name no substitute font could resolve): a
    // 50x50 square, at code 11 in the font's OWN encoding only.
    std::string box = T1Num(0) + T1Num(600) + T1Op(13) + T1Num(0) + T1Num(0) + T1Op(21) + T1Num(100) + T1Op(6) +
                      T1Num(100) + T1Op(7) + T1Num(-100) + T1Op(6) + T1Op(9) + T1Op(14);
    std::string alpha = T1Num(0) + T1Num(400) + T1Op(13) + T1Num(0) + T1Num(0) + T1Op(21) + T1Num(50) + T1Op(6) +
                        T1Num(50) + T1Op(7) + T1Num(-50) + T1Op(6) + T1Op(9) + T1Op(14);
    std::string notdef = T1Num(0) + T1Num(250) + T1Op(13) + T1Op(14);
    auto entry = [](const std::string &name, const std::string &cs) {
        std::string enc = T1Encrypt(cs, 4330);  // declared length includes the 4 lenIV lead bytes
        return "/" + name + " " + std::to_string(enc.size()) + " RD " + enc + " ND\n";
    };
    std::string priv = "dup /Private 8 dict dup begin\n/lenIV 4 def\nend\n/CharStrings 3 dict dup begin\n" +
                       entry(".notdef", notdef) + entry("box", box) + entry("eacute", alpha) + "end\nend\n";
    std::string clear =
        "%!PS-AdobeFont-1.0: SynthT1\n/FontMatrix [0.001 0 0 0.001 0 0] readonly def\n/Encoding 256 array\n"
        "0 1 255 {1 index exch /.notdef put} for\ndup 65 /box put\ndup 11 /eacute put\nreadonly def\n"
        "currentdict end\ncurrentfile eexec\n";
    return clear + T1Encrypt(priv, 55665) + std::string(512, '0') + "\ncleartomark\n";
}

void TestEmbeddedType1FontFileUsesBuiltinEncoding() {
    std::string program = BuildType1Program();
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [] >>"},
            // No /Encoding, symbolic flags: exactly how pdflatex embeds a
            // Computer Modern math font -- codes resolve through the
            // font program's own encoding.
            {3, "<< /Type /Font /Subtype /Type1 /BaseFont /ABCDEF+SynthT1 /FirstChar 11 /LastChar 11 /Widths [400] "
                "/FontDescriptor 4 0 R >>"},
            {4, "<< /Type /FontDescriptor /FontName /ABCDEF+SynthT1 /Flags 4 /FontFile 5 0 R >>"},
            {5, "<< /Length " + std::to_string(program.size()) + " >>\nstream\n" + program + "\nendstream"},
        },
        1);
    pdfxref::XrefTable table;
    pdfobj::Object font_dict = LoadFontDict(doc, &table, 3);
    pdffont::PdfFont font;
    font.Load(B(doc), doc.size(), table, font_dict);
    CHECK(!font.IsType3());
    int w, h, xoff, yoff;
    unsigned char *bmp = font.GetGlyphBitmap(11, 100.0f, 100.0f, &w, &h, &xoff, &yoff);  // 100px/em: 50 units -> 5px
    CHECK(bmp != nullptr);
    CHECK(w == 5 && h == 5);
    font.FreeGlyphBitmap(bmp);
    CHECK(font.GetWidth(11) == 400);   // /Widths
    CHECK(font.GetWidth(65) == 600);   // outside /Widths, no /MissingWidth: the glyph's own hsbw advance
    bmp = font.GetGlyphBitmap(65, 100.0f, 100.0f, &w, &h, &xoff, &yoff);
    CHECK(bmp != nullptr);
    CHECK(w == 10 && h == 10);
    font.FreeGlyphBitmap(bmp);
    CHECK(font.GetGlyphBitmap(66, 100.0f, 100.0f, &w, &h, &xoff, &yoff) == nullptr);  // unencoded, symbolic: nothing
    CHECK(font.GetUnicodeText(11) == "\xC3\xA9");  // "eacute" via the Adobe Glyph List, no /ToUnicode needed

    // A /Differences override still wins over the built-in encoding.
    std::string doc2 = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [] >>"},
            {3, "<< /Type /Font /Subtype /Type1 /BaseFont /ABCDEF+SynthT1 /Encoding << /Differences [65 /eacute] >> "
                "/FontDescriptor 4 0 R >>"},
            {4, "<< /Type /FontDescriptor /FontName /ABCDEF+SynthT1 /Flags 4 /FontFile 5 0 R >>"},
            {5, "<< /Length " + std::to_string(program.size()) + " >>\nstream\n" + program + "\nendstream"},
        },
        1);
    pdfxref::XrefTable table2;
    pdfobj::Object font_dict2 = LoadFontDict(doc2, &table2, 3);
    pdffont::PdfFont font2;
    font2.Load(B(doc2), doc2.size(), table2, font_dict2);
    bmp = font2.GetGlyphBitmap(65, 100.0f, 100.0f, &w, &h, &xoff, &yoff);
    CHECK(bmp != nullptr);
    CHECK(w == 5 && h == 5);  // alpha's 50-unit square, not box's 100
    font2.FreeGlyphBitmap(bmp);
}

int main() {
    TestType3FontLoadsCharProcsAndScalesWidths();
    TestStandardEncodingTablesAgree();
    TestEmbeddedType1FontFileUsesBuiltinEncoding();
    TestStandard14SubstitutionResolvesGlyphs();
    TestStandard14BoldItalicSelection();
    TestSymbolFontSkipsGlyphsButKeepsWidths();
    TestMissingWidthFallback();
    TestDifferencesOverridesBaseEncoding();
    TestCompositeFontWArrayBothForms();
    TestCompositeFontDefaultWidthWithNoW();
    std::printf("pdf_font_test: all checks passed\n");
    return 0;
}
