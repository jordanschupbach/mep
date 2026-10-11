// aac::Decoder (aac_decoder.h): AAC-LC per ISO/IEC 14496-3 subpart 4.
//
// The order of work for one access unit follows the standard's decoding
// process: parse every syntax element of the raw_data_block into per-
// channel side information and quantized spectra, inverse-quantize and
// rescale, apply the channel-pair tools (M/S, then intensity), perceptual
// noise substitution, TNS, and finally the IMDCT filterbank with
// windowing and overlap-add against the previous frame's second half.
//
// The Huffman codebooks, the scalefactor codebook and the scalefactor
// band tables below are the standard's own constants (ISO/IEC 14496-3
// tables 4.A.1-4.A.12 and 4.129-4.140); aac_decoder_test.cpp checks every
// codebook is prefix-free and complete.

#include "aac_decoder.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace aac {

namespace {

// --- Standard tables ----------------------------------------------------------

// clang-format off
constexpr uint32_t kScalefactorCode[121] = {
    0x3ffe8, 0x3ffe6, 0x3ffe7, 0x3ffe5, 0x7fff5, 0x7fff1, 0x7ffed, 0x7fff6,
    0x7ffee, 0x7ffef, 0x7fff0, 0x7fffc, 0x7fffd, 0x7ffff, 0x7fffe, 0x7fff7,
    0x7fff8, 0x7fffb, 0x7fff9, 0x3ffe4, 0x7fffa, 0x3ffe3, 0x1ffef, 0x1fff0,
    0x0fff5, 0x1ffee, 0x0fff2, 0x0fff3, 0x0fff4, 0x0fff1, 0x07ff6, 0x07ff7,
    0x03ff9, 0x03ff5, 0x03ff7, 0x03ff3, 0x03ff6, 0x03ff2, 0x01ff7, 0x01ff5,
    0x00ff9, 0x00ff7, 0x00ff6, 0x007f9, 0x00ff4, 0x007f8, 0x003f9, 0x003f7,
    0x003f5, 0x001f8, 0x001f7, 0x000fa, 0x000f8, 0x000f6, 0x00079, 0x0003a,
    0x00038, 0x0001a, 0x0000b, 0x00004, 0x00000, 0x0000a, 0x0000c, 0x0001b,
    0x00039, 0x0003b, 0x00078, 0x0007a, 0x000f7, 0x000f9, 0x001f6, 0x001f9,
    0x003f4, 0x003f6, 0x003f8, 0x007f5, 0x007f4, 0x007f6, 0x007f7, 0x00ff5,
    0x00ff8, 0x01ff4, 0x01ff6, 0x01ff8, 0x03ff8, 0x03ff4, 0x0fff0, 0x07ff4,
    0x0fff6, 0x07ff5, 0x3ffe2, 0x7ffd9, 0x7ffda, 0x7ffdb, 0x7ffdc, 0x7ffdd,
    0x7ffde, 0x7ffd8, 0x7ffd2, 0x7ffd3, 0x7ffd4, 0x7ffd5, 0x7ffd6, 0x7fff2,
    0x7ffdf, 0x7ffe7, 0x7ffe8, 0x7ffe9, 0x7ffea, 0x7ffeb, 0x7ffe6, 0x7ffe0,
    0x7ffe1, 0x7ffe2, 0x7ffe3, 0x7ffe4, 0x7ffe5, 0x7ffd7, 0x7ffec, 0x7fff4,
    0x7fff3,
};
constexpr uint8_t kScalefactorBits[121] = {
    18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 18, 19, 18, 17, 17, 16, 17, 16, 16, 16, 16, 15, 15,
    14, 14, 14, 14, 14, 14, 13, 13, 12, 12, 12, 11, 12, 11, 10, 10,
    10, 9, 9, 8, 8, 8, 7, 6, 6, 5, 4, 3, 1, 4, 4, 5,
    6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 10, 11, 11, 11, 11, 12,
    12, 13, 13, 13, 14, 14, 16, 15, 16, 15, 18, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19,
};
constexpr uint16_t kCodes1[81] = {
    0x07f8, 0x01f1, 0x07fd, 0x03f5, 0x0068, 0x03f0, 0x07f7, 0x01ec,
    0x07f5, 0x03f1, 0x0072, 0x03f4, 0x0074, 0x0011, 0x0076, 0x01eb,
    0x006c, 0x03f6, 0x07fc, 0x01e1, 0x07f1, 0x01f0, 0x0061, 0x01f6,
    0x07f2, 0x01ea, 0x07fb, 0x01f2, 0x0069, 0x01ed, 0x0077, 0x0017,
    0x006f, 0x01e6, 0x0064, 0x01e5, 0x0067, 0x0015, 0x0062, 0x0012,
    0x0000, 0x0014, 0x0065, 0x0016, 0x006d, 0x01e9, 0x0063, 0x01e4,
    0x006b, 0x0013, 0x0071, 0x01e3, 0x0070, 0x01f3, 0x07fe, 0x01e7,
    0x07f3, 0x01ef, 0x0060, 0x01ee, 0x07f0, 0x01e2, 0x07fa, 0x03f3,
    0x006a, 0x01e8, 0x0075, 0x0010, 0x0073, 0x01f4, 0x006e, 0x03f7,
    0x07f6, 0x01e0, 0x07f9, 0x03f2, 0x0066, 0x01f5, 0x07ff, 0x01f7,
    0x07f4,
};
constexpr uint8_t kBits1[81] = {
    11, 9, 11, 10, 7, 10, 11, 9, 11, 10, 7, 10, 7, 5, 7, 9,
    7, 10, 11, 9, 11, 9, 7, 9, 11, 9, 11, 9, 7, 9, 7, 5,
    7, 9, 7, 9, 7, 5, 7, 5, 1, 5, 7, 5, 7, 9, 7, 9,
    7, 5, 7, 9, 7, 9, 11, 9, 11, 9, 7, 9, 11, 9, 11, 10,
    7, 9, 7, 5, 7, 9, 7, 10, 11, 9, 11, 10, 7, 9, 11, 9,
    11,
};
constexpr uint16_t kCodes2[81] = {
    0x01f3, 0x006f, 0x01fd, 0x00eb, 0x0023, 0x00ea, 0x01f7, 0x00e8,
    0x01fa, 0x00f2, 0x002d, 0x0070, 0x0020, 0x0006, 0x002b, 0x006e,
    0x0028, 0x00e9, 0x01f9, 0x0066, 0x00f8, 0x00e7, 0x001b, 0x00f1,
    0x01f4, 0x006b, 0x01f5, 0x00ec, 0x002a, 0x006c, 0x002c, 0x000a,
    0x0027, 0x0067, 0x001a, 0x00f5, 0x0024, 0x0008, 0x001f, 0x0009,
    0x0000, 0x0007, 0x001d, 0x000b, 0x0030, 0x00ef, 0x001c, 0x0064,
    0x001e, 0x000c, 0x0029, 0x00f3, 0x002f, 0x00f0, 0x01fc, 0x0071,
    0x01f2, 0x00f4, 0x0021, 0x00e6, 0x00f7, 0x0068, 0x01f8, 0x00ee,
    0x0022, 0x0065, 0x0031, 0x0002, 0x0026, 0x00ed, 0x0025, 0x006a,
    0x01fb, 0x0072, 0x01fe, 0x0069, 0x002e, 0x00f6, 0x01ff, 0x006d,
    0x01f6,
};
constexpr uint8_t kBits2[81] = {
    9, 7, 9, 8, 6, 8, 9, 8, 9, 8, 6, 7, 6, 5, 6, 7,
    6, 8, 9, 7, 8, 8, 6, 8, 9, 7, 9, 8, 6, 7, 6, 5,
    6, 7, 6, 8, 6, 5, 6, 5, 3, 5, 6, 5, 6, 8, 6, 7,
    6, 5, 6, 8, 6, 8, 9, 7, 9, 8, 6, 8, 8, 7, 9, 8,
    6, 7, 6, 4, 6, 8, 6, 7, 9, 7, 9, 7, 6, 8, 9, 7,
    9,
};
constexpr uint16_t kCodes3[81] = {
    0x0000, 0x0009, 0x00ef, 0x000b, 0x0019, 0x00f0, 0x01eb, 0x01e6,
    0x03f2, 0x000a, 0x0035, 0x01ef, 0x0034, 0x0037, 0x01e9, 0x01ed,
    0x01e7, 0x03f3, 0x01ee, 0x03ed, 0x1ffa, 0x01ec, 0x01f2, 0x07f9,
    0x07f8, 0x03f8, 0x0ff8, 0x0008, 0x0038, 0x03f6, 0x0036, 0x0075,
    0x03f1, 0x03eb, 0x03ec, 0x0ff4, 0x0018, 0x0076, 0x07f4, 0x0039,
    0x0074, 0x03ef, 0x01f3, 0x01f4, 0x07f6, 0x01e8, 0x03ea, 0x1ffc,
    0x00f2, 0x01f1, 0x0ffb, 0x03f5, 0x07f3, 0x0ffc, 0x00ee, 0x03f7,
    0x7ffe, 0x01f0, 0x07f5, 0x7ffd, 0x1ffb, 0x3ffa, 0xffff, 0x00f1,
    0x03f0, 0x3ffc, 0x01ea, 0x03ee, 0x3ffb, 0x0ff6, 0x0ffa, 0x7ffc,
    0x07f2, 0x0ff5, 0xfffe, 0x03f4, 0x07f7, 0x7ffb, 0x0ff7, 0x0ff9,
    0x7ffa,
};
constexpr uint8_t kBits3[81] = {
    1, 4, 8, 4, 5, 8, 9, 9, 10, 4, 6, 9, 6, 6, 9, 9,
    9, 10, 9, 10, 13, 9, 9, 11, 11, 10, 12, 4, 6, 10, 6, 7,
    10, 10, 10, 12, 5, 7, 11, 6, 7, 10, 9, 9, 11, 9, 10, 13,
    8, 9, 12, 10, 11, 12, 8, 10, 15, 9, 11, 15, 13, 14, 16, 8,
    10, 14, 9, 10, 14, 12, 12, 15, 11, 12, 16, 10, 11, 15, 12, 12,
    15,
};
constexpr uint16_t kCodes4[81] = {
    0x0007, 0x0016, 0x00f6, 0x0018, 0x0008, 0x00ef, 0x01ef, 0x00f3,
    0x07f8, 0x0019, 0x0017, 0x00ed, 0x0015, 0x0001, 0x00e2, 0x00f0,
    0x0070, 0x03f0, 0x01ee, 0x00f1, 0x07fa, 0x00ee, 0x00e4, 0x03f2,
    0x07f6, 0x03ef, 0x07fd, 0x0005, 0x0014, 0x00f2, 0x0009, 0x0004,
    0x00e5, 0x00f4, 0x00e8, 0x03f4, 0x0006, 0x0002, 0x00e7, 0x0003,
    0x0000, 0x006b, 0x00e3, 0x0069, 0x01f3, 0x00eb, 0x00e6, 0x03f6,
    0x006e, 0x006a, 0x01f4, 0x03ec, 0x01f0, 0x03f9, 0x00f5, 0x00ec,
    0x07fb, 0x00ea, 0x006f, 0x03f7, 0x07f9, 0x03f3, 0x0fff, 0x00e9,
    0x006d, 0x03f8, 0x006c, 0x0068, 0x01f5, 0x03ee, 0x01f2, 0x07f4,
    0x07f7, 0x03f1, 0x0ffe, 0x03ed, 0x01f1, 0x07f5, 0x07fe, 0x03f5,
    0x07fc,
};
constexpr uint8_t kBits4[81] = {
    4, 5, 8, 5, 4, 8, 9, 8, 11, 5, 5, 8, 5, 4, 8, 8,
    7, 10, 9, 8, 11, 8, 8, 10, 11, 10, 11, 4, 5, 8, 4, 4,
    8, 8, 8, 10, 4, 4, 8, 4, 4, 7, 8, 7, 9, 8, 8, 10,
    7, 7, 9, 10, 9, 10, 8, 8, 11, 8, 7, 10, 11, 10, 12, 8,
    7, 10, 7, 7, 9, 10, 9, 11, 11, 10, 12, 10, 9, 11, 11, 10,
    11,
};
constexpr uint16_t kCodes5[81] = {
    0x1fff, 0x0ff7, 0x07f4, 0x07e8, 0x03f1, 0x07ee, 0x07f9, 0x0ff8,
    0x1ffd, 0x0ffd, 0x07f1, 0x03e8, 0x01e8, 0x00f0, 0x01ec, 0x03ee,
    0x07f2, 0x0ffa, 0x0ff4, 0x03ef, 0x01f2, 0x00e8, 0x0070, 0x00ec,
    0x01f0, 0x03ea, 0x07f3, 0x07eb, 0x01eb, 0x00ea, 0x001a, 0x0008,
    0x0019, 0x00ee, 0x01ef, 0x07ed, 0x03f0, 0x00f2, 0x0073, 0x000b,
    0x0000, 0x000a, 0x0071, 0x00f3, 0x07e9, 0x07ef, 0x01ee, 0x00ef,
    0x0018, 0x0009, 0x001b, 0x00eb, 0x01e9, 0x07ec, 0x07f6, 0x03eb,
    0x01f3, 0x00ed, 0x0072, 0x00e9, 0x01f1, 0x03ed, 0x07f7, 0x0ff6,
    0x07f0, 0x03e9, 0x01ed, 0x00f1, 0x01ea, 0x03ec, 0x07f8, 0x0ff9,
    0x1ffc, 0x0ffc, 0x0ff5, 0x07ea, 0x03f3, 0x03f2, 0x07f5, 0x0ffb,
    0x1ffe,
};
constexpr uint8_t kBits5[81] = {
    13, 12, 11, 11, 10, 11, 11, 12, 13, 12, 11, 10, 9, 8, 9, 10,
    11, 12, 12, 10, 9, 8, 7, 8, 9, 10, 11, 11, 9, 8, 5, 4,
    5, 8, 9, 11, 10, 8, 7, 4, 1, 4, 7, 8, 11, 11, 9, 8,
    5, 4, 5, 8, 9, 11, 11, 10, 9, 8, 7, 8, 9, 10, 11, 12,
    11, 10, 9, 8, 9, 10, 11, 12, 13, 12, 12, 11, 10, 10, 11, 12,
    13,
};
constexpr uint16_t kCodes6[81] = {
    0x07fe, 0x03fd, 0x01f1, 0x01eb, 0x01f4, 0x01ea, 0x01f0, 0x03fc,
    0x07fd, 0x03f6, 0x01e5, 0x00ea, 0x006c, 0x0071, 0x0068, 0x00f0,
    0x01e6, 0x03f7, 0x01f3, 0x00ef, 0x0032, 0x0027, 0x0028, 0x0026,
    0x0031, 0x00eb, 0x01f7, 0x01e8, 0x006f, 0x002e, 0x0008, 0x0004,
    0x0006, 0x0029, 0x006b, 0x01ee, 0x01ef, 0x0072, 0x002d, 0x0002,
    0x0000, 0x0003, 0x002f, 0x0073, 0x01fa, 0x01e7, 0x006e, 0x002b,
    0x0007, 0x0001, 0x0005, 0x002c, 0x006d, 0x01ec, 0x01f9, 0x00ee,
    0x0030, 0x0024, 0x002a, 0x0025, 0x0033, 0x00ec, 0x01f2, 0x03f8,
    0x01e4, 0x00ed, 0x006a, 0x0070, 0x0069, 0x0074, 0x00f1, 0x03fa,
    0x07ff, 0x03f9, 0x01f6, 0x01ed, 0x01f8, 0x01e9, 0x01f5, 0x03fb,
    0x07fc,
};
constexpr uint8_t kBits6[81] = {
    11, 10, 9, 9, 9, 9, 9, 10, 11, 10, 9, 8, 7, 7, 7, 8,
    9, 10, 9, 8, 6, 6, 6, 6, 6, 8, 9, 9, 7, 6, 4, 4,
    4, 6, 7, 9, 9, 7, 6, 4, 4, 4, 6, 7, 9, 9, 7, 6,
    4, 4, 4, 6, 7, 9, 9, 8, 6, 6, 6, 6, 6, 8, 9, 10,
    9, 8, 7, 7, 7, 7, 8, 10, 11, 10, 9, 9, 9, 9, 9, 10,
    11,
};
constexpr uint16_t kCodes7[64] = {
    0x0000, 0x0005, 0x0037, 0x0074, 0x00f2, 0x01eb, 0x03ed, 0x07f7,
    0x0004, 0x000c, 0x0035, 0x0071, 0x00ec, 0x00ee, 0x01ee, 0x01f5,
    0x0036, 0x0034, 0x0072, 0x00ea, 0x00f1, 0x01e9, 0x01f3, 0x03f5,
    0x0073, 0x0070, 0x00eb, 0x00f0, 0x01f1, 0x01f0, 0x03ec, 0x03fa,
    0x00f3, 0x00ed, 0x01e8, 0x01ef, 0x03ef, 0x03f1, 0x03f9, 0x07fb,
    0x01ed, 0x00ef, 0x01ea, 0x01f2, 0x03f3, 0x03f8, 0x07f9, 0x07fc,
    0x03ee, 0x01ec, 0x01f4, 0x03f4, 0x03f7, 0x07f8, 0x0ffd, 0x0ffe,
    0x07f6, 0x03f0, 0x03f2, 0x03f6, 0x07fa, 0x07fd, 0x0ffc, 0x0fff,
};
constexpr uint8_t kBits7[64] = {
    1, 3, 6, 7, 8, 9, 10, 11, 3, 4, 6, 7, 8, 8, 9, 9,
    6, 6, 7, 8, 8, 9, 9, 10, 7, 7, 8, 8, 9, 9, 10, 10,
    8, 8, 9, 9, 10, 10, 10, 11, 9, 8, 9, 9, 10, 10, 11, 11,
    10, 9, 9, 10, 10, 11, 12, 12, 11, 10, 10, 10, 11, 11, 12, 12,
};
constexpr uint16_t kCodes8[64] = {
    0x000e, 0x0005, 0x0010, 0x0030, 0x006f, 0x00f1, 0x01fa, 0x03fe,
    0x0003, 0x0000, 0x0004, 0x0012, 0x002c, 0x006a, 0x0075, 0x00f8,
    0x000f, 0x0002, 0x0006, 0x0014, 0x002e, 0x0069, 0x0072, 0x00f5,
    0x002f, 0x0011, 0x0013, 0x002a, 0x0032, 0x006c, 0x00ec, 0x00fa,
    0x0071, 0x002b, 0x002d, 0x0031, 0x006d, 0x0070, 0x00f2, 0x01f9,
    0x00ef, 0x0068, 0x0033, 0x006b, 0x006e, 0x00ee, 0x00f9, 0x03fc,
    0x01f8, 0x0074, 0x0073, 0x00ed, 0x00f0, 0x00f6, 0x01f6, 0x01fd,
    0x03fd, 0x00f3, 0x00f4, 0x00f7, 0x01f7, 0x01fb, 0x01fc, 0x03ff,
};
constexpr uint8_t kBits8[64] = {
    5, 4, 5, 6, 7, 8, 9, 10, 4, 3, 4, 5, 6, 7, 7, 8,
    5, 4, 4, 5, 6, 7, 7, 8, 6, 5, 5, 6, 6, 7, 8, 8,
    7, 6, 6, 6, 7, 7, 8, 9, 8, 7, 6, 7, 7, 8, 8, 10,
    9, 7, 7, 8, 8, 8, 9, 9, 10, 8, 8, 8, 9, 9, 9, 10,
};
constexpr uint16_t kCodes9[169] = {
    0x0000, 0x0005, 0x0037, 0x00e7, 0x01de, 0x03ce, 0x03d9, 0x07c8,
    0x07cd, 0x0fc8, 0x0fdd, 0x1fe4, 0x1fec, 0x0004, 0x000c, 0x0035,
    0x0072, 0x00ea, 0x00ed, 0x01e2, 0x03d1, 0x03d3, 0x03e0, 0x07d8,
    0x0fcf, 0x0fd5, 0x0036, 0x0034, 0x0071, 0x00e8, 0x00ec, 0x01e1,
    0x03cf, 0x03dd, 0x03db, 0x07d0, 0x0fc7, 0x0fd4, 0x0fe4, 0x00e6,
    0x0070, 0x00e9, 0x01dd, 0x01e3, 0x03d2, 0x03dc, 0x07cc, 0x07ca,
    0x07de, 0x0fd8, 0x0fea, 0x1fdb, 0x01df, 0x00eb, 0x01dc, 0x01e6,
    0x03d5, 0x03de, 0x07cb, 0x07dd, 0x07dc, 0x0fcd, 0x0fe2, 0x0fe7,
    0x1fe1, 0x03d0, 0x01e0, 0x01e4, 0x03d6, 0x07c5, 0x07d1, 0x07db,
    0x0fd2, 0x07e0, 0x0fd9, 0x0feb, 0x1fe3, 0x1fe9, 0x07c4, 0x01e5,
    0x03d7, 0x07c6, 0x07cf, 0x07da, 0x0fcb, 0x0fda, 0x0fe3, 0x0fe9,
    0x1fe6, 0x1ff3, 0x1ff7, 0x07d3, 0x03d8, 0x03e1, 0x07d4, 0x07d9,
    0x0fd3, 0x0fde, 0x1fdd, 0x1fd9, 0x1fe2, 0x1fea, 0x1ff1, 0x1ff6,
    0x07d2, 0x03d4, 0x03da, 0x07c7, 0x07d7, 0x07e2, 0x0fce, 0x0fdb,
    0x1fd8, 0x1fee, 0x3ff0, 0x1ff4, 0x3ff2, 0x07e1, 0x03df, 0x07c9,
    0x07d6, 0x0fca, 0x0fd0, 0x0fe5, 0x0fe6, 0x1feb, 0x1fef, 0x3ff3,
    0x3ff4, 0x3ff5, 0x0fe0, 0x07ce, 0x07d5, 0x0fc6, 0x0fd1, 0x0fe1,
    0x1fe0, 0x1fe8, 0x1ff0, 0x3ff1, 0x3ff8, 0x3ff6, 0x7ffc, 0x0fe8,
    0x07df, 0x0fc9, 0x0fd7, 0x0fdc, 0x1fdc, 0x1fdf, 0x1fed, 0x1ff5,
    0x3ff9, 0x3ffb, 0x7ffd, 0x7ffe, 0x1fe7, 0x0fcc, 0x0fd6, 0x0fdf,
    0x1fde, 0x1fda, 0x1fe5, 0x1ff2, 0x3ffa, 0x3ff7, 0x3ffc, 0x3ffd,
    0x7fff,
};
constexpr uint8_t kBits9[169] = {
    1, 3, 6, 8, 9, 10, 10, 11, 11, 12, 12, 13, 13, 3, 4, 6,
    7, 8, 8, 9, 10, 10, 10, 11, 12, 12, 6, 6, 7, 8, 8, 9,
    10, 10, 10, 11, 12, 12, 12, 8, 7, 8, 9, 9, 10, 10, 11, 11,
    11, 12, 12, 13, 9, 8, 9, 9, 10, 10, 11, 11, 11, 12, 12, 12,
    13, 10, 9, 9, 10, 11, 11, 11, 12, 11, 12, 12, 13, 13, 11, 9,
    10, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 11, 10, 10, 11, 11,
    12, 12, 13, 13, 13, 13, 13, 13, 11, 10, 10, 11, 11, 11, 12, 12,
    13, 13, 14, 13, 14, 11, 10, 11, 11, 12, 12, 12, 12, 13, 13, 14,
    14, 14, 12, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 14, 15, 12,
    11, 12, 12, 12, 13, 13, 13, 13, 14, 14, 15, 15, 13, 12, 12, 12,
    13, 13, 13, 13, 14, 14, 14, 14, 15,
};
constexpr uint16_t kCodes10[169] = {
    0x0022, 0x0008, 0x001d, 0x0026, 0x005f, 0x00d3, 0x01cf, 0x03d0,
    0x03d7, 0x03ed, 0x07f0, 0x07f6, 0x0ffd, 0x0007, 0x0000, 0x0001,
    0x0009, 0x0020, 0x0054, 0x0060, 0x00d5, 0x00dc, 0x01d4, 0x03cd,
    0x03de, 0x07e7, 0x001c, 0x0002, 0x0006, 0x000c, 0x001e, 0x0028,
    0x005b, 0x00cd, 0x00d9, 0x01ce, 0x01dc, 0x03d9, 0x03f1, 0x0025,
    0x000b, 0x000a, 0x000d, 0x0024, 0x0057, 0x0061, 0x00cc, 0x00dd,
    0x01cc, 0x01de, 0x03d3, 0x03e7, 0x005d, 0x0021, 0x001f, 0x0023,
    0x0027, 0x0059, 0x0064, 0x00d8, 0x00df, 0x01d2, 0x01e2, 0x03dd,
    0x03ee, 0x00d1, 0x0055, 0x0029, 0x0056, 0x0058, 0x0062, 0x00ce,
    0x00e0, 0x00e2, 0x01da, 0x03d4, 0x03e3, 0x07eb, 0x01c9, 0x005e,
    0x005a, 0x005c, 0x0063, 0x00ca, 0x00da, 0x01c7, 0x01ca, 0x01e0,
    0x03db, 0x03e8, 0x07ec, 0x01e3, 0x00d2, 0x00cb, 0x00d0, 0x00d7,
    0x00db, 0x01c6, 0x01d5, 0x01d8, 0x03ca, 0x03da, 0x07ea, 0x07f1,
    0x01e1, 0x00d4, 0x00cf, 0x00d6, 0x00de, 0x00e1, 0x01d0, 0x01d6,
    0x03d1, 0x03d5, 0x03f2, 0x07ee, 0x07fb, 0x03e9, 0x01cd, 0x01c8,
    0x01cb, 0x01d1, 0x01d7, 0x01df, 0x03cf, 0x03e0, 0x03ef, 0x07e6,
    0x07f8, 0x0ffa, 0x03eb, 0x01dd, 0x01d3, 0x01d9, 0x01db, 0x03d2,
    0x03cc, 0x03dc, 0x03ea, 0x07ed, 0x07f3, 0x07f9, 0x0ff9, 0x07f2,
    0x03ce, 0x01e4, 0x03cb, 0x03d8, 0x03d6, 0x03e2, 0x03e5, 0x07e8,
    0x07f4, 0x07f5, 0x07f7, 0x0ffb, 0x07fa, 0x03ec, 0x03df, 0x03e1,
    0x03e4, 0x03e6, 0x03f0, 0x07e9, 0x07ef, 0x0ff8, 0x0ffe, 0x0ffc,
    0x0fff,
};
constexpr uint8_t kBits10[169] = {
    6, 5, 6, 6, 7, 8, 9, 10, 10, 10, 11, 11, 12, 5, 4, 4,
    5, 6, 7, 7, 8, 8, 9, 10, 10, 11, 6, 4, 5, 5, 6, 6,
    7, 8, 8, 9, 9, 10, 10, 6, 5, 5, 5, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 7, 6, 6, 6, 6, 7, 7, 8, 8, 9, 9, 10,
    10, 8, 7, 6, 7, 7, 7, 8, 8, 8, 9, 10, 10, 11, 9, 7,
    7, 7, 7, 8, 8, 9, 9, 9, 10, 10, 11, 9, 8, 8, 8, 8,
    8, 9, 9, 9, 10, 10, 11, 11, 9, 8, 8, 8, 8, 8, 9, 9,
    10, 10, 10, 11, 11, 10, 9, 9, 9, 9, 9, 9, 10, 10, 10, 11,
    11, 12, 10, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 12, 11,
    10, 9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 12, 11, 10, 10, 10,
    10, 10, 10, 11, 11, 12, 12, 12, 12,
};
constexpr uint16_t kCodes11[289] = {
    0x0000, 0x0006, 0x0019, 0x003d, 0x009c, 0x00c6, 0x01a7, 0x0390,
    0x03c2, 0x03df, 0x07e6, 0x07f3, 0x0ffb, 0x07ec, 0x0ffa, 0x0ffe,
    0x038e, 0x0005, 0x0001, 0x0008, 0x0014, 0x0037, 0x0042, 0x0092,
    0x00af, 0x0191, 0x01a5, 0x01b5, 0x039e, 0x03c0, 0x03a2, 0x03cd,
    0x07d6, 0x00ae, 0x0017, 0x0007, 0x0009, 0x0018, 0x0039, 0x0040,
    0x008e, 0x00a3, 0x00b8, 0x0199, 0x01ac, 0x01c1, 0x03b1, 0x0396,
    0x03be, 0x03ca, 0x009d, 0x003c, 0x0015, 0x0016, 0x001a, 0x003b,
    0x0044, 0x0091, 0x00a5, 0x00be, 0x0196, 0x01ae, 0x01b9, 0x03a1,
    0x0391, 0x03a5, 0x03d5, 0x0094, 0x009a, 0x0036, 0x0038, 0x003a,
    0x0041, 0x008c, 0x009b, 0x00b0, 0x00c3, 0x019e, 0x01ab, 0x01bc,
    0x039f, 0x038f, 0x03a9, 0x03cf, 0x0093, 0x00bf, 0x003e, 0x003f,
    0x0043, 0x0045, 0x009e, 0x00a7, 0x00b9, 0x0194, 0x01a2, 0x01ba,
    0x01c3, 0x03a6, 0x03a7, 0x03bb, 0x03d4, 0x009f, 0x01a0, 0x008f,
    0x008d, 0x0090, 0x0098, 0x00a6, 0x00b6, 0x00c4, 0x019f, 0x01af,
    0x01bf, 0x0399, 0x03bf, 0x03b4, 0x03c9, 0x03e7, 0x00a8, 0x01b6,
    0x00ab, 0x00a4, 0x00aa, 0x00b2, 0x00c2, 0x00c5, 0x0198, 0x01a4,
    0x01b8, 0x038c, 0x03a4, 0x03c4, 0x03c6, 0x03dd, 0x03e8, 0x00ad,
    0x03af, 0x0192, 0x00bd, 0x00bc, 0x018e, 0x0197, 0x019a, 0x01a3,
    0x01b1, 0x038d, 0x0398, 0x03b7, 0x03d3, 0x03d1, 0x03db, 0x07dd,
    0x00b4, 0x03de, 0x01a9, 0x019b, 0x019c, 0x01a1, 0x01aa, 0x01ad,
    0x01b3, 0x038b, 0x03b2, 0x03b8, 0x03ce, 0x03e1, 0x03e0, 0x07d2,
    0x07e5, 0x00b7, 0x07e3, 0x01bb, 0x01a8, 0x01a6, 0x01b0, 0x01b2,
    0x01b7, 0x039b, 0x039a, 0x03ba, 0x03b5, 0x03d6, 0x07d7, 0x03e4,
    0x07d8, 0x07ea, 0x00ba, 0x07e8, 0x03a0, 0x01bd, 0x01b4, 0x038a,
    0x01c4, 0x0392, 0x03aa, 0x03b0, 0x03bc, 0x03d7, 0x07d4, 0x07dc,
    0x07db, 0x07d5, 0x07f0, 0x00c1, 0x07fb, 0x03c8, 0x03a3, 0x0395,
    0x039d, 0x03ac, 0x03ae, 0x03c5, 0x03d8, 0x03e2, 0x03e6, 0x07e4,
    0x07e7, 0x07e0, 0x07e9, 0x07f7, 0x0190, 0x07f2, 0x0393, 0x01be,
    0x01c0, 0x0394, 0x0397, 0x03ad, 0x03c3, 0x03c1, 0x03d2, 0x07da,
    0x07d9, 0x07df, 0x07eb, 0x07f4, 0x07fa, 0x0195, 0x07f8, 0x03bd,
    0x039c, 0x03ab, 0x03a8, 0x03b3, 0x03b9, 0x03d0, 0x03e3, 0x03e5,
    0x07e2, 0x07de, 0x07ed, 0x07f1, 0x07f9, 0x07fc, 0x0193, 0x0ffd,
    0x03dc, 0x03b6, 0x03c7, 0x03cc, 0x03cb, 0x03d9, 0x03da, 0x07d3,
    0x07e1, 0x07ee, 0x07ef, 0x07f5, 0x07f6, 0x0ffc, 0x0fff, 0x019d,
    0x01c2, 0x00b5, 0x00a1, 0x0096, 0x0097, 0x0095, 0x0099, 0x00a0,
    0x00a2, 0x00ac, 0x00a9, 0x00b1, 0x00b3, 0x00bb, 0x00c0, 0x018f,
    0x0004,
};
constexpr uint8_t kBits11[289] = {
    4, 5, 6, 7, 8, 8, 9, 10, 10, 10, 11, 11, 12, 11, 12, 12,
    10, 5, 4, 5, 6, 7, 7, 8, 8, 9, 9, 9, 10, 10, 10, 10,
    11, 8, 6, 5, 5, 6, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10,
    10, 10, 8, 7, 6, 6, 6, 7, 7, 8, 8, 8, 9, 9, 9, 10,
    10, 10, 10, 8, 8, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9,
    10, 10, 10, 10, 8, 8, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9,
    9, 10, 10, 10, 10, 8, 9, 8, 8, 8, 8, 8, 8, 8, 9, 9,
    9, 10, 10, 10, 10, 10, 8, 9, 8, 8, 8, 8, 8, 8, 9, 9,
    9, 10, 10, 10, 10, 10, 10, 8, 10, 9, 8, 8, 9, 9, 9, 9,
    9, 10, 10, 10, 10, 10, 10, 11, 8, 10, 9, 9, 9, 9, 9, 9,
    9, 10, 10, 10, 10, 10, 10, 11, 11, 8, 11, 9, 9, 9, 9, 9,
    9, 10, 10, 10, 10, 10, 11, 10, 11, 11, 8, 11, 10, 9, 9, 10,
    9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 8, 11, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 9, 11, 10, 9,
    9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 9, 11, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 9, 12,
    10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 9,
    9, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 9,
    5,
};
constexpr uint16_t kSwb1024_96[42] = {
    0, 4, 8, 12, 16, 20, 24, 28,
    32, 36, 40, 44, 48, 52, 56, 64,
    72, 80, 88, 96, 108, 120, 132, 144,
    156, 172, 188, 212, 240, 276, 320, 384,
    448, 512, 576, 640, 704, 768, 832, 896,
    960, 1024,
};
constexpr uint16_t kSwb1024_64[48] = {
    0, 4, 8, 12, 16, 20, 24, 28,
    32, 36, 40, 44, 48, 52, 56, 64,
    72, 80, 88, 100, 112, 124, 140, 156,
    172, 192, 216, 240, 268, 304, 344, 384,
    424, 464, 504, 544, 584, 624, 664, 704,
    744, 784, 824, 864, 904, 944, 984, 1024,
};
constexpr uint16_t kSwb1024_48[50] = {
    0, 4, 8, 12, 16, 20, 24, 28,
    32, 36, 40, 48, 56, 64, 72, 80,
    88, 96, 108, 120, 132, 144, 160, 176,
    196, 216, 240, 264, 292, 320, 352, 384,
    416, 448, 480, 512, 544, 576, 608, 640,
    672, 704, 736, 768, 800, 832, 864, 896,
    928, 1024,
};
constexpr uint16_t kSwb1024_32[52] = {
    0, 4, 8, 12, 16, 20, 24, 28,
    32, 36, 40, 48, 56, 64, 72, 80,
    88, 96, 108, 120, 132, 144, 160, 176,
    196, 216, 240, 264, 292, 320, 352, 384,
    416, 448, 480, 512, 544, 576, 608, 640,
    672, 704, 736, 768, 800, 832, 864, 896,
    928, 960, 992, 1024,
};
constexpr uint16_t kSwb1024_24[48] = {
    0, 4, 8, 12, 16, 20, 24, 28,
    32, 36, 40, 44, 52, 60, 68, 76,
    84, 92, 100, 108, 116, 124, 136, 148,
    160, 172, 188, 204, 220, 240, 260, 284,
    308, 336, 364, 396, 432, 468, 508, 552,
    600, 652, 704, 768, 832, 896, 960, 1024,
};
constexpr uint16_t kSwb1024_16[44] = {
    0, 8, 16, 24, 32, 40, 48, 56,
    64, 72, 80, 88, 100, 112, 124, 136,
    148, 160, 172, 184, 196, 212, 228, 244,
    260, 280, 300, 320, 344, 368, 396, 424,
    456, 492, 532, 572, 616, 664, 716, 772,
    832, 896, 960, 1024,
};
constexpr uint16_t kSwb1024_8[41] = {
    0, 12, 24, 36, 48, 60, 72, 84,
    96, 108, 120, 132, 144, 156, 172, 188,
    204, 220, 236, 252, 268, 288, 308, 328,
    348, 372, 396, 420, 448, 476, 508, 544,
    580, 620, 664, 712, 764, 820, 880, 944,
    1024,
};
constexpr uint16_t kSwb128_96[13] = {
    0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64, 92, 128,
};
constexpr uint16_t kSwb128_48[15] = {
    0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80, 96, 112, 128,
};
constexpr uint16_t kSwb128_24[16] = {
    0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 64, 76, 92, 108, 128,
};
constexpr uint16_t kSwb128_16[16] = {
    0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 60, 72, 88, 108, 128,
};
constexpr uint16_t kSwb128_8[16] = {
    0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 60, 72, 88, 108, 128,
};
// clang-format on

constexpr int kSampleRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
constexpr uint8_t kNumSwb1024[13] = {41, 41, 47, 49, 49, 51, 47, 47, 43, 43, 43, 40, 40};
constexpr uint8_t kNumSwb128[13] = {12, 12, 12, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15};
constexpr uint8_t kTnsMaxBands1024[13] = {31, 31, 34, 40, 42, 51, 46, 46, 42, 42, 42, 39, 39};
constexpr uint8_t kTnsMaxBands128[13] = {9, 9, 10, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14};
const uint16_t *const kSwb1024[13] = {kSwb1024_96, kSwb1024_96, kSwb1024_64, kSwb1024_48, kSwb1024_48, kSwb1024_32, kSwb1024_24,
                                      kSwb1024_24, kSwb1024_16, kSwb1024_16, kSwb1024_16, kSwb1024_8,  kSwb1024_8};
const uint16_t *const kSwb128[13] = {kSwb128_96, kSwb128_96, kSwb128_96, kSwb128_48, kSwb128_48, kSwb128_48, kSwb128_24,
                                     kSwb128_24, kSwb128_16, kSwb128_16, kSwb128_16, kSwb128_8,  kSwb128_8};

// Band types (section codebooks) with a meaning beyond "Huffman codebook n".
constexpr int kZeroHcb = 0;
constexpr int kEscHcb = 11;
constexpr int kNoiseHcb = 13;
constexpr int kIntensityHcb2 = 14;  // out of phase
constexpr int kIntensityHcb = 15;   // in phase

constexpr int kOnlyLong = 0;
constexpr int kLongStart = 1;
constexpr int kEightShort = 2;
constexpr int kLongStop = 3;

// --- Huffman decoding ------------------------------------------------------------

// A two-level lookup table: the first kPrimaryBits of the stream index the
// primary table; a longer code's entry there points at a subtable indexed
// by the bits that follow.
class Vlc {
public:
    template <typename Code>
    void Build(const Code *codes, const uint8_t *bits, int count) {
        table_.assign(size_t{1} << kPrimaryBits, Entry{});
        std::vector<int> max_len(size_t{1} << kPrimaryBits, 0);
        for (int i = 0; i < count; ++i) {
            const int len = bits[i];
            const uint32_t code = static_cast<uint32_t>(codes[i]);
            if (len <= kPrimaryBits) {
                const uint32_t first = code << (kPrimaryBits - len);
                for (uint32_t j = 0; j < (1u << (kPrimaryBits - len)); ++j)
                    table_[first + j] = Entry{i, static_cast<int8_t>(len), 0};
            } else {
                const uint32_t prefix = code >> (len - kPrimaryBits);
                max_len[prefix] = std::max(max_len[prefix], len);
            }
        }
        for (size_t prefix = 0; prefix < max_len.size(); ++prefix) {
            if (max_len[prefix] == 0) continue;
            const int sub = max_len[prefix] - kPrimaryBits;
            table_[prefix] = Entry{static_cast<int32_t>(table_.size()), 0, static_cast<int8_t>(sub)};
            table_.resize(table_.size() + (size_t{1} << sub));
        }
        for (int i = 0; i < count; ++i) {
            const int len = bits[i];
            if (len <= kPrimaryBits) continue;
            const uint32_t code = static_cast<uint32_t>(codes[i]);
            const Entry head = table_[code >> (len - kPrimaryBits)];
            const int rest = len - kPrimaryBits;
            const uint32_t first = static_cast<uint32_t>(head.value) + ((code & ((1u << rest) - 1u)) << (head.sub_bits - rest));
            for (uint32_t j = 0; j < (1u << (head.sub_bits - rest)); ++j)
                table_[first + j] = Entry{i, static_cast<int8_t>(len), 0};
        }
    }
    // The next symbol, or -1 for bits that are no code.
    int Decode(BitReader &br) const;

private:
    static constexpr int kPrimaryBits = 9;
    struct Entry {
        int32_t value = -1;  // symbol, or a subtable's offset when len == 0
        int8_t len = 0;
        int8_t sub_bits = 0;
    };
    std::vector<Entry> table_;
};

}  // namespace

// MSB-first reader over one access unit. Reading past the end yields zero
// bits and leaves Overrun() true, which Decode reports as an error.
class BitReader {
public:
    BitReader(const uint8_t *data, size_t len) : data_(data), size_(len), bits_(len * 8) {}
    uint32_t Peek(int n) const {
        if (n == 0) return 0;
        const size_t byte = pos_ >> 3;
        uint64_t v = 0;
        if (byte + 8 <= size_) {
            for (int i = 0; i < 8; ++i) v = (v << 8) | data_[byte + static_cast<size_t>(i)];
        } else {
            for (size_t i = 0; i < 8; ++i) v = (v << 8) | (byte + i < size_ ? data_[byte + i] : 0u);
        }
        const int shift = 64 - static_cast<int>(pos_ & 7) - n;
        return static_cast<uint32_t>((v >> shift) & ((uint64_t{1} << n) - 1));
    }
    uint32_t Read(int n) {
        const uint32_t v = Peek(n);
        pos_ += static_cast<size_t>(n);
        return v;
    }
    bool ReadBit() { return Read(1) != 0; }
    void Skip(size_t n) { pos_ += n; }
    void ByteAlign() { pos_ = (pos_ + 7) & ~size_t{7}; }
    bool Overrun() const { return pos_ > bits_; }
    size_t Left() const { return pos_ >= bits_ ? 0 : bits_ - pos_; }

private:
    const uint8_t *data_;
    size_t size_;
    size_t bits_;
    size_t pos_ = 0;
};

namespace {

int Vlc::Decode(BitReader &br) const {
    const Entry &e = table_[br.Peek(kPrimaryBits)];
    if (e.len > 0) {
        br.Skip(static_cast<size_t>(e.len));
        return e.value;
    }
    if (e.sub_bits == 0) return -1;
    const uint32_t rest = br.Peek(kPrimaryBits + e.sub_bits) & ((1u << e.sub_bits) - 1u);
    const Entry &s = table_[static_cast<size_t>(e.value) + rest];
    if (s.len <= 0) return -1;
    br.Skip(static_cast<size_t>(s.len));
    return s.value;
}

// --- Filterbank ------------------------------------------------------------------

// In-place radix-2 complex FFT (forward, e^{-2 pi i nk/N}) of one fixed size.
class Fft {
public:
    explicit Fft(int n) : n_(n), twiddle_(static_cast<size_t>(n / 2)), rev_(static_cast<size_t>(n)) {
        for (int k = 0; k < n / 2; ++k)
            twiddle_[static_cast<size_t>(k)] = std::polar(1.0f, static_cast<float>(-2.0 * M_PI * k / n));
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[static_cast<size_t>(i)] = r;
        }
    }
    void Run(std::complex<float> *x) const {
        for (int i = 0; i < n_; ++i) {
            const int r = rev_[static_cast<size_t>(i)];
            if (r > i) std::swap(x[i], x[r]);
        }
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len / 2, step = n_ / len;
            for (int i = 0; i < n_; i += len) {
                for (int j = 0; j < half; ++j) {
                    const std::complex<float> t = x[i + j + half] * twiddle_[static_cast<size_t>(j * step)];
                    x[i + j + half] = x[i + j] - t;
                    x[i + j] += t;
                }
            }
        }
    }

private:
    int n_;
    std::vector<std::complex<float>> twiddle_;
    std::vector<int> rev_;
};

// IMDCT of window length N (2048 or 256) from N/2 coefficients, including
// the standard's 2/N scale: a DCT-IV of size M = N/2 through an M/2-point
// FFT, then unfolded into N samples by the DCT-IV's symmetries.
class Imdct {
public:
    explicit Imdct(int n) : n_(n), m_(n / 2), fft_(n / 4), pre_(static_cast<size_t>(n / 4)), buf_(static_cast<size_t>(n / 4)),
                            dct_(static_cast<size_t>(n / 2)) {
        for (int j = 0; j < m_ / 2; ++j)
            pre_[static_cast<size_t>(j)] = std::polar(1.0f, static_cast<float>(-M_PI * (j + 0.125) / m_));
    }
    void Run(const float *in, float *out) {
        const int q = m_ / 2;
        for (int j = 0; j < q; ++j)
            buf_[static_cast<size_t>(j)] =
                std::complex<float>(in[2 * j], in[m_ - 1 - 2 * j]) * pre_[static_cast<size_t>(j)];
        fft_.Run(buf_.data());
        for (int k = 0; k < q; ++k) {
            const std::complex<float> s = buf_[static_cast<size_t>(k)] * pre_[static_cast<size_t>(k)];
            dct_[static_cast<size_t>(2 * k)] = s.real();
            dct_[static_cast<size_t>(m_ - 1 - 2 * k)] = -s.imag();
        }
        // y[n] = c(n + M/2), c being the DCT-IV extended by c(2M-1-m) = -c(m)
        // and c(m+2M) = -c(m).
        const float scale = 2.0f / static_cast<float>(n_);
        const int half = m_ / 2;
        for (int k = 0; k < half; ++k) out[k] = dct_[static_cast<size_t>(k + half)] * scale;
        for (int k = half; k < half + m_; ++k) out[k] = -dct_[static_cast<size_t>(2 * m_ - 1 - (k + half))] * scale;
        for (int k = half + m_; k < n_; ++k) out[k] = -dct_[static_cast<size_t>(k + half - 2 * m_)] * scale;
    }

private:
    int n_, m_;
    Fft fft_;
    std::vector<std::complex<float>> pre_, buf_;
    std::vector<float> dct_;
};

// Zeroth-order modified Bessel function of the first kind, for the KBD window.
double BesselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

// The rising half (N/2 values) of a KBD window of length N.
std::vector<float> KbdRise(int n, double alpha) {
    const int half = n / 2;
    std::vector<double> w(static_cast<size_t>(half + 1));
    double total = 0.0;
    for (int k = 0; k <= half; ++k) {
        const double r = (k - n / 4.0) / (n / 4.0);
        w[static_cast<size_t>(k)] = BesselI0(M_PI * alpha * std::sqrt(std::max(0.0, 1.0 - r * r)));
        total += w[static_cast<size_t>(k)];
    }
    std::vector<float> out(static_cast<size_t>(half));
    double acc = 0.0;
    for (int k = 0; k < half; ++k) {
        acc += w[static_cast<size_t>(k)];
        out[static_cast<size_t>(k)] = static_cast<float>(std::sqrt(acc / total));
    }
    return out;
}

std::vector<float> SineRise(int n) {
    std::vector<float> out(static_cast<size_t>(n / 2));
    for (int k = 0; k < n / 2; ++k) out[static_cast<size_t>(k)] = static_cast<float>(std::sin(M_PI / n * (k + 0.5)));
    return out;
}

// Everything built once and shared by every decoder.
struct Tables {
    Vlc scalefactor;
    Vlc spectral[11];
    std::array<float, 8192> pow43{};  // |q|^(4/3)
    std::array<float, 256> gain{};    // 2^(0.25 * (sf - 100))
    std::vector<float> long_rise[2], short_rise[2];  // [shape]: 0 sine, 1 KBD
    Tables() {
        scalefactor.Build(kScalefactorCode, kScalefactorBits, 121);
        const uint16_t *codes[11] = {kCodes1, kCodes2, kCodes3, kCodes4, kCodes5, kCodes6, kCodes7, kCodes8, kCodes9, kCodes10, kCodes11};
        const uint8_t *bits[11] = {kBits1, kBits2, kBits3, kBits4, kBits5, kBits6, kBits7, kBits8, kBits9, kBits10, kBits11};
        const int sizes[11] = {81, 81, 81, 81, 81, 81, 64, 64, 169, 169, 289};
        for (int i = 0; i < 11; ++i) spectral[i].Build(codes[i], bits[i], sizes[i]);
        for (size_t i = 0; i < pow43.size(); ++i) pow43[i] = static_cast<float>(std::pow(static_cast<double>(i), 4.0 / 3.0));
        for (size_t i = 0; i < gain.size(); ++i) gain[i] = static_cast<float>(std::pow(2.0, 0.25 * (static_cast<double>(i) - 100.0)));
        long_rise[0] = SineRise(2048);
        long_rise[1] = KbdRise(2048, 4.0);
        short_rise[0] = SineRise(256);
        short_rise[1] = KbdRise(256, 6.0);
    }
};

const Tables &GetTables() {
    static const Tables tables;
    return tables;
}

// The filterbank's transforms hold scratch buffers, so each thread gets its own.
Imdct &LongImdct() {
    thread_local Imdct imdct(2048);
    return imdct;
}
Imdct &ShortImdct() {
    thread_local Imdct imdct(256);
    return imdct;
}

// --- Syntax ----------------------------------------------------------------------

bool Fail(std::string *error, const char *why) {
    if (error) *error = why;
    return false;
}

bool ReadIcsInfo(BitReader &br, int sr_index, Decoder::IcsInfo &ics, std::string *error) {
    br.Skip(1);  // ics_reserved_bit
    ics.window_sequence = static_cast<int>(br.Read(2));
    ics.window_shape = static_cast<int>(br.Read(1));
    ics.window_group_length.fill(0);
    if (ics.window_sequence == kEightShort) {
        ics.max_sfb = static_cast<int>(br.Read(4));
        const uint32_t grouping = br.Read(7);
        ics.num_windows = 8;
        ics.num_window_groups = 1;
        ics.window_group_length[0] = 1;
        for (int i = 0; i < 7; ++i) {
            if (grouping & (1u << (6 - i))) {
                ++ics.window_group_length[static_cast<size_t>(ics.num_window_groups - 1)];
            } else {
                ics.window_group_length[static_cast<size_t>(ics.num_window_groups)] = 1;
                ++ics.num_window_groups;
            }
        }
        ics.num_swb = kNumSwb128[sr_index];
        ics.swb_offset = kSwb128[sr_index];
    } else {
        ics.max_sfb = static_cast<int>(br.Read(6));
        ics.num_windows = 1;
        ics.num_window_groups = 1;
        ics.window_group_length[0] = 1;
        ics.num_swb = kNumSwb1024[sr_index];
        ics.swb_offset = kSwb1024[sr_index];
        if (br.ReadBit()) return Fail(error, "AAC: predictor data (AAC Main) in an LC stream");
    }
    if (ics.max_sfb > ics.num_swb) return Fail(error, "AAC: max_sfb beyond the scalefactor bands");
    return true;
}

// Escape sequence of codebook 11: N ones, a zero, then an (N+4)-bit word.
int ReadEscape(BitReader &br) {
    int n = 0;
    while (br.ReadBit()) {
        if (++n > 8) return -1;
    }
    return static_cast<int>((1u << (n + 4)) | br.Read(n + 4));
}

void SkipProgramConfig(BitReader &br) {
    br.Skip(4 + 2 + 4);  // element_instance_tag, object_type, sampling_frequency_index
    const int front = static_cast<int>(br.Read(4)), side = static_cast<int>(br.Read(4)), back = static_cast<int>(br.Read(4));
    const int lfe = static_cast<int>(br.Read(2)), assoc = static_cast<int>(br.Read(3)), cc = static_cast<int>(br.Read(4));
    if (br.ReadBit()) br.Skip(4);  // mono mixdown
    if (br.ReadBit()) br.Skip(4);  // stereo mixdown
    if (br.ReadBit()) br.Skip(3);  // matrix mixdown
    br.Skip(static_cast<size_t>((front + side + back) * 5 + lfe * 4 + assoc * 4 + cc * 5));
    br.ByteAlign();
    br.Skip(static_cast<size_t>(br.Read(8)) * 8);  // comment field
}

}  // namespace

// --- Decoder ---------------------------------------------------------------------

Decoder::Decoder() { GetTables(); }

bool Decoder::Configure(const uint8_t *asc, size_t len, std::string *error) {
    sample_rate_ = channels_ = 0;
    sample_rate_index_ = -1;
    BitReader br(asc, len);
    auto object_type = [&br]() {
        int t = static_cast<int>(br.Read(5));
        if (t == 31) t = 32 + static_cast<int>(br.Read(6));
        return t;
    };
    auto rate_index = [&br](int *explicit_rate) {
        const int index = static_cast<int>(br.Read(4));
        if (index == 15) *explicit_rate = static_cast<int>(br.Read(24));
        return index;
    };
    int aot = object_type();
    int explicit_rate = 0;
    const int index = rate_index(&explicit_rate);
    const int config = static_cast<int>(br.Read(4));
    if (aot == 5 || aot == 29) {
        // Explicit SBR (and PS): the extension's rate, then the core's object type.
        int ext_rate = 0;
        rate_index(&ext_rate);
        aot = object_type();
    }
    if (br.Overrun()) return Fail(error, "AAC: truncated AudioSpecificConfig");
    if (aot != 2) {
        if (error) *error = "AAC: unsupported audio object type " + std::to_string(aot) + " (only AAC-LC is)";
        return false;
    }
    if (br.ReadBit()) return Fail(error, "AAC: 960-sample frames are not supported");
    if (br.ReadBit()) br.Skip(14);  // dependsOnCoreCoder: coreCoderDelay
    br.Skip(1);                     // extensionFlag
    if (config != 1 && config != 2) {
        if (error) *error = "AAC: unsupported channel configuration " + std::to_string(config) + " (only mono and stereo are)";
        return false;
    }
    int sr_index = index;
    if (index == 15) {
        // An explicit rate: the nearest standard one picks the band tables.
        sr_index = 0;
        for (int i = 1; i < 13; ++i)
            if (std::abs(kSampleRates[i] - explicit_rate) < std::abs(kSampleRates[sr_index] - explicit_rate)) sr_index = i;
    } else if (index > 12) {
        return Fail(error, "AAC: invalid sampling frequency index");
    }
    sample_rate_index_ = sr_index;
    sample_rate_ = index == 15 ? explicit_rate : kSampleRates[index];
    channels_ = config;
    Reset();
    return true;
}

void Decoder::Reset() {
    for (Channel &c : ch_) {
        c.overlap.fill(0.0f);
        c.prev_window_shape = 0;
    }
}

bool Decoder::DecodeIcs(BitReader &br, Channel &ch, bool common_window, std::string *error) {
    const Tables &tab = GetTables();
    ch.global_gain = static_cast<int>(br.Read(8));
    if (!common_window && !ReadIcsInfo(br, sample_rate_index_, ch.ics, error)) return false;
    const IcsInfo &ics = ch.ics;
    const bool is_long = ics.window_sequence != kEightShort;

    // Section data: the codebook of every scalefactor band.
    const int sect_bits = is_long ? 5 : 3;
    const int sect_esc = (1 << sect_bits) - 1;
    for (int g = 0; g < ics.num_window_groups; ++g) {
        int k = 0;
        while (k < ics.max_sfb) {
            const int cb = static_cast<int>(br.Read(4));
            if (cb == 12) return Fail(error, "AAC: reserved section codebook");
            int len = 0, incr;
            while ((incr = static_cast<int>(br.Read(sect_bits))) == sect_esc) {
                len += sect_esc;
                if (br.Overrun()) return Fail(error, "AAC: truncated section data");
            }
            len += incr;
            if (k + len > ics.max_sfb) return Fail(error, "AAC: section beyond max_sfb");
            if (br.Overrun()) return Fail(error, "AAC: truncated section data");
            for (int i = 0; i < len; ++i) ch.band_type[static_cast<size_t>(g * 64 + k + i)] = static_cast<uint8_t>(cb);
            k += len;
        }
    }

    // Scalefactors, intensity positions and noise energies, each its own
    // differential chain.
    int sf = ch.global_gain, is_pos = 0, noise = ch.global_gain - 90;
    bool first_noise = true;
    for (int g = 0; g < ics.num_window_groups; ++g) {
        for (int b = 0; b < ics.max_sfb; ++b) {
            const size_t idx = static_cast<size_t>(g * 64 + b);
            const int cb = ch.band_type[idx];
            if (cb == kZeroHcb) {
                ch.sf[idx] = 0;
                continue;
            }
            if (cb == kNoiseHcb && first_noise) {
                first_noise = false;
                noise += static_cast<int>(br.Read(9)) - 256;
                ch.sf[idx] = noise;
                continue;
            }
            const int delta = tab.scalefactor.Decode(br);
            if (delta < 0) return Fail(error, "AAC: invalid scalefactor code");
            if (cb == kIntensityHcb || cb == kIntensityHcb2) {
                is_pos += delta - 60;
                ch.sf[idx] = is_pos;
            } else if (cb == kNoiseHcb) {
                noise += delta - 60;
                ch.sf[idx] = noise;
            } else {
                sf += delta - 60;
                if (sf < 0 || sf > 255) return Fail(error, "AAC: scalefactor out of range");
                ch.sf[idx] = sf;
            }
        }
    }

    // Pulse data (long windows only): applied to the quantized values below.
    int pulse_count = 0, pulse_start = 0;
    std::array<int, 4> pulse_offset{}, pulse_amp{};
    if (br.ReadBit()) {
        if (!is_long) return Fail(error, "AAC: pulse data in a short-window frame");
        pulse_count = static_cast<int>(br.Read(2)) + 1;
        pulse_start = static_cast<int>(br.Read(6));
        if (pulse_start >= ics.num_swb) return Fail(error, "AAC: pulse start beyond the bands");
        for (int i = 0; i < pulse_count; ++i) {
            pulse_offset[static_cast<size_t>(i)] = static_cast<int>(br.Read(5));
            pulse_amp[static_cast<size_t>(i)] = static_cast<int>(br.Read(4));
        }
    }

    // TNS: per window, up to 3 (long) or 1 (short) filters, kept as
    // direct-form LPC coefficients.
    ch.tns_present = br.ReadBit();
    if (ch.tns_present) {
        for (int w = 0; w < ics.num_windows; ++w) {
            const int n_filt = static_cast<int>(br.Read(is_long ? 2 : 1));
            ch.tns_n_filt[static_cast<size_t>(w)] = n_filt;
            if (n_filt == 0) continue;
            const int coef_res = static_cast<int>(br.Read(1));
            for (int f = 0; f < n_filt; ++f) {
                Channel::TnsFilter &flt = ch.tns[static_cast<size_t>(w)][static_cast<size_t>(f)];
                flt.length = static_cast<int>(br.Read(is_long ? 6 : 4));
                flt.order = static_cast<int>(br.Read(is_long ? 5 : 3));
                if (flt.order > (is_long ? 12 : 7)) return Fail(error, "AAC: TNS order beyond the LC limit");
                if (flt.order == 0) continue;
                flt.direction = br.ReadBit();
                const int compress = static_cast<int>(br.Read(1));
                const int res_bits = coef_res + 3, coef_bits = res_bits - compress;
                const double iqfac = ((1 << (res_bits - 1)) - 0.5) / (M_PI / 2.0);
                const double iqfac_m = ((1 << (res_bits - 1)) + 0.5) / (M_PI / 2.0);
                std::array<float, 32> refl{};
                for (int i = 0; i < flt.order; ++i) {
                    int v = static_cast<int>(br.Read(coef_bits));
                    if (v & (1 << (coef_bits - 1))) v -= 1 << coef_bits;
                    refl[static_cast<size_t>(i)] = static_cast<float>(std::sin(v / (v >= 0 ? iqfac : iqfac_m)));
                }
                // Reflection coefficients to LPC (a[0] = 1 implied).
                std::array<float, 32> a{}, b{};
                for (int m = 1; m <= flt.order; ++m) {
                    for (int i = 1; i < m; ++i)
                        b[static_cast<size_t>(i)] = a[static_cast<size_t>(i)] + refl[static_cast<size_t>(m - 1)] * a[static_cast<size_t>(m - i)];
                    for (int i = 1; i < m; ++i) a[static_cast<size_t>(i)] = b[static_cast<size_t>(i)];
                    a[static_cast<size_t>(m)] = refl[static_cast<size_t>(m - 1)];
                }
                flt.lpc = a;
            }
        }
    }
    if (br.ReadBit()) return Fail(error, "AAC: gain control data (AAC SSR) in an LC stream");

    // Spectral data, window-major within each band of each group.
    std::array<int, 1024> q{};
    int win0 = 0;
    for (int g = 0; g < ics.num_window_groups; ++g) {
        const int glen = ics.window_group_length[static_cast<size_t>(g)];
        for (int b = 0; b < ics.max_sfb; ++b) {
            const int cb = ch.band_type[static_cast<size_t>(g * 64 + b)];
            if (cb == kZeroHcb || cb >= kNoiseHcb) continue;
            const Vlc &vlc = tab.spectral[cb - 1];
            const bool unsigned_cb = cb == 3 || cb == 4 || cb >= 7;
            const int dim = cb <= 4 ? 4 : 2;
            const int lav_mod = cb <= 2 ? 3 : cb <= 4 ? 3 : cb <= 6 ? 9 : cb <= 8 ? 8 : cb <= 10 ? 13 : 17;
            const int start = ics.swb_offset[b], end = ics.swb_offset[b + 1];
            for (int w = 0; w < glen; ++w) {
                int *out = q.data() + (win0 + w) * 128;
                for (int k = start; k < end; k += dim) {
                    int sym = vlc.Decode(br);
                    if (sym < 0) return Fail(error, "AAC: invalid spectral code");
                    int v[4];
                    for (int i = dim - 1; i >= 0; --i) {
                        v[i] = sym % lav_mod;
                        sym /= lav_mod;
                    }
                    if (!unsigned_cb) {
                        const int off = lav_mod / 2;  // signed books center their values
                        for (int i = 0; i < dim; ++i) v[i] -= off;
                    } else {
                        for (int i = 0; i < dim; ++i)
                            if (v[i] != 0 && br.ReadBit()) v[i] = -v[i];
                        if (cb == kEscHcb) {
                            for (int i = 0; i < dim; ++i) {
                                if (v[i] == 16 || v[i] == -16) {
                                    const int e = ReadEscape(br);
                                    if (e < 0) return Fail(error, "AAC: invalid escape sequence");
                                    v[i] = v[i] < 0 ? -e : e;
                                }
                            }
                        }
                    }
                    for (int i = 0; i < dim; ++i) out[k + i] = v[i];
                }
            }
        }
        win0 += glen;
    }
    if (br.Overrun()) return Fail(error, "AAC: truncated channel stream");
    if (pulse_count > 0) {
        int k = ics.swb_offset[pulse_start];
        for (int i = 0; i < pulse_count; ++i) {
            k += pulse_offset[static_cast<size_t>(i)];
            if (k >= 1024) return Fail(error, "AAC: pulse beyond the spectrum");
            int &v = q[static_cast<size_t>(k)];
            v += v > 0 ? pulse_amp[static_cast<size_t>(i)] : -pulse_amp[static_cast<size_t>(i)];
        }
    }

    // Inverse quantization and scaling; noise bands filled with scaled,
    // energy-normalized noise; intensity bands left at zero for the pair.
    ch.spec.fill(0.0f);
    win0 = 0;
    for (int g = 0; g < ics.num_window_groups; ++g) {
        const int glen = ics.window_group_length[static_cast<size_t>(g)];
        for (int b = 0; b < ics.max_sfb; ++b) {
            const size_t idx = static_cast<size_t>(g * 64 + b);
            const int cb = ch.band_type[idx];
            const int start = ics.swb_offset[b], end = ics.swb_offset[b + 1];
            if (cb == kZeroHcb || cb == kIntensityHcb || cb == kIntensityHcb2) continue;
            for (int w = 0; w < glen; ++w) {
                const int base = (win0 + w) * 128;
                float *out = ch.spec.data() + base;
                if (cb == kNoiseHcb) {
                    float energy = 0.0f;
                    for (int k = start; k < end; ++k) {
                        noise_seed_ = noise_seed_ * 1664525u + 1013904223u;
                        out[k] = static_cast<float>(static_cast<int32_t>(noise_seed_));
                        energy += out[k] * out[k];
                    }
                    const float scale = static_cast<float>(std::pow(2.0, 0.25 * ch.sf[idx])) / std::sqrt(std::max(energy, 1e-30f));
                    for (int k = start; k < end; ++k) out[k] *= scale;
                    continue;
                }
                const float gain = tab.gain[static_cast<size_t>(ch.sf[idx])];
                const int *in = q.data() + base;
                for (int k = start; k < end; ++k) {
                    const int v = in[k];
                    if (v == 0) continue;
                    const int mag = std::min(std::abs(v), 8191);
                    out[k] = (v < 0 ? -tab.pow43[static_cast<size_t>(mag)] : tab.pow43[static_cast<size_t>(mag)]) * gain;
                }
            }
        }
        win0 += glen;
    }
    return true;
}

void Decoder::Filterbank(Channel &ch, float *time_out) {
    const Tables &tab = GetTables();
    const IcsInfo &ics = ch.ics;
    const bool is_long = ics.window_sequence != kEightShort;

    // TNS, an all-pole filter run over each filter's span of coefficients.
    if (ch.tns_present) {
        const int tns_max = is_long ? kTnsMaxBands1024[sample_rate_index_] : kTnsMaxBands128[sample_rate_index_];
        for (int w = 0; w < ics.num_windows; ++w) {
            float *x = ch.spec.data() + w * 128;
            int bottom = ics.num_swb;
            for (int f = 0; f < ch.tns_n_filt[static_cast<size_t>(w)]; ++f) {
                const Channel::TnsFilter &flt = ch.tns[static_cast<size_t>(w)][static_cast<size_t>(f)];
                const int top = bottom;
                bottom = std::max(top - flt.length, 0);
                if (flt.order == 0) continue;
                const int start = ics.swb_offset[std::min({bottom, tns_max, ics.max_sfb})];
                const int end = ics.swb_offset[std::min({top, tns_max, ics.max_sfb})];
                const int size = end - start;
                if (size <= 0) continue;
                int pos = flt.direction ? end - 1 : start;
                const int inc = flt.direction ? -1 : 1;
                std::array<float, 32> state{};
                for (int n = 0; n < size; ++n, pos += inc) {
                    float y = x[pos];
                    for (int i = 1; i <= flt.order; ++i) y -= flt.lpc[static_cast<size_t>(i)] * state[static_cast<size_t>(i - 1)];
                    for (int i = flt.order - 1; i > 0; --i) state[static_cast<size_t>(i)] = state[static_cast<size_t>(i - 1)];
                    state[0] = y;
                    x[pos] = y;
                }
            }
        }
    }

    // IMDCT, windowing, overlap-add.
    scratch_.assign(2048 + 256, 0.0f);
    float *buf = scratch_.data();
    float *tmp = scratch_.data() + 2048;
    const float *long_prev = tab.long_rise[ch.prev_window_shape].data();
    const float *long_cur = tab.long_rise[ics.window_shape].data();
    const float *short_prev = tab.short_rise[ch.prev_window_shape].data();
    const float *short_cur = tab.short_rise[ics.window_shape].data();
    switch (ics.window_sequence) {
        case kOnlyLong:
            LongImdct().Run(ch.spec.data(), buf);
            for (int n = 0; n < 1024; ++n) {
                buf[n] *= long_prev[n];
                buf[1024 + n] *= long_cur[1023 - n];
            }
            break;
        case kLongStart:
            LongImdct().Run(ch.spec.data(), buf);
            for (int n = 0; n < 1024; ++n) buf[n] *= long_prev[n];
            for (int n = 0; n < 128; ++n) buf[1472 + n] *= short_cur[127 - n];
            for (int n = 1600; n < 2048; ++n) buf[n] = 0.0f;
            break;
        case kLongStop:
            LongImdct().Run(ch.spec.data(), buf);
            for (int n = 0; n < 448; ++n) buf[n] = 0.0f;
            for (int n = 0; n < 128; ++n) buf[448 + n] *= short_prev[n];
            for (int n = 0; n < 1024; ++n) buf[1024 + n] *= long_cur[1023 - n];
            break;
        default:  // eight short
            for (int w = 0; w < 8; ++w) {
                ShortImdct().Run(ch.spec.data() + w * 128, tmp);
                const float *rise = w == 0 ? short_prev : short_cur;
                float *o = buf + 448 + w * 128;
                for (int n = 0; n < 128; ++n) {
                    o[n] += tmp[n] * rise[n];
                    o[128 + n] += tmp[128 + n] * short_cur[127 - n];
                }
            }
            break;
    }
    for (int n = 0; n < 1024; ++n) time_out[n] = ch.overlap[static_cast<size_t>(n)] + buf[n];
    std::memcpy(ch.overlap.data(), buf + 1024, 1024 * sizeof(float));
    ch.prev_window_shape = ics.window_shape;
}

bool Decoder::Decode(const uint8_t *data, size_t len, std::vector<int16_t> *out, std::string *error) {
    if (sample_rate_index_ < 0) return Fail(error, "AAC: decoder not configured");
    BitReader br(data, len);
    int decoded = 0;  // channels with a spectrum this frame
    std::array<uint8_t, 8 * 64> ms_used{};
    int ms_mask_present = 0;
    bool have_pair = false;
    Channel lfe;  // parsed and dropped
    for (;;) {
        if (br.Left() < 3) return Fail(error, "AAC: access unit ends before its END element");
        const int id = static_cast<int>(br.Read(3));
        if (id == 7) break;  // END
        switch (id) {
            case 0:    // SCE
            case 3: {  // LFE
                br.Skip(4);
                const bool use = id == 0 && decoded < channels_;
                Channel &c = use ? ch_[static_cast<size_t>(decoded)] : lfe;
                if (!DecodeIcs(br, c, false, error)) return false;
                if (use) ++decoded;
                break;
            }
            case 1: {  // CPE
                br.Skip(4);
                if (channels_ != 2 || decoded != 0) return Fail(error, "AAC: unexpected channel pair element");
                Channel &l = ch_[0], &r = ch_[1];
                const bool common = br.ReadBit();
                ms_mask_present = 0;
                if (common) {
                    if (!ReadIcsInfo(br, sample_rate_index_, l.ics, error)) return false;
                    r.ics = l.ics;
                    ms_mask_present = static_cast<int>(br.Read(2));
                    if (ms_mask_present == 3) return Fail(error, "AAC: reserved ms_mask_present");
                    if (ms_mask_present == 1) {
                        for (int g = 0; g < l.ics.num_window_groups; ++g)
                            for (int b = 0; b < l.ics.max_sfb; ++b) ms_used[static_cast<size_t>(g * 64 + b)] = br.ReadBit() ? 1 : 0;
                    } else if (ms_mask_present == 2) {
                        ms_used.fill(1);
                    }
                }
                if (!DecodeIcs(br, l, common, error) || !DecodeIcs(br, r, common, error)) return false;
                decoded = 2;
                have_pair = true;
                break;
            }
            case 2:
                return Fail(error, "AAC: coupling channel elements are not supported");
            case 4: {  // DSE
                br.Skip(4);
                const bool align = br.ReadBit();
                size_t count = br.Read(8);
                if (count == 255) count += br.Read(8);
                if (align) br.ByteAlign();
                br.Skip(count * 8);
                break;
            }
            case 5:
                SkipProgramConfig(br);
                break;
            case 6: {  // FIL (an SBR payload among others: skipped)
                size_t count = br.Read(4);
                if (count == 15) count += br.Read(8) - 1;
                br.Skip(count * 8);
                break;
            }
            default:
                break;
        }
        if (br.Overrun()) return Fail(error, "AAC: truncated access unit");
    }
    if (decoded == 0) return Fail(error, "AAC: access unit holds no audio channel");

    if (have_pair) {
        Channel &l = ch_[0], &r = ch_[1];
        const IcsInfo &ics = l.ics;
        int win0 = 0;
        for (int g = 0; g < ics.num_window_groups; ++g) {
            const int glen = ics.window_group_length[static_cast<size_t>(g)];
            for (int b = 0; b < ics.max_sfb; ++b) {
                const size_t idx = static_cast<size_t>(g * 64 + b);
                const int lt = l.band_type[idx], rt = r.band_type[idx];
                const int start = ics.swb_offset[b], end = ics.swb_offset[b + 1];
                const bool ms = ms_mask_present != 0 && ms_used[idx];
                for (int w = 0; w < glen; ++w) {
                    float *a = l.spec.data() + (win0 + w) * 128, *c = r.spec.data() + (win0 + w) * 128;
                    if (ms && lt < kNoiseHcb && rt < kNoiseHcb) {
                        for (int k = start; k < end; ++k) {
                            const float m = a[k], s = c[k];
                            a[k] = m + s;
                            c[k] = m - s;
                        }
                    } else if (ms && lt == kNoiseHcb && rt == kNoiseHcb) {
                        // Correlated noise: the right channel takes the left's vector.
                        const float ratio = static_cast<float>(std::pow(2.0, 0.25 * (r.sf[idx] - l.sf[idx])));
                        for (int k = start; k < end; ++k) c[k] = a[k] * ratio;
                    }
                    if (rt == kIntensityHcb || rt == kIntensityHcb2) {
                        float scale = (rt == kIntensityHcb ? 1.0f : -1.0f) * static_cast<float>(std::pow(0.5, 0.25 * r.sf[idx]));
                        if (ms) scale = -scale;
                        for (int k = start; k < end; ++k) c[k] = a[k] * scale;
                    }
                }
            }
            win0 += glen;
        }
    }

    // Filterbank and interleave; a lone SCE in a stereo stream plays on both sides.
    float time[2][1024];
    for (int c = 0; c < decoded; ++c) Filterbank(ch_[static_cast<size_t>(c)], time[c]);
    if (decoded < channels_) std::memcpy(time[1], time[0], sizeof time[0]);
    const size_t base = out->size();
    out->resize(base + 1024 * static_cast<size_t>(channels_));
    int16_t *o = out->data() + base;
    for (int n = 0; n < 1024; ++n) {
        for (int c = 0; c < channels_; ++c) {
            const float v = std::nearbyint(time[c][n]);
            *o++ = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
        }
    }
    return true;
}

bool CheckTables(std::string *error) {
    struct Book {
        const char *name;
        std::vector<uint32_t> codes;
        const uint8_t *bits;
        int count;
    };
    std::vector<Book> books;
    books.push_back({"scalefactor", std::vector<uint32_t>(kScalefactorCode, kScalefactorCode + 121), kScalefactorBits, 121});
    const uint16_t *codes[11] = {kCodes1, kCodes2, kCodes3, kCodes4, kCodes5, kCodes6, kCodes7, kCodes8, kCodes9, kCodes10, kCodes11};
    const uint8_t *bits[11] = {kBits1, kBits2, kBits3, kBits4, kBits5, kBits6, kBits7, kBits8, kBits9, kBits10, kBits11};
    const int sizes[11] = {81, 81, 81, 81, 81, 81, 64, 64, 169, 169, 289};
    static const char *const names[11] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"};
    for (int i = 0; i < 11; ++i)
        books.push_back({names[i], std::vector<uint32_t>(codes[i], codes[i] + sizes[i]), bits[i], sizes[i]});
    for (const Book &book : books) {
        // Complete: the Kraft sum is exactly 1 (in units of 2^-32).
        uint64_t kraft = 0;
        for (int i = 0; i < book.count; ++i) {
            if (book.bits[i] == 0 || book.bits[i] > 32 || (book.bits[i] < 32 && book.codes[static_cast<size_t>(i)] >> book.bits[i]) != 0)
                return Fail(error, "AAC tables: a code wider than its length");
            kraft += uint64_t{1} << (32 - book.bits[i]);
        }
        if (kraft != (uint64_t{1} << 32)) {
            if (error) *error = std::string("AAC tables: codebook ") + book.name + " is not complete";
            return false;
        }
        // Prefix-free: no code is the start of another.
        for (int i = 0; i < book.count; ++i) {
            for (int j = 0; j < book.count; ++j) {
                if (i == j || book.bits[j] < book.bits[i]) continue;
                if ((book.codes[static_cast<size_t>(j)] >> (book.bits[j] - book.bits[i])) == book.codes[static_cast<size_t>(i)]) {
                    if (error) *error = std::string("AAC tables: codebook ") + book.name + " is not prefix-free";
                    return false;
                }
            }
        }
    }
    for (int i = 0; i < 13; ++i) {
        if (kSwb1024[i][kNumSwb1024[i]] != 1024 || kSwb128[i][kNumSwb128[i]] != 128)
            return Fail(error, "AAC tables: a scalefactor band table does not end at its window length");
    }
    return true;
}

}  // namespace aac
