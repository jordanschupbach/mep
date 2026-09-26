// Windowless coverage for cad_pattern.h (plans/CAD_FEM_PLAN.md Part E.5).
//
// A PATTERN'S VOLUME IS ITS ONLY HONEST SUMMARY, and for separate copies
// it is exactly n times the seed's -- which is worth checking because the
// two ways of getting it wrong both survive everything else. A copy
// placed on top of another still counts once in a face tally and twice
// here; and a *mirrored* copy is perfectly valid, perfectly watertight,
// and has a negative volume, because a reflection reverses handedness
// and leaves every face pointing into the material. So every body every
// test here produces is validated, tessellated, checked watertight, and
// then checked for a positive volume before its size is looked at.
//
// The merged patterns are checked against inclusion-exclusion, which is
// the only closed form available once copies overlap: two boxes
// overlapping in a slab are 2V - (the slab), three in a row are
// 3V - 2(slab), and so on for a run where only neighbours meet.

#include "cad_pattern.h"

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

// Every body of a pattern, measured and added up. A copy that came out
// inside out drags the total down by twice its size rather than being
// quietly tolerated.
double TotalVolume(const Model &model, const std::vector<EntityId> &bodies) {
    double total = 0.0;
    for (EntityId body : bodies) total += SolidVolume(model, body);
    return total;
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

// PlaneFromNormal picks its own in-plane axes, which are not the world's
// -- so a test that wants sketch (x, y) to land on world (x, y) has to
// say so.
SketchPlane AxisAlignedPlane() {
    SketchPlane plane = PlaneFromNormal(Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 1.0});
    plane.x_axis = Vec3d{1.0, 0.0, 0.0};
    plane.y_axis = Vec3d{0.0, 1.0, 0.0};
    return plane;
}

Model Seed(EntityId *body, const Vec3d &corner = Vec3d{0.0, 0.0, 0.0},
           const Vec3d &size = Vec3d{2.0, 3.0, 4.0}) {
    Model model;
    std::string error;
    Check(MakeBox(corner, size, &model, body), "MakeBox", __LINE__);
    Check(BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
    return model;
}

// --- Transform ----------------------------------------------------------

void TestTransform() {
    EntityId body = kNoEntity;
    const Model box = Seed(&body);
    const double volume = SolidVolume(box, body);
    CHECK(Near(volume, 24.0, 1e-9));

    // A rigid motion changes nothing measurable.
    std::string error;
    Model moved;
    EntityId moved_body = kNoEntity;
    const Mat4d rigid = Mat4d::Translation(Vec3d{10.0, -4.0, 2.5}) *
                        Mat4d::Rotation(Vec3d{1.0, 2.0, 3.0}, 0.7);
    CHECK_MESSAGE(TransformBody(box, body, rigid, &moved, &moved_body, &error), error);
    CHECK(Near(SolidVolume(moved, moved_body), 24.0, 1e-9));
    std::printf("a rigid motion leaves the volume at %.9f\n", SolidVolume(moved, moved_body));

    // A reflection does not, unless every face is turned round -- and the
    // volume coming out *positive* is the whole content of that claim.
    Model mirrored;
    EntityId mirrored_body = kNoEntity;
    CHECK_MESSAGE(TransformBody(box, body, Reflection(Vec3d{5.0, 0.0, 0.0}, Vec3d{1.0, 0.0, 0.0}),
                                &mirrored, &mirrored_body, &error),
                  error);
    const double reflected = SolidVolume(mirrored, mirrored_body);
    std::printf("a reflection keeps it positive at %.9f\n", reflected);
    CHECK(reflected > 0.0);
    CHECK(Near(reflected, 24.0, 1e-9));
    // And it really did move: the box spanned x in [0,2], so reflecting
    // about x = 5 puts it in [8,10].
    const Box3d extent = BodyExtent(mirrored, mirrored_body);
    CHECK(Near(extent.x.lo, 8.0, 1e-12) && Near(extent.x.hi, 10.0, 1e-12));

    // Reflecting twice about the same plane is the identity.
    Model back;
    EntityId back_body = kNoEntity;
    CHECK_MESSAGE(TransformBody(mirrored, mirrored_body,
                                Reflection(Vec3d{5.0, 0.0, 0.0}, Vec3d{1.0, 0.0, 0.0}), &back,
                                &back_body, &error),
                  error);
    const Box3d again = BodyExtent(back, back_body);
    CHECK(Near(again.x.lo, 0.0, 1e-12) && Near(again.x.hi, 2.0, 1e-12));
    CHECK(Near(SolidVolume(back, back_body), 24.0, 1e-9));

    // A degenerate transform is refused rather than producing a body of
    // no thickness.
    Model flat;
    EntityId flat_body = kNoEntity;
    CHECK(!TransformBody(box, body, Mat4d::Scaling(Vec3d{1.0, 1.0, 0.0}), &flat, &flat_body, &error));
    CHECK(error.find("flattens") != std::string::npos);
}

// --- Separate patterns ---------------------------------------------------

void TestLinearAndGrid() {
    EntityId body = kNoEntity;
    const Model box = Seed(&body);
    std::string error;

    for (int count : {1, 2, 5, 9}) {
        Model out;
        std::vector<EntityId> bodies;
        CHECK_MESSAGE(PatternBody(box, body, LinearPlacements(Vec3d{1.0, 0.0, 0.0}, 5.0, count),
                                  PatternCombine::Separate, {}, &out, &bodies, &error),
                      error);
        CHECK(static_cast<int>(bodies.size()) == count);
        CHECK(Near(TotalVolume(out, bodies), 24.0 * count, 1e-9));
    }
    std::printf("linear: 1, 2, 5 and 9 copies each total exactly n times the seed\n");

    // The spacing is real, not just the count: nine copies 5 apart span
    // 8*5 + 2 in x.
    Model out;
    std::vector<EntityId> bodies;
    CHECK_MESSAGE(PatternBody(box, body, LinearPlacements(Vec3d{1.0, 0.0, 0.0}, 5.0, 9),
                              PatternCombine::Separate, {}, &out, &bodies, &error),
                  error);
    const Box3d first = BodyExtent(out, bodies.front());
    const Box3d last = BodyExtent(out, bodies.back());
    CHECK(Near(first.x.lo, 0.0, 1e-12));
    CHECK(Near(last.x.hi, 8.0 * 5.0 + 2.0, 1e-12));
    // The direction is normalised, so its length does not change the step.
    Model scaled;
    std::vector<EntityId> scaled_bodies;
    CHECK_MESSAGE(PatternBody(box, body, LinearPlacements(Vec3d{100.0, 0.0, 0.0}, 5.0, 9),
                              PatternCombine::Separate, {}, &scaled, &scaled_bodies, &error),
                  error);
    CHECK(Near(BodyExtent(scaled, scaled_bodies.back()).x.hi, 42.0, 1e-12));

    Model grid;
    std::vector<EntityId> grid_bodies;
    CHECK_MESSAGE(PatternBody(box, body,
                              GridPlacements(Vec3d{1.0, 0.0, 0.0}, 5.0, 4, Vec3d{0.0, 1.0, 0.0}, 7.0, 3),
                              PatternCombine::Separate, {}, &grid, &grid_bodies, &error),
                  error);
    CHECK(grid_bodies.size() == 12);
    CHECK(Near(TotalVolume(grid, grid_bodies), 24.0 * 12, 1e-9));
    std::printf("a 4 by 3 grid: 12 bodies, %.6f\n", TotalVolume(grid, grid_bodies));
}

void TestCircular() {
    // A box standing off the axis, so the copies do not overlap.
    EntityId body = kNoEntity;
    const Model box = Seed(&body, Vec3d{10.0, -1.0, 0.0}, Vec3d{2.0, 2.0, 3.0});
    std::string error;

    for (int count : {2, 3, 6, 12}) {
        Model out;
        std::vector<EntityId> bodies;
        CHECK_MESSAGE(PatternBody(box, body,
                                  CircularPlacements(Vec3d{}, Vec3d{0.0, 0.0, 1.0}, count, kTwoPi, true),
                                  PatternCombine::Separate, {}, &out, &bodies, &error),
                      error);
        CHECK(static_cast<int>(bodies.size()) == count);
        CHECK(Near(TotalVolume(out, bodies), 12.0 * count, 1e-9));
        // Every copy stays the same distance from the axis.
        for (EntityId made : bodies) {
            const Box3d extent = BodyExtent(out, made);
            const double radius = std::hypot(extent.x.Mid(), extent.y.Mid());
            CHECK(Near(radius, 11.0, 1e-9));
        }
    }
    std::printf("circular, closed: 2, 3, 6 and 12 about the z axis, each copy still 11 from it\n");

    // `close` is the whole ambiguity of a circular pattern. Six copies
    // spanning a full turn step by 60 degrees; six copies with one at
    // each end of a full turn would put the sixth on top of the first,
    // which is why the open form is meant for a partial sweep.
    const std::vector<Mat4d> closed = CircularPlacements(Vec3d{}, Vec3d{0, 0, 1}, 6, kTwoPi, true);
    const std::vector<Mat4d> open = CircularPlacements(Vec3d{}, Vec3d{0, 0, 1}, 6, kHalfPi, false);
    Model a;
    std::vector<EntityId> a_bodies;
    CHECK_MESSAGE(PatternBody(box, body, closed, PatternCombine::Separate, {}, &a, &a_bodies, &error),
                  error);
    CHECK(a_bodies.size() == 6);
    Model b;
    std::vector<EntityId> b_bodies;
    CHECK_MESSAGE(PatternBody(box, body, open, PatternCombine::Separate, {}, &b, &b_bodies, &error),
                  error);
    CHECK(b_bodies.size() == 6);
    // The open form's last copy is at the full 90 degrees: the seed's
    // centre was on +x, so the last one's is on +y.
    const Box3d last = BodyExtent(b, b_bodies.back());
    CHECK(Near(last.x.Mid(), 0.0, 1e-9));
    CHECK(Near(last.y.Mid(), 11.0, 1e-9));
    std::printf("circular, open: six copies over 90 degrees put the last one on the y axis\n");

    // A closed full turn asked for with `close` false doubles up on the
    // seed, and that duplicate is dropped rather than handed to a boolean
    // that cannot resolve it.
    Model doubled;
    std::vector<EntityId> doubled_bodies;
    CHECK_MESSAGE(PatternBody(box, body, CircularPlacements(Vec3d{}, Vec3d{0, 0, 1}, 7, kTwoPi, false),
                              PatternCombine::Separate, {}, &doubled, &doubled_bodies, &error),
                  error);
    CHECK(doubled_bodies.size() == 6);
    std::printf("and a full turn with a copy at each end drops the repeat: 7 asked, 6 placed\n");
}

void TestMirror() {
    EntityId body = kNoEntity;
    const Model box = Seed(&body, Vec3d{1.0, 0.0, 0.0}, Vec3d{2.0, 3.0, 4.0});
    std::string error;
    Model out;
    std::vector<EntityId> bodies;
    CHECK_MESSAGE(PatternBody(box, body, MirrorPlacements(Vec3d{}, Vec3d{1.0, 0.0, 0.0}),
                              PatternCombine::Separate, {}, &out, &bodies, &error),
                  error);
    CHECK(bodies.size() == 2);
    // Both positive, both the same size: a mirrored copy that kept its
    // handedness would show here as -24 and a total of 0.
    CHECK(Near(SolidVolume(out, bodies[0]), 24.0, 1e-9));
    CHECK(Near(SolidVolume(out, bodies[1]), 24.0, 1e-9));
    CHECK(Near(TotalVolume(out, bodies), 48.0, 1e-9));
    const Box3d reflected = BodyExtent(out, bodies[1]);
    CHECK(Near(reflected.x.lo, -3.0, 1e-12) && Near(reflected.x.hi, -1.0, 1e-12));
    std::printf("mirror: both copies %.6f, the reflection in x [%.1f, %.1f]\n",
                SolidVolume(out, bodies[1]), reflected.x.lo, reflected.x.hi);

    // Mirroring something that straddles the plane puts the copy on top
    // of the original. Separately that is legal; merged it is the
    // coincident-face case, and is reported.
    EntityId straddling = kNoEntity;
    const Model centred = Seed(&straddling, Vec3d{-1.0, 0.0, 0.0}, Vec3d{2.0, 3.0, 4.0});
    Model overlap;
    std::vector<EntityId> overlap_bodies;
    CHECK_MESSAGE(PatternBody(centred, straddling, MirrorPlacements(Vec3d{}, Vec3d{1.0, 0.0, 0.0}),
                              PatternCombine::Separate, {}, &overlap, &overlap_bodies, &error),
                  error);
    CHECK(overlap_bodies.size() == 2);
}

void TestSketchDriven() {
    Sketch sketch(AxisAlignedPlane());
    const SketchId a = sketch.AddPoint(Vec2d{0.0, 0.0});
    const SketchId b = sketch.AddPoint(Vec2d{6.0, 0.0});
    const SketchId c = sketch.AddPoint(Vec2d{6.0, 8.0});
    const SketchId d = sketch.AddPoint(Vec2d{-4.0, 5.0});

    std::vector<Mat4d> placements;
    std::string error;
    CHECK_MESSAGE(SketchPlacements(sketch, {a, b, c, d}, &placements, &error), error);
    CHECK(placements.size() == 4);

    EntityId body = kNoEntity;
    const Model box = Seed(&body, Vec3d{0.0, 0.0, 0.0}, Vec3d{1.0, 1.0, 1.0});
    Model out;
    std::vector<EntityId> bodies;
    CHECK_MESSAGE(PatternBody(box, body, placements, PatternCombine::Separate, {}, &out, &bodies, &error),
                  error);
    CHECK(bodies.size() == 4);
    CHECK(Near(TotalVolume(out, bodies), 4.0, 1e-9));
    std::printf("sketch-driven: a copy at each of four sketch points, %.6f total\n",
                TotalVolume(out, bodies));

    // Moving the sketch point moves the copy, which is the only reason to
    // drive a pattern from a sketch rather than from four numbers.
    sketch.GetPoint(c)->position = Vec2d{20.0, 1.0};
    std::vector<Mat4d> again;
    CHECK_MESSAGE(SketchPlacements(sketch, {a, b, c, d}, &again, &error), error);
    Model moved;
    std::vector<EntityId> moved_bodies;
    CHECK_MESSAGE(PatternBody(box, body, again, PatternCombine::Separate, {}, &moved, &moved_bodies,
                              &error),
                  error);
    CHECK(Near(BodyExtent(moved, moved_bodies[2]).x.lo, 20.0, 1e-9));
    CHECK(Near(TotalVolume(moved, moved_bodies), 4.0, 1e-9));

    // One point is a pattern of one thing, which is not a pattern.
    CHECK(!SketchPlacements(sketch, {a}, &placements, &error));
    CHECK(error.find("at least two") != std::string::npos);
    CHECK(!SketchPlacements(sketch, {a, 999}, &placements, &error));
    CHECK(error.find("does not exist") != std::string::npos);
}

// --- Merged patterns -----------------------------------------------------

void TestMerge() {
    // The copies have to overlap *transversally*. Stepping a box along
    // one axis leaves its four other faces coplanar with the neighbour's,
    // which is the coincident-face case Part C.4 does not handle -- so
    // the step here is diagonal, and then consecutive copies meet in an
    // ordinary box of overlap and nothing is shared.
    const Vec3d size{2.0, 3.0, 4.0};
    const Vec3d step{1.5, 0.7, 0.3};
    const double single = size.x * size.y * size.z;
    // [0,2]x[0,3]x[0,4] against the same shifted by `step`.
    const double overlap = (size.x - step.x) * (size.y - step.y) * (size.z - step.z);
    EntityId body = kNoEntity;
    const Model box = Seed(&body, Vec3d{0.0, 0.0, 0.0}, size);
    std::string error;

    std::printf("merged, boxes overlapping their neighbours diagonally:\n");
    for (int count : {2, 3, 4}) {
        Model out;
        std::vector<EntityId> bodies;
        CHECK_MESSAGE(PatternBody(box, body, LinearPlacements(step, step.Length(), count),
                                  PatternCombine::Merge, {}, &out, &bodies, &error),
                      error);
        CHECK(bodies.size() == 1);
        // Only neighbours meet -- copy i and copy i+2 are already clear of
        // each other in x -- so inclusion-exclusion stops at the first term.
        const double expected = single * count - overlap * (count - 1);
        const double volume = SolidVolume(out, bodies.front());
        std::printf("  %d copies: %.9f, wanted %.9f\n", count, volume, expected);
        // Looser than the exact checks above, and for a reason worth
        // naming: a merged body's edges were *computed*, by intersecting
        // surfaces to a tolerance, rather than carried across unchanged.
        // The separate patterns are exact because nothing was solved.
        CHECK(Near(volume, expected, 1e-6));
        CHECK(Near(BodyExtent(out, bodies.front()).x.hi, step.x * (count - 1) + size.x, 1e-6));
    }

    // And the case that does not work, stated rather than hidden: copies
    // that merely abut share a whole face, and Part C.4 says so by name.
    // The separate form still gives the caller something usable.
    Model touching;
    std::vector<EntityId> touching_bodies;
    CHECK(!PatternBody(box, body, LinearPlacements(Vec3d{1.0, 0.0, 0.0}, size.x, 3),
                       PatternCombine::Merge, {}, &touching, &touching_bodies, &error));
    std::printf("  abutting copies are refused: %s\n", error.c_str());
    CHECK(error.find("coincident face") != std::string::npos);
    CHECK(error.find("copy 2 of 3") != std::string::npos);
    Model apart;
    std::vector<EntityId> apart_bodies;
    CHECK_MESSAGE(PatternBody(box, body, LinearPlacements(Vec3d{1.0, 0.0, 0.0}, size.x, 3),
                              PatternCombine::Separate, {}, &apart, &apart_bodies, &error),
                  error);
    CHECK(Near(TotalVolume(apart, apart_bodies), single * 3, 1e-9));
}

void TestRefusals() {
    EntityId body = kNoEntity;
    const Model box = Seed(&body);
    std::string error;
    Model out;
    std::vector<EntityId> bodies;
    CHECK(!PatternBody(box, body, {}, PatternCombine::Separate, {}, &out, &bodies, &error));
    CHECK(error.find("at least one") != std::string::npos);
    CHECK(!PatternBody(box, 999, LinearPlacements(Vec3d{1, 0, 0}, 5.0, 3), PatternCombine::Separate, {},
                       &out, &bodies, &error));
    CHECK(error.find("no such body") != std::string::npos);
    // Counts below one produce no placements, which the pattern then
    // refuses rather than silently returning nothing.
    CHECK(LinearPlacements(Vec3d{1, 0, 0}, 5.0, 0).empty());
    CHECK(CircularPlacements(Vec3d{}, Vec3d{0, 0, 1}, 0, kTwoPi, true).empty());
    CHECK(CircularPlacements(Vec3d{}, Vec3d{}, 4, kTwoPi, true).empty());
}

// --- Composing with the rest of Part E -----------------------------------

void TestComposeWithFeatures() {
    // A filleted, shelled block, patterned. The point is not the number
    // at the end but that a pattern reads whatever Parts E.1 to E.4
    // produced without knowing anything about it.
    Model box;
    EntityId body = kNoEntity;
    std::string error;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, 4.0, 4.0}, &box, &body));
    CHECK(BuildAllPCurves(&box, {}, &error));

    EntityId edge = kNoEntity;
    for (EntityId shell_id : box.GetBody(body)->shells) {
        for (EntityId candidate : box.EdgesOfShell(shell_id)) {
            const Vec3d p = box.EdgeStartPoint(candidate);
            const Vec3d q = box.EdgeEndPoint(candidate);
            if ((p - Vec3d{0.0, 0.0, 0.0}).Length() < 1e-9 && (q - Vec3d{0.0, 0.0, 4.0}).Length() < 1e-9) {
                edge = candidate;
            }
        }
    }
    CHECK(edge != kNoEntity);
    BlendOptions blend;
    blend.radius = 0.75;
    Model rounded;
    EntityId rounded_body = kNoEntity;
    CHECK_MESSAGE(BlendEdge(box, body, edge, blend, &rounded, &rounded_body, &error), error);
    const double one = SolidVolume(rounded, rounded_body);
    CHECK(one > 0.0);

    Model out;
    std::vector<EntityId> bodies;
    CHECK_MESSAGE(PatternBody(rounded, rounded_body,
                              GridPlacements(Vec3d{1, 0, 0}, 6.0, 3, Vec3d{0, 1, 0}, 6.0, 2),
                              PatternCombine::Separate, {}, &out, &bodies, &error),
                  error);
    CHECK(bodies.size() == 6);
    CHECK(Near(TotalVolume(out, bodies), one * 6.0, 1e-6));
    std::printf("a filleted block patterned 3 by 2: %.6f, which is 6 x %.6f\n",
                TotalVolume(out, bodies), one);

    // And mirrored, where the fillet is what makes the reflection worth
    // checking: a curved face reflected without turning round is exactly
    // how a mirror bug hides.
    Model mirrored;
    std::vector<EntityId> mirrored_bodies;
    CHECK_MESSAGE(PatternBody(rounded, rounded_body, MirrorPlacements(Vec3d{-2.0, 0.0, 0.0}, Vec3d{1, 0, 0}),
                              PatternCombine::Separate, {}, &mirrored, &mirrored_bodies, &error),
                  error);
    CHECK(mirrored_bodies.size() == 2);
    CHECK(Near(SolidVolume(mirrored, mirrored_bodies[1]), one, 1e-6));
    CHECK(SolidVolume(mirrored, mirrored_bodies[1]) > 0.0);
    std::printf("and mirrored, the rounded copy is still %.6f\n",
                SolidVolume(mirrored, mirrored_bodies[1]));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestTransform();
    TestLinearAndGrid();
    TestCircular();
    TestMirror();
    TestSketchDriven();
    TestMerge();
    TestRefusals();
    TestComposeWithFeatures();
    std::printf("cad_pattern_test passed (%d checks)\n", g_checks);
    return 0;
}
