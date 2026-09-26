#include "cad_intersect.h"

#include <algorithm>
#include <cmath>

namespace cad {
namespace {

inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// Concrete-type access. Kind() has already said what the surface is, so
// these only ever fail on a caller error, and returning null lets the
// analytic table decline rather than assert.
template <typename T>
const T *As(const Surface &surface, SurfaceKind kind) {
    if (surface.Kind() != kind) return nullptr;
    return dynamic_cast<const T *>(&surface);
}

// --- Curve-curve -----------------------------------------------------

// The distance from a point on `a` to the whole of `b`. Built on
// Curve3::ClosestPoint, which is already two-stage and already verified
// exact against brute force -- reusing it here is both less code and
// strictly more robust than a second projection written from scratch.
double DistanceToCurve(const Curve3 &a, const Curve3 &b, double t, const Tolerance &tolerance,
                       double *out_t2) {
    double t2 = 0.0;
    Vec3d on_b;
    if (!b.ClosestPoint(a.Point(t), &t2, &on_b, tolerance)) return 1e300;
    if (out_t2 != nullptr) *out_t2 = t2;
    return (a.Point(t) - on_b).Length();
}

void AddCurveHit(std::vector<CurveCurveHit> &hits, const CurveCurveHit &hit, const Tolerance &tolerance) {
    for (const CurveCurveHit &existing : hits) {
        if (tolerance.SamePoint(existing.point, hit.point)) return;
    }
    hits.push_back(hit);
}

// --- Analytic helpers -------------------------------------------------

// The plane through `point` with unit normal `normal`, as the pair
// (normal, offset) with offset = normal . point.
struct PlaneForm {
    Vec3d normal;
    double offset = 0.0;
};

PlaneForm PlaneOf(const PlaneSurface &plane) {
    PlaneForm form;
    form.normal = plane.PlaneNormal().Normalized();
    form.offset = form.normal.Dot(plane.Origin());
    return form;
}

// A circle lying in a plane, as a Curve3.
std::shared_ptr<const Curve3> MakeCircle(const Vec3d &center, const Vec3d &normal, double radius) {
    const Vec3d n = normal.Normalized();
    const Vec3d x = n.AnyPerpendicular();
    return std::make_shared<Circle3>(center, x, n.Cross(x), radius, 0.0, kTwoPi);
}

std::shared_ptr<const Curve3> MakeEllipse(const Vec3d &center, const Vec3d &major_axis, const Vec3d &minor_axis,
                                          double major, double minor) {
    return std::make_shared<Ellipse3>(center, major_axis, minor_axis, major, minor, 0.0, kTwoPi);
}

// Clips an infinite line to the part where both surfaces actually contain
// the point, by sampling and then bisecting at the boundaries. Analytic
// intersections are naturally unbounded -- a plane meets a cylinder in a
// line whether or not the cylinder is that long -- and the branch has to
// be cut back to where both surfaces are defined before it can become an
// edge.
bool ClipLineToSurfaces(const Vec3d &origin, const Vec3d &direction, const Surface &a, const Surface &b,
                        const IntersectOptions &options, double extent, double *out_lo, double *out_hi) {
    auto on_both = [&](double t) {
        const Vec3d p = origin + direction * t;
        return a.ContainsPoint(p, nullptr, nullptr, options.tolerance) &&
               b.ContainsPoint(p, nullptr, nullptr, options.tolerance);
    };
    // The window is the part of the line where the two surfaces could
    // possibly both be -- the overlap of their bounding boxes -- not a
    // span sized by the whole model.
    //
    // Two things go wrong with the obvious version. The line's origin
    // comes out of a 3x3 solve and is wherever that lands, usually near
    // the coordinate origin, so a window centred on it can miss the
    // surfaces entirely. And a window sized by the *model* gives a
    // fixed number of samples to cover it, so a genuine overlap shorter
    // than one sample interval is not refined, it is never seen: a
    // fillet's cutter is a fraction of the size of the part it is cutting,
    // and its faces met the part's in runs that fell between samples.
    // The same fillet on a smaller part worked, which is exactly the
    // shape of bug that gets called intermittent.
    Box3d overlap = a.Bounds();
    const Box3d other = b.Bounds();
    if (overlap.IsEmpty() || other.IsEmpty()) return false;
    overlap.x = Interval{std::max(overlap.x.lo, other.x.lo), std::min(overlap.x.hi, other.x.hi)};
    overlap.y = Interval{std::max(overlap.y.lo, other.y.lo), std::min(overlap.y.hi, other.y.hi)};
    overlap.z = Interval{std::max(overlap.z.lo, other.z.lo), std::min(overlap.z.hi, other.z.hi)};
    // A little slack, so a surface that only just reaches the other is
    // not excluded by rounding.
    const double slack = std::max(1e-9, extent * 1e-6);
    double lowest = 0.0;
    double highest = 0.0;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3d point{(corner & 1) ? overlap.x.hi + slack : overlap.x.lo - slack,
                          (corner & 2) ? overlap.y.hi + slack : overlap.y.lo - slack,
                          (corner & 4) ? overlap.z.hi + slack : overlap.z.lo - slack};
        const double along = (point - origin).Dot(direction);
        lowest = (corner == 0) ? along : std::min(lowest, along);
        highest = (corner == 0) ? along : std::max(highest, along);
    }
    if (!(highest > lowest)) return false;
    const int samples = std::max(256, options.samples * 4);
    auto parameter = [&](int i) {
        return lowest + (highest - lowest) * static_cast<double>(i) / static_cast<double>(samples);
    };
    int first = -1;
    int last = -1;
    for (int i = 0; i <= samples; ++i) {
        if (!on_both(parameter(i))) continue;
        if (first < 0) first = i;
        last = i;
    }
    if (first < 0) return false;
    // Bisect outward from the first and last accepted samples to find the
    // true ends, so the clipped range is not quantised to the sampling.
    double lo = parameter(first);
    if (first > 0) {
        double outside = parameter(first - 1);
        double inside = lo;
        for (int i = 0; i < 40; ++i) {
            const double mid = 0.5 * (outside + inside);
            if (on_both(mid)) {
                inside = mid;
            } else {
                outside = mid;
            }
        }
        lo = inside;
    }
    double hi = parameter(last);
    if (last < samples) {
        double outside = parameter(last + 1);
        double inside = hi;
        for (int i = 0; i < 40; ++i) {
            const double mid = 0.5 * (outside + inside);
            if (on_both(mid)) {
                inside = mid;
            } else {
                outside = mid;
            }
        }
        hi = inside;
    }
    if (!(hi > lo)) return false;
    *out_lo = lo;
    *out_hi = hi;
    return true;
}

double ModelExtent(const Surface &a, const Surface &b) {
    Box3d box = a.Bounds();
    box.Expand(b.Bounds());
    return std::max(1.0, box.Diagonal());
}

// --- The analytic table ----------------------------------------------

bool PlanePlane(const PlaneSurface &a, const PlaneSurface &b, const Surface &sa, const Surface &sb,
                SurfaceIntersection *out, const IntersectOptions &options) {
    const PlaneForm fa = PlaneOf(a);
    const PlaneForm fb = PlaneOf(b);
    const Vec3d direction = fa.normal.Cross(fb.normal);
    if (direction.Length() <= options.tolerance.angular) {
        // Parallel. Either the same plane or no intersection at all, and
        // the difference matters to a boolean: coincident faces have to be
        // merged, not intersected.
        const double gap = std::fabs(fa.offset - fb.offset * (fa.normal.Dot(fb.normal) > 0.0 ? 1.0 : -1.0));
        out->kind = (gap <= options.tolerance.linear) ? ContactKind::Coincident : ContactKind::None;
        out->exact = true;
        out->note = (out->kind == ContactKind::Coincident) ? "planes are coincident" : "planes are parallel";
        return true;
    }
    // A point on the line: solve the 3x3 system whose rows are the two
    // normals and the line direction.
    const Mat3d system = Mat3d::FromRows(fa.normal, fb.normal, direction);
    Vec3d origin;
    if (!system.Solve(Vec3d{fa.offset, fb.offset, 0.0}, &origin, options.tolerance)) return false;
    const Vec3d unit = direction.Normalized();

    double lo = 0.0;
    double hi = 0.0;
    if (!ClipLineToSurfaces(origin, unit, sa, sb, options, ModelExtent(sa, sb), &lo, &hi)) {
        out->kind = ContactKind::None;
        out->exact = true;
        out->note = "planes cross, but not within either face's parameter range";
        return true;
    }
    IntersectionBranch branch;
    branch.curve = std::make_shared<Line3>(Line3::FromPoints(origin + unit * lo, origin + unit * hi));
    out->branches.push_back(branch);
    out->kind = ContactKind::Transversal;
    out->exact = true;
    out->note = "plane/plane: a line";
    return true;
}

bool PlaneSphere(const PlaneSurface &plane, const SphereSurface &sphere, SurfaceIntersection *out,
                 const IntersectOptions &options) {
    const PlaneForm form = PlaneOf(plane);
    const double signed_distance = form.normal.Dot(sphere.Center()) - form.offset;
    const double distance = std::fabs(signed_distance);
    const double radius = sphere.Radius();
    if (distance > radius + options.tolerance.linear) {
        out->kind = ContactKind::None;
        out->exact = true;
        return true;
    }
    const Vec3d center = sphere.Center() - form.normal * signed_distance;
    if (distance >= radius - options.tolerance.linear) {
        // Tangent: a single point. Reported as Tangent rather than as a
        // degenerate circle, because a boolean cannot separate material
        // along it.
        out->kind = ContactKind::Tangent;
        out->exact = true;
        out->note = "plane touches sphere at one point";
        return true;
    }
    IntersectionBranch branch;
    branch.curve = MakeCircle(center, form.normal, std::sqrt(radius * radius - distance * distance));
    branch.closed = true;
    out->branches.push_back(branch);
    out->kind = ContactKind::Transversal;
    out->exact = true;
    out->note = "plane/sphere: a circle";
    return true;
}

bool PlaneCylinder(const PlaneSurface &plane, const CylinderSurface &cylinder, const Surface &sa,
                   const Surface &sb, SurfaceIntersection *out, const IntersectOptions &options) {
    const PlaneForm form = PlaneOf(plane);
    const Vec3d axis = cylinder.Axis().Normalized();
    const double alignment = form.normal.Dot(axis);
    const double radius = cylinder.Radius();

    if (std::fabs(std::fabs(alignment) - 1.0) <= options.tolerance.angular) {
        // Perpendicular to the axis: a circle.
        const double t = (form.offset - form.normal.Dot(cylinder.Origin())) / alignment;
        IntersectionBranch branch;
        branch.curve = MakeCircle(cylinder.Origin() + axis * t, axis, radius);
        branch.closed = true;
        out->branches.push_back(branch);
        out->kind = ContactKind::Transversal;
        out->exact = true;
        out->note = "plane/cylinder: a circle";
        return true;
    }

    if (std::fabs(alignment) <= options.tolerance.angular) {
        // Parallel to the axis: zero, one or two lines, depending on how
        // far the plane is from the axis.
        const double axis_distance = std::fabs(form.normal.Dot(cylinder.Origin()) - form.offset);
        if (axis_distance > radius + options.tolerance.linear) {
            out->kind = ContactKind::None;
            out->exact = true;
            return true;
        }
        const Vec3d foot = cylinder.Origin() - form.normal * (form.normal.Dot(cylinder.Origin()) - form.offset);
        if (axis_distance >= radius - options.tolerance.linear) {
            out->kind = ContactKind::Tangent;
            out->exact = true;
            out->note = "plane is tangent to the cylinder along one line";
            return true;
        }
        const double half = std::sqrt(radius * radius - axis_distance * axis_distance);
        const Vec3d across = axis.Cross(form.normal).Normalized();
        const double extent = ModelExtent(sa, sb);
        for (int side = -1; side <= 1; side += 2) {
            const Vec3d origin = foot + across * (half * static_cast<double>(side));
            double lo = 0.0;
            double hi = 0.0;
            if (!ClipLineToSurfaces(origin, axis, sa, sb, options, extent, &lo, &hi)) continue;
            IntersectionBranch branch;
            branch.curve = std::make_shared<Line3>(Line3::FromPoints(origin + axis * lo, origin + axis * hi));
            out->branches.push_back(branch);
        }
        out->kind = out->branches.empty() ? ContactKind::None : ContactKind::Transversal;
        out->exact = true;
        out->note = "plane/cylinder: two lines";
        return true;
    }

    // Oblique: an ellipse. Its minor axis is the cylinder's own radius and
    // its major axis is that divided by the cosine of the tilt, which is
    // exactly |normal . axis|.
    const double t = (form.offset - form.normal.Dot(cylinder.Origin())) / alignment;
    const Vec3d center = cylinder.Origin() + axis * t;
    const Vec3d minor_direction = axis.Cross(form.normal).Normalized();
    const Vec3d major_direction = form.normal.Cross(minor_direction).Normalized();
    IntersectionBranch branch;
    branch.curve = MakeEllipse(center, major_direction, minor_direction, radius / std::fabs(alignment), radius);
    branch.closed = true;
    out->branches.push_back(branch);
    out->kind = ContactKind::Transversal;
    out->exact = true;
    out->note = "plane/cylinder: an ellipse";
    return true;
}

bool PlaneCone(const PlaneSurface &plane, const ConeSurface &cone, SurfaceIntersection *out,
               const IntersectOptions &options) {
    const PlaneForm form = PlaneOf(plane);
    const Vec3d axis = cone.Axis().Normalized();
    const Vec3d apex = cone.Apex();
    const double half_angle = cone.HalfAngle();
    const double alignment = form.normal.Dot(axis);
    const double apex_distance = form.normal.Dot(apex) - form.offset;

    if (std::fabs(apex_distance) <= options.tolerance.linear) {
        // Through the apex: a point or a pair of lines. Left to the
        // general path, since the degenerate vertex needs care a closed
        // form would only obscure.
        return false;
    }
    if (std::fabs(std::fabs(alignment) - 1.0) <= options.tolerance.angular) {
        // Perpendicular to the axis: a circle.
        const double height = std::fabs(apex_distance);
        IntersectionBranch branch;
        branch.curve = MakeCircle(apex - form.normal * apex_distance, axis, height * std::tan(half_angle));
        branch.closed = true;
        out->branches.push_back(branch);
        out->kind = ContactKind::Transversal;
        out->exact = true;
        out->note = "plane/cone: a circle";
        return true;
    }
    // An ellipse only when the plane cuts every generator, which happens
    // when it is steeper than the cone's own half angle. A parabola or
    // hyperbola is a real intersection too, but unbounded, and this
    // kernel has no representation for one -- so it declines and lets the
    // general marcher produce a bounded piece.
    if (std::fabs(alignment) <= std::sin(half_angle) + options.tolerance.angular) return false;

    // The two extreme points, where the plane meets the two generators
    // lying in the plane spanned by the axis and the normal.
    Vec3d across = form.normal - axis * alignment;
    if (across.LengthSquared() <= 0.0) return false;
    across = across.Normalized();
    Vec3d ends[2];
    for (int side = 0; side < 2; ++side) {
        const double sign = (side == 0) ? 1.0 : -1.0;
        const Vec3d generator = axis * std::cos(half_angle) + across * (sign * std::sin(half_angle));
        const double denominator = form.normal.Dot(generator);
        if (std::fabs(denominator) <= 1e-300) return false;
        ends[side] = apex + generator * ((form.offset - form.normal.Dot(apex)) / denominator);
    }
    const Vec3d center = (ends[0] + ends[1]) * 0.5;
    const Vec3d major_direction = (ends[1] - ends[0]).Normalized();
    const double major = (ends[1] - ends[0]).Length() * 0.5;
    // The semi-minor axis, from requiring a point at C + s*m to lie on
    // the cone: ((P-A).axis)^2 = |P-A|^2 cos^2(alpha), a quadratic in s.
    const Vec3d minor_direction = form.normal.Cross(major_direction).Normalized();
    const Vec3d offset = center - apex;
    const double cos2 = std::cos(half_angle) * std::cos(half_angle);
    const double a_term = minor_direction.Dot(axis) * minor_direction.Dot(axis) - cos2;
    const double b_term = 2.0 * (offset.Dot(axis) * minor_direction.Dot(axis) - cos2 * offset.Dot(minor_direction));
    const double c_term = offset.Dot(axis) * offset.Dot(axis) - cos2 * offset.Dot(offset);
    double minor = 0.0;
    if (std::fabs(a_term) <= 1e-300) return false;
    const double discriminant = b_term * b_term - 4.0 * a_term * c_term;
    if (discriminant < 0.0) return false;
    const double root = std::sqrt(discriminant);
    minor = std::fabs((-b_term + root) / (2.0 * a_term) - (-b_term - root) / (2.0 * a_term)) * 0.5;
    if (!(minor > 0.0)) return false;

    IntersectionBranch branch;
    branch.curve = MakeEllipse(center, major_direction, minor_direction, major, minor);
    branch.closed = true;
    out->branches.push_back(branch);
    out->kind = ContactKind::Transversal;
    out->exact = true;
    out->note = "plane/cone: an ellipse";
    return true;
}

bool SphereSphere(const SphereSurface &a, const SphereSurface &b, SurfaceIntersection *out,
                  const IntersectOptions &options) {
    const Vec3d between = b.Center() - a.Center();
    const double distance = between.Length();
    const double ra = a.Radius();
    const double rb = b.Radius();
    if (distance <= options.tolerance.linear) {
        out->kind = (std::fabs(ra - rb) <= options.tolerance.linear) ? ContactKind::Coincident
                                                                     : ContactKind::None;
        out->exact = true;
        out->note = "concentric spheres";
        return true;
    }
    if (distance > ra + rb + options.tolerance.linear ||
        distance < std::fabs(ra - rb) - options.tolerance.linear) {
        out->kind = ContactKind::None;
        out->exact = true;
        return true;
    }
    if (distance >= ra + rb - options.tolerance.linear ||
        distance <= std::fabs(ra - rb) + options.tolerance.linear) {
        out->kind = ContactKind::Tangent;
        out->exact = true;
        out->note = "spheres touch at one point";
        return true;
    }
    const Vec3d axis = between / distance;
    const double along = (distance * distance + ra * ra - rb * rb) / (2.0 * distance);
    const double radius = std::sqrt(std::max(0.0, ra * ra - along * along));
    IntersectionBranch branch;
    branch.curve = MakeCircle(a.Center() + axis * along, axis, radius);
    branch.closed = true;
    out->branches.push_back(branch);
    out->kind = ContactKind::Transversal;
    out->exact = true;
    out->note = "sphere/sphere: a circle";
    return true;
}

bool SphereCylinderCoaxial(const SphereSurface &sphere, const CylinderSurface &cylinder,
                           SurfaceIntersection *out, const IntersectOptions &options) {
    // Only the coaxial case is closed-form; otherwise the intersection is
    // a general quartic and belongs to the marcher.
    const Vec3d axis = cylinder.Axis().Normalized();
    const Vec3d to_center = sphere.Center() - cylinder.Origin();
    const Vec3d off_axis = to_center - axis * to_center.Dot(axis);
    if (off_axis.Length() > options.tolerance.linear) return false;

    const double r_cylinder = cylinder.Radius();
    const double r_sphere = sphere.Radius();
    if (r_cylinder > r_sphere + options.tolerance.linear) {
        out->kind = ContactKind::None;
        out->exact = true;
        return true;
    }
    if (r_cylinder >= r_sphere - options.tolerance.linear) {
        out->kind = ContactKind::Tangent;
        out->exact = true;
        out->note = "the cylinder touches the sphere's equator";
        return true;
    }
    const double half = std::sqrt(r_sphere * r_sphere - r_cylinder * r_cylinder);
    for (int side = -1; side <= 1; side += 2) {
        IntersectionBranch branch;
        branch.curve = MakeCircle(sphere.Center() + axis * (half * static_cast<double>(side)), axis, r_cylinder);
        branch.closed = true;
        out->branches.push_back(branch);
    }
    out->kind = ContactKind::Transversal;
    out->exact = true;
    out->note = "coaxial sphere/cylinder: two circles";
    return true;
}

bool CylinderCylinder(const CylinderSurface &a, const CylinderSurface &b, const Surface &sa, const Surface &sb,
                      SurfaceIntersection *out, const IntersectOptions &options) {
    const Vec3d axis_a = a.Axis().Normalized();
    const Vec3d axis_b = b.Axis().Normalized();
    if (axis_a.Cross(axis_b).Length() > options.tolerance.angular) return false;  // skew: the marcher's job

    // Parallel axes: the problem drops to two circles in the plane
    // perpendicular to them.
    const Vec3d between = b.Origin() - a.Origin();
    const Vec3d offset = between - axis_a * between.Dot(axis_a);
    const double distance = offset.Length();
    const double ra = a.Radius();
    const double rb = b.Radius();
    if (distance <= options.tolerance.linear) {
        out->kind = (std::fabs(ra - rb) <= options.tolerance.linear) ? ContactKind::Coincident
                                                                     : ContactKind::None;
        out->exact = true;
        out->note = "coaxial cylinders";
        return true;
    }
    if (distance > ra + rb + options.tolerance.linear ||
        distance < std::fabs(ra - rb) - options.tolerance.linear) {
        out->kind = ContactKind::None;
        out->exact = true;
        return true;
    }
    if (distance >= ra + rb - options.tolerance.linear ||
        distance <= std::fabs(ra - rb) + options.tolerance.linear) {
        out->kind = ContactKind::Tangent;
        out->exact = true;
        out->note = "parallel cylinders touch along one line";
        return true;
    }
    const Vec3d unit = offset / distance;
    const Vec3d across = axis_a.Cross(unit).Normalized();
    const double along = (distance * distance + ra * ra - rb * rb) / (2.0 * distance);
    const double half = std::sqrt(std::max(0.0, ra * ra - along * along));
    const double extent = ModelExtent(sa, sb);
    for (int side = -1; side <= 1; side += 2) {
        const Vec3d origin = a.Origin() + unit * along + across * (half * static_cast<double>(side));
        double lo = 0.0;
        double hi = 0.0;
        if (!ClipLineToSurfaces(origin, axis_a, sa, sb, options, extent, &lo, &hi)) continue;
        IntersectionBranch branch;
        branch.curve = std::make_shared<Line3>(Line3::FromPoints(origin + axis_a * lo, origin + axis_a * hi));
        out->branches.push_back(branch);
    }
    out->kind = out->branches.empty() ? ContactKind::None : ContactKind::Transversal;
    out->exact = true;
    out->note = "parallel cylinders: two lines";
    return true;
}

bool PlaneTorus(const PlaneSurface &plane, const TorusSurface &torus, SurfaceIntersection *out,
                const IntersectOptions &options) {
    const PlaneForm form = PlaneOf(plane);
    // Only the two axis-aligned families are closed-form. A general plane
    // cuts a torus in a quartic -- including the Villarceau circles at one
    // special angle -- and that is the marcher's business.
    const Vec3d unit_axis = torus.Axis().Normalized();
    const double alignment = form.normal.Dot(unit_axis);
    const double major = torus.MajorRadius();
    const double minor = torus.MinorRadius();

    if (std::fabs(std::fabs(alignment) - 1.0) <= options.tolerance.angular) {
        // Perpendicular to the axis: zero, one or two concentric circles.
        const double height = form.normal.Dot(torus.Center()) - form.offset;
        const double distance = std::fabs(height);
        if (distance > minor + options.tolerance.linear) {
            out->kind = ContactKind::None;
            out->exact = true;
            return true;
        }
        const Vec3d center = torus.Center() - form.normal * height;
        if (distance >= minor - options.tolerance.linear) {
            IntersectionBranch branch;
            branch.curve = MakeCircle(center, unit_axis, major);
            branch.closed = true;
            out->branches.push_back(branch);
            out->kind = ContactKind::Tangent;
            out->exact = true;
            out->note = "plane grazes the torus in one circle";
            return true;
        }
        const double half = std::sqrt(minor * minor - distance * distance);
        for (int side = -1; side <= 1; side += 2) {
            IntersectionBranch branch;
            branch.curve = MakeCircle(center, unit_axis, major + half * static_cast<double>(side));
            branch.closed = true;
            out->branches.push_back(branch);
        }
        out->kind = ContactKind::Transversal;
        out->exact = true;
        out->note = "plane/torus: two concentric circles";
        return true;
    }
    if (std::fabs(alignment) <= options.tolerance.angular) {
        // Containing the axis: two circles of the tube's own radius.
        const double axis_offset = form.normal.Dot(torus.Center()) - form.offset;
        if (std::fabs(axis_offset) > options.tolerance.linear) {
            // Parallel to the axis but offset: a quartic, not closed form.
            return false;
        }
        const Vec3d across = unit_axis.Cross(form.normal).Normalized();
        for (int side = -1; side <= 1; side += 2) {
            IntersectionBranch branch;
            branch.curve =
                MakeCircle(torus.Center() + across * (major * static_cast<double>(side)), form.normal, minor);
            branch.closed = true;
            out->branches.push_back(branch);
        }
        out->kind = ContactKind::Transversal;
        out->exact = true;
        out->note = "plane through the axis: two circles";
        return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------
// C.1
// ---------------------------------------------------------------------

std::vector<CurveCurveHit> IntersectCurves(const Curve3 &a, const Curve3 &b, const IntersectOptions &options) {
    std::vector<CurveCurveHit> hits;
    double a_lo = 0.0, a_hi = 0.0, b_lo = 0.0, b_hi = 0.0;
    a.Domain(&a_lo, &a_hi);
    b.Domain(&b_lo, &b_hi);
    if (!(a_hi > a_lo) || !(b_hi > b_lo)) return hits;

    // Cheap rejection first. A curve's bounds may be loose (a NURBS
    // returns its control polygon's box) but they always contain it, so a
    // miss here is a genuine miss.
    if (!a.Bounds().Overlaps(b.Bounds(), options.tolerance.linear)) return hits;

    // The approach is minimisation of the distance from a point on `a` to
    // the whole of `b`, not a Newton solve of the two stationarity
    // conditions. The Newton form is faster and fails exactly where it
    // matters most: at a tangency the two tangents are parallel, its
    // Jacobian is singular, and it reports no hit at all -- which is how
    // a line tangent to a circle came to be missed entirely. Brent
    // minimisation has no such degeneracy, and the projection it builds
    // on is the one already verified against brute force in Part A.5.
    const int samples = std::max(8, options.samples);
    std::vector<double> distances(Idx(samples + 1));
    for (int i = 0; i <= samples; ++i) {
        const double t = a_lo + (a_hi - a_lo) * static_cast<double>(i) / static_cast<double>(samples);
        distances[Idx(i)] = DistanceToCurve(a, b, t, options.tolerance, nullptr);
    }

    int coincident_samples = 0;
    for (double d : distances) {
        if (d <= options.tolerance.linear) ++coincident_samples;
    }
    // Most samples coinciding means the curves share a stretch, not that
    // they cross many times. Reporting a hundred "crossings" along an
    // overlap is the classic way this goes wrong.
    if (coincident_samples > samples / 2) {
        CurveCurveHit hit;
        hit.kind = ContactKind::Coincident;
        hit.t1 = a_lo;
        hit.t1_end = a_hi;
        hit.t2 = b_lo;
        hit.t2_end = b_hi;
        hit.point = a.Point(0.5 * (a_lo + a_hi));
        hits.push_back(hit);
        return hits;
    }

    auto parameter = [&](int i) {
        return a_lo + (a_hi - a_lo) * static_cast<double>(i) / static_cast<double>(samples);
    };
    for (int i = 0; i <= samples; ++i) {
        const bool left_ok = (i == 0) || distances[Idx(i)] <= distances[Idx(i - 1)];
        const bool right_ok = (i == samples) || distances[Idx(i)] <= distances[Idx(i + 1)];
        if (!left_ok || !right_ok) continue;

        // Refine this local minimum over the bracketing interval.
        const double lo = parameter(std::max(0, i - 1));
        const double hi = parameter(std::min(samples, i + 1));
        double t1 = parameter(i);
        double value = distances[Idx(i)];
        if (hi > lo) {
            SolveOptions solve;
            solve.max_iterations = options.max_iterations;
            solve.x_tol = options.tolerance.linear * 1e-3;
            double arg = t1;
            double minimum = value;
            BrentMinimize([&](double t) { return DistanceToCurve(a, b, t, options.tolerance, nullptr); }, lo,
                          hi, &arg, &minimum, solve);
            if (minimum <= value) {
                t1 = arg;
                value = minimum;
            }
        }
        if (value > options.tolerance.linear) continue;

        double t2 = 0.0;
        DistanceToCurve(a, b, t1, options.tolerance, &t2);
        CurveCurveHit hit;
        hit.t1 = t1;
        hit.t2 = t2;
        hit.point = (a.Point(t1) + b.Point(t2)) * 0.5;
        // Tangential when the two tangents are parallel there: the curves
        // touch without crossing, which a boolean must not treat as a
        // transversal split.
        hit.kind = options.tolerance.ParallelDirection(a.Tangent(t1), b.Tangent(t2))
                       ? ContactKind::Tangent
                       : ContactKind::Transversal;
        AddCurveHit(hits, hit, options.tolerance);
    }
    return hits;
}

std::vector<CurveSurfaceHit> IntersectCurveSurface(const Curve3 &curve, const Surface &surface,
                                                   const IntersectOptions &options) {
    std::vector<CurveSurfaceHit> hits;
    double t_lo = 0.0;
    double t_hi = 0.0;
    curve.Domain(&t_lo, &t_hi);
    if (!(t_hi > t_lo)) return hits;
    if (!curve.Bounds().Overlaps(surface.Bounds(), options.tolerance.linear)) return hits;

    // Same structure and same reasoning as IntersectCurves: minimise the
    // distance rather than Newton-solve C(t) = S(u,v), because at a
    // tangency the root is a double root and Newton's Jacobian is
    // singular there. A grazing hit is a real and important case -- it is
    // exactly what a boolean must not mistake for a crossing.
    auto distance_at = [&](double t) {
        double u = 0.0;
        double v = 0.0;
        Vec3d on_surface;
        if (!surface.ClosestPoint(curve.Point(t), &u, &v, &on_surface, options.tolerance)) return 1e300;
        return (curve.Point(t) - on_surface).Length();
    };

    const int samples = std::max(8, options.samples);
    std::vector<double> distances(Idx(samples + 1));
    for (int i = 0; i <= samples; ++i) {
        const double t = t_lo + (t_hi - t_lo) * static_cast<double>(i) / static_cast<double>(samples);
        distances[Idx(i)] = distance_at(t);
    }
    auto parameter = [&](int i) {
        return t_lo + (t_hi - t_lo) * static_cast<double>(i) / static_cast<double>(samples);
    };
    for (int i = 0; i <= samples; ++i) {
        const bool left_ok = (i == 0) || distances[Idx(i)] <= distances[Idx(i - 1)];
        const bool right_ok = (i == samples) || distances[Idx(i)] <= distances[Idx(i + 1)];
        if (!left_ok || !right_ok) continue;
        const double lo = parameter(std::max(0, i - 1));
        const double hi = parameter(std::min(samples, i + 1));
        double t = parameter(i);
        double value = distances[Idx(i)];
        if (hi > lo) {
            SolveOptions solve;
            solve.max_iterations = options.max_iterations;
            solve.x_tol = options.tolerance.linear * 1e-3;
            double arg = t;
            double minimum = value;
            BrentMinimize(distance_at, lo, hi, &arg, &minimum, solve);
            if (minimum <= value) {
                t = arg;
                value = minimum;
            }
        }
        if (value > options.tolerance.linear) continue;

        double u = 0.0;
        double v = 0.0;
        Vec3d on_surface;
        if (!surface.ClosestPoint(curve.Point(t), &u, &v, &on_surface, options.tolerance)) continue;

        // Polish with Newton on C(t) = S(u,v). Brent found the right
        // basin robustly -- including at a tangency, where Newton cannot
        // -- but it is minimising a distance that has a corner at a
        // transversal crossing, so it converges only linearly and stops
        // at roughly the acceptance tolerance. Newton from here is
        // quadratic and reaches machine precision in three or four
        // steps; where its Jacobian is singular (exactly the tangential
        // case) it declines and Brent's answer stands.
        {
            double pt = t;
            double pu = u;
            double pv = v;
            std::vector<Vec3d> dc;
            std::vector<std::vector<Vec3d>> ds;
            bool ok = true;
            for (int step = 0; step < 8 && ok; ++step) {
                curve.Derivatives(pt, 1, &dc);
                surface.Derivatives(pu, pv, 1, &ds);
                const Vec3d residual = dc[0] - ds[0][0];
                if (residual.Length() <= 1e-15) break;
                const Mat3d jacobian = Mat3d::FromColumns(dc[1], -ds[1][0], -ds[0][1]);
                Vec3d delta;
                if (!jacobian.Solve(-residual, &delta, options.tolerance)) {
                    ok = false;
                    break;
                }
                if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
                    ok = false;
                    break;
                }
                pt = Clamp(pt + delta.x, t_lo, t_hi);
                pu += delta.y;
                pv += delta.z;
            }
            if (ok && (curve.Point(pt) - surface.Point(pu, pv)).Length() <
                          (curve.Point(t) - on_surface).Length()) {
                t = pt;
                u = pu;
                v = pv;
            }
        }
        const Vec3d point = curve.Point(t);
        bool duplicate = false;
        for (const CurveSurfaceHit &existing : hits) {
            if (options.tolerance.SamePoint(existing.point, point)) duplicate = true;
        }
        if (duplicate) continue;
        CurveSurfaceHit hit;
        hit.t = t;
        hit.u = u;
        hit.v = v;
        hit.point = point;
        // Tangential when the curve grazes the surface -- its tangent lies
        // in the tangent plane.
        hit.kind = (std::fabs(curve.Tangent(t).Dot(surface.Normal(u, v))) <= options.tolerance.angular)
                       ? ContactKind::Tangent
                       : ContactKind::Transversal;
        hits.push_back(hit);
    }
    return hits;
}

// ---------------------------------------------------------------------
// C.2
// ---------------------------------------------------------------------

bool IntersectSurfacesAnalytic(const Surface &a, const Surface &b, SurfaceIntersection *out,
                               const IntersectOptions &options) {
    out->branches.clear();
    out->exact = false;
    out->note.clear();

    // Each pair is tried in both orders, so every case is written once.
    for (int swap = 0; swap < 2; ++swap) {
        const Surface &first = (swap == 0) ? a : b;
        const Surface &second = (swap == 0) ? b : a;
        const PlaneSurface *plane = As<PlaneSurface>(first, SurfaceKind::Plane);
        if (plane != nullptr) {
            if (const PlaneSurface *other = As<PlaneSurface>(second, SurfaceKind::Plane)) {
                return PlanePlane(*plane, *other, first, second, out, options);
            }
            if (const SphereSurface *sphere = As<SphereSurface>(second, SurfaceKind::Sphere)) {
                return PlaneSphere(*plane, *sphere, out, options);
            }
            if (const CylinderSurface *cylinder = As<CylinderSurface>(second, SurfaceKind::Cylinder)) {
                return PlaneCylinder(*plane, *cylinder, first, second, out, options);
            }
            if (const ConeSurface *cone = As<ConeSurface>(second, SurfaceKind::Cone)) {
                if (PlaneCone(*plane, *cone, out, options)) return true;
                continue;
            }
            if (const TorusSurface *torus = As<TorusSurface>(second, SurfaceKind::Torus)) {
                if (PlaneTorus(*plane, *torus, out, options)) return true;
                continue;
            }
        }
        if (const SphereSurface *sphere = As<SphereSurface>(first, SurfaceKind::Sphere)) {
            if (const SphereSurface *other = As<SphereSurface>(second, SurfaceKind::Sphere)) {
                return SphereSphere(*sphere, *other, out, options);
            }
            if (const CylinderSurface *cylinder = As<CylinderSurface>(second, SurfaceKind::Cylinder)) {
                if (SphereCylinderCoaxial(*sphere, *cylinder, out, options)) return true;
                continue;
            }
        }
        if (const CylinderSurface *cylinder = As<CylinderSurface>(first, SurfaceKind::Cylinder)) {
            if (const CylinderSurface *other = As<CylinderSurface>(second, SurfaceKind::Cylinder)) {
                if (CylinderCylinder(*cylinder, *other, first, second, out, options)) return true;
                continue;
            }
        }
    }
    return false;
}

bool BuildPCurveOnSurface(const Surface &surface, const Curve3 &curve, double t_start, double t_end,
                          const IntersectOptions &options, std::shared_ptr<const Curve3> *out) {
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    surface.Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    const double period_u = surface.IsClosedU(options.tolerance) ? (u_hi - u_lo) : 0.0;
    const double period_v = surface.IsClosedV(options.tolerance) ? (v_hi - v_lo) : 0.0;
    auto unwrap = [](double value, double reference, double period) {
        if (period <= 0.0) return value;
        return value - period * std::round((value - reference) / period);
    };

    const int samples = std::max(8, options.samples);
    std::vector<Vec3d> lifted;
    Vec2d previous;
    bool have_previous = false;
    for (int i = 0; i <= samples; ++i) {
        const double t = t_start + (t_end - t_start) * static_cast<double>(i) / static_cast<double>(samples);
        const Vec3d point = curve.Point(t);
        double u = 0.0;
        double v = 0.0;
        Vec3d on_surface;
        if (!surface.ClosestPoint(point, &u, &v, &on_surface, options.tolerance)) return false;
        if (!options.tolerance.SamePoint(point, on_surface)) return false;
        if (have_previous) {
            u = unwrap(u, previous.x, period_u);
            v = unwrap(v, previous.y, period_v);
        }
        previous = Vec2d{u, v};
        have_previous = true;
        lifted.push_back(Vec3d{u, v, 0.0});
    }
    NurbsCurve3 fitted;
    if (!NurbsCurve3::Interpolate(lifted, 3, Parameterization::Centripetal, &fitted)) return false;
    *out = std::make_shared<NurbsCurve3>(fitted);
    return true;
}

SurfaceIntersection IntersectSurfaces(const Surface &a, const Surface &b, const IntersectOptions &options) {
    SurfaceIntersection result;
    if (IntersectSurfacesAnalytic(a, b, &result, options)) return result;
    return MarchSurfaces(a, b, options);
}

}  // namespace cad

// ---------------------------------------------------------------------
// C.3 The general marcher
// ---------------------------------------------------------------------

namespace cad {
namespace {

// One point on the intersection, carrying its coordinates in both
// parameter spaces as well as in space. All three are needed: the 3D
// point becomes the edge, and the two parameter traces become the
// p-curves that let each face be trimmed by it.
struct MarchPoint {
    Vec3d position;
    Vec2d uv_a;
    Vec2d uv_b;
};

// Newton correction back onto both surfaces, constrained to a plane.
//
// The unconstrained system A(ua,va) = B(ub,vb) is three equations in four
// unknowns -- underdetermined, because the solution set *is* the
// intersection curve. Adding the constraint that the point lies in a
// given plane picks one point on that curve, and choosing the plane
// perpendicular to the direction of travel is what makes the marcher step
// forward rather than slide along.
bool CorrectToIntersection(const Surface &a, const Surface &b, const Vec3d &anchor, const Vec3d &plane_normal,
                           MarchPoint *point, const IntersectOptions &options) {
    double a_u_lo = 0.0, a_u_hi = 0.0, a_v_lo = 0.0, a_v_hi = 0.0;
    double b_u_lo = 0.0, b_u_hi = 0.0, b_v_lo = 0.0, b_v_hi = 0.0;
    a.Domain(&a_u_lo, &a_u_hi, &a_v_lo, &a_v_hi);
    b.Domain(&b_u_lo, &b_u_hi, &b_v_lo, &b_v_hi);

    std::vector<std::vector<Vec3d>> da;
    std::vector<std::vector<Vec3d>> db;
    for (int iteration = 0; iteration < options.max_iterations; ++iteration) {
        a.Derivatives(point->uv_a.x, point->uv_a.y, 1, &da);
        b.Derivatives(point->uv_b.x, point->uv_b.y, 1, &db);
        const Vec3d gap = da[0][0] - db[0][0];
        const double plane_residual = (da[0][0] - anchor).Dot(plane_normal);
        if (gap.Length() <= options.tolerance.linear * 1e-2 &&
            std::fabs(plane_residual) <= options.tolerance.linear * 1e-2) {
            break;
        }
        MatrixNd jacobian(4, 4);
        const Vec3d au = da[1][0];
        const Vec3d av = da[0][1];
        const Vec3d bu = db[1][0];
        const Vec3d bv = db[0][1];
        for (int r = 0; r < 3; ++r) {
            jacobian(static_cast<std::size_t>(r), 0) = au[static_cast<std::size_t>(r)];
            jacobian(static_cast<std::size_t>(r), 1) = av[static_cast<std::size_t>(r)];
            jacobian(static_cast<std::size_t>(r), 2) = -bu[static_cast<std::size_t>(r)];
            jacobian(static_cast<std::size_t>(r), 3) = -bv[static_cast<std::size_t>(r)];
        }
        jacobian(3, 0) = au.Dot(plane_normal);
        jacobian(3, 1) = av.Dot(plane_normal);
        jacobian(3, 2) = 0.0;
        jacobian(3, 3) = 0.0;
        std::vector<double> rhs = {-gap.x, -gap.y, -gap.z, -plane_residual};
        std::vector<double> step;
        if (!jacobian.SolveLU(rhs, &step)) return false;
        bool finite = true;
        for (double value : step) {
            if (!std::isfinite(value)) finite = false;
        }
        if (!finite) return false;
        point->uv_a.x += step[0];
        point->uv_a.y += step[1];
        point->uv_b.x += step[2];
        point->uv_b.y += step[3];
    }
    point->position = a.Point(point->uv_a.x, point->uv_a.y);
    // Converged only if the two surfaces really do meet here.
    return options.tolerance.SamePoint(point->position, b.Point(point->uv_b.x, point->uv_b.y));
}

// Is the point still on the surface, allowing for periodicity?
//
// A closed surface's parameter domain has sides that are not edges of the
// surface at all, only of its parameterization. A cylinder's u = 0 and
// u = 2*pi name the same points; a curve crossing there has not left the
// cylinder. Treating that as a domain exit is what made an intersection
// circle come back as two half-circles -- two spheres meet in a circle
// that crosses the seam twice, and the march stopped dead at each
// crossing.
//
// The parameters themselves are deliberately *not* wrapped: the trace
// they feed becomes a p-curve, which has to be continuous, and wrapping
// mid-curve would tear it. Evaluating a periodic surface outside its
// nominal range is well defined anyway -- the trigonometry is periodic --
// so the unwrapped value is both continuous and correct.
bool InDomain(const Surface &surface, const Vec2d &uv, double slack) {
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    surface.Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    const Tolerance tolerance;
    if (!surface.IsClosedU(tolerance)) {
        if (uv.x < u_lo - slack || uv.x > u_hi + slack) return false;
    }
    if (!surface.IsClosedV(tolerance)) {
        if (uv.y < v_lo - slack || uv.y > v_hi + slack) return false;
    }
    return true;
}

// The intersection's tangent: perpendicular to both normals, so the cross
// product of them. Zero-length exactly where the surfaces are tangent,
// which is where marching cannot continue and the branch ends.
Vec3d IntersectionTangent(const Surface &a, const Surface &b, const MarchPoint &point) {
    const Vec3d na = a.Normal(point.uv_a.x, point.uv_a.y);
    const Vec3d nb = b.Normal(point.uv_b.x, point.uv_b.y);
    return na.Cross(nb);
}

// Walks one branch from a seed, in one direction. Returns the points
// traced, and says whether the branch closed on itself.
std::vector<MarchPoint> MarchFrom(const Surface &a, const Surface &b, const MarchPoint &seed, int sign,
                                  double scale, const IntersectOptions &options, bool *out_closed) {
    std::vector<MarchPoint> trace;
    *out_closed = false;
    MarchPoint current = seed;
    trace.push_back(current);

    double step = scale * options.max_step_fraction * 0.25;
    const double min_step = scale * options.min_step_fraction;
    const double max_step = scale * options.max_step_fraction;
    const double slack = options.tolerance.linear * 10.0;

    for (int i = 0; i < options.max_points_per_branch; ++i) {
        const Vec3d tangent = IntersectionTangent(a, b, current);
        if (tangent.Length() <= 1e-12) break;  // tangential contact: no way forward
        const Vec3d direction = tangent.Normalized() * static_cast<double>(sign);

        bool advanced = false;
        for (int attempt = 0; attempt < 12 && !advanced; ++attempt) {
            MarchPoint candidate = current;
            const Vec3d predicted = current.position + direction * step;
            // Seed the correction with a first-order guess in each
            // parameter space, so Newton starts close and converges in
            // two or three steps rather than wandering.
            std::vector<std::vector<Vec3d>> da;
            std::vector<std::vector<Vec3d>> db;
            a.Derivatives(current.uv_a.x, current.uv_a.y, 1, &da);
            b.Derivatives(current.uv_b.x, current.uv_b.y, 1, &db);
            const Vec3d motion = direction * step;
            double e = 0.0, f = 0.0, g = 0.0;
            a.FirstFundamentalForm(current.uv_a.x, current.uv_a.y, &e, &f, &g);
            const double det_a = e * g - f * f;
            if (det_a > 0.0) {
                const double pu = motion.Dot(da[1][0]);
                const double pv = motion.Dot(da[0][1]);
                candidate.uv_a.x += (g * pu - f * pv) / det_a;
                candidate.uv_a.y += (e * pv - f * pu) / det_a;
            }
            b.FirstFundamentalForm(current.uv_b.x, current.uv_b.y, &e, &f, &g);
            const double det_b = e * g - f * f;
            if (det_b > 0.0) {
                const double pu = motion.Dot(db[1][0]);
                const double pv = motion.Dot(db[0][1]);
                candidate.uv_b.x += (g * pu - f * pv) / det_b;
                candidate.uv_b.y += (e * pv - f * pu) / det_b;
            }

            if (!CorrectToIntersection(a, b, predicted, direction, &candidate, options)) {
                step = std::max(min_step, step * 0.5);
                if (step <= min_step * 1.001) break;
                continue;
            }
            // How far the straight step missed the true curve. The
            // midpoint of the chord, corrected back onto the
            // intersection, is the cheapest honest estimate of that.
            MarchPoint midpoint = current;
            const Vec3d chord_middle = (current.position + candidate.position) * 0.5;
            if (CorrectToIntersection(a, b, chord_middle, direction, &midpoint, options)) {
                const double deviation = (midpoint.position - chord_middle).Length();
                if (deviation > options.march_tolerance && step > min_step * 1.001) {
                    step = std::max(min_step, step * 0.5);
                    continue;
                }
                if (deviation < options.march_tolerance * 0.1) {
                    step = std::min(max_step, step * 1.5);
                }
            }
            current = candidate;
            advanced = true;
        }
        if (!advanced) break;

        // Left one of the parameter domains? The branch ends there. The
        // exit point is not refined onto the boundary here -- Part C.4
        // trims branches against the faces' real trimming loops anyway,
        // and doing it twice would only disagree.
        if (!InDomain(a, current.uv_a, slack) || !InDomain(b, current.uv_b, slack)) {
            trace.push_back(current);
            break;
        }
        // Closed on itself? Only after enough points that the test cannot
        // fire on the first step back toward the seed.
        // Closed on itself? Compared in space rather than in parameters,
        // so a branch that has crossed a seam still recognises its own
        // start. The threshold tracks the step actually taken, with
        // enough margin that stepping *past* the seed is still caught.
        if (trace.size() > 6 && (current.position - seed.position).Length() <= step * 1.25) {
            *out_closed = true;
            break;
        }
        trace.push_back(current);
    }
    return trace;
}

}  // namespace

SurfaceIntersection MarchSurfaces(const Surface &a, const Surface &b, const IntersectOptions &options) {
    SurfaceIntersection result;
    result.exact = false;
    if (!a.Bounds().Overlaps(b.Bounds(), options.tolerance.linear)) {
        result.kind = ContactKind::None;
        result.note = "bounding boxes do not overlap";
        return result;
    }
    Box3d combined = a.Bounds();
    combined.Expand(b.Bounds());
    const double scale = std::max(1e-9, combined.Diagonal());

    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    a.Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    const int grid = std::max(4, options.seed_grid);

    // Seeding. The signed distance from a point of A to B -- the gap
    // measured along B's normal -- changes sign exactly across the
    // intersection, so a sign change between neighbouring grid samples
    // brackets a point on it.
    std::vector<double> signed_gap(static_cast<std::size_t>((grid + 1) * (grid + 1)), 0.0);
    std::vector<Vec2d> projected(static_cast<std::size_t>((grid + 1) * (grid + 1)));
    std::vector<char> valid(static_cast<std::size_t>((grid + 1) * (grid + 1)), 0);
    auto index = [grid](int i, int j) { return static_cast<std::size_t>(i * (grid + 1) + j); };
    for (int i = 0; i <= grid; ++i) {
        for (int j = 0; j <= grid; ++j) {
            const double u = u_lo + (u_hi - u_lo) * static_cast<double>(i) / static_cast<double>(grid);
            const double v = v_lo + (v_hi - v_lo) * static_cast<double>(j) / static_cast<double>(grid);
            const Vec3d p = a.Point(u, v);
            double bu = 0.0;
            double bv = 0.0;
            Vec3d on_b;
            if (!b.ClosestPoint(p, &bu, &bv, &on_b, options.tolerance)) continue;
            const Vec3d normal = b.Normal(bu, bv);
            if (normal.LengthSquared() <= 0.0) continue;
            signed_gap[index(i, j)] = (p - on_b).Dot(normal);
            projected[index(i, j)] = Vec2d{bu, bv};
            valid[index(i, j)] = 1;
        }
    }

    std::vector<MarchPoint> seeds;
    for (int i = 0; i <= grid; ++i) {
        for (int j = 0; j <= grid; ++j) {
            if (!valid[index(i, j)]) continue;
            const double here = signed_gap[index(i, j)];
            // Only the two forward neighbours, so each cell edge is
            // considered once.
            const int neighbours[2][2] = {{i + 1, j}, {i, j + 1}};
            for (const auto &n : neighbours) {
                if (n[0] > grid || n[1] > grid || !valid[index(n[0], n[1])]) continue;
                const double there = signed_gap[index(n[0], n[1])];
                const bool crosses = (here > 0.0) != (there > 0.0);
                const bool touches = std::fabs(here) <= options.tolerance.linear;
                if (!crosses && !touches) continue;
                // Bisect along the cell edge to land near the crossing.
                double lo = 0.0;
                double hi = 1.0;
                auto sample = [&](double t) {
                    const double u = u_lo + (u_hi - u_lo) *
                                                (static_cast<double>(i) + t * static_cast<double>(n[0] - i)) /
                                                static_cast<double>(grid);
                    const double v = v_lo + (v_hi - v_lo) *
                                                (static_cast<double>(j) + t * static_cast<double>(n[1] - j)) /
                                                static_cast<double>(grid);
                    return Vec2d{u, v};
                };
                if (crosses) {
                    for (int k = 0; k < 30; ++k) {
                        const double mid = 0.5 * (lo + hi);
                        const Vec2d uv = sample(mid);
                        const Vec3d p = a.Point(uv.x, uv.y);
                        double bu = 0.0;
                        double bv = 0.0;
                        Vec3d on_b;
                        if (!b.ClosestPoint(p, &bu, &bv, &on_b, options.tolerance)) break;
                        const double value = (p - on_b).Dot(b.Normal(bu, bv));
                        if ((value > 0.0) == (here > 0.0)) {
                            lo = mid;
                        } else {
                            hi = mid;
                        }
                    }
                }
                const Vec2d uv = sample(crosses ? 0.5 * (lo + hi) : 0.0);
                MarchPoint seed;
                seed.uv_a = uv;
                const Vec3d p = a.Point(uv.x, uv.y);
                double bu = 0.0;
                double bv = 0.0;
                if (!b.ClosestPoint(p, &bu, &bv, nullptr, options.tolerance)) continue;
                seed.uv_b = Vec2d{bu, bv};
                seed.position = p;
                // Correct onto the intersection proper, in the plane
                // perpendicular to the tangent there.
                const Vec3d tangent = IntersectionTangent(a, b, seed);
                if (tangent.Length() <= 1e-12) continue;
                if (!CorrectToIntersection(a, b, seed.position, tangent.Normalized(), &seed, options)) continue;
                if (!InDomain(a, seed.uv_a, options.tolerance.linear * 10.0) ||
                    !InDomain(b, seed.uv_b, options.tolerance.linear * 10.0)) {
                    continue;
                }
                seeds.push_back(seed);
            }
        }
    }
    if (seeds.empty()) {
        result.kind = ContactKind::None;
        result.note = "no seed point found on a " + std::to_string(grid) + "x" + std::to_string(grid) +
                      " grid; a branch smaller than one cell would be missed";
        return result;
    }

    // March each seed that is not already covered by a branch found
    // earlier. Without this the same curve is traced once per seed on it,
    // which for a long branch is dozens of duplicates.
    std::vector<std::vector<MarchPoint>> branches;
    auto already_covered = [&](const MarchPoint &seed) {
        for (const std::vector<MarchPoint> &branch : branches) {
            for (const MarchPoint &point : branch) {
                if ((point.position - seed.position).Length() <= scale * options.max_step_fraction * 0.5) {
                    return true;
                }
            }
        }
        return false;
    };

    for (const MarchPoint &seed : seeds) {
        if (already_covered(seed)) continue;
        bool closed = false;
        std::vector<MarchPoint> forward = MarchFrom(a, b, seed, 1, scale, options, &closed);
        std::vector<MarchPoint> branch;
        if (closed) {
            branch = std::move(forward);
            // Close it exactly. The march stops as soon as it recognises
            // the seed, which leaves the last step's worth of curve
            // untraced -- about half a per cent of a circle's length with
            // the default step. Repeating the seed as the final point
            // spans that gap and makes the fitted curve genuinely closed
            // rather than nearly so.
            branch.push_back(seed);
        } else {
            // Open branch: march the other way too and join, so the trace
            // covers the whole curve rather than half of it.
            bool backward_closed = false;
            std::vector<MarchPoint> backward = MarchFrom(a, b, seed, -1, scale, options, &backward_closed);
            std::reverse(backward.begin(), backward.end());
            branch = std::move(backward);
            if (!forward.empty()) branch.insert(branch.end(), forward.begin() + 1, forward.end());
        }
        if (branch.size() < 2) continue;
        branches.push_back(std::move(branch));
    }

    for (const std::vector<MarchPoint> &raw : branches) {
        // Thin the trace before fitting, and this is not an optimisation.
        //
        // The step adapts down to its minimum wherever the curve bends
        // hard or approaches a tangency, so a trace routinely contains
        // runs of points a few microns apart. Interpolating a cubic
        // through those makes the collocation matrix ill-conditioned --
        // centripetal parameterization gives them near-zero parameter
        // intervals -- and the resulting curve oscillates wildly *between*
        // the points it passes exactly through. Measured on a sphere met
        // by an offset cylinder, the fitted curve strayed 0.13 from a
        // surface every traced point was on to 1e-9.
        const double spacing = std::max(options.march_tolerance, scale * 1e-6);
        std::vector<MarchPoint> branch;
        for (const MarchPoint &point : raw) {
            if (!branch.empty() && (point.position - branch.back().position).Length() < spacing) continue;
            branch.push_back(point);
        }
        // Always keep the true end, or the branch is silently shortened.
        if (!raw.empty() && (branch.empty() || (branch.back().position - raw.back().position).Length() > 0.0)) {
            branch.push_back(raw.back());
        }
        if (branch.size() < 2) continue;

        std::vector<Vec3d> points;
        std::vector<Vec3d> trace_a;
        std::vector<Vec3d> trace_b;
        points.reserve(branch.size());
        for (const MarchPoint &point : branch) {
            points.push_back(point.position);
            trace_a.push_back(Vec3d{point.uv_a.x, point.uv_a.y, 0.0});
            trace_b.push_back(Vec3d{point.uv_b.x, point.uv_b.y, 0.0});
        }
        IntersectionBranch fitted;
        NurbsCurve3 curve;
        const int degree = std::min(3, static_cast<int>(points.size()) - 1);
        if (degree < 1) continue;
        if (!NurbsCurve3::Interpolate(points, degree, Parameterization::Centripetal, &curve)) continue;

        // Verify the fit against the surfaces it is supposed to lie on,
        // rather than trusting that passing through the traced points is
        // enough. A curve can interpolate every point and still bulge off
        // the surface between them, and a branch that does is worse than
        // useless to a boolean.
        double worst = 0.0;
        {
            double lo = 0.0;
            double hi = 0.0;
            curve.Domain(&lo, &hi);
            for (int k = 0; k <= 128; ++k) {
                const Vec3d p = curve.Point(lo + (hi - lo) * static_cast<double>(k) / 128.0);
                double u = 0.0;
                double v = 0.0;
                Vec3d on_a;
                Vec3d on_b;
                a.ClosestPoint(p, &u, &v, &on_a, options.tolerance);
                b.ClosestPoint(p, &u, &v, &on_b, options.tolerance);
                worst = std::max(worst, std::max((p - on_a).Length(), (p - on_b).Length()));
            }
        }
        if (worst > options.march_tolerance * 10.0) {
            result.note += " [a branch's fit deviates " + std::to_string(worst) +
                           " from the surfaces and was dropped]";
            continue;
        }

        fitted.curve = std::make_shared<NurbsCurve3>(curve);
        NurbsCurve3 pcurve_a;
        NurbsCurve3 pcurve_b;
        if (NurbsCurve3::Interpolate(trace_a, degree, Parameterization::Centripetal, &pcurve_a)) {
            fitted.pcurve_a = std::make_shared<NurbsCurve3>(pcurve_a);
        }
        if (NurbsCurve3::Interpolate(trace_b, degree, Parameterization::Centripetal, &pcurve_b)) {
            fitted.pcurve_b = std::make_shared<NurbsCurve3>(pcurve_b);
        }
        fitted.closed = (points.front() - points.back()).Length() <= scale * options.max_step_fraction;
        result.branches.push_back(fitted);
    }
    if (result.branches.empty()) {
        // Seeds were found but nothing survived. The surfaces do meet --
        // that is what a seed is -- so this is a contact the marcher
        // could not trace, which in practice means a tangency or a
        // singular point on the intersection curve. Saying None here
        // would tell a boolean the two are disjoint, which is worse than
        // useless; Tangent says "they touch and I cannot give you a curve
        // to cut along", which a caller can act on.
        result.kind = ContactKind::Tangent;
        result.note = "found " + std::to_string(seeds.size()) +
                      " seed point(s) but could not trace a usable branch; the intersection is most likely "
                      "tangential or singular there" + result.note;
        return result;
    }
    result.kind = ContactKind::Transversal;
    result.note = "marched " + std::to_string(result.branches.size()) + " branch(es)" + result.note;
    return result;
}

}  // namespace cad
