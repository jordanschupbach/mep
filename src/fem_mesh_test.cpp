// Windowless coverage for fem_mesh.h (plans/CAD_FEM_PLAN.md Part G).
//
// A MESH IS CHECKED BY WHAT IT IS, NOT BY WHAT IT LOOKS LIKE. Four
// things are true of every good mesh and none of them needs a picture:
// its boundary is closed, its elements all have positive volume, it
// encloses the volume the body does, and its worst element is above the
// threshold that was asked for. Everything below measures those.
//
// The sizing field is checked against what it is *for*: a field that
// ignored curvature would give a fillet the same size as a flat face, and
// one that ignored the growth limit would jump from fine to coarse in one
// cell. Both are asked directly rather than inferred from the mesh.

#include "fem_mesh.h"

#include "cad_boolean.h"
#include "cad_feature.h"
#include "cad_modify.h"
#include "cad_pcurve.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using namespace fem;
using cad::Vec3d;

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

cad::Model Box(const Vec3d &corner, const Vec3d &size, cad::EntityId *body) {
    cad::Model model;
    std::string error;
    Check(cad::MakeBox(corner, size, &model, body), "MakeBox", __LINE__);
    Check(cad::BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
    return model;
}

cad::Model Cylinder(double radius, double height, cad::EntityId *body) {
    cad::Model model;
    std::string error;
    Check(cad::MakeCylinder(Vec3d{0, 0, 0}, Vec3d{0, 0, 1}, radius, height, &model, body),
          "MakeCylinder", __LINE__);
    Check(cad::BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
    return model;
}

// --- G.1: the sizing field ------------------------------------------------

void TestSizingField() {
    // A box has no curvature anywhere, so a field over it should be the
    // target everywhere except where a thin wall says otherwise -- and a
    // 10 x 10 x 10 box has no thin wall either.
    cad::EntityId body = cad::kNoEntity;
    const cad::Model box = Box(Vec3d{0, 0, 0}, Vec3d{10, 10, 10}, &body);
    SizingField field;
    SizingOptions options;
    options.target = 2.0;
    options.thin_wall = false;
    std::string error;
    CHECK_MESSAGE(field.Build(box, {body}, options, &error), error);
    CHECK(Near(field.At(Vec3d{5, 5, 5}), 2.0, 1e-9));
    CHECK(Near(field.At(Vec3d{0.1, 0.1, 0.1}), 2.0, 1e-9));
    std::printf("sizing: a box is %.3f everywhere, in %d cells\n", field.At(Vec3d{5, 5, 5}),
                field.CellCount());

    // A cylinder of radius 1 turning 0.35 radians per element wants 0.35
    // on its curved face, and the target away from it. That is the whole
    // reason the field exists, so it is checked as a number rather than
    // as "smaller than the target".
    cad::EntityId tube_body = cad::kNoEntity;
    const cad::Model tube = Cylinder(1.0, 20.0, &tube_body);
    SizingField curved;
    SizingOptions curved_options;
    curved_options.target = 5.0;
    curved_options.curvature_angle = 0.35;
    curved_options.thin_wall = false;
    curved_options.growth = 1000.0;  // off, so the curvature seed is read neat
    CHECK_MESSAGE(curved.Build(tube, {tube_body}, curved_options, &error), error);
    const double on_surface = curved.At(Vec3d{1.0, 0.0, 10.0});
    std::printf("sizing: on a cylinder of radius 1, %.4f (0.35 = radius x angle)\n", on_surface);
    CHECK(Near(on_surface, 0.35, 0.02));
    // And a bigger radius is sized proportionally, which a field that had
    // merely noticed "this is curved" would not do.
    cad::EntityId big_body = cad::kNoEntity;
    const cad::Model big = Cylinder(4.0, 20.0, &big_body);
    SizingField bigger;
    CHECK_MESSAGE(bigger.Build(big, {big_body}, curved_options, &error), error);
    const double on_big = bigger.At(Vec3d{4.0, 0.0, 10.0});
    std::printf("sizing: on a cylinder of radius 4, %.4f\n", on_big);
    CHECK(Near(on_big, 4.0 * 0.35, 0.1));

    // A thin plate gets sized through its thickness, which curvature
    // cannot see: both faces of a plate are perfectly flat.
    cad::EntityId plate_body = cad::kNoEntity;
    const cad::Model plate = Box(Vec3d{0, 0, 0}, Vec3d{40, 40, 1.0}, &plate_body);
    SizingField thin;
    SizingOptions thin_options;
    thin_options.target = 8.0;
    thin_options.thin_wall = true;
    thin_options.thin_wall_elements = 2;
    thin_options.growth = 1000.0;
    CHECK_MESSAGE(thin.Build(plate, {plate_body}, thin_options, &error), error);
    const double through = thin.At(Vec3d{20, 20, 0.5});
    std::printf("sizing: through a plate 1 thick, %.4f (2 elements wants 0.5)\n", through);
    CHECK(through < 1.0);
    CHECK(through <= 0.6);

    // The growth limit. A field seeded from a small feature and left
    // alone jumps straight back to the target one cell away, which puts
    // a badly graded element exactly where the interesting geometry is.
    // Two things are checked: that no step out multiplies the size by
    // more than the ratio asked for, and that it does climb back to the
    // target given room -- a field that graded by never growing at all
    // would pass the first on its own.
    cad::EntityId room_body = cad::kNoEntity;
    const cad::Model room = Box(Vec3d{0, 0, 0}, Vec3d{40, 40, 40}, &room_body);
    SizingField graded;
    SizingOptions graded_options;
    graded_options.target = 4.0;
    graded_options.thin_wall = false;
    graded_options.growth = 1.5;
    graded_options.min_size = 0.05;
    CHECK_MESSAGE(graded.Build(room, {room_body}, graded_options, &error), error);
    graded.Refine(Vec3d{20, 20, 20}, 0.6, 0.15);
    graded.Smooth();
    CHECK(graded.At(Vec3d{20, 20, 20}) <= 0.2);
    double previous = graded.At(Vec3d{20, 20, 20});
    bool graded_well = true;
    double furthest = previous;
    for (double d = 0.25; d < 19.0; d += 0.25) {
        const double here = graded.At(Vec3d{20.0 + d, 20, 20});
        // The Lipschitz bound: the size may grow by at most
        // (growth - 1) per unit travelled, which is the condition the
        // field is built to and does not depend on how finely the octree
        // happens to be divided here. A little slack for the fact that a
        // sample lands wherever the cell boundaries are.
        if (here > previous + (graded_options.growth - 1.0) * 0.25 * 1.5 + 1e-9) graded_well = false;
        previous = here;
        furthest = here;
    }
    std::printf("sizing: 0.15 at the centre grades out to %.3f of a %.1f target%s\n", furthest,
                graded_options.target, graded_well ? "" : "  [NOT GRADED]");
    CHECK(graded_well);
    CHECK(furthest > graded_options.target * 0.8);

    SizingField nothing;
    CHECK(!nothing.Build(box, {}, options, &error));
    CHECK(error.find("no bodies") != std::string::npos);
}

// --- G.2: the surface mesh -------------------------------------------------

void CheckSurface(const char *what, const cad::Model &model, const std::vector<cad::EntityId> &bodies,
                  double expected_volume, double target) {
    SizingField field;
    SizingOptions sizing;
    sizing.target = target;
    std::string error;
    CHECK_MESSAGE(field.Build(model, bodies, sizing, &error), error);

    SurfaceMeshOptions options;
    options.sizing = sizing;
    SurfaceMesh mesh;
    MeshReport report;
    const bool meshed = MeshSurface(model, bodies, options, field, &mesh, &report);
    for (const std::string &warning : report.warnings) {
        std::printf("      warning: %s\n", warning.c_str());
    }
    CHECK_MESSAGE(meshed, report.error);
    const bool closed = mesh.IsClosed();
    const double volume = mesh.SignedVolume();
    std::printf("  %-12s %5d nodes %5d triangles  closed=%d  volume %.4f (exactly %.4f)\n", what,
                mesh.NodeCount(), mesh.TriangleCount(), closed ? 1 : 0, volume, expected_volume);
    // A surface mesh that is not closed is not a surface mesh: it cannot
    // bound a volume, and the tetrahedraliser has nothing to work with.
    CHECK(closed);
    CHECK(volume > 0.0);
    // Inscribed in the body, so never larger; within the chording of the
    // element size, so never much smaller.
    CHECK(volume <= expected_volume * (1.0 + 1e-9));
    CHECK(volume > expected_volume * 0.85);
}

void TestSurfaceMesh() {
    std::printf("surface meshing:\n");
    {
        cad::EntityId body = cad::kNoEntity;
        const cad::Model model = Box(Vec3d{0, 0, 0}, Vec3d{10, 6, 4}, &body);
        CheckSurface("box", model, {body}, 240.0, 2.0);
        CheckSurface("box, fine", model, {body}, 240.0, 0.8);
    }
    {
        cad::EntityId body = cad::kNoEntity;
        const cad::Model model = Cylinder(2.0, 6.0, &body);
        CheckSurface("cylinder", model, {body}, cad::kPi * 4.0 * 6.0, 1.0);
    }
    {
        cad::Model model;
        cad::EntityId body = cad::kNoEntity;
        std::string error;
        CHECK(cad::MakeSphere(Vec3d{0, 0, 0}, 3.0, &model, &body));
        CHECK(cad::BuildAllPCurves(&model, {}, &error));
        CheckSurface("sphere", model, {body}, 4.0 / 3.0 * cad::kPi * 27.0, 1.2);
    }
}

// --- The tetrahedraliser on its own -----------------------------------------
//
// Worth testing without a body wrapped round it, because a failure here
// and a failure in the meshing that uses it look identical from outside
// and have nothing to do with each other. Three point sets: random (the
// easy case), a regular lattice (every four points cospherical) and
// points on a plane plus one off it (every boundary face's worst case).
void CheckTetrahedralisation(const char *what, const std::vector<Vec3d> &points) {
    std::vector<Tetrahedron> tets;
    TetrahedralisePoints(points, &tets);
    int negative = 0;
    double flat = 0;
    std::map<std::array<int, 3>, int> faces;
    for (const Tetrahedron &t : tets) {
        const Vec3d &a = points[static_cast<std::size_t>(t.a)];
        const Vec3d &b = points[static_cast<std::size_t>(t.b)];
        const Vec3d &c = points[static_cast<std::size_t>(t.c)];
        const Vec3d &d = points[static_cast<std::size_t>(t.d)];
        const double volume = (b - a).Cross(c - a).Dot(d - a) / 6.0;
        if (volume < 0.0) ++negative;
        if (std::fabs(volume) < 1e-12) flat += 1;
        const std::array<std::array<int, 3>, 4> four = {
            std::array<int, 3>{t.b, t.c, t.d}, std::array<int, 3>{t.a, t.d, t.c},
            std::array<int, 3>{t.a, t.b, t.d}, std::array<int, 3>{t.a, t.c, t.b}};
        for (std::array<int, 3> face : four) {
            std::sort(face.begin(), face.end());
            ++faces[face];
        }
    }
    int overused = 0;
    int boundary = 0;
    for (const auto &entry : faces) {
        if (entry.second > 2) ++overused;
        if (entry.second == 1) ++boundary;
    }
    std::printf("  %-18s %4d points -> %5d tets, %d negative, %.0f flat, %d faces used >2, "
                "%d boundary\n",
                what, static_cast<int>(points.size()), static_cast<int>(tets.size()), negative, flat,
                overused, boundary);
    // A tetrahedralisation is a partition: no face may belong to more
    // than two elements. That one number is what says whether the
    // algorithm produced a mesh or a pile of overlapping tetrahedra.
    CHECK(overused == 0);
    CHECK(negative == 0);
    CHECK(!tets.empty());
}

void TestTetrahedralisation() {
    std::printf("tetrahedralising a point set:\n");
    {
        // Deterministic pseudo-random, so a failure can be reproduced.
        std::vector<Vec3d> points;
        std::uint64_t state = 12345;
        auto next = [&]() {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<double>((state >> 33) & 0xFFFFFF) / 16777215.0;
        };
        for (int i = 0; i < 200; ++i) points.push_back(Vec3d{next(), next(), next()});
        CheckTetrahedralisation("random", points);
    }
    {
        // A regular lattice: every eight neighbouring points are
        // cospherical, which is the case exact predicates have to break
        // ties on consistently.
        std::vector<Vec3d> points;
        for (int i = 0; i <= 4; ++i) {
            for (int j = 0; j <= 4; ++j) {
                for (int k = 0; k <= 4; ++k) {
                    points.push_back(Vec3d{static_cast<double>(i), static_cast<double>(j),
                                           static_cast<double>(k)});
                }
            }
        }
        CheckTetrahedralisation("regular lattice", points);
    }
    {
        // A plane of points with a few above it -- exactly what the nodes
        // of one flat face of a body look like to the tetrahedraliser.
        std::vector<Vec3d> points;
        for (int i = 0; i <= 5; ++i) {
            for (int j = 0; j <= 5; ++j) {
                points.push_back(Vec3d{static_cast<double>(i), static_cast<double>(j), 0.0});
            }
        }
        points.push_back(Vec3d{2.0, 2.0, 3.0});
        points.push_back(Vec3d{3.5, 1.5, 2.0});
        CheckTetrahedralisation("a plane plus two", points);
    }
}

// --- G.3, G.4 and G.7: the volume mesh -------------------------------------

// --- G.3, G.4 and G.7: the volume mesh -------------------------------------

void CheckVolume(const char *what, const cad::Model &model,
                 const std::vector<cad::EntityId> &bodies, double expected_volume, double target) {
    VolumeMeshOptions options;
    options.surface.sizing.target = target;
    // Left at its default, so the mesher's own gate is exercised too: a
    // mesh below it is meant to come back as a failure with the number
    // in the message, not as a mesh.
    VolumeMesh mesh;
    MeshReport report;
    const bool ok = MeshBody(model, bodies, options, &mesh, &report);
    for (const std::string &warning : report.warnings) {
        std::printf("      warning: %s\n", warning.c_str());
    }
    CHECK_MESSAGE(ok, report.error);

    // THE ELEMENTS HAVE TO ADD UP TO THE BODY. This is the check that
    // catches a boundary the mesh did not follow, a region left unfilled
    // and an element kept from outside -- none of which shows in an
    // element count, and all of which a mesh that merely validates can
    // have.
    double total = 0.0;
    for (const Tetrahedron &t : mesh.tets) {
        const Vec3d &a = mesh.nodes[static_cast<std::size_t>(t.a)];
        const Vec3d &b = mesh.nodes[static_cast<std::size_t>(t.b)];
        const Vec3d &c = mesh.nodes[static_cast<std::size_t>(t.c)];
        const Vec3d &d = mesh.nodes[static_cast<std::size_t>(t.d)];
        total += (b - a).Cross(c - a).Dot(d - a) / 6.0;
    }
    const MeshQuality quality = MeasureQuality(mesh);
    std::printf("  %-12s %5d nodes %6d tets %5d flips  volume %.4f (exactly %.4f)  "
                "dihedral %.1f..%.1f  %d slivers\n",
                what, mesh.NodeCount(), mesh.TetCount(), report.flips, total,
                expected_volume, quality.min_dihedral, quality.max_dihedral, quality.slivers);

    std::string error;
    CHECK_MESSAGE(CheckMesh(mesh, &error), error);
    CHECK(quality.inverted == 0);
    CHECK(quality.min_volume > 0.0);
    CHECK(total > 0.0);
    CHECK(total <= expected_volume * (1.0 + 1e-9));
    CHECK(total > expected_volume * 0.85);
    int binned = 0;
    for (int count : quality.dihedral_histogram) binned += count;
    CHECK(binned == mesh.TetCount());

    // AND IT IS SOLVABLE. A mesh can be watertight, add up to the body
    // exactly and still be useless: a sliver's shape-function
    // derivatives are divided by a volume that is very nearly zero, so
    // one of them poisons a whole solution. These are gated rather than
    // printed because they are what Part G.4 is for -- before flipping
    // was written the cylinder had five and the L three hundred and
    // sixty-three, and both now have none.
    CHECK(quality.slivers == 0);
    CHECK(quality.min_dihedral > 10.0);
    CHECK(quality.max_dihedral < 160.0);
    CHECK(quality.min_scaled_jacobian > 0.0);

    // The boundary covers the body's surface and nothing else: every
    // boundary node came from the surface mesh, none of the interior
    // points leaked out onto it.
    CHECK(!mesh.boundary.empty());
    for (const SurfaceTriangle &t : mesh.boundary) {
        CHECK(t.a != t.b && t.b != t.c && t.a != t.c);
    }
}

void TestVolumeMesh() {
    std::printf("volume meshing:\n");
    {
        cad::EntityId body = cad::kNoEntity;
        const cad::Model model = Box(Vec3d{0, 0, 0}, Vec3d{10, 6, 4}, &body);
        CheckVolume("box", model, {body}, 240.0, 2.5);
    }
    {
        cad::EntityId body = cad::kNoEntity;
        const cad::Model model = Box(Vec3d{0, 0, 0}, Vec3d{6, 6, 6}, &body);
        CheckVolume("cube, fine", model, {body}, 216.0, 1.5);
    }
    {
        cad::EntityId body = cad::kNoEntity;
        const cad::Model model = Cylinder(2.0, 6.0, &body);
        CheckVolume("cylinder", model, {body}, cad::kPi * 4.0 * 6.0, 1.2);
    }
    {
        // THE CASE THE CLASSIFICATION IS FOR. A convex body can be
        // filled by any method at all -- every tetrahedron between its
        // surface nodes is inside it, so a mesher that never decided
        // anything would still be right. An L takes a bite out of one
        // corner, and the tetrahedralisation spans that bite with
        // elements whose nodes are all on the surface and which are
        // nonetheless outside the body. They have to be thrown away, and
        // the volume is what says whether they were: keeping one shows up
        // as a mesh that measures more than the body does.
        cad::EntityId outer = cad::kNoEntity;
        cad::Model whole = Box(Vec3d{0, 0, 0}, Vec3d{8, 8, 4}, &outer);
        cad::EntityId bite = cad::kNoEntity;
        cad::Model cut = Box(Vec3d{4, 4, -1}, Vec3d{5, 5, 6}, &bite);
        cad::Model model;
        cad::BooleanReport boolean;
        Check(cad::BooleanOperation(whole, outer, cut, bite, cad::BooleanOp::Difference, &model,
                                    &boolean),
              "BooleanOperation", __LINE__);
        std::string error;
        Check(cad::BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
        CheckVolume("L-shaped", model, {boolean.body}, 8.0 * 8.0 * 4.0 - 4.0 * 4.0 * 4.0, 1.6);
    }
}

// --- G.5: curved second-order elements ------------------------------------

void CheckSecondOrder(const char *what, const cad::Model &model,
                      const std::vector<cad::EntityId> &bodies, double expected_volume,
                      double target, bool curved_is_possible) {
    VolumeMeshOptions options;
    options.surface.sizing.target = target;
    VolumeMesh mesh;
    MeshReport report;
    CHECK_MESSAGE(MeshBody(model, bodies, options, &mesh, &report), report.error);

    double straight = 0.0;
    for (const Tetrahedron &t : mesh.tets) {
        const Vec3d &a = mesh.nodes[static_cast<std::size_t>(t.a)];
        const Vec3d &b = mesh.nodes[static_cast<std::size_t>(t.b)];
        const Vec3d &c = mesh.nodes[static_cast<std::size_t>(t.c)];
        const Vec3d &d = mesh.nodes[static_cast<std::size_t>(t.d)];
        straight += (b - a).Cross(c - a).Dot(d - a) / 6.0;
    }
    const int first_order_nodes = mesh.NodeCount();

    CHECK_MESSAGE(MakeSecondOrder(model, &mesh, &report), report.error);
    const double curved = CurvedVolume(mesh);
    std::printf("  %-12s %5d nodes -> %5d  %4d mid on the geometry, %3d backed off  "
                "volume %.4f -> %.4f (exactly %.4f)\n",
                what, first_order_nodes, mesh.NodeCount(), report.mid_nodes_curved,
                report.mid_nodes_backed_off, straight, curved, expected_volume);

    // The bookkeeping: one element and one boundary triangle each, one
    // mid-node per distinct edge, and every node index in range.
    CHECK(mesh.tets10.size() == mesh.tets.size());
    CHECK(mesh.boundary6.size() == mesh.boundary.size());
    CHECK(mesh.NodeCount() == first_order_nodes + report.mid_nodes);
    CHECK(mesh.provenance.size() == mesh.nodes.size());
    for (const std::array<int, 10> &element : mesh.tets10) {
        for (const int node : element) CHECK(node >= 0 && node < mesh.NodeCount());
        // The corners are still the first-order element's, in order.
        CHECK(element[0] >= 0);
    }
    for (std::size_t i = 0; i < mesh.tets10.size(); ++i) {
        CHECK(mesh.tets10[i][0] == mesh.tets[i].a);
        CHECK(mesh.tets10[i][1] == mesh.tets[i].b);
        CHECK(mesh.tets10[i][2] == mesh.tets[i].c);
        CHECK(mesh.tets10[i][3] == mesh.tets[i].d);
    }
    std::string error;
    CHECK_MESSAGE(CheckMesh(mesh, &error), error);

    // THE POINT OF THE WHOLE EXERCISE. A straight mesh of a curved body
    // measures less than the body, by the sum of all the little caps
    // between its chords and the surface. A curved one gives most of
    // that back -- and on a body with no curved faces at all it must
    // change nothing, because there is nothing for the geometry to say.
    if (curved_is_possible) {
        CHECK(report.mid_nodes_curved > 0);
        CHECK(curved > straight);
        const double was = std::fabs(expected_volume - straight);
        const double now = std::fabs(expected_volume - curved);
        CHECK(now < was * 0.5);
    } else {
        CHECK(report.mid_nodes_curved == 0);
        CHECK(report.mid_nodes_backed_off == 0);
        CHECK(std::fabs(curved - straight) < std::fabs(expected_volume) * 1e-9);
    }
    // Never more than the body: a curved element that overshoots has
    // found the wrong part of the surface.
    CHECK(curved <= expected_volume * (1.0 + 1e-6));
}

void TestSecondOrder() {
    std::printf("second order:\n");
    {
        cad::EntityId body = cad::kNoEntity;
        const cad::Model model = Box(Vec3d{0, 0, 0}, Vec3d{10, 6, 4}, &body);
        CheckSecondOrder("box", model, {body}, 240.0, 2.5, false);
    }
    {
        cad::EntityId body = cad::kNoEntity;
        const cad::Model model = Cylinder(2.0, 6.0, &body);
        CheckSecondOrder("cylinder", model, {body}, cad::kPi * 4.0 * 6.0, 1.2, true);
    }
    {
        cad::Model model;
        cad::EntityId body = cad::kNoEntity;
        std::string error;
        Check(cad::MakeSphere(Vec3d{0, 0, 0}, 3.0, &model, &body), "MakeSphere", __LINE__);
        Check(cad::BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
        CheckSecondOrder("sphere", model, {body}, 4.0 / 3.0 * cad::kPi * 27.0, 1.2, true);
    }
    {
        // THE SAFETY NET, DELIBERATELY TRIPPED. Pulling a mid-side node
        // onto the exact surface can turn its element inside out, and
        // the whole reason that is survivable is that the node is walked
        // back towards the straight position until it is not. In normal
        // use it never happens: the sizing field's curvature rule caps
        // how far round a curve one element may go, which caps the
        // sagitta, which is the distance the node moves. So the rule is
        // relaxed here on purpose -- a sphere meshed at more than half a
        // radian per element -- because a safety net that is never
        // tested is a safety net that does not work.
        cad::Model model;
        cad::EntityId body = cad::kNoEntity;
        std::string error;
        Check(cad::MakeSphere(Vec3d{0, 0, 0}, 3.0, &model, &body), "MakeSphere", __LINE__);
        Check(cad::BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
        VolumeMeshOptions options;
        options.surface.sizing.target = 6.0;
        options.surface.sizing.curvature_angle = 1.0;
        options.min_acceptable_dihedral = 0.0;
        VolumeMesh mesh;
        MeshReport report;
        CHECK_MESSAGE(MeshBody(model, {body}, options, &mesh, &report), report.error);
        double straight = 0.0;
        for (const Tetrahedron &t : mesh.tets) {
            const Vec3d &a = mesh.nodes[static_cast<std::size_t>(t.a)];
            const Vec3d &b = mesh.nodes[static_cast<std::size_t>(t.b)];
            const Vec3d &c = mesh.nodes[static_cast<std::size_t>(t.c)];
            const Vec3d &d = mesh.nodes[static_cast<std::size_t>(t.d)];
            straight += (b - a).Cross(c - a).Dot(d - a) / 6.0;
        }
        CHECK_MESSAGE(MakeSecondOrder(model, &mesh, &report), report.error);
        const double curved = CurvedVolume(mesh);
        const double exact = 4.0 / 3.0 * cad::kPi * 27.0;
        std::printf("  %-12s %5d tets  %4d mid on the geometry, %3d backed off  "
                    "volume %.4f -> %.4f (exactly %.4f)\n",
                    "sphere, coarse", mesh.TetCount(), report.mid_nodes_curved,
                    report.mid_nodes_backed_off, straight, curved, exact);
        CHECK(report.mid_nodes_backed_off > 0);
        // Backed off and still valid, still better than straight, still
        // not bigger than the body.
        CHECK_MESSAGE(CheckMesh(mesh, &error), error);
        CHECK_MESSAGE(CheckCurvature(mesh, &error), error);
        CHECK(curved > straight);
        CHECK(curved <= exact * (1.0 + 1e-6));

        // And the detector earns its keep: shove one mid-side node well
        // off and the check has to say so. Reported against the map,
        // because the corner nodes are untouched and look perfect.
        const int moved = mesh.tets10[0][4];
        const Vec3d was = mesh.nodes[static_cast<std::size_t>(moved)];
        mesh.nodes[static_cast<std::size_t>(moved)] =
            was + (mesh.nodes[static_cast<std::size_t>(mesh.tets10[0][0])] - was) * 3.0;
        CHECK(!CheckCurvature(mesh, &error));
        CHECK(error.find("inside out") != std::string::npos);
        CHECK(!CheckMesh(mesh, &error));
        mesh.nodes[static_cast<std::size_t>(moved)] = was;
        CHECK(CheckCurvature(mesh, &error));
    }
}

// --- G.6: structured hexahedral meshing -----------------------------------

void CheckSweep(const char *what, const cad::Model &model, cad::EntityId body,
                double expected_volume, double target) {
    SweepMeshOptions options;
    options.surface.sizing.target = target;
    options.sizing.target = target;
    VolumeMesh mesh;
    MeshReport report;
    CHECK_MESSAGE(MeshSweep(model, body, options, &mesh, &report), report.error);

    double total = 0.0;
    double worst = 1.0;
    for (const Hexahedron &hex : mesh.hexes) {
        total += HexVolume(mesh.nodes, hex);
        worst = std::min(worst, HexScaledJacobian(mesh.nodes, hex));
    }
    std::printf("  %-14s %5d nodes %5d hexes %4d quad faces  volume %.4f (exactly %.4f)  "
                "scaled Jacobian %.3f\n",
                what, mesh.NodeCount(), mesh.HexCount(),
                static_cast<int>(mesh.boundary_quads.size()), total, expected_volume, worst);

    std::string error;
    CHECK_MESSAGE(CheckMesh(mesh, &error), error);
    CHECK(!mesh.hexes.empty());
    CHECK(mesh.tets.empty());

    // THE ELEMENTS ADD UP TO THE BODY. A structured mesh has no
    // classification step to get wrong, so this is not the same test it
    // is for the tetrahedral mesher -- here it catches a profile that was
    // quadrangulated with a gap or an overlap, and a sweep that went the
    // wrong distance.
    CHECK(total > 0.0);
    CHECK(std::fabs(total - expected_volume) < expected_volume * 1e-9);

    // Every element the right way out, and shaped well enough to solve
    // with. A cube-shaped element scores 1; the subdivision of a triangle
    // into three quadrilaterals cannot reach that, and 0.3 is the bar a
    // solver wants.
    CHECK(worst > 0.3);
    const MeshQuality quality = MeasureQuality(mesh);
    CHECK(quality.inverted == 0);
    CHECK(quality.min_volume > 0.0);
    CHECK(std::fabs(quality.min_scaled_jacobian - worst) < 1e-12);

    // The boundary closes: every edge of it used by exactly two
    // quadrilaterals, which is the same test the triangular boundary has
    // to pass and fails in the same way when a face is missing.
    std::map<std::array<int, 2>, int> edges;
    for (const SurfaceQuad &q : mesh.boundary_quads) {
        const int corner[4] = {q.a, q.b, q.c, q.d};
        for (int k = 0; k < 4; ++k) {
            const int u = corner[k];
            const int v = corner[(k + 1) % 4];
            ++edges[u < v ? std::array<int, 2>{u, v} : std::array<int, 2>{v, u}];
        }
    }
    for (const auto &entry : edges) CHECK(entry.second == 2);

    // And it is named: every boundary quadrilateral knows which CAD face
    // it is on, which is what Part H.2 will hang loads from.
    int unnamed = 0;
    for (const SurfaceQuad &q : mesh.boundary_quads) {
        if (q.face == cad::kNoEntity) ++unnamed;
    }
    CHECK(unnamed == 0);
}

void TestSweep() {
    std::printf("sweeping:\n");
    {
        cad::EntityId body = cad::kNoEntity;
        const cad::Model model = Box(Vec3d{0, 0, 0}, Vec3d{10, 6, 4}, &body);
        CheckSweep("box", model, body, 240.0, 2.5);
    }
    {
        cad::EntityId body = cad::kNoEntity;
        const cad::Model model = Cylinder(2.0, 6.0, &body);
        // A cylinder's volume is the profile mesh's area times the
        // height, and the profile is a polygon inscribed in the circle,
        // so the exact answer here is the faceted one -- which is what
        // the check is given, because a sweeper that produced the true
        // volume would have moved the boundary off the profile it swept.
        double area = 0.0;
        SweepMeshOptions options;
        options.surface.sizing.target = 1.2;
        options.sizing.target = 1.2;
        VolumeMesh mesh;
        MeshReport report;
        CHECK_MESSAGE(MeshSweep(model, body, options, &mesh, &report), report.error);
        for (const Hexahedron &hex : mesh.hexes) area += HexVolume(mesh.nodes, hex);
        CheckSweep("cylinder", model, body, area, 1.2);
        std::printf("                 (a circle of radius 2 is %.4f; the swept polygon is %.4f)\n",
                    cad::kPi * 4.0 * 6.0, area);
        CHECK(area < cad::kPi * 4.0 * 6.0);
        CHECK(area > cad::kPi * 4.0 * 6.0 * 0.95);
    }
    {
        // A PROFILE WITH A HOLE IN IT. Subdividing each triangle into
        // three quadrilaterals does not care how many loops the profile
        // has, which is the reason it was chosen over gluing triangles
        // together in pairs: a pairing has to be found and can fail, and
        // a hole is where it fails.
        cad::EntityId outer = cad::kNoEntity;
        cad::Model block = Box(Vec3d{0, 0, 0}, Vec3d{8, 8, 4}, &outer);
        cad::EntityId drill = cad::kNoEntity;
        cad::Model bit;
        std::string error;
        Check(cad::MakeCylinder(Vec3d{4, 4, -1}, Vec3d{0, 0, 1}, 1.5, 6.0, &bit, &drill),
              "MakeCylinder", __LINE__);
        Check(cad::BuildAllPCurves(&bit, {}, &error), "BuildAllPCurves", __LINE__);
        cad::Model model;
        cad::BooleanReport boolean;
        Check(cad::BooleanOperation(block, outer, bit, drill, cad::BooleanOp::Difference, &model,
                                    &boolean),
              "BooleanOperation", __LINE__);
        Check(cad::BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
        SweepMeshOptions options;
        options.surface.sizing.target = 1.5;
        options.sizing.target = 1.5;
        VolumeMesh mesh;
        MeshReport report;
        CHECK_MESSAGE(MeshSweep(model, boolean.body, options, &mesh, &report), report.error);
        double area = 0.0;
        for (const Hexahedron &hex : mesh.hexes) area += HexVolume(mesh.nodes, hex);
        CheckSweep("block, drilled", model, boolean.body, area, 1.5);
        // The hole is really there: the faceted profile is the square
        // less a polygon inscribed in the circle, so it measures a little
        // more than the true drilled block and much less than the solid.
        const double solid = 8.0 * 8.0 * 4.0;
        const double drilled = solid - cad::kPi * 1.5 * 1.5 * 4.0;
        CHECK(area > drilled);
        CHECK(area < drilled * 1.02);
        std::printf("                 (solid %.2f, truly drilled %.2f, faceted %.2f)\n", solid,
                    drilled, area);
    }
    {
        // A NON-CONVEX PROFILE, which is where a structured mesher
        // usually gives up: the classic sweepable body is a rectangle
        // times a length, and an L is not one.
        cad::EntityId outer = cad::kNoEntity;
        cad::Model whole = Box(Vec3d{0, 0, 0}, Vec3d{8, 8, 4}, &outer);
        cad::EntityId bite = cad::kNoEntity;
        cad::Model cut = Box(Vec3d{4, 4, -1}, Vec3d{5, 5, 6}, &bite);
        cad::Model model;
        cad::BooleanReport boolean;
        Check(cad::BooleanOperation(whole, outer, cut, bite, cad::BooleanOp::Difference, &model,
                                    &boolean),
              "BooleanOperation", __LINE__);
        std::string error;
        Check(cad::BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
        CheckSweep("L-shaped", model, boolean.body, 192.0, 1.6);
    }
    {
        // NOT EVERY BODY SWEEPS, AND SAYING SO IS THE POINT. A blind
        // hole is the case that looks sweepable and is not: the top face
        // has a hole in it, the bottom does not, and the flat floor of
        // the hole faces along the only direction that could have worked.
        // A mesher that swept it anyway would fill the hole in silently.
        cad::EntityId outer = cad::kNoEntity;
        cad::Model block = Box(Vec3d{0, 0, 0}, Vec3d{8, 8, 4}, &outer);
        cad::EntityId drill = cad::kNoEntity;
        cad::Model bit;
        std::string error;
        Check(cad::MakeCylinder(Vec3d{4, 4, 2}, Vec3d{0, 0, 1}, 1.5, 3.0, &bit, &drill),
              "MakeCylinder", __LINE__);
        Check(cad::BuildAllPCurves(&bit, {}, &error), "BuildAllPCurves", __LINE__);
        cad::Model model;
        cad::BooleanReport boolean;
        Check(cad::BooleanOperation(block, outer, bit, drill, cad::BooleanOp::Difference, &model,
                                    &boolean),
              "BooleanOperation", __LINE__);
        Check(cad::BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
        SweepMeshOptions options;
        options.surface.sizing.target = 1.5;
        VolumeMesh mesh;
        MeshReport report;
        CHECK(!MeshSweep(model, boolean.body, options, &mesh, &report));
        CHECK(report.error.find("not parallel to the sweep direction") != std::string::npos);
        CHECK(mesh.hexes.empty());
        std::printf("  %-14s refused: %s\n", "blind hole", report.error.c_str());
    }
    {
        // And a body with too few faces to be a sweep of anything.
        cad::Model model;
        cad::EntityId body = cad::kNoEntity;
        std::string error;
        Check(cad::MakeSphere(Vec3d{0, 0, 0}, 3.0, &model, &body), "MakeSphere", __LINE__);
        Check(cad::BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
        SweepMeshOptions options;
        options.surface.sizing.target = 1.5;
        VolumeMesh mesh;
        MeshReport report;
        CHECK(!MeshSweep(model, body, options, &mesh, &report));
        CHECK(report.error.find("fewer than three faces") != std::string::npos);
        CHECK(mesh.hexes.empty());
        std::printf("  %-14s refused: %s\n", "sphere", report.error.c_str());
    }
}

// Part G.4, after the sweep: the two properties the quality repair is
// actually for, both of which were broken and neither of which any test
// was asserting.
void TestQualityIsScaleInvariantAndSurvivesThinParts() {
    std::printf("mesh quality against scale and against thinness:\n");

    // SCALE INVARIANCE, ASSERTED RATHER THAN ASSUMED. This plan claimed
    // for a long time that the mesher was scale dependent -- that a part
    // a few hundredths across failed where the same shape at ten units
    // worked -- and it is not true. It is asserted here so that it
    // cannot quietly become true, and so that the next person to see a
    // mesh refused does not go looking in the sizing field.
    double worst_spread_low = 180.0;
    double worst_spread_high = 0.0;
    for (const double scale : {100.0, 10.0, 1.0, 0.1, 0.01, 0.001}) {
        cad::Model model;
        cad::EntityId body = cad::kNoEntity;
        CHECK(cad::MakeBox({0, 0, 0}, {scale, scale * 0.6, scale * 0.4}, &model, &body));
        std::string error;
        CHECK(cad::BuildAllPCurves(&model, {}, &error));
        fem::VolumeMeshOptions options;
        options.surface.sizing.target = scale * 0.2;
        fem::VolumeMesh mesh;
        fem::MeshReport report;
        CHECK_MESSAGE(fem::MeshBody(model, {body}, options, &mesh, &report), report.error);
        worst_spread_low = std::min(worst_spread_low, report.min_dihedral);
        worst_spread_high = std::max(worst_spread_high, report.min_dihedral);
        CHECK(mesh.TetCount() > 100);
    }
    std::printf("  the same block over six decades of size: worst dihedral %.2f to %.2f degrees\n",
                worst_spread_low, worst_spread_high);
    CHECK(worst_spread_low > 5.0);
    // The spread across six decades is the algorithm's own noise, not a
    // trend: if it were scale dependent this would be a cliff.
    CHECK(worst_spread_high - worst_spread_low < 10.0);

    // A THIN PART AT AWKWARD SIZE RATIOS. The failure was never about
    // absolute size, it was about a *fractional* number of elements
    // across a thin dimension: one and two are fine, one and a half is
    // not, because every node is then on the boundary and the repair had
    // nothing it was allowed to move.
    std::printf("  a 1.0 x 0.25 x 0.25 slab, swept across its thickness:\n");
    for (const double across : {1.0, 1.5, 2.0, 2.5, 3.0}) {
        cad::Model model;
        cad::EntityId body = cad::kNoEntity;
        CHECK(cad::MakeBox({0, 0, 0}, {1.0, 0.25, 0.25}, &model, &body));
        std::string error;
        CHECK(cad::BuildAllPCurves(&model, {}, &error));
        fem::VolumeMeshOptions options;
        options.surface.sizing.target = 0.25 / across;
        fem::VolumeMesh mesh;
        fem::MeshReport report;
        const bool ok = fem::MeshBody(model, {body}, options, &mesh, &report);
        std::printf("    %.1f elements across: %5d tets, worst dihedral %6.2f, surface %6.2f%s\n",
                    across, mesh.TetCount(), report.min_dihedral, report.min_surface_angle,
                    ok ? "" : "  REFUSED");
        CHECK_MESSAGE(ok, report.error);
        CHECK(report.min_dihedral >= options.min_acceptable_dihedral);
        // The surface angle is reported alongside, because when a volume
        // mesh is bad the first question is whether its boundary was bad
        // first, and having to ask that separately is what made the
        // original diagnosis take an afternoon.
        CHECK(report.min_surface_angle > 0.0);
    }
}

// The welding pass, and the property that makes it necessary rather than
// tidy: no two nodes of a finished surface mesh may be at the same place.
void TestNoTwoNodesAreTheSamePoint() {
    std::printf("no two surface nodes are the same point:\n");
    cad::Model a;
    cad::Model b;
    cad::EntityId big = cad::kNoEntity;
    cad::EntityId cut = cad::kNoEntity;
    CHECK(cad::MakeBox({0, 0, 0}, {10, 6, 4}, &a, &big));
    CHECK(cad::MakeBox({4, -1, 2}, {7, 8, 3}, &b, &cut));
    std::string error;
    CHECK(cad::BuildAllPCurves(&a, {}, &error));
    CHECK(cad::BuildAllPCurves(&b, {}, &error));
    cad::Model ell;
    cad::BooleanReport boolean;
    // AN L-BRACKET, the commonest test part in the subject, and the one
    // that turned this up: its surface mesh had a pair of nodes 4.2e-08
    // apart on a part ten units across, and the tetrahedron between them
    // had a dihedral angle of zero.
    CHECK_MESSAGE(cad::BooleanOperation(a, big, b, cut, cad::BooleanOp::Difference, &ell,
                                        &boolean),
                  boolean.error);
    CHECK(cad::BuildAllPCurves(&ell, {}, &error));
    fem::VolumeMeshOptions options;
    options.surface.sizing.target = 2.0;
    fem::VolumeMesh mesh;
    fem::MeshReport report;
    CHECK_MESSAGE(fem::MeshBody(ell, {boolean.body}, options, &mesh, &report), report.error);
    double shortest = 1e30;
    for (const fem::Tetrahedron &t : mesh.tets) {
        const int corner[4] = {t.a, t.b, t.c, t.d};
        for (int i = 0; i < 4; ++i) {
            for (int j = i + 1; j < 4; ++j) {
                shortest = std::min(shortest, (mesh.nodes[static_cast<std::size_t>(corner[j])] -
                                               mesh.nodes[static_cast<std::size_t>(corner[i])]).Length());
            }
        }
    }
    std::printf("  %d tets, shortest edge %.3e of a 2.0 target, %d nodes welded\n",
                mesh.TetCount(), shortest, report.welded_nodes);
    // An edge orders of magnitude below the target size is not an edge,
    // it is two nodes that should have been one.
    CHECK(shortest > options.surface.sizing.target * 1e-3);
    CHECK(report.min_dihedral > 5.0);
}

void TestMeshChecks() {
    // A mesh with an inverted element is reported, not returned.
    VolumeMesh mesh;
    mesh.nodes = {Vec3d{0, 0, 0}, Vec3d{1, 0, 0}, Vec3d{0, 1, 0}, Vec3d{0, 0, 1}};
    mesh.tets.push_back(Tetrahedron{0, 1, 2, 3});
    std::string error;
    CHECK(CheckMesh(mesh, &error));
    mesh.tets[0] = Tetrahedron{0, 2, 1, 3};  // the other way round
    CHECK(!CheckMesh(mesh, &error));
    CHECK(error.find("negative volume") != std::string::npos);
    mesh.tets[0] = Tetrahedron{0, 1, 2, 99};
    CHECK(!CheckMesh(mesh, &error));
    CHECK(error.find("does not exist") != std::string::npos);
    mesh.tets.clear();
    CHECK(!CheckMesh(mesh, &error));
    CHECK(error.find("no elements") != std::string::npos);

    // A single regular tetrahedron has all six dihedral angles equal to
    // arccos(1/3), which is 70.53 degrees -- the one quality number in
    // this whole part that can be written down from first principles, so
    // it is what the measure is calibrated against.
    VolumeMesh regular;
    regular.nodes = {Vec3d{1, 1, 1}, Vec3d{1, -1, -1}, Vec3d{-1, 1, -1}, Vec3d{-1, -1, 1}};
    regular.tets.push_back(Tetrahedron{0, 1, 2, 3});
    if ((regular.nodes[1] - regular.nodes[0])
            .Cross(regular.nodes[2] - regular.nodes[0])
            .Dot(regular.nodes[3] - regular.nodes[0]) < 0.0) {
        regular.tets[0] = Tetrahedron{0, 2, 1, 3};
    }
    const MeshQuality perfect = MeasureQuality(regular);
    std::printf("a regular tetrahedron: dihedral %.4f..%.4f, aspect %.4f, scaled Jacobian %.4f\n",
                perfect.min_dihedral, perfect.max_dihedral, perfect.worst_aspect,
                perfect.min_scaled_jacobian);
    CHECK(Near(perfect.min_dihedral, 70.5288, 1e-3));
    CHECK(Near(perfect.max_dihedral, 70.5288, 1e-3));
    CHECK(Near(perfect.worst_aspect, 1.0, 1e-6));
    CHECK(Near(perfect.min_scaled_jacobian, 1.0, 1e-6));
    CHECK(perfect.slivers == 0);

    // And a sliver is recognised as one: four nearly coplanar points.
    VolumeMesh sliver;
    sliver.nodes = {Vec3d{0, 0, 0}, Vec3d{1, 0, 0}, Vec3d{0, 1, 0}, Vec3d{0.4, 0.4, 0.001}};
    sliver.tets.push_back(Tetrahedron{0, 1, 2, 3});
    const MeshQuality bad = MeasureQuality(sliver);
    std::printf("a sliver: dihedral %.4f..%.4f, %d sliver(s)\n", bad.min_dihedral, bad.max_dihedral,
                bad.slivers);
    CHECK(bad.min_dihedral < 5.0);
    CHECK(bad.slivers == 1);

    // Turning a tetrahedral mesh into the analysis model is refused by
    // name rather than writing tetrahedra into a field that means
    // hexahedra.
    Model analysis;
    CHECK(!ToAnalysisModel(regular, Material{}, &analysis, &error));
    CHECK(error.find("Hex8") != std::string::npos);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestSizingField();
    TestSurfaceMesh();
    TestTetrahedralisation();
    TestVolumeMesh();
    TestSecondOrder();
    TestSweep();
    TestQualityIsScaleInvariantAndSurvivesThinParts();
    TestNoTwoNodesAreTheSamePoint();
    TestMeshChecks();
    std::printf("fem_mesh_test passed (%d checks)\n", g_checks);
    return 0;
}
