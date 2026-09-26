#include "cad_sketch.h"

#include "cad_arrangement.h"
#include "cad_surface.h"

#include <algorithm>
#include <cmath>

namespace cad {
namespace {

Vec3d Lift(const Vec2d &p) { return Vec3d{p.x, p.y, 0.0}; }

// A frame's x axis, chosen from the normal alone and deterministically.
// Which axis it starts from matters: taking a fixed one means a normal
// nearly parallel to it gives a frame that swings wildly for a tiny
// change in the normal, and a sketch attached to such a face would appear
// to spin on rebuild. The least-aligned coordinate axis has no such
// neighbourhood.
Vec3d StableTangent(const Vec3d &normal) {
    const double ax = std::fabs(normal.x);
    const double ay = std::fabs(normal.y);
    const double az = std::fabs(normal.z);
    Vec3d seed{1.0, 0.0, 0.0};
    if (ay <= ax && ay <= az) {
        seed = Vec3d{0.0, 1.0, 0.0};
    } else if (az <= ax && az <= ay) {
        seed = Vec3d{0.0, 0.0, 1.0};
    }
    const Vec3d tangent = seed - normal * seed.Dot(normal);
    return tangent.Normalized();
}

}  // namespace

SketchPlane PlaneFromNormal(const Vec3d &origin, const Vec3d &normal) {
    SketchPlane plane;
    plane.origin = origin;
    const Vec3d unit = normal.Normalized();
    plane.x_axis = StableTangent(unit);
    plane.y_axis = unit.Cross(plane.x_axis).Normalized();
    return plane;
}

bool AttachPlaneToFace(const Model &model, EntityId face_id, int generating_feature, const std::string &role,
                       double offset, SketchPlane *out) {
    const Face *face = model.GetFace(face_id);
    if (face == nullptr) return false;
    const Surface *surface = model.SurfaceAt(face->surface);
    if (surface == nullptr || surface->Kind() != SurfaceKind::Plane) return false;
    double u0 = 0.0;
    double u1 = 0.0;
    double v0 = 0.0;
    double v1 = 0.0;
    surface->Domain(&u0, &u1, &v0, &v1);
    const double u = 0.5 * (u0 + u1);
    const double v = 0.5 * (v0 + v1);
    Vec3d normal = surface->Normal(u, v);
    // The face's orientation, not the surface's: a Reversed face's
    // material is on the other side, and a sketch attached to it should
    // face out of the solid like every other consumer of that face.
    if (face->orientation == Orientation::Reversed) normal = -normal;

    *out = PlaneFromNormal(surface->Point(u, v) + normal * offset, normal);
    out->attached = true;
    out->offset = offset;
    out->face = CaptureFaceName(model, face_id, generating_feature, role);
    return true;
}

ResolveStatus ReattachPlane(const Model &model, SketchPlane *plane, const NamingOptions &options) {
    if (plane == nullptr || !plane->attached) return ResolveStatus::Lost;
    const ResolveResult found = ResolveFace(model, plane->face, options);
    if (found.status != ResolveStatus::Resolved) return found.status;
    SketchPlane rebuilt;
    if (!AttachPlaneToFace(model, found.entity, plane->face.generating_feature, plane->face.role, plane->offset,
                           &rebuilt)) {
        return ResolveStatus::Lost;
    }
    rebuilt.face = plane->face;
    *plane = rebuilt;
    return ResolveStatus::Resolved;
}

// --- Names -------------------------------------------------------------

namespace {
struct KindName {
    ConstraintKind kind;
    const char *name;
};
// Every kind, once. The switch in ConstraintKindName below has no default
// case, so a kind added to the enum and forgotten here fails to compile.
constexpr KindName kConstraintNames[] = {
    {ConstraintKind::Coincident, "coincident"},
    {ConstraintKind::Horizontal, "horizontal"},
    {ConstraintKind::Vertical, "vertical"},
    {ConstraintKind::Parallel, "parallel"},
    {ConstraintKind::Perpendicular, "perpendicular"},
    {ConstraintKind::Tangent, "tangent"},
    {ConstraintKind::Equal, "equal"},
    {ConstraintKind::Concentric, "concentric"},
    {ConstraintKind::Collinear, "collinear"},
    {ConstraintKind::Symmetric, "symmetric"},
    {ConstraintKind::PointOnObject, "point_on_object"},
    {ConstraintKind::Distance, "distance"},
    {ConstraintKind::HorizontalDistance, "horizontal_distance"},
    {ConstraintKind::VerticalDistance, "vertical_distance"},
    {ConstraintKind::Angle, "angle"},
    {ConstraintKind::Radius, "radius"},
    {ConstraintKind::Diameter, "diameter"},
    {ConstraintKind::ArcRadius, "arc_radius"},
};
}  // namespace

const char *ConstraintKindName(ConstraintKind kind) {
    switch (kind) {
        case ConstraintKind::Coincident: return "coincident";
        case ConstraintKind::Horizontal: return "horizontal";
        case ConstraintKind::Vertical: return "vertical";
        case ConstraintKind::Parallel: return "parallel";
        case ConstraintKind::Perpendicular: return "perpendicular";
        case ConstraintKind::Tangent: return "tangent";
        case ConstraintKind::Equal: return "equal";
        case ConstraintKind::Concentric: return "concentric";
        case ConstraintKind::Collinear: return "collinear";
        case ConstraintKind::Symmetric: return "symmetric";
        case ConstraintKind::PointOnObject: return "point_on_object";
        case ConstraintKind::Distance: return "distance";
        case ConstraintKind::HorizontalDistance: return "horizontal_distance";
        case ConstraintKind::VerticalDistance: return "vertical_distance";
        case ConstraintKind::Angle: return "angle";
        case ConstraintKind::Radius: return "radius";
        case ConstraintKind::Diameter: return "diameter";
        case ConstraintKind::ArcRadius: return "arc_radius";
    }
    return "unknown";
}

bool ConstraintKindFromName(const std::string &name, ConstraintKind *out) {
    for (const KindName &entry : kConstraintNames) {
        if (name == entry.name) {
            *out = entry.kind;
            return true;
        }
    }
    return false;
}

const char *SketchEntityKindName(SketchEntityKind kind) {
    switch (kind) {
        case SketchEntityKind::Point: return "point";
        case SketchEntityKind::Line: return "line";
        case SketchEntityKind::Arc: return "arc";
        case SketchEntityKind::Circle: return "circle";
        case SketchEntityKind::Ellipse: return "ellipse";
        case SketchEntityKind::Spline: return "spline";
    }
    return "unknown";
}

// Takes an int rather than the enum so cad_constraint.h need not be
// included here: the status is produced by the solver, and this header is
// the one a serialiser already has.
const char *SketchStatusName(int status) {
    switch (status) {
        case 0: return "solved";
        case 1: return "under_constrained";
        case 2: return "redundant";
        case 3: return "conflicting";
        case 4: return "not_converged";
        default: return "unknown";
    }
}

// --- Sketch ------------------------------------------------------------

Sketch::Sketch() : Sketch(SketchPlane{}) {}

Sketch::Sketch(const SketchPlane &plane) : plane_(plane) {
    origin_ = AddPoint(Vec2d{0.0, 0.0}, true);
}

void Sketch::Restore(const SketchPlane &plane, std::vector<SketchPoint> points,
                     std::vector<SketchEntity> entities, std::vector<SketchConstraint> constraints,
                     SketchId origin, SketchId next_id) {
    plane_ = plane;
    points_ = std::move(points);
    entities_ = std::move(entities);
    constraints_ = std::move(constraints);
    origin_ = origin;
    next_id_ = next_id;
}

SketchId Sketch::AddPoint(const Vec2d &position, bool fixed) {
    SketchPoint point;
    point.id = NextId();
    point.position = position;
    point.fixed = fixed;
    points_.push_back(point);
    return point.id;
}

SketchId Sketch::AddLine(const Vec2d &start, const Vec2d &end, bool construction) {
    return AddLineFromPoints(AddPoint(start), AddPoint(end), construction);
}

SketchId Sketch::AddLineFromPoints(SketchId start, SketchId end, bool construction) {
    SketchEntity entity;
    entity.id = NextId();
    entity.kind = SketchEntityKind::Line;
    entity.construction = construction;
    entity.points = {start, end};
    entities_.push_back(entity);
    return entity.id;
}

SketchId Sketch::AddArc(const Vec2d &centre, const Vec2d &start, const Vec2d &end, bool ccw,
                        bool construction) {
    // Both endpoints are put on one circle before the arc is created, so
    // the internal ArcRadius constraint is satisfied from the outset. An
    // arc created already violating its own invariant would be pulled
    // into shape by the first solve, moving geometry the user placed
    // deliberately and for no reason they could see.
    const double radius = std::max((start - centre).Length(), (end - centre).Length());
    auto on_circle = [&](const Vec2d &p) {
        const Vec2d offset = p - centre;
        const double length = offset.Length();
        if (length <= 0.0) return centre + Vec2d{radius, 0.0};
        return centre + offset * (radius / length);
    };
    SketchEntity entity;
    entity.id = NextId();
    entity.kind = SketchEntityKind::Arc;
    entity.construction = construction;
    entity.arc_ccw = ccw;
    entity.points = {AddPoint(centre), AddPoint(on_circle(start)), AddPoint(on_circle(end))};
    entities_.push_back(entity);

    SketchConstraint internal;
    internal.kind = ConstraintKind::ArcRadius;
    internal.entities = {entity.id};
    AddConstraint(internal);
    return entity.id;
}

SketchId Sketch::AddCircle(const Vec2d &centre, double radius, bool construction) {
    SketchEntity entity;
    entity.id = NextId();
    entity.kind = SketchEntityKind::Circle;
    entity.construction = construction;
    entity.points = {AddPoint(centre)};
    entity.scalars = {radius};
    entities_.push_back(entity);
    return entity.id;
}

SketchId Sketch::AddEllipse(const Vec2d &centre, double major, double minor, double rotation,
                            bool construction) {
    SketchEntity entity;
    entity.id = NextId();
    entity.kind = SketchEntityKind::Ellipse;
    entity.construction = construction;
    entity.points = {AddPoint(centre)};
    entity.scalars = {major, minor, rotation};
    entities_.push_back(entity);
    return entity.id;
}

SketchId Sketch::AddSpline(const std::vector<Vec2d> &control_points, int degree, bool construction) {
    SketchEntity entity;
    entity.id = NextId();
    entity.kind = SketchEntityKind::Spline;
    entity.construction = construction;
    entity.degree = std::max(1, std::min(degree, static_cast<int>(control_points.size()) - 1));
    for (const Vec2d &p : control_points) entity.points.push_back(AddPoint(p));
    // A clamped uniform knot vector. It is stored rather than recomputed
    // so that a spline keeps its shape when control points are added or
    // moved by the solver -- and so that two sketches that look the same
    // are the same.
    const int n = static_cast<int>(control_points.size()) - 1;
    const int p = entity.degree;
    const int m = n + p + 1;
    entity.knots.assign(static_cast<std::size_t>(m) + 1, 0.0);
    for (int i = 0; i <= m; ++i) {
        if (i <= p) {
            entity.knots[static_cast<std::size_t>(i)] = 0.0;
        } else if (i >= n + 1) {
            entity.knots[static_cast<std::size_t>(i)] = 1.0;
        } else {
            entity.knots[static_cast<std::size_t>(i)] =
                static_cast<double>(i - p) / static_cast<double>(n - p + 1);
        }
    }
    entities_.push_back(entity);
    return entity.id;
}

SketchId Sketch::AddConstraint(const SketchConstraint &constraint) {
    SketchConstraint copy = constraint;
    copy.id = NextId();
    constraints_.push_back(copy);
    return copy.id;
}

SketchId Sketch::Constrain(ConstraintKind kind, const std::vector<SketchId> &points,
                           const std::vector<SketchId> &entities, double value) {
    SketchConstraint constraint;
    constraint.kind = kind;
    constraint.points = points;
    constraint.entities = entities;
    constraint.value = value;
    return AddConstraint(constraint);
}

bool Sketch::RemoveConstraint(SketchId id) {
    const auto found = std::find_if(constraints_.begin(), constraints_.end(),
                                    [id](const SketchConstraint &c) { return c.id == id; });
    if (found == constraints_.end()) return false;
    constraints_.erase(found);
    return true;
}

bool Sketch::RemoveEntity(SketchId entity_id, const std::vector<SketchId> &orphans) {
    const auto found = std::find_if(entities_.begin(), entities_.end(),
                                    [entity_id](const SketchEntity &e) { return e.id == entity_id; });
    if (found == entities_.end()) return false;
    entities_.erase(found);
    // The origin is never removed even if asked for: it is the sketch's
    // anchor, and a sketch without one is free to translate and rotate
    // however well constrained everything else in it is.
    points_.erase(std::remove_if(points_.begin(), points_.end(),
                                 [&](const SketchPoint &p) {
                                     if (p.id == origin_) return false;
                                     return std::find(orphans.begin(), orphans.end(), p.id) != orphans.end();
                                 }),
                  points_.end());
    return true;
}

const SketchPoint *Sketch::GetPoint(SketchId id) const {
    for (const SketchPoint &p : points_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}
SketchPoint *Sketch::GetPoint(SketchId id) {
    return const_cast<SketchPoint *>(static_cast<const Sketch *>(this)->GetPoint(id));
}
const SketchEntity *Sketch::GetEntity(SketchId id) const {
    for (const SketchEntity &e : entities_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}
SketchEntity *Sketch::GetEntity(SketchId id) {
    return const_cast<SketchEntity *>(static_cast<const Sketch *>(this)->GetEntity(id));
}
const SketchConstraint *Sketch::GetConstraint(SketchId id) const {
    for (const SketchConstraint &c : constraints_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}
SketchConstraint *Sketch::GetConstraint(SketchId id) {
    return const_cast<SketchConstraint *>(static_cast<const Sketch *>(this)->GetConstraint(id));
}

void Sketch::SetPointPosition(SketchId id, const Vec2d &position) {
    if (SketchPoint *point = GetPoint(id); point != nullptr) point->position = position;
}

void Sketch::SetPointFixed(SketchId id, bool fixed) {
    if (SketchPoint *point = GetPoint(id); point != nullptr) point->fixed = fixed;
}

void Sketch::SetScalarFixed(SketchId entity_id, int index, bool fixed) {
    SketchEntity *entity = GetEntity(entity_id);
    if (entity == nullptr || index < 0 || static_cast<std::size_t>(index) >= entity->scalars.size()) return;
    entity->scalar_fixed.resize(entity->scalars.size(), false);
    entity->scalar_fixed[static_cast<std::size_t>(index)] = fixed;
}

bool Sketch::IsScalarFixed(SketchId entity_id, int index) const {
    const SketchEntity *entity = GetEntity(entity_id);
    if (entity == nullptr || index < 0) return false;
    if (static_cast<std::size_t>(index) >= entity->scalar_fixed.size()) return false;
    return entity->scalar_fixed[static_cast<std::size_t>(index)];
}

bool Sketch::CentrePoint(SketchId entity_id, SketchId *out) const {
    const SketchEntity *entity = GetEntity(entity_id);
    if (entity == nullptr || entity->points.empty()) return false;
    switch (entity->kind) {
        case SketchEntityKind::Arc:
        case SketchEntityKind::Circle:
        case SketchEntityKind::Ellipse:
            *out = entity->points[0];
            return true;
        default:
            return false;
    }
}

bool Sketch::Radius(SketchId entity_id, double *out) const {
    const SketchEntity *entity = GetEntity(entity_id);
    if (entity == nullptr) return false;
    if (entity->kind == SketchEntityKind::Circle) {
        if (entity->scalars.empty()) return false;
        *out = entity->scalars[0];
        return true;
    }
    if (entity->kind == SketchEntityKind::Arc) {
        const SketchPoint *centre = GetPoint(entity->points[0]);
        const SketchPoint *start = GetPoint(entity->points[1]);
        if (centre == nullptr || start == nullptr) return false;
        *out = (start->position - centre->position).Length();
        return true;
    }
    return false;
}

std::shared_ptr<const Curve3> Sketch::Curve(SketchId entity_id) const {
    const SketchEntity *entity = GetEntity(entity_id);
    if (entity == nullptr) return nullptr;
    auto position = [&](std::size_t k) -> Vec2d {
        const SketchPoint *point = GetPoint(entity->points[k]);
        return point != nullptr ? point->position : Vec2d{};
    };
    switch (entity->kind) {
        case SketchEntityKind::Point:
            return nullptr;
        case SketchEntityKind::Line: {
            if (entity->points.size() < 2) return nullptr;
            const Vec2d a = position(0);
            const Vec2d b = position(1);
            if ((b - a).Length() <= 0.0) return nullptr;
            return std::make_shared<Line3>(Line3::FromPoints(Lift(a), Lift(b)));
        }
        case SketchEntityKind::Arc: {
            if (entity->points.size() < 3) return nullptr;
            const Vec2d centre = position(0);
            const Vec2d start = position(1);
            const Vec2d end = position(2);
            const double radius = (start - centre).Length();
            if (radius <= 0.0) return nullptr;
            double a0 = std::atan2(start.y - centre.y, start.x - centre.x);
            double a1 = std::atan2(end.y - centre.y, end.x - centre.x);
            // The curve always runs in increasing parameter, so a
            // clockwise arc is expressed by swapping the endpoints and
            // reversing -- Circle3 has no notion of direction beyond its
            // own frame, and giving it a negative sweep would make its
            // domain run backwards, which every consumer here assumes it
            // does not.
            if (!entity->arc_ccw) std::swap(a0, a1);
            while (a1 <= a0) a1 += kTwoPi;
            auto arc = std::make_shared<Circle3>(Lift(centre), Vec3d{1.0, 0.0, 0.0}, Vec3d{0.0, 1.0, 0.0},
                                                 radius, a0, a1);
            if (entity->arc_ccw) return arc;
            std::shared_ptr<Curve3> reversed(arc->Clone().release());
            reversed->Reverse();
            return reversed;
        }
        case SketchEntityKind::Circle: {
            if (entity->points.empty() || entity->scalars.empty()) return nullptr;
            if (entity->scalars[0] <= 0.0) return nullptr;
            return std::make_shared<Circle3>(Lift(position(0)), Vec3d{1.0, 0.0, 0.0}, Vec3d{0.0, 1.0, 0.0},
                                             entity->scalars[0], 0.0, kTwoPi);
        }
        case SketchEntityKind::Ellipse: {
            if (entity->points.empty() || entity->scalars.size() < 3) return nullptr;
            const double major = entity->scalars[0];
            const double minor = entity->scalars[1];
            const double rotation = entity->scalars[2];
            if (major <= 0.0 || minor <= 0.0) return nullptr;
            const Vec3d x_axis{std::cos(rotation), std::sin(rotation), 0.0};
            const Vec3d y_axis{-std::sin(rotation), std::cos(rotation), 0.0};
            return std::make_shared<Ellipse3>(Lift(position(0)), x_axis, y_axis, major, minor, 0.0, kTwoPi);
        }
        case SketchEntityKind::Spline: {
            if (entity->points.size() < 2) return nullptr;
            std::vector<Vec3d> control;
            control.reserve(entity->points.size());
            for (std::size_t k = 0; k < entity->points.size(); ++k) control.push_back(Lift(position(k)));
            auto spline = std::make_shared<NurbsCurve3>();
            std::string error;
            if (!NurbsCurve3::CreateFromPoints(entity->degree, entity->knots, control, spline.get(), &error)) {
                return nullptr;
            }
            return spline;
        }
    }
    return nullptr;
}

std::shared_ptr<const Curve3> Sketch::WorldCurve(SketchId entity_id) const {
    const std::shared_ptr<const Curve3> flat = Curve(entity_id);
    if (!flat) return nullptr;
    // Every curve type here is closed under a rigid motion except that
    // there is no transform on Curve3, so the mapping is done by
    // rebuilding the curve on the plane's own axes. A spline is rebuilt
    // from its transformed control points, which is exact because a
    // B-spline is an affine combination of them.
    const SketchEntity *entity = GetEntity(entity_id);
    if (entity == nullptr) return nullptr;
    auto world = [&](const Vec2d &p) { return plane_.ToWorld(p); };
    auto position = [&](std::size_t k) -> Vec2d {
        const SketchPoint *point = GetPoint(entity->points[k]);
        return point != nullptr ? point->position : Vec2d{};
    };
    switch (entity->kind) {
        case SketchEntityKind::Line:
            return std::make_shared<Line3>(Line3::FromPoints(world(position(0)), world(position(1))));
        case SketchEntityKind::Arc:
        case SketchEntityKind::Circle:
        case SketchEntityKind::Ellipse: {
            double lo = 0.0;
            double hi = 0.0;
            flat->Domain(&lo, &hi);
            const Vec2d centre = position(0);
            if (entity->kind == SketchEntityKind::Ellipse) {
                const double rotation = entity->scalars[2];
                const Vec3d x_axis = plane_.x_axis * std::cos(rotation) + plane_.y_axis * std::sin(rotation);
                const Vec3d y_axis = plane_.y_axis * std::cos(rotation) - plane_.x_axis * std::sin(rotation);
                return std::make_shared<Ellipse3>(world(centre), x_axis, y_axis, entity->scalars[0],
                                                  entity->scalars[1], lo, hi);
            }
            double radius = 0.0;
            if (!Radius(entity_id, &radius)) return nullptr;
            return std::make_shared<Circle3>(world(centre), plane_.x_axis, plane_.y_axis, radius, lo, hi);
        }
        case SketchEntityKind::Spline: {
            std::vector<Vec3d> control;
            for (std::size_t k = 0; k < entity->points.size(); ++k) control.push_back(world(position(k)));
            auto spline = std::make_shared<NurbsCurve3>();
            std::string error;
            if (!NurbsCurve3::CreateFromPoints(entity->degree, entity->knots, control, spline.get(), &error)) {
                return nullptr;
            }
            return spline;
        }
        case SketchEntityKind::Point:
            return nullptr;
    }
    return nullptr;
}

// --- Profile extraction ------------------------------------------------

// The signed area a loop of curve pieces encloses, by Green's theorem:
// twice the area is the integral of (x dy - y dx) around it.
//
// Computed from the curves rather than from the polygon the arrangement
// samples them with. The arrangement's polygon is what decides topology,
// and eight points per edge is plenty for that -- but it is an inscribed
// polygon, so a circle of radius 1.5 comes out 2.5% small, and a user
// reading the area of a profile off a dialog would see that. The same
// divergence-theorem argument Part A.6 makes for volumes applies here,
// and the integrand is polynomial in the curve's own derivatives, so a
// fixed Gauss rule is exact for lines and converges very fast otherwise.
double LoopArea(const Sketch &sketch, const std::vector<Sketch::Profile::Piece> &pieces) {
    std::vector<double> nodes;
    std::vector<double> weights;
    GaussLegendreRule(16, &nodes, &weights);
    double twice = 0.0;
    for (const Sketch::Profile::Piece &piece : pieces) {
        const std::shared_ptr<const Curve3> curve = sketch.Curve(piece.entity);
        if (!curve) continue;
        const double half = 0.5 * (piece.t1 - piece.t0);
        const double middle = 0.5 * (piece.t1 + piece.t0);
        for (std::size_t k = 0; k < nodes.size(); ++k) {
            const double t = middle + half * nodes[k];
            std::vector<Vec3d> derivatives;
            curve->Derivatives(t, 1, &derivatives);
            if (derivatives.size() < 2) continue;
            const Vec3d p = derivatives[0];
            const Vec3d d = derivatives[1];
            twice += weights[k] * half * (p.x * d.y - p.y * d.x);
        }
    }
    return 0.5 * twice;
}

bool Sketch::ExtractProfiles(std::vector<Profile> *out, std::string *error) const {
    out->clear();
    if (error != nullptr) error->clear();

    Arrangement arrangement;
    std::vector<SketchId> tags;
    for (const SketchEntity &entity : entities_) {
        if (entity.construction) continue;
        if (entity.kind == SketchEntityKind::Point) continue;
        const std::shared_ptr<const Curve3> curve = Curve(entity.id);
        if (!curve) continue;
        ArrangementSegment segment;
        segment.curve = curve;
        curve->Domain(&segment.t0, &segment.t1);
        segment.tag = static_cast<int>(tags.size());
        tags.push_back(entity.id);
        arrangement.AddSegment(segment);
    }
    if (tags.empty()) return true;
    if (!arrangement.Build()) {
        // Not an error: open geometry has no cycles, which is the normal
        // state of a sketch halfway through being drawn.
        return true;
    }

    auto convert = [&](int cycle) {
        std::vector<Profile::Piece> pieces;
        for (const ArrangementSegment &segment : arrangement.CycleSegments(cycle)) {
            Profile::Piece piece;
            piece.entity = (segment.tag >= 0 && static_cast<std::size_t>(segment.tag) < tags.size())
                               ? tags[static_cast<std::size_t>(segment.tag)]
                               : kNoSketchId;
            piece.t0 = segment.t0;
            piece.t1 = segment.t1;
            pieces.push_back(piece);
        }
        return pieces;
    };

    for (const ArrangementRegion &region : arrangement.Regions(1)) {
        Profile profile;
        profile.outer = convert(region.outer_cycle);
        for (int hole : region.hole_cycles) profile.holes.push_back(convert(hole));
        // The arrangement's own area is the sampled polygon's; the
        // profile reports the curves'.
        profile.area = LoopArea(*this, profile.outer);
        for (const std::vector<Profile::Piece> &hole : profile.holes) {
            profile.area += LoopArea(*this, hole);
        }
        profile.interior = region.interior.front();
        out->push_back(std::move(profile));
    }
    return true;
}

}  // namespace cad
