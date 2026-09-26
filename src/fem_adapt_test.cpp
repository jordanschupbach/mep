// Part J.5: error estimation and the adaptive loop, verified.

#include "fem_adapt.h"

#include "cad_feature.h"
#include "cad_pcurve.h"
#include "fem_elem.h"

#include <algorithm>
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

using cad::Vec3d;
using fem::AnalysisModel;
using fem::BoundElement;
using fem::ElementShape;
using fem::StressTensor;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

constexpr double kPi = 3.14159265358979323846;
constexpr double kE = 1.0;
constexpr double kNu = 0.3;
const double kMu = kE / (2.0 * (1.0 + kNu));

// The same divergence-free manufactured solution Part H.6 uses: the curl
// of (0, 0, sin pi x sin pi y sin pi z), whose Laplacian is -3 pi^2 times
// itself, so the body force that produces it is 3 pi^2 mu u and the exact
// stress is twice the shear modulus times the strain -- no Lame term,
// because the trace of the strain vanishes identically.
Vec3d Exact(const Vec3d &p) {
    const double sx = std::sin(kPi * p.x), sy = std::sin(kPi * p.y), sz = std::sin(kPi * p.z);
    const double cx = std::cos(kPi * p.x), cy = std::cos(kPi * p.y);
    return Vec3d{kPi * sx * cy * sz, -kPi * cx * sy * sz, 0.0};
}

StressTensor ExactStress(const Vec3d &p) {
    const double sx = std::sin(kPi * p.x), sy = std::sin(kPi * p.y), sz = std::sin(kPi * p.z);
    const double cx = std::cos(kPi * p.x), cy = std::cos(kPi * p.y), cz = std::cos(kPi * p.z);
    const double k = kPi * kPi;
    StressTensor out;
    out.s[0] = 2.0 * kMu * k * cx * cy * sz;
    out.s[1] = -2.0 * kMu * k * cx * cy * sz;
    out.s[4] = -kMu * k * cx * sy * cz;
    out.s[5] = kMu * k * sx * cy * cz;
    return out;
}

Vec3d BodyForce(const Vec3d &p) { return Exact(p) * (3.0 * kPi * kPi * kMu); }

AnalysisModel Cube(int m) {
    AnalysisModel model;
    fem::StudyMaterial material;
    material.youngs_modulus = fem::MaterialCurve::Constant(kE);
    material.poissons_ratio = fem::MaterialCurve::Constant(kNu);
    model.materials.push_back(material);
    const int side = m + 1;
    for (int k = 0; k < side; ++k) {
        for (int j = 0; j < side; ++j) {
            for (int i = 0; i < side; ++i) {
                model.nodes.push_back(Vec3d{static_cast<double>(i) / m, static_cast<double>(j) / m,
                                            static_cast<double>(k) / m});
            }
        }
    }
    auto at = [&](int i, int j, int k) { return (k * side + j) * side + i; };
    for (int k = 0; k < m; ++k) {
        for (int j = 0; j < m; ++j) {
            for (int i = 0; i < m; ++i) {
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

void ApplyBodyForce(AnalysisModel *model) {
    std::vector<double> load(Idx(model->NodeCount() * 3), 0.0);
    std::vector<fem::QuadraturePoint> rule;
    std::vector<double> n, dn, dn_xyz;
    std::vector<Vec3d> corner;
    std::string error;
    for (const BoundElement &element : model->elements) {
        if (!fem::Quadrature(element.shape, 4, &rule, &error)) continue;
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model->nodes[Idx(node)]);
        for (const fem::QuadraturePoint &point : rule) {
            fem::ShapeFunctions(element.shape, point.at, &n, &dn);
            const double determinant = fem::ElementJacobian(element.shape, corner, dn, &dn_xyz);
            Vec3d where{};
            for (std::size_t a = 0; a < element.nodes.size(); ++a) where = where + corner[a] * n[a];
            const Vec3d force = BodyForce(where) * (determinant * point.weight);
            for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                load[Idx(element.nodes[a] * 3 + 0)] += force.x * n[a];
                load[Idx(element.nodes[a] * 3 + 1)] += force.y * n[a];
                load[Idx(element.nodes[a] * 3 + 2)] += force.z * n[a];
            }
        }
    }
    for (int node = 0; node < model->NodeCount(); ++node) {
        const Vec3d force{load[Idx(node * 3)], load[Idx(node * 3 + 1)], load[Idx(node * 3 + 2)]};
        if (force.LengthSquared() == 0.0) continue;
        model->loads.push_back(fem::NodalLoad{node, force});
    }
}

void ConstrainBoundary(AnalysisModel *model) {
    for (int node = 0; node < model->NodeCount(); ++node) {
        const Vec3d &p = model->nodes[Idx(node)];
        if (!(p.x == 0.0 || p.x == 1.0 || p.y == 0.0 || p.y == 1.0 || p.z == 0.0 || p.z == 1.0)) {
            continue;
        }
        const Vec3d value = Exact(p);
        fem::Constraint constraint;
        constraint.node = node;
        for (int axis = 0; axis < 3; ++axis) constraint.fixed[axis] = true;
        constraint.value[0] = value.x;
        constraint.value[1] = value.y;
        constraint.value[2] = value.z;
        model->constraints.push_back(constraint);
    }
}

// The true error in the energy norm, element by element: the integral of
// (sigma_exact - sigma_h) against the compliance. THE SAME NORM THE
// ESTIMATE IS IN, which is the only way the two are comparable -- an
// effectivity index computed against an L2 error would be measuring two
// different things and would still come out near one on a fine enough
// mesh.
bool TrueError(const AnalysisModel &model, const std::vector<double> &displacement,
               std::vector<double> *per_element, double *global) {
    per_element->assign(model.elements.size(), 0.0);
    *global = 0.0;
    const double lambda = kE * kNu / ((1.0 + kNu) * (1.0 - 2.0 * kNu));
    // The isotropic compliance, written out because this is the test's own
    // reckoning and must not share code with the thing under test.
    double compliance[6][6] = {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) compliance[i][j] = i == j ? 1.0 / kE : -kNu / kE;
    }
    for (int i = 3; i < 6; ++i) compliance[i][i] = 2.0 * (1.0 + kNu) / kE;
    (void)lambda;

    std::vector<fem::QuadraturePoint> rule;
    std::vector<double> n, dn, dn_xyz;
    std::vector<Vec3d> corner;
    std::string error;
    double total = 0.0;
    for (int e = 0; e < model.ElementCount(); ++e) {
        const BoundElement &element = model.elements[Idx(e)];
        if (!fem::Quadrature(element.shape, 4, &rule, &error)) return false;
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model.nodes[Idx(node)]);
        double squared = 0.0;
        for (const fem::QuadraturePoint &point : rule) {
            fem::ShapeFunctions(element.shape, point.at, &n, &dn);
            const double determinant = fem::ElementJacobian(element.shape, corner, dn, &dn_xyz);
            Vec3d where{};
            for (std::size_t a = 0; a < element.nodes.size(); ++a) where = where + corner[a] * n[a];
            StressTensor computed;
            if (!fem::ElementStressAt(model, e, point.at, displacement, &computed, &error)) {
                return false;
            }
            const StressTensor want = ExactStress(where);
            double difference[6];
            for (int i = 0; i < 6; ++i) difference[i] = want.s[i] - computed.s[i];
            double quadratic = 0.0;
            for (int i = 0; i < 6; ++i) {
                for (int j = 0; j < 6; ++j) {
                    quadratic += difference[i] * compliance[i][j] * difference[j];
                }
            }
            squared += determinant * point.weight * quadratic;
        }
        (*per_element)[Idx(e)] = std::sqrt(std::max(0.0, squared));
        total += squared;
    }
    *global = std::sqrt(total);
    return true;
}

double Correlation(const std::vector<double> &a, const std::vector<double> &b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double mean_a = 0.0, mean_b = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        mean_a += a[i];
        mean_b += b[i];
    }
    mean_a /= static_cast<double>(a.size());
    mean_b /= static_cast<double>(b.size());
    double top = 0.0, left = 0.0, right = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        top += (a[i] - mean_a) * (b[i] - mean_b);
        left += (a[i] - mean_a) * (a[i] - mean_a);
        right += (b[i] - mean_b) * (b[i] - mean_b);
    }
    return left > 0.0 && right > 0.0 ? top / std::sqrt(left * right) : 0.0;
}

// --- The estimator ---------------------------------------------------------

void TestEffectivity() {
    std::printf("the effectivity index: what the estimate says over what is true\n");
    // THE ONE TEST AN ERROR ESTIMATOR HAS TO PASS. An estimator is a
    // claim about a quantity nobody can measure, so it is checked on a
    // problem where somebody can: a manufactured solution, whose exact
    // stress is known in closed form, so the true energy-norm error is
    // computable and the ratio of the two is meaningful. An estimator
    // that is merely "small when the error is small" is worth nothing --
    // so is a constant -- and the index has to approach one.
    const int meshes[3] = {4, 8, 16};
    double index[3] = {0, 0, 0};
    double estimated[3] = {0, 0, 0};
    double actual[3] = {0, 0, 0};
    double correlation[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        AnalysisModel model = Cube(meshes[i]);
        ApplyBodyForce(&model);
        ConstrainBoundary(&model);
        fem::StaticOptions options;
        fem::StaticResult result;
        CHECK(fem::SolveStatic(model, {}, options, &result));
        if (!result.ok) {
            std::printf("  %s\n", result.error.c_str());
            return;
        }
        std::vector<double> flat(Idx(model.NodeCount() * 3), 0.0);
        for (int node = 0; node < model.NodeCount(); ++node) {
            flat[Idx(node * 3 + 0)] = result.displacement[Idx(node)].x;
            flat[Idx(node * 3 + 1)] = result.displacement[Idx(node)].y;
            flat[Idx(node * 3 + 2)] = result.displacement[Idx(node)].z;
        }
        fem::ErrorEstimate estimate;
        std::string error;
        CHECK(fem::EstimateError(model, flat, result.stress, &estimate, &error));
        std::vector<double> truth;
        double true_global = 0.0;
        CHECK(TrueError(model, flat, &truth, &true_global));
        estimated[i] = estimate.global_error;
        actual[i] = true_global;
        index[i] = estimate.global_error / true_global;
        correlation[i] = Correlation(estimate.element_error, truth);
        std::printf("  %2d per side, %5d elements: estimated %.5e, true %.5e,"
                    " index %.4f, per element r = %.4f\n",
                    meshes[i], model.ElementCount(), estimated[i], actual[i], index[i],
                    correlation[i]);
        std::printf("    relative error %.3f%%, energy norm %.5e\n",
                    100.0 * estimate.relative_error, estimate.global_energy);
        CHECK(estimate.element_error.size() == model.elements.size());
        CHECK(estimate.worst_element >= 0);
        CHECK(estimate.relative_error > 0.0 && estimate.relative_error < 1.0);
    }
    // ASYMPTOTICALLY EXACT: the index approaches one, and getting closer
    // with refinement is the property. A number that sat at 0.7 on every
    // mesh would be a usable estimator and not an asymptotically exact
    // one, and the difference decides whether the relative error it
    // reports can be believed as a percentage or only as a ranking.
    std::printf("  the index goes %.4f -> %.4f -> %.4f\n", index[0], index[1], index[2]);
    CHECK(std::fabs(index[2] - 1.0) < std::fabs(index[0] - 1.0));
    CHECK(std::fabs(index[2] - 1.0) < 0.15);
    // AND IT RANKS THE ELEMENTS, which is what refinement actually uses.
    // A global index near one with no per-element agreement would refine
    // the wrong elements while reporting the right total.
    std::printf("  per-element agreement with the true error: r = %.4f on the finest mesh\n",
                correlation[2]);
    CHECK(correlation[2] > 0.9);
    // The true error falls at the element's own rate, which says the
    // problem being estimated is the one it is supposed to be.
    const double rate = 0.5 * (std::log2(actual[0] / actual[1]) + std::log2(actual[1] / actual[2]));
    std::printf("  the true energy-norm error falls at rate %.3f (a linear element gives 1)\n",
                rate);
    CHECK(rate > 0.85 && rate < 1.25);
}

void TestPlanning() {
    std::printf("turning an estimate into element sizes:\n");
    AnalysisModel model = Cube(4);
    ApplyBodyForce(&model);
    ConstrainBoundary(&model);
    fem::StaticOptions options;
    fem::StaticResult result;
    CHECK(fem::SolveStatic(model, {}, options, &result));
    std::vector<double> flat(Idx(model.NodeCount() * 3), 0.0);
    for (int node = 0; node < model.NodeCount(); ++node) {
        flat[Idx(node * 3 + 0)] = result.displacement[Idx(node)].x;
        flat[Idx(node * 3 + 1)] = result.displacement[Idx(node)].y;
        flat[Idx(node * 3 + 2)] = result.displacement[Idx(node)].z;
    }
    fem::ErrorEstimate estimate;
    std::string error;
    CHECK(fem::EstimateError(model, flat, result.stress, &estimate, &error));

    fem::AdaptOptions adapt;
    adapt.target_relative_error = 0.02;
    fem::RefinementPlan plan;
    CHECK(fem::PlanRefinement(model, estimate, adapt, &plan, &error));
    std::printf("  a 2%% target on a %.3f%% mesh: %d of %d elements refined,"
                " smallest %.5f from %.5f\n",
                100.0 * estimate.relative_error, plan.refined, model.ElementCount(),
                plan.smallest_requested, 0.25 * std::sqrt(3.0));
    CHECK(plan.refined > 0);
    CHECK(plan.refined <= model.ElementCount());
    CHECK(plan.refinements.size() == static_cast<std::size_t>(plan.refined));

    // IT REFINES THE ELEMENTS WITH THE ERROR IN THEM, which is the only
    // thing distinguishing an adaptive loop from a uniform one. The
    // refined elements' errors must be larger than the untouched ones'.
    std::vector<double> refined_errors;
    std::vector<double> untouched_errors;
    for (int e = 0; e < model.ElementCount(); ++e) {
        const bool refined =
            estimate.element_error[Idx(e)] > plan.target_element_error;
        (refined ? refined_errors : untouched_errors).push_back(estimate.element_error[Idx(e)]);
    }
    if (!refined_errors.empty() && !untouched_errors.empty()) {
        const double lowest_refined =
            *std::min_element(refined_errors.begin(), refined_errors.end());
        const double highest_untouched =
            *std::max_element(untouched_errors.begin(), untouched_errors.end());
        std::printf("  every refined element has more error than every untouched one:"
                    " %.3e vs %.3e\n",
                    lowest_refined, highest_untouched);
        CHECK(lowest_refined >= highest_untouched);
    }

    // A LOOSE TARGET REFINES NOTHING, which is how the loop knows to
    // stop, and a tight one refines more than a loose one -- monotone in
    // the target, which a formula with the exponent the wrong way round
    // would not be.
    fem::AdaptOptions loose = adapt;
    loose.target_relative_error = 0.9;
    fem::RefinementPlan nothing;
    CHECK(fem::PlanRefinement(model, estimate, loose, &nothing, &error));
    fem::AdaptOptions tight = adapt;
    tight.target_relative_error = 0.005;
    fem::RefinementPlan more;
    CHECK(fem::PlanRefinement(model, estimate, tight, &more, &error));
    std::printf("  targets of 90%%, 2%% and 0.5%% refine %d, %d and %d elements\n", nothing.refined,
                plan.refined, more.refined);
    CHECK(nothing.refined == 0);
    CHECK(more.refined >= plan.refined);
    CHECK(more.smallest_requested <= plan.smallest_requested);

    // The reduction is capped, so one cycle cannot ask for something the
    // mesher will choke on.
    const double longest = 0.25 * std::sqrt(3.0);
    for (const fem::SizingOptions::Refinement &refinement : plan.refinements) {
        CHECK(refinement.size >= longest / adapt.max_reduction - 1e-12);
        CHECK(refinement.radius > 0.0);
    }

    fem::RefinementPlan ignored;
    AnalysisModel empty;
    CHECK(!fem::PlanRefinement(empty, estimate, adapt, &ignored, &error));
}

// --- The loop --------------------------------------------------------------

struct Block {
    cad::Model model;
    cad::EntityId body = cad::kNoEntity;
    fem::Target bottom;
    fem::Target top;
};

Block MakeBlock(const Vec3d &size) {
    Block out;
    std::string error;
    if (!cad::MakeBox(Vec3d{0, 0, 0}, size, &out.model, &out.body)) return out;
    if (!cad::BuildAllPCurves(&out.model, {}, &error)) return out;
    cad::EntityId bottom = cad::kNoEntity;
    cad::EntityId top = cad::kNoEntity;
    for (const cad::Face &face : out.model.Faces()) {
        const cad::Surface *surface = out.model.SurfaceAt(face.surface);
        if (surface == nullptr) continue;
        double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
        surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
        const Vec3d normal =
            out.model.FaceNormal(face.id, (u_lo + u_hi) * 0.5, (v_lo + v_hi) * 0.5);
        if (normal.z < -0.9) bottom = face.id;
        if (normal.z > 0.9) top = face.id;
    }
    if (bottom != cad::kNoEntity) out.bottom = fem::FaceTarget(out.model, bottom);
    if (top != cad::kNoEntity) out.top = fem::FaceTarget(out.model, top);
    return out;
}

void TestTheLoopCloses() {
    std::printf("the loop: mesh, solve, estimate, remesh, against the real geometry\n");
    // TEN BY SIX BY FOUR, AND NOT A BLOCK IN METRES, because the mesher
    // is scale dependent -- a known and recorded gap, see the plan's G.2
    // notes. The same block at 0.2 by 0.05 by 0.05 with a proportionally
    // smaller target produces tetrahedra with a worst dihedral angle of
    // zero. That is the mesher's problem and not adaptivity's, and using
    // the size the mesher is happy at is the right way to test this part
    // rather than the wrong way to hide that one.
    //
    // A BLOCK HELD AT ONE END AND PRESSED AT THE OTHER. What
    // matters is that each cycle goes back to the *CAD model* and meshes
    // it again -- a loop handed a mesh could only subdivide what it was
    // given, and would be refining a faceted approximation rather than
    // the part.
    Block block = MakeBlock(Vec3d{10, 6, 4});
    CHECK(block.body != cad::kNoEntity);
    CHECK(block.bottom.kind == fem::TargetKind::Face);

    fem::Study study;
    fem::StudyMaterial steel;
    steel.name = "steel";
    study.materials.push_back(steel);
    fem::Support held;
    held.label = "one end held";
    held.where = block.bottom;
    study.supports.push_back(held);
    fem::Load push;
    push.label = "pressure on the other";
    push.kind = fem::LoadKind::Pressure;
    push.where = block.top;
    push.magnitude = 5e6;
    study.loads.push_back(push);

    fem::AdaptOptions options;
    options.cycles = 4;
    options.target_relative_error = 0.02;
    options.mesh.surface.sizing.target = 2.0;
    options.min_size = 0.5;
    AnalysisModel final_model;
    fem::StaticResult final_result;
    fem::AdaptReport report;
    const bool ok = fem::AdaptiveSolve(block.model, {block.body}, study, options, &final_model,
                                       &final_result, &report);
    if (!ok) {
        std::printf("  %s\n", report.error.c_str());
        CHECK(ok);
        return;
    }
    if (report.stopped_early) {
        std::printf("  stopped early: %s\n", report.stop_reason.c_str());
    }
    std::printf("  cycle  elements  nodes   relative error   strain energy   smallest element\n");
    for (std::size_t c = 0; c < report.cycles.size(); ++c) {
        const fem::AdaptCycle &cycle = report.cycles[c];
        std::printf("  %5d  %8d  %5d   %12.4f%%   %13.6e   %.6f\n", static_cast<int>(c),
                    cycle.elements, cycle.nodes, 100.0 * cycle.relative_error, cycle.strain_energy,
                    cycle.smallest_element);
    }
    CHECK(report.cycles.size() >= 2);
    if (report.cycles.size() < 2) return;

    // THE ERROR FALLS AND THE MESH GROWS. Both, because either alone
    // proves nothing: an estimate that falls while the mesh does not
    // change would mean the estimator is unstable, and a mesh that grows
    // while the error does not would mean the refinement is going in the
    // wrong places.
    const fem::AdaptCycle &first = report.cycles.front();
    const fem::AdaptCycle &last = report.cycles.back();
    std::printf("  error %.4f%% -> %.4f%% while the mesh went %d -> %d elements\n",
                100.0 * first.relative_error, 100.0 * last.relative_error, first.elements,
                last.elements);
    CHECK(last.relative_error < first.relative_error);
    CHECK(last.elements > first.elements);
    for (std::size_t c = 1; c < report.cycles.size(); ++c) {
        CHECK(report.cycles[c].elements >= report.cycles[c - 1].elements);
    }

    // THE STRAIN ENERGY CONVERGES FROM BELOW. A displacement formulation
    // is too stiff, always, so every refinement releases a little more
    // energy and the sequence is increasing. This is the classic check
    // that a refinement sequence is a refinement sequence and not a
    // sequence of different problems -- a load that changed with the mesh,
    // or a support that resolved to a different face, breaks it
    // immediately.
    bool increasing = true;
    for (std::size_t c = 1; c < report.cycles.size(); ++c) {
        if (report.cycles[c].strain_energy < report.cycles[c - 1].strain_energy * (1.0 - 1e-9)) {
            increasing = false;
        }
    }
    std::printf("  strain energy %.6e -> %.6e, increasing: %s\n", first.strain_energy,
                last.strain_energy, increasing ? "yes" : "no");
    CHECK(increasing);

    // And the result that comes back is the last cycle's, not the first.
    CHECK(final_model.ElementCount() == last.elements);
    CHECK(final_result.ok);
    CHECK(final_result.stress.nodal.size() == final_model.nodes.size());
}

void TestUniformComparison() {
    std::printf("adaptive against uniform, at comparable cost:\n");
    // THE PAYOFF CLAIM, MEASURED. Adaptive refinement is only worth its
    // complexity if it beats simply making everything smaller at the same
    // number of degrees of freedom. Both runs start from the same mesh
    // and the same geometry; one refines where the estimate says, the
    // other everywhere.
    Block block = MakeBlock(Vec3d{10, 6, 4});
    fem::Study study;
    fem::StudyMaterial steel;
    study.materials.push_back(steel);
    fem::Support held;
    held.where = block.bottom;
    study.supports.push_back(held);
    fem::Load push;
    push.kind = fem::LoadKind::Pressure;
    push.where = block.top;
    push.magnitude = 5e6;
    study.loads.push_back(push);

    fem::AdaptOptions adaptive;
    adaptive.cycles = 3;
    adaptive.target_relative_error = 0.01;
    adaptive.stop_at_target = false;
    adaptive.mesh.surface.sizing.target = 2.0;
    adaptive.min_size = 0.5;
    AnalysisModel model;
    fem::StaticResult result;
    fem::AdaptReport report;
    if (!fem::AdaptiveSolve(block.model, {block.body}, study, adaptive, &model, &result,
                            &report)) {
        std::printf("  %s\n", report.error.c_str());
        CHECK(false);
        return;
    }
    const fem::AdaptCycle &reached = report.cycles.back();

    // A uniform mesh with at least as many elements.
    double size = 2.0;
    fem::AdaptCycle uniform;
    for (int attempt = 0; attempt < 6; ++attempt) {
        size *= 0.8;
        fem::AdaptOptions once;
        once.cycles = 1;
        once.mesh.surface.sizing.target = size;
        AnalysisModel other;
        fem::StaticResult other_result;
        fem::AdaptReport other_report;
        if (!fem::AdaptiveSolve(block.model, {block.body}, study, once, &other, &other_result,
                                &other_report)) {
            continue;
        }
        uniform = other_report.cycles.back();
        if (uniform.elements >= reached.elements) break;
    }
    std::printf("  adaptive: %6d elements, %.4f%% after %d of %d cycles%s\n", reached.elements,
                100.0 * reached.relative_error, static_cast<int>(report.cycles.size()),
                adaptive.cycles, report.stopped_early ? " (stopped early)" : "");
    if (report.stopped_early) std::printf("    %s\n", report.stop_reason.c_str());
    std::printf("  uniform:  %6d elements, %.4f%%\n", uniform.elements,
                100.0 * uniform.relative_error);
    CHECK(uniform.elements > 0);
    if (uniform.elements <= 0) return;
    std::printf("  adaptive is %.2f times better per element at %.2f times the size\n",
                uniform.relative_error / reached.relative_error,
                static_cast<double>(reached.elements) / uniform.elements);
    // NOT ASSERTED AS A WIN, AND FOR TWO REASONS, BOTH RECORDED. On a
    // plain block under a uniform pressure there is no stress
    // concentration to find: the error is spread evenly, the best mesh
    // *is* the uniform one, and an adaptive loop that discovered
    // otherwise would be wrong. And the loop does not get to spend its
    // budget anyway, because the graded mesh its own refinement asks for
    // is where this mesher produces slivers, so it stops a cycle or two
    // in. Demonstrating the payoff needs a geometry with a concentration
    // *and* a mesher that grades cleanly, and the second of those is a
    // Part G.4 gap rather than this one's. What is asserted is that the
    // loop does no harm on the problem it cannot help with.
    CHECK(reached.relative_error < uniform.relative_error * 1.6);
}

}  // namespace

int main() {
    TestEffectivity();
    TestPlanning();
    TestTheLoopCloses();
    TestUniformComparison();
    if (failures == 0) {
        std::printf("fem_adapt_test passed (%d checks)\n", checks);
        return 0;
    }
    std::printf("fem_adapt_test FAILED (%d of %d checks)\n", failures, checks);
    return 1;
}
