// Part I.3: thermal-structural coupling, sequential.

#include "fem_map.h"
#include "fem_static.h"
#include "fem_thermal.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char *what, int line) {
    ++checks;
    if (ok) return;
    std::fflush(stdout);
    std::printf("CHECK FAILED: %s at %s:%d\n", what, __FILE__, line);
    ++failures;
}

#define CHECK(x) Check((x), #x, __LINE__)
#define CHECK_MESSAGE(x, message) Check((x), (std::string(#x) + ": " + (message)).c_str(), __LINE__)

using cad::Vec3d;
using fem::AnalysisModel;
using fem::BoundElement;
using fem::ElementShape;
using fem::StaticResult;
using fem::StudyMaterial;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// COORDINATES ARE COMPARED WITH A TOLERANCE, NEVER WITH ==. The nodes are
// built as `size * i / n`, and `0.3 * 4 / 4` is not bit-identical to
// `0.3`; so `p.x == size.x` was false at the far face, the constraints
// meant to hold it were never applied, and a block meant to be fully
// restrained expanded almost freely. The stress came out at 38% of the
// closed form and looked like a factor somebody had forgotten.
bool Near(double a, double b, double scale) { return std::fabs(a - b) <= scale * 1e-9; }

// A block of hexahedra over [0,size], nx by ny by nz.
AnalysisModel Block(const Vec3d &size, int nx, int ny, int nz, double expansion) {
    AnalysisModel model;
    StudyMaterial steel;
    steel.youngs_modulus = fem::MaterialCurve::Constant(210e9);
    steel.poissons_ratio = fem::MaterialCurve::Constant(0.3);
    steel.thermal_expansion = fem::MaterialCurve::Constant(expansion);
    model.materials.push_back(steel);
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                model.nodes.push_back(Vec3d{size.x * i / nx, size.y * j / ny, size.z * k / nz});
            }
        }
    }
    auto at = [&](int i, int j, int k) { return (k * (ny + 1) + j) * (nx + 1) + i; };
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                BoundElement element;
                element.shape = ElementShape::Hex8;
                element.nodes = {at(i, j, k),         at(i + 1, j, k),
                                 at(i + 1, j + 1, k), at(i, j + 1, k),
                                 at(i, j, k + 1),     at(i + 1, j, k + 1),
                                 at(i + 1, j + 1, k + 1), at(i, j + 1, k + 1)};
                model.elements.push_back(element);
            }
        }
    }
    return model;
}

// The same block with its edges' mid-nodes added, which turns every Hex8
// into a Hex20 -- and is what lets the linear-gradient case be exact
// rather than merely convergent.
void MakeQuadratic(AnalysisModel *model) {
    static const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {0, 4}, {1, 5},
                                     {2, 6}, {3, 7}, {4, 5}, {5, 6}, {6, 7}, {7, 4}};
    std::map<std::pair<int, int>, int> middles;
    for (BoundElement &element : model->elements) {
        element.shape = ElementShape::Hex20;
        const std::vector<int> corners = element.nodes;
        for (const auto &edge : edges) {
            const int u = corners[Idx(edge[0])];
            const int v = corners[Idx(edge[1])];
            const std::pair<int, int> key = u < v ? std::make_pair(u, v) : std::make_pair(v, u);
            const auto found = middles.find(key);
            if (found != middles.end()) {
                element.nodes.push_back(found->second);
                continue;
            }
            const int index = model->NodeCount();
            model->nodes.push_back((model->nodes[Idx(u)] + model->nodes[Idx(v)]) * 0.5);
            middles[key] = index;
            element.nodes.push_back(index);
        }
    }
}

// THREE SYMMETRY PLANES, NOT A THREE-POINT PIN. The body has to be left
// free to expand, so the restraint may only remove the six rigid motions
// -- and the textbook way of doing that, pinning one corner in three
// directions, a second in two and a third in one, is badly conditioned.
// It leaves modes that are *nearly* rigid: a rotation combined with just
// enough shear to keep the pinned node still, which strains the body very
// little and so has a tiny pivot. The factorization refused the matrix and
// blamed an unrestrained rigid-body mode, which is the usual cause and was
// not this one.
//
// Holding each of three faces against motion normal to itself removes the
// same six motions using whole faces, which is well conditioned, and costs
// nothing in accuracy here because the exact solution satisfies it: a
// block expanding about its own corner has no normal displacement on the
// three faces through that corner.
void PinRigidMotions(AnalysisModel *model, const Vec3d &size) {
    (void)size;
    for (int node = 0; node < model->NodeCount(); ++node) {
        const Vec3d &p = model->nodes[Idx(node)];
        fem::Constraint constraint;
        constraint.node = node;
        bool any = false;
        if (Near(p.x, 0.0, 1.0)) { constraint.fixed[0] = true; any = true; }
        if (Near(p.y, 0.0, 1.0)) { constraint.fixed[1] = true; any = true; }
        if (Near(p.z, 0.0, 1.0)) { constraint.fixed[2] = true; any = true; }
        if (any) model->constraints.push_back(constraint);
    }
}

// --- Free expansion: the sharpest test there is -----------------------------

void TestFreeExpansion() {
    std::printf("a free block heated uniformly:\n");
    const Vec3d size{0.3, 0.2, 0.1};
    const double expansion = 1.2e-5;
    const double rise = 150.0;
    AnalysisModel model = Block(size, 4, 3, 2, expansion);
    model.reference_temperature = 20.0;
    model.node_temperature.assign(Idx(model.NodeCount()), 20.0 + rise);
    PinRigidMotions(&model, size);
    std::vector<fem::NodalLoad> thermal;
    std::string error;
    CHECK_MESSAGE(fem::ThermalLoad(model, &thermal, &error), error);
    model.loads = thermal;

    StaticResult result;
    CHECK_MESSAGE(fem::SolveStatic(model, {}, {}, &result), result.error);
    if (!result.ok) return;

    // It expanded by exactly alpha * dT in every direction. The far corner
    // moves by that times its distance from the pinned one.
    const Vec3d wanted{size.x * expansion * rise, size.y * expansion * rise,
                       size.z * expansion * rise};
    Vec3d found{};
    for (int node = 0; node < model.NodeCount(); ++node) {
        const Vec3d &p = model.nodes[Idx(node)];
        if (!Near(p.x, size.x, size.x) || !Near(p.y, size.y, size.y) ||
            !Near(p.z, size.z, size.z)) {
            continue;
        }
        found = result.displacement[Idx(node)];
    }
    std::printf("  far corner moved (%.6e, %.6e, %.6e), should be (%.6e, %.6e, %.6e)\n", found.x,
                found.y, found.z, wanted.x, wanted.y, wanted.z);
    CHECK((found - wanted).Length() < wanted.Length() * 1e-9);

    // AND THE STRESS IS ZERO. This is the whole test: a uniform
    // temperature on an unconstrained body strains it and stresses it not
    // at all, so the thermal load and the stiffness must cancel *exactly*.
    // An error anywhere -- in the load's integration, in the constitutive
    // matrix, or in the stress recovery forgetting to take the thermal
    // strain out -- shows up here as a large stress in the right places,
    // which is the most convincing kind of wrong answer.
    double worst = 0.0;
    for (const fem::StressTensor &s : result.element_stress) {
        worst = std::max(worst, std::fabs(s.VonMises()));
    }
    // Against the stress a fully restrained block of the same rise would
    // see, which is the scale of what could have gone wrong.
    const double restrained = 210e9 * expansion * rise / (1.0 - 2.0 * 0.3);
    std::printf("  worst von Mises %.4e Pa, against %.4e if it had been restrained\n", worst,
                restrained);
    CHECK(worst < restrained * 1e-10);
}

// --- A linear gradient is also stress-free ---------------------------------

void TestLinearGradient() {
    std::printf("a free block with a linear temperature gradient:\n");
    // A LINEAR TEMPERATURE FIELD IS STRESS-FREE IN A FREE BODY, and a
    // trilinear element cannot show it. The strain a linear field wants is
    // compatible -- it satisfies Saint-Venant -- so there is a displacement
    // field that produces it and no stress, and that field is
    // *quadratic*: integrating a strain linear in x gives a displacement
    // with an x-squared term in it. Hex8 has no such term, so it develops
    // stress: nine percent of the fully restrained value on the mesh
    // below, which is discretisation error and not a defect.
    //
    // The test is therefore that the stress falls with refinement for Hex8
    // and is *exactly* zero for a quadratic element, which is a far
    // stronger pair of statements than either alone -- the first says the
    // error converges, the second says what it converges to.
    const Vec3d size{0.3, 0.2, 0.1};
    const double expansion = 1.2e-5;
    const double restrained = 210e9 * expansion * 400.0 / (1.0 - 2.0 * 0.3);
    auto run = [&](int n, bool quadratic) {
        AnalysisModel model = Block(size, n, n, n, expansion);
        if (quadratic) MakeQuadratic(&model);
        model.reference_temperature = 20.0;
        model.node_temperature.assign(Idx(model.NodeCount()), 0.0);
        for (int node = 0; node < model.NodeCount(); ++node) {
            model.node_temperature[Idx(node)] = 20.0 + 400.0 * model.nodes[Idx(node)].x / size.x;
        }
        // A RESTRAINT THE EXACT SOLUTION SATISFIES, which the three
        // symmetry planes are not here. Working the stress-free field out
        // properly gives
        //     u_x = G(x) - g'(y^2 + z^2)/2,  u_y = g(x) y,  u_z = g(x) z
        // where g is the thermal strain and G its integral: quadratic in
        // *every* direction, not just along the gradient. So u_x is not
        // zero on the x = 0 face -- only at the one point where y and z
        // both vanish -- and holding the whole face fixes a body that
        // wants to dish, which produces stress of its own and got worse
        // the finer the mesh resolved the dishing. u_y on y = 0 and u_z on
        // z = 0 are satisfied exactly, so those stay.
        for (int node = 0; node < model.NodeCount(); ++node) {
            const Vec3d &p = model.nodes[Idx(node)];
            fem::Constraint constraint;
            constraint.node = node;
            bool any = false;
            if (Near(p.y, 0.0, 1.0)) { constraint.fixed[1] = true; any = true; }
            if (Near(p.z, 0.0, 1.0)) { constraint.fixed[2] = true; any = true; }
            if (Near(p.x, 0.0, 1.0) && Near(p.y, 0.0, 1.0) && Near(p.z, 0.0, 1.0)) {
                constraint.fixed[0] = true;
                any = true;
            }
            if (any) model.constraints.push_back(constraint);
        }
        std::vector<fem::NodalLoad> thermal;
        std::string error;
        if (!fem::ThermalLoad(model, &thermal, &error)) {
            std::printf("    %s\n", error.c_str());
            ++failures;
            return 0.0;
        }
        model.loads = thermal;
        StaticResult result;
        if (!fem::SolveStatic(model, {}, {}, &result)) {
            std::printf("    %s\n", result.error.c_str());
            ++failures;
            return 0.0;
        }
        double worst = 0.0;
        for (const fem::StressTensor &s : result.element_stress) {
            worst = std::max(worst, std::fabs(s.VonMises()));
        }
        return worst;
    };
    const double coarse = run(2, false);
    const double finer = run(4, false);
    const double finest = run(8, false);
    const double curved = run(2, true);
    std::printf("  Hex8:  %.4e, %.4e, %.4e Pa as the mesh halves twice\n", coarse, finer, finest);
    std::printf("  Hex20: %.4e Pa on the coarsest mesh of all\n", curved);
    std::printf("  (a fully restrained block of the same gradient would see %.4e)\n", restrained);
    CHECK(finer < coarse * 0.7);
    CHECK(finest < finer * 0.7);
    // AND A QUADRATIC ELEMENT GETS IT EXACTLY, on two elements a side,
    // because the stress-free displacement field is in its space.
    CHECK(curved < restrained * 1e-9);
}

// --- Fully restrained: a closed form ---------------------------------------

void TestRestrained() {
    std::printf("a fully restrained block heated uniformly:\n");
    // Every face held, so no strain at all: the stress is whatever it takes
    // to prevent the expansion, which for an isotropic material is
    // hydrostatic at -E alpha dT / (1 - 2v).
    const Vec3d size{0.3, 0.2, 0.1};
    const double expansion = 1.2e-5;
    const double rise = 150.0;
    const double youngs = 210e9;
    const double poisson = 0.3;
    AnalysisModel model = Block(size, 4, 3, 2, expansion);
    model.reference_temperature = 20.0;
    model.node_temperature.assign(Idx(model.NodeCount()), 20.0 + rise);
    for (int node = 0; node < model.NodeCount(); ++node) {
        const Vec3d &p = model.nodes[Idx(node)];
        fem::Constraint constraint;
        constraint.node = node;
        bool any = false;
        if (Near(p.x, 0.0, 1.0) || Near(p.x, size.x, size.x)) {
            constraint.fixed[0] = true;
            any = true;
        }
        if (Near(p.y, 0.0, 1.0) || Near(p.y, size.y, size.y)) {
            constraint.fixed[1] = true;
            any = true;
        }
        if (Near(p.z, 0.0, 1.0) || Near(p.z, size.z, size.z)) {
            constraint.fixed[2] = true;
            any = true;
        }
        if (any) model.constraints.push_back(constraint);
    }
    std::vector<fem::NodalLoad> thermal;
    std::string error;
    CHECK_MESSAGE(fem::ThermalLoad(model, &thermal, &error), error);
    model.loads = thermal;
    StaticResult result;
    CHECK_MESSAGE(fem::SolveStatic(model, {}, {}, &result), result.error);
    if (!result.ok) return;

    const double exact = -youngs * expansion * rise / (1.0 - 2.0 * poisson);
    double mean = 0.0;
    for (const fem::StressTensor &s : result.element_stress) {
        mean += (s.s[0] + s.s[1] + s.s[2]) / 3.0;
    }
    mean /= static_cast<double>(result.element_stress.size());
    std::printf("  hydrostatic stress %.6e Pa, exactly %.6e; largest displacement %.3e m\n", mean,
                exact, result.max_displacement);
    CHECK(std::fabs(mean - exact) < std::fabs(exact) * 1e-9);
}

// --- The mapping ------------------------------------------------------------

void TestMapping() {
    std::printf("mapping a field between two different meshes:\n");
    const Vec3d size{0.3, 0.2, 0.1};
    const AnalysisModel coarse = Block(size, 3, 2, 2, 1.2e-5);
    const AnalysisModel fine = Block(size, 7, 5, 4, 1.2e-5);

    // A LINEAR FIELD MUST MAP EXACTLY, for the same reason an element must
    // represent one exactly: adding ten degrees everywhere has to arrive as
    // ten degrees everywhere, and a gradient has to arrive as the same
    // gradient. An interpolation that loses either turns a thermal-stress
    // answer into a plausible one that is wrong by what it lost.
    auto linear = [&](const Vec3d &p) { return 20.0 + 3.0 * p.x - 2.0 * p.y + 5.0 * p.z; };
    std::vector<double> source(Idx(coarse.NodeCount()), 0.0);
    for (int node = 0; node < coarse.NodeCount(); ++node) {
        source[Idx(node)] = linear(coarse.nodes[Idx(node)]);
    }
    std::vector<double> mapped;
    fem::MapReport report;
    CHECK_MESSAGE(fem::MapField(coarse.nodes, coarse.elements, source, fine.nodes, &mapped, &report),
                  report.error);
    double worst = 0.0;
    for (int node = 0; node < fine.NodeCount(); ++node) {
        worst = std::max(worst, std::fabs(mapped[Idx(node)] - linear(fine.nodes[Idx(node)])));
    }
    std::printf("  %d of %d target nodes found inside a source element, %d extrapolated "
                "(worst %.2e m outside)\n",
                report.inside, fine.NodeCount(), report.extrapolated, report.worst_distance);
    std::printf("  a linear field maps to within %.2e K\n", worst);
    CHECK(worst < 1e-11);

    // A curved field maps to the accuracy of the coarse mesh, which is the
    // honest expectation: the mapping cannot recover what the source mesh
    // never resolved.
    auto curved = [&](const Vec3d &p) {
        return 20.0 + 300.0 * std::sin(3.14159265358979 * p.x / size.x);
    };
    for (int node = 0; node < coarse.NodeCount(); ++node) {
        source[Idx(node)] = curved(coarse.nodes[Idx(node)]);
    }
    CHECK_MESSAGE(fem::MapField(coarse.nodes, coarse.elements, source, fine.nodes, &mapped, &report),
                  report.error);
    worst = 0.0;
    for (int node = 0; node < fine.NodeCount(); ++node) {
        worst = std::max(worst, std::fabs(mapped[Idx(node)] - curved(fine.nodes[Idx(node)])));
    }
    std::printf("  a sine over three elements maps to within %.4f K of 300\n", worst);
    CHECK(worst < 40.0);

    // The inverse map on its own, on a deliberately distorted element: the
    // piece everything else here is bookkeeping around.
    std::vector<Vec3d> distorted;
    fem::ReferenceNodes(ElementShape::Hex8, &distorted);
    for (std::size_t i = 0; i < distorted.size(); ++i) {
        const std::uint64_t h = (i + 1) * 0x9E3779B97F4A7C15ull;
        auto unit = [&](int shift) {
            return static_cast<double>((h >> shift) & 0xFFFF) / 65535.0 - 0.5;
        };
        distorted[i] = distorted[i] + Vec3d{unit(0), unit(16), unit(32)} * 0.3;
    }
    std::vector<double> shape;
    std::vector<double> dn;
    double worst_reference = 0.0;
    for (const Vec3d &wanted : {Vec3d{0.2, -0.4, 0.6}, Vec3d{-0.9, 0.1, -0.3}, Vec3d{0, 0, 0}}) {
        fem::ShapeFunctions(ElementShape::Hex8, wanted, &shape, &dn);
        Vec3d physical{};
        for (std::size_t a = 0; a < distorted.size(); ++a) {
            physical = physical + distorted[a] * shape[a];
        }
        Vec3d recovered{};
        double distance = 0.0;
        CHECK(fem::InverseMap(ElementShape::Hex8, distorted, physical, &recovered, &distance));
        worst_reference = std::max(worst_reference, (recovered - wanted).Length());
    }
    std::printf("  the inverse map recovers a reference coordinate to %.2e\n", worst_reference);
    CHECK(worst_reference < 1e-10);
}

// --- End to end -------------------------------------------------------------

void TestEndToEnd() {
    std::printf("a thermal solve mapped onto a different structural mesh:\n");
    const Vec3d size{0.3, 0.05, 0.05};
    const double expansion = 1.2e-5;
    const double hot = 500.0;
    const double cold = 300.0;

    // The thermal mesh: fine along the length, where the gradient is.
    fem::ThermalModel thermal;
    fem::ThermalMaterial conductor;
    conductor.conductivity = fem::MaterialCurve::Constant(45.0);
    thermal.materials.push_back(conductor);
    {
        const AnalysisModel scaffold = Block(size, 12, 1, 1, expansion);
        thermal.nodes = scaffold.nodes;
        thermal.elements = scaffold.elements;
        for (int node = 0; node < static_cast<int>(thermal.nodes.size()); ++node) {
            const double x = thermal.nodes[Idx(node)].x;
            if (Near(x, 0.0, 1.0)) thermal.fixed.push_back({node, hot});
            if (Near(x, size.x, size.x)) thermal.fixed.push_back({node, cold});
        }
    }
    fem::ThermalResult heat;
    CHECK_MESSAGE(fem::SolveSteadyHeat(thermal, {}, &heat), heat.error);
    if (!heat.ok) return;

    // The structural mesh: different, and deliberately so -- coarser along
    // the length and finer across it, which is what a stress mesh wants and
    // a thermal one does not.
    AnalysisModel structure = Block(size, 5, 3, 3, expansion);
    structure.reference_temperature = cold;
    fem::MapReport report;
    CHECK_MESSAGE(fem::MapField(thermal.nodes, thermal.elements, heat.temperature, structure.nodes,
                                &structure.node_temperature, &report),
                  report.error);
    std::printf("  thermal mesh %d nodes, structural %d; %d mapped inside, %d extrapolated\n",
                static_cast<int>(thermal.nodes.size()), structure.NodeCount(), report.inside,
                report.extrapolated);

    // The temperature field is linear in x, so the mapping is exact and the
    // free body should still be stress free -- which makes this an
    // end-to-end test with a known answer rather than a plausibility check.
    double worst_temperature = 0.0;
    for (int node = 0; node < structure.NodeCount(); ++node) {
        const double exact = hot + (cold - hot) * structure.nodes[Idx(node)].x / size.x;
        worst_temperature =
            std::max(worst_temperature, std::fabs(structure.node_temperature[Idx(node)] - exact));
    }
    std::printf("  mapped temperature is within %.2e K of the analytic gradient\n",
                worst_temperature);
    CHECK(worst_temperature < 1e-9);

    PinRigidMotions(&structure, size);
    std::vector<fem::NodalLoad> load;
    std::string error;
    CHECK_MESSAGE(fem::ThermalLoad(structure, &load, &error), error);
    structure.loads = load;
    StaticResult result;
    CHECK_MESSAGE(fem::SolveStatic(structure, {}, {}, &result), result.error);
    if (!result.ok) return;
    double worst = 0.0;
    for (const fem::StressTensor &s : result.element_stress) {
        worst = std::max(worst, std::fabs(s.VonMises()));
    }
    const double restrained = 210e9 * expansion * (hot - cold) / (1.0 - 2.0 * 0.3);
    std::printf("  worst von Mises %.4e Pa, against %.4e if it had been restrained\n", worst,
                restrained);
    // A FEW PERCENT, NOT ZERO, AND THE REASON IS THE ELEMENT. The mapped
    // field is linear in x, which is stress-free in a free body -- but the
    // displacement that produces it is quadratic, and Hex8 has no
    // quadratic term. So the stress here is discretisation error, bounded
    // rather than eliminated, and the linear-gradient case above shows it
    // converging away and vanishing outright for Hex20.
    CHECK(worst < restrained * 0.05);

    // And restrained along its length it develops the stress the gradient
    // is worth -- the same model, so the difference is the boundary
    // condition and nothing else.
    for (int node = 0; node < structure.NodeCount(); ++node) {
        const double x = structure.nodes[Idx(node)].x;
        if (!Near(x, 0.0, 1.0) && !Near(x, size.x, size.x)) continue;
        fem::Constraint constraint;
        constraint.node = node;
        constraint.fixed[0] = true;
        structure.constraints.push_back(constraint);
    }
    StaticResult held;
    CHECK_MESSAGE(fem::SolveStatic(structure, {}, {}, &held), held.error);
    if (!held.ok) return;
    double mean = 0.0;
    for (const fem::StressTensor &s : held.element_stress) mean += s.s[0];
    mean /= static_cast<double>(held.element_stress.size());
    // Held in x only, so the axial stress is -E alpha dT averaged over the
    // gradient, which for a linear field is the mean rise.
    const double exact = -210e9 * expansion * 0.5 * (hot - cold);
    std::printf("  held along its length: mean axial stress %.4e Pa, beam theory gives %.4e\n", mean,
                exact);
    CHECK(std::fabs(mean - exact) < std::fabs(exact) * 0.02);
}

}  // namespace

int main() {
    TestFreeExpansion();
    TestLinearGradient();
    TestRestrained();
    TestMapping();
    TestEndToEnd();
    if (failures != 0) {
        std::printf("fem_couple_test FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("fem_couple_test passed (%d checks)\n", checks);
    return 0;
}
