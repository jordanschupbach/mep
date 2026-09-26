// Part G.7: the mesher measured against somebody else's.
//
// Every other test in this part checks the mesh against itself or against
// a number that can be written down: the elements add up to the body, no
// face belongs to three of them, a regular tetrahedron's dihedral angle
// is 70.5288 degrees. Those catch a great deal and they cannot catch one
// thing -- a mesh that is valid, watertight, correctly sized and simply
// much worse than it should be. Nothing internal says what "should" is.
// So this compares against Gmsh, on the same geometry at the same
// requested size, with the quality of both measured by the *same* code:
// this library's own metrics, applied to the other mesher's elements.
//
// Comparing Gmsh's quality report with ours would compare two
// definitions, which is how one gets a comfortable answer and no
// information.
//
// THE GEOMETRY GOES ACROSS AS STEP, WHICH MAKES THIS TWO TESTS AT ONCE.
// Gmsh reads the file through OpenCASCADE, so if it meshes a body of the
// right volume then the STEP this library wrote described the right body
// to a completely independent kernel -- a far stronger statement about
// Part F than reading our own file back can ever be.
//
// AND IT IMMEDIATELY EARNED ITS KEEP. The first run meshed zero elements
// on every body, from files that this library's own reader was perfectly
// happy with, and Gmsh reported no error at all: it said "Done reading"
// and produced nothing. The export had a shape representation and no
// product structure, and a receiver finds its roots by following
// SHAPE_DEFINITION_REPRESENTATION back to a PRODUCT_DEFINITION. With no
// product there are no roots, so the file reads cleanly and yields
// nothing. Eight entities fixed it. Nothing internal to this repository
// would ever have found it, because our own reader does not look for
// roots -- which is the whole argument for this test existing.
//
// Needs Gmsh, so it takes the path to it as an argument rather than
// searching: a test that quietly skips when a tool is missing is a test
// that quietly stops testing.

#include "cad_boolean.h"
#include "cad_feature.h"
#include "cad_pcurve.h"
#include "cad_step.h"
#include "fem_mesh.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char *what, int line) {
    ++checks;
    if (ok) return;
    std::printf("CHECK FAILED: %s at %s:%d\n", what, __FILE__, line);
    ++failures;
}

#define CHECK(x) Check((x), #x, __LINE__)
#define CHECK_MESSAGE(x, message) Check((x), (std::string(#x) + ": " + (message)).c_str(), __LINE__)

using cad::Vec3d;
using fem::MeshQuality;
using fem::Tetrahedron;
using fem::VolumeMesh;

std::string directory;
std::string gmsh;

// --- Reading what Gmsh wrote ---------------------------------------------
//
// Format 2.2 ASCII, which is asked for explicitly because it is a dozen
// lines to parse and 4.1 is not. Node numbers are one-based and need not
// be contiguous, so they are mapped rather than assumed.
bool ReadGmshMesh(const std::string &path, VolumeMesh *out, std::string *error) {
    std::ifstream in(path);
    if (!in) {
        *error = "cannot open " + path;
        return false;
    }
    *out = VolumeMesh{};
    std::map<long long, int> renumbered;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("$Nodes", 0) == 0) {
            long long count = 0;
            in >> count;
            for (long long i = 0; i < count; ++i) {
                long long id = 0;
                double x = 0.0, y = 0.0, z = 0.0;
                in >> id >> x >> y >> z;
                renumbered[id] = out->NodeCount();
                out->nodes.push_back(Vec3d{x, y, z});
                out->provenance.push_back({});
            }
            continue;
        }
        if (line.rfind("$Elements", 0) != 0) continue;
        long long count = 0;
        in >> count;
        for (long long i = 0; i < count; ++i) {
            long long id = 0;
            int type = 0;
            int tags = 0;
            in >> id >> type >> tags;
            for (int t = 0; t < tags; ++t) {
                long long ignored = 0;
                in >> ignored;
            }
            // 15 a point, 1 a line, 2 a triangle, 4 a tetrahedron. Only
            // the last is an element of a volume mesh.
            static const std::map<int, int> nodes_of_type = {{15, 1}, {1, 2}, {2, 3}, {4, 4}};
            const auto found = nodes_of_type.find(type);
            if (found == nodes_of_type.end()) {
                *error = "element type " + std::to_string(type) + " is not one this reads";
                return false;
            }
            std::vector<int> corner;
            for (int k = 0; k < found->second; ++k) {
                long long node = 0;
                in >> node;
                const auto at = renumbered.find(node);
                if (at == renumbered.end()) {
                    *error = "an element refers to node " + std::to_string(node) +
                             ", which the file did not define";
                    return false;
                }
                corner.push_back(at->second);
            }
            if (type == 4) {
                out->tets.push_back(Tetrahedron{corner[0], corner[1], corner[2], corner[3]});
            }
        }
    }
    if (out->tets.empty()) {
        *error = "the file has no tetrahedra";
        return false;
    }
    return true;
}

double VolumeOf(const VolumeMesh &mesh) {
    double total = 0.0;
    for (const Tetrahedron &t : mesh.tets) {
        const Vec3d &a = mesh.nodes[static_cast<std::size_t>(t.a)];
        const Vec3d &b = mesh.nodes[static_cast<std::size_t>(t.b)];
        const Vec3d &c = mesh.nodes[static_cast<std::size_t>(t.c)];
        const Vec3d &d = mesh.nodes[static_cast<std::size_t>(t.d)];
        total += (b - a).Cross(c - a).Dot(d - a) / 6.0;
    }
    return total;
}

struct Outcome {
    int nodes = 0;
    int tets = 0;
    double volume = 0.0;
    double min_dihedral = 0.0;
    double max_dihedral = 0.0;
    int slivers = 0;
    int reversed = 0;
    bool valid = false;
    std::string error;
};

// Gmsh's own winding is its business; what matters is that the elements
// tile the body. They are turned over here so that this library's
// metrics, which are signed, describe their shape rather than their
// bookkeeping -- and how many needed it is reported, because "all of
// them" and "some of them" mean very different things.
Outcome Judge(VolumeMesh *mesh) {
    Outcome out;
    for (Tetrahedron &t : mesh->tets) {
        const Vec3d &a = mesh->nodes[static_cast<std::size_t>(t.a)];
        const Vec3d &b = mesh->nodes[static_cast<std::size_t>(t.b)];
        const Vec3d &c = mesh->nodes[static_cast<std::size_t>(t.c)];
        const Vec3d &d = mesh->nodes[static_cast<std::size_t>(t.d)];
        if ((b - a).Cross(c - a).Dot(d - a) < 0.0) {
            std::swap(t.c, t.d);
            ++out.reversed;
        }
    }
    const MeshQuality quality = fem::MeasureQuality(*mesh);
    out.nodes = mesh->NodeCount();
    out.tets = mesh->TetCount();
    out.volume = VolumeOf(*mesh);
    out.min_dihedral = quality.min_dihedral;
    out.max_dihedral = quality.max_dihedral;
    out.slivers = quality.slivers;
    out.valid = fem::CheckMesh(*mesh, &out.error);
    return out;
}

Outcome RunGmsh(const std::string &name, const std::string &step, double target, int algorithm) {
    Outcome out;
    const std::string mesh_path = directory + "/" + name + "_" + std::to_string(algorithm) + ".msh";
    std::ostringstream command;
    // The same size request as this library was given, as nearly as the
    // two can be made to agree: a maximum element size, and elements per
    // full turn matched to our own radians-per-element.
    command << "'" << gmsh << "' -3 -format msh22 -v 0"
            << " -clmax " << target << " -setnumber Mesh.MeshSizeFromCurvature "
            << static_cast<int>(std::lround(2.0 * cad::kPi / 0.35))
            << " -setnumber Mesh.Algorithm3D " << algorithm << " '" << step << "' -o '"
            << mesh_path << "'";
    if (std::system(command.str().c_str()) != 0) {
        out.error = "gmsh would not run: " + command.str();
        return out;
    }
    VolumeMesh mesh;
    if (!ReadGmshMesh(mesh_path, &mesh, &out.error)) return out;
    return Judge(&mesh);
}

void Compare(const char *what, const cad::Model &model, cad::EntityId body, double analytic,
             double target, double faceting) {
    std::string step;
    std::string error;
    if (!cad::WriteStepShapes(model, {body}, &step, &error)) {
        std::printf("  %-14s STEP export failed: %s\n", what, error.c_str());
        ++failures;
        return;
    }
    const std::string step_path = directory + "/" + what + ".step";
    std::ofstream(step_path) << step;

    fem::VolumeMeshOptions options;
    options.surface.sizing.target = target;
    VolumeMesh mine;
    fem::MeshReport report;
    CHECK_MESSAGE(fem::MeshBody(model, {body}, options, &mine, &report), report.error);
    const Outcome ours = Judge(&mine);
    // The same mesh with its mid-side nodes put on the exact geometry.
    // Reported alongside the first-order ones because it is the one
    // number here where this library is not trying to match Gmsh but to
    // beat it, and on a curved body it does: a second-order mesh measures
    // the sphere to a hundredth of a percent where every first-order mesh
    // of any mesher is several percent short.
    double curved = 0.0;
    {
        VolumeMesh raised = mine;
        fem::MeshReport order;
        if (fem::MakeSecondOrder(model, &raised, &order)) curved = fem::CurvedVolume(raised);
    }
    const Outcome theirs = RunGmsh(what, step_path, target, 1);
    const Outcome netgen = RunGmsh(what, step_path, target, 4);

    std::printf("  %s, target %.2f, exactly %.4f\n", what, target, analytic);
    for (const auto &entry : {std::pair<const char *, const Outcome *>{"mep", &ours},
                              {"gmsh, delaunay", &theirs},
                              {"gmsh, netgen frontal", &netgen}}) {
        const Outcome &o = *entry.second;
        if (!o.error.empty() && o.tets == 0) {
            std::printf("    %-22s failed: %s\n", entry.first, o.error.c_str());
            continue;
        }
        std::printf("    %-22s %5d nodes %6d tets  volume %9.4f (%+6.2f%%)  dihedral %5.1f..%5.1f"
                    "  %3d slivers  %s\n",
                    entry.first, o.nodes, o.tets, o.volume,
                    (o.volume - analytic) / analytic * 100.0, o.min_dihedral, o.max_dihedral,
                    o.slivers, o.valid ? "valid" : o.error.c_str());
    }
    if (curved != 0.0) {
        std::printf("    %-22s %5s %6s %11s volume %9.4f (%+6.2f%%)\n", "mep, second order", "", "",
                    "", curved, (curved - analytic) / analytic * 100.0);
    }

    // GMSH'S MESH HAS TO PASS OUR CHECKER. It is a known-good mesh from a
    // mesher twenty years older than this one, so if our checker rejects
    // it the fault is the checker's. This is the only test in the suite
    // that can catch a checker which is wrong in the strict direction --
    // every other one would simply pass.
    CHECK_MESSAGE(theirs.valid, theirs.error);
    CHECK_MESSAGE(netgen.valid, netgen.error);

    // THE GEOMETRY CROSSED OVER. Gmsh read the body through OpenCASCADE
    // from the STEP this library wrote, so a mesh of the right volume says
    // the file described the right solid to an independent kernel. The
    // allowance is the faceting of *that body* at *this* size and nothing
    // looser: for a polyhedron it is rounding and nothing else, because a
    // flat-faced body meshed with flat elements has no excuse. Not
    // literally zero -- a volume is a sum of a thousand floating-point
    // triple products and the last digit is not free -- but near enough
    // that a single misplaced element would show.
    CHECK(std::fabs(theirs.volume - analytic) <= std::fabs(analytic) * faceting);
    CHECK(std::fabs(netgen.volume - analytic) <= std::fabs(analytic) * faceting);
    CHECK(std::fabs(ours.volume - analytic) <= std::fabs(analytic) * faceting);
    // A convex body meshed with straight elements is always smaller than
    // the body, never larger. This is the check that would catch a mesher
    // of either sort putting an element outside the geometry.
    if (faceting > 1e-6) {
        CHECK(ours.volume < analytic);
        CHECK(theirs.volume < analytic);
    }
    // And the two faceted answers are close to each other, which is a
    // tighter statement than either being close to the body: they face
    // the same faceting, so a gap between them is a difference in where
    // the nodes went rather than in how many there are.
    CHECK(std::fabs(ours.volume - theirs.volume) <= std::fabs(analytic) * 0.025);

    // THE SECOND-ORDER MESH BEATS BOTH FIRST-ORDER ONES, and by a lot
    // where there is curvature to follow. Part G.5's claim, checked
    // against somebody else's mesh rather than only against the analytic
    // number.
    if (curved != 0.0 && faceting > 1e-6) {
        CHECK(std::fabs(curved - analytic) < std::fabs(theirs.volume - analytic) * 0.25);
    }

    // AND OURS IS IN THE SAME LEAGUE. Not identical -- two meshers given
    // the same size never agree element for element -- but a node count
    // off by more than four times either way means the sizing field is
    // not doing what was asked, and a worst angle less than half theirs
    // means the quality work is not.
    CHECK(ours.nodes > theirs.nodes / 4);
    CHECK(ours.nodes < theirs.nodes * 4);
    CHECK(ours.min_dihedral > theirs.min_dihedral * 0.5);
    CHECK(ours.valid);
    // Ours carries no slivers at all, which is a stronger claim than
    // theirs makes and is checked rather than hoped for.
    CHECK(ours.slivers == 0);
}

cad::Model Box(const Vec3d &corner, const Vec3d &size, cad::EntityId *body) {
    cad::Model model;
    std::string error;
    cad::MakeBox(corner, size, &model, body);
    cad::BuildAllPCurves(&model, {}, &error);
    return model;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::printf("usage: mep-fem-mesh-compare-test /path/to/gmsh [scratch directory]\n");
        return 2;
    }
    gmsh = argv[1];
    directory = argc > 2 ? argv[2] : ".";
    std::printf("measured against gmsh, geometry carried across as STEP:\n");

    {
        cad::EntityId body = cad::kNoEntity;
        const cad::Model model = Box(Vec3d{0, 0, 0}, Vec3d{10, 6, 4}, &body);
        Compare("box", model, body, 240.0, 2.5, 1e-9);
    }
    {
        cad::Model model;
        cad::EntityId body = cad::kNoEntity;
        std::string error;
        cad::MakeCylinder(Vec3d{0, 0, 0}, Vec3d{0, 0, 1}, 2.0, 6.0, &model, &body);
        cad::BuildAllPCurves(&model, {}, &error);
        Compare("cylinder", model, body, cad::kPi * 4.0 * 6.0, 1.2, 0.02);
    }
    {
        cad::Model model;
        cad::EntityId body = cad::kNoEntity;
        std::string error;
        cad::MakeSphere(Vec3d{0, 0, 0}, 3.0, &model, &body);
        cad::BuildAllPCurves(&model, {}, &error);
        Compare("sphere", model, body, 4.0 / 3.0 * cad::kPi * 27.0, 1.2, 0.06);
    }
    {
        cad::EntityId outer = cad::kNoEntity;
        cad::Model whole = Box(Vec3d{0, 0, 0}, Vec3d{8, 8, 4}, &outer);
        cad::EntityId bite = cad::kNoEntity;
        cad::Model cut = Box(Vec3d{4, 4, -1}, Vec3d{5, 5, 6}, &bite);
        cad::Model model;
        cad::BooleanReport boolean;
        std::string error;
        if (cad::BooleanOperation(whole, outer, cut, bite, cad::BooleanOp::Difference, &model,
                                  &boolean)) {
            cad::BuildAllPCurves(&model, {}, &error);
            Compare("ell", model, boolean.body, 192.0, 1.6, 1e-9);
        } else {
            std::printf("  the L-shaped body could not be built\n");
            ++failures;
        }
    }

    if (failures != 0) {
        std::printf("fem_mesh_compare_test FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("fem_mesh_compare_test passed (%d checks)\n", checks);
    return 0;
}
