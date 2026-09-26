// Part I.4: modal analysis.

#include "fem_modal.h"

#include <algorithm>
#include <map>
#include <utility>
#include <cmath>
#include <cstdio>
#include <string>
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
using fem::ModalOptions;
using fem::ModalResult;
using fem::StudyMaterial;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }


constexpr double kPi = 3.14159265358979323846;

bool Near(double a, double b, double scale) { return std::fabs(a - b) <= scale * 1e-9; }

AnalysisModel Bar(const Vec3d &size, int nx, int ny, int nz, double youngs, double poisson,
                  double density) {
    AnalysisModel model;
    StudyMaterial material;
    material.youngs_modulus = fem::MaterialCurve::Constant(youngs);
    material.poissons_ratio = fem::MaterialCurve::Constant(poisson);
    material.density = density;
    model.materials.push_back(material);
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

// --- A free body has exactly six zero frequencies --------------------------

void TestRigidBodyModes() {
    std::printf("an unrestrained block:\n");
    const AnalysisModel model = Bar(Vec3d{0.3, 0.1, 0.08}, 4, 2, 2, 210e9, 0.3, 7850.0);
    ModalOptions options;
    options.modes = 9;
    ModalResult result;
    CHECK_MESSAGE(fem::SolveModal(model, {}, options, &result), result.error);
    if (!result.ok) return;
    std::printf("  first nine frequencies (Hz):");
    for (const double f : result.frequency) std::printf(" %.3f", f);
    std::printf("\n");
    // SIX ZERO MODES AND NO MORE. Three translations and three rotations
    // carry no strain, so they carry no energy and no frequency; a seventh
    // would be a mechanism the mesh invented, and five would mean one was
    // restrained by something that should not have restrained it. This is
    // the element rank check of Part H.1 restated for the whole assembly,
    // and it is the cheapest verification a modal solver has.
    std::printf("  %d of them are rigid-body modes\n", result.rigid_body_modes);
    CHECK(result.rigid_body_modes == 6);
    // And the first flexible mode is a real frequency, well clear of them.
    CHECK(result.frequency[6] > 1.0);
    CHECK(result.frequency[6] < result.frequency[7] + 1e-9);
}

// --- Longitudinal modes of a bar, against the closed form -------------------

void TestLongitudinalBar() {
    std::printf("a fixed-free bar in longitudinal vibration:\n");
    // The wave speed is sqrt(E/rho) and the fixed-free modes are at
    // (2n-1)c/(4L). A slender bar with Poisson's ratio zero has no lateral
    // coupling at all, so the closed form is exact for the continuum and
    // the only error left is the discretisation -- which is what makes
    // this worth measuring rather than merely comparing.
    const double length = 2.0;
    const double youngs = 210e9;
    const double density = 7850.0;
    const double speed = std::sqrt(youngs / density);
    for (const int n : {8, 16, 32}) {
        AnalysisModel model = Bar(Vec3d{length, 0.05, 0.05}, n, 1, 1, youngs, 0.0, density);
        for (int node = 0; node < model.NodeCount(); ++node) {
            const Vec3d &p = model.nodes[Idx(node)];
            fem::Constraint constraint;
            constraint.node = node;
            bool any = false;
            if (Near(p.x, 0.0, 1.0)) { constraint.fixed[0] = true; any = true; }
            // Held against lateral motion everywhere, so that the modes
            // found are the longitudinal ones and not the far softer
            // bending ones -- which are a real part of the structure and
            // not what this case is about.
            constraint.fixed[1] = true;
            constraint.fixed[2] = true;
            any = true;
            if (any) model.constraints.push_back(constraint);
        }
        ModalOptions options;
        options.modes = 3;
        ModalResult result;
        CHECK_MESSAGE(fem::SolveModal(model, {}, options, &result), result.error);
        if (!result.ok) return;
        std::printf("  %2d elements:", n);
        for (int mode = 0; mode < 3; ++mode) {
            const double exact = (2.0 * (mode + 1) - 1.0) * speed / (4.0 * length);
            std::printf("  %.2f (%.2f, %+.2f%%)", result.frequency[Idx(mode)], exact,
                        (result.frequency[Idx(mode)] / exact - 1.0) * 100.0);
        }
        std::printf("\n");
        for (int mode = 0; mode < 3; ++mode) {
            const double exact = (2.0 * (mode + 1) - 1.0) * speed / (4.0 * length);
            CHECK(result.frequency[Idx(mode)] > exact * 0.95);
            CHECK(result.frequency[Idx(mode)] < exact * 1.15);
        }
        if (n == 32) {
            CHECK(std::fabs(result.frequency[0] / (speed / (4.0 * length)) - 1.0) < 0.002);
        }
    }
}

// --- Consistent and lumped bracket the answer -------------------------------

void TestMassBracketing() {
    std::printf("consistent and lumped mass:\n");
    const double length = 2.0;
    const double youngs = 210e9;
    const double density = 7850.0;
    const double speed = std::sqrt(youngs / density);
    const double exact = speed / (4.0 * length);
    auto run = [&](bool lumped) {
        AnalysisModel model = Bar(Vec3d{length, 0.05, 0.05}, 8, 1, 1, youngs, 0.0, density);
        for (int node = 0; node < model.NodeCount(); ++node) {
            fem::Constraint constraint;
            constraint.node = node;
            if (Near(model.nodes[Idx(node)].x, 0.0, 1.0)) constraint.fixed[0] = true;
            constraint.fixed[1] = true;
            constraint.fixed[2] = true;
            model.constraints.push_back(constraint);
        }
        ModalOptions options;
        options.modes = 1;
        options.lumped_mass = lumped;
        ModalResult result;
        if (!fem::SolveModal(model, {}, options, &result)) {
            std::printf("    %s\n", result.error.c_str());
            ++failures;
            return 0.0;
        }
        return result.frequency[0];
    };
    const double consistent = run(false);
    const double lumped = run(true);
    std::printf("  lumped %.4f Hz <= exact %.4f Hz <= consistent %.4f Hz\n", lumped, exact,
                consistent);
    // A LUMPED MASS MATRIX IS NOT A CRUDER CONSISTENT ONE, it errs the
    // other way: consistent mass over-estimates every frequency and lumped
    // under-estimates them, so the two together bracket the answer. That
    // is worth far more than either alone, and it is a property of the
    // formulation rather than of this problem -- so a solver that got it
    // backwards would be caught here whatever it was solving.
    CHECK(consistent > exact);
    CHECK(lumped < exact);
    CHECK(consistent < exact * 1.02);
    CHECK(lumped > exact * 0.98);
}

// --- The Sturm check --------------------------------------------------------

void TestSturmCheck() {
    std::printf("counting modes by inertia rather than by iteration:\n");
    AnalysisModel model = Bar(Vec3d{1.0, 0.1, 0.06}, 6, 2, 2, 210e9, 0.3, 7850.0);
    for (int node = 0; node < model.NodeCount(); ++node) {
        if (!Near(model.nodes[Idx(node)].x, 0.0, 1.0)) continue;
        fem::Constraint constraint;
        constraint.node = node;
        constraint.fixed[0] = constraint.fixed[1] = constraint.fixed[2] = true;
        model.constraints.push_back(constraint);
    }
    ModalOptions options;
    options.modes = 8;
    ModalResult result;
    CHECK_MESSAGE(fem::SolveModal(model, {}, options, &result), result.error);
    if (!result.ok) return;
    std::printf("  eight frequencies:");
    for (const double f : result.frequency) std::printf(" %.1f", f);
    std::printf("\n");
    std::printf("  the factorization counts %d below the highest; %s\n", result.modes_below,
                result.sturm_agrees ? "they agree" : "THEY DO NOT");
    CHECK(result.sturm_agrees);
    CHECK(result.modes_below == 8);

    // AND THE COUNT IS RIGHT AT EVERY CUT, not just at the last one. Each
    // frequency the iteration returned should have exactly as many modes
    // below it as its own index -- which turns one comparison into eight,
    // and would catch a solver that found the right modes in the wrong
    // order as well as one that missed one.
    for (int mode = 0; mode < 8; ++mode) {
        const double midpoint =
            mode == 0 ? result.frequency[0] * 0.5
                      : 0.5 * (result.frequency[Idx(mode - 1)] + result.frequency[Idx(mode)]);
        int below = -1;
        std::string error;
        CHECK_MESSAGE(fem::CountModesBelow(model, {}, midpoint, false, &below, &error), error);
        CHECK(below == mode);
    }

    // A deliberately missed mode: ask for fewer than there are below the
    // highest, which is what happens when two frequencies are close and
    // the iteration converges to one of them twice.
    int below_third = 0;
    std::string error;
    CHECK_MESSAGE(fem::CountModesBelow(model, {}, result.frequency[3] * 1.001, false, &below_third,
                                       &error),
                  error);
    std::printf("  below the fourth frequency there are %d modes, as there should be\n",
                below_third);
    CHECK(below_third == 4);
}

// --- Mode shapes are orthogonal and normalised ------------------------------

void TestShapes() {
    std::printf("the mode shapes themselves:\n");
    AnalysisModel model = Bar(Vec3d{1.0, 0.1, 0.06}, 5, 2, 2, 210e9, 0.3, 7850.0);
    for (int node = 0; node < model.NodeCount(); ++node) {
        if (!Near(model.nodes[Idx(node)].x, 0.0, 1.0)) continue;
        fem::Constraint constraint;
        constraint.node = node;
        constraint.fixed[0] = constraint.fixed[1] = constraint.fixed[2] = true;
        model.constraints.push_back(constraint);
    }
    ModalOptions options;
    options.modes = 5;
    ModalResult result;
    CHECK_MESSAGE(fem::SolveModal(model, {}, options, &result), result.error);
    if (!result.ok) return;

    fem::SparsityPattern pattern;
    std::string error;
    CHECK_MESSAGE(pattern.Build(model, &error), error);
    num::SparseMatrix mass;
    CHECK_MESSAGE(fem::AssembleMass(model, pattern, false, &mass, &error), error);
    auto flatten = [&](const std::vector<Vec3d> &shape) {
        std::vector<double> out(Idx(model.NodeCount() * 3), 0.0);
        for (int node = 0; node < model.NodeCount(); ++node) {
            out[Idx(node * 3 + 0)] = shape[Idx(node)].x;
            out[Idx(node * 3 + 1)] = shape[Idx(node)].y;
            out[Idx(node * 3 + 2)] = shape[Idx(node)].z;
        }
        return out;
    };
    double worst_norm = 0.0;
    double worst_cross = 0.0;
    for (int i = 0; i < 5; ++i) {
        const std::vector<double> a = flatten(result.shape[Idx(i)]);
        std::vector<double> ma;
        mass.Multiply(a, &ma);
        for (int j = 0; j < 5; ++j) {
            const std::vector<double> b = flatten(result.shape[Idx(j)]);
            double product = 0.0;
            for (std::size_t k = 0; k < a.size(); ++k) product += b[k] * ma[k];
            if (i == j) {
                worst_norm = std::max(worst_norm, std::fabs(product - 1.0));
            } else {
                worst_cross = std::max(worst_cross, std::fabs(product));
            }
        }
    }
    // MASS-ORTHONORMAL, which is what makes a modal superposition's
    // participation factors mean anything and is not automatic: shapes
    // orthogonalised in the Euclidean inner product rather than the mass
    // one come out nearly right and not orthogonal, and a forced-response
    // built on them is wrong in a way that only shows up off resonance.
    std::printf("  phi^T M phi is 1 to %.2e, and 0 between modes to %.2e\n", worst_norm,
                worst_cross);
    CHECK(worst_norm < 1e-9);
    CHECK(worst_cross < 1e-8);
}


// --- Buckling (Part I.5) ----------------------------------------------------

// A column: pinned at both ends against lateral motion, held axially at
// one end and pushed at the other. Euler's answer is
// `P_cr = pi^2 E I / L^2`, and the point of the case is that it is a
// *stability* limit rather than a strength one -- the column is nowhere
// near yielding when it goes.
struct Column {
    AnalysisModel model;
    double euler = 0.0;
    double applied = 0.0;
};

// Hex8 into Hex20, by adding the mid-edge nodes.
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

// QUADRATIC ELEMENTS, AND NOT AS A REFINEMENT. A column slender enough
// for Euler's formula to mean anything has elements twenty-five times
// longer than they are thick, and a trilinear hexahedron in bending
// cannot represent the bending mode without a spurious shear along with
// it -- Part H.1 measured that and this is what it looks like downstream.
// With Hex8 the critical load came out a hundred and twenty times too
// high and *fell* with refinement, 2.1e6 to 5.6e5 to 2.7e5, which is
// locking easing rather than a solution converging. Hex20 has the bending
// mode in its space and needs no such apology.
// THE END CONDITION IS THE WHOLE OF EULER'S FORMULA, and it is easier to
// get wrong than it looks. Fixing y and z across the far face does *not*
// clamp it: rotation about y tilts the face, which moves its nodes along
// x, and x is free there -- so that end is pinned. Fixing all three at the
// near face does clamp it, because the same rotation is resisted by the x
// constraint. The first attempt here fixed all three at one end and two at
// the other and was described as pinned-pinned; it is clamped-pinned, and
// the solver said so in two ways at once -- a critical load 2.24 times
// Euler against the 2.046 that condition predicts, and a second-to-first
// mode ratio of 3.00 against the 2.96 it predicts rather than the 4.0 that
// pinned-pinned does.
//
// So both are built and both are checked, which is a stronger test than
// either: it says the solver responds to the end condition, not merely
// that it lands near a number.
Column MakeColumn(double length, double width, double depth, int n, double load,
                  bool clamp_base) {
    Column out;
    const double youngs = 210e9;
    out.model = Bar(Vec3d{length, width, depth}, n, 1, 1, youngs, 0.3, 7850.0);
    MakeQuadratic(&out.model);
    const double second_moment = width * depth * depth * depth / 12.0;
    out.euler = kPi * kPi * youngs * second_moment / (length * length);
    out.applied = load;

    std::vector<int> pushed;
    for (int node = 0; node < out.model.NodeCount(); ++node) {
        const Vec3d &p = out.model.nodes[Idx(node)];
        fem::Constraint constraint;
        constraint.node = node;
        if (Near(p.x, 0.0, 1.0)) {
            constraint.fixed[1] = constraint.fixed[2] = true;
            // Clamped holds x across the whole face, which resists the
            // rotation; pinned holds it at one node, which does not and
            // only stops the column sliding along itself.
            if (clamp_base || (Near(p.y, 0.0, 1.0) && Near(p.z, 0.0, 1.0))) {
                constraint.fixed[0] = true;
            }
        } else if (Near(p.x, length, length)) {
            constraint.fixed[1] = constraint.fixed[2] = true;
            pushed.push_back(node);
        }
        // NOTHING HELD ALONG THE LENGTH. Holding u_y at every node to
        // force weak-axis bending looks harmless and is not: it forbids
        // the cross-section's Poisson contraction, which turns the beam
        // into a strip in cylindrical bending and stiffens it by exactly
        // 1/(1 - v^2). At Poisson 0.3 that is 9.9%, and the buckling load
        // came out 8.3% high and climbing towards it -- a discrepancy that
        // looks like mesh convergence and is a different structure. The
        // section is 50 by 20, so the weak axis is the short one anyway
        // and the first mode is the wanted one without being told.
        out.model.constraints.push_back(constraint);
    }
    for (const int node : pushed) {
        out.model.loads.push_back(
            fem::NodalLoad{node, Vec3d{-load / static_cast<double>(pushed.size()), 0, 0}});
    }
    return out;
}

void TestEulerColumn() {
    std::printf("a column, against Euler, at two end conditions:\n");
    const double length = 2.0;
    const double width = 0.05;
    const double depth = 0.02;
    // Pinned-pinned buckles at Euler's load; clamped at one end and
    // pinned at the other buckles at 2.0457 times it, which is the
    // standard effective-length factor 0.699 squared and inverted.
    struct Case {
        bool clamp;
        const char *name;
        double against_euler;
        double mode_ratio;
    };
    for (const Case &which : {Case{false, "pinned-pinned", 1.0, 4.0},
                              Case{true, "clamped-pinned", 2.0457, 2.96}}) {
        std::printf("  %s (expecting %.4f x Euler)\n", which.name, which.against_euler);
        double last = 0.0;
        for (const int n : {10, 20, 30}) {
            const Column column = MakeColumn(length, width, depth, n, 1000.0, which.clamp);
            fem::BucklingOptions options;
            options.modes = 2;
            fem::BucklingResult result;
            CHECK_MESSAGE(fem::SolveBuckling(column.model, {}, options, &result), result.error);
            if (!result.ok || result.factor.size() < 2) return;
            const double critical = result.factor[0] * column.applied;
            const double wanted = column.euler * which.against_euler;
            std::printf("      %2d elements: P_cr = %.2f N against %.2f (%+.2f%%), "
                        "second mode %.3f times the first\n",
                        n, critical, wanted, (critical / wanted - 1.0) * 100.0,
                        result.factor[1] / result.factor[0]);
            CHECK(result.factor[0] > 0.0);
            CHECK(critical > wanted * 0.95);
            CHECK(critical < wanted * 1.10);
            CHECK(result.factor[1] / result.factor[0] > which.mode_ratio * 0.9);
            CHECK(result.factor[1] / result.factor[0] < which.mode_ratio * 1.1);
            last = critical;
        }
        CHECK(last > 0.0);
    }

    // THE FACTOR SCALES INVERSELY WITH THE LOAD, EXACTLY. The buckling
    // load is a property of the structure, so doubling the applied load
    // halves the factor -- to round-off, not approximately, because the
    // stress stiffness is linear in the stress and the stress is linear in
    // the load. It is the sharpest check here and needs no closed form.
    const Column single = MakeColumn(length, width, depth, 12, 1000.0, false);
    const Column doubled = MakeColumn(length, width, depth, 12, 2000.0, false);
    fem::BucklingOptions options;
    options.modes = 1;
    fem::BucklingResult one;
    fem::BucklingResult two;
    CHECK_MESSAGE(fem::SolveBuckling(single.model, {}, options, &one), one.error);
    CHECK_MESSAGE(fem::SolveBuckling(doubled.model, {}, options, &two), two.error);
    if (!one.ok || !two.ok) return;
    std::printf("  at 1000 N the factor is %.6f, at 2000 N it is %.6f (ratio %.9f)\n",
                one.factor[0], two.factor[0], one.factor[0] / two.factor[0]);
    CHECK(std::fabs(one.factor[0] / two.factor[0] - 2.0) < 1e-6);
}

void TestTensionDoesNotBuckle() {
    std::printf("the same column pulled instead of pushed:\n");
    // A COLUMN IN TENSION HAS NO POSITIVE BUCKLING FACTOR, and the
    // negative one it does have is not a mistake: it says the structure
    // buckles if the load is *reversed*, at the same magnitude. Reporting
    // only positive factors would hide exactly that, which is a real
    // design question for anything whose load can reverse.
    const Column pushed = MakeColumn(2.0, 0.05, 0.02, 12, 1000.0, false);
    Column pulled = MakeColumn(2.0, 0.05, 0.02, 12, 1000.0, false);
    for (fem::NodalLoad &load : pulled.model.loads) load.force = load.force * -1.0;
    fem::BucklingOptions options;
    options.modes = 2;
    fem::BucklingResult compression;
    fem::BucklingResult tension;
    CHECK_MESSAGE(fem::SolveBuckling(pushed.model, {}, options, &compression), compression.error);
    CHECK_MESSAGE(fem::SolveBuckling(pulled.model, {}, options, &tension), tension.error);
    if (!compression.ok || !tension.ok) return;
    std::printf("  in compression the first factor is %+.4f, in tension %+.4f\n",
                compression.factor[0], tension.factor[0]);
    CHECK(compression.factor[0] > 0.0);
    CHECK(tension.factor[0] < 0.0);
    CHECK(std::fabs(std::fabs(tension.factor[0]) - compression.factor[0]) <
          compression.factor[0] * 1e-6);
}

void TestBucklingShape() {
    std::printf("the first buckling shape:\n");
    const Column column = MakeColumn(2.0, 0.05, 0.02, 16, 1000.0, false);
    fem::BucklingOptions options;
    options.modes = 2;
    fem::BucklingResult result;
    CHECK_MESSAGE(fem::SolveBuckling(column.model, {}, options, &result), result.error);
    if (!result.ok || result.factor.size() < 2) return;
    // A HALF SINE, which is a pinned-pinned column's first mode. Checked
    // by correlation rather than by eye: dotted against sin(pi x / L),
    // where anything but the first mode correlates badly.
    double dot = 0.0;
    double shape_norm = 0.0;
    double sine_norm = 0.0;
    for (int node = 0; node < column.model.NodeCount(); ++node) {
        const double x = column.model.nodes[Idx(node)].x;
        const double sine = std::sin(kPi * x / 2.0);
        const double value = result.shape[0][Idx(node)].z;
        dot += value * sine;
        shape_norm += value * value;
        sine_norm += sine * sine;
    }
    const double correlation = std::fabs(dot) / std::sqrt(shape_norm * sine_norm);
    std::printf("  correlates with a half sine to %.6f\n", correlation);
    CHECK(correlation > 0.99);
    // And the second mode is the full sine, at four times the load: the
    // Euler factor goes as n squared.
    std::printf("  second factor %.4f is %.3f times the first (Euler says 4)\n", result.factor[1],
                result.factor[1] / result.factor[0]);
    CHECK(result.factor[1] / result.factor[0] > 3.6);
    CHECK(result.factor[1] / result.factor[0] < 4.4);
}

}  // namespace

void TestUnimplementedIsRefused() {
    std::printf("an option that is not written:\n");
    const AnalysisModel model = Bar(Vec3d{0.3, 0.1, 0.08}, 2, 1, 1, 210e9, 0.3, 7850.0);
    ModalOptions options;
    options.method = fem::ModalMethod::Lanczos;
    ModalResult result;
    CHECK(!fem::SolveModal(model, {}, options, &result));
    CHECK(result.error.find("not written yet") != std::string::npos);
    std::printf("  %s\n", result.error.c_str());
}

int main() {
    TestUnimplementedIsRefused();
    TestRigidBodyModes();
    TestLongitudinalBar();
    TestMassBracketing();
    TestSturmCheck();
    TestShapes();
    TestEulerColumn();
    TestTensionDoesNotBuckle();
    TestBucklingShape();
    if (failures != 0) {
        std::printf("fem_modal_test FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("fem_modal_test passed (%d checks)\n", checks);
    return 0;
}
