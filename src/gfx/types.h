#pragma once

// Stage A of the raylib removal: mep's own render/platform/input value
// types, deliberately shaped identically (same field names, same layout)
// to the raylib types they replace at every include site in main.cpp,
// editor.cpp, lua_env.cpp and model3d_doc.cpp. That's not accidental
// raylib-flavor -- it's what makes retargeting ~2000 existing call sites
// in those files a mechanical rename instead of a redesign, and it costs
// nothing going into Stage B: raylib's own Font/Mesh/Texture shapes are
// themselves just "what a stb_truetype atlas / GL buffer / GL texture
// naturally look like", so the in-house backend fills these same fields
// from its own gfx::tt (see gfx/truetype.h)/OpenGL calls instead of
// raylib's.
//
// Nothing in this header (or anywhere else in the project, as of Stage
// B10) includes raylib.h -- gfx/backend_native.cpp and its siblings are
// hand-rolled GLFW/OpenGL/ALSA, no raylib or miniaudio left to wrap
// (see MINIAUDIO_REMOVAL_PLAN.md for the latter).

#include <cstddef>

namespace gfx {

struct Vector2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Rectangle {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct Color {
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
    unsigned char a = 255;
};

// Column-major 4x4, same field layout/naming as raylib's Matrix so the
// gizmo/camera math in main.cpp/model3d_doc.cpp ports unchanged.
struct Matrix {
    float m0, m4, m8, m12;
    float m1, m5, m9, m13;
    float m2, m6, m10, m14;
    float m3, m7, m11, m15;
};

// -- Textures / images / render targets -------------------------------

struct Texture2D {
    unsigned int id = 0;
    int width = 0;
    int height = 0;
    int mipmaps = 1;
    int format = 0;
};

struct Image {
    void *data = nullptr;
    int width = 0;
    int height = 0;
    int mipmaps = 1;
    int format = 0;
};

struct RenderTexture2D {
    unsigned int id = 0;
    Texture2D texture;
    Texture2D depth;
};

enum class TextureFilter { Point, Bilinear };

// -- Fonts / glyphs -----------------------------------------------------

struct GlyphInfo {
    int value = 0;
    int offsetX = 0;
    int offsetY = 0;
    int advanceX = 0;
    Image image;
};

struct Font {
    int baseSize = 0;
    int glyphCount = 0;
    int glyphPadding = 0;
    Texture2D texture;
    Rectangle *recs = nullptr;
    GlyphInfo *glyphs = nullptr;
};

// -- 3D: mesh / material / model / camera / ray -------------------------

// Same field set/order as raylib's Mesh so model3d_doc.cpp's raylib-based
// import path and main.cpp's GPU mesh cache both port field-for-field.
struct Mesh {
    int vertexCount = 0;
    int triangleCount = 0;
    float *vertices = nullptr;
    float *texcoords = nullptr;
    float *texcoords2 = nullptr;
    float *normals = nullptr;
    float *tangents = nullptr;
    unsigned char *colors = nullptr;
    // 32-bit, not 16 (plans/CAD_FEM_PLAN.md Part 0.4). The narrower type
    // capped an indexed mesh at 65,536 vertices, which every consumer in
    // this tree worked around the same way: by giving up on indexing and
    // uploading a flat triangle list instead (see the OBJ and VOX
    // importers' own notes, and BuildModel3DGpuMesh in main.cpp). That
    // costs three vertices per triangle where one would do, and it
    // discards the vertex *sharing* that a per-vertex field needs to
    // interpolate smoothly across a surface -- which is exactly how a FEM
    // result gets drawn (Part J.2). A finite-element mesh passes 65,536
    // vertices almost immediately, so the cap had to go either way.
    unsigned int *indices = nullptr;
    float *animVertices = nullptr;
    float *animNormals = nullptr;
    unsigned char *boneIds = nullptr;
    float *boneWeights = nullptr;
    Matrix *boneMatrices = nullptr;
    int boneCount = 0;
    unsigned int vaoId = 0;
    unsigned int *vboId = nullptr;
};

constexpr int kMaterialMapAlbedo = 0;
// Metalness/normal/roughness: CHESS_SET_BENCHMARK_PLAN.md's Phase 1 basic-
// PBR material slots. Index values match raylib's own MATERIAL_MAP_*
// convention (not load-bearing here, just familiar) -- each map's
// MaterialMap::value carries the scalar roughness/metalness/normal-map-
// strength used when no texture is bound, or as a multiplier when one is.
constexpr int kMaterialMapMetalness = 1;
constexpr int kMaterialMapNormal = 2;
constexpr int kMaterialMapRoughness = 3;
constexpr int kMaxMaterialMaps = 12;

// Image::format is an opaque backend-defined pixel-format id (raylib's
// PixelFormat enum in the Stage A backend) -- this is the one value
// src/ ever constructs directly (SaveImageEditorPng's raw RGBA8 export),
// so it's named here rather than left as a raylib magic number. Must
// match whatever the active backend's ExportImage expects for "8-bit
// RGBA, no compression".
constexpr int kPixelFormatR8G8B8A8 = 7;

struct MaterialMap {
    Texture2D texture;
    Color color;
    float value = 0.0f;
};

// Deliberately opaque beyond `maps`: main.cpp reads/writes
// material.maps[kMaterialMapAlbedo/Normal/Roughness/Metalness]
// (CHESS_SET_BENCHMARK_PLAN.md's Phase 1 basic-PBR shading), but never
// touches `shader`/`params` themselves -- those stay backend-internal.
struct Material {
    MaterialMap *maps = nullptr;
    void *backend_shader = nullptr;
    float params[4] = {0, 0, 0, 0};
};

struct Model {
    Matrix transform{};
    int meshCount = 0;
    int materialCount = 0;
    Mesh *meshes = nullptr;
    Material *materials = nullptr;
    int *meshMaterial = nullptr;
};

enum class CameraProjection { Perspective, Orthographic };

struct Camera3D {
    Vector3 position;
    Vector3 target;
    Vector3 up;
    float fovy = 45.0f;
    CameraProjection projection = CameraProjection::Perspective;
};

// One light for the mesh shader's multi-light loop (MULTILIGHT_ANIMATION_
// PLAN.md Part A) -- deliberately its own small POD here rather than
// model3d_doc.h's Light, matching this header's usual "gfx:: doesn't know
// about the document model" layering; NativeRenderer3DBackend::
// SetSceneLights (called once per frame, mirroring SetUnlitMode's own
// per-frame-until-changed statefulness) is where a Scene's lights get
// converted into these. `direction_or_position` is a world-space
// direction (Directional, normalized before uploading) or world-space
// position (Point) depending on `type` -- same dual-purpose-by-type shape
// model3d_doc.h's own Light uses. `color` is 0..1 float (not the 0..255
// gfx::Color), since intensity multiplies it and can push components
// above 1.0 for a genuinely bright light.
enum class LightType { Directional, Point };
struct SceneLight {
    LightType type = LightType::Directional;
    Vector3 direction_or_position;
    Vector3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;  // Point lights only
};

struct Ray {
    Vector3 position;
    Vector3 direction;
};

struct RayCollision {
    bool hit = false;
    float distance = 0.0f;
    Vector3 point;
    Vector3 normal;
};

struct BoundingBox {
    Vector3 min;
    Vector3 max;
};

// -- Audio ----------------------------------------------------------------

struct Sound {
    void *backend_handle = nullptr;
    // Mirrors raylib's Sound::frameCount: 0 after a failed LoadSound, so
    // callers can check load success without a separate IsReady query.
    unsigned int frameCount = 0;
};

// -- Input: keys/buttons/cursor -----------------------------------------
//
// Deliberately its own enum, not raylib's KeyboardKey values reused --
// every backend (raylib today, GLFW-direct in Stage B) translates its own
// native key codes into these, so gfx::Key never depends on either
// backend's numbering. Covers exactly the KEY_* constants referenced
// anywhere in src/ today (see A2's retargeting of editor.cpp/main.cpp).
//
// A-Z and Zero-Nine are each a full, contiguous run (not just the
// letters/digits referenced by name elsewhere in src/): a few call sites
// iterate the whole alphabet or digit range with operator++ (Alt-letter
// leader-key dispatch, Alt-1..9 workspace switching), the same way
// raylib's own KEY_A..KEY_Z/KEY_ZERO..KEY_NINE are contiguous ASCII
// ranges today.
enum class Key {
    None = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Zero, One, Two, Three, Four, Five, Six, Seven, Eight, Nine,
    Backslash, Backspace, Delete, Down, End, Enter, Equal, Escape,
    Home, Insert, KpEnter, Left, LeftAlt, LeftBracket, LeftControl,
    LeftShift, LeftSuper, Minus, PageDown, PageUp, Right, RightAlt,
    RightBracket, RightControl, RightShift, RightSuper, Tab, Up,
};

inline Key operator++(Key &k, int) {
    Key old = k;
    k = static_cast<Key>(static_cast<int>(k) + 1);
    return old;
}
inline bool operator<(Key a, Key b) { return static_cast<int>(a) < static_cast<int>(b); }
inline bool operator<=(Key a, Key b) { return static_cast<int>(a) <= static_cast<int>(b); }
inline bool operator>(Key a, Key b) { return static_cast<int>(a) > static_cast<int>(b); }
inline bool operator>=(Key a, Key b) { return static_cast<int>(a) >= static_cast<int>(b); }
inline int operator-(Key a, Key b) { return static_cast<int>(a) - static_cast<int>(b); }

enum class MouseButton { Left, Right, Middle };

enum class MouseCursor { Default, PointingHand, ResizeEw, ResizeNs };

// Only window-resizable is ever requested (see main.cpp's SetConfigFlags
// call) -- kept as a named bool rather than a raylib-style flags bitmask
// since nothing else in this app sets a second flag alongside it.

// Colors used as named constants anywhere in src/ (WHITE/BLACK/etc. --
// raylib's own CLITERAL(Color){...} values, reproduced exactly).
inline constexpr Color White{255, 255, 255, 255};
inline constexpr Color Black{0, 0, 0, 255};
inline constexpr Color Blank{0, 0, 0, 0};
inline constexpr Color Yellow{253, 249, 0, 255};
inline constexpr Color Red{230, 41, 55, 255};
inline constexpr Color Green{0, 228, 48, 255};
inline constexpr Color Blue{0, 121, 241, 255};

// Backend diagnostic log levels -- passed through to the callback
// installed via Platform::SetTraceLogCallback (raylib's own TraceLogLevel
// ordinals in the Stage A backend). Named here so main.cpp's log-file
// callback doesn't need to know which backend supplied them.
constexpr int kLogTrace = 1;
constexpr int kLogDebug = 2;
constexpr int kLogInfo = 3;
constexpr int kLogWarning = 4;
constexpr int kLogError = 5;
constexpr int kLogFatal = 6;

}  // namespace gfx
