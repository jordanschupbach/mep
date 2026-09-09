// NativeRenderer3DBackend: the in-house 3D rendering engine for Stage B's
// native backend, backing DrawModel3DPane's viewport and the gizmo/debug
// primitives main.cpp draws directly (DrawCube/DrawSphere/DrawLine3D/
// DrawCylinderEx for the translate/rotate/scale gizmo rings). Two
// shaders: a flat position-only one for those debug primitives (uniform
// color, no texture) and a textured one for DrawMesh, matching
// model3d_doc.h's own documented "no lighting model" simplification
// (texelColor * tint, nothing more).
//
// Model file import (LoadModel) and procedural generation (GenMeshCube
// etc.) are NOT implemented here -- PLAN's Stage B8, a separate step
// (format parsers / parametric mesh math, unrelated to rendering).
// Everything here operates on already-built gfx::Mesh data.

#include "gfx/backend_native_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "gfx/gl_shader.h"
#include "gfx/renderer2d.h"
#include "gfx/vecmath.h"

namespace gfx {

// Format-specific model importers, one per backend_native_model_<format>.cpp
// translation unit -- declared here (not a shared header) since LoadModel's
// dispatch below is their only caller.
Model LoadObjModel(const char *file_name);
Model LoadIqmModel(const char *file_name);
Model LoadVoxModel(const char *file_name);
Model LoadM3dModel(const char *file_name);
Model LoadGltfModel(const char *file_name);

namespace {

struct FlatVtx {
    float x, y, z;
};

// No #version line in any of these four shader bodies -- gl_shader.cpp's
// CompileShaderStage prepends the right one for the target platform.
const char *kFlatVertexSrc = R"GLSL(
layout(location=0) in vec3 aPos;
uniform mat4 uMvp;
void main() { gl_Position = uMvp * vec4(aPos, 1.0); }
)GLSL";

const char *kFlatFragmentSrc = R"GLSL(
uniform vec4 uColor;
out vec4 FragColor;
void main() { FragColor = uColor; }
)GLSL";

// aVertColor: per-vertex RGBA (mesh.colors -- see gfx/types.h's own
// comment on Mesh). Used by the VOX importer (backend_native_model_vox.cpp)
// to carry each voxel's palette color, since DrawMesh's `material` only
// offers one uniform tint for the whole mesh -- everything else that
// builds a Mesh leaves colors null, which UploadMesh below fills with
// opaque white so this attribute is always meaningful to multiply by.
const char *kMeshVertexSrc = R"GLSL(
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aTexCoord;
layout(location=2) in vec4 aVertColor;
uniform mat4 uMvp;
out vec2 vTexCoord;
out vec4 vVertColor;
void main() {
    gl_Position = uMvp * vec4(aPos, 1.0);
    vTexCoord = aTexCoord;
    vVertColor = aVertColor;
}
)GLSL";

const char *kMeshFragmentSrc = R"GLSL(
in vec2 vTexCoord;
in vec4 vVertColor;
uniform sampler2D uTex;
uniform vec4 uColor;
out vec4 FragColor;
void main() { FragColor = texture(uTex, vTexCoord) * uColor * vVertColor; }
)GLSL";

void AppendCubeTris(std::vector<FlatVtx> &out, Vector3 c, float hw, float hh, float hl) {
    // 8 corners, 12 triangles (2 per face), CCW when viewed from outside.
    Vector3 p[8] = {
        {c.x - hw, c.y - hh, c.z - hl}, {c.x + hw, c.y - hh, c.z - hl}, {c.x + hw, c.y + hh, c.z - hl},
        {c.x - hw, c.y + hh, c.z - hl}, {c.x - hw, c.y - hh, c.z + hl}, {c.x + hw, c.y - hh, c.z + hl},
        {c.x + hw, c.y + hh, c.z + hl}, {c.x - hw, c.y + hh, c.z + hl},
    };
    int faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
    for (auto &f : faces) {
        out.push_back({p[f[0]].x, p[f[0]].y, p[f[0]].z});
        out.push_back({p[f[1]].x, p[f[1]].y, p[f[1]].z});
        out.push_back({p[f[2]].x, p[f[2]].y, p[f[2]].z});
        out.push_back({p[f[0]].x, p[f[0]].y, p[f[0]].z});
        out.push_back({p[f[2]].x, p[f[2]].y, p[f[2]].z});
        out.push_back({p[f[3]].x, p[f[3]].y, p[f[3]].z});
    }
}

void AppendSphereTris(std::vector<FlatVtx> &out, Vector3 center, float radius, int rings = 16, int slices = 16) {
    for (int r = 0; r < rings; r++) {
        float lat0 = kPi * (-0.5f + static_cast<float>(r) / static_cast<float>(rings));
        float lat1 = kPi * (-0.5f + static_cast<float>(r + 1) / static_cast<float>(rings));
        for (int s = 0; s < slices; s++) {
            float lon0 = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(slices);
            float lon1 = 2.0f * kPi * static_cast<float>(s + 1) / static_cast<float>(slices);
            auto pt = [&](float lat, float lon) -> Vector3 {
                return {center.x + radius * std::cos(lat) * std::cos(lon), center.y + radius * std::sin(lat),
                        center.z + radius * std::cos(lat) * std::sin(lon)};
            };
            Vector3 a = pt(lat0, lon0), b = pt(lat1, lon0), c = pt(lat1, lon1), d = pt(lat0, lon1);
            out.push_back({a.x, a.y, a.z});
            out.push_back({b.x, b.y, b.z});
            out.push_back({c.x, c.y, c.z});
            out.push_back({a.x, a.y, a.z});
            out.push_back({c.x, c.y, c.z});
            out.push_back({d.x, d.y, d.z});
        }
    }
}

void AppendCylinderTris(std::vector<FlatVtx> &out, Vector3 start, Vector3 end, float r0, float r1, int sides) {
    Vector3 axis = Vector3Normalize(Vector3Subtract(end, start));
    // Any vector not parallel to axis, to build a perpendicular basis.
    Vector3 helper = std::fabs(axis.y) < 0.99f ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
    Vector3 u = Vector3Normalize(Vector3CrossProduct(axis, helper));
    Vector3 v = Vector3CrossProduct(axis, u);
    sides = std::max(sides, 3);
    for (int i = 0; i < sides; i++) {
        float a0 = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(sides);
        float a1 = 2.0f * kPi * static_cast<float>(i + 1) / static_cast<float>(sides);
        auto ring = [&](Vector3 base, float radius, float ang) -> Vector3 {
            return Vector3Add(base, Vector3Add(Vector3Scale(u, radius * std::cos(ang)),
                                                Vector3Scale(v, radius * std::sin(ang))));
        };
        Vector3 p0 = ring(start, r0, a0), p1 = ring(start, r0, a1);
        Vector3 p2 = ring(end, r1, a1), p3 = ring(end, r1, a0);
        out.push_back({p0.x, p0.y, p0.z});
        out.push_back({p1.x, p1.y, p1.z});
        out.push_back({p2.x, p2.y, p2.z});
        out.push_back({p0.x, p0.y, p0.z});
        out.push_back({p2.x, p2.y, p2.z});
        out.push_back({p3.x, p3.y, p3.z});
    }
}

// gfx::Matrix's fields are declared grouped by row (m0,m4,m8,m12 is "row
// 0", see gfx/types.h) for layout-compatibility with raylib's own Matrix
// -- so its in-memory field order is m0,m4,m8,m12,m1,m5,m9,m13,..., NOT
// the sequential m0,m1,m2,...,m15 a flat column-major float[16] needs.
// `&matrix.m0` therefore is *not* valid to hand to glUniformMatrix4fv
// directly; every field must be placed into a real array by name first
// (matching what raylib's own MatrixToFloatV does before *its*
// glUniformMatrix4fv calls).
void UploadMatrix(gl::GLint loc, const Matrix &m) {
    float v[16] = {m.m0, m.m1, m.m2,  m.m3,  m.m4,  m.m5,  m.m6,  m.m7,
                   m.m8, m.m9, m.m10, m.m11, m.m12, m.m13, m.m14, m.m15};
    gl::UniformMatrix4fv(loc, 1, gl::GL_FALSE_, v);
}

// Standard Moller-Trumbore ray/triangle intersection.
bool RayTriangleIntersect(Ray ray, Vector3 v0, Vector3 v1, Vector3 v2, float *out_t) {
    constexpr float kEps = 1e-7f;
    Vector3 edge1 = Vector3Subtract(v1, v0);
    Vector3 edge2 = Vector3Subtract(v2, v0);
    Vector3 pvec = Vector3CrossProduct(ray.direction, edge2);
    float det = Vector3DotProduct(edge1, pvec);
    if (std::fabs(det) < kEps) return false;
    float inv_det = 1.0f / det;
    Vector3 tvec = Vector3Subtract(ray.position, v0);
    float u = Vector3DotProduct(tvec, pvec) * inv_det;
    if (u < 0.0f || u > 1.0f) return false;
    Vector3 qvec = Vector3CrossProduct(tvec, edge1);
    float v = Vector3DotProduct(ray.direction, qvec) * inv_det;
    if (v < 0.0f || u + v > 1.0f) return false;
    float t = Vector3DotProduct(edge2, qvec) * inv_det;
    if (t < kEps) return false;
    *out_t = t;
    return true;
}

// Accumulates a procedural mesh's positions/normals/texcoords/indices as
// plain vectors, then hands ownership of freshly `new[]`'d flat arrays to
// a gfx::Mesh (freed by UnloadMesh above) -- shared by every GenMesh*
// below rather than each hand-rolling its own array bookkeeping.
struct MeshBuilder {
    std::vector<float> positions, normals, texcoords;
    std::vector<unsigned short> indices;

    void AddVertex(Vector3 p, Vector3 n, float u, float v) {
        positions.push_back(p.x);
        positions.push_back(p.y);
        positions.push_back(p.z);
        normals.push_back(n.x);
        normals.push_back(n.y);
        normals.push_back(n.z);
        texcoords.push_back(u);
        texcoords.push_back(v);
    }
    void AddTriangle(int a, int b, int c) {
        indices.push_back(static_cast<unsigned short>(a));
        indices.push_back(static_cast<unsigned short>(b));
        indices.push_back(static_cast<unsigned short>(c));
    }

    Mesh Build() {
        Mesh m{};
        m.vertexCount = static_cast<int>(positions.size() / 3);
        m.triangleCount = static_cast<int>(indices.size() / 3);
        auto *v = new float[positions.size()];
        std::copy(positions.begin(), positions.end(), v);
        m.vertices = v;
        auto *n = new float[normals.size()];
        std::copy(normals.begin(), normals.end(), n);
        m.normals = n;
        auto *uv = new float[texcoords.size()];
        std::copy(texcoords.begin(), texcoords.end(), uv);
        m.texcoords = uv;
        auto *idx = new unsigned short[indices.size()];
        std::copy(indices.begin(), indices.end(), idx);
        m.indices = idx;
        return m;
    }
};

}  // namespace

struct NativeRenderer3DBackend::Impl {
    NativeContext *ctx = nullptr;

    gl::GLuint flat_program = 0;
    gl::GLint flat_mvp_loc = -1, flat_color_loc = -1;
    gl::GLuint mesh_program = 0;
    gl::GLint mesh_mvp_loc = -1, mesh_tex_loc = -1, mesh_color_loc = -1;

    gl::GLuint flat_vao = 0, flat_vbo = 0;
    gl::GLuint white_texture = 0;

    Matrix view = MatrixIdentity();
    Matrix proj = MatrixIdentity();
    std::vector<Matrix> matrix_stack{MatrixIdentity()};

    void EnsureInit() {
        if (flat_program != 0) return;
        flat_program = gl::BuildProgram(kFlatVertexSrc, kFlatFragmentSrc);
        flat_mvp_loc = gl::GetUniformLocation(flat_program, "uMvp");
        flat_color_loc = gl::GetUniformLocation(flat_program, "uColor");
        mesh_program = gl::BuildProgram(kMeshVertexSrc, kMeshFragmentSrc);
        mesh_mvp_loc = gl::GetUniformLocation(mesh_program, "uMvp");
        mesh_tex_loc = gl::GetUniformLocation(mesh_program, "uTex");
        mesh_color_loc = gl::GetUniformLocation(mesh_program, "uColor");

        gl::GenVertexArrays(1, &flat_vao);
        gl::BindVertexArray(flat_vao);
        gl::GenBuffers(1, &flat_vbo);
        gl::BindBuffer(gl::GL_ARRAY_BUFFER, flat_vbo);
        gl::EnableVertexAttribArray(0);
        gl::VertexAttribPointer(0, 3, gl::GL_FLOAT, gl::GL_FALSE_, sizeof(FlatVtx), nullptr);

        unsigned char white_pixels[4] = {255, 255, 255, 255};
        gl::GenTextures(1, &white_texture);
        gl::BindTexture(gl::GL_TEXTURE_2D, white_texture);
        gl::TexImage2D(gl::GL_TEXTURE_2D, 0, static_cast<gl::GLint>(gl::GL_RGBA8), 1, 1, 0, gl::GL_RGBA,
                       gl::GL_UNSIGNED_BYTE, white_pixels);
        gl::TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, static_cast<gl::GLint>(gl::GL_NEAREST));
        gl::TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, static_cast<gl::GLint>(gl::GL_NEAREST));
    }

    // "Apply `local` first, then whatever's already been accumulated" --
    // matches OpenGL's own glTranslatef/glMultMatrixf post-multiply
    // semantics, so main.cpp's mechanically-ported rlTranslatef/
    // rlMultMatrixf gizmo code (now gfx::TranslateMatrix/MultMatrix)
    // composes the same way it did against raylib's real rlgl stack.
    void ComposeIntoTop(Matrix local) { matrix_stack.back() = MatrixMultiply(local, matrix_stack.back()); }

    Matrix CurrentMvp(Matrix model = MatrixIdentity()) {
        Matrix m = MatrixMultiply(model, matrix_stack.back());
        return MatrixMultiply(m, MatrixMultiply(view, proj));
    }

    void FlushFlat(const std::vector<FlatVtx> &verts, Color color, gl::GLenum mode, Matrix model = MatrixIdentity()) {
        if (verts.empty()) return;
        Matrix mvp = CurrentMvp(model);
        gl::UseProgram(flat_program);
        UploadMatrix(flat_mvp_loc, mvp);
        gl::Uniform4f(flat_color_loc, static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f,
                     static_cast<float>(color.b) / 255.0f, static_cast<float>(color.a) / 255.0f);
        gl::BindVertexArray(flat_vao);
        gl::BindBuffer(gl::GL_ARRAY_BUFFER, flat_vbo);
        gl::BufferData(gl::GL_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(sizeof(FlatVtx)) * static_cast<gl::GLsizeiptr>(verts.size()),
                       verts.data(), gl::GL_STREAM_DRAW);
        gl::DrawArrays(mode, 0, static_cast<gl::GLsizei>(verts.size()));
    }
};

NativeRenderer3DBackend::NativeRenderer3DBackend(NativeContext *ctx) : impl_(new Impl()) { impl_->ctx = ctx; }
NativeRenderer3DBackend::~NativeRenderer3DBackend() { delete impl_; }

void NativeRenderer3DBackend::BeginMode3D(gfx::Camera3D camera) {
    impl_->EnsureInit();
    int w = 0, h = 0;
    glfwGetFramebufferSize(NativeContextWindow(impl_->ctx), &w, &h);
    float aspect = h != 0 ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
    impl_->view = gfx::MatrixLookAt(camera.position, camera.target, camera.up);
    impl_->proj = gfx::MatrixPerspective(camera.fovy * gfx::kDeg2Rad, aspect, 0.05f, 4000.0f);
    gl::Enable(gl::GL_DEPTH_TEST);
}

void NativeRenderer3DBackend::EndMode3D() { gl::Disable(gl::GL_DEPTH_TEST); }

void NativeRenderer3DBackend::DrawGrid(int slices, float spacing) {
    std::vector<FlatVtx> verts;
    float half = static_cast<float>(slices) * spacing * 0.5f;
    for (int i = 0; i <= slices; i++) {
        float pos = -half + static_cast<float>(i) * spacing;
        verts.push_back({pos, 0, -half});
        verts.push_back({pos, 0, half});
        verts.push_back({-half, 0, pos});
        verts.push_back({half, 0, pos});
    }
    impl_->FlushFlat(verts, gfx::Color{130, 130, 130, 255}, gl::GL_LINES);
}

void NativeRenderer3DBackend::DrawLine3D(gfx::Vector3 start, gfx::Vector3 end, gfx::Color color) {
    std::vector<FlatVtx> verts = {{start.x, start.y, start.z}, {end.x, end.y, end.z}};
    impl_->FlushFlat(verts, color, gl::GL_LINES);
}

void NativeRenderer3DBackend::DrawCube(gfx::Vector3 position, float width, float height, float length,
                                        gfx::Color color) {
    std::vector<FlatVtx> verts;
    AppendCubeTris(verts, position, width * 0.5f, height * 0.5f, length * 0.5f);
    impl_->FlushFlat(verts, color, gl::GL_TRIANGLES);
}

void NativeRenderer3DBackend::DrawSphere(gfx::Vector3 center, float radius, gfx::Color color) {
    std::vector<FlatVtx> verts;
    AppendSphereTris(verts, center, radius);
    impl_->FlushFlat(verts, color, gl::GL_TRIANGLES);
}

void NativeRenderer3DBackend::DrawCylinderEx(gfx::Vector3 start, gfx::Vector3 end, float start_radius,
                                              float end_radius, int sides, gfx::Color color) {
    std::vector<FlatVtx> verts;
    AppendCylinderTris(verts, start, end, start_radius, end_radius, sides);
    impl_->FlushFlat(verts, color, gl::GL_TRIANGLES);
}

void NativeRenderer3DBackend::DrawBoundingBox(gfx::BoundingBox box, gfx::Color color) {
    gfx::Vector3 p[8] = {
        {box.min.x, box.min.y, box.min.z}, {box.max.x, box.min.y, box.min.z}, {box.max.x, box.max.y, box.min.z},
        {box.min.x, box.max.y, box.min.z}, {box.min.x, box.min.y, box.max.z}, {box.max.x, box.min.y, box.max.z},
        {box.max.x, box.max.y, box.max.z}, {box.min.x, box.max.y, box.max.z},
    };
    int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                         {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    std::vector<FlatVtx> verts;
    for (auto &e : edges) {
        verts.push_back({p[e[0]].x, p[e[0]].y, p[e[0]].z});
        verts.push_back({p[e[1]].x, p[e[1]].y, p[e[1]].z});
    }
    impl_->FlushFlat(verts, color, gl::GL_LINES);
}

gfx::Ray NativeRenderer3DBackend::GetScreenToWorldRayEx(gfx::Vector2 position, gfx::Camera3D camera, int width,
                                                         int height) {
    gfx::Vector3 forward = gfx::Vector3Normalize(gfx::Vector3Subtract(camera.target, camera.position));
    gfx::Vector3 right = gfx::Vector3Normalize(gfx::Vector3CrossProduct(forward, camera.up));
    gfx::Vector3 up = gfx::Vector3CrossProduct(right, forward);
    float top = std::tan(camera.fovy * 0.5f * gfx::kDeg2Rad);
    float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    float right_extent = top * aspect;
    float ndc_x = (2.0f * position.x / static_cast<float>(width) - 1.0f) * right_extent;
    float ndc_y = (1.0f - 2.0f * position.y / static_cast<float>(height)) * top;
    gfx::Vector3 dir = gfx::Vector3Normalize(gfx::Vector3Add(
        gfx::Vector3Add(forward, gfx::Vector3Scale(right, ndc_x)), gfx::Vector3Scale(up, ndc_y)));
    return gfx::Ray{camera.position, dir};
}

gfx::Vector2 NativeRenderer3DBackend::GetWorldToScreenEx(gfx::Vector3 position, gfx::Camera3D camera, int width,
                                                          int height) {
    gfx::Vector3 forward = gfx::Vector3Normalize(gfx::Vector3Subtract(camera.target, camera.position));
    gfx::Vector3 right = gfx::Vector3Normalize(gfx::Vector3CrossProduct(forward, camera.up));
    gfx::Vector3 up = gfx::Vector3CrossProduct(right, forward);
    gfx::Vector3 rel = gfx::Vector3Subtract(position, camera.position);
    float dist = gfx::Vector3DotProduct(rel, forward);
    if (dist <= 1e-5f) return {-1.0f, -1.0f};  // behind the camera
    float view_x = gfx::Vector3DotProduct(rel, right);
    float view_y = gfx::Vector3DotProduct(rel, up);
    float top = std::tan(camera.fovy * 0.5f * gfx::kDeg2Rad);
    float aspect = height != 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    float ndc_x = view_x / (dist * top * aspect);
    float ndc_y = view_y / (dist * top);
    return {(ndc_x * 0.5f + 0.5f) * static_cast<float>(width),
            (1.0f - (ndc_y * 0.5f + 0.5f)) * static_cast<float>(height)};
}

namespace {
// True if `name` ends with `suffix`, case-insensitively -- file extension
// matching for LoadModel's format dispatch below.
bool EndsWithCI(const std::string &name, const char *suffix) {
    size_t len = std::strlen(suffix);
    if (name.size() < len) return false;
    return std::equal(name.end() - static_cast<long>(len), name.end(), suffix,
                       [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}
}  // namespace

gfx::Model NativeRenderer3DBackend::LoadModel(const char *file_name) {
    std::string path(file_name);
    // Each format's parser lives in its own translation unit
    // (backend_native_model_<format>.cpp) -- declared extern here rather
    // than in a shared header, since nothing outside this dispatch calls
    // them directly.
    if (EndsWithCI(path, ".obj")) return LoadObjModel(file_name);
    if (EndsWithCI(path, ".iqm")) return LoadIqmModel(file_name);
    if (EndsWithCI(path, ".vox")) return LoadVoxModel(file_name);
    if (EndsWithCI(path, ".m3d")) return LoadM3dModel(file_name);
    if (EndsWithCI(path, ".gltf") || EndsWithCI(path, ".glb")) return LoadGltfModel(file_name);
    std::fprintf(stderr, "gfx native: LoadModel: unsupported or not-yet-implemented format for '%s'\n",
                 file_name);
    return gfx::Model{};
}

void NativeRenderer3DBackend::UnloadModel(gfx::Model model) {
    // model3d_doc.cpp reads .meshes and, per-material, .maps[kMaterialMapAlbedo]
    // (color and, if present, a GPU texture it reads back via
    // LoadImageFromTexture) from an import, then discards the Model -- so
    // this frees the CPU-side mesh arrays LoadModel's format parsers
    // allocated (none of these meshes have been through UploadMesh, no
    // GPU mesh resources to release at this point) and, unlike raylib's
    // own UnloadModel (which deliberately leaks a model's material
    // textures -- see its own comment on shared-texture ownership), does
    // unload each material's texture too: every texture here was
    // freshly uploaded by this same LoadModel call, never shared, so
    // there's no aliasing risk in owning its cleanup.
    for (int i = 0; i < model.meshCount; i++) {
        delete[] model.meshes[i].vertices;
        delete[] model.meshes[i].texcoords;
        delete[] model.meshes[i].normals;
        delete[] model.meshes[i].colors;
        delete[] model.meshes[i].indices;
    }
    delete[] model.meshes;
    for (int i = 0; i < model.materialCount; i++) {
        if (model.materials[i].maps == nullptr) continue;
        gfx::Texture2D tex = model.materials[i].maps[gfx::kMaterialMapAlbedo].texture;
        if (tex.id != 0) gfx::UnloadTexture(tex);
        delete[] model.materials[i].maps;
    }
    delete[] model.materials;
    delete[] model.meshMaterial;
}

void NativeRenderer3DBackend::UnloadMesh(gfx::Mesh mesh) {
    if (mesh.vaoId != 0) {
        auto vao = static_cast<gl::GLuint>(mesh.vaoId);
        gl::DeleteVertexArrays(1, &vao);
    }
    if (mesh.vboId != nullptr) {
        gl::DeleteBuffers(4, mesh.vboId);
        delete[] mesh.vboId;
    }
    // CPU-side arrays: owned by whichever GenMesh*/import call produced
    // this Mesh (see MeshBuilder::Build below for the procedural-gen
    // side) -- freed here same as raylib's own UnloadMesh does, since
    // GetRayCollisionMesh needs them to stay valid (readable) for the
    // Mesh's whole lifetime, not just until UploadMesh runs.
    delete[] mesh.vertices;
    delete[] mesh.texcoords;
    delete[] mesh.normals;
    delete[] mesh.colors;
    delete[] mesh.indices;
}

void NativeRenderer3DBackend::UploadMesh(gfx::Mesh *mesh, bool dynamic) {
    gl::GLenum usage = dynamic ? gl::GL_DYNAMIC_DRAW : gl::GL_STATIC_DRAW;
    gl::GLuint vao = 0;
    gl::GenVertexArrays(1, &vao);
    gl::BindVertexArray(vao);

    auto *vbos = new unsigned int[4]{0, 0, 0, 0};
    gl::GenBuffers(4, vbos);

    gl::BindBuffer(gl::GL_ARRAY_BUFFER, vbos[0]);
    gl::BufferData(gl::GL_ARRAY_BUFFER,
                   static_cast<gl::GLsizeiptr>(sizeof(float)) * 3 * mesh->vertexCount, mesh->vertices, usage);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 3, gl::GL_FLOAT, gl::GL_FALSE_, 0, nullptr);

    std::vector<float> zeros_uv;
    const float *texcoords = mesh->texcoords;
    if (texcoords == nullptr) {
        zeros_uv.assign(static_cast<size_t>(mesh->vertexCount) * 2, 0.0f);
        texcoords = zeros_uv.data();
    }
    gl::BindBuffer(gl::GL_ARRAY_BUFFER, vbos[1]);
    gl::BufferData(gl::GL_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(sizeof(float)) * 2 * mesh->vertexCount,
                   texcoords, usage);
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 2, gl::GL_FLOAT, gl::GL_FALSE_, 0, nullptr);

    // aVertColor (see kMeshVertexSrc's own comment): defaults to opaque
    // white when mesh->colors is null, so every mesh (not just VOX
    // imports, the only importer that sets real per-vertex colors today)
    // multiplies by a meaningful value.
    std::vector<unsigned char> white_colors;
    const unsigned char *colors = mesh->colors;
    if (colors == nullptr) {
        white_colors.assign(static_cast<size_t>(mesh->vertexCount) * 4, 255);
        colors = white_colors.data();
    }
    gl::BindBuffer(gl::GL_ARRAY_BUFFER, vbos[2]);
    gl::BufferData(gl::GL_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(4) * mesh->vertexCount, colors, usage);
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 4, gl::GL_UNSIGNED_BYTE, gl::GL_TRUE_, 0, nullptr);

    if (mesh->indices != nullptr) {
        gl::BindBuffer(gl::GL_ELEMENT_ARRAY_BUFFER, vbos[3]);
        gl::BufferData(gl::GL_ELEMENT_ARRAY_BUFFER,
                       static_cast<gl::GLsizeiptr>(sizeof(unsigned short)) * 3 * mesh->triangleCount,
                       mesh->indices, usage);
    }

    mesh->vaoId = vao;
    mesh->vboId = vbos;
}

void NativeRenderer3DBackend::DrawMesh(gfx::Mesh mesh, gfx::Material material, gfx::Matrix transform) {
    if (mesh.vaoId == 0) return;
    gfx::Matrix mvp = impl_->CurrentMvp(transform);
    gl::UseProgram(impl_->mesh_program);
    UploadMatrix(impl_->mesh_mvp_loc, mvp);
    gfx::Color tint = gfx::White;
    gl::GLuint tex_id = impl_->white_texture;
    if (material.maps != nullptr) {
        tint = material.maps[gfx::kMaterialMapAlbedo].color;
        if (material.maps[gfx::kMaterialMapAlbedo].texture.id != 0) {
            tex_id = material.maps[gfx::kMaterialMapAlbedo].texture.id;
        }
    }
    gl::Uniform4f(impl_->mesh_color_loc, static_cast<float>(tint.r) / 255.0f, static_cast<float>(tint.g) / 255.0f,
                 static_cast<float>(tint.b) / 255.0f, static_cast<float>(tint.a) / 255.0f);
    gl::ActiveTexture(gl::GL_TEXTURE0);
    gl::BindTexture(gl::GL_TEXTURE_2D, tex_id);
    gl::Uniform1i(impl_->mesh_tex_loc, 0);
    gl::BindVertexArray(static_cast<gl::GLuint>(mesh.vaoId));
    if (mesh.indices != nullptr) {
        gl::DrawElements(gl::GL_TRIANGLES, mesh.triangleCount * 3, gl::GL_UNSIGNED_SHORT, nullptr);
    } else {
        gl::DrawArrays(gl::GL_TRIANGLES, 0, mesh.vertexCount);
    }
}

gfx::Material NativeRenderer3DBackend::LoadMaterialDefault() {
    auto *maps = new gfx::MaterialMap[gfx::kMaxMaterialMaps]();
    maps[gfx::kMaterialMapAlbedo].color = gfx::White;
    gfx::Material m{};
    m.maps = maps;
    return m;
}

gfx::Mesh NativeRenderer3DBackend::GenMeshCube(float width, float height, float length) {
    MeshBuilder b;
    float hw = width * 0.5f, hh = height * 0.5f, hl = length * 0.5f;
    struct Face {
        gfx::Vector3 normal, u_dir, v_dir;
    };
    Face faces[6] = {
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   // front (+Z)
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}, // back (-Z)
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  // right (+X)
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},  // left (-X)
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  // top (+Y)
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},  // bottom (-Y)
    };
    gfx::Vector3 half{hw, hh, hl};
    for (auto &f : faces) {
        auto scale_by_half = [&](gfx::Vector3 v) -> gfx::Vector3 {
            return {v.x * half.x, v.y * half.y, v.z * half.z};
        };
        gfx::Vector3 center = scale_by_half(f.normal);
        gfx::Vector3 u = scale_by_half(f.u_dir);
        gfx::Vector3 v = scale_by_half(f.v_dir);
        int base = static_cast<int>(b.positions.size() / 3);
        b.AddVertex(gfx::Vector3Subtract(gfx::Vector3Subtract(center, u), v), f.normal, 0, 0);
        b.AddVertex(gfx::Vector3Subtract(gfx::Vector3Add(center, u), v), f.normal, 1, 0);
        b.AddVertex(gfx::Vector3Add(gfx::Vector3Add(center, u), v), f.normal, 1, 1);
        b.AddVertex(gfx::Vector3Add(gfx::Vector3Subtract(center, u), v), f.normal, 0, 1);
        b.AddTriangle(base, base + 1, base + 2);
        b.AddTriangle(base, base + 2, base + 3);
    }
    return b.Build();
}

gfx::Mesh NativeRenderer3DBackend::GenMeshSphere(float radius, int rings, int slices) {
    rings = std::max(rings, 2);
    slices = std::max(slices, 3);
    MeshBuilder b;
    for (int r = 0; r <= rings; r++) {
        float lat = kPi * (-0.5f + static_cast<float>(r) / static_cast<float>(rings));
        for (int s = 0; s <= slices; s++) {
            float lon = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(slices);
            gfx::Vector3 n{std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon)};
            float u = static_cast<float>(s) / static_cast<float>(slices);
            float v = static_cast<float>(r) / static_cast<float>(rings);
            b.AddVertex(gfx::Vector3Scale(n, radius), n, u, v);
        }
    }
    int stride = slices + 1;
    for (int r = 0; r < rings; r++) {
        for (int s = 0; s < slices; s++) {
            int i0 = r * stride + s, i1 = i0 + 1, i2 = i0 + stride, i3 = i2 + 1;
            b.AddTriangle(i0, i2, i1);
            b.AddTriangle(i1, i2, i3);
        }
    }
    return b.Build();
}

gfx::Mesh NativeRenderer3DBackend::GenMeshCylinder(float radius, float height, int slices) {
    slices = std::max(slices, 3);
    MeshBuilder b;
    for (int s = 0; s <= slices; s++) {
        float ang = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(slices);
        gfx::Vector3 n{std::cos(ang), 0, std::sin(ang)};
        float u = static_cast<float>(s) / static_cast<float>(slices);
        b.AddVertex({n.x * radius, 0, n.z * radius}, n, u, 1);
        b.AddVertex({n.x * radius, height, n.z * radius}, n, u, 0);
    }
    for (int s = 0; s < slices; s++) {
        int i0 = s * 2, i1 = i0 + 1, i2 = i0 + 2, i3 = i0 + 3;
        b.AddTriangle(i0, i2, i1);
        b.AddTriangle(i1, i2, i3);
    }
    auto add_cap = [&](float y, gfx::Vector3 normal, bool flip) {
        int center = static_cast<int>(b.positions.size() / 3);
        b.AddVertex({0, y, 0}, normal, 0.5f, 0.5f);
        int start = center + 1;
        for (int s = 0; s <= slices; s++) {
            float ang = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(slices);
            b.AddVertex({std::cos(ang) * radius, y, std::sin(ang) * radius}, normal,
                        std::cos(ang) * 0.5f + 0.5f, std::sin(ang) * 0.5f + 0.5f);
        }
        for (int s = 0; s < slices; s++) {
            if (flip) b.AddTriangle(center, start + s, start + s + 1);
            else b.AddTriangle(center, start + s + 1, start + s);
        }
    };
    add_cap(0, {0, -1, 0}, false);
    add_cap(height, {0, 1, 0}, true);
    return b.Build();
}

gfx::Mesh NativeRenderer3DBackend::GenMeshCone(float radius, float height, int slices) {
    slices = std::max(slices, 3);
    MeshBuilder b;
    for (int s = 0; s < slices; s++) {
        float ang0 = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(slices);
        float ang1 = 2.0f * kPi * static_cast<float>(s + 1) / static_cast<float>(slices);
        gfx::Vector3 p0{std::cos(ang0) * radius, 0, std::sin(ang0) * radius};
        gfx::Vector3 p1{std::cos(ang1) * radius, 0, std::sin(ang1) * radius};
        gfx::Vector3 apex{0, height, 0};
        gfx::Vector3 normal = gfx::Vector3Normalize(
            gfx::Vector3CrossProduct(gfx::Vector3Subtract(p1, p0), gfx::Vector3Subtract(apex, p0)));
        int base = static_cast<int>(b.positions.size() / 3);
        b.AddVertex(p0, normal, static_cast<float>(s) / static_cast<float>(slices), 1);
        b.AddVertex(p1, normal, static_cast<float>(s + 1) / static_cast<float>(slices), 1);
        b.AddVertex(apex, normal, (static_cast<float>(s) + 0.5f) / static_cast<float>(slices), 0);
        b.AddTriangle(base, base + 1, base + 2);
    }
    int center = static_cast<int>(b.positions.size() / 3);
    b.AddVertex({0, 0, 0}, {0, -1, 0}, 0.5f, 0.5f);
    int start = center + 1;
    for (int s = 0; s <= slices; s++) {
        float ang = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(slices);
        b.AddVertex({std::cos(ang) * radius, 0, std::sin(ang) * radius}, {0, -1, 0}, std::cos(ang) * 0.5f + 0.5f,
                    std::sin(ang) * 0.5f + 0.5f);
    }
    for (int s = 0; s < slices; s++) b.AddTriangle(center, start + s + 1, start + s);
    return b.Build();
}

gfx::Mesh NativeRenderer3DBackend::GenMeshPlane(float width, float length, int res_x, int res_z) {
    res_x = std::max(res_x, 1);
    res_z = std::max(res_z, 1);
    MeshBuilder b;
    for (int z = 0; z <= res_z; z++) {
        for (int x = 0; x <= res_x; x++) {
            float px = (static_cast<float>(x) / static_cast<float>(res_x) - 0.5f) * width;
            float pz = (static_cast<float>(z) / static_cast<float>(res_z) - 0.5f) * length;
            float u = static_cast<float>(x) / static_cast<float>(res_x);
            float v = static_cast<float>(z) / static_cast<float>(res_z);
            b.AddVertex({px, 0, pz}, {0, 1, 0}, u, v);
        }
    }
    for (int z = 0; z < res_z; z++) {
        for (int x = 0; x < res_x; x++) {
            int i0 = z * (res_x + 1) + x, i1 = i0 + 1, i2 = i0 + res_x + 1, i3 = i2 + 1;
            b.AddTriangle(i0, i2, i1);
            b.AddTriangle(i1, i2, i3);
        }
    }
    return b.Build();
}

gfx::Mesh NativeRenderer3DBackend::GenMeshTorus(float radius, float size, int rad_seg, int sides) {
    rad_seg = std::max(rad_seg, 3);
    sides = std::max(sides, 3);
    MeshBuilder b;
    for (int i = 0; i <= rad_seg; i++) {
        float u = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(rad_seg);
        gfx::Vector3 center{std::cos(u) * radius, 0, std::sin(u) * radius};
        for (int j = 0; j <= sides; j++) {
            float v = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(sides);
            gfx::Vector3 dir{std::cos(u) * std::cos(v), std::sin(v), std::sin(u) * std::cos(v)};
            gfx::Vector3 p = gfx::Vector3Add(center, gfx::Vector3Scale(dir, size));
            b.AddVertex(p, dir, static_cast<float>(i) / static_cast<float>(rad_seg),
                        static_cast<float>(j) / static_cast<float>(sides));
        }
    }
    int stride = sides + 1;
    for (int i = 0; i < rad_seg; i++) {
        for (int j = 0; j < sides; j++) {
            int i0 = i * stride + j, i1 = i0 + 1, i2 = i0 + stride, i3 = i2 + 1;
            b.AddTriangle(i0, i1, i2);
            b.AddTriangle(i1, i3, i2);
        }
    }
    return b.Build();
}

gfx::RayCollision NativeRenderer3DBackend::GetRayCollisionMesh(gfx::Ray ray, gfx::Mesh mesh, gfx::Matrix transform) {
    gfx::RayCollision closest{};
    closest.hit = false;
    float closest_t = 1e30f;
    int tri_count = mesh.indices != nullptr ? mesh.triangleCount : mesh.vertexCount / 3;
    for (int i = 0; i < tri_count; i++) {
        int i0, i1, i2;
        if (mesh.indices != nullptr) {
            i0 = mesh.indices[i * 3 + 0];
            i1 = mesh.indices[i * 3 + 1];
            i2 = mesh.indices[i * 3 + 2];
        } else {
            i0 = i * 3;
            i1 = i * 3 + 1;
            i2 = i * 3 + 2;
        }
        auto vertex_at = [&](int idx) -> gfx::Vector3 {
            gfx::Vector3 v{mesh.vertices[idx * 3], mesh.vertices[idx * 3 + 1], mesh.vertices[idx * 3 + 2]};
            return gfx::Vector3Transform(v, transform);
        };
        gfx::Vector3 v0 = vertex_at(i0), v1 = vertex_at(i1), v2 = vertex_at(i2);
        float t = 0.0f;
        if (RayTriangleIntersect(ray, v0, v1, v2, &t) && t < closest_t) {
            closest_t = t;
            closest.hit = true;
            closest.distance = t;
            closest.point = gfx::Vector3Add(ray.position, gfx::Vector3Scale(ray.direction, t));
            closest.normal = gfx::Vector3Normalize(
                gfx::Vector3CrossProduct(gfx::Vector3Subtract(v1, v0), gfx::Vector3Subtract(v2, v0)));
        }
    }
    return closest;
}

gfx::RayCollision NativeRenderer3DBackend::GetRayCollisionBox(gfx::Ray ray, gfx::BoundingBox box) {
    // Standard slab test.
    float tmin = -1e30f, tmax = 1e30f;
    float origin[3] = {ray.position.x, ray.position.y, ray.position.z};
    float dir[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
    float bmin[3] = {box.min.x, box.min.y, box.min.z};
    float bmax[3] = {box.max.x, box.max.y, box.max.z};
    for (int i = 0; i < 3; i++) {
        if (std::fabs(dir[i]) < 1e-8f) {
            if (origin[i] < bmin[i] || origin[i] > bmax[i]) return gfx::RayCollision{};
            continue;
        }
        float t1 = (bmin[i] - origin[i]) / dir[i];
        float t2 = (bmax[i] - origin[i]) / dir[i];
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return gfx::RayCollision{};
    }
    if (tmax < 0.0f) return gfx::RayCollision{};
    float t = tmin >= 0.0f ? tmin : tmax;
    gfx::RayCollision hit{};
    hit.hit = true;
    hit.distance = t;
    hit.point = gfx::Vector3Add(ray.position, gfx::Vector3Scale(ray.direction, t));
    return hit;
}

// glPolygonMode doesn't exist on OpenGL ES/WebGL (only real desktop GL);
// raylib's own rlEnableWireMode/rlDisableWireMode no-op there for exactly
// this reason (see its rlgl.h), so matching that on Emscripten is full
// parity with this app's existing web build, not a regression.
void NativeRenderer3DBackend::EnableWireMode() {
#ifndef __EMSCRIPTEN__
    gl::PolygonMode(gl::GL_FRONT_AND_BACK, gl::GL_LINE);
#endif
}
void NativeRenderer3DBackend::DisableWireMode() {
#ifndef __EMSCRIPTEN__
    gl::PolygonMode(gl::GL_FRONT_AND_BACK, gl::GL_FILL);
#endif
}

void NativeRenderer3DBackend::PushMatrix() { impl_->matrix_stack.push_back(impl_->matrix_stack.back()); }
void NativeRenderer3DBackend::PopMatrix() {
    if (impl_->matrix_stack.size() > 1) impl_->matrix_stack.pop_back();
}
void NativeRenderer3DBackend::TranslateMatrix(float x, float y, float z) {
    impl_->ComposeIntoTop(gfx::MatrixTranslate(x, y, z));
}
void NativeRenderer3DBackend::MultMatrix(const float *matrix_16_column_major) {
    gfx::Matrix m{};
    std::memcpy(&m, matrix_16_column_major, sizeof(float) * 16);
    impl_->ComposeIntoTop(m);
}

}  // namespace gfx
