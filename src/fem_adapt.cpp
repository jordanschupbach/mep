#include "fem_adapt.h"

#include "fem_elem.h"

#include <algorithm>
#include <cmath>

namespace fem {
namespace {

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// The compliance matrix, by Gauss-Jordan on the constitutive one.
//
// INVERTED RATHER THAN WRITTEN OUT, because the closed form for the
// compliance of an isotropic material is only the closed form for an
// isotropic material, and the study's materials may be orthotropic or
// temperature dependent. Six by six once per element is nothing next to
// the solve that produced the field.
bool Invert6(const double in[6][6], double out[6][6]) {
    double a[6][12];
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) {
            a[i][j] = in[i][j];
            a[i][6 + j] = i == j ? 1.0 : 0.0;
        }
    }
    for (int column = 0; column < 6; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 6; ++row) {
            if (std::fabs(a[row][column]) > std::fabs(a[pivot][column])) pivot = row;
        }
        if (std::fabs(a[pivot][column]) < 1e-300) return false;
        if (pivot != column) {
            for (int j = 0; j < 12; ++j) std::swap(a[column][j], a[pivot][j]);
        }
        const double scale = 1.0 / a[column][column];
        for (int j = 0; j < 12; ++j) a[column][j] *= scale;
        for (int row = 0; row < 6; ++row) {
            if (row == column) continue;
            const double factor = a[row][column];
            if (factor == 0.0) continue;
            for (int j = 0; j < 12; ++j) a[row][j] -= factor * a[column][j];
        }
    }
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) out[i][j] = a[i][6 + j];
    }
    return true;
}

double Quadratic(const double matrix[6][6], const double v[6]) {
    double sum = 0.0;
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) sum += v[i] * matrix[i][j] * v[j];
    }
    return sum;
}

}  // namespace

bool EstimateError(const AnalysisModel &model, const std::vector<double> &displacement,
                   const StressField &recovered, ErrorEstimate *out, std::string *error) {
    *out = ErrorEstimate{};
    if (displacement.size() != Idx(model.NodeCount() * 3)) {
        *error = "the displacement vector is not three per node";
        return false;
    }
    if (recovered.nodal.size() != model.nodes.size()) {
        *error = "the recovered field does not match the model";
        return false;
    }
    out->element_error.assign(model.elements.size(), 0.0);
    out->element_energy.assign(model.elements.size(), 0.0);
    out->element_size.assign(model.elements.size(), 0.0);

    std::vector<QuadraturePoint> rule;
    std::vector<double> n;
    std::vector<double> dn;
    std::vector<double> dn_xyz;
    std::vector<cad::Vec3d> corner;
    double error_squared = 0.0;
    double energy_squared = 0.0;

    for (int e = 0; e < model.ElementCount(); ++e) {
        const BoundElement &element = model.elements[Idx(e)];
        if (element.material < 0 || element.material >= static_cast<int>(model.materials.size())) {
            *error = "the element names a material the model does not have";
            return false;
        }
        double constitutive[6][6];
        if (!model.materials[Idx(element.material)].ConstitutiveMatrix(model.temperature,
                                                                      constitutive, error)) {
            return false;
        }
        double compliance[6][6];
        if (!Invert6(constitutive, compliance)) {
            *error = "a material's constitutive matrix is singular";
            return false;
        }
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model.nodes[Idx(node)]);
        for (std::size_t a = 0; a < corner.size(); ++a) {
            for (std::size_t b = a + 1; b < corner.size(); ++b) {
                out->element_size[Idx(e)] =
                    std::max(out->element_size[Idx(e)], (corner[b] - corner[a]).Length());
            }
        }
        // INTEGRATED TWO DEGREES ABOVE THE ELEMENT'S OWN RULE. The
        // integrand is a difference of two stress fields of different
        // polynomial order, so it is not what the element's own rule was
        // chosen to integrate exactly, and under-integrating an error
        // estimate makes it look better on exactly the elements where the
        // two fields disagree most.
        int degree = 0;
        for (degree = 4; degree >= 1; --degree) {
            std::string ignored;
            if (Quadrature(element.shape, degree, &rule, &ignored)) break;
        }
        if (degree < 1) {
            *error = "no quadrature rule for an element the error estimate was asked about";
            return false;
        }
        for (const QuadraturePoint &point : rule) {
            ShapeFunctions(element.shape, point.at, &n, &dn);
            const double determinant = ElementJacobian(element.shape, corner, dn, &dn_xyz);
            if (!(determinant > 0.0)) {
                *error = "the element is turned inside out where the error was asked for";
                return false;
            }
            StressTensor element_stress;
            if (!ElementStressAt(model, e, point.at, displacement, &element_stress, error)) {
                return false;
            }
            // The recovered field is the *continuous* one: its nodal
            // values interpolated with the element's own shape functions.
            double difference[6] = {0, 0, 0, 0, 0, 0};
            double smooth[6] = {0, 0, 0, 0, 0, 0};
            for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                const StressTensor &at = recovered.nodal[Idx(element.nodes[a])];
                for (int i = 0; i < 6; ++i) smooth[i] += n[a] * at.s[i];
            }
            for (int i = 0; i < 6; ++i) difference[i] = smooth[i] - element_stress.s[i];
            const double weight = determinant * point.weight;
            out->element_error[Idx(e)] += weight * Quadratic(compliance, difference);
            out->element_energy[Idx(e)] += weight * Quadratic(compliance, smooth);
        }
        out->element_error[Idx(e)] = std::sqrt(std::max(0.0, out->element_error[Idx(e)]));
        out->element_energy[Idx(e)] = std::sqrt(std::max(0.0, out->element_energy[Idx(e)]));
        error_squared += out->element_error[Idx(e)] * out->element_error[Idx(e)];
        energy_squared += out->element_energy[Idx(e)] * out->element_energy[Idx(e)];
        if (out->worst_element < 0 ||
            out->element_error[Idx(e)] > out->element_error[Idx(out->worst_element)]) {
            out->worst_element = e;
        }
    }
    out->global_error = std::sqrt(error_squared);
    out->global_energy = std::sqrt(energy_squared);
    const double total = std::sqrt(energy_squared + error_squared);
    out->relative_error = total > 0.0 ? out->global_error / total : 0.0;
    return true;
}

bool PlanRefinement(const AnalysisModel &model, const ErrorEstimate &estimate,
                    const AdaptOptions &options, RefinementPlan *out, std::string *error) {
    *out = RefinementPlan{};
    if (estimate.element_error.size() != model.elements.size()) {
        *error = "the estimate does not match the model";
        return false;
    }
    if (model.elements.empty()) {
        *error = "there is nothing to refine";
        return false;
    }
    // THE ERROR IS SPREAD EVENLY OVER THE ELEMENTS, which is what an
    // optimal mesh looks like: a mesh where one element carries most of
    // the error is a mesh with the wrong elements somewhere, whether it
    // is too coarse there or too fine everywhere else. The permitted
    // error per element is therefore the target share of the total.
    const double total = std::sqrt(estimate.global_energy * estimate.global_energy +
                                   estimate.global_error * estimate.global_error);
    const double count = static_cast<double>(model.elements.size());
    out->target_element_error = options.target_relative_error * total / std::sqrt(count);
    if (!(out->target_element_error > 0.0)) {
        *error = "the model has no strain energy in it to measure an error against";
        return false;
    }
    for (int e = 0; e < model.ElementCount(); ++e) {
        const double ratio = estimate.element_error[Idx(e)] / out->target_element_error;
        if (!(ratio > 1.0)) continue;
        const int order = model.elements[Idx(e)].shape == ElementShape::Tet10 ||
                                  model.elements[Idx(e)].shape == ElementShape::Hex20 ||
                                  model.elements[Idx(e)].shape == ElementShape::Wedge15 ||
                                  model.elements[Idx(e)].shape == ElementShape::Tri6 ||
                                  model.elements[Idx(e)].shape == ElementShape::Quad8
                              ? 2
                              : 1;
        const double factor =
            std::min(options.max_reduction, std::pow(ratio, 1.0 / static_cast<double>(order)));
        double size = estimate.element_size[Idx(e)] / factor;
        if (options.min_size > 0.0) size = std::max(size, options.min_size);
        cad::Vec3d centre{};
        for (const int node : model.elements[Idx(e)].nodes) centre = centre + model.nodes[Idx(node)];
        centre = centre * (1.0 / static_cast<double>(model.elements[Idx(e)].nodes.size()));
        SizingOptions::Refinement refinement;
        refinement.centre = centre;
        // Half the element's own size: enough to cover the element that
        // asked for it and not so much that it refines its neighbours'
        // neighbours, which the growth limit will grade towards anyway.
        refinement.radius = estimate.element_size[Idx(e)] * 0.5;
        refinement.size = size;
        out->refinements.push_back(refinement);
        ++out->refined;
        out->smallest_requested = out->smallest_requested > 0.0
                                      ? std::min(out->smallest_requested, size)
                                      : size;
    }
    return true;
}

bool AdaptiveSolve(const cad::Model &model, const std::vector<cad::EntityId> &bodies,
                   const Study &study, const AdaptOptions &options, AnalysisModel *out_model,
                   StaticResult *out_result, AdaptReport *report) {
    *report = AdaptReport{};
    VolumeMeshOptions mesh_options = options.mesh;
    for (int cycle = 0; cycle < std::max(1, options.cycles); ++cycle) {
        VolumeMesh mesh;
        MeshReport mesh_report;
        if (!MeshBody(model, bodies, mesh_options, &mesh, &mesh_report)) {
            if (report->cycles.empty()) {
                report->error = "cycle " + std::to_string(cycle) + ": " + mesh_report.error;
                return false;
            }
            // A later cycle failing is not the same kind of event as the
            // first one failing. The first means the problem cannot be
            // meshed at all; a later one means the refinement asked for
            // something this mesher cannot grade to, and there is a
            // perfectly good answer from the cycle before it.
            report->stopped_early = true;
            report->stop_reason = "cycle " + std::to_string(cycle) + ": " + mesh_report.error;
            break;
        }
        AnalysisModel analysis;
        BindReport bind;
        if (!BindStudy(model, mesh, study, {}, &analysis, &bind)) {
            if (report->cycles.empty()) {
                report->error = "cycle " + std::to_string(cycle) + ": " + bind.error;
                return false;
            }
            report->stopped_early = true;
            report->stop_reason = "cycle " + std::to_string(cycle) + ": " + bind.error;
            break;
        }
        StaticResult result;
        if (!SolveStatic(analysis, {}, options.solve, &result)) {
            if (report->cycles.empty()) {
                report->error = "cycle " + std::to_string(cycle) + ": " + result.error;
                return false;
            }
            report->stopped_early = true;
            report->stop_reason = "cycle " + std::to_string(cycle) + ": " + result.error;
            break;
        }
        std::vector<double> flat(Idx(analysis.NodeCount() * 3), 0.0);
        for (int node = 0; node < analysis.NodeCount(); ++node) {
            flat[Idx(node * 3 + 0)] = result.displacement[Idx(node)].x;
            flat[Idx(node * 3 + 1)] = result.displacement[Idx(node)].y;
            flat[Idx(node * 3 + 2)] = result.displacement[Idx(node)].z;
        }
        ErrorEstimate estimate;
        if (!EstimateError(analysis, flat, result.stress, &estimate, &report->error)) return false;

        AdaptCycle record;
        record.elements = analysis.ElementCount();
        record.nodes = analysis.NodeCount();
        record.relative_error = estimate.relative_error;
        record.global_error = estimate.global_error;
        record.strain_energy = result.strain_energy;
        record.max_von_mises = result.max_von_mises;
        record.smallest_element = estimate.element_size.empty()
                                      ? 0.0
                                      : *std::min_element(estimate.element_size.begin(),
                                                          estimate.element_size.end());
        *out_model = analysis;
        *out_result = result;

        const bool met = estimate.relative_error <= options.target_relative_error;
        const bool last = cycle + 1 >= std::max(1, options.cycles);
        if (!met && !last) {
            RefinementPlan plan;
            if (!PlanRefinement(analysis, estimate, options, &plan, &report->error)) return false;
            record.refined = plan.refined;
            // ADDED TO THE ORIGINAL OPTIONS, NOT TO THE LAST CYCLE'S. The
            // refinements are absolute sizes at absolute places, so
            // accumulating them across cycles would leave an earlier
            // cycle's coarser request sitting on top of a later cycle's
            // finer one -- and since a sizing field takes the smaller of
            // what it is told, the stale ones are harmless but the list
            // grows without bound. Rebuilding from the original each time
            // keeps a cycle's mesh a function of that cycle's estimate.
            mesh_options.surface.sizing.refinements = options.mesh.surface.sizing.refinements;
            for (const SizingOptions::Refinement &refinement : plan.refinements) {
                mesh_options.surface.sizing.refinements.push_back(refinement);
            }
        }
        report->cycles.push_back(record);
        if (met) {
            report->reached_target = options.stop_at_target;
            if (options.stop_at_target) break;
        }
    }
    report->ok = true;
    return true;
}

}  // namespace fem
