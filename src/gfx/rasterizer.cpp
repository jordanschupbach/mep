#include "gfx/rasterizer.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace gfx {
namespace raster {

void AddLine(std::vector<Edge> &edges, float x0, float y0, float x1, float y1) {
    if (y0 == y1) return;  // horizontal edges never cross a scanline
    Edge e;
    e.winding = y1 > y0 ? 1 : -1;
    if (y0 < y1) {
        e.ymin = y0;
        e.ymax = y1;
        e.x_at_ymin = x0;
        e.dxdy = (x1 - x0) / (y1 - y0);
    } else {
        e.ymin = y1;
        e.ymax = y0;
        e.x_at_ymin = x1;
        e.dxdy = (x0 - x1) / (y0 - y1);
    }
    edges.push_back(e);
}

namespace {
constexpr float kToleranceSq = 0.09f;  // ~0.3px chord deviation, same bar as this module's own doc comment
constexpr int kMaxDepth = 10;

void FlattenQuadraticRec(std::vector<Edge> &edges, float x0, float y0, float cx, float cy, float x1, float y1,
                          int depth) {
    float dx = x1 - x0, dy = y1 - y0;
    float d = (cx - x1) * dy - (cy - y1) * dx;
    if (depth >= kMaxDepth || d * d < kToleranceSq * (dx * dx + dy * dy)) {
        AddLine(edges, x0, y0, x1, y1);
        return;
    }
    float x01 = (x0 + cx) * 0.5f, y01 = (y0 + cy) * 0.5f;
    float x12 = (cx + x1) * 0.5f, y12 = (cy + y1) * 0.5f;
    float x012 = (x01 + x12) * 0.5f, y012 = (y01 + y12) * 0.5f;
    FlattenQuadraticRec(edges, x0, y0, x01, y01, x012, y012, depth + 1);
    FlattenQuadraticRec(edges, x012, y012, x12, y12, x1, y1, depth + 1);
}

void FlattenCubicRec(std::vector<Edge> &edges, float x0, float y0, float c1x, float c1y, float c2x, float c2y,
                      float x1, float y1, int depth) {
    float dx = x1 - x0, dy = y1 - y0;
    float d1 = (c1x - x1) * dy - (c1y - y1) * dx;
    float d2 = (c2x - x1) * dy - (c2y - y1) * dx;
    float d = std::max(d1 * d1, d2 * d2);
    if (depth >= kMaxDepth || d < kToleranceSq * (dx * dx + dy * dy)) {
        AddLine(edges, x0, y0, x1, y1);
        return;
    }
    float x01 = (x0 + c1x) * 0.5f, y01 = (y0 + c1y) * 0.5f;
    float x12 = (c1x + c2x) * 0.5f, y12 = (c1y + c2y) * 0.5f;
    float x23 = (c2x + x1) * 0.5f, y23 = (c2y + y1) * 0.5f;
    float x012 = (x01 + x12) * 0.5f, y012 = (y01 + y12) * 0.5f;
    float x123 = (x12 + x23) * 0.5f, y123 = (y12 + y23) * 0.5f;
    float x0123 = (x012 + x123) * 0.5f, y0123 = (y012 + y123) * 0.5f;
    FlattenCubicRec(edges, x0, y0, x01, y01, x012, y012, x0123, y0123, depth + 1);
    FlattenCubicRec(edges, x0123, y0123, x123, y123, x23, y23, x1, y1, depth + 1);
}
}  // namespace

void FlattenQuadratic(std::vector<Edge> &edges, float x0, float y0, float cx, float cy, float x1, float y1) {
    FlattenQuadraticRec(edges, x0, y0, cx, cy, x1, y1, 0);
}

void FlattenCubic(std::vector<Edge> &edges, float x0, float y0, float c1x, float c1y, float c2x, float c2y, float x1,
                   float y1) {
    FlattenCubicRec(edges, x0, y0, c1x, c1y, c2x, c2y, x1, y1, 0);
}

namespace {
constexpr int kSupersample = 4;
}

std::vector<unsigned char> Rasterize(std::vector<Edge> &edges, int width, int height, FillRule rule) {
    std::vector<unsigned char> out(static_cast<size_t>(std::max(width, 0)) * static_cast<size_t>(std::max(height, 0)),
                                    0);
    if (edges.empty() || width <= 0 || height <= 0) return out;
    std::vector<float> row_coverage(static_cast<size_t>(width));
    std::vector<std::pair<float, int>> crossings;  // (x, winding)
    for (int y = 0; y < height; y++) {
        std::fill(row_coverage.begin(), row_coverage.end(), 0.0f);
        for (int s = 0; s < kSupersample; s++) {
            float sy = static_cast<float>(y) + (static_cast<float>(s) + 0.5f) / static_cast<float>(kSupersample);
            crossings.clear();
            for (const Edge &e : edges) {
                if (sy < e.ymin || sy >= e.ymax) continue;
                float x = e.x_at_ymin + (sy - e.ymin) * e.dxdy;
                crossings.emplace_back(x, e.winding);
            }
            if (crossings.empty()) continue;
            std::sort(crossings.begin(), crossings.end(),
                      [](const std::pair<float, int> &a, const std::pair<float, int> &b) { return a.first < b.first; });
            // Walk spans between consecutive crossings where the
            // "inside" test (nonzero accumulated winding, or even-odd
            // crossing parity) holds, adding exact fractional horizontal
            // coverage (clipped to [0, width)) for this sub-scanline.
            int winding = 0;
            bool inside_evenodd = false;
            float span_start = 0.0f;
            bool in_span = false;
            for (const auto &cr : crossings) {
                bool inside_before, inside_after;
                if (rule == FillRule::kNonZero) {
                    int before = winding;
                    winding += cr.second;
                    inside_before = before != 0;
                    inside_after = winding != 0;
                } else {
                    inside_before = inside_evenodd;
                    inside_evenodd = !inside_evenodd;
                    inside_after = inside_evenodd;
                }
                if (!inside_before && inside_after) {
                    span_start = cr.first;
                    in_span = true;
                } else if (inside_before && !inside_after && in_span) {
                    float x0 = std::clamp(span_start, 0.0f, static_cast<float>(width));
                    float x1 = std::clamp(cr.first, 0.0f, static_cast<float>(width));
                    if (x1 > x0) {
                        int ix0 = static_cast<int>(std::floor(x0));
                        int ix1 = static_cast<int>(std::floor(x1));
                        if (ix0 == ix1) {
                            row_coverage[static_cast<size_t>(ix0)] += (x1 - x0);
                        } else {
                            row_coverage[static_cast<size_t>(ix0)] += (static_cast<float>(ix0 + 1) - x0);
                            for (int px = ix0 + 1; px < ix1; px++) row_coverage[static_cast<size_t>(px)] += 1.0f;
                            if (ix1 < width) row_coverage[static_cast<size_t>(ix1)] += (x1 - static_cast<float>(ix1));
                        }
                    }
                    in_span = false;
                }
            }
        }
        for (int x = 0; x < width; x++) {
            float coverage = row_coverage[static_cast<size_t>(x)] / static_cast<float>(kSupersample);
            coverage = std::clamp(coverage, 0.0f, 1.0f);
            out[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
                static_cast<unsigned char>(coverage * 255.0f + 0.5f);
        }
    }
    return out;
}

}  // namespace raster
}  // namespace gfx
