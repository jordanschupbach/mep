#include "fem_movie.h"

#include "jpeg_codec.h"
#include "mov_container.h"
#include "png_codec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace fem {
namespace {

using cad::Vec3d;

size_t Idx(int i) { return static_cast<size_t>(i); }

// --- The legend's typeface -------------------------------------------------
//
// Five by seven, capitals only, drawn as pictures rather than stored as a
// table of hex. A hex table is unreviewable: nobody can see that
// 0x7E,0x11,0x11,0x11,0x7E is an 'A', so nobody can see when it is not,
// and a typo in one becomes a permanently misspelt axis label that
// everybody stops noticing. The cost is a few dozen lines of source and
// a one-off decode at first use.
//
// Capitals only because the whole character set has to be drawn by hand
// and lowercase would double that for a colour bar reading "MAX 1.67E+07"
// -- which wants capitals anyway. Lowercase input is folded up.
const char *const kGlyphArt[] = {
    /* ' ' */ ".....:.....:.....:.....:.....:.....:.....",
    /* '!' */ "..X..:..X..:..X..:..X..:..X..:.....:..X..",
    /* '"' */ ".X.X.:.X.X.:.....:.....:.....:.....:.....",
    /* '#' */ ".X.X.:.X.X.:XXXXX:.X.X.:XXXXX:.X.X.:.X.X.",
    /* '$' */ "..X..:.XXXX:X.X..:.XXX.:..X.X:XXXX.:..X..",
    /* '%' */ "XX..X:XX..X:...X.:..X..:.X...:X..XX:X..XX",
    /* '&' */ ".XX..:X..X.:X.X..:.X...:X.X.X:X..X.:.XX.X",
    /* '\''*/ "..X..:..X..:.....:.....:.....:.....:.....",
    /* '(' */ "...X.:..X..:.X...:.X...:.X...:..X..:...X.",
    /* ')' */ ".X...:..X..:...X.:...X.:...X.:..X..:.X...",
    /* '*' */ ".....:X.X.X:.XXX.:XXXXX:.XXX.:X.X.X:.....",
    /* '+' */ ".....:..X..:..X..:XXXXX:..X..:..X..:.....",
    /* ',' */ ".....:.....:.....:.....:..X..:..X..:.X...",
    /* '-' */ ".....:.....:.....:XXXXX:.....:.....:.....",
    /* '.' */ ".....:.....:.....:.....:.....:.XX..:.XX..",
    /* '/' */ "....X:...X.:..X..:..X..:.X...:X....:X....",
    /* '0' */ ".XXX.:X...X:X..XX:X.X.X:XX..X:X...X:.XXX.",
    /* '1' */ "..X..:.XX..:..X..:..X..:..X..:..X..:.XXX.",
    /* '2' */ ".XXX.:X...X:....X:...X.:..X..:.X...:XXXXX",
    /* '3' */ "XXXXX:...X.:..X..:...X.:....X:X...X:.XXX.",
    /* '4' */ "...X.:..XX.:.X.X.:X..X.:XXXXX:...X.:...X.",
    /* '5' */ "XXXXX:X....:XXXX.:....X:....X:X...X:.XXX.",
    /* '6' */ "..XX.:.X...:X....:XXXX.:X...X:X...X:.XXX.",
    /* '7' */ "XXXXX:....X:...X.:..X..:.X...:.X...:.X...",
    /* '8' */ ".XXX.:X...X:X...X:.XXX.:X...X:X...X:.XXX.",
    /* '9' */ ".XXX.:X...X:X...X:.XXXX:....X:...X.:.XX..",
    /* ':' */ ".....:.XX..:.XX..:.....:.XX..:.XX..:.....",
    /* ';' */ ".....:.XX..:.XX..:.....:.XX..:..X..:.X...",
    /* '<' */ "...X.:..X..:.X...:X....:.X...:..X..:...X.",
    /* '=' */ ".....:.....:XXXXX:.....:XXXXX:.....:.....",
    /* '>' */ ".X...:..X..:...X.:....X:...X.:..X..:.X...",
    /* '?' */ ".XXX.:X...X:....X:...X.:..X..:.....:..X..",
    /* '@' */ ".XXX.:X...X:X.XXX:X.X.X:X.XXX:X....:.XXX.",
    /* 'A' */ "..X..:.X.X.:X...X:X...X:XXXXX:X...X:X...X",
    /* 'B' */ "XXXX.:X...X:X...X:XXXX.:X...X:X...X:XXXX.",
    /* 'C' */ ".XXX.:X...X:X....:X....:X....:X...X:.XXX.",
    /* 'D' */ "XXX..:X..X.:X...X:X...X:X...X:X..X.:XXX..",
    /* 'E' */ "XXXXX:X....:X....:XXXX.:X....:X....:XXXXX",
    /* 'F' */ "XXXXX:X....:X....:XXXX.:X....:X....:X....",
    /* 'G' */ ".XXX.:X...X:X....:X.XXX:X...X:X...X:.XXX.",
    /* 'H' */ "X...X:X...X:X...X:XXXXX:X...X:X...X:X...X",
    /* 'I' */ ".XXX.:..X..:..X..:..X..:..X..:..X..:.XXX.",
    /* 'J' */ "..XXX:...X.:...X.:...X.:...X.:X..X.:.XX..",
    /* 'K' */ "X...X:X..X.:X.X..:XX...:X.X..:X..X.:X...X",
    /* 'L' */ "X....:X....:X....:X....:X....:X....:XXXXX",
    /* 'M' */ "X...X:XX.XX:X.X.X:X.X.X:X...X:X...X:X...X",
    /* 'N' */ "X...X:X...X:XX..X:X.X.X:X..XX:X...X:X...X",
    /* 'O' */ ".XXX.:X...X:X...X:X...X:X...X:X...X:.XXX.",
    /* 'P' */ "XXXX.:X...X:X...X:XXXX.:X....:X....:X....",
    /* 'Q' */ ".XXX.:X...X:X...X:X...X:X.X.X:X..X.:.XX.X",
    /* 'R' */ "XXXX.:X...X:X...X:XXXX.:X.X..:X..X.:X...X",
    /* 'S' */ ".XXXX:X....:X....:.XXX.:....X:....X:XXXX.",
    /* 'T' */ "XXXXX:..X..:..X..:..X..:..X..:..X..:..X..",
    /* 'U' */ "X...X:X...X:X...X:X...X:X...X:X...X:.XXX.",
    /* 'V' */ "X...X:X...X:X...X:X...X:X...X:.X.X.:..X..",
    /* 'W' */ "X...X:X...X:X...X:X.X.X:X.X.X:XX.XX:X...X",
    /* 'X' */ "X...X:X...X:.X.X.:..X..:.X.X.:X...X:X...X",
    /* 'Y' */ "X...X:X...X:.X.X.:..X..:..X..:..X..:..X..",
    /* 'Z' */ "XXXXX:....X:...X.:..X..:.X...:X....:XXXXX",
    /* '[' */ ".XXX.:.X...:.X...:.X...:.X...:.X...:.XXX.",
    /* '\\'*/ "X....:X....:.X...:..X..:..X..:...X.:....X",
    /* ']' */ ".XXX.:...X.:...X.:...X.:...X.:...X.:.XXX.",
    /* '^' */ "..X..:.X.X.:X...X:.....:.....:.....:.....",
    /* '_' */ ".....:.....:.....:.....:.....:.....:XXXXX",
};
constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;
constexpr int kFirstGlyph = 32;
constexpr int kGlyphCount = static_cast<int>(sizeof(kGlyphArt) / sizeof(kGlyphArt[0]));

bool GlyphPixel(char c, int col, int row) {
    int code = static_cast<unsigned char>(c);
    if (code >= 'a' && code <= 'z') code -= 32;
    const int index = code - kFirstGlyph;
    if (index < 0 || index >= kGlyphCount) return false;
    // Six characters per row in the art: five pixels and the separator.
    const char *art = kGlyphArt[Idx(index)];
    return art[Idx(row * (kGlyphW + 1) + col)] == 'X';
}

// --- The image -------------------------------------------------------------

struct Image {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
    std::vector<double> depth;  // 1/distance; larger is nearer

    void Reset(int w, int h, const std::array<unsigned char, 3> &background) {
        width = w;
        height = h;
        rgba.assign(Idx(w * h * 4), 255);
        depth.assign(Idx(w * h), 0.0);
        for (int i = 0; i < w * h; ++i) {
            rgba[Idx(i * 4 + 0)] = background[0];
            rgba[Idx(i * 4 + 1)] = background[1];
            rgba[Idx(i * 4 + 2)] = background[2];
            rgba[Idx(i * 4 + 3)] = 255;
        }
    }
};

unsigned char ToByte(double v) {
    const double clamped = std::clamp(v, 0.0, 1.0);
    return static_cast<unsigned char>(std::lround(clamped * 255.0));
}

void PlotBlend(Image *image, int x, int y, double r, double g, double b, double alpha) {
    if (x < 0 || y < 0 || x >= image->width || y >= image->height) return;
    const size_t at = Idx((y * image->width + x) * 4);
    for (int c = 0; c < 3; ++c) {
        const double had = static_cast<double>(image->rgba[at + Idx(c)]) / 255.0;
        const double want = c == 0 ? r : (c == 1 ? g : b);
        image->rgba[at + Idx(c)] = ToByte(had * (1.0 - alpha) + want * alpha);
    }
}

void DrawText(Image *image, int x, int y, int scale, double r, double g, double b,
              const std::string &text) {
    int pen = x;
    for (const char c : text) {
        for (int row = 0; row < kGlyphH; ++row) {
            for (int col = 0; col < kGlyphW; ++col) {
                if (!GlyphPixel(c, col, row)) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        PlotBlend(image, pen + col * scale + dx, y + row * scale + dy, r, g, b, 1.0);
                    }
                }
            }
        }
        pen += (kGlyphW + 1) * scale;
    }
}

int TextWidth(const std::string &text, int scale) {
    return static_cast<int>(text.size()) * (kGlyphW + 1) * scale;
}

// A number a legend can carry: three significant figures, and an exponent
// only when one is needed. A stress field's range runs from 0 to 1.7e7
// and a displacement's from 0 to 3e-6, so neither a fixed number of
// decimals nor a plain %g is readable across both.
std::string Label(double value) {
    if (!std::isfinite(value)) return "--";
    const double magnitude = std::fabs(value);
    char buffer[64];
    if (magnitude != 0.0 && (magnitude >= 1e5 || magnitude < 1e-3)) {
        std::snprintf(buffer, sizeof(buffer), "%.3e", value);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.4g", value);
    }
    return buffer;
}

// --- Geometry --------------------------------------------------------------

struct Basis {
    Vec3d eye;
    Vec3d right;
    Vec3d up;
    Vec3d forward;  // from the eye toward the model
    double near_plane = 1e-3;
    double x_scale = 1.0;
    double y_scale = 1.0;
};

Vec3d Cross(const Vec3d &a, const Vec3d &b) {
    return Vec3d{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double Dot(const Vec3d &a, const Vec3d &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr double kTwoPi = 6.283185307179586;

Vec3d Normalised(const Vec3d &v) {
    const double length = std::sqrt(Dot(v, v));
    if (length <= 0.0) return Vec3d{0, 0, 1};
    return Vec3d{v.x / length, v.y / length, v.z / length};
}

Basis BuildBasis(const MovieView &view, const MovieOptions &options, double turn, int width,
                 int height) {
    Basis basis;
    const double pitch = options.camera.pitch;
    auto direction = [&](double at_yaw) {
        return Vec3d{std::cos(pitch) * std::cos(at_yaw), std::cos(pitch) * std::sin(at_yaw),
                     std::sin(pitch)};
    };
    const double yaw = options.camera.yaw + turn;
    const Vec3d offset = direction(yaw);
    const double half_y = options.camera.fov_y * 0.5;
    const double aspect = static_cast<double>(width) / static_cast<double>(height);
    const double half_x = std::atan(std::tan(half_y) * aspect);
    const double half = std::min(half_x, half_y);
    double distance = options.camera.distance;
    const bool fitting = distance <= 0.0;
    if (fitting) distance = view.radius / std::max(1e-9, std::sin(half)) * 1.15;

    Vec3d up = options.camera.up;
    basis.y_scale = 1.0 / std::tan(half_y);
    basis.x_scale = basis.y_scale * static_cast<double>(height) / static_cast<double>(width);
    auto place = [&](double at) {
        basis.eye = Vec3d{view.centre.x + offset.x * at, view.centre.y + offset.y * at,
                          view.centre.z + offset.z * at};
        basis.forward = Normalised(Vec3d{view.centre.x - basis.eye.x, view.centre.y - basis.eye.y,
                                         view.centre.z - basis.eye.z});
        Vec3d chosen = up;
        if (std::fabs(Dot(Normalised(chosen), basis.forward)) > 0.999) chosen = Vec3d{0, 1, 0};
        basis.right = Normalised(Cross(basis.forward, chosen));
        basis.up = Normalised(Cross(basis.right, basis.forward));
    };
    if (fitting) {
        // The bounding sphere is a starting guess; the box is the shape.
        // Project its eight corners, see how much of the frame they
        // actually use, and move in until they use most of it. Perspective
        // makes that non-linear, so it is iterated rather than solved --
        // three passes settle it to well under a pixel.
        //
        // AND THE SAME DISTANCE FOR EVERY FRAME OF AN ORBIT. A fit done
        // per frame zooms in as the part turns edge on and out again as
        // it comes broadside -- for a ten-to-one beam that is a factor of
        // several, and the film is then a part breathing rather than
        // turning. When the camera is going to orbit, the fit is the
        // worst case over the whole circle, evaluated once.
        const int samples = options.camera.orbit ? 24 : 1;
        double fitted = 0.0;
        for (int sample = 0; sample < samples; ++sample) {
            const double at_yaw =
                options.camera.yaw + kTwoPi * static_cast<double>(sample) / samples;
            const Vec3d where = samples == 1 ? offset : direction(at_yaw);
            double trial = distance;
            for (int pass = 0; pass < 3; ++pass) {
                const Vec3d eye{view.centre.x + where.x * trial, view.centre.y + where.y * trial,
                                view.centre.z + where.z * trial};
                const Vec3d forward = Normalised(Vec3d{view.centre.x - eye.x, view.centre.y - eye.y,
                                                       view.centre.z - eye.z});
                Vec3d chosen = up;
                if (std::fabs(Dot(Normalised(chosen), forward)) > 0.999) chosen = Vec3d{0, 1, 0};
                const Vec3d right = Normalised(Cross(forward, chosen));
                const Vec3d over = Normalised(Cross(right, forward));
                double worst = 0.0;
                for (int corner = 0; corner < 8; ++corner) {
                    const Vec3d p{(corner & 1) != 0 ? view.high.x : view.low.x,
                                  (corner & 2) != 0 ? view.high.y : view.low.y,
                                  (corner & 4) != 0 ? view.high.z : view.low.z};
                    const Vec3d rel{p.x - eye.x, p.y - eye.y, p.z - eye.z};
                    const double depth = std::max(1e-9, Dot(rel, forward));
                    worst = std::max(worst, std::fabs(Dot(rel, right) * basis.x_scale / depth));
                    worst = std::max(worst, std::fabs(Dot(rel, over) * basis.y_scale / depth));
                }
                if (!(worst > 0.0)) break;
                // 0.84 rather than 1.0: room for the deformed shape,
                // which is bigger than the box this was fitted to, and
                // for the legend.
                trial *= worst / 0.84;
            }
            fitted = std::max(fitted, trial);
        }
        distance = fitted;
    }
    place(distance);
    basis.near_plane = std::max(1e-9, view.radius * 1e-3);
    return basis;
}

// A vertex on its way to the screen: the camera-space position, the lit
// colour, and (once projected) where it lands.
struct Vertex {
    double cx = 0.0, cy = 0.0, cz = 0.0;  // camera space, cz forward and positive
    double r = 0.0, g = 0.0, b = 0.0;
    double sx = 0.0, sy = 0.0, inv = 0.0;
};

Vertex Lerp(const Vertex &a, const Vertex &b, double t) {
    Vertex v;
    v.cx = a.cx + (b.cx - a.cx) * t;
    v.cy = a.cy + (b.cy - a.cy) * t;
    v.cz = a.cz + (b.cz - a.cz) * t;
    v.r = a.r + (b.r - a.r) * t;
    v.g = a.g + (b.g - a.g) * t;
    v.b = a.b + (b.b - a.b) * t;
    return v;
}

void Project(Vertex *v, const Basis &basis, int width, int height) {
    v->inv = 1.0 / std::max(basis.near_plane, v->cz);
    const double ndc_x = v->cx * basis.x_scale * v->inv;
    const double ndc_y = v->cy * basis.y_scale * v->inv;
    v->sx = (0.5 + 0.5 * ndc_x) * static_cast<double>(width);
    v->sy = (0.5 - 0.5 * ndc_y) * static_cast<double>(height);
}

double EdgeFunction(double ax, double ay, double bx, double by, double px, double py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

void FillTriangle(Image *image, const Vertex &a, const Vertex &b, const Vertex &c) {
    const double area = EdgeFunction(a.sx, a.sy, b.sx, b.sy, c.sx, c.sy);
    if (std::fabs(area) < 1e-12) return;
    const int min_x = std::max(0, static_cast<int>(std::floor(std::min({a.sx, b.sx, c.sx}))));
    const int max_x =
        std::min(image->width - 1, static_cast<int>(std::ceil(std::max({a.sx, b.sx, c.sx}))));
    const int min_y = std::max(0, static_cast<int>(std::floor(std::min({a.sy, b.sy, c.sy}))));
    const int max_y =
        std::min(image->height - 1, static_cast<int>(std::ceil(std::max({a.sy, b.sy, c.sy}))));
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const double px = static_cast<double>(x) + 0.5;
            const double py = static_cast<double>(y) + 0.5;
            double wa = EdgeFunction(b.sx, b.sy, c.sx, c.sy, px, py) / area;
            double wb = EdgeFunction(c.sx, c.sy, a.sx, a.sy, px, py) / area;
            double wc = 1.0 - wa - wb;
            if (wa < 0.0 || wb < 0.0 || wc < 0.0) continue;
            // PERSPECTIVE-CORRECT, which matters here rather than being
            // pedantry: a benchmark part is routinely a long thin strip
            // seen at a shallow angle, and screen-linear interpolation
            // puts its colour bands visibly in the wrong place.
            const double inv = wa * a.inv + wb * b.inv + wc * c.inv;
            if (inv <= 0.0) continue;
            const size_t at = Idx(y * image->width + x);
            if (inv <= image->depth[at]) continue;
            image->depth[at] = inv;
            const double r = (wa * a.r * a.inv + wb * b.r * b.inv + wc * c.r * c.inv) / inv;
            const double g = (wa * a.g * a.inv + wb * b.g * b.inv + wc * c.g * c.inv) / inv;
            const double bl = (wa * a.b * a.inv + wb * b.b * b.inv + wc * c.b * c.inv) / inv;
            image->rgba[at * 4 + 0] = ToByte(r);
            image->rgba[at * 4 + 1] = ToByte(g);
            image->rgba[at * 4 + 2] = ToByte(bl);
        }
    }
}

// Clips a triangle to the near plane and draws whatever survives. Between
// zero and two triangles come out, which is why this is a routine rather
// than a test at the top of FillTriangle.
void DrawClipped(Image *image, const Basis &basis, Vertex a, Vertex b, Vertex c) {
    Vertex in[3];
    Vertex out[3];
    int in_count = 0;
    int out_count = 0;
    const Vertex source[3] = {a, b, c};
    for (const Vertex &v : source) {
        if (v.cz >= basis.near_plane) {
            in[in_count++] = v;
        } else {
            out[out_count++] = v;
        }
    }
    auto at_plane = [&](const Vertex &near_side, const Vertex &far_side) {
        const double t = (basis.near_plane - far_side.cz) / (near_side.cz - far_side.cz);
        return Lerp(far_side, near_side, t);
    };
    auto emit = [&](Vertex p, Vertex q, Vertex r) {
        Project(&p, basis, image->width, image->height);
        Project(&q, basis, image->width, image->height);
        Project(&r, basis, image->width, image->height);
        FillTriangle(image, p, q, r);
    };
    if (in_count == 3) {
        emit(a, b, c);
    } else if (in_count == 2) {
        const Vertex p = at_plane(in[0], out[0]);
        const Vertex q = at_plane(in[1], out[0]);
        emit(in[0], in[1], q);
        emit(in[0], q, p);
    } else if (in_count == 1) {
        emit(in[0], at_plane(in[0], out[0]), at_plane(in[0], out[1]));
    }
}

void DrawLine(Image *image, const Basis &basis, Vertex a, Vertex b, double r, double g, double bl,
              double alpha) {
    if (a.cz < basis.near_plane && b.cz < basis.near_plane) return;
    if (a.cz < basis.near_plane) {
        a = Lerp(a, b, (basis.near_plane - a.cz) / (b.cz - a.cz));
    } else if (b.cz < basis.near_plane) {
        b = Lerp(b, a, (basis.near_plane - b.cz) / (a.cz - b.cz));
    }
    Project(&a, basis, image->width, image->height);
    Project(&b, basis, image->width, image->height);
    const double dx = b.sx - a.sx;
    const double dy = b.sy - a.sy;
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy)))));
    if (steps > 8 * (image->width + image->height)) return;
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(steps);
        const int x = static_cast<int>(std::lround(a.sx + dx * t));
        const int y = static_cast<int>(std::lround(a.sy + dy * t));
        if (x < 0 || y < 0 || x >= image->width || y >= image->height) continue;
        const double inv = a.inv + (b.inv - a.inv) * t;
        const size_t at = Idx(y * image->width + x);
        // A LINE TRACING A SURFACE IS EXACTLY AS FAR AWAY AS THE SURFACE,
        // so an honest depth test makes it flicker in and out along its
        // own length. The bias lets it win the tie without letting it
        // show through the solid in front of it.
        if (inv * 1.002 <= image->depth[at]) continue;
        PlotBlend(image, x, y, r, g, bl, alpha);
    }
}

std::vector<unsigned char> Downsample(const std::vector<unsigned char> &pixels, int width,
                                      int height, int factor) {
    if (factor <= 1) return pixels;
    const int out_w = width / factor;
    const int out_h = height / factor;
    std::vector<unsigned char> out(Idx(out_w * out_h * 4), 255);
    const double count = static_cast<double>(factor * factor);
    for (int y = 0; y < out_h; ++y) {
        for (int x = 0; x < out_w; ++x) {
            double sum[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < factor; ++dy) {
                for (int dx = 0; dx < factor; ++dx) {
                    const size_t at = Idx(((y * factor + dy) * width + x * factor + dx) * 4);
                    for (int c = 0; c < 4; ++c) sum[c] += static_cast<double>(pixels[at + Idx(c)]);
                }
            }
            const size_t to = Idx((y * out_w + x) * 4);
            for (int c = 0; c < 4; ++c) {
                out[to + Idx(c)] = static_cast<unsigned char>(std::lround(sum[c] / count));
            }
        }
    }
    return out;
}

void DrawLegend(Image *image, const MovieOptions &options, double low, double high) {
    const int scale = std::max(1, image->height / 260);
    const int margin = 8 * scale;
    const int bar_w = 10 * scale;
    const int bar_h = image->height / 3;
    const int bar_x = image->width - margin - bar_w;
    const int bar_y = (image->height - bar_h) / 2;
    for (int y = 0; y < bar_h; ++y) {
        const double t = 1.0 - static_cast<double>(y) / static_cast<double>(std::max(1, bar_h - 1));
        unsigned char rgba[4];
        MapColor(options.render.color_map, t, rgba);
        for (int x = 0; x < bar_w; ++x) {
            PlotBlend(image, bar_x + x, bar_y + y, static_cast<double>(rgba[0]) / 255.0,
                      static_cast<double>(rgba[1]) / 255.0, static_cast<double>(rgba[2]) / 255.0,
                      1.0);
        }
    }
    const std::string top = Label(high);
    const std::string bottom = Label(low);
    DrawText(image, bar_x - margin / 2 - TextWidth(top, scale), bar_y, scale, 0.92, 0.92, 0.92, top);
    DrawText(image, bar_x - margin / 2 - TextWidth(bottom, scale), bar_y + bar_h - kGlyphH * scale,
             scale, 0.92, 0.92, 0.92, bottom);
    if (!options.units.empty()) {
        DrawText(image, bar_x - margin / 2 - TextWidth(options.units, scale),
                 bar_y + bar_h + 3 * scale, scale, 0.72, 0.74, 0.78, options.units);
    }
    if (!options.caption.empty()) {
        DrawText(image, margin, margin, scale, 0.94, 0.94, 0.94, options.caption);
    }
}

// A field's own range over every frame, which is what the colour bar has
// to be fixed to.
void RangeOver(const std::vector<std::vector<double>> &values, double *low, double *high) {
    *low = 0.0;
    *high = 0.0;
    bool any = false;
    for (const std::vector<double> &frame : values) {
        for (const double v : frame) {
            if (!std::isfinite(v)) continue;
            if (!any) {
                *low = v;
                *high = v;
                any = true;
                continue;
            }
            *low = std::min(*low, v);
            *high = std::max(*high, v);
        }
    }
    if (!any || *high <= *low) *high = *low + 1.0;
}

}  // namespace

MovieView FitView(const AnalysisModel &model) {
    MovieView view;
    if (model.nodes.empty()) return view;
    Vec3d low = model.nodes.front();
    Vec3d high = low;
    for (const Vec3d &p : model.nodes) {
        low.x = std::min(low.x, p.x);
        low.y = std::min(low.y, p.y);
        low.z = std::min(low.z, p.z);
        high.x = std::max(high.x, p.x);
        high.y = std::max(high.y, p.y);
        high.z = std::max(high.z, p.z);
    }
    view.low = low;
    view.high = high;
    view.centre = Vec3d{(low.x + high.x) * 0.5, (low.y + high.y) * 0.5, (low.z + high.z) * 0.5};
    const double dx = high.x - low.x;
    const double dy = high.y - low.y;
    const double dz = high.z - low.z;
    view.radius = 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz);
    if (view.radius <= 0.0) view.radius = 1.0;
    return view;
}

namespace {
// Everything below is one routine; the two public entry points are thin
// wrappers over it, so a scene and a single-result film cannot drift
// apart in how they are drawn.
bool RenderMany(const std::vector<const RenderMesh *> &meshes, const RenderMesh *ghost,
                const std::vector<Annotation> &labels, const MovieView &view,
                const MovieOptions &options, double turn, double frame_min, double frame_max,
                std::vector<unsigned char> *out, std::string *error);
}  // namespace

bool RenderScene(const std::vector<const RenderMesh *> &meshes,
                 const std::vector<Annotation> &labels, const MovieView &view,
                 const MovieOptions &options, double turn, double frame_min, double frame_max,
                 std::vector<unsigned char> *out, std::string *error) {
    std::vector<const RenderMesh *> solids;
    solids.reserve(meshes.size());
    for (const RenderMesh *mesh : meshes) {
        if (mesh != nullptr) solids.push_back(mesh);
    }
    return RenderMany(solids, nullptr, labels, view, options, turn, frame_min, frame_max, out,
                      error);
}

bool RenderFrame(const RenderMesh &mesh, const RenderMesh *ghost, const MovieView &view,
                 const MovieOptions &options, double turn, double frame_min, double frame_max,
                 std::vector<unsigned char> *out, std::string *error) {
    return RenderMany({&mesh}, ghost, {}, view, options, turn, frame_min, frame_max, out, error);
}

namespace {

bool RenderMany(const std::vector<const RenderMesh *> &meshes, const RenderMesh *ghost,
                const std::vector<Annotation> &labels, const MovieView &view,
                const MovieOptions &options, double turn, double frame_min, double frame_max,
                std::vector<unsigned char> *out, std::string *error) {
    if (out == nullptr) {
        if (error != nullptr) *error = "RenderFrame needs somewhere to put the pixels";
        return false;
    }
    const int supersample = std::clamp(options.supersample, 1, 4);
    const int width = options.width * supersample;
    const int height = options.height * supersample;
    if (options.width <= 0 || options.height <= 0) {
        if (error != nullptr) *error = "a frame needs a positive width and height";
        return false;
    }
    Image image;
    image.Reset(width, height, options.background);
    const Basis basis = BuildBasis(view, options, turn, width, height);

    auto to_camera = [&](double x, double y, double z) {
        const Vec3d rel{x - basis.eye.x, y - basis.eye.y, z - basis.eye.z};
        Vertex v;
        v.cx = Dot(rel, basis.right);
        v.cy = Dot(rel, basis.up);
        v.cz = Dot(rel, basis.forward);
        return v;
    };

    // TWO-SIDED LIGHTING, deliberately. A cut-away or a section presents
    // faces whose outward normal points away from the eye, and a
    // one-sided term paints every one of them black -- which reads as a
    // hole in the part rather than as the inside of it.
    auto shade = [&](Vertex *v, double nx, double ny, double nz, double r, double g, double b) {
        const Vec3d normal = Normalised(Vec3d{nx, ny, nz});
        const double key = std::fabs(Dot(normal, basis.forward));
        const Vec3d fill = Normalised(Vec3d{basis.right.x + basis.up.x, basis.right.y + basis.up.y,
                                            basis.right.z + basis.up.z});
        const double side = std::fabs(Dot(normal, fill));
        const double light = 0.34 + 0.52 * key + 0.20 * side;
        v->r = r * light;
        v->g = g * light;
        v->b = b * light;
    };

    auto vertex_of = [&](const RenderMesh &m, unsigned int i) {
        const size_t at = Idx(static_cast<int>(i));
        Vertex v = to_camera(static_cast<double>(m.positions[at * 3 + 0]),
                             static_cast<double>(m.positions[at * 3 + 1]),
                             static_cast<double>(m.positions[at * 3 + 2]));
        double nx = 0.0, ny = 0.0, nz = 1.0;
        if (m.normals.size() >= (at + 1) * 3) {
            nx = static_cast<double>(m.normals[at * 3 + 0]);
            ny = static_cast<double>(m.normals[at * 3 + 1]);
            nz = static_cast<double>(m.normals[at * 3 + 2]);
        }
        double r = 0.7, g = 0.7, b = 0.7;
        if (m.colors.size() >= (at + 1) * 4) {
            r = static_cast<double>(m.colors[at * 4 + 0]) / 255.0;
            g = static_cast<double>(m.colors[at * 4 + 1]) / 255.0;
            b = static_cast<double>(m.colors[at * 4 + 2]) / 255.0;
        }
        shade(&v, nx, ny, nz, r, g, b);
        return v;
    };

    // A LINE PER TRIANGLE EDGE, ONCE. Every interior edge of a closed
    // surface belongs to two triangles, so drawing them per triangle
    // draws each one twice -- twice the work, and, because the lines are
    // blended rather than opaque, twice the darkness on the shared ones
    // and not on the silhouette, which reads as a mesh with random heavy
    // lines in it.
    auto unique_edges = [](const RenderMesh &m) {
        std::vector<std::pair<unsigned int, unsigned int>> edges;
        edges.reserve(m.indices.size());
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            for (int e = 0; e < 3; ++e) {
                unsigned int u = m.indices[t + Idx(e)];
                unsigned int v = m.indices[t + Idx((e + 1) % 3)];
                if (u > v) std::swap(u, v);
                edges.emplace_back(u, v);
            }
        }
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        return edges;
    };
    // AND ONLY WHEN THERE IS ROOM FOR THEM. A mesh drawn at fewer than
    // about a dozen pixels per edge is not a mesh any more, it is a grey
    // wash over the contour plot -- and the contour plot is the thing
    // being looked at. The threshold is in pixels rather than elements so
    // that asking for a larger frame gets the mesh back rather than
    // needing a second setting changed to match.
    const int room = image.width * image.height / (supersample * supersample) / 150;

    // The ghost first, so the solid can hide the parts of it that are
    // behind the part rather than the other way round.
    if (ghost != nullptr && options.ghost_undeformed) {
        const auto edges = unique_edges(*ghost);
        if (static_cast<int>(edges.size()) <= room) {
            for (const auto &edge : edges) {
                DrawLine(&image, basis, vertex_of(*ghost, edge.first),
                         vertex_of(*ghost, edge.second), 0.38, 0.40, 0.45, 0.32);
            }
        }
    }

    // The solids of every mesh first, then the lines of every mesh, and
    // not mesh by mesh: a line belonging to the first item has to be
    // hidden by the solid of the second, and drawing each item complete
    // in turn would let the first item's wireframe show through
    // everything drawn after it.
    for (const RenderMesh *m : meshes) {
        for (size_t t = 0; t + 2 < m->indices.size(); t += 3) {
            DrawClipped(&image, basis, vertex_of(*m, m->indices[t + 0]),
                        vertex_of(*m, m->indices[t + 1]), vertex_of(*m, m->indices[t + 2]));
        }
    }
    if (options.mesh_lines) {
        int total = 0;
        std::vector<std::vector<std::pair<unsigned int, unsigned int>>> per_mesh;
        per_mesh.reserve(meshes.size());
        for (const RenderMesh *m : meshes) {
            per_mesh.push_back(unique_edges(*m));
            total += static_cast<int>(per_mesh.back().size());
        }
        // Counted over the whole scene, not per item: ten items of a
        // hundred edges each is a thousand edges on the screen however
        // few any one of them has.
        if (total <= room) {
            for (size_t i = 0; i < meshes.size(); ++i) {
                for (const auto &edge : per_mesh[i]) {
                    DrawLine(&image, basis, vertex_of(*meshes[i], edge.first),
                             vertex_of(*meshes[i], edge.second), 0.08, 0.09, 0.11, 0.5);
                }
            }
        }
    }
    for (const RenderMesh *m : meshes) {
        for (size_t l = 0; l + 1 < m->line_indices.size(); l += 2) {
            const Vertex a = vertex_of(*m, m->line_indices[l + 0]);
            const Vertex b = vertex_of(*m, m->line_indices[l + 1]);
            DrawLine(&image, basis, a, b, a.r, a.g, a.b, 1.0);
        }
    }

    std::vector<unsigned char> pixels =
        Downsample(image.rgba, width, height, supersample);
    // The legend is drawn *after* the downsample, at the output
    // resolution, because supersampled text is blurred text -- and a
    // number that cannot be read is worse than no number.
    Image final_image;
    final_image.width = options.width;
    final_image.height = options.height;
    final_image.rgba = std::move(pixels);
    // A LABEL IS DRAWN AT THE OUTPUT RESOLUTION AND DEPTH-TESTED AGAINST
    // NOTHING. It is an annotation, not geometry: hiding it behind the
    // part it names would defeat the only thing it is for. What it does
    // respect is the camera -- a label behind the eye is not drawn at
    // all, and one outside the frame is clipped by the plotting.
    for (const Annotation &label : labels) {
        const Vec3d rel{label.at.x - basis.eye.x, label.at.y - basis.eye.y,
                        label.at.z - basis.eye.z};
        Vertex v;
        v.cx = Dot(rel, basis.right);
        v.cy = Dot(rel, basis.up);
        v.cz = Dot(rel, basis.forward);
        if (v.cz < basis.near_plane) continue;
        Project(&v, basis, width, height);
        const int scale = std::max(1, options.height / 260);
        const int x = static_cast<int>(std::lround(v.sx / supersample)) + 5 * scale;
        const int y = static_cast<int>(std::lround(v.sy / supersample)) - kGlyphH * scale / 2;
        // A tick at the point itself, so the text is tied to somewhere
        // rather than floating near it.
        for (int d = -2 * scale; d <= 2 * scale; ++d) {
            PlotBlend(&final_image, static_cast<int>(std::lround(v.sx / supersample)) + d,
                      static_cast<int>(std::lround(v.sy / supersample)),
                      static_cast<double>(label.color[0]) / 255.0,
                      static_cast<double>(label.color[1]) / 255.0,
                      static_cast<double>(label.color[2]) / 255.0, 0.9);
        }
        DrawText(&final_image, x, y, scale, static_cast<double>(label.color[0]) / 255.0,
                 static_cast<double>(label.color[1]) / 255.0,
                 static_cast<double>(label.color[2]) / 255.0, label.text);
    }
    if (options.legend) DrawLegend(&final_image, options, frame_min, frame_max);
    *out = std::move(final_image.rgba);
    return true;
}

// Builds the drawable surface for one frame, plus the undeformed ghost.
bool BuildFrameMesh(const AnalysisModel &model, const std::vector<Vec3d> &displacement,
                    const std::vector<double> &values, const MovieOptions &options, double scale,
                    RenderMesh *out, std::string *error) {
    RenderOptions render = options.render;
    // The displacement scale is settled once over the whole film and
    // passed in, for the same reason the camera is.
    render.auto_scale = false;
    render.displacement_scale = scale;
    return RenderSurface(model, displacement, values, render, out, error);
}

}  // namespace

bool WriteMovie(const AnalysisModel &model,
                const std::vector<std::vector<Vec3d>> &displacement,
                const std::vector<std::vector<double>> &values, const MovieOptions &options,
                const std::string &path, MovieReport *out, std::string *error) {
    auto fail = [&](const std::string &message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (displacement.empty()) return fail("a film needs at least one frame");
    if (values.size() != displacement.size() && values.size() != 1) {
        return fail("a film wants one field per frame, or one field for all of them");
    }
    if (options.fps <= 0) return fail("a film needs a positive frame rate");

    // ONE DISPLACEMENT SCALE FOR THE WHOLE FILM. RenderSurface's own
    // auto-scale makes the largest displacement a fixed fraction of the
    // model -- per call. Applied per frame that normalises every frame to
    // the same size, so a mode shape swinging from one extreme to the
    // other through zero would be drawn at full amplitude throughout and
    // the animation would show nothing moving at all. The scale is
    // therefore fixed here, from the largest displacement anywhere in the
    // film.
    double peak = 0.0;
    for (const std::vector<Vec3d> &frame : displacement) {
        for (const Vec3d &u : frame) {
            peak = std::max(peak, std::sqrt(Dot(u, u)));
        }
    }
    const MovieView view = FitView(model);
    double scale = 1.0;
    if (options.render.auto_scale) {
        scale = peak > 0.0 ? options.render.auto_scale_fraction * view.radius * 2.0 / peak : 1.0;
    } else {
        scale = options.render.displacement_scale;
    }

    double low = 0.0;
    double high = 0.0;
    RangeOver(values, &low, &high);
    if (!options.render.auto_range) {
        low = options.render.range_min;
        high = options.render.range_max;
    }
    MovieOptions fixed = options;
    fixed.render.auto_range = false;
    fixed.render.range_min = low;
    fixed.render.range_max = high;

    RenderMesh ghost;
    if (options.ghost_undeformed) {
        RenderOptions rest = fixed.render;
        rest.undeformed = true;
        rest.auto_scale = false;
        rest.displacement_scale = 0.0;
        if (!RenderSurface(model, displacement.front(), values.front(), rest, &ghost, error)) {
            return false;
        }
    }

    std::vector<std::string> jpeg_frames;
    jpeg_frames.reserve(displacement.size());
    for (size_t f = 0; f < displacement.size(); ++f) {
        const std::vector<double> &field = values.size() == 1 ? values.front() : values[f];
        RenderMesh frame;
        if (!BuildFrameMesh(model, displacement[f], field, fixed, scale, &frame, error)) {
            return false;
        }
        const double turn = options.camera.orbit
                                ? 6.283185307179586 * static_cast<double>(f) /
                                      static_cast<double>(displacement.size())
                                : 0.0;
        std::vector<unsigned char> pixels;
        if (!RenderFrame(frame, options.ghost_undeformed ? &ghost : nullptr, view, fixed, turn, low,
                         high, &pixels, error)) {
            return false;
        }
        std::string encoded = jpeg::Encode(options.width, options.height, 4, pixels.data(),
                                           options.width * 4, options.quality);
        if (encoded.empty()) return fail("a frame could not be encoded as JPEG");
        jpeg_frames.push_back(std::move(encoded));
    }

    std::string mov_error;
    if (!mov::WriteMovFile(path, options.width, options.height, options.fps, 1, jpeg_frames,
                           &mov_error)) {
        return fail(mov_error.empty() ? "the film could not be written" : mov_error);
    }
    if (out != nullptr) {
        out->frames = static_cast<int>(jpeg_frames.size());
        out->width = options.width;
        out->height = options.height;
        out->seconds = static_cast<double>(jpeg_frames.size()) / static_cast<double>(options.fps);
        out->field_min = low;
        out->field_max = high;
        out->displacement_scale = scale;
        long long bytes = 0;
        for (const std::string &frame : jpeg_frames) bytes += static_cast<long long>(frame.size());
        out->bytes = bytes;
    }
    return true;
}

bool WriteStill(const AnalysisModel &model, const std::vector<Vec3d> &displacement,
                const std::vector<double> &values, const MovieOptions &options,
                const std::string &path, MovieReport *out, std::string *error) {
    const MovieView view = FitView(model);
    RenderMesh mesh;
    RenderOptions render = options.render;
    if (render.auto_scale) {
        double peak = 0.0;
        for (const Vec3d &u : displacement) peak = std::max(peak, std::sqrt(Dot(u, u)));
        render.auto_scale = false;
        render.displacement_scale =
            peak > 0.0 ? render.auto_scale_fraction * view.radius * 2.0 / peak : 1.0;
    }
    if (!RenderSurface(model, displacement, values, render, &mesh, error)) return false;
    RenderMesh ghost;
    if (options.ghost_undeformed) {
        RenderOptions rest = render;
        rest.undeformed = true;
        rest.displacement_scale = 0.0;
        if (!RenderSurface(model, displacement, values, rest, &ghost, error)) return false;
    }
    std::vector<unsigned char> pixels;
    if (!RenderFrame(mesh, options.ghost_undeformed ? &ghost : nullptr, view, options, 0.0,
                     mesh.field_min, mesh.field_max, &pixels, error)) {
        return false;
    }
    const std::string encoded =
        png::Encode(options.width, options.height, 4, pixels.data(), options.width * 4);
    if (encoded.empty()) {
        if (error != nullptr) *error = "the still could not be encoded as PNG";
        return false;
    }
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        if (error != nullptr) *error = "could not write '" + path + "'";
        return false;
    }
    const size_t written = std::fwrite(encoded.data(), 1, encoded.size(), file);
    std::fclose(file);
    if (written != encoded.size()) {
        if (error != nullptr) *error = "the still was not fully written";
        return false;
    }
    if (out != nullptr) {
        out->frames = 1;
        out->width = options.width;
        out->height = options.height;
        out->field_min = mesh.field_min;
        out->field_max = mesh.field_max;
        out->displacement_scale = mesh.scale_used;
        out->bytes = static_cast<long long>(encoded.size());
    }
    return true;
}

}  // namespace fem
