#ifndef MEP_FEM_ASSEMBLE_H
#define MEP_FEM_ASSEMBLE_H

#include "fem_study.h"
#include "num_sparse.h"

#include <string>
#include <vector>

// Assembly (plans/CAD_FEM_PLAN.md Part H.3).
//
// SYMBOLIC FIRST, THEN NUMERIC, AND THE SPLIT IS THE POINT. Where every
// non-zero of the global matrix lies follows from the connectivity alone:
// two degrees of freedom share an entry exactly when some element holds
// them both. That structure does not change when a material changes, when
// a load case changes, or between the iterations of a nonlinear solve --
// only the numbers in it do. Working it out once and filling it
// repeatedly is the difference between a nonlinear solve that rebuilds
// its matrix from a list of triplets every iteration, sorting and merging
// millions of them each time, and one that writes straight into a
// structure it already has.
//
// It is also what makes the fill checkable. A pattern built from
// connectivity and a matrix built from element stiffnesses are two
// independent derivations of the same set of positions, and the test
// compares them.
//
// CONSTRAINTS, AND WHY THERE ARE THREE WAYS. A single-point constraint --
// this node, this direction, this value -- is eliminated: the row and
// column leave the system and their effect moves to the right-hand side.
// That is exact, it makes the system smaller, and it cannot be done for a
// constraint that couples several degrees of freedom. Those need either a
// Lagrange multiplier, which is exact and makes the matrix indefinite and
// larger, or a penalty, which keeps it positive definite and the same
// size and is approximate by however large the penalty is not. Both are
// here because neither is right in every case, and the test holds them to
// each other.
namespace fem {

// One term of a linear constraint: a coefficient on one node's one
// direction.
struct ConstraintTerm {
    int node = 0;
    int axis = 0;
    double coefficient = 1.0;
};

// sum(coefficient * displacement) = value.
//
// Tying two nodes together is `u_a - u_b = 0`; a symmetry plane is one
// term; a rigid link is three rows per dependent node.
struct MultiPointConstraint {
    std::string label = "constraint";
    std::vector<ConstraintTerm> terms;
    double value = 0.0;
};

// Dependent nodes that move as a rigid body with a controlling node --
// what a bolt, a bearing or a load spread over a face is modelled as.
//
// The rotation is taken about the controlling node and enters through the
// dependent node's offset from it, which is why this needs the model's
// coordinates and is not a purely topological statement.
struct RigidLink {
    std::string label = "rigid link";
    int controlling = -1;
    std::vector<int> dependent;
};

// The link's rows. Six degrees of freedom of motion are represented by
// the controlling node's three translations plus three rotation degrees
// of freedom appended to the model -- which a displacement-only solver
// does not have, so this writes the *translation-only* form: every
// dependent node moves with the controlling node exactly. That is a rigid
// link without rotation, which is what a short connection is, and it is
// said here rather than implied.
bool ExpandRigidLink(const AnalysisModel &model, const RigidLink &link,
                     std::vector<MultiPointConstraint> *out, std::string *error);

// Where the non-zeros are, worked out from connectivity alone.
class SparsityPattern {
public:
    bool Build(const AnalysisModel &model, std::string *error);

    int Dofs() const { return dofs_; }
    int NonZeros() const { return static_cast<int>(col_index_.size()); }
    const std::vector<int> &RowStart() const { return row_start_; }
    const std::vector<int> &ColIndex() const { return col_index_; }

    // Where (row, col) lives in a value array laid out to this pattern,
    // or -1 if the pattern has no such entry -- which for an assembly
    // means the connectivity and the element matrices disagree, and is
    // worth an error rather than a silent drop.
    int Find(int row, int col) const;

private:
    int dofs_ = 0;
    std::vector<int> row_start_;
    std::vector<int> col_index_;
};

// Fills the pattern with the assembled stiffness.
bool AssembleStiffness(const AnalysisModel &model, const SparsityPattern &pattern,
                       num::SparseMatrix *out, std::string *error);

// The nodal load vector, one entry per degree of freedom.
std::vector<double> AssembleLoads(const AnalysisModel &model);

enum class MpcMethod {
    // Exact. Adds one unknown per constraint and makes the matrix
    // symmetric indefinite, which a plain Cholesky cannot factor and an
    // LDL^T can.
    Lagrange,
    // Approximate, by however large the penalty is not; keeps the matrix
    // positive definite and its original size. Too small a penalty leaves
    // the constraint loose, too large destroys the conditioning, and the
    // scale below is relative to the largest diagonal so that the choice
    // does not depend on the units the model is in.
    Penalty,
};

struct AssemblyOptions {
    MpcMethod mpc = MpcMethod::Lagrange;
    double penalty_scale = 1e8;
};

// A system ready to solve, with single-point constraints eliminated and
// multi-point ones applied.
struct System {
    num::SparseMatrix matrix;
    std::vector<double> rhs;
    // The global degree of freedom each row stands for; -1 for a Lagrange
    // multiplier, which stands for a constraint rather than a motion.
    std::vector<int> dof_of_row;
    // Per global degree of freedom.
    std::vector<bool> fixed;
    std::vector<double> prescribed;
    int multipliers = 0;

    int Size() const { return static_cast<int>(rhs.size()); }
    // Puts a solution of the reduced system back into a full
    // displacement vector, prescribed values included.
    void Expand(const std::vector<double> &reduced, std::vector<double> *full) const;
};

bool BuildSystem(const AnalysisModel &model, const std::vector<MultiPointConstraint> &constraints,
                 const AssemblyOptions &options, System *out, std::string *error);

// The reaction at every constrained degree of freedom, from a full
// displacement solution: K*u - f, which is zero where nothing holds the
// model and is the reaction where something does.
//
// Exposed because the sum of the reactions balancing the applied load is
// the cheapest global check there is that a solve was right, and it is
// worth making without a solver wrapped round it.
bool Reactions(const AnalysisModel &model, const SparsityPattern &pattern,
               const std::vector<double> &displacement, std::vector<cad::Vec3d> *out,
               std::string *error);

}  // namespace fem

#endif
