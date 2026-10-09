// Coverage for ccitt_codec.h/.cpp's Group 3/Group 4 fax decoder.
//
// The two real test vectors are strips lifted out of TIFF files
// libtiff wrote (via Pillow's `compression="group3"` /
// `compression="group4"`) from one hand-built 64x24 bitmap -- the same
// "an independent real-world encoder is the oracle" rigor
// pdf_filters_test.cpp's own LZW vectors use, and the reason those
// bytes are embedded here rather than read from a file (test/ is
// gitignored in this repo). One bitmap through both coders also pins
// down that the one- and two-dimensional paths agree with each other,
// not just each with itself.
//
// Polarity, which is easy to get backwards in both directions at once
// and so is worth stating: libtiff's fax coder treats a 0 sample as a
// CCITT *white* pixel, so kBitmap below (1 = ink) is what was handed
// to it, and PDF's own default /BlackIs1 false means this decoder
// emits the mirror image of that -- 1 bits for white, 0 for ink.

#include "ccitt_codec.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

constexpr int kWidth = 64;
constexpr int kHeight = 24;
constexpr int kStride = (kWidth + 7) / 8;

// The source bitmap, 1 = ink: a hollow box (rows 3-9), a pair of
// diagonals (rows 12-17), and a 3-on/3-off comb (row 20). Between them
// they cover short and long runs in both colours, rows that repeat the
// one above exactly (what Group 4's vertical mode compresses to
// nothing) and rows that don't, plus make-up-length white runs on the
// blank rows.
const char *const kBitmap[kHeight] = {
    "                                                                ",
    "                                                                ",
    "                                                                ",
    "        ********************************                        ",
    "        *                              *                        ",
    "        *                              *                        ",
    "        *                              *                        ",
    "        *                              *                        ",
    "        *                              *                        ",
    "        ********************************                        ",
    "                                                                ",
    "                                                                ",
    "          *             *                                       ",
    "           *             *                                      ",
    "            *             *                                     ",
    "             *             *                                    ",
    "              *             *                                   ",
    "               *             *                                  ",
    "                                                                ",
    "                                                                ",
    "***   ***   ***   ***   ***   ***   ***   ***   ***   ***   *** ",
    "                                                                ",
    "                                                                ",
    "                                                                ",
};

// Group 3 one-dimensional (T.4 Modified Huffman), libtiff defaults:
// no EOL codes, no byte alignment -- i.e. PDF's own /K 0.
const unsigned char kG3[] = {
    0x00, 0x1D, 0x9A, 0x80, 0x0E, 0xCD, 0x40, 0x07, 0x66, 0xA0, 0x03, 0x30, 0x6A, 0x50, 0x00, 0x33,
    0x40, 0x69, 0x40, 0x00, 0xCD, 0x01, 0xA5, 0x00, 0x03, 0x34, 0x06, 0x94, 0x00, 0x0C, 0xD0, 0x1A,
    0x50, 0x00, 0x33, 0x40, 0x69, 0x40, 0x00, 0xCC, 0x1A, 0x94, 0x00, 0x0E, 0xCD, 0x40, 0x07, 0x66,
    0xA0, 0x02, 0x74, 0x1A, 0x28, 0x00, 0x14, 0x20, 0xD0, 0xB8, 0x00, 0x90, 0x83, 0x42, 0xC0, 0x02,
    0x1A, 0x0D, 0x0A, 0x80, 0x0E, 0x88, 0x34, 0x28, 0x00, 0x3A, 0xA0, 0xD0, 0x98, 0x00, 0xEC, 0xD4,
    0x00, 0x76, 0x6A, 0x00, 0x26, 0xB4, 0x51, 0x45, 0x14, 0x51, 0x45, 0x14, 0x51, 0x0E, 0x00, 0x3B,
    0x35, 0x00, 0x1D, 0x9A, 0x80, 0x0E, 0xCD, 0x40,
};

// Group 4 (T.6, pure two-dimensional) -- PDF's own /K -1.
const unsigned char kG4[] = {
    0xE6, 0x60, 0xD5, 0x94, 0x07, 0xFF, 0xFF, 0xFC, 0x71, 0xC9, 0xD1, 0x0D, 0x59, 0x41, 0x94, 0x2E,
    0xCA, 0x0C, 0xA1, 0x66, 0x50, 0x65, 0x0A, 0xB2, 0x83, 0x28, 0x51, 0x94, 0x19, 0x42, 0x62, 0x39,
    0x35, 0x8C, 0x46, 0x23, 0x11, 0x88, 0xC4, 0x62, 0x31, 0x18, 0x8C, 0x46, 0x28, 0x88, 0x88, 0x88,
    0x88, 0x88, 0xF0, 0x01, 0x00, 0x10,
};

std::string AsString(const unsigned char *b, size_t n) {
    return std::string(reinterpret_cast<const char *>(b), n);
}

// kBitmap packed the way a /BlackIs1 false image reads it: 1 bit per
// pixel, MSB first, 1 = white, rows byte-aligned.
std::string ExpectedSamples(bool black_is_1) {
    std::string s(static_cast<size_t>(kStride) * kHeight, static_cast<char>(black_is_1 ? 0x00 : 0xFF));
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (kBitmap[y][x] != '*') continue;
            size_t at = static_cast<size_t>(y) * kStride + static_cast<size_t>(x) / 8;
            unsigned char mask = static_cast<unsigned char>(0x80u >> (x % 8));
            unsigned char v = static_cast<unsigned char>(s[at]);
            s[at] = static_cast<char>(black_is_1 ? (v | mask) : (v & ~mask));
        }
    }
    return s;
}

void TestGroup3OneDimensional() {
    ccitt::Params p;
    p.k = 0;
    p.columns = kWidth;
    p.rows = kHeight;
    std::string out;
    int rows = 0;
    CHECK(ccitt::Decode(AsString(kG3, sizeof kG3), p, &out, &rows));
    CHECK(rows == kHeight);
    CHECK(out == ExpectedSamples(false));
}

void TestGroup4TwoDimensional() {
    ccitt::Params p;
    p.k = -1;
    p.columns = kWidth;
    p.rows = kHeight;
    std::string out;
    int rows = 0;
    CHECK(ccitt::Decode(AsString(kG4, sizeof kG4), p, &out, &rows));
    CHECK(rows == kHeight);
    CHECK(out == ExpectedSamples(false));
}

// /BlackIs1 only flips what the samples mean, never how the runs
// decode -- so the same bytes come back exactly inverted.
void TestBlackIs1Inverts() {
    ccitt::Params p;
    p.k = -1;
    p.columns = kWidth;
    p.rows = kHeight;
    p.black_is_1 = true;
    std::string out;
    int rows = 0;
    CHECK(ccitt::Decode(AsString(kG4, sizeof kG4), p, &out, &rows));
    CHECK(out == ExpectedSamples(true));
}

// Without a stated /Rows the decoder runs to the end of the data, and
// must still land on exactly the rows the encoder wrote.
void TestRowsInferredFromData() {
    ccitt::Params p;
    p.k = 0;
    p.columns = kWidth;
    p.rows = 0;
    std::string out;
    int rows = 0;
    CHECK(ccitt::Decode(AsString(kG3, sizeof kG3), p, &out, &rows));
    CHECK(rows == kHeight);
    CHECK(out == ExpectedSamples(false));
}

// A stream cut short still produces a full-height image (the rows it
// reached, then white) rather than failing the whole figure -- what
// keeps a damaged PDF rendering everything else on its page.
void TestTruncatedStreamKeepsWhatItGot() {
    ccitt::Params p;
    p.k = 0;
    p.columns = kWidth;
    p.rows = kHeight;
    std::string out;
    int rows = 0;
    CHECK(ccitt::Decode(AsString(kG3, 20), p, &out, &rows));
    CHECK(rows > 0 && rows < kHeight);
    CHECK(out.size() == static_cast<size_t>(kStride) * kHeight);
    const std::string full = ExpectedSamples(false);
    // The rows it did reach match the real thing...
    CHECK(out.compare(0, static_cast<size_t>(kStride) * static_cast<size_t>(rows), full, 0,
                       static_cast<size_t>(kStride) * static_cast<size_t>(rows)) == 0);
    // ...and the tail is white, not garbage.
    for (size_t i = static_cast<size_t>(kStride) * static_cast<size_t>(rows); i < out.size(); ++i)
        CHECK(static_cast<unsigned char>(out[i]) == 0xFF);
}

void TestRejectsNonsense() {
    ccitt::Params p;
    p.k = 0;
    p.columns = kWidth;
    std::string out;
    int rows = 0;
    // No input at all: nothing decodable, and no output pretending otherwise.
    CHECK(!ccitt::Decode("", p, &out, &rows));
    CHECK(out.empty());
    CHECK(rows == 0);
    // A degenerate /Columns is refused rather than divided by.
    p.columns = 0;
    CHECK(!ccitt::Decode(AsString(kG3, sizeof kG3), p, &out, &rows));
}

}  // namespace

int main() {
    TestGroup3OneDimensional();
    TestGroup4TwoDimensional();
    TestBlackIs1Inverts();
    TestRowsInferredFromData();
    TestTruncatedStreamKeepsWhatItGot();
    TestRejectsNonsense();
    std::printf("ccitt_codec_test: all checks passed\n");
    return 0;
}
