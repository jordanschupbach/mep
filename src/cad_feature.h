#ifndef MEP_CAD_FEATURE_H
#define MEP_CAD_FEATURE_H

#include "cad_boolean.h"
#include "cad_naming.h"
#include "cad_sketch.h"
#include "cad_topology.h"

#include <memory>
#include <string>
#include <vector>

// Parametric feature modeling (plans/CAD_FEM_PLAN.md Parts E.1 and E.2).
//
// A part is not geometry. It is an ordered list of operations -- sketch a
// rectangle, extrude it 10, sketch a circle on the top face, cut it
// through -- and the geometry is what you get by replaying them. Changing
// the 10 to a 14 and replaying is the whole point, and it is why the tree
// rather than the solid is what gets stored and undone.
//
// WHAT REBUILD ACTUALLY GUARANTEES, and what it does not. Replaying from
// the first dirty feature is straightforward; what is not is that a
// feature further down still means what it meant. A cut positioned
// against "that face" has to find that face again in a solid that has
// changed shape, which is Part B.4's problem and is solved there by
// naming rather than by index. When a reference cannot be resolved, the
// feature fails -- explicitly, with a message naming what was lost --
// and the features after it are still attempted. A tree that stops dead
// at the first failure is far more annoying than one that tells you which
// three features are broken.
//
// THE PROFILE-TO-SOLID CONSTRUCTION is the substance of E.2, and it is
// the same shape for every sketch-based feature: each piece of the
// profile's boundary sweeps into one side face, the profile itself caps
// each end, and the orientation conventions are what make the result a
// solid rather than a bag of faces. Those conventions are spelled out at
// the construction in cad_feature.cpp, because getting one of them
// backwards produces a body that validates structurally and has a
// negative volume.
namespace cad {

enum class FeatureKind {
    // A sketch, held as a feature so that it takes part in the tree: a
    // sketch that changes marks everything built on it dirty.
    Sketch,
    Extrude,
    Revolve,
    Loft,
    Sweep,
};

// What a feature does to the body already there.
enum class FeatureCombine {
    // Leaves the previous body alone and adds a second one. The result
    // holds both, which is what a multi-body part is.
    NewBody,
    Add,
    Cut,
    Intersect,
};

// How far an extrude goes.
enum class ExtrudeEnd {
    // `distance` from the sketch plane, one way.
    Blind,
    // `distance` in total, half of it each way from the sketch plane --
    // so a symmetric extrude of the same `distance` as a blind one makes
    // a solid of the same thickness, centred instead of sitting on top.
    // (This comment used to say "each way", which is not what the
    // rebuild does and cost an afternoon of parts coming out half the
    // size they were asked for.)
    Symmetric,
    // Far enough to pass through anything it might meet. Only meaningful
    // for a Cut or an Intersect, and resolved from the target body's own
    // bounding box rather than from a large constant.
    ThroughAll,
};

struct Feature {
    int id = -1;
    FeatureKind kind = FeatureKind::Extrude;
    std::string name;
    // A suppressed feature is skipped on rebuild and its inputs are still
    // checked, so un-suppressing it does not surprise anyone.
    bool suppressed = false;

    // --- Inputs --------------------------------------------------------
    // The Sketch feature supplying the profile, and which of that
    // sketch's profiles to use. -1 for the profile of largest area, which
    // is what a single closed outline with holes comes out as and so is
    // what is wanted almost always.
    int sketch = -1;
    int profile_index = -1;

    FeatureCombine combine = FeatureCombine::NewBody;

    // --- Extrude -------------------------------------------------------
    ExtrudeEnd end = ExtrudeEnd::Blind;
    double distance = 1.0;
    // Extrude against the sketch plane's normal instead of along it.
    bool reverse = false;

    // --- Revolve -------------------------------------------------------
    // The axis, as a construction line in the sketch. Revolving about a
    // line the user drew keeps the axis parametric: move the line and the
    // revolve follows, which an axis given as two numbers would not.
    int axis_entity = kNoSketchId;
    double angle = kTwoPi;

    // --- Loft and sweep ------------------------------------------------
    // Sketch features whose profiles are the sections, in order.
    std::vector<int> sections;
    // A Sketch feature holding the spine, and which of its entities is
    // the spine curve. The spine is one entity rather than a profile
    // because it need not close, and usually does not.
    int spine = -1;
    int spine_entity = kNoSketchId;
    // Sections generated along the spine. More follows a curved spine
    // more closely and costs a ruled surface per piece per section.
    int spine_sections = 12;
    // Radians of twist from start to end, and the scale at the end.
    double twist = 0.0;
    double end_scale = 1.0;

    // --- The sketch itself ---------------------------------------------
    // Only for FeatureKind::Sketch. Held by value: a feature tree is a
    // document, and a rebuild must not depend on anything outside it.
    Sketch sketch_geometry;
};

// What one feature produced.
struct FeatureResult {
    bool ok = false;
    std::string error;
    // The bodies in existence after this feature. A part is usually one,
    // but FeatureCombine::NewBody makes more.
    std::vector<EntityId> bodies;
    double volume = 0.0;
};

class FeatureTree {
public:
    FeatureTree();

    // --- Building the tree ----------------------------------------------
    int AddSketch(const Sketch &sketch, const std::string &name);
    int AddFeature(const Feature &feature);
    bool RemoveFeature(int id);
    const Feature *Get(int id) const;
    Feature *GetMutable(int id);
    const std::vector<Feature> &Features() const { return features_; }

    // Marks a feature and everything after it as needing a rebuild.
    // Called for you by GetMutable; exposed for a caller that changed a
    // feature some other way.
    void MarkDirty(int id);

    // --- Rebuilding -------------------------------------------------------
    //
    // Replays from the first dirty feature. Returns false if any feature
    // failed, with every failure available through ResultOf -- the return
    // value says "something is broken", not "nothing was built".
    bool Rebuild(std::string *error);

    const FeatureResult *ResultOf(int id) const;
    // The geometry as of the last feature that produced any.
    const Model &Result() const { return model_; }
    std::vector<EntityId> Bodies() const { return bodies_; }
    // Total volume of every body, by tessellation. The cheapest thing to
    // assert about a rebuild that is worth asserting.
    double Volume() const;

private:
    struct Step {
        Model model;
        std::vector<EntityId> bodies;
    };

    bool Evaluate(const Feature &feature, const Model &incoming, const std::vector<EntityId> &incoming_bodies,
                  Step *out, std::string *error) const;

    std::vector<Feature> features_;
    std::vector<FeatureResult> results_;
    std::vector<Step> steps_;
    int next_id_ = 0;
    int first_dirty_ = 0;
    Model model_;
    std::vector<EntityId> bodies_;
};

// --- The construction underneath, usable on its own --------------------
//
// Exposed because they are what the features are, and because testing
// them directly is how the orientation conventions get pinned down
// without a whole tree standing in the way.

// Sweeps a sketch profile along `direction` for `distance`, building a
// closed solid. `direction` need not be the sketch normal -- an oblique
// extrude is the same construction.
bool ExtrudeProfile(const Sketch &sketch, const Sketch::Profile &profile, const Vec3d &direction,
                    double distance, Model *out, EntityId *out_body, std::string *error);

// Revolves a sketch profile about an axis given in sketch coordinates.
// `angle` may be a full turn, in which case there are no cap faces and
// the side faces carry a seam instead.
bool RevolveProfile(const Sketch &sketch, const Sketch::Profile &profile, const Vec2d &axis_point,
                    const Vec2d &axis_direction, double angle, Model *out, EntityId *out_body,
                    std::string *error);

// Lofts between the outer loops of several profiles, each on its own
// sketch plane, capping the first and last.
bool LoftProfiles(const std::vector<const Sketch *> &sketches, const std::vector<Sketch::Profile> &profiles,
                  Model *out, EntityId *out_body, std::string *error);

// Sweeps a sketch profile along a spine curve, with optional twist (in
// radians, applied linearly along the spine) and scaling (1 at the start,
// `end_scale` at the end). The frame is rotation-minimizing, so the
// profile carries no twist beyond the one asked for.
bool SweepProfile(const Sketch &sketch, const Sketch::Profile &profile, const Curve3 &spine, int sections,
                  double twist, double end_scale, Model *out, EntityId *out_body, std::string *error);

// --- Naming an edge by the faces it lies between -----------------------
//
// Part B.4 will not guess: it reports a reference it cannot pin down as
// ambiguous rather than picking one, deliberately, because resolving
// wrongly costs a silently incorrect part. A box's twelve edges are
// alike in everything that scoring looks at, so a direct reference to
// one of them is ambiguous against the rest and always will be. Its two
// faces are not alike, so naming them instead is both stronger and the
// usual practice -- and it is what Part E.3's fillets will refer to
// their edges by when they are built.

// The edge two named faces share, resolved against a rebuilt model.
bool ResolveEdgeBetweenFaces(const Model &model, EntityId body, const EntityName &face_a,
                             const EntityName &face_b, EntityId *out, std::string *error);

// Names an edge by the two faces it lies between, for a feature to keep.
bool CaptureEdgeBetweenFaces(const Model &model, EntityId edge, int generating_feature, EntityName *face_a,
                             EntityName *face_b);

// --- Blends: fillets and chamfers (Part E.3) ---------------------------
//
// TRIM-AND-STITCH, NOT A BOOLEAN. The obvious way to round an edge is to
// build the material the fillet removes as a solid and subtract it, and
// that way does not work. A corner cutter is made almost entirely of
// degeneracies: its rounded face is *tangent* to both faces it trims
// rather than crossing them, and its remaining faces end exactly *on*
// them. Every one of those is the coincident-face case the boolean in
// Part C.4 does not handle, so the result is right when the arithmetic
// happens to land on the tolerant side and wrong when it does not --
// which is exactly what a sweep of box sizes showed when it was tried.
// That attempt and its post-mortem are written up in
// plans/CAD_FEM_PLAN.md.
//
// So a blend is done as what it actually is: a local edit. The two faces
// along the edge are trimmed back to the line where the rolling ball
// touches them, the blend surface is stitched in between, and the faces
// at the edge's two ends get an arc where they used to have a corner.
// Nothing else in the body is touched, nothing is classified, and no
// surface is ever intersected with one it is tangent to.
//
// WHAT IT HANDLES, precisely -- a straight edge between two planar faces,
// whose two end faces are planar and square to the edge. That covers the
// edges of anything prismatic, which is what a first blend is for, and
// every case outside it is refused by name rather than attempted. The
// general case (curved edges, curved neighbours, blends that run into
// each other at a corner) needs surface-surface intersection between the
// blend surface and its neighbours, which is Part C.3's marcher applied
// to a surface that does not exist yet; that is a later piece of work and
// not a tolerance away from this one.
//
// Concave edges work, and by the same code: the ball rolls in the void
// rather than on the solid, so the centre is offset to the other side and
// the blend surface faces inward. That is two signs, both named below.

enum class BlendKind {
    // A rolling-ball fillet: the blend face is a cylinder of the given
    // radius, tangent to both faces.
    Fillet,
    // A flat chamfer: the blend face is a plane through the two points
    // the fillet would have been tangent at, so `radius` is the setback
    // along each face.
    Chamfer,
};

struct BlendOptions {
    BlendKind kind = BlendKind::Fillet;
    // The fillet radius, or the chamfer's setback along each face.
    double radius = 1.0;
    Tolerance tolerance;
};

// Blends one edge of one body, writing the whole result into `out` as a
// fresh model holding a single body. Other bodies in `model` are not
// copied; a caller with several should blend each.
bool BlendEdge(const Model &model, EntityId body, EntityId edge, const BlendOptions &options, Model *out,
               EntityId *out_body, std::string *error);

// Blends several edges in turn. Each blend rebuilds the body, so the
// edge ids of the later ones do not survive the earlier ones -- they are
// carried across by the face pair each lies between, which is what
// CaptureEdgeBetweenFaces is for and is why blends refer to edges that
// way in the first place. An edge that two earlier blends have both
// eaten into no longer lies between the same two faces and is reported
// by name rather than silently skipped.
bool BlendEdges(const Model &model, EntityId body, const std::vector<EntityId> &edges,
                const BlendOptions &options, Model *out, EntityId *out_body, std::string *error);

// The profile a feature means: the one named by `profile_index`, or the
// largest if that is -1.
bool SelectProfile(const Sketch &sketch, int profile_index, Sketch::Profile *out, std::string *error);

}  // namespace cad

#endif
