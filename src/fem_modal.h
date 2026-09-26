#ifndef MEP_FEM_MODAL_H
#define MEP_FEM_MODAL_H

#include "fem_assemble.h"
#include "fem_static.h"

#include <string>
#include <vector>

// Modal analysis (plans/CAD_FEM_PLAN.md Part I.4).
//
// The generalised eigenproblem `K phi = lambda M phi`, whose eigenvalues
// are the squared natural frequencies and whose vectors are the shapes
// the structure moves in. Everything here rests on two things Part H
// already built: the assembled stiffness, and a factorization that
// reports how many negative pivots it found.
//
// SHIFT AND INVERT, BECAUSE THE WANTED MODES ARE THE SMALL ONES. A power
// method converges to the *largest* eigenvalue, and the largest natural
// frequency of a finite element mesh is an artefact of the mesh rather
// than anything a structure does. Replacing the operator by
// `(K - sigma M)^-1 M` turns the eigenvalues nearest `sigma` into the
// largest ones, so the same iteration finds the modes that matter and
// finds them fastest. It costs one factorization, which the direct solver
// already does well, and it is why a modal solver is built on a direct
// solver rather than an iterative one.
//
// AND THE STURM SEQUENCE, BECAUSE THE FAILURE MODE IS SILENCE. An
// iterative eigensolver returns the modes it converged to. If it missed
// one -- and a pair of close or repeated frequencies is exactly when it
// does -- nothing about the answer says so: the shapes are orthogonal,
// the frequencies are real and ordered, and a mode is simply absent. The
// count of negative pivots of `K - sigma M` is the number of eigenvalues
// below `sigma`, by Sylvester's law of inertia, and it comes from a
// factorization rather than from the iteration. Comparing the two is the
// only check that catches a missed mode.
namespace fem {

enum class ModalMethod {
    // Subspace iteration: robust, simple, and converges to the lowest
    // modes together, which makes a missed mode less likely to begin
    // with. The right default.
    SubspaceIteration,
    // Lanczos with full reorthogonalisation. Fewer factorized solves for
    // the same number of modes, and more sensitive to losing orthogonality
    // -- which is what the reorthogonalisation is for and what makes it
    // cost memory proportional to the number of steps.
    Lanczos,
};

struct ModalOptions {
    int modes = 6;
    ModalMethod method = ModalMethod::SubspaceIteration;
    // A LUMPED MASS MATRIX IS NOT A CRUDER CONSISTENT ONE, it errs the
    // other way. A consistent mass matrix over-estimates every frequency
    // and a lumped one under-estimates them, so the two together bracket
    // the answer -- which is worth far more than either alone and is what
    // the tests check.
    bool lumped_mass = false;
    // The frequencies are sought nearest this, in Hz. Zero is right for
    // the fundamental modes; a non-zero shift finds the modes near a
    // forcing frequency without computing everything below it.
    double around_frequency = 0.0;
    int max_iterations = 300;
    double tolerance = 1e-10;
    num::Ordering ordering = num::Ordering::ApproximateMinimumDegree;
    // Ask the factorization how many modes lie below the highest one
    // found, and report whether it agrees. Costs one extra factorization
    // and is the only thing that can catch a missed mode.
    bool sturm_check = true;
};

struct ModalResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    // Ascending. Squared angular frequency, rad^2/s^2.
    std::vector<double> eigenvalue;
    // The same, in Hz.
    std::vector<double> frequency;
    // One full-length displacement vector per mode, mass-normalised so
    // that phi^T M phi = 1 -- which is what makes a modal superposition's
    // participation factors mean anything.
    std::vector<std::vector<cad::Vec3d>> shape;
    // How many of the modes found are rigid-body motions, which an
    // unrestrained model has six of and a restrained one none.
    int rigid_body_modes = 0;
    // What the Sturm count said and whether it agreed.
    int modes_below = 0;
    bool sturm_agrees = true;
    int iterations = 0;
};

bool SolveModal(const AnalysisModel &model, const std::vector<MultiPointConstraint> &constraints,
                const ModalOptions &options, ModalResult *out);

// The mass matrix, consistent or lumped, filled into a pattern built for
// the stiffness -- which it fits, because mass couples exactly the degrees
// of freedom an element holds and so has the same structure.
bool AssembleMass(const AnalysisModel &model, const SparsityPattern &pattern, bool lumped,
                  num::SparseMatrix *out, std::string *error);

// How many natural frequencies lie below `frequency`, counted from the
// inertia of `K - sigma M` rather than from any iteration.
bool CountModesBelow(const AnalysisModel &model,
                     const std::vector<MultiPointConstraint> &constraints, double frequency,
                     bool lumped_mass, int *out, std::string *error);

// --- Linear buckling (Part I.5) -------------------------------------------

// The stress stiffness matrix at a given stress state: the second-order
// part of the strain that a displacement-based formulation drops, which
// is what makes a compressed strut softer in bending than an uncompressed
// one and eventually not stiff at all.
//
//     K_G[3a+i][3b+j] = delta_ij * integral( dN_a/dx_k * sigma_kl * dN_b/dx_l )
//
// Symmetric, and *indefinite*: compression makes it negative and tension
// positive, which is the whole physics and is also why the eigenproblem
// below is posed the way round it is.
bool AssembleStressStiffness(const AnalysisModel &model, const StaticResult &state,
                             const SparsityPattern &pattern, num::SparseMatrix *out,
                             std::string *error);

struct BucklingOptions {
    int modes = 4;
    int max_iterations = 300;
    double tolerance = 1e-10;
    num::Ordering ordering = num::Ordering::ApproximateMinimumDegree;
    StaticOptions statics;
};

struct BucklingResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    // The load factors, smallest magnitude first. The structure buckles
    // when the applied load is multiplied by this.
    //
    // A NEGATIVE FACTOR IS NOT A MISTAKE. It means the structure buckles
    // when the load is *reversed* -- a strut in tension, loaded the other
    // way -- and reporting only the positive ones would hide a design
    // whose safety depends on never reversing a load. They are returned
    // and it is the caller's business which matter.
    std::vector<double> factor;
    std::vector<std::vector<cad::Vec3d>> shape;
    // The static solution the stress stiffness came from, since a caller
    // almost always wants both and solving twice would be a waste and a
    // chance for them to disagree.
    StaticResult statics;
    int iterations = 0;
};

// THE SAME EIGENSOLVER, POSED THE OTHER WAY ROUND. Buckling asks for
// `(K + lambda K_G) phi = 0`, and the obvious rearrangement
// `K phi = -lambda K_G phi` has an indefinite matrix on the right, which
// a subspace iteration cannot orthogonalise against. Swapping them --
// `-K_G phi = mu K phi`, with `lambda = 1/mu` -- puts the positive
// definite matrix where the method needs it, and the largest `mu` is the
// smallest load factor, which is the one that matters.
bool SolveBuckling(const AnalysisModel &model,
                   const std::vector<MultiPointConstraint> &constraints,
                   const BucklingOptions &options, BucklingResult *out);

}  // namespace fem

#endif
