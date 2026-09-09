// Stage B8 model-import smoke test: loads a real model file via
// gfx::LoadModel, uploads+draws every mesh it contains, and exports a
// screenshot for visual verification -- the same "read back and check it
// isn't blank" pattern the other gfx smoke tests use, plus a saved PNG
// for a human (or Claude) to actually look at.
//
// Usage: mep-gfx-native-model-smoke <model-file> <screenshot-out.png>

#include <cstdio>
#include <cstdlib>

#include "gfx/backend_native.h"
#include "gfx/platform.h"
#include "gfx/renderer2d.h"
#include "gfx/renderer3d.h"
#include "gfx/vecmath.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <model-file> <screenshot-out.png>\n", argv[0]);
        return 1;
    }

    gfx::SetBackends(gfx::ToBackends(gfx::CreateNativeBackendSet()));
    gfx::SetWindowResizable();
    gfx::InitWindow(800, 600, "mep gfx native model-import smoke test");
    gfx::SetTargetFPS(60);

    gfx::Model model = gfx::LoadModel(argv[1]);
    if (model.meshCount == 0) {
        std::fprintf(stderr, "smoke: FAILED -- LoadModel('%s') returned 0 meshes\n", argv[1]);
        gfx::CloseWindow();
        return 1;
    }
    std::printf("smoke: loaded '%s': %d mesh(es), %d material(s)\n", argv[1], model.meshCount, model.materialCount);
    for (int i = 0; i < model.materialCount; i++) {
        gfx::Color c = model.materials[i].maps[gfx::kMaterialMapAlbedo].color;
        gfx::Texture2D t = model.materials[i].maps[gfx::kMaterialMapAlbedo].texture;
        std::printf("smoke: material[%d]: color=(%d,%d,%d,%d) texture=%s (%dx%d)\n", i, c.r, c.g, c.b, c.a,
                    t.id != 0 ? "yes" : "no", t.width, t.height);
    }

    // Compute a bounding sphere from every mesh's raw vertex data so the
    // camera frames the model regardless of its actual size/units --
    // models this test might be pointed at range from unit cubes to
    // building-scale assets, and (glTF) may come back as many meshes of
    // very different sizes rather than mesh 0 alone representing the
    // whole model.
    gfx::Vector3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (int m = 0; m < model.meshCount; m++) {
        for (int i = 0; i < model.meshes[m].vertexCount; i++) {
            float x = model.meshes[m].vertices[i * 3 + 0];
            float y = model.meshes[m].vertices[i * 3 + 1];
            float z = model.meshes[m].vertices[i * 3 + 2];
            mn.x = std::min(mn.x, x); mn.y = std::min(mn.y, y); mn.z = std::min(mn.z, z);
            mx.x = std::max(mx.x, x); mx.y = std::max(mx.y, y); mx.z = std::max(mx.z, z);
        }
    }
    gfx::Vector3 center = gfx::Vector3Scale(gfx::Vector3Add(mn, mx), 0.5f);
    float radius = gfx::Vector3Distance(mn, mx) * 0.5f;
    if (radius < 1e-4f) radius = 1.0f;

    for (int i = 0; i < model.meshCount; i++) gfx::UploadMesh(&model.meshes[i], false);
    gfx::Material material = gfx::LoadMaterialDefault();

    gfx::Camera3D camera{};
    camera.position = gfx::Vector3Add(center, gfx::Vector3{radius * 1.8f, radius * 1.3f, radius * 1.8f});
    camera.target = center;
    camera.up = {0, 1, 0};
    camera.fovy = 45.0f;
    camera.projection = gfx::CameraProjection::Perspective;

    for (int frame = 0; frame < 5 && !gfx::WindowShouldClose(); frame++) {
        gfx::BeginDrawing();
        gfx::ClearBackground(gfx::Color{25, 25, 35, 255});
        gfx::BeginMode3D(camera);
        gfx::DrawGrid(10, radius * 0.3f > 0 ? radius * 0.3f : 1.0f);
        for (int i = 0; i < model.meshCount; i++) {
            gfx::DrawMesh(model.meshes[i], material, gfx::MatrixIdentity());
        }
        gfx::EndMode3D();
        gfx::EndDrawing();
    }

    int width = gfx::GetScreenWidth();
    int height = gfx::GetScreenHeight();
    unsigned char *pixels = gfx::ReadScreenPixels(width, height);  // already top-down, see its own comment
    gfx::Image shot{pixels, width, height, 1, gfx::kPixelFormatR8G8B8A8};
    gfx::ExportImage(shot, argv[2]);
    std::free(pixels);

    // Not bothering with per-mesh GPU cleanup (UnloadMesh) here -- this
    // process exits immediately after, taking the GL context (and
    // everything allocated against it) with it. gfx::UnloadModel still
    // covers the CPU-side arrays/mesh array LoadModel allocated.
    gfx::UnloadModel(model);
    gfx::CloseWindow();
    std::printf("smoke: OK -- wrote %s\n", argv[2]);
    return 0;
}
