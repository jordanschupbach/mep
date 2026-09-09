#include "image_doc.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

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
