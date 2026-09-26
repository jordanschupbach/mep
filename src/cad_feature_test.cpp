// Windowless coverage for cad_feature.h (plans/CAD_FEM_PLAN.md Parts E.1
// and E.2).
//
// Two oracles, and the split between them is the point.
//
//   EVERY BODY IS CHECKED, not just measured. A feature that builds a
//   solid inside out, or one whose faces are sewn to the wrong
//   neighbours, can still have a plausible volume -- so each result goes
//   through Part B.3's validator and is tessellated and checked
//   watertight before its volume is looked at. The volume is then
//   compared against a closed form: a box is its sides multiplied, a
//   frustum is h/3 * (A1 + A2 + sqrt(A1*A2)), a quarter tube is a
//   quarter of the whole one.
//
//   THE SIGN OF THE VOLUME IS ITSELF A TEST. A solid built with its
//   orientation conventions backwards is perfectly valid, perfectly
//   watertight, and has a negative volume -- which is how the revolve's
//   sweep direction was found to depend on which side of the axis the
//   profile sits, rather than on the sign of the angle alone.
//
// The tree is checked by changing it: a part is built, a dimension is
// altered, and the rebuild has to produce the part the new dimension
// describes. That is the only thing a feature tree is for, and a tree
// that builds correctly once and does not follow a change is no better
// than the solid it produced.

#include "cad_feature.h"

#include "cad_naming.h"
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

// The same, but printing whatever the operation put in `error` first --
// a blend that refuses is much easier to fix when its own sentence is on
// screen rather than just the line number that noticed.
void CheckMessage(bool condition, const char *expression, int line, const std::string &message) {
    if (!condition && !message.empty()) std::fprintf(stderr, "  reported: %s\n", message.c_str());
    Check(condition, expression, line);
}
#define CHECK_MESSAGE(condition, message) CheckMessage((condition), #condition, __LINE__, (message))

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// A body's volume, having first insisted that it is a body at all.
// Returns a NaN if it is not, so a caller comparing against an expected
// value fails rather than passing on a zero.
double SolidVolume(const Model &model, EntityId body, const TessellationOptions &options = {}) {
    ValidationReport report;
    if (!ValidateBody(model, body, &report, {})) {
        std::fprintf(stderr, "  invalid body:\n%s\n", report.Summary().c_str());
        return std::nan("");
    }
    TessellationMesh mesh;
    std::string error;
    if (!TessellateBody(model, body, options, &mesh, &error)) {
        std::fprintf(stderr, "  could not tessellate: %s\n", error.c_str());
        return std::nan("");
    }
    if (!mesh.IsClosed()) {
        std::fprintf(stderr, "  tessellation is not watertight\n");
        return std::nan("");
    }
    return mesh.SignedVolume();
}


// A curved blend face is tessellated *inside* the true surface, so a
// fillet's mesh always reads a little under the closed form, and what a
// fillet removes is small enough that the default chording is a fifth of
// the answer. This is fine enough that the deficit is about a five
// hundredth of what a fillet takes away, which is what makes the bound
// below a sharp check rather than a formality, and no finer -- the
// triangulator is quadratic in the number of points, so the setting is
// chosen where the check stops getting usefully sharper rather than
// where it stops being affordable.
TessellationOptions FineMesh() {
    TessellationOptions options;
    options.chord_tolerance = 3e-4;
    options.angle_tolerance = 0.02;
    options.max_triangles_per_face = 200000;
    return options;
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

void Rectangle(Sketch *sketch, const Vec2d &lo, const Vec2d &hi, bool construction = false) {
    const SketchId a = sketch->AddPoint(lo);
    const SketchId b = sketch->AddPoint(Vec2d{hi.x, lo.y});
    const SketchId c = sketch->AddPoint(hi);
    const SketchId d = sketch->AddPoint(Vec2d{lo.x, hi.y});
    sketch->AddLineFromPoints(a, b, construction);
    sketch->AddLineFromPoints(b, c, construction);
    sketch->AddLineFromPoints(c, d, construction);
    sketch->AddLineFromPoints(d, a, construction);
}

SketchPlane AxisAlignedPlane(double z) {
    SketchPlane plane = PlaneFromNormal(Vec3d{0.0, 0.0, z}, Vec3d{0.0, 0.0, 1.0});
    // Pinned rather than taken from PlaneFromNormal's own choice, so that
    // a sketch's x and y are the world's and the expected numbers below
    // can be written down.
    plane.x_axis = Vec3d{1.0, 0.0, 0.0};
    plane.y_axis = Vec3d{0.0, 1.0, 0.0};
    return plane;
}

// --- E.2: extrude ------------------------------------------------------

void TestExtrude() {
    std::printf("extrude\n");

    // A square. Exact, because nothing here is curved: every face is
    // planar and the tessellation is the solid.
    {
        Sketch sketch;
        Rectangle(&sketch, Vec2d{0.0, 0.0}, Vec2d{4.0, 4.0});
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        CHECK(profile.outer.size() == 4);
        CHECK(profile.holes.empty());
        Model model;
        EntityId body = kNoEntity;
        CHECK(ExtrudeProfile(sketch, profile, Vec3d{0.0, 0.0, 1.0}, 3.0, &model, &body, &error));
        CHECK(FaceCount(model, body) == 6);
        CHECK(Near(SolidVolume(model, body), 48.0, 1e-9));
    }

    // The same square extruded the other way. The near and far caps swap,
    // and with them every orientation in the construction -- a solid
    // built without allowing for that comes out inside out, valid, and
    // with a volume of exactly the wrong sign.
    {
        Sketch sketch;
        Rectangle(&sketch, Vec2d{0.0, 0.0}, Vec2d{4.0, 4.0});
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        Model model;
        EntityId body = kNoEntity;
        CHECK(ExtrudeProfile(sketch, profile, Vec3d{0.0, 0.0, -1.0}, 3.0, &model, &body, &error));
        const double volume = SolidVolume(model, body);
        std::printf("  extruded against the plane normal: %+.9f\n", volume);
        CHECK(Near(volume, 48.0, 1e-9));
    }

    // A circle. The profile comes back as two arcs -- the arrangement
    // cuts every closed curve in half so that it has two endpoints -- and
    // the construction has to put it back together as one face with a
    // seam rather than two half-faces, which is what a cylinder is.
    {
        Sketch sketch;
        sketch.AddCircle(Vec2d{0.0, 0.0}, 2.0);
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        CHECK(profile.outer.size() == 2);
        Model model;
        EntityId body = kNoEntity;
        CHECK(ExtrudeProfile(sketch, profile, Vec3d{0.0, 0.0, 1.0}, 5.0, &model, &body, &error));
        // Three faces, as MakeCylinder builds: one seamed tube and two
        // caps. Four would mean the circle was left in two halves.
        CHECK(FaceCount(model, body) == 3);
        const double exact = kPi * 4.0 * 5.0;
        const double volume = SolidVolume(model, body);
        std::printf("  circle extruded: %.6f (exactly %.6f)\n", volume, exact);
        // Below the exact value: the tessellation inscribes the circle.
        CHECK(volume > 0.0 && volume < exact);
        CHECK(Near(volume, exact, exact * 0.01));
    }

    // A plate with a hole. The hole's loop runs the other way round, and
    // a construction that does not notice adds its volume instead of
    // taking it away.
    {
        Sketch sketch;
        Rectangle(&sketch, Vec2d{0.0, 0.0}, Vec2d{6.0, 6.0});
        sketch.AddCircle(Vec2d{3.0, 3.0}, 1.5);
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        CHECK(profile.holes.size() == 1);
        Model model;
        EntityId body = kNoEntity;
        CHECK(ExtrudeProfile(sketch, profile, Vec3d{0.0, 0.0, 1.0}, 2.0, &model, &body, &error));
        // Two caps, four sides, one seamed tube for the hole.
        CHECK(FaceCount(model, body) == 7);
        const double exact = (36.0 - kPi * 2.25) * 2.0;
        const double volume = SolidVolume(model, body);
        std::printf("  plate with a hole: %.6f (exactly %.6f)\n", volume, exact);
        // Above, this time: an inscribed hole is smaller than the real one.
        CHECK(volume > exact);
        CHECK(Near(volume, exact, exact * 0.01));
    }

    // Refusals.
    {
        Sketch sketch;
        Rectangle(&sketch, Vec2d{0.0, 0.0}, Vec2d{4.0, 4.0});
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        Model model;
        EntityId body = kNoEntity;
        CHECK(!ExtrudeProfile(sketch, profile, Vec3d{0.0, 0.0, 1.0}, 0.0, &model, &body, &error));
        CHECK(!error.empty());
        CHECK(!ExtrudeProfile(sketch, profile, Vec3d{1.0, 0.0, 0.0}, 3.0, &model, &body, &error));
        CHECK(error.find("sketch plane") != std::string::npos);
    }
    {
        Sketch empty;
        Sketch::Profile profile;
        std::string error;
        CHECK(!SelectProfile(empty, -1, &profile, &error));
        CHECK(error.find("no closed region") != std::string::npos);
    }
}

// --- E.2: revolve ------------------------------------------------------

void TestRevolve() {
    std::printf("revolve\n");

    // A rectangle offset from the axis, turned a quarter: a quarter of a
    // tube, whose volume is a quarter of pi*(R^2 - r^2)*h.
    {
        Sketch sketch;
        Rectangle(&sketch, Vec2d{2.0, 0.0}, Vec2d{3.0, 4.0});
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        Model model;
        EntityId body = kNoEntity;
        CHECK(RevolveProfile(sketch, profile, Vec2d{0.0, 0.0}, Vec2d{0.0, 1.0}, kHalfPi, &model, &body,
                             &error));
        // Four swept sides and two flat ends.
        CHECK(FaceCount(model, body) == 6);
        const double exact = kPi * (9.0 - 4.0) * 4.0 * 0.25;
        const double volume = SolidVolume(model, body);
        std::printf("  quarter tube: %.6f (exactly %.6f)\n", volume, exact);
        CHECK(volume > 0.0);
        CHECK(Near(volume, exact, exact * 0.01));
    }

    // The same, all the way round. No flat ends at all now: each swept
    // face closes on itself and carries a seam instead.
    {
        Sketch sketch;
        Rectangle(&sketch, Vec2d{2.0, 0.0}, Vec2d{3.0, 4.0});
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        Model model;
        EntityId body = kNoEntity;
        CHECK(RevolveProfile(sketch, profile, Vec2d{0.0, 0.0}, Vec2d{0.0, 1.0}, kTwoPi, &model, &body,
                             &error));
        CHECK(FaceCount(model, body) == 4);
        const double exact = kPi * (9.0 - 4.0) * 4.0;
        const double volume = SolidVolume(model, body);
        std::printf("  whole tube: %.6f (exactly %.6f)\n", volume, exact);
        CHECK(volume > 0.0);
        CHECK(Near(volume, exact, exact * 0.01));
    }

    // Mirrored across the axis. The sweep sets off the other way, so
    // every orientation flips -- this is the case that showed the sweep
    // direction cannot be read off the angle's sign alone.
    {
        Sketch sketch;
        Rectangle(&sketch, Vec2d{-3.0, 0.0}, Vec2d{-2.0, 4.0});
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        Model model;
        EntityId body = kNoEntity;
        CHECK(RevolveProfile(sketch, profile, Vec2d{0.0, 0.0}, Vec2d{0.0, 1.0}, kHalfPi, &model, &body,
                             &error));
        const double exact = kPi * (9.0 - 4.0) * 4.0 * 0.25;
        const double volume = SolidVolume(model, body);
        std::printf("  mirrored quarter tube: %+.6f (exactly %.6f)\n", volume, exact);
        CHECK(volume > 0.0);
        CHECK(Near(volume, exact, exact * 0.01));
    }

    // A profile crossing its own axis sweeps through itself. Refused,
    // because the result is not a solid and every later operation would
    // have to cope with that.
    {
        Sketch sketch;
        Rectangle(&sketch, Vec2d{-1.0, 0.0}, Vec2d{3.0, 4.0});
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        Model model;
        EntityId body = kNoEntity;
        CHECK(!RevolveProfile(sketch, profile, Vec2d{0.0, 0.0}, Vec2d{0.0, 1.0}, kPi, &model, &body, &error));
        CHECK(error.find("crosses its own axis") != std::string::npos);
    }
}

// --- E.2: loft ---------------------------------------------------------

void TestLoft() {
    std::printf("loft\n");
    {
        Sketch lower;
        Rectangle(&lower, Vec2d{-2.0, -2.0}, Vec2d{2.0, 2.0});
        Sketch upper(AxisAlignedPlane(5.0));
        Rectangle(&upper, Vec2d{-1.0, -1.0}, Vec2d{1.0, 1.0});
        Sketch::Profile a;
        Sketch::Profile b;
        std::string error;
        CHECK(SelectProfile(lower, -1, &a, &error));
        CHECK(SelectProfile(upper, -1, &b, &error));
        Model model;
        EntityId body = kNoEntity;
        CHECK(LoftProfiles({&lower, &upper}, {a, b}, &model, &body, &error));
        CHECK(FaceCount(model, body) == 6);
        // A frustum: h/3 * (A1 + A2 + sqrt(A1*A2)). Exact, since every
        // face is planar.
        const double exact = 5.0 / 3.0 * (16.0 + 4.0 + 8.0);
        const double volume = SolidVolume(model, body);
        std::printf("  square frustum: %.9f (exactly %.9f)\n", volume, exact);
        CHECK(Near(volume, exact, 1e-9));
    }

    // Sections that do not correspond are refused rather than matched by
    // guesswork -- a loft between a triangle and a circle is a real
    // feature and a much larger one, and guessing produces a twisted,
    // self-intersecting result that still validates.
    {
        Sketch lower;
        Rectangle(&lower, Vec2d{-2.0, -2.0}, Vec2d{2.0, 2.0});
        Sketch upper(AxisAlignedPlane(5.0));
        upper.AddCircle(Vec2d{0.0, 0.0}, 1.0);
        Sketch::Profile a;
        Sketch::Profile b;
        std::string error;
        CHECK(SelectProfile(lower, -1, &a, &error));
        CHECK(SelectProfile(upper, -1, &b, &error));
        Model model;
        EntityId body = kNoEntity;
        CHECK(!LoftProfiles({&lower, &upper}, {a, b}, &model, &body, &error));
        CHECK(error.find("same number of boundary pieces") != std::string::npos);
    }
}

// --- E.1: the tree -----------------------------------------------------

void TestTree() {
    std::printf("feature tree\n");
    FeatureTree tree;

    // A 10 x 6 plate, 2 thick.
    Sketch base;
    Rectangle(&base, Vec2d{0.0, 0.0}, Vec2d{10.0, 6.0});
    const int base_sketch = tree.AddSketch(base, "base");
    Feature plate;
    plate.kind = FeatureKind::Extrude;
    plate.name = "plate";
    plate.sketch = base_sketch;
    plate.distance = 2.0;
    const int plate_id = tree.AddFeature(plate);

    // A hole through it, sketched on the top face's plane and cut all
    // the way -- so that it follows the plate's thickness rather than
    // being a pocket of its own depth.
    Sketch hole_sketch(AxisAlignedPlane(2.0));
    hole_sketch.AddCircle(Vec2d{3.0, 3.0}, 1.0);
    const int hole_sketch_id = tree.AddSketch(hole_sketch, "hole sketch");
    Feature hole;
    hole.kind = FeatureKind::Extrude;
    hole.name = "hole";
    hole.sketch = hole_sketch_id;
    hole.distance = 4.0;
    hole.reverse = true;
    hole.end = ExtrudeEnd::ThroughAll;
    hole.combine = FeatureCombine::Cut;
    const int hole_id = tree.AddFeature(hole);

    std::string error;
    if (!tree.Rebuild(&error)) std::printf("  TREE REBUILD: %s\n", error.substr(0,400).c_str());
    CHECK(tree.Rebuild(&error));
    CHECK(error.empty());
    CHECK(tree.ResultOf(plate_id)->ok);
    CHECK(tree.ResultOf(hole_id)->ok);
    CHECK(Near(tree.ResultOf(plate_id)->volume, 120.0, 1e-9));
    const double drilled = 120.0 - kPi * 2.0;
    std::printf("  plate with a through hole: %.6f (exactly %.6f)\n", tree.Volume(), drilled);
    CHECK(Near(tree.Volume(), drilled, drilled * 0.01));

    // The parametric change: thicken the plate and rebuild. The hole is a
    // separate feature that knows nothing about the plate's thickness,
    // and it has to end up going all the way through anyway. This is the
    // whole reason the tree exists rather than the solid.
    tree.GetMutable(plate_id)->distance = 5.0;
    CHECK(tree.Rebuild(&error));
    const double thicker = 300.0 - kPi * 5.0;
    std::printf("  after thickening to 5: %.6f (exactly %.6f)\n", tree.Volume(), thicker);
    CHECK(Near(tree.Volume(), thicker, thicker * 0.01));

    // A boss added on top, poking out past the plate.
    Sketch boss_sketch(AxisAlignedPlane(2.0));
    Rectangle(&boss_sketch, Vec2d{7.0, 1.0}, Vec2d{9.0, 5.0});
    const int boss_sketch_id = tree.AddSketch(boss_sketch, "boss sketch");
    Feature boss;
    boss.kind = FeatureKind::Extrude;
    boss.name = "boss";
    boss.sketch = boss_sketch_id;
    boss.distance = 4.0;
    boss.combine = FeatureCombine::Add;
    const int boss_id = tree.AddFeature(boss);
    CHECK(tree.Rebuild(&error));
    // The boss spans z from 2 to 6 and the plate 0 to 5, so it adds only
    // the millimetre above the plate's own top.
    const double with_boss = thicker + 2.0 * 4.0 * 1.0;
    std::printf("  after a boss: %.6f (exactly %.6f)\n", tree.Volume(), with_boss);
    CHECK(Near(tree.Volume(), with_boss, with_boss * 0.01));

    // Suppressing a feature takes it out without removing it, and
    // un-suppressing puts it back exactly.
    tree.GetMutable(boss_id)->suppressed = true;
    CHECK(tree.Rebuild(&error));
    CHECK(Near(tree.Volume(), thicker, thicker * 0.01));
    tree.GetMutable(boss_id)->suppressed = false;
    CHECK(tree.Rebuild(&error));
    CHECK(Near(tree.Volume(), with_boss, with_boss * 0.01));

    // A feature that cannot build is reported and the rest of the tree is
    // still there. Stopping at the first failure would mean a part with
    // three broken features took three rebuilds to understand.
    Sketch nothing;
    const int empty_sketch = tree.AddSketch(nothing, "empty");
    Feature broken;
    broken.kind = FeatureKind::Extrude;
    broken.name = "broken";
    broken.sketch = empty_sketch;
    broken.distance = 1.0;
    const int broken_id = tree.AddFeature(broken);
    CHECK(!tree.Rebuild(&error));
    CHECK(error.find("broken") != std::string::npos);
    CHECK(!tree.ResultOf(broken_id)->ok);
    CHECK(tree.ResultOf(broken_id)->error.find("no closed region") != std::string::npos);
    // Everything before it survived, and so did the geometry.
    CHECK(tree.ResultOf(plate_id)->ok);
    CHECK(tree.ResultOf(hole_id)->ok);
    CHECK(tree.ResultOf(boss_id)->ok);
    CHECK(Near(tree.Volume(), with_boss, with_boss * 0.01));
    std::printf("  a broken feature is reported, and the %zu before it still built\n",
                tree.Features().size() - 1);

    // Removing it puts the tree back in order.
    CHECK(tree.RemoveFeature(broken_id));
    CHECK(tree.RemoveFeature(empty_sketch));
    CHECK(tree.Rebuild(&error));
    CHECK(error.empty());
}

// A second body rather than a combined one, which is what a multi-body
// part is, and the refusal when there is nothing to combine with.
void TestMultiBody() {
    std::printf("multi-body\n");
    FeatureTree tree;
    Sketch first;
    Rectangle(&first, Vec2d{0.0, 0.0}, Vec2d{2.0, 2.0});
    const int first_sketch = tree.AddSketch(first, "first");
    Feature a;
    a.kind = FeatureKind::Extrude;
    a.name = "a";
    a.sketch = first_sketch;
    a.distance = 2.0;
    tree.AddFeature(a);

    Sketch second;
    Rectangle(&second, Vec2d{10.0, 10.0}, Vec2d{13.0, 13.0});
    const int second_sketch = tree.AddSketch(second, "second");
    Feature b;
    b.kind = FeatureKind::Extrude;
    b.name = "b";
    b.sketch = second_sketch;
    b.distance = 2.0;
    b.combine = FeatureCombine::NewBody;
    const int b_id = tree.AddFeature(b);

    std::string error;
    CHECK(tree.Rebuild(&error));
    CHECK(tree.Bodies().size() == 2);
    CHECK(Near(tree.Volume(), 8.0 + 18.0, 1e-9));
    std::printf("  two bodies totalling %.6f\n", tree.Volume());

    // A cut with nothing to cut is refused rather than quietly becoming a
    // new body, which is almost never what was meant.
    FeatureTree lonely;
    Sketch only;
    Rectangle(&only, Vec2d{0.0, 0.0}, Vec2d{2.0, 2.0});
    const int only_sketch = lonely.AddSketch(only, "only");
    Feature cut;
    cut.kind = FeatureKind::Extrude;
    cut.name = "cut";
    cut.sketch = only_sketch;
    cut.distance = 2.0;
    cut.combine = FeatureCombine::Cut;
    lonely.AddFeature(cut);
    CHECK(!lonely.Rebuild(&error));
    CHECK(error.find("no body to combine with") != std::string::npos);
    (void)b_id;
}

// An intersect, and a revolve driven through the tree by a construction
// line -- the axis stays parametric, so moving the line moves the axis.
void TestRevolveFeatureAndIntersect() {
    std::printf("revolve feature and intersect\n");
    FeatureTree tree;
    Sketch profile;
    Rectangle(&profile, Vec2d{2.0, 0.0}, Vec2d{3.0, 4.0});
    const SketchId axis_start = profile.AddPoint(Vec2d{0.0, 0.0});
    const SketchId axis_end = profile.AddPoint(Vec2d{0.0, 1.0});
    const SketchId axis = profile.AddLineFromPoints(axis_start, axis_end, true);
    const int sketch_id = tree.AddSketch(profile, "profile");
    Feature revolve;
    revolve.kind = FeatureKind::Revolve;
    revolve.name = "tube";
    revolve.sketch = sketch_id;
    revolve.axis_entity = axis;
    revolve.angle = kTwoPi;
    const int revolve_id = tree.AddFeature(revolve);

    std::string error;
    CHECK(tree.Rebuild(&error));
    CHECK(error.empty());
    const double exact = kPi * (9.0 - 4.0) * 4.0;
    std::printf("  revolved tube: %.6f (exactly %.6f)\n", tree.Volume(), exact);
    CHECK(Near(tree.Volume(), exact, exact * 0.01));

    // Half a turn instead, by changing one number.
    tree.GetMutable(revolve_id)->angle = kPi;
    CHECK(tree.Rebuild(&error));
    std::printf("  half a turn: %.6f (exactly %.6f)\n", tree.Volume(), exact * 0.5);
    CHECK(Near(tree.Volume(), exact * 0.5, exact * 0.01));

    // A revolve with no axis line to turn about says so.
    tree.GetMutable(revolve_id)->axis_entity = kNoSketchId;
    CHECK(!tree.Rebuild(&error));
    CHECK(error.find("line in the sketch to turn about") != std::string::npos);
}


// --- E.2: sweep --------------------------------------------------------

void TestSweep() {
    std::printf("sweep\n");

    // A straight spine is an extrude by another name, so the volume is
    // exact and says the frames and the ruled surfaces between them line
    // up with what a plain extrude would build.
    {
        Sketch sketch;
        Rectangle(&sketch, Vec2d{-1.0, -1.0}, Vec2d{1.0, 1.0});
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        const Line3 spine = Line3::FromPoints(Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 6.0});
        Model model;
        EntityId body = kNoEntity;
        CHECK(SweepProfile(sketch, profile, spine, 2, 0.0, 1.0, &model, &body, &error));
        CHECK(Near(SolidVolume(model, body), 24.0, 1e-9));
    }

    // Tapered along a straight spine: a frustum, exact again.
    {
        Sketch sketch;
        Rectangle(&sketch, Vec2d{-1.0, -1.0}, Vec2d{1.0, 1.0});
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        const Line3 spine = Line3::FromPoints(Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 6.0});
        Model model;
        EntityId body = kNoEntity;
        CHECK(SweepProfile(sketch, profile, spine, 2, 0.0, 0.5, &model, &body, &error));
        const double exact = 6.0 / 3.0 * (4.0 + 1.0 + 2.0);
        std::printf("  tapered: %.9f (exactly %.9f)\n", SolidVolume(model, body), exact);
        CHECK(Near(SolidVolume(model, body), exact, 1e-9));
    }

    // A quarter-circle spine. Pappus gives the volume of a solid of
    // revolution-by-sweeping: the section's area times the distance its
    // centroid travels. That is an independent check -- nothing in the
    // construction knows about it.
    {
        Sketch sketch;
        Rectangle(&sketch, Vec2d{-1.0, -1.0}, Vec2d{1.0, 1.0});
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        const Circle3 spine(Vec3d{5.0, 0.0, 0.0}, Vec3d{-1.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 1.0}, 5.0, 0.0,
                            kHalfPi);
        Model model;
        EntityId body = kNoEntity;
        CHECK(SweepProfile(sketch, profile, spine, 24, 0.0, 1.0, &model, &body, &error));
        const double pappus = 4.0 * (kHalfPi * 5.0);
        const double volume = SolidVolume(model, body);
        std::printf("  bent round a quarter circle: %.6f (Pappus %.6f)\n", volume, pappus);
        CHECK(volume > 0.0);
        CHECK(Near(volume, pappus, pappus * 0.01));
    }

    // Twisting about a straight spine moves no area, so Pappus still
    // says the same number -- the sections are chorded by the ruled
    // surfaces between them, which is where the small shortfall comes from.
    {
        Sketch sketch;
        Rectangle(&sketch, Vec2d{-1.0, -1.0}, Vec2d{1.0, 1.0});
        Sketch::Profile profile;
        std::string error;
        CHECK(SelectProfile(sketch, -1, &profile, &error));
        const Line3 spine = Line3::FromPoints(Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 6.0});
        Model model;
        EntityId body = kNoEntity;
        CHECK(SweepProfile(sketch, profile, spine, 24, kHalfPi, 1.0, &model, &body, &error));
        const double volume = SolidVolume(model, body);
        std::printf("  twisted a quarter turn: %.6f (24 less the chording)\n", volume);
        CHECK(volume > 0.0 && volume < 24.0);
        CHECK(Near(volume, 24.0, 24.0 * 0.05));
    }
}

// --- Naming an edge by its two faces -----------------------------------
//
// The capability Part E.3 will need: a reference that still means the
// same edge after the feature before it changes shape. Checked on its
// own, because a direct edge reference is ambiguous on a box and always
// will be -- Part B.4 is right to say so, and the answer is to name
// something it can tell apart.
void TestEdgeNaming() {
    std::printf("naming an edge by its faces\n");
    FeatureTree tree;
    // Not a cube: its faces have to be distinguishable even if its edges
    // are not.
    Sketch base;
    Rectangle(&base, Vec2d{0.0, 0.0}, Vec2d{4.0, 7.0});
    const int sketch_id = tree.AddSketch(base, "base");
    Feature block;
    block.kind = FeatureKind::Extrude;
    block.name = "block";
    block.sketch = sketch_id;
    block.distance = 4.0;
    const int block_id = tree.AddFeature(block);

    std::string error;
    CHECK(tree.Rebuild(&error));
    CHECK(Near(tree.Volume(), 112.0, 1e-9));

    // The vertical edge at the origin corner.
    EntityId edge = kNoEntity;
    for (const Edge &candidate : tree.Result().Edges()) {
        const Curve3 *curve = tree.Result().CurveAt(candidate.curve);
        if (curve == nullptr) continue;
        const Vec3d a = curve->Point(candidate.t_start);
        const Vec3d b = curve->Point(candidate.t_end);
        const Vec3d direction = (b - a).Normalized();
        if (std::fabs(std::fabs(direction.z) - 1.0) > 1e-9) continue;
        if (std::fabs(a.x) < 1e-9 && std::fabs(a.y) < 1e-9) edge = candidate.id;
    }
    CHECK(edge != kNoEntity);
    const Vec3d before = tree.Result().EdgeStartPoint(edge);

    EntityName face_a;
    EntityName face_b;
    CHECK(CaptureEdgeBetweenFaces(tree.Result(), edge, block_id, &face_a, &face_b));

    // Both faces resolve on their own, which is the point: a face has a
    // normal, a role and a set of neighbours to be told apart by, and on
    // a block its edges have far less.
    CHECK(ResolveFace(tree.Result(), face_a).status == ResolveStatus::Resolved);
    CHECK(ResolveFace(tree.Result(), face_b).status == ResolveStatus::Resolved);

    // So the edge between them resolves -- and keeps resolving after the
    // block changes size, when every id in the model is new.
    EntityId found = kNoEntity;
    CHECK(ResolveEdgeBetweenFaces(tree.Result(), tree.Bodies().back(), face_a, face_b, &found, &error));
    CHECK(found == edge);

    tree.GetMutable(block_id)->distance = 10.0;
    CHECK(tree.Rebuild(&error));
    CHECK(Near(tree.Volume(), 280.0, 1e-9));
    CHECK(ResolveEdgeBetweenFaces(tree.Result(), tree.Bodies().back(), face_a, face_b, &found, &error));
    const Vec3d after = tree.Result().EdgeStartPoint(found);
    std::printf("  after the block grows, the same edge is found again at (%.1f, %.1f, %.1f)\n", after.x,
                after.y, after.z);
    CHECK((after - before).Length() < 1e-9);
}


// --- E.3: fillets and chamfers -----------------------------------------
//
// THE ORACLE IS A CLOSED FORM AND THE TEST IS A SWEEP. Rounding one
// straight edge of a prism with a ball of radius r takes away the
// difference between the corner square and the quarter disc inside it,
// which is (1 - pi/4) * r^2 per unit of edge; a chamfer takes away the
// triangle, r^2/2 per unit. Both are exact, so the check is exact.
//
// The sweep over box sizes is not padding. The boolean-cutter attempt
// that this replaced passed on a 4x4x4 box and failed on 4x5x4, 4x6x4,
// 4x7x4 and 4x9x4 while passing on 4x8x4 -- a pattern that only appears
// if you run more than one size, and the only honest reading of which is
// that the one that passed passed by luck. Anything built here gets the
// same sweep for the same reason.

// How much the mesh can read under the closed form, worked out rather
// than guessed. Chording an arc of angle `sweep` into n segments leaves
// (r^2/2)(sweep - n sin(sweep/n)) of area outside the mesh, and the
// tessellator takes at least sweep/angle_tolerance segments, so the
// deficit is at most (r^2/2) * sweep * angle_tolerance^2 / 6 per unit of
// edge. Checking against that bound rather than a round number means a
// fillet that is the wrong size fails even when it is only slightly
// wrong -- and the bound turns out to be tight, which is itself a sign
// that the difference really is the chording and not something else.
double ChordingBound(double radius, double sweep, double length) {
    const TessellationOptions options = FineMesh();
    // The coarsest step either rule allows. The angle rule caps the step
    // directly; the chord rule caps the sagitta, r(1 - cos(step/2)).
    const double by_chord = 2.0 * std::acos(std::max(-1.0, 1.0 - options.chord_tolerance / radius));
    const double step = std::max(options.angle_tolerance, by_chord);
    // sweep - n sin(sweep/n) with n = sweep/step, to leading order, and a
    // tenth over for the rounding of n to a whole number of segments.
    return 1.1 * 0.5 * radius * radius * sweep * step * step / 6.0 * length;
}

// The rolling-ball property, checked against the B-rep itself: the blend
// surface is a cylinder of the radius asked for, its axis runs along the
// edge it replaced, it spans exactly the angle between the two faces,
// and that axis sits exactly `radius` from each of the planes the ball
// was rolled between. None of this goes through the tessellator, so none
// of it is limited by how finely the cylinder happens to be chorded --
// which is what makes it the sharp half of the fillet's verification.
void CheckFilletSurface(const Model &model, EntityId body, double radius, const Vec3d &along, double length,
                        double sweep) {
    EntityId blend = kNoEntity;
    for (EntityId shell_id : model.GetBody(body)->shells) {
        for (EntityId face_id : model.GetShell(shell_id)->faces) {
            if (model.GetFace(face_id)->name == "fillet") blend = face_id;
        }
    }
    CHECK(blend != kNoEntity);
    const Face *face = model.GetFace(blend);
    const auto *cylinder = dynamic_cast<const CylinderSurface *>(model.SurfaceAt(face->surface));
    CHECK(cylinder != nullptr);
    CHECK(Near(cylinder->Radius(), radius, 1e-12));
    CHECK(Near(std::fabs(cylinder->Axis().Dot(along.Normalized())), 1.0, 1e-12));
    double u_lo = 0.0;
    double u_hi = 0.0;
    double v_lo = 0.0;
    double v_hi = 0.0;
    cylinder->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    CHECK(Near(u_hi - u_lo, sweep, 1e-12));
    CHECK(Near(v_hi - v_lo, length, 1e-12));
    CHECK(Near(cylinder->Area(), radius * sweep * length, 1e-9));

    int tangencies = 0;
    for (EntityId edge : model.EdgesOfFace(blend)) {
        const Curve3 *curve = model.CurveAt(model.GetEdge(edge)->curve);
        if (curve == nullptr || curve->Kind() != CurveKind::Line) continue;
        for (EntityId other : model.FacesOfEdge(edge)) {
            if (other == blend) continue;
            const auto *plane =
                dynamic_cast<const PlaneSurface *>(model.SurfaceAt(model.GetFace(other)->surface));
            CHECK(plane != nullptr);
            CHECK(Near(std::fabs(plane->SignedDistance(cylinder->Origin())), radius, 1e-12));
            ++tangencies;
        }
    }
    CHECK(tangencies == 2);
}

EntityId EdgeBetweenPoints(const Model &model, EntityId body, const Vec3d &a, const Vec3d &b) {
    const Body *solid = model.GetBody(body);
    if (solid == nullptr) return kNoEntity;
    for (EntityId shell_id : solid->shells) {
        for (EntityId edge_id : model.EdgesOfShell(shell_id)) {
            const Vec3d p = model.EdgeStartPoint(edge_id);
            const Vec3d q = model.EdgeEndPoint(edge_id);
            if (((p - a).Length() < 1e-9 && (q - b).Length() < 1e-9) ||
                ((p - b).Length() < 1e-9 && (q - a).Length() < 1e-9)) {
                return edge_id;
            }
        }
    }
    return kNoEntity;
}

void TestFilletBox() {
    std::printf("fillet, swept over box sizes:\n");
    const double r = 0.75;
    for (int depth = 4; depth <= 9; ++depth) {
        const double d = static_cast<double>(depth);
        Model box;
        EntityId body = kNoEntity;
        CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, d, 4.0}, &box, &body));
        // The vertical edge at the origin, between the x = 0 and y = 0
        // faces. Convex, four units long.
        const EntityId edge = EdgeBetweenPoints(box, body, Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 4.0});
        CHECK(edge != kNoEntity);

        BlendOptions options;
        options.radius = r;
        Model out;
        EntityId blended = kNoEntity;
        std::string error;
        CHECK_MESSAGE(BlendEdge(box, body, edge, options, &out, &blended, &error), error);

        const double expected = 4.0 * d * 4.0 - (1.0 - kPi / 4.0) * r * r * 4.0;
        const double volume = SolidVolume(out, blended, FineMesh());
        const double bound = ChordingBound(r, kPi / 2.0, 4.0);
        std::printf("  4 x %.0f x 4: %.6f, wanted %.6f (chording allows %.6f under)\n", d, volume,
                    expected, bound);
        // Never over: a mesh inside a convex surface cannot enclose more
        // than the solid does.
        CHECK(volume <= expected + 1e-9);
        CHECK(volume >= expected - bound);
        CheckFilletSurface(out, blended, r, Vec3d{0.0, 0.0, 1.0}, 4.0, kPi / 2.0);
        // One face gained, and the two it was cut into are still there.
        CHECK(FaceCount(out, blended) == 7);
    }

    // The radius matters, which is worth one check of its own: a formula
    // that ignored it would still pass a single-radius sweep.
    for (double radius : {0.25, 0.5, 1.0, 1.9}) {
        Model box;
        EntityId body = kNoEntity;
        CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, 5.0, 4.0}, &box, &body));
        const EntityId edge = EdgeBetweenPoints(box, body, Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 4.0});
        BlendOptions options;
        options.radius = radius;
        Model out;
        EntityId blended = kNoEntity;
        std::string error;
        CHECK_MESSAGE(BlendEdge(box, body, edge, options, &out, &blended, &error), error);
        const double expected = 80.0 - (1.0 - kPi / 4.0) * radius * radius * 4.0;
        const double volume = SolidVolume(out, blended, FineMesh());
        CHECK(volume <= expected + 1e-9);
        CHECK(volume >= expected - ChordingBound(radius, kPi / 2.0, 4.0));
        CheckFilletSurface(out, blended, radius, Vec3d{0.0, 0.0, 1.0}, 4.0, kPi / 2.0);
    }
    std::printf("  radii 0.25 through 1.9 all match the closed form\n");
}

void TestChamferBox() {
    const double setback = 0.8;
    for (int depth = 4; depth <= 9; ++depth) {
        const double d = static_cast<double>(depth);
        Model box;
        EntityId body = kNoEntity;
        CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, d, 4.0}, &box, &body));
        const EntityId edge = EdgeBetweenPoints(box, body, Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 4.0});
        BlendOptions options;
        options.kind = BlendKind::Chamfer;
        options.radius = setback;
        Model out;
        EntityId blended = kNoEntity;
        std::string error;
        CHECK_MESSAGE(BlendEdge(box, body, edge, options, &out, &blended, &error), error);
        const double expected = 4.0 * d * 4.0 - 0.5 * setback * setback * 4.0;
        CHECK(Near(SolidVolume(out, blended, FineMesh()), expected, 1e-9));
    }
    std::printf("chamfer: the same sweep, exact to 1e-9 (the cut is flat, so there is nothing to "
                "tessellate away)\n");
}

// A concave edge adds material instead of removing it, and it is the same
// construction with one sign changed -- so the test is the same closed
// form with the sign changed, and a fillet that had the convexity test
// backwards would come out low by exactly twice the difference.
void TestFilletConcave() {
    Sketch sketch(AxisAlignedPlane(0.0));
    // An L: 4 wide, 4 tall, 1 thick. The reflex corner is at (1, 1).
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
    std::string error;
    CHECK_MESSAGE(SelectProfile(sketch, -1, &profile, &error), error);

    const double height = 3.0;
    Model model;
    EntityId body = kNoEntity;
    CHECK_MESSAGE(ExtrudeProfile(sketch, profile, Vec3d{0.0, 0.0, 1.0}, height, &model, &body, &error),
                  error);
    const double base = 4.0 * 1.0 + 3.0 * 1.0;
    CHECK(Near(SolidVolume(model, body, FineMesh()), base * height, 1e-6));

    const double r = 0.4;
    const EntityId edge =
        EdgeBetweenPoints(model, body, Vec3d{1.0, 1.0, 0.0}, Vec3d{1.0, 1.0, height});
    CHECK(edge != kNoEntity);
    BlendOptions options;
    options.radius = r;
    Model out;
    EntityId blended = kNoEntity;
    CHECK_MESSAGE(BlendEdge(model, body, edge, options, &out, &blended, &error), error);

    const double expected = base * height + (1.0 - kPi / 4.0) * r * r * height;
    const double volume = SolidVolume(out, blended, FineMesh());
    std::printf("concave fillet on an L: %.6f, wanted %.6f (the corner gains %.6f)\n", volume, expected,
                expected - base * height);
    // A concave blend's mesh reads *over*, not under: the chords now cut
    // across material that the true surface leaves out. The sign of that
    // is the same statement as the sign of the volume change, so getting
    // the convexity test backwards fails here twice.
    CHECK(volume >= expected - 1e-9);
    CHECK(volume <= expected + ChordingBound(r, kPi / 2.0, height));
    CheckFilletSurface(out, blended, r, Vec3d{0.0, 0.0, 1.0}, height, kPi / 2.0);
}

// A right angle is the one case where the fillet's setback along each
// face happens to equal its radius, and a construction that quietly
// assumed that would pass every test above. So: an equilateral prism,
// whose faces meet at 60 degrees, on a plane tilted away from all three
// axes so that nothing lines up with anything.
//
// For an edge whose faces meet at an interior angle a, the ball sits
// r/tan(a/2) back along each face and sweeps through pi - a, so it takes
// away r^2/tan(a/2) - r^2 (pi - a)/2 per unit of edge. At a = pi/2 that
// is the (1 - pi/4) r^2 used above; at a = pi/3 it is (sqrt(3) - pi/3)
// r^2, which is more than twice as much.
void TestFilletWedge() {
    SketchPlane plane = PlaneFromNormal(Vec3d{1.0, 2.0, 3.0}, Vec3d{1.0, 2.0, 3.0}.Normalized());
    Sketch sketch(plane);
    const double side = 6.0;
    const double h = side * std::sqrt(3.0) / 2.0;
    // The apex at the origin of the sketch, so its world position is the
    // plane's own origin and the edge to blend is easy to name.
    const Vec2d apex{0.0, 0.0};
    const Vec2d left{-side / 2.0, -h};
    const Vec2d right{side / 2.0, -h};
    const SketchId a = sketch.AddPoint(apex);
    const SketchId b = sketch.AddPoint(right);
    const SketchId c = sketch.AddPoint(left);
    sketch.AddLineFromPoints(a, b);
    sketch.AddLineFromPoints(b, c);
    sketch.AddLineFromPoints(c, a);

    Sketch::Profile profile;
    std::string error;
    CHECK_MESSAGE(SelectProfile(sketch, -1, &profile, &error), error);

    const double length = 5.0;
    const Vec3d normal = sketch.Plane().Normal();
    Model model;
    EntityId body = kNoEntity;
    CHECK_MESSAGE(ExtrudeProfile(sketch, profile, normal, length, &model, &body, &error), error);
    const double base = 0.5 * side * h;
    CHECK(Near(SolidVolume(model, body, FineMesh()), base * length, 1e-9));

    const Vec3d bottom = sketch.Plane().ToWorld(apex);
    const EntityId edge = EdgeBetweenPoints(model, body, bottom, bottom + normal * length);
    CHECK(edge != kNoEntity);

    const double r = 0.5;
    BlendOptions options;
    options.radius = r;
    Model out;
    EntityId blended = kNoEntity;
    CHECK_MESSAGE(BlendEdge(model, body, edge, options, &out, &blended, &error), error);

    const double interior = kPi / 3.0;
    const double sweep = kPi - interior;
    const double removed = (r * r / std::tan(interior / 2.0) - r * r * sweep / 2.0) * length;
    const double expected = base * length - removed;
    const double volume = SolidVolume(out, blended, FineMesh());
    std::printf("60-degree edge on a tilted prism: %.6f, wanted %.6f (took away %.6f, which is "
                "%.2f times what a right angle would)\n",
                volume, expected, removed, removed / ((1.0 - kPi / 4.0) * r * r * length));
    CHECK(volume <= expected + 1e-9);
    CHECK(volume >= expected - ChordingBound(r, sweep, length));
    // And the setback really is not the radius: 0.5 / tan(30 degrees).
    CheckFilletSurface(out, blended, r, normal, length, sweep);
    CHECK(Near(r / std::tan(interior / 2.0), 0.8660254037844387, 1e-12));

    // The chamfer on the same edge, where `radius` means the setback
    // itself rather than a ball radius, so it takes away the triangle
    // with two sides of that length and the angle between them.
    BlendOptions flat;
    flat.kind = BlendKind::Chamfer;
    flat.radius = 0.8;
    Model cut;
    EntityId cut_body = kNoEntity;
    CHECK_MESSAGE(BlendEdge(model, body, edge, flat, &cut, &cut_body, &error), error);
    const double chamfer_area = 0.5 * flat.radius * flat.radius * std::sin(interior);
    CHECK(Near(SolidVolume(cut, cut_body, FineMesh()), base * length - chamfer_area * length, 1e-9));
    std::printf("  and the chamfer takes away exactly the triangle, to 1e-9\n");
}

// Several edges at once. They are carried across the rebuilds by the face
// pair each lies between, which is the whole reason Part B.4's naming was
// built before Part E.3 needed it.
void TestBlendSeveralEdges() {
    Model box;
    EntityId body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, 6.0, 4.0}, &box, &body));
    const std::vector<EntityId> edges = {
        EdgeBetweenPoints(box, body, Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 4.0}),
        EdgeBetweenPoints(box, body, Vec3d{4.0, 0.0, 0.0}, Vec3d{4.0, 0.0, 4.0}),
        EdgeBetweenPoints(box, body, Vec3d{4.0, 6.0, 0.0}, Vec3d{4.0, 6.0, 4.0}),
        EdgeBetweenPoints(box, body, Vec3d{0.0, 6.0, 0.0}, Vec3d{0.0, 6.0, 4.0}),
    };
    for (EntityId e : edges) CHECK(e != kNoEntity);

    const double r = 0.6;
    BlendOptions options;
    options.radius = r;
    Model out;
    EntityId blended = kNoEntity;
    std::string error;
    CHECK_MESSAGE(BlendEdges(box, body, edges, options, &out, &blended, &error), error);

    const double expected = 96.0 - 4.0 * (1.0 - kPi / 4.0) * r * r * 4.0;
    const double volume = SolidVolume(out, blended, FineMesh());
    std::printf("all four vertical edges rounded: %.6f, wanted %.6f\n", volume, expected);
    CHECK(volume <= expected + 1e-9);
    CHECK(volume >= expected - 4.0 * ChordingBound(r, kPi / 2.0, 4.0));
    CHECK(FaceCount(out, blended) == 10);
}

// What it refuses, and that it says why. A blend that quietly produced a
// self-intersecting solid when the radius does not fit would be far worse
// than one that stops, so the refusals are tested as carefully as the
// successes.
void TestBlendRefusals() {
    Model box;
    EntityId body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, 5.0, 4.0}, &box, &body));
    const EntityId edge = EdgeBetweenPoints(box, body, Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 4.0});

    Model out;
    EntityId blended = kNoEntity;
    std::string error;

    BlendOptions too_big;
    too_big.radius = 4.5;  // wider than the 4-unit face it would trim
    CHECK(!BlendEdge(box, body, edge, too_big, &out, &blended, &error));
    std::printf("refused an oversized radius: %s\n", error.c_str());
    CHECK(error.find("too large") != std::string::npos);

    BlendOptions negative;
    negative.radius = -1.0;
    CHECK(!BlendEdge(box, body, edge, negative, &out, &blended, &error));
    CHECK(!error.empty());

    // A cylinder's rim: a closed circular edge, between a plane and a
    // cylinder. Refused twice over, and by name.
    Model tube;
    EntityId tube_body = kNoEntity;
    CHECK(MakeCylinder(Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 1.0}, 2.0, 5.0, &tube, &tube_body));
    EntityId curved = kNoEntity;
    for (EntityId shell_id : tube.GetBody(tube_body)->shells) {
        for (EntityId e : tube.EdgesOfShell(shell_id)) {
            const Curve3 *c = tube.CurveAt(tube.GetEdge(e)->curve);
            if (c != nullptr && c->Kind() == CurveKind::Circle) curved = e;
        }
    }
    CHECK(curved != kNoEntity);
    BlendOptions options;
    options.radius = 0.3;
    CHECK(!BlendEdge(tube, tube_body, curved, options, &out, &blended, &error));
    std::printf("refused a curved edge: %s\n", error.c_str());
    CHECK(error.find("straight") != std::string::npos || error.find("closed") != std::string::npos);

    // Two edges that meet at a corner. The first blend eats the corner
    // vertex the second one needed, and what used to be the flat face
    // across that end is now the first blend's cylinder -- so the second
    // is refused rather than attempted, which is the honest answer until
    // vertex blends exist.
    Model corner_box;
    EntityId corner_body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, 5.0, 4.0}, &corner_box, &corner_body));
    const std::vector<EntityId> touching = {
        EdgeBetweenPoints(corner_box, corner_body, Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 4.0}),
        EdgeBetweenPoints(corner_box, corner_body, Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, 0.0, 0.0}),
    };
    for (EntityId e : touching) CHECK(e != kNoEntity);
    BlendOptions pair;
    pair.radius = 0.5;
    CHECK(!BlendEdges(corner_box, corner_body, touching, pair, &out, &blended, &error));
    std::printf("refused two edges meeting at a corner: %s\n", error.c_str());
    CHECK(error.find("blend 2 of 2") != std::string::npos);
}

// Rounding one edge must not disturb any other. The quickest way to say
// that precisely: every face the blend did not touch is still exactly
// where it was, and the two it did touch shrank by exactly the setback.
void TestBlendIsLocal() {
    Model box;
    EntityId body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, 7.0, 4.0}, &box, &body));
    const EntityId edge = EdgeBetweenPoints(box, body, Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 4.0});
    BlendOptions options;
    options.radius = 0.5;
    Model out;
    EntityId blended = kNoEntity;
    std::string error;
    CHECK_MESSAGE(BlendEdge(box, body, edge, options, &out, &blended, &error), error);

    // The far corner is untouched, and so is the whole extent.
    const Box3d bounds = out.Bounds();
    CHECK(Near(bounds.x.lo, 0.0, 1e-12) && Near(bounds.y.lo, 0.0, 1e-12));
    CHECK(Near(bounds.x.hi, 4.0, 1e-12) && Near(bounds.y.hi, 7.0, 1e-12));
    CHECK(EdgeBetweenPoints(out, blended, Vec3d{4.0, 7.0, 0.0}, Vec3d{4.0, 7.0, 4.0}) != kNoEntity);
    // The corner vertex is gone, replaced by the two tangent points.
    CHECK(EdgeBetweenPoints(out, blended, Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 4.0}) == kNoEntity);
    CHECK(EdgeBetweenPoints(out, blended, Vec3d{0.0, 0.5, 0.0}, Vec3d{0.0, 0.5, 4.0}) != kNoEntity);
    CHECK(EdgeBetweenPoints(out, blended, Vec3d{0.5, 0.0, 0.0}, Vec3d{0.5, 0.0, 4.0}) != kNoEntity);
    std::printf("the blend is local: the far edges, and the body's extent, are untouched\n");

    // And it composes: blending the rounded body again still works, so a
    // blend is an ordinary operation on an ordinary body rather than
    // something only a primitive survives.
    const EntityId next = EdgeBetweenPoints(out, blended, Vec3d{4.0, 0.0, 0.0}, Vec3d{4.0, 0.0, 4.0});
    CHECK(next != kNoEntity);
    Model twice;
    EntityId twice_body = kNoEntity;
    CHECK_MESSAGE(BlendEdge(out, blended, next, options, &twice, &twice_body, &error), error);
    const double expected = 112.0 - 2.0 * (1.0 - kPi / 4.0) * 0.25 * 4.0;
    const double volume = SolidVolume(twice, twice_body, FineMesh());
    CHECK(volume <= expected + 1e-9);
    CHECK(volume >= expected - 2.0 * ChordingBound(0.5, kPi / 2.0, 4.0));
}

// Part B.4, after the sweep: a name captured from a model must resolve
// on that same model. Not "usually", not "on simple shapes" -- always,
// because the entity it names is right there and unchanged.
void TestAnExactNameAlwaysResolves() {
    std::printf("every face of an L-bracket names itself unambiguously:\n");
    // AN L-BRACKET, because a box cannot show this. `ResolveFace`
    // requires the best candidate to beat the runner-up by 15% of its
    // score -- sound when choosing between approximations, and wrong
    // when one candidate matched *exactly*. On a box the runners-up
    // score far lower and the margin never bites. On an L the held face
    // scored 1.000000 against 0.873604, a gap of 12.6%, and a support
    // attached to it came back ambiguous against the very model it had
    // been captured from.
    Model a;
    Model b;
    EntityId big = kNoEntity;
    EntityId cut = kNoEntity;
    CHECK(MakeBox({0, 0, 0}, {10, 6, 4}, &a, &big));
    CHECK(MakeBox({4, -1, 2}, {7, 8, 3}, &b, &cut));
    std::string error;
    CHECK(BuildAllPCurves(&a, {}, &error));
    CHECK(BuildAllPCurves(&b, {}, &error));
    Model ell;
    BooleanReport boolean;
    CHECK_MESSAGE(BooleanOperation(a, big, b, cut, BooleanOp::Difference, &ell, &boolean),
                  boolean.error);
    CHECK(BuildAllPCurves(&ell, {}, &error));

    int resolved = 0;
    int wrong = 0;
    int ambiguous = 0;
    double worst_margin = 1.0;
    for (const Face &face : ell.Faces()) {
        // Captured exactly as `fem::FaceTarget` captures it, which is the
        // path that matters: the face's own role, and no generating
        // feature, because a body from a boolean has no feature tree.
        const EntityName name = CaptureFaceName(ell, face.id, -1, face.name);
        const ResolveResult found = ResolveFace(ell, name);
        if (found.status != ResolveStatus::Resolved) {
            ++ambiguous;
            std::printf("  face %lld: %s\n", static_cast<long long>(face.id),
                        found.explanation.c_str());
            continue;
        }
        ++resolved;
        // AND IT RESOLVES TO THE RIGHT FACE, which is the half a status
        // check leaves out: a rule that resolved everything to the same
        // face would pass a count.
        if (found.entity != face.id) ++wrong;
        worst_margin = std::min(worst_margin, found.score);
    }
    std::printf("  %d of %d faces resolved, %d to the wrong face, %d ambiguous;"
                " lowest score %.6f\n",
                resolved, static_cast<int>(ell.Faces().size()), wrong, ambiguous, worst_margin);
    CHECK(resolved == static_cast<int>(ell.Faces().size()));
    CHECK(wrong == 0);
    CHECK(ambiguous == 0);
    // Every one of them matched exactly, which is why the dominance
    // margin should never have been consulted for any of them -- and is
    // the whole content of the fix. A face captured from a model and
    // looked up in that same, unchanged model is not an approximation
    // being chosen between.
    CHECK(worst_margin >= 1.0 - 1e-9);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestExtrude();
    TestRevolve();
    TestLoft();
    TestSweep();
    TestTree();
    TestMultiBody();
    TestRevolveFeatureAndIntersect();
    TestEdgeNaming();
    TestFilletBox();
    TestChamferBox();
    TestFilletConcave();
    TestFilletWedge();
    TestBlendSeveralEdges();
    TestBlendRefusals();
    TestBlendIsLocal();
    TestAnExactNameAlwaysResolves();
    std::printf("cad_feature_test passed (%d checks)\n", g_checks);
    return 0;
}
