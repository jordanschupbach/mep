#include "jpeg_codec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace jpeg {

namespace {

// Zigzag-to-natural-order index mapping (JPEG spec Annex A, Figure A.6)
// -- coefficients arrive in this zigzag scan order and must be placed at
// the corresponding natural (row-major) position in the 8x8 block before
// dequantization/IDCT.
constexpr int kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                             12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                             35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                             58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct HuffTable {
    // Canonical JPEG Huffman table representation (spec Annex C): for
    // each code length 1..16, `codes[len]` lists the symbols assigned
    // that length, in the order given by the DHT segment -- codes
    // within a length are consecutive starting from the first unused
    // code value carried over from the previous length (the standard
    // JPEG code-assignment procedure, simpler than DEFLATE's general
    // canonical construction since JPEG already transmits "how many
    // codes of each length" directly).
    bool present = false;
    std::vector<std::vector<uint8_t>> symbols_by_len = std::vector<std::vector<uint8_t>>(17);
    // Decode lookup: min_code[len]/max_code[len]/val_ptr[len] (the
    // classic JPEG reference-decoder structure, spec Annex F.2.2.3).
    int min_code[18] = {0};
    int max_code[18] = {0};
    int val_ptr[18] = {0};
    std::vector<uint8_t> values;  // symbols, flattened in code order

    void Build() {
        values.clear();
        int code = 0;
        for (int len = 1; len <= 16; len++) {
            val_ptr[len] = static_cast<int>(values.size());
            if (symbols_by_len[static_cast<size_t>(len)].empty()) {
                min_code[len] = 0;
                max_code[len] = -1;  // no codes of this length
            } else {
                min_code[len] = code;
                for (uint8_t s : symbols_by_len[static_cast<size_t>(len)]) {
                    values.push_back(s);
                    code++;
                }
                max_code[len] = code - 1;
            }
            code <<= 1;
        }
    }
};

struct Component {
    int id = 0;
    int h = 1, v = 1;      // sampling factors
    int quant_table = 0;
    int dc_table = 0, ac_table = 0;
    int dc_pred = 0;
};

class BitReader {
public:
    BitReader(const unsigned char *data, size_t len) : data_(data), len_(len) {}

    // Returns -1 on a marker/EOF encountered where a data bit was
    // expected (byte-stuffed 0xFF00 is transparently unstuffed and
    // returns the literal 0xFF byte's bits; any other 0xFFxx sequence
    // is a real marker, meaning the entropy-coded segment has ended).
    int GetBit() {
        if (bit_pos_ == 0) {
            if (pos_ >= len_) return -1;
            unsigned char b = data_[pos_];
            if (b == 0xFF) {
                if (pos_ + 1 >= len_) return -1;
                unsigned char next = data_[pos_ + 1];
                if (next == 0x00) {
                    pos_ += 2;  // stuffed byte -- consume both, value is the literal 0xFF
                } else {
                    return -1;  // a real marker (restart, DNL, EOI, ...) -- stop before consuming it
                }
            } else {
                pos_++;
            }
            cur_byte_ = b;
            bit_pos_ = 8;
        }
        bit_pos_--;
        return (cur_byte_ >> bit_pos_) & 1;
    }

    long GetBits(int n) {
        long v = 0;
        for (int i = 0; i < n; i++) {
            int b = GetBit();
            if (b < 0) return -1;
            v = (v << 1) | b;
        }
        return v;
    }

    // Resets to the next byte boundary and skips a restart marker
    // (RST0-RST7, 0xFFD0-0xFFD7) if the stream is now sitting on one --
    // called between MCUs when a restart interval boundary is reached.
    void SyncToRestart() {
        bit_pos_ = 0;
        if (pos_ + 1 < len_ && data_[pos_] == 0xFF && data_[pos_ + 1] >= 0xD0 && data_[pos_ + 1] <= 0xD7) {
            pos_ += 2;
        }
    }

    size_t Pos() const { return pos_; }

private:
    const unsigned char *data_;
    size_t len_;
    size_t pos_ = 0;
    unsigned char cur_byte_ = 0;
    int bit_pos_ = 0;
};

int DecodeHuffmanSymbol(BitReader &br, const HuffTable &table) {
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        int bit = br.GetBit();
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        if (table.max_code[len] >= 0 && code <= table.max_code[len] && code >= table.min_code[len]) {
            int idx = table.val_ptr[len] + (code - table.min_code[len]);
            if (idx < 0 || idx >= static_cast<int>(table.values.size())) return -1;
            return table.values[static_cast<size_t>(idx)];
        }
    }
    return -1;
}

// Reads a JPEG-encoded signed value of `size` bits (spec Annex F.1.2.1's
// "EXTEND" procedure): the raw bits are the magnitude in an offset
// encoding where the top half of the range represents positive values
// and the bottom half represents negative ones.
int ExtendSign(long raw, int size) {
    if (size == 0) return 0;
    long vt = 1L << (size - 1);
    if (raw < vt) return static_cast<int>(raw - (1L << size) + 1);
    return static_cast<int>(raw);
}

bool DecodeBlock(BitReader &br, const HuffTable &dc_table, const HuffTable &ac_table, int *dc_pred, int block[64]) {
    std::memset(block, 0, 64 * sizeof(int));
    int dc_sym = DecodeHuffmanSymbol(br, dc_table);
    if (dc_sym < 0 || dc_sym > 11) return false;
    long dc_bits = dc_sym > 0 ? br.GetBits(dc_sym) : 0;
    if (dc_sym > 0 && dc_bits < 0) return false;
    int diff = ExtendSign(dc_bits, dc_sym);
    *dc_pred += diff;
    block[0] = *dc_pred;

    int k = 1;
    while (k < 64) {
        int rs = DecodeHuffmanSymbol(br, ac_table);
        if (rs < 0) return false;
        int run = rs >> 4, size = rs & 0x0F;
        if (size == 0) {
            if (run == 15) {
                k += 16;  // ZRL: 16 zero coefficients
                continue;
            }
            break;  // EOB
        }
        k += run;
        if (k >= 64) return false;
        long bits = br.GetBits(size);
        if (bits < 0) return false;
        block[kZigzag[k]] = ExtendSign(bits, size);
        k++;
    }
    return true;
}

// Separable 1D-row-then-1D-column IDCT (AAN-style scaled algorithm is
// faster, but this direct floating-point form is simpler to verify
// correct and fast enough -- these are decoded once, not per-frame; see
// STB_IMAGE_REMOVAL_PLAN.md's own Phase 3 note that correctness matters
// more than raw speed here). Operates in place: `block` holds
// dequantized natural-order coefficients in, spatial-domain samples
// (still centered on 0, caller adds the level shift) out.
void Idct8x8(float block[64]) {
    static float cos_table[8][8];
    static bool init = false;
    if (!init) {
        for (int x = 0; x < 8; x++)
            for (int u = 0; u < 8; u++)
                cos_table[x][u] =
                    std::cos(static_cast<float>((2 * x + 1) * u) * 3.14159265358979323846f / 16.0f);
        init = true;
    }
    auto cu = [](int u) { return u == 0 ? 0.70710678118654752440f : 1.0f; };

    float tmp[64];
    // Rows: for each row `y`, compute 1D IDCT along x.
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            float sum = 0.0f;
            for (int u = 0; u < 8; u++) sum += cu(u) * block[y * 8 + u] * cos_table[x][u];
            tmp[y * 8 + x] = sum * 0.5f;
        }
    }
    // Columns: for each column `x`, compute 1D IDCT along y.
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            float sum = 0.0f;
            for (int v = 0; v < 8; v++) sum += cu(v) * tmp[v * 8 + x] * cos_table[y][v];
            block[y * 8 + x] = sum * 0.5f;
        }
    }
}

uint16_t ReadU16BE(const unsigned char *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

}  // namespace

unsigned char *Decode(const unsigned char *data, size_t len, int *width, int *height, std::string *out_error) {
    auto fail = [&](const char *msg) -> unsigned char * {
        if (out_error) *out_error = msg;
        return nullptr;
    };
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) return fail("not a JPEG file");

    uint16_t quant_tables[4][64] = {{0}};
    HuffTable dc_tables[4], ac_tables[4];
    std::vector<Component> comps;
    int width_v = 0, height_v = 0;
    int restart_interval = 0;
    size_t pos = 2;
    bool have_sof = false;

    while (pos + 4 <= len) {
        if (data[pos] != 0xFF) return fail("JPEG: malformed marker segment");
        unsigned char marker = data[pos + 1];
        pos += 2;
        if (marker == 0xD9) break;  // EOI
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;  // no-length markers
        if (pos + 2 > len) return fail("JPEG: truncated marker");
        uint16_t seg_len = ReadU16BE(data + pos);
        if (seg_len < 2 || pos + seg_len > len) return fail("JPEG: truncated marker segment");
        const unsigned char *seg = data + pos + 2;
        uint16_t seg_data_len = static_cast<uint16_t>(seg_len - 2);

        if (marker == 0xDB) {  // DQT
            size_t p = 0;
            while (p < seg_data_len) {
                int precision = seg[p] >> 4, table_id = seg[p] & 0x0F;
                p++;
                if (table_id > 3) return fail("JPEG: invalid quantization table id");
                // DQT elements are transmitted in zigzag scan order (spec
                // Annex B.2.4.1); store dezigzagged so dequantization can
                // index the table by natural (row-major) position, matching
                // how DecodeBlock already places coefficients.
                for (int i = 0; i < 64; i++) {
                    if (precision == 0) {
                        quant_tables[table_id][static_cast<size_t>(kZigzag[i])] = seg[p];
                        p++;
                    } else {
                        quant_tables[table_id][static_cast<size_t>(kZigzag[i])] = ReadU16BE(seg + p);
                        p += 2;
                    }
                }
            }
        } else if (marker == 0xC0) {  // SOF0: baseline DCT
            if (seg[0] != 8) return fail("JPEG: only 8-bit sample precision is supported");
            height_v = ReadU16BE(seg + 1);
            width_v = ReadU16BE(seg + 3);
            int nc = seg[5];
            if (nc != 1 && nc != 3) return fail("JPEG: only grayscale or 3-component (YCbCr) images are supported");
            comps.resize(static_cast<size_t>(nc));
            for (int i = 0; i < nc; i++) {
                const unsigned char *cp = seg + 6 + i * 3;
                comps[static_cast<size_t>(i)].id = cp[0];
                comps[static_cast<size_t>(i)].h = cp[1] >> 4;
                comps[static_cast<size_t>(i)].v = cp[1] & 0x0F;
                comps[static_cast<size_t>(i)].quant_table = cp[2];
            }
            have_sof = true;
        } else if (marker >= 0xC1 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            return fail("JPEG: only baseline (SOF0) sequential DCT is supported, not progressive/arithmetic/lossless");
        } else if (marker == 0xC4) {  // DHT
            size_t p = 0;
            while (p < seg_data_len) {
                int table_class = seg[p] >> 4, table_id = seg[p] & 0x0F;
                p++;
                if (table_id > 3) return fail("JPEG: invalid Huffman table id");
                HuffTable &t = table_class == 0 ? dc_tables[table_id] : ac_tables[table_id];
                int counts[17] = {0};
                int total = 0;
                for (int i = 1; i <= 16; i++) {
                    counts[i] = seg[p + static_cast<size_t>(i) - 1];
                    total += counts[i];
                }
                p += 16;
                t.symbols_by_len.assign(17, {});
                for (int i = 1; i <= 16; i++) {
                    for (int j = 0; j < counts[i]; j++) t.symbols_by_len[static_cast<size_t>(i)].push_back(seg[p++]);
                }
                t.Build();
                t.present = true;
                (void)total;
            }
        } else if (marker == 0xDD) {  // DRI
            restart_interval = ReadU16BE(seg);
        } else if (marker == 0xDA) {  // SOS
            if (!have_sof) return fail("JPEG: SOS before SOF");
            int ns = seg[0];
            for (int i = 0; i < ns; i++) {
                int cid = seg[1 + i * 2];
                int tables = seg[2 + i * 2];
                bool found = false;
                for (Component &c : comps) {
                    if (c.id == cid) {
                        c.dc_table = tables >> 4;
                        c.ac_table = tables & 0x0F;
                        found = true;
                        break;
                    }
                }
                if (!found) return fail("JPEG: SOS references an undeclared component");
            }
            // Entropy-coded data starts right after the SOS segment.
            size_t scan_start = pos + seg_len;

            int h_max = 1, v_max = 1;
            for (const Component &c : comps) {
                h_max = std::max(h_max, c.h);
                v_max = std::max(v_max, c.v);
            }
            int mcu_w = 8 * h_max, mcu_h = 8 * v_max;
            int mcus_x = (width_v + mcu_w - 1) / mcu_w;
            int mcus_y = (height_v + mcu_h - 1) / mcu_h;

            // One full-resolution-of-that-component plane per component
            // (each component's own sample grid, before upsampling).
            std::vector<std::vector<unsigned char>> planes(comps.size());
            std::vector<int> plane_w(comps.size()), plane_h(comps.size());
            for (size_t i = 0; i < comps.size(); i++) {
                plane_w[i] = mcus_x * comps[i].h * 8;
                plane_h[i] = mcus_y * comps[i].v * 8;
                planes[i].assign(static_cast<size_t>(plane_w[i]) * static_cast<size_t>(plane_h[i]), 0);
            }

            BitReader br(data + scan_start, len - scan_start);
            for (Component &c : comps) c.dc_pred = 0;
            int mcus_since_restart = 0;
            for (int my = 0; my < mcus_y; my++) {
                for (int mx = 0; mx < mcus_x; mx++) {
                    for (size_t ci = 0; ci < comps.size(); ci++) {
                        Component &c = comps[ci];
                        if (!dc_tables[c.dc_table].present || !ac_tables[c.ac_table].present) {
                            return fail("JPEG: scan references an undefined Huffman table");
                        }
                        for (int by = 0; by < c.v; by++) {
                            for (int bx = 0; bx < c.h; bx++) {
                                int block[64];
                                if (!DecodeBlock(br, dc_tables[c.dc_table], ac_tables[c.ac_table], &c.dc_pred, block)) {
                                    return fail("JPEG: corrupt entropy-coded data");
                                }
                                float fblock[64];
                                const uint16_t *q = quant_tables[c.quant_table];
                                for (int i = 0; i < 64; i++) fblock[i] = static_cast<float>(block[i] * q[i]);
                                Idct8x8(fblock);
                                int px0 = (mx * c.h + bx) * 8, py0 = (my * c.v + by) * 8;
                                for (int yy = 0; yy < 8; yy++) {
                                    for (int xx = 0; xx < 8; xx++) {
                                        float v = fblock[yy * 8 + xx] + 128.0f;
                                        int iv = static_cast<int>(std::lround(v));
                                        iv = std::clamp(iv, 0, 255);
                                        size_t idx = static_cast<size_t>(py0 + yy) * static_cast<size_t>(plane_w[ci]) +
                                                     static_cast<size_t>(px0 + xx);
                                        planes[ci][idx] = static_cast<unsigned char>(iv);
                                    }
                                }
                            }
                        }
                    }
                    mcus_since_restart++;
                    if (restart_interval > 0 && mcus_since_restart == restart_interval &&
                        !(my == mcus_y - 1 && mx == mcus_x - 1)) {
                        br.SyncToRestart();
                        for (Component &c : comps) c.dc_pred = 0;
                        mcus_since_restart = 0;
                    }
                }
            }

            // Upsample (nearest/box replication -- "fast" non-fancy
            // upsampling, matching stb_image's own default) + color
            // convert + assemble RGBA8 output.
            auto *out = static_cast<unsigned char *>(std::malloc(static_cast<size_t>(width_v) * static_cast<size_t>(height_v) * 4));
            if (!out) return fail("JPEG: out of memory");
            for (int y = 0; y < height_v; y++) {
                for (int x = 0; x < width_v; x++) {
                    unsigned char r, g, b;
                    if (comps.size() == 1) {
                        int sx = x * comps[0].h / h_max, sy = y * comps[0].v / v_max;
                        r = g = b = planes[0][static_cast<size_t>(sy) * static_cast<size_t>(plane_w[0]) + static_cast<size_t>(sx)];
                    } else {
                        int sx0 = x * comps[0].h / h_max, sy0 = y * comps[0].v / v_max;
                        int sx1 = x * comps[1].h / h_max, sy1 = y * comps[1].v / v_max;
                        int sx2 = x * comps[2].h / h_max, sy2 = y * comps[2].v / v_max;
                        float yv = planes[0][static_cast<size_t>(sy0) * static_cast<size_t>(plane_w[0]) + static_cast<size_t>(sx0)];
                        float cb = planes[1][static_cast<size_t>(sy1) * static_cast<size_t>(plane_w[1]) + static_cast<size_t>(sx1)] - 128.0f;
                        float cr = planes[2][static_cast<size_t>(sy2) * static_cast<size_t>(plane_w[2]) + static_cast<size_t>(sx2)] - 128.0f;
                        int rv = static_cast<int>(std::lround(yv + 1.402f * cr));
                        int gv = static_cast<int>(std::lround(yv - 0.344136f * cb - 0.714136f * cr));
                        int bv = static_cast<int>(std::lround(yv + 1.772f * cb));
                        r = static_cast<unsigned char>(std::clamp(rv, 0, 255));
                        g = static_cast<unsigned char>(std::clamp(gv, 0, 255));
                        b = static_cast<unsigned char>(std::clamp(bv, 0, 255));
                    }
                    size_t di = (static_cast<size_t>(y) * static_cast<size_t>(width_v) + static_cast<size_t>(x)) * 4;
                    out[di + 0] = r;
                    out[di + 1] = g;
                    out[di + 2] = b;
                    out[di + 3] = 255;
                }
            }
            *width = width_v;
            *height = height_v;
            return out;
        }
        pos += seg_len;
    }
    return fail("JPEG: no scan data found (missing SOS)");
}

}  // namespace jpeg
