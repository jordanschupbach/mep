#pragma once

// mep's own in-house CFF (Compact Font Format) / Type2-charstring
// outline parser + rasterizer -- see PDFIUM_REMOVAL_PLAN.md Phase 9.
// The single largest net-new subsystem in the whole PDFium removal
// plan: confirmed *required*, not optional, by Phase 1's own fixture
// audit -- every font tectonic/XeLaTeX embeds (mep's own LaTeX export
// path) is CFF-flavored (`/FontFile3`, `/Subtype Type1C` or
// `CIDFontType0C`), simple and composite alike, never TrueType. Zero
// code reuse from gfx/truetype.h -- CFF's compact INDEX/DICT structures
// and Type2 charstring operator set are a completely distinct outline
// format from TrueType's glyf table.
//
// Deliberately GID-direct only (mirrors gfx/truetype.h's own
// GetGlyphBitmap sibling, added in this same phase): a CFF font's
// CharStrings INDEX is inherently indexed by glyph index already (GID
// IS the CharStrings item number, for both non-CID and CID-keyed CFF),
// so there is no separate "codepoint" entry point the way TrueType
// needs one for its cmap-based lookup -- PDF font-dict wiring (Phase
// 10) resolves a character code (or CID) to a GID itself (via
// /Encoding+/Differences+charset for simple Type1C fonts, or via the
// charset's CID<->GID mapping for CIDFontType0C) before ever calling
// into this module.
//
// Supports: Type2 charstring path operators (moveto/lineto/curveto in
// all their compact relative-coordinate forms), stem hints and
// hint/counter masks (parsed only to keep the bytestream position
// correct -- hinting itself has no effect on outline shape at the
// sizes mep renders), local/global subroutine calls (with the spec's
// own bias formula), and the 4 "flex" hint operators real font tools
// (FontForge, AFDKO) actually emit. CID-keyed CFF's FDArray/FDSelect
// (per-glyph choice of which Private DICT's local subrs apply) is
// supported since it's required simply to execute composite-font
// charstrings correctly, not just to map CID->GID (which is Phase 10's
// job via the charset table this module also exposes).
//
// Explicitly NOT supported (documented, deliberate scope limits):
// - CharstringType 1 (old Type 1 charstrings inside a CFF wrapper --
//   vanishingly rare; virtually every modern CFF-in-OpenType/PDF font
//   uses CharstringType 2).
// - The deprecated 4-argument "seac-like" form of `endchar` (accented
//   character composition via Standard Encoding) -- rare in modern
//   fonts (Adobe deprecated it); detected and tolerated by simply not
//   drawing the accent, not by crashing/failing the glyph.
// - The arithmetic/logical/storage escape operators (and/or/not/abs/
//   add/sub/div/mul/sqrt/drop/put/get/ifelse/random, `12 3`-`12 28`
//   apart from the flex ones) -- a leftover-from-Type1 extension real
//   font-generation tools don't emit for ordinary outline data; treated
//   as an unrecognized operator (stack cleared, charstring continues),
//   matching this codebase's "skip unsupported content" convention.

#include <cstdint>
#include <vector>

namespace gfx {
namespace cff {

// One INDEX structure's parsed item boundaries (CFF spec section 5) --
// a non-owning view into the font's own data buffer, same convention as
// gfx::tt::FontInfo.
struct Index {
    int count = 0;
    std::vector<uint32_t> offsets;   // count+1 entries, 0-based, relative to data_start
    const unsigned char *data_start = nullptr;
};

// A CID-keyed font's Font DICT array entry: just the piece Phase 9's
// charstring interpreter needs (the local subrs it selects via
// FDSelect) -- the rest of each Font DICT (its own FontMatrix override,
// etc.) is out of scope for outline rendering.
struct FdEntry {
    Index local_subrs;
};

// Parsed font-wide state -- built once by InitFont, read by
// GetGlyphBitmap. Holds non-owning pointers/offsets into the caller's
// own buffer (must outlive this struct), same contract as
// gfx::tt::FontInfo.
struct FontInfo {
    const unsigned char *data = nullptr;
    int data_size = 0;

    Index charstrings;
    Index global_subrs;
    Index strings;  // this font's own String INDEX -- custom glyph-name strings for SIDs beyond the ~391 predefined ones (see pdf_encodings.h's CffStandardString/CffStandardStringCount, which own that predefined table -- deliberately NOT duplicated here to keep this module PDF-agnostic/reusable, per this file's own top comment)
    int num_glyphs = 0;

    bool is_cid = false;
    Index local_subrs;             // valid when !is_cid
    std::vector<FdEntry> fd_array;  // valid when is_cid
    std::vector<uint8_t> fd_select;  // valid when is_cid; size == num_glyphs, gid -> fd_array index

    // Charset: gid -> SID (non-CID) or gid -> CID (CID-keyed), per spec
    // 13, indexed DIRECTLY by gid -- charset[gid] is that glyph's own
    // SID/CID (sized to num_glyphs; charset[0] is left at its default 0,
    // matching .notdef's implicit SID/CID 0, but is never separately
    // written to -- callers still index by plain `gid`, not `gid-1`; an
    // earlier caller in this plan's own Phase 10 misread this as
    // "index i holds gid (i+1)'s SID", an off-by-one that silently
    // resolved every simple CFF font's character codes to the wrong,
    // adjacent glyph -- see PDFIUM_REMOVAL_PLAN.md's Phase 10 writeup).
    // Empty if the font used a predefined charset (ISOAdobe/
    // Expert/ExpertSubset -- rare for embedded PDF fonts, whose charset
    // is essentially always custom); Phase 10's CID<->GID mapping for
    // CIDFontType0 tolerates that by falling back to identity.
    std::vector<uint16_t> charset;

    // FontMatrix (spec 9182 Top DICT key 12 7), default 0.001 scale
    // (1000 units/em, the standard PostScript convention) if the font
    // doesn't override it. `a`/`d` are what GetGlyphBitmap actually
    // uses for scaling (matching gfx::tt's own uniform-scale contract);
    // `b`/`c`/`e`/`f` are read but unused (a skewed/offset FontMatrix is
    // vanishingly rare in practice for embedded PDF fonts).
    double font_matrix[6] = {0.001, 0, 0, 0.001, 0, 0};
};

// Parses a bare CFF table's header, top DICT, and the INDEXes/DICTs
// GetGlyphBitmap needs (CharStrings, Global/Local Subrs, FDArray/
// FDSelect for CID-keyed fonts, charset). Returns false if this isn't a
// recognizable CFF table, uses CharstringType other than 2, or is
// missing CharStrings.
bool InitFont(FontInfo *info, const unsigned char *data, int data_size);

// Fetches item `i` (0-based) from a parsed INDEX -- exposed so callers
// can read arbitrary INDEXes this module parses (FontInfo::strings, in
// particular, for custom-SID glyph-name resolution) without needing
// their own copy of the INDEX item-lookup logic. Returns false for an
// out-of-range index or an empty/never-parsed Index.
bool GetIndexItem(const Index &index, int i, const unsigned char **out_ptr, uint32_t *out_len);

// Rasterizes one glyph, addressed directly by glyph index (the
// CharStrings INDEX position -- see this file's own top comment for why
// there's no codepoint-based entry point). Same contract as
// gfx::tt::GetGlyphBitmap: returns nullptr for an out-of-range index or
// an empty outline (e.g. space); caller frees a non-null result via
// FreeBitmap. `scale_x`/`scale_y` already include both FontMatrix and
// any additional caller-side scale (multiply them together before
// calling, the same way a caller would combine gfx::tt's unitsPerEm
// scale with its own target size).
unsigned char *GetGlyphBitmap(const FontInfo *info, float scale_x, float scale_y, int glyph_index, int *width,
                               int *height, int *xoff, int *yoff);

void FreeBitmap(unsigned char *bitmap);

}  // namespace cff
}  // namespace gfx
