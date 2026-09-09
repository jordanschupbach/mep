#include "png_codec.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "deflate.h"

namespace png {

namespace {

constexpr unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

uint32_t ReadU32BE(const unsigned char *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
void WriteU32BE(std::string &s, uint32_t v) {
    s.push_back(static_cast<char>((v >> 24) & 0xFF));
    s.push_back(static_cast<char>((v >> 16) & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
}

struct Chunk {
    std::string type;
    const unsigned char *data;
    uint32_t length;
};

// -- Decode -----------------------------------------------------------------

int PaethPredictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// Number of samples (channel values) per pixel, for each PNG color type.
int ChannelsForColorType(int color_type, std::string *error) {
    switch (color_type) {
        case 0:
            return 1;  // grayscale
        case 2:
            return 3;  // truecolor
        case 3:
            return 1;  // indexed
        case 4:
            return 2;  // grayscale + alpha
        case 6:
            return 4;  // truecolor + alpha
        default:
            if (error) *error = "unsupported PNG color type";
            return -1;
    }
}

// Reverses the per-scanline filtering (PNG spec section 6), producing
// the unfiltered "raw" byte stream (still bit-packed per bit_depth, not
// yet expanded to samples).
bool Unfilter(const std::string &inflated, int width, int height, int channels, int bit_depth,
              std::vector<unsigned char> &raw, std::string *error) {
    int bpp = std::max(1, (channels * bit_depth) / 8);
    size_t row_bits = static_cast<size_t>(width) * static_cast<size_t>(channels) * static_cast<size_t>(bit_depth);
    size_t row_bytes = (row_bits + 7) / 8;
    size_t expected = (row_bytes + 1) * static_cast<size_t>(height);
    if (inflated.size() < expected) {
        if (error) *error = "PNG: truncated pixel data";
        return false;
    }
    raw.assign(row_bytes * static_cast<size_t>(height), 0);
    const unsigned char *src = reinterpret_cast<const unsigned char *>(inflated.data());
    std::vector<unsigned char> prior(row_bytes, 0);
    for (int y = 0; y < height; y++) {
        unsigned char filter_type = src[0];
        const unsigned char *filt = src + 1;
        unsigned char *out = raw.data() + static_cast<size_t>(y) * row_bytes;
        for (size_t x = 0; x < row_bytes; x++) {
            int a = x >= static_cast<size_t>(bpp) ? out[x - static_cast<size_t>(bpp)] : 0;
            int b = prior[x];
            int c = x >= static_cast<size_t>(bpp) ? prior[x - static_cast<size_t>(bpp)] : 0;
            int raw_val;
            switch (filter_type) {
                case 0:
                    raw_val = filt[x];
                    break;
                case 1:
                    raw_val = filt[x] + a;
                    break;
                case 2:
                    raw_val = filt[x] + b;
                    break;
                case 3:
                    raw_val = filt[x] + (a + b) / 2;
                    break;
                case 4:
                    raw_val = filt[x] + PaethPredictor(a, b, c);
                    break;
                default:
                    if (error) *error = "PNG: invalid scanline filter type";
                    return false;
            }
            out[x] = static_cast<unsigned char>(raw_val & 0xFF);
        }
        std::memcpy(prior.data(), out, row_bytes);
        src += 1 + row_bytes;
    }
    return true;
}

// Reads `bit_depth`-wide sample `index` (0-based, MSB-first packing
// within each byte, per PNG spec 7.2) out of one scanline's raw bytes.
unsigned Sample(const unsigned char *row, int bit_depth, size_t index) {
    if (bit_depth == 8) return row[index];
    if (bit_depth == 16) return row[index * 2];  // high byte only -- matches stb_image's own 16-bit-per-channel truncation to 8
    size_t bit_off = index * static_cast<size_t>(bit_depth);
    size_t byte_off = bit_off / 8;
    int shift = 8 - bit_depth - static_cast<int>(bit_off % 8);
    unsigned mask = (1u << bit_depth) - 1u;
    return (row[byte_off] >> shift) & mask;
}

// Scales a `bit_depth`-wide sample up to a full 0-255 byte (exact
// bit-replication scaling, e.g. 1-bit 0/1 -> 0/255, matching PNG's own
// spec-recommended approach for sub-8-bit grayscale).
unsigned char Scale(unsigned val, int bit_depth) {
    if (bit_depth >= 8) return static_cast<unsigned char>(val);
    unsigned max_val = (1u << bit_depth) - 1u;
    return static_cast<unsigned char>((val * 255u) / max_val);
}

}  // namespace

unsigned char *Decode(const unsigned char *data, size_t len, int *width, int *height, std::string *out_error) {
    auto fail = [&](const char *msg) -> unsigned char * {
        if (out_error) *out_error = msg;
        return nullptr;
    };
    if (len < 8 || std::memcmp(data, kSignature, 8) != 0) return fail("not a PNG file");

    size_t pos = 8;
    bool have_ihdr = false;
    int w = 0, h = 0, bit_depth = 0, color_type = 0, interlace = 0;
    std::vector<unsigned char> palette;   // RGB triples
    std::vector<unsigned char> trns;      // per-palette-entry alpha (indexed) or a single transparent value (gray/truecolor)
    bool have_trns = false;
    std::string idat;

    while (pos + 8 <= len) {
        uint32_t clen = ReadU32BE(data + pos);
        if (pos + 8 + clen + 4 > len) return fail("PNG: truncated chunk");
        std::string ctype(reinterpret_cast<const char *>(data + pos + 4), 4);
        const unsigned char *cdata = data + pos + 8;

        if (ctype == "IHDR") {
            if (clen < 13) return fail("PNG: malformed IHDR");
            w = static_cast<int>(ReadU32BE(cdata));
            h = static_cast<int>(ReadU32BE(cdata + 4));
            bit_depth = cdata[8];
            color_type = cdata[9];
            interlace = cdata[12];
            have_ihdr = true;
            if (w <= 0 || h <= 0) return fail("PNG: invalid dimensions");
            if (interlace != 0) return fail("PNG: interlaced (Adam7) images aren't supported");
        } else if (ctype == "PLTE") {
            palette.assign(cdata, cdata + clen);
        } else if (ctype == "tRNS") {
            trns.assign(cdata, cdata + clen);
            have_trns = true;
        } else if (ctype == "IDAT") {
            idat.append(reinterpret_cast<const char *>(cdata), clen);
        } else if (ctype == "IEND") {
            pos += 8 + clen + 4;
            break;
        }
        pos += 8 + clen + 4;
    }
    if (!have_ihdr) return fail("PNG: missing IHDR");
    if (idat.empty()) return fail("PNG: missing IDAT");

    std::string error;
    int channels = ChannelsForColorType(color_type, &error);
    if (channels < 0) return fail(error.c_str());
    if (color_type == 3 && palette.empty()) return fail("PNG: indexed color image with no PLTE chunk");

    std::string inflated;
    if (!deflate::InflateZlib(reinterpret_cast<const unsigned char *>(idat.data()), idat.size(), inflated)) {
        return fail("PNG: failed to decompress IDAT (corrupt or unsupported zlib stream)");
    }

    std::vector<unsigned char> raw;
    if (!Unfilter(inflated, w, h, channels, bit_depth, raw, &error)) return fail(error.c_str());

    size_t row_bits = static_cast<size_t>(w) * static_cast<size_t>(channels) * static_cast<size_t>(bit_depth);
    size_t row_bytes = (row_bits + 7) / 8;

    auto *out = static_cast<unsigned char *>(std::malloc(static_cast<size_t>(w) * static_cast<size_t>(h) * 4));
    if (!out) return fail("PNG: out of memory");

    // tRNS's single-transparent-value form (color types 0/2 only): PNG
    // stores this as 16-bit sample(s) regardless of the image's own bit
    // depth (spec 11.3.2.1) -- take the low byte to compare against our
    // already-8-bit-scaled decoded samples.
    bool gray_trns_set = have_trns && color_type == 0 && trns.size() >= 2;
    unsigned gray_trns_val = gray_trns_set ? ((static_cast<unsigned>(trns[0]) << 8) | trns[1]) : 0;
    bool rgb_trns_set = have_trns && color_type == 2 && trns.size() >= 6;
    unsigned r_trns = rgb_trns_set ? ((static_cast<unsigned>(trns[0]) << 8) | trns[1]) : 0;
    unsigned g_trns = rgb_trns_set ? ((static_cast<unsigned>(trns[2]) << 8) | trns[3]) : 0;
    unsigned b_trns = rgb_trns_set ? ((static_cast<unsigned>(trns[4]) << 8) | trns[5]) : 0;

    for (int y = 0; y < h; y++) {
        const unsigned char *row = raw.data() + static_cast<size_t>(y) * row_bytes;
        unsigned char *dst = out + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        for (int x = 0; x < w; x++) {
            unsigned char r = 0, g = 0, b = 0, a = 255;
            switch (color_type) {
                case 0: {  // grayscale
                    unsigned raw_gray = Sample(row, bit_depth, static_cast<size_t>(x));
                    unsigned char gray = Scale(raw_gray, bit_depth);
                    r = g = b = gray;
                    unsigned full_val = bit_depth == 16 ? ((static_cast<unsigned>(row[static_cast<size_t>(x) * 2]) << 8) |
                                                            row[static_cast<size_t>(x) * 2 + 1])
                                                         : raw_gray;
                    if (gray_trns_set && full_val == gray_trns_val) a = 0;
                    break;
                }
                case 2: {  // truecolor
                    size_t base = static_cast<size_t>(x) * 3;
                    unsigned rv = Sample(row, bit_depth, base), gv = Sample(row, bit_depth, base + 1),
                             bv = Sample(row, bit_depth, base + 2);
                    r = Scale(rv, bit_depth);
                    g = Scale(gv, bit_depth);
                    b = Scale(bv, bit_depth);
                    if (rgb_trns_set) {
                        unsigned fr = bit_depth == 16 ? ((static_cast<unsigned>(row[base * 2]) << 8) | row[base * 2 + 1]) : rv;
                        unsigned fg = bit_depth == 16
                                          ? ((static_cast<unsigned>(row[(base + 1) * 2]) << 8) | row[(base + 1) * 2 + 1])
                                          : gv;
                        unsigned fb = bit_depth == 16
                                          ? ((static_cast<unsigned>(row[(base + 2) * 2]) << 8) | row[(base + 2) * 2 + 1])
                                          : bv;
                        if (fr == r_trns && fg == g_trns && fb == b_trns) a = 0;
                    }
                    break;
                }
                case 3: {  // indexed
                    unsigned idx = Sample(row, bit_depth, static_cast<size_t>(x));
                    if (idx * 3 + 2 < palette.size()) {
                        r = palette[idx * 3];
                        g = palette[idx * 3 + 1];
                        b = palette[idx * 3 + 2];
                    }
                    a = (have_trns && idx < trns.size()) ? trns[idx] : 255;
                    break;
                }
                case 4: {  // grayscale + alpha
                    size_t base = static_cast<size_t>(x) * 2;
                    r = g = b = Scale(Sample(row, bit_depth, base), bit_depth);
                    a = Scale(Sample(row, bit_depth, base + 1), bit_depth);
                    break;
                }
                case 6: {  // truecolor + alpha
                    size_t base = static_cast<size_t>(x) * 4;
                    r = Scale(Sample(row, bit_depth, base), bit_depth);
                    g = Scale(Sample(row, bit_depth, base + 1), bit_depth);
                    b = Scale(Sample(row, bit_depth, base + 2), bit_depth);
                    a = Scale(Sample(row, bit_depth, base + 3), bit_depth);
                    break;
                }
                default:
                    break;
            }
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    }

    *width = w;
    *height = h;
    return out;
}

// -- Encode -------------------------------------------------------------

namespace {

// PNG color type for a given component count (see png_codec.h's own
// comment on the 4 supported `comp` values) -- always 8 bits/channel,
// no palette, matching every real call site's plain RGBA8 buffers.
int ColorTypeForComp(int comp) {
    switch (comp) {
        case 1:
            return 0;
        case 2:
            return 4;
        case 3:
            return 2;
        case 4:
            return 6;
        default:
            return -1;
    }
}

void WriteChunk(std::string &out, const char *type, const std::string &data) {
    WriteU32BE(out, static_cast<uint32_t>(data.size()));
    std::string type_and_data = std::string(type, 4) + data;
    out += type_and_data;
    uint32_t crc = deflate::Crc32(0, reinterpret_cast<const unsigned char *>(type_and_data.data()), type_and_data.size());
    WriteU32BE(out, crc);
}

// Picks, per scanline, whichever of PNG's 5 filter types minimizes the
// sum of absolute (signed) filtered-byte values -- the standard
// "minimum sum of absolute differences" heuristic real encoders
// (including libpng's default) use; simple, and meaningfully better
// than always emitting "None" for what the DEFLATE stage that follows
// can then compress (see STB_IMAGE_REMOVAL_PLAN.md's Phase 2 notes).
void FilterScanline(const unsigned char *row, const unsigned char *prior, size_t row_bytes, int bpp,
                     std::vector<unsigned char> &best) {
    std::vector<unsigned char> candidate(row_bytes);
    long best_score = -1;
    for (int ftype = 0; ftype < 5; ftype++) {
        long score = 0;
        for (size_t x = 0; x < row_bytes; x++) {
            int a = x >= static_cast<size_t>(bpp) ? row[x - static_cast<size_t>(bpp)] : 0;
            int b = prior ? prior[x] : 0;
            int c = (prior && x >= static_cast<size_t>(bpp)) ? prior[x - static_cast<size_t>(bpp)] : 0;
            int v;
            switch (ftype) {
                case 0:
                    v = row[x];
                    break;
                case 1:
                    v = row[x] - a;
                    break;
                case 2:
                    v = row[x] - b;
                    break;
                case 3:
                    v = row[x] - (a + b) / 2;
                    break;
                default:
                    v = row[x] - PaethPredictor(a, b, c);
                    break;
            }
            auto sv = static_cast<signed char>(v & 0xFF);
            score += std::abs(static_cast<int>(sv));
            candidate[x] = static_cast<unsigned char>(v & 0xFF);
        }
        if (best_score < 0 || score < best_score) {
            best_score = score;
            best.assign(1, static_cast<unsigned char>(ftype));
            best.insert(best.end(), candidate.begin(), candidate.end());
        }
    }
}

}  // namespace

std::string Encode(int width, int height, int comp, const unsigned char *pixels, int stride_bytes) {
    if (width <= 0 || height <= 0 || pixels == nullptr) return {};
    int color_type = ColorTypeForComp(comp);
    if (color_type < 0) return {};
    int bpp = comp;  // 8 bits/channel always

    std::string filtered;
    filtered.reserve(static_cast<size_t>(height) * (static_cast<size_t>(width) * static_cast<size_t>(comp) + 1));
    std::vector<unsigned char> best;
    const unsigned char *prior = nullptr;
    for (int y = 0; y < height; y++) {
        const unsigned char *row = pixels + static_cast<size_t>(y) * static_cast<size_t>(stride_bytes);
        FilterScanline(row, prior, static_cast<size_t>(width) * static_cast<size_t>(comp), bpp, best);
        filtered.append(reinterpret_cast<const char *>(best.data()), best.size());
        prior = row;
    }

    std::string compressed =
        deflate::DeflateZlib(reinterpret_cast<const unsigned char *>(filtered.data()), filtered.size());

    std::string out;
    out += std::string(reinterpret_cast<const char *>(kSignature), 8);

    std::string ihdr;
    WriteU32BE(ihdr, static_cast<uint32_t>(width));
    WriteU32BE(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(static_cast<char>(8));            // bit depth
    ihdr.push_back(static_cast<char>(color_type));
    ihdr.push_back(static_cast<char>(0));             // compression method
    ihdr.push_back(static_cast<char>(0));             // filter method
    ihdr.push_back(static_cast<char>(0));             // interlace method
    WriteChunk(out, "IHDR", ihdr);
    WriteChunk(out, "IDAT", compressed);
    WriteChunk(out, "IEND", "");
    return out;
}

}  // namespace png
