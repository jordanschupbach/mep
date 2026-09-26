#ifndef MEP_FEM_SOLVE_H
#define MEP_FEM_SOLVE_H

#include "cad_math.h"
#include "fem_model.h"
#include "num_sparse.h"

#include <functional>
#include <string>
#include <vector>

// Linear static finite-element analysis (plans/CAD_FEM_PLAN.md Part 0.6,
// and the seed of Parts H.1, H.3 and H.6).
//
// What this is: isotropic linear elasticity, small strain, small
// displacement, Hex8 elements, solved directly. It is the narrowest
// useful solver there is, and deliberately so -- Part 0.6 exists to prove
// the whole pipeline from geometry to a coloured picture works end to
// end, on a problem whose answer is known independently, before any of
// the harder physics is built on top of it.
//
// What it is not, and what each omission costs, so nobody discovers these
// by being surprised by a result:
//   * No large deformation, no plasticity, no contact (Part I.6-I.8).
//     Results scale linearly with load, which is exactly wrong once a
//     structure yields or parts touch.
//   * Hex8 only. Tet4/Tet10 and the second-order elements that make
//     curved boundaries worth having arrive with the mesher (Part G.5).
//   * Loads are nodal forces only. Pressure on a face, gravity and
//     thermal expansion are all Part H.2's consistent load vectors.
namespace fem {

// Where a stress tensor was evaluated. Gauss-point values are what the
// element actually computes; nodal values are extrapolated and averaged
// from them, which is what a contour plot needs and also what makes the
// averaging error visible (see StressField's own note).
struct StressTensor {
    // Voigt order throughout: xx, yy, zz, xy, yz, zx.
    double s[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

    double VonMises() const;
    // The three principal stresses, sorted descending. Needed by the
    // failure criteria in Part J.1 and useful on its own for reading a
    // result: a von Mises plot cannot distinguish tension from
    // compression, and plenty of materials care a great deal.
    void Principal(double *out_sorted_three) const;
};

struct Result {
    bool ok = false;
    std::string error;

    // One displacement vector per node, in model node order.
    std::vector<cad::Vec3d> displacements;
    // Stress at each node, averaged across every element touching it.
    std::vector<StressTensor> nodal_stress;
    // Stress at each element's centroid -- unaveraged, so a large
    // disagreement between this and the averaged nodal values at the same
    // place is a direct measure of discretization error rather than
    // noise. Part J.5's error estimator is built on exactly that gap.
    std::vector<StressTensor> element_stress;
    // Reaction force at every constrained degree of freedom, summed per
    // node. Their total must balance the applied load, which is the
    // cheapest global check there is that the solve was right, and is
    // asserted by Solve itself.
    std::vector<cad::Vec3d> reactions;

    // Summary values, cheap to report and the first thing anyone looks at.
    double max_displacement_magnitude = 0.0;
    int max_displacement_node = -1;
    double max_von_mises = 0.0;
    int max_von_mises_node = -1;
    // Residual of the equilibrium check described above, relative to the
    // applied load magnitude. Should be at round-off.
    double equilibrium_residual = 0.0;
    int solver_iterations = 0;  // 0 for the direct solver
    std::string solver_used;
};

enum class SolverKind {
    // Sparse LDL^T. The right default at this scale: one factorization
    // answers any number of load cases.
    Direct,
    // Preconditioned conjugate gradients, for when the factorization
    // stops fitting in memory.
    Iterative,
};

struct SolveOptions {
    SolverKind solver = SolverKind::Direct;
    num::Ordering ordering = num::Ordering::MinimumDegree;
    double iterative_tolerance = 1e-10;
    int iterative_max_iterations = 5000;
    // Called with a 0..1 fraction and a stage name, if set. The mep-fem
    // process streams these back as JSON-RPC notifications so a long
    // solve shows progress rather than appearing hung.
    std::function<void(double, const std::string &)> progress;
};

// The isotropic 3D constitutive matrix (6x6, Voigt order), exposed
// because the element routine, the stress recovery and the tests all need
// exactly the same one and a second copy would be a second thing to get
// wrong.
void ConstitutiveMatrix(const Material &material, double out[6][6]);

// One Hex8 element's 24x24 stiffness matrix, integrated with 2x2x2 Gauss
// quadrature.
//
// Full integration, not reduced. That choice has a known cost: a
// fully-integrated trilinear hexahedron exhibits shear locking, meaning
// it is artificially stiff in bending, badly so when the element is thin
// in the bending direction. The honest fix is B-bar or selective reduced
// integration (Part H.1 lists it), and it is not here yet -- so the
// cantilever check in the tests converges toward beam theory from below
// and needs several elements through the thickness to get close. Reduced
// integration alone would trade locking for hourglassing, which is worse
// because it is not conservative.
void Hex8Stiffness(const Model &model, const Element &element, double out[24][24]);

// Assembles the global stiffness matrix. Exposed separately from Solve
// because modal analysis, buckling and every nonlinear iteration in Part
// I reuse exactly this, and because a test can check its symmetry and
// rank without solving anything.
num::SparseMatrix AssembleStiffness(const Model &model);

Result Solve(const Model &model, const SolveOptions &options = {});

// JSON encoding of a result, the other half of the mep-fem wire format.
std::string ResultToJson(const Result &result);

}  // namespace fem

#endif
