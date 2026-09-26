#include "cad_curve.h"

#include <algorithm>
#include <cmath>

namespace cad {
namespace {

inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// The point where two coplanar lines meet, as a parameter along the first.
// Returns false when they are parallel. Used by the arc-to-NURBS
// construction, where the two lines are tangents at the ends of a span
// and their meeting point is the middle control point.
bool IntersectLines(const Vec3d &p0, const Vec3d &t0, const Vec3d &p2, const Vec3d &t2, Vec3d *out) {
    // Solve p0 + a*t0 = p2 + b*t2 in the least-squares sense, which for
    // genuinely coplanar input is exact and for input that has drifted
    // slightly out of plane gives the nearest thing to an answer.
    const Vec3d w = p2 - p0;
    const double a = t0.Dot(t0);
    const double b = t0.Dot(t2);
    const double c = t2.Dot(t2);
    const double d = t0.Dot(w);
    const double e = t2.Dot(w);
    const double denominator = a * c - b * b;
    if (std::fabs(denominator) <= 1e-300) return false;
    const double s = (d * c - e * b) / denominator;
    *out = p0 + t0 * s;
    return true;
}

// Expands `box` by the extreme points of r_x*cos(u)*X + r_y*sin(u)*Y over
// the angular range [start, end], exactly rather than by sampling. Along
// each world axis k the coordinate is r_x*X_k*cos(u) + r_y*Y_k*sin(u),
// whose stationary points are at u = atan2(r_y*Y_k, r_x*X_k) and that
// angle plus pi. Including only the ones inside the range is what makes
// an *arc*'s box tight rather than the whole conic's.
void ExpandConicBounds(Box3d *box, const Vec3d &center, const Vec3d &x_axis, const Vec3d &y_axis, double rx,
                       double ry, double start, double end) {
    auto point_at = [&](double u) { return center + x_axis * (rx * std::cos(u)) + y_axis * (ry * std::sin(u)); };
    box->Expand(point_at(start));
    box->Expand(point_at(end));
    for (int k = 0; k < 3; ++k) {
        const double ax = rx * x_axis[static_cast<std::size_t>(k)];
        const double ay = ry * y_axis[static_cast<std::size_t>(k)];
        if (ax == 0.0 && ay == 0.0) continue;
        const double base = std::atan2(ay, ax);
        for (int half = 0; half < 2; ++half) {
            const double candidate = base + static_cast<double>(half) * kPi;
            // The stationary angle is only defined modulo 2*pi; shift it
            // into the arc's own range before testing membership.
            double u = candidate;
            while (u < start) u += kTwoPi;
            while (u > end) u -= kTwoPi;
            if (u >= start && u <= end) box->Expand(point_at(u));
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------
// Curve3 derived quantities
// ---------------------------------------------------------------------

Vec3d Curve3::Tangent(double u) const {
    std::vector<Vec3d> ders;
    Derivatives(u, 1, &ders);
    return ders[1].Normalized();
}

double Curve3::Curvature(double u) const {
    std::vector<Vec3d> ders;
    Derivatives(u, 2, &ders);
    const double speed = ders[1].Length();
    if (speed <= 0.0) return 0.0;
    return ders[1].Cross(ders[2]).Length() / (speed * speed * speed);
}

Vec3d Curve3::Normal(double u) const {
    std::vector<Vec3d> ders;
    Derivatives(u, 2, &ders);
    const Vec3d tangent = ders[1].Normalized();
    // Gram-Schmidt: remove the tangential part of the acceleration, and
    // what is left points at the centre of curvature.
    const Vec3d normal = ders[2] - tangent * ders[2].Dot(tangent);
    return normal.Normalized();
}

double Curve3::Length(double from, double to, double tolerance) const {
    // Arc length is the integral of |C'(u)|, which is a square root and so
    // never a polynomial -- fixed-order quadrature would be wrong by an
    // unknown amount, which is why this one is adaptive even though most
    // integrals in this kernel are not.
    if (to < from) std::swap(from, to);
    return AdaptiveQuadrature(
        [this](double u) {
            std::vector<Vec3d> ders;
            Derivatives(u, 1, &ders);
            return ders[1].Length();
        },
        from, to, tolerance);
}

double Curve3::Length(double tolerance) const {
    double lo = 0.0;
    double hi = 0.0;
    Domain(&lo, &hi);
    return Length(lo, hi, tolerance);
}

bool Curve3::ClosestPoint(const Vec3d &point, double *out_u, Vec3d *out_point, const Tolerance &tolerance,
                          int sample_count) const {
    double lo = 0.0;
    double hi = 0.0;
    Domain(&lo, &hi);
    if (!(hi > lo) || sample_count < 2) return false;

    // Stage one: sample, and collect every *local* minimum rather than
    // only the global best. A curve that passes near the query point
    // twice has two basins, and Newton from the single best sample can
    // still slide into the other one; refining each candidate and
    // comparing at the end costs a few extra iterations and removes the
    // failure entirely.
    std::vector<double> parameters;
    std::vector<double> distances;
    parameters.reserve(Idx(sample_count + 1));
    distances.reserve(Idx(sample_count + 1));
    for (int i = 0; i <= sample_count; ++i) {
        const double u = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(sample_count);
        parameters.push_back(u);
        distances.push_back((Point(u) - point).LengthSquared());
    }
    std::vector<double> candidates;
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const bool left_ok = (i == 0) || distances[i] <= distances[i - 1];
        const bool right_ok = (i + 1 == parameters.size()) || distances[i] <= distances[i + 1];
        if (left_ok && right_ok) candidates.push_back(parameters[i]);
    }
    if (candidates.empty()) candidates.push_back(parameters[0]);

    const bool closed = IsClosed(tolerance);
    double best_u = candidates[0];
    double best_distance = (Point(best_u) - point).LengthSquared();

    for (double start : candidates) {
        double u = start;
        std::vector<Vec3d> ders;
        // Newton on f(u) = C'(u).(C(u) - P), whose root is the condition
        // that the residual is perpendicular to the tangent -- i.e. a
        // stationary point of the distance.
        for (int iteration = 0; iteration < 64; ++iteration) {
            Derivatives(u, 2, &ders);
            const Vec3d residual = ders[0] - point;
            const double f = ders[1].Dot(residual);
            const double df = ders[2].Dot(residual) + ders[1].Dot(ders[1]);
            const double newton_step = f / df;
            // Three ways this step is useless, all of them reachable:
            //
            //  * df is zero. This is not exotic -- query the centre of a
            //    circle and every point on it is equidistant, making
            //    C''.(C-P) = -r^2 exactly cancel |C'|^2 = r^2. The
            //    distance function is genuinely flat, there is no unique
            //    answer, and any point on the curve is correct.
            //  * The step is not finite, from df underflowing.
            //  * The step is larger than the entire domain. Rounding near
            //    the degenerate case above produces df ~ 1e-17 against
            //    f ~ 1e-16, and the resulting step of ~1e284 is not a
            //    refinement of anything.
            //
            // All three mean Newton has nothing to add, so the sampled
            // candidate stands. (An earlier version clamped or wrapped
            // such a step instead; wrapping it into the domain by
            // repeated addition took about 1e283 iterations, which is
            // how this case announced itself.)
            if (!std::isfinite(newton_step) || std::fabs(newton_step) > (hi - lo)) break;
            double next = u - newton_step;
            if (closed) {
                // Wrap rather than clamp: on a closed curve the domain
                // ends are not boundaries, and clamping there would stop
                // the iteration crossing the seam. Modular, not iterative,
                // so the cost does not depend on how far out `next` lands.
                const double span = hi - lo;
                if (span > 0.0) next -= span * std::floor((next - lo) / span);
                if (!std::isfinite(next)) break;
            } else {
                next = Clamp(next, lo, hi);
            }
            const double step = std::fabs(next - u);
            u = next;
            if (step <= tolerance.linear * 1e-3 || std::fabs(f) <= tolerance.linear * 1e-6) break;
        }
        const double distance = (Point(u) - point).LengthSquared();
        if (distance < best_distance) {
            best_distance = distance;
            best_u = u;
        }
    }
    // The domain ends are candidate minima in their own right for an open
    // curve -- the true closest point is frequently an endpoint, and no
    // stationary-point search will ever find it there.
    if (!closed) {
        for (double end : {lo, hi}) {
            const double distance = (Point(end) - point).LengthSquared();
            if (distance < best_distance) {
                best_distance = distance;
                best_u = end;
            }
        }
    }
    *out_u = best_u;
    if (out_point != nullptr) *out_point = Point(best_u);
    return true;
}

bool Curve3::ContainsPoint(const Vec3d &point, double *out_u, const Tolerance &tolerance) const {
    double u = 0.0;
    Vec3d on_curve;
    if (!ClosestPoint(point, &u, &on_curve, tolerance)) return false;
    if (out_u != nullptr) *out_u = u;
    return tolerance.SamePoint(on_curve, point);
}

void Curve3::Tessellate(std::vector<double> *out_parameters, std::vector<Vec3d> *out_points,
                        double chord_tolerance, double angle_tolerance, int max_samples) const {
    out_parameters->clear();
    out_points->clear();
    double lo = 0.0;
    double hi = 0.0;
    Domain(&lo, &hi);
    if (!(hi > lo)) return;

    // Seed at the curve's own break parameters so a kink is always a
    // segment endpoint, never straddled.
    std::vector<double> seeds;
    seeds.push_back(lo);
    for (double b : NaturalBreaks()) {
        if (b > lo && b < hi) seeds.push_back(b);
    }
    seeds.push_back(hi);
    std::sort(seeds.begin(), seeds.end());
    seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());

    out_parameters->push_back(seeds.front());
    out_points->push_back(Point(seeds.front()));

    // Explicit stack rather than recursion: the depth is bounded by
    // max_samples, not by the geometry, and a pathological curve should
    // exhaust a vector rather than the call stack.
    struct Segment {
        double u0, u1;
        int depth;
    };
    for (std::size_t s = 0; s + 1 < seeds.size(); ++s) {
        std::vector<Segment> stack;
        stack.push_back(Segment{seeds[s], seeds[s + 1], 0});
        while (!stack.empty()) {
            const Segment segment = stack.back();
            stack.pop_back();
            const Vec3d p0 = Point(segment.u0);
            const Vec3d p1 = Point(segment.u1);
            const double um = 0.5 * (segment.u0 + segment.u1);
            const Vec3d pm = Point(um);

            // Chord deviation: how far the true midpoint is from the
            // straight segment that would replace it.
            const Vec3d chord = p1 - p0;
            const double chord_length = chord.Length();
            double deviation = 0.0;
            if (chord_length > 0.0) {
                deviation = chord.Cross(pm - p0).Length() / chord_length;
            } else {
                deviation = (pm - p0).Length();
            }
            const Vec3d t0 = Tangent(segment.u0);
            const Vec3d t1 = Tangent(segment.u1);
            double angle = 0.0;
            if (t0.LengthSquared() > 0.0 && t1.LengthSquared() > 0.0) {
                angle = std::atan2(t0.Cross(t1).Length(), t0.Dot(t1));
            }
            const bool flat_enough = deviation <= chord_tolerance && angle <= angle_tolerance;
            const bool too_many = static_cast<int>(out_parameters->size()) >= max_samples;
            if (flat_enough || segment.depth >= 24 || too_many) {
                out_parameters->push_back(segment.u1);
                out_points->push_back(p1);
                continue;
            }
            // Pushed in reverse so the left half is processed first and
            // the output stays in increasing parameter order.
            stack.push_back(Segment{um, segment.u1, segment.depth + 1});
            stack.push_back(Segment{segment.u0, um, segment.depth + 1});
        }
    }
}

// ---------------------------------------------------------------------
// Line3
// ---------------------------------------------------------------------

Line3::Line3(const Vec3d &origin, const Vec3d &direction, double lo, double hi)
    : origin_(origin), direction_(direction), lo_(lo), hi_(hi) {}

Line3 Line3::FromPoints(const Vec3d &a, const Vec3d &b) { return Line3(a, b - a, 0.0, 1.0); }

std::unique_ptr<Curve3> Line3::Clone() const { return std::make_unique<Line3>(*this); }

void Line3::Domain(double *lo, double *hi) const {
    *lo = lo_;
    *hi = hi_;
}

Vec3d Line3::Point(double u) const { return origin_ + direction_ * u; }

void Line3::Derivatives(double u, int max_derivative, std::vector<Vec3d> *out) const {
    out->assign(Idx(max_derivative + 1), Vec3d{});
    (*out)[0] = Point(u);
    if (max_derivative >= 1) (*out)[1] = direction_;
    // Everything above the first derivative is exactly zero, which the
    // assign above already established.
}

Box3d Line3::Bounds() const {
    Box3d box;
    box.Expand(Point(lo_));
    box.Expand(Point(hi_));
    return box;
}

bool Line3::IsClosed(const Tolerance &tolerance) const {
    // Only a degenerate line is "closed", and that is worth reporting
    // honestly rather than answering a flat no.
    return tolerance.SamePoint(Point(lo_), Point(hi_));
}

void Line3::Transform(const Mat4d &transform) {
    origin_ = transform.TransformPoint(origin_);
    direction_ = transform.TransformVector(direction_);
}

bool Line3::ToNurbs(int *out_degree, std::vector<double> *out_knots, std::vector<Vec4d> *out_control) const {
    *out_degree = 1;
    *out_knots = {lo_, lo_, hi_, hi_};
    out_control->clear();
    out_control->push_back(Vec4d::FromWeighted(Point(lo_), 1.0));
    out_control->push_back(Vec4d::FromWeighted(Point(hi_), 1.0));
    return true;
}

void Line3::Reverse() {
    // Keep the domain and flip the geometry, matching ReverseCurve's own
    // convention in cad_nurbs.cpp.
    const Vec3d start = Point(lo_);
    const Vec3d end = Point(hi_);
    const double span = hi_ - lo_;
    direction_ = (start - end) / (span != 0.0 ? span : 1.0);
    origin_ = end - direction_ * lo_;
}

double Line3::Length(double from, double to, double) const {
    return direction_.Length() * std::fabs(to - from);
}

// ---------------------------------------------------------------------
// Circle3
// ---------------------------------------------------------------------

Circle3::Circle3(const Vec3d &center, const Vec3d &x_axis, const Vec3d &y_axis, double radius, double start_angle,
                 double end_angle)
    : center_(center), radius_(radius), start_(start_angle), end_(end_angle) {
    // Orthonormalize rather than trust: a caller assembling a frame from
    // a normal and a reference direction routinely hands over axes that
    // are a degree or two off, and a silently sheared "circle" is a
    // miserable thing to debug.
    x_axis_ = x_axis.Normalized();
    const Vec3d projected = y_axis - x_axis_ * x_axis_.Dot(y_axis);
    y_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : x_axis_.AnyPerpendicular();
    if (end_ < start_) end_ += kTwoPi;
}

Circle3 Circle3::FromCenterNormalRadius(const Vec3d &center, const Vec3d &normal, double radius) {
    const Vec3d n = normal.Normalized();
    const Vec3d x = n.AnyPerpendicular();
    return Circle3(center, x, n.Cross(x), radius, 0.0, kTwoPi);
}

bool Circle3::FromThreePoints(const Vec3d &a, const Vec3d &b, const Vec3d &c, Circle3 *out,
                              const Tolerance &tolerance) {
    const Vec3d ab = b - a;
    const Vec3d ac = c - a;
    const Vec3d normal = ab.Cross(ac);
    const double normal_length_squared = normal.LengthSquared();
    // Collinear (or coincident) points define no circle. This is a real
    // outcome when fitting an arc through sampled data that happens to be
    // straight, not a caller error.
    if (normal_length_squared <= tolerance.linear * tolerance.linear) return false;

    // Circumcentre, by the standard barycentric formula.
    const double ab2 = ab.LengthSquared();
    const double ac2 = ac.LengthSquared();
    const Vec3d to_center =
        (normal.Cross(ab) * ac2 + ac.Cross(normal) * ab2) / (2.0 * normal_length_squared);
    const Vec3d center = a + to_center;
    const double radius = to_center.Length();
    const Vec3d x = (a - center).Normalized();
    const Vec3d unit_normal = normal.Normalized();
    *out = Circle3(center, x, unit_normal.Cross(x), radius, 0.0, kTwoPi);
    return true;
}

std::unique_ptr<Curve3> Circle3::Clone() const { return std::make_unique<Circle3>(*this); }

void Circle3::Domain(double *lo, double *hi) const {
    *lo = start_;
    *hi = end_;
}

Vec3d Circle3::Point(double u) const {
    return center_ + x_axis_ * (radius_ * std::cos(u)) + y_axis_ * (radius_ * std::sin(u));
}

void Circle3::Derivatives(double u, int max_derivative, std::vector<Vec3d> *out) const {
    out->assign(Idx(max_derivative + 1), Vec3d{});
    const double c = std::cos(u);
    const double s = std::sin(u);
    for (int k = 0; k <= max_derivative; ++k) {
        // Differentiating (cos, sin) k times rotates it by k quarter
        // turns: the pattern repeats with period 4, so the derivative of
        // any order is exact and costs nothing.
        double dc = 0.0;
        double ds = 0.0;
        switch (k % 4) {
            case 0: dc = c;  ds = s;  break;
            case 1: dc = -s; ds = c;  break;
            case 2: dc = -c; ds = -s; break;
            default: dc = s; ds = -c; break;
        }
        (*out)[Idx(k)] = x_axis_ * (radius_ * dc) + y_axis_ * (radius_ * ds);
    }
    (*out)[0] = Point(u);
}

Box3d Circle3::Bounds() const {
    Box3d box;
    ExpandConicBounds(&box, center_, x_axis_, y_axis_, radius_, radius_, start_, end_);
    return box;
}

bool Circle3::IsClosed(const Tolerance &tolerance) const {
    return std::fabs((end_ - start_) - kTwoPi) <= tolerance.angular ||
           tolerance.SamePoint(Point(start_), Point(end_));
}

void Circle3::Transform(const Mat4d &transform) {
    center_ = transform.TransformPoint(center_);
    const Vec3d x = transform.TransformVector(x_axis_);
    const Vec3d y = transform.TransformVector(y_axis_);
    // A non-uniform scale turns a circle into an ellipse, which this type
    // cannot represent. Taking the mean radius is the honest
    // approximation, and callers that must not lose the shape convert to
    // an Ellipse3 or a NURBS first -- flagged here rather than silently
    // producing a wrong circle.
    radius_ *= 0.5 * (x.Length() + y.Length());
    x_axis_ = x.Normalized();
    const Vec3d projected = y - x_axis_ * x_axis_.Dot(y);
    y_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : x_axis_.AnyPerpendicular();
}

bool Circle3::ToNurbs(int *out_degree, std::vector<double> *out_knots, std::vector<Vec4d> *out_control) const {
    // A7.1. A conic needs a rational quadratic, and one quadratic span
    // covers at most a half turn before the construction degenerates (the
    // two end tangents become parallel), so the arc is split into spans
    // of at most 90 degrees -- which also keeps the weights comfortably
    // away from zero.
    const double sweep = end_ - start_;
    if (!(sweep > 0.0)) return false;
    int arc_count = 1;
    if (sweep > kHalfPi) arc_count = 2;
    if (sweep > kPi) arc_count = 3;
    if (sweep > 1.5 * kPi) arc_count = 4;
    const double delta = sweep / static_cast<double>(arc_count);
    const double w1 = std::cos(delta * 0.5);

    *out_degree = 2;
    const int control_count = 2 * arc_count + 1;
    out_control->assign(Idx(control_count), Vec4d{});

    Vec3d p0 = Point(start_);
    Vec3d t0 = x_axis_ * -std::sin(start_) + y_axis_ * std::cos(start_);
    (*out_control)[0] = Vec4d::FromWeighted(p0, 1.0);
    int index = 0;
    double angle = start_;
    for (int i = 1; i <= arc_count; ++i) {
        angle += delta;
        const Vec3d p2 = Point(angle);
        const Vec3d t2 = x_axis_ * -std::sin(angle) + y_axis_ * std::cos(angle);
        Vec3d p1;
        if (!IntersectLines(p0, t0, p2, t2, &p1)) return false;
        (*out_control)[Idx(index + 1)] = Vec4d::FromWeighted(p1, w1);
        (*out_control)[Idx(index + 2)] = Vec4d::FromWeighted(p2, 1.0);
        index += 2;
        p0 = p2;
        t0 = t2;
    }

    out_knots->assign(Idx(control_count + 3), 0.0);
    const int j = 2 * arc_count + 1;
    for (int i = 0; i < 3; ++i) {
        (*out_knots)[Idx(i)] = start_;
        (*out_knots)[Idx(i + j)] = end_;
    }
    // Interior knots are evenly spaced in the *parameter*, which for this
    // construction is the angle -- so the NURBS and the analytic form
    // share a parameterization and can be compared value for value.
    for (int i = 1; i < arc_count; ++i) {
        const double value = start_ + delta * static_cast<double>(i);
        (*out_knots)[Idx(1 + 2 * i)] = value;
        (*out_knots)[Idx(2 + 2 * i)] = value;
    }
    return true;
}

void Circle3::Reverse() {
    // We want Q(u) = P(phi - u) with phi = start + end, so that Q runs
    // from P(end) to P(start) over the *same* domain. Expanding
    // cos(phi-u) and sin(phi-u) gives exactly a change of frame:
    //
    //   X' =  cos(phi) X + sin(phi) Y
    //   Y' =  sin(phi) X - cos(phi) Y
    //
    // which is orthonormal (X'.Y' = cos.sin - sin.cos = 0) and
    // left-handed relative to the original, i.e. the plane normal flips
    // -- exactly right for a reversed circle. This works because a
    // circle's two radii are equal, so an angular shift *is* a rotation
    // of the frame. Ellipse3::Reverse cannot use it; see its own note.
    const double phi = start_ + end_;
    const double c = std::cos(phi);
    const double s = std::sin(phi);
    const Vec3d new_x = x_axis_ * c + y_axis_ * s;
    const Vec3d new_y = x_axis_ * s - y_axis_ * c;
    x_axis_ = new_x;
    y_axis_ = new_y;
}

double Circle3::Length(double from, double to, double) const { return radius_ * std::fabs(to - from); }

// ---------------------------------------------------------------------
// Ellipse3
// ---------------------------------------------------------------------

Ellipse3::Ellipse3(const Vec3d &center, const Vec3d &x_axis, const Vec3d &y_axis, double major_radius,
                   double minor_radius, double start_angle, double end_angle)
    : center_(center), major_(major_radius), minor_(minor_radius), start_(start_angle), end_(end_angle) {
    x_axis_ = x_axis.Normalized();
    const Vec3d projected = y_axis - x_axis_ * x_axis_.Dot(y_axis);
    y_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : x_axis_.AnyPerpendicular();
    if (end_ < start_) end_ += kTwoPi;
}

std::unique_ptr<Curve3> Ellipse3::Clone() const { return std::make_unique<Ellipse3>(*this); }

void Ellipse3::Domain(double *lo, double *hi) const {
    *lo = start_;
    *hi = end_;
}

Vec3d Ellipse3::Point(double u) const {
    return center_ + x_axis_ * (major_ * std::cos(u)) + y_axis_ * (minor_ * std::sin(u));
}

void Ellipse3::Derivatives(double u, int max_derivative, std::vector<Vec3d> *out) const {
    out->assign(Idx(max_derivative + 1), Vec3d{});
    const double c = std::cos(u);
    const double s = std::sin(u);
    for (int k = 0; k <= max_derivative; ++k) {
        double dc = 0.0;
        double ds = 0.0;
        switch (k % 4) {
            case 0: dc = c;  ds = s;  break;
            case 1: dc = -s; ds = c;  break;
            case 2: dc = -c; ds = -s; break;
            default: dc = s; ds = -c; break;
        }
        (*out)[Idx(k)] = x_axis_ * (major_ * dc) + y_axis_ * (minor_ * ds);
    }
    (*out)[0] = Point(u);
}

Box3d Ellipse3::Bounds() const {
    Box3d box;
    ExpandConicBounds(&box, center_, x_axis_, y_axis_, major_, minor_, start_, end_);
    return box;
}

bool Ellipse3::IsClosed(const Tolerance &tolerance) const {
    return std::fabs((end_ - start_) - kTwoPi) <= tolerance.angular ||
           tolerance.SamePoint(Point(start_), Point(end_));
}

void Ellipse3::Transform(const Mat4d &transform) {
    center_ = transform.TransformPoint(center_);
    const Vec3d x = transform.TransformVector(x_axis_);
    const Vec3d y = transform.TransformVector(y_axis_);
    major_ *= x.Length();
    minor_ *= y.Length();
    x_axis_ = x.Normalized();
    const Vec3d projected = y - x_axis_ * x_axis_.Dot(y);
    y_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : x_axis_.AnyPerpendicular();
}

bool Ellipse3::ToNurbs(int *out_degree, std::vector<double> *out_knots, std::vector<Vec4d> *out_control) const {
    // An ellipse is the affine image of a circle, and a NURBS is affine
    // invariant -- so building the unit circle's exact representation and
    // then scaling its control points along the two axes is exact, not an
    // approximation. Deriving it this way rather than repeating the A7.1
    // construction also means the ellipse cannot drift out of agreement
    // with the circle.
    Circle3 unit(center_, x_axis_, y_axis_, 1.0, start_, end_);
    if (!unit.ToNurbs(out_degree, out_knots, out_control)) return false;
    for (Vec4d &control : *out_control) {
        const Vec3d p = control.Project() - center_;
        const double along_x = p.Dot(x_axis_) * major_;
        const double along_y = p.Dot(y_axis_) * minor_;
        const Vec3d scaled = center_ + x_axis_ * along_x + y_axis_ * along_y;
        control = Vec4d::FromWeighted(scaled, control.w);
    }
    return true;
}

void Ellipse3::Reverse() {
    // Negating the y axis gives Q(u) = P(-u), which traces the ellipse
    // backwards; the domain therefore becomes [-end, -start].
    //
    // Unlike Circle3, the domain cannot be shifted back to where it was.
    // Doing so would need Q(u) = P(phi - u) to be an ellipse in canonical
    // form, and expanding it gives axes V1 = a cos(phi) X + b sin(phi) Y
    // and V2 = a sin(phi) X - b cos(phi) Y whose dot product is
    // (a^2 - b^2) sin(phi) cos(phi). That vanishes only when a = b (a
    // circle) or phi is a multiple of a quarter turn -- so for a general
    // elliptical arc the reversed curve simply is not representable with
    // the original domain. Hence Curve3::Reverse promises direction, not
    // domain, and callers re-read Domain() afterwards.
    const double start = start_;
    const double end = end_;
    y_axis_ = -y_axis_;
    start_ = -end;
    end_ = -start;
}

// ---------------------------------------------------------------------
// NurbsCurve3
// ---------------------------------------------------------------------

bool NurbsCurve3::Create(int degree, const std::vector<double> &knots, const std::vector<Vec4d> &control,
                         NurbsCurve3 *out, std::string *error) {
    if (!ValidateKnotVector(knots, degree, static_cast<int>(control.size()), error)) return false;
    for (const Vec4d &c : control) {
        if (c.w <= 0.0) {
            *error = "control point weights must be positive; a zero or negative weight is a point at infinity "
                     "or a sign flip, neither of which this kernel represents";
            return false;
        }
    }
    out->degree_ = degree;
    out->knots_ = knots;
    out->control_ = control;
    return true;
}

bool NurbsCurve3::CreateFromPoints(int degree, const std::vector<double> &knots, const std::vector<Vec3d> &control,
                                   NurbsCurve3 *out, std::string *error) {
    std::vector<Vec4d> weighted;
    weighted.reserve(control.size());
    for (const Vec3d &p : control) weighted.push_back(Vec4d::FromWeighted(p, 1.0));
    return Create(degree, knots, weighted, out, error);
}

bool NurbsCurve3::Interpolate(const std::vector<Vec3d> &points, int degree, Parameterization parameterization,
                              NurbsCurve3 *out) {
    std::vector<double> knots;
    std::vector<Vec4d> control;
    if (!InterpolateCurve(points, degree, parameterization, &knots, &control)) return false;
    out->degree_ = degree;
    out->knots_ = std::move(knots);
    out->control_ = std::move(control);
    return true;
}

bool NurbsCurve3::Approximate(const std::vector<Vec3d> &points, int degree, int control_point_count,
                              Parameterization parameterization, NurbsCurve3 *out) {
    std::vector<double> knots;
    std::vector<Vec4d> control;
    if (!ApproximateCurve(points, degree, control_point_count, parameterization, &knots, &control)) return false;
    out->degree_ = degree;
    out->knots_ = std::move(knots);
    out->control_ = std::move(control);
    return true;
}

bool NurbsCurve3::IsRational(const Tolerance &tolerance) const {
    for (const Vec4d &c : control_) {
        if (std::fabs(c.w - 1.0) > tolerance.relative + tolerance.linear) return true;
    }
    return false;
}

std::unique_ptr<Curve3> NurbsCurve3::Clone() const { return std::make_unique<NurbsCurve3>(*this); }

void NurbsCurve3::Domain(double *lo, double *hi) const {
    KnotDomain(knots_, degree_, static_cast<int>(control_.size()), lo, hi);
}

Vec3d NurbsCurve3::Point(double u) const {
    return CurvePointHomogeneous(degree_, knots_, control_, u).Project();
}

void NurbsCurve3::Derivatives(double u, int max_derivative, std::vector<Vec3d> *out) const {
    std::vector<Vec4d> homogeneous;
    CurveDerivativesHomogeneous(degree_, knots_, control_, u, max_derivative, &homogeneous);
    RationalDerivatives(homogeneous, max_derivative, out);
}

Box3d NurbsCurve3::Bounds() const {
    // The control polygon's box. Contains the curve by the convex hull
    // property, and is not tight -- which is stated in the interface, and
    // is why the subdivision in Part C.3 refines before it trusts a box.
    Box3d box;
    for (const Vec4d &c : control_) box.Expand(c.Project());
    return box;
}

bool NurbsCurve3::IsClosed(const Tolerance &tolerance) const {
    if (control_.size() < 2) return false;
    return tolerance.SamePoint(control_.front().Project(), control_.back().Project());
}

void NurbsCurve3::Transform(const Mat4d &transform) {
    // Transform the *Euclidean* points and re-weight. Applying the matrix
    // to the weighted coordinates directly would be equivalent for an
    // affine transform, but only because the last row is (0,0,0,1) -- the
    // explicit form stays correct if that ever stops being true.
    for (Vec4d &c : control_) {
        c = Vec4d::FromWeighted(transform.TransformPoint(c.Project()), c.w);
    }
}

bool NurbsCurve3::ToNurbs(int *out_degree, std::vector<double> *out_knots, std::vector<Vec4d> *out_control) const {
    *out_degree = degree_;
    *out_knots = knots_;
    *out_control = control_;
    return true;
}

void NurbsCurve3::Reverse() { ReverseCurve(&knots_, &control_); }

std::vector<double> NurbsCurve3::NaturalBreaks() const {
    std::vector<double> breaks;
    for (const KnotSpan &span : InteriorKnots(knots_, degree_, static_cast<int>(control_.size()))) {
        breaks.push_back(span.value);
    }
    return breaks;
}

bool NurbsCurve3::InsertKnot(double u, int multiplicity) {
    return cad::InsertKnot(degree_, &knots_, &control_, u, multiplicity);
}

int NurbsCurve3::RemoveKnot(double u, int times, double tolerance) {
    return cad::RemoveKnot(degree_, &knots_, &control_, u, times, tolerance);
}

bool NurbsCurve3::ElevateDegree(int times) {
    return cad::ElevateDegree(&degree_, &knots_, &control_, times);
}

bool NurbsCurve3::Split(double u, NurbsCurve3 *left, NurbsCurve3 *right) const {
    std::vector<double> left_knots;
    std::vector<Vec4d> left_control;
    std::vector<double> right_knots;
    std::vector<Vec4d> right_control;
    if (!SplitCurve(degree_, knots_, control_, u, &left_knots, &left_control, &right_knots, &right_control)) {
        return false;
    }
    left->degree_ = degree_;
    left->knots_ = std::move(left_knots);
    left->control_ = std::move(left_control);
    right->degree_ = degree_;
    right->knots_ = std::move(right_knots);
    right->control_ = std::move(right_control);
    return true;
}

bool CurveToNurbs(const Curve3 &curve, NurbsCurve3 *out) {
    int degree = 0;
    std::vector<double> knots;
    std::vector<Vec4d> control;
    if (!curve.ToNurbs(&degree, &knots, &control)) return false;
    std::string error;
    return NurbsCurve3::Create(degree, knots, control, out, &error);
}

}  // namespace cad
