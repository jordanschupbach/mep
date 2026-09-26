#include "cad_surface.h"

#include <algorithm>
#include <cmath>

namespace cad {
namespace {

inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

bool IntersectLines(const Vec3d &p0, const Vec3d &t0, const Vec3d &p2, const Vec3d &t2, Vec3d *out) {
    const Vec3d w = p2 - p0;
    const double a = t0.Dot(t0);
    const double b = t0.Dot(t2);
    const double c = t2.Dot(t2);
    const double d = t0.Dot(w);
    const double e = t2.Dot(w);
    const double denominator = a * c - b * b;
    if (std::fabs(denominator) <= 1e-300) return false;
    *out = p0 + t0 * ((d * c - e * b) / denominator);
    return true;
}

// A8.1 (MakeRevolvedSurf). Revolves a NURBS generatrix about an axis,
// producing a tensor-product control grid with the *revolution* as the
// first (u) parameter. Every quadric in this file goes through here --
// cylinder, cone, sphere and torus are all revolutions of a line or an
// arc -- which is deliberate: one construction that is right beats five
// that are each nearly right, and the tests cross-check all five against
// their own analytic evaluation.
bool RevolveControlPoints(const Vec3d &axis_origin, const Vec3d &axis_direction, double angle_lo, double angle_hi,
                          const std::vector<double> &generatrix_knots, const std::vector<Vec4d> &generatrix,
                          std::vector<double> *out_knots_u, std::vector<double> *out_knots_v,
                          std::vector<Vec4d> *out_control, int *out_count_u, int *out_count_v) {
    const double sweep = angle_hi - angle_lo;
    if (!(sweep > 0.0)) return false;
    int arc_count = 1;
    if (sweep > kHalfPi) arc_count = 2;
    if (sweep > kPi) arc_count = 3;
    if (sweep > 1.5 * kPi) arc_count = 4;
    const double delta = sweep / static_cast<double>(arc_count);
    const double wm = std::cos(delta * 0.5);

    const int count_u = 2 * arc_count + 1;
    const int count_v = static_cast<int>(generatrix.size());
    *out_count_u = count_u;
    *out_count_v = count_v;
    out_control->assign(Idx(count_u) * Idx(count_v), Vec4d{});

    std::vector<double> cosines(Idx(arc_count + 1), 0.0);
    std::vector<double> sines(Idx(arc_count + 1), 0.0);
    for (int i = 0; i <= arc_count; ++i) {
        const double angle = angle_lo + delta * static_cast<double>(i);
        cosines[Idx(i)] = std::cos(angle);
        sines[Idx(i)] = std::sin(angle);
    }
    const Vec3d axis = axis_direction.Normalized();

    for (int j = 0; j < count_v; ++j) {
        const Vec4d &source = generatrix[Idx(j)];
        const Vec3d p = source.Project();
        const double weight = source.w;
        // Project the generatrix point onto the axis to find the centre
        // of the circle it sweeps.
        const Vec3d on_axis = axis_origin + axis * (p - axis_origin).Dot(axis);
        Vec3d radial = p - on_axis;
        const double radius = radial.Length();
        // A generatrix point *on* the axis stays put: its circle has zero
        // radius. Not a degeneracy to reject -- it is what closes a
        // sphere at its poles and a cone at its apex.
        Vec3d x_axis = (radius > 0.0) ? radial / radius : axis.AnyPerpendicular();
        Vec3d y_axis = axis.Cross(x_axis);

        auto circle_point = [&](int i) {
            return on_axis + x_axis * (radius * cosines[Idx(i)]) + y_axis * (radius * sines[Idx(i)]);
        };
        auto circle_tangent = [&](int i) {
            return x_axis * -sines[Idx(i)] + y_axis * cosines[Idx(i)];
        };

        Vec3d p0 = circle_point(0);
        Vec3d t0 = circle_tangent(0);
        (*out_control)[SurfaceIndex(0, j, count_v)] = Vec4d::FromWeighted(p0, weight);
        int index = 0;
        for (int i = 1; i <= arc_count; ++i) {
            const Vec3d p2 = circle_point(i);
            const Vec3d t2 = circle_tangent(i);
            Vec3d p1;
            if (radius > 0.0) {
                if (!IntersectLines(p0, t0, p2, t2, &p1)) return false;
            } else {
                p1 = on_axis;
            }
            (*out_control)[SurfaceIndex(index + 1, j, count_v)] = Vec4d::FromWeighted(p1, wm * weight);
            (*out_control)[SurfaceIndex(index + 2, j, count_v)] = Vec4d::FromWeighted(p2, weight);
            index += 2;
            p0 = p2;
            t0 = t2;
        }
    }

    out_knots_u->assign(Idx(count_u + 3), 0.0);
    const int j = 2 * arc_count + 1;
    for (int i = 0; i < 3; ++i) {
        (*out_knots_u)[Idx(i)] = angle_lo;
        (*out_knots_u)[Idx(i + j)] = angle_hi;
    }
    for (int i = 1; i < arc_count; ++i) {
        const double value = angle_lo + delta * static_cast<double>(i);
        (*out_knots_u)[Idx(1 + 2 * i)] = value;
        (*out_knots_u)[Idx(2 + 2 * i)] = value;
    }
    *out_knots_v = generatrix_knots;
    return true;
}

// Swaps the two parameter directions of a control grid. Needed because
// A8.1 produces the revolution in u while RevolutionSurface's interface
// puts the profile there.
void TransposeGrid(std::vector<Vec4d> *control, int *count_u, int *count_v, std::vector<double> *knots_u,
                   std::vector<double> *knots_v, int *degree_u, int *degree_v) {
    std::vector<Vec4d> transposed(control->size(), Vec4d{});
    for (int i = 0; i < *count_u; ++i) {
        for (int j = 0; j < *count_v; ++j) {
            transposed[SurfaceIndex(j, i, *count_u)] = (*control)[SurfaceIndex(i, j, *count_v)];
        }
    }
    *control = std::move(transposed);
    std::swap(*count_u, *count_v);
    std::swap(*knots_u, *knots_v);
    std::swap(*degree_u, *degree_v);
}

}  // namespace

// ---------------------------------------------------------------------
// Surface derived quantities
// ---------------------------------------------------------------------

Vec3d Surface::Normal(double u, double v) const {
    std::vector<std::vector<Vec3d>> ders;
    Derivatives(u, v, 1, &ders);
    return ders[1][0].Cross(ders[0][1]).Normalized();
}

void Surface::FirstFundamentalForm(double u, double v, double *e, double *f, double *g) const {
    std::vector<std::vector<Vec3d>> ders;
    Derivatives(u, v, 1, &ders);
    *e = ders[1][0].Dot(ders[1][0]);
    *f = ders[1][0].Dot(ders[0][1]);
    *g = ders[0][1].Dot(ders[0][1]);
}

void Surface::SecondFundamentalForm(double u, double v, double *l, double *m, double *n) const {
    std::vector<std::vector<Vec3d>> ders;
    Derivatives(u, v, 2, &ders);
    const Vec3d normal = ders[1][0].Cross(ders[0][1]).Normalized();
    *l = ders[2][0].Dot(normal);
    *m = ders[1][1].Dot(normal);
    *n = ders[0][2].Dot(normal);
}

double Surface::GaussianCurvature(double u, double v) const {
    double e = 0.0, f = 0.0, g = 0.0, l = 0.0, m = 0.0, n = 0.0;
    FirstFundamentalForm(u, v, &e, &f, &g);
    SecondFundamentalForm(u, v, &l, &m, &n);
    const double denominator = e * g - f * f;
    if (std::fabs(denominator) <= 1e-300) return 0.0;
    return (l * n - m * m) / denominator;
}

double Surface::MeanCurvature(double u, double v) const {
    double e = 0.0, f = 0.0, g = 0.0, l = 0.0, m = 0.0, n = 0.0;
    FirstFundamentalForm(u, v, &e, &f, &g);
    SecondFundamentalForm(u, v, &l, &m, &n);
    const double denominator = 2.0 * (e * g - f * f);
    if (std::fabs(denominator) <= 1e-300) return 0.0;
    return (e * n - 2.0 * f * m + g * l) / denominator;
}

void Surface::PrincipalCurvatures(double u, double v, double *k1, double *k2) const {
    // The principal curvatures are the roots of k^2 - 2Hk + K = 0. The
    // discriminant can go slightly negative from round-off at an umbilic
    // point (a sphere, where both curvatures are equal), so it is clamped
    // rather than allowed to produce NaN.
    const double mean = MeanCurvature(u, v);
    const double gaussian = GaussianCurvature(u, v);
    const double discriminant = std::max(0.0, mean * mean - gaussian);
    const double root = std::sqrt(discriminant);
    double a = mean + root;
    double b = mean - root;
    if (std::fabs(b) > std::fabs(a)) std::swap(a, b);
    *k1 = a;
    *k2 = b;
}

double Surface::Area(double tolerance) const {
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    // The area element is sqrt(EG - F^2): the area of the parallelogram
    // the two tangent vectors span. Nested adaptive quadrature -- the
    // outer integral's integrand is itself an adaptive integral, which is
    // expensive but needs no assumption about the surface's shape.
    return AdaptiveQuadrature(
        [&](double u) {
            return AdaptiveQuadrature(
                [&](double v) {
                    double e = 0.0, f = 0.0, g = 0.0;
                    FirstFundamentalForm(u, v, &e, &f, &g);
                    return std::sqrt(std::max(0.0, e * g - f * f));
                },
                v_lo, v_hi, tolerance);
        },
        u_lo, u_hi, tolerance);
}

bool Surface::ClosestPoint(const Vec3d &point, double *out_u, double *out_v, Vec3d *out_point,
                           const Tolerance &tolerance, int sample_count) const {
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    if (!(u_hi > u_lo) || !(v_hi > v_lo) || sample_count < 2) return false;

    // Coarse grid, then Newton from the best cell. Same reasoning as the
    // curve case: Newton alone finds *a* stationary point, and a surface
    // that wraps around the query point has many.
    double best_u = u_lo;
    double best_v = v_lo;
    double best_distance = 1e300;
    for (int i = 0; i <= sample_count; ++i) {
        const double u = u_lo + (u_hi - u_lo) * static_cast<double>(i) / static_cast<double>(sample_count);
        for (int j = 0; j <= sample_count; ++j) {
            const double v = v_lo + (v_hi - v_lo) * static_cast<double>(j) / static_cast<double>(sample_count);
            const double distance = (Point(u, v) - point).LengthSquared();
            if (distance < best_distance) {
                best_distance = distance;
                best_u = u;
                best_v = v;
            }
        }
    }

    const bool closed_u = IsClosedU(tolerance);
    const bool closed_v = IsClosedV(tolerance);
    double u = best_u;
    double v = best_v;
    std::vector<std::vector<Vec3d>> ders;
    for (int iteration = 0; iteration < 64; ++iteration) {
        Derivatives(u, v, 2, &ders);
        const Vec3d residual = ders[0][0] - point;
        const Vec3d su = ders[1][0];
        const Vec3d sv = ders[0][1];
        // Solve the 2x2 system for the stationary point of the squared
        // distance: both tangents perpendicular to the residual.
        const double f1 = su.Dot(residual);
        const double f2 = sv.Dot(residual);
        const double j11 = ders[2][0].Dot(residual) + su.Dot(su);
        const double j12 = ders[1][1].Dot(residual) + su.Dot(sv);
        const double j21 = j12;  // symmetric: both equal Suv.r + Su.Sv
        const double j22 = ders[0][2].Dot(residual) + sv.Dot(sv);
        const double determinant = j11 * j22 - j12 * j21;
        if (std::fabs(determinant) <= 1e-300) break;
        const double du = -(f1 * j22 - f2 * j12) / determinant;
        const double dv = -(j11 * f2 - j21 * f1) / determinant;
        // Same three guards as the curve case, for the same reasons: a
        // degenerate point (a sphere's pole, or the centre of a sphere
        // queried from inside) makes the system singular or the step
        // enormous, and neither is a refinement.
        if (!std::isfinite(du) || !std::isfinite(dv)) break;
        if (std::fabs(du) > (u_hi - u_lo) || std::fabs(dv) > (v_hi - v_lo)) break;

        double next_u = u + du;
        double next_v = v + dv;
        if (closed_u) {
            const double span = u_hi - u_lo;
            next_u -= span * std::floor((next_u - u_lo) / span);
        } else {
            next_u = Clamp(next_u, u_lo, u_hi);
        }
        if (closed_v) {
            const double span = v_hi - v_lo;
            next_v -= span * std::floor((next_v - v_lo) / span);
        } else {
            next_v = Clamp(next_v, v_lo, v_hi);
        }
        if (!std::isfinite(next_u) || !std::isfinite(next_v)) break;
        const double step = std::fabs(next_u - u) + std::fabs(next_v - v);
        u = next_u;
        v = next_v;
        if (step <= tolerance.linear * 1e-3) break;
    }
    // Newton can wander to a worse point on a badly-behaved patch; keep
    // whichever of the refined and sampled answers is actually closer.
    if ((Point(u, v) - point).LengthSquared() > best_distance) {
        u = best_u;
        v = best_v;
    }
    *out_u = u;
    *out_v = v;
    if (out_point != nullptr) *out_point = Point(u, v);
    return true;
}

bool Surface::ContainsPoint(const Vec3d &point, double *out_u, double *out_v, const Tolerance &tolerance) const {
    double u = 0.0;
    double v = 0.0;
    Vec3d on_surface;
    if (!ClosestPoint(point, &u, &v, &on_surface, tolerance)) return false;
    if (out_u != nullptr) *out_u = u;
    if (out_v != nullptr) *out_v = v;
    return tolerance.SamePoint(on_surface, point);
}

bool Surface::IsoCurve(bool fix_u, double fixed_value, NurbsCurve3 *out) const {
    int degree_u = 0, degree_v = 0, count_u = 0, count_v = 0;
    std::vector<double> knots_u;
    std::vector<double> knots_v;
    std::vector<Vec4d> control;
    if (!ToNurbs(&degree_u, &degree_v, &knots_u, &knots_v, &control, &count_u, &count_v)) return false;
    int out_degree = 0;
    std::vector<double> out_knots;
    std::vector<Vec4d> out_control;
    if (!SurfaceIsoCurve(degree_u, degree_v, knots_u, knots_v, control, count_u, count_v, fix_u, fixed_value,
                         &out_degree, &out_knots, &out_control)) {
        return false;
    }
    std::string error;
    return NurbsCurve3::Create(out_degree, out_knots, out_control, out, &error);
}

// ---------------------------------------------------------------------
// PlaneSurface
// ---------------------------------------------------------------------

PlaneSurface::PlaneSurface(const Vec3d &origin, const Vec3d &x_axis, const Vec3d &y_axis, double u_lo,
                           double u_hi, double v_lo, double v_hi)
    : origin_(origin), u_lo_(u_lo), u_hi_(u_hi), v_lo_(v_lo), v_hi_(v_hi) {
    x_axis_ = x_axis.Normalized();
    const Vec3d projected = y_axis - x_axis_ * x_axis_.Dot(y_axis);
    y_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : x_axis_.AnyPerpendicular();
}

double PlaneSurface::SignedDistance(const Vec3d &point) const { return (point - origin_).Dot(PlaneNormal()); }

std::unique_ptr<Surface> PlaneSurface::Clone() const { return std::make_unique<PlaneSurface>(*this); }

void PlaneSurface::Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const {
    *u_lo = u_lo_;
    *u_hi = u_hi_;
    *v_lo = v_lo_;
    *v_hi = v_hi_;
}

Vec3d PlaneSurface::Point(double u, double v) const { return origin_ + x_axis_ * u + y_axis_ * v; }

void PlaneSurface::Derivatives(double u, double v, int max_derivative,
                               std::vector<std::vector<Vec3d>> *out) const {
    out->assign(Idx(max_derivative + 1), std::vector<Vec3d>(Idx(max_derivative + 1), Vec3d{}));
    (*out)[0][0] = Point(u, v);
    if (max_derivative >= 1) {
        (*out)[1][0] = x_axis_;
        (*out)[0][1] = y_axis_;
    }
    // Every second and higher derivative of a plane is zero, which the
    // assign has already established.
}

Box3d PlaneSurface::Bounds() const {
    Box3d box;
    box.Expand(Point(u_lo_, v_lo_));
    box.Expand(Point(u_hi_, v_lo_));
    box.Expand(Point(u_lo_, v_hi_));
    box.Expand(Point(u_hi_, v_hi_));
    return box;
}

void PlaneSurface::Transform(const Mat4d &transform) {
    origin_ = transform.TransformPoint(origin_);
    x_axis_ = transform.TransformVector(x_axis_).Normalized();
    const Vec3d y = transform.TransformVector(y_axis_);
    const Vec3d projected = y - x_axis_ * x_axis_.Dot(y);
    y_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : x_axis_.AnyPerpendicular();
}

bool PlaneSurface::ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                           std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                           int *out_count_v) const {
    *out_degree_u = 1;
    *out_degree_v = 1;
    *out_count_u = 2;
    *out_count_v = 2;
    *out_knots_u = {u_lo_, u_lo_, u_hi_, u_hi_};
    *out_knots_v = {v_lo_, v_lo_, v_hi_, v_hi_};
    out_control->assign(4, Vec4d{});
    (*out_control)[SurfaceIndex(0, 0, 2)] = Vec4d::FromWeighted(Point(u_lo_, v_lo_), 1.0);
    (*out_control)[SurfaceIndex(0, 1, 2)] = Vec4d::FromWeighted(Point(u_lo_, v_hi_), 1.0);
    (*out_control)[SurfaceIndex(1, 0, 2)] = Vec4d::FromWeighted(Point(u_hi_, v_lo_), 1.0);
    (*out_control)[SurfaceIndex(1, 1, 2)] = Vec4d::FromWeighted(Point(u_hi_, v_hi_), 1.0);
    return true;
}

bool PlaneSurface::IsClosedU(const Tolerance &) const { return false; }
bool PlaneSurface::IsClosedV(const Tolerance &) const { return false; }

double PlaneSurface::Area(double) const { return (u_hi_ - u_lo_) * (v_hi_ - v_lo_); }

// ---------------------------------------------------------------------
// CylinderSurface
// ---------------------------------------------------------------------

CylinderSurface::CylinderSurface(const Vec3d &origin, const Vec3d &x_axis, const Vec3d &axis, double radius,
                                 double u_lo, double u_hi, double v_lo, double v_hi)
    : origin_(origin), radius_(radius), u_lo_(u_lo), u_hi_(u_hi), v_lo_(v_lo), v_hi_(v_hi) {
    axis_ = axis.Normalized();
    const Vec3d projected = x_axis - axis_ * axis_.Dot(x_axis);
    x_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : axis_.AnyPerpendicular();
    y_axis_ = axis_.Cross(x_axis_);
}

std::unique_ptr<Surface> CylinderSurface::Clone() const { return std::make_unique<CylinderSurface>(*this); }

void CylinderSurface::Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const {
    *u_lo = u_lo_;
    *u_hi = u_hi_;
    *v_lo = v_lo_;
    *v_hi = v_hi_;
}

Vec3d CylinderSurface::Point(double u, double v) const {
    return origin_ + x_axis_ * (radius_ * std::cos(u)) + y_axis_ * (radius_ * std::sin(u)) + axis_ * v;
}

void CylinderSurface::Derivatives(double u, double v, int max_derivative,
                                  std::vector<std::vector<Vec3d>> *out) const {
    out->assign(Idx(max_derivative + 1), std::vector<Vec3d>(Idx(max_derivative + 1), Vec3d{}));
    const double c = std::cos(u);
    const double s = std::sin(u);
    (*out)[0][0] = Point(u, v);
    // Derivatives in u cycle with period 4; v is linear, so the only
    // non-zero v derivative is the first, and every mixed partial past
    // (k,1) vanishes.
    for (int k = 1; k <= max_derivative; ++k) {
        double dc = 0.0;
        double ds = 0.0;
        switch (k % 4) {
            case 0: dc = c;  ds = s;  break;
            case 1: dc = -s; ds = c;  break;
            case 2: dc = -c; ds = -s; break;
            default: dc = s; ds = -c; break;
        }
        (*out)[Idx(k)][0] = x_axis_ * (radius_ * dc) + y_axis_ * (radius_ * ds);
    }
    if (max_derivative >= 1) (*out)[0][1] = axis_;
}

Box3d CylinderSurface::Bounds() const {
    Box3d box;
    // Sample the angular extremes exactly, the same way the conic bounds
    // in cad_curve.cpp do, at both ends of the height range.
    for (double v : {v_lo_, v_hi_}) {
        const Vec3d center = origin_ + axis_ * v;
        box.Expand(Point(u_lo_, v));
        box.Expand(Point(u_hi_, v));
        for (int k = 0; k < 3; ++k) {
            const double ax = radius_ * x_axis_[Idx(k)];
            const double ay = radius_ * y_axis_[Idx(k)];
            if (ax == 0.0 && ay == 0.0) continue;
            const double base = std::atan2(ay, ax);
            for (int half = 0; half < 2; ++half) {
                double u = base + static_cast<double>(half) * kPi;
                while (u < u_lo_) u += kTwoPi;
                while (u > u_hi_) u -= kTwoPi;
                if (u >= u_lo_ && u <= u_hi_) {
                    box.Expand(center + x_axis_ * (radius_ * std::cos(u)) + y_axis_ * (radius_ * std::sin(u)));
                }
            }
        }
    }
    return box;
}

void CylinderSurface::Transform(const Mat4d &transform) {
    origin_ = transform.TransformPoint(origin_);
    const Vec3d x = transform.TransformVector(x_axis_);
    radius_ *= x.Length();
    axis_ = transform.TransformVector(axis_).Normalized();
    const Vec3d projected = x - axis_ * axis_.Dot(x);
    x_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : axis_.AnyPerpendicular();
    y_axis_ = axis_.Cross(x_axis_);
    // A reflection turns the parameterization inside out. The angle u is
    // measured from x_axis_ towards y_axis_, and y_axis_ is recomputed
    // above as axis x x_axis -- a cross product, which comes out on the
    // other side when the transform reverses handedness. So the
    // transformed surface traces the same points with u running
    // backwards, and the trimmed domain has to say so. Nothing produced
    // a reflection before Part E.5's mirror pattern, which is why this
    // went unnoticed: the surface was right as a *set* and wrong as a
    // parameterization, and only a trimmed face notices the difference.
    if (transform.LinearDeterminant() < 0.0) {
        const double lo = u_lo_;
        u_lo_ = -u_hi_;
        u_hi_ = -lo;
    }
}

bool CylinderSurface::ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                              std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control,
                              int *out_count_u, int *out_count_v) const {
    // The generatrix is the straight segment from v_lo to v_hi at angle
    // u_lo; revolving it gives the cylinder.
    const std::vector<double> generatrix_knots = {v_lo_, v_lo_, v_hi_, v_hi_};
    std::vector<Vec4d> generatrix;
    const Vec3d start = origin_ + x_axis_ * radius_ + axis_ * v_lo_;
    const Vec3d end = origin_ + x_axis_ * radius_ + axis_ * v_hi_;
    generatrix.push_back(Vec4d::FromWeighted(start, 1.0));
    generatrix.push_back(Vec4d::FromWeighted(end, 1.0));
    *out_degree_u = 2;
    *out_degree_v = 1;
    return RevolveControlPoints(origin_, axis_, u_lo_, u_hi_, generatrix_knots, generatrix, out_knots_u,
                                out_knots_v, out_control, out_count_u, out_count_v);
}

bool CylinderSurface::IsClosedU(const Tolerance &tolerance) const {
    return std::fabs((u_hi_ - u_lo_) - kTwoPi) <= tolerance.angular;
}
bool CylinderSurface::IsClosedV(const Tolerance &) const { return false; }

double CylinderSurface::Area(double) const { return radius_ * (u_hi_ - u_lo_) * (v_hi_ - v_lo_); }

// ---------------------------------------------------------------------
// ConeSurface
// ---------------------------------------------------------------------

ConeSurface::ConeSurface(const Vec3d &apex, const Vec3d &x_axis, const Vec3d &axis, double half_angle,
                         double u_lo, double u_hi, double v_lo, double v_hi)
    : apex_(apex), half_angle_(half_angle), u_lo_(u_lo), u_hi_(u_hi), v_lo_(v_lo), v_hi_(v_hi) {
    axis_ = axis.Normalized();
    const Vec3d projected = x_axis - axis_ * axis_.Dot(x_axis);
    x_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : axis_.AnyPerpendicular();
    y_axis_ = axis_.Cross(x_axis_);
}

std::unique_ptr<Surface> ConeSurface::Clone() const { return std::make_unique<ConeSurface>(*this); }

void ConeSurface::Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const {
    *u_lo = u_lo_;
    *u_hi = u_hi_;
    *v_lo = v_lo_;
    *v_hi = v_hi_;
}

Vec3d ConeSurface::Point(double u, double v) const {
    const double r = v * std::sin(half_angle_);
    const double h = v * std::cos(half_angle_);
    return apex_ + x_axis_ * (r * std::cos(u)) + y_axis_ * (r * std::sin(u)) + axis_ * h;
}

void ConeSurface::Derivatives(double u, double v, int max_derivative,
                              std::vector<std::vector<Vec3d>> *out) const {
    out->assign(Idx(max_derivative + 1), std::vector<Vec3d>(Idx(max_derivative + 1), Vec3d{}));
    const double sin_half = std::sin(half_angle_);
    const double cos_half = std::cos(half_angle_);
    const double c = std::cos(u);
    const double s = std::sin(u);
    (*out)[0][0] = Point(u, v);
    // S is linear in v and trigonometric in u, so only the (k,0) and
    // (k,1) partials are non-zero.
    for (int k = 1; k <= max_derivative; ++k) {
        double dc = 0.0;
        double ds = 0.0;
        switch (k % 4) {
            case 0: dc = c;  ds = s;  break;
            case 1: dc = -s; ds = c;  break;
            case 2: dc = -c; ds = -s; break;
            default: dc = s; ds = -c; break;
        }
        (*out)[Idx(k)][0] = x_axis_ * (v * sin_half * dc) + y_axis_ * (v * sin_half * ds);
        if (max_derivative >= 1) {
            (*out)[Idx(k)][1] = x_axis_ * (sin_half * dc) + y_axis_ * (sin_half * ds);
        }
    }
    if (max_derivative >= 1) {
        (*out)[0][1] = x_axis_ * (sin_half * c) + y_axis_ * (sin_half * s) + axis_ * cos_half;
    }
}

Box3d ConeSurface::Bounds() const {
    Box3d box;
    for (double v : {v_lo_, v_hi_}) {
        const double r = v * std::sin(half_angle_);
        const Vec3d center = apex_ + axis_ * (v * std::cos(half_angle_));
        box.Expand(Point(u_lo_, v));
        box.Expand(Point(u_hi_, v));
        for (int k = 0; k < 3; ++k) {
            const double ax = r * x_axis_[Idx(k)];
            const double ay = r * y_axis_[Idx(k)];
            if (ax == 0.0 && ay == 0.0) continue;
            const double base = std::atan2(ay, ax);
            for (int half = 0; half < 2; ++half) {
                double u = base + static_cast<double>(half) * kPi;
                while (u < u_lo_) u += kTwoPi;
                while (u > u_hi_) u -= kTwoPi;
                if (u >= u_lo_ && u <= u_hi_) {
                    box.Expand(center + x_axis_ * (r * std::cos(u)) + y_axis_ * (r * std::sin(u)));
                }
            }
        }
    }
    return box;
}

void ConeSurface::Transform(const Mat4d &transform) {
    apex_ = transform.TransformPoint(apex_);
    const Vec3d x = transform.TransformVector(x_axis_);
    axis_ = transform.TransformVector(axis_).Normalized();
    const Vec3d projected = x - axis_ * axis_.Dot(x);
    x_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : axis_.AnyPerpendicular();
    y_axis_ = axis_.Cross(x_axis_);
    if (transform.LinearDeterminant() < 0.0) {
        // See CylinderSurface::Transform: a reflection makes u run the
        // other way, so the trimmed domain flips with it.
        const double lo = u_lo_;
        u_lo_ = -u_hi_;
        u_hi_ = -lo;
    }
}

bool ConeSurface::ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                          std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                          int *out_count_v) const {
    const std::vector<double> generatrix_knots = {v_lo_, v_lo_, v_hi_, v_hi_};
    std::vector<Vec4d> generatrix;
    generatrix.push_back(Vec4d::FromWeighted(Point(u_lo_, v_lo_), 1.0));
    generatrix.push_back(Vec4d::FromWeighted(Point(u_lo_, v_hi_), 1.0));
    *out_degree_u = 2;
    *out_degree_v = 1;
    return RevolveControlPoints(apex_, axis_, u_lo_, u_hi_, generatrix_knots, generatrix, out_knots_u,
                                out_knots_v, out_control, out_count_u, out_count_v);
}

bool ConeSurface::IsClosedU(const Tolerance &tolerance) const {
    return std::fabs((u_hi_ - u_lo_) - kTwoPi) <= tolerance.angular;
}
bool ConeSurface::IsClosedV(const Tolerance &) const { return false; }

// ---------------------------------------------------------------------
// SphereSurface
// ---------------------------------------------------------------------

SphereSurface::SphereSurface(const Vec3d &center, double radius, const Vec3d &x_axis, const Vec3d &axis,
                             double u_lo, double u_hi, double v_lo, double v_hi)
    : center_(center), radius_(radius), u_lo_(u_lo), u_hi_(u_hi), v_lo_(v_lo), v_hi_(v_hi) {
    axis_ = axis.Normalized();
    const Vec3d projected = x_axis - axis_ * axis_.Dot(x_axis);
    x_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : axis_.AnyPerpendicular();
    y_axis_ = axis_.Cross(x_axis_);
}

std::unique_ptr<Surface> SphereSurface::Clone() const { return std::make_unique<SphereSurface>(*this); }

void SphereSurface::Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const {
    *u_lo = u_lo_;
    *u_hi = u_hi_;
    *v_lo = v_lo_;
    *v_hi = v_hi_;
}

Vec3d SphereSurface::Point(double u, double v) const {
    const double cv = std::cos(v);
    return center_ + x_axis_ * (radius_ * cv * std::cos(u)) + y_axis_ * (radius_ * cv * std::sin(u)) +
           axis_ * (radius_ * std::sin(v));
}

void SphereSurface::Derivatives(double u, double v, int max_derivative,
                                std::vector<std::vector<Vec3d>> *out) const {
    out->assign(Idx(max_derivative + 1), std::vector<Vec3d>(Idx(max_derivative + 1), Vec3d{}));
    // Both parameters are trigonometric, so every partial is a product of
    // two quarter-turn-shifted sinusoids -- computed directly rather than
    // by any recurrence, and exact.
    for (int k = 0; k <= max_derivative; ++k) {
        for (int l = 0; l + k <= max_derivative; ++l) {
            const double uc = std::cos(u + static_cast<double>(k) * kHalfPi);
            const double us = std::sin(u + static_cast<double>(k) * kHalfPi);
            // d^l/dv^l of cos(v) and of sin(v).
            const double vc = std::cos(v + static_cast<double>(l) * kHalfPi);
            const double vs = std::sin(v + static_cast<double>(l) * kHalfPi);
            Vec3d value = x_axis_ * (radius_ * vc * uc) + y_axis_ * (radius_ * vc * us);
            // The axial term has no u dependence, so it survives only in
            // the pure-v partials.
            if (k == 0) value += axis_ * (radius_ * vs);
            (*out)[Idx(k)][Idx(l)] = value;
        }
    }
    (*out)[0][0] = Point(u, v);
}

Box3d SphereSurface::Bounds() const {
    Box3d box;
    // For a partial sphere an exact box needs the stationary points in
    // both parameters; sampling a modest grid plus the corners is tight
    // enough for a culling bound and is exact for the full sphere, which
    // is the case that matters.
    const cad::Tolerance tolerance;
    if (IsClosedU(tolerance) && std::fabs(v_hi_ - v_lo_ - kPi) <= tolerance.angular) {
        box.Expand(center_ - Vec3d{radius_, radius_, radius_});
        box.Expand(center_ + Vec3d{radius_, radius_, radius_});
        return box;
    }
    for (int i = 0; i <= 32; ++i) {
        const double u = u_lo_ + (u_hi_ - u_lo_) * static_cast<double>(i) / 32.0;
        for (int j = 0; j <= 32; ++j) {
            const double v = v_lo_ + (v_hi_ - v_lo_) * static_cast<double>(j) / 32.0;
            box.Expand(Point(u, v));
        }
    }
    return box;
}

void SphereSurface::Transform(const Mat4d &transform) {
    center_ = transform.TransformPoint(center_);
    const Vec3d x = transform.TransformVector(x_axis_);
    radius_ *= x.Length();
    axis_ = transform.TransformVector(axis_).Normalized();
    const Vec3d projected = x - axis_ * axis_.Dot(x);
    x_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : axis_.AnyPerpendicular();
    y_axis_ = axis_.Cross(x_axis_);
    if (transform.LinearDeterminant() < 0.0) {
        // See CylinderSurface::Transform: a reflection makes u run the
        // other way, so the trimmed domain flips with it.
        const double lo = u_lo_;
        u_lo_ = -u_hi_;
        u_hi_ = -lo;
    }
}

bool SphereSurface::ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                            std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                            int *out_count_v) const {
    // The generatrix is the meridian arc from v_lo to v_hi at longitude
    // u_lo -- itself a circular arc, so its exact NURBS form comes from
    // Circle3, and revolving it gives the sphere. Two exact
    // constructions composed, with no fitting anywhere.
    const Circle3 meridian(center_, axis_, x_axis_, radius_, v_lo_ + kHalfPi, v_hi_ + kHalfPi);
    int meridian_degree = 0;
    std::vector<double> meridian_knots;
    std::vector<Vec4d> meridian_control;
    if (!meridian.ToNurbs(&meridian_degree, &meridian_knots, &meridian_control)) return false;
    // Circle3 parameterizes from its own x axis, which here is the polar
    // axis; shift the knot range back to latitude.
    for (double &knot : meridian_knots) knot -= kHalfPi;
    *out_degree_u = 2;
    *out_degree_v = meridian_degree;
    return RevolveControlPoints(center_, axis_, u_lo_, u_hi_, meridian_knots, meridian_control, out_knots_u,
                                out_knots_v, out_control, out_count_u, out_count_v);
}

bool SphereSurface::IsClosedU(const Tolerance &tolerance) const {
    return std::fabs((u_hi_ - u_lo_) - kTwoPi) <= tolerance.angular;
}
bool SphereSurface::IsClosedV(const Tolerance &) const { return false; }

double SphereSurface::Area(double) const {
    return radius_ * radius_ * (u_hi_ - u_lo_) * (std::sin(v_hi_) - std::sin(v_lo_));
}

// ---------------------------------------------------------------------
// TorusSurface
// ---------------------------------------------------------------------

TorusSurface::TorusSurface(const Vec3d &center, const Vec3d &x_axis, const Vec3d &axis, double major_radius,
                           double minor_radius, double u_lo, double u_hi, double v_lo, double v_hi)
    : center_(center), major_(major_radius), minor_(minor_radius), u_lo_(u_lo), u_hi_(u_hi), v_lo_(v_lo),
      v_hi_(v_hi) {
    axis_ = axis.Normalized();
    const Vec3d projected = x_axis - axis_ * axis_.Dot(x_axis);
    x_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : axis_.AnyPerpendicular();
    y_axis_ = axis_.Cross(x_axis_);
}

std::unique_ptr<Surface> TorusSurface::Clone() const { return std::make_unique<TorusSurface>(*this); }

void TorusSurface::Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const {
    *u_lo = u_lo_;
    *u_hi = u_hi_;
    *v_lo = v_lo_;
    *v_hi = v_hi_;
}

Vec3d TorusSurface::Point(double u, double v) const {
    const double r = major_ + minor_ * std::cos(v);
    return center_ + x_axis_ * (r * std::cos(u)) + y_axis_ * (r * std::sin(u)) + axis_ * (minor_ * std::sin(v));
}

void TorusSurface::Derivatives(double u, double v, int max_derivative,
                               std::vector<std::vector<Vec3d>> *out) const {
    out->assign(Idx(max_derivative + 1), std::vector<Vec3d>(Idx(max_derivative + 1), Vec3d{}));
    for (int k = 0; k <= max_derivative; ++k) {
        for (int l = 0; l + k <= max_derivative; ++l) {
            const double uc = std::cos(u + static_cast<double>(k) * kHalfPi);
            const double us = std::sin(u + static_cast<double>(k) * kHalfPi);
            const double vc = std::cos(v + static_cast<double>(l) * kHalfPi);
            const double vs = std::sin(v + static_cast<double>(l) * kHalfPi);
            // The radial factor is (major + minor*cos v); differentiating
            // in v hits only the minor*cos v part, so the constant major
            // term survives only at l = 0.
            const double radial = (l == 0 ? major_ : 0.0) + minor_ * vc;
            Vec3d value = x_axis_ * (radial * uc) + y_axis_ * (radial * us);
            if (k == 0) value += axis_ * (minor_ * vs);
            (*out)[Idx(k)][Idx(l)] = value;
        }
    }
    (*out)[0][0] = Point(u, v);
}

Box3d TorusSurface::Bounds() const {
    Box3d box;
    const cad::Tolerance tolerance;
    if (IsClosedU(tolerance) && IsClosedV(tolerance)) {
        const double outer = major_ + minor_;
        // The exact box of a full torus: outer radius in the plane,
        // minor radius along the axis.
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                for (int sz = -1; sz <= 1; sz += 2) {
                    box.Expand(center_ + x_axis_ * (outer * static_cast<double>(sx)) +
                               y_axis_ * (outer * static_cast<double>(sy)) +
                               axis_ * (minor_ * static_cast<double>(sz)));
                }
            }
        }
        return box;
    }
    for (int i = 0; i <= 32; ++i) {
        const double u = u_lo_ + (u_hi_ - u_lo_) * static_cast<double>(i) / 32.0;
        for (int j = 0; j <= 32; ++j) {
            const double v = v_lo_ + (v_hi_ - v_lo_) * static_cast<double>(j) / 32.0;
            box.Expand(Point(u, v));
        }
    }
    return box;
}

void TorusSurface::Transform(const Mat4d &transform) {
    center_ = transform.TransformPoint(center_);
    const Vec3d x = transform.TransformVector(x_axis_);
    const double scale = x.Length();
    major_ *= scale;
    minor_ *= scale;
    axis_ = transform.TransformVector(axis_).Normalized();
    const Vec3d projected = x - axis_ * axis_.Dot(x);
    x_axis_ = projected.LengthSquared() > 0.0 ? projected.Normalized() : axis_.AnyPerpendicular();
    y_axis_ = axis_.Cross(x_axis_);
    if (transform.LinearDeterminant() < 0.0) {
        // See CylinderSurface::Transform: a reflection makes u run the
        // other way, so the trimmed domain flips with it.
        const double lo = u_lo_;
        u_lo_ = -u_hi_;
        u_hi_ = -lo;
    }
}

bool TorusSurface::ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                           std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                           int *out_count_v) const {
    // The generatrix is the tube's own circle, in the plane containing
    // the axis and the x direction, centred at distance `major` out.
    const Vec3d tube_center = center_ + x_axis_ * major_;
    const Circle3 tube(tube_center, x_axis_, axis_, minor_, v_lo_, v_hi_);
    int tube_degree = 0;
    std::vector<double> tube_knots;
    std::vector<Vec4d> tube_control;
    if (!tube.ToNurbs(&tube_degree, &tube_knots, &tube_control)) return false;
    *out_degree_u = 2;
    *out_degree_v = tube_degree;
    return RevolveControlPoints(center_, axis_, u_lo_, u_hi_, tube_knots, tube_control, out_knots_u, out_knots_v,
                                out_control, out_count_u, out_count_v);
}

bool TorusSurface::IsClosedU(const Tolerance &tolerance) const {
    return std::fabs((u_hi_ - u_lo_) - kTwoPi) <= tolerance.angular;
}
bool TorusSurface::IsClosedV(const Tolerance &tolerance) const {
    return std::fabs((v_hi_ - v_lo_) - kTwoPi) <= tolerance.angular;
}

double TorusSurface::Area(double) const {
    // Integral of |Su||Sv| = minor*(major + minor*cos v) over the domain.
    return minor_ * major_ * (u_hi_ - u_lo_) * (v_hi_ - v_lo_) +
           minor_ * minor_ * (u_hi_ - u_lo_) * (std::sin(v_hi_) - std::sin(v_lo_));
}

// ---------------------------------------------------------------------
// NurbsSurface
// ---------------------------------------------------------------------

bool NurbsSurface::Create(int degree_u, int degree_v, const std::vector<double> &knots_u,
                          const std::vector<double> &knots_v, const std::vector<Vec4d> &control, int count_u,
                          int count_v, NurbsSurface *out, std::string *error) {
    if (!ValidateKnotVector(knots_u, degree_u, count_u, error)) return false;
    if (!ValidateKnotVector(knots_v, degree_v, count_v, error)) return false;
    if (control.size() != Idx(count_u) * Idx(count_v)) {
        *error = "control grid must hold count_u * count_v points";
        return false;
    }
    for (const Vec4d &c : control) {
        if (c.w <= 0.0) {
            *error = "control point weights must be positive";
            return false;
        }
    }
    out->degree_u_ = degree_u;
    out->degree_v_ = degree_v;
    out->knots_u_ = knots_u;
    out->knots_v_ = knots_v;
    out->control_ = control;
    out->count_u_ = count_u;
    out->count_v_ = count_v;
    return true;
}

std::unique_ptr<Surface> NurbsSurface::Clone() const { return std::make_unique<NurbsSurface>(*this); }

void NurbsSurface::Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const {
    KnotDomain(knots_u_, degree_u_, count_u_, u_lo, u_hi);
    KnotDomain(knots_v_, degree_v_, count_v_, v_lo, v_hi);
}

Vec3d NurbsSurface::Point(double u, double v) const {
    return SurfacePointHomogeneous(degree_u_, degree_v_, knots_u_, knots_v_, control_, count_u_, count_v_, u, v)
        .Project();
}

void NurbsSurface::Derivatives(double u, double v, int max_derivative,
                               std::vector<std::vector<Vec3d>> *out) const {
    std::vector<std::vector<Vec4d>> homogeneous;
    SurfaceDerivativesHomogeneous(degree_u_, degree_v_, knots_u_, knots_v_, control_, count_u_, count_v_, u, v,
                                  max_derivative, &homogeneous);
    RationalSurfaceDerivatives(homogeneous, max_derivative, out);
}

Box3d NurbsSurface::Bounds() const {
    Box3d box;
    for (const Vec4d &c : control_) box.Expand(c.Project());
    return box;
}

void NurbsSurface::Transform(const Mat4d &transform) {
    for (Vec4d &c : control_) c = Vec4d::FromWeighted(transform.TransformPoint(c.Project()), c.w);
}

bool NurbsSurface::ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                           std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                           int *out_count_v) const {
    *out_degree_u = degree_u_;
    *out_degree_v = degree_v_;
    *out_knots_u = knots_u_;
    *out_knots_v = knots_v_;
    *out_control = control_;
    *out_count_u = count_u_;
    *out_count_v = count_v_;
    return true;
}

bool NurbsSurface::IsClosedU(const Tolerance &tolerance) const {
    for (int j = 0; j < count_v_; ++j) {
        if (!tolerance.SamePoint(control_[SurfaceIndex(0, j, count_v_)].Project(),
                                 control_[SurfaceIndex(count_u_ - 1, j, count_v_)].Project())) {
            return false;
        }
    }
    return count_v_ > 0;
}

bool NurbsSurface::IsClosedV(const Tolerance &tolerance) const {
    for (int i = 0; i < count_u_; ++i) {
        if (!tolerance.SamePoint(control_[SurfaceIndex(i, 0, count_v_)].Project(),
                                 control_[SurfaceIndex(i, count_v_ - 1, count_v_)].Project())) {
            return false;
        }
    }
    return count_u_ > 0;
}

bool NurbsSurface::InsertKnot(bool in_u, double value, int multiplicity) {
    return InsertKnotSurface(degree_u_, degree_v_, &knots_u_, &knots_v_, &control_, &count_u_, &count_v_, in_u,
                             value, multiplicity);
}

// ---------------------------------------------------------------------
// ExtrusionSurface
// ---------------------------------------------------------------------

ExtrusionSurface::ExtrusionSurface(std::unique_ptr<Curve3> profile, const Vec3d &direction, double v_lo,
                                   double v_hi)
    : profile_(std::move(profile)), direction_(direction), v_lo_(v_lo), v_hi_(v_hi) {}

ExtrusionSurface::ExtrusionSurface(const ExtrusionSurface &other)
    : profile_(other.profile_ ? other.profile_->Clone() : nullptr), direction_(other.direction_),
      v_lo_(other.v_lo_), v_hi_(other.v_hi_) {}

ExtrusionSurface &ExtrusionSurface::operator=(const ExtrusionSurface &other) {
    if (this != &other) {
        profile_ = other.profile_ ? other.profile_->Clone() : nullptr;
        direction_ = other.direction_;
        v_lo_ = other.v_lo_;
        v_hi_ = other.v_hi_;
    }
    return *this;
}

std::unique_ptr<Surface> ExtrusionSurface::Clone() const { return std::make_unique<ExtrusionSurface>(*this); }

void ExtrusionSurface::Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const {
    profile_->Domain(u_lo, u_hi);
    *v_lo = v_lo_;
    *v_hi = v_hi_;
}

Vec3d ExtrusionSurface::Point(double u, double v) const { return profile_->Point(u) + direction_ * v; }

void ExtrusionSurface::Derivatives(double u, double v, int max_derivative,
                                   std::vector<std::vector<Vec3d>> *out) const {
    out->assign(Idx(max_derivative + 1), std::vector<Vec3d>(Idx(max_derivative + 1), Vec3d{}));
    std::vector<Vec3d> curve_ders;
    profile_->Derivatives(u, max_derivative, &curve_ders);
    // Linear in v, so only the pure-u partials and the single first v
    // derivative are non-zero; every mixed partial vanishes.
    for (int k = 0; k <= max_derivative; ++k) (*out)[Idx(k)][0] = curve_ders[Idx(k)];
    (*out)[0][0] = Point(u, v);
    if (max_derivative >= 1) (*out)[0][1] = direction_;
}

Box3d ExtrusionSurface::Bounds() const {
    Box3d box = profile_->Bounds();
    Box3d result;
    result.Expand(box.Min() + direction_ * v_lo_);
    result.Expand(box.Max() + direction_ * v_lo_);
    result.Expand(box.Min() + direction_ * v_hi_);
    result.Expand(box.Max() + direction_ * v_hi_);
    return result;
}

void ExtrusionSurface::Transform(const Mat4d &transform) {
    profile_->Transform(transform);
    direction_ = transform.TransformVector(direction_);
}

bool ExtrusionSurface::ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                               std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control,
                               int *out_count_u, int *out_count_v) const {
    int degree = 0;
    std::vector<double> knots;
    std::vector<Vec4d> control;
    if (!profile_->ToNurbs(&degree, &knots, &control)) return false;
    *out_degree_u = degree;
    *out_degree_v = 1;
    *out_knots_u = knots;
    *out_knots_v = {v_lo_, v_lo_, v_hi_, v_hi_};
    *out_count_u = static_cast<int>(control.size());
    *out_count_v = 2;
    out_control->assign(control.size() * 2, Vec4d{});
    for (std::size_t i = 0; i < control.size(); ++i) {
        const Vec3d base = control[i].Project();
        (*out_control)[SurfaceIndex(static_cast<int>(i), 0, 2)] =
            Vec4d::FromWeighted(base + direction_ * v_lo_, control[i].w);
        (*out_control)[SurfaceIndex(static_cast<int>(i), 1, 2)] =
            Vec4d::FromWeighted(base + direction_ * v_hi_, control[i].w);
    }
    return true;
}

bool ExtrusionSurface::IsClosedU(const Tolerance &tolerance) const { return profile_->IsClosed(tolerance); }
bool ExtrusionSurface::IsClosedV(const Tolerance &) const { return false; }

// ---------------------------------------------------------------------
// RevolutionSurface
// ---------------------------------------------------------------------

RevolutionSurface::RevolutionSurface(std::unique_ptr<Curve3> profile, const Vec3d &axis_origin,
                                     const Vec3d &axis_direction, double v_lo, double v_hi)
    : profile_(std::move(profile)), axis_origin_(axis_origin), axis_direction_(axis_direction.Normalized()),
      v_lo_(v_lo), v_hi_(v_hi) {}

RevolutionSurface::RevolutionSurface(const RevolutionSurface &other)
    : profile_(other.profile_ ? other.profile_->Clone() : nullptr), axis_origin_(other.axis_origin_),
      axis_direction_(other.axis_direction_), v_lo_(other.v_lo_), v_hi_(other.v_hi_) {}

RevolutionSurface &RevolutionSurface::operator=(const RevolutionSurface &other) {
    if (this != &other) {
        profile_ = other.profile_ ? other.profile_->Clone() : nullptr;
        axis_origin_ = other.axis_origin_;
        axis_direction_ = other.axis_direction_;
        v_lo_ = other.v_lo_;
        v_hi_ = other.v_hi_;
    }
    return *this;
}

std::unique_ptr<Surface> RevolutionSurface::Clone() const { return std::make_unique<RevolutionSurface>(*this); }

void RevolutionSurface::Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const {
    profile_->Domain(u_lo, u_hi);
    *v_lo = v_lo_;
    *v_hi = v_hi_;
}

Vec3d RevolutionSurface::Point(double u, double v) const {
    const Vec3d p = profile_->Point(u);
    const Vec3d on_axis = axis_origin_ + axis_direction_ * (p - axis_origin_).Dot(axis_direction_);
    const Vec3d radial = p - on_axis;
    const Vec3d perpendicular = axis_direction_.Cross(radial);
    return on_axis + radial * std::cos(v) + perpendicular * std::sin(v);
}

void RevolutionSurface::Derivatives(double u, double v, int max_derivative,
                                    std::vector<std::vector<Vec3d>> *out) const {
    // The profile's own derivatives rotate rigidly with the surface, so
    // each u partial is the rotated profile derivative and each v partial
    // differentiates the rotation. Built by composing the two rather than
    // by a finite difference, so the result is exact.
    out->assign(Idx(max_derivative + 1), std::vector<Vec3d>(Idx(max_derivative + 1), Vec3d{}));
    std::vector<Vec3d> profile_ders;
    profile_->Derivatives(u, max_derivative, &profile_ders);
    const double cv = std::cos(v);
    const double sv = std::sin(v);
    for (int k = 0; k <= max_derivative; ++k) {
        const Vec3d p = profile_ders[Idx(k)];
        // For k = 0 the point must be decomposed about the axis; for
        // higher derivatives the axis origin drops out and only the
        // direction matters.
        const Vec3d reference = (k == 0) ? (p - axis_origin_) : p;
        const Vec3d along = axis_direction_ * reference.Dot(axis_direction_);
        const Vec3d radial = reference - along;
        const Vec3d perpendicular = axis_direction_.Cross(radial);
        for (int l = 0; l + k <= max_derivative; ++l) {
            const double c = std::cos(v + static_cast<double>(l) * kHalfPi);
            const double s = std::sin(v + static_cast<double>(l) * kHalfPi);
            Vec3d value = radial * c + perpendicular * s;
            // The axial component and the origin do not rotate, so they
            // appear only in the l = 0 term.
            if (l == 0) value += along + ((k == 0) ? axis_origin_ : Vec3d{});
            (*out)[Idx(k)][Idx(l)] = value;
        }
    }
    (*out)[0][0] = Point(u, v);
    (void)cv;
    (void)sv;
}

Box3d RevolutionSurface::Bounds() const {
    Box3d box;
    double u_lo = 0.0, u_hi = 0.0;
    profile_->Domain(&u_lo, &u_hi);
    for (int i = 0; i <= 48; ++i) {
        const double u = u_lo + (u_hi - u_lo) * static_cast<double>(i) / 48.0;
        for (int j = 0; j <= 48; ++j) {
            const double v = v_lo_ + (v_hi_ - v_lo_) * static_cast<double>(j) / 48.0;
            box.Expand(Point(u, v));
        }
    }
    return box;
}

void RevolutionSurface::Transform(const Mat4d &transform) {
    profile_->Transform(transform);
    axis_origin_ = transform.TransformPoint(axis_origin_);
    axis_direction_ = transform.TransformVector(axis_direction_).Normalized();
}

bool RevolutionSurface::ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                                std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control,
                                int *out_count_u, int *out_count_v) const {
    int profile_degree = 0;
    std::vector<double> profile_knots;
    std::vector<Vec4d> profile_control;
    if (!profile_->ToNurbs(&profile_degree, &profile_knots, &profile_control)) return false;
    int degree_u = 2;
    int degree_v = profile_degree;
    if (!RevolveControlPoints(axis_origin_, axis_direction_, v_lo_, v_hi_, profile_knots, profile_control,
                              out_knots_u, out_knots_v, out_control, out_count_u, out_count_v)) {
        return false;
    }
    // A8.1 puts the revolution in u; this type's interface puts the
    // profile there, so the grid is transposed on the way out.
    TransposeGrid(out_control, out_count_u, out_count_v, out_knots_u, out_knots_v, &degree_u, &degree_v);
    *out_degree_u = degree_u;
    *out_degree_v = degree_v;
    return true;
}

bool RevolutionSurface::IsClosedU(const Tolerance &tolerance) const { return profile_->IsClosed(tolerance); }
bool RevolutionSurface::IsClosedV(const Tolerance &tolerance) const {
    return std::fabs((v_hi_ - v_lo_) - kTwoPi) <= tolerance.angular;
}

// ---------------------------------------------------------------------
// RuledSurface
// ---------------------------------------------------------------------

RuledSurface::RuledSurface(std::unique_ptr<Curve3> first, std::unique_ptr<Curve3> second)
    : first_(std::move(first)), second_(std::move(second)) {}

RuledSurface::RuledSurface(const RuledSurface &other)
    : first_(other.first_ ? other.first_->Clone() : nullptr),
      second_(other.second_ ? other.second_->Clone() : nullptr) {}

RuledSurface &RuledSurface::operator=(const RuledSurface &other) {
    if (this != &other) {
        first_ = other.first_ ? other.first_->Clone() : nullptr;
        second_ = other.second_ ? other.second_->Clone() : nullptr;
    }
    return *this;
}

std::unique_ptr<Surface> RuledSurface::Clone() const { return std::make_unique<RuledSurface>(*this); }

void RuledSurface::Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const {
    // The two curves are traversed in lockstep over a normalized [0,1]
    // parameter, so they need not share a domain -- which they generally
    // do not, one being an arc and the other a spline.
    *u_lo = 0.0;
    *u_hi = 1.0;
    *v_lo = 0.0;
    *v_hi = 1.0;
}

Vec3d RuledSurface::Point(double u, double v) const {
    double a_lo = 0.0, a_hi = 0.0, b_lo = 0.0, b_hi = 0.0;
    first_->Domain(&a_lo, &a_hi);
    second_->Domain(&b_lo, &b_hi);
    const Vec3d a = first_->Point(a_lo + u * (a_hi - a_lo));
    const Vec3d b = second_->Point(b_lo + u * (b_hi - b_lo));
    return a * (1.0 - v) + b * v;
}

void RuledSurface::Derivatives(double u, double v, int max_derivative,
                               std::vector<std::vector<Vec3d>> *out) const {
    out->assign(Idx(max_derivative + 1), std::vector<Vec3d>(Idx(max_derivative + 1), Vec3d{}));
    double a_lo = 0.0, a_hi = 0.0, b_lo = 0.0, b_hi = 0.0;
    first_->Domain(&a_lo, &a_hi);
    second_->Domain(&b_lo, &b_hi);
    const double a_scale = a_hi - a_lo;
    const double b_scale = b_hi - b_lo;
    std::vector<Vec3d> a_ders;
    std::vector<Vec3d> b_ders;
    first_->Derivatives(a_lo + u * a_scale, max_derivative, &a_ders);
    second_->Derivatives(b_lo + u * b_scale, max_derivative, &b_ders);
    // The chain rule for the reparameterization contributes a factor of
    // the domain length per derivative order.
    double a_factor = 1.0;
    double b_factor = 1.0;
    for (int k = 0; k <= max_derivative; ++k) {
        const Vec3d a = a_ders[Idx(k)] * a_factor;
        const Vec3d b = b_ders[Idx(k)] * b_factor;
        (*out)[Idx(k)][0] = a * (1.0 - v) + b * v;
        if (max_derivative - k >= 1) (*out)[Idx(k)][1] = b - a;
        a_factor *= a_scale;
        b_factor *= b_scale;
    }
}

Box3d RuledSurface::Bounds() const {
    Box3d box = first_->Bounds();
    box.Expand(second_->Bounds());
    return box;
}

void RuledSurface::Transform(const Mat4d &transform) {
    first_->Transform(transform);
    second_->Transform(transform);
}

bool RuledSurface::ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                           std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                           int *out_count_v) const {
    // Both sections must share a degree and a knot vector before they can
    // be the two rows of a tensor-product grid. That compatibility step
    // is the whole content of a two-section loft.
    const Curve3 *sections[2] = {first_.get(), second_.get()};
    std::vector<const Curve3 *> list(sections, sections + 2);
    NurbsSurface surface;
    std::string error;
    if (!LoftSurface(list, 1, Parameterization::Uniform, &surface, &error)) return false;
    return surface.ToNurbs(out_degree_u, out_degree_v, out_knots_u, out_knots_v, out_control, out_count_u,
                           out_count_v);
}

bool RuledSurface::IsClosedU(const Tolerance &tolerance) const {
    return first_->IsClosed(tolerance) && second_->IsClosed(tolerance);
}
bool RuledSurface::IsClosedV(const Tolerance &) const { return false; }

// ---------------------------------------------------------------------
// OffsetSurface
// ---------------------------------------------------------------------

OffsetSurface::OffsetSurface(std::unique_ptr<Surface> base, double distance)
    : base_(std::move(base)), distance_(distance) {}

OffsetSurface::OffsetSurface(const OffsetSurface &other)
    : base_(other.base_ ? other.base_->Clone() : nullptr), distance_(other.distance_) {}

OffsetSurface &OffsetSurface::operator=(const OffsetSurface &other) {
    if (this != &other) {
        base_ = other.base_ ? other.base_->Clone() : nullptr;
        distance_ = other.distance_;
    }
    return *this;
}

bool OffsetSurface::IsDegenerateAt(double u, double v) const {
    // The offset folds where the offset distance reaches the local radius
    // of curvature. The sign matters and is easy to get backwards, so it
    // is worth pinning down against a case with a known answer.
    //
    // This file's second fundamental form is II = <S_ij, n>, which for an
    // outward-oriented sphere of radius r gives principal curvatures of
    // -1/r (the surface curves *away* from its own normal). Under that
    // convention the offset of radius r by distance d has radius r + d,
    // degenerate at d = -r, and substituting k = -1/r shows the condition
    // is 1 - d*k <= 0, not the 1 + d*k that a convention with the
    // opposite sign would give.
    //
    // Checked both ways round in the tests: offsetting a sphere outward
    // is never degenerate, inward past its own radius always is.
    double k1 = 0.0;
    double k2 = 0.0;
    base_->PrincipalCurvatures(u, v, &k1, &k2);
    return (1.0 - distance_ * k1) <= 0.0 || (1.0 - distance_ * k2) <= 0.0;
}

std::unique_ptr<Surface> OffsetSurface::Clone() const { return std::make_unique<OffsetSurface>(*this); }

void OffsetSurface::Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const {
    base_->Domain(u_lo, u_hi, v_lo, v_hi);
}

Vec3d OffsetSurface::Point(double u, double v) const {
    return base_->Point(u, v) + base_->Normal(u, v) * distance_;
}

void OffsetSurface::Derivatives(double u, double v, int max_derivative,
                                std::vector<std::vector<Vec3d>> *out) const {
    // The analytic derivative of an offset needs the derivative of the
    // unit normal, which needs one order more from the base than is being
    // asked for and grows unpleasant past first order. Central
    // differences in parameter space are used instead -- accurate to
    // about 1e-9 for the first derivatives, which is enough for the
    // tessellation and shelling this type exists to serve, and honest
    // about not being exact.
    out->assign(Idx(max_derivative + 1), std::vector<Vec3d>(Idx(max_derivative + 1), Vec3d{}));
    (*out)[0][0] = Point(u, v);
    if (max_derivative < 1) return;
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    const double hu = std::max(1e-7, (u_hi - u_lo) * 1e-6);
    const double hv = std::max(1e-7, (v_hi - v_lo) * 1e-6);
    auto at = [&](double a, double b) { return Point(Clamp(a, u_lo, u_hi), Clamp(b, v_lo, v_hi)); };
    (*out)[1][0] = (at(u + hu, v) - at(u - hu, v)) / (2.0 * hu);
    (*out)[0][1] = (at(u, v + hv) - at(u, v - hv)) / (2.0 * hv);
    if (max_derivative >= 2) {
        (*out)[2][0] = (at(u + hu, v) - at(u, v) * 2.0 + at(u - hu, v)) / (hu * hu);
        (*out)[0][2] = (at(u, v + hv) - at(u, v) * 2.0 + at(u, v - hv)) / (hv * hv);
        (*out)[1][1] = (at(u + hu, v + hv) - at(u + hu, v - hv) - at(u - hu, v + hv) + at(u - hu, v - hv)) /
                       (4.0 * hu * hv);
    }
}

Box3d OffsetSurface::Bounds() const {
    Box3d box = base_->Bounds();
    const double pad = std::fabs(distance_);
    Box3d padded;
    padded.Expand(box.Min() - Vec3d{pad, pad, pad});
    padded.Expand(box.Max() + Vec3d{pad, pad, pad});
    return padded;
}

void OffsetSurface::Transform(const Mat4d &transform) { base_->Transform(transform); }

bool OffsetSurface::ToNurbs(int *, int *, std::vector<double> *, std::vector<double> *, std::vector<Vec4d> *,
                            int *, int *) const {
    // An offset of a NURBS is not a NURBS in general -- it is only
    // approximable, by fitting. Returning false rather than silently
    // producing an approximation keeps the "ToNurbs is exact" contract
    // that every other surface here honours; Part E.4 is where a fitted
    // offset belongs, with a tolerance the caller chooses.
    return false;
}

bool OffsetSurface::IsClosedU(const Tolerance &tolerance) const { return base_->IsClosedU(tolerance); }
bool OffsetSurface::IsClosedV(const Tolerance &tolerance) const { return base_->IsClosedV(tolerance); }

// ---------------------------------------------------------------------
// Construction operations
// ---------------------------------------------------------------------

bool LoftSurface(const std::vector<const Curve3 *> &sections, int degree_v, Parameterization parameterization,
                 NurbsSurface *out, std::string *error) {
    if (sections.size() < 2) {
        *error = "a loft needs at least two sections";
        return false;
    }
    // Step 1: every section as a NURBS.
    std::vector<int> degrees;
    std::vector<std::vector<double>> knots;
    std::vector<std::vector<Vec4d>> controls;
    for (const Curve3 *section : sections) {
        int degree = 0;
        std::vector<double> section_knots;
        std::vector<Vec4d> section_control;
        if (section == nullptr || !section->ToNurbs(&degree, &section_knots, &section_control)) {
            *error = "a section could not be converted to a NURBS";
            return false;
        }
        // Normalize each section's parameter range to [0,1] so sections
        // with different domains (an arc parameterized by angle next to a
        // spline parameterized 0..1) can be made compatible at all.
        const double lo = section_knots.front();
        const double hi = section_knots.back();
        if (!(hi > lo)) {
            *error = "a section has an empty parameter range";
            return false;
        }
        for (double &knot : section_knots) knot = (knot - lo) / (hi - lo);
        degrees.push_back(degree);
        knots.push_back(std::move(section_knots));
        controls.push_back(std::move(section_control));
    }

    // Step 2: a common degree.
    const int target_degree = *std::max_element(degrees.begin(), degrees.end());
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (degrees[i] < target_degree) {
            if (!ElevateDegree(&degrees[i], &knots[i], &controls[i], target_degree - degrees[i])) {
                *error = "degree elevation failed while making the sections compatible";
                return false;
            }
        }
    }

    // Step 3: a common knot vector -- the union of every section's
    // interior knots, at the maximum multiplicity any section gives it.
    // This is what lets sections of genuinely different shape be lofted:
    // each gains the others' knots without changing its own geometry.
    std::vector<KnotSpan> merged;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        for (const KnotSpan &span : InteriorKnots(knots[i], target_degree, static_cast<int>(controls[i].size()))) {
            bool found = false;
            for (KnotSpan &existing : merged) {
                if (std::fabs(existing.value - span.value) < 1e-12) {
                    existing.multiplicity = std::max(existing.multiplicity, span.multiplicity);
                    found = true;
                    break;
                }
            }
            if (!found) merged.push_back(span);
        }
    }
    std::sort(merged.begin(), merged.end(),
              [](const KnotSpan &a, const KnotSpan &b) { return a.value < b.value; });
    for (std::size_t i = 0; i < sections.size(); ++i) {
        std::vector<double> to_add;
        for (const KnotSpan &span : merged) {
            int present = 0;
            for (double knot : knots[i]) {
                if (std::fabs(knot - span.value) < 1e-12) ++present;
            }
            for (int k = present; k < span.multiplicity; ++k) to_add.push_back(span.value);
        }
        if (!to_add.empty()) {
            if (!RefineKnots(target_degree, &knots[i], &controls[i], to_add)) {
                *error = "knot refinement failed while making the sections compatible";
                return false;
            }
        }
    }
    const std::size_t control_count = controls.front().size();
    for (const std::vector<Vec4d> &control : controls) {
        if (control.size() != control_count) {
            *error = "sections could not be made compatible (differing control point counts after refinement)";
            return false;
        }
    }

    // Step 4: interpolate across the sections, control point by control
    // point. Each column of the grid is a curve through the corresponding
    // control point of every section.
    const int section_count = static_cast<int>(sections.size());
    const int v_degree = std::min(degree_v, section_count - 1);

    // One parameterization shared by every column, averaged over all of
    // them. This is the step that makes skinning work: interpolating each
    // column against parameters derived from its own point spacing gives
    // each column a different knot vector, and columns with different
    // knot vectors cannot be the rows of one tensor-product surface.
    // Averaging (rather than taking the first column's) is the standard
    // choice -- it keeps the parameterization representative of the whole
    // surface instead of whichever column happened to be first.
    std::vector<double> parameters(Idx(section_count), 0.0);
    {
        std::vector<double> accumulated(Idx(section_count), 0.0);
        for (std::size_t i = 0; i < control_count; ++i) {
            std::vector<Vec3d> column;
            for (int j = 0; j < section_count; ++j) column.push_back(controls[Idx(j)][i].Project());
            // Parameterization is a question about *distance* between
            // sections, so it is measured on the projected points; only
            // the interpolation itself needs to be homogeneous.
            const std::vector<double> column_parameters = ComputeParameters(column, parameterization);
            for (int j = 0; j < section_count; ++j) accumulated[Idx(j)] += column_parameters[Idx(j)];
        }
        const double inverse = 1.0 / static_cast<double>(control_count);
        for (int j = 0; j < section_count; ++j) parameters[Idx(j)] = accumulated[Idx(j)] * inverse;
        // Guard the degenerate case where every column collapsed to a
        // point and the averaged parameters are not increasing.
        bool increasing = true;
        for (int j = 1; j < section_count; ++j) {
            if (!(parameters[Idx(j)] > parameters[Idx(j - 1)])) increasing = false;
        }
        if (!increasing) {
            for (int j = 0; j < section_count; ++j) {
                parameters[Idx(j)] = static_cast<double>(j) / static_cast<double>(section_count - 1);
            }
        }
    }

    std::vector<Vec4d> grid(Idx(static_cast<int>(control_count)) * Idx(section_count), Vec4d{});
    std::vector<double> v_knots;
    if (v_degree <= 1) {
        // Linear across sections: the control points are the sections'
        // own, and the knot vector is the parameters clamped.
        v_knots.assign(Idx(section_count + 2), 0.0);
        v_knots[0] = 0.0;
        for (int j = 0; j < section_count; ++j) v_knots[Idx(j + 1)] = parameters[Idx(j)];
        v_knots[Idx(section_count + 1)] = 1.0;
        for (std::size_t i = 0; i < control_count; ++i) {
            for (int j = 0; j < section_count; ++j) {
                grid[SurfaceIndex(static_cast<int>(i), j, section_count)] = controls[Idx(j)][i];
            }
        }
    } else {
        for (std::size_t i = 0; i < control_count; ++i) {
            // Homogeneous, not projected: the sections' weights have to
            // be interpolated too, or every rational section (every arc
            // and circle) degrades into a polynomial through its own
            // control points. See InterpolateHomogeneous' own note.
            std::vector<Vec4d> column;
            for (int j = 0; j < section_count; ++j) column.push_back(controls[Idx(j)][i]);
            std::vector<double> column_knots;
            std::vector<Vec4d> column_control;
            if (!InterpolateHomogeneous(column, v_degree, parameters, &column_knots, &column_control)) {
                *error = "interpolation across the sections failed";
                return false;
            }
            if (i == 0) v_knots = column_knots;
            for (std::size_t j = 0; j < column_control.size(); ++j) {
                grid[SurfaceIndex(static_cast<int>(i), static_cast<int>(j), section_count)] = column_control[j];
            }
        }
    }
    return NurbsSurface::Create(target_degree, v_degree, knots.front(), v_knots, grid,
                                static_cast<int>(control_count), section_count, out, error);
}

std::vector<Frame> RotationMinimizingFrames(const Curve3 &curve, const std::vector<double> &parameters,
                                            const Vec3d &initial_normal) {
    // Wang's double-reflection method. Each frame is carried to the next
    // by two reflections, which is exact to second order and -- unlike
    // simple projection -- introduces no drift that accumulates along a
    // long spine.
    std::vector<Frame> frames;
    if (parameters.empty()) return frames;
    Vec3d normal = initial_normal;
    const Vec3d first_tangent = curve.Tangent(parameters.front());
    if (normal.LengthSquared() <= 0.0 || std::fabs(normal.Dot(first_tangent)) > 0.99) {
        normal = first_tangent.AnyPerpendicular();
    } else {
        normal = (normal - first_tangent * normal.Dot(first_tangent)).Normalized();
    }

    Frame current;
    current.parameter = parameters.front();
    current.origin = curve.Point(parameters.front());
    current.tangent = first_tangent;
    current.normal = normal;
    current.binormal = first_tangent.Cross(normal);
    frames.push_back(current);

    for (std::size_t i = 1; i < parameters.size(); ++i) {
        const Vec3d next_origin = curve.Point(parameters[i]);
        const Vec3d next_tangent = curve.Tangent(parameters[i]);
        // First reflection: through the plane bisecting the two points.
        const Vec3d v1 = next_origin - current.origin;
        const double c1 = v1.Dot(v1);
        Vec3d reflected_normal = current.normal;
        Vec3d reflected_tangent = current.tangent;
        if (c1 > 0.0) {
            reflected_normal = current.normal - v1 * (2.0 / c1 * v1.Dot(current.normal));
            reflected_tangent = current.tangent - v1 * (2.0 / c1 * v1.Dot(current.tangent));
        }
        // Second reflection: through the plane bisecting the two tangents,
        // which lands the frame on the new tangent exactly.
        const Vec3d v2 = next_tangent - reflected_tangent;
        const double c2 = v2.Dot(v2);
        Vec3d next_normal = reflected_normal;
        if (c2 > 0.0) next_normal = reflected_normal - v2 * (2.0 / c2 * v2.Dot(reflected_normal));
        next_normal = (next_normal - next_tangent * next_normal.Dot(next_tangent)).Normalized();

        Frame frame;
        frame.parameter = parameters[i];
        frame.origin = next_origin;
        frame.tangent = next_tangent;
        frame.normal = next_normal;
        frame.binormal = next_tangent.Cross(next_normal);
        frames.push_back(frame);
        current = frame;
    }
    return frames;
}

bool SweepSurface(const Curve3 &profile, const Curve3 &spine, int sample_count, NurbsSurface *out,
                  std::string *error) {
    if (sample_count < 2) {
        *error = "a sweep needs at least two spine samples";
        return false;
    }
    double spine_lo = 0.0;
    double spine_hi = 0.0;
    spine.Domain(&spine_lo, &spine_hi);
    std::vector<double> parameters;
    for (int i = 0; i < sample_count; ++i) {
        parameters.push_back(spine_lo + (spine_hi - spine_lo) * static_cast<double>(i) /
                                            static_cast<double>(sample_count - 1));
    }
    const std::vector<Frame> frames = RotationMinimizingFrames(spine, parameters);

    // Place a copy of the profile in each frame, then loft through them.
    // The profile is interpreted in its own local xy plane, which is the
    // convention every CAD sweep uses.
    double profile_lo = 0.0;
    double profile_hi = 0.0;
    profile.Domain(&profile_lo, &profile_hi);
    const Vec3d profile_origin = profile.Point(profile_lo);
    (void)profile_origin;

    std::vector<std::unique_ptr<Curve3>> placed;
    std::vector<const Curve3 *> sections;
    for (const Frame &frame : frames) {
        std::unique_ptr<Curve3> copy = profile.Clone();
        // The transform taking the profile's own frame (origin, x, y) to
        // this spine frame (origin, normal, binormal).
        const Mat4d placement = Mat4d::Frame(frame.origin, frame.tangent, frame.normal);
        copy->Transform(placement);
        sections.push_back(copy.get());
        placed.push_back(std::move(copy));
    }
    return LoftSurface(sections, 3, Parameterization::ChordLength, out, error);
}

}  // namespace cad
