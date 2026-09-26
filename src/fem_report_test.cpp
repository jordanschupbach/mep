// Parts J.3 and J.4: animation, probes, path plots, time histories and
// the tables they leave in.

#include "fem_animate.h"
#include "fem_probe.h"

#include "fem_thermal.h"

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

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }
bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

AnalysisModel Bar(int nx, int ny, int nz, const Vec3d &size) {
    AnalysisModel model;
    fem::StudyMaterial material;
    material.youngs_modulus = fem::MaterialCurve::Constant(210e9);
    material.poissons_ratio = fem::MaterialCurve::Constant(0.3);
    material.density = 7850.0;
    model.materials.push_back(material);
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                model.nodes.push_back(Vec3d{size.x * i / nx, size.y * j / ny, size.z * k / nz});
            }
        }
    }
    auto at = [&](int i, int j, int k) { return (k * (ny + 1) + j) * (nx + 1) + i; };
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

// --- J.3: animation --------------------------------------------------------

void TestModeAnimation() {
    std::printf("mode shape animation:\n");
    AnalysisModel model = Bar(8, 2, 2, Vec3d{0.4, 0.05, 0.05});
    for (int node = 0; node < model.NodeCount(); ++node) {
        if (!Near(model.nodes[Idx(node)].x, 0.0, 1e-12)) continue;
        fem::Constraint constraint;
        constraint.node = node;
        constraint.fixed[0] = constraint.fixed[1] = constraint.fixed[2] = true;
        model.constraints.push_back(constraint);
    }
    fem::ModalOptions options;
    options.modes = 3;
    fem::ModalResult modes;
    CHECK(fem::SolveModal(model, {}, options, &modes));
    if (!modes.ok) {
        std::printf("  %s\n", modes.error.c_str());
        return;
    }
    std::printf("  a cantilever's first three frequencies: %.2f, %.2f, %.2f Hz\n",
                modes.frequency[0], modes.frequency[1], modes.frequency[2]);

    fem::AnimationOptions animation;
    animation.frames = 24;
    animation.amplitude_fraction = 0.08;
    fem::Animation out;
    std::string error;
    CHECK(fem::AnimateMode(model, modes, 0, animation, &out, &error));
    CHECK(out.frames == 24);
    CHECK(static_cast<int>(out.displacement.size()) == 24);

    // THE PERIOD IS THE MODE'S OWN, so two modes animated side by side
    // move at their true relative speeds -- which is most of what a
    // modal animation is for and is lost the moment every mode is given
    // the same one-second loop.
    std::printf("  frame times span %.6f s against the mode's period %.6f s\n", out.time.back(),
                out.period);
    CHECK(Near(out.period, 1.0 / modes.frequency[0], 1e-12));

    // Peak displacement is the requested fraction of the diagonal. A mode
    // shape is an eigenvector with no magnitude of its own -- mass
    // normalised, its amplitude is in units of one over root mass -- so
    // this is the only scale it can have.
    const double diagonal = Vec3d{0.4, 0.05, 0.05}.Length();
    double peak = 0.0;
    for (const auto &frame : out.displacement) {
        for (const Vec3d &u : frame) peak = std::max(peak, u.Length());
    }
    std::printf("  peak movement %.6f of the diagonal (asked for %.2f)\n", peak / diagonal,
                animation.amplitude_fraction);
    CHECK(Near(peak / diagonal, animation.amplitude_fraction, 1e-12));

    // Frame k is cos(2 pi k / N) times the shape, exactly: a mode
    // animation is a cosine and nothing else, and the extremes half a
    // cycle apart are exact negatives of each other.
    double worst = 0.0;
    for (int frame = 0; frame < 24; ++frame) {
        const double expected =
            std::cos(2.0 * cad::kPi * static_cast<double>(frame) / 24.0) * out.amplitude_scale;
        for (std::size_t i = 0; i < modes.shape[0].size(); ++i) {
            worst = std::max(worst, (out.displacement[Idx(frame)][i] -
                                     modes.shape[0][i] * expected).Length());
        }
    }
    std::printf("  every frame is cos(2 pi k / N) times the shape, to %.2e of a peak %.2e\n",
                worst, peak);
    CHECK(worst < peak * 1e-14);
    for (std::size_t i = 0; i < modes.shape[0].size(); ++i) {
        CHECK((out.displacement[0][i] + out.displacement[12][i]).Length() < peak * 1e-14);
    }

    // THE LOOP CLOSES: the last frame is one step short of the first
    // rather than a copy of it, so playing the frames round on a loop
    // does not stutter. Checked by continuing the cosine one frame past
    // the end and finding the first frame there.
    const double next = std::cos(2.0 * cad::kPi * 24.0 / 24.0) * out.amplitude_scale;
    for (std::size_t i = 0; i < modes.shape[0].size(); ++i) {
        CHECK((out.displacement[0][i] - modes.shape[0][i] * next).Length() < peak * 1e-14);
    }
    std::printf("  the 24th frame continues into the 0th exactly, so the loop is seamless\n");

    fem::Animation ignored;
    CHECK(!fem::AnimateMode(model, modes, 99, animation, &ignored, &error));
    fem::AnimationOptions none = animation;
    none.frames = 0;
    CHECK(!fem::AnimateMode(model, modes, 0, none, &ignored, &error));
}

void TestHistoryResampling() {
    std::printf("resampling a recorded history onto a frame rate:\n");
    // AN EXPONENTIAL DECAY, whose value at any time is known, sampled
    // coarsely and then resampled finely. The error is the linear
    // interpolation's and must fall as the square of the sample spacing:
    // a routine that snapped to the nearest recorded frame instead --
    // which is the obvious implementation and produces an animation that
    // holds still and jumps -- converges at first order and is caught by
    // the rate rather than by the values.
    auto exact = [](double t) { return std::exp(-3.0 * t); };
    double error_at[3] = {0, 0, 0};
    const int counts[3] = {9, 17, 33};
    for (int c = 0; c < 3; ++c) {
        std::vector<double> times;
        std::vector<std::vector<double>> states;
        for (int i = 0; i < counts[c]; ++i) {
            const double t = static_cast<double>(i) / (counts[c] - 1);
            times.push_back(t);
            states.push_back({exact(t), 2.0 * exact(t)});
        }
        const std::vector<double> frames = fem::FrameTimes(0.0, 1.0, 101);
        std::vector<std::vector<double>> out;
        std::string error;
        CHECK(fem::ResampleHistory(times, states, frames, &out, &error));
        CHECK(out.size() == frames.size());
        double worst = 0.0;
        for (std::size_t f = 0; f < frames.size(); ++f) {
            worst = std::max(worst, std::fabs(out[f][0] - exact(frames[f])));
            // The second column is twice the first at every recorded
            // time, and must stay so at every resampled one: a
            // resampling that mixed columns up would still converge.
            CHECK(Near(out[f][1], 2.0 * out[f][0], 1e-15));
        }
        error_at[c] = worst;
        std::printf("  %2d samples -> 101 frames: worst error %.4e\n", counts[c], worst);
    }
    const double rate = 0.5 * (std::log2(error_at[0] / error_at[1]) +
                               std::log2(error_at[1] / error_at[2]));
    std::printf("  the interpolation error falls at rate %.3f\n", rate);
    CHECK(rate > 1.8 && rate < 2.2);

    // At a recorded time the answer is the recorded value, exactly.
    std::vector<double> times{0.0, 1.0, 3.0};
    std::vector<double> values{10.0, 20.0, 60.0};
    CHECK(fem::SampleAt(times, values, 0.0) == 10.0);
    CHECK(fem::SampleAt(times, values, 1.0) == 20.0);
    CHECK(fem::SampleAt(times, values, 3.0) == 60.0);
    CHECK(Near(fem::SampleAt(times, values, 2.0), 40.0, 1e-15));
    // Clamped rather than extrapolated: answering for a time before the
    // history starts by running a line backwards invents a number that
    // looks like data.
    CHECK(fem::SampleAt(times, values, -5.0) == 10.0);
    CHECK(fem::SampleAt(times, values, 99.0) == 60.0);

    std::vector<std::vector<double>> ignored;
    std::string error;
    CHECK(!fem::ResampleHistory({0.0, 1.0}, {{1.0}}, {0.5}, &ignored, &error));
    CHECK(!fem::ResampleHistory({1.0, 0.0}, {{1.0}, {2.0}}, {0.5}, &ignored, &error));
    CHECK(!fem::ResampleHistory({}, {}, {0.5}, &ignored, &error));
}

void TestTransientRecordsItsHistory() {
    std::printf("a transient recorded at asked-for times rather than at its own steps:\n");
    // A BAR COOLING FROM A UNIFORM TEMPERATURE with both ends held: the
    // adaptive integrator takes short steps at the start and long ones
    // later, so a history recorded at *its* steps is unevenly spaced and
    // changes shape with the tolerance. Asking for fixed times is the
    // point, and the test is that the recorded fields are the ones the
    // solve actually passed through.
    AnalysisModel bar = Bar(20, 1, 1, Vec3d{0.5, 0.02, 0.02});
    fem::ThermalModel thermal;
    thermal.nodes = bar.nodes;
    thermal.elements = bar.elements;
    fem::ThermalMaterial heat;
    heat.density = 7850.0;
    heat.specific_heat = fem::MaterialCurve::Constant(460.0);
    heat.conductivity = fem::MaterialCurve::Constant(50.0);
    thermal.materials.push_back(heat);
    for (int node = 0; node < bar.NodeCount(); ++node) {
        const double x = bar.nodes[Idx(node)].x;
        if (Near(x, 0.0, 1e-12) || Near(x, 0.5, 1e-12)) {
            thermal.fixed.push_back({node, 0.0});
        }
    }
    const std::vector<double> initial(bar.nodes.size(), 100.0);
    fem::TransientOptions options;
    options.theta = 0.5;
    // FOUR THOUSAND SECONDS, BECAUSE STEEL IS SLOW. The diffusivity is
    // k over rho c, 1.39e-05 m^2/s, so a half-metre bar has a
    // fundamental time constant of L^2/(pi^2 alpha) = 1830 s. I first
    // wrote forty and the centre of the bar was still at its initial
    // hundred degrees at the end -- correct physics, and a test measuring
    // nothing.
    options.end_time = 4000.0;
    options.step = 20.0;
    options.adaptive = true;
    options.sample_times = fem::FrameTimes(0.0, 4000.0, 21);
    fem::TransientResult result;
    CHECK(fem::SolveTransientHeat(thermal, initial, options, &result));
    if (!result.ok) {
        std::printf("  %s\n", result.error.c_str());
        return;
    }
    std::printf("  %d steps between %.4f and %.4f s, %d samples asked for and %d recorded\n",
                result.steps, result.smallest_step, result.largest_step,
                static_cast<int>(options.sample_times.size()),
                static_cast<int>(result.samples.size()));
    CHECK(result.samples.size() == options.sample_times.size());
    // The frame rate is finer than the steps in places and coarser in
    // others, which is the situation this exists for.
    CHECK(result.largest_step > 200.0);

    // The first sample is the initial condition and the last is the
    // solution the solve returned -- the two the history shares with the
    // rest of the result, and the two that would show a recorder that was
    // off by one step.
    double worst_first = 0.0;
    double worst_last = 0.0;
    double worst_held = 0.0;
    for (std::size_t i = 0; i < bar.nodes.size(); ++i) {
        const double x = bar.nodes[i].x;
        const bool held = Near(x, 0.0, 1e-12) || Near(x, 0.5, 1e-12);
        // AT THE FREE NODES ONLY. A prescribed temperature is imposed at
        // the first step rather than blended in -- a body at one
        // temperature with a face suddenly held at another is the
        // commonest transient there is, and that disagreement is the
        // problem rather than an inconsistency to smooth away -- so the
        // held nodes are *not* at the initial value in the first sample,
        // and asserting that they are tests the wrong thing. What they
        // must be is the prescribed value, which is the sharper check.
        if (held) {
            worst_held = std::max(worst_held, std::fabs(result.samples.front()[i]));
        } else {
            worst_first = std::max(worst_first, std::fabs(result.samples.front()[i] - initial[i]));
        }
        worst_last =
            std::max(worst_last, std::fabs(result.samples.back()[i] - result.temperature[i]));
    }
    std::printf("  first sample: free nodes at the initial 100 K to %.2e, held nodes at 0 K"
                " to %.2e\n",
                worst_first, worst_held);
    std::printf("  last sample is the returned answer to %.2e\n", worst_last);
    CHECK(worst_first < 1e-12);
    CHECK(worst_held < 1e-12);
    CHECK(worst_last < 1e-12);

    // AND IT COOLS MONOTONICALLY, which the closed form says and which a
    // recorder that interpolated across the wrong pair of states would
    // break. The centre of a bar held at zero at both ends decays towards
    // zero without ever turning back.
    // BY POSITION, NOT BY INDEX. Half way through the node array of a
    // bar meshed 20 by 1 by 1 is node 42, which is on the *end* face and
    // is one of the two held at zero -- so the history there is zero
    // throughout and every check of it passes for the wrong reason.
    int centre = 0;
    for (int node = 0; node < bar.NodeCount(); ++node) {
        if (Near(bar.nodes[Idx(node)].x, 0.25, 1e-9)) centre = node;
    }
    double previous = result.samples.front()[Idx(centre)];
    bool monotone = true;
    for (std::size_t f = 1; f < result.samples.size(); ++f) {
        const double now = result.samples[f][Idx(centre)];
        if (now > previous + 1e-9) monotone = false;
        previous = now;
    }
    std::printf("  the centre goes %.3f -> %.3f K, monotonically: %s\n",
                result.samples.front()[Idx(centre)], result.samples.back()[Idx(centre)],
                monotone ? "yes" : "no");
    CHECK(monotone);
    CHECK(result.samples.back()[Idx(centre)] < result.samples.front()[Idx(centre)] * 0.5);
}

// --- J.4: probes -----------------------------------------------------------

void TestProbes() {
    std::printf("point probes:\n");
    AnalysisModel model = Bar(4, 2, 2, Vec3d{1.0, 1.0, 1.0});
    // A GENERAL LINEAR FIELD. Every element in the library reproduces a
    // linear field exactly, so a probe anywhere inside must return the
    // exact value -- not nearly. That is the strongest statement
    // available about an interpolation and it holds at arbitrary points
    // rather than only at nodes, which is what distinguishes a probe from
    // a lookup.
    auto exact = [](const Vec3d &p) { return 3.0 + 2.0 * p.x - 1.5 * p.y + 0.75 * p.z; };
    std::vector<double> field(model.nodes.size());
    for (int node = 0; node < model.NodeCount(); ++node) {
        field[Idx(node)] = exact(model.nodes[Idx(node)]);
    }
    std::vector<Vec3d> at{Vec3d{0.0, 0.0, 0.0},    Vec3d{1.0, 1.0, 1.0},
                          Vec3d{0.5, 0.5, 0.5},    Vec3d{0.137, 0.911, 0.42},
                          Vec3d{0.25, 0.0, 0.625}, Vec3d{0.999, 0.001, 0.5}};
    std::vector<fem::Probe> probes;
    std::string error;
    CHECK(fem::ProbePoints(model, field, at, &probes, &error));
    double worst = 0.0;
    for (const fem::Probe &probe : probes) {
        worst = std::max(worst, std::fabs(probe.value - exact(probe.at)));
        CHECK(probe.inside);
    }
    std::printf("  %d points, including corners and interior: worst error %.2e\n",
                static_cast<int>(probes.size()), worst);
    CHECK(worst < 1e-13);

    // A point outside says so, and reports how far -- which is the
    // difference between a point a hair beyond a faceted boundary and a
    // point somewhere else entirely, and only the caller can judge which
    // it has.
    std::vector<fem::Probe> outside;
    CHECK(fem::ProbePoints(model, field, {Vec3d{3.0, 0.5, 0.5}}, &outside, &error));
    std::printf("  a point two units clear of the model: inside %s, distance %.6f\n",
                outside[0].inside ? "yes" : "no", outside[0].distance);
    CHECK(!outside[0].inside);
    CHECK(outside[0].distance > 1.5);

    CHECK(!fem::ProbePoints(model, std::vector<double>(3), at, &probes, &error));
}

void TestPathPlot() {
    std::printf("path plots:\n");
    AnalysisModel model = Bar(6, 2, 2, Vec3d{1.0, 1.0, 1.0});
    auto exact = [](const Vec3d &p) { return 10.0 * p.x + p.y; };
    std::vector<double> field(model.nodes.size());
    for (int node = 0; node < model.NodeCount(); ++node) {
        field[Idx(node)] = exact(model.nodes[Idx(node)]);
    }
    // A polyline whose two segments differ in length by three to one:
    // sampling per segment rather than by arc length would put three
    // quarters of the points on a quarter of the path, and the plot would
    // show a feature moving as the path was redrawn.
    const std::vector<Vec3d> through{Vec3d{0.0, 0.5, 0.5}, Vec3d{0.25, 0.5, 0.5},
                                     Vec3d{1.0, 0.5, 0.5}};
    std::vector<fem::PathSample> path;
    std::string error;
    CHECK(fem::ProbePath(model, field, through, 41, &path, &error));
    CHECK(path.size() == 41);
    CHECK(Near(path.front().distance, 0.0, 1e-15));
    CHECK(Near(path.back().distance, 1.0, 1e-15));
    double worst_spacing = 0.0;
    double worst_value = 0.0;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i > 0) {
            worst_spacing = std::max(worst_spacing,
                                     std::fabs(path[i].distance - path[i - 1].distance - 0.025));
        }
        worst_value = std::max(worst_value, std::fabs(path[i].value - exact(path[i].at)));
        CHECK(path[i].inside);
    }
    std::printf("  41 samples over a 1.0 m path in two unequal segments:\n");
    std::printf("    spacing even to %.2e, values exact to %.2e\n", worst_spacing, worst_value);
    CHECK(worst_spacing < 1e-15);
    CHECK(worst_value < 1e-13);

    const fem::Table table = fem::PathTable(path, "von Mises");
    CHECK(table.columns.size() == 5);
    CHECK(table.rows.size() == 41);

    std::vector<fem::PathSample> ignored;
    CHECK(!fem::ProbePath(model, field, {Vec3d{}}, 10, &ignored, &error));
    CHECK(!fem::ProbePath(model, field, through, 1, &ignored, &error));
    CHECK(!fem::ProbePath(model, field, {Vec3d{0.5, 0.5, 0.5}, Vec3d{0.5, 0.5, 0.5}}, 10, &ignored,
                          &error));
}

void TestTimeHistory() {
    std::printf("time histories at fixed points:\n");
    AnalysisModel model = Bar(4, 1, 1, Vec3d{1.0, 0.2, 0.2});
    std::vector<double> times;
    std::vector<std::vector<double>> states;
    for (int frame = 0; frame < 11; ++frame) {
        const double t = 0.1 * frame;
        times.push_back(t);
        std::vector<double> state(model.nodes.size());
        for (int node = 0; node < model.NodeCount(); ++node) {
            state[Idx(node)] = model.nodes[Idx(node)].x * std::exp(-t);
        }
        states.push_back(std::move(state));
    }
    const std::vector<Vec3d> at{Vec3d{0.25, 0.1, 0.1}, Vec3d{0.75, 0.1, 0.1}};
    fem::TimeHistory history;
    std::string error;
    CHECK(fem::ProbeTimeHistory(model, times, states, at, &history, &error));
    CHECK(history.time.size() == 11);
    CHECK(history.value.size() == 11);
    CHECK(history.value[0].size() == 2);
    double worst = 0.0;
    for (std::size_t frame = 0; frame < times.size(); ++frame) {
        for (std::size_t p = 0; p < at.size(); ++p) {
            const double want = at[p].x * std::exp(-times[frame]);
            worst = std::max(worst, std::fabs(history.value[frame][p] - want));
        }
    }
    std::printf("  2 points over 11 frames: worst error %.2e against the closed form\n", worst);
    CHECK(worst < 1e-14);
    CHECK(history.inside[0] && history.inside[1]);

    const fem::Table table = fem::HistoryTable(history, "temperature");
    CHECK(table.columns.size() == 3);
    CHECK(table.rows.size() == 11);
    std::printf("  exported columns: %s | %s | %s\n", table.columns[0].c_str(),
                table.columns[1].c_str(), table.columns[2].c_str());

    CHECK(!fem::ProbeTimeHistory(model, times, {}, at, &history, &error));
    CHECK(!fem::ProbeTimeHistory(model, {}, {}, at, &history, &error));
}

// --- Export ----------------------------------------------------------------

void TestExport() {
    std::printf("the tables a result leaves in:\n");
    fem::Table table;
    table.columns = {"distance", "von Mises at (1, 2, 3)"};
    table.rows = {{0.0, 1.0 / 3.0}, {0.5, 1e-18}, {1.0, -2.5e8}};

    const std::string csv = fem::ToCsv(table);
    // A COLUMN NAME WITH A COMMA IN IT is not a hypothetical: every
    // history column is named after a point, and a point has three. Left
    // unquoted, "von Mises at (1, 2, 3)" becomes three columns and the
    // file still parses -- into a table with the wrong shape.
    CHECK(csv.find("\"von Mises at (1, 2, 3)\"") != std::string::npos);
    std::size_t lines = 0;
    for (const char ch : csv) {
        if (ch == '\n') ++lines;
    }
    CHECK(lines == 4);

    // SEVENTEEN SIGNIFICANT DIGITS, so the numbers come back exactly.
    // Exported at six, a third would return as 0.333333 and a comparison
    // against another run would be measuring the formatting.
    std::vector<double> parsed;
    std::size_t start = csv.find('\n') + 1;
    while (start < csv.size()) {
        const std::size_t stop = csv.find('\n', start);
        std::string row = csv.substr(start, stop - start);
        std::size_t comma = 0;
        while (true) {
            const std::size_t next = row.find(',', comma);
            parsed.push_back(std::strtod(row.substr(comma, next - comma).c_str(), nullptr));
            if (next == std::string::npos) break;
            comma = next + 1;
        }
        start = stop == std::string::npos ? csv.size() : stop + 1;
    }
    CHECK(parsed.size() == 6);
    double worst = 0.0;
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
        for (std::size_t c = 0; c < 2; ++c) {
            const double want = table.rows[r][c];
            const double got = parsed[r * 2 + c];
            worst = std::max(worst, want == got ? 0.0 : 1.0);
        }
    }
    std::printf("  csv: %d values, all of them bit-exact on the way back: %s\n",
                static_cast<int>(parsed.size()), worst == 0.0 ? "yes" : "no");
    CHECK(worst == 0.0);

    const std::string org = fem::ToOrgTable(table);
    std::printf("%s", org.c_str());
    // Four lines: a header, org's separator, and three rows.
    std::size_t org_lines = 0;
    std::size_t first_width = std::string::npos;
    bool aligned = true;
    start = 0;
    while (start < org.size()) {
        const std::size_t stop = org.find('\n', start);
        const std::string row = org.substr(start, stop - start);
        ++org_lines;
        if (first_width == std::string::npos) first_width = row.size();
        if (row.size() != first_width) aligned = false;
        CHECK(!row.empty() && row.front() == '|');
        CHECK(row.back() == '|');
        start = stop == std::string::npos ? org.size() : stop + 1;
    }
    CHECK(org_lines == 5);
    // Every line the same width, which is what makes it readable as text
    // as well as parseable as a table -- and an org table that does not
    // line up is exactly as valid and no use at all in a notebook.
    CHECK(aligned);
    CHECK(org.find("|---") == org.find('\n') + 1);
}

}  // namespace

int main() {
    TestModeAnimation();
    TestHistoryResampling();
    TestTransientRecordsItsHistory();
    TestProbes();
    TestPathPlot();
    TestTimeHistory();
    TestExport();
    if (failures == 0) {
        std::printf("fem_report_test passed (%d checks)\n", checks);
        return 0;
    }
    std::printf("fem_report_test FAILED (%d of %d checks)\n", failures, checks);
    return 1;
}
