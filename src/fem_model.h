#ifndef MEP_FEM_MODEL_H
#define MEP_FEM_MODEL_H

#include "cad_math.h"

#include <array>
#include <string>
#include <vector>

// The finite-element analysis model (plans/CAD_FEM_PLAN.md Parts 0.6 and
// H.2), and the JSON encoding the mep-fem solver process reads it through.
//
// This is deliberately the *narrow* version of what Part H.2 eventually
// describes. The full model binds every boundary condition to a CAD face,
// edge or vertex id resolved through persistent naming, so that editing a
// dimension and remeshing re-applies the constraints automatically -- that
// is the whole argument for coupling the two halves. None of that exists
// yet: there is no B-rep to name and no mesher to remesh with. So the
// conditions here attach to node indices, which is what a mesh alone can
// offer, and the topology binding is layered on top later rather than
// retrofitted into the solver. The solver never needs to know the
// difference: by the time a model reaches it, every condition has been
// resolved to nodes either way.
namespace fem {

// Only one element type so far. Hex8 rather than Tet4 for the first pass
// because a structured box mesh is trivially hexahedral, and because Tet4
// is the worst-behaved element in the library for bending -- starting
// with it would have made the cantilever check below look like a solver
// bug rather than the known stiffness of a linear tetrahedron.
enum class ElementType { Hex8 };

// Node ordering for Hex8 follows the standard isoparametric convention:
// nodes 0-3 are the -zeta face counter-clockwise seen from +zeta, nodes
// 4-7 the +zeta face in the same rotational order. Every shape function
// in fem_solve.cpp is written against this, and a mesh that violates it
// produces inverted elements rather than wrong-but-plausible answers --
// which is why Model::Validate checks element Jacobians.
struct Element {
    ElementType type = ElementType::Hex8;
    std::array<int, 8> nodes{};
};

// Isotropic linear elastic material. Young's modulus and Poisson's ratio
// are the inputs a person has; Lame parameters are derived where needed.
// `density` is unused by a static solve and carried anyway, because modal
// analysis (Part I.4) needs it and a model file that loses it on a
// round-trip would be a nuisance to re-specify.
struct Material {
    std::string name = "default";
    double youngs_modulus = 210e9;  // Pa (structural steel)
    double poissons_ratio = 0.3;
    double density = 7850.0;  // kg/m^3

    // Poisson's ratio must be in (-1, 0.5): at exactly 0.5 the material is
    // incompressible and the isotropic constitutive matrix is singular,
    // which is a real modelling situation (rubber) needing a mixed
    // formulation rather than a value this solver can accept.
    bool IsValid(std::string *error) const;
};

// A prescribed displacement on one node. Each axis is independently free
// or fixed; a fixed axis holds `value` (usually zero, but a prescribed
// non-zero displacement is how a "move this face by 1 mm" load case is
// expressed, so it is supported from the start).
struct Constraint {
    int node = 0;
    bool fixed[3] = {false, false, false};
    double value[3] = {0.0, 0.0, 0.0};
};

struct NodalLoad {
    int node = 0;
    cad::Vec3d force;  // N
};

struct Model {
    std::vector<cad::Vec3d> nodes;
    std::vector<Element> elements;
    Material material;
    std::vector<Constraint> constraints;
    std::vector<NodalLoad> loads;

    int NodeCount() const { return static_cast<int>(nodes.size()); }
    int ElementCount() const { return static_cast<int>(elements.size()); }
    // Three translational degrees of freedom per node; DOF index for node
    // n's axis a is 3n+a throughout the solver.
    int DofCount() const { return NodeCount() * 3; }

    // Structural checks that are cheap and catch the mistakes that would
    // otherwise surface as a singular matrix or a nonsensical answer:
    // out-of-range node references, degenerate or inverted elements
    // (negative Jacobian anywhere in the element), an invalid material,
    // and the absence of any constraint at all. Returns false with a
    // description; never modifies the model.
    bool Validate(std::string *error) const;

    cad::Box3d BoundingBox() const;
};

// Generates a structured hexahedral mesh of an axis-aligned box: the
// "hardcode a box" half of Part 0.6's vertical slice, and genuinely
// useful afterwards as the reference mesh every solver check in Part H is
// written against (a structured mesh has no mesher bugs in it, so a
// failure is unambiguously the solver's).
//
// `divisions` is the element count along each axis, each at least 1.
// Nodes are generated in x-fastest order, so node (i,j,k) is at index
// i + (nx+1)*(j + (ny+1)*k) -- callers pick boundary node sets by walking
// that indexing rather than by searching coordinates.
Model MakeBoxMesh(const cad::Vec3d &min_corner, const cad::Vec3d &size, const std::array<int, 3> &divisions,
                  const Material &material);

// Node indices on one face of a box mesh built by MakeBoxMesh, selected
// by axis (0/1/2) and side (false = minimum, true = maximum). The
// stand-in for real topology-based selection (Part H.2) that keeps the
// vertical slice honest: a caller says "the x = 0 face", not "nodes 0
// through 24".
std::vector<int> BoxMeshFaceNodes(const std::array<int, 3> &divisions, int axis, bool max_side);

// JSON encoding, the wire format between the editor and the mep-fem
// process. Text rather than a packed binary format: a solve request for a
// mesh of any size this pass produces is well under a megabyte, and being
// able to read a failing request by eye is worth more than the bytes.
std::string ModelToJson(const Model &model);
bool ModelFromJson(const std::string &text, Model *out, std::string *error);

}  // namespace fem

#endif
