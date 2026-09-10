#pragma once

// CHESS_SET_BENCHMARK_PLAN.md Phase 3: procedural texture generation for
// the image editor. Pure functions over an RGBA8 pixel buffer (row-major,
// width*height*4 bytes, no row padding -- the exact layout
// ImageEditorLayer::pixels and png::Encode's comp=4 both already use), so
// callers can operate directly on a layer's buffer or a standalone one.
// Deterministic given a seed: same seed + same params always produce
// byte-identical output (no time-based or global RNG state).

#include <cstdint>
#include <vector>

namespace procgen {

struct Rgba8 {
    unsigned char r = 0, g = 0, b = 0, a = 255;
};

// Value noise in [0, 1), deterministic per (x, y, seed) -- hashes the
// integer lattice points around (x, y) and bilinearly interpolates with a
// smoothstep-eased fractional part (Ken Perlin's "quintic" easing wasn't
// felt necessary here, matching this module's whole "cheap but good
// enough for a texture, not a research paper" scope). No allocation, no
// external RNG: safe to call from any of the fill functions below in a
// tight per-pixel loop.
float ValueNoise2D(float x, float y, uint32_t seed);

// Fractal Brownian motion: `octaves` layers of ValueNoise2D at doubling
// frequency and halving amplitude (the standard fBm recipe), normalized
// back into roughly [0, 1]. The shared building block for both the wood
// and marble generators below.
float Fbm2D(float x, float y, uint32_t seed, int octaves);

// Fills the whole buffer with a linear or radial gradient between two
// colors. Linear: `angle_degrees` is the gradient direction (0 = left to
// right). Radial: centered on the buffer, radius = half the smaller
// dimension.
void FillLinearGradient(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b,
                         float angle_degrees);
void FillRadialGradient(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b);

// Fills the whole buffer with raw fBm noise mapped between two colors
// (t = Fbm2D output, lerp(color_a, color_b, t)). `scale` controls the
// noise frequency (higher = finer grain).
void FillNoise(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b, float scale,
               int octaves, uint32_t seed);

// Wood grain: concentric rings around a vertical axis through the buffer
// center, radial distance warped by fBm, banded via sin() into alternating
// color_a/color_b rings. `ring_scale` controls ring spacing (higher =
// tighter rings); `warp` controls how much the fBm distorts ring shape
// (0 = perfect circles). This is a *cross-section* pattern (what a tree's
// end grain looks like face-on) -- correct for a round tabletop or a
// piece's flat cap, but wrapping it around a cylinder's sides via
// angle-mapped U produces a spiral/barber-pole look, not wood grain
// (CHESS_SET_BENCHMARK_PLAN.md Phase 6 found this the hard way on the
// first chess-piece render). Use FillWoodTurned below for anything
// UV-wrapped circumferentially, e.g. a lathe-turned object's sides.
void FillWood(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b,
              float ring_scale, float warp, uint32_t seed);

// Wood grain for a surface UV-wrapped around a cylinder (u = angle/2pi in
// [0,1], v = length in [0,1], the exact convention model3d_doc.cpp's
// AddLatheToScene produces) -- streaks run lengthwise along v (the way
// real wood grain looks on a lathe-turned spindle, since the growth rings
// of the original log run parallel to the lathe's own axis), and the
// streak pattern is seamless across the u=0/u=1 wrap: each streak's
// position comes from sampling Fbm2D at a point on a circle
// (cos(u*2pi), sin(u*2pi)) rather than at u directly, so it's periodic in
// u by construction with no visible seam. `ring_scale` controls how many
// streaks wrap around; `warp` controls how much each streak's shade
// wanders along its own length (0 = perfectly straight streaks).
void FillWoodTurned(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b,
                     float ring_scale, float warp, uint32_t seed);

// Checkerboard: `squares_x` by `squares_y` alternating color_a/color_b
// tiles, color_a in the (0,0) corner tile. For a chess board texture,
// pass 8/8 -- or skip the texture entirely and use real per-tile 3D
// geometry (model.addPrimitive plane x64) for crisper edges; this is the
// texture-based alternative when a single flat plane object is preferred.
void FillCheckerboard(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b,
                       int squares_x, int squares_y);

// Marble: fBm turbulence fed through sin(x + turbulence * strength), the
// standard procedural-marble recipe, mapped between two colors. `scale`
// controls the base vein frequency; `turbulence` controls how much the
// noise distorts the veins (0 = plain sine bands, no marbling).
void FillMarble(std::vector<unsigned char> *pixels, int width, int height, Rgba8 color_a, Rgba8 color_b, float scale,
                 float turbulence, uint32_t seed);

// Separable box blur (horizontal pass then vertical pass), `radius` in
// pixels each side (a radius-1 blur samples 3 pixels per axis). Edges
// clamp to the nearest in-bounds pixel rather than wrapping or padding
// with black, so blurred textures don't darken at their border.
void BoxBlur(std::vector<unsigned char> *pixels, int width, int height, int radius);

}  // namespace procgen
