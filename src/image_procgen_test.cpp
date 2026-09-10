// CHESS_SET_BENCHMARK_PLAN.md Phase 3 coverage for image_procgen.h: pure
// value/checks (determinism, buffer sizing, gradient endpoints, "actually
// varies" sanity on the noise-driven patterns) plus an opt-in
// sample-image dump (pass a directory as argv[1]) so the patterns can be
// visually inspected via the Read tool -- matching mep-model3d-doc-test's
// own "windowless, pure-CPU, no live GL context needed" shape, since
// image_procgen.cpp touches only std::vector<unsigned char> buffers. The
// dump is opt-in (no argv[1] -> checks only, no files written) so `just
// test`'s no-arg invocation doesn't litter the working directory.

#include "image_procgen.h"
#include "png_codec.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

void WritePng(const std::string &path, int width, int height, const std::vector<unsigned char> &pixels) {
    std::string encoded = png::Encode(width, height, 4, pixels.data(), width * 4);
    CHECK(!encoded.empty());
    std::ofstream out(path, std::ios::binary);
    out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    CHECK(out.good());
}

// Red-channel min/max across the buffer -- a cheap "did this actually
// produce variation, not a flat fill" sanity check for the noise-driven
// generators (wood/marble/noise all sweep between two visibly different
// colors, so a near-frozen range means the generator regressed to a
// constant).
void RedChannelRange(const std::vector<unsigned char> &pixels, unsigned char *out_min, unsigned char *out_max) {
    unsigned char lo = 255, hi = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        lo = std::min(lo, pixels[i]);
        hi = std::max(hi, pixels[i]);
    }
    *out_min = lo;
    *out_max = hi;
}
}  // namespace

int main(int argc, char **argv) {
    const bool dump_samples = argc > 1;
    const std::string out_dir = dump_samples ? argv[1] : "";
    const int w = 128, h = 128;
    const procgen::Rgba8 dark{40, 25, 10, 255};
    const procgen::Rgba8 light{210, 180, 140, 255};

    // -- Determinism: same seed/params -> byte-identical output. --
    std::vector<unsigned char> noise_a, noise_b, noise_c;
    procgen::FillNoise(&noise_a, w, h, dark, light, 0.08f, 4, 42);
    procgen::FillNoise(&noise_b, w, h, dark, light, 0.08f, 4, 42);
    CHECK(noise_a == noise_b);
    procgen::FillNoise(&noise_c, w, h, dark, light, 0.08f, 4, 43);
    CHECK(noise_a != noise_c);  // different seed -> (overwhelmingly likely) different output
    CHECK(noise_a.size() == static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

    // -- Fbm2D is itself deterministic per (x, y, seed). --
    CHECK(procgen::Fbm2D(3.7f, 1.2f, 7, 4) == procgen::Fbm2D(3.7f, 1.2f, 7, 4));

    // -- Linear gradient: angle 0 (left -> right) should land color_a at
    // the left edge and color_b (lighter) toward the right edge. --
    std::vector<unsigned char> lin;
    procgen::FillLinearGradient(&lin, w, h, dark, light, 0.0f);
    // Pixel centers sit half a pixel in from each edge, so the corner
    // pixel is close to (not exactly) color_a -- allow a small tolerance
    // rather than requiring exact equality.
    CHECK(std::abs(static_cast<int>(lin[0]) - static_cast<int>(dark.r)) <= 3);  // pixel (0, 0), red channel
    size_t right_edge = (static_cast<size_t>(h / 2) * static_cast<size_t>(w) + static_cast<size_t>(w - 1)) * 4;
    CHECK(lin[right_edge] > lin[0]);

    // -- Radial gradient: center should read closer to color_a (dark)
    // than a corner, since radius grows outward from the buffer center. --
    std::vector<unsigned char> rad;
    procgen::FillRadialGradient(&rad, w, h, dark, light);
    size_t center = (static_cast<size_t>(h / 2) * static_cast<size_t>(w) + static_cast<size_t>(w / 2)) * 4;
    size_t corner = 0;
    CHECK(rad[center] < rad[corner]);

    // -- Wood / marble: confirm they actually vary (not a flat fill). --
    std::vector<unsigned char> wood;
    procgen::FillWood(&wood, w, h, dark, light, 0.35f, 6.0f, 11);
    unsigned char wood_lo, wood_hi;
    RedChannelRange(wood, &wood_lo, &wood_hi);
    CHECK(static_cast<int>(wood_hi) - static_cast<int>(wood_lo) > 50);

    std::vector<unsigned char> marble;
    procgen::FillMarble(&marble, w, h, dark, light, 0.03f, 6.0f, 5);
    unsigned char marble_lo, marble_hi;
    RedChannelRange(marble, &marble_lo, &marble_hi);
    CHECK(static_cast<int>(marble_hi) - static_cast<int>(marble_lo) > 50);

    // -- Wood-turned: varies, and (unlike FillWood) is seamless across the
    // u=0/u=1 wrap -- every row's first and last pixel should be close in
    // color, since a lathe UV-wraps this texture circumferentially and a
    // visible seam is exactly the bug this generator exists to avoid.
    std::vector<unsigned char> wood_turned;
    procgen::FillWoodTurned(&wood_turned, w, h, dark, light, 4.0f, 3.0f, 11);
    unsigned char wt_lo, wt_hi;
    RedChannelRange(wood_turned, &wt_lo, &wt_hi);
    CHECK(static_cast<int>(wt_hi) - static_cast<int>(wt_lo) > 50);
    long long seam_delta_sum = 0;
    for (int y = 0; y < h; ++y) {
        size_t left = (static_cast<size_t>(y) * static_cast<size_t>(w) + 0) * 4;
        size_t right = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(w - 1)) * 4;
        seam_delta_sum += std::abs(static_cast<int>(wood_turned[left]) - static_cast<int>(wood_turned[right]));
    }
    // Average left/right delta well under the full color range -- a
    // continuous wrap, not a hard seam (which would read close to the full
    // dark-to-light swing on most rows).
    CHECK(seam_delta_sum / h < 40);

    // -- Checkerboard: 8x8 on a 128x128 buffer means 16px tiles; corner
    // tile (0,0) is color_a, its neighbor at (20,0) (still tile column 1)
    // is color_b, and tiling is periodic (tile (0,0) matches tile (2,2)).
    std::vector<unsigned char> checker;
    procgen::FillCheckerboard(&checker, w, h, dark, light, 8, 8);
    auto checker_px = [&](int x, int y) {
        size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
        return checker[i];
    };
    CHECK(checker_px(2, 2) == dark.r);
    CHECK(checker_px(20, 2) == light.r);
    CHECK(checker_px(2, 2) == checker_px(34, 34));  // two tiles over = same color

    // -- Box blur: determinism, and it must actually reduce local
    // variance (compare adjacent-pixel red-channel deltas before/after). --
    std::vector<unsigned char> blurred = noise_a;
    procgen::BoxBlur(&blurred, w, h, 4);
    CHECK(blurred.size() == noise_a.size());
    std::vector<unsigned char> blurred_again = noise_a;
    procgen::BoxBlur(&blurred_again, w, h, 4);
    CHECK(blurred == blurred_again);
    long long raw_delta_sum = 0, blurred_delta_sum = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 1; x < w; ++x) {
            size_t i0 = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x - 1)) * 4;
            size_t i1 = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            raw_delta_sum += std::abs(static_cast<int>(noise_a[i0]) - static_cast<int>(noise_a[i1]));
            blurred_delta_sum += std::abs(static_cast<int>(blurred[i0]) - static_cast<int>(blurred[i1]));
        }
    }
    CHECK(blurred_delta_sum < raw_delta_sum);

    // -- Dump sample images for visual inspection, only when a directory
    // was explicitly requested (see file-header comment). --
    if (dump_samples) {
        WritePng(out_dir + "/procgen_gradient_linear.png", w, h, lin);
        WritePng(out_dir + "/procgen_gradient_radial.png", w, h, rad);
        WritePng(out_dir + "/procgen_noise.png", w, h, noise_a);
        WritePng(out_dir + "/procgen_wood.png", w, h, wood);
        WritePng(out_dir + "/procgen_wood_turned.png", w, h, wood_turned);
        WritePng(out_dir + "/procgen_marble.png", w, h, marble);
        WritePng(out_dir + "/procgen_checkerboard.png", w, h, checker);
        WritePng(out_dir + "/procgen_blurred_noise.png", w, h, blurred);
        std::printf("image_procgen_test: all checks passed, samples written to %s\n", out_dir.c_str());
    } else {
        std::printf("image_procgen_test: all checks passed\n");
    }
    return 0;
}
