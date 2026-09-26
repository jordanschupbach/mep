#ifndef MEP_CAD_MATH_H
#define MEP_CAD_MATH_H

#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <vector>

// The double-precision math core under mep's CAD kernel and FEM stack
// (plans/CAD_FEM_PLAN.md Part 0.1).
//
// Deliberately NOT an extension of gfx/vecmath.h, and deliberately not
// sharing a single type with it: that header is float, graphics-shaped
// (its Matrix is a GL-upload layout, see this repo's own
// UploadMatrix/glUniformMatrix4fv note) and its precision is fine for a
// renderer and nowhere near enough for exact geometry. A surface-surface
// intersection marches along a curve correcting back onto two surfaces by
// Newton iteration; in float that iteration stalls at roughly 1e-4 of the
// model size, which is larger than the tolerances a CAD boolean has to
// decide coincidence at. So the kernel is double from top to bottom and
// converts to float exactly once, in the tessellator that produces
// MeshData for display (Part B.5). A float leaking upward from there
// shows up much later as an intermittent boolean failure, which is why
// the two type families are kept visibly distinct rather than made
// interoperable.
namespace cad {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 6.28318530717958647692;
inline constexpr double kHalfPi = 1.57079632679489661923;

// Machine epsilon for double, and a "scaled zero" a few ulps above it --
// the floor below which no tolerance is meaningful regardless of what a
// caller asks for.
inline constexpr double kEps = std::numeric_limits<double>::epsilon();

inline double Square(double x) { return x * x; }
inline double Clamp(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline double DegToRad(double d) { return d * (kPi / 180.0); }
inline double RadToDeg(double r) { return r * (180.0 / kPi); }

// ---------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------

struct Vec2d {
    double x = 0.0, y = 0.0;

    constexpr Vec2d() = default;
    constexpr Vec2d(double xx, double yy) : x(xx), y(yy) {}

    double &operator[](std::size_t i) { return i == 0 ? x : y; }
    const double &operator[](std::size_t i) const { return i == 0 ? x : y; }

    Vec2d operator-() const { return {-x, -y}; }
    Vec2d operator+(const Vec2d &o) const { return {x + o.x, y + o.y}; }
    Vec2d operator-(const Vec2d &o) const { return {x - o.x, y - o.y}; }
    Vec2d operator*(double s) const { return {x * s, y * s}; }
    Vec2d operator/(double s) const { return {x / s, y / s}; }
    Vec2d &operator+=(const Vec2d &o) { x += o.x; y += o.y; return *this; }
    Vec2d &operator-=(const Vec2d &o) { x -= o.x; y -= o.y; return *this; }
    Vec2d &operator*=(double s) { x *= s; y *= s; return *this; }
    Vec2d &operator/=(double s) { x /= s; y /= s; return *this; }

    double Dot(const Vec2d &o) const { return x * o.x + y * o.y; }
    // The 2D "cross product": the z of the 3D cross of (x,y,0) with
    // (o.x,o.y,0). Signed area of the parallelogram, so it doubles as the
    // orientation test used all over the 2D sketcher -- though note that
    // for *robust* orientation the predicates in cad_predicates.h are the
    // ones to use, not this.
    double Cross(const Vec2d &o) const { return x * o.y - y * o.x; }
    double LengthSquared() const { return x * x + y * y; }
    double Length() const { return std::sqrt(LengthSquared()); }
    // Rotated a quarter turn counter-clockwise. The 2D normal, used by
    // the sketch constraint Jacobians often enough to earn a name.
    Vec2d Perp() const { return {-y, x}; }
    Vec2d Normalized() const {
        const double len = Length();
        return len > 0.0 ? Vec2d{x / len, y / len} : Vec2d{0.0, 0.0};
    }
};

inline Vec2d operator*(double s, const Vec2d &v) { return v * s; }

struct Vec3d {
    double x = 0.0, y = 0.0, z = 0.0;

    constexpr Vec3d() = default;
    constexpr Vec3d(double xx, double yy, double zz) : x(xx), y(yy), z(zz) {}

    double &operator[](std::size_t i) { return i == 0 ? x : (i == 1 ? y : z); }
    const double &operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : z); }

    Vec3d operator-() const { return {-x, -y, -z}; }
    Vec3d operator+(const Vec3d &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3d operator-(const Vec3d &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3d operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3d operator/(double s) const { return {x / s, y / s, z / s}; }
    Vec3d &operator+=(const Vec3d &o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3d &operator-=(const Vec3d &o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3d &operator*=(double s) { x *= s; y *= s; z *= s; return *this; }
    Vec3d &operator/=(double s) { x /= s; y /= s; z /= s; return *this; }

    double Dot(const Vec3d &o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3d Cross(const Vec3d &o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double LengthSquared() const { return x * x + y * y + z * z; }
    double Length() const { return std::sqrt(LengthSquared()); }
    Vec3d Normalized() const {
        const double len = Length();
        return len > 0.0 ? Vec3d{x / len, y / len, z / len} : Vec3d{0.0, 0.0, 0.0};
    }
    // Any unit vector perpendicular to this one. Picks the world axis this
    // vector is *least* aligned with before crossing, which is what keeps
    // the result well-conditioned no matter which way `this` points -- the
    // naive "always cross with Z" loses all precision for a vector that is
    // itself nearly Z, and that case is not rare (every extrusion along
    // the default up axis hits it).
    Vec3d AnyPerpendicular() const;
};

inline Vec3d operator*(double s, const Vec3d &v) { return v * s; }

// Homogeneous point, for rational geometry (plans/CAD_FEM_PLAN.md Part
// A.2). A NURBS curve or surface is a *rational* B-spline: its control
// points carry weights, and it is evaluated by running the ordinary
// polynomial B-spline machinery on the weighted points (w*x, w*y, w*z, w)
// and dividing through by the last component at the end. That is not a
// trick -- it is what makes exact circles, cylinders and spheres
// representable at all, since no polynomial can trace a conic.
//
// Deliberately separate from Vec3d rather than a base of it: mixing the
// two up is the classic NURBS bug, and the type system is the cheapest
// place to catch it. Conversion is explicit in both directions.
struct Vec4d {
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;

    constexpr Vec4d() = default;
    constexpr Vec4d(double xx, double yy, double zz, double ww) : x(xx), y(yy), z(zz), w(ww) {}
    // The weighted form of a Euclidean point: (w*p, w).
    static Vec4d FromWeighted(const Vec3d &p, double weight) {
        return {p.x * weight, p.y * weight, p.z * weight, weight};
    }

    double &operator[](std::size_t i) { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    const double &operator[](std::size_t i) const { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }

    Vec4d operator+(const Vec4d &o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Vec4d operator-(const Vec4d &o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    Vec4d operator*(double s) const { return {x * s, y * s, z * s, w * s}; }
    Vec4d operator/(double s) const { return {x / s, y / s, z / s, w / s}; }
    Vec4d &operator+=(const Vec4d &o) { x += o.x; y += o.y; z += o.z; w += o.w; return *this; }
    Vec4d &operator-=(const Vec4d &o) { x -= o.x; y -= o.y; z -= o.z; w -= o.w; return *this; }

    // The xyz part on its own -- the *weighted* coordinates, not a point.
    Vec3d Weighted() const { return {x, y, z}; }
    // Perspective divide back to a Euclidean point. A zero weight is a
    // malformed control point rather than a point at infinity as far as
    // this kernel is concerned; it returns the origin instead of
    // infinities, and the validity checks reject it upstream.
    Vec3d Project() const { return (w != 0.0) ? Vec3d{x / w, y / w, z / w} : Vec3d{}; }
};

inline Vec4d operator*(double s, const Vec4d &v) { return v * s; }

// Signed volume of the parallelepiped -- a.Dot(b.Cross(c)). Named because
// it reads far better than the nested form at every call site that wants
// "are these three directions coplanar / is this tet inverted".
inline double ScalarTriple(const Vec3d &a, const Vec3d &b, const Vec3d &c) {
    return a.Dot(b.Cross(c));
}

// ---------------------------------------------------------------------
// Tolerance
// ---------------------------------------------------------------------

// The tolerance model, carried per entity rather than kept as one global
// epsilon. This is not over-engineering: a single global tolerance is the
// classic reason a kernel that works on its own primitives falls over on
// imported data. A STEP file from another system arrives with edges that
// only meet to 1e-4 because that is what *its* kernel guaranteed, while
// the same model's freshly-built features meet to 1e-9. One global value
// must then be either too loose (distinct features get merged) or too
// tight (the imported body will not stitch). Storing a tolerance on each
// vertex, edge and face lets both coexist, and lets an operation report
// the tolerance of its own result rather than inheriting a guess.
struct Tolerance {
    // Absolute distance below which two points are the same point. The
    // default is the value most kernels settle on for models measured in
    // millimetres: tight enough that it is far below any real feature,
    // loose enough to absorb the accumulated error of a few hundred
    // floating-point operations.
    double linear = 1e-7;
    // Radians below which two directions are parallel. Not derived from
    // `linear`: an angle is scale-free and a length is not, so tying them
    // together makes the angular test wrong at every scale but one.
    double angular = 1e-9;
    // Relative companion to `linear`, applied to magnitudes rather than
    // differences -- used where the quantity compared grows with the model
    // (a parameter range, a curve length) and a fixed absolute value would
    // be meaningless at both extremes of scale.
    double relative = 1e-12;

    bool IsZero(double v) const { return std::fabs(v) <= linear; }
    bool SameScalar(double a, double b) const { return std::fabs(a - b) <= linear; }
    bool SamePoint(const Vec3d &a, const Vec3d &b) const {
        return (a - b).LengthSquared() <= linear * linear;
    }
    bool SamePoint(const Vec2d &a, const Vec2d &b) const {
        return (a - b).LengthSquared() <= linear * linear;
    }
    // Both directions assumed unit-length. Compares the sine of the angle
    // between them (the cross product's magnitude) rather than the cosine:
    // near parallel, cos is flat to second order and loses most of its
    // significant digits, while sin is linear and keeps them.
    bool SameDirection(const Vec3d &a, const Vec3d &b) const {
        return a.Cross(b).Length() <= angular && a.Dot(b) > 0.0;
    }
    bool ParallelDirection(const Vec3d &a, const Vec3d &b) const {
        return a.Cross(b).Length() <= angular;
    }
    // Scale-aware equality for a quantity of magnitude ~`scale`.
    bool SameRelative(double a, double b, double scale) const {
        return std::fabs(a - b) <= linear + relative * std::fabs(scale);
    }
};

// ---------------------------------------------------------------------
// Intervals
// ---------------------------------------------------------------------

// A closed real interval with outward-rounded-in-spirit arithmetic. Used
// by the subdivision half of surface-surface intersection (Part C.3) to
// prove that a pair of patches *cannot* intersect and cull them without
// any further work, and by the sizing field in the mesher. Deliberately
// not a fully rigorous interval type: it does not switch the FPU rounding
// mode per operation, so the bounds are tight-but-not-certified. That is
// the right trade here -- the culling test is followed by an exact
// refinement step either way, so a bound that is off by an ulp costs one
// extra subdivision, not a wrong answer. Anywhere a *proof* is needed,
// the adaptive predicates in cad_predicates.h are the tool instead.
struct Interval {
    double lo = 0.0, hi = 0.0;

    constexpr Interval() = default;
    constexpr explicit Interval(double v) : lo(v), hi(v) {}
    constexpr Interval(double l, double h) : lo(l), hi(h) {}

    static Interval Empty() {
        return {std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest()};
    }
    bool IsEmpty() const { return lo > hi; }
    double Mid() const { return 0.5 * (lo + hi); }
    double Width() const { return hi - lo; }
    bool Contains(double v) const { return v >= lo && v <= hi; }
    bool Overlaps(const Interval &o) const { return lo <= o.hi && o.lo <= hi; }
    void Expand(double v) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    void Expand(const Interval &o) {
        if (o.lo < lo) lo = o.lo;
        if (o.hi > hi) hi = o.hi;
    }
    // Grow by `d` on both ends -- the tolerance pad applied before an
    // overlap test, so two boxes that touch within tolerance are reported
    // as overlapping rather than missed.
    Interval Padded(double d) const { return {lo - d, hi + d}; }

    Interval operator+(const Interval &o) const { return {lo + o.lo, hi + o.hi}; }
    Interval operator-(const Interval &o) const { return {lo - o.hi, hi - o.lo}; }
    Interval operator*(const Interval &o) const;
};

// Axis-aligned bounding box in 3D, as three intervals. The workhorse of
// every culling step in the kernel.
struct Box3d {
    Interval x = Interval::Empty();
    Interval y = Interval::Empty();
    Interval z = Interval::Empty();

    bool IsEmpty() const { return x.IsEmpty() || y.IsEmpty() || z.IsEmpty(); }
    void Expand(const Vec3d &p) {
        x.Expand(p.x);
        y.Expand(p.y);
        z.Expand(p.z);
    }
    void Expand(const Box3d &o) {
        x.Expand(o.x);
        y.Expand(o.y);
        z.Expand(o.z);
    }
    bool Overlaps(const Box3d &o) const { return x.Overlaps(o.x) && y.Overlaps(o.y) && z.Overlaps(o.z); }
    bool Overlaps(const Box3d &o, double pad) const {
        return x.Padded(pad).Overlaps(o.x) && y.Padded(pad).Overlaps(o.y) && z.Padded(pad).Overlaps(o.z);
    }
    Vec3d Min() const { return {x.lo, y.lo, z.lo}; }
    Vec3d Max() const { return {x.hi, y.hi, z.hi}; }
    Vec3d Center() const { return {x.Mid(), y.Mid(), z.Mid()}; }
    Vec3d Extent() const { return {x.Width(), y.Width(), z.Width()}; }
    // Longest edge -- the natural scale of whatever this box bounds, and
    // so the multiplier for any relative tolerance applied to it.
    double Diagonal() const { return Extent().Length(); }
};

// ---------------------------------------------------------------------
// Matrices
// ---------------------------------------------------------------------

// Row-major 3x3. Jacobians, metric tensors (the first fundamental form
// the surface mesher works in, Part G.2), inertia tensors, and rotations.
struct Mat3d {
    // m[row][col].
    double m[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};

    static Mat3d Identity() { return {}; }
    static Mat3d Zero();
    // Columns as the three given vectors -- the usual way a Jacobian or a
    // frame gets built.
    static Mat3d FromColumns(const Vec3d &c0, const Vec3d &c1, const Vec3d &c2);
    static Mat3d FromRows(const Vec3d &r0, const Vec3d &r1, const Vec3d &r2);
    // Right-handed rotation of `angle` radians about a unit `axis`
    // (Rodrigues). Unnormalized axes are normalized first.
    static Mat3d Rotation(const Vec3d &axis, double angle);

    Vec3d Column(std::size_t i) const { return {m[0][i], m[1][i], m[2][i]}; }
    Vec3d Row(std::size_t i) const { return {m[i][0], m[i][1], m[i][2]}; }

    Vec3d operator*(const Vec3d &v) const;
    Mat3d operator*(const Mat3d &o) const;
    Mat3d operator+(const Mat3d &o) const;
    Mat3d operator*(double s) const;

    double Determinant() const;
    Mat3d Transposed() const;
    // Returns false (leaving *out untouched) when |det| <= tol.linear --
    // a singular Jacobian is a real, expected outcome at a surface pole or
    // a degenerate element, and the callers that hit it need to branch on
    // it rather than receive infinities.
    bool Inverse(Mat3d *out, const Tolerance &tol = {}) const;
    // Solves M*x = b. Same false-on-singular contract as Inverse.
    bool Solve(const Vec3d &b, Vec3d *out, const Tolerance &tol = {}) const;
};

// Row-major 4x4 affine transform. Rigid motions, scaling, and the
// placement matrices STEP carries on every representation item.
struct Mat4d {
    double m[4][4] = {{1.0, 0.0, 0.0, 0.0},
                      {0.0, 1.0, 0.0, 0.0},
                      {0.0, 0.0, 1.0, 0.0},
                      {0.0, 0.0, 0.0, 1.0}};

    static Mat4d Identity() { return {}; }
    static Mat4d Translation(const Vec3d &t);
    static Mat4d Scaling(const Vec3d &s);
    static Mat4d Rotation(const Vec3d &axis, double angle);
    // The frame with origin `origin` and the given axes as its X/Y/Z --
    // i.e. the transform taking local coordinates to global. `x_axis` and
    // `z_axis` need not be exactly orthonormal: they are orthonormalized
    // (z kept, x projected perpendicular to it) the way STEP's own
    // AXIS2_PLACEMENT_3D is defined to be interpreted.
    static Mat4d Frame(const Vec3d &origin, const Vec3d &z_axis, const Vec3d &x_axis);

    Mat3d LinearPart() const;
    Vec3d TranslationPart() const { return {m[0][3], m[1][3], m[2][3]}; }

    Mat4d operator*(const Mat4d &o) const;
    // Transforms a point (translation applied).
    Vec3d TransformPoint(const Vec3d &p) const;
    // Transforms a direction (translation ignored). NOT the right
    // transform for a normal under non-uniform scaling -- use
    // TransformNormal for that.
    Vec3d TransformVector(const Vec3d &v) const;
    // Transforms a surface normal: the inverse-transpose of the linear
    // part, which is the only thing that keeps a normal perpendicular to
    // its surface when the transform scales non-uniformly. Falls back to
    // TransformVector (and leaves the result unnormalized) if the linear
    // part is singular.
    Vec3d TransformNormal(const Vec3d &n) const;
    // The determinant of the 3x3 linear part. Negative means the
    // transform reverses handedness -- a reflection. Worth having as a
    // named thing rather than open-coded, because a surface parameterized
    // by an angle has to notice: its two parameter directions cross the
    // other way round afterwards, so the angle runs backwards. See
    // CylinderSurface::Transform.
    double LinearDeterminant() const;
    bool Inverse(Mat4d *out, const Tolerance &tol = {}) const;
};

// ---------------------------------------------------------------------
// Small dense linear algebra
// ---------------------------------------------------------------------

// A dynamically sized, row-major dense matrix for the *small* systems the
// kernel solves inline: Levenberg-Marquardt normal equations, least-
// squares curve fitting, element-level FEM matrices. Anything large and
// sparse belongs in num_sparse.h instead -- this type is deliberately
// dense and deliberately has no opinion about sparsity, because the code
// that uses it is solving 3x3s and 12x12s where a sparse representation
// would be pure overhead.
class MatrixNd {
public:
    MatrixNd() = default;
    MatrixNd(std::size_t rows, std::size_t cols) : rows_(rows), cols_(cols), a_(rows * cols, 0.0) {}

    std::size_t Rows() const { return rows_; }
    std::size_t Cols() const { return cols_; }
    double &operator()(std::size_t r, std::size_t c) { return a_[r * cols_ + c]; }
    double operator()(std::size_t r, std::size_t c) const { return a_[r * cols_ + c]; }

    void Fill(double v) { a_.assign(a_.size(), v); }
    void SetIdentity();
    MatrixNd Transposed() const;
    MatrixNd operator*(const MatrixNd &o) const;
    std::vector<double> operator*(const std::vector<double> &v) const;

    // LU with partial pivoting. Returns false on a singular (to `tol`)
    // matrix rather than producing NaNs.
    bool SolveLU(const std::vector<double> &b, std::vector<double> *out, double tol = 1e-14) const;
    // Cholesky, for the symmetric positive-definite case (the LM normal
    // equations, once damped). Roughly twice as fast as LU and, more to
    // the point, its failure *is* the test for positive-definiteness --
    // which is how the LM driver below decides its damping is still too
    // small.
    bool SolveCholesky(const std::vector<double> &b, std::vector<double> *out, double tol = 1e-14) const;
    // Householder QR least-squares: minimizes ||A*x - b|| for an
    // overdetermined system. Preferred over forming the normal equations
    // (AtA) when conditioning matters, because it squares neither the
    // matrix nor its condition number.
    bool SolveLeastSquares(const std::vector<double> &b, std::vector<double> *out, double tol = 1e-14) const;

private:
    std::size_t rows_ = 0, cols_ = 0;
    std::vector<double> a_;
};

// Singular value decomposition, A = U * diag(S) * V^T, by one-sided
// Jacobi. Added for Part D.3: a sketch's constraint Jacobian has to be
// asked two questions that no factorisation-with-pivoting answers
// honestly -- what is its numerical rank, and what directions is the
// sketch still free to move in. The first decides whether constraints are
// redundant, the second *is* the set of drag handles, and both need
// singular values rather than a pivot sequence, because a nearly
// dependent constraint is a small singular value and not a zero pivot.
//
// One-sided Jacobi rather than Golub-Kahan for two reasons. It computes
// the small singular values to high *relative* accuracy, which is exactly
// what distinguishes "redundant" from "very nearly redundant". And it is
// about sixty lines of rotations that can be read and checked, against
// several hundred for bidiagonalisation plus an implicit QR sweep -- for
// matrices of the size a sketch produces, the speed difference is not
// worth the difference in how confident one can be that it is right.
//
// `u` and `v` may be null when only the singular values are wanted.
// Singular values come back in descending order.
bool Svd(const MatrixNd &a, MatrixNd *u, std::vector<double> *s, MatrixNd *v);

// The numerical rank: the number of singular values above
// `relative_tolerance` times the largest. Returns -1 if the
// decomposition fails.
int MatrixRank(const MatrixNd &a, double relative_tolerance = 1e-10);

// An orthonormal basis for the null space of `a` -- the directions x for
// which A*x is zero. Returns the basis size, or -1 on failure.
int NullSpace(const MatrixNd &a, std::vector<std::vector<double>> *basis,
              double relative_tolerance = 1e-10);

// ---------------------------------------------------------------------
// Root finding, minimization, quadrature
// ---------------------------------------------------------------------

// How an iterative solve ended. Callers in the kernel genuinely branch on
// all four -- a point-inversion that hit MaxIterations near a surface pole
// is a different situation from one that diverged, and neither is a bug.
enum class SolveStatus { Converged, MaxIterations, Diverged, Singular };

struct SolveResult {
    SolveStatus status = SolveStatus::MaxIterations;
    int iterations = 0;
    // Final residual norm (2-norm for the vector cases).
    double residual = 0.0;
    bool Ok() const { return status == SolveStatus::Converged; }
};

struct SolveOptions {
    int max_iterations = 100;
    // Convergence on the residual...
    double f_tol = 1e-12;
    // ...and on the step size. Both are checked: a flat function converges
    // in the residual while barely moving, a steep one the other way
    // round, and a point-inversion on a nearly-degenerate surface can
    // satisfy one forever without the other.
    double x_tol = 1e-12;
};

// Scalar Newton with a bisection safety net -- Brent's own hybrid idea,
// reduced to the case where a derivative is available. `f` returns the
// value and writes the derivative. The bracket [a,b] must contain a sign
// change; the Newton step is taken when it stays inside the bracket and
// is reducing the interval fast enough, and a bisection step is taken
// when it does not. That guarantee matters: a bare Newton on a curve
// parameter can and does wander off the parameter range entirely.
SolveResult NewtonBracketed(const std::function<double(double, double *)> &f, double a, double b, double *root,
                            const SolveOptions &opt = {});

// Brent's method without a derivative: inverse quadratic interpolation
// falling back to bisection. For the many places where a derivative is
// available but expensive (every NURBS evaluation computes the point and
// the derivative together, but a *residual's* derivative may need the
// chain rule through a projection).
SolveResult BrentRoot(const std::function<double(double)> &f, double a, double b, double *root,
                      const SolveOptions &opt = {});

// Brent's parabolic-interpolation minimization on a bracketed interval.
// The fallback under point-projection (Part A.5): where the Newton
// iteration on the derivative of the squared distance fails to converge,
// minimizing that squared distance directly always terminates.
SolveResult BrentMinimize(const std::function<double(double)> &f, double a, double b, double *arg_min,
                          double *min_value, const SolveOptions &opt = {});

// Multidimensional Newton for a square system: `residual_and_jacobian`
// fills `r` (n residuals) and `j` (n x n), and the driver solves
// J*dx = -r by LU. Used for curve-curve and curve-surface intersection
// refinement, where the system is genuinely square and small.
SolveResult NewtonSolve(const std::function<void(const std::vector<double> &, std::vector<double> *, MatrixNd *)> &residual_and_jacobian,
                        std::vector<double> *x, const SolveOptions &opt = {});

// Levenberg-Marquardt for an m-residual, n-parameter least-squares
// problem (m >= n or m < n, both allowed -- the damped normal equations
// are solvable either way, which is exactly why LM rather than
// Gauss-Newton is the driver for the sketch constraint solver, where an
// under-constrained sketch is the normal state rather than an error).
//
// The damping schedule is the standard one: start at `lambda0` scaled by
// the diagonal of AtA, decrease it by 10 on an accepted step, increase it
// by 10 on a rejected one. Marquardt's diagonal scaling (rather than
// Levenberg's plain identity) is used so the damping is invariant to the
// units of each parameter -- a sketch mixing millimetres and radians is
// otherwise damped almost entirely in whichever has the larger numbers.
SolveResult LevenbergMarquardt(const std::function<void(const std::vector<double> &, std::vector<double> *, MatrixNd *)> &residual_and_jacobian,
                               std::vector<double> *x, std::size_t residual_count,
                               const SolveOptions &opt = {}, double lambda0 = 1e-3);

// Gauss-Legendre nodes and weights on [-1, 1], computed on demand by
// Newton iteration on the Legendre polynomial (and cached). Computed
// rather than tabulated because the table for orders up to 32 is some
// hundreds of lines of constants that cannot be checked by reading them,
// whereas the recurrence can -- and the test verifies the result the only
// way that means anything: an order-n rule must integrate every
// polynomial up to degree 2n-1 exactly.
void GaussLegendreRule(int n, std::vector<double> *nodes, std::vector<double> *weights);

// Integrates `f` over [a, b] with a fixed n-point Gauss-Legendre rule.
// The workhorse for mass properties (Part A.6) and every FEM element
// integral, where the integrand's polynomial degree is known in advance
// and adaptivity would be wasted work.
double GaussLegendre(const std::function<double(double)> &f, double a, double b, int n);

// Adaptive Gauss-Kronrod (G7,K15) with interval bisection. For the
// integrands whose degree is *not* known in advance -- arc length along a
// NURBS curve, area of a trimmed face -- where a fixed rule either
// over-integrates everywhere or silently under-integrates near a kink.
// The Kronrod extension reuses all 7 Gauss nodes, so the error estimate
// costs 8 extra evaluations rather than a second full rule.
// `out_error`, when non-null, receives the final estimated absolute error.
double AdaptiveQuadrature(const std::function<double(double)> &f, double a, double b, double abs_tol = 1e-10,
                          int max_depth = 24, double *out_error = nullptr);

}  // namespace cad

#endif
