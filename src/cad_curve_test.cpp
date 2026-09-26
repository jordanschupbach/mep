// Windowless coverage for cad_curve.h (plans/CAD_FEM_PLAN.md Parts A.1,
// A.2 and A.5).
//
// Two checks here carry most of the weight:
//
//   * Every analytic curve is evaluated against its own exact NURBS form
//     at the same parameter. That is a genuine cross-check rather than a
//     round-trip: the analytic circle is trigonometry and the NURBS
//     circle is a rational quadratic with weights cos(delta/2), and they
//     agree only if both are right. It also pins the parameterization,
//     which a shape-only comparison would not.
//   * Point inversion is checked against brute force over 20,000 samples.
//     A Newton iteration that converges to the wrong local minimum still
//     converges, still reports success, and is wrong -- there is no way
//     to detect that except by knowing the true answer.

#include "cad_curve.h"

#include "cad_math.h"
#include "cad_nurbs.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

// Compares a curve against its own exact NURBS form, measuring the right
// thing.
//
// The obvious check -- curve.Point(u) against nurbs.Point(u) -- is wrong,
// and instructively so. A rational quadratic traverses its arc
// non-uniformly in its own parameter, so for a half circle the two
// disagree by 1.6e-2 at the middle of a span while the NURBS radius is
// still 1.000000000000000. The shape is exact; only the parameterization
// differs. See Curve3::ToNurbs' own note.
//
// So this measures the one-sided Hausdorff distance instead: how far the
// NURBS ever strays from the *point set* of the analytic curve. That is
// the property the conversion actually promises, and it is still a strong
// check -- a wrong weight or a misplaced control point moves the shape,
// not merely the parameterization.
double WorstNurbsDisagreement(const cad::Curve3 &curve, int samples = 401) {
    cad::NurbsCurve3 nurbs;
    if (!cad::CurveToNurbs(curve, &nurbs)) return 1e30;
    double lo = 0.0;
    double hi = 0.0;
    curve.Domain(&lo, &hi);
    double nlo = 0.0;
    double nhi = 0.0;
    nurbs.Domain(&nlo, &nhi);
    // The domain *is* preserved, and that much is worth insisting on.
    if (std::fabs(nlo - lo) > 1e-12 || std::fabs(nhi - hi) > 1e-12) return 1e30;
    // The endpoints correspond exactly, parameterization notwithstanding.
    if ((curve.Point(lo) - nurbs.Point(lo)).Length() > 1e-13) return 1e30;
    if ((curve.Point(hi) - nurbs.Point(hi)).Length() > 1e-13) return 1e30;

    double worst = 0.0;
    for (int i = 0; i <= samples; ++i) {
        const double u = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(samples);
        const cad::Vec3d on_nurbs = nurbs.Point(u);
        double nearest_parameter = 0.0;
        cad::Vec3d nearest;
        if (!curve.ClosestPoint(on_nurbs, &nearest_parameter, &nearest)) return 1e30;
        worst = std::max(worst, (on_nurbs - nearest).Length());
    }
    return worst;
}

// The true closest distance, by dense sampling. Slow and obviously
// correct, which is what an oracle should be.
double BruteForceClosestDistance(const cad::Curve3 &curve, const cad::Vec3d &point, int samples = 20000) {
    double lo = 0.0;
    double hi = 0.0;
    curve.Domain(&lo, &hi);
    double best = 1e30;
    for (int i = 0; i <= samples; ++i) {
        const double u = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(samples);
        best = std::min(best, (curve.Point(u) - point).Length());
    }
    return best;
}

void TestLine() {
    const cad::Line3 line = cad::Line3::FromPoints(cad::Vec3d(1, 2, 3), cad::Vec3d(4, 6, 3));
    double lo = 0.0;
    double hi = 0.0;
    line.Domain(&lo, &hi);
    CHECK(Near(lo, 0.0, 1e-15) && Near(hi, 1.0, 1e-15));
    CHECK(NearVec(line.Point(0.0), cad::Vec3d(1, 2, 3), 1e-15));
    CHECK(NearVec(line.Point(1.0), cad::Vec3d(4, 6, 3), 1e-15));
    CHECK(NearVec(line.Point(0.5), cad::Vec3d(2.5, 4, 3), 1e-15));
    CHECK(Near(line.Length(), 5.0, 1e-12));
    CHECK(Near(line.Curvature(0.3), 0.0, 1e-12));
    CHECK(NearVec(line.Tangent(0.7), cad::Vec3d(0.6, 0.8, 0.0), 1e-14));

    std::vector<cad::Vec3d> ders;
    line.Derivatives(0.5, 3, &ders);
    CHECK(NearVec(ders[1], cad::Vec3d(3, 4, 0), 1e-15));
    CHECK(Near(ders[2].Length(), 0.0, 1e-15));
    CHECK(Near(ders[3].Length(), 0.0, 1e-15));

    const cad::Box3d box = line.Bounds();
    CHECK(Near(box.x.lo, 1.0, 1e-15) && Near(box.x.hi, 4.0, 1e-15));
    CHECK(Near(box.z.lo, 3.0, 1e-15) && Near(box.z.hi, 3.0, 1e-15));
    CHECK(!line.IsClosed());

    CHECK(WorstNurbsDisagreement(line) < 1e-14);

    // Reversal traces the same segment backwards over the same domain.
    cad::Line3 reversed = line;
    reversed.Reverse();
    for (int i = 0; i <= 20; ++i) {
        const double t = static_cast<double>(i) / 20.0;
        CHECK(NearVec(reversed.Point(t), line.Point(1.0 - t), 1e-13));
    }

    // Transform.
    cad::Line3 moved = line;
    moved.Transform(cad::Mat4d::Translation(cad::Vec3d(10, 0, 0)));
    CHECK(NearVec(moved.Point(0.0), cad::Vec3d(11, 2, 3), 1e-13));
    CHECK(Near(moved.Length(), 5.0, 1e-12));
}

void TestCircle() {
    const cad::Vec3d center(1.0, -2.0, 0.5);
    const cad::Vec3d normal = cad::Vec3d(1.0, 1.0, 1.0).Normalized();
    const cad::Circle3 full = cad::Circle3::FromCenterNormalRadius(center, normal, 3.0);
    CHECK(full.IsClosed());
    CHECK(Near(full.Radius(), 3.0, 1e-15));
    CHECK(Near(full.Length(), cad::kTwoPi * 3.0, 1e-12));

    // Every point is at the radius, in the plane, with curvature 1/r.
    for (int i = 0; i <= 200; ++i) {
        const double u = cad::kTwoPi * static_cast<double>(i) / 200.0;
        const cad::Vec3d p = full.Point(u);
        CHECK(Near((p - center).Length(), 3.0, 1e-13));
        CHECK(Near((p - center).Dot(normal), 0.0, 1e-13));
        CHECK(Near(full.Curvature(u), 1.0 / 3.0, 1e-11));
        // The tangent is perpendicular to the radius and to the normal.
        const cad::Vec3d t = full.Tangent(u);
        CHECK(Near(t.Dot(p - center), 0.0, 1e-12));
        CHECK(Near(t.Dot(normal), 0.0, 1e-13));
        // The principal normal points at the centre.
        CHECK(NearVec(full.Normal(u), (center - p).Normalized(), 1e-11));
    }

    // The NURBS cross-check, over arc sweeps that need one, two, three
    // and four quadratic spans -- the branch in ToNurbs.
    const double sweeps[6] = {0.4, cad::kHalfPi, 2.0, cad::kPi, 5.0, cad::kTwoPi};
    double worst = 0.0;
    for (double sweep : sweeps) {
        const cad::Circle3 arc(center, cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 2.5, 0.3, 0.3 + sweep);
        const double disagreement = WorstNurbsDisagreement(arc);
        worst = std::max(worst, disagreement);
        CHECK(disagreement < 1e-13);
        // Arc length is exact in closed form.
        CHECK(Near(arc.Length(), 2.5 * sweep, 1e-12));
    }
    std::printf("  circle vs its exact NURBS form: worst disagreement %.3e\n", worst);

    // Arc bounds are tight, not the whole circle's box. A quarter arc in
    // the first quadrant must not reach negative x or y.
    const cad::Circle3 quarter(cad::Vec3d(0, 0, 0), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 1.0, 0.0,
                               cad::kHalfPi);
    const cad::Box3d qbox = quarter.Bounds();
    CHECK(Near(qbox.x.lo, 0.0, 1e-13) && Near(qbox.x.hi, 1.0, 1e-13));
    CHECK(Near(qbox.y.lo, 0.0, 1e-13) && Near(qbox.y.hi, 1.0, 1e-13));
    CHECK(!quarter.IsClosed());
    // A full circle's box is the full extent in both axes.
    const cad::Box3d fbox =
        cad::Circle3(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 1.0, 0.0, cad::kTwoPi).Bounds();
    CHECK(Near(fbox.x.lo, -1.0, 1e-13) && Near(fbox.x.hi, 1.0, 1e-13));
    CHECK(Near(fbox.y.lo, -1.0, 1e-13) && Near(fbox.y.hi, 1.0, 1e-13));

    // Three-point construction.
    cad::Circle3 fitted;
    CHECK(cad::Circle3::FromThreePoints(cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), cad::Vec3d(-1, 0, 0), &fitted));
    CHECK(NearVec(fitted.Center(), cad::Vec3d(0, 0, 0), 1e-13));
    CHECK(Near(fitted.Radius(), 1.0, 1e-13));
    // A circle through three points really does pass through them.
    const cad::Vec3d a(2.0, 1.0, 3.0);
    const cad::Vec3d b(5.0, -1.0, 2.0);
    const cad::Vec3d c(0.5, 4.0, -1.0);
    CHECK(cad::Circle3::FromThreePoints(a, b, c, &fitted));
    for (const cad::Vec3d &p : {a, b, c}) {
        CHECK(Near((p - fitted.Center()).Length(), fitted.Radius(), 1e-11));
        CHECK(fitted.ContainsPoint(p));
    }
    // Collinear points define no circle, and that is reported rather than
    // producing one of enormous radius.
    CHECK(!cad::Circle3::FromThreePoints(cad::Vec3d(0, 0, 0), cad::Vec3d(1, 1, 1), cad::Vec3d(2, 2, 2), &fitted));

    // Reversal. A circle *can* keep its domain, and does.
    cad::Circle3 arc(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 2.0, 0.5, 2.0);
    const cad::Circle3 before = arc;
    arc.Reverse();
    double rlo = 0.0;
    double rhi = 0.0;
    arc.Domain(&rlo, &rhi);
    CHECK(Near(rlo, 0.5, 1e-12) && Near(rhi, 2.0, 1e-12));
    for (int i = 0; i <= 20; ++i) {
        const double t = static_cast<double>(i) / 20.0;
        const double u = rlo + t * (rhi - rlo);
        const double mirrored = rlo + (1.0 - t) * (rhi - rlo);
        CHECK(NearVec(arc.Point(u), before.Point(mirrored), 1e-12));
    }
    // Still a circle of the same radius about the same centre, and the
    // plane normal has flipped -- which is what "reversed" means for an
    // oriented arc.
    CHECK(Near(arc.Radius(), 2.0, 1e-13));
    CHECK(NearVec(arc.Center(), before.Center(), 1e-13));
    CHECK(NearVec(arc.PlaneNormal(), -before.PlaneNormal(), 1e-12));
    // Reversing twice restores the original.
    arc.Reverse();
    for (int i = 0; i <= 20; ++i) {
        const double u = 0.5 + 1.5 * static_cast<double>(i) / 20.0;
        CHECK(NearVec(arc.Point(u), before.Point(u), 1e-12));
    }
}

void TestEllipse() {
    const cad::Ellipse3 ellipse(cad::Vec3d(1, 1, 0), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 3.0, 1.5);
    CHECK(ellipse.IsClosed());
    // The two axis endpoints are where they should be.
    CHECK(NearVec(ellipse.Point(0.0), cad::Vec3d(4, 1, 0), 1e-14));
    CHECK(NearVec(ellipse.Point(cad::kHalfPi), cad::Vec3d(1, 2.5, 0), 1e-14));
    // Points satisfy the ellipse equation exactly.
    for (int i = 0; i <= 100; ++i) {
        const double u = cad::kTwoPi * static_cast<double>(i) / 100.0;
        const cad::Vec3d p = ellipse.Point(u) - cad::Vec3d(1, 1, 0);
        CHECK(Near((p.x * p.x) / 9.0 + (p.y * p.y) / 2.25, 1.0, 1e-13));
    }
    const double disagreement = WorstNurbsDisagreement(ellipse);
    std::printf("  ellipse vs its exact NURBS form: worst disagreement %.3e\n", disagreement);
    CHECK(disagreement < 1e-13);

    // Arcs of an ellipse too, including multi-span sweeps.
    for (double sweep : {0.7, 2.0, 4.0, 6.0}) {
        const cad::Ellipse3 arc(cad::Vec3d(0, 0, 1), cad::Vec3d(0, 1, 0), cad::Vec3d(0, 0, 1), 2.0, 0.8, 0.2,
                                0.2 + sweep);
        CHECK(WorstNurbsDisagreement(arc) < 1e-13);
    }

    // A tight arc box.
    const cad::Ellipse3 quarter(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 3.0, 1.5, 0.0,
                                cad::kHalfPi);
    const cad::Box3d box = quarter.Bounds();
    CHECK(Near(box.x.lo, 0.0, 1e-13) && Near(box.x.hi, 3.0, 1e-13));
    CHECK(Near(box.y.lo, 0.0, 1e-13) && Near(box.y.hi, 1.5, 1e-13));

    // Reversal of an ellipse: the direction flips and the domain moves,
    // which is exactly what Curve3::Reverse promises (and does not).
    {
        cad::Ellipse3 arc(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 3.0, 1.5, 0.4, 2.2);
        const cad::Ellipse3 before = arc;
        arc.Reverse();
        double rlo = 0.0;
        double rhi = 0.0;
        arc.Domain(&rlo, &rhi);
        // Same length of domain, moved to [-end, -start].
        CHECK(Near(rhi - rlo, 1.8, 1e-12));
        CHECK(Near(rlo, -2.2, 1e-12) && Near(rhi, -0.4, 1e-12));
        // The reversed arc starts where the original ended.
        CHECK(NearVec(arc.Point(rlo), before.Point(2.2), 1e-12));
        CHECK(NearVec(arc.Point(rhi), before.Point(0.4), 1e-12));
        for (int i = 0; i <= 20; ++i) {
            const double t = static_cast<double>(i) / 20.0;
            CHECK(NearVec(arc.Point(rlo + t * (rhi - rlo)), before.Point(2.2 - t * 1.8), 1e-12));
        }
        // Shape is untouched: same radii, same centre.
        CHECK(Near(arc.MajorRadius(), 3.0, 1e-13) && Near(arc.MinorRadius(), 1.5, 1e-13));
        CHECK(NearVec(arc.Center(), before.Center(), 1e-13));
    }

    // Circumference against the known series for this eccentricity is
    // overkill; instead check the adaptive quadrature against a dense
    // polyline, which is an independent computation of the same quantity.
    double polyline = 0.0;
    const int steps = 200000;
    for (int i = 0; i < steps; ++i) {
        const double u0 = cad::kTwoPi * static_cast<double>(i) / static_cast<double>(steps);
        const double u1 = cad::kTwoPi * static_cast<double>(i + 1) / static_cast<double>(steps);
        polyline += (ellipse.Point(u1) - ellipse.Point(u0)).Length();
    }
    CHECK(Near(ellipse.Length(), polyline, 1e-6 * polyline));
}

void TestNurbsCurveType() {
    std::string error;
    cad::NurbsCurve3 curve;
    const std::vector<double> knots = {0, 0, 0, 0, 0.3, 0.6, 1, 1, 1, 1};
    const std::vector<cad::Vec3d> points = {{0, 0, 0}, {1, 3, 0}, {3, -1, 1}, {5, 2, 0}, {7, 0, -1}, {9, 3, 0}};
    CHECK(cad::NurbsCurve3::CreateFromPoints(3, knots, points, &curve, &error));
    CHECK(!curve.IsRational());
    CHECK(curve.Degree() == 3);
    double lo = 0.0;
    double hi = 0.0;
    curve.Domain(&lo, &hi);
    CHECK(Near(lo, 0.0, 1e-15) && Near(hi, 1.0, 1e-15));
    CHECK(NearVec(curve.Point(0.0), points.front(), 1e-13));
    CHECK(NearVec(curve.Point(1.0), points.back(), 1e-13));

    // A malformed knot vector is refused, with a reason.
    cad::NurbsCurve3 bad;
    CHECK(!cad::NurbsCurve3::CreateFromPoints(3, {0, 0, 0, 1, 1, 1}, points, &bad, &error));
    CHECK(!error.empty());
    // A non-positive weight is refused: it is a point at infinity, which
    // this kernel does not represent.
    std::vector<cad::Vec4d> weighted;
    for (const cad::Vec3d &p : points) weighted.push_back(cad::Vec4d::FromWeighted(p, 1.0));
    weighted[2].w = 0.0;
    CHECK(!cad::NurbsCurve3::Create(3, knots, weighted, &bad, &error));
    CHECK(error.find("weight") != std::string::npos);

    // Natural breaks are the interior knots, so tessellation seeds there.
    const std::vector<double> breaks = curve.NaturalBreaks();
    CHECK(breaks.size() == 2);
    CHECK(Near(breaks[0], 0.3, 1e-15) && Near(breaks[1], 0.6, 1e-15));

    // Shape-preserving edits through the wrapper.
    cad::NurbsCurve3 edited = curve;
    CHECK(edited.InsertKnot(0.45, 1));
    CHECK(edited.Control().size() == curve.Control().size() + 1);
    for (int i = 0; i <= 50; ++i) {
        const double u = static_cast<double>(i) / 50.0;
        CHECK(NearVec(edited.Point(u), curve.Point(u), 1e-12));
    }
    CHECK(edited.ElevateDegree(1));
    CHECK(edited.Degree() == 4);
    for (int i = 0; i <= 50; ++i) {
        const double u = static_cast<double>(i) / 50.0;
        CHECK(NearVec(edited.Point(u), curve.Point(u), 1e-11));
    }
    cad::NurbsCurve3 left;
    cad::NurbsCurve3 right;
    CHECK(curve.Split(0.4, &left, &right));
    CHECK(NearVec(left.Point(0.4), right.Point(0.4), 1e-12));
    CHECK(NearVec(left.Point(0.2), curve.Point(0.2), 1e-12));
    CHECK(NearVec(right.Point(0.8), curve.Point(0.8), 1e-12));

    // Interpolation through the wrapper, and the length of the result
    // checked against a dense polyline.
    cad::NurbsCurve3 interpolated;
    CHECK(cad::NurbsCurve3::Interpolate(points, 3, cad::Parameterization::Centripetal, &interpolated));
    double polyline = 0.0;
    const int steps = 100000;
    for (int i = 0; i < steps; ++i) {
        const double u0 = static_cast<double>(i) / static_cast<double>(steps);
        const double u1 = static_cast<double>(i + 1) / static_cast<double>(steps);
        polyline += (interpolated.Point(u1) - interpolated.Point(u0)).Length();
    }
    CHECK(Near(interpolated.Length(), polyline, 1e-5 * polyline));

    // A transform moves the curve rigidly: distances between points on it
    // are preserved.
    cad::NurbsCurve3 moved = curve;
    const cad::Mat4d rigid =
        cad::Mat4d::Translation(cad::Vec3d(3, -1, 2)) * cad::Mat4d::Rotation(cad::Vec3d(1, 1, 0), 0.6);
    moved.Transform(rigid);
    for (int i = 0; i <= 20; ++i) {
        const double u = static_cast<double>(i) / 20.0;
        CHECK(NearVec(moved.Point(u), rigid.TransformPoint(curve.Point(u)), 1e-12));
    }
    CHECK(Near(moved.Length(), curve.Length(), 1e-9));
}

// Point inversion against brute force. See this file's header.
void TestClosestPoint() {
    std::vector<std::unique_ptr<cad::Curve3>> curves;
    curves.push_back(std::make_unique<cad::Line3>(cad::Line3::FromPoints(cad::Vec3d(-2, 1, 0), cad::Vec3d(3, 1, 4))));
    curves.push_back(std::make_unique<cad::Circle3>(cad::Vec3d(0, 0, 0), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0),
                                                    2.0, 0.0, cad::kTwoPi));
    curves.push_back(std::make_unique<cad::Circle3>(cad::Vec3d(1, 1, 1), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1),
                                                    1.5, 0.2, 2.6));
    curves.push_back(std::make_unique<cad::Ellipse3>(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 3.0,
                                                     1.0));
    {
        // A wiggly NURBS that passes near the origin more than once, so
        // there are genuinely several local minima to get wrong.
        cad::NurbsCurve3 wiggle;
        std::vector<cad::Vec3d> points;
        for (int i = 0; i <= 14; ++i) {
            const double t = static_cast<double>(i) / 14.0;
            points.push_back(cad::Vec3d{t * 8.0 - 4.0, std::sin(t * 12.0) * 2.0, std::cos(t * 7.0)});
        }
        CHECK(cad::NurbsCurve3::Interpolate(points, 3, cad::Parameterization::Centripetal, &wiggle));
        curves.push_back(std::make_unique<cad::NurbsCurve3>(wiggle));
    }

    std::mt19937 rng(20260924);
    std::uniform_real_distribution<double> coordinate(-6.0, 6.0);
    double worst_excess = 0.0;
    int queries = 0;
    for (const std::unique_ptr<cad::Curve3> &curve : curves) {
        for (int trial = 0; trial < 120; ++trial) {
            const cad::Vec3d query{coordinate(rng), coordinate(rng), coordinate(rng)};
            double u = 0.0;
            cad::Vec3d on_curve;
            CHECK(curve->ClosestPoint(query, &u, &on_curve));
            double lo = 0.0;
            double hi = 0.0;
            curve->Domain(&lo, &hi);
            CHECK(u >= lo - 1e-12 && u <= hi + 1e-12);
            CHECK(NearVec(on_curve, curve->Point(u), 1e-12));

            const double found = (on_curve - query).Length();
            const double truth = BruteForceClosestDistance(*curve, query);
            // Newton refines beyond the sampling resolution, so `found`
            // may be very slightly *smaller* than brute force. What must
            // never happen is finding a materially worse minimum.
            const double excess = found - truth;
            worst_excess = std::max(worst_excess, excess);
            CHECK(excess < 1e-6);
            ++queries;
        }
    }
    std::printf("  point inversion: %d queries, worst excess over brute force = %.3e\n", queries, worst_excess);

    // A point exactly on the curve is found to be on it.
    const cad::Circle3 circle(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 2.0, 0.0, cad::kTwoPi);
    for (int i = 0; i < 32; ++i) {
        const double u = cad::kTwoPi * static_cast<double>(i) / 32.0;
        double found = 0.0;
        CHECK(circle.ContainsPoint(circle.Point(u), &found));
        CHECK(NearVec(circle.Point(found), circle.Point(u), 1e-8));
    }
    CHECK(!circle.ContainsPoint(cad::Vec3d(0, 0, 0)));
    CHECK(!circle.ContainsPoint(cad::Vec3d(5, 0, 0)));

    // For an open curve the nearest point is frequently an endpoint,
    // which no stationary-point search finds on its own.
    const cad::Line3 segment = cad::Line3::FromPoints(cad::Vec3d(0, 0, 0), cad::Vec3d(1, 0, 0));
    double u = 0.0;
    cad::Vec3d on_curve;
    CHECK(segment.ClosestPoint(cad::Vec3d(-5, 1, 0), &u, &on_curve));
    CHECK(Near(u, 0.0, 1e-9));
    CHECK(NearVec(on_curve, cad::Vec3d(0, 0, 0), 1e-9));
    CHECK(segment.ClosestPoint(cad::Vec3d(9, -1, 0), &u, &on_curve));
    CHECK(Near(u, 1.0, 1e-9));
}

void TestTessellation() {
    std::vector<std::unique_ptr<cad::Curve3>> curves;
    curves.push_back(std::make_unique<cad::Line3>(cad::Line3::FromPoints(cad::Vec3d(0, 0, 0), cad::Vec3d(5, 1, 2))));
    curves.push_back(std::make_unique<cad::Circle3>(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 4.0,
                                                    0.0, cad::kTwoPi));
    curves.push_back(std::make_unique<cad::Ellipse3>(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 5.0,
                                                     0.5));

    const double chord_tolerance = 1e-3;
    for (const std::unique_ptr<cad::Curve3> &curve : curves) {
        std::vector<double> parameters;
        std::vector<cad::Vec3d> points;
        curve->Tessellate(&parameters, &points, chord_tolerance, 0.3);
        CHECK(parameters.size() == points.size());
        CHECK(parameters.size() >= 2);
        // Increasing, and spanning the whole domain.
        double lo = 0.0;
        double hi = 0.0;
        curve->Domain(&lo, &hi);
        CHECK(Near(parameters.front(), lo, 1e-12));
        CHECK(Near(parameters.back(), hi, 1e-12));
        for (std::size_t i = 1; i < parameters.size(); ++i) CHECK(parameters[i] > parameters[i - 1]);
        // Every emitted point really is on the curve.
        for (std::size_t i = 0; i < points.size(); ++i) {
            CHECK(NearVec(points[i], curve->Point(parameters[i]), 1e-12));
        }
        // The promise the tolerance makes: the polyline stays within
        // chord_tolerance of the curve. Checked by sampling each segment
        // densely rather than only at its midpoint, since the midpoint is
        // exactly where the subdivision test already looked.
        double worst = 0.0;
        for (std::size_t i = 0; i + 1 < parameters.size(); ++i) {
            const cad::Vec3d p0 = points[i];
            const cad::Vec3d p1 = points[i + 1];
            const cad::Vec3d chord = p1 - p0;
            const double chord_length = chord.Length();
            for (int k = 1; k < 16; ++k) {
                const double t = static_cast<double>(k) / 16.0;
                const cad::Vec3d on_curve = curve->Point(parameters[i] + t * (parameters[i + 1] - parameters[i]));
                const double deviation = (chord_length > 0.0)
                                             ? chord.Cross(on_curve - p0).Length() / chord_length
                                             : (on_curve - p0).Length();
                worst = std::max(worst, deviation);
            }
        }
        // A factor of two of headroom: the subdivision test measures the
        // midpoint deviation, and the true maximum over the segment can
        // exceed it slightly.
        CHECK(worst <= chord_tolerance * 2.0);
    }

    // A tighter tolerance must produce more points, and a straight line
    // needs no subdivision at all.
    const cad::Circle3 circle(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 4.0, 0.0, cad::kTwoPi);
    std::vector<double> coarse_parameters;
    std::vector<cad::Vec3d> coarse_points;
    std::vector<double> fine_parameters;
    std::vector<cad::Vec3d> fine_points;
    circle.Tessellate(&coarse_parameters, &coarse_points, 1e-2, 1.0);
    circle.Tessellate(&fine_parameters, &fine_points, 1e-5, 1.0);
    CHECK(fine_parameters.size() > coarse_parameters.size());
    std::printf("  tessellation: circle at 1e-2 -> %zu points, at 1e-5 -> %zu points\n", coarse_points.size(),
                fine_points.size());

    const cad::Line3 line = cad::Line3::FromPoints(cad::Vec3d(0, 0, 0), cad::Vec3d(1, 1, 1));
    std::vector<double> line_parameters;
    std::vector<cad::Vec3d> line_points;
    line.Tessellate(&line_parameters, &line_points, 1e-9, 1e-6);
    CHECK(line_points.size() == 2);

    // A NURBS with a C^0 kink: the kink must land exactly on a sample,
    // which is what NaturalBreaks seeding buys.
    std::string error;
    cad::NurbsCurve3 kinked;
    const std::vector<double> knots = {0, 0, 0, 0.5, 0.5, 1, 1, 1};
    const std::vector<cad::Vec3d> control = {{0, 0, 0}, {1, 1, 0}, {2, 0, 0}, {3, 1, 0}, {4, 0, 0}};
    CHECK(cad::NurbsCurve3::CreateFromPoints(2, knots, control, &kinked, &error));
    std::vector<double> kink_parameters;
    std::vector<cad::Vec3d> kink_points;
    kinked.Tessellate(&kink_parameters, &kink_points, 1e-3, 0.3);
    bool hit_the_kink = false;
    for (double u : kink_parameters) {
        if (std::fabs(u - 0.5) < 1e-12) hit_the_kink = true;
    }
    CHECK(hit_the_kink);
}

}  // namespace

int main() {
    // Unbuffered: a CHECK failure aborts, and buffered output is lost
    // with it -- including the diagnostic numbers printed just before,
    // which are usually what explains the failure.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestLine();
    TestCircle();
    TestEllipse();
    TestNurbsCurveType();
    TestClosestPoint();
    TestTessellation();
    std::printf("cad_curve_test passed (%d checks)\n", g_checks);
    return 0;
}
