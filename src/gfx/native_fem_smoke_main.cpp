// The Part 0.6 vertical slice, end to end and on screen: generate a box
// mesh, solve a cantilever under a tip load, colour the deformed surface
// by von Mises stress, draw it, and read the framebuffer back to confirm
// the picture is actually a stress plot rather than a blank pane.
//
// This is the whole point of Part 0.6. Every layer underneath it --
// cad_math's quadrature, num_sparse's factorization, the widened index
// type, the element formulation, the surface extraction and the colour
// map -- is exercised by one command, so a regression anywhere in the
// stack shows up here with a picture attached.
//
// Usage: mep-gfx-native-fem-smoke <screenshot-out.png>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "fem_model.h"
#include "fem_solve.h"
#include "fem_viz.h"
#include "gfx/backend_native.h"
#include "gfx/platform.h"
#include "gfx/renderer2d.h"
#include "gfx/renderer3d.h"
#include "gfx/vecmath.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <screenshot-out.png>\n", argv[0]);
        return 1;
    }

    // --- The problem: a steel cantilever, 2 m x 0.2 m x 0.2 m, with a
    // 5 kN downward load spread over its free end.
    fem::Material steel;
    steel.name = "steel";
    steel.youngs_modulus = 210e9;
    steel.poissons_ratio = 0.3;
    const std::array<int, 3> divisions = {40, 4, 4};
    fem::Model model =
        fem::MakeBoxMesh(cad::Vec3d{0, 0, 0}, cad::Vec3d{2.0, 0.2, 0.2}, divisions, steel);
    for (int n : fem::BoxMeshFaceNodes(divisions, 0, false)) {
        fem::Constraint c;
        c.node = n;
        c.fixed[0] = c.fixed[1] = c.fixed[2] = true;
        model.constraints.push_back(c);
    }
    const std::vector<int> tip = fem::BoxMeshFaceNodes(divisions, 0, true);
    for (int n : tip) {
        fem::NodalLoad l;
        l.node = n;
        l.force = cad::Vec3d{0.0, 0.0, -5000.0 / static_cast<double>(tip.size())};
        model.loads.push_back(l);
    }
    std::printf("smoke: %d nodes, %d elements, %d dofs\n", model.NodeCount(), model.ElementCount(),
                model.DofCount());

    const fem::Result result = fem::Solve(model);
    if (!result.ok) {
        std::fprintf(stderr, "smoke: FAILED -- solve: %s\n", result.error.c_str());
        return 1;
    }
    std::printf("smoke: max displacement %.4e m, max von Mises %.4e Pa, equilibrium residual %.2e\n",
                result.max_displacement_magnitude, result.max_von_mises, result.equilibrium_residual);
    if (result.equilibrium_residual > 1e-9) {
        std::fprintf(stderr, "smoke: FAILED -- reactions do not balance the applied load\n");
        return 1;
    }

    fem::ResultMeshOptions view;
    view.field = fem::ResultField::VonMises;
    view.color_map = fem::ColorMap::Viridis;
    const fem::ResultMesh contour = fem::BuildResultMesh(model, result, view);
    std::printf("smoke: surface %d vertices, %d triangles; field %.3e..%.3e Pa; displacement x%.1f\n",
                contour.surface_vertex_count, contour.surface_triangle_count, contour.field_min,
                contour.field_max, contour.displacement_scale_used);
    if (contour.surface_triangle_count == 0) {
        std::fprintf(stderr, "smoke: FAILED -- surface extraction produced nothing\n");
        return 1;
    }
    // The surface of a solid mesh must be much smaller than the solid: if
    // interior faces were not culled this would be several times larger.
    const int all_faces = model.ElementCount() * 6;
    if (contour.surface_triangle_count / 2 >= all_faces) {
        std::fprintf(stderr, "smoke: FAILED -- interior faces were not culled\n");
        return 1;
    }

    gfx::SetBackends(gfx::ToBackends(gfx::CreateNativeBackendSet()));
    gfx::InitWindow(1000, 500, "mep FEM result smoke test");
    gfx::SetTargetFPS(60);

    gfx::Mesh mesh{};
    mesh.vertexCount = contour.surface_vertex_count;
    mesh.triangleCount = contour.surface_triangle_count;
    mesh.vertices = new float[contour.positions.size()];
    std::copy(contour.positions.begin(), contour.positions.end(), mesh.vertices);
    mesh.normals = new float[contour.normals.size()];
    std::copy(contour.normals.begin(), contour.normals.end(), mesh.normals);
    mesh.colors = new unsigned char[contour.colors.size()];
    std::copy(contour.colors.begin(), contour.colors.end(), mesh.colors);
    mesh.indices = new unsigned int[contour.indices.size()];
    std::copy(contour.indices.begin(), contour.indices.end(), mesh.indices);
    gfx::UploadMesh(&mesh, false);

    gfx::Material material = gfx::LoadMaterialDefault();
    // Unlit so the colours read as the field values they encode; shading
    // would modulate them by geometry, which is exactly the confusion a
    // contour plot exists to avoid.
    gfx::SetUnlitMode(true);

    gfx::Camera3D camera{};
    camera.position = {1.0f, -3.2f, 1.1f};
    camera.target = {1.0f, 0.1f, -0.05f};
    camera.up = {0, 0, 1};
    camera.fovy = 35.0f;
    camera.projection = gfx::CameraProjection::Perspective;

    for (int frame = 0; frame < 5 && !gfx::WindowShouldClose(); frame++) {
        gfx::BeginDrawing();
        gfx::ClearBackground(gfx::Color{18, 18, 24, 255});
        gfx::BeginMode3D(camera);
        gfx::DrawMesh(mesh, material, gfx::MatrixIdentity());
        gfx::EndMode3D();
        gfx::EndDrawing();
    }

    const int width = gfx::GetScreenWidth();
    const int height = gfx::GetScreenHeight();
    unsigned char *pixels = gfx::ReadScreenPixels(width, height);
    gfx::Image shot{pixels, width, height, 1, gfx::kPixelFormatR8G8B8A8};
    gfx::ExportImage(shot, argv[1]);

    // Verify the picture is a *contour* plot: a stress field that ranges
    // from near zero at the tip to its maximum at the root must produce
    // pixels from both ends of the colour map. A blank pane, a
    // single-colour pane (the field collapsed), or a failure to upload
    // the vertex colours all fail this.
    unsigned char low_rgba[4];
    unsigned char high_rgba[4];
    fem::MapColor(view.color_map, 0.0, low_rgba);
    fem::MapColor(view.color_map, 1.0, high_rgba);
    long near_low = 0, near_high = 0, background = 0;
    for (int i = 0; i < width * height; i++) {
        const unsigned char *p = pixels + static_cast<std::size_t>(i) * 4;
        if (p[0] < 30 && p[1] < 30 && p[2] < 40) {
            background++;
            continue;
        }
        auto distance_to = [p](const unsigned char *c) {
            const int dr = static_cast<int>(p[0]) - static_cast<int>(c[0]);
            const int dg = static_cast<int>(p[1]) - static_cast<int>(c[1]);
            const int db = static_cast<int>(p[2]) - static_cast<int>(c[2]);
            return dr * dr + dg * dg + db * db;
        };
        if (distance_to(low_rgba) < 3000) near_low++;
        if (distance_to(high_rgba) < 3000) near_high++;
    }
    std::free(pixels);
    const long drawn = static_cast<long>(width) * height - background;
    std::printf("smoke: %ld pixels drawn, %ld near the low end of the map, %ld near the high end\n", drawn,
                near_low, near_high);

    int failures = 0;
    if (drawn < 20000) {
        std::fprintf(stderr, "smoke: FAILED -- almost nothing was drawn (%ld pixels)\n", drawn);
        failures++;
    }
    if (near_low < 200) {
        std::fprintf(stderr, "smoke: FAILED -- no low-stress region in the image (%ld pixels)\n", near_low);
        failures++;
    }
    if (near_high < 50) {
        std::fprintf(stderr, "smoke: FAILED -- no high-stress region in the image (%ld pixels)\n", near_high);
        failures++;
    }

    delete[] mesh.vertices;
    delete[] mesh.normals;
    delete[] mesh.colors;
    delete[] mesh.indices;
    gfx::CloseWindow();
    if (failures > 0) {
        std::fprintf(stderr, "smoke: FAILED (%d checks)\n", failures);
        return 1;
    }
    std::printf("smoke: OK -- wrote %s\n", argv[1]);
    return 0;
}
