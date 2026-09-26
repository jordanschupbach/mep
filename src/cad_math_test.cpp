// Windowless coverage for cad_math.h (plans/CAD_FEM_PLAN.md Part 0.1).
// Pure CPU arithmetic with no backend of any kind, so unlike
// mep-model3d-doc-test this binary links nothing but its own translation
// unit -- no gfx, no X11, no GL.
//
// The interesting tests here are the ones that check a *property* rather
// than a recorded value: a Gauss-Legendre rule of order n integrating
// every polynomial up to degree 2n-1 exactly, a rotation matrix staying
// orthonormal, a transformed normal staying perpendicular to its
// transformed surface under non-uniform scaling. Those catch the whole
// class of algorithm mistakes that a hand-computed expected value cannot,
// because a wrong implementation and a wrong expectation are usually
// wrong the same way.

#include "cad_math.h"

#include <cstdio>
#include <random>
#include <cstdlib>
#include <vector>

namespace {

int g_checks = 0;

void Check(bool condition, const char *expression, int line) {
    ++g_checks;
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

bool Near(double a, double b, double tol = 1e-10) { return std::fabs(a - b) <= tol; }
bool NearVec(const cad::Vec3d &a, const cad::Vec3d &b, double tol = 1e-10) {
    return (a - b).Length() <= tol;
}

void TestVectors() {
    const cad::Vec3d a(1.0, 2.0, 3.0);
    const cad::Vec3d b(4.0, 5.0, 6.0);
    CHECK(Near(a.Dot(b), 32.0));
    CHECK(NearVec(a.Cross(b), cad::Vec3d(-3.0, 6.0, -3.0)));
    CHECK(Near(a.Length(), std::sqrt(14.0)));
    CHECK(Near(a.Normalized().Length(), 1.0));
    // Cross product is antisymmetric, and both operands are perpendicular
    // to the result -- the invariants that catch a transposed term.
    CHECK(NearVec(a.Cross(b), -b.Cross(a)));
    CHECK(Near(a.Dot(a.Cross(b)), 0.0));
    CHECK(Near(b.Dot(a.Cross(b)), 0.0));

    // Degenerate normalization returns zero rather than NaN: callers in
    // the kernel test the result's length instead of guarding every call.
    const cad::Vec3d zero;
    CHECK(Near(zero.Normalized().Length(), 0.0));

    // AnyPerpendicular over every axis, including the cases the naive
    // "always cross with Z" implementation gets wrong.
    const cad::Vec3d dirs[5] = {cad::Vec3d(1.0, 0.0, 0.0), cad::Vec3d(0.0, 1.0, 0.0), cad::Vec3d(0.0, 0.0, 1.0),
                                cad::Vec3d(0.0, 0.0, -1.0), cad::Vec3d(1.0, 1.0, 1.0)};
    for (const cad::Vec3d &d : dirs) {
        const cad::Vec3d u = d.Normalized();
        const cad::Vec3d p = u.AnyPerpendicular();
        CHECK(Near(p.Length(), 1.0, 1e-12));
        CHECK(Near(p.Dot(u), 0.0, 1e-12));
    }

    const cad::Vec2d p2(3.0, 4.0);
    CHECK(Near(p2.Length(), 5.0));
    CHECK(Near(p2.Cross(p2.Perp()), 25.0));
    CHECK(Near(p2.Dot(p2.Perp()), 0.0));
    CHECK(Near(cad::ScalarTriple(cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), cad::Vec3d(0, 0, 1)), 1.0));
}

void TestTolerance() {
    cad::Tolerance tol;
    CHECK(tol.SamePoint(cad::Vec3d(0.0, 0.0, 0.0), cad::Vec3d(1e-9, 0.0, 0.0)));
    CHECK(!tol.SamePoint(cad::Vec3d(0.0, 0.0, 0.0), cad::Vec3d(1e-5, 0.0, 0.0)));
    CHECK(tol.SameDirection(cad::Vec3d(1, 0, 0), cad::Vec3d(1, 0, 0)));
    // Antiparallel is parallel but not the same direction -- the
    // distinction every face-orientation test depends on.
    CHECK(!tol.SameDirection(cad::Vec3d(1, 0, 0), cad::Vec3d(-1, 0, 0)));
    CHECK(tol.ParallelDirection(cad::Vec3d(1, 0, 0), cad::Vec3d(-1, 0, 0)));
    // A per-entity tolerance really is per-entity: a looser one accepts
    // what the default rejects. This is the whole point of the struct.
    cad::Tolerance loose;
    loose.linear = 1e-3;
    CHECK(loose.SamePoint(cad::Vec3d(0.0, 0.0, 0.0), cad::Vec3d(1e-5, 0.0, 0.0)));
    CHECK(tol.SameRelative(1000.0, 1000.0 + 1e-8, 1000.0));
}

void TestInterval() {
    const cad::Interval a(1.0, 3.0);
    const cad::Interval b(-2.0, 4.0);
    CHECK(a.Overlaps(b));
    CHECK(!a.Overlaps(cad::Interval(5.0, 6.0)));
    CHECK(a.Padded(3.0).Overlaps(cad::Interval(5.0, 6.0)));
    CHECK(Near(a.Mid(), 2.0));
    CHECK(Near(a.Width(), 2.0));
    const cad::Interval sum = a + b;
    CHECK(Near(sum.lo, -1.0) && Near(sum.hi, 7.0));
    const cad::Interval diff = a - b;
    CHECK(Near(diff.lo, -3.0) && Near(diff.hi, 5.0));
    // Multiplication across zero is where a sign-case implementation goes
    // wrong: [1,3]*[-2,4] must be [-6,12], not [-2,12].
    const cad::Interval prod = a * b;
    CHECK(Near(prod.lo, -6.0) && Near(prod.hi, 12.0));
    const cad::Interval both_negative = cad::Interval(-3.0, -1.0) * cad::Interval(-4.0, -2.0);
    CHECK(Near(both_negative.lo, 2.0) && Near(both_negative.hi, 12.0));

    cad::Interval empty = cad::Interval::Empty();
    CHECK(empty.IsEmpty());
    empty.Expand(5.0);
    CHECK(!empty.IsEmpty() && Near(empty.lo, 5.0) && Near(empty.hi, 5.0));

    cad::Box3d box;
    CHECK(box.IsEmpty());
    box.Expand(cad::Vec3d(0.0, 0.0, 0.0));
    box.Expand(cad::Vec3d(1.0, 2.0, 2.0));
    CHECK(!box.IsEmpty());
    CHECK(Near(box.Diagonal(), 3.0));
    CHECK(NearVec(box.Center(), cad::Vec3d(0.5, 1.0, 1.0)));
    cad::Box3d other;
    other.Expand(cad::Vec3d(2.0, 2.0, 2.0));
    other.Expand(cad::Vec3d(3.0, 3.0, 3.0));
    CHECK(!box.Overlaps(other));
    CHECK(box.Overlaps(other, 1.5));
}

void TestMat3d() {
    // A rotation must be orthonormal with determinant +1, and rotating by
    // an angle then by its negative must return the identity.
    const cad::Vec3d axis = cad::Vec3d(1.0, 2.0, 3.0).Normalized();
    const cad::Mat3d r = cad::Mat3d::Rotation(axis, 0.7);
    CHECK(Near(r.Determinant(), 1.0, 1e-12));
    const cad::Mat3d should_be_identity = r * r.Transposed();
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) CHECK(Near(should_be_identity.m[i][j], i == j ? 1.0 : 0.0, 1e-12));
    // The rotation axis itself is fixed by the rotation.
    CHECK(NearVec(r * axis, axis, 1e-12));
    const cad::Mat3d back = cad::Mat3d::Rotation(axis, -0.7);
    CHECK(NearVec(back * (r * cad::Vec3d(1.0, 0.0, 0.0)), cad::Vec3d(1.0, 0.0, 0.0), 1e-12));

    // A known 90-degree rotation about Z, checked against the answer
    // anyone can verify by hand.
    const cad::Mat3d rz = cad::Mat3d::Rotation(cad::Vec3d(0, 0, 1), cad::kHalfPi);
    CHECK(NearVec(rz * cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 1e-12));

    cad::Mat3d general = cad::Mat3d::FromRows(cad::Vec3d(2, 1, 1), cad::Vec3d(1, 3, 2), cad::Vec3d(1, 0, 0));
    cad::Mat3d inv;
    CHECK(general.Inverse(&inv));
    const cad::Mat3d product = general * inv;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) CHECK(Near(product.m[i][j], i == j ? 1.0 : 0.0, 1e-12));

    cad::Vec3d solved;
    CHECK(general.Solve(cad::Vec3d(4, 5, 1), &solved));
    CHECK(NearVec(general * solved, cad::Vec3d(4, 5, 1), 1e-12));

    // Singular matrices report failure instead of returning infinities.
    const cad::Mat3d singular = cad::Mat3d::FromRows(cad::Vec3d(1, 2, 3), cad::Vec3d(2, 4, 6), cad::Vec3d(1, 1, 1));
    cad::Mat3d unused;
    CHECK(!singular.Inverse(&unused));

    CHECK(NearVec(cad::Mat3d::FromColumns(cad::Vec3d(1, 2, 3), cad::Vec3d(4, 5, 6), cad::Vec3d(7, 8, 9)).Column(1),
                  cad::Vec3d(4, 5, 6)));
}

void TestMat4d() {
    const cad::Mat4d t = cad::Mat4d::Translation(cad::Vec3d(1, 2, 3));
    CHECK(NearVec(t.TransformPoint(cad::Vec3d(0, 0, 0)), cad::Vec3d(1, 2, 3)));
    // A direction is unaffected by translation; a point is not. Getting
    // these two swapped is the single most common transform bug.
    CHECK(NearVec(t.TransformVector(cad::Vec3d(1, 0, 0)), cad::Vec3d(1, 0, 0)));

    const cad::Mat4d composed = t * cad::Mat4d::Rotation(cad::Vec3d(0, 0, 1), cad::kHalfPi);
    CHECK(NearVec(composed.TransformPoint(cad::Vec3d(1, 0, 0)), cad::Vec3d(1, 3, 3), 1e-12));
    cad::Mat4d inv;
    CHECK(composed.Inverse(&inv));
    CHECK(NearVec(inv.TransformPoint(composed.TransformPoint(cad::Vec3d(5, 6, 7))), cad::Vec3d(5, 6, 7), 1e-12));

    // Frame() orthonormalizes: given an x hint that is not perpendicular
    // to z (exactly what real STEP files carry), the result must still be
    // an orthonormal frame whose z is the one that was asked for.
    const cad::Mat4d frame =
        cad::Mat4d::Frame(cad::Vec3d(1, 1, 1), cad::Vec3d(0, 0, 2), cad::Vec3d(1.0, 0.0, 0.5));
    const cad::Mat3d lin = frame.LinearPart();
    for (std::size_t i = 0; i < 3; ++i) CHECK(Near(lin.Column(i).Length(), 1.0, 1e-12));
    CHECK(Near(lin.Column(0).Dot(lin.Column(1)), 0.0, 1e-12));
    CHECK(Near(lin.Column(1).Dot(lin.Column(2)), 0.0, 1e-12));
    CHECK(Near(lin.Column(0).Dot(lin.Column(2)), 0.0, 1e-12));
    CHECK(NearVec(lin.Column(2), cad::Vec3d(0, 0, 1), 1e-12));
    CHECK(Near(lin.Determinant(), 1.0, 1e-12));
    // A degenerate x hint (parallel to z) must not produce a garbage
    // frame -- it falls back to an arbitrary perpendicular.
    const cad::Mat4d degenerate = cad::Mat4d::Frame(cad::Vec3d(), cad::Vec3d(0, 0, 1), cad::Vec3d(0, 0, 5));
    CHECK(Near(degenerate.LinearPart().Determinant(), 1.0, 1e-12));

    // The normal transform under non-uniform scaling: a plane's normal
    // must stay perpendicular to two independent in-plane directions
    // after the transform. TransformVector would fail this, which is the
    // entire reason TransformNormal exists.
    const cad::Mat4d squash = cad::Mat4d::Scaling(cad::Vec3d(1.0, 4.0, 1.0));
    const cad::Vec3d normal = cad::Vec3d(1.0, 1.0, 0.0).Normalized();
    const cad::Vec3d in_plane_a = cad::Vec3d(1.0, -1.0, 0.0).Normalized();
    const cad::Vec3d in_plane_b(0.0, 0.0, 1.0);
    const cad::Vec3d moved_normal = squash.TransformNormal(normal);
    CHECK(Near(moved_normal.Normalized().Dot(squash.TransformVector(in_plane_a).Normalized()), 0.0, 1e-12));
    CHECK(Near(moved_normal.Normalized().Dot(squash.TransformVector(in_plane_b).Normalized()), 0.0, 1e-12));
    CHECK(std::fabs(normal.Dot(squash.TransformVector(in_plane_a).Normalized())) > 0.1);
}

void TestMatrixNd() {
    cad::MatrixNd a(3, 3);
    a(0, 0) = 2.0; a(0, 1) = 1.0; a(0, 2) = 1.0;
    a(1, 0) = 1.0; a(1, 1) = 3.0; a(1, 2) = 2.0;
    a(2, 0) = 1.0; a(2, 1) = 0.0; a(2, 2) = 0.0;
    std::vector<double> rhs;
    rhs.push_back(4.0);
    rhs.push_back(5.0);
    rhs.push_back(1.0);
    std::vector<double> x;
    CHECK(a.SolveLU(rhs, &x));
    const std::vector<double> check = a * x;
    for (std::size_t i = 0; i < 3; ++i) CHECK(Near(check[i], rhs[i], 1e-12));

    // Cholesky on a symmetric positive-definite matrix, and its refusal
    // on one that is not -- the refusal is load-bearing, since it is how
    // LevenbergMarquardt detects that its damping is too small.
    cad::MatrixNd spd(3, 3);
    spd(0, 0) = 4.0; spd(0, 1) = 1.0; spd(0, 2) = 1.0;
    spd(1, 0) = 1.0; spd(1, 1) = 3.0; spd(1, 2) = 0.0;
    spd(2, 0) = 1.0; spd(2, 1) = 0.0; spd(2, 2) = 2.0;
    std::vector<double> xs;
    CHECK(spd.SolveCholesky(rhs, &xs));
    const std::vector<double> check_spd = spd * xs;
    for (std::size_t i = 0; i < 3; ++i) CHECK(Near(check_spd[i], rhs[i], 1e-12));
    cad::MatrixNd indefinite(2, 2);
    indefinite(0, 0) = 1.0; indefinite(0, 1) = 2.0;
    indefinite(1, 0) = 2.0; indefinite(1, 1) = 1.0;
    std::vector<double> small_rhs;
    small_rhs.push_back(1.0);
    small_rhs.push_back(1.0);
    std::vector<double> unused;
    CHECK(!indefinite.SolveCholesky(small_rhs, &unused));

    // Least squares: fit y = m*x + c to four exactly-collinear points, so
    // the answer is known and the residual should be zero.
    cad::MatrixNd design(4, 2);
    std::vector<double> ys;
    for (std::size_t i = 0; i < 4; ++i) {
        const double xi = static_cast<double>(i);
        design(i, 0) = xi;
        design(i, 1) = 1.0;
        ys.push_back(3.0 * xi + 7.0);
    }
    std::vector<double> fit;
    CHECK(design.SolveLeastSquares(ys, &fit));
    CHECK(Near(fit[0], 3.0, 1e-10));
    CHECK(Near(fit[1], 7.0, 1e-10));

    // Over-determined and genuinely inconsistent: the least-squares
    // answer must be the normal-equation solution, not a failure.
    std::vector<double> noisy = ys;
    noisy[0] += 1.0;
    noisy[3] -= 1.0;
    std::vector<double> noisy_fit;
    CHECK(design.SolveLeastSquares(noisy, &noisy_fit));
    // Residual must be orthogonal to both columns of the design matrix --
    // the defining property of a least-squares solution.
    const std::vector<double> predicted = design * noisy_fit;
    double dot_col0 = 0.0;
    double dot_col1 = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
        const double r = noisy[i] - predicted[i];
        dot_col0 += r * design(i, 0);
        dot_col1 += r * design(i, 1);
    }
    CHECK(Near(dot_col0, 0.0, 1e-10));
    CHECK(Near(dot_col1, 0.0, 1e-10));
}

void TestRootFinding() {
    // cos(x) - x has its single root near 0.739085133215.
    auto f = [](double x) { return std::cos(x) - x; };
    auto f_and_d = [](double x, double *d) {
        *d = -std::sin(x) - 1.0;
        return std::cos(x) - x;
    };
    double root = 0.0;
    cad::SolveResult r = cad::NewtonBracketed(f_and_d, 0.0, 1.0, &root);
    CHECK(r.Ok());
    CHECK(Near(root, 0.7390851332151607, 1e-12));

    double broot = 0.0;
    r = cad::BrentRoot(f, 0.0, 1.0, &broot);
    CHECK(r.Ok());
    CHECK(Near(broot, 0.7390851332151607, 1e-10));

    // No sign change in the bracket is reported, not papered over with an
    // endpoint -- the caller (a curve-parameter search) needs to know.
    double bad = 0.0;
    r = cad::BrentRoot(f, 2.0, 3.0, &bad);
    CHECK(r.status == cad::SolveStatus::Diverged);
    r = cad::NewtonBracketed(f_and_d, 2.0, 3.0, &bad);
    CHECK(r.status == cad::SolveStatus::Diverged);

    // An exact root sitting on a bracket endpoint.
    auto linear = [](double x) { return x - 2.0; };
    double exact = 0.0;
    r = cad::BrentRoot(linear, 2.0, 5.0, &exact);
    CHECK(r.Ok() && Near(exact, 2.0));

    // A deliberately nasty one for the bisection safety net: x^3 is flat
    // at its root, so the Newton step alone converges only linearly (by a
    // factor of 2/3 per step) and the "not halving fast enough" guard has
    // to take over with bisection or the iteration crawls.
    //
    // Note what a flat root does to the two convergence criteria, because
    // it is not a defect and callers doing point-inversion on a
    // tangentially-touching surface will meet it: stopping at
    // |f| <= 1e-12 means stopping at |x| <= 1e-4, since f is the *cube* of
    // the distance from the root. A residual tolerance cannot locate a
    // triple root more precisely than its own cube root. Only the step
    // criterion can, which is exactly why SolveOptions carries both.
    auto cubic = [](double x, double *d) {
        *d = 3.0 * x * x;
        return x * x * x;
    };
    double cube_root = 0.0;
    r = cad::NewtonBracketed(cubic, -1.0, 2.0, &cube_root);
    CHECK(r.Ok());
    CHECK(r.residual <= 1e-12);          // the contract it was asked for
    CHECK(std::fabs(cube_root) < 1e-3);  // all that residual can imply here

    // Forcing convergence through the step criterion instead pins the
    // root to full precision -- and takes the bisection path the whole
    // way, so this is the real test of the safety net.
    cad::SolveOptions step_only;
    step_only.f_tol = 0.0;
    step_only.x_tol = 1e-12;
    r = cad::NewtonBracketed(cubic, -1.0, 2.0, &cube_root, step_only);
    CHECK(r.Ok());
    CHECK(Near(cube_root, 0.0, 1e-9));
}

void TestMinimize() {
    // A parabola with its minimum at 1.5, then a case the parabolic fit
    // cannot shortcut.
    auto quad = [](double x) { return (x - 1.5) * (x - 1.5) + 2.0; };
    double arg = 0.0;
    double val = 0.0;
    cad::SolveResult r = cad::BrentMinimize(quad, -5.0, 10.0, &arg, &val);
    CHECK(r.Ok());
    CHECK(Near(arg, 1.5, 1e-7));
    CHECK(Near(val, 2.0, 1e-12));

    auto quartic = [](double x) { return std::pow(x - 0.3, 4.0) + 1.0; };
    r = cad::BrentMinimize(quartic, -2.0, 2.0, &arg, &val);
    CHECK(r.Ok());
    CHECK(Near(arg, 0.3, 1e-3));

    // The shape point-projection actually hits: squared distance from a
    // point to a unit circle, parameterized by angle.
    const cad::Vec2d target(2.0, 0.5);
    auto dist_sq = [&target](double t) {
        const cad::Vec2d on_circle(std::cos(t), std::sin(t));
        return (on_circle - target).LengthSquared();
    };
    r = cad::BrentMinimize(dist_sq, -1.0, 1.0, &arg, &val);
    CHECK(r.Ok());
    CHECK(Near(arg, std::atan2(0.5, 2.0), 1e-6));
}

void TestNewtonSolve() {
    // x^2 + y^2 = 4, x*y = 1 -- two residuals, two unknowns, solved from
    // a start near the (1.93, 0.518) branch.
    auto system = [](const std::vector<double> &v, std::vector<double> *r, cad::MatrixNd *j) {
        const double x = v[0];
        const double y = v[1];
        (*r)[0] = x * x + y * y - 4.0;
        (*r)[1] = x * y - 1.0;
        (*j)(0, 0) = 2.0 * x;
        (*j)(0, 1) = 2.0 * y;
        (*j)(1, 0) = y;
        (*j)(1, 1) = x;
    };
    std::vector<double> v;
    v.push_back(2.0);
    v.push_back(0.4);
    const cad::SolveResult r = cad::NewtonSolve(system, &v);
    CHECK(r.Ok());
    CHECK(Near(v[0] * v[0] + v[1] * v[1], 4.0, 1e-10));
    CHECK(Near(v[0] * v[1], 1.0, 1e-10));

    // A singular Jacobian is reported as such rather than producing NaNs.
    auto singular = [](const std::vector<double> &v2, std::vector<double> *r2, cad::MatrixNd *j2) {
        (*r2)[0] = v2[0] + v2[1] - 1.0;
        (*r2)[1] = 2.0 * v2[0] + 2.0 * v2[1] - 5.0;
        (*j2)(0, 0) = 1.0;
        (*j2)(0, 1) = 1.0;
        (*j2)(1, 0) = 2.0;
        (*j2)(1, 1) = 2.0;
    };
    std::vector<double> w;
    w.push_back(0.0);
    w.push_back(0.0);
    CHECK(cad::NewtonSolve(singular, &w).status == cad::SolveStatus::Singular);
}

void TestLevenbergMarquardt() {
    // Rosenbrock in least-squares form: the standard torture test for an
    // LM driver, whose narrow curved valley defeats plain Gauss-Newton
    // from this starting point.
    auto rosenbrock = [](const std::vector<double> &v, std::vector<double> *r, cad::MatrixNd *j) {
        const double x = v[0];
        const double y = v[1];
        (*r)[0] = 10.0 * (y - x * x);
        (*r)[1] = 1.0 - x;
        (*j)(0, 0) = -20.0 * x;
        (*j)(0, 1) = 10.0;
        (*j)(1, 0) = -1.0;
        (*j)(1, 1) = 0.0;
    };
    std::vector<double> v;
    v.push_back(-1.2);
    v.push_back(1.0);
    cad::SolveOptions opt;
    opt.max_iterations = 200;
    const cad::SolveResult r = cad::LevenbergMarquardt(rosenbrock, &v, 2, opt);
    CHECK(r.Ok());
    CHECK(Near(v[0], 1.0, 1e-5));
    CHECK(Near(v[1], 1.0, 1e-5));

    // Under-determined: one residual, two parameters. This is the normal
    // state of a sketch that is not yet fully constrained, so it must
    // converge to *some* point on the solution line rather than fail --
    // which is exactly what Marquardt's damped diagonal buys.
    auto under = [](const std::vector<double> &v2, std::vector<double> *r2, cad::MatrixNd *j2) {
        (*r2)[0] = v2[0] + v2[1] - 3.0;
        (*j2)(0, 0) = 1.0;
        (*j2)(0, 1) = 1.0;
    };
    std::vector<double> w;
    w.push_back(0.0);
    w.push_back(0.0);
    const cad::SolveResult ru = cad::LevenbergMarquardt(under, &w, 1);
    CHECK(ru.Ok());
    CHECK(Near(w[0] + w[1], 3.0, 1e-8));

    // Over-determined and inconsistent: three residuals demanding
    // contradictory things of one parameter. The right outcome is the
    // least-squares compromise (the mean, here 2.0) reported with a
    // non-zero residual, not a claim of success.
    auto over = [](const std::vector<double> &v3, std::vector<double> *r3, cad::MatrixNd *j3) {
        (*r3)[0] = v3[0] - 1.0;
        (*r3)[1] = v3[0] - 2.0;
        (*r3)[2] = v3[0] - 3.0;
        (*j3)(0, 0) = 1.0;
        (*j3)(1, 0) = 1.0;
        (*j3)(2, 0) = 1.0;
    };
    std::vector<double> u;
    u.push_back(0.0);
    const cad::SolveResult ro = cad::LevenbergMarquardt(over, &u, 3, opt);
    CHECK(Near(u[0], 2.0, 1e-6));
    CHECK(ro.residual > 1.0);
    CHECK(!ro.Ok());

    // A parameter no residual depends on: the zero-diagonal fallback in
    // the damping must keep the system solvable rather than singular.
    auto ignores_second = [](const std::vector<double> &v4, std::vector<double> *r4, cad::MatrixNd *j4) {
        (*r4)[0] = v4[0] - 5.0;
        (*j4)(0, 0) = 1.0;
        (*j4)(0, 1) = 0.0;
    };
    std::vector<double> z;
    z.push_back(0.0);
    z.push_back(9.0);
    const cad::SolveResult rz = cad::LevenbergMarquardt(ignores_second, &z, 1);
    CHECK(rz.Ok());
    CHECK(Near(z[0], 5.0, 1e-8));
}

void TestQuadrature() {
    // The property that actually defines a Gauss-Legendre rule: order n
    // integrates every polynomial of degree <= 2n-1 exactly. Checking
    // this across orders catches a wrong node, a wrong weight and a
    // broken symmetry all at once, which no single recorded value would.
    for (int n = 1; n <= 16; ++n) {
        std::vector<double> nodes;
        std::vector<double> weights;
        cad::GaussLegendreRule(n, &nodes, &weights);
        CHECK(nodes.size() == static_cast<std::size_t>(n));
        CHECK(weights.size() == static_cast<std::size_t>(n));
        // Weights sum to the length of [-1,1].
        double weight_sum = 0.0;
        for (double w : weights) {
            CHECK(w > 0.0);
            weight_sum += w;
        }
        CHECK(Near(weight_sum, 2.0, 1e-12));
        // Nodes are symmetric about zero and strictly increasing.
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            CHECK(Near(nodes[i], -nodes[nodes.size() - 1 - i], 1e-14));
            if (i > 0) CHECK(nodes[i] > nodes[i - 1]);
            CHECK(nodes[i] > -1.0 && nodes[i] < 1.0);
        }
        for (int degree = 0; degree <= 2 * n - 1; ++degree) {
            const double expected = (degree % 2 == 1) ? 0.0 : 2.0 / (static_cast<double>(degree) + 1.0);
            const int captured_degree = degree;
            const double got = cad::GaussLegendre(
                [captured_degree](double x) { return std::pow(x, static_cast<double>(captured_degree)); }, -1.0, 1.0, n);
            CHECK(Near(got, expected, 1e-11));
        }
    }

    // Integration over a non-symmetric interval, so the [a,b] mapping is
    // exercised rather than just the reference rule.
    const double sin_integral = cad::GaussLegendre([](double x) { return std::sin(x); }, 0.0, cad::kPi, 12);
    CHECK(Near(sin_integral, 2.0, 1e-12));

    // Adaptive quadrature on a smooth integrand...
    double err = 0.0;
    double got = cad::AdaptiveQuadrature([](double x) { return std::exp(-x * x); }, -3.0, 3.0, 1e-12, 30, &err);
    // sqrt(pi)*erf(3), to full double precision.
    CHECK(Near(got, 1.7724146965190422, 1e-10));
    CHECK(err < 1e-9);

    // ...and on one with a kink at the origin, where a fixed rule of any
    // order is badly wrong and adaptivity is the whole point. |x| over
    // [-1,2] is 0.5 + 2.0.
    got = cad::AdaptiveQuadrature([](double x) { return std::fabs(x); }, -1.0, 2.0, 1e-10, 40, &err);
    CHECK(Near(got, 2.5, 1e-8));

    // A peak narrow enough that a 15-point panel over the whole range
    // would miss it entirely: 1/(1 + 10000*x^2) over [-1,1], whose exact
    // value is atan(100)/50.
    got = cad::AdaptiveQuadrature([](double x) { return 1.0 / (1.0 + 10000.0 * x * x); }, -1.0, 1.0, 1e-12, 40, &err);
    CHECK(Near(got, std::atan(100.0) / 50.0, 1e-9));
}

// --- SVD, rank and null space -----------------------------------------
//
// The decomposition is checked by its defining properties rather than
// against recorded numbers: U and V orthonormal, A reconstructed from
// U*S*V^T, and the singular values matching the square roots of the
// eigenvalues of A^T*A, which for the small cases here are known in
// closed form. Rank and null space are then checked against constructions
// whose answers are known by design -- a matrix built as an outer product
// has rank one, a matrix with a repeated row has a null direction that
// can be written down.
void TestSvd() {
    auto reconstruct = [](const cad::MatrixNd &u, const std::vector<double> &s, const cad::MatrixNd &v,
                          std::size_t m, std::size_t n) {
        cad::MatrixNd out(m, n);
        for (std::size_t i = 0; i < m; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                double sum = 0.0;
                for (std::size_t k = 0; k < s.size(); ++k) sum += u(i, k) * s[k] * v(j, k);
                out(i, j) = sum;
            }
        }
        return out;
    };
    auto worst_difference = [](const cad::MatrixNd &x, const cad::MatrixNd &y) {
        double worst = 0.0;
        for (std::size_t i = 0; i < x.Rows(); ++i) {
            for (std::size_t j = 0; j < x.Cols(); ++j) worst = std::max(worst, std::fabs(x(i, j) - y(i, j)));
        }
        return worst;
    };
    auto orthonormality_error = [](const cad::MatrixNd &q, std::size_t columns) {
        double worst = 0.0;
        for (std::size_t a = 0; a < columns; ++a) {
            for (std::size_t b = 0; b < columns; ++b) {
                double dot = 0.0;
                for (std::size_t i = 0; i < q.Rows(); ++i) dot += q(i, a) * q(i, b);
                worst = std::max(worst, std::fabs(dot - (a == b ? 1.0 : 0.0)));
            }
        }
        return worst;
    };

    // A diagonal matrix: the singular values are the absolute diagonal
    // entries, in descending order, and nothing else can be true.
    {
        cad::MatrixNd a(3, 3);
        a(0, 0) = 2.0;
        a(1, 1) = -5.0;
        a(2, 2) = 0.5;
        std::vector<double> s;
        CHECK(cad::Svd(a, nullptr, &s, nullptr));
        CHECK(s.size() == 3);
        CHECK(Near(s[0], 5.0, 1e-12));
        CHECK(Near(s[1], 2.0, 1e-12));
        CHECK(Near(s[2], 0.5, 1e-12));
    }

    // Random matrices, tall, square and wide, checked by reconstruction
    // and orthonormality. The wide case is the one a sketch produces and
    // the one that goes through the transpose path.
    {
        std::mt19937 rng(20260924);
        std::uniform_real_distribution<double> value(-3.0, 3.0);
        double worst_reconstruction = 0.0;
        double worst_orthonormal = 0.0;
        for (int trial = 0; trial < 60; ++trial) {
            const std::size_t m = 1 + static_cast<std::size_t>(rng() % 7);
            const std::size_t n = 1 + static_cast<std::size_t>(rng() % 7);
            cad::MatrixNd a(m, n);
            for (std::size_t i = 0; i < m; ++i) {
                for (std::size_t j = 0; j < n; ++j) a(i, j) = value(rng);
            }
            cad::MatrixNd u;
            cad::MatrixNd v;
            std::vector<double> s;
            CHECK(cad::Svd(a, &u, &s, &v));
            CHECK(s.size() == std::min(m, n));
            for (std::size_t k = 0; k + 1 < s.size(); ++k) CHECK(s[k] >= s[k + 1] - 1e-12);
            worst_reconstruction = std::max(worst_reconstruction, worst_difference(a, reconstruct(u, s, v, m, n)));
            // Only the columns with a non-zero singular value are
            // determined; the rest are deliberately left at zero.
            std::size_t determined = 0;
            while (determined < s.size() && s[determined] > s.front() * 1e-12) ++determined;
            worst_orthonormal = std::max(worst_orthonormal, orthonormality_error(u, determined));
            worst_orthonormal = std::max(worst_orthonormal, orthonormality_error(v, determined));
        }
        std::printf("  svd: 60 random shapes, reconstruction %.3e, orthonormality %.3e\n",
                    worst_reconstruction, worst_orthonormal);
        CHECK(worst_reconstruction < 1e-12);
        CHECK(worst_orthonormal < 1e-12);
    }

    // Rank, on matrices whose rank is known by construction.
    {
        // An outer product has rank one however large it is.
        cad::MatrixNd outer(5, 4);
        const double left[5] = {1.0, 2.0, -1.0, 0.5, 3.0};
        const double right[4] = {2.0, -1.0, 4.0, 0.25};
        for (std::size_t i = 0; i < 5; ++i) {
            for (std::size_t j = 0; j < 4; ++j) outer(i, j) = left[i] * right[j];
        }
        CHECK(cad::MatrixRank(outer) == 1);

        cad::MatrixNd identity(4, 4);
        identity.SetIdentity();
        CHECK(cad::MatrixRank(identity) == 4);

        // A duplicated row adds no rank.
        cad::MatrixNd repeated(3, 3);
        repeated(0, 0) = 1.0;
        repeated(0, 1) = 2.0;
        repeated(1, 1) = 1.0;
        repeated(2, 0) = 1.0;
        repeated(2, 1) = 2.0;
        CHECK(cad::MatrixRank(repeated) == 2);

        cad::MatrixNd zero(3, 3);
        CHECK(cad::MatrixRank(zero) == 0);
    }

    // The null space, checked the only way that means anything: every
    // basis vector must be annihilated by the matrix, the basis must be
    // orthonormal, and its size must be the rank-nullity count.
    {
        std::mt19937 rng(7771);
        std::uniform_real_distribution<double> value(-2.0, 2.0);
        double worst_annihilation = 0.0;
        for (int trial = 0; trial < 40; ++trial) {
            const std::size_t m = 1 + static_cast<std::size_t>(rng() % 5);
            const std::size_t n = 1 + static_cast<std::size_t>(rng() % 6);
            cad::MatrixNd a(m, n);
            for (std::size_t i = 0; i < m; ++i) {
                for (std::size_t j = 0; j < n; ++j) a(i, j) = value(rng);
            }
            // Force a dependency half the time, so the null space is not
            // always the trivial "wide matrix" one.
            if (m >= 2 && (trial % 2) == 0) {
                for (std::size_t j = 0; j < n; ++j) a(m - 1, j) = 2.0 * a(0, j);
            }
            std::vector<std::vector<double>> basis;
            const int count = cad::NullSpace(a, &basis);
            CHECK(count >= 0);
            CHECK(static_cast<int>(n) - cad::MatrixRank(a) == count);
            for (const std::vector<double> &direction : basis) {
                const std::vector<double> image = a * direction;
                for (double component : image) worst_annihilation = std::max(worst_annihilation, std::fabs(component));
                double norm = 0.0;
                for (double x : direction) norm += x * x;
                CHECK(Near(norm, 1.0, 1e-10));
            }
            for (std::size_t i = 0; i < basis.size(); ++i) {
                for (std::size_t j = i + 1; j < basis.size(); ++j) {
                    double dot = 0.0;
                    for (std::size_t k = 0; k < n; ++k) dot += basis[i][k] * basis[j][k];
                    CHECK(std::fabs(dot) < 1e-10);
                }
            }
        }
        std::printf("  null space: 40 random matrices, worst |A*x| over the basis %.3e\n", worst_annihilation);
        CHECK(worst_annihilation < 1e-11);
    }
}

}  // namespace

int main() {
    TestVectors();
    TestTolerance();
    TestInterval();
    TestMat3d();
    TestMat4d();
    TestMatrixNd();
    TestRootFinding();
    TestMinimize();
    TestNewtonSolve();
    TestLevenbergMarquardt();
    TestQuadrature();
    TestSvd();
    std::printf("cad_math_test passed (%d checks)\n", g_checks);
    return 0;
}
