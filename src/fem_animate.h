#ifndef MEP_FEM_ANIMATE_H
#define MEP_FEM_ANIMATE_H

#include "fem_modal.h"
#include "fem_study.h"

#include <string>
#include <vector>

// Animation (plans/CAD_FEM_PLAN.md Part J.3).
//
// Produces the *frames* -- one displacement field per frame, at known
// times -- and nothing else. The keyframe track, the camera and the video
// encoder already exist for the 3D modeller
// (`Model3DRenderAnimationToVideoFile`), and a second copy of any of them
// built for results would be a second copy to keep working.
//
// WHY A MODE SHAPE NEEDS A SCALE IMPOSED ON IT, which is the one thing
// here that is not obvious. A mode shape is an eigenvector: it has a
// direction and no magnitude. `SolveModal` returns it mass-normalised, so
// `phi^T M phi = 1`, which is what makes a modal superposition's
// participation factors mean anything -- and which makes its amplitude a
// number in units of one over the square root of mass, with no physical
// interpretation as a displacement at all. A mode animation that drew it
// at "true scale" would draw whatever that number happened to be: a
// invisible for a heavy model and off the screen for a light one, with
// the *same mode shape* in both. So the amplitude is chosen from the
// geometry, exactly as the deformed-shape scale is, and the frames say
// what scale was used.
namespace fem {

struct AnimationOptions {
    int frames = 36;
    // Peak displacement as a fraction of the model's bounding diagonal.
    double amplitude_fraction = 0.08;
    // A full cycle returns to where it started, so the last frame is not
    // a repeat of the first and the loop is seamless. A half sweep goes
    // from one extreme to the other and back is the viewer's problem --
    // useful for a still sequence rather than a loop.
    bool full_cycle = true;
};

struct Animation {
    // Seconds. For a mode, real seconds at the mode's own frequency, so
    // two modes animated side by side move at their true relative speeds
    // -- which is most of what a modal animation is for.
    std::vector<double> time;
    std::vector<std::vector<cad::Vec3d>> displacement;
    double amplitude_scale = 1.0;
    double period = 0.0;
    int frames = 0;
};

// One mode, animated over a cycle of its own period.
bool AnimateMode(const AnalysisModel &model, const ModalResult &modes, int mode,
                 const AnimationOptions &options, Animation *out, std::string *error);

// A recorded time history, resampled onto evenly spaced frames.
//
// `times` and `states` come from a transient solve's `sample_times` and
// `samples`. Linear between samples, clamped outside them.
bool ResampleHistory(const std::vector<double> &times,
                     const std::vector<std::vector<double>> &states,
                     const std::vector<double> &at, std::vector<std::vector<double>> *out,
                     std::string *error);

// The same for one scalar, which is what a probe's time history is
// (Part J.4).
double SampleAt(const std::vector<double> &times, const std::vector<double> &values, double at);

// Evenly spaced times covering a history, for driving a fixed frame rate.
std::vector<double> FrameTimes(double from, double to, int frames);

}  // namespace fem

#endif
