#pragma once

// Facade over IRenderer3DBackend (see gfx/backend.h). Backs
// DrawModel3DPane's viewport, Model3DRenderToImageFile, and
// model3d_doc.cpp's import/procgen utility calls.

#include "gfx/backend.h"
#include "gfx/types.h"

namespace gfx {

inline void BeginMode3D(Camera3D camera) { GetBackends().renderer3d->BeginMode3D(camera); }
inline void EndMode3D() { GetBackends().renderer3d->EndMode3D(); }
inline void DrawGrid(int slices, float spacing) { GetBackends().renderer3d->DrawGrid(slices, spacing); }
inline void DrawLine3D(Vector3 start, Vector3 end, Color color) {
    GetBackends().renderer3d->DrawLine3D(start, end, color);
}
inline void DrawCube(Vector3 position, float width, float height, float length, Color color) {
    GetBackends().renderer3d->DrawCube(position, width, height, length, color);
}
inline void DrawSphere(Vector3 center, float radius, Color color) {
    GetBackends().renderer3d->DrawSphere(center, radius, color);
}
inline void DrawCylinderEx(Vector3 start, Vector3 end, float start_radius, float end_radius, int sides,
                            Color color) {
    GetBackends().renderer3d->DrawCylinderEx(start, end, start_radius, end_radius, sides, color);
}
inline void DrawBoundingBox(BoundingBox box, Color color) {
    GetBackends().renderer3d->DrawBoundingBox(box, color);
}

inline Ray GetScreenToWorldRayEx(Vector2 position, Camera3D camera, int width, int height) {
    return GetBackends().renderer3d->GetScreenToWorldRayEx(position, camera, width, height);
}
inline Vector2 GetWorldToScreenEx(Vector3 position, Camera3D camera, int width, int height) {
    return GetBackends().renderer3d->GetWorldToScreenEx(position, camera, width, height);
}

inline Model LoadModel(const char *file_name) { return GetBackends().renderer3d->LoadModel(file_name); }
inline void UnloadModel(Model model) { GetBackends().renderer3d->UnloadModel(model); }
inline void UnloadMesh(Mesh mesh) { GetBackends().renderer3d->UnloadMesh(mesh); }
inline void UploadMesh(Mesh *mesh, bool dynamic) { GetBackends().renderer3d->UploadMesh(mesh, dynamic); }
inline void DrawMesh(Mesh mesh, Material material, Matrix transform) {
    GetBackends().renderer3d->DrawMesh(mesh, material, transform);
}
inline Material LoadMaterialDefault() { return GetBackends().renderer3d->LoadMaterialDefault(); }

inline Mesh GenMeshCube(float width, float height, float length) {
    return GetBackends().renderer3d->GenMeshCube(width, height, length);
}
inline Mesh GenMeshSphere(float radius, int rings, int slices) {
    return GetBackends().renderer3d->GenMeshSphere(radius, rings, slices);
}
inline Mesh GenMeshCylinder(float radius, float height, int slices) {
    return GetBackends().renderer3d->GenMeshCylinder(radius, height, slices);
}
inline Mesh GenMeshCone(float radius, float height, int slices) {
    return GetBackends().renderer3d->GenMeshCone(radius, height, slices);
}
inline Mesh GenMeshPlane(float width, float length, int res_x, int res_z) {
    return GetBackends().renderer3d->GenMeshPlane(width, length, res_x, res_z);
}
inline Mesh GenMeshTorus(float radius, float size, int rad_seg, int sides) {
    return GetBackends().renderer3d->GenMeshTorus(radius, size, rad_seg, sides);
}

inline RayCollision GetRayCollisionMesh(Ray ray, Mesh mesh, Matrix transform) {
    return GetBackends().renderer3d->GetRayCollisionMesh(ray, mesh, transform);
}
inline RayCollision GetRayCollisionBox(Ray ray, BoundingBox box) {
    return GetBackends().renderer3d->GetRayCollisionBox(ray, box);
}

inline void EnableWireMode() { GetBackends().renderer3d->EnableWireMode(); }
inline void DisableWireMode() { GetBackends().renderer3d->DisableWireMode(); }
inline void PushMatrix() { GetBackends().renderer3d->PushMatrix(); }
inline void PopMatrix() { GetBackends().renderer3d->PopMatrix(); }
inline void TranslateMatrix(float x, float y, float z) {
    GetBackends().renderer3d->TranslateMatrix(x, y, z);
}
inline void MultMatrix(const float *matrix_16_column_major) {
    GetBackends().renderer3d->MultMatrix(matrix_16_column_major);
}

}  // namespace gfx
