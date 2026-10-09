#include "svg_raster.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "gfx/rasterizer.h"
#include "gfx/truetype.h"
#include "html_doc.h"
#include "svg_doc.h"

namespace svg_raster {
namespace {

namespace raster = gfx::raster;

constexpr float kPi = 3.14159265358979323846f;

const DomNode *FindSvg(const DomNode &node) {
    if (node.type == DomNodeType::Element && node.tag == "svg") return &node;
    for (const auto &child : node.children)
        if (const DomNode *found = FindSvg(*child)) return found;
    return nullptr;
}

// The first kilobytes, lowercased: enough to see the root element past an
// XML declaration, a doctype and a licence comment.
std::string Head(const unsigned char *bytes, size_t len) {
    std::string head(reinterpret_cast<const char *>(bytes), std::min<size_t>(len, 4096));
    std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return head;
}

// The parsed document and its <svg> root, or null.
const DomNode *ParseSvg(const unsigned char *bytes, size_t len, HtmlDoc &doc) {
    if (!LooksLikeSvg(bytes, len)) return nullptr;
    ParseHtml(std::string(reinterpret_cast<const char *>(bytes), len), doc, /*full_document=*/true,
              /*compute_styles=*/false);
    return doc.root ? FindSvg(*doc.root) : nullptr;
}

// A premultiplied-alpha float canvas the shapes are composited onto with
// source-over, one coverage mask at a time.
struct Canvas {
    int w = 0, h = 0;
    std::vector<float> px;  // r, g, b, a premultiplied, row-major

    void Paint(const std::vector<unsigned char> &coverage, int x0, int y0, int cw, int ch, const SvgPaint &paint) {
        const float a = static_cast<float>(paint.a) / 255.0f;
        const float r = static_cast<float>(paint.r) / 255.0f, g = static_cast<float>(paint.g) / 255.0f,
                    b = static_cast<float>(paint.b) / 255.0f;
        for (int y = 0; y < ch; ++y) {
            const int py = y0 + y;
            if (py < 0 || py >= h) continue;
            for (int x = 0; x < cw; ++x) {
                const int pxx = x0 + x;
                if (pxx < 0 || pxx >= w) continue;
                const unsigned char c = coverage[static_cast<size_t>(y) * static_cast<size_t>(cw) + static_cast<size_t>(x)];
                if (!c) continue;
                const float sa = a * static_cast<float>(c) / 255.0f;
                float *d = &px[(static_cast<size_t>(py) * static_cast<size_t>(w) + static_cast<size_t>(pxx)) * 4];
                d[0] = r * sa + d[0] * (1.0f - sa);
                d[1] = g * sa + d[1] * (1.0f - sa);
                d[2] = b * sa + d[2] * (1.0f - sa);
                d[3] = sa + d[3] * (1.0f - sa);
            }
        }
    }

    // Fills `edges` (raster space) with `paint`, rasterizing only the
    // window their bounds cover.
    void Fill(std::vector<raster::Edge> &edges, const SvgPaint &paint) {
        if (edges.empty() || !paint.present || paint.a == 0) return;
        float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
        for (const raster::Edge &e : edges) {
            const float xa = e.x_at_ymin, xb = e.x_at_ymin + e.dxdy * (e.ymax - e.ymin);
            minx = std::min({minx, xa, xb});
            maxx = std::max({maxx, xa, xb});
            miny = std::min(miny, e.ymin);
            maxy = std::max(maxy, e.ymax);
        }
        const int x0 = std::max(0, static_cast<int>(std::floor(minx)));
        const int y0 = std::max(0, static_cast<int>(std::floor(miny)));
        const int x1 = std::min(w, static_cast<int>(std::ceil(maxx)) + 1);
        const int y1 = std::min(h, static_cast<int>(std::ceil(maxy)) + 1);
        if (x1 <= x0 || y1 <= y0) return;
        const std::vector<unsigned char> cov = raster::RasterizeRegion(edges, x0, y0, x1 - x0, y1 - y0);
        Paint(cov, x0, y0, x1 - x0, y1 - y0, paint);
    }
};

// Appends a closed polygon's edges wound clockwise on screen (negative
// shoelace area in y-down space), so pieces added together -- a stroke's
// segments and joins -- union under the nonzero rule instead of a
// reversed piece cancelling an overlap out.
void AddPolygon(std::vector<raster::Edge> &edges, std::vector<float> pts) {
    const size_t n = pts.size() / 2;
    if (n < 3) return;
    float area = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        area += pts[2 * i] * pts[2 * j + 1] - pts[2 * j] * pts[2 * i + 1];
    }
    if (area > 0.0f)
        for (size_t i = 0; i < n / 2; ++i) {
            std::swap(pts[2 * i], pts[2 * (n - 1 - i)]);
            std::swap(pts[2 * i + 1], pts[2 * (n - 1 - i) + 1]);
        }
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        raster::AddLine(edges, pts[2 * i], pts[2 * i + 1], pts[2 * j], pts[2 * j + 1]);
    }
}

// A stroke's outline: a rectangle per segment and a round join at each
// vertex between two (and all around a closed outline). Open ends are
// butt, SVG's default cap.
std::vector<raster::Edge> StrokeEdges(const std::vector<float> &pts, bool closed, float width) {
    std::vector<raster::Edge> edges;
    const size_t n = pts.size() / 2;
    const float hw = std::max(width, 0.0f) * 0.5f;
    if (n < 2 || hw <= 0.0f) return edges;
    const size_t segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i) {
        const size_t j = (i + 1) % n;
        const float x0 = pts[2 * i], y0 = pts[2 * i + 1], x1 = pts[2 * j], y1 = pts[2 * j + 1];
        const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0f) continue;
        const float nx = -dy / len * hw, ny = dx / len * hw;
        AddPolygon(edges, {x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny});
    }
    // Joins only where they show: a hairline's joins are below a pixel.
    if (hw >= 0.75f) {
        const int sides = std::max(8, std::min(32, static_cast<int>(hw * 4.0f)));
        for (size_t i = 0; i < n; ++i) {
            if (!closed && (i == 0 || i == n - 1)) continue;
            std::vector<float> disc;
            for (int k = 0; k < sides; ++k) {
                const float t = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
                disc.push_back(pts[2 * i] + hw * std::cos(t));
                disc.push_back(pts[2 * i + 1] + hw * std::sin(t));
            }
            AddPolygon(edges, std::move(disc));
        }
    }
    return edges;
}

// UTF-8 to codepoints; a malformed byte stands for itself.
std::vector<int> Codepoints(const std::string &s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + static_cast<size_t>(len) > s.size()) len = 1;
        int cp = len == 1 ? c : c & (0x7f >> len);
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3f);
        out.push_back(cp);
        i += static_cast<size_t>(len);
    }
    return out;
}

// One glyph of a text shape, rasterized: its coverage and where its top-left
// lands, relative to the shape's anchor point.
struct PlacedGlyph {
    std::vector<unsigned char> coverage;
    int x = 0, y = 0, w = 0, h = 0;
};

// Sets `shape`'s text along its baseline (rotated by shape.rotation),
// glyph by glyph, relative to its anchor point.
std::vector<PlacedGlyph> LayoutText(const gfx::tt::FontInfo &font, const SvgShape &shape) {
    std::vector<PlacedGlyph> out;
    if (shape.font_size <= 0.0f) return out;
    const float scale = shape.font_size / static_cast<float>(std::max(1, font.units_per_em));
    const std::vector<int> cps = Codepoints(shape.text);
    float advance = 0.0f;
    for (int cp : cps) {
        int aw = 0, lsb = 0;
        gfx::tt::GetCodepointHMetrics(&font, cp, &aw, &lsb);
        advance += static_cast<float>(aw) * scale;
    }
    float pen = 0.0f;
    if (shape.text_anchor == "middle") pen -= advance * 0.5f;
    else if (shape.text_anchor == "end") pen -= advance;
    const float cs = std::cos(shape.rotation), sn = std::sin(shape.rotation);
    for (int cp : cps) {
        int gw = 0, gh = 0, xoff = 0, yoff = 0;
        // Font units (y up) to screen pixels (y down), turned by the rotation.
        unsigned char *bitmap =
            shape.rotation == 0.0f
                ? gfx::tt::GetCodepointBitmap(&font, scale, scale, cp, &gw, &gh, &xoff, &yoff)
                : gfx::tt::GetGlyphBitmapMatrix(&font, scale * cs, scale * sn, scale * sn, -scale * cs,
                                                gfx::tt::FindGlyphIndex(&font, cp), &gw, &gh, &xoff, &yoff);
        if (bitmap) {
            PlacedGlyph g;
            g.coverage.assign(bitmap, bitmap + static_cast<size_t>(gw) * static_cast<size_t>(gh));
            g.x = static_cast<int>(std::lround(pen * cs)) + xoff;
            g.y = static_cast<int>(std::lround(pen * sn)) + yoff;
            g.w = gw;
            g.h = gh;
            out.push_back(std::move(g));
            gfx::tt::FreeBitmap(bitmap);
        }
        int aw = 0, lsb = 0;
        gfx::tt::GetCodepointHMetrics(&font, cp, &aw, &lsb);
        pen += static_cast<float>(aw) * scale;
    }
    return out;
}

void DrawText(Canvas &canvas, const gfx::tt::FontInfo &font, const SvgShape &shape) {
    if (shape.points.size() < 2) return;
    const int ax = static_cast<int>(std::lround(shape.points[0])), ay = static_cast<int>(std::lround(shape.points[1]));
    for (const PlacedGlyph &g : LayoutText(font, shape)) canvas.Paint(g.coverage, ax + g.x, ay + g.y, g.w, g.h, shape.fill);
}

// The canvas's premultiplied floats as straight-alpha RGBA8.
std::vector<unsigned char> ToRgba(const Canvas &canvas) {
    std::vector<unsigned char> rgba(canvas.px.size(), 0);
    for (size_t i = 0; i < canvas.px.size(); i += 4) {
        const float a = canvas.px[i + 3];
        if (a <= 0.0f) continue;
        for (int c = 0; c < 3; ++c)
            rgba[i + static_cast<size_t>(c)] =
                static_cast<unsigned char>(std::clamp(canvas.px[i + static_cast<size_t>(c)] / a * 255.0f + 0.5f, 0.0f, 255.0f));
        rgba[i + 3] = static_cast<unsigned char>(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
    }
    return rgba;
}

}  // namespace

bool LooksLikeSvg(const unsigned char *bytes, size_t len) {
    if (!bytes || len == 0) return false;
    const std::string head = Head(bytes, len);
    const size_t svg = head.find("<svg");
    if (svg == std::string::npos) return false;
    // Nothing but a prolog before it: the XML declaration, a doctype,
    // comments, whitespace (an HTML page that merely contains an <svg>
    // is not an SVG file).
    const size_t html = head.find("<html");
    return html == std::string::npos || html > svg;
}

bool Dimensions(const unsigned char *bytes, size_t len, int *width, int *height) {
    HtmlDoc doc;
    const DomNode *svg = ParseSvg(bytes, len, doc);
    float w = 0.0f, h = 0.0f;
    if (!svg || !SvgIntrinsicSize(*svg, w, h) || w < 1.0f || h < 1.0f) return false;
    *width = static_cast<int>(std::lround(w));
    *height = static_cast<int>(std::lround(h));
    return true;
}

bool Rasterize(const unsigned char *bytes, size_t len, std::vector<unsigned char> *rgba, int *width, int *height,
               std::string *error, const unsigned char *font_ttf, size_t font_len, int max_side) {
    HtmlDoc doc;
    const DomNode *svg = ParseSvg(bytes, len, doc);
    if (!svg) {
        if (error) *error = "not an SVG document";
        return false;
    }
    float w = 0.0f, h = 0.0f;
    if (!SvgIntrinsicSize(*svg, w, h)) {
        w = 300.0f;  // the replaced-element default svg_doc.h names
        h = 150.0f;
    }
    if (w < 1.0f || h < 1.0f) {
        if (error) *error = "the SVG has no size";
        return false;
    }
    const float fit = std::min(1.0f, static_cast<float>(std::max(1, max_side)) / std::max(w, h));
    Canvas canvas;
    canvas.w = std::max(1, static_cast<int>(std::lround(w * fit)));
    canvas.h = std::max(1, static_cast<int>(std::lround(h * fit)));
    canvas.px.assign(static_cast<size_t>(canvas.w) * static_cast<size_t>(canvas.h) * 4, 0.0f);

    gfx::tt::FontInfo font;
    const bool have_font =
        font_ttf && font_len > 0 && gfx::tt::InitFont(&font, font_ttf, static_cast<int>(font_len));

    const SvgDisplayList list =
        BuildSvgDisplayList(*svg, static_cast<float>(canvas.w), static_cast<float>(canvas.h));
    for (const SvgShape &shape : list.shapes) {
        if (shape.kind == SvgShape::Kind::Text) {
            if (have_font) DrawText(canvas, font, shape);
            continue;
        }
        if (shape.kind == SvgShape::Kind::Polygon && shape.fill.present) {
            std::vector<raster::Edge> edges;
            const size_t n = shape.points.size() / 2;
            for (size_t i = 0; i < n; ++i) {
                const size_t j = (i + 1) % n;
                raster::AddLine(edges, shape.points[2 * i], shape.points[2 * i + 1], shape.points[2 * j],
                                shape.points[2 * j + 1]);
            }
            canvas.Fill(edges, shape.fill);
        }
        if (shape.stroke.present) {
            const bool closed = shape.kind == SvgShape::Kind::Polygon || shape.closed;
            std::vector<raster::Edge> edges = StrokeEdges(shape.points, closed, shape.stroke_width);
            canvas.Fill(edges, shape.stroke);
        }
    }

    *rgba = ToRgba(canvas);
    *width = canvas.w;
    *height = canvas.h;
    return true;
}

bool RasterizeText(const SvgShape &shape, const unsigned char *font_ttf, size_t font_len, std::vector<unsigned char> *rgba,
                   int *width, int *height, int *left, int *top) {
    gfx::tt::FontInfo font;
    if (!font_ttf || font_len == 0 || !gfx::tt::InitFont(&font, font_ttf, static_cast<int>(font_len))) return false;
    const std::vector<PlacedGlyph> glyphs = LayoutText(font, shape);
    if (glyphs.empty()) return false;
    int x0 = glyphs[0].x, y0 = glyphs[0].y, x1 = x0, y1 = y0;
    for (const PlacedGlyph &g : glyphs) {
        x0 = std::min(x0, g.x);
        y0 = std::min(y0, g.y);
        x1 = std::max(x1, g.x + g.w);
        y1 = std::max(y1, g.y + g.h);
    }
    Canvas canvas;
    canvas.w = std::max(1, x1 - x0);
    canvas.h = std::max(1, y1 - y0);
    canvas.px.assign(static_cast<size_t>(canvas.w) * static_cast<size_t>(canvas.h) * 4, 0.0f);
    for (const PlacedGlyph &g : glyphs) canvas.Paint(g.coverage, g.x - x0, g.y - y0, g.w, g.h, shape.fill);
    *rgba = ToRgba(canvas);
    *width = canvas.w;
    *height = canvas.h;
    *left = x0;
    *top = y0;
    return true;
}

}  // namespace svg_raster
