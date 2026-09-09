#pragma once

// mep's own in-house GIF decoder -- see STB_IMAGE_REMOVAL_PLAN.md for
// the full writeup. Replaces stb_image.h's GIF path. Decode only (no
// call site ever writes GIF). Always produces RGBA8 output, matching
// every real call site's fixed `desired_channels=4` request.
//
// Scope (STB_IMAGE_REMOVAL_PLAN.md Scoping decision 4): first frame
// only, matching current usage -- no call site requests animation
// frames. LZW decompression (GIF's own variable-code-width dictionary
// scheme, distinct from DEFLATE) is still required.

#include <cstddef>
#include <string>

namespace gif {

// Decodes a GIF file's first frame into freshly-malloc'd RGBA8 pixels
// (row-major, width*4 bytes per row -- the full logical screen size,
// with the first image descriptor's frame composited over a
// transparent/background-filled canvas at its own offset). Returns
// nullptr on failure with `*out_error` set; caller frees a non-null
// result with std::free.
unsigned char *Decode(const unsigned char *data, size_t len, int *width, int *height, std::string *out_error);

}  // namespace gif
