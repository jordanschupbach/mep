#ifndef MEP_IMAGE_DOC_H
#define MEP_IMAGE_DOC_H

#include <cstddef>
#include <string>
#include <vector>

// Deliberately raylib-free (same reasoning as vterm.h): decoding is pure
// CPU-side pixel data, so it's usable/testable without a GL context or any
// rendering library at all. main.cpp is the only place that turns the
// decoded RGBA8 buffer into a raylib Texture2D for drawing.
class ImageDoc {
public:
    // Decodes a PNG/JPEG/BMP/GIF byte buffer (whatever stb_image.h supports)
    // via stbi_load_from_memory, forcing 4 channels (RGBA8) regardless of
    // the source format so main.cpp's texture upload path never needs to
    // branch on channel count. Returns false (and sets Error()) on a
    // corrupt/unsupported file; leaves the ImageDoc in an unloaded state
    // (Width()/Height() == 0, Pixels() == nullptr).
    bool LoadFromMemory(const unsigned char *bytes, size_t len);
    ~ImageDoc();

    int Width() const { return width_; }
    int Height() const { return height_; }
    // Row-major RGBA8, width_*height_*4 bytes -- nullptr if LoadFromMemory
    // wasn't called or failed.
    const unsigned char *Pixels() const { return pixels_; }
    const std::string &Error() const { return error_; }

private:
    unsigned char *pixels_ = nullptr;  // owned, freed via stbi_image_free
    int width_ = 0, height_ = 0;
    std::string error_;
};

// Extension check (case-insensitive) for the raster formats stb_image
// supports that this feature exposes: png, jpg/jpeg, bmp, gif.
bool IsImagePath(const std::string &path);

// Decodes a standard base64 string (the wasm build's /read-binary bridge
// response body, see the comment above mep_js_read_file_binary in
// editor.cpp) into raw bytes. Returns an empty vector on malformed input --
// callers treat that the same as "file didn't decode" via ImageDoc's own
// Error().
std::vector<unsigned char> Base64Decode(const std::string &b64);

#endif
