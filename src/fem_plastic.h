#ifndef MEP_FEM_PLASTIC_H
#define MEP_FEM_PLASTIC_H

#include "fem_study.h"

#include <string>
#include <vector>

// Material nonlinearity (plans/CAD_FEM_PLAN.md Part I.7).
//
// J2 plasticity: a metal yields when the *deviatoric* stress reaches a
// limit, and is unaffected by pressure -- squeeze steel from every side
// and it does not yield, which is why a von Mises stress ignores the
// hydrostatic part.
//
// RADIAL RETURN, AND WHY IT IS EXACT RATHER THAN A SCHEME. Take an
// elastic trial step, and if it lands outside the yield surface, pull it
// back. For J2 with isotropic elasticity the pull-back direction is
// exactly radial in deviatoric space -- the trial deviator and the final
// deviator are parallel -- so the correction is one scalar and the
// "iteration" is a single equation. That is a property of the yield
// surface being a cylinder about the hydrostatic axis, not an
// approximation, and it is why J2 is the plasticity model everybody
// implements first.
//
// TWO HARDENING RULES, AND THEY ARE NOT INTERCHANGEABLE. Isotropic
// hardening grows the yield surface, so a bar pulled past yield and then
// pushed needs the *raised* stress to yield again. Kinematic hardening
// moves the surface instead, so the elastic range stays 2*sigma_y wide and
// the bar yields in compression *earlier* than it first did in tension --
// the Bauschinger effect, which is what real metals do and what decides
// the life of anything cycled.
//
// AND THE CONSISTENT ALGORITHMIC TANGENT. The obvious tangent is the
// continuum elastoplastic modulus, which is the derivative of the
// *constitutive law*. What Newton needs is the derivative of the
// *algorithm* -- of the stress the return map actually returns, given the
// strain it was actually handed. They differ by a term in the step size,
// and using the wrong one costs Newton its quadratic convergence: it
// still converges, linearly, which looks like a hard problem rather than
// a wrong derivative. This is the classic way plasticity is got wrong and
// the tests measure the difference rather than asserting it.
namespace fem {

struct J2Material {
    std::string name = "steel";
    double youngs_modulus = 210e9;
    double poissons_ratio = 0.3;
    // The stress at which it first yields.
    double yield_stress = 250e6;
    // Linear hardening, split between the two rules: the surface grows by
    // `isotropic_modulus` per unit plastic strain and moves by
    // `kinematic_modulus`. Setting one to zero gives the pure other.
    double isotropic_modulus = 2e9;
    double kinematic_modulus = 0.0;

    double ShearModulus() const;
    double BulkModulus() const;
};

// What a point remembers between steps. A plastic material has no
// stress-strain curve, only a stress-strain *history*, and this is it.
struct PlasticState {
    // Voigt, with engineering shear -- the same convention the strain
    // arrives in.
    double plastic_strain[6] = {0, 0, 0, 0, 0, 0};
    // Where the yield surface has moved to, for kinematic hardening.
    double back_stress[6] = {0, 0, 0, 0, 0, 0};
    // The accumulated equivalent plastic strain, which only ever grows and
    // is what isotropic hardening is a function of.
    double accumulated = 0.0;
};

// The return map: given a total strain, update the state and give back the
// stress and the tangent.
//
// `consistent` chooses which tangent: the algorithmic one, or the
// continuum elastoplastic modulus. The option exists so that the cost of
// getting it wrong can be measured rather than described.
void RadialReturn(const J2Material &material, const double strain[6], PlasticState *state,
                  double out_stress[6], double out_tangent[6][6], bool consistent = true);

// The equivalent (von Mises) stress of a Voigt tensor.
double EquivalentStress(const double stress[6]);

}  // namespace fem

#endif
