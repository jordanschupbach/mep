// PDFIUM_REMOVAL_PLAN.md Phase 4 coverage for pdf_filters.h/.cpp's
// remaining filters (FlateDecode + Predictor were already covered by
// pdf_xref_test.cpp in Phase 3, since xref streams/ObjStm needed them
// first): ASCIIHexDecode/ASCII85Decode/RunLengthDecode/LZWDecode/
// DCTDecode, and the /Filter-array chain dispatcher (DecodeStream).
//
// ASCII85 test vectors come from Python's stdlib `base64.a85encode`/
// `a85decode` (an independent, spec-conformant implementation) rather
// than hand-derivation. LZWDecode's test vectors come from a real
// LZW-compressed TIFF strip produced by ImageMagick+libtiff (an
// independent real-world LZW encoder, the same "external tool as
// oracle" rigor STB_IMAGE_REMOVAL_PLAN.md used for JPEG/BMP/GIF) --
// both a plain-LZW strip and one libtiff wrote with its own default
// TIFF horizontal-differencing Predictor, exercising LZWDecode's
// Predictor pass-through in the same real data. Both byte arrays are
// embedded directly (not read from a file at runtime) since test/ is
// gitignored in this repo (confirmed in Phase 1).

#include "pdf_filters.h"

#include "jpeg_codec.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

pdfobj::Object MakeIntDict(std::initializer_list<std::pair<const char *, long long>> entries) {
    pdfobj::Object dict;
    dict.type = pdfobj::Type::Dict;
    for (const auto &kv : entries) {
        pdfobj::Object v;
        v.type = pdfobj::Type::Int;
        v.int_val = kv.second;
        dict.dict_val[kv.first] = v;
    }
    return dict;
}

void TestASCIIHexDecode() {
    std::string out;
    CHECK(pdffilter::ASCIIHexDecode("48656C6C6F>", &out));
    CHECK(out == "Hello");
    CHECK(pdffilter::ASCIIHexDecode("48 65 6C\n6C 6F>", &out));  // interior whitespace tolerated
    CHECK(out == "Hello");
    CHECK(pdffilter::ASCIIHexDecode("901>", &out));  // odd digit count: implicit trailing 0
    CHECK(out.size() == 2);
    CHECK(static_cast<unsigned char>(out[0]) == 0x90);
    CHECK(static_cast<unsigned char>(out[1]) == 0x10);
    CHECK(pdffilter::ASCIIHexDecode("48656C6C6F", &out));  // no terminator: decode what's there
    CHECK(out == "Hello");
}

void TestASCII85Decode() {
    std::string out;
    // Reference vectors from Python's base64.a85encode/a85decode.
    CHECK(pdffilter::ASCII85Decode("9jqo^~>", &out));
    CHECK(out == "Man ");
    CHECK(pdffilter::ASCII85Decode("9jn~>", &out));  // partial group: 3 digits -> 2 bytes
    CHECK(out == "Ma");
    CHECK(pdffilter::ASCII85Decode("z~>", &out));  // 'z' shorthand: 4 zero bytes
    CHECK(out.size() == 4);
    for (char c : out) CHECK(c == '\0');
    CHECK(pdffilter::ASCII85Decode("87cURD_*#4DfTZ)+T~>", &out));
    CHECK(out == "Hello, World!");
}

void TestRunLengthDecode() {
    std::string out;
    // Literal run: length byte 4 -> copy next 5 literal bytes.
    std::string literal_input;
    literal_input.push_back(static_cast<char>(4));
    literal_input += "ABCDE";
    literal_input.push_back(static_cast<char>(128));  // EOD
    CHECK(pdffilter::RunLengthDecode(literal_input, &out));
    CHECK(out == "ABCDE");

    // Repeat run: length byte 253 (257-253=4 repeats) of 'X'.
    std::string repeat_input;
    repeat_input.push_back(static_cast<char>(253));
    repeat_input.push_back('X');
    repeat_input.push_back(static_cast<char>(128));
    CHECK(pdffilter::RunLengthDecode(repeat_input, &out));
    CHECK(out == "XXXX");

    // Mixed, no explicit EOD (tolerated: decode falls off the end).
    std::string mixed;
    mixed.push_back(static_cast<char>(1));
    mixed += "hi";
    mixed.push_back(static_cast<char>(255));
    mixed.push_back('!');
    CHECK(pdffilter::RunLengthDecode(mixed, &out));
    CHECK(out == "hi!!");  // 255 -> 257-255=2 repeats of '!'
}

void TestLZWDecodeNoPredictor() {
    // Real LZW-compressed strip from a 16x4 8-bit grayscale TIFF written
    // by ImageMagick/libtiff with Predictor explicitly disabled
    // (`-define tiff:predictor=1`) -- ground truth captured via
    // `magick raw.tiff -depth 8 gray:` on the uncompressed source.
    static const unsigned char kLzwNoPredictor[] = {
        0x80, 0x02, 0xa0, 0x41, 0x40, 0xa0, 0x78, 0x51, 0x07, 0x14, 0x0c, 0x87, 0x84, 0x62, 0x31, 0x40,
        0xb4, 0x0f, 0x88, 0x06, 0x43, 0x22, 0x31, 0x6c, 0x54, 0x5a, 0x37, 0x20, 0x92, 0xc9, 0x65, 0x52,
        0xfc, 0x12, 0x0b, 0x06, 0x19, 0x48, 0x61, 0x50, 0xd8, 0x71, 0x92, 0x25, 0x13, 0x8a, 0x0d, 0xe5,
        0x51, 0x88, 0xdc, 0x70, 0xd3, 0x01,
    };
    static const unsigned char kGroundTruth[] = {
        0x0a, 0x0a, 0x0a, 0x14, 0x14, 0x1e, 0x28, 0x28, 0x28, 0x28, 0x32, 0x3c, 0x46, 0x46, 0x50, 0x5a,
        0x0f, 0x0f, 0x0f, 0x19, 0x19, 0x23, 0x2d, 0x2d, 0x2d, 0x2d, 0x37, 0x41, 0x4b, 0x4b, 0x55, 0x5f,
        0x14, 0x14, 0x14, 0x1e, 0x1e, 0x28, 0x32, 0x32, 0x32, 0x32, 0x3c, 0x46, 0x50, 0x50, 0x5a, 0x64,
        0x19, 0x19, 0x19, 0x23, 0x23, 0x2d, 0x37, 0x37, 0x37, 0x37, 0x41, 0x4b, 0x55, 0x55, 0x5f, 0x69,
    };
    std::string raw(reinterpret_cast<const char *>(kLzwNoPredictor), sizeof(kLzwNoPredictor));
    std::string out;
    CHECK(pdffilter::LZWDecode(raw, nullptr, &out));
    CHECK(out.size() == sizeof(kGroundTruth));
    CHECK(std::memcmp(out.data(), kGroundTruth, sizeof(kGroundTruth)) == 0);
}

void TestLZWDecodeWithTiffPredictor() {
    // Same source image, but this time libtiff applied ITS OWN default
    // Predictor 2 (horizontal differencing) on top of LZW, exactly the
    // combination a real PDF's /Filter [/LZWDecode] + /DecodeParms
    // << /Predictor 2 /Columns 16 >> represents.
    static const unsigned char kLzwWithPredictor[] = {
        0x80, 0x02, 0x80, 0x00, 0x08, 0x10, 0x2a, 0x05, 0x03, 0x83, 0x41, 0x41, 0x40, 0xf8, 0x44, 0x12,
        0x0f, 0x0e, 0x85, 0x05, 0x21, 0xb0, 0x68, 0x1c, 0x42, 0x1c, 0x19, 0x89, 0xc3, 0xe1, 0x30, 0xe8,
        0x08,
    };
    static const unsigned char kGroundTruth[] = {
        0x0a, 0x0a, 0x0a, 0x14, 0x14, 0x1e, 0x28, 0x28, 0x28, 0x28, 0x32, 0x3c, 0x46, 0x46, 0x50, 0x5a,
        0x0f, 0x0f, 0x0f, 0x19, 0x19, 0x23, 0x2d, 0x2d, 0x2d, 0x2d, 0x37, 0x41, 0x4b, 0x4b, 0x55, 0x5f,
        0x14, 0x14, 0x14, 0x1e, 0x1e, 0x28, 0x32, 0x32, 0x32, 0x32, 0x3c, 0x46, 0x50, 0x50, 0x5a, 0x64,
        0x19, 0x19, 0x19, 0x23, 0x23, 0x2d, 0x37, 0x37, 0x37, 0x37, 0x41, 0x4b, 0x55, 0x55, 0x5f, 0x69,
    };
    std::string raw(reinterpret_cast<const char *>(kLzwWithPredictor), sizeof(kLzwWithPredictor));
    pdfobj::Object parms = MakeIntDict({{"Predictor", 2}, {"Columns", 16}});
    std::string out;
    CHECK(pdffilter::LZWDecode(raw, &parms, &out));
    CHECK(out.size() == sizeof(kGroundTruth));
    CHECK(std::memcmp(out.data(), kGroundTruth, sizeof(kGroundTruth)) == 0);
}

void TestDCTDecode() {
    // Light smoke test only -- jpeg::Decode itself is already covered
    // rigorously by mep-jpeg-codec-test; this just confirms the
    // pdffilter::DCTDecode wrapper plumbs dimensions/pixels through.
    int w = 4, h = 4;
    std::vector<unsigned char> pixels(static_cast<size_t>(w * h * 3));
    for (int i = 0; i < w * h; ++i) {
        pixels[static_cast<size_t>(i) * 3 + 0] = static_cast<unsigned char>(i * 10);
        pixels[static_cast<size_t>(i) * 3 + 1] = static_cast<unsigned char>(i * 20);
        pixels[static_cast<size_t>(i) * 3 + 2] = static_cast<unsigned char>(i * 30);
    }
    std::string jpeg_bytes = jpeg::Encode(w, h, 3, pixels.data(), w * 3, 90);
    CHECK(!jpeg_bytes.empty());

    std::string rgba;
    int out_w = 0, out_h = 0;
    CHECK(pdffilter::DCTDecode(jpeg_bytes, &rgba, &out_w, &out_h));
    CHECK(out_w == w && out_h == h);
    CHECK(rgba.size() == static_cast<size_t>(w * h * 4));

    CHECK(!pdffilter::DCTDecode("not a jpeg", &rgba, &out_w, &out_h));
}

void TestDecodeStreamChain() {
    // Single filter, Name form (not array).
    {
        pdfobj::Object dict;
        dict.type = pdfobj::Type::Dict;
        pdfobj::Object filter;
        filter.type = pdfobj::Type::Name;
        filter.str_val = "ASCIIHexDecode";
        dict.dict_val["Filter"] = filter;
        std::string out;
        CHECK(pdffilter::DecodeStream("48656C6C6F>", &dict, &out));
        CHECK(out == "Hello");
    }
    // Chain: [ASCII85Decode, ...] -- decode ASCII85 to get raw bytes,
    // matching a real-world "/Filter [/ASCII85Decode /FlateDecode]"
    // shape (tested here with just the first stage isolated via a
    // 1-element array, since FlateDecode's own chain behavior is
    // already covered by pdf_xref_test.cpp).
    {
        pdfobj::Object dict;
        dict.type = pdfobj::Type::Dict;
        pdfobj::Object filter;
        filter.type = pdfobj::Type::Array;
        pdfobj::Object name;
        name.type = pdfobj::Type::Name;
        name.str_val = "ASCII85Decode";
        filter.array_val.push_back(name);
        dict.dict_val["Filter"] = filter;
        std::string out;
        CHECK(pdffilter::DecodeStream("9jqo^~>", &dict, &out));
        CHECK(out == "Man ");
    }
    // No /Filter at all: passthrough.
    {
        pdfobj::Object dict;
        dict.type = pdfobj::Type::Dict;
        std::string out;
        CHECK(pdffilter::DecodeStream("raw bytes", &dict, &out));
        CHECK(out == "raw bytes");
    }
    // Unrecognized filter name: tolerated, passthrough for that stage.
    {
        pdfobj::Object dict;
        dict.type = pdfobj::Type::Dict;
        pdfobj::Object filter;
        filter.type = pdfobj::Type::Name;
        filter.str_val = "SomeFutureFilter";
        dict.dict_val["Filter"] = filter;
        std::string out;
        CHECK(pdffilter::DecodeStream("unchanged", &dict, &out));
        CHECK(out == "unchanged");
    }
    // DCTDecode named in /Filter: DecodeStream refuses rather than
    // mangling image bytes as if they were plain data.
    {
        pdfobj::Object dict;
        dict.type = pdfobj::Type::Dict;
        pdfobj::Object filter;
        filter.type = pdfobj::Type::Name;
        filter.str_val = "DCTDecode";
        dict.dict_val["Filter"] = filter;
        std::string out;
        CHECK(!pdffilter::DecodeStream("jpeg bytes here", &dict, &out));
    }
}

}  // namespace

int main() {
    TestASCIIHexDecode();
    TestASCII85Decode();
    TestRunLengthDecode();
    TestLZWDecodeNoPredictor();
    TestLZWDecodeWithTiffPredictor();
    TestDCTDecode();
    TestDecodeStreamChain();
    std::printf("pdf_filters_test: all checks passed\n");
    return 0;
}
