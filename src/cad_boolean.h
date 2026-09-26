#ifndef MEP_CAD_BOOLEAN_H
#define MEP_CAD_BOOLEAN_H

#include "cad_classify.h"
#include "cad_intersect.h"
#include "cad_topology.h"

#include <string>
#include <vector>

// Boolean operations on solids (plans/CAD_FEM_PLAN.md Part C.4).
//
// The shape of the algorithm is imprint, classify, stitch:
//
//   IMPRINT   Intersect every pair of faces, trim the resulting curves to
//             the parts that actually lie on both faces, and cut each
//             face along them. A face is cut in its own parameter space:
//             its trimming loops and the new intersection curves are
//             assembled into a planar arrangement, and the regions of
//             that arrangement are the pieces the face falls into.
//   CLASSIFY  Decide, for each piece, whether it lies inside or outside
//             the other solid, by taking a point in its interior and
//             asking cad_classify.h.
//   STITCH    Keep the pieces the operation calls for, reversing the
//             orientation of those that end up bounding the other way,
//             and sew them back into a shell by matching edges that
//             describe the same curve.
//
// The planar arrangement is where the real work is, and it is worth
// saying why it cannot be avoided. A face cut by a curve does not simply
// become two faces: the curve may enter and leave several times, may form
// a closed loop entirely inside the face (a hole), may run along an
// existing edge, or may touch the boundary without crossing it. Only
// building the arrangement and reading its regions off gets all of those
// right, and every shortcut that handles the common case leaves the rest
// silently wrong.
//
// When no face of one solid cuts a face of the other there is nothing to
// imprint, and the answer is one of the inputs: the two solids are
// disjoint, or one lies wholly inside the other. Those cases are handled
// separately and exactly -- A - B with B inside A is A carrying B as an
// inner shell bounding a cavity -- except where the result would be
// empty or two unconnected lumps, which are not a body.
//
// KNOWN LIMITATIONS, stated rather than discovered:
//   * A face whose parameterization has a pole -- a sphere's poles, a
//     cone's apex, any surface of revolution meeting its axis -- cannot
//     be cut. The arrangement works in parameter space, and at a pole the
//     face's boundary there is a degenerate line carrying no p-curve, so
//     the parameter rectangle never closes and the cycles bounding the
//     pieces do not exist; an intersection curve crossing the seam also
//     arrives unwrapped, running outside the face's own domain. Such a
//     boolean is refused up front rather than returning an invalid solid.
//     A seam without a pole (a cylinder, a torus) is handled.
//   * Coincident faces -- two solids sharing a planar face exactly -- are
//     not merged. The intersection of two such faces is a region, not a
//     curve, and handling it needs a separate code path that this does
//     not have. Such a boolean is refused with an explanation.
//   * A tangential contact between faces yields no curve to cut along, so
//     the operation is refused rather than producing a zero-thickness
//     sliver.
//   * The arrangement works on the p-curves' sampled geometry to decide
//     topology, while the edges it produces carry the exact curves. That
//     is the same trade Part B.5 makes, and it means two curves that
//     approach within the sampling resolution without meeting can be
//     merged into one vertex.
namespace cad {

enum class BooleanOp {
    Union,
    Intersection,
    // A minus B.
    Difference,
};

struct BooleanOptions {
    IntersectOptions intersect;
    ClassifyOptions classify;
    Tolerance tolerance;
    // Samples per curve when building the arrangement and when trimming
    // intersection branches to the faces they lie on.
    int curve_samples = 64;
    // Validate the result before returning it. On by default: an invalid
    // body produced here surfaces as an inexplicable failure several
    // parts of the plan later, which is exactly what Part B.3 exists to
    // prevent.
    bool validate_result = true;
};

struct BooleanReport {
    bool ok = false;
    std::string error;
    EntityId body = kNoEntity;
    // Diagnostics worth surfacing even on success.
    int face_pairs_intersected = 0;
    int intersection_curves = 0;
    int pieces_from_a = 0;
    int pieces_from_b = 0;
    int pieces_kept = 0;
};

// A deep copy of one body into another model, optionally turned inside
// out and marked as bounding a void rather than material.
//
// Exposed because it is what "add this body to the part" means in Part
// E's feature tree, and what the containment shortcuts here already
// needed: when neither solid cuts the other, the answer is one of the
// inputs copied out.
bool CopyBodyInto(const Model &source, EntityId body, Model *out, EntityId *out_body, bool reverse,
                  bool as_void, const std::string &name, std::string *error);

// Performs `op` on two bodies, writing the result into `out` (which may
// be empty or may already hold other bodies). Both inputs must have
// p-curves built and should be valid; an invalid input produces an
// invalid output, which is why the result is checked rather than
// assumed.
bool BooleanOperation(const Model &a_model, EntityId a_body, const Model &b_model, EntityId b_body,
                      BooleanOp op, Model *out, BooleanReport *report, const BooleanOptions &options = {});

}  // namespace cad

#endif
