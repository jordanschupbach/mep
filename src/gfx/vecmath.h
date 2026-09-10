#pragma once

// Vector/matrix math mirroring the subset of raylib's raymath.h actually
// used in src/ (main.cpp's 3D gizmo code, model3d_doc.cpp). Unlike the
// rest of gfx/, this has no backend dependency at all -- it's plain
// arithmetic with no OS/GPU involvement -- so it's implemented directly
// here rather than dispatched through a backend interface. This is
// already fully in-house as of Stage A; Stage B needs no changes here.

#include <cmath>

#include "gfx/types.h"

namespace gfx {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kDeg2Rad = kPi / 180.0f;
inline constexpr float kRad2Deg = 180.0f / kPi;

inline Vector3 Vector3Add(Vector3 a, Vector3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vector3 Vector3Subtract(Vector3 a, Vector3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vector3 Vector3Scale(Vector3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline float Vector3DotProduct(Vector3 a, Vector3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vector3 Vector3CrossProduct(Vector3 a, Vector3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Vector3Length(Vector3 v) { return std::sqrt(Vector3DotProduct(v, v)); }
inline Vector3 Vector3Normalize(Vector3 v) {
    float len = Vector3Length(v);
    if (len == 0.0f) return v;
    return Vector3Scale(v, 1.0f / len);
}
inline float Vector3Distance(Vector3 a, Vector3 b) { return Vector3Length(Vector3Subtract(b, a)); }
inline Vector3 Vector3Min(Vector3 a, Vector3 b) {
    return {std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)};
}
inline Vector3 Vector3Max(Vector3 a, Vector3 b) {
    return {std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)};
}
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
// Rotates `v` about unit axis `axis` by `angle` radians (Rodrigues' formula).
inline Vector3 Vector3RotateByAxisAngle(Vector3 v, Vector3 axis, float angle) {
    axis = Vector3Normalize(axis);
    float s = std::sin(angle), c = std::cos(angle);
    Vector3 term1 = Vector3Scale(v, c);
    Vector3 term2 = Vector3Scale(Vector3CrossProduct(axis, v), s);
    Vector3 term3 = Vector3Scale(axis, Vector3DotProduct(axis, v) * (1.0f - c));
    return Vector3Add(Vector3Add(term1, term2), term3);
}
inline Vector3 Vector3Transform(Vector3 v, Matrix m) {
    return {
        m.m0 * v.x + m.m4 * v.y + m.m8 * v.z + m.m12,
        m.m1 * v.x + m.m5 * v.y + m.m9 * v.z + m.m13,
        m.m2 * v.x + m.m6 * v.y + m.m10 * v.z + m.m14,
    };
}

inline Vector2 Vector2Subtract(Vector2 a, Vector2 b) { return {a.x - b.x, a.y - b.y}; }
inline float Vector2DotProduct(Vector2 a, Vector2 b) { return a.x * b.x + a.y * b.y; }
inline float Vector2Distance(Vector2 a, Vector2 b) {
    return std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
}

inline Matrix MatrixIdentity() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}
inline Matrix MatrixMultiply(Matrix l, Matrix r) {
    // raylib's own convention: result = left * right, applied left-then-right
    // to a column vector (i.e. matches raylib's MatrixMultiply exactly).
    Matrix out{};
    out.m0 = l.m0 * r.m0 + l.m1 * r.m4 + l.m2 * r.m8 + l.m3 * r.m12;
    out.m1 = l.m0 * r.m1 + l.m1 * r.m5 + l.m2 * r.m9 + l.m3 * r.m13;
    out.m2 = l.m0 * r.m2 + l.m1 * r.m6 + l.m2 * r.m10 + l.m3 * r.m14;
    out.m3 = l.m0 * r.m3 + l.m1 * r.m7 + l.m2 * r.m11 + l.m3 * r.m15;
    out.m4 = l.m4 * r.m0 + l.m5 * r.m4 + l.m6 * r.m8 + l.m7 * r.m12;
    out.m5 = l.m4 * r.m1 + l.m5 * r.m5 + l.m6 * r.m9 + l.m7 * r.m13;
    out.m6 = l.m4 * r.m2 + l.m5 * r.m6 + l.m6 * r.m10 + l.m7 * r.m14;
    out.m7 = l.m4 * r.m3 + l.m5 * r.m7 + l.m6 * r.m11 + l.m7 * r.m15;
    out.m8 = l.m8 * r.m0 + l.m9 * r.m4 + l.m10 * r.m8 + l.m11 * r.m12;
    out.m9 = l.m8 * r.m1 + l.m9 * r.m5 + l.m10 * r.m9 + l.m11 * r.m13;
    out.m10 = l.m8 * r.m2 + l.m9 * r.m6 + l.m10 * r.m10 + l.m11 * r.m14;
    out.m11 = l.m8 * r.m3 + l.m9 * r.m7 + l.m10 * r.m11 + l.m11 * r.m15;
    out.m12 = l.m12 * r.m0 + l.m13 * r.m4 + l.m14 * r.m8 + l.m15 * r.m12;
    out.m13 = l.m12 * r.m1 + l.m13 * r.m5 + l.m14 * r.m9 + l.m15 * r.m13;
    out.m14 = l.m12 * r.m2 + l.m13 * r.m6 + l.m14 * r.m10 + l.m15 * r.m14;
    out.m15 = l.m12 * r.m3 + l.m13 * r.m7 + l.m14 * r.m11 + l.m15 * r.m15;
    return out;
}
inline Matrix MatrixTranslate(float x, float y, float z) {
    Matrix m = MatrixIdentity();
    m.m12 = x;
    m.m13 = y;
    m.m14 = z;
    return m;
}
inline Matrix MatrixScale(float x, float y, float z) {
    Matrix m = MatrixIdentity();
    m.m0 = x;
    m.m5 = y;
    m.m10 = z;
    return m;
}
inline Matrix MatrixRotateXYZ(Vector3 angle) {
    Matrix result = MatrixIdentity();
    float cx = std::cos(-angle.x), sx = std::sin(-angle.x);
    float cy = std::cos(-angle.y), sy = std::sin(-angle.y);
    float cz = std::cos(-angle.z), sz = std::sin(-angle.z);
    result.m0 = cy * cz;
    result.m1 = (sx * sy * cz) + (cx * sz);
    result.m2 = -(cx * sy * cz) + (sx * sz);
    result.m4 = -cy * sz;
    result.m5 = -(sx * sy * sz) + (cx * cz);
    result.m6 = (cx * sy * sz) + (sx * cz);
    result.m8 = sy;
    result.m9 = -sx * cy;
    result.m10 = cx * cy;
    return result;
}
// Determinant-adjugate inverse; sufficient for the affine
// (rotation+translation, no shear/projective) matrices this app builds.
inline Matrix MatrixInvert(Matrix m) {
    float a00 = m.m0, a01 = m.m1, a02 = m.m2, a03 = m.m3;
    float a10 = m.m4, a11 = m.m5, a12 = m.m6, a13 = m.m7;
    float a20 = m.m8, a21 = m.m9, a22 = m.m10, a23 = m.m11;
    float a30 = m.m12, a31 = m.m13, a32 = m.m14, a33 = m.m15;

    float b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10;
    float b02 = a00 * a13 - a03 * a10, b03 = a01 * a12 - a02 * a11;
    float b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
    float b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30;
    float b08 = a20 * a33 - a23 * a30, b09 = a21 * a32 - a22 * a31;
    float b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;

    float det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    float inv_det = (det == 0.0f) ? 0.0f : 1.0f / det;

    Matrix out{};
    out.m0 = (a11 * b11 - a12 * b10 + a13 * b09) * inv_det;
    out.m1 = (-a01 * b11 + a02 * b10 - a03 * b09) * inv_det;
    out.m2 = (a31 * b05 - a32 * b04 + a33 * b03) * inv_det;
    out.m3 = (-a21 * b05 + a22 * b04 - a23 * b03) * inv_det;
    out.m4 = (-a10 * b11 + a12 * b08 - a13 * b07) * inv_det;
    out.m5 = (a00 * b11 - a02 * b08 + a03 * b07) * inv_det;
    out.m6 = (-a30 * b05 + a32 * b02 - a33 * b01) * inv_det;
    out.m7 = (a20 * b05 - a22 * b02 + a23 * b01) * inv_det;
    out.m8 = (a10 * b10 - a11 * b08 + a13 * b06) * inv_det;
    out.m9 = (-a00 * b10 + a01 * b08 - a03 * b06) * inv_det;
    out.m10 = (a30 * b04 - a31 * b02 + a33 * b00) * inv_det;
    out.m11 = (-a20 * b04 + a21 * b02 - a23 * b00) * inv_det;
    out.m12 = (-a10 * b09 + a11 * b07 - a12 * b06) * inv_det;
    out.m13 = (a00 * b09 - a01 * b07 + a02 * b06) * inv_det;
    out.m14 = (-a30 * b03 + a31 * b01 - a32 * b00) * inv_det;
    out.m15 = (a20 * b03 - a21 * b01 + a22 * b00) * inv_det;
    return out;
}

// Right-handed perspective projection, symmetric frustum. `fovy` in
// radians. Verified against MatrixMultiply's own convention (see its
// call sites' comments): MatrixMultiply(A, B) means "apply A first, then
// B", so a model-view-projection combine is
// MatrixMultiply(MatrixMultiply(model, view), projection).
inline Matrix MatrixPerspective(float fovy, float aspect, float near_plane, float far_plane) {
    float top = near_plane * std::tan(fovy * 0.5f);
    float right = top * aspect;
    Matrix m{};
    m.m0 = near_plane / right;
    m.m5 = near_plane / top;
    m.m10 = -(far_plane + near_plane) / (far_plane - near_plane);
    m.m11 = -1.0f;
    m.m14 = -(2.0f * far_plane * near_plane) / (far_plane - near_plane);
    return m;
}

// Right-handed orthographic projection (OpenGL NDC z in [-1,1], same
// convention as MatrixPerspective above) -- used by the shadow-map pass's
// directional-light projection (CHESS_REALISM_PLAN.md Phase 3): a
// directional light has no single eye position/FOV, so its "camera" is an
// orthographic box tightly fit around the scene instead of a perspective
// frustum.
inline Matrix MatrixOrtho(float left, float right, float bottom, float top, float near_plane, float far_plane) {
    Matrix m{};
    m.m0 = 2.0f / (right - left);
    m.m5 = 2.0f / (top - bottom);
    m.m10 = -2.0f / (far_plane - near_plane);
    m.m12 = -(right + left) / (right - left);
    m.m13 = -(top + bottom) / (top - bottom);
    m.m14 = -(far_plane + near_plane) / (far_plane - near_plane);
    m.m15 = 1.0f;
    return m;
}

inline Matrix MatrixLookAt(Vector3 eye, Vector3 target, Vector3 up) {
    Vector3 zaxis = Vector3Normalize(Vector3Subtract(eye, target));
    Vector3 xaxis = Vector3Normalize(Vector3CrossProduct(up, zaxis));
    Vector3 yaxis = Vector3CrossProduct(zaxis, xaxis);
    Matrix m{};
    m.m0 = xaxis.x; m.m4 = xaxis.y; m.m8 = xaxis.z; m.m12 = -Vector3DotProduct(xaxis, eye);
    m.m1 = yaxis.x; m.m5 = yaxis.y; m.m9 = yaxis.z; m.m13 = -Vector3DotProduct(yaxis, eye);
    m.m2 = zaxis.x; m.m6 = zaxis.y; m.m10 = zaxis.z; m.m14 = -Vector3DotProduct(zaxis, eye);
    m.m3 = 0; m.m7 = 0; m.m11 = 0; m.m15 = 1;
    return m;
}

inline Color Fade(Color c, float alpha) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return {c.r, c.g, c.b, static_cast<unsigned char>(255.0f * alpha)};
}
inline Color ColorAlpha(Color c, float alpha) { return Fade(c, alpha); }

}  // namespace gfx
