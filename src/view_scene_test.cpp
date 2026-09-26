// Part L.2: the scene, verified.
//
// A scene is a value, so it can be checked the way a value is: by what
// it contains and what that implies. Each property below is one a
// specific, plausible mistake destroys.
//
//   * a box tessellated into a scene encloses the box's volume -- the
//     triangles are there, wound the right way, and indexed correctly;
//   * a scene's bounds are the geometry's bounds, not the unit box a
//     forgotten early-return would leave;
//   * adding a second result widens the shared colour range rather than
//     replacing it, which is what makes one colour bar honest for two
//     items;
//   * a mode drawn at the top of its cycle and at the bottom are mirror
//     images about the undeformed shape, and one drawn at the quarter
//     point coincides with it -- the amplitude is not renormalised per
//     phase, which is the same trap fem_movie.h warns about;
//   * raw geometry is validated rather than trusted: a triangle naming a
//     vertex that is not there is refused, not written past;
//   * and normals invented for a raw mesh point out of the shape they
//     came from.

#include "view_scene.h"

#include "cad_pcurve.h"
#include "cad_topology.h"
#include "fem_elem.h"
#include "fem_movie.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char *what, int line) {
    ++checks;
    if (ok) return;
    std::fflush(stdout);
    std::printf("CHECK FAILED: %s at %s:%d\n", what, __FILE__, line);
    ++failures;
}

#define CHECK(x) Check((x), #x, __LINE__)

using cad::Vec3d;
using fem::AnalysisModel;
using fem::BoundElement;
using fem::ElementShape;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// The volume a closed triangle mesh encloses, by the divergence theorem.
// Positive when it is wound outward, which is half of what this checks.
double EnclosedVolume(const fem::RenderMesh &mesh) {
    double total = 0.0;
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        Vec3d p[3];
        for (int k = 0; k < 3; ++k) {
            const std::size_t at = Idx(static_cast<int>(mesh.indices[t + Idx(k)]));
            p[k] = Vec3d{static_cast<double>(mesh.positions[at * 3]),
                         static_cast<double>(mesh.positions[at * 3 + 1]),
                         static_cast<double>(mesh.positions[at * 3 + 2])};
        }
        total += p[0].Dot(p[1].Cross(p[2])) / 6.0;
    }
    return total;
}

AnalysisModel Bar(double lx, double ly, double lz, int nx, int ny, int nz) {
    AnalysisModel model;
    fem::StudyMaterial material;
    material.youngs_modulus = fem::MaterialCurve::Constant(210e9);
    material.poissons_ratio = fem::MaterialCurve::Constant(0.3);
    material.density = 7850.0;
    model.materials.push_back(material);
    auto at = [&](int i, int j, int k) { return (k * (ny + 1) + j) * (nx + 1) + i; };
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                model.nodes.push_back(Vec3d{lx * i / nx, ly * j / ny, lz * k / nz});
            }
        }
    }
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                BoundElement element;
                element.shape = ElementShape::Hex8;
                element.nodes = {at(i, j, k),         at(i + 1, j, k),
                                 at(i + 1, j + 1, k), at(i, j + 1, k),
                                 at(i, j, k + 1),     at(i + 1, j, k + 1),
                                 at(i + 1, j + 1, k + 1), at(i, j + 1, k + 1)};
                model.elements.push_back(element);
            }
        }
    }
    return model;
}

// A stress field with only the averaged nodal values filled in, which
// is what SampleField reads for a component.
fem::StressField ZeroStress(const AnalysisModel &model) {
    fem::StressField stress;
    stress.nodal.assign(Idx(model.NodeCount()), fem::StressTensor{});
    stress.contributions.assign(Idx(model.NodeCount()), 1);
    stress.discontinuity.assign(Idx(model.NodeCount()), 0.0);
    return stress;
}

// --- A CAD body goes in whole ---------------------------------------------
void APartIsItsGeometry() {
    std::printf("a part\n");
    cad::Model model;
    cad::EntityId body = cad::kNoEntity;
    std::string error;
    CHECK(cad::MakeBox(Vec3d{0, 0, 0}, Vec3d{3, 2, 1}, &model, &body));
    CHECK(cad::BuildAllPCurves(&model, {}, &error));

    view::Scene scene;
    view::PartOptions options;
    options.name = "block";
    const bool added = view::AddPart(&scene, model, body, options, &error);
    if (!added) std::printf("  AddPart: %s\n", error.c_str());
    CHECK(added);
    CHECK(scene.items.size() == 1);
    CHECK(scene.items.front().name == "block");
    // NOT a field: plain geometry is not measuring anything, and a
    // colour bar on it would be decoration.
    CHECK(!scene.has_field);

    const double volume = EnclosedVolume(scene.items.front().mesh);
    std::printf("  a 3 x 2 x 1 block tessellates to %d triangles enclosing %.6f\n",
                scene.TriangleCount(), volume);
    CHECK(std::fabs(volume - 6.0) < 1e-6);

    Vec3d low, high;
    CHECK(scene.Bounds(&low, &high));
    CHECK(std::fabs(low.x) < 1e-9 && std::fabs(low.y) < 1e-9 && std::fabs(low.z) < 1e-9);
    CHECK(std::fabs(high.x - 3.0) < 1e-5 && std::fabs(high.y - 2.0) < 1e-5 &&
          std::fabs(high.z - 1.0) < 1e-5);
    std::printf("  its bounds are (%.1f %.1f %.1f) to (%.1f %.1f %.1f)\n", low.x, low.y, low.z,
                high.x, high.y, high.z);
}

// --- One colour bar for two results ---------------------------------------
void TwoResultsShareARange() {
    std::printf("two results\n");
    const AnalysisModel model = Bar(2.0, 0.4, 0.4, 4, 1, 1);
    const std::vector<Vec3d> still(Idx(model.NodeCount()), Vec3d{0, 0, 0});
    fem::StressField stress = ZeroStress(model);
    // A von Mises that runs 0 to 100 on the first item.
    for (int n = 0; n < model.NodeCount(); ++n) {
        stress.nodal[Idx(n)].s[0] = model.nodes[Idx(n)].x * 50.0;
    }
    view::Scene scene;
    view::ResultOptions options;
    options.field = fem::ScalarField::StressXX;
    options.render.auto_scale = false;
    options.render.displacement_scale = 0.0;
    std::string error;
    CHECK(view::AddResult(&scene, model, still, stress, options, &error));
    CHECK(scene.has_field);
    const double first_low = scene.field_min;
    const double first_high = scene.field_max;
    std::printf("  the first runs %.1f to %.1f\n", first_low, first_high);

    // The second is the same field, three times as large.
    for (int n = 0; n < model.NodeCount(); ++n) {
        stress.nodal[Idx(n)].s[0] = model.nodes[Idx(n)].x * 150.0;
    }
    CHECK(view::AddResult(&scene, model, still, stress, options, &error));
    std::printf("  with the second it runs %.1f to %.1f\n", scene.field_min, scene.field_max);
    CHECK(scene.items.size() == 2);
    CHECK(scene.field_min <= first_low + 1e-9);
    CHECK(scene.field_max > first_high * 2.5);
    // Same field, so the bar can still be labelled.
    CHECK(scene.units != "mixed");
}

// --- A mode's amplitude is not renormalised per phase ----------------------
void AModesPhaseIsItsTime() {
    std::printf("a mode\n");
    const AnalysisModel model = Bar(4.0, 0.4, 0.3, 8, 1, 1);
    fem::ModalResult modes;
    modes.frequency.push_back(20.0);
    modes.eigenvalue.push_back(20.0 * 20.0 * 39.47841760435743);
    modes.shape.emplace_back();
    std::vector<Vec3d> &shape = modes.shape.back();
    shape.assign(Idx(model.NodeCount()), Vec3d{0, 0, 0});
    for (int n = 0; n < model.NodeCount(); ++n) {
        const double x = model.nodes[Idx(n)].x / 4.0;
        shape[Idx(n)].z = x * x;
    }

    auto at_phase = [&](double phase) {
        view::Scene scene;
        std::string error;
        CHECK(view::AddMode(&scene, model, modes, 0, phase, 0.10, fem::RenderOptions{}, "mode",
                            &error));
        return scene;
    };
    const view::Scene top = at_phase(0.0);
    const view::Scene quarter = at_phase(3.14159265358979 * 0.5);
    const view::Scene bottom = at_phase(3.14159265358979);

    // The tip is the last node; its drawn height is what moves.
    auto tip_z = [&](const view::Scene &scene) {
        double highest = -1e30;
        double lowest = 1e30;
        const fem::RenderMesh &mesh = scene.items.front().mesh;
        for (std::size_t v = 0; v + 2 < mesh.positions.size(); v += 3) {
            if (static_cast<double>(mesh.positions[v]) < 3.9) continue;  // the tip end
            highest = std::max(highest, static_cast<double>(mesh.positions[v + 2]));
            lowest = std::min(lowest, static_cast<double>(mesh.positions[v + 2]));
        }
        return 0.5 * (highest + lowest);
    };
    const double a = tip_z(top);
    const double b = tip_z(quarter);
    const double c = tip_z(bottom);
    std::printf("  the tip sits at %+.4f at the top of the cycle, %+.4f a quarter through,"
                " %+.4f at the bottom\n", a, b, c);
    // A quarter of the way round, cos is zero and the mode is at rest:
    // the drawn shape is the undeformed one, whose tip is at 0.15.
    CHECK(std::fabs(b - 0.15) < 1e-6);
    // And the two extremes are equal and opposite about it.
    CHECK(std::fabs((a - b) + (c - b)) < 1e-6);
    CHECK(std::fabs(a - b) > 0.2);
}

// --- Raw geometry is checked, not trusted ---------------------------------
void RawGeometryIsValidated() {
    std::printf("raw geometry\n");
    view::Scene scene;
    std::string error;

    view::RawMesh ragged;
    ragged.positions = {0, 0, 0, 1, 0};
    CHECK(!view::AddMesh(&scene, ragged, &error));
    std::printf("  a ragged position array: %s\n", error.c_str());

    view::RawMesh dangling;
    dangling.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    dangling.indices = {0, 1, 7};
    CHECK(!view::AddMesh(&scene, dangling, &error));
    std::printf("  a triangle naming vertex 7 of 3: %s\n", error.c_str());

    view::RawMesh wrong_colors;
    wrong_colors.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    wrong_colors.indices = {0, 1, 2};
    wrong_colors.colors = {255, 0, 0};
    CHECK(!view::AddMesh(&scene, wrong_colors, &error));
    std::printf("  three colour bytes for three vertices: %s\n", error.c_str());

    // Nothing was added by any of the three.
    CHECK(scene.items.empty());

    // A good one, with normals invented for it.
    view::RawMesh fine;
    fine.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    fine.indices = {0, 1, 2};
    fine.name = "triangle";
    CHECK(view::AddMesh(&scene, fine, &error));
    CHECK(scene.items.size() == 1);
    const fem::RenderMesh &mesh = scene.items.front().mesh;
    CHECK(mesh.normals.size() == 9);
    // A triangle in the z = 0 plane wound anticlockwise has its normal
    // along +z, at every one of its vertices.
    for (int v = 0; v < 3; ++v) {
        CHECK(std::fabs(static_cast<double>(mesh.normals[Idx(v * 3 + 2)]) - 1.0) < 1e-5);
    }
    std::printf("  a good triangle gets a unit normal along +z at all three corners\n");
}

// --- Annotation ------------------------------------------------------------
void AnnotationIsGeometryToo() {
    std::printf("annotation\n");
    view::Scene scene;
    view::AddLine(&scene, Vec3d{0, 0, 0}, Vec3d{1, 0, 0}, {255, 60, 60, 255}, "x axis");
    view::AddPoint(&scene, Vec3d{1, 0, 0}, 0.2, {255, 255, 0, 255}, "probe");
    view::AddLabel(&scene, Vec3d{1, 0, 0}, "PROBE A", {235, 235, 240, 255});
    CHECK(scene.items.size() == 2);
    CHECK(scene.labels.size() == 1);
    // A line and a cross are lines and no triangles, so they never get a
    // face drawn or a normal used.
    CHECK(scene.TriangleCount() == 0);
    CHECK(scene.items[0].mesh.line_indices.size() == 2);
    CHECK(scene.items[1].mesh.line_indices.size() == 6);
    Vec3d low, high;
    CHECK(scene.Bounds(&low, &high));
    std::printf("  a line, a cross and a label span (%.1f %.1f %.1f) to (%.1f %.1f %.1f)\n",
                low.x, low.y, low.z, high.x, high.y, high.z);
    // A SCENE'S POSITIONS ARE FLOATS, because that is what the renderer
    // takes, so its bounds come back at float precision and not at
    // double. Worth asserting at the right tolerance rather than at a
    // tighter one that would fail for a reason that is not a fault.
    CHECK(std::fabs(high.x - 1.1) < 1e-6);

    // And an empty scene says it is empty rather than claiming a box.
    view::Scene nothing;
    CHECK(nothing.Empty());
    CHECK(!nothing.Bounds(&low, &high));

    // THE CAPTION SURVIVES A CLEAR and everything derived from the items
    // does not. A caption is what the viewer is called -- set once, at
    // the top of a script -- and a frame callback gets a cleared scene
    // every time it runs, so clearing the title with the contents made
    // it vanish the first time anybody touched the slider.
    scene.caption = "A TITLE";
    scene.NoteField(0.0, 100.0, "von Mises");
    scene.Clear();
    CHECK(scene.caption == "A TITLE");
    CHECK(scene.items.empty());
    CHECK(scene.labels.empty());
    CHECK(!scene.has_field);
    CHECK(scene.units.empty());
    std::printf("  a clear keeps the caption and drops everything the items decided\n");
}

// --- It renders -----------------------------------------------------------
void ASceneRenders() {
    std::printf("rendering a scene\n");
    cad::Model model;
    cad::EntityId body = cad::kNoEntity;
    std::string error;
    CHECK(cad::MakeBox(Vec3d{0, 0, 0}, Vec3d{2, 2, 2}, &model, &body));
    CHECK(cad::BuildAllPCurves(&model, {}, &error));
    view::Scene scene;
    CHECK(view::AddPart(&scene, model, body, view::PartOptions{}, &error));
    view::AddLabel(&scene, Vec3d{1, 1, 2}, "TOP", {255, 240, 120, 255});

    std::vector<const fem::RenderMesh *> meshes;
    for (const view::Item &item : scene.items) meshes.push_back(&item.mesh);
    std::vector<fem::Annotation> labels;
    for (const view::Label &label : scene.labels) {
        labels.push_back(fem::Annotation{label.at, label.text,
                                         {label.color[0], label.color[1], label.color[2]}});
    }

    Vec3d low, high;
    CHECK(scene.Bounds(&low, &high));
    fem::MovieView view;
    view.low = low;
    view.high = high;
    view.centre = (low + high) * 0.5;
    view.radius = 0.5 * std::sqrt((high - low).Dot(high - low));

    fem::MovieOptions options;
    options.width = 320;
    options.height = 240;
    options.supersample = 1;
    options.legend = false;
    std::vector<unsigned char> pixels;
    CHECK(fem::RenderScene(meshes, labels, view, options, 0.0, 0.0, 1.0, &pixels, &error));
    CHECK(pixels.size() == Idx(options.width * options.height * 4));

    int drawn = 0;
    int label_lit = 0;
    for (int i = 0; i < options.width * options.height; ++i) {
        const int r = pixels[Idx(i * 4)];
        const int g = pixels[Idx(i * 4 + 1)];
        const int b = pixels[Idx(i * 4 + 2)];
        if (std::abs(r - 18) > 6 || std::abs(g - 20) > 6 || std::abs(b - 24) > 6) ++drawn;
        // The label's own yellow, which nothing else in this picture is.
        if (r > 200 && g > 190 && b < 160) ++label_lit;
    }
    std::printf("  %d of %d pixels are drawn, %d of them the label's yellow\n", drawn,
                options.width * options.height, label_lit);
    CHECK(drawn > 2000);
    CHECK(label_lit > 20);
}

}  // namespace

int main() {
    APartIsItsGeometry();
    TwoResultsShareARange();
    AModesPhaseIsItsTime();
    RawGeometryIsValidated();
    AnnotationIsGeometryToo();
    ASceneRenders();

    if (failures != 0) {
        std::printf("view_scene_test FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("view_scene_test passed (%d checks)\n", checks);
    return 0;
}
