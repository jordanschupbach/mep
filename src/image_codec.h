#pragma once

// Format-sniffing dispatcher over mep's in-house PNG/JPEG/BMP/GIF
// decoders (png_codec.h, jpeg_codec.h, bmp_codec.h, gif_codec.h) --
// see STB_IMAGE_REMOVAL_PLAN.md. Replaces stb_image.h's
// stbi_load/stbi_load_from_memory at every real call site (all of
// which request exactly `desired_channels=4`, so this dispatcher
// always produces RGBA8 like they did).

#include <cstddef>
#include <string>

namespace image_codec {

// Detects the format from the first bytes of `data` (PNG/JPEG/BMP/GIF
// magic) and decodes into freshly-malloc'd RGBA8 pixels (row-major,
// width*4 bytes per row). Returns nullptr on failure (unrecognized
// format or a real decode error) with `*out_error` set; caller frees a
// non-null result with std::free.
unsigned char *Decode(const unsigned char *data, size_t len, int *width, int *height, std::string *out_error);

// Reads `path` and decodes it the same way. Returns nullptr (with
// `*out_error` set) if the file can't be opened or fails to decode.
unsigned char *DecodeFile(const char *path, int *width, int *height, std::string *out_error);

}  // namespace image_codec
