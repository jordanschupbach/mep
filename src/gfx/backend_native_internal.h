#pragma once

// Internal wiring shared by gfx/backend_native.cpp and its
// renderer2d/text implementation files (backend_native_renderer2d.cpp,
// backend_native_text.cpp) -- not part of gfx/'s public API, just split
// out so those two substantial implementations don't have to live
// inline inside backend_native.cpp itself.

#include "gfx/backend.h"

struct GLFWwindow;

namespace gfx {

// The same NativeContext backend_native.cpp's Platform/Input classes use
// (defined there); forward-declared here so the renderer2d/text classes
// below can take a pointer to it without a circular include. Only
// `window` is actually touched from outside backend_native.cpp today
// (for framebuffer size queries).
struct NativeContext;
GLFWwindow *NativeContextWindow(NativeContext *ctx);

class NativeAudioBackend : public IAudioBackend {
public:
    NativeAudioBackend();
    ~NativeAudioBackend() override;
    NativeAudioBackend(const NativeAudioBackend &) = delete;
    NativeAudioBackend &operator=(const NativeAudioBackend &) = delete;

    void InitAudioDevice() override;
    bool IsAudioDeviceReady() override;
    Sound LoadSound(const char *file_name) override;
    void UnloadSound(Sound sound) override;
    void PlaySound(Sound sound) override;
    void PauseSound(Sound sound) override;
    void ResumeSound(Sound sound) override;
    bool IsSoundPlaying(Sound sound) override;
    void SetSoundVolume(Sound sound, float volume) override;

private:
    struct Impl;
    Impl *impl_;
};

class NativeRenderer2DBackend : public IRenderer2DBackend {
public:
    explicit NativeRenderer2DBackend(NativeContext *ctx);
    ~NativeRenderer2DBackend() override;
    NativeRenderer2DBackend(const NativeRenderer2DBackend &) = delete;
    NativeRenderer2DBackend &operator=(const NativeRenderer2DBackend &) = delete;

    void BeginDrawing() override;
    void EndDrawing() override;
    void ClearBackground(Color color) override;
    void BeginScissorMode(int x, int y, int width, int height) override;
    void EndScissorMode() override;

    void DrawRectangle(int x, int y, int width, int height, Color color) override;
    void DrawRectangleRec(Rectangle rec, Color color) override;
    void DrawRectangleGradientEx(Rectangle rec, Color top_left, Color bottom_left, Color top_right,
                                  Color bottom_right) override;
    void DrawRectangleLines(int x, int y, int width, int height, Color color) override;
    void DrawRectangleLinesEx(Rectangle rec, float line_thick, Color color) override;
    void DrawRectangleRounded(Rectangle rec, float roundness, int segments, Color color) override;
    void DrawRectangleRoundedLines(Rectangle rec, float roundness, int segments, Color color) override;
    void DrawRectangleRoundedLinesEx(Rectangle rec, float roundness, int segments, float line_thick,
                                      Color color) override;
    void DrawEllipseLines(int center_x, int center_y, float radius_h, float radius_v, Color color) override;
    void DrawLine(int start_x, int start_y, int end_x, int end_y, Color color) override;
    void DrawLineEx(Vector2 start, Vector2 end, float thick, Color color) override;
    void DrawCircle(int center_x, int center_y, float radius, Color color) override;
    void DrawCircleV(Vector2 center, float radius, Color color) override;
    void DrawCircleLines(int center_x, int center_y, float radius, Color color) override;
    void DrawRing(Vector2 center, float inner_radius, float outer_radius, float start_angle, float end_angle,
                  int segments, Color color) override;
    void DrawTriangle(Vector2 v1, Vector2 v2, Vector2 v3, Color color) override;
    void DrawTriangleFan(const Vector2 *points, int point_count, Color color) override;

    bool CheckCollisionPointRec(Vector2 point, Rectangle rec) override;

    Image LoadImage(const char *file_name) override;
    Image GenImageColor(int width, int height, Color color) override;
    Image ImageFromImage(Image image, Rectangle rec) override;
    void ImageFormat(Image *image, int new_format) override;
    void ImageFlipVertical(Image *image) override;
    void ImageCrop(Image *image, Rectangle crop) override;
    void UnloadImage(Image image) override;
    bool ExportImage(Image image, const char *file_name) override;
    std::vector<unsigned char> ExportImageToMemory(Image image, const char *file_type) override;
    Vector3 ColorToHSV(Color color) override;
    Color ColorFromHSV(float hue, float saturation, float value) override;

    Texture2D LoadTextureFromImage(Image image) override;
    void UpdateTexture(Texture2D texture, const void *pixels) override;
    void UnloadTexture(Texture2D texture) override;
    void DrawTextureEx(Texture2D texture, Vector2 position, float rotation, float scale, Color tint) override;
    void DrawTextureRec(Texture2D texture, Rectangle source, Vector2 position, Color tint) override;
    void DrawTexturePro(Texture2D texture, Rectangle source, Rectangle dest, Vector2 origin, float rotation,
                         Color tint) override;

    RenderTexture2D LoadRenderTexture(int width, int height) override;
    void UnloadRenderTexture(RenderTexture2D target) override;
    void BeginTextureMode(RenderTexture2D target) override;
    void EndTextureMode() override;
    Image LoadImageFromTexture(Texture2D texture) override;
    Image LoadImageFromScreen() override;
    unsigned char *ReadScreenPixels(int width, int height) override;

private:
    void EnsureInit();

    struct Impl;
    Impl *impl_;
};

class NativeTextBackend : public ITextBackend {
public:
    explicit NativeTextBackend(NativeRenderer2DBackend *renderer2d);
    ~NativeTextBackend() override;
    NativeTextBackend(const NativeTextBackend &) = delete;
    NativeTextBackend &operator=(const NativeTextBackend &) = delete;

    Font LoadFontFromMemory(const char *file_type, const unsigned char *file_data, int data_size, int font_size,
                             int *codepoints, int codepoint_count) override;
    void UnloadFont(Font font) override;
    void SetTextureFilter(Texture2D texture, TextureFilter filter) override;
    GlyphInfo *LoadFontData(const unsigned char *file_data, int data_size, int font_size, int *codepoints,
                             int codepoint_count) override;
    Image GenImageFontAtlas(const GlyphInfo *glyphs, Rectangle **glyph_recs, int glyph_count, int font_size,
                             int padding) override;
    void UnloadFontData(GlyphInfo *glyphs, int glyph_count) override;
    void FreeGlyphRects(Rectangle *recs) override;
    int GetGlyphIndex(Font font, int codepoint) override;
    Vector2 MeasureTextEx(Font font, const char *text, float font_size, float spacing) override;
    void DrawTextEx(Font font, const char *text, Vector2 position, float font_size, float spacing,
                     Color tint) override;
    void DrawTextCodepoint(Font font, int codepoint, Vector2 position, float font_size, Color tint) override;
    int GetCodepointNext(const char *text, int *codepoint_size) override;

private:
    struct Impl;
    Impl *impl_;
};

class NativeRenderer3DBackend : public IRenderer3DBackend {
public:
    explicit NativeRenderer3DBackend(NativeContext *ctx);
    ~NativeRenderer3DBackend() override;
    NativeRenderer3DBackend(const NativeRenderer3DBackend &) = delete;
    NativeRenderer3DBackend &operator=(const NativeRenderer3DBackend &) = delete;

    void BeginMode3D(Camera3D camera) override;
    void EndMode3D() override;
    void DrawGrid(int slices, float spacing) override;
    void DrawLine3D(Vector3 start, Vector3 end, Color color) override;
    void DrawCube(Vector3 position, float width, float height, float length, Color color) override;
    void DrawSphere(Vector3 center, float radius, Color color) override;
    void DrawCylinderEx(Vector3 start, Vector3 end, float start_radius, float end_radius, int sides,
                         Color color) override;
    void DrawBoundingBox(BoundingBox box, Color color) override;

    Ray GetScreenToWorldRayEx(Vector2 position, Camera3D camera, int width, int height) override;
    Vector2 GetWorldToScreenEx(Vector3 position, Camera3D camera, int width, int height) override;

    Model LoadModel(const char *file_name) override;
    void UnloadModel(Model model) override;
    void UnloadMesh(Mesh mesh) override;
    void UploadMesh(Mesh *mesh, bool dynamic) override;
    void DrawMesh(Mesh mesh, Material material, Matrix transform) override;
    Material LoadMaterialDefault() override;

    Mesh GenMeshCube(float width, float height, float length) override;
    Mesh GenMeshSphere(float radius, int rings, int slices) override;
    Mesh GenMeshCylinder(float radius, float height, int slices) override;
    Mesh GenMeshCone(float radius, float height, int slices) override;
    Mesh GenMeshPlane(float width, float length, int res_x, int res_z) override;
    Mesh GenMeshTorus(float radius, float size, int rad_seg, int sides) override;

    RayCollision GetRayCollisionMesh(Ray ray, Mesh mesh, Matrix transform) override;
    RayCollision GetRayCollisionBox(Ray ray, BoundingBox box) override;

    void EnableWireMode() override;
    void DisableWireMode() override;
    void PushMatrix() override;
    void PopMatrix() override;
    void TranslateMatrix(float x, float y, float z) override;
    void MultMatrix(const float *matrix_16_column_major) override;

private:
    struct Impl;
    Impl *impl_;
};

}  // namespace gfx
