#include "fem_plastic.h"

#include <algorithm>
#include <cmath>

namespace fem {
namespace {

// Deviator of a Voigt tensor. The shear components are already
// deviatoric, so only the normal ones lose the mean.
void Deviator(const double stress[6], double out[6]) {
    const double mean = (stress[0] + stress[1] + stress[2]) / 3.0;
    out[0] = stress[0] - mean;
    out[1] = stress[1] - mean;
    out[2] = stress[2] - mean;
    out[3] = stress[3];
    out[4] = stress[4];
    out[5] = stress[5];
}

// THE INNER PRODUCT COUNTS THE SHEARS TWICE, and forgetting that is the
// commonest arithmetic slip in a plasticity routine. A symmetric tensor
// has six independent numbers and nine components; in Voigt form each
// shear stands for two, so the contraction that matches the tensor one is
// the normals plus twice the shears.
double DoubleContraction(const double a[6], const double b[6]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] +
           2.0 * (a[3] * b[3] + a[4] * b[4] + a[5] * b[5]);
}

void ElasticTangent(double lambda, double shear, double out[6][6]) {
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) out[i][j] = 0.0;
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i][j] = lambda;
        out[i][i] = lambda + 2.0 * shear;
    }
    for (int i = 3; i < 6; ++i) out[i][i] = shear;
}

// THE RETURN MAP ITSELF, AS A PURE FUNCTION. It takes a state and gives
// back a state, which is what lets the tangent be obtained by
// differencing it: a tangent computed from the same function the solver
// calls cannot disagree with it.
//
// Returns whether the step was plastic, and leaves the multiplier and the
// return direction where a caller that wants the continuum modulus can
// find them.
bool ReturnMap(const J2Material &material, const double strain[6], PlasticState *state,
               double out_stress[6], double *out_gamma, double out_direction[6]) {
    const double shear = material.ShearModulus();
    const double lambda = material.BulkModulus() - 2.0 * shear / 3.0;

    // The elastic trial: what the strain would do if nothing yielded.
    double elastic[6];
    for (int i = 0; i < 6; ++i) elastic[i] = strain[i] - state->plastic_strain[i];
    const double volumetric = elastic[0] + elastic[1] + elastic[2];
    double trial[6];
    for (int i = 0; i < 3; ++i) trial[i] = lambda * volumetric + 2.0 * shear * elastic[i];
    // The shears arrive as engineering strain, which is twice the tensor
    // shear, so they take the modulus once rather than twice. Getting this
    // backwards doubles every shear stress and is invisible uniaxially.
    for (int i = 3; i < 6; ++i) trial[i] = shear * elastic[i];

    double relative[6];
    for (int i = 0; i < 6; ++i) relative[i] = trial[i] - state->back_stress[i];
    double deviator[6];
    Deviator(relative, deviator);
    const double norm = std::sqrt(DoubleContraction(deviator, deviator));
    const double yield = material.yield_stress + material.isotropic_modulus * state->accumulated;
    const double excess = norm - std::sqrt(2.0 / 3.0) * yield;
    *out_gamma = 0.0;
    for (int i = 0; i < 6; ++i) out_direction[i] = 0.0;

    if (excess <= 0.0 || !(norm > 0.0)) {
        for (int i = 0; i < 6; ++i) out_stress[i] = trial[i];
        return false;
    }
    // Linear hardening makes the multiplier one division rather than an
    // iteration: the yield condition after the return is linear in it.
    const double hardening = material.isotropic_modulus + material.kinematic_modulus;
    const double gamma = excess / (2.0 * shear + (2.0 / 3.0) * hardening);
    for (int i = 0; i < 6; ++i) out_direction[i] = deviator[i] / norm;
    *out_gamma = gamma;

    for (int i = 0; i < 6; ++i) {
        out_stress[i] = trial[i] - 2.0 * shear * gamma * out_direction[i];
    }
    for (int i = 0; i < 3; ++i) {
        state->plastic_strain[i] += gamma * out_direction[i];
        state->back_stress[i] +=
            (2.0 / 3.0) * material.kinematic_modulus * gamma * out_direction[i];
    }
    for (int i = 3; i < 6; ++i) {
        // Back to engineering shear for the stored plastic strain, which
        // is the convention the strain arrives in.
        state->plastic_strain[i] += 2.0 * gamma * out_direction[i];
        state->back_stress[i] +=
            (2.0 / 3.0) * material.kinematic_modulus * gamma * out_direction[i];
    }
    state->accumulated += std::sqrt(2.0 / 3.0) * gamma;
    return true;
}

}  // namespace

double J2Material::ShearModulus() const {
    return youngs_modulus / (2.0 * (1.0 + poissons_ratio));
}

double J2Material::BulkModulus() const {
    return youngs_modulus / (3.0 * (1.0 - 2.0 * poissons_ratio));
}

double EquivalentStress(const double stress[6]) {
    double deviator[6];
    Deviator(stress, deviator);
    return std::sqrt(1.5 * DoubleContraction(deviator, deviator));
}

void RadialReturn(const J2Material &material, const double strain[6], PlasticState *state,
                  double out_stress[6], double out_tangent[6][6], bool consistent) {
    const PlasticState incoming = *state;
    const double shear = material.ShearModulus();
    const double lambda = material.BulkModulus() - 2.0 * shear / 3.0;
    double gamma = 0.0;
    double direction[6];
    const bool plastic = ReturnMap(material, strain, state, out_stress, &gamma, direction);

    if (!plastic) {
        // Elastic, including unloading from a plastic state -- which is
        // what makes the elastic range after yield the right width rather
        // than zero.
        ElasticTangent(lambda, shear, out_tangent);
        return;
    }

    if (!consistent) {
        // The CONTINUUM elastoplastic modulus: the derivative of the
        // constitutive law rather than of the algorithm. Offered so that
        // the difference between the two can be measured rather than
        // described.
        ElasticTangent(lambda, shear, out_tangent);
        const double hardening = material.isotropic_modulus + material.kinematic_modulus;
        const double plastic_term =
            4.0 * shear * shear / (2.0 * shear + (2.0 / 3.0) * hardening);
        for (int i = 0; i < 6; ++i) {
            for (int j = 0; j < 6; ++j) {
                const double row = i < 3 ? 1.0 : 0.5;
                out_tangent[i][j] -= row * plastic_term * direction[i] * direction[j];
            }
        }
        return;
    }

    // THE CONSISTENT TANGENT, DIFFERENCED FROM THE MAP, AND THAT IS A
    // CHOICE RATHER THAN A SHORTCUT. The consistent algorithmic tangent
    // is by definition the derivative of this function -- of the stress
    // the return map returns, given the strain it was handed and the
    // state it started from. A closed form exists for linear hardening
    // and is a page of Voigt bookkeeping in which the engineering-shear
    // factors appear three times and must be right three times. It was
    // written here first and was wrong twice over: the algorithmic
    // correction evaluated to nothing at all, and what remained disagreed
    // with a finite difference of the map by twenty percent. Both were
    // invisible in every test of the *stress*, which was exact throughout
    // -- a uniaxial curve, a hardening slope and a Bauschinger reversal
    // all pass with a wrong tangent, because the tangent does not affect
    // the answer, only how fast Newton finds it.
    //
    // Differencing the map cannot disagree with the map. It costs twelve
    // extra evaluations of a routine that is a few dozen operations, and
    // the closed form is an optimisation that is recorded as one.
    double size = 0.0;
    for (int i = 0; i < 6; ++i) size = std::max(size, std::fabs(strain[i]));
    const double step = std::max(size, 1e-12) * 1e-7;
    for (int column = 0; column < 6; ++column) {
        double up_strain[6];
        double down_strain[6];
        for (int i = 0; i < 6; ++i) {
            up_strain[i] = strain[i];
            down_strain[i] = strain[i];
        }
        up_strain[column] += step;
        down_strain[column] -= step;
        PlasticState up = incoming;
        PlasticState down = incoming;
        double up_stress[6];
        double down_stress[6];
        double ignored_gamma = 0.0;
        double ignored_direction[6];
        ReturnMap(material, up_strain, &up, up_stress, &ignored_gamma, ignored_direction);
        ReturnMap(material, down_strain, &down, down_stress, &ignored_gamma, ignored_direction);
        for (int row = 0; row < 6; ++row) {
            out_tangent[row][column] = (up_stress[row] - down_stress[row]) / (2.0 * step);
        }
    }
}

}  // namespace fem
