#pragma once

// mep's own in-house 2D scanline polygon rasterizer -- see
// PDFIUM_REMOVAL_PLAN.md Phase 6. Factored out of gfx/truetype.cpp's
// glyph-fill code (STB_TRUETYPE_REMOVAL_PLAN.md Phase 4's own writeup
// covers the algorithm choice: horizontal coverage computed exactly from
// real edge x-intersections, vertical antialiasing via a fixed 4x
// supersample per pixel row) so PDF content-stream path filling (Phase 7
// onward) can reuse the exact same tested rasterizer instead of a second
// implementation. truetype.cpp itself is unchanged in behavior -- this
// is a pure extraction, verified by exact-output comparison before any
// new caller (or new capability below) is added.
//
// Two additions on top of what glyph fill alone needed: an even-odd
// winding rule (PDF's `f*`/`W*` path-painting operators, vs. `f`/`W`'s
// nonzero rule -- TrueType glyph outlines are always nonzero-wound, so
// truetype.cpp itself never needed this), and cubic Bezier flattening
// (PDF's `c`/`v`/`y` path-construction operators, and CFF/Type2
// charstrings' own curve type -- TrueType only needed quadratic).

#include <vector>

namespace gfx {
namespace raster {

enum class FillRule { kNonZero, kEvenOdd };

// One polygon edge, already in the rasterizer's own pixel/raster space
// (y increasing downward, matching how GetCodepointBitmap and PDF page
// rendering both orient their output buffers). `winding` is +1 if the
// original segment's y increased (went downward), -1 if it decreased --
// used by the nonzero fill rule; ignored (every crossing just toggles
// in/out) under the even-odd rule.
struct Edge {
    float x_at_ymin = 0, dxdy = 0;
    float ymin = 0, ymax = 0;
    int winding = 0;
};

// Appends one line segment as an edge, in raster space. A no-op for a
// horizontal segment (y0 == y1), which never crosses a scanline.
void AddLine(std::vector<Edge> &edges, float x0, float y0, float x1, float y1);

// Flattens one quadratic Bezier (p0 on-curve, c control, p1 on-curve --
// TrueType's own curve type) into line-segment edges via recursive
// subdivision, stopping once the control point's deviation from the
// p0-p1 chord is below a fixed ~0.3px tolerance (or a depth guard is
// hit). All points already in raster space.
void FlattenQuadratic(std::vector<Edge> &edges, float x0, float y0, float cx, float cy, float x1, float y1);

// Flattens one cubic Bezier (p0 on-curve, c1/c2 control, p1 on-curve --
// PDF's `c`/`v`/`y` operators and CFF/Type2 charstrings' curve type)
// into line-segment edges via De Casteljau subdivision, same tolerance/
// depth-guard bar as FlattenQuadratic (deviation measured from both
// control points to the p0-p1 chord, not just one, since a cubic has
// two). All points already in raster space.
void FlattenCubic(std::vector<Edge> &edges, float x0, float y0, float c1x, float c1y, float c2x, float c2y, float x1,
                   float y1);

// Rasterizes `edges` (already in raster space) into a `width`x`height`
// single-channel (0-255) antialiased coverage buffer, using `rule` to
// decide which spans between edge crossings count as "inside". Returns
// an all-zero buffer (not a null/empty vector) for an empty edge list or
// a non-positive width/height, so callers can always index it safely.
std::vector<unsigned char> Rasterize(std::vector<Edge> &edges, int width, int height,
                                      FillRule rule = FillRule::kNonZero);

// A cubic-Bezier outline contour in font units (y-up): the shared output
// shape of the CFF/Type 2 (gfx/cff.cpp) and Type 1 (gfx/type1.cpp)
// charstring interpreters, so both feed the one RasterizeOutline below
// instead of each carrying its own bounding-box/edge-list/malloc
// boilerplate.
struct OutlineSegment {
    bool is_curve = false;
    float x = 0, y = 0;                         // end point
    float c1x = 0, c1y = 0, c2x = 0, c2y = 0;   // cubic control points (is_curve only)
};
struct OutlineContour {
    float start_x = 0, start_y = 0;
    std::vector<OutlineSegment> segments;  // implicitly closed back to (start_x, start_y)
};

// Rasterizes `contours` through the 2x2 matrix [a b; c d] (font units ->
// device pixels, PDF's own row-vector convention: rx = a*x + c*y,
// ry = b*x + d*y). A y-down device space is expressed by the caller
// through the matrix itself (a plain "scale by sx/sy" is {sx, 0, 0,
// -sy}), which is what lets one function serve upright, rotated and
// skewed text alike. Returns a malloc'd width*height coverage bitmap
// (caller std::free()s it), or nullptr for an empty/degenerate outline;
// *xoff/*yoff are the bitmap's top-left offset from the glyph origin in
// device pixels (the same contract as gfx::tt::GetGlyphBitmap).
unsigned char *RasterizeOutline(const std::vector<OutlineContour> &contours, float a, float b, float c, float d,
                                int *width, int *height, int *xoff, int *yoff);

}  // namespace raster
}  // namespace gfx
