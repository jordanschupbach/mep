#ifndef MEP_FEM_THERMAL_H
#define MEP_FEM_THERMAL_H

#include "fem_elem.h"
#include "fem_mesh.h"
#include "fem_solve.h"
#include "fem_study.h"
#include "num_sparse.h"

#include <string>
#include <vector>

// Steady-state heat conduction (plans/CAD_FEM_PLAN.md Part I.1).
//
// WHY THIS IS THE FIRST PHYSICS AFTER STATICS. It is a scalar field, so
// one unknown per node instead of three, and the whole of the machinery
// Parts H.1 to H.5 built -- shape functions, quadrature, a sparsity
// pattern, constraint elimination, a direct and an iterative solver --
// applies unchanged with the vector parts taken out. Everything genuinely
// new here is in the boundary conditions, and that is where the
// interesting part is: two of the four are *nonlinear*.
//
//   * A fixed temperature is a prescribed value, eliminated exactly as a
//     prescribed displacement is.
//   * A flux is a load, integrated over the face the same way a pressure
//     is.
//   * CONVECTION IS NOT A LOAD. `h(T - T_ambient)` depends on the
//     unknown, so part of it belongs on the left-hand side: it adds
//     `h * integral(N N^T)` to the conductivity matrix and
//     `h * T_ambient * integral(N)` to the load. Treating the whole of it
//     as a load and iterating converges slowly or not at all, and treating
//     it as a fixed temperature is a different problem.
//   * RADIATION IS NONLINEAR IN THE UNKNOWN TO THE FOURTH POWER.
//     `emissivity * sigma * (T^4 - T_ambient^4)` is linearised about the
//     current temperature and iterated, which is Newton's method with the
//     derivative written out: the tangent term is
//     `4 * emissivity * sigma * T^3`. Using the secant coefficient
//     `emissivity * sigma * (T + T_a)(T^2 + T_a^2)` on the left instead
//     converges too, and linearly rather than quadratically; the tangent
//     is used here and the residual is reported so that a caller can see
//     it converge.
namespace fem {

struct ThermalMaterial {
    std::string name = "material";
    // Only the transient solve needs these two, and it needs both: what
    // governs how fast a body heats is the product, the volumetric heat
    // capacity in J/(m^3 K). They are kept apart because that is how a
    // datasheet gives them.
    double density = 7850.0;                                    // kg/m^3
    MaterialCurve specific_heat = MaterialCurve::Constant(460.0);  // J/(kg K)
    // W/(m K). A curve because conductivity varies with temperature for
    // every real material, and once it does the problem is nonlinear in
    // the same loop radiation already needs.
    MaterialCurve conductivity = MaterialCurve::Constant(45.0);
};

// One facet of the boundary carrying a condition. Built by hand or, more
// usually, by FaceConditions below from a mesh and a CAD face.
struct ThermalFacet {
    ElementShape shape = ElementShape::Tri3;
    std::vector<int> nodes;
    // W/m^2, positive into the body.
    double flux = 0.0;
    // W/(m^2 K); zero for no convection.
    double convection = 0.0;
    // Zero for no radiation. The Stefan-Boltzmann constant is applied
    // here, so this is the emissivity itself.
    double emissivity = 0.0;
    // K. Shared by convection and radiation, which is what a surface
    // exchanging with one environment means.
    double ambient = 293.15;
};

struct ThermalModel {
    std::vector<cad::Vec3d> nodes;
    // Shape and connectivity are exactly the structural ones; only the
    // number of unknowns per node differs. Sharing the type is the point:
    // a thermal-structural coupling has to be on the same mesh.
    std::vector<BoundElement> elements;
    std::vector<ThermalMaterial> materials;
    // W/m^3, one per element, or empty for none.
    std::vector<double> source;
    std::vector<ThermalFacet> facets;
    // Node index and value, in kelvin.
    std::vector<std::pair<int, double>> fixed;

    int NodeCount() const { return static_cast<int>(nodes.size()); }
    int ElementCount() const { return static_cast<int>(elements.size()); }
};

struct ThermalOptions {
    SolverKind solver = SolverKind::Direct;
    num::Ordering ordering = num::Ordering::ApproximateMinimumDegree;
    // Newton, for radiation and for temperature-dependent conductivity.
    int max_iterations = 40;
    // On the residual, relative to the largest heat flow in the problem.
    double tolerance = 1e-10;
    double initial_temperature = 293.15;
};

struct ThermalResult {
    bool ok = false;
    std::string error;
    std::vector<double> temperature;
    // Heat flux at each element's centre, W/m^2.
    std::vector<cad::Vec3d> flux;
    // Heat flowing in or out at each fixed node, W. Their sum plus every
    // applied source and surface load is zero, which is the conservation
    // check the solver makes on itself.
    std::vector<double> reaction;
    double lowest = 0.0;
    double highest = 0.0;
    int iterations = 0;
    double residual = 0.0;
    // Net power imbalance over the total power in the problem. At
    // round-off for a converged solve.
    double energy_residual = 0.0;
    // One field per entry of `sample_times`, in the same order.
    std::vector<std::vector<double>> samples;
};

bool SolveSteadyHeat(const ThermalModel &model, const ThermalOptions &options,
                     ThermalResult *out);

// Facets covering one CAD face of a mesh, ready for a condition to be set
// on them. The thermal counterpart of FaceTraction, and for the same
// reason: a condition belongs to a face of the model, not to a list of
// node numbers.
bool FaceConditions(const VolumeMesh &mesh, cad::EntityId face, std::vector<ThermalFacet> *out,
                    std::string *error);

// --- Transient heat (Part I.2) --------------------------------------------

// GENERALISED THETA, AND THE CHOICE IS NOT A DETAIL. The step is
//
//     C (T' - T)/dt = theta * S(T') + (1 - theta) * S(T)
//
// where S is the steady residual -- everything conducted, supplied and
// exchanged -- and C the heat capacity. Three values matter:
//
//   theta = 1    backward Euler. First order in the step, and
//                unconditionally stable *and monotone*: it cannot
//                overshoot, whatever the step. The right default, because
//                a thermal transient's early steps are always too large
//                for the gradients in them.
//   theta = 0.5  Crank-Nicolson. Second order, and unconditionally stable
//                without being monotone -- it oscillates around a sharp
//                front instead of diverging from it, which looks like a
//                mesh problem and is not. Worth its extra order when the
//                solution is smooth in time.
//   theta = 0    forward Euler. Explicit, no solve per step, and only
//                conditionally stable: the limit is proportional to the
//                square of the element size, so refining a mesh by ten
//                costs a hundred times the steps. Offered because it is
//                the right answer for a very short transient on a coarse
//                mesh, and refused by the stability check when it is not.
struct TransientOptions {
    double theta = 1.0;
    double end_time = 1.0;
    // The first step. Zero asks for one hundredth of the end time, which
    // is a starting guess and nothing more when adaptive is on.
    double step = 0.0;
    // ADAPTIVE BY STEP DOUBLING. One step of dt and two of dt/2 differ by
    // an amount proportional to the local error, which is the cheapest
    // honest estimate there is: it costs three solves per accepted step
    // and needs nothing but the integrator it is estimating.
    bool adaptive = false;
    // Relative to the temperature range the solution has covered so far,
    // so that the same tolerance means the same thing whether the model is
    // in kelvin or in degrees and whatever it is being heated by.
    double tolerance = 1e-4;
    double min_step = 0.0;
    double max_step = 0.0;
    int max_steps = 200000;
    SolverKind solver = SolverKind::Direct;
    num::Ordering ordering = num::Ordering::ApproximateMinimumDegree;
    int newton_iterations = 30;
    double newton_tolerance = 1e-10;
    // TIMES TO RECORD THE WHOLE FIELD AT, for animation (Part J.3) and
    // for time histories at a probe (J.4). Ascending, within [0, end
    // time]. Asking for them by time rather than taking whatever the
    // steps happen to be is the whole point: the steps are adaptive, so a
    // history recorded at them is unevenly spaced and changes shape when
    // the tolerance does, which is no way to drive a fixed frame rate.
    // The field is linearly interpolated within the step that brackets
    // each time, which matches the integrator's own order.
    std::vector<double> sample_times;
};

struct TransientResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    // At the end time.
    std::vector<double> temperature;
    double time = 0.0;
    int steps = 0;
    int rejected = 0;
    double smallest_step = 0.0;
    double largest_step = 0.0;
    double lowest = 0.0;
    double highest = 0.0;
    // Energy in minus energy stored, over the energy that moved. A
    // statement about the whole history rather than about one step.
    double energy_residual = 0.0;
    // One field per entry of `sample_times`, in the same order.
    std::vector<std::vector<double>> samples;
};

bool SolveTransientHeat(const ThermalModel &model, const std::vector<double> &initial,
                        const TransientOptions &options, TransientResult *out);

// The explicit method's stability limit: the largest step forward Euler
// can take on this model before it diverges. Exposed because a caller
// choosing theta = 0 needs to know it, and because it is the number that
// explains why explicit time integration is not the easy option it looks
// like.
double ExplicitStabilityLimit(const ThermalModel &model, double temperature);

}  // namespace fem

#endif
