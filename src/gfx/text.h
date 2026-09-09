#pragma once

// Facade over ITextBackend (see gfx/backend.h). `gfx::Font` mirrors
// raylib's Font shape field-for-field (see gfx/types.h), so main.cpp's
// font-atlas setup (ApplyFontSize, StartIconFontBakeAsync) and the
// DrawLineFast fast-path glyph blit both port by prefixing calls with
// `gfx::` and reading the exact same fields (baseSize, glyphPadding,
// glyphs[i].offsetX/offsetY, recs[i].x/y/width/height, texture).

#include "gfx/backend.h"
#include "gfx/types.h"

namespace gfx {

inline Font LoadFontFromMemory(const char *file_type, const unsigned char *file_data, int data_size,
                                int font_size, int *codepoints, int codepoint_count) {
    return GetBackends().text->LoadFontFromMemory(file_type, file_data, data_size, font_size, codepoints,
                                                    codepoint_count);
}
inline void UnloadFont(Font font) { GetBackends().text->UnloadFont(font); }
inline void SetTextureFilter(Texture2D texture, TextureFilter filter) {
    GetBackends().text->SetTextureFilter(texture, filter);
}
inline GlyphInfo *LoadFontData(const unsigned char *file_data, int data_size, int font_size,
                                int *codepoints, int codepoint_count) {
    return GetBackends().text->LoadFontData(file_data, data_size, font_size, codepoints, codepoint_count);
}
inline Image GenImageFontAtlas(const GlyphInfo *glyphs, Rectangle **glyph_recs, int glyph_count,
                                int font_size, int padding) {
    return GetBackends().text->GenImageFontAtlas(glyphs, glyph_recs, glyph_count, font_size, padding);
}
inline void UnloadFontData(GlyphInfo *glyphs, int glyph_count) {
    GetBackends().text->UnloadFontData(glyphs, glyph_count);
}
inline void FreeGlyphRects(Rectangle *recs) { GetBackends().text->FreeGlyphRects(recs); }
inline int GetGlyphIndex(Font font, int codepoint) { return GetBackends().text->GetGlyphIndex(font, codepoint); }
inline Vector2 MeasureTextEx(Font font, const char *text, float font_size, float spacing) {
    return GetBackends().text->MeasureTextEx(font, text, font_size, spacing);
}
inline void DrawTextEx(Font font, const char *text, Vector2 position, float font_size, float spacing,
                        Color tint) {
    GetBackends().text->DrawTextEx(font, text, position, font_size, spacing, tint);
}
inline void DrawTextCodepoint(Font font, int codepoint, Vector2 position, float font_size, Color tint) {
    GetBackends().text->DrawTextCodepoint(font, codepoint, position, font_size, tint);
}
inline int GetCodepointNext(const char *text, int *codepoint_size) {
    return GetBackends().text->GetCodepointNext(text, codepoint_size);
}

}  // namespace gfx
