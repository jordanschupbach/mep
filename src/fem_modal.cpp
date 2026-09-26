#include "fem_modal.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fem {
namespace {

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

constexpr double kTwoPi = 6.28318530717958647692;

// Eigenvalues and vectors of a small dense symmetric matrix, by cyclic
// Jacobi rotations. Written here rather than reached for because the
// projected problem is a few dozen unknowns at most, and a routine whose
// whole content is "rotate until the off-diagonal is gone" has nowhere
// for a subtle error to hide.
void SymmetricEigen(std::vector<double> a, int n, std::vector<double> *values,
                    std::vector<double> *vectors) {
    vectors->assign(Idx(n * n), 0.0);
    for (int i = 0; i < n; ++i) (*vectors)[Idx(i * n + i)] = 1.0;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) off += a[Idx(i * n + j)] * a[Idx(i * n + j)];
        }
        if (off < 1e-30) break;
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                const double apq = a[Idx(p * n + q)];
                if (std::fabs(apq) < 1e-300) continue;
                const double theta = (a[Idx(q * n + q)] - a[Idx(p * n + p)]) / (2.0 * apq);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                 (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int k = 0; k < n; ++k) {
                    const double akp = a[Idx(k * n + p)];
                    const double akq = a[Idx(k * n + q)];
                    a[Idx(k * n + p)] = c * akp - s * akq;
                    a[Idx(k * n + q)] = s * akp + c * akq;
                }
                for (int k = 0; k < n; ++k) {
                    const double apk = a[Idx(p * n + k)];
                    const double aqk = a[Idx(q * n + k)];
                    a[Idx(p * n + k)] = c * apk - s * aqk;
                    a[Idx(q * n + k)] = s * apk + c * aqk;
                }
                for (int k = 0; k < n; ++k) {
                    const double vkp = (*vectors)[Idx(k * n + p)];
                    const double vkq = (*vectors)[Idx(k * n + q)];
                    (*vectors)[Idx(k * n + p)] = c * vkp - s * vkq;
                    (*vectors)[Idx(k * n + q)] = s * vkp + c * vkq;
                }
            }
        }
    }
    values->assign(Idx(n), 0.0);
    for (int i = 0; i < n; ++i) (*values)[Idx(i)] = a[Idx(i * n + i)];
}

double Dot(const std::vector<double> &a, const std::vector<double> &b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) sum += a[i] * b[i];
    return sum;
}

}  // namespace

bool AssembleMass(const AnalysisModel &model, const SparsityPattern &pattern, bool lumped,
                  num::SparseMatrix *out, std::string *error) {
    error->clear();
    const int dofs = pattern.Dofs();
    std::vector<double> values(Idx(pattern.NonZeros()), 0.0);
    std::vector<QuadraturePoint> rule;
    std::vector<double> n;
    std::vector<double> dn;
    std::vector<double> dn_xyz;
    std::vector<cad::Vec3d> corner;
    for (const BoundElement &element : model.elements) {
        if (element.material < 0 ||
            element.material >= static_cast<int>(model.materials.size())) {
            *error = "an element names a material the model does not have";
            return false;
        }
        const double density = model.materials[Idx(element.material)].density;
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model.nodes[Idx(node)]);
        // A degree above the stiffness's rule: N N^T is twice the shape
        // functions' order where B^T D B is twice their derivatives'.
        if (!Quadrature(element.shape, ElementNodeCount(element.shape) > 8 ? 4 : 3, &rule, error)) {
            return false;
        }
        const int count = static_cast<int>(element.nodes.size());
        for (const QuadraturePoint &point : rule) {
            ShapeFunctions(element.shape, point.at, &n, &dn);
            const double determinant = ElementJacobian(element.shape, corner, dn, &dn_xyz);
            if (!(determinant > 0.0)) {
                *error = "an element is turned inside out where the mass was integrated";
                return false;
            }
            const double weight = determinant * point.weight * density;
            for (int a = 0; a < count; ++a) {
                for (int b = 0; b < count; ++b) {
                    // LUMPED BY ROW SUM, which for these elements is the
                    // same as the usual scaling and is exact in one
                    // respect that matters: the total mass is preserved,
                    // because summing every row of the consistent matrix
                    // and putting it on the diagonal moves mass around
                    // without creating or destroying any.
                    const double value = n[Idx(a)] * n[Idx(b)] * weight;
                    const int target = lumped ? a : b;
                    for (int axis = 0; axis < 3; ++axis) {
                        const int row = element.nodes[Idx(a)] * 3 + axis;
                        const int column = element.nodes[Idx(target)] * 3 + axis;
                        const int at = pattern.Find(row, column);
                        if (at < 0) {
                            *error = "the sparsity pattern has no entry the mass matrix needs";
                            return false;
                        }
                        values[Idx(at)] += value;
                    }
                }
            }
        }
    }
    std::vector<num::Triplet> triplets;
    triplets.reserve(Idx(pattern.NonZeros()));
    for (int row = 0; row < dofs; ++row) {
        for (int at = pattern.RowStart()[Idx(row)]; at < pattern.RowStart()[Idx(row + 1)]; ++at) {
            if (values[Idx(at)] == 0.0) continue;
            triplets.push_back(num::Triplet{row, pattern.ColIndex()[Idx(at)], values[Idx(at)]});
        }
    }
    *out = num::SparseMatrix::FromTriplets(dofs, dofs, triplets);
    return true;
}

namespace {

// K and M reduced onto the free degrees of freedom, which is what the
// eigenproblem is posed on: a prescribed degree of freedom does not
// vibrate.
struct Reduced {
    bool ok = false;
    std::string error;
    num::SparseMatrix stiffness;
    num::SparseMatrix mass;
    std::vector<int> dof_of_row;
    int size = 0;
};

Reduced Reduce(const AnalysisModel &model, const std::vector<MultiPointConstraint> &constraints,
               bool lumped) {
    Reduced out;
    System system;
    AssemblyOptions assembly;
    if (!BuildSystem(model, constraints, assembly, &system, &out.error)) return out;
    if (system.multipliers > 0) {
        out.error = "multi-point constraints in a modal analysis need the mass matrix projected "
                    "onto the constraint's null space, which is not written; use elimination or "
                    "a penalty for now";
        return out;
    }
    SparsityPattern pattern;
    if (!pattern.Build(model, &out.error)) return out;
    num::SparseMatrix mass;
    if (!AssembleMass(model, pattern, lumped, &mass, &out.error)) return out;

    out.dof_of_row = system.dof_of_row;
    out.size = static_cast<int>(system.dof_of_row.size());
    std::vector<int> row_of(Idx(pattern.Dofs()), -1);
    for (int i = 0; i < out.size; ++i) row_of[Idx(system.dof_of_row[Idx(i)])] = i;
    std::vector<num::Triplet> triplets;
    for (int row = 0; row < mass.Rows(); ++row) {
        const int reduced_row = row_of[Idx(row)];
        if (reduced_row < 0) continue;
        for (int at = mass.RowStart()[Idx(row)]; at < mass.RowStart()[Idx(row + 1)]; ++at) {
            const int reduced_column = row_of[Idx(mass.ColIndex()[Idx(at)])];
            if (reduced_column < 0) continue;
            triplets.push_back(
                num::Triplet{reduced_row, reduced_column, mass.Values()[Idx(at)]});
        }
    }
    out.mass = num::SparseMatrix::FromTriplets(out.size, out.size, triplets);
    out.stiffness = system.matrix;
    out.ok = true;
    return out;
}

}  // namespace

bool CountModesBelow(const AnalysisModel &model,
                     const std::vector<MultiPointConstraint> &constraints, double frequency,
                     bool lumped_mass, int *out, std::string *error) {
    *out = 0;
    const Reduced reduced = Reduce(model, constraints, lumped_mass);
    if (!reduced.ok) {
        *error = reduced.error;
        return false;
    }
    const double sigma = (kTwoPi * frequency) * (kTwoPi * frequency);
    std::vector<num::Triplet> triplets;
    for (int row = 0; row < reduced.size; ++row) {
        for (int at = reduced.stiffness.RowStart()[Idx(row)];
             at < reduced.stiffness.RowStart()[Idx(row + 1)]; ++at) {
            triplets.push_back(num::Triplet{row, reduced.stiffness.ColIndex()[Idx(at)],
                                            reduced.stiffness.Values()[Idx(at)]});
        }
        for (int at = reduced.mass.RowStart()[Idx(row)];
             at < reduced.mass.RowStart()[Idx(row + 1)]; ++at) {
            triplets.push_back(num::Triplet{row, reduced.mass.ColIndex()[Idx(at)],
                                            -sigma * reduced.mass.Values()[Idx(at)]});
        }
    }
    const num::SparseMatrix shifted =
        num::SparseMatrix::FromTriplets(reduced.size, reduced.size, triplets);
    num::SparseLDLT factor;
    // A loose singularity tolerance: the shifted matrix is *meant* to be
    // indefinite, and a pivot near zero means an eigenvalue near the
    // shift, which is information rather than failure.
    if (!factor.Factorize(shifted, num::Ordering::ApproximateMinimumDegree, 1e-300)) {
        *error = factor.Error();
        return false;
    }
    // SYLVESTER'S LAW OF INERTIA: the number of negative pivots of
    // `K - sigma M` is the number of eigenvalues below sigma, whatever
    // ordering the factorization used and whatever the pivots themselves
    // are. That is what makes this an independent check rather than a
    // restatement of the iteration.
    *out = factor.NegativeEigenvalueCount();
    return true;
}

bool SolveModal(const AnalysisModel &model, const std::vector<MultiPointConstraint> &constraints,
                const ModalOptions &options, ModalResult *out) {
    *out = ModalResult{};
    if (options.method == ModalMethod::Lanczos) {
        // SAID RATHER THAN SILENTLY SUBSTITUTED. An option that is
        // accepted and ignored is worse than one that is refused: a caller
        // who asked for Lanczos because subspace iteration was too slow
        // would get subspace iteration and the same run time, and conclude
        // the method makes no difference.
        out->error = "Lanczos is not written yet; subspace iteration is what this solves with, "
                     "and asking for the other one gets this message rather than the same answer "
                     "under a different name";
        return false;
    }
    const Reduced reduced = Reduce(model, constraints, options.lumped_mass);
    if (!reduced.ok) {
        out->error = reduced.error;
        return false;
    }
    const int size = reduced.size;
    const int wanted = std::max(1, std::min(options.modes, size));

    // A SHIFT BELOW THE LOWEST MODE, NOT AT ZERO, when the model is free.
    // An unrestrained structure has six zero eigenvalues, so `K` is
    // singular and `K - 0*M` cannot be factored; shifting a little below
    // zero makes it definite and leaves the rigid modes where they are,
    // at an eigenvalue the solver then reports as zero.
    double sigma = (kTwoPi * options.around_frequency) * (kTwoPi * options.around_frequency);
    double trace_k = 0.0;
    double trace_m = 0.0;
    for (int i = 0; i < size; ++i) {
        trace_k += reduced.stiffness.At(i, i);
        trace_m += reduced.mass.At(i, i);
    }
    const double typical = trace_m > 0.0 ? trace_k / trace_m : 1.0;
    if (options.around_frequency == 0.0) sigma = -typical * 1e-6;

    std::vector<num::Triplet> triplets;
    for (int row = 0; row < size; ++row) {
        for (int at = reduced.stiffness.RowStart()[Idx(row)];
             at < reduced.stiffness.RowStart()[Idx(row + 1)]; ++at) {
            triplets.push_back(num::Triplet{row, reduced.stiffness.ColIndex()[Idx(at)],
                                            reduced.stiffness.Values()[Idx(at)]});
        }
        for (int at = reduced.mass.RowStart()[Idx(row)];
             at < reduced.mass.RowStart()[Idx(row + 1)]; ++at) {
            triplets.push_back(num::Triplet{row, reduced.mass.ColIndex()[Idx(at)],
                                            -sigma * reduced.mass.Values()[Idx(at)]});
        }
    }
    const num::SparseMatrix shifted = num::SparseMatrix::FromTriplets(size, size, triplets);
    num::SparseLDLT factor;
    if (!factor.Factorize(shifted, options.ordering, 1e-300)) {
        out->error = factor.Error();
        return false;
    }

    auto multiply_mass = [&](const std::vector<double> &x, std::vector<double> *y) {
        reduced.mass.Multiply(x, y);
    };
    auto apply = [&](const std::vector<double> &x, std::vector<double> *y) {
        std::vector<double> rhs;
        multiply_mass(x, &rhs);
        return factor.Solve(rhs, y);
    };

    // The subspace: a few more vectors than modes wanted, which is what
    // makes the wanted ones converge rather than the whole block crawl.
    const int block = std::min(size, std::max(wanted + 4, wanted * 2));
    std::vector<std::vector<double>> basis(Idx(block), std::vector<double>(Idx(size), 0.0));
    for (int j = 0; j < block; ++j) {
        for (int i = 0; i < size; ++i) {
            // Deterministic and not an eigenvector of anything: a starting
            // block that happened to be M-orthogonal to a mode would miss
            // it entirely, which is the silent failure this part is about.
            basis[Idx(j)][Idx(i)] =
                std::sin(1.0 + static_cast<double>(i) * 0.7 + static_cast<double>(j) * 1.3);
        }
    }

    std::vector<double> previous(Idx(block), 0.0);
    std::vector<double> values;
    std::vector<double> vectors;
    for (int iteration = 0; iteration < std::max(1, options.max_iterations); ++iteration) {
        out->iterations = iteration + 1;
        for (int j = 0; j < block; ++j) {
            std::vector<double> next;
            if (!apply(basis[Idx(j)], &next)) {
                out->error = "the shifted factorization would not solve";
                return false;
            }
            basis[Idx(j)] = next;
        }
        // M-orthonormalise, by modified Gram-Schmidt in the M inner
        // product -- which is the one the eigenproblem is posed in, and
        // using the Euclidean one instead is a classic way to get modes
        // that are nearly right and not orthogonal.
        for (int j = 0; j < block; ++j) {
            for (int pass = 0; pass < 2; ++pass) {
                for (int i = 0; i < j; ++i) {
                    std::vector<double> mi;
                    multiply_mass(basis[Idx(i)], &mi);
                    const double overlap = Dot(basis[Idx(j)], mi);
                    for (int k = 0; k < size; ++k) {
                        basis[Idx(j)][Idx(k)] -= overlap * basis[Idx(i)][Idx(k)];
                    }
                }
            }
            std::vector<double> mj;
            multiply_mass(basis[Idx(j)], &mj);
            const double norm = std::sqrt(std::max(Dot(basis[Idx(j)], mj), 0.0));
            if (!(norm > 0.0)) {
                for (int k = 0; k < size; ++k) {
                    basis[Idx(j)][Idx(k)] = std::sin(2.0 + static_cast<double>(k * (j + 3)));
                }
                continue;
            }
            for (int k = 0; k < size; ++k) basis[Idx(j)][Idx(k)] /= norm;
        }

        // The projected problem: K and M restricted to the subspace.
        std::vector<double> small_k(Idx(block * block), 0.0);
        std::vector<double> small_m(Idx(block * block), 0.0);
        for (int j = 0; j < block; ++j) {
            std::vector<double> kj;
            std::vector<double> mj;
            reduced.stiffness.Multiply(basis[Idx(j)], &kj);
            multiply_mass(basis[Idx(j)], &mj);
            for (int i = 0; i < block; ++i) {
                small_k[Idx(i * block + j)] = Dot(basis[Idx(i)], kj);
                small_m[Idx(i * block + j)] = Dot(basis[Idx(i)], mj);
            }
        }
        // M is the identity after orthonormalising, up to round-off, so
        // the projected problem is a plain symmetric one.
        SymmetricEigen(small_k, block, &values, &vectors);
        std::vector<int> order(Idx(block));
        for (int i = 0; i < block; ++i) order[Idx(i)] = i;
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return values[Idx(a)] < values[Idx(b)]; });

        std::vector<std::vector<double>> rotated(Idx(block), std::vector<double>(Idx(size), 0.0));
        for (int j = 0; j < block; ++j) {
            const int from = order[Idx(j)];
            for (int i = 0; i < block; ++i) {
                const double weight = vectors[Idx(i * block + from)];
                if (weight == 0.0) continue;
                for (int k = 0; k < size; ++k) {
                    rotated[Idx(j)][Idx(k)] += weight * basis[Idx(i)][Idx(k)];
                }
            }
        }
        basis.swap(rotated);

        double worst = 0.0;
        for (int j = 0; j < wanted; ++j) {
            const double value = values[Idx(order[Idx(j)])];
            const double reference = std::max(std::fabs(value), typical * 1e-12);
            worst = std::max(worst, std::fabs(value - previous[Idx(j)]) / reference);
            previous[Idx(j)] = value;
        }
        if (iteration > 0 && worst < options.tolerance) break;
    }

    for (int j = 0; j < wanted; ++j) {
        const double value = std::max(0.0, values[Idx(j)] == 0.0 ? 0.0 : values[Idx(j)]);
        out->eigenvalue.push_back(value);
        out->frequency.push_back(std::sqrt(std::max(0.0, value)) / kTwoPi);
        std::vector<cad::Vec3d> shape(model.nodes.size(), cad::Vec3d{});
        for (int i = 0; i < size; ++i) {
            const int dof = reduced.dof_of_row[Idx(i)];
            if (dof < 0) continue;
            const int node = dof / 3;
            const int axis = dof % 3;
            double *component[3] = {&shape[Idx(node)].x, &shape[Idx(node)].y, &shape[Idx(node)].z};
            *component[axis] = basis[Idx(j)][Idx(i)];
        }
        out->shape.push_back(shape);
    }
    // A rigid-body mode is one that *is* a rigid-body motion, and the way
    // to find out is to ask whether it lies in the span of the six.
    //
    // THE OBVIOUS TEST IS A THRESHOLD ON THE FREQUENCY AND IT IS WRONG
    // FOR THIN STRUCTURES. What stood here compared each eigenvalue
    // against `trace(K)/trace(M)`, called that scale-free, and it is not:
    // that ratio is an average dominated by the *stiffest* degrees of
    // freedom, and a thin part has a stiffness range of many orders. A
    // cantilevered strip 10 m long and 0.1 m thick reported its first
    // out-of-plane bending mode -- a perfectly genuine 1.2 Hz mode -- as
    // a rigid-body motion, because the through-thickness stiffness that
    // sets the average is enormous next to the bending stiffness that
    // sets the answer. Telling someone their model is unrestrained when
    // it is merely flexible is a bad way to be wrong: the natural next
    // step is to add constraints that do not belong.
    //
    // A rigid-body motion is a geometric object, not a small number. The
    // six of them are written down exactly by `RigidBodyModes`, and a
    // mode is one of them exactly when it lies in their span -- measured
    // in the mass metric, which is the one the modes are orthonormal in.
    // For a properly restrained model none of the six is admissible, so
    // every projection is near zero and the count is zero, which is the
    // right answer arrived at for the right reason.
    {
        // The six rigid-body motions, written out at each node: three
        // translations and three rotations about the origin. Built here
        // rather than reached for, because they depend on nothing but the
        // node positions.
        std::vector<std::vector<double>> basis_rigid;
        std::vector<double> scratch;
        // Which degrees of freedom the constraints removed, so that a
        // rigid motion they forbid can be discarded rather than clipped.
        std::vector<bool> free_dof(Idx(model.NodeCount() * 3), false);
        for (int i = 0; i < size; ++i) {
            const int dof = reduced.dof_of_row[Idx(i)];
            if (dof >= 0) free_dof[Idx(dof)] = true;
        }
        auto rigid_value = [&](int mode, int dof) {
            const cad::Vec3d &p = model.nodes[Idx(dof / 3)];
            const int axis = dof % 3;
            double motion[3] = {0.0, 0.0, 0.0};
            if (mode < 3) {
                motion[mode] = 1.0;
            } else if (mode == 3) {
                motion[1] = -p.z;
                motion[2] = p.y;
            } else if (mode == 4) {
                motion[0] = p.z;
                motion[2] = -p.x;
            } else {
                motion[0] = -p.y;
                motion[1] = p.x;
            }
            return motion[axis];
        };
        for (int mode = 0; mode < 6; ++mode) {
            // A RIGID MOTION A CONSTRAINT FORBIDS IS NOT ADMISSIBLE AT
            // ALL, and must be dropped rather than clipped to the free
            // degrees of freedom. Zeroing its constrained components
            // leaves a vector that is no longer a rigid motion -- a
            // translation cut off at a symmetry plane is a shear -- and
            // counting projections onto *that* reported rigid-body modes
            // for a cylinder sector that has none.
            double scale = 0.0;
            double blocked = 0.0;
            for (int dof = 0; dof < model.NodeCount() * 3; ++dof) {
                const double value = std::fabs(rigid_value(mode, dof));
                scale = std::max(scale, value);
                if (!free_dof[Idx(dof)]) blocked = std::max(blocked, value);
            }
            if (blocked > std::max(scale, 1.0) * 1e-9) continue;
            std::vector<double> direction(Idx(size), 0.0);
            for (int i = 0; i < size; ++i) {
                const int dof = reduced.dof_of_row[Idx(i)];
                if (dof < 0) continue;
                direction[Idx(i)] = rigid_value(mode, dof);
            }
            // Mass-orthonormalised against the ones already kept, and
            // dropped if the constraints have removed it -- a model held
            // on a face has none of the six left, one held at a point
            // still has three rotations.
            for (const std::vector<double> &already : basis_rigid) {
                reduced.mass.Multiply(already, &scratch);
                double overlap = 0.0;
                for (int i = 0; i < size; ++i) overlap += direction[Idx(i)] * scratch[Idx(i)];
                for (int i = 0; i < size; ++i) direction[Idx(i)] -= overlap * already[Idx(i)];
            }
            reduced.mass.Multiply(direction, &scratch);
            double norm = 0.0;
            for (int i = 0; i < size; ++i) norm += direction[Idx(i)] * scratch[Idx(i)];
            if (!(norm > 1e-30)) continue;
            const double normalise = 1.0 / std::sqrt(norm);
            for (int i = 0; i < size; ++i) direction[Idx(i)] *= normalise;
            basis_rigid.push_back(std::move(direction));
        }
        for (int j = 0; j < wanted; ++j) {
            double inside = 0.0;
            for (const std::vector<double> &direction : basis_rigid) {
                reduced.mass.Multiply(direction, &scratch);
                double overlap = 0.0;
                for (int i = 0; i < size; ++i) overlap += basis[Idx(j)][Idx(i)] * scratch[Idx(i)];
                inside += overlap * overlap;
            }
            reduced.mass.Multiply(basis[Idx(j)], &scratch);
            double total = 0.0;
            for (int i = 0; i < size; ++i) total += basis[Idx(j)][Idx(i)] * scratch[Idx(i)];
            if (total > 0.0 && inside / total > 0.99) ++out->rigid_body_modes;
        }
    }

    if (options.sturm_check && !out->eigenvalue.empty()) {
        // Counted a little above the highest mode found, so that a mode
        // exactly at the boundary is not the thing being asked about.
        const double highest = out->frequency.back() * 1.0001 + 1e-9;
        std::string error;
        if (!CountModesBelow(model, constraints, highest, options.lumped_mass, &out->modes_below,
                             &error)) {
            out->warnings.push_back("the Sturm check could not be made: " + error);
        } else {
            out->sturm_agrees = out->modes_below == static_cast<int>(out->frequency.size());
            if (!out->sturm_agrees) {
                out->warnings.push_back(
                    "the factorization finds " + std::to_string(out->modes_below) +
                    " modes below the highest one returned and " +
                    std::to_string(out->frequency.size()) +
                    " were returned; a mode has been missed, which for close or repeated "
                    "frequencies is what an iterative eigensolver does silently");
            }
        }
    }
    out->ok = true;
    return true;
}

bool AssembleStressStiffness(const AnalysisModel &model, const StaticResult &state,
                             const SparsityPattern &pattern, num::SparseMatrix *out,
                             std::string *error) {
    error->clear();
    if (static_cast<int>(state.displacement.size()) != model.NodeCount()) {
        *error = "the static solution does not cover the model's nodes";
        return false;
    }
    std::vector<double> full(Idx(model.NodeCount() * 3), 0.0);
    for (int node = 0; node < model.NodeCount(); ++node) {
        full[Idx(node * 3 + 0)] = state.displacement[Idx(node)].x;
        full[Idx(node * 3 + 1)] = state.displacement[Idx(node)].y;
        full[Idx(node * 3 + 2)] = state.displacement[Idx(node)].z;
    }
    std::vector<double> values(Idx(pattern.NonZeros()), 0.0);
    std::vector<QuadraturePoint> rule;
    std::vector<double> n;
    std::vector<double> dn;
    std::vector<double> dn_xyz;
    std::vector<cad::Vec3d> corner;
    for (int e = 0; e < model.ElementCount(); ++e) {
        const BoundElement &element = model.elements[Idx(e)];
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model.nodes[Idx(node)]);
        if (!Quadrature(element.shape, 0, &rule, error)) return false;
        const int count = static_cast<int>(element.nodes.size());
        for (const QuadraturePoint &point : rule) {
            ShapeFunctions(element.shape, point.at, &n, &dn);
            const double determinant = ElementJacobian(element.shape, corner, dn, &dn_xyz);
            if (!(determinant > 0.0)) {
                *error = "an element is turned inside out where the stress stiffness was "
                         "integrated";
                return false;
            }
            // THE STRESS AT THE QUADRATURE POINT, not the element average.
            // The stress stiffness is what the stress *field* does to the
            // stiffness, and averaging it first throws away the part that
            // varies -- which for a column in bending is the part that
            // matters.
            StressTensor stress;
            if (!ElementStressAt(model, e, point.at, full, &stress, error)) return false;
            // Voigt back to a tensor.
            const double sigma[3][3] = {{stress.s[0], stress.s[3], stress.s[5]},
                                        {stress.s[3], stress.s[1], stress.s[4]},
                                        {stress.s[5], stress.s[4], stress.s[2]}};
            const double weight = determinant * point.weight;
            for (int a = 0; a < count; ++a) {
                for (int b = 0; b < count; ++b) {
                    double coupling = 0.0;
                    for (int k = 0; k < 3; ++k) {
                        for (int l = 0; l < 3; ++l) {
                            coupling += dn_xyz[Idx(a * 3 + k)] * sigma[k][l] *
                                        dn_xyz[Idx(b * 3 + l)];
                        }
                    }
                    const double value = coupling * weight;
                    if (value == 0.0) continue;
                    // The same scalar on all three diagonal blocks: the
                    // geometric stiffness does not couple the directions.
                    for (int axis = 0; axis < 3; ++axis) {
                        const int row = element.nodes[Idx(a)] * 3 + axis;
                        const int column = element.nodes[Idx(b)] * 3 + axis;
                        const int at = pattern.Find(row, column);
                        if (at < 0) {
                            *error = "the sparsity pattern has no entry the stress stiffness "
                                     "needs";
                            return false;
                        }
                        values[Idx(at)] += value;
                    }
                }
            }
        }
    }
    std::vector<num::Triplet> triplets;
    triplets.reserve(Idx(pattern.NonZeros()));
    for (int row = 0; row < pattern.Dofs(); ++row) {
        for (int at = pattern.RowStart()[Idx(row)]; at < pattern.RowStart()[Idx(row + 1)]; ++at) {
            if (values[Idx(at)] == 0.0) continue;
            triplets.push_back(num::Triplet{row, pattern.ColIndex()[Idx(at)], values[Idx(at)]});
        }
    }
    *out = num::SparseMatrix::FromTriplets(pattern.Dofs(), pattern.Dofs(), triplets);
    return true;
}

bool SolveBuckling(const AnalysisModel &model,
                   const std::vector<MultiPointConstraint> &constraints,
                   const BucklingOptions &options, BucklingResult *out) {
    *out = BucklingResult{};
    if (!SolveStatic(model, constraints, options.statics, &out->statics)) {
        out->error = "the static solve the stress stiffness comes from failed: " +
                     out->statics.error;
        return false;
    }
    SparsityPattern pattern;
    if (!pattern.Build(model, &out->error)) return false;
    num::SparseMatrix geometric;
    if (!AssembleStressStiffness(model, out->statics, pattern, &geometric, &out->error)) {
        return false;
    }

    System system;
    if (!BuildSystem(model, constraints, options.statics.assembly, &system, &out->error)) {
        return false;
    }
    if (system.multipliers > 0) {
        out->error = "multi-point constraints in a buckling analysis need the stress stiffness "
                     "projected onto the constraint's null space, which is not written";
        return false;
    }
    const int size = static_cast<int>(system.dof_of_row.size());
    std::vector<int> row_of(Idx(pattern.Dofs()), -1);
    for (int i = 0; i < size; ++i) row_of[Idx(system.dof_of_row[Idx(i)])] = i;
    std::vector<num::Triplet> triplets;
    for (int row = 0; row < geometric.Rows(); ++row) {
        const int reduced_row = row_of[Idx(row)];
        if (reduced_row < 0) continue;
        for (int at = geometric.RowStart()[Idx(row)]; at < geometric.RowStart()[Idx(row + 1)];
             ++at) {
            const int reduced_column = row_of[Idx(geometric.ColIndex()[Idx(at)])];
            if (reduced_column < 0) continue;
            // Negated here, so that what the iteration sees is `-K_G`.
            triplets.push_back(
                num::Triplet{reduced_row, reduced_column, -geometric.Values()[Idx(at)]});
        }
    }
    const num::SparseMatrix negated = num::SparseMatrix::FromTriplets(size, size, triplets);
    const num::SparseMatrix &stiffness = system.matrix;

    num::SparseLDLT factor;
    if (!factor.Factorize(stiffness, options.ordering)) {
        out->error = factor.Error();
        return false;
    }
    const int wanted = std::max(1, std::min(options.modes, size));
    const int block = std::min(size, std::max(wanted + 4, wanted * 2));
    std::vector<std::vector<double>> basis(Idx(block), std::vector<double>(Idx(size), 0.0));
    for (int j = 0; j < block; ++j) {
        for (int i = 0; i < size; ++i) {
            basis[Idx(j)][Idx(i)] =
                std::sin(1.0 + static_cast<double>(i) * 0.7 + static_cast<double>(j) * 1.3);
        }
    }

    std::vector<double> previous(Idx(block), 0.0);
    std::vector<double> values;
    std::vector<double> vectors;
    std::vector<int> order;
    for (int iteration = 0; iteration < std::max(1, options.max_iterations); ++iteration) {
        out->iterations = iteration + 1;
        for (int j = 0; j < block; ++j) {
            std::vector<double> rhs;
            negated.Multiply(basis[Idx(j)], &rhs);
            std::vector<double> next;
            if (!factor.Solve(rhs, &next)) {
                out->error = "the factorization would not solve";
                return false;
            }
            basis[Idx(j)] = next;
        }
        // Orthonormalised in the *stiffness* inner product, which is the
        // positive definite one here -- the roles of K and M are swapped
        // relative to a modal analysis, and so are the inner products.
        for (int j = 0; j < block; ++j) {
            for (int pass = 0; pass < 2; ++pass) {
                for (int i = 0; i < j; ++i) {
                    std::vector<double> ki;
                    stiffness.Multiply(basis[Idx(i)], &ki);
                    const double overlap = Dot(basis[Idx(j)], ki);
                    for (int k = 0; k < size; ++k) {
                        basis[Idx(j)][Idx(k)] -= overlap * basis[Idx(i)][Idx(k)];
                    }
                }
            }
            std::vector<double> kj;
            stiffness.Multiply(basis[Idx(j)], &kj);
            const double norm = std::sqrt(std::max(Dot(basis[Idx(j)], kj), 0.0));
            if (!(norm > 0.0)) {
                for (int k = 0; k < size; ++k) {
                    basis[Idx(j)][Idx(k)] = std::sin(2.0 + static_cast<double>(k * (j + 3)));
                }
                continue;
            }
            for (int k = 0; k < size; ++k) basis[Idx(j)][Idx(k)] /= norm;
        }
        std::vector<double> small(Idx(block * block), 0.0);
        for (int j = 0; j < block; ++j) {
            std::vector<double> gj;
            negated.Multiply(basis[Idx(j)], &gj);
            for (int i = 0; i < block; ++i) small[Idx(i * block + j)] = Dot(basis[Idx(i)], gj);
        }
        SymmetricEigen(small, block, &values, &vectors);
        order.assign(Idx(block), 0);
        for (int i = 0; i < block; ++i) order[Idx(i)] = i;
        // The largest |mu| first, since lambda = 1/mu and the smallest
        // load factor is what a buckling analysis is for.
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return std::fabs(values[Idx(a)]) > std::fabs(values[Idx(b)]);
        });
        std::vector<std::vector<double>> rotated(Idx(block), std::vector<double>(Idx(size), 0.0));
        for (int j = 0; j < block; ++j) {
            const int from = order[Idx(j)];
            for (int i = 0; i < block; ++i) {
                const double weight = vectors[Idx(i * block + from)];
                if (weight == 0.0) continue;
                for (int k = 0; k < size; ++k) {
                    rotated[Idx(j)][Idx(k)] += weight * basis[Idx(i)][Idx(k)];
                }
            }
        }
        basis.swap(rotated);
        double worst = 0.0;
        for (int j = 0; j < wanted; ++j) {
            const double value = values[Idx(order[Idx(j)])];
            worst = std::max(worst,
                             std::fabs(value - previous[Idx(j)]) / std::max(std::fabs(value), 1e-300));
            previous[Idx(j)] = value;
        }
        if (iteration > 0 && worst < options.tolerance) break;
    }

    for (int j = 0; j < wanted; ++j) {
        const double mu = values[Idx(order[Idx(j)])];
        if (!(std::fabs(mu) > 0.0)) continue;
        out->factor.push_back(1.0 / mu);
        std::vector<cad::Vec3d> shape(model.nodes.size(), cad::Vec3d{});
        for (int i = 0; i < size; ++i) {
            const int dof = system.dof_of_row[Idx(i)];
            if (dof < 0) continue;
            const int node = dof / 3;
            const int axis = dof % 3;
            double *component[3] = {&shape[Idx(node)].x, &shape[Idx(node)].y, &shape[Idx(node)].z};
            *component[axis] = basis[Idx(j)][Idx(i)];
        }
        out->shape.push_back(shape);
    }
    out->ok = true;
    return true;
}

}  // namespace fem
