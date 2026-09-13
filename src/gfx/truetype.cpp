#include "gfx/truetype.h"

#include "gfx/rasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace gfx {
namespace tt {

namespace {

// -- Big-endian primitive readers -------------------------------------
// Every multi-byte field in an sfnt font is big-endian ("network byte
// order"); no bounds checking beyond what InitFont's own table-length
// checks already establish -- matches stb_truetype's own "trust the
// buffer" contract (see backend_native_text.cpp's LoadFontData comment
// on stbtt_InitFont).

uint8_t U8(const unsigned char *p) { return p[0]; }
int8_t S8(const unsigned char *p) { return static_cast<int8_t>(p[0]); }
uint16_t U16(const unsigned char *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
int16_t S16(const unsigned char *p) { return static_cast<int16_t>(U16(p)); }
uint32_t U32(const unsigned char *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
// F2Dot14: 2's-complement 2.14 fixed point, used by composite-glyph transforms.
float F2Dot14(const unsigned char *p) { return static_cast<float>(S16(p)) / 16384.0f; }

// -- Table directory ----------------------------------------------------

struct RawTable {
    uint32_t off = 0, len = 0;
};

RawTable FindTable(const unsigned char *data, int data_size, const char *tag) {
    if (data_size < 12) return {};
    uint16_t num_tables = U16(data + 4);
    for (int i = 0; i < num_tables; i++) {
        const unsigned char *rec = data + 12 + i * 16;
        if (rec + 16 > data + data_size) break;
        if (std::memcmp(rec, tag, 4) == 0) {
            RawTable t;
            t.off = U32(rec + 8);
            t.len = U32(rec + 12);
            return t;
        }
    }
    return {};
}

// -- cmap: codepoint -> glyph index -------------------------------------
// Only formats 4 (BMP, used by every embedded font as a fallback) and 12
// (full Unicode incl. supplementary planes -- needed for the icon font's
// and JetBrains Mono's Private-Use-Area icon codepoints above U+FFFF,
// confirmed via this plan's own Phase 1 cmap-format inventory) are
// implemented; every shipped font has one of these, so no fallback to
// the legacy format 0/6 subtables is needed.

int LookupFormat4(const unsigned char *sub, int codepoint) {
    if (codepoint > 0xFFFF) return 0;
    uint16_t seg_count_x2 = U16(sub + 6);
    int seg_count = seg_count_x2 / 2;
    const unsigned char *end_codes = sub + 14;
    const unsigned char *start_codes = end_codes + seg_count_x2 + 2;  // +2 skips reservedPad
    const unsigned char *id_deltas = start_codes + seg_count_x2;
    const unsigned char *id_range_offsets = id_deltas + seg_count_x2;
    for (int i = 0; i < seg_count; i++) {
        uint16_t end_code = U16(end_codes + i * 2);
        if (codepoint > end_code) continue;
        uint16_t start_code = U16(start_codes + i * 2);
        if (codepoint < start_code) return 0;
        int16_t id_delta = S16(id_deltas + i * 2);
        uint16_t id_range_offset = U16(id_range_offsets + i * 2);
        if (id_range_offset == 0) {
            return (codepoint + id_delta) & 0xFFFF;
        }
        const unsigned char *glyph_id_addr =
            id_range_offsets + i * 2 + id_range_offset + static_cast<uint32_t>(codepoint - start_code) * 2;
        uint16_t glyph_id = U16(glyph_id_addr);
        if (glyph_id == 0) return 0;
        return (glyph_id + id_delta) & 0xFFFF;
    }
    return 0;
}

int LookupFormat12(const unsigned char *sub, int codepoint) {
    uint32_t num_groups = U32(sub + 12);
    const unsigned char *groups = sub + 16;
    // Groups are sorted by startCharCode; binary search.
    uint32_t lo = 0, hi = num_groups;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const unsigned char *g = groups + mid * 12;
        uint32_t start = U32(g), end = U32(g + 4);
        if (static_cast<uint32_t>(codepoint) < start) {
            hi = mid;
        } else if (static_cast<uint32_t>(codepoint) > end) {
            lo = mid + 1;
        } else {
            uint32_t start_glyph_id = U32(g + 8);
            return static_cast<int>(start_glyph_id + (static_cast<uint32_t>(codepoint) - start));
        }
    }
    return 0;
}

int FindGlyphIndex(const FontInfo *info, int codepoint) {
    if (info->cmap_format == 0 || info->cmap_subtable_off == 0) return 0;
    const unsigned char *sub = info->data + info->cmap_subtable_off;
    if (info->cmap_format == 4) return LookupFormat4(sub, codepoint);
    if (info->cmap_format == 12) return LookupFormat12(sub, codepoint);
    return 0;
}

// -- glyf/loca: glyph outlines -------------------------------------------

uint32_t GlyphOffset(const FontInfo *info, int glyph_index, uint32_t *out_len) {
    if (glyph_index < 0 || glyph_index >= info->num_glyphs) {
        *out_len = 0;
        return 0;
    }
    const unsigned char *loca = info->data + info->loca_off;
    uint32_t off0, off1;
    if (info->loca_long) {
        off0 = U32(loca + static_cast<uint32_t>(glyph_index) * 4);
        off1 = U32(loca + static_cast<uint32_t>(glyph_index + 1) * 4);
    } else {
        off0 = static_cast<uint32_t>(U16(loca + static_cast<uint32_t>(glyph_index) * 2)) * 2;
        off1 = static_cast<uint32_t>(U16(loca + static_cast<uint32_t>(glyph_index + 1) * 2)) * 2;
    }
    *out_len = off1 - off0;
    return info->glyf_off + off0;
}

struct GlyphPoint {
    float x = 0, y = 0;
    bool on_curve = false;
};

using Contour = std::vector<GlyphPoint>;

// Reads a simple-glyph's contours (flags + delta-encoded x/y coordinates
// per the glyf table spec) into `out`, in font units, un-transformed.
void ReadSimpleGlyph(const unsigned char *g, int16_t num_contours, std::vector<Contour> &out) {
    const unsigned char *p = g + 10;
    std::vector<int> end_pts(static_cast<size_t>(num_contours));
    for (int i = 0; i < num_contours; i++) {
        end_pts[static_cast<size_t>(i)] = U16(p);
        p += 2;
    }
    int num_points = num_contours > 0 ? end_pts.back() + 1 : 0;
    uint16_t instruction_len = U16(p);
    p += 2 + instruction_len;

    std::vector<uint8_t> flags(static_cast<size_t>(num_points));
    for (int i = 0; i < num_points;) {
        uint8_t flag = U8(p++);
        flags[static_cast<size_t>(i++)] = flag;
        if (flag & 0x08) {  // REPEAT_FLAG
            uint8_t repeat = U8(p++);
            for (int r = 0; r < repeat && i < num_points; r++) flags[static_cast<size_t>(i++)] = flag;
        }
    }

    std::vector<int> xs(static_cast<size_t>(num_points)), ys(static_cast<size_t>(num_points));
    int x = 0;
    for (int i = 0; i < num_points; i++) {
        uint8_t flag = flags[static_cast<size_t>(i)];
        if (flag & 0x02) {  // X_SHORT_VECTOR
            uint8_t dx = U8(p++);
            x += (flag & 0x10) ? dx : -static_cast<int>(dx);  // SAME_OR_POSITIVE_X
        } else if (!(flag & 0x10)) {  // not short, not "same" -> a full int16 delta follows
            x += S16(p);
            p += 2;
        }
        xs[static_cast<size_t>(i)] = x;
    }
    int y = 0;
    for (int i = 0; i < num_points; i++) {
        uint8_t flag = flags[static_cast<size_t>(i)];
        if (flag & 0x04) {  // Y_SHORT_VECTOR
            uint8_t dy = U8(p++);
            y += (flag & 0x20) ? dy : -static_cast<int>(dy);  // SAME_OR_POSITIVE_Y
        } else if (!(flag & 0x20)) {
            y += S16(p);
            p += 2;
        }
        ys[static_cast<size_t>(i)] = y;
    }

    int start = 0;
    for (int c = 0; c < num_contours; c++) {
        int end = end_pts[static_cast<size_t>(c)];
        Contour contour;
        contour.reserve(static_cast<size_t>(end - start + 1));
        for (int i = start; i <= end; i++) {
            GlyphPoint gp;
            gp.x = static_cast<float>(xs[static_cast<size_t>(i)]);
            gp.y = static_cast<float>(ys[static_cast<size_t>(i)]);
            gp.on_curve = (flags[static_cast<size_t>(i)] & 0x01) != 0;
            contour.push_back(gp);
        }
        out.push_back(std::move(contour));
        start = end + 1;
    }
}

void ReadGlyphContours(const FontInfo *info, int glyph_index, std::vector<Contour> &out, int depth);

// Composite glyph: each component references another glyph by index plus
// an (dx, dy) offset and an optional 2x2 transform; components are
// unioned into the outer glyph's contour list. Point-matching (args
// interpreted as point indices rather than an xy offset -- rare in
// practice) is skipped rather than crashing, since no font mep ships is
// known to use it, matching stb_truetype's own "best effort" contract.
void ReadCompositeGlyph(const FontInfo *info, const unsigned char *g, std::vector<Contour> &out, int depth) {
    if (depth > 8) return;  // guard against a malformed self-referential chain
    const unsigned char *p = g + 10;
    for (;;) {
        uint16_t flags = U16(p);
        uint16_t glyph_index = U16(p + 2);
        p += 4;
        float dx = 0, dy = 0;
        bool args_are_xy = (flags & 0x0002) != 0;  // ARGS_ARE_XY_VALUES
        if (flags & 0x0001) {                      // ARG_1_AND_2_ARE_WORDS
            if (args_are_xy) {
                dx = static_cast<float>(S16(p));
                dy = static_cast<float>(S16(p + 2));
            }
            p += 4;
        } else {
            if (args_are_xy) {
                dx = static_cast<float>(S8(p));
                dy = static_cast<float>(S8(p + 1));
            }
            p += 2;
        }
        float a = 1, b = 0, c = 0, d = 1;
        if (flags & 0x0008) {  // WE_HAVE_A_SCALE
            a = d = F2Dot14(p);
            p += 2;
        } else if (flags & 0x0040) {  // WE_HAVE_AN_X_AND_Y_SCALE
            a = F2Dot14(p);
            d = F2Dot14(p + 2);
            p += 4;
        } else if (flags & 0x0080) {  // WE_HAVE_A_TWO_BY_TWO
            a = F2Dot14(p);
            b = F2Dot14(p + 2);
            c = F2Dot14(p + 4);
            d = F2Dot14(p + 6);
            p += 8;
        }
        if (args_are_xy) {
            std::vector<Contour> sub;
            ReadGlyphContours(info, glyph_index, sub, depth + 1);
            for (Contour &contour : sub) {
                for (GlyphPoint &pt : contour) {
                    float nx = a * pt.x + c * pt.y + dx;
                    float ny = b * pt.x + d * pt.y + dy;
                    pt.x = nx;
                    pt.y = ny;
                }
                out.push_back(std::move(contour));
            }
        }
        if (!(flags & 0x0020)) break;  // MORE_COMPONENTS
    }
}

void ReadGlyphContours(const FontInfo *info, int glyph_index, std::vector<Contour> &out, int depth) {
    uint32_t len = 0;
    uint32_t off = GlyphOffset(info, glyph_index, &len);
    if (len == 0) return;  // empty glyph (e.g. space) -- no contours
    const unsigned char *g = info->data + off;
    int16_t num_contours = S16(g);
    if (num_contours >= 0) {
        ReadSimpleGlyph(g, num_contours, out);
    } else {
        ReadCompositeGlyph(info, g, out, depth);
    }
}

// -- Scanline rasterization ----------------------------------------------
// The actual polygon-fill rasterizer (nonzero-winding, exact horizontal
// coverage + 4x vertical supersample -- see STB_TRUETYPE_REMOVAL_PLAN.md's
// Phase 4 for why this hybrid approach was chosen over full 2D analytic
// coverage) now lives in gfx/rasterizer.h/.cpp, shared with PDF content-
// stream path filling (PDFIUM_REMOVAL_PLAN.md Phase 6) -- this file keeps
// only what's genuinely TrueType-specific: turning a glyf-table contour
// (with its implied-on-curve-midpoint quadratic convention) into the
// shared module's Edge list.

using gfx::raster::AddLine;
using gfx::raster::Edge;
using gfx::raster::FlattenQuadratic;

// Builds the edge list for every contour, applying the "expand implied
// on-curve midpoints, then rotate to start on an on-curve point" TrueType
// contour convention (see STB_TRUETYPE_REMOVAL_PLAN.md's Phase 3 note on
// this being the one genuinely fiddly spec detail here) before walking
// the result as alternating line/quadratic segments.
void BuildEdgesForContour(std::vector<Edge> &edges, const Contour &raw) {
    int n = static_cast<int>(raw.size());
    if (n < 2) return;
    std::vector<GlyphPoint> pts;
    pts.reserve(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; i++) {
        pts.push_back(raw[static_cast<size_t>(i)]);
        const GlyphPoint &a = raw[static_cast<size_t>(i)];
        const GlyphPoint &b = raw[static_cast<size_t>((i + 1) % n)];
        if (!a.on_curve && !b.on_curve) {
            GlyphPoint mid;
            mid.x = (a.x + b.x) * 0.5f;
            mid.y = (a.y + b.y) * 0.5f;
            mid.on_curve = true;
            pts.push_back(mid);
        }
    }
    int m = static_cast<int>(pts.size());
    int start = -1;
    for (int i = 0; i < m; i++) {
        if (pts[static_cast<size_t>(i)].on_curve) {
            start = i;
            break;
        }
    }
    if (start < 0) {
        // Degenerate: every point off-curve (a contour made entirely of
        // implied midpoints never occurs after the expansion above
        // unless the original contour itself was empty of on-curve
        // points at every vertex -- synthesize a start the same way.
        GlyphPoint mid;
        mid.x = (pts.front().x + pts.back().x) * 0.5f;
        mid.y = (pts.front().y + pts.back().y) * 0.5f;
        mid.on_curve = true;
        pts.insert(pts.begin(), mid);
        m++;
        start = 0;
    }
    std::rotate(pts.begin(), pts.begin() + start, pts.end());

    GlyphPoint current = pts[0];
    const GlyphPoint contour_start = current;
    int i = 1;
    while (i <= m) {
        const GlyphPoint &p = pts[static_cast<size_t>(i % m)];
        if (p.on_curve) {
            AddLine(edges, current.x, current.y, p.x, p.y);
            current = p;
        } else {
            const GlyphPoint &next = pts[static_cast<size_t>((i + 1) % m)];  // guaranteed on-curve by the expansion above
            FlattenQuadratic(edges, current.x, current.y, p.x, p.y, next.x, next.y);
            current = next;
            i++;
        }
        i++;
    }
    // Close the contour back to its own start (a no-op AddLine if the
    // walk above already landed exactly on it).
    AddLine(edges, current.x, current.y, contour_start.x, contour_start.y);
}

}  // namespace

bool InitFont(FontInfo *info, const unsigned char *data, int data_size, int offset) {
    if (!data || data_size < 12 || offset < 0 || offset + 12 > data_size) return false;
    const unsigned char *base = data + offset;
    int remaining = data_size - offset;
    uint32_t sfnt_tag = U32(base);
    // 0x00010000 = TrueType, 'true'/'typ1' = older Apple variants,
    // 'OTTO' would mean CFF-flavored (deliberately not supported --
    // every embedded font is glyf-flavored, see this file's own top
    // comment).
    if (sfnt_tag != 0x00010000 && sfnt_tag != 0x74727565) return false;

    RawTable head = FindTable(base, remaining, "head");
    RawTable hhea = FindTable(base, remaining, "hhea");
    RawTable maxp = FindTable(base, remaining, "maxp");
    RawTable hmtx = FindTable(base, remaining, "hmtx");
    RawTable cmap = FindTable(base, remaining, "cmap");
    RawTable loca = FindTable(base, remaining, "loca");
    RawTable glyf = FindTable(base, remaining, "glyf");
    // `cmap` is deliberately NOT required here (PDFIUM_REMOVAL_PLAN.md
    // Phase 9 finding): PDF-embedded, subsetted TrueType fonts routinely
    // omit it entirely -- PDF's own font-dict encoding (/Differences, a
    // /CIDToGIDMap) addresses glyphs directly and has no use for a
    // Unicode cmap, confirmed via a real font extracted from
    // `test/pdf_fixtures/libreoffice_test.pdf` during this phase's own
    // verification. A cmap-less font simply can't answer
    // GetCodepointBitmap (cmap_format stays 0, FindGlyphIndex already
    // returns 0 for that) -- GetGlyphBitmap's GID-direct path, this
    // phase's whole reason for existing, needs no cmap at all.
    if (head.len == 0 || hhea.len == 0 || maxp.len == 0 || hmtx.len == 0 || loca.len == 0 || glyf.len == 0) {
        return false;
    }

    info->data = base;
    info->data_size = remaining;
    info->head_off = head.off;
    info->hhea_off = hhea.off;
    info->maxp_off = maxp.off;
    info->hmtx_off = hmtx.off;
    info->hmtx_len = hmtx.len;
    info->loca_off = loca.off;
    info->loca_len = loca.len;
    info->glyf_off = glyf.off;
    info->glyf_len = glyf.len;

    info->units_per_em = U16(base + head.off + 18);
    info->loca_long = U16(base + head.off + 50) != 0;
    info->num_h_metrics = U16(base + hhea.off + 34);
    info->num_glyphs = U16(base + maxp.off + 4);

    // cmap subtable selection: prefer format 12 (full Unicode, needed for
    // supplementary-plane PUA icon codepoints), fall back to format 4
    // (BMP-only) -- see this file's own top comment. Mep's own UI fonts
    // always have one of these (confirmed by STB_TRUETYPE_REMOVAL_PLAN.md's
    // Phase 1 cmap inventory); PDF-embedded fonts routinely have neither
    // (or no cmap at all, see this function's own comment above) --
    // cmap_format simply stays 0 in that case, tolerated throughout
    // (FindGlyphIndex/GetCodepointBitmap), not a load failure.
    uint16_t num_subtables = cmap.len != 0 ? U16(base + cmap.off + 2) : 0;
    const unsigned char *cmap_base = base + cmap.off;
    uint32_t best_off = 0;
    int best_format = 0;
    for (int i = 0; i < num_subtables; i++) {
        const unsigned char *rec = cmap_base + 4 + i * 8;
        uint32_t sub_off = U32(rec + 4);
        int format = U16(cmap_base + sub_off);
        if (format == 12 && best_format != 12) {
            best_off = cmap.off + sub_off;
            best_format = 12;
        } else if (format == 4 && best_format == 0) {
            best_off = cmap.off + sub_off;
            best_format = 4;
        }
    }
    info->cmap_subtable_off = best_off;
    info->cmap_format = best_format;
    return true;
}

float ScaleForPixelHeight(const FontInfo *info, float pixel_height) {
    int ascent = 0, descent = 0, line_gap = 0;
    GetFontVMetrics(info, &ascent, &descent, &line_gap);
    int span = ascent - descent;
    if (span <= 0) return 0.0f;
    return pixel_height / static_cast<float>(span);
}

void GetFontVMetrics(const FontInfo *info, int *ascent, int *descent, int *line_gap) {
    const unsigned char *h = info->data + info->hhea_off;
    *ascent = S16(h + 4);
    *descent = S16(h + 6);
    *line_gap = S16(h + 8);
}

void GetGlyphHMetrics(const FontInfo *info, int glyph_index, int *advance_width, int *left_side_bearing) {
    if (glyph_index < 0 || glyph_index >= info->num_glyphs) {
        *advance_width = 0;
        *left_side_bearing = 0;
        return;
    }
    const unsigned char *hmtx = info->data + info->hmtx_off;
    if (glyph_index < info->num_h_metrics) {
        *advance_width = U16(hmtx + glyph_index * 4);
        *left_side_bearing = S16(hmtx + glyph_index * 4 + 2);
    } else {
        // Glyphs beyond numberOfHMetrics repeat the last entry's advance
        // (standard hmtx compaction); their own left-side-bearing follows
        // in a trailing array of lsb-only int16 entries.
        *advance_width = U16(hmtx + (info->num_h_metrics - 1) * 4);
        int extra_index = glyph_index - info->num_h_metrics;
        *left_side_bearing = S16(hmtx + info->num_h_metrics * 4 + extra_index * 2);
    }
}

void GetCodepointHMetrics(const FontInfo *info, int codepoint, int *advance_width, int *left_side_bearing) {
    GetGlyphHMetrics(info, FindGlyphIndex(info, codepoint), advance_width, left_side_bearing);
}

unsigned char *GetCodepointBitmap(const FontInfo *info, float scale_x, float scale_y, int codepoint, int *width,
                                   int *height, int *xoff, int *yoff) {
    int glyph_index = FindGlyphIndex(info, codepoint);
    if (glyph_index <= 0) {
        *width = *height = *xoff = *yoff = 0;
        return nullptr;
    }
    return GetGlyphBitmap(info, scale_x, scale_y, glyph_index, width, height, xoff, yoff);
}

unsigned char *GetGlyphBitmap(const FontInfo *info, float scale_x, float scale_y, int glyph_index, int *width,
                               int *height, int *xoff, int *yoff) {
    *width = *height = *xoff = *yoff = 0;
    if (glyph_index < 0 || glyph_index >= info->num_glyphs) return nullptr;

    std::vector<Contour> contours;
    ReadGlyphContours(info, glyph_index, contours, 0);
    if (contours.empty()) return nullptr;

    float xmin = 1e30f, ymin = 1e30f, xmax = -1e30f, ymax = -1e30f;
    for (const Contour &c : contours) {
        for (const GlyphPoint &p : c) {
            xmin = std::min(xmin, p.x);
            xmax = std::max(xmax, p.x);
            ymin = std::min(ymin, p.y);
            ymax = std::max(ymax, p.y);
        }
    }
    if (xmax <= xmin || ymax <= ymin) return nullptr;

    int ix0 = static_cast<int>(std::floor(xmin * scale_x));
    int ix1 = static_cast<int>(std::ceil(xmax * scale_x));
    int iy0 = static_cast<int>(std::floor(-ymax * scale_y));  // font y-up -> raster y-down
    int iy1 = static_cast<int>(std::ceil(-ymin * scale_y));
    int w = ix1 - ix0;
    int h = iy1 - iy0;
    if (w <= 0 || h <= 0) return nullptr;

    std::vector<Edge> edges;
    for (const Contour &c : contours) {
        Contour raster_space;
        raster_space.reserve(c.size());
        for (const GlyphPoint &p : c) {
            GlyphPoint rp;
            rp.x = p.x * scale_x - static_cast<float>(ix0);
            rp.y = -p.y * scale_y - static_cast<float>(iy0);
            rp.on_curve = p.on_curve;
            raster_space.push_back(rp);
        }
        BuildEdgesForContour(edges, raster_space);
    }

    std::vector<unsigned char> pixels = gfx::raster::Rasterize(edges, w, h);
    auto *out = static_cast<unsigned char *>(std::malloc(pixels.size()));
    std::memcpy(out, pixels.data(), pixels.size());

    *width = w;
    *height = h;
    *xoff = ix0;
    *yoff = iy0;
    return out;
}

void FreeBitmap(unsigned char *bitmap) { std::free(bitmap); }

}  // namespace tt
}  // namespace gfx
