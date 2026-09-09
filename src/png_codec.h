#pragma once

// mep's own in-house PNG decoder/encoder -- see
// STB_IMAGE_REMOVAL_PLAN.md for the full writeup. Replaces stb_image.h's
// PNG path (a subset of gfx/backend_native_renderer2d.cpp's/
// image_doc.cpp's stbi_load*) and stb_image_write.h's stbi_write_png*.
// Decoding always produces RGBA8 output regardless of the source PNG's
// own color type/bit depth, matching every real call site's own
// `desired_channels=4` request (see that plan's Phase 1 survey -- no
// caller ever reads the original channel count back). Compression uses
// deflate.h's zlib-wrapped codec (Scoping decision 1: PNG's IDAT chunks
// are RFC 1950 zlib streams, the exact format MINIZ_REMOVAL_PLAN.md's
// codec already produces/consumes).
//
// Supports: color types 0/2/3/4/6 (grayscale, truecolor, indexed,
// grayscale+alpha, truecolor+alpha), bit depths 1/2/4/8/16, tRNS
// transparency (both the indexed per-palette-entry form and the
// grayscale/truecolor single-transparent-value form), multi-chunk IDAT
// concatenation. Does NOT support Adam7 interlacing (rejected with an
// error -- see that plan's Scoping decision, no real-world fixture
// needs it).

#include <cstddef>
#include <string>

namespace png {

// Decodes a PNG file's bytes into freshly-malloc'd RGBA8 pixels
// (row-major, no row padding -- width*4 bytes per row). Returns nullptr
// on failure with `*out_error` set; caller frees a non-null result with
// std::free (matches stbi_image_free's contract).
unsigned char *Decode(const unsigned char *data, size_t len, int *width, int *height, std::string *out_error);

// Encodes pixel data (row-major, `stride_bytes` per row, `comp` channels
// per pixel: 1=gray, 2=gray+alpha, 3=RGB, 4=RGBA, all at 8 bits/channel)
// into a complete, ready-to-write PNG file. Empty string on failure
// (width/height <= 0).
std::string Encode(int width, int height, int comp, const unsigned char *pixels, int stride_bytes);

}  // namespace png
