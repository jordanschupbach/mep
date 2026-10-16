// In-house Zstandard decoder (RFC 8878). See zstd_decode.h.
//
// Structure follows the RFC section by section: frames -> blocks ->
// (literals section, sequences section) -> sequence execution. Bitstreams
// are read through two small readers: ForwardBits for FSE table
// descriptions (little-endian, LSB first) and BackwardBits for the
// Huffman and FSE payload streams (read from the end, starting just
// below the final byte's highest set bit). BackwardBits tracks a signed
// bit position so "read past the beginning" (which the format uses to
// terminate the Huffman-weights stream) is just pos < 0.

#include "zstd_decode.h"

#include <array>
#include <bit>
#include <cstring>
#include <vector>

namespace zstd {

namespace {

constexpr uint32_t kFrameMagic = 0xFD2FB528u;
constexpr uint32_t kSkippableMagicMask = 0xFFFFFFF0u;
constexpr uint32_t kSkippableMagic = 0x184D2A50u;
constexpr size_t kMaxBlockSize = 128 * 1024;

uint32_t Read32(const unsigned char *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t Read64(const unsigned char *p) {
    return static_cast<uint64_t>(Read32(p)) | (static_cast<uint64_t>(Read32(p + 4)) << 32);
}

int HighBit(uint32_t v) { return static_cast<int>(std::bit_width(v)) - 1; }

// Little-endian bit reader used for FSE table descriptions. Reads past the
// end return zero bits; callers check `bitpos` against the size afterward.
struct ForwardBits {
    const unsigned char *p = nullptr;
    size_t len = 0;
    size_t bitpos = 0;

    uint32_t Peek(int n) const {
        uint64_t v = 0;
        size_t byte = bitpos >> 3;
        for (size_t i = 0; i < 5 && byte + i < len; i++) v |= static_cast<uint64_t>(p[byte + i]) << (8 * i);
        v >>= (bitpos & 7);
        return static_cast<uint32_t>(v & ((uint64_t{1} << n) - 1));
    }
    void Skip(int n) { bitpos += static_cast<size_t>(n); }
    uint32_t Read(int n) {
        uint32_t v = Peek(n);
        Skip(n);
        return v;
    }
};

// Reverse bitstream (RFC 8878 4.1): the last byte's highest set bit is a
// marker; data bits precede it and are consumed from the end toward the
// start. A read of n bits returns bits [pos-n, pos) with bit pos-1 most
// significant; bits before the stream start read as zero.
struct BackwardBits {
    const unsigned char *p = nullptr;
    size_t len = 0;
    int64_t pos = 0;

    bool Init(const unsigned char *data, size_t n) {
        p = data;
        len = n;
        if (n == 0) return false;
        unsigned char last = data[n - 1];
        if (last == 0) return false;
        pos = static_cast<int64_t>((n - 1) * 8) + HighBit(last);
        return true;
    }

    // Bits [start, start+n), n <= 56, zero outside the buffer.
    uint64_t Extract(int64_t start, int n) const {
        if (n <= 0) return 0;
        if (start < 0) {
            if (start + n <= 0) return 0;
            int shift = static_cast<int>(-start);
            return Extract(0, n - shift) << shift;
        }
        size_t byte = static_cast<size_t>(start >> 3);
        uint64_t v;
        if (byte + 8 <= len) {
            v = Read64(p + byte);
        } else {
            v = 0;
            for (size_t i = 0; i < 8 && byte + i < len; i++) v |= static_cast<uint64_t>(p[byte + i]) << (8 * i);
        }
        v >>= (start & 7);
        return v & ((uint64_t{1} << n) - 1);
    }
    uint64_t Read(int n) {
        if (n == 0) return 0;
        pos -= n;
        return Extract(pos, n);
    }
    uint64_t Peek(int n) const { return Extract(pos - n, n); }
    void Skip(int n) { pos -= n; }
};

struct FseEntry {
    uint16_t symbol = 0;
    uint8_t nb_bits = 0;
    uint16_t baseline = 0;
};

struct FseTable {
    std::vector<FseEntry> entries;
    int accuracy = 0;
    bool valid = false;
};

bool BuildFseTable(const std::vector<int16_t> &norm, int accuracy, FseTable *out) {
    size_t size = size_t{1} << accuracy;
    out->entries.assign(size, FseEntry{});
    out->accuracy = accuracy;
    out->valid = false;
    std::vector<uint32_t> next(norm.size(), 0);
    size_t high = size - 1;
    for (size_t s = 0; s < norm.size(); s++) {
        if (norm[s] == -1) {
            out->entries[high].symbol = static_cast<uint16_t>(s);
            if (high == 0) return false;
            high--;
            next[s] = 1;
        } else {
            next[s] = static_cast<uint32_t>(norm[s] > 0 ? norm[s] : 0);
        }
    }
    size_t step = (size >> 1) + (size >> 3) + 3;
    size_t mask = size - 1;
    size_t pos = 0;
    for (size_t s = 0; s < norm.size(); s++) {
        for (int i = 0; i < norm[s]; i++) {
            out->entries[pos].symbol = static_cast<uint16_t>(s);
            do {
                pos = (pos + step) & mask;
            } while (pos > high);
        }
    }
    if (pos != 0) return false;
    for (size_t u = 0; u < size; u++) {
        uint16_t s = out->entries[u].symbol;
        uint32_t ns = next[s]++;
        if (ns == 0) return false;
        int nb = accuracy - HighBit(ns);
        if (nb < 0) return false;
        out->entries[u].nb_bits = static_cast<uint8_t>(nb);
        out->entries[u].baseline = static_cast<uint16_t>((ns << nb) - size);
    }
    out->valid = true;
    return true;
}

void BuildRleTable(uint16_t symbol, FseTable *out) {
    out->entries.assign(1, FseEntry{symbol, 0, 0});
    out->accuracy = 0;
    out->valid = true;
}

// RFC 8878 4.1.1: FSE table description. `max_symbol` is the largest
// symbol value allowed.
bool ReadFseDescription(const unsigned char *p, size_t len, int max_symbol, int max_accuracy, FseTable *out,
                        size_t *consumed) {
    if (len == 0) return false;
    ForwardBits br{p, len, 0};
    int accuracy = static_cast<int>(br.Read(4)) + 5;
    if (accuracy > max_accuracy) return false;
    int32_t remaining = (int32_t{1} << accuracy) + 1;
    int32_t threshold = int32_t{1} << accuracy;
    int nb_bits = accuracy + 1;
    std::vector<int16_t> norm(static_cast<size_t>(max_symbol) + 1, 0);
    int s = 0;
    while (remaining > 1 && s <= max_symbol) {
        int32_t max = (2 * threshold - 1) - remaining;
        int32_t count;
        int32_t low = static_cast<int32_t>(br.Peek(nb_bits - 1));
        if (low < max) {
            count = low;
            br.Skip(nb_bits - 1);
        } else {
            count = static_cast<int32_t>(br.Peek(nb_bits));
            if (count >= threshold) count -= max;
            br.Skip(nb_bits);
        }
        count -= 1;
        remaining -= count < 0 ? -count : count;
        norm[static_cast<size_t>(s)] = static_cast<int16_t>(count);
        s++;
        if (count == 0) {
            for (;;) {
                uint32_t r = br.Read(2);
                s += static_cast<int>(r);
                if (s > max_symbol + 1) return false;
                if (r != 3) break;
            }
        }
        if (remaining < 1) return false;
        while (remaining < threshold) {
            nb_bits--;
            threshold >>= 1;
        }
        if (br.bitpos > len * 8) return false;
    }
    if (remaining != 1) return false;
    norm.resize(static_cast<size_t>(s));
    *consumed = (br.bitpos + 7) / 8;
    if (*consumed > len) return false;
    return BuildFseTable(norm, accuracy, out);
}

struct HufTable {
    std::vector<uint16_t> entries;  // symbol | (nb_bits << 8)
    int max_bits = 0;
    bool valid = false;
};

bool BuildHufTable(const std::vector<uint8_t> &weights, HufTable *out) {
    uint32_t total = 0;
    for (uint8_t w : weights) {
        if (w > 11) return false;
        if (w > 0) total += uint32_t{1} << (w - 1);
    }
    if (total == 0) return false;
    int max_bits = HighBit(total) + 1;
    uint32_t rest = (uint32_t{1} << max_bits) - total;
    if (rest == 0 || (rest & (rest - 1)) != 0) return false;
    std::vector<uint8_t> w = weights;
    w.push_back(static_cast<uint8_t>(HighBit(rest) + 1));
    if (max_bits > 11 || w.size() > 256) return false;
    out->max_bits = max_bits;
    out->entries.assign(size_t{1} << max_bits, 0);
    size_t pos = 0;
    for (int weight = 1; weight <= max_bits; weight++) {
        size_t span = size_t{1} << (weight - 1);
        uint16_t nb = static_cast<uint16_t>(max_bits + 1 - weight);
        for (size_t s = 0; s < w.size(); s++) {
            if (w[s] != weight) continue;
            if (pos + span > out->entries.size()) return false;
            for (size_t i = 0; i < span; i++) out->entries[pos + i] = static_cast<uint16_t>(s | (static_cast<size_t>(nb) << 8));
            pos += span;
        }
    }
    if (pos != out->entries.size()) return false;
    out->valid = true;
    return true;
}

// RFC 8878 4.2.1: Huffman tree description.
bool ReadHuffmanTree(const unsigned char *p, size_t len, HufTable *out, size_t *consumed) {
    if (len == 0) return false;
    unsigned header = p[0];
    std::vector<uint8_t> weights;
    if (header >= 128) {
        size_t n = header - 127;
        size_t bytes = (n + 1) / 2;
        if (1 + bytes > len) return false;
        for (size_t i = 0; i < n; i++) {
            unsigned char b = p[1 + i / 2];
            weights.push_back(static_cast<uint8_t>((i % 2 == 0) ? (b >> 4) : (b & 15)));
        }
        *consumed = 1 + bytes;
    } else {
        size_t csize = header;
        if (csize == 0 || 1 + csize > len) return false;
        const unsigned char *q = p + 1;
        FseTable ft;
        size_t desc = 0;
        if (!ReadFseDescription(q, csize, 255, 6, &ft, &desc)) return false;
        BackwardBits br;
        if (desc >= csize || !br.Init(q + desc, csize - desc)) return false;
        uint32_t s1 = static_cast<uint32_t>(br.Read(ft.accuracy));
        uint32_t s2 = static_cast<uint32_t>(br.Read(ft.accuracy));
        for (;;) {
            if (weights.size() >= 255) return false;
            const FseEntry &e1 = ft.entries[s1];
            weights.push_back(static_cast<uint8_t>(e1.symbol));
            s1 = e1.baseline + static_cast<uint32_t>(br.Read(e1.nb_bits));
            if (br.pos < 0) {
                weights.push_back(static_cast<uint8_t>(ft.entries[s2].symbol));
                break;
            }
            if (weights.size() >= 255) return false;
            const FseEntry &e2 = ft.entries[s2];
            weights.push_back(static_cast<uint8_t>(e2.symbol));
            s2 = e2.baseline + static_cast<uint32_t>(br.Read(e2.nb_bits));
            if (br.pos < 0) {
                weights.push_back(static_cast<uint8_t>(ft.entries[s1].symbol));
                break;
            }
        }
        *consumed = 1 + csize;
    }
    return BuildHufTable(weights, out);
}

bool DecodeHufStream(const HufTable &t, const unsigned char *p, size_t len, unsigned char *out, size_t count) {
    BackwardBits br;
    if (!br.Init(p, len)) return false;
    int mb = t.max_bits;
    for (size_t i = 0; i < count; i++) {
        uint16_t e = t.entries[static_cast<size_t>(br.Peek(mb))];
        out[i] = static_cast<unsigned char>(e & 0xFF);
        br.Skip(e >> 8);
    }
    return br.pos == 0;
}

// Sequence code tables (RFC 8878 3.1.1.3.2.1).
constexpr std::array<uint32_t, 36> kLLBase = {0,  1,  2,  3,  4,  5,   6,   7,   8,   9,    10,   11,
                                              12, 13, 14, 15, 16, 18,  20,  22,  24,  28,   32,   40,
                                              48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
constexpr std::array<uint8_t, 36> kLLBits = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  1,  1,
                                             1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr std::array<uint32_t, 53> kMLBase = {
    3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,  17,  18,  19,  20,   21,   22,   23,   24,    25,    26,   27,
    28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
constexpr std::array<uint8_t, 53> kMLBits = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0, 0,
                                             0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

constexpr std::array<int16_t, 36> kLLDefault = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2,  2,  2,
                                                2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
constexpr std::array<int16_t, 53> kMLDefault = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,  1,  1,  1,
                                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,
                                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
constexpr std::array<int16_t, 29> kOFDefault = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1,
                                                1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

struct FrameState {
    HufTable huf;
    FseTable ll, of, ml;
    uint64_t rep[3] = {1, 4, 8};
    size_t frame_start = 0;
};

struct Decoder {
    std::string &out;
    std::string *error;
    size_t max_output;
    std::vector<unsigned char> literals;

    bool Fail(const char *msg) {
        if (error) *error = std::string("zstd: ") + msg;
        return false;
    }

    bool CheckGrowth(size_t extra) {
        if (max_output != 0 && out.size() + extra > max_output) return Fail("output exceeds size limit");
        return true;
    }

    bool DecodeLiterals(const unsigned char *p, size_t len, FrameState &fs, size_t *consumed) {
        if (len == 0) return Fail("truncated literals header");
        unsigned b0 = p[0];
        unsigned type = b0 & 3;
        unsigned sf = (b0 >> 2) & 3;
        if (type == 0 || type == 1) {
            size_t regen, hdr;
            if (sf == 0 || sf == 2) {
                regen = b0 >> 3;
                hdr = 1;
            } else if (sf == 1) {
                if (len < 2) return Fail("truncated literals header");
                regen = (b0 >> 4) | (static_cast<size_t>(p[1]) << 4);
                hdr = 2;
            } else {
                if (len < 3) return Fail("truncated literals header");
                regen = (b0 >> 4) | (static_cast<size_t>(p[1]) << 4) | (static_cast<size_t>(p[2]) << 12);
                hdr = 3;
            }
            if (regen > kMaxBlockSize) return Fail("literals too large");
            if (type == 0) {
                if (hdr + regen > len) return Fail("truncated raw literals");
                literals.assign(p + hdr, p + hdr + regen);
                *consumed = hdr + regen;
            } else {
                if (hdr + 1 > len) return Fail("truncated RLE literals");
                literals.assign(regen, p[hdr]);
                *consumed = hdr + 1;
            }
            return true;
        }
        size_t regen, csize, hdr;
        int streams;
        if (sf <= 1) {
            if (len < 3) return Fail("truncated literals header");
            uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
            regen = (v >> 4) & 0x3FF;
            csize = (v >> 14) & 0x3FF;
            hdr = 3;
            streams = sf == 0 ? 1 : 4;
        } else if (sf == 2) {
            if (len < 4) return Fail("truncated literals header");
            uint32_t v = Read32(p);
            regen = (v >> 4) & 0x3FFF;
            csize = (v >> 18) & 0x3FFF;
            hdr = 4;
            streams = 4;
        } else {
            if (len < 5) return Fail("truncated literals header");
            uint64_t v = static_cast<uint64_t>(Read32(p)) | (static_cast<uint64_t>(p[4]) << 32);
            regen = static_cast<size_t>((v >> 4) & 0x3FFFF);
            csize = static_cast<size_t>((v >> 22) & 0x3FFFF);
            hdr = 5;
            streams = 4;
        }
        if (regen > kMaxBlockSize) return Fail("literals too large");
        if (hdr + csize > len) return Fail("truncated compressed literals");
        const unsigned char *q = p + hdr;
        size_t qlen = csize;
        if (type == 2) {
            size_t tree = 0;
            if (!ReadHuffmanTree(q, qlen, &fs.huf, &tree)) return Fail("bad Huffman tree");
            q += tree;
            qlen -= tree;
        } else if (!fs.huf.valid) {
            return Fail("treeless literals without a previous Huffman table");
        }
        literals.resize(regen);
        if (streams == 1) {
            if (!DecodeHufStream(fs.huf, q, qlen, literals.data(), regen)) return Fail("bad Huffman stream");
        } else {
            if (qlen < 6) return Fail("truncated jump table");
            size_t s1 = static_cast<size_t>(q[0]) | (static_cast<size_t>(q[1]) << 8);
            size_t s2 = static_cast<size_t>(q[2]) | (static_cast<size_t>(q[3]) << 8);
            size_t s3 = static_cast<size_t>(q[4]) | (static_cast<size_t>(q[5]) << 8);
            if (6 + s1 + s2 + s3 > qlen) return Fail("bad jump table");
            size_t s4 = qlen - 6 - s1 - s2 - s3;
            size_t seg = (regen + 3) / 4;
            if (seg * 3 > regen) return Fail("bad 4-stream literal size");
            const unsigned char *d = q + 6;
            unsigned char *o = literals.data();
            if (!DecodeHufStream(fs.huf, d, s1, o, seg) || !DecodeHufStream(fs.huf, d + s1, s2, o + seg, seg) ||
                !DecodeHufStream(fs.huf, d + s1 + s2, s3, o + 2 * seg, seg) ||
                !DecodeHufStream(fs.huf, d + s1 + s2 + s3, s4, o + 3 * seg, regen - 3 * seg)) {
                return Fail("bad Huffman stream");
            }
        }
        *consumed = hdr + csize;
        return true;
    }

    bool ReadTable(unsigned mode, const unsigned char *p, size_t len, size_t *pos, int max_symbol, int max_accuracy,
                   const int16_t *def, size_t def_count, int def_accuracy, FseTable *table) {
        switch (mode) {
            case 0: {
                std::vector<int16_t> norm(def, def + def_count);
                if (!BuildFseTable(norm, def_accuracy, table)) return Fail("bad predefined table");
                return true;
            }
            case 1:
                if (*pos >= len) return Fail("truncated RLE table");
                if (p[*pos] > max_symbol) return Fail("bad RLE symbol");
                BuildRleTable(p[*pos], table);
                (*pos)++;
                return true;
            case 2: {
                size_t used = 0;
                if (!ReadFseDescription(p + *pos, len - *pos, max_symbol, max_accuracy, table, &used))
                    return Fail("bad FSE table");
                *pos += used;
                return true;
            }
            default:
                if (!table->valid) return Fail("repeat table mode without a previous table");
                return true;
        }
    }

    bool DecodeBlock(const unsigned char *p, size_t len, FrameState &fs) {
        size_t lit_used = 0;
        if (!DecodeLiterals(p, len, fs, &lit_used)) return false;
        size_t pos = lit_used;
        if (pos >= len) return Fail("truncated sequences header");
        size_t nb_seq = p[pos];
        if (nb_seq < 128) {
            pos += 1;
        } else if (nb_seq < 255) {
            if (pos + 2 > len) return Fail("truncated sequences header");
            nb_seq = ((nb_seq - 128) << 8) + p[pos + 1];
            pos += 2;
        } else {
            if (pos + 3 > len) return Fail("truncated sequences header");
            nb_seq = static_cast<size_t>(p[pos + 1]) + (static_cast<size_t>(p[pos + 2]) << 8) + 0x7F00;
            pos += 3;
        }
        size_t lit_pos = 0;
        if (nb_seq > 0) {
            if (pos >= len) return Fail("truncated sequences header");
            unsigned modes = p[pos++];
            if (modes & 3) return Fail("reserved sequence mode bits set");
            if (!ReadTable((modes >> 6) & 3, p, len, &pos, 35, 9, kLLDefault.data(), kLLDefault.size(), 6, &fs.ll)) return false;
            if (!ReadTable((modes >> 4) & 3, p, len, &pos, 31, 8, kOFDefault.data(), kOFDefault.size(), 5, &fs.of)) return false;
            if (!ReadTable((modes >> 2) & 3, p, len, &pos, 52, 9, kMLDefault.data(), kMLDefault.size(), 6, &fs.ml)) return false;
            BackwardBits br;
            if (pos >= len || !br.Init(p + pos, len - pos)) return Fail("bad sequence bitstream");
            uint32_t ll_state = static_cast<uint32_t>(br.Read(fs.ll.accuracy));
            uint32_t of_state = static_cast<uint32_t>(br.Read(fs.of.accuracy));
            uint32_t ml_state = static_cast<uint32_t>(br.Read(fs.ml.accuracy));
            for (size_t i = 0; i < nb_seq; i++) {
                const FseEntry &lle = fs.ll.entries[ll_state];
                const FseEntry &ofe = fs.of.entries[of_state];
                const FseEntry &mle = fs.ml.entries[ml_state];
                uint32_t of_code = ofe.symbol;
                if (of_code > 31) return Fail("bad offset code");
                if (mle.symbol >= kMLBase.size() || lle.symbol >= kLLBase.size()) return Fail("bad length code");
                uint64_t of_value = (uint64_t{1} << of_code) + br.Read(static_cast<int>(of_code));
                size_t ml = kMLBase[mle.symbol] + static_cast<size_t>(br.Read(kMLBits[mle.symbol]));
                size_t ll = kLLBase[lle.symbol] + static_cast<size_t>(br.Read(kLLBits[lle.symbol]));
                uint64_t offset;
                if (of_value > 3) {
                    offset = of_value - 3;
                    fs.rep[2] = fs.rep[1];
                    fs.rep[1] = fs.rep[0];
                    fs.rep[0] = offset;
                } else {
                    uint64_t idx = of_value - 1 + (ll == 0 ? 1 : 0);
                    if (idx == 0) {
                        offset = fs.rep[0];
                    } else {
                        offset = idx == 3 ? fs.rep[0] - 1 : fs.rep[idx];
                        if (idx > 1) fs.rep[2] = fs.rep[1];
                        fs.rep[1] = fs.rep[0];
                        fs.rep[0] = offset;
                    }
                }
                if (i + 1 < nb_seq) {
                    ll_state = lle.baseline + static_cast<uint32_t>(br.Read(lle.nb_bits));
                    ml_state = mle.baseline + static_cast<uint32_t>(br.Read(mle.nb_bits));
                    of_state = ofe.baseline + static_cast<uint32_t>(br.Read(ofe.nb_bits));
                }
                // Execute.
                if (lit_pos + ll > literals.size()) return Fail("sequence literal length out of range");
                if (!CheckGrowth(ll + ml)) return false;
                out.append(reinterpret_cast<const char *>(literals.data() + lit_pos), ll);
                lit_pos += ll;
                size_t produced = out.size() - fs.frame_start;
                if (offset == 0 || offset > produced) return Fail("match offset out of range");
                size_t off = static_cast<size_t>(offset);
                size_t base = out.size();
                out.resize(base + ml);
                char *d = &out[0];
                if (off >= ml) {
                    std::memcpy(d + base, d + base - off, ml);
                } else {
                    for (size_t k = 0; k < ml; k++) d[base + k] = d[base + k - off];
                }
            }
            if (br.pos != 0) return Fail("sequence bitstream not fully consumed");
        } else if (pos != len) {
            return Fail("trailing bytes after empty sequences section");
        }
        size_t rest = literals.size() - lit_pos;
        if (!CheckGrowth(rest)) return false;
        out.append(reinterpret_cast<const char *>(literals.data() + lit_pos), rest);
        return true;
    }

    bool DecodeFrame(const unsigned char *p, size_t len, size_t *consumed) {
        if (len < 6) return Fail("truncated frame header");
        unsigned fhd = p[4];
        unsigned fcs_flag = fhd >> 6;
        bool single = (fhd >> 5) & 1;
        bool has_checksum = (fhd >> 2) & 1;
        unsigned did_flag = fhd & 3;
        if (fhd & 8) return Fail("reserved frame header bit set");
        size_t pos = 5;
        if (!single) pos++;  // Window_Descriptor; we keep the whole output, so the window never limits us
        static constexpr size_t kDidSize[4] = {0, 1, 2, 4};
        size_t did_size = kDidSize[did_flag];
        if (pos + did_size > len) return Fail("truncated frame header");
        uint32_t dict_id = 0;
        for (size_t i = 0; i < did_size; i++) dict_id |= static_cast<uint32_t>(p[pos + i]) << (8 * i);
        pos += did_size;
        if (dict_id != 0) return Fail("frames that need a dictionary are not supported");
        size_t fcs_size = fcs_flag == 0 ? (single ? 1 : 0) : (size_t{1} << fcs_flag);
        if (pos + fcs_size > len) return Fail("truncated frame header");
        bool has_fcs = fcs_size > 0;
        uint64_t fcs = 0;
        for (size_t i = 0; i < fcs_size; i++) fcs |= static_cast<uint64_t>(p[pos + i]) << (8 * i);
        if (fcs_size == 2) fcs += 256;
        pos += fcs_size;

        FrameState fs;
        fs.frame_start = out.size();
        if (has_fcs && (max_output == 0 || fcs <= max_output) && fcs < (uint64_t{1} << 32)) {
            out.reserve(out.size() + static_cast<size_t>(fcs));
        }
        for (;;) {
            if (pos + 3 > len) return Fail("truncated block header");
            uint32_t bh = static_cast<uint32_t>(p[pos]) | (static_cast<uint32_t>(p[pos + 1]) << 8) |
                          (static_cast<uint32_t>(p[pos + 2]) << 16);
            pos += 3;
            bool last = bh & 1;
            unsigned type = (bh >> 1) & 3;
            size_t size = bh >> 3;
            if (type == 0) {
                if (pos + size > len) return Fail("truncated raw block");
                if (!CheckGrowth(size)) return false;
                out.append(reinterpret_cast<const char *>(p + pos), size);
                pos += size;
            } else if (type == 1) {
                if (pos + 1 > len) return Fail("truncated RLE block");
                if (size > kMaxBlockSize) return Fail("RLE block too large");
                if (!CheckGrowth(size)) return false;
                out.append(size, static_cast<char>(p[pos]));
                pos += 1;
            } else if (type == 2) {
                if (pos + size > len) return Fail("truncated compressed block");
                if (size > kMaxBlockSize) return Fail("compressed block too large");
                if (!DecodeBlock(p + pos, size, fs)) return false;
                pos += size;
            } else {
                return Fail("reserved block type");
            }
            if (last) break;
        }
        size_t produced = out.size() - fs.frame_start;
        if (has_fcs && produced != fcs) return Fail("frame content size mismatch");
        if (has_checksum) {
            if (pos + 4 > len) return Fail("truncated checksum");
            uint32_t want = Read32(p + pos);
            uint32_t got = static_cast<uint32_t>(
                Xxh64(reinterpret_cast<const unsigned char *>(out.data()) + fs.frame_start, produced, 0));
            if (want != got) return Fail("content checksum mismatch");
            pos += 4;
        }
        *consumed = pos;
        return true;
    }
};

uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

constexpr uint64_t kP1 = 11400714785074694791ull;
constexpr uint64_t kP2 = 14029467366897019727ull;
constexpr uint64_t kP3 = 1609587929392839161ull;
constexpr uint64_t kP4 = 9650029242287828579ull;
constexpr uint64_t kP5 = 2870177450012600261ull;

uint64_t XxRound(uint64_t acc, uint64_t input) {
    acc += input * kP2;
    acc = Rotl(acc, 31);
    return acc * kP1;
}

uint64_t XxMerge(uint64_t acc, uint64_t val) {
    acc ^= XxRound(0, val);
    return acc * kP1 + kP4;
}

}  // namespace

uint64_t Xxh64(const unsigned char *data, size_t len, uint64_t seed) {
    const unsigned char *p = data;
    const unsigned char *end = data + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
        while (static_cast<size_t>(end - p) >= 32) {
            v1 = XxRound(v1, Read64(p));
            v2 = XxRound(v2, Read64(p + 8));
            v3 = XxRound(v3, Read64(p + 16));
            v4 = XxRound(v4, Read64(p + 24));
            p += 32;
        }
        h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
        h = XxMerge(h, v1);
        h = XxMerge(h, v2);
        h = XxMerge(h, v3);
        h = XxMerge(h, v4);
    } else {
        h = seed + kP5;
    }
    h += static_cast<uint64_t>(len);
    while (static_cast<size_t>(end - p) >= 8) {
        h ^= XxRound(0, Read64(p));
        h = Rotl(h, 27) * kP1 + kP4;
        p += 8;
    }
    if (static_cast<size_t>(end - p) >= 4) {
        h ^= static_cast<uint64_t>(Read32(p)) * kP1;
        h = Rotl(h, 23) * kP2 + kP3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<uint64_t>(*p) * kP5;
        h = Rotl(h, 11) * kP1;
        p++;
    }
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

bool LooksLikeZstd(const unsigned char *data, size_t len) {
    if (len < 4) return false;
    uint32_t m = Read32(data);
    return m == kFrameMagic || (m & kSkippableMagicMask) == kSkippableMagic;
}

bool Decompress(const unsigned char *data, size_t len, std::string &out, std::string *error, size_t max_output) {
    Decoder dec{out, error, max_output, {}};
    size_t pos = 0;
    if (len == 0) return dec.Fail("empty input");
    while (pos < len) {
        if (len - pos < 4) return dec.Fail("trailing garbage");
        uint32_t magic = Read32(data + pos);
        if ((magic & kSkippableMagicMask) == kSkippableMagic) {
            if (len - pos < 8) return dec.Fail("truncated skippable frame");
            uint64_t size = Read32(data + pos + 4);
            if (size > len - pos - 8) return dec.Fail("truncated skippable frame");
            pos += 8 + static_cast<size_t>(size);
            continue;
        }
        if (magic != kFrameMagic) return dec.Fail("bad frame magic");
        size_t used = 0;
        if (!dec.DecodeFrame(data + pos, len - pos, &used)) return false;
        pos += used;
    }
    return true;
}

}  // namespace zstd
