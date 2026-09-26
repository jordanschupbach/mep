#include "fem_result.h"

#include "fem_elem.h"
#include "fem_static.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fem {
namespace {

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

int BasisTerms(RecoveryBasis basis, ElementShape shape) {
    switch (basis) {
        case RecoveryBasis::Constant: return 1;
        // A two-dimensional element has zeta zero at every one of its
        // points, so a {1, xi, eta, zeta} basis has an identically zero
        // column and a singular normal matrix. Three terms, not four.
        case RecoveryBasis::Linear: return ElementDimension(shape) == 2 ? 3 : 4;
        case RecoveryBasis::Shape: return ElementNodeCount(shape);
    }
    return 1;
}

void EvaluateBasis(RecoveryBasis basis, ElementShape shape, const cad::Vec3d &at,
                   std::vector<double> *out) {
    out->clear();
    switch (basis) {
        case RecoveryBasis::Constant:
            out->push_back(1.0);
            return;
        case RecoveryBasis::Linear:
            out->push_back(1.0);
            out->push_back(at.x);
            out->push_back(at.y);
            if (ElementDimension(shape) != 2) out->push_back(at.z);
            return;
        case RecoveryBasis::Shape: {
            std::vector<double> dn;
            ShapeFunctions(shape, at, out, &dn);
            return;
        }
    }
}

// Cholesky of a small dense symmetric matrix, in place, lower triangle.
// Returns false the moment a pivot is not positive, which is the signal
// that the quadrature points cannot support this many basis terms --
// they are too few, or they lie on a plane the basis needs them off.
bool Cholesky(std::vector<double> *matrix, int n) {
    std::vector<double> &a = *matrix;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            double sum = a[Idx(i * n + j)];
            for (int k = 0; k < j; ++k) sum -= a[Idx(i * n + k)] * a[Idx(j * n + k)];
            if (i == j) {
                // The scale here is set by the basis values themselves,
                // which are order one on a reference element, so an
                // absolute floor is meaningful and a relative one would
                // need a scale there is nothing to measure.
                if (!(sum > 1e-12)) return false;
                a[Idx(i * n + j)] = std::sqrt(sum);
            } else {
                a[Idx(i * n + j)] = sum / a[Idx(j * n + j)];
            }
        }
    }
    return true;
}

void CholeskySolve(const std::vector<double> &factor, int n, const double *rhs, double *out) {
    std::vector<double> y(Idx(n), 0.0);
    for (int i = 0; i < n; ++i) {
        double sum = rhs[i];
        for (int k = 0; k < i; ++k) sum -= factor[Idx(i * n + k)] * y[Idx(k)];
        y[Idx(i)] = sum / factor[Idx(i * n + i)];
    }
    for (int i = n - 1; i >= 0; --i) {
        double sum = y[Idx(i)];
        for (int k = i + 1; k < n; ++k) sum -= factor[Idx(k * n + i)] * out[k];
        out[i] = sum / factor[Idx(i * n + i)];
    }
}


int ElementOrder(ElementShape shape) {
    switch (shape) {
        case ElementShape::Tri6:
        case ElementShape::Quad8:
        case ElementShape::Tet10:
        case ElementShape::Hex20:
        case ElementShape::Wedge15: return 2;
        default: return 1;
    }
}

// WHERE A PATCH TAKES ITS SAMPLES, WHICH IS NOT WHERE THE ELEMENT IS
// INTEGRATED. This distinction cost me a rewrite and is the one thing in
// this file most worth reading. A Hex8 is integrated at its 2x2x2 Gauss
// points because one point would leave it rank deficient -- but the
// point where a trilinear element's strain is superconvergent is its
// *centre*, and the 2x2x2 points are a distance h/(2 sqrt 3) away from
// it, where the strain error is O(h) rather than O(h^2). Sampling at the
// integration points because they are already there is the obvious
// shortcut, and it throws away the entire order the recovery exists to
// gain: it measured a convergence rate of 1.19 where the correct points
// give 2.
//
// The rule is the *reduced* one -- a rule of the element's own order,
// which is p points per direction for a tensor-product shape and the
// classical low rule for a simplex. One point for a Hex8 or a Tet4,
// 2x2x2 for a Hex20, four for a Tet10.
int BarlowDegree(ElementShape shape) { return ElementOrder(shape); }

int PolynomialTerms(int degree) { return degree == 2 ? 10 : (degree == 1 ? 4 : 1); }

// The polynomial the patch is fitted in, in coordinates centred on the
// node and scaled by the patch's own size. CENTRING AND SCALING ARE NOT
// TIDINESS. A patch a tenth of a millimetre across on a part positioned
// a metre from the origin gives a normal matrix whose entries span
// sixteen orders of magnitude, and Cholesky on that reports a singular
// patch for a perfectly good one -- so a model would recover stress
// correctly at the origin and fall back to averaging everywhere else.
void PolynomialAt(int degree, const cad::Vec3d &offset, std::vector<double> *out) {
    out->clear();
    out->push_back(1.0);
    if (degree < 1) return;
    out->push_back(offset.x);
    out->push_back(offset.y);
    out->push_back(offset.z);
    if (degree < 2) return;
    out->push_back(offset.x * offset.x);
    out->push_back(offset.y * offset.y);
    out->push_back(offset.z * offset.z);
    out->push_back(offset.x * offset.y);
    out->push_back(offset.y * offset.z);
    out->push_back(offset.z * offset.x);
}

struct Sample {
    cad::Vec3d at;
    StressTensor stress;
};

}  // namespace

const char *RecoveryBasisName(RecoveryBasis basis) {
    switch (basis) {
        case RecoveryBasis::Constant: return "constant";
        case RecoveryBasis::Linear: return "linear";
        case RecoveryBasis::Shape: return "shape functions";
    }
    return "?";
}

namespace {

// One node's fitted patch polynomial, kept so that a boundary node whose
// own patch is too small can borrow it.
struct Patch {
    bool fitted = false;
    int degree = 0;
    int wanted = 0;
    cad::Vec3d centre;
    double scale = 1.0;
    // terms x 6, component-major.
    std::vector<double> coefficients;
};

StressTensor EvaluatePatch(const Patch &patch, const cad::Vec3d &at) {
    std::vector<double> basis;
    PolynomialAt(patch.degree, (at - patch.centre) * (1.0 / patch.scale), &basis);
    const int terms = static_cast<int>(basis.size());
    StressTensor out;
    for (int component = 0; component < 6; ++component) {
        double sum = 0.0;
        for (int i = 0; i < terms; ++i) {
            sum += patch.coefficients[Idx(component * terms + i)] * basis[Idx(i)];
        }
        out.s[component] = sum;
    }
    return out;
}

bool RecoverByPatch(const AnalysisModel &model, const std::vector<double> &displacement,
                    StressField *out, std::string *error) {
    // Every element's quadrature points, in real coordinates, with the
    // stress there. These are the superconvergent samples and they are
    // the only input the fits get.
    std::vector<cad::Vec3d> sample_at;
    std::vector<StressTensor> sample_stress;
    std::vector<int> element_sample_offset(model.elements.size() + 1, 0);
    std::vector<std::vector<int>> elements_of_node(model.nodes.size());
    std::vector<QuadraturePoint> rule;
    std::vector<double> n;
    std::vector<double> dn;
    for (int e = 0; e < model.ElementCount(); ++e) {
        const BoundElement &element = model.elements[Idx(e)];
        element_sample_offset[Idx(e)] = static_cast<int>(sample_at.size());
        if (!Quadrature(element.shape, BarlowDegree(element.shape), &rule, error)) return false;
        for (const QuadraturePoint &point : rule) {
            ShapeFunctions(element.shape, point.at, &n, &dn);
            cad::Vec3d where{};
            for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                where = where + model.nodes[Idx(element.nodes[a])] * n[a];
            }
            StressTensor stress;
            if (!ElementStressAt(model, e, point.at, displacement, &stress, error)) return false;
            sample_at.push_back(where);
            sample_stress.push_back(stress);
        }
        for (const int node : element.nodes) elements_of_node[Idx(node)].push_back(e);
    }
    element_sample_offset[model.elements.size()] = static_cast<int>(sample_at.size());

    std::vector<Patch> patches(model.nodes.size());
    std::vector<double> normal;
    std::vector<double> rhs;
    std::vector<double> basis;
    auto fit = [&](int node, int degree) {
        const std::vector<int> &around = elements_of_node[Idx(node)];
        Patch &patch = patches[Idx(node)];
        const int terms = PolynomialTerms(degree);
        int samples = 0;
        for (const int e : around) {
            samples += element_sample_offset[Idx(e) + 1] - element_sample_offset[Idx(e)];
        }
        if (terms > samples) return false;
        normal.assign(Idx(terms * terms), 0.0);
        for (const int e : around) {
            for (int g = element_sample_offset[Idx(e)]; g < element_sample_offset[Idx(e) + 1];
                 ++g) {
                PolynomialAt(degree, (sample_at[Idx(g)] - patch.centre) * (1.0 / patch.scale),
                             &basis);
                for (int i = 0; i < terms; ++i) {
                    for (int j = 0; j <= i; ++j) {
                        normal[Idx(i * terms + j)] += basis[Idx(i)] * basis[Idx(j)];
                    }
                }
            }
        }
        for (int i = 0; i < terms; ++i) {
            for (int j = i + 1; j < terms; ++j) {
                normal[Idx(i * terms + j)] = normal[Idx(j * terms + i)];
            }
        }
        std::vector<double> factor = normal;
        // A PATCH CAN HAVE ENOUGH SAMPLES AND STILL NOT DETERMINE THE
        // FIT. The four elements meeting at a node on a flat face of a
        // hex mesh have coplanar centres, so a linear polynomial's
        // coefficient off that plane is unconstrained however many
        // samples there are. Counting points would call that patch fine;
        // only the factorisation knows.
        if (!Cholesky(&factor, terms)) return false;
        patch.coefficients.assign(Idx(terms * 6), 0.0);
        for (int component = 0; component < 6; ++component) {
            rhs.assign(Idx(terms), 0.0);
            for (const int e : around) {
                for (int g = element_sample_offset[Idx(e)]; g < element_sample_offset[Idx(e) + 1];
                     ++g) {
                    PolynomialAt(degree, (sample_at[Idx(g)] - patch.centre) * (1.0 / patch.scale),
                                 &basis);
                    for (int i = 0; i < terms; ++i) {
                        rhs[Idx(i)] += basis[Idx(i)] * sample_stress[Idx(g)].s[component];
                    }
                }
            }
            CholeskySolve(factor, terms, rhs.data(), &patch.coefficients[Idx(component * terms)]);
        }
        patch.degree = degree;
        patch.fitted = true;
        return true;
    };

    // Pass one: every node at its elements' own order. ONE ATTEMPT, AND
    // NO STEPPING DOWN HERE -- falling back to a lower degree on this
    // node's own patch looks like the graceful thing and is the wrong
    // order of preference. A Hex20 corner touches one element and so has
    // eight Barlow samples against a quadratic's ten terms, so a
    // step-down succeeds, reports itself as a perfectly good fit, and
    // quietly makes the recovery linear all over the surface -- which is
    // exactly where stresses are read. It cost the whole order the
    // recovery exists to gain. Lower degrees get their turn below, after
    // borrowing.
    for (int node = 0; node < model.NodeCount(); ++node) {
        const std::vector<int> &around = elements_of_node[Idx(node)];
        if (around.empty()) continue;
        int degree = 1;
        double radius = 0.0;
        for (const int e : around) {
            degree = std::max(degree, ElementOrder(model.elements[Idx(e)].shape));
            for (int g = element_sample_offset[Idx(e)]; g < element_sample_offset[Idx(e) + 1];
                 ++g) {
                radius = std::max(radius, (sample_at[Idx(g)] - model.nodes[Idx(node)]).Length());
            }
        }
        if (!(radius > 0.0)) continue;
        patches[Idx(node)].centre = model.nodes[Idx(node)];
        patches[Idx(node)].scale = radius;
        patches[Idx(node)].wanted = degree;
        fit(node, degree);
    }

    // `out->nodal` arrives holding the plain average, which is what a
    // node with no usable patch anywhere near it keeps.
    for (int node = 0; node < model.NodeCount(); ++node) {
        if (patches[Idx(node)].fitted) {
            out->nodal[Idx(node)] = EvaluatePatch(patches[Idx(node)], model.nodes[Idx(node)]);
            ++out->own_patch;
            continue;
        }
        if (patches[Idx(node)].scale <= 0.0) {
            ++out->averaged;
            continue;
        }
        // BORROW A NEIGHBOUR'S PATCH RATHER THAN DROP THE ORDER. A corner
        // node touches one element, which is where a stress concentration
        // most often is, and a lower-order fit exactly there would make
        // the recovery worst where it matters most. Extending a
        // neighbour's polynomial out to this node keeps the order; the
        // cost is that the fit is being used slightly outside the data it
        // was made from, which is a far smaller error than the
        // alternative.
        const Patch *best = nullptr;
        double nearest = 0.0;
        for (const int e : elements_of_node[Idx(node)]) {
            for (const int other : model.elements[Idx(e)].nodes) {
                if (other == node || !patches[Idx(other)].fitted) continue;
                const double distance =
                    (model.nodes[Idx(other)] - model.nodes[Idx(node)]).Length();
                if (best != nullptr && distance >= nearest) continue;
                best = &patches[Idx(other)];
                nearest = distance;
            }
        }
        if (best != nullptr) {
            out->nodal[Idx(node)] = EvaluatePatch(*best, model.nodes[Idx(node)]);
            ++out->borrowed_patch;
            continue;
        }
        // Only now, with no neighbour to borrow from -- a mesh of one
        // element, or a node whose whole neighbourhood is degenerate --
        // is a lower-degree fit better than nothing. It is counted
        // separately so that a result which quietly lost an order
        // somewhere says so.
        bool recovered = false;
        for (int degree = patches[Idx(node)].wanted - 1; degree >= 0 && !recovered; --degree) {
            recovered = fit(node, degree);
        }
        if (recovered) {
            out->nodal[Idx(node)] = EvaluatePatch(patches[Idx(node)], model.nodes[Idx(node)]);
            ++out->reduced_order;
            continue;
        }
        ++out->averaged;
    }
    return true;
}

}  // namespace

bool RecoverStress(const AnalysisModel &model, const std::vector<double> &displacement,
                   Recovery how, StressField *out, std::string *error) {
    *out = StressField{};
    if (displacement.size() != Idx(model.NodeCount() * 3)) {
        *error = "the displacement vector is not three per node";
        return false;
    }
    out->nodal.assign(model.nodes.size(), StressTensor{});
    out->contributions.assign(model.nodes.size(), 0);
    out->discontinuity.assign(model.nodes.size(), 0.0);
    out->element_centre.assign(model.elements.size(), StressTensor{});
    out->element_offset.assign(model.elements.size() + 1, 0);
    out->element_basis.assign(model.elements.size(), RecoveryBasis::Shape);

    std::vector<cad::Vec3d> reference_nodes;
    std::vector<QuadraturePoint> rule;
    std::vector<double> values;
    std::vector<double> normal;
    std::vector<double> rhs;
    std::vector<double> coefficients;

    for (int e = 0; e < model.ElementCount(); ++e) {
        const BoundElement &element = model.elements[Idx(e)];
        const int count = static_cast<int>(element.nodes.size());
        out->element_offset[Idx(e)] = static_cast<int>(out->element_nodal.size());
        ReferenceNodes(element.shape, &reference_nodes);

        cad::Vec3d centre{};
        for (const cad::Vec3d &p : reference_nodes) centre = centre + p;
        centre = centre * (1.0 / static_cast<double>(reference_nodes.size()));
        if (!ElementStressAt(model, e, centre, displacement, &out->element_centre[Idx(e)], error)) {
            return false;
        }

        std::vector<StressTensor> at_node(Idx(count));
        if (how != Recovery::Extrapolated) {
            for (int a = 0; a < count; ++a) {
                if (!ElementStressAt(model, e, reference_nodes[Idx(a)], displacement,
                                     &at_node[Idx(a)], error)) {
                    return false;
                }
            }
        } else {
            if (!Quadrature(element.shape, 0, &rule, error)) return false;
            const int points = static_cast<int>(rule.size());
            std::vector<StressTensor> sampled(Idx(points));
            for (int g = 0; g < points; ++g) {
                if (!ElementStressAt(model, e, rule[Idx(g)].at, displacement, &sampled[Idx(g)],
                                     error)) {
                    return false;
                }
            }
            // THE FIT CANNOT CARRY MORE INFORMATION THAN THE POINTS HOLD,
            // and the honest thing is to step down rather than to
            // regularise. A Tet4 is integrated at one point, so the only
            // field its samples determine is a constant -- which is also
            // the exact answer, since a Tet4's stress *is* constant. A
            // Tet10 has four points and ten nodes, and four points
            // determine a linear field exactly, which is again what that
            // element's stress is. The step-down is not a degradation in
            // either case; it is the fit matching the element.
            RecoveryBasis basis = RecoveryBasis::Shape;
            std::vector<double> factor;
            int terms = 0;
            for (;;) {
                terms = BasisTerms(basis, element.shape);
                if (terms <= points) {
                    normal.assign(Idx(terms * terms), 0.0);
                    for (int g = 0; g < points; ++g) {
                        EvaluateBasis(basis, element.shape, rule[Idx(g)].at, &values);
                        for (int i = 0; i < terms; ++i) {
                            for (int j = 0; j <= i; ++j) {
                                normal[Idx(i * terms + j)] += values[Idx(i)] * values[Idx(j)];
                            }
                        }
                    }
                    for (int i = 0; i < terms; ++i) {
                        for (int j = i + 1; j < terms; ++j) {
                            normal[Idx(i * terms + j)] = normal[Idx(j * terms + i)];
                        }
                    }
                    factor = normal;
                    if (Cholesky(&factor, terms)) break;
                }
                if (basis == RecoveryBasis::Shape) {
                    basis = RecoveryBasis::Linear;
                } else if (basis == RecoveryBasis::Linear) {
                    basis = RecoveryBasis::Constant;
                } else {
                    *error = "the quadrature rule has no points to recover stress from";
                    return false;
                }
            }
            out->element_basis[Idx(e)] = basis;
            if (basis != RecoveryBasis::Shape) ++out->shortfalls;

            coefficients.assign(Idx(terms * 6), 0.0);
            rhs.assign(Idx(terms), 0.0);
            for (int component = 0; component < 6; ++component) {
                std::fill(rhs.begin(), rhs.end(), 0.0);
                for (int g = 0; g < points; ++g) {
                    EvaluateBasis(basis, element.shape, rule[Idx(g)].at, &values);
                    for (int i = 0; i < terms; ++i) {
                        rhs[Idx(i)] += values[Idx(i)] * sampled[Idx(g)].s[component];
                    }
                }
                CholeskySolve(factor, terms, rhs.data(), &coefficients[Idx(component * terms)]);
            }
            for (int a = 0; a < count; ++a) {
                EvaluateBasis(basis, element.shape, reference_nodes[Idx(a)], &values);
                for (int component = 0; component < 6; ++component) {
                    double sum = 0.0;
                    for (int i = 0; i < terms; ++i) {
                        sum += coefficients[Idx(component * terms + i)] * values[Idx(i)];
                    }
                    at_node[Idx(a)].s[component] = sum;
                }
            }
        }

        for (int a = 0; a < count; ++a) {
            out->element_nodal.push_back(at_node[Idx(a)]);
            const int node = element.nodes[Idx(a)];
            for (int i = 0; i < 6; ++i) out->nodal[Idx(node)].s[i] += at_node[Idx(a)].s[i];
            ++out->contributions[Idx(node)];
        }
    }
    out->element_offset[model.elements.size()] = static_cast<int>(out->element_nodal.size());

    for (int node = 0; node < model.NodeCount(); ++node) {
        const int count = out->contributions[Idx(node)];
        if (count == 0) continue;
        const double share = 1.0 / static_cast<double>(count);
        for (int i = 0; i < 6; ++i) out->nodal[Idx(node)].s[i] *= share;
    }
    // PATCH RECOVERY REPLACES THE AVERAGE RATHER THAN REFINING IT. The
    // averaged field is built here first because a node the patches
    // cannot reach keeps it, and because `element_nodal` -- each
    // element's own opinion, which the discontinuity is measured against
    // -- is the same either way.
    if (how == Recovery::Patch) {
        out->element_basis.clear();
        if (!RecoverByPatch(model, displacement, out, error)) return false;
    }
    // THE SECOND PASS IS WHAT MAKES THIS AN ERROR MAP: the disagreement
    // can only be measured once the average it is measured against
    // exists. The shear terms are counted twice because the tensor has
    // them twice -- Voigt storage keeps one of each off-diagonal pair and
    // the Frobenius norm wants both.
    for (int e = 0; e < model.ElementCount(); ++e) {
        const BoundElement &element = model.elements[Idx(e)];
        const int base = out->element_offset[Idx(e)];
        for (std::size_t a = 0; a < element.nodes.size(); ++a) {
            const int node = element.nodes[a];
            const StressTensor &mine = out->element_nodal[Idx(base) + a];
            const StressTensor &average = out->nodal[Idx(node)];
            double sum = 0.0;
            for (int i = 0; i < 3; ++i) {
                const double d = mine.s[i] - average.s[i];
                sum += d * d;
            }
            for (int i = 3; i < 6; ++i) {
                const double d = mine.s[i] - average.s[i];
                sum += 2.0 * d * d;
            }
            out->discontinuity[Idx(node)] =
                std::max(out->discontinuity[Idx(node)], std::sqrt(sum));
        }
    }
    return true;
}

StressInvariants Invariants(const StressTensor &stress) {
    StressInvariants out;
    stress.Principal(out.principal);
    out.von_mises = stress.VonMises();
    out.tresca = out.principal[0] - out.principal[2];
    out.max_shear = 0.5 * out.tresca;
    out.hydrostatic = (stress.s[0] + stress.s[1] + stress.s[2]) / 3.0;
    if (out.von_mises > 0.0) out.triaxiality = out.hydrostatic / out.von_mises;
    return out;
}

const char *FailureCriterionName(FailureCriterion criterion) {
    switch (criterion) {
        case FailureCriterion::VonMises: return "von Mises";
        case FailureCriterion::Tresca: return "Tresca";
        case FailureCriterion::MaxPrincipal: return "maximum principal";
        case FailureCriterion::MohrCoulomb: return "Mohr-Coulomb";
    }
    return "?";
}

double SafetyFactor(const StressTensor &stress, const Strength &strength,
                    FailureCriterion criterion) {
    const double tensile = strength.tensile;
    const double compressive = strength.compressive > 0.0 ? strength.compressive : tensile;
    if (!(tensile > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    const StressInvariants at = Invariants(stress);
    double effective = 0.0;
    switch (criterion) {
        case FailureCriterion::VonMises:
            effective = at.von_mises / tensile;
            break;
        case FailureCriterion::Tresca:
            effective = at.tresca / tensile;
            break;
        case FailureCriterion::MaxPrincipal:
            // Compression does not break a brittle material in this
            // criterion's view, so a wholly compressive state is safe
            // however large it is. That is the criterion's opinion, not a
            // missing case: Mohr-Coulomb is the one that disagrees.
            effective = std::max(0.0, at.principal[0]) / tensile;
            break;
        case FailureCriterion::MohrCoulomb:
            // sigma_1/St - sigma_3/Sc = 1 at failure, with sigma_3
            // negative in compression so the second term adds. Reduces to
            // Tresca exactly when the two strengths are equal, which is
            // the check the tests make of it.
            effective = std::max(0.0, at.principal[0]) / tensile -
                        std::min(0.0, at.principal[2]) / compressive;
            break;
    }
    if (!(effective > 0.0)) return std::numeric_limits<double>::infinity();
    return 1.0 / effective;
}

const std::vector<ScalarField> &AllScalarFields() {
    static const std::vector<ScalarField> all = {
        ScalarField::DisplacementMagnitude, ScalarField::DisplacementX,
        ScalarField::DisplacementY,         ScalarField::DisplacementZ,
        ScalarField::VonMises,              ScalarField::Tresca,
        ScalarField::MaxPrincipal,          ScalarField::MinPrincipal,
        ScalarField::MaxShear,              ScalarField::Hydrostatic,
        ScalarField::Triaxiality,           ScalarField::StressXX,
        ScalarField::StressYY,              ScalarField::StressZZ,
        ScalarField::StressXY,              ScalarField::StressYZ,
        ScalarField::StressZX,              ScalarField::SafetyFactor,
        ScalarField::Discontinuity,
    };
    return all;
}

const char *ScalarFieldName(ScalarField field) {
    switch (field) {
        case ScalarField::DisplacementMagnitude: return "displacement";
        case ScalarField::DisplacementX: return "displacement x";
        case ScalarField::DisplacementY: return "displacement y";
        case ScalarField::DisplacementZ: return "displacement z";
        case ScalarField::VonMises: return "von Mises";
        case ScalarField::Tresca: return "Tresca";
        case ScalarField::MaxPrincipal: return "max principal";
        case ScalarField::MinPrincipal: return "min principal";
        case ScalarField::MaxShear: return "max shear";
        case ScalarField::Hydrostatic: return "hydrostatic";
        case ScalarField::Triaxiality: return "triaxiality";
        case ScalarField::StressXX: return "stress xx";
        case ScalarField::StressYY: return "stress yy";
        case ScalarField::StressZZ: return "stress zz";
        case ScalarField::StressXY: return "stress xy";
        case ScalarField::StressYZ: return "stress yz";
        case ScalarField::StressZX: return "stress zx";
        case ScalarField::SafetyFactor: return "safety factor";
        case ScalarField::Discontinuity: return "stress discontinuity";
    }
    return "?";
}

bool LargerIsWorse(ScalarField field) { return field != ScalarField::SafetyFactor; }

bool SampleField(const AnalysisModel &model, const std::vector<cad::Vec3d> &displacement,
                 const StressField &stress, ScalarField field, const Strength &strength,
                 std::vector<double> *out, std::string *error) {
    if (displacement.size() != model.nodes.size()) {
        *error = "the displacement vector is not one per node";
        return false;
    }
    if (field != ScalarField::DisplacementMagnitude && field != ScalarField::DisplacementX &&
        field != ScalarField::DisplacementY && field != ScalarField::DisplacementZ &&
        stress.nodal.size() != model.nodes.size()) {
        *error = "the stress field does not match the model";
        return false;
    }
    out->assign(model.nodes.size(), 0.0);
    for (int node = 0; node < model.NodeCount(); ++node) {
        const cad::Vec3d &u = displacement[Idx(node)];
        double value = 0.0;
        switch (field) {
            case ScalarField::DisplacementMagnitude: value = u.Length(); break;
            case ScalarField::DisplacementX: value = u.x; break;
            case ScalarField::DisplacementY: value = u.y; break;
            case ScalarField::DisplacementZ: value = u.z; break;
            case ScalarField::Discontinuity: value = stress.discontinuity[Idx(node)]; break;
            default: {
                const StressTensor &s = stress.nodal[Idx(node)];
                if (field == ScalarField::SafetyFactor) {
                    value = SafetyFactor(s, strength, FailureCriterion::VonMises);
                    break;
                }
                if (field >= ScalarField::StressXX && field <= ScalarField::StressZX) {
                    const int component = static_cast<int>(field) -
                                          static_cast<int>(ScalarField::StressXX);
                    value = s.s[Idx(component)];
                    break;
                }
                const StressInvariants at = Invariants(s);
                switch (field) {
                    case ScalarField::VonMises: value = at.von_mises; break;
                    case ScalarField::Tresca: value = at.tresca; break;
                    case ScalarField::MaxPrincipal: value = at.principal[0]; break;
                    case ScalarField::MinPrincipal: value = at.principal[2]; break;
                    case ScalarField::MaxShear: value = at.max_shear; break;
                    case ScalarField::Hydrostatic: value = at.hydrostatic; break;
                    case ScalarField::Triaxiality: value = at.triaxiality; break;
                    default: break;
                }
                break;
            }
        }
        (*out)[Idx(node)] = value;
    }
    return true;
}

}  // namespace fem
