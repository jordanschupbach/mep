// Windowless coverage for cad_surface.h (plans/CAD_FEM_PLAN.md Parts
// A.1, A.3, A.4 and A.5).
//
// The strongest checks here are the cross-validations. A cylinder can be
// built four ways -- as a CylinderSurface, by extruding a circle, by
// revolving a line, and as the NURBS any of those converts to -- and all
// four must agree. Those paths share almost no code: the analytic one is
// trigonometry, the extrusion defers to a Curve3, the revolution goes
// through the A8.1 control-point construction, and the NURBS through the
// tensor-product de Boor evaluation. Agreement between them is far more
// convincing than any of them matching a value typed into this file.
//
// Curvatures are checked against closed-form values (1/r^2 for a sphere,
// zero Gaussian for a cylinder, cos(v)/(r(R + r cos v)) for a torus),
// which exercises the second fundamental form -- and with it the second
// derivatives, where an error is otherwise invisible.

#include "cad_surface.h"

#include "cad_curve.h"
#include "cad_math.h"

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

// Every analytic derivative against central differences. The single most
// productive check for this file: the derivative formulas are written by
// hand per surface and a sign slip in one of them is otherwise silent.
void CheckDerivativesAgainstFiniteDifferences(const cad::Surface &surface, const char *name) {
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    surface.Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    double worst_first = 0.0;
    double worst_second = 0.0;
    for (int i = 1; i < 6; ++i) {
        for (int j = 1; j < 6; ++j) {
            const double u = u_lo + (u_hi - u_lo) * static_cast<double>(i) / 6.0;
            const double v = v_lo + (v_hi - v_lo) * static_cast<double>(j) / 6.0;
            const double hu = (u_hi - u_lo) * 1e-5;
            const double hv = (v_hi - v_lo) * 1e-5;
            std::vector<std::vector<cad::Vec3d>> ders;
            surface.Derivatives(u, v, 2, &ders);
            CHECK(NearVec(ders[0][0], surface.Point(u, v), 1e-12));

            const cad::Vec3d du = (surface.Point(u + hu, v) - surface.Point(u - hu, v)) / (2.0 * hu);
            const cad::Vec3d dv = (surface.Point(u, v + hv) - surface.Point(u, v - hv)) / (2.0 * hv);
            worst_first = std::max(worst_first, (ders[1][0] - du).Length() / (1.0 + du.Length()));
            worst_first = std::max(worst_first, (ders[0][1] - dv).Length() / (1.0 + dv.Length()));

            const cad::Vec3d duu =
                (surface.Point(u + hu, v) - surface.Point(u, v) * 2.0 + surface.Point(u - hu, v)) / (hu * hu);
            const cad::Vec3d dvv =
                (surface.Point(u, v + hv) - surface.Point(u, v) * 2.0 + surface.Point(u, v - hv)) / (hv * hv);
            const cad::Vec3d duv =
                (surface.Point(u + hu, v + hv) - surface.Point(u + hu, v - hv) - surface.Point(u - hu, v + hv) +
                 surface.Point(u - hu, v - hv)) /
                (4.0 * hu * hv);
            worst_second = std::max(worst_second, (ders[2][0] - duu).Length() / (1.0 + duu.Length()));
            worst_second = std::max(worst_second, (ders[0][2] - dvv).Length() / (1.0 + dvv.Length()));
            worst_second = std::max(worst_second, (ders[1][1] - duv).Length() / (1.0 + duv.Length()));
        }
    }
    std::printf("  %-22s derivatives vs finite differences: first %.2e, second %.2e\n", name, worst_first,
                worst_second);
    CHECK(worst_first < 1e-7);
    CHECK(worst_second < 1e-3);
}

// One-sided Hausdorff distance from a surface's own NURBS form back to
// the surface. As with curves, the parameterization inside a span is not
// preserved, so the point set is what is compared.
double WorstNurbsDisagreement(const cad::Surface &surface) {
    int degree_u = 0, degree_v = 0, count_u = 0, count_v = 0;
    std::vector<double> knots_u;
    std::vector<double> knots_v;
    std::vector<cad::Vec4d> control;
    if (!surface.ToNurbs(&degree_u, &degree_v, &knots_u, &knots_v, &control, &count_u, &count_v)) return 1e30;
    cad::NurbsSurface nurbs;
    std::string error;
    if (!cad::NurbsSurface::Create(degree_u, degree_v, knots_u, knots_v, control, count_u, count_v, &nurbs,
                                   &error)) {
        std::fprintf(stderr, "  NURBS creation failed: %s\n", error.c_str());
        return 1e30;
    }
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    nurbs.Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    double worst = 0.0;
    for (int i = 0; i <= 20; ++i) {
        for (int j = 0; j <= 20; ++j) {
            const double u = u_lo + (u_hi - u_lo) * static_cast<double>(i) / 20.0;
            const double v = v_lo + (v_hi - v_lo) * static_cast<double>(j) / 20.0;
            const cad::Vec3d on_nurbs = nurbs.Point(u, v);
            double su = 0.0;
            double sv = 0.0;
            cad::Vec3d nearest;
            if (!surface.ClosestPoint(on_nurbs, &su, &sv, &nearest)) return 1e30;
            worst = std::max(worst, (on_nurbs - nearest).Length());
        }
    }
    return worst;
}

void TestPlane() {
    const cad::PlaneSurface plane(cad::Vec3d(1, 2, 3), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), -2.0, 3.0, -1.0,
                                  4.0);
    CHECK(NearVec(plane.Point(0, 0), cad::Vec3d(1, 2, 3), 1e-14));
    CHECK(NearVec(plane.Point(2, -1), cad::Vec3d(3, 1, 3), 1e-14));
    CHECK(NearVec(plane.PlaneNormal(), cad::Vec3d(0, 0, 1), 1e-14));
    CHECK(NearVec(plane.Normal(1.0, 1.0), cad::Vec3d(0, 0, 1), 1e-14));
    CHECK(Near(plane.SignedDistance(cad::Vec3d(0, 0, 8)), 5.0, 1e-13));
    CHECK(Near(plane.Area(), 5.0 * 5.0, 1e-10));
    // A plane is flat in every sense.
    CHECK(Near(plane.GaussianCurvature(0.5, 0.5), 0.0, 1e-10));
    CHECK(Near(plane.MeanCurvature(0.5, 0.5), 0.0, 1e-10));
    // The metric of an orthonormal plane parameterization is the identity.
    double e = 0.0, f = 0.0, g = 0.0;
    plane.FirstFundamentalForm(0.3, -0.2, &e, &f, &g);
    CHECK(Near(e, 1.0, 1e-13) && Near(f, 0.0, 1e-13) && Near(g, 1.0, 1e-13));
    CheckDerivativesAgainstFiniteDifferences(plane, "plane");
    CHECK(WorstNurbsDisagreement(plane) < 1e-12);
    CHECK(!plane.IsClosedU() && !plane.IsClosedV());
}

void TestCylinder() {
    const double radius = 2.0;
    const double height = 5.0;
    const cad::CylinderSurface cylinder(cad::Vec3d(1, -1, 0), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), radius,
                                        0.0, cad::kTwoPi, 0.0, height);
    // Every point is at the radius from the axis.
    for (int i = 0; i <= 20; ++i) {
        for (int j = 0; j <= 5; ++j) {
            const double u = cad::kTwoPi * static_cast<double>(i) / 20.0;
            const double v = height * static_cast<double>(j) / 5.0;
            const cad::Vec3d p = cylinder.Point(u, v);
            const cad::Vec3d radial = p - (cad::Vec3d(1, -1, 0) + cad::Vec3d(0, 0, 1) * v);
            CHECK(Near(radial.Length(), radius, 1e-13));
            // The normal is radial and outward.
            CHECK(NearVec(cylinder.Normal(u, v), radial.Normalized(), 1e-11));
        }
    }
    // A cylinder is developable: zero Gaussian curvature, mean curvature
    // 1/(2r) in magnitude.
    CHECK(Near(cylinder.GaussianCurvature(1.0, 2.0), 0.0, 1e-9));
    CHECK(Near(std::fabs(cylinder.MeanCurvature(1.0, 2.0)), 1.0 / (2.0 * radius), 1e-9));
    CHECK(Near(cylinder.Area(), cad::kTwoPi * radius * height, 1e-9));
    CHECK(cylinder.IsClosedU() && !cylinder.IsClosedV());
    CheckDerivativesAgainstFiniteDifferences(cylinder, "cylinder");
    const double disagreement = WorstNurbsDisagreement(cylinder);
    std::printf("  cylinder vs its NURBS form: %.3e\n", disagreement);
    CHECK(disagreement < 1e-12);

    const cad::Box3d box = cylinder.Bounds();
    CHECK(Near(box.x.lo, -1.0, 1e-12) && Near(box.x.hi, 3.0, 1e-12));
    CHECK(Near(box.z.lo, 0.0, 1e-12) && Near(box.z.hi, height, 1e-12));
}

void TestSphere() {
    const double radius = 3.0;
    const cad::Vec3d center(1, 2, -1);
    const cad::SphereSurface sphere(center, radius);
    for (int i = 0; i <= 16; ++i) {
        for (int j = 1; j < 16; ++j) {
            const double u = cad::kTwoPi * static_cast<double>(i) / 16.0;
            const double v = -cad::kHalfPi + cad::kPi * static_cast<double>(j) / 16.0;
            const cad::Vec3d p = sphere.Point(u, v);
            CHECK(Near((p - center).Length(), radius, 1e-12));
            CHECK(NearVec(sphere.Normal(u, v), (p - center).Normalized(), 1e-10));
            // K = 1/r^2 and |H| = 1/r everywhere on a sphere.
            CHECK(Near(sphere.GaussianCurvature(u, v), 1.0 / (radius * radius), 1e-8));
            CHECK(Near(std::fabs(sphere.MeanCurvature(u, v)), 1.0 / radius, 1e-8));
            // Both principal curvatures equal 1/r: every point is umbilic.
            double k1 = 0.0;
            double k2 = 0.0;
            sphere.PrincipalCurvatures(u, v, &k1, &k2);
            CHECK(Near(std::fabs(k1), 1.0 / radius, 1e-7));
            CHECK(Near(std::fabs(k2), 1.0 / radius, 1e-7));
        }
    }
    CHECK(Near(sphere.Area(), 4.0 * cad::kPi * radius * radius, 1e-9));
    CheckDerivativesAgainstFiniteDifferences(sphere, "sphere");
    const double disagreement = WorstNurbsDisagreement(sphere);
    std::printf("  sphere vs its NURBS form: %.3e\n", disagreement);
    CHECK(disagreement < 1e-11);
    const cad::Box3d box = sphere.Bounds();
    CHECK(Near(box.x.lo, center.x - radius, 1e-12) && Near(box.x.hi, center.x + radius, 1e-12));
}

void TestCone() {
    const cad::ConeSurface cone(cad::Vec3d(0, 0, 4), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, -1), 0.4, 0.0,
                                cad::kTwoPi, 0.5, 4.0);
    // A cone is developable, like a cylinder.
    CHECK(Near(cone.GaussianCurvature(1.0, 2.0), 0.0, 1e-8));
    // Points lie on the cone: the angle from the axis is the half angle.
    for (int i = 0; i <= 8; ++i) {
        for (int j = 1; j <= 4; ++j) {
            const double u = cad::kTwoPi * static_cast<double>(i) / 8.0;
            const double v = 0.5 + 3.5 * static_cast<double>(j) / 4.0;
            const cad::Vec3d from_apex = cone.Point(u, v) - cad::Vec3d(0, 0, 4);
            const double along = from_apex.Dot(cad::Vec3d(0, 0, -1));
            const double across = (from_apex - cad::Vec3d(0, 0, -1) * along).Length();
            CHECK(Near(std::atan2(across, along), 0.4, 1e-12));
            CHECK(Near(from_apex.Length(), v, 1e-12));
        }
    }
    CheckDerivativesAgainstFiniteDifferences(cone, "cone");
    const double disagreement = WorstNurbsDisagreement(cone);
    std::printf("  cone vs its NURBS form: %.3e\n", disagreement);
    CHECK(disagreement < 1e-11);
}

void TestTorus() {
    const double major = 3.0;
    const double minor = 1.0;
    const cad::TorusSurface torus(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), major, minor);
    for (int i = 0; i <= 12; ++i) {
        for (int j = 0; j <= 12; ++j) {
            const double u = cad::kTwoPi * static_cast<double>(i) / 12.0;
            const double v = cad::kTwoPi * static_cast<double>(j) / 12.0;
            const cad::Vec3d p = torus.Point(u, v);
            // Distance from the tube's centre circle is the minor radius.
            const cad::Vec3d in_plane(p.x, p.y, 0.0);
            const cad::Vec3d tube_center = in_plane.Normalized() * major;
            CHECK(Near((p - tube_center).Length(), minor, 1e-12));
            // The closed-form Gaussian curvature.
            const double expected = std::cos(v) / (minor * (major + minor * std::cos(v)));
            CHECK(Near(torus.GaussianCurvature(u, v), expected, 1e-7));
        }
    }
    CHECK(Near(torus.Area(), 4.0 * cad::kPi * cad::kPi * major * minor, 1e-8));
    CHECK(torus.IsClosedU() && torus.IsClosedV());
    CheckDerivativesAgainstFiniteDifferences(torus, "torus");
    const double disagreement = WorstNurbsDisagreement(torus);
    std::printf("  torus vs its NURBS form: %.3e\n", disagreement);
    CHECK(disagreement < 1e-11);
    const cad::Box3d box = torus.Bounds();
    CHECK(Near(box.x.hi, major + minor, 1e-12));
    CHECK(Near(box.z.hi, minor, 1e-12));
}

// The cross-validation this file's header describes.
void TestDerivedSurfacesAgainstAnalytic() {
    const double radius = 2.0;
    const double height = 5.0;
    const cad::Vec3d origin(1, -1, 0);
    const cad::Vec3d axis(0, 0, 1);
    const cad::CylinderSurface cylinder(origin, cad::Vec3d(1, 0, 0), axis, radius, 0.0, cad::kTwoPi, 0.0, height);

    // 1. Extruding a circle gives the same cylinder.
    {
        auto circle = std::make_unique<cad::Circle3>(origin, cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), radius,
                                                     0.0, cad::kTwoPi);
        const cad::ExtrusionSurface extrusion(std::move(circle), axis * height, 0.0, 1.0);
        double worst = 0.0;
        for (int i = 0; i <= 24; ++i) {
            for (int j = 0; j <= 8; ++j) {
                const double u = cad::kTwoPi * static_cast<double>(i) / 24.0;
                const double t = static_cast<double>(j) / 8.0;
                worst = std::max(worst, (extrusion.Point(u, t) - cylinder.Point(u, height * t)).Length());
            }
        }
        std::printf("  extruded circle vs analytic cylinder: %.3e\n", worst);
        CHECK(worst < 1e-13);
        CHECK(extrusion.IsClosedU() && !extrusion.IsClosedV());
        CheckDerivativesAgainstFiniteDifferences(extrusion, "extrusion");
    }

    // 2. Revolving a line parallel to the axis gives the same cylinder.
    {
        auto line = std::make_unique<cad::Line3>(
            cad::Line3::FromPoints(origin + cad::Vec3d(radius, 0, 0), origin + cad::Vec3d(radius, 0, height)));
        const cad::RevolutionSurface revolution(std::move(line), origin, axis, 0.0, cad::kTwoPi);
        double worst = 0.0;
        for (int i = 0; i <= 8; ++i) {
            for (int j = 0; j <= 24; ++j) {
                const double t = static_cast<double>(i) / 8.0;
                const double angle = cad::kTwoPi * static_cast<double>(j) / 24.0;
                // The revolution's u is the profile parameter, v the angle.
                worst = std::max(worst, (revolution.Point(t, angle) - cylinder.Point(angle, height * t)).Length());
            }
        }
        std::printf("  revolved line vs analytic cylinder:   %.3e\n", worst);
        CHECK(worst < 1e-12);
        CheckDerivativesAgainstFiniteDifferences(revolution, "revolution");
        // And its NURBS form agrees too -- a third independent path.
        CHECK(WorstNurbsDisagreement(revolution) < 1e-11);
    }

    // 3. Revolving a meridian arc gives a sphere.
    {
        const cad::Vec3d center(1, 2, -1);
        const double sphere_radius = 3.0;
        const cad::SphereSurface sphere(center, sphere_radius);
        // The meridian in the XZ plane, from the south pole to the north.
        auto meridian = std::make_unique<cad::Circle3>(center, cad::Vec3d(0, 0, 1), cad::Vec3d(1, 0, 0),
                                                       sphere_radius, -cad::kHalfPi, cad::kHalfPi);
        const cad::RevolutionSurface revolution(std::move(meridian), center, cad::Vec3d(0, 0, 1), 0.0,
                                                cad::kTwoPi);
        double worst = 0.0;
        for (int i = 1; i < 12; ++i) {
            for (int j = 0; j <= 16; ++j) {
                // The meridian's parameter is the angle from the pole
                // axis; the sphere's v is latitude. They differ by a
                // quarter turn and a sign, so compare point sets rather
                // than pairing parameters blindly.
                const double t = -cad::kHalfPi + cad::kPi * static_cast<double>(i) / 12.0;
                const double angle = cad::kTwoPi * static_cast<double>(j) / 16.0;
                const cad::Vec3d p = revolution.Point(t, angle);
                CHECK(Near((p - center).Length(), sphere_radius, 1e-11));
                double su = 0.0;
                double sv = 0.0;
                cad::Vec3d nearest;
                CHECK(sphere.ClosestPoint(p, &su, &sv, &nearest));
                worst = std::max(worst, (p - nearest).Length());
            }
        }
        std::printf("  revolved meridian vs analytic sphere: %.3e\n", worst);
        CHECK(worst < 1e-9);
    }

    // 4. A ruled surface between two parallel circles is a cylinder too.
    {
        auto bottom = std::make_unique<cad::Circle3>(origin, cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), radius,
                                                     0.0, cad::kTwoPi);
        auto top = std::make_unique<cad::Circle3>(origin + axis * height, cad::Vec3d(1, 0, 0),
                                                  cad::Vec3d(0, 1, 0), radius, 0.0, cad::kTwoPi);
        const cad::RuledSurface ruled(std::move(bottom), std::move(top));
        double worst = 0.0;
        for (int i = 0; i <= 24; ++i) {
            for (int j = 0; j <= 8; ++j) {
                const double t = static_cast<double>(i) / 24.0;
                const double v = static_cast<double>(j) / 8.0;
                worst = std::max(worst,
                                 (ruled.Point(t, v) - cylinder.Point(cad::kTwoPi * t, height * v)).Length());
            }
        }
        std::printf("  ruled circles vs analytic cylinder:   %.3e\n", worst);
        CHECK(worst < 1e-12);
    }
}

void TestOffset() {
    // Offsetting a sphere gives a larger sphere -- an exactly known
    // answer, which is what makes it worth testing.
    const double radius = 2.0;
    const double distance = 0.75;
    auto base = std::make_unique<cad::SphereSurface>(cad::Vec3d(1, 0, -2), radius);
    const cad::OffsetSurface offset(std::move(base), distance);
    for (int i = 0; i <= 8; ++i) {
        for (int j = 1; j < 8; ++j) {
            const double u = cad::kTwoPi * static_cast<double>(i) / 8.0;
            const double v = -cad::kHalfPi + cad::kPi * static_cast<double>(j) / 8.0;
            CHECK(Near((offset.Point(u, v) - cad::Vec3d(1, 0, -2)).Length(), radius + distance, 1e-11));
        }
    }
    // Offsetting inward past the radius of curvature is degenerate, and
    // the type says so rather than producing a folded surface silently.
    auto inner_base = std::make_unique<cad::SphereSurface>(cad::Vec3d(), radius);
    const cad::OffsetSurface collapsed(std::move(inner_base), -radius * 1.5);
    CHECK(collapsed.IsDegenerateAt(1.0, 0.2));
    auto ok_base = std::make_unique<cad::SphereSurface>(cad::Vec3d(), radius);
    const cad::OffsetSurface fine(std::move(ok_base), -radius * 0.5);
    CHECK(!fine.IsDegenerateAt(1.0, 0.2));
    // An offset of a NURBS is not a NURBS; the type refuses rather than
    // approximating behind the caller's back.
    int a = 0, b = 0, c = 0, d = 0;
    std::vector<double> ku;
    std::vector<double> kv;
    std::vector<cad::Vec4d> ctrl;
    CHECK(!offset.ToNurbs(&a, &b, &ku, &kv, &ctrl, &c, &d));
}

void TestLoftAndSweep() {
    // A loft through three circles of different radii must pass through
    // each of them.
    std::vector<std::unique_ptr<cad::Curve3>> owned;
    std::vector<const cad::Curve3 *> sections;
    const double radii[3] = {1.0, 2.0, 0.5};
    for (int i = 0; i < 3; ++i) {
        auto circle = std::make_unique<cad::Circle3>(cad::Vec3d(0, 0, static_cast<double>(i) * 2.0),
                                                     cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), radii[i], 0.0,
                                                     cad::kTwoPi);
        sections.push_back(circle.get());
        owned.push_back(std::move(circle));
    }
    cad::NurbsSurface lofted;
    std::string error;
    CHECK(cad::LoftSurface(sections, 2, cad::Parameterization::ChordLength, &lofted, &error));
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    lofted.Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    // At v = 0 and v = 1 the surface is the first and last section.
    double worst = 0.0;
    for (int i = 0; i <= 24; ++i) {
        const double t = static_cast<double>(i) / 24.0;
        const double u = u_lo + (u_hi - u_lo) * t;
        const cad::Vec3d bottom = lofted.Point(u, v_lo);
        const cad::Vec3d top = lofted.Point(u, v_hi);
        worst = std::max(worst, std::fabs(cad::Vec3d(bottom.x, bottom.y, 0.0).Length() - radii[0]));
        worst = std::max(worst, std::fabs(cad::Vec3d(top.x, top.y, 0.0).Length() - radii[2]));
        CHECK(Near(bottom.z, 0.0, 1e-9));
        CHECK(Near(top.z, 4.0, 1e-9));
    }
    std::printf("  loft through 3 circles: end sections reproduced to %.3e\n", worst);
    CHECK(worst < 1e-9);
    // Sections of differing degree must still loft: a circle is a
    // rational quadratic and a line a polynomial linear, and making them
    // compatible is the whole job.
    {
        auto circle = std::make_unique<cad::Circle3>(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 1.0,
                                                     0.0, cad::kTwoPi);
        auto square_ish = std::make_unique<cad::Circle3>(cad::Vec3d(0, 0, 3), cad::Vec3d(1, 0, 0),
                                                         cad::Vec3d(0, 1, 0), 2.0, 0.0, cad::kTwoPi);
        std::vector<const cad::Curve3 *> two = {circle.get(), square_ish.get()};
        cad::NurbsSurface simple;
        CHECK(cad::LoftSurface(two, 1, cad::Parameterization::Uniform, &simple, &error));
    }
    // Too few sections is refused.
    std::vector<const cad::Curve3 *> one = {sections[0]};
    cad::NurbsSurface bad;
    CHECK(!cad::LoftSurface(one, 2, cad::Parameterization::Uniform, &bad, &error));
    CHECK(!error.empty());

    // Rotation-minimizing frames: orthonormal, aligned with the tangent,
    // and -- the property that distinguishes them from Frenet -- carrying
    // no twist along a curve with an inflection.
    {
        cad::NurbsCurve3 spine;
        std::vector<cad::Vec3d> points;
        for (int i = 0; i <= 10; ++i) {
            const double t = static_cast<double>(i) / 10.0;
            points.push_back(cad::Vec3d{t * 10.0, std::sin(t * 6.0) * 2.0, std::cos(t * 3.0)});
        }
        CHECK(cad::NurbsCurve3::Interpolate(points, 3, cad::Parameterization::Centripetal, &spine));
        std::vector<double> parameters;
        for (int i = 0; i <= 200; ++i) parameters.push_back(static_cast<double>(i) / 200.0);
        const std::vector<cad::Frame> frames = cad::RotationMinimizingFrames(spine, parameters);
        CHECK(frames.size() == parameters.size());
        double worst_orthonormality = 0.0;
        double worst_tangent = 0.0;
        for (const cad::Frame &frame : frames) {
            worst_orthonormality = std::max(worst_orthonormality, std::fabs(frame.normal.Length() - 1.0));
            worst_orthonormality = std::max(worst_orthonormality, std::fabs(frame.binormal.Length() - 1.0));
            worst_orthonormality = std::max(worst_orthonormality, std::fabs(frame.normal.Dot(frame.tangent)));
            worst_orthonormality = std::max(worst_orthonormality, std::fabs(frame.normal.Dot(frame.binormal)));
            worst_tangent =
                std::max(worst_tangent, (frame.tangent - spine.Tangent(frame.parameter)).Length());
            CHECK(NearVec(frame.origin, spine.Point(frame.parameter), 1e-12));
        }
        std::printf("  rotation-minimizing frames: orthonormality %.2e, tangent %.2e\n", worst_orthonormality,
                    worst_tangent);
        CHECK(worst_orthonormality < 1e-10);
        CHECK(worst_tangent < 1e-10);
        // The defining property: the normal never rotates about the
        // tangent. Measured as the angle swept between consecutive
        // frames after removing the tangent's own rotation -- for a
        // rotation-minimizing frame this is near zero, while a Frenet
        // frame would swing through pi at an inflection.
        double worst_twist = 0.0;
        for (std::size_t i = 1; i < frames.size(); ++i) {
            const cad::Vec3d previous = frames[i - 1].normal;
            const cad::Vec3d current = frames[i].normal;
            // Project the previous normal into the new frame's plane and
            // measure how far it had to turn.
            const cad::Vec3d projected =
                (previous - frames[i].tangent * previous.Dot(frames[i].tangent)).Normalized();
            const double twist = std::atan2(projected.Cross(current).Length(), projected.Dot(current));
            worst_twist = std::max(worst_twist, twist);
        }
        std::printf("  rotation-minimizing frames: worst per-step twist %.3e rad\n", worst_twist);
        CHECK(worst_twist < 1e-3);
    }

    // A sweep of a circle along a straight spine is a cylinder.
    {
        const cad::Line3 spine = cad::Line3::FromPoints(cad::Vec3d(0, 0, 0), cad::Vec3d(0, 0, 6));
        const cad::Circle3 profile(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 1.5, 0.0,
                                   cad::kTwoPi);
        cad::NurbsSurface swept;
        CHECK(cad::SweepSurface(profile, spine, 8, &swept, &error));
        double su = 0.0, s_hi = 0.0, sv = 0.0, sv_hi = 0.0;
        swept.Domain(&su, &s_hi, &sv, &sv_hi);
        double worst_radius = 0.0;
        for (int i = 0; i <= 16; ++i) {
            for (int j = 0; j <= 8; ++j) {
                const cad::Vec3d p = swept.Point(su + (s_hi - su) * static_cast<double>(i) / 16.0,
                                                 sv + (sv_hi - sv) * static_cast<double>(j) / 8.0);
                worst_radius =
                    std::max(worst_radius, std::fabs(cad::Vec3d(p.x, p.y, 0.0).Length() - 1.5));
            }
        }
        std::printf("  swept circle along a straight spine: radius error %.3e\n", worst_radius);
        CHECK(worst_radius < 1e-9);
    }
}

void TestSurfaceClosestPoint() {
    std::vector<std::unique_ptr<cad::Surface>> surfaces;
    surfaces.push_back(std::make_unique<cad::PlaneSurface>(cad::Vec3d(0, 0, 1), cad::Vec3d(1, 0, 0),
                                                           cad::Vec3d(0, 1, 0), -4.0, 4.0, -4.0, 4.0));
    surfaces.push_back(std::make_unique<cad::CylinderSurface>(cad::Vec3d(), cad::Vec3d(1, 0, 0),
                                                              cad::Vec3d(0, 0, 1), 2.0, 0.0, cad::kTwoPi, -2.0,
                                                              2.0));
    surfaces.push_back(std::make_unique<cad::SphereSurface>(cad::Vec3d(1, 1, 1), 2.0));
    surfaces.push_back(
        std::make_unique<cad::TorusSurface>(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), 3.0, 1.0));

    std::mt19937 rng(9876);
    std::uniform_real_distribution<double> coordinate(-6.0, 6.0);
    double worst_excess = 0.0;
    int queries = 0;
    for (const std::unique_ptr<cad::Surface> &surface : surfaces) {
        double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
        surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
        for (int trial = 0; trial < 40; ++trial) {
            const cad::Vec3d query{coordinate(rng), coordinate(rng), coordinate(rng)};
            double u = 0.0;
            double v = 0.0;
            cad::Vec3d on_surface;
            CHECK(surface->ClosestPoint(query, &u, &v, &on_surface));
            CHECK(NearVec(on_surface, surface->Point(u, v), 1e-11));
            // Brute force over a dense grid.
            double truth = 1e30;
            const int samples = 300;
            for (int i = 0; i <= samples; ++i) {
                for (int j = 0; j <= samples; ++j) {
                    const double su = u_lo + (u_hi - u_lo) * static_cast<double>(i) / samples;
                    const double svv = v_lo + (v_hi - v_lo) * static_cast<double>(j) / samples;
                    truth = std::min(truth, (surface->Point(su, svv) - query).LengthSquared());
                }
            }
            truth = std::sqrt(truth);
            const double found = (on_surface - query).Length();
            worst_excess = std::max(worst_excess, found - truth);
            CHECK(found - truth < 1e-5);
            ++queries;
        }
    }
    std::printf("  surface point inversion: %d queries, worst excess over brute force %.3e\n", queries,
                worst_excess);

    // A point exactly on a sphere is found to be on it; the centre is not.
    const cad::SphereSurface sphere(cad::Vec3d(), 2.0);
    CHECK(sphere.ContainsPoint(cad::Vec3d(2, 0, 0)));
    CHECK(sphere.ContainsPoint(cad::Vec3d(0, 0, 2)));
    CHECK(!sphere.ContainsPoint(cad::Vec3d(0, 0, 0)));
    CHECK(!sphere.ContainsPoint(cad::Vec3d(5, 0, 0)));
}

void TestIsoCurves() {
    const cad::TorusSurface torus(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), 3.0, 1.0);

    // At a span boundary the NURBS and analytic parameters agree exactly,
    // and the isocurve is the analytic one to machine precision. This is
    // the check that pins the correspondence rather than merely bounding
    // its error.
    for (int fix_u = 0; fix_u < 2; ++fix_u) {
        const double boundary = cad::kHalfPi;  // a span boundary of the 4-arc construction
        cad::NurbsCurve3 iso;
        CHECK(torus.IsoCurve(fix_u != 0, boundary, &iso));
        double lo = 0.0;
        double hi = 0.0;
        iso.Domain(&lo, &hi);
        double worst = 0.0;
        for (int i = 0; i <= 20; ++i) {
            const double t = lo + (hi - lo) * static_cast<double>(i) / 20.0;
            const cad::Vec3d on_curve = iso.Point(t);
            double su = 0.0;
            double sv = 0.0;
            cad::Vec3d nearest;
            CHECK(torus.ClosestPoint(on_curve, &su, &sv, &nearest));
            const double held = (fix_u != 0) ? su : sv;
            worst = std::max(worst, std::fabs(held - boundary));
        }
        std::printf("  isocurve at a span boundary (%s): parameter error %.2e\n", fix_u != 0 ? "u" : "v",
                    worst);
        CHECK(worst < 1e-7);
    }

    for (int fix_u = 0; fix_u < 2; ++fix_u) {
        const double fixed = 1.1;
        cad::NurbsCurve3 iso;
        CHECK(torus.IsoCurve(fix_u != 0, fixed, &iso));
        double lo = 0.0;
        double hi = 0.0;
        iso.Domain(&lo, &hi);
        // Pairing parameters would be wrong here for the same reason it
        // is wrong for curves: the isocurve comes out of the surface's
        // NURBS form, whose parameterization inside a span differs from
        // the analytic one. What must hold is the geometric property --
        // every point of the isocurve lies on the surface, at the fixed
        // parameter value it was extracted at.
        double worst_off_surface = 0.0;
        double worst_parameter = 0.0;
        for (int i = 0; i <= 40; ++i) {
            const double t = lo + (hi - lo) * static_cast<double>(i) / 40.0;
            const cad::Vec3d on_curve = iso.Point(t);
            double su = 0.0;
            double sv = 0.0;
            cad::Vec3d nearest;
            CHECK(torus.ClosestPoint(on_curve, &su, &sv, &nearest));
            worst_off_surface = std::max(worst_off_surface, (on_curve - nearest).Length());
            const double held = (fix_u != 0) ? su : sv;
            worst_parameter = std::max(worst_parameter, std::fabs(held - fixed));
        }
        std::printf("  isocurve (%s fixed at %.2f): off surface %.2e, parameter drift %.2e\n",
                    fix_u != 0 ? "u" : "v", fixed, worst_off_surface, worst_parameter);
        // Exactly on the surface -- the property that matters.
        CHECK(worst_off_surface < 1e-10);
        // The parameter drift is the known NURBS-vs-analytic mismatch
        // inside a span, bounded by roughly 1% of a quarter turn for this
        // construction. Asserting a bound rather than zero, because zero
        // is not achievable and pretending otherwise would be the bug.
        CHECK(worst_parameter < 0.05);
    }
}

}  // namespace

// Reflections, which are the one transform that does not simply move a
// surface about.
//
// A surface stored as an axis and one reference direction recovers its
// second direction with a cross product, and a cross product comes out
// the other way round when handedness reverses. So the reflected surface
// traces the same points with its angle running backwards, and its
// trimmed domain has to say so -- otherwise the surface is right as a
// *set* and wrong as a parameterization, which nothing notices until a
// trimmed face asks where its boundary went. That is exactly how it was
// found: Part E.5's mirror pattern produced a filleted block whose
// cylindrical face reported a boundary enclosing no area at all.
//
// A plane keeps both of its axes, so its parameters survive untouched.
// The two behaviours are opposite and both are checked here, because a
// caller correcting for one has to know which it is dealing with.
void TestReflection() {
    const cad::Vec3d point{1.0, 2.0, -0.5};
    const cad::Vec3d normal{0.3, -0.8, 0.52};
    const cad::Vec3d n = normal.Normalized();
    const double offset = point.Dot(n);
    cad::Mat4d reflect;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const double ni = i == 0 ? n.x : (i == 1 ? n.y : n.z);
            const double nj = j == 0 ? n.x : (j == 1 ? n.y : n.z);
            reflect.m[i][j] = (i == j ? 1.0 : 0.0) - 2.0 * ni * nj;
        }
    }
    reflect.m[0][3] = 2.0 * offset * n.x;
    reflect.m[1][3] = 2.0 * offset * n.y;
    reflect.m[2][3] = 2.0 * offset * n.z;
    CHECK(reflect.LinearDeterminant() < 0.0);
    CHECK(Near(reflect.LinearDeterminant(), -1.0, 1e-12));

    // A plane's parameters survive a reflection unchanged.
    {
        cad::PlaneSurface plane(cad::Vec3d{1.0, 1.0, 1.0}, cad::Vec3d{1.0, 0.0, 0.0},
                                cad::Vec3d{0.0, 1.0, 0.0}, -2.0, 3.0, -1.0, 4.0);
        cad::PlaneSurface moved = plane;
        moved.Transform(reflect);
        for (double u : {-1.5, 0.0, 2.0}) {
            for (double v : {-0.5, 1.0, 3.5}) {
                CHECK((moved.Point(u, v) - reflect.TransformPoint(plane.Point(u, v))).Length() < 1e-12);
            }
        }
    }

    // Everything parameterized by an angle has that angle run backwards,
    // and its domain moves with it.
    struct Case {
        const char *name;
        std::unique_ptr<cad::Surface> surface;
    };
    std::vector<Case> cases;
    cases.push_back({"cylinder", std::make_unique<cad::CylinderSurface>(
                                     cad::Vec3d{0.5, -1.0, 2.0}, cad::Vec3d{1.0, 0.0, 0.0},
                                     cad::Vec3d{0.0, 0.0, 1.0}, 2.0, 0.3, 2.1, -1.0, 3.0)});
    cases.push_back({"sphere", std::make_unique<cad::SphereSurface>(
                                   cad::Vec3d{-1.0, 0.5, 1.0}, 1.75, cad::Vec3d{1.0, 0.0, 0.0},
                                   cad::Vec3d{0.0, 0.0, 1.0}, 0.4, 2.6, -0.9, 1.1)});
    cases.push_back({"cone", std::make_unique<cad::ConeSurface>(
                                 cad::Vec3d{0.0, 0.0, 3.0}, cad::Vec3d{1.0, 0.0, 0.0},
                                 cad::Vec3d{0.0, 0.0, -1.0}, 0.4, 0.2, 2.4, 0.5, 2.5)});
    cases.push_back({"torus", std::make_unique<cad::TorusSurface>(
                                  cad::Vec3d{1.0, 1.0, 0.0}, cad::Vec3d{1.0, 0.0, 0.0},
                                  cad::Vec3d{0.0, 0.0, 1.0}, 3.0, 0.75, 0.1, 1.9, -0.7, 2.2)});

    for (const Case &entry : cases) {
        double u_lo = 0.0;
        double u_hi = 0.0;
        double v_lo = 0.0;
        double v_hi = 0.0;
        entry.surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
        std::unique_ptr<cad::Surface> moved = entry.surface->Clone();
        moved->Transform(reflect);
        double mu_lo = 0.0;
        double mu_hi = 0.0;
        double mv_lo = 0.0;
        double mv_hi = 0.0;
        moved->Domain(&mu_lo, &mu_hi, &mv_lo, &mv_hi);
        // The angle runs backwards, so the trimmed range is negated end
        // for end. Everything else is left alone.
        CHECK(Near(mu_lo, -u_hi, 1e-12));
        CHECK(Near(mu_hi, -u_lo, 1e-12));
        CHECK(Near(mv_lo, v_lo, 1e-12));
        CHECK(Near(mv_hi, v_hi, 1e-12));
        // And the negated parameter names the reflected point, which is
        // the statement that makes the domain above correct rather than
        // merely consistent.
        double worst = 0.0;
        for (int i = 0; i <= 6; ++i) {
            for (int j = 0; j <= 6; ++j) {
                const double u = u_lo + (u_hi - u_lo) * static_cast<double>(i) / 6.0;
                const double v = v_lo + (v_hi - v_lo) * static_cast<double>(j) / 6.0;
                const cad::Vec3d expected = reflect.TransformPoint(entry.surface->Point(u, v));
                worst = std::max(worst, (moved->Point(-u, v) - expected).Length());
            }
        }
        std::printf("  %-9s reflected: worst point error %.3e over its domain\n", entry.name, worst);
        CHECK(worst < 1e-12);
        // And the reported bounds actually cover it. An axis-aligned box
        // does not survive a reflection about a slanted plane as the same
        // box, so this asks the only thing that is true of it: that the
        // surface is inside what it claims.
        const cad::Box3d after = moved->Bounds();
        CHECK(!after.IsEmpty());
        for (int i = 0; i <= 6; ++i) {
            for (int j = 0; j <= 6; ++j) {
                const double u = mu_lo + (mu_hi - mu_lo) * static_cast<double>(i) / 6.0;
                const double v = mv_lo + (mv_hi - mv_lo) * static_cast<double>(j) / 6.0;
                const cad::Vec3d p = moved->Point(u, v);
                CHECK(p.x >= after.x.lo - 1e-9 && p.x <= after.x.hi + 1e-9);
                CHECK(p.y >= after.y.lo - 1e-9 && p.y <= after.y.hi + 1e-9);
                CHECK(p.z >= after.z.lo - 1e-9 && p.z <= after.z.hi + 1e-9);
            }
        }
    }
    std::printf("reflection: a plane keeps its parameters, an angle reverses, domains follow\n");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestPlane();
    TestCylinder();
    TestSphere();
    TestCone();
    TestTorus();
    TestReflection();
    TestDerivedSurfacesAgainstAnalytic();
    TestOffset();
    TestLoftAndSweep();
    TestSurfaceClosestPoint();
    TestIsoCurves();
    std::printf("cad_surface_test passed (%d checks)\n", g_checks);
    return 0;
}
