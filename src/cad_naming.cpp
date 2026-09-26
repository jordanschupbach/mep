#include "cad_naming.h"

#include <algorithm>
#include <cmath>

namespace cad {
namespace {

// Weights for each piece of evidence. Chosen so that no single signal can
// resolve a name by itself: the generating feature and role together are
// worth less than the acceptance threshold, so a rebuild that keeps the
// role but produces geometry of a different kind in a different place
// will not resolve. That is deliberate -- the failure mode worth avoiding
// is confident wrongness.
constexpr double kWeightFeature = 0.25;
constexpr double kWeightRole = 0.20;
constexpr double kWeightKind = 0.15;
constexpr double kWeightNeighbours = 0.20;
constexpr double kWeightDirection = 0.12;
constexpr double kWeightPoint = 0.08;
constexpr double kWeightTotal =
    kWeightFeature + kWeightRole + kWeightKind + kWeightNeighbours + kWeightDirection + kWeightPoint;

// Jaccard similarity of two sorted multisets of feature ids.
double NeighbourSimilarity(const std::vector<int> &a, const std::vector<int> &b) {
    if (a.empty() && b.empty()) return 1.0;
    if (a.empty() || b.empty()) return 0.0;
    std::vector<int> intersection;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(intersection));
    std::vector<int> union_set;
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(union_set));
    if (union_set.empty()) return 1.0;
    return static_cast<double>(intersection.size()) / static_cast<double>(union_set.size());
}

// Distance similarity, scaled by the model so it means the same thing for
// a part measured in millimetres and one in metres. Falls off smoothly
// rather than as a threshold: a face that moved a little is a better
// match than one that moved a lot, and both are worse than one that did
// not move.
double PointSimilarity(const Vec3d &a, const Vec3d &b, double scale) {
    if (scale <= 0.0) return 1.0;
    const double distance = (a - b).Length() / scale;
    return 1.0 / (1.0 + distance * distance);
}

double DirectionSimilarity(const Vec3d &a, const Vec3d &b) {
    if (a.LengthSquared() <= 0.0 || b.LengthSquared() <= 0.0) return 0.5;
    // Maps the dot product from [-1,1] to [0,1], so opposite directions
    // score zero rather than negative.
    return 0.5 * (1.0 + Clamp(a.Normalized().Dot(b.Normalized()), -1.0, 1.0));
}

std::vector<int> NeighbourFeaturesOfFace(const Model &model, EntityId face_id) {
    std::vector<int> features;
    for (EntityId e : model.EdgesOfFace(face_id)) {
        for (EntityId f : model.FacesOfEdge(e)) {
            if (f == face_id) continue;
            const Face *neighbour = model.GetFace(f);
            if (neighbour == nullptr) continue;
            // The neighbour's own generating feature is not stored on the
            // Face (features arrive in Part E), so its name stands in --
            // stable across a rebuild for exactly the same reason.
            features.push_back(static_cast<int>(std::hash<std::string>{}(neighbour->name) & 0x7fffffff));
        }
    }
    std::sort(features.begin(), features.end());
    features.erase(std::unique(features.begin(), features.end()), features.end());
    return features;
}

std::vector<int> NeighbourFeaturesOfEdge(const Model &model, EntityId edge_id) {
    std::vector<int> features;
    for (EntityId f : model.FacesOfEdge(edge_id)) {
        const Face *face = model.GetFace(f);
        if (face == nullptr) continue;
        features.push_back(static_cast<int>(std::hash<std::string>{}(face->name) & 0x7fffffff));
    }
    std::sort(features.begin(), features.end());
    features.erase(std::unique(features.begin(), features.end()), features.end());
    return features;
}

double ScoreCandidate(const EntityName &name, const EntityName &candidate, double scale) {
    double score = 0.0;
    if (name.generating_feature == candidate.generating_feature) score += kWeightFeature;
    if (name.role == candidate.role && !name.role.empty()) score += kWeightRole;
    if (name.is_face == candidate.is_face) {
        const bool same_kind =
            name.is_face ? (name.surface_kind == candidate.surface_kind) : (name.curve_kind == candidate.curve_kind);
        if (same_kind) score += kWeightKind;
    }
    score += kWeightNeighbours * NeighbourSimilarity(name.neighbour_features, candidate.neighbour_features);
    score += kWeightDirection * DirectionSimilarity(name.sample_direction, candidate.sample_direction);
    score += kWeightPoint * PointSimilarity(name.sample_point, candidate.sample_point, scale);
    return score / kWeightTotal;
}

ResolveResult ResolveAgainst(const std::vector<std::pair<EntityId, EntityName>> &candidates,
                             const EntityName &name, double scale, const NamingOptions &options) {
    ResolveResult result;
    std::vector<std::pair<double, EntityId>> scored;
    for (const auto &entry : candidates) {
        const double score = ScoreCandidate(name, entry.second, scale);
        if (score >= options.accept_threshold) scored.push_back({score, entry.first});
    }
    std::sort(scored.begin(), scored.end(),
              [](const std::pair<double, EntityId> &a, const std::pair<double, EntityId> &b) {
                  return a.first > b.first;
              });
    for (const auto &entry : scored) {
        result.candidates.push_back(entry.second);
        result.scores.push_back(entry.first);
    }
    if (scored.empty()) {
        result.status = ResolveStatus::Lost;
        result.explanation =
            "no entity scored above the acceptance threshold; the referenced entity has most likely been "
            "removed by a later operation";
        return result;
    }
    result.entity = scored.front().second;
    result.score = scored.front().first;
    if (scored.size() == 1) {
        result.status = ResolveStatus::Resolved;
        result.explanation = "one candidate matched";
        return result;
    }
    const double best = scored[0].first;
    const double runner_up = scored[1].first;
    // AN EXACT MATCH WINS OUTRIGHT, WHATEVER THE RUNNER-UP SCORED. The
    // margin below exists to choose between *approximations*: when
    // nothing matches the recorded name exactly, two similar candidates
    // really are too close to call and asking is better than guessing. A
    // score of one is a different situation -- every recorded trait
    // matched -- and no candidate that matched fewer of them has a better
    // claim to being the entity that was named, however close its score
    // came.
    //
    // Without this an L-bracket cannot carry a study. Its held face
    // scored 1.000000 against a runner-up of 0.873604, a gap of 12.6%
    // where the margin asks for 15%, so a support attached to that face
    // and resolved against *the very model it was captured from* came
    // back ambiguous. Simple boxes never showed it because their
    // runners-up score far lower.
    //
    // Two candidates both scoring one is a different thing again -- two
    // entities the recorded name genuinely cannot tell apart -- and that
    // stays ambiguous, which is the case the caution was written for.
    const bool exact = best >= 1.0 - 1e-9 && runner_up < 1.0 - 1e-9;
    if (exact || best - runner_up >= options.dominance_margin * best) {
        result.status = ResolveStatus::Resolved;
        result.explanation = exact ? "one candidate matched the recorded name exactly"
                                   : "best candidate scored " + std::to_string(best) +
                                         " against " + std::to_string(runner_up) +
                                         " for the runner-up";
        return result;
    }
    result.status = ResolveStatus::Ambiguous;
    result.explanation = "the best two candidates scored " + std::to_string(best) + " and " +
                         std::to_string(runner_up) + ", too close to choose between; a caller should ask "
                                                     "rather than guess";
    return result;
}

double ModelScale(const Model &model) {
    const Box3d bounds = model.Bounds();
    return bounds.IsEmpty() ? 1.0 : std::max(1e-12, bounds.Diagonal());
}

}  // namespace

EntityName CaptureFaceName(const Model &model, EntityId face_id, int generating_feature,
                           const std::string &role) {
    EntityName name;
    name.is_face = true;
    name.generating_feature = generating_feature;
    name.role = role;
    const Face *face = model.GetFace(face_id);
    if (face == nullptr) return name;
    const Surface *surface = model.SurfaceAt(face->surface);
    if (surface != nullptr) {
        name.surface_kind = surface->Kind();
        double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
        surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
        const double u = 0.5 * (u_lo + u_hi);
        const double v = 0.5 * (v_lo + v_hi);
        name.sample_point = surface->Point(u, v);
        name.sample_direction = model.FaceNormal(face_id, u, v);
    }
    name.loop_count = static_cast<int>(face->loops.size());
    name.edge_count = static_cast<int>(model.EdgesOfFace(face_id).size());
    name.neighbour_features = NeighbourFeaturesOfFace(model, face_id);
    return name;
}

EntityName CaptureEdgeName(const Model &model, EntityId edge_id, int generating_feature,
                           const std::string &role) {
    EntityName name;
    name.is_face = false;
    name.generating_feature = generating_feature;
    name.role = role;
    const Edge *edge = model.GetEdge(edge_id);
    if (edge == nullptr) return name;
    const Curve3 *curve = model.CurveAt(edge->curve);
    if (curve != nullptr) {
        name.curve_kind = curve->Kind();
        const double t = 0.5 * (edge->t_start + edge->t_end);
        name.sample_point = curve->Point(t);
        name.sample_direction = curve->Tangent(t);
    }
    name.edge_count = 1;
    name.neighbour_features = NeighbourFeaturesOfEdge(model, edge_id);
    return name;
}

ResolveResult ResolveFace(const Model &model, const EntityName &name, const NamingOptions &options) {
    std::vector<std::pair<EntityId, EntityName>> candidates;
    for (const Face &face : model.Faces()) {
        // The candidate's own name is captured with the *sought* name's
        // feature and role, so that those two signals compare equal only
        // when the candidate genuinely carries them -- which for a face
        // is recorded in Face::name until Part E gives features real ids.
        EntityName candidate = CaptureFaceName(model, face.id, name.generating_feature, face.name);
        candidates.push_back({face.id, candidate});
    }
    return ResolveAgainst(candidates, name, ModelScale(model), options);
}

ResolveResult ResolveEdge(const Model &model, const EntityName &name, const NamingOptions &options) {
    std::vector<std::pair<EntityId, EntityName>> candidates;
    for (const Edge &edge : model.Edges()) {
        EntityName candidate = CaptureEdgeName(model, edge.id, name.generating_feature, name.role);
        candidates.push_back({edge.id, candidate});
    }
    return ResolveAgainst(candidates, name, ModelScale(model), options);
}

}  // namespace cad
