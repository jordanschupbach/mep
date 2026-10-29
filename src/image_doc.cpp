#include "image_doc.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "image_codec.h"

bool ImageDoc::LoadFromMemory(const unsigned char *bytes, size_t len) {
    std::string error;
    pixels_ = image_codec::Decode(bytes, len, &width_, &height_, &error);
    if (!pixels_) {
        error_ = error;
        width_ = height_ = 0;
        return false;
    }
    return true;
}

bool ImageDoc::LoadFromRgba(const std::vector<unsigned char> &rgba, int width, int height) {
    if (width <= 0 || height <= 0 ||
        rgba.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4) {
        error_ = "bad pixel buffer";
        return false;
    }
    if (pixels_) std::free(pixels_);
    pixels_ = static_cast<unsigned char *>(std::malloc(rgba.size()));
    if (!pixels_) {
        error_ = "out of memory";
        width_ = height_ = 0;
        return false;
    }
    std::memcpy(pixels_, rgba.data(), rgba.size());
    width_ = width;
    height_ = height;
    return true;
}

ImageDoc::~ImageDoc() {
    if (pixels_) std::free(pixels_);
}

bool IsImagePath(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    // Lowercases a single character for case-insensitive extension comparison.
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "gif";
}

namespace {
std::string LowerExtension(const std::string &path) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}
}  // namespace

bool IsSvgPath(const std::string &path) { return LowerExtension(path) == "svg"; }

bool IsConvertedImagePath(const std::string &path) {
    static const char *const kExts[] = {"webp", "tif", "tiff", "ico", "tga", "ppm", "pgm", "pbm", "pnm", "pam",
                                        "qoi",  "avif", "heic", "heif", "jxl", "jp2", "exr", "dds", "psd"};
    const std::string ext = LowerExtension(path);
    for (const char *e : kExts)
        if (ext == e) return true;
    return false;
}

bool IsViewerImagePath(const std::string &path) {
    return IsImagePath(path) || IsSvgPath(path) || IsConvertedImagePath(path);
}

std::vector<unsigned char> Base64Decode(const std::string &b64) {
    /**
     * @brief Maps one base64 alphabet character to its 6-bit value.
     * @param c The character to decode.
     * @return The character's 6-bit value (0-63), or -1 if c is not a base64 alphabet character.
     */
    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<unsigned char> out;
    out.reserve(b64.size() / 4 * 3);
    int buf = 0, bits = 0;
    for (char c : b64) {
        if (c == '=' || std::isspace(static_cast<unsigned char>(c))) continue;
        int v = decode_char(c);
        if (v < 0) return {};  // malformed input
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}
