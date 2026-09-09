#pragma once

// The Implementor side of the bridge: one small abstract interface per
// subsystem (platform/window, input, audio, 2D rendering, text, 3D
// rendering). gfx/backend_native.cpp implements all six, on GLFW+OpenGL+
// an in-house ALSA audio backend (this app's raylib dependency was fully
// removed in Stage B10 -- see PLAN; miniaudio was later removed too, see
// MINIAUDIO_REMOVAL_PLAN.md). Everything in src/ outside gfx/ talks only to the
// free-function facades in gfx/platform.h, gfx/input.h, etc. -- never to
// these interfaces or to a concrete backend directly.

#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

#include "gfx/types.h"

namespace gfx {

class IPlatformBackend {
public:
    virtual ~IPlatformBackend() = default;
    virtual void InitWindow(int width, int height, const char *title) = 0;
    virtual void CloseWindow() = 0;
    virtual bool IsWindowReady() = 0;
    virtual bool WindowShouldClose() = 0;
    virtual void SetWindowResizable() = 0;
    virtual void MaximizeWindow() = 0;
    virtual void SetTargetFPS(int fps) = 0;
    virtual int GetScreenWidth() = 0;
    virtual int GetScreenHeight() = 0;
    virtual double GetTime() = 0;
    virtual float GetFrameTime() = 0;
    virtual void SetExitKey(Key key) = 0;  // Key::None disables the default Escape-quits-app behavior
    virtual std::string GetClipboardText() = 0;
    virtual void SetClipboardText(const std::string &text) = 0;
    virtual void SetMouseCursor(MouseCursor cursor) = 0;
    // Native window handle -- a GLFWwindow*, same as raylib's
    // GetWindowHandle() today -- passed straight through to
    // agent_ui_input.cpp's glfwGetX11Window() call, unchanged in both
    // Stage A (raylib-owned GLFW window) and Stage B (mep-owned one).
    virtual void *GetNativeWindowHandle() = 0;

    // Redirects the backend's own internal diagnostic logging (raylib's
    // TraceLog calls today -- texture/font/shader load chatter) to a
    // caller-supplied sink instead of stdout; see main.cpp's
    // SetUpTraceLogFile for why (thousands of lines of icon-font glyph
    // warnings per launch otherwise). `log_level` is one of the
    // gfx::kLog* constants in gfx/types.h.
    using TraceLogCallback = void (*)(int log_level, const char *text, va_list args);
    virtual void SetTraceLogCallback(TraceLogCallback callback) = 0;
};

class IInputBackend {
public:
    virtual ~IInputBackend() = default;
    virtual bool IsKeyPressed(Key key) = 0;
    virtual bool IsKeyPressedRepeat(Key key) = 0;
    virtual bool IsKeyDown(Key key) = 0;
    virtual bool IsKeyReleased(Key key) = 0;
    virtual Key GetKeyPressed() = 0;    // drains one queued key-down event per call, Key::None when empty
    virtual int GetCharPressed() = 0;   // drains one queued Unicode codepoint per call, 0 when empty
    virtual bool IsMouseButtonPressed(MouseButton button) = 0;
    virtual bool IsMouseButtonDown(MouseButton button) = 0;
    virtual bool IsMouseButtonReleased(MouseButton button) = 0;
    virtual Vector2 GetMousePosition() = 0;
    virtual Vector2 GetMouseWheelMoveV() = 0;
};

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    virtual void InitAudioDevice() = 0;
    virtual bool IsAudioDeviceReady() = 0;
    virtual Sound LoadSound(const char *file_name) = 0;
    virtual void UnloadSound(Sound sound) = 0;
    virtual void PlaySound(Sound sound) = 0;
    virtual void PauseSound(Sound sound) = 0;
    virtual void ResumeSound(Sound sound) = 0;
    virtual bool IsSoundPlaying(Sound sound) = 0;
    virtual void SetSoundVolume(Sound sound, float volume) = 0;
};

class ITextBackend {
public:
    virtual ~ITextBackend() = default;
    virtual Font LoadFontFromMemory(const char *file_type, const unsigned char *file_data, int data_size,
                                     int font_size, int *codepoints, int codepoint_count) = 0;
    virtual void UnloadFont(Font font) = 0;
    virtual void SetTextureFilter(Texture2D texture, TextureFilter filter) = 0;
    // Split CPU-bake/GPU-upload pair backing main.cpp's icon-font async
    // bake: LoadFontData+GenImageFontAtlas do no GL calls (safe off the
    // main thread), LoadTextureFromImage below does the GPU upload and
    // must run on the main/GL thread.
    virtual GlyphInfo *LoadFontData(const unsigned char *file_data, int data_size, int font_size,
                                     int *codepoints, int codepoint_count) = 0;
    virtual Image GenImageFontAtlas(const GlyphInfo *glyphs, Rectangle **glyph_recs, int glyph_count,
                                     int font_size, int padding) = 0;
    // Frees the CPU-side glyph/rect arrays LoadFontData/GenImageFontAtlas
    // allocated, for the (never actually reached, but kept for exact
    // parity) discard path in main.cpp's icon-font bake finalize.
    virtual void UnloadFontData(GlyphInfo *glyphs, int glyph_count) = 0;
    virtual void FreeGlyphRects(Rectangle *recs) = 0;
    virtual int GetGlyphIndex(Font font, int codepoint) = 0;
    virtual Vector2 MeasureTextEx(Font font, const char *text, float font_size, float spacing) = 0;
    virtual void DrawTextEx(Font font, const char *text, Vector2 position, float font_size, float spacing,
                             Color tint) = 0;
    virtual void DrawTextCodepoint(Font font, int codepoint, Vector2 position, float font_size,
                                    Color tint) = 0;
    virtual int GetCodepointNext(const char *text, int *codepoint_size) = 0;
};

class IRenderer2DBackend {
public:
    virtual ~IRenderer2DBackend() = default;
    virtual void BeginDrawing() = 0;
    virtual void EndDrawing() = 0;
    virtual void ClearBackground(Color color) = 0;
    virtual void BeginScissorMode(int x, int y, int width, int height) = 0;
    virtual void EndScissorMode() = 0;

    virtual void DrawRectangle(int x, int y, int width, int height, Color color) = 0;
    virtual void DrawRectangleRec(Rectangle rec, Color color) = 0;
    virtual void DrawRectangleGradientEx(Rectangle rec, Color top_left, Color bottom_left, Color top_right,
                                          Color bottom_right) = 0;
    virtual void DrawRectangleLines(int x, int y, int width, int height, Color color) = 0;
    virtual void DrawRectangleLinesEx(Rectangle rec, float line_thick, Color color) = 0;
    virtual void DrawRectangleRounded(Rectangle rec, float roundness, int segments, Color color) = 0;
    virtual void DrawRectangleRoundedLines(Rectangle rec, float roundness, int segments, Color color) = 0;
    virtual void DrawRectangleRoundedLinesEx(Rectangle rec, float roundness, int segments, float line_thick,
                                              Color color) = 0;
    virtual void DrawEllipseLines(int center_x, int center_y, float radius_h, float radius_v, Color color) = 0;
    virtual void DrawLine(int start_x, int start_y, int end_x, int end_y, Color color) = 0;
    virtual void DrawLineEx(Vector2 start, Vector2 end, float thick, Color color) = 0;
    virtual void DrawCircle(int center_x, int center_y, float radius, Color color) = 0;
    virtual void DrawCircleV(Vector2 center, float radius, Color color) = 0;
    virtual void DrawCircleLines(int center_x, int center_y, float radius, Color color) = 0;
    virtual void DrawRing(Vector2 center, float inner_radius, float outer_radius, float start_angle,
                           float end_angle, int segments, Color color) = 0;
    virtual void DrawTriangle(Vector2 v1, Vector2 v2, Vector2 v3, Color color) = 0;
    virtual void DrawTriangleFan(const Vector2 *points, int point_count, Color color) = 0;

    virtual bool CheckCollisionPointRec(Vector2 point, Rectangle rec) = 0;

    virtual Image LoadImage(const char *file_name) = 0;
    virtual Image GenImageColor(int width, int height, Color color) = 0;
    virtual Image ImageFromImage(Image image, Rectangle rec) = 0;
    virtual void ImageFormat(Image *image, int new_format) = 0;
    virtual void ImageFlipVertical(Image *image) = 0;
    virtual void ImageCrop(Image *image, Rectangle crop) = 0;
    virtual void UnloadImage(Image image) = 0;
    virtual bool ExportImage(Image image, const char *file_name) = 0;
    // Encodes to real image bytes in memory (PNG only -- model3d_doc.cpp's
    // glTF exporter is the only caller, embedding a texture as a data:
    // URI) rather than writing to disk. Empty vector on failure; unlike
    // raylib's own ExportImageToMemory (a malloc'd buffer the caller must
    // MemFree), this owns its own buffer so there's no separate free call.
    virtual std::vector<unsigned char> ExportImageToMemory(Image image, const char *file_type) = 0;
    virtual Vector3 ColorToHSV(Color color) = 0;
    virtual Color ColorFromHSV(float hue, float saturation, float value) = 0;

    virtual Texture2D LoadTextureFromImage(Image image) = 0;
    virtual void UpdateTexture(Texture2D texture, const void *pixels) = 0;
    virtual void UnloadTexture(Texture2D texture) = 0;
    virtual void DrawTextureEx(Texture2D texture, Vector2 position, float rotation, float scale,
                                Color tint) = 0;
    virtual void DrawTextureRec(Texture2D texture, Rectangle source, Vector2 position, Color tint) = 0;
    virtual void DrawTexturePro(Texture2D texture, Rectangle source, Rectangle dest, Vector2 origin,
                                 float rotation, Color tint) = 0;

    virtual RenderTexture2D LoadRenderTexture(int width, int height) = 0;
    virtual void UnloadRenderTexture(RenderTexture2D target) = 0;
    virtual void BeginTextureMode(RenderTexture2D target) = 0;
    virtual void EndTextureMode() = 0;
    virtual Image LoadImageFromTexture(Texture2D texture) = 0;
    virtual Image LoadImageFromScreen() = 0;
    // Raw RGBA8 screen readback (backs the mep_screenshot RPC) --
    // raylib's rlReadScreenPixels today, glReadPixels directly in Stage B.
    virtual unsigned char *ReadScreenPixels(int width, int height) = 0;
};

class IRenderer3DBackend {
public:
    virtual ~IRenderer3DBackend() = default;
    virtual void BeginMode3D(Camera3D camera) = 0;
    virtual void EndMode3D() = 0;
    virtual void DrawGrid(int slices, float spacing) = 0;
    virtual void DrawLine3D(Vector3 start, Vector3 end, Color color) = 0;
    virtual void DrawCube(Vector3 position, float width, float height, float length, Color color) = 0;
    virtual void DrawSphere(Vector3 center, float radius, Color color) = 0;
    virtual void DrawCylinderEx(Vector3 start, Vector3 end, float start_radius, float end_radius, int sides,
                                 Color color) = 0;
    virtual void DrawBoundingBox(BoundingBox box, Color color) = 0;

    // Camera-space projection helpers for gizmo/picking math (mouse
    // position <-> world-space ray, world position -> screen position).
    virtual Ray GetScreenToWorldRayEx(Vector2 position, Camera3D camera, int width, int height) = 0;
    virtual Vector2 GetWorldToScreenEx(Vector3 position, Camera3D camera, int width, int height) = 0;

    virtual Model LoadModel(const char *file_name) = 0;
    virtual void UnloadModel(Model model) = 0;
    virtual void UnloadMesh(Mesh mesh) = 0;
    virtual void UploadMesh(Mesh *mesh, bool dynamic) = 0;
    virtual void DrawMesh(Mesh mesh, Material material, Matrix transform) = 0;
    virtual Material LoadMaterialDefault() = 0;

    virtual Mesh GenMeshCube(float width, float height, float length) = 0;
    virtual Mesh GenMeshSphere(float radius, int rings, int slices) = 0;
    virtual Mesh GenMeshCylinder(float radius, float height, int slices) = 0;
    virtual Mesh GenMeshCone(float radius, float height, int slices) = 0;
    virtual Mesh GenMeshPlane(float width, float length, int res_x, int res_z) = 0;
    virtual Mesh GenMeshTorus(float radius, float size, int rad_seg, int sides) = 0;

    virtual RayCollision GetRayCollisionMesh(Ray ray, Mesh mesh, Matrix transform) = 0;
    virtual RayCollision GetRayCollisionBox(Ray ray, BoundingBox box) = 0;

    virtual void EnableWireMode() = 0;
    virtual void DisableWireMode() = 0;
    // Matrix-stack shim: ports main.cpp's rlPushMatrix/rlTranslatef/
    // rlMultMatrixf/rlPopMatrix gizmo-overlay code unchanged in Stage A.
    // Candidate for a cleanup (explicit transform args to DrawMesh/
    // DrawLine3D instead) once Stage B no longer needs 1:1 fidelity.
    virtual void PushMatrix() = 0;
    virtual void PopMatrix() = 0;
    virtual void TranslateMatrix(float x, float y, float z) = 0;
    virtual void MultMatrix(const float *matrix_16_column_major) = 0;
};

struct Backends {
    IPlatformBackend *platform = nullptr;
    IInputBackend *input = nullptr;
    IAudioBackend *audio = nullptr;
    ITextBackend *text = nullptr;
    IRenderer2DBackend *renderer2d = nullptr;
    IRenderer3DBackend *renderer3d = nullptr;
};

// Installs the active backend set. Must be called once, before any
// gfx::* facade function, from main() before InitWindow. Ownership stays
// with the caller (main() keeps the concrete backend objects alive for
// the process lifetime, same as raylib's own global state today).
void SetBackends(const Backends &backends);
const Backends &GetBackends();

}  // namespace gfx
