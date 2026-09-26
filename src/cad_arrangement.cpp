#include "cad_arrangement.h"

#include "cad_intersect.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace cad {
namespace {

inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

Vec2d At(const Curve3 &curve, double t) {
    const Vec3d p = curve.Point(t);
    return Vec2d{p.x, p.y};
}

}  // namespace

// --- Polygon helpers ---------------------------------------------------

bool PointInPolygon2(const Vec2d &p, const std::vector<Vec2d> &polygon) {
    bool inside = false;
    const std::size_t n = polygon.size();
    if (n < 3) return false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2d &a = polygon[i];
        const Vec2d &b = polygon[j];
        if ((a.y > p.y) != (b.y > p.y)) {
            const double x = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
            if (p.x < x) inside = !inside;
        }
    }
    return inside;
}

double DistanceToPolygon(const Vec2d &p, const std::vector<Vec2d> &polygon) {
    double best = std::numeric_limits<double>::infinity();
    const std::size_t n = polygon.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2d a = polygon[i];
        const Vec2d b = polygon[(i + 1) % n];
        const Vec2d ab = b - a;
        const double len2 = ab.Dot(ab);
        double t = 0.0;
        if (len2 > 0.0) t = std::clamp((p - a).Dot(ab) / len2, 0.0, 1.0);
        best = std::min(best, (p - (a + ab * t)).Length());
    }
    return best;
}

bool InteriorPoints(const std::vector<Vec2d> &outer, const std::vector<std::vector<Vec2d>> &holes, int wanted,
                    std::vector<Vec2d> *out) {
    Box3d extent;
    for (const Vec2d &p : outer) extent.Expand(Vec3d{p.x, p.y, 0.0});
    if (extent.IsEmpty()) return false;
    for (int resolution = 8; resolution <= 256; resolution *= 2) {
        std::vector<std::pair<double, Vec2d>> ranked;
        for (int i = 1; i < resolution; ++i) {
            for (int j = 1; j < resolution; ++j) {
                const Vec2d candidate{
                    extent.x.lo + extent.x.Width() * static_cast<double>(i) / static_cast<double>(resolution),
                    extent.y.lo + extent.y.Width() * static_cast<double>(j) / static_cast<double>(resolution)};
                if (!PointInPolygon2(candidate, outer)) continue;
                bool in_hole = false;
                for (const std::vector<Vec2d> &hole : holes) {
                    if (PointInPolygon2(candidate, hole)) in_hole = true;
                }
                if (in_hole) continue;
                double depth = DistanceToPolygon(candidate, outer);
                for (const std::vector<Vec2d> &hole : holes) {
                    depth = std::min(depth, DistanceToPolygon(candidate, hole));
                }
                ranked.push_back({depth, candidate});
            }
        }
        // A point that is merely "inside" by parity but sits on an edge
        // has a depth of nearly nothing, and "nearly nothing" has to be
        // measured against the region rather than against zero. A grid
        // laid over an area several units across lands exactly on the
        // edges of a region whose arms are half a unit wide, and the
        // distance from such a point to the polygon comes back as a
        // rounding error -- small, but not zero, so a test against zero
        // accepts it. The grid is refined until a point is found that is
        // inside by a margin worth believing.
        std::sort(ranked.begin(), ranked.end(),
                  [](const std::pair<double, Vec2d> &x, const std::pair<double, Vec2d> &y) {
                      return x.first > y.first;
                  });
        const double floor_depth = 1e-6 * std::max(extent.x.Width(), extent.y.Width());
        if (ranked.empty() || ranked.front().first <= floor_depth) continue;
        // Candidates are spread relative to how deep the region is, not
        // to how big it is: a thin arm has no two points far apart by the
        // region's overall standard, and would yield a single candidate.
        const double spread = 0.5 * ranked.front().first;
        out->clear();
        for (const std::pair<double, Vec2d> &entry : ranked) {
            if (entry.first <= floor_depth) break;
            bool too_close = false;
            for (const Vec2d &chosen : *out) {
                if ((chosen - entry.second).Length() < spread) too_close = true;
            }
            if (too_close) continue;
            out->push_back(entry.second);
            if (static_cast<int>(out->size()) >= wanted) break;
        }
        if (out->empty()) out->push_back(ranked.front().second);
        return true;
    }
    return false;
}

bool InteriorPoint(const std::vector<Vec2d> &outer, const std::vector<std::vector<Vec2d>> &holes, Vec2d *out) {
    std::vector<Vec2d> points;
    if (!InteriorPoints(outer, holes, 1, &points) || points.empty()) return false;
    *out = points.front();
    return true;
}

double PolygonSignedArea(const std::vector<Vec2d> &polygon) {
    double twice = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        twice += polygon[i].Cross(polygon[(i + 1) % polygon.size()]);
    }
    return 0.5 * twice;
}

// --- Arrangement -------------------------------------------------------

void Arrangement::AddSegment(std::shared_ptr<const Curve3> curve, int tag) {
    ArrangementSegment segment;
    segment.curve = std::move(curve);
    segment.curve->Domain(&segment.t0, &segment.t1);
    segment.tag = tag;
    input_.push_back(segment);
}

bool Arrangement::Build(int samples) {
    SplitAtIntersections(samples);
    if (!BuildHalfEdges()) return false;
    LinkFaces();
    ExtractCycles();
    return !cycles_.empty();
}

int Arrangement::VertexFor(const Vec2d &p) {
    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        if ((vertices_[i].position - p).Length() <= vertex_tolerance_) return static_cast<int>(i);
    }
    vertices_.push_back(ArrangementVertex{p});
    return static_cast<int>(vertices_.size()) - 1;
}

// Splits every input segment wherever another crosses it. Without this
// the arrangement has edges passing through each other with no vertex,
// and the face walk runs straight past the crossing.
void Arrangement::SplitAtIntersections(int samples) {
    Box3d extent;
    for (const ArrangementSegment &segment : input_) {
        for (int i = 0; i <= 8; ++i) {
            const Vec2d p =
                At(*segment.curve, segment.t0 + (segment.t1 - segment.t0) * static_cast<double>(i) / 8.0);
            extent.Expand(Vec3d{p.x, p.y, 0.0});
        }
    }
    vertex_tolerance_ = std::max(1e-12, extent.Extent().Length() * 1e-7);

    std::vector<std::vector<double>> cuts(input_.size());
    for (std::size_t i = 0; i < input_.size(); ++i) {
        cuts[i].push_back(input_[i].t0);
        cuts[i].push_back(input_[i].t1);
        // A segment that closes on itself -- a circle with nothing to
        // cross -- has only one endpoint, so it cannot become a half-edge
        // and the face walk never sees it. It is cut in half so that it
        // has two vertices like every other segment, and the circle then
        // bounds a region the way it should. A drilled hole is exactly
        // this case, and without the cut the plate came out solid.
        const Vec2d start = At(*input_[i].curve, input_[i].t0);
        const Vec2d end = At(*input_[i].curve, input_[i].t1);
        if ((end - start).Length() <= std::max(1e-12, (input_[i].t1 - input_[i].t0) * 1e-6)) {
            cuts[i].push_back(0.5 * (input_[i].t0 + input_[i].t1));
        }
    }
    IntersectOptions options;
    options.samples = samples;
    options.tolerance.linear = vertex_tolerance_ * 10.0;
    for (std::size_t i = 0; i < input_.size(); ++i) {
        for (std::size_t j = i + 1; j < input_.size(); ++j) {
            for (const CurveCurveHit &hit : IntersectCurves(*input_[i].curve, *input_[j].curve, options)) {
                if (hit.kind == ContactKind::Coincident) continue;
                if (hit.t1 > input_[i].t0 && hit.t1 < input_[i].t1) cuts[i].push_back(hit.t1);
                if (hit.t2 > input_[j].t0 && hit.t2 < input_[j].t1) cuts[j].push_back(hit.t2);
            }
        }
    }
    for (std::size_t i = 0; i < input_.size(); ++i) {
        std::sort(cuts[i].begin(), cuts[i].end());
        for (std::size_t k = 0; k + 1 < cuts[i].size(); ++k) {
            if (cuts[i][k + 1] - cuts[i][k] <= 1e-12) continue;
            ArrangementSegment piece = input_[i];
            piece.t0 = cuts[i][k];
            piece.t1 = cuts[i][k + 1];
            pieces_.push_back(piece);
        }
    }
}

bool Arrangement::BuildHalfEdges() {
    for (const ArrangementSegment &piece : pieces_) {
        const int a = VertexFor(At(*piece.curve, piece.t0));
        const int b = VertexFor(At(*piece.curve, piece.t1));
        if (a == b) continue;  // collapsed by merging; carries no area

        ArrangementHalfEdge forward;
        forward.origin = a;
        forward.segment = piece;
        ArrangementHalfEdge backward;
        backward.origin = b;
        backward.segment = piece;
        std::swap(backward.segment.t0, backward.segment.t1);

        const int index = static_cast<int>(half_edges_.size());
        forward.twin = index + 1;
        backward.twin = index;
        half_edges_.push_back(forward);
        half_edges_.push_back(backward);
    }
    return !half_edges_.empty();
}

double Arrangement::OutgoingAngle(const ArrangementHalfEdge &edge) const {
    const Vec2d origin = At(*edge.segment.curve, edge.segment.t0);
    // A short step along the segment rather than the analytic tangent:
    // the curve's parameterization can be uneven, and what the walk needs
    // is the direction the edge actually leaves in.
    const double step = 1e-6 * (edge.segment.t1 - edge.segment.t0);
    const Vec2d ahead = At(*edge.segment.curve, edge.segment.t0 + step);
    Vec2d direction = ahead - origin;
    if (direction.LengthSquared() <= 0.0) {
        direction = At(*edge.segment.curve, edge.segment.t1) - origin;
    }
    return std::atan2(direction.y, direction.x);
}

// The face-traversal links. From a half-edge arriving at a vertex, the
// next one is whichever leaves that vertex immediately clockwise of the
// way we came in -- which is what keeps the face's interior on the left
// and makes bounded faces come out counter-clockwise.
void Arrangement::LinkFaces() {
    std::vector<std::vector<int>> outgoing(vertices_.size());
    for (std::size_t i = 0; i < half_edges_.size(); ++i) {
        outgoing[Idx(half_edges_[i].origin)].push_back(static_cast<int>(i));
    }
    for (std::vector<int> &edges : outgoing) {
        std::sort(edges.begin(), edges.end(), [this](int x, int y) {
            return OutgoingAngle(half_edges_[Idx(x)]) < OutgoingAngle(half_edges_[Idx(y)]);
        });
    }
    for (std::size_t i = 0; i < half_edges_.size(); ++i) {
        const int twin = half_edges_[i].twin;
        const int vertex = half_edges_[Idx(twin)].origin;
        const std::vector<int> &edges = outgoing[Idx(vertex)];
        const auto found = std::find(edges.begin(), edges.end(), twin);
        if (found == edges.end()) continue;
        // One step backwards in counter-clockwise order is one step
        // clockwise.
        const std::size_t position = static_cast<std::size_t>(found - edges.begin());
        const std::size_t previous = (position + edges.size() - 1) % edges.size();
        half_edges_[i].next = edges[previous];
    }
}

void Arrangement::ExtractCycles() {
    for (std::size_t i = 0; i < half_edges_.size(); ++i) {
        if (half_edges_[i].visited) continue;
        ArrangementCycle cycle;
        int current = static_cast<int>(i);
        bool ok = true;
        for (int guard = 0; guard < static_cast<int>(half_edges_.size()) + 2; ++guard) {
            if (current < 0 || half_edges_[Idx(current)].visited) {
                ok = (current == static_cast<int>(i));
                break;
            }
            half_edges_[Idx(current)].visited = true;
            cycle.half_edges.push_back(current);
            current = half_edges_[Idx(current)].next;
            if (current == static_cast<int>(i)) break;
        }
        if (!ok || cycle.half_edges.size() < 2) continue;
        cycle.signed_area = PolygonSignedArea(CyclePolygon(cycle, 8));
        cycles_.push_back(std::move(cycle));
    }
}

std::vector<Vec2d> Arrangement::CyclePolygon(const ArrangementCycle &cycle, int samples_per_edge) const {
    std::vector<Vec2d> polygon;
    for (int h : cycle.half_edges) {
        const ArrangementHalfEdge &edge = half_edges_[Idx(h)];
        for (int i = 0; i < samples_per_edge; ++i) {
            const double f = static_cast<double>(i) / static_cast<double>(samples_per_edge);
            polygon.push_back(At(*edge.segment.curve, edge.segment.t0 +
                                                          f * (edge.segment.t1 - edge.segment.t0)));
        }
    }
    return polygon;
}

std::vector<Vec2d> Arrangement::CyclePolygon(int cycle, int samples_per_edge) const {
    if (cycle < 0 || Idx(cycle) >= cycles_.size()) return {};
    return CyclePolygon(cycles_[Idx(cycle)], samples_per_edge);
}

std::vector<ArrangementSegment> Arrangement::CycleSegments(int cycle) const {
    std::vector<ArrangementSegment> segments;
    if (cycle < 0 || Idx(cycle) >= cycles_.size()) return segments;
    for (int h : cycles_[Idx(cycle)].half_edges) segments.push_back(half_edges_[Idx(h)].segment);
    return segments;
}

bool Arrangement::IsTwinCycle(const ArrangementCycle &a, const ArrangementCycle &b) const {
    if (a.half_edges.size() != b.half_edges.size()) return false;
    std::set<int> twins;
    for (int h : a.half_edges) twins.insert(half_edges_[Idx(h)].twin);
    for (int h : b.half_edges) {
        if (twins.count(h) == 0) return false;
    }
    return true;
}

std::vector<ArrangementRegion> Arrangement::Regions(int interior_candidates) const {
    // A positive-area cycle bounds a region; a negative one is either a
    // hole in some region or the arrangement's unbounded outer face.
    std::vector<int> outers;
    std::vector<int> inners;
    int unbounded = -1;
    for (std::size_t i = 0; i < cycles_.size(); ++i) {
        if (cycles_[i].signed_area > 0.0) {
            outers.push_back(static_cast<int>(i));
            continue;
        }
        // The unbounded face is a negative cycle as well, and it is not a
        // hole in anything. Telling the two apart matters: mistaking it
        // for a hole makes the whole area a hole in itself, so no
        // interior point exists and the region is silently dropped --
        // which is exactly what happened to every L-shaped piece of one
        // of two solids while the other's came out fine.
        //
        // It is identified as the negative cycle of largest area. That is
        // sound rather than a heuristic: a hole always lies inside the
        // region it is a hole of, so it is always smaller than that
        // region's boundary, while the unbounded face's cycle traces the
        // whole arrangement's outer boundary.
        if (unbounded < 0 || std::fabs(cycles_[i].signed_area) > std::fabs(cycles_[Idx(unbounded)].signed_area)) {
            if (unbounded >= 0) inners.push_back(unbounded);
            unbounded = static_cast<int>(i);
            continue;
        }
        inners.push_back(static_cast<int>(i));
    }

    std::vector<ArrangementRegion> regions;
    for (int outer : outers) {
        const std::vector<Vec2d> outer_polygon = CyclePolygon(outer, 8);
        ArrangementRegion region;
        region.outer_cycle = outer;
        std::vector<std::vector<Vec2d>> hole_polygons;
        for (int inner : inners) {
            // A hole is never a hole in the region its own twin bounds:
            // those two cycles run along the same edges, so every point
            // of one is inside the other and the region would be declared
            // a hole in itself, leaving it with no interior point at all.
            if (IsTwinCycle(cycles_[Idx(inner)], cycles_[Idx(outer)])) continue;
            const std::vector<Vec2d> polygon = CyclePolygon(inner, 8);
            if (polygon.empty()) continue;
            // Containment is tested with a point in the hole's own
            // interior, not with a point on its boundary. A boundary
            // point is shared with whatever the hole touches, and a
            // crossing test on it answers by round-off.
            Vec2d probe;
            if (!InteriorPoint(polygon, {}, &probe)) continue;
            if (!PointInPolygon2(probe, outer_polygon)) continue;
            // A hole belongs to the smallest region containing it -- but
            // not to its own twin. A closed curve lying inside a region
            // gives two cycles along the same edges in opposite
            // directions: the area it encloses, and the hole it leaves in
            // the area around it. A point in the hole is trivially inside
            // the enclosed area, so a plain containment test hands the
            // hole to its own twin and the surrounding region never
            // learns it has a hole -- a drilled plate came out solid that
            // way.
            bool inside_smaller = false;
            for (int candidate : outers) {
                if (candidate == outer) continue;
                if (std::fabs(cycles_[Idx(candidate)].signed_area) >= std::fabs(cycles_[Idx(outer)].signed_area)) {
                    continue;
                }
                if (IsTwinCycle(cycles_[Idx(inner)], cycles_[Idx(candidate)])) continue;
                if (PointInPolygon2(probe, CyclePolygon(candidate, 8))) inside_smaller = true;
            }
            if (inside_smaller) continue;
            region.hole_cycles.push_back(inner);
            hole_polygons.push_back(polygon);
        }
        if (!InteriorPoints(outer_polygon, hole_polygons, interior_candidates, &region.interior) ||
            region.interior.empty()) {
            continue;
        }
        region.area = cycles_[Idx(outer)].signed_area;
        for (int hole : region.hole_cycles) region.area -= std::fabs(cycles_[Idx(hole)].signed_area);
        regions.push_back(std::move(region));
    }
    return regions;
}

}  // namespace cad
