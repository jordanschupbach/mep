#ifndef MEP_FEM_STATIC_H
#define MEP_FEM_STATIC_H

#include "fem_assemble.h"
#include "fem_result.h"
#include "fem_solve.h"
#include "fem_study.h"
#include "num_sparse.h"

#include <string>
#include <vector>

// Linear static analysis (plans/CAD_FEM_PLAN.md Part H.6).
//
// The same physics Part 0.6's solver does -- isotropic linear elasticity,
// small strain, small displacement -- on the analysis model Parts H.1 to
// H.3 built instead of on the eight-node-only one Part 0.6 could offer.
// Any element the library has, any material the study carries, any
// constraint the assembly can apply.
//
// HOW THIS IS VERIFIED, AND WHY A RESIDUAL IS NOT ENOUGH. A linear
// solver can satisfy ||Ku - f|| to round-off while solving the wrong
// problem, because the residual only ever sees the matrix it was handed:
// a wrong element, a wrong load vector and a wrong constraint all produce
// a system that is solved perfectly. So the tests do three things the
// residual cannot:
//
//   * A MANUFACTURED SOLUTION. Choose a displacement field, work out
//     analytically what body force it is the answer to, apply that force,
//     and compare against the field. The error must fall at the rate the
//     element's own order predicts -- squared in h for a linear element,
//     cubed for a quadratic one -- and a rate is far harder to fake than
//     a number: an element that is wrong converges at the wrong rate or
//     to the wrong thing.
//   * PUBLISHED BENCHMARKS, whose reference values were computed by other
//     people with other codes.
//   * ANOTHER SOLVER ENTIRELY, on the identical mesh.
namespace fem {

struct StaticOptions {
    // Direct is right up to the point where the factor stops fitting.
    SolverKind solver = SolverKind::Direct;
    num::Ordering ordering = num::Ordering::ApproximateMinimumDegree;
    num::MultigridOptions multigrid;
    double iterative_tolerance = 1e-10;
    int iterative_max_iterations = 5000;
    // Supply the rigid-body modes to the multigrid preconditioner. Almost
    // always what is wanted -- see the note in num_sparse.h -- and off
    // only to demonstrate what happens without them.
    bool rigid_body_near_null_space = true;
    AssemblyOptions assembly;
    // How the stress gets from the quadrature points to the nodes (J.1).
    // `AtNodes` is the naive way and is kept to measure the other against.
    Recovery recovery = Recovery::Extrapolated;
};

struct StaticResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;

    std::vector<cad::Vec3d> displacement;
    // One tensor per element, at its centroid: unaveraged, so the gap
    // between this and the averaged nodal value at the same place is a
    // direct measure of discretisation error rather than of noise. Part
    // J.5's estimator is built on exactly that gap.
    std::vector<StressTensor> element_stress;
    // Averaged over every element touching the node.
    std::vector<StressTensor> nodal_stress;
    // The full recovered field: the two above are views into it, kept
    // because they were the shape of this result before J.1 and plenty
    // of callers only want them. The unaveraged per-element-per-node
    // values and the discontinuity map live only here.
    StressField stress;
    std::vector<cad::Vec3d> reaction;

    double max_displacement = 0.0;
    int max_displacement_node = -1;
    double max_von_mises = 0.0;
    int max_von_mises_node = -1;
    // The work the loads did on the displacements, which equals twice the
    // stored energy for a linear problem and is the single most useful
    // scalar for comparing two solutions of the same problem.
    double strain_energy = 0.0;
    // ||sum of reactions + sum of loads|| over the applied load, which
    // should be at round-off. Checked by SolveStatic itself.
    double equilibrium_residual = 0.0;
    int iterations = 0;
    std::string solver_used;
};

bool SolveStatic(const AnalysisModel &model,
                 const std::vector<MultiPointConstraint> &constraints,
                 const StaticOptions &options, StaticResult *out);

// The stress at one point of one element, from a full displacement
// vector. Exposed because a test comparing against an analytic field
// wants to ask at its own points rather than at the ones the solver chose.
bool ElementStressAt(const AnalysisModel &model, int element, const cad::Vec3d &reference,
                     const std::vector<double> &displacement, StressTensor *out,
                     std::string *error);

// The consistent nodal load of thermal expansion (Part I.3).
//
// A temperature change wants to strain the material by
// `alpha * (T - T_ref)` in every direction, and the load that produces
// that strain is `integral(B^T D epsilon_thermal)`. Two things make this
// worth its own function rather than a term inside the assembly:
//
//   * IT IS INTEGRATED AGAINST THE FIELD, not against an element average.
//     A uniform temperature on an unconstrained body produces *no stress*
//     -- the thermal load and the stiffness cancel exactly -- so any error
//     in this integral shows up as stress in a body that should have none,
//     which is the sharpest test there is of both.
//   * It is a load like any other, so it superposes with pressure and
//     gravity, and a caller can see how much of an answer is thermal by
//     leaving it out.
bool ThermalLoad(const AnalysisModel &model, std::vector<NodalLoad> *out, std::string *error);

// The six rigid-body modes of a model, restricted to the degrees of
// freedom a system actually carries. What a multigrid preconditioner
// needs for elasticity, and what a test checking an assembly's rank uses.
std::vector<std::vector<double>> RigidBodyModes(const AnalysisModel &model, const System &system);

}  // namespace fem

#endif
