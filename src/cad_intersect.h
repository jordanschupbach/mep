#ifndef MEP_CAD_INTERSECT_H
#define MEP_CAD_INTERSECT_H

#include "cad_curve.h"
#include "cad_math.h"
#include "cad_surface.h"

#include <memory>
#include <string>
#include <vector>

// Intersections (plans/CAD_FEM_PLAN.md Parts C.1, C.2 and C.3).
//
// This is the part the plan calls "the project": every B-rep kernel's
// robustness reputation is decided by surface-surface intersection, and
// the reason is that the answer is a *curve* whose existence, shape and
// topology all depend on the input in ways no single formula covers.
//
// The structure of the answer is the argument of Part C.2. Roughly ninety
// per cent of the intersections a mechanical model actually asks for are
// between two members of a small family -- plane, cylinder, cone, sphere,
// torus -- and every one of those pairs has a closed form. A plane meets
// a cylinder in a line, a pair of lines, a circle or an ellipse, and
// which one it is falls out of two dot products. Routing those cases here
// means the general marcher in C.3 is reserved for the intersections that
// genuinely need it, where being slower and more delicate costs little
// because they are rare.
//
// The general path is not merely a fallback for exotic geometry, though.
// It is also what the analytic cases degrade into near tangency, where
// the closed forms are still exactly right but the *decision* between
// branches is ill-conditioned. Those cases are reported as tangent rather
// than resolved, so a caller can widen a tolerance or refuse, instead of
// receiving a confident answer built on a coin flip.
namespace cad {

// What kind of contact was found. Distinguishing these is not pedantry:
// a boolean that treats a tangential touch as a transversal crossing
// produces a body with a zero-thickness sliver, which then fails to mesh.
enum class ContactKind {
    // A clean transversal crossing -- the normal case, and the only one
    // where the answer is a simple point or curve.
    Transversal,
    // The two touch without crossing. The intersection exists but has no
    // interior, and no boolean can use it to separate material.
    Tangent,
    // They coincide over a region: two identical planes, two curves
    // sharing a segment. Booleans handle these by a separate path.
    Coincident,
    // They do not meet.
    None,
};

// ---------------------------------------------------------------------
// C.1 Curve-curve and curve-surface
// ---------------------------------------------------------------------

struct CurveCurveHit {
    ContactKind kind = ContactKind::Transversal;
    double t1 = 0.0;
    double t2 = 0.0;
    Vec3d point;
    // For a Coincident hit, the shared interval on each curve. Meaningless
    // otherwise.
    double t1_end = 0.0;
    double t2_end = 0.0;
};

struct IntersectOptions {
    Tolerance tolerance;
    // Initial subdivision of each curve or surface before refinement.
    // Raising it costs time linearly and reduces the chance of missing a
    // hit that is shorter than one interval.
    int samples = 64;
    // Newton iterations per candidate.
    int max_iterations = 40;

    // --- The general surface-surface marcher (Part C.3) ---------------

    // Grid resolution, per direction, used to find points to start
    // marching from. A branch smaller than one grid cell can be missed
    // entirely, which is the marcher's characteristic failure -- so this
    // is the knob that trades time against the size of feature it can
    // see.
    int seed_grid = 24;
    // How far the marched curve may deviate from the true intersection
    // between one point and the next. The step size adapts to hold it.
    double march_tolerance = 1e-4;
    // Bounds on the adaptive step, as fractions of the model size.
    double min_step_fraction = 1e-5;
    double max_step_fraction = 0.1;
    int max_points_per_branch = 4000;
};

// Every intersection of two curves. Closed forms are used where both are
// lines or circles in the same plane; otherwise the curves are sampled,
// close approaches are isolated, and Newton refines them.
//
// Note that two curves in 3D generically do *not* intersect -- they pass
// each other. So a hit means the two came within the tolerance, and the
// returned point is the midpoint of the closest approach. That is the
// useful definition for a kernel, where edges that were built to meet do
// meet only to within the tolerance they were built with.
std::vector<CurveCurveHit> IntersectCurves(const Curve3 &a, const Curve3 &b,
                                           const IntersectOptions &options = {});

struct CurveSurfaceHit {
    ContactKind kind = ContactKind::Transversal;
    double t = 0.0;      // parameter on the curve
    double u = 0.0;      // parameters on the surface
    double v = 0.0;
    Vec3d point;
};

// Every crossing of a curve through a surface. Used by the boolean
// imprinting in C.4 to find where an intersection curve meets an existing
// edge, and by ray casting to classify a point as inside or outside.
std::vector<CurveSurfaceHit> IntersectCurveSurface(const Curve3 &curve, const Surface &surface,
                                                   const IntersectOptions &options = {});

// ---------------------------------------------------------------------
// C.2 / C.3 Surface-surface
// ---------------------------------------------------------------------

// One branch of an intersection. The 3D curve is what the edge will be
// built from; the two p-curves are what the faces on either side need in
// order to be trimmed by it, and producing them here -- rather than
// re-deriving them by projection afterwards -- is what keeps them
// consistent with the curve and with each other.
struct IntersectionBranch {
    std::shared_ptr<const Curve3> curve;
    std::shared_ptr<const Curve3> pcurve_a;  // in a's parameter space, z = 0
    std::shared_ptr<const Curve3> pcurve_b;
    bool closed = false;
};

struct SurfaceIntersection {
    ContactKind kind = ContactKind::None;
    std::vector<IntersectionBranch> branches;
    // True when the branches came from a closed form rather than from
    // marching. Callers do not need to care; tests and diagnostics do,
    // because it says which code path ran.
    bool exact = false;
    std::string note;
};

// The analytic table (Part C.2). Returns false when the pair is not one
// it handles, leaving `out` untouched -- the caller then falls through to
// the general path.
//
// Handled: plane/plane, plane/sphere, plane/cylinder, plane/cone,
// plane/torus (axis-aligned cases), sphere/sphere, sphere/cylinder
// (coaxial), cylinder/cylinder (parallel or coaxial).
bool IntersectSurfacesAnalytic(const Surface &a, const Surface &b, SurfaceIntersection *out,
                               const IntersectOptions &options = {});

// The general marcher (Part C.3), for the pairs the analytic table
// declines. Finds seed points on a grid, then walks each branch by
// stepping along the tangent (the cross product of the two normals) and
// correcting back onto both surfaces, then fits the traced points to a
// NURBS with p-curves in both parameter spaces.
//
// Slower and more delicate than a closed form, which is exactly why Part
// C.2 exists to keep it off the common path. Its known limitation is
// stated plainly: a branch smaller than one seed grid cell can be missed,
// because nothing sampled it. Raising `seed_grid` is the remedy, and
// there is no way to be sure without one.
SurfaceIntersection MarchSurfaces(const Surface &a, const Surface &b, const IntersectOptions &options = {});

// The full intersection: the analytic table where it applies, the general
// marcher otherwise.
SurfaceIntersection IntersectSurfaces(const Surface &a, const Surface &b,
                                      const IntersectOptions &options = {});

// Builds the p-curve of a 3D curve on a surface, by projection and
// fitting. Exposed because the boolean code needs it for curves that did
// not come from an intersection -- an existing edge being imprinted onto
// a new face, for instance.
//
// Returns false if any sample fails to project, which for a curve that
// genuinely lies on the surface means the caller has a geometry problem
// worth hearing about rather than a p-curve worth approximating.
bool BuildPCurveOnSurface(const Surface &surface, const Curve3 &curve, double t_start, double t_end,
                          const IntersectOptions &options, std::shared_ptr<const Curve3> *out);

}  // namespace cad

#endif
