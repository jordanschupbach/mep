#pragma once

// mep's own in-house TrueType (glyf-outline) parser + scanline
// rasterizer -- see STB_TRUETYPE_REMOVAL_PLAN.md for the full writeup.
// Replaces third_party/stb_truetype.h behind the same shape of API
// gfx/backend_native_text.cpp's LoadFontData already calls, narrowed to
// exactly what that one call site needs: parse an sfnt/TrueType font
// buffer, look up per-codepoint metrics, and rasterize one glyph's
// outline to an antialiased coverage bitmap. No dependency on anything
// in gfx/ -- this is a standalone, reusable module, same spirit as
// stb_truetype itself.
//
// Deliberately does NOT support: CFF/PostScript outlines (`CFF ` table
// -- every font mep ships uses `glyf`, confirmed by that plan's Phase 1
// font inventory; the one font that didn't was re-subsetted from its
// TrueType-flavored release instead of adding a second outline format),
// hinting instruction execution (stb_truetype doesn't either -- see that
// plan's Scoping decision 2), kerning, ligatures/shaping, variable-font
// axes, or color glyphs. Matches gfx/text.h's own flat per-codepoint API.

#include <cstdint>

namespace gfx {
namespace tt {

// Parsed table locations + font-wide metrics -- built once by InitFont,
// read by every other call. Holds a non-owning pointer into the
// caller's own buffer (matches stbtt_fontinfo's own contract: the
// buffer must outlive the FontInfo, which backend_native_text.cpp's
// LoadFontData already satisfies -- the FontInfo never outlives the one
// function call that owns file_data).
struct FontInfo {
    const unsigned char *data = nullptr;
    int data_size = 0;

    uint32_t glyf_off = 0, glyf_len = 0;
    uint32_t loca_off = 0, loca_len = 0;
    uint32_t hmtx_off = 0, hmtx_len = 0;
    uint32_t hhea_off = 0;
    uint32_t head_off = 0;
    uint32_t maxp_off = 0;
    uint32_t cmap_subtable_off = 0;  // offset of the chosen cmap subtable (absolute, from start of `data`)
    int cmap_format = 0;             // 4 or 12; 0 if no usable subtable was found

    int units_per_em = 1000;
    int num_glyphs = 0;
    int num_h_metrics = 0;
    bool loca_long = false;  // head table's indexToLocFormat: 0 = 16-bit (halved offsets), 1 = 32-bit
};

// Parses the sfnt table directory and the head/hhea/maxp/hmtx/cmap
// tables' own headers (not glyph outlines yet -- those are decoded
// lazily, per glyph, by GetCodepointBitmap). `data` must outlive `info`.
// Returns false (matching stbtt_InitFont's own 0-on-failure convention)
// if this isn't a recognizable sfnt/TrueType font or is missing a
// required table.
bool InitFont(FontInfo *info, const unsigned char *data, int data_size, int offset = 0);

// Font-units-to-pixels scale factor for a target pixel height (matches
// stbtt_ScaleForPixelHeight: pixel_height / (ascent - descent)).
float ScaleForPixelHeight(const FontInfo *info, float pixel_height);

// Ascent/descent/line-gap, in font units (unscaled) -- caller applies
// its own scale factor, matching stbtt_GetFontVMetrics.
void GetFontVMetrics(const FontInfo *info, int *ascent, int *descent, int *line_gap);

// Advance width + left side bearing for one codepoint, in font units
// (unscaled). 0/0 if the codepoint isn't in the font's cmap.
void GetCodepointHMetrics(const FontInfo *info, int codepoint, int *advance_width, int *left_side_bearing);

// Same as GetCodepointHMetrics, but addressed directly by glyph index,
// bypassing cmap lookup -- the GID-direct sibling PDFIUM_REMOVAL_PLAN.md
// Phase 10 needs to compute a fallback advance width for a code that
// resolved to a glyph without going through a codepoint at all (the
// same symbolic-no-cmap / direct-code-as-GID case GetGlyphBitmap's own
// sibling was added for in Phase 9). 0/0 for an out-of-range index.
void GetGlyphHMetrics(const FontInfo *info, int glyph_index, int *advance_width, int *left_side_bearing);

// Rasterizes one codepoint's glyph outline (resolving composite glyphs
// recursively) to a tightly-cropped, antialiased single-channel coverage
// bitmap, scaled by (scale_x, scale_y). Returns nullptr (matching
// stbtt_GetCodepointBitmap's own contract) if the codepoint has no glyph
// or an empty outline (e.g. space). `*xoff`/`*yoff` are the offset from
// the glyph's origin (baseline, pen position) to the bitmap's top-left
// corner, in pixels -- yoff is typically negative (the glyph sits above
// the baseline). Caller frees the result via FreeBitmap.
unsigned char *GetCodepointBitmap(const FontInfo *info, float scale_x, float scale_y, int codepoint, int *width,
                                   int *height, int *xoff, int *yoff);

// Same as GetCodepointBitmap, but addressed directly by glyph index,
// bypassing cmap lookup entirely -- needed for PDF embedded TrueType
// fonts (PDFIUM_REMOVAL_PLAN.md Phase 9), where a character code maps
// to a glyph index via the PDF font's own `/Encoding` + `/Differences`
// or a `/CIDToGIDMap`, not necessarily the font's built-in Unicode
// cmap. `glyph_index` 0 (.notdef) is rasterized like any other index,
// not specially rejected -- only an out-of-range index or an empty
// outline (e.g. space) returns nullptr, matching
// GetCodepointBitmap's own "no outline" contract.
unsigned char *GetGlyphBitmap(const FontInfo *info, float scale_x, float scale_y, int glyph_index, int *width,
                               int *height, int *xoff, int *yoff);

void FreeBitmap(unsigned char *bitmap);

}  // namespace tt
}  // namespace gfx
