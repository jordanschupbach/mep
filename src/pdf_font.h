#pragma once

// mep's own in-house PDF font-dict wiring -- see PDFIUM_REMOVAL_PLAN.md
// Phase 10. Resolves a Font resource dict (simple Type1/TrueType, or
// composite Type0/CID) into a `PdfFont`: a code (1 byte for simple
// fonts, 2 bytes for the overwhelmingly-common Identity-H composite
// case -- see Scoping decision below) -> (glyph outline, advance width)
// resolver, bridging into Phase 9's `gfx::tt`/`gfx::cff` outline
// engines. Text-showing operators (Tj/TJ/etc., PDFIUM_REMOVAL_PLAN.md
// Phase 10's other half) live in pdf_content.cpp and call this class
// per glyph; this header only resolves the font itself.
//
// Font programs handled: embedded TrueType (/FontFile2), bare CFF or
// OpenType-wrapped CFF (/FontFile3), Type 1 (/FontFile -- gfx/type1.h,
// what pdflatex/dvips embed for every Computer Modern font), Type 3
// (glyphs as content streams, run by pdf_content.cpp -- see IsType3),
// and standard-14 substitution when nothing is embedded. A simple
// font's code -> glyph resolution follows spec 9.6.6: /Differences,
// then an explicit base encoding, then the embedded program's OWN
// built-in encoding (the "Builtin" case every dvipdfmx-embedded math
// font relies on), then StandardEncoding for non-symbolic fonts.
//
// Scoping decisions:
// - Composite fonts: `/Encoding Identity-H` (2-byte, direct CID==code)
//   and a `/CIDToGIDMap` that's either `/Identity` or an embedded
//   stream -- confirmed "the overwhelmingly common case" by Phase 1's
//   fixture audit (both tectonic and jpeg_test's composite fonts use
//   Identity-H). An embedded CMap *stream* for `/Encoding` (a full
//   PostScript-like CMap program mapping arbitrary variable-width byte
//   sequences to CIDs) is a documented non-goal -- real-world usage
//   overwhelmingly sticks to the predefined Identity-H/Identity-V CMaps
//   for exactly the reason mep's own LaTeX export path does: simpler,
//   and PDF readers already have to support it as a baseline. A font
//   with a real embedded encoding CMap falls back to treating it as
//   Identity-H (2-byte, direct), which is wrong but no worse than
//   producing nothing.
// - Standard-14 non-embedded fonts are substituted with mep's own
//   already-vendored Liberation Sans/Serif/Mono (OFL-1.1, see
//   src/office_font_data*.h) rather than fetching/deriving separate
//   Adobe AFM metrics: since the substitute's own glyph outlines are
//   what's actually drawn, using that SAME font's own hmtx advance
//   widths (falling back only when neither /Widths nor the PDF's own
//   default width apply) keeps shape and spacing self-consistent,
//   rather than mixing Liberation glyphs with a different foundry's
//   metrics. A deliberate, documented departure from this plan's
//   original "AFM-derived metrics" phrasing -- see
//   PDFIUM_REMOVAL_PLAN.md's Phase 10 writeup.
// - Symbol/ZapfDingbats (non-embedded, no Liberation equivalent):
//   glyphs render as empty (skipped, not a placeholder box) while
//   widths still come from /Widths if present -- text flow/spacing
//   stays approximately right even though nothing is drawn, matching
//   this codebase's general "degrade gracefully" convention.

#include "pdf_object.h"
#include "pdf_xref.h"

#include <cstddef>
#include <memory>
#include <string>

namespace pdffont {

class PdfFont {
public:
    PdfFont();
    ~PdfFont();
    PdfFont(const PdfFont &) = delete;
    PdfFont &operator=(const PdfFont &) = delete;
    PdfFont(PdfFont &&) noexcept;
    PdfFont &operator=(PdfFont &&) noexcept;

    // Resolves `font_dict` (already dereferenced by the caller) into
    // usable state: picks the outline engine (embedded TrueType/CFF, or
    // a standard-14 substitute), builds the code/CID -> GID and
    // -> width tables. Tolerant of anything it can't fully resolve --
    // worst case, GetGlyphBitmap returns nullptr for every code and
    // GetWidth returns a flat default, matching this codebase's
    // "degrade gracefully" convention; there's no failure return here.
    void Load(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
              const pdfobj::Object &font_dict);

    // 2 for a composite (Type0) font (Identity-H, 2-byte codes -- see
    // this header's own Scoping note), 1 for a simple font.
    int BytesPerCode() const { return is_composite_ ? 2 : 1; }

    // Whether word spacing (Tw) applies to `code` -- per spec 9.3.3,
    // only ever true for the single-byte code 32 in a font using
    // single-byte codes (never for composite/2-byte fonts, regardless
    // of what the 2-byte value happens to be).
    bool WordSpacingApplies(uint32_t code) const { return !is_composite_ && code == 32; }

    // Glyph-space advance width for `code`, in 1/1000 text-space units
    // per em (the standard PDF convention regardless of the font's own
    // internal unitsPerEm/FontMatrix) -- ready to plug into the text-
    // showing advance formula (spec 9.4.3) as `w0`.
    double GetWidth(uint32_t code) const;

    // Rasterizes `code`'s glyph outline (already resolved to the right
    // engine + GID internally) at `scale_x`/`scale_y` (font units ->
    // device pixels, i.e. already including both the font's own
    // unitsPerEm/FontMatrix scale and the caller's target size --
    // same convention gfx::tt/gfx::cff's own GetGlyphBitmap use).
    // Returns nullptr for an unmapped code or an empty outline (e.g.
    // space), matching those engines' own contract. Caller frees a
    // non-null result via FreeGlyphBitmap.
    unsigned char *GetGlyphBitmap(uint32_t code, float scale_x, float scale_y, int *width, int *height, int *xoff,
                                   int *yoff) const;
    // Same, through a full 2x2 matrix [a b; c d] mapping text-space em
    // units -> device pixels (rx = a*x + c*y, ry = b*x + d*y; the text
    // rendering matrix's own linear part, so rotated/skewed text renders
    // rotated/skewed). GetGlyphBitmap is exactly {scale_x, 0, 0, -scale_y}.
    unsigned char *GetGlyphBitmapMatrix(uint32_t code, float a, float b, float c, float d, int *width, int *height,
                                        int *xoff, int *yoff) const;
    void FreeGlyphBitmap(unsigned char *bitmap) const;

    // Type 3 fonts (spec 9.6.5) have no outlines: each glyph is a content
    // stream the caller (pdf_content.cpp) runs itself, with CTM =
    // Type3FontMatrix() x text rendering matrix and Type3Resources() (the
    // font's own /Resources; a Null object if it has none, in which case
    // the page's apply). GetGlyphBitmap* return nullptr for these;
    // GetWidth already includes the /FontMatrix conversion.
    bool IsType3() const;
    const double *Type3FontMatrix() const;  // 6 entries, PDF [a b c d e f]
    const pdfobj::Object &Type3Resources() const;
    bool Type3CharProc(uint32_t code, std::string *out_content) const;

    // Best-known Unicode text for `code`, UTF-8 encoded -- for text
    // extraction/search (PDFIUM_REMOVAL_PLAN.md Phase 11), entirely
    // independent of GetGlyphBitmap's own code->GID resolution (a
    // ligature glyph, for instance, renders as ONE glyph but its
    // /ToUnicode text is typically multiple characters, e.g. "fi").
    // Precedence: `/ToUnicode` CMap (`bfchar`/`bfrange`) when the font
    // dict has one -- the PDF author's own explicit, authoritative
    // mapping; else, for a simple font, the same encoding-table-implied
    // Unicode GetGlyphBitmap's named-encoding path already computes
    // (StandardEncoding/WinAnsiEncoding/MacRomanEncoding + /Differences
    // -> Adobe Glyph List); else empty (composite font with neither --
    // Scoping decision, see pdf_font.h's own top comment on non-Identity
    // encodings: a composite font's CID has no inherent Unicode meaning
    // without either source).
    std::string GetUnicodeText(uint32_t code) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool is_composite_ = false;
};

}  // namespace pdffont
