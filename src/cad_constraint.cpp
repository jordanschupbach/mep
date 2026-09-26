#include "cad_constraint.h"

#include "cad_dual.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <tuple>

namespace cad {
namespace {

inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

struct DualVec2 {
    Dual x;
    Dual y;
};
DualVec2 operator-(const DualVec2 &a, const DualVec2 &b) { return DualVec2{a.x - b.x, a.y - b.y}; }
DualVec2 operator+(const DualVec2 &a, const DualVec2 &b) { return DualVec2{a.x + b.x, a.y + b.y}; }
DualVec2 operator*(const DualVec2 &a, double s) { return DualVec2{a.x * Dual(s), a.y * Dual(s)}; }
Dual Dot(const DualVec2 &a, const DualVec2 &b) { return a.x * b.x + a.y * b.y; }
Dual Cross(const DualVec2 &a, const DualVec2 &b) { return a.x * b.y - a.y * b.x; }

// The softened length, in two dimensions. See cad_dual.h for why the
// softening is there at all.
Dual Length(const DualVec2 &a, const Softening &soft) { return Sqrt(a.x * a.x + a.y * a.y + Dual(soft.squared)); }

// --- Reading a sketch as duals ----------------------------------------

struct Evaluator {
    const Sketch &sketch;
    const SketchParameters &parameters;
    Softening soft;

    DualVec2 Point(SketchId id, LocalVars *vars) const {
        const SketchPoint *point = sketch.GetPoint(id);
        if (point == nullptr) return DualVec2{};
        return DualVec2{vars->Variable(point->position.x, parameters.PointX(id)),
                        vars->Variable(point->position.y, parameters.PointY(id))};
    }

    Dual Scalar(SketchId entity_id, int index, LocalVars *vars) const {
        const SketchEntity *entity = sketch.GetEntity(entity_id);
        if (entity == nullptr || static_cast<std::size_t>(index) >= entity->scalars.size()) return Dual(0.0);
        return vars->Variable(entity->scalars[Idx(index)], parameters.Scalar(entity_id, index));
    }

    // A line as its two endpoints. Also accepts a pair of loose points,
    // which is what a horizontal or vertical constraint between two
    // arbitrary points needs.
    bool Line(SketchId entity_id, LocalVars *vars, DualVec2 *start, DualVec2 *end) const {
        const SketchEntity *entity = sketch.GetEntity(entity_id);
        if (entity == nullptr || entity->kind != SketchEntityKind::Line || entity->points.size() < 2) {
            return false;
        }
        *start = Point(entity->points[0], vars);
        *end = Point(entity->points[1], vars);
        return true;
    }

    // A circle or an arc as a centre and a radius. An arc's radius is
    // derived from its start point, and the derivatives of that
    // derivation come out of the arithmetic -- which is the whole reason
    // an arc can be stored as three points without the constraint code
    // having to know.
    bool CircleLike(SketchId entity_id, LocalVars *vars, DualVec2 *centre, Dual *radius) const {
        const SketchEntity *entity = sketch.GetEntity(entity_id);
        if (entity == nullptr || entity->points.empty()) return false;
        if (entity->kind == SketchEntityKind::Circle) {
            if (entity->scalars.empty()) return false;
            *centre = Point(entity->points[0], vars);
            *radius = Scalar(entity_id, 0, vars);
            return true;
        }
        if (entity->kind == SketchEntityKind::Arc) {
            if (entity->points.size() < 3) return false;
            *centre = Point(entity->points[0], vars);
            const DualVec2 start = Point(entity->points[1], vars);
            *radius = Length(start - *centre, soft);
            return true;
        }
        return false;
    }
};

// The two points a Horizontal or Vertical constraint is about, taken
// either from an explicit pair or from a line entity.
bool PairFor(const Evaluator &eval, const SketchConstraint &constraint, LocalVars *vars, DualVec2 *a,
             DualVec2 *b) {
    if (constraint.points.size() >= 2) {
        *a = eval.Point(constraint.points[0], vars);
        *b = eval.Point(constraint.points[1], vars);
        return true;
    }
    if (!constraint.entities.empty()) return eval.Line(constraint.entities[0], vars, a, b);
    return false;
}

// --- The residuals ----------------------------------------------------
//
// Each is written as the thing the constraint says, divided where
// necessary to make it a length or an angle rather than a length squared.
// The scaling matters: a least-squares solver weights residuals by how
// big they are, so a parallelism written as a bare cross product would
// count for a hundred times more than a coincidence on a sketch drawn a
// hundred units across, and the sketch would come out parallel and not
// quite joined up.
bool EvaluateOne(const Evaluator &eval, const SketchConstraint &constraint, LocalVars *vars,
                 std::vector<Dual> *out) {
    out->clear();
    const Softening soft = eval.soft;
    const std::vector<SketchId> &points = constraint.points;
    const std::vector<SketchId> &entities = constraint.entities;

    auto two_lines = [&](DualVec2 *d1, DualVec2 *d2) {
        if (entities.size() < 2) return false;
        DualVec2 s1;
        DualVec2 e1;
        DualVec2 s2;
        DualVec2 e2;
        if (!eval.Line(entities[0], vars, &s1, &e1)) return false;
        if (!eval.Line(entities[1], vars, &s2, &e2)) return false;
        *d1 = e1 - s1;
        *d2 = e2 - s2;
        return true;
    };

    switch (constraint.kind) {
        case ConstraintKind::Coincident: {
            if (points.size() < 2) return false;
            const DualVec2 a = eval.Point(points[0], vars);
            const DualVec2 b = eval.Point(points[1], vars);
            out->push_back(a.x - b.x);
            out->push_back(a.y - b.y);
            return true;
        }
        case ConstraintKind::Horizontal: {
            DualVec2 a;
            DualVec2 b;
            if (!PairFor(eval, constraint, vars, &a, &b)) return false;
            out->push_back(b.y - a.y);
            return true;
        }
        case ConstraintKind::Vertical: {
            DualVec2 a;
            DualVec2 b;
            if (!PairFor(eval, constraint, vars, &a, &b)) return false;
            out->push_back(b.x - a.x);
            return true;
        }
        case ConstraintKind::Parallel: {
            DualVec2 d1;
            DualVec2 d2;
            if (!two_lines(&d1, &d2)) return false;
            out->push_back(Cross(d1, d2) / (Length(d1, soft) * Length(d2, soft)));
            return true;
        }
        case ConstraintKind::Perpendicular: {
            DualVec2 d1;
            DualVec2 d2;
            if (!two_lines(&d1, &d2)) return false;
            out->push_back(Dot(d1, d2) / (Length(d1, soft) * Length(d2, soft)));
            return true;
        }
        case ConstraintKind::Equal: {
            if (entities.size() < 2) return false;
            DualVec2 d1;
            DualVec2 d2;
            if (two_lines(&d1, &d2)) {
                out->push_back(Length(d1, soft) - Length(d2, soft));
                return true;
            }
            DualVec2 c1;
            DualVec2 c2;
            Dual r1;
            Dual r2;
            if (!eval.CircleLike(entities[0], vars, &c1, &r1)) return false;
            if (!eval.CircleLike(entities[1], vars, &c2, &r2)) return false;
            out->push_back(r1 - r2);
            return true;
        }
        case ConstraintKind::Concentric: {
            if (entities.size() < 2) return false;
            DualVec2 c1;
            DualVec2 c2;
            Dual r1;
            Dual r2;
            if (!eval.CircleLike(entities[0], vars, &c1, &r1)) return false;
            if (!eval.CircleLike(entities[1], vars, &c2, &r2)) return false;
            out->push_back(c1.x - c2.x);
            out->push_back(c1.y - c2.y);
            return true;
        }
        case ConstraintKind::Collinear: {
            if (entities.size() < 2) return false;
            DualVec2 s1;
            DualVec2 e1;
            DualVec2 s2;
            DualVec2 e2;
            if (!eval.Line(entities[0], vars, &s1, &e1)) return false;
            if (!eval.Line(entities[1], vars, &s2, &e2)) return false;
            const DualVec2 d1 = e1 - s1;
            const Dual length = Length(d1, soft);
            out->push_back(Cross(d1, s2 - s1) / length);
            out->push_back(Cross(d1, e2 - s1) / length);
            return true;
        }
        case ConstraintKind::Symmetric: {
            if (points.size() < 2 || entities.empty()) return false;
            DualVec2 s;
            DualVec2 e;
            if (!eval.Line(entities[0], vars, &s, &e)) return false;
            const DualVec2 a = eval.Point(points[0], vars);
            const DualVec2 b = eval.Point(points[1], vars);
            const DualVec2 axis = e - s;
            const Dual length = Length(axis, soft);
            const DualVec2 middle = (a + b) * 0.5;
            // The midpoint is on the axis, and the two points are on
            // opposite sides of it at the same distance -- which is the
            // same as saying the line joining them is perpendicular to it.
            out->push_back(Cross(axis, middle - s) / length);
            out->push_back(Dot(axis, b - a) / length);
            return true;
        }
        case ConstraintKind::PointOnObject: {
            if (points.empty() || entities.empty()) return false;
            const DualVec2 p = eval.Point(points[0], vars);
            const SketchEntity *entity = eval.sketch.GetEntity(entities[0]);
            if (entity == nullptr) return false;
            if (entity->kind == SketchEntityKind::Line) {
                DualVec2 s;
                DualVec2 e;
                if (!eval.Line(entities[0], vars, &s, &e)) return false;
                const DualVec2 d = e - s;
                out->push_back(Cross(d, p - s) / Length(d, soft));
                return true;
            }
            if (entity->kind == SketchEntityKind::Ellipse) {
                if (entity->scalars.size() < 3) return false;
                const DualVec2 centre = eval.Point(entity->points[0], vars);
                const Dual major = eval.Scalar(entities[0], 0, vars);
                const Dual minor = eval.Scalar(entities[0], 1, vars);
                const Dual rotation = eval.Scalar(entities[0], 2, vars);
                const double c = std::cos(rotation.v);
                const double s = std::sin(rotation.v);
                // The rotation enters through its sine and cosine, whose
                // derivatives are folded in by hand here because the dual
                // arithmetic above has no trigonometric functions -- the
                // ellipse is the only residual that needs them.
                Dual cos_r(c);
                Dual sin_r(s);
                for (int i = 0; i < kMaxLocal; ++i) {
                    cos_r.d[Idx(i)] = -s * rotation.d[Idx(i)];
                    sin_r.d[Idx(i)] = c * rotation.d[Idx(i)];
                }
                const DualVec2 offset = p - centre;
                const Dual along = offset.x * cos_r + offset.y * sin_r;
                const Dual across = offset.y * cos_r - offset.x * sin_r;
                const Dual u = along / major;
                const Dual w = across / minor;
                // Multiplied back up by the mean radius so the residual
                // is a length rather than a dimensionless number, for the
                // same scaling reason as everything else here.
                out->push_back((u * u + w * w - Dual(1.0)) * ((major + minor) * 0.25));
                return true;
            }
            DualVec2 centre;
            Dual radius;
            if (!eval.CircleLike(entities[0], vars, &centre, &radius)) return false;
            out->push_back(Length(p - centre, soft) - radius);
            return true;
        }
        case ConstraintKind::Tangent: {
            if (entities.size() < 2) return false;
            const SketchEntity *first = eval.sketch.GetEntity(entities[0]);
            const SketchEntity *second = eval.sketch.GetEntity(entities[1]);
            if (first == nullptr || second == nullptr) return false;
            const bool first_is_line = first->kind == SketchEntityKind::Line;
            const bool second_is_line = second->kind == SketchEntityKind::Line;
            if (first_is_line && second_is_line) return false;
            if (first_is_line || second_is_line) {
                const SketchId line_id = first_is_line ? entities[0] : entities[1];
                const SketchId round_id = first_is_line ? entities[1] : entities[0];
                DualVec2 s;
                DualVec2 e;
                if (!eval.Line(line_id, vars, &s, &e)) return false;
                DualVec2 centre;
                Dual radius;
                if (!eval.CircleLike(round_id, vars, &centre, &radius)) return false;
                const DualVec2 d = e - s;
                const Dual distance = Cross(d, centre - s) / Length(d, soft);
                // The squared form, so that tangency on either side of
                // the line satisfies the same equation -- a signed
                // distance would pick a side, and a sketch whose circle
                // happened to start on the other one would be dragged
                // through the line to reach it. Divided back down to a
                // length, which is what the difference of the two
                // squares is over their sum.
                out->push_back((distance * distance - radius * radius) / (SmoothAbs(distance, soft) + radius));
                return true;
            }
            DualVec2 c1;
            DualVec2 c2;
            Dual r1;
            Dual r2;
            if (!eval.CircleLike(entities[0], vars, &c1, &r1)) return false;
            if (!eval.CircleLike(entities[1], vars, &c2, &r2)) return false;
            const Dual centre_distance = Length(c2 - c1, soft);
            // Two circles touch either outside each other or one inside
            // the other, and those are different equations. Which is
            // meant cannot be read off the geometry while the solver is
            // moving it, so it is recorded on the constraint: `value`
            // negative means internal. Deciding it per evaluation instead
            // would let the residual jump between the two as the sketch
            // moved, and the solve would never settle.
            if (constraint.value < 0.0) {
                out->push_back(centre_distance - SmoothAbs(r1 - r2, soft));
            } else {
                out->push_back(centre_distance - (r1 + r2));
            }
            return true;
        }
        case ConstraintKind::Distance: {
            if (points.size() < 2) return false;
            const DualVec2 a = eval.Point(points[0], vars);
            const DualVec2 b = eval.Point(points[1], vars);
            out->push_back(Length(b - a, soft) - Dual(constraint.value));
            return true;
        }
        case ConstraintKind::HorizontalDistance: {
            if (points.size() < 2) return false;
            const DualVec2 a = eval.Point(points[0], vars);
            const DualVec2 b = eval.Point(points[1], vars);
            // Signed, which is what a horizontal dimension means: it says
            // where the second point is relative to the first, not merely
            // how far.
            out->push_back((b.x - a.x) - Dual(constraint.value));
            return true;
        }
        case ConstraintKind::VerticalDistance: {
            if (points.size() < 2) return false;
            const DualVec2 a = eval.Point(points[0], vars);
            const DualVec2 b = eval.Point(points[1], vars);
            out->push_back((b.y - a.y) - Dual(constraint.value));
            return true;
        }
        case ConstraintKind::Angle: {
            DualVec2 d1;
            DualVec2 d2;
            if (!two_lines(&d1, &d2)) return false;
            Dual angle = Atan2(Cross(d1, d2), Dot(d1, d2)) - Dual(constraint.value);
            // Brought into (-pi, pi] by subtracting a whole number of
            // turns. The shift is a constant, so it changes the value and
            // not one derivative -- which is what makes this a wrap
            // rather than a different function.
            const double turns = std::round(angle.v / kTwoPi);
            angle.v -= turns * kTwoPi;
            out->push_back(angle);
            return true;
        }
        case ConstraintKind::Radius: {
            if (entities.empty()) return false;
            DualVec2 centre;
            Dual radius;
            if (!eval.CircleLike(entities[0], vars, &centre, &radius)) return false;
            out->push_back(radius - Dual(constraint.value));
            return true;
        }
        case ConstraintKind::Diameter: {
            if (entities.empty()) return false;
            DualVec2 centre;
            Dual radius;
            if (!eval.CircleLike(entities[0], vars, &centre, &radius)) return false;
            out->push_back(radius * Dual(2.0) - Dual(constraint.value));
            return true;
        }
        case ConstraintKind::ArcRadius: {
            if (entities.empty()) return false;
            const SketchEntity *entity = eval.sketch.GetEntity(entities[0]);
            if (entity == nullptr || entity->kind != SketchEntityKind::Arc || entity->points.size() < 3) {
                return false;
            }
            const DualVec2 centre = eval.Point(entity->points[0], vars);
            const DualVec2 start = eval.Point(entity->points[1], vars);
            const DualVec2 end = eval.Point(entity->points[2], vars);
            out->push_back(Length(start - centre, soft) - Length(end - centre, soft));
            return true;
        }
    }
    return false;
}

double SketchScale(const Sketch &sketch) {
    Box3d extent;
    for (const SketchPoint &point : sketch.Points()) {
        extent.Expand(Vec3d{point.position.x, point.position.y, 0.0});
    }
    double scale = extent.IsEmpty() ? 1.0 : extent.Extent().Length();
    for (const SketchEntity &entity : sketch.Entities()) {
        for (double s : entity.scalars) scale = std::max(scale, std::fabs(s));
    }
    return std::max(1.0, scale);
}

}  // namespace

// --- SketchParameters --------------------------------------------------

SketchParameters::SketchParameters(const Sketch &sketch) {
    for (const SketchPoint &point : sketch.Points()) {
        if (point.fixed) continue;
        point_slots_.push_back({point.id, count_});
        owners_.push_back(Owner{point.id, kNoSketchId, 0});
        owners_.push_back(Owner{point.id, kNoSketchId, 1});
        count_ += 2;
    }
    for (const SketchEntity &entity : sketch.Entities()) {
        for (std::size_t k = 0; k < entity.scalars.size(); ++k) {
            if (k < entity.scalar_fixed.size() && entity.scalar_fixed[k]) continue;
            scalar_slots_.emplace_back(entity.id, static_cast<int>(k), count_);
            owners_.push_back(Owner{kNoSketchId, entity.id, static_cast<int>(k)});
            ++count_;
        }
    }
}

int SketchParameters::PointX(SketchId point) const {
    for (const std::pair<SketchId, int> &slot : point_slots_) {
        if (slot.first == point) return slot.second;
    }
    return -1;
}
int SketchParameters::PointY(SketchId point) const {
    const int x = PointX(point);
    return x < 0 ? -1 : x + 1;
}
int SketchParameters::Scalar(SketchId entity, int index) const {
    for (const std::tuple<SketchId, int, int> &slot : scalar_slots_) {
        if (std::get<0>(slot) == entity && std::get<1>(slot) == index) return std::get<2>(slot);
    }
    return -1;
}

void SketchParameters::Gather(const Sketch &sketch, std::vector<double> *out) const {
    out->assign(static_cast<std::size_t>(count_), 0.0);
    for (const std::pair<SketchId, int> &slot : point_slots_) {
        const SketchPoint *point = sketch.GetPoint(slot.first);
        if (point == nullptr) continue;
        (*out)[Idx(slot.second)] = point->position.x;
        (*out)[Idx(slot.second + 1)] = point->position.y;
    }
    for (const std::tuple<SketchId, int, int> &slot : scalar_slots_) {
        const SketchEntity *entity = sketch.GetEntity(std::get<0>(slot));
        if (entity == nullptr) continue;
        (*out)[Idx(std::get<2>(slot))] = entity->scalars[Idx(std::get<1>(slot))];
    }
}

void SketchParameters::Scatter(const std::vector<double> &values, Sketch *sketch) const {
    if (static_cast<int>(values.size()) != count_) return;
    for (const std::pair<SketchId, int> &slot : point_slots_) {
        SketchPoint *point = sketch->GetPoint(slot.first);
        if (point == nullptr) continue;
        point->position = Vec2d{values[Idx(slot.second)], values[Idx(slot.second + 1)]};
    }
    for (const std::tuple<SketchId, int, int> &slot : scalar_slots_) {
        SketchEntity *entity = sketch->GetEntity(std::get<0>(slot));
        if (entity == nullptr) continue;
        entity->scalars[Idx(std::get<1>(slot))] = values[Idx(std::get<2>(slot))];
    }
}

// --- Evaluation --------------------------------------------------------

int ResidualCount(const Sketch &sketch, const SketchConstraint &constraint) {
    if (!constraint.driving) return 0;
    SketchParameters parameters(sketch);
    Evaluator eval{sketch, parameters, Softening{}};
    LocalVars vars;
    std::vector<Dual> residuals;
    if (!EvaluateOne(eval, constraint, &vars, &residuals)) return 0;
    return static_cast<int>(residuals.size());
}

bool EvaluateConstraints(const Sketch &sketch, const SketchParameters &parameters,
                         std::vector<double> *residuals, MatrixNd *jacobian,
                         std::vector<SketchId> *residual_owner, std::string *error) {
    if (error != nullptr) error->clear();
    const double scale = SketchScale(sketch);
    Softening soft;
    soft.squared = (1e-9 * scale) * (1e-9 * scale);
    Evaluator eval{sketch, parameters, soft};

    std::vector<double> values;
    std::vector<SketchId> owners;
    std::vector<std::vector<std::pair<int, double>>> rows;
    for (const SketchConstraint &constraint : sketch.Constraints()) {
        if (!constraint.driving) continue;
        LocalVars vars;
        std::vector<Dual> local;
        if (!EvaluateOne(eval, constraint, &vars, &local)) {
            if (error != nullptr) {
                *error = "constraint " + std::to_string(constraint.id) +
                         " does not apply to the geometry it names";
            }
            return false;
        }
        for (const Dual &residual : local) {
            values.push_back(residual.v);
            owners.push_back(constraint.id);
            std::vector<std::pair<int, double>> row;
            for (int k = 0; k < vars.count; ++k) {
                row.push_back({vars.column[Idx(k)], residual.d[Idx(k)]});
            }
            rows.push_back(std::move(row));
        }
    }

    if (residuals != nullptr) *residuals = values;
    if (residual_owner != nullptr) *residual_owner = owners;
    if (jacobian != nullptr) {
        *jacobian = MatrixNd(values.size(), static_cast<std::size_t>(std::max(1, parameters.Count())));
        for (std::size_t r = 0; r < rows.size(); ++r) {
            for (const std::pair<int, double> &entry : rows[r]) {
                if (entry.first < 0 || entry.first >= parameters.Count()) continue;
                // Accumulated, not assigned: a constraint can touch the
                // same parameter through two of its arguments (a line
                // told to be tangent to an arc that shares its endpoint),
                // and the two contributions add.
                (*jacobian)(r, Idx(entry.first)) += entry.second;
            }
        }
    }
    return true;
}

void UpdateReferenceDimensions(Sketch *sketch) {
    for (const SketchConstraint &constraint : sketch->Constraints()) {
        if (constraint.driving) continue;
        SketchConstraint *target = sketch->GetConstraint(constraint.id);
        if (target == nullptr) continue;
        auto point_of = [&](std::size_t k) -> const SketchPoint * {
            return k < constraint.points.size() ? sketch->GetPoint(constraint.points[k]) : nullptr;
        };
        const SketchPoint *a = point_of(0);
        const SketchPoint *b = point_of(1);
        switch (constraint.kind) {
            case ConstraintKind::Distance:
                if (a != nullptr && b != nullptr) target->value = (b->position - a->position).Length();
                break;
            case ConstraintKind::HorizontalDistance:
                if (a != nullptr && b != nullptr) target->value = b->position.x - a->position.x;
                break;
            case ConstraintKind::VerticalDistance:
                if (a != nullptr && b != nullptr) target->value = b->position.y - a->position.y;
                break;
            case ConstraintKind::Radius:
                if (!constraint.entities.empty()) {
                    double radius = 0.0;
                    if (sketch->Radius(constraint.entities[0], &radius)) target->value = radius;
                }
                break;
            case ConstraintKind::Diameter:
                if (!constraint.entities.empty()) {
                    double radius = 0.0;
                    if (sketch->Radius(constraint.entities[0], &radius)) target->value = 2.0 * radius;
                }
                break;
            case ConstraintKind::Angle: {
                if (constraint.entities.size() < 2) break;
                auto direction = [&](SketchId id, Vec2d *out) {
                    const SketchEntity *entity = sketch->GetEntity(id);
                    if (entity == nullptr || entity->points.size() < 2) return false;
                    const SketchPoint *s = sketch->GetPoint(entity->points[0]);
                    const SketchPoint *e = sketch->GetPoint(entity->points[1]);
                    if (s == nullptr || e == nullptr) return false;
                    *out = e->position - s->position;
                    return true;
                };
                Vec2d d1;
                Vec2d d2;
                if (direction(constraint.entities[0], &d1) && direction(constraint.entities[1], &d2)) {
                    target->value = std::atan2(d1.Cross(d2), d1.Dot(d2));
                }
                break;
            }
            default:
                break;
        }
    }
}

// --- Solving -----------------------------------------------------------

namespace {

// Independent groups of constraints, found by joining every parameter a
// constraint touches. A sketch is very often several unconnected pieces,
// and solving them as one dense block is both slower and worse
// conditioned than solving each on its own.
struct Clustering {
    std::vector<int> parameter_cluster;            // -1 for a parameter no constraint touches
    std::vector<std::vector<int>> cluster_params;  // parameters, per cluster
    std::vector<std::vector<int>> cluster_constraints;
    // Constraints that touch no parameter at all -- every point they
    // name is fixed. They cannot be solved, only checked, and they are
    // exactly how a conflict between two anchors shows up.
    std::vector<int> unsolvable;
};

Clustering BuildClusters(const Sketch &sketch, const SketchParameters &parameters) {
    const int n = parameters.Count();
    std::vector<int> parent(static_cast<std::size_t>(std::max(0, n)));
    for (int i = 0; i < n; ++i) parent[Idx(i)] = i;
    std::function<int(int)> find = [&](int x) {
        while (parent[Idx(x)] != x) {
            parent[Idx(x)] = parent[Idx(parent[Idx(x)])];
            x = parent[Idx(x)];
        }
        return x;
    };
    auto unite = [&](int a, int b) {
        const int ra = find(a);
        const int rb = find(b);
        if (ra != rb) parent[Idx(ra)] = rb;
    };

    const double scale = SketchScale(sketch);
    Softening soft;
    soft.squared = (1e-9 * scale) * (1e-9 * scale);
    Evaluator eval{sketch, parameters, soft};

    std::vector<std::vector<int>> touched;
    const std::vector<SketchConstraint> &constraints = sketch.Constraints();
    for (const SketchConstraint &constraint : constraints) {
        std::vector<int> columns;
        if (constraint.driving) {
            LocalVars vars;
            std::vector<Dual> local;
            if (EvaluateOne(eval, constraint, &vars, &local)) {
                for (int k = 0; k < vars.count; ++k) columns.push_back(vars.column[Idx(k)]);
            }
        }
        std::sort(columns.begin(), columns.end());
        columns.erase(std::unique(columns.begin(), columns.end()), columns.end());
        for (std::size_t k = 1; k < columns.size(); ++k) unite(columns[0], columns[k]);
        touched.push_back(std::move(columns));
    }

    Clustering clustering;
    clustering.parameter_cluster.assign(static_cast<std::size_t>(std::max(0, n)), -1);
    std::map<int, int> root_to_cluster;
    for (std::size_t c = 0; c < constraints.size(); ++c) {
        if (touched[c].empty()) {
            if (constraints[c].driving) clustering.unsolvable.push_back(static_cast<int>(c));
            continue;
        }
        const int root = find(touched[c][0]);
        auto found = root_to_cluster.find(root);
        if (found == root_to_cluster.end()) {
            const int index = static_cast<int>(clustering.cluster_params.size());
            root_to_cluster.emplace(root, index);
            clustering.cluster_params.emplace_back();
            clustering.cluster_constraints.emplace_back();
            found = root_to_cluster.find(root);
        }
        clustering.cluster_constraints[Idx(found->second)].push_back(static_cast<int>(c));
    }
    for (int i = 0; i < n; ++i) {
        const auto found = root_to_cluster.find(find(i));
        if (found == root_to_cluster.end()) continue;
        clustering.parameter_cluster[Idx(i)] = found->second;
        clustering.cluster_params[Idx(found->second)].push_back(i);
    }
    return clustering;
}

// Solves one cluster with Levenberg-Marquardt. The residual function
// evaluates only that cluster's constraints, against a scratch copy of
// the sketch, so nothing outside the cluster is touched.
SolveResult SolveCluster(Sketch *sketch, const SketchParameters &parameters, const std::vector<int> &columns,
                         const std::vector<int> &constraint_indices, const ConstraintSolveOptions &options) {
    std::vector<double> all;
    parameters.Gather(*sketch, &all);

    std::vector<double> local;
    local.reserve(columns.size());
    for (int column : columns) local.push_back(all[Idx(column)]);

    const double scale = SketchScale(*sketch);
    Softening soft;
    soft.squared = (1e-9 * scale) * (1e-9 * scale);

    // How many residuals the cluster has, and a map from each local
    // parameter's global column to its position in the local vector.
    std::map<int, int> column_to_local;
    for (std::size_t k = 0; k < columns.size(); ++k) column_to_local.emplace(columns[k], static_cast<int>(k));

    Sketch scratch = *sketch;
    std::size_t residual_count = 0;
    {
        Evaluator eval{scratch, parameters, soft};
        for (int index : constraint_indices) {
            LocalVars vars;
            std::vector<Dual> values;
            if (EvaluateOne(eval, scratch.Constraints()[Idx(index)], &vars, &values)) {
                residual_count += values.size();
            }
        }
    }
    if (residual_count == 0 || columns.empty()) return SolveResult{SolveStatus::Converged, 0, 0.0};

    auto residual_and_jacobian = [&](const std::vector<double> &x, std::vector<double> *r, MatrixNd *j) {
        std::vector<double> full = all;
        for (std::size_t k = 0; k < columns.size(); ++k) full[Idx(columns[k])] = x[k];
        parameters.Scatter(full, &scratch);
        Evaluator eval{scratch, parameters, soft};
        r->clear();
        if (j != nullptr) *j = MatrixNd(residual_count, columns.size());
        std::size_t row = 0;
        for (int index : constraint_indices) {
            LocalVars vars;
            std::vector<Dual> values;
            if (!EvaluateOne(eval, scratch.Constraints()[Idx(index)], &vars, &values)) continue;
            for (const Dual &value : values) {
                r->push_back(value.v);
                if (j != nullptr) {
                    for (int k = 0; k < vars.count; ++k) {
                        const auto found = column_to_local.find(vars.column[Idx(k)]);
                        if (found == column_to_local.end()) continue;
                        (*j)(row, Idx(found->second)) += value.d[Idx(k)];
                    }
                }
                ++row;
            }
        }
        r->resize(residual_count, 0.0);
    };

    SolveOptions solve_options;
    solve_options.max_iterations = options.max_iterations;
    solve_options.f_tol = options.tolerance * options.tolerance;
    solve_options.x_tol = options.tolerance * 1e-3;
    const SolveResult result = LevenbergMarquardt(residual_and_jacobian, &local, residual_count, solve_options);

    for (std::size_t k = 0; k < columns.size(); ++k) all[Idx(columns[k])] = local[k];
    parameters.Scatter(all, sketch);
    return result;
}

// The analysis that turns a Jacobian and a residual into an answer a user
// can act on.
void Diagnose(const Sketch &sketch, const SketchParameters &parameters, const ConstraintSolveOptions &options,
              SketchDiagnosis *out) {
    std::vector<double> residuals;
    MatrixNd jacobian;
    std::vector<SketchId> owners;
    std::string error;
    if (!EvaluateConstraints(sketch, parameters, &residuals, &jacobian, &owners, &error)) {
        out->status = SketchStatus::NotConverged;
        out->message = error;
        return;
    }
    const double scale = SketchScale(sketch);
    const double tolerance = options.tolerance * scale;

    out->parameters = parameters.Count();
    out->residuals = static_cast<int>(residuals.size());
    double norm = 0.0;
    for (double r : residuals) norm += r * r;
    out->residual_norm = std::sqrt(norm);

    if (residuals.empty()) {
        out->rank = 0;
        out->degrees_of_freedom = out->parameters;
        out->free_directions.clear();
        for (int i = 0; i < out->parameters; ++i) {
            std::vector<double> direction(static_cast<std::size_t>(out->parameters), 0.0);
            direction[Idx(i)] = 1.0;
            out->free_directions.push_back(std::move(direction));
        }
        out->status = out->parameters > 0 ? SketchStatus::UnderConstrained : SketchStatus::Solved;
        return;
    }

    out->rank = MatrixRank(jacobian, options.rank_tolerance);
    out->degrees_of_freedom = std::max(0, out->parameters - out->rank);
    NullSpace(jacobian, &out->free_directions, options.rank_tolerance);

    // Dependencies among the *constraints* live in the left null space:
    // combinations of rows that the parameters cannot distinguish. Each
    // one is either consistent with the residual or not, and that is
    // exactly the difference between a redundant constraint and a
    // conflicting one. Nothing else tells them apart -- both are a rank
    // deficiency, and a rank deficiency on its own is not a complaint.
    std::vector<std::vector<double>> left_null;
    NullSpace(jacobian.Transposed(), &left_null, options.rank_tolerance);
    std::vector<bool> conflicting(residuals.size(), false);
    std::vector<bool> redundant(residuals.size(), false);
    for (const std::vector<double> &y : left_null) {
        double projection = 0.0;
        double largest = 0.0;
        for (std::size_t i = 0; i < residuals.size() && i < y.size(); ++i) {
            projection += y[i] * residuals[i];
            largest = std::max(largest, std::fabs(y[i]));
        }
        if (largest <= 0.0) continue;
        const bool inconsistent = std::fabs(projection) > tolerance;
        for (std::size_t i = 0; i < residuals.size() && i < y.size(); ++i) {
            // A row participates in the dependency when its weight in it
            // is a real fraction of the largest, rather than the residue
            // of a rounding error in the decomposition.
            if (std::fabs(y[i]) < 0.05 * largest) continue;
            if (inconsistent) {
                conflicting[i] = true;
            } else {
                redundant[i] = true;
            }
        }
    }
    auto collect = [&](const std::vector<bool> &flags, std::vector<SketchId> *ids) {
        ids->clear();
        for (std::size_t i = 0; i < flags.size(); ++i) {
            if (!flags[i]) continue;
            if (std::find(ids->begin(), ids->end(), owners[i]) == ids->end()) ids->push_back(owners[i]);
        }
    };
    collect(conflicting, &out->conflicting);
    collect(redundant, &out->redundant);

    if (!out->conflicting.empty()) {
        out->status = SketchStatus::Conflicting;
        out->message = "constraints depend on one another and disagree";
    } else if (out->residual_norm > tolerance) {
        out->status = SketchStatus::NotConverged;
        out->message = "the solver did not reach a solution";
    } else if (!out->redundant.empty()) {
        out->status = SketchStatus::Redundant;
        out->message = "constraints depend on one another but agree";
    } else if (out->degrees_of_freedom > 0) {
        out->status = SketchStatus::UnderConstrained;
    } else {
        out->status = SketchStatus::Solved;
    }
}

}  // namespace

bool DiagnoseSketch(const Sketch &sketch, SketchDiagnosis *diagnosis,
                    const ConstraintSolveOptions &options) {
    *diagnosis = SketchDiagnosis{};
    const SketchParameters parameters(sketch);
    Diagnose(sketch, parameters, options, diagnosis);
    const Clustering clustering = BuildClusters(sketch, parameters);
    diagnosis->clusters = static_cast<int>(clustering.cluster_params.size());
    return diagnosis->message.empty() || diagnosis->status != SketchStatus::NotConverged ||
           diagnosis->residuals > 0;
}

bool SolveSketch(Sketch *sketch, SketchDiagnosis *diagnosis, const ConstraintSolveOptions &options) {
    *diagnosis = SketchDiagnosis{};
    const SketchParameters parameters(*sketch);
    {
        // A first evaluation, purely to report a constraint that names
        // geometry it cannot apply to before anything is moved.
        std::string error;
        if (!EvaluateConstraints(*sketch, parameters, nullptr, nullptr, nullptr, &error)) {
            diagnosis->status = SketchStatus::NotConverged;
            diagnosis->message = error;
            return false;
        }
    }

    const Clustering clustering = BuildClusters(*sketch, parameters);
    diagnosis->clusters = static_cast<int>(clustering.cluster_params.size());
    int iterations = 0;
    if (options.decompose) {
        for (std::size_t c = 0; c < clustering.cluster_params.size(); ++c) {
            const SolveResult result = SolveCluster(sketch, parameters, clustering.cluster_params[c],
                                                    clustering.cluster_constraints[c], options);
            iterations = std::max(iterations, result.iterations);
        }
    } else {
        std::vector<int> columns;
        for (int i = 0; i < parameters.Count(); ++i) columns.push_back(i);
        std::vector<int> all_constraints;
        for (std::size_t i = 0; i < sketch->Constraints().size(); ++i) {
            if (sketch->Constraints()[i].driving) all_constraints.push_back(static_cast<int>(i));
        }
        const SolveResult result = SolveCluster(sketch, parameters, columns, all_constraints, options);
        iterations = result.iterations;
    }

    UpdateReferenceDimensions(sketch);
    Diagnose(*sketch, parameters, options, diagnosis);
    diagnosis->clusters = static_cast<int>(clustering.cluster_params.size());
    diagnosis->iterations = iterations;
    return true;
}

bool DragPoint(Sketch *sketch, SketchId point_id, const Vec2d &target, SketchDiagnosis *diagnosis,
               const ConstraintSolveOptions &options) {
    *diagnosis = SketchDiagnosis{};
    const SketchPoint *point = sketch->GetPoint(point_id);
    if (point == nullptr) {
        diagnosis->message = "no such point";
        return false;
    }
    const SketchParameters parameters(*sketch);
    const int px = parameters.PointX(point_id);
    const int py = parameters.PointY(point_id);

    // The point is moved along the directions the sketch is actually free
    // to move in, not pinned to the pointer.
    //
    // Pinning is the obvious implementation and it is wrong. A rectangle
    // with a dimensioned width, dragged sideways by a corner, has no
    // solution with that corner where the pointer is -- so a pinned solve
    // reports a conflict and the whole drag is refused, including the
    // vertical part of it that was perfectly possible. What a user means
    // by dragging is "go as far toward here as you can", and the set of
    // directions that can be gone in is exactly the null space of the
    // constraint Jacobian: the same basis Part D.3 reports as drag
    // handles. Projecting the requested motion onto it and then
    // re-solving to clean up the second-order drift converges on the
    // reachable point closest to the pointer, and it falls out to doing
    // nothing at all when the sketch is fully constrained.
    int iterations = 0;
    if (px >= 0 && py >= 0) {
        for (int step = 0; step < 16; ++step) {
            std::vector<double> x;
            parameters.Gather(*sketch, &x);
            const Vec2d current{x[Idx(px)], x[Idx(py)]};
            const Vec2d wanted = target - current;
            const double scale = SketchScale(*sketch);
            if (wanted.Length() <= options.tolerance * scale) break;

            MatrixNd jacobian;
            std::string error;
            if (!EvaluateConstraints(*sketch, parameters, nullptr, &jacobian, nullptr, &error)) {
                diagnosis->message = error;
                return false;
            }
            std::vector<std::vector<double>> free_directions;
            if (jacobian.Rows() == 0) {
                // Nothing constrains anything: the point simply goes
                // where it was asked to.
                x[Idx(px)] = target.x;
                x[Idx(py)] = target.y;
                parameters.Scatter(x, sketch);
                break;
            }
            NullSpace(jacobian, &free_directions, options.rank_tolerance);
            // The combination of free directions that moves *this point*
            // as close to the pointer as possible, and moves the rest of
            // the sketch as little as possible while doing it.
            //
            // Simply projecting the requested motion onto each direction
            // and adding the results is the tempting shortcut and it
            // under-shoots, badly. A rectangle's height direction moves
            // two corners together, so each unit of it moves the dragged
            // corner by only 1/sqrt(2); the projection then delivers half
            // the requested motion per step, and sixteen steps still
            // leave the corner a measurable distance from the pointer.
            // What is wanted is a least-squares fit in the two
            // coordinates of the dragged point, whose normal equations
            // are 2x2 however many free directions there are.
            const std::size_t modes = free_directions.size();
            double g00 = 0.0;
            double g01 = 0.0;
            double g11 = 0.0;
            for (const std::vector<double> &direction : free_directions) {
                const double mx = direction[Idx(px)];
                const double my = direction[Idx(py)];
                g00 += mx * mx;
                g01 += mx * my;
                g11 += my * my;
            }
            // Inverted through its eigenvalues, with the directions the
            // point genuinely cannot move in dropped rather than divided
            // by. Those are not an error: a corner on a dimensioned edge
            // cannot move along it, and the answer is to move in the
            // other direction only.
            const double trace = g00 + g11;
            const double determinant = g00 * g11 - g01 * g01;
            const double gap = std::sqrt(std::max(0.0, trace * trace - 4.0 * determinant));
            const double eigen[2] = {0.5 * (trace + gap), 0.5 * (trace - gap)};
            Vec2d axes[2];
            if (std::fabs(g01) > 1e-300) {
                for (int k = 0; k < 2; ++k) {
                    axes[k] = Vec2d{eigen[k] - g11, g01};
                    const double length = axes[k].Length();
                    axes[k] = (length > 0.0) ? axes[k] / length : Vec2d{1.0, 0.0};
                }
            } else {
                axes[0] = (g00 >= g11) ? Vec2d{1.0, 0.0} : Vec2d{0.0, 1.0};
                axes[1] = (g00 >= g11) ? Vec2d{0.0, 1.0} : Vec2d{1.0, 0.0};
            }
            const double cutoff = std::max(trace, 1.0) * 1e-12;
            Vec2d weights{0.0, 0.0};
            for (int k = 0; k < 2; ++k) {
                if (eigen[k] <= cutoff) continue;
                weights += axes[k] * (axes[k].Dot(wanted) / eigen[k]);
            }
            std::vector<double> motion(x.size(), 0.0);
            for (std::size_t m = 0; m < modes; ++m) {
                const std::vector<double> &direction = free_directions[m];
                const double coefficient = direction[Idx(px)] * weights.x + direction[Idx(py)] * weights.y;
                for (std::size_t i = 0; i < motion.size(); ++i) motion[i] += coefficient * direction[i];
            }
            const Vec2d achieved{motion[Idx(px)], motion[Idx(py)]};
            if (achieved.Length() <= options.tolerance * scale) break;
            for (std::size_t i = 0; i < x.size(); ++i) x[i] += motion[i];
            parameters.Scatter(x, sketch);

            // The projection is linear and the constraints are not, so
            // the step lands slightly off the solution set; re-solving
            // puts it back on, near where it landed rather than somewhere
            // else entirely, which is what makes the drag continuous.
            SketchDiagnosis inner;
            SolveSketch(sketch, &inner, options);
            iterations += inner.iterations;
            if (inner.status == SketchStatus::Conflicting) break;
        }
    }

    const bool ok = DiagnoseSketch(*sketch, diagnosis, options);
    diagnosis->iterations = iterations;
    return ok;
}

}  // namespace cad
