#include "fem_assemble.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fem {
namespace {

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

}  // namespace

bool ExpandRigidLink(const AnalysisModel &model, const RigidLink &link,
                     std::vector<MultiPointConstraint> *out, std::string *error) {
    if (link.controlling < 0 || link.controlling >= model.NodeCount()) {
        *error = "the rigid link '" + link.label + "' has no controlling node";
        return false;
    }
    for (const int node : link.dependent) {
        if (node < 0 || node >= model.NodeCount()) {
            *error = "the rigid link '" + link.label + "' names a node that does not exist";
            return false;
        }
        if (node == link.controlling) continue;
        for (int axis = 0; axis < 3; ++axis) {
            MultiPointConstraint row;
            row.label = link.label;
            row.terms.push_back(ConstraintTerm{node, axis, 1.0});
            row.terms.push_back(ConstraintTerm{link.controlling, axis, -1.0});
            row.value = 0.0;
            out->push_back(row);
        }
    }
    return true;
}

bool SparsityPattern::Build(const AnalysisModel &model, std::string *error) {
    row_start_.clear();
    col_index_.clear();
    dofs_ = model.NodeCount() * 3;
    if (dofs_ == 0) {
        *error = "the model has no nodes";
        return false;
    }
    // Gathered per row as sets, which is the clear way to say "each
    // column once" and costs a log factor that the fill, being done many
    // times to one pattern, does not care about.
    std::vector<std::set<int>> columns(Idx(dofs_));
    for (const BoundElement &element : model.elements) {
        for (const int node : element.nodes) {
            if (node >= 0 && node < model.NodeCount()) continue;
            *error = "an element refers to node " + std::to_string(node) + ", which does not exist";
            return false;
        }
        for (const int a : element.nodes) {
            for (const int b : element.nodes) {
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        columns[Idx(a * 3 + i)].insert(b * 3 + j);
                    }
                }
            }
        }
    }
    // A node no element uses would give an empty row and a singular
    // matrix; the diagonal is added so the structure is at least square
    // and the failure is the solver's to report rather than the
    // pattern's to hide.
    row_start_.push_back(0);
    for (int row = 0; row < dofs_; ++row) {
        columns[Idx(row)].insert(row);
        for (const int column : columns[Idx(row)]) col_index_.push_back(column);
        row_start_.push_back(static_cast<int>(col_index_.size()));
    }
    return true;
}

int SparsityPattern::Find(int row, int col) const {
    if (row < 0 || row >= dofs_) return -1;
    const int begin = row_start_[Idx(row)];
    const int end = row_start_[Idx(row + 1)];
    const auto first = col_index_.begin() + begin;
    const auto last = col_index_.begin() + end;
    const auto found = std::lower_bound(first, last, col);
    if (found == last || *found != col) return -1;
    return static_cast<int>(found - col_index_.begin());
}

bool AssembleStiffness(const AnalysisModel &model, const SparsityPattern &pattern,
                       num::SparseMatrix *out, std::string *error) {
    error->clear();
    if (pattern.Dofs() != model.NodeCount() * 3) {
        *error = "the sparsity pattern was built for a different model";
        return false;
    }
    std::vector<double> values(Idx(pattern.NonZeros()), 0.0);

    // ONE CONSTITUTIVE MATRIX PER MATERIAL WHEN THE MODEL IS ISOTHERMAL,
    // AND ONE PER ELEMENT WHEN IT IS NOT. Building it costs a matrix
    // inversion for an orthotropic material, so it is worth not doing per
    // element -- but a model carrying a temperature field has a different
    // modulus in every element, and using one average would throw away
    // exactly the variation a thermal-stress problem is about.
    const bool isothermal = model.node_temperature.empty();
    std::vector<std::vector<double>> constitutive(model.materials.size());
    if (isothermal) {
        for (std::size_t i = 0; i < model.materials.size(); ++i) {
            double d[6][6];
            if (!model.materials[i].ConstitutiveMatrix(model.temperature, d, error)) return false;
            constitutive[i].assign(36, 0.0);
            for (int r = 0; r < 6; ++r) {
                for (int c = 0; c < 6; ++c) constitutive[i][Idx(r * 6 + c)] = d[r][c];
            }
        }
    }

    std::vector<cad::Vec3d> corner;
    std::vector<double> k;
    std::vector<int> dof;
    for (const BoundElement &element : model.elements) {
        if (element.material < 0 ||
            element.material >= static_cast<int>(model.materials.size())) {
            *error = "an element names material " + std::to_string(element.material) +
                     ", which the model does not have";
            return false;
        }
        double d[6][6];
        if (isothermal) {
            for (int r = 0; r < 6; ++r) {
                for (int c = 0; c < 6; ++c) {
                    d[r][c] = constitutive[Idx(element.material)][Idx(r * 6 + c)];
                }
            }
        } else if (!model.materials[Idx(element.material)].ConstitutiveMatrix(
                       model.MeanTemperature(element.nodes), d, error)) {
            return false;
        }
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model.nodes[Idx(node)]);
        ElementOptions options;
        if (!ElementStiffness(element.shape, corner, d, options, &k, error)) return false;
        const int size = static_cast<int>(element.nodes.size()) * 3;
        dof.clear();
        for (const int node : element.nodes) {
            for (int axis = 0; axis < 3; ++axis) dof.push_back(node * 3 + axis);
        }
        for (int r = 0; r < size; ++r) {
            for (int c = 0; c < size; ++c) {
                const double value = k[Idx(r * size + c)];
                if (value == 0.0) continue;
                const int at = pattern.Find(dof[Idx(r)], dof[Idx(c)]);
                if (at < 0) {
                    *error = "the sparsity pattern has no entry for a value the element "
                             "stiffness produced, so the pattern and the connectivity disagree";
                    return false;
                }
                values[Idx(at)] += value;
            }
        }
    }

    std::vector<num::Triplet> triplets;
    triplets.reserve(Idx(pattern.NonZeros()));
    for (int row = 0; row < pattern.Dofs(); ++row) {
        for (int at = pattern.RowStart()[Idx(row)]; at < pattern.RowStart()[Idx(row + 1)]; ++at) {
            triplets.push_back(num::Triplet{row, pattern.ColIndex()[Idx(at)], values[Idx(at)]});
        }
    }
    *out = num::SparseMatrix::FromTriplets(pattern.Dofs(), pattern.Dofs(), triplets);
    return true;
}

std::vector<double> AssembleLoads(const AnalysisModel &model) {
    std::vector<double> out(Idx(model.NodeCount() * 3), 0.0);
    for (const NodalLoad &load : model.loads) {
        if (load.node < 0 || load.node >= model.NodeCount()) continue;
        out[Idx(load.node * 3 + 0)] += load.force.x;
        out[Idx(load.node * 3 + 1)] += load.force.y;
        out[Idx(load.node * 3 + 2)] += load.force.z;
    }
    return out;
}

void System::Expand(const std::vector<double> &reduced, std::vector<double> *full) const {
    full->assign(fixed.size(), 0.0);
    for (std::size_t i = 0; i < fixed.size(); ++i) {
        if (fixed[i]) (*full)[i] = prescribed[i];
    }
    for (std::size_t row = 0; row < dof_of_row.size() && row < reduced.size(); ++row) {
        if (dof_of_row[row] < 0) continue;
        (*full)[Idx(dof_of_row[row])] = reduced[row];
    }
}

bool BuildSystem(const AnalysisModel &model, const std::vector<MultiPointConstraint> &constraints,
                 const AssemblyOptions &options, System *out, std::string *error) {
    *out = System{};
    SparsityPattern pattern;
    if (!pattern.Build(model, error)) return false;
    num::SparseMatrix stiffness;
    if (!AssembleStiffness(model, pattern, &stiffness, error)) return false;
    const std::vector<double> load = AssembleLoads(model);
    const int dofs = pattern.Dofs();

    out->fixed.assign(Idx(dofs), false);
    out->prescribed.assign(Idx(dofs), 0.0);
    for (const Constraint &constraint : model.constraints) {
        if (constraint.node < 0 || constraint.node >= model.NodeCount()) {
            *error = "a constraint names node " + std::to_string(constraint.node) +
                     ", which the model does not have";
            return false;
        }
        for (int axis = 0; axis < 3; ++axis) {
            if (!constraint.fixed[axis]) continue;
            out->fixed[Idx(constraint.node * 3 + axis)] = true;
            out->prescribed[Idx(constraint.node * 3 + axis)] = constraint.value[axis];
        }
    }

    // ELIMINATION: the free degrees of freedom keep their equations, and
    // a prescribed one's column moves to the right-hand side as the force
    // it takes to hold it there.
    std::vector<int> row_of_dof(Idx(dofs), -1);
    for (int i = 0; i < dofs; ++i) {
        if (out->fixed[Idx(i)]) continue;
        row_of_dof[Idx(i)] = static_cast<int>(out->dof_of_row.size());
        out->dof_of_row.push_back(i);
    }
    const int free_count = static_cast<int>(out->dof_of_row.size());
    if (free_count == 0) {
        *error = "every degree of freedom is prescribed, so there is nothing to solve for";
        return false;
    }

    std::vector<double> rhs(Idx(free_count), 0.0);
    std::vector<num::Triplet> triplets;
    double largest_diagonal = 0.0;
    for (int row = 0; row < dofs; ++row) {
        const int reduced_row = row_of_dof[Idx(row)];
        for (int at = stiffness.RowStart()[Idx(row)]; at < stiffness.RowStart()[Idx(row + 1)];
             ++at) {
            const int column = stiffness.ColIndex()[Idx(at)];
            const double value = stiffness.Values()[Idx(at)];
            if (value == 0.0) continue;
            if (row == column) largest_diagonal = std::max(largest_diagonal, std::fabs(value));
            if (reduced_row < 0) continue;
            const int reduced_column = row_of_dof[Idx(column)];
            if (reduced_column >= 0) {
                triplets.push_back(num::Triplet{reduced_row, reduced_column, value});
                continue;
            }
            rhs[Idx(reduced_row)] -= value * out->prescribed[Idx(column)];
        }
        if (reduced_row >= 0) rhs[Idx(reduced_row)] += load[Idx(row)];
    }

    // Multi-point constraints, written as rows of C over the free
    // degrees of freedom. A term on a prescribed degree of freedom is
    // known, so it moves to the constraint's own right-hand side.
    std::vector<std::vector<std::pair<int, double>>> c_rows;
    std::vector<double> c_values;
    for (const MultiPointConstraint &constraint : constraints) {
        std::vector<std::pair<int, double>> row;
        double value = constraint.value;
        for (const ConstraintTerm &term : constraint.terms) {
            if (term.node < 0 || term.node >= model.NodeCount() || term.axis < 0 ||
                term.axis > 2) {
                *error = "the constraint '" + constraint.label + "' names a degree of freedom "
                         "that does not exist";
                return false;
            }
            const int dof = term.node * 3 + term.axis;
            if (out->fixed[Idx(dof)]) {
                value -= term.coefficient * out->prescribed[Idx(dof)];
                continue;
            }
            row.push_back({row_of_dof[Idx(dof)], term.coefficient});
        }
        if (row.empty()) {
            // Every term was prescribed, so the constraint is either
            // already satisfied or already violated, and neither is
            // something to add an equation about.
            if (std::fabs(value) > 1e-9) {
                *error = "the constraint '" + constraint.label +
                         "' involves only prescribed degrees of freedom and they do not satisfy "
                         "it, so the model is over-constrained and has no solution";
                return false;
            }
            continue;
        }
        c_rows.push_back(row);
        c_values.push_back(value);
    }

    if (c_rows.empty() || options.mpc == MpcMethod::Penalty) {
        const double penalty = largest_diagonal * options.penalty_scale;
        for (std::size_t i = 0; i < c_rows.size(); ++i) {
            // K + penalty * C^T C, and f + penalty * C^T g.
            for (const auto &a : c_rows[i]) {
                rhs[Idx(a.first)] += penalty * a.second * c_values[i];
                for (const auto &b : c_rows[i]) {
                    triplets.push_back(
                        num::Triplet{a.first, b.first, penalty * a.second * b.second});
                }
            }
        }
        out->matrix = num::SparseMatrix::FromTriplets(free_count, free_count, triplets);
        out->rhs = rhs;
        return true;
    }

    // LAGRANGE: [[K, C^T], [C, 0]] with the constraint values below the
    // loads. Symmetric, indefinite, and one row larger per constraint.
    //
    // THE CONSTRAINT ROWS ARE SCALED BY THE STIFFNESS, and without that
    // the system is unfactorable for a reason that has nothing to do with
    // it being wrong. A constraint row's coefficients are of order one
    // while the stiffness is of order 1e11, so after the stiffness block
    // is eliminated the multiplier's own pivot is the Schur complement
    // -C K^-1 C^T, of order 1e-11. A factorization that rejects a pivot
    // small relative to the *largest* diagonal then refuses a perfectly
    // well-posed saddle point, and says "an unrestrained rigid-body mode
    // is the usual cause", which it is, and it was not this time.
    //
    // Multiplying each constraint equation through by the largest
    // diagonal changes nothing mathematically -- it is the same equation
    // -- and brings the multiplier's pivot to the same order as the rest,
    // at the cost of the multiplier itself now being the force divided by
    // that scale. Nothing here reads the multipliers, and Expand ignores
    // their rows.
    const double row_scale = largest_diagonal > 0.0 ? largest_diagonal : 1.0;
    const int size = free_count + static_cast<int>(c_rows.size());
    for (std::size_t i = 0; i < c_rows.size(); ++i) {
        const int row = free_count + static_cast<int>(i);
        out->dof_of_row.push_back(-1);
        for (const auto &term : c_rows[i]) {
            triplets.push_back(num::Triplet{row, term.first, term.second * row_scale});
            triplets.push_back(num::Triplet{term.first, row, term.second * row_scale});
        }
        rhs.push_back(c_values[i] * row_scale);
    }
    out->multipliers = static_cast<int>(c_rows.size());
    out->matrix = num::SparseMatrix::FromTriplets(size, size, triplets);
    out->rhs = rhs;
    return true;
}

bool Reactions(const AnalysisModel &model, const SparsityPattern &pattern,
               const std::vector<double> &displacement, std::vector<cad::Vec3d> *out,
               std::string *error) {
    num::SparseMatrix stiffness;
    if (!AssembleStiffness(model, pattern, &stiffness, error)) return false;
    if (static_cast<int>(displacement.size()) != pattern.Dofs()) {
        *error = "the displacement vector does not cover the model's degrees of freedom";
        return false;
    }
    std::vector<double> product;
    stiffness.Multiply(displacement, &product);
    const std::vector<double> load = AssembleLoads(model);
    out->assign(model.nodes.size(), cad::Vec3d{});
    for (int node = 0; node < model.NodeCount(); ++node) {
        (*out)[Idx(node)] = cad::Vec3d{product[Idx(node * 3 + 0)] - load[Idx(node * 3 + 0)],
                                       product[Idx(node * 3 + 1)] - load[Idx(node * 3 + 1)],
                                       product[Idx(node * 3 + 2)] - load[Idx(node * 3 + 2)]};
    }
    return true;
}

}  // namespace fem
