#ifndef MEP_FEM_MAP_H
#define MEP_FEM_MAP_H

#include "fem_study.h"

#include <string>
#include <vector>

// Mapping a field from one mesh onto another (plans/CAD_FEM_PLAN.md I.3).
//
// WHY THIS IS THE POINT OF THE SEQUENTIAL COUPLING. Solving a thermal
// problem and then a structural one on the *same* mesh needs no mapping at
// all -- the temperatures are already indexed by the nodes the stiffness
// wants. That is also the case that teaches nothing and hides everything:
// every later multiphysics step has the two fields on different meshes,
// because the mesh a thermal problem wants and the mesh a stress problem
// wants are not the same. A thermal gradient needs elements through the
// thickness of a wall; a stress concentration needs them around a fillet
// and does not care about the wall. So the mapping is built and tested
// first, on meshes that deliberately differ.
//
// WHAT IT HAS TO GET RIGHT. A mapped field must reproduce a linear field
// exactly, for the same reason an element must: a rigid translation of the
// temperature -- adding ten degrees everywhere -- has to arrive as ten
// degrees everywhere, and a linear gradient has to arrive as the same
// gradient. An interpolation that loses either turns a thermal-stress
// answer into a plausible one that is wrong by the amount it lost, and
// nothing downstream can tell.
namespace fem {

struct MapReport {
    bool ok = false;
    std::string error;
    // Target nodes found inside a source element.
    int inside = 0;
    // Target nodes outside every source element, which is not an error:
    // two meshes of the same body disagree about its boundary by the
    // faceting of each, so a node of one lands just outside the other
    // routinely. Those take the value of the nearest source element,
    // evaluated at the closest point of it -- which is the right answer
    // for a node a hair outside and the wrong one for a node genuinely
    // elsewhere, so the distance is reported.
    int extrapolated = 0;
    // METRES, from the point to the closest point of the element it took
    // its value from. Not the inverse map's residual, which is what this
    // used to hold: an isoparametric map extrapolates happily, so a point
    // far outside converges with a residual of zero and this read zero for
    // exactly the case it exists to report.
    double worst_distance = 0.0;
};

// Interpolates `values`, one per node of the source mesh, onto the
// positions in `to`.
bool MapField(const std::vector<cad::Vec3d> &from_nodes,
              const std::vector<BoundElement> &from_elements,
              const std::vector<double> &values, const std::vector<cad::Vec3d> &to,
              std::vector<double> *out, MapReport *report);

// The reference coordinates of a physical point within one element, by
// Newton on the isoparametric map. Returns false if it does not converge,
// which for a badly distorted element is possible and is worth knowing
// about rather than accepting a wrong answer from.
//
// Exposed because it is the piece worth testing on its own: everything
// else in this file is bookkeeping around it.
bool InverseMap(ElementShape shape, const std::vector<cad::Vec3d> &nodes, const cad::Vec3d &point,
                cad::Vec3d *out_reference, double *out_distance);

// Whether a reference coordinate is inside the reference element, with a
// tolerance in reference units -- which are the same size whatever the
// element's physical size, so the tolerance means the same thing on a
// millimetre part and a metre one.
bool InsideReference(ElementShape shape, const cad::Vec3d &reference, double tolerance);

}  // namespace fem

#endif
