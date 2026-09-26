#include "fem_nonlinear.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fem {
namespace {

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

}  // namespace

bool InternalForce(const AnalysisModel &model, const std::vector<double> &displacement,
                   const SparsityPattern &pattern, std::vector<double> *out_force,
                   num::SparseMatrix *out_tangent, std::string *error) {
    error->clear();
    const int dofs = pattern.Dofs();
    if (static_cast<int>(displacement.size()) != dofs) {
        *error = "the displacement does not cover the model's degrees of freedom";
        return false;
    }
    out_force->assign(Idx(dofs), 0.0);
    std::vector<double> values(Idx(pattern.NonZeros()), 0.0);

    std::vector<QuadraturePoint> rule;
    std::vector<double> n;
    std::vector<double> dn;
    std::vector<double> dn_dx;
    std::vector<cad::Vec3d> corner;
    std::vector<double> b;
    for (const BoundElement &element : model.elements) {
        if (element.material < 0 ||
            element.material >= static_cast<int>(model.materials.size())) {
            *error = "an element names a material the model does not have";
            return false;
        }
        double d[6][6];
        if (!model.materials[Idx(element.material)].ConstitutiveMatrix(
                model.MeanTemperature(element.nodes), d, error)) {
            return false;
        }
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model.nodes[Idx(node)]);
        if (!Quadrature(element.shape, 0, &rule, error)) return false;
        const int count = static_cast<int>(element.nodes.size());
        const int columns = count * 3;
        b.assign(Idx(6 * columns), 0.0);
        for (const QuadraturePoint &point : rule) {
            ShapeFunctions(element.shape, point.at, &n, &dn);
            // THE DERIVATIVES ARE WITH RESPECT TO THE *REFERENCE*
            // COORDINATES, which is what makes this total Lagrangian:
            // the integral is over the undeformed body and never moves.
            const double determinant = ElementJacobian(element.shape, corner, dn, &dn_dx);
            if (!(determinant > 0.0)) {
                *error = "an element of the undeformed mesh is turned inside out";
                return false;
            }
            // The displacement gradient, and F = I + H.
            double h[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
            for (int a = 0; a < count; ++a) {
                for (int i = 0; i < 3; ++i) {
                    const double u = displacement[Idx(element.nodes[Idx(a)] * 3 + i)];
                    for (int j = 0; j < 3; ++j) h[i][j] += u * dn_dx[Idx(a * 3 + j)];
                }
            }
            double f[3][3];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) f[i][j] = (i == j ? 1.0 : 0.0) + h[i][j];
            }
            // GREEN-LAGRANGE FROM THE GRADIENT, NOT FROM `(F^T F - I)/2`.
            // The two are the same algebra and not the same arithmetic.
            // Written the textbook way, a strain of 1e-8 is computed by
            // subtracting one from 1.00000002, which throws away eight of
            // the sixteen digits available -- so the residual of a lightly
            // loaded model stalls at 5e-8 and Newton looks as though it
            // will not converge. Expanding it,
            //     E = (H + H^T + H^T H)/2
            // has no such subtraction: the linear part is exact and the
            // quadratic part is a small correction to it. The large-strain
            // answer is identical and the small-strain one is eight digits
            // better.
            double e[3][3];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    double quadratic = 0.0;
                    for (int k = 0; k < 3; ++k) quadratic += h[k][i] * h[k][j];
                    e[i][j] = 0.5 * (h[i][j] + h[j][i] + quadratic);
                }
            }
            const double strain[6] = {e[0][0], e[1][1], e[2][2],
                                      2.0 * e[0][1], 2.0 * e[1][2], 2.0 * e[2][0]};
            double stress[6] = {0, 0, 0, 0, 0, 0};
            for (int r = 0; r < 6; ++r) {
                for (int q = 0; q < 6; ++q) stress[r] += d[r][q] * strain[q];
            }
            // The deformation-dependent strain-displacement matrix.
            static const int pair[6][2] = {{0, 0}, {1, 1}, {2, 2}, {0, 1}, {1, 2}, {2, 0}};
            for (int r = 0; r < 6; ++r) {
                const int p = pair[r][0];
                const int q = pair[r][1];
                for (int a = 0; a < count; ++a) {
                    for (int k = 0; k < 3; ++k) {
                        const double value =
                            r < 3 ? f[k][p] * dn_dx[Idx(a * 3 + p)]
                                  : f[k][p] * dn_dx[Idx(a * 3 + q)] +
                                        f[k][q] * dn_dx[Idx(a * 3 + p)];
                        b[Idx(r * columns + a * 3 + k)] = value;
                    }
                }
            }
            const double weight = determinant * point.weight;
            for (int i = 0; i < columns; ++i) {
                double sum = 0.0;
                for (int r = 0; r < 6; ++r) sum += b[Idx(r * columns + i)] * stress[r];
                (*out_force)[Idx(element.nodes[Idx(i / 3)] * 3 + i % 3)] += sum * weight;
            }
            if (out_tangent == nullptr) continue;
            // The material part, B^T D B, and the geometric part, which is
            // the same stress-stiffness term Part I.5 assembles -- with
            // the second Piola-Kirchhoff stress and the reference
            // derivatives, because that is the configuration everything
            // here lives in.
            const double sigma[3][3] = {{stress[0], stress[3], stress[5]},
                                        {stress[3], stress[1], stress[4]},
                                        {stress[5], stress[4], stress[2]}};
            for (int a = 0; a < count; ++a) {
                for (int bb = 0; bb < count; ++bb) {
                    double geometric = 0.0;
                    for (int k = 0; k < 3; ++k) {
                        for (int l = 0; l < 3; ++l) {
                            geometric += dn_dx[Idx(a * 3 + k)] * sigma[k][l] *
                                         dn_dx[Idx(bb * 3 + l)];
                        }
                    }
                    for (int i = 0; i < 3; ++i) {
                        for (int j = 0; j < 3; ++j) {
                            double material = 0.0;
                            for (int r = 0; r < 6; ++r) {
                                for (int q = 0; q < 6; ++q) {
                                    material += b[Idx(r * columns + a * 3 + i)] * d[r][q] *
                                                b[Idx(q * columns + bb * 3 + j)];
                                }
                            }
                            const double total =
                                (material + (i == j ? geometric : 0.0)) * weight;
                            if (total == 0.0) continue;
                            const int at = pattern.Find(element.nodes[Idx(a)] * 3 + i,
                                                        element.nodes[Idx(bb)] * 3 + j);
                            if (at < 0) {
                                *error = "the sparsity pattern has no entry the tangent needs";
                                return false;
                            }
                            values[Idx(at)] += total;
                        }
                    }
                }
            }
        }
    }
    if (out_tangent == nullptr) return true;
    std::vector<num::Triplet> triplets;
    triplets.reserve(Idx(pattern.NonZeros()));
    for (int row = 0; row < dofs; ++row) {
        for (int at = pattern.RowStart()[Idx(row)]; at < pattern.RowStart()[Idx(row + 1)]; ++at) {
            if (values[Idx(at)] == 0.0) continue;
            triplets.push_back(num::Triplet{row, pattern.ColIndex()[Idx(at)], values[Idx(at)]});
        }
    }
    *out_tangent = num::SparseMatrix::FromTriplets(dofs, dofs, triplets);
    return true;
}

namespace {

// The machinery both solvers share: which degrees of freedom are free,
// what the prescribed values are, and how to reduce a full-length vector
// or matrix onto the free ones.
struct Freedom {
    bool ok = false;
    std::string error;
    SparsityPattern pattern;
    std::vector<int> dof_of_row;
    std::vector<int> row_of_dof;
    std::vector<bool> fixed;
    std::vector<double> prescribed;
    std::vector<double> external;
    int size = 0;
};

Freedom Prepare(const AnalysisModel &model, const std::vector<MultiPointConstraint> &constraints,
                const AssemblyOptions &assembly) {
    Freedom out;
    System system;
    if (!BuildSystem(model, constraints, assembly, &system, &out.error)) return out;
    if (system.multipliers > 0) {
        out.error = "multi-point constraints in a nonlinear solve need the constraint applied at "
                    "every iteration, which is not written; use elimination or a penalty";
        return out;
    }
    if (!out.pattern.Build(model, &out.error)) return out;
    out.dof_of_row = system.dof_of_row;
    out.fixed = system.fixed;
    out.prescribed = system.prescribed;
    out.size = static_cast<int>(out.dof_of_row.size());
    out.row_of_dof.assign(Idx(out.pattern.Dofs()), -1);
    for (int i = 0; i < out.size; ++i) out.row_of_dof[Idx(out.dof_of_row[Idx(i)])] = i;
    out.external = AssembleLoads(model);
    out.ok = true;
    return out;
}

num::SparseMatrix ReduceMatrix(const num::SparseMatrix &full, const Freedom &freedom) {
    std::vector<num::Triplet> triplets;
    for (int row = 0; row < full.Rows(); ++row) {
        const int reduced_row = freedom.row_of_dof[Idx(row)];
        if (reduced_row < 0) continue;
        for (int at = full.RowStart()[Idx(row)]; at < full.RowStart()[Idx(row + 1)]; ++at) {
            const int reduced_column = freedom.row_of_dof[Idx(full.ColIndex()[Idx(at)])];
            if (reduced_column < 0) continue;
            triplets.push_back(
                num::Triplet{reduced_row, reduced_column, full.Values()[Idx(at)]});
        }
    }
    return num::SparseMatrix::FromTriplets(freedom.size, freedom.size, triplets);
}

std::vector<cad::Vec3d> AsNodes(const std::vector<double> &flat, int nodes) {
    std::vector<cad::Vec3d> out(Idx(nodes), cad::Vec3d{});
    for (int node = 0; node < nodes; ++node) {
        out[Idx(node)] = cad::Vec3d{flat[Idx(node * 3 + 0)], flat[Idx(node * 3 + 1)],
                                    flat[Idx(node * 3 + 2)]};
    }
    return out;
}

}  // namespace

bool SolveNonlinear(const AnalysisModel &model,
                    const std::vector<MultiPointConstraint> &constraints,
                    const NonlinearOptions &options, NonlinearResult *out) {
    *out = NonlinearResult{};
    const Freedom freedom = Prepare(model, constraints, options.assembly);
    if (!freedom.ok) {
        out->error = freedom.error;
        return false;
    }
    const int dofs = freedom.pattern.Dofs();
    std::vector<double> displacement(Idx(dofs), 0.0);
    for (int i = 0; i < dofs; ++i) {
        if (freedom.fixed[Idx(i)]) displacement[Idx(i)] = 0.0;
    }
    // MEASURED AGAINST THE FORCES IN PLAY, NOT AGAINST THE APPLIED LOAD.
    // A model driven entirely by prescribed displacement -- a press, a
    // contact rig, anything pushed rather than pulled -- has no applied
    // load at all, so normalising by it falls back to one newton and the
    // test becomes "is the residual below a nanonewton", which no model
    // in newtons and metres ever satisfies.
    //
    // The scale that is always available is the out-of-balance the
    // increment *starts* with: moving the prescribed freedoms to their
    // new values and leaving the rest where they were is a state out of
    // equilibrium by a definite amount, and driving that down by nine
    // orders is the same demand as before for a load-driven model. It is
    // not the internal force -- with no applied load the residual IS the
    // internal force on the free rows, so that ratio is identically one,
    // which the first attempt at this duly printed thirty times.
    double external_norm = 0.0;
    for (const double value : freedom.external) external_norm += value * value;
    external_norm = std::sqrt(external_norm);

    double factor = 0.0;
    double increment = 1.0 / std::max(1, options.steps);
    int cuts = 0;
    std::vector<double> force;
    num::SparseMatrix tangent;
    while (factor < 1.0 - 1e-12) {
        const double target = std::min(1.0, factor + increment);
        const std::vector<double> start = displacement;
        // A PRESCRIBED DISPLACEMENT IS RAMPED WITH THE LOAD, because a
        // nonlinear path depends on how it was travelled: applying the
        // whole of a large prescribed displacement at the first step is a
        // different problem from reaching it gradually, and usually one
        // Newton cannot solve.
        for (int i = 0; i < dofs; ++i) {
            if (freedom.fixed[Idx(i)]) displacement[Idx(i)] = target * freedom.prescribed[Idx(i)];
        }
        bool converged = false;
        std::vector<double> residuals;
        double opening_norm = 0.0;
        double penalty = 0.0;
        for (int iteration = 0; iteration < options.max_iterations; ++iteration) {
            if (!InternalForce(model, displacement, freedom.pattern, &force, &tangent,
                               &out->error)) {
                return false;
            }
            std::vector<num::Triplet> extra;
            if (!options.contact.nodes.empty()) {
                if (penalty == 0.0) {
                    // Scaled to the stiffness, so that the same number
                    // means the same thing whatever the model is made of.
                    double largest = 0.0;
                    for (int i = 0; i < tangent.Rows(); ++i) {
                        largest = std::max(largest, std::fabs(tangent.At(i, i)));
                    }
                    penalty = largest * options.contact.penalty_scale;
                }
                out->touching = AddContact(model, options.contact, penalty, displacement, &force,
                                           &extra, &out->worst_penetration);
            }
            std::vector<double> residual(Idx(freedom.size), 0.0);
            for (int i = 0; i < freedom.size; ++i) {
                const int dof = freedom.dof_of_row[Idx(i)];
                residual[Idx(i)] = target * freedom.external[Idx(dof)] - force[Idx(dof)];
            }
            double norm = 0.0;
            for (const double value : residual) norm += value * value;
            norm = std::sqrt(norm);
            if (residuals.empty()) opening_norm = norm;
            const double scale =
                std::max(std::max(external_norm, opening_norm), 1e-300);
            norm /= scale;
            residuals.push_back(norm);
            ++out->total_iterations;
            if (norm < options.tolerance) {
                converged = true;
                break;
            }
            num::SparseMatrix with_contact = tangent;
            if (!extra.empty()) {
                std::vector<num::Triplet> all;
                for (int row = 0; row < tangent.Rows(); ++row) {
                    for (int p = tangent.RowStart()[Idx(row)];
                         p < tangent.RowStart()[Idx(row + 1)]; ++p) {
                        all.push_back(num::Triplet{row, tangent.ColIndex()[Idx(p)],
                                                   tangent.Values()[Idx(p)]});
                    }
                }
                for (const num::Triplet &entry : extra) all.push_back(entry);
                with_contact =
                    num::SparseMatrix::FromTriplets(tangent.Rows(), tangent.Cols(), all);
            }
            const num::SparseMatrix reduced = ReduceMatrix(with_contact, freedom);
            num::SparseLDLT solver;
            if (!solver.Factorize(reduced, options.ordering)) {
                out->warnings.push_back("the tangent could not be factored at load factor " +
                                        std::to_string(target) + ": " + solver.Error());
                break;
            }
            std::vector<double> step;
            if (!solver.Solve(residual, &step)) break;

            // LINE SEARCH ON THE ENERGY, not on the residual norm. The
            // quantity Newton is driving to zero is the projection of the
            // residual onto the step it proposed, and backtracking on that
            // keeps the direction's descent property; backtracking on the
            // norm can reject a good step that happens to overshoot.
            double alpha = 1.0;
            if (options.line_search) {
                double reference = 0.0;
                for (int i = 0; i < freedom.size; ++i) reference += step[Idx(i)] * residual[Idx(i)];
                for (int trial = 0; trial < 8; ++trial) {
                    std::vector<double> candidate = displacement;
                    for (int i = 0; i < freedom.size; ++i) {
                        candidate[Idx(freedom.dof_of_row[Idx(i)])] += alpha * step[Idx(i)];
                    }
                    std::vector<double> trial_force;
                    if (!InternalForce(model, candidate, freedom.pattern, &trial_force, nullptr,
                                       &out->error)) {
                        return false;
                    }
                    // WITH THE CONTACT FORCE INCLUDED, because a line
                    // search that judges a different problem from the one
                    // being solved will reject good steps and accept bad
                    // ones -- and the contact force is most of the
                    // residual near an obstacle.
                    if (!options.contact.nodes.empty()) {
                        AddContact(model, options.contact, penalty, candidate, &trial_force,
                                   nullptr, nullptr);
                    }
                    double projected = 0.0;
                    for (int i = 0; i < freedom.size; ++i) {
                        const int dof = freedom.dof_of_row[Idx(i)];
                        projected += step[Idx(i)] *
                                     (target * freedom.external[Idx(dof)] - trial_force[Idx(dof)]);
                    }
                    if (std::fabs(projected) <= std::fabs(reference) * 0.5 || trial == 7) break;
                    alpha *= 0.5;
                }
            }
            for (int i = 0; i < freedom.size; ++i) {
                displacement[Idx(freedom.dof_of_row[Idx(i)])] += alpha * step[Idx(i)];
            }
        }
        if (!converged) {
            if (cuts >= options.max_cuts) {
                out->error = "the step at load factor " + std::to_string(target) +
                             " would not converge, and the increment has been halved " +
                             std::to_string(cuts) +
                             " times; past a limit point there is no equilibrium to find under "
                             "load control, and arc length is what walks round one";
                out->last_residuals = residuals;
                return false;
            }
            ++cuts;
            ++out->cuts;
            increment *= 0.5;
            displacement = start;
            continue;
        }
        factor = target;
        out->last_residuals = residuals;
        NonlinearStep step;
        step.load_factor = factor;
        step.displacement = AsNodes(displacement, model.NodeCount());
        step.iterations = static_cast<int>(residuals.size());
        step.residual = residuals.back();
        out->path.push_back(step);
    }
    out->displacement = AsNodes(displacement, model.NodeCount());
    out->load_factor = factor;
    out->ok = true;
    return true;
}

bool SolveArcLength(const AnalysisModel &model,
                    const std::vector<MultiPointConstraint> &constraints,
                    const ArcLengthOptions &options, NonlinearResult *out) {
    *out = NonlinearResult{};
    const Freedom freedom = Prepare(model, constraints, options.assembly);
    if (!freedom.ok) {
        out->error = freedom.error;
        return false;
    }
    const int dofs = freedom.pattern.Dofs();
    std::vector<double> displacement(Idx(dofs), 0.0);
    double factor = 0.0;
    double external_norm = 0.0;
    for (const double value : freedom.external) external_norm += value * value;
    external_norm = std::sqrt(external_norm);
    if (!(external_norm > 0.0)) external_norm = 1.0;

    std::vector<double> force;
    num::SparseMatrix tangent;
    // The arc length, set from what a linear solve under the whole load
    // would give -- which is the only length scale the problem offers
    // before anything has been solved.
    double arc = 0.0;
    {
        if (!InternalForce(model, displacement, freedom.pattern, &force, &tangent, &out->error)) {
            return false;
        }
        const num::SparseMatrix reduced = ReduceMatrix(tangent, freedom);
        num::SparseLDLT solver;
        if (!solver.Factorize(reduced, options.ordering)) {
            out->error = solver.Error();
            return false;
        }
        std::vector<double> rhs(Idx(freedom.size), 0.0);
        for (int i = 0; i < freedom.size; ++i) {
            rhs[Idx(i)] = freedom.external[Idx(freedom.dof_of_row[Idx(i)])];
        }
        std::vector<double> linear;
        if (!solver.Solve(rhs, &linear)) {
            out->error = "the initial tangent would not solve";
            return false;
        }
        double norm = 0.0;
        for (const double value : linear) norm += value * value;
        const double whole = std::sqrt(norm);
        const double fraction =
            options.first_step > 0.0 ? options.first_step : 1.0 / std::max(1, options.steps);
        arc = whole * fraction;
    }
    // Which way along the path to set off. The first step follows the
    // linear solution; afterwards the sign is chosen to keep going the
    // same way, which is what stops the method turning round at a limit
    // point and retracing its own steps.
    double previous_sign = 1.0;
    std::vector<double> previous_step(Idx(freedom.size), 0.0);

    for (int step = 0; step < options.steps; ++step) {
        const std::vector<double> start = displacement;
        const double start_factor = factor;
        bool converged = false;
        std::vector<double> residuals;
        double trial_arc = arc;
        for (int attempt = 0; attempt < 8 && !converged; ++attempt) {
            displacement = start;
            factor = start_factor;
            std::vector<double> total(Idx(freedom.size), 0.0);
            residuals.clear();
            for (int iteration = 0; iteration < options.max_iterations; ++iteration) {
                if (!InternalForce(model, displacement, freedom.pattern, &force, &tangent,
                                   &out->error)) {
                    return false;
                }
                std::vector<double> residual(Idx(freedom.size), 0.0);
                std::vector<double> reference(Idx(freedom.size), 0.0);
                for (int i = 0; i < freedom.size; ++i) {
                    const int dof = freedom.dof_of_row[Idx(i)];
                    reference[Idx(i)] = freedom.external[Idx(dof)];
                    residual[Idx(i)] = factor * reference[Idx(i)] - force[Idx(dof)];
                }
                double norm = 0.0;
                for (const double value : residual) norm += value * value;
                norm = std::sqrt(norm) / external_norm;
                residuals.push_back(norm);
                ++out->total_iterations;
                if (iteration > 0 && norm < options.tolerance) {
                    converged = true;
                    break;
                }
                const num::SparseMatrix reduced = ReduceMatrix(tangent, freedom);
                num::SparseLDLT solver;
                // PAST A LIMIT POINT THE TANGENT IS INDEFINITE, which is
                // the whole reason arc length exists, so a negative pivot
                // is expected here rather than a failure. Only a singular
                // one -- exactly at the limit point -- is a problem.
                if (!solver.Factorize(reduced, options.ordering, 1e-300)) {
                    residuals.push_back(1e30);
                    break;
                }
                std::vector<double> from_residual;
                std::vector<double> from_load;
                if (!solver.Solve(residual, &from_residual) ||
                    !solver.Solve(reference, &from_load)) {
                    break;
                }
                // The cylindrical constraint: the increment's length over
                // the whole step is the arc length. Substituting
                // `du = du_R + dlambda du_F` gives a quadratic in
                // dlambda, and the root to take is the one that keeps
                // going the way the path was going.
                double a = 0.0;
                double b = 0.0;
                double c = 0.0;
                for (int i = 0; i < freedom.size; ++i) {
                    const double sum = total[Idx(i)] + from_residual[Idx(i)];
                    a += from_load[Idx(i)] * from_load[Idx(i)];
                    b += 2.0 * sum * from_load[Idx(i)];
                    c += sum * sum;
                }
                c -= trial_arc * trial_arc;
                double delta = 0.0;
                if (std::fabs(a) < 1e-300) {
                    delta = std::fabs(b) > 1e-300 ? -c / b : 0.0;
                } else {
                    const double discriminant = b * b - 4.0 * a * c;
                    if (discriminant < 0.0) {
                        // No point on the path at this radius: shorten and
                        // try the step again.
                        residuals.push_back(1e30);
                        break;
                    }
                    const double root = std::sqrt(discriminant);
                    const double first = (-b + root) / (2.0 * a);
                    const double second = (-b - root) / (2.0 * a);
                    // Whichever continues the direction of travel.
                    double best = first;
                    double best_score = -1e300;
                    for (const double candidate : {first, second}) {
                        double score = 0.0;
                        for (int i = 0; i < freedom.size; ++i) {
                            score += previous_step[Idx(i)] *
                                     (total[Idx(i)] + from_residual[Idx(i)] +
                                      candidate * from_load[Idx(i)]);
                        }
                        score *= previous_sign;
                        if (score <= best_score) continue;
                        best_score = score;
                        best = candidate;
                    }
                    delta = best;
                }
                for (int i = 0; i < freedom.size; ++i) {
                    const double change = from_residual[Idx(i)] + delta * from_load[Idx(i)];
                    total[Idx(i)] += change;
                    displacement[Idx(freedom.dof_of_row[Idx(i)])] += change;
                }
                factor += delta;
            }
            if (converged) {
                previous_step = total;
                previous_sign = 1.0;
                break;
            }
            trial_arc *= 0.5;
            ++out->cuts;
        }
        if (!converged) {
            out->warnings.push_back("the arc-length step " + std::to_string(step) +
                                    " would not converge even at a sixteenth of the arc length; "
                                    "the path stops here");
            displacement = start;
            factor = start_factor;
            break;
        }
        if (options.adaptive) {
            // GROWN FROM WHAT ACTUALLY WORKED, NOT FROM WHAT WAS ASKED.
            // The retry loop shortens the arc until a step converges;
            // adapting the persistent arc length from its own previous
            // value instead means the shortening is forgotten and then
            // repeated, and near a limit point -- where every step is
            // hard -- it collapses towards nothing. The path then
            // "converges" step after step with no increment at all: on a
            // shallow arch it climbed to its limit load and then produced
            // a hundred and thirty identical steps.
            const double wanted = 4.0;
            const double used = std::max(1.0, static_cast<double>(residuals.size()));
            const double factor_change =
                std::max(0.6, std::min(options.max_factor, std::sqrt(wanted / used)));
            arc = trial_arc * factor_change;
        }
        // A STEP THAT MOVES NOWHERE IS A STALL, NOT A CONVERGENCE. It is
        // what a collapsing arc length looks like from the outside, and
        // silently returning a path made of identical points would be the
        // worst of the available answers.
        {
            double moved = 0.0;
            double travelled = 0.0;
            for (int i = 0; i < dofs; ++i) {
                moved = std::max(moved, std::fabs(displacement[Idx(i)] - start[Idx(i)]));
                travelled = std::max(travelled, std::fabs(displacement[Idx(i)]));
            }
            if (moved <= std::max(travelled, 1e-300) * 1e-10) {
                out->warnings.push_back(
                    "the arc-length path stopped moving at step " + std::to_string(step) +
                    ", at load factor " + std::to_string(factor) +
                    "; the arc length has collapsed, which is what happens at a limit point the "
                    "step control cannot get past");
                break;
            }
        }
        out->last_residuals = residuals;
        NonlinearStep record;
        record.load_factor = factor;
        record.displacement = AsNodes(displacement, model.NodeCount());
        record.iterations = static_cast<int>(residuals.size());
        record.residual = residuals.back();
        out->path.push_back(record);
    }
    out->displacement = AsNodes(displacement, model.NodeCount());
    out->load_factor = factor;
    out->ok = !out->path.empty();
    if (!out->ok) out->error = "no arc-length step converged";
    return out->ok;
}

}  // namespace fem
