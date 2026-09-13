// PDFIUM_REMOVAL_PLAN.md Phase 6 coverage for gfx/rasterizer.h/.cpp's
// two additions beyond the pure extraction from truetype.cpp (which
// itself is verified by live glyph-rendering screenshots, not this
// test, per the plan's own Phase 6 note -- glyph rendering only ever
// exercises the nonzero rule + quadratic flattening, neither of which
// changed): the even-odd fill rule, and cubic Bezier flattening.

#include "gfx/rasterizer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using gfx::raster::AddLine;
using gfx::raster::Edge;
using gfx::raster::FillRule;
using gfx::raster::FlattenCubic;
using gfx::raster::Rasterize;

void AddRectClockwise(std::vector<Edge> &edges, float x0, float y0, float x1, float y1) {
    // top-left -> top-right -> bottom-right -> bottom-left -> back to top-left.
    AddLine(edges, x0, y0, x1, y0);
    AddLine(edges, x1, y0, x1, y1);
    AddLine(edges, x1, y1, x0, y1);
    AddLine(edges, x0, y1, x0, y0);
}

unsigned char CoverageAt(const std::vector<unsigned char> &buf, int width, int x, int y) {
    return buf[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
}

void TestNonZeroFillsOverlapOfSameWindingRects() {
    // Two same-direction (clockwise) rectangles overlapping in [3,6]x[3,6]
    // -- nonzero winding there is 2 (still "inside").
    std::vector<Edge> edges;
    AddRectClockwise(edges, 0, 0, 6, 6);
    AddRectClockwise(edges, 3, 3, 9, 9);
    std::vector<unsigned char> buf = Rasterize(edges, 9, 9, FillRule::kNonZero);
    CHECK(CoverageAt(buf, 9, 1, 1) > 200);  // inside A only
    CHECK(CoverageAt(buf, 9, 7, 7) > 200);  // inside B only
    CHECK(CoverageAt(buf, 9, 4, 4) > 200);  // overlap: winding 2, still filled under nonzero
}

void TestEvenOddPunchesHoleInOverlap() {
    // Same two rectangles, even-odd rule: the overlap is crossed an even
    // number of times (once entering/leaving each rectangle's boundary)
    // -> a hole, not filled.
    std::vector<Edge> edges;
    AddRectClockwise(edges, 0, 0, 6, 6);
    AddRectClockwise(edges, 3, 3, 9, 9);
    std::vector<unsigned char> buf = Rasterize(edges, 9, 9, FillRule::kEvenOdd);
    CHECK(CoverageAt(buf, 9, 1, 1) > 200);  // inside A only: still filled
    CHECK(CoverageAt(buf, 9, 7, 7) > 200);  // inside B only: still filled
    CHECK(CoverageAt(buf, 9, 4, 4) == 0);   // overlap: even crossings -> hole
}

void TestCubicDegenerateStraightLineMatchesRectArea() {
    // Control points collinear with (and between) each edge's endpoints
    // -- a cubic that's geometrically just a straight line -- should
    // flatten to (approximately) the same filled area as a plain
    // rectangle, not something visibly smaller/larger from spurious
    // curvature.
    constexpr float W = 40, H = 20;
    std::vector<Edge> edges;
    FlattenCubic(edges, 0, 0, W / 3, 0, 2 * W / 3, 0, W, 0);
    FlattenCubic(edges, W, 0, W, H / 3, W, 2 * H / 3, W, H);
    FlattenCubic(edges, W, H, 2 * W / 3, H, W / 3, H, 0, H);
    FlattenCubic(edges, 0, H, 0, 2 * H / 3, 0, H / 3, 0, 0);
    std::vector<unsigned char> buf = Rasterize(edges, static_cast<int>(W), static_cast<int>(H));
    double area = 0;
    for (unsigned char c : buf) area += static_cast<double>(c) / 255.0;
    double expected = static_cast<double>(W) * static_cast<double>(H);
    CHECK(std::abs(area - expected) < 1.0);  // within ~1px^2 of exact (antialiasing rounding only)
}

void TestCubicCircleApproximationArea() {
    // A full circle built from 4 cubic arcs using the standard
    // kappa = 4/3*(sqrt(2)-1) control-point-distance constant -- a
    // well-known, independently-verifiable circle approximation (max
    // radial error ~0.027% of radius). Rasterized area should match the
    // analytic pi*r^2 within a few percent (antialiasing + the
    // approximation's own tiny error), a real quantitative check rather
    // than just "didn't crash".
    constexpr float R = 30, PAD = 4;
    constexpr float CX = R + PAD, CY = R + PAD;
    constexpr float K = 0.5522847498f;
    std::vector<Edge> edges;
    // Start at (CX+R, CY), sweep clockwise (raster space, y-down) through
    // 4 quarter-arcs back to the start.
    float x0 = CX + R, y0 = CY;
    struct Pt {
        float x, y;
    };
    Pt points[4] = {{CX, CY + R}, {CX - R, CY}, {CX, CY - R}, {CX + R, CY}};
    Pt ctrl_out[4] = {
        {CX + R, CY + R * K}, {CX - R * K, CY + R}, {CX - R, CY - R * K}, {CX + R * K, CY - R}};
    Pt ctrl_in[4] = {{CX + R * K, CY + R}, {CX - R, CY + R * K}, {CX - R * K, CY - R}, {CX + R, CY - R * K}};
    float cx = x0, cy = y0;
    for (int i = 0; i < 4; ++i) {
        FlattenCubic(edges, cx, cy, ctrl_out[i].x, ctrl_out[i].y, ctrl_in[i].x, ctrl_in[i].y, points[i].x,
                     points[i].y);
        cx = points[i].x;
        cy = points[i].y;
    }
    int dim = static_cast<int>(2 * R + 2 * PAD);
    std::vector<unsigned char> buf = Rasterize(edges, dim, dim);
    double area = 0;
    for (unsigned char c : buf) area += static_cast<double>(c) / 255.0;
    double expected = 3.14159265358979 * static_cast<double>(R) * static_cast<double>(R);
    double rel_error = std::abs(area - expected) / expected;
    CHECK(rel_error < 0.03);  // within 3% of the analytic circle area
}

}  // namespace

int main() {
    TestNonZeroFillsOverlapOfSameWindingRects();
    TestEvenOddPunchesHoleInOverlap();
    TestCubicDegenerateStraightLineMatchesRectArea();
    TestCubicCircleApproximationArea();
    std::printf("rasterizer_test: all checks passed\n");
    return 0;
}
