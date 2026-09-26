#include "cad_doc.h"

#include "json.h"

#include <cmath>
#include <map>
#include <string>

namespace cad {
namespace {

// --- Small conversions --------------------------------------------------
//
// Written out rather than templated. There are six of them, they are read
// far more often than they are changed, and a reader that says which
// field was missing is worth more here than one that is clever.

Json ToJson(const Vec2d &v) {
    Json out = Json::Array();
    out.push_back(Json(v.x));
    out.push_back(Json(v.y));
    return out;
}
Json ToJson(const Vec3d &v) {
    Json out = Json::Array();
    out.push_back(Json(v.x));
    out.push_back(Json(v.y));
    out.push_back(Json(v.z));
    return out;
}
Json ToJson(const std::vector<double> &values) {
    Json out = Json::Array();
    for (double value : values) out.push_back(Json(value));
    return out;
}
Json ToJson(const std::vector<int> &values) {
    Json out = Json::Array();
    for (int value : values) out.push_back(Json(value));
    return out;
}
Json ToJson(const std::vector<bool> &values) {
    Json out = Json::Array();
    for (bool value : values) out.push_back(Json(value));
    return out;
}

bool FromJson(const Json &j, Vec2d *out, std::string *error) {
    if (j.size() != 2) {
        *error = "expected two numbers for a 2D point";
        return false;
    }
    out->x = j.items()[0].as_double();
    out->y = j.items()[1].as_double();
    return true;
}
bool FromJson(const Json &j, Vec3d *out, std::string *error) {
    if (j.size() != 3) {
        *error = "expected three numbers for a 3D point";
        return false;
    }
    out->x = j.items()[0].as_double();
    out->y = j.items()[1].as_double();
    out->z = j.items()[2].as_double();
    return true;
}
std::vector<double> DoublesFrom(const Json &j) {
    std::vector<double> out;
    for (const Json &item : j.items()) out.push_back(item.as_double());
    return out;
}
std::vector<int> IntsFrom(const Json &j) {
    std::vector<int> out;
    for (const Json &item : j.items()) out.push_back(item.as_int());
    return out;
}
std::vector<bool> BoolsFrom(const Json &j) {
    std::vector<bool> out;
    for (const Json &item : j.items()) out.push_back(item.as_bool());
    return out;
}

// --- Enumerations -------------------------------------------------------
//
// Written as names, not numbers. A number would make the file smaller and
// would silently change meaning the day a kind is inserted in the middle
// of an enum -- which is the sort of change nobody thinks of as a format
// change until a year-old file opens as the wrong shape.

const char *SurfaceKindName(SurfaceKind kind) {
    switch (kind) {
        case SurfaceKind::Plane: return "plane";
        case SurfaceKind::Cylinder: return "cylinder";
        case SurfaceKind::Cone: return "cone";
        case SurfaceKind::Sphere: return "sphere";
        case SurfaceKind::Torus: return "torus";
        case SurfaceKind::Nurbs: return "nurbs";
        case SurfaceKind::Extrusion: return "extrusion";
        case SurfaceKind::Revolution: return "revolution";
        case SurfaceKind::Ruled: return "ruled";
        case SurfaceKind::Offset: return "offset";
    }
    return "plane";
}
bool SurfaceKindFromName(const std::string &name, SurfaceKind *out) {
    static const std::pair<const char *, SurfaceKind> kAll[] = {
        {"plane", SurfaceKind::Plane},           {"cylinder", SurfaceKind::Cylinder},
        {"cone", SurfaceKind::Cone},             {"sphere", SurfaceKind::Sphere},
        {"torus", SurfaceKind::Torus},           {"nurbs", SurfaceKind::Nurbs},
        {"extrusion", SurfaceKind::Extrusion},   {"revolution", SurfaceKind::Revolution},
        {"ruled", SurfaceKind::Ruled},           {"offset", SurfaceKind::Offset}};
    for (const auto &entry : kAll) {
        if (name == entry.first) {
            *out = entry.second;
            return true;
        }
    }
    return false;
}

const char *CurveKindName(CurveKind kind) {
    switch (kind) {
        case CurveKind::Line: return "line";
        case CurveKind::Circle: return "circle";
        case CurveKind::Ellipse: return "ellipse";
        case CurveKind::Nurbs: return "nurbs";
    }
    return "line";
}
bool CurveKindFromName(const std::string &name, CurveKind *out) {
    if (name == "line") { *out = CurveKind::Line; return true; }
    if (name == "circle") { *out = CurveKind::Circle; return true; }
    if (name == "ellipse") { *out = CurveKind::Ellipse; return true; }
    if (name == "nurbs") { *out = CurveKind::Nurbs; return true; }
    return false;
}

bool SketchEntityKindFromName(const std::string &name, SketchEntityKind *out) {
    static const std::pair<const char *, SketchEntityKind> kAll[] = {
        {"point", SketchEntityKind::Point},     {"line", SketchEntityKind::Line},
        {"arc", SketchEntityKind::Arc},         {"circle", SketchEntityKind::Circle},
        {"ellipse", SketchEntityKind::Ellipse}, {"spline", SketchEntityKind::Spline}};
    for (const auto &entry : kAll) {
        if (name == entry.first) {
            *out = entry.second;
            return true;
        }
    }
    return false;
}

const char *FeatureKindName(FeatureKind kind) {
    switch (kind) {
        case FeatureKind::Sketch: return "sketch";
        case FeatureKind::Extrude: return "extrude";
        case FeatureKind::Revolve: return "revolve";
        case FeatureKind::Loft: return "loft";
        case FeatureKind::Sweep: return "sweep";
    }
    return "extrude";
}
bool FeatureKindFromName(const std::string &name, FeatureKind *out) {
    static const std::pair<const char *, FeatureKind> kAll[] = {
        {"sketch", FeatureKind::Sketch}, {"extrude", FeatureKind::Extrude},
        {"revolve", FeatureKind::Revolve}, {"loft", FeatureKind::Loft}, {"sweep", FeatureKind::Sweep}};
    for (const auto &entry : kAll) {
        if (name == entry.first) {
            *out = entry.second;
            return true;
        }
    }
    return false;
}

const char *CombineName(FeatureCombine combine) {
    switch (combine) {
        case FeatureCombine::NewBody: return "new";
        case FeatureCombine::Add: return "add";
        case FeatureCombine::Cut: return "cut";
        case FeatureCombine::Intersect: return "intersect";
    }
    return "new";
}
bool CombineFromName(const std::string &name, FeatureCombine *out) {
    if (name == "new") { *out = FeatureCombine::NewBody; return true; }
    if (name == "add") { *out = FeatureCombine::Add; return true; }
    if (name == "cut") { *out = FeatureCombine::Cut; return true; }
    if (name == "intersect") { *out = FeatureCombine::Intersect; return true; }
    return false;
}

const char *ExtrudeEndName(ExtrudeEnd end) {
    switch (end) {
        case ExtrudeEnd::Blind: return "blind";
        case ExtrudeEnd::Symmetric: return "symmetric";
        case ExtrudeEnd::ThroughAll: return "through";
    }
    return "blind";
}
bool ExtrudeEndFromName(const std::string &name, ExtrudeEnd *out) {
    if (name == "blind") { *out = ExtrudeEnd::Blind; return true; }
    if (name == "symmetric") { *out = ExtrudeEnd::Symmetric; return true; }
    if (name == "through") { *out = ExtrudeEnd::ThroughAll; return true; }
    return false;
}

// --- Persistent names ----------------------------------------------------
//
// Part B.4's EntityName, which is what makes a reference survive a
// rebuild. Storing it is the difference between a document that reopens
// and one that reopens with its references broken.

Json ToJson(const EntityName &name) {
    Json out = Json::Object();
    out["feature"] = Json(name.generating_feature);
    out["role"] = Json(name.role);
    out["is_face"] = Json(name.is_face);
    out["surface"] = Json(std::string(SurfaceKindName(name.surface_kind)));
    out["curve"] = Json(std::string(CurveKindName(name.curve_kind)));
    out["point"] = ToJson(name.sample_point);
    out["direction"] = ToJson(name.sample_direction);
    out["neighbours"] = ToJson(name.neighbour_features);
    out["loops"] = Json(name.loop_count);
    out["edges"] = Json(name.edge_count);
    return out;
}

bool FromJson(const Json &j, EntityName *out, std::string *error) {
    out->generating_feature = j.get("feature").as_int(-1);
    out->role = j.get("role").as_string("");
    out->is_face = j.get("is_face").as_bool(true);
    if (!SurfaceKindFromName(j.get("surface").as_string("plane"), &out->surface_kind)) {
        *error = "unknown surface kind '" + j.get("surface").as_string("") + "' in a stored name";
        return false;
    }
    if (!CurveKindFromName(j.get("curve").as_string("line"), &out->curve_kind)) {
        *error = "unknown curve kind '" + j.get("curve").as_string("") + "' in a stored name";
        return false;
    }
    if (!FromJson(j.get("point"), &out->sample_point, error)) return false;
    if (!FromJson(j.get("direction"), &out->sample_direction, error)) return false;
    out->neighbour_features = IntsFrom(j.get("neighbours"));
    out->loop_count = j.get("loops").as_int(0);
    out->edge_count = j.get("edges").as_int(0);
    return true;
}

// --- Sketches -------------------------------------------------------------

Json ToJson(const SketchPlane &plane) {
    Json out = Json::Object();
    out["origin"] = ToJson(plane.origin);
    out["x"] = ToJson(plane.x_axis);
    out["y"] = ToJson(plane.y_axis);
    out["offset"] = Json(plane.offset);
    out["attached"] = Json(plane.attached);
    if (plane.attached) out["face"] = ToJson(plane.face);
    return out;
}

bool FromJson(const Json &j, SketchPlane *out, std::string *error) {
    if (!FromJson(j.get("origin"), &out->origin, error)) return false;
    if (!FromJson(j.get("x"), &out->x_axis, error)) return false;
    if (!FromJson(j.get("y"), &out->y_axis, error)) return false;
    out->offset = j.get("offset").as_double(0.0);
    out->attached = j.get("attached").as_bool(false);
    if (out->attached && !FromJson(j.get("face"), &out->face, error)) return false;
    return true;
}

Json ToJson(const Sketch &sketch) {
    Json out = Json::Object();
    out["plane"] = ToJson(sketch.Plane());
    out["origin_point"] = Json(sketch.Origin());
    out["next_id"] = Json(sketch.PeekNextId());

    Json points = Json::Array();
    for (const SketchPoint &point : sketch.Points()) {
        Json p = Json::Object();
        p["id"] = Json(point.id);
        p["at"] = ToJson(point.position);
        p["fixed"] = Json(point.fixed);
        points.push_back(std::move(p));
    }
    out["points"] = std::move(points);

    Json entities = Json::Array();
    for (const SketchEntity &entity : sketch.Entities()) {
        Json e = Json::Object();
        e["id"] = Json(entity.id);
        e["kind"] = Json(std::string(SketchEntityKindName(entity.kind)));
        e["construction"] = Json(entity.construction);
        e["points"] = ToJson(entity.points);
        e["scalars"] = ToJson(entity.scalars);
        e["scalar_fixed"] = ToJson(entity.scalar_fixed);
        e["arc_ccw"] = Json(entity.arc_ccw);
        e["degree"] = Json(entity.degree);
        e["knots"] = ToJson(entity.knots);
        entities.push_back(std::move(e));
    }
    out["entities"] = std::move(entities);

    Json constraints = Json::Array();
    for (const SketchConstraint &constraint : sketch.Constraints()) {
        Json c = Json::Object();
        c["id"] = Json(constraint.id);
        c["kind"] = Json(std::string(ConstraintKindName(constraint.kind)));
        c["points"] = ToJson(constraint.points);
        c["entities"] = ToJson(constraint.entities);
        c["value"] = Json(constraint.value);
        c["driving"] = Json(constraint.driving);
        constraints.push_back(std::move(c));
    }
    out["constraints"] = std::move(constraints);
    return out;
}

bool FromJson(const Json &j, Sketch *out, std::string *error) {
    SketchPlane plane;
    if (!FromJson(j.get("plane"), &plane, error)) return false;

    std::vector<SketchPoint> points;
    for (const Json &p : j.get("points").items()) {
        SketchPoint point;
        point.id = p.get("id").as_int(kNoSketchId);
        if (!FromJson(p.get("at"), &point.position, error)) return false;
        point.fixed = p.get("fixed").as_bool(false);
        points.push_back(point);
    }

    std::vector<SketchEntity> entities;
    for (const Json &e : j.get("entities").items()) {
        SketchEntity entity;
        entity.id = e.get("id").as_int(kNoSketchId);
        if (!SketchEntityKindFromName(e.get("kind").as_string(""), &entity.kind)) {
            *error = "unknown sketch entity kind '" + e.get("kind").as_string("") + "'";
            return false;
        }
        entity.construction = e.get("construction").as_bool(false);
        entity.points = IntsFrom(e.get("points"));
        entity.scalars = DoublesFrom(e.get("scalars"));
        entity.scalar_fixed = BoolsFrom(e.get("scalar_fixed"));
        entity.arc_ccw = e.get("arc_ccw").as_bool(true);
        entity.degree = e.get("degree").as_int(3);
        entity.knots = DoublesFrom(e.get("knots"));
        entities.push_back(std::move(entity));
    }

    std::vector<SketchConstraint> constraints;
    for (const Json &c : j.get("constraints").items()) {
        SketchConstraint constraint;
        constraint.id = c.get("id").as_int(kNoSketchId);
        if (!ConstraintKindFromName(c.get("kind").as_string(""), &constraint.kind)) {
            *error = "unknown constraint kind '" + c.get("kind").as_string("") + "'";
            return false;
        }
        constraint.points = IntsFrom(c.get("points"));
        constraint.entities = IntsFrom(c.get("entities"));
        constraint.value = c.get("value").as_double(0.0);
        constraint.driving = c.get("driving").as_bool(true);
        constraints.push_back(std::move(constraint));
    }

    out->Restore(plane, std::move(points), std::move(entities), std::move(constraints),
                 j.get("origin_point").as_int(kNoSketchId), j.get("next_id").as_int(0));
    return true;
}

// --- Features --------------------------------------------------------------

Json ToJson(const Feature &feature) {
    Json out = Json::Object();
    out["id"] = Json(feature.id);
    out["kind"] = Json(std::string(FeatureKindName(feature.kind)));
    out["name"] = Json(feature.name);
    out["suppressed"] = Json(feature.suppressed);
    out["sketch"] = Json(feature.sketch);
    out["profile"] = Json(feature.profile_index);
    out["combine"] = Json(std::string(CombineName(feature.combine)));
    out["end"] = Json(std::string(ExtrudeEndName(feature.end)));
    out["distance"] = Json(feature.distance);
    out["reverse"] = Json(feature.reverse);
    out["axis_entity"] = Json(feature.axis_entity);
    out["angle"] = Json(feature.angle);
    out["sections"] = ToJson(feature.sections);
    out["spine"] = Json(feature.spine);
    out["spine_entity"] = Json(feature.spine_entity);
    out["spine_sections"] = Json(feature.spine_sections);
    out["twist"] = Json(feature.twist);
    out["end_scale"] = Json(feature.end_scale);
    // Only a Sketch feature carries one, and writing an empty sketch for
    // every extrude would treble the size of a typical file for nothing.
    if (feature.kind == FeatureKind::Sketch) out["geometry"] = ToJson(feature.sketch_geometry);
    return out;
}

bool FromJson(const Json &j, Feature *out, std::string *error) {
    out->id = j.get("id").as_int(-1);
    if (!FeatureKindFromName(j.get("kind").as_string(""), &out->kind)) {
        *error = "unknown feature kind '" + j.get("kind").as_string("") + "'";
        return false;
    }
    out->name = j.get("name").as_string("");
    out->suppressed = j.get("suppressed").as_bool(false);
    out->sketch = j.get("sketch").as_int(-1);
    out->profile_index = j.get("profile").as_int(-1);
    if (!CombineFromName(j.get("combine").as_string("new"), &out->combine)) {
        *error = "unknown combine '" + j.get("combine").as_string("") + "'";
        return false;
    }
    if (!ExtrudeEndFromName(j.get("end").as_string("blind"), &out->end)) {
        *error = "unknown extrude end '" + j.get("end").as_string("") + "'";
        return false;
    }
    out->distance = j.get("distance").as_double(1.0);
    out->reverse = j.get("reverse").as_bool(false);
    out->axis_entity = j.get("axis_entity").as_int(kNoSketchId);
    out->angle = j.get("angle").as_double(kTwoPi);
    out->sections = IntsFrom(j.get("sections"));
    out->spine = j.get("spine").as_int(-1);
    out->spine_entity = j.get("spine_entity").as_int(kNoSketchId);
    out->spine_sections = j.get("spine_sections").as_int(12);
    out->twist = j.get("twist").as_double(0.0);
    out->end_scale = j.get("end_scale").as_double(1.0);
    if (out->kind == FeatureKind::Sketch && !FromJson(j.get("geometry"), &out->sketch_geometry, error)) {
        return false;
    }
    return true;
}

}  // namespace

// --- The document ------------------------------------------------------

std::string WriteCadDocument(const CadDocument &document) {
    Json out = Json::Object();
    out["format"] = Json(std::string("mepcad"));
    out["version"] = Json(kCadDocumentVersion);
    out["title"] = Json(document.title);
    out["notes"] = Json(document.notes);
    Json features = Json::Array();
    for (const Feature &feature : document.tree.Features()) features.push_back(ToJson(feature));
    out["features"] = std::move(features);
    return out.dump();
}

bool CadDocumentVersion(const std::string &text, int *out_version, std::string *error) {
    error->clear();
    Json root;
    if (!Json::Parse(text, &root)) {
        *error = "this is not a document at all: it did not parse";
        return false;
    }
    if (root.get("format").as_string("") != "mepcad") {
        *error = "this is not a .mepcad document";
        return false;
    }
    *out_version = root.get("version").as_int(-1);
    return true;
}

bool ReadCadDocument(const std::string &text, CadDocument *out, std::string *error) {
    error->clear();
    *out = CadDocument{};
    int version = -1;
    if (!CadDocumentVersion(text, &version, error)) return false;
    if (version < 1) {
        *error = "the document does not say which version it is";
        return false;
    }
    if (version > kCadDocumentVersion) {
        *error = "this document is version " + std::to_string(version) + " and this build reads up to " +
                 std::to_string(kCadDocumentVersion) + "; it was written by a newer mep";
        return false;
    }
    Json root;
    Json::Parse(text, &root);
    out->title = root.get("title").as_string("");
    out->notes = root.get("notes").as_string("");

    // Features go back in through AddFeature, which is what keeps the
    // tree's own bookkeeping -- the next id, and everything being marked
    // dirty -- consistent with what was read. A document is a list of
    // operations, and putting them back one at a time is how they were
    // put in.
    for (const Json &j : root.get("features").items()) {
        Feature feature;
        if (!FromJson(j, &feature, error)) return false;
        const int stored = feature.id;
        const int given = feature.kind == FeatureKind::Sketch
                              ? out->tree.AddSketch(feature.sketch_geometry, feature.name)
                              : out->tree.AddFeature(feature);
        if (given != stored) {
            *error = "feature " + std::to_string(stored) + " came back as " + std::to_string(given) +
                     "; the file's features are out of order, and every reference between them would "
                     "be wrong";
            return false;
        }
        // AddSketch does not carry the rest of a Sketch feature's fields,
        // and a Sketch feature has few, but suppression is one of them.
        if (feature.kind == FeatureKind::Sketch) {
            out->tree.GetMutable(given)->suppressed = feature.suppressed;
        }
    }
    return true;
}

std::string WriteSketch(const Sketch &sketch) {
    Json out = Json::Object();
    out["format"] = Json(std::string("mepsketch"));
    out["version"] = Json(kCadDocumentVersion);
    out["sketch"] = ToJson(sketch);
    return out.dump();
}

bool ReadSketch(const std::string &text, Sketch *out, std::string *error) {
    error->clear();
    Json root;
    if (!Json::Parse(text, &root)) {
        *error = "this is not a sketch at all: it did not parse";
        return false;
    }
    if (root.get("format").as_string("") != "mepsketch") {
        *error = "this is not a sketch document";
        return false;
    }
    const int version = root.get("version").as_int(-1);
    if (version > kCadDocumentVersion) {
        *error = "this sketch is version " + std::to_string(version) + " and this build reads up to " +
                 std::to_string(kCadDocumentVersion);
        return false;
    }
    return FromJson(root.get("sketch"), out, error);
}

}  // namespace cad
