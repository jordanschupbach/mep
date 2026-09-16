// gfx/type1.h/.cpp coverage: a synthetic Type 1 font program is built
// here (cleartext header, a real eexec-encrypted private portion with
// lenIV-encrypted charstrings, Subrs, CharStrings, custom /Encoding) in
// each of the three layouts InitFont accepts -- PDF-embedded/PFB-style
// binary eexec, PFB segmented container, and hex-encoded PFA -- and
// its glyphs rasterized through the shared gfx::raster outline
// rasterizer. Real-font behavior (Latin Modern / Computer Modern from
// pdflatex output) was verified live against poppler's rendering of
// the same pages.

#include "gfx/type1.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// Type 1 charstring number encoding (spec 6.2): the 32-bit form for
// everything, for builder simplicity.
std::string Num(int v) {
    std::string s;
    s.push_back(static_cast<char>(255));
    s.push_back(static_cast<char>((v >> 24) & 0xFF));
    s.push_back(static_cast<char>((v >> 16) & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
    return s;
}
std::string Op(int op) { return std::string(1, static_cast<char>(op)); }
std::string Esc(int op) { return Op(12) + Op(op); }

// Spec chapter 7 encryption, prefixing `lead` throwaway bytes.
std::string Encrypt(const std::string &plain, uint16_t r, int lead) {
    const uint16_t c1 = 52845, c2 = 22719;
    std::string in(static_cast<size_t>(lead), 'X');
    in += plain;
    std::string out;
    for (char ch : in) {
        unsigned char p = static_cast<unsigned char>(ch);
        unsigned char c = static_cast<unsigned char>(p ^ (r >> 8));
        r = static_cast<uint16_t>((c + r) * c1 + c2);
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// The declared length counts the 4 lenIV lead bytes, as in a real font.
std::string Blob(const std::string &cs) {
    std::string enc = Encrypt(cs, 4330, 4);
    return std::to_string(enc.size()) + " RD " + enc;
}

struct Glyph {
    std::string name, charstring;
};

// A square with its lower-left at the current point, `size` on a side.
std::string Square(int size) { return Num(size) + Op(6) + Num(size) + Op(7) + Num(-size) + Op(6) + Op(9); }

std::string BuildPrivatePortion(const std::vector<Glyph> &glyphs, const std::vector<std::string> &subrs) {
    std::string p = "dup /Private 8 dict dup begin\n/lenIV 4 def\n";
    if (!subrs.empty()) {
        p += "/Subrs " + std::to_string(subrs.size()) + " array\n";
        for (size_t i = 0; i < subrs.size(); ++i) p += "dup " + std::to_string(i) + " " + Blob(subrs[i]) + " NP\n";
        p += "ND\n";
    }
    p += "end\n/CharStrings " + std::to_string(glyphs.size()) + " dict dup begin\n";
    for (const Glyph &g : glyphs) p += "/" + g.name + " " + Blob(g.charstring) + " ND\n";
    p += "end\nend\nmark currentfile closefile\n";
    return p;
}

std::string BuildClearText() {
    return "%!PS-AdobeFont-1.0: SynthFont 001.000\n"
           "/FontName /SynthFont def\n"
           "/FontMatrix [0.001 0 0 0.001 0 0] readonly def\n"
           "/Encoding 256 array\n"
           "0 1 255 {1 index exch /.notdef put} for\n"
           "dup 65 /A put\n"
           "dup 66 /B put\n"
           "dup 200 /C put\n"
           "readonly def\n"
           "currentdict end\ncurrentfile eexec\n";
}

std::vector<Glyph> StandardGlyphs() {
    return {
        {".notdef", Num(0) + Num(250) + Op(13) + Op(14)},
        // A: sidebearing 10, width 600, a 100-unit square at (10,0).
        {"A", Num(10) + Num(600) + Op(13) + Num(0) + Num(0) + Op(21) + Square(100) + Op(14)},
        // B: same square drawn via Subrs[3] (sidebearing 20, width 700).
        {"B", Num(20) + Num(700) + Op(13) + Num(0) + Num(0) + Op(21) + Num(3) + Op(10) + Op(9) + Op(14)},
        // C: seac composite -- base A, accent B displaced by (adx 50, ady 150), asb = B's own sidebearing 20.
        {"C", Num(10) + Num(600) + Op(13) + Num(20) + Num(50) + Num(150) + Num(65) + Num(66) + Esc(6)},
        // D: flex bump from (0,0) to (100,0) via the standard OtherSubrs protocol, then a box beneath.
        {"D", Num(0) + Num(500) + Op(13) + Num(0) + Num(40) + Op(21) +
                  Num(1) + Op(10) +                                          // Subrs[1]: flex start
                  Num(0) + Num(0) + Op(21) + Num(2) + Op(10) +               // reference point
                  Num(10) + Num(20) + Op(21) + Num(2) + Op(10) +             // c1
                  Num(20) + Num(10) + Op(21) + Num(2) + Op(10) +             // c2
                  Num(20) + Num(0) + Op(21) + Num(2) + Op(10) +              // p1 (50,70)
                  Num(20) + Num(0) + Op(21) + Num(2) + Op(10) +              // c3
                  Num(20) + Num(-10) + Op(21) + Num(2) + Op(10) +            // c4
                  Num(10) + Num(-20) + Op(21) + Num(2) + Op(10) +            // p2 (100,40)
                  Num(50) + Num(100) + Num(40) + Num(0) + Op(10) +           // Subrs[0]: flex end (fd, end x, end y)
                  Num(-40) + Op(7) + Num(-100) + Op(6) + Op(9) + Op(14)},
        // E: fractional width via div (spec: `a b div`): 900/2 = 450.
        {"E", Num(0) + Num(900) + Num(2) + Esc(12) + Op(13) + Op(14)},
    };
}

std::vector<std::string> StandardSubrs() {
    return {
        Num(3) + Num(0) + Esc(16) + Esc(17) + Esc(17) + Esc(33) + Op(11),  // 3 0 callothersubr pop pop setcurrentpoint
        Num(0) + Num(1) + Esc(16) + Op(11),                                  // 0 1 callothersubr
        Num(0) + Num(2) + Esc(16) + Op(11),                                  // 0 2 callothersubr
        Square(100) + Op(11),
    };
}

std::string BuildBinaryFont() {
    return BuildClearText() + Encrypt(BuildPrivatePortion(StandardGlyphs(), StandardSubrs()), 55665, 4) +
           std::string(512, '0') + "\ncleartomark\n";
}

std::string Segment(int type, const std::string &data) {
    std::string s;
    s.push_back(static_cast<char>(0x80));
    s.push_back(static_cast<char>(type));
    uint32_t n = static_cast<uint32_t>(data.size());
    for (int b = 0; b < 4; ++b) s.push_back(static_cast<char>((n >> (8 * b)) & 0xFF));
    return s + data;
}

std::string BuildPfbFont() {
    std::string clear = BuildClearText();
    std::string bin = Encrypt(BuildPrivatePortion(StandardGlyphs(), StandardSubrs()), 55665, 4);
    std::string trailer = std::string(512, '0') + "\ncleartomark\n";
    return Segment(1, clear) + Segment(2, bin) + Segment(1, trailer) + "\x80\x03";
}

std::string BuildHexFont() {
    std::string bin = Encrypt(BuildPrivatePortion(StandardGlyphs(), StandardSubrs()), 55665, 4);
    std::string hex;
    const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < bin.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(bin[i]);
        hex.push_back(digits[c >> 4]);
        hex.push_back(digits[c & 15]);
        if (i % 32 == 31) hex.push_back('\n');
    }
    return BuildClearText() + hex + "\n" + std::string(512, '0') + "\ncleartomark\n";
}

const unsigned char *B(const std::string &s) { return reinterpret_cast<const unsigned char *>(s.data()); }

void CheckCommonStructure(const gfx::t1::FontInfo &fi) {
    CHECK(fi.num_glyphs == 6);
    CHECK(fi.subrs.size() == 4);
    CHECK(fi.font_matrix[0] == 0.001);
    CHECK(!fi.encoding_standard);
    CHECK(gfx::t1::GidForName(&fi, "A") == 1);
    CHECK(gfx::t1::GidForName(&fi, "zzz") == -1);
    CHECK(fi.builtin_encoding[65] == 1);
    CHECK(fi.builtin_encoding[66] == 2);
    CHECK(fi.builtin_encoding[200] == 3);
    CHECK(fi.builtin_encoding[67] == -1);
    CHECK(gfx::t1::GetGlyphAdvance(&fi, 1) == 600);
    CHECK(gfx::t1::GetGlyphAdvance(&fi, 5) == 450);  // div
}

void TestBinaryEexecFont() {
    std::string font = BuildBinaryFont();
    gfx::t1::FontInfo fi;
    CHECK(gfx::t1::InitFont(&fi, B(font), static_cast<int>(font.size())));
    CheckCommonStructure(fi);

    int w, h, xo, yo;
    unsigned char *bmp = gfx::t1::GetGlyphBitmap(&fi, 1.0f, 1.0f, 1, &w, &h, &xo, &yo);
    CHECK(bmp != nullptr);
    CHECK(w == 100 && h == 100 && xo == 10 && yo == -100);  // square at (10..110, 0..100), y-down output
    CHECK(bmp[50 * 100 + 50] == 255);
    gfx::t1::FreeBitmap(bmp);

    // B draws its square through a Subr; the outline is identical apart from the sidebearing.
    bmp = gfx::t1::GetGlyphBitmap(&fi, 1.0f, 1.0f, 2, &w, &h, &xo, &yo);
    CHECK(bmp != nullptr);
    CHECK(w == 100 && h == 100 && xo == 20 && yo == -100);
    gfx::t1::FreeBitmap(bmp);

    // Empty outline (.notdef here) -> nullptr, like the other engines.
    CHECK(gfx::t1::GetGlyphBitmap(&fi, 1.0f, 1.0f, 0, &w, &h, &xo, &yo) == nullptr);
}

void TestSeacComposition() {
    std::string font = BuildBinaryFont();
    gfx::t1::FontInfo fi;
    CHECK(gfx::t1::InitFont(&fi, B(font), static_cast<int>(font.size())));
    int w, h, xo, yo;
    unsigned char *bmp = gfx::t1::GetGlyphBitmap(&fi, 1.0f, 1.0f, 3, &w, &h, &xo, &yo);
    CHECK(bmp != nullptr);
    // Base A: x 10..110, y 0..100. Accent B's sidebearing point lands at
    // (sbx + adx - asb, ady) = (10 + 50 - 20, 150) relative to B's own
    // sidebearing (20): square at x 60..160, y 150..250.
    CHECK(xo == 10 && w == 150);
    CHECK(yo == -250 && h == 250);
    CHECK(bmp[(250 - 50) * 150 + (50 - 10)] == 255);   // inside base
    CHECK(bmp[(250 - 200) * 150 + (100 - 10)] == 255); // inside accent
    CHECK(bmp[(250 - 125) * 150 + (50 - 10)] == 0);    // gap between them
    gfx::t1::FreeBitmap(bmp);
}

void TestFlexViaOtherSubrs() {
    std::string font = BuildBinaryFont();
    gfx::t1::FontInfo fi;
    CHECK(gfx::t1::InitFont(&fi, B(font), static_cast<int>(font.size())));
    int w, h, xo, yo;
    unsigned char *bmp = gfx::t1::GetGlyphBitmap(&fi, 1.0f, 1.0f, 4, &w, &h, &xo, &yo);
    CHECK(bmp != nullptr);
    // Box 0..100 x 0..40 with a flex bump on top peaking near y=62.
    CHECK(w == 100 && xo == 0);
    CHECK(h >= 60 && h <= 71);
    CHECK(bmp[(h - 20) * w + 50] == 255);  // inside the box
    CHECK(bmp[(h - 58) * w + 50] > 0);     // inside the bump, near its peak
    CHECK(bmp[(h - 50) * w + 2] == 0);     // the bump is a curve: its ends are low
    gfx::t1::FreeBitmap(bmp);
}

void TestPfbContainerAndHexEexec() {
    std::string pfb = BuildPfbFont();
    gfx::t1::FontInfo fi;
    CHECK(gfx::t1::InitFont(&fi, B(pfb), static_cast<int>(pfb.size())));
    CheckCommonStructure(fi);

    std::string hex = BuildHexFont();
    gfx::t1::FontInfo fh;
    CHECK(gfx::t1::InitFont(&fh, B(hex), static_cast<int>(hex.size())));
    CheckCommonStructure(fh);
    CHECK(fh.charstrings[1] == fi.charstrings[1]);
}

void TestRotationMatrix() {
    std::string font = BuildBinaryFont();
    gfx::t1::FontInfo fi;
    CHECK(gfx::t1::InitFont(&fi, B(font), static_cast<int>(font.size())));
    int w, h, xo, yo;
    // A's square (10..110 x 0..100) through a 90-degree rotation plus
    // half scale: rx = -y/2, ry = x/2.
    unsigned char *bmp = gfx::t1::GetGlyphBitmapMatrix(&fi, 0, 0.5f, -0.5f, 0, 1, &w, &h, &xo, &yo);
    CHECK(bmp != nullptr);
    CHECK(w == 50 && h == 50 && xo == -50 && yo == 5);
    gfx::t1::FreeBitmap(bmp);
}

void TestStandardEncodingAndGarbage() {
    CHECK(std::strcmp(gfx::t1::StandardEncodingName(65), "A") == 0);
    CHECK(std::strcmp(gfx::t1::StandardEncodingName(193), "grave") == 0);
    CHECK(std::strcmp(gfx::t1::StandardEncodingName(251), "germandbls") == 0);
    CHECK(gfx::t1::StandardEncodingName(0) == nullptr);
    CHECK(gfx::t1::StandardEncodingName(128) == nullptr);

    gfx::t1::FontInfo fi;
    std::string junk = "%!PS-AdobeFont-1.0: nothing here\n/FontMatrix [0.001 0 0 0.001 0 0] def\n";
    CHECK(!gfx::t1::InitFont(&fi, B(junk), static_cast<int>(junk.size())));
    std::string truncated = BuildBinaryFont().substr(0, 400);
    CHECK(!gfx::t1::InitFont(&fi, B(truncated), static_cast<int>(truncated.size())) || fi.num_glyphs <= 6);
}

}  // namespace

int main() {
    TestBinaryEexecFont();
    TestSeacComposition();
    TestFlexViaOtherSubrs();
    TestPfbContainerAndHexEexec();
    TestRotationMatrix();
    TestStandardEncodingAndGarbage();
    std::printf("type1_test: all checks passed\n");
    return 0;
}
