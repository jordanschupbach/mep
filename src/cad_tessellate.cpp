#include "cad_tessellate.h"

#include "cad_pcurve.h"
#include "cad_predicates.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>

namespace cad {
namespace {

inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

double UnwrapToward(double value, double reference, double period) {
    if (period <= 0.0) return value;
    return value - period * std::round((value - reference) / period);
}

// One edge tessellated once. The 3D points are what makes the mesh
// watertight: every face using this edge takes these exact numbers.
struct EdgeSamples {
    std::vector<double> parameters;  // along the edge's own curve
    std::vector<Vec3d> points;
};

EdgeSamples TessellateEdge(const Model &model, EntityId edge_id, const TessellationOptions &options) {
    EdgeSamples samples;
    const Edge *edge = model.GetEdge(edge_id);
    if (edge == nullptr) return samples;
    const Curve3 *curve = model.CurveAt(edge->curve);
    if (curve == nullptr) return samples;

    // Curve3::Tessellate works over the curve's whole domain; an edge may
    // be a sub-range of it, so the samples are filtered and the endpoints
    // forced to be exactly the edge's own.
    std::vector<double> parameters;
    std::vector<Vec3d> points;
    curve->Tessellate(&parameters, &points, options.chord_tolerance, options.angle_tolerance);

    const double lo = std::min(edge->t_start, edge->t_end);
    const double hi = std::max(edge->t_start, edge->t_end);
    double domain_lo = 0.0;
    double domain_hi = 0.0;
    curve->Domain(&domain_lo, &domain_hi);

    // AN EDGE'S RANGE IS NOT ITS CURVE'S DOMAIN, and on a periodic curve
    // it need not even be inside it: an arc of a circle whose domain is
    // [0, 2pi] may perfectly legally run from 3pi/2 to 5pi/2, because an
    // angle only means anything modulo a turn. Filtering the curve's own
    // tessellation to the edge then keeps samples for the part that
    // overlaps and none at all for the part that does not, leaving half
    // the edge as one straight chord -- which is how a sphere read back
    // from a STEP file came out a percent short of the one it was
    // written from, while validating perfectly and being watertight.
    const bool inside = lo >= domain_lo - 1e-12 && hi <= domain_hi + 1e-12;
    if (inside) {
        samples.parameters.push_back(lo);
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            if (parameters[i] > lo + 1e-12 && parameters[i] < hi - 1e-12) {
                samples.parameters.push_back(parameters[i]);
            }
        }
        samples.parameters.push_back(hi);
    } else {
        // Outside, the curve's breakpoints say nothing about where this
        // edge needs them, so it is sampled at the density the curve
        // asked for over its own domain. That density is what the chord
        // and angle tolerances worked out, so it carries them across.
        const double span = domain_hi - domain_lo;
        const double step = parameters.size() > 1 && span > 0.0
                                ? span / static_cast<double>(parameters.size() - 1)
                                : (hi - lo);
        const int count = step > 0.0
                              ? std::max(2, static_cast<int>(std::ceil((hi - lo) / step)))
                              : 2;
        for (int i = 0; i <= count; ++i) {
            samples.parameters.push_back(lo + (hi - lo) * static_cast<double>(i) /
                                                  static_cast<double>(count));
        }
    }
    // A sub-range can end up with too few samples to follow a curved
    // edge; ensure a floor so a short arc is not one straight chord.
    if (samples.parameters.size() < 4 && curve->Kind() != CurveKind::Line) {
        samples.parameters.clear();
        const int count = 16;
        for (int i = 0; i <= count; ++i) {
            samples.parameters.push_back(lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(count));
        }
    }
    for (double t : samples.parameters) samples.points.push_back(curve->Point(t));

    // The samples were gathered in ascending curve parameter, but an
    // edge may run the other way along its curve (t_start > t_end) --
    // a boolean's imprinted edges routinely do, where MakeBox's never
    // did. Every consumer reads this array as "the edge from its start
    // vertex to its end vertex", and BuildLoopPCurves builds the
    // matching p-curve by walking t_start -> t_end, so the two only
    // agree if the samples carry the edge's own direction.
    if (edge->t_end < edge->t_start) {
        std::reverse(samples.parameters.begin(), samples.parameters.end());
        std::reverse(samples.points.begin(), samples.points.end());
    }
    // Snap the two ends onto the edge's own vertices. Evaluating the
    // curve gets within its own tolerance of them, which is not the same
    // as landing on them: at a corner where three edges meet, each edge
    // reaches the shared vertex along a different curve and therefore
    // misses it by a different ~1e-9. The faces then no longer meet in
    // space, and a solid that is watertight by construction tessellates
    // into one that is not -- which is exactly how a box/box intersection
    // came out with 27 vertices where 8 would do. The vertex is the
    // single authority for where that point is, so the samples defer to
    // it and every face arrives bit-identically.
    if (const Vertex *v = model.GetVertex(edge->start_vertex); v != nullptr) {
        samples.points.front() = v->point;
    }
    if (const Vertex *v = model.GetVertex(edge->end_vertex); v != nullptr) {
        samples.points.back() = v->point;
    }
    return samples;
}

// The boundary of one loop, in the face's parameter space, with the 3D
// position of each point carried alongside. The pairing is the whole
// point: (u,v) decides the triangulation, the 3D position goes into the
// mesh, and the two need not be consistent to round-off for the result to
// be watertight -- only the 3D positions need to be shared.
struct BoundaryPoint {
    Vec2d uv;
    Vec3d position;
};

bool BuildLoopBoundary(const Model &model, EntityId loop_id, const std::map<EntityId, EdgeSamples> &edge_samples,
                       const TessellationOptions &options, std::vector<BoundaryPoint> *out, std::string *error) {
    const Loop *loop = model.GetLoop(loop_id);
    if (loop == nullptr) {
        *error = "no such loop";
        return false;
    }
    const Face *face = model.GetFace(loop->face);
    if (face == nullptr) return false;
    const Surface *surface = model.SurfaceAt(face->surface);
    if (surface == nullptr) return false;
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    const double period_u = surface->IsClosedU(options.tolerance) ? (u_hi - u_lo) : 0.0;
    const double period_v = surface->IsClosedV(options.tolerance) ? (v_hi - v_lo) : 0.0;

    for (EntityId coedge_id : loop->coedges) {
        const CoEdge *coedge = model.GetCoEdge(coedge_id);
        if (coedge == nullptr) continue;
        const Edge *edge = model.GetEdge(coedge->edge);
        if (edge == nullptr) continue;
        const Curve3 *pcurve = model.PCurveAt(coedge->pcurve);
        if (pcurve == nullptr) {
            *error = "coedge " + std::to_string(coedge_id) + " has no p-curve; build them before tessellating";
            return false;
        }
        const auto found = edge_samples.find(coedge->edge);
        if (found == edge_samples.end()) continue;
        const EdgeSamples &samples = found->second;
        if (samples.points.size() < 2) continue;

        double pcurve_lo = 0.0;
        double pcurve_hi = 0.0;
        pcurve->Domain(&pcurve_lo, &pcurve_hi);

        const std::size_t count = samples.points.size();
        for (std::size_t i = 0; i < count; ++i) {
            // Every point, including the last. Deduplication happens
            // afterwards and in *parameter* space, which is the only
            // place it can be decided correctly.
            //
            // The tempting shortcut -- drop each coedge's last point
            // because the next one supplies it -- is right for an
            // ordinary edge and wrong at a pole. There the next coedge
            // does start at the same 3D point, but at a different (u,v):
            // a sphere's seam ends at (2*pi, pi/2) and the next use
            // begins at (0, pi/2), both being the north pole. Dropping
            // the last point means the polygon never reaches the pole at
            // all, the degenerate-boundary bridge below never fires, and
            // the face collapses to a sliver.
            const std::size_t index =
                coedge->orientation == Orientation::Forward ? i : (count - 1 - i);
            const Vec3d position = samples.points[index];

            // The parameter-space coordinate. Projecting gives the right
            // answer up to the seam ambiguity, which is resolved by
            // unwrapping toward the p-curve -- the p-curve already made
            // that choice for this coedge, and re-deriving it here would
            // risk disagreeing with it.
            const double fraction = static_cast<double>(i) / static_cast<double>(count - 1);
            (void)0;
            const Vec2d reference = PCurvePoint(*pcurve, pcurve_lo + fraction * (pcurve_hi - pcurve_lo));
            double u = 0.0;
            double v = 0.0;
            surface->ClosestPoint(position, &u, &v, nullptr, options.tolerance);
            BoundaryPoint point;
            point.uv = Vec2d{UnwrapToward(u, reference.x, period_u), UnwrapToward(v, reference.y, period_v)};
            point.position = position;
            // At a degenerate point (a pole) the projected u means
            // nothing; the p-curve's own value is the only sensible one.
            //
            // Degenerate means the area element vanishes, which is not
            // the same as the two derivatives being parallel. At a
            // sphere's pole they stay exactly perpendicular; it is dS/du
            // alone that shrinks to nothing. Comparing the cross product
            // against |Su|*|Sv| therefore measures the angle between
            // them, which is 90 degrees right up to the pole, and the
            // pole is never recognised. It has to be compared against
            // the area element a non-degenerate point would have.
            std::vector<std::vector<Vec3d>> ders;
            surface->Derivatives(point.uv.x, point.uv.y, 1, &ders);
            const double scale = std::max(ders[1][0].Length(), ders[0][1].Length());
            if (ders[1][0].Cross(ders[0][1]).Length() <= scale * scale * 1e-9) {
                point.uv = reference;
            }
            out->push_back(point);
        }
    }
    if (out->size() < 3) return false;

    // Remove consecutive points that repeat the same parameter
    // coordinate -- an ordinary shared corner, contributed once by each
    // of the two coedges meeting there. Done in parameter space, so a
    // pole (same 3D point, two different parameters) is correctly *not*
    // merged: those two points are distinct corners of the parameter
    // rectangle and the face needs both.
    {
        std::vector<BoundaryPoint> unique;
        for (const BoundaryPoint &point : *out) {
            if (!unique.empty() && (unique.back().uv - point.uv).Length() <= 1e-12) continue;
            unique.push_back(point);
        }
        while (unique.size() > 1 && (unique.front().uv - unique.back().uv).Length() <= 1e-12) {
            unique.pop_back();
        }
        *out = std::move(unique);
    }
    if (out->size() < 3) return false;

    // Bridge the gaps left by degenerate boundaries.
    //
    // A sphere's face is bounded in parameter space by the rectangle
    // [0,2pi] x [-pi/2,pi/2], but only its two vertical sides are edges:
    // the top and bottom collapse to the poles, so there is nothing to
    // make a coedge from. The boundary collected above is therefore two
    // parallel line segments -- a polygon of zero area that no
    // triangulator can do anything with.
    //
    // The fix is to walk the boundary and, wherever consecutive points
    // jump in parameter space, insert points along the straight parameter
    // segment between them. Their 3D positions come from the surface,
    // which at a pole gives the same point for every u -- exactly right,
    // since that is what being a pole means.
    {
        const double diagonal = std::sqrt((u_hi - u_lo) * (u_hi - u_lo) + (v_hi - v_lo) * (v_hi - v_lo));
        const double max_step = diagonal / 16.0;
        std::vector<BoundaryPoint> bridged;
        for (std::size_t i = 0; i < out->size(); ++i) {
            const BoundaryPoint &current = (*out)[i];
            const BoundaryPoint &next = (*out)[(i + 1) % out->size()];
            bridged.push_back(current);
            const double gap = (next.uv - current.uv).Length();
            if (gap <= max_step) continue;
            // Only bridge a *degenerate* boundary -- one where the two
            // parameter points are far apart and yet name the same 3D
            // point, which is exactly what a pole is. A long parameter
            // segment between two genuinely different points is an
            // ordinary edge and must not be subdivided here: the face on
            // the other side of it would subdivide independently, place
            // its points at different parameters, and the two would no
            // longer meet. That is a crack, and it is how this condition
            // came to be written -- a box tessellated with the
            // unconditional version had exactly correct volume and was
            // not watertight.
            if (!options.tolerance.SamePoint(current.position, next.position)) continue;
            const int steps = std::min(64, static_cast<int>(std::ceil(gap / max_step)));
            for (int k = 1; k < steps; ++k) {
                const double fraction = static_cast<double>(k) / static_cast<double>(steps);
                BoundaryPoint inserted;
                inserted.uv = current.uv + (next.uv - current.uv) * fraction;
                inserted.position = surface->Point(inserted.uv.x, inserted.uv.y);
                bridged.push_back(inserted);
            }
        }
        *out = std::move(bridged);
    }
    return out->size() >= 3;
}

double PolygonSignedArea(const std::vector<BoundaryPoint> &polygon) {
    double twice = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Vec2d &a = polygon[i].uv;
        const Vec2d &b = polygon[(i + 1) % polygon.size()].uv;
        twice += a.Cross(b);
    }
    return 0.5 * twice;
}

// Point-in-polygon by ray crossing, in parameter space. Used to decide
// which candidate interior points are inside the face and which
// triangles of the Delaunay belong to it.
bool PointInPolygon(const Vec2d &p, const std::vector<BoundaryPoint> &polygon) {
    bool inside = false;
    const std::size_t n = polygon.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2d &a = polygon[i].uv;
        const Vec2d &b = polygon[j].uv;
        if ((a.y > p.y) != (b.y > p.y)) {
            const double x = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
            if (p.x < x) inside = !inside;
        }
    }
    return inside;
}

// Delaunay triangulation of a point set in parameter space, by
// Bowyer-Watson insertion, over the exact predicates from Part 0.2.
//
// This replaces an ear clipper, and the reason is worth recording because
// the failure was not obvious. Ear clipping triangulates a polygon using
// only its boundary vertices, which is exactly right for a flat region
// and exactly wrong for a curved one. A cylinder's tube is a rectangle in
// parameter space whose bottom and top sides each carry one point per
// sample of a circular edge -- dozens of points, collinear in (u,v). A
// clipper either stalls on them (they form no convex ear) or, if
// collinear ears are allowed, emits triangles whose three corners are all
// on the bottom rim. Those have zero area in parameter space and are
// perfectly real in 3D: flat triangles lying inside the end disc, nowhere
// near the surface they claim to approximate. They cancel in pairs, which
// is why the tube's computed volume came out as exactly zero.
//
// Delaunay over boundary *and interior* points has neither problem. It
// never produces a triangle from three collinear points, it fills the
// interior with the structure a curved face needs, and the interior
// points are free to be placed wherever the curvature demands.
//
// LIMITATION: this is unconstrained. Boundary edges are respected as long
// as the region is convex or nearly so, which covers every face this part
// of the plan produces, and triangles straying outside are removed by the
// centroid test below. Genuinely non-convex faces arrive with Part C's
// booleans and want constrained Delaunay with edge recovery -- the same
// machinery Part G.3 needs for volume meshing, built once there.
struct Triangle {
    int a = 0, b = 0, c = 0;
    bool alive = true;
};

// Delaunay triangulation by Bowyer-Watson, with one vertex at infinity.
//
// WHY THERE IS NO SUPER-TRIANGLE. The usual way to start this algorithm
// is to wrap the points in a triangle big enough to contain them, then
// throw away everything still touching it at the end. That is wrong, and
// not by a little: what has to be contained is not the points but the
// *circumcircles*, and a triangulation's circumcircles are unbounded
// relative to its extent. A single sliver -- two points a hair apart and
// a third far away -- has a circumradius of (distance)^2 / (4 * hair),
// so no fixed multiple of the point set's size is enough. When the super
// triangle is too close, the algorithm quite correctly builds triangles
// onto it, and the final "throw away anything touching it" step then
// punches holes in the middle of the result.
//
// That is not hypothetical here. A cylinder's seam is a straight line,
// so it is sampled at its two ends, while the face's interior is gridded
// by curvature into a hundred rows. The strip between the seam and the
// first interior column is therefore spanned by slivers whose
// circumradii are a hundred times the face's own size, and a super
// triangle at twenty times the extent sat inside them. The mesh came
// apart along the seam, and -- the tell -- it came apart *more* as the
// tolerance was tightened.
//
// The fix is the standard one and has no constant to get wrong. There is
// one extra vertex, and it is at infinity. A ghost triangle (a, b, INF)
// stands for the half-plane beyond hull edge a->b; a point conflicts
// with it when it lies on the far side of that edge, which is an
// Orient2D test rather than an InCircle one. Every triangle, ghost or
// real, is stored with its own region on its left, which is what makes
// the cavity's edges cancel in pairs the same way for both.
void DelaunayTriangulate(const std::vector<Vec2d> &points, std::vector<Triangle> *out) {
    out->clear();
    const int count = static_cast<int>(points.size());
    if (count < 3) return;
    const int infinity = count;

    auto coordinates = [&points](int i) {
        return std::array<double, 2>{points[Idx(i)].x, points[Idx(i)].y};
    };

    // Seed: the first three points that are not collinear. A point set
    // with no such triple bounds no area and has no triangulation, which
    // the caller reports as a face with no triangles.
    int second = -1;
    for (int i = 1; i < count && second < 0; ++i) {
        if (points[Idx(i)].x != points[0].x || points[Idx(i)].y != points[0].y) second = i;
    }
    if (second < 0) return;
    int third = -1;
    for (int i = second + 1; i < count && third < 0; ++i) {
        if (Orient2D(coordinates(0).data(), coordinates(second).data(), coordinates(i).data()) != 0) {
            third = i;
        }
    }
    if (third < 0) return;

    std::vector<Triangle> triangles;
    Triangle seed{0, second, third, true};
    if (Orient2D(coordinates(seed.a).data(), coordinates(seed.b).data(), coordinates(seed.c).data()) < 0) {
        std::swap(seed.b, seed.c);
    }
    triangles.push_back(seed);
    // The three ghosts, each reversed against the hull edge it covers so
    // that its own half-plane is on its left.
    triangles.push_back(Triangle{seed.b, seed.a, infinity, true});
    triangles.push_back(Triangle{seed.c, seed.b, infinity, true});
    triangles.push_back(Triangle{seed.a, seed.c, infinity, true});

    auto conflicts = [&](const Triangle &t, int p) {
        const std::array<double, 2> point = coordinates(p);
        if (t.c != infinity) {
            const std::array<double, 2> a = coordinates(t.a);
            const std::array<double, 2> b = coordinates(t.b);
            const std::array<double, 2> c = coordinates(t.c);
            return InCircle(a.data(), b.data(), c.data(), point.data()) > 0;
        }
        const std::array<double, 2> a = coordinates(t.a);
        const std::array<double, 2> b = coordinates(t.b);
        const int side = Orient2D(a.data(), b.data(), point.data());
        if (side != 0) return side > 0;
        // Exactly on the hull edge's line. It belongs to this ghost only
        // if it falls within the edge, in which case the real triangle on
        // the other side conflicts too and the shared edge cancels --
        // which is what keeps a point landing on an existing edge from
        // producing a triangle of zero area.
        const double along_x = b[0] - a[0];
        const double along_y = b[1] - a[1];
        return (point[0] - a[0]) * along_x + (point[1] - a[1]) * along_y > 0.0 &&
               (point[0] - b[0]) * along_x + (point[1] - b[1]) * along_y < 0.0;
    };

    // Dead triangles are compacted away once they outnumber the live
    // ones. The scan above is over every entry, so leaving them in place
    // means re-reading the whole history of the triangulation at every
    // insertion; clearing them out amortises to nothing and cannot change
    // the result, since a dead triangle is skipped either way.
    std::size_t dead = 0;
    std::vector<std::pair<int, int>> cavity;
    for (int i = 0; i < count; ++i) {
        if (i == 0 || i == second || i == third) continue;
        if (dead * 2 > triangles.size()) {
            triangles.erase(std::remove_if(triangles.begin(), triangles.end(),
                                           [](const Triangle &t) { return !t.alive; }),
                            triangles.end());
            dead = 0;
        }
        cavity.clear();
        for (Triangle &t : triangles) {
            if (!t.alive || !conflicts(t, i)) continue;
            t.alive = false;
            ++dead;
            cavity.push_back({t.a, t.b});
            cavity.push_back({t.b, t.c});
            cavity.push_back({t.c, t.a});
        }
        // The cavity's boundary is every directed edge that appears once.
        for (std::size_t e = 0; e < cavity.size(); ++e) {
            bool shared = false;
            for (std::size_t f = 0; f < cavity.size() && !shared; ++f) {
                if (e == f) continue;
                if (cavity[e].first == cavity[f].second && cavity[e].second == cavity[f].first) shared = true;
            }
            if (shared) continue;
            const int from = cavity[e].first;
            const int to = cavity[e].second;
            // The new triangle is (from, to, i) as a cycle; a ghost one is
            // rotated so that infinity lands last, which is where the rest
            // of this function expects to find it.
            if (from == infinity) {
                triangles.push_back(Triangle{to, i, infinity, true});
            } else if (to == infinity) {
                triangles.push_back(Triangle{i, from, infinity, true});
            } else {
                triangles.push_back(Triangle{from, to, i, true});
            }
        }
    }

    for (const Triangle &t : triangles) {
        if (!t.alive || t.c == infinity) continue;
        out->push_back(t);
    }
}

// Bridges each hole into the outer polygon with a doubled-back seam, so
// the whole face becomes one simple polygon that ear clipping can handle.
// The standard construction: find the hole's rightmost vertex, find a
// visible outer vertex, and splice.
void BridgeHoles(std::vector<BoundaryPoint> *outer, std::vector<std::vector<BoundaryPoint>> *holes) {
    for (std::vector<BoundaryPoint> &hole : *holes) {
        if (hole.size() < 3 || outer->size() < 3) continue;
        // A hole must run opposite to the outer loop for the bridge to
        // produce a simple polygon.
        if (PolygonSignedArea(hole) > 0.0) std::reverse(hole.begin(), hole.end());
        std::size_t hole_index = 0;
        for (std::size_t i = 1; i < hole.size(); ++i) {
            if (hole[i].uv.x > hole[hole_index].uv.x) hole_index = i;
        }
        // The nearest outer vertex to the right of it. Nearest rather
        // than strictly-visible: the faces this pass builds have convex
        // or near-convex outer boundaries, and the full visibility test
        // is only needed for deeply non-convex ones, which arrive with
        // Part C's booleans.
        std::size_t outer_index = 0;
        double best = 1e300;
        for (std::size_t i = 0; i < outer->size(); ++i) {
            const double distance = ((*outer)[i].uv - hole[hole_index].uv).LengthSquared();
            if (distance < best) {
                best = distance;
                outer_index = i;
            }
        }
        std::vector<BoundaryPoint> merged;
        merged.insert(merged.end(), outer->begin(), outer->begin() + static_cast<std::ptrdiff_t>(outer_index + 1));
        for (std::size_t i = 0; i <= hole.size(); ++i) {
            merged.push_back(hole[(hole_index + i) % hole.size()]);
        }
        merged.push_back((*outer)[outer_index]);
        merged.insert(merged.end(), outer->begin() + static_cast<std::ptrdiff_t>(outer_index + 1), outer->end());
        *outer = std::move(merged);
    }
    holes->clear();
}

}  // namespace

double TessellationMesh::SignedVolume() const {
    // The divergence theorem on a triangle mesh: each triangle
    // contributes a tetrahedron with the origin.
    double total = 0.0;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const Vec3d &a = positions[Idx(indices[t])];
        const Vec3d &b = positions[Idx(indices[t + 1])];
        const Vec3d &c = positions[Idx(indices[t + 2])];
        total += a.Dot(b.Cross(c));
    }
    return total / 6.0;
}

bool TessellationMesh::IsClosed() const {
    // Every directed edge must appear exactly once, and its reverse
    // exactly once. A crack shows up as an unmatched edge; a flipped
    // triangle as a duplicate running the same way.
    //
    // The comparison is by *position*, not by vertex index. Each face is
    // emitted with its own range of vertices so that it can carry its own
    // normals -- two faces meeting along an edge therefore have separate
    // indices for the same point, and an index-based test would call even
    // a perfectly watertight mesh open. Welding by position is also the
    // more honest test: it asks whether the faces actually meet in space,
    // which is what watertight means.
    std::map<std::tuple<long long, long long, long long>, int> welded;
    std::vector<int> vertex_key(positions.size(), 0);
    // A quantisation fine enough to separate genuinely distinct points and
    // coarse enough to merge the same point arrived at from two faces.
    // Shared edge samples are bit-identical by construction, so this only
    // has to survive that, not bridge a real gap.
    constexpr double kQuantum = 1e9;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        // llround already returns long long; casting it again trips
        // -Wuseless-cast, which this build treats as an error.
        const auto key = std::make_tuple(std::llround(positions[i].x * kQuantum),
                                         std::llround(positions[i].y * kQuantum),
                                         std::llround(positions[i].z * kQuantum));
        const auto found = welded.find(key);
        if (found == welded.end()) {
            const int next = static_cast<int>(welded.size());
            welded.emplace(key, next);
            vertex_key[i] = next;
        } else {
            vertex_key[i] = found->second;
        }
    }

    std::map<std::pair<int, int>, int> directed;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const int v[3] = {vertex_key[Idx(indices[t])], vertex_key[Idx(indices[t + 1])],
                          vertex_key[Idx(indices[t + 2])]};
        // Degenerate triangles (two corners welded together) carry no
        // area and no boundary; skipping them is correct rather than
        // lenient, and they are produced deliberately at poles.
        if (v[0] == v[1] || v[1] == v[2] || v[2] == v[0]) continue;
        for (int k = 0; k < 3; ++k) ++directed[std::make_pair(v[k], v[(k + 1) % 3])];
    }
    for (const auto &entry : directed) {
        if (entry.second != 1) return false;
        const auto reverse = directed.find(std::make_pair(entry.first.second, entry.first.first));
        if (reverse == directed.end() || reverse->second != 1) return false;
    }
    return !directed.empty();
}

namespace {

bool TessellateFaceWithSamples(const Model &model, EntityId face_id,
                               const std::map<EntityId, EdgeSamples> &edge_samples,
                               const TessellationOptions &options, TessellationMesh *out, std::string *error) {
    const Face *face = model.GetFace(face_id);
    if (face == nullptr) {
        *error = "no such face";
        return false;
    }
    const Surface *surface = model.SurfaceAt(face->surface);
    if (surface == nullptr) {
        *error = "face " + std::to_string(face_id) + " has no surface";
        return false;
    }

    std::vector<BoundaryPoint> outer;
    std::vector<std::vector<BoundaryPoint>> holes;
    for (EntityId loop_id : face->loops) {
        const Loop *loop = model.GetLoop(loop_id);
        if (loop == nullptr) continue;
        std::vector<BoundaryPoint> boundary;
        if (!BuildLoopBoundary(model, loop_id, edge_samples, options, &boundary, error)) {
            if (loop->is_outer) return false;
            continue;
        }
        if (loop->is_outer) {
            outer = std::move(boundary);
        } else {
            holes.push_back(std::move(boundary));
        }
    }
    if (outer.size() < 3) {
        *error = "face " + std::to_string(face_id) + " has no usable outer boundary";
        return false;
    }
    BridgeHoles(&outer, &holes);

    // Interior points, on a grid fine enough that each cell's 3D chord
    // error is within tolerance. The grid resolution comes from the
    // surface's own curvature rather than from a fixed number: a plane
    // needs none at all, a tightly-curved torus needs many.
    std::vector<BoundaryPoint> points = std::move(outer);
    // Captured before any interior point is added: the region test below
    // must use the face's actual boundary, not the augmented point set.
    const std::vector<BoundaryPoint> boundary_polygon = points;
    {
        Box3d parameter_extent;
        for (const BoundaryPoint &point : points) {
            parameter_extent.Expand(Vec3d{point.uv.x, point.uv.y, 0.0});
        }
        const double span_u = parameter_extent.x.Width();
        const double span_v = parameter_extent.y.Width();
        // Sagitta of one cell, estimated from the largest principal
        // curvature over a coarse probe of the region.
        double max_curvature = 0.0;
        for (int i = 0; i <= 4; ++i) {
            for (int j = 0; j <= 4; ++j) {
                const double u = parameter_extent.x.lo + span_u * static_cast<double>(i) / 4.0;
                const double v = parameter_extent.y.lo + span_v * static_cast<double>(j) / 4.0;
                double k1 = 0.0;
                double k2 = 0.0;
                surface->PrincipalCurvatures(u, v, &k1, &k2);
                if (std::isfinite(k1)) max_curvature = std::max(max_curvature, std::fabs(k1));
            }
        }
        int cells_u = 1;
        int cells_v = 1;
        if (max_curvature > 0.0) {
            // For a cell of 3D size h on a surface of curvature k, the
            // sagitta is about k*h^2/8, so h = sqrt(8*tol/k).
            const double allowed = std::sqrt(8.0 * options.chord_tolerance / max_curvature);
            // Convert that 3D size back into parameter steps through the
            // metric -- the first fundamental form, which is exactly what
            // it is for.
            double e = 0.0, f = 0.0, g = 0.0;
            surface->FirstFundamentalForm(parameter_extent.x.Mid(), parameter_extent.y.Mid(), &e, &f, &g);
            const double scale_u = std::sqrt(std::max(1e-30, e));
            const double scale_v = std::sqrt(std::max(1e-30, g));
            cells_u = std::max(1, static_cast<int>(std::ceil(span_u * scale_u / allowed)));
            cells_v = std::max(1, static_cast<int>(std::ceil(span_v * scale_v / allowed)));
            const int limit = std::max(4, static_cast<int>(std::sqrt(
                                              static_cast<double>(options.max_triangles_per_face))));
            cells_u = std::min(cells_u, limit);
            cells_v = std::min(cells_v, limit);
        }
        for (int i = 1; i < cells_u; ++i) {
            for (int j = 1; j < cells_v; ++j) {
                const Vec2d uv{parameter_extent.x.lo + span_u * static_cast<double>(i) /
                                                           static_cast<double>(cells_u),
                               parameter_extent.y.lo + span_v * static_cast<double>(j) /
                                                           static_cast<double>(cells_v)};
                // Against the *boundary*, not against `points` -- which
                // is being appended to inside this very loop, so testing
                // against it would ask whether each candidate is inside a
                // polygon that already contains the previous candidates.
                if (!PointInPolygon(uv, boundary_polygon)) continue;
                BoundaryPoint interior;
                interior.uv = uv;
                interior.position = surface->Point(uv.x, uv.y);
                points.push_back(interior);
            }
        }
    }

    std::vector<Vec2d> parameter_points;
    parameter_points.reserve(points.size());
    for (const BoundaryPoint &point : points) parameter_points.push_back(point.uv);
    std::vector<Triangle> delaunay;
    DelaunayTriangulate(parameter_points, &delaunay);

    // Keep only the triangles inside the face. The boundary polygon is
    // the first `boundary_count` points, in order.
    std::vector<int> triangles;
    for (const Triangle &t : delaunay) {
        const Vec2d centroid =
            (points[Idx(t.a)].uv + points[Idx(t.b)].uv + points[Idx(t.c)].uv) / 3.0;
        if (!PointInPolygon(centroid, boundary_polygon)) continue;
        triangles.push_back(t.a);
        triangles.push_back(t.b);
        triangles.push_back(t.c);
    }
    if (triangles.empty()) {
        *error = "face " + std::to_string(face_id) + " produced no triangles";
        return false;
    }

    // Emit. Normals come from the *surface*, not from the triangles, so a
    // coarse tessellation of a curved face still shades smoothly -- and
    // they are flipped for a reversed face, which is the only place the
    // face's orientation reaches the output.
    const int base = out->VertexCount();
    for (const BoundaryPoint &point : points) {
        out->positions.push_back(point.position);
        Vec3d normal = surface->Normal(point.uv.x, point.uv.y);
        if (normal.LengthSquared() <= 0.0) normal = Vec3d{0.0, 0.0, 1.0};
        out->normals.push_back(face->orientation == Orientation::Forward ? normal : -normal);
    }
    for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
        // A reversed face's triangles must wind the other way, or the
        // mesh's outward normal disagrees with the solid's.
        if (face->orientation == Orientation::Forward) {
            out->indices.push_back(base + triangles[t]);
            out->indices.push_back(base + triangles[t + 1]);
            out->indices.push_back(base + triangles[t + 2]);
        } else {
            out->indices.push_back(base + triangles[t]);
            out->indices.push_back(base + triangles[t + 2]);
            out->indices.push_back(base + triangles[t + 1]);
        }
        out->triangle_face.push_back(face_id);
    }
    return true;
}

std::map<EntityId, EdgeSamples> SampleEdges(const Model &model, const std::vector<EntityId> &edges,
                                            const TessellationOptions &options) {
    std::map<EntityId, EdgeSamples> samples;
    for (EntityId e : edges) samples[e] = TessellateEdge(model, e, options);
    return samples;
}

}  // namespace

bool TessellateFace(const Model &model, EntityId face_id, const TessellationOptions &options,
                    TessellationMesh *out, std::string *error) {
    const std::map<EntityId, EdgeSamples> samples =
        SampleEdges(model, model.EdgesOfFace(face_id), options);
    return TessellateFaceWithSamples(model, face_id, samples, options, out, error);
}

bool TessellateBody(const Model &model, EntityId body_id, const TessellationOptions &options,
                    TessellationMesh *out, std::string *error) {
    const Body *body = model.GetBody(body_id);
    if (body == nullptr) {
        *error = "no such body";
        return false;
    }
    // Every edge of the body tessellated once, before any face is built.
    // This is what makes the result watertight, and it is the whole
    // reason TessellateBody exists rather than a loop over TessellateFace.
    std::vector<EntityId> edges;
    for (EntityId s : body->shells) {
        for (EntityId e : model.EdgesOfShell(s)) {
            if (std::find(edges.begin(), edges.end(), e) == edges.end()) edges.push_back(e);
        }
    }
    const std::map<EntityId, EdgeSamples> samples = SampleEdges(model, edges, options);

    for (EntityId s : body->shells) {
        const Shell *shell = model.GetShell(s);
        if (shell == nullptr) continue;
        for (EntityId f : shell->faces) {
            if (!TessellateFaceWithSamples(model, f, samples, options, out, error)) return false;
        }
    }
    return true;
}

bool TessellateModel(const Model &model, const TessellationOptions &options, TessellationMesh *out,
                     std::string *error) {
    for (const Body &body : model.Bodies()) {
        if (!TessellateBody(model, body.id, options, out, error)) return false;
    }
    return true;
}

}  // namespace cad
