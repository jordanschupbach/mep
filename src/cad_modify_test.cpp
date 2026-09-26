// Windowless coverage for cad_modify.h (plans/CAD_FEM_PLAN.md Part E.4).
//
// EVERY ANSWER HERE IS EXACT, and that is the reason this part is
// restricted to planar faces before anything else. A polyhedron
// tessellates without approximation, so a volume can be checked to 1e-9
// rather than to whatever the chording happens to allow -- and the
// closed forms are available:
//
//   offset      a box by d            (a+2d)(b+2d)(c+2d)
//   offset      a prism by d          (A + P d + d^2 sum tan(ext/2)) (h + 2d)
//   draft       a box by angle t      a b h - tan(t)(a+b)h^2 + (4/3)tan(t)^2 h^3
//   shell       a box, sealed         a b c - (a-2t)(b-2t)(c-2t)
//   shell       a box, top opened     a b c - (a-2t)(b-2t)(c-t)
//
// The offset of a non-convex prism is the one worth spelling out, since
// it is the test that a reflex corner is handled rather than merely
// survived: with the corners re-solved as plane meetings -- mitred, not
// rounded -- the area grows by A + P d + d^2 sum over corners of
// tan(exterior/2), and a reflex corner contributes a *negative* term. An
// L has five square corners and one reflex, so that sum is 5 - 1 = 4, and
// a construction that treated the reflex corner like the others would
// come out 2 d^2 too large.

#include "cad_modify.h"

#include "cad_feature.h"
#include "cad_pcurve.h"
#include "cad_tessellate.h"
#include "cad_validate.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// Validated, tessellated, watertight, and only then measured. A NaN if
// any of that fails, so a caller comparing against an expected value
// fails rather than passing on a zero.
double SolidVolume(const Model &model, EntityId body) {
    ValidationReport report;
    if (!ValidateBody(model, body, &report, {})) {
        std::fprintf(stderr, "  invalid body:\n%s\n", report.Summary().c_str());
        return std::nan("");
    }
    TessellationMesh mesh;
    std::string error;
    if (!TessellateBody(model, body, {}, &mesh, &error)) {
        std::fprintf(stderr, "  could not tessellate: %s\n", error.c_str());
        return std::nan("");
    }
    if (!mesh.IsClosed()) {
        std::fprintf(stderr, "  tessellation is not watertight\n");
        return std::nan("");
    }
    return mesh.SignedVolume();
}

int FaceCount(const Model &model, EntityId body) {
    const Body *b = model.GetBody(body);
    if (b == nullptr) return 0;
    int count = 0;
    for (EntityId shell_id : b->shells) {
        const Shell *shell = model.GetShell(shell_id);
        if (shell != nullptr) count += static_cast<int>(shell->faces.size());
    }
    return count;
}

// The body's own extent, from its corners. Model::Bounds unions the
// *surfaces*, which are trimmed a hair wider than their faces so that a
// p-curve cannot land exactly on a domain edge -- close enough for a
// viewport and not close enough for a check written to 1e-12.
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

// The face of `body` whose outward normal points along `direction`.
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

SketchPlane AxisAlignedPlane(double z) {
    SketchPlane plane = PlaneFromNormal(Vec3d{0.0, 0.0, z}, Vec3d{0.0, 0.0, 1.0});
    plane.x_axis = Vec3d{1.0, 0.0, 0.0};
    plane.y_axis = Vec3d{0.0, 1.0, 0.0};
    return plane;
}

// The L-shaped prism used for the reflex-corner tests: 4 by 4 overall,
// 1 thick in each arm, extruded `height`.
bool MakeEll(double height, Model *model, EntityId *body, std::string *error) {
    Sketch sketch(AxisAlignedPlane(0.0));
    const SketchId a = sketch.AddPoint(Vec2d{0.0, 0.0});
    const SketchId b = sketch.AddPoint(Vec2d{4.0, 0.0});
    const SketchId c = sketch.AddPoint(Vec2d{4.0, 1.0});
    const SketchId d = sketch.AddPoint(Vec2d{1.0, 1.0});
    const SketchId e = sketch.AddPoint(Vec2d{1.0, 4.0});
    const SketchId f = sketch.AddPoint(Vec2d{0.0, 4.0});
    sketch.AddLineFromPoints(a, b);
    sketch.AddLineFromPoints(b, c);
    sketch.AddLineFromPoints(c, d);
    sketch.AddLineFromPoints(d, e);
    sketch.AddLineFromPoints(e, f);
    sketch.AddLineFromPoints(f, a);
    Sketch::Profile profile;
    if (!SelectProfile(sketch, -1, &profile, error)) return false;
    return ExtrudeProfile(sketch, profile, Vec3d{0.0, 0.0, 1.0}, height, model, body, error);
}

// --- Offset ------------------------------------------------------------

void TestOffsetBox() {
    std::printf("offset a box:\n");
    for (double d : {-0.9, -0.5, -0.1, 0.25, 1.0, 3.0}) {
        Model box;
        EntityId body = kNoEntity;
        CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 3.0, 4.0}, &box, &body));
        std::string error;
        CHECK(BuildAllPCurves(&box, {}, &error));
        Model out;
        EntityId offset = kNoEntity;
        CHECK_MESSAGE(OffsetBody(box, body, d, {}, &out, &offset, &error), error);
        const double expected = (2.0 + 2.0 * d) * (3.0 + 2.0 * d) * (4.0 + 2.0 * d);
        const double volume = SolidVolume(out, offset);
        std::printf("  by %+.2f: %.9f, wanted %.9f\n", d, volume, expected);
        CHECK(Near(volume, expected, 1e-9));
        // Topology untouched: that is the claim the whole approach rests on.
        CHECK(FaceCount(out, offset) == 6);
        CHECK(out.CountsOfBody(offset).vertices == 8);
        CHECK(out.CountsOfBody(offset).edges == 12);
    }
}

// The reflex corner. Mitred, so the area picks up d^2 per square corner
// and *loses* d^2 at the reflex one.
void TestOffsetReflex() {
    const double area = 4.0 * 1.0 + 3.0 * 1.0;
    const double perimeter = 4.0 + 1.0 + 3.0 + 3.0 + 1.0 + 4.0;
    for (double d : {-0.4, -0.2, 0.3, 0.75, 1.5}) {
        Model model;
        EntityId body = kNoEntity;
        std::string error;
        CHECK_MESSAGE(MakeEll(2.0, &model, &body, &error), error);
        Model out;
        EntityId offset = kNoEntity;
        CHECK_MESSAGE(OffsetBody(model, body, d, {}, &out, &offset, &error), error);
        // Five square corners contribute +d^2 each, the reflex one -d^2.
        const double section = area + perimeter * d + 4.0 * d * d;
        const double expected = section * (2.0 + 2.0 * d);
        const double volume = SolidVolume(out, offset);
        CHECK(Near(volume, expected, 1e-9));
    }
    std::printf("offset an L-prism: the reflex corner subtracts its d^2, at five offsets\n");

    // And the sign of that term is checked on its own, because getting it
    // wrong is the plausible mistake and it is worth failing loudly.
    Model model;
    EntityId body = kNoEntity;
    std::string error;
    CHECK_MESSAGE(MakeEll(2.0, &model, &body, &error), error);
    Model out;
    EntityId offset = kNoEntity;
    CHECK_MESSAGE(OffsetBody(model, body, 1.0, {}, &out, &offset, &error), error);
    const double treating_all_corners_alike = (area + perimeter + 6.0) * 4.0;
    CHECK(!Near(SolidVolume(out, offset), treating_all_corners_alike, 1e-6));
}

void TestOffsetRefusals() {
    Model box;
    EntityId body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 3.0, 4.0}, &box, &body));
    std::string error;
    CHECK(BuildAllPCurves(&box, {}, &error));
    Model out;
    EntityId offset = kNoEntity;

    // Half of the smallest dimension is exactly where the box closes up.
    CHECK(!OffsetBody(box, body, -1.0, {}, &out, &offset, &error));
    std::printf("offset inwards past the middle is refused: %s\n", error.c_str());
    CHECK(error.find("passed through each other") != std::string::npos ||
          error.find("collapsed") != std::string::npos);
    CHECK(!OffsetBody(box, body, -2.5, {}, &out, &offset, &error));
    CHECK(!error.empty());
    CHECK(!OffsetBody(box, body, 0.0, {}, &out, &offset, &error));
    CHECK(!error.empty());

    // A curved face is refused by name rather than approximated.
    Model tube;
    EntityId tube_body = kNoEntity;
    CHECK(MakeCylinder(Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 1.0}, 2.0, 5.0, &tube, &tube_body));
    CHECK(BuildAllPCurves(&tube, {}, &error));
    CHECK(!OffsetBody(tube, tube_body, 0.5, {}, &out, &offset, &error));
    std::printf("a curved face is refused: %s\n", error.c_str());
    CHECK(error.find("not a plane") != std::string::npos);
}

// --- Draft --------------------------------------------------------------

void TestDraft() {
    const double a = 4.0;
    const double b = 6.0;
    const double h = 3.0;
    std::printf("draft the four sides of a box about its base:\n");
    for (double degrees : {1.0, 5.0, 12.0, 20.0}) {
        Model box;
        EntityId body = kNoEntity;
        CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{a, b, h}, &box, &body));
        std::string error;
        CHECK(BuildAllPCurves(&box, {}, &error));
        const std::vector<EntityId> sides = {
            FaceFacing(box, body, Vec3d{1.0, 0.0, 0.0}), FaceFacing(box, body, Vec3d{-1.0, 0.0, 0.0}),
            FaceFacing(box, body, Vec3d{0.0, 1.0, 0.0}), FaceFacing(box, body, Vec3d{0.0, -1.0, 0.0})};
        for (EntityId f : sides) CHECK(f != kNoEntity);

        const double angle = DegToRad(degrees);
        Model out;
        EntityId drafted = kNoEntity;
        CHECK_MESSAGE(DraftFaces(box, body, sides, Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 1.0}, angle, {},
                                 &out, &drafted, &error),
                      error);
        // Each section at height z is (a - 2 z tan t) by (b - 2 z tan t),
        // so the volume integrates to this. The base keeps its size --
        // that is what the neutral plane is for -- and the top shrinks.
        const double t = std::tan(angle);
        const double expected = a * b * h - t * (a + b) * h * h + 4.0 / 3.0 * t * t * h * h * h;
        const double volume = SolidVolume(out, drafted);
        std::printf("  %4.1f degrees: %.9f, wanted %.9f\n", degrees, volume, expected);
        CHECK(Near(volume, expected, 1e-9));
        CHECK(FaceCount(out, drafted) == 6);
        // The base is untouched and the top has shrunk by exactly the
        // taper -- which is the statement that the draft turned about the
        // neutral plane and not about anything else.
        const Box3d bounds = BodyExtent(out, drafted);
        CHECK(Near(bounds.x.lo, 0.0, 1e-12) && Near(bounds.x.hi, a, 1e-12));
        CHECK(Near(bounds.y.lo, 0.0, 1e-12) && Near(bounds.y.hi, b, 1e-12));
        CHECK(Near(bounds.z.lo, 0.0, 1e-12) && Near(bounds.z.hi, h, 1e-12));
    }

    // A negative angle flares instead of tapering, and the same formula
    // says so.
    Model box;
    EntityId body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{a, b, h}, &box, &body));
    std::string error;
    CHECK(BuildAllPCurves(&box, {}, &error));
    const std::vector<EntityId> sides = {
        FaceFacing(box, body, Vec3d{1.0, 0.0, 0.0}), FaceFacing(box, body, Vec3d{-1.0, 0.0, 0.0}),
        FaceFacing(box, body, Vec3d{0.0, 1.0, 0.0}), FaceFacing(box, body, Vec3d{0.0, -1.0, 0.0})};
    Model out;
    EntityId drafted = kNoEntity;
    const double angle = DegToRad(-8.0);
    CHECK_MESSAGE(DraftFaces(box, body, sides, Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 1.0}, angle, {}, &out,
                             &drafted, &error),
                  error);
    const double t = std::tan(angle);
    const double expected = a * b * h - t * (a + b) * h * h + 4.0 / 3.0 * t * t * h * h * h;
    CHECK(Near(SolidVolume(out, drafted), expected, 1e-9));
    CHECK(SolidVolume(out, drafted) > a * b * h);
    std::printf("  and -8 degrees flares it, to %.6f\n", SolidVolume(out, drafted));

    // Drafting one face only tapers one side, so the answer is the
    // trapezoidal prism -- a different formula, which is the point.
    Model single;
    EntityId single_body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{a, b, h}, &single, &single_body));
    CHECK(BuildAllPCurves(&single, {}, &error));
    const EntityId one = FaceFacing(single, single_body, Vec3d{1.0, 0.0, 0.0});
    Model one_out;
    EntityId one_drafted = kNoEntity;
    const double small = DegToRad(10.0);
    CHECK_MESSAGE(DraftFaces(single, single_body, {one}, Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 1.0}, small,
                             {}, &one_out, &one_drafted, &error),
                  error);
    const double one_expected = b * (a * h - 0.5 * std::tan(small) * h * h);
    CHECK(Near(SolidVolume(one_out, one_drafted), one_expected, 1e-9));
    std::printf("  one face alone gives the trapezoidal prism, %.9f\n",
                SolidVolume(one_out, one_drafted));
}

void TestDraftRefusals() {
    Model box;
    EntityId body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, 6.0, 3.0}, &box, &body));
    std::string error;
    CHECK(BuildAllPCurves(&box, {}, &error));
    Model out;
    EntityId drafted = kNoEntity;

    // A face square to the pull direction has no hinge to turn about.
    const EntityId top = FaceFacing(box, body, Vec3d{0.0, 0.0, 1.0});
    CHECK(!DraftFaces(box, body, {top}, Vec3d{}, Vec3d{0.0, 0.0, 1.0}, DegToRad(5.0), {}, &out, &drafted,
                      &error));
    std::printf("drafting a face that faces the pull is refused: %s\n", error.c_str());
    CHECK(error.find("hinge") != std::string::npos);

    // Far enough and the top closes to a point, then past itself.
    const std::vector<EntityId> sides = {
        FaceFacing(box, body, Vec3d{1.0, 0.0, 0.0}), FaceFacing(box, body, Vec3d{-1.0, 0.0, 0.0}),
        FaceFacing(box, body, Vec3d{0.0, 1.0, 0.0}), FaceFacing(box, body, Vec3d{0.0, -1.0, 0.0})};
    CHECK(!DraftFaces(box, body, sides, Vec3d{}, Vec3d{0.0, 0.0, 1.0}, DegToRad(60.0), {}, &out, &drafted,
                      &error));
    std::printf("drafting past where the top closes up is refused: %s\n", error.c_str());
    CHECK(!DraftFaces(box, body, {}, Vec3d{}, Vec3d{0.0, 0.0, 1.0}, DegToRad(5.0), {}, &out, &drafted,
                      &error));
}

// --- Shell and thicken ---------------------------------------------------

void TestThicken() {
    const double a = 4.0;
    const double b = 6.0;
    const double c = 5.0;
    std::printf("hollow a box, sealed:\n");
    for (double t : {0.2, 0.5, 1.0, 1.9}) {
        Model box;
        EntityId body = kNoEntity;
        CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{a, b, c}, &box, &body));
        std::string error;
        CHECK(BuildAllPCurves(&box, {}, &error));
        Model out;
        EntityId shelled = kNoEntity;
        CHECK_MESSAGE(ShellBody(box, body, {}, t, {}, &out, &shelled, &error), error);
        const double expected = a * b * c - (a - 2 * t) * (b - 2 * t) * (c - 2 * t);
        const double volume = SolidVolume(out, shelled);
        std::printf("  wall %.1f: %.9f, wanted %.9f\n", t, volume, expected);
        CHECK(Near(volume, expected, 1e-9));
        // Twelve faces in two shells, the inner one a void.
        CHECK(FaceCount(out, shelled) == 12);
        const Body *solid = out.GetBody(shelled);
        CHECK(solid->shells.size() == 2);
        CHECK(out.GetShell(solid->shells[0])->is_outer);
        CHECK(!out.GetShell(solid->shells[1])->is_outer);
        // The outside is untouched: hollowing takes nothing off the
        // outside, which a construction that offset the whole body
        // instead of carrying a void would get wrong.
        const Box3d bounds = BodyExtent(out, shelled);
        CHECK(Near(bounds.x.hi - bounds.x.lo, a, 1e-12));
        CHECK(Near(bounds.z.hi - bounds.z.lo, c, 1e-12));
    }
}

void TestShellOpenFace() {
    const double a = 4.0;
    const double b = 6.0;
    const double c = 5.0;
    std::printf("shell a box with the top opened:\n");
    for (double t : {0.25, 0.75, 1.5}) {
        Model box;
        EntityId body = kNoEntity;
        CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{a, b, c}, &box, &body));
        std::string error;
        CHECK(BuildAllPCurves(&box, {}, &error));
        const EntityId top = FaceFacing(box, body, Vec3d{0.0, 0.0, 1.0});
        CHECK(top != kNoEntity);
        Model out;
        EntityId shelled = kNoEntity;
        CHECK_MESSAGE(ShellBody(box, body, {top}, t, {}, &out, &shelled, &error), error);
        // The cavity runs all the way up to the opened face rather than
        // stopping a wall short of it, so its height is c - t, not c - 2t.
        const double expected = a * b * c - (a - 2 * t) * (b - 2 * t) * (c - t);
        const double volume = SolidVolume(out, shelled);
        std::printf("  wall %.2f: %.9f, wanted %.9f\n", t, volume, expected);
        CHECK(Near(volume, expected, 1e-9));
        // Five outer faces, five inner, and the rim: one shell, eleven
        // faces, and the rim is the only one with a hole in it.
        CHECK(FaceCount(out, shelled) == 11);
        CHECK(out.GetBody(shelled)->shells.size() == 1);
        int with_holes = 0;
        for (EntityId f : out.GetShell(out.GetBody(shelled)->shells[0])->faces) {
            if (out.GetFace(f)->loops.size() > 1) ++with_holes;
        }
        CHECK(with_holes == 1);
    }

    // Two faces opened: a tube, open at both ends.
    Model box;
    EntityId body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{a, b, c}, &box, &body));
    std::string error;
    CHECK(BuildAllPCurves(&box, {}, &error));
    const double t = 0.5;
    Model out;
    EntityId shelled = kNoEntity;
    CHECK_MESSAGE(ShellBody(box, body,
                            {FaceFacing(box, body, Vec3d{0.0, 0.0, 1.0}),
                             FaceFacing(box, body, Vec3d{0.0, 0.0, -1.0})},
                            t, {}, &out, &shelled, &error),
                  error);
    const double expected = a * b * c - (a - 2 * t) * (b - 2 * t) * c;
    std::printf("  open at both ends: %.9f, wanted %.9f\n", SolidVolume(out, shelled), expected);
    CHECK(Near(SolidVolume(out, shelled), expected, 1e-9));
    CHECK(FaceCount(out, shelled) == 10);
}

void TestShellReflexAndRefusals() {
    // An L again, hollowed: the reflex corner's inner wall is the one
    // that closes up first, so this is where a wall that is too thick
    // shows itself.
    Model model;
    EntityId body = kNoEntity;
    std::string error;
    CHECK_MESSAGE(MakeEll(3.0, &model, &body, &error), error);
    Model out;
    EntityId shelled = kNoEntity;
    const double t = 0.3;
    CHECK_MESSAGE(ShellBody(model, body, {}, t, {}, &out, &shelled, &error), error);
    const double area = 7.0;
    const double perimeter = 16.0;
    const double inner_section = area - perimeter * t + 4.0 * t * t;
    const double expected = area * 3.0 - inner_section * (3.0 - 2.0 * t);
    std::printf("hollow an L-prism: %.9f, wanted %.9f\n", SolidVolume(out, shelled), expected);
    CHECK(Near(SolidVolume(out, shelled), expected, 1e-9));

    // The arms are 1 thick, so a wall of 0.5 leaves nothing.
    CHECK(!ShellBody(model, body, {}, 0.6, {}, &out, &shelled, &error));
    std::printf("a wall thicker than the part is refused: %s\n", error.c_str());
    CHECK(!error.empty());
    CHECK(!ShellBody(model, body, {}, -1.0, {}, &out, &shelled, &error));
    CHECK(!ShellBody(model, body, {999}, 0.2, {}, &out, &shelled, &error));
    CHECK(error.find("not in this body") != std::string::npos);

    // A hollow body carries its cavity as a second shell, and re-solving
    // one of those means re-solving both. That is the same routine run
    // twice and is not wired up, so a sealed hollow body fed back in is
    // refused by name rather than quietly losing its cavity -- which is
    // what a version that only looked at the first shell would do.
    Model box;
    EntityId box_body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, 4.0, 4.0}, &box, &box_body));
    CHECK(BuildAllPCurves(&box, {}, &error));
    Model hollow;
    EntityId hollow_body = kNoEntity;
    CHECK_MESSAGE(ShellBody(box, box_body, {}, 0.5, {}, &hollow, &hollow_body, &error), error);
    Model again;
    EntityId again_body = kNoEntity;
    CHECK(!OffsetBody(hollow, hollow_body, 0.1, {}, &again, &again_body, &error));
    std::printf("offsetting an already-hollow body is refused: %s\n", error.c_str());
    CHECK(error.find("one shell") != std::string::npos);
    // But one opened at a face is a single shell, so that one composes.
    const EntityId lid = FaceFacing(box, box_body, Vec3d{0.0, 0.0, 1.0});
    Model open_box;
    EntityId open_body = kNoEntity;
    CHECK_MESSAGE(ShellBody(box, box_body, {lid}, 0.5, {}, &open_box, &open_body, &error), error);
    Model grown;
    EntityId grown_body = kNoEntity;
    CHECK_MESSAGE(OffsetBody(open_box, open_body, 0.25, {}, &grown, &grown_body, &error), error);
    CHECK(SolidVolume(grown, grown_body) > SolidVolume(open_box, open_body));
    std::printf("an opened shell is one shell, so it offsets: %.6f from %.6f\n",
                SolidVolume(grown, grown_body), SolidVolume(open_box, open_body));
}

// --- The operations compose ---------------------------------------------

void TestCompose() {
    // Draft, then shell, then offset: each reads the body the one before
    // it produced, which is the only real test that these leave behind an
    // ordinary body rather than something only their own code can read.
    Model box;
    EntityId body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{6.0, 6.0, 4.0}, &box, &body));
    std::string error;
    CHECK(BuildAllPCurves(&box, {}, &error));
    const std::vector<EntityId> sides = {
        FaceFacing(box, body, Vec3d{1.0, 0.0, 0.0}), FaceFacing(box, body, Vec3d{-1.0, 0.0, 0.0}),
        FaceFacing(box, body, Vec3d{0.0, 1.0, 0.0}), FaceFacing(box, body, Vec3d{0.0, -1.0, 0.0})};
    const double angle = DegToRad(6.0);
    Model drafted;
    EntityId drafted_body = kNoEntity;
    CHECK_MESSAGE(DraftFaces(box, body, sides, Vec3d{}, Vec3d{0.0, 0.0, 1.0}, angle, {}, &drafted,
                             &drafted_body, &error),
                  error);
    const double t = std::tan(angle);
    const double after_draft = 36.0 * 4.0 - t * 12.0 * 16.0 + 4.0 / 3.0 * t * t * 64.0;
    CHECK(Near(SolidVolume(drafted, drafted_body), after_draft, 1e-9));

    const EntityId top = FaceFacing(drafted, drafted_body, Vec3d{0.0, 0.0, 1.0});
    CHECK(top != kNoEntity);
    Model shelled;
    EntityId shelled_body = kNoEntity;
    CHECK_MESSAGE(ShellBody(drafted, drafted_body, {top}, 0.4, {}, &shelled, &shelled_body, &error),
                  error);
    // Exactly, not just "smaller": the cavity is the drafted box with
    // every kept face moved in along its own normal -- so its sides move
    // by w/cos(t), not by w -- running from the raised floor up to the
    // opened top, which makes it a frustum of known size.
    const double wall = 0.4;
    const double floor_at = wall;
    const double inset = 2.0 * wall / std::cos(angle);
    const double bottom_side = 6.0 - inset - 2.0 * floor_at * t;
    const double top_side = 6.0 - inset - 2.0 * 4.0 * t;
    const double cavity = (4.0 - floor_at) / 3.0 *
                          (bottom_side * bottom_side + top_side * top_side + bottom_side * top_side);
    const double hollow = SolidVolume(shelled, shelled_body);
    std::printf("drafted then shelled: %.9f, wanted %.9f\n", hollow, after_draft - cavity);
    CHECK(Near(hollow, after_draft - cavity, 1e-9));
    CHECK(FaceCount(shelled, shelled_body) == 11);

    // And offsetting the drafted box grows it by the right amount --
    // checked against the same integral with each face moved out by d
    // along its own normal, which for a tapered side means the section
    // grows by 2d/cos(t) rather than by 2d.
    const double d = 0.3;
    Model grown;
    EntityId grown_body = kNoEntity;
    CHECK_MESSAGE(OffsetBody(drafted, drafted_body, d, {}, &grown, &grown_body, &error), error);
    // Offsetting a tapered side moves its plane along its own normal, so
    // the section grows by 2d/cos(t) rather than by 2d -- and the base
    // drops by d, where the taper has not yet narrowed it, which is worth
    // another 2 d tan(t). Leaving that second term out is an easy mistake
    // and costs 2% here, which is why the check is written to 1e-9.
    const double height = 4.0 + 2.0 * d;
    const double grown_bottom = 6.0 + 2.0 * d / std::cos(angle) + 2.0 * d * t;
    const double grown_top = grown_bottom - 2.0 * t * height;
    const double expected =
        height / 3.0 * (grown_bottom * grown_bottom + grown_top * grown_top + grown_bottom * grown_top);
    std::printf("drafted then offset: %.9f, wanted %.9f\n", SolidVolume(grown, grown_body), expected);
    CHECK(Near(SolidVolume(grown, grown_body), expected, 1e-9));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestOffsetBox();
    TestOffsetReflex();
    TestOffsetRefusals();
    TestDraft();
    TestDraftRefusals();
    TestThicken();
    TestShellOpenFace();
    TestShellReflexAndRefusals();
    TestCompose();
    std::printf("cad_modify_test passed (%d checks)\n", g_checks);
    return 0;
}
