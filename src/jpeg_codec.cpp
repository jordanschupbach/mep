#include "jpeg_codec.h"

#include <algorithm>
#include <array>
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

// ===================== Encoder =====================
//
// Standard Annex-K quantization/Huffman tables below are used as
// reasonable defaults, not because encode/decode round-trip correctness
// depends on matching the spec's published byte values exactly: a JFIF
// DQT/DHT segment is self-describing (the file carries whatever table it
// actually used), so any *valid* table -- one satisfying the canonical
// Huffman code-assignment constraints -- produces a fully spec-compliant,
// independently-decodable baseline JPEG regardless of whether these
// happen to be bit-for-bit the "official" Annex K values. Using the
// well-known standard tables is still the right choice for broad
// interoperability with real-world decoders, which is the whole reason
// the fully in-house Motion-JPEG approach (ANIMATION_VIDEO_PLAN.md Phase
// 2) was chosen over a simpler non-standard per-frame codec.

void WriteU16BE(std::string *out, uint16_t v) {
    out->push_back(static_cast<char>((v >> 8) & 0xFF));
    out->push_back(static_cast<char>(v & 0xFF));
}

// Standard IJG luminance/chrominance base quantization tables (natural,
// row-major order -- spec Annex K.1), scaled per component/quality by
// ScaleQuantValue below.
constexpr int kBaseQuantLuma[64] = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
                                     14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
                                     18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
                                     49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr int kBaseQuantChroma[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                       24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                       99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                       99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// Standard IJG quality->scale formula (jcparam.c's jpeg_quality_scaling +
// the base-table application it feeds): quality 50 reproduces the base
// table unchanged; below/above 50 scales down/up asymmetrically. Baseline
// 8-bit precision requires every quantization value to fit one byte
// (1-255), hence the clamp.
uint16_t ScaleQuantValue(int base_val, int quality) {
    quality = std::clamp(quality, 1, 100);
    int scale = quality < 50 ? (5000 / quality) : (200 - quality * 2);
    int v = (base_val * scale + 50) / 100;
    return static_cast<uint16_t>(std::clamp(v, 1, 255));
}

void BuildQuantTable(const int base[64], int quality, uint16_t out_natural[64]) {
    for (int i = 0; i < 64; i++) out_natural[i] = ScaleQuantValue(base[i], quality);
}

// Standard Annex-K Huffman tables: bits[1..16] = how many codes of each
// length (bits[0] unused, kept for 1-indexing symmetry with the spec's
// own numbering); vals = symbols in code order, length-major. DC symbols
// are literal size categories (0-11); AC symbols pack (run<<4|size), 0x00
// = EOB, 0xF0 = ZRL (16 zero run).
constexpr uint8_t kDcLumaBits[17] = {0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t kDcLumaVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr uint8_t kDcChromaBits[17] = {0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
constexpr uint8_t kDcChromaVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr uint8_t kAcLumaBits[17] = {0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
constexpr uint8_t kAcLumaVals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
constexpr uint8_t kAcChromaBits[17] = {0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
constexpr uint8_t kAcChromaVals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

struct EncodeHuffEntry {
    uint16_t code = 0;
    uint8_t length = 0;
};
using EncodeHuffTable = std::array<EncodeHuffEntry, 256>;

// Canonical Huffman code assignment (spec Annex C, Figure C.2) in the
// encode direction (symbol -> code/length) -- the same procedure
// HuffTable::Build above runs in the decode direction (code -> symbol),
// so a table built here from the same bits/vals a DHT segment carries
// always agrees with what HuffTable::Build reconstructs from that
// segment.
EncodeHuffTable BuildEncodeHuffTable(const uint8_t bits[17], const uint8_t *vals) {
    EncodeHuffTable table{};
    int code = 0;
    int vi = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len]; i++) {
            uint8_t symbol = vals[vi++];
            table[symbol].code = static_cast<uint16_t>(code);
            table[symbol].length = static_cast<uint8_t>(len);
            code++;
        }
        code <<= 1;
    }
    return table;
}

// MSB-first bit accumulator with JPEG byte stuffing (any emitted 0xFF
// byte is immediately followed by a stuffed 0x00, matching BitReader's
// own unstuffing above). `nbits` per call is always <=16 here (a Huffman
// code or a DC/AC value's magnitude bits, both well under the type's
// width), so left-shifting the accumulator never needs masking -- bits
// already emitted simply shift out the top of the 32-bit word over time.
class BitWriter {
public:
    void PutBits(uint32_t value, int nbits) {
        if (nbits <= 0) return;
        acc_ = (acc_ << nbits) | (value & ((1u << nbits) - 1u));
        nbits_ += nbits;
        while (nbits_ >= 8) {
            nbits_ -= 8;
            EmitByte(static_cast<unsigned char>((acc_ >> nbits_) & 0xFF));
        }
    }
    void PutHuff(const EncodeHuffTable &table, int symbol) { PutBits(table[static_cast<size_t>(symbol)].code, table[static_cast<size_t>(symbol)].length); }
    // Pads the final partial byte with 1-bits (spec's fill-bit convention)
    // so the entropy-coded segment ends byte-aligned before EOI.
    void Flush() {
        if (nbits_ > 0) {
            unsigned char byte = static_cast<unsigned char>(((acc_ << (8 - nbits_)) | ((1u << (8 - nbits_)) - 1u)) & 0xFF);
            EmitByte(byte);
            nbits_ = 0;
        }
    }
    const std::string &Data() const { return out_; }

private:
    void EmitByte(unsigned char b) {
        out_.push_back(static_cast<char>(b));
        if (b == 0xFF) out_.push_back(static_cast<char>(0x00));
    }
    uint32_t acc_ = 0;
    int nbits_ = 0;
    std::string out_;
};

// Number of bits needed to represent |v| (0 for v==0) -- the JPEG "size
// category" used for both DC diffs and AC coefficients (spec Table
// F.1's SSSS), inverse of Decode's ExtendSign above.
int ValueCategory(int v) {
    int m = v < 0 ? -v : v;
    int size = 0;
    while (m) {
        size++;
        m >>= 1;
    }
    return size;
}

// Encodes `v` (whose category is `size`, i.e. already computed by
// ValueCategory) into the size-bit representation ExtendSign decodes
// back to `v` -- the forward direction of the same EXTEND procedure.
uint32_t ValueBits(int v, int size) {
    if (v >= 0) return static_cast<uint32_t>(v);
    return static_cast<uint32_t>(v + (1 << size) - 1);
}

// Clamps a quantized coefficient to the largest magnitude the given
// category limit can represent. Real photographic/rendered content never
// approaches this, but a synthetic worst-case block (e.g. an exact
// checkerboard at quality=100's quant=1) can theoretically produce a
// coefficient whose category exceeds what the standard AC table's 4-bit
// size nibble (max 10) or the DC table's range (max 11) can carry --
// clamping keeps every emitted symbol valid without ever needing a wider
// table. DC clamps the *diff* actually transmitted, and the running
// dc_pred is advanced by that same clamped diff (see WriteBlockEntropy)
// so encoder and decoder state never diverge, bounding any quality loss
// to the one clamped block instead of drifting for the rest of the scan.
int ClampToCategory(int v, int max_category) {
    int limit = (1 << max_category) - 1;
    return std::clamp(v, -limit, limit);
}
constexpr int kMaxDcCategory = 11;
constexpr int kMaxAcCategory = 10;

// Forward separable 2D DCT-II (spec Annex A.3), the exact transpose of
// Idct8x8 above: same cosine basis table and per-axis 0.5*C(u) scaling,
// just contracting over the spatial index instead of the frequency
// index in each 1D pass. Operates in place: `block` holds level-shifted
// (mean-subtracted) spatial samples in, natural-order frequency
// coefficients out.
void Fdct8x8(float block[64]) {
    static float cos_table[8][8];
    static bool init = false;
    if (!init) {
        for (int x = 0; x < 8; x++)
            for (int u = 0; u < 8; u++)
                cos_table[x][u] = std::cos(static_cast<float>((2 * x + 1) * u) * 3.14159265358979323846f / 16.0f);
        init = true;
    }
    auto cu = [](int u) { return u == 0 ? 0.70710678118654752440f : 1.0f; };

    float tmp[64];
    for (int y = 0; y < 8; y++) {
        for (int u = 0; u < 8; u++) {
            float sum = 0.0f;
            for (int x = 0; x < 8; x++) sum += block[y * 8 + x] * cos_table[x][u];
            tmp[y * 8 + u] = sum * cu(u) * 0.5f;
        }
    }
    for (int u = 0; u < 8; u++) {
        for (int v = 0; v < 8; v++) {
            float sum = 0.0f;
            for (int y = 0; y < 8; y++) sum += tmp[y * 8 + u] * cos_table[y][v];
            block[v * 8 + u] = sum * cu(v) * 0.5f;
        }
    }
}

float SamplePlane(const std::vector<float> &plane, int w, int h, int x, int y) {
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    return plane[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
}

// Fills an 8x8 level-shifted (mean-subtracted, i.e. centered on 0 like
// Idct8x8's own convention) block from `plane` at block origin (bx0,by0)
// -- edge blocks (image dimensions not a multiple of 8) clamp to the
// nearest real pixel (SamplePlane) rather than zero-padding, avoiding an
// artificial hard edge at the image boundary that would otherwise waste
// bits encoding a frequency discontinuity nothing in the source image
// actually has.
void ExtractBlock(const std::vector<float> &plane, int w, int h, int bx0, int by0, float block[64]) {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) block[y * 8 + x] = SamplePlane(plane, w, h, bx0 + x, by0 + y) - 128.0f;
}

void WriteBlockEntropy(BitWriter *bw, const EncodeHuffTable &dc_table, const EncodeHuffTable &ac_table, const int zz[64],
                        int *dc_pred) {
    int dc_diff = ClampToCategory(zz[0] - *dc_pred, kMaxDcCategory);
    *dc_pred += dc_diff;
    int dc_size = ValueCategory(dc_diff);
    bw->PutHuff(dc_table, dc_size);
    if (dc_size > 0) bw->PutBits(ValueBits(dc_diff, dc_size), dc_size);

    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = ClampToCategory(zz[k], kMaxAcCategory);
        if (v == 0) {
            run++;
            continue;
        }
        while (run > 15) {
            bw->PutHuff(ac_table, 0xF0);
            run -= 16;
        }
        int size = ValueCategory(v);
        int symbol = (run << 4) | size;
        bw->PutHuff(ac_table, symbol);
        bw->PutBits(ValueBits(v, size), size);
        run = 0;
    }
    if (run > 0) bw->PutHuff(ac_table, 0x00);
}

void WriteDqtSegment(std::string *out, int id, const uint16_t q_natural[64]) {
    out->push_back(static_cast<char>(0xFF));
    out->push_back(static_cast<char>(0xDB));
    WriteU16BE(out, static_cast<uint16_t>(2 + 1 + 64));
    out->push_back(static_cast<char>(id & 0x0F));  // precision nibble 0 (8-bit) | table id
    for (int k = 0; k < 64; k++) out->push_back(static_cast<char>(q_natural[kZigzag[k]] & 0xFF));
}

void WriteDhtSegment(std::string *out, int table_class, int id, const uint8_t bits[17], const uint8_t *vals) {
    int nvals = 0;
    for (int i = 1; i <= 16; i++) nvals += bits[i];
    out->push_back(static_cast<char>(0xFF));
    out->push_back(static_cast<char>(0xC4));
    WriteU16BE(out, static_cast<uint16_t>(2 + 1 + 16 + nvals));
    out->push_back(static_cast<char>((table_class << 4) | id));
    for (int i = 1; i <= 16; i++) out->push_back(static_cast<char>(bits[i]));
    for (int i = 0; i < nvals; i++) out->push_back(static_cast<char>(vals[i]));
}

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

std::string Encode(int width, int height, int comp, const unsigned char *pixels, int stride_bytes, int quality) {
    if (width <= 0 || height <= 0) return "";
    if (comp != 1 && comp != 3 && comp != 4) return "";
    bool color = comp != 1;

    // RGB(A)->YCbCr (JFIF/BT.601 formula, the exact inverse of Decode's
    // own YCbCr->RGB conversion above) or a direct grayscale copy, once
    // up front -- ExtractBlock below samples these with clamped edges
    // rather than re-converting per block.
    std::vector<float> plane_y(static_cast<size_t>(width) * static_cast<size_t>(height));
    std::vector<float> plane_cb, plane_cr;
    if (color) {
        plane_cb.resize(plane_y.size());
        plane_cr.resize(plane_y.size());
    }
    for (int y = 0; y < height; y++) {
        const unsigned char *row = pixels + static_cast<size_t>(y) * static_cast<size_t>(stride_bytes);
        for (int x = 0; x < width; x++) {
            size_t idx = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
            if (!color) {
                plane_y[idx] = static_cast<float>(row[x]);
                continue;
            }
            const unsigned char *px = row + static_cast<size_t>(x) * static_cast<size_t>(comp);
            float r = static_cast<float>(px[0]), g = static_cast<float>(px[1]), b = static_cast<float>(px[2]);
            plane_y[idx] = 0.299f * r + 0.587f * g + 0.114f * b;
            plane_cb[idx] = -0.168736f * r - 0.331264f * g + 0.5f * b + 128.0f;
            plane_cr[idx] = 0.5f * r - 0.418688f * g - 0.081312f * b + 128.0f;
        }
    }

    uint16_t q_luma[64], q_chroma[64];
    BuildQuantTable(kBaseQuantLuma, quality, q_luma);
    if (color) BuildQuantTable(kBaseQuantChroma, quality, q_chroma);
    EncodeHuffTable dc_luma_huff = BuildEncodeHuffTable(kDcLumaBits, kDcLumaVals);
    EncodeHuffTable ac_luma_huff = BuildEncodeHuffTable(kAcLumaBits, kAcLumaVals);
    EncodeHuffTable dc_chroma_huff, ac_chroma_huff;
    if (color) {
        dc_chroma_huff = BuildEncodeHuffTable(kDcChromaBits, kDcChromaVals);
        ac_chroma_huff = BuildEncodeHuffTable(kAcChromaBits, kAcChromaVals);
    }

    std::string out;
    out.push_back(static_cast<char>(0xFF));
    out.push_back(static_cast<char>(0xD8));  // SOI

    out.push_back(static_cast<char>(0xFF));
    out.push_back(static_cast<char>(0xE0));  // APP0 (JFIF)
    WriteU16BE(&out, 16);
    out += "JFIF";
    out.push_back(static_cast<char>(0x00));
    out.push_back(static_cast<char>(0x01));  // version 1.01
    out.push_back(static_cast<char>(0x01));
    out.push_back(static_cast<char>(0x00));  // density units: none, aspect ratio only
    WriteU16BE(&out, 1);                     // Xdensity
    WriteU16BE(&out, 1);                     // Ydensity
    out.push_back(static_cast<char>(0x00));  // no thumbnail
    out.push_back(static_cast<char>(0x00));

    WriteDqtSegment(&out, 0, q_luma);
    if (color) WriteDqtSegment(&out, 1, q_chroma);

    int nc = color ? 3 : 1;
    out.push_back(static_cast<char>(0xFF));
    out.push_back(static_cast<char>(0xC0));  // SOF0
    WriteU16BE(&out, static_cast<uint16_t>(2 + 1 + 2 + 2 + 1 + 3 * nc));
    out.push_back(static_cast<char>(8));  // 8-bit sample precision
    WriteU16BE(&out, static_cast<uint16_t>(height));
    WriteU16BE(&out, static_cast<uint16_t>(width));
    out.push_back(static_cast<char>(nc));
    // 4:4:4 -- every component sampled at full resolution (h=v=1), so an
    // MCU is exactly one 8x8 block per component with no chroma
    // upsampling to worry about on the decode side.
    out.push_back(static_cast<char>(1));
    out.push_back(static_cast<char>(0x11));
    out.push_back(static_cast<char>(0));  // quant table 0 (luma)
    if (color) {
        out.push_back(static_cast<char>(2));
        out.push_back(static_cast<char>(0x11));
        out.push_back(static_cast<char>(1));  // quant table 1 (chroma)
        out.push_back(static_cast<char>(3));
        out.push_back(static_cast<char>(0x11));
        out.push_back(static_cast<char>(1));
    }

    WriteDhtSegment(&out, 0, 0, kDcLumaBits, kDcLumaVals);
    WriteDhtSegment(&out, 1, 0, kAcLumaBits, kAcLumaVals);
    if (color) {
        WriteDhtSegment(&out, 0, 1, kDcChromaBits, kDcChromaVals);
        WriteDhtSegment(&out, 1, 1, kAcChromaBits, kAcChromaVals);
    }

    out.push_back(static_cast<char>(0xFF));
    out.push_back(static_cast<char>(0xDA));  // SOS
    WriteU16BE(&out, static_cast<uint16_t>(2 + 1 + 2 * nc + 3));
    out.push_back(static_cast<char>(nc));
    out.push_back(static_cast<char>(1));
    out.push_back(static_cast<char>(0x00));  // component 1: DC table 0, AC table 0
    if (color) {
        out.push_back(static_cast<char>(2));
        out.push_back(static_cast<char>(0x11));  // component 2: DC table 1, AC table 1
        out.push_back(static_cast<char>(3));
        out.push_back(static_cast<char>(0x11));
    }
    out.push_back(static_cast<char>(0));   // Ss
    out.push_back(static_cast<char>(63));  // Se
    out.push_back(static_cast<char>(0));   // Ah/Al

    BitWriter bw;
    int mcus_x = (width + 7) / 8, mcus_y = (height + 7) / 8;
    int dc_pred_y = 0, dc_pred_cb = 0, dc_pred_cr = 0;
    for (int my = 0; my < mcus_y; my++) {
        for (int mx = 0; mx < mcus_x; mx++) {
            int bx0 = mx * 8, by0 = my * 8;
            float block[64];
            int zz[64];
            ExtractBlock(plane_y, width, height, bx0, by0, block);
            Fdct8x8(block);
            for (int k = 0; k < 64; k++) zz[k] = static_cast<int>(std::lround(block[kZigzag[k]] / static_cast<float>(q_luma[kZigzag[k]])));
            WriteBlockEntropy(&bw, dc_luma_huff, ac_luma_huff, zz, &dc_pred_y);
            if (color) {
                ExtractBlock(plane_cb, width, height, bx0, by0, block);
                Fdct8x8(block);
                for (int k = 0; k < 64; k++)
                    zz[k] = static_cast<int>(std::lround(block[kZigzag[k]] / static_cast<float>(q_chroma[kZigzag[k]])));
                WriteBlockEntropy(&bw, dc_chroma_huff, ac_chroma_huff, zz, &dc_pred_cb);

                ExtractBlock(plane_cr, width, height, bx0, by0, block);
                Fdct8x8(block);
                for (int k = 0; k < 64; k++)
                    zz[k] = static_cast<int>(std::lround(block[kZigzag[k]] / static_cast<float>(q_chroma[kZigzag[k]])));
                WriteBlockEntropy(&bw, dc_chroma_huff, ac_chroma_huff, zz, &dc_pred_cr);
            }
        }
    }
    bw.Flush();
    out += bw.Data();

    out.push_back(static_cast<char>(0xFF));
    out.push_back(static_cast<char>(0xD9));  // EOI
    return out;
}

}  // namespace jpeg
