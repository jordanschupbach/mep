#pragma once

// mep's own in-house baseline JPEG decoder -- see
// STB_IMAGE_REMOVAL_PLAN.md for the full writeup. Replaces stb_image.h's
// JPEG path. Decode only (no call site ever writes JPEG -- see that
// plan's Phase 1 survey); always produces RGBA8 output (alpha always
// 255 -- JPEG has no alpha channel), matching every real call site's
// fixed `desired_channels=4` request.
//
// Baseline sequential DCT only (Huffman-coded, SOF0) -- see that plan's
// Scoping decision 2: progressive JPEG (SOF2) and arithmetic coding are
// out of scope, unused by anything in-tree.

#include <cstddef>
#include <string>

namespace jpeg {

// Decodes a JPEG file's bytes into freshly-malloc'd RGBA8 pixels
// (row-major, width*4 bytes per row, alpha always 255). Returns nullptr
// on failure with `*out_error` set; caller frees a non-null result with
// std::free.
unsigned char *Decode(const unsigned char *data, size_t len, int *width, int *height, std::string *out_error);

}  // namespace jpeg
