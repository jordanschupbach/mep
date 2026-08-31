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
    /**
     * @brief Decodes an in-memory PNG/JPEG/BMP/GIF byte buffer into an RGBA8 pixel buffer.
     * @param bytes Pointer to the raw encoded image file bytes.
     * @param len Length of the buffer pointed to by bytes, in bytes.
     * @return true on successful decode; false (with Error() set) on a corrupt/unsupported file.
     */
    bool LoadFromMemory(const unsigned char *bytes, size_t len);
    /**
     * @brief Frees the decoded pixel buffer, if one was allocated by LoadFromMemory.
     */
    ~ImageDoc();

    /**
     * @brief Returns the decoded image's width in pixels.
     * @return Width in pixels, or 0 if no image has been successfully loaded.
     */
    int Width() const { return width_; }
    /**
     * @brief Returns the decoded image's height in pixels.
     * @return Height in pixels, or 0 if no image has been successfully loaded.
     */
    int Height() const { return height_; }
    // Row-major RGBA8, width_*height_*4 bytes -- nullptr if LoadFromMemory
    // wasn't called or failed.
    /**
     * @brief Returns the decoded pixel buffer.
     * @return Pointer to row-major RGBA8 pixel data, or nullptr if no image is loaded.
     */
    const unsigned char *Pixels() const { return pixels_; }
    /**
     * @brief Returns the error message from the most recent failed LoadFromMemory call.
     * @return Human-readable error description, or an empty string if the last load succeeded.
     */
    const std::string &Error() const { return error_; }

private:
    unsigned char *pixels_ = nullptr;  // owned, freed via stbi_image_free
    int width_ = 0, height_ = 0;
    std::string error_;
};

// Extension check (case-insensitive) for the raster formats stb_image
// supports that this feature exposes: png, jpg/jpeg, bmp, gif.
/**
 * @brief Checks whether a path's extension names a raster format this module can decode.
 * @param path File path (or bare filename) to check.
 * @return true if the extension (case-insensitive) is png, jpg, jpeg, bmp, or gif.
 */
bool IsImagePath(const std::string &path);

// Decodes a standard base64 string (the wasm build's /read-binary bridge
// response body, see the comment above mep_js_read_file_binary in
// editor.cpp) into raw bytes. Returns an empty vector on malformed input --
// callers treat that the same as "file didn't decode" via ImageDoc's own
// Error().
/**
 * @brief Decodes a standard base64-encoded string into raw bytes.
 * @param b64 The base64-encoded input string (whitespace and '=' padding are tolerated).
 * @return The decoded bytes, or an empty vector if the input contains characters outside the base64 alphabet.
 */
std::vector<unsigned char> Base64Decode(const std::string &b64);

#endif
