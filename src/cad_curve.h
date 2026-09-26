#ifndef MEP_CAD_CURVE_H
#define MEP_CAD_CURVE_H

#include "cad_math.h"
#include "cad_nurbs.h"

#include <memory>
#include <string>
#include <vector>

// Curve geometry (plans/CAD_FEM_PLAN.md Parts A.1, A.2 and A.5): one
// interface, four implementations, and the point-inversion every other
// part of the kernel leans on.
//
// The analytic curves stay first-class rather than being converted to
// NURBS on sight, which is a decision worth stating plainly because the
// opposite is tempting: a kernel that represents everything as a NURBS
// has one evaluation path and one intersection routine instead of many.
// It also has no idea that two of its curves are circles. Concretely,
// that costs three things this kernel is not willing to give up:
//
//   * Exact intersections. Line/line, line/circle and circle/circle have
//     closed-form answers. Fed to a general NURBS intersector they become
//     iterative searches that are slower and, near tangency, less
//     reliable -- and Part C.2's whole argument is that the analytic
//     cases are the common ones.
//   * Lossless interchange. A STEP file that came in carrying a CIRCLE
//     should go out carrying a CIRCLE, not a rational quadratic
//     approximation of one. Downstream CAM and inspection software reads
//     that distinction.
//   * Honest reporting. "This edge is a circular arc of radius 5" is
//     something a user asks for, and reconstructing it from control
//     points afterwards is both lossy and unnecessary.
//
// Every analytic curve can still produce its exact NURBS form on demand
// (ToNurbs), which is what lofting, offsetting and STEP export use.
namespace cad {

enum class CurveKind { Line, Circle, Ellipse, Nurbs };

class Curve3 {
public:
    virtual ~Curve3() = default;

    virtual CurveKind Kind() const = 0;
    virtual std::unique_ptr<Curve3> Clone() const = 0;

    // The parameter interval this curve is defined over. Analytic curves
    // carry a real interval (an arc is a circle plus an angle range), so
    // this is not always [0,1] and callers must not assume it is.
    virtual void Domain(double *lo, double *hi) const = 0;

    virtual Vec3d Point(double u) const = 0;

    // Derivatives 0..max_derivative at `u`, so a caller that needs the
    // point and the tangent and the curvature pays for one evaluation
    // rather than three. Orders above what the curve can support come
    // back as zero rather than as an error.
    virtual void Derivatives(double u, int max_derivative, std::vector<Vec3d> *out) const = 0;

    // A bound on the curve, not necessarily a tight one. Analytic curves
    // return an exact box; a NURBS returns its control polygon's box,
    // which contains the curve by the convex hull property and is usually
    // somewhat larger. Callers that need tightness subdivide.
    virtual Box3d Bounds() const = 0;

    virtual bool IsClosed(const Tolerance &tolerance = {}) const = 0;

    virtual void Transform(const Mat4d &transform) = 0;

    // The exact NURBS representation. Every curve here has one -- that is
    // what makes NURBS the lingua franca -- and for the analytic families
    // the *shape* is exact, not approximate: a NURBS circle passes through
    // radius r to machine precision, not to a fitting tolerance.
    //
    // What is NOT preserved, and this matters enough to state plainly
    // because it is invisible until it bites: the parameterization inside
    // a span. The conversion preserves the domain [lo, hi], both
    // endpoints, and every internal span boundary exactly -- but a
    // rational quadratic traverses its arc non-uniformly in its own
    // parameter, so for a half-circle the NURBS at the parameter meaning
    // "22.5 degrees" is actually at 21.6 degrees. This is not a defect to
    // be fixed: no NURBS can be parameterized by arc angle, since that
    // would need transcendental basis functions, and the standard
    // construction trades parameterization for exactness of shape.
    //
    // The consequence for callers: never assume curve.Point(u) equals
    // nurbs.Point(u) away from a span boundary. Where a genuine parameter
    // correspondence is needed -- constructing the p-curve of an edge on
    // a face (Part B.2) is the case that will care -- invert one against
    // the other with ClosestPoint rather than pairing parameters.
    virtual bool ToNurbs(int *out_degree, std::vector<double> *out_knots,
                         std::vector<Vec4d> *out_control) const = 0;

    // Reverses the direction of travel in place: the curve traces the
    // same point set in the opposite order, so the old start point is the
    // new end point.
    //
    // The domain is NOT guaranteed to be preserved, and callers must
    // re-read Domain() afterwards. Line3, Circle3 and NurbsCurve3 do keep
    // it; Ellipse3 cannot, because a reversed elliptical arc has no
    // canonical representation over the original parameter range (see
    // Ellipse3::Reverse for the algebra). Promising domain preservation
    // in the interface would mean either a lie or an ellipse that silently
    // changes shape, so the interface promises the weaker, true thing.
    virtual void Reverse() = 0;

    // --- Derived quantities, implemented once on top of Derivatives ----

    // Unit tangent. Zero-length at a cusp or a stationary point, which
    // callers test for rather than being protected from -- a curve that
    // genuinely has a cusp is not an error.
    Vec3d Tangent(double u) const;

    // |C' x C''| / |C'|^3. Zero on a straight curve; 1/r on a circle of
    // radius r.
    double Curvature(double u) const;

    // The principal normal: the unit vector in the osculating plane
    // pointing toward the centre of curvature. Zero where curvature is.
    Vec3d Normal(double u) const;

    // Arc length over [from, to], by adaptive quadrature of |C'(u)|.
    // Line and Circle override this with their closed forms -- not
    // primarily for speed, but because quadrature of a constant is exact
    // anyway and the closed form says so without a tolerance argument.
    virtual double Length(double from, double to, double tolerance = 1e-10) const;
    double Length(double tolerance = 1e-10) const;

    // --- Point inversion (Part A.5) ------------------------------------

    // The closest point on the curve to `point`. Returns false only if the
    // curve is degenerate; otherwise it always produces *a* local minimum,
    // and `out_u` is clamped to the domain.
    //
    // Two-stage, and both stages are necessary. A coarse sampling locates
    // the right basin -- a bare Newton started at the domain midpoint
    // converges to whichever local minimum it happens to find, and a curve
    // that loops back on itself has several. Newton then refines, solving
    // C'(u).(C(u) - P) = 0, which is the condition that the residual
    // vector is perpendicular to the tangent.
    //
    // `sample_count` controls the coarse stage. The default is generous
    // because the failure it prevents (converging to the wrong branch) is
    // silent, whereas its cost is a few dozen evaluations.
    bool ClosestPoint(const Vec3d &point, double *out_u, Vec3d *out_point = nullptr,
                      const Tolerance &tolerance = {}, int sample_count = 64) const;

    // Whether `point` lies on the curve to within tolerance, and where.
    bool ContainsPoint(const Vec3d &point, double *out_u = nullptr, const Tolerance &tolerance = {}) const;

    // Parameters at which the curve's smoothness drops -- the interior
    // knot values for a NURBS, empty for the analytic families, which are
    // analytic everywhere. Tessellate seeds its subdivision here so that
    // a kink is never straddled by a single segment: recursive halving
    // alone can step right over one if it happens to land near a
    // midpoint, and the result is a visibly cut corner.
    virtual std::vector<double> NaturalBreaks() const { return {}; }

    // Samples the curve adaptively so that the polyline through the
    // samples is within `chord_tolerance` of the true curve, and no
    // segment turns by more than `angle_tolerance` radians. The
    // tessellation primitive everything visual is built on, and the
    // seeding step for the intersection code in Part C.
    void Tessellate(std::vector<double> *out_parameters, std::vector<Vec3d> *out_points,
                    double chord_tolerance = 1e-3, double angle_tolerance = 0.2,
                    int max_samples = 10000) const;
};

// ---------------------------------------------------------------------
// Line
// ---------------------------------------------------------------------

// A bounded straight segment. Parameterized by arc length when
// `direction` is a unit vector, which the constructor does not require --
// a caller building a line from two points wants the parameter to run
// 0..1, and forcing unit speed would make that awkward.
class Line3 : public Curve3 {
public:
    Line3() = default;
    Line3(const Vec3d &origin, const Vec3d &direction, double lo = 0.0, double hi = 1.0);
    // The segment from `a` to `b`, with the domain [0,1].
    static Line3 FromPoints(const Vec3d &a, const Vec3d &b);

    const Vec3d &Origin() const { return origin_; }
    const Vec3d &Direction() const { return direction_; }

    CurveKind Kind() const override { return CurveKind::Line; }
    std::unique_ptr<Curve3> Clone() const override;
    void Domain(double *lo, double *hi) const override;
    Vec3d Point(double u) const override;
    void Derivatives(double u, int max_derivative, std::vector<Vec3d> *out) const override;
    Box3d Bounds() const override;
    bool IsClosed(const Tolerance &tolerance = {}) const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree, std::vector<double> *out_knots, std::vector<Vec4d> *out_control) const override;
    void Reverse() override;
    // Overriding one overload of Length hides the other, so it is pulled
    // back in explicitly -- otherwise curve.Length() stops compiling for
    // exactly the two types that have an exact closed form for it.
    using Curve3::Length;
    double Length(double from, double to, double tolerance) const override;

private:
    Vec3d origin_;
    Vec3d direction_{1.0, 0.0, 0.0};
    double lo_ = 0.0;
    double hi_ = 1.0;
};

// ---------------------------------------------------------------------
// Circle and ellipse
// ---------------------------------------------------------------------

// A circular arc, parameterized by angle in radians. The frame is stored
// as an explicit orthonormal pair rather than as a normal alone, because
// the x axis is what fixes where the parameter's zero lies -- and an arc
// that loses that has lost its start point.
class Circle3 : public Curve3 {
public:
    Circle3() = default;
    Circle3(const Vec3d &center, const Vec3d &x_axis, const Vec3d &y_axis, double radius, double start_angle = 0.0,
            double end_angle = kTwoPi);
    // A full circle in the plane with the given normal, with the x axis
    // chosen arbitrarily but deterministically.
    static Circle3 FromCenterNormalRadius(const Vec3d &center, const Vec3d &normal, double radius);
    // The circle through three non-collinear points. Returns false if they
    // are collinear (or coincident) -- a real, common case when fitting,
    // not an error.
    static bool FromThreePoints(const Vec3d &a, const Vec3d &b, const Vec3d &c, Circle3 *out,
                                const Tolerance &tolerance = {});

    const Vec3d &Center() const { return center_; }
    double Radius() const { return radius_; }
    // The plane's normal. Named distinctly from Curve3::Normal(u), which
    // is the *principal* normal -- the direction toward the centre of
    // curvature at a parameter. Both are "the normal" in ordinary speech
    // and they are not the same vector, so neither gets the bare name.
    Vec3d PlaneNormal() const { return x_axis_.Cross(y_axis_); }
    const Vec3d &XAxis() const { return x_axis_; }
    const Vec3d &YAxis() const { return y_axis_; }

    CurveKind Kind() const override { return CurveKind::Circle; }
    std::unique_ptr<Curve3> Clone() const override;
    void Domain(double *lo, double *hi) const override;
    Vec3d Point(double u) const override;
    void Derivatives(double u, int max_derivative, std::vector<Vec3d> *out) const override;
    Box3d Bounds() const override;
    bool IsClosed(const Tolerance &tolerance = {}) const override;
    void Transform(const Mat4d &transform) override;
    // The exact rational representation: piecewise quadratic, one segment
    // per 90-degree span (or fewer for a short arc). Exact, not fitted --
    // no polynomial can trace a circle, and this is the construction that
    // makes NURBS able to.
    bool ToNurbs(int *out_degree, std::vector<double> *out_knots, std::vector<Vec4d> *out_control) const override;
    void Reverse() override;
    // Overriding one overload of Length hides the other, so it is pulled
    // back in explicitly -- otherwise curve.Length() stops compiling for
    // exactly the two types that have an exact closed form for it.
    using Curve3::Length;
    double Length(double from, double to, double tolerance) const override;

private:
    Vec3d center_;
    Vec3d x_axis_{1.0, 0.0, 0.0};
    Vec3d y_axis_{0.0, 1.0, 0.0};
    double radius_ = 1.0;
    double start_ = 0.0;
    double end_ = kTwoPi;
};

// An elliptical arc. Same frame convention as Circle3, with the two radii
// along the stored axes.
class Ellipse3 : public Curve3 {
public:
    Ellipse3() = default;
    Ellipse3(const Vec3d &center, const Vec3d &x_axis, const Vec3d &y_axis, double major_radius,
             double minor_radius, double start_angle = 0.0, double end_angle = kTwoPi);

    const Vec3d &Center() const { return center_; }
    double MajorRadius() const { return major_; }
    double MinorRadius() const { return minor_; }
    // The same three Circle3 exposes, and for the same reason: anything
    // writing this out has to say which way the major axis points.
    Vec3d PlaneNormal() const { return x_axis_.Cross(y_axis_); }
    const Vec3d &XAxis() const { return x_axis_; }
    const Vec3d &YAxis() const { return y_axis_; }

    CurveKind Kind() const override { return CurveKind::Ellipse; }
    std::unique_ptr<Curve3> Clone() const override;
    void Domain(double *lo, double *hi) const override;
    Vec3d Point(double u) const override;
    void Derivatives(double u, int max_derivative, std::vector<Vec3d> *out) const override;
    Box3d Bounds() const override;
    bool IsClosed(const Tolerance &tolerance = {}) const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree, std::vector<double> *out_knots, std::vector<Vec4d> *out_control) const override;
    void Reverse() override;

private:
    Vec3d center_;
    Vec3d x_axis_{1.0, 0.0, 0.0};
    Vec3d y_axis_{0.0, 1.0, 0.0};
    double major_ = 1.0;
    double minor_ = 1.0;
    double start_ = 0.0;
    double end_ = kTwoPi;
};

// ---------------------------------------------------------------------
// NURBS curve
// ---------------------------------------------------------------------

class NurbsCurve3 : public Curve3 {
public:
    NurbsCurve3() = default;
    // Returns false (leaving the object empty) if the knot vector does not
    // match the degree and control point count -- see ValidateKnotVector.
    static bool Create(int degree, const std::vector<double> &knots, const std::vector<Vec4d> &control,
                       NurbsCurve3 *out, std::string *error);
    // Convenience for the non-rational case.
    static bool CreateFromPoints(int degree, const std::vector<double> &knots, const std::vector<Vec3d> &control,
                                 NurbsCurve3 *out, std::string *error);
    // Interpolation and approximation, wrapping cad_nurbs.h's fitting.
    static bool Interpolate(const std::vector<Vec3d> &points, int degree, Parameterization parameterization,
                            NurbsCurve3 *out);
    static bool Approximate(const std::vector<Vec3d> &points, int degree, int control_point_count,
                            Parameterization parameterization, NurbsCurve3 *out);

    int Degree() const { return degree_; }
    const std::vector<double> &Knots() const { return knots_; }
    const std::vector<Vec4d> &Control() const { return control_; }
    bool IsRational(const Tolerance &tolerance = {}) const;

    CurveKind Kind() const override { return CurveKind::Nurbs; }
    std::unique_ptr<Curve3> Clone() const override;
    void Domain(double *lo, double *hi) const override;
    Vec3d Point(double u) const override;
    void Derivatives(double u, int max_derivative, std::vector<Vec3d> *out) const override;
    Box3d Bounds() const override;
    bool IsClosed(const Tolerance &tolerance = {}) const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree, std::vector<double> *out_knots, std::vector<Vec4d> *out_control) const override;
    void Reverse() override;
    std::vector<double> NaturalBreaks() const override;

    // Shape-preserving edits, forwarding to cad_nurbs.h.
    bool InsertKnot(double u, int multiplicity = 1);
    int RemoveKnot(double u, int times, double tolerance);
    bool ElevateDegree(int times);
    bool Split(double u, NurbsCurve3 *left, NurbsCurve3 *right) const;

private:
    int degree_ = 3;
    std::vector<double> knots_;
    std::vector<Vec4d> control_;
};

// Builds the exact NURBS form of any curve and returns it as a
// NurbsCurve3 -- the conversion every caller that wants one uniform
// representation goes through.
bool CurveToNurbs(const Curve3 &curve, NurbsCurve3 *out);

}  // namespace cad

#endif
