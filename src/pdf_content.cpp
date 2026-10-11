#include "pdf_content.h"

#include <cstring>

#include "gfx/rasterizer.h"
#include "pdf_filters.h"
#include "pdf_font.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace pdfrender {

// -- Matrix ------------------------------------------------------------

Mat2D Multiply(const Mat2D &m1, const Mat2D &m2) {
    Mat2D r;
    r.a = m1.a * m2.a + m1.b * m2.c;
    r.b = m1.a * m2.b + m1.b * m2.d;
    r.c = m1.c * m2.a + m1.d * m2.c;
    r.d = m1.c * m2.b + m1.d * m2.d;
    r.e = m1.e * m2.a + m1.f * m2.c + m2.e;
    r.f = m1.e * m2.b + m1.f * m2.d + m2.f;
    return r;
}

void Transform(const Mat2D &m, double x, double y, double *out_x, double *out_y) {
    *out_x = m.a * x + m.c * y + m.e;
    *out_y = m.b * x + m.d * y + m.f;
}

// See pdf_content.h's own doc comment: derived by tracking where each
// corner of the unrotated [0,W]x[0,H] page lands after physically
// rotating the page `rotate` degrees clockwise, then flipping to device
// (y-down) space -- verified against pdftoppm's actual rendered output
// during this phase's own testing, not just derived on paper.
Mat2D PageToDeviceMatrix(double llx, double lly, double urx, double ury, int rotate, double px_per_pt) {
    double w = urx - llx, h = ury - lly;
    Mat2D m;
    if (rotate == 90) {
        m = Mat2D{0, 1, 1, 0, 0, 0};
    } else if (rotate == 180) {
        m = Mat2D{-1, 0, 0, 1, w, 0};
    } else if (rotate == 270) {
        m = Mat2D{0, -1, -1, 0, h, w};
    } else {
        m = Mat2D{1, 0, 0, -1, 0, h};
    }
    // Fold in the box-origin translation (old_x = user_x - llx, old_y =
    // user_y - lly) directly into e/f, then scale every coefficient by
    // px_per_pt (a uniform scale of the whole output applies to all 6).
    m.e = m.e - m.a * llx - m.c * lly;
    m.f = m.f - m.b * llx - m.d * lly;
    m.a *= px_per_pt;
    m.b *= px_per_pt;
    m.c *= px_per_pt;
    m.d *= px_per_pt;
    m.e *= px_per_pt;
    m.f *= px_per_pt;
    return m;
}

// -- Canvas --------------------------------------------------------------

Canvas Canvas::MakeWhite(int width, int height) {
    Canvas c;
    c.width = std::max(width, 1);
    c.height = std::max(height, 1);
    c.rgba.assign(static_cast<size_t>(c.width) * static_cast<size_t>(c.height) * 4, 255);
    return c;
}

namespace {

// -- Content-stream tokenizer ---------------------------------------------
// Reuses pdf_object.h's object lexer for every operand (numbers, names,
// strings, arrays, dicts -- the last needed for BI's inline image dict,
// skipped wholesale in this phase, see Do/BI handling below); a bare
// keyword (letters/`*`/`'`/`"`, e.g. "re", "f*", "Tj") is an operator.
// PDF operators are never `true`/`false`/`null` (those only ever appear
// as dict/array VALUES, which pdfobj::ParseObject already handles when
// parsing e.g. BI's dict), so treating every letter-led bare token as an
// operator unconditionally is safe -- notably including the single-
// letter "f" and "n" operators, which would otherwise collide with
// pdfobj's own `false`/`null` keyword recognition if this dispatched by
// content instead of by leading-character class.

bool IsWs(unsigned char c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
bool IsDelim(unsigned char c) {
    switch (c) {
        case '(':
        case ')':
        case '<':
        case '>':
        case '[':
        case ']':
        case '{':
        case '}':
        case '/':
        case '%':
            return true;
        default:
            return false;
    }
}

struct ContentToken {
    bool is_operator = false;
    pdfobj::Object operand;
    std::string op;
};

bool NextContentToken(const unsigned char *data, size_t len, size_t &pos, ContentToken *out) {
    pdfobj::SkipWhitespaceAndComments(data, len, pos);
    if (pos >= len) return false;
    unsigned char c = data[pos];
    if (c == '/' || c == '(' || c == '[' || c == '<' || c == '+' || c == '-' || c == '.' ||
        (c >= '0' && c <= '9')) {
        out->is_operator = false;
        return pdfobj::ParseObject(data, len, pos, &out->operand);
    }
    size_t start = pos;
    while (pos < len && !IsWs(data[pos]) && !IsDelim(data[pos])) ++pos;
    if (pos == start) {
        ++pos;  // stray delimiter with no meaning here (e.g. an orphan ']'): skip one byte, keep going
        return NextContentToken(data, len, pos, out);
    }
    out->is_operator = true;
    out->op.assign(reinterpret_cast<const char *>(data + start), pos - start);
    return true;
}

// -- Color spaces ----------------------------------------------------------
// Scoping decision 3 (PDFIUM_REMOVAL_PLAN.md): DeviceGray/RGB/CMYK
// direct, Indexed via its base space + lookup table, ICCBased via its
// declared /N component count (profile itself ignored), Separation/
// DeviceN approximated as grayscale = 1 - average(tint components)
// (ignoring the alternate space and tint-transform function entirely --
// a documented, deliberately crude fallback, not a real colorimetric
// conversion). Pattern color space (rare for plain fill/stroke color,
// mostly used for tiling fills) falls back to a flat mid-gray rather
// than either crashing or silently keeping a stale color.

enum class CsKind { kDeviceGray, kDeviceRGB, kDeviceCMYK, kIndexed, kSeparation, kPattern };

struct ColorSpaceInfo {
    CsKind kind = CsKind::kDeviceGray;
    int n = 1;
    std::shared_ptr<ColorSpaceInfo> base;  // Indexed only
    std::string lookup;                    // Indexed only: base->n bytes per entry, 0-255 each
};

void CmykToRgb(double c, double m, double y, double k, float *rgb) {
    rgb[0] = static_cast<float>(1.0 - std::min(1.0, c + k));
    rgb[1] = static_cast<float>(1.0 - std::min(1.0, m + k));
    rgb[2] = static_cast<float>(1.0 - std::min(1.0, y + k));
}

pdfobj::Object Deref(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                      const pdfobj::Object &obj) {
    if (!obj.IsReference()) return obj;
    return pdfxref::ResolveObject(data, len, table, obj.ref_val.num, obj.ref_val.gen);
}

ColorSpaceInfo BuildColorSpaceInfo(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                                    const pdfobj::Object &cs_obj_in) {
    pdfobj::Object cs_obj = Deref(data, len, table, cs_obj_in);
    if (cs_obj.IsName()) {
        const std::string &name = cs_obj.str_val;
        if (name == "DeviceGray" || name == "CalGray" || name == "G") return {CsKind::kDeviceGray, 1, nullptr, ""};
        if (name == "DeviceCMYK" || name == "CMYK") return {CsKind::kDeviceCMYK, 4, nullptr, ""};
        if (name == "Pattern") return {CsKind::kPattern, 1, nullptr, ""};
        return {CsKind::kDeviceRGB, 3, nullptr, ""};  // DeviceRGB/CalRGB/Lab/RGB and any unrecognized name
    }
    if (!cs_obj.IsArray() || cs_obj.array_val.empty()) return {CsKind::kDeviceGray, 1, nullptr, ""};

    pdfobj::Object family = Deref(data, len, table, cs_obj.array_val[0]);
    std::string fam = family.AsString("");
    if (fam == "ICCBased" && cs_obj.array_val.size() >= 2) {
        pdfobj::Object stream_dict;
        std::string raw;
        const pdfobj::Object &ref = cs_obj.array_val[1];
        int n = 3;
        if (ref.IsReference()) {
            pdfxref::ResolveStream(data, len, table, ref.ref_val.num, ref.ref_val.gen, &stream_dict, &raw);
            if (const pdfobj::Object *n_obj = stream_dict.Find("N")) n = static_cast<int>(n_obj->AsInt(3));
        }
        if (n == 1) return {CsKind::kDeviceGray, 1, nullptr, ""};
        if (n == 4) return {CsKind::kDeviceCMYK, 4, nullptr, ""};
        return {CsKind::kDeviceRGB, 3, nullptr, ""};
    }
    if (fam == "Indexed" && cs_obj.array_val.size() >= 4) {
        auto base = std::make_shared<ColorSpaceInfo>(BuildColorSpaceInfo(data, len, table, cs_obj.array_val[1]));
        // /Lookup is either a literal string or a reference to a stream
        // object (spec 8.6.6.3 allows both) -- try the stream form first
        // (ResolveStream fails cleanly, leaving `lookup` untouched, if the
        // reference isn't actually a stream), falling back to a plain
        // dereferenced string otherwise.
        const pdfobj::Object &lookup_ref = cs_obj.array_val[3];
        std::string lookup;
        bool resolved_as_stream = false;
        if (lookup_ref.IsReference()) {
            pdfobj::Object stream_dict;
            std::string raw;
            if (pdfxref::ResolveStream(data, len, table, lookup_ref.ref_val.num, lookup_ref.ref_val.gen, &stream_dict,
                                        &raw)) {
                pdffilter::DecodeStream(raw, &stream_dict, &lookup);
                resolved_as_stream = true;
            }
        }
        if (!resolved_as_stream) {
            pdfobj::Object lookup_obj = Deref(data, len, table, lookup_ref);
            if (lookup_obj.IsString()) lookup = lookup_obj.str_val;
        }
        return {CsKind::kIndexed, 1, base, lookup};
    }
    if ((fam == "Separation" || fam == "DeviceN") && cs_obj.array_val.size() >= 2) {
        int n = 1;
        pdfobj::Object names = Deref(data, len, table, cs_obj.array_val[1]);
        if (names.IsArray()) n = static_cast<int>(names.array_val.size());
        return {CsKind::kSeparation, std::max(1, n), nullptr, ""};
    }
    if (fam == "Pattern") return {CsKind::kPattern, 1, nullptr, ""};
    return {CsKind::kDeviceGray, 1, nullptr, ""};
}

void ApplyColor(const ColorSpaceInfo &cs, const std::vector<double> &comps, float *out_rgb) {
    switch (cs.kind) {
        case CsKind::kDeviceGray: {
            float g = comps.empty() ? 0.0f : static_cast<float>(comps[0]);
            out_rgb[0] = out_rgb[1] = out_rgb[2] = g;
            return;
        }
        case CsKind::kDeviceRGB:
            for (int i = 0; i < 3; ++i) {
                out_rgb[i] = static_cast<float>(static_cast<size_t>(i) < comps.size() ? comps[static_cast<size_t>(i)] : 0.0);
            }
            return;
        case CsKind::kDeviceCMYK: {
            double c = comps.size() > 0 ? comps[0] : 0, m = comps.size() > 1 ? comps[1] : 0,
                   y = comps.size() > 2 ? comps[2] : 0, k = comps.size() > 3 ? comps[3] : 0;
            CmykToRgb(c, m, y, k, out_rgb);
            return;
        }
        case CsKind::kIndexed: {
            int index = comps.empty() ? 0 : static_cast<int>(comps[0]);
            int base_n = cs.base ? cs.base->n : 1;
            std::vector<double> base_comps(static_cast<size_t>(base_n), 0.0);
            size_t off = static_cast<size_t>(index) * static_cast<size_t>(base_n);
            for (int i = 0; i < base_n; ++i) {
                size_t idx = off + static_cast<size_t>(i);
                base_comps[static_cast<size_t>(i)] = idx < cs.lookup.size()
                                                          ? static_cast<unsigned char>(cs.lookup[idx]) / 255.0
                                                          : 0.0;
            }
            if (cs.base) ApplyColor(*cs.base, base_comps, out_rgb);
            return;
        }
        case CsKind::kSeparation: {
            double sum = 0;
            for (double v : comps) sum += v;
            double avg = comps.empty() ? 0.0 : sum / static_cast<double>(comps.size());
            float g = static_cast<float>(1.0 - avg);
            out_rgb[0] = out_rgb[1] = out_rgb[2] = g;
            return;
        }
        case CsKind::kPattern:
            out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.5f;
            return;
    }
}

// -- Graphics state ----------------------------------------------------

// A path's coverage over just its own device-space bounding box (clipped
// to the canvas) rather than the whole page: a page of thousands of tiny
// strokes (a plotted mesh) used to allocate, rasterize and composite a
// full-page buffer per stroke, which took seconds to minutes per page.
struct Coverage {
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<unsigned char> data;  // w*h, row-major; (0,0) is canvas (x0,y0)
};

// The current clip: a Coverage rectangle, fully clipped (0) outside it,
// so intersecting clips and compositing under one only touch that
// rectangle rather than the whole page. null = unclipped.
using ClipMask = std::shared_ptr<const Coverage>;

// `clip`'s 0-1 factor at canvas pixel (x, y).
double ClipFactor(const ClipMask &clip, int x, int y) {
    if (!clip) return 1.0;
    int cx = x - clip->x0, cy = y - clip->y0;
    if (cx < 0 || cy < 0 || cx >= clip->w || cy >= clip->h) return 0.0;
    return static_cast<double>(clip->data[static_cast<size_t>(cy) * static_cast<size_t>(clip->w) + static_cast<size_t>(cx)]) / 255.0;
}

struct GState {
    Mat2D ctm;
    double line_width = 1.0;
    // `d` (spec 8.4.3.6): on/off lengths in user space, and the phase
    // into them each subpath starts at. Empty = solid.
    std::vector<double> dash;
    double dash_phase = 0;
    float fill_rgb[3] = {0, 0, 0};
    float stroke_rgb[3] = {0, 0, 0};
    float fill_alpha = 1, stroke_alpha = 1;
    ColorSpaceInfo fill_cs;
    ColorSpaceInfo stroke_cs;
    ClipMask clip;

    // Text state (spec 9.3) -- persists across BT/ET and survives q/Q
    // (unlike Tm/Tlm, the text/text-line matrices, which reset at every
    // BT and live on the Interpreter itself, not in GState).
    std::shared_ptr<pdffont::PdfFont> font;  // shared_ptr: cheap GState copies on q, font itself immutable once loaded
    double font_size = 1.0;
    double char_spacing = 0;    // Tc
    double word_spacing = 0;    // Tw
    double h_scale = 100.0;     // Tz, percent
    double leading = 0;         // TL
    int render_mode = 0;        // Tr: 0=fill,1=stroke,2=fill+stroke,3=invisible (4-7, clipping variants, treated as their non-clipping base mode -- clipping via text is out of scope)
    double rise = 0;            // Ts
};

struct DPoint {
    double x = 0, y = 0;
};

struct SubPath {
    std::vector<DPoint> points;
    bool closed = false;
};

// Same tolerance/depth bar as gfx::raster's own cubic flattener, just
// collecting points instead of Edges -- path construction needs actual
// point sequences for stroking (offset quads per segment), which an
// Edge list alone can't reconstruct.
void FlattenCubicToPoints(std::vector<DPoint> &pts, DPoint p0, DPoint c1, DPoint c2, DPoint p1, int depth) {
    constexpr double kToleranceSq = 0.09;
    double dx = p1.x - p0.x, dy = p1.y - p0.y;
    double d1 = (c1.x - p1.x) * dy - (c1.y - p1.y) * dx;
    double d2 = (c2.x - p1.x) * dy - (c2.y - p1.y) * dx;
    double d = std::max(d1 * d1, d2 * d2);
    if (depth >= 10 || d < kToleranceSq * (dx * dx + dy * dy)) {
        pts.push_back(p1);
        return;
    }
    DPoint p01{(p0.x + c1.x) / 2, (p0.y + c1.y) / 2};
    DPoint p12{(c1.x + c2.x) / 2, (c1.y + c2.y) / 2};
    DPoint p23{(c2.x + p1.x) / 2, (c2.y + p1.y) / 2};
    DPoint p012{(p01.x + p12.x) / 2, (p01.y + p12.y) / 2};
    DPoint p123{(p12.x + p23.x) / 2, (p12.y + p23.y) / 2};
    DPoint p0123{(p012.x + p123.x) / 2, (p012.y + p123.y) / 2};
    FlattenCubicToPoints(pts, p0, p01, p012, p0123, depth + 1);
    FlattenCubicToPoints(pts, p0123, p123, p23, p1, depth + 1);
}

// -- Compositing ---------------------------------------------------------

Coverage RasterizeEdges(std::vector<gfx::raster::Edge> &edges, int canvas_w, int canvas_h,
                        gfx::raster::FillRule rule) {
    Coverage cov;
    if (edges.empty()) return cov;
    float min_x = edges.front().x_at_ymin, max_x = min_x;
    float min_y = edges.front().ymin, max_y = edges.front().ymax;
    for (const auto &e : edges) {
        float x_end = e.x_at_ymin + (e.ymax - e.ymin) * e.dxdy;
        min_x = std::min({min_x, e.x_at_ymin, x_end});
        max_x = std::max({max_x, e.x_at_ymin, x_end});
        min_y = std::min(min_y, e.ymin);
        max_y = std::max(max_y, e.ymax);
    }
    // Clamp in float before converting: a far-off-page path can put
    // coordinates outside int's range.
    auto clampf = [](float v, int hi) { return std::clamp(v, 0.0f, static_cast<float>(hi)); };
    int x0 = static_cast<int>(std::floor(clampf(min_x, canvas_w))), x1 = static_cast<int>(std::ceil(clampf(max_x, canvas_w)));
    int y0 = static_cast<int>(std::floor(clampf(min_y, canvas_h))), y1 = static_cast<int>(std::ceil(clampf(max_y, canvas_h)));
    x1 = std::min(canvas_w, x1 + 1);
    y1 = std::min(canvas_h, y1 + 1);
    if (x1 <= x0 || y1 <= y0) return cov;
    cov.x0 = x0;
    cov.y0 = y0;
    cov.w = x1 - x0;
    cov.h = y1 - y0;
    cov.data = gfx::raster::RasterizeRegion(edges, x0, y0, cov.w, cov.h, rule);
    return cov;
}

void CompositeCoverage(Canvas &canvas, const Coverage &coverage, const ClipMask &clip, const float rgb[3],
                       float alpha) {
    unsigned char r = static_cast<unsigned char>(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    unsigned char g = static_cast<unsigned char>(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f);
    unsigned char b = static_cast<unsigned char>(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);
    // Only where the path's own rectangle and the clip's overlap.
    int ylo = 0, yhi = coverage.h, xlo = 0, xhi = coverage.w;
    if (clip) {
        ylo = std::max(ylo, clip->y0 - coverage.y0);
        yhi = std::min(yhi, clip->y0 + clip->h - coverage.y0);
        xlo = std::max(xlo, clip->x0 - coverage.x0);
        xhi = std::min(xhi, clip->x0 + clip->w - coverage.x0);
    }
    for (int y = ylo; y < yhi; ++y) {
        for (int x = xlo; x < xhi; ++x) {
            unsigned char c = coverage.data[static_cast<size_t>(y) * static_cast<size_t>(coverage.w) + static_cast<size_t>(x)];
            if (c == 0) continue;
            size_t i = static_cast<size_t>(coverage.y0 + y) * static_cast<size_t>(canvas.width) +
                       static_cast<size_t>(coverage.x0 + x);
            double cov = static_cast<double>(c) / 255.0 * ClipFactor(clip, coverage.x0 + x, coverage.y0 + y);
            double a = cov * static_cast<double>(std::clamp(alpha, 0.0f, 1.0f));
            if (a <= 0.0) continue;
            unsigned char *px = &canvas.rgba[i * 4];
            px[0] = static_cast<unsigned char>(px[0] + (static_cast<double>(r) - px[0]) * a + 0.5);
            px[1] = static_cast<unsigned char>(px[1] + (static_cast<double>(g) - px[1]) * a + 0.5);
            px[2] = static_cast<unsigned char>(px[2] + (static_cast<double>(b) - px[2]) * a + 0.5);
            // px[3] (alpha channel) left at 255 -- canvas is always opaque,
            // matching pdf_doc.h's RenderPage contract.
        }
    }
}

// Composites a single glyph's small coverage bitmap (from
// pdffont::PdfFont::GetGlyphBitmap) onto the canvas at device-pixel
// offset (origin_x+xoff, origin_y+yoff) -- a glyph bitmap is tiny
// relative to the page, so this walks just its own w*h pixels rather
// than the whole canvas the way CompositeCoverage does for a full-page
// path fill.
void CompositeGlyphBitmap(Canvas &canvas, const unsigned char *bitmap, int gw, int gh, double origin_x,
                           double origin_y, int xoff, int yoff, const ClipMask &clip, const float rgb[3],
                           float alpha) {
    unsigned char r = static_cast<unsigned char>(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    unsigned char g = static_cast<unsigned char>(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f);
    unsigned char b = static_cast<unsigned char>(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);
    // Floor, not round: the glyph was rasterized with its sub-pixel
    // offset already applied (see ShowText), so the whole-pixel part is
    // all that is left to place. Rounding here would add the half-pixel
    // displacement back.
    int base_x = static_cast<int>(std::floor(origin_x)) + xoff;
    int base_y = static_cast<int>(std::floor(origin_y)) + yoff;
    for (int y = 0; y < gh; ++y) {
        int py = base_y + y;
        if (py < 0 || py >= canvas.height) continue;
        for (int x = 0; x < gw; ++x) {
            int px_x = base_x + x;
            if (px_x < 0 || px_x >= canvas.width) continue;
            double cov = static_cast<double>(bitmap[static_cast<size_t>(y) * static_cast<size_t>(gw) + static_cast<size_t>(x)]) / 255.0;
            if (cov <= 0.0) continue;
            size_t cpx = static_cast<size_t>(py) * static_cast<size_t>(canvas.width) + static_cast<size_t>(px_x);
            cov *= ClipFactor(clip, px_x, py);
            double a = cov * static_cast<double>(std::clamp(alpha, 0.0f, 1.0f));
            if (a <= 0.0) continue;
            unsigned char *dst = &canvas.rgba[cpx * 4];
            dst[0] = static_cast<unsigned char>(dst[0] + (static_cast<double>(r) - dst[0]) * a + 0.5);
            dst[1] = static_cast<unsigned char>(dst[1] + (static_cast<double>(g) - dst[1]) * a + 0.5);
            dst[2] = static_cast<unsigned char>(dst[2] + (static_cast<double>(b) - dst[2]) * a + 0.5);
        }
    }
}

Coverage RasterizeFill(const std::vector<SubPath> &path, int width, int height, gfx::raster::FillRule rule) {
    std::vector<gfx::raster::Edge> edges;
    for (const SubPath &sp : path) {
        if (sp.points.size() < 2) continue;
        for (size_t i = 0; i + 1 < sp.points.size(); ++i) {
            gfx::raster::AddLine(edges, static_cast<float>(sp.points[i].x), static_cast<float>(sp.points[i].y),
                                  static_cast<float>(sp.points[i + 1].x), static_cast<float>(sp.points[i + 1].y));
        }
        // Fill always implicitly closes every subpath, regardless of
        // whether 'h' was used (spec 8.5.3.1).
        const DPoint &first = sp.points.front();
        const DPoint &last = sp.points.back();
        gfx::raster::AddLine(edges, static_cast<float>(last.x), static_cast<float>(last.y),
                              static_cast<float>(first.x), static_cast<float>(first.y));
    }
    return RasterizeEdges(edges, width, height, rule);
}

void AddQuadClockwise(std::vector<gfx::raster::Edge> &edges, DPoint a, DPoint b, DPoint c, DPoint d) {
    gfx::raster::AddLine(edges, static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(b.x),
                          static_cast<float>(b.y));
    gfx::raster::AddLine(edges, static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(c.x),
                          static_cast<float>(c.y));
    gfx::raster::AddLine(edges, static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(d.x),
                          static_cast<float>(d.y));
    gfx::raster::AddLine(edges, static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(a.x),
                          static_cast<float>(a.y));
}

// Basic stroke: each segment becomes a rectangle of the given device-
// space half-width, no miter joins/round caps/dash patterns (Scoping
// decision 6) -- overlapping per-segment quads at each join are simply
// unioned via nonzero fill (consistently-wound quads mean overlaps just
// accumulate winding > 1, still "inside", no seam/gap artifacts).
Coverage RasterizeStroke(const std::vector<SubPath> &path, double half_width, int width, int height) {
    std::vector<gfx::raster::Edge> edges;
    for (const SubPath &sp : path) {
        size_t n = sp.points.size();
        if (n < 2) continue;
        size_t segments = sp.closed ? n : n - 1;
        for (size_t i = 0; i < segments; ++i) {
            const DPoint &p0 = sp.points[i];
            const DPoint &p1 = sp.points[(i + 1) % n];
            double dx = p1.x - p0.x, dy = p1.y - p0.y;
            double len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-9) continue;
            double nx = -dy / len * half_width, ny = dx / len * half_width;
            AddQuadClockwise(edges, {p0.x + nx, p0.y + ny}, {p1.x + nx, p1.y + ny}, {p1.x - nx, p1.y - ny},
                              {p0.x - nx, p0.y - ny});
        }
    }
    return RasterizeEdges(edges, width, height, gfx::raster::FillRule::kNonZero);
}

// Splits `path` into the "on" pieces of a dash pattern (device-space
// lengths, already scaled from user space), each one an open subpath --
// RasterizeStroke then strokes them like any other. The pattern restarts
// at `phase` for every subpath (spec 8.4.3.6); a closed subpath's closing
// segment is dashed too. A degenerate pattern (all zero / negative)
// leaves the path solid.
std::vector<SubPath> DashPath(const std::vector<SubPath> &path, const std::vector<double> &dash, double phase) {
    double total = 0;
    for (double d : dash) {
        if (d < 0) return path;
        total += d;
    }
    if (dash.empty() || total <= 1e-9) return path;
    std::vector<SubPath> out;
    for (const SubPath &sp : path) {
        size_t n = sp.points.size();
        if (n < 2) continue;
        // Position within the pattern: index `di`, `left` length remaining in it.
        size_t di = 0;
        double left = dash[0];
        double ph = std::fmod(std::max(0.0, phase), total);
        while (ph > 0) {
            if (ph >= left) {
                ph -= left;
                di = (di + 1) % dash.size();
                left = dash[di];
            } else {
                left -= ph;
                ph = 0;
            }
        }
        bool on = di % 2 == 0;
        SubPath cur;
        if (on) cur.points.push_back(sp.points[0]);
        size_t segments = sp.closed ? n : n - 1;
        for (size_t i = 0; i < segments; ++i) {
            DPoint a = sp.points[i];
            const DPoint &b = sp.points[(i + 1) % n];
            double seg = std::hypot(b.x - a.x, b.y - a.y);
            while (seg > 1e-12) {
                double step = std::min(seg, left);
                double t = step / seg;
                DPoint p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                if (on) cur.points.push_back(p);
                a = p;
                seg -= step;
                left -= step;
                if (left <= 1e-12) {
                    // Dash boundary: close out an "on" piece, start the next.
                    if (on && cur.points.size() >= 2) out.push_back(std::move(cur));
                    cur = SubPath{};
                    di = (di + 1) % dash.size();
                    left = dash[di];
                    on = di % 2 == 0;
                    if (on) cur.points.push_back(a);
                }
            }
        }
        if (on && cur.points.size() >= 2) out.push_back(std::move(cur));
    }
    return out;
}

double CtmScale(const Mat2D &m) { return std::sqrt(std::abs(m.a * m.d - m.b * m.c)); }

// The clip after intersecting `existing` with a new clip path's coverage:
// the overlap of the two rectangles (possibly empty = everything clipped).
ClipMask IntersectMask(const ClipMask &existing, Coverage fresh) {
    if (!existing) return std::make_shared<const Coverage>(std::move(fresh));
    Coverage out;
    out.x0 = std::max(existing->x0, fresh.x0);
    out.y0 = std::max(existing->y0, fresh.y0);
    out.w = std::max(0, std::min(existing->x0 + existing->w, fresh.x0 + fresh.w) - out.x0);
    out.h = std::max(0, std::min(existing->y0 + existing->h, fresh.y0 + fresh.h) - out.y0);
    if (out.w == 0 || out.h == 0) out.w = out.h = 0;
    out.data.resize(static_cast<size_t>(out.w) * static_cast<size_t>(out.h));
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            auto at = [&](const Coverage &c) {
                return static_cast<int>(c.data[static_cast<size_t>(out.y0 + y - c.y0) * static_cast<size_t>(c.w) +
                                               static_cast<size_t>(out.x0 + x - c.x0)]);
            };
            out.data[static_cast<size_t>(y) * static_cast<size_t>(out.w) + static_cast<size_t>(x)] =
                static_cast<unsigned char>(at(*existing) * at(fresh) / 255);
        }
    }
    return std::make_shared<const Coverage>(std::move(out));
}

// -- Images ----------------------------------------------------------------
// PDFIUM_REMOVAL_PLAN.md Phase 8. An image XObject (or inline image)
// occupies the unit square [0,1]x[0,1] in the current user space (spec
// 8.9.5.1), sampled at Width x Height with row 0 at the TOP (image
// space is y-down, unlike PDF user space). Composited via inverse
// mapping: for each device pixel in the image's device-space bounding
// box, invert the CTM to find its (u,v) position in the unit square and
// nearest-neighbor-sample the source -- simpler and more robust for
// arbitrary rotated/skewed placements than forward-mapping, matching
// this codebase's general "correctness over resampling-quality tricks"
// convention (e.g. the JPEG codec's own nearest/box chroma upsampling
// choice).

bool Invert(const Mat2D &m, Mat2D *out) {
    double det = m.a * m.d - m.b * m.c;
    if (std::abs(det) < 1e-12) return false;
    double inv_det = 1.0 / det;
    out->a = m.d * inv_det;
    out->b = -m.b * inv_det;
    out->c = -m.c * inv_det;
    out->d = m.a * inv_det;
    out->e = -(m.e * out->a + m.f * out->c);
    out->f = -(m.e * out->b + m.f * out->d);
    return true;
}

struct DecodedImage {
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;  // width*height*4, straight alpha
    bool is_mask = false;             // true: rgb unused, colorized with the current fill color at draw time
    // /Interpolate (spec 8.9.5.1, Table 89), default false: whether the
    // producer wants the image smoothed when it is drawn larger than its
    // own sample grid. Honoured only for magnification -- minification
    // is averaged regardless, since that is anti-aliasing rather than
    // interpolation and no viewer leaves it off.
    bool interpolate = false;
};

// Full-name-or-abbreviated key lookup -- inline images (BI/ID/EI) use
// abbreviated keys (spec Table 93); regular Image XObjects always use
// full names. Sharing one decode path for both means checking both
// spellings everywhere rather than normalizing keys up front.
const pdfobj::Object *FindKey(const pdfobj::Object &dict, const char *full, const char *abbrev) {
    if (const pdfobj::Object *v = dict.Find(full)) return v;
    return abbrev ? dict.Find(abbrev) : nullptr;
}

// Reads a `bpc`-wide sample (MSB-first, spec 8.9.5.2) at the given
// 0-based sample index within one image row's raw bytes.
unsigned ReadSample(const unsigned char *row, int bpc, int sample_index) {
    if (bpc == 8) return row[sample_index];
    if (bpc == 16) return row[sample_index * 2];  // high byte only, matches this codebase's own 16-bit truncation convention (png_codec.cpp)
    long bit_off = static_cast<long>(sample_index) * bpc;
    long byte_off = bit_off / 8;
    int shift = 8 - bpc - static_cast<int>(bit_off % 8);
    unsigned mask = (1u << bpc) - 1u;
    return (row[byte_off] >> shift) & mask;
}

// Decodes a generic (non-DCT) image's raw sample bytes into RGBA8,
// honoring /Decode (or inline's /D) and /ImageMask (or /IM). `cs` is
// pre-resolved by the caller (Null/unused for an ImageMask).
DecodedImage UnpackSamples(const std::string &samples, int width, int height, int bpc, int n_comps,
                            const ColorSpaceInfo &cs, bool is_indexed, bool is_mask, const pdfobj::Object *decode_arr) {
    DecodedImage img;
    img.width = std::max(width, 0);
    img.height = std::max(height, 0);
    img.is_mask = is_mask;
    img.rgba.assign(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4, 0);
    if (img.width == 0 || img.height == 0 || bpc <= 0) return img;

    int actual_n = is_mask ? 1 : n_comps;
    size_t row_bytes = (static_cast<size_t>(width) * static_cast<size_t>(actual_n) * static_cast<size_t>(bpc) + 7) / 8;
    if (samples.size() < row_bytes * static_cast<size_t>(height)) {
        // Truncated/short sample data: tolerate, decode whatever rows fit
        // (matches this codebase's "skip bad content" convention) rather
        // than refusing the whole image.
        height = static_cast<int>(samples.size() / std::max<size_t>(row_bytes, 1));
    }

    double dmax_default = is_indexed ? static_cast<double>((1u << bpc) - 1u) : 1.0;
    std::vector<double> dmin(static_cast<size_t>(actual_n), 0.0), dmax(static_cast<size_t>(actual_n), dmax_default);
    if (decode_arr && decode_arr->IsArray() && decode_arr->array_val.size() == static_cast<size_t>(actual_n) * 2) {
        for (int i = 0; i < actual_n; ++i) {
            dmin[static_cast<size_t>(i)] = decode_arr->array_val[static_cast<size_t>(i) * 2].AsDouble();
            dmax[static_cast<size_t>(i)] = decode_arr->array_val[static_cast<size_t>(i) * 2 + 1].AsDouble();
        }
    }
    double max_sample = static_cast<double>((1u << bpc) - 1u);

    const unsigned char *base = reinterpret_cast<const unsigned char *>(samples.data());
    for (int y = 0; y < img.height && y < height; ++y) {
        const unsigned char *row = base + static_cast<size_t>(y) * row_bytes;
        for (int x = 0; x < width; ++x) {
            std::vector<double> comps(static_cast<size_t>(actual_n));
            for (int c = 0; c < actual_n; ++c) {
                unsigned raw = ReadSample(row, bpc, x * actual_n + c);
                double norm = max_sample > 0 ? static_cast<double>(raw) / max_sample : 0.0;
                comps[static_cast<size_t>(c)] = dmin[static_cast<size_t>(c)] + norm * (dmax[static_cast<size_t>(c)] - dmin[static_cast<size_t>(c)]);
            }
            size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
            if (is_mask) {
                // Default Decode [0 1]: sample 0 -> paint (opaque), 1 -> don't (spec 8.9.6.2).
                bool paint = comps[0] < 0.5;
                img.rgba[idx + 3] = paint ? 255 : 0;
            } else {
                float rgb[3];
                ApplyColor(cs, comps, rgb);
                img.rgba[idx + 0] = static_cast<unsigned char>(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f);
                img.rgba[idx + 1] = static_cast<unsigned char>(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f);
                img.rgba[idx + 2] = static_cast<unsigned char>(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);
                img.rgba[idx + 3] = 255;
            }
        }
    }
    return img;
}

// Decodes an /SMask (always DeviceGray, no /ColorSpace needed) into a
// standalone width/height + gray buffer for alpha lookup, independent
// of the base image's own dimensions (spec allows them to differ).
struct SMaskAlpha {
    int width = 0, height = 0;
    std::vector<unsigned char> gray;
};

bool DecodeSMask(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                  const pdfobj::Object &smask_ref, SMaskAlpha *out) {
    if (!smask_ref.IsReference()) return false;
    pdfobj::Object dict;
    std::string raw;
    if (!pdfxref::ResolveStream(doc_data, doc_len, table, smask_ref.ref_val.num, smask_ref.ref_val.gen, &dict, &raw)) {
        return false;
    }
    std::string decoded;
    if (!pdffilter::DecodeStream(raw, &dict, &decoded)) return false;
    int width = static_cast<int>(FindKey(dict, "Width", nullptr) ? FindKey(dict, "Width", nullptr)->AsInt() : 0);
    int height = static_cast<int>(FindKey(dict, "Height", nullptr) ? FindKey(dict, "Height", nullptr)->AsInt() : 0);
    int bpc = static_cast<int>(FindKey(dict, "BitsPerComponent", nullptr) ? FindKey(dict, "BitsPerComponent", nullptr)->AsInt() : 8);
    ColorSpaceInfo gray_cs{CsKind::kDeviceGray, 1, nullptr, ""};
    DecodedImage unpacked = UnpackSamples(decoded, width, height, bpc, 1, gray_cs, false, false, dict.Find("Decode"));
    out->width = unpacked.width;
    out->height = unpacked.height;
    out->gray.resize(static_cast<size_t>(out->width) * static_cast<size_t>(out->height));
    for (size_t i = 0; i < out->gray.size(); ++i) out->gray[i] = unpacked.rgba[i * 4 + 0];
    return true;
}

// An image dict's /ColorSpace, resolved through the page's
// /Resources /ColorSpace when it's a named (non-device) space.
ColorSpaceInfo ImageColorSpace(const pdfobj::Object &dict, const pdfobj::Object &resources,
                               const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table) {
    const pdfobj::Object *cs_obj = FindKey(dict, "ColorSpace", "CS");
    if (!cs_obj) return {CsKind::kDeviceGray, 1, nullptr, ""};
    pdfobj::Object resolved_cs = *cs_obj;
    if (cs_obj->IsName()) {
        const std::string &n = cs_obj->str_val;
        if (n != "DeviceGray" && n != "DeviceRGB" && n != "DeviceCMYK" && n != "G" && n != "RGB" && n != "CMYK") {
            const pdfobj::Object *cs_dict = resources.IsDict() ? resources.Find("ColorSpace") : nullptr;
            if (cs_dict) {
                pdfobj::Object rd = Deref(doc_data, doc_len, table, *cs_dict);
                if (const pdfobj::Object *found = rd.Find(n)) resolved_cs = *found;
            }
        }
    }
    return BuildColorSpaceInfo(doc_data, doc_len, table, resolved_cs);
}

// Top-level image decode: dispatches DCTDecode straight to jpeg::Decode
// (spec 7.4.8: never combined with another filter), else runs the
// generic filter chain and unpacks samples. CCITTFaxDecode goes through
// that generic chain like any other byte-producing filter (see
// pdf_filters.h's own note on why it isn't beside it the way DCTDecode
// is). Bakes /SMask alpha into the result (nearest-neighbor-resampled
// to the base image's own dimensions, since the two can differ).
// Returns false for a filter DecodeStream still refuses
// (JBIG2Decode/JPXDecode -- Scoping decision 4's remaining
// scanned-image non-goal): the image is simply skipped, matching
// RenderPage's "skip bad content" contract.
bool DecodeImageDict(const pdfobj::Object &dict, const std::string &raw, const pdfobj::Object &resources,
                      const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                      DecodedImage *out) {
    if (const pdfobj::Object *interp = FindKey(dict, "Interpolate", "I"))
        out->interpolate = interp->type == pdfobj::Type::Bool && interp->bool_val;
    bool is_mask = false;
    if (const pdfobj::Object *im = FindKey(dict, "ImageMask", "IM")) is_mask = im->bool_val && im->type == pdfobj::Type::Bool;
    int width = static_cast<int>(FindKey(dict, "Width", "W") ? FindKey(dict, "Width", "W")->AsInt() : 0);
    int height = static_cast<int>(FindKey(dict, "Height", "H") ? FindKey(dict, "Height", "H")->AsInt() : 0);
    if (width <= 0 || height <= 0) return false;

    const pdfobj::Object *filter = FindKey(dict, "Filter", "F");
    bool is_dct = false;
    if (filter) {
        if (filter->IsName() && (filter->str_val == "DCTDecode" || filter->str_val == "DCT")) is_dct = true;
        if (filter->IsArray()) {
            for (const auto &f : filter->array_val) {
                if (f.AsString("") == "DCTDecode" || f.AsString("") == "DCT") is_dct = true;
            }
        }
    }

    if (is_dct) {
        std::string rgba_buf;
        int jw = 0, jh = 0;
        if (!pdffilter::DCTDecode(raw, &rgba_buf, &jw, &jh)) return false;
        out->width = jw;
        out->height = jh;
        out->is_mask = false;
        out->rgba.assign(rgba_buf.begin(), rgba_buf.end());
        // The JPEG decoder yields plain gray/RGB; a /Decode [1 0 ...] and a
        // Separation/DeviceN space (samples are ink *amounts*: 1 = full
        // ink, i.e. dark -- same 1 - tint approximation as ColorToRgb) each
        // invert that, e.g. a /Separation /Black figure drawn as a negative
        // before this.
        bool invert = ImageColorSpace(dict, resources, doc_data, doc_len, table).kind == CsKind::kSeparation;
        if (const pdfobj::Object *decode = FindKey(dict, "Decode", "D")) {
            if (decode->IsArray() && decode->array_val.size() >= 2 &&
                decode->array_val[0].AsDouble(0) > decode->array_val[1].AsDouble(0)) {
                invert = !invert;
            }
        }
        if (invert) {
            for (size_t i = 0; i + 3 < out->rgba.size(); i += 4) {
                for (size_t c = 0; c < 3; ++c) out->rgba[i + c] = static_cast<unsigned char>(255 - out->rgba[i + c]);
            }
        }
    } else {
        std::string decoded;
        if (!pdffilter::DecodeStream(raw, &dict, &decoded)) return false;  // JBIG2/JPX: skip, Scoping decision 4
        int bpc = is_mask ? 1
                           : static_cast<int>(FindKey(dict, "BitsPerComponent", "BPC")
                                                   ? FindKey(dict, "BitsPerComponent", "BPC")->AsInt()
                                                   : 8);
        ColorSpaceInfo cs{CsKind::kDeviceGray, 1, nullptr, ""};
        bool is_indexed = false;
        if (!is_mask) {
            cs = ImageColorSpace(dict, resources, doc_data, doc_len, table);
            is_indexed = cs.kind == CsKind::kIndexed;
        }
        int n_comps = is_mask ? 1 : cs.n;
        *out = UnpackSamples(decoded, width, height, bpc, n_comps, cs, is_indexed, is_mask,
                              FindKey(dict, "Decode", "D"));
    }

    if (!is_mask) {
        if (const pdfobj::Object *smask_ref = dict.Find("SMask")) {
            SMaskAlpha smask;
            if (DecodeSMask(doc_data, doc_len, table, *smask_ref, &smask) && smask.width > 0 && smask.height > 0) {
                for (int y = 0; y < out->height; ++y) {
                    int sy = smask.height * y / out->height;
                    for (int x = 0; x < out->width; ++x) {
                        int sx = smask.width * x / out->width;
                        size_t didx = (static_cast<size_t>(y) * static_cast<size_t>(out->width) + static_cast<size_t>(x)) * 4;
                        out->rgba[didx + 3] = smask.gray[static_cast<size_t>(sy) * static_cast<size_t>(smask.width) + static_cast<size_t>(sx)];
                    }
                }
            }
        }
    }
    return true;
}

// One filtered sample of `img` at unit-square position (u, v), given how
// much of the source one device pixel covers (`fu`, `fv`, in texels).
//
// Nearest-neighbour was the original choice, and it aliases badly under
// *minification* -- which is what a scanned page is, 2500-odd source
// pixels landing on a 1200-pixel-wide render. One texel in four or more
// was being read and the rest discarded, so thin strokes broke up and
// bitonal scans came out speckled. Bilinear would not fix that either:
// it still reads at most four texels however many a pixel covers, so
// minification has to be a box average over the real footprint.
//
// Magnification is left alone unless the producer asked for smoothing
// via /Interpolate, which defaults to false (spec 8.9.5.1) -- blurring
// a deliberately blocky image, or the hard edge of a stencil mask, is
// not an improvement.
//
// Written against the alpha channel as well as the colour, which is what
// makes it smooth an ImageMask -- a scanned bitonal page, and the bitmap
// glyphs of a Type 3 font, are ImageMasks, and they are the case where
// nearest-neighbour is most visible.
void SampleImage(const DecodedImage &img, double u, double v, double fu, double fv, unsigned char out[4]) {
    const double cx = u * img.width;
    const double cy = (1.0 - v) * img.height;  // image space is y-down
    const double hx = std::max(0.5, std::abs(fu) * 0.5);
    const double hy = std::max(0.5, std::abs(fv) * 0.5);

    if (hx <= 0.5 && hy <= 0.5) {
        if (!img.interpolate) {
            // Magnifying an image whose producer did not ask for
            // interpolation: spec 8.9.5.1 says do not smooth it, and
            // /Interpolate defaults to false. Blurring here would also
            // soften the deliberately hard edges of an ImageMask used
            // as a stencil.
            const int col = std::clamp(static_cast<int>(cx), 0, img.width - 1);
            const int row = std::clamp(static_cast<int>(cy), 0, img.height - 1);
            const size_t idx =
                (static_cast<size_t>(row) * static_cast<size_t>(img.width) + static_cast<size_t>(col)) * 4;
            for (int c = 0; c < 4; ++c) out[c] = img.rgba[idx + static_cast<size_t>(c)];
            return;
        }
        // Magnifying with /Interpolate true: bilinear between the four
        // texels around the sample point. Texel centres sit at +0.5,
        // hence the shift.
        const double sx = cx - 0.5, sy = cy - 0.5;
        const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
        const double tx = sx - static_cast<double>(x0), ty = sy - static_cast<double>(y0);
        const int xa = std::clamp(x0, 0, img.width - 1), xb = std::clamp(x0 + 1, 0, img.width - 1);
        const int ya = std::clamp(y0, 0, img.height - 1), yb = std::clamp(y0 + 1, 0, img.height - 1);
        const size_t i00 = (static_cast<size_t>(ya) * static_cast<size_t>(img.width) + static_cast<size_t>(xa)) * 4;
        const size_t i10 = (static_cast<size_t>(ya) * static_cast<size_t>(img.width) + static_cast<size_t>(xb)) * 4;
        const size_t i01 = (static_cast<size_t>(yb) * static_cast<size_t>(img.width) + static_cast<size_t>(xa)) * 4;
        const size_t i11 = (static_cast<size_t>(yb) * static_cast<size_t>(img.width) + static_cast<size_t>(xb)) * 4;
        for (int c = 0; c < 4; ++c) {
            const double top = static_cast<double>(img.rgba[i00 + static_cast<size_t>(c)]) * (1.0 - tx) +
                               static_cast<double>(img.rgba[i10 + static_cast<size_t>(c)]) * tx;
            const double bot = static_cast<double>(img.rgba[i01 + static_cast<size_t>(c)]) * (1.0 - tx) +
                               static_cast<double>(img.rgba[i11 + static_cast<size_t>(c)]) * tx;
            out[c] = static_cast<unsigned char>(std::clamp(top * (1.0 - ty) + bot * ty, 0.0, 255.0) + 0.5);
        }
        return;
    }

    // Minifying: average the texels this device pixel actually covers.
    // Capped per axis so a figure scaled down to nothing cannot walk the
    // whole source for one pixel -- past that the skipped texels change
    // the average by less than a level.
    constexpr int kMaxTaps = 8;
    int ix0 = static_cast<int>(std::floor(cx - hx)), ix1 = static_cast<int>(std::ceil(cx + hx));
    int iy0 = static_cast<int>(std::floor(cy - hy)), iy1 = static_cast<int>(std::ceil(cy + hy));
    ix0 = std::clamp(ix0, 0, img.width - 1);
    ix1 = std::clamp(ix1, ix0 + 1, img.width);
    iy0 = std::clamp(iy0, 0, img.height - 1);
    iy1 = std::clamp(iy1, iy0 + 1, img.height);
    const int step_x = std::max(1, (ix1 - ix0 + kMaxTaps - 1) / kMaxTaps);
    const int step_y = std::max(1, (iy1 - iy0 + kMaxTaps - 1) / kMaxTaps);
    // Integers, not doubles: the samples are bytes and the counts are
    // small, so this cannot overflow a 32-bit accumulator for any
    // footprint this loop will take (8x8 taps x 255). A stencil's colour
    // comes from the fill colour rather than the image, so only its
    // alpha is averaged -- there is nothing in the other three channels
    // to average.
    //
    // Measured cost of the filtering as a whole, on the fixture built
    // for it (a full-page 2400x3000 stencil, tools/pdf_make_scan.py):
    // 108ms to draw with point sampling against 143ms with this, three
    // runs each. Most of that 108ms is decoding 7.2M source pixels, not
    // sampling them. The +32% buys 39% less error against poppler on
    // that fixture and 16-32% on the pages of a real 4205x6000 CCITT
    // scan; on a page of text rather than an image it is unmeasurable.
    // The cap below never binds at a scan's own ratio -- 4 and 8 taps
    // per axis time identically -- it is only there so a figure scaled
    // down to nothing cannot walk the whole source for one pixel.
    unsigned int acc_a = 0, acc_r = 0, acc_g = 0, acc_b = 0;
    unsigned int n = 0;
    for (int y = iy0; y < iy1; y += step_y) {
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(img.width);
        for (int x = ix0; x < ix1; x += step_x) {
            const unsigned char *t = &img.rgba[(row + static_cast<size_t>(x)) * 4];
            acc_a += t[3];
            if (!img.is_mask) {
                acc_r += t[0];
                acc_g += t[1];
                acc_b += t[2];
            }
            ++n;
        }
    }
    if (n == 0) n = 1;
    out[3] = static_cast<unsigned char>((acc_a + n / 2) / n);
    if (img.is_mask) {
        out[0] = out[1] = out[2] = 0;  // unused; DrawImage substitutes the fill colour
    } else {
        out[0] = static_cast<unsigned char>((acc_r + n / 2) / n);
        out[1] = static_cast<unsigned char>((acc_g + n / 2) / n);
        out[2] = static_cast<unsigned char>((acc_b + n / 2) / n);
    }
}

// Composites `img` onto `canvas` as if it fills the unit square under
// `ctm`, via inverse-mapping + filtered sampling (see SampleImage). `mask_rgb` colors an ImageMask's opaque
// texels (spec: painted with the current fill color); ignored for a
// real image.
void DrawImage(Canvas &canvas, const DecodedImage &img, const Mat2D &ctm, const ClipMask &clip,
                const float mask_rgb[3], float alpha) {
    if (img.width <= 0 || img.height <= 0) return;
    Mat2D inv;
    if (!Invert(ctm, &inv)) return;

    double corners_x[4], corners_y[4];
    Transform(ctm, 0, 0, &corners_x[0], &corners_y[0]);
    Transform(ctm, 1, 0, &corners_x[1], &corners_y[1]);
    Transform(ctm, 1, 1, &corners_x[2], &corners_y[2]);
    Transform(ctm, 0, 1, &corners_x[3], &corners_y[3]);
    double min_x = *std::min_element(corners_x, corners_x + 4), max_x = *std::max_element(corners_x, corners_x + 4);
    double min_y = *std::min_element(corners_y, corners_y + 4), max_y = *std::max_element(corners_y, corners_y + 4);
    int x0 = std::max(0, static_cast<int>(std::floor(min_x)));
    int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
    int x1 = std::min(canvas.width, static_cast<int>(std::ceil(max_x)));
    int y1 = std::min(canvas.height, static_cast<int>(std::ceil(max_y)));

    // One device pixel's footprint in source texels. The inverse maps
    // device space to the unit square, so its linear part gives how far
    // u and v move per pixel; scaling by the source dimensions turns
    // that into texels. Both axes of the step are counted, so a rotated
    // or skewed placement gets a footprint rather than just its
    // axis-aligned part.
    const double texels_per_px_u = std::hypot(inv.a, inv.c) * static_cast<double>(img.width);
    const double texels_per_px_v = std::hypot(inv.b, inv.d) * static_cast<double>(img.height);

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            double u, v;
            Transform(inv, static_cast<double>(px) + 0.5, static_cast<double>(py) + 0.5, &u, &v);
            if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
            unsigned char sample[4];
            SampleImage(img, u, v, texels_per_px_u, texels_per_px_v, sample);
            double src_alpha = static_cast<double>(sample[3]) / 255.0;
            if (src_alpha <= 0) continue;
            size_t cpx = static_cast<size_t>(py) * static_cast<size_t>(canvas.width) + static_cast<size_t>(px);
            double a = src_alpha * static_cast<double>(std::clamp(alpha, 0.0f, 1.0f));
            a *= ClipFactor(clip, px, py);
            if (a <= 0) continue;
            unsigned char r, g, b;
            if (img.is_mask) {
                r = static_cast<unsigned char>(std::clamp(mask_rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f);
                g = static_cast<unsigned char>(std::clamp(mask_rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f);
                b = static_cast<unsigned char>(std::clamp(mask_rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                r = sample[0];
                g = sample[1];
                b = sample[2];
            }
            unsigned char *dst = &canvas.rgba[cpx * 4];
            dst[0] = static_cast<unsigned char>(dst[0] + (static_cast<double>(r) - dst[0]) * a + 0.5);
            dst[1] = static_cast<unsigned char>(dst[1] + (static_cast<double>(g) - dst[1]) * a + 0.5);
            dst[2] = static_cast<unsigned char>(dst[2] + (static_cast<double>(b) - dst[2]) * a + 0.5);
        }
    }
}

// -- Interpreter -----------------------------------------------------------

struct Interpreter {
    Interpreter(Canvas &canvas_in, const unsigned char *doc_data_in, size_t doc_len_in,
                const pdfxref::XrefTable &table_in)
        : canvas(canvas_in), doc_data(doc_data_in), doc_len(doc_len_in), table(table_in) {}

    Canvas &canvas;
    const unsigned char *doc_data;
    size_t doc_len;
    const pdfxref::XrefTable &table;

    // Shared across every render of this document when the caller
    // provides one; null means fall back to the per-Interpreter
    // font_cache below.
    RenderCache *cache = nullptr;

    std::vector<GState> gs;
    std::vector<pdfobj::Object> operands;
    std::vector<SubPath> path;
    DPoint current_point{0, 0}, subpath_start{0, 0};
    bool has_current_point = false;
    bool pending_clip = false;
    gfx::raster::FillRule pending_clip_rule = gfx::raster::FillRule::kNonZero;
    int form_depth = 0;

    // Text object state (spec 9.4.2): Tm/Tlm reset at every BT, unlike
    // the rest of text state which lives in GState and survives q/Q/BT/ET.
    Mat2D text_matrix, text_line_matrix;

    // Fonts are cached per Font dict object number so repeated Tf calls
    // (or repeated glyphs under one Tf) don't re-parse/re-decode the
    // embedded font program every time -- scoped to this one
    // Interpreter instance (a Form XObject's recursive sub-Interpreter,
    // see DoXObject, gets its own cache; not shared across Do calls,
    // a deliberate simplicity-over-micro-optimization tradeoff, see
    // PDFIUM_REMOVAL_PLAN.md Phase 10's own note on this).
    std::unordered_map<int, std::shared_ptr<pdffont::PdfFont>> font_cache;
    // Which object number each resolved font came from, so a cached
    // glyph can be keyed by it. -1 for a font that was not an indirect
    // reference and so has no stable identity to cache under.
    std::unordered_map<const pdffont::PdfFont *, int> font_obj_of_;

    // Non-null only for text extraction (ExtractContentStreamText,
    // PDFIUM_REMOVAL_PLAN.md Phase 11) -- ShowText appends one TextGlyph
    // per code shown when set, in ADDITION to (not instead of) its
    // normal rendering, so the exact same interpreter pass serves both
    // purposes at once and can never disagree with itself about
    // position. A real render call (RenderContentStream) simply never
    // sets this.
    std::vector<TextGlyph> *text_output = nullptr;
    // With it, where the non-text content of each marked-content sequence
    // is painted (see MarkedBox).
    std::vector<MarkedBox> *marked_output = nullptr;

    // The marked-content sequences open here (BMC/BDC ... EMC, spec
    // 14.6), innermost last. Only kept while extracting.
    struct Marked {
        int mcid = -1;
        bool artifact = false;
        std::string actual_text;
    };
    std::vector<Marked> marked;

    int CurrentMcid() const {
        for (size_t i = marked.size(); i-- > 0;)
            if (marked[i].mcid >= 0) return marked[i].mcid;
        return -1;
    }
    bool InArtifact() const {
        for (const Marked &m : marked)
            if (m.artifact) return true;
        return false;
    }
    // BMC / BDC: the tag, and for BDC its property list -- a dict, or a
    // name looked up in the resources' /Properties.
    void BeginMarked(const pdfobj::Object &resources) {
        Marked m;
        if (!text_output && !marked_output) return;
        pdfobj::Object props;
        std::string tag;
        if (operands.size() >= 2) {
            tag = operands[operands.size() - 2].AsString("");
            props = operands.back();
            if (!props.IsDict() && resources.IsDict()) {
                if (const pdfobj::Object *table_entry = resources.Find("Properties")) {
                    pdfobj::Object all = Deref(doc_data, doc_len, table, *table_entry);
                    if (const pdfobj::Object *named = all.Find(props.AsString(""))) props = Deref(doc_data, doc_len, table, *named);
                }
            }
        } else if (!operands.empty()) {
            tag = operands.back().AsString("");
        }
        m.artifact = tag == "Artifact";
        if (props.IsDict()) {
            if (const pdfobj::Object *id = props.Find("MCID"))
                if (id->IsNumber()) m.mcid = static_cast<int>(id->AsDouble());
            if (const pdfobj::Object *actual = props.Find("ActualText"))
                if (actual->IsString()) m.actual_text = pdfobj::TextStringToUtf8(actual->str_val);
        }
        marked.push_back(std::move(m));
    }
    void EndMarked() {
        if (!marked.empty()) marked.pop_back();
    }
    // Something other than text was painted over these device points.
    void NoteMarkedBox(double x0, double y0, double x1, double y1) {
        if (!marked_output) return;
        const int mcid = CurrentMcid();
        if (mcid < 0) return;
        MarkedBox b;
        b.mcid = mcid;
        b.left = std::min(x0, x1);
        b.right = std::max(x0, x1);
        b.bottom = std::min(y0, y1);
        b.top = std::max(y0, y1);
        marked_output->push_back(b);
    }
    void NoteMarkedPath() {
        if (!marked_output || path.empty() || CurrentMcid() < 0) return;
        bool any = false;
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        for (const SubPath &sp : path)
            for (const DPoint &pt : sp.points) {
                if (!any) x0 = x1 = pt.x, y0 = y1 = pt.y, any = true;
                x0 = std::min(x0, pt.x), x1 = std::max(x1, pt.x);
                y0 = std::min(y0, pt.y), y1 = std::max(y1, pt.y);
            }
        if (any) NoteMarkedBox(x0, y0, x1, y1);
    }
    void NoteMarkedImage() {
        if (!marked_output) return;
        double xs[4], ys[4];
        Transform(Top().ctm, 0, 0, &xs[0], &ys[0]);
        Transform(Top().ctm, 1, 0, &xs[1], &ys[1]);
        Transform(Top().ctm, 1, 1, &xs[2], &ys[2]);
        Transform(Top().ctm, 0, 1, &xs[3], &ys[3]);
        NoteMarkedBox(*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
                      *std::max_element(ys, ys + 4));
    }

    // The resources dict the current Run() executes against -- kept
    // here so ShowText can hand a Type 3 glyph procedure the page's (or
    // enclosing Form's) resources when the font has none of its own.
    const pdfobj::Object *run_resources = nullptr;

    // Set by a Type 3 glyph procedure's `d1` operator (spec 9.6.5): the
    // glyph is a stencil painted in the text fill color, and any color
    // operators inside it shall be ignored.
    bool color_locked = false;

    GState &Top() { return gs.back(); }

    std::shared_ptr<pdffont::PdfFont> ResolveFont(const std::string &name, const pdfobj::Object &resources) {
        const pdfobj::Object *font_dict_entry = resources.IsDict() ? resources.Find("Font") : nullptr;
        if (!font_dict_entry) return nullptr;
        pdfobj::Object fonts = Deref(doc_data, doc_len, table, *font_dict_entry);
        const pdfobj::Object *ref = fonts.Find(name);
        if (!ref) return nullptr;
        int key = ref->IsReference() ? ref->ref_val.num : -1;
        if (key >= 0) {
            auto it = font_cache.find(key);
            if (it != font_cache.end()) return it->second;
        }
        // Resolve the dict before consulting the shared cache: a font
        // that is not an indirect reference has no stable key and must
        // be loaded inline.
        const unsigned char *data = doc_data;
        const size_t len = doc_len;
        const pdfxref::XrefTable &tbl = table;
        pdfobj::Object entry = *ref;
        auto load = [data, len, &tbl, entry]() -> std::shared_ptr<pdffont::PdfFont> {
            pdfobj::Object font_dict = Deref(data, len, tbl, entry);
            if (!font_dict.IsDict()) return nullptr;
            auto f = std::make_shared<pdffont::PdfFont>();
            f->Load(data, len, tbl, font_dict);
            return f;
        };
        std::shared_ptr<pdffont::PdfFont> font = (cache && key >= 0) ? cache->Font(key, load) : load();
        if (!font) return nullptr;
        // Kept locally as well, so a second Tf for the same font in this
        // stream costs no lock at all.
        if (key >= 0) font_cache[key] = font;
        font_obj_of_[font.get()] = key;
        return font;
    }

    // Shows one PDF string's worth of glyphs (spec 9.4.3), advancing
    // text_matrix per code shown. Vertical writing mode (Identity-V and
    // friends) is out of scope -- Scoping decision, see this plan's own
    // writeup -- only horizontal advance is implemented.
    void ShowText(const std::string &bytes) {
        GState &g = Top();
        if (!g.font) return;
        int bpc = g.font->BytesPerCode();
        double h_frac = g.h_scale / 100.0;

        for (size_t i = 0; i + static_cast<size_t>(bpc) <= bytes.size(); i += static_cast<size_t>(bpc)) {
            uint32_t code = static_cast<unsigned char>(bytes[i]);
            if (bpc == 2) code = (code << 8) | static_cast<unsigned char>(bytes[i + 1]);

            // Trm = [Tfs*Th 0 0; 0 Tfs 0; 0 Trise 1] * Tm * CTM (spec 9.4.4).
            Mat2D text_space_matrix{g.font_size * h_frac, 0, 0, g.font_size, 0, g.rise};
            Mat2D trm = Multiply(text_space_matrix, Multiply(text_matrix, g.ctm));
            double origin_x, origin_y;
            Transform(trm, 0, 0, &origin_x, &origin_y);
            // Rasterize the glyph -- unless this run is only after the
            // text, in which case there is nothing to draw it onto. An
            // extraction pass is given a throwaway 1x1 canvas, so every
            // glyph was being outlined from its charstring, allocated,
            // composited against that one pixel and freed, purely to be
            // discarded. It is about 98% of extraction time: on the
            // fixtures, pulling a page's glyphs out took 3.5-4.5ms while
            // the row grouping and text assembly that actually use the
            // result took 0.06ms. Search re-extracts every page of the
            // document, so it paid that per page.
            //
            // Nothing in the glyph bitmap feeds the extracted text: the
            // text comes from the code's /ToUnicode, and DrawType3Glyph
            // deliberately runs a glyph procedure with no text_output of
            // its own (see its comment).
            if (g.render_mode != 3 && text_output == nullptr) {  // 3 = invisible
                const float *rgb = (g.render_mode == 1) ? g.stroke_rgb : g.fill_rgb;  // 1=stroke-only: approximate with stroke color, no separate outline-only rendering
                const float alpha = (g.render_mode == 1) ? g.stroke_alpha : g.fill_alpha;
                if (g.font->IsType3()) {
                    // A Type 3 glyph is a content stream, not an
                    // outline, so there is no bitmap to cache -- it runs
                    // through a sub-interpreter against the live canvas.
                    DrawType3Glyph(code, trm);
                } else {
                    const float ma = static_cast<float>(trm.a), mb = static_cast<float>(trm.b);
                    const float mc = static_cast<float>(trm.c), md = static_cast<float>(trm.d);
                    // Where the pen sits *within* its pixel. Composite
                    // time rounds the origin to a whole pixel, so
                    // without this each glyph is displaced by up to half
                    // a pixel horizontally and vertically, independently
                    // of its neighbours. Running prose hides that;
                    // stacked mathematics does not, because a fraction
                    // bar and the numerator above it get unrelated
                    // displacements.
                    //
                    // Quantized rather than taken exactly, so the glyph
                    // cache still hits: a run of text shares a matrix,
                    // and N phases per axis costs N*N cache entries per
                    // glyph instead of one rasterization per occurrence.
                    //
                    // Two per axis, measured. Against poppler at 144dpi
                    // on the maths fixtures, two phases scored
                    // 0.0355/0.0382/0.0367 RMSE and four scored
                    // 0.0368/0.0374/0.0364 -- the same within noise --
                    // while four made a warm re-render at a new zoom
                    // about 35% slower (5.6ms against 3.5ms a page),
                    // four times the cache entries meaning that many
                    // fewer hits. Half-pixel placement already removes
                    // most of the misalignment; the rest is not worth
                    // the cache.
                    constexpr int kPhaseSteps = 2;
                    auto phase_of = [](double v) {
                        double frac = v - std::floor(v);
                        int q = static_cast<int>(std::lround(frac * kPhaseSteps));
                        return q % kPhaseSteps;  // a frac that rounds to 1.0 is phase 0 of the next pixel
                    };
                    const int px_phase = phase_of(origin_x), py_phase = phase_of(origin_y);
                    const float sub_x = static_cast<float>(px_phase) / kPhaseSteps;
                    const float sub_y = static_cast<float>(py_phase) / kPhaseSteps;
                    // Every glyph in a text run shares this matrix, and
                    // a page reuses a few dozen (code, matrix) pairs
                    // across thousands of occurrences, so this is where
                    // caching pays. Without it each occurrence was
                    // outlined from its charstring, allocated,
                    // composited and freed again.
                    auto raster = [&]() -> std::shared_ptr<RenderCache::Glyph> {
                        int gw = 0, gh = 0, xoff = 0, yoff = 0;
                        unsigned char *bmp =
                            g.font->GetGlyphBitmapMatrix(code, ma, mb, mc, md, &gw, &gh, &xoff, &yoff, sub_x, sub_y);
                        if (!bmp) return nullptr;
                        auto out = std::make_shared<RenderCache::Glyph>();
                        out->w = gw;
                        out->h = gh;
                        out->xoff = xoff;
                        out->yoff = yoff;
                        const size_t n = static_cast<size_t>(gw) * static_cast<size_t>(gh);
                        out->bitmap.assign(bmp, bmp + n);
                        g.font->FreeGlyphBitmap(bmp);
                        return out;
                    };
                    const int font_obj = [&]() {
                        auto it = font_obj_of_.find(g.font.get());
                        return it == font_obj_of_.end() ? -1 : it->second;
                    }();
                    if (cache && font_obj >= 0) {
                        // The phase joins the code in the key: the same
                        // glyph at the same size is a different bitmap
                        // at a different sub-pixel position.
                        const unsigned int keyed_code =
                            (code << 4) | (static_cast<unsigned int>(px_phase) << 2) |
                            static_cast<unsigned int>(py_phase);
                        std::shared_ptr<const RenderCache::Glyph> cached =
                            cache->GlyphBitmap(font_obj, keyed_code, ma, mb, mc, md, raster);
                        if (cached)
                            CompositeGlyphBitmap(canvas, cached->bitmap.data(), cached->w, cached->h, origin_x,
                                                 origin_y, cached->xoff, cached->yoff, g.clip, rgb, alpha);
                    } else {
                        // No shared cache, or a font with no stable
                        // object number to key one by.
                        std::shared_ptr<RenderCache::Glyph> made = raster();
                        if (made)
                            CompositeGlyphBitmap(canvas, made->bitmap.data(), made->w, made->h, origin_x, origin_y,
                                                 made->xoff, made->yoff, g.clip, rgb, alpha);
                    }
                }
            }

            double w0 = g.font->GetWidth(code);

            if (text_output) {
                std::string text = g.font->GetUnicodeText(code);
                if (!text.empty()) {
                    // The glyph's em box, transformed corner by corner
                    // through the full text render matrix and then
                    // bounded. Ascent/descent stay approximate
                    // (0.75em/-0.2em from the baseline) because no real
                    // per-font ascent/descent is plumbed through PdfFont
                    // for this purpose -- a rough highlight box, not
                    // pixel-exact glyph bounds, is the norm for PDF
                    // search UIs generally.
                    //
                    // Taking all four corners rather than just the
                    // baseline's two endpoints is what makes this hold
                    // for rotated text. The advance used to be measured
                    // along x and the em height along y independently,
                    // which for 90-degree-rotated text (a landscape
                    // page, a rotated axis label) put the whole advance
                    // into the vertical extent and left left == right:
                    // every box on such a page was zero-width, so the
                    // row grouper and the word-gap test had no usable
                    // horizontal geometry and a landscape page
                    // extracted one character per line. For unrotated
                    // text this is arithmetically identical to what it
                    // replaced.
                    const double ex = w0 / 1000.0;
                    const double asc = 0.75, desc = -0.2;
                    const double corners[4][2] = {{0, desc}, {ex, desc}, {0, asc}, {ex, asc}};
                    double min_x = 0, max_x = 0, min_y = 0, max_y = 0;
                    for (int ci = 0; ci < 4; ++ci) {
                        double cx, cy;
                        Transform(trm, corners[ci][0], corners[ci][1], &cx, &cy);
                        if (ci == 0) {
                            min_x = max_x = cx;
                            min_y = max_y = cy;
                        } else {
                            min_x = std::min(min_x, cx);
                            max_x = std::max(max_x, cx);
                            min_y = std::min(min_y, cy);
                            max_y = std::max(max_y, cy);
                        }
                    }
                    TextGlyph tg;
                    tg.left = min_x;
                    tg.right = max_x;
                    tg.bottom = min_y;
                    tg.top = max_y;
                    tg.utf8_text = std::move(text);
                    tg.mcid = CurrentMcid();
                    tg.artifact = InArtifact();
                    for (size_t mi = marked.size(); mi-- > 0;)
                        if (!marked[mi].actual_text.empty()) {
                            tg.actual_text = marked[mi].actual_text;
                            break;
                        }
                    text_output->push_back(std::move(tg));
                }
            }
            double word_spacing = g.font->WordSpacingApplies(code) ? g.word_spacing : 0.0;
            double tx = ((w0 / 1000.0) * g.font_size + g.char_spacing + word_spacing) * h_frac;
            text_matrix = Multiply(Mat2D{1, 0, 0, 1, tx, 0}, text_matrix);
        }
    }

    // TJ array element that's a number: per spec 9.4.3, subtracted
    // (in thousandths of a text-space unit) from the advance -- applied
    // as its own zero-glyph "advance-only" step between string runs.
    void ApplyTjAdjustment(double adjustment) {
        GState &g = Top();
        double h_frac = g.h_scale / 100.0;
        double tx = (-adjustment / 1000.0) * g.font_size * h_frac;
        text_matrix = Multiply(Mat2D{1, 0, 0, 1, tx, 0}, text_matrix);
    }

    // Type 3 glyph (spec 9.6.5): runs the code's /CharProcs content
    // stream in a sub-interpreter whose CTM is FontMatrix x Trm, exactly
    // like a Form XObject (DoXObject) but with the text state's fill
    // color as both fill and stroke color (a `d1` stencil glyph paints
    // in the fill color, and even `d0` glyphs conventionally inherit it).
    void DrawType3Glyph(uint32_t code, const Mat2D &trm) {
        if (form_depth > 16) return;
        std::string proc;
        if (!Top().font->Type3CharProc(code, &proc)) return;
        const double *fm = Top().font->Type3FontMatrix();
        Mat2D glyph_ctm = Multiply(Mat2D{fm[0], fm[1], fm[2], fm[3], fm[4], fm[5]}, trm);
        const pdfobj::Object &font_res = Top().font->Type3Resources();
        const pdfobj::Object *res = font_res.IsDict() ? &font_res : run_resources;

        ++form_depth;
        Interpreter sub{canvas, doc_data, doc_len, table};
        sub.cache = cache;
        sub.gs.push_back(Top());
        sub.Top().ctm = glyph_ctm;
        for (int k = 0; k < 3; ++k) sub.Top().stroke_rgb[k] = Top().fill_rgb[k];
        sub.Top().stroke_alpha = Top().fill_alpha;
        sub.form_depth = form_depth;
        // No text_output: a glyph procedure's own nested text (rare) is
        // not this glyph's extractable text -- the code's /ToUnicode is.
        sub.Run(proc, res ? *res : pdfobj::Object());
        --form_depth;
    }

    double Num(size_t index_from_end) const {
        if (index_from_end >= operands.size()) return 0;
        return operands[operands.size() - 1 - index_from_end].AsDouble();
    }

    void MoveTo(double x, double y) {
        double dx, dy;
        Transform(Top().ctm, x, y, &dx, &dy);
        current_point = {dx, dy};
        subpath_start = current_point;
        has_current_point = true;
        path.push_back(SubPath{{current_point}, false});
    }

    void LineTo(double x, double y) {
        if (path.empty()) MoveTo(x, y);
        double dx, dy;
        Transform(Top().ctm, x, y, &dx, &dy);
        current_point = {dx, dy};
        path.back().points.push_back(current_point);
    }

    void CurveTo(double x1, double y1, double x2, double y2, double x3, double y3) {
        if (path.empty()) MoveTo(x1, y1);
        DPoint p0 = current_point;
        DPoint c1, c2, p1;
        Transform(Top().ctm, x1, y1, &c1.x, &c1.y);
        Transform(Top().ctm, x2, y2, &c2.x, &c2.y);
        Transform(Top().ctm, x3, y3, &p1.x, &p1.y);
        FlattenCubicToPoints(path.back().points, p0, c1, c2, p1, 0);
        current_point = p1;
    }

    void Rect(double x, double y, double w, double h) {
        DPoint p0, p1, p2, p3;
        Transform(Top().ctm, x, y, &p0.x, &p0.y);
        Transform(Top().ctm, x + w, y, &p1.x, &p1.y);
        Transform(Top().ctm, x + w, y + h, &p2.x, &p2.y);
        Transform(Top().ctm, x, y + h, &p3.x, &p3.y);
        path.push_back(SubPath{{p0, p1, p2, p3}, true});
        current_point = p0;
        subpath_start = p0;
        has_current_point = true;
    }

    void ClosePath() {
        if (!path.empty()) {
            path.back().closed = true;
            current_point = subpath_start;
        }
    }

    void FinishPaint(bool do_fill, gfx::raster::FillRule fill_rule, bool do_stroke) {
        if (do_fill || do_stroke) NoteMarkedPath();
        if (do_fill && !path.empty()) {
            auto coverage = RasterizeFill(path, canvas.width, canvas.height, fill_rule);
            CompositeCoverage(canvas, coverage, Top().clip, Top().fill_rgb, Top().fill_alpha);
        }
        if (do_stroke && !path.empty()) {
            const double scale = CtmScale(Top().ctm);
            double half = std::max(1.0, Top().line_width * scale) / 2.0;
            std::vector<SubPath> dashed;
            if (!Top().dash.empty()) {
                std::vector<double> dev_dash = Top().dash;
                for (double &d : dev_dash) d *= scale;
                dashed = DashPath(path, dev_dash, Top().dash_phase * scale);
            }
            auto coverage = RasterizeStroke(Top().dash.empty() ? path : dashed, half, canvas.width, canvas.height);
            CompositeCoverage(canvas, coverage, Top().clip, Top().stroke_rgb, Top().stroke_alpha);
        }
        if (pending_clip && !path.empty()) {
            Top().clip = IntersectMask(Top().clip, RasterizeFill(path, canvas.width, canvas.height, pending_clip_rule));
        }
        pending_clip = false;
        path.clear();
        has_current_point = false;
    }

    void SetColorSpace(bool is_fill, const pdfobj::Object &name_obj, const pdfobj::Object &resources) {
        pdfobj::Object cs_obj = name_obj;
        std::string name = name_obj.AsString("");
        if (name != "DeviceGray" && name != "DeviceRGB" && name != "DeviceCMYK" && name != "Pattern") {
            const pdfobj::Object *cs_dict = resources.IsDict() ? resources.Find("ColorSpace") : nullptr;
            if (cs_dict) {
                pdfobj::Object resolved_dict = Deref(doc_data, doc_len, table, *cs_dict);
                if (const pdfobj::Object *found = resolved_dict.Find(name)) cs_obj = *found;
            }
        }
        ColorSpaceInfo info = BuildColorSpaceInfo(doc_data, doc_len, table, cs_obj);
        if (is_fill) {
            Top().fill_cs = info;
            Top().fill_rgb[0] = Top().fill_rgb[1] = Top().fill_rgb[2] = 0;
        } else {
            Top().stroke_cs = info;
            Top().stroke_rgb[0] = Top().stroke_rgb[1] = Top().stroke_rgb[2] = 0;
        }
    }

    void SetColor(bool is_fill) {
        std::vector<double> comps;
        for (const auto &o : operands) {
            if (o.IsNumber()) comps.push_back(o.AsDouble());
        }
        const ColorSpaceInfo &cs = is_fill ? Top().fill_cs : Top().stroke_cs;
        float rgb[3];
        ApplyColor(cs, comps, rgb);
        float *dst = is_fill ? Top().fill_rgb : Top().stroke_rgb;
        dst[0] = rgb[0];
        dst[1] = rgb[1];
        dst[2] = rgb[2];
    }

    // `d` / an ExtGState's /D: `arr` is the on/off array (an odd-length
    // one repeats -- spec: [3] means 3 on, 3 off).
    void SetDash(const pdfobj::Object &arr, double phase) {
        Top().dash.clear();
        if (arr.IsArray()) {
            for (const auto &v : arr.array_val) Top().dash.push_back(v.AsDouble());
        }
        if (Top().dash.size() % 2 == 1) {
            const std::vector<double> once = Top().dash;
            Top().dash.insert(Top().dash.end(), once.begin(), once.end());
        }
        Top().dash_phase = phase;
    }

    void ApplyExtGState(const std::string &name, const pdfobj::Object &resources) {
        const pdfobj::Object *eg_dict = resources.IsDict() ? resources.Find("ExtGState") : nullptr;
        if (!eg_dict) return;
        pdfobj::Object resolved = Deref(doc_data, doc_len, table, *eg_dict);
        const pdfobj::Object *entry = resolved.Find(name);
        if (!entry) return;
        pdfobj::Object eg = Deref(doc_data, doc_len, table, *entry);
        if (const pdfobj::Object *ca = eg.Find("ca")) Top().fill_alpha = static_cast<float>(ca->AsDouble(1.0));
        if (const pdfobj::Object *CA = eg.Find("CA")) Top().stroke_alpha = static_cast<float>(CA->AsDouble(1.0));
        if (const pdfobj::Object *lw = eg.Find("LW")) Top().line_width = lw->AsDouble(Top().line_width);
        if (const pdfobj::Object *d = eg.Find("D")) {
            pdfobj::Object dd = Deref(doc_data, doc_len, table, *d);
            if (dd.IsArray() && dd.array_val.size() >= 2) {
                SetDash(Deref(doc_data, doc_len, table, dd.array_val[0]), dd.array_val[1].AsDouble());
            }
        }
    }

    void DoXObject(const std::string &name, const pdfobj::Object &resources);

    static bool IsColorOperator(const std::string &op) {
        return op == "g" || op == "G" || op == "rg" || op == "RG" || op == "k" || op == "K" || op == "cs" ||
               op == "CS" || op == "sc" || op == "scn" || op == "SC" || op == "SCN";
    }

    void Run(const std::string &content, const pdfobj::Object &resources) {
        run_resources = &resources;
        size_t pos = 0;
        const unsigned char *data = reinterpret_cast<const unsigned char *>(content.data());
        size_t len = content.size();
        ContentToken tok;
        while (NextContentToken(data, len, pos, &tok)) {
            if (!tok.is_operator) {
                operands.push_back(std::move(tok.operand));
                continue;
            }
            const std::string &op = tok.op;
            if (color_locked && IsColorOperator(op)) {
                // Inside a `d1` Type 3 glyph: color operators are ignored (spec 9.6.5).
            } else if (op == "d1") {
                color_locked = true;  // `d0` (colored glyph) needs nothing: colors apply normally
            } else if (op == "q") {
                gs.push_back(Top());
            } else if (op == "Q") {
                if (gs.size() > 1) gs.pop_back();
            } else if (op == "cm" && operands.size() >= 6) {
                Mat2D m{Num(5), Num(4), Num(3), Num(2), Num(1), Num(0)};
                Top().ctm = Multiply(m, Top().ctm);
            } else if (op == "w" && !operands.empty()) {
                Top().line_width = Num(0);
            } else if (op == "d" && operands.size() >= 2) {
                SetDash(operands[operands.size() - 2], Num(0));
            } else if (op == "m" && operands.size() >= 2) {
                MoveTo(Num(1), Num(0));
            } else if (op == "l" && operands.size() >= 2) {
                LineTo(Num(1), Num(0));
            } else if (op == "c" && operands.size() >= 6) {
                CurveTo(Num(5), Num(4), Num(3), Num(2), Num(1), Num(0));
            } else if (op == "v" && operands.size() >= 4) {
                // 'v''s implicit first control point IS the current point --
                // already in device space, so no extra transform applies.
                if (path.empty()) MoveTo(Num(3), Num(2));
                DPoint p0 = current_point;
                DPoint c2, p1;
                Transform(Top().ctm, Num(3), Num(2), &c2.x, &c2.y);
                Transform(Top().ctm, Num(1), Num(0), &p1.x, &p1.y);
                FlattenCubicToPoints(path.back().points, p0, p0, c2, p1, 0);
                current_point = p1;
            } else if (op == "y" && operands.size() >= 4) {
                if (path.empty()) MoveTo(Num(3), Num(2));
                DPoint p0 = current_point;
                DPoint c1, p1;
                Transform(Top().ctm, Num(3), Num(2), &c1.x, &c1.y);
                Transform(Top().ctm, Num(1), Num(0), &p1.x, &p1.y);
                FlattenCubicToPoints(path.back().points, p0, c1, p1, p1, 0);
                current_point = p1;
            } else if (op == "re" && operands.size() >= 4) {
                Rect(Num(3), Num(2), Num(1), Num(0));
            } else if (op == "h") {
                ClosePath();
            } else if (op == "S") {
                FinishPaint(false, gfx::raster::FillRule::kNonZero, true);
            } else if (op == "s") {
                ClosePath();
                FinishPaint(false, gfx::raster::FillRule::kNonZero, true);
            } else if (op == "f" || op == "F") {
                FinishPaint(true, gfx::raster::FillRule::kNonZero, false);
            } else if (op == "f*") {
                FinishPaint(true, gfx::raster::FillRule::kEvenOdd, false);
            } else if (op == "B") {
                FinishPaint(true, gfx::raster::FillRule::kNonZero, true);
            } else if (op == "B*") {
                FinishPaint(true, gfx::raster::FillRule::kEvenOdd, true);
            } else if (op == "b") {
                ClosePath();
                FinishPaint(true, gfx::raster::FillRule::kNonZero, true);
            } else if (op == "b*") {
                ClosePath();
                FinishPaint(true, gfx::raster::FillRule::kEvenOdd, true);
            } else if (op == "n") {
                FinishPaint(false, gfx::raster::FillRule::kNonZero, false);
            } else if (op == "W") {
                pending_clip = true;
                pending_clip_rule = gfx::raster::FillRule::kNonZero;
            } else if (op == "W*") {
                pending_clip = true;
                pending_clip_rule = gfx::raster::FillRule::kEvenOdd;
            } else if (op == "g" && !operands.empty()) {
                Top().fill_cs = {CsKind::kDeviceGray, 1, nullptr, ""};
                float v = static_cast<float>(Num(0));
                Top().fill_rgb[0] = Top().fill_rgb[1] = Top().fill_rgb[2] = v;
            } else if (op == "G" && !operands.empty()) {
                Top().stroke_cs = {CsKind::kDeviceGray, 1, nullptr, ""};
                float v = static_cast<float>(Num(0));
                Top().stroke_rgb[0] = Top().stroke_rgb[1] = Top().stroke_rgb[2] = v;
            } else if (op == "rg" && operands.size() >= 3) {
                Top().fill_cs = {CsKind::kDeviceRGB, 3, nullptr, ""};
                Top().fill_rgb[0] = static_cast<float>(Num(2));
                Top().fill_rgb[1] = static_cast<float>(Num(1));
                Top().fill_rgb[2] = static_cast<float>(Num(0));
            } else if (op == "RG" && operands.size() >= 3) {
                Top().stroke_cs = {CsKind::kDeviceRGB, 3, nullptr, ""};
                Top().stroke_rgb[0] = static_cast<float>(Num(2));
                Top().stroke_rgb[1] = static_cast<float>(Num(1));
                Top().stroke_rgb[2] = static_cast<float>(Num(0));
            } else if (op == "k" && operands.size() >= 4) {
                Top().fill_cs = {CsKind::kDeviceCMYK, 4, nullptr, ""};
                CmykToRgb(Num(3), Num(2), Num(1), Num(0), Top().fill_rgb);
            } else if (op == "K" && operands.size() >= 4) {
                Top().stroke_cs = {CsKind::kDeviceCMYK, 4, nullptr, ""};
                CmykToRgb(Num(3), Num(2), Num(1), Num(0), Top().stroke_rgb);
            } else if (op == "cs" && !operands.empty()) {
                SetColorSpace(true, operands.back(), resources);
            } else if (op == "CS" && !operands.empty()) {
                SetColorSpace(false, operands.back(), resources);
            } else if (op == "sc" || op == "scn") {
                SetColor(true);
            } else if (op == "SC" || op == "SCN") {
                SetColor(false);
            } else if (op == "gs" && !operands.empty()) {
                ApplyExtGState(operands.back().AsString(""), resources);
            } else if (op == "Do" && !operands.empty()) {
                DoXObject(operands.back().AsString(""), resources);
            } else if (op == "BMC" || op == "BDC") {
                BeginMarked(resources);
            } else if (op == "EMC") {
                EndMarked();
            } else if (op == "BT") {
                text_matrix = Mat2D{};
                text_line_matrix = Mat2D{};
            } else if (op == "ET") {
                // No state to tear down -- text state itself lives in
                // GState (survives BT/ET per spec), only Tm/Tlm reset.
            } else if (op == "Tc" && !operands.empty()) {
                Top().char_spacing = Num(0);
            } else if (op == "Tw" && !operands.empty()) {
                Top().word_spacing = Num(0);
            } else if (op == "Tz" && !operands.empty()) {
                Top().h_scale = Num(0);
            } else if (op == "TL" && !operands.empty()) {
                Top().leading = Num(0);
            } else if (op == "Tf" && operands.size() >= 2) {
                Top().font = ResolveFont(operands[operands.size() - 2].AsString(""), resources);
                Top().font_size = Num(0);
            } else if (op == "Tr" && !operands.empty()) {
                Top().render_mode = static_cast<int>(Num(0));
            } else if (op == "Ts" && !operands.empty()) {
                Top().rise = Num(0);
            } else if (op == "Td" && operands.size() >= 2) {
                Mat2D translate{1, 0, 0, 1, Num(1), Num(0)};
                text_line_matrix = Multiply(translate, text_line_matrix);
                text_matrix = text_line_matrix;
            } else if (op == "TD" && operands.size() >= 2) {
                Top().leading = -Num(0);
                Mat2D translate{1, 0, 0, 1, Num(1), Num(0)};
                text_line_matrix = Multiply(translate, text_line_matrix);
                text_matrix = text_line_matrix;
            } else if (op == "Tm" && operands.size() >= 6) {
                text_line_matrix = Mat2D{Num(5), Num(4), Num(3), Num(2), Num(1), Num(0)};
                text_matrix = text_line_matrix;
            } else if (op == "T*") {
                Mat2D translate{1, 0, 0, 1, 0, -Top().leading};
                text_line_matrix = Multiply(translate, text_line_matrix);
                text_matrix = text_line_matrix;
            } else if (op == "Tj" && !operands.empty()) {
                ShowText(operands.back().str_val);
            } else if (op == "'" && !operands.empty()) {
                Mat2D translate{1, 0, 0, 1, 0, -Top().leading};
                text_line_matrix = Multiply(translate, text_line_matrix);
                text_matrix = text_line_matrix;
                ShowText(operands.back().str_val);
            } else if (op == "\"" && operands.size() >= 3) {
                Top().word_spacing = Num(2);
                Top().char_spacing = Num(1);
                Mat2D translate{1, 0, 0, 1, 0, -Top().leading};
                text_line_matrix = Multiply(translate, text_line_matrix);
                text_matrix = text_line_matrix;
                ShowText(operands.back().str_val);
            } else if (op == "TJ" && !operands.empty() && operands.back().IsArray()) {
                for (const auto &elem : operands.back().array_val) {
                    if (elem.IsString()) {
                        ShowText(elem.str_val);
                    } else if (elem.IsNumber()) {
                        ApplyTjAdjustment(elem.AsDouble());
                    }
                }
            } else if (op == "BI") {
                // Inline image: "BI" is followed by bare /Key value pairs
                // (NOT wrapped in << >>, unlike every other PDF dict) using
                // abbreviated key names (spec Table 93), then "ID", one
                // whitespace byte, the raw data, then "EI".
                pdfobj::Object dict;
                dict.type = pdfobj::Type::Dict;
                while (true) {
                    pdfobj::SkipWhitespaceAndComments(data, len, pos);
                    if (pos + 2 <= len && data[pos] == 'I' && data[pos + 1] == 'D' &&
                        (pos + 2 == len || IsWs(data[pos + 2]))) {
                        pos += 2;
                        break;
                    }
                    ContentToken key_tok;
                    if (!NextContentToken(data, len, pos, &key_tok) || key_tok.is_operator || !key_tok.operand.IsName()) {
                        break;  // malformed: bail rather than loop forever
                    }
                    pdfobj::Object value;
                    if (!pdfobj::ParseObject(data, len, pos, &value)) break;
                    dict.dict_val[key_tok.operand.str_val] = value;
                }
                if (pos < len && IsWs(data[pos])) ++pos;  // exactly one whitespace byte separates "ID" from the data

                size_t data_start = pos;
                bool has_filter = FindKey(dict, "Filter", "F") != nullptr;
                size_t data_end;
                if (!has_filter) {
                    int w = static_cast<int>(FindKey(dict, "Width", "W") ? FindKey(dict, "Width", "W")->AsInt() : 0);
                    int h = static_cast<int>(FindKey(dict, "Height", "H") ? FindKey(dict, "Height", "H")->AsInt() : 0);
                    bool im = FindKey(dict, "ImageMask", "IM") && FindKey(dict, "ImageMask", "IM")->bool_val;
                    int bpc = im ? 1
                                 : static_cast<int>(FindKey(dict, "BitsPerComponent", "BPC")
                                                         ? FindKey(dict, "BitsPerComponent", "BPC")->AsInt()
                                                         : 8);
                    int n = im ? 1 : BuildColorSpaceInfo(doc_data, doc_len, table, FindKey(dict, "ColorSpace", "CS")
                                                                                        ? *FindKey(dict, "ColorSpace", "CS")
                                                                                        : pdfobj::Object())
                                          .n;
                    size_t row_bytes = (static_cast<size_t>(w) * static_cast<size_t>(n) * static_cast<size_t>(bpc) + 7) / 8;
                    size_t exact = row_bytes * static_cast<size_t>(std::max(h, 0));
                    data_end = std::min(len, data_start + exact);
                } else {
                    // Filtered data: exact compressed length isn't known
                    // without decoding it, so fall back to scanning for a
                    // whitespace-bounded "EI" (a plain substring scan is a
                    // deliberate simplification -- filtered inline images
                    // are rare enough in practice, mostly small icons/
                    // masks, that this is an acceptable tolerance).
                    data_end = data_start;
                    while (data_end + 2 <= len) {
                        if (data[data_end] == 'E' && data[data_end + 1] == 'I' &&
                            (data_end == data_start || IsWs(data[data_end - 1])) &&
                            (data_end + 2 == len || IsWs(data[data_end + 2]))) {
                            break;
                        }
                        ++data_end;
                    }
                }

                std::string raw(reinterpret_cast<const char *>(data + data_start), data_end - data_start);
                DecodedImage img;
                if (DecodeImageDict(dict, raw, resources, doc_data, doc_len, table, &img)) {
                    DrawImage(canvas, img, Top().ctm, Top().clip, Top().fill_rgb, Top().fill_alpha);
            NoteMarkedImage();
                }

                pos = data_end;
                pdfobj::SkipWhitespaceAndComments(data, len, pos);
                if (pos + 2 <= len && data[pos] == 'E' && data[pos + 1] == 'I') pos += 2;
            }
            operands.clear();
        }
    }
};

}  // namespace

// -- RenderCache -----------------------------------------------------------

namespace {
// Rough budget for cached glyph bitmaps. A glyph at a normal reading
// scale is a few hundred bytes and one page has on the order of a
// hundred distinct (code, matrix) pairs, so this holds many pages'
// worth at several scales; past it the whole glyph map is dropped
// rather than evicted one by one, which keeps the bookkeeping to a
// counter and costs one re-raster of the pages still on screen.
constexpr size_t kGlyphCacheBudget = 48u * 1024u * 1024u;

unsigned int FloatBits(float f) {
    unsigned int out = 0;
    static_assert(sizeof(out) == sizeof(f), "float is not 32 bits");
    std::memcpy(&out, &f, sizeof(out));
    return out;
}
}  // namespace

size_t RenderCache::GlyphKeyHash::operator()(const GlyphKey &k) const {
    // A plain mix; the keys are small integers and raw float bits, which
    // std::hash would not spread any better.
    size_t h = static_cast<size_t>(k.font_obj) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<size_t>(k.code) + 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
    for (unsigned int v : {k.a, k.b, k.c, k.d}) h ^= static_cast<size_t>(v) + 0x9E3779B9ull + (h << 6) + (h >> 2);
    return h;
}

std::shared_ptr<pdffont::PdfFont> RenderCache::Font(int obj_num,
                                                    const std::function<std::shared_ptr<pdffont::PdfFont>()> &load) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = fonts_.find(obj_num);
        if (it != fonts_.end()) return it->second;
    }
    // Loaded without the lock: parsing an embedded font program is the
    // slow thing this cache exists to avoid, and holding the mutex
    // across it would serialize two panes' renders on it.
    std::shared_ptr<pdffont::PdfFont> loaded = load();
    std::lock_guard<std::mutex> lk(mu_);
    auto it = fonts_.find(obj_num);
    if (it != fonts_.end()) return it->second;  // another thread won; keep one copy
    fonts_[obj_num] = loaded;
    return loaded;
}

std::shared_ptr<const RenderCache::Glyph> RenderCache::GlyphBitmap(
    int font_obj, unsigned int code, float a, float b, float c, float d,
    const std::function<std::shared_ptr<Glyph>()> &render) {
    const GlyphKey key{font_obj, code, FloatBits(a), FloatBits(b), FloatBits(c), FloatBits(d)};
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = glyphs_.find(key);
        if (it != glyphs_.end()) return it->second;
    }
    std::shared_ptr<Glyph> made = render();
    std::lock_guard<std::mutex> lk(mu_);
    auto it = glyphs_.find(key);
    if (it != glyphs_.end()) return it->second;
    if (glyph_bytes_ > kGlyphCacheBudget) {
        glyphs_.clear();
        glyph_bytes_ = 0;
    }
    if (made) glyph_bytes_ += made->bitmap.size();
    std::shared_ptr<const Glyph> stored = made;
    glyphs_[key] = stored;  // a null result is cached too: a bad glyph is not retried per occurrence
    return stored;
}

void RenderCache::Clear() {
    std::lock_guard<std::mutex> lk(mu_);
    fonts_.clear();
    glyphs_.clear();
    glyph_bytes_ = 0;
}

size_t RenderCache::GlyphBytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return glyph_bytes_;
}


void Interpreter::DoXObject(const std::string &name, const pdfobj::Object &resources) {
    if (form_depth > 16) return;  // guard against a malformed self-referential Form chain
    const pdfobj::Object *xobj_dict = resources.IsDict() ? resources.Find("XObject") : nullptr;
    if (!xobj_dict) return;
    pdfobj::Object resolved = Deref(doc_data, doc_len, table, *xobj_dict);
    const pdfobj::Object *entry = resolved.Find(name);
    if (!entry || !entry->IsReference()) return;

    pdfobj::Object stream_dict;
    std::string raw;
    if (!pdfxref::ResolveStream(doc_data, doc_len, table, entry->ref_val.num, entry->ref_val.gen, &stream_dict,
                                 &raw)) {
        return;
    }
    const pdfobj::Object *subtype = stream_dict.Find("Subtype");
    if (subtype && subtype->AsString("") == "Image") {
        DecodedImage img;
        if (DecodeImageDict(stream_dict, raw, resources, doc_data, doc_len, table, &img)) {
            DrawImage(canvas, img, Top().ctm, Top().clip, Top().fill_rgb, Top().fill_alpha);
            NoteMarkedImage();
        }
        return;
    }
    if (!subtype || subtype->AsString("") != "Form") return;

    std::string decoded;
    if (!pdffilter::DecodeStream(raw, &stream_dict, &decoded)) return;

    Mat2D form_matrix;
    if (const pdfobj::Object *m = stream_dict.Find("Matrix")) {
        if (m->IsArray() && m->array_val.size() == 6) {
            form_matrix = Mat2D{m->array_val[0].AsDouble(1), m->array_val[1].AsDouble(0), m->array_val[2].AsDouble(0),
                                 m->array_val[3].AsDouble(1), m->array_val[4].AsDouble(0), m->array_val[5].AsDouble(0)};
        }
    }

    pdfobj::Object form_resources = resources;
    if (const pdfobj::Object *r = stream_dict.Find("Resources")) form_resources = Deref(doc_data, doc_len, table, *r);

    gs.push_back(Top());
    Top().ctm = Multiply(form_matrix, Top().ctm);

    if (const pdfobj::Object *bbox = stream_dict.Find("BBox")) {
        if (bbox->IsArray() && bbox->array_val.size() == 4) {
            double x0 = bbox->array_val[0].AsDouble(), y0 = bbox->array_val[1].AsDouble();
            double x1 = bbox->array_val[2].AsDouble(), y1 = bbox->array_val[3].AsDouble();
            std::vector<SubPath> clip_path;
            DPoint p0, p1, p2, p3;
            Transform(Top().ctm, x0, y0, &p0.x, &p0.y);
            Transform(Top().ctm, x1, y0, &p1.x, &p1.y);
            Transform(Top().ctm, x1, y1, &p2.x, &p2.y);
            Transform(Top().ctm, x0, y1, &p3.x, &p3.y);
            clip_path.push_back(SubPath{{p0, p1, p2, p3}, true});
            Top().clip = IntersectMask(Top().clip,
                                       RasterizeFill(clip_path, canvas.width, canvas.height, gfx::raster::FillRule::kNonZero));
        }
    }

    ++form_depth;
    Interpreter sub{canvas, doc_data, doc_len, table};
    sub.cache = cache;  // a Form XObject's fonts and glyphs are the same document's
    sub.gs.push_back(Top());
    sub.form_depth = form_depth;
    sub.text_output = text_output;  // propagate so text inside a Form XObject is extracted too
    sub.marked_output = marked_output;
    sub.marked = marked;  // a form drawn inside a marked-content sequence is that sequence's content
    sub.Run(decoded, form_resources);
    --form_depth;

    gs.pop_back();
}

std::string GetPageContent(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                            const pdfdoc::Page &page, PageContentStatus *out_status) {
    if (out_status) *out_status = PageContentStatus{};
    const pdfobj::Object *contents = page.dict.Find("Contents");
    if (!contents) return "";
    std::string result;
    auto append_stream = [&](const pdfobj::Object &ref) {
        if (!ref.IsReference()) return;
        if (out_status) out_status->streams_total++;
        pdfobj::Object dict;
        std::string raw;
        if (!pdfxref::ResolveStream(data, len, table, ref.ref_val.num, ref.ref_val.gen, &dict, &raw)) {
            if (out_status) out_status->streams_failed++;
            return;
        }
        std::string decoded;
        if (!pdffilter::DecodeStream(raw, &dict, &decoded)) {
            if (out_status) out_status->streams_failed++;
            return;
        }
        if (!result.empty()) result.push_back(' ');
        result += decoded;
    };
    // /Contents is an array of content streams or a single one -- but the
    // array itself is very often an indirect reference (e.g. a linearized
    // PDF that packs the array object into an ObjStm), not an inline
    // array in the page dict. Deref before the IsArray() check: same
    // un-dereferenced-indirect-reference bug class already fixed for
    // /Widths/W/Resources -- here it silently blanked EVERY page of any
    // document whose /Contents was `N 0 R` -> array (caught by a live
    // render of a real linearized textbook PDF, all pages blank).
    pdfobj::Object resolved = Deref(data, len, table, *contents);
    if (resolved.IsArray()) {
        for (const auto &c : resolved.array_val) append_stream(c);
    } else {
        append_stream(*contents);
    }
    return result;
}

void RenderContentStream(const std::string &content, Canvas &canvas, const Mat2D &initial_ctm,
                          const pdfobj::Object &resources, const unsigned char *doc_data, size_t doc_len,
                          const pdfxref::XrefTable &table, RenderCache *cache) {
    Interpreter interp{canvas, doc_data, doc_len, table};
    interp.cache = cache;
    GState initial;
    initial.ctm = initial_ctm;
    interp.gs.push_back(initial);
    interp.Run(content, resources);
}

void ExtractContentStreamText(const std::string &content, Canvas &canvas, const Mat2D &initial_ctm,
                               const pdfobj::Object &resources, const unsigned char *doc_data, size_t doc_len,
                               const pdfxref::XrefTable &table, std::vector<TextGlyph> *out_glyphs,
                               std::vector<MarkedBox> *out_marked, RenderCache *cache) {
    Interpreter interp{canvas, doc_data, doc_len, table};
    interp.cache = cache;
    GState initial;
    initial.ctm = initial_ctm;
    interp.gs.push_back(initial);
    interp.text_output = out_glyphs;
    interp.marked_output = out_marked;
    interp.Run(content, resources);
}

}  // namespace pdfrender
