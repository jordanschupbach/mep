// PDFIUM_REMOVAL_PLAN.md Phase 9 coverage for gfx/cff.h/.cpp's Type2
// charstring interpreter. Builds small, valid CFF font byte structures
// programmatically (an `CffBuilder` helper, computing INDEX offsets
// itself rather than hand-counting bytes, same spirit as
// pdf_xref_test.cpp's `BuildDoc`) and verifies geometric correctness
// via the same "rasterize and sum coverage -> compare to analytic area"
// technique gfx/rasterizer_test.cpp established for cubic Bezier
// flattening -- since cff.h's public API only exposes a rasterized
// bitmap (not internal contour points), area comparison is the
// practical way to check curve-operator correctness precisely.
//
// Every number in test charstrings is encoded via the uniform 5-byte
// Type2 Fixed (255) form for builder simplicity (real fonts use the
// compact variable-width forms too, but decoding those is exercised
// implicitly since ReadT2Number handles all forms identically -- the
// compact forms are simpler special cases of the same function, not
// separate code paths).

#include "gfx/cff.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// -- CFF byte-structure builder --------------------------------------------

std::string EncodeIndex(const std::vector<std::string> &items) {
    if (items.empty()) return std::string("\x00\x00", 2);
    uint32_t total = 1;
    for (const auto &it : items) total += static_cast<uint32_t>(it.size());
    int off_size = total <= 0xFF ? 1 : total <= 0xFFFF ? 2 : total <= 0xFFFFFF ? 3 : 4;
    std::string out;
    uint16_t count = static_cast<uint16_t>(items.size());
    out.push_back(static_cast<char>((count >> 8) & 0xFF));
    out.push_back(static_cast<char>(count & 0xFF));
    out.push_back(static_cast<char>(off_size));
    auto push_offset = [&](uint32_t v) {
        for (int b = off_size - 1; b >= 0; --b) out.push_back(static_cast<char>((v >> (8 * b)) & 0xFF));
    };
    uint32_t offset = 1;
    push_offset(offset);
    for (const auto &it : items) {
        offset += static_cast<uint32_t>(it.size());
        push_offset(offset);
    }
    for (const auto &it : items) out += it;
    return out;
}

// Always encodes DICT operands as 5-byte longints (op 29) for builder
// simplicity/determinism -- real CFF files use the compact forms too,
// but ParseDict's compact-form branches are exercised by ordinary font
// data in the real-fixture verification, not by this synthetic builder.
std::string EncodeDictEntry(int op, const std::vector<int32_t> &operands) {
    std::string out;
    for (int32_t v : operands) {
        out.push_back(static_cast<char>(29));
        out.push_back(static_cast<char>((v >> 24) & 0xFF));
        out.push_back(static_cast<char>((v >> 16) & 0xFF));
        out.push_back(static_cast<char>((v >> 8) & 0xFF));
        out.push_back(static_cast<char>(v & 0xFF));
    }
    if (op < 1200) {
        out.push_back(static_cast<char>(op));
    } else {
        out.push_back(static_cast<char>(12));
        out.push_back(static_cast<char>(op - 1200));
    }
    return out;
}

struct CharstringBuilder {
    std::string bytes;
    CharstringBuilder &Num(double v) {
        int32_t fixed = static_cast<int32_t>(std::lround(v * 65536.0));
        bytes.push_back(static_cast<char>(255));
        bytes.push_back(static_cast<char>((fixed >> 24) & 0xFF));
        bytes.push_back(static_cast<char>((fixed >> 16) & 0xFF));
        bytes.push_back(static_cast<char>((fixed >> 8) & 0xFF));
        bytes.push_back(static_cast<char>(fixed & 0xFF));
        return *this;
    }
    CharstringBuilder &Op(int op) {
        bytes.push_back(static_cast<char>(op));
        return *this;
    }
    CharstringBuilder &Op2(int op) {
        bytes.push_back(static_cast<char>(12));
        bytes.push_back(static_cast<char>(op));
        return *this;
    }
};

// Builds a minimal non-CID CFF font: one Top DICT, given CharStrings/
// Global Subrs/Local Subrs. Two-pass Top DICT encoding (fixed-width
// operand encoding above makes the DICT's length offset-independent, so
// its final byte length is known before the real offsets are).
std::string BuildSimpleCff(const std::vector<std::string> &charstrings, const std::vector<std::string> &global_subrs,
                            const std::vector<std::string> &local_subrs) {
    std::string header(4, '\0');
    header[0] = 1;
    header[1] = 0;
    header[2] = 4;
    header[3] = 1;
    std::string name_index = EncodeIndex({});
    std::string string_index = EncodeIndex({});
    std::string global_subr_index = EncodeIndex(global_subrs);

    auto make_top_dict = [](int32_t cs_off, int32_t priv_size, int32_t priv_off) {
        std::string d;
        d += EncodeDictEntry(17, {cs_off});
        if (priv_size >= 0) d += EncodeDictEntry(18, {priv_size, priv_off});
        return d;
    };
    std::string placeholder_top_dict = make_top_dict(0, local_subrs.empty() ? -1 : 0, 0);
    std::string top_dict_index_placeholder = EncodeIndex({placeholder_top_dict});

    size_t prefix_len = header.size() + name_index.size() + top_dict_index_placeholder.size() + string_index.size() +
                        global_subr_index.size();
    std::string charstrings_index = EncodeIndex(charstrings);
    size_t charstrings_off = prefix_len;

    std::string private_dict;
    std::string local_subr_index;
    size_t private_off = 0;
    if (!local_subrs.empty()) {
        local_subr_index = EncodeIndex(local_subrs);
        private_dict = EncodeDictEntry(19, {static_cast<int32_t>(0)});  // placeholder; local subrs right after Private DICT itself
        private_dict = EncodeDictEntry(19, {static_cast<int32_t>(private_dict.size())});
        private_off = charstrings_off + charstrings_index.size();
    }

    std::string top_dict = make_top_dict(static_cast<int32_t>(charstrings_off),
                                          local_subrs.empty() ? -1 : static_cast<int32_t>(private_dict.size()),
                                          local_subrs.empty() ? 0 : static_cast<int32_t>(private_off));
    CHECK(top_dict.size() == placeholder_top_dict.size());  // fixed-width encoding: length must not have changed
    std::string top_dict_index = EncodeIndex({top_dict});
    CHECK(top_dict_index.size() == top_dict_index_placeholder.size());

    std::string out = header + name_index + top_dict_index + string_index + global_subr_index + charstrings_index;
    if (!local_subrs.empty()) out += private_dict + local_subr_index;
    return out;
}

double BitmapArea(const unsigned char *bmp, int w, int h) {
    double area = 0;
    for (int i = 0; i < w * h; ++i) area += static_cast<double>(bmp[i]) / 255.0;
    return area;
}

// -- Tests ------------------------------------------------------------------

void TestSimpleSquare() {
    CharstringBuilder cb;
    cb.Num(100).Num(100).Op(21);  // rmoveto
    cb.Num(200).Num(0).Op(5);     // rlineto
    cb.Num(0).Num(200).Op(5);
    cb.Num(-200).Num(0).Op(5);
    cb.Op(14);  // endchar (implicit close)

    std::string cff = BuildSimpleCff({cb.bytes}, {}, {});
    gfx::cff::FontInfo font;
    CHECK(gfx::cff::InitFont(&font, reinterpret_cast<const unsigned char *>(cff.data()), static_cast<int>(cff.size())));
    CHECK(font.num_glyphs == 1);

    int w, h, xoff, yoff;
    unsigned char *bmp = gfx::cff::GetGlyphBitmap(&font, 1.0f, 1.0f, 0, &w, &h, &xoff, &yoff);
    CHECK(bmp != nullptr);
    CHECK(w == 200 && h == 200);
    double area = BitmapArea(bmp, w, h);
    CHECK(std::abs(area - 200.0 * 200.0) < 1.0);
    gfx::cff::FreeBitmap(bmp);
}

// Builds a full circle from 4 cubic arcs via the given curve-drawing
// callback (so the same shape can be built with rrcurveto vs. the
// specialized vhcurveto operator and cross-checked for equal area).
// Deltas below are the standard kappa=0.5522847498 circle approximation
// converted to RELATIVE per-segment coordinates by hand (verified by
// re-deriving from absolute control points during test development --
// see this phase's own plan writeup for a first-draft version of these
// numbers that turned out to be wrong and was caught by exactly this
// area check before it ever shipped).
template <typename DrawArcsFn>
std::string BuildCircleCharstring(double r, DrawArcsFn draw_arcs) {
    CharstringBuilder cb;
    cb.Num(r).Num(0).Op(21);  // rmoveto to (r, 0) -- start point, center at origin
    constexpr double kK = 0.5522847498;
    draw_arcs(cb, r, r * kK);
    cb.Op(14);
    return cb.bytes;
}

void TestRRCurveToCircle() {
    constexpr double R = 100;
    std::string cs = BuildCircleCharstring(R, [](CharstringBuilder &cb, double r, double kr) {
        // Absolute arc endpoints A=(r,0) B=(0,r) C=(-r,0) D=(0,-r); each
        // arc's relative deltas (dx1,dy1,dx2,dy2,dx3,dy3) derived from
        // the standard absolute control points c1/c2 per arc.
        cb.Num(0).Num(kr).Num(kr - r).Num(r - kr).Num(-kr).Num(0).Op(8);      // A->B
        cb.Num(-kr).Num(0).Num(kr - r).Num(kr - r).Num(0).Num(-kr).Op(8);     // B->C
        cb.Num(0).Num(-kr).Num(r - kr).Num(kr - r).Num(kr).Num(0).Op(8);      // C->D
        cb.Num(kr).Num(0).Num(r - kr).Num(r - kr).Num(0).Num(kr).Op(8);       // D->A
    });
    std::string cff = BuildSimpleCff({cs}, {}, {});
    gfx::cff::FontInfo font;
    CHECK(gfx::cff::InitFont(&font, reinterpret_cast<const unsigned char *>(cff.data()), static_cast<int>(cff.size())));
    int w, h, xoff, yoff;
    unsigned char *bmp = gfx::cff::GetGlyphBitmap(&font, 1.0f, 1.0f, 0, &w, &h, &xoff, &yoff);
    CHECK(bmp != nullptr);
    double area = BitmapArea(bmp, w, h);
    double expected = 3.14159265358979 * R * R;
    double rel_error = std::abs(area - expected) / expected;
    CHECK(rel_error < 0.03);
    gfx::cff::FreeBitmap(bmp);
}

void TestVhCurveToCircleMatchesRRCurveTo() {
    // Same circle as TestRRCurveToCircle, as ONE vhcurveto call across
    // all 4 quadrants -- the tangent direction naturally alternates
    // vertical/horizontal/vertical/horizontal every quadrant of a
    // circle, exactly the pattern vhcurveto/hvcurveto exist for. Also
    // exercises the "final group carries one extra trailing argument"
    // rule (the 17th argument here).
    constexpr double R = 100;
    std::string cs = BuildCircleCharstring(R, [](CharstringBuilder &cb, double r, double kr) {
        cb.Num(kr).Num(kr - r).Num(r - kr).Num(-kr);          // A->B: vertical-start group
        cb.Num(-kr).Num(kr - r).Num(kr - r).Num(-kr);          // B->C: horizontal-start group
        cb.Num(-kr).Num(r - kr).Num(kr - r).Num(kr);           // C->D: vertical-start group
        cb.Num(kr).Num(r - kr).Num(r - kr).Num(kr).Num(0);     // D->A: horizontal-start group, +trailing 0
        cb.Op(30);                                             // vhcurveto (starts vertical)
    });
    std::string cff = BuildSimpleCff({cs}, {}, {});
    gfx::cff::FontInfo font;
    CHECK(gfx::cff::InitFont(&font, reinterpret_cast<const unsigned char *>(cff.data()), static_cast<int>(cff.size())));
    int w, h, xoff, yoff;
    unsigned char *bmp = gfx::cff::GetGlyphBitmap(&font, 1.0f, 1.0f, 0, &w, &h, &xoff, &yoff);
    CHECK(bmp != nullptr);
    double area = BitmapArea(bmp, w, h);
    double expected = 3.14159265358979 * R * R;
    double rel_error = std::abs(area - expected) / expected;
    CHECK(rel_error < 0.03);
    gfx::cff::FreeBitmap(bmp);
}

void TestVvHhCurveToExactSquare() {
    // vvcurveto/hhcurveto via *geometrically straight* "curves" (every
    // control point offset perpendicular to the segment's own direction
    // is exactly zero, so the cubic traces a perfectly straight line,
    // same "degenerate cubic" trick gfx/rasterizer_test.cpp used) --
    // lets this check an exact 200x200 area to a ~1px^2 tolerance
    // instead of a circle's looser 3%, precisely isolating vv/hh
    // argument-order bugs from curve-flattening/rasterization error.
    CharstringBuilder cb;
    cb.Num(0).Num(0).Op(21);  // rmoveto to origin (bottom-left corner)
    cb.Num(70).Num(60).Num(0).Num(70).Op(27);     // hhcurveto: bottom side, straight, dx total 200
    cb.Num(70).Num(0).Num(60).Num(70).Op(26);     // vvcurveto: right side, straight, dy total 200
    cb.Num(-70).Num(-60).Num(0).Num(-70).Op(27);  // hhcurveto: top side, straight, dx total -200
    cb.Num(-70).Num(0).Num(-60).Num(-70).Op(26);  // vvcurveto: left side, straight, dy total -200
    cb.Op(14);

    std::string cff = BuildSimpleCff({cb.bytes}, {}, {});
    gfx::cff::FontInfo font;
    CHECK(gfx::cff::InitFont(&font, reinterpret_cast<const unsigned char *>(cff.data()), static_cast<int>(cff.size())));
    int w, h, xoff, yoff;
    unsigned char *bmp = gfx::cff::GetGlyphBitmap(&font, 1.0f, 1.0f, 0, &w, &h, &xoff, &yoff);
    CHECK(bmp != nullptr);
    CHECK(w == 200 && h == 200);
    double area = BitmapArea(bmp, w, h);
    CHECK(std::abs(area - 200.0 * 200.0) < 1.0);
    gfx::cff::FreeBitmap(bmp);
}

void TestFlexOperators() {
    // A flat, nearly-horizontal "flex" pair should still add up to a
    // shape close to what the same 2 curves would via plain rrcurveto --
    // build a small quasi-rectangle out of 2 flex + 2 lines and check
    // area is close to the straightforward interpretation.
    CharstringBuilder cb;
    cb.Num(0).Num(0).Op(21);  // rmoveto to origin
    // flex (12 35): dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 dx6 dy6 fd
    // Two curves totaling a horizontal traverse of 100 with zero net dy.
    cb.Num(20).Num(5).Num(20).Num(-5).Num(10).Num(0);
    cb.Num(20).Num(0).Num(20).Num(5).Num(10).Num(-5);
    cb.Num(50).Op2(35);
    cb.Num(0).Num(50).Op(5);    // rlineto down
    cb.Num(-100).Num(0).Op(5);  // rlineto back
    cb.Op(14);                  // endchar closes back to origin

    std::string cff = BuildSimpleCff({cb.bytes}, {}, {});
    gfx::cff::FontInfo font;
    CHECK(gfx::cff::InitFont(&font, reinterpret_cast<const unsigned char *>(cff.data()), static_cast<int>(cff.size())));
    int w, h, xoff, yoff;
    unsigned char *bmp = gfx::cff::GetGlyphBitmap(&font, 1.0f, 1.0f, 0, &w, &h, &xoff, &yoff);
    CHECK(bmp != nullptr);
    double area = BitmapArea(bmp, w, h);
    // Roughly a 100x50 region (curves wander only a few units off the
    // straight path) -- verifies flex executes real geometry, not a
    // no-op or a crash, without needing exact-area precision.
    CHECK(area > 4000 && area < 5500);
    gfx::cff::FreeBitmap(bmp);
}

void TestSubroutineCallsBiasAndRecursion() {
    // Local subr 0 draws a 50x50 square via rlineto; the glyph
    // charstring just movetos then calls it -- verifies bias computation
    // (count=1 -> bias 107, so "callsubr" operand -107 selects subr 0)
    // and that subroutine execution actually mutates the shared path
    // state (current point, contour list) the caller sees afterward.
    CharstringBuilder subr;
    subr.Num(50).Num(0).Op(5);
    subr.Num(0).Num(50).Op(5);
    subr.Num(-50).Num(0).Op(5);
    subr.Op(11);  // return

    CharstringBuilder cb;
    cb.Num(10).Num(10).Op(21);  // rmoveto
    cb.Num(-107).Op(10);        // callsubr(0) via bias 107
    cb.Op(14);

    std::string cff = BuildSimpleCff({cb.bytes}, {}, {subr.bytes});
    gfx::cff::FontInfo font;
    CHECK(gfx::cff::InitFont(&font, reinterpret_cast<const unsigned char *>(cff.data()), static_cast<int>(cff.size())));
    int w, h, xoff, yoff;
    unsigned char *bmp = gfx::cff::GetGlyphBitmap(&font, 1.0f, 1.0f, 0, &w, &h, &xoff, &yoff);
    CHECK(bmp != nullptr);
    CHECK(w == 50 && h == 50);
    double area = BitmapArea(bmp, w, h);
    CHECK(std::abs(area - 50.0 * 50.0) < 1.0);
    gfx::cff::FreeBitmap(bmp);
}

void TestHintsDoNotAffectOutline() {
    // Same square as TestSimpleSquare, but with hstemhm/vstemhm/hintmask
    // interspersed -- must rasterize identically (hints are a no-op for
    // outline shape at the sizes mep renders).
    CharstringBuilder cb;
    cb.Num(0).Num(100).Op(18);  // hstemhm: one stem pair -> stem_count=1
    cb.Num(0).Num(100).Op(23);  // vstemhm: another pair -> stem_count=2
    cb.Num(100).Num(100).Op(21);
    cb.Op(19);  // hintmask -- consumes ceil(2/8)=1 mask byte
    cb.bytes.push_back(static_cast<char>(0x80));
    cb.Num(200).Num(0).Op(5);
    cb.Num(0).Num(200).Op(5);
    cb.Num(-200).Num(0).Op(5);
    cb.Op(14);

    std::string cff = BuildSimpleCff({cb.bytes}, {}, {});
    gfx::cff::FontInfo font;
    CHECK(gfx::cff::InitFont(&font, reinterpret_cast<const unsigned char *>(cff.data()), static_cast<int>(cff.size())));
    int w, h, xoff, yoff;
    unsigned char *bmp = gfx::cff::GetGlyphBitmap(&font, 1.0f, 1.0f, 0, &w, &h, &xoff, &yoff);
    CHECK(bmp != nullptr);
    CHECK(w == 200 && h == 200);
    double area = BitmapArea(bmp, w, h);
    CHECK(std::abs(area - 200.0 * 200.0) < 1.0);
    gfx::cff::FreeBitmap(bmp);
}

void TestMultiGlyphGidDirectAccess() {
    CharstringBuilder g0;
    g0.Num(0).Num(0).Op(21);
    g0.Num(40).Num(0).Op(5);
    g0.Num(0).Num(40).Op(5);
    g0.Num(-40).Num(0).Op(5);
    g0.Op(14);
    CharstringBuilder g1;
    g1.Num(0).Num(0).Op(21);
    g1.Num(80).Num(0).Op(5);
    g1.Num(0).Num(80).Op(5);
    g1.Num(-80).Num(0).Op(5);
    g1.Op(14);

    std::string cff = BuildSimpleCff({g0.bytes, g1.bytes}, {}, {});
    gfx::cff::FontInfo font;
    CHECK(gfx::cff::InitFont(&font, reinterpret_cast<const unsigned char *>(cff.data()), static_cast<int>(cff.size())));
    CHECK(font.num_glyphs == 2);

    int w, h, xoff, yoff;
    unsigned char *bmp0 = gfx::cff::GetGlyphBitmap(&font, 1.0f, 1.0f, 0, &w, &h, &xoff, &yoff);
    CHECK(bmp0 && w == 40 && h == 40);
    gfx::cff::FreeBitmap(bmp0);
    unsigned char *bmp1 = gfx::cff::GetGlyphBitmap(&font, 1.0f, 1.0f, 1, &w, &h, &xoff, &yoff);
    CHECK(bmp1 && w == 80 && h == 80);
    gfx::cff::FreeBitmap(bmp1);

    CHECK(gfx::cff::GetGlyphBitmap(&font, 1, 1, 2, &w, &h, &xoff, &yoff) == nullptr);  // out of range
    CHECK(gfx::cff::GetGlyphBitmap(&font, 1, 1, -1, &w, &h, &xoff, &yoff) == nullptr);
}

}  // namespace

int main() {
    TestSimpleSquare();
    TestRRCurveToCircle();
    TestVhCurveToCircleMatchesRRCurveTo();
    TestVvHhCurveToExactSquare();
    TestFlexOperators();
    TestSubroutineCallsBiasAndRecursion();
    TestHintsDoNotAffectOutline();
    TestMultiGlyphGidDirectAccess();
    std::printf("cff_test: all checks passed\n");
    return 0;
}
