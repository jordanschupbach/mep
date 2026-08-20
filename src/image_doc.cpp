#include "image_doc.h"

#include <algorithm>
#include <cctype>

#define STB_IMAGE_IMPLEMENTATION
// raylib's own rtextures.c already compiles its own copy of stb_image.h
// (STB_IMAGE_IMPLEMENTATION, no STATIC) into libraylib.a -- without this,
// both TUs emit non-static stbi_* symbols and the native link fails with
// "multiple definition of `stbi_load_from_memory'" etc. STATIC restricts
// every stbi_* symbol here to internal linkage, so this TU's copy can't
// collide with raylib's.
#define STB_IMAGE_STATIC
// Only the formats this feature exposes (see IsImagePath below) need
// decoding -- trims stb_image's compiled surface (and its dependency on
// stb_image_resize/HDR/PSD/PIC/PNM parsing) down to what's actually reached.
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#include "../third_party/stb_image.h"

bool ImageDoc::LoadFromMemory(const unsigned char *bytes, size_t len) {
    int channels = 0;
    pixels_ = stbi_load_from_memory(bytes, static_cast<int>(len), &width_, &height_, &channels, 4);
    if (!pixels_) {
        error_ = stbi_failure_reason() ? stbi_failure_reason() : "unknown decode error";
        width_ = height_ = 0;
        return false;
    }
    return true;
}

ImageDoc::~ImageDoc() {
    if (pixels_) stbi_image_free(pixels_);
}

bool IsImagePath(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "gif";
}

std::vector<unsigned char> Base64Decode(const std::string &b64) {
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
