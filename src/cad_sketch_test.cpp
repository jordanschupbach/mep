// Windowless coverage for cad_sketch.h and cad_constraint.h
// (plans/CAD_FEM_PLAN.md Parts D.1 to D.4).
//
// Three oracles, each answering a different question:
//
//   THE JACOBIAN is checked against central finite differences of the
//   residual, for every constraint kind, on configurations chosen to be
//   generic rather than convenient. The derivatives are produced by dual
//   arithmetic, so this is not checking one formula against another --
//   there is only one formula. What it checks is that the residual means
//   what the constraint claims and that every partial lands in the right
//   column, which is exactly where a hand-written Jacobian goes wrong and
//   where an automatic one still can.
//
//   THE SOLUTIONS are checked against closed-form geometry. A triangle
//   with three given sides has angles the law of cosines fixes; a line
//   tangent to a circle is at the radius from its centre; two externally
//   tangent circles have their centres exactly the sum of the radii
//   apart. None of these can be satisfied by a solver that converged to
//   the wrong thing.
//
//   THE DIAGNOSIS is checked by construction. A rectangle with two
//   dimensions is fully constrained and one with one dimension has
//   exactly one degree of freedom; the free direction that reports is
//   then verified to be a real one, by moving along it and finding every
//   residual unchanged to first order. Redundant and conflicting sketches
//   are built to be redundant and conflicting, and the discrimination
//   between them is the point.

#include "cad_sketch.h"

#include "cad_constraint.h"
#include "cad_math.h"
#include "cad_topology.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace cad;

int g_checks = 0;

void Check(bool condition, const char *expression, int line) {
    ++g_checks;
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

Vec2d PointAt(const Sketch &sketch, SketchId id) {
    const SketchPoint *point = sketch.GetPoint(id);
    Check(point != nullptr, "point exists", __LINE__);
    return point->position;
}

// --- The Jacobian oracle ----------------------------------------------

// Worst absolute disagreement between the analytic Jacobian and a central
// difference of the residual, over every entry.
double JacobianError(const Sketch &sketch) {
    const SketchParameters parameters(sketch);
    std::vector<double> base;
    MatrixNd jacobian;
    std::vector<SketchId> owners;
    std::string error;
    Check(EvaluateConstraints(sketch, parameters, &base, &jacobian, &owners, &error), "evaluate", __LINE__);
    if (base.empty() || parameters.Count() == 0) return 0.0;

    std::vector<double> x;
    parameters.Gather(sketch, &x);
    double worst = 0.0;
    for (int column = 0; column < parameters.Count(); ++column) {
        const double step = 1e-6 * std::max(1.0, std::fabs(x[static_cast<std::size_t>(column)]));
        Sketch forward = sketch;
        Sketch backward = sketch;
        std::vector<double> xp = x;
        std::vector<double> xm = x;
        xp[static_cast<std::size_t>(column)] += step;
        xm[static_cast<std::size_t>(column)] -= step;
        parameters.Scatter(xp, &forward);
        parameters.Scatter(xm, &backward);
        std::vector<double> rp;
        std::vector<double> rm;
        Check(EvaluateConstraints(forward, parameters, &rp, nullptr, nullptr, &error), "evaluate+", __LINE__);
        Check(EvaluateConstraints(backward, parameters, &rm, nullptr, nullptr, &error), "evaluate-", __LINE__);
        Check(rp.size() == base.size() && rm.size() == base.size(), "residual count stable", __LINE__);
        for (std::size_t row = 0; row < base.size(); ++row) {
            const double difference = (rp[row] - rm[row]) / (2.0 * step);
            worst = std::max(worst, std::fabs(difference - jacobian(row, static_cast<std::size_t>(column))));
        }
    }
    return worst;
}

// Every constraint kind, on a configuration with nothing special about
// it: no axis alignment, no equal lengths, no right angles, nothing at
// the origin. A Jacobian bug that only shows up off the special cases is
// the usual kind.
void TestJacobians() {
    std::printf("jacobians\n");
    std::mt19937 rng(20260924);
    std::uniform_real_distribution<double> jitter(-0.35, 0.35);
    auto shake = [&](Vec2d p) { return Vec2d{p.x + jitter(rng), p.y + jitter(rng)}; };

    struct Case {
        const char *name;
        double worst = 0.0;
    };
    std::vector<Case> cases;

    auto run = [&](const char *name, const std::function<void(Sketch *)> &build) {
        // Several randomised configurations per kind, so a derivative
        // that happens to be right at one point does not pass.
        double worst = 0.0;
        for (int trial = 0; trial < 5; ++trial) {
            Sketch sketch;
            build(&sketch);
            worst = std::max(worst, JacobianError(sketch));
        }
        cases.push_back(Case{name, worst});
        Check(worst < 1e-6, name, __LINE__);
    };

    run("coincident", [&](Sketch *s) {
        const SketchId a = s->AddPoint(shake(Vec2d{1.3, 2.7}));
        const SketchId b = s->AddPoint(shake(Vec2d{-0.6, 1.1}));
        s->Constrain(ConstraintKind::Coincident, {a, b}, {});
    });
    run("horizontal (points)", [&](Sketch *s) {
        const SketchId a = s->AddPoint(shake(Vec2d{1.3, 2.7}));
        const SketchId b = s->AddPoint(shake(Vec2d{-0.6, 1.1}));
        s->Constrain(ConstraintKind::Horizontal, {a, b}, {});
    });
    run("vertical (line)", [&](Sketch *s) {
        const SketchId line = s->AddLine(shake(Vec2d{1.3, 2.7}), shake(Vec2d{-0.6, 1.1}));
        s->Constrain(ConstraintKind::Vertical, {}, {line});
    });
    run("parallel", [&](Sketch *s) {
        const SketchId l1 = s->AddLine(shake(Vec2d{0.4, 0.2}), shake(Vec2d{2.9, 1.3}));
        const SketchId l2 = s->AddLine(shake(Vec2d{0.1, 2.2}), shake(Vec2d{3.3, 3.9}));
        s->Constrain(ConstraintKind::Parallel, {}, {l1, l2});
    });
    run("perpendicular", [&](Sketch *s) {
        const SketchId l1 = s->AddLine(shake(Vec2d{0.4, 0.2}), shake(Vec2d{2.9, 1.3}));
        const SketchId l2 = s->AddLine(shake(Vec2d{0.1, 2.2}), shake(Vec2d{3.3, 3.9}));
        s->Constrain(ConstraintKind::Perpendicular, {}, {l1, l2});
    });
    run("equal (lines)", [&](Sketch *s) {
        const SketchId l1 = s->AddLine(shake(Vec2d{0.4, 0.2}), shake(Vec2d{2.9, 1.3}));
        const SketchId l2 = s->AddLine(shake(Vec2d{0.1, 2.2}), shake(Vec2d{3.3, 3.9}));
        s->Constrain(ConstraintKind::Equal, {}, {l1, l2});
    });
    run("equal (circle and arc)", [&](Sketch *s) {
        const SketchId circle = s->AddCircle(shake(Vec2d{1.1, 0.7}), 1.37);
        const SketchId arc = s->AddArc(shake(Vec2d{4.2, 2.1}), shake(Vec2d{5.4, 2.6}), shake(Vec2d{3.9, 3.3}));
        s->Constrain(ConstraintKind::Equal, {}, {circle, arc});
    });
    run("concentric", [&](Sketch *s) {
        const SketchId c1 = s->AddCircle(shake(Vec2d{1.1, 0.7}), 1.37);
        const SketchId c2 = s->AddCircle(shake(Vec2d{2.3, 1.9}), 0.61);
        s->Constrain(ConstraintKind::Concentric, {}, {c1, c2});
    });
    run("collinear", [&](Sketch *s) {
        const SketchId l1 = s->AddLine(shake(Vec2d{0.4, 0.2}), shake(Vec2d{2.9, 1.3}));
        const SketchId l2 = s->AddLine(shake(Vec2d{3.4, 1.9}), shake(Vec2d{5.1, 2.7}));
        s->Constrain(ConstraintKind::Collinear, {}, {l1, l2});
    });
    run("symmetric", [&](Sketch *s) {
        const SketchId axis = s->AddLine(shake(Vec2d{0.2, -1.1}), shake(Vec2d{0.6, 3.4}));
        const SketchId a = s->AddPoint(shake(Vec2d{-1.4, 1.2}));
        const SketchId b = s->AddPoint(shake(Vec2d{2.1, 0.9}));
        s->Constrain(ConstraintKind::Symmetric, {a, b}, {axis});
    });
    run("point on line", [&](Sketch *s) {
        const SketchId line = s->AddLine(shake(Vec2d{0.4, 0.2}), shake(Vec2d{2.9, 1.3}));
        const SketchId p = s->AddPoint(shake(Vec2d{1.8, 1.9}));
        s->Constrain(ConstraintKind::PointOnObject, {p}, {line});
    });
    run("point on circle", [&](Sketch *s) {
        const SketchId circle = s->AddCircle(shake(Vec2d{1.1, 0.7}), 1.37);
        const SketchId p = s->AddPoint(shake(Vec2d{2.9, 1.6}));
        s->Constrain(ConstraintKind::PointOnObject, {p}, {circle});
    });
    run("point on arc", [&](Sketch *s) {
        const SketchId arc = s->AddArc(shake(Vec2d{4.2, 2.1}), shake(Vec2d{5.4, 2.6}), shake(Vec2d{3.9, 3.3}));
        const SketchId p = s->AddPoint(shake(Vec2d{5.9, 3.4}));
        s->Constrain(ConstraintKind::PointOnObject, {p}, {arc});
    });
    run("point on ellipse", [&](Sketch *s) {
        const SketchId ellipse = s->AddEllipse(shake(Vec2d{1.1, 0.7}), 2.3, 1.1, 0.37);
        const SketchId p = s->AddPoint(shake(Vec2d{3.1, 1.6}));
        s->Constrain(ConstraintKind::PointOnObject, {p}, {ellipse});
    });
    run("tangent (line and circle)", [&](Sketch *s) {
        const SketchId line = s->AddLine(shake(Vec2d{-1.2, 0.3}), shake(Vec2d{3.4, 1.1}));
        const SketchId circle = s->AddCircle(shake(Vec2d{1.1, 2.7}), 1.37);
        s->Constrain(ConstraintKind::Tangent, {}, {line, circle});
    });
    run("tangent (line and arc)", [&](Sketch *s) {
        const SketchId arc = s->AddArc(shake(Vec2d{4.2, 2.1}), shake(Vec2d{5.4, 2.6}), shake(Vec2d{3.9, 3.3}));
        const SketchId line = s->AddLine(shake(Vec2d{-1.2, 0.3}), shake(Vec2d{3.4, 1.1}));
        s->Constrain(ConstraintKind::Tangent, {}, {line, arc});
    });
    run("tangent (circles, external)", [&](Sketch *s) {
        const SketchId c1 = s->AddCircle(shake(Vec2d{0.0, 0.0}), 1.37);
        const SketchId c2 = s->AddCircle(shake(Vec2d{3.7, 1.1}), 0.83);
        s->Constrain(ConstraintKind::Tangent, {}, {c1, c2}, 1.0);
    });
    run("tangent (circles, internal)", [&](Sketch *s) {
        const SketchId c1 = s->AddCircle(shake(Vec2d{0.0, 0.0}), 3.37);
        const SketchId c2 = s->AddCircle(shake(Vec2d{1.2, 0.4}), 0.83);
        s->Constrain(ConstraintKind::Tangent, {}, {c1, c2}, -1.0);
    });
    run("distance", [&](Sketch *s) {
        const SketchId a = s->AddPoint(shake(Vec2d{1.3, 2.7}));
        const SketchId b = s->AddPoint(shake(Vec2d{-0.6, 1.1}));
        s->Constrain(ConstraintKind::Distance, {a, b}, {}, 2.5);
    });
    run("horizontal distance", [&](Sketch *s) {
        const SketchId a = s->AddPoint(shake(Vec2d{1.3, 2.7}));
        const SketchId b = s->AddPoint(shake(Vec2d{-0.6, 1.1}));
        s->Constrain(ConstraintKind::HorizontalDistance, {a, b}, {}, 2.5);
    });
    run("vertical distance", [&](Sketch *s) {
        const SketchId a = s->AddPoint(shake(Vec2d{1.3, 2.7}));
        const SketchId b = s->AddPoint(shake(Vec2d{-0.6, 1.1}));
        s->Constrain(ConstraintKind::VerticalDistance, {a, b}, {}, 2.5);
    });
    run("angle", [&](Sketch *s) {
        const SketchId l1 = s->AddLine(shake(Vec2d{0.4, 0.2}), shake(Vec2d{2.9, 1.3}));
        const SketchId l2 = s->AddLine(shake(Vec2d{0.1, 2.2}), shake(Vec2d{3.3, 3.9}));
        s->Constrain(ConstraintKind::Angle, {}, {l1, l2}, 0.6);
    });
    run("radius", [&](Sketch *s) {
        const SketchId circle = s->AddCircle(shake(Vec2d{1.1, 0.7}), 1.37);
        s->Constrain(ConstraintKind::Radius, {}, {circle}, 2.0);
    });
    run("diameter (arc)", [&](Sketch *s) {
        const SketchId arc = s->AddArc(shake(Vec2d{4.2, 2.1}), shake(Vec2d{5.4, 2.6}), shake(Vec2d{3.9, 3.3}));
        s->Constrain(ConstraintKind::Diameter, {}, {arc}, 3.0);
    });
    run("arc radius (internal)", [&](Sketch *s) {
        // Created by AddArc; nothing else to add.
        s->AddArc(shake(Vec2d{4.2, 2.1}), shake(Vec2d{5.4, 2.6}), shake(Vec2d{3.9, 3.3}));
    });

    double worst = 0.0;
    for (const Case &entry : cases) worst = std::max(worst, entry.worst);
    std::printf("  %zu constraint kinds, worst analytic-vs-difference disagreement %.3e\n", cases.size(),
                worst);
    CHECK(worst < 1e-6);
}

// --- D.1 geometry ------------------------------------------------------

void TestGeometry() {
    std::printf("geometry\n");
    Sketch sketch;

    // A line, and its curve.
    {
        const SketchId line = sketch.AddLine(Vec2d{1.0, 2.0}, Vec2d{4.0, 6.0});
        const std::shared_ptr<const Curve3> curve = sketch.Curve(line);
        CHECK(curve != nullptr);
        CHECK(Near(curve->Length(), 5.0, 1e-12));
    }

    // A circle: its curve closes, and its length is the circumference.
    {
        const SketchId circle = sketch.AddCircle(Vec2d{2.0, -1.0}, 1.5);
        const std::shared_ptr<const Curve3> curve = sketch.Curve(circle);
        CHECK(curve != nullptr);
        CHECK(Near(curve->Length(), kTwoPi * 1.5, 1e-9));
        double radius = 0.0;
        CHECK(sketch.Radius(circle, &radius));
        CHECK(Near(radius, 1.5, 1e-15));
    }

    // An arc: both endpoints land on it, at the radius from the centre,
    // and it runs the way it was told to. AddArc puts the endpoints on a
    // common circle itself, which is what the radius check confirms.
    {
        const Vec2d centre{1.0, 1.0};
        const SketchId ccw = sketch.AddArc(centre, Vec2d{3.0, 1.0}, Vec2d{1.0, 2.6}, true);
        double radius = 0.0;
        CHECK(sketch.Radius(ccw, &radius));
        CHECK(Near(radius, 2.0, 1e-12));
        const SketchEntity *entity = sketch.GetEntity(ccw);
        CHECK(entity != nullptr);
        CHECK(Near((PointAt(sketch, entity->points[2]) - centre).Length(), 2.0, 1e-12));

        const std::shared_ptr<const Curve3> curve = sketch.Curve(ccw);
        CHECK(curve != nullptr);
        double lo = 0.0;
        double hi = 0.0;
        curve->Domain(&lo, &hi);
        const Vec3d start = curve->Point(lo);
        const Vec3d end = curve->Point(hi);
        CHECK(Near(start.x, 3.0, 1e-9));
        CHECK(Near(start.y, 1.0, 1e-9));
        CHECK(Near(end.x, PointAt(sketch, entity->points[2]).x, 1e-9));
        CHECK(Near(end.y, PointAt(sketch, entity->points[2]).y, 1e-9));
        // A quarter turn, near enough: the end point is at 90 degrees.
        CHECK(Near(curve->Length(), 2.0 * kHalfPi, 1e-6));

        // The same three points the other way round trace the rest of
        // the circle, so the two arc lengths add to the whole.
        const SketchId cw = sketch.AddArc(centre, Vec2d{3.0, 1.0}, Vec2d{1.0, 3.0}, false);
        const std::shared_ptr<const Curve3> other = sketch.Curve(cw);
        CHECK(other != nullptr);
        CHECK(Near(curve->Length() + other->Length(), kTwoPi * 2.0, 1e-6));
    }

    // An ellipse: its axes come out where they were put.
    {
        const SketchId ellipse = sketch.AddEllipse(Vec2d{0.0, 0.0}, 3.0, 1.0, 0.0);
        const std::shared_ptr<const Curve3> curve = sketch.Curve(ellipse);
        CHECK(curve != nullptr);
        CHECK(Near(curve->Point(0.0).x, 3.0, 1e-12));
        CHECK(Near(curve->Point(kHalfPi).y, 1.0, 1e-12));
    }

    // A spline interpolates its first and last control points and stays
    // inside their convex hull, which for a monotone set of points means
    // staying inside their bounding box.
    {
        const std::vector<Vec2d> control{{0.0, 0.0}, {1.0, 2.0}, {3.0, 2.5}, {4.0, 0.5}};
        const SketchId spline = sketch.AddSpline(control, 3);
        const std::shared_ptr<const Curve3> curve = sketch.Curve(spline);
        CHECK(curve != nullptr);
        double lo = 0.0;
        double hi = 0.0;
        curve->Domain(&lo, &hi);
        CHECK(Near(curve->Point(lo).x, 0.0, 1e-12));
        CHECK(Near(curve->Point(lo).y, 0.0, 1e-12));
        CHECK(Near(curve->Point(hi).x, 4.0, 1e-12));
        CHECK(Near(curve->Point(hi).y, 0.5, 1e-12));
        for (int i = 0; i <= 32; ++i) {
            const Vec3d p = curve->Point(lo + (hi - lo) * static_cast<double>(i) / 32.0);
            CHECK(p.x >= -1e-9 && p.x <= 4.0 + 1e-9);
            CHECK(p.y >= -1e-9 && p.y <= 2.5 + 1e-9);
        }
    }

    // World curves agree with plane curves mapped through the plane.
    {
        SketchPlane plane = PlaneFromNormal(Vec3d{1.0, 2.0, 3.0}, Vec3d{1.0, 1.0, 1.0});
        Sketch tilted(plane);
        const SketchId line = tilted.AddLine(Vec2d{1.0, 2.0}, Vec2d{4.0, 6.0});
        const SketchId circle = tilted.AddCircle(Vec2d{-1.0, 0.5}, 2.0);
        const SketchId arc = tilted.AddArc(Vec2d{0.0, 0.0}, Vec2d{2.0, 0.0}, Vec2d{0.0, 2.0});
        const SketchId ellipse = tilted.AddEllipse(Vec2d{1.0, 1.0}, 2.0, 1.0, 0.4);
        const SketchId spline = tilted.AddSpline({{0.0, 0.0}, {1.0, 2.0}, {3.0, 1.0}}, 2);
        double worst = 0.0;
        for (SketchId id : {line, circle, arc, ellipse, spline}) {
            const std::shared_ptr<const Curve3> flat = tilted.Curve(id);
            const std::shared_ptr<const Curve3> world = tilted.WorldCurve(id);
            CHECK(flat != nullptr && world != nullptr);
            double lo = 0.0;
            double hi = 0.0;
            flat->Domain(&lo, &hi);
            double wlo = 0.0;
            double whi = 0.0;
            world->Domain(&wlo, &whi);
            for (int i = 0; i <= 24; ++i) {
                const double f = static_cast<double>(i) / 24.0;
                const Vec3d a = flat->Point(lo + f * (hi - lo));
                const Vec3d b = world->Point(wlo + f * (whi - wlo));
                worst = std::max(worst, (plane.ToWorld(Vec2d{a.x, a.y}) - b).Length());
            }
        }
        std::printf("  world curves agree with the plane mapping to %.3e\n", worst);
        CHECK(worst < 1e-9);
        // And the plane's own round trip is exact.
        const Vec2d probe{1.7, -2.3};
        CHECK((plane.ToPlane(plane.ToWorld(probe)) - probe).Length() < 1e-12);
    }
}

// A sketch plane attached to a model face follows it through a rebuild
// that changes every dimension -- which is the whole reason the
// attachment goes through Part B.4's naming rather than a face index.
void TestPlaneAttachment() {
    std::printf("plane attachment\n");
    Model box;
    EntityId body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 3.0, 4.0}, &box, &body));

    // The top face: the one whose outward normal points along +z.
    EntityId top = kNoEntity;
    std::string role;
    for (const Face &face : box.Faces()) {
        const Surface *surface = box.SurfaceAt(face.surface);
        if (surface == nullptr) continue;
        double u0 = 0.0;
        double u1 = 0.0;
        double v0 = 0.0;
        double v1 = 0.0;
        surface->Domain(&u0, &u1, &v0, &v1);
        Vec3d normal = surface->Normal(0.5 * (u0 + u1), 0.5 * (v0 + v1));
        if (face.orientation == Orientation::Reversed) normal = -normal;
        if (normal.z > 0.9) {
            top = face.id;
            // The role a face plays is the name the primitive gave it;
            // Part B.4 scores candidates partly on that, so making one up
            // here would be testing a reference no real caller makes.
            role = face.name;
        }
    }
    CHECK(top != kNoEntity);

    SketchPlane plane;
    CHECK(AttachPlaneToFace(box, top, 1, role, 0.0, &plane));
    CHECK(plane.attached);
    CHECK(Near(plane.origin.z, 4.0, 1e-12));
    CHECK(Near(plane.Normal().z, 1.0, 1e-12));

    // The same box, taller and wider. Every dimension differs, so nothing
    // but the name can find the face again.
    Model rebuilt;
    EntityId rebuilt_body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{5.0, 7.0, 9.0}, &rebuilt, &rebuilt_body));
    CHECK(ReattachPlane(rebuilt, &plane, {}) == ResolveStatus::Resolved);
    CHECK(Near(plane.origin.z, 9.0, 1e-12));
    CHECK(Near(plane.Normal().z, 1.0, 1e-12));

    // An offset plane stands off the face by exactly the offset.
    EntityId rebuilt_top = kNoEntity;
    for (const Face &face : rebuilt.Faces()) {
        if (face.name == role) rebuilt_top = face.id;
    }
    CHECK(rebuilt_top != kNoEntity);
    SketchPlane offset;
    CHECK(AttachPlaneToFace(rebuilt, rebuilt_top, 1, role, 2.5, &offset));
    CHECK(Near(offset.origin.z, 11.5, 1e-12));

    // The frame is stable: attaching twice gives the same axes, so a
    // sketch's own coordinates do not get renumbered by a rebuild.
    SketchPlane again;
    CHECK(AttachPlaneToFace(rebuilt, rebuilt_top, 1, role, 0.0, &again));
    CHECK((again.x_axis - plane.x_axis).Length() < 1e-15);
    CHECK((again.y_axis - plane.y_axis).Length() < 1e-15);
}

// --- D.2 solving, against closed forms ---------------------------------

void TestSolveTriangle() {
    std::printf("solve: triangle by three sides\n");
    // Three sides fix a triangle up to placement, and the law of cosines
    // fixes its angles. The sketch is anchored by fixing one vertex and
    // making one side horizontal, so the remaining freedom is exactly the
    // triangle's shape -- which the three lengths then determine.
    Sketch sketch;
    const SketchId a = sketch.Origin();
    const SketchId b = sketch.AddPoint(Vec2d{3.3, 0.4});
    const SketchId c = sketch.AddPoint(Vec2d{1.1, 2.9});
    const SketchId ab = sketch.AddLineFromPoints(a, b);
    sketch.AddLineFromPoints(b, c);
    sketch.AddLineFromPoints(c, a);
    sketch.Constrain(ConstraintKind::Horizontal, {}, {ab});
    const double side_ab = 5.0;
    const double side_bc = 4.0;
    const double side_ca = 3.0;
    sketch.Constrain(ConstraintKind::Distance, {a, b}, {}, side_ab);
    sketch.Constrain(ConstraintKind::Distance, {b, c}, {}, side_bc);
    sketch.Constrain(ConstraintKind::Distance, {c, a}, {}, side_ca);

    SketchDiagnosis diagnosis;
    CHECK(SolveSketch(&sketch, &diagnosis, {}));
    CHECK(diagnosis.status == SketchStatus::Solved);
    CHECK(diagnosis.degrees_of_freedom == 0);

    const Vec2d pa = PointAt(sketch, a);
    const Vec2d pb = PointAt(sketch, b);
    const Vec2d pc = PointAt(sketch, c);
    CHECK(Near((pb - pa).Length(), side_ab, 1e-9));
    CHECK(Near((pc - pb).Length(), side_bc, 1e-9));
    CHECK(Near((pa - pc).Length(), side_ca, 1e-9));
    // 3-4-5: the angle at C is a right angle, by the law of cosines.
    const double cos_c = ((pa - pc).Dot(pb - pc)) / (side_ca * side_bc);
    std::printf("  3-4-5 triangle: cos of the angle opposite the long side is %.3e (exactly 0)\n", cos_c);
    CHECK(std::fabs(cos_c) < 1e-9);
}

void TestSolveRectangle() {
    std::printf("solve: rectangle\n");
    // Four lines joined corner to corner, two told to be horizontal and
    // two vertical, with a width and a height. That is fully constrained
    // once one corner is anchored, and every corner's position is then
    // known exactly.
    Sketch sketch;
    const SketchId p0 = sketch.Origin();
    const SketchId p1 = sketch.AddPoint(Vec2d{3.1, 0.2});
    const SketchId p2 = sketch.AddPoint(Vec2d{2.8, 1.9});
    const SketchId p3 = sketch.AddPoint(Vec2d{0.3, 2.2});
    const SketchId bottom = sketch.AddLineFromPoints(p0, p1);
    const SketchId right = sketch.AddLineFromPoints(p1, p2);
    const SketchId top = sketch.AddLineFromPoints(p2, p3);
    const SketchId left = sketch.AddLineFromPoints(p3, p0);
    sketch.Constrain(ConstraintKind::Horizontal, {}, {bottom});
    sketch.Constrain(ConstraintKind::Horizontal, {}, {top});
    sketch.Constrain(ConstraintKind::Vertical, {}, {right});
    sketch.Constrain(ConstraintKind::Vertical, {}, {left});
    sketch.Constrain(ConstraintKind::HorizontalDistance, {p0, p1}, {}, 4.0);
    sketch.Constrain(ConstraintKind::VerticalDistance, {p1, p2}, {}, 2.5);

    SketchDiagnosis diagnosis;
    CHECK(SolveSketch(&sketch, &diagnosis, {}));
    CHECK(diagnosis.status == SketchStatus::Solved);
    CHECK(diagnosis.degrees_of_freedom == 0);
    CHECK((PointAt(sketch, p0) - Vec2d{0.0, 0.0}).Length() < 1e-9);
    CHECK((PointAt(sketch, p1) - Vec2d{4.0, 0.0}).Length() < 1e-9);
    CHECK((PointAt(sketch, p2) - Vec2d{4.0, 2.5}).Length() < 1e-9);
    CHECK((PointAt(sketch, p3) - Vec2d{0.0, 2.5}).Length() < 1e-9);
}

void TestSolveTangency() {
    std::printf("solve: tangency\n");
    // A line through two fixed-ish points, tangent to a circle whose
    // centre is anchored: the radius must come out as the distance from
    // that centre to the line.
    {
        Sketch sketch;
        const SketchId a = sketch.AddPoint(Vec2d{-3.0, 0.0}, true);
        const SketchId b = sketch.AddPoint(Vec2d{3.0, 2.0}, true);
        const SketchId line = sketch.AddLineFromPoints(a, b);
        const SketchId circle = sketch.AddCircle(Vec2d{0.0, 4.0}, 0.5);
        sketch.SetPointFixed(sketch.GetEntity(circle)->points[0], true);
        sketch.Constrain(ConstraintKind::Tangent, {}, {line, circle});

        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        const Vec2d pa = PointAt(sketch, a);
        const Vec2d pb = PointAt(sketch, b);
        const Vec2d d = pb - pa;
        const Vec2d centre{0.0, 4.0};
        const double expected = std::fabs(d.Cross(centre - pa)) / d.Length();
        double radius = 0.0;
        CHECK(sketch.Radius(circle, &radius));
        std::printf("  line/circle: radius %.9f, distance from centre %.9f\n", radius, expected);
        CHECK(Near(radius, expected, 1e-9));
    }

    // Two circles tangent from the outside: their centres end up exactly
    // the sum of the radii apart.
    {
        Sketch sketch;
        const SketchId c1 = sketch.AddCircle(Vec2d{0.0, 0.0}, 2.0);
        const SketchId c2 = sketch.AddCircle(Vec2d{6.0, 1.0}, 1.0);
        sketch.SetPointFixed(sketch.GetEntity(c1)->points[0], true);
        sketch.Constrain(ConstraintKind::Radius, {}, {c1}, 2.0);
        sketch.Constrain(ConstraintKind::Radius, {}, {c2}, 1.0);
        sketch.Constrain(ConstraintKind::Tangent, {}, {c1, c2}, 1.0);
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        const Vec2d centre1 = PointAt(sketch, sketch.GetEntity(c1)->points[0]);
        const Vec2d centre2 = PointAt(sketch, sketch.GetEntity(c2)->points[0]);
        CHECK(Near((centre2 - centre1).Length(), 3.0, 1e-9));
    }

    // And from the inside: the distance is the difference of the radii.
    {
        Sketch sketch;
        const SketchId c1 = sketch.AddCircle(Vec2d{0.0, 0.0}, 4.0);
        const SketchId c2 = sketch.AddCircle(Vec2d{1.0, 0.5}, 1.0);
        sketch.SetPointFixed(sketch.GetEntity(c1)->points[0], true);
        sketch.Constrain(ConstraintKind::Radius, {}, {c1}, 4.0);
        sketch.Constrain(ConstraintKind::Radius, {}, {c2}, 1.0);
        sketch.Constrain(ConstraintKind::Tangent, {}, {c1, c2}, -1.0);
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        const Vec2d centre2 = PointAt(sketch, sketch.GetEntity(c2)->points[0]);
        CHECK(Near(centre2.Length(), 3.0, 1e-9));
    }
}

void TestSolveAssortedConstraints() {
    std::printf("solve: the rest of the kinds\n");

    // Angle, parallel and perpendicular, each checked by measuring the
    // angle the solve produced.
    {
        Sketch sketch;
        const SketchId l1 = sketch.AddLine(Vec2d{0.0, 0.0}, Vec2d{2.0, 0.3});
        const SketchId l2 = sketch.AddLine(Vec2d{0.0, 1.0}, Vec2d{1.5, 2.2});
        sketch.SetPointFixed(sketch.GetEntity(l1)->points[0], true);
        sketch.SetPointFixed(sketch.GetEntity(l1)->points[1], true);
        const double wanted = 0.7;
        sketch.Constrain(ConstraintKind::Angle, {}, {l1, l2}, wanted);
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        const SketchEntity *e1 = sketch.GetEntity(l1);
        const SketchEntity *e2 = sketch.GetEntity(l2);
        const Vec2d d1 = PointAt(sketch, e1->points[1]) - PointAt(sketch, e1->points[0]);
        const Vec2d d2 = PointAt(sketch, e2->points[1]) - PointAt(sketch, e2->points[0]);
        CHECK(Near(std::atan2(d1.Cross(d2), d1.Dot(d2)), wanted, 1e-9));
    }
    {
        Sketch sketch;
        const SketchId l1 = sketch.AddLine(Vec2d{0.0, 0.0}, Vec2d{2.0, 0.3});
        const SketchId l2 = sketch.AddLine(Vec2d{0.0, 1.0}, Vec2d{1.5, 2.2});
        sketch.SetPointFixed(sketch.GetEntity(l1)->points[0], true);
        sketch.SetPointFixed(sketch.GetEntity(l1)->points[1], true);
        sketch.Constrain(ConstraintKind::Perpendicular, {}, {l1, l2});
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        const SketchEntity *e1 = sketch.GetEntity(l1);
        const SketchEntity *e2 = sketch.GetEntity(l2);
        const Vec2d d1 = PointAt(sketch, e1->points[1]) - PointAt(sketch, e1->points[0]);
        const Vec2d d2 = PointAt(sketch, e2->points[1]) - PointAt(sketch, e2->points[0]);
        CHECK(std::fabs(d1.Dot(d2)) / (d1.Length() * d2.Length()) < 1e-9);
    }
    {
        Sketch sketch;
        const SketchId l1 = sketch.AddLine(Vec2d{0.0, 0.0}, Vec2d{2.0, 0.3});
        const SketchId l2 = sketch.AddLine(Vec2d{0.0, 1.0}, Vec2d{1.5, 2.2});
        sketch.SetPointFixed(sketch.GetEntity(l1)->points[0], true);
        sketch.SetPointFixed(sketch.GetEntity(l1)->points[1], true);
        sketch.Constrain(ConstraintKind::Parallel, {}, {l1, l2});
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        const SketchEntity *e1 = sketch.GetEntity(l1);
        const SketchEntity *e2 = sketch.GetEntity(l2);
        const Vec2d d1 = PointAt(sketch, e1->points[1]) - PointAt(sketch, e1->points[0]);
        const Vec2d d2 = PointAt(sketch, e2->points[1]) - PointAt(sketch, e2->points[0]);
        CHECK(std::fabs(d1.Cross(d2)) / (d1.Length() * d2.Length()) < 1e-9);
    }

    // Symmetry about a line: the two points end up mirror images.
    {
        Sketch sketch;
        const SketchId s = sketch.AddPoint(Vec2d{0.0, 0.0}, true);
        const SketchId e = sketch.AddPoint(Vec2d{0.0, 3.0}, true);
        const SketchId axis = sketch.AddLineFromPoints(s, e);
        const SketchId a = sketch.AddPoint(Vec2d{-2.0, 1.0}, true);
        const SketchId b = sketch.AddPoint(Vec2d{1.4, 1.9});
        sketch.Constrain(ConstraintKind::Symmetric, {a, b}, {axis});
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        const Vec2d pb = PointAt(sketch, b);
        // Mirror of (-2, 1) across the y axis.
        CHECK(Near(pb.x, 2.0, 1e-9));
        CHECK(Near(pb.y, 1.0, 1e-9));
    }

    // Equal, concentric, collinear and point-on-object.
    {
        Sketch sketch;
        const SketchId l1 = sketch.AddLine(Vec2d{0.0, 0.0}, Vec2d{3.0, 0.0});
        const SketchId l2 = sketch.AddLine(Vec2d{0.0, 2.0}, Vec2d{1.0, 2.0});
        sketch.SetPointFixed(sketch.GetEntity(l1)->points[0], true);
        sketch.SetPointFixed(sketch.GetEntity(l1)->points[1], true);
        sketch.SetPointFixed(sketch.GetEntity(l2)->points[0], true);
        sketch.Constrain(ConstraintKind::Equal, {}, {l1, l2});
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        const SketchEntity *e2 = sketch.GetEntity(l2);
        CHECK(Near((PointAt(sketch, e2->points[1]) - PointAt(sketch, e2->points[0])).Length(), 3.0, 1e-9));
    }
    {
        Sketch sketch;
        const SketchId c1 = sketch.AddCircle(Vec2d{1.0, 2.0}, 2.0);
        const SketchId c2 = sketch.AddCircle(Vec2d{4.0, -1.0}, 1.0);
        sketch.SetPointFixed(sketch.GetEntity(c1)->points[0], true);
        sketch.Constrain(ConstraintKind::Concentric, {}, {c1, c2});
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        CHECK((PointAt(sketch, sketch.GetEntity(c2)->points[0]) - Vec2d{1.0, 2.0}).Length() < 1e-9);
    }
    {
        Sketch sketch;
        const SketchId l1 = sketch.AddLine(Vec2d{0.0, 0.0}, Vec2d{4.0, 2.0});
        const SketchId l2 = sketch.AddLine(Vec2d{5.0, 1.0}, Vec2d{7.0, 5.0});
        sketch.SetPointFixed(sketch.GetEntity(l1)->points[0], true);
        sketch.SetPointFixed(sketch.GetEntity(l1)->points[1], true);
        sketch.Constrain(ConstraintKind::Collinear, {}, {l1, l2});
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        const SketchEntity *e2 = sketch.GetEntity(l2);
        const Vec2d d{4.0, 2.0};
        for (int k = 0; k < 2; ++k) {
            const Vec2d p = PointAt(sketch, e2->points[static_cast<std::size_t>(k)]);
            CHECK(std::fabs(d.Cross(p)) / d.Length() < 1e-9);
        }
    }
    {
        Sketch sketch;
        const SketchId circle = sketch.AddCircle(Vec2d{0.0, 0.0}, 2.0);
        sketch.SetPointFixed(sketch.GetEntity(circle)->points[0], true);
        sketch.Constrain(ConstraintKind::Radius, {}, {circle}, 2.0);
        const SketchId p = sketch.AddPoint(Vec2d{5.0, 0.3});
        sketch.Constrain(ConstraintKind::PointOnObject, {p}, {circle});
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        CHECK(Near(PointAt(sketch, p).Length(), 2.0, 1e-9));
    }
    {
        // Point on an ellipse, checked against the implicit equation.
        Sketch sketch;
        const SketchId ellipse = sketch.AddEllipse(Vec2d{0.0, 0.0}, 3.0, 1.5, 0.0);
        sketch.SetPointFixed(sketch.GetEntity(ellipse)->points[0], true);
        // The ellipse's own shape is held, or the solver will satisfy
        // "this point is on the ellipse" by moving the ellipse to the
        // point, which is a correct answer to a question nobody meant.
        for (int k = 0; k < 3; ++k) sketch.SetScalarFixed(ellipse, k, true);
        const SketchId p = sketch.AddPoint(Vec2d{4.0, 0.9});
        sketch.Constrain(ConstraintKind::PointOnObject, {p}, {ellipse});
        sketch.Constrain(ConstraintKind::VerticalDistance, {sketch.Origin(), p}, {}, 0.9);
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        // Checked against the curve rather than against the equation
        // written out here: the ellipse's axes and rotation are
        // parameters too, so the solver is entitled to have changed them,
        // and an axis-aligned formula would be testing an assumption
        // rather than the constraint.
        const Vec2d pp = PointAt(sketch, p);
        const std::shared_ptr<const Curve3> curve = sketch.Curve(ellipse);
        CHECK(curve != nullptr);
        double t = 0.0;
        Vec3d closest;
        CHECK(curve->ClosestPoint(Vec3d{pp.x, pp.y, 0.0}, &t, &closest));
        CHECK((closest - Vec3d{pp.x, pp.y, 0.0}).Length() < 1e-8);
    }
}

// --- D.3 diagnosis and dragging ----------------------------------------

// Builds a rectangle anchored at the origin with `dimensions` of its two
// dimensional constraints applied, so the tests below can ask for a
// sketch with a known number of degrees of freedom.
Sketch BuildRectangle(int dimensions, std::vector<SketchId> *corners) {
    Sketch sketch;
    const SketchId p0 = sketch.Origin();
    const SketchId p1 = sketch.AddPoint(Vec2d{3.1, 0.2});
    const SketchId p2 = sketch.AddPoint(Vec2d{2.8, 1.9});
    const SketchId p3 = sketch.AddPoint(Vec2d{0.3, 2.2});
    const SketchId bottom = sketch.AddLineFromPoints(p0, p1);
    const SketchId right = sketch.AddLineFromPoints(p1, p2);
    const SketchId top = sketch.AddLineFromPoints(p2, p3);
    const SketchId left = sketch.AddLineFromPoints(p3, p0);
    sketch.Constrain(ConstraintKind::Horizontal, {}, {bottom});
    sketch.Constrain(ConstraintKind::Horizontal, {}, {top});
    sketch.Constrain(ConstraintKind::Vertical, {}, {right});
    sketch.Constrain(ConstraintKind::Vertical, {}, {left});
    if (dimensions >= 1) sketch.Constrain(ConstraintKind::HorizontalDistance, {p0, p1}, {}, 4.0);
    if (dimensions >= 2) sketch.Constrain(ConstraintKind::VerticalDistance, {p1, p2}, {}, 2.5);
    if (corners != nullptr) *corners = {p0, p1, p2, p3};
    return sketch;
}

void TestDiagnosis() {
    std::printf("diagnosis\n");

    // Fully constrained.
    {
        Sketch sketch = BuildRectangle(2, nullptr);
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        CHECK(diagnosis.status == SketchStatus::Solved);
        CHECK(diagnosis.degrees_of_freedom == 0);
        CHECK(diagnosis.free_directions.empty());
        CHECK(diagnosis.redundant.empty());
        CHECK(diagnosis.conflicting.empty());
    }

    // One dimension short: exactly one degree of freedom, and the free
    // direction it reports is a real one. "Real" is checked rather than
    // assumed: move along it and every residual must be unchanged to
    // first order, which is what being in the null space means.
    {
        Sketch sketch = BuildRectangle(1, nullptr);
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        CHECK(diagnosis.status == SketchStatus::UnderConstrained);
        CHECK(diagnosis.degrees_of_freedom == 1);
        CHECK(diagnosis.free_directions.size() == 1);

        const SketchParameters parameters(sketch);
        std::vector<double> before;
        std::string error;
        CHECK(EvaluateConstraints(sketch, parameters, &before, nullptr, nullptr, &error));
        std::vector<double> x;
        parameters.Gather(sketch, &x);
        const double step = 1e-5;
        std::vector<double> moved = x;
        for (std::size_t i = 0; i < moved.size(); ++i) moved[i] += step * diagnosis.free_directions[0][i];
        Sketch nudged = sketch;
        parameters.Scatter(moved, &nudged);
        std::vector<double> after;
        CHECK(EvaluateConstraints(nudged, parameters, &after, nullptr, nullptr, &error));
        double worst = 0.0;
        for (std::size_t i = 0; i < before.size(); ++i) worst = std::max(worst, std::fabs(after[i] - before[i]));
        std::printf("  moving %g along the reported free direction changes the residuals by %.3e\n", step,
                    worst);
        // Second order in the step, so far below the step itself.
        CHECK(worst < step * 1e-3);
    }

    // No dimensions at all: two degrees of freedom (width and height).
    {
        Sketch sketch = BuildRectangle(0, nullptr);
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        CHECK(diagnosis.status == SketchStatus::UnderConstrained);
        CHECK(diagnosis.degrees_of_freedom == 2);
    }

    // Redundant: a diagonal dimension consistent with the width and
    // height. The sketch still solves, and the extra constraint is named
    // rather than silently tolerated.
    {
        std::vector<SketchId> corners;
        Sketch sketch = BuildRectangle(2, &corners);
        const double diagonal = std::sqrt(4.0 * 4.0 + 2.5 * 2.5);
        const SketchId extra =
            sketch.Constrain(ConstraintKind::Distance, {corners[0], corners[2]}, {}, diagonal);
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        CHECK(diagnosis.status == SketchStatus::Redundant);
        CHECK(diagnosis.conflicting.empty());
        CHECK(!diagnosis.redundant.empty());
        CHECK(std::find(diagnosis.redundant.begin(), diagnosis.redundant.end(), extra) !=
              diagnosis.redundant.end());
        // And it really did solve: the geometry is the rectangle asked for.
        CHECK((PointAt(sketch, corners[2]) - Vec2d{4.0, 2.5}).Length() < 1e-8);
        std::printf("  redundant: %zu constraint(s) named, residual %.3e\n", diagnosis.redundant.size(),
                    diagnosis.residual_norm);
    }

    // Conflicting: the same diagonal, wrong. Rank-deficient in exactly
    // the same way; what differs is that the dependency is also present
    // in the residual.
    {
        std::vector<SketchId> corners;
        Sketch sketch = BuildRectangle(2, &corners);
        const SketchId extra = sketch.Constrain(ConstraintKind::Distance, {corners[0], corners[2]}, {}, 9.0);
        SketchDiagnosis diagnosis;
        CHECK(SolveSketch(&sketch, &diagnosis, {}));
        CHECK(diagnosis.status == SketchStatus::Conflicting);
        CHECK(!diagnosis.conflicting.empty());
        CHECK(std::find(diagnosis.conflicting.begin(), diagnosis.conflicting.end(), extra) !=
              diagnosis.conflicting.end());
        std::printf("  conflicting: %zu constraint(s) named, residual %.3e\n", diagnosis.conflicting.size(),
                    diagnosis.residual_norm);
    }

    // Two rectangles that share nothing are two clusters, and solving
    // them together gives the same answer as solving each alone.
    {
        Sketch sketch = BuildRectangle(2, nullptr);
        const SketchId q0 = sketch.AddPoint(Vec2d{10.0, 10.0}, true);
        const SketchId q1 = sketch.AddPoint(Vec2d{13.1, 10.2});
        const SketchId q2 = sketch.AddPoint(Vec2d{12.8, 11.9});
        const SketchId q3 = sketch.AddPoint(Vec2d{10.3, 12.2});
        const SketchId b2 = sketch.AddLineFromPoints(q0, q1);
        const SketchId r2 = sketch.AddLineFromPoints(q1, q2);
        const SketchId t2 = sketch.AddLineFromPoints(q2, q3);
        const SketchId l2 = sketch.AddLineFromPoints(q3, q0);
        sketch.Constrain(ConstraintKind::Horizontal, {}, {b2});
        sketch.Constrain(ConstraintKind::Horizontal, {}, {t2});
        sketch.Constrain(ConstraintKind::Vertical, {}, {r2});
        sketch.Constrain(ConstraintKind::Vertical, {}, {l2});
        sketch.Constrain(ConstraintKind::HorizontalDistance, {q0, q1}, {}, 6.0);
        sketch.Constrain(ConstraintKind::VerticalDistance, {q1, q2}, {}, 1.5);

        Sketch decomposed = sketch;
        SketchDiagnosis with_clusters;
        ConstraintSolveOptions clustered;
        clustered.decompose = true;
        CHECK(SolveSketch(&decomposed, &with_clusters, clustered));

        Sketch monolithic = sketch;
        SketchDiagnosis without;
        ConstraintSolveOptions single;
        single.decompose = false;
        CHECK(SolveSketch(&monolithic, &without, single));

        CHECK(with_clusters.clusters == 2);
        CHECK(with_clusters.status == SketchStatus::Solved);
        CHECK(without.status == SketchStatus::Solved);
        double worst = 0.0;
        for (const SketchPoint &point : decomposed.Points()) {
            const SketchPoint *other = monolithic.GetPoint(point.id);
            CHECK(other != nullptr);
            worst = std::max(worst, (point.position - other->position).Length());
        }
        std::printf("  clustering: %d clusters, agreeing with the single solve to %.3e\n",
                    with_clusters.clusters, worst);
        CHECK(worst < 1e-8);
        CHECK((PointAt(decomposed, q2) - Vec2d{16.0, 11.5}).Length() < 1e-8);
    }
}

void TestDragging() {
    std::printf("dragging\n");
    // A rectangle with a width but no height: dragging a top corner
    // upwards should change the height and nothing else, and the
    // constraints must still hold afterwards.
    std::vector<SketchId> corners;
    Sketch sketch = BuildRectangle(1, &corners);
    SketchDiagnosis diagnosis;
    CHECK(SolveSketch(&sketch, &diagnosis, {}));
    CHECK(diagnosis.degrees_of_freedom == 1);

    CHECK(DragPoint(&sketch, corners[2], Vec2d{4.0, 6.0}, &diagnosis, {}));
    CHECK(diagnosis.status == SketchStatus::UnderConstrained);
    CHECK((PointAt(sketch, corners[2]) - Vec2d{4.0, 6.0}).Length() < 1e-7);
    CHECK((PointAt(sketch, corners[3]) - Vec2d{0.0, 6.0}).Length() < 1e-7);
    CHECK((PointAt(sketch, corners[1]) - Vec2d{4.0, 0.0}).Length() < 1e-7);
    CHECK(diagnosis.residual_norm < 1e-8);

    // Dragging sideways cannot change the width, which is dimensioned:
    // the corner follows in y and stays put in x.
    CHECK(DragPoint(&sketch, corners[2], Vec2d{9.0, 3.0}, &diagnosis, {}));
    CHECK(Near(PointAt(sketch, corners[2]).x, 4.0, 1e-7));
    CHECK(Near(PointAt(sketch, corners[2]).y, 3.0, 1e-7));
    CHECK(diagnosis.residual_norm < 1e-8);

    // A fully constrained sketch cannot be dragged anywhere, and says so
    // by leaving the geometry alone rather than by distorting it.
    Sketch rigid = BuildRectangle(2, &corners);
    CHECK(SolveSketch(&rigid, &diagnosis, {}));
    const Vec2d before = PointAt(rigid, corners[2]);
    CHECK(DragPoint(&rigid, corners[2], Vec2d{9.0, 9.0}, &diagnosis, {}));
    CHECK((PointAt(rigid, corners[2]) - before).Length() < 1e-7);

    std::printf("  a dimensioned width holds while the height follows the pointer\n");
}

// --- D.4 profile extraction --------------------------------------------

void TestProfiles() {
    std::printf("profiles\n");

    auto square = [](Sketch *sketch, const Vec2d &corner, double side) {
        const SketchId a = sketch->AddPoint(corner);
        const SketchId b = sketch->AddPoint(corner + Vec2d{side, 0.0});
        const SketchId c = sketch->AddPoint(corner + Vec2d{side, side});
        const SketchId d = sketch->AddPoint(corner + Vec2d{0.0, side});
        sketch->AddLineFromPoints(a, b);
        sketch->AddLineFromPoints(b, c);
        sketch->AddLineFromPoints(c, d);
        sketch->AddLineFromPoints(d, a);
    };

    // One square: one profile, its own area.
    {
        Sketch sketch;
        square(&sketch, Vec2d{0.0, 0.0}, 4.0);
        std::vector<Sketch::Profile> profiles;
        std::string error;
        CHECK(sketch.ExtractProfiles(&profiles, &error));
        CHECK(profiles.size() == 1);
        CHECK(profiles[0].holes.empty());
        CHECK(Near(profiles[0].area, 16.0, 1e-9));
        CHECK(profiles[0].outer.size() == 4);
    }

    // A circle inside a square: one profile with a hole, and the area is
    // the difference. Reporting it as two profiles would be the obvious
    // wrong answer, and an extrude built on it would fill the hole.
    {
        Sketch sketch;
        square(&sketch, Vec2d{0.0, 0.0}, 6.0);
        sketch.AddCircle(Vec2d{3.0, 3.0}, 1.5);
        std::vector<Sketch::Profile> profiles;
        std::string error;
        CHECK(sketch.ExtractProfiles(&profiles, &error));
        // The square with the circular hole, and the disc itself: both
        // are genuine regions of the arrangement, and which one an
        // extrude wants is the caller's business.
        CHECK(profiles.size() == 2);
        double washer = 0.0;
        double disc = 0.0;
        for (const Sketch::Profile &profile : profiles) {
            if (profile.holes.empty()) {
                disc = profile.area;
            } else {
                washer = profile.area;
                CHECK(profile.holes.size() == 1);
            }
        }
        std::printf("  washer %.6f (exactly %.6f), disc %.6f (exactly %.6f)\n", washer,
                    36.0 - kPi * 2.25, disc, kPi * 2.25);
        CHECK(Near(disc, kPi * 2.25, 1e-3));
        CHECK(Near(washer, 36.0 - kPi * 2.25, 1e-3));
    }

    // Two squares side by side: two profiles, neither a hole in the other.
    {
        Sketch sketch;
        square(&sketch, Vec2d{0.0, 0.0}, 2.0);
        square(&sketch, Vec2d{5.0, 0.0}, 3.0);
        std::vector<Sketch::Profile> profiles;
        std::string error;
        CHECK(sketch.ExtractProfiles(&profiles, &error));
        CHECK(profiles.size() == 2);
        double total = 0.0;
        for (const Sketch::Profile &profile : profiles) {
            CHECK(profile.holes.empty());
            total += profile.area;
        }
        CHECK(Near(total, 4.0 + 9.0, 1e-9));
    }

    // Two overlapping squares: the arrangement cuts them into three
    // regions -- each one's own part and the shared part -- and each
    // curve appears in more than one, split where they cross.
    {
        Sketch sketch;
        square(&sketch, Vec2d{0.0, 0.0}, 4.0);
        square(&sketch, Vec2d{2.0, 2.0}, 4.0);
        std::vector<Sketch::Profile> profiles;
        std::string error;
        CHECK(sketch.ExtractProfiles(&profiles, &error));
        CHECK(profiles.size() == 3);
        double total = 0.0;
        for (const Sketch::Profile &profile : profiles) total += profile.area;
        // The two L-shapes and the overlap: 12 + 12 + 4.
        std::printf("  two overlapping squares: %zu regions totalling %.9f (exactly 28)\n", profiles.size(),
                    total);
        CHECK(Near(total, 28.0, 1e-9));
    }

    // Construction geometry is not part of any profile.
    {
        Sketch sketch;
        square(&sketch, Vec2d{0.0, 0.0}, 4.0);
        sketch.AddLine(Vec2d{-1.0, 2.0}, Vec2d{5.0, 2.0}, true);
        std::vector<Sketch::Profile> profiles;
        std::string error;
        CHECK(sketch.ExtractProfiles(&profiles, &error));
        CHECK(profiles.size() == 1);
        CHECK(Near(profiles[0].area, 16.0, 1e-9));
        // Without the construction flag the same line would cut the
        // square in two, which is the check that the flag is doing
        // something rather than being ignored.
        Sketch cut;
        square(&cut, Vec2d{0.0, 0.0}, 4.0);
        cut.AddLine(Vec2d{-1.0, 2.0}, Vec2d{5.0, 2.0}, false);
        CHECK(cut.ExtractProfiles(&profiles, &error));
        CHECK(profiles.size() == 2);
    }

    // Open geometry has no profile, and that is not an error.
    {
        Sketch sketch;
        sketch.AddLine(Vec2d{0.0, 0.0}, Vec2d{2.0, 0.0});
        sketch.AddLine(Vec2d{2.0, 0.0}, Vec2d{2.0, 2.0});
        std::vector<Sketch::Profile> profiles;
        std::string error;
        CHECK(sketch.ExtractProfiles(&profiles, &error));
        CHECK(profiles.empty());
        CHECK(error.empty());
    }

    // A profile made of arcs and lines: a slot, whose area is known.
    {
        Sketch sketch;
        const double half = 3.0;
        const double radius = 1.0;
        sketch.AddLine(Vec2d{-half, radius}, Vec2d{half, radius});
        sketch.AddLine(Vec2d{half, -radius}, Vec2d{-half, -radius});
        sketch.AddArc(Vec2d{half, 0.0}, Vec2d{half, radius}, Vec2d{half, -radius}, false);
        sketch.AddArc(Vec2d{-half, 0.0}, Vec2d{-half, -radius}, Vec2d{-half, radius}, false);
        std::vector<Sketch::Profile> profiles;
        std::string error;
        CHECK(sketch.ExtractProfiles(&profiles, &error));
        CHECK(profiles.size() == 1);
        const double exact = 2.0 * half * 2.0 * radius + kPi * radius * radius;
        std::printf("  slot: %.6f (exactly %.6f)\n", profiles[0].area, exact);
        CHECK(Near(profiles[0].area, exact, 2e-3));
    }
}

// A whole sketch, drawn the way one would be: geometry placed roughly,
// constrained, solved, and then extruded-ready. This is the test that
// would catch the parts working and the whole not.
void TestEndToEnd() {
    std::printf("end to end\n");
    Sketch sketch;
    const SketchId p0 = sketch.Origin();
    const SketchId p1 = sketch.AddPoint(Vec2d{9.3, 0.7});
    const SketchId p2 = sketch.AddPoint(Vec2d{9.1, 5.2});
    const SketchId p3 = sketch.AddPoint(Vec2d{0.4, 4.8});
    const SketchId bottom = sketch.AddLineFromPoints(p0, p1);
    const SketchId right = sketch.AddLineFromPoints(p1, p2);
    const SketchId top = sketch.AddLineFromPoints(p2, p3);
    const SketchId left = sketch.AddLineFromPoints(p3, p0);
    sketch.Constrain(ConstraintKind::Horizontal, {}, {bottom});
    sketch.Constrain(ConstraintKind::Horizontal, {}, {top});
    sketch.Constrain(ConstraintKind::Vertical, {}, {right});
    sketch.Constrain(ConstraintKind::Vertical, {}, {left});
    sketch.Constrain(ConstraintKind::HorizontalDistance, {p0, p1}, {}, 10.0);
    sketch.Constrain(ConstraintKind::VerticalDistance, {p1, p2}, {}, 6.0);

    // Two holes, placed roughly, constrained to be equal and to sit on a
    // construction centreline at fixed distances from the left edge.
    const SketchId centreline = sketch.AddLine(Vec2d{0.0, 2.7}, Vec2d{10.0, 3.3}, true);
    sketch.Constrain(ConstraintKind::Horizontal, {}, {centreline});
    // Its ends are pinned to the plate's sides, which is what a user
    // would do and what makes the sketch fully constrained: without it
    // the line can slide along itself, and the diagnosis correctly
    // reports the two degrees of freedom that leaves.
    sketch.Constrain(ConstraintKind::PointOnObject, {sketch.GetEntity(centreline)->points[0]}, {left});
    sketch.Constrain(ConstraintKind::PointOnObject, {sketch.GetEntity(centreline)->points[1]}, {right});
    const SketchId hole_a = sketch.AddCircle(Vec2d{2.4, 2.9}, 0.8);
    const SketchId hole_b = sketch.AddCircle(Vec2d{7.7, 3.1}, 1.2);
    const SketchId centre_a = sketch.GetEntity(hole_a)->points[0];
    const SketchId centre_b = sketch.GetEntity(hole_b)->points[0];
    sketch.Constrain(ConstraintKind::PointOnObject, {centre_a}, {centreline});
    sketch.Constrain(ConstraintKind::PointOnObject, {centre_b}, {centreline});
    sketch.Constrain(ConstraintKind::Equal, {}, {hole_a, hole_b});
    sketch.Constrain(ConstraintKind::Radius, {}, {hole_a}, 1.0);
    sketch.Constrain(ConstraintKind::VerticalDistance, {p0, centre_a}, {}, 3.0);
    sketch.Constrain(ConstraintKind::HorizontalDistance, {p0, centre_a}, {}, 2.5);
    sketch.Constrain(ConstraintKind::HorizontalDistance, {centre_a, centre_b}, {}, 5.0);
    // A reference dimension, which measures rather than constrains.
    const SketchId reference =
        sketch.AddConstraint(SketchConstraint{kNoSketchId, ConstraintKind::Distance, {p0, p2}, {}, 0.0, false});

    SketchDiagnosis diagnosis;
    CHECK(SolveSketch(&sketch, &diagnosis, {}));
    CHECK(diagnosis.status == SketchStatus::Solved);
    CHECK(diagnosis.degrees_of_freedom == 0);
    CHECK(diagnosis.residual_norm < 1e-8);

    CHECK((PointAt(sketch, p2) - Vec2d{10.0, 6.0}).Length() < 1e-8);
    CHECK((PointAt(sketch, centre_a) - Vec2d{2.5, 3.0}).Length() < 1e-8);
    CHECK((PointAt(sketch, centre_b) - Vec2d{7.5, 3.0}).Length() < 1e-8);
    double radius_b = 0.0;
    CHECK(sketch.Radius(hole_b, &radius_b));
    CHECK(Near(radius_b, 1.0, 1e-9));

    // The reference dimension now reports the diagonal.
    const SketchConstraint *measured = sketch.GetConstraint(reference);
    CHECK(measured != nullptr);
    CHECK(Near(measured->value, std::sqrt(100.0 + 36.0), 1e-8));

    // And the profile is the plate with two holes.
    std::vector<Sketch::Profile> profiles;
    std::string error;
    CHECK(sketch.ExtractProfiles(&profiles, &error));
    const Sketch::Profile *plate = nullptr;
    for (const Sketch::Profile &profile : profiles) {
        if (profile.holes.size() == 2) plate = &profile;
    }
    CHECK(plate != nullptr);
    const double exact = 60.0 - 2.0 * kPi;
    std::printf("  plate with two holes: %.6f (exactly %.6f), %d dof, solved in %d iterations\n", plate->area,
                exact, diagnosis.degrees_of_freedom, diagnosis.iterations);
    CHECK(Near(plate->area, exact, 5e-3));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestJacobians();
    TestGeometry();
    TestPlaneAttachment();
    TestSolveTriangle();
    TestSolveRectangle();
    TestSolveTangency();
    TestSolveAssortedConstraints();
    TestDiagnosis();
    TestDragging();
    TestProfiles();
    TestEndToEnd();
    std::printf("cad_sketch_test passed (%d checks)\n", g_checks);
    return 0;
}
