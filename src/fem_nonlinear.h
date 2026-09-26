#ifndef MEP_FEM_NONLINEAR_H
#define MEP_FEM_NONLINEAR_H

#include "fem_assemble.h"
#include "fem_contact.h"
#include "fem_static.h"

#include <functional>
#include <string>
#include <vector>

// Geometric nonlinearity (plans/CAD_FEM_PLAN.md Part I.6).
//
// TOTAL LAGRANGIAN: everything is integrated over the *undeformed* body,
// with the Green-Lagrange strain and the second Piola-Kirchhoff stress.
// The alternative -- updating the reference configuration as the body
// moves -- needs the same equations rewritten each step and buys nothing
// for a hyperelastic material.
//
// WHY THE STRAIN MEASURE IS THE WHOLE POINT. Linear analysis uses
// `(grad u + grad u^T)/2`, which is not zero for a rigid rotation: rotate
// a body ninety degrees and it reports a strain of one, so a solver built
// on it tears a rotating part apart. Green-Lagrange,
// `E = (F^T F - I)/2`, is exactly zero for any rigid rotation however
// large, and that single property is what "geometrically nonlinear"
// means. It is also the sharpest available test, because it is exact
// rather than asymptotic.
//
// NEWTON WITH A CONSISTENT TANGENT, AND THE PROOF IS THE CONVERGENCE
// RATE. The tangent has two parts -- the material one, which is the
// linear stiffness with the deformation folded in, and the geometric one,
// which is the same stress-stiffness term Part I.5 assembles. Leave
// either out and Newton still converges, linearly, which looks like a
// hard problem rather than a wrong derivative. So the residual is
// reported at every iteration and the tests check that it *squares*.
//
// AND ARC LENGTH, BECAUSE LOAD CONTROL CANNOT PASS A LIMIT POINT. A
// shallow arch pushed down reaches a load beyond which no equilibrium
// exists at all; Newton under load control diverges there, and the honest
// reading of that divergence is not "the solver failed" but "there is
// nothing to find". Arc length makes the load factor an unknown and
// constrains the *distance* travelled along the equilibrium path instead,
// which walks round the limit point and down the far side.
namespace fem {

struct NonlinearOptions {
    // Load steps for the load-controlled path.
    int steps = 10;
    int max_iterations = 30;
    // On the residual, relative to the applied load.
    double tolerance = 1e-9;
    // Halve the step and try again, up to this many times, when a step
    // will not converge.
    int max_cuts = 6;
    // Line search on the Newton direction. Cheap insurance: a full
    // Newton step can overshoot badly early in a strongly nonlinear
    // problem, and backtracking on the energy costs one residual
    // evaluation per trial.
    bool line_search = true;
    num::Ordering ordering = num::Ordering::ApproximateMinimumDegree;
    AssemblyOptions assembly;
    // Rigid obstacles the model may not pass through. Empty for none.
    ContactSet contact;
};

struct ArcLengthOptions {
    int steps = 40;
    int max_iterations = 30;
    double tolerance = 1e-9;
    // The first step's arc length, as a fraction of the displacement a
    // linear solve under the full load would give. Zero picks 1/steps.
    double first_step = 0.0;
    // Adapt the arc length to how hard the last step was.
    bool adaptive = true;
    double max_factor = 2.0;
    num::Ordering ordering = num::Ordering::ApproximateMinimumDegree;
    AssemblyOptions assembly;
};

struct NonlinearStep {
    double load_factor = 0.0;
    std::vector<cad::Vec3d> displacement;
    int iterations = 0;
    double residual = 0.0;
};

struct NonlinearResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    std::vector<NonlinearStep> path;
    // The last converged state.
    std::vector<cad::Vec3d> displacement;
    double load_factor = 0.0;
    int total_iterations = 0;
    int cuts = 0;
    // The residual of each Newton iteration of the last step, so that a
    // caller -- or a test -- can see it square.
    std::vector<double> last_residuals;
    // How many nodes were touching at the end, and by how much they had
    // sunk in. The penetration is the penalty method's error made visible:
    // it is the price of the method, and reporting it lets a caller decide
    // whether the price was worth paying.
    int touching = 0;
    double worst_penetration = 0.0;
};

// The internal force and consistent tangent of the whole model at a
// displacement state. Exposed because every check worth making here is
// about these two rather than about a solution: the internal force of a
// rigidly rotated body must vanish, and the tangent must be the
// derivative of the internal force, which a finite difference can test.
bool InternalForce(const AnalysisModel &model, const std::vector<double> &displacement,
                   const SparsityPattern &pattern, std::vector<double> *out_force,
                   num::SparseMatrix *out_tangent, std::string *error);

// Load-controlled Newton-Raphson, in steps.
bool SolveNonlinear(const AnalysisModel &model,
                    const std::vector<MultiPointConstraint> &constraints,
                    const NonlinearOptions &options, NonlinearResult *out);

// Riks arc-length continuation, which can pass a limit point.
bool SolveArcLength(const AnalysisModel &model,
                    const std::vector<MultiPointConstraint> &constraints,
                    const ArcLengthOptions &options, NonlinearResult *out);

}  // namespace fem

#endif
