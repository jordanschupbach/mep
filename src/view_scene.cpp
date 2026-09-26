#include "view_scene.h"

#include "cad_tessellate.h"
#include "fem_animate.h"
#include "fem_result.h"

#include <algorithm>
#include <cmath>

namespace view {
namespace {

using cad::Vec3d;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }
float F(double v) { return static_cast<float>(v); }

// Appends one vertex to a RenderMesh and returns its index.
unsigned int PushVertex(fem::RenderMesh *mesh, const Vec3d &at, const Vec3d &normal,
                        const std::array<unsigned char, 4> &color) {
    const unsigned int index = static_cast<unsigned int>(mesh->positions.size() / 3);
    mesh->positions.push_back(F(at.x));
    mesh->positions.push_back(F(at.y));
    mesh->positions.push_back(F(at.z));
    mesh->normals.push_back(F(normal.x));
    mesh->normals.push_back(F(normal.y));
    mesh->normals.push_back(F(normal.z));
    for (int c = 0; c < 4; ++c) mesh->colors.push_back(color[Idx(c)]);
    return index;
}

void Finish(fem::RenderMesh *mesh) {
    mesh->vertex_count = static_cast<int>(mesh->positions.size() / 3);
    mesh->triangle_count = static_cast<int>(mesh->indices.size() / 3);
}

// A line-only item, which is what every annotation is.
fem::RenderMesh *LineItem(Scene *scene, const std::string &name) {
    scene->items.push_back(Item{});
    scene->items.back().name = name;
    return &scene->items.back().mesh;
}

}  // namespace

bool Scene::Empty() const { return items.empty() && labels.empty(); }

int Scene::TriangleCount() const {
    int total = 0;
    for (const Item &item : items) total += static_cast<int>(item.mesh.indices.size() / 3);
    return total;
}

int Scene::VertexCount() const {
    int total = 0;
    for (const Item &item : items) total += static_cast<int>(item.mesh.positions.size() / 3);
    return total;
}

bool Scene::Bounds(Vec3d *low, Vec3d *high) const {
    bool any = false;
    for (const Item &item : items) {
        for (std::size_t v = 0; v + 2 < item.mesh.positions.size(); v += 3) {
            const Vec3d p{static_cast<double>(item.mesh.positions[v]),
                          static_cast<double>(item.mesh.positions[v + 1]),
                          static_cast<double>(item.mesh.positions[v + 2])};
            if (!any) {
                *low = p;
                *high = p;
                any = true;
                continue;
            }
            low->x = std::min(low->x, p.x);
            low->y = std::min(low->y, p.y);
            low->z = std::min(low->z, p.z);
            high->x = std::max(high->x, p.x);
            high->y = std::max(high->y, p.y);
            high->z = std::max(high->z, p.z);
        }
    }
    // A label is somewhere too, and a scene that is only labels should
    // still frame itself rather than showing a unit box at the origin.
    for (const Label &label : labels) {
        if (!any) {
            *low = label.at;
            *high = label.at;
            any = true;
            continue;
        }
        low->x = std::min(low->x, label.at.x);
        low->y = std::min(low->y, label.at.y);
        low->z = std::min(low->z, label.at.z);
        high->x = std::max(high->x, label.at.x);
        high->y = std::max(high->y, label.at.y);
        high->z = std::max(high->z, label.at.z);
    }
    return any;
}

void Scene::Clear() {
    items.clear();
    labels.clear();
    has_field = false;
    field_min = 0.0;
    field_max = 0.0;
    units.clear();
    // THE CAPTION SURVIVES, and that is a deliberate exception rather
    // than an oversight. Everything else here is derived from the items:
    // the colour range is their range, the units are their field's, and
    // a scene with no items has none of it. A caption is what the viewer
    // is *called* -- it is set once, at the top of a script, next to the
    // part it names -- and clearing it on every frame meant the title
    // vanished the first time anybody touched the slider. Setting it
    // inside the frame callback would work and is a silly thing to
    // require.
}

void Scene::NoteField(double low, double high, const std::string &units_of_it) {
    if (!has_field) {
        has_field = true;
        field_min = low;
        field_max = high;
        units = units_of_it;
        return;
    }
    field_min = std::min(field_min, low);
    field_max = std::max(field_max, high);
    // TWO RESULTS, ONE COLOUR BAR, AND ONE NAME FOR IT. If the second
    // field is not the first's, the bar cannot honestly be labelled
    // either, so it is labelled neither.
    if (units != units_of_it) units = "mixed";
}

bool AddPart(Scene *scene, const cad::Model &model, cad::EntityId body,
             const PartOptions &options, std::string *error) {
    if (model.GetBody(body) == nullptr) {
        if (error != nullptr) *error = "no such body";
        return false;
    }
    cad::TessellationOptions tessellation;
    if (options.chord_tolerance > 0.0) {
        tessellation.chord_tolerance = options.chord_tolerance;
    } else {
        // A THOUSANDTH OF THE BODY'S OWN DIAGONAL. An absolute default
        // is right for exactly one size of part and never the one in
        // front of you: 1e-2 gives a 10 mm hole a handful of facets and
        // a 10 m plate more triangles than it can draw.
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
        const double diagonal = (box.Max() - box.Min()).Length();
        tessellation.chord_tolerance = (diagonal > 0.0 ? diagonal : 1.0) * 1e-3;
    }
    cad::TessellationMesh mesh;
    std::string ignored;
    if (!cad::TessellateBody(model, body, tessellation, &mesh, &ignored)) {
        if (error != nullptr) {
            *error = ignored.empty() ? "the body could not be tessellated" : ignored;
        }
        return false;
    }
    scene->items.push_back(Item{});
    Item &item = scene->items.back();
    item.name = options.name.empty() ? "part" : options.name;
    for (std::size_t v = 0; v < mesh.positions.size(); ++v) {
        const Vec3d normal = v < mesh.normals.size() ? mesh.normals[v] : Vec3d{0, 0, 1};
        PushVertex(&item.mesh, mesh.positions[v], normal, options.color);
    }
    for (const int index : mesh.indices) {
        item.mesh.indices.push_back(static_cast<unsigned int>(index));
    }
    if (options.facets) {
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            for (int e = 0; e < 3; ++e) {
                item.mesh.line_indices.push_back(
                    static_cast<unsigned int>(mesh.indices[t + Idx(e)]));
                item.mesh.line_indices.push_back(
                    static_cast<unsigned int>(mesh.indices[t + Idx((e + 1) % 3)]));
            }
        }
    }
    Finish(&item.mesh);
    return true;
}

bool AddResult(Scene *scene, const fem::AnalysisModel &model,
               const std::vector<Vec3d> &displacement, const fem::StressField &stress,
               const ResultOptions &options, std::string *error) {
    std::vector<double> values;
    fem::Strength strength;
    strength.tensile = options.strength;
    if (!fem::SampleField(model, displacement, stress, options.field, strength, &values, error)) {
        return false;
    }
    scene->items.push_back(Item{});
    Item &item = scene->items.back();
    item.name = options.name.empty() ? fem::ScalarFieldName(options.field) : options.name;
    if (!fem::RenderSurface(model, displacement, values, options.render, &item.mesh, error)) {
        scene->items.pop_back();
        return false;
    }
    scene->NoteField(item.mesh.field_min, item.mesh.field_max,
                     fem::ScalarFieldName(options.field));
    return true;
}

bool AddMode(Scene *scene, const fem::AnalysisModel &model, const fem::ModalResult &modes,
             int mode, double phase, double amplitude_fraction, const fem::RenderOptions &render,
             const std::string &name, std::string *error) {
    if (mode < 0 || mode >= static_cast<int>(modes.shape.size())) {
        if (error != nullptr) *error = "no such mode";
        return false;
    }
    const std::vector<Vec3d> &shape = modes.shape[Idx(mode)];
    if (static_cast<int>(shape.size()) != model.NodeCount()) {
        if (error != nullptr) *error = "the mode shape does not match the model";
        return false;
    }
    // The amplitude from the geometry, not from the eigenvector -- see
    // the note in the header and the longer one in fem_animate.h.
    Vec3d low{0, 0, 0};
    Vec3d high{0, 0, 0};
    if (!model.nodes.empty()) {
        low = model.nodes.front();
        high = low;
        for (const Vec3d &p : model.nodes) {
            low.x = std::min(low.x, p.x);
            low.y = std::min(low.y, p.y);
            low.z = std::min(low.z, p.z);
            high.x = std::max(high.x, p.x);
            high.y = std::max(high.y, p.y);
            high.z = std::max(high.z, p.z);
        }
    }
    const Vec3d span = high - low;
    const double diagonal = std::sqrt(span.Dot(span));
    double peak = 0.0;
    for (const Vec3d &u : shape) peak = std::max(peak, std::sqrt(u.Dot(u)));
    const double scale =
        peak > 0.0 ? amplitude_fraction * (diagonal > 0.0 ? diagonal : 1.0) / peak : 1.0;
    const double at = std::cos(phase) * scale;

    std::vector<Vec3d> displacement(shape.size());
    std::vector<double> magnitude(shape.size(), 0.0);
    for (std::size_t n = 0; n < shape.size(); ++n) {
        displacement[n] = shape[n] * at;
        // THE COLOUR IS THE MODE'S, NOT THE INSTANT'S. Colouring by the
        // displacement *at this phase* looks reasonable and is a trap:
        // a quarter of the way round the cycle the whole shape is at
        // rest, every value is zero, and the colour bar collapses to a
        // range of 2e-17 -- so scrubbing the slider makes the part fade
        // to a single colour and back for no physical reason. The
        // amplitude of the movement at a point is a property of the
        // *mode*, and it is what the colour should mean; the phase is
        // already shown, by the shape moving.
        magnitude[n] = std::sqrt(shape[n].Dot(shape[n])) * scale;
    }
    fem::RenderOptions options = render;
    // The scale is already in the displacement, so the renderer must not
    // apply a second one -- and must not auto-scale, which would
    // normalise every phase to the same size and draw a mode that never
    // moves. fem_movie.h's WriteMovie has the same note for the same
    // reason.
    options.auto_scale = false;
    options.displacement_scale = 1.0;
    scene->items.push_back(Item{});
    Item &item = scene->items.back();
    item.name = name.empty() ? "mode" : name;
    if (!fem::RenderSurface(model, displacement, magnitude, options, &item.mesh, error)) {
        scene->items.pop_back();
        return false;
    }
    scene->NoteField(item.mesh.field_min, item.mesh.field_max, "amplitude");
    return true;
}

bool AddMesh(Scene *scene, const RawMesh &raw, std::string *error) {
    if (raw.positions.size() % 3 != 0) {
        if (error != nullptr) *error = "positions come three numbers to a vertex";
        return false;
    }
    const int vertices = static_cast<int>(raw.positions.size() / 3);
    if (vertices == 0) {
        if (error != nullptr) *error = "a mesh needs at least one vertex";
        return false;
    }
    if (raw.indices.size() % 3 != 0) {
        if (error != nullptr) *error = "indices come three to a triangle";
        return false;
    }
    if (raw.line_indices.size() % 2 != 0) {
        if (error != nullptr) *error = "line indices come two to a segment";
        return false;
    }
    if (!raw.colors.empty() && static_cast<int>(raw.colors.size()) != vertices * 4) {
        if (error != nullptr) *error = "colours come four bytes to a vertex, or none at all";
        return false;
    }
    for (const int index : raw.indices) {
        if (index >= 0 && index < vertices) continue;
        if (error != nullptr) *error = "a triangle names a vertex that is not there";
        return false;
    }
    for (const int index : raw.line_indices) {
        if (index >= 0 && index < vertices) continue;
        if (error != nullptr) *error = "a line names a vertex that is not there";
        return false;
    }

    scene->items.push_back(Item{});
    Item &item = scene->items.back();
    item.name = raw.name.empty() ? "mesh" : raw.name;
    for (int v = 0; v < vertices; ++v) {
        std::array<unsigned char, 4> colour = raw.color;
        if (!raw.colors.empty()) {
            for (int c = 0; c < 4; ++c) colour[Idx(c)] = raw.colors[Idx(v * 4 + c)];
        }
        const Vec3d at{raw.positions[Idx(v * 3)], raw.positions[Idx(v * 3 + 1)],
                       raw.positions[Idx(v * 3 + 2)]};
        PushVertex(&item.mesh, at, Vec3d{0, 0, 1}, colour);
    }
    for (const int index : raw.indices) item.mesh.indices.push_back(static_cast<unsigned int>(index));
    for (const int index : raw.line_indices) {
        item.mesh.line_indices.push_back(static_cast<unsigned int>(index));
    }
    // NORMALS FROM THE TRIANGLES, because a script that hands over
    // positions and indices has not thought about normals and should not
    // have to. Area-weighted, which is the accumulation that makes a
    // smooth surface smooth without any extra input.
    for (std::size_t t = 0; t + 2 < raw.indices.size(); t += 3) {
        const int a = raw.indices[t];
        const int b = raw.indices[t + 1];
        const int c = raw.indices[t + 2];
        const Vec3d pa{raw.positions[Idx(a * 3)], raw.positions[Idx(a * 3 + 1)],
                       raw.positions[Idx(a * 3 + 2)]};
        const Vec3d pb{raw.positions[Idx(b * 3)], raw.positions[Idx(b * 3 + 1)],
                       raw.positions[Idx(b * 3 + 2)]};
        const Vec3d pc{raw.positions[Idx(c * 3)], raw.positions[Idx(c * 3 + 1)],
                       raw.positions[Idx(c * 3 + 2)]};
        const Vec3d normal = (pb - pa).Cross(pc - pa);
        for (const int at : {a, b, c}) {
            item.mesh.normals[Idx(at * 3 + 0)] += F(normal.x);
            item.mesh.normals[Idx(at * 3 + 1)] += F(normal.y);
            item.mesh.normals[Idx(at * 3 + 2)] += F(normal.z);
        }
    }
    if (!raw.indices.empty()) {
        for (int v = 0; v < vertices; ++v) {
            // The seed normal above is still in there; it is dwarfed by
            // any real triangle and only survives for a vertex no
            // triangle uses, which is exactly when it is wanted.
            const float x = item.mesh.normals[Idx(v * 3)];
            const float y = item.mesh.normals[Idx(v * 3 + 1)];
            const float z = item.mesh.normals[Idx(v * 3 + 2)];
            const float length = std::sqrt(x * x + y * y + z * z);
            if (length <= 0.0f) continue;
            item.mesh.normals[Idx(v * 3)] = x / length;
            item.mesh.normals[Idx(v * 3 + 1)] = y / length;
            item.mesh.normals[Idx(v * 3 + 2)] = z / length;
        }
    }
    Finish(&item.mesh);
    return true;
}

void AddLine(Scene *scene, const Vec3d &a, const Vec3d &b,
             const std::array<unsigned char, 4> &color, const std::string &name) {
    fem::RenderMesh *mesh = LineItem(scene, name.empty() ? "line" : name);
    const unsigned int from = PushVertex(mesh, a, Vec3d{0, 0, 1}, color);
    const unsigned int to = PushVertex(mesh, b, Vec3d{0, 0, 1}, color);
    mesh->line_indices.push_back(from);
    mesh->line_indices.push_back(to);
    Finish(mesh);
}

void AddPoint(Scene *scene, const Vec3d &at, double size,
              const std::array<unsigned char, 4> &color, const std::string &name) {
    fem::RenderMesh *mesh = LineItem(scene, name.empty() ? "point" : name);
    const double half = size > 0.0 ? size * 0.5 : 0.5;
    const Vec3d axes[3] = {{half, 0, 0}, {0, half, 0}, {0, 0, half}};
    for (const Vec3d &axis : axes) {
        const unsigned int from = PushVertex(mesh, at - axis, Vec3d{0, 0, 1}, color);
        const unsigned int to = PushVertex(mesh, at + axis, Vec3d{0, 0, 1}, color);
        mesh->line_indices.push_back(from);
        mesh->line_indices.push_back(to);
    }
    Finish(mesh);
}

void AddLabel(Scene *scene, const Vec3d &at, const std::string &text,
              const std::array<unsigned char, 4> &color) {
    Label label;
    label.at = at;
    label.text = text;
    label.color = color;
    scene->labels.push_back(std::move(label));
}

}  // namespace view
