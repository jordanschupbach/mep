#include "deflate.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace deflate {

// -- CRC-32 (IEEE 802.3 polynomial, reflected) -----------------------------

namespace {
std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}
}  // namespace

uint32_t Crc32(uint32_t crc, const unsigned char *data, size_t len) {
    static const std::array<uint32_t, 256> table = MakeCrcTable();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t Adler32(uint32_t adler, const unsigned char *data, size_t len) {
    uint32_t a = adler & 0xFFFF, b = (adler >> 16) & 0xFFFF;
    constexpr uint32_t kMod = 65521;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

// ===========================================================================
// Shared Huffman machinery (RFC 1951 section 3.2.2's canonical-code
// construction, used by both the fixed tables and dynamic per-block
// tables, on both the decode and encode sides).
// ===========================================================================

namespace {

constexpr int kMaxBits = 15;

// Decode-side canonical Huffman table: count[len] = how many codes of
// that length exist, symbol[] = symbols in canonical code order --
// mirrors the well-known "puff.c" reference approach (RFC 1951's own
// appendix describes the same algorithm).
struct HuffTable {
    std::array<int, kMaxBits + 1> count{};
    std::vector<int> symbol;
};

void ConstructDecodeTable(HuffTable &h, const uint8_t *lengths, int n) {
    h.count.fill(0);
    for (int i = 0; i < n; i++) h.count[static_cast<size_t>(lengths[i])]++;
    h.count[0] = 0;
    std::array<int, kMaxBits + 2> offs{};
    for (size_t len = 1; len <= kMaxBits; len++) offs[len + 1] = offs[len] + h.count[len];
    h.symbol.assign(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; i++) {
        if (lengths[i] != 0) h.symbol[static_cast<size_t>(offs[static_cast<size_t>(lengths[i])]++)] = i;
    }
}

// Encode-side: canonical code *values* for each symbol (RFC 1951
// 3.2.2's pseudocode directly).
void ConstructEncodeCodes(const uint8_t *lengths, int n, std::vector<uint16_t> &codes) {
    std::array<int, kMaxBits + 1> count{};
    for (int i = 0; i < n; i++) count[static_cast<size_t>(lengths[i])]++;
    count[0] = 0;
    std::array<uint32_t, kMaxBits + 1> next_code{};
    uint32_t code = 0;
    for (size_t bits = 1; bits <= kMaxBits; bits++) {
        code = (code + static_cast<uint32_t>(count[bits - 1])) << 1;
        next_code[bits] = code;
    }
    codes.assign(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; i++) {
        if (lengths[i] != 0) codes[static_cast<size_t>(i)] = static_cast<uint16_t>(next_code[static_cast<size_t>(lengths[i])]++);
    }
}

// -- Length/distance extra-bits tables (RFC 1951 section 3.2.5) -----------

constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,   17,   25,
                                     33,   49,   65,   97,   129,  193,  257,  385,  513,  769,
                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

constexpr int kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// -- Fixed Huffman tables (RFC 1951 section 3.2.6) -------------------------

void FixedLitLengths(uint8_t *lengths /*[288]*/) {
    int i = 0;
    for (; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
}
void FixedDistLengths(uint8_t *lengths /*[30]*/) {
    for (int i = 0; i < 30; i++) lengths[i] = 5;
}

// ===========================================================================
// Inflate (decode)
// ===========================================================================

class BitReader {
public:
    BitReader(const unsigned char *data, size_t len) : data_(data), len_(len) {}

    // -1 on exhausted input.
    int GetBit() {
        if (byte_pos_ >= len_) return -1;
        int bit = (data_[byte_pos_] >> bit_pos_) & 1;
        bit_pos_++;
        if (bit_pos_ == 8) {
            bit_pos_ = 0;
            byte_pos_++;
        }
        return bit;
    }

    // Raw multi-bit value, LSB-first (RFC 1951 3.1.1: "packed starting
    // with the least-significant bit"). -1 on exhausted input.
    long GetBits(int n) {
        long val = 0;
        for (int i = 0; i < n; i++) {
            int b = GetBit();
            if (b < 0) return -1;
            val |= static_cast<long>(b) << i;
        }
        return val;
    }

    void AlignToByte() {
        if (bit_pos_ != 0) {
            bit_pos_ = 0;
            byte_pos_++;
        }
    }

    bool ReadBytes(unsigned char *out, size_t n) {
        if (byte_pos_ + n > len_) return false;
        std::memcpy(out, data_ + byte_pos_, n);
        byte_pos_ += n;
        return true;
    }

    bool Exhausted() const { return byte_pos_ >= len_; }

private:
    const unsigned char *data_;
    size_t len_;
    size_t byte_pos_ = 0;
    int bit_pos_ = 0;
};

// Decodes one Huffman-coded symbol, MSB-of-code-first (matches how
// DeflateRaw's BitWriter emits Huffman codes -- see its own comment).
// Returns -1 on error/exhausted input.
int DecodeSymbol(BitReader &br, const HuffTable &h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; len++) {
        int bit = br.GetBit();
        if (bit < 0) return -1;
        code |= bit;
        int count = h.count[static_cast<size_t>(len)];
        if (code - first < count) return h.symbol[static_cast<size_t>(index + (code - first))];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

bool InflateBlockData(BitReader &br, const HuffTable &lit_table, const HuffTable &dist_table, std::string &out) {
    for (;;) {
        int sym = DecodeSymbol(br, lit_table);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back(static_cast<char>(sym));
        } else if (sym == 256) {
            return true;  // end of block
        } else {
            int idx = sym - 257;
            if (idx >= 29) return false;
            long extra = kLengthExtra[idx] > 0 ? br.GetBits(kLengthExtra[idx]) : 0;
            if (extra < 0) return false;
            long length = kLengthBase[idx] + extra;

            int dsym = DecodeSymbol(br, dist_table);
            if (dsym < 0 || dsym >= 30) return false;
            long dextra = kDistExtra[dsym] > 0 ? br.GetBits(kDistExtra[dsym]) : 0;
            if (dextra < 0) return false;
            long distance = kDistBase[dsym] + dextra;
            if (static_cast<size_t>(distance) > out.size()) return false;

            size_t start = out.size() - static_cast<size_t>(distance);
            for (long i = 0; i < length; i++) out.push_back(out[start + static_cast<size_t>(i)]);
        }
    }
}

bool InflateDynamicBlock(BitReader &br, std::string &out) {
    long hlit = br.GetBits(5);
    long hdist = br.GetBits(5);
    long hclen = br.GetBits(4);
    if (hlit < 0 || hdist < 0 || hclen < 0) return false;
    hlit += 257;
    hdist += 1;
    hclen += 4;

    uint8_t cl_lengths[19] = {0};
    for (long i = 0; i < hclen; i++) {
        long v = br.GetBits(3);
        if (v < 0) return false;
        cl_lengths[kCodeLengthOrder[i]] = static_cast<uint8_t>(v);
    }
    HuffTable cl_table;
    ConstructDecodeTable(cl_table, cl_lengths, 19);

    std::vector<uint8_t> lengths(static_cast<size_t>(hlit + hdist), 0);
    size_t i = 0;
    while (i < lengths.size()) {
        int sym = DecodeSymbol(br, cl_table);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[i++] = static_cast<uint8_t>(sym);
        } else if (sym == 16) {
            if (i == 0) return false;
            long rep = br.GetBits(2);
            if (rep < 0) return false;
            uint8_t prev = lengths[i - 1];
            for (long r = 0; r < rep + 3 && i < lengths.size(); r++) lengths[i++] = prev;
        } else if (sym == 17) {
            long rep = br.GetBits(3);
            if (rep < 0) return false;
            for (long r = 0; r < rep + 3 && i < lengths.size(); r++) lengths[i++] = 0;
        } else {  // 18
            long rep = br.GetBits(7);
            if (rep < 0) return false;
            for (long r = 0; r < rep + 11 && i < lengths.size(); r++) lengths[i++] = 0;
        }
    }

    HuffTable lit_table, dist_table;
    ConstructDecodeTable(lit_table, lengths.data(), static_cast<int>(hlit));
    ConstructDecodeTable(dist_table, lengths.data() + hlit, static_cast<int>(hdist));
    return InflateBlockData(br, lit_table, dist_table, out);
}

}  // namespace

bool InflateRaw(const unsigned char *data, size_t len, std::string &out) {
    BitReader br(data, len);
    out.clear();
    for (;;) {
        long bfinal = br.GetBit();
        if (bfinal < 0) return false;
        long btype = br.GetBits(2);
        if (btype < 0) return false;

        if (btype == 0) {  // stored
            br.AlignToByte();
            unsigned char hdr[4];
            if (!br.ReadBytes(hdr, 4)) return false;
            uint16_t block_len = static_cast<uint16_t>(hdr[0] | (hdr[1] << 8));
            uint16_t nlen = static_cast<uint16_t>(hdr[2] | (hdr[3] << 8));
            if (static_cast<uint16_t>(~block_len) != nlen) return false;
            size_t old_size = out.size();
            out.resize(old_size + block_len);
            if (block_len > 0 && !br.ReadBytes(reinterpret_cast<unsigned char *>(&out[old_size]), block_len)) return false;
        } else if (btype == 1) {  // fixed Huffman
            uint8_t lit_lengths[288], dist_lengths[30];
            FixedLitLengths(lit_lengths);
            FixedDistLengths(dist_lengths);
            HuffTable lit_table, dist_table;
            ConstructDecodeTable(lit_table, lit_lengths, 288);
            ConstructDecodeTable(dist_table, dist_lengths, 30);
            if (!InflateBlockData(br, lit_table, dist_table, out)) return false;
        } else if (btype == 2) {  // dynamic Huffman
            if (!InflateDynamicBlock(br, out)) return false;
        } else {
            return false;  // btype == 3 is reserved/invalid
        }

        if (bfinal) break;
    }
    return true;
}

// ===========================================================================
// Deflate (encode) -- fixed Huffman blocks only, see this file's own top
// comment for why.
// ===========================================================================

namespace {

class BitWriter {
public:
    void PutBit(int bit) {
        bitbuf_ |= static_cast<unsigned>(bit & 1) << bitcount_;
        bitcount_++;
        if (bitcount_ == 8) Flush8();
    }
    // Raw multi-bit value, LSB-first.
    void PutBits(uint32_t val, int n) {
        for (int i = 0; i < n; i++) PutBit(static_cast<int>((val >> i) & 1));
    }
    // Huffman code, MSB-of-code-first (RFC 1951 3.1.1: "Huffman codes
    // are packed starting with the most-significant bit of the code").
    void PutHuffman(uint16_t code, int len) {
        for (int i = len - 1; i >= 0; i--) PutBit((code >> i) & 1);
    }
    void FinishByte() {
        if (bitcount_ > 0) Flush8();
    }
    std::string Take() { return std::move(out_); }

private:
    void Flush8() {
        out_.push_back(static_cast<char>(bitbuf_));
        bitbuf_ = 0;
        bitcount_ = 0;
    }
    std::string out_;
    unsigned bitbuf_ = 0;
    int bitcount_ = 0;
};

struct Token {
    bool is_match;
    unsigned char literal;
    int length;    // match only
    int distance;  // match only
};

constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kWindowSize = 32768;
constexpr int kHashBits = 15;
constexpr int kHashSize = 1 << kHashBits;
constexpr int kMaxChain = 64;  // bounded match-search depth -- see this file's own top comment on why speed isn't critical here

uint32_t Hash3(const unsigned char *p) {
    uint32_t h = (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
    h = (h * 2654435761u) >> (32 - kHashBits);
    return h & (kHashSize - 1);
}

// Hash-chain LZ77 match finder (classic zlib-style structure: a head[]
// table of the most recent position per 3-byte hash, plus a prev[] chain
// linking earlier positions with the same hash) -- greedy (always takes
// the best match found within kMaxChain candidates, no lazy-matching
// one-byte-ahead lookahead), which is exactly the simplification
// MINIZ_REMOVAL_PLAN.md's Scoping decision 3 calls sufficient.
std::vector<Token> FindMatches(const unsigned char *data, size_t n) {
    std::vector<Token> tokens;
    std::vector<int> head(kHashSize, -1);
    std::vector<int> prev(n, -1);

    size_t i = 0;
    while (i < n) {
        int best_len = 0;
        int best_dist = 0;
        if (i + kMinMatch <= n) {
            uint32_t h = Hash3(data + i);
            int cand = head[h];
            int tries = kMaxChain;
            while (cand >= 0 && tries-- > 0) {
                if (static_cast<size_t>(i) - static_cast<size_t>(cand) > kWindowSize) break;
                size_t max_len = std::min<size_t>(kMaxMatch, n - i);
                size_t len = 0;
                while (len < max_len && data[static_cast<size_t>(cand) + len] == data[i + len]) len++;
                if (static_cast<int>(len) > best_len) {
                    best_len = static_cast<int>(len);
                    best_dist = static_cast<int>(i) - cand;
                    if (best_len >= kMaxMatch) break;
                }
                cand = prev[static_cast<size_t>(cand)];
            }
        }
        if (best_len >= kMinMatch) {
            tokens.push_back(Token{true, 0, best_len, best_dist});
            size_t end = i + static_cast<size_t>(best_len);
            for (; i < end && i + kMinMatch <= n; i++) {
                uint32_t h = Hash3(data + i);
                prev[i] = head[h];
                head[h] = static_cast<int>(i);
            }
            i = end;
        } else {
            tokens.push_back(Token{false, data[i], 0, 0});
            if (i + kMinMatch <= n) {
                uint32_t h = Hash3(data + i);
                prev[i] = head[h];
                head[h] = static_cast<int>(i);
            }
            i++;
        }
    }
    return tokens;
}

int LengthSymbol(int length, int *extra_bits, int *extra_val) {
    for (int i = 28; i >= 0; i--) {
        if (length >= kLengthBase[i]) {
            *extra_bits = kLengthExtra[i];
            *extra_val = length - kLengthBase[i];
            return 257 + i;
        }
    }
    return -1;
}

int DistSymbol(int distance, int *extra_bits, int *extra_val) {
    for (int i = 29; i >= 0; i--) {
        if (distance >= kDistBase[i]) {
            *extra_bits = kDistExtra[i];
            *extra_val = distance - kDistBase[i];
            return i;
        }
    }
    return -1;
}

}  // namespace

std::string DeflateRaw(const unsigned char *data, size_t len) {
    uint8_t lit_lengths[288], dist_lengths[30];
    FixedLitLengths(lit_lengths);
    FixedDistLengths(dist_lengths);
    std::vector<uint16_t> lit_codes, dist_codes;
    ConstructEncodeCodes(lit_lengths, 288, lit_codes);
    ConstructEncodeCodes(dist_lengths, 30, dist_codes);

    BitWriter bw;
    bw.PutBits(1, 1);  // BFINAL: a single block covers the whole input -- simplest valid encoding, no size limit that matters for mep's own document-sized payloads
    bw.PutBits(1, 2);  // BTYPE: fixed Huffman

    std::vector<Token> tokens = len > 0 ? FindMatches(data, len) : std::vector<Token>();
    for (const Token &t : tokens) {
        if (!t.is_match) {
            bw.PutHuffman(lit_codes[t.literal], lit_lengths[t.literal]);
        } else {
            int extra_bits = 0, extra_val = 0;
            int lsym = LengthSymbol(t.length, &extra_bits, &extra_val);
            bw.PutHuffman(lit_codes[static_cast<size_t>(lsym)], lit_lengths[lsym]);
            if (extra_bits > 0) bw.PutBits(static_cast<uint32_t>(extra_val), extra_bits);

            int dextra_bits = 0, dextra_val = 0;
            int dsym = DistSymbol(t.distance, &dextra_bits, &dextra_val);
            bw.PutHuffman(dist_codes[static_cast<size_t>(dsym)], dist_lengths[dsym]);
            if (dextra_bits > 0) bw.PutBits(static_cast<uint32_t>(dextra_val), dextra_bits);
        }
    }
    bw.PutHuffman(lit_codes[256], lit_lengths[256]);  // end-of-block
    bw.FinishByte();
    return bw.Take();
}

// ===========================================================================
// zlib wrapper (RFC 1950)
// ===========================================================================

bool InflateZlib(const unsigned char *data, size_t len, std::string &out) {
    if (len < 6) return false;
    uint8_t cmf = data[0];
    if ((cmf & 0x0F) != 8) return false;  // compression method must be 8 (deflate)
    // FDICT (preset dictionary) isn't produced by any writer mep needs
    // to read (PNG/M3D neither use one) -- reject rather than silently
    // mis-decode if it's ever set.
    if (data[1] & 0x20) return false;
    if (!InflateRaw(data + 2, len - 6, out)) return false;
    uint32_t stored_adler =
        (static_cast<uint32_t>(data[len - 4]) << 24) | (static_cast<uint32_t>(data[len - 3]) << 16) |
        (static_cast<uint32_t>(data[len - 2]) << 8) | static_cast<uint32_t>(data[len - 1]);
    uint32_t actual = Adler32(1, reinterpret_cast<const unsigned char *>(out.data()), out.size());
    return actual == stored_adler;
}

std::string DeflateZlib(const unsigned char *data, size_t len) {
    std::string out;
    out.push_back(static_cast<char>(0x78));  // CMF: deflate, 32K window
    out.push_back(static_cast<char>(0x9C));  // FLG: default compression level, no preset dictionary, valid check bits for this CMF/FLG pair
    out += DeflateRaw(data, len);
    uint32_t adler = Adler32(1, data, len);
    out.push_back(static_cast<char>((adler >> 24) & 0xFF));
    out.push_back(static_cast<char>((adler >> 16) & 0xFF));
    out.push_back(static_cast<char>((adler >> 8) & 0xFF));
    out.push_back(static_cast<char>(adler & 0xFF));
    return out;
}

}  // namespace deflate
