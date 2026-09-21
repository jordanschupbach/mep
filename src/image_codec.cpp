#include "image_codec.h"

#include <cstdint>
#include <cstdio>

#include "bmp_codec.h"
#include "gif_codec.h"
#include "jpeg_codec.h"
#include "png_codec.h"

namespace image_codec {
namespace {

// Reads the whole of `path` into `out`. Same failure vocabulary the two
// public *File entry points below report through `*out_error`.
/**
 * @brief Reads a file's entire contents into `out`.
 * @param path Filesystem path to read.
 * @param out Receives the file's bytes on success.
 * @param out_error Set to a short reason when the read fails.
 * @return True on a complete read, false otherwise.
 */
bool ReadWholeFile(const char *path, std::string *out, std::string *out_error) {
    std::FILE *fp = std::fopen(path, "rb");
    if (!fp) {
        if (out_error) *out_error = "could not open file";
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(fp);
        if (out_error) *out_error = "empty or unreadable file";
        return false;
    }
    out->assign(static_cast<size_t>(size), '\0');
    size_t n = std::fread(out->data(), 1, static_cast<size_t>(size), fp);
    std::fclose(fp);
    if (n != static_cast<size_t>(size)) {
        if (out_error) *out_error = "short read";
        return false;
    }
    return true;
}

unsigned ReadBE16(const unsigned char *p) { return (unsigned{p[0]} << 8) | unsigned{p[1]}; }
unsigned ReadBE32(const unsigned char *p) {
    return (unsigned{p[0]} << 24) | (unsigned{p[1]} << 16) | (unsigned{p[2]} << 8) | unsigned{p[3]};
}
unsigned ReadLE16(const unsigned char *p) { return unsigned{p[0]} | (unsigned{p[1]} << 8); }
unsigned ReadLE32(const unsigned char *p) {
    return unsigned{p[0]} | (unsigned{p[1]} << 8) | (unsigned{p[2]} << 16) | (unsigned{p[3]} << 24);
}

// JPEG carries its size in an SOF ("start of frame") marker segment,
// which sits after however many APPn/DQT/DHT/COM segments the encoder
// chose to emit -- so unlike the other three formats this needs a walk
// rather than a fixed offset. Every SOFn (0xC0..0xCF) has the same
// header layout, so this accepts progressive/arithmetic frames too even
// though jpeg::Decode itself only handles baseline: the dimensions are
// readable either way, and the caller learns about an unsupported frame
// from the decode, not from here.
/**
 * @brief Walks a JPEG's marker chain to its SOF segment and reads the frame dimensions.
 * @param data The file's bytes (starting at the SOI marker).
 * @param len Byte count.
 * @param width Receives the frame width.
 * @param height Receives the frame height.
 * @return True if an SOF segment was found and read.
 */
bool JpegDimensions(const unsigned char *data, size_t len, int *width, int *height) {
    size_t p = 2;  // past SOI
    while (p + 1 < len) {
        if (data[p] != 0xFF) {  // not at a marker: resync rather than give up
            p++;
            continue;
        }
        const unsigned char m = data[p + 1];
        if (m == 0xFF) {  // fill byte, the marker's real code is the next one
            p++;
            continue;
        }
        // Standalone markers (no length field of their own).
        if (m == 0x01 || m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) {
            p += 2;
            continue;
        }
        if (m == 0xD9) break;  // EOI
        if (p + 4 > len) break;
        const unsigned seg_len = ReadBE16(data + p + 2);
        if (seg_len < 2) break;
        // SOF0..SOF15, minus the three codes in that range that mean
        // something else entirely (DHT, JPGA, DAC).
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (p + 9 > len) break;
            *height = static_cast<int>(ReadBE16(data + p + 5));
            *width = static_cast<int>(ReadBE16(data + p + 7));
            return *width > 0 && *height > 0;
        }
        if (m == 0xDA) break;  // SOS: entropy-coded scan data, no SOF beyond it
        p += 2 + seg_len;
    }
    return false;
}

}  // namespace

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
    std::string buf;
    if (!ReadWholeFile(path, &buf, out_error)) return nullptr;
    return Decode(reinterpret_cast<const unsigned char *>(buf.data()), buf.size(), width, height, out_error);
}

bool Dimensions(const unsigned char *data, size_t len, int *width, int *height, std::string *out_error) {
    int w = 0, h = 0;
    if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        // IHDR is required to be the first chunk: 8-byte signature, then
        // the chunk's own 4-byte length + 4-byte type, then width/height.
        if (len >= 24 && data[12] == 'I' && data[13] == 'H' && data[14] == 'D' && data[15] == 'R') {
            w = static_cast<int>(ReadBE32(data + 16));
            h = static_cast<int>(ReadBE32(data + 20));
        }
    } else if (len >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
        JpegDimensions(data, len, &w, &h);
    } else if (len >= 2 && data[0] == 'B' && data[1] == 'M') {
        // The DIB header's own size distinguishes the ancient 12-byte
        // BITMAPCOREHEADER (16-bit dimensions) from BITMAPINFOHEADER and
        // its successors (32-bit, and a negative height meaning top-down
        // row order -- a sign bit, not part of the size).
        if (len >= 22) {
            const unsigned dib = ReadLE32(data + 14);
            if (dib == 12) {
                w = static_cast<std::int16_t>(ReadLE16(data + 18));
                h = static_cast<std::int16_t>(ReadLE16(data + 20));
            } else if (len >= 26) {
                w = static_cast<std::int32_t>(ReadLE32(data + 18));
                h = static_cast<std::int32_t>(ReadLE32(data + 22));
            }
            if (h < 0) h = -h;
            if (w < 0) w = -w;
        }
    } else if (len >= 10 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') {
        // The logical screen descriptor, which is also the canvas size
        // gif::Decode composites the first frame onto.
        w = static_cast<int>(ReadLE16(data + 6));
        h = static_cast<int>(ReadLE16(data + 8));
    } else {
        if (out_error) *out_error = "unrecognized image format (not PNG/JPEG/BMP/GIF)";
        return false;
    }
    if (w <= 0 || h <= 0) {
        if (out_error) *out_error = "truncated or malformed image header";
        return false;
    }
    if (width) *width = w;
    if (height) *height = h;
    return true;
}

bool DimensionsFile(const char *path, int *width, int *height, std::string *out_error) {
    std::string buf;
    if (!ReadWholeFile(path, &buf, out_error)) return false;
    return Dimensions(reinterpret_cast<const unsigned char *>(buf.data()), buf.size(), width, height, out_error);
}

}  // namespace image_codec
