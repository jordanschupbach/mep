// Windowless coverage for cad_assembly.h (plans/CAD_FEM_PLAN.md Part E.6).
//
// THE DEGREE-OF-FREEDOM COUNT IS THE SHARPEST TEST HERE, sharper than any
// position, because it reads the rank of the Jacobian rather than the
// answer: it says what the mates *mean*, not merely where the solver
// stopped. Two plates, one on the other, tell the whole story in four
// numbers -- 6 free, 3 after a coincident mate, 1 after a hole is lined
// up, 0 after a second hole -- and a mate formulated so that it removes
// the wrong count gets caught there even when it happens to produce a
// plausible placement.
//
// The placements are then checked exactly. Every mate used below has a
// unique solution by construction, so "converged" and "converged to the
// right answer" are different claims and both are made.

#include "cad_assembly.h"

#include "cad_feature.h"

#include "cad_pcurve.h"
#include "cad_tessellate.h"
#include "cad_validate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

void CheckMessage(bool condition, const char *expression, int line, const std::string &message) {
    if (!condition && !message.empty()) std::fprintf(stderr, "  reported: %s\n", message.c_str());
    Check(condition, expression, line);
}
#define CHECK_MESSAGE(condition, message) CheckMessage((condition), #condition, __LINE__, (message))

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool NearVec(const Vec3d &a, const Vec3d &b, double tol) { return (a - b).Length() <= tol; }

Model BoxPart(const Vec3d &corner, const Vec3d &size, EntityId *body) {
    Model model;
    std::string error;
    Check(MakeBox(corner, size, &model, body), "MakeBox", __LINE__);
    Check(BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
    return model;
}

Model CylinderPart(double radius, double height, EntityId *body) {
    Model model;
    std::string error;
    Check(MakeCylinder(Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 1.0}, radius, height, &model, body),
          "MakeCylinder", __LINE__);
    Check(BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
    return model;
}

EntityId FaceFacing(const Model &model, EntityId body, const Vec3d &direction) {
    const Body *solid = model.GetBody(body);
    if (solid == nullptr) return kNoEntity;
    for (EntityId shell_id : solid->shells) {
        for (EntityId face_id : model.GetShell(shell_id)->faces) {
            const Face *face = model.GetFace(face_id);
            const auto *plane = dynamic_cast<const PlaneSurface *>(model.SurfaceAt(face->surface));
            if (plane == nullptr) continue;
            Vec3d normal = plane->PlaneNormal().Normalized();
            if (face->orientation == Orientation::Reversed) normal = normal * -1.0;
            if (normal.Dot(direction.Normalized()) > 0.999) return face_id;
        }
    }
    return kNoEntity;
}

double SolidVolume(const Model &model, EntityId body) {
    ValidationReport report;
    if (!ValidateBody(model, body, &report, {})) return std::nan("");
    TessellationMesh mesh;
    std::string error;
    if (!TessellateBody(model, body, {}, &mesh, &error)) return std::nan("");
    if (!mesh.IsClosed()) return std::nan("");
    return mesh.SignedVolume();
}

Box3d BodyExtent(const Model &model, EntityId body) {
    Box3d extent;
    const Body *solid = model.GetBody(body);
    if (solid == nullptr) return extent;
    for (EntityId shell_id : solid->shells) {
        for (EntityId vertex : model.VerticesOfShell(shell_id)) {
            extent.Expand(model.GetVertex(vertex)->point);
        }
    }
    return extent;
}

// A mate end, written without brace-initialising every field: the ones
// that record where a datum came from are filled in by CaptureMateEnd
// and are left alone here.
MateEnd On(int instance, const Datum &datum) {
    MateEnd end;
    end.instance = instance;
    end.datum = datum;
    return end;
}

Datum AxisAt(const Vec3d &origin, const Vec3d &direction) {
    Datum d;
    d.kind = DatumKind::Axis;
    d.origin = origin;
    d.direction = direction.Normalized();
    return d;
}

Datum PointAt(const Vec3d &origin) {
    Datum d;
    d.kind = DatumKind::Point;
    d.origin = origin;
    return d;
}

// --- Rotations -----------------------------------------------------------

void TestQuaternion() {
    // A quarter turn about z takes x to y.
    const Quatd q = Quatd::FromAxisAngle(Vec3d{0.0, 0.0, 1.0}, kHalfPi);
    CHECK(NearVec(q.Rotate(Vec3d{1.0, 0.0, 0.0}), Vec3d{0.0, 1.0, 0.0}, 1e-12));
    CHECK(NearVec(q.Rotate(Vec3d{0.0, 0.0, 1.0}), Vec3d{0.0, 0.0, 1.0}, 1e-12));
    // Composition is rotation after rotation, in that order.
    const Quatd twice = q * q;
    CHECK(NearVec(twice.Rotate(Vec3d{1.0, 0.0, 0.0}), Vec3d{-1.0, 0.0, 0.0}, 1e-12));
    // The conjugate undoes it.
    const Quatd back = q.Conjugate() * q;
    CHECK(NearVec(back.Rotate(Vec3d{3.0, -1.0, 2.0}), Vec3d{3.0, -1.0, 2.0}, 1e-12));
    // Length is preserved, which a quaternion that had drifted off the
    // unit sphere would not do.
    const Quatd tilted = Quatd::FromAxisAngle(Vec3d{1.0, 2.0, -3.0}, 1.234);
    CHECK(Near(tilted.Rotate(Vec3d{4.0, 5.0, 6.0}).Length(), Vec3d(4.0, 5.0, 6.0).Length(), 1e-12));
    // Between really takes one to the other, including the half-turn case
    // where there is no preferred axis.
    for (const Vec3d &to : {Vec3d{0.0, 1.0, 0.0}, Vec3d{-1.0, 0.0, 0.0}, Vec3d{0.3, -0.4, 0.5}}) {
        const Quatd r = Quatd::Between(Vec3d{1.0, 0.0, 0.0}, to);
        CHECK(NearVec(r.Rotate(Vec3d{1.0, 0.0, 0.0}), to.Normalized(), 1e-12));
    }
    // And the matrix form agrees with the direct one, which is what
    // Flatten depends on.
    Placement p;
    p.position = Vec3d{1.0, -2.0, 3.0};
    p.rotation = tilted;
    const Mat4d m = p.ToMatrix();
    for (const Vec3d &v : {Vec3d{1.0, 0.0, 0.0}, Vec3d{0.0, 1.0, 0.0}, Vec3d{2.0, -3.0, 0.5}}) {
        CHECK(NearVec(m.TransformPoint(v), p.Apply(v), 1e-12));
    }
    CHECK(m.LinearDeterminant() > 0.0);
    std::printf("quaternions: rotation, composition, inverse and the matrix form all agree\n");
}

// --- Datums --------------------------------------------------------------

void TestDatums() {
    EntityId body = kNoEntity;
    const Model box = BoxPart(Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 3.0, 4.0}, &body);
    std::string error;

    // A planar face gives the plane it lies in, with the face's *outward*
    // normal -- not the surface's own, which for half a box's faces
    // points the other way.
    Datum top;
    CHECK_MESSAGE(DatumFromFace(box, FaceFacing(box, body, Vec3d{0.0, 0.0, 1.0}), &top, &error), error);
    CHECK(top.kind == DatumKind::Plane);
    CHECK(NearVec(top.direction, Vec3d{0.0, 0.0, 1.0}, 1e-12));
    CHECK(Near(top.origin.z, 4.0, 1e-12));
    Datum bottom;
    CHECK_MESSAGE(DatumFromFace(box, FaceFacing(box, body, Vec3d{0.0, 0.0, -1.0}), &bottom, &error),
                  error);
    CHECK(NearVec(bottom.direction, Vec3d{0.0, 0.0, -1.0}, 1e-12));
    CHECK(Near(bottom.origin.z, 0.0, 1e-12));

    Datum raised;
    CHECK_MESSAGE(OffsetDatumPlane(top, 1.5, &raised, &error), error);
    CHECK(Near(raised.origin.z, 5.5, 1e-12));
    CHECK(!OffsetDatumPlane(PointAt(Vec3d{}), 1.0, &raised, &error));

    // A cylindrical face gives its axis.
    EntityId tube_body = kNoEntity;
    const Model tube = CylinderPart(2.0, 5.0, &tube_body);
    EntityId curved = kNoEntity;
    for (EntityId shell_id : tube.GetBody(tube_body)->shells) {
        for (EntityId face_id : tube.GetShell(shell_id)->faces) {
            if (dynamic_cast<const CylinderSurface *>(tube.SurfaceAt(tube.GetFace(face_id)->surface))) {
                curved = face_id;
            }
        }
    }
    CHECK(curved != kNoEntity);
    Datum axis;
    CHECK_MESSAGE(DatumFromFace(tube, curved, &axis, &error), error);
    CHECK(axis.kind == DatumKind::Axis);
    CHECK(NearVec(axis.direction, Vec3d{0.0, 0.0, 1.0}, 1e-12));

    // A circular edge stands for its hole, so it gives the axis through
    // the centre rather than the circle.
    EntityId rim = kNoEntity;
    for (EntityId shell_id : tube.GetBody(tube_body)->shells) {
        for (EntityId e : tube.EdgesOfShell(shell_id)) {
            if (dynamic_cast<const Circle3 *>(tube.CurveAt(tube.GetEdge(e)->curve))) rim = e;
        }
    }
    CHECK(rim != kNoEntity);
    Datum rim_axis;
    CHECK_MESSAGE(DatumFromEdge(tube, rim, &rim_axis, &error), error);
    CHECK(rim_axis.kind == DatumKind::Axis);
    CHECK(Near(std::fabs(rim_axis.direction.z), 1.0, 1e-12));
    CHECK(Near(rim_axis.origin.x, 0.0, 1e-12) && Near(rim_axis.origin.y, 0.0, 1e-12));

    // Constructed datums.
    Datum through;
    CHECK_MESSAGE(DatumPlaneThroughPoints(Vec3d{0, 0, 0}, Vec3d{1, 0, 0}, Vec3d{0, 1, 0}, &through, &error),
                  error);
    CHECK(Near(std::fabs(through.direction.z), 1.0, 1e-12));
    CHECK(!DatumPlaneThroughPoints(Vec3d{0, 0, 0}, Vec3d{1, 0, 0}, Vec3d{2, 0, 0}, &through, &error));
    CHECK(error.find("in a line") != std::string::npos);

    Datum side;
    CHECK_MESSAGE(DatumPlaneThroughPoints(Vec3d{0, 0, 0}, Vec3d{0, 1, 0}, Vec3d{0, 0, 1}, &side, &error),
                  error);
    CHECK(Near(std::fabs(side.direction.x), 1.0, 1e-12));
    Datum meeting;
    CHECK_MESSAGE(DatumAxisFromPlanes(top, side, &meeting, &error), error);
    // The top of the box is z = 4 and that side is x = 0, so they meet
    // along the y axis at that height.
    CHECK(Near(std::fabs(meeting.direction.y), 1.0, 1e-12));
    CHECK(Near(meeting.origin.x, 0.0, 1e-12) && Near(meeting.origin.z, 4.0, 1e-12));
    // Two faces of a box that face opposite ways are still parallel.
    CHECK(!DatumAxisFromPlanes(top, bottom, &meeting, &error));
    CHECK(error.find("parallel") != std::string::npos);

    Datum where;
    CHECK_MESSAGE(DatumPointFromAxisAndPlane(AxisAt(Vec3d{1.0, 2.0, 0.0}, Vec3d{0, 0, 1}), top, &where,
                                             &error),
                  error);
    CHECK(NearVec(where.origin, Vec3d{1.0, 2.0, 4.0}, 1e-12));
    CHECK(!DatumPointFromAxisAndPlane(AxisAt(Vec3d{}, Vec3d{1, 0, 0}), top, &where, &error));

    // And a datum moves with its instance.
    Placement p;
    p.position = Vec3d{5.0, 0.0, 0.0};
    p.rotation = Quatd::FromAxisAngle(Vec3d{1, 0, 0}, kHalfPi);
    const Datum moved = PlaceDatum(top, p);
    CHECK(NearVec(moved.direction, Vec3d{0.0, -1.0, 0.0}, 1e-12));
    CHECK(NearVec(moved.origin, Vec3d{5.0 + top.origin.x, -4.0, top.origin.y}, 1e-12));
    std::printf("datums: from faces, edges and vertices, constructed, offset and placed\n");
}

// --- Mates and degrees of freedom -----------------------------------------

// Two plates, one lying on the other. The count goes 6, 3, 1, 0 as the
// mates are added, and each of those numbers is a statement about what
// the mate means rather than about where the solver stopped.
void TestDegreesOfFreedom() {
    EntityId plate_body = kNoEntity;
    const Model plate = BoxPart(Vec3d{0.0, 0.0, 0.0}, Vec3d{10.0, 6.0, 1.0}, &plate_body);

    Assembly assembly;
    const int part = assembly.AddPart(plate, plate_body, "plate");
    const int lower = assembly.AddInstance(part, "lower", {}, true);
    Placement guess;
    guess.position = Vec3d{4.0, -3.0, 7.0};
    guess.rotation = Quatd::FromAxisAngle(Vec3d{1.0, 2.0, 0.5}, 0.7);
    const int upper = assembly.AddInstance(part, "upper", guess, false);

    AssemblyReport report;
    CHECK(assembly.Solve(&report));
    std::printf("two plates, mates added one at a time:\n");
    std::printf("  no mates:               %d degrees of freedom\n", report.degrees_of_freedom);
    CHECK(report.degrees_of_freedom == 6);

    Datum top;
    Datum bottom;
    std::string error;
    CHECK_MESSAGE(DatumFromFace(plate, FaceFacing(plate, plate_body, Vec3d{0, 0, 1}), &top, &error),
                  error);
    CHECK_MESSAGE(DatumFromFace(plate, FaceFacing(plate, plate_body, Vec3d{0, 0, -1}), &bottom, &error),
                  error);

    Mate flush;
    flush.kind = MateKind::Coincident;
    flush.name = "upper sits on lower";
    flush.a = On(lower, top);
    flush.b = On(upper, bottom);
    assembly.AddMate(flush);
    CHECK_MESSAGE(assembly.Solve(&report), report.error);
    std::printf("  + faces flush:          %d\n", report.degrees_of_freedom);
    CHECK(report.degrees_of_freedom == 3);

    Mate hole;
    hole.kind = MateKind::Concentric;
    hole.name = "first hole";
    hole.a = On(lower, AxisAt(Vec3d{2.0, 3.0, 0.0}, Vec3d{0, 0, 1}));
    hole.b = On(upper, AxisAt(Vec3d{2.0, 3.0, 0.0}, Vec3d{0, 0, 1}));
    assembly.AddMate(hole);
    CHECK_MESSAGE(assembly.Solve(&report), report.error);
    std::printf("  + one hole lined up:    %d\n", report.degrees_of_freedom);
    // Two of the concentric mate's four go to angles the flush mate had
    // already taken, so it is worth two here and not four.
    CHECK(report.degrees_of_freedom == 1);

    Mate second;
    second.kind = MateKind::Concentric;
    second.name = "second hole";
    second.a = On(lower, AxisAt(Vec3d{8.0, 3.0, 0.0}, Vec3d{0, 0, 1}));
    second.b = On(upper, AxisAt(Vec3d{8.0, 3.0, 0.0}, Vec3d{0, 0, 1}));
    assembly.AddMate(second);
    CHECK_MESSAGE(assembly.Solve(&report), report.error);
    std::printf("  + a second hole:        %d, residual %.3e in %d iterations\n",
                report.degrees_of_freedom, report.residual, report.iterations);
    CHECK(report.degrees_of_freedom == 0);

    // And it converged to the one placement those mates describe, from a
    // starting guess that was nowhere near it.
    const Placement &solved = assembly.GetInstance(upper)->placement;
    CHECK(NearVec(solved.position, Vec3d{0.0, 0.0, 1.0}, 1e-7));
    CHECK(NearVec(solved.rotation.Rotate(Vec3d{1, 0, 0}), Vec3d{1, 0, 0}, 1e-7));
    CHECK(NearVec(solved.rotation.Rotate(Vec3d{0, 0, 1}), Vec3d{0, 0, 1}, 1e-7));
    CHECK(report.residual < 1e-9);

    // Flattened, the stack is 10 by 6 by 2 and weighs what two plates
    // weigh -- which is the check that a solved placement is a real
    // transform and not just a number in a report.
    Model flat;
    std::vector<EntityId> bodies;
    CHECK_MESSAGE(assembly.Flatten(&flat, &bodies, &error), error);
    CHECK(bodies.size() == 2);
    CHECK(Near(SolidVolume(flat, bodies[0]) + SolidVolume(flat, bodies[1]), 120.0, 1e-9));
    CHECK(Near(BodyExtent(flat, bodies[1]).z.lo, 1.0, 1e-7));
    CHECK(Near(BodyExtent(flat, bodies[1]).z.hi, 2.0, 1e-7));
    std::printf("  flattened: two plates, %.6f in all, the upper from z=1 to z=2\n",
                SolidVolume(flat, bodies[0]) + SolidVolume(flat, bodies[1]));
}

void TestOffsetAndDistance() {
    EntityId body = kNoEntity;
    const Model box = BoxPart(Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 2.0, 2.0}, &body);
    std::string error;
    Datum right;
    Datum left;
    CHECK_MESSAGE(DatumFromFace(box, FaceFacing(box, body, Vec3d{1, 0, 0}), &right, &error), error);
    CHECK_MESSAGE(DatumFromFace(box, FaceFacing(box, body, Vec3d{-1, 0, 0}), &left, &error), error);

    for (double gap : {0.0, 0.5, 3.0}) {
        Assembly assembly;
        const int part = assembly.AddPart(box, body, "cube");
        const int fixed = assembly.AddInstance(part, "fixed", {}, true);
        Placement guess;
        guess.position = Vec3d{-7.0, 2.0, 1.0};
        const int free = assembly.AddInstance(part, "free", guess, false);
        Mate mate;
        mate.kind = gap == 0.0 ? MateKind::Coincident : MateKind::Offset;
        mate.a = On(fixed, right);
        mate.b = On(free, left);
        mate.value = gap;
        assembly.AddMate(mate);
        AssemblyReport report;
        CHECK_MESSAGE(assembly.Solve(&report), report.error);
        CHECK(report.degrees_of_freedom == 3);
        Model flat;
        std::vector<EntityId> bodies;
        CHECK_MESSAGE(assembly.Flatten(&flat, &bodies, &error), error);
        // The free cube's near face is exactly `gap` beyond the fixed
        // one's far face.
        CHECK(Near(BodyExtent(flat, bodies[1]).x.lo, 2.0 + gap, 1e-7));
    }
    std::printf("offset: a cube held 0, 0.5 and 3 beyond another's face\n");

    // Distance between two points, which leaves a sphere of solutions --
    // five degrees of freedom -- but pins the one number it names.
    Assembly assembly;
    const int part = assembly.AddPart(box, body, "cube");
    const int fixed = assembly.AddInstance(part, "fixed", {}, true);
    Placement guess;
    guess.position = Vec3d{1.0, 1.0, 1.0};
    const int free = assembly.AddInstance(part, "free", guess, false);
    Mate span;
    span.kind = MateKind::Distance;
    span.a = On(fixed, PointAt(Vec3d{0.0, 0.0, 0.0}));
    span.b = On(free, PointAt(Vec3d{0.0, 0.0, 0.0}));
    span.value = 9.0;
    assembly.AddMate(span);
    AssemblyReport report;
    CHECK_MESSAGE(assembly.Solve(&report), report.error);
    CHECK(report.degrees_of_freedom == 5);
    CHECK(Near(assembly.GetInstance(free)->placement.position.Length(), 9.0, 1e-7));
    std::printf("distance: one number pinned, five degrees of freedom left\n");
}

void TestAngleAndParallel() {
    EntityId body = kNoEntity;
    const Model box = BoxPart(Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 2.0, 2.0}, &body);

    for (double degrees : {30.0, 45.0, 90.0, 120.0}) {
        Assembly assembly;
        const int part = assembly.AddPart(box, body, "cube");
        const int fixed = assembly.AddInstance(part, "fixed", {}, true);
        Placement guess;
        guess.rotation = Quatd::FromAxisAngle(Vec3d{0, 0, 1}, 0.2);
        const int free = assembly.AddInstance(part, "free", guess, false);
        Mate angle;
        angle.kind = MateKind::Angle;
        angle.a = On(fixed, AxisAt(Vec3d{}, Vec3d{1, 0, 0}));
        angle.b = On(free, AxisAt(Vec3d{}, Vec3d{1, 0, 0}));
        angle.value = DegToRad(degrees);
        assembly.AddMate(angle);
        AssemblyReport report;
        CHECK_MESSAGE(assembly.Solve(&report), report.error);
        CHECK(report.degrees_of_freedom == 5);
        const Vec3d turned = assembly.GetInstance(free)->placement.rotation.Rotate(Vec3d{1, 0, 0});
        CHECK(Near(std::acos(std::max(-1.0, std::min(1.0, turned.Dot(Vec3d{1, 0, 0})))),
                   DegToRad(degrees), 1e-7));
    }
    std::printf("angle: 30, 45, 90 and 120 degrees, each held to 1e-7\n");

    Assembly assembly;
    const int part = assembly.AddPart(box, body, "cube");
    const int fixed = assembly.AddInstance(part, "fixed", {}, true);
    Placement guess;
    guess.rotation = Quatd::FromAxisAngle(Vec3d{1, 1, 0}, 1.1);
    const int free = assembly.AddInstance(part, "free", guess, false);
    Mate parallel;
    parallel.kind = MateKind::Parallel;
    parallel.a = On(fixed, AxisAt(Vec3d{}, Vec3d{0, 0, 1}));
    parallel.b = On(free, AxisAt(Vec3d{}, Vec3d{0, 0, 1}));
    assembly.AddMate(parallel);
    AssemblyReport report;
    CHECK_MESSAGE(assembly.Solve(&report), report.error);
    CHECK(report.degrees_of_freedom == 4);
    const Vec3d up = assembly.GetInstance(free)->placement.rotation.Rotate(Vec3d{0, 0, 1});
    CHECK(Near(std::fabs(up.z), 1.0, 1e-7));
    std::printf("parallel: either way round, four degrees of freedom left\n");
}

void TestConcentricEitherWayRound() {
    // A pin into a hole goes in from either end, which is what makes
    // concentric a cross product rather than a difference. Starting the
    // pin upside down has to converge, not fight.
    EntityId pin_body = kNoEntity;
    const Model pin = CylinderPart(0.5, 4.0, &pin_body);
    EntityId plate_body = kNoEntity;
    const Model plate = BoxPart(Vec3d{-5.0, -5.0, 0.0}, Vec3d{10.0, 10.0, 1.0}, &plate_body);

    Assembly assembly;
    const int plate_part = assembly.AddPart(plate, plate_body, "plate");
    const int pin_part = assembly.AddPart(pin, pin_body, "pin");
    const int base = assembly.AddInstance(plate_part, "plate", {}, true);
    Placement upside_down;
    upside_down.position = Vec3d{3.0, -2.0, 6.0};
    upside_down.rotation = Quatd::FromAxisAngle(Vec3d{1, 0, 0}, kPi * 0.9);
    const int post = assembly.AddInstance(pin_part, "pin", upside_down, false);

    Mate concentric;
    concentric.kind = MateKind::Concentric;
    concentric.a = On(base, AxisAt(Vec3d{1.0, 2.0, 0.0}, Vec3d{0, 0, 1}));
    concentric.b = On(post, AxisAt(Vec3d{0.0, 0.0, 0.0}, Vec3d{0, 0, 1}));
    assembly.AddMate(concentric);
    AssemblyReport report;
    CHECK_MESSAGE(assembly.Solve(&report), report.error);
    CHECK(report.degrees_of_freedom == 2);
    // On the axis, whichever way up it ended.
    const Placement &p = assembly.GetInstance(post)->placement;
    const Vec3d on_axis = p.Apply(Vec3d{0.0, 0.0, 0.0});
    CHECK(Near(on_axis.x, 1.0, 1e-7) && Near(on_axis.y, 2.0, 1e-7));
    CHECK(Near(std::fabs(p.rotation.Rotate(Vec3d{0, 0, 1}).z), 1.0, 1e-7));
    std::printf("concentric: a pin started upside down still lands on the axis (%.6f, %.6f)\n",
                on_axis.x, on_axis.y);
}

void TestOverAndUnderConstrained() {
    EntityId body = kNoEntity;
    const Model box = BoxPart(Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 2.0, 2.0}, &body);
    std::string error;
    Datum right;
    Datum left;
    CHECK_MESSAGE(DatumFromFace(box, FaceFacing(box, body, Vec3d{1, 0, 0}), &right, &error), error);
    CHECK_MESSAGE(DatumFromFace(box, FaceFacing(box, body, Vec3d{-1, 0, 0}), &left, &error), error);

    // The same mate twice. Redundant, consistent, and not an error: the
    // rank is what counts, so the degree-of-freedom count is unchanged
    // and nothing is reported.
    {
        Assembly assembly;
        const int part = assembly.AddPart(box, body, "cube");
        const int fixed = assembly.AddInstance(part, "fixed", {}, true);
        const int free = assembly.AddInstance(part, "free", {}, false);
        for (int i = 0; i < 2; ++i) {
            Mate mate;
            mate.kind = MateKind::Coincident;
            mate.a = On(fixed, right);
            mate.b = On(free, left);
            assembly.AddMate(mate);
        }
        AssemblyReport report;
        CHECK_MESSAGE(assembly.Solve(&report), report.error);
        CHECK(report.degrees_of_freedom == 3);
        CHECK(report.unsatisfied.empty());
        std::printf("the same mate twice: still 3 degrees of freedom, nothing reported\n");
    }

    // Two mates that cannot both hold: the same faces asked for two
    // different gaps. The solver splits the difference, and the report
    // names both, because which one to give up is not its decision.
    {
        Assembly assembly;
        const int part = assembly.AddPart(box, body, "cube");
        const int fixed = assembly.AddInstance(part, "fixed", {}, true);
        const int free = assembly.AddInstance(part, "free", {}, false);
        Mate near_mate;
        near_mate.kind = MateKind::Offset;
        near_mate.a = On(fixed, right);
        near_mate.b = On(free, left);
        near_mate.value = 1.0;
        const int a = assembly.AddMate(near_mate);
        Mate far_mate = near_mate;
        far_mate.value = 4.0;
        const int b = assembly.AddMate(far_mate);
        AssemblyReport report;
        CHECK(!assembly.Solve(&report));
        std::printf("two gaps at once: %s\n", report.error.c_str());
        CHECK(report.unsatisfied.size() == 2);
        CHECK(report.unsatisfied[0] == a && report.unsatisfied[1] == b);
        // Least squares, so it sits halfway between what the two asked
        // for: one wanted the face at x = 3, the other at x = 6.
        CHECK(Near(assembly.GetInstance(free)->placement.position.x, 4.5, 1e-6));
        CHECK(Near(report.residual, 1.5, 1e-6));
    }

    // Nothing grounded: the mates are still satisfiable, but the whole
    // assembly can be picked up and moved, so six degrees of freedom
    // survive that no amount of mating removes.
    {
        Assembly assembly;
        const int part = assembly.AddPart(box, body, "cube");
        const int one = assembly.AddInstance(part, "one", {}, false);
        Placement guess;
        guess.position = Vec3d{5.0, 1.0, 0.0};
        const int two = assembly.AddInstance(part, "two", guess, false);
        Mate mate;
        mate.kind = MateKind::Coincident;
        mate.a = On(one, right);
        mate.b = On(two, left);
        assembly.AddMate(mate);
        AssemblyReport report;
        CHECK_MESSAGE(assembly.Solve(&report), report.error);
        CHECK(report.degrees_of_freedom == 9);
        std::printf("nothing grounded: 9 degrees of freedom, being 3 from the mate and 6 for the "
                    "whole assembly\n");
    }

    // A mate naming an instance that is not there is refused rather than
    // quietly skipped.
    {
        Assembly assembly;
        const int part = assembly.AddPart(box, body, "cube");
        const int one = assembly.AddInstance(part, "one", {}, true);
        Mate mate;
        mate.kind = MateKind::Coincident;
        mate.a = On(one, right);
        mate.b = On(999, left);
        assembly.AddMate(mate);
        AssemblyReport report;
        CHECK(!assembly.Solve(&report));
        CHECK(report.error.find("not here") != std::string::npos);
    }
}

// --- Sub-assemblies ------------------------------------------------------

// A sub-assembly is rigid from outside: its own mates are solved within
// it, and the parent places it as one piece. The saving is in the
// parameters -- six for the whole sub-assembly rather than six for every
// part in it -- and the degree-of-freedom count is where that shows.
void TestSubAssembly() {
    EntityId cube_body = kNoEntity;
    const Model cube = BoxPart(Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 2.0, 2.0}, &cube_body);
    std::string error;
    Datum right;
    Datum left;
    CHECK_MESSAGE(DatumFromFace(cube, FaceFacing(cube, cube_body, Vec3d{1, 0, 0}), &right, &error), error);
    CHECK_MESSAGE(DatumFromFace(cube, FaceFacing(cube, cube_body, Vec3d{-1, 0, 0}), &left, &error), error);
    Datum top;
    Datum bottom;
    CHECK_MESSAGE(DatumFromFace(cube, FaceFacing(cube, cube_body, Vec3d{0, 0, 1}), &top, &error), error);
    CHECK_MESSAGE(DatumFromFace(cube, FaceFacing(cube, cube_body, Vec3d{0, 0, -1}), &bottom, &error),
                  error);

    // A pair: two cubes side by side, fully pinned to each other.
    auto make_pair = [&]() {
        auto pair = std::make_shared<Assembly>();
        const int part = pair->AddPart(cube, cube_body, "cube");
        const int first = pair->AddInstance(part, "first", {}, true);
        Placement guess;
        guess.position = Vec3d{4.0, 1.0, 1.0};
        const int second = pair->AddInstance(part, "second", guess, false);
        Mate side;
        side.kind = MateKind::Coincident;
        side.a = On(first, right);
        side.b = On(second, left);
        pair->AddMate(side);
        Mate level;
        level.kind = MateKind::Coincident;
        level.flip = true;
        level.a = On(first, bottom);
        level.b = On(second, bottom);
        pair->AddMate(level);
        Mate aligned;
        aligned.kind = MateKind::PointOnPoint;
        aligned.a = On(first, PointAt(Vec3d{2.0, 0.0, 0.0}));
        aligned.b = On(second, PointAt(Vec3d{0.0, 0.0, 0.0}));
        pair->AddMate(aligned);
        return pair;
    };

    auto pair = make_pair();
    AssemblyReport inner;
    CHECK_MESSAGE(pair->Solve(&inner), inner.error);
    CHECK(inner.degrees_of_freedom == 0);
    Model pair_flat;
    std::vector<EntityId> pair_bodies;
    CHECK_MESSAGE(pair->Flatten(&pair_flat, &pair_bodies, &error), error);
    CHECK(pair_bodies.size() == 2);
    CHECK(Near(BodyExtent(pair_flat, pair_bodies[1]).x.lo, 2.0, 1e-7));

    // Now two of those pairs, stacked. Four cubes, but the outer solve
    // carries one instance's six parameters, not two.
    Assembly outer;
    const int pair_part = outer.AddSubAssembly(make_pair(), "pair");
    const int lower = outer.AddInstance(pair_part, "lower", {}, true);
    Placement guess;
    guess.position = Vec3d{-3.0, 5.0, 9.0};
    guess.rotation = Quatd::FromAxisAngle(Vec3d{1.0, 0.3, 0.2}, 0.9);
    const int upper = outer.AddInstance(pair_part, "upper", guess, false);

    Mate stacked;
    stacked.kind = MateKind::Coincident;
    stacked.a = On(lower, top);
    stacked.b = On(upper, bottom);
    outer.AddMate(stacked);
    Mate ends;
    ends.kind = MateKind::PointOnPoint;
    ends.a = On(lower, PointAt(Vec3d{0.0, 0.0, 2.0}));
    ends.b = On(upper, PointAt(Vec3d{0.0, 0.0, 0.0}));
    outer.AddMate(ends);
    Mate square;
    square.kind = MateKind::Parallel;
    square.a = On(lower, AxisAt(Vec3d{}, Vec3d{1, 0, 0}));
    square.b = On(upper, AxisAt(Vec3d{}, Vec3d{1, 0, 0}));
    outer.AddMate(square);

    AssemblyReport report;
    CHECK_MESSAGE(outer.SolveDeep(&report), report.error);
    std::printf("two pairs stacked: %d degrees of freedom over one instance's six\n",
                report.degrees_of_freedom);
    CHECK(report.degrees_of_freedom == 0);

    // Flattened, the nesting is gone: four cubes, each placed once, in a
    // 4 by 2 by 4 block. A transform applied twice -- once inside and
    // once outside -- would put them somewhere else entirely.
    Model flat;
    std::vector<EntityId> bodies;
    CHECK_MESSAGE(outer.Flatten(&flat, &bodies, &error), error);
    CHECK(bodies.size() == 4);
    double total = 0.0;
    Box3d whole;
    for (EntityId b : bodies) {
        total += SolidVolume(flat, b);
        const Box3d extent = BodyExtent(flat, b);
        whole.Expand(Vec3d{extent.x.lo, extent.y.lo, extent.z.lo});
        whole.Expand(Vec3d{extent.x.hi, extent.y.hi, extent.z.hi});
    }
    std::printf("  flattened to four cubes, %.6f, filling %.1f x %.1f x %.1f\n", total, whole.x.Width(),
                whole.y.Width(), whole.z.Width());
    CHECK(Near(total, 32.0, 1e-9));
    CHECK(Near(whole.x.Width(), 4.0, 1e-7));
    CHECK(Near(whole.y.Width(), 2.0, 1e-7));
    CHECK(Near(whole.z.Width(), 4.0, 1e-7));

    // Three levels deep, to check the transforms compose rather than
    // accumulate: a pair of pairs of pairs is eight cubes and nothing
    // placed twice.
    auto stack_of = [&](std::shared_ptr<Assembly> inner_assembly, double height) {
        auto out = std::make_shared<Assembly>();
        const int part = out->AddSubAssembly(std::move(inner_assembly), "level");
        const int a = out->AddInstance(part, "a", {}, true);
        Placement up;
        up.position = Vec3d{0.0, 0.0, height};
        out->AddInstance(part, "b", up, true);
        (void)a;
        return out;
    };
    auto two = stack_of(make_pair(), 2.0);
    auto four = stack_of(two, 4.0);
    Assembly deep;
    const int deep_part = deep.AddSubAssembly(four, "four");
    deep.AddInstance(deep_part, "all", {}, true);
    AssemblyReport deep_report;
    CHECK_MESSAGE(deep.SolveDeep(&deep_report), deep_report.error);
    Model deep_flat;
    std::vector<EntityId> deep_bodies;
    CHECK_MESSAGE(deep.Flatten(&deep_flat, &deep_bodies, &error), error);
    CHECK(deep_bodies.size() == 8);
    double deep_total = 0.0;
    Box3d deep_extent;
    for (EntityId b : deep_bodies) {
        deep_total += SolidVolume(deep_flat, b);
        const Box3d e = BodyExtent(deep_flat, b);
        deep_extent.Expand(Vec3d{e.x.lo, e.y.lo, e.z.lo});
        deep_extent.Expand(Vec3d{e.x.hi, e.y.hi, e.z.hi});
    }
    CHECK(Near(deep_total, 64.0, 1e-9));
    CHECK(Near(deep_extent.z.Width(), 8.0, 1e-7));
    std::printf("  three levels deep: eight cubes over a height of %.1f\n", deep_extent.z.Width());
}

// --- Mates that survive a rebuild ---------------------------------------

// A mate that remembers only the plane it was made on is wrong the moment
// the part changes thickness. One that remembers the *face* is still
// right, and Part B.4 is what turns the name back into a face.
void TestMatesSurviveARebuild() {
    auto plate_tree = [](double thickness) {
        auto tree = std::make_shared<FeatureTree>();
        Sketch sketch(PlaneFromNormal(Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 1.0}));
        const SketchId a = sketch.AddPoint(Vec2d{0.0, 0.0});
        const SketchId b = sketch.AddPoint(Vec2d{10.0, 0.0});
        const SketchId c = sketch.AddPoint(Vec2d{10.0, 6.0});
        const SketchId d = sketch.AddPoint(Vec2d{0.0, 6.0});
        sketch.AddLineFromPoints(a, b);
        sketch.AddLineFromPoints(b, c);
        sketch.AddLineFromPoints(c, d);
        sketch.AddLineFromPoints(d, a);
        const int sketch_id = tree->AddSketch(sketch, "outline");
        Feature extrude;
        extrude.kind = FeatureKind::Extrude;
        extrude.name = "plate";
        extrude.sketch = sketch_id;
        extrude.distance = thickness;
        tree->AddFeature(extrude);
        std::string error;
        Check(tree->Rebuild(&error), "tree->Rebuild", __LINE__);
        return tree;
    };

    auto lower_tree = plate_tree(1.0);
    auto upper_tree = plate_tree(1.0);

    Assembly assembly;
    const int lower_part = assembly.AddPartFromTree(lower_tree, "lower plate");
    const int upper_part = assembly.AddPartFromTree(upper_tree, "upper plate");
    const int lower = assembly.AddInstance(lower_part, "lower", {}, true);
    Placement guess;
    guess.position = Vec3d{2.0, 1.0, 5.0};
    const int upper = assembly.AddInstance(upper_part, "upper", guess, false);

    const Model &lower_model = assembly.GetPart(lower_part)->model;
    const Model &upper_model = assembly.GetPart(upper_part)->model;
    const EntityId lower_body = assembly.GetPart(lower_part)->body;
    const EntityId upper_body = assembly.GetPart(upper_part)->body;

    std::string error;
    Mate flush;
    flush.kind = MateKind::Coincident;
    flush.name = "upper sits on lower";
    CHECK_MESSAGE(CaptureMateEnd(lower_model, FaceFacing(lower_model, lower_body, Vec3d{0, 0, 1}), false,
                                 lower, -1, "", &flush.a, &error),
                  error);
    CHECK_MESSAGE(CaptureMateEnd(upper_model, FaceFacing(upper_model, upper_body, Vec3d{0, 0, -1}), false,
                                 upper, -1, "", &flush.b, &error),
                  error);
    assembly.AddMate(flush);

    // The other two mates line the plates up sideways, and they are named
    // too. Side faces are the right thing to use here: they do not move
    // when the plate is thickened, so this test is about the *top* face
    // following, with everything else held still.
    Mate along_x;
    along_x.kind = MateKind::Coincident;
    along_x.flip = true;  // both face the same way, so their planes coincide
    CHECK_MESSAGE(CaptureMateEnd(lower_model, FaceFacing(lower_model, lower_body, Vec3d{0, -1, 0}), false,
                                 lower, -1, "", &along_x.a, &error),
                  error);
    CHECK_MESSAGE(CaptureMateEnd(upper_model, FaceFacing(upper_model, upper_body, Vec3d{0, -1, 0}), false,
                                 upper, -1, "", &along_x.b, &error),
                  error);
    assembly.AddMate(along_x);

    Mate along_y;
    along_y.kind = MateKind::Coincident;
    along_y.flip = true;
    CHECK_MESSAGE(CaptureMateEnd(lower_model, FaceFacing(lower_model, lower_body, Vec3d{1, 0, 0}), false,
                                 lower, -1, "", &along_y.a, &error),
                  error);
    CHECK_MESSAGE(CaptureMateEnd(upper_model, FaceFacing(upper_model, upper_body, Vec3d{1, 0, 0}), false,
                                 upper, -1, "", &along_y.b, &error),
                  error);
    assembly.AddMate(along_y);

    AssemblyReport report;
    CHECK_MESSAGE(assembly.Solve(&report), report.error);
    CHECK(Near(assembly.GetInstance(upper)->placement.position.z, 1.0, 1e-7));
    std::printf("plates 1 thick: the upper sits at z = %.6f\n",
                assembly.GetInstance(upper)->placement.position.z);

    // Now thicken the lower plate. Its top face is somewhere else
    // entirely, and the mate has to follow it there.
    lower_tree->GetMutable(lower_tree->Features().back().id)->distance = 4.0;
    std::vector<int> lost;
    CHECK_MESSAGE(assembly.RebuildParts(&lost, &error), error);
    CHECK(lost.empty());
    CHECK_MESSAGE(assembly.Solve(&report), report.error);
    CHECK(report.lost.empty());
    std::printf("after thickening the lower plate to 4: the upper sits at z = %.6f\n",
                assembly.GetInstance(upper)->placement.position.z);
    CHECK(Near(assembly.GetInstance(upper)->placement.position.z, 4.0, 1e-7));

    // And again, to a different number, because following once could be
    // luck.
    lower_tree->GetMutable(lower_tree->Features().back().id)->distance = 2.5;
    CHECK_MESSAGE(assembly.RebuildParts(&lost, &error), error);
    CHECK_MESSAGE(assembly.Solve(&report), report.error);
    CHECK(Near(assembly.GetInstance(upper)->placement.position.z, 2.5, 1e-7));
    CHECK(report.degrees_of_freedom == 0);

    // The flattened stack really is that tall, which is the check that
    // the part was rebuilt and not merely re-measured.
    Model flat;
    std::vector<EntityId> bodies;
    CHECK_MESSAGE(assembly.Flatten(&flat, &bodies, &error), error);
    CHECK(Near(SolidVolume(flat, bodies[0]), 10.0 * 6.0 * 2.5, 1e-9));
    CHECK(Near(BodyExtent(flat, bodies[1]).z.hi, 3.5, 1e-7));
    std::printf("  and the rebuilt stack is %.1f tall, of which %.1f is the lower plate\n",
                BodyExtent(flat, bodies[1]).z.hi, 2.5);

    // AND THE OTHER HALF OF THE CLAIM. A mate written against a
    // coordinate rather than against a face does *not* follow, and the
    // failure is exactly the kind that would otherwise go unnoticed: the
    // assembly still solves, just to the wrong answer. Here it is made
    // to show itself by putting the two in contradiction, which is the
    // only way a solver can tell you that one of them is stale.
    Mate stale;
    stale.kind = MateKind::PointOnPoint;
    stale.name = "written against a coordinate";
    stale.a = On(lower, PointAt(Vec3d{0.0, 0.0, 1.0}));  // the old thickness
    stale.b = On(upper, PointAt(Vec3d{0.0, 0.0, 0.0}));
    const int stale_id = assembly.AddMate(stale);
    CHECK(!assembly.Solve(&report));
    std::printf("  a mate written against z = 1 instead of against the face: %s\n", report.error.c_str());
    CHECK(std::find(report.unsatisfied.begin(), report.unsatisfied.end(), stale_id) !=
          report.unsatisfied.end());
    CHECK(assembly.RemoveMate(stale_id));

    // A reference that really is gone is reported as lost, not as
    // unsatisfied -- a different problem with a different fix. Pointing a
    // mate at a face of a model that no longer has anything like it does
    // that.
    // Worth noting what does *not* make a reference lost: Part B.4 scores
    // on several signals at once, and mislabelling one of them is not
    // enough -- a name with the wrong role and the wrong surface kind
    // still resolves, because the sample point, the direction and the
    // neighbourhood all still agree. That is the design working, and it
    // is why this uses a reference to something genuinely not there: a
    // cylindrical face, named on a cylinder, looked for on a flat plate.
    EntityId tube_body = kNoEntity;
    const Model tube = CylinderPart(2.0, 5.0, &tube_body);
    EntityId curved = kNoEntity;
    for (EntityId shell_id : tube.GetBody(tube_body)->shells) {
        for (EntityId face_id : tube.GetShell(shell_id)->faces) {
            if (dynamic_cast<const CylinderSurface *>(tube.SurfaceAt(tube.GetFace(face_id)->surface))) {
                curved = face_id;
            }
        }
    }
    CHECK(curved != kNoEntity);
    Mate orphan;
    orphan.kind = MateKind::Coincident;
    CHECK_MESSAGE(CaptureMateEnd(tube, curved, false, lower, -1, "", &orphan.a, &error), error);
    orphan.b = On(upper, PointAt(Vec3d{}));
    const int orphan_id = assembly.AddMate(orphan);
    std::vector<int> orphan_lost;
    CHECK_MESSAGE(assembly.ResolveNamedMates(&orphan_lost, &error), error);
    CHECK(orphan_lost.size() == 1 && orphan_lost[0] == orphan_id);
    std::printf("  and a reference that is genuinely gone is reported lost, not unsatisfied\n");
}

// --- A whole assembly ----------------------------------------------------

void TestThreePartAssembly() {
    // A base, a bracket standing on it at a right angle, and a pin
    // through both. Three parts, four mates, nothing left loose.
    EntityId base_body = kNoEntity;
    const Model base = BoxPart(Vec3d{0.0, 0.0, 0.0}, Vec3d{12.0, 8.0, 1.0}, &base_body);
    EntityId bracket_body = kNoEntity;
    const Model bracket = BoxPart(Vec3d{0.0, 0.0, 0.0}, Vec3d{6.0, 1.0, 5.0}, &bracket_body);
    EntityId pin_body = kNoEntity;
    const Model pin = CylinderPart(0.4, 3.0, &pin_body);

    std::string error;
    Datum base_top;
    CHECK_MESSAGE(DatumFromFace(base, FaceFacing(base, base_body, Vec3d{0, 0, 1}), &base_top, &error),
                  error);
    Datum bracket_bottom;
    CHECK_MESSAGE(
        DatumFromFace(bracket, FaceFacing(bracket, bracket_body, Vec3d{0, 0, -1}), &bracket_bottom, &error),
        error);
    Datum bracket_back;
    CHECK_MESSAGE(
        DatumFromFace(bracket, FaceFacing(bracket, bracket_body, Vec3d{0, -1, 0}), &bracket_back, &error),
        error);
    Datum base_back;
    CHECK_MESSAGE(DatumFromFace(base, FaceFacing(base, base_body, Vec3d{0, -1, 0}), &base_back, &error),
                  error);

    Assembly assembly;
    const int base_part = assembly.AddPart(base, base_body, "base");
    const int bracket_part = assembly.AddPart(bracket, bracket_body, "bracket");
    const int pin_part = assembly.AddPart(pin, pin_body, "pin");
    const int the_base = assembly.AddInstance(base_part, "base", {}, true);
    Placement bracket_guess;
    bracket_guess.position = Vec3d{-2.0, 5.0, 9.0};
    bracket_guess.rotation = Quatd::FromAxisAngle(Vec3d{0.3, 1.0, -0.2}, 1.3);
    const int the_bracket = assembly.AddInstance(bracket_part, "bracket", bracket_guess, false);

    // The bracket stands on the base, its back flush with the base's back,
    // and its left edge 3 along.
    Mate standing;
    standing.kind = MateKind::Coincident;
    standing.name = "bracket on base";
    standing.a = On(the_base, base_top);
    standing.b = On(the_bracket, bracket_bottom);
    assembly.AddMate(standing);

    Mate flush_back;
    flush_back.kind = MateKind::Coincident;
    flush_back.name = "backs flush";
    flush_back.flip = true;  // both face the same way, not towards each other
    flush_back.a = On(the_base, base_back);
    flush_back.b = On(the_bracket, bracket_back);
    assembly.AddMate(flush_back);

    Mate along;
    along.kind = MateKind::Offset;
    along.name = "3 along";
    Datum base_left;
    CHECK_MESSAGE(DatumFromFace(base, FaceFacing(base, base_body, Vec3d{-1, 0, 0}), &base_left, &error),
                  error);
    Datum bracket_left;
    CHECK_MESSAGE(
        DatumFromFace(bracket, FaceFacing(bracket, bracket_body, Vec3d{-1, 0, 0}), &bracket_left, &error),
        error);
    along.flip = true;
    along.a = On(the_base, base_left);
    along.b = On(the_bracket, bracket_left);
    along.value = -3.0;  // measured along the base's outward -x normal
    assembly.AddMate(along);

    AssemblyReport report;
    CHECK_MESSAGE(assembly.Solve(&report), report.error);
    // Nothing is loose yet -- the pin is not in the assembly until its
    // own mates go in below, and an instance with no mates is six
    // degrees of freedom whatever else is pinned down.
    CHECK(report.degrees_of_freedom == 0);
    const Placement &placed = assembly.GetInstance(the_bracket)->placement;
    CHECK(NearVec(placed.position, Vec3d{3.0, 0.0, 1.0}, 1e-7));
    CHECK(NearVec(placed.rotation.Rotate(Vec3d{1, 0, 0}), Vec3d{1, 0, 0}, 1e-7));
    std::printf("bracket on base: placed at (%.6f, %.6f, %.6f), 0 degrees of freedom\n",
                placed.position.x, placed.position.y, placed.position.z);

    // The pin goes through a hole in the bracket, sitting on the base.
    Placement pin_guess;
    pin_guess.position = Vec3d{9.0, 9.0, 9.0};
    const int the_pin = assembly.AddInstance(pin_part, "pin", pin_guess, false);
    Mate pin_axis;
    pin_axis.kind = MateKind::Concentric;
    pin_axis.a = On(the_bracket, AxisAt(Vec3d{3.0, 0.5, 3.0}, Vec3d{0, 1, 0}));
    pin_axis.b = On(the_pin, AxisAt(Vec3d{0.0, 0.0, 0.0}, Vec3d{0, 0, 1}));
    assembly.AddMate(pin_axis);
    Mate pin_depth;
    pin_depth.kind = MateKind::PointOnPoint;
    pin_depth.a = On(the_bracket, PointAt(Vec3d{3.0, -1.0, 3.0}));
    pin_depth.b = On(the_pin, PointAt(Vec3d{0.0, 0.0, 0.0}));
    assembly.AddMate(pin_depth);
    CHECK_MESSAGE(assembly.Solve(&report), report.error);
    // The pin's own spin about its axis is still free; everything else
    // is pinned.
    std::printf("  with the pin: %d degrees of freedom left (its own spin)\n",
                report.degrees_of_freedom);
    CHECK(report.degrees_of_freedom == 1);
    const Vec3d pin_base = assembly.GetInstance(the_pin)->placement.Apply(Vec3d{0, 0, 0});
    CHECK(NearVec(pin_base, Vec3d{6.0, -1.0, 4.0}, 1e-7));

    Model flat;
    std::vector<EntityId> bodies;
    CHECK_MESSAGE(assembly.Flatten(&flat, &bodies, &error), error);
    CHECK(bodies.size() == 3);
    double total = 0.0;
    for (EntityId b : bodies) {
        const double v = SolidVolume(flat, b);
        CHECK(v > 0.0);
        total += v;
    }
    const double expected = 12.0 * 8.0 * 1.0 + 6.0 * 1.0 * 5.0 + kPi * 0.16 * 3.0;
    std::printf("  flattened: %.4f against %.4f (the pin is tessellated, so it reads a little under)\n",
                total, expected);
    CHECK(total < expected);
    CHECK(total > expected - 0.05);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestQuaternion();
    TestDatums();
    TestDegreesOfFreedom();
    TestOffsetAndDistance();
    TestAngleAndParallel();
    TestConcentricEitherWayRound();
    TestOverAndUnderConstrained();
    TestThreePartAssembly();
    TestSubAssembly();
    TestMatesSurviveARebuild();
    std::printf("cad_assembly_test passed (%d checks)\n", g_checks);
    return 0;
}
