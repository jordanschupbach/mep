// Stage B integration smoke test: exercises the *actual* gfx:: facade
// (not raw GL like native_smoke_main.cpp) against the native backend --
// window creation, 2D primitives, texture upload/draw, and text, all
// through the same gfx::Draw*/gfx::Text* calls main.cpp itself uses.
// Confirms end to end, not just "it compiles": reads back the rendered
// frame and checks it isn't blank, so a silent no-op renderer would fail
// this test instead of passing it.
//
// Takes one optional argument: a path to a TTF file to test text
// rendering with. Skips the text portion (but still checks 2D primitives
// rendered) if omitted or the load fails, so this stays runnable without
// a font file lying around.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "gfx/backend_native.h"
#include "gfx/platform.h"
#include "gfx/renderer2d.h"
#include "gfx/renderer3d.h"
#include "gfx/text.h"
#include "gfx/vecmath.h"

namespace {

std::vector<unsigned char> ReadFile(const char *path) {
    std::vector<unsigned char> data;
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr) return data;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) {
        data.resize(static_cast<size_t>(size));
        size_t read = std::fread(data.data(), 1, static_cast<size_t>(size), f);
        if (read != static_cast<size_t>(size)) data.clear();
    }
    std::fclose(f);
    return data;
}

// True if the RGBA8 buffer has more than one distinct color -- a cheap
// "did anything actually get drawn" check, not a pixel-perfect one.
bool HasVisualContent(const unsigned char *pixels, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    unsigned char r0 = pixels[0], g0 = pixels[1], b0 = pixels[2];
    for (int i = 0; i < width * height; i++) {
        size_t p = static_cast<size_t>(i) * 4;
        if (pixels[p] != r0 || pixels[p + 1] != g0 || pixels[p + 2] != b0) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    gfx::SetBackends(gfx::ToBackends(gfx::CreateNativeBackendSet()));

    gfx::SetWindowResizable();
    gfx::InitWindow(800, 600, "mep gfx native integration smoke test");
    gfx::SetTargetFPS(60);

    gfx::Font font{};
    bool have_font = false;
    if (argc > 1) {
        std::vector<unsigned char> ttf = ReadFile(argv[1]);
        if (!ttf.empty()) {
            font = gfx::LoadFontFromMemory(".ttf", ttf.data(), static_cast<int>(ttf.size()), 32, nullptr, 0);
            have_font = font.texture.id != 0;
            if (!have_font) std::fprintf(stderr, "smoke: font load failed (texture id 0)\n");
        } else {
            std::fprintf(stderr, "smoke: couldn't read font file '%s'\n", argv[1]);
        }
    }

    // A generated (not loaded from a file) image, to exercise
    // LoadTextureFromImage/DrawTexturePro/UnloadTexture too.
    gfx::Image swatch = gfx::GenImageColor(64, 64, gfx::Color{40, 180, 220, 255});
    gfx::Texture2D swatch_tex = gfx::LoadTextureFromImage(swatch);
    gfx::UnloadImage(swatch);

    // Procedurally-generated meshes (GenMeshTorus/GenMeshSphere), to
    // exercise the real GenMesh*/UploadMesh/DrawMesh/LoadMaterialDefault
    // path end to end.
    gfx::Mesh torus_mesh = gfx::GenMeshTorus(1.0f, 0.35f, 24, 16);
    gfx::UploadMesh(&torus_mesh, false);
    gfx::Material torus_material = gfx::LoadMaterialDefault();
    torus_material.maps[gfx::kMaterialMapAlbedo].color = gfx::Color{200, 120, 255, 255};

    gfx::Camera3D camera{};
    camera.position = {4, 3, 6};
    camera.target = {0, 0.5f, 0};
    camera.up = {0, 1, 0};
    camera.fovy = 45.0f;
    camera.projection = gfx::CameraProjection::Perspective;

    for (int frame = 0; frame < 10 && !gfx::WindowShouldClose(); frame++) {
        gfx::BeginDrawing();
        gfx::ClearBackground(gfx::Color{20, 20, 30, 255});
        gfx::DrawRectangle(50, 50, 200, 100, gfx::Color{200, 60, 60, 255});
        gfx::DrawRectangleRounded(gfx::Rectangle{300, 50, 200, 100}, 0.3f, 12, gfx::Color{60, 200, 90, 255});
        gfx::DrawCircle(650, 100, 50, gfx::Color{240, 200, 40, 255});
        gfx::DrawLineEx(gfx::Vector2{50, 200}, gfx::Vector2{750, 220}, 4.0f, gfx::White);
        gfx::DrawTriangle(gfx::Vector2{100, 300}, gfx::Vector2{200, 300}, gfx::Vector2{150, 250}, gfx::Blue);
        gfx::DrawTextureEx(swatch_tex, gfx::Vector2{50, 350}, 0.0f, 1.5f, gfx::White);
        if (have_font) {
            gfx::DrawTextEx(font, "mep gfx native backend", gfx::Vector2{50, 450}, 32.0f, 2.0f, gfx::White);
        }

        gfx::BeginMode3D(camera);
        gfx::DrawGrid(10, 1.0f);
        gfx::DrawCube(gfx::Vector3{-2, 0.5f, 0}, 1.0f, 1.0f, 1.0f, gfx::Red);
        gfx::DrawSphere(gfx::Vector3{0, 0.5f, -2}, 0.6f, gfx::Green);
        gfx::DrawCylinderEx(gfx::Vector3{2, 0, 0}, gfx::Vector3{2, 1.5f, 0}, 0.4f, 0.2f, 16, gfx::Yellow);
        gfx::DrawLine3D(gfx::Vector3{-3, 0, -3}, gfx::Vector3{3, 3, 3}, gfx::White);
        gfx::Matrix torus_transform = gfx::MatrixMultiply(gfx::MatrixRotateXYZ({1.0f, 0.4f, 0}),
                                                           gfx::MatrixTranslate(0, 1.0f, 2.0f));
        gfx::DrawMesh(torus_mesh, torus_material, torus_transform);
        gfx::EndMode3D();

        gfx::EndDrawing();
    }

    // Render-to-texture + LoadImageFromTexture round trip: exercises the
    // FBO-attach + glReadPixels texture-readback path (portable to
    // OpenGL ES/WebGL, unlike glGetTexImage, which this backend used to
    // rely on here -- see backend_native_renderer2d.cpp's own comment).
    gfx::RenderTexture2D rt = gfx::LoadRenderTexture(128, 128);
    gfx::BeginTextureMode(rt);
    gfx::ClearBackground(gfx::Color{10, 10, 10, 255});
    gfx::DrawCircle(64, 64, 40, gfx::Color{255, 120, 40, 255});
    gfx::EndTextureMode();
    gfx::Image rt_image = gfx::LoadImageFromTexture(rt.texture);
    bool rt_ok = HasVisualContent(static_cast<const unsigned char *>(rt_image.data), rt_image.width, rt_image.height);
    std::free(rt_image.data);
    gfx::UnloadRenderTexture(rt);
    if (!rt_ok) std::fprintf(stderr, "smoke: FAILED -- LoadImageFromTexture readback is a single flat color\n");

    int width = gfx::GetScreenWidth();
    int height = gfx::GetScreenHeight();
    unsigned char *pixels = gfx::ReadScreenPixels(width, height);  // already top-down, see its own comment
    bool ok = HasVisualContent(pixels, width, height) && rt_ok;
    if (argc > 2) {
        gfx::Image shot{pixels, width, height, 1, gfx::kPixelFormatR8G8B8A8};
        gfx::ExportImage(shot, argv[2]);
    }
    std::free(pixels);

    gfx::UnloadTexture(swatch_tex);
    gfx::UnloadMesh(torus_mesh);
    if (have_font) gfx::UnloadFont(font);
    gfx::CloseWindow();

    if (!ok) {
        std::fprintf(stderr, "smoke: FAILED -- framebuffer is a single flat color, nothing drew\n");
        return 1;
    }
    std::printf("smoke: OK -- 2D primitives%s rendered real pixel content\n", have_font ? " + text" : "");
    return 0;
}
