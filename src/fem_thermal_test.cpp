// Part I.1: steady-state heat conduction.
//
// One test per boundary condition, each against an answer written down
// rather than computed, because the four conditions fail in different
// ways: a flux that is integrated wrongly gives the right shape and the
// wrong level, a convection term put on the wrong side of the equation
// gives a plausible field that does not satisfy its own boundary
// condition, and radiation linearised wrongly converges to the right
// answer slowly enough that it looks like it works.

#include "fem_thermal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
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
#define CHECK_MESSAGE(x, message) Check((x), (std::string(#x) + ": " + (message)).c_str(), __LINE__)

using cad::Vec3d;
using fem::ElementShape;
using fem::ThermalFacet;
using fem::ThermalModel;
using fem::ThermalOptions;
using fem::ThermalResult;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

constexpr double kPi = 3.14159265358979323846;
constexpr double kStefanBoltzmann = 5.670374419e-8;

// A slab of hexahedra: length along x, unit cross-section, so that every
// case below is one-dimensional and has a closed form.
struct Slab {
    ThermalModel model;
    std::vector<ThermalFacet> at_x_min;
    std::vector<ThermalFacet> at_x_max;
    int divisions = 0;
    double length = 0.0;
};

Slab MakeSlab(double length, double conductivity, int nx, int ny) {
    Slab out;
    out.divisions = nx;
    out.length = length;
    fem::ThermalMaterial material;
    material.conductivity = fem::MaterialCurve::Constant(conductivity);
    out.model.materials.push_back(material);
    const int side = ny + 1;
    for (int k = 0; k < side; ++k) {
        for (int j = 0; j < side; ++j) {
            for (int i = 0; i <= nx; ++i) {
                out.model.nodes.push_back(Vec3d{length * i / nx, static_cast<double>(j) / ny,
                                                static_cast<double>(k) / ny});
            }
        }
    }
    auto at = [&](int i, int j, int k) { return (k * side + j) * (nx + 1) + i; };
    for (int k = 0; k < ny; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                fem::BoundElement element;
                element.shape = ElementShape::Hex8;
                element.nodes = {at(i, j, k),         at(i + 1, j, k),
                                 at(i + 1, j + 1, k), at(i, j + 1, k),
                                 at(i, j, k + 1),     at(i + 1, j, k + 1),
                                 at(i + 1, j + 1, k + 1), at(i, j + 1, k + 1)};
                out.model.elements.push_back(element);
            }
        }
    }
    // The two ends, as quadrilateral facets.
    for (int k = 0; k < ny; ++k) {
        for (int j = 0; j < ny; ++j) {
            ThermalFacet low;
            low.shape = ElementShape::Quad4;
            low.nodes = {at(0, j, k), at(0, j + 1, k), at(0, j + 1, k + 1), at(0, j, k + 1)};
            out.at_x_min.push_back(low);
            ThermalFacet high;
            high.shape = ElementShape::Quad4;
            high.nodes = {at(nx, j, k), at(nx, j + 1, k), at(nx, j + 1, k + 1), at(nx, j, k + 1)};
            out.at_x_max.push_back(high);
        }
    }
    return out;
}

void FixEnd(Slab *slab, bool max_side, double value) {
    for (int node = 0; node < slab->model.NodeCount(); ++node) {
        const double x = slab->model.nodes[Idx(node)].x;
        const bool there = max_side ? std::fabs(x - slab->length) < 1e-12 : std::fabs(x) < 1e-12;
        if (there) slab->model.fixed.push_back({node, value});
    }
}

// --- Conduction between two fixed temperatures -----------------------------

void TestFixedTemperatures() {
    std::printf("conduction between two fixed temperatures:\n");
    const double length = 0.2;
    const double k = 45.0;
    Slab slab = MakeSlab(length, k, 8, 2);
    FixEnd(&slab, false, 400.0);
    FixEnd(&slab, true, 300.0);

    ThermalResult result;
    CHECK_MESSAGE(fem::SolveSteadyHeat(slab.model, {}, &result), result.error);
    if (!result.ok) return;

    // T(x) = 400 - 500x, exactly: a linear field is in the element's own
    // space, so a correct element reproduces it to round-off and not
    // approximately. This is the patch test again, in one unknown.
    double worst = 0.0;
    for (int node = 0; node < slab.model.NodeCount(); ++node) {
        const double exact = 400.0 - 100.0 * slab.model.nodes[Idx(node)].x / length;
        worst = std::max(worst, std::fabs(result.temperature[Idx(node)] - exact));
    }
    // Fourier: q = -k dT/dx = k * 100 / L, and the total through the unit
    // area is the same.
    const double exact_flux = k * 100.0 / length;
    double flux = 0.0;
    for (const Vec3d &q : result.flux) flux += q.x;
    flux /= static_cast<double>(result.flux.size());
    std::printf("  worst node error %.2e K, flux %.4f W/m2 (exactly %.4f), energy residual %.2e\n",
                worst, flux, exact_flux, result.energy_residual);
    CHECK(worst < 1e-9);
    CHECK(std::fabs(flux - exact_flux) < exact_flux * 1e-9);
    CHECK(result.energy_residual < 1e-10);
    // What went in came out: the reaction at one end balances the other.
    double supplied = 0.0;
    double removed = 0.0;
    for (int node = 0; node < slab.model.NodeCount(); ++node) {
        if (std::fabs(slab.model.nodes[Idx(node)].x) < 1e-12) supplied += result.reaction[Idx(node)];
        if (std::fabs(slab.model.nodes[Idx(node)].x - length) < 1e-12) {
            removed += result.reaction[Idx(node)];
        }
    }
    std::printf("  %.4f W in at the hot end, %.4f W out at the cold end\n", supplied, removed);
    CHECK(supplied > 0.0);
    CHECK(std::fabs(supplied + removed) < std::fabs(supplied) * 1e-9);
    CHECK(std::fabs(supplied - exact_flux) < exact_flux * 1e-9);
}

// --- Flux in, convection out ------------------------------------------------

void TestFluxAndConvection() {
    std::printf("a flux in one end and convection out the other:\n");
    const double length = 0.2;
    const double k = 45.0;
    const double flux = 5000.0;   // W/m^2
    const double h = 25.0;        // W/(m^2 K)
    const double ambient = 300.0;
    Slab slab = MakeSlab(length, k, 8, 2);
    for (ThermalFacet &facet : slab.at_x_min) {
        facet.flux = flux;
        slab.model.facets.push_back(facet);
    }
    for (ThermalFacet &facet : slab.at_x_max) {
        facet.convection = h;
        facet.ambient = ambient;
        slab.model.facets.push_back(facet);
    }

    ThermalResult result;
    CHECK_MESSAGE(fem::SolveSteadyHeat(slab.model, {}, &result), result.error);
    if (!result.ok) return;

    // NOTHING IS FIXED HERE, and that is the point of the case: the
    // temperature level is set by the balance between the flux in and the
    // convection out, not by a prescribed value. In steady state all the
    // heat that enters leaves, so q = h (T_L - T_ambient), giving
    // T_L = T_ambient + q/h and T(x) = T_L + q (L - x)/k.
    const double at_far = ambient + flux / h;
    double worst = 0.0;
    for (int node = 0; node < slab.model.NodeCount(); ++node) {
        const double x = slab.model.nodes[Idx(node)].x;
        const double exact = at_far + flux * (length - x) / k;
        worst = std::max(worst, std::fabs(result.temperature[Idx(node)] - exact));
    }
    std::printf("  T at the hot face %.4f K, at the cooled face %.4f K (exactly %.4f and %.4f)\n",
                result.highest, result.lowest, at_far + flux * length / k, at_far);
    std::printf("  worst node error %.2e K, energy residual %.2e, %d sweeps\n", worst,
                result.energy_residual, result.iterations);
    CHECK(worst < 1e-8);
    CHECK(result.energy_residual < 1e-10);
    // Linear, so Newton is exact on the first correction and the second
    // sweep only confirms it.
    CHECK(result.iterations == 1);
}

// --- Radiation --------------------------------------------------------------

void TestRadiation() {
    std::printf("a flux in one end and radiation out the other:\n");
    const double length = 0.2;
    const double k = 45.0;
    const double flux = 5000.0;
    const double emissivity = 0.8;
    const double ambient = 300.0;
    Slab slab = MakeSlab(length, k, 8, 2);
    for (ThermalFacet &facet : slab.at_x_min) {
        facet.flux = flux;
        slab.model.facets.push_back(facet);
    }
    for (ThermalFacet &facet : slab.at_x_max) {
        facet.emissivity = emissivity;
        facet.ambient = ambient;
        slab.model.facets.push_back(facet);
    }

    ThermalResult result;
    CHECK_MESSAGE(fem::SolveSteadyHeat(slab.model, {}, &result), result.error);
    if (!result.ok) return;

    // The radiating face's temperature solves
    // q = e sigma (T^4 - T_a^4), which is a quartic with one physical
    // root. Solved here by bisection, which shares no code with the
    // solver's Newton iteration -- so agreeing is a statement about both.
    const double sigma = emissivity * kStefanBoltzmann;
    double low = ambient;
    double high = 3000.0;
    for (int step = 0; step < 200; ++step) {
        const double middle = 0.5 * (low + high);
        const double residual = sigma * (std::pow(middle, 4.0) - std::pow(ambient, 4.0)) - flux;
        if (residual > 0.0) {
            high = middle;
        } else {
            low = middle;
        }
    }
    const double at_far = 0.5 * (low + high);
    double worst = 0.0;
    for (int node = 0; node < slab.model.NodeCount(); ++node) {
        const double x = slab.model.nodes[Idx(node)].x;
        const double exact = at_far + flux * (length - x) / k;
        worst = std::max(worst, std::fabs(result.temperature[Idx(node)] - exact));
    }
    std::printf("  radiating face %.4f K (bisection of the quartic gives %.4f), worst error %.2e K\n",
                result.lowest, at_far, worst);
    std::printf("  %d Newton sweeps to a residual of %.2e, energy residual %.2e\n",
                result.iterations, result.residual, result.energy_residual);
    CHECK(worst < 1e-6);
    CHECK(result.energy_residual < 1e-9);
    // QUADRATICALLY, WHICH IS WHAT THE CONSISTENT TANGENT BUYS. The secant
    // coefficient converges too, linearly, and would take tens of sweeps
    // here rather than a handful.
    CHECK(result.iterations <= 8);
}

// --- A manufactured solution, and the rate ---------------------------------

void TestManufacturedSolution() {
    std::printf("a manufactured temperature field, and the rate it converges at:\n");
    // T = sin(pi x) sin(pi y) sin(pi z) satisfies -k laplacian(T) = f with
    // f = 3 pi^2 k T, and vanishes on the unit cube's faces, so the
    // boundary condition is a fixed zero and the source is the whole of
    // the problem.
    const double k = 45.0;
    auto exact = [](const Vec3d &p) {
        return std::sin(kPi * p.x) * std::sin(kPi * p.y) * std::sin(kPi * p.z);
    };
    std::vector<double> sizes;
    std::vector<double> errors;
    for (const int m : {3, 6, 12, 24}) {
        ThermalModel model;
        fem::ThermalMaterial material;
        material.conductivity = fem::MaterialCurve::Constant(k);
        model.materials.push_back(material);
        const int side = m + 1;
        for (int c = 0; c < side; ++c) {
            for (int b = 0; b < side; ++b) {
                for (int a = 0; a <= m; ++a) {
                    model.nodes.push_back(Vec3d{static_cast<double>(a) / m,
                                                static_cast<double>(b) / m,
                                                static_cast<double>(c) / m});
                }
            }
        }
        auto at = [&](int i, int j, int l) { return (l * side + j) * side + i; };
        for (int c = 0; c < m; ++c) {
            for (int b = 0; b < m; ++b) {
                for (int a = 0; a < m; ++a) {
                    fem::BoundElement element;
                    element.shape = ElementShape::Hex8;
                    element.nodes = {at(a, b, c),         at(a + 1, b, c),
                                     at(a + 1, b + 1, c), at(a, b + 1, c),
                                     at(a, b, c + 1),     at(a + 1, b, c + 1),
                                     at(a + 1, b + 1, c + 1), at(a, b + 1, c + 1)};
                    model.elements.push_back(element);
                }
            }
        }
        // The source, element by element at its centre. A constant per
        // element rather than a field, which is what ThermalModel offers
        // and is enough: it is second-order accurate, the same as the
        // element.
        for (const fem::BoundElement &element : model.elements) {
            Vec3d centre{};
            for (const int node : element.nodes) centre = centre + model.nodes[Idx(node)];
            centre = centre * 0.125;
            model.source.push_back(3.0 * kPi * kPi * k * exact(centre));
        }
        for (int node = 0; node < model.NodeCount(); ++node) {
            const Vec3d &p = model.nodes[Idx(node)];
            if (p.x == 0.0 || p.x == 1.0 || p.y == 0.0 || p.y == 1.0 || p.z == 0.0 || p.z == 1.0) {
                model.fixed.push_back({node, 0.0});
            }
        }
        ThermalResult result;
        ThermalOptions options;
        options.initial_temperature = 0.0;
        CHECK_MESSAGE(fem::SolveSteadyHeat(model, options, &result), result.error);
        if (!result.ok) return;
        double squared = 0.0;
        for (int node = 0; node < model.NodeCount(); ++node) {
            const double difference = result.temperature[Idx(node)] - exact(model.nodes[Idx(node)]);
            squared += difference * difference;
        }
        sizes.push_back(1.0 / m);
        errors.push_back(std::sqrt(squared / model.NodeCount()));
    }
    std::vector<double> orders;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        if (i == 0) {
            std::printf("      h = %.4f   error %.4e\n", sizes[i], errors[i]);
            continue;
        }
        const double order =
            std::log(errors[i - 1] / errors[i]) / std::log(sizes[i - 1] / sizes[i]);
        orders.push_back(order);
        std::printf("      h = %.4f   error %.4e   order %.3f\n", sizes[i], errors[i], order);
    }
    CHECK(orders.back() > 1.7);
    CHECK(orders.back() < 2.6);
    CHECK(errors.back() < errors.front() * 0.2);
}

// --- Temperature-dependent conductivity ------------------------------------

void TestTemperatureDependent() {
    std::printf("conductivity that varies with temperature:\n");
    // A slab between two fixed temperatures with k(T) linear in T. The
    // exact answer is not linear in x any more, and the heat flow is the
    // one thing still writable: in steady state q is constant, so
    // q L = integral of k(T) dT between the two ends, which for a linear
    // k is the mean conductivity times the difference.
    const double length = 0.2;
    const double hot = 800.0;
    const double cold = 300.0;
    fem::MaterialCurve varying;
    varying.points = {{300.0, 60.0}, {800.0, 20.0}};
    Slab slab = MakeSlab(length, 1.0, 16, 1);
    slab.model.materials[0].conductivity = varying;
    FixEnd(&slab, false, hot);
    FixEnd(&slab, true, cold);

    ThermalResult result;
    CHECK_MESSAGE(fem::SolveSteadyHeat(slab.model, {}, &result), result.error);
    if (!result.ok) return;

    const double mean = 0.5 * (varying.At(hot) + varying.At(cold));
    const double exact_flow = mean * (hot - cold) / length;
    double supplied = 0.0;
    for (int node = 0; node < slab.model.NodeCount(); ++node) {
        if (std::fabs(slab.model.nodes[Idx(node)].x) < 1e-12) supplied += result.reaction[Idx(node)];
    }
    std::printf("  heat flow %.4f W (the mean-conductivity integral gives %.4f), %d sweeps\n",
                supplied, exact_flow, result.iterations);
    CHECK(std::fabs(supplied - exact_flow) < exact_flow * 0.01);
    CHECK(result.energy_residual < 1e-9);
    // NOT LINEAR IN X ANY MORE, which is the check that the varying
    // conductivity reached the element rather than being averaged away.
    // The direction is worth getting right rather than guessing at: the
    // conductivity *falls* with temperature here, so it is the hot end
    // that conducts badly, the gradient is steep there and shallow at the
    // cold end, and the temperature drops quickly and then levels off.
    // The midpoint is therefore *below* the straight line, not above it.
    double middle = 0.0;
    int counted = 0;
    for (int node = 0; node < slab.model.NodeCount(); ++node) {
        if (std::fabs(slab.model.nodes[Idx(node)].x - length * 0.5) > 1e-12) continue;
        middle += result.temperature[Idx(node)];
        ++counted;
    }
    middle /= std::max(1, counted);
    std::printf("  midpoint %.4f K, against %.4f K if conductivity were constant\n", middle,
                0.5 * (hot + cold));
    CHECK(middle < 0.5 * (hot + cold) - 1.0);
    // MANY SWEEPS, AND THAT IS THE POINT RATHER THAN AN OVERSIGHT. The
    // conductivity is iterated as a fixed point, because its consistent
    // tangent is not symmetric and the direct solver factors symmetric
    // matrices; the note in fem_thermal.cpp says what fixing it needs.
    // Linear convergence on a conductivity that drops by two thirds across
    // the range costs tens of sweeps, and the number is asserted so that
    // making it quadratic later shows up here as the improvement it is.
    CHECK(result.iterations > 10);
    CHECK(result.iterations < 40);
}

// --- Transient (Part I.2) --------------------------------------------------

// A slab starting uniform at T0, both faces dropped to T1 at t = 0. The
// exact answer is a Fourier series in space and decaying exponentials in
// time, and it is worth writing out rather than approximating because it
// is the only thing here that knows what the right transient is.
//
//   T(x,t) = T1 + (T0 - T1) * sum over odd n of
//            (4/(n pi)) sin(n pi x/L) exp(-alpha (n pi/L)^2 t)
double SlabExact(double x, double t, double length, double alpha, double start, double face) {
    double sum = 0.0;
    for (int n = 1; n < 400; n += 2) {
        const double lambda = n * kPi / length;
        sum += (4.0 / (n * kPi)) * std::sin(lambda * x) * std::exp(-alpha * lambda * lambda * t);
    }
    return face + (start - face) * sum;
}

void TestTransientAgainstFourier() {
    std::printf("a slab quenched at both faces, against its Fourier series:\n");
    const double length = 0.1;
    const double k = 45.0;
    const double density = 7850.0;
    const double specific_heat = 460.0;
    const double alpha = k / (density * specific_heat);
    const double start = 500.0;
    const double face = 300.0;
    // Long enough that several modes have decayed and short enough that
    // the first has not: the interesting part of the transient.
    const double end = 0.25 * length * length / alpha;

    Slab slab = MakeSlab(length, k, 40, 1);
    slab.model.materials[0].density = density;
    slab.model.materials[0].specific_heat = fem::MaterialCurve::Constant(specific_heat);
    FixEnd(&slab, false, face);
    FixEnd(&slab, true, face);
    std::vector<double> initial(Idx(slab.model.NodeCount()), start);

    fem::TransientOptions options;
    options.theta = 0.5;
    options.end_time = end;
    options.step = end / 200.0;
    fem::TransientResult result;
    CHECK_MESSAGE(fem::SolveTransientHeat(slab.model, initial, options, &result), result.error);
    if (!result.ok) return;

    double worst = 0.0;
    for (int node = 0; node < slab.model.NodeCount(); ++node) {
        const double x = slab.model.nodes[Idx(node)].x;
        if (x < 1e-12 || x > length - 1e-12) continue;
        const double exact = SlabExact(x, end, length, alpha, start, face);
        worst = std::max(worst, std::fabs(result.temperature[Idx(node)] - exact));
    }
    std::printf("  after %.4f s: middle %.4f K, series says %.4f K, worst node error %.4f K\n", end,
                result.highest, SlabExact(length * 0.5, end, length, alpha, start, face), worst);
    std::printf("  %d steps, energy residual %.2e\n", result.steps, result.energy_residual);
    // A fraction of a kelvin out of a two hundred kelvin transient.
    CHECK(worst < (start - face) * 0.005);
    CHECK(result.energy_residual < 1e-6);
    // It really cooled: the middle is between the two, not at either.
    CHECK(result.highest < start);
    CHECK(result.highest > face);
}

void TestTimeOrder() {
    std::printf("the order of the time integration:\n");
    // MEASURED AGAINST A REFERENCE ON THE SAME MESH, not against the
    // analytic answer. The spatial discretisation has an error of its own
    // and it does not shrink when the step does, so comparing with the
    // exact solution measures the sum of the two and reports an order of
    // zero as soon as the spatial part dominates. A reference taken with a
    // very small step on the same mesh has the same spatial error, which
    // therefore cancels.
    const double length = 0.1;
    const double k = 45.0;
    const double alpha = k / (7850.0 * 460.0);
    const double end = 0.05 * length * length / alpha;
    for (const double theta : {1.0, 0.5}) {
        auto run = [&](double step) {
            Slab slab = MakeSlab(length, k, 12, 1);
            slab.model.materials[0].density = 7850.0;
            slab.model.materials[0].specific_heat = fem::MaterialCurve::Constant(460.0);
            FixEnd(&slab, false, 300.0);
            FixEnd(&slab, true, 300.0);
            std::vector<double> initial(Idx(slab.model.NodeCount()), 500.0);
            fem::TransientOptions options;
            options.theta = theta;
            options.end_time = end;
            options.step = step;
            fem::TransientResult result;
            if (!fem::SolveTransientHeat(slab.model, initial, options, &result)) {
                std::printf("    %s\n", result.error.c_str());
                ++failures;
            }
            return result.temperature;
        };
        const std::vector<double> reference = run(end / 2048.0);
        std::vector<double> errors;
        std::vector<int> counts;
        for (const int steps : {8, 16, 32, 64}) {
            const std::vector<double> coarse = run(end / steps);
            double worst = 0.0;
            for (std::size_t i = 0; i < coarse.size() && i < reference.size(); ++i) {
                worst = std::max(worst, std::fabs(coarse[i] - reference[i]));
            }
            errors.push_back(worst);
            counts.push_back(steps);
        }
        std::printf("  theta = %.1f, expecting order %.0f\n", theta, theta == 0.5 ? 2.0 : 1.0);
        std::vector<double> orders;
        for (std::size_t i = 0; i < errors.size(); ++i) {
            if (i == 0) {
                std::printf("      %3d steps   error %.4e\n", counts[i], errors[i]);
                continue;
            }
            const double order = std::log(errors[i - 1] / errors[i]) / std::log(2.0);
            orders.push_back(order);
            std::printf("      %3d steps   error %.4e   order %.3f\n", counts[i], errors[i], order);
        }
        const double expected = theta == 0.5 ? 2.0 : 1.0;
        CHECK(orders.back() > expected - 0.25);
        CHECK(orders.back() < expected + 0.5);
        CHECK(errors.back() < errors.front() * 0.4);
    }
}

void TestAdaptiveStepping() {
    std::printf("adaptive stepping:\n");
    // A slab heated by a flux that is switched on at t = 0: the gradient
    // is steepest at the very start and settles, which is the shape every
    // thermal transient has and the shape a fixed step handles worst --
    // too coarse at the start or wastefully fine at the end.
    const double length = 0.1;
    const double k = 45.0;
    const double end = 200.0;
    auto build = [&]() {
        Slab slab = MakeSlab(length, k, 20, 1);
        slab.model.materials[0].density = 7850.0;
        slab.model.materials[0].specific_heat = fem::MaterialCurve::Constant(460.0);
        for (ThermalFacet &facet : slab.at_x_min) {
            facet.flux = 20000.0;
            slab.model.facets.push_back(facet);
        }
        for (ThermalFacet &facet : slab.at_x_max) {
            facet.convection = 50.0;
            facet.ambient = 300.0;
            slab.model.facets.push_back(facet);
        }
        return slab;
    };
    Slab slab = build();
    std::vector<double> initial(Idx(slab.model.NodeCount()), 300.0);

    fem::TransientOptions fine;
    fine.theta = 1.0;
    fine.end_time = end;
    fine.step = end / 4000.0;
    fem::TransientResult reference;
    CHECK_MESSAGE(fem::SolveTransientHeat(slab.model, initial, fine, &reference), reference.error);

    fem::TransientOptions adaptive;
    adaptive.theta = 1.0;
    adaptive.end_time = end;
    adaptive.step = end / 1000.0;
    adaptive.adaptive = true;
    adaptive.tolerance = 2e-4;
    fem::TransientResult result;
    CHECK_MESSAGE(fem::SolveTransientHeat(slab.model, initial, adaptive, &result), result.error);
    if (!result.ok || !reference.ok) return;

    double worst = 0.0;
    for (std::size_t i = 0; i < result.temperature.size(); ++i) {
        worst = std::max(worst, std::fabs(result.temperature[i] - reference.temperature[i]));
    }
    std::printf("  %d steps (%d rejected), from %.4g s to %.4g s; %d fixed steps for the reference\n",
                result.steps, result.rejected, result.smallest_step, result.largest_step,
                reference.steps);
    std::printf("  agrees with the fine fixed-step run to %.4f K of a %.1f K rise\n", worst,
                reference.highest - 300.0);
    // FEWER STEPS AND A STEP THAT GREW, which together are the claim: an
    // adaptive integrator that took a uniform step would be a fixed-step
    // integrator with extra cost.
    CHECK(result.steps < reference.steps / 4);
    CHECK(result.largest_step > result.smallest_step * 10.0);
    // And it agrees with the fine run, which is what the tolerance was
    // asked for.
    CHECK(worst < (reference.highest - 300.0) * 0.01);
    CHECK(result.energy_residual < 1e-5);
}

void TestExplicitStability() {
    std::printf("the explicit method's stability limit:\n");
    const double length = 0.1;
    Slab slab = MakeSlab(length, 45.0, 10, 1);
    slab.model.materials[0].density = 7850.0;
    slab.model.materials[0].specific_heat = fem::MaterialCurve::Constant(460.0);
    FixEnd(&slab, false, 400.0);
    FixEnd(&slab, true, 300.0);
    std::vector<double> initial(Idx(slab.model.NodeCount()), 300.0);

    const double limit = fem::ExplicitStabilityLimit(slab.model, 300.0);
    const double alpha = 45.0 / (7850.0 * 460.0);
    const double h = length / 10.0;
    std::printf("  limit %.4g s; h^2/(2 alpha) is %.4g s\n", limit, h * h / (2.0 * alpha));
    // The classical explicit limit for this operator is h^2/(2 alpha) up
    // to a constant that depends on the element and the lumping. Being
    // within an order of magnitude of it is the statement worth making;
    // being exactly it would mean the bound had been fitted to the formula
    // rather than computed from the matrices.
    CHECK(limit > 0.0);
    CHECK(limit < h * h / (2.0 * alpha));
    CHECK(limit > h * h / (2.0 * alpha) * 0.01);

    // ASKED FOR TOO LARGE A STEP, IT REFUSES RATHER THAN DIVERGING. An
    // explicit method handed a step past its limit does not produce a
    // slightly worse answer, it produces oscillations that double every
    // step until the numbers stop being numbers -- and the result looks
    // like a modelling problem.
    fem::TransientOptions bad;
    bad.theta = 0.0;
    bad.end_time = 10.0;
    bad.step = limit * 4.0;
    fem::TransientResult result;
    CHECK(!fem::SolveTransientHeat(slab.model, initial, bad, &result));
    CHECK(result.error.find("conditional stability") != std::string::npos);
    std::printf("  %s\n", result.error.c_str());

    // And within the limit it works, and agrees with the implicit method.
    fem::TransientOptions good = bad;
    good.step = limit * 0.4;
    good.end_time = 20.0;
    fem::TransientResult explicit_run;
    CHECK_MESSAGE(fem::SolveTransientHeat(slab.model, initial, good, &explicit_run),
                  explicit_run.error);
    fem::TransientOptions implicit = good;
    implicit.theta = 1.0;
    fem::TransientResult implicit_run;
    CHECK_MESSAGE(fem::SolveTransientHeat(slab.model, initial, implicit, &implicit_run),
                  implicit_run.error);
    if (!explicit_run.ok || !implicit_run.ok) return;
    double worst = 0.0;
    for (std::size_t i = 0; i < explicit_run.temperature.size(); ++i) {
        worst = std::max(worst,
                         std::fabs(explicit_run.temperature[i] - implicit_run.temperature[i]));
    }
    std::printf("  explicit and implicit agree to %.4f K at the same step\n", worst);
    CHECK(worst < 5.0);
}

}  // namespace

int main() {
    TestFixedTemperatures();
    TestFluxAndConvection();
    TestRadiation();
    TestManufacturedSolution();
    TestTemperatureDependent();
    TestTransientAgainstFourier();
    TestTimeOrder();
    TestAdaptiveStepping();
    TestExplicitStability();
    if (failures != 0) {
        std::printf("fem_thermal_test FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("fem_thermal_test passed (%d checks)\n", checks);
    return 0;
}
