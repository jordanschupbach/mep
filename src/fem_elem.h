#ifndef MEP_FEM_ELEM_H
#define MEP_FEM_ELEM_H

#include "cad_math.h"
#include "fem_model.h"

#include <string>
#include <vector>

// The element library (plans/CAD_FEM_PLAN.md Part H.1).
//
// Shape functions, their derivatives, quadrature rules and element
// stiffness for every element type the mesher can produce and the shell
// and 2D work will want. Part 0.6's solver had Hex8's shape functions
// written into it directly, which was right for one element and is the
// wrong shape for eleven.
//
// WHAT AN ELEMENT HAS TO GET RIGHT, AND HOW IT IS CHECKED. Almost every
// mistake in an element -- a sign in a derivative, a node in the wrong
// place, a quadrature weight that does not sum to the reference volume --
// produces a solver that runs, converges, and is wrong by ten or twenty
// percent. None of it shows up as a crash. So each element here is held
// to properties that follow from what an element *is*, every one of
// which can be checked without knowing the right answer to any problem:
//
//   * The shape functions sum to one everywhere, so a constant is
//     represented exactly.
//   * Each is one at its own node and zero at the others, so the nodal
//     values are the nodal values.
//   * Their derivatives sum to zero, which is the same statement
//     differentiated, and is what makes rigid-body motion strain-free.
//   * The isoparametric map reproduces a linear field exactly, which is
//     what makes constant strain representable and is the whole content
//     of the patch test.
//   * The quadrature weights sum to the reference element's volume, and
//     the rule integrates polynomials to its stated degree exactly.
//   * The element stiffness matrix is symmetric and has exactly six zero
//     eigenvalues in three dimensions -- three translations and three
//     rotations -- and no more. One too many is a spurious mechanism,
//     which is what reduced integration buys and is worse than the
//     locking it cures.
//
// The last of those is the one worth dwelling on. A rank-deficient
// element does not fail loudly: it produces a solution with a
// zero-energy mode in it, which looks like a plausible deformation and is
// not one.
namespace fem {

enum class ElementShape {
    // Two-dimensional: plane stress, plane strain, and the shells of
    // Part I when they arrive.
    Tri3,
    Tri6,
    Quad4,
    Quad8,
    // Three-dimensional solids.
    Tet4,
    Tet10,
    Hex8,
    Hex20,
    Wedge6,
    Wedge15,
    Pyr5,
};

// Every shape, in order, for tests and for anything that wants to sweep
// the library rather than name its members.
const std::vector<ElementShape> &AllElementShapes();

const char *ElementShapeName(ElementShape shape);
int ElementNodeCount(ElementShape shape);
// Two or three. A two-dimensional element ignores the third reference
// coordinate and the third component of its node positions.
int ElementDimension(ElementShape shape);

// The nodes' own reference coordinates, three per node whatever the
// dimension, so that one signature serves both.
void ReferenceNodes(ElementShape shape, std::vector<cad::Vec3d> *out);

// The shape functions and their derivatives with respect to the
// reference coordinates, at one reference point.
//
// `out_n` gets ElementNodeCount values; `out_dn` gets
// ElementNodeCount * 3, row-major by node, with the unused third column
// zero for a two-dimensional element.
void ShapeFunctions(ElementShape shape, const cad::Vec3d &at, std::vector<double> *out_n,
                    std::vector<double> *out_dn);

struct QuadraturePoint {
    cad::Vec3d at;
    double weight = 0.0;
};

// A rule that integrates polynomials of total degree `degree` exactly
// over the reference element. Degree zero asks for the default for the
// shape, which is the lowest that integrates that element's own
// stiffness exactly on an undistorted geometry.
//
// Returns false, with a reason, for a degree no rule here reaches --
// rather than quietly returning a lower-order rule, which would turn a
// request for accuracy into a silent loss of it.
bool Quadrature(ElementShape shape, int degree, std::vector<QuadraturePoint> *out,
                std::string *error);

// The Jacobian of the map from reference to real coordinates at one
// point: fills `out_dn_xyz` (node-major, three per node) with the shape
// derivatives in real coordinates and returns the determinant. A
// non-positive determinant means the element is turned inside out or
// folded, and the caller is expected to treat that as an error rather
// than as a number.
double ElementJacobian(ElementShape shape, const std::vector<cad::Vec3d> &nodes,
                       const std::vector<double> &dn_ref, std::vector<double> *out_dn_xyz);

// How a two-dimensional element treats the third direction. There is no
// sensible default: plane stress is a thin plate free to thin, plane
// strain is a long body that cannot, and the two differ by far more than
// rounding.
enum class PlaneKind { Strain, Stress };

struct ElementOptions {
    // Zero means the shape's own default.
    int integration_degree = 0;
    // B-bar: integrate the volumetric part of the strain over the whole
    // element rather than at each point.
    //
    // WHAT IT CURES, AND WHAT IT DOES NOT. As Poisson's ratio approaches
    // a half the material stops being able to change volume, and each
    // quadrature point of each element then imposes its own
    // incompressibility constraint. Count them: a mesh of low-order
    // elements has more such constraints than it has degrees of freedom
    // to satisfy them with, so the only displacement field left is nearly
    // nothing. That is volumetric locking, and it is severe -- at
    // Poisson's ratio 0.4999 a fully integrated mesh can be several times
    // too stiff. B-bar imposes the constraint once per element instead of
    // once per point, which is enough constraints to be right and few
    // enough to be satisfiable.
    //
    // It does *not* cure shear locking, which is a different fault with a
    // similar name: a trilinear hexahedron in bending cannot represent
    // the bending mode without a spurious shear along with it, and
    // averaging the volumetric part does nothing about that. The cure
    // there is incompatible modes or an assumed strain field, and neither
    // is here yet. Reduced integration would cure either and buys a
    // zero-energy mode in exchange, which is worse than what it cures,
    // because a spurious mechanism is not conservative and does not
    // announce itself.
    bool b_bar = false;
    PlaneKind plane = PlaneKind::Strain;
    // Out-of-plane thickness for a two-dimensional element. Ignored in
    // three dimensions.
    double thickness = 1.0;
};

// The strain-displacement matrix at one point, from the real-coordinate
// shape derivatives: six rows in three dimensions (Voigt order xx, yy,
// zz, xy, yz, zx), three in two (xx, yy, xy). `out` is row-major,
// rows x (nodes * dimension).
void StrainDisplacement(ElementShape shape, const std::vector<double> &dn_xyz,
                        std::vector<double> *out);

// The constitutive matrix: 6x6 in three dimensions, 3x3 in two.
void ConstitutiveMatrix2D(const Material &material, PlaneKind plane, double out[3][3]);

// One element's stiffness matrix, row-major, (nodes * dimension) square.
bool ElementStiffness(ElementShape shape, const std::vector<cad::Vec3d> &nodes,
                      const Material &material, const ElementOptions &options,
                      std::vector<double> *out, std::string *error);

// The same, given the constitutive matrix directly. Three-dimensional
// only, and the form an orthotropic or temperature-dependent material
// reaches: those have no single modulus and Poisson's ratio to pass, and
// reducing them to one would be inventing a material that is not there.
bool ElementStiffness(ElementShape shape, const std::vector<cad::Vec3d> &nodes,
                      const double constitutive[6][6], const ElementOptions &options,
                      std::vector<double> *out, std::string *error);

// The volume (or area, in two dimensions) the quadrature rule sees. Not
// the same question as the geometric volume for a curved element, and
// that is the point: it is what the stiffness is actually integrated
// over, so it is what a test should compare against.
bool ElementVolume(ElementShape shape, const std::vector<cad::Vec3d> &nodes,
                   const ElementOptions &options, double *out, std::string *error);

}  // namespace fem

#endif
