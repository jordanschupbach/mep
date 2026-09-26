#include "fem_contact.h"

#include <algorithm>
#include <cmath>

namespace fem {

int AddContact(const AnalysisModel &model, const ContactSet &contact, double penalty,
               const std::vector<double> &displacement, std::vector<double> *force,
               std::vector<num::Triplet> *tangent, double *out_worst_penetration) {
    int touching = 0;
    if (out_worst_penetration != nullptr) *out_worst_penetration = 0.0;
    for (const int node : contact.nodes) {
        if (node < 0 || node >= model.NodeCount()) continue;
        const std::size_t at = static_cast<std::size_t>(node);
        const cad::Vec3d moved{model.nodes[at].x + displacement[at * 3 + 0],
                               model.nodes[at].y + displacement[at * 3 + 1],
                               model.nodes[at].z + displacement[at * 3 + 2]};
        for (const RigidPlane &plane : contact.planes) {
            const cad::Vec3d normal = plane.normal.Normalized();
            const double gap = (moved - plane.point).Dot(normal);
            // THE INEQUALITY IS THE WHOLE DIFFICULTY. A node above the
            // plane contributes nothing at all -- no force, no stiffness,
            // not even a small one -- and the instant it crosses, it
            // contributes both. That discontinuity is why a contact solve
            // is not merely a nonlinear one, and why the active set is
            // reported rather than assumed to have settled.
            if (gap >= 0.0) continue;
            ++touching;
            if (out_worst_penetration != nullptr) {
                *out_worst_penetration = std::max(*out_worst_penetration, -gap);
            }
            // THE SIGN IS THE ONE THE TANGENT BELOW IS THE DERIVATIVE OF,
            // and getting that wrong is not a sign error, it is a
            // different method. The penalty is an energy, penalty*gap^2/2
            // for a penetrating node, so the internal force is its
            // derivative, penalty*gap along the normal -- pointing *into*
            // the plane, because gap is negative -- and the stiffness is
            // its derivative in turn, penalty*n(x)n. Written with the
            // force flipped and the stiffness not, Newton takes steps
            // computed from a matrix that is not the slope of the thing
            // it is driving to zero: it stalled at a fixed residual of
            // 3.9e-01 N and halved the increment six times over. The
            // residual, f_ext - f_int, then comes out along +n, which is
            // the plane pushing the node back out, as it must.
            const cad::Vec3d push = normal * (penalty * gap);
            if (force != nullptr) {
                (*force)[at * 3 + 0] += push.x;
                (*force)[at * 3 + 1] += push.y;
                (*force)[at * 3 + 2] += push.z;
            }
            if (tangent == nullptr) continue;
            const double direction[3] = {normal.x, normal.y, normal.z};
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    const double value = penalty * direction[i] * direction[j];
                    if (value == 0.0) continue;
                    tangent->push_back(num::Triplet{node * 3 + i, node * 3 + j, value});
                }
            }
        }
    }
    return touching;
}

}  // namespace fem
