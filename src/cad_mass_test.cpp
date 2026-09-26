// Windowless coverage for cad_mass.h (plans/CAD_FEM_PLAN.md Part A.6).
//
// Every check is against a closed-form value from elementary mechanics --
// a sphere's 2/5 m r^2, a cylinder's 1/2 m r^2 about its axis, a torus's
// 2 pi^2 R r^2 volume. That is the right oracle for this file: these are
// among the most thoroughly known quantities in physics, and agreement
// with them exercises the divergence-theorem integration, the surface
// derivatives underneath it, the quadrature, the parallel-axis shift and
// the face orientation convention all at once.
//
// Note what a *sign* error in orientation does: it negates the volume.
// So a positive volume of the right magnitude is itself evidence that
// every face's normal was oriented outward correctly.

#include "cad_mass.h"

#include "cad_math.h"
#include "cad_surface.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

bool NearRelative(double a, double b, double relative) {
    const double scale = std::max(std::fabs(a), std::fabs(b));
    if (scale < 1e-12) return std::fabs(a - b) < 1e-12;
    return std::fabs(a - b) / scale <= relative;
}
bool NearVec(const cad::Vec3d &a, const cad::Vec3d &b, double tol) { return (a - b).Length() <= tol; }

// A flat disc, as a cone of half-angle pi/2: S(u,v) = centre +
// v*(cos u * X + sin u * Y), so u is the angle and v the radius. Using
// the cone for this is not a trick -- a disc genuinely is a degenerate
// cone -- and it means the cap surfaces exercise the same code path the
// cone test does.
std::unique_ptr<cad::ConeSurface> MakeDisc(const cad::Vec3d &center, const cad::Vec3d &axis, double radius) {
    return std::make_unique<cad::ConeSurface>(center, cad::Vec3d(1, 0, 0).Cross(axis).LengthSquared() > 1e-9
                                                          ? cad::Vec3d(1, 0, 0)
                                                          : cad::Vec3d(0, 1, 0),
                                              axis, cad::kHalfPi, 0.0, cad::kTwoPi, 0.0, radius);
}

// Orients a face outward relative to a point known to be inside the
// solid, so the test never has to reason about each surface's own normal
// convention by hand.
cad::MassFace OrientOutward(const cad::Surface &surface, const cad::Vec3d &interior) {
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    surface.Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    const double u = 0.5 * (u_lo + u_hi);
    const double v = 0.5 * (v_lo + v_hi);
    const cad::Vec3d point = surface.Point(u, v);
    const cad::Vec3d normal = surface.Normal(u, v);
    cad::MassFace face;
    face.surface = &surface;
    face.reversed = normal.Dot(point - interior) < 0.0;
    return face;
}

void TestBox() {
    // A box from six planes, deliberately not centred at the origin so
    // the centroid and the parallel-axis shift are both exercised.
    const double a = 2.0, b = 3.0, c = 4.0;
    const cad::Vec3d min_corner(1.0, -2.0, 0.5);
    const cad::Vec3d center = min_corner + cad::Vec3d(a, b, c) * 0.5;

    std::vector<std::unique_ptr<cad::PlaneSurface>> owned;
    // Each face as a plane whose parameter rectangle is exactly the face.
    owned.push_back(std::make_unique<cad::PlaneSurface>(min_corner, cad::Vec3d(0, 1, 0), cad::Vec3d(0, 0, 1), 0.0,
                                                        b, 0.0, c));  // x = min
    owned.push_back(std::make_unique<cad::PlaneSurface>(min_corner + cad::Vec3d(a, 0, 0), cad::Vec3d(0, 1, 0),
                                                        cad::Vec3d(0, 0, 1), 0.0, b, 0.0, c));  // x = max
    owned.push_back(std::make_unique<cad::PlaneSurface>(min_corner, cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), 0.0,
                                                        a, 0.0, c));  // y = min
    owned.push_back(std::make_unique<cad::PlaneSurface>(min_corner + cad::Vec3d(0, b, 0), cad::Vec3d(1, 0, 0),
                                                        cad::Vec3d(0, 0, 1), 0.0, a, 0.0, c));  // y = max
    owned.push_back(std::make_unique<cad::PlaneSurface>(min_corner, cad::Vec3d(1, 0, 0), cad::Vec3d(0, 1, 0), 0.0,
                                                        a, 0.0, b));  // z = min
    owned.push_back(std::make_unique<cad::PlaneSurface>(min_corner + cad::Vec3d(0, 0, c), cad::Vec3d(1, 0, 0),
                                                        cad::Vec3d(0, 1, 0), 0.0, a, 0.0, b));  // z = max

    std::vector<cad::MassFace> faces;
    for (const std::unique_ptr<cad::PlaneSurface> &plane : owned) faces.push_back(OrientOutward(*plane, center));

    cad::MassProperties properties;
    cad::MassOptions options;
    options.density = 2.5;
    CHECK(cad::ComputeMassProperties(faces, &properties, options));

    const double expected_volume = a * b * c;
    std::printf("  box: volume %.12f (exact %.12f)\n", properties.volume, expected_volume);
    CHECK(NearRelative(properties.volume, expected_volume, 1e-12));
    CHECK(NearRelative(properties.area, 2.0 * (a * b + b * c + c * a), 1e-12));
    CHECK(NearVec(properties.centroid, center, 1e-11));
    CHECK(NearRelative(properties.mass, expected_volume * 2.5, 1e-12));

    // I_xx = m(b^2 + c^2)/12 for a box about its own centroid.
    const double mass = properties.mass;
    CHECK(NearRelative(properties.inertia_centroid.m[0][0], mass * (b * b + c * c) / 12.0, 1e-10));
    CHECK(NearRelative(properties.inertia_centroid.m[1][1], mass * (a * a + c * c) / 12.0, 1e-10));
    CHECK(NearRelative(properties.inertia_centroid.m[2][2], mass * (a * a + b * b) / 12.0, 1e-10));
    // A box aligned with the axes has no products of inertia about its
    // centroid -- but it does about the displaced origin, which is what
    // makes the parallel-axis shift worth checking.
    CHECK(std::fabs(properties.inertia_centroid.m[0][1]) < 1e-9 * mass);
    CHECK(std::fabs(properties.inertia_centroid.m[1][2]) < 1e-9 * mass);
    CHECK(std::fabs(properties.inertia_origin.m[0][1] + mass * center.x * center.y) < 1e-9 * mass);
}

void TestSphere() {
    const double radius = 1.7;
    const cad::Vec3d center(0.5, -1.0, 2.0);
    const cad::SphereSurface sphere(center, radius);
    std::vector<cad::MassFace> faces;
    faces.push_back(OrientOutward(sphere, center));

    cad::MassProperties properties;
    cad::MassOptions options;
    options.subdivisions = 12;
    CHECK(cad::ComputeMassProperties(faces, &properties, options));

    const double expected_volume = 4.0 / 3.0 * cad::kPi * radius * radius * radius;
    const double expected_area = 4.0 * cad::kPi * radius * radius;
    std::printf("  sphere: volume %.12f (exact %.12f), area %.12f (exact %.12f)\n", properties.volume,
                expected_volume, properties.area, expected_area);
    CHECK(NearRelative(properties.volume, expected_volume, 1e-10));
    CHECK(NearRelative(properties.area, expected_area, 1e-10));
    CHECK(NearVec(properties.centroid, center, 1e-9));
    // I = 2/5 m r^2 about every axis through the centre.
    const double expected_inertia = 0.4 * properties.mass * radius * radius;
    for (int i = 0; i < 3; ++i) {
        CHECK(NearRelative(properties.inertia_centroid.m[i][i], expected_inertia, 1e-9));
    }
    // Every principal moment is the same: a sphere is isotropic.
    double principal[3];
    properties.PrincipalMoments(principal);
    for (int i = 0; i < 3; ++i) CHECK(NearRelative(principal[i], expected_inertia, 1e-8));
}

void TestCylinder() {
    const double radius = 1.25;
    const double height = 3.5;
    const cad::Vec3d base(0, 0, 0);
    const cad::Vec3d axis(0, 0, 1);
    const cad::Vec3d center = base + axis * (height * 0.5);

    const cad::CylinderSurface tube(base, cad::Vec3d(1, 0, 0), axis, radius, 0.0, cad::kTwoPi, 0.0, height);
    const std::unique_ptr<cad::ConeSurface> bottom = MakeDisc(base, axis, radius);
    const std::unique_ptr<cad::ConeSurface> top = MakeDisc(base + axis * height, axis, radius);

    std::vector<cad::MassFace> faces;
    faces.push_back(OrientOutward(tube, center));
    faces.push_back(OrientOutward(*bottom, center));
    faces.push_back(OrientOutward(*top, center));

    cad::MassProperties properties;
    cad::MassOptions options;
    options.subdivisions = 10;
    CHECK(cad::ComputeMassProperties(faces, &properties, options));

    const double expected_volume = cad::kPi * radius * radius * height;
    const double expected_area = 2.0 * cad::kPi * radius * height + 2.0 * cad::kPi * radius * radius;
    std::printf("  cylinder: volume %.12f (exact %.12f), area %.12f (exact %.12f)\n", properties.volume,
                expected_volume, properties.area, expected_area);
    CHECK(NearRelative(properties.volume, expected_volume, 1e-10));
    CHECK(NearRelative(properties.area, expected_area, 1e-10));
    CHECK(NearVec(properties.centroid, center, 1e-9));
    const double mass = properties.mass;
    // I_zz = m r^2 / 2 about the axis; I_xx = m (3r^2 + h^2) / 12.
    CHECK(NearRelative(properties.inertia_centroid.m[2][2], 0.5 * mass * radius * radius, 1e-9));
    CHECK(NearRelative(properties.inertia_centroid.m[0][0],
                       mass * (3.0 * radius * radius + height * height) / 12.0, 1e-9));
    CHECK(NearRelative(properties.inertia_centroid.m[1][1],
                       mass * (3.0 * radius * radius + height * height) / 12.0, 1e-9));
}

void TestTorus() {
    const double major = 3.0;
    const double minor = 0.8;
    const cad::TorusSurface torus(cad::Vec3d(), cad::Vec3d(1, 0, 0), cad::Vec3d(0, 0, 1), major, minor);
    std::vector<cad::MassFace> faces;
    // A torus has no interior point on its axis, so orientation is set
    // from a point inside the tube instead.
    faces.push_back(OrientOutward(torus, cad::Vec3d(-major, 0, 0)));

    cad::MassProperties properties;
    cad::MassOptions options;
    options.subdivisions = 16;
    CHECK(cad::ComputeMassProperties(faces, &properties, options));

    const double expected_volume = 2.0 * cad::kPi * cad::kPi * major * minor * minor;
    const double expected_area = 4.0 * cad::kPi * cad::kPi * major * minor;
    std::printf("  torus: volume %.12f (exact %.12f), area %.12f (exact %.12f)\n", properties.volume,
                expected_volume, properties.area, expected_area);
    CHECK(NearRelative(properties.volume, expected_volume, 1e-9));
    CHECK(NearRelative(properties.area, expected_area, 1e-9));
    CHECK(NearVec(properties.centroid, cad::Vec3d(), 1e-8));
    const double mass = properties.mass;
    // I_zz = m(R^2 + 3r^2/4) about the symmetry axis, and
    // I_xx = I_yy = m(4R^2 + 5r^2)/8 about a diameter.
    CHECK(NearRelative(properties.inertia_centroid.m[2][2],
                       mass * (major * major + 0.75 * minor * minor), 1e-8));
    CHECK(NearRelative(properties.inertia_centroid.m[0][0],
                       mass * (4.0 * major * major + 5.0 * minor * minor) / 8.0, 1e-8));
}

// Orientation is load-bearing: reversing every face must negate the
// volume exactly, which is the property the boolean checks in Part C.5
// will depend on.
void TestOrientation() {
    const cad::SphereSurface sphere(cad::Vec3d(), 1.0);
    cad::MassFace outward;
    outward.surface = &sphere;
    outward.reversed = false;
    cad::MassFace inward = outward;
    inward.reversed = true;

    cad::MassProperties a;
    cad::MassProperties b;
    cad::MassOptions options;
    options.subdivisions = 10;
    CHECK(cad::ComputeMassProperties({outward}, &a, options));
    CHECK(cad::ComputeMassProperties({inward}, &b, options));
    CHECK(NearRelative(a.volume, -b.volume, 1e-12));
    CHECK(a.volume > 0.0);
    // Area is unsigned, so it is unaffected.
    CHECK(NearRelative(a.area, b.area, 1e-12));

    // A null face is rejected rather than silently skipped.
    cad::MassFace null_face;
    cad::MassProperties unused;
    CHECK(!cad::ComputeMassProperties({null_face}, &unused, options));
}

// Convergence: refining the quadrature must approach the exact value, and
// a planar solid must already be exact at the coarsest setting because
// its integrand is a polynomial the Gauss rule integrates exactly.
void TestConvergence() {
    const double radius = 1.0;
    const cad::SphereSurface sphere(cad::Vec3d(), radius);
    std::vector<cad::MassFace> faces;
    faces.push_back(cad::MassFace{&sphere, false});
    const double exact = 4.0 / 3.0 * cad::kPi;
    double previous_error = 1e30;
    for (int subdivisions : {1, 2, 4, 8}) {
        cad::MassOptions options;
        options.subdivisions = subdivisions;
        options.quadrature_order = 4;
        cad::MassProperties properties;
        CHECK(cad::ComputeMassProperties(faces, &properties, options));
        const double error = std::fabs(properties.volume - exact);
        std::printf("  sphere volume at %2d subdivisions: error %.3e\n", subdivisions, error);
        CHECK(error < previous_error);
        previous_error = error;
    }
    CHECK(previous_error < 1e-10);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestBox();
    TestSphere();
    TestCylinder();
    TestTorus();
    TestOrientation();
    TestConvergence();
    std::printf("cad_mass_test passed (%d checks)\n", g_checks);
    return 0;
}
