// Windowless coverage for cad_boolean.h (plans/CAD_FEM_PLAN.md Part C.4,
// and the algebraic fuzzing of Part C.5).
//
// A boolean result is checked three ways, and all three matter:
//
//   VALID    Every result goes through Part B.3's validator. That is the
//            check that catches a shell left open along an edge, two
//            faces disagreeing about which side the material is on, or a
//            loop that does not close -- none of which show up in a
//            volume, because an open shell still has one.
//   CLOSED   The tessellation must be watertight, welded by position.
//            Validity is topological and says the faces are sewn to each
//            other; this says they actually meet in space.
//   VOLUME   Against the closed form where there is one, and against the
//            algebraic identities where there is not.
//
// The identity V(A or B) + V(A and B) == V(A) + V(B) is the strongest
// test here, because it holds *exactly* under tessellation: both sides
// facet the same curved surfaces the same way, so the faceting error
// cancels rather than being something to allow a tolerance for. A
// boolean that drops or duplicates a face piece breaks it immediately,
// which is how three missing pieces of a box/box union were found.

#include "cad_boolean.h"

#include "cad_math.h"
#include "cad_pcurve.h"
#include "cad_tessellate.h"
#include "cad_topology.h"
#include "cad_validate.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// The corners of a boolean result are computed, not given: they come out
// of a surface-surface intersection and land within its tolerance of the
// exact point, not on it. A unit cube cut out of two boxes therefore has
// a volume of 1.000000002, and asserting 1.0 to the last bit would be
// asserting something untrue about the algorithm rather than something
// demanding about the implementation. A relative error of 1e-6 is still
// four orders tighter than any wrong answer seen while building this.
constexpr double kVolumeTolerance = 1e-6;

// Relative to the volume being checked, since the error is relative: the
// corners move by a tolerance, and how much volume that is depends on how
// big the solid is.
bool NearVolume(double actual, double expected) {
    return std::fabs(actual - expected) <= kVolumeTolerance * std::max(1.0, std::fabs(expected));
}

// Runs one operation and reports everything worth asserting about the
// result in one struct, so each test reads as a list of expectations
// rather than a repeated twelve-line preamble.
struct Outcome {
    bool ok = false;
    std::string error;
    bool valid = false;
    bool closed = false;
    double volume = 0.0;
    int faces = 0;
    int pieces_kept = 0;
};

Outcome Run(const Model &a, EntityId a_body, const Model &b, EntityId b_body, BooleanOp op) {
    Outcome outcome;
    Model out;
    BooleanReport report;
    BooleanOptions options;
    outcome.ok = BooleanOperation(a, a_body, b, b_body, op, &out, &report, options);
    outcome.error = report.error;
    outcome.pieces_kept = report.pieces_kept;
    if (!outcome.ok) return outcome;

    ValidationReport validation;
    outcome.valid = ValidateBody(out, report.body, &validation, {});

    const Body *body = out.GetBody(report.body);
    if (body != nullptr) {
        for (EntityId s : body->shells) {
            const Shell *shell = out.GetShell(s);
            if (shell != nullptr) outcome.faces += static_cast<int>(shell->faces.size());
        }
    }

    TessellationMesh mesh;
    std::string error;
    if (TessellateBody(out, report.body, {}, &mesh, &error)) {
        outcome.closed = mesh.IsClosed();
        outcome.volume = mesh.SignedVolume();
    }
    return outcome;
}

// Two axis-aligned boxes, p-curves built, ready to hand to Run.
struct BoxPair {
    Model a;
    Model b;
    EntityId a_body = kNoEntity;
    EntityId b_body = kNoEntity;
};

BoxPair MakeBoxPair(const Vec3d &a_min, const Vec3d &a_size, const Vec3d &b_min, const Vec3d &b_size) {
    BoxPair pair;
    MakeBox(a_min, a_size, &pair.a, &pair.a_body);
    MakeBox(b_min, b_size, &pair.b, &pair.b_body);
    std::string error;
    Check(BuildAllPCurves(&pair.a, {}, &error), "BuildAllPCurves(a)", __LINE__);
    Check(BuildAllPCurves(&pair.b, {}, &error), "BuildAllPCurves(b)", __LINE__);
    return pair;
}

// --- The canonical overlapping pair ------------------------------------
//
// A = [0,2]^3 and B = [1,3]^3 overlap in the unit cube [1,2]^3. Every
// face of the overlap region is worth naming: each of the three faces of
// A that B reaches is cut into an L-shaped piece outside B and a unit
// square inside it, and symmetrically for B. That makes the piece counts
// predictable, and asserting them catches a miscut that the volume alone
// would not -- a face split into the wrong two pieces of the right total
// area still integrates correctly.

void TestOverlappingBoxes() {
    std::printf("overlapping boxes\n");
    BoxPair pair = MakeBoxPair(Vec3d{0, 0, 0}, Vec3d{2, 2, 2}, Vec3d{1, 1, 1}, Vec3d{2, 2, 2});

    const Outcome united = Run(pair.a, pair.a_body, pair.b, pair.b_body, BooleanOp::Union);
    CHECK(united.ok);
    CHECK(united.valid);
    CHECK(united.closed);
    // Three uncut faces plus three L-shaped pieces, from each solid.
    CHECK(united.pieces_kept == 12);
    CHECK(united.faces == 12);
    CHECK(NearVolume(united.volume, 15.0));

    const Outcome met = Run(pair.a, pair.a_body, pair.b, pair.b_body, BooleanOp::Intersection);
    CHECK(met.ok);
    CHECK(met.valid);
    CHECK(met.closed);
    // The unit cube: three faces from each solid, no uncut face at all.
    CHECK(met.pieces_kept == 6);
    CHECK(met.faces == 6);
    CHECK(NearVolume(met.volume, 1.0));

    const Outcome cut = Run(pair.a, pair.a_body, pair.b, pair.b_body, BooleanOp::Difference);
    CHECK(cut.ok);
    CHECK(cut.valid);
    CHECK(cut.closed);
    // A's six pieces, plus the three faces of B that now bound the notch.
    CHECK(cut.pieces_kept == 9);
    CHECK(cut.faces == 9);
    CHECK(NearVolume(cut.volume, 7.0));

    // The identities, on the numbers just computed.
    CHECK(NearVolume(united.volume + met.volume, 8.0 + 8.0));
    CHECK(NearVolume(cut.volume + met.volume, 8.0));

    std::printf("  union %.6f, intersection %.6f, difference %.6f\n", united.volume, met.volume, cut.volume);
}

// --- Containment and disjointness --------------------------------------
//
// Neither cuts the other, so none of the imprint runs at all. These are
// the shortcuts, and they are the cases most likely to be quietly wrong,
// because "no intersection curves" reads like "nothing to do".

void TestContainment() {
    std::printf("containment\n");
    BoxPair nested = MakeBoxPair(Vec3d{0, 0, 0}, Vec3d{4, 4, 4}, Vec3d{1, 1, 1}, Vec3d{2, 2, 2});

    const Outcome united = Run(nested.a, nested.a_body, nested.b, nested.b_body, BooleanOp::Union);
    CHECK(united.ok);
    CHECK(united.valid);
    CHECK(united.closed);
    CHECK(united.faces == 6);
    CHECK(NearVolume(united.volume, 64.0));

    const Outcome met = Run(nested.a, nested.a_body, nested.b, nested.b_body, BooleanOp::Intersection);
    CHECK(met.ok);
    CHECK(met.valid);
    CHECK(met.closed);
    CHECK(met.faces == 6);
    CHECK(NearVolume(met.volume, 8.0));

    // The cavity: two shells, twelve faces, and a signed volume that is
    // the outer box less the void. The inner shell's faces have to point
    // into the material for that subtraction to happen at all -- a void
    // shell copied without flipping gives 64 + 8, not 64 - 8.
    const Outcome cut = Run(nested.a, nested.a_body, nested.b, nested.b_body, BooleanOp::Difference);
    CHECK(cut.ok);
    CHECK(cut.valid);
    CHECK(cut.closed);
    CHECK(cut.faces == 12);
    CHECK(NearVolume(cut.volume, 56.0));

    // The other way round: A inside B leaves nothing behind.
    const Outcome empty = Run(nested.b, nested.b_body, nested.a, nested.a_body, BooleanOp::Difference);
    CHECK(!empty.ok);
    CHECK(empty.error.find("empty result") != std::string::npos);

    std::printf("  union %.6f, intersection %.6f, cavity %.6f\n", united.volume, met.volume, cut.volume);
}

void TestDisjoint() {
    std::printf("disjoint\n");
    BoxPair apart = MakeBoxPair(Vec3d{0, 0, 0}, Vec3d{1, 1, 1}, Vec3d{5, 5, 5}, Vec3d{1, 1, 1});

    // A - B is A: removing something that never met it changes nothing.
    const Outcome cut = Run(apart.a, apart.a_body, apart.b, apart.b_body, BooleanOp::Difference);
    CHECK(cut.ok);
    CHECK(cut.valid);
    CHECK(cut.closed);
    CHECK(NearVolume(cut.volume, 1.0));

    // The other two would need a body with two unconnected outer shells,
    // or no body at all. Both are refused, and the refusal says which.
    for (BooleanOp op : {BooleanOp::Union, BooleanOp::Intersection}) {
        const Outcome refused = Run(apart.a, apart.a_body, apart.b, apart.b_body, op);
        CHECK(!refused.ok);
        CHECK(refused.error.find("disjoint") != std::string::npos);
    }
}

// --- A curved face with a seam -----------------------------------------
//
// A cylinder through a box exercises everything the planar cases do not:
// an intersection circle lying wholly inside a face with nothing to
// cross, a lateral face whose parameter space is periodic, and the same
// circle described as one closed curve on the plane and as two arcs on
// the cylinder. The volumes cannot be checked against the closed form to
// more than the faceting error -- but the two halves must still add back
// to the box exactly, because they facet the cylinder identically.

void TestCylinderThroughBox() {
    std::printf("cylinder through box\n");
    Model box;
    Model cylinder;
    EntityId box_body = kNoEntity;
    EntityId cylinder_body = kNoEntity;
    MakeBox(Vec3d{-1, -1, -1}, Vec3d{2, 2, 2}, &box, &box_body);
    MakeCylinder(Vec3d{0, 0, -2}, Vec3d{0, 0, 1}, 0.5, 4.0, &cylinder, &cylinder_body);
    std::string error;
    CHECK(BuildAllPCurves(&box, {}, &error));
    CHECK(BuildAllPCurves(&cylinder, {}, &error));

    const Outcome drilled = Run(box, box_body, cylinder, cylinder_body, BooleanOp::Difference);
    CHECK(drilled.ok);
    CHECK(drilled.valid);
    CHECK(drilled.closed);
    // Six box faces -- two of them now carrying a circular hole -- plus
    // the piece of the cylinder's lateral face inside the box.
    CHECK(drilled.faces == 7);

    const Outcome plug = Run(box, box_body, cylinder, cylinder_body, BooleanOp::Intersection);
    CHECK(plug.ok);
    CHECK(plug.valid);
    CHECK(plug.closed);
    // The plug: two discs and the lateral wall.
    CHECK(plug.faces == 3);

    // Exact, despite both being faceted approximations of a curved solid.
    CHECK(NearVolume(drilled.volume + plug.volume, 8.0));
    // And each within the faceting error of the closed form.
    const double exact_plug = kPi * 0.25 * 2.0;
    CHECK(Near(plug.volume, exact_plug, 0.05));
    CHECK(Near(drilled.volume, 8.0 - exact_plug, 0.05));

    std::printf("  drilled %.6f + plug %.6f = %.9f (box is 8)\n", drilled.volume, plug.volume,
                drilled.volume + plug.volume);
}

// --- The refusals ------------------------------------------------------
//
// Each of these is a case the implementation knows it cannot do. What is
// tested is that it says so, rather than returning something plausible:
// an unchecked boolean that quietly drops a face is far worse than one
// that refuses, because the damage surfaces much later.

void TestRefusals() {
    std::printf("refusals\n");

    // A parameterization pole.
    {
        Model box;
        Model sphere;
        EntityId box_body = kNoEntity;
        EntityId sphere_body = kNoEntity;
        MakeBox(Vec3d{0, 0, 0}, Vec3d{2, 2, 2}, &box, &box_body);
        MakeSphere(Vec3d{1, 1, 2}, 0.5, &sphere, &sphere_body);
        std::string error;
        CHECK(BuildAllPCurves(&box, {}, &error));
        CHECK(BuildAllPCurves(&sphere, {}, &error));
        const Outcome refused = Run(box, box_body, sphere, sphere_body, BooleanOp::Union);
        CHECK(!refused.ok);
        CHECK(refused.error.find("pole") != std::string::npos);
    }

    // Coincident faces: two boxes sharing the plane x = 2 exactly.
    {
        BoxPair touching = MakeBoxPair(Vec3d{0, 0, 0}, Vec3d{2, 2, 2}, Vec3d{2, 0, 0}, Vec3d{2, 2, 2});
        const Outcome refused = Run(touching.a, touching.a_body, touching.b, touching.b_body, BooleanOp::Union);
        CHECK(!refused.ok);
        CHECK(!refused.error.empty());
    }
}

// --- Algebraic fuzzing (Part C.5) --------------------------------------
//
// Random overlapping boxes, each triple checked against
// V(A or B) + V(A and B) == V(A) + V(B) and V(A - B) + V(A and B) ==
// V(A). The boxes are generated on a coarse grid deliberately: shared
// coordinates are where the arrangement's decisions become ties, and a
// generator using continuous random values almost never produces one.
// Pairs whose faces land coincident are skipped rather than counted as
// failures, since that case is refused by design.

void TestFuzz() {
    std::printf("fuzz\n");
    std::mt19937 rng(20260923);
    std::uniform_int_distribution<int> size(2, 5);
    std::uniform_int_distribution<int> inset(0, 3);
    std::uniform_int_distribution<int> overhang(1, 3);
    std::uniform_int_distribution<int> shift(-3, 3);

    int tested = 0;
    int refused = 0;
    double worst_union = 0.0;
    double worst_difference = 0.0;
    for (int trial = 0; trial < 40; ++trial) {
        // A corner overlap, constructed rather than hoped for. B starts
        // strictly inside A on every axis and ends strictly outside it on
        // every axis, so the two genuinely interpenetrate: neither
        // contains the other and they are not disjoint. Generating both
        // boxes freely almost never produces that -- the first attempt
        // here refused 58 of 60 pairs as disjoint or nested, which tests
        // the shortcuts rather than the boolean.
        //
        // The half-integer offsets are the point of the grid: they keep
        // B's faces off A's, since coincident faces are refused by
        // design, while leaving every other coordinate an exact small
        // number so that ties in the arrangement are common rather than
        // vanishingly unlikely.
        Vec3d a_min{static_cast<double>(shift(rng)), static_cast<double>(shift(rng)),
                    static_cast<double>(shift(rng))};
        Vec3d a_size{static_cast<double>(size(rng)), static_cast<double>(size(rng)),
                     static_cast<double>(size(rng))};
        Vec3d offset{std::min(static_cast<double>(inset(rng)) + 0.5, a_size.x - 0.5),
                     std::min(static_cast<double>(inset(rng)) + 0.5, a_size.y - 0.5),
                     std::min(static_cast<double>(inset(rng)) + 0.5, a_size.z - 0.5)};
        Vec3d b_min = a_min + offset;
        Vec3d b_size{a_size.x - offset.x + static_cast<double>(overhang(rng)),
                     a_size.y - offset.y + static_cast<double>(overhang(rng)),
                     a_size.z - offset.z + static_cast<double>(overhang(rng))};

        BoxPair pair = MakeBoxPair(a_min, a_size, b_min, b_size);
        const Outcome united = Run(pair.a, pair.a_body, pair.b, pair.b_body, BooleanOp::Union);
        const Outcome met = Run(pair.a, pair.a_body, pair.b, pair.b_body, BooleanOp::Intersection);
        const Outcome cut = Run(pair.a, pair.a_body, pair.b, pair.b_body, BooleanOp::Difference);
        if (!united.ok || !met.ok || !cut.ok) {
            ++refused;
            continue;
        }
        CHECK(united.valid);
        CHECK(met.valid);
        CHECK(cut.valid);
        CHECK(united.closed);
        CHECK(met.closed);
        CHECK(cut.closed);

        const double volume_a = a_size.x * a_size.y * a_size.z;
        const double volume_b = b_size.x * b_size.y * b_size.z;
        // A corner overlap always cuts each solid into the same pieces:
        // three faces untouched, three split in two. That the counts are
        // predictable at all is worth asserting -- it is what a dropped
        // or duplicated piece breaks first.
        CHECK(united.faces == 12);
        CHECK(met.faces == 6);
        CHECK(cut.faces == 9);

        worst_union = std::max(worst_union, std::fabs(united.volume + met.volume - volume_a - volume_b));
        worst_difference = std::max(worst_difference, std::fabs(cut.volume + met.volume - volume_a));
        CHECK(NearVolume(united.volume + met.volume, volume_a + volume_b));
        CHECK(NearVolume(cut.volume + met.volume, volume_a));
        // The overlap is the box of the two ranges' intersection, which
        // is known in closed form here and pins the absolute answer
        // rather than only the sums.
        const double overlap = (a_size.x - offset.x) * (a_size.y - offset.y) * (a_size.z - offset.z);
        CHECK(NearVolume(met.volume, overlap));
        CHECK(NearVolume(united.volume, volume_a + volume_b - overlap));
        CHECK(NearVolume(cut.volume, volume_a - overlap));
        ++tested;
    }
    std::printf("  %d random pairs verified, %d refused; worst identity error %.3e / %.3e\n", tested, refused,
                worst_union, worst_difference);
    CHECK(refused == 0);
    CHECK(tested >= 35);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestOverlappingBoxes();
    TestContainment();
    TestDisjoint();
    TestCylinderThroughBox();
    TestRefusals();
    TestFuzz();
    std::printf("cad_boolean_test passed (%d checks)\n", g_checks);
    return 0;
}
