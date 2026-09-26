// Part B.5 on screen: build B-rep solids, tessellate them, and draw the
// result (plans/CAD_FEM_PLAN.md Part B.5).
//
// The numeric checks live in mep-cad-topology-test; what this adds is the
// thing numbers cannot show. A tessellation can have the right volume,
// pass the watertightness test, and still look wrong -- a seam placed on
// the wrong side of a cylinder produces a visible scar, a pole handled
// badly produces a pinched cap, and a face whose normals are inverted
// renders black. Those are all visible at a glance and invisible to a
// volume check.
//
// Usage: mep-gfx-native-brep-smoke <screenshot-out.png>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cad_pcurve.h"
#include "cad_tessellate.h"
#include "cad_topology.h"
#include "gfx/backend_native.h"
#include "gfx/platform.h"
#include "gfx/renderer2d.h"
#include "gfx/renderer3d.h"
#include "gfx/vecmath.h"

namespace {

// One tessellated body, uploaded and placed.
struct Piece {
    gfx::Mesh mesh{};
    gfx::Vector3 offset{};
    gfx::Color color{};
};

bool BuildPiece(cad::Model *model, cad::EntityId body, const cad::Vec3d &offset, gfx::Color color,
                double *out_volume, Piece *out) {
    std::string error;
    if (!cad::BuildAllPCurves(model, {}, &error)) {
        std::fprintf(stderr, "smoke: p-curves failed: %s\n", error.c_str());
        return false;
    }
    cad::TessellationOptions options;
    options.chord_tolerance = 1e-3;
    cad::TessellationMesh tessellation;
    if (!cad::TessellateBody(*model, body, options, &tessellation, &error)) {
        std::fprintf(stderr, "smoke: tessellation failed: %s\n", error.c_str());
        return false;
    }
    if (!tessellation.IsClosed()) {
        std::fprintf(stderr, "smoke: FAILED -- tessellation is not watertight\n");
        return false;
    }
    *out_volume = tessellation.SignedVolume();

    out->mesh.vertexCount = tessellation.VertexCount();
    out->mesh.triangleCount = tessellation.TriangleCount();
    out->mesh.vertices = new float[tessellation.positions.size() * 3];
    out->mesh.normals = new float[tessellation.normals.size() * 3];
    for (std::size_t i = 0; i < tessellation.positions.size(); ++i) {
        out->mesh.vertices[i * 3 + 0] = static_cast<float>(tessellation.positions[i].x);
        out->mesh.vertices[i * 3 + 1] = static_cast<float>(tessellation.positions[i].y);
        out->mesh.vertices[i * 3 + 2] = static_cast<float>(tessellation.positions[i].z);
        out->mesh.normals[i * 3 + 0] = static_cast<float>(tessellation.normals[i].x);
        out->mesh.normals[i * 3 + 1] = static_cast<float>(tessellation.normals[i].y);
        out->mesh.normals[i * 3 + 2] = static_cast<float>(tessellation.normals[i].z);
    }
    out->mesh.indices = new unsigned int[tessellation.indices.size()];
    for (std::size_t i = 0; i < tessellation.indices.size(); ++i) {
        out->mesh.indices[i] = static_cast<unsigned int>(tessellation.indices[i]);
    }
    out->offset = gfx::Vector3{static_cast<float>(offset.x), static_cast<float>(offset.y),
                               static_cast<float>(offset.z)};
    out->color = color;
    gfx::UploadMesh(&out->mesh, false);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <screenshot-out.png>\n", argv[0]);
        return 1;
    }
    gfx::SetBackends(gfx::ToBackends(gfx::CreateNativeBackendSet()));
    gfx::InitWindow(1100, 460, "mep B-rep tessellation smoke test");
    gfx::SetTargetFPS(60);

    std::vector<Piece> pieces;
    int failures = 0;

    struct Spec {
        const char *name;
        double exact;
        cad::Vec3d offset;
        gfx::Color color;
    };
    const Spec specs[4] = {
        {"box", 2.0 * 2.0 * 2.0, {-6.0, 0, 0}, gfx::Color{210, 90, 70, 255}},
        {"cylinder", cad::kPi * 1.0 * 1.0 * 2.4, {-2.0, 0, 0}, gfx::Color{80, 160, 200, 255}},
        {"sphere", 4.0 / 3.0 * cad::kPi * 1.3 * 1.3 * 1.3, {2.0, 0, 0}, gfx::Color{120, 190, 110, 255}},
        // 2*pi^2*R*r^2 -- the minor radius is squared. (Written without
        // the square the first time, which made a correct tessellation
        // look like a 60% error.)
        {"torus", 2.0 * cad::kPi * cad::kPi * 1.0 * 0.4 * 0.4, {6.0, 0, 0}, gfx::Color{200, 170, 80, 255}},
    };
    for (int which = 0; which < 4; ++which) {
        cad::Model model;
        cad::EntityId body = cad::kNoEntity;
        bool built = false;
        switch (which) {
            case 0: built = cad::MakeBox(cad::Vec3d(-1, -1, -1), cad::Vec3d(2, 2, 2), &model, &body); break;
            case 1:
                built = cad::MakeCylinder(cad::Vec3d(0, 0, -1.2), cad::Vec3d(0, 0, 1), 1.0, 2.4, &model, &body);
                break;
            case 2: built = cad::MakeSphere(cad::Vec3d(0, 0, 0), 1.3, &model, &body); break;
            default:
                built = cad::MakeTorus(cad::Vec3d(), cad::Vec3d(0, 0, 1), 1.0, 0.4, &model, &body);
                break;
        }
        if (!built) {
            std::fprintf(stderr, "smoke: FAILED -- could not build %s\n", specs[which].name);
            return 1;
        }
        Piece piece;
        double volume = 0.0;
        if (!BuildPiece(&model, body, specs[which].offset, specs[which].color, &volume, &piece)) return 1;
        const double relative = std::fabs(volume - specs[which].exact) / specs[which].exact;
        std::printf("smoke: %-9s %6d tris, volume %.6f (exact %.6f, err %.2e)\n", specs[which].name,
                    piece.mesh.triangleCount, volume, specs[which].exact, relative);
        // The bodies here are small (radii around 1), so a chord
        // tolerance of 1e-3 is a large fraction of the feature size; the
        // volume threshold is set accordingly rather than aspirationally.
        if (relative > 8e-3) {
            std::fprintf(stderr, "smoke: FAILED -- %s volume is off by %.2e\n", specs[which].name, relative);
            ++failures;
        }
        pieces.push_back(piece);
    }

    gfx::Material material = gfx::LoadMaterialDefault();
    gfx::Camera3D camera{};
    camera.position = {0.5f, -11.0f, 5.5f};
    camera.target = {0.0f, 0.0f, -0.2f};
    camera.up = {0, 0, 1};
    camera.fovy = 42.0f;
    camera.projection = gfx::CameraProjection::Perspective;

    for (int frame = 0; frame < 5 && !gfx::WindowShouldClose(); frame++) {
        gfx::BeginDrawing();
        gfx::ClearBackground(gfx::Color{22, 24, 30, 255});
        gfx::BeginMode3D(camera);
        for (const Piece &piece : pieces) {
            material.maps[gfx::kMaterialMapAlbedo].color = piece.color;
            gfx::DrawMesh(piece.mesh, material,
                          gfx::MatrixTranslate(piece.offset.x, piece.offset.y, piece.offset.z));
        }
        gfx::EndMode3D();
        gfx::EndDrawing();
    }

    const int width = gfx::GetScreenWidth();
    const int height = gfx::GetScreenHeight();
    unsigned char *pixels = gfx::ReadScreenPixels(width, height);
    gfx::Image shot{pixels, width, height, 1, gfx::kPixelFormatR8G8B8A8};
    gfx::ExportImage(shot, argv[1]);

    // Each body must actually be visible, and lit -- a face whose normals
    // came out inverted renders black against a dark background and would
    // pass every numeric check in the suite.
    long drawn = 0;
    long lit = 0;
    for (int i = 0; i < width * height; i++) {
        const unsigned char *p = pixels + static_cast<std::size_t>(i) * 4;
        if (p[0] < 30 && p[1] < 32 && p[2] < 38) continue;
        drawn++;
        if (p[0] > 60 || p[1] > 60 || p[2] > 60) lit++;
    }
    std::free(pixels);
    std::printf("smoke: %ld pixels drawn, %ld of them lit\n", drawn, lit);
    if (drawn < 40000) {
        std::fprintf(stderr, "smoke: FAILED -- too little was drawn (%ld pixels)\n", drawn);
        ++failures;
    }
    if (lit < drawn / 2) {
        std::fprintf(stderr, "smoke: FAILED -- most of what was drawn is unlit, which is what an inverted "
                             "normal looks like\n");
        ++failures;
    }

    for (Piece &piece : pieces) {
        delete[] piece.mesh.vertices;
        delete[] piece.mesh.normals;
        delete[] piece.mesh.indices;
    }
    gfx::CloseWindow();
    if (failures > 0) {
        std::fprintf(stderr, "smoke: FAILED (%d checks)\n", failures);
        return 1;
    }
    std::printf("smoke: OK -- wrote %s\n", argv[1]);
    return 0;
}
