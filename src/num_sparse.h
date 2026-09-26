#ifndef MEP_NUM_SPARSE_H
#define MEP_NUM_SPARSE_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// Sparse linear algebra (plans/CAD_FEM_PLAN.md Part 0.3).
//
// This is on the critical path for two things that look unrelated: the
// sketch constraint solver (Part D.2), which solves a nonlinear
// least-squares system whose Jacobian is extremely sparse because each
// constraint touches two or three entities out of hundreds, and every FEM
// solver from Part H onward, whose stiffness matrix is sparse for exactly
// the same structural reason -- an element couples only the handful of
// nodes it contains. Neither can use the dense MatrixNd in cad_math.h: a
// 100k-DOF stiffness matrix is 80 GB dense and about 8 MB sparse.
//
// There is no external dependency here, in keeping with the rest of this
// codebase (docs/architecture.org: "no UI toolkit, no browser engine, no
// PDF library"). What that costs and what it does not is worth being
// precise about, since numerical linear algebra is the one area where
// the gap between a correct implementation and a fast one is largest:
//
//   * Correctness and convergence are not compromised. The factorization
//     below is a textbook up-looking simplicial LDL^T, and it produces
//     the same factors to round-off as any other implementation of the
//     same algorithm.
//   * Raw speed is. A supernodal factorization groups columns with
//     identical sparsity patterns so the inner loops become dense
//     matrix-matrix products running at cache-friendly BLAS-3 speed;
//     this simplicial one works a column at a time and will be several
//     times slower on a large 3D problem. That is a known, bounded gap
//     with a known fix (Part H.4), and taking it now buys a factorization
//     that is small enough to read and test.
namespace num {

// One entry of a matrix under construction. Assembly in both the
// constraint solver and the FEM assembler naturally produces duplicates
// -- every element that touches a pair of DOFs contributes to the same
// coefficient -- so duplicates are summed rather than rejected.
struct Triplet {
    int row = 0;
    int col = 0;
    double value = 0.0;
};

// Compressed sparse row. Immutable once built: every operation that would
// change the pattern returns a new matrix instead, which keeps the
// symbolic analysis below safe to cache against a given matrix.
class SparseMatrix {
public:
    SparseMatrix() = default;

    // Builds from triplets, summing duplicates and dropping exact zeros.
    // Out-of-range entries are ignored rather than clamped -- silently
    // moving a coefficient to a different DOF would be far worse than
    // losing it, and the assembler is where such a bug belongs.
    static SparseMatrix FromTriplets(int rows, int cols, const std::vector<Triplet> &triplets);
    // Identity, and a diagonal matrix -- used by the tests and as a
    // trivial preconditioner.
    static SparseMatrix Identity(int n);

    int Rows() const { return rows_; }
    int Cols() const { return cols_; }
    int NonZeros() const { return static_cast<int>(values_.size()); }
    bool IsEmpty() const { return rows_ == 0 || cols_ == 0; }

    const std::vector<int> &RowStart() const { return row_start_; }
    const std::vector<int> &ColIndex() const { return col_index_; }
    const std::vector<double> &Values() const { return values_; }

    // Coefficient lookup by binary search within the row. O(log nnz_row)
    // -- fine for tests and diagnostics, not for an inner loop.
    double At(int row, int col) const;

    // y = A*x. `x` must have Cols() entries, `y` Rows().
    void Multiply(const std::vector<double> &x, std::vector<double> *y) const;
    // y = A^T * x.
    void MultiplyTransposed(const std::vector<double> &x, std::vector<double> *y) const;

    SparseMatrix Transposed() const;
    // A^T * A, the normal-equations matrix behind the sketch solver's
    // sparse path. Symmetric by construction, stored in full.
    SparseMatrix TransposedTimesSelf() const;

    // True when the pattern and the values are both symmetric to `tol`.
    // The factorization below assumes symmetry and does not check it on
    // every call, so this exists for the caller to assert once.
    bool IsSymmetric(double tol = 1e-12) const;

    // P*A*P^T for a permutation given as `perm[new_index] = old_index`.
    SparseMatrix Permuted(const std::vector<int> &perm) const;

private:
    int rows_ = 0;
    int cols_ = 0;
    std::vector<int> row_start_;   // size rows_+1
    std::vector<int> col_index_;   // size nnz, sorted within each row
    std::vector<double> values_;   // size nnz
};

// ---------------------------------------------------------------------
// Fill-reducing orderings
// ---------------------------------------------------------------------

// Which ordering to apply before factorizing. The choice matters more
// than almost anything else in a direct solver: factorizing a 3D FEM
// stiffness matrix in its natural node order can produce an L with two
// orders of magnitude more nonzeros than a good ordering does, and the
// factorization cost grows faster than that.
enum class Ordering {
    // No reordering. Correct, and occasionally the right answer for a
    // matrix that is already banded (a 1D problem, a swept mesh).
    Natural,
    // Reverse Cuthill-McKee: a breadth-first numbering from a
    // pseudo-peripheral node, reversed. Cheap, deterministic, and good
    // at turning an arbitrary mesh's matrix into a narrow band. Weaker
    // than minimum degree on 3D problems, stronger on thin/elongated
    // ones where a band really does exist.
    ReverseCuthillMcKee,
    // Minimum degree: repeatedly eliminate the node of least degree,
    // filling in its neighbourhood as a clique.
    //
    // The straightforward O(n*d^2) formulation, kept because it is the
    // definition the approximate one is measured against and because on a
    // small matrix it is exact where the approximation is not. It forms
    // each clique explicitly, so it is too slow to leave on past a few
    // tens of thousands of unknowns -- which is what
    // ApproximateMinimumDegree exists for and why that is the default.
    MinimumDegree,
    // AMD: the same heuristic on a quotient graph, with approximate
    // degrees, supervariables, mass elimination and element absorption.
    // Near-linear in practice and the right default for anything large.
    // The approximation is of the *degree*, which is a heuristic choice
    // of pivot -- not of the factorization, which is exact either way.
    ApproximateMinimumDegree,
};

// Computes an ordering of a symmetric matrix's graph. Returns
// `perm[new_index] = old_index`; `InvertPermutation` gives the other
// direction. Only the pattern is read, never the values.
std::vector<int> ComputeOrdering(const SparseMatrix &a, Ordering ordering);
std::vector<int> InvertPermutation(const std::vector<int> &perm);

// ---------------------------------------------------------------------
// Direct factorization
// ---------------------------------------------------------------------

// Sparse LDL^T of a symmetric matrix: A = P^T (L D L^T) P, with L unit
// lower triangular and D diagonal.
//
// No pivoting. That is a real restriction and not a shortcut: LDL^T
// without pivoting is guaranteed to exist only for a matrix that is
// positive definite (or quasi-definite). A FEM stiffness matrix with its
// rigid-body modes properly constrained is positive definite, which is
// why this is the right factorization for Part H; an *unconstrained* one
// is singular, and the honest response to that is to say so rather than
// to return a number, which is what Factorize does. Indefinite systems
// -- the saddle-point matrices that Lagrange-multiplier constraints and
// mixed formulations produce -- need Bunch-Kaufman pivoting and are not
// handled here; Part H.3 uses elimination rather than multipliers for
// single-point constraints partly for this reason.
class SparseLDLT {
public:
    // Analyzes and factorizes in one call. Returns false on a structurally
    // or numerically singular matrix, with a description in Error().
    //
    // `singular_tolerance` is relative to the largest diagonal entry of
    // `a`: a pivot smaller than that fraction of it is reported as
    // singular rather than divided by. An exact-zero test is not enough,
    // and the difference is the single most common user error in finite
    // elements -- a model that is not fully restrained has rigid-body
    // modes whose pivots land at round-off rather than at exactly zero,
    // so an exact test lets the factorization "succeed" and return
    // arbitrarily large garbage. The default is loose enough to admit a
    // genuinely ill-conditioned but valid model (a thin shell can reach
    // 1e10) and tight enough to catch a rigid-body mode (round-off, near
    // 1e-16). A caller that knows its matrix is worse-conditioned than
    // that can lower it.
    bool Factorize(const SparseMatrix &a, Ordering ordering = Ordering::ApproximateMinimumDegree,
                   double singular_tolerance = 1e-13);

    // Solves A*x = b using the stored factors. Factorize must have
    // succeeded. Cheap relative to factorization, so many right-hand
    // sides (a transient solve's time steps, a modal solver's shift-invert
    // iterations) reuse one factorization -- which is the main reason to
    // prefer a direct solver at all.
    bool Solve(const std::vector<double> &b, std::vector<double> *x) const;

    bool IsFactorized() const { return factorized_; }
    const std::string &Error() const { return error_; }
    // Nonzeros in L. The measure of how well the ordering worked, and
    // what the tests compare across orderings.
    int FactorNonZeros() const { return static_cast<int>(l_values_.size()); }
    // Number of negative entries in D. For a matrix that should be
    // positive definite this must be zero; it is also exactly the
    // information Sturm-sequence mode counting needs in Part I.4, which
    // is why it is exposed rather than kept internal.
    int NegativeEigenvalueCount() const;
    const std::vector<double> &Diagonal() const { return d_; }

    // --- Out of core ------------------------------------------------------
    //
    // Writes L to `path`, releases it from memory, and makes Solve stream
    // it back a column at a time. The factor is the large thing -- its
    // fill is many times the matrix's own non-zeros in three dimensions --
    // and it is also the thing a solve reads once forwards and once
    // backwards in column order, which is exactly what a file is good at.
    //
    // WHAT THIS IS NOT. The *factorization* still needs the whole factor
    // in memory, because the up-looking algorithm above reaches back into
    // arbitrary earlier columns as it forms each row. Making that
    // out-of-core means a left-looking or multifrontal scheme, where a
    // column is finished and never revisited, and that is a different
    // factorization rather than an option on this one. What is here helps
    // the case where a factor is computed once and solved against many
    // times afterwards -- a transient, a modal shift-invert, a load-case
    // sweep -- and it is said plainly rather than described as more.
    bool SpillTo(const std::string &path, std::string *error);
    // Reads a spilled factor back into memory.
    bool Reload(std::string *error);
    bool IsSpilled() const { return !spill_path_.empty(); }
    // Bytes of L held in memory: what a caller decides to spill on.
    long long BytesInMemory() const;

private:
    bool factorized_ = false;
    int n_ = 0;
    std::string error_;
    std::vector<int> perm_;      // perm_[new] = old
    std::vector<int> inv_perm_;  // inv_perm_[old] = new
    // L in compressed sparse column form, unit diagonal not stored.
    std::vector<int> l_start_;
    std::vector<int> l_index_;
    std::vector<double> l_values_;
    std::vector<double> d_;
    std::vector<int> parent_;  // elimination tree
    // Where L went, when it is not in l_index_/l_values_.
    std::string spill_path_;
};

// ---------------------------------------------------------------------
// Iterative solution
// ---------------------------------------------------------------------

enum class Preconditioner {
    None,
    // Diagonal scaling. Almost free, and on a well-scaled FEM problem
    // still worth a large constant factor.
    Jacobi,
    // Incomplete Cholesky with no fill beyond A's own pattern. Much more
    // effective than Jacobi and still cheap to build, but it can break
    // down (a non-positive pivot) even for a positive-definite A, in
    // which case IncompleteCholesky below reports failure and the caller
    // falls back to Jacobi rather than silently producing a useless
    // preconditioner.
    IncompleteCholesky,
    // Smoothed-aggregation algebraic multigrid, as one V-cycle per
    // application.
    //
    // WHAT MAKES IT DIFFERENT IN KIND from the three above. Jacobi and
    // incomplete Cholesky are local: they damp the parts of the error
    // that vary quickly from one unknown to the next and do nothing to
    // the parts that vary slowly across the whole model, so the iteration
    // count grows as the mesh is refined -- roughly like 1/h, which means
    // halving the element size doubles the work per solve on top of the
    // work from having more unknowns. Multigrid represents the slowly
    // varying error on a coarser problem, where it is no longer slowly
    // varying, and recurses. Done properly the iteration count stops
    // depending on the mesh size at all, which is the only thing that
    // makes a very large model solvable iteratively.
    AlgebraicMultigrid,
};

// Smoothed aggregation, and the choices it needs.
struct MultigridOptions {
    // Two unknowns are "strongly connected" when |a_ij| exceeds this
    // fraction of the largest off-diagonal in row i. Aggregates are grown
    // along strong connections only, so this is what decides the shape of
    // the coarsening. Measured against the row's own maximum rather than
    // against sqrt(a_ii * a_jj), which is the textbook form and does not
    // survive the change of scale the Galerkin product makes at each
    // level -- see the note where it is used.
    double strength_threshold = 0.25;
    // Stop coarsening at this size and factor the coarse problem
    // directly. Small enough that the direct solve is negligible, large
    // enough that the hierarchy does not go on for ever.
    int coarse_limit = 300;
    int max_levels = 25;
    // Two, not one, and measured rather than chosen: on a cantilever at
    // four thousand unknowns one sweep took 41 iterations and two took
    // 32, for one extra damped-Jacobi pass in each direction. Three took
    // 28, which no longer pays for itself.
    int smoothing_sweeps = 2;
    // Damping for the Jacobi smoother and for smoothing the prolongator.
    // Zero derives it from an estimate of the spectral radius, which is
    // what it should be; a fixed value is for reproducing a particular
    // run.
    double damping = 0.0;
    // THE NEAR-NULL SPACE IS WHAT MAKES THIS WORK ON ELASTICITY, and
    // getting it wrong is the difference between mesh-independent
    // convergence and no convergence at all. Multigrid is built on the
    // coarse grid being able to represent the error that the smoother
    // cannot reduce, and for an elliptic operator that error looks like
    // the operator's near-null space: for a Laplacian, a constant; for
    // elasticity, the six rigid-body modes. The default is the single
    // constant vector, which is right for a scalar problem and only
    // partly right for a vector one. A caller with coordinates should
    // pass the rigid modes.
    std::vector<std::vector<double>> near_null_space;
    // Unknowns per node: 3 for a solid, 1 for a scalar field.
    //
    // AGGREGATION HAPPENS ON NODES, NOT ON UNKNOWNS, AND IT MATTERS. Told
    // nothing, the aggregator works on the matrix graph, where a node's
    // x, y and z rows are three separate vertices that may land in three
    // different aggregates. The six rigid-body modes then describe
    // nothing in particular on any of them, and the method degenerates to
    // a slightly better Jacobi: on a cantilever its iteration count grew
    // with the mesh exactly as incomplete Cholesky's did, which is the
    // one thing multigrid is supposed not to do. With the block size the
    // node graph is aggregated instead and every unknown of a node goes
    // where the node goes.
    int block_size = 1;
};

struct IterativeOptions {
    int max_iterations = 1000;
    // Convergence is on the *relative* residual ||b - Ax|| / ||b||, so
    // the same tolerance means the same thing regardless of how the
    // problem is scaled.
    double tolerance = 1e-10;
    Preconditioner preconditioner = Preconditioner::IncompleteCholesky;
    MultigridOptions multigrid;
    // Restart length for GMRES only. Larger converges in fewer iterations
    // but costs memory and orthogonalization time quadratic in this.
    int restart = 30;
};

struct IterativeResult {
    bool converged = false;
    int iterations = 0;
    double relative_residual = 0.0;
    std::string message;
};

// The multigrid hierarchy, exposed rather than kept inside the solver.
//
// Worth its own type because almost everything that can be wrong with a
// multigrid method is visible in the hierarchy rather than in the answer:
// coarsening that barely coarsens, a level whose operator is denser than
// the one above it, a V-cycle that is not symmetric and so cannot be used
// with conjugate gradients at all. Each of those is checkable here
// without solving anything.
class AlgebraicMultigridPreconditioner {
public:
    bool Build(const SparseMatrix &a, const MultigridOptions &options, std::string *error);

    int Levels() const { return static_cast<int>(levels_.size()); }
    int RowsAtLevel(int level) const;
    int NonZerosAtLevel(int level) const;
    // Total non-zeros across every level divided by the finest level's.
    // The standard measure of whether a hierarchy is affordable: much
    // above two and the cycle costs more than it saves.
    double OperatorComplexity() const;

    // One V-cycle: an approximate solve of A z = r.
    void Apply(const std::vector<double> &r, std::vector<double> *z) const;

private:
    struct Level {
        SparseMatrix a;
        SparseMatrix p;   // prolongation from the level below
        std::vector<double> inverse_diagonal;
        double damping = 0.0;
    };
    std::vector<Level> levels_;
    SparseLDLT coarse_;
    int sweeps_ = 1;
};

// A * B for two sparse matrices. Exposed because the Galerkin coarse
// operator P^T A P is two of these and a test that wants to check one
// against a dense product needs the same routine the hierarchy used.
SparseMatrix Multiply(const SparseMatrix &a, const SparseMatrix &b);

// Preconditioned conjugate gradients, for symmetric positive definite A.
// The workhorse for large structural and thermal problems where a direct
// factorization stops fitting in memory.
IterativeResult ConjugateGradient(const SparseMatrix &a, const std::vector<double> &b, std::vector<double> *x,
                                  const IterativeOptions &opt = {});

// Restarted GMRES, for a general (unsymmetric, possibly indefinite) A.
// Present because not every system in the roadmap is symmetric --
// transient problems with convective boundary terms and the tangent
// matrices of contact and friction are not.
IterativeResult Gmres(const SparseMatrix &a, const std::vector<double> &b, std::vector<double> *x,
                      const IterativeOptions &opt = {});

// Builds an IC(0) factor with A's own lower-triangular pattern. Returns
// false (leaving the outputs unspecified) if a non-positive pivot is hit.
// Exposed because a caller solving many systems with the same matrix
// wants to build this once.
bool IncompleteCholesky(const SparseMatrix &a, std::vector<int> *l_start, std::vector<int> *l_index,
                        std::vector<double> *l_values);

// Dot product and 2-norm, shared by the solvers and useful to callers
// checking a residual themselves.
double Dot(const std::vector<double> &a, const std::vector<double> &b);
double Norm2(const std::vector<double> &v);

}  // namespace num

#endif
