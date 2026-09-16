// PDFIUM_REMOVAL_PLAN.md Phase 7 + Phase 8 coverage for pdf_content.h/
// .cpp: the content-stream interpreter's graphics-state stack, path
// construction/painting, clipping, color operators, Form XObjects
// (Phase 7), and image XObjects/inline images/SMask (Phase 8). Most
// Phase 7 tests render a hand-written content stream directly via
// pdfrender::RenderContentStream against a dummy/empty XrefTable+
// Resources (none of the graphics-state/path/device-color operators
// need to resolve anything) -- the Form XObject and every image test
// need a real xref-backed document instead (built the same "compute
// offsets from actual string positions" way as pdf_xref_test.cpp/
// pdf_document_test.cpp), since `Do` always resolves through
// Resources/XObject and pdfxref::ResolveStream.

#include "pdf_content.h"

#include "deflate.h"
#include "jpeg_codec.h"
#include "pdf_crypt.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using pdfrender::Canvas;
using pdfrender::Mat2D;

struct RGB {
    unsigned char r, g, b;
};

RGB PixelAt(const Canvas &c, int x, int y) {
    size_t i = (static_cast<size_t>(y) * static_cast<size_t>(c.width) + static_cast<size_t>(x)) * 4;
    return {c.rgba[i], c.rgba[i + 1], c.rgba[i + 2]};
}

bool CloseTo(unsigned char a, unsigned char b, int tol = 3) { return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= tol; }

void Render(const std::string &content, Canvas &canvas, const Mat2D &ctm) {
    pdfobj::Object resources;  // Null: fine, no operator in these tests needs a real Resources lookup unless noted
    pdfxref::XrefTable table;  // empty: fine for the same reason
    pdfrender::RenderContentStream(content, canvas, ctm, resources, nullptr, 0, table);
}

Mat2D IdentityPageMatrix(int w, int h) {
    // A simple "1 device px per point, y-flip only" matrix -- equivalent
    // to PageToDeviceMatrix(0,0,w,h,0,1.0), spelled out again here would
    // just be redundant; use the real function to also exercise it.
    return pdfrender::PageToDeviceMatrix(0, 0, w, h, 0, 1.0);
}

// -- Real xref-backed document construction (Form/Image XObjects always
// resolve through Resources/XObject + pdfxref::ResolveStream, so these
// can't be tested against a dummy empty XrefTable the way Phase 7's
// pure graphics-state/path/color tests could) -- same "compute offsets
// from actual string positions" approach as pdf_document_test.cpp's own
// BuildDoc, duplicated here rather than shared since each phase's test
// file has stayed self-contained throughout this plan.

std::string XrefLine(long long offset, int gen, char kind) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%010lld %05d %c \n", offset, gen, kind);
    return buf;
}

std::string BuildDoc(const std::vector<std::pair<int, std::string>> &objects, int root_num,
                      const std::string &trailer_extra = "") {
    std::string doc = "%PDF-1.4\n";
    std::vector<std::pair<int, size_t>> offsets;
    int max_num = 0;
    for (const auto &obj : objects) {
        offsets.emplace_back(obj.first, doc.size());
        doc += std::to_string(obj.first) + " 0 obj\n" + obj.second + "\nendobj\n";
        max_num = std::max(max_num, obj.first);
    }
    size_t xref_off = doc.size();
    doc += "xref\n0 " + std::to_string(max_num + 1) + "\n";
    doc += XrefLine(0, 65535, 'f');
    for (int n = 1; n <= max_num; ++n) {
        auto it = std::find_if(offsets.begin(), offsets.end(), [&](const auto &p) { return p.first == n; });
        doc += it == offsets.end() ? XrefLine(0, 0, 'f') : XrefLine(static_cast<long long>(it->second), 0, 'n');
    }
    // `trailer_extra` (PDFIUM_REMOVAL_PLAN.md Phase 12): lets a caller
    // add e.g. " /Encrypt 5 0 R /ID [<..> <..>]" without this helper
    // needing to know anything about encryption itself.
    doc += "trailer\n<< /Size " + std::to_string(max_num + 1) + " /Root " + std::to_string(root_num) + " 0 R" +
           trailer_extra + " >>\n";
    doc += "startxref\n" + std::to_string(xref_off) + "\n%%EOF\n";
    return doc;
}

// Renders object `page_content_obj_num`'s content (already resolved via
// a real XrefTable built from `doc`) onto a `w`x`h` canvas at 1px/pt.
// std::string's operator+=(const char*) stops at the first embedded NUL
// byte (via strlen) -- every raw-pixel-byte literal below needs this
// explicit-length form instead, since 0x00 is a completely ordinary
// byte value for image samples.
std::string Bytes(std::initializer_list<unsigned char> bytes) { return std::string(bytes.begin(), bytes.end()); }

Canvas RenderDoc(const std::string &doc, const pdfobj::Object &resources, const std::string &content, int w, int h) {
    Canvas canvas = Canvas::MakeWhite(w, h);
    pdfxref::XrefTable table;
    table.Load(reinterpret_cast<const unsigned char *>(doc.data()), doc.size());
    pdfrender::RenderContentStream(content, canvas, IdentityPageMatrix(w, h), resources,
                                    reinterpret_cast<const unsigned char *>(doc.data()), doc.size(), table);
    return canvas;
}

void TestPageToDeviceMatrixCorners() {
    // Rotate 0: (0,0) [user bottom-left] -> device (0,H) [bottom-left,
    // y-down]; (0,H) [user top-left] -> device (0,0).
    {
        Mat2D m = pdfrender::PageToDeviceMatrix(0, 0, 100, 200, 0, 1.0);
        double x, y;
        pdfrender::Transform(m, 0, 0, &x, &y);
        CHECK(x == 0 && y == 200);
        pdfrender::Transform(m, 0, 200, &x, &y);
        CHECK(x == 0 && y == 0);
        pdfrender::Transform(m, 100, 0, &x, &y);
        CHECK(x == 100 && y == 200);
    }
    // Rotate 90: output dims swap (H x W = 200x100 -> device is 200 wide, 100 tall).
    {
        Mat2D m = pdfrender::PageToDeviceMatrix(0, 0, 100, 200, 90, 1.0);
        double x, y;
        pdfrender::Transform(m, 0, 0, &x, &y);  // user bottom-left
        CHECK(x == 0 && y == 0);
        pdfrender::Transform(m, 100, 0, &x, &y);  // user bottom-right
        CHECK(x == 0 && y == 100);
        pdfrender::Transform(m, 0, 200, &x, &y);  // user top-left
        CHECK(x == 200 && y == 0);
    }
    // Rotate 180.
    {
        Mat2D m = pdfrender::PageToDeviceMatrix(0, 0, 100, 200, 180, 1.0);
        double x, y;
        pdfrender::Transform(m, 0, 0, &x, &y);
        CHECK(x == 100 && y == 0);
        pdfrender::Transform(m, 100, 200, &x, &y);
        CHECK(x == 0 && y == 200);
    }
    // Rotate 270.
    {
        Mat2D m = pdfrender::PageToDeviceMatrix(0, 0, 100, 200, 270, 1.0);
        double x, y;
        pdfrender::Transform(m, 0, 0, &x, &y);
        CHECK(x == 200 && y == 100);
        pdfrender::Transform(m, 100, 0, &x, &y);
        CHECK(x == 200 && y == 0);
    }
}

void TestFillRectangleKnownColor() {
    Canvas canvas = Canvas::MakeWhite(50, 50);
    // A red rectangle from (10,10) to (30,30) in a 50x50 page (1px/pt, rotate 0).
    std::string content = "1 0 0 rg 10 10 20 20 re f";
    Render(content, canvas, IdentityPageMatrix(50, 50));
    // Rect spans user x/y [10,30]; device y = 50 - user_y flips that to
    // device y range [20,40], device x range stays [10,30].
    RGB inside = PixelAt(canvas, 20, 30);
    RGB outside = PixelAt(canvas, 5, 5);
    CHECK(inside.r > 200 && inside.g < 30 && inside.b < 30);
    CHECK(outside.r == 255 && outside.g == 255 && outside.b == 255);
}

void TestGraphicsStateStackIsolation() {
    Canvas canvas = Canvas::MakeWhite(50, 50);
    // Set red, push, change to blue + translate, pop -- final fill (after Q) should still be red at the original position.
    std::string content = "1 0 0 rg q 0 0 1 rg 1 0 0 0 1 0 cm Q 10 10 20 20 re f";
    Render(content, canvas, IdentityPageMatrix(50, 50));
    RGB p = PixelAt(canvas, 20, 30);
    CHECK(p.r > 200 && p.g < 30 && p.b < 30);  // red, not blue -- Q restored color and CTM
}

void TestEvenOddVsNonZeroThroughOperators() {
    // Two same-direction (both drawn via 're', which always winds the
    // same way) overlapping rectangles: nonzero 'f' fills the overlap,
    // even-odd 'f*' punches a hole in it.
    {
        Canvas canvas = Canvas::MakeWhite(20, 20);
        std::string content = "0 0 0 rg 0 0 12 12 re 6 6 12 12 re f";
        Render(content, canvas, IdentityPageMatrix(20, 20));
        RGB overlap = PixelAt(canvas, 9, 20 - 9);
        CHECK(overlap.r < 30);  // filled (black) under nonzero
    }
    {
        Canvas canvas = Canvas::MakeWhite(20, 20);
        std::string content = "0 0 0 rg 0 0 12 12 re 6 6 12 12 re f*";
        Render(content, canvas, IdentityPageMatrix(20, 20));
        RGB overlap = PixelAt(canvas, 9, 20 - 9);
        CHECK(overlap.r > 240);  // hole (white) under even-odd
    }
}

void TestCmykConversion() {
    Canvas canvas = Canvas::MakeWhite(10, 10);
    // Pure black via CMYK (0 0 0 1 k) over the whole 10x10 canvas.
    std::string content = "0 0 0 1 k 0 0 10 10 re f";
    Render(content, canvas, IdentityPageMatrix(10, 10));
    RGB p = PixelAt(canvas, 5, 5);
    CHECK(p.r < 5 && p.g < 5 && p.b < 5);
}

void TestClipRestrictsFill() {
    Canvas canvas = Canvas::MakeWhite(40, 40);
    // Clip to [5,5]-[15,15], then fill a much bigger black rectangle --
    // only the clipped region should end up black.
    std::string content = "5 5 10 10 re W n 0 0 0 rg 0 0 40 40 re f";
    Render(content, canvas, IdentityPageMatrix(40, 40));
    RGB inside_clip = PixelAt(canvas, 10, 40 - 10);
    RGB outside_clip = PixelAt(canvas, 30, 40 - 30);
    CHECK(inside_clip.r < 30);
    CHECK(outside_clip.r > 240);
}

void TestStrokeLine() {
    Canvas canvas = Canvas::MakeWhite(40, 40);
    // A thick horizontal black line across the middle.
    std::string content = "4 w 0 0 0 RG 5 20 m 35 20 l S";
    Render(content, canvas, IdentityPageMatrix(40, 40));
    RGB on_line = PixelAt(canvas, 20, 20);
    RGB off_line = PixelAt(canvas, 20, 2);
    CHECK(on_line.r < 30);
    CHECK(off_line.r > 240);
}

void TestExtGStateAlphaBlends() {
    Canvas canvas = Canvas::MakeWhite(10, 10);
    // 50% alpha black over white should land near mid-gray. No real
    // Resources/ExtGState dict is wired up in this test (`gs` looks up
    // `/GS1` in Resources/ExtGState, which is Null here) -- so this
    // exercises the "ExtGState not found: alpha stays at its prior
    // value" tolerance path instead. Alpha itself is exercised for real
    // via the ApplyExtGState code path in TestExtGStateAlphaViaResources
    // below, which does wire up a real Resources dict.
    std::string content = "0 0 0 rg 0 0 10 10 re f";
    Render(content, canvas, IdentityPageMatrix(10, 10));
    RGB p = PixelAt(canvas, 5, 5);
    CHECK(p.r < 5);  // sanity: full-alpha fill still fully opaque
}

void TestExtGStateAlphaViaResources() {
    Canvas canvas = Canvas::MakeWhite(10, 10);
    pdfobj::Object resources;
    resources.type = pdfobj::Type::Dict;
    pdfobj::Object extgstate_dict;
    extgstate_dict.type = pdfobj::Type::Dict;
    pdfobj::Object gs1;
    gs1.type = pdfobj::Type::Dict;
    pdfobj::Object ca;
    ca.type = pdfobj::Type::Real;
    ca.real_val = 0.5;
    gs1.dict_val["ca"] = ca;
    extgstate_dict.dict_val["GS1"] = gs1;
    resources.dict_val["ExtGState"] = extgstate_dict;

    pdfxref::XrefTable table;
    std::string content = "/GS1 gs 0 0 0 rg 0 0 10 10 re f";
    pdfrender::RenderContentStream(content, canvas, IdentityPageMatrix(10, 10), resources, nullptr, 0, table);
    RGB p = PixelAt(canvas, 5, 5);
    // 50% black over white -> ~127.
    CHECK(CloseTo(p.r, 128, 5) && CloseTo(p.g, 128, 5) && CloseTo(p.b, 128, 5));
}

void TestIndexedColorSpace() {
    Canvas canvas = Canvas::MakeWhite(10, 10);
    pdfobj::Object resources;
    resources.type = pdfobj::Type::Dict;
    pdfobj::Object cs_dict;
    cs_dict.type = pdfobj::Type::Dict;
    pdfobj::Object indexed;
    indexed.type = pdfobj::Type::Array;
    pdfobj::Object fam;
    fam.type = pdfobj::Type::Name;
    fam.str_val = "Indexed";
    pdfobj::Object base;
    base.type = pdfobj::Type::Name;
    base.str_val = "DeviceRGB";
    pdfobj::Object hival;
    hival.type = pdfobj::Type::Int;
    hival.int_val = 1;
    pdfobj::Object lookup;
    lookup.type = pdfobj::Type::String;
    lookup.str_val = std::string("\x00\x00\x00", 3) + std::string("\xff\x00\x00", 3);  // index 0 = black, index 1 = red
    indexed.array_val = {fam, base, hival, lookup};
    cs_dict.dict_val["MyIdx"] = indexed;
    resources.dict_val["ColorSpace"] = cs_dict;

    pdfxref::XrefTable table;
    std::string content = "/MyIdx cs 1 scn 0 0 10 10 re f";  // select index 1 -> red
    pdfrender::RenderContentStream(content, canvas, IdentityPageMatrix(10, 10), resources, nullptr, 0, table);
    RGB p = PixelAt(canvas, 5, 5);
    CHECK(p.r > 200 && p.g < 30 && p.b < 30);
}

void TestFormXObject() {
    // Form's own content ("0 0 10 10 re f" in red) draws in form space;
    // /Matrix translates the form by (5,5) into the page; /BBox [0 0 10
    // 10] (also form space) doesn't additionally clip anything here
    // since it exactly covers what's drawn.
    std::string form_content = "1 0 0 rg 0 0 10 10 re f";
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 40 40] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /XObject /Subtype /Form /BBox [0 0 10 10] /Matrix [1 0 0 1 5 5] /Length " +
                    std::to_string(form_content.size()) + " >>\nstream\n" + form_content + "\nendstream"},
            {5, "<< /Length 8 >>\nstream\n/Fm1 Do\nendstream"},
        },
        1);
    pdfobj::Object resources;
    resources.type = pdfobj::Type::Dict;
    pdfobj::Object xobj_dict;
    xobj_dict.type = pdfobj::Type::Dict;
    pdfobj::Object fm1_ref;
    fm1_ref.type = pdfobj::Type::Reference;
    fm1_ref.ref_val = {4, 0};
    xobj_dict.dict_val["Fm1"] = fm1_ref;
    resources.dict_val["XObject"] = xobj_dict;

    Canvas canvas = RenderDoc(doc, resources, "/Fm1 Do", 40, 40);
    // Form-space (0,0)-(10,10) -> user (5,5)-(15,15) -> device x[5,15], y[25,35] (y=40-user_y).
    RGB inside = PixelAt(canvas, 10, 30);
    RGB outside = PixelAt(canvas, 25, 25);
    CHECK(inside.r > 200 && inside.g < 30 && inside.b < 30);
    CHECK(outside.r == 255 && outside.g == 255 && outside.b == 255);
}

pdfobj::Object MakeXObjectResources(int obj_num, const char *name) {
    pdfobj::Object resources;
    resources.type = pdfobj::Type::Dict;
    pdfobj::Object xobj_dict;
    xobj_dict.type = pdfobj::Type::Dict;
    pdfobj::Object ref;
    ref.type = pdfobj::Type::Reference;
    ref.ref_val = {obj_num, 0};
    xobj_dict.dict_val[name] = ref;
    resources.dict_val["XObject"] = xobj_dict;
    return resources;
}

void TestImageXObjectRawRGB() {
    // 2x2 RGB, row-major top-to-bottom: red,green / blue,white.
    std::string pixels = Bytes({0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff});
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 20 20] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /XObject /Subtype /Image /Width 2 /Height 2 /BitsPerComponent 8 /ColorSpace "
                "/DeviceRGB /Length " +
                    std::to_string(pixels.size()) + " >>\nstream\n" + pixels + "\nendstream"},
            {5, "<< /Length 20 >>\nstream\n20 0 0 20 0 0 cm /Im1 Do\nendstream"},
        },
        1);
    Canvas canvas = RenderDoc(doc, MakeXObjectResources(4, "Im1"), "20 0 0 20 0 0 cm /Im1 Do", 20, 20);
    RGB top_left = PixelAt(canvas, 5, 5);
    RGB top_right = PixelAt(canvas, 15, 5);
    RGB bot_left = PixelAt(canvas, 5, 15);
    RGB bot_right = PixelAt(canvas, 15, 15);
    CHECK(top_left.r > 200 && top_left.g < 30 && top_left.b < 30);      // red
    CHECK(top_right.r < 30 && top_right.g > 200 && top_right.b < 30);   // green
    CHECK(bot_left.r < 30 && bot_left.g < 30 && bot_left.b > 200);      // blue
    CHECK(bot_right.r > 200 && bot_right.g > 200 && bot_right.b > 200);  // white
}

void TestImageXObjectFlateDecode() {
    std::string pixels = Bytes({0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff});
    std::string compressed = deflate::DeflateZlib(reinterpret_cast<const unsigned char *>(pixels.data()), pixels.size());
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 20 20] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /XObject /Subtype /Image /Width 2 /Height 2 /BitsPerComponent 8 /ColorSpace "
                "/DeviceRGB /Filter /FlateDecode /Length " +
                    std::to_string(compressed.size()) + " >>\nstream\n" + compressed + "\nendstream"},
            {5, "<< /Length 20 >>\nstream\n20 0 0 20 0 0 cm /Im1 Do\nendstream"},
        },
        1);
    Canvas canvas = RenderDoc(doc, MakeXObjectResources(4, "Im1"), "20 0 0 20 0 0 cm /Im1 Do", 20, 20);
    RGB top_left = PixelAt(canvas, 5, 5);
    RGB bot_right = PixelAt(canvas, 15, 15);
    CHECK(top_left.r > 200 && top_left.g < 30 && top_left.b < 30);
    CHECK(bot_right.r > 200 && bot_right.g > 200 && bot_right.b > 200);
}

void TestImageMask() {
    // 2x2 1-bit mask, default Decode [0 1] (bit 0 = paint, 1 = don't):
    // row0 = paint,don't; row1 = paint,paint.
    std::string mask_data;
    mask_data.push_back(static_cast<char>(0x40));  // 0b01000000
    mask_data.push_back(static_cast<char>(0x00));
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 20 20] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /XObject /Subtype /Image /Width 2 /Height 2 /ImageMask true /Length " +
                    std::to_string(mask_data.size()) + " >>\nstream\n" + mask_data + "\nendstream"},
            {5, "<< /Length 40 >>\nstream\n1 0 0 rg 20 0 0 20 0 0 cm /Im1 Do\nendstream"},
        },
        1);
    Canvas canvas = RenderDoc(doc, MakeXObjectResources(4, "Im1"), "1 0 0 rg 20 0 0 20 0 0 cm /Im1 Do", 20, 20);
    RGB top_left = PixelAt(canvas, 5, 5);    // row0,col0: paint -> red
    RGB top_right = PixelAt(canvas, 15, 5);  // row0,col1: don't -> white
    RGB bot_left = PixelAt(canvas, 5, 15);   // row1,col0: paint -> red
    CHECK(top_left.r > 200 && top_left.g < 30 && top_left.b < 30);
    CHECK(top_right.r == 255 && top_right.g == 255 && top_right.b == 255);
    CHECK(bot_left.r > 200 && bot_left.g < 30 && bot_left.b < 30);
}

void TestIndexedImageXObject() {
    // 2x2, 1-component 8bpc indices into a 2-entry DeviceRGB palette
    // (0=black, 1=red): top row both index 1 (red), bottom row both index 0 (black).
    std::string lookup = Bytes({0x00, 0x00, 0x00, 0xff, 0x00, 0x00});
    std::string indices;
    indices.push_back(1);
    indices.push_back(1);
    indices.push_back(0);
    indices.push_back(0);
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 20 20] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /XObject /Subtype /Image /Width 2 /Height 2 /BitsPerComponent 8 /ColorSpace "
                "[/Indexed /DeviceRGB 1 (" +
                    lookup + ")] /Length " + std::to_string(indices.size()) + " >>\nstream\n" + indices +
                    "\nendstream"},
            {5, "<< /Length 20 >>\nstream\n20 0 0 20 0 0 cm /Im1 Do\nendstream"},
        },
        1);
    Canvas canvas = RenderDoc(doc, MakeXObjectResources(4, "Im1"), "20 0 0 20 0 0 cm /Im1 Do", 20, 20);
    RGB top = PixelAt(canvas, 10, 5);
    RGB bottom = PixelAt(canvas, 10, 15);
    CHECK(top.r > 200 && top.g < 30 && top.b < 30);  // red
    CHECK(bottom.r < 30 && bottom.g < 30 && bottom.b < 30);  // black
}

void TestSMaskAlpha() {
    // 1x1 red base image with a 1x1 SMask at gray 128 (~50% alpha) --
    // blended over white should land near (255, 128, 128).
    std::string base_pixel = Bytes({0xff, 0x00, 0x00});
    std::string smask_pixel;
    smask_pixel.push_back(static_cast<char>(128));
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 10 10] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 6 0 R >>"},
            {4, "<< /Type /XObject /Subtype /Image /Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace "
                "/DeviceGray /Length 1 >>\nstream\n" +
                    smask_pixel + "\nendstream"},
            {5, "<< /Type /XObject /Subtype /Image /Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace "
                "/DeviceRGB /SMask 4 0 R /Length 3 >>\nstream\n" +
                    base_pixel + "\nendstream"},
            {6, "<< /Length 20 >>\nstream\n10 0 0 10 0 0 cm /Im1 Do\nendstream"},
        },
        1);
    Canvas canvas = RenderDoc(doc, MakeXObjectResources(5, "Im1"), "10 0 0 10 0 0 cm /Im1 Do", 10, 10);
    RGB p = PixelAt(canvas, 5, 5);
    CHECK(p.r == 255);
    CHECK(CloseTo(p.g, 128, 5) && CloseTo(p.b, 128, 5));
}

void TestInlineImageUnfiltered() {
    std::string pixel;
    pixel.push_back(0);
    pixel.push_back(static_cast<char>(255));
    pixel.push_back(0);  // green
    std::string content = "10 0 0 10 0 0 cm BI /W 1 /H 1 /BPC 8 /CS /RGB ID " + pixel + " EI";
    Canvas canvas = Canvas::MakeWhite(10, 10);
    Render(content, canvas, IdentityPageMatrix(10, 10));
    RGB p = PixelAt(canvas, 5, 5);
    CHECK(p.r < 30 && p.g > 200 && p.b < 30);
}

void TestDctImageXObject() {
    // A small synthetic solid-blue JPEG (jpeg::Encode -- already
    // rigorously verified independently by mep-jpeg-codec-test; this
    // just confirms the Image-XObject DCT dispatch plumbs it through).
    constexpr int W = 8, H = 8;
    std::vector<unsigned char> pixels(static_cast<size_t>(W * H * 3));
    for (int i = 0; i < W * H; ++i) {
        pixels[static_cast<size_t>(i) * 3 + 0] = 0;
        pixels[static_cast<size_t>(i) * 3 + 1] = 0;
        pixels[static_cast<size_t>(i) * 3 + 2] = 255;
    }
    std::string jpeg_bytes = jpeg::Encode(W, H, 3, pixels.data(), W * 3, 95);
    CHECK(!jpeg_bytes.empty());
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 20 20] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /XObject /Subtype /Image /Width " + std::to_string(W) + " /Height " + std::to_string(H) +
                    " /BitsPerComponent 8 /ColorSpace /DeviceRGB /Filter /DCTDecode /Length " +
                    std::to_string(jpeg_bytes.size()) + " >>\nstream\n" + jpeg_bytes + "\nendstream"},
            {5, "<< /Length 20 >>\nstream\n20 0 0 20 0 0 cm /Im1 Do\nendstream"},
        },
        1);
    Canvas canvas = RenderDoc(doc, MakeXObjectResources(4, "Im1"), "20 0 0 20 0 0 cm /Im1 Do", 20, 20);
    RGB p = PixelAt(canvas, 10, 10);
    CHECK(p.r < 20 && p.g < 20 && p.b > 220);  // blue, small JPEG rounding tolerance
}

// -- Phase 10: text-showing operators --------------------------------------
// Uses standard-14 (Helvetica) substitution throughout -- no FontFile
// needed, keeping these synthetic test PDFs simple -- real embedded-font
// text rendering is verified separately, end to end against real
// fixtures, in this phase's own PDFIUM_REMOVAL_PLAN.md writeup.

pdfobj::Object MakeFontResources(int obj_num, const char *name) {
    pdfobj::Object resources;
    resources.type = pdfobj::Type::Dict;
    pdfobj::Object font_dict;
    font_dict.type = pdfobj::Type::Dict;
    pdfobj::Object ref;
    ref.type = pdfobj::Type::Reference;
    ref.ref_val = {obj_num, 0};
    font_dict.dict_val[name] = ref;
    resources.dict_val["Font"] = font_dict;
    return resources;
}

int CountDarkPixels(const Canvas &c) {
    int n = 0;
    for (int y = 0; y < c.height; ++y) {
        for (int x = 0; x < c.width; ++x) {
            if (PixelAt(c, x, y).r < 128) ++n;
        }
    }
    return n;
}

void TestBasicTextShowing() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 100 50] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"},
            {5, "<< /Length 40 >>\nstream\nBT /F1 24 Tf 10 10 Td (A) Tj ET\nendstream"},
        },
        1);
    Canvas canvas = RenderDoc(doc, MakeFontResources(4, "F1"), "BT /F1 24 Tf 10 10 Td (A) Tj ET", 100, 50);
    CHECK(CountDarkPixels(canvas) > 0);  // an 'A' actually got drawn somewhere
}

// Bounding box of every dark pixel: {min_x, min_y, max_x, max_y}, or
// all -1 if nothing is dark.
struct DarkBox {
    int x0 = -1, y0 = -1, x1 = -1, y1 = -1;
};
DarkBox DarkBounds(const Canvas &c) {
    DarkBox b;
    for (int y = 0; y < c.height; ++y) {
        for (int x = 0; x < c.width; ++x) {
            if (PixelAt(c, x, y).r >= 128) continue;
            if (b.x0 < 0 || x < b.x0) b.x0 = x;
            if (b.y0 < 0 || y < b.y0) b.y0 = y;
            if (x > b.x1) b.x1 = x;
            if (y > b.y1) b.y1 = y;
        }
    }
    return b;
}

void TestRotatedTextRendersRotated() {
    // A lowercase 'l' is a tall thin bar upright; under a 90-degree Tm
    // it must come out as a wide flat bar (an earlier version rendered
    // every glyph upright regardless of Tm/CTM rotation -- visibly
    // wrong for the rotated y-axis labels every R/matplotlib plot has).
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 100 100] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},
            {4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"},
        },
        1);
    Canvas upright = RenderDoc(doc, MakeFontResources(4, "F1"), "BT /F1 40 Tf 40 30 Td (l) Tj ET", 100, 100);
    DarkBox u = DarkBounds(upright);
    CHECK(u.x0 >= 0);
    CHECK((u.y1 - u.y0) > 3 * (u.x1 - u.x0));

    Canvas rotated = RenderDoc(doc, MakeFontResources(4, "F1"), "BT /F1 40 Tf 0 1 -1 0 60 30 Tm (l) Tj ET", 100, 100);
    DarkBox r = DarkBounds(rotated);
    CHECK(r.x0 >= 0);
    CHECK((r.x1 - r.x0) > 3 * (r.y1 - r.y0));
    // Same ink either way, just turned.
    CHECK(std::abs(CountDarkPixels(upright) - CountDarkPixels(rotated)) < CountDarkPixels(upright) / 5 + 4);
}

void TestType3GlyphProcedureDraws() {
    // A Type 3 font whose one glyph is a filled unit square in a
    // 100-unit glyph space (FontMatrix 0.01): at 20pt it covers exactly
    // 20x20 device pixels from the text origin. Previously Type 3 fonts
    // fell through to a Liberation substitute keyed by /Differences
    // names ("sq" here, so: nothing drawn at all).
    std::string proc = "100 0 d0 0 0 100 100 re f";
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 100 50] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},
            {4, "<< /Type /Font /Subtype /Type3 /FontBBox [0 0 100 100] /FontMatrix [0.01 0 0 0.01 0 0] "
                "/CharProcs 5 0 R /Encoding << /Type /Encoding /Differences [65 /sq] >> /FirstChar 65 /LastChar 65 "
                "/Widths [100] >>"},
            {5, "<< /sq 6 0 R >>"},
            {6, "<< /Length " + std::to_string(proc.size()) + " >>\nstream\n" + proc + "\nendstream"},
        },
        1);
    Canvas canvas = RenderDoc(doc, MakeFontResources(4, "F1"), "BT /F1 20 Tf 10 10 Td (AA) Tj ET", 100, 50);
    DarkBox b = DarkBounds(canvas);
    // First glyph: user x 10..30, y 10..30 -> device y 20..40; the
    // second follows at the /Widths advance (100 glyph units = 1 em =
    // 20pt): x 30..50. Together: x 10..50.
    CHECK(b.x0 == 10 && b.x1 == 49);
    CHECK(b.y0 == 20 && b.y1 == 39);
    RGB inside = PixelAt(canvas, 20, 30);
    CHECK(inside.r < 10 && inside.g < 10 && inside.b < 10);
}

void TestType3D1GlyphIgnoresItsOwnColorAndUsesTextFill() {
    // A `d1` glyph is a stencil: its own `1 0 0 rg` must be ignored and
    // the text fill color (blue, set before BT) painted instead.
    std::string proc = "100 0 0 0 100 100 d1 1 0 0 rg 0 0 100 100 re f";
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 100 50] >>"},
            {3, "<< /Type /Page /Parent 2 0 R >>"},
            {4, "<< /Type /Font /Subtype /Type3 /FontBBox [0 0 100 100] /FontMatrix [0.01 0 0 0.01 0 0] "
                "/CharProcs 5 0 R /Encoding << /Type /Encoding /Differences [65 /sq] >> /FirstChar 65 /LastChar 65 "
                "/Widths [100] >>"},
            {5, "<< /sq 6 0 R >>"},
            {6, "<< /Length " + std::to_string(proc.size()) + " >>\nstream\n" + proc + "\nendstream"},
        },
        1);
    Canvas canvas = RenderDoc(doc, MakeFontResources(4, "F1"), "0 0 1 rg BT /F1 20 Tf 10 10 Td (A) Tj ET", 100, 50);
    RGB p = PixelAt(canvas, 20, 30);
    CHECK(p.r < 10 && p.g < 10 && p.b > 245);  // blue, not the glyph's own red
}

void TestInvisibleRenderModeDrawsNothing() {
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 100 50] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"},
        },
        1);
    Canvas canvas = RenderDoc(doc, MakeFontResources(4, "F1"), "BT /F1 24 Tf 3 Tr 10 10 Td (A) Tj ET", 100, 50);
    CHECK(CountDarkPixels(canvas) == 0);
}

void TestTdAdvancesBetweenGlyphs() {
    // Two separate Td-positioned single-character strings -- since Td
    // sets an absolute-from-line-start position (not a relative
    // per-call offset accumulated the way one might assume), the 2nd
    // Td here is relative to the (reset-at-BT) line matrix, giving 2
    // 'A's at x=10 and x=10+40=50. Confirms Td actually repositions
    // (and that text_line_matrix, not just text_matrix, is what Td
    // composes against).
    std::string content = "BT /F1 24 Tf 10 10 Td (A) Tj 40 0 Td (A) Tj ET";
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 100 50] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"},
        },
        1);
    Canvas canvas = RenderDoc(doc, MakeFontResources(4, "F1"), content, 100, 50);
    // Expect dark pixels in both a left region (x<30) and a right
    // region (x>=30) of the canvas -- 2 distinct glyph placements, not
    // one glyph or both stacked on top of each other.
    bool left = false, right = false;
    for (int y = 0; y < canvas.height && !(left && right); ++y) {
        for (int x = 0; x < canvas.width; ++x) {
            if (PixelAt(canvas, x, y).r < 128) {
                if (x < 30) left = true;
                if (x >= 30) right = true;
            }
        }
    }
    CHECK(left && right);
}

void TestCharSpacingIncreasesAdvance() {
    // Same content, once with Tc 0 and once with a large Tc -- the
    // large-Tc version's 2nd glyph must land further right, confirmed
    // by comparing the rightmost dark-pixel column between the two
    // renders rather than assuming exact pixel positions.
    auto rightmost_dark_x = [](const Canvas &c) {
        int rightmost = -1;
        for (int y = 0; y < c.height; ++y) {
            for (int x = 0; x < c.width; ++x) {
                if (PixelAt(c, x, y).r < 128) rightmost = std::max(rightmost, x);
            }
        }
        return rightmost;
    };
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 150 50] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"},
        },
        1);
    Canvas normal = RenderDoc(doc, MakeFontResources(4, "F1"), "BT /F1 24 Tf 10 10 Td (AA) Tj ET", 150, 50);
    Canvas spaced = RenderDoc(doc, MakeFontResources(4, "F1"), "BT /F1 24 Tf 20 Tc 10 10 Td (AA) Tj ET", 150, 50);
    CHECK(rightmost_dark_x(spaced) > rightmost_dark_x(normal));
}

void TestTJArrayAdjustmentShiftsGlyph() {
    // A large positive TJ number between two glyphs shifts the 2nd
    // glyph LEFT (spec: subtracted from the advance) relative to no
    // adjustment -- confirmed via the same rightmost-dark-pixel
    // comparison technique.
    auto rightmost_dark_x = [](const Canvas &c) {
        int rightmost = -1;
        for (int y = 0; y < c.height; ++y) {
            for (int x = 0; x < c.width; ++x) {
                if (PixelAt(c, x, y).r < 128) rightmost = std::max(rightmost, x);
            }
        }
        return rightmost;
    };
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 150 50] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"},
        },
        1);
    Canvas normal = RenderDoc(doc, MakeFontResources(4, "F1"), "BT /F1 24 Tf 10 10 Td [(A)(A)] TJ ET", 150, 50);
    Canvas shifted = RenderDoc(doc, MakeFontResources(4, "F1"), "BT /F1 24 Tf 10 10 Td [(A)500(A)] TJ ET", 150, 50);
    CHECK(rightmost_dark_x(shifted) < rightmost_dark_x(normal));
}

void TestTStarUsesLeading() {
    std::string content = "BT /F1 24 Tf 20 TL 10 40 Td (A) Tj T* (A) Tj ET";
    std::string doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 100 50] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"},
        },
        1);
    Canvas canvas = RenderDoc(doc, MakeFontResources(4, "F1"), content, 100, 50);
    // T* moves down by `leading` in TEXT space, i.e. UP in device space
    // (y-flip) by 20 user-space units -- expect dark pixels in both an
    // upper (device y < 20) and lower (device y >= 20) band.
    bool upper = false, lower = false;
    for (int y = 0; y < canvas.height && !(upper && lower); ++y) {
        for (int x = 0; x < canvas.width; ++x) {
            if (PixelAt(canvas, x, y).r < 128) {
                if (y < 20) upper = true;
                if (y >= 20) lower = true;
            }
        }
    }
    CHECK(upper && lower);
}

// PDFIUM_REMOVAL_PLAN.md Phase 12: end-to-end encryption coverage lives
// here (not pdf_crypt_test.cpp, which stays deliberately dependency-
// light -- pdf_object.cpp only, pure primitive/key-derivation vector
// tests) since it needs the full real pipeline this file already links:
// PdfDocument::Load -> GetPageContent -> pdfxref::ResolveStream (where
// Phase 12's decryption hook actually lives) -> RenderContentStream.
// Deliberately does NOT use this file's own RenderDoc helper above --
// RenderDoc renders a content string handed to it directly and only
// touches the xref table to resolve Form/Image XObject references, so
// none of its existing callers (including TestFormXObject) actually
// exercise resolving the PAGE's own /Contents stream through
// ResolveStream at all, which is exactly the step that needs decrypting
// here.
std::string HexDecode(const std::string &hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        out.push_back(static_cast<char>(static_cast<unsigned char>((nibble(hex[i]) << 4) | nibble(hex[i + 1]))));
    }
    return out;
}

std::string HexEncode(const std::string &data) {
    static const char *kDigits = "0123456789ABCDEF";
    std::string out;
    out.reserve(data.size() * 2);
    for (char ch : data) {
        unsigned char c = static_cast<unsigned char>(ch);
        out.push_back(kDigits[c >> 4]);
        out.push_back(kDigits[c & 0xF]);
    }
    return out;
}

// Independently reproduces Algorithm 2 (key derivation) + Algorithm 6's
// /U computation for R>=3, using only pdf_crypt.h's own already-vector-
// verified Md5/Rc4 -- the same computation SetupStandardSecurityHandler
// performs internally, so this test's real value is confirming the
// *pipeline* wiring (ResolveStream actually decrypting the right
// object with the right key), not re-deriving Algorithm 2/6 a second,
// differently-buggy way (pdf_crypt_test.cpp's own
// TestSetupStandardSecurityHandlerR3Rc4 already covers that algorithm
// in isolation).
struct Rc4EncryptionFixture {
    std::string o, u, id0, file_key;
    long long p = 0;
};

Rc4EncryptionFixture MakeRc4Fixture() {
    Rc4EncryptionFixture f;
    f.id0 = "0123456789ABCDEF";
    f.p = -4;
    int key_len_bytes = 16;
    f.o.resize(32);
    for (size_t i = 0; i < 32; ++i) f.o[i] = static_cast<char>(static_cast<unsigned char>(i + 1));

    std::string pad(
        "\x28\xBF\x4E\x5E\x4E\x75\x8A\x41\x64\x00\x4E\x56\xFF\xFA\x01\x08"
        "\x2E\x2E\x00\xB6\xD0\x68\x3E\x80\x2F\x0C\xA9\xFE\x64\x53\x69\x7A",
        32);
    unsigned char p_bytes[4] = {
        static_cast<unsigned char>(f.p & 0xFF),
        static_cast<unsigned char>((f.p >> 8) & 0xFF),
        static_cast<unsigned char>((f.p >> 16) & 0xFF),
        static_cast<unsigned char>((f.p >> 24) & 0xFF),
    };
    std::string digest = pdfcrypt::Md5(pad + f.o + std::string(reinterpret_cast<char *>(p_bytes), 4) + f.id0);
    for (int i = 0; i < 50; ++i) digest = pdfcrypt::Md5(digest.substr(0, static_cast<size_t>(key_len_bytes)));
    f.file_key = digest.substr(0, static_cast<size_t>(key_len_bytes));

    std::string enc16 = pdfcrypt::Rc4(f.file_key, pdfcrypt::Md5(pad + f.id0));
    for (int i = 1; i <= 19; ++i) {
        std::string key_i = f.file_key;
        for (char &c : key_i) c = static_cast<char>(static_cast<unsigned char>(c) ^ static_cast<unsigned char>(i));
        enc16 = pdfcrypt::Rc4(key_i, enc16);
    }
    f.u = enc16 + std::string(16, '\0');
    return f;
}

// Algorithm 1: per-object key for (num, gen=0), RC4 (no AES "sAlT" suffix).
std::string Rc4ObjectKey(const std::string &file_key, int num) {
    std::string input = file_key;
    unsigned char extra[5] = {static_cast<unsigned char>(num & 0xFF), static_cast<unsigned char>((num >> 8) & 0xFF),
                               static_cast<unsigned char>((num >> 16) & 0xFF), 0, 0};
    input.append(reinterpret_cast<char *>(extra), 5);
    return pdfcrypt::Md5(input).substr(0, std::min(file_key.size() + 5, static_cast<size_t>(16)));
}

std::string EncryptTrailerExtra(const Rc4EncryptionFixture &f) {
    return " /Encrypt 5 0 R /ID [<" + HexEncode(f.id0) + "> <" + HexEncode(f.id0) + ">]";
}

std::string EncryptDictObject(const Rc4EncryptionFixture &f) {
    return "<< /Filter /Standard /V 2 /R 3 /O <" + HexEncode(f.o) + "> /U <" + HexEncode(f.u) + "> /P " +
           std::to_string(f.p) + " /Length 128 >>";
}

// V4/R4 with a single /StdCF crypt filter using AESV2 for both streams
// and strings -- Algorithm 2/6's own key derivation is identical to the
// V2/R3 RC4 case above (R>=3 uses the same 50-round stretch and 19-round
// /U computation regardless of which CFM the /CF dict names), so
// Rc4EncryptionFixture's file_key/o/u/id0/p are reused as-is here; only
// the dict's /V/R/CF/StmF/StrF entries and the per-object key's AES
// "sAlT" suffix differ.
std::string EncryptDictObjectAesV2(const Rc4EncryptionFixture &f) {
    return "<< /Filter /Standard /V 4 /R 4 /O <" + HexEncode(f.o) + "> /U <" + HexEncode(f.u) + "> /P " +
           std::to_string(f.p) +
           " /Length 128 /CF << /StdCF << /CFM /AESV2 /Length 16 >> >> /StmF /StdCF /StrF /StdCF >>";
}

void TestEncryptedRc4DocumentRendersIdenticallyToUnencrypted() {
    std::string plain_content = "1 0 0 rg 0 0 10 10 re f";  // a red square, same shape TestFormXObject already uses

    std::string plain_doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 20 20] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>"},
            {4, "<< /Length " + std::to_string(plain_content.size()) + " >>\nstream\n" + plain_content +
                    "\nendstream"},
        },
        1);
    pdfdoc::PdfDocument plain_pdfdoc;
    plain_pdfdoc.Load(reinterpret_cast<const unsigned char *>(plain_doc.data()), plain_doc.size());
    const pdfdoc::Page *plain_page = plain_pdfdoc.GetPage(0);
    CHECK(plain_page != nullptr);
    std::string resolved_plain = pdfrender::GetPageContent(reinterpret_cast<const unsigned char *>(plain_doc.data()),
                                                             plain_doc.size(), plain_pdfdoc.Xref(), *plain_page);
    CHECK(resolved_plain == plain_content);
    pdfobj::Object empty_resources;
    Canvas plain_canvas = Canvas::MakeWhite(20, 20);
    pdfrender::RenderContentStream(resolved_plain, plain_canvas, IdentityPageMatrix(20, 20), empty_resources,
                                    reinterpret_cast<const unsigned char *>(plain_doc.data()), plain_doc.size(),
                                    plain_pdfdoc.Xref());

    Rc4EncryptionFixture f = MakeRc4Fixture();
    std::string encrypted_content = pdfcrypt::Rc4(Rc4ObjectKey(f.file_key, 4), plain_content);
    std::string enc_doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 20 20] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>"},
            {4, "<< /Length " + std::to_string(encrypted_content.size()) + " >>\nstream\n" + encrypted_content +
                    "\nendstream"},
            {5, EncryptDictObject(f)},
        },
        1, EncryptTrailerExtra(f));

    const unsigned char *enc_data = reinterpret_cast<const unsigned char *>(enc_doc.data());
    pdfxref::XrefTable enc_table;
    enc_table.Load(enc_data, enc_doc.size());
    CHECK(enc_table.IsEncrypted());
    CHECK(enc_table.Encryption() != nullptr);  // empty password authenticated

    pdfdoc::PdfDocument enc_pdfdoc;
    enc_pdfdoc.Load(enc_data, enc_doc.size());
    CHECK(enc_pdfdoc.PageCount() == 1);
    const pdfdoc::Page *enc_page = enc_pdfdoc.GetPage(0);
    CHECK(enc_page != nullptr);

    std::string resolved_enc = pdfrender::GetPageContent(enc_data, enc_doc.size(), enc_pdfdoc.Xref(), *enc_page);
    CHECK(resolved_enc == plain_content);  // decryption recovered the exact original content-stream bytes

    Canvas enc_canvas = Canvas::MakeWhite(20, 20);
    pdfrender::RenderContentStream(resolved_enc, enc_canvas, IdentityPageMatrix(20, 20), empty_resources, enc_data,
                                    enc_doc.size(), enc_pdfdoc.Xref());
    CHECK(enc_canvas.rgba == plain_canvas.rgba);  // byte-identical render to the unencrypted source
}

// AES-128-CBC (V4/R4/AESV2) sibling of the RC4 test above -- the
// ciphertext-plus-IV bytes below were generated once, out of band, via
// `openssl enc -aes-128-cbc` as an independent oracle (see this phase's
// own PDFIUM_REMOVAL_PLAN.md writeup for the exact key/IV/invocation
// used), keyed with the per-object AES key Algorithm 1 (with its
// AESV2-only "sAlT" suffix) derives from this same fixture's file_key
// for object (num=4, gen=0) -- this module never implements AES
// *encryption* itself (pdf_crypt.h only ever needs to decrypt), so
// there's no in-process way to generate this ciphertext the way the RC4
// sibling test above generates its own (RC4 is symmetric).
void TestEncryptedAesV2DocumentRendersIdenticallyToUnencrypted() {
    std::string plain_content = "1 0 0 rg 0 0 10 10 re f";

    std::string plain_doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 20 20] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>"},
            {4, "<< /Length " + std::to_string(plain_content.size()) + " >>\nstream\n" + plain_content +
                    "\nendstream"},
        },
        1);
    pdfdoc::PdfDocument plain_pdfdoc;
    plain_pdfdoc.Load(reinterpret_cast<const unsigned char *>(plain_doc.data()), plain_doc.size());
    const pdfdoc::Page *plain_page = plain_pdfdoc.GetPage(0);
    CHECK(plain_page != nullptr);
    pdfobj::Object empty_resources;
    Canvas plain_canvas = Canvas::MakeWhite(20, 20);
    pdfrender::RenderContentStream(plain_content, plain_canvas, IdentityPageMatrix(20, 20), empty_resources,
                                    reinterpret_cast<const unsigned char *>(plain_doc.data()), plain_doc.size(),
                                    plain_pdfdoc.Xref());

    Rc4EncryptionFixture f = MakeRc4Fixture();
    // IV (16 bytes) || AES-128-CBC-PKCS7(plain_content) under the
    // AESV2 object key for (num=4, gen=0) derived from this fixture's
    // own file_key -- see this test's own top comment.
    std::string iv_and_ciphertext = HexDecode(
        "000102030405060708090a0b0c0d0e0f"
        "c43c0f7dd1236297ead1eb8ef4d5764908799e60f1a3a39be72d6791667eeff1");
    std::string enc_doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 20 20] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>"},
            {4, "<< /Length " + std::to_string(iv_and_ciphertext.size()) + " >>\nstream\n" + iv_and_ciphertext +
                    "\nendstream"},
            {5, EncryptDictObjectAesV2(f)},
        },
        1, EncryptTrailerExtra(f));

    const unsigned char *enc_data = reinterpret_cast<const unsigned char *>(enc_doc.data());
    pdfxref::XrefTable enc_table;
    enc_table.Load(enc_data, enc_doc.size());
    CHECK(enc_table.IsEncrypted());
    CHECK(enc_table.Encryption() != nullptr);
    CHECK(enc_table.Encryption()->stream_method == pdfcrypt::CryptMethod::kAesV2);

    pdfdoc::PdfDocument enc_pdfdoc;
    enc_pdfdoc.Load(enc_data, enc_doc.size());
    CHECK(enc_pdfdoc.PageCount() == 1);
    const pdfdoc::Page *enc_page = enc_pdfdoc.GetPage(0);
    CHECK(enc_page != nullptr);

    std::string resolved_enc = pdfrender::GetPageContent(enc_data, enc_doc.size(), enc_pdfdoc.Xref(), *enc_page);
    CHECK(resolved_enc == plain_content);  // AES-CBC decryption + PKCS#7 unpad recovered the exact original bytes

    Canvas enc_canvas = Canvas::MakeWhite(20, 20);
    pdfrender::RenderContentStream(resolved_enc, enc_canvas, IdentityPageMatrix(20, 20), empty_resources, enc_data,
                                    enc_doc.size(), enc_pdfdoc.Xref());
    CHECK(enc_canvas.rgba == plain_canvas.rgba);
}

void TestWrongPasswordDocumentReportsZeroPages() {
    std::string plain_content = "1 0 0 rg 0 0 10 10 re f";
    Rc4EncryptionFixture f = MakeRc4Fixture();
    std::string encrypted_content = pdfcrypt::Rc4(Rc4ObjectKey(f.file_key, 4), plain_content);

    // Tamper /U so the empty password no longer authenticates -- the
    // overwhelmingly common real-world reason /U wouldn't match: the
    // document actually has a real, non-empty user password, which this
    // module has no way to prompt for or supply (matches PDFium's own
    // FPDF_ERR_PASSWORD outcome for the same situation).
    Rc4EncryptionFixture bad_f = f;
    bad_f.u[0] = static_cast<char>(static_cast<unsigned char>(bad_f.u[0]) ^ 0xFF);

    std::string bad_doc = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /MediaBox [0 0 20 20] >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>"},
            {4, "<< /Length " + std::to_string(encrypted_content.size()) + " >>\nstream\n" + encrypted_content +
                    "\nendstream"},
            {5, EncryptDictObject(bad_f)},
        },
        1, EncryptTrailerExtra(bad_f));

    const unsigned char *bad_data = reinterpret_cast<const unsigned char *>(bad_doc.data());
    pdfxref::XrefTable bad_table;
    bad_table.Load(bad_data, bad_doc.size());
    CHECK(bad_table.IsEncrypted());
    CHECK(bad_table.Encryption() == nullptr);

    pdfdoc::PdfDocument bad_pdfdoc;
    bad_pdfdoc.Load(bad_data, bad_doc.size());
    CHECK(bad_pdfdoc.PageCount() == 0);
}

}  // namespace

int main() {
    TestPageToDeviceMatrixCorners();
    TestFillRectangleKnownColor();
    TestGraphicsStateStackIsolation();
    TestEvenOddVsNonZeroThroughOperators();
    TestCmykConversion();
    TestClipRestrictsFill();
    TestStrokeLine();
    TestExtGStateAlphaBlends();
    TestExtGStateAlphaViaResources();
    TestIndexedColorSpace();
    TestFormXObject();
    TestImageXObjectRawRGB();
    TestImageXObjectFlateDecode();
    TestImageMask();
    TestIndexedImageXObject();
    TestSMaskAlpha();
    TestInlineImageUnfiltered();
    TestDctImageXObject();
    TestBasicTextShowing();
    TestRotatedTextRendersRotated();
    TestType3GlyphProcedureDraws();
    TestType3D1GlyphIgnoresItsOwnColorAndUsesTextFill();
    TestInvisibleRenderModeDrawsNothing();
    TestTdAdvancesBetweenGlyphs();
    TestCharSpacingIncreasesAdvance();
    TestTJArrayAdjustmentShiftsGlyph();
    TestTStarUsesLeading();
    TestEncryptedRc4DocumentRendersIdenticallyToUnencrypted();
    TestEncryptedAesV2DocumentRendersIdenticallyToUnencrypted();
    TestWrongPasswordDocumentReportsZeroPages();
    std::printf("pdf_content_test: all checks passed\n");
    return 0;
}
