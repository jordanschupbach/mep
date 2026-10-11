#pragma once

// Recolouring a rendered page (or image) onto the editor's own two
// colours -- the "night mode" a PDF pane applies when
// PdfSession::theme_colors is set, which it is by default.
//
// Lives in its own header because it is pure arithmetic with two
// properties worth asserting, and main.cpp (where it is used, and where
// it used to live) has no test of its own and is the one translation
// unit `just lint-tidy` cannot analyse at all:
//
//   1. A light theme must be a no-op. Black text on white paper has to
//      map to itself exactly, or every light-theme reader sees the
//      recolour they did not ask for.
//   2. A dark theme must preserve how heavy the text looks. A
//      half-covered glyph edge should carry half the light either way.
//
// The second is why this is not the obvious lerp. The original version
// took a Rec.601 luma of the gamma-encoded channel values and
// interpolated the gamma-encoded theme colours by it -- two mistakes
// pointing the same way, landing on exactly the mid-luminance pixels
// that antialiased edges are made of. Measured over a rendered page of
// equations, the dark-theme recolour came out with 31.7% less perceived
// ink than the same page in light theme: stems and fraction bars really
// did render about a third lighter, which is what "dark-theme maths
// looks thin and muddy" was.
//
// So: the source pixel's luminance is taken in linear light, the theme
// colours are interpolated in linear light, and the result is re-encoded
// to sRGB.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace theme {

// sRGB transfer function and its inverse (IEC 61966-2-1).
inline const std::array<float, 256> &SrgbToLinearTable() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const double c = static_cast<double>(i) / 255.0;
            t[static_cast<size_t>(i)] =
                static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
        return t;
    }();
    return table;
}

inline unsigned char LinearToSrgbByte(float lin) {
    const double c = std::clamp(static_cast<double>(lin), 0.0, 1.0);
    const double enc = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return static_cast<unsigned char>(std::lround(enc * 255.0));
}

// Ramp resolution, deliberately far finer than the 8 bits of the pixels
// it maps: the quantization happens in *linear* light while the output
// is gamma-encoded, and near black the sRGB curve has slope 12.92, so
// one step of a 256-entry linear ramp is about 13 output levels there.
// At 256 entries property (1) above failed by up to 6 levels in the
// shadows. 4096 makes it exact for all 256 greys, for 12KB of tables.
constexpr int kRampSize = 4096;

// How dark a pixel is, in linear light, as a ramp index: 0 = the page's
// own paper, kRampSize-1 = fully inked.
//
// Rec.709's coefficients, which are the ones defined for linear-light
// luminance. (Rec.601's 0.299/0.587/0.114 are a luma of gamma-encoded
// values -- a different quantity, which this used as though it were this
// one.)
inline int CoverageIndex(unsigned char r, unsigned char g, unsigned char b) {
    const std::array<float, 256> &lut = SrgbToLinearTable();
    const float lin = 0.2126f * lut[r] + 0.7152f * lut[g] + 0.0722f * lut[b];
    const int idx = static_cast<int>(std::lround((1.0f - lin) * (kRampSize - 1)));
    return std::clamp(idx, 0, kRampSize - 1);
}

// The whole coverage-to-colour mapping for one (fg, bg) pair, built once
// per image rather than per pixel: a page raster can be 16 megapixels at
// the scales rendered_scale now reaches, so the per-pixel work has to
// stay a few table lookups rather than a few pow() calls.
struct Ramp {
    std::array<unsigned char, kRampSize> r{}, g{}, b{};
    // Fast path for a grey pixel, as one 256-entry table of RGB triples.
    //
    // Worth having because a rendered page is essentially all grey:
    // white paper, black text, and the antialiased edges between them --
    // a page of equations measured 100.00% grey. The luminance
    // coefficients sum to 1, so for r == g == b the coverage index
    // depends on that one value alone and the whole mapping collapses to
    // a single lookup, which is what keeps a 16-megapixel page's
    // recolour off the slow path entirely.
    //
    // Measured cost on a real page: 3.7 ms/Mpx here against 2.2 ms/Mpx
    // for the arithmetic lerp this replaced. The gap is not the lookup
    // -- it is that the old version had no data-dependent branch and no
    // table, so the compiler vectorized it and cannot vectorize this.
    // Accepted: +1.5 ms/Mpx is ~3ms for a page at the normal reading
    // scale, it happens once per page per upload rather than per frame,
    // and it buys back the 31.7% ink error described above.
    std::array<unsigned char, 256 * 3> grey{};

    // `fg` is the colour fully-inked pixels become, `bg` the colour the
    // page's own paper becomes.
    static Ramp Make(unsigned char fr8, unsigned char fg8, unsigned char fb8, unsigned char br8, unsigned char bg8,
                     unsigned char bb8) {
        const std::array<float, 256> &lut = SrgbToLinearTable();
        Ramp ramp;
        const float fr = lut[fr8], fg = lut[fg8], fb = lut[fb8];
        const float br = lut[br8], bg = lut[bg8], bb = lut[bb8];
        for (int i = 0; i < kRampSize; ++i) {
            const float a = static_cast<float>(i) / static_cast<float>(kRampSize - 1);
            const size_t k = static_cast<size_t>(i);
            ramp.r[k] = LinearToSrgbByte(br + (fr - br) * a);
            ramp.g[k] = LinearToSrgbByte(bg + (fg - bg) * a);
            ramp.b[k] = LinearToSrgbByte(bb + (fb - bb) * a);
        }
        for (int v = 0; v < 256; ++v) {
            const size_t k = static_cast<size_t>(CoverageIndex(static_cast<unsigned char>(v),
                                                               static_cast<unsigned char>(v),
                                                               static_cast<unsigned char>(v)));
            ramp.grey[static_cast<size_t>(v) * 3 + 0] = ramp.r[k];
            ramp.grey[static_cast<size_t>(v) * 3 + 1] = ramp.g[k];
            ramp.grey[static_cast<size_t>(v) * 3 + 2] = ramp.b[k];
        }
        return ramp;
    }

    // One pixel's recoloured RGB. Takes the grey path when it can, which
    // on a page of text is nearly always.
    void Apply(const unsigned char *src, unsigned char *dst) const {
        if (src[0] == src[1] && src[1] == src[2]) {
            const unsigned char *e = &grey[static_cast<size_t>(src[0]) * 3];
            dst[0] = e[0];
            dst[1] = e[1];
            dst[2] = e[2];
            return;
        }
        const size_t k = static_cast<size_t>(CoverageIndex(src[0], src[1], src[2]));
        dst[0] = r[k];
        dst[1] = g[k];
        dst[2] = b[k];
    }
};

// Relative luminance of an already-encoded colour, in linear light --
// exposed for the tests, which measure "how heavy does this look".
inline float RelativeLuminance(unsigned char r, unsigned char g, unsigned char b) {
    const std::array<float, 256> &lut = SrgbToLinearTable();
    return 0.2126f * lut[r] + 0.7152f * lut[g] + 0.0722f * lut[b];
}

}  // namespace theme
