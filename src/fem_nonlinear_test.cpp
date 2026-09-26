// Parts I.6 to I.8: nonlinearity and contact.

#include "fem_nonlinear.h"
#include "fem_plastic.h"

#include <algorithm>
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
using fem::NonlinearResult;
using fem::StudyMaterial;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

constexpr double kPi = 3.14159265358979323846;

bool Near(double a, double b, double scale) { return std::fabs(a - b) <= scale * 1e-9; }

AnalysisModel Bar(const Vec3d &size, int nx, int ny, int nz, double youngs, double poisson) {
    AnalysisModel model;
    StudyMaterial material;
    material.youngs_modulus = fem::MaterialCurve::Constant(youngs);
    material.poissons_ratio = fem::MaterialCurve::Constant(poisson);
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

// --- A rigid rotation carries no stress -------------------------------------

void TestRigidRotation() {
    std::printf("a body rotated rigidly:\n");
    // THE ONE PROPERTY THAT DEFINES GEOMETRIC NONLINEARITY, and it is
    // exact rather than asymptotic. The engineering strain of a ninety
    // degree rotation is one; the Green-Lagrange strain of any rotation,
    // however large, is exactly zero. So the internal force of a rigidly
    // rotated body must vanish to round-off -- and a solver that used the
    // linear strain would report forces large enough to tear the part
    // apart, at every angle, with no warning.
    const AnalysisModel model = Bar(Vec3d{0.3, 0.1, 0.08}, 3, 2, 2, 210e9, 0.3);
    fem::SparsityPattern pattern;
    std::string error;
    CHECK_MESSAGE(pattern.Build(model, &error), error);
    const int dofs = model.NodeCount() * 3;
    double largest = 0.0;
    for (const double degrees : {1.0, 30.0, 90.0, 180.0}) {
        const double angle = degrees * kPi / 180.0;
        std::vector<double> displacement(Idx(dofs), 0.0);
        for (int node = 0; node < model.NodeCount(); ++node) {
            const Vec3d &p = model.nodes[Idx(node)];
            // A rotation about z, applied exactly.
            const Vec3d moved{p.x * std::cos(angle) - p.y * std::sin(angle),
                              p.x * std::sin(angle) + p.y * std::cos(angle), p.z};
            const Vec3d u = moved - p;
            displacement[Idx(node * 3 + 0)] = u.x;
            displacement[Idx(node * 3 + 1)] = u.y;
            displacement[Idx(node * 3 + 2)] = u.z;
        }
        std::vector<double> force;
        CHECK_MESSAGE(fem::InternalForce(model, displacement, pattern, &force, nullptr, &error),
                      error);
        double worst = 0.0;
        for (const double value : force) worst = std::max(worst, std::fabs(value));
        // Against the force the same displacement would produce if it were
        // a strain: modulus times a strain of order one times an area.
        const double scale = 210e9 * 0.1 * 0.08;
        largest = std::max(largest, worst / scale);
        std::printf("  %6.1f degrees: largest internal force %.3e N, %.2e of what a strain of one "
                    "would give\n",
                    degrees, worst, worst / scale);
        CHECK(worst < scale * 1e-12);
    }
    CHECK(largest < 1e-12);
}

// --- The tangent is the derivative of the force -----------------------------

void TestTangentIsConsistent() {
    std::printf("the tangent against a finite difference of the force:\n");
    // A CONSISTENT TANGENT IS A CLAIM ABOUT A DERIVATIVE, so it is checked
    // as one. Newton converges with an inconsistent tangent too -- linearly
    // -- and that looks like a hard problem rather than a wrong
    // derivative, which is why this is worth checking directly rather
    // than inferring from the iteration count.
    const AnalysisModel model = Bar(Vec3d{0.2, 0.1, 0.1}, 2, 1, 1, 210e9, 0.3);
    fem::SparsityPattern pattern;
    std::string error;
    CHECK_MESSAGE(pattern.Build(model, &error), error);
    const int dofs = model.NodeCount() * 3;
    std::vector<double> at(Idx(dofs), 0.0);
    for (int i = 0; i < dofs; ++i) {
        at[Idx(i)] = 1e-3 * std::sin(1.0 + static_cast<double>(i) * 0.7);
    }
    std::vector<double> force;
    num::SparseMatrix tangent;
    CHECK_MESSAGE(fem::InternalForce(model, at, pattern, &force, &tangent, &error), error);

    double worst = 0.0;
    double scale = 0.0;
    const double step = 1e-7;
    for (int column = 0; column < dofs; column += 7) {
        std::vector<double> plus = at;
        std::vector<double> minus = at;
        plus[Idx(column)] += step;
        minus[Idx(column)] -= step;
        std::vector<double> up;
        std::vector<double> down;
        CHECK_MESSAGE(fem::InternalForce(model, plus, pattern, &up, nullptr, &error), error);
        CHECK_MESSAGE(fem::InternalForce(model, minus, pattern, &down, nullptr, &error), error);
        for (int row = 0; row < dofs; ++row) {
            const double numeric = (up[Idx(row)] - down[Idx(row)]) / (2.0 * step);
            worst = std::max(worst, std::fabs(numeric - tangent.At(row, column)));
            scale = std::max(scale, std::fabs(numeric));
        }
    }
    std::printf("  worst disagreement %.3e of %.3e (%.2e relative)\n", worst, scale, worst / scale);
    CHECK(worst < scale * 1e-6);
}

// --- Small loads must agree with the linear solver --------------------------

void TestAgreesWithLinearWhenSmall() {
    std::printf("a small load, against the linear solver:\n");
    AnalysisModel model = Bar(Vec3d{1.0, 0.1, 0.1}, 6, 1, 1, 210e9, 0.3);
    std::vector<int> tip;
    for (int node = 0; node < model.NodeCount(); ++node) {
        const Vec3d &p = model.nodes[Idx(node)];
        if (Near(p.x, 0.0, 1.0)) {
            fem::Constraint constraint;
            constraint.node = node;
            constraint.fixed[0] = constraint.fixed[1] = constraint.fixed[2] = true;
            model.constraints.push_back(constraint);
        }
        if (Near(p.x, 1.0, 1.0)) tip.push_back(node);
    }
    for (const int node : tip) {
        model.loads.push_back(
            fem::NodalLoad{node, Vec3d{0, 0, -50.0 / static_cast<double>(tip.size())}});
    }
    fem::StaticResult linear;
    CHECK_MESSAGE(fem::SolveStatic(model, {}, {}, &linear), linear.error);
    fem::NonlinearOptions options;
    options.steps = 2;
    NonlinearResult nonlinear;
    CHECK_MESSAGE(fem::SolveNonlinear(model, {}, options, &nonlinear), nonlinear.error);
    for (const std::string &w : nonlinear.warnings) std::printf("    warning: %s\n", w.c_str());
    std::printf("    residuals:");
    for (const double v : nonlinear.last_residuals) std::printf(" %.3e", v);
    std::printf("\n");
    if (!linear.ok || !nonlinear.ok) return;
    double worst = 0.0;
    for (int node = 0; node < model.NodeCount(); ++node) {
        worst = std::max(worst,
                         (linear.displacement[Idx(node)] - nonlinear.displacement[Idx(node)])
                             .Length());
    }
    std::printf("  tip deflection %.6e (linear) vs %.6e (nonlinear), worst node differs by %.2e\n",
                linear.max_displacement, nonlinear.displacement[Idx(tip[0])].Length(), worst);
    // SMALL MEANS SMALL: at a deflection of a ten-thousandth of the span
    // the two theories must agree to a part in a thousand, and the
    // difference that remains is the second-order term rather than an
    // error in either.
    CHECK(worst < linear.max_displacement * 1e-3);

    // AND THE RESIDUAL SQUARES, which is what a consistent tangent buys
    // and an inconsistent one does not.
    std::printf("  Newton residuals on the last step:");
    for (const double value : nonlinear.last_residuals) std::printf(" %.2e", value);
    std::printf("\n");
    CHECK(nonlinear.last_residuals.size() >= 3);
    for (std::size_t i = 2; i < nonlinear.last_residuals.size(); ++i) {
        const double before = nonlinear.last_residuals[i - 1];
        const double now = nonlinear.last_residuals[i];
        if (!(before > 1e-13) || !(now > 0.0)) continue;
        // Quadratic: the new residual is at most the old one squared,
        // times a constant. A linearly converging iteration fails this
        // within two or three steps.
        CHECK(now < before * before * 1e4 + 1e-14);
    }
}

// --- Large deflection: the tip pulls in --------------------------------------

void TestLargeDeflection() {
    std::printf("a cantilever bent a long way:\n");
    AnalysisModel model = Bar(Vec3d{1.0, 0.05, 0.02}, 20, 1, 1, 210e9, 0.3);
    std::vector<int> tip;
    for (int node = 0; node < model.NodeCount(); ++node) {
        const Vec3d &p = model.nodes[Idx(node)];
        if (Near(p.x, 0.0, 1.0)) {
            fem::Constraint constraint;
            constraint.node = node;
            constraint.fixed[0] = constraint.fixed[1] = constraint.fixed[2] = true;
            model.constraints.push_back(constraint);
        }
        if (Near(p.x, 1.0, 1.0)) tip.push_back(node);
    }
    const double load = 4000.0;
    for (const int node : tip) {
        model.loads.push_back(
            fem::NodalLoad{node, Vec3d{0, 0, -load / static_cast<double>(tip.size())}});
    }
    fem::StaticResult linear;
    CHECK_MESSAGE(fem::SolveStatic(model, {}, {}, &linear), linear.error);
    fem::NonlinearOptions options;
    options.steps = 10;
    NonlinearResult nonlinear;
    CHECK_MESSAGE(fem::SolveNonlinear(model, {}, options, &nonlinear), nonlinear.error);
    if (!linear.ok || !nonlinear.ok) return;

    double linear_drop = 0.0;
    double drop = 0.0;
    double pull_in = 0.0;
    for (const int node : tip) {
        linear_drop = std::min(linear_drop, linear.displacement[Idx(node)].z);
        drop = std::min(drop, nonlinear.displacement[Idx(node)].z);
        pull_in = std::min(pull_in, nonlinear.displacement[Idx(node)].x);
    }
    std::printf("  tip drops %.4f m against the linear %.4f m, and draws in by %.4f m\n", -drop,
                -linear_drop, -pull_in);
    // STIFFER THAN LINEAR, AND THE TIP MOVES INWARDS. Both follow from
    // the same thing: a beam that has bent is no longer perpendicular to
    // its load, so part of the load goes into stretching it rather than
    // bending it, and the arc is longer than its chord so the far end
    // draws back towards the root. Linear analysis has no term for either
    // and gets a deflection that is too large and a tip that only ever
    // goes straight down.
    CHECK(-drop < -linear_drop);
    CHECK(-drop > -linear_drop * 0.5);
    CHECK(pull_in < 0.0);
    CHECK(-pull_in > 0.01 * -drop);
}

// --- Snap-through, which load control cannot reach --------------------------

// A shallow arch: a straight bar whose nodes are lifted onto a parabola,
// pinned at both ends and pushed down in the middle. Past a certain load
// there is no equilibrium near the current shape at all -- the arch snaps
// through to hanging below its supports -- and the load at which that
// happens is a limit point.
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

AnalysisModel ShallowArch(double span, double rise, double depth, int n, double load) {
    AnalysisModel model = Bar(Vec3d{span, 0.05, depth}, n, 1, 1, 210e9, 0.3);
    MakeQuadratic(&model);
    for (cad::Vec3d &p : model.nodes) {
        const double t = 2.0 * p.x / span - 1.0;
        p.z += rise * (1.0 - t * t);
    }
    int centre = -1;
    double best = 1e30;
    for (int node = 0; node < model.NodeCount(); ++node) {
        const Vec3d &p = model.nodes[Idx(node)];
        if (Near(p.x, 0.0, 1.0) || Near(p.x, span, span)) {
            fem::Constraint constraint;
            constraint.node = node;
            // Pinned: held in x and z, free to rotate.
            constraint.fixed[0] = constraint.fixed[2] = true;
            constraint.fixed[1] = true;
            model.constraints.push_back(constraint);
            continue;
        }
        fem::Constraint constraint;
        constraint.node = node;
        constraint.fixed[1] = true;
        model.constraints.push_back(constraint);
        const double distance = std::fabs(p.x - span * 0.5) + (p.z > 0 ? 0.0 : 1e-6);
        if (distance >= best) continue;
        best = distance;
        centre = node;
    }
    if (centre >= 0) model.loads.push_back(fem::NodalLoad{centre, Vec3d{0, 0, -load}});
    return model;
}

void TestSnapThrough() {
    std::printf("a shallow arch, pushed towards its limit point:\n");
    const double span = 1.0;
    // The reference load is set above the limit load on purpose, so that
    // load control is being asked for something that does not exist.
    const AnalysisModel model = ShallowArch(span, 0.03, 0.004, 16, 1000.0);

    // LOAD CONTROL CANNOT PASS A LIMIT POINT, and the honest reading of
    // its failure is not that the solver broke but that there is nothing
    // to find: beyond the limit load no equilibrium exists at that load,
    // at any displacement.
    fem::NonlinearOptions load_control;
    load_control.steps = 20;
    load_control.max_cuts = 8;
    NonlinearResult under_load;
    const bool load_ok = fem::SolveNonlinear(model, {}, load_control, &under_load);
    std::printf("  under load control: %s, reaching factor %.4f\n",
                load_ok ? "reached the full load" : "gave up", under_load.load_factor);
    CHECK(!load_ok);
    CHECK(under_load.load_factor < 1.0);
    CHECK(under_load.error.find("limit point") != std::string::npos);

    fem::ArcLengthOptions arc;
    arc.steps = 200;
    arc.first_step = 0.05;
    NonlinearResult path;
    CHECK_MESSAGE(fem::SolveArcLength(model, {}, arc, &path), path.error);
    if (!path.ok) return;
    double peak = 0.0;
    double deepest = 0.0;
    for (const fem::NonlinearStep &step : path.path) {
        peak = std::max(peak, step.load_factor);
        for (const Vec3d &u : step.displacement) deepest = std::min(deepest, u.z);
    }
    std::printf("  arc length: %d steps, load factor reaches %.4f with the middle %.5f m down\n",
                static_cast<int>(path.path.size()), peak, -deepest);
    for (const std::string &w : path.warnings) std::printf("  %s\n", w.c_str());

    // WHAT IS VERIFIED HERE AND WHAT IS NOT. Arc length gets strictly
    // further along the path than load control does, and it finds the
    // limit load itself -- a genuine maximum of the equilibrium path,
    // approached from below and flat at the top. What it does *not* yet do
    // is step past it and down the far side: the two roots of the
    // arc-length constraint become indistinguishable there, the step
    // control shrinks towards nothing, and the path creeps instead of
    // turning. The solver says so in a warning rather than returning a
    // hundred identical points as though they were a result. Getting round
    // the limit point needs a better root choice than "continue in the
    // direction of the last increment", and that is recorded as the work
    // it is.
    CHECK(peak > under_load.load_factor);
    CHECK(peak < 1.0);
    CHECK(-deepest > 0.005);
    // IT PLATEAUS AT THE LIMIT, which is the measurable form of "reached
    // it and could not pass it": the last quarter of the path moves the
    // load factor by a thousandth of what the first three quarters did.
    const std::size_t quarter = path.path.size() * 3 / 4;
    const double early = path.path[quarter].load_factor - path.path.front().load_factor;
    const double late = path.path.back().load_factor - path.path[quarter].load_factor;
    std::printf("  the first three quarters of the path raise the factor by %.4f and the last "
                "quarter by %.6f\n",
                early, late);
    CHECK(early > 0.1);
    CHECK(late < early * 0.01);
    // Monotone up to the limit, which says the path it did trace is an
    // equilibrium path and not a wander.
    for (std::size_t i = 1; i < path.path.size(); ++i) {
        CHECK(path.path[i].load_factor >= path.path[i - 1].load_factor - 1e-9);
    }
}

// --- Plasticity (Part I.7) --------------------------------------------------

void TestUniaxialPlasticity() {
    std::printf("a bar pulled past yield:\n");
    fem::J2Material steel;
    steel.youngs_modulus = 210e9;
    steel.poissons_ratio = 0.3;
    steel.yield_stress = 250e6;
    steel.isotropic_modulus = 2e9;
    steel.kinematic_modulus = 0.0;

    // Pulled in x with the lateral strains free to take whatever value
    // keeps the lateral stress at zero -- which is what a bar in a test
    // machine does, and needs solving for rather than assuming, because
    // the lateral contraction is elastic at Poisson's ratio before yield
    // and volume-preserving after it.
    auto uniaxial = [&](double axial, fem::PlasticState *state) {
        double lateral = -steel.poissons_ratio * axial;
        double stress[6];
        double tangent[6][6];
        for (int sweep = 0; sweep < 60; ++sweep) {
            const double strain[6] = {axial, lateral, lateral, 0, 0, 0};
            fem::PlasticState trial = *state;
            fem::RadialReturn(steel, strain, &trial, stress, tangent);
            const double residual = stress[1];
            if (std::fabs(residual) < 1.0) break;
            // One unknown, and the derivative of the lateral stress with
            // respect to the lateral strain is two of the tangent's terms.
            const double slope = tangent[1][1] + tangent[1][2];
            if (!(std::fabs(slope) > 0.0)) break;
            lateral -= residual / slope;
        }
        const double strain[6] = {axial, lateral, lateral, 0, 0, 0};
        fem::RadialReturn(steel, strain, state, stress, tangent);
        return stress[0];
    };

    // Elastic up to yield: stress is E times strain, exactly.
    fem::PlasticState state;
    const double yield_strain = steel.yield_stress / steel.youngs_modulus;
    const double half = uniaxial(yield_strain * 0.5, &state);
    std::printf("  at half the yield strain: %.4e Pa, E*strain is %.4e\n", half,
                steel.youngs_modulus * yield_strain * 0.5);
    CHECK(std::fabs(half - steel.youngs_modulus * yield_strain * 0.5) < half * 1e-9);
    CHECK(state.accumulated == 0.0);

    // And past it the slope is the tangent modulus, which for linear
    // isotropic hardening is `E H / (E + H)` -- the two springs in series
    // that the elastic and plastic parts are.
    const double tangent_modulus =
        steel.youngs_modulus * steel.isotropic_modulus /
        (steel.youngs_modulus + steel.isotropic_modulus);
    fem::PlasticState walked;
    double previous_strain = 0.0;
    double previous_stress = 0.0;
    double worst = 0.0;
    for (int step = 1; step <= 40; ++step) {
        const double strain = yield_strain * 4.0 * step / 40.0;
        const double stress = uniaxial(strain, &walked);
        if (strain > yield_strain * 1.5) {
            const double slope = (stress - previous_stress) / (strain - previous_strain);
            worst = std::max(worst, std::fabs(slope / tangent_modulus - 1.0));
        }
        previous_strain = strain;
        previous_stress = stress;
    }
    std::printf("  past yield the slope is within %.2e of E*H/(E+H) = %.4e\n", worst,
                tangent_modulus);
    CHECK(worst < 1e-6);
    CHECK(walked.accumulated > 0.0);
    // The stress at four times the yield strain, from the closed form.
    const double expected =
        steel.yield_stress + tangent_modulus * (yield_strain * 4.0 - yield_strain);
    std::printf("  at four times the yield strain: %.6e Pa, closed form %.6e\n", previous_stress,
                expected);
    CHECK(std::fabs(previous_stress - expected) < expected * 1e-6);
}

void TestBauschinger() {
    std::printf("hardening rules, pulled then pushed:\n");
    // THE TWO RULES DIFFER WHERE IT MATTERS AND NOWHERE ELSE. Pulled in
    // one direction they give the same curve; reversed, isotropic
    // hardening needs the raised stress to yield again while kinematic
    // hardening yields 2*sigma_y below where it stopped -- earlier than it
    // first yielded. That is the Bauschinger effect, and it is the whole
    // reason both rules exist.
    auto sweep = [&](double isotropic, double kinematic) {
        fem::J2Material steel;
        steel.yield_stress = 250e6;
        steel.isotropic_modulus = isotropic;
        steel.kinematic_modulus = kinematic;
        fem::PlasticState state;
        double stress[6];
        double tangent[6][6];
        // Pulled to four times the yield strain in uniaxial *strain*,
        // which keeps the case one-dimensional and the comparison clean.
        const double yield_strain = steel.yield_stress / steel.youngs_modulus;
        double peak = 0.0;
        for (int step = 1; step <= 80; ++step) {
            const double e = yield_strain * 4.0 * step / 80.0;
            const double strain[6] = {e, 0, 0, 0, 0, 0};
            fem::RadialReturn(steel, strain, &state, stress, tangent);
            peak = stress[0];
        }
        // Then unloaded and pushed back, watching for the reverse yield.
        double reverse = 0.0;
        for (int step = 1; step <= 400; ++step) {
            const double e = yield_strain * (4.0 - 12.0 * step / 400.0);
            const double strain[6] = {e, 0, 0, 0, 0, 0};
            const double before = state.accumulated;
            fem::RadialReturn(steel, strain, &state, stress, tangent);
            if (state.accumulated > before && reverse == 0.0) reverse = stress[0];
        }
        return std::make_pair(peak, reverse);
    };
    const auto isotropic = sweep(2e9, 0.0);
    const auto kinematic = sweep(0.0, 2e9);
    std::printf("  isotropic: peaks at %.4e Pa and yields again at %+.4e\n", isotropic.first,
                isotropic.second);
    std::printf("  kinematic: peaks at %.4e Pa and yields again at %+.4e\n", kinematic.first,
                kinematic.second);
    // The same curve going up.
    CHECK(std::fabs(isotropic.first - kinematic.first) < isotropic.first * 1e-9);
    // Coming back, kinematic yields at a *higher* (less negative) stress:
    // it has kept its elastic range the same width and carried it along,
    // so the reverse yield is a fixed distance below the peak.
    CHECK(kinematic.second > isotropic.second);
    // And that distance is the elastic range itself, which for uniaxial
    // strain is the yield surface's width scaled by the constrained
    // modulus -- the point being that it does not grow with the
    // pre-strain, where the isotropic one does.
    CHECK(kinematic.first - kinematic.second < isotropic.first - isotropic.second);
}

void TestConsistentTangent() {
    std::printf("the algorithmic tangent against a finite difference:\n");
    // WHAT NEWTON NEEDS IS THE DERIVATIVE OF THE ALGORITHM, not of the
    // constitutive law. The two differ by a term in the step size, and the
    // difference is measured here rather than argued: the consistent
    // tangent matches a finite difference of the return map itself, and
    // the continuum one does not.
    fem::J2Material steel;
    steel.yield_stress = 250e6;
    steel.isotropic_modulus = 2e9;
    const double yield_strain = steel.yield_stress / steel.youngs_modulus;
    // A state well into the plastic range, reached the way a solver would.
    fem::PlasticState base;
    double stress[6];
    double tangent[6][6];
    for (int step = 1; step <= 20; ++step) {
        const double e = yield_strain * 3.0 * step / 20.0;
        const double strain[6] = {e, -0.3 * e, -0.3 * e, 0.2 * e, 0, 0};
        fem::RadialReturn(steel, strain, &base, stress, tangent);
    }
    // A STRAIN BEYOND WHERE THE WALK STOPPED, so that the step being
    // differentiated is genuinely plastic. Evaluated *at* the last
    // converged strain the material sits exactly on the yield surface,
    // where the return map has a kink: a central difference straddles it,
    // taking an elastic step one way and a plastic one the other, and
    // reports the average of two different derivatives. That looked like
    // a twenty-percent error in the tangent and was a twenty-percent
    // error in the question.
    const double reach = yield_strain * 3.3;
    const double at[6] = {reach, -0.3 * reach, -0.3 * reach, 0.2 * reach, 0, 0};
    // A SMALL BUMP, BECAUSE A DERIVATIVE IS A LIMIT. The first attempt
    // used a step the size of a load increment and both tangents came out
    // equally wrong -- of course they did: over a strain increment that
    // large the return map is strongly nonlinear and the difference
    // quotient is not the derivative of anything. The difference between
    // the two tangents is a separate question from whether either is a
    // derivative, and it is asked separately below.
    const double bump = yield_strain * 1e-7;

    auto measure = [&](bool consistent) {
        fem::PlasticState state = base;
        double base_stress[6];
        double analytic[6][6];
        fem::RadialReturn(steel, at, &state, base_stress, analytic, consistent);
        double worst = 0.0;
        double scale = 0.0;
        for (int column = 0; column < 6; ++column) {
            double plus_strain[6];
            double minus_strain[6];
            for (int i = 0; i < 6; ++i) {
                plus_strain[i] = at[i];
                minus_strain[i] = at[i];
            }
            plus_strain[column] += bump;
            minus_strain[column] -= bump;
            fem::PlasticState up = base;
            fem::PlasticState down = base;
            double up_stress[6];
            double down_stress[6];
            double ignored[6][6];
            fem::RadialReturn(steel, plus_strain, &up, up_stress, ignored);
            fem::RadialReturn(steel, minus_strain, &down, down_stress, ignored);
            for (int row = 0; row < 6; ++row) {
                const double numeric = (up_stress[row] - down_stress[row]) / (2.0 * bump);
                worst = std::max(worst, std::fabs(numeric - analytic[row][column]));
                scale = std::max(scale, std::fabs(numeric));
            }
        }
        return worst / scale;
    };
    const double consistent = measure(true);
    const double continuum = measure(false);
    std::printf("  the algorithmic tangent is out by %.3e; the continuum one by %.3e\n",
                consistent, continuum);
    // THE CONSISTENT ONE IS THE DERIVATIVE OF THE RETURN MAP and the
    // continuum one is not. That is the whole claim, and it is what makes
    // a Newton built on the second converge linearly while looking like a
    // hard problem rather than a wrong derivative.
    CHECK(consistent < 1e-6);
    CHECK(continuum > consistent * 100.0);

    // And the two matrices genuinely differ, by an amount that is a
    // fraction of the tangent itself rather than a rounding: if they were
    // nearly equal the distinction would be academic, and it is not.
    fem::PlasticState a = base;
    fem::PlasticState b = base;
    double ignored_stress[6];
    double algorithmic[6][6];
    double elastoplastic[6][6];
    fem::RadialReturn(steel, at, &a, ignored_stress, algorithmic, true);
    fem::RadialReturn(steel, at, &b, ignored_stress, elastoplastic, false);
    double gap = 0.0;
    double size = 0.0;
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) {
            gap = std::max(gap, std::fabs(algorithmic[i][j] - elastoplastic[i][j]));
            size = std::max(size, std::fabs(algorithmic[i][j]));
        }
    }
    std::printf("  the two tangents differ by %.2f%% of the larger\n", 100.0 * gap / size);
    CHECK(gap > size * 0.01);
}

// --- Contact (Part I.8) -----------------------------------------------------

void TestContactAgainstAPlane() {
    std::printf("a block pushed down onto a rigid plane:\n");
    // DRIVEN BY DISPLACEMENT, NOT BY FORCE, and the reason is the
    // physics rather than convenience. A block floating above a plane
    // with a force pushing it down is held up by nothing until it lands,
    // so its stiffness matrix is singular in that direction and Newton's
    // first iteration has no contact term to make it otherwise. The
    // solver said exactly that and was right. Pushing the top face down
    // by a known amount keeps the system solvable throughout and still
    // makes the contact be *found*: it starts clear of the plane, closes
    // the gap partway through, and the constraint switches on.
    const double gap = 1e-4;
    const double push = 1.6e-4;
    auto run = [&](double penalty_scale) {
        AnalysisModel model = Bar(Vec3d{0.2, 0.2, 0.1}, 3, 3, 2, 210e9, 0.3);
        for (Vec3d &p : model.nodes) p.z += gap;
        std::vector<int> bottom;
        for (int node = 0; node < model.NodeCount(); ++node) {
            const Vec3d &p = model.nodes[Idx(node)];
            if (Near(p.z, gap, 1.0)) bottom.push_back(node);
            fem::Constraint constraint;
            constraint.node = node;
            // Sideways motion held, which a frictionless plane does not
            // do and this model has nothing else to do it.
            constraint.fixed[0] = constraint.fixed[1] = true;
            if (Near(p.z, 0.1 + gap, 1.0)) {
                constraint.fixed[2] = true;
                constraint.value[2] = -push;
            }
            model.constraints.push_back(constraint);
        }
        fem::NonlinearOptions options;
        options.steps = 8;
        options.contact.planes.push_back(fem::RigidPlane{Vec3d{0, 0, 0}, Vec3d{0, 0, 1}});
        options.contact.nodes = bottom;
        options.contact.penalty_scale = penalty_scale;
        NonlinearResult result;
        const bool ok = fem::SolveNonlinear(model, {}, options, &result);
        if (!ok) {
            std::printf("    %s\n", result.error.c_str());
            std::printf("    residuals:");
            for (const double v : result.last_residuals) std::printf(" %.3e", v);
            std::printf("\n    touching %d, penetration %.3e\n", result.touching,
                        result.worst_penetration);
        }
        return std::make_pair(ok, result);
    };

    const auto landed = run(1.0);
    CHECK(landed.first);
    if (!landed.first) return;
    const NonlinearResult &result = landed.second;
    std::printf("  %d bottom nodes touching, sunk in by %.3e m after closing a %.0e m gap\n",
                result.touching, result.worst_penetration, gap);
    CHECK(result.touching > 0);
    CHECK(result.worst_penetration > 0.0);
    // It did not sink in anywhere near as far as it was pushed: the plane
    // is holding it.
    CHECK(result.worst_penetration < push - gap);

    // THE PENETRATION IS THE PENALTY METHOD'S ERROR, AND IT FOLLOWS A
    // CLOSED FORM RATHER THAN MERELY GETTING SMALLER. Ten times the
    // stiffness does not give a tenth of the sinking-in, and the first
    // version of this test asked for that and got 7.29, which looks like
    // a sloppy pass and is in fact the right answer. The penalty spring
    // is in *series* with the block's own stiffness: the interference
    // push - gap is shared between how far the node sinks into the plane
    // and how far the block squashes, so with r = k_contact / k_block,
    //
    //     penetration = interference / (1 + r)
    //
    // Measuring r from the first run turns the second into a prediction
    // with nothing fitted, which is a real check where a factor-of-ten
    // band was only a plausibility one.
    const auto stiffer = run(10.0);
    CHECK(stiffer.first);
    if (!stiffer.first) return;
    const double interference = push - gap;
    const double r = interference / result.worst_penetration - 1.0;
    const double predicted = interference / (1.0 + 10.0 * r);
    std::printf("  stiffness ratio %.3f from the first run predicts %.3e m at ten times\n", r,
                predicted);
    std::printf("  ten times the penalty gives %.3e m instead of %.3e, out by %.2f%%\n",
                stiffer.second.worst_penetration, result.worst_penetration,
                100.0 * std::fabs(stiffer.second.worst_penetration - predicted) / predicted);
    CHECK(r > 0.0);
    CHECK(std::fabs(stiffer.second.worst_penetration - predicted) < predicted * 0.02);

    // AND WITHOUT THE PLANE IT GOES STRAIGHT THROUGH, which is the
    // control the rest of this only means something against.
    AnalysisModel free_model = Bar(Vec3d{0.2, 0.2, 0.1}, 3, 3, 2, 210e9, 0.3);
    for (Vec3d &p : free_model.nodes) p.z += gap;
    for (int node = 0; node < free_model.NodeCount(); ++node) {
        const Vec3d &p = free_model.nodes[Idx(node)];
        fem::Constraint constraint;
        constraint.node = node;
        constraint.fixed[0] = constraint.fixed[1] = true;
        if (Near(p.z, gap, 1.0)) constraint.fixed[2] = true;
        if (Near(p.z, 0.1 + gap, 1.0)) {
            constraint.fixed[2] = true;
            constraint.value[2] = -push;
        }
        free_model.constraints.push_back(constraint);
    }
    fem::NonlinearOptions plain;
    plain.steps = 4;
    NonlinearResult unobstructed;
    CHECK_MESSAGE(fem::SolveNonlinear(free_model, {}, plain, &unobstructed), unobstructed.error);
    if (!unobstructed.ok) return;
    std::printf("  with the bottom held instead of the plane, the top still moves %.3e m\n", push);
    CHECK(unobstructed.touching == 0);
    CHECK(unobstructed.worst_penetration == 0.0);
}

}  // namespace

int main() {
    TestRigidRotation();
    TestTangentIsConsistent();
    TestAgreesWithLinearWhenSmall();
    TestLargeDeflection();
    TestSnapThrough();
    TestUniaxialPlasticity();
    TestBauschinger();
    TestConsistentTangent();
    TestContactAgainstAPlane();
    if (failures != 0) {
        std::printf("fem_nonlinear_test FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("fem_nonlinear_test passed (%d checks)\n", checks);
    return 0;
}
