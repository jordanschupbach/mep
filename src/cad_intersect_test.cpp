// Windowless coverage for cad_intersect.h and cad_classify.h
// (plans/CAD_FEM_PLAN.md Parts C.1, C.2 and the classification third of
// C.4).
//
// The defining property of an intersection curve is checkable exactly and
// cheaply: every point on it lies on *both* surfaces. That is asserted
// for every branch of every case below, and it is a far stronger test
// than comparing against a recorded curve -- it cannot be satisfied by a
// curve that is the right shape in the wrong place, or the right place
// with the wrong parameterization.
//
// On top of that, each analytic case has a closed-form answer worth
// checking against directly: a plane through a sphere's centre cuts a
// great circle, an oblique plane cuts a cylinder in an ellipse whose
// minor axis is the cylinder's own radius, and so on. Those pin the
// branch selection, which the on-both-surfaces test alone would not.

#include "cad_intersect.h"

#include "cad_classify.h"
#include "cad_math.h"
#include "cad_pcurve.h"
#include "cad_topology.h"

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

// The property every branch must have. Returns the worst distance from
// either surface over the whole branch.
double WorstOffSurface(const cad::SurfaceIntersection &result, const cad::Surface &a, const cad::Surface &b) {
    cad::Tolerance tolerance;
    double worst = 0.0;
    for (const cad::IntersectionBranch &branch : result.branches) {
        if (!branch.curve) return 1e30;
        double lo = 0.0;
        double hi = 0.0;
        branch.curve->Domain(&lo, &hi);
        for (int i = 0; i <= 64; ++i) {
            const double t = lo + (hi - lo) * static_cast<double>(i) / 64.0;
            const cad::Vec3d p = branch.curve->Point(t);
            double u = 0.0;
            double v = 0.0;
            cad::Vec3d on_a;
            cad::Vec3d on_b;
            a.ClosestPoint(p, &u, &v, &on_a, tolerance);
            b.ClosestPoint(p, &u, &v, &on_b, tolerance);
            worst = std::max(worst, std::max((p - on_a).Length(), (p - on_b).Length()));
        }
    }
    return worst;
}

cad::PlaneSurface BigPlane(const cad::Vec3d &origin, const cad::Vec3d &x, const cad::Vec3d &y,
                           double half = 20.0) {
    return cad::PlaneSurface(origin, x, y, -half, half, -half, half);
}

void TestCurveCurve() {
    // Two lines crossing at a known point.
    {
        const cad::Line3 a = cad::Line3::FromPoints(cad::Vec3d(-2, 0, 0), cad::Vec3d(2, 0, 0));
        const cad::Line3 b = cad::Line3::FromPoints(cad::Vec3d(0, -2, 0), cad::Vec3d(0, 2, 0));
        const std::vector<cad::CurveCurveHit> hits = cad::IntersectCurves(a, b);
        CHECK(hits.size() == 1);
        CHECK(hits[0].kind == cad::ContactKind::Transversal);
        CHECK((hits[0].point - cad::Vec3d(0, 0, 0)).Length() < 1e-9);
        CHECK(Near(hits[0].t1, 0.5, 1e-6));
        CHECK(Near(hits[0].t2, 0.5, 1e-6));
    }
    // Two lines that pass each other in space without meeting: generic in
    // 3D, and must report nothing rather than the closest approach.
    {
        const cad::Line3 a = cad::Line3::FromPoints(cad::Vec3d(-2, 0, 0), cad::Vec3d(2, 0, 0));
        const cad::Line3 b = cad::Line3::FromPoints(cad::Vec3d(0, -2, 1), cad::Vec3d(0, 2, 1));
        CHECK(cad::IntersectCurves(a, b).empty());
    }
    // Parallel, disjoint.
    {
        const cad::Line3 a = cad::Line3::FromPoints(cad::Vec3d(0, 0, 0), cad::Vec3d(1, 0, 0));
        const cad::Line3 b = cad::Line3::FromPoints(cad::Vec3d(0, 1, 0), cad::Vec3d(1, 1, 0));
        CHECK(cad::IntersectCurves(a, b).empty());
    }
    // A line through a circle: two crossings.
    {
        const cad::Circle3 circle(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 2.0, 0.0,
                                  cad::kTwoPi);
        const cad::Line3 line = cad::Line3::FromPoints(cad::Vec3d(-5, 0, 0), cad::Vec3d(5, 0, 0));
        const std::vector<cad::CurveCurveHit> hits = cad::IntersectCurves(circle, line);
        CHECK(hits.size() == 2);
        for (const cad::CurveCurveHit &hit : hits) {
            CHECK(Near(std::fabs(hit.point.x), 2.0, 1e-6));
            CHECK(Near(hit.point.y, 0.0, 1e-6));
        }
    }
    // A line tangent to a circle: one hit, reported as tangent. A boolean
    // that treated this as a crossing would produce a zero-width sliver.
    {
        const cad::Circle3 circle(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 2.0, 0.0,
                                  cad::kTwoPi);
        const cad::Line3 line = cad::Line3::FromPoints(cad::Vec3d(-5, 2, 0), cad::Vec3d(5, 2, 0));
        const std::vector<cad::CurveCurveHit> hits = cad::IntersectCurves(circle, line);
        CHECK(hits.size() == 1);
        CHECK(hits[0].kind == cad::ContactKind::Tangent);
        CHECK((hits[0].point - cad::Vec3d(0, 2, 0)).Length() < 1e-5);
    }
    // A curve against itself: coincident over its whole length, reported
    // once rather than as dozens of crossings.
    {
        const cad::Line3 a = cad::Line3::FromPoints(cad::Vec3d(0, 0, 0), cad::Vec3d(3, 1, 0));
        const std::vector<cad::CurveCurveHit> hits = cad::IntersectCurves(a, a);
        CHECK(hits.size() == 1);
        CHECK(hits[0].kind == cad::ContactKind::Coincident);
    }
}

void TestCurveSurface() {
    const cad::SphereSurface sphere(cad::Vec3d(), 2.0);
    // A line through the centre pierces the sphere twice.
    {
        const cad::Line3 line = cad::Line3::FromPoints(cad::Vec3d(-6, 0, 0), cad::Vec3d(6, 0, 0));
        const std::vector<cad::CurveSurfaceHit> hits = cad::IntersectCurveSurface(line, sphere);
        CHECK(hits.size() == 2);
        for (const cad::CurveSurfaceHit &hit : hits) {
            CHECK(Near(hit.point.Length(), 2.0, 1e-8));
            CHECK(hit.kind == cad::ContactKind::Transversal);
            CHECK((hit.point - sphere.Point(hit.u, hit.v)).Length() < 1e-8);
        }
    }
    // A line that misses entirely.
    {
        const cad::Line3 line = cad::Line3::FromPoints(cad::Vec3d(-6, 5, 0), cad::Vec3d(6, 5, 0));
        CHECK(cad::IntersectCurveSurface(line, sphere).empty());
    }
    // A line tangent to the sphere: one hit, reported as tangent.
    {
        const cad::Line3 line = cad::Line3::FromPoints(cad::Vec3d(-6, 2, 0), cad::Vec3d(6, 2, 0));
        const std::vector<cad::CurveSurfaceHit> hits = cad::IntersectCurveSurface(line, sphere);
        CHECK(hits.size() == 1);
        CHECK(hits[0].kind == cad::ContactKind::Tangent);
    }
    // A line through a plane, at a known place.
    {
        const cad::PlaneSurface plane = BigPlane(cad::Vec3d(0, 0, 3), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0));
        const cad::Line3 line = cad::Line3::FromPoints(cad::Vec3d(1, 1, 0), cad::Vec3d(1, 1, 10));
        const std::vector<cad::CurveSurfaceHit> hits = cad::IntersectCurveSurface(line, plane);
        CHECK(hits.size() == 1);
        CHECK((hits[0].point - cad::Vec3d(1, 1, 3)).Length() < 1e-9);
    }
}

void TestAnalyticTable() {
    const double big = 20.0;
    const cad::PlaneSurface xy = BigPlane(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), big);
    const cad::PlaneSurface xz = BigPlane(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), big);
    const cad::SphereSurface sphere(cad::Vec3d(), 2.0);
    const cad::CylinderSurface cylinder(cad::Vec3d(0, 0, -5), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), 1.5,
                                        0.0, cad::kTwoPi, 0.0, 10.0);
    const cad::TorusSurface torus(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), 3.0, 1.0);
    const cad::ConeSurface cone(cad::Vec3d(0, 0, 6), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, -1), 0.5, 0.0,
                                cad::kTwoPi, 0.1, 8.0);

    double worst_overall = 0.0;
    // `name` is carried for the failure message a CHECK would print if
    // this ever regresses; it is unused on the passing path.
    auto expect = [&](const char *name, const cad::Surface &a, const cad::Surface &b, cad::ContactKind kind,
                      std::size_t branches) {
        (void)name;
        cad::SurfaceIntersection result;
        CHECK(cad::IntersectSurfacesAnalytic(a, b, &result, {}));
        CHECK(result.exact);
        CHECK(result.kind == kind);
        CHECK(result.branches.size() == branches);
        const double worst = WorstOffSurface(result, a, b);
        worst_overall = std::max(worst_overall, worst);
        // Branches must lie on both surfaces. The clip tolerance sets the
        // floor for a line whose end is cut at a face boundary.
        CHECK(worst < 1e-6);
        return result;
    };

    // plane/plane
    {
        const cad::SurfaceIntersection result =
            expect("plane/plane", xy, xz, cad::ContactKind::Transversal, 1);
        CHECK(result.branches[0].curve->Kind() == cad::CurveKind::Line);
        // The x axis.
        for (int i = 0; i <= 8; ++i) {
            const cad::Vec3d p = result.branches[0].curve->Point(static_cast<double>(i) / 8.0);
            CHECK(Near(p.y, 0.0, 1e-9) && Near(p.z, 0.0, 1e-9));
        }
    }
    // Parallel and coincident planes are distinguished -- a boolean must
    // merge coincident faces rather than intersect them.
    {
        cad::SurfaceIntersection result;
        const cad::PlaneSurface shifted = BigPlane(cad::Vec3d(0, 0, 1), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0));
        CHECK(cad::IntersectSurfacesAnalytic(xy, shifted, &result, {}));
        CHECK(result.kind == cad::ContactKind::None);
        CHECK(cad::IntersectSurfacesAnalytic(xy, xy, &result, {}));
        CHECK(result.kind == cad::ContactKind::Coincident);
    }
    // plane/sphere through the centre: a great circle.
    {
        const cad::SurfaceIntersection result =
            expect("plane/sphere", xy, sphere, cad::ContactKind::Transversal, 1);
        CHECK(result.branches[0].curve->Kind() == cad::CurveKind::Circle);
        CHECK(result.branches[0].closed);
        CHECK(Near(result.branches[0].curve->Length(), cad::kTwoPi * 2.0, 1e-9));
    }
    // Offset: a smaller circle of exactly the predicted radius.
    {
        const cad::PlaneSurface offset = BigPlane(cad::Vec3d(0, 0, 1), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0));
        const cad::SurfaceIntersection result =
            expect("plane/sphere offset", offset, sphere, cad::ContactKind::Transversal, 1);
        const double expected = std::sqrt(4.0 - 1.0);
        CHECK(Near(result.branches[0].curve->Length(), cad::kTwoPi * expected, 1e-9));
    }
    // Tangent and miss are their own outcomes, not degenerate circles.
    {
        cad::SurfaceIntersection result;
        const cad::PlaneSurface touching =
            BigPlane(cad::Vec3d(0, 0, 2), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0));
        CHECK(cad::IntersectSurfacesAnalytic(touching, sphere, &result, {}));
        CHECK(result.kind == cad::ContactKind::Tangent);
        CHECK(result.branches.empty());
        const cad::PlaneSurface missing =
            BigPlane(cad::Vec3d(0, 0, 9), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0));
        CHECK(cad::IntersectSurfacesAnalytic(missing, sphere, &result, {}));
        CHECK(result.kind == cad::ContactKind::None);
    }
    // sphere/sphere
    {
        const cad::SphereSurface other(cad::Vec3d(3, 0, 0), 2.0);
        const cad::SurfaceIntersection result =
            expect("sphere/sphere", sphere, other, cad::ContactKind::Transversal, 1);
        // Centres 3 apart, both radius 2: the circle sits at x = 1.5 with
        // radius sqrt(4 - 2.25).
        const double expected = std::sqrt(4.0 - 2.25);
        CHECK(Near(result.branches[0].curve->Length(), cad::kTwoPi * expected, 1e-9));
        for (int i = 0; i <= 8; ++i) {
            const cad::Vec3d p = result.branches[0].curve->Point(cad::kTwoPi * static_cast<double>(i) / 8.0);
            CHECK(Near(p.x, 1.5, 1e-9));
        }
    }
    // plane/cylinder, all three branches of the case analysis.
    {
        const cad::SurfaceIntersection perpendicular =
            expect("plane/cyl perp", xy, cylinder, cad::ContactKind::Transversal, 1);
        CHECK(perpendicular.branches[0].curve->Kind() == cad::CurveKind::Circle);
        CHECK(Near(perpendicular.branches[0].curve->Length(), cad::kTwoPi * 1.5, 1e-9));

        const cad::SurfaceIntersection parallel =
            expect("plane/cyl parallel", xz, cylinder, cad::ContactKind::Transversal, 2);
        for (const cad::IntersectionBranch &branch : parallel.branches) {
            CHECK(branch.curve->Kind() == cad::CurveKind::Line);
        }

        // Oblique: an ellipse whose minor semi-axis is the cylinder's
        // radius and whose major is that over |normal . axis|.
        const cad::PlaneSurface tilted =
            BigPlane(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 1).Normalized(), big);
        const cad::SurfaceIntersection oblique =
            expect("plane/cyl oblique", tilted, cylinder, cad::ContactKind::Transversal, 1);
        CHECK(oblique.branches[0].curve->Kind() == cad::CurveKind::Ellipse);
        const auto *ellipse = dynamic_cast<const cad::Ellipse3 *>(oblique.branches[0].curve.get());
        CHECK(ellipse != nullptr);
        CHECK(Near(ellipse->MinorRadius(), 1.5, 1e-9));
        // The plane's normal is (0,-1,1)/sqrt(2), so |n.axis| = 1/sqrt(2).
        CHECK(Near(ellipse->MajorRadius(), 1.5 * std::sqrt(2.0), 1e-9));
    }
    // plane/cone perpendicular: a circle whose radius follows the half
    // angle and the distance from the apex.
    {
        const cad::SurfaceIntersection result =
            expect("plane/cone perp", xy, cone, cad::ContactKind::Transversal, 1);
        CHECK(Near(result.branches[0].curve->Length(), cad::kTwoPi * 6.0 * std::tan(0.5), 1e-8));
    }
    // plane/torus: two concentric circles, and two tube circles.
    {
        const cad::SurfaceIntersection perpendicular =
            expect("plane/torus perp", xy, torus, cad::ContactKind::Transversal, 2);
        double lengths[2] = {perpendicular.branches[0].curve->Length(),
                             perpendicular.branches[1].curve->Length()};
        if (lengths[0] > lengths[1]) std::swap(lengths[0], lengths[1]);
        CHECK(Near(lengths[0], cad::kTwoPi * 2.0, 1e-8));  // R - r
        CHECK(Near(lengths[1], cad::kTwoPi * 4.0, 1e-8));  // R + r

        const cad::SurfaceIntersection through_axis =
            expect("plane/torus axis", xz, torus, cad::ContactKind::Transversal, 2);
        for (const cad::IntersectionBranch &branch : through_axis.branches) {
            CHECK(Near(branch.curve->Length(), cad::kTwoPi * 1.0, 1e-8));
        }
    }
    // cylinder/cylinder, parallel axes.
    {
        const cad::CylinderSurface other(cad::Vec3d(2, 0, -5), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), 1.5,
                                         0.0, cad::kTwoPi, 0.0, 10.0);
        expect("cyl/cyl parallel", cylinder, other, cad::ContactKind::Transversal, 2);
        cad::SurfaceIntersection coaxial;
        CHECK(cad::IntersectSurfacesAnalytic(cylinder, cylinder, &coaxial, {}));
        CHECK(coaxial.kind == cad::ContactKind::Coincident);
    }
    // Coaxial sphere and cylinder: two circles.
    {
        const cad::CylinderSurface narrow(cad::Vec3d(0, 0, -5), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), 1.0,
                                          0.0, cad::kTwoPi, 0.0, 10.0);
        const cad::SurfaceIntersection result =
            expect("sphere/cyl coaxial", sphere, narrow, cad::ContactKind::Transversal, 2);
        for (const cad::IntersectionBranch &branch : result.branches) {
            CHECK(Near(branch.curve->Length(), cad::kTwoPi * 1.0, 1e-8));
        }
    }
    std::printf("  analytic table: worst distance from either surface over all branches = %.2e\n",
                worst_overall);

    // A pair with no closed form is declined by the table and picked up
    // by the general marcher, which reports itself as inexact so a caller
    // can tell which path produced the answer.
    {
        const cad::CylinderSurface skew(cad::Vec3d(0, 0, 0), cad::Vec3d(0, 0, 1), cad::Vec3d(1, 0, 0), 1.0, 0.0,
                                        cad::kTwoPi, -5.0, 5.0);
        cad::SurfaceIntersection result;
        CHECK(!cad::IntersectSurfacesAnalytic(cylinder, skew, &result, {}));
        const cad::SurfaceIntersection general = cad::IntersectSurfaces(cylinder, skew, {});
        CHECK(!general.exact);
        CHECK(!general.branches.empty());
        CHECK(WorstOffSurface(general, cylinder, skew) < 1e-4);
    }
}

void TestPCurveOnSurface() {
    // The intersection of a plane and a sphere, expressed back in the
    // sphere's own parameters, must map to the same circle.
    const cad::PlaneSurface plane = BigPlane(cad::Vec3d(0, 0, 1), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0));
    const cad::SphereSurface sphere(cad::Vec3d(), 2.0);
    cad::SurfaceIntersection result;
    CHECK(cad::IntersectSurfacesAnalytic(plane, sphere, &result, {}));
    CHECK(result.branches.size() == 1);

    double lo = 0.0;
    double hi = 0.0;
    result.branches[0].curve->Domain(&lo, &hi);
    std::shared_ptr<const cad::Curve3> pcurve;
    CHECK(cad::BuildPCurveOnSurface(sphere, *result.branches[0].curve, lo, hi, {}, &pcurve));
    double p_lo = 0.0;
    double p_hi = 0.0;
    pcurve->Domain(&p_lo, &p_hi);
    double worst = 0.0;
    for (int i = 0; i <= 32; ++i) {
        const double t = p_lo + (p_hi - p_lo) * static_cast<double>(i) / 32.0;
        const cad::Vec2d uv = cad::PCurvePoint(*pcurve, t);
        const cad::Vec3d p = sphere.Point(uv.x, uv.y);
        // On the plane, and at the right height.
        worst = std::max(worst, std::fabs(p.z - 1.0));
    }
    std::printf("  p-curve of an intersection on its surface: worst deviation %.2e\n", worst);
    CHECK(worst < 1e-6);
}

// Part C.3. The marcher has an unusually good oracle available: on every
// pair the analytic table also handles, the two must agree. That is a
// genuine cross-check between a closed form and a numerical walk sharing
// no code at all.
void TestMarcher() {
    const double big = 8.0;
    const cad::SphereSurface sphere(cad::Vec3d(), 2.0);

    struct AgreementCase {
        const char *name;
        const cad::Surface *a;
        const cad::Surface *b;
    };
    const cad::PlaneSurface cutting = BigPlane(cad::Vec3d(0, 0, 0.5), cad::Vec3d(1, 0, 0),
                                               cad::Vec3d(0, 1, 0), big);
    const cad::SphereSurface other(cad::Vec3d(3, 0, 0), 2.0);
    const AgreementCase agreement[2] = {
        {"plane/sphere", &cutting, &sphere},
        {"sphere/sphere", &sphere, &other},
    };
    for (const AgreementCase &c : agreement) {
        cad::SurfaceIntersection exact;
        CHECK(cad::IntersectSurfacesAnalytic(*c.a, *c.b, &exact, {}));
        CHECK(exact.branches.size() == 1);
        const cad::SurfaceIntersection marched = cad::MarchSurfaces(*c.a, *c.b, {});
        CHECK(!marched.exact);
        CHECK(marched.kind == cad::ContactKind::Transversal);
        // One curve, not several: a branch split at a seam would show up
        // here as two, which is exactly how the seam bug announced itself.
        CHECK(marched.branches.size() == 1);
        CHECK(marched.branches[0].closed);
        CHECK(marched.branches[0].pcurve_a != nullptr);
        CHECK(marched.branches[0].pcurve_b != nullptr);

        // Same length as the closed form, and every marched point on the
        // closed-form curve.
        const double exact_length = exact.branches[0].curve->Length();
        const double marched_length = marched.branches[0].curve->Length();
        double worst = 0.0;
        double lo = 0.0;
        double hi = 0.0;
        marched.branches[0].curve->Domain(&lo, &hi);
        for (int k = 0; k <= 64; ++k) {
            const cad::Vec3d p = marched.branches[0].curve->Point(lo + (hi - lo) * static_cast<double>(k) / 64.0);
            double t = 0.0;
            cad::Vec3d nearest;
            CHECK(exact.branches[0].curve->ClosestPoint(p, &t, &nearest));
            worst = std::max(worst, (p - nearest).Length());
        }
        std::printf("  marcher vs analytic (%s): length %.6f against %.6f, deviation %.2e\n", c.name,
                    marched_length, exact_length, worst);
        CHECK(std::fabs(marched_length - exact_length) < exact_length * 1e-4);
        CHECK(worst < 1e-5);
    }

    // A pair with no closed form at all: two cylinders crossing at right
    // angles. Nothing to compare against, so the defining property is
    // checked instead -- every point on both surfaces -- along with the
    // branch count, which is two closed curves for a rod passing through
    // a rod.
    {
        const cad::CylinderSurface upright(cad::Vec3d(0, 0, -4), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), 1.5,
                                           0.0, cad::kTwoPi, 0.0, 8.0);
        const cad::CylinderSurface across(cad::Vec3d(-4, 0, 0), cad::Vec3d(0, 1, 0), cad::Vec3d(1, 0, 0), 1.0,
                                          0.0, cad::kTwoPi, 0.0, 8.0);
        cad::SurfaceIntersection declined;
        CHECK(!cad::IntersectSurfacesAnalytic(upright, across, &declined, {}));
        const cad::SurfaceIntersection marched = cad::IntersectSurfaces(upright, across, {});
        CHECK(marched.kind == cad::ContactKind::Transversal);
        CHECK(marched.branches.size() == 2);
        double worst = 0.0;
        for (const cad::IntersectionBranch &branch : marched.branches) {
            CHECK(branch.closed);
            double lo = 0.0;
            double hi = 0.0;
            branch.curve->Domain(&lo, &hi);
            for (int k = 0; k <= 64; ++k) {
                const cad::Vec3d p = branch.curve->Point(lo + (hi - lo) * static_cast<double>(k) / 64.0);
                double u = 0.0;
                double v = 0.0;
                cad::Vec3d on_a;
                cad::Vec3d on_b;
                upright.ClosestPoint(p, &u, &v, &on_a, {});
                across.ClosestPoint(p, &u, &v, &on_b, {});
                worst = std::max(worst, std::max((p - on_a).Length(), (p - on_b).Length()));
            }
        }
        std::printf("  marcher, two crossing cylinders: 2 closed branches, worst off-surface %.2e\n", worst);
        CHECK(worst < 1e-5);
    }

    // A cylinder through a sphere gives two closed rims, one at each end.
    {
        const cad::CylinderSurface bore(cad::Vec3d(0.5, 0, -4), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), 1.0,
                                        0.0, cad::kTwoPi, 0.0, 8.0);
        const cad::SurfaceIntersection marched = cad::MarchSurfaces(sphere, bore, {});
        CHECK(marched.branches.size() == 2);
        for (const cad::IntersectionBranch &branch : marched.branches) CHECK(branch.closed);
    }

    // A tangential contact is reported as Tangent, not as "no
    // intersection". The surfaces genuinely touch -- there are seed
    // points -- and telling a boolean they are disjoint would be worse
    // than telling it the answer is unavailable.
    {
        const cad::CylinderSurface tangent(cad::Vec3d(1, 0, -4), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), 1.0,
                                           0.0, cad::kTwoPi, 0.0, 8.0);
        const cad::SurfaceIntersection marched = cad::MarchSurfaces(sphere, tangent, {});
        CHECK(marched.kind == cad::ContactKind::Tangent);
        CHECK(marched.branches.empty());
        CHECK(marched.note.find("tangential or singular") != std::string::npos);
        std::printf("  marcher, tangential case: refused with a reason rather than a wrong curve\n");
    }

    // Surfaces that do not meet at all: no seeds, no branches.
    {
        const cad::SphereSurface far_away(cad::Vec3d(50, 0, 0), 1.0);
        const cad::SurfaceIntersection marched = cad::MarchSurfaces(sphere, far_away, {});
        CHECK(marched.kind == cad::ContactKind::None);
        CHECK(marched.branches.empty());
    }
}

// The classification third of C.4.
void TestClassification() {
    struct Case {
        const char *name;
        cad::Vec3d inside;
        cad::Vec3d outside;
        cad::Vec3d boundary;
    };
    for (int which = 0; which < 4; ++which) {
        cad::Model model;
        cad::EntityId body = cad::kNoEntity;
        Case c{};
        switch (which) {
            case 0:
                CHECK(cad::MakeBox(cad::Vec3d(0, 0, 0), cad::Vec3d(2, 3, 4), &model, &body));
                c = {"box", cad::Vec3d(1, 1.5, 2), cad::Vec3d(5, 5, 5), cad::Vec3d(1, 1.5, 0)};
                break;
            case 1:
                CHECK(cad::MakeCylinder(cad::Vec3d(0, 0, 0), cad::Vec3d(0, 0, 1), 1.5, 4.0, &model, &body));
                c = {"cylinder", cad::Vec3d(0.3, 0.2, 2.0), cad::Vec3d(5, 0, 2), cad::Vec3d(1.5, 0, 2.0)};
                break;
            case 2:
                CHECK(cad::MakeSphere(cad::Vec3d(), 2.0, &model, &body));
                c = {"sphere", cad::Vec3d(0.4, -0.3, 0.2), cad::Vec3d(5, 0, 0), cad::Vec3d(0, 0, 2.0)};
                break;
            default:
                CHECK(cad::MakeTorus(cad::Vec3d(), cad::Vec3d(0, 0, 1), 3.0, 1.0, &model, &body));
                // Inside the tube; outside is on the axis, which is the
                // case a naive "inside the bounding box" test gets wrong.
                c = {"torus", cad::Vec3d(3.0, 0.0, 0.5), cad::Vec3d(0, 0, 0), cad::Vec3d(4.0, 0, 0)};
                break;
        }
        std::string error;
        CHECK(cad::BuildAllPCurves(&model, {}, &error));

        CHECK(cad::ClassifyPoint(model, body, c.inside) == cad::PointClass::Inside);
        CHECK(cad::ClassifyPoint(model, body, c.outside) == cad::PointClass::Outside);
        CHECK(cad::ClassifyPoint(model, body, c.boundary) == cad::PointClass::Boundary);
        std::printf("  classification: %-9s inside/outside/boundary all correct\n", c.name);
    }

    // A ray from well outside a convex solid must cross its boundary an
    // even number of times, and from inside an odd number -- the property
    // the parity argument rests on.
    {
        cad::Model model;
        cad::EntityId body = cad::kNoEntity;
        CHECK(cad::MakeBox(cad::Vec3d(0, 0, 0), cad::Vec3d(2, 2, 2), &model, &body));
        std::string error;
        CHECK(cad::BuildAllPCurves(&model, {}, &error));
        const std::vector<cad::RayHit> from_outside =
            cad::CastRay(model, body, cad::Vec3d(-5, 1.0, 1.0), cad::Vec3d(1, 0, 0));
        CHECK(from_outside.size() == 2);
        CHECK(from_outside[0].distance < from_outside[1].distance);
        // The first hit's outward normal opposes the ray; the second
        // agrees with it. That is what entering and leaving mean.
        CHECK(from_outside[0].normal.Dot(cad::Vec3d(1, 0, 0)) < 0.0);
        CHECK(from_outside[1].normal.Dot(cad::Vec3d(1, 0, 0)) > 0.0);
        const std::vector<cad::RayHit> from_inside =
            cad::CastRay(model, body, cad::Vec3d(1.0, 1.0, 1.0), cad::Vec3d(1, 0, 0));
        CHECK(from_inside.size() == 1);
    }

    // Randomised: for a sphere, the analytic answer is known for every
    // point, so classification can be checked exhaustively rather than at
    // a handful of places.
    {
        cad::Model model;
        cad::EntityId body = cad::kNoEntity;
        const double radius = 2.0;
        CHECK(cad::MakeSphere(cad::Vec3d(0.5, -0.25, 0.75), radius, &model, &body));
        std::string error;
        CHECK(cad::BuildAllPCurves(&model, {}, &error));
        std::mt19937 rng(424242);
        std::uniform_real_distribution<double> coordinate(-4.0, 4.5);
        int agreed = 0;
        int tested = 0;
        for (int trial = 0; trial < 120; ++trial) {
            const cad::Vec3d p{coordinate(rng), coordinate(rng), coordinate(rng)};
            const double distance = (p - cad::Vec3d(0.5, -0.25, 0.75)).Length();
            // Skip points near the surface: those are the Boundary case,
            // which the analytic comparison does not cover.
            if (std::fabs(distance - radius) < 0.05) continue;
            ++tested;
            const cad::PointClass classified = cad::ClassifyPoint(model, body, p);
            const cad::PointClass expected =
                (distance < radius) ? cad::PointClass::Inside : cad::PointClass::Outside;
            if (classified == expected) {
                ++agreed;
            } else {
                std::printf("    MISMATCH at (%.4f, %.4f, %.4f), |d-r| = %.4f, got %d want %d\n", p.x, p.y,
                            p.z, distance - radius, static_cast<int>(classified),
                            static_cast<int>(expected));
            }
        }
        std::printf("  classification: %d of %d random points against the analytic sphere\n", agreed, tested);
        CHECK(tested > 100);
        CHECK(agreed == tested);
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestCurveCurve();
    TestCurveSurface();
    TestAnalyticTable();
    TestPCurveOnSurface();
    TestMarcher();
    TestClassification();
    std::printf("cad_intersect_test passed (%d checks)\n", g_checks);
    return 0;
}
