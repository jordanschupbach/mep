#ifndef MEP_CAD_PATTERN_H
#define MEP_CAD_PATTERN_H

#include "cad_boolean.h"
#include "cad_sketch.h"
#include "cad_topology.h"

#include <string>
#include <vector>

// Patterns and transforms (plans/CAD_FEM_PLAN.md Part E.5).
//
// A PATTERN IS A LIST OF PLACEMENTS, and everything else follows from
// that. Linear, circular, mirror, sketch-driven and table-driven are not
// five operations; they are five ways of writing down a list of rigid
// transforms, after which one routine copies the body once per entry and
// decides what to do with the copies. So the generators below return
// `std::vector<Mat4d>` and are pure functions of their arguments -- they
// never touch a body -- and `PatternBody` is the only thing that does.
//
// That split is what makes table-driven patterns free rather than a
// sixth implementation, and it is why a caller who wants placements this
// header does not offer can simply hand `PatternBody` its own list.
//
// MIRRORING IS NOT A RIGID MOTION and the difference is not cosmetic. A
// reflection reverses handedness, so a face's surface normal comes out
// pointing into the material rather than out of it, and a body mirrored
// without noticing is perfectly valid, perfectly watertight and has a
// negative volume. `TransformBody` checks the determinant and turns every
// face round when it is negative, which is the same correction Part E.4's
// cavity needed and is made in the same place: the orientation flag, not
// the loops, because a loop runs counter-clockwise in its surface's own
// parameters whichever way the face points.
namespace cad {

// What to do with the copies once they are placed.
enum class PatternCombine {
    // Each copy is its own body. A multi-body part, and the only option
    // that cannot fail for geometric reasons.
    Separate,
    // Unioned into one. Copies that overlap transversally merge; copies
    // that merely touch along a face are the coincident-face case Part
    // C.4 does not handle, and are reported rather than approximated.
    Merge,
};

struct PatternOptions {
    BooleanOptions boolean;
    // Drop a placement that would put a copy exactly where the seed
    // already is. A circular pattern of a body centred on its own axis
    // does this, and so does the identity entry every generator includes.
    bool skip_coincident = true;
    Tolerance tolerance;
};

// --- The one routine ---------------------------------------------------

// A deep copy of `body` through `transform`, into a model of its own.
bool TransformBody(const Model &model, EntityId body, const Mat4d &transform, Model *out,
                   EntityId *out_body, std::string *error);

// Places one copy of `body` per entry in `placements`.
bool PatternBody(const Model &model, EntityId body, const std::vector<Mat4d> &placements,
                 PatternCombine combine, const PatternOptions &options, Model *out,
                 std::vector<EntityId> *out_bodies, std::string *error);

// --- The generators ----------------------------------------------------
//
// Each returns `count` transforms -- or `count_u * count_v` for the two
// dimensional form -- of which the first is the identity, so the seed
// body is placement zero rather than a special case the caller has to
// remember to include.

// `count` copies spaced `spacing` apart along `direction`.
std::vector<Mat4d> LinearPlacements(const Vec3d &direction, double spacing, int count);

// A grid: the outer product of two linear patterns.
std::vector<Mat4d> GridPlacements(const Vec3d &direction_u, double spacing_u, int count_u,
                                  const Vec3d &direction_v, double spacing_v, int count_v);

// `count` copies about an axis. `total_angle` is the angle the whole
// pattern spans, not the step: a full turn of six copies steps by 60
// degrees and does not put a seventh on top of the first, while six
// copies spanning 90 degrees step by 18. Which of those a caller means
// is the one thing a circular pattern is always ambiguous about, so
// `close` says it: true spans the angle and leaves the last step open,
// false puts a copy at each end.
std::vector<Mat4d> CircularPlacements(const Vec3d &axis_point, const Vec3d &axis, int count,
                                      double total_angle, bool close);

// The identity and one reflection.
std::vector<Mat4d> MirrorPlacements(const Vec3d &plane_point, const Vec3d &plane_normal);

// A copy at every point of a sketch, translated from the first point to
// each of the others. Driving a pattern from sketch geometry is what
// makes it parametric: move the sketch point and the copy follows.
bool SketchPlacements(const Sketch &sketch, const std::vector<SketchId> &points,
                      std::vector<Mat4d> *out, std::string *error);

// Reflection about a plane, on its own -- the one transform Mat4d does
// not build and the one whose sign is easy to get wrong.
Mat4d Reflection(const Vec3d &plane_point, const Vec3d &plane_normal);

}  // namespace cad

#endif
