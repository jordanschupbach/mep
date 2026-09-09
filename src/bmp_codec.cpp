#include "bmp_codec.h"

#include <cstdint>
#include <cstdlib>

namespace bmp {

namespace {

uint32_t ReadU32LE(const unsigned char *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t ReadU16LE(const unsigned char *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
int32_t ReadI32LE(const unsigned char *p) { return static_cast<int32_t>(ReadU32LE(p)); }

// Given a bitfield mask, returns {shift, bit_width} so a raw field
// value can be extracted and rescaled to 8 bits.
void MaskShiftWidth(uint32_t mask, int *shift, int *width) {
    *shift = 0;
    *width = 0;
    if (mask == 0) return;
    while ((mask & 1) == 0) {
        mask >>= 1;
        (*shift)++;
    }
    while (mask & 1) {
        mask >>= 1;
        (*width)++;
    }
}

unsigned char ExtractChannel(uint32_t pixel, uint32_t mask) {
    if (mask == 0) return 0;
    int shift = 0, width = 0;
    MaskShiftWidth(mask, &shift, &width);
    uint32_t raw = (pixel & mask) >> shift;
    uint32_t max_raw = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
    if (max_raw == 0) return 0;
    if (width == 8) return static_cast<unsigned char>(raw);
    return static_cast<unsigned char>((raw * 255u + max_raw / 2u) / max_raw);
}

}  // namespace

unsigned char *Decode(const unsigned char *data, size_t len, int *width, int *height, std::string *out_error) {
    auto fail = [&](const char *msg) -> unsigned char * {
        if (out_error) *out_error = msg;
        return nullptr;
    };
    if (len < 14 + 40 || data[0] != 'B' || data[1] != 'M') return fail("not a BMP file");

    uint32_t pixel_data_offset = ReadU32LE(data + 10);
    uint32_t dib_header_size = ReadU32LE(data + 14);
    if (dib_header_size < 40 || 14u + dib_header_size > len) return fail("BMP: unsupported or truncated DIB header");

    const unsigned char *dib = data + 14;
    int32_t raw_width = ReadI32LE(dib + 4);
    int32_t raw_height = ReadI32LE(dib + 8);
    uint16_t bitcount = ReadU16LE(dib + 14);
    uint32_t compression = ReadU32LE(dib + 16);

    if (raw_width <= 0) return fail("BMP: invalid width");
    if (raw_height == 0) return fail("BMP: invalid height");
    bool top_down = raw_height < 0;
    int w = raw_width;
    int h = top_down ? -raw_height : raw_height;

    if (bitcount != 16 && bitcount != 24 && bitcount != 32) {
        return fail("BMP: only 16/24/32-bit images are supported");
    }
    if (compression != 0 /* BI_RGB */ && compression != 3 /* BI_BITFIELDS */) {
        return fail("BMP: only uncompressed (BI_RGB/BI_BITFIELDS) images are supported");
    }

    uint32_t r_mask = 0, g_mask = 0, b_mask = 0, a_mask = 0;
    size_t pixel_start = pixel_data_offset;
    if (compression == 3) {
        if (dib_header_size >= 56) {
            // BITMAPV3INFOHEADER and later carry R/G/B/A masks inline.
            r_mask = ReadU32LE(dib + 40);
            g_mask = ReadU32LE(dib + 44);
            b_mask = ReadU32LE(dib + 48);
            a_mask = ReadU32LE(dib + 52);
        } else if (dib_header_size >= 52) {
            r_mask = ReadU32LE(dib + 40);
            g_mask = ReadU32LE(dib + 44);
            b_mask = ReadU32LE(dib + 48);
        } else {
            // Classic BITMAPINFOHEADER (40 bytes) + BI_BITFIELDS: three
            // DWORD masks (R,G,B, no alpha) follow the header directly.
            if (len < 14 + 40 + 12) return fail("BMP: truncated bitfield masks");
            r_mask = ReadU32LE(data + 14 + 40);
            g_mask = ReadU32LE(data + 14 + 40 + 4);
            b_mask = ReadU32LE(data + 14 + 40 + 8);
        }
    } else {
        // BI_RGB defaults. 24- and 32-bit share the same byte order (bytes
        // stored B,G,R[,X] low-to-high -- the 32-bit form's 4th byte is
        // reserved/unused, not alpha).
        if (bitcount == 16) {
            r_mask = 0x7C00;
            g_mask = 0x03E0;
            b_mask = 0x001F;
        } else {
            r_mask = 0xFF0000;
            g_mask = 0x00FF00;
            b_mask = 0x0000FF;
        }
    }

    int bytes_per_pixel = bitcount / 8;
    size_t row_stride = (static_cast<size_t>(w) * static_cast<size_t>(bytes_per_pixel) + 3u) & ~static_cast<size_t>(3);
    size_t needed = pixel_start + row_stride * static_cast<size_t>(h);
    if (needed > len) return fail("BMP: truncated pixel data");

    auto *out = static_cast<unsigned char *>(std::malloc(static_cast<size_t>(w) * static_cast<size_t>(h) * 4));
    if (!out) return fail("BMP: out of memory");

    for (int y = 0; y < h; y++) {
        // BMP rows are stored bottom-up unless the height is negative.
        int src_row = top_down ? y : (h - 1 - y);
        const unsigned char *row = data + pixel_start + row_stride * static_cast<size_t>(src_row);
        for (int x = 0; x < w; x++) {
            const unsigned char *px = row + static_cast<size_t>(x) * static_cast<size_t>(bytes_per_pixel);
            uint32_t pixel;
            if (bytes_per_pixel == 2) {
                pixel = ReadU16LE(px);
            } else if (bytes_per_pixel == 3) {
                pixel = static_cast<uint32_t>(px[0]) | (static_cast<uint32_t>(px[1]) << 8) |
                        (static_cast<uint32_t>(px[2]) << 16);
            } else {
                pixel = ReadU32LE(px);
            }
            size_t di = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            out[di + 0] = ExtractChannel(pixel, r_mask);
            out[di + 1] = ExtractChannel(pixel, g_mask);
            out[di + 2] = ExtractChannel(pixel, b_mask);
            out[di + 3] = a_mask != 0 ? ExtractChannel(pixel, a_mask) : static_cast<unsigned char>(255);
        }
    }

    *width = w;
    *height = h;
    return out;
}

}  // namespace bmp
