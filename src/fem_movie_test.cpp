// Part J.3's second half: results as a playable film, verified.
//
// HOW YOU TEST A PICTURE, taking fem_render_test.cpp's answer and
// pushing it one stage further. That file could check geometry, because
// geometry has measurable properties. An image has fewer, but it has
// some, and every one of them below is a property that a specific,
// plausible bug destroys:
//
//   * a silhouette that is symmetric when the camera is on a symmetry
//     plane -- the projection is not skewed, and the aspect handling is
//     not transposed;
//   * a nearer solid that hides a farther one exactly -- the depth test
//     works, and works in the right direction;
//   * a colour bar whose ends are the colour map's ends -- the legend is
//     measuring the field rather than decorating the frame;
//   * a frame at the middle of a mode's cycle that matches the
//     undeformed shape, and a frame at its extreme that does not -- the
//     amplitude is NOT renormalised per frame, which is the single
//     easiest way to produce an animation in which nothing appears to
//     move;
//   * and a file that reads back through the demuxer and decodes to
//     something close to the pixels that went in -- the whole chain,
//     end to end.

#include "fem_movie.h"

#include "fem_animate.h"
#include "fem_elem.h"
#include "fem_modal.h"
#include "jpeg_codec.h"
#include "mov_container.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char *what, int line) {
    ++checks;
    if (ok) return;
    std::fflush(stdout);
    std::printf("CHECK FAILED: %s at %s:%d\n", what, __FILE__, line);
    ++failures;
}

#define CHECK(x) Check((x), #x, __LINE__)

using cad::Vec3d;
using fem::AnalysisModel;
using fem::BoundElement;
using fem::ElementShape;
using fem::MovieOptions;
using fem::MovieReport;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// A block of hexes, `nx` by `ny` by `nz` elements over the given size.
AnalysisModel Block(double lx, double ly, double lz, int nx, int ny, int nz) {
    AnalysisModel model;
    fem::StudyMaterial material;
    material.youngs_modulus = fem::MaterialCurve::Constant(210e9);
    material.poissons_ratio = fem::MaterialCurve::Constant(0.3);
    material.density = 7850.0;
    model.materials.push_back(material);
    auto at = [&](int i, int j, int k) { return (k * (ny + 1) + j) * (nx + 1) + i; };
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                model.nodes.push_back(Vec3d{lx * i / nx, ly * j / ny, lz * k / nz});
            }
        }
    }
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                BoundElement element;
                element.shape = ElementShape::Hex8;
                element.nodes = {at(i, j, k),         at(i + 1, j, k),
                                 at(i + 1, j + 1, k), at(i, j + 1, k),
                                 at(i, j, k + 1),     at(i + 1, j, k + 1),
                                 at(i + 1, j + 1, k + 1), at(i, j + 1, k + 1)};
                model.elements.push_back(element);
            }
        }
    }
    return model;
}

bool IsBackground(const std::vector<unsigned char> &pixels, std::size_t pixel,
                  const MovieOptions &options) {
    for (int c = 0; c < 3; ++c) {
        const int had = static_cast<int>(pixels[pixel * 4 + Idx(c)]);
        const int want = static_cast<int>(options.background[Idx(c)]);
        if (std::abs(had - want) > 6) return false;
    }
    return true;
}

int CountDrawn(const std::vector<unsigned char> &pixels, const MovieOptions &options) {
    int drawn = 0;
    for (int i = 0; i < options.width * options.height; ++i) {
        if (!IsBackground(pixels, Idx(i), options)) ++drawn;
    }
    return drawn;
}

int Differing(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b) {
    int count = 0;
    for (std::size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            if (std::abs(static_cast<int>(a[i + Idx(c)]) - static_cast<int>(b[i + Idx(c)])) > 8) {
                ++count;
                break;
            }
        }
    }
    return count;
}

std::string TempPath(const char *name) {
    const char *dir = std::getenv("TMPDIR");
    return std::string(dir != nullptr ? dir : "/tmp") + "/mep-fem-movie-" + name;
}

// --- The projection is not skewed -----------------------------------------
//
// A cube seen from straight along +x, with +z up, is a square: its
// silhouette is symmetric left-to-right about the frame's centre. Getting
// the aspect ratio onto the wrong axis, or the screen y flip wrong, or
// the right and up vectors the wrong way round all break that, and none
// of them is visible in a geometry test because none of them touches the
// geometry.
void ProjectionIsSquare() {
    const AnalysisModel model = Block(1.0, 1.0, 1.0, 2, 2, 2);
    const std::vector<Vec3d> still(Idx(model.NodeCount()), Vec3d{0, 0, 0});
    const std::vector<double> flat(Idx(model.NodeCount()), 1.0);
    MovieOptions options;
    options.width = 240;
    options.height = 180;
    options.supersample = 1;
    options.legend = false;
    options.mesh_lines = false;
    options.ghost_undeformed = false;
    options.camera.yaw = 0.0;
    options.camera.pitch = 0.0;
    options.camera.orbit = false;

    fem::RenderMesh mesh;
    std::string error;
    fem::RenderOptions render = options.render;
    render.auto_scale = false;
    render.displacement_scale = 0.0;
    CHECK(fem::RenderSurface(model, still, flat, render, &mesh, &error));
    std::vector<unsigned char> pixels;
    CHECK(fem::RenderFrame(mesh, nullptr, fem::FitView(model), options, 0.0, 0.0, 1.0, &pixels,
                           &error));
    CHECK(pixels.size() == Idx(options.width * options.height * 4));

    int left = options.width;
    int right = -1;
    int top = options.height;
    int bottom = -1;
    for (int y = 0; y < options.height; ++y) {
        for (int x = 0; x < options.width; ++x) {
            if (IsBackground(pixels, Idx(y * options.width + x), options)) continue;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    CHECK(right > left && bottom > top);
    // Centred, and square: a cube seen face-on is as wide as it is tall,
    // whatever the frame's own aspect ratio is.
    const double centre_x = 0.5 * (left + right);
    const double centre_y = 0.5 * (top + bottom);
    CHECK(std::fabs(centre_x - options.width * 0.5) < 2.0);
    CHECK(std::fabs(centre_y - options.height * 0.5) < 2.0);
    const double width = right - left;
    const double height = bottom - top;
    CHECK(std::fabs(width - height) < 3.0);
    std::printf("  a unit cube, seen face on, %g by %g pixels, centred at (%.1f, %.1f)\n", width,
                height, centre_x, centre_y);
}

// --- The depth test hides what is behind ----------------------------------
void NearerHidesFarther() {
    // Two blocks, one directly behind the other along the view direction.
    AnalysisModel one = Block(1.0, 1.0, 1.0, 1, 1, 1);
    AnalysisModel two = one;
    const int base = two.NodeCount();
    for (int n = 0; n < base; ++n) {
        Vec3d p = two.nodes[Idx(n)];
        // The camera below sits at +x and looks back along -x, so a
        // block at negative x is the one behind. Getting this backwards
        // puts the "hidden" block in front and the test passes for the
        // wrong reason -- which it did, the first time.
        p.x -= 4.0;
        two.nodes.push_back(p);
    }
    BoundElement behind = two.elements.front();
    for (int &node : behind.nodes) node += base;
    two.elements.push_back(behind);

    MovieOptions options;
    options.width = 200;
    options.height = 200;
    options.supersample = 1;
    options.legend = false;
    options.mesh_lines = false;
    options.ghost_undeformed = false;
    options.camera.yaw = 0.0;
    options.camera.pitch = 0.0;
    // A fixed distance and no auto-fit, so the second block does not
    // change the framing and the comparison is of the same picture.
    options.camera.distance = 12.0;

    auto render = [&](const AnalysisModel &model, std::vector<unsigned char> *pixels) {
        const std::vector<Vec3d> still(Idx(model.NodeCount()), Vec3d{0, 0, 0});
        const std::vector<double> flat(Idx(model.NodeCount()), 1.0);
        fem::RenderOptions render_options = options.render;
        render_options.auto_scale = false;
        render_options.displacement_scale = 0.0;
        fem::RenderMesh mesh;
        std::string error;
        fem::MovieView view;
        view.centre = Vec3d{0.5, 0.5, 0.5};
        view.radius = 1.0;
        CHECK(fem::RenderSurface(model, still, flat, render_options, &mesh, &error));
        CHECK(fem::RenderFrame(mesh, nullptr, view, options, 0.0, 0.0, 1.0, pixels, &error));
    };
    std::vector<unsigned char> front;
    std::vector<unsigned char> both;
    render(one, &front);
    render(two, &both);
    // The far block is entirely behind the near one and the same size, so
    // it must contribute nothing at all. Anything it does contribute is a
    // depth test that is backwards or missing.
    const int changed = Differing(front, both);
    std::printf("  a second block, 4 units behind the first, changes %d pixels\n", changed);
    CHECK(changed == 0);
}

// --- The colour bar measures the field ------------------------------------
void LegendEndsMatchTheMap() {
    const AnalysisModel model = Block(1.0, 1.0, 1.0, 1, 1, 1);
    const std::vector<Vec3d> still(Idx(model.NodeCount()), Vec3d{0, 0, 0});
    std::vector<double> field(Idx(model.NodeCount()), 0.0);
    for (int n = 0; n < model.NodeCount(); ++n) field[Idx(n)] = model.nodes[Idx(n)].z;

    MovieOptions options;
    options.width = 320;
    options.height = 240;
    options.supersample = 1;
    options.legend = true;
    options.units = "Z";
    options.caption = "COLOUR BAR";
    options.render.color_map = fem::ColorMap::Viridis;

    fem::RenderOptions render = options.render;
    render.auto_scale = false;
    render.displacement_scale = 0.0;
    fem::RenderMesh mesh;
    std::string error;
    CHECK(fem::RenderSurface(model, still, field, render, &mesh, &error));
    std::vector<unsigned char> pixels;
    CHECK(fem::RenderFrame(mesh, nullptr, fem::FitView(model), options, 0.0, 0.0, 1.0, &pixels,
                           &error));

    // The bar's geometry is DrawLegend's, restated here rather than
    // exported: a test that asks the code under test where to look can
    // only ever agree with it.
    const int scale = std::max(1, options.height / 260);
    const int margin = 8 * scale;
    const int bar_w = 10 * scale;
    const int bar_h = options.height / 3;
    const int bar_x = options.width - margin - bar_w;
    const int bar_y = (options.height - bar_h) / 2;
    const int middle = bar_x + bar_w / 2;
    unsigned char top_want[4];
    unsigned char bottom_want[4];
    fem::MapColor(options.render.color_map, 1.0, top_want);
    fem::MapColor(options.render.color_map, 0.0, bottom_want);
    const std::size_t top_at = Idx(bar_y * options.width + middle);
    const std::size_t bottom_at = Idx((bar_y + bar_h - 1) * options.width + middle);
    for (int c = 0; c < 3; ++c) {
        CHECK(std::abs(static_cast<int>(pixels[top_at * 4 + Idx(c)]) -
                       static_cast<int>(top_want[Idx(c)])) <= 2);
        CHECK(std::abs(static_cast<int>(pixels[bottom_at * 4 + Idx(c)]) -
                       static_cast<int>(bottom_want[Idx(c)])) <= 2);
    }
    // And the caption is actually drawn: some pixel in the top-left
    // corner is neither the background nor the part.
    int lit = 0;
    for (int y = margin; y < margin + 8 * scale; ++y) {
        for (int x = margin; x < margin + 200; ++x) {
            if (x >= options.width) continue;
            if (!IsBackground(pixels, Idx(y * options.width + x), options)) ++lit;
        }
    }
    std::printf("  the caption lights %d pixels; the bar's ends are the map's ends\n", lit);
    CHECK(lit > 40);
}

// --- A mode animation really moves ----------------------------------------
//
// THE BUG THIS EXISTS FOR. RenderSurface's auto-scale makes the largest
// displacement a fixed fraction of the model, per call. Applied once per
// frame it normalises every frame to the same size, and a mode shape
// swinging from one extreme through zero to the other is then drawn at
// full amplitude in all of them -- an animation of a thing that never
// moves. The film fixes one scale over the whole sequence instead, and
// the test of that is that the frame at the middle of the cycle, where
// the displacement really is zero, looks like the undeformed part, while
// the frame at the extreme does not.
void TheAmplitudeIsNotRenormalised() {
    const AnalysisModel model = Block(4.0, 0.4, 0.3, 8, 1, 1);
    // A hand-made "mode": a cantilever's first bending shape, which needs
    // no solver and has a known sign.
    fem::ModalResult modes;
    modes.frequency.push_back(20.0);
    modes.eigenvalue.push_back(20.0 * 20.0 * 39.47841760435743);
    modes.shape.emplace_back();
    std::vector<Vec3d> &shape = modes.shape.back();
    shape.assign(Idx(model.NodeCount()), Vec3d{0, 0, 0});
    for (int n = 0; n < model.NodeCount(); ++n) {
        const double x = model.nodes[Idx(n)].x / 4.0;
        shape[Idx(n)].z = x * x;
    }

    fem::AnimationOptions animation_options;
    animation_options.frames = 8;
    animation_options.amplitude_fraction = 0.10;
    fem::Animation animation;
    std::string error;
    CHECK(fem::AnimateMode(model, modes, 0, animation_options, &animation, &error));
    CHECK(animation.frames == 8);

    MovieOptions options;
    options.width = 280;
    options.height = 160;
    options.supersample = 1;
    options.legend = false;
    options.mesh_lines = false;
    options.ghost_undeformed = false;
    options.camera.yaw = 1.5707963267948966;  // along -y, so bending in z is edge on
    options.camera.pitch = 0.0;

    const std::vector<double> flat(Idx(model.NodeCount()), 1.0);
    const fem::MovieView view = fem::FitView(model);
    // The scale WriteMovie computes, restated: the peak over the whole
    // sequence, not this frame's.
    double peak = 0.0;
    for (const std::vector<Vec3d> &frame : animation.displacement) {
        for (const Vec3d &u : frame) peak = std::max(peak, std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z));
    }
    const double scale = options.render.auto_scale_fraction * view.radius * 2.0 / peak;

    auto frame_pixels = [&](const std::vector<Vec3d> &displacement) {
        fem::RenderOptions render = options.render;
        render.auto_scale = false;
        render.displacement_scale = scale;
        render.auto_range = false;
        render.range_min = 0.0;
        render.range_max = 1.0;
        fem::RenderMesh mesh;
        std::vector<unsigned char> pixels;
        CHECK(fem::RenderSurface(model, displacement, flat, render, &mesh, &error));
        CHECK(fem::RenderFrame(mesh, nullptr, view, options, 0.0, 0.0, 1.0, &pixels, &error));
        return pixels;
    };

    const std::vector<Vec3d> rest(Idx(model.NodeCount()), Vec3d{0, 0, 0});
    const std::vector<unsigned char> undeformed = frame_pixels(rest);
    // A full cycle over eight frames: frame 0 is the peak, frame 2 is
    // the zero crossing, frame 4 is the opposite peak.
    const std::vector<unsigned char> at_peak = frame_pixels(animation.displacement[0]);
    const std::vector<unsigned char> at_zero = frame_pixels(animation.displacement[2]);
    const std::vector<unsigned char> at_other = frame_pixels(animation.displacement[4]);

    const int zero_moved = Differing(undeformed, at_zero);
    const int peak_moved = Differing(undeformed, at_peak);
    const int other_moved = Differing(undeformed, at_other);
    std::printf("  against the undeformed frame: the zero crossing differs in %d pixels,"
                " the peak in %d, the opposite peak in %d\n",
                zero_moved, peak_moved, other_moved);
    CHECK(zero_moved < 40);
    CHECK(peak_moved > 500);
    // The two extremes are opposite displacements of the same shape, so
    // they must move by about the same amount and in opposite directions.
    CHECK(other_moved > 500);
    CHECK(std::fabs(static_cast<double>(peak_moved - other_moved)) <
          0.25 * static_cast<double>(peak_moved));
    CHECK(Differing(at_peak, at_other) > 900);
}

// --- The whole chain, out to a file and back ------------------------------
void TheFilmReadsBack() {
    const AnalysisModel model = Block(2.0, 0.5, 0.4, 6, 2, 2);
    std::vector<std::vector<Vec3d>> frames;
    std::vector<std::vector<double>> fields;
    const int frame_count = 6;
    for (int f = 0; f < frame_count; ++f) {
        const double t = static_cast<double>(f) / frame_count;
        std::vector<Vec3d> displacement(Idx(model.NodeCount()), Vec3d{0, 0, 0});
        std::vector<double> field(Idx(model.NodeCount()), 0.0);
        for (int n = 0; n < model.NodeCount(); ++n) {
            const double x = model.nodes[Idx(n)].x / 2.0;
            displacement[Idx(n)].z = 0.02 * x * x * std::sin(6.283185307179586 * t);
            field[Idx(n)] = x;
        }
        frames.push_back(std::move(displacement));
        fields.push_back(std::move(field));
    }

    MovieOptions options;
    options.width = 240;
    options.height = 180;
    options.fps = 12;
    options.supersample = 2;
    options.caption = "A TEST FILM";
    options.units = "X OVER L";

    const std::string path = TempPath("chain.mov");
    MovieReport report;
    std::string error;
    const bool made = fem::WriteMovie(model, frames, fields, options, path, &report, &error);
    if (!made) std::printf("  WriteMovie: %s\n", error.c_str());
    CHECK(made);
    if (!made) return;
    CHECK(report.frames == frame_count);
    CHECK(report.width == options.width && report.height == options.height);
    CHECK(std::fabs(report.seconds - static_cast<double>(frame_count) / options.fps) < 1e-9);
    CHECK(report.bytes > 0);

    mov::MovFile file;
    std::string open_error;
    const bool opened = mov::OpenMovFile(path, &file, &open_error);
    if (!opened) std::printf("  OpenMovFile: %s\n", open_error.c_str());
    CHECK(opened);
    if (!opened) return;
    CHECK(file.width == options.width);
    CHECK(file.height == options.height);
    CHECK(std::fabs(file.fps - options.fps) < 1e-6);
    CHECK(static_cast<int>(file.frame_index.size()) == frame_count);

    // And a frame really decodes to the picture that was drawn. JPEG is
    // lossy, so the claim is a mean difference of a few levels, not
    // equality -- but a frame that is blank, transposed, or a different
    // frame entirely fails it by a mile.
    const std::vector<unsigned char> bytes = mov::ReadMovFrameJpeg(path, file, 1);
    CHECK(!bytes.empty());
    int width = 0;
    int height = 0;
    std::string decode_error;
    unsigned char *decoded = jpeg::Decode(bytes.data(), bytes.size(), &width, &height, &decode_error);
    CHECK(decoded != nullptr);
    if (decoded == nullptr) return;
    CHECK(width == options.width && height == options.height);

    fem::RenderOptions render = options.render;
    render.auto_scale = false;
    render.displacement_scale = report.displacement_scale;
    render.auto_range = false;
    render.range_min = report.field_min;
    render.range_max = report.field_max;
    fem::RenderMesh mesh;
    CHECK(fem::RenderSurface(model, frames[1], fields[1], render, &mesh, &error));
    fem::RenderMesh ghost;
    fem::RenderOptions rest = render;
    rest.undeformed = true;
    rest.displacement_scale = 0.0;
    CHECK(fem::RenderSurface(model, frames[1], fields[1], rest, &ghost, &error));
    MovieOptions fixed = options;
    fixed.render = render;
    std::vector<unsigned char> drawn;
    CHECK(fem::RenderFrame(mesh, &ghost, fem::FitView(model), fixed, 0.0, report.field_min,
                           report.field_max, &drawn, &error));

    double total = 0.0;
    for (int i = 0; i < width * height; ++i) {
        for (int c = 0; c < 3; ++c) {
            total += std::fabs(static_cast<double>(decoded[Idx(i * 4 + c)]) -
                               static_cast<double>(drawn[Idx(i * 4 + c)]));
        }
    }
    const double mean = total / (static_cast<double>(width * height) * 3.0);
    std::printf("  frame 1 read back from the file differs from the drawing by %.2f levels"
                " on average (%d bytes for %d frames)\n",
                mean, static_cast<int>(report.bytes), report.frames);
    CHECK(mean < 6.0);
    std::free(decoded);

    // And a still of the same frame is a PNG that is not empty.
    const std::string still = TempPath("still.png");
    MovieReport still_report;
    CHECK(fem::WriteStill(model, frames[1], fields[1], options, still, &still_report, &error));
    CHECK(still_report.bytes > 1000);
    std::printf("  the still is %d bytes of PNG\n", static_cast<int>(still_report.bytes));
    std::remove(path.c_str());
    std::remove(still.c_str());
}

// --- The camera can be told to turn ---------------------------------------
void OrbitTurnsTheCamera() {
    const AnalysisModel model = Block(2.0, 0.3, 0.3, 4, 1, 1);
    const std::vector<Vec3d> still(Idx(model.NodeCount()), Vec3d{0, 0, 0});
    const std::vector<double> flat(Idx(model.NodeCount()), 1.0);
    MovieOptions options;
    options.width = 200;
    options.height = 150;
    options.supersample = 1;
    options.legend = false;
    options.ghost_undeformed = false;
    options.mesh_lines = false;
    options.camera.pitch = 0.0;
    // A FIXED DISTANCE, because the point being tested is the bearing.
    // Left to fit itself the camera moves in until the part fills the
    // frame from whichever side it is seen, which is the right behaviour
    // and would make this comparison measure nothing.
    options.camera.distance = 6.0;

    fem::RenderOptions render = options.render;
    render.auto_scale = false;
    render.displacement_scale = 0.0;
    fem::RenderMesh mesh;
    std::string error;
    CHECK(fem::RenderSurface(model, still, flat, render, &mesh, &error));
    const fem::MovieView view = fem::FitView(model);
    std::vector<unsigned char> along;
    std::vector<unsigned char> across;
    // A long thin bar seen end-on covers far less of the frame than the
    // same bar seen broadside, which is the whole reason an orbit is
    // worth having.
    options.camera.yaw = 0.0;
    CHECK(fem::RenderFrame(mesh, nullptr, view, options, 0.0, 0.0, 1.0, &along, &error));
    CHECK(fem::RenderFrame(mesh, nullptr, view, options, 1.5707963267948966, 0.0, 1.0, &across,
                           &error));
    const int end_on = CountDrawn(along, options);
    const int broadside = CountDrawn(across, options);
    std::printf("  a 2 x 0.3 x 0.3 bar covers %d pixels end on and %d broadside\n", end_on,
                broadside);
    CHECK(broadside > 2 * end_on);

    // AND AN ORBITING CAMERA DOES NOT BREATHE. Left to fit itself per
    // frame, the camera moves in as a long part turns edge on and out
    // again as it comes broadside, and the film shows a part pulsing
    // rather than turning. The fit is therefore the worst case over the
    // whole circle, done once -- so at every turn the part is inside the
    // frame, and at some turn it nearly fills it.
    options.camera.distance = 0.0;
    options.camera.orbit = true;
    int widest = 0;
    int touching_the_edge = 0;
    for (int step = 0; step < 12; ++step) {
        const double turn = 6.283185307179586 * step / 12.0;
        std::vector<unsigned char> pixels;
        CHECK(fem::RenderFrame(mesh, nullptr, view, options, turn, 0.0, 1.0, &pixels, &error));
        int left = options.width;
        int right = -1;
        for (int y = 0; y < options.height; ++y) {
            for (int x = 0; x < options.width; ++x) {
                if (IsBackground(pixels, Idx(y * options.width + x), options)) continue;
                left = std::min(left, x);
                right = std::max(right, x);
                if (x <= 1 || y <= 1 || x >= options.width - 2 || y >= options.height - 2) {
                    ++touching_the_edge;
                }
            }
        }
        widest = std::max(widest, right - left);
    }
    std::printf("  orbiting, the widest view spans %d of %d pixels and nothing reaches the"
                " frame's edge (%d)\n",
                widest, options.width, touching_the_edge);
    CHECK(touching_the_edge == 0);
    CHECK(widest > options.width / 2);
}

}  // namespace

int main() {
    std::printf("the projection\n");
    ProjectionIsSquare();
    std::printf("the depth test\n");
    NearerHidesFarther();
    std::printf("the legend\n");
    LegendEndsMatchTheMap();
    std::printf("a mode animation\n");
    TheAmplitudeIsNotRenormalised();
    std::printf("the camera\n");
    OrbitTurnsTheCamera();
    std::printf("the whole chain\n");
    TheFilmReadsBack();

    if (failures != 0) {
        std::printf("fem_movie_test FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("fem_movie_test passed (%d checks)\n", checks);
    return 0;
}
