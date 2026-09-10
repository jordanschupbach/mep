#pragma once

// mep's own in-house baseline JPEG codec -- see STB_IMAGE_REMOVAL_PLAN.md
// for the decoder's original writeup. Replaces stb_image.h's JPEG path.
// Decode always produces RGBA8 output (alpha always 255 -- JPEG has no
// alpha channel), matching every real call site's fixed
// `desired_channels=4` request.
//
// Encode was added for ANIMATION_VIDEO_PLAN.md Phase 2, whose Motion-JPEG
// `.mov` export is the first real call site that writes JPEG -- until
// then this really was decode-only. Baseline sequential DCT only
// (Huffman-coded, SOF0) in both directions -- progressive JPEG (SOF2)
// and arithmetic coding remain out of scope, unused by anything in-tree.

#include <cstddef>
#include <string>

namespace jpeg {

// Decodes a JPEG file's bytes into freshly-malloc'd RGBA8 pixels
// (row-major, width*4 bytes per row, alpha always 255). Returns nullptr
// on failure with `*out_error` set; caller frees a non-null result with
// std::free.
unsigned char *Decode(const unsigned char *data, size_t len, int *width, int *height, std::string *out_error);

// Encodes pixel data (row-major, `stride_bytes` per row, `comp` channels
// per pixel: 1=gray, 3=RGB, 4=RGBA with alpha silently dropped -- JPEG
// has no alpha channel, so unlike png::Encode there is no 2=gray+alpha
// case) into a complete baseline sequential JFIF file. `quality` is
// 1-100 (IJG-style quantization scaling, clamped). Empty string on
// failure (width/height <= 0, or an unsupported `comp`).
std::string Encode(int width, int height, int comp, const unsigned char *pixels, int stride_bytes, int quality);

}  // namespace jpeg
