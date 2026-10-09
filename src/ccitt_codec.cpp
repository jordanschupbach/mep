#include "ccitt_codec.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace ccitt {

namespace {

// --- the T.4 run-length code tables ----------------------------------
//
// Written out as (code bits, bit count, run length) triples straight
// from T.4 Tables 1-3 rather than generated, because there is no
// pattern to generate them from: they are a hand-assigned Huffman
// code. `bits` holds the code right-aligned, so `len` says how many
// of its low bits are the code.

struct RunCode {
    unsigned short bits;
    unsigned char len;
    short run;
};

// T.4 Table 1: white terminating codes (runs 0-63).
constexpr RunCode kWhiteTerm[] = {
    {0x35, 8, 0},   {0x07, 6, 1},   {0x07, 4, 2},   {0x08, 4, 3},   {0x0B, 4, 4},   {0x0C, 4, 5},
    {0x0E, 4, 6},   {0x0F, 4, 7},   {0x13, 5, 8},   {0x14, 5, 9},   {0x07, 5, 10},  {0x08, 5, 11},
    {0x08, 6, 12},  {0x03, 6, 13},  {0x34, 6, 14},  {0x35, 6, 15},  {0x2A, 6, 16},  {0x2B, 6, 17},
    {0x27, 7, 18},  {0x0C, 7, 19},  {0x08, 7, 20},  {0x17, 7, 21},  {0x03, 7, 22},  {0x04, 7, 23},
    {0x28, 7, 24},  {0x2B, 7, 25},  {0x13, 7, 26},  {0x24, 7, 27},  {0x18, 7, 28},  {0x02, 8, 29},
    {0x03, 8, 30},  {0x1A, 8, 31},  {0x1B, 8, 32},  {0x12, 8, 33},  {0x13, 8, 34},  {0x14, 8, 35},
    {0x15, 8, 36},  {0x16, 8, 37},  {0x17, 8, 38},  {0x28, 8, 39},  {0x29, 8, 40},  {0x2A, 8, 41},
    {0x2B, 8, 42},  {0x2C, 8, 43},  {0x2D, 8, 44},  {0x04, 8, 45},  {0x05, 8, 46},  {0x0A, 8, 47},
    {0x0B, 8, 48},  {0x52, 8, 49},  {0x53, 8, 50},  {0x54, 8, 51},  {0x55, 8, 52},  {0x24, 8, 53},
    {0x25, 8, 54},  {0x58, 8, 55},  {0x59, 8, 56},  {0x5A, 8, 57},  {0x5B, 8, 58},  {0x4A, 8, 59},
    {0x4B, 8, 60},  {0x32, 8, 61},  {0x33, 8, 62},  {0x34, 8, 63},
};

// T.4 Table 2: white make-up codes (multiples of 64, up to 1728).
constexpr RunCode kWhiteMakeup[] = {
    {0x1B, 5, 64},    {0x12, 5, 128},   {0x17, 6, 192},   {0x37, 7, 256},   {0x36, 8, 320},
    {0x37, 8, 384},   {0x64, 8, 448},   {0x65, 8, 512},   {0x68, 8, 576},   {0x67, 8, 640},
    {0xCC, 9, 704},   {0xCD, 9, 768},   {0xD2, 9, 832},   {0xD3, 9, 896},   {0xD4, 9, 960},
    {0xD5, 9, 1024},  {0xD6, 9, 1088},  {0xD7, 9, 1152},  {0xD8, 9, 1216},  {0xD9, 9, 1280},
    {0xDA, 9, 1344},  {0xDB, 9, 1408},  {0x98, 9, 1472},  {0x99, 9, 1536},  {0x9A, 9, 1600},
    {0x18, 6, 1664},  {0x9B, 9, 1728},
};

// T.4 Table 1: black terminating codes (runs 0-63).
constexpr RunCode kBlackTerm[] = {
    {0x037, 10, 0},  {0x002, 3, 1},   {0x003, 2, 2},   {0x002, 2, 3},   {0x003, 3, 4},   {0x003, 4, 5},
    {0x002, 4, 6},   {0x003, 5, 7},   {0x005, 6, 8},   {0x004, 6, 9},   {0x004, 7, 10},  {0x005, 7, 11},
    {0x007, 7, 12},  {0x004, 8, 13},  {0x007, 8, 14},  {0x018, 9, 15},  {0x017, 10, 16}, {0x018, 10, 17},
    {0x008, 10, 18}, {0x067, 11, 19}, {0x068, 11, 20}, {0x06C, 11, 21}, {0x037, 11, 22}, {0x028, 11, 23},
    {0x017, 11, 24}, {0x018, 11, 25}, {0xCA, 12, 26},  {0xCB, 12, 27},  {0xCC, 12, 28},  {0xCD, 12, 29},
    {0x68, 12, 30},  {0x69, 12, 31},  {0x6A, 12, 32},  {0x6B, 12, 33},  {0xD2, 12, 34},  {0xD3, 12, 35},
    {0xD4, 12, 36},  {0xD5, 12, 37},  {0xD6, 12, 38},  {0xD7, 12, 39},  {0x6C, 12, 40},  {0x6D, 12, 41},
    {0xDA, 12, 42},  {0xDB, 12, 43},  {0x54, 12, 44},  {0x55, 12, 45},  {0x56, 12, 46},  {0x57, 12, 47},
    {0x64, 12, 48},  {0x65, 12, 49},  {0x52, 12, 50},  {0x53, 12, 51},  {0x24, 12, 52},  {0x37, 12, 53},
    {0x38, 12, 54},  {0x27, 12, 55},  {0x28, 12, 56},  {0x58, 12, 57},  {0x59, 12, 58},  {0x2B, 12, 59},
    {0x2C, 12, 60},  {0x5A, 12, 61},  {0x66, 12, 62},  {0x67, 12, 63},
};

// T.4 Table 2: black make-up codes.
constexpr RunCode kBlackMakeup[] = {
    {0x0F, 10, 64},   {0xC8, 12, 128},  {0xC9, 12, 192},  {0x5B, 12, 256},  {0x33, 12, 320},
    {0x34, 12, 384},  {0x35, 12, 448},  {0x6C, 13, 512},  {0x6D, 13, 576},  {0x4A, 13, 640},
    {0x4B, 13, 704},  {0x4C, 13, 768},  {0x4D, 13, 832},  {0x72, 13, 896},  {0x73, 13, 960},
    {0x74, 13, 1024}, {0x75, 13, 1088}, {0x76, 13, 1152}, {0x77, 13, 1216}, {0x52, 13, 1280},
    {0x53, 13, 1344}, {0x54, 13, 1408}, {0x55, 13, 1472}, {0x5A, 13, 1536}, {0x5B, 13, 1600},
    {0x64, 13, 1664}, {0x65, 13, 1728},
};

// T.4 Table 3: extended make-up codes, shared by both colours.
constexpr RunCode kExtMakeup[] = {
    {0x08, 11, 1792}, {0x0C, 11, 1856}, {0x0D, 11, 1920}, {0x12, 12, 1984}, {0x13, 12, 2048},
    {0x14, 12, 2112}, {0x15, 12, 2176}, {0x16, 12, 2240}, {0x17, 12, 2304}, {0x1C, 12, 2368},
    {0x1D, 12, 2432}, {0x1E, 12, 2496}, {0x1F, 12, 2560},
};

// The longest run code is 13 bits (black make-ups), so a flat table
// over the next 13 bits of input decodes any of them in one indexed
// read -- 8192 entries each for white and black, built once. Prefix-
// freeness is what makes this work: every 13-bit window starting with
// a given code maps to that code, so the table is just each code
// splatted across its own suffix range.
constexpr int kLookupBits = 13;
constexpr int kLookupSize = 1 << kLookupBits;

struct Decoded {
    short run;        // -1 = no code starts here
    unsigned char len;
};

struct RunTable {
    std::array<Decoded, kLookupSize> e;
};

void Splat(RunTable &t, const RunCode *codes, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const RunCode &c = codes[i];
        int shift = kLookupBits - c.len;
        int base = static_cast<int>(c.bits) << shift;
        for (int s = 0; s < (1 << shift); ++s) {
            t.e[static_cast<size_t>(base + s)] = {c.run, c.len};
        }
    }
}

const RunTable &WhiteTable() {
    static const RunTable t = [] {
        RunTable r{};
        r.e.fill({-1, 0});
        Splat(r, kWhiteTerm, std::size(kWhiteTerm));
        Splat(r, kWhiteMakeup, std::size(kWhiteMakeup));
        Splat(r, kExtMakeup, std::size(kExtMakeup));
        return r;
    }();
    return t;
}

const RunTable &BlackTable() {
    static const RunTable t = [] {
        RunTable r{};
        r.e.fill({-1, 0});
        Splat(r, kBlackTerm, std::size(kBlackTerm));
        Splat(r, kBlackMakeup, std::size(kBlackMakeup));
        Splat(r, kExtMakeup, std::size(kExtMakeup));
        return r;
    }();
    return t;
}

// --- bit reader -------------------------------------------------------
//
// MSB-first, and deliberately tolerant of reading past the end: Peek
// pads with zero bits so the row loop can finish its arithmetic and
// exit on Exhausted() rather than every call site checking a bound.

class BitReader {
public:
    BitReader(const unsigned char *data, size_t len) : data_(data), len_(len) {}

    unsigned Peek(int n) const {
        unsigned v = 0;
        for (int i = 0; i < n; ++i) {
            size_t bit = pos_ + static_cast<size_t>(i);
            size_t byte = bit >> 3;
            unsigned b = byte < len_ ? ((data_[byte] >> (7 - (bit & 7))) & 1u) : 0u;
            v = (v << 1) | b;
        }
        return v;
    }
    void Skip(int n) { pos_ += static_cast<size_t>(n); }
    void AlignByte() { pos_ = (pos_ + 7) & ~static_cast<size_t>(7); }
    bool Exhausted() const { return pos_ >= len_ * 8; }
    size_t BitPos() const { return pos_; }

private:
    const unsigned char *data_;
    size_t len_;
    size_t pos_ = 0;
};

// One colour's total run: any number of make-up codes (>= 64) followed
// by one terminating code (0-63), per T.4 4.1.3. Returns -1 if the bits
// here aren't a valid code at all.
int ReadRun(BitReader &br, bool white) {
    const RunTable &t = white ? WhiteTable() : BlackTable();
    int total = 0;
    for (int guard = 0; guard < 64; ++guard) {
        if (br.Exhausted()) return -1;
        Decoded d = t.e[br.Peek(kLookupBits)];
        if (d.run < 0) return -1;
        br.Skip(d.len);
        total += d.run;
        if (d.run < 64) return total;  // terminating code ends the run
    }
    return -1;
}

// An EOL is eleven or more 0 bits followed by a single 1 (T.4 4.1.2 --
// the "or more" is fill, inserted to pad a line out to a minimum
// transmission time). Consumes one if it is next; returns false and
// leaves the position alone otherwise.
bool SkipEol(BitReader &br) {
    int zeros = 0;
    while (zeros < 64) {
        unsigned bit = br.Peek(zeros + 1) & 1u;
        if (bit == 1u) break;
        ++zeros;
    }
    if (zeros < 11) return false;
    br.Skip(zeros + 1);
    return true;
}

// The changing elements of a row: the x of every colour transition,
// left to right. A row starts white, so changes[0] is where the first
// black run begins, changes[1] where it ends, and so on. Both T.4's
// two-dimensional mode and the final paint step work off these rather
// than off pixels.
using Changes = std::vector<int>;

// b1 per T.4 4.2.1.3.1: the first changing element on the reference
// line strictly right of a0 whose colour (the colour it changes *to*)
// is the opposite of `colour` -- i.e. an even index when the current
// colour is white, an odd one when it is black.
int FindB1(const Changes &ref, int a0, bool white, int columns) {
    size_t i = white ? 0 : 1;
    for (; i < ref.size(); i += 2) {
        if (ref[i] > a0) return ref[i];
    }
    return columns;
}

int FindB2(const Changes &ref, int b1, int columns) {
    for (size_t i = 0; i < ref.size(); ++i) {
        if (ref[i] == b1 && i + 1 < ref.size()) return ref[i + 1];
        if (ref[i] > b1) return ref[i];
    }
    return columns;
}

bool DecodeRow1D(BitReader &br, int columns, Changes *out) {
    out->clear();
    int pos = 0;
    bool white = true;
    while (pos < columns) {
        int run = ReadRun(br, white);
        if (run < 0) return false;
        pos = std::min(pos + run, columns);
        out->push_back(pos);
        white = !white;
    }
    return true;
}

bool DecodeRow2D(BitReader &br, const Changes &ref, int columns, Changes *out) {
    out->clear();
    int a0 = -1;
    bool white = true;
    int guard = 0;
    while (a0 < columns) {
        if (++guard > 2 * columns + 64) return false;
        if (br.Exhausted()) return false;
        int b1 = FindB1(ref, a0, white, columns);
        int b2 = FindB2(ref, b1, columns);

        // Mode codes, T.4 Table 4. Matched longest-first by inspecting
        // the next 7 bits; every mode is a prefix code within them.
        unsigned w = br.Peek(7);
        int a1;
        if ((w >> 6) == 0x1u) {               // V0: 1
            br.Skip(1);
            a1 = b1;
        } else if ((w >> 4) == 0x3u) {        // VR1: 011
            br.Skip(3);
            a1 = b1 + 1;
        } else if ((w >> 4) == 0x2u) {        // VL1: 010
            br.Skip(3);
            a1 = b1 - 1;
        } else if ((w >> 4) == 0x1u) {        // H: 001
            br.Skip(3);
            int start = a0 < 0 ? 0 : a0;
            int r1 = ReadRun(br, white);
            if (r1 < 0) return false;
            int r2 = ReadRun(br, !white);
            if (r2 < 0) return false;
            int m1 = std::min(start + r1, columns);
            int m2 = std::min(m1 + r2, columns);
            out->push_back(m1);
            out->push_back(m2);
            a0 = m2;
            continue;  // colour is unchanged by a horizontal pair
        } else if ((w >> 3) == 0x1u) {        // P: 0001
            br.Skip(4);
            a0 = b2;   // the run of `white` simply extends past b2
            continue;
        } else if ((w >> 1) == 0x3u) {        // VR2: 000011
            br.Skip(6);
            a1 = b1 + 2;
        } else if ((w >> 1) == 0x2u) {        // VL2: 000010
            br.Skip(6);
            a1 = b1 - 2;
        } else if (w == 0x3u) {               // VR3: 0000011
            br.Skip(7);
            a1 = b1 + 3;
        } else if (w == 0x2u) {               // VL3: 0000010
            br.Skip(7);
            a1 = b1 - 3;
        } else {
            return false;  // EOL, an extension code, or damage
        }
        a1 = std::clamp(a1, 0, columns);
        out->push_back(a1);
        a0 = a1;
        white = !white;
    }
    return true;
}

// Paints one row's changing elements into packed 1-bpp samples.
// `white_bit` is what a white pixel reads as, which /BlackIs1 picks.
void PaintRow(const Changes &changes, int columns, unsigned char white_bit, unsigned char *row) {
    size_t stride = (static_cast<size_t>(columns) + 7) / 8;
    std::fill(row, row + stride, white_bit ? 0xFFu : 0x00u);
    // changes[0] starts black, changes[1] back to white, alternating.
    for (size_t i = 0; i + 1 <= changes.size(); i += 2) {
        int from = changes[i];
        int to = (i + 1 < changes.size()) ? changes[i + 1] : columns;
        for (int x = std::max(0, from); x < std::min(to, columns); ++x) {
            size_t byte = static_cast<size_t>(x) >> 3;
            unsigned char mask = static_cast<unsigned char>(0x80u >> (x & 7));
            if (white_bit) {
                row[byte] = static_cast<unsigned char>(row[byte] & ~mask);
            } else {
                row[byte] = static_cast<unsigned char>(row[byte] | mask);
            }
        }
    }
}

}  // namespace

bool Decode(const std::string &raw, const Params &p, std::string *out, int *out_rows) {
    out->clear();
    if (out_rows) *out_rows = 0;
    if (p.columns <= 0 || p.columns > 1 << 16) return false;

    const size_t stride = (static_cast<size_t>(p.columns) + 7) / 8;
    const unsigned char white_bit = p.black_is_1 ? 0u : 1u;

    // A stated /Rows is allocated up front so a stream that dies early
    // still hands back a full-height image (the tail staying white),
    // which is what keeps the caller's /Height contract intact.
    const bool height_known = p.rows > 0;
    if (height_known) out->assign(stride * static_cast<size_t>(p.rows), static_cast<char>(white_bit ? 0xFF : 0x00));

    BitReader br(reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
    Changes ref, cur;
    // The imaginary line above the first one is all white (T.6 2.2.1),
    // i.e. it changes colour only at the right edge.
    ref.assign(2, p.columns);

    std::vector<unsigned char> row(stride);
    int y = 0;
    for (;; ++y) {
        if (height_known && y >= p.rows) break;
        if (br.Exhausted()) break;

        if (p.byte_align && p.k >= 0) br.AlignByte();

        // Group 3 wraps each line in an EOL; Group 4 has none, but
        // tolerating a stray one costs nothing and some producers emit
        // an EOFB at the end.
        bool had_eol = SkipEol(br);
        if (br.Exhausted()) break;

        bool two_d;
        if (p.k < 0) {
            two_d = true;
        } else if (p.k == 0) {
            two_d = false;
        } else {
            // Mixed mode: the bit right after the EOL tags this line
            // (1 = one-dimensional). Without an EOL to hang it off,
            // assume one-dimensional rather than consuming a data bit.
            if (had_eol) {
                two_d = br.Peek(1) == 0;
                br.Skip(1);
            } else {
                two_d = false;
            }
        }

        if (p.byte_align && p.k < 0) br.AlignByte();

        bool ok = two_d ? DecodeRow2D(br, ref, p.columns, &cur) : DecodeRow1D(br, p.columns, &cur);
        if (!ok) break;

        PaintRow(cur, p.columns, white_bit, row.data());
        if (height_known) {
            std::copy(row.begin(), row.end(), out->begin() + static_cast<std::ptrdiff_t>(stride * static_cast<size_t>(y)));
        } else {
            out->append(reinterpret_cast<const char *>(row.data()), stride);
        }

        // The reference line for the next row, with the two sentinel
        // columns FindB1/FindB2 fall back on.
        ref = cur;
        ref.push_back(p.columns);
        ref.push_back(p.columns);
    }

    if (out_rows) *out_rows = y;
    if (y == 0) {
        out->clear();
        return false;
    }
    return true;
}

}  // namespace ccitt
