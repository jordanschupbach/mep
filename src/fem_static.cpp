#include "fem_static.h"

#include "fem_result.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fem {
namespace {

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

}  // namespace

std::vector<std::vector<double>> RigidBodyModes(const AnalysisModel &model, const System &system) {
    std::vector<std::vector<double>> out;
    for (int mode = 0; mode < 6; ++mode) {
        std::vector<double> full(Idx(model.NodeCount() * 3), 0.0);
        for (int node = 0; node < model.NodeCount(); ++node) {
            const cad::Vec3d &p = model.nodes[Idx(node)];
            cad::Vec3d motion{};
            if (mode < 3) {
                double component[3] = {0.0, 0.0, 0.0};
                component[mode] = 1.0;
                motion = cad::Vec3d{component[0], component[1], component[2]};
            } else {
                const cad::Vec3d about = mode == 3 ? cad::Vec3d{1, 0, 0}
                                                   : (mode == 4 ? cad::Vec3d{0, 1, 0}
                                                                : cad::Vec3d{0, 0, 1});
                motion = about.Cross(p);
            }
            full[Idx(node * 3 + 0)] = motion.x;
            full[Idx(node * 3 + 1)] = motion.y;
            full[Idx(node * 3 + 2)] = motion.z;
        }
        std::vector<double> reduced(system.dof_of_row.size(), 0.0);
        for (std::size_t row = 0; row < system.dof_of_row.size(); ++row) {
            if (system.dof_of_row[row] < 0) continue;
            reduced[row] = full[Idx(system.dof_of_row[row])];
        }
        out.push_back(reduced);
    }
    return out;
}

bool ThermalLoad(const AnalysisModel &model, std::vector<NodalLoad> *out, std::string *error) {
    out->clear();
    error->clear();
    if (model.node_temperature.empty()) return true;
    if (static_cast<int>(model.node_temperature.size()) != model.NodeCount()) {
        *error = "the temperature field does not have one value per node";
        return false;
    }
    std::vector<double> accumulated(Idx(model.NodeCount() * 3), 0.0);
    std::vector<QuadraturePoint> rule;
    std::vector<double> n;
    std::vector<double> dn;
    std::vector<double> dn_xyz;
    std::vector<double> b;
    std::vector<cad::Vec3d> corner;
    for (const BoundElement &element : model.elements) {
        if (element.material < 0 ||
            element.material >= static_cast<int>(model.materials.size())) {
            *error = "an element names a material the model does not have";
            return false;
        }
        const StudyMaterial &material = model.materials[Idx(element.material)];
        double d[6][6];
        if (!material.ConstitutiveMatrix(model.MeanTemperature(element.nodes), d, error)) {
            return false;
        }
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model.nodes[Idx(node)]);
        // A degree above the element's own, since the temperature field
        // varies across it and the strain it wants varies with it.
        if (!Quadrature(element.shape, ElementNodeCount(element.shape) > 8 ? 4 : 3, &rule, error)) {
            return false;
        }
        const int count = static_cast<int>(element.nodes.size());
        const int columns = count * 3;
        for (const QuadraturePoint &point : rule) {
            ShapeFunctions(element.shape, point.at, &n, &dn);
            const double determinant = ElementJacobian(element.shape, corner, dn, &dn_xyz);
            if (!(determinant > 0.0)) {
                *error = "an element is turned inside out where the thermal load was integrated";
                return false;
            }
            double local = 0.0;
            for (int a = 0; a < count; ++a) {
                local += n[Idx(a)] * model.node_temperature[Idx(element.nodes[Idx(a)])];
            }
            const double expansion = material.thermal_expansion.At(local);
            const double strain = expansion * (local - model.reference_temperature);
            // Volumetric only: a temperature change wants no shear.
            const double thermal[6] = {strain, strain, strain, 0.0, 0.0, 0.0};
            double stress[6] = {0, 0, 0, 0, 0, 0};
            for (int r = 0; r < 6; ++r) {
                for (int c = 0; c < 6; ++c) stress[r] += d[r][c] * thermal[c];
            }
            StrainDisplacement(element.shape, dn_xyz, &b);
            const double weight = determinant * point.weight;
            for (int i = 0; i < columns; ++i) {
                double sum = 0.0;
                for (int r = 0; r < 6; ++r) sum += b[Idx(r * columns + i)] * stress[r];
                accumulated[Idx(element.nodes[Idx(i / 3)] * 3 + i % 3)] += sum * weight;
            }
        }
    }
    for (int node = 0; node < model.NodeCount(); ++node) {
        const cad::Vec3d force{accumulated[Idx(node * 3 + 0)], accumulated[Idx(node * 3 + 1)],
                               accumulated[Idx(node * 3 + 2)]};
        if (force.LengthSquared() == 0.0) continue;
        out->push_back(NodalLoad{node, force});
    }
    return true;
}

bool ElementStressAt(const AnalysisModel &model, int element, const cad::Vec3d &reference,
                     const std::vector<double> &displacement, StressTensor *out,
                     std::string *error) {
    *out = StressTensor{};
    if (element < 0 || element >= model.ElementCount()) {
        *error = "no such element";
        return false;
    }
    const BoundElement &at = model.elements[Idx(element)];
    if (at.material < 0 || at.material >= static_cast<int>(model.materials.size())) {
        *error = "the element names a material the model does not have";
        return false;
    }
    double d[6][6];
    if (!model.materials[Idx(at.material)].ConstitutiveMatrix(model.temperature, d, error)) {
        return false;
    }
    std::vector<cad::Vec3d> corner;
    for (const int node : at.nodes) corner.push_back(model.nodes[Idx(node)]);
    std::vector<double> n;
    std::vector<double> dn;
    std::vector<double> dn_xyz;
    ShapeFunctions(at.shape, reference, &n, &dn);
    const double determinant = ElementJacobian(at.shape, corner, dn, &dn_xyz);
    if (!(determinant > 0.0)) {
        *error = "the element is turned inside out where the stress was asked for";
        return false;
    }
    std::vector<double> b;
    StrainDisplacement(at.shape, dn_xyz, &b);
    const int columns = static_cast<int>(at.nodes.size()) * 3;
    std::vector<double> local(Idx(columns), 0.0);
    for (std::size_t a = 0; a < at.nodes.size(); ++a) {
        for (int axis = 0; axis < 3; ++axis) {
            local[a * 3 + Idx(axis)] = displacement[Idx(at.nodes[a] * 3 + axis)];
        }
    }
    double strain[6] = {0, 0, 0, 0, 0, 0};
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < columns; ++c) strain[r] += b[Idx(r * columns + c)] * local[Idx(c)];
    }
    // THE THERMAL STRAIN IS TAKEN OUT BEFORE THE STRESS IS COMPUTED, and
    // forgetting to is the classic way a thermal-stress result comes out
    // wrong in a way that looks plausible. Stress is the *elastic* part of
    // the strain times the stiffness: a body free to expand has strain
    // everywhere and stress nowhere, and reading the total strain would
    // report the whole of the expansion as stress -- a large number, of the
    // right order, in the right places.
    if (!model.node_temperature.empty()) {
        double temperature = 0.0;
        for (std::size_t a = 0; a < at.nodes.size(); ++a) {
            temperature += n[a] * model.node_temperature[Idx(at.nodes[a])];
        }
        const double expansion =
            model.materials[Idx(at.material)].thermal_expansion.At(temperature);
        const double thermal = expansion * (temperature - model.reference_temperature);
        for (int r = 0; r < 3; ++r) strain[r] -= thermal;
    }
    for (int r = 0; r < 6; ++r) {
        double sum = 0.0;
        for (int c = 0; c < 6; ++c) sum += d[r][c] * strain[c];
        out->s[r] = sum;
    }
    return true;
}

bool SolveStatic(const AnalysisModel &model,
                 const std::vector<MultiPointConstraint> &constraints,
                 const StaticOptions &options, StaticResult *out) {
    *out = StaticResult{};
    System system;
    if (!BuildSystem(model, constraints, options.assembly, &system, &out->error)) return false;

    std::vector<double> reduced;
    if (options.solver == SolverKind::Direct) {
        num::SparseLDLT factor;
        // A Lagrange-multiplier system is indefinite and its multiplier
        // rows must be eliminated last, which a fill-reducing ordering
        // will not respect. The natural order is what the assembly built.
        const num::Ordering ordering =
            system.multipliers > 0 ? num::Ordering::Natural : options.ordering;
        if (!factor.Factorize(system.matrix, ordering)) {
            out->error = factor.Error();
            return false;
        }
        if (!factor.Solve(system.rhs, &reduced)) {
            out->error = "the factorization would not solve";
            return false;
        }
        out->solver_used = "direct LDL^T";
    } else {
        num::IterativeOptions iterative;
        iterative.preconditioner = num::Preconditioner::AlgebraicMultigrid;
        iterative.multigrid = options.multigrid;
        iterative.multigrid.block_size = 3;
        if (options.rigid_body_near_null_space && system.multipliers == 0) {
            iterative.multigrid.near_null_space = RigidBodyModes(model, system);
        }
        iterative.tolerance = options.iterative_tolerance;
        iterative.max_iterations = options.iterative_max_iterations;
        const num::IterativeResult result =
            num::ConjugateGradient(system.matrix, system.rhs, &reduced, iterative);
        out->iterations = result.iterations;
        out->solver_used = "conjugate gradients with algebraic multigrid";
        if (!result.message.empty()) out->warnings.push_back(result.message);
        if (!result.converged) {
            out->error = "the iterative solver did not converge: relative residual " +
                         std::to_string(result.relative_residual) + " after " +
                         std::to_string(result.iterations) + " iterations";
            return false;
        }
    }

    std::vector<double> full;
    system.Expand(reduced, &full);
    out->displacement.assign(model.nodes.size(), cad::Vec3d{});
    for (int node = 0; node < model.NodeCount(); ++node) {
        out->displacement[Idx(node)] = cad::Vec3d{full[Idx(node * 3 + 0)], full[Idx(node * 3 + 1)],
                                                  full[Idx(node * 3 + 2)]};
        const double magnitude = out->displacement[Idx(node)].Length();
        if (magnitude <= out->max_displacement) continue;
        out->max_displacement = magnitude;
        out->max_displacement_node = node;
    }

    // Stress, recovered by Part J.1 rather than read off at the nodes.
    //
    // The difference is not cosmetic: the quadrature points are where a
    // displacement element's stress is superconvergent, so a fit through
    // them read out at the nodes beats evaluating at the nodes directly,
    // and `StaticOptions::recovery` keeps the naive way available to
    // measure that by. The unaveraged per-element values and the
    // disagreement between them come along with it, which is what a
    // result viewer needs to show its own accuracy and what Part J.5's
    // estimator is computed from.
    if (!RecoverStress(model, full, options.recovery, &out->stress, &out->error)) return false;
    out->nodal_stress = out->stress.nodal;
    out->element_stress = out->stress.element_centre;
    for (int node = 0; node < model.NodeCount(); ++node) {
        if (out->stress.contributions[Idx(node)] == 0) continue;
        const double von_mises = out->nodal_stress[Idx(node)].VonMises();
        if (von_mises <= out->max_von_mises) continue;
        out->max_von_mises = von_mises;
        out->max_von_mises_node = node;
    }

    // Reactions, and the equilibrium check they make possible.
    SparsityPattern pattern;
    if (!pattern.Build(model, &out->error)) return false;
    if (!Reactions(model, pattern, full, &out->reaction, &out->error)) return false;
    // MEASURED AGAINST HOW MUCH FORCE IS IN THE PROBLEM, NOT AGAINST HOW
    // MUCH IT SUMS TO. The sum of the applied forces is the obvious scale
    // and it is wrong for any self-equilibrated load: a body force that
    // pushes one way over half the model and the other way over the rest
    // sums to nothing, so dividing by it turns a residual at round-off
    // into a number in the hundreds. The manufactured solution in the
    // tests is exactly that case, and this reported a broken solve for
    // every mesh of a solve that was correct. The scale is the total
    // magnitude of the forces involved -- applied and reacted -- which is
    // what "how big are the numbers being cancelled" means.
    cad::Vec3d applied{};
    double magnitude = 0.0;
    for (const NodalLoad &load : model.loads) {
        applied = applied + load.force;
        magnitude += load.force.Length();
    }
    std::vector<bool> held(model.nodes.size(), false);
    for (const Constraint &constraint : model.constraints) {
        if (constraint.node < 0 || constraint.node >= model.NodeCount()) continue;
        if (!constraint.fixed[0] && !constraint.fixed[1] && !constraint.fixed[2]) continue;
        held[Idx(constraint.node)] = true;
    }
    cad::Vec3d total{};
    for (int node = 0; node < model.NodeCount(); ++node) {
        if (!held[Idx(node)]) continue;
        total = total + out->reaction[Idx(node)];
        magnitude += out->reaction[Idx(node)].Length();
    }
    out->equilibrium_residual = (total + applied).Length() / std::max(magnitude, 1e-300);

    // Strain energy, as the work the loads did. For a linear problem this
    // equals twice the stored energy, and it is the scalar two solutions
    // of the same problem are most usefully compared on: it is a global
    // quantity, so a local meshing difference moves it far less than it
    // moves a peak stress.
    for (const NodalLoad &load : model.loads) {
        out->strain_energy += 0.5 * load.force.Dot(out->displacement[Idx(load.node)]);
    }
    out->ok = true;
    return true;
}

}  // namespace fem
