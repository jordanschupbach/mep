#pragma once

// mep's own in-house BMP decoder -- see STB_IMAGE_REMOVAL_PLAN.md for
// the full writeup. Replaces stb_image.h's BMP path. Decode only (no
// call site ever writes BMP). Always produces RGBA8 output, matching
// every real call site's fixed `desired_channels=4` request.
//
// Scope (STB_IMAGE_REMOVAL_PLAN.md Scoping decision 3): uncompressed
// `BI_RGB` only, 24-bit (no alpha, output alpha forced to 255) or
// 32-bit (alpha channel present). RLE-compressed BMP (`BI_RLE8`/
// `BI_RLE4`) is out of scope -- a legacy format essentially never
// produced by modern tools.

#include <cstddef>
#include <string>

namespace bmp {

// Decodes a BMP file's bytes into freshly-malloc'd RGBA8 pixels
// (row-major, top-to-bottom, width*4 bytes per row -- BMP's own
// bottom-up row order, or top-down for a negative-height header, is
// normalized away here). Returns nullptr on failure with `*out_error`
// set; caller frees a non-null result with std::free.
unsigned char *Decode(const unsigned char *data, size_t len, int *width, int *height, std::string *out_error);

}  // namespace bmp
