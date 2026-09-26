#include "fem_thermal.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fem {
namespace {

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// Stefan-Boltzmann, W/(m^2 K^4).
constexpr double kStefanBoltzmann = 5.670374419e-8;

// The area element of a facet embedded in three dimensions, and its
// normal: the cross product of the two parameter tangents.
cad::Vec3d FacetNormal(ElementShape shape, const std::vector<cad::Vec3d> &nodes,
                       const std::vector<double> &dn) {
    cad::Vec3d along_u{};
    cad::Vec3d along_v{};
    const int count = ElementNodeCount(shape);
    for (int a = 0; a < count; ++a) {
        along_u = along_u + nodes[Idx(a)] * dn[Idx(a * 3 + 0)];
        along_v = along_v + nodes[Idx(a)] * dn[Idx(a * 3 + 1)];
    }
    return along_u.Cross(along_v);
}


// The steady residual and its tangent at one temperature field.
//
// FACTORED OUT BECAUSE BOTH SOLVERS NEED EXACTLY THIS. The steady solve
// drives the residual to zero; the transient one balances it against the
// heat capacity. Two copies of the assembly would be two places for a
// boundary condition to be integrated differently, and the difference
// would show up as a transient that settles to the wrong steady state --
// which is the hardest kind of disagreement to find, because each solver
// is self-consistent.
//
// `residual` is S(T): everything supplied minus everything conducted away.
// `triplets` is -dS/dT, which is positive definite and symmetric, and is
// the matrix a Newton step solves with.
struct Assembled {
    std::vector<double> residual;
    std::vector<num::Triplet> triplets;
    // The magnitude of every applied heat term, as a scale to measure a
    // residual against.
    double power = 0.0;
    // The same terms *signed*: the net power entering the body through
    // its surfaces and its sources. Kept separately from `power` because
    // a conservation check needs the sum and a tolerance needs the
    // magnitude, and one cannot be recovered from the other.
    double inflow = 0.0;
    bool ok = true;
    std::string error;
};

Assembled AssembleSteady(const ThermalModel &model, const std::vector<double> &temperature) {
    Assembled out;
    const int n = model.NodeCount();
    out.residual.assign(Idx(n), 0.0);
    std::vector<QuadraturePoint> rule;
    std::vector<double> shape;
    std::vector<double> dn;
    std::vector<double> dn_xyz;
    std::vector<cad::Vec3d> corner;

    for (int e = 0; e < model.ElementCount(); ++e) {
        const BoundElement &element = model.elements[Idx(e)];
        if (element.material < 0 || element.material >= static_cast<int>(model.materials.size())) {
            out.ok = false;
            out.error = "an element names a material the model does not have";
            return out;
        }
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model.nodes[Idx(node)]);
        if (!Quadrature(element.shape, 0, &rule, &out.error)) {
            out.ok = false;
            return out;
        }
        const int count = static_cast<int>(element.nodes.size());
        const double heat = e < static_cast<int>(model.source.size()) ? model.source[Idx(e)] : 0.0;
        for (const QuadraturePoint &point : rule) {
            ShapeFunctions(element.shape, point.at, &shape, &dn);
            const double determinant = ElementJacobian(element.shape, corner, dn, &dn_xyz);
            if (!(determinant > 0.0)) {
                out.ok = false;
                out.error = "element " + std::to_string(e) + " is turned inside out";
                return out;
            }
            double local = 0.0;
            cad::Vec3d gradient{};
            for (int a = 0; a < count; ++a) {
                const double t = temperature[Idx(element.nodes[Idx(a)])];
                local += shape[Idx(a)] * t;
                gradient = gradient + cad::Vec3d{dn_xyz[Idx(a * 3 + 0)], dn_xyz[Idx(a * 3 + 1)],
                                                 dn_xyz[Idx(a * 3 + 2)]} * t;
            }
            // THE CONSISTENT TANGENT FOR A TEMPERATURE-DEPENDENT
            // CONDUCTIVITY IS NOT SYMMETRIC, AND THAT IS WHY IT IS NOT
            // HERE. Differentiating properly adds
            // `dk/dT * N_b * (grad N_a . grad T)`, which carries one index
            // on a shape function and the other on a gradient and so is
            // not the same either way round. It was written, and the solve
            // diverged -- 6400 K on a slab running from 800 to 300 --
            // because SparseLDLT factors a symmetric matrix and was handed
            // something that was not one. Fixing it means the
            // non-symmetric solver path, and that is recorded rather than
            // bodged. What is left is a fixed-point iteration on the
            // conductivity: it converges, linearly. Radiation and
            // convection keep their consistent tangents, which are
            // symmetric, and converge quadratically.
            const double k = model.materials[Idx(element.material)].conductivity.At(local);
            const double weight = determinant * point.weight;
            for (int a = 0; a < count; ++a) {
                const cad::Vec3d ga{dn_xyz[Idx(a * 3 + 0)], dn_xyz[Idx(a * 3 + 1)],
                                    dn_xyz[Idx(a * 3 + 2)]};
                out.residual[Idx(element.nodes[Idx(a)])] +=
                    (heat * shape[Idx(a)] - k * ga.Dot(gradient)) * weight;
                out.power += std::fabs(heat * shape[Idx(a)]) * weight;
                out.inflow += heat * shape[Idx(a)] * weight;
                for (int b = 0; b < count; ++b) {
                    const cad::Vec3d gb{dn_xyz[Idx(b * 3 + 0)], dn_xyz[Idx(b * 3 + 1)],
                                        dn_xyz[Idx(b * 3 + 2)]};
                    out.triplets.push_back(num::Triplet{element.nodes[Idx(a)],
                                                        element.nodes[Idx(b)],
                                                        k * ga.Dot(gb) * weight});
                }
            }
        }
    }

    for (const ThermalFacet &facet : model.facets) {
        corner.clear();
        for (const int node : facet.nodes) corner.push_back(model.nodes[Idx(node)]);
        if (!Quadrature(facet.shape, 4, &rule, &out.error)) {
            out.ok = false;
            return out;
        }
        const int count = static_cast<int>(facet.nodes.size());
        for (const QuadraturePoint &point : rule) {
            ShapeFunctions(facet.shape, point.at, &shape, &dn);
            const double area = FacetNormal(facet.shape, corner, dn).Length() * point.weight;
            if (!(area > 0.0)) continue;
            double local = 0.0;
            for (int a = 0; a < count; ++a) {
                local += shape[Idx(a)] * temperature[Idx(facet.nodes[Idx(a)])];
            }
            double flow = facet.flux;
            double tangent = 0.0;
            if (facet.convection != 0.0) {
                flow -= facet.convection * (local - facet.ambient);
                tangent += facet.convection;
            }
            if (facet.emissivity != 0.0) {
                const double sigma = facet.emissivity * kStefanBoltzmann;
                flow -= sigma * (local * local * local * local -
                                 facet.ambient * facet.ambient * facet.ambient * facet.ambient);
                tangent += 4.0 * sigma * local * local * local;
            }
            for (int a = 0; a < count; ++a) {
                out.residual[Idx(facet.nodes[Idx(a)])] += flow * shape[Idx(a)] * area;
                out.power += std::fabs(flow * shape[Idx(a)]) * area;
                out.inflow += flow * shape[Idx(a)] * area;
                if (tangent == 0.0) continue;
                for (int b = 0; b < count; ++b) {
                    out.triplets.push_back(num::Triplet{facet.nodes[Idx(a)], facet.nodes[Idx(b)],
                                                        tangent * shape[Idx(a)] * shape[Idx(b)] *
                                                            area});
                }
            }
        }
    }
    return out;
}

// The heat capacity matrix, consistent rather than lumped: the integral
// of rho * c * N N^T. Assembled once for a constant specific heat, which
// is what the transient solve asks for.
std::vector<num::Triplet> AssembleCapacity(const ThermalModel &model,
                                           const std::vector<double> &temperature,
                                           std::vector<double> *out_row_sums, bool *ok,
                                           std::string *error) {
    std::vector<num::Triplet> out;
    out_row_sums->assign(Idx(model.NodeCount()), 0.0);
    *ok = true;
    std::vector<QuadraturePoint> rule;
    std::vector<double> shape;
    std::vector<double> dn;
    std::vector<double> dn_xyz;
    std::vector<cad::Vec3d> corner;
    for (int e = 0; e < model.ElementCount(); ++e) {
        const BoundElement &element = model.elements[Idx(e)];
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model.nodes[Idx(node)]);
        // A degree above the element's own: N N^T is twice the shape
        // functions' order, where the conductivity matrix needs only twice
        // their derivatives'.
        if (!Quadrature(element.shape, ElementNodeCount(element.shape) > 8 ? 4 : 3, &rule, error)) {
            *ok = false;
            return out;
        }
        const int count = static_cast<int>(element.nodes.size());
        const ThermalMaterial &material = model.materials[Idx(element.material)];
        for (const QuadraturePoint &point : rule) {
            ShapeFunctions(element.shape, point.at, &shape, &dn);
            const double determinant = ElementJacobian(element.shape, corner, dn, &dn_xyz);
            double local = 0.0;
            for (int a = 0; a < count; ++a) {
                local += shape[Idx(a)] * temperature[Idx(element.nodes[Idx(a)])];
            }
            const double capacity = material.density * material.specific_heat.At(local);
            const double weight = determinant * point.weight * capacity;
            for (int a = 0; a < count; ++a) {
                (*out_row_sums)[Idx(element.nodes[Idx(a)])] += shape[Idx(a)] * weight;
                for (int b = 0; b < count; ++b) {
                    out.push_back(num::Triplet{element.nodes[Idx(a)], element.nodes[Idx(b)],
                                               shape[Idx(a)] * shape[Idx(b)] * weight});
                }
            }
        }
    }
    return out;
}

}  // namespace

bool FaceConditions(const VolumeMesh &mesh, cad::EntityId face, std::vector<ThermalFacet> *out,
                    std::string *error) {
    out->clear();
    if (!mesh.boundary_quads.empty()) {
        for (const SurfaceQuad &quad : mesh.boundary_quads) {
            if (quad.face != face) continue;
            ThermalFacet facet;
            facet.shape = ElementShape::Quad4;
            facet.nodes = {quad.a, quad.b, quad.c, quad.d};
            out->push_back(facet);
        }
    } else if (!mesh.boundary6.empty() && mesh.boundary6.size() == mesh.boundary.size()) {
        for (std::size_t i = 0; i < mesh.boundary.size(); ++i) {
            if (mesh.boundary[i].face != face) continue;
            ThermalFacet facet;
            facet.shape = ElementShape::Tri6;
            for (const int node : mesh.boundary6[i]) facet.nodes.push_back(node);
            out->push_back(facet);
        }
    } else {
        for (const SurfaceTriangle &t : mesh.boundary) {
            if (t.face != face) continue;
            ThermalFacet facet;
            facet.shape = ElementShape::Tri3;
            facet.nodes = {t.a, t.b, t.c};
            out->push_back(facet);
        }
    }
    if (out->empty()) {
        *error = "no part of the mesh's boundary lies on that face";
        return false;
    }
    return true;
}

bool SolveSteadyHeat(const ThermalModel &model, const ThermalOptions &options,
                     ThermalResult *out) {
    *out = ThermalResult{};
    const int n = model.NodeCount();
    if (n == 0 || model.elements.empty()) {
        out->error = "the model has no elements";
        return false;
    }
    if (model.materials.empty()) {
        out->error = "the model has no material";
        return false;
    }
    for (const ThermalMaterial &material : model.materials) {
        if (material.conductivity.IsValid(&out->error)) continue;
        return false;
    }

    // The sparsity pattern, from connectivity alone, once. One unknown per
    // node rather than three, and otherwise exactly Part H.3's argument.
    std::vector<std::set<int>> columns(Idx(n));
    for (const BoundElement &element : model.elements) {
        for (const int a : element.nodes) {
            if (a >= 0 && a < n) continue;
            out->error = "an element refers to a node that does not exist";
            return false;
        }
        for (const int a : element.nodes) {
            for (const int b : element.nodes) columns[Idx(a)].insert(b);
        }
    }
    // A facet couples its own nodes too, through the convection and
    // radiation terms, and those pairs need not already be in the pattern
    // -- a facet of a second-order element has nodes an element shares and
    // a facet of a first-order one does not, but saying so costs nothing.
    for (const ThermalFacet &facet : model.facets) {
        for (const int a : facet.nodes) {
            if (a < 0 || a >= n) {
                out->error = "a facet refers to a node that does not exist";
                return false;
            }
            for (const int b : facet.nodes) columns[Idx(a)].insert(b);
        }
    }
    for (int i = 0; i < n; ++i) columns[Idx(i)].insert(i);

    std::vector<bool> fixed(Idx(n), false);
    std::vector<double> prescribed(Idx(n), options.initial_temperature);
    for (const auto &entry : model.fixed) {
        if (entry.first < 0 || entry.first >= n) {
            out->error = "a fixed temperature names a node that does not exist";
            return false;
        }
        fixed[Idx(entry.first)] = true;
        prescribed[Idx(entry.first)] = entry.second;
    }
    std::vector<int> row_of(Idx(n), -1);
    std::vector<int> node_of;
    for (int i = 0; i < n; ++i) {
        if (fixed[Idx(i)]) continue;
        row_of[Idx(i)] = static_cast<int>(node_of.size());
        node_of.push_back(i);
    }
    if (node_of.empty()) {
        out->error = "every node's temperature is prescribed, so there is nothing to solve for";
        return false;
    }

    std::vector<double> temperature = prescribed;

    // NEWTON, BECAUSE TWO OF THE FOUR CONDITIONS ARE NONLINEAR. Each
    // sweep assembles the tangent and the residual at the current
    // temperature and solves for a correction. With no radiation and a
    // constant conductivity the residual is linear and the first
    // correction is exact, so the loop costs one extra assembly.
    double scale = 1.0;
    for (int sweep = 0; sweep < std::max(1, options.max_iterations); ++sweep) {
        const Assembled at = AssembleSteady(model, temperature);
        if (!at.ok) {
            out->error = at.error;
            return false;
        }
        if (sweep == 0) scale = std::max(at.power, 1e-300);
        double worst = 0.0;
        for (const int node : node_of) worst = std::max(worst, std::fabs(at.residual[Idx(node)]));
        out->residual = worst / scale;
        out->iterations = sweep;
        if (out->residual < options.tolerance) break;

        const int size = static_cast<int>(node_of.size());
        std::vector<num::Triplet> reduced;
        reduced.reserve(at.triplets.size());
        for (const num::Triplet &entry : at.triplets) {
            const int row = row_of[Idx(entry.row)];
            const int column = row_of[Idx(entry.col)];
            if (row < 0 || column < 0) continue;
            reduced.push_back(num::Triplet{row, column, entry.value});
        }
        std::vector<double> rhs(Idx(size), 0.0);
        for (int i = 0; i < size; ++i) rhs[Idx(i)] = at.residual[Idx(node_of[Idx(i)])];
        const num::SparseMatrix tangent = num::SparseMatrix::FromTriplets(size, size, reduced);
        std::vector<double> correction;
        if (options.solver == SolverKind::Direct) {
            num::SparseLDLT factor;
            if (!factor.Factorize(tangent, options.ordering)) {
                out->error = factor.Error();
                return false;
            }
            if (!factor.Solve(rhs, &correction)) {
                out->error = "the factorization would not solve";
                return false;
            }
        } else {
            num::IterativeOptions iterative;
            iterative.preconditioner = num::Preconditioner::AlgebraicMultigrid;
            iterative.tolerance = 1e-12;
            iterative.max_iterations = 5000;
            const num::IterativeResult result =
                num::ConjugateGradient(tangent, rhs, &correction, iterative);
            if (!result.converged) {
                out->error = "the iterative solver did not converge: " + result.message;
                return false;
            }
        }
        for (int i = 0; i < size; ++i) temperature[Idx(node_of[Idx(i)])] += correction[Idx(i)];
    }

    out->temperature = temperature;
    out->lowest = temperature.empty() ? 0.0 : *std::min_element(temperature.begin(), temperature.end());
    out->highest = temperature.empty() ? 0.0 : *std::max_element(temperature.begin(), temperature.end());

    // Flux at each element's centre: -k grad T, which is Fourier's law and
    // the thing an engineer actually reads off a thermal result.
    out->flux.assign(model.elements.size(), cad::Vec3d{});
    std::vector<QuadraturePoint> rule;
    std::vector<double> shape;
    std::vector<double> dn;
    std::vector<double> dn_xyz;
    std::vector<cad::Vec3d> corner;
    std::vector<cad::Vec3d> reference;
    for (int e = 0; e < model.ElementCount(); ++e) {
        const BoundElement &element = model.elements[Idx(e)];
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model.nodes[Idx(node)]);
        ReferenceNodes(element.shape, &reference);
        cad::Vec3d centre{};
        for (const cad::Vec3d &p : reference) centre = centre + p;
        centre = centre * (1.0 / static_cast<double>(reference.size()));
        ShapeFunctions(element.shape, centre, &shape, &dn);
        ElementJacobian(element.shape, corner, dn, &dn_xyz);
        const int count = static_cast<int>(element.nodes.size());
        double local = 0.0;
        cad::Vec3d gradient{};
        for (int a = 0; a < count; ++a) {
            const double t = temperature[Idx(element.nodes[Idx(a)])];
            local += shape[Idx(a)] * t;
            gradient = gradient + cad::Vec3d{dn_xyz[Idx(a * 3 + 0)], dn_xyz[Idx(a * 3 + 1)],
                                             dn_xyz[Idx(a * 3 + 2)]} * t;
        }
        const double k = model.materials[Idx(element.material)].conductivity.At(local);
        out->flux[Idx(e)] = gradient * -k;
    }

    // Conservation. The heat flowing in at the fixed nodes plus everything
    // applied must come to zero, and that is the cheapest global check
    // there is that the solve was right: it is a statement about the
    // assembled system rather than about any one element.
    //
    // The reaction at a fixed node is the residual there, which for a
    // converged solve is zero everywhere else.
    out->reaction.assign(Idx(n), 0.0);
    double net = 0.0;
    double total = 0.0;
    {
        const Assembled at = AssembleSteady(model, temperature);
        if (!at.ok) {
            out->error = at.error;
            return false;
        }
        // WHAT GOES IN MUST COME OUT: the heat the fixed nodes supply,
        // plus the net heat entering through the surfaces and the
        // sources, is zero for a converged steady solve.
        //
        // The obvious thing -- summing the residual vector -- measures
        // something else entirely and looks plausible while doing it.
        // The residual is zero at every free node once Newton has
        // converged and carries the reaction at every fixed one, so its
        // sum is the total heat *supplied* rather than the imbalance;
        // normalised by a total that counts the supply and the loss
        // alike, it lands on exactly one half whenever the solve is
        // perfect. NAFEMS T1 printed 5.00e-01 on three meshes whose
        // temperatures were right to 1e-13 K, which is how it was found.
        // A self-check that reads 0.5 for a correct answer is worse than
        // no self-check, because the number looks like a measurement.
        total = at.power;
        double supplied = 0.0;
        for (int i = 0; i < n; ++i) {
            if (!fixed[Idx(i)]) continue;
            // The heat the fixed node had to supply is the negative of
            // the residual left there.
            out->reaction[Idx(i)] = -at.residual[Idx(i)];
            supplied += out->reaction[Idx(i)];
            total += std::fabs(at.residual[Idx(i)]);
        }
        net = supplied + at.inflow;
    }
    out->energy_residual = std::fabs(net) / std::max(total, 1e-300);
    out->ok = true;
    return true;
}


double ExplicitStabilityLimit(const ThermalModel &model, double temperature) {
    // The limit is 2 / max eigenvalue of C^-1 K, and the cheap bound on
    // that eigenvalue is the largest row of K divided by the smallest
    // lumped capacity -- Gershgorin, which over-estimates the eigenvalue
    // and so under-estimates the limit, which is the safe direction.
    const std::vector<double> uniform(Idx(model.NodeCount()), temperature);
    const Assembled at = AssembleSteady(model, uniform);
    if (!at.ok) return 0.0;
    std::vector<double> lumped;
    bool ok = true;
    std::string error;
    AssembleCapacity(model, uniform, &lumped, &ok, &error);
    if (!ok) return 0.0;
    std::vector<double> row(Idx(model.NodeCount()), 0.0);
    for (const num::Triplet &entry : at.triplets) {
        row[Idx(entry.row)] += std::fabs(entry.value);
    }
    double worst = 0.0;
    for (int i = 0; i < model.NodeCount(); ++i) {
        if (!(lumped[Idx(i)] > 0.0)) continue;
        worst = std::max(worst, row[Idx(i)] / lumped[Idx(i)]);
    }
    return worst > 0.0 ? 2.0 / worst : 0.0;
}

bool SolveTransientHeat(const ThermalModel &model, const std::vector<double> &initial,
                        const TransientOptions &options, TransientResult *out) {
    *out = TransientResult{};
    const int n = model.NodeCount();
    if (n == 0 || model.elements.empty()) {
        out->error = "the model has no elements";
        return false;
    }
    if (static_cast<int>(initial.size()) != n) {
        out->error = "the initial temperature does not cover every node";
        return false;
    }
    if (options.theta < 0.0 || options.theta > 1.0) {
        out->error = "theta must be between zero and one";
        return false;
    }
    if (!(options.end_time > 0.0)) {
        out->error = "the end time must be positive";
        return false;
    }

    std::vector<bool> fixed(Idx(n), false);
    std::vector<double> prescribed = initial;
    for (const auto &entry : model.fixed) {
        if (entry.first < 0 || entry.first >= n) {
            out->error = "a fixed temperature names a node that does not exist";
            return false;
        }
        fixed[Idx(entry.first)] = true;
        prescribed[Idx(entry.first)] = entry.second;
    }
    std::vector<int> row_of(Idx(n), -1);
    std::vector<int> node_of;
    for (int i = 0; i < n; ++i) {
        if (fixed[Idx(i)]) continue;
        row_of[Idx(i)] = static_cast<int>(node_of.size());
        node_of.push_back(i);
    }
    if (node_of.empty()) {
        out->error = "every node's temperature is prescribed, so nothing changes with time";
        return false;
    }
    const int size = static_cast<int>(node_of.size());

    // A FIXED TEMPERATURE IS IMPOSED AT THE FIRST STEP, NOT BLENDED IN.
    // The initial field may disagree with it -- a body at room temperature
    // with one face suddenly held hot is the commonest transient there is
    // -- and that disagreement is the problem, not an inconsistency to be
    // smoothed away.
    std::vector<double> temperature = initial;
    for (int i = 0; i < n; ++i) {
        if (fixed[Idx(i)]) temperature[Idx(i)] = prescribed[Idx(i)];
    }

    if (options.theta == 0.0) {
        const double limit = ExplicitStabilityLimit(model, temperature[0]);
        const double asked = options.step > 0.0 ? options.step : options.end_time * 0.01;
        if (limit > 0.0 && asked > limit) {
            out->error = "forward Euler is stable on this model only up to a step of " +
                         std::to_string(limit) + " and was asked for " + std::to_string(asked) +
                         "; this is the conditional stability of an explicit method rather than a "
                         "tolerance to loosen";
            return false;
        }
    }

    // One step of the theta method, from `from` over `dt`, by Newton.
    auto take_step = [&](const std::vector<double> &from, double dt, std::vector<double> *to,
                         std::string *error) {
        const Assembled before = AssembleSteady(model, from);
        if (!before.ok) {
            *error = before.error;
            return false;
        }
        *to = from;
        for (int sweep = 0; sweep < std::max(1, options.newton_iterations); ++sweep) {
            const Assembled after = AssembleSteady(model, *to);
            if (!after.ok) {
                *error = after.error;
                return false;
            }
            std::vector<double> lumped;
            bool ok = true;
            const std::vector<num::Triplet> capacity =
                AssembleCapacity(model, *to, &lumped, &ok, error);
            if (!ok) return false;

            // Residual of the step:
            //   C (T' - T)/dt - theta S(T') - (1 - theta) S(T) = 0
            std::vector<double> residual(Idx(n), 0.0);
            for (const num::Triplet &entry : capacity) {
                residual[Idx(entry.row)] -= entry.value *
                                            ((*to)[Idx(entry.col)] - from[Idx(entry.col)]) / dt;
            }
            for (int i = 0; i < n; ++i) {
                residual[Idx(i)] += options.theta * after.residual[Idx(i)] +
                                    (1.0 - options.theta) * before.residual[Idx(i)];
            }
            double worst = 0.0;
            double scale = 0.0;
            for (const int node : node_of) {
                worst = std::max(worst, std::fabs(residual[Idx(node)]));
                scale = std::max(scale, std::fabs(lumped[Idx(node)]) / dt);
            }
            // Relative to a capacity over the step, which is the size of
            // the terms being cancelled and has the units the residual
            // does.
            double reference = 0.0;
            for (const int node : node_of) {
                reference = std::max(reference, std::fabs((*to)[Idx(node)]));
            }
            const double measure = worst / std::max(scale * std::max(reference, 1.0), 1e-300);
            if (measure < options.newton_tolerance) break;

            std::vector<num::Triplet> reduced;
            reduced.reserve(capacity.size() + after.triplets.size());
            for (const num::Triplet &entry : capacity) {
                const int row = row_of[Idx(entry.row)];
                const int column = row_of[Idx(entry.col)];
                if (row < 0 || column < 0) continue;
                reduced.push_back(num::Triplet{row, column, entry.value / dt});
            }
            if (options.theta != 0.0) {
                for (const num::Triplet &entry : after.triplets) {
                    const int row = row_of[Idx(entry.row)];
                    const int column = row_of[Idx(entry.col)];
                    if (row < 0 || column < 0) continue;
                    reduced.push_back(num::Triplet{row, column, options.theta * entry.value});
                }
            }
            std::vector<double> rhs(Idx(size), 0.0);
            for (int i = 0; i < size; ++i) rhs[Idx(i)] = residual[Idx(node_of[Idx(i)])];
            const num::SparseMatrix tangent =
                num::SparseMatrix::FromTriplets(size, size, reduced);
            std::vector<double> correction;
            if (options.solver == SolverKind::Direct) {
                num::SparseLDLT factor;
                if (!factor.Factorize(tangent, options.ordering)) {
                    *error = factor.Error();
                    return false;
                }
                if (!factor.Solve(rhs, &correction)) {
                    *error = "the factorization would not solve";
                    return false;
                }
            } else {
                num::IterativeOptions iterative;
                iterative.preconditioner = num::Preconditioner::AlgebraicMultigrid;
                iterative.tolerance = 1e-12;
                iterative.max_iterations = 5000;
                const num::IterativeResult result =
                    num::ConjugateGradient(tangent, rhs, &correction, iterative);
                if (!result.converged) {
                    *error = "the iterative solver did not converge: " + result.message;
                    return false;
                }
            }
            for (int i = 0; i < size; ++i) (*to)[Idx(node_of[Idx(i)])] += correction[Idx(i)];
        }
        return true;
    };

    const double first = options.step > 0.0 ? options.step : options.end_time * 0.01;
    const double smallest = options.min_step > 0.0 ? options.min_step : options.end_time * 1e-9;
    const double largest = options.max_step > 0.0 ? options.max_step : options.end_time;
    double dt = std::min(std::max(first, smallest), largest);
    out->smallest_step = dt;
    out->largest_step = dt;
    double time = 0.0;
    // For the energy check: the heat that crossed the boundary, summed
    // over the history with the same theta weighting the solve used.
    //
    // TWO TERMS, AND THE SECOND ONE IS EASY TO MISS. Summing the steady
    // residual over every node gives the *applied* heat and nothing else:
    // the conduction terms cancel internally, because the shape function
    // derivatives sum to zero, so a slab with no source and no surface
    // flow sums to exactly nothing. The heat a *fixed* node supplies to
    // hold itself at its temperature is not in that sum, and for a body
    // being quenched through its faces it is the whole of the energy flow.
    // Left out, the check reported a residual of 1.00 on a solve that was
    // right -- which is worse than no check, because it is a check that
    // cries wolf.
    double supplied = 0.0;
    const std::vector<double> at_start = temperature;
    auto boundary_heat = [&](const std::vector<double> &from, const std::vector<double> &to,
                             double dt) {
        std::vector<double> lumped;
        bool ok = true;
        std::string error;
        const std::vector<num::Triplet> capacity =
            AssembleCapacity(model, to, &lumped, &ok, &error);
        if (!ok) return 0.0;
        const Assembled before = AssembleSteady(model, from);
        const Assembled after = AssembleSteady(model, to);
        std::vector<double> rate(Idx(n), 0.0);
        for (const num::Triplet &entry : capacity) {
            rate[Idx(entry.row)] += entry.value * (to[Idx(entry.col)] - from[Idx(entry.col)]) / dt;
        }
        double total = 0.0;
        for (int i = 0; i < n; ++i) {
            if (!fixed[Idx(i)]) continue;
            total += (rate[Idx(i)] - options.theta * after.residual[Idx(i)] -
                      (1.0 - options.theta) * before.residual[Idx(i)]) * dt;
        }
        return total;
    };

    // Emits every requested sample time the step just taken has passed.
    // INTERPOLATED WITHIN THE STEP rather than snapped to whichever end
    // is nearer: a frame rate finer than the step size is the normal case
    // for an animation, and snapping would make the picture hold still
    // and then jump, which reads as a solver artefact and is not one.
    std::size_t next_sample = 0;
    auto record = [&](const std::vector<double> &from, const std::vector<double> &to, double t0,
                      double t1) {
        while (next_sample < options.sample_times.size() &&
               options.sample_times[next_sample] <= t1 + 1e-15) {
            const double want = options.sample_times[next_sample];
            const double span = t1 - t0;
            const double alpha = span > 0.0 ? cad::Clamp((want - t0) / span, 0.0, 1.0) : 1.0;
            std::vector<double> field(from.size(), 0.0);
            for (std::size_t i = 0; i < from.size(); ++i) {
                field[i] = from[i] + (to[i] - from[i]) * alpha;
            }
            out->samples.push_back(std::move(field));
            ++next_sample;
        }
    };
    // A sample at or before the start is the initial condition, which no
    // step will ever bracket.
    record(temperature, temperature, 0.0, 0.0);

    while (time < options.end_time * (1.0 - 1e-12)) {
        if (out->steps >= options.max_steps) {
            out->error = "the transient needed more than " + std::to_string(options.max_steps) +
                         " steps to reach the end time";
            return false;
        }
        dt = std::min(dt, options.end_time - time);
        const std::vector<double> before_step = temperature;
        const double step_start = time;
        std::vector<double> whole;
        std::string error;
        if (!take_step(temperature, dt, &whole, &error)) {
            out->error = error;
            return false;
        }
        if (!options.adaptive) {
            // The heat that crossed the boundary over the step, theta
            // weighted the same way the step was.
            const Assembled before = AssembleSteady(model, temperature);
            const Assembled after = AssembleSteady(model, whole);
            for (int i = 0; i < n; ++i) {
                supplied += dt * (options.theta * after.residual[Idx(i)] +
                                  (1.0 - options.theta) * before.residual[Idx(i)]);
            }
            supplied += boundary_heat(temperature, whole, dt);
            temperature = whole;
            time += dt;
            ++out->steps;
            record(before_step, temperature, step_start, time);
            continue;
        }

        // STEP DOUBLING. One step of dt against two of dt/2: the
        // difference is proportional to the local error, with no need for
        // a second integrator or an embedded formula. It costs three
        // solves per accepted step, which is the price of an estimate that
        // measures the method actually being used.
        std::vector<double> half;
        std::vector<double> halves;
        if (!take_step(temperature, dt * 0.5, &half, &error) ||
            !take_step(half, dt * 0.5, &halves, &error)) {
            out->error = error;
            return false;
        }
        double difference = 0.0;
        double span = 0.0;
        for (const int node : node_of) {
            difference = std::max(difference, std::fabs(halves[Idx(node)] - whole[Idx(node)]));
            span = std::max(span, std::fabs(halves[Idx(node)] - at_start[Idx(node)]));
        }
        // The estimate is of the *halved* solution's error, which for a
        // method of order p is the difference over (2^p - 1).
        const double order = options.theta == 0.5 ? 2.0 : 1.0;
        const double estimate = difference / (std::pow(2.0, order) - 1.0);
        const double allowed = options.tolerance * std::max(span, 1.0);
        if (estimate > allowed && dt > smallest * (1.0 + 1e-12)) {
            // Rejected: halve and try again. Not a smooth factor, because
            // a rejected step means the estimate was wrong about the step
            // before it too.
            ++out->rejected;
            dt = std::max(dt * 0.5, smallest);
            continue;
        }
        const Assembled before = AssembleSteady(model, temperature);
        const Assembled middle = AssembleSteady(model, half);
        const Assembled after = AssembleSteady(model, halves);
        for (int i = 0; i < n; ++i) {
            supplied += dt * 0.5 * (options.theta * middle.residual[Idx(i)] +
                                    (1.0 - options.theta) * before.residual[Idx(i)]);
            supplied += dt * 0.5 * (options.theta * after.residual[Idx(i)] +
                                    (1.0 - options.theta) * middle.residual[Idx(i)]);
        }
        supplied += boundary_heat(temperature, half, dt * 0.5);
        supplied += boundary_heat(half, halves, dt * 0.5);
        temperature = halves;
        time += dt;
        ++out->steps;
        record(before_step, temperature, step_start, time);
        out->smallest_step = std::min(out->smallest_step, dt);
        out->largest_step = std::max(out->largest_step, dt);
        // Grown by the usual safety-factored power law, and capped at
        // doubling: a step that grows faster than that outruns its own
        // error estimate.
        const double growth = estimate > 0.0
                                  ? 0.9 * std::pow(allowed / estimate, 1.0 / (order + 1.0))
                                  : 2.0;
        dt = std::min(std::min(dt * std::min(growth, 2.0), largest), options.end_time - time > 0.0
                                                                        ? largest
                                                                        : largest);
    }

    // Anything still outstanding sits at or beyond the end time.
    record(temperature, temperature, time, time);
    out->temperature = temperature;
    out->time = time;
    out->lowest = *std::min_element(temperature.begin(), temperature.end());
    out->highest = *std::max_element(temperature.begin(), temperature.end());

    // CONSERVATION OVER THE WHOLE HISTORY, which is a far stronger check
    // than one step's residual: the heat that crossed the boundary must
    // equal the heat now stored, and an integrator that is subtly wrong
    // fails it by an amount that grows with the number of steps.
    std::vector<double> lumped;
    bool ok = true;
    std::string error;
    AssembleCapacity(model, temperature, &lumped, &ok, &error);
    if (!ok) {
        out->error = error;
        return false;
    }
    double stored = 0.0;
    double magnitude = 0.0;
    for (int i = 0; i < n; ++i) {
        stored += lumped[Idx(i)] * (temperature[Idx(i)] - at_start[Idx(i)]);
        magnitude += std::fabs(lumped[Idx(i)] * (temperature[Idx(i)] - at_start[Idx(i)]));
    }
    out->energy_residual =
        std::fabs(supplied - stored) / std::max(std::max(std::fabs(stored), magnitude), 1e-300);
    out->ok = true;
    return true;
}

}  // namespace fem
