#ifndef MEP_FEM_ADAPT_H
#define MEP_FEM_ADAPT_H

#include "fem_mesh.h"
#include "fem_result.h"
#include "fem_static.h"
#include "fem_study.h"

#include <string>
#include <vector>

// Adaptivity (plans/CAD_FEM_PLAN.md Part J.5).
//
// WHY THIS IS THE ARGUMENT FOR THE WHOLE ARCHITECTURE, and not merely a
// feature. Adaptive refinement means throwing a mesh away and building a
// better one, and a better one can only be built against something that
// still knows what the part *is*. A package that imports a mesh can
// subdivide the elements it was given; it cannot put new nodes on the
// fillet, because it no longer has the fillet -- it has a faceted
// approximation, and every refinement makes a finer approximation of the
// facets rather than of the part. Keeping the exact B-rep through
// meshing, binding and solving is what makes the loop close.
//
// THE ESTIMATOR IS ZIENKIEWICZ AND ZHU'S, and its whole idea is that the
// recovered stress field of Part J.1 is a better answer than the
// element's own. If it is, then the difference between them estimates how
// wrong the element's own is -- without knowing the true answer, which is
// the entire difficulty of error estimation. The estimate is measured in
// the energy norm, because that is the norm the finite element method
// actually minimises in, so it is the one in which the statement is true
// rather than merely plausible.
namespace fem {

struct ErrorEstimate {
    // The energy norm of (recovered stress - element stress) over each
    // element.
    std::vector<double> element_error;
    // The energy norm of the recovered stress over each element: the
    // denominator, per element, and useful on its own -- an element with
    // a large error and no energy in it is a different problem from one
    // with both.
    std::vector<double> element_energy;
    // The longest edge of each element, which is what the sizing field
    // means by a size.
    std::vector<double> element_size;

    double global_error = 0.0;
    double global_energy = 0.0;
    // error / sqrt(energy^2 + error^2), which is the form the estimate is
    // conventionally quoted in and is bounded by one however bad the
    // mesh. Dividing by the energy alone is unbounded and reports
    // thousands of percent for a mesh that is merely coarse.
    double relative_error = 0.0;
    int worst_element = -1;
};

bool EstimateError(const AnalysisModel &model, const std::vector<double> &displacement,
                   const StressField &recovered, ErrorEstimate *out, std::string *error);

struct AdaptOptions {
    // The relative energy-norm error to aim for. Five percent is the
    // usual engineering default and is not a claim about the stress at
    // any one point.
    double target_relative_error = 0.05;
    int cycles = 3;
    // The most an element's size may fall in one cycle. Uncapped, the
    // first cycle on a bad mesh asks for elements a hundred times smaller
    // and the mesher either refuses or produces something enormous; the
    // estimate is not trustworthy enough at that distance to be obeyed in
    // one jump.
    double max_reduction = 3.0;
    double min_size = 0.0;
    // Stop once the target is met.
    bool stop_at_target = true;
    StaticOptions solve;
    VolumeMeshOptions mesh;
};

struct RefinementPlan {
    std::vector<SizingOptions::Refinement> refinements;
    int refined = 0;
    double smallest_requested = 0.0;
    // The per-element error the target implies, which is what each
    // element is being compared against.
    double target_element_error = 0.0;
};

// Turns an estimate into sizing-field refinements.
//
// THE EXPONENT IS THE ELEMENT'S CONVERGENCE RATE, not a tuning constant.
// An element of order p has an energy-norm error going as h^p, so an
// element whose error is `ratio` times too big needs its size divided by
// `ratio^(1/p)`. Using the wrong exponent does not break the loop -- it
// still converges -- it just takes more cycles, which is exactly the kind
// of quietly-wrong that is worth naming.
bool PlanRefinement(const AnalysisModel &model, const ErrorEstimate &estimate,
                    const AdaptOptions &options, RefinementPlan *out, std::string *error);

struct AdaptCycle {
    int elements = 0;
    int nodes = 0;
    double relative_error = 0.0;
    double global_error = 0.0;
    double strain_energy = 0.0;
    double max_von_mises = 0.0;
    double smallest_element = 0.0;
    int refined = 0;
};

struct AdaptReport {
    bool ok = false;
    std::string error;
    std::vector<AdaptCycle> cycles;
    bool reached_target = false;
    // STOPPED BECAUSE THE NEXT MESH COULD NOT BE BUILT, rather than
    // because the target was met. A refined sizing field asks for a
    // graded mesh, and grading is where a Delaunay mesher produces
    // slivers; the loop cannot fix that and must not pretend it did not
    // happen. What it *can* do is return the best result it reached and
    // say why it stopped, which is more use than a failure -- the answer
    // from the last good cycle is a real answer.
    bool stopped_early = false;
    std::string stop_reason;
};

// Mesh, bind, solve, estimate, refine, and go round again.
//
// Takes the CAD model and the study rather than an analysis model,
// because that is the point: each cycle re-meshes the *geometry*. An
// adaptive loop handed a mesh could only subdivide it.
bool AdaptiveSolve(const cad::Model &model, const std::vector<cad::EntityId> &bodies,
                   const Study &study, const AdaptOptions &options, AnalysisModel *out_model,
                   StaticResult *out_result, AdaptReport *report);

}  // namespace fem

#endif
