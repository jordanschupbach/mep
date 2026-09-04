#ifndef MEP_SVG_DOC_H
#define MEP_SVG_DOC_H

#include <string>
#include <vector>

#include "html_doc.h"

// Renderer-independent SVG geometry. BuildSvgDisplayList walks an inline
// <svg> DOM subtree (parsed by ParseHtml, so element/attribute names are the
// usual lowercase strings) and flattens every shape into already-transformed
// polylines, triangulated fills, and positioned text runs in the target
// viewport's own pixel space. main.cpp then replays the list with raylib
// primitives, exactly the way it replays a <canvas> element's command list --
// keeping the SVG grammar (path data, transforms, viewBox, inheritance)
// entirely out of the draw pass so it can be unit tested without a window.
//
// Deliberately not covered yet (WEBKIT_PARITY_PLAN.md Part VII, SVG):
// - even-odd/nonzero fill rules with holes -- each subpath is filled on its
//   own, so a ring drawn as two subpaths fills solid;
// - real gradients/patterns -- a `url(#id)` paint resolves to the average of
//   the referenced gradient's stops, a flat approximation;
// - clip paths, masks, markers, filters, and <style>-sheet selectors that
//   target SVG elements (presentation attributes and style="" are honored).

struct SvgPaint {
    bool present = false;  // false = "none" / not painted
    unsigned char r = 0, g = 0, b = 0, a = 255;
};

struct SvgShape {
    enum class Kind { Polyline, Polygon, Text };
    Kind kind = Kind::Polyline;
    // Flattened outline as x,y pairs in target space. A Polygon's outline is
    // implicitly closed; a Polyline is open unless `closed` is set (a closed
    // path that only strokes still records its geometry as a Polyline).
    std::vector<float> points;
    bool closed = false;
    // Polygon fills, as index triples into `points`. Every triangle is
    // wound counter-clockwise on screen (negative cross product in y-down
    // coordinates), which is the order raylib's DrawTriangle requires.
    std::vector<unsigned> triangles;
    SvgPaint fill, stroke;
    float stroke_width = 1.0f;  // already scaled into target space
    // Text: `points` holds the single anchor position (x, y = baseline).
    std::string text;
    float font_size = 16.0f;
    std::string text_anchor = "start";  // start | middle | end
};

struct SvgDisplayList {
    float width = 0.0f, height = 0.0f;  // the target viewport it was built for
    std::vector<SvgShape> shapes;
};

// Intrinsic size from `width`/`height` attributes, falling back to the
// viewBox's own dimensions. Returns false when neither is usable, in which
// case callers apply the HTML replaced-element default (300x150).
bool SvgIntrinsicSize(const DomNode &svg, float &width, float &height);

// Flattens `svg` (an <svg> element) into a display list sized to
// `target_width` x `target_height` CSS pixels. `current_color` is used for
// `fill="currentColor"` and defaults to black like a real UA's initial value.
SvgDisplayList BuildSvgDisplayList(const DomNode &svg, float target_width, float target_height,
                                   SvgPaint current_color = SvgPaint{true, 0, 0, 0, 255});

// Spec-accurate CSS color parsing (#rgb[a], #rrggbb[aa], rgb()/rgba(),
// named colors, "transparent"); shared with the <canvas> binding. Unlike
// html_doc.cpp's ParseColor, named colors are the real sRGB values rather
// than the softened reading-mode palette used for page text.
bool ParseCssColor(const std::string &text, unsigned char &r, unsigned char &g, unsigned char &b, unsigned char &a);

// Exposed for tests: parses an SVG path data string into flattened
// subpaths (each a list of x,y pairs; `closed` marks a trailing Z).
struct SvgSubpath {
    std::vector<float> points;
    bool closed = false;
};
std::vector<SvgSubpath> ParseSvgPathData(const std::string &data);

// Exposed for tests: triangulates a simple polygon (x,y pairs) by ear
// clipping. Falls back to a fan for degenerate/self-intersecting input so
// something is still drawn. Output winding follows SvgShape::triangles.
std::vector<unsigned> TriangulateSvgPolygon(const std::vector<float> &points);

#endif
