#pragma once

// Standard base64 encoding.
//
// Header-only and in its own file because two unrelated subsystems need
// it and neither should have to link the other: model3d_doc.cpp writes
// glTF `data:` URIs with it, and the PDF pane hands a rendered page to a
// model as an image. It used to be a file-local copy in model3d_doc.cpp;
// putting it beside Base64Decode in image_doc.h instead would have made
// every glTF consumer link the image decoder for 25 lines of arithmetic.
//
// The inverse, Base64Decode, stays in image_doc.h where its own caller
// (the wasm read-binary bridge) already lives.

#include <cstddef>
#include <string>

namespace base64 {

/**
 * @brief Encodes raw bytes as standard base64 with '=' padding.
 * @param data The bytes to encode.
 * @param len How many bytes to read from `data`.
 * @return The base64 text, unwrapped (no line breaks), as the `data:`
 *         URI and JSON payload forms both want it.
 */
inline std::string Encode(const unsigned char *data, size_t len) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const unsigned int n = (static_cast<unsigned int>(data[i]) << 16) |
                               (static_cast<unsigned int>(data[i + 1]) << 8) |
                               static_cast<unsigned int>(data[i + 2]);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back(kTable[n & 0x3F]);
    }
    const size_t rem = len - i;
    if (rem == 1) {
        const unsigned int n = static_cast<unsigned int>(data[i]) << 16;
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const unsigned int n =
            (static_cast<unsigned int>(data[i]) << 16) | (static_cast<unsigned int>(data[i + 1]) << 8);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

}  // namespace base64
