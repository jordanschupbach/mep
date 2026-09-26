// The B-rep onto STEP entities (Part F.3).
//
// ANALYTIC SURFACES STAY ANALYTIC. A cylinder goes out as a
// CYLINDRICAL_SURFACE, not as a B-spline that happens to be round, and
// that is not a nicety: the receiving system's own fillet, draft and
// shell operations all depend on knowing that a face is a cylinder, and a
// file that has thrown that away is a file whose recipient can only
// tessellate it. It is also the thing a round trip through another
// kernel tests hardest, since a reader that silently accepts an approximate
// surface will not notice it has been handed one.
//
// The writing itself is bookkeeping: every distinct point, direction and
// placement is emitted once and referred to afterwards, because a file
// that repeats CARTESIAN_POINT for every use of the origin is several
// times larger and is what a naive writer produces.

#include "cad_step.h"

#include <cmath>
#include <map>
#include <string>

namespace cad {
namespace {

class Writer {
public:
    Writer(const Model &model, const StepWriteOptions &options) : model_(model), options_(options) {}

    bool Run(const std::vector<EntityId> &bodies, std::string *out, std::string *error);

private:
    int Next() { return next_++; }
    int Emit(const std::string &type, const std::string &arguments) {
        const int id = Next();
        body_ += "#" + std::to_string(id) + "=" + type + "(" + arguments + ");\n";
        return id;
    }
    // For a complex instance, whose body is not "TYPE(args)".
    int EmitRaw(const std::string &text) {
        const int id = Next();
        body_ += "#" + std::to_string(id) + "=" + text + ";\n";
        return id;
    }
    static std::string Real(double v);
    static std::string Quote(const std::string &s) { return "'" + s + "'"; }
    static std::string Reference(int id) { return "#" + std::to_string(id); }
    static std::string List(const std::vector<int> &ids);

    int PointId(const Vec3d &p);
    int DirectionId(const Vec3d &d);
    int PlacementId(const Vec3d &origin, const Vec3d &axis, const Vec3d &ref);

    int CurveId(int index, const Curve3 &curve);
    int SurfaceId(int index, const Surface &surface);
    int VertexId(EntityId vertex);
    int EdgeId(EntityId edge);
    int LoopId(EntityId loop);
    int FaceId(EntityId face);
    int ShellId(EntityId shell);

    const Model &model_;
    StepWriteOptions options_;
    std::string body_;
    int next_ = 1;
    std::map<std::string, int> points_;
    std::map<std::string, int> directions_;
    std::map<int, int> curves_;
    std::map<int, int> surfaces_;
    std::map<EntityId, int> vertices_;
    std::map<EntityId, int> edges_;
    std::string error_;
};

std::string Writer::Real(double v) {
    if (!std::isfinite(v)) v = 0.0;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.15G", v);
    std::string text(buffer);
    if (text.find('.') == std::string::npos) {
        const std::size_t e = text.find('E');
        if (e == std::string::npos) {
            text += ".";
        } else {
            text.insert(e, ".");
        }
    }
    return text;
}

std::string Writer::List(const std::vector<int> &ids) {
    std::string out = "(";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i != 0) out += ",";
        out += Reference(ids[i]);
    }
    out += ")";
    return out;
}

int Writer::PointId(const Vec3d &p) {
    const std::string key = Real(p.x) + "|" + Real(p.y) + "|" + Real(p.z);
    const auto found = points_.find(key);
    if (found != points_.end()) return found->second;
    const int id = Emit("CARTESIAN_POINT", "''," + std::string("(") + Real(p.x) + "," + Real(p.y) + "," +
                                               Real(p.z) + ")");
    points_[key] = id;
    return id;
}

int Writer::DirectionId(const Vec3d &d) {
    const Vec3d unit = d.Normalized();
    const std::string key = Real(unit.x) + "|" + Real(unit.y) + "|" + Real(unit.z);
    const auto found = directions_.find(key);
    if (found != directions_.end()) return found->second;
    const int id = Emit("DIRECTION", "''," + std::string("(") + Real(unit.x) + "," + Real(unit.y) + "," +
                                          Real(unit.z) + ")");
    directions_[key] = id;
    return id;
}

int Writer::PlacementId(const Vec3d &origin, const Vec3d &axis, const Vec3d &ref) {
    const int o = PointId(origin);
    const int z = DirectionId(axis);
    const int x = DirectionId(ref);
    return Emit("AXIS2_PLACEMENT_3D", "''," + Reference(o) + "," + Reference(z) + "," + Reference(x));
}

int Writer::CurveId(int index, const Curve3 &curve) {
    const auto found = curves_.find(index);
    if (found != curves_.end()) return found->second;
    int id = 0;
    if (const auto *line = dynamic_cast<const Line3 *>(&curve)) {
        const int origin = PointId(line->Origin());
        const double length = line->Direction().Length();
        const int direction = DirectionId(line->Direction());
        const int vector = Emit("VECTOR", "''," + Reference(direction) + "," + Real(length));
        id = Emit("LINE", "''," + Reference(origin) + "," + Reference(vector));
    } else if (const auto *circle = dynamic_cast<const Circle3 *>(&curve)) {
        const int placement =
            PlacementId(circle->Center(), circle->PlaneNormal(), circle->XAxis());
        id = Emit("CIRCLE", "''," + Reference(placement) + "," + Real(circle->Radius()));
    } else if (const auto *ellipse = dynamic_cast<const Ellipse3 *>(&curve)) {
        const int placement =
            PlacementId(ellipse->Center(), ellipse->PlaneNormal(), ellipse->XAxis());
        id = Emit("ELLIPSE", "''," + Reference(placement) + "," + Real(ellipse->MajorRadius()) + "," +
                                 Real(ellipse->MinorRadius()));
    } else {
        // Everything else goes out as the NURBS it can always be turned
        // into. Part A's ToNurbs is exact for every analytic family, so
        // this loses the *name* of the shape and not the shape.
        int degree = 3;
        std::vector<double> knots;
        std::vector<Vec4d> control;
        if (!curve.ToNurbs(&degree, &knots, &control) || control.empty()) {
            error_ = "a curve could not be written: it is neither a line, a circle, an ellipse, nor "
                     "convertible to a B-spline";
            return 0;
        }
        std::vector<int> point_ids;
        bool rational = false;
        for (const Vec4d &c : control) {
            if (std::fabs(c.w - 1.0) > 1e-12) rational = true;
            const double w = c.w != 0.0 ? c.w : 1.0;
            point_ids.push_back(PointId(Vec3d{c.x / w, c.y / w, c.z / w}));
        }
        // Distinct knots with multiplicities, which is how STEP says it.
        std::vector<double> distinct;
        std::vector<int> multiplicity;
        for (double k : knots) {
            if (!distinct.empty() && std::fabs(k - distinct.back()) < 1e-12) {
                ++multiplicity.back();
            } else {
                distinct.push_back(k);
                multiplicity.push_back(1);
            }
        }
        std::string knot_values = "(";
        std::string knot_counts = "(";
        for (std::size_t i = 0; i < distinct.size(); ++i) {
            if (i != 0) {
                knot_values += ",";
                knot_counts += ",";
            }
            knot_values += Real(distinct[i]);
            knot_counts += std::to_string(multiplicity[i]);
        }
        knot_values += ")";
        knot_counts += ")";
        const std::string common = "''," + std::to_string(degree) + "," + List(point_ids) +
                                   ",.UNSPECIFIED.,.F.,.F.," + knot_counts + "," + knot_values +
                                   ",.UNSPECIFIED.";
        if (!rational) {
            id = Emit("B_SPLINE_CURVE_WITH_KNOTS", common);
        } else {
            std::string weights = "(";
            for (std::size_t i = 0; i < control.size(); ++i) {
                if (i != 0) weights += ",";
                weights += Real(control[i].w);
            }
            weights += ")";
            // The complex form, which is how STEP expresses a rational
            // B-spline: several simple records making one instance.
            id = EmitRaw(std::string("(BOUNDED_CURVE()B_SPLINE_CURVE(") +
                     std::to_string(degree) + "," + List(point_ids) +
                          ",.UNSPECIFIED.,.F.,.F.)B_SPLINE_CURVE_WITH_KNOTS(" + knot_counts + "," +
                          knot_values + ",.UNSPECIFIED.)CURVE()GEOMETRIC_REPRESENTATION_ITEM()"
                          "RATIONAL_B_SPLINE_CURVE(" + weights + ")REPRESENTATION_ITEM(''))");
        }
    }
    curves_[index] = id;
    return id;
}

int Writer::SurfaceId(int index, const Surface &surface) {
    const auto found = surfaces_.find(index);
    if (found != surfaces_.end()) return found->second;
    int id = 0;
    if (const auto *plane = dynamic_cast<const PlaneSurface *>(&surface)) {
        const Vec3d origin = plane->Point(0.0, 0.0);
        const Vec3d x = (plane->Point(1.0, 0.0) - origin).Normalized();
        const int placement = PlacementId(origin, plane->PlaneNormal(), x);
        id = Emit("PLANE", "''," + Reference(placement));
    } else if (const auto *cylinder = dynamic_cast<const CylinderSurface *>(&surface)) {
        const Vec3d origin = cylinder->Origin();
        const Vec3d x = (cylinder->Point(0.0, 0.0) - origin).Normalized();
        const int placement = PlacementId(origin, cylinder->Axis(), x);
        id = Emit("CYLINDRICAL_SURFACE", "''," + Reference(placement) + "," + Real(cylinder->Radius()));
    } else if (const auto *sphere = dynamic_cast<const SphereSurface *>(&surface)) {
        // Its real axes, not the defaults. A sphere is the same set of
        // points whichever way its frame points, so writing the frame out
        // wrongly costs nothing until the p-curves arrive -- and then the
        // face's boundary is somewhere the surface no longer is.
        const Vec3d x = (sphere->Point(0.0, 0.0) - sphere->Center()).Normalized();
        const int placement = PlacementId(sphere->Center(), sphere->Axis(), x);
        id = Emit("SPHERICAL_SURFACE", "''," + Reference(placement) + "," + Real(sphere->Radius()));
    } else if (const auto *cone = dynamic_cast<const ConeSurface *>(&surface)) {
        // STEP places a cone by the circle of a given radius; this
        // kernel places it by its apex, so a radius has to be chosen and
        // the placement moved to match. One unit along the axis is as
        // good as any and keeps the numbers readable.
        const double half_angle = cone->HalfAngle();
        const double radius = std::tan(half_angle);
        const Vec3d origin = cone->Apex() + cone->Axis().Normalized();
        const Vec3d x = (cone->Point(0.0, 1.0) - origin).Normalized();
        const int placement = PlacementId(origin, cone->Axis(), x);
        id = Emit("CONICAL_SURFACE",
                  "''," + Reference(placement) + "," + Real(radius) + "," + Real(half_angle));
    } else if (const auto *torus = dynamic_cast<const TorusSurface *>(&surface)) {
        const Vec3d x = (torus->Point(0.0, 0.0) - torus->Center()).Normalized();
        const int placement = PlacementId(torus->Center(), torus->Axis(), x);
        id = Emit("TOROIDAL_SURFACE", "''," + Reference(placement) + "," + Real(torus->MajorRadius()) +
                                          "," + Real(torus->MinorRadius()));
    } else {
        int degree_u = 3;
        int degree_v = 3;
        std::vector<double> knots_u;
        std::vector<double> knots_v;
        std::vector<Vec4d> control;
        int count_u = 0;
        int count_v = 0;
        if (!surface.ToNurbs(&degree_u, &degree_v, &knots_u, &knots_v, &control, &count_u, &count_v) ||
            control.empty()) {
            error_ = "a surface could not be written: it is not one of the analytic families and "
                     "cannot be turned into a B-spline";
            return 0;
        }
        bool rational = false;
        std::string grid = "(";
        for (int i = 0; i < count_u; ++i) {
            if (i != 0) grid += ",";
            grid += "(";
            for (int j = 0; j < count_v; ++j) {
                if (j != 0) grid += ",";
                const Vec4d &c = control[static_cast<std::size_t>(i * count_v + j)];
                if (std::fabs(c.w - 1.0) > 1e-12) rational = true;
                const double w = c.w != 0.0 ? c.w : 1.0;
                grid += Reference(PointId(Vec3d{c.x / w, c.y / w, c.z / w}));
            }
            grid += ")";
        }
        grid += ")";
        auto knot_text = [](const std::vector<double> &knots, std::string *counts, std::string *values) {
            std::vector<double> distinct;
            std::vector<int> multiplicity;
            for (double k : knots) {
                if (!distinct.empty() && std::fabs(k - distinct.back()) < 1e-12) {
                    ++multiplicity.back();
                } else {
                    distinct.push_back(k);
                    multiplicity.push_back(1);
                }
            }
            *counts = "(";
            *values = "(";
            for (std::size_t i = 0; i < distinct.size(); ++i) {
                if (i != 0) {
                    *counts += ",";
                    *values += ",";
                }
                *counts += std::to_string(multiplicity[i]);
                *values += Real(distinct[i]);
            }
            *counts += ")";
            *values += ")";
        };
        std::string counts_u;
        std::string values_u;
        std::string counts_v;
        std::string values_v;
        knot_text(knots_u, &counts_u, &values_u);
        knot_text(knots_v, &counts_v, &values_v);
        if (!rational) {
            id = Emit("B_SPLINE_SURFACE_WITH_KNOTS",
                      "''," + std::to_string(degree_u) + "," + std::to_string(degree_v) + "," + grid +
                          ",.UNSPECIFIED.,.F.,.F.,.F.," + counts_u + "," + counts_v + "," + values_u +
                          "," + values_v + ",.UNSPECIFIED.");
        } else {
            std::string weights = "(";
            for (int i = 0; i < count_u; ++i) {
                if (i != 0) weights += ",";
                weights += "(";
                for (int j = 0; j < count_v; ++j) {
                    if (j != 0) weights += ",";
                    weights += Real(control[static_cast<std::size_t>(i * count_v + j)].w);
                }
                weights += ")";
            }
            weights += ")";
            id = EmitRaw(std::string("(BOUNDED_SURFACE()B_SPLINE_SURFACE(") +
                     std::to_string(degree_u) + "," + std::to_string(degree_v) + "," + grid +
                     ",.UNSPECIFIED.,.F.,.F.,.F.)B_SPLINE_SURFACE_WITH_KNOTS(" + counts_u + "," +
                     counts_v + "," + values_u + "," + values_v +
                              ",.UNSPECIFIED.)GEOMETRIC_REPRESENTATION_ITEM()"
                              "RATIONAL_B_SPLINE_SURFACE(" + weights +
                              ")REPRESENTATION_ITEM('')SURFACE())");
        }
    }
    surfaces_[index] = id;
    return id;
}

int Writer::VertexId(EntityId vertex) {
    const auto found = vertices_.find(vertex);
    if (found != vertices_.end()) return found->second;
    const int point = PointId(model_.GetVertex(vertex)->point);
    const int id = Emit("VERTEX_POINT", "''," + Reference(point));
    vertices_[vertex] = id;
    return id;
}

int Writer::EdgeId(EntityId edge) {
    const auto found = edges_.find(edge);
    if (found != edges_.end()) return found->second;
    const Edge *e = model_.GetEdge(edge);
    const Curve3 *curve = model_.CurveAt(e->curve);
    const int geometry = CurveId(e->curve, *curve);
    if (geometry == 0) return 0;
    // An edge that runs against its curve's own direction is written with
    // its vertices the way the *curve* goes and same_sense false, which
    // is what the schema means by that flag.
    const bool forward = e->t_end >= e->t_start;
    const int a = VertexId(forward ? e->start_vertex : e->end_vertex);
    const int b = VertexId(forward ? e->end_vertex : e->start_vertex);
    const int id = Emit("EDGE_CURVE", "''," + Reference(a) + "," + Reference(b) + "," +
                                          Reference(geometry) + "," + (forward ? ".T." : ".F."));
    edges_[edge] = id;
    return id;
}

int Writer::LoopId(EntityId loop_id) {
    const Loop *loop = model_.GetLoop(loop_id);
    std::vector<int> uses;
    for (EntityId coedge_id : loop->coedges) {
        const CoEdge *coedge = model_.GetCoEdge(coedge_id);
        const Edge *edge = model_.GetEdge(coedge->edge);
        const int written = EdgeId(coedge->edge);
        if (written == 0) return 0;
        // The coedge's direction is against the *edge*; the file's
        // EDGE_CURVE may itself be reversed. Both are folded in here so
        // that a reader multiplying them gets back what was meant.
        const bool edge_forward = edge->t_end >= edge->t_start;
        const bool forward = (coedge->orientation == Orientation::Forward) == edge_forward;
        uses.push_back(Emit("ORIENTED_EDGE", "'',*,*," + Reference(written) + "," +
                                                 (forward ? ".T." : ".F.")));
    }
    return Emit("EDGE_LOOP", "''," + List(uses));
}

int Writer::FaceId(EntityId face_id) {
    const Face *face = model_.GetFace(face_id);
    const int surface = SurfaceId(face->surface, *model_.SurfaceAt(face->surface));
    if (surface == 0) return 0;
    std::vector<int> bounds;
    for (EntityId loop_id : face->loops) {
        const Loop *loop = model_.GetLoop(loop_id);
        const int written = LoopId(loop_id);
        if (written == 0) return 0;
        bounds.push_back(Emit(loop->is_outer ? "FACE_OUTER_BOUND" : "FACE_BOUND",
                              "''," + Reference(written) + ",.T."));
    }
    return Emit("ADVANCED_FACE", "''," + List(bounds) + "," + Reference(surface) + "," +
                                     (face->orientation == Orientation::Forward ? ".T." : ".F."));
}

int Writer::ShellId(EntityId shell_id) {
    const Shell *shell = model_.GetShell(shell_id);
    std::vector<int> faces;
    for (EntityId face : shell->faces) {
        const int written = FaceId(face);
        if (written == 0) return 0;
        faces.push_back(written);
    }
    return Emit("CLOSED_SHELL", "''," + List(faces));
}

bool Writer::Run(const std::vector<EntityId> &bodies, std::string *out, std::string *error) {
    error->clear();
    if (bodies.empty()) {
        *error = "there are no bodies to write";
        return false;
    }
    // The units and the tolerance, which every AP214 file carries and
    // most readers insist on.
    // The units are complex instances -- several simple records making
    // one entity -- so they are written whole rather than through Emit,
    // which exists to write the simple form and would have to be bent out
    // of shape to produce this.
    const int length = EmitRaw("(LENGTH_UNIT()NAMED_UNIT(*)SI_UNIT(.MILLI.,.METRE.))");
    const int angle = EmitRaw("(NAMED_UNIT(*)PLANE_ANGLE_UNIT()SI_UNIT($,.RADIAN.))");
    const int solid_angle = EmitRaw("(NAMED_UNIT(*)SI_UNIT($,.STERADIAN.)SOLID_ANGLE_UNIT())");
    const int tolerance_value = Emit("UNCERTAINTY_MEASURE_WITH_UNIT",
                                     "LENGTH_MEASURE(1.E-7)," + Reference(length) +
                                         ",'distance_accuracy_value','confusion accuracy'");
    const int context = EmitRaw(
        "(GEOMETRIC_REPRESENTATION_CONTEXT(3)GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT((" +
        Reference(tolerance_value) + "))GLOBAL_UNIT_ASSIGNED_CONTEXT((" + Reference(length) + "," +
        Reference(angle) + "," + Reference(solid_angle) + "))REPRESENTATION_CONTEXT('',''))");

    std::vector<int> items;
    for (EntityId body_id : bodies) {
        const Body *body = model_.GetBody(body_id);
        if (body == nullptr || body->shells.empty()) {
            *error = "a body to be written has no shells";
            return false;
        }
        const int outer = ShellId(body->shells.front());
        if (outer == 0) {
            *error = error_.empty() ? "a shell could not be written" : error_;
            return false;
        }
        std::vector<int> voids;
        for (std::size_t i = 1; i < body->shells.size(); ++i) {
            const int inner = ShellId(body->shells[i]);
            if (inner == 0) {
                *error = error_.empty() ? "an inner shell could not be written" : error_;
                return false;
            }
            voids.push_back(Emit("ORIENTED_CLOSED_SHELL", "'',*," + Reference(inner) + ",.F."));
        }
        const std::string name = body->name.empty() ? "body" : body->name;
        if (voids.empty()) {
            items.push_back(Emit("MANIFOLD_SOLID_BREP", Quote(name) + "," + Reference(outer)));
        } else {
            items.push_back(
                Emit("BREP_WITH_VOIDS", Quote(name) + "," + Reference(outer) + "," + List(voids)));
        }
    }
    const int shape =
        Emit("ADVANCED_BREP_SHAPE_REPRESENTATION", "''," + List(items) + "," + Reference(context));

    // --- The product structure ---------------------------------------------
    //
    // WITHOUT THIS THE FILE IS INVISIBLE, AND IT LOOKS PERFECTLY FINE
    // WHILE BEING SO. A shape representation on its own is valid Part 21
    // and this library's own reader is happy with it -- it is only when
    // another system opens the file that the omission shows, and then it
    // does not show as an error either. A receiver transfers *roots*, and
    // it finds them by following SHAPE_DEFINITION_REPRESENTATION back to a
    // PRODUCT_DEFINITION; with no product there are no roots, so the file
    // reads cleanly and yields nothing. OpenCASCADE 7.8, through Gmsh,
    // reported "Done reading" and then meshed zero elements, which is
    // exactly the shape of the bug: silence rather than a complaint.
    //
    // So every export carries the minimum AP214 product structure. It is
    // eight entities and it is not optional.
    const int application =
        Emit("APPLICATION_CONTEXT", Quote("automotive design"));
    Emit("APPLICATION_PROTOCOL_DEFINITION",
         "'international standard','automotive_design',2000," + Reference(application));
    const int product_context = Emit(
        "PRODUCT_CONTEXT", "''," + Reference(application) + ",'mechanical'");
    const std::string product_name = options_.product.empty() ? "mep" : options_.product;
    const int product = Emit("PRODUCT", Quote(product_name) + "," + Quote(product_name) + ",''," +
                                            List({product_context}));
    Emit("PRODUCT_RELATED_PRODUCT_CATEGORY", "'part',$," + List({product}));
    const int formation =
        Emit("PRODUCT_DEFINITION_FORMATION", "'',''," + Reference(product));
    const int definition_context = Emit(
        "PRODUCT_DEFINITION_CONTEXT", "'part definition'," + Reference(application) + ",'design'");
    const int definition = Emit("PRODUCT_DEFINITION", "'design',''," + Reference(formation) + "," +
                                                          Reference(definition_context));
    const int definition_shape =
        Emit("PRODUCT_DEFINITION_SHAPE", "'',''," + Reference(definition));
    Emit("SHAPE_DEFINITION_REPRESENTATION", Reference(definition_shape) + "," + Reference(shape));

    *out = "ISO-10303-21;\nHEADER;\n";
    *out += "FILE_DESCRIPTION((" + Quote(options_.description) + "),'2;1');\n";
    *out += "FILE_NAME('','',(" + Quote(options_.author) + "),(" + Quote(options_.organisation) +
            "),'mep','mep','');\n";
    *out += "FILE_SCHEMA((" + Quote(options_.schema) + "));\nENDSEC;\nDATA;\n";
    *out += body_;
    *out += "ENDSEC;\nEND-ISO-10303-21;\n";
    return true;
}

}  // namespace

bool WriteStepShapes(const Model &model, const std::vector<EntityId> &bodies, std::string *out,
                     std::string *error, const StepWriteOptions &options) {
    Writer writer(model, options);
    return writer.Run(bodies, out, error);
}

}  // namespace cad
