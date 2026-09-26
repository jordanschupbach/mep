#ifndef MEP_CAD_VALIDATE_H
#define MEP_CAD_VALIDATE_H

#include "cad_topology.h"

#include <string>
#include <vector>

// B-rep validity checking (plans/CAD_FEM_PLAN.md Part B.3).
//
// The plan calls this "the single highest-leverage investment in the
// whole plan", and the reason is about *where failures surface*. A
// boolean operation that produces a body with one edge referenced by
// three faces does not fail. Neither does the fillet applied to it
// afterwards, nor the STEP export. It fails eventually, in the
// tetrahedral mesher, as a boundary-recovery loop that will not
// terminate -- several thousand lines and two parts of the plan away
// from the operation that actually broke, with nothing left of the
// evidence.
//
// So every kernel operation checks its output here, and the cost of
// doing so is the price of being able to debug the kernel at all.
//
// The checks fall into three groups:
//
//   * Structural -- every reference resolves, every face has exactly one
//     outer loop, every loop closes as a vertex chain. Cheap, and catches
//     the majority of construction mistakes.
//   * Combinatorial -- each edge is used by exactly two coedges with
//     opposite senses (the manifold condition), and the Euler-Poincare
//     relation holds. These catch the errors that are structurally
//     well-formed but describe an impossible solid.
//   * Geometric -- edges lie on both of their faces, vertices lie on
//     their edges, p-curves agree with the 3D curves they represent, and
//     loops run the right way round in parameter space. The expensive
//     group, and the one that catches geometry and topology having
//     drifted apart.
namespace cad {

enum class Severity {
    // The model is not a valid B-rep. Downstream operations may do
    // anything at all with it.
    Error,
    // Suspicious but survivable, or a condition this kernel tolerates
    // deliberately (a non-manifold edge in an open shell, say).
    Warning,
};

struct Diagnostic {
    Severity severity = Severity::Error;
    // A short stable slug, so a test can assert that a *specific* check
    // fired rather than matching on prose.
    std::string category;
    std::string message;
    EntityId entity = kNoEntity;
};

struct ValidationReport {
    std::vector<Diagnostic> diagnostics;
    // Genus implied by the Euler-Poincare relation for each body, in the
    // order the bodies appear. Reported rather than merely checked: a
    // torus really does have genus 1, and an operation that was supposed
    // to produce a sphere and produced genus 1 has a specific, nameable
    // bug.
    std::vector<int> body_genus;

    bool Ok() const;
    int ErrorCount() const;
    int WarningCount() const;
    // Every diagnostic of one category, for tests and for a UI that wants
    // to group them.
    std::vector<const Diagnostic *> OfCategory(const std::string &category) const;
    std::string Summary() const;
};

struct ValidationOptions {
    Tolerance tolerance;
    // Samples per edge for the geometric checks. The default is enough to
    // catch a p-curve that wanders; raising it costs time linearly.
    int samples_per_edge = 16;
    // Skip the geometric group. Worth doing in an inner loop that is
    // checking structure only -- a boolean's intermediate states are
    // structurally meaningful before they are geometrically settled.
    bool check_geometry = true;
    // Whether a shell is expected to be closed. An open shell (a surface
    // patch, a sheet body) is a legitimate thing this kernel will
    // eventually carry, and its free edges are not errors.
    bool require_closed_shells = true;
};

// Validates every body in the model. Returns report->Ok(), so the common
// case reads as `if (!Validate(model, &report)) { ... }`.
bool Validate(const Model &model, ValidationReport *report, const ValidationOptions &options = {});

// One body, when the model holds several and only one is of interest.
bool ValidateBody(const Model &model, EntityId body, ValidationReport *report,
                  const ValidationOptions &options = {});

}  // namespace cad

#endif
