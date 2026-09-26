#ifndef MEP_CAD_NAMING_H
#define MEP_CAD_NAMING_H

#include "cad_math.h"
#include "cad_topology.h"

#include <string>
#include <vector>

// Persistent naming (plans/CAD_FEM_PLAN.md Part B.4).
//
// This is the problem that breaks parametric CAD, and it is worth being
// precise about what it is. A feature history says "fillet that edge" and
// "put a hole through that face". When a dimension changes, the whole
// history replays and produces an entirely new set of topological
// entities -- new ids, possibly a different count, possibly in a
// different order. "That face" has to be found again in the new model,
// and nothing about an integer id survives the rebuild.
//
// No system solves this completely; the literature is clear that it
// cannot be solved completely, because a rebuild can genuinely destroy
// the thing a later feature referred to. What a system can do is:
//
//   * get the common cases right, by recording enough about an entity
//     that it can be recognised rather than merely indexed; and
//   * know when it is unsure, and say so, instead of silently picking the
//     wrong face and quietly moving a hole to the other side of a part.
//
// The second is the design constraint here. Resolve() returns Ambiguous
// or Lost as first-class outcomes, and a caller that treats them as
// errors worth surfacing will behave better than one that takes the best
// guess -- which is why there is no "just give me the best match"
// convenience overload.
//
// The signature recorded is deliberately redundant: the generating
// feature and role, the kind of geometry, a point that lies on the entity,
// and the features that generated its neighbours. Any one of these can
// survive a rebuild that destroys the others, and the scoring below
// weighs them so that agreement on several outvotes disagreement on one.
namespace cad {

// What a name records. Built by CaptureName at the moment an entity is
// created (or selected), and used later against a rebuilt model.
struct EntityName {
    // The feature that produced this entity, and its role within that
    // feature. Together these are the strongest signal -- a box's "top"
    // face is the top face after any dimension change.
    int generating_feature = -1;
    std::string role;

    // What kind of geometry underlies it. A face that was a cylinder and
    // is now a plane is almost certainly not the same face.
    SurfaceKind surface_kind = SurfaceKind::Plane;
    CurveKind curve_kind = CurveKind::Line;
    bool is_face = true;

    // A point on the entity, in model space. Weak on its own -- the whole
    // point of a parametric change is that geometry moves -- but a strong
    // tiebreaker between candidates that are otherwise equal.
    Vec3d sample_point;
    // The entity's outward direction where that is meaningful (a face's
    // normal at the sample point). Survives scaling, which the point does
    // not.
    Vec3d sample_direction;

    // Generating features of the entities adjacent to this one, sorted.
    // This is what distinguishes two faces that are alike in every other
    // respect: they have different neighbours.
    std::vector<int> neighbour_features;
    // Counts, as a cheap structural fingerprint.
    int loop_count = 0;
    int edge_count = 0;
};

enum class ResolveStatus {
    // Exactly one candidate scored well and clearly better than the rest.
    Resolved,
    // Several candidates scored comparably. The reference cannot be
    // honoured without guessing, so the caller is told rather than given
    // a guess.
    Ambiguous,
    // Nothing scored well enough. The entity the name referred to is most
    // likely gone -- a face consumed by a boolean, an edge that a fillet
    // removed.
    Lost,
};

struct ResolveResult {
    ResolveStatus status = ResolveStatus::Lost;
    EntityId entity = kNoEntity;
    double score = 0.0;
    // Every candidate that scored above the acceptance threshold, best
    // first. Populated for Ambiguous so a UI can offer a choice, and for
    // Resolved so a caller can see what it beat.
    std::vector<EntityId> candidates;
    std::vector<double> scores;
    std::string explanation;
};

struct NamingOptions {
    // A candidate must reach this fraction of the maximum possible score
    // to be considered at all.
    double accept_threshold = 0.45;
    // The best candidate must beat the runner-up by this much (as a
    // fraction of the best score) to count as unambiguous. Set
    // deliberately high: reporting Ambiguous costs a user one click,
    // while resolving wrongly costs them a part that is silently
    // incorrect.
    double dominance_margin = 0.15;
    Tolerance tolerance;
};

// Records a name for a face or an edge of the given model.
EntityName CaptureFaceName(const Model &model, EntityId face, int generating_feature, const std::string &role);
EntityName CaptureEdgeName(const Model &model, EntityId edge, int generating_feature, const std::string &role);

// Finds the entity in `model` that the name refers to.
ResolveResult ResolveFace(const Model &model, const EntityName &name, const NamingOptions &options = {});
ResolveResult ResolveEdge(const Model &model, const EntityName &name, const NamingOptions &options = {});

}  // namespace cad

#endif
