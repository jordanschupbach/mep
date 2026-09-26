// STEP entities onto the B-rep (Part F.2).
//
// Everything hard about STEP is here rather than in the parser. The file
// parses; what it then says is a question of which of several hundred
// entity types it used, in which of several allowed spellings, with which
// conventions about orientation -- and the conventions are where the real
// work is. Three of them are worth knowing before reading any of this:
//
//   AN EDGE HAS TWO ORIENTATIONS AND THEY MULTIPLY. An EDGE_CURVE carries
//   a sense against its own curve; an ORIENTED_EDGE carries another
//   against the EDGE_CURVE. A loop traverses the edge forwards only when
//   the two agree, and a reader that honours one and forgets the other
//   produces faces whose boundaries run backwards -- which validates
//   structurally and tessellates inside out.
//
//   A FACE HAS ITS OWN. ADVANCED_FACE's same_sense says whether the
//   face's outward normal is the surface's own, which is exactly the
//   Orientation this kernel stores on a Face, and a FACE_BOUND carries
//   yet another orientation on top of the loop inside it.
//
//   PARAMETERS ARE NOT NORMALISED. A STEP circle is parameterised by
//   angle in degrees if the file says so, a B-spline by whatever knots it
//   was given, and a trimmed curve by either parameters or points. The
//   edge's range has to be worked out from the vertices rather than
//   assumed, because the two disagree often enough in real files that
//   assuming is how a reader ends up with edges that run the long way
//   round.

#include "cad_pcurve.h"
#include "cad_step.h"
#include "cad_validate.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace cad {
namespace {

// --- Reading arguments ---------------------------------------------------

const StepEntity *Resolve(const StepFile &file, const StepValue &value) {
    return value.kind == StepValue::Kind::Reference ? file.Get(value.reference) : nullptr;
}

bool ArgumentAt(const StepEntity &entity, std::size_t index, const StepValue **out) {
    if (index >= entity.arguments.size()) return false;
    *out = &entity.arguments[index];
    return true;
}

double Number(const StepEntity &entity, std::size_t index, double fallback) {
    const StepValue *value = nullptr;
    if (!ArgumentAt(entity, index, &value) || !value->IsNumber()) return fallback;
    return value->AsDouble();
}

bool Flag(const StepEntity &entity, std::size_t index, bool fallback) {
    const StepValue *value = nullptr;
    if (!ArgumentAt(entity, index, &value) || value->kind != StepValue::Kind::Enumeration) {
        return fallback;
    }
    return value->text == "T";
}

// The sub-record of a complex instance with this type, or the entity
// itself when it is simple and matches.
const StepEntity *Part(const StepEntity &entity, const std::string &type) {
    if (!entity.IsComplex()) return entity.type == type ? &entity : nullptr;
    for (const StepEntity &part : entity.parts) {
        if (part.type == type) return &part;
    }
    return nullptr;
}

// --- The reader ----------------------------------------------------------

class Reader {
public:
    Reader(const StepFile &file, Model *model, StepReadReport *report, const StepReadOptions &options)
        : file_(file), model_(model), report_(report), options_(options) {}

    bool Run();

private:
    void Unsupported(const StepEntity &entity) { ++report_->unsupported[entity.type]; }
    void Warn(const std::string &message) {
        if (report_->warnings.size() < 64) report_->warnings.push_back(message);
    }

    bool ReadPoint(const StepEntity &entity, Vec3d *out);
    bool ReadDirection(const StepEntity &entity, Vec3d *out);
    // AXIS2_PLACEMENT_3D: a location, an axis (local z) and a reference
    // direction (local x). Both directions are optional in the schema and
    // routinely absent in real files, which is why the defaults are here
    // rather than at each call site.
    bool ReadPlacement(const StepEntity &entity, Vec3d *origin, Vec3d *axis, Vec3d *ref);

    int ReadCurve(const StepEntity &entity);
    int ReadSurface(const StepEntity &entity);
    bool NurbsCurveFrom(const StepEntity &entity, std::shared_ptr<const Curve3> *out);
    bool NurbsSurfaceFrom(const StepEntity &entity, std::shared_ptr<const Surface> *out);

    EntityId ReadVertex(const StepEntity &entity);
    EntityId ReadEdge(const StepEntity &entity);
    bool ReadLoop(const StepEntity &entity, bool bound_orientation, std::vector<EntityId> *coedges);
    EntityId ReadFace(const StepEntity &entity);
    bool ReadShell(const StepEntity &entity, std::vector<EntityId> *faces);

    const StepFile &file_;
    Model *model_;
    StepReadReport *report_;
    StepReadOptions options_;
    std::map<int, int> curves_;
    std::map<int, int> surfaces_;
    std::map<int, EntityId> vertices_;
    std::map<int, EntityId> edges_;
    // An EDGE_CURVE's sense, kept so that an ORIENTED_EDGE can multiply
    // the two together. See the note at the top of the file.
    std::map<int, bool> edge_sense_;
};

bool Reader::ReadPoint(const StepEntity &entity, Vec3d *out) {
    const StepEntity *record = Part(entity, "CARTESIAN_POINT");
    if (record == nullptr) return false;
    const StepValue *coordinates = nullptr;
    if (!ArgumentAt(*record, 1, &coordinates) || coordinates->kind != StepValue::Kind::List) return false;
    const std::vector<StepValue> &items = coordinates->items;
    out->x = items.size() > 0 ? items[0].AsDouble() : 0.0;
    out->y = items.size() > 1 ? items[1].AsDouble() : 0.0;
    out->z = items.size() > 2 ? items[2].AsDouble() : 0.0;
    return true;
}

bool Reader::ReadDirection(const StepEntity &entity, Vec3d *out) {
    const StepEntity *record = Part(entity, "DIRECTION");
    if (record == nullptr) return false;
    const StepValue *ratios = nullptr;
    if (!ArgumentAt(*record, 1, &ratios) || ratios->kind != StepValue::Kind::List) return false;
    const std::vector<StepValue> &items = ratios->items;
    out->x = items.size() > 0 ? items[0].AsDouble() : 0.0;
    out->y = items.size() > 1 ? items[1].AsDouble() : 0.0;
    out->z = items.size() > 2 ? items[2].AsDouble() : 0.0;
    return out->Length() > 0.0;
}

bool Reader::ReadPlacement(const StepEntity &entity, Vec3d *origin, Vec3d *axis, Vec3d *ref) {
    const StepEntity *record = Part(entity, "AXIS2_PLACEMENT_3D");
    if (record == nullptr) return false;
    const StepEntity *location = Resolve(file_, record->arguments.size() > 1 ? record->arguments[1]
                                                                            : StepValue{});
    if (location == nullptr || !ReadPoint(*location, origin)) return false;
    *axis = Vec3d{0.0, 0.0, 1.0};
    *ref = Vec3d{1.0, 0.0, 0.0};
    if (record->arguments.size() > 2) {
        const StepEntity *z = Resolve(file_, record->arguments[2]);
        if (z != nullptr) ReadDirection(*z, axis);
    }
    if (record->arguments.size() > 3) {
        const StepEntity *x = Resolve(file_, record->arguments[3]);
        if (x != nullptr) ReadDirection(*x, ref);
    }
    *axis = axis->Normalized();
    // The reference direction is only a hint: the schema says the local x
    // is its component perpendicular to the axis, and files exist where
    // the two are not perpendicular to begin with.
    const Vec3d projected = *ref - *axis * axis->Dot(*ref);
    *ref = projected.LengthSquared() > 0.0 ? projected.Normalized() : axis->AnyPerpendicular();
    return true;
}

bool Reader::NurbsCurveFrom(const StepEntity &entity, std::shared_ptr<const Curve3> *out) {
    const StepEntity *with_knots = Part(entity, "B_SPLINE_CURVE_WITH_KNOTS");
    const StepEntity *base = Part(entity, "B_SPLINE_CURVE");
    if (with_knots == nullptr) return false;
    if (base == nullptr) base = with_knots;

    const int degree = static_cast<int>(Number(*base, 0, 3));
    const StepValue *control = nullptr;
    if (!ArgumentAt(*base, 1, &control) || control->kind != StepValue::Kind::List) return false;

    std::vector<Vec4d> points;
    for (const StepValue &reference : control->items) {
        const StepEntity *p = Resolve(file_, reference);
        Vec3d position;
        if (p == nullptr || !ReadPoint(*p, &position)) return false;
        points.push_back(Vec4d{position.x, position.y, position.z, 1.0});
    }

    // Rational curves arrive as a complex instance with a
    // RATIONAL_B_SPLINE_CURVE part carrying the weights. Control points
    // are stored weighted here, so the weights are multiplied in.
    if (const StepEntity *rational = Part(entity, "RATIONAL_B_SPLINE_CURVE")) {
        const StepValue *weights = nullptr;
        if (ArgumentAt(*rational, 0, &weights) && weights->kind == StepValue::Kind::List) {
            for (std::size_t i = 0; i < points.size() && i < weights->items.size(); ++i) {
                const double w = weights->items[i].AsDouble();
                points[i] = Vec4d{points[i].x * w, points[i].y * w, points[i].z * w, w};
            }
        }
    }

    // The knots are given as distinct values with multiplicities, which
    // is a better representation than the flat vector and is not the one
    // anything else here uses.
    const StepValue *multiplicities = nullptr;
    const StepValue *values = nullptr;
    if (!ArgumentAt(*with_knots, 5, &multiplicities) || !ArgumentAt(*with_knots, 6, &values)) {
        // The argument positions differ between the simple and complex
        // spellings; in the complex one B_SPLINE_CURVE_WITH_KNOTS carries
        // only its own three.
        if (!ArgumentAt(*with_knots, 0, &multiplicities) || !ArgumentAt(*with_knots, 1, &values)) {
            return false;
        }
    }
    if (multiplicities->kind != StepValue::Kind::List || values->kind != StepValue::Kind::List) {
        return false;
    }
    std::vector<double> knots;
    for (std::size_t i = 0; i < values->items.size() && i < multiplicities->items.size(); ++i) {
        const long long count = multiplicities->items[i].integer;
        for (long long k = 0; k < count; ++k) knots.push_back(values->items[i].AsDouble());
    }

    auto curve = std::make_shared<NurbsCurve3>();
    std::string error;
    if (!NurbsCurve3::Create(degree, knots, points, curve.get(), &error)) return false;
    *out = curve;
    return true;
}

bool Reader::NurbsSurfaceFrom(const StepEntity &entity, std::shared_ptr<const Surface> *out) {
    const StepEntity *with_knots = Part(entity, "B_SPLINE_SURFACE_WITH_KNOTS");
    const StepEntity *base = Part(entity, "B_SPLINE_SURFACE");
    if (with_knots == nullptr) return false;
    if (base == nullptr) base = with_knots;

    const int degree_u = static_cast<int>(Number(*base, 0, 3));
    const int degree_v = static_cast<int>(Number(*base, 1, 3));
    const StepValue *grid = nullptr;
    if (!ArgumentAt(*base, 2, &grid) || grid->kind != StepValue::Kind::List) return false;

    // STEP stores the grid as a list of u-rows, each a list over v.
    const int count_u = static_cast<int>(grid->items.size());
    if (count_u == 0 || grid->items[0].kind != StepValue::Kind::List) return false;
    const int count_v = static_cast<int>(grid->items[0].items.size());
    std::vector<Vec4d> control;
    for (const StepValue &row : grid->items) {
        if (static_cast<int>(row.items.size()) != count_v) return false;
        for (const StepValue &reference : row.items) {
            const StepEntity *p = Resolve(file_, reference);
            Vec3d position;
            if (p == nullptr || !ReadPoint(*p, &position)) return false;
            control.push_back(Vec4d{position.x, position.y, position.z, 1.0});
        }
    }
    if (const StepEntity *rational = Part(entity, "RATIONAL_B_SPLINE_SURFACE")) {
        const StepValue *weights = nullptr;
        if (ArgumentAt(*rational, 0, &weights) && weights->kind == StepValue::Kind::List) {
            std::size_t at = 0;
            for (const StepValue &row : weights->items) {
                for (const StepValue &w : row.items) {
                    if (at >= control.size()) break;
                    const double weight = w.AsDouble();
                    control[at] = Vec4d{control[at].x * weight, control[at].y * weight,
                                        control[at].z * weight, weight};
                    ++at;
                }
            }
        }
    }

    auto knots_from = [](const StepValue *multiplicities, const StepValue *values,
                         std::vector<double> *out_knots) {
        if (multiplicities == nullptr || values == nullptr) return false;
        if (multiplicities->kind != StepValue::Kind::List || values->kind != StepValue::Kind::List) {
            return false;
        }
        for (std::size_t i = 0; i < values->items.size() && i < multiplicities->items.size(); ++i) {
            const long long count = multiplicities->items[i].integer;
            for (long long k = 0; k < count; ++k) out_knots->push_back(values->items[i].AsDouble());
        }
        return true;
    };

    const StepValue *mult_u = nullptr;
    const StepValue *mult_v = nullptr;
    const StepValue *knots_u_values = nullptr;
    const StepValue *knots_v_values = nullptr;
    const bool simple = ArgumentAt(*with_knots, 7, &mult_u) && ArgumentAt(*with_knots, 8, &mult_v) &&
                        ArgumentAt(*with_knots, 9, &knots_u_values) &&
                        ArgumentAt(*with_knots, 10, &knots_v_values);
    if (!simple) {
        if (!ArgumentAt(*with_knots, 0, &mult_u) || !ArgumentAt(*with_knots, 1, &mult_v) ||
            !ArgumentAt(*with_knots, 2, &knots_u_values) ||
            !ArgumentAt(*with_knots, 3, &knots_v_values)) {
            return false;
        }
    }
    std::vector<double> knots_u;
    std::vector<double> knots_v;
    if (!knots_from(mult_u, knots_u_values, &knots_u)) return false;
    if (!knots_from(mult_v, knots_v_values, &knots_v)) return false;

    auto surface = std::make_shared<NurbsSurface>();
    std::string error;
    if (!NurbsSurface::Create(degree_u, degree_v, knots_u, knots_v, control, count_u, count_v,
                              surface.get(), &error)) {
        return false;
    }
    *out = surface;
    return true;
}

int Reader::ReadCurve(const StepEntity &entity) {
    const auto found = curves_.find(entity.id);
    if (found != curves_.end()) return found->second;
    int index = -1;
    std::shared_ptr<const Curve3> curve;

    if (Part(entity, "LINE") != nullptr) {
        const StepEntity *record = Part(entity, "LINE");
        const StepEntity *origin = Resolve(file_, record->arguments.size() > 1 ? record->arguments[1]
                                                                              : StepValue{});
        const StepEntity *vector = Resolve(file_, record->arguments.size() > 2 ? record->arguments[2]
                                                                               : StepValue{});
        Vec3d point;
        Vec3d direction{1.0, 0.0, 0.0};
        double magnitude = 1.0;
        if (origin != nullptr && ReadPoint(*origin, &point) && vector != nullptr) {
            const StepEntity *vec = Part(*vector, "VECTOR");
            if (vec != nullptr) {
                const StepEntity *d = Resolve(file_, vec->arguments.size() > 1 ? vec->arguments[1]
                                                                               : StepValue{});
                if (d != nullptr) ReadDirection(*d, &direction);
                magnitude = Number(*vec, 2, 1.0);
            }
            curve = std::make_shared<Line3>(point, direction.Normalized() * magnitude, -1e6, 1e6);
        }
    } else if (Part(entity, "CIRCLE") != nullptr) {
        const StepEntity *record = Part(entity, "CIRCLE");
        const StepEntity *placement = Resolve(file_, record->arguments.size() > 1 ? record->arguments[1]
                                                                                  : StepValue{});
        Vec3d origin;
        Vec3d axis;
        Vec3d ref;
        if (placement != nullptr && ReadPlacement(*placement, &origin, &axis, &ref)) {
            curve = std::make_shared<Circle3>(origin, ref, axis.Cross(ref), Number(*record, 2, 1.0), 0.0,
                                              kTwoPi);
        }
    } else if (Part(entity, "ELLIPSE") != nullptr) {
        const StepEntity *record = Part(entity, "ELLIPSE");
        const StepEntity *placement = Resolve(file_, record->arguments.size() > 1 ? record->arguments[1]
                                                                                  : StepValue{});
        Vec3d origin;
        Vec3d axis;
        Vec3d ref;
        if (placement != nullptr && ReadPlacement(*placement, &origin, &axis, &ref)) {
            curve = std::make_shared<Ellipse3>(origin, ref, axis.Cross(ref), Number(*record, 2, 1.0),
                                               Number(*record, 3, 1.0), 0.0, kTwoPi);
        }
    } else if (Part(entity, "B_SPLINE_CURVE_WITH_KNOTS") != nullptr) {
        NurbsCurveFrom(entity, &curve);
    }

    if (curve != nullptr) {
        index = model_->AddCurve(curve);
    } else {
        Unsupported(entity);
    }
    curves_[entity.id] = index;
    return index;
}

int Reader::ReadSurface(const StepEntity &entity) {
    const auto found = surfaces_.find(entity.id);
    if (found != surfaces_.end()) return found->second;
    int index = -1;
    std::shared_ptr<const Surface> surface;
    Vec3d origin;
    Vec3d axis;
    Vec3d ref;

    // One `record`, assigned in turn: declaring it in each branch would
    // shadow the one before it, which this build treats as an error --
    // rightly, since a shadowed pointer in a chain like this is how the
    // wrong entity ends up being read.
    const StepEntity *record = nullptr;
    if ((record = Part(entity, "PLANE")) != nullptr) {
        const StepEntity *placement = Resolve(file_, record->arguments.size() > 1 ? record->arguments[1]
                                                                                  : StepValue{});
        if (placement != nullptr && ReadPlacement(*placement, &origin, &axis, &ref)) {
            // The plane's own u and v are the placement's x and y, and a
            // generous domain: the face's bounds decide what is actually
            // used, and a domain trimmed to nothing loses p-curves.
            surface = std::make_shared<PlaneSurface>(origin, ref, axis.Cross(ref), -1e5, 1e5, -1e5, 1e5);
        }
    } else if ((record = Part(entity, "CYLINDRICAL_SURFACE")) != nullptr) {
        const StepEntity *placement = Resolve(file_, record->arguments.size() > 1 ? record->arguments[1]
                                                                                  : StepValue{});
        if (placement != nullptr && ReadPlacement(*placement, &origin, &axis, &ref)) {
            surface = std::make_shared<CylinderSurface>(origin, ref, axis, Number(*record, 2, 1.0), 0.0,
                                                        kTwoPi, -1e5, 1e5);
        }
    } else if ((record = Part(entity, "CONICAL_SURFACE")) != nullptr) {
        const StepEntity *placement = Resolve(file_, record->arguments.size() > 1 ? record->arguments[1]
                                                                                  : StepValue{});
        if (placement != nullptr && ReadPlacement(*placement, &origin, &axis, &ref)) {
            // STEP gives the radius at the placement's plane and the half
            // angle; this kernel's cone is written from its apex.
            const double radius = Number(*record, 2, 1.0);
            const double half_angle = Number(*record, 3, 0.25 * kPi);
            const double to_apex = std::fabs(std::tan(half_angle)) > 1e-12
                                       ? radius / std::tan(half_angle)
                                       : 0.0;
            surface = std::make_shared<ConeSurface>(origin - axis * to_apex, ref, axis, half_angle, 0.0,
                                                    kTwoPi, 0.0, 1e5);
        }
    } else if ((record = Part(entity, "SPHERICAL_SURFACE")) != nullptr) {
        const StepEntity *placement = Resolve(file_, record->arguments.size() > 1 ? record->arguments[1]
                                                                                  : StepValue{});
        if (placement != nullptr && ReadPlacement(*placement, &origin, &axis, &ref)) {
            surface = std::make_shared<SphereSurface>(origin, Number(*record, 2, 1.0), ref, axis);
        }
    } else if ((record = Part(entity, "TOROIDAL_SURFACE")) != nullptr) {
        const StepEntity *placement = Resolve(file_, record->arguments.size() > 1 ? record->arguments[1]
                                                                                  : StepValue{});
        if (placement != nullptr && ReadPlacement(*placement, &origin, &axis, &ref)) {
            surface = std::make_shared<TorusSurface>(origin, ref, axis, Number(*record, 2, 2.0),
                                                     Number(*record, 3, 1.0));
        }
    } else if (Part(entity, "B_SPLINE_SURFACE_WITH_KNOTS") != nullptr) {
        NurbsSurfaceFrom(entity, &surface);
    }

    if (surface != nullptr) {
        index = model_->AddSurface(surface);
    } else {
        Unsupported(entity);
    }
    surfaces_[entity.id] = index;
    return index;
}

EntityId Reader::ReadVertex(const StepEntity &entity) {
    const auto found = vertices_.find(entity.id);
    if (found != vertices_.end()) return found->second;
    EntityId id = kNoEntity;
    const StepEntity *record = Part(entity, "VERTEX_POINT");
    if (record != nullptr) {
        const StepEntity *point = Resolve(file_, record->arguments.size() > 1 ? record->arguments[1]
                                                                              : StepValue{});
        Vec3d position;
        if (point != nullptr && ReadPoint(*point, &position)) id = model_->AddVertex(position);
    }
    if (id == kNoEntity) Unsupported(entity);
    vertices_[entity.id] = id;
    return id;
}

EntityId Reader::ReadEdge(const StepEntity &entity) {
    const auto found = edges_.find(entity.id);
    if (found != edges_.end()) return found->second;
    EntityId id = kNoEntity;
    const StepEntity *record = Part(entity, "EDGE_CURVE");
    if (record != nullptr && record->arguments.size() >= 5) {
        const StepEntity *start = Resolve(file_, record->arguments[1]);
        const StepEntity *end = Resolve(file_, record->arguments[2]);
        const StepEntity *geometry = Resolve(file_, record->arguments[3]);
        const bool same_sense = Flag(*record, 4, true);
        if (start != nullptr && end != nullptr && geometry != nullptr) {
            const EntityId a = ReadVertex(*start);
            const EntityId b = ReadVertex(*end);
            const int curve = ReadCurve(*geometry);
            if (a != kNoEntity && b != kNoEntity && curve >= 0) {
                // The parameter range comes from the vertices, not from
                // the file: a STEP edge carries no range of its own, and
                // working it out from the geometry is the only way to get
                // an arc that runs the short way round when it should.
                const Curve3 *c = model_->CurveAt(curve);
                double lo = 0.0;
                double hi = 1.0;
                c->Domain(&lo, &hi);
                double t_start = lo;
                double t_end = hi;
                Vec3d hit;
                c->ClosestPoint(model_->GetVertex(a)->point, &t_start, &hit);
                c->ClosestPoint(model_->GetVertex(b)->point, &t_end, &hit);
                const bool periodic = c->Kind() == CurveKind::Circle || c->Kind() == CurveKind::Ellipse;
                const bool closed = a == b;
                if (closed) {
                    // A whole circle: the two ends are the same point, so
                    // the range is one full turn from wherever it starts.
                    t_end = t_start + (periodic ? kTwoPi : hi - lo);
                } else if (periodic) {
                    // STEP means the arc travelling in the curve's own
                    // direction, so an end parameter behind the start is
                    // one turn ahead of it.
                    if (t_end <= t_start) t_end += kTwoPi;
                }
                if (periodic) {
                    // AND IT IS PUT WHERE THE CURVE'S DOMAIN IS. An angle
                    // only means anything modulo a turn, so [3pi/2, 5pi/2]
                    // and [-pi/2, pi/2] are the same arc -- but they are
                    // not the same thing to ask a kernel about. Round-off
                    // at a surface's pole, where the angle is decided by
                    // the last bit of a cosine that is supposed to be
                    // zero, comes out differently for the two, and a
                    // sphere's seam then projects to a different place in
                    // parameter space. The canonical form is the one
                    // starting inside the curve's own domain, and it is
                    // free to insist on it here.
                    while (t_start >= lo + kTwoPi) {
                        t_start -= kTwoPi;
                        t_end -= kTwoPi;
                    }
                    while (t_start < lo) {
                        t_start += kTwoPi;
                        t_end += kTwoPi;
                    }
                }
                id = model_->AddEdge(curve, a, b, t_start, t_end);
                edge_sense_[entity.id] = same_sense;
            }
        }
    }
    if (id == kNoEntity) Unsupported(entity);
    edges_[entity.id] = id;
    return id;
}

bool Reader::ReadLoop(const StepEntity &entity, bool bound_orientation, std::vector<EntityId> *coedges) {
    const StepEntity *record = Part(entity, "EDGE_LOOP");
    if (record == nullptr) {
        Unsupported(entity);
        return false;
    }
    const StepValue *list = nullptr;
    if (!ArgumentAt(*record, 1, &list) || list->kind != StepValue::Kind::List) return false;

    std::vector<std::pair<EntityId, Orientation>> uses;
    for (const StepValue &reference : list->items) {
        const StepEntity *oriented = Resolve(file_, reference);
        if (oriented == nullptr) return false;
        const StepEntity *use = Part(*oriented, "ORIENTED_EDGE");
        if (use == nullptr || use->arguments.size() < 5) return false;
        const StepEntity *edge = Resolve(file_, use->arguments[3]);
        if (edge == nullptr) return false;
        const EntityId built = ReadEdge(*edge);
        if (built == kNoEntity) return false;
        // The two orientations multiply -- see the note at the top.
        const bool oriented_sense = Flag(*use, 4, true);
        const bool edge_sense = edge_sense_.count(edge->id) != 0 ? edge_sense_[edge->id] : true;
        const bool forward = oriented_sense == edge_sense;
        uses.push_back({built, forward ? Orientation::Forward : Orientation::Reversed});
    }
    if (uses.empty()) return false;
    // And the bound carries one more.
    if (!bound_orientation) {
        std::reverse(uses.begin(), uses.end());
        for (auto &use : uses) use.second = Flip(use.second);
    }
    for (const auto &use : uses) coedges->push_back(model_->AddCoEdge(use.first, use.second));
    return true;
}

EntityId Reader::ReadFace(const StepEntity &entity) {
    const StepEntity *record = Part(entity, "ADVANCED_FACE");
    if (record == nullptr) record = Part(entity, "FACE_SURFACE");
    if (record == nullptr || record->arguments.size() < 4) {
        Unsupported(entity);
        return kNoEntity;
    }
    const StepEntity *geometry = Resolve(file_, record->arguments[2]);
    if (geometry == nullptr) return kNoEntity;
    const int surface = ReadSurface(*geometry);
    if (surface < 0) return kNoEntity;
    const bool same_sense = Flag(*record, 3, true);

    const StepValue *bounds = nullptr;
    if (!ArgumentAt(*record, 1, &bounds) || bounds->kind != StepValue::Kind::List) return kNoEntity;

    std::vector<EntityId> loops;
    bool saw_outer = false;
    for (const StepValue &reference : bounds->items) {
        const StepEntity *bound = Resolve(file_, reference);
        if (bound == nullptr) continue;
        const StepEntity *record_bound = Part(*bound, "FACE_OUTER_BOUND");
        bool is_outer = record_bound != nullptr;
        if (record_bound == nullptr) record_bound = Part(*bound, "FACE_BOUND");
        if (record_bound == nullptr || record_bound->arguments.size() < 3) continue;
        const StepEntity *loop = Resolve(file_, record_bound->arguments[1]);
        if (loop == nullptr) continue;
        const bool orientation = Flag(*record_bound, 2, true);
        std::vector<EntityId> coedges;
        if (!ReadLoop(*loop, orientation, &coedges)) {
            Warn("a bound of face #" + std::to_string(entity.id) + " could not be built");
            continue;
        }
        // Exactly one loop is the outer one. A file that marks none --
        // which happens, since FACE_BOUND is legal for all of them -- has
        // its first taken as the outer, because a face with none at all
        // is not a face.
        if (is_outer && saw_outer) is_outer = false;
        if (is_outer) saw_outer = true;
        loops.push_back(model_->AddLoop(coedges, is_outer));
    }
    if (loops.empty()) {
        Warn("face #" + std::to_string(entity.id) + " has no usable bounds");
        return kNoEntity;
    }
    if (!saw_outer) model_->GetLoop(loops.front())->is_outer = true;
    return model_->AddFace(surface, same_sense ? Orientation::Forward : Orientation::Reversed, loops,
                           "step");
}

bool Reader::ReadShell(const StepEntity &entity, std::vector<EntityId> *faces) {
    const StepEntity *record = Part(entity, "CLOSED_SHELL");
    if (record == nullptr) record = Part(entity, "OPEN_SHELL");
    if (record == nullptr || record->arguments.size() < 2) {
        Unsupported(entity);
        return false;
    }
    const StepValue *list = nullptr;
    if (!ArgumentAt(*record, 1, &list) || list->kind != StepValue::Kind::List) return false;
    for (const StepValue &reference : list->items) {
        const StepEntity *face = Resolve(file_, reference);
        if (face == nullptr) continue;
        const EntityId built = ReadFace(*face);
        if (built != kNoEntity) faces->push_back(built);
    }
    return !faces->empty();
}

bool Reader::Run() {
    std::vector<const StepEntity *> solids = file_.OfType("MANIFOLD_SOLID_BREP");
    for (const StepEntity *brep : file_.OfType("BREP_WITH_VOIDS")) solids.push_back(brep);
    for (const StepEntity *solid : solids) {
        const StepEntity *record = Part(*solid, "MANIFOLD_SOLID_BREP");
        if (record == nullptr) record = Part(*solid, "BREP_WITH_VOIDS");
        if (record == nullptr || record->arguments.size() < 2) continue;
        const StepEntity *outer = Resolve(file_, record->arguments[1]);
        if (outer == nullptr) continue;
        std::vector<EntityId> faces;
        if (!ReadShell(*outer, &faces)) {
            Warn("solid #" + std::to_string(solid->id) + " has no usable outer shell");
            continue;
        }
        std::vector<EntityId> shells{model_->AddShell(faces, true)};
        // The voids of a BREP_WITH_VOIDS are inner shells, which is
        // exactly what Part B.1 carries them as.
        if (record->type == "BREP_WITH_VOIDS" && record->arguments.size() >= 3 &&
            record->arguments[2].kind == StepValue::Kind::List) {
            for (const StepValue &reference : record->arguments[2].items) {
                const StepEntity *void_shell = Resolve(file_, reference);
                if (void_shell == nullptr) continue;
                const StepEntity *inner = Part(*void_shell, "ORIENTED_CLOSED_SHELL");
                const StepEntity *target = inner != nullptr && inner->arguments.size() > 2
                                               ? Resolve(file_, inner->arguments[2])
                                               : void_shell;
                std::vector<EntityId> void_faces;
                if (target != nullptr && ReadShell(*target, &void_faces)) {
                    shells.push_back(model_->AddShell(void_faces, false));
                }
            }
        }
        const std::string name = record->arguments.empty() ? "" : record->arguments[0].text;
        report_->bodies.push_back(model_->AddBody(shells, name));
        report_->body_names.push_back(name);
    }

    // A shell-based surface model is not a solid, but it is what a lot of
    // files carry, and reading it as an open body is more use than
    // refusing it.
    for (const StepEntity *model_entity : file_.OfType("SHELL_BASED_SURFACE_MODEL")) {
        const StepEntity *record = Part(*model_entity, "SHELL_BASED_SURFACE_MODEL");
        if (record == nullptr || record->arguments.size() < 2 ||
            record->arguments[1].kind != StepValue::Kind::List) {
            continue;
        }
        std::vector<EntityId> faces;
        for (const StepValue &reference : record->arguments[1].items) {
            const StepEntity *shell = Resolve(file_, reference);
            if (shell != nullptr) ReadShell(*shell, &faces);
        }
        if (faces.empty()) continue;
        report_->bodies.push_back(model_->AddBody({model_->AddShell(faces, true)},
                                                  record->arguments[0].text));
        report_->body_names.push_back(record->arguments[0].text);
    }

    if (report_->bodies.empty()) {
        report_->error = "the file has no solid in it that this reader could build: no "
                         "MANIFOLD_SOLID_BREP and no SHELL_BASED_SURFACE_MODEL was usable";
        return false;
    }

    if (options_.build_pcurves) {
        PCurveOptions pcurve_options;
        pcurve_options.tolerance = options_.tolerance;
        std::string error;
        if (!BuildAllPCurves(model_, pcurve_options, &error)) {
            Warn("p-curves could not all be built: " + error);
        }
    }
    if (options_.require_valid) {
        for (EntityId body : report_->bodies) {
            ValidationReport validation;
            if (!ValidateBody(*model_, body, &validation, {})) {
                report_->error = "a solid in this file is not valid:\n" + validation.Summary();
                return false;
            }
        }
    }
    report_->ok = true;
    return true;
}

}  // namespace

bool ReadStepShapes(const StepFile &file, Model *out, StepReadReport *report,
                    const StepReadOptions &options) {
    *report = StepReadReport{};
    Reader reader(file, out, report, options);
    return reader.Run();
}

bool ReadStepText(const std::string &text, Model *out, StepReadReport *report,
                  const StepReadOptions &options) {
    *report = StepReadReport{};
    StepFile file;
    if (!ParseStepFile(text, &file, &report->error)) return false;
    return ReadStepShapes(file, out, report, options);
}

}  // namespace cad
