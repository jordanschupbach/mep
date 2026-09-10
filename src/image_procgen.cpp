#include "image_procgen.h"

#include <algorithm>
#include <cmath>

namespace procgen {

namespace {

constexpr float kPi = 3.14159265358979323846f;

size_t PixelIndex(int width, int x, int y) {
    return (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
}

// Deterministic integer-lattice hash (a standard "multiply, xor-shift,
// multiply, xor-shift" mix -- no cryptographic properties needed, just a
// well-distributed, seed-dependent, allocation-free function of (x, y)).
uint32_t HashLattice(int32_t x, int32_t y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
                 seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    return h;
}

float LatticeValue(int32_t x, int32_t y, uint32_t seed) {
    return static_cast<float>(HashLattice(x, y, seed)) / static_cast<float>(0xFFFFFFFFu);
}

float Smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

unsigned char LerpChannel(unsigned char a, unsigned char b, float t) {
    float clamped_t = std::clamp(t, 0.0f, 1.0f);
    float v = Lerp(static_cast<float>(a), static_cast<float>(b), clamped_t);
    return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0f, 255.0f)));
}

Rgba8 LerpColor(Rgba8 a, Rgba8 b, float t) {
    return Rgba8{LerpChannel(a.r, b.r, t), LerpChannel(a.g, b.g, t), LerpChannel(a.b, b.b, t),
                 LerpChannel(a.a, b.a, t)};
}

void SetPixel(std::vector<unsigned char> *pixels, int width, int x, int y, Rgba8 c) {
    size_t idx = PixelIndex(width, x, y);
    (*pixels)[idx + 0] = c.r;
    (*pixels)[idx + 1] = c.g;
    (*pixels)[idx + 2] = c.b;
    (*pixels)[idx + 3] = c.a;
}

}  // namespace

float ValueNoise2D(float x, float y, uint32_t seed) {
    int32_t x0 = static_cast<int32_t>(std::floor(x));
    int32_t y0 = static_cast<int32_t>(std::floor(y));
    int32_t x1 = x0 + 1;
    int32_t y1 = y0 + 1;
    float tx = Smoothstep(x - static_cast<float>(x0));
    float ty = Smoothstep(y - static_cast<float>(y0));
    float v00 = LatticeValue(x0, y0, seed);
    float v10 = LatticeValue(x1, y0, seed);
    float v01 = LatticeValue(x0, y1, seed);
    float v11 = LatticeValue(x1, y1, seed);
    float vx0 = Lerp(v00, v10, tx);
    float vx1 = Lerp(v01, v11, tx);
    return Lerp(vx0, vx1, ty);
}

float Fbm2D(float x, float y, uint32_t seed, int octaves) {
    float sum = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    float max_sum = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        uint32_t octave_seed = seed + static_cast<uint32_t>(i) * 101u;
        sum += ValueNoise2D(x * frequency, y * frequency, octave_seed) * amplitude;
        max_sum += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return max_sum > 0.0f ? sum / max_sum : 0.0f;
}

void FillLinearGradient(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b,
                         float angle_degrees) {
    if (width <= 0 || height <= 0) return;
    pixels->resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    float rad = angle_degrees * (kPi / 180.0f);
    float dx = std::cos(rad);
    float dy = std::sin(rad);
    float corners_x[4] = {0.0f, static_cast<float>(width), 0.0f, static_cast<float>(width)};
    float corners_y[4] = {0.0f, 0.0f, static_cast<float>(height), static_cast<float>(height)};
    float min_proj = corners_x[0] * dx + corners_y[0] * dy;
    float max_proj = min_proj;
    for (int i = 1; i < 4; ++i) {
        float proj = corners_x[i] * dx + corners_y[i] * dy;
        min_proj = std::min(min_proj, proj);
        max_proj = std::max(max_proj, proj);
    }
    float range = max_proj - min_proj;
    if (range < 1e-6f) range = 1.0f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float proj = (static_cast<float>(x) + 0.5f) * dx + (static_cast<float>(y) + 0.5f) * dy;
            float t = (proj - min_proj) / range;
            SetPixel(pixels, width, x, y, LerpColor(color_a, color_b, t));
        }
    }
}

void FillRadialGradient(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b) {
    if (width <= 0 || height <= 0) return;
    pixels->resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    float cx = static_cast<float>(width) * 0.5f;
    float cy = static_cast<float>(height) * 0.5f;
    float radius = static_cast<float>(std::min(width, height)) * 0.5f;
    if (radius < 1e-6f) radius = 1.0f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float ddx = (static_cast<float>(x) + 0.5f) - cx;
            float ddy = (static_cast<float>(y) + 0.5f) - cy;
            float dist = std::sqrt(ddx * ddx + ddy * ddy);
            float t = dist / radius;
            SetPixel(pixels, width, x, y, LerpColor(color_a, color_b, t));
        }
    }
}

void FillNoise(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b, float scale,
               int octaves, uint32_t seed) {
    if (width <= 0 || height <= 0) return;
    pixels->resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float t = Fbm2D(static_cast<float>(x) * scale, static_cast<float>(y) * scale, seed, octaves);
            SetPixel(pixels, width, x, y, LerpColor(color_a, color_b, t));
        }
    }
}

void FillWood(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b,
              float ring_scale, float warp, uint32_t seed) {
    if (width <= 0 || height <= 0) return;
    pixels->resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    float cx = static_cast<float>(width) * 0.5f;
    float cy = static_cast<float>(height) * 0.5f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float ddx = static_cast<float>(x) - cx;
            float ddy = static_cast<float>(y) - cy;
            float dist = std::sqrt(ddx * ddx + ddy * ddy);
            float warp_noise = Fbm2D(static_cast<float>(x) * 0.02f, static_cast<float>(y) * 0.02f, seed, 4) - 0.5f;
            float warped_dist = dist + warp_noise * warp;
            float ring = std::sin(warped_dist * ring_scale) * 0.5f + 0.5f;
            SetPixel(pixels, width, x, y, LerpColor(color_a, color_b, ring));
        }
    }
}

void FillMarble(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b, float scale,
                 float turbulence, uint32_t seed) {
    if (width <= 0 || height <= 0) return;
    pixels->resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float fx = static_cast<float>(x);
            float fy = static_cast<float>(y);
            float turb = Fbm2D(fx * scale, fy * scale, seed, 4);
            float t = std::sin((fx + fy) * scale + turb * turbulence) * 0.5f + 0.5f;
            SetPixel(pixels, width, x, y, LerpColor(color_a, color_b, t));
        }
    }
}

void FillWoodTurned(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b,
                     float ring_scale, float warp, uint32_t seed) {
    if (width <= 0 || height <= 0) return;
    pixels->resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    constexpr float kTwoPi = 2.0f * kPi;
    for (int y = 0; y < height; ++y) {
        float v = static_cast<float>(y) / static_cast<float>(height);
        for (int x = 0; x < width; ++x) {
            float u = static_cast<float>(x) / static_cast<float>(width);
            // Sample on a circle in noise-space so this value is exactly
            // periodic in u (u=0 and u=1 land on the same circle point) --
            // the trick that makes the streak pattern wrap seamlessly.
            float cx = std::cos(u * kTwoPi);
            float cy = std::sin(u * kTwoPi);
            float streak = Fbm2D(cx * 2.5f, cy * 2.5f, seed, 3);
            // Along-length wobble, keyed by the streak's own identity (not
            // by u directly) so it varies per-streak without reintroducing
            // a u-seam.
            float length_wobble = Fbm2D(v * 3.0f, streak * 6.0f + 100.0f, seed + 17u, 2);
            float band = std::sin((streak + length_wobble * warp * 0.15f) * ring_scale * kTwoPi) * 0.5f + 0.5f;
            SetPixel(pixels, width, x, y, LerpColor(color_a, color_b, band));
        }
    }
}

void FillCheckerboard(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b,
                       int squares_x, int squares_y) {
    if (width <= 0 || height <= 0 || squares_x <= 0 || squares_y <= 0) return;
    pixels->resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        int ty = y * squares_y / height;
        for (int x = 0; x < width; ++x) {
            int tx = x * squares_x / width;
            bool light = (tx + ty) % 2 == 0;
            SetPixel(pixels, width, x, y, light ? color_a : color_b);
        }
    }
}

void BoxBlur(std::vector<unsigned char> *pixels, int width, int height, int radius) {
    if (width <= 0 || height <= 0 || radius <= 0) return;
    std::vector<unsigned char> temp(pixels->size());
    int window = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int sum[4] = {0, 0, 0, 0};
            for (int k = -radius; k <= radius; ++k) {
                int sx = std::clamp(x + k, 0, width - 1);
                size_t idx = PixelIndex(width, sx, y);
                for (int c = 0; c < 4; ++c) sum[c] += (*pixels)[idx + static_cast<size_t>(c)];
            }
            size_t didx = PixelIndex(width, x, y);
            for (int c = 0; c < 4; ++c) {
                temp[didx + static_cast<size_t>(c)] = static_cast<unsigned char>(sum[c] / window);
            }
        }
    }
    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) {
            int sum[4] = {0, 0, 0, 0};
            for (int k = -radius; k <= radius; ++k) {
                int sy = std::clamp(y + k, 0, height - 1);
                size_t idx = PixelIndex(width, x, sy);
                for (int c = 0; c < 4; ++c) sum[c] += temp[idx + static_cast<size_t>(c)];
            }
            size_t didx = PixelIndex(width, x, y);
            for (int c = 0; c < 4; ++c) {
                (*pixels)[didx + static_cast<size_t>(c)] = static_cast<unsigned char>(sum[c] / window);
            }
        }
    }
}

}  // namespace procgen
