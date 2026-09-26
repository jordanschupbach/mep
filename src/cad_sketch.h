#ifndef MEP_CAD_SKETCH_H
#define MEP_CAD_SKETCH_H

#include "cad_curve.h"
#include "cad_math.h"
#include "cad_naming.h"
#include "cad_topology.h"

#include <memory>
#include <string>
#include <vector>

// 2D sketch geometry on a plane (plans/CAD_FEM_PLAN.md Parts D.1 and D.4).
//
// THE ONE DESIGN DECISION EVERYTHING ELSE FOLLOWS FROM: points are the
// degrees of freedom, and entities are built out of shared points.
//
// An arc is a centre point and two endpoints, not a centre plus a radius
// plus two angles. The second form is the more obvious one and it is
// wrong for a sketcher, because the thing a user does most is join one
// curve's end to another's -- and in the second form that join is a
// constraint between a point and a *derived* quantity, whose derivative
// has to be pushed back through the derivation. In the first form it is
// two points being the same point, which is the simplest constraint there
// is, and dragging an endpoint is just moving a parameter.
//
// The price is that an arc's representation is redundant: three points
// are six numbers where an arc has five degrees of freedom. That missing
// equation is added explicitly, as an internal constraint keeping the two
// endpoints equidistant from the centre, and it is visible to the solver
// and to the degree-of-freedom count like any other. Making the
// redundancy explicit is what keeps the diagnosis in Part D.3 honest --
// an implicit invariant maintained on the side would show up there as a
// phantom degree of freedom that no drag handle corresponds to.
//
// A circle keeps its radius as a scalar rather than as a point on the
// rim, because a circle genuinely has no distinguished point, and
// inventing one would mean the solver could rotate it -- a degree of
// freedom that does not exist and that the diagnosis would have to report.
namespace cad {

using SketchId = int;
constexpr SketchId kNoSketchId = -1;

// The plane a sketch lives on.
//
// It can stand alone, or be attached to a face of a model. Attached, it
// is recomputed from that face on every rebuild, which is what makes a
// sketch drawn on a block's top face stay on the top face after the block
// gets taller. The face is remembered by Part B.4's persistent name
// rather than by index, for the same reason every other reference in this
// kernel is.
struct SketchPlane {
    Vec3d origin{0.0, 0.0, 0.0};
    Vec3d x_axis{1.0, 0.0, 0.0};
    Vec3d y_axis{0.0, 1.0, 0.0};

    Vec3d Normal() const { return x_axis.Cross(y_axis).Normalized(); }
    Vec3d ToWorld(const Vec2d &p) const { return origin + x_axis * p.x + y_axis * p.y; }
    Vec2d ToPlane(const Vec3d &p) const {
        const Vec3d d = p - origin;
        return Vec2d{d.Dot(x_axis), d.Dot(y_axis)};
    }

    bool attached = false;
    EntityName face;
    // How far along the face's normal the sketch plane sits. A sketch on
    // an offset plane is common enough (a boss standing off a face) that
    // it belongs here rather than being a separate datum.
    double offset = 0.0;
};

// Builds a plane from a point and a normal, choosing the in-plane x axis
// deterministically so that the same face always yields the same frame --
// otherwise every rebuild would renumber the sketch's own coordinates.
SketchPlane PlaneFromNormal(const Vec3d &origin, const Vec3d &normal);

// Attaches a plane to a planar face of `model`, capturing the name that
// will find that face again after a rebuild. Fails on a non-planar face.
bool AttachPlaneToFace(const Model &model, EntityId face, int generating_feature, const std::string &role,
                       double offset, SketchPlane *out);

// Re-resolves an attached plane against a rebuilt model. Returns the
// resolution status so a caller can tell "the face moved" from "the face
// is gone", which are different things to report to a user.
ResolveStatus ReattachPlane(const Model &model, SketchPlane *plane, const NamingOptions &options = {});

enum class SketchEntityKind {
    Point,
    Line,
    Arc,
    Circle,
    Ellipse,
    Spline,
};

struct SketchPoint {
    SketchId id = kNoSketchId;
    Vec2d position;
    // A fixed point contributes no parameters at all. That is how a
    // sketch is anchored: without at least one, every sketch is
    // under-constrained by the three degrees of freedom of a rigid
    // motion, and the solver would happily translate the whole thing.
    bool fixed = false;
};

struct SketchEntity {
    SketchId id = kNoSketchId;
    SketchEntityKind kind = SketchEntityKind::Line;
    // Construction geometry is real geometry that constraints may refer
    // to and that profile extraction ignores. A centreline is the obvious
    // case; so is a circle used only to keep three points concentric.
    bool construction = false;

    // Point references. What they mean, by kind:
    //   Point    [p]
    //   Line     [start, end]
    //   Arc      [centre, start, end]
    //   Circle   [centre]
    //   Ellipse  [centre]
    //   Spline   [c0 .. cn]  (control points)
    std::vector<SketchId> points;

    // Scalar degrees of freedom the entity owns itself. By kind:
    //   Circle   [radius]
    //   Ellipse  [major, minor, rotation]
    //   others   []
    std::vector<double> scalars;
    // Which of them the solver may not move. Empty means none are fixed.
    // The same idea as a fixed point and needed for the same reason: an
    // ellipse's axes are unbounded parameters, and a sketch that says
    // only "this point is on that ellipse" lets the solver satisfy it by
    // shrinking the ellipse onto the point -- which it will, because that
    // is a perfectly good solution of the system as stated.
    std::vector<bool> scalar_fixed;

    // An arc runs counter-clockwise from its start point to its end point
    // when true. Which of the two arcs between the endpoints is meant is
    // not derivable from the points, so it is stored.
    bool arc_ccw = true;

    // Spline only.
    int degree = 3;
    std::vector<double> knots;
};

// A sketch: a plane, points, entities, and the constraints between them.
// The constraints live here rather than in cad_constraint.h because they
// are part of the document; cad_constraint.h holds the solver that reads
// them.
enum class ConstraintKind {
    // Geometric.
    Coincident,      // points[0] == points[1]
    Horizontal,      // points[0], points[1] share a y   (or entities[0] is a line)
    Vertical,        // ... share an x
    Parallel,        // entities[0] || entities[1]        (lines)
    Perpendicular,   // entities[0] _|_ entities[1]       (lines)
    Tangent,         // line/circle, line/arc, circle/circle, circle/arc, arc/arc
    Equal,           // equal length (lines) or equal radius (circles and arcs)
    Concentric,      // entities share a centre
    Collinear,       // entities[1]'s endpoints lie on entities[0]'s line
    Symmetric,       // points[0], points[1] mirror across entities[0] (a line)
    PointOnObject,   // points[0] lies on entities[0]

    // Dimensional. `value` carries the dimension.
    Distance,
    HorizontalDistance,
    VerticalDistance,
    Angle,           // between two lines, in radians
    Radius,
    Diameter,

    // Internal, created automatically with an arc: its two endpoints stay
    // equidistant from its centre. See the note at the top of this file.
    ArcRadius,
};

// A stable name for each kind, for the Lua and agent-RPC surfaces and
// for anything that serialises a sketch. Kept next to the enum so that
// adding a kind without naming it is a compile error rather than a
// silently unreachable API.
const char *ConstraintKindName(ConstraintKind kind);
bool ConstraintKindFromName(const std::string &name, ConstraintKind *out);

const char *SketchEntityKindName(SketchEntityKind kind);
const char *SketchStatusName(int status);

struct SketchConstraint {
    SketchId id = kNoSketchId;
    ConstraintKind kind = ConstraintKind::Coincident;
    std::vector<SketchId> points;
    std::vector<SketchId> entities;
    double value = 0.0;
    // A reference dimension measures the sketch rather than constraining
    // it: it contributes no residual and no rank, and its value is
    // rewritten from the geometry after each solve.
    bool driving = true;
};

class Sketch {
public:
    Sketch();
    explicit Sketch(const SketchPlane &plane);

    const SketchPlane &Plane() const { return plane_; }
    SketchPlane &Plane() { return plane_; }

    // --- Construction ---------------------------------------------------
    SketchId AddPoint(const Vec2d &position, bool fixed = false);
    SketchId AddLine(const Vec2d &start, const Vec2d &end, bool construction = false);
    SketchId AddLineFromPoints(SketchId start, SketchId end, bool construction = false);
    // The arc through `start` and `end` about `centre`. The two endpoints
    // are placed exactly on the circle of the larger of the two radii, so
    // the internal ArcRadius constraint starts satisfied rather than
    // having to pull the sketch straight on the first solve.
    SketchId AddArc(const Vec2d &centre, const Vec2d &start, const Vec2d &end, bool ccw = true,
                    bool construction = false);
    SketchId AddCircle(const Vec2d &centre, double radius, bool construction = false);
    SketchId AddEllipse(const Vec2d &centre, double major, double minor, double rotation,
                        bool construction = false);
    SketchId AddSpline(const std::vector<Vec2d> &control_points, int degree = 3, bool construction = false);

    SketchId AddConstraint(const SketchConstraint &constraint);
    SketchId Constrain(ConstraintKind kind, const std::vector<SketchId> &points,
                       const std::vector<SketchId> &entities, double value = 0.0);
    bool RemoveConstraint(SketchId id);
    /**
     * Removes an entity, and with it the points listed in `orphans`.
     *
     * The caller decides which points die rather than this working it out,
     * because "used only by this entity" is a question about the whole
     * sketch and the answer differs depending on whether constraints
     * count as a use. Constraints naming any of them must already be
     * gone: the solver refuses to evaluate a sketch whose constraint
     * names geometry that is not there, so one stale reference would make
     * the whole sketch unsolvable rather than merely leaving a stray.
     */
    bool RemoveEntity(SketchId entity, const std::vector<SketchId> &orphans);

    // --- Access ---------------------------------------------------------
    const std::vector<SketchPoint> &Points() const { return points_; }
    const std::vector<SketchEntity> &Entities() const { return entities_; }
    const std::vector<SketchConstraint> &Constraints() const { return constraints_; }

    const SketchPoint *GetPoint(SketchId id) const;
    SketchPoint *GetPoint(SketchId id);
    const SketchEntity *GetEntity(SketchId id) const;
    SketchEntity *GetEntity(SketchId id);
    const SketchConstraint *GetConstraint(SketchId id) const;
    SketchConstraint *GetConstraint(SketchId id);

    // The sketch's anchor: a fixed point at the plane origin, created by
    // the constructor. Every sketch has one, because a sketch without one
    // is under-constrained by a rigid motion no matter what else is said
    // about it.
    SketchId Origin() const { return origin_; }

    void SetPointPosition(SketchId id, const Vec2d &position);
    void SetPointFixed(SketchId id, bool fixed);
    void SetScalarFixed(SketchId entity, int index, bool fixed);
    bool IsScalarFixed(SketchId entity, int index) const;

    // --- Derived geometry ------------------------------------------------
    //
    // The entity as a curve in the sketch plane's own coordinates (a
    // Curve3 confined to z = 0, which is the p-curve convention Part B.2
    // uses, so these go straight into a planar arrangement). Returns null
    // for a Point, which is not a curve, and for a degenerate entity.
    std::shared_ptr<const Curve3> Curve(SketchId entity) const;
    // The same curve in world space.
    std::shared_ptr<const Curve3> WorldCurve(SketchId entity) const;

    // Where an entity's centre is, for the kinds that have one.
    bool CentrePoint(SketchId entity, SketchId *out) const;
    // The radius of a circle or an arc.
    bool Radius(SketchId entity, double *out) const;

    // --- Profile extraction (Part D.4) -----------------------------------
    //
    // The closed regions the non-construction geometry divides the plane
    // into, with nested regions reported as holes rather than as separate
    // profiles. That distinction is the whole point: an extrude wants "the
    // region between these curves", and a washer drawn as two circles is
    // one profile with a hole, not two profiles.
    struct Profile {
        // Entities bounding the outer loop and each hole, in traversal
        // order, each with the parameter range of the piece used. An
        // entity appears more than once when other curves cut it.
        struct Piece {
            SketchId entity = kNoSketchId;
            double t0 = 0.0;
            double t1 = 1.0;
        };
        std::vector<Piece> outer;
        std::vector<std::vector<Piece>> holes;
        // Signed area, holes subtracted.
        double area = 0.0;
        Vec2d interior;
    };
    // Returns false with an explanation when the geometry cannot be
    // arranged at all; an empty profile list with no error means the
    // sketch has no closed region, which is a legitimate state to be in
    // while drawing.
    bool ExtractProfiles(std::vector<Profile> *out, std::string *error) const;

    // --- For the document layer (Part F.1) --------------------------------
    //
    // Reading a saved sketch has to restore it *exactly*, ids included,
    // because its constraints refer to its points and entities by id and
    // a sketch whose constraint names geometry that is not there is not
    // solvable at all. Replaying the Add calls in order would very nearly
    // work and would break the first time a kind allocated an id
    // differently, so the document layer puts the arrays back directly
    // and says so here rather than pretending it went through the front
    // door.
    void Restore(const SketchPlane &plane, std::vector<SketchPoint> points,
                 std::vector<SketchEntity> entities, std::vector<SketchConstraint> constraints,
                 SketchId origin, SketchId next_id);
    SketchId PeekNextId() const { return next_id_; }

private:
    SketchId NextId() { return next_id_++; }

    SketchPlane plane_;
    std::vector<SketchPoint> points_;
    std::vector<SketchEntity> entities_;
    std::vector<SketchConstraint> constraints_;
    SketchId origin_ = kNoSketchId;
    SketchId next_id_ = 0;
};

}  // namespace cad

#endif
