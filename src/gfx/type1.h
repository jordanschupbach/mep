#pragma once

// mep's own in-house Type 1 font program parser + charstring
// interpreter -- the `/FontFile` sibling of gfx/cff.h (`/FontFile3`) and
// gfx/truetype.h (`/FontFile2`). Type 1 is what pdflatex/dvips embed for
// every Computer Modern / Latin Modern / AMS font (text AND math), i.e.
// the overwhelming majority of LaTeX-produced PDFs in the wild (arXiv
// papers, journal preprints); before this module such fonts fell back to
// a Liberation Sans substitute, which is merely ugly for text and simply
// wrong for math symbols (whose glyph names Liberation doesn't have).
//
// Handles both the PFA layout PDF embeds (cleartext header, `eexec`
// binary-encrypted private portion, cleartext trailer -- with
// /Length1/2/3 boundaries deliberately NOT relied upon, since real
// producers frequently get them wrong) and the segmented PFB container
// (0x80 0x01/0x02 headers, as found on disk), plus hex-encoded eexec
// sections. Decodes eexec/charstring encryption, /lenIV, /Subrs,
// /CharStrings, the built-in /Encoding (StandardEncoding or a custom
// `dup <code> /<name> put` array), and /FontMatrix.
//
// Charstring interpreter: every Type 1 path operator, hint operators
// (parsed, no outline effect), Subrs, seac (accented-character
// composition, extremely common in TeX text fonts), sbw/hsbw side
// bearings, div, and the flex/hint-replacement OtherSubrs protocol
// (OtherSubrs 0-3 emulated natively; unknown OtherSubrs hand their
// arguments back through `pop`, the standard tolerant treatment).
// Outlines feed gfx::raster::RasterizeOutline (shared with gfx/cff.cpp).
//
// Deliberately PDF-agnostic like its two siblings: code -> glyph-name
// resolution through a PDF /Encoding + /Differences is pdf_font.cpp's job
// (via GidForName below); this module only knows the font's OWN encoding.

#include <cstdint>
#include <string>
#include <vector>

namespace gfx {
namespace t1 {

struct FontInfo {
    std::vector<std::string> glyph_names;  // gid -> glyph name, CharStrings order (gid 0 is whatever came first, NOT necessarily .notdef)
    std::vector<std::string> charstrings;  // gid -> decrypted charstring, lenIV bytes already stripped
    std::vector<std::string> subrs;        // decrypted local subroutines
    int num_glyphs = 0;
    double font_matrix[6] = {0.001, 0, 0, 0.001, 0, 0};

    // Built-in /Encoding: code -> gid (-1 = unencoded). A font declaring
    // `/Encoding StandardEncoding def` gets it resolved here through
    // this module's own StandardEncoding name table, so callers need
    // not care which form the font used.
    int builtin_encoding[256];
    bool encoding_standard = false;
};

// Parses a Type 1 font program (PFA/PFB/PDF-embedded). Returns false if
// no eexec section or CharStrings could be found. Copies what it needs
// out of `data`, which may be freed afterwards (unlike gfx::tt/gfx::cff).
bool InitFont(FontInfo *info, const unsigned char *data, int data_size);

// gid for `name`, or -1.
int GidForName(const FontInfo *info, const std::string &name);

// StandardEncoding glyph name for `code`, or nullptr (also what seac's
// bchar/achar codes resolve through).
const char *StandardEncodingName(int code);

// Advance width (hsbw/sbw's wx), in font units; 0 for an unknown gid.
double GetGlyphAdvance(const FontInfo *info, int gid);

// Same contract as gfx::cff::GetGlyphBitmapMatrix / GetGlyphBitmap
// (matrix = font units -> device pixels, rx = a*x + c*y, ry = b*x + d*y;
// caller frees via FreeBitmap; nullptr for an empty outline).
unsigned char *GetGlyphBitmapMatrix(const FontInfo *info, float a, float b, float c, float d, int gid, int *width,
                                    int *height, int *xoff, int *yoff);
unsigned char *GetGlyphBitmap(const FontInfo *info, float scale_x, float scale_y, int gid, int *width, int *height,
                               int *xoff, int *yoff);
void FreeBitmap(unsigned char *bitmap);

}  // namespace t1
}  // namespace gfx
