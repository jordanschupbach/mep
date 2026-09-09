// NativeTextBackend: font rasterization for Stage B's native backend, via
// vendored stb_truetype.h (third_party/stb_truetype.h -- see
// third_party_licenses/stb_truetype-LICENSE.txt). Bakes one atlas per
// LoadFontFromMemory/LoadFontData+GenImageFontAtlas call, using a simple
// shelf packer (good enough for a few thousand glyphs at UI sizes; not
// worth a more sophisticated packer until this backend is actually swapped
// in and atlas memory/rebuild-time is a measured problem).
//
// Atlas pixels are baked as white RGB + coverage alpha, not a bare
// single-channel bitmap -- see backend_native_renderer2d.cpp's top
// comment for why: main.cpp's DrawLineFast (and gfx::DrawTextEx/
// DrawTextCodepoint below) draw glyphs through the *same* textured-quad
// shader as everything else (a plain gfx::DrawTexturePro call), so the
// atlas has to look like an ordinary tinted RGBA texture when sampled,
// not something a special shader is needed to interpret.

#include "gfx/backend_native_internal.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gfx/gl_loader.h"

// See backend_native_renderer2d.cpp's own note on STB_*_IMPLEMENTATION
// placement: exactly one translation unit in the final binary may define
// this. Not linked into mep_core/mep yet, so safe here for now; resolve
// when Stage B10 merges this backend in (image_doc.cpp already claims
// STB_IMAGE_IMPLEMENTATION for that binary, and this file needs no image
// decode of its own, just STB_TRUETYPE_IMPLEMENTATION).
#define STB_TRUETYPE_IMPLEMENTATION
// Vendored third-party header -- suppress mep's own strict -Wall/-Wextra/
// ... flags (MEP_STRICT_FLAGS in CMakeLists.txt) for just this include,
// same as image_doc.cpp does for stb_image.h.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wduplicated-branches"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#endif
#include "../third_party/stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace gfx {

namespace {

struct BakedGlyph {
    std::vector<unsigned char> bitmap;  // single-channel coverage, tightly cropped
    int width = 0, height = 0;
    int atlas_x = 0, atlas_y = 0;  // filled in by the shelf packer
};

// Raylib's own PIXELFORMAT_UNCOMPRESSED_GRAYSCALE value -- not one of
// gfx/types.h's shared kPixelFormat* constants since nothing outside
// this file ever reads a LoadFontData-produced glyph image's .format
// (GenImageFontAtlas reads its raw bytes directly; gfx::UnloadImage just
// frees .data regardless of format).
constexpr int kGlyphBitmapFormat = 1;

// Simple shelf packer: fixed atlas width, glyphs placed left-to-right,
// wrapping to a new "shelf" (row) when one doesn't fit; `padding` empty
// pixels are reserved around every glyph so DrawLineFast's own padding-
// expansion trick (see its comment in main.cpp) samples transparent
// pixels instead of a neighboring glyph.
constexpr int kAtlasWidth = 2048;

void PackShelf(std::vector<BakedGlyph> &glyphs, int padding, int *out_width, int *out_height) {
    int pen_x = padding, pen_y = padding, shelf_h = 0;
    for (BakedGlyph &g : glyphs) {
        int cell_w = g.width + padding * 2;
        int cell_h = g.height + padding * 2;
        if (pen_x + cell_w > kAtlasWidth) {
            pen_x = padding;
            pen_y += shelf_h + padding;
            shelf_h = 0;
        }
        g.atlas_x = pen_x + padding;
        g.atlas_y = pen_y + padding;
        pen_x += cell_w;
        shelf_h = std::max(shelf_h, cell_h);
    }
    *out_width = kAtlasWidth;
    *out_height = pen_y + shelf_h + padding;
}

std::vector<int> DefaultCodepoints() {
    // Matches raylib's own LoadFontFromMemory(..., nullptr, 0) default:
    // printable ASCII, 32 ('space') through 126 ('~').
    std::vector<int> out;
    for (int c = 32; c <= 126; c++) out.push_back(c);
    return out;
}

}  // namespace

struct NativeTextBackend::Impl {
    NativeRenderer2DBackend *renderer2d = nullptr;
};

NativeTextBackend::NativeTextBackend(NativeRenderer2DBackend *renderer2d) : impl_(new Impl()) {
    impl_->renderer2d = renderer2d;
}

NativeTextBackend::~NativeTextBackend() { delete impl_; }

gfx::GlyphInfo *NativeTextBackend::LoadFontData(const unsigned char *file_data, int data_size, int font_size,
                                                 int *codepoints, int codepoint_count) {
    (void)data_size;  // stbtt_InitFont trusts the buffer like raylib's own LoadFontData does
    auto *font_info = new stbtt_fontinfo();
    if (stbtt_InitFont(font_info, file_data, 0) == 0) {
        std::fprintf(stderr, "gfx native: stbtt_InitFont failed\n");
        delete font_info;
        return nullptr;
    }
    float scale = stbtt_ScaleForPixelHeight(font_info, static_cast<float>(font_size));
    // stbtt_GetCodepointBitmap's own yoff is relative to the glyph's
    // baseline (typically negative -- most glyphs sit above it), not to
    // the top of a font-size-tall line box. Adding the (scaled) font
    // ascent -- the baseline's own distance down from the top of that
    // box -- converts it to a top-of-line-relative offset, matching
    // raylib's own LoadFontData (rtext.c) exactly. Without this, every
    // glyph renders "ascent pixels" too high relative to the rest of the
    // UI (rectangles, cursors, line/pane chrome -- none of which go
    // through this offset at all).
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(font_info, &ascent, &descent, &line_gap);
    int ascent_offset = static_cast<int>(static_cast<float>(ascent) * scale);

    std::vector<int> owned_codepoints;
    if (codepoints == nullptr || codepoint_count <= 0) {
        owned_codepoints = DefaultCodepoints();
        codepoints = owned_codepoints.data();
        codepoint_count = static_cast<int>(owned_codepoints.size());
    }

    // Each glyph's `.image` is a real, independently malloc'd single-
    // channel coverage bitmap -- not an internal carrier for some other
    // structure -- so it behaves exactly like any other gfx::Image to a
    // caller: GenImageFontAtlas reads it to pack the final atlas, and
    // main.cpp's icon-font async-bake path (ApplyFontSize) is free to
    // gfx::UnloadImage() a glyph's image and replace it with a fresh
    // gfx::ImageFromImage() crop, exactly as raylib's own LoadFontData
    // output supports. (A previous version of this function instead
    // stashed a pointer into a side-table here, which broke exactly that
    // UnloadImage call -- freeing a non-heap pointer and corrupting the
    // heap the first time the icon font's async bake result was consumed.)
    auto *out = new gfx::GlyphInfo[static_cast<size_t>(codepoint_count)];
    for (int i = 0; i < codepoint_count; i++) {
        int cp = codepoints[i];
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(font_info, cp, &advance, &lsb);
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char *bitmap = stbtt_GetCodepointBitmap(font_info, scale, scale, cp, &w, &h, &xoff, &yoff);

        out[i].value = cp;
        out[i].offsetX = xoff;
        out[i].offsetY = yoff + ascent_offset;
        out[i].advanceX = static_cast<int>(static_cast<float>(advance) * scale);
        out[i].image.width = w;
        out[i].image.height = h;
        out[i].image.mipmaps = 1;
        out[i].image.format = kGlyphBitmapFormat;
        if (bitmap != nullptr && w > 0 && h > 0) {
            size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
            auto *copy = static_cast<unsigned char *>(std::malloc(n));
            std::memcpy(copy, bitmap, n);
            out[i].image.data = copy;
            stbtt_FreeBitmap(bitmap, nullptr);
        } else {
            out[i].image.data = nullptr;
        }
    }
    delete font_info;
    return out;
}

gfx::Image NativeTextBackend::GenImageFontAtlas(const gfx::GlyphInfo *glyphs, gfx::Rectangle **glyph_recs,
                                                 int glyph_count, int font_size, int padding) {
    (void)font_size;
    std::vector<BakedGlyph> baked;
    baked.reserve(static_cast<size_t>(glyph_count));
    for (int i = 0; i < glyph_count; i++) {
        BakedGlyph g;
        g.width = glyphs[i].image.width;
        g.height = glyphs[i].image.height;
        if (g.width > 0 && g.height > 0 && glyphs[i].image.data != nullptr) {
            const auto *src = static_cast<const unsigned char *>(glyphs[i].image.data);
            g.bitmap.assign(src, src + static_cast<size_t>(g.width) * static_cast<size_t>(g.height));
        }
        baked.push_back(std::move(g));
    }

    int atlas_w = 0, atlas_h = 0;
    PackShelf(baked, padding, &atlas_w, &atlas_h);

    auto *pixels = static_cast<unsigned char *>(
        std::calloc(static_cast<size_t>(atlas_w) * static_cast<size_t>(atlas_h) * 4, 1));
    auto *recs = new gfx::Rectangle[static_cast<size_t>(glyph_count)];
    for (int i = 0; i < glyph_count; i++) {
        const BakedGlyph &g = baked[static_cast<size_t>(i)];
        for (int y = 0; y < g.height; y++) {
            for (int x = 0; x < g.width; x++) {
                unsigned char coverage = g.bitmap[static_cast<size_t>(y) * static_cast<size_t>(g.width) +
                                                   static_cast<size_t>(x)];
                size_t px = (static_cast<size_t>(g.atlas_y + y) * static_cast<size_t>(atlas_w) +
                             static_cast<size_t>(g.atlas_x + x)) *
                            4;
                pixels[px + 0] = 255;
                pixels[px + 1] = 255;
                pixels[px + 2] = 255;
                pixels[px + 3] = coverage;
            }
        }
        recs[i] = gfx::Rectangle{static_cast<float>(g.atlas_x), static_cast<float>(g.atlas_y),
                                  static_cast<float>(g.width), static_cast<float>(g.height)};
    }
    *glyph_recs = recs;
    return gfx::Image{pixels, atlas_w, atlas_h, 1, gfx::kPixelFormatR8G8B8A8};
}

void NativeTextBackend::UnloadFontData(gfx::GlyphInfo *glyphs, int glyph_count) {
    for (int i = 0; i < glyph_count; i++) std::free(glyphs[i].image.data);
    delete[] glyphs;
}

void NativeTextBackend::FreeGlyphRects(gfx::Rectangle *recs) { delete[] recs; }

gfx::Font NativeTextBackend::LoadFontFromMemory(const char * /*file_type*/, const unsigned char *file_data,
                                                 int data_size, int font_size, int *codepoints,
                                                 int codepoint_count) {
    gfx::GlyphInfo *glyphs = LoadFontData(file_data, data_size, font_size, codepoints, codepoint_count);
    if (glyphs == nullptr) return gfx::Font{};
    int actual_count = codepoint_count > 0 ? codepoint_count : static_cast<int>(DefaultCodepoints().size());
    gfx::Rectangle *recs = nullptr;
    gfx::Image atlas = GenImageFontAtlas(glyphs, &recs, actual_count, font_size, 2);

    gfx::Font font{};
    font.baseSize = font_size;
    font.glyphCount = actual_count;
    font.glyphPadding = 2;
    font.texture = impl_->renderer2d->LoadTextureFromImage(atlas);
    font.recs = recs;
    font.glyphs = glyphs;
    std::free(atlas.data);
    return font;
}

void NativeTextBackend::UnloadFont(gfx::Font font) {
    impl_->renderer2d->UnloadTexture(font.texture);
    UnloadFontData(font.glyphs, font.glyphCount);
    FreeGlyphRects(font.recs);
}

void NativeTextBackend::SetTextureFilter(gfx::Texture2D texture, gfx::TextureFilter filter) {
    gl::BindTexture(gl::GL_TEXTURE_2D, texture.id);
    gl::GLint f = filter == gfx::TextureFilter::Bilinear ? static_cast<gl::GLint>(gl::GL_LINEAR)
                                                          : static_cast<gl::GLint>(gl::GL_NEAREST);
    gl::TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, f);
    gl::TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, f);
}

int NativeTextBackend::GetGlyphIndex(gfx::Font font, int codepoint) {
    for (int i = 0; i < font.glyphCount; i++) {
        if (font.glyphs[i].value == codepoint) return i;
    }
    for (int i = 0; i < font.glyphCount; i++) {
        if (font.glyphs[i].value == '?') return i;
    }
    return 0;
}

int NativeTextBackend::GetCodepointNext(const char *text, int *codepoint_size) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(text);
    if (bytes[0] < 0x80) {
        *codepoint_size = 1;
        return bytes[0];
    }
    int extra = 0, cp = 0;
    if ((bytes[0] & 0xE0) == 0xC0) { extra = 1; cp = bytes[0] & 0x1F; }
    else if ((bytes[0] & 0xF0) == 0xE0) { extra = 2; cp = bytes[0] & 0x0F; }
    else if ((bytes[0] & 0xF8) == 0xF0) { extra = 3; cp = bytes[0] & 0x07; }
    else {
        *codepoint_size = 1;
        return '?';
    }
    for (int i = 1; i <= extra; i++) {
        if ((bytes[i] & 0xC0) != 0x80) {
            *codepoint_size = 1;
            return '?';
        }
        cp = (cp << 6) | (bytes[i] & 0x3F);
    }
    *codepoint_size = extra + 1;
    return cp;
}

gfx::Vector2 NativeTextBackend::MeasureTextEx(gfx::Font font, const char *text, float font_size, float spacing) {
    if (font.baseSize <= 0) return {0, font_size};
    float scale = font_size / static_cast<float>(font.baseSize);
    float width = 0.0f;
    int len = static_cast<int>(std::strlen(text));
    int i = 0;
    int glyph_count_on_line = 0;
    while (i < len) {
        int size = 0;
        int cp = GetCodepointNext(text + i, &size);
        i += size;
        int idx = GetGlyphIndex(font, cp);
        width += static_cast<float>(font.glyphs[idx].advanceX) * scale;
        glyph_count_on_line++;
    }
    if (glyph_count_on_line > 1) width += spacing * static_cast<float>(glyph_count_on_line - 1);
    return {width, font_size};
}

void NativeTextBackend::DrawTextCodepoint(gfx::Font font, int codepoint, gfx::Vector2 position, float font_size,
                                           gfx::Color tint) {
    if (font.baseSize <= 0) return;
    float scale = font_size / static_cast<float>(font.baseSize);
    int idx = GetGlyphIndex(font, codepoint);
    gfx::Rectangle src = font.recs[idx];
    gfx::Rectangle dst{position.x + static_cast<float>(font.glyphs[idx].offsetX) * scale,
                       position.y + static_cast<float>(font.glyphs[idx].offsetY) * scale, src.width * scale,
                       src.height * scale};
    impl_->renderer2d->DrawTexturePro(font.texture, src, dst, {0, 0}, 0.0f, tint);
}

void NativeTextBackend::DrawTextEx(gfx::Font font, const char *text, gfx::Vector2 position, float font_size,
                                    float spacing, gfx::Color tint) {
    if (font.baseSize <= 0) return;
    float scale = font_size / static_cast<float>(font.baseSize);
    int len = static_cast<int>(std::strlen(text));
    int i = 0;
    float x = position.x;
    while (i < len) {
        int size = 0;
        int cp = GetCodepointNext(text + i, &size);
        i += size;
        int idx = GetGlyphIndex(font, cp);
        DrawTextCodepoint(font, cp, {x, position.y}, font_size, tint);
        x += static_cast<float>(font.glyphs[idx].advanceX) * scale + spacing;
    }
}

}  // namespace gfx
