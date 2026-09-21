// Windowless test for image_codec.h's header-only dimension sniffer
// (image_codec::Dimensions), the cheap "how big is this figure?" answer
// org inline images size themselves from (Buffer::org_image_rows ->
// OrgImageLayoutFor). Round-trips PNG/JPEG through this repo's own
// encoders and hand-builds the two formats that have no encoder here
// (BMP, GIF), then checks the sniffed size against what a full
// image_codec::Decode reports.
// CHECK(), never assert(): the Release build strips assert() entirely.
#include "image_codec.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "jpeg_codec.h"
#include "png_codec.h"

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

const unsigned char *Bytes(const std::string &s) { return reinterpret_cast<const unsigned char *>(s.data()); }

// A w*h RGBA gradient, the pixel source both encoders are fed.
std::vector<unsigned char> Pixels(int w, int h) {
    std::vector<unsigned char> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; y++) {
        for (int xx = 0; xx < w; xx++) {
            size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(xx)) * 4;
            px[i + 0] = static_cast<unsigned char>(xx * 255 / (w > 1 ? w - 1 : 1));
            px[i + 1] = static_cast<unsigned char>(y * 255 / (h > 1 ? h - 1 : 1));
            px[i + 2] = 0x40;
            px[i + 3] = 0xFF;
        }
    }
    return px;
}

void PutLE16(std::string *s, unsigned v) {
    s->push_back(static_cast<char>(v & 0xFF));
    s->push_back(static_cast<char>((v >> 8) & 0xFF));
}
void PutLE32(std::string *s, unsigned v) {
    PutLE16(s, v & 0xFFFF);
    PutLE16(s, (v >> 16) & 0xFFFF);
}

// A 40-byte-DIB-header BMP with `h` negative meaning top-down rows --
// the sign is row order, not a dimension, so the sniff must report it
// as a positive height.
std::string MakeBmp(int w, int h, bool top_down) {
    const unsigned row_bytes = (static_cast<unsigned>(w) * 3u + 3u) & ~3u;
    const unsigned pixel_bytes = row_bytes * static_cast<unsigned>(h);
    std::string s = "BM";
    PutLE32(&s, 54u + pixel_bytes);  // file size
    PutLE32(&s, 0);                  // reserved
    PutLE32(&s, 54);                 // pixel data offset
    PutLE32(&s, 40);                 // DIB header size (BITMAPINFOHEADER)
    PutLE32(&s, static_cast<unsigned>(w));
    PutLE32(&s, top_down ? static_cast<unsigned>(-h) : static_cast<unsigned>(h));
    PutLE16(&s, 1);   // planes
    PutLE16(&s, 24);  // bits per pixel
    PutLE32(&s, 0);   // BI_RGB
    PutLE32(&s, pixel_bytes);
    PutLE32(&s, 2835);  // x pixels/metre
    PutLE32(&s, 2835);  // y pixels/metre
    PutLE32(&s, 0);     // palette colors used
    PutLE32(&s, 0);     // important colors
    s.append(pixel_bytes, '\x20');
    return s;
}

// A single-frame GIF87a whose logical screen descriptor carries the size
// (a 2-color global table, one 2x2-ish LZW-coded frame -- just enough
// for gif::Decode to agree with the sniff).
std::string MakeGif(int w, int h) {
    std::string s = "GIF87a";
    PutLE16(&s, static_cast<unsigned>(w));
    PutLE16(&s, static_cast<unsigned>(h));
    s.push_back(static_cast<char>(0x80));  // global color table, 2 entries
    s.push_back('\x00');                   // background index
    s.push_back('\x00');                   // pixel aspect ratio
    s.append("\x00\x00\x00", 3);           // color 0: black
    s.append("\xFF\xFF\xFF", 3);           // color 1: white
    s.push_back(',');                      // image descriptor
    PutLE16(&s, 0);
    PutLE16(&s, 0);
    PutLE16(&s, static_cast<unsigned>(w));
    PutLE16(&s, static_cast<unsigned>(h));
    s.push_back('\x00');  // no local table, not interlaced
    const int min_code_size = 2;
    s.push_back(static_cast<char>(min_code_size));
    // A deliberately naive LZW stream: every pixel as its own literal
    // code, no dictionary matches emitted. Still has to track the code
    // width exactly the way gif_codec.cpp's decoder grows it (one new
    // table entry per code after the first, widening when next_code
    // reaches 1<<width), because that is what the reader uses to decide
    // how many bits the *next* code occupies.
    const int clear_code = 1 << min_code_size;
    const int end_code = clear_code + 1;
    std::string codes;
    unsigned acc = 0;
    int nbits = 0;
    int width = min_code_size + 1;
    auto emit = [&](int code) {
        acc |= static_cast<unsigned>(code) << nbits;
        nbits += width;
        while (nbits >= 8) {
            codes.push_back(static_cast<char>(acc & 0xFF));
            acc >>= 8;
            nbits -= 8;
        }
    };
    emit(clear_code);
    int next_code = end_code + 1;
    bool first = true;
    for (int i = 0; i < w * h; i++) {
        emit(0);
        if (!first && next_code < 4096) {
            next_code++;
            if (next_code == (1 << width) && width < 12) width++;
        }
        first = false;
    }
    emit(end_code);
    if (nbits > 0) codes.push_back(static_cast<char>(acc & 0xFF));
    s.push_back(static_cast<char>(codes.size()));
    s += codes;
    s.push_back('\x00');  // block terminator
    s.push_back(';');     // trailer
    return s;
}

// Sniffs `data` and, when `also_decode`, checks a full decode agrees.
void CheckSize(const std::string &data, int want_w, int want_h, bool also_decode, const char *what) {
    int w = 0, h = 0;
    std::string err;
    if (!image_codec::Dimensions(Bytes(data), data.size(), &w, &h, &err)) {
        std::fprintf(stderr, "Dimensions(%s) failed: %s\n", what, err.c_str());
        std::abort();
    }
    if (w != want_w || h != want_h) {
        std::fprintf(stderr, "Dimensions(%s) = %dx%d, want %dx%d\n", what, w, h, want_w, want_h);
        std::abort();
    }
    if (!also_decode) return;
    int dw = 0, dh = 0;
    std::string derr;
    unsigned char *px = image_codec::Decode(Bytes(data), data.size(), &dw, &dh, &derr);
    if (!px) {
        std::fprintf(stderr, "Decode(%s) failed: %s\n", what, derr.c_str());
        std::abort();
    }
    std::free(px);
    if (dw != w || dh != h) {
        std::fprintf(stderr, "Decode(%s) = %dx%d, sniff said %dx%d\n", what, dw, dh, w, h);
        std::abort();
    }
}
}  // namespace

int main() {
    // --- PNG: IHDR at its fixed offset, agreeing with a real decode.
    {
        const int w = 37, h = 11;
        std::vector<unsigned char> px = Pixels(w, h);
        std::string png = png::Encode(w, h, 4, px.data(), w * 4);
        CHECK(!png.empty());
        CheckSize(png, w, h, true, "png");
    }
    // --- JPEG: found by walking the marker chain to SOF0, past whatever
    // header segments the encoder emitted first.
    {
        const int w = 64, h = 48;
        std::vector<unsigned char> px = Pixels(w, h);
        std::string jpg = jpeg::Encode(w, h, 4, px.data(), w * 4, 80);
        CHECK(!jpg.empty());
        CheckSize(jpg, w, h, true, "jpeg");
    }
    // --- BMP: both row orders report a positive height.
    {
        CheckSize(MakeBmp(9, 5, false), 9, 5, true, "bmp bottom-up");
        CheckSize(MakeBmp(9, 5, true), 9, 5, true, "bmp top-down");
    }
    // --- GIF: the logical screen descriptor, which is also the canvas
    // size gif::Decode composites onto.
    {
        CheckSize(MakeGif(6, 4), 6, 4, true, "gif");
    }
    // --- Rejections: nothing recognizable, and truncated headers.
    {
        int w = 0, h = 0;
        std::string err;
        const std::string junk = "not an image at all, just prose";
        CHECK(!image_codec::Dimensions(Bytes(junk), junk.size(), &w, &h, &err));
        CHECK(!err.empty());
        CHECK(!image_codec::Dimensions(nullptr, 0, &w, &h, &err));

        // A PNG signature with nothing behind it: recognized as PNG, but
        // no IHDR to read, so this must fail rather than report 0x0.
        const std::string stub_png("\x89PNG\r\n\x1a\n", 8);
        CHECK(!image_codec::Dimensions(Bytes(stub_png), stub_png.size(), &w, &h, &err));
        // Likewise a bare SOI with no SOF segment behind it.
        const std::string stub_jpg("\xFF\xD8\xFF\xD9", 4);
        CHECK(!image_codec::Dimensions(Bytes(stub_jpg), stub_jpg.size(), &w, &h, &err));

        // A truncated PNG still sniffs: the header is all this reads, so
        // it answers for a file a full decode would reject. That's the
        // documented contract (a claim about the header, not the file).
        std::vector<unsigned char> px = Pixels(20, 10);
        std::string png = png::Encode(20, 10, 4, px.data(), 20 * 4);
        CHECK(!png.empty());
        const std::string head = png.substr(0, 30);
        CHECK(image_codec::Dimensions(Bytes(head), head.size(), &w, &h, &err));
        CHECK(w == 20 && h == 10);
    }
    // --- The file-reading wrapper, including a path that isn't there.
    {
        const int w = 12, h = 34;
        std::vector<unsigned char> px = Pixels(w, h);
        std::string png = png::Encode(w, h, 4, px.data(), w * 4);
        CHECK(!png.empty());
        const char *path = "image_codec_test_tmp.png";
        std::FILE *fp = std::fopen(path, "wb");
        CHECK(fp != nullptr);
        CHECK(std::fwrite(png.data(), 1, png.size(), fp) == png.size());
        std::fclose(fp);

        int fw = 0, fh = 0;
        std::string err;
        CHECK(image_codec::DimensionsFile(path, &fw, &fh, &err));
        CHECK(fw == w && fh == h);
        std::remove(path);
        CHECK(!image_codec::DimensionsFile(path, &fw, &fh, &err));
        CHECK(!err.empty());
    }

    std::printf("image_codec_test: all checks passed\n");
    return 0;
}
