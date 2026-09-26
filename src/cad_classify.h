#ifndef MEP_CAD_CLASSIFY_H
#define MEP_CAD_CLASSIFY_H

#include "cad_intersect.h"
#include "cad_math.h"
#include "cad_topology.h"

#include <vector>

// Point classification against a solid (plans/CAD_FEM_PLAN.md, the
// "classify" third of Part C.4's imprint-classify-stitch).
//
// A boolean works by cutting both bodies along their intersection curves
// and then deciding, for each resulting piece of surface, whether it lies
// inside the other body, outside it, or on its boundary. That decision is
// this file, and everything else in a boolean is bookkeeping around it.
//
// The method is ray casting with crossing parity: fire a ray from the
// point and count how many times it passes through the boundary. Odd
// means inside. What makes it delicate is not the idea but the
// degeneracies -- a ray that grazes a face tangentially, passes exactly
// through an edge, or hits a vertex, gets counted once, twice or not at
// all depending on arithmetic that is right at the tolerance. The
// standard defence, used here, is to detect that the ray was unlucky and
// fire a different one rather than to try to decide the degenerate case.
namespace cad {

enum class PointClass {
    Inside,
    Outside,
    // Within tolerance of the boundary. A boolean must treat this as its
    // own case: a face piece lying *on* the other body's surface is kept
    // or dropped depending on whether the two agree about which way is
    // out, and guessing Inside or Outside for it produces a solid with a
    // duplicated or missing face.
    Boundary,
};

struct ClassifyOptions {
    IntersectOptions intersect;
    // How many differently-aimed rays to try before giving up. Each
    // retry costs a full pass over the faces, and needing more than two
    // is rare enough that the default is generous.
    int max_ray_attempts = 8;

    // Samples along the ray when looking for crossings, and the
    // |direction . normal| below which a crossing counts as grazing and
    // the ray is retried.
    //
    // These two have to be chosen together, and getting that wrong is
    // subtle. A ray that nearly misses a body enters and leaves through a
    // very short chord; if that chord is shorter than one sampling
    // interval, the two crossings collapse into one sample and only one
    // is found -- giving odd parity and calling an outside point inside.
    //
    // For a sphere of radius r, a chord of length c meets the surface at
    // |direction . normal| = (c/2)/r. So a grazing threshold of g catches
    // every chord shorter than 2*g*r, and the sampling must resolve
    // anything longer than that. With the defaults below, over a ray
    // reaching four body-diagonals, the threshold has roughly an order of
    // magnitude of margin over the sampling interval.
    int ray_samples = 256;
    double grazing_threshold = 0.02;
};

// Classifies a point against one body of the model.
PointClass ClassifyPoint(const Model &model, EntityId body, const Vec3d &point,
                         const ClassifyOptions &options = {});

// Whether a point in a face's parameter space lies within that face's
// trimming loops -- inside the outer one and outside every hole. The
// building block of the above, and separately useful: a boolean asks it
// of every candidate face piece.
//
// Requires p-curves. Returns false (meaning "not in the face") when they
// are missing, rather than guessing.
bool PointInFace(const Model &model, EntityId face, double u, double v);

// Where a ray crosses a body's boundary, in increasing distance. Exposed
// because the imprinting half of a boolean wants the hits themselves, not
// only their parity.
struct RayHit {
    double distance = 0.0;
    EntityId face = kNoEntity;
    Vec3d point;
    Vec3d normal;  // the face's outward normal there
    double u = 0.0;
    double v = 0.0;
};

std::vector<RayHit> CastRay(const Model &model, EntityId body, const Vec3d &origin, const Vec3d &direction,
                            const ClassifyOptions &options = {});

}  // namespace cad

#endif
