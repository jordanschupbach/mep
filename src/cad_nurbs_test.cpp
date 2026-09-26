// Windowless coverage for cad_nurbs.h (plans/CAD_FEM_PLAN.md Parts A.2
// and A.3).
//
// NURBS code is unusually easy to get *almost* right: a transposed index
// or a dropped term typically yields a curve that still looks like a
// curve, passes through roughly the right region, and is wrong in a way
// no amount of staring at it reveals. So nothing here checks a recorded
// control point or a recorded evaluated value. Every test checks a
// property that a correct implementation must have and an almost-correct
// one will not:
//
//   * Basis functions against BasisFunctionDirect, a literal Cox-de Boor
//     recurrence that shares no code with the fast algorithm.
//   * Partition of unity, which is the defining property of a B-spline
//     basis and fails immediately for a bad knot vector or span index.
//   * Every derivative against central finite differences.
//   * Every shape-preserving operation (knot insertion and removal,
//     degree elevation, splitting, reversal, Bezier decomposition) for
//     *geometric invariance*: the control points may change however they
//     like, but the curve must not move.
//   * An exact circle. A circle is representable as a rational quadratic
//     NURBS and by no polynomial one, so checking that every evaluated
//     point lies at radius exactly 1 tests the rational machinery
//     against geometry whose answer is known in closed form -- and is the
//     single test most likely to catch a confusion between weighted and
//     Euclidean coordinates.

#include "cad_nurbs.h"

#include "cad_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
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

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool NearVec(const cad::Vec3d &a, const cad::Vec3d &b, double tol) { return (a - b).Length() <= tol; }

// A non-trivial test curve: degree 3, 7 control points, a non-uniform
// knot vector with one interior knot of multiplicity 2 (so the curve is
// only C^1 there, exercising the repeated-knot paths), and weights that
// are genuinely not all 1.
struct TestCurve {
    int degree = 3;
    std::vector<double> knots;
    std::vector<cad::Vec4d> control;
};

TestCurve MakeTestCurve(bool rational) {
    TestCurve c;
    c.degree = 3;
    c.knots = {0.0, 0.0, 0.0, 0.0, 0.25, 0.5, 0.5, 1.0, 1.0, 1.0, 1.0};
    const cad::Vec3d points[7] = {{0.0, 0.0, 0.0},  {1.0, 2.0, 0.5}, {3.0, 1.0, -1.0}, {4.0, -1.0, 0.5},
                                  {6.0, 0.5, 1.5}, {7.0, 3.0, 0.0}, {9.0, 1.0, -0.5}};
    const double weights[7] = {1.0, 2.0, 0.5, 1.5, 1.0, 3.0, 1.0};
    for (int i = 0; i < 7; ++i) {
        c.control.push_back(cad::Vec4d::FromWeighted(points[i], rational ? weights[i] : 1.0));
    }
    return c;
}

cad::Vec3d EvaluateCurve(const TestCurve &c, double u) {
    return cad::CurvePointHomogeneous(c.degree, c.knots, c.control, u).Project();
}

// The standard nine-point rational quadratic representation of the full
// unit circle. Not a construction of this codebase -- it is the textbook
// one, which is what makes it a fair external check.
TestCurve MakeUnitCircle() {
    TestCurve c;
    c.degree = 2;
    c.knots = {0.0, 0.0, 0.0, 0.25, 0.25, 0.5, 0.5, 0.75, 0.75, 1.0, 1.0, 1.0};
    const double s = std::sqrt(2.0) / 2.0;
    const cad::Vec3d points[9] = {{1, 0, 0}, {1, 1, 0},   {0, 1, 0},  {-1, 1, 0}, {-1, 0, 0},
                                  {-1, -1, 0}, {0, -1, 0}, {1, -1, 0}, {1, 0, 0}};
    const double weights[9] = {1.0, s, 1.0, s, 1.0, s, 1.0, s, 1.0};
    for (int i = 0; i < 9; ++i) c.control.push_back(cad::Vec4d::FromWeighted(points[i], weights[i]));
    return c;
}

void TestKnotVectors() {
    std::string error;
    CHECK(cad::ValidateKnotVector({0, 0, 0, 0, 1, 1, 1, 1}, 3, 4, &error));
    // Wrong length.
    CHECK(!cad::ValidateKnotVector({0, 0, 0, 1, 1, 1}, 3, 4, &error));
    CHECK(error.find("entries") != std::string::npos);
    // Decreasing.
    CHECK(!cad::ValidateKnotVector({0, 0, 0, 0, 1, 0.5, 1, 1}, 3, 4, &error));
    CHECK(error.find("non-decreasing") != std::string::npos);
    // Interior multiplicity above the degree would disconnect the curve.
    CHECK(!cad::ValidateKnotVector({0, 0, 0, 0.5, 0.5, 0.5, 1, 1, 1}, 2, 6, &error));
    CHECK(error.find("multiplicity") != std::string::npos);
    // Interior multiplicity *equal* to the degree is legal (a C^0 kink).
    CHECK(cad::ValidateKnotVector({0, 0, 0, 0.5, 0.5, 1, 1, 1}, 2, 5, &error));
    // Degenerate range.
    CHECK(!cad::ValidateKnotVector({0, 0, 0, 0, 0, 0, 0, 0}, 3, 4, &error));

    const std::vector<double> uniform = cad::ClampedUniformKnots(3, 7);
    CHECK(uniform.size() == 11);
    CHECK(cad::ValidateKnotVector(uniform, 3, 7, &error));
    CHECK(cad::IsClamped(uniform, 3));
    CHECK(Near(uniform[0], 0.0, 1e-15) && Near(uniform.back(), 1.0, 1e-15));
    for (std::size_t i = 1; i < uniform.size(); ++i) CHECK(uniform[i] >= uniform[i - 1]);

    const TestCurve c = MakeTestCurve(true);
    double lo = 0.0;
    double hi = 0.0;
    cad::KnotDomain(c.knots, c.degree, static_cast<int>(c.control.size()), &lo, &hi);
    CHECK(Near(lo, 0.0, 1e-15) && Near(hi, 1.0, 1e-15));
    const std::vector<cad::KnotSpan> interior =
        cad::InteriorKnots(c.knots, c.degree, static_cast<int>(c.control.size()));
    // Two interior knots, not three: the knot vector is
    // {0,0,0,0, 0.25, 0.5,0.5, 1,1,1,1}, and the repeated 1s are the
    // clamped domain end rather than an interior knot.
    CHECK(interior.size() == 2);
    CHECK(Near(interior[0].value, 0.25, 1e-15) && interior[0].multiplicity == 1);
    CHECK(Near(interior[1].value, 0.5, 1e-15) && interior[1].multiplicity == 2);
    // The domain endpoints must never be reported as interior.
    for (const cad::KnotSpan &span : interior) CHECK(span.value > lo && span.value < hi);
}

void TestBasisFunctions() {
    const TestCurve c = MakeTestCurve(false);
    const int count = static_cast<int>(c.control.size());
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> pick(0.0, 1.0);

    for (int trial = 0; trial < 500; ++trial) {
        // Sample the domain, including its exact endpoints and both sides
        // of the double knot at 0.5, which is where span selection is
        // most easily got wrong.
        double u = pick(rng);
        if (trial == 0) u = 0.0;
        if (trial == 1) u = 1.0;
        if (trial == 2) u = 0.5;
        if (trial == 3) u = 0.25;
        if (trial == 4) u = std::nextafter(0.5, 0.0);
        if (trial == 5) u = std::nextafter(0.5, 1.0);

        const int span = cad::FindSpan(c.degree, c.knots, u, count);
        CHECK(span >= c.degree && span < count);
        // The span really does contain u (with the endpoint convention).
        CHECK(c.knots[static_cast<std::size_t>(span)] <= u || Near(u, 0.0, 0.0));
        CHECK(u <= c.knots[static_cast<std::size_t>(span + 1)]);

        std::vector<double> basis(static_cast<std::size_t>(c.degree + 1), 0.0);
        cad::BasisFunctions(span, u, c.degree, c.knots, basis.data());

        // Partition of unity, and non-negativity.
        double sum = 0.0;
        for (double b : basis) {
            CHECK(b >= -1e-14);
            sum += b;
        }
        CHECK(Near(sum, 1.0, 1e-13));

        // Against the independent Cox-de Boor recurrence, for every basis
        // function -- including the ones the fast algorithm never
        // computes, which must be zero.
        for (int i = 0; i < count; ++i) {
            const double direct = cad::BasisFunctionDirect(i, c.degree, c.knots, u);
            const int local = i - (span - c.degree);
            const double fast = (local >= 0 && local <= c.degree) ? basis[static_cast<std::size_t>(local)] : 0.0;
            CHECK(Near(direct, fast, 1e-12));
        }
    }
}

void TestBasisDerivatives() {
    const TestCurve c = MakeTestCurve(false);
    const int count = static_cast<int>(c.control.size());
    const double samples[5] = {0.1, 0.25, 0.4, 0.7, 0.9};
    for (double u : samples) {
        const int span = cad::FindSpan(c.degree, c.knots, u, count);
        std::vector<std::vector<double>> ders;
        cad::BasisFunctionDerivatives(span, u, c.degree, 2, c.knots, &ders);
        CHECK(ders.size() == 3);

        // Order 0 must reproduce the basis functions themselves.
        std::vector<double> basis(static_cast<std::size_t>(c.degree + 1), 0.0);
        cad::BasisFunctions(span, u, c.degree, c.knots, basis.data());
        for (int j = 0; j <= c.degree; ++j) {
            CHECK(Near(ders[0][static_cast<std::size_t>(j)], basis[static_cast<std::size_t>(j)], 1e-13));
        }

        // Derivatives of a partition of unity sum to zero.
        double first_sum = 0.0;
        double second_sum = 0.0;
        for (int j = 0; j <= c.degree; ++j) {
            first_sum += ders[1][static_cast<std::size_t>(j)];
            second_sum += ders[2][static_cast<std::size_t>(j)];
        }
        CHECK(Near(first_sum, 0.0, 1e-10));
        CHECK(Near(second_sum, 0.0, 1e-8));

        // Against central differences of the direct recurrence.
        const double h = 1e-6;
        for (int j = 0; j <= c.degree; ++j) {
            const int global = span - c.degree + j;
            const double plus = cad::BasisFunctionDirect(global, c.degree, c.knots, u + h);
            const double minus = cad::BasisFunctionDirect(global, c.degree, c.knots, u - h);
            const double numeric = (plus - minus) / (2.0 * h);
            CHECK(Near(ders[1][static_cast<std::size_t>(j)], numeric, 1e-5));
        }
    }
}

void TestCurveEvaluation() {
    for (int rational = 0; rational < 2; ++rational) {
        const TestCurve c = MakeTestCurve(rational != 0);
        const int count = static_cast<int>(c.control.size());
        // Evaluation must equal the direct weighted sum over every basis
        // function -- the definition, computed the slow way.
        const double samples[7] = {0.0, 0.1, 0.25, 0.5, 0.6, 0.95, 1.0};
        for (double u : samples) {
            cad::Vec4d direct{0, 0, 0, 0};
            for (int i = 0; i < count; ++i) {
                direct += c.control[static_cast<std::size_t>(i)] * cad::BasisFunctionDirect(i, c.degree, c.knots, u);
            }
            const cad::Vec4d fast = cad::CurvePointHomogeneous(c.degree, c.knots, c.control, u);
            CHECK(NearVec(direct.Project(), fast.Project(), 1e-11));
        }
        // A clamped curve starts and ends at its first and last control
        // points exactly.
        CHECK(NearVec(EvaluateCurve(c, 0.0), c.control.front().Project(), 1e-13));
        CHECK(NearVec(EvaluateCurve(c, 1.0), c.control.back().Project(), 1e-13));
    }
}

void TestCurveDerivatives() {
    // The rational case is the one that matters: for a non-rational curve
    // the Euclidean derivative is just the projection of the homogeneous
    // one, so a missing quotient-rule correction would go unnoticed.
    for (int rational = 0; rational < 2; ++rational) {
        const TestCurve c = MakeTestCurve(rational != 0);
        const double samples[5] = {0.05, 0.2, 0.35, 0.65, 0.9};
        for (double u : samples) {
            std::vector<cad::Vec4d> homogeneous;
            cad::CurveDerivativesHomogeneous(c.degree, c.knots, c.control, u, 2, &homogeneous);
            std::vector<cad::Vec3d> ders;
            cad::RationalDerivatives(homogeneous, 2, &ders);

            // Order 0 is the point itself.
            CHECK(NearVec(ders[0], EvaluateCurve(c, u), 1e-12));

            const double h = 1e-5;
            const cad::Vec3d plus = EvaluateCurve(c, u + h);
            const cad::Vec3d minus = EvaluateCurve(c, u - h);
            const cad::Vec3d centre = EvaluateCurve(c, u);
            const cad::Vec3d first_numeric = (plus - minus) / (2.0 * h);
            const cad::Vec3d second_numeric = (plus - centre * 2.0 + minus) / (h * h);
            CHECK(NearVec(ders[1], first_numeric, 1e-6 * (1.0 + ders[1].Length())));
            CHECK(NearVec(ders[2], second_numeric, 1e-3 * (1.0 + ders[2].Length())));
        }
        // Derivatives above the degree vanish identically.
        std::vector<cad::Vec4d> high;
        cad::CurveDerivativesHomogeneous(c.degree, c.knots, c.control, 0.4, c.degree + 2, &high);
        for (std::size_t k = static_cast<std::size_t>(c.degree) + 1; k < high.size(); ++k) {
            CHECK(Near(high[k].Weighted().Length(), 0.0, 1e-15));
        }
    }
}

// The exact circle. See this file's header for why this one carries more
// weight than its size suggests.
void TestExactCircle() {
    const TestCurve circle = MakeUnitCircle();
    std::string error;
    CHECK(cad::ValidateKnotVector(circle.knots, circle.degree, static_cast<int>(circle.control.size()), &error));

    double worst_radius = 0.0;
    for (int i = 0; i <= 400; ++i) {
        const double u = static_cast<double>(i) / 400.0;
        const cad::Vec3d p = EvaluateCurve(circle, u);
        worst_radius = std::max(worst_radius, std::fabs(p.Length() - 1.0));
        CHECK(Near(p.z, 0.0, 1e-14));
    }
    std::printf("  exact circle: worst radius error over 401 samples = %.3e\n", worst_radius);
    CHECK(worst_radius < 1e-14);

    // The quarter-point parameters land on the axes exactly.
    CHECK(NearVec(EvaluateCurve(circle, 0.0), cad::Vec3d(1, 0, 0), 1e-14));
    CHECK(NearVec(EvaluateCurve(circle, 0.25), cad::Vec3d(0, 1, 0), 1e-14));
    CHECK(NearVec(EvaluateCurve(circle, 0.5), cad::Vec3d(-1, 0, 0), 1e-14));
    CHECK(NearVec(EvaluateCurve(circle, 0.75), cad::Vec3d(0, -1, 0), 1e-14));

    // The tangent is perpendicular to the radius everywhere, and the
    // curvature is exactly 1. Curvature needs the *second* derivative, so
    // this is the test that would catch a missing binomial term in
    // RationalDerivatives -- a first-derivative-only check would not.
    double worst_curvature = 0.0;
    for (int i = 0; i < 200; ++i) {
        const double u = (static_cast<double>(i) + 0.5) / 200.0;
        std::vector<cad::Vec4d> homogeneous;
        cad::CurveDerivativesHomogeneous(circle.degree, circle.knots, circle.control, u, 2, &homogeneous);
        std::vector<cad::Vec3d> ders;
        cad::RationalDerivatives(homogeneous, 2, &ders);
        CHECK(Near(ders[0].Dot(ders[1]), 0.0, 1e-12));
        // kappa = |C' x C''| / |C'|^3
        const double speed = ders[1].Length();
        const double curvature = ders[1].Cross(ders[2]).Length() / (speed * speed * speed);
        worst_curvature = std::max(worst_curvature, std::fabs(curvature - 1.0));
    }
    std::printf("  exact circle: worst curvature error = %.3e (exact value 1)\n", worst_curvature);
    CHECK(worst_curvature < 1e-9);
}

// Every operation in this group must leave the curve where it was.
void TestShapePreservingOperations() {
    const TestCurve original = MakeTestCurve(true);
    auto agrees_with_original = [&original](int degree, const std::vector<double> &knots,
                                            const std::vector<cad::Vec4d> &control, double tol) {
        double worst = 0.0;
        for (int i = 0; i <= 200; ++i) {
            const double u = static_cast<double>(i) / 200.0;
            const cad::Vec3d a = EvaluateCurve(original, u);
            const cad::Vec3d b = cad::CurvePointHomogeneous(degree, knots, control, u).Project();
            worst = std::max(worst, (a - b).Length());
        }
        return worst <= tol;
    };

    // --- Knot insertion -------------------------------------------------
    {
        std::vector<double> knots = original.knots;
        std::vector<cad::Vec4d> control = original.control;
        CHECK(cad::InsertKnot(original.degree, &knots, &control, 0.3, 1));
        CHECK(control.size() == original.control.size() + 1);
        CHECK(knots.size() == original.knots.size() + 1);
        std::string error;
        CHECK(cad::ValidateKnotVector(knots, original.degree, static_cast<int>(control.size()), &error));
        CHECK(agrees_with_original(original.degree, knots, control, 1e-13));

        // Inserting at an existing knot, and inserting several at once.
        CHECK(cad::InsertKnot(original.degree, &knots, &control, 0.25, 2));
        CHECK(agrees_with_original(original.degree, knots, control, 1e-13));
        // Asking past the legal multiplicity is clamped, not refused --
        // 0.5 already has multiplicity 2 and the degree is 3.
        const std::size_t before = control.size();
        CHECK(cad::InsertKnot(original.degree, &knots, &control, 0.5, 5));
        CHECK(control.size() == before + 1);
        CHECK(agrees_with_original(original.degree, knots, control, 1e-13));
    }

    // --- Refinement (many knots at once) must match repeated insertion --
    {
        std::vector<double> refined_knots = original.knots;
        std::vector<cad::Vec4d> refined_control = original.control;
        const std::vector<double> extra = {0.1, 0.2, 0.3, 0.7, 0.8};
        CHECK(cad::RefineKnots(original.degree, &refined_knots, &refined_control, extra));
        CHECK(refined_control.size() == original.control.size() + extra.size());
        CHECK(agrees_with_original(original.degree, refined_knots, refined_control, 1e-13));

        std::vector<double> one_at_a_time_knots = original.knots;
        std::vector<cad::Vec4d> one_at_a_time_control = original.control;
        for (double u : extra) {
            CHECK(cad::InsertKnot(original.degree, &one_at_a_time_knots, &one_at_a_time_control, u, 1));
        }
        CHECK(one_at_a_time_control.size() == refined_control.size());
        for (std::size_t i = 0; i < refined_control.size(); ++i) {
            CHECK(NearVec(refined_control[i].Project(), one_at_a_time_control[i].Project(), 1e-11));
        }
    }

    // --- Knot removal undoes insertion ----------------------------------
    {
        std::vector<double> knots = original.knots;
        std::vector<cad::Vec4d> control = original.control;
        CHECK(cad::InsertKnot(original.degree, &knots, &control, 0.35, 1));
        const int removed = cad::RemoveKnot(original.degree, &knots, &control, 0.35, 1, 1e-9);
        CHECK(removed == 1);
        CHECK(control.size() == original.control.size());
        CHECK(knots.size() == original.knots.size());
        for (std::size_t i = 0; i < control.size(); ++i) {
            CHECK(NearVec(control[i].Project(), original.control[i].Project(), 1e-9));
        }
        // A knot that is genuinely part of the shape must NOT come out --
        // removing it would move the curve, and silently doing so is the
        // failure this function's tolerance argument exists to prevent.
        std::vector<double> stubborn_knots = original.knots;
        std::vector<cad::Vec4d> stubborn_control = original.control;
        const int refused = cad::RemoveKnot(original.degree, &stubborn_knots, &stubborn_control, 0.25, 1, 1e-12);
        CHECK(refused == 0);
        CHECK(stubborn_control.size() == original.control.size());
        // With an absurdly loose tolerance it will come out, and then the
        // curve really has moved -- which is the caller's choice to make.
        const int forced = cad::RemoveKnot(original.degree, &stubborn_knots, &stubborn_control, 0.25, 1, 1e9);
        CHECK(forced == 1);
        CHECK(stubborn_control.size() == original.control.size() - 1);
    }

    // --- Degree elevation ------------------------------------------------
    {
        for (int times = 1; times <= 3; ++times) {
            int degree = original.degree;
            std::vector<double> knots = original.knots;
            std::vector<cad::Vec4d> control = original.control;
            CHECK(cad::ElevateDegree(&degree, &knots, &control, times));
            CHECK(degree == original.degree + times);
            std::string error;
            CHECK(cad::ValidateKnotVector(knots, degree, static_cast<int>(control.size()), &error));
            CHECK(agrees_with_original(degree, knots, control, 1e-11));
            // End points are preserved exactly by degree elevation.
            CHECK(NearVec(control.front().Project(), original.control.front().Project(), 1e-12));
            CHECK(NearVec(control.back().Project(), original.control.back().Project(), 1e-12));
        }
    }

    // --- Splitting --------------------------------------------------------
    {
        const double split_at = 0.4;
        std::vector<double> left_knots;
        std::vector<cad::Vec4d> left_control;
        std::vector<double> right_knots;
        std::vector<cad::Vec4d> right_control;
        CHECK(cad::SplitCurve(original.degree, original.knots, original.control, split_at, &left_knots,
                              &left_control, &right_knots, &right_control));
        std::string error;
        CHECK(cad::ValidateKnotVector(left_knots, original.degree, static_cast<int>(left_control.size()), &error));
        CHECK(cad::ValidateKnotVector(right_knots, original.degree, static_cast<int>(right_control.size()), &error));
        CHECK(cad::IsClamped(left_knots, original.degree));
        CHECK(cad::IsClamped(right_knots, original.degree));
        for (int i = 0; i <= 100; ++i) {
            const double t = static_cast<double>(i) / 100.0;
            const double ul = t * split_at;
            const double ur = split_at + t * (1.0 - split_at);
            CHECK(NearVec(cad::CurvePointHomogeneous(original.degree, left_knots, left_control, ul).Project(),
                          EvaluateCurve(original, ul), 1e-12));
            CHECK(NearVec(cad::CurvePointHomogeneous(original.degree, right_knots, right_control, ur).Project(),
                          EvaluateCurve(original, ur), 1e-12));
        }
        // The two halves meet exactly at the split.
        CHECK(NearVec(left_control.back().Project(), right_control.front().Project(), 1e-13));
        // Splitting outside the open domain is refused.
        CHECK(!cad::SplitCurve(original.degree, original.knots, original.control, 0.0, &left_knots, &left_control,
                               &right_knots, &right_control));
        CHECK(!cad::SplitCurve(original.degree, original.knots, original.control, 1.0, &left_knots, &left_control,
                               &right_knots, &right_control));
    }

    // --- Reversal ---------------------------------------------------------
    {
        std::vector<double> knots = original.knots;
        std::vector<cad::Vec4d> control = original.control;
        cad::ReverseCurve(&knots, &control);
        std::string error;
        CHECK(cad::ValidateKnotVector(knots, original.degree, static_cast<int>(control.size()), &error));
        for (int i = 0; i <= 100; ++i) {
            const double u = static_cast<double>(i) / 100.0;
            CHECK(NearVec(cad::CurvePointHomogeneous(original.degree, knots, control, 1.0 - u).Project(),
                          EvaluateCurve(original, u), 1e-12));
        }
        // Reversing twice is the identity.
        cad::ReverseCurve(&knots, &control);
        CHECK(agrees_with_original(original.degree, knots, control, 1e-13));
    }

    // --- Bezier decomposition ---------------------------------------------
    {
        const cad::BezierSegments bezier =
            cad::DecomposeCurve(original.degree, original.knots, original.control);
        CHECK(bezier.degree == original.degree);
        CHECK(!bezier.segments.empty());
        // One breakpoint more than segments: no duplicated final segment.
        CHECK(bezier.breakpoints.size() == bezier.segments.size() + 1);
        // The distinct knot spans of this curve are [0,0.25], [0.25,0.5],
        // [0.5,1], so three segments.
        CHECK(bezier.segments.size() == 3);
        for (const std::vector<cad::Vec4d> &segment : bezier.segments) {
            CHECK(segment.size() == static_cast<std::size_t>(original.degree + 1));
        }
        // Each segment, evaluated as a clamped Bezier over its own
        // parameter interval, reproduces the original curve there.
        for (std::size_t s = 0; s < bezier.segments.size(); ++s) {
            const double a = bezier.breakpoints[s];
            const double b = bezier.breakpoints[s + 1];
            std::vector<double> bezier_knots;
            for (int k = 0; k <= original.degree; ++k) bezier_knots.push_back(0.0);
            for (int k = 0; k <= original.degree; ++k) bezier_knots.push_back(1.0);
            for (int i = 0; i <= 40; ++i) {
                const double t = static_cast<double>(i) / 40.0;
                const cad::Vec3d from_segment =
                    cad::CurvePointHomogeneous(original.degree, bezier_knots, bezier.segments[s], t).Project();
                const cad::Vec3d from_curve = EvaluateCurve(original, a + t * (b - a));
                CHECK(NearVec(from_segment, from_curve, 1e-11));
            }
        }
    }
}

void TestFitting() {
    // Interpolation must pass through every point, exactly.
    std::vector<cad::Vec3d> points;
    for (int i = 0; i < 12; ++i) {
        const double t = static_cast<double>(i) / 11.0;
        points.push_back(cad::Vec3d{std::cos(t * 3.0) * (1.0 + t), std::sin(t * 4.0), t * t * 2.0 - t});
    }
    const cad::Parameterization kinds[3] = {cad::Parameterization::Uniform, cad::Parameterization::ChordLength,
                                            cad::Parameterization::Centripetal};
    for (cad::Parameterization kind : kinds) {
        for (int degree = 2; degree <= 5; ++degree) {
            std::vector<double> knots;
            std::vector<cad::Vec4d> control;
            CHECK(cad::InterpolateCurve(points, degree, kind, &knots, &control));
            std::string error;
            CHECK(cad::ValidateKnotVector(knots, degree, static_cast<int>(control.size()), &error));
            CHECK(control.size() == points.size());
            const std::vector<double> parameters = cad::ComputeParameters(points, kind);
            double worst = 0.0;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const cad::Vec3d on_curve =
                    cad::CurvePointHomogeneous(degree, knots, control, parameters[i]).Project();
                worst = std::max(worst, (on_curve - points[i]).Length());
            }
            CHECK(worst < 1e-10);
        }
    }
    // Parameterization: all three start at 0, end at 1, and increase.
    for (cad::Parameterization kind : kinds) {
        const std::vector<double> parameters = cad::ComputeParameters(points, kind);
        CHECK(Near(parameters.front(), 0.0, 1e-15));
        CHECK(Near(parameters.back(), 1.0, 1e-15));
        for (std::size_t i = 1; i < parameters.size(); ++i) CHECK(parameters[i] > parameters[i - 1]);
    }
    // Too few points for the degree is refused rather than producing a
    // malformed curve.
    std::vector<double> knots;
    std::vector<cad::Vec4d> control;
    std::vector<cad::Vec3d> two_points = {points[0], points[1]};
    CHECK(!cad::InterpolateCurve(two_points, 3, cad::Parameterization::ChordLength, &knots, &control));

    // Approximation: fewer control points than data, endpoints exact, and
    // the fit close because the data is smooth.
    std::vector<cad::Vec3d> dense;
    for (int i = 0; i <= 200; ++i) {
        const double t = static_cast<double>(i) / 200.0;
        dense.push_back(cad::Vec3d{t * 4.0, std::sin(t * 6.283185307179586), std::cos(t * 3.141592653589793)});
    }
    std::vector<double> fit_knots;
    std::vector<cad::Vec4d> fit_control;
    CHECK(cad::ApproximateCurve(dense, 3, 15, cad::Parameterization::ChordLength, &fit_knots, &fit_control));
    CHECK(fit_control.size() == 15);
    CHECK(NearVec(fit_control.front().Project(), dense.front(), 1e-12));
    CHECK(NearVec(fit_control.back().Project(), dense.back(), 1e-12));
    const std::vector<double> dense_parameters =
        cad::ComputeParameters(dense, cad::Parameterization::ChordLength);
    double worst_fit = 0.0;
    for (std::size_t i = 0; i < dense.size(); ++i) {
        const cad::Vec3d on_curve =
            cad::CurvePointHomogeneous(3, fit_knots, fit_control, dense_parameters[i]).Project();
        worst_fit = std::max(worst_fit, (on_curve - dense[i]).Length());
    }
    std::printf("  approximation: 201 points -> 15 control points, worst error %.3e\n", worst_fit);
    CHECK(worst_fit < 5e-3);
    // More control points must fit strictly better.
    std::vector<double> finer_knots;
    std::vector<cad::Vec4d> finer_control;
    CHECK(cad::ApproximateCurve(dense, 3, 40, cad::Parameterization::ChordLength, &finer_knots, &finer_control));
    double finer_worst = 0.0;
    for (std::size_t i = 0; i < dense.size(); ++i) {
        const cad::Vec3d on_curve =
            cad::CurvePointHomogeneous(3, finer_knots, finer_control, dense_parameters[i]).Project();
        finer_worst = std::max(finer_worst, (on_curve - dense[i]).Length());
    }
    CHECK(finer_worst < worst_fit);
}

// A bicubic test surface with non-uniform knots in both directions and
// genuinely varying weights.
struct TestSurface {
    int degree_u = 3;
    int degree_v = 2;
    std::vector<double> knots_u;
    std::vector<double> knots_v;
    std::vector<cad::Vec4d> control;
    int count_u = 6;
    int count_v = 5;
};

TestSurface MakeTestSurface(bool rational) {
    TestSurface s;
    s.degree_u = 3;
    s.degree_v = 2;
    s.count_u = 6;
    s.count_v = 5;
    s.knots_u = {0.0, 0.0, 0.0, 0.0, 0.4, 0.7, 1.0, 1.0, 1.0, 1.0};
    s.knots_v = {0.0, 0.0, 0.0, 0.5, 0.5, 1.0, 1.0, 1.0};
    std::mt19937 rng(4242);
    std::uniform_real_distribution<double> jitter(-0.3, 0.3);
    std::uniform_real_distribution<double> weight(0.5, 2.0);
    for (int i = 0; i < s.count_u; ++i) {
        for (int j = 0; j < s.count_v; ++j) {
            const cad::Vec3d p{static_cast<double>(i) + jitter(rng), static_cast<double>(j) + jitter(rng),
                               std::sin(static_cast<double>(i * j)) + jitter(rng)};
            s.control.push_back(cad::Vec4d::FromWeighted(p, rational ? weight(rng) : 1.0));
        }
    }
    return s;
}

cad::Vec3d EvaluateSurface(const TestSurface &s, double u, double v) {
    return cad::SurfacePointHomogeneous(s.degree_u, s.degree_v, s.knots_u, s.knots_v, s.control, s.count_u,
                                        s.count_v, u, v)
        .Project();
}

void TestSurfaceEvaluation() {
    for (int rational = 0; rational < 2; ++rational) {
        const TestSurface s = MakeTestSurface(rational != 0);
        std::string error;
        CHECK(cad::ValidateKnotVector(s.knots_u, s.degree_u, s.count_u, &error));
        CHECK(cad::ValidateKnotVector(s.knots_v, s.degree_v, s.count_v, &error));

        // Against the full double sum over every basis function pair.
        const double samples[4] = {0.0, 0.3, 0.75, 1.0};
        for (double u : samples) {
            for (double v : samples) {
                cad::Vec4d direct{0, 0, 0, 0};
                for (int i = 0; i < s.count_u; ++i) {
                    const double bu = cad::BasisFunctionDirect(i, s.degree_u, s.knots_u, u);
                    if (bu == 0.0) continue;
                    for (int j = 0; j < s.count_v; ++j) {
                        const double bv = cad::BasisFunctionDirect(j, s.degree_v, s.knots_v, v);
                        direct += s.control[cad::SurfaceIndex(i, j, s.count_v)] * (bu * bv);
                    }
                }
                CHECK(NearVec(direct.Project(), EvaluateSurface(s, u, v), 1e-11));
            }
        }
        // Corners coincide with corner control points.
        CHECK(NearVec(EvaluateSurface(s, 0.0, 0.0), s.control[cad::SurfaceIndex(0, 0, s.count_v)].Project(), 1e-12));
        CHECK(NearVec(EvaluateSurface(s, 1.0, 1.0),
                      s.control[cad::SurfaceIndex(s.count_u - 1, s.count_v - 1, s.count_v)].Project(), 1e-12));
    }
}

void TestSurfaceDerivatives() {
    for (int rational = 0; rational < 2; ++rational) {
        const TestSurface s = MakeTestSurface(rational != 0);
        const double samples[3] = {0.2, 0.55, 0.85};
        for (double u : samples) {
            for (double v : samples) {
                std::vector<std::vector<cad::Vec4d>> homogeneous;
                cad::SurfaceDerivativesHomogeneous(s.degree_u, s.degree_v, s.knots_u, s.knots_v, s.control,
                                                   s.count_u, s.count_v, u, v, 2, &homogeneous);
                std::vector<std::vector<cad::Vec3d>> ders;
                cad::RationalSurfaceDerivatives(homogeneous, 2, &ders);

                CHECK(NearVec(ders[0][0], EvaluateSurface(s, u, v), 1e-12));

                const double h = 1e-5;
                const cad::Vec3d du_numeric =
                    (EvaluateSurface(s, u + h, v) - EvaluateSurface(s, u - h, v)) / (2.0 * h);
                const cad::Vec3d dv_numeric =
                    (EvaluateSurface(s, u, v + h) - EvaluateSurface(s, u, v - h)) / (2.0 * h);
                CHECK(NearVec(ders[1][0], du_numeric, 1e-5 * (1.0 + ders[1][0].Length())));
                CHECK(NearVec(ders[0][1], dv_numeric, 1e-5 * (1.0 + ders[0][1].Length())));

                // The mixed partial -- the term the surface rational
                // correction is most likely to get wrong, since it is the
                // only one where both binomial sums interact.
                const double hm = 1e-4;
                const cad::Vec3d mixed_numeric =
                    (EvaluateSurface(s, u + hm, v + hm) - EvaluateSurface(s, u + hm, v - hm) -
                     EvaluateSurface(s, u - hm, v + hm) + EvaluateSurface(s, u - hm, v - hm)) /
                    (4.0 * hm * hm);
                CHECK(NearVec(ders[1][1], mixed_numeric, 1e-2 * (1.0 + ders[1][1].Length())));

                // A surface normal from the two tangents must be non-zero
                // on a non-degenerate patch.
                CHECK(ders[1][0].Cross(ders[0][1]).Length() > 1e-6);
            }
        }
    }
}

void TestSurfaceOperations() {
    const TestSurface s = MakeTestSurface(true);

    // Isocurves must agree with the surface along their own line.
    for (int fix_u = 0; fix_u < 2; ++fix_u) {
        const double fixed[3] = {0.0, 0.45, 1.0};
        for (double f : fixed) {
            int degree = 0;
            std::vector<double> knots;
            std::vector<cad::Vec4d> control;
            CHECK(cad::SurfaceIsoCurve(s.degree_u, s.degree_v, s.knots_u, s.knots_v, s.control, s.count_u,
                                       s.count_v, fix_u != 0, f, &degree, &knots, &control));
            CHECK(degree == (fix_u != 0 ? s.degree_v : s.degree_u));
            for (int i = 0; i <= 50; ++i) {
                const double t = static_cast<double>(i) / 50.0;
                const cad::Vec3d from_curve = cad::CurvePointHomogeneous(degree, knots, control, t).Project();
                const cad::Vec3d from_surface =
                    (fix_u != 0) ? EvaluateSurface(s, f, t) : EvaluateSurface(s, t, f);
                CHECK(NearVec(from_curve, from_surface, 1e-11));
            }
        }
    }

    // Surface knot insertion, in each direction, must leave the surface
    // unmoved while adding exactly one row or column.
    for (int in_u = 0; in_u < 2; ++in_u) {
        std::vector<double> knots_u = s.knots_u;
        std::vector<double> knots_v = s.knots_v;
        std::vector<cad::Vec4d> control = s.control;
        int count_u = s.count_u;
        int count_v = s.count_v;
        CHECK(cad::InsertKnotSurface(s.degree_u, s.degree_v, &knots_u, &knots_v, &control, &count_u, &count_v,
                                     in_u != 0, 0.35, 1));
        if (in_u != 0) {
            CHECK(count_u == s.count_u + 1);
            CHECK(count_v == s.count_v);
        } else {
            CHECK(count_u == s.count_u);
            CHECK(count_v == s.count_v + 1);
        }
        CHECK(control.size() == static_cast<std::size_t>(count_u * count_v));
        std::string error;
        CHECK(cad::ValidateKnotVector(knots_u, s.degree_u, count_u, &error));
        CHECK(cad::ValidateKnotVector(knots_v, s.degree_v, count_v, &error));
        double worst = 0.0;
        for (int i = 0; i <= 25; ++i) {
            for (int j = 0; j <= 25; ++j) {
                const double u = static_cast<double>(i) / 25.0;
                const double v = static_cast<double>(j) / 25.0;
                const cad::Vec3d after = cad::SurfacePointHomogeneous(s.degree_u, s.degree_v, knots_u, knots_v,
                                                                      control, count_u, count_v, u, v)
                                             .Project();
                worst = std::max(worst, (after - EvaluateSurface(s, u, v)).Length());
            }
        }
        CHECK(worst < 1e-12);
    }
}

}  // namespace

int main() {
    TestKnotVectors();
    TestBasisFunctions();
    TestBasisDerivatives();
    TestCurveEvaluation();
    TestCurveDerivatives();
    TestExactCircle();
    TestShapePreservingOperations();
    TestFitting();
    TestSurfaceEvaluation();
    TestSurfaceDerivatives();
    TestSurfaceOperations();
    std::printf("cad_nurbs_test passed (%d checks)\n", g_checks);
    return 0;
}
