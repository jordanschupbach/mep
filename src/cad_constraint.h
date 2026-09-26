#ifndef MEP_CAD_CONSTRAINT_H
#define MEP_CAD_CONSTRAINT_H

#include "cad_math.h"
#include "cad_sketch.h"

#include <string>
#include <tuple>
#include <vector>

// The sketch constraint solver and its diagnosis (plans/CAD_FEM_PLAN.md
// Parts D.2 and D.3).
//
// A sketch is a nonlinear least-squares problem: every constraint
// contributes residuals that are zero when it is satisfied, and the
// solver looks for parameter values making all of them zero at once.
// Levenberg-Marquardt is the driver rather than Gauss-Newton because an
// under-constrained sketch -- one with more parameters than independent
// equations -- is the normal state of a sketch being drawn, not an error,
// and LM's damped normal equations are solvable in that case while
// Gauss-Newton's are not.
//
// DERIVATIVES ARE EXACT, AND COME FROM THE RESIDUAL ITSELF. Each
// constraint is written once, in terms of a dual number carrying its own
// partial derivatives, so the Jacobian is the chain rule applied by the
// arithmetic rather than a second hand-derived formula that has to be
// kept in step with the first. That is not a finite difference: the
// values are exact to the last bit, and they cannot drift out of
// agreement with the residual because there is only one piece of code.
// The test still checks them against central differences, which now tests
// something real -- that the residual means what the constraint says, and
// that each partial lands in the right column.
//
// WHAT THE DIAGNOSIS IS FOR. "Too many constraints" is two completely
// different situations and a tool that conflates them is miserable to
// use. Three sides of a rectangle told to be equal and a diagonal told to
// be a length consistent with them is *redundant*: the equations depend
// on each other but agree, and the sketch solves. The same sketch with a
// diagonal of the wrong length is *conflicting*: the equations depend on
// each other and disagree, and nothing satisfies them. Both look like
// rank deficiency in the Jacobian. What tells them apart is whether the
// dependency is also present in the residual -- see SketchStatus below.
namespace cad {

// Where each of a sketch's parameters lives in the solver's vector.
//
// A fixed point contributes nothing, which is how anchoring works: the
// parameter simply does not exist, rather than existing and being held by
// a constraint. That matters for the degree-of-freedom count, which would
// otherwise have to know to subtract the anchors back out again.
class SketchParameters {
public:
    explicit SketchParameters(const Sketch &sketch);

    int Count() const { return count_; }
    // -1 when the point is fixed or unknown.
    int PointX(SketchId point) const;
    int PointY(SketchId point) const;
    // -1 when the entity has no such scalar.
    int Scalar(SketchId entity, int index) const;

    void Gather(const Sketch &sketch, std::vector<double> *out) const;
    void Scatter(const std::vector<double> &values, Sketch *sketch) const;

    // Which point and coordinate a parameter belongs to, for turning a
    // free direction back into motion a user can see.
    struct Owner {
        SketchId point = kNoSketchId;
        SketchId entity = kNoSketchId;
        int component = 0;  // 0 = x or first scalar, 1 = y, ...
    };
    const Owner &OwnerOf(int parameter) const { return owners_[static_cast<std::size_t>(parameter)]; }

private:
    int count_ = 0;
    std::vector<std::pair<SketchId, int>> point_slots_;  // point id -> first slot
    // Entity, scalar index, slot. Listed one by one rather than as a
    // base offset because a fixed scalar takes no slot, so an entity's
    // scalars need not be contiguous in the parameter vector.
    std::vector<std::tuple<SketchId, int, int>> scalar_slots_;
    std::vector<Owner> owners_;
};

enum class SketchStatus {
    // Every constraint satisfied, and no freedom left: the sketch is
    // fully constrained, which is the state a finished sketch should be in.
    Solved,
    // Every constraint satisfied, but the sketch can still move. Not an
    // error -- most sketches are here most of the time.
    UnderConstrained,
    // The constraints depend on one another but agree. The sketch solves;
    // the extra constraints simply say nothing new.
    Redundant,
    // The constraints depend on one another and disagree. Nothing
    // satisfies them, and `conflicting` names the ones involved.
    Conflicting,
    // The solver ran out of iterations without the residual coming down,
    // and without a rank deficiency to explain why.
    NotConverged,
};

struct ConstraintSolveOptions {
    int max_iterations = 200;
    // Residual norm below which the sketch counts as satisfied. In the
    // sketch's own length units, so a sketch drawn in metres and one in
    // millimetres are not held to the same absolute standard -- the
    // solver scales this by the sketch's extent.
    double tolerance = 1e-9;
    // Solve each independent group of constraints as its own problem.
    // A 400-entity sketch is almost never one coupled system, and solving
    // it as one means factorising a dense block that is mostly zeros.
    bool decompose = true;
    // Relative singular-value cutoff for the rank, and so for the
    // redundant-versus-independent decision.
    double rank_tolerance = 1e-9;
};

struct SketchDiagnosis {
    SketchStatus status = SketchStatus::NotConverged;
    int parameters = 0;
    int residuals = 0;
    int rank = 0;
    // parameters - rank: how many independent ways the sketch can still
    // move. Zero means fully constrained.
    int degrees_of_freedom = 0;
    double residual_norm = 0.0;
    int iterations = 0;
    int clusters = 0;
    // An orthonormal basis for those ways, each vector `parameters` long.
    // These are the drag handles: moving along one changes nothing that
    // any constraint can see.
    std::vector<std::vector<double>> free_directions;
    // Constraints implicated in a dependency that agrees, and in one that
    // does not. Never both non-empty for the same dependency.
    std::vector<SketchId> redundant;
    std::vector<SketchId> conflicting;
    std::string message;
};

// The residuals and Jacobian at the sketch's current geometry.
// `residual_owner` names, for each row, the constraint that produced it.
// Exposed because the diagnosis, the solver and the tests all need it,
// and because a UI that wants to show a constraint's current error should
// read the same numbers the solver does rather than recompute them.
bool EvaluateConstraints(const Sketch &sketch, const SketchParameters &parameters,
                         std::vector<double> *residuals, MatrixNd *jacobian,
                         std::vector<SketchId> *residual_owner, std::string *error);

// How many residuals a constraint contributes, and whether it is
// supported at all in the sketch's current configuration.
int ResidualCount(const Sketch &sketch, const SketchConstraint &constraint);

// Solves, updating the sketch's geometry in place, and fills in the
// diagnosis. Returns false only when the sketch cannot be evaluated at
// all (a constraint naming a missing entity); a conflicting sketch
// returns true with the conflict reported, because that is an answer
// rather than a failure.
bool SolveSketch(Sketch *sketch, SketchDiagnosis *diagnosis, const ConstraintSolveOptions &options = {});

// The same analysis without moving anything.
bool DiagnoseSketch(const Sketch &sketch, SketchDiagnosis *diagnosis,
                    const ConstraintSolveOptions &options = {});

// Drag `point` toward `target`, satisfying the constraints. Implemented
// as an ordinary solve with the dragged point temporarily pinned, warm
// started from where the sketch already is -- which is what makes it fast
// enough to run per mouse-move frame, and what makes it follow the
// pointer rather than jumping to some other solution of the same system.
bool DragPoint(Sketch *sketch, SketchId point, const Vec2d &target, SketchDiagnosis *diagnosis,
               const ConstraintSolveOptions &options = {});

// Rewrites every reference (non-driving) dimension from the geometry, so
// that it measures what the sketch actually is. Called at the end of a
// solve; exposed for a caller that changed geometry another way.
void UpdateReferenceDimensions(Sketch *sketch);

}  // namespace cad

#endif
