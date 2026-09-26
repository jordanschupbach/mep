#ifndef MEP_FEM_RESULT_H
#define MEP_FEM_RESULT_H

#include "fem_solve.h"
#include "fem_study.h"

#include <string>
#include <vector>

// Result fields and stress recovery (plans/CAD_FEM_PLAN.md Part J.1).
//
// WHY RECOVERY IS A SUBJECT AND NOT A LOOKUP. A displacement-based finite
// element solves for displacement; stress is a derivative of it, so it is
// one polynomial order lower and it is *discontinuous across element
// boundaries* -- each element has its own idea of the stress at a shared
// node, and they do not agree. Everything here follows from that one
// fact:
//
//   * The disagreement is not noise to be averaged away quietly. It is
//     the discretisation error made visible, it is what Part J.5's
//     estimator is computed from, and a result viewer that only ever
//     shows the averaged field is hiding its own accuracy. So the
//     unaveraged values are kept alongside the averaged ones.
//   * Where the stress is *sampled* before it reaches the nodes decides
//     how accurate it is. The nodes are the worst place to ask: the
//     quadrature points are superconvergent for this -- Barlow's result,
//     that a displacement element's strain is one order more accurate
//     there than anywhere else in the element -- so the accurate answer
//     is to evaluate at the Gauss points and extrapolate outwards.
//     `Recovery::AtNodes` is kept to demonstrate the difference rather
//     than because anyone should choose it.
namespace fem {

enum class Recovery {
    // Evaluate the stress at each node's own reference coordinates. The
    // obvious thing and the least accurate one.
    AtNodes,
    // Evaluate at the quadrature points and fit a field through them,
    // then read that field at the nodes. Note that this *extrapolates* --
    // the Gauss points are strictly inside the element and the nodes are
    // on its boundary -- which is why it is not simply an averaging and
    // why it can overshoot on a coarse mesh. It is still the more
    // accurate of the two, by a margin the tests measure.
    //
    // AND ON AN AFFINE MESH IT IS *IDENTICAL* TO `AtNodes`, which is not
    // what the textbook description suggests and is worth knowing before
    // reaching for it. A trilinear hexahedron on a constant Jacobian has
    // a stress field that is exactly trilinear, so fitting eight shape
    // functions through eight Gauss points is exact interpolation rather
    // than smoothing, and reading it at the nodes returns precisely what
    // evaluating there directly would. The same holds for a Tet4, whose
    // stress is constant. The two differ only where the fit cannot be
    // exact: a distorted element, whose map is rational rather than
    // polynomial, or an over-determined rule. Superconvergence is a
    // property of *where the element is sampled*, and re-reading the same
    // polynomial elsewhere does not create it.
    Extrapolated,
    // Superconvergent patch recovery: fit one polynomial per node through
    // the quadrature points of every element touching it, and read it at
    // the node. This is the one that is actually better, and the reason
    // is that the fit is over-determined by construction -- a patch holds
    // far more samples than the polynomial has terms -- so it is a
    // genuine least-squares smoothing of superconvergent data rather than
    // a re-reading of one element's own field. The recovered field is
    // also *continuous*, needing no averaging afterwards, which is what
    // makes the gap between it and the element's own stress an error
    // estimate rather than a measure of how the mesh is arranged.
    //
    // At a boundary node the node's own patch is often too small to
    // support the polynomial -- a corner of a hex mesh touches one
    // element. Zienkiewicz and Zhu's answer, and the one here, is to
    // borrow an interior neighbour's patch and evaluate *its* polynomial
    // at the boundary node, rather than to drop the order where the
    // stress concentrations are.
    Patch,
};

// Which polynomial the recovery fitted through the quadrature points.
// Reported rather than assumed, because an element whose rule has fewer
// points than the fit has terms cannot support the fit -- a Tet4 has one
// Gauss point and four nodes -- and silently returning a constant where
// a linear field was expected is the kind of thing that shows up as a
// mysteriously flat contour plot.
enum class RecoveryBasis {
    Constant,
    Linear,
    // The element's own shape functions: the fit then reproduces
    // anything the element itself can represent.
    Shape,
};

const char *RecoveryBasisName(RecoveryBasis basis);

struct StressField {
    // The unaveraged view: `element_nodal[element_offset[e] + a]` is what
    // element `e` thinks the stress is at its own node `a`. Two elements
    // meeting at a node appear twice, with different values, and that is
    // the information this exists to carry.
    std::vector<int> element_offset;  // ElementCount() + 1 entries
    std::vector<StressTensor> element_nodal;
    // The stress at each element's centroid, unaveraged -- one value per
    // element, which is what a piecewise-constant plot draws.
    std::vector<StressTensor> element_centre;
    // Averaged over every element touching the node.
    std::vector<StressTensor> nodal;
    std::vector<int> contributions;
    // The worst disagreement at each node, as the Frobenius norm of the
    // difference between a contributing element's tensor and the average,
    // in stress units. Zero at a node inside a region of constant stress,
    // large at a stress concentration a mesh is too coarse to resolve.
    std::vector<double> discontinuity;
    // What each element's fit came out as, and how many fell short of
    // `Shape`. Empty for `Recovery::Patch`, which fits per node.
    std::vector<RecoveryBasis> element_basis;
    int shortfalls = 0;
    // Patch recovery only: how many nodes were fitted on their own patch,
    // how many borrowed a neighbour's, and how many fell back to plain
    // averaging because neither was possible.
    int own_patch = 0;
    int borrowed_patch = 0;
    // Fitted, but below the element's own order, because there was
    // neither a patch nor a neighbour that could support it.
    int reduced_order = 0;
    int averaged = 0;
};

// `displacement` is the full nodal vector, three per node, in node order.
bool RecoverStress(const AnalysisModel &model, const std::vector<double> &displacement,
                   Recovery how, StressField *out, std::string *error);

struct StressInvariants {
    // Descending, so `principal[0]` is the largest tensile value and
    // `principal[2]` the largest compressive one.
    double principal[3] = {0.0, 0.0, 0.0};
    double von_mises = 0.0;
    // The difference between the largest and smallest principal stress,
    // which is what Tresca's criterion compares against the yield
    // strength. Also called the stress intensity.
    double tresca = 0.0;
    double max_shear = 0.0;  // half of Tresca
    double hydrostatic = 0.0;
    // Hydrostatic over von Mises. Not a decoration: ductile materials
    // tear rather than yield where it is high, which is exactly the
    // information a von Mises plot throws away, and it is why a notch
    // root is more dangerous than its von Mises value suggests. Zero
    // where the von Mises stress is, since the ratio has no meaning there.
    double triaxiality = 0.0;
};

StressInvariants Invariants(const StressTensor &stress);

enum class FailureCriterion {
    // Ductile metals. Shear-driven, so it is blind to hydrostatic stress
    // altogether -- a material under uniform pressure has a von Mises
    // stress of zero however large the pressure.
    VonMises,
    // Ductile metals, more conservative than von Mises by up to 15.5%
    // (the two agree in uniaxial tension and differ most in pure shear).
    Tresca,
    // Brittle materials, which part rather than yield: what matters is
    // the largest tensile stress, and a compressive state is harmless.
    MaxPrincipal,
    // Brittle materials that are much stronger in compression than in
    // tension -- concrete, cast iron, rock. The one criterion here that
    // needs two strengths, because it is the one that knows they differ.
    MohrCoulomb,
};

const char *FailureCriterionName(FailureCriterion criterion);

struct Strength {
    double tensile = 0.0;
    // A positive magnitude. Zero means "the same as tensile", which is
    // right for a metal and wrong for everything Mohr-Coulomb is for.
    double compressive = 0.0;
};

// How many times over the load could be applied before the criterion says
// the material has failed -- so 1.0 is exactly at the limit and below 1.0
// has already passed it.
//
// Returns infinity where the effective stress is zero, which is the true
// answer and is better than a large finite number: a viewer that cannot
// display infinity will show something obviously special, where 1e30
// would quietly plot as a colour.
double SafetyFactor(const StressTensor &stress, const Strength &strength,
                    FailureCriterion criterion);

// One scalar per node, for contouring (J.2), probing (J.4) and export.
// Kept here rather than in the renderer because a probe and a contour
// plot must agree about what "von Mises" means, and the only way to be
// sure of that is for there to be one function.
enum class ScalarField {
    DisplacementMagnitude,
    DisplacementX,
    DisplacementY,
    DisplacementZ,
    VonMises,
    Tresca,
    MaxPrincipal,
    MinPrincipal,
    MaxShear,
    Hydrostatic,
    Triaxiality,
    StressXX,
    StressYY,
    StressZZ,
    StressXY,
    StressYZ,
    StressZX,
    SafetyFactor,
    // The recovery's own disagreement at the node: an error map, in
    // stress units.
    Discontinuity,
};

const std::vector<ScalarField> &AllScalarFields();
const char *ScalarFieldName(ScalarField field);
// Whether larger values are worse, which is what a colour legend and a
// "find the worst node" query both need and which is not the same for
// every field -- a safety factor is worst where it is smallest.
bool LargerIsWorse(ScalarField field);

bool SampleField(const AnalysisModel &model, const std::vector<cad::Vec3d> &displacement,
                 const StressField &stress, ScalarField field, const Strength &strength,
                 std::vector<double> *out, std::string *error);

}  // namespace fem

#endif
