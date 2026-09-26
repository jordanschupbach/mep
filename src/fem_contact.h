#ifndef MEP_FEM_CONTACT_H
#define MEP_FEM_CONTACT_H

#include "fem_study.h"
#include "num_sparse.h"

#include <string>
#include <vector>

// Contact (plans/CAD_FEM_PLAN.md Part I.8).
//
// WHAT MAKES CONTACT THE HARDEST PART OF THE PLAN, and why this is the
// easy corner of it. A contact problem is not merely nonlinear, it is
// *non-smooth*: the constraint is an inequality, so the stiffness changes
// discontinuously the instant a node touches, and Newton's quadratic
// convergence assumes a derivative that does not jump. Everything in
// contact -- penalty, Lagrange multipliers, augmentation, active-set
// strategies -- is a way of living with that.
//
// WHAT IS HERE: node-to-surface against a rigid plane, by penalty. A node
// that crosses the plane is pushed back with a force proportional to how
// far it crossed. It is the simplest thing that is genuinely contact, it
// is exactly right in the limit of a large penalty, and its error is
// visible and measurable -- the penetration -- rather than hidden.
//
// WHAT IS NOT, and it is most of the subject: deformable-to-deformable
// contact, where both surfaces move and the pairing has to be found each
// iteration; segment-to-segment mortar, which integrates the constraint
// over the surface rather than sampling it at nodes and is what makes a
// pressure distribution come out smooth; augmented Lagrangian, which
// removes the penetration the penalty leaves; friction, which makes the
// tangent unsymmetric; and self-contact, which needs the search to
// consider a body against itself. Each is listed in the plan and none is
// written.
namespace fem {

// A rigid obstacle: material may not cross to the negative side of the
// plane through `point` with outward `normal`.
struct RigidPlane {
    cad::Vec3d point;
    cad::Vec3d normal{0, 0, 1};
};

struct ContactSet {
    std::vector<RigidPlane> planes;
    // The nodes that may touch. Naming them rather than testing every
    // node is not an optimisation: a node on the far side of the body
    // that happens to lie beyond the plane is not in contact, it is
    // somewhere else, and a search that cannot tell the difference is a
    // search that glues the model to the obstacle.
    std::vector<int> nodes;
    // Force per unit penetration. Relative to the largest diagonal of the
    // stiffness, so that the same number means the same thing whatever
    // the model is made of or measured in.
    double penalty_scale = 1e3;
};

// Adds the contact force and stiffness at a displacement state. Returns
// how many nodes are touching, which is the active set and is worth
// seeing: a solve whose active set is still changing at the last
// iteration has not converged whatever its residual says.
int AddContact(const AnalysisModel &model, const ContactSet &contact, double penalty,
               const std::vector<double> &displacement, std::vector<double> *force,
               std::vector<num::Triplet> *tangent, double *out_worst_penetration);

}  // namespace fem

#endif
