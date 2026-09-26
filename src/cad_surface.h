#ifndef MEP_CAD_SURFACE_H
#define MEP_CAD_SURFACE_H

#include "cad_curve.h"
#include "cad_math.h"
#include "cad_nurbs.h"

#include <memory>
#include <string>
#include <vector>

// Surface geometry (plans/CAD_FEM_PLAN.md Parts A.1, A.3, A.4 and A.5).
//
// Same argument as cad_curve.h for keeping the analytic families
// first-class: a plane that knows it is a plane intersects another plane
// in closed form (Part C.2), survives a STEP round-trip as a PLANE, and
// can answer "what is the radius of this hole" without reverse-
// engineering control points.
//
// One addition here with no curve counterpart: the fundamental forms.
// They are not decoration -- the first fundamental form is the metric
// tensor that the surface mesher in Part G.2 needs in order to generate
// well-shaped elements. Meshing a face by running a plain 2D Delaunay in
// (u,v) produces triangles that are well-shaped *in parameter space* and
// can be arbitrarily distorted in 3D, because the parameterization is not
// an isometry. Refining under the induced metric instead is what makes
// the elements right, and that metric is E, F, G below.
namespace cad {

enum class SurfaceKind { Plane, Cylinder, Cone, Sphere, Torus, Nurbs, Extrusion, Revolution, Ruled, Offset };

class Surface {
public:
    virtual ~Surface() = default;

    virtual SurfaceKind Kind() const = 0;
    virtual std::unique_ptr<Surface> Clone() const = 0;

    virtual void Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const = 0;
    virtual Vec3d Point(double u, double v) const = 0;

    // Partial derivatives up to total order `max_derivative`, indexed
    // out[k][l] = the (k in u, l in v) partial. out[0][0] is the point.
    virtual void Derivatives(double u, double v, int max_derivative,
                             std::vector<std::vector<Vec3d>> *out) const = 0;

    virtual Box3d Bounds() const = 0;
    virtual void Transform(const Mat4d &transform) = 0;

    // The exact NURBS form. As with curves the *shape* is exact for every
    // analytic family; the parameterization inside a span is not
    // preserved for anything built on a conic (see Curve3::ToNurbs for
    // why that is inherent rather than a defect).
    virtual bool ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                         std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                         int *out_count_v) const = 0;

    // Whether the surface closes on itself in each direction -- a
    // cylinder in u, a torus in both. The topology in Part B needs this
    // to know where a face has a seam rather than a boundary.
    virtual bool IsClosedU(const Tolerance &tolerance = {}) const = 0;
    virtual bool IsClosedV(const Tolerance &tolerance = {}) const = 0;

    // --- Derived quantities -------------------------------------------

    // The unit normal, from the cross product of the two tangents. Zero
    // at a degenerate point (a sphere's pole, a cone's apex), which is a
    // real feature of those surfaces rather than an error -- callers
    // near a pole must handle it, and Part B.2's p-curve construction is
    // where that first matters.
    Vec3d Normal(double u, double v) const;

    // The first fundamental form: E = Su.Su, F = Su.Sv, G = Sv.Sv. The
    // metric induced on parameter space by the embedding -- see this
    // file's header for why the mesher needs it.
    void FirstFundamentalForm(double u, double v, double *e, double *f, double *g) const;

    // The second fundamental form: L = Suu.n, M = Suv.n, N = Svv.n.
    //
    // Note the sign convention, because differential geometry texts are
    // split on it and the difference is invisible until something like an
    // offset silently folds the wrong way: with the *outward* normal this
    // convention makes a convex surface's curvatures negative. A sphere
    // of radius r oriented outward has principal curvatures -1/r, mean
    // curvature -1/r, and Gaussian curvature +1/r^2 (positive, since the
    // two signs multiply). Callers that want the "radius of curvature"
    // want 1/|k|.
    void SecondFundamentalForm(double u, double v, double *l, double *m, double *n) const;

    // Gaussian and mean curvature, from the two forms. The curvature-
    // driven sizing field in Part G.1 reads these: a face gets smaller
    // elements where it bends faster, and "faster" means these.
    double GaussianCurvature(double u, double v) const;
    double MeanCurvature(double u, double v) const;
    // The two principal curvatures, sorted so |k1| >= |k2|. The larger in
    // magnitude is what bounds the element size.
    void PrincipalCurvatures(double u, double v, double *k1, double *k2) const;

    // Surface area over the whole domain, by quadrature of the area
    // element sqrt(EG - F^2). Analytic surfaces override this where a
    // closed form exists.
    virtual double Area(double tolerance = 1e-8) const;

    // --- Point inversion (Part A.5) ------------------------------------

    // The closest point on the surface to `point`. Same two-stage shape
    // as Curve3::ClosestPoint and for the same reason -- a coarse grid
    // locates the basin, then Newton refines -- but the Newton step here
    // is a 2x2 solve rather than a scalar division, since both parameters
    // move together.
    bool ClosestPoint(const Vec3d &point, double *out_u, double *out_v, Vec3d *out_point = nullptr,
                      const Tolerance &tolerance = {}, int sample_count = 24) const;

    bool ContainsPoint(const Vec3d &point, double *out_u = nullptr, double *out_v = nullptr,
                       const Tolerance &tolerance = {}) const;

    // An isoparametric curve, as a NURBS. The bridge from surface
    // questions to curve questions that tessellation and the marching in
    // Part C.3 both take.
    //
    // `fixed_value` is in the surface's *NURBS* parameterization, which
    // shares the surface's domain and agrees with the surface's own
    // parameter at every span boundary but not strictly inside a span --
    // the same inherent mismatch Curve3::ToNurbs describes. Extracting at
    // v = 1.1 on a torus therefore lands at a true angle of about 1.086.
    // The curve is exactly on the surface either way; it is the label on
    // the parameter that differs.
    //
    // Callers that need an isocurve at a specific *surface* parameter
    // should extract at a span boundary (where the two agree exactly), or
    // invert with ClosestPoint. Tessellation and marching, the two
    // consumers this exists for, care only that the curve is on the
    // surface and that consecutive extractions sweep it monotonically --
    // both of which hold.
    bool IsoCurve(bool fix_u, double fixed_value, NurbsCurve3 *out) const;
};

// ---------------------------------------------------------------------
// Analytic surfaces
// ---------------------------------------------------------------------

// S(u,v) = origin + u*x_axis + v*y_axis, over a rectangular domain.
class PlaneSurface : public Surface {
public:
    PlaneSurface() = default;
    PlaneSurface(const Vec3d &origin, const Vec3d &x_axis, const Vec3d &y_axis, double u_lo = -1.0,
                 double u_hi = 1.0, double v_lo = -1.0, double v_hi = 1.0);

    const Vec3d &Origin() const { return origin_; }
    Vec3d PlaneNormal() const { return x_axis_.Cross(y_axis_); }
    // Signed distance from the plane -- positive on the normal's side.
    double SignedDistance(const Vec3d &point) const;

    SurfaceKind Kind() const override { return SurfaceKind::Plane; }
    std::unique_ptr<Surface> Clone() const override;
    void Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const override;
    Vec3d Point(double u, double v) const override;
    void Derivatives(double u, double v, int max_derivative, std::vector<std::vector<Vec3d>> *out) const override;
    Box3d Bounds() const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                 std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                 int *out_count_v) const override;
    bool IsClosedU(const Tolerance &tolerance = {}) const override;
    bool IsClosedV(const Tolerance &tolerance = {}) const override;
    double Area(double tolerance = 1e-8) const override;

private:
    Vec3d origin_;
    Vec3d x_axis_{1.0, 0.0, 0.0};
    Vec3d y_axis_{0.0, 1.0, 0.0};
    double u_lo_ = -1.0, u_hi_ = 1.0, v_lo_ = -1.0, v_hi_ = 1.0;
};

// S(u,v) = origin + r*(cos(u)*x + sin(u)*y) + v*axis. u is the angle, v
// the height -- the convention STEP's CYLINDRICAL_SURFACE uses, so an
// imported cylinder needs no reparameterization.
class CylinderSurface : public Surface {
public:
    CylinderSurface() = default;
    CylinderSurface(const Vec3d &origin, const Vec3d &x_axis, const Vec3d &axis, double radius,
                    double u_lo = 0.0, double u_hi = kTwoPi, double v_lo = 0.0, double v_hi = 1.0);

    double Radius() const { return radius_; }
    const Vec3d &Axis() const { return axis_; }
    const Vec3d &Origin() const { return origin_; }

    SurfaceKind Kind() const override { return SurfaceKind::Cylinder; }
    std::unique_ptr<Surface> Clone() const override;
    void Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const override;
    Vec3d Point(double u, double v) const override;
    void Derivatives(double u, double v, int max_derivative, std::vector<std::vector<Vec3d>> *out) const override;
    Box3d Bounds() const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                 std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                 int *out_count_v) const override;
    bool IsClosedU(const Tolerance &tolerance = {}) const override;
    bool IsClosedV(const Tolerance &tolerance = {}) const override;
    double Area(double tolerance = 1e-8) const override;

private:
    Vec3d origin_;
    Vec3d x_axis_{1.0, 0.0, 0.0};
    Vec3d y_axis_{0.0, 1.0, 0.0};
    Vec3d axis_{0.0, 0.0, 1.0};
    double radius_ = 1.0;
    double u_lo_ = 0.0, u_hi_ = kTwoPi, v_lo_ = 0.0, v_hi_ = 1.0;
};

// S(u,v) = apex + v*(cos(half_angle)*axis) + v*sin(half_angle)*(cos u * x
// + sin u * y), i.e. v is distance along the axis from the apex. The
// apex (v = 0) is a genuine singular point where the normal is undefined,
// and the domain is allowed to include it -- rejecting it would make a
// full cone unrepresentable.
class ConeSurface : public Surface {
public:
    ConeSurface() = default;
    ConeSurface(const Vec3d &apex, const Vec3d &x_axis, const Vec3d &axis, double half_angle, double u_lo = 0.0,
                double u_hi = kTwoPi, double v_lo = 0.0, double v_hi = 1.0);

    double HalfAngle() const { return half_angle_; }
    const Vec3d &Apex() const { return apex_; }
    const Vec3d &Axis() const { return axis_; }

    SurfaceKind Kind() const override { return SurfaceKind::Cone; }
    std::unique_ptr<Surface> Clone() const override;
    void Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const override;
    Vec3d Point(double u, double v) const override;
    void Derivatives(double u, double v, int max_derivative, std::vector<std::vector<Vec3d>> *out) const override;
    Box3d Bounds() const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                 std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                 int *out_count_v) const override;
    bool IsClosedU(const Tolerance &tolerance = {}) const override;
    bool IsClosedV(const Tolerance &tolerance = {}) const override;

private:
    Vec3d apex_;
    Vec3d x_axis_{1.0, 0.0, 0.0};
    Vec3d y_axis_{0.0, 1.0, 0.0};
    Vec3d axis_{0.0, 0.0, 1.0};
    double half_angle_ = 0.5;
    double u_lo_ = 0.0, u_hi_ = kTwoPi, v_lo_ = 0.0, v_hi_ = 1.0;
};

// S(u,v) = center + r*(cos(v)*(cos(u)*x + sin(u)*y) + sin(v)*axis), with
// u the longitude and v the latitude in [-pi/2, pi/2]. Both poles are
// singular, same as the cone's apex.
class SphereSurface : public Surface {
public:
    SphereSurface() = default;
    SphereSurface(const Vec3d &center, double radius, const Vec3d &x_axis = Vec3d{1, 0, 0},
                  const Vec3d &axis = Vec3d{0, 0, 1}, double u_lo = 0.0, double u_hi = kTwoPi,
                  double v_lo = -kHalfPi, double v_hi = kHalfPi);

    const Vec3d &Center() const { return center_; }
    double Radius() const { return radius_; }
    const Vec3d &Axis() const { return axis_; }

    SurfaceKind Kind() const override { return SurfaceKind::Sphere; }
    std::unique_ptr<Surface> Clone() const override;
    void Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const override;
    Vec3d Point(double u, double v) const override;
    void Derivatives(double u, double v, int max_derivative, std::vector<std::vector<Vec3d>> *out) const override;
    Box3d Bounds() const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                 std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                 int *out_count_v) const override;
    bool IsClosedU(const Tolerance &tolerance = {}) const override;
    bool IsClosedV(const Tolerance &tolerance = {}) const override;
    double Area(double tolerance = 1e-8) const override;

private:
    Vec3d center_;
    Vec3d x_axis_{1.0, 0.0, 0.0};
    Vec3d y_axis_{0.0, 1.0, 0.0};
    Vec3d axis_{0.0, 0.0, 1.0};
    double radius_ = 1.0;
    double u_lo_ = 0.0, u_hi_ = kTwoPi, v_lo_ = -kHalfPi, v_hi_ = kHalfPi;
};

// A torus of major radius R about `axis` and minor radius r. u runs
// around the axis, v around the tube. Unlike the sphere and cone this one
// has no singular point as long as r < R.
class TorusSurface : public Surface {
public:
    TorusSurface() = default;
    TorusSurface(const Vec3d &center, const Vec3d &x_axis, const Vec3d &axis, double major_radius,
                 double minor_radius, double u_lo = 0.0, double u_hi = kTwoPi, double v_lo = 0.0,
                 double v_hi = kTwoPi);

    double MajorRadius() const { return major_; }
    double MinorRadius() const { return minor_; }
    const Vec3d &Center() const { return center_; }
    const Vec3d &Axis() const { return axis_; }

    SurfaceKind Kind() const override { return SurfaceKind::Torus; }
    std::unique_ptr<Surface> Clone() const override;
    void Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const override;
    Vec3d Point(double u, double v) const override;
    void Derivatives(double u, double v, int max_derivative, std::vector<std::vector<Vec3d>> *out) const override;
    Box3d Bounds() const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                 std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                 int *out_count_v) const override;
    bool IsClosedU(const Tolerance &tolerance = {}) const override;
    bool IsClosedV(const Tolerance &tolerance = {}) const override;
    double Area(double tolerance = 1e-8) const override;

private:
    Vec3d center_;
    Vec3d x_axis_{1.0, 0.0, 0.0};
    Vec3d y_axis_{0.0, 1.0, 0.0};
    Vec3d axis_{0.0, 0.0, 1.0};
    double major_ = 2.0;
    double minor_ = 0.5;
    double u_lo_ = 0.0, u_hi_ = kTwoPi, v_lo_ = 0.0, v_hi_ = kTwoPi;
};

// ---------------------------------------------------------------------
// NURBS surface
// ---------------------------------------------------------------------

class NurbsSurface : public Surface {
public:
    NurbsSurface() = default;
    static bool Create(int degree_u, int degree_v, const std::vector<double> &knots_u,
                       const std::vector<double> &knots_v, const std::vector<Vec4d> &control, int count_u,
                       int count_v, NurbsSurface *out, std::string *error);

    int DegreeU() const { return degree_u_; }
    int DegreeV() const { return degree_v_; }
    int CountU() const { return count_u_; }
    int CountV() const { return count_v_; }
    const std::vector<double> &KnotsU() const { return knots_u_; }
    const std::vector<double> &KnotsV() const { return knots_v_; }
    const std::vector<Vec4d> &Control() const { return control_; }

    SurfaceKind Kind() const override { return SurfaceKind::Nurbs; }
    std::unique_ptr<Surface> Clone() const override;
    void Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const override;
    Vec3d Point(double u, double v) const override;
    void Derivatives(double u, double v, int max_derivative, std::vector<std::vector<Vec3d>> *out) const override;
    Box3d Bounds() const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                 std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                 int *out_count_v) const override;
    bool IsClosedU(const Tolerance &tolerance = {}) const override;
    bool IsClosedV(const Tolerance &tolerance = {}) const override;

    bool InsertKnot(bool in_u, double value, int multiplicity = 1);

private:
    int degree_u_ = 3;
    int degree_v_ = 3;
    int count_u_ = 0;
    int count_v_ = 0;
    std::vector<double> knots_u_;
    std::vector<double> knots_v_;
    std::vector<Vec4d> control_;
};

// ---------------------------------------------------------------------
// Derived surfaces (Part A.4)
// ---------------------------------------------------------------------

// A profile curve swept along a straight direction. S(u,v) = C(u) + v*d.
class ExtrusionSurface : public Surface {
public:
    ExtrusionSurface() = default;
    ExtrusionSurface(std::unique_ptr<Curve3> profile, const Vec3d &direction, double v_lo = 0.0,
                     double v_hi = 1.0);
    ExtrusionSurface(const ExtrusionSurface &other);
    ExtrusionSurface &operator=(const ExtrusionSurface &other);

    SurfaceKind Kind() const override { return SurfaceKind::Extrusion; }
    std::unique_ptr<Surface> Clone() const override;
    void Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const override;
    Vec3d Point(double u, double v) const override;
    void Derivatives(double u, double v, int max_derivative, std::vector<std::vector<Vec3d>> *out) const override;
    Box3d Bounds() const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                 std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                 int *out_count_v) const override;
    bool IsClosedU(const Tolerance &tolerance = {}) const override;
    bool IsClosedV(const Tolerance &tolerance = {}) const override;

private:
    std::unique_ptr<Curve3> profile_;
    Vec3d direction_{0.0, 0.0, 1.0};
    double v_lo_ = 0.0, v_hi_ = 1.0;
};

// A profile curve revolved about an axis. u is the profile parameter and
// v the rotation angle.
class RevolutionSurface : public Surface {
public:
    RevolutionSurface() = default;
    RevolutionSurface(std::unique_ptr<Curve3> profile, const Vec3d &axis_origin, const Vec3d &axis_direction,
                      double v_lo = 0.0, double v_hi = kTwoPi);
    RevolutionSurface(const RevolutionSurface &other);
    RevolutionSurface &operator=(const RevolutionSurface &other);

    SurfaceKind Kind() const override { return SurfaceKind::Revolution; }
    std::unique_ptr<Surface> Clone() const override;
    void Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const override;
    Vec3d Point(double u, double v) const override;
    void Derivatives(double u, double v, int max_derivative, std::vector<std::vector<Vec3d>> *out) const override;
    Box3d Bounds() const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                 std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                 int *out_count_v) const override;
    bool IsClosedU(const Tolerance &tolerance = {}) const override;
    bool IsClosedV(const Tolerance &tolerance = {}) const override;

private:
    std::unique_ptr<Curve3> profile_;
    Vec3d axis_origin_;
    Vec3d axis_direction_{0.0, 0.0, 1.0};
    double v_lo_ = 0.0, v_hi_ = kTwoPi;
};

// The straight-line interpolation between two curves. The two-section
// loft, and the simplest surface that is not a primitive.
class RuledSurface : public Surface {
public:
    RuledSurface() = default;
    RuledSurface(std::unique_ptr<Curve3> first, std::unique_ptr<Curve3> second);
    RuledSurface(const RuledSurface &other);
    RuledSurface &operator=(const RuledSurface &other);

    SurfaceKind Kind() const override { return SurfaceKind::Ruled; }
    std::unique_ptr<Surface> Clone() const override;
    void Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const override;
    Vec3d Point(double u, double v) const override;
    void Derivatives(double u, double v, int max_derivative, std::vector<std::vector<Vec3d>> *out) const override;
    Box3d Bounds() const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                 std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                 int *out_count_v) const override;
    bool IsClosedU(const Tolerance &tolerance = {}) const override;
    bool IsClosedV(const Tolerance &tolerance = {}) const override;

private:
    std::unique_ptr<Curve3> first_;
    std::unique_ptr<Curve3> second_;
};

// An offset of another surface along its own normal. Evaluated lazily:
// an offset surface self-intersects wherever the offset distance exceeds
// the local radius of curvature, and resolving that needs trimming, which
// is Part E.4's job and not representable here. What this type does is
// evaluate correctly wherever the offset is well-defined, and report
// where it is not.
class OffsetSurface : public Surface {
public:
    OffsetSurface() = default;
    OffsetSurface(std::unique_ptr<Surface> base, double distance);
    OffsetSurface(const OffsetSurface &other);
    OffsetSurface &operator=(const OffsetSurface &other);

    double Distance() const { return distance_; }
    // True where the offset is degenerate -- the offset distance has
    // reached the local radius of curvature and the surface folds. Part
    // E.4 trims these regions away; this reports them.
    bool IsDegenerateAt(double u, double v) const;

    SurfaceKind Kind() const override { return SurfaceKind::Offset; }
    std::unique_ptr<Surface> Clone() const override;
    void Domain(double *u_lo, double *u_hi, double *v_lo, double *v_hi) const override;
    Vec3d Point(double u, double v) const override;
    void Derivatives(double u, double v, int max_derivative, std::vector<std::vector<Vec3d>> *out) const override;
    Box3d Bounds() const override;
    void Transform(const Mat4d &transform) override;
    bool ToNurbs(int *out_degree_u, int *out_degree_v, std::vector<double> *out_knots_u,
                 std::vector<double> *out_knots_v, std::vector<Vec4d> *out_control, int *out_count_u,
                 int *out_count_v) const override;
    bool IsClosedU(const Tolerance &tolerance = {}) const override;
    bool IsClosedV(const Tolerance &tolerance = {}) const override;

private:
    std::unique_ptr<Surface> base_;
    double distance_ = 0.0;
};

// ---------------------------------------------------------------------
// Construction operations (Part A.4)
//
// Unlike the types above, these are *operations* that produce a
// NurbsSurface rather than surface kinds of their own. That is how they
// are used -- nobody wants a "loft surface" that has to re-run the
// skinning algorithm on every evaluation -- and it keeps the evaluation
// path in this file finite.
// ---------------------------------------------------------------------

// Skins a surface through a sequence of section curves. The sections are
// made compatible first (raised to a common degree and a merged knot
// vector), which is the step that makes lofting through curves of
// different shapes work at all.
bool LoftSurface(const std::vector<const Curve3 *> &sections, int degree_v, Parameterization parameterization,
                 NurbsSurface *out, std::string *error);

// Sweeps a profile along a spine curve.
//
// The frame is rotation-minimizing (the double-reflection method), not
// Frenet. That matters: the Frenet frame is undefined wherever the spine
// is momentarily straight, and it spins through 180 degrees at an
// inflection, so a Frenet sweep along an S-curve twists the profile
// visibly. A rotation-minimizing frame is defined everywhere the tangent
// is and carries no such twist.
bool SweepSurface(const Curve3 &profile, const Curve3 &spine, int sample_count, NurbsSurface *out,
                  std::string *error);

// The rotation-minimizing frames along a curve, sampled at the given
// parameters. Exposed because the sweep is not the only thing that wants
// them -- Part E.2's swept features and Part J.2's tube rendering do too.
struct Frame {
    double parameter = 0.0;
    Vec3d origin;
    Vec3d tangent;
    Vec3d normal;
    Vec3d binormal;
};
std::vector<Frame> RotationMinimizingFrames(const Curve3 &curve, const std::vector<double> &parameters,
                                            const Vec3d &initial_normal = Vec3d{});

}  // namespace cad

#endif
