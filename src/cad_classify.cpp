#include "cad_classify.h"

#include "cad_pcurve.h"

#include <algorithm>
#include <cmath>

namespace cad {
namespace {

// Samples a loop's p-curves into a polygon in parameter space. The same
// construction the tessellator uses, and for the same reason: a polygon
// is what a containment test can be run against.
std::vector<Vec2d> LoopPolygon(const Model &model, EntityId loop_id, int samples_per_coedge) {
    std::vector<Vec2d> polygon;
    const Loop *loop = model.GetLoop(loop_id);
    if (loop == nullptr) return polygon;
    for (EntityId coedge_id : loop->coedges) {
        const CoEdge *coedge = model.GetCoEdge(coedge_id);
        if (coedge == nullptr) return {};
        const Curve3 *pcurve = model.PCurveAt(coedge->pcurve);
        if (pcurve == nullptr) return {};
        double lo = 0.0;
        double hi = 0.0;
        pcurve->Domain(&lo, &hi);
        // Inclusive of the end point. Stopping one step short leaves the
        // polygon not reaching the corners of the parameter region, and
        // those corners are exactly where the interesting points are: a
        // sphere's poles sit at the top and bottom of its rectangle, and
        // a polygon that stops short of them puts them outside the face.
        // The duplicate where one coedge's end meets the next one's start
        // is a zero-length segment, which both the crossing test and the
        // distance test below handle without special-casing.
        for (int i = 0; i <= samples_per_coedge; ++i) {
            const double t = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(samples_per_coedge);
            polygon.push_back(PCurvePoint(*pcurve, t));
        }
    }
    return polygon;
}

bool PointInPolygon(const Vec2d &p, const std::vector<Vec2d> &polygon) {
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

// How close a parameter point is to a loop's boundary polygon, in
// parameter space. Used to notice that a ray hit an edge rather than a
// face's interior -- the degeneracy that makes crossing parity wrong.
double DistanceToPolygon(const Vec2d &p, const std::vector<Vec2d> &polygon) {
    double best = 1e300;
    const std::size_t n = polygon.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2d &a = polygon[j];
        const Vec2d &b = polygon[i];
        const Vec2d edge = b - a;
        const double length_squared = edge.LengthSquared();
        double t = 0.0;
        if (length_squared > 0.0) t = Clamp((p - a).Dot(edge) / length_squared, 0.0, 1.0);
        best = std::min(best, (p - (a + edge * t)).Length());
    }
    return best;
}

// A deterministic spread of ray directions. Deterministic matters: a
// classification that depends on a random number is a classification that
// cannot be reproduced when it goes wrong.
Vec3d RayDirection(int attempt) {
    static const Vec3d kDirections[8] = {
        Vec3d{1.0, 0.0, 0.0},
        Vec3d{0.7071067811865476, 0.5773502691896258, 0.4082482904638631},
        Vec3d{-0.3333333333333333, 0.8164965809277261, 0.4714045207910317},
        Vec3d{0.2672612419124244, -0.5345224838248488, 0.8017837257372732},
        Vec3d{-0.6, 0.48, -0.64},
        Vec3d{0.4364357804719848, 0.8728715609439696, -0.2182178902359924},
        Vec3d{-0.5144957554275265, -0.2572478777137632, 0.8181884472346283},
        Vec3d{0.1543033499620919, -0.7715167498104595, -0.6172133998483676},
    };
    return kDirections[static_cast<std::size_t>(attempt) % 8].Normalized();
}

}  // namespace

bool PointInFace(const Model &model, EntityId face_id, double u, double v) {
    const Face *face = model.GetFace(face_id);
    if (face == nullptr) return false;
    const Vec2d p{u, v};
    bool inside_outer = false;
    for (EntityId loop_id : face->loops) {
        const Loop *loop = model.GetLoop(loop_id);
        if (loop == nullptr) return false;
        const std::vector<Vec2d> polygon = LoopPolygon(model, loop_id, 24);
        if (polygon.size() < 3) return false;

        // A face includes its own boundary, and points *on* that boundary
        // are not rare: an edge is on two faces, and a sphere's poles sit
        // exactly on the top and bottom of its parameter rectangle. Ray
        // crossing is undefined there -- whether it reports inside
        // depends on which side of an exactly-coincident edge the
        // arithmetic lands -- so proximity to the boundary is tested
        // first and answers "yes" without asking the parity question.
        Box3d extent;
        for (const Vec2d &point : polygon) extent.Expand(Vec3d{point.x, point.y, 0.0});
        const double on_boundary = std::max(1e-12, extent.Extent().Length() * 1e-9);
        const bool near_edge = DistanceToPolygon(p, polygon) <= on_boundary;
        const bool inside = near_edge || PointInPolygon(p, polygon);
        if (loop->is_outer) {
            if (!inside) return false;
            inside_outer = true;
        } else if (inside && !near_edge) {
            // Strictly inside a hole means outside the face; on the
            // hole's own rim it is still on the face.
            return false;
        }
    }
    return inside_outer;
}

std::vector<RayHit> CastRay(const Model &model, EntityId body_id, const Vec3d &origin, const Vec3d &direction,
                            const ClassifyOptions &options) {
    std::vector<RayHit> hits;
    const Body *body = model.GetBody(body_id);
    if (body == nullptr) return hits;
    const Vec3d unit = direction.Normalized();
    // Long enough to leave any body in the model.
    const Box3d bounds = model.Bounds();
    const double reach = bounds.IsEmpty() ? 1.0 : bounds.Diagonal() * 4.0 + 1.0;
    const Line3 ray(origin, unit, 0.0, reach);

    for (EntityId shell_id : body->shells) {
        const Shell *shell = model.GetShell(shell_id);
        if (shell == nullptr) continue;
        for (EntityId face_id : shell->faces) {
            const Face *face = model.GetFace(face_id);
            if (face == nullptr) continue;
            const Surface *surface = model.SurfaceAt(face->surface);
            if (surface == nullptr) continue;
            IntersectOptions ray_options = options.intersect;
            ray_options.samples = std::max(ray_options.samples, options.ray_samples);
            for (const CurveSurfaceHit &hit : IntersectCurveSurface(ray, *surface, ray_options)) {
                if (hit.t <= options.intersect.tolerance.linear) continue;  // behind or at the origin
                if (!PointInFace(model, face_id, hit.u, hit.v)) continue;
                RayHit entry;
                entry.distance = hit.t;
                entry.face = face_id;
                entry.point = hit.point;
                entry.normal = model.FaceNormal(face_id, hit.u, hit.v);
                entry.u = hit.u;
                entry.v = hit.v;
                hits.push_back(entry);
            }
        }
    }
    std::sort(hits.begin(), hits.end(),
              [](const RayHit &a, const RayHit &b) { return a.distance < b.distance; });
    return hits;
}

PointClass ClassifyPoint(const Model &model, EntityId body_id, const Vec3d &point,
                         const ClassifyOptions &options) {
    const Body *body = model.GetBody(body_id);
    if (body == nullptr) return PointClass::Outside;

    // On the boundary? Checked first and separately, because a point on a
    // face is neither inside nor outside and no amount of ray casting
    // will say so reliably.
    for (EntityId shell_id : body->shells) {
        const Shell *shell = model.GetShell(shell_id);
        if (shell == nullptr) continue;
        for (EntityId face_id : shell->faces) {
            const Face *face = model.GetFace(face_id);
            if (face == nullptr) continue;
            const Surface *surface = model.SurfaceAt(face->surface);
            if (surface == nullptr) continue;
            double u = 0.0;
            double v = 0.0;
            Vec3d on_surface;
            if (!surface->ClosestPoint(point, &u, &v, &on_surface, options.intersect.tolerance)) continue;
            if (!options.intersect.tolerance.SamePoint(point, on_surface)) continue;
            if (PointInFace(model, face_id, u, v)) return PointClass::Boundary;
        }
    }

    // Crossing parity, retried on a different ray when the first one was
    // unlucky. A ray is unlucky when it passes within tolerance of a
    // face's boundary in parameter space (so it may have hit an edge, and
    // been counted once, twice or not at all) or grazes a face
    // tangentially. Detecting and re-firing is far more reliable than
    // trying to decide such a hit correctly.
    for (int attempt = 0; attempt < std::max(1, options.max_ray_attempts); ++attempt) {
        const Vec3d direction = RayDirection(attempt);
        const std::vector<RayHit> hits = CastRay(model, body_id, point, direction, options);
        bool unlucky = false;
        for (std::size_t i = 0; i < hits.size(); ++i) {
            // Grazing: the ray lies in the face's tangent plane, so
            // whether it "crossed" is not determined.
            if (std::fabs(direction.Dot(hits[i].normal)) <= options.grazing_threshold) unlucky = true;
            // Through an edge or vertex: two hits at the same place, or a
            // hit close to the face's boundary in parameter space.
            if (i > 0 && std::fabs(hits[i].distance - hits[i - 1].distance) <=
                             options.intersect.tolerance.linear * 10.0) {
                unlucky = true;
            }
            // Proximity to a face's boundary in parameter space is
            // deliberately *not* treated as unlucky. Much of that
            // boundary is a seam -- a cylinder's or sphere's parameter
            // rectangle has two sides that are not edges of the solid at
            // all, only of its parameterization -- and a ray crossing
            // there is perfectly well defined. Hitting a *real* edge
            // shows up instead as two hits at the same distance, which
            // the test above already catches.
        }
        if (unlucky) continue;
        return (hits.size() % 2 == 1) ? PointClass::Inside : PointClass::Outside;
    }
    // Every ray was degenerate. Saying Outside here would be a guess, but
    // there is no better answer available and a caller that cares can
    // move the point; what matters is that this is reached only in
    // genuinely pathological configurations, not silently in ordinary use.
    return PointClass::Outside;
}

}  // namespace cad
