#ifndef MEP_SVG_RASTER_H
#define MEP_SVG_RASTER_H

#include <cstddef>
#include <string>
#include <vector>

// An SVG file as a picture: the inline-image path (an org [[file:x.svg]]
// link, a mepml \image(x.svg), a block's file= figure) decodes PNG/JPEG/
// GIF/BMP through image_codec, and an SVG through this instead. The SVG
// grammar is svg_doc's (BuildSvgDisplayList, the same flattening inline
// <svg> in an HTML page goes through); this fills its shapes on the CPU
// with gfx/rasterizer's antialiased scanline filler and draws its text with
// gfx/truetype, so the result is an ordinary RGBA image any texture upload
// takes. Kept apart from image_codec so the many small targets that link
// that one do not pull in the HTML parser and font stack.
namespace svg_raster {

// Whether `bytes` look like an SVG document (an <svg element near the top,
// after any XML declaration, doctype or comments).
bool LooksLikeSvg(const unsigned char *bytes, size_t len);

// The document's size in pixels: its width/height, else its viewBox's.
// False when it is not an SVG or gives neither.
bool Dimensions(const unsigned char *bytes, size_t len, int *width, int *height);

// Renders the document at its own size (scaled down to fit `max_side` on
// its longer side) into straight-alpha RGBA8 `rgba` (width*height*4 bytes,
// transparent where nothing is drawn). Text is set in `font_ttf` (a
// TrueType font's bytes, which must outlive the call); with no font, text
// is left out. False, with `*error` saying why, when it cannot.
bool Rasterize(const unsigned char *bytes, size_t len, std::vector<unsigned char> *rgba, int *width, int *height,
               std::string *error, const unsigned char *font_ttf = nullptr, size_t font_len = 0,
               int max_side = 4096);

}  // namespace svg_raster

#endif
