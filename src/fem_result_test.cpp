// Part J.1: result fields and stress recovery, verified.

#include "fem_result.h"

#include "fem_elem.h"
#include "fem_static.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char *what, int line) {
    ++checks;
    if (ok) return;
    std::fflush(stdout);
    std::printf("CHECK FAILED: %s at %s:%d\n", what, __FILE__, line);
    ++failures;
}

#define CHECK(x) Check((x), #x, __LINE__)

using cad::Vec3d;
using fem::AnalysisModel;
using fem::BoundElement;
using fem::ElementShape;
using fem::Recovery;
using fem::RecoveryBasis;
using fem::StressField;
using fem::StressTensor;
using fem::StudyMaterial;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

constexpr double kPi = 3.14159265358979323846;
constexpr double kE = 1.0;
constexpr double kNu = 0.3;
const double kMu = kE / (2.0 * (1.0 + kNu));

// --- A field whose stress is writable in closed form -----------------------
//
// The same divergence-free curl field Part H.6's manufactured solution
// uses, and it is here for a different reason. THIS TEST DOES NOT SOLVE
// ANYTHING. It sets the nodal displacements to the exact field's values
// and asks the recovery what stress it makes of them, which separates the
// recovery's own error from the solver's completely: any discrepancy is
// the interpolation and the recovery and nothing else. A test that solved
// first would be measuring the sum of two errors and attributing it to
// one of them.
Vec3d Exact(const Vec3d &p) {
    const double sx = std::sin(kPi * p.x), sy = std::sin(kPi * p.y), sz = std::sin(kPi * p.z);
    const double cx = std::cos(kPi * p.x), cy = std::cos(kPi * p.y);
    return Vec3d{kPi * sx * cy * sz, -kPi * cx * sy * sz, 0.0};
}

// Divergence-free, so the trace of the strain vanishes and the stress is
// simply twice the shear modulus times it -- no Lame term at all.
StressTensor ExactStress(const Vec3d &p) {
    const double sx = std::sin(kPi * p.x), sy = std::sin(kPi * p.y), sz = std::sin(kPi * p.z);
    const double cx = std::cos(kPi * p.x), cy = std::cos(kPi * p.y), cz = std::cos(kPi * p.z);
    const double k = kPi * kPi;
    StressTensor out;
    out.s[0] = 2.0 * kMu * k * cx * cy * sz;
    out.s[1] = -2.0 * kMu * k * cx * cy * sz;
    out.s[2] = 0.0;
    out.s[3] = 0.0;
    out.s[4] = -kMu * k * cx * sy * cz;
    out.s[5] = kMu * k * sx * cy * cz;
    return out;
}

// --- Meshes ----------------------------------------------------------------

AnalysisModel Cube(ElementShape shape, int m, bool distort) {
    AnalysisModel model;
    StudyMaterial material;
    material.youngs_modulus = fem::MaterialCurve::Constant(kE);
    material.poissons_ratio = fem::MaterialCurve::Constant(kNu);
    model.materials.push_back(material);

    const int side = m + 1;
    for (int k = 0; k < side; ++k) {
        for (int j = 0; j < side; ++j) {
            for (int i = 0; i < side; ++i) {
                Vec3d p{static_cast<double>(i) / m, static_cast<double>(j) / m,
                        static_cast<double>(k) / m};
                // AN UNDISTORTED MESH IS THE EASY CASE AND HIDES THINGS.
                // On a regular grid the isoparametric map is affine, the
                // Jacobian is constant, and an extrapolation that got the
                // reference coordinates confused would still work. Moving
                // the interior nodes breaks all three.
                const bool interior = i > 0 && i < m && j > 0 && j < m && k > 0 && k < m;
                if (distort && interior) {
                    const double d = 0.22 / m;
                    p.x += d * std::sin(3.0 * p.y + 1.0);
                    p.y += d * std::sin(3.0 * p.z + 2.0);
                    p.z += d * std::sin(3.0 * p.x + 3.0);
                }
                model.nodes.push_back(p);
            }
        }
    }
    auto at = [&](int i, int j, int k) { return (k * side + j) * side + i; };
    for (int k = 0; k < m; ++k) {
        for (int j = 0; j < m; ++j) {
            for (int i = 0; i < m; ++i) {
                const int h[8] = {at(i, j, k),         at(i + 1, j, k),
                                  at(i + 1, j + 1, k), at(i, j + 1, k),
                                  at(i, j, k + 1),     at(i + 1, j, k + 1),
                                  at(i + 1, j + 1, k + 1), at(i, j + 1, k + 1)};
                if (shape == ElementShape::Hex8 || shape == ElementShape::Hex20) {
                    BoundElement element;
                    element.shape = ElementShape::Hex8;
                    element.nodes = {h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]};
                    model.elements.push_back(element);
                    continue;
                }
                const int split[6][4] = {{h[0], h[1], h[2], h[6]}, {h[0], h[2], h[3], h[6]},
                                         {h[0], h[3], h[7], h[6]}, {h[0], h[7], h[4], h[6]},
                                         {h[0], h[4], h[5], h[6]}, {h[0], h[5], h[1], h[6]}};
                for (const auto &tet : split) {
                    BoundElement element;
                    element.shape = ElementShape::Tet4;
                    element.nodes = {tet[0], tet[1], tet[2], tet[3]};
                    model.elements.push_back(element);
                }
            }
        }
    }
    if (shape != ElementShape::Tet10 && shape != ElementShape::Hex20) return model;

    static const int tet_edges[6][2] = {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}};
    // The order the element library's own mid-side nodes follow its
    // corners. Getting it wrong builds a hex that is inside out at half
    // its quadrature points, which is what happened and is why this is a
    // copy of that table rather than a plausible-looking one.
    static const int hex_edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {0, 4}, {1, 5},
                                         {2, 6}, {3, 7}, {4, 5}, {5, 6}, {6, 7}, {7, 4}};
    const int edge_count = shape == ElementShape::Tet10 ? 6 : 12;
    std::map<std::pair<int, int>, int> middles;
    for (BoundElement &element : model.elements) {
        const std::vector<int> corners = element.nodes;
        element.shape = shape;
        for (int e = 0; e < edge_count; ++e) {
            const int a = shape == ElementShape::Tet10 ? tet_edges[e][0] : hex_edges[e][0];
            const int b = shape == ElementShape::Tet10 ? tet_edges[e][1] : hex_edges[e][1];
            const int u = corners[Idx(a)];
            const int v = corners[Idx(b)];
            const std::pair<int, int> key = u < v ? std::make_pair(u, v) : std::make_pair(v, u);
            const auto found = middles.find(key);
            if (found != middles.end()) {
                element.nodes.push_back(found->second);
                continue;
            }
            const int index = model.NodeCount();
            model.nodes.push_back((model.nodes[Idx(u)] + model.nodes[Idx(v)]) * 0.5);
            middles[key] = index;
            element.nodes.push_back(index);
        }
    }
    return model;
}

std::vector<double> SampleExact(const AnalysisModel &model) {
    std::vector<double> out(Idx(model.NodeCount() * 3), 0.0);
    for (int node = 0; node < model.NodeCount(); ++node) {
        const Vec3d u = Exact(model.nodes[Idx(node)]);
        out[Idx(node * 3 + 0)] = u.x;
        out[Idx(node * 3 + 1)] = u.y;
        out[Idx(node * 3 + 2)] = u.z;
    }
    return out;
}

double Frobenius(const StressTensor &a, const StressTensor &b) {
    double sum = 0.0;
    for (int i = 0; i < 3; ++i) sum += (a.s[i] - b.s[i]) * (a.s[i] - b.s[i]);
    for (int i = 3; i < 6; ++i) sum += 2.0 * (a.s[i] - b.s[i]) * (a.s[i] - b.s[i]);
    return std::sqrt(sum);
}

// --- Invariants ------------------------------------------------------------

void TestInvariants() {
    std::printf("stress invariants against their closed forms:\n");
    {
        // Uniaxial tension: the one state where every criterion agrees,
        // which is exactly why every strength is quoted in it.
        StressTensor s;
        s.s[0] = 250.0;
        const fem::StressInvariants at = fem::Invariants(s);
        CHECK(Near(at.principal[0], 250.0, 1e-12));
        CHECK(Near(at.principal[1], 0.0, 1e-12));
        CHECK(Near(at.principal[2], 0.0, 1e-12));
        CHECK(Near(at.von_mises, 250.0, 1e-12));
        CHECK(Near(at.tresca, 250.0, 1e-12));
        CHECK(Near(at.hydrostatic, 250.0 / 3.0, 1e-12));
        CHECK(Near(at.triaxiality, 1.0 / 3.0, 1e-12));
        std::printf("  uniaxial 250: von Mises %.6f, Tresca %.6f, triaxiality %.6f\n",
                    at.von_mises, at.tresca, at.triaxiality);
    }
    {
        // Pure shear: where the two ductile criteria disagree most, by
        // exactly 2/sqrt(3).
        StressTensor s;
        s.s[3] = 100.0;
        const fem::StressInvariants at = fem::Invariants(s);
        CHECK(Near(at.principal[0], 100.0, 1e-10));
        CHECK(Near(at.principal[1], 0.0, 1e-10));
        CHECK(Near(at.principal[2], -100.0, 1e-10));
        CHECK(Near(at.von_mises, 100.0 * std::sqrt(3.0), 1e-10));
        CHECK(Near(at.tresca, 200.0, 1e-10));
        CHECK(Near(at.hydrostatic, 0.0, 1e-12));
        CHECK(Near(at.triaxiality, 0.0, 1e-12));
        std::printf("  pure shear 100: Tresca over von Mises %.9f (2/sqrt3 is %.9f)\n",
                    at.tresca / at.von_mises, 2.0 / std::sqrt(3.0));
        CHECK(Near(at.tresca / at.von_mises, 2.0 / std::sqrt(3.0), 1e-12));
    }
    {
        // Hydrostatic: von Mises is blind to it, and that blindness is
        // the criterion's content rather than a limitation of this code.
        StressTensor s;
        s.s[0] = s.s[1] = s.s[2] = -1e9;
        const fem::StressInvariants at = fem::Invariants(s);
        CHECK(Near(at.von_mises, 0.0, 1e-6));
        CHECK(Near(at.tresca, 0.0, 1e-6));
        CHECK(Near(at.hydrostatic, -1e9, 1e-3));
        CHECK(at.triaxiality == 0.0);
        std::printf("  a gigapascal of pressure: von Mises %.3e, hydrostatic %.3e\n",
                    at.von_mises, at.hydrostatic);
    }

    // THE INVARIANTS ARE INVARIANTS, which is a property the closed forms
    // above cannot test because they are all aligned with the axes. Over
    // random tensors: the principal values must reproduce all three
    // characteristic coefficients of the tensor they came from, and the
    // ratio of the two ductile criteria must stay inside [1, 2/sqrt(3)]
    // -- a bound that follows from the geometry of the two surfaces and
    // has nothing to do with how either is computed here.
    std::mt19937 rng(20260925u);
    std::uniform_real_distribution<double> spread(-100.0, 100.0);
    double worst_trace = 0.0, worst_second = 0.0, worst_determinant = 0.0;
    double lowest_ratio = 1e9, highest_ratio = 0.0;
    for (int trial = 0; trial < 20000; ++trial) {
        StressTensor s;
        for (int i = 0; i < 6; ++i) s.s[i] = spread(rng);
        const fem::StressInvariants at = fem::Invariants(s);
        CHECK(at.principal[0] >= at.principal[1] && at.principal[1] >= at.principal[2]);
        const double trace = s.s[0] + s.s[1] + s.s[2];
        const double second = s.s[0] * s.s[1] + s.s[1] * s.s[2] + s.s[2] * s.s[0] -
                              s.s[3] * s.s[3] - s.s[4] * s.s[4] - s.s[5] * s.s[5];
        const double determinant = s.s[0] * (s.s[1] * s.s[2] - s.s[4] * s.s[4]) -
                                   s.s[3] * (s.s[3] * s.s[2] - s.s[4] * s.s[5]) +
                                   s.s[5] * (s.s[3] * s.s[4] - s.s[1] * s.s[5]);
        const double p0 = at.principal[0], p1 = at.principal[1], p2 = at.principal[2];
        // SCALED BY THE LARGEST PRINCIPAL MAGNITUDE, not by p0. A wholly
        // compressive state has a small p0 and a large p2, and dividing
        // the second and third coefficients by the small one turns a
        // correct eigenvalue into a relative error of order one.
        const double scale = std::max(std::max(std::fabs(p0), std::fabs(p2)), 1e-30);
        worst_trace = std::max(worst_trace, std::fabs(p0 + p1 + p2 - trace) / scale);
        worst_second =
            std::max(worst_second, std::fabs(p0 * p1 + p1 * p2 + p2 * p0 - second) / (scale * scale));
        worst_determinant = std::max(worst_determinant,
                                     std::fabs(p0 * p1 * p2 - determinant) / (scale * scale * scale));
        if (at.von_mises > 1e-9) {
            const double ratio = at.tresca / at.von_mises;
            lowest_ratio = std::min(lowest_ratio, ratio);
            highest_ratio = std::max(highest_ratio, ratio);
        }
    }
    std::printf("  over 20000 random tensors the three characteristic coefficients are\n");
    std::printf("    reproduced to %.2e, %.2e and %.2e relative\n", worst_trace, worst_second,
                worst_determinant);
    std::printf("  Tresca over von Mises stays in [%.6f, %.6f]; the bound is [1, %.6f]\n",
                lowest_ratio, highest_ratio, 2.0 / std::sqrt(3.0));
    CHECK(worst_trace < 1e-13);
    CHECK(worst_second < 1e-13);
    CHECK(worst_determinant < 1e-13);
    CHECK(lowest_ratio >= 1.0 - 1e-12);
    CHECK(highest_ratio <= 2.0 / std::sqrt(3.0) + 1e-12);
    // The upper end is attained exactly, by every state whose middle
    // principal stress is the mean of the other two. The lower end is
    // only approached -- it needs an exactly uniaxial state, which random
    // sampling never produces -- so how close it gets is a fact about the
    // sampling, and asking for better would be asking the wrong thing.
    CHECK(lowest_ratio < 1.01);
    CHECK(highest_ratio > 2.0 / std::sqrt(3.0) - 0.001);
}

void TestSafetyFactors() {
    std::printf("safety factors:\n");
    fem::Strength steel;
    steel.tensile = 250e6;
    {
        StressTensor s;
        s.s[0] = 125e6;
        CHECK(Near(fem::SafetyFactor(s, steel, fem::FailureCriterion::VonMises), 2.0, 1e-12));
        CHECK(Near(fem::SafetyFactor(s, steel, fem::FailureCriterion::Tresca), 2.0, 1e-12));
        CHECK(Near(fem::SafetyFactor(s, steel, fem::FailureCriterion::MaxPrincipal), 2.0, 1e-12));
        std::printf("  half the yield strength, uniaxial: every criterion says 2.000000\n");
    }
    {
        // In pure shear Tresca is the more conservative by 2/sqrt(3), and
        // a design code that names one and a solver that plots the other
        // differ by 15.5% on a shaft in torsion.
        StressTensor s;
        s.s[3] = 100e6;
        const double mises = fem::SafetyFactor(s, steel, fem::FailureCriterion::VonMises);
        const double tresca = fem::SafetyFactor(s, steel, fem::FailureCriterion::Tresca);
        std::printf("  100 MPa of pure shear: von Mises %.6f, Tresca %.6f (%.1f%% apart)\n", mises,
                    tresca, 100.0 * (mises / tresca - 1.0));
        CHECK(Near(mises / tresca, 2.0 / std::sqrt(3.0), 1e-12));
    }
    {
        // A brittle material in pure compression: Rankine says it cannot
        // break, Mohr-Coulomb says it can, and the gap between them is
        // the whole reason concrete needs the second criterion.
        StressTensor s;
        s.s[0] = s.s[1] = s.s[2] = 0.0;
        s.s[2] = -30e6;
        fem::Strength concrete;
        concrete.tensile = 3e6;
        concrete.compressive = 30e6;
        const double rankine = fem::SafetyFactor(s, concrete, fem::FailureCriterion::MaxPrincipal);
        const double coulomb = fem::SafetyFactor(s, concrete, fem::FailureCriterion::MohrCoulomb);
        std::printf("  30 MPa of compression on concrete: Rankine %.3f, Mohr-Coulomb %.6f\n",
                    rankine, coulomb);
        CHECK(std::isinf(rankine));
        CHECK(Near(coulomb, 1.0, 1e-12));
    }
    {
        // MOHR-COULOMB WITH ONE STRENGTH IS TRESCA -- but only where the
        // state straddles zero, and I first wrote this test as though it
        // held everywhere. It does not, and the difference is the
        // criterion rather than a bug in it: with the largest and
        // smallest principal stresses both positive, Tresca compares
        // their difference against the strength while Mohr-Coulomb
        // compares the tensile one on its own, and those are different
        // statements about a material. Restricting the comparison to the
        // straddling case is what makes it a test of the formula.
        std::mt19937 rng(7u);
        std::uniform_real_distribution<double> spread(-100e6, 100e6);
        double worst = 0.0;
        int straddling = 0;
        for (int trial = 0; trial < 5000; ++trial) {
            StressTensor s;
            for (int i = 0; i < 6; ++i) s.s[i] = spread(rng);
            double principal[3];
            s.Principal(principal);
            if (!(principal[0] > 0.0 && principal[2] < 0.0)) continue;
            ++straddling;
            const double tresca = fem::SafetyFactor(s, steel, fem::FailureCriterion::Tresca);
            const double coulomb = fem::SafetyFactor(s, steel, fem::FailureCriterion::MohrCoulomb);
            worst = std::max(worst, std::fabs(tresca - coulomb) / std::max(1.0, tresca));
        }
        std::printf("  with equal strengths Mohr-Coulomb is Tresca where the state straddles\n");
        std::printf("    zero: %.2e over the %d of 5000 tensors that do\n", worst, straddling);
        CHECK(worst < 1e-12);
        CHECK(straddling > 3000);
    }
    {
        // Zero stress is infinitely safe, and saying so with an infinity
        // rather than a large number is what keeps a contour plot honest.
        StressTensor s;
        CHECK(std::isinf(fem::SafetyFactor(s, steel, fem::FailureCriterion::VonMises)));
        CHECK(std::isnan(fem::SafetyFactor(s, fem::Strength{}, fem::FailureCriterion::VonMises)));
    }
}

// --- Recovery --------------------------------------------------------------

void TestConstantStressPatch() {
    std::printf("the patch test: a linear displacement field, on distorted meshes:\n");
    // A LINEAR DISPLACEMENT FIELD HAS CONSTANT STRESS, and every element
    // in the library can represent it exactly. So the recovery must
    // return that constant at every node of every element, and the
    // disagreement between neighbours must be zero -- not small, zero.
    // Anything else means the extrapolation is inventing a gradient,
    // which is precisely the failure that would otherwise show up only as
    // a slightly wrong stress concentration on a real model.
    const ElementShape shapes[] = {ElementShape::Tet4, ElementShape::Tet10, ElementShape::Hex8,
                                   ElementShape::Hex20};
    for (const ElementShape shape : shapes) {
        AnalysisModel model = Cube(shape, 3, true);
        std::vector<double> displacement(Idx(model.NodeCount() * 3), 0.0);
        for (int node = 0; node < model.NodeCount(); ++node) {
            const Vec3d &p = model.nodes[Idx(node)];
            // A general linear field, not an axis-aligned stretch: it has
            // every strain component non-zero, so a transposed Jacobian
            // or a swapped Voigt index cannot hide in it.
            displacement[Idx(node * 3 + 0)] = 1e-3 * (2.0 * p.x + 0.7 * p.y - 0.3 * p.z);
            displacement[Idx(node * 3 + 1)] = 1e-3 * (-0.4 * p.x + 1.1 * p.y + 0.9 * p.z);
            displacement[Idx(node * 3 + 2)] = 1e-3 * (0.2 * p.x - 0.6 * p.y + 1.7 * p.z);
        }
        StressField field;
        std::string error;
        CHECK(fem::RecoverStress(model, displacement, Recovery::Extrapolated, &field, &error));
        const StressTensor reference = field.element_centre[0];
        double worst = 0.0;
        double scale = 0.0;
        for (int i = 0; i < 6; ++i) scale = std::max(scale, std::fabs(reference.s[i]));
        for (const StressTensor &at : field.element_nodal) {
            worst = std::max(worst, Frobenius(at, reference));
        }
        double worst_gap = 0.0;
        for (const double d : field.discontinuity) worst_gap = std::max(worst_gap, d);
        std::printf("  %-6s %5d elements: worst nodal error %.2e, worst neighbour gap %.2e"
                    " (stress %.2e)\n",
                    fem::ElementShapeName(shape), model.ElementCount(), worst, worst_gap, scale);
        CHECK(worst < scale * 1e-11);
        CHECK(worst_gap < scale * 1e-11);
    }
}

void TestBasisStepsDown() {
    std::printf("what each element's quadrature rule can support:\n");
    // The step-down is reported, not silent. A Tet4 is integrated at one
    // point and a Tet10 at four, so neither can support a fit in its own
    // shape functions -- and neither needs to, because a Tet4's stress is
    // constant and a Tet10's is linear. An element whose fit fell short
    // *and* whose stress is not that simple would be a real loss of
    // accuracy, which is why the count is in the result.
    struct Expected {
        ElementShape shape;
        RecoveryBasis basis;
    };
    // Only the Tet4 falls short, and it is the one that loses nothing by
    // it: one Gauss point, a constant fit, and a constant stress field to
    // fit. I expected the Tet10 to step down too, on the assumption its
    // rule had four points; it has eleven, which is more than its ten
    // shape functions, so its fit is a least-squares one over its own
    // basis. Reading the rule rather than assuming it is the point.
    const Expected expected[] = {{ElementShape::Tet4, RecoveryBasis::Constant},
                                 {ElementShape::Tet10, RecoveryBasis::Shape},
                                 {ElementShape::Hex8, RecoveryBasis::Shape},
                                 {ElementShape::Hex20, RecoveryBasis::Shape}};
    for (const Expected &want : expected) {
        AnalysisModel model = Cube(want.shape, 2, false);
        std::vector<double> displacement(Idx(model.NodeCount() * 3), 0.0);
        for (int node = 0; node < model.NodeCount(); ++node) {
            displacement[Idx(node * 3)] = 1e-3 * model.nodes[Idx(node)].x;
        }
        StressField field;
        std::string error;
        CHECK(fem::RecoverStress(model, displacement, Recovery::Extrapolated, &field, &error));
        std::vector<fem::QuadraturePoint> rule;
        std::string ignored;
        fem::Quadrature(want.shape, 0, &rule, &ignored);
        std::printf("  %-6s %2d nodes, %2d quadrature points -> fits %s\n",
                    fem::ElementShapeName(want.shape), fem::ElementNodeCount(want.shape),
                    static_cast<int>(rule.size()), fem::RecoveryBasisName(field.element_basis[0]));
        for (const RecoveryBasis basis : field.element_basis) CHECK(basis == want.basis);
    }
}

// Returns the worst nodal error, and through `out_rms` the root-mean-
// square one. BOTH, BECAUSE THEY SAY DIFFERENT THINGS. Superconvergence
// is a statement in the mean -- it is proved in an integral norm -- and
// the worst node of a mesh is almost always on the surface, where the
// patch is borrowed and evaluated beyond its data. Reporting only the
// max would understate the gain; reporting only the mean would hide that
// the surface is harder. The rate is the same in both.
double RecoveryError(const AnalysisModel &model, Recovery how, double *out_worst_gap,
                     double *out_rms = nullptr, double *out_interior_rms = nullptr,
                     double *out_surface_rms = nullptr) {
    const std::vector<double> displacement = SampleExact(model);
    StressField field;
    std::string error;
    if (!fem::RecoverStress(model, displacement, how, &field, &error)) {
        std::printf("    recovery failed: %s\n", error.c_str());
        return -1.0;
    }
    double worst = 0.0;
    double sum = 0.0;
    double interior_sum = 0.0;
    double surface_sum = 0.0;
    int counted = 0;
    int interior = 0;
    int surface = 0;
    for (int node = 0; node < model.NodeCount(); ++node) {
        if (field.contributions[Idx(node)] == 0) continue;
        const cad::Vec3d &p = model.nodes[Idx(node)];
        const double e = Frobenius(field.nodal[Idx(node)], ExactStress(p));
        worst = std::max(worst, e);
        sum += e * e;
        ++counted;
        const bool on_surface = p.x <= 1e-12 || p.x >= 1.0 - 1e-12 || p.y <= 1e-12 ||
                                p.y >= 1.0 - 1e-12 || p.z <= 1e-12 || p.z >= 1.0 - 1e-12;
        if (on_surface) {
            surface_sum += e * e;
            ++surface;
            continue;
        }
        interior_sum += e * e;
        ++interior;
    }
    if (out_rms != nullptr) {
        *out_rms = counted > 0 ? std::sqrt(sum / static_cast<double>(counted)) : 0.0;
    }
    if (out_interior_rms != nullptr) {
        *out_interior_rms =
            interior > 0 ? std::sqrt(interior_sum / static_cast<double>(interior)) : 0.0;
    }
    if (out_surface_rms != nullptr) {
        *out_surface_rms = surface > 0 ? std::sqrt(surface_sum / static_cast<double>(surface)) : 0.0;
    }
    if (out_worst_gap != nullptr) {
        *out_worst_gap = 0.0;
        for (const double d : field.discontinuity) {
            *out_worst_gap = std::max(*out_worst_gap, d);
        }
    }
    return worst;
}

double Rate(double coarse, double fine) { return std::log2(coarse / fine); }

void TestPatchRecoveryBeatsBoth() {
    std::printf("recovered stress against a field whose stress is known exactly:\n");
    // NO SOLVE HAPPENS HERE. The nodal displacements are set to the exact
    // field, so the only error in the answer is the element's own
    // interpolation and this recovery. That separation matters: a test
    // that solved first would be measuring the sum of two errors and
    // attributing it to one of them.
    //
    // WHAT THIS MEASURED THAT I DID NOT EXPECT. Extrapolating from the
    // Gauss points came out *bit for bit identical* to evaluating at the
    // nodes, on every mesh, for both element types. It is not a bug and
    // it is not a coincidence: on an affine map a trilinear hexahedron's
    // stress field is exactly trilinear, so fitting eight shape functions
    // through eight Gauss points is exact interpolation, and re-reading
    // that same polynomial at the nodes returns what evaluating there
    // directly returns. Superconvergence is a property of where the
    // element is *sampled*, and moving a polynomial's argument does not
    // create it.
    //
    // Patch recovery is the one that is actually different, because its
    // fit is over-determined -- a patch holds far more samples than the
    // polynomial has terms -- so it is a genuine least-squares smoothing
    // of superconvergent data. That shows up here as a better convergence
    // *rate*, which is the part that cannot be arranged by luck.
    struct Row {
        ElementShape shape;
        int meshes[3];
    };
    const Row rows[] = {{ElementShape::Hex8, {4, 8, 16}}, {ElementShape::Hex20, {2, 4, 8}}};
    for (const Row &row : rows) {
        std::printf("  %s:\n", fem::ElementShapeName(row.shape));
        double at_nodes[3] = {0, 0, 0};
        double extrapolated[3] = {0, 0, 0};
        double patch[3] = {0, 0, 0};
        double naive_rms[3] = {0, 0, 0};
        double patch_rms[3] = {0, 0, 0};
        double naive_inside[3] = {0, 0, 0};
        double patch_inside[3] = {0, 0, 0};
        double naive_surface[3] = {0, 0, 0};
        double patch_surface[3] = {0, 0, 0};
        double gaps[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
            AnalysisModel model = Cube(row.shape, row.meshes[i], false);
            at_nodes[i] = RecoveryError(model, Recovery::AtNodes, nullptr, &naive_rms[i],
                                        &naive_inside[i], &naive_surface[i]);
            extrapolated[i] = RecoveryError(model, Recovery::Extrapolated, nullptr);
            patch[i] = RecoveryError(model, Recovery::Patch, &gaps[i], &patch_rms[i],
                                     &patch_inside[i], &patch_surface[i]);
            std::printf("    %2d per side: rms inside %.3e -> %.3e (%.2fx), on the surface"
                        " %.3e -> %.3e (%.2fx)\n",
                        row.meshes[i], naive_inside[i], patch_inside[i],
                        naive_inside[i] / patch_inside[i], naive_surface[i], patch_surface[i],
                        naive_surface[i] / patch_surface[i]);
            // Identical in every norm: the same polynomial read at the
            // same place.
            CHECK(std::fabs(extrapolated[i] - at_nodes[i]) <= at_nodes[i] * 1e-12);
            // Nothing is asserted per mesh here: the coarse ones are
            // worse and the reason is below.
            CHECK(patch_surface[i] > 0.0);
            CHECK(patch[i] > 0.0);
        }
        const double naive_rate =
            0.5 * (Rate(at_nodes[0], at_nodes[1]) + Rate(at_nodes[1], at_nodes[2]));
        const double patch_rate = 0.5 * (Rate(patch[0], patch[1]) + Rate(patch[1], patch[2]));
        const double naive_inside_rate =
            0.5 * (Rate(naive_inside[0], naive_inside[1]) + Rate(naive_inside[1], naive_inside[2]));
        const double patch_inside_rate =
            0.5 * (Rate(patch_inside[0], patch_inside[1]) + Rate(patch_inside[1], patch_inside[2]));
        const double naive_surface_rate = 0.5 * (Rate(naive_surface[0], naive_surface[1]) +
                                                 Rate(naive_surface[1], naive_surface[2]));
        const double patch_surface_rate = 0.5 * (Rate(patch_surface[0], patch_surface[1]) +
                                                 Rate(patch_surface[1], patch_surface[2]));
        const double gap_rate = 0.5 * (Rate(gaps[0], gaps[1]) + Rate(gaps[1], gaps[2]));
        std::printf("    rate inside %.3f -> %.3f, on the surface %.3f -> %.3f,"
                    " at the worst node %.3f -> %.3f\n",
                    naive_inside_rate, patch_inside_rate, naive_surface_rate, patch_surface_rate,
                    naive_rate, patch_rate);
        std::printf("    the gap between the patch field and the elements falls at rate %.3f\n",
                    gap_rate);
        // BETTER IN THE MEAN ON EVERY MESH, AND AT THE WORST NODE ONLY
        // ASYMPTOTICALLY. The worst node is on the surface, where the
        // patch is borrowed from inside and evaluated beyond the data it
        // was fitted to, and on a mesh too coarse to resolve the field
        // that extrapolation overshoots -- on the coarsest Hex20 mesh
        // here by four times. The rate is the same in both norms and it
        // is a whole order better, which is the claim being made; a
        // single mesh's number is not.
        // THE SURFACE GAINS A WHOLE ORDER; THE INTERIOR GAINS NOTHING,
        // because there was nothing there to gain. Plain averaging over
        // the elements meeting at an interior node of a regular mesh is
        // *already* superconvergent -- the errors of the elements on
        // opposite sides are equal and opposite and cancel in the mean --
        // so it converges at very nearly the same rate patch recovery
        // does, with a smaller constant. That cancellation is one-sided
        // at a boundary node, which is why averaging is only first-order
        // there and why the worst node of the mesh is always on the
        // surface. SPR's entire value is fixing exactly that, which is
        // also where every result is read.
        CHECK(patch_surface_rate > naive_surface_rate + 1.2);
        CHECK(patch_rate > naive_rate + 0.8);
        // AND THE INTERIOR GAINS NO ORDER AT ALL -- asserted, because it
        // is the half of the explanation that is easy to leave out.
        CHECK(patch_inside_rate < naive_inside_rate + 0.4);
        // By the finest mesh the surface gain has carried the whole mesh
        // in the mean. Not yet at the worst single node for the Hex20,
        // where the crossover is still in progress at eight elements a
        // side; the rate says where that is going.
        CHECK(patch_surface[2] < naive_surface[2]);
        CHECK(patch_rms[2] < naive_rms[2]);
        // AND THE GAP IS THE ERROR'S PROXY. It needs no exact answer to
        // compute -- it is the difference between the recovered field and
        // each element's own -- and it falls at the same rate as the
        // error it stands in for. That equivalence is the entire basis of
        // Part J.5 estimating an error it cannot measure, so it is
        // checked here rather than assumed there.
        CHECK(gap_rate > naive_rate - 0.35);
    }
}

void TestPatchesAtTheBoundary() {
    std::printf("patches at the boundary, where the stress concentrations are:\n");
    // A CORNER NODE OF A HEX8 MESH TOUCHES ONE ELEMENT, AND SO HAS ONE
    // SAMPLE, because the Barlow point of a trilinear element is its
    // centre. A linear patch needs four. Nor is a node on a face any
    // better off: it touches four elements whose centres are coplanar, so
    // the normal matrix is singular in the direction off that plane and
    // the fit fails for a reason no count of samples would reveal. On a
    // hex mesh, then, *every* surface node borrows -- which is the
    // situation Zienkiewicz and Zhu's prescription was written for. The
    // fallback is not to drop the order there -- that would make the recovery worst exactly where a
    // result is read -- but to evaluate an interior neighbour's
    // polynomial out at the corner. Checking how many nodes take each
    // route is checking that the fallback is actually exercised rather
    // than dead code, and the error at the borrowing nodes is checked
    // against the error everywhere else so that "it ran" is not mistaken
    // for "it worked".
    for (const ElementShape shape : {ElementShape::Hex8, ElementShape::Hex20}) {
        AnalysisModel model = Cube(shape, 4, false);
        StressField field;
        std::string error;
        CHECK(fem::RecoverStress(model, SampleExact(model), Recovery::Patch, &field, &error));
        std::printf("  %-6s %d nodes: %d own, %d borrowed, %d reduced, %d averaged\n",
                    fem::ElementShapeName(shape), model.NodeCount(), field.own_patch,
                    field.borrowed_patch, field.reduced_order, field.averaged);
        CHECK(field.own_patch + field.borrowed_patch + field.reduced_order + field.averaged ==
              model.NodeCount());
        // Every surface node borrows, and none of them silently drops an
        // order or falls back to averaging. On a 4-per-side cube that is
        // 27 interior nodes against 98 on the surface, so the borrowing
        // path carries four fifths of the mesh: it is the common case,
        // not an edge case.
        CHECK(field.averaged == 0);
        CHECK(field.reduced_order == 0);
        CHECK(field.borrowed_patch > field.own_patch);
        double worst_interior = 0.0;
        double worst_boundary = 0.0;
        for (int node = 0; node < model.NodeCount(); ++node) {
            const Vec3d &p = model.nodes[Idx(node)];
            const bool on_surface = p.x <= 1e-12 || p.x >= 1.0 - 1e-12 || p.y <= 1e-12 ||
                                    p.y >= 1.0 - 1e-12 || p.z <= 1e-12 || p.z >= 1.0 - 1e-12;
            const double e = Frobenius(field.nodal[Idx(node)], ExactStress(p));
            if (on_surface) {
                worst_boundary = std::max(worst_boundary, e);
            } else {
                worst_interior = std::max(worst_interior, e);
            }
        }
        std::printf("    worst error: interior %.3e, surface %.3e (%.2f times)\n", worst_interior,
                    worst_boundary, worst_boundary / worst_interior);
        // The surface is genuinely harder -- the patch is one-sided and
        // the polynomial is used slightly beyond its data -- so it is
        // allowed to be worse, but not by an order.
        CHECK(worst_boundary < worst_interior * 10.0);
    }
    // And a single element, where there is no neighbour to borrow from at
    // all, must still produce an answer rather than a failure.
    AnalysisModel one = Cube(ElementShape::Hex8, 1, false);
    StressField field;
    std::string error;
    CHECK(fem::RecoverStress(one, SampleExact(one), Recovery::Patch, &field, &error));
    std::printf("  a mesh of one element: %d own, %d borrowed, %d reduced, %d averaged\n",
                field.own_patch, field.borrowed_patch, field.reduced_order, field.averaged);
    CHECK(field.own_patch + field.borrowed_patch + field.reduced_order + field.averaged ==
          one.NodeCount());
    // One trilinear element has one Barlow point, which determines a
    // constant and nothing more. The reduced-order pass fits that, and
    // the other seven nodes then borrow it -- so the answer is the
    // element's centroid stress everywhere, which is the right answer to
    // give and is reported as reduced rather than passed off as a fit.
    CHECK(field.reduced_order == 1);
}

void TestFieldSampling() {
    std::printf("the scalar fields a viewer and a probe both read:\n");
    AnalysisModel model = Cube(ElementShape::Hex8, 2, false);
    const std::vector<double> flat = SampleExact(model);
    std::vector<Vec3d> displacement(model.nodes.size());
    for (int node = 0; node < model.NodeCount(); ++node) {
        displacement[Idx(node)] = Vec3d{flat[Idx(node * 3)], flat[Idx(node * 3 + 1)],
                                        flat[Idx(node * 3 + 2)]};
    }
    StressField field;
    std::string error;
    CHECK(fem::RecoverStress(model, flat, Recovery::Extrapolated, &field, &error));
    fem::Strength steel;
    steel.tensile = 250e6;

    // Every field named must produce a value at every node, and the ones
    // that are defined in terms of each other must agree -- a probe that
    // disagrees with the contour it is drawn on is worse than either
    // being wrong.
    std::vector<double> mises, tresca, shear, sxx, principal_max;
    CHECK(fem::SampleField(model, displacement, field, fem::ScalarField::VonMises, steel, &mises,
                           &error));
    CHECK(fem::SampleField(model, displacement, field, fem::ScalarField::Tresca, steel, &tresca,
                           &error));
    CHECK(fem::SampleField(model, displacement, field, fem::ScalarField::MaxShear, steel, &shear,
                           &error));
    CHECK(fem::SampleField(model, displacement, field, fem::ScalarField::StressXX, steel, &sxx,
                           &error));
    CHECK(fem::SampleField(model, displacement, field, fem::ScalarField::MaxPrincipal, steel,
                           &principal_max, &error));
    for (int node = 0; node < model.NodeCount(); ++node) {
        CHECK(Near(shear[Idx(node)], 0.5 * tresca[Idx(node)], 1e-12));
        CHECK(Near(sxx[Idx(node)], field.nodal[Idx(node)].s[0], 1e-15));
        CHECK(principal_max[Idx(node)] >= field.nodal[Idx(node)].s[0] - 1e-9);
        CHECK(tresca[Idx(node)] >= mises[Idx(node)] - 1e-9);
    }
    int named = 0;
    for (const fem::ScalarField which : fem::AllScalarFields()) {
        std::vector<double> values;
        CHECK(fem::SampleField(model, displacement, field, which, steel, &values, &error));
        CHECK(values.size() == model.nodes.size());
        CHECK(std::string(fem::ScalarFieldName(which)) != "?");
        ++named;
    }
    std::printf("  %d fields, all sampled at all %d nodes; larger is worse for %d of them\n",
                named, model.NodeCount(), named - 1);
    CHECK(!fem::LargerIsWorse(fem::ScalarField::SafetyFactor));
}

void TestTheSolverUsesIt() {
    std::printf("the static solver's own result carries the recovered field:\n");
    // A BAR IN PURE TENSION, whose stress is known without solving
    // anything: force over area, everywhere, exactly. The recovery has to
    // give that at every node including the corners, where a naive
    // extrapolation is most likely to overshoot.
    AnalysisModel model = Cube(ElementShape::Hex8, 3, false);
    for (int node = 0; node < model.NodeCount(); ++node) {
        const Vec3d &p = model.nodes[Idx(node)];
        if (Near(p.x, 0.0, 1e-12)) {
            fem::Constraint constraint;
            constraint.node = node;
            constraint.fixed[0] = true;
            if (Near(p.y, 0.0, 1e-12)) constraint.fixed[1] = true;
            if (Near(p.z, 0.0, 1e-12)) constraint.fixed[2] = true;
            model.constraints.push_back(constraint);
        }
    }
    // A uniform traction on the far face, shared by tributary area: the
    // corner nodes get a quarter of what an interior node gets, and the
    // edges a half. Sharing it equally would put a stress concentration
    // at the corners that is not in the problem.
    const double traction = 1e-3;
    const int side = 4;
    for (int node = 0; node < model.NodeCount(); ++node) {
        const Vec3d &p = model.nodes[Idx(node)];
        if (!Near(p.x, 1.0, 1e-12)) continue;
        double share = 1.0;
        if (Near(p.y, 0.0, 1e-12) || Near(p.y, 1.0, 1e-12)) share *= 0.5;
        if (Near(p.z, 0.0, 1e-12) || Near(p.z, 1.0, 1e-12)) share *= 0.5;
        const double cell = 1.0 / ((side - 1) * (side - 1));
        model.loads.push_back(fem::NodalLoad{node, Vec3d{traction * cell * share, 0, 0}});
    }
    fem::StaticOptions options;
    fem::StaticResult result;
    CHECK(fem::SolveStatic(model, {}, options, &result));
    if (!result.ok) {
        std::printf("  %s\n", result.error.c_str());
        return;
    }
    double worst = 0.0;
    for (int node = 0; node < model.NodeCount(); ++node) {
        StressTensor want;
        want.s[0] = traction;
        worst = std::max(worst, Frobenius(result.nodal_stress[Idx(node)], want));
    }
    double worst_gap = 0.0;
    for (const double d : result.stress.discontinuity) worst_gap = std::max(worst_gap, d);
    std::printf("  uniform tension %.1e: worst nodal error %.2e, worst neighbour gap %.2e\n",
                traction, worst, worst_gap);
    CHECK(worst < traction * 1e-9);
    CHECK(worst_gap < traction * 1e-9);
    CHECK(result.stress.element_nodal.size() == Idx(model.ElementCount() * 8));
    CHECK(result.stress.shortfalls == 0);

    // And the naive recovery is available, gives the same answer on this
    // problem -- a constant field is exact for both -- which is what says
    // the two paths are solving the same problem before the harder tests
    // above distinguish them.
    fem::StaticOptions naive = options;
    naive.recovery = Recovery::AtNodes;
    fem::StaticResult other;
    CHECK(fem::SolveStatic(model, {}, naive, &other));
    double difference = 0.0;
    for (int node = 0; node < model.NodeCount(); ++node) {
        difference = std::max(difference, Frobenius(result.nodal_stress[Idx(node)],
                                                    other.nodal_stress[Idx(node)]));
    }
    std::printf("  against the naive recovery on the same constant field: %.2e apart\n",
                difference);
    CHECK(difference < traction * 1e-9);
}

}  // namespace

int main() {
    TestInvariants();
    TestSafetyFactors();
    TestConstantStressPatch();
    TestBasisStepsDown();
    TestPatchRecoveryBeatsBoth();
    TestPatchesAtTheBoundary();
    TestFieldSampling();
    TestTheSolverUsesIt();
    if (failures == 0) {
        std::printf("fem_result_test passed (%d checks)\n", checks);
        return 0;
    }
    std::printf("fem_result_test FAILED (%d of %d checks)\n", failures, checks);
    return 1;
}
