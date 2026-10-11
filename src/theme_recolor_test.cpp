// theme_recolor.h: the two properties the PDF/image "night mode"
// recolour has to satisfy, and a third that catches the mistake it was
// written to fix.
//
// These are pure numeric invariants with no visible failure mode short
// of someone noticing that dark-theme text looks wrong, which is exactly
// the kind of thing that rots silently -- and the code used to live in
// main.cpp, which has no test and which `just lint-tidy` cannot analyse.

#include "theme_recolor.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_checks = 0;

// Plain `if`, never assert: this project defaults to Release, where
// NDEBUG compiles assert's side effects away along with the check (see
// agent_rpc_test.cpp's own note on the bug that caused). The message
// goes to stderr, not stdout, for the same reason the other tests here
// do it: stdout is fully buffered when the runner pipes it, and abort()
// does not flush, so a printf'd failure prints nothing at all.
void Check(bool condition, const char *expression, int line) {
    ++g_checks;
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

struct Rgb {
    unsigned char r, g, b;
};

// Pure black text on pure white paper: the default light theme.
constexpr Rgb kLightFg{0, 0, 0};
constexpr Rgb kLightBg{255, 255, 255};
// A representative dark theme.
constexpr Rgb kDarkFg{0xc0, 0xca, 0xf5};
constexpr Rgb kDarkBg{0x1a, 0x1b, 0x26};

theme::Ramp MakeRamp(Rgb fg, Rgb bg) { return theme::Ramp::Make(fg.r, fg.g, fg.b, bg.r, bg.g, bg.b); }

// Property 1: a light theme is a no-op.
//
// Black-on-white has to map every grey to itself exactly. It is not
// automatic -- the coverage index is quantized in linear light while the
// output is gamma-encoded, and near black the sRGB curve is steep enough
// that a 256-entry ramp was off by up to 6 levels here.
void TestLightThemeIsIdentity() {
    const theme::Ramp ramp = MakeRamp(kLightFg, kLightBg);
    for (int v = 0; v < 256; ++v) {
        const unsigned char c = static_cast<unsigned char>(v);
        const int idx = theme::CoverageIndex(c, c, c);
        CHECK(ramp.r[static_cast<size_t>(idx)] == c);
        CHECK(ramp.g[static_cast<size_t>(idx)] == c);
        CHECK(ramp.b[static_cast<size_t>(idx)] == c);
    }
}

// Property 2: a recolour preserves how heavy the text looks.
//
// "Perceived ink" is the mean effective coverage in linear light: for
// each pixel, how far its luminance sits from the paper's, as a fraction
// of the full paper-to-ink span. A recolour that preserves it renders
// stems and fraction bars at the same weight; the old gamma-ignoring
// lerp lost 31.7% of it on a real page of equations.
//
// The synthetic page here is a ramp of coverages rather than a real
// raster, which exercises the whole range rather than whatever a
// particular page happened to contain.
double PerceivedInk(const std::vector<unsigned char> &grey, Rgb fg, Rgb bg) {
    const theme::Ramp ramp = MakeRamp(fg, bg);
    const float l_fg = theme::RelativeLuminance(fg.r, fg.g, fg.b);
    const float l_bg = theme::RelativeLuminance(bg.r, bg.g, bg.b);
    CHECK(l_bg != l_fg);
    double total = 0;
    for (unsigned char v : grey) {
        const int idx = theme::CoverageIndex(v, v, v);
        const size_t k = static_cast<size_t>(idx);
        const float lum = theme::RelativeLuminance(ramp.r[k], ramp.g[k], ramp.b[k]);
        total += static_cast<double>((l_bg - lum) / (l_bg - l_fg));
    }
    return total / static_cast<double>(grey.size());
}

void TestRecolourPreservesPerceivedInk() {
    std::vector<unsigned char> page;
    for (int v = 0; v < 256; ++v) page.push_back(static_cast<unsigned char>(v));

    // What the page perceptually shows as rendered: white paper, black
    // ink, so coverage is one minus the linear luminance.
    double reference = 0;
    for (unsigned char v : page) reference += 1.0 - static_cast<double>(theme::RelativeLuminance(v, v, v));
    reference /= static_cast<double>(page.size());

    const double dark = PerceivedInk(page, kDarkFg, kDarkBg);
    const double light = PerceivedInk(page, kLightFg, kLightBg);
    // Within a quarter of a percent; the residual is 8-bit rounding.
    CHECK(dark > reference * 0.9975 && dark < reference * 1.0025);
    CHECK(light > reference * 0.9975 && light < reference * 1.0025);
}

// Property 3: the ramp hits its endpoints exactly and runs monotonically
// between them. An off-by-one at either end would tint the paper or cap
// the text contrast; a non-monotonic ramp would band.
void TestRampEndpointsAndMonotonicity() {
    for (const auto &pair : {std::pair<Rgb, Rgb>{kDarkFg, kDarkBg}, std::pair<Rgb, Rgb>{kLightFg, kLightBg}}) {
        const theme::Ramp ramp = MakeRamp(pair.first, pair.second);
        CHECK(ramp.r[0] == pair.second.r);
        CHECK(ramp.g[0] == pair.second.g);
        CHECK(ramp.b[0] == pair.second.b);
        const size_t last = theme::kRampSize - 1;
        CHECK(ramp.r[last] == pair.first.r);
        CHECK(ramp.g[last] == pair.first.g);
        CHECK(ramp.b[last] == pair.first.b);
        for (size_t i = 1; i < theme::kRampSize; ++i) {
            const bool up = pair.first.r >= pair.second.r;
            CHECK(up ? ramp.r[i] >= ramp.r[i - 1] : ramp.r[i] <= ramp.r[i - 1]);
        }
    }
}

// A half-covered pixel must come back perceptually halfway between the
// two theme colours, not arithmetically halfway. This is the single
// number the old code got wrong, and the one that made glyph edges --
// which is most of what small text is -- land in the wrong place.
void TestMidCoverageIsPerceptuallyMidway() {
    const theme::Ramp ramp = MakeRamp(kDarkFg, kDarkBg);
    const size_t mid = theme::kRampSize / 2;
    const float l_fg = theme::RelativeLuminance(kDarkFg.r, kDarkFg.g, kDarkFg.b);
    const float l_bg = theme::RelativeLuminance(kDarkBg.r, kDarkBg.g, kDarkBg.b);
    const float got = theme::RelativeLuminance(ramp.r[mid], ramp.g[mid], ramp.b[mid]);
    const float want = l_bg + (l_fg - l_bg) * 0.5f;
    CHECK(got > want * 0.99f && got < want * 1.01f);
    // ...and it is NOT the arithmetic midpoint of the encoded channels,
    // which is what a plain lerp would have produced.
    const int arithmetic = (static_cast<int>(kDarkFg.g) + static_cast<int>(kDarkBg.g)) / 2;
    CHECK(std::abs(static_cast<int>(ramp.g[mid]) - arithmetic) > 10);
}

// Round-tripping every byte through the two transfer functions must come
// back where it started, or every value in the ramp is slightly off.
void TestSrgbRoundTrip() {
    const std::array<float, 256> &lut = theme::SrgbToLinearTable();
    for (int v = 0; v < 256; ++v) CHECK(theme::LinearToSrgbByte(lut[static_cast<size_t>(v)]) == v);
    CHECK(lut[0] == 0.0f);
    CHECK(lut[255] > 0.999f && lut[255] <= 1.0f);
}

}  // namespace

int main() {
    TestSrgbRoundTrip();
    TestLightThemeIsIdentity();
    TestRampEndpointsAndMonotonicity();
    TestMidCoverageIsPerceptuallyMidway();
    TestRecolourPreservesPerceivedInk();
    std::printf("theme_recolor_test: all %d checks passed\n", g_checks);
    return 0;
}
