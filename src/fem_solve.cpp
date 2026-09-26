#include "fem_solve.h"

#include "fem_elem.h"

#include "json.h"

#include <algorithm>
#include <cmath>

namespace fem {
namespace {

inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

constexpr double kHexCorner[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                     {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};

// THE ELEMENT MATHS LIVES IN fem_elem.cpp NOW, NOT HERE. Part 0.6 wrote
// Hex8's shape functions, Jacobian and strain-displacement matrix into
// this file, which was right for one element and became a duplicate the
// moment Part H.1 built the library. Two copies of the same shape
// functions is two places for a sign to be wrong and one of them to be
// noticed.
//
// The wrappers below keep this file's own shapes, because its callers
// pass fixed-size arrays and a Model rather than vectors and an
// AnalysisModel. Retiring those callers is what replacing `fem::Model`
// with `AnalysisModel` will do; until then this is a translation layer
// and nothing more.
void HexShape(double xi, double eta, double zeta, double n[8], double dn_ref[8][3]) {
    std::vector<double> shape;
    std::vector<double> derivative;
    ShapeFunctions(ElementShape::Hex8, cad::Vec3d{xi, eta, zeta}, &shape, &derivative);
    for (int i = 0; i < 8; ++i) {
        n[i] = shape[Idx(i)];
        for (int c = 0; c < 3; ++c) dn_ref[i][c] = derivative[Idx(i * 3 + c)];
    }
}

double HexJacobian(const Model &model, const Element &element, const double dn_ref[8][3],
                   double dn_xyz[8][3]) {
    std::vector<cad::Vec3d> nodes;
    for (int i = 0; i < 8; ++i) nodes.push_back(model.nodes[Idx(element.nodes[Idx(i)])]);
    std::vector<double> flat(24, 0.0);
    for (int i = 0; i < 8; ++i) {
        for (int c = 0; c < 3; ++c) flat[Idx(i * 3 + c)] = dn_ref[i][c];
    }
    std::vector<double> physical;
    const double determinant = ElementJacobian(ElementShape::Hex8, nodes, flat, &physical);
    for (int i = 0; i < 8; ++i) {
        for (int c = 0; c < 3; ++c) dn_xyz[i][c] = physical[Idx(i * 3 + c)];
    }
    return determinant;
}

void StrainDisplacement(const double dn_xyz[8][3], double b[6][24]) {
    std::vector<double> flat(24, 0.0);
    for (int i = 0; i < 8; ++i) {
        for (int c = 0; c < 3; ++c) flat[Idx(i * 3 + c)] = dn_xyz[i][c];
    }
    std::vector<double> matrix;
    fem::StrainDisplacement(ElementShape::Hex8, flat, &matrix);
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 24; ++c) b[r][c] = matrix[Idx(r * 24 + c)];
    }
}

// Stress at one reference point, from the element's nodal displacements.
StressTensor StressAt(const Model &model, const Element &element, const std::vector<double> &displacement,
                      const double d_matrix[6][6], double xi, double eta, double zeta) {
    double n[8];
    double dn_ref[8][3];
    double dn_xyz[8][3];
    HexShape(xi, eta, zeta, n, dn_ref);
    HexJacobian(model, element, dn_ref, dn_xyz);
    double b[6][24];
    StrainDisplacement(dn_xyz, b);

    double local[24];
    for (int i = 0; i < 8; ++i) {
        const int node = element.nodes[Idx(i)];
        for (int a = 0; a < 3; ++a) local[i * 3 + a] = displacement[Idx(node * 3 + a)];
    }
    double strain[6] = {0, 0, 0, 0, 0, 0};
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 24; ++c) strain[r] += b[r][c] * local[c];
    }
    StressTensor stress;
    for (int r = 0; r < 6; ++r) {
        double sum = 0.0;
        for (int c = 0; c < 6; ++c) sum += d_matrix[r][c] * strain[c];
        stress.s[r] = sum;
    }
    return stress;
}

}  // namespace

double StressTensor::VonMises() const {
    const double sxx = s[0], syy = s[1], szz = s[2], sxy = s[3], syz = s[4], szx = s[5];
    const double deviatoric = 0.5 * ((sxx - syy) * (sxx - syy) + (syy - szz) * (syy - szz) + (szz - sxx) * (szz - sxx));
    return std::sqrt(deviatoric + 3.0 * (sxy * sxy + syz * syz + szx * szx));
}

void StressTensor::Principal(double *out_sorted_three) const {
    // Eigenvalues of the symmetric 3x3 stress tensor, by the closed-form
    // trigonometric solution of its characteristic cubic. Closed form
    // rather than an iterative eigensolver because this runs once per
    // node on every result and the 3x3 symmetric case has an exact,
    // well-conditioned formula.
    const double sxx = s[0], syy = s[1], szz = s[2], sxy = s[3], syz = s[4], szx = s[5];
    const double p1 = sxy * sxy + syz * syz + szx * szx;
    if (p1 == 0.0) {
        // Already diagonal.
        out_sorted_three[0] = sxx;
        out_sorted_three[1] = syy;
        out_sorted_three[2] = szz;
        std::sort(out_sorted_three, out_sorted_three + 3, std::greater<double>());
        return;
    }
    const double trace_third = (sxx + syy + szz) / 3.0;
    const double q = trace_third;
    const double p2 = (sxx - q) * (sxx - q) + (syy - q) * (syy - q) + (szz - q) * (szz - q) + 2.0 * p1;
    const double p = std::sqrt(p2 / 6.0);
    // B = (A - qI)/p has determinant r; its eigenvalues are 2cos of
    // evenly-spaced angles.
    const double b11 = (sxx - q) / p, b22 = (syy - q) / p, b33 = (szz - q) / p;
    const double b12 = sxy / p, b23 = syz / p, b13 = szx / p;
    const double r = 0.5 * (b11 * (b22 * b33 - b23 * b23) - b12 * (b12 * b33 - b23 * b13) +
                            b13 * (b12 * b23 - b22 * b13));
    const double r_clamped = cad::Clamp(r, -1.0, 1.0);
    const double phi = std::acos(r_clamped) / 3.0;
    out_sorted_three[0] = q + 2.0 * p * std::cos(phi);
    out_sorted_three[2] = q + 2.0 * p * std::cos(phi + 2.0 * cad::kPi / 3.0);
    out_sorted_three[1] = 3.0 * q - out_sorted_three[0] - out_sorted_three[2];  // trace is invariant
}

void ConstitutiveMatrix(const Material &material, double out[6][6]) {
    const double e = material.youngs_modulus;
    const double nu = material.poissons_ratio;
    const double factor = e / ((1.0 + nu) * (1.0 - 2.0 * nu));
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 6; ++c) out[r][c] = 0.0;
    }
    out[0][0] = out[1][1] = out[2][2] = factor * (1.0 - nu);
    out[0][1] = out[0][2] = out[1][0] = out[1][2] = out[2][0] = out[2][1] = factor * nu;
    // Shear terms use the *engineering* shear strain convention (gamma =
    // 2*epsilon), which is what the B matrix above produces -- hence G,
    // not 2G, on the diagonal here.
    const double shear = e / (2.0 * (1.0 + nu));
    out[3][3] = out[4][4] = out[5][5] = shear;
}

void Hex8Stiffness(const Model &model, const Element &element, double out[24][24]) {
    std::vector<cad::Vec3d> nodes;
    for (int i = 0; i < 8; ++i) nodes.push_back(model.nodes[Idx(element.nodes[Idx(i)])]);
    std::vector<double> k;
    std::string error;
    for (int r = 0; r < 24; ++r) {
        for (int c = 0; c < 24; ++c) out[r][c] = 0.0;
    }
    // A failure here is an element Model::Validate should already have
    // rejected -- inverted, or a material that is not one -- so it leaves
    // the matrix zero rather than inventing a diagnosis this signature has
    // no way to return.
    if (!ElementStiffness(ElementShape::Hex8, nodes, model.material, {}, &k, &error)) return;
    for (int r = 0; r < 24; ++r) {
        for (int c = 0; c < 24; ++c) out[r][c] = k[Idx(r * 24 + c)];
    }
}

num::SparseMatrix AssembleStiffness(const Model &model) {
    std::vector<num::Triplet> triplets;
    // 576 entries per element, exactly; reserving avoids a dozen
    // reallocations of a vector that gets very large.
    triplets.reserve(model.elements.size() * 24 * 24);
    for (const Element &element : model.elements) {
        double ke[24][24];
        Hex8Stiffness(model, element, ke);
        int dof[24];
        for (int i = 0; i < 8; ++i) {
            for (int a = 0; a < 3; ++a) dof[i * 3 + a] = element.nodes[Idx(i)] * 3 + a;
        }
        for (int r = 0; r < 24; ++r) {
            for (int c = 0; c < 24; ++c) {
                if (ke[r][c] != 0.0) triplets.push_back(num::Triplet{dof[r], dof[c], ke[r][c]});
            }
        }
    }
    return num::SparseMatrix::FromTriplets(model.DofCount(), model.DofCount(), triplets);
}

Result Solve(const Model &model, const SolveOptions &options) {
    Result result;
    auto report = [&options](double fraction, const std::string &stage) {
        if (options.progress) options.progress(fraction, stage);
    };

    std::string error;
    if (!model.Validate(&error)) {
        result.error = error;
        return result;
    }
    report(0.0, "validating");

    const int dof_count = model.DofCount();
    report(0.05, "assembling");
    const num::SparseMatrix stiffness_free = AssembleStiffness(model);

    // Applied load vector.
    std::vector<double> force(Idx(dof_count), 0.0);
    for (const NodalLoad &load : model.loads) {
        force[Idx(load.node * 3 + 0)] += load.force.x;
        force[Idx(load.node * 3 + 1)] += load.force.y;
        force[Idx(load.node * 3 + 2)] += load.force.z;
    }

    // Constraints by elimination rather than Lagrange multipliers: zero
    // the constrained rows and columns, put 1 on the diagonal, and move
    // the prescribed value's contribution to the right-hand side. This
    // keeps the matrix positive definite, which is what lets the LDL^T
    // factorization work without pivoting (see SparseLDLT's own note on
    // why saddle-point systems are out of scope).
    std::vector<int> is_fixed(Idx(dof_count), 0);
    std::vector<double> prescribed(Idx(dof_count), 0.0);
    for (const Constraint &c : model.constraints) {
        for (int a = 0; a < 3; ++a) {
            if (!c.fixed[a]) continue;
            const int dof = c.node * 3 + a;
            is_fixed[Idx(dof)] = 1;
            prescribed[Idx(dof)] = c.value[a];
        }
    }

    // Move the prescribed displacements' contributions across first,
    // using the *unmodified* matrix -- afterwards its constrained columns
    // are gone and the information with them.
    std::vector<double> rhs = force;
    {
        std::vector<double> k_times_prescribed;
        stiffness_free.Multiply(prescribed, &k_times_prescribed);
        for (int i = 0; i < dof_count; ++i) {
            if (!is_fixed[Idx(i)]) rhs[Idx(i)] -= k_times_prescribed[Idx(i)];
        }
    }
    std::vector<num::Triplet> constrained;
    constrained.reserve(stiffness_free.Values().size());
    const std::vector<int> &row_start = stiffness_free.RowStart();
    const std::vector<int> &col_index = stiffness_free.ColIndex();
    const std::vector<double> &values = stiffness_free.Values();
    for (int r = 0; r < dof_count; ++r) {
        if (is_fixed[Idx(r)]) {
            // The constrained row becomes `diagonal * u_r = diagonal *
            // prescribed_r`, which has the same solution as putting a 1 on
            // the diagonal but leaves the matrix well scaled. That matters:
            // stiffness diagonals here are of order 1e11, so a 1.0 among
            // them spans eleven orders of magnitude for no reason, and it
            // makes a *relative* singularity test -- the thing that catches
            // an under-constrained model -- impossible to set a threshold
            // for. Reusing the row's own original diagonal needs no
            // arbitrary scale factor and is exactly the right magnitude.
            const double diagonal = stiffness_free.At(r, r);
            const double pivot = (diagonal > 0.0) ? diagonal : 1.0;
            constrained.push_back(num::Triplet{r, r, pivot});
            rhs[Idx(r)] = pivot * prescribed[Idx(r)];
            continue;
        }
        for (int p = row_start[Idx(r)]; p < row_start[Idx(r) + 1]; ++p) {
            const int c = col_index[Idx(p)];
            if (is_fixed[Idx(c)]) continue;
            constrained.push_back(num::Triplet{r, c, values[Idx(p)]});
        }
    }
    const num::SparseMatrix stiffness = num::SparseMatrix::FromTriplets(dof_count, dof_count, constrained);

    report(0.15, "solving");
    std::vector<double> displacement;
    if (options.solver == SolverKind::Direct) {
        num::SparseLDLT ldlt;
        if (!ldlt.Factorize(stiffness, options.ordering)) {
            result.error = "factorization failed: " + ldlt.Error();
            return result;
        }
        if (!ldlt.Solve(rhs, &displacement)) {
            result.error = "solve failed after a successful factorization (this should not happen)";
            return result;
        }
        result.solver_used = "direct LDL^T";
    } else {
        num::IterativeOptions iterative;
        iterative.tolerance = options.iterative_tolerance;
        iterative.max_iterations = options.iterative_max_iterations;
        iterative.preconditioner = num::Preconditioner::IncompleteCholesky;
        const num::IterativeResult r = num::ConjugateGradient(stiffness, rhs, &displacement, iterative);
        result.solver_iterations = r.iterations;
        result.solver_used = "conjugate gradients";
        if (!r.converged) {
            result.error = "the iterative solver did not converge: " + r.message + " (relative residual " +
                           std::to_string(r.relative_residual) + ")";
            return result;
        }
    }

    report(0.7, "recovering stress");
    result.displacements.assign(model.nodes.size(), cad::Vec3d{});
    for (int n = 0; n < model.NodeCount(); ++n) {
        result.displacements[Idx(n)] =
            cad::Vec3d{displacement[Idx(n * 3 + 0)], displacement[Idx(n * 3 + 1)], displacement[Idx(n * 3 + 2)]};
        const double magnitude = result.displacements[Idx(n)].Length();
        if (magnitude > result.max_displacement_magnitude) {
            result.max_displacement_magnitude = magnitude;
            result.max_displacement_node = n;
        }
    }

    double d_matrix[6][6];
    ConstitutiveMatrix(model.material, d_matrix);

    // Element-centroid stress, unaveraged.
    result.element_stress.assign(model.elements.size(), StressTensor{});
    for (std::size_t e = 0; e < model.elements.size(); ++e) {
        result.element_stress[e] = StressAt(model, model.elements[e], displacement, d_matrix, 0.0, 0.0, 0.0);
    }

    // Nodal stress: each element evaluates the stress at each of its own
    // corners, and every element touching a node contributes to that
    // node's average.
    //
    // Evaluating at the corners rather than extrapolating from the Gauss
    // points is a simplification. The Gauss points are the
    // superconvergent locations for this element -- stress is most
    // accurate there and least accurate at the corners -- so the proper
    // recovery extrapolates outward from them, and Part J.1 will. What is
    // here is correct but less accurate near boundaries, and the
    // difference vanishes under refinement.
    result.nodal_stress.assign(model.nodes.size(), StressTensor{});
    std::vector<int> contributions(model.nodes.size(), 0);
    for (const Element &element : model.elements) {
        for (int c = 0; c < 8; ++c) {
            const StressTensor stress = StressAt(model, element, displacement, d_matrix, kHexCorner[c][0],
                                                 kHexCorner[c][1], kHexCorner[c][2]);
            const int node = element.nodes[Idx(c)];
            for (int i = 0; i < 6; ++i) result.nodal_stress[Idx(node)].s[i] += stress.s[i];
            ++contributions[Idx(node)];
        }
    }
    for (std::size_t n = 0; n < result.nodal_stress.size(); ++n) {
        if (contributions[n] == 0) continue;
        const double inverse = 1.0 / static_cast<double>(contributions[n]);
        for (int i = 0; i < 6; ++i) result.nodal_stress[n].s[i] *= inverse;
        const double von_mises = result.nodal_stress[n].VonMises();
        if (von_mises > result.max_von_mises) {
            result.max_von_mises = von_mises;
            result.max_von_mises_node = static_cast<int>(n);
        }
    }

    // Reactions, from the unconstrained matrix: R = K*u - f, which is
    // non-zero only where a constraint supplied the missing force.
    report(0.9, "computing reactions");
    std::vector<double> internal;
    stiffness_free.Multiply(displacement, &internal);
    result.reactions.assign(model.nodes.size(), cad::Vec3d{});
    cad::Vec3d reaction_total;
    cad::Vec3d applied_total;
    for (int n = 0; n < model.NodeCount(); ++n) {
        cad::Vec3d r;
        for (int a = 0; a < 3; ++a) {
            const int dof = n * 3 + a;
            if (!is_fixed[Idx(dof)]) continue;
            const double value = internal[Idx(dof)] - force[Idx(dof)];
            if (a == 0) r.x = value;
            if (a == 1) r.y = value;
            if (a == 2) r.z = value;
        }
        result.reactions[Idx(n)] = r;
        reaction_total += r;
    }
    for (const NodalLoad &load : model.loads) applied_total += load.force;

    // Global equilibrium: the reactions must balance the applied load
    // exactly, to round-off. This is nearly free and it catches a
    // surprising range of mistakes -- a mis-assembled element, a
    // constraint applied to the wrong degree of freedom, a load vector
    // built in the wrong units -- none of which a residual check on the
    // constrained system would notice, because that system is
    // self-consistent by construction.
    const cad::Vec3d imbalance = reaction_total + applied_total;
    const double scale = std::max(applied_total.Length(), 1e-30);
    result.equilibrium_residual = imbalance.Length() / scale;

    report(1.0, "done");
    result.ok = true;
    return result;
}

std::string ResultToJson(const Result &result) {
    Json root;
    root["ok"] = Json(result.ok);
    if (!result.error.empty()) root["error"] = Json(result.error);
    if (!result.ok) return root.dump();

    Json displacements;
    for (const cad::Vec3d &d : result.displacements) {
        displacements.push_back(Json(d.x));
        displacements.push_back(Json(d.y));
        displacements.push_back(Json(d.z));
    }
    root["displacements"] = displacements;

    // Von Mises per node rather than the full tensor: it is what the
    // colour map in Part J.2 consumes, and shipping six components per
    // node would multiply the message size by six for data nothing yet
    // reads. The tensors stay available in-process for anything that
    // needs them.
    Json von_mises;
    for (const StressTensor &s : result.nodal_stress) von_mises.push_back(Json(s.VonMises()));
    root["von_mises"] = von_mises;

    Json summary;
    summary["max_displacement"] = Json(result.max_displacement_magnitude);
    summary["max_displacement_node"] = Json(static_cast<double>(result.max_displacement_node));
    summary["max_von_mises"] = Json(result.max_von_mises);
    summary["max_von_mises_node"] = Json(static_cast<double>(result.max_von_mises_node));
    summary["equilibrium_residual"] = Json(result.equilibrium_residual);
    summary["solver"] = Json(result.solver_used);
    summary["iterations"] = Json(static_cast<double>(result.solver_iterations));
    root["summary"] = summary;
    return root.dump();
}

}  // namespace fem
