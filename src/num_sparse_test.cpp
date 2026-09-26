// Windowless coverage for num_sparse.h (plans/CAD_FEM_PLAN.md Part 0.3).
//
// The test that matters most here is the last one, and it is worth saying
// why. Checking that a linear solver returns a small residual proves
// almost nothing: a solver can satisfy ||Ax - b|| tightly while solving
// the wrong problem, because the residual only ever sees the matrix it
// was handed. So the final test discretizes a Poisson problem whose exact
// solution is known in closed form, solves it on two grids, and checks
// that the error against that exact solution falls by the factor the
// discretization's own order predicts. That catches an assembly bug, an
// ordering bug, a factorization bug and a solver bug alike, and it is the
// same technique (the method of manufactured solutions) the FEM solvers
// in Part H are verified with later.

#include "num_sparse.h"

#include "cad_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

namespace {

int g_checks = 0;

void Check(bool condition, const char *expression, int line) {
    ++g_checks;
    if (condition) return;
    // Flushed before aborting, because abort does not: a failing test
    // that loses the table it was printing takes the diagnosis with it,
    // which cost a run to discover.
    std::fflush(stdout);
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

bool Near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// The 5-point Laplacian on an n x n interior grid of the unit square with
// homogeneous Dirichlet boundaries. Symmetric positive definite, sparse,
// and the standard model problem for exactly this layer.
num::SparseMatrix Laplacian2D(int n, double *out_h) {
    const double h = 1.0 / (static_cast<double>(n) + 1.0);
    *out_h = h;
    const double inv_h2 = 1.0 / (h * h);
    std::vector<num::Triplet> t;
    auto index = [n](int i, int j) { return i * n + j; };
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            const int row = index(i, j);
            t.push_back(num::Triplet{row, row, 4.0 * inv_h2});
            if (i > 0) t.push_back(num::Triplet{row, index(i - 1, j), -inv_h2});
            if (i < n - 1) t.push_back(num::Triplet{row, index(i + 1, j), -inv_h2});
            if (j > 0) t.push_back(num::Triplet{row, index(i, j - 1), -inv_h2});
            if (j < n - 1) t.push_back(num::Triplet{row, index(i, j + 1), -inv_h2});
        }
    }
    return num::SparseMatrix::FromTriplets(n * n, n * n, t);
}

void TestAssembly() {
    std::vector<num::Triplet> t;
    t.push_back(num::Triplet{0, 0, 1.0});
    t.push_back(num::Triplet{0, 2, 2.0});
    t.push_back(num::Triplet{0, 0, 3.0});   // duplicate: must sum to 4
    t.push_back(num::Triplet{1, 1, 5.0});
    t.push_back(num::Triplet{2, 0, 7.0});
    t.push_back(num::Triplet{1, 2, 2.0});
    t.push_back(num::Triplet{1, 2, -2.0});  // cancels exactly: must be dropped
    t.push_back(num::Triplet{9, 9, 1.0});   // out of range: ignored, not clamped
    t.push_back(num::Triplet{-1, 0, 1.0});
    const num::SparseMatrix a = num::SparseMatrix::FromTriplets(3, 3, t);
    CHECK(a.Rows() == 3 && a.Cols() == 3);
    CHECK(Near(a.At(0, 0), 4.0));
    CHECK(Near(a.At(0, 2), 2.0));
    CHECK(Near(a.At(1, 1), 5.0));
    CHECK(Near(a.At(2, 0), 7.0));
    CHECK(Near(a.At(1, 2), 0.0));
    CHECK(Near(a.At(2, 2), 0.0));
    // Four stored entries: the cancelled one and both out-of-range ones
    // are gone.
    CHECK(a.NonZeros() == 4);
    // Columns are sorted within each row -- the merge in IncompleteCholesky
    // and the binary search in At both depend on it.
    for (int r = 0; r < a.Rows(); ++r) {
        for (int p = a.RowStart()[static_cast<std::size_t>(r)] + 1;
             p < a.RowStart()[static_cast<std::size_t>(r) + 1]; ++p) {
            CHECK(a.ColIndex()[static_cast<std::size_t>(p)] >
                  a.ColIndex()[static_cast<std::size_t>(p - 1)]);
        }
    }

    // Matrix-vector products against a dense reference.
    std::vector<double> x;
    x.push_back(1.0);
    x.push_back(2.0);
    x.push_back(3.0);
    std::vector<double> y;
    a.Multiply(x, &y);
    CHECK(Near(y[0], 4.0 * 1.0 + 2.0 * 3.0));
    CHECK(Near(y[1], 5.0 * 2.0));
    CHECK(Near(y[2], 7.0 * 1.0));
    std::vector<double> yt;
    a.MultiplyTransposed(x, &yt);
    CHECK(Near(yt[0], 4.0 * 1.0 + 7.0 * 3.0));
    CHECK(Near(yt[1], 5.0 * 2.0));
    CHECK(Near(yt[2], 2.0 * 1.0));

    const num::SparseMatrix at = a.Transposed();
    CHECK(Near(at.At(2, 0), 2.0));
    CHECK(Near(at.At(0, 2), 7.0));
    CHECK(!a.IsSymmetric());
    CHECK(num::SparseMatrix::Identity(4).IsSymmetric());

    // A^T*A must be symmetric and match the explicit product.
    const num::SparseMatrix ata = a.TransposedTimesSelf();
    CHECK(ata.IsSymmetric());
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double expected = 0.0;
            for (int k = 0; k < 3; ++k) expected += a.At(k, i) * a.At(k, j);
            CHECK(Near(ata.At(i, j), expected));
        }
    }
}

void TestOrderings() {
    double h = 0.0;
    const num::SparseMatrix a = Laplacian2D(12, &h);
    const int n = a.Rows();

    const num::Ordering kinds[4] = {num::Ordering::Natural, num::Ordering::ReverseCuthillMcKee,
                                    num::Ordering::MinimumDegree,
                                    num::Ordering::ApproximateMinimumDegree};
    for (num::Ordering kind : kinds) {
        const std::vector<int> perm = num::ComputeOrdering(a, kind);
        CHECK(static_cast<int>(perm.size()) == n);
        // Every ordering must be a genuine permutation -- a bug that
        // repeats or drops an index produces a factorization that is
        // silently wrong rather than one that fails.
        std::vector<int> seen(static_cast<std::size_t>(n), 0);
        for (int v : perm) {
            CHECK(v >= 0 && v < n);
            CHECK(seen[static_cast<std::size_t>(v)] == 0);
            seen[static_cast<std::size_t>(v)] = 1;
        }
        const std::vector<int> inv = num::InvertPermutation(perm);
        for (int i = 0; i < n; ++i) CHECK(perm[static_cast<std::size_t>(inv[static_cast<std::size_t>(i)])] == i);
        // Permuting must preserve the matrix, not just its shape.
        const num::SparseMatrix pa = a.Permuted(perm);
        CHECK(pa.NonZeros() == a.NonZeros());
        CHECK(pa.IsSymmetric());
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                if (a.At(i, j) != 0.0) {
                    CHECK(Near(pa.At(inv[static_cast<std::size_t>(i)], inv[static_cast<std::size_t>(j)]),
                               a.At(i, j)));
                }
            }
        }
    }

    // A disconnected graph (two separate bodies, which an assembly makes
    // routine) must still produce a complete permutation -- a single
    // breadth-first sweep would silently cover only one component.
    std::vector<num::Triplet> t;
    for (int i = 0; i < 6; ++i) t.push_back(num::Triplet{i, i, 2.0});
    t.push_back(num::Triplet{0, 1, -1.0});
    t.push_back(num::Triplet{1, 0, -1.0});
    t.push_back(num::Triplet{3, 4, -1.0});
    t.push_back(num::Triplet{4, 3, -1.0});
    const num::SparseMatrix split = num::SparseMatrix::FromTriplets(6, 6, t);
    for (num::Ordering kind : kinds) {
        const std::vector<int> perm = num::ComputeOrdering(split, kind);
        CHECK(perm.size() == 6);
        std::vector<int> seen(6, 0);
        for (int v : perm) seen[static_cast<std::size_t>(v)] = 1;
        for (int s : seen) CHECK(s == 1);
    }
}

void TestDirectSolve() {
    // Small system, cross-checked against the dense LU in cad_math -- two
    // independent implementations agreeing is worth more than either
    // agreeing with a value typed into this file.
    std::vector<num::Triplet> t;
    t.push_back(num::Triplet{0, 0, 4.0});
    t.push_back(num::Triplet{0, 1, 1.0});
    t.push_back(num::Triplet{1, 0, 1.0});
    t.push_back(num::Triplet{1, 1, 3.0});
    t.push_back(num::Triplet{1, 2, 1.0});
    t.push_back(num::Triplet{2, 1, 1.0});
    t.push_back(num::Triplet{2, 2, 2.0});
    const num::SparseMatrix a = num::SparseMatrix::FromTriplets(3, 3, t);
    CHECK(a.IsSymmetric());

    std::vector<double> b;
    b.push_back(1.0);
    b.push_back(2.0);
    b.push_back(3.0);

    cad::MatrixNd dense(3, 3);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) dense(static_cast<std::size_t>(i), static_cast<std::size_t>(j)) = a.At(i, j);
    std::vector<double> dense_x;
    CHECK(dense.SolveLU(b, &dense_x));

    const num::Ordering kinds[4] = {num::Ordering::Natural, num::Ordering::ReverseCuthillMcKee,
                                    num::Ordering::MinimumDegree,
                                    num::Ordering::ApproximateMinimumDegree};
    for (num::Ordering kind : kinds) {
        num::SparseLDLT ldlt;
        CHECK(ldlt.Factorize(a, kind));
        std::vector<double> x;
        CHECK(ldlt.Solve(b, &x));
        for (std::size_t i = 0; i < 3; ++i) CHECK(Near(x[i], dense_x[i], 1e-10));
        // This matrix is positive definite, so D must have no negative
        // entries whichever ordering was used.
        CHECK(ldlt.NegativeEigenvalueCount() == 0);
    }

    // A singular matrix must be reported, not solved. An unconstrained
    // stiffness matrix looks exactly like this, so the message matters.
    std::vector<num::Triplet> st;
    st.push_back(num::Triplet{0, 0, 1.0});
    st.push_back(num::Triplet{0, 1, -1.0});
    st.push_back(num::Triplet{1, 0, -1.0});
    st.push_back(num::Triplet{1, 1, 1.0});
    const num::SparseMatrix singular = num::SparseMatrix::FromTriplets(2, 2, st);
    num::SparseLDLT bad;
    CHECK(!bad.Factorize(singular, num::Ordering::Natural));
    CHECK(!bad.Error().empty());
    CHECK(!bad.IsFactorized());
    std::vector<double> unused;
    CHECK(!bad.Solve(b, &unused));

    // An indefinite matrix: LDL^T without pivoting still runs here, and
    // the negative entry in D is the signal the header promises.
    std::vector<num::Triplet> it;
    it.push_back(num::Triplet{0, 0, 1.0});
    it.push_back(num::Triplet{1, 1, -2.0});
    const num::SparseMatrix indefinite = num::SparseMatrix::FromTriplets(2, 2, it);
    num::SparseLDLT ind;
    CHECK(ind.Factorize(indefinite, num::Ordering::Natural));
    CHECK(ind.NegativeEigenvalueCount() == 1);
}

void TestFillReduction() {
    double h = 0.0;
    const num::SparseMatrix a = Laplacian2D(20, &h);
    int fill[4] = {0, 0, 0, 0};
    const num::Ordering kinds[4] = {num::Ordering::Natural, num::Ordering::ReverseCuthillMcKee,
                                    num::Ordering::MinimumDegree,
                                    num::Ordering::ApproximateMinimumDegree};
    std::vector<double> b(static_cast<std::size_t>(a.Rows()), 1.0);
    std::vector<double> reference;
    for (int k = 0; k < 4; ++k) {
        num::SparseLDLT ldlt;
        CHECK(ldlt.Factorize(a, kinds[k]));
        fill[k] = ldlt.FactorNonZeros();
        std::vector<double> x;
        CHECK(ldlt.Solve(b, &x));
        // Whatever the ordering, the answer is the same problem's answer.
        if (k == 0) {
            reference = x;
        } else {
            for (std::size_t i = 0; i < x.size(); ++i) CHECK(Near(x[i], reference[i], 1e-8));
        }
        // And it really solves the system.
        std::vector<double> ax;
        a.Multiply(x, &ax);
        for (std::size_t i = 0; i < ax.size(); ++i) CHECK(Near(ax[i], b[i], 1e-6));
    }
    // The orderings must actually earn their place: both should produce
    // less fill than the natural ordering on a 2D grid, and minimum
    // degree should beat or match RCM.
    std::printf("  fill: natural=%d rcm=%d mindeg=%d amd=%d\n", fill[0], fill[1], fill[2], fill[3]);
    CHECK(fill[1] < fill[0]);
    CHECK(fill[2] < fill[0]);
    CHECK(fill[2] <= fill[1]);
    // AMD APPROXIMATES THE DEGREE, NOT THE FACTORIZATION. It may pick a
    // different pivot from exact minimum degree and so produce a
    // different amount of fill -- but it has to be in the same league,
    // and it has to beat the orderings it exists to replace.
    CHECK(fill[3] < fill[0]);
    CHECK(fill[3] <= fill[1]);
    CHECK(fill[3] < fill[2] * 2);
}

// A 3D Laplacian, which is where an ordering earns its keep: in two
// dimensions almost anything works and in three the difference between
// orderings is the difference between a factorization that fits and one
// that does not.
num::SparseMatrix Laplacian3D(int m) {
    std::vector<num::Triplet> triplets;
    const int n = m * m * m;
    auto at = [&](int i, int j, int k) { return (k * m + j) * m + i; };
    for (int k = 0; k < m; ++k) {
        for (int j = 0; j < m; ++j) {
            for (int i = 0; i < m; ++i) {
                const int row = at(i, j, k);
                triplets.push_back(num::Triplet{row, row, 6.0});
                const int neighbours[6][3] = {{i - 1, j, k}, {i + 1, j, k}, {i, j - 1, k},
                                              {i, j + 1, k}, {i, j, k - 1}, {i, j, k + 1}};
                for (const auto &nb : neighbours) {
                    if (nb[0] < 0 || nb[0] >= m || nb[1] < 0 || nb[1] >= m || nb[2] < 0 ||
                        nb[2] >= m) {
                        continue;
                    }
                    triplets.push_back(num::Triplet{row, at(nb[0], nb[1], nb[2]), -1.0});
                }
            }
        }
    }
    return num::SparseMatrix::FromTriplets(n, n, triplets);
}

double SecondsFor(const num::SparseMatrix &a, num::Ordering ordering, int *out_fill) {
    const std::clock_t began = std::clock();
    num::SparseLDLT ldlt;
    const bool ok = ldlt.Factorize(a, ordering);
    const std::clock_t ended = std::clock();
    *out_fill = ok ? ldlt.FactorNonZeros() : -1;
    if (!ok) std::printf("      %s\n", ldlt.Error().c_str());
    return static_cast<double>(ended - began) / CLOCKS_PER_SEC;
}

void TestMultigrid() {
    std::printf("smoothed-aggregation multigrid:\n");
    // The sparse-times-sparse product the Galerkin operator is built
    // from, against a dense product of the same two matrices. Everything
    // in the hierarchy rests on this, so it is checked on its own first.
    {
        std::vector<num::Triplet> ta;
        std::vector<num::Triplet> tb;
        for (int i = 0; i < 5; ++i) {
            for (int j = 0; j < 4; ++j) {
                if ((i + j) % 3 == 0) continue;
                ta.push_back(num::Triplet{i, j, 1.0 + 0.25 * static_cast<double>(i * 4 + j)});
            }
        }
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 3; ++j) {
                if ((i * j) % 2 == 1) continue;
                tb.push_back(num::Triplet{i, j, 0.5 - 0.1 * static_cast<double>(i * 3 + j)});
            }
        }
        const num::SparseMatrix a = num::SparseMatrix::FromTriplets(5, 4, ta);
        const num::SparseMatrix b = num::SparseMatrix::FromTriplets(4, 3, tb);
        const num::SparseMatrix product = num::Multiply(a, b);
        CHECK(product.Rows() == 5 && product.Cols() == 3);
        for (int i = 0; i < 5; ++i) {
            for (int j = 0; j < 3; ++j) {
                double expected = 0.0;
                for (int k = 0; k < 4; ++k) expected += a.At(i, k) * b.At(k, j);
                CHECK(Near(product.At(i, j), expected, 1e-12));
            }
        }
    }

    const num::SparseMatrix a = Laplacian3D(16);
    num::MultigridOptions options;
    num::AlgebraicMultigridPreconditioner amg;
    std::string error;
    if (!amg.Build(a, options, &error)) {
        std::printf("  build failed: %s\n", error.c_str());
        CHECK(false);
        return;
    }
    std::printf("  %d levels, operator complexity %.2f:", amg.Levels(), amg.OperatorComplexity());
    for (int level = 0; level < amg.Levels(); ++level) {
        std::printf(" %d", amg.RowsAtLevel(level));
    }
    std::printf("\n");

    // IT HAS TO ACTUALLY COARSEN. A hierarchy whose levels barely shrink
    // costs the work of every level and buys the convergence of none, and
    // it is the most common way an aggregation scheme fails while
    // appearing to work.
    CHECK(amg.Levels() >= 3);
    for (int level = 1; level < amg.Levels(); ++level) {
        CHECK(amg.RowsAtLevel(level) < amg.RowsAtLevel(level - 1) * 3 / 4);
    }
    // And it has to be affordable. Much above two and the cycle costs
    // more than it saves.
    CHECK(amg.OperatorComplexity() > 1.0);
    CHECK(amg.OperatorComplexity() < 2.5);

    // THE V-CYCLE MUST BE SYMMETRIC OR CONJUGATE GRADIENTS IS INVALID.
    // Not approximately: the method's whole derivation assumes the
    // preconditioner is a symmetric positive-definite operator, and an
    // asymmetric one does not merely converge more slowly, it converges
    // to the wrong thing or wanders. This is why the smoother is damped
    // Jacobi and why there are the same number of sweeps before and after
    // the coarse solve, and it is worth checking rather than arguing.
    {
        const int n = a.Rows();
        std::vector<double> u(static_cast<std::size_t>(n), 0.0);
        std::vector<double> v(static_cast<std::size_t>(n), 0.0);
        for (int i = 0; i < n; ++i) {
            u[static_cast<std::size_t>(i)] = std::sin(0.7 * static_cast<double>(i));
            v[static_cast<std::size_t>(i)] = std::cos(0.3 * static_cast<double>(i) + 1.0);
        }
        std::vector<double> mu;
        std::vector<double> mv;
        amg.Apply(u, &mu);
        amg.Apply(v, &mv);
        double left = 0.0;
        double right = 0.0;
        double scale = 0.0;
        for (int i = 0; i < n; ++i) {
            left += mu[static_cast<std::size_t>(i)] * v[static_cast<std::size_t>(i)];
            right += u[static_cast<std::size_t>(i)] * mv[static_cast<std::size_t>(i)];
            scale += std::fabs(mu[static_cast<std::size_t>(i)] * v[static_cast<std::size_t>(i)]);
        }
        std::printf("  <Mu,v> - <u,Mv> = %.3e against %.3e\n", left - right, scale);
        CHECK(std::fabs(left - right) < scale * 1e-10);
        // Positive definite, which is the other half of the requirement.
        double energy = 0.0;
        for (int i = 0; i < n; ++i) {
            energy += u[static_cast<std::size_t>(i)] * mu[static_cast<std::size_t>(i)];
        }
        CHECK(energy > 0.0);
    }

    // THE DEFINING PROPERTY: the iteration count stops depending on the
    // mesh. A local preconditioner damps only the error that varies
    // quickly, so the count grows as the grid is refined; multigrid
    // represents the slowly varying error where it is no longer slowly
    // varying. This is the whole reason the part exists, and a table is
    // the only honest way to show it -- one grid size would prove nothing.
    std::printf("  %-8s %-10s %-10s %-10s %-10s\n", "grid", "unknowns", "jacobi", "ic(0)", "amg");
    std::vector<int> jacobi_counts;
    std::vector<int> cholesky_counts;
    std::vector<int> amg_counts;
    for (const int m : {8, 12, 16, 20}) {
        const num::SparseMatrix problem = Laplacian3D(m);
        std::vector<double> b(static_cast<std::size_t>(problem.Rows()), 1.0);
        int counts[3] = {0, 0, 0};
        const num::Preconditioner kinds[3] = {num::Preconditioner::Jacobi,
                                              num::Preconditioner::IncompleteCholesky,
                                              num::Preconditioner::AlgebraicMultigrid};
        for (int which = 0; which < 3; ++which) {
            num::IterativeOptions opt;
            opt.preconditioner = kinds[which];
            opt.tolerance = 1e-10;
            opt.max_iterations = 4000;
            std::vector<double> x;
            const num::IterativeResult result = num::ConjugateGradient(problem, b, &x, opt);
            CHECK(result.converged);
            counts[which] = result.iterations;
            // And it solved the problem, whichever preconditioner was used.
            std::vector<double> ax;
            problem.Multiply(x, &ax);
            double worst = 0.0;
            for (std::size_t i = 0; i < ax.size(); ++i) {
                worst = std::max(worst, std::fabs(ax[i] - 1.0));
            }
            CHECK(worst < 1e-6);
        }
        std::printf("  %-8d %-10d %-10d %-10d %-10d\n", m, problem.Rows(), counts[0], counts[1],
                    counts[2]);
        jacobi_counts.push_back(counts[0]);
        cholesky_counts.push_back(counts[1]);
        amg_counts.push_back(counts[2]);
    }
    // THE COMPARISON IS BETWEEN THE TRENDS, NOT BETWEEN TWO NUMBERS. On
    // any single grid a preconditioner can be made to look good by
    // tuning, and on a coarse one multigrid loses to incomplete Cholesky
    // outright -- it is a method for large problems and says so. What
    // makes it different in kind is that the local preconditioners' counts
    // grow with the mesh and multigrid's does not.
    CHECK(jacobi_counts.back() > jacobi_counts.front() * 2);
    CHECK(cholesky_counts.back() > cholesky_counts.front() * 3 / 2);
    CHECK(amg_counts.back() < amg_counts.front() * 13 / 10);
    // So its advantage widens as the problem grows, which is the claim
    // stated as a number: the ratio at the finest grid is at least twice
    // the ratio at the coarsest.
    const double coarse_ratio = static_cast<double>(jacobi_counts.front()) /
                                static_cast<double>(amg_counts.front());
    const double fine_ratio = static_cast<double>(jacobi_counts.back()) /
                              static_cast<double>(amg_counts.back());
    std::printf("  multigrid's advantage over Jacobi: %.2fx at %d unknowns, %.2fx at %d\n",
                coarse_ratio, 512, fine_ratio, 8000);
    CHECK(fine_ratio > coarse_ratio * 2.0);
    CHECK(amg_counts.back() * 2 < jacobi_counts.back());
}

void TestOutOfCore() {
    std::printf("a factor solved from disk:\n");
    const num::SparseMatrix a = Laplacian3D(12);
    num::SparseLDLT ldlt;
    CHECK(ldlt.Factorize(a, num::Ordering::ApproximateMinimumDegree));
    const long long in_memory = ldlt.BytesInMemory();
    std::vector<std::vector<double>> answers;
    // Several right-hand sides, because that is the case spilling is for:
    // one factorization solved against many times.
    for (int which = 0; which < 3; ++which) {
        std::vector<double> b(static_cast<std::size_t>(a.Rows()), 0.0);
        for (std::size_t i = 0; i < b.size(); ++i) {
            b[i] = 1.0 + static_cast<double>((i * 7 + static_cast<std::size_t>(which)) % 5);
        }
        std::vector<double> x;
        CHECK(ldlt.Solve(b, &x));
        answers.push_back(x);
    }

    const std::string path = std::string(std::getenv("TMPDIR") != nullptr ? std::getenv("TMPDIR")
                                                                         : "/tmp") +
                             "/mep-num-sparse-spill.bin";
    std::string error;
    if (!ldlt.SpillTo(path, &error)) std::printf("  %s\n", error.c_str());
    CHECK(ldlt.IsSpilled());
    // THE MEMORY IS ACTUALLY RELEASED, which is the only reason to do
    // this. A vector that has been cleared still holds its allocation, so
    // the test asks for the number rather than trusting the call.
    std::printf("  %lld bytes of factor in memory, %lld after spilling to a file\n", in_memory,
                ldlt.BytesInMemory());
    CHECK(in_memory > 0);
    CHECK(ldlt.BytesInMemory() == 0);

    // AND THE ANSWERS ARE THE SAME BITS. Streaming the factor from a file
    // changes the order nothing is read in and none of the arithmetic, so
    // agreement to round-off would not be good enough -- it would mean
    // something had changed that should not have.
    for (int which = 0; which < 3; ++which) {
        std::vector<double> b(static_cast<std::size_t>(a.Rows()), 0.0);
        for (std::size_t i = 0; i < b.size(); ++i) {
            b[i] = 1.0 + static_cast<double>((i * 7 + static_cast<std::size_t>(which)) % 5);
        }
        std::vector<double> x;
        CHECK(ldlt.Solve(b, &x));
        CHECK(x.size() == answers[static_cast<std::size_t>(which)].size());
        for (std::size_t i = 0; i < x.size(); ++i) {
            CHECK(x[i] == answers[static_cast<std::size_t>(which)][i]);
        }
    }

    // Read back into memory, and it is the same factor again.
    if (!ldlt.Reload(&error)) std::printf("  %s\n", error.c_str());
    CHECK(!ldlt.IsSpilled());
    CHECK(ldlt.BytesInMemory() == in_memory);
    std::vector<double> b(static_cast<std::size_t>(a.Rows()), 1.0);
    std::vector<double> x;
    CHECK(ldlt.Solve(b, &x));
    std::vector<double> ax;
    a.Multiply(x, &ax);
    for (std::size_t i = 0; i < ax.size(); ++i) CHECK(Near(ax[i], 1.0, 1e-8));

    // A spilled factor whose file has gone is a failed solve, not a
    // solve with whatever was in the buffer. This is the failure mode
    // worth checking, because the rest of the object still looks
    // perfectly factorized.
    CHECK(ldlt.SpillTo(path, &error));
    CHECK(std::remove(path.c_str()) == 0);
    CHECK(!ldlt.Solve(b, &x));
    CHECK(!ldlt.Reload(&error));
    std::printf("  with the file gone: %s\n", error.c_str());
}

void TestAmdScales() {
    std::printf("ordering a three-dimensional problem:\n");
    // THE SIZE IS THE TEST. Exact minimum degree forms every clique
    // explicitly, so it used to be switched off above twenty thousand
    // unknowns and replaced with Cuthill-McKee -- a correctness-adjacent
    // fallback, because the caller asked for one ordering and got
    // another. AMD is left on at every size, so what has to be shown is
    // that it stays fast where the exact one does not, and that its fill
    // is no worse for it.
    for (const int m : {8, 14, 20}) {
        const num::SparseMatrix a = Laplacian3D(m);
        int amd_fill = 0;
        int md_fill = 0;
        int rcm_fill = 0;
        const double amd_seconds = SecondsFor(a, num::Ordering::ApproximateMinimumDegree, &amd_fill);
        const double rcm_seconds = SecondsFor(a, num::Ordering::ReverseCuthillMcKee, &rcm_fill);
        const double md_seconds = SecondsFor(a, num::Ordering::MinimumDegree, &md_fill);
        std::printf("  %5d unknowns:  amd %8d in %6.3fs   mindeg %8d in %6.3fs   rcm %8d in "
                    "%6.3fs\n",
                    a.Rows(), amd_fill, amd_seconds, md_fill, md_seconds, rcm_fill, rcm_seconds);
        CHECK(amd_fill > 0);
        CHECK(md_fill > 0);
        // Both minimum-degree variants beat a bandwidth ordering in three
        // dimensions, which is the whole reason to prefer them.
        CHECK(amd_fill < rcm_fill);
        CHECK(amd_fill < md_fill * 1.5);
        // And AMD is not slower than the exact one it replaces. On the
        // smallest problem the two are too quick to compare, so this only
        // asks at the size where it matters.
        if (a.Rows() > 2000) CHECK(amd_seconds <= md_seconds);

        // It still has to solve the problem. An ordering is a permutation
        // and nothing else, so a wrong one shows up here rather than in
        // the fill count.
        num::SparseLDLT ldlt;
        std::string ignored;
        CHECK(ldlt.Factorize(a, num::Ordering::ApproximateMinimumDegree));
        std::vector<double> b(static_cast<std::size_t>(a.Rows()), 1.0);
        std::vector<double> x;
        CHECK(ldlt.Solve(b, &x));
        std::vector<double> ax;
        a.Multiply(x, &ax);
        double worst = 0.0;
        for (std::size_t i = 0; i < ax.size(); ++i) worst = std::max(worst, std::fabs(ax[i] - 1.0));
        CHECK(worst < 1e-8);
    }
}

void TestIterative() {
    double h = 0.0;
    const num::SparseMatrix a = Laplacian2D(16, &h);
    const int n = a.Rows();
    std::vector<double> b(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) b[static_cast<std::size_t>(i)] = std::sin(static_cast<double>(i) * 0.1);

    num::SparseLDLT direct;
    CHECK(direct.Factorize(a));
    std::vector<double> exact;
    CHECK(direct.Solve(b, &exact));

    const num::Preconditioner kinds[3] = {num::Preconditioner::None, num::Preconditioner::Jacobi,
                                          num::Preconditioner::IncompleteCholesky};
    int iterations[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) {
        num::IterativeOptions opt;
        opt.preconditioner = kinds[k];
        opt.tolerance = 1e-12;
        opt.max_iterations = 5000;
        std::vector<double> x;
        const num::IterativeResult r = num::ConjugateGradient(a, b, &x, opt);
        CHECK(r.converged);
        iterations[k] = r.iterations;
        // Agreement with the direct solve, not merely a small residual.
        for (std::size_t i = 0; i < x.size(); ++i) CHECK(Near(x[i], exact[i], 1e-8));
    }
    std::printf("  CG iterations: none=%d jacobi=%d ic0=%d\n", iterations[0], iterations[1], iterations[2]);
    // IC(0) must be a real improvement over no preconditioning, or it is
    // not worth its cost. (Jacobi on this matrix is a constant diagonal,
    // so it changes nothing -- which is itself worth asserting, since a
    // Jacobi that *did* change the count would mean a scaling bug.)
    CHECK(iterations[1] == iterations[0]);
    CHECK(iterations[2] < iterations[0]);

    // A zero right-hand side is a real case (an unloaded body) and must
    // not divide by zero in the relative-residual test.
    std::vector<double> zero_b(static_cast<std::size_t>(n), 0.0);
    std::vector<double> zero_x;
    const num::IterativeResult rz = num::ConjugateGradient(a, zero_b, &zero_x);
    CHECK(rz.converged);
    for (double v : zero_x) CHECK(v == 0.0);

    // CG on a matrix that is not positive definite must say so rather
    // than iterating to a meaningless answer.
    std::vector<num::Triplet> nt;
    nt.push_back(num::Triplet{0, 0, 1.0});
    nt.push_back(num::Triplet{1, 1, -1.0});
    const num::SparseMatrix not_spd = num::SparseMatrix::FromTriplets(2, 2, nt);
    std::vector<double> nb;
    nb.push_back(1.0);
    nb.push_back(1.0);
    std::vector<double> nx;
    num::IterativeOptions nopt;
    nopt.preconditioner = num::Preconditioner::None;
    const num::IterativeResult rn = num::ConjugateGradient(not_spd, nb, &nx, nopt);
    CHECK(!rn.converged);
    CHECK(!rn.message.empty());
}

void TestGmres() {
    // 1D advection-diffusion: -u'' + beta*u' = 1, central differences.
    // The first-derivative term makes the matrix unsymmetric, which is
    // precisely the case CG cannot handle and GMRES exists for.
    const int n = 60;
    const double h = 1.0 / (static_cast<double>(n) + 1.0);
    const double beta = 4.0;
    std::vector<num::Triplet> t;
    for (int i = 0; i < n; ++i) {
        t.push_back(num::Triplet{i, i, 2.0 / (h * h)});
        if (i > 0) t.push_back(num::Triplet{i, i - 1, -1.0 / (h * h) - beta / (2.0 * h)});
        if (i < n - 1) t.push_back(num::Triplet{i, i + 1, -1.0 / (h * h) + beta / (2.0 * h)});
    }
    const num::SparseMatrix a = num::SparseMatrix::FromTriplets(n, n, t);
    CHECK(!a.IsSymmetric());

    std::vector<double> b(static_cast<std::size_t>(n), 1.0);
    std::vector<double> x;
    num::IterativeOptions opt;
    opt.tolerance = 1e-11;
    opt.restart = 30;
    opt.max_iterations = 2000;
    const num::IterativeResult r = num::Gmres(a, b, &x, opt);
    CHECK(r.converged);

    // The reported residual must be the true one, not the internal
    // estimate -- the check the implementation's own comment promises.
    std::vector<double> ax;
    a.Multiply(x, &ax);
    double residual = 0.0;
    for (std::size_t i = 0; i < ax.size(); ++i) residual += (ax[i] - b[i]) * (ax[i] - b[i]);
    CHECK(std::sqrt(residual) / num::Norm2(b) <= 1e-10);

    // GMRES on a symmetric system must agree with CG on the same system.
    double hh = 0.0;
    const num::SparseMatrix spd = Laplacian2D(8, &hh);
    std::vector<double> sb(static_cast<std::size_t>(spd.Rows()), 1.0);
    std::vector<double> gx;
    std::vector<double> cx;
    num::IterativeOptions gopt;
    gopt.tolerance = 1e-12;
    gopt.restart = 50;
    gopt.max_iterations = 5000;
    CHECK(num::Gmres(spd, sb, &gx, gopt).converged);
    CHECK(num::ConjugateGradient(spd, sb, &cx, gopt).converged);
    for (std::size_t i = 0; i < gx.size(); ++i) CHECK(Near(gx[i], cx[i], 1e-7));
}

// The real verification: a problem whose exact solution is known, solved
// on two grids, checked for the convergence *rate* the discretization
// promises. See this file's header comment for why a residual check is
// not a substitute for this.
void TestManufacturedSolution() {
    // -Laplacian(u) = 2*pi^2*sin(pi x)*sin(pi y) on the unit square, with
    // u = 0 on the boundary. Exact solution: u = sin(pi x)*sin(pi y).
    // The 5-point stencil is second-order accurate, so halving h must cut
    // the maximum error by about four.
    auto solve_on_grid = [](int n) {
        double h = 0.0;
        const num::SparseMatrix a = Laplacian2D(n, &h);
        std::vector<double> b(static_cast<std::size_t>(n * n), 0.0);
        std::vector<double> exact(static_cast<std::size_t>(n * n), 0.0);
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                const double x = (static_cast<double>(i) + 1.0) * h;
                const double y = (static_cast<double>(j) + 1.0) * h;
                const std::size_t row = static_cast<std::size_t>(i * n + j);
                b[row] = 2.0 * cad::kPi * cad::kPi * std::sin(cad::kPi * x) * std::sin(cad::kPi * y);
                exact[row] = std::sin(cad::kPi * x) * std::sin(cad::kPi * y);
            }
        }
        num::SparseLDLT ldlt;
        if (!ldlt.Factorize(a)) return -1.0;
        std::vector<double> u;
        if (!ldlt.Solve(b, &u)) return -1.0;
        double max_error = 0.0;
        for (std::size_t i = 0; i < u.size(); ++i) max_error = std::max(max_error, std::fabs(u[i] - exact[i]));
        return max_error;
    };

    const double coarse = solve_on_grid(15);
    const double fine = solve_on_grid(31);
    CHECK(coarse > 0.0);
    CHECK(fine > 0.0);
    const double ratio = coarse / fine;
    // Second order means a ratio near 4. A band rather than an exact
    // value, because the grids are finite and the constant is not.
    std::printf("  manufactured solution: coarse=%.3e fine=%.3e ratio=%.2f\n", coarse, fine, ratio);
    CHECK(ratio > 3.5 && ratio < 4.5);
    // And the absolute error has to be small, not merely converging.
    CHECK(fine < 5e-3);

    // The same problem through CG must reach the same answer -- if the
    // iterative and direct paths disagree here, one of them is solving a
    // different system.
    double h = 0.0;
    const num::SparseMatrix a = Laplacian2D(15, &h);
    std::vector<double> b(static_cast<std::size_t>(15 * 15), 0.0);
    for (int i = 0; i < 15; ++i) {
        for (int j = 0; j < 15; ++j) {
            const double x = (static_cast<double>(i) + 1.0) * h;
            const double y = (static_cast<double>(j) + 1.0) * h;
            b[static_cast<std::size_t>(i * 15 + j)] =
                2.0 * cad::kPi * cad::kPi * std::sin(cad::kPi * x) * std::sin(cad::kPi * y);
        }
    }
    num::SparseLDLT ldlt;
    CHECK(ldlt.Factorize(a));
    std::vector<double> direct_u;
    CHECK(ldlt.Solve(b, &direct_u));
    std::vector<double> cg_u;
    num::IterativeOptions opt;
    opt.tolerance = 1e-13;
    opt.max_iterations = 5000;
    CHECK(num::ConjugateGradient(a, b, &cg_u, opt).converged);
    for (std::size_t i = 0; i < direct_u.size(); ++i) CHECK(Near(cg_u[i], direct_u[i], 1e-7));
}

// One factorization reused for many right-hand sides -- the property that
// makes a direct solver worth its memory for transient and modal
// analyses, both of which solve with the same matrix hundreds of times.
void TestMultipleRightHandSides() {
    double h = 0.0;
    const num::SparseMatrix a = Laplacian2D(10, &h);
    num::SparseLDLT ldlt;
    CHECK(ldlt.Factorize(a));
    for (int trial = 0; trial < 20; ++trial) {
        std::vector<double> b(static_cast<std::size_t>(a.Rows()), 0.0);
        for (std::size_t i = 0; i < b.size(); ++i) {
            b[i] = std::cos(static_cast<double>(i) * 0.37 + static_cast<double>(trial));
        }
        std::vector<double> x;
        CHECK(ldlt.Solve(b, &x));
        std::vector<double> ax;
        a.Multiply(x, &ax);
        for (std::size_t i = 0; i < ax.size(); ++i) CHECK(Near(ax[i], b[i], 1e-7));
    }
}

}  // namespace

int main() {
    TestAssembly();
    TestOrderings();
    TestDirectSolve();
    TestFillReduction();
    TestAmdScales();
    TestOutOfCore();
    TestMultigrid();
    TestIterative();
    TestGmres();
    TestManufacturedSolution();
    TestMultipleRightHandSides();
    std::printf("num_sparse_test passed (%d checks)\n", g_checks);
    return 0;
}
