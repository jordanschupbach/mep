#include "cad_assembly.h"

#include "cad_dual.h"
#include "cad_pattern.h"

#include <algorithm>
#include <cmath>

namespace cad {

// --- Rotations ---------------------------------------------------------

Quatd Quatd::FromAxisAngle(const Vec3d &axis, double angle) {
    const double length = axis.Length();
    if (!(length > 0.0)) return Quatd{};
    const Vec3d unit = axis * (1.0 / length);
    const double half = 0.5 * angle;
    const double s = std::sin(half);
    return Quatd{std::cos(half), unit.x * s, unit.y * s, unit.z * s};
}

Quatd Quatd::Between(const Vec3d &from, const Vec3d &to) {
    const Vec3d a = from.Normalized();
    const Vec3d b = to.Normalized();
    const double d = a.Dot(b);
    if (d > 1.0 - 1e-12) return Quatd{};
    // Opposite directions: the shortest path is a half turn about any
    // perpendicular, and there is no preferred one -- so say so by
    // picking a stable perpendicular rather than whatever round-off
    // suggests.
    if (d < -1.0 + 1e-12) {
        const Vec3d axis = a.AnyPerpendicular();
        return Quatd{0.0, axis.x, axis.y, axis.z};
    }
    const Vec3d axis = a.Cross(b);
    return Quatd{1.0 + d, axis.x, axis.y, axis.z}.Normalized();
}

Quatd Quatd::Normalized() const {
    const double n = Norm();
    if (!(n > 0.0)) return Quatd{};
    return Quatd{w / n, x / n, y / n, z / n};
}

Vec3d Quatd::Rotate(const Vec3d &v) const {
    const Quatd q = Normalized();
    const Vec3d u{q.x, q.y, q.z};
    const Vec3d t = u.Cross(v) * 2.0;
    return v + t * q.w + u.Cross(t);
}

Quatd Quatd::operator*(const Quatd &o) const {
    return Quatd{w * o.w - x * o.x - y * o.y - z * o.z, w * o.x + x * o.w + y * o.z - z * o.y,
                 w * o.y - x * o.z + y * o.w + z * o.x, w * o.z + x * o.y - y * o.x + z * o.w};
}

Mat4d Placement::ToMatrix() const {
    Mat4d out;
    const Vec3d ex = rotation.Rotate(Vec3d{1.0, 0.0, 0.0});
    const Vec3d ey = rotation.Rotate(Vec3d{0.0, 1.0, 0.0});
    const Vec3d ez = rotation.Rotate(Vec3d{0.0, 0.0, 1.0});
    out.m[0][0] = ex.x; out.m[0][1] = ey.x; out.m[0][2] = ez.x; out.m[0][3] = position.x;
    out.m[1][0] = ex.y; out.m[1][1] = ey.y; out.m[1][2] = ez.y; out.m[1][3] = position.y;
    out.m[2][0] = ex.z; out.m[2][1] = ey.z; out.m[2][2] = ez.z; out.m[2][3] = position.z;
    return out;
}

// --- Datums ------------------------------------------------------------

bool DatumFromFace(const Model &model, EntityId face_id, Datum *out, std::string *error) {
    error->clear();
    const Face *face = model.GetFace(face_id);
    if (face == nullptr) {
        *error = "no such face";
        return false;
    }
    const Surface *surface = model.SurfaceAt(face->surface);
    if (const auto *plane = dynamic_cast<const PlaneSurface *>(surface)) {
        Vec3d normal = plane->PlaneNormal().Normalized();
        if (face->orientation == Orientation::Reversed) normal = normal * -1.0;
        out->kind = DatumKind::Plane;
        out->origin = plane->Point(0.0, 0.0);
        out->direction = normal;
        out->name = face->name.empty() ? "plane" : face->name;
        return true;
    }
    if (const auto *cylinder = dynamic_cast<const CylinderSurface *>(surface)) {
        out->kind = DatumKind::Axis;
        out->origin = cylinder->Origin();
        out->direction = cylinder->Axis().Normalized();
        out->name = face->name.empty() ? "axis" : face->name;
        return true;
    }
    *error = "a datum can be taken from a planar face or a cylindrical one; this face is neither";
    return false;
}

bool DatumFromEdge(const Model &model, EntityId edge_id, Datum *out, std::string *error) {
    error->clear();
    const Edge *edge = model.GetEdge(edge_id);
    const Curve3 *curve = edge != nullptr ? model.CurveAt(edge->curve) : nullptr;
    if (curve == nullptr) {
        *error = "no such edge";
        return false;
    }
    if (const auto *line = dynamic_cast<const Line3 *>(curve)) {
        const Vec3d direction = line->Direction();
        if (!(direction.Length() > 0.0)) {
            *error = "the edge has no direction";
            return false;
        }
        out->kind = DatumKind::Axis;
        out->origin = model.EdgeStartPoint(edge_id);
        out->direction = direction.Normalized();
        out->name = "edge axis";
        return true;
    }
    if (const auto *circle = dynamic_cast<const Circle3 *>(curve)) {
        // A hole's rim stands for the hole: the useful datum is the axis
        // through its centre, not the circle itself.
        out->kind = DatumKind::Axis;
        out->origin = circle->Center();
        out->direction = circle->PlaneNormal().Normalized();
        out->name = "circle axis";
        return true;
    }
    *error = "a datum can be taken from a straight edge or a circular one; this edge is neither";
    return false;
}

bool DatumFromVertex(const Model &model, EntityId vertex, Datum *out, std::string *error) {
    error->clear();
    const Vertex *v = model.GetVertex(vertex);
    if (v == nullptr) {
        *error = "no such vertex";
        return false;
    }
    out->kind = DatumKind::Point;
    out->origin = v->point;
    out->name = "vertex";
    return true;
}

bool OffsetDatumPlane(const Datum &plane, double distance, Datum *out, std::string *error) {
    error->clear();
    if (plane.kind != DatumKind::Plane) {
        *error = "only a plane can be offset along its normal";
        return false;
    }
    *out = plane;
    out->origin = plane.origin + plane.direction.Normalized() * distance;
    return true;
}

bool DatumPlaneThroughPoints(const Vec3d &a, const Vec3d &b, const Vec3d &c, Datum *out,
                             std::string *error) {
    error->clear();
    const Vec3d normal = (b - a).Cross(c - a);
    if (!(normal.Length() > 1e-12)) {
        *error = "the three points are in a line, so they name no plane";
        return false;
    }
    out->kind = DatumKind::Plane;
    out->origin = a;
    out->direction = normal.Normalized();
    out->name = "plane through three points";
    return true;
}

bool DatumAxisFromPlanes(const Datum &a, const Datum &b, Datum *out, std::string *error) {
    error->clear();
    if (a.kind != DatumKind::Plane || b.kind != DatumKind::Plane) {
        *error = "an axis from planes needs two planes";
        return false;
    }
    const Vec3d na = a.direction.Normalized();
    const Vec3d nb = b.direction.Normalized();
    const Vec3d along = na.Cross(nb);
    if (!(along.Length() > 1e-12)) {
        *error = "the two planes are parallel, so they meet in no line";
        return false;
    }
    const Vec3d direction = along.Normalized();
    Vec3d point;
    if (!Mat3d::FromRows(na, nb, direction)
             .Solve(Vec3d{na.Dot(a.origin), nb.Dot(b.origin), 0.0}, &point)) {
        *error = "the line where the two planes meet is not determined";
        return false;
    }
    out->kind = DatumKind::Axis;
    out->origin = point;
    out->direction = direction;
    out->name = "axis from two planes";
    return true;
}

bool DatumPointFromAxisAndPlane(const Datum &axis, const Datum &plane, Datum *out, std::string *error) {
    error->clear();
    if (axis.kind != DatumKind::Axis || plane.kind != DatumKind::Plane) {
        *error = "this needs an axis and a plane";
        return false;
    }
    const Vec3d d = axis.direction.Normalized();
    const Vec3d n = plane.direction.Normalized();
    const double along = d.Dot(n);
    if (!(std::fabs(along) > 1e-12)) {
        *error = "the axis runs along the plane, so it meets it nowhere or everywhere";
        return false;
    }
    out->kind = DatumKind::Point;
    out->origin = axis.origin + d * ((plane.origin - axis.origin).Dot(n) / along);
    out->name = "axis meets plane";
    return true;
}

bool CaptureMateEnd(const Model &model, EntityId face_or_edge, bool is_edge, int instance,
                    int generating_feature, const std::string &role, MateEnd *out, std::string *error) {
    error->clear();
    Datum datum;
    // An empty role means "whatever this entity calls itself", which is
    // almost always what is wanted: the role is the strongest signal Part
    // B.4 has, and it only helps if it is the same string the rebuilt
    // model will use. A role invented at the call site scores against
    // nothing and turns a resolvable reference into a lost one.
    if (is_edge) {
        if (!DatumFromEdge(model, face_or_edge, &datum, error)) return false;
        out->source = CaptureEdgeName(model, face_or_edge, generating_feature, role);
    } else {
        if (!DatumFromFace(model, face_or_edge, &datum, error)) return false;
        const Face *face = model.GetFace(face_or_edge);
        out->source =
            CaptureFaceName(model, face_or_edge, generating_feature, role.empty() ? face->name : role);
    }
    out->instance = instance;
    out->datum = datum;
    out->named = true;
    out->source_is_edge = is_edge;
    return true;
}

Datum PlaceDatum(const Datum &datum, const Placement &placement) {
    Datum out = datum;
    out.origin = placement.Apply(datum.origin);
    out.direction = placement.ApplyDirection(datum.direction);
    return out;
}

// --- The solver ---------------------------------------------------------

namespace {

struct DualVec3 {
    Dual x, y, z;
};
DualVec3 operator+(const DualVec3 &a, const DualVec3 &b) { return DualVec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
DualVec3 operator-(const DualVec3 &a, const DualVec3 &b) { return DualVec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
DualVec3 operator*(const DualVec3 &a, const Dual &s) { return DualVec3{a.x * s, a.y * s, a.z * s}; }
Dual Dot(const DualVec3 &a, const DualVec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
DualVec3 Cross(const DualVec3 &a, const DualVec3 &b) {
    return DualVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Dual Length(const DualVec3 &a, const Softening &soft) {
    return Sqrt(a.x * a.x + a.y * a.y + a.z * a.z + Dual(soft.squared));
}

// One instance's seven parameters, read as duals. A grounded instance
// contributes constants instead, which is how it stays put: no columns,
// no derivatives, nothing for the solver to move.
struct DualPlacement {
    DualVec3 position;
    Dual w, x, y, z;
};

// Rotation by a quaternion, written so the dual arithmetic can
// differentiate it: v + 2w(u x v) + 2u x (u x v), divided through by the
// squared norm so that it is a rotation whether or not the quaternion is
// currently a unit one. That division is what lets the solver take a step
// that lengthens the quaternion without the geometry lurching -- the
// normalisation residual then pulls it back, rather than having to hold
// it exactly true at every trial point.
DualVec3 RotateBy(const DualPlacement &p, const DualVec3 &v) {
    const DualVec3 u{p.x, p.y, p.z};
    const Dual norm = p.w * p.w + p.x * p.x + p.y * p.y + p.z * p.z;
    const DualVec3 t = Cross(u, v) * Dual(2.0);
    const DualVec3 rotated = v * norm + t * p.w + Cross(u, t);
    const Dual inverse = Dual(1.0) / norm;
    return rotated * inverse;
}

DualVec3 Constant(const Vec3d &v) { return DualVec3{Dual(v.x), Dual(v.y), Dual(v.z)}; }

// Where each instance's parameters sit in the Jacobian, or -1 throughout
// for a grounded one.
struct Columns {
    int position = -1;  // three, starting here
    int rotation = -1;  // four, starting here
};

}  // namespace

int Assembly::AddPart(Model model, EntityId body, const std::string &name) {
    Part part;
    part.name = name;
    part.model = std::move(model);
    part.body = body;
    parts_.push_back(std::move(part));
    return static_cast<int>(parts_.size()) - 1;
}

int Assembly::AddPartFromTree(std::shared_ptr<FeatureTree> tree, const std::string &name) {
    Part part;
    part.name = name;
    part.tree = std::move(tree);
    parts_.push_back(std::move(part));
    // Evaluate it once now, so that an instance of it has geometry before
    // anything asks.
    std::vector<int> lost;
    std::string error;
    RebuildParts(&lost, &error);
    return static_cast<int>(parts_.size()) - 1;
}

int Assembly::AddSubAssembly(std::shared_ptr<Assembly> assembly, const std::string &name) {
    Part part;
    part.name = name;
    part.sub = std::move(assembly);
    parts_.push_back(std::move(part));
    return static_cast<int>(parts_.size()) - 1;
}

bool Assembly::RebuildParts(std::vector<int> *lost, std::string *error) {
    error->clear();
    lost->clear();
    for (Part &part : parts_) {
        if (part.tree == nullptr) continue;
        std::string why;
        if (!part.tree->Rebuild(&why)) {
            *error = "part " + part.name + " could not be rebuilt: " + why;
            return false;
        }
        const std::vector<EntityId> bodies = part.tree->Bodies();
        if (bodies.empty()) {
            *error = "part " + part.name + " rebuilt to nothing";
            return false;
        }
        // The tree owns its model, and the assembly needs one it can hand
        // to a transform, so it is copied across. Copying a Model copies
        // the topology and shares the geometry -- see cad_topology.h.
        part.model = part.tree->Result();
        part.body = bodies.back();
    }
    return ResolveNamedMates(lost, error);
}

bool Assembly::ResolveNamedMates(std::vector<int> *lost, std::string *error) {
    error->clear();
    lost->clear();
    for (Mate &mate : mates_) {
        for (MateEnd *end : {&mate.a, &mate.b}) {
            if (!end->named) continue;
            const Instance *instance = GetInstance(end->instance);
            const Part *part = instance != nullptr ? GetPart(instance->part) : nullptr;
            if (part == nullptr) continue;
            const ResolveResult found = end->source_is_edge
                                            ? ResolveEdge(part->model, end->source, {})
                                            : ResolveFace(part->model, end->source, {});
            if (found.status != ResolveStatus::Resolved) {
                lost->push_back(mate.id);
                continue;
            }
            Datum datum;
            std::string why;
            const bool ok = end->source_is_edge ? DatumFromEdge(part->model, found.entity, &datum, &why)
                                                : DatumFromFace(part->model, found.entity, &datum, &why);
            if (!ok) {
                lost->push_back(mate.id);
                continue;
            }
            end->datum = datum;
        }
    }
    std::sort(lost->begin(), lost->end());
    lost->erase(std::unique(lost->begin(), lost->end()), lost->end());
    return true;
}

int Assembly::AddInstance(int part, const std::string &name, const Placement &at, bool grounded) {
    Instance instance;
    instance.id = next_instance_++;
    instance.part = part;
    instance.name = name;
    instance.placement = at;
    instance.placement.rotation = instance.placement.rotation.Normalized();
    instance.grounded = grounded;
    instances_.push_back(instance);
    return instance.id;
}

int Assembly::AddMate(const Mate &mate) {
    Mate copy = mate;
    copy.id = next_mate_++;
    mates_.push_back(copy);
    return copy.id;
}

bool Assembly::RemoveMate(int id) {
    for (std::size_t i = 0; i < mates_.size(); ++i) {
        if (mates_[i].id != id) continue;
        mates_.erase(mates_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

const Assembly::Part *Assembly::GetPart(int id) const {
    if (id < 0 || id >= static_cast<int>(parts_.size())) return nullptr;
    return &parts_[static_cast<std::size_t>(id)];
}
const Assembly::Instance *Assembly::GetInstance(int id) const {
    for (const Instance &instance : instances_) {
        if (instance.id == id) return &instance;
    }
    return nullptr;
}
Assembly::Instance *Assembly::GetInstanceMutable(int id) {
    for (Instance &instance : instances_) {
        if (instance.id == id) return &instance;
    }
    return nullptr;
}
const Mate *Assembly::GetMate(int id) const {
    for (const Mate &mate : mates_) {
        if (mate.id == id) return &mate;
    }
    return nullptr;
}
Mate *Assembly::GetMateMutable(int id) {
    for (Mate &mate : mates_) {
        if (mate.id == id) return &mate;
    }
    return nullptr;
}

bool Assembly::Solve(AssemblyReport *report) {
    *report = AssemblyReport{};

    // --- Parameters -----------------------------------------------------
    std::vector<Columns> columns(instances_.size());
    std::vector<double> x;
    std::vector<std::size_t> free_instances;
    for (std::size_t i = 0; i < instances_.size(); ++i) {
        const Instance &instance = instances_[i];
        if (instance.grounded) continue;
        columns[i].position = static_cast<int>(x.size());
        x.push_back(instance.placement.position.x);
        x.push_back(instance.placement.position.y);
        x.push_back(instance.placement.position.z);
        columns[i].rotation = static_cast<int>(x.size());
        const Quatd q = instance.placement.rotation.Normalized();
        x.push_back(q.w);
        x.push_back(q.x);
        x.push_back(q.y);
        x.push_back(q.z);
        free_instances.push_back(i);
    }
    if (free_instances.empty()) {
        // Everything is grounded. That is a valid assembly and there is
        // nothing to solve, but the mates still have to be checked --
        // otherwise a contradiction between two grounded parts is
        // silently reported as success.
        report->ok = true;
        report->degrees_of_freedom = 0;
    }

    std::vector<std::size_t> index_of;
    index_of.resize(instances_.size());
    for (std::size_t i = 0; i < instances_.size(); ++i) index_of[i] = i;
    auto find_instance = [&](int id) -> int {
        for (std::size_t i = 0; i < instances_.size(); ++i) {
            if (instances_[i].id == id) return static_cast<int>(i);
        }
        return -1;
    };

    std::vector<const Mate *> active;
    for (const Mate &mate : mates_) {
        if (mate.suppressed) continue;
        if (find_instance(mate.a.instance) < 0 || find_instance(mate.b.instance) < 0) {
            report->error = "mate " + std::to_string(mate.id) + " names an instance that is not here";
            return false;
        }
        active.push_back(&mate);
    }

    // How many residuals each mate contributes. Several are deliberately
    // over-determined -- three numbers for a two-parameter condition like
    // "these directions are parallel" -- because the redundant form is
    // the one without a singularity in it, and least squares does not
    // mind. The degree-of-freedom count below reads the Jacobian's rank,
    // not the residual count, so nothing downstream is misled.
    auto residual_count = [](MateKind kind) {
        switch (kind) {
            case MateKind::Coincident:
            case MateKind::Offset: return 4;
            case MateKind::Concentric: return 6;
            case MateKind::Parallel: return 3;
            case MateKind::Angle: return 1;
            case MateKind::PointOnPoint: return 3;
            case MateKind::Distance: return 1;
        }
        return 0;
    };
    std::vector<int> mate_row;
    std::size_t rows = 0;
    for (const Mate *mate : active) {
        mate_row.push_back(static_cast<int>(rows));
        rows += static_cast<std::size_t>(residual_count(mate->kind));
    }
    const std::size_t gauge_row = rows;
    rows += free_instances.size();

    const Softening soft;

    auto read_placement = [&](int instance_index, LocalVars *vars) {
        const Instance &instance = instances_[static_cast<std::size_t>(instance_index)];
        const Columns &c = columns[static_cast<std::size_t>(instance_index)];
        DualPlacement out;
        const Placement &p = instance.placement;
        out.position = DualVec3{vars->Variable(p.position.x, c.position < 0 ? -1 : c.position),
                                vars->Variable(p.position.y, c.position < 0 ? -1 : c.position + 1),
                                vars->Variable(p.position.z, c.position < 0 ? -1 : c.position + 2)};
        out.w = vars->Variable(p.rotation.w, c.rotation < 0 ? -1 : c.rotation);
        out.x = vars->Variable(p.rotation.x, c.rotation < 0 ? -1 : c.rotation + 1);
        out.y = vars->Variable(p.rotation.y, c.rotation < 0 ? -1 : c.rotation + 2);
        out.z = vars->Variable(p.rotation.z, c.rotation < 0 ? -1 : c.rotation + 3);
        return out;
    };

    // Reads the current parameter vector back into the instances, so the
    // residual function can be written against placements rather than
    // against a flat array.
    auto adopt = [&](const std::vector<double> &values) {
        for (std::size_t k = 0; k < free_instances.size(); ++k) {
            Instance &instance = instances_[free_instances[k]];
            const Columns &c = columns[free_instances[k]];
            instance.placement.position =
                Vec3d{values[static_cast<std::size_t>(c.position)],
                      values[static_cast<std::size_t>(c.position + 1)],
                      values[static_cast<std::size_t>(c.position + 2)]};
            instance.placement.rotation =
                Quatd{values[static_cast<std::size_t>(c.rotation)],
                      values[static_cast<std::size_t>(c.rotation + 1)],
                      values[static_cast<std::size_t>(c.rotation + 2)],
                      values[static_cast<std::size_t>(c.rotation + 3)]};
        }
    };

    auto evaluate = [&](const std::vector<double> &values, std::vector<double> *residuals,
                        MatrixNd *jacobian) {
        adopt(values);
        residuals->assign(rows, 0.0);
        if (jacobian != nullptr) *jacobian = MatrixNd(rows, values.size());

        auto emit = [&](std::size_t row, const Dual &value, const LocalVars &vars) {
            (*residuals)[row] = value.v;
            if (jacobian == nullptr) return;
            for (int k = 0; k < vars.count; ++k) {
                (*jacobian)(row, static_cast<std::size_t>(vars.column[static_cast<std::size_t>(k)])) +=
                    value.d[static_cast<std::size_t>(k)];
            }
        };

        for (std::size_t m = 0; m < active.size(); ++m) {
            const Mate &mate = *active[m];
            const std::size_t row = static_cast<std::size_t>(mate_row[m]);
            LocalVars vars;
            const DualPlacement pa = read_placement(find_instance(mate.a.instance), &vars);
            const DualPlacement pb = read_placement(find_instance(mate.b.instance), &vars);
            const DualVec3 origin_a = pa.position + RotateBy(pa, Constant(mate.a.datum.origin));
            const DualVec3 origin_b = pb.position + RotateBy(pb, Constant(mate.b.datum.origin));
            const DualVec3 dir_a = RotateBy(pa, Constant(mate.a.datum.direction.Normalized()));
            const DualVec3 dir_b = RotateBy(pb, Constant(mate.b.datum.direction.Normalized()));
            const DualVec3 between = origin_b - origin_a;

            switch (mate.kind) {
                case MateKind::Coincident:
                case MateKind::Offset: {
                    // Facing each other means the normals are opposite, so
                    // their *sum* is what has to vanish. Three numbers for
                    // a two-parameter condition; see above.
                    const DualVec3 facing = mate.flip ? dir_a - dir_b : dir_a + dir_b;
                    emit(row + 0, facing.x, vars);
                    emit(row + 1, facing.y, vars);
                    emit(row + 2, facing.z, vars);
                    emit(row + 3, Dot(between, dir_a) - Dual(mate.value), vars);
                    break;
                }
                case MateKind::Concentric: {
                    // A cross product, so a pin fits its hole either way
                    // round -- which is what people mean by concentric and
                    // is not what "the directions are equal" would give.
                    const DualVec3 turn = Cross(dir_a, dir_b);
                    emit(row + 0, turn.x, vars);
                    emit(row + 1, turn.y, vars);
                    emit(row + 2, turn.z, vars);
                    const DualVec3 across = between - dir_a * Dot(between, dir_a);
                    emit(row + 3, across.x, vars);
                    emit(row + 4, across.y, vars);
                    emit(row + 5, across.z, vars);
                    break;
                }
                case MateKind::Parallel: {
                    const DualVec3 turn = Cross(dir_a, dir_b);
                    emit(row + 0, turn.x, vars);
                    emit(row + 1, turn.y, vars);
                    emit(row + 2, turn.z, vars);
                    break;
                }
                case MateKind::Angle:
                    emit(row, Dot(dir_a, dir_b) - Dual(std::cos(mate.value)), vars);
                    break;
                case MateKind::PointOnPoint:
                    emit(row + 0, between.x, vars);
                    emit(row + 1, between.y, vars);
                    emit(row + 2, between.z, vars);
                    break;
                case MateKind::Distance:
                    emit(row, Length(between, soft) - Dual(mate.value), vars);
                    break;
            }
        }

        // The gauge. A quaternion has four numbers and three degrees of
        // freedom, and this residual is the fourth's only job -- without
        // it the solver has a direction it can move in that changes
        // nothing, and the degree-of-freedom count comes out one too high
        // for every instance.
        for (std::size_t k = 0; k < free_instances.size(); ++k) {
            LocalVars vars;
            const DualPlacement p = read_placement(static_cast<int>(free_instances[k]), &vars);
            emit(gauge_row + k, p.w * p.w + p.x * p.x + p.y * p.y + p.z * p.z - Dual(1.0), vars);
        }
    };

    SolveResult result;
    if (!x.empty()) {
        SolveOptions options;
        options.max_iterations = 200;
        options.f_tol = 1e-14;
        options.x_tol = 1e-14;
        result = LevenbergMarquardt(evaluate, &x, rows, options);
        adopt(x);
    }
    for (Instance &instance : instances_) {
        instance.placement.rotation = instance.placement.rotation.Normalized();
    }

    // --- What came of it -------------------------------------------------
    std::vector<double> residuals;
    MatrixNd jacobian;
    evaluate(x, &residuals, &jacobian);
    double worst = 0.0;
    for (std::size_t m = 0; m < active.size(); ++m) {
        double mate_worst = 0.0;
        const std::size_t row = static_cast<std::size_t>(mate_row[m]);
        for (int k = 0; k < residual_count(active[m]->kind); ++k) {
            mate_worst = std::max(mate_worst, std::fabs(residuals[row + static_cast<std::size_t>(k)]));
        }
        worst = std::max(worst, mate_worst);
        if (mate_worst > 1e-7) report->unsatisfied.push_back(active[m]->id);
    }
    report->residual = worst;
    report->iterations = result.iterations;
    if (!x.empty()) {
        const int rank = MatrixRank(jacobian);
        report->degrees_of_freedom = rank < 0 ? -1 : static_cast<int>(x.size()) - rank;
    }
    report->ok = report->unsatisfied.empty();
    if (!report->ok) {
        report->error = std::to_string(report->unsatisfied.size()) +
                        " mate(s) could not be satisfied; the worst is out by " +
                        std::to_string(worst);
    }
    return report->ok;
}

namespace {

// Places every body of an assembly into `out`, with `outer` applied on
// top of each instance's own placement. Recursive, because a
// sub-assembly's instances are placed within it and the whole thing is
// then placed within its parent -- so the transforms compose, one level
// per nesting, and the leaf bodies are transformed exactly once.
bool FlattenInto(const Assembly &assembly, const Mat4d &outer, Model *out,
                 std::vector<EntityId> *out_bodies, std::string *error) {
    for (const Assembly::Instance &instance : assembly.Instances()) {
        const Assembly::Part *part = assembly.GetPart(instance.part);
        if (part == nullptr) {
            *error = "instance " + std::to_string(instance.id) + " names no part";
            return false;
        }
        const Mat4d here = outer * instance.placement.ToMatrix();
        if (part->sub != nullptr) {
            if (!FlattenInto(*part->sub, here, out, out_bodies, error)) return false;
            continue;
        }
        if (part->body == kNoEntity) {
            *error = "part " + part->name + " has no body to place";
            return false;
        }
        EntityId made = kNoEntity;
        if (!TransformBody(part->model, part->body, here, out, &made, error)) {
            *error = "instance " + instance.name + " could not be placed: " + *error;
            return false;
        }
        out_bodies->push_back(made);
    }
    return true;
}

}  // namespace

bool Assembly::Flatten(Model *out, std::vector<EntityId> *out_bodies, std::string *error) const {
    error->clear();
    *out = Model();
    out_bodies->clear();
    return FlattenInto(*this, Mat4d::Identity(), out, out_bodies, error);
}

bool Assembly::SolveDeep(AssemblyReport *report) {
    // Innermost first. A sub-assembly is rigid from outside, so its own
    // shape has to be settled before there is anything to place -- and a
    // sub-assembly that cannot be solved is reported as this assembly's
    // failure, since from here that is what it is.
    for (Part &part : parts_) {
        if (part.sub == nullptr) continue;
        AssemblyReport inner;
        if (!part.sub->SolveDeep(&inner)) {
            *report = AssemblyReport{};
            report->error = "sub-assembly " + part.name + ": " + inner.error;
            report->unsatisfied = inner.unsatisfied;
            report->lost = inner.lost;
            return false;
        }
    }
    return Solve(report);
}

}  // namespace cad
