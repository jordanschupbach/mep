// ANIMATION_VIDEO_PLAN.md Phase 2 coverage for jpeg_codec.h's new
// Encode(): the critical correctness gate for the whole Motion-JPEG
// `.mov` pipeline downstream (Phases 3-6) is that mep's own encoder
// produces bitstreams mep's own (pre-existing, already-tested) Decode()
// reads back correctly -- so every check here round-trips through both
// halves rather than inspecting encoded bytes directly. Opt-in sample
// dump (pass a directory as argv[1]) so an encoded frame can be visually
// inspected via the Read tool, matching mep-image-procgen-test's own
// shape.

#include "jpeg_codec.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// Average per-channel absolute difference between two same-sized RGBA8
// buffers -- a simple, easy-to-reason-about stand-in for PSNR (lower is
// closer; 0 is byte-identical). JPEG is lossy, so round-tripped images
// are never expected to hit 0, only to stay within a threshold sane for
// the content and quality level.
double MeanAbsError(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b, int width, int height) {
    CHECK(a.size() == b.size());
    double sum = 0.0;
    size_t n = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    for (size_t i = 0; i < n; i++) sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    return sum / static_cast<double>(n);
}

// Encodes `rgba` (comp=4, stride=width*4) at `quality`, decodes the
// result back via jpeg::Decode, and returns the reconstructed RGBA8
// buffer -- the shared round-trip step every check below builds on.
std::vector<unsigned char> RoundTrip(const std::vector<unsigned char> &rgba, int width, int height, int quality,
                                      std::string *out_encoded) {
    std::string encoded = jpeg::Encode(width, height, 4, rgba.data(), width * 4, quality);
    CHECK(!encoded.empty());
    if (out_encoded) *out_encoded = encoded;
    int dw = 0, dh = 0;
    std::string error;
    unsigned char *decoded = jpeg::Decode(reinterpret_cast<const unsigned char *>(encoded.data()), encoded.size(), &dw, &dh, &error);
    CHECK(decoded != nullptr);
    CHECK(dw == width);
    CHECK(dh == height);
    std::vector<unsigned char> out(decoded, decoded + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    std::free(decoded);
    return out;
}

std::vector<unsigned char> SolidColor(int width, int height, unsigned char r, unsigned char g, unsigned char b) {
    std::vector<unsigned char> px(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i + 0] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = 255;
    }
    return px;
}

std::vector<unsigned char> Gradient(int width, int height) {
    std::vector<unsigned char> px(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
            px[i + 0] = static_cast<unsigned char>(x * 255 / (width > 1 ? width - 1 : 1));
            px[i + 1] = static_cast<unsigned char>(y * 255 / (height > 1 ? height - 1 : 1));
            px[i + 2] = 128;
            px[i + 3] = 255;
        }
    }
    return px;
}

// Exact-alternating checkerboard at maximum contrast -- the pathological
// input ClampToCategory (jpeg_codec.cpp) exists for: an idealized 1-pixel
// checkerboard's DCT coefficients can exceed the standard AC table's
// category-10 range at quality=100 (quant step 1). This exercises that
// clamp path rather than assuming it's dead code.
std::vector<unsigned char> Checkerboard(int width, int height) {
    std::vector<unsigned char> px(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
            unsigned char v = ((x + y) % 2 == 0) ? 0 : 255;
            px[i + 0] = px[i + 1] = px[i + 2] = v;
            px[i + 3] = 255;
        }
    }
    return px;
}

void WriteFile(const std::string &path, const std::string &data) {
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    CHECK(out.good());
}
}  // namespace

int main(int argc, char **argv) {
    const bool dump_samples = argc > 1;
    const std::string out_dir = dump_samples ? argv[1] : "";

    // -- Solid color, exact multiple of 8: near-lossless at high quality
    // (a flat block's only nonzero coefficient is DC, so quantization
    // error is tiny regardless of chroma). --
    {
        auto src = SolidColor(64, 64, 200, 60, 30);
        auto got = RoundTrip(src, 64, 64, 90, nullptr);
        CHECK(MeanAbsError(src, got, 64, 64) < 3.0);
    }

    // -- Grayscale (comp=1) round trip. --
    {
        int w = 40, h = 40;
        std::vector<unsigned char> gray(static_cast<size_t>(w) * static_cast<size_t>(h));
        for (size_t i = 0; i < gray.size(); i++) gray[i] = static_cast<unsigned char>(i % 256);
        std::string encoded = jpeg::Encode(w, h, 1, gray.data(), w, 85);
        CHECK(!encoded.empty());
        int dw = 0, dh = 0;
        std::string error;
        unsigned char *decoded = jpeg::Decode(reinterpret_cast<const unsigned char *>(encoded.data()), encoded.size(), &dw, &dh, &error);
        CHECK(decoded != nullptr);
        CHECK(dw == w && dh == h);
        double sum = 0.0;
        for (int i = 0; i < w * h; i++) {
            int r = decoded[i * 4 + 0], gexp = static_cast<int>(gray[static_cast<size_t>(i)]);
            sum += std::abs(r - gexp);
            CHECK(decoded[i * 4 + 0] == decoded[i * 4 + 1] && decoded[i * 4 + 1] == decoded[i * 4 + 2]);  // still gray
            CHECK(decoded[i * 4 + 3] == 255);
        }
        CHECK(sum / (w * h) < 5.0);
        std::free(decoded);
    }

    // -- RGBA input (comp=4): alpha channel is silently dropped, not
    // reflected in the color result, and decode always reports alpha
    // 255 (JPEG has no alpha channel at all). --
    {
        int w = 16, h = 16;
        std::vector<unsigned char> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i + 0] = 10;
            rgba[i + 1] = 220;
            rgba[i + 2] = 40;
            rgba[i + 3] = 0;  // fully "transparent" in the source -- must not affect color encoding
        }
        auto got = RoundTrip(rgba, w, h, 90, nullptr);
        for (size_t i = 0; i < got.size(); i += 4) CHECK(got[i + 3] == 255);
        // The alpha channel alone contributes |0-255|/4 == ~64 to this
        // per-channel average (source alpha is 0, decode always reports
        // 255) -- the real thing under test is that the *color* channels
        // stayed near-lossless despite the source's alpha=0, so the bound
        // just needs to be comfortably above that fixed ~64 floor, not
        // near 0.
        CHECK(MeanAbsError(rgba, got, w, h) < 70.0);
    }

    // -- Gradient, non-multiple-of-8 dimensions (exercises the
    // clamp-to-edge padding in ExtractBlock/SamplePlane). --
    {
        auto src = Gradient(37, 23);
        auto got = RoundTrip(src, 37, 23, 85, nullptr);
        CHECK(MeanAbsError(src, got, 37, 23) < 12.0);
    }

    // -- Checkerboard at quality=100 (quant step 1): the ClampToCategory
    // stress case. Must not crash, must still decode to the right
    // dimensions, and (since decode's own DecodeBlock hard-rejects any
    // DC category >11) the very fact this decodes at all confirms every
    // emitted symbol stayed within the standard tables' range. --
    {
        auto src = Checkerboard(48, 48);
        std::string encoded;
        auto got = RoundTrip(src, 48, 48, 100, &encoded);
        CHECK(got.size() == src.size());
        if (dump_samples) WriteFile(out_dir + "/checkerboard_q100.jpg", encoded);
    }

    // -- Determinism: identical input/quality -> byte-identical output. --
    {
        auto src = Gradient(32, 32);
        std::string a = jpeg::Encode(32, 32, 4, src.data(), 32 * 4, 80);
        std::string b = jpeg::Encode(32, 32, 4, src.data(), 32 * 4, 80);
        CHECK(a == b);
    }

    // -- Invalid inputs: empty string, not a crash. --
    {
        std::vector<unsigned char> px(16);
        CHECK(jpeg::Encode(0, 4, 4, px.data(), 16, 80).empty());
        CHECK(jpeg::Encode(4, 0, 4, px.data(), 16, 80).empty());
        CHECK(jpeg::Encode(4, 4, 2, px.data(), 16, 80).empty());  // gray+alpha: no JPEG equivalent
    }

    if (dump_samples) {
        auto src = Gradient(128, 128);
        std::string encoded = jpeg::Encode(128, 128, 4, src.data(), 128 * 4, 85);
        CHECK(!encoded.empty());
        WriteFile(out_dir + "/gradient_q85.jpg", encoded);
    }

    std::printf("jpeg_encoder_test: all checks passed\n");
    return 0;
}
