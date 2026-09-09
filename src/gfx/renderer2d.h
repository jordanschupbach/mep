#pragma once

// Facade over IRenderer2DBackend (see gfx/backend.h). Covers the ~86
// Draw* chrome/pane functions in main.cpp -- 2D primitives, scissoring,
// textures, render targets, and the raw screen readback backing the
// mep_screenshot RPC.

#include "gfx/backend.h"
#include "gfx/types.h"

namespace gfx {

inline void BeginDrawing() { GetBackends().renderer2d->BeginDrawing(); }
inline void EndDrawing() { GetBackends().renderer2d->EndDrawing(); }
inline void ClearBackground(Color color) { GetBackends().renderer2d->ClearBackground(color); }
inline void BeginScissorMode(int x, int y, int width, int height) {
    GetBackends().renderer2d->BeginScissorMode(x, y, width, height);
}
inline void EndScissorMode() { GetBackends().renderer2d->EndScissorMode(); }

inline void DrawRectangle(int x, int y, int width, int height, Color color) {
    GetBackends().renderer2d->DrawRectangle(x, y, width, height, color);
}
inline void DrawRectangleRec(Rectangle rec, Color color) {
    GetBackends().renderer2d->DrawRectangleRec(rec, color);
}
inline void DrawRectangleGradientEx(Rectangle rec, Color top_left, Color bottom_left, Color top_right,
                                     Color bottom_right) {
    GetBackends().renderer2d->DrawRectangleGradientEx(rec, top_left, bottom_left, top_right, bottom_right);
}
inline void DrawRectangleLines(int x, int y, int width, int height, Color color) {
    GetBackends().renderer2d->DrawRectangleLines(x, y, width, height, color);
}
inline void DrawRectangleLinesEx(Rectangle rec, float line_thick, Color color) {
    GetBackends().renderer2d->DrawRectangleLinesEx(rec, line_thick, color);
}
inline void DrawRectangleRounded(Rectangle rec, float roundness, int segments, Color color) {
    GetBackends().renderer2d->DrawRectangleRounded(rec, roundness, segments, color);
}
inline void DrawRectangleRoundedLines(Rectangle rec, float roundness, int segments, Color color) {
    GetBackends().renderer2d->DrawRectangleRoundedLines(rec, roundness, segments, color);
}
inline void DrawRectangleRoundedLinesEx(Rectangle rec, float roundness, int segments, float line_thick,
                                         Color color) {
    GetBackends().renderer2d->DrawRectangleRoundedLinesEx(rec, roundness, segments, line_thick, color);
}
inline void DrawEllipseLines(int center_x, int center_y, float radius_h, float radius_v, Color color) {
    GetBackends().renderer2d->DrawEllipseLines(center_x, center_y, radius_h, radius_v, color);
}
inline void DrawLine(int start_x, int start_y, int end_x, int end_y, Color color) {
    GetBackends().renderer2d->DrawLine(start_x, start_y, end_x, end_y, color);
}
inline void DrawLineEx(Vector2 start, Vector2 end, float thick, Color color) {
    GetBackends().renderer2d->DrawLineEx(start, end, thick, color);
}
inline void DrawCircle(int center_x, int center_y, float radius, Color color) {
    GetBackends().renderer2d->DrawCircle(center_x, center_y, radius, color);
}
inline void DrawCircleV(Vector2 center, float radius, Color color) {
    GetBackends().renderer2d->DrawCircleV(center, radius, color);
}
inline void DrawCircleLines(int center_x, int center_y, float radius, Color color) {
    GetBackends().renderer2d->DrawCircleLines(center_x, center_y, radius, color);
}
inline void DrawRing(Vector2 center, float inner_radius, float outer_radius, float start_angle,
                      float end_angle, int segments, Color color) {
    GetBackends().renderer2d->DrawRing(center, inner_radius, outer_radius, start_angle, end_angle, segments,
                                        color);
}
inline void DrawTriangle(Vector2 v1, Vector2 v2, Vector2 v3, Color color) {
    GetBackends().renderer2d->DrawTriangle(v1, v2, v3, color);
}
inline void DrawTriangleFan(const Vector2 *points, int point_count, Color color) {
    GetBackends().renderer2d->DrawTriangleFan(points, point_count, color);
}

inline bool CheckCollisionPointRec(Vector2 point, Rectangle rec) {
    return GetBackends().renderer2d->CheckCollisionPointRec(point, rec);
}

inline Image LoadImage(const char *file_name) { return GetBackends().renderer2d->LoadImage(file_name); }
inline Image GenImageColor(int width, int height, Color color) {
    return GetBackends().renderer2d->GenImageColor(width, height, color);
}
inline Image ImageFromImage(Image image, Rectangle rec) {
    return GetBackends().renderer2d->ImageFromImage(image, rec);
}
inline void ImageFormat(Image *image, int new_format) { GetBackends().renderer2d->ImageFormat(image, new_format); }
inline void ImageFlipVertical(Image *image) { GetBackends().renderer2d->ImageFlipVertical(image); }
inline void ImageCrop(Image *image, Rectangle crop) { GetBackends().renderer2d->ImageCrop(image, crop); }
inline void UnloadImage(Image image) { GetBackends().renderer2d->UnloadImage(image); }
inline bool ExportImage(Image image, const char *file_name) {
    return GetBackends().renderer2d->ExportImage(image, file_name);
}
inline std::vector<unsigned char> ExportImageToMemory(Image image, const char *file_type) {
    return GetBackends().renderer2d->ExportImageToMemory(image, file_type);
}
inline Vector3 ColorToHSV(Color color) { return GetBackends().renderer2d->ColorToHSV(color); }
inline Color ColorFromHSV(float hue, float saturation, float value) {
    return GetBackends().renderer2d->ColorFromHSV(hue, saturation, value);
}

inline Texture2D LoadTextureFromImage(Image image) {
    return GetBackends().renderer2d->LoadTextureFromImage(image);
}
inline void UpdateTexture(Texture2D texture, const void *pixels) {
    GetBackends().renderer2d->UpdateTexture(texture, pixels);
}
inline void UnloadTexture(Texture2D texture) { GetBackends().renderer2d->UnloadTexture(texture); }
inline void DrawTextureEx(Texture2D texture, Vector2 position, float rotation, float scale, Color tint) {
    GetBackends().renderer2d->DrawTextureEx(texture, position, rotation, scale, tint);
}
inline void DrawTextureRec(Texture2D texture, Rectangle source, Vector2 position, Color tint) {
    GetBackends().renderer2d->DrawTextureRec(texture, source, position, tint);
}
inline void DrawTexturePro(Texture2D texture, Rectangle source, Rectangle dest, Vector2 origin,
                            float rotation, Color tint) {
    GetBackends().renderer2d->DrawTexturePro(texture, source, dest, origin, rotation, tint);
}

inline RenderTexture2D LoadRenderTexture(int width, int height) {
    return GetBackends().renderer2d->LoadRenderTexture(width, height);
}
inline void UnloadRenderTexture(RenderTexture2D target) {
    GetBackends().renderer2d->UnloadRenderTexture(target);
}
inline void BeginTextureMode(RenderTexture2D target) { GetBackends().renderer2d->BeginTextureMode(target); }
inline void EndTextureMode() { GetBackends().renderer2d->EndTextureMode(); }
inline Image LoadImageFromTexture(Texture2D texture) {
    return GetBackends().renderer2d->LoadImageFromTexture(texture);
}
inline Image LoadImageFromScreen() { return GetBackends().renderer2d->LoadImageFromScreen(); }
inline unsigned char *ReadScreenPixels(int width, int height) {
    return GetBackends().renderer2d->ReadScreenPixels(width, height);
}

}  // namespace gfx
