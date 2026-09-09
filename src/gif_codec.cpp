#include "gif_codec.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gif {

namespace {

uint16_t ReadU16LE(const unsigned char *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

struct RgbColor {
    unsigned char r = 0, g = 0, b = 0;
};

// GIF's own LZW variant: codes start at `min_code_size + 1` bits, grow
// by one bit each time the dictionary fills the current width (up to a
// 12-bit cap), and a Clear Code resets the dictionary/width at any
// point. Distinct from TIFF LZW (fixed growth points) and from
// DEFLATE's LZ77+Huffman (a completely different compression scheme).
class LzwDecoder {
public:
    LzwDecoder(const unsigned char *data, size_t len, int min_code_size)
        : data_(data), len_(len), min_code_size_(min_code_size) {
        clear_code_ = 1 << min_code_size;
        end_code_ = clear_code_ + 1;
        ResetTable();
    }

    // Decodes the entire LZW-compressed stream (already de-sub-blocked
    // into one contiguous buffer) into indices, one byte per pixel.
    // Returns false on a malformed stream.
    bool Decode(std::vector<unsigned char> *out) {
        int prev_code = -1;
        for (;;) {
            int code = ReadCode();
            if (code < 0) return false;
            if (code == clear_code_) {
                ResetTable();
                prev_code = -1;
                continue;
            }
            if (code == end_code_) return true;

            std::vector<unsigned char> entry;
            if (code < next_code_ && !table_[static_cast<size_t>(code)].empty()) {
                entry = table_[static_cast<size_t>(code)];
            } else if (code == next_code_ && prev_code >= 0) {
                entry = table_[static_cast<size_t>(prev_code)];
                entry.push_back(entry[0]);
            } else {
                return false;  // invalid code
            }
            out->insert(out->end(), entry.begin(), entry.end());

            if (prev_code >= 0 && next_code_ < 4096) {
                std::vector<unsigned char> new_entry = table_[static_cast<size_t>(prev_code)];
                new_entry.push_back(entry[0]);
                table_[static_cast<size_t>(next_code_)] = std::move(new_entry);
                next_code_++;
                if (next_code_ == (1 << code_size_) && code_size_ < 12) code_size_++;
            }
            prev_code = code;
        }
    }

private:
    void ResetTable() {
        table_.assign(4096, {});
        for (int i = 0; i < clear_code_; i++) table_[static_cast<size_t>(i)] = {static_cast<unsigned char>(i)};
        next_code_ = end_code_ + 1;
        code_size_ = min_code_size_ + 1;
    }

    int ReadCode() {
        int code = 0;
        for (int i = 0; i < code_size_; i++) {
            if (byte_pos_ >= len_) return -1;
            int bit = (data_[byte_pos_] >> bit_pos_) & 1;
            code |= bit << i;
            bit_pos_++;
            if (bit_pos_ == 8) {
                bit_pos_ = 0;
                byte_pos_++;
            }
        }
        return code;
    }

    const unsigned char *data_;
    size_t len_;
    int min_code_size_;
    int clear_code_;
    int end_code_;
    int next_code_ = 0;
    int code_size_ = 0;
    std::vector<std::vector<unsigned char>> table_;
    size_t byte_pos_ = 0;
    int bit_pos_ = 0;
};

// Concatenates a GIF sub-block sequence (each block: 1 length byte +
// that many data bytes, terminated by a zero-length block) into one
// buffer, advancing `*pos` past the terminator.
bool ReadSubBlocks(const unsigned char *data, size_t len, size_t *pos, std::vector<unsigned char> *out) {
    for (;;) {
        if (*pos >= len) return false;
        unsigned char block_len = data[*pos];
        (*pos)++;
        if (block_len == 0) return true;
        if (*pos + block_len > len) return false;
        out->insert(out->end(), data + *pos, data + *pos + block_len);
        *pos += block_len;
    }
}

// Skips a sub-block sequence without collecting it (used for extension
// blocks that this decoder doesn't need the content of).
bool SkipSubBlocks(const unsigned char *data, size_t len, size_t *pos) {
    for (;;) {
        if (*pos >= len) return false;
        unsigned char block_len = data[*pos];
        (*pos)++;
        if (block_len == 0) return true;
        if (*pos + block_len > len) return false;
        *pos += block_len;
    }
}

}  // namespace

unsigned char *Decode(const unsigned char *data, size_t len, int *width, int *height, std::string *out_error) {
    auto fail = [&](const char *msg) -> unsigned char * {
        if (out_error) *out_error = msg;
        return nullptr;
    };
    if (len < 13 || (std::memcmp(data, "GIF87a", 6) != 0 && std::memcmp(data, "GIF89a", 6) != 0)) {
        return fail("not a GIF file");
    }

    int screen_w = ReadU16LE(data + 6);
    int screen_h = ReadU16LE(data + 8);
    unsigned char packed = data[10];
    unsigned char bg_color_index = data[11];
    bool has_global_table = (packed & 0x80) != 0;
    int global_table_size = 2 << (packed & 0x07);

    std::vector<RgbColor> global_table;
    size_t pos = 13;
    if (has_global_table) {
        if (pos + static_cast<size_t>(global_table_size) * 3 > len) return fail("GIF: truncated global color table");
        global_table.resize(static_cast<size_t>(global_table_size));
        for (int i = 0; i < global_table_size; i++) {
            global_table[static_cast<size_t>(i)] = {data[pos], data[pos + 1], data[pos + 2]};
            pos += 3;
        }
    }
    if (screen_w <= 0 || screen_h <= 0) return fail("GIF: invalid logical screen size");

    int transparent_index = -1;

    for (;;) {
        if (pos >= len) return fail("GIF: unexpected end of file before an image descriptor");
        unsigned char block_type = data[pos];
        pos++;

        if (block_type == 0x3B) {
            return fail("GIF: no image descriptor found (file ends at trailer)");
        }
        if (block_type == 0x21) {  // Extension
            if (pos >= len) return fail("GIF: truncated extension");
            unsigned char label = data[pos];
            pos++;
            if (label == 0xF9) {  // Graphic Control Extension
                if (pos >= len) return fail("GIF: truncated graphic control extension");
                unsigned char sub_block_size = data[pos];
                if (sub_block_size != 4 || pos + 1 + 4 > len) return fail("GIF: malformed graphic control extension");
                unsigned char gce_packed = data[pos + 1];
                unsigned char trans_idx = data[pos + 1 + 3];
                if (gce_packed & 0x01) transparent_index = trans_idx;
                pos += 1 + sub_block_size;
                unsigned char terminator = pos < len ? data[pos] : 0xFF;
                if (terminator == 0) pos++;
            } else {
                if (!SkipSubBlocks(data, len, &pos)) return fail("GIF: truncated extension sub-blocks");
            }
            continue;
        }
        if (block_type != 0x2C) return fail("GIF: unexpected block type in stream");

        // Image Descriptor.
        if (pos + 9 > len) return fail("GIF: truncated image descriptor");
        int img_left = ReadU16LE(data + pos);
        int img_top = ReadU16LE(data + pos + 2);
        int img_w = ReadU16LE(data + pos + 4);
        int img_h = ReadU16LE(data + pos + 6);
        unsigned char img_packed = data[pos + 8];
        pos += 9;
        bool has_local_table = (img_packed & 0x80) != 0;
        bool interlaced = (img_packed & 0x40) != 0;
        int local_table_size = 2 << (img_packed & 0x07);

        const std::vector<RgbColor> *palette = &global_table;
        std::vector<RgbColor> local_table;
        if (has_local_table) {
            if (pos + static_cast<size_t>(local_table_size) * 3 > len) return fail("GIF: truncated local color table");
            local_table.resize(static_cast<size_t>(local_table_size));
            for (int i = 0; i < local_table_size; i++) {
                local_table[static_cast<size_t>(i)] = {data[pos], data[pos + 1], data[pos + 2]};
                pos += 3;
            }
            palette = &local_table;
        }
        if (palette->empty()) return fail("GIF: no color table available for image data");
        if (img_w <= 0 || img_h <= 0) return fail("GIF: invalid image dimensions");

        if (pos >= len) return fail("GIF: truncated image data");
        int min_code_size = data[pos];
        pos++;
        if (min_code_size < 2 || min_code_size > 8) return fail("GIF: invalid LZW minimum code size");

        std::vector<unsigned char> compressed;
        if (!ReadSubBlocks(data, len, &pos, &compressed)) return fail("GIF: truncated image sub-blocks");

        std::vector<unsigned char> indices;
        indices.reserve(static_cast<size_t>(img_w) * static_cast<size_t>(img_h));
        LzwDecoder lzw(compressed.data(), compressed.size(), min_code_size);
        if (!lzw.Decode(&indices)) return fail("GIF: corrupt LZW data");
        if (indices.size() < static_cast<size_t>(img_w) * static_cast<size_t>(img_h)) {
            return fail("GIF: LZW stream shorter than image dimensions");
        }

        // De-interlace into normal top-to-bottom row order if needed
        // (GIF's 4-pass interlace: rows 0,8,16,... then 4,12,20,...
        // then 2,6,10,... then 1,3,5,...).
        std::vector<unsigned char> deinterlaced;
        const unsigned char *row_source = indices.data();
        if (interlaced) {
            deinterlaced.resize(static_cast<size_t>(img_w) * static_cast<size_t>(img_h));
            static const int kStarts[4] = {0, 4, 2, 1};
            static const int kSteps[4] = {8, 8, 4, 2};
            size_t src_row = 0;
            for (int pass = 0; pass < 4; pass++) {
                for (int y = kStarts[pass]; y < img_h; y += kSteps[pass]) {
                    std::memcpy(deinterlaced.data() + static_cast<size_t>(y) * static_cast<size_t>(img_w),
                                indices.data() + src_row * static_cast<size_t>(img_w), static_cast<size_t>(img_w));
                    src_row++;
                }
            }
            row_source = deinterlaced.data();
        }

        auto *out = static_cast<unsigned char *>(
            std::malloc(static_cast<size_t>(screen_w) * static_cast<size_t>(screen_h) * 4));
        if (!out) return fail("GIF: out of memory");
        RgbColor bg = (has_global_table && bg_color_index < global_table.size())
                          ? global_table[bg_color_index]
                          : RgbColor{0, 0, 0};
        for (int y = 0; y < screen_h; y++) {
            for (int x = 0; x < screen_w; x++) {
                size_t di = (static_cast<size_t>(y) * static_cast<size_t>(screen_w) + static_cast<size_t>(x)) * 4;
                out[di + 0] = bg.r;
                out[di + 1] = bg.g;
                out[di + 2] = bg.b;
                out[di + 3] = (transparent_index >= 0) ? static_cast<unsigned char>(0) : static_cast<unsigned char>(255);
            }
        }
        for (int y = 0; y < img_h; y++) {
            int dy = img_top + y;
            if (dy < 0 || dy >= screen_h) continue;
            for (int x = 0; x < img_w; x++) {
                int dx = img_left + x;
                if (dx < 0 || dx >= screen_w) continue;
                unsigned char idx = row_source[static_cast<size_t>(y) * static_cast<size_t>(img_w) + static_cast<size_t>(x)];
                if (idx >= palette->size()) continue;
                if (static_cast<int>(idx) == transparent_index) continue;  // leave background/prior pixel showing
                RgbColor c = (*palette)[idx];
                size_t di = (static_cast<size_t>(dy) * static_cast<size_t>(screen_w) + static_cast<size_t>(dx)) * 4;
                out[di + 0] = c.r;
                out[di + 1] = c.g;
                out[di + 2] = c.b;
                out[di + 3] = 255;
            }
        }

        *width = screen_w;
        *height = screen_h;
        return out;
    }
}

}  // namespace gif
