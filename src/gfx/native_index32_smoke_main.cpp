// Smoke test for 32-bit mesh indices (plans/CAD_FEM_PLAN.md Part 0.4).
//
// gfx::Mesh::indices was `unsigned short`, capping an indexed mesh at
// 65,536 vertices. Every importer in this tree worked around that by
// giving up on indexing entirely and uploading flat triangle lists, so
// nothing in the codebase ever exercised a large *indexed* mesh -- which
// means widening the type could have broken the GL path with no existing
// test noticing. This is that test.
//
// It builds one indexed grid mesh with well over 65,536 vertices, colors
// each vertex by whether its index is below or above that boundary, draws
// it face-on filling the viewport, and reads the framebuffer back. Under
// the old 16-bit type the high half's indices would have wrapped around
// to low values: the top half of the grid would have referenced vertices
// from the bottom half, producing scrambled geometry and, decisively, the
// wrong color in the wrong half of the screen. So the pixel check below
// is not a "did anything draw" check -- it is specifically sensitive to
// the failure widening the type was meant to fix.
//
// Usage: mep-gfx-native-index32-smoke <screenshot-out.png>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "gfx/backend_native.h"
#include "gfx/platform.h"
#include "gfx/renderer2d.h"
#include "gfx/renderer3d.h"
#include "gfx/vecmath.h"

namespace {

constexpr int kIndexBoundary = 65536;  // what the old unsigned short could hold
// 300x300 = 90,000 vertices: comfortably past the boundary, while still
// building and uploading in a few milliseconds.
constexpr int kGridSize = 300;

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <screenshot-out.png>\n", argv[0]);
        return 1;
    }

    gfx::SetBackends(gfx::ToBackends(gfx::CreateNativeBackendSet()));
    gfx::InitWindow(800, 600, "mep gfx 32-bit index smoke test");
    gfx::SetTargetFPS(60);

    // A flat grid in the XY plane spanning [-1,1]. Row-major, so vertex
    // index = row * kGridSize + col, and the index boundary falls at a
    // known row -- which is what makes the color split a horizontal line
    // at a predictable height.
    const int vertex_count = kGridSize * kGridSize;
    const int boundary_row = kIndexBoundary / kGridSize;
    std::vector<float> positions(static_cast<size_t>(vertex_count) * 3, 0.0f);
    std::vector<unsigned char> colors(static_cast<size_t>(vertex_count) * 4, 255);
    for (int row = 0; row < kGridSize; row++) {
        for (int col = 0; col < kGridSize; col++) {
            const int v = row * kGridSize + col;
            const float u = static_cast<float>(col) / static_cast<float>(kGridSize - 1);
            const float w = static_cast<float>(row) / static_cast<float>(kGridSize - 1);
            positions[static_cast<size_t>(v) * 3 + 0] = u * 2.0f - 1.0f;
            positions[static_cast<size_t>(v) * 3 + 1] = w * 2.0f - 1.0f;
            positions[static_cast<size_t>(v) * 3 + 2] = 0.0f;
            // Red below the old 16-bit ceiling, green above it.
            const bool high = v >= kIndexBoundary;
            colors[static_cast<size_t>(v) * 4 + 0] = high ? 0 : 255;
            colors[static_cast<size_t>(v) * 4 + 1] = high ? 255 : 0;
            colors[static_cast<size_t>(v) * 4 + 2] = 0;
            colors[static_cast<size_t>(v) * 4 + 3] = 255;
        }
    }

    std::vector<unsigned int> indices;
    indices.reserve(static_cast<size_t>(kGridSize - 1) * static_cast<size_t>(kGridSize - 1) * 6);
    for (int row = 0; row + 1 < kGridSize; row++) {
        for (int col = 0; col + 1 < kGridSize; col++) {
            const unsigned int v00 = static_cast<unsigned int>(row * kGridSize + col);
            const unsigned int v10 = v00 + 1;
            const unsigned int v01 = v00 + static_cast<unsigned int>(kGridSize);
            const unsigned int v11 = v01 + 1;
            indices.push_back(v00);
            indices.push_back(v10);
            indices.push_back(v11);
            indices.push_back(v00);
            indices.push_back(v11);
            indices.push_back(v01);
        }
    }

    gfx::Mesh mesh{};
    mesh.vertexCount = vertex_count;
    mesh.triangleCount = static_cast<int>(indices.size() / 3);
    mesh.vertices = new float[positions.size()];
    std::copy(positions.begin(), positions.end(), mesh.vertices);
    mesh.colors = new unsigned char[colors.size()];
    std::copy(colors.begin(), colors.end(), mesh.colors);
    mesh.indices = new unsigned int[indices.size()];
    std::copy(indices.begin(), indices.end(), mesh.indices);

    std::printf("smoke: %d vertices (%d past the old 16-bit ceiling), %d triangles\n", mesh.vertexCount,
                mesh.vertexCount - kIndexBoundary, mesh.triangleCount);
    if (mesh.vertexCount <= kIndexBoundary) {
        std::fprintf(stderr, "smoke: FAILED -- the grid must exceed %d vertices to test anything\n", kIndexBoundary);
        return 1;
    }

    gfx::UploadMesh(&mesh, false);
    gfx::Material material = gfx::LoadMaterialDefault();
    // Unlit: the check below is about which vertex color reached which
    // pixel, and shading would modulate both away from their exact values.
    gfx::SetUnlitMode(true);

    gfx::Camera3D camera{};
    camera.position = {0.0f, 0.0f, 2.6f};
    camera.target = {0.0f, 0.0f, 0.0f};
    camera.up = {0, 1, 0};
    camera.fovy = 45.0f;
    camera.projection = gfx::CameraProjection::Perspective;

    for (int frame = 0; frame < 5 && !gfx::WindowShouldClose(); frame++) {
        gfx::BeginDrawing();
        gfx::ClearBackground(gfx::Color{0, 0, 40, 255});
        gfx::BeginMode3D(camera);
        gfx::DrawMesh(mesh, material, gfx::MatrixIdentity());
        gfx::EndMode3D();
        gfx::EndDrawing();
    }

    const int width = gfx::GetScreenWidth();
    const int height = gfx::GetScreenHeight();
    unsigned char *pixels = gfx::ReadScreenPixels(width, height);  // top-down
    gfx::Image shot{pixels, width, height, 1, gfx::kPixelFormatR8G8B8A8};
    gfx::ExportImage(shot, argv[1]);

    // The grid's +Y is up in world space and ReadScreenPixels hands back
    // rows top-down, so the high-index (green) rows land in the *upper*
    // part of the image and the low-index (red) rows in the lower part.
    long red_above = 0, green_above = 0, red_below = 0, green_below = 0;
    const float boundary_fraction = static_cast<float>(boundary_row) / static_cast<float>(kGridSize - 1);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const size_t p = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
            const unsigned char r = pixels[p + 0];
            const unsigned char g = pixels[p + 1];
            const bool is_red = r > 150 && g < 100;
            const bool is_green = g > 150 && r < 100;
            // Only count pixels well clear of the colour-transition band,
            // so interpolation across the boundary row is not miscounted.
            const float row_fraction = 1.0f - static_cast<float>(y) / static_cast<float>(height - 1);
            if (row_fraction > boundary_fraction + 0.12f) {
                if (is_red) red_above++;
                if (is_green) green_above++;
            } else if (row_fraction < boundary_fraction - 0.12f) {
                if (is_red) red_below++;
                if (is_green) green_below++;
            }
        }
    }
    std::free(pixels);

    std::printf("smoke: above the boundary: %ld green, %ld red; below: %ld red, %ld green\n", green_above, red_above,
                red_below, green_below);

    int failures = 0;
    if (green_above < 10000) {
        std::fprintf(stderr, "smoke: FAILED -- too few green pixels above the boundary (%ld)\n", green_above);
        failures++;
    }
    if (red_below < 10000) {
        std::fprintf(stderr, "smoke: FAILED -- too few red pixels below the boundary (%ld)\n", red_below);
        failures++;
    }
    // The decisive assertions: a 16-bit wraparound puts low-index (red)
    // geometry where the high-index (green) half belongs, and vice versa.
    if (red_above > green_above / 20) {
        std::fprintf(stderr, "smoke: FAILED -- red bleeding into the high-index half (%ld red vs %ld green)\n",
                     red_above, green_above);
        failures++;
    }
    if (green_below > red_below / 20) {
        std::fprintf(stderr, "smoke: FAILED -- green bleeding into the low-index half (%ld green vs %ld red)\n",
                     green_below, red_below);
        failures++;
    }

    delete[] mesh.vertices;
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
