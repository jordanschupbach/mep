// Part H.2: the analysis model bound to named geometry.
//
// The test that matters here is not that a load can be applied. It is
// that the *same study* applies correctly to a model whose dimensions
// have changed and which has been meshed again from scratch -- because
// that is the one thing a mesh-file FEM tool cannot do, and the reason
// the CAD half of this plan exists.

#include "cad_feature.h"
#include "cad_naming.h"
#include "cad_pcurve.h"
#include "fem_study.h"

#include <cmath>
#include <cstdio>
#include <map>
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
using fem::AnalysisModel;
using fem::BindOptions;
using fem::BindReport;
using fem::Load;
using fem::LoadKind;
using fem::MaterialCurve;
using fem::MaterialKind;
using fem::Study;
using fem::StudyMaterial;
using fem::Support;
using fem::Target;
using fem::TargetKind;
using fem::VolumeMesh;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// A box, and the persistent names of two of its faces: the one at the
// bottom and the one at the top. Captured the way a user's click would
// capture them -- from the model as built -- and then used against a
// model built differently.
struct Block {
    cad::Model model;
    cad::EntityId body = cad::kNoEntity;
    fem::Target bottom;
    fem::Target top;
    cad::EntityId bottom_id = cad::kNoEntity;
    cad::EntityId top_id = cad::kNoEntity;
};

Block MakeBlock(const Vec3d &size) {
    Block out;
    std::string error;
    if (!cad::MakeBox(Vec3d{0, 0, 0}, size, &out.model, &out.body)) return out;
    if (!cad::BuildAllPCurves(&out.model, {}, &error)) return out;
    // Find the faces whose outward normal is -z and +z. This is the click:
    // a user picks a face in the viewport and what gets stored is its
    // name, not its id.
    for (const cad::Face &face : out.model.Faces()) {
        const cad::Surface *surface = out.model.SurfaceAt(face.surface);
        if (surface == nullptr) continue;
        double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
        surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
        const Vec3d normal =
            out.model.FaceNormal(face.id, (u_lo + u_hi) * 0.5, (v_lo + v_hi) * 0.5);
        if (normal.z < -0.9) out.bottom_id = face.id;
        if (normal.z > 0.9) out.top_id = face.id;
    }
    // Captured the way a click would capture it, through the helper that
    // gets the role right.
    if (out.bottom_id != cad::kNoEntity) out.bottom = fem::FaceTarget(out.model, out.bottom_id);
    if (out.top_id != cad::kNoEntity) out.top = fem::FaceTarget(out.model, out.top_id);
    return out;
}

bool MeshBlock(const cad::Model &model, cad::EntityId body, double target, VolumeMesh *out) {
    fem::VolumeMeshOptions options;
    options.surface.sizing.target = target;
    fem::MeshReport report;
    if (fem::MeshBody(model, {body}, options, out, &report)) return true;
    std::printf("    meshing failed: %s\n", report.error.c_str());
    return false;
}

Study PressureStudy(const Block &block, double pressure) {
    Study study;
    StudyMaterial steel;
    steel.name = "steel";
    study.materials.push_back(steel);

    Support held;
    held.label = "bottom held";
    held.where = block.bottom;
    study.supports.push_back(held);

    Load push;
    push.label = "pressure on top";
    push.kind = LoadKind::Pressure;
    push.where = block.top;
    push.magnitude = pressure;
    study.loads.push_back(push);
    return study;
}

// --- Materials ------------------------------------------------------------

void TestMaterials() {
    std::printf("materials:\n");
    // A curve with one point is a constant, and held flat outside its
    // range rather than extrapolated.
    MaterialCurve constant = MaterialCurve::Constant(7.0);
    CHECK(constant.At(-1000.0) == 7.0);
    CHECK(constant.At(1000.0) == 7.0);
    MaterialCurve falling;
    falling.points = {{20.0, 210e9}, {200.0, 190e9}, {400.0, 150e9}};
    std::string error;
    CHECK_MESSAGE(falling.IsValid(&error), error);
    CHECK(std::fabs(falling.At(20.0) - 210e9) < 1.0);
    CHECK(std::fabs(falling.At(110.0) - 200e9) < 1e6);
    CHECK(std::fabs(falling.At(300.0) - 170e9) < 1e6);
    // FLAT, NOT EXTRAPOLATED. A modulus extrapolated past 400 goes
    // negative before long, and a negative modulus is not a warning, it
    // is a confident answer of the wrong sign.
    CHECK(falling.At(10000.0) == 150e9);
    CHECK(falling.At(-10000.0) == 210e9);
    MaterialCurve backwards;
    backwards.points = {{400.0, 1.0}, {20.0, 2.0}};
    CHECK(!backwards.IsValid(&error));

    // Temperature dependence reaches the constitutive matrix.
    StudyMaterial hot;
    hot.youngs_modulus = falling;
    double cold_matrix[6][6];
    double hot_matrix[6][6];
    CHECK_MESSAGE(hot.ConstitutiveMatrix(20.0, cold_matrix, &error), error);
    CHECK_MESSAGE(hot.ConstitutiveMatrix(400.0, hot_matrix, &error), error);
    CHECK(hot_matrix[0][0] < cold_matrix[0][0] * 0.8);

    // AN ORTHOTROPIC MATERIAL WITH EQUAL CONSTANTS IS THE ISOTROPIC ONE.
    // The two matrices are built by completely different routes -- one
    // written out as a stiffness, the other filled in as a compliance and
    // inverted -- so agreeing to round-off is a real statement about both.
    StudyMaterial isotropic;
    isotropic.youngs_modulus = MaterialCurve::Constant(210e9);
    isotropic.poissons_ratio = MaterialCurve::Constant(0.3);
    StudyMaterial orthotropic = isotropic;
    orthotropic.kind = MaterialKind::Orthotropic;
    for (int i = 0; i < 3; ++i) {
        orthotropic.e[i] = 210e9;
        orthotropic.nu[i] = 0.3;
        orthotropic.g[i] = 210e9 / (2.0 * 1.3);
    }
    double a[6][6];
    double b[6][6];
    CHECK_MESSAGE(isotropic.ConstitutiveMatrix(20.0, a, &error), error);
    CHECK_MESSAGE(orthotropic.ConstitutiveMatrix(20.0, b, &error), error);
    double worst = 0.0;
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) worst = std::max(worst, std::fabs(a[i][j] - b[i][j]));
    }
    std::printf("  orthotropic with equal constants matches isotropic to %.2e of %.3g\n", worst,
                a[0][0]);
    CHECK(worst < a[0][0] * 1e-12);

    // A stiffer direction is stiffer, which is the whole reason the type
    // exists.
    StudyMaterial wood = orthotropic;
    wood.e[0] = 12e9;
    wood.e[1] = 0.9e9;
    wood.e[2] = 0.6e9;
    wood.nu[0] = 0.4;
    wood.nu[1] = 0.3;
    wood.nu[2] = 0.02;
    wood.g[0] = 0.7e9;
    wood.g[1] = 0.04e9;
    wood.g[2] = 0.6e9;
    double timber[6][6];
    CHECK_MESSAGE(wood.ConstitutiveMatrix(20.0, timber, &error), error);
    CHECK(timber[0][0] > timber[1][1] * 5.0);
    // Symmetric, as a compliance-derived stiffness must be.
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) {
            CHECK(std::fabs(timber[i][j] - timber[j][i]) <= std::fabs(timber[i][i]) * 1e-12);
        }
    }
    // AND AN IMPOSSIBLE SET IS REFUSED. A Poisson's ratio far too large
    // for the ratio of moduli it sits between describes a material that
    // stores negative energy, which is not a material.
    StudyMaterial impossible = orthotropic;
    impossible.e[0] = 1e9;
    impossible.e[1] = 100e9;
    impossible.nu[0] = 0.9;
    impossible.nu[1] = 0.9;
    impossible.nu[2] = 0.9;
    double nonsense[6][6];
    CHECK(!impossible.ConstitutiveMatrix(20.0, nonsense, &error));
    std::printf("  an unstable set is refused: %s\n", error.c_str());

    // The isotropic view refuses an orthotropic material rather than
    // averaging it.
    fem::Material reduced;
    CHECK(!wood.AsIsotropic(20.0, &reduced, &error));
    CHECK(isotropic.AsIsotropic(20.0, &reduced, &error));
    CHECK(std::fabs(reduced.youngs_modulus - 210e9) < 1.0);
}

// --- Consistent loads -----------------------------------------------------

void TestConsistentLoads() {
    std::printf("consistent loads:\n");
    const Block block = MakeBlock(Vec3d{10, 6, 4});
    CHECK(block.top_id != cad::kNoEntity);
    VolumeMesh mesh;
    if (!MeshBlock(block.model, block.body, 2.0, &mesh)) {
        ++failures;
        return;
    }

    // A UNIFORM PRESSURE'S NODAL FORCES SUM TO PRESSURE TIMES AREA, along
    // the inward normal, and that is not a tautology: they come from
    // integrating the shape functions over each triangle, so the sum
    // being right says the area element and the shape functions are both
    // right. It also checks the winding -- a boundary wound inwards would
    // give the correct magnitude and the wrong sign.
    const double pressure = 1e6;
    std::vector<fem::NodalLoad> applied;
    double area = 0.0;
    std::string error;
    CHECK_MESSAGE(fem::FaceTraction(mesh, block.top_id, Vec3d{}, pressure, &applied, &area, &error),
                  error);
    Vec3d total{};
    for (const fem::NodalLoad &load : applied) total = total + load.force;
    std::printf("  top face area %.4f (exactly %.4f), pressure %.3g gives (%.4g, %.4g, %.4g)\n",
                area, 10.0 * 6.0, pressure, total.x, total.y, total.z);
    CHECK(std::fabs(area - 60.0) < 1e-9);
    CHECK(std::fabs(total.x) < pressure * area * 1e-12);
    CHECK(std::fabs(total.y) < pressure * area * 1e-12);
    CHECK(std::fabs(total.z + pressure * 60.0) < pressure * 60.0 * 1e-12);

    // THE MOMENT IS RIGHT TOO, which the total cannot tell us. A uniform
    // pressure over a rectangle acts through its centre; nodal forces
    // that summed correctly but were distributed wrongly would fail this
    // and pass the check above.
    Vec3d moment{};
    for (const fem::NodalLoad &load : applied) {
        moment = moment + mesh.nodes[Idx(load.node)].Cross(load.force);
    }
    const Vec3d centre{5.0, 3.0, 4.0};
    const Vec3d wanted = centre.Cross(Vec3d{0, 0, -pressure * 60.0});
    std::printf("  moment about the origin (%.4g, %.4g, %.4g), should be (%.4g, %.4g, %.4g)\n",
                moment.x, moment.y, moment.z, wanted.x, wanted.y, wanted.z);
    CHECK((moment - wanted).Length() < wanted.Length() * 1e-9);

    // A traction is the same integral with a vector instead of a normal.
    CHECK_MESSAGE(
        fem::FaceTraction(mesh, block.top_id, Vec3d{1e5, 0, 0}, 0.0, &applied, &area, &error),
        error);
    total = Vec3d{};
    for (const fem::NodalLoad &load : applied) total = total + load.force;
    CHECK(std::fabs(total.x - 1e5 * 60.0) < 1e5 * 60.0 * 1e-12);
    CHECK(std::fabs(total.z) < 1e5 * 60.0 * 1e-12);
}

// --- Gravity --------------------------------------------------------------

void TestGravity() {
    std::printf("gravity:\n");
    const Block block = MakeBlock(Vec3d{10, 6, 4});
    VolumeMesh mesh;
    if (!MeshBlock(block.model, block.body, 2.0, &mesh)) {
        ++failures;
        return;
    }
    Study study = PressureStudy(block, 0.0);
    study.loads.clear();
    Load weight;
    weight.label = "gravity";
    weight.kind = LoadKind::Gravity;
    weight.vector = Vec3d{0, 0, -9.81};
    study.loads.push_back(weight);

    AnalysisModel bound;
    BindReport report;
    CHECK_MESSAGE(fem::BindStudy(block.model, mesh, study, {}, &bound, &report), report.error);
    const double mass = 7850.0 * 10.0 * 6.0 * 4.0;
    const Vec3d total = bound.TotalLoad();
    std::printf("  weight %.6g N, should be %.6g N\n", -total.z, mass * 9.81);
    // INTEGRATED OVER THE ELEMENTS, not the total split between nodes,
    // which for a graded mesh is a different distribution. The sum coming
    // out right says the element volumes add up, which is the mesher's
    // claim checked again from a different direction.
    CHECK(std::fabs(total.z + mass * 9.81) < mass * 9.81 * 1e-9);
    CHECK(std::fabs(total.x) < mass * 9.81 * 1e-12);
}

// --- The point of the whole exercise --------------------------------------

void TestSurvivesARebuild() {
    std::printf("the same study on a changed model:\n");
    const double pressure = 2e6;

    struct Outcome {
        bool ok = false;
        int nodes = 0;
        int elements = 0;
        int held = 0;
        double area = 0.0;
        Vec3d force;
        cad::EntityId top = cad::kNoEntity;
    };

    // The study is captured once, from the first block, and never
    // touched again.
    const Block first = MakeBlock(Vec3d{10, 6, 4});
    CHECK(first.top_id != cad::kNoEntity);
    CHECK(first.bottom_id != cad::kNoEntity);
    const Study study = PressureStudy(first, pressure);

    auto bind_to = [&](const Vec3d &size, double target) {
        Outcome outcome;
        const Block block = MakeBlock(size);
        VolumeMesh mesh;
        if (!MeshBlock(block.model, block.body, target, &mesh)) return outcome;
        AnalysisModel bound;
        BindReport report;
        if (!fem::BindStudy(block.model, mesh, study, {}, &bound, &report)) {
            std::printf("    binding failed: %s\n", report.error.c_str());
            return outcome;
        }
        outcome.ok = true;
        outcome.nodes = bound.NodeCount();
        outcome.elements = bound.ElementCount();
        outcome.held = static_cast<int>(bound.constraints.size());
        outcome.force = bound.TotalLoad();
        outcome.top = block.top_id;
        for (const fem::BoundCondition &condition : report.conditions) {
            if (condition.area > 0.0) outcome.area = condition.area;
            CHECK(condition.status == cad::ResolveStatus::Resolved);
            // The study named the top face; binding must have found the
            // top face of *this* model, not merely some face.
            if (condition.label == "pressure on top") CHECK(condition.entity == block.top_id);
        }
        return outcome;
    };

    // The same study, three different blocks, three different meshes.
    const Outcome same = bind_to(Vec3d{10, 6, 4}, 2.0);
    const Outcome taller = bind_to(Vec3d{10, 6, 12}, 2.0);
    const Outcome wider = bind_to(Vec3d{25, 6, 4}, 2.0);
    const Outcome finer = bind_to(Vec3d{10, 6, 4}, 1.1);

    for (const auto &entry : {std::pair<const char *, const Outcome *>{"10 x 6 x 4", &same},
                              {"10 x 6 x 12, taller", &taller},
                              {"25 x 6 x 4, wider", &wider},
                              {"10 x 6 x 4, finer mesh", &finer}}) {
        const Outcome &o = *entry.second;
        CHECK(o.ok);
        if (!o.ok) continue;
        std::printf("  %-24s %5d nodes %5d elements, %3d held, top area %7.3f, load %.6g N\n",
                    entry.first, o.nodes, o.elements, o.held, o.area, -o.force.z);
    }
    if (!(same.ok && taller.ok && wider.ok && finer.ok)) return;

    // THE STUDY STILL MEANS WHAT IT MEANT. Every block's top face got the
    // pressure and every block's bottom face was held, with no id from
    // the first model surviving anywhere: the meshes have different node
    // counts, the faces have different ids, and the loaded area follows
    // the geometry.
    CHECK(std::fabs(same.area - 60.0) < 1e-9);
    CHECK(std::fabs(taller.area - 60.0) < 1e-9);
    CHECK(std::fabs(wider.area - 150.0) < 1e-9);
    CHECK(std::fabs(same.force.z + pressure * 60.0) < pressure * 60.0 * 1e-9);
    CHECK(std::fabs(wider.force.z + pressure * 150.0) < pressure * 150.0 * 1e-9);

    // A taller block has the same top, so the same load, on a different
    // mesh -- which is the case a node-number study gets silently wrong,
    // because node 37 of the taller mesh is somewhere else entirely.
    CHECK(std::fabs(taller.force.z - same.force.z) < std::fabs(same.force.z) * 1e-9);
    CHECK(taller.nodes != same.nodes);

    // And refining the mesh changes the node count and nothing else.
    CHECK(finer.nodes > same.nodes * 2);
    CHECK(std::fabs(finer.area - same.area) < 1e-9);
    CHECK(std::fabs(finer.force.z - same.force.z) < std::fabs(same.force.z) * 1e-9);
    CHECK(finer.held > same.held);
}

// --- What happens when a name cannot be honoured --------------------------

void TestUnresolved() {
    std::printf("a name that cannot be honoured:\n");
    const Block block = MakeBlock(Vec3d{10, 6, 4});
    VolumeMesh mesh;
    if (!MeshBlock(block.model, block.body, 2.0, &mesh)) {
        ++failures;
        return;
    }
    Study study = PressureStudy(block, 1e6);
    // A name for geometry that was never in this model: a cylindrical
    // face, where the block has none.
    study.loads[0].where.name.surface_kind = cad::SurfaceKind::Cylinder;
    study.loads[0].where.name.role = "a face that is not here";
    study.loads[0].where.name.neighbour_features = {99, 99, 99, 99};
    study.loads[0].where.name.sample_point = Vec3d{1e4, -1e4, 1e4};
    study.loads[0].where.name.sample_direction = Vec3d{0.577, 0.577, 0.577};
    study.loads[0].where.name.loop_count = 7;
    study.loads[0].where.name.edge_count = 33;

    AnalysisModel bound;
    BindReport report;
    // REFUSED, NOT GUESSED AT. A load moved quietly onto the wrong face
    // is worse than a study that will not run: the answer looks
    // plausible, and nothing about it says which face it was computed on.
    CHECK(!fem::BindStudy(block.model, mesh, study, {}, &bound, &report));
    CHECK(report.lost + report.ambiguous == 1);
    CHECK(report.error.find("cannot be found") != std::string::npos);
    std::printf("  %s\n", report.error.c_str());

    // And a caller who would rather see the rest can say so -- an
    // interactive one, showing what is still attached while the user
    // fixes what is not.
    BindOptions lenient;
    lenient.allow_unresolved = true;
    CHECK_MESSAGE(fem::BindStudy(block.model, mesh, study, lenient, &bound, &report), report.error);
    CHECK(!report.warnings.empty());
    CHECK(bound.loads.empty());
    CHECK(!bound.constraints.empty());

    // A study with nothing holding it still is refused whatever the
    // loads say: it has no unique solution, and a solver handed one
    // returns whichever rigid motion the factorisation happened to pick.
    Study loose = PressureStudy(block, 1e6);
    loose.supports.clear();
    CHECK(!fem::BindStudy(block.model, mesh, loose, {}, &bound, &report));
    CHECK(report.error.find("holds the model still") != std::string::npos);
}

}  // namespace

int main() {
    TestMaterials();
    TestConsistentLoads();
    TestGravity();
    TestSurvivesARebuild();
    TestUnresolved();
    if (failures != 0) {
        std::printf("fem_study_test FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("fem_study_test passed (%d checks)\n", checks);
    return 0;
}
