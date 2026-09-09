#include "image_codec.h"

#include <cstdio>

#include "bmp_codec.h"
#include "gif_codec.h"
#include "jpeg_codec.h"
#include "png_codec.h"

namespace image_codec {

unsigned char *Decode(const unsigned char *data, size_t len, int *width, int *height, std::string *out_error) {
    if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return png::Decode(data, len, width, height, out_error);
    }
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
        return jpeg::Decode(data, len, width, height, out_error);
    }
    if (len >= 2 && data[0] == 'B' && data[1] == 'M') {
        return bmp::Decode(data, len, width, height, out_error);
    }
    if (len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') {
        return gif::Decode(data, len, width, height, out_error);
    }
    if (out_error) *out_error = "unrecognized image format (not PNG/JPEG/BMP/GIF)";
    return nullptr;
}

unsigned char *DecodeFile(const char *path, int *width, int *height, std::string *out_error) {
    std::FILE *fp = std::fopen(path, "rb");
    if (!fp) {
        if (out_error) *out_error = "could not open file";
        return nullptr;
    }
    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(fp);
        if (out_error) *out_error = "empty or unreadable file";
        return nullptr;
    }
    std::string buf(static_cast<size_t>(size), '\0');
    size_t n = std::fread(buf.data(), 1, static_cast<size_t>(size), fp);
    std::fclose(fp);
    if (n != static_cast<size_t>(size)) {
        if (out_error) *out_error = "short read";
        return nullptr;
    }
    return Decode(reinterpret_cast<const unsigned char *>(buf.data()), buf.size(), width, height, out_error);
}

}  // namespace image_codec
