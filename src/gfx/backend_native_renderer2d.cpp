// NativeRenderer2DBackend: the in-house 2D rendering engine for Stage B's
// native backend. A single textured-quad shader handles everything --
// solid fills (a 1x1 white texture stands in for "no texture", the same
// trick raylib's own rlgl uses internally), real image/photo textures,
// and font glyphs. Glyphs work through this same shader/path because
// main.cpp's DrawLineFast (ported mechanically in Stage A) draws glyphs
// via a plain gfx::DrawTexturePro call, with no separate glyph-drawing
// entry point of its own -- so backend_native_text.cpp's font atlas is
// baked as white RGB + coverage alpha (see its own comment) specifically
// so sampling it through this ordinary "texture.rgba * tint" shader
// already produces correctly tinted, anti-aliased glyphs.
//
// Deliberately immediate, not batched: every Draw* call is its own
// glBufferData+glDrawArrays. Simpler and more obviously correct than a
// batching system, at the cost of a much higher draw-call count than
// raylib's own internal batcher -- correctness first, per PLAN's own
// framing of this as "the most mechanically well-understood" chunk of
// Stage B. Worth revisiting once this backend is actually swapped in and
// real editor responsiveness can be measured (DrawLineFast alone issues
// one glyph draw per on-screen character).

#include "gfx/backend_native_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gfx/gl_shader.h"
#include "gfx/vecmath.h"
#include "image_codec.h"
#include "png_codec.h"

namespace gfx {

namespace {

struct Vtx {
    float x, y, u, v, r, g, b, a;
};

Vtx MakeVtx(float x, float y, float u, float v, gfx::Color c) {
    return {x, y, u, v, static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
            static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f};
}

void AppendSolidQuad(std::vector<Vtx> &out, float x, float y, float w, float h, gfx::Color c) {
    out.push_back(MakeVtx(x, y, 0, 0, c));
    out.push_back(MakeVtx(x + w, y, 0, 0, c));
    out.push_back(MakeVtx(x, y + h, 0, 0, c));
    out.push_back(MakeVtx(x + w, y, 0, 0, c));
    out.push_back(MakeVtx(x + w, y + h, 0, 0, c));
    out.push_back(MakeVtx(x, y + h, 0, 0, c));
}

// Non-mitered thick polyline: one independent quad per segment. Corners
// show a small gap/overlap instead of a true miter join -- an accepted
// simplification (see this file's own top comment); good enough for the
// UI-chrome outlines this backs (panel borders, rounded-rect outlines).
void AppendThickPolyline(std::vector<Vtx> &out, const gfx::Vector2 *pts, int count, float thick, gfx::Color c,
                          bool closed) {
    if (count < 2) return;
    int segs = closed ? count : count - 1;
    for (int i = 0; i < segs; i++) {
        gfx::Vector2 a = pts[i];
        gfx::Vector2 b = pts[(i + 1) % count];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) continue;
        float nx = -dy / len * thick * 0.5f;
        float ny = dx / len * thick * 0.5f;
        gfx::Vector2 p0{a.x + nx, a.y + ny}, p1{b.x + nx, b.y + ny}, p2{b.x - nx, b.y - ny}, p3{a.x - nx, a.y - ny};
        out.push_back(MakeVtx(p0.x, p0.y, 0, 0, c));
        out.push_back(MakeVtx(p1.x, p1.y, 0, 0, c));
        out.push_back(MakeVtx(p2.x, p2.y, 0, 0, c));
        out.push_back(MakeVtx(p0.x, p0.y, 0, 0, c));
        out.push_back(MakeVtx(p2.x, p2.y, 0, 0, c));
        out.push_back(MakeVtx(p3.x, p3.y, 0, 0, c));
    }
}

// Rounded-rectangle boundary (corner arcs + implied straight edges, in
// winding order), used by DrawRectangleRounded/RoundedLines/RoundedLinesEx.
// `roundness` is a 0..1 fraction of the shorter side, matching raylib's
// own convention.
std::vector<gfx::Vector2> RoundedRectBoundary(gfx::Rectangle rec, float roundness, int segments) {
    float radius = roundness * std::min(rec.width, rec.height) * 0.5f;
    segments = std::max(segments, 2);
    if (radius <= 0.01f) {
        return {{rec.x, rec.y},
                {rec.x + rec.width, rec.y},
                {rec.x + rec.width, rec.y + rec.height},
                {rec.x, rec.y + rec.height}};
    }
    std::vector<gfx::Vector2> pts;
    auto arc = [&](float cx, float cy, float start_deg, float end_deg) {
        for (int i = 0; i <= segments; i++) {
            float t = start_deg + (end_deg - start_deg) * static_cast<float>(i) / static_cast<float>(segments);
            float rad = t * gfx::kDeg2Rad;
            pts.push_back({cx + radius * std::cos(rad), cy + radius * std::sin(rad)});
        }
    };
    float x = rec.x, y = rec.y, w = rec.width, h = rec.height;
    arc(x + radius, y + radius, 180, 270);
    arc(x + w - radius, y + radius, 270, 360);
    arc(x + w - radius, y + h - radius, 0, 90);
    arc(x + radius, y + h - radius, 90, 180);
    return pts;
}

int CircleSegments(float radius) { return std::clamp(static_cast<int>(radius), 12, 64); }

// No #version line here (or in kSolidFragmentSrc below) -- gl_shader.cpp's
// CompileShaderStage prepends the right one for the target platform.
const char *kSolidVertexSrc = R"GLSL(
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
uniform mat4 uProj;
out vec2 vUV;
out vec4 vColor;
void main() {
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
}
)GLSL";

const char *kSolidFragmentSrc = R"GLSL(
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 FragColor;
void main() {
    FragColor = texture(uTex, vUV) * vColor;
}
)GLSL";

}  // namespace

struct NativeRenderer2DBackend::Impl {
    NativeContext *ctx = nullptr;

    gl::GLuint solid_program = 0;
    gl::GLint solid_proj_loc = -1;

    gl::GLuint vao = 0, vbo = 0;
    gl::GLuint white_texture = 0;

    float proj[16] = {};
    int target_width = 0, target_height = 0;
    gl::GLuint current_fbo = 0;

    // One entry per LoadRenderTexture, so UnloadRenderTexture/EndTextureMode
    // know what to tear down / restore. Keyed by the fbo id stored in the
    // gfx::RenderTexture2D the caller holds.
    struct TargetInfo {
        gl::GLuint fbo = 0;
        gl::GLuint color_tex = 0;
        gl::GLuint depth_rb = 0;
        int width = 0, height = 0;
    };
    std::vector<TargetInfo> targets;

    void UpdateProjection(int w, int h) {
        target_width = w;
        target_height = h;
        float ortho[16] = {2.0f / static_cast<float>(w), 0, 0, 0, 0, -2.0f / static_cast<float>(h), 0, 0,
                            0,                             0, -1, 0, -1, 1,                          0, 1};
        std::memcpy(proj, ortho, sizeof(proj));
    }

    void Flush(gl::GLuint program, gl::GLint proj_loc, gl::GLuint texture, const Vtx *verts, int count,
               gl::GLenum mode) {
        if (count <= 0) return;
        gl::UseProgram(program);
        gl::UniformMatrix4fv(proj_loc, 1, gl::GL_FALSE_, proj);
        gl::ActiveTexture(gl::GL_TEXTURE0);
        gl::BindTexture(gl::GL_TEXTURE_2D, texture);
        gl::BindVertexArray(vao);
        gl::BindBuffer(gl::GL_ARRAY_BUFFER, vbo);
        gl::BufferData(gl::GL_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(sizeof(Vtx)) * count, verts,
                        gl::GL_STREAM_DRAW);
        gl::DrawArrays(mode, 0, count);
    }

    void FlushSolid(const std::vector<Vtx> &verts, gl::GLenum mode = gl::GL_TRIANGLES) {
        Flush(solid_program, solid_proj_loc, white_texture, verts.data(), static_cast<int>(verts.size()), mode);
    }

    void FlushTextured(gl::GLuint texture, const std::vector<Vtx> &verts, gl::GLenum mode = gl::GL_TRIANGLES) {
        Flush(solid_program, solid_proj_loc, texture, verts.data(), static_cast<int>(verts.size()), mode);
    }
};

NativeRenderer2DBackend::NativeRenderer2DBackend(NativeContext *ctx) : impl_(new Impl()) {
    impl_->ctx = ctx;
}

NativeRenderer2DBackend::~NativeRenderer2DBackend() {
    delete impl_;
}

// Lazily builds shaders/VAO/white-texture on first real use (BeginDrawing,
// called once InitWindow has already made a GL context current) rather
// than in the constructor, which runs before InitWindow.
void NativeRenderer2DBackend::EnsureInit() {
    if (impl_->solid_program != 0) return;
    impl_->solid_program = gl::BuildProgram(kSolidVertexSrc, kSolidFragmentSrc);
    impl_->solid_proj_loc = gl::GetUniformLocation(impl_->solid_program, "uProj");

    gl::GenVertexArrays(1, &impl_->vao);
    gl::BindVertexArray(impl_->vao);
    gl::GenBuffers(1, &impl_->vbo);
    gl::BindBuffer(gl::GL_ARRAY_BUFFER, impl_->vbo);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 2, gl::GL_FLOAT, gl::GL_FALSE_, sizeof(Vtx), nullptr);
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 2, gl::GL_FLOAT, gl::GL_FALSE_, sizeof(Vtx),
                             reinterpret_cast<const void *>(sizeof(float) * 2));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 4, gl::GL_FLOAT, gl::GL_FALSE_, sizeof(Vtx),
                             reinterpret_cast<const void *>(sizeof(float) * 4));

    unsigned char white_pixels[4] = {255, 255, 255, 255};
    gl::GenTextures(1, &impl_->white_texture);
    gl::BindTexture(gl::GL_TEXTURE_2D, impl_->white_texture);
    gl::TexImage2D(gl::GL_TEXTURE_2D, 0, static_cast<gl::GLint>(gl::GL_RGBA8), 1, 1, 0, gl::GL_RGBA,
                   gl::GL_UNSIGNED_BYTE, white_pixels);
    gl::TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, static_cast<gl::GLint>(gl::GL_NEAREST));
    gl::TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, static_cast<gl::GLint>(gl::GL_NEAREST));

    gl::Enable(gl::GL_BLEND);
    gl::BlendFunc(gl::GL_SRC_ALPHA, gl::GL_ONE_MINUS_SRC_ALPHA);
}

void NativeRenderer2DBackend::BeginDrawing() {
    EnsureInit();
    int w = 0, h = 0;
    NativeContextFramebufferSize(impl_->ctx, &w, &h);
    gl::Viewport(0, 0, w, h);
    impl_->UpdateProjection(w, h);
}

void NativeRenderer2DBackend::EndDrawing() { NativeContextSwapBuffers(impl_->ctx); }

void NativeRenderer2DBackend::ClearBackground(gfx::Color color) {
    gl::ClearColor(static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f,
                   static_cast<float>(color.b) / 255.0f, static_cast<float>(color.a) / 255.0f);
    // Depth too, not just color -- matches raylib's own ClearBackground
    // (rlClearScreenBuffers clears both). Without this the depth buffer
    // keeps whatever was in it (often all-zero on this GL driver), which
    // fails every 3D fragment's default GL_LESS depth test and makes the
    // entire 3D viewport silently draw nothing.
    gl::Clear(gl::GL_COLOR_BUFFER_BIT | gl::GL_DEPTH_BUFFER_BIT);
}

void NativeRenderer2DBackend::BeginScissorMode(int x, int y, int width, int height) {
    gl::Enable(gl::GL_SCISSOR_TEST);
    // GL's scissor origin is bottom-left; gfx::'s is top-left (see
    // Impl::UpdateProjection's own Y-flip) -- flip y the same way here.
    gl::Scissor(x, impl_->target_height - y - height, width, height);
}

void NativeRenderer2DBackend::EndScissorMode() { gl::Disable(gl::GL_SCISSOR_TEST); }

void NativeRenderer2DBackend::DrawRectangle(int x, int y, int width, int height, gfx::Color color) {
    std::vector<Vtx> v;
    AppendSolidQuad(v, static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                    static_cast<float>(height), color);
    impl_->FlushSolid(v);
}

void NativeRenderer2DBackend::DrawRectangleRec(gfx::Rectangle rec, gfx::Color color) {
    std::vector<Vtx> v;
    AppendSolidQuad(v, rec.x, rec.y, rec.width, rec.height, color);
    impl_->FlushSolid(v);
}

void NativeRenderer2DBackend::DrawRectangleGradientEx(gfx::Rectangle rec, gfx::Color top_left,
                                                        gfx::Color bottom_left, gfx::Color top_right,
                                                        gfx::Color bottom_right) {
    std::vector<Vtx> v = {
        MakeVtx(rec.x, rec.y, 0, 0, top_left),
        MakeVtx(rec.x + rec.width, rec.y, 0, 0, top_right),
        MakeVtx(rec.x, rec.y + rec.height, 0, 0, bottom_left),
        MakeVtx(rec.x + rec.width, rec.y, 0, 0, top_right),
        MakeVtx(rec.x + rec.width, rec.y + rec.height, 0, 0, bottom_right),
        MakeVtx(rec.x, rec.y + rec.height, 0, 0, bottom_left),
    };
    impl_->FlushSolid(v);
}

void NativeRenderer2DBackend::DrawRectangleLines(int x, int y, int width, int height, gfx::Color color) {
    gfx::Vector2 pts[4] = {{static_cast<float>(x), static_cast<float>(y)},
                            {static_cast<float>(x + width), static_cast<float>(y)},
                            {static_cast<float>(x + width), static_cast<float>(y + height)},
                            {static_cast<float>(x), static_cast<float>(y + height)}};
    std::vector<Vtx> v;
    for (auto &p : pts) v.push_back(MakeVtx(p.x, p.y, 0, 0, color));
    impl_->FlushSolid(v, gl::GL_LINE_LOOP);
}

void NativeRenderer2DBackend::DrawRectangleLinesEx(gfx::Rectangle rec, float line_thick, gfx::Color color) {
    gfx::Vector2 pts[4] = {{rec.x, rec.y},
                            {rec.x + rec.width, rec.y},
                            {rec.x + rec.width, rec.y + rec.height},
                            {rec.x, rec.y + rec.height}};
    std::vector<Vtx> v;
    AppendThickPolyline(v, pts, 4, line_thick, color, true);
    impl_->FlushSolid(v);
}

void NativeRenderer2DBackend::DrawRectangleRounded(gfx::Rectangle rec, float roundness, int segments,
                                                    gfx::Color color) {
    auto pts = RoundedRectBoundary(rec, roundness, std::max(segments, 4));
    gfx::Vector2 center{rec.x + rec.width / 2.0f, rec.y + rec.height / 2.0f};
    std::vector<Vtx> v;
    for (size_t i = 0; i < pts.size(); i++) {
        size_t j = (i + 1) % pts.size();
        v.push_back(MakeVtx(center.x, center.y, 0, 0, color));
        v.push_back(MakeVtx(pts[i].x, pts[i].y, 0, 0, color));
        v.push_back(MakeVtx(pts[j].x, pts[j].y, 0, 0, color));
    }
    impl_->FlushSolid(v);
}

void NativeRenderer2DBackend::DrawRectangleRoundedLines(gfx::Rectangle rec, float roundness, int segments,
                                                          gfx::Color color) {
    auto pts = RoundedRectBoundary(rec, roundness, std::max(segments, 4));
    std::vector<Vtx> v;
    for (auto &p : pts) v.push_back(MakeVtx(p.x, p.y, 0, 0, color));
    impl_->FlushSolid(v, gl::GL_LINE_LOOP);
}

void NativeRenderer2DBackend::DrawRectangleRoundedLinesEx(gfx::Rectangle rec, float roundness, int segments,
                                                            float line_thick, gfx::Color color) {
    auto pts = RoundedRectBoundary(rec, roundness, std::max(segments, 4));
    std::vector<Vtx> v;
    AppendThickPolyline(v, pts.data(), static_cast<int>(pts.size()), line_thick, color, true);
    impl_->FlushSolid(v);
}

void NativeRenderer2DBackend::DrawEllipseLines(int center_x, int center_y, float radius_h, float radius_v,
                                                gfx::Color color) {
    int segs = CircleSegments(std::max(radius_h, radius_v));
    std::vector<Vtx> v;
    for (int i = 0; i < segs; i++) {
        float t = static_cast<float>(i) / static_cast<float>(segs) * 360.0f * gfx::kDeg2Rad;
        v.push_back(MakeVtx(static_cast<float>(center_x) + radius_h * std::cos(t),
                             static_cast<float>(center_y) + radius_v * std::sin(t), 0, 0, color));
    }
    impl_->FlushSolid(v, gl::GL_LINE_LOOP);
}

void NativeRenderer2DBackend::DrawLine(int start_x, int start_y, int end_x, int end_y, gfx::Color color) {
    std::vector<Vtx> v = {MakeVtx(static_cast<float>(start_x), static_cast<float>(start_y), 0, 0, color),
                           MakeVtx(static_cast<float>(end_x), static_cast<float>(end_y), 0, 0, color)};
    impl_->FlushSolid(v, gl::GL_LINES);
}

void NativeRenderer2DBackend::DrawLineEx(gfx::Vector2 start, gfx::Vector2 end, float thick, gfx::Color color) {
    gfx::Vector2 pts[2] = {start, end};
    std::vector<Vtx> v;
    AppendThickPolyline(v, pts, 2, thick, color, false);
    impl_->FlushSolid(v);
}

void NativeRenderer2DBackend::DrawCircle(int center_x, int center_y, float radius, gfx::Color color) {
    int segs = CircleSegments(radius);
    std::vector<Vtx> v;
    gfx::Vector2 c{static_cast<float>(center_x), static_cast<float>(center_y)};
    for (int i = 0; i < segs; i++) {
        float t0 = static_cast<float>(i) / static_cast<float>(segs) * 360.0f * gfx::kDeg2Rad;
        float t1 = static_cast<float>(i + 1) / static_cast<float>(segs) * 360.0f * gfx::kDeg2Rad;
        v.push_back(MakeVtx(c.x, c.y, 0, 0, color));
        v.push_back(MakeVtx(c.x + radius * std::cos(t0), c.y + radius * std::sin(t0), 0, 0, color));
        v.push_back(MakeVtx(c.x + radius * std::cos(t1), c.y + radius * std::sin(t1), 0, 0, color));
    }
    impl_->FlushSolid(v);
}

void NativeRenderer2DBackend::DrawCircleV(gfx::Vector2 center, float radius, gfx::Color color) {
    DrawCircle(static_cast<int>(center.x), static_cast<int>(center.y), radius, color);
}

void NativeRenderer2DBackend::DrawCircleLines(int center_x, int center_y, float radius, gfx::Color color) {
    DrawEllipseLines(center_x, center_y, radius, radius, color);
}

void NativeRenderer2DBackend::DrawRing(gfx::Vector2 center, float inner_radius, float outer_radius,
                                        float start_angle, float end_angle, int segments, gfx::Color color) {
    segments = std::max(segments, 1);
    std::vector<Vtx> v;
    for (int i = 0; i < segments; i++) {
        float a0 = (start_angle + (end_angle - start_angle) * static_cast<float>(i) / static_cast<float>(segments)) *
                    gfx::kDeg2Rad;
        float a1 = (start_angle +
                    (end_angle - start_angle) * static_cast<float>(i + 1) / static_cast<float>(segments)) *
                    gfx::kDeg2Rad;
        gfx::Vector2 ia{center.x + inner_radius * std::cos(a0), center.y + inner_radius * std::sin(a0)};
        gfx::Vector2 oa{center.x + outer_radius * std::cos(a0), center.y + outer_radius * std::sin(a0)};
        gfx::Vector2 ib{center.x + inner_radius * std::cos(a1), center.y + inner_radius * std::sin(a1)};
        gfx::Vector2 ob{center.x + outer_radius * std::cos(a1), center.y + outer_radius * std::sin(a1)};
        v.push_back(MakeVtx(ia.x, ia.y, 0, 0, color));
        v.push_back(MakeVtx(oa.x, oa.y, 0, 0, color));
        v.push_back(MakeVtx(ob.x, ob.y, 0, 0, color));
        v.push_back(MakeVtx(ia.x, ia.y, 0, 0, color));
        v.push_back(MakeVtx(ob.x, ob.y, 0, 0, color));
        v.push_back(MakeVtx(ib.x, ib.y, 0, 0, color));
    }
    impl_->FlushSolid(v);
}

void NativeRenderer2DBackend::DrawTriangle(gfx::Vector2 v1, gfx::Vector2 v2, gfx::Vector2 v3, gfx::Color color) {
    std::vector<Vtx> v = {MakeVtx(v1.x, v1.y, 0, 0, color), MakeVtx(v2.x, v2.y, 0, 0, color),
                           MakeVtx(v3.x, v3.y, 0, 0, color)};
    impl_->FlushSolid(v);
}

void NativeRenderer2DBackend::DrawTriangleFan(const gfx::Vector2 *points, int point_count, gfx::Color color) {
    if (point_count < 3) return;
    std::vector<Vtx> v;
    for (int i = 1; i < point_count - 1; i++) {
        v.push_back(MakeVtx(points[0].x, points[0].y, 0, 0, color));
        v.push_back(MakeVtx(points[i].x, points[i].y, 0, 0, color));
        v.push_back(MakeVtx(points[i + 1].x, points[i + 1].y, 0, 0, color));
    }
    impl_->FlushSolid(v);
}

bool NativeRenderer2DBackend::CheckCollisionPointRec(gfx::Vector2 point, gfx::Rectangle rec) {
    return point.x >= rec.x && point.x <= rec.x + rec.width && point.y >= rec.y && point.y <= rec.y + rec.height;
}

// -- Images (pure CPU, mep's own image_codec/png_codec) -------------------

gfx::Image NativeRenderer2DBackend::LoadImage(const char *file_name) {
    int w = 0, h = 0;
    std::string error;
    unsigned char *data = image_codec::DecodeFile(file_name, &w, &h, &error);
    if (data == nullptr) return gfx::Image{};
    return gfx::Image{data, w, h, 1, gfx::kPixelFormatR8G8B8A8};
}

gfx::Image NativeRenderer2DBackend::GenImageColor(int width, int height, gfx::Color color) {
    auto *data = static_cast<unsigned char *>(std::malloc(static_cast<size_t>(width) * static_cast<size_t>(height) * 4));
    for (int i = 0; i < width * height; i++) {
        data[i * 4 + 0] = color.r;
        data[i * 4 + 1] = color.g;
        data[i * 4 + 2] = color.b;
        data[i * 4 + 3] = color.a;
    }
    return gfx::Image{data, width, height, 1, gfx::kPixelFormatR8G8B8A8};
}

gfx::Image NativeRenderer2DBackend::ImageFromImage(gfx::Image image, gfx::Rectangle rec) {
    int rx = static_cast<int>(rec.x), ry = static_cast<int>(rec.y);
    int rw = static_cast<int>(rec.width), rh = static_cast<int>(rec.height);
    auto *out = static_cast<unsigned char *>(std::malloc(static_cast<size_t>(rw) * static_cast<size_t>(rh) * 4));
    const auto *src = static_cast<const unsigned char *>(image.data);
    for (int y = 0; y < rh; y++) {
        std::memcpy(out + static_cast<size_t>(y) * static_cast<size_t>(rw) * 4,
                    src + (static_cast<size_t>(ry + y) * static_cast<size_t>(image.width) +
                           static_cast<size_t>(rx)) *
                              4,
                    static_cast<size_t>(rw) * 4);
    }
    return gfx::Image{out, rw, rh, 1, gfx::kPixelFormatR8G8B8A8};
}

void NativeRenderer2DBackend::ImageFormat(gfx::Image *image, int new_format) {
    if (new_format != gfx::kPixelFormatR8G8B8A8) {
        std::fprintf(stderr, "gfx native: ImageFormat only supports RGBA8 today (requested format %d)\n",
                     new_format);
    }
    image->format = gfx::kPixelFormatR8G8B8A8;
}

void NativeRenderer2DBackend::ImageFlipVertical(gfx::Image *image) {
    auto *data = static_cast<unsigned char *>(image->data);
    size_t stride = static_cast<size_t>(image->width) * 4;
    std::vector<unsigned char> row(stride);
    for (int y = 0; y < image->height / 2; y++) {
        unsigned char *a = data + static_cast<size_t>(y) * stride;
        unsigned char *b = data + static_cast<size_t>(image->height - 1 - y) * stride;
        std::memcpy(row.data(), a, stride);
        std::memcpy(a, b, stride);
        std::memcpy(b, row.data(), stride);
    }
}

void NativeRenderer2DBackend::ImageCrop(gfx::Image *image, gfx::Rectangle crop) {
    gfx::Image cropped = ImageFromImage(*image, crop);
    std::free(image->data);
    *image = cropped;
}

void NativeRenderer2DBackend::UnloadImage(gfx::Image image) { std::free(image.data); }

bool NativeRenderer2DBackend::ExportImage(gfx::Image image, const char *file_name) {
    std::string encoded = png::Encode(image.width, image.height, 4, static_cast<const unsigned char *>(image.data),
                                       image.width * 4);
    if (encoded.empty()) return false;
    std::FILE *fp = std::fopen(file_name, "wb");
    if (!fp) return false;
    size_t written = std::fwrite(encoded.data(), 1, encoded.size(), fp);
    std::fclose(fp);
    return written == encoded.size();
}

std::vector<unsigned char> NativeRenderer2DBackend::ExportImageToMemory(gfx::Image image, const char *file_type) {
    std::vector<unsigned char> out;
    if (std::strcmp(file_type, ".png") != 0) {
        std::fprintf(stderr, "gfx native: ExportImageToMemory: unsupported file type '%s' (only .png)\n", file_type);
        return out;
    }
    std::string encoded = png::Encode(image.width, image.height, 4, static_cast<const unsigned char *>(image.data),
                                       image.width * 4);
    out.assign(encoded.begin(), encoded.end());
    return out;
}

gfx::Vector3 NativeRenderer2DBackend::ColorToHSV(gfx::Color color) {
    float r = static_cast<float>(color.r) / 255.0f, g = static_cast<float>(color.g) / 255.0f,
          b = static_cast<float>(color.b) / 255.0f;
    float max_c = std::max({r, g, b}), min_c = std::min({r, g, b});
    float delta = max_c - min_c;
    float h = 0.0f;
    if (delta > 1e-6f) {
        if (max_c == r) h = 60.0f * std::fmod((g - b) / delta, 6.0f);
        else if (max_c == g) h = 60.0f * ((b - r) / delta + 2.0f);
        else h = 60.0f * ((r - g) / delta + 4.0f);
    }
    if (h < 0.0f) h += 360.0f;
    float s = max_c <= 1e-6f ? 0.0f : delta / max_c;
    return {h, s, max_c};
}

gfx::Color NativeRenderer2DBackend::ColorFromHSV(float hue, float saturation, float value) {
    float c = value * saturation;
    float hp = std::fmod(hue, 360.0f) / 60.0f;
    float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else { r = c; b = x; }
    float m = value - c;
    return {static_cast<unsigned char>(std::lround((r + m) * 255.0f)),
            static_cast<unsigned char>(std::lround((g + m) * 255.0f)),
            static_cast<unsigned char>(std::lround((b + m) * 255.0f)), 255};
}

// -- Textures -------------------------------------------------------------

gfx::Texture2D NativeRenderer2DBackend::LoadTextureFromImage(gfx::Image image) {
    gl::GLuint id = 0;
    gl::GenTextures(1, &id);
    gl::BindTexture(gl::GL_TEXTURE_2D, id);
    gl::TexImage2D(gl::GL_TEXTURE_2D, 0, static_cast<gl::GLint>(gl::GL_RGBA8), image.width, image.height, 0,
                   gl::GL_RGBA, gl::GL_UNSIGNED_BYTE, image.data);
    gl::TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, static_cast<gl::GLint>(gl::GL_LINEAR));
    gl::TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, static_cast<gl::GLint>(gl::GL_LINEAR));
    gl::TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_WRAP_S, static_cast<gl::GLint>(gl::GL_CLAMP_TO_EDGE));
    gl::TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_WRAP_T, static_cast<gl::GLint>(gl::GL_CLAMP_TO_EDGE));
    return gfx::Texture2D{id, image.width, image.height, 1, image.format};
}

void NativeRenderer2DBackend::UpdateTexture(gfx::Texture2D texture, const void *pixels) {
    gl::BindTexture(gl::GL_TEXTURE_2D, texture.id);
    gl::TexSubImage2D(gl::GL_TEXTURE_2D, 0, 0, 0, texture.width, texture.height, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE,
                       pixels);
}

void NativeRenderer2DBackend::UnloadTexture(gfx::Texture2D texture) { gl::DeleteTextures(1, &texture.id); }

void NativeRenderer2DBackend::DrawTexturePro(gfx::Texture2D texture, gfx::Rectangle source, gfx::Rectangle dest,
                                              gfx::Vector2 origin, float rotation, gfx::Color tint) {
    float tex_w = static_cast<float>(texture.width), tex_h = static_cast<float>(texture.height);
    float u0 = source.x / tex_w, v0 = source.y / tex_h;
    float u1 = (source.x + source.width) / tex_w, v1 = (source.y + source.height) / tex_h;

    float x0 = -origin.x, y0 = -origin.y;
    float x1 = dest.width - origin.x, y1 = dest.height - origin.y;
    float rad = rotation * gfx::kDeg2Rad;
    float c = std::cos(rad), s = std::sin(rad);
    auto rot = [&](float x, float y) -> gfx::Vector2 {
        return {dest.x + x * c - y * s, dest.y + x * s + y * c};
    };
    gfx::Vector2 p0 = rot(x0, y0), p1 = rot(x1, y0), p2 = rot(x1, y1), p3 = rot(x0, y1);

    std::vector<Vtx> v = {
        MakeVtx(p0.x, p0.y, u0, v0, tint), MakeVtx(p1.x, p1.y, u1, v0, tint), MakeVtx(p2.x, p2.y, u1, v1, tint),
        MakeVtx(p0.x, p0.y, u0, v0, tint), MakeVtx(p2.x, p2.y, u1, v1, tint), MakeVtx(p3.x, p3.y, u0, v1, tint),
    };
    impl_->FlushTextured(texture.id, v);
}

void NativeRenderer2DBackend::DrawTextureEx(gfx::Texture2D texture, gfx::Vector2 position, float rotation,
                                             float scale, gfx::Color tint) {
    gfx::Rectangle source{0, 0, static_cast<float>(texture.width), static_cast<float>(texture.height)};
    gfx::Rectangle dest{position.x, position.y, static_cast<float>(texture.width) * scale,
                        static_cast<float>(texture.height) * scale};
    DrawTexturePro(texture, source, dest, {0, 0}, rotation, tint);
}

void NativeRenderer2DBackend::DrawTextureRec(gfx::Texture2D texture, gfx::Rectangle source, gfx::Vector2 position,
                                              gfx::Color tint) {
    // `source`'s width/height may be negative (a caller's deliberate trick
    // to flip which part of the texture gets sampled -- e.g. the 3D
    // modeler's render-texture blit, DrawModel3DPane, uses a negative
    // source.height to flip a render-texture's bottom-up GL row order back
    // to top-down). That sign is meaningful ONLY for the source UV
    // computation inside DrawTexturePro below; the *destination* quad's
    // on-screen size must stay positive regardless, or its geometry gets
    // pushed in the wrong direction (found via CHESS_SET_BENCHMARK_PLAN.md's
    // GUI-toggle work: the model3d viewport blit's quad was being drawn
    // entirely above its own scissor rect and clipped away, rendering as a
    // blank pane -- this was the root cause, not the toggle/lighting code).
    gfx::Rectangle dest{position.x, position.y, std::fabs(source.width), std::fabs(source.height)};
    DrawTexturePro(texture, source, dest, {0, 0}, 0.0f, tint);
}

// -- Render targets (framebuffers) ---------------------------------------

gfx::RenderTexture2D NativeRenderer2DBackend::LoadRenderTexture(int width, int height) {
    Impl::TargetInfo info;
    info.width = width;
    info.height = height;
    gl::GenTextures(1, &info.color_tex);
    gl::BindTexture(gl::GL_TEXTURE_2D, info.color_tex);
    gl::TexImage2D(gl::GL_TEXTURE_2D, 0, static_cast<gl::GLint>(gl::GL_RGBA8), width, height, 0, gl::GL_RGBA,
                   gl::GL_UNSIGNED_BYTE, nullptr);
    gl::TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, static_cast<gl::GLint>(gl::GL_LINEAR));
    gl::TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, static_cast<gl::GLint>(gl::GL_LINEAR));

    gl::GenRenderbuffers(1, &info.depth_rb);
    gl::BindRenderbuffer(gl::GL_RENDERBUFFER, info.depth_rb);
    gl::RenderbufferStorage(gl::GL_RENDERBUFFER, gl::GL_DEPTH_COMPONENT24, width, height);

    gl::GenFramebuffers(1, &info.fbo);
    gl::BindFramebuffer(gl::GL_FRAMEBUFFER, info.fbo);
    gl::FramebufferTexture2D(gl::GL_FRAMEBUFFER, gl::GL_COLOR_ATTACHMENT0, gl::GL_TEXTURE_2D, info.color_tex, 0);
    gl::FramebufferRenderbuffer(gl::GL_FRAMEBUFFER, gl::GL_DEPTH_ATTACHMENT, gl::GL_RENDERBUFFER, info.depth_rb);
    if (gl::CheckFramebufferStatus(gl::GL_FRAMEBUFFER) != gl::GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "gfx native: LoadRenderTexture: incomplete framebuffer\n");
    }
    gl::BindFramebuffer(gl::GL_FRAMEBUFFER, impl_->current_fbo);

    impl_->targets.push_back(info);
    gfx::RenderTexture2D rt;
    rt.id = info.fbo;
    rt.texture = gfx::Texture2D{info.color_tex, width, height, 1, gfx::kPixelFormatR8G8B8A8};
    rt.depth = gfx::Texture2D{info.depth_rb, width, height, 1, 0};
    return rt;
}

void NativeRenderer2DBackend::UnloadRenderTexture(gfx::RenderTexture2D target) {
    for (size_t i = 0; i < impl_->targets.size(); i++) {
        if (impl_->targets[i].fbo == target.id) {
            gl::DeleteFramebuffers(1, &impl_->targets[i].fbo);
            gl::DeleteTextures(1, &impl_->targets[i].color_tex);
            gl::DeleteRenderbuffers(1, &impl_->targets[i].depth_rb);
            impl_->targets.erase(impl_->targets.begin() + static_cast<long>(i));
            return;
        }
    }
}

void NativeRenderer2DBackend::BeginTextureMode(gfx::RenderTexture2D target) {
    gl::BindFramebuffer(gl::GL_FRAMEBUFFER, target.id);
    impl_->current_fbo = target.id;
    gl::Viewport(0, 0, target.texture.width, target.texture.height);
    impl_->UpdateProjection(target.texture.width, target.texture.height);
}

void NativeRenderer2DBackend::EndTextureMode() {
    gl::BindFramebuffer(gl::GL_FRAMEBUFFER, 0);
    impl_->current_fbo = 0;
    int w = 0, h = 0;
    NativeContextFramebufferSize(impl_->ctx, &w, &h);
    gl::Viewport(0, 0, w, h);
    impl_->UpdateProjection(w, h);
}

gfx::Image NativeRenderer2DBackend::LoadImageFromTexture(gfx::Texture2D texture) {
    // glGetTexImage doesn't exist on OpenGL ES/WebGL, so texture readback
    // has to go through a framebuffer + glReadPixels (which does) instead
    // -- attach the texture to a throwaway FBO, read it, done. Works
    // identically on desktop GL, so there's no platform branch here.
    auto *data = static_cast<unsigned char *>(
        std::malloc(static_cast<size_t>(texture.width) * static_cast<size_t>(texture.height) * 4));
    gl::GLuint fbo = 0;
    gl::GenFramebuffers(1, &fbo);
    gl::BindFramebuffer(gl::GL_FRAMEBUFFER, fbo);
    gl::FramebufferTexture2D(gl::GL_FRAMEBUFFER, gl::GL_COLOR_ATTACHMENT0, gl::GL_TEXTURE_2D, texture.id, 0);
    gl::ReadPixels(0, 0, texture.width, texture.height, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE, data);
    gl::BindFramebuffer(gl::GL_FRAMEBUFFER, impl_->current_fbo);
    gl::DeleteFramebuffers(1, &fbo);
    return gfx::Image{data, texture.width, texture.height, 1, gfx::kPixelFormatR8G8B8A8};
}

namespace {
// glReadPixels returns rows bottom-up ((0,0) is the framebuffer's
// bottom-left corner) with whatever alpha the framebuffer happened to
// have -- raylib's own rlReadScreenPixels flips to top-down and forces
// opaque alpha before handing pixels back (see its own NOTE 1/2
// comments), and every caller on both sides of the gfx:: bridge (this
// backend's ReadScreenPixels/LoadImageFromScreen, main.cpp's
// UiScreenshot/render-to-image RPCs) relies on that exact contract
// without flipping/opaquing again themselves.
unsigned char *ReadPixelsFlipped(int width, int height) {
    std::vector<unsigned char> raw(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    gl::ReadPixels(0, 0, width, height, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE, raw.data());
    auto *out = static_cast<unsigned char *>(std::malloc(raw.size()));
    size_t row_bytes = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; y++) {
        unsigned char *dst_row = out + static_cast<size_t>(height - 1 - y) * row_bytes;
        std::memcpy(dst_row, raw.data() + static_cast<size_t>(y) * row_bytes, row_bytes);
        for (size_t x = 0; x < row_bytes; x += 4) dst_row[x + 3] = 255;
    }
    return out;
}
}  // namespace

gfx::Image NativeRenderer2DBackend::LoadImageFromScreen() {
    int w = impl_->target_width, h = impl_->target_height;
    return gfx::Image{ReadPixelsFlipped(w, h), w, h, 1, gfx::kPixelFormatR8G8B8A8};
}

unsigned char *NativeRenderer2DBackend::ReadScreenPixels(int width, int height) { return ReadPixelsFlipped(width, height); }

}  // namespace gfx
