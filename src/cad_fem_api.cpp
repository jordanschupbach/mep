#include "cad_fem_api.h"

#include "cad_boolean.h"
#include "cad_exchange.h"
#include "cad_mass.h"
#include "cad_modify.h"
#include "cad_pcurve.h"
#include "cad_step.h"
#include "cad_tessellate.h"
#include "cad_validate.h"
#include "fem_animate.h"
#include "fem_movie.h"
#include "fem_probe.h"
#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace cadfem {
namespace {

using cad::EntityId;
using cad::Vec3d;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

constexpr double kTwoPi = 6.283185307179586;
// Degrees on the surface, radians underneath. A camera bearing written
// into a script by hand is written in degrees, every time.
constexpr double kDegrees = 0.017453292519943295;

Vec3d ReadVec(const Json &value, const Vec3d &fallback) {
    if (!value.is_array() || value.size() < 3) return fallback;
    const std::vector<Json> &items = value.items();
    return Vec3d{items[0].as_double(fallback.x), items[1].as_double(fallback.y),
                 items[2].as_double(fallback.z)};
}

Json WriteVec(const Vec3d &v) {
    Json out = Json::Array();
    out.push_back(v.x);
    out.push_back(v.y);
    out.push_back(v.z);
    return out;
}

// A face's outward normal and a point on it, at the middle of its
// parameter domain. Used for reporting faces and for picking one by
// direction, which is how an agent that cannot click refers to a face.
bool FaceSample(const cad::Model &model, EntityId face, Vec3d *out_point, Vec3d *out_normal) {
    const cad::Face *f = model.GetFace(face);
    if (f == nullptr) return false;
    const cad::Surface *surface = model.SurfaceAt(f->surface);
    if (surface == nullptr) return false;
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    const double u = (u_lo + u_hi) * 0.5;
    const double v = (v_lo + v_hi) * 0.5;
    *out_point = surface->Point(u, v);
    *out_normal = model.FaceNormal(face, u, v);
    return true;
}

// A point on an edge, and the direction it runs in there. The midpoint
// of its own parameter range rather than of its endpoints, so a
// half-circle is sampled on the arc and not at the centre of the chord.
bool EdgeSample(const cad::Model &model, EntityId edge, Vec3d *out_point, Vec3d *out_direction,
                Vec3d *out_start, Vec3d *out_end, double *out_length) {
    const cad::Edge *e = model.GetEdge(edge);
    if (e == nullptr) return false;
    const cad::Curve3 *curve = model.CurveAt(e->curve);
    if (curve == nullptr) return false;
    const double middle = (e->t_start + e->t_end) * 0.5;
    *out_point = curve->Point(middle);
    *out_start = curve->Point(e->t_start);
    *out_end = curve->Point(e->t_end);
    const double step = std::max(1e-9, (e->t_end - e->t_start) * 1e-4);
    const Vec3d ahead = curve->Point(std::min(e->t_end, middle + step));
    const Vec3d behind = curve->Point(std::max(e->t_start, middle - step));
    Vec3d direction = ahead - behind;
    if (direction.LengthSquared() > 0.0) direction = direction.Normalized();
    *out_direction = direction;
    // Polyline length over a modest number of samples: enough to tell a
    // short edge from a long one, which is what picking one needs.
    double length = 0.0;
    Vec3d previous = *out_start;
    for (int i = 1; i <= 16; ++i) {
        const double t = e->t_start + (e->t_end - e->t_start) * i / 16.0;
        const Vec3d here = curve->Point(t);
        length += (here - previous).Length();
        previous = here;
    }
    *out_length = length;
    return true;
}

std::vector<EntityId> EdgesOfBody(const cad::Model &model, EntityId body) {
    std::vector<EntityId> out;
    const cad::Body *b = model.GetBody(body);
    if (b == nullptr) return out;
    for (const EntityId shell_id : b->shells) {
        for (const EntityId edge : model.EdgesOfShell(shell_id)) {
            if (std::find(out.begin(), out.end(), edge) == out.end()) out.push_back(edge);
        }
    }
    return out;
}

std::vector<EntityId> FacesOfBody(const cad::Model &model, EntityId body) {
    std::vector<EntityId> out;
    const cad::Body *b = model.GetBody(body);
    if (b == nullptr) return out;
    for (const EntityId shell_id : b->shells) {
        const cad::Shell *shell = model.GetShell(shell_id);
        if (shell == nullptr) continue;
        for (const EntityId face : shell->faces) out.push_back(face);
    }
    return out;
}

// Mass properties from the *tessellated* boundary, by the divergence
// theorem over its triangles.
//
// AND NOT FROM `cad::ComputeMassProperties`, WHICH IS THE OBVIOUS THING
// AND IS WRONG HERE. That function takes a list of bare `Surface`
// pointers and integrates each over its whole parameter domain -- it has
// no way to know which part of a surface a face actually uses, because
// nothing in its arguments says. For a body the kernel just built that
// happens to be right, since `MakeBox` hands each face a plane whose
// domain is the face. For the same box written to STEP and read back it
// is catastrophically wrong: a STEP plane is untrimmed, its domain comes
// back as -1e5 to 1e5, and a 24 m^3 block measured 1.2e11. The failure
// is silent, it depends on where the body came from rather than on what
// it is, and the analytic answer looks exactly as authoritative as the
// correct one.
//
// The tessellation honours trimming by construction, because trimming is
// what it is for. The cost is that a curved face is only as accurate as
// the chord tolerance, which is a real and stateable limitation where
// "wrong by ten orders of magnitude depending on provenance" is not.
struct BodyMass {
    double volume = 0.0;
    double area = 0.0;
    Vec3d centroid;
    double inertia[3][3] = {};  // about the centroid, per unit density
};

bool ComputeBodyMass(const cad::Model &model, EntityId body, double chord, BodyMass *out,
                     bool *out_closed, std::string *error) {
    cad::TessellationOptions options;
    options.chord_tolerance = chord;
    cad::TessellationMesh mesh;
    if (!cad::TessellateBody(model, body, options, &mesh, error)) return false;
    *out_closed = mesh.IsClosed();
    *out = BodyMass{};
    double covariance[3][3] = {};
    for (int t = 0; t + 2 < static_cast<int>(mesh.indices.size()); t += 3) {
        const Vec3d a = mesh.positions[Idx(mesh.indices[Idx(t)])];
        const Vec3d b = mesh.positions[Idx(mesh.indices[Idx(t + 1)])];
        const Vec3d c = mesh.positions[Idx(mesh.indices[Idx(t + 2)])];
        out->area += 0.5 * (b - a).Cross(c - a).Length();
        // The tetrahedron this triangle makes with the origin, signed.
        // Summed over a closed surface the outside cancels and what is
        // left is the solid, wherever the origin happens to be.
        const double v = a.Dot(b.Cross(c)) / 6.0;
        out->volume += v;
        out->centroid = out->centroid + (a + b + c) * (v * 0.25);
        const Vec3d corner[3] = {a, b, c};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                // The second moment of that tetrahedron about the origin.
                // The 2s on the diagonal terms and the 1s on the cross
                // terms are the integral of the barycentric products over
                // a simplex, and the whole coefficient is checked against
                // a block's closed-form inertia in the test rather than
                // trusted from memory -- it is exactly the sort of
                // constant that is wrong by a factor of two in half the
                // places it is written down.
                double sum = 0.0;
                for (int k = 0; k < 3; ++k) {
                    for (int l = 0; l < 3; ++l) {
                        sum += (k == l ? 2.0 : 1.0) * 0.5 *
                               (corner[k][Idx(i)] * corner[l][Idx(j)] +
                                corner[l][Idx(i)] * corner[k][Idx(j)]);
                    }
                }
                covariance[i][j] += v * sum / 20.0;
            }
        }
    }
    if (!(std::fabs(out->volume) > 0.0)) {
        *error = "the body tessellated to a surface enclosing no volume";
        return false;
    }
    out->centroid = out->centroid * (1.0 / out->volume);
    // Shift the second moments to the centroid, then turn them into an
    // inertia tensor: I = trace(C) * identity - C.
    const Vec3d g = out->centroid;
    const double gv[3] = {g.x, g.y, g.z};
    double centred[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            centred[i][j] = covariance[i][j] - out->volume * gv[i] * gv[j];
        }
    }
    const double trace = centred[0][0] + centred[1][1] + centred[2][2];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out->inertia[i][j] = (i == j ? trace : 0.0) - centred[i][j];
        }
    }
    return true;
}

bool WriteTextFile(const std::string &path, const std::string &text, std::string *error) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        *error = "cannot write " + path;
        return false;
    }
    file << text;
    if (!file) {
        *error = "failed while writing " + path;
        return false;
    }
    return true;
}

bool ReadTextFile(const std::string &path, std::string *out, std::string *error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        *error = "cannot read " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    *out = buffer.str();
    return true;
}

fem::ScalarField ResolveField(const std::string &name, bool *ok) {
    *ok = true;
    for (const fem::ScalarField field : fem::AllScalarFields()) {
        if (name == fem::ScalarFieldName(field)) return field;
    }
    // Also accept the compact spellings an agent is likelier to type than
    // the display names, which have spaces in them.
    struct Alias {
        const char *name;
        fem::ScalarField field;
    };
    static const Alias aliases[] = {
        {"displacement", fem::ScalarField::DisplacementMagnitude},
        {"von_mises", fem::ScalarField::VonMises},
        {"vonmises", fem::ScalarField::VonMises},
        {"tresca", fem::ScalarField::Tresca},
        {"max_principal", fem::ScalarField::MaxPrincipal},
        {"min_principal", fem::ScalarField::MinPrincipal},
        {"max_shear", fem::ScalarField::MaxShear},
        {"hydrostatic", fem::ScalarField::Hydrostatic},
        {"triaxiality", fem::ScalarField::Triaxiality},
        {"safety_factor", fem::ScalarField::SafetyFactor},
        {"discontinuity", fem::ScalarField::Discontinuity},
        // The components. Missing until an example asked for
        // `stress_yy` -- which is the spelling of the *target quantity*
        // of two of the NAFEMS benchmarks -- and got "no field called
        // 'stress_yy'" back. The display names have spaces in them and
        // nobody types a space into a field name.
        {"displacement_x", fem::ScalarField::DisplacementX},
        {"displacement_y", fem::ScalarField::DisplacementY},
        {"displacement_z", fem::ScalarField::DisplacementZ},
        {"stress_xx", fem::ScalarField::StressXX},
        {"stress_yy", fem::ScalarField::StressYY},
        {"stress_zz", fem::ScalarField::StressZZ},
        {"stress_xy", fem::ScalarField::StressXY},
        {"stress_yz", fem::ScalarField::StressYZ},
        {"stress_zx", fem::ScalarField::StressZX},
    };
    for (const Alias &alias : aliases) {
        if (name == alias.name) return alias.field;
    }
    *ok = false;
    return fem::ScalarField::VonMises;
}

}  // namespace

// The same resolver, on the outside. A caller that is not an adapter --
// the viewer, which takes a field name from a script -- has to accept
// exactly the spellings `fem.field` accepts, and a second table of
// aliases would drift from this one the first time either grew.
fem::ScalarField FieldByName(const std::string &name, bool *ok) {
    return ResolveField(name, ok);
}

Session &SharedSession() {
    static Session session;
    return session;
}

Session::Session() = default;
Session::~Session() = default;

bool Session::LookUpResult(int handle, ResultView *out) const {
    const auto found = results_.find(handle);
    if (found == results_.end() || out == nullptr) return false;
    out->model = &found->second.model;
    out->statics = &found->second.result;
    out->modes = &found->second.modes;
    out->has_modes = found->second.has_modes;
    return true;
}

const cad::Model *Session::LookUpDocument(int handle) const {
    const auto found = documents_.find(handle);
    return found == documents_.end() ? nullptr : &found->second.model;
}

int Session::DocumentCount() const { return static_cast<int>(documents_.size()); }
int Session::StudyCount() const { return static_cast<int>(studies_.size()); }
int Session::MeshCount() const { return static_cast<int>(meshes_.size()); }
int Session::ResultCount() const { return static_cast<int>(results_.size()); }

Session::Document *Session::Doc(const Json &params, std::string *error) {
    const int id = params.get("document").as_int(-1);
    const auto found = documents_.find(id);
    if (found == documents_.end()) {
        *error = "no document " + std::to_string(id) + " is open";
        return nullptr;
    }
    return &found->second;
}

Session::StudyRecord *Session::StudyOf(const Json &params, std::string *error) {
    const int id = params.get("study").as_int(-1);
    const auto found = studies_.find(id);
    if (found == studies_.end()) {
        *error = "no study " + std::to_string(id);
        return nullptr;
    }
    return &found->second;
}

Session::MeshRecord *Session::MeshOf(const Json &params, std::string *error) {
    const int id = params.get("mesh").as_int(-1);
    const auto found = meshes_.find(id);
    if (found == meshes_.end()) {
        *error = "no mesh " + std::to_string(id);
        return nullptr;
    }
    return &found->second;
}

Session::ResultRecord *Session::ResultOf(const Json &params, std::string *error) {
    const int id = params.get("result").as_int(-1);
    const auto found = results_.find(id);
    if (found == results_.end()) {
        *error = "no result " + std::to_string(id);
        return nullptr;
    }
    return &found->second;
}

namespace {

// A handle's worth of a document's geometry, rebuilt after any change to
// it. THE PCURVES MATTER: every downstream user -- meshing, persistent
// naming, tessellation -- asks a face for the curve of its boundary in
// parameter space, and a body built by a primitive or a boolean has none
// until they are computed. Forgetting this gives a body that looks
// correct in every count and cannot be meshed.
bool Finish(cad::Model *model, std::string *error) { return cad::BuildAllPCurves(model, {}, error); }

double ModelScale(const cad::Model &model, cad::EntityId body) {
    cad::Box3d box;
    for (const cad::Face &face : model.Faces()) {
        const cad::Surface *surface = model.SurfaceAt(face.surface);
        if (surface == nullptr) continue;
        double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
        surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
        for (int i = 0; i <= 2; ++i) {
            for (int j = 0; j <= 2; ++j) {
                box.Expand(surface->Point(u_lo + (u_hi - u_lo) * i * 0.5,
                                             v_lo + (v_hi - v_lo) * j * 0.5));
            }
        }
    }
    (void)body;
    const double diagonal = (box.Max() - box.Min()).Length();
    return diagonal > 0.0 ? diagonal : 1.0;
}

std::string LowerExtension(const std::string &path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string out = path.substr(dot + 1);
    for (char &ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

}  // namespace

bool Session::Call(const std::string &method, const Json &params, Json *out, std::string *error) {
    *out = Json::Object();
    error->clear();
    if (FindMethod(method) == nullptr) {
        *error = "no such method: " + method;
        return false;
    }

    // --- Documents ---------------------------------------------------------
    if (method == "part.new") {
        const int id = next_id_++;
        Document document;
        document.title = params.get("title").as_string("part");
        documents_[id] = std::move(document);
        (*out)["document"] = id;
        return true;
    }
    if (method == "part.close") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        const int id = params.get("document").as_int(-1);
        // EVERYTHING BUILT ON IT GOES TOO. A mesh of a body that no longer
        // exists is not a mesh, and a result naming a document that has
        // been closed is a handle that will fail confusingly later rather
        // than clearly now.
        for (auto it = results_.begin(); it != results_.end();) {
            it = it->second.document == id ? results_.erase(it) : std::next(it);
        }
        for (auto it = meshes_.begin(); it != meshes_.end();) {
            it = it->second.document == id ? meshes_.erase(it) : std::next(it);
        }
        for (auto it = studies_.begin(); it != studies_.end();) {
            it = it->second.document == id ? studies_.erase(it) : std::next(it);
        }
        documents_.erase(id);
        (*out)["closed"] = id;
        return true;
    }
    if (method == "part.list") {
        Json documents = Json::Array();
        for (const auto &entry : documents_) {
            Json one = Json::Object();
            one["document"] = entry.first;
            one["title"] = entry.second.title;
            one["bodies"] = static_cast<int>(entry.second.model.Bodies().size());
            documents.push_back(std::move(one));
        }
        Json studies = Json::Array();
        for (const auto &entry : studies_) {
            Json one = Json::Object();
            one["study"] = entry.first;
            one["document"] = entry.second.document;
            one["name"] = entry.second.study.name;
            one["supports"] = static_cast<int>(entry.second.study.supports.size());
            one["loads"] = static_cast<int>(entry.second.study.loads.size());
            studies.push_back(std::move(one));
        }
        Json meshes = Json::Array();
        for (const auto &entry : meshes_) {
            Json one = Json::Object();
            one["mesh"] = entry.first;
            one["document"] = entry.second.document;
            one["nodes"] = entry.second.mesh.NodeCount();
            one["elements"] = entry.second.mesh.TetCount();
            meshes.push_back(std::move(one));
        }
        Json results = Json::Array();
        for (const auto &entry : results_) {
            Json one = Json::Object();
            one["result"] = entry.first;
            one["study"] = entry.second.study;
            one["modal"] = entry.second.has_modes;
            results.push_back(std::move(one));
        }
        (*out)["documents"] = std::move(documents);
        (*out)["studies"] = std::move(studies);
        (*out)["meshes"] = std::move(meshes);
        (*out)["results"] = std::move(results);
        return true;
    }

    // --- Primitives --------------------------------------------------------
    if (method == "part.box" || method == "part.cylinder" || method == "part.sphere") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        cad::Model built;
        EntityId body = cad::kNoEntity;
        bool made = false;
        if (method == "part.box") {
            const Vec3d size = ReadVec(params.get("size"), Vec3d{});
            if (!(size.x > 0.0 && size.y > 0.0 && size.z > 0.0)) {
                *error = "a box needs a positive size in all three directions";
                return false;
            }
            made = cad::MakeBox(ReadVec(params.get("at"), Vec3d{}), size, &built, &body);
        } else if (method == "part.cylinder") {
            const double radius = params.get("radius").as_double(0.0);
            const double height = params.get("height").as_double(0.0);
            if (!(radius > 0.0) || !(height > 0.0)) {
                *error = "a cylinder needs a positive radius and height";
                return false;
            }
            made = cad::MakeCylinder(ReadVec(params.get("at"), Vec3d{}),
                                     ReadVec(params.get("axis"), Vec3d{0, 0, 1}), radius, height,
                                     &built, &body);
        } else {
            const double radius = params.get("radius").as_double(0.0);
            if (!(radius > 0.0)) {
                *error = "a sphere needs a positive radius";
                return false;
            }
            made = cad::MakeSphere(ReadVec(params.get("at"), Vec3d{}), radius, &built, &body);
        }
        if (!made) {
            *error = "the primitive could not be built";
            return false;
        }
        // A primitive arrives in a model of its own, because that is the
        // signature the kernel offers. Merging it in is a boolean union
        // with nothing, which the kernel also offers -- but only if there
        // is already something to union with.
        if (document->model.Bodies().empty()) {
            document->model = std::move(built);
        } else {
            cad::Model merged = document->model;
            EntityId copied = cad::kNoEntity;
            if (!cad::CopyBodyInto(built, body, &merged, &copied, false, false, "", error)) {
                return false;
            }
            document->model = std::move(merged);
            body = copied;
        }
        if (!Finish(&document->model, error)) return false;
        (*out)["body"] = static_cast<long long>(body);
        return true;
    }

    if (method == "part.boolean") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        const std::string op = params.get("op").as_string("");
        cad::BooleanOp kind = cad::BooleanOp::Union;
        if (op == "union") {
            kind = cad::BooleanOp::Union;
        } else if (op == "difference" || op == "subtract") {
            kind = cad::BooleanOp::Difference;
        } else if (op == "intersection" || op == "intersect") {
            kind = cad::BooleanOp::Intersection;
        } else {
            *error = "op must be union, difference or intersection, not '" + op + "'";
            return false;
        }
        const EntityId a = static_cast<EntityId>(params.get("a").as_int(0));
        const EntityId b = static_cast<EntityId>(params.get("b").as_int(0));
        if (document->model.GetBody(a) == nullptr || document->model.GetBody(b) == nullptr) {
            *error = "both a and b must be bodies of this document";
            return false;
        }
        cad::Model result;
        cad::BooleanReport report;
        if (!cad::BooleanOperation(document->model, a, document->model, b, kind, &result,
                                   &report)) {
            *error = report.error.empty() ? "the boolean failed" : report.error;
            return false;
        }
        if (report.body == cad::kNoEntity) {
            *error = "the boolean produced nothing at all, which usually means the two bodies"
                     " do not overlap the way the operation needs";
            return false;
        }
        document->model = std::move(result);
        if (!Finish(&document->model, error)) return false;
        (*out)["body"] = static_cast<long long>(report.body);
        (*out)["pieces_kept"] = report.pieces_kept;
        return true;
    }

    if (method == "part.shell") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        const EntityId body = static_cast<EntityId>(params.get("body").as_int(0));
        if (document->model.GetBody(body) == nullptr) {
            *error = "no such body";
            return false;
        }
        std::vector<EntityId> open;
        for (const Json &face : params.get("open_faces").items()) {
            open.push_back(static_cast<EntityId>(face.as_int(0)));
        }
        cad::Model result;
        EntityId made = cad::kNoEntity;
        if (!cad::ShellBody(document->model, body, open, params.get("thickness").as_double(0.0),
                            {}, &result, &made, error)) {
            return false;
        }
        document->model = std::move(result);
        if (!Finish(&document->model, error)) return false;
        (*out)["body"] = static_cast<long long>(made);
        return true;
    }

    // --- Interrogation ------------------------------------------------------
    if (method == "part.info") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        (*out)["title"] = document->title;
        (*out)["bodies"] = static_cast<int>(document->model.Bodies().size());
        (*out)["faces"] = static_cast<int>(document->model.Faces().size());
        (*out)["edges"] = static_cast<int>(document->model.Edges().size());
        (*out)["vertices"] = static_cast<int>(document->model.Vertices().size());
        Json ids = Json::Array();
        for (const cad::Body &body : document->model.Bodies()) {
            ids.push_back(static_cast<long long>(body.id));
        }
        (*out)["body_ids"] = std::move(ids);
        return true;
    }
    if (method == "part.edges") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        const EntityId body = static_cast<EntityId>(params.get("body").as_int(0));
        if (document->model.GetBody(body) == nullptr) {
            *error = "no such body";
            return false;
        }
        // WHY AN EDGE IS ON THIS SURFACE AT ALL, given that a face was
        // thought to be enough. A simple support is a *line*: a beam on
        // two knife edges, a plate resting on its four sides. Expressed
        // as a face it becomes a built-in end, which is a stiffer
        // structure and a different benchmark -- NAFEMS FV5 and FV52 are
        // both simply supported and neither can be stated without this.
        const Vec3d along = ReadVec(params.get("along"), Vec3d{0, 0, 0});
        const bool filtering = along.LengthSquared() > 0.0;
        const Vec3d unit = filtering ? along.Normalized() : Vec3d{0, 0, 1};
        Json list = Json::Array();
        for (const EntityId edge : EdgesOfBody(document->model, body)) {
            Vec3d point, direction, start, end;
            double length = 0.0;
            if (!EdgeSample(document->model, edge, &point, &direction, &start, &end, &length)) {
                continue;
            }
            // An edge has no preferred sense, so "runs along this
            // direction" has to ignore the sign.
            if (filtering && std::fabs(direction.Dot(unit)) < 0.999) continue;
            Json one = Json::Object();
            one["id"] = static_cast<long long>(edge);
            one["point"] = WriteVec(point);
            one["direction"] = WriteVec(direction);
            one["from"] = WriteVec(start);
            one["to"] = WriteVec(end);
            one["length"] = length;
            list.push_back(std::move(one));
        }
        (*out)["edges"] = std::move(list);
        return true;
    }
    if (method == "part.edge_at") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        const EntityId body = static_cast<EntityId>(params.get("body").as_int(0));
        if (document->model.GetBody(body) == nullptr) {
            *error = "no such body";
            return false;
        }
        // NEAREST TO A POINT, not "nearest a direction" as a face is
        // picked. A face has one outward normal and a body rarely has
        // two faces sharing it; a box has four edges running along x and
        // a direction cannot tell them apart. Where an edge is, is the
        // only thing that distinguishes it.
        const Vec3d want = ReadVec(params.get("point"), Vec3d{0, 0, 0});
        const Vec3d along = ReadVec(params.get("along"), Vec3d{0, 0, 0});
        const bool filtering = along.LengthSquared() > 0.0;
        const Vec3d unit = filtering ? along.Normalized() : Vec3d{0, 0, 1};
        EntityId best = cad::kNoEntity;
        double nearest = 0.0;
        Vec3d best_point, best_direction, best_from, best_to;
        double best_length = 0.0;
        for (const EntityId edge : EdgesOfBody(document->model, body)) {
            Vec3d point, direction, from, to;
            double length = 0.0;
            if (!EdgeSample(document->model, edge, &point, &direction, &from, &to, &length)) {
                continue;
            }
            if (filtering && std::fabs(direction.Dot(unit)) < 0.999) continue;
            // Measured to the whole edge, sampled along it, rather than
            // to its midpoint: the midpoint of a long edge can be
            // further from the point asked for than the *end* of a
            // completely different one.
            const cad::Edge *e = document->model.GetEdge(edge);
            const cad::Curve3 *curve = document->model.CurveAt(e->curve);
            double distance = (point - want).Length();
            for (int i = 0; i <= 16; ++i) {
                const double t = e->t_start + (e->t_end - e->t_start) * i / 16.0;
                distance = std::min(distance, (curve->Point(t) - want).Length());
            }
            if (best == cad::kNoEntity || distance < nearest) {
                best = edge;
                nearest = distance;
                best_point = point;
                best_direction = direction;
                best_from = from;
                best_to = to;
                best_length = length;
            }
        }
        if (best == cad::kNoEntity) {
            *error = "this body has no edge matching that description";
            return false;
        }
        (*out)["edge"] = static_cast<long long>(best);
        (*out)["point"] = WriteVec(best_point);
        (*out)["direction"] = WriteVec(best_direction);
        (*out)["from"] = WriteVec(best_from);
        (*out)["to"] = WriteVec(best_to);
        (*out)["length"] = best_length;
        (*out)["distance"] = nearest;
        return true;
    }
    if (method == "part.faces" || method == "part.face_at") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        const EntityId body = static_cast<EntityId>(params.get("body").as_int(0));
        if (document->model.GetBody(body) == nullptr) {
            *error = "no such body";
            return false;
        }
        const std::vector<EntityId> faces = FacesOfBody(document->model, body);
        if (method == "part.faces") {
            Json list = Json::Array();
            for (const EntityId face : faces) {
                Vec3d point, normal;
                if (!FaceSample(document->model, face, &point, &normal)) continue;
                Json one = Json::Object();
                one["id"] = static_cast<long long>(face);
                one["point"] = WriteVec(point);
                one["normal"] = WriteVec(normal);
                // THE SAMPLE POINT ABOVE IS NOT ALWAYS ON THE FACE, which
                // is the trap this addition exists for. `point` is the
                // surface evaluated at the middle of its own parameter
                // domain, and a face is a *trimmed* piece of a surface:
                // a quarter of an elliptical tube reports a point at the
                // far side of the whole ellipse, outside the part
                // entirely. That is right for asking which way a face
                // looks and useless for asking where it is -- and a
                // caller picking "the face nearest here" gets a
                // completely different face without anything going
                // wrong.
                //
                // The centroid and the box come from the face's own
                // tessellation, which honours the trimming by
                // construction, so they are always on or around the face
                // itself.
                cad::TessellationMesh tessellation;
                std::string ignored;
                cad::TessellationOptions tessellation_options;
                if (cad::TessellateFace(document->model, face, tessellation_options, &tessellation,
                                        &ignored) &&
                    !tessellation.positions.empty()) {
                    Vec3d low = tessellation.positions.front();
                    Vec3d high = low;
                    Vec3d centre{0, 0, 0};
                    for (const Vec3d &p : tessellation.positions) {
                        low.x = std::min(low.x, p.x);
                        low.y = std::min(low.y, p.y);
                        low.z = std::min(low.z, p.z);
                        high.x = std::max(high.x, p.x);
                        high.y = std::max(high.y, p.y);
                        high.z = std::max(high.z, p.z);
                        centre = centre + p;
                    }
                    const double count = static_cast<double>(tessellation.positions.size());
                    one["centroid"] = WriteVec(Vec3d{centre.x / count, centre.y / count,
                                                     centre.z / count});
                    one["low"] = WriteVec(low);
                    one["high"] = WriteVec(high);
                }
                list.push_back(std::move(one));
            }
            (*out)["faces"] = std::move(list);
            return true;
        }
        // PICKING A FACE BY DIRECTION, because an agent cannot click on
        // one. Every study in the tests names its held face and its
        // loaded face this way, and it is the only addressing scheme that
        // survives the body being rebuilt at a different size.
        const Vec3d want = ReadVec(params.get("normal"), Vec3d{0, 0, 1});
        if (!(want.LengthSquared() > 0.0)) {
            *error = "the direction to look along cannot be zero";
            return false;
        }
        const Vec3d unit = want.Normalized();
        EntityId best = cad::kNoEntity;
        double agreement = -2.0;
        Vec3d best_point, best_normal;
        for (const EntityId face : faces) {
            Vec3d point, normal;
            if (!FaceSample(document->model, face, &point, &normal)) continue;
            if (!(normal.LengthSquared() > 0.0)) continue;
            const double dot = normal.Normalized().Dot(unit);
            if (dot <= agreement) continue;
            agreement = dot;
            best = face;
            best_point = point;
            best_normal = normal;
        }
        if (best == cad::kNoEntity) {
            *error = "the body has no face with a usable normal";
            return false;
        }
        (*out)["face"] = static_cast<long long>(best);
        (*out)["point"] = WriteVec(best_point);
        (*out)["normal"] = WriteVec(best_normal);
        (*out)["agreement"] = agreement;
        return true;
    }
    if (method == "part.mass") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        const EntityId body = static_cast<EntityId>(params.get("body").as_int(0));
        if (document->model.GetBody(body) == nullptr) {
            *error = "no such body";
            return false;
        }
        const double density = params.get("density").as_double(1.0);
        BodyMass properties;
        bool closed = false;
        // RELATIVE TO THE BODY, NOT ABSOLUTE. The tessellator's default
        // chord tolerance is 1e-2 in model units, which on a part
        // measured in metres is a centimetre -- so a 10 mm hole gets a
        // handful of facets and the volume came out 0.035% high because
        // an inscribed polygon removes less material than the cylinder
        // does. An absolute default is right for exactly one size of
        // model, and it is never the one in front of you.
        const double chord =
            params.get("chord_tolerance")
                .as_double(ModelScale(document->model, body) * 1e-5);
        if (!ComputeBodyMass(document->model, body, chord, &properties, &closed, error)) {
            return false;
        }
        (*out)["chord_tolerance"] = chord;
        (*out)["volume"] = properties.volume;
        (*out)["area"] = properties.area;
        (*out)["mass"] = properties.volume * density;
        (*out)["density"] = density;
        (*out)["centroid"] = WriteVec(properties.centroid);
        // REPORTED, BECAUSE THE NUMBERS MEAN NOTHING WITHOUT IT. An open
        // surface still produces a volume from the divergence theorem;
        // it is just not the volume of anything.
        (*out)["closed"] = closed;
        Json inertia = Json::Array();
        for (int i = 0; i < 3; ++i) {
            Json row = Json::Array();
            for (int j = 0; j < 3; ++j) row.push_back(properties.inertia[i][j] * density);
            inertia.push_back(std::move(row));
        }
        (*out)["inertia"] = std::move(inertia);
        return true;
    }
    if (method == "part.validate") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        const EntityId body = static_cast<EntityId>(params.get("body").as_int(0));
        if (document->model.GetBody(body) == nullptr) {
            *error = "no such body";
            return false;
        }
        cad::ValidationReport report;
        const bool ok = cad::ValidateBody(document->model, body, &report);
        (*out)["ok"] = ok;
        (*out)["errors"] = report.ErrorCount();
        (*out)["warnings"] = report.WarningCount();
        Json problems = Json::Array();
        for (const cad::Diagnostic &diagnostic : report.diagnostics) {
            Json one = Json::Object();
            one["severity"] = diagnostic.severity == cad::Severity::Error ? "error" : "warning";
            one["category"] = diagnostic.category;
            one["what"] = diagnostic.message;
            one["entity"] = static_cast<long long>(diagnostic.entity);
            problems.push_back(std::move(one));
        }
        (*out)["problems"] = std::move(problems);
        if (!report.body_genus.empty()) (*out)["genus"] = report.body_genus.front();
        return true;
    }

    // --- Files ---------------------------------------------------------------
    if (method == "part.export") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        const std::string path = params.get("path").as_string("");
        if (path.empty()) {
            *error = "an export needs a path";
            return false;
        }
        std::string format = params.get("format").as_string("");
        if (format.empty()) format = LowerExtension(path);
        if (format == "stp") format = "step";
        std::vector<EntityId> bodies;
        if (params.contains("body")) {
            bodies.push_back(static_cast<EntityId>(params.get("body").as_int(0)));
        } else {
            for (const cad::Body &body : document->model.Bodies()) bodies.push_back(body.id);
        }
        if (bodies.empty()) {
            *error = "there is nothing in this document to export";
            return false;
        }
        std::string text;
        bool made = false;
        if (format == "step") {
            made = cad::WriteStepShapes(document->model, bodies, &text, error);
        } else if (format == "stl") {
            made = cad::WriteStl(document->model, bodies, &text, error);
        } else if (format == "obj") {
            made = cad::WriteObj(document->model, bodies, &text, error);
        } else if (format == "gltf") {
            made = cad::WriteGltf(document->model, bodies, &text, error);
        } else {
            *error = "unknown export format '" + format + "'; step, stl, obj and gltf are written";
            return false;
        }
        if (!made) return false;
        if (!WriteTextFile(path, text, error)) return false;
        (*out)["path"] = path;
        (*out)["format"] = format;
        (*out)["bytes"] = static_cast<long long>(text.size());
        (*out)["bodies"] = static_cast<int>(bodies.size());
        return true;
    }
    if (method == "part.import") {
        const std::string path = params.get("path").as_string("");
        std::string format = params.get("format").as_string("");
        if (format.empty()) format = LowerExtension(path);
        if (format == "stp") format = "step";
        std::string text;
        if (!ReadTextFile(path, &text, error)) return false;
        Document document;
        document.title = path;
        document.path = path;
        Json warnings = Json::Array();
        int features = 0;
        double volume = 0.0;
        if (format == "step") {
            cad::StepReadReport report;
            if (!cad::ReadStepText(text, &document.model, &report)) {
                *error = report.error.empty() ? "the STEP file could not be read" : report.error;
                return false;
            }
            for (const std::string &warning : report.warnings) warnings.push_back(warning);
            if (!Finish(&document.model, error)) return false;
        } else if (format == "mepcad") {
            // A `.mepcad` FILE IS A FEATURE TREE AND NOTHING ELSE -- no
            // faces, no surfaces, no geometry at all -- so reading one is
            // reading the operations and then replaying them. That is
            // what makes this worth having here rather than routing
            // through STEP: the same file opens in the editor's CAD pane
            // as an editable part, and opens here as the body it builds.
            // A STEP export of it would be neither.
            //
            // Nothing is *written* back. The headless session holds an
            // evaluated model built by primitives and booleans, with no
            // history to record; Part K.5 says so and this does not
            // change it. The traffic is one way on purpose.
            cad::CadDocument read;
            if (!cad::ReadCadDocument(text, &read, error)) return false;
            std::string rebuild_error;
            if (!read.tree.Rebuild(&rebuild_error)) {
                *error = rebuild_error.empty() ? "the feature tree would not rebuild"
                                               : rebuild_error;
                return false;
            }
            document.model = read.tree.Result();
            if (!read.title.empty()) document.title = read.title;
            features = static_cast<int>(read.tree.Features().size());
            volume = read.tree.Volume();
            // Deliberately NOT run through Finish: the tree's own rebuild
            // has already produced a finished model, and re-running the
            // topology fixups over it is at best wasted work and at worst
            // a second opinion about a model that already had one.
        } else {
            *error = "unknown import format '" + format +
                     "'; step and mepcad are read into a model";
            return false;
        }
        const int id = next_id_++;
        (*out)["document"] = id;
        (*out)["bodies"] = static_cast<int>(document.model.Bodies().size());
        (*out)["warnings"] = std::move(warnings);
        if (features > 0) {
            (*out)["features"] = features;
            (*out)["volume"] = volume;
        }
        documents_[id] = std::move(document);
        return true;
    }

    // --- Meshing and studies --------------------------------------------------
    if (method == "fem.mesh") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        const EntityId body = static_cast<EntityId>(params.get("body").as_int(0));
        if (document->model.GetBody(body) == nullptr) {
            *error = "no such body";
            return false;
        }
        // A TARGET SIZE IN THE MODEL'S OWN UNITS, defaulting to a tenth
        // of its diagonal. An absolute default would be wrong for every
        // model but one, and the commonest way to get an unusable mesh
        // out of any mesher is to ask it for the wrong size without
        // realising the units are not what you thought.
        const double scale = ModelScale(document->model, body);
        const double size = params.get("size").as_double(scale * 0.1);
        const int order = params.get("order").as_int(1);
        const std::string how = params.get("method").as_string("tetrahedra");

        // SWEEPING WAS ON THE INSIDE AND NOT ON THE SURFACE, which meant
        // the one mesher that suits a prismatic part was unreachable by
        // anybody driving mep rather than linking it. Part G.6 built it
        // for exactly the case the tetrahedral pipeline is worst at:
        // Hex8 under bending against Tet4 under bending is not a close
        // contest, and an extruded profile is precisely the sort of part
        // that gets bent. Every plate, beam and membrane benchmark in
        // examples/nafems is an extruded profile.
        fem::VolumeMesh mesh;
        fem::MeshReport report;
        if (how == "sweep" || how == "hexahedra" || how == "hex") {
            fem::SweepMeshOptions sweep;
            sweep.sizing.target = size;
            sweep.surface.sizing.target = size;
            sweep.layers = params.get("layers").as_int(0);
            if (!fem::MeshSweep(document->model, body, sweep, &mesh, &report)) {
                *error = report.error.empty()
                             ? "the body could not be swept into hexahedra; it has to be one"
                               " planar face swept onto a parallel one"
                             : report.error;
                return false;
            }
        } else if (how == "tetrahedra" || how == "tet" || how == "tets") {
            fem::VolumeMeshOptions options;
            options.surface.sizing.target = size;
            if (!fem::MeshBody(document->model, {body}, options, &mesh, &report)) {
                *error = report.error.empty() ? "the body could not be meshed" : report.error;
                return false;
            }
        } else {
            *error = "no meshing method called '" + how + "'; tetrahedra and sweep are offered";
            return false;
        }
        if (order >= 2 && mesh.hexes.empty()) {
            fem::MeshReport curved;
            if (!fem::MakeSecondOrder(document->model, &mesh, &curved)) {
                *error = curved.error.empty() ? "the mesh could not be made second order"
                                              : curved.error;
                return false;
            }
        }
        const int id = next_id_++;
        MeshRecord record;
        record.document = params.get("document").as_int(-1);
        record.worst_dihedral = report.min_dihedral;
        record.mesh = std::move(mesh);
        (*out)["mesh"] = id;
        (*out)["nodes"] = record.mesh.NodeCount();
        (*out)["elements"] =
            record.mesh.hexes.empty() ? record.mesh.TetCount() : record.mesh.HexCount();
        (*out)["shape"] = record.mesh.hexes.empty() ? "tetrahedra" : "hexahedra";
        (*out)["order"] = record.mesh.tets10.empty() ? 1 : 2;
        (*out)["worst_dihedral"] = record.worst_dihedral;
        (*out)["target_size"] = size;
        meshes_[id] = std::move(record);
        return true;
    }
    if (method == "fem.study") {
        Document *document = Doc(params, error);
        if (document == nullptr) return false;
        const int id = next_id_++;
        StudyRecord record;
        record.document = params.get("document").as_int(-1);
        record.study.name = params.get("name").as_string("study");
        // A DEFAULT MATERIAL, so that a study is solvable the moment it
        // has a support and a load. Steel rather than nothing, because a
        // study with no material at all fails at bind time with a message
        // about an index, and the commonest thing anyone wants is steel.
        fem::StudyMaterial steel;
        steel.name = "steel";
        record.study.materials.push_back(steel);
        (*out)["study"] = id;
        (*out)["material"] = steel.name;
        studies_[id] = std::move(record);
        return true;
    }
    if (method == "fem.material") {
        StudyRecord *study = StudyOf(params, error);
        if (study == nullptr) return false;
        fem::StudyMaterial material;
        material.name = params.get("name").as_string("material");
        material.youngs_modulus =
            fem::MaterialCurve::Constant(params.get("youngs_modulus").as_double(210e9));
        material.poissons_ratio =
            fem::MaterialCurve::Constant(params.get("poissons_ratio").as_double(0.3));
        material.density = params.get("density").as_double(7850.0);
        material.thermal_expansion =
            fem::MaterialCurve::Constant(params.get("thermal_expansion").as_double(1.2e-5));
        if (!(material.youngs_modulus.At(20.0) > 0.0)) {
            *error = "the modulus must be positive";
            return false;
        }
        const double nu = material.poissons_ratio.At(20.0);
        // HALF IS INCOMPRESSIBLE AND THE CONSTITUTIVE MATRIX DIVIDES BY
        // (1 - 2 nu). Refusing it here says so; letting it through gives
        // an infinity that surfaces as a solver failure four calls later.
        if (!(nu > -1.0 && nu < 0.5)) {
            *error = "Poisson's ratio must be between -1 and 0.5, exclusive";
            return false;
        }
        study->study.materials.assign(1, material);
        (*out)["materials"] = static_cast<int>(study->study.materials.size());
        (*out)["name"] = material.name;
        return true;
    }
    if (method == "fem.support" || method == "fem.load") {
        StudyRecord *study = StudyOf(params, error);
        if (study == nullptr) return false;
        const auto document = documents_.find(study->document);
        if (document == documents_.end()) {
            *error = "the study's document has been closed";
            return false;
        }
        const bool gravity = params.get("kind").as_string("") == "gravity";
        fem::Target target;
        if (params.contains("edge")) {
            const EntityId edge = static_cast<EntityId>(params.get("edge").as_int(0));
            if (document->second.model.GetEdge(edge) == nullptr) {
                *error = "no such edge in the study's document";
                return false;
            }
            target = fem::EdgeTarget(document->second.model, edge);
        } else if (!gravity) {
            const EntityId face = static_cast<EntityId>(params.get("face").as_int(0));
            if (document->second.model.GetFace(face) == nullptr) {
                *error = "no such face in the study's document";
                return false;
            }
            // CAPTURED AS A PERSISTENT NAME, not as an id. That is the
            // whole of Part B.4 and the reason a study survives the body
            // being rebuilt at a different size -- which an adaptive loop
            // does on every cycle.
            target = fem::FaceTarget(document->second.model, face);
        }
        if (method == "fem.support") {
            fem::Support support;
            support.label = params.get("label").as_string("support");
            support.where = target;
            if (params.get("fixed").is_array()) {
                const std::vector<Json> &axes = params.get("fixed").items();
                for (int axis = 0; axis < 3; ++axis) {
                    support.fixed[axis] =
                        axis < static_cast<int>(axes.size()) ? axes[Idx(axis)].as_bool(true) : false;
                }
            }
            study->study.supports.push_back(support);
            (*out)["supports"] = static_cast<int>(study->study.supports.size());
            return true;
        }
        fem::Load load;
        load.label = params.get("label").as_string("load");
        load.where = target;
        const std::string kind = params.get("kind").as_string("");
        if (kind == "force") {
            load.kind = fem::LoadKind::Force;
        } else if (kind == "pressure") {
            load.kind = fem::LoadKind::Pressure;
        } else if (kind == "traction") {
            load.kind = fem::LoadKind::Traction;
        } else if (kind == "gravity") {
            load.kind = fem::LoadKind::Gravity;
        } else {
            *error = "kind must be force, pressure, traction or gravity, not '" + kind + "'";
            return false;
        }
        load.magnitude = params.get("magnitude").as_double(0.0);
        load.vector = ReadVec(params.get("vector"), Vec3d{});
        if (load.kind == fem::LoadKind::Pressure && load.magnitude == 0.0) {
            *error = "a pressure needs a magnitude";
            return false;
        }
        if (load.kind != fem::LoadKind::Pressure && !(load.vector.LengthSquared() > 0.0)) {
            *error = "a force, traction or gravity load needs a vector";
            return false;
        }
        study->study.loads.push_back(load);
        (*out)["loads"] = static_cast<int>(study->study.loads.size());
        return true;
    }

    // --- Solving ---------------------------------------------------------------
    if (method == "fem.solve" || method == "fem.modal") {
        StudyRecord *study = StudyOf(params, error);
        if (study == nullptr) return false;
        MeshRecord *mesh = MeshOf(params, error);
        if (mesh == nullptr) return false;
        if (mesh->document != study->document) {
            *error = "the mesh and the study are of different documents";
            return false;
        }
        const auto document = documents_.find(study->document);
        if (document == documents_.end()) {
            *error = "the study's document has been closed";
            return false;
        }
        ResultRecord record;
        record.document = study->document;
        record.study = params.get("study").as_int(-1);
        record.mesh = params.get("mesh").as_int(-1);
        fem::BindReport bind;
        if (!fem::BindStudy(document->second.model, mesh->mesh, study->study, {}, &record.model,
                            &bind)) {
            *error = bind.error.empty() ? "the study could not be attached to the mesh"
                                        : bind.error;
            return false;
        }
        Json warnings = Json::Array();
        for (const std::string &warning : bind.warnings) warnings.push_back(warning);

        if (method == "fem.solve") {
            if (!fem::SolveStatic(record.model, {}, {}, &record.result)) {
                *error = record.result.error;
                return false;
            }
            std::vector<double> flat(Idx(record.model.NodeCount() * 3), 0.0);
            for (int node = 0; node < record.model.NodeCount(); ++node) {
                flat[Idx(node * 3 + 0)] = record.result.displacement[Idx(node)].x;
                flat[Idx(node * 3 + 1)] = record.result.displacement[Idx(node)].y;
                flat[Idx(node * 3 + 2)] = record.result.displacement[Idx(node)].z;
            }
            fem::ErrorEstimate estimate;
            std::string ignored;
            // THE ERROR ESTIMATE COMES BACK WITH EVERY SOLVE, not only
            // from the adaptive loop. A stress number without any idea of
            // how converged it is, handed to something that will act on
            // it, is the single most dangerous output this whole system
            // produces; it costs one pass over the elements to say.
            if (fem::EstimateError(record.model, flat, record.result.stress, &estimate,
                                   &ignored)) {
                (*out)["relative_error"] = estimate.relative_error;
            }
            (*out)["max_displacement"] = record.result.max_displacement;
            (*out)["max_displacement_node"] = record.result.max_displacement_node;
            (*out)["max_von_mises"] = record.result.max_von_mises;
            (*out)["max_von_mises_node"] = record.result.max_von_mises_node;
            (*out)["strain_energy"] = record.result.strain_energy;
            (*out)["equilibrium_residual"] = record.result.equilibrium_residual;
            (*out)["solver"] = record.result.solver_used;
        } else {
            fem::ModalOptions options;
            options.modes = std::max(1, params.get("modes").as_int(6));
            if (!fem::SolveModal(record.model, {}, options, &record.modes)) {
                *error = record.modes.error;
                return false;
            }
            record.has_modes = true;
            Json frequencies = Json::Array();
            for (const double hz : record.modes.frequency) frequencies.push_back(hz);
            (*out)["frequencies"] = std::move(frequencies);
            (*out)["rigid_body_modes"] = record.modes.rigid_body_modes;
            (*out)["sturm_agrees"] = record.modes.sturm_agrees;
        }
        const int id = next_id_++;
        (*out)["result"] = id;
        (*out)["nodes"] = record.model.NodeCount();
        (*out)["elements"] = record.model.ElementCount();
        (*out)["warnings"] = std::move(warnings);
        results_[id] = std::move(record);
        return true;
    }

    if (method == "fem.adapt") {
        StudyRecord *study = StudyOf(params, error);
        if (study == nullptr) return false;
        const auto document = documents_.find(study->document);
        if (document == documents_.end()) {
            *error = "the study's document has been closed";
            return false;
        }
        const EntityId body = static_cast<EntityId>(params.get("body").as_int(0));
        if (document->second.model.GetBody(body) == nullptr) {
            *error = "no such body";
            return false;
        }
        fem::AdaptOptions options;
        options.cycles = std::max(1, params.get("cycles").as_int(3));
        options.target_relative_error = params.get("target").as_double(0.05);
        const double scale = ModelScale(document->second.model, body);
        options.mesh.surface.sizing.target = params.get("size").as_double(scale * 0.1);
        ResultRecord record;
        record.document = study->document;
        record.study = params.get("study").as_int(-1);
        fem::AdaptReport report;
        if (!fem::AdaptiveSolve(document->second.model, {body}, study->study, options,
                                &record.model, &record.result, &report)) {
            *error = report.error;
            return false;
        }
        Json cycles = Json::Array();
        for (const fem::AdaptCycle &cycle : report.cycles) {
            Json one = Json::Object();
            one["elements"] = cycle.elements;
            one["nodes"] = cycle.nodes;
            one["relative_error"] = cycle.relative_error;
            one["strain_energy"] = cycle.strain_energy;
            one["max_von_mises"] = cycle.max_von_mises;
            one["refined"] = cycle.refined;
            cycles.push_back(std::move(one));
        }
        const int id = next_id_++;
        (*out)["result"] = id;
        (*out)["cycles"] = std::move(cycles);
        (*out)["reached_target"] = report.reached_target;
        (*out)["stopped_early"] = report.stopped_early;
        if (report.stopped_early) (*out)["stop_reason"] = report.stop_reason;
        (*out)["nodes"] = record.model.NodeCount();
        (*out)["elements"] = record.model.ElementCount();
        results_[id] = std::move(record);
        return true;
    }

    // --- Reading results ----------------------------------------------------------
    if (method == "fem.summary") {
        ResultRecord *record = ResultOf(params, error);
        if (record == nullptr) return false;
        (*out)["nodes"] = record->model.NodeCount();
        (*out)["elements"] = record->model.ElementCount();
        (*out)["modal"] = record->has_modes;
        if (record->has_modes) {
            Json frequencies = Json::Array();
            for (const double hz : record->modes.frequency) frequencies.push_back(hz);
            (*out)["frequencies"] = std::move(frequencies);
            return true;
        }
        (*out)["max_displacement"] = record->result.max_displacement;
        (*out)["max_von_mises"] = record->result.max_von_mises;
        (*out)["strain_energy"] = record->result.strain_energy;
        (*out)["equilibrium_residual"] = record->result.equilibrium_residual;
        return true;
    }

    if (method == "fem.field" || method == "fem.probe" || method == "fem.path" ||
        method == "fem.export") {
        ResultRecord *record = ResultOf(params, error);
        if (record == nullptr) return false;
        if (record->has_modes) {
            *error = "this result is a modal one; it has frequencies and mode shapes rather"
                     " than a stress field";
            return false;
        }
        bool known = false;
        const std::string name = params.get("field").as_string("von_mises");
        const fem::ScalarField which = ResolveField(name, &known);
        if (!known) {
            *error = "no field called '" + name + "'";
            return false;
        }
        std::vector<double> values;
        fem::Strength strength;
        strength.tensile = params.get("strength").as_double(250e6);
        if (!fem::SampleField(record->model, record->result.displacement, record->result.stress,
                              which, strength, &values, error)) {
            return false;
        }

        if (method == "fem.field") {
            int lowest = -1;
            int highest = -1;
            for (int node = 0; node < record->model.NodeCount(); ++node) {
                if (!std::isfinite(values[Idx(node)])) continue;
                if (lowest < 0 || values[Idx(node)] < values[Idx(lowest)]) lowest = node;
                if (highest < 0 || values[Idx(node)] > values[Idx(highest)]) highest = node;
            }
            (*out)["field"] = fem::ScalarFieldName(which);
            (*out)["larger_is_worse"] = fem::LargerIsWorse(which);
            if (lowest >= 0) {
                (*out)["min"] = values[Idx(lowest)];
                (*out)["min_node"] = lowest;
                (*out)["at_min"] = WriteVec(record->model.nodes[Idx(lowest)]);
            }
            if (highest >= 0) {
                (*out)["max"] = values[Idx(highest)];
                (*out)["max_node"] = highest;
                (*out)["at_max"] = WriteVec(record->model.nodes[Idx(highest)]);
            }
            return true;
        }

        if (method == "fem.probe") {
            std::vector<Vec3d> at;
            for (const Json &point : params.get("points").items()) {
                at.push_back(ReadVec(point, Vec3d{}));
            }
            if (at.empty()) {
                *error = "probe needs at least one point";
                return false;
            }
            std::vector<fem::Probe> probes;
            if (!fem::ProbePoints(record->model, values, at, &probes, error)) return false;
            Json list = Json::Array();
            for (const fem::Probe &probe : probes) {
                Json one = Json::Object();
                one["at"] = WriteVec(probe.at);
                one["value"] = probe.value;
                one["inside"] = probe.inside;
                one["distance"] = probe.distance;
                list.push_back(std::move(one));
            }
            (*out)["field"] = fem::ScalarFieldName(which);
            (*out)["probes"] = std::move(list);
            return true;
        }

        const bool exporting = method == "fem.export";
        const bool along_a_path = params.contains("from") && params.contains("to");
        fem::Table table;
        if (along_a_path || !exporting) {
            const Vec3d from = ReadVec(params.get("from"), Vec3d{});
            const Vec3d to = ReadVec(params.get("to"), Vec3d{});
            std::vector<fem::PathSample> path;
            if (!fem::ProbePath(record->model, values, {from, to},
                                std::max(2, params.get("samples").as_int(20)), &path, error)) {
                return false;
            }
            table = fem::PathTable(path, fem::ScalarFieldName(which));
            if (!exporting) {
                Json rows = Json::Array();
                for (const fem::PathSample &sample : path) {
                    Json one = Json::Object();
                    one["distance"] = sample.distance;
                    one["at"] = WriteVec(sample.at);
                    one["value"] = sample.value;
                    one["inside"] = sample.inside;
                    rows.push_back(std::move(one));
                }
                (*out)["field"] = fem::ScalarFieldName(which);
                (*out)["rows"] = std::move(rows);
                (*out)["csv"] = fem::ToCsv(table);
                return true;
            }
        } else {
            // Every node, when no path was named: the whole field, which
            // is what someone exporting to plot elsewhere usually wants.
            table.columns = {"node", "x", "y", "z", fem::ScalarFieldName(which)};
            for (int node = 0; node < record->model.NodeCount(); ++node) {
                const Vec3d &p = record->model.nodes[Idx(node)];
                table.rows.push_back({static_cast<double>(node), p.x, p.y, p.z, values[Idx(node)]});
            }
        }
        const std::string path = params.get("path").as_string("");
        if (path.empty()) {
            *error = "an export needs a path";
            return false;
        }
        std::string format = params.get("format").as_string("");
        if (format.empty()) format = LowerExtension(path);
        std::string text;
        if (format == "csv") {
            text = fem::ToCsv(table);
        } else if (format == "org") {
            text = fem::ToOrgTable(table);
        } else {
            *error = "unknown export format '" + format + "'; csv and org are written";
            return false;
        }
        if (!WriteTextFile(path, text, error)) return false;
        (*out)["path"] = path;
        (*out)["format"] = format;
        (*out)["rows"] = static_cast<int>(table.rows.size());
        (*out)["bytes"] = static_cast<long long>(text.size());
        return true;
    }

    if (method == "fem.animate") {
        ResultRecord *record = ResultOf(params, error);
        if (record == nullptr) return false;
        if (!record->has_modes) {
            *error = "only a modal result has mode shapes to animate";
            return false;
        }
        fem::AnimationOptions options;
        options.frames = std::max(1, params.get("frames").as_int(24));
        options.amplitude_fraction = params.get("amplitude").as_double(0.08);
        fem::Animation animation;
        if (!fem::AnimateMode(record->model, record->modes, params.get("mode").as_int(0), options,
                              &animation, error)) {
            return false;
        }
        (*out)["frames"] = animation.frames;
        (*out)["period"] = animation.period;
        (*out)["amplitude_scale"] = animation.amplitude_scale;
        Json times = Json::Array();
        for (const double t : animation.time) times.push_back(t);
        (*out)["time"] = std::move(times);
        Json frames = Json::Array();
        for (const std::vector<Vec3d> &frame : animation.displacement) {
            Json one = Json::Array();
            for (const Vec3d &u : frame) {
                one.push_back(u.x);
                one.push_back(u.y);
                one.push_back(u.z);
            }
            frames.push_back(std::move(one));
        }
        // FLATTENED, THREE NUMBERS PER NODE. A frame of ten thousand
        // nodes as ten thousand three-element arrays is an order of
        // magnitude more JSON than the same numbers laid end to end, and
        // an animation is the one call here that returns enough data for
        // that to matter.
        (*out)["displacement"] = std::move(frames);
        return true;
    }

    if (method == "fem.nodes") {
        ResultRecord *record = ResultOf(params, error);
        if (record == nullptr) return false;
        // FLATTENED, THREE NUMBERS PER NODE, matching fem.animate's own
        // layout -- which is the whole point of having this at all. A
        // mode shape identified by what it *is* rather than by where it
        // sits in the list needs the shape and the coordinates in the
        // same indexing, and until now the surface handed out the first
        // and kept the second.
        Json nodes = Json::Array();
        for (const Vec3d &p : record->model.nodes) {
            nodes.push_back(p.x);
            nodes.push_back(p.y);
            nodes.push_back(p.z);
        }
        (*out)["count"] = record->model.NodeCount();
        (*out)["nodes"] = std::move(nodes);
        return true;
    }

    if (method == "fem.movie" || method == "fem.image") {
        ResultRecord *record = ResultOf(params, error);
        if (record == nullptr) return false;
        const std::string path = params.get("path").as_string("");
        if (path.empty()) {
            *error = "a picture needs a path to be written to";
            return false;
        }

        fem::MovieOptions options;
        options.width = std::max(64, params.get("width").as_int(720));
        options.height = std::max(64, params.get("height").as_int(540));
        options.fps = std::max(1, params.get("fps").as_int(20));
        options.camera.orbit = params.get("orbit").as_bool(false);
        options.caption = params.get("caption").as_string("");
        if (params.contains("yaw")) {
            options.camera.yaw = params.get("yaw").as_double(0.0) * kDegrees;
        }
        if (params.contains("pitch")) {
            options.camera.pitch = params.get("pitch").as_double(0.0) * kDegrees;
        }
        options.render.auto_scale_fraction = params.get("amplitude").as_double(0.08);
        const std::string map = params.get("color_map").as_string("viridis");
        if (map == "blue_to_red") {
            options.render.color_map = fem::ColorMap::BlueToRed;
        } else if (map == "greyscale" || map == "grayscale") {
            options.render.color_map = fem::ColorMap::Greyscale;
        } else if (map != "viridis" && !map.empty()) {
            *error = "no colour map called '" + map + "'";
            return false;
        }

        const int frame_count = std::max(1, params.get("frames").as_int(36));
        std::vector<std::vector<Vec3d>> displacement;
        std::vector<std::vector<double>> values;

        if (record->has_modes) {
            // A MODE HAS NO STRESS FIELD, so it is coloured by how far
            // each point moves. Colouring it by a stress would mean
            // inventing an amplitude for an eigenvector, and the whole
            // point of fem_animate.h's amplitude note is that there is
            // not one.
            fem::AnimationOptions animation_options;
            animation_options.frames = frame_count;
            animation_options.amplitude_fraction = options.render.auto_scale_fraction;
            fem::Animation animation;
            const int mode = params.get("mode").as_int(0);
            if (!fem::AnimateMode(record->model, record->modes, mode, animation_options,
                                  &animation, error)) {
                return false;
            }
            if (options.units.empty()) options.units = "MOVEMENT";
            if (options.caption.empty() && mode < static_cast<int>(record->modes.frequency.size())) {
                char text[96];
                std::snprintf(text, sizeof(text), "MODE %d   %.4g HZ", mode + 1,
                              record->modes.frequency[Idx(mode)]);
                options.caption = text;
            }
            for (const std::vector<Vec3d> &frame : animation.displacement) {
                std::vector<double> magnitude(frame.size(), 0.0);
                for (std::size_t n = 0; n < frame.size(); ++n) {
                    magnitude[n] = std::sqrt(frame[n].x * frame[n].x + frame[n].y * frame[n].y +
                                             frame[n].z * frame[n].z);
                }
                displacement.push_back(frame);
                values.push_back(std::move(magnitude));
            }
            // A still of a mode is that mode at its extreme, which is
            // frame zero: AnimateMode starts a cycle at the peak.
            if (method == "fem.image") {
                displacement.resize(1);
                values.resize(1);
            }
        } else {
            bool known = false;
            const std::string name = params.get("field").as_string("von_mises");
            const fem::ScalarField which = ResolveField(name, &known);
            if (!known) {
                *error = "no field called '" + name + "'";
                return false;
            }
            std::vector<double> field;
            fem::Strength strength;
            strength.tensile = params.get("strength").as_double(250e6);
            if (!fem::SampleField(record->model, record->result.displacement,
                                  record->result.stress, which, strength, &field, error)) {
                return false;
            }
            if (options.units.empty()) options.units = fem::ScalarFieldName(which);
            if (method == "fem.image") {
                displacement.push_back(record->result.displacement);
                values.push_back(field);
            } else {
                // THE LOAD GOING ON AND COMING OFF AGAIN, rather than a
                // still held for two seconds. A linear static result
                // scales exactly with the load, so every intermediate
                // frame is a real answer to a real load case rather than
                // an interpolation -- and a full cycle loops seamlessly,
                // which a ramp that stops at full load does not.
                for (int f = 0; f < frame_count; ++f) {
                    const double phase = kTwoPi * static_cast<double>(f) /
                                         static_cast<double>(frame_count);
                    const double factor = 0.5 * (1.0 - std::cos(phase));
                    std::vector<Vec3d> step(record->result.displacement.size());
                    for (std::size_t n = 0; n < step.size(); ++n) {
                        step[n] = record->result.displacement[n] * factor;
                    }
                    std::vector<double> scaled(field.size(), 0.0);
                    for (std::size_t n = 0; n < field.size(); ++n) scaled[n] = field[n] * factor;
                    displacement.push_back(std::move(step));
                    values.push_back(std::move(scaled));
                }
            }
        }

        fem::MovieReport report;
        const bool made =
            method == "fem.image"
                ? fem::WriteStill(record->model, displacement.front(), values.front(), options,
                                  path, &report, error)
                : fem::WriteMovie(record->model, displacement, values, options, path, &report,
                                  error);
        if (!made) return false;
        (*out)["path"] = path;
        (*out)["width"] = report.width;
        (*out)["height"] = report.height;
        (*out)["field_min"] = report.field_min;
        (*out)["field_max"] = report.field_max;
        (*out)["bytes"] = report.bytes;
        if (method == "fem.movie") {
            (*out)["frames"] = report.frames;
            (*out)["seconds"] = report.seconds;
        }
        return true;
    }

    *error = "the method '" + method + "' is declared but not implemented";
    return false;
}

}  // namespace cadfem
