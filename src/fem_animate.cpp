#include "fem_animate.h"

#include <algorithm>
#include <cmath>

namespace fem {
namespace {

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

double Diagonal(const AnalysisModel &model) {
    if (model.nodes.empty()) return 0.0;
    cad::Vec3d low = model.nodes[0];
    cad::Vec3d high = low;
    for (const cad::Vec3d &p : model.nodes) {
        low = cad::Vec3d{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = cad::Vec3d{std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    return (high - low).Length();
}

}  // namespace

std::vector<double> FrameTimes(double from, double to, int frames) {
    std::vector<double> out;
    if (frames <= 0) return out;
    if (frames == 1) {
        out.push_back(from);
        return out;
    }
    for (int i = 0; i < frames; ++i) {
        out.push_back(from + (to - from) * static_cast<double>(i) /
                                 static_cast<double>(frames - 1));
    }
    return out;
}

bool AnimateMode(const AnalysisModel &model, const ModalResult &modes, int mode,
                 const AnimationOptions &options, Animation *out, std::string *error) {
    *out = Animation{};
    if (mode < 0 || mode >= static_cast<int>(modes.shape.size())) {
        *error = "no such mode";
        return false;
    }
    const std::vector<cad::Vec3d> &shape = modes.shape[Idx(mode)];
    if (shape.size() != model.nodes.size()) {
        *error = "the mode shape is not one vector per node";
        return false;
    }
    if (options.frames <= 0) {
        *error = "an animation needs at least one frame";
        return false;
    }
    double largest = 0.0;
    for (const cad::Vec3d &u : shape) largest = std::max(largest, u.Length());
    if (!(largest > 0.0)) {
        *error = "the mode shape is everywhere zero";
        return false;
    }
    const double diagonal = Diagonal(model);
    out->amplitude_scale =
        diagonal > 0.0 ? options.amplitude_fraction * diagonal / largest : 1.0;
    // A RIGID-BODY MODE HAS NO PERIOD, and dividing by its zero frequency
    // would give an animation of infinite length rather than an error.
    // Giving it a period of one second is the useful answer: the shape is
    // still worth looking at -- it is how you see that a model is not held
    // -- and one second is as good a speed as any for a motion that has
    // no natural one.
    const double frequency = mode < static_cast<int>(modes.frequency.size())
                                 ? modes.frequency[Idx(mode)]
                                 : 0.0;
    out->period = frequency > 0.0 ? 1.0 / frequency : 1.0;
    out->frames = options.frames;
    for (int frame = 0; frame < options.frames; ++frame) {
        // The last frame of a full cycle is one step short of the first,
        // not a copy of it, so playing the frames on a loop is seamless.
        const double fraction = static_cast<double>(frame) /
                                static_cast<double>(options.full_cycle
                                                        ? options.frames
                                                        : std::max(1, options.frames - 1));
        const double angle = 2.0 * cad::kPi * (options.full_cycle ? fraction : fraction * 0.5);
        const double amplitude = std::cos(angle) * out->amplitude_scale;
        out->time.push_back(fraction * out->period * (options.full_cycle ? 1.0 : 0.5));
        std::vector<cad::Vec3d> field(shape.size());
        for (std::size_t i = 0; i < shape.size(); ++i) field[i] = shape[i] * amplitude;
        out->displacement.push_back(std::move(field));
    }
    return true;
}

double SampleAt(const std::vector<double> &times, const std::vector<double> &values, double at) {
    if (times.empty() || values.size() != times.size()) return 0.0;
    if (at <= times.front()) return values.front();
    if (at >= times.back()) return values.back();
    // CLAMPED AT BOTH ENDS RATHER THAN EXTRAPOLATED. A probe asked for a
    // time before the history starts is a caller's mistake, and answering
    // it with a straight line run backwards out of the first two samples
    // invents a value that looks like data.
    const auto upper = std::upper_bound(times.begin(), times.end(), at);
    const std::size_t after = static_cast<std::size_t>(upper - times.begin());
    const std::size_t before = after - 1;
    const double span = times[after] - times[before];
    if (!(span > 0.0)) return values[before];
    const double alpha = (at - times[before]) / span;
    return values[before] + (values[after] - values[before]) * alpha;
}

bool ResampleHistory(const std::vector<double> &times,
                     const std::vector<std::vector<double>> &states,
                     const std::vector<double> &at, std::vector<std::vector<double>> *out,
                     std::string *error) {
    out->clear();
    if (times.size() != states.size()) {
        *error = "there is not one recorded state per recorded time";
        return false;
    }
    if (times.empty()) {
        *error = "the history is empty";
        return false;
    }
    for (std::size_t i = 1; i < times.size(); ++i) {
        if (times[i] < times[i - 1]) {
            *error = "the recorded times are not ascending";
            return false;
        }
    }
    const std::size_t width = states.front().size();
    for (const std::vector<double> &state : states) {
        if (state.size() != width) {
            *error = "the recorded states are not all the same length";
            return false;
        }
    }
    std::vector<double> column(times.size(), 0.0);
    out->assign(at.size(), std::vector<double>(width, 0.0));
    // Walked one field component at a time so the interpolation is the
    // same code the scalar probe uses, rather than a second copy of it
    // written for vectors.
    for (std::size_t c = 0; c < width; ++c) {
        for (std::size_t i = 0; i < times.size(); ++i) column[i] = states[i][c];
        for (std::size_t f = 0; f < at.size(); ++f) {
            (*out)[f][c] = SampleAt(times, column, at[f]);
        }
    }
    return true;
}

}  // namespace fem
