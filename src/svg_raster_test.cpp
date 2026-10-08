// svg_raster: an SVG file drawn into pixels, checked pixel by pixel.
#include "svg_raster.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "font_data.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char *expression, int line) {
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    ++g_failures;
}
#define CHECK(x) Check((x), #x, __LINE__)

const unsigned char *Bytes(const std::string &s) { return reinterpret_cast<const unsigned char *>(s.data()); }

struct Picture {
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    bool ok = false;
    const unsigned char *At(int x, int y) const {
        return &rgba[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4];
    }
    bool Is(int x, int y, int r, int g, int b, int a = 255) const {
        const unsigned char *p = At(x, y);
        auto near = [](int v, int want) { return v >= want - 2 && v <= want + 2; };
        return near(p[0], r) && near(p[1], g) && near(p[2], b) && near(p[3], a);
    }
};

Picture Draw(const std::string &svg, bool font = false, int max_side = 4096) {
    Picture p;
    std::string err;
    p.ok = svg_raster::Rasterize(Bytes(svg), svg.size(), &p.rgba, &p.w, &p.h, &err,
                                 font ? kJetBrainsMonoRegularTtf : nullptr, font ? sizeof kJetBrainsMonoRegularTtf : 0,
                                 max_side);
    return p;
}

void TestSniffing() {
    CHECK(svg_raster::LooksLikeSvg(Bytes(std::string("<svg xmlns='http://www.w3.org/2000/svg'/>")), 41));
    const std::string prolog =
        "<?xml version=\"1.0\"?>\n<!DOCTYPE svg>\n<!-- licence -->\n<svg width=\"10\" height=\"10\"></svg>";
    CHECK(svg_raster::LooksLikeSvg(Bytes(prolog), prolog.size()));
    const std::string page = "<html><body><svg width=\"10\" height=\"10\"></svg></body></html>";
    CHECK(!svg_raster::LooksLikeSvg(Bytes(page), page.size()));
    const unsigned char png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    CHECK(!svg_raster::LooksLikeSvg(png, sizeof png));

    int w = 0, h = 0;
    CHECK(svg_raster::Dimensions(Bytes(prolog), prolog.size(), &w, &h) && w == 10 && h == 10);
    const std::string boxed = "<svg viewBox=\"0 0 960 640\"></svg>";
    CHECK(svg_raster::Dimensions(Bytes(boxed), boxed.size(), &w, &h) && w == 960 && h == 640);
    CHECK(!svg_raster::Dimensions(Bytes(page), page.size(), &w, &h));
}

void TestShapes() {
    // A full-size background given as 100%, a red square, a stroked line
    // and a filled circle -- the vocabulary of a generated plot.
    const Picture p = Draw(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" viewBox=\"0 0 100 100\">"
        "<rect width=\"100%\" height=\"100%\" fill=\"rgb(255,255,255)\"/>"
        "<rect x=\"10\" y=\"10\" width=\"20\" height=\"20\" fill=\"#ff0000\"/>"
        "<line x1=\"40\" y1=\"50\" x2=\"90\" y2=\"50\" stroke=\"rgb(0,0,255)\" stroke-width=\"4\"/>"
        "<circle cx=\"70\" cy=\"80\" r=\"8\" fill=\"rgb(0,128,0)\"/>"
        "</svg>");
    CHECK(p.ok && p.w == 100 && p.h == 100);
    if (!p.ok) return;
    CHECK(p.Is(95, 95, 255, 255, 255));  // the 100% background reaches the far corner
    CHECK(p.Is(5, 5, 255, 255, 255));
    CHECK(p.Is(20, 20, 255, 0, 0));      // inside the square
    CHECK(p.Is(65, 50, 0, 0, 255));      // on the line
    CHECK(p.Is(65, 45, 255, 255, 255));  // clear of its 4px width
    CHECK(p.Is(70, 80, 0, 128, 0));      // the circle's centre
    CHECK(p.Is(70, 92, 255, 255, 255));  // outside it
}

void TestTransparency() {
    const Picture p = Draw("<svg width=\"20\" height=\"20\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"20\" fill=\"black\"/></svg>");
    CHECK(p.ok);
    if (!p.ok) return;
    CHECK(p.Is(5, 10, 0, 0, 0, 255));
    CHECK(p.At(15, 10)[3] == 0);  // nothing drawn there: transparent
}

void TestStyleSheet() {
    // A sheet rule beats a presentation attribute; inline style beats both;
    // a class rule beats a bare tag rule.
    const Picture p = Draw(
        "<svg width=\"30\" height=\"10\">"
        "<style>rect{fill:rgb(0,0,255);} .hot{fill:rgb(255,0,0)}</style>"
        "<rect x=\"0\" width=\"10\" height=\"10\" fill=\"black\"/>"
        "<rect x=\"10\" width=\"10\" height=\"10\" fill=\"black\" style=\"fill:rgb(0,255,0)\"/>"
        "<rect x=\"20\" width=\"10\" height=\"10\" class=\"hot\"/>"
        "</svg>");
    CHECK(p.ok);
    if (!p.ok) return;
    CHECK(p.Is(5, 5, 0, 0, 255));
    CHECK(p.Is(15, 5, 0, 255, 0));
    CHECK(p.Is(25, 5, 255, 0, 0));
}

int DarkPixels(const Picture &p, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (p.At(x, y)[3] > 128 && p.At(x, y)[0] < 100) ++n;
    return n;
}

void TestText() {
    const std::string svg =
        "<svg width=\"120\" height=\"60\"><text x=\"60\" y=\"45\" font-size=\"40\" text-anchor=\"middle\">MW</text></svg>";
    const Picture with = Draw(svg, true);
    CHECK(with.ok);
    if (!with.ok) return;
    // Glyph ink, centred on x = 60: on both sides of it, above the baseline.
    CHECK(DarkPixels(with, 30, 10, 60, 46) > 40);
    CHECK(DarkPixels(with, 60, 10, 90, 46) > 40);
    CHECK(DarkPixels(with, 0, 0, 120, 8) == 0);
    // No font: the text is left out, the rest still draws.
    const Picture without = Draw(svg, false);
    CHECK(without.ok && DarkPixels(without, 0, 0, 120, 60) == 0);
}

void TestMaxSide() {
    const Picture p = Draw("<svg width=\"2000\" height=\"1000\"><rect width=\"100%\" height=\"100%\" fill=\"red\"/></svg>",
                           false, 500);
    CHECK(p.ok && p.w == 500 && p.h == 250);
    if (p.ok) CHECK(p.Is(499, 249, 255, 0, 0));
    std::string err;
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    CHECK(!svg_raster::Rasterize(Bytes(std::string("hello")), 5, &rgba, &w, &h, &err) && !err.empty());
}

}  // namespace

int main() {
    TestSniffing();
    TestShapes();
    TestTransparency();
    TestStyleSheet();
    TestText();
    TestMaxSide();
    if (g_failures) {
        std::fprintf(stderr, "svg_raster_test: %d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("svg_raster_test: all checks passed\n");
    return EXIT_SUCCESS;
}
