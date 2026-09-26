#ifndef MEP_FEM_MOVIE_H
#define MEP_FEM_MOVIE_H

#include "fem_render.h"

#include <array>
#include <string>
#include <vector>

// Results as a playable film (plans/CAD_FEM_PLAN.md Part J.3, the half
// J.3 deferred).
//
// Part J.3 produces the frames of an animation -- one displacement field
// per frame, at known times -- and deliberately stops there, on the
// grounds that the camera, the keyframe track and the video encoder
// already exist for the 3D modeller and a second copy of any of them
// would be a second copy to keep working. That is right about the
// encoder and wrong about the renderer, and this file is the difference.
//
// WHY NOT Model3DRenderAnimationToVideoFile. Two reasons, and the second
// is the one that settles it.
//
//   * Its animation is a track of *transforms*. A mode shape is not a
//     transform of anything: every node moves differently, which is what
//     makes it a mode shape rather than a rigid motion. Driving it
//     through an object keyframe track would mean one object per frame
//     and a visibility track to hide the other thirty-five, which is not
//     a feature that exists and would be a strange one to add.
//   * It renders through OpenGL, from a live editor buffer. A benchmark
//     that produces its own film has to run in a test and on a machine
//     with no display, and a headless GL context is a thing that either
//     works or wastes a day. The rasteriser below is about two hundred
//     lines, has no dependencies at all, and renders the same picture on
//     every machine -- which also means a test can assert what is in it.
//
// The encoder is shared, though: these frames go through the same
// jpeg::Encode and mov::WriteMovFile the modeller's export uses, and the
// file that comes out opens in mep's own video pane and in anything else
// that plays a QuickTime motion-JPEG.
namespace fem {

// Where the camera sits, in the model's own frame. Spherical rather than
// a point and a target because the interesting thing about a result view
// is the *direction* it is seen from; the distance that frames the model
// is arithmetic, and asking the caller for it is asking them to measure
// the model first.
struct MovieCamera {
    double yaw = 0.9;      // radians, anticlockwise about +z from +x
    double pitch = 0.5;    // radians above the horizon
    double distance = 0.0; // 0 fits the model's bounding sphere
    double fov_y = 0.7;    // radians, vertical
    cad::Vec3d up{0, 0, 1};
    // Turns a full circle over the length of the film. A fixed camera
    // shows how much a thing moves; a turning one shows the *shape* of
    // the movement, which one viewpoint of a symmetric part routinely
    // hides -- two mode shapes that differ only in which diameter they
    // bend about look identical from directly above.
    bool orbit = false;
};

struct MovieOptions {
    int width = 720;
    int height = 540;
    int fps = 20;
    int quality = 88;
    // Rendered this many times larger and boxed back down. Three lines of
    // code against the single worst artefact of a software rasteriser,
    // which is that a mesh edge against a dark background crawls.
    int supersample = 2;

    MovieCamera camera;
    RenderOptions render;

    // Edges of the drawn triangles, dark. A contour plot with no mesh on
    // it cannot be read for convergence: the whole question a benchmark
    // asks is whether the mesh is fine enough, and a picture that hides
    // the mesh cannot be used to answer it.
    bool mesh_lines = true;
    // The undeformed shape, as faint lines behind the deformed one.
    // Without it a mode animation is a shape wobbling in empty space with
    // nothing to be displaced *from*.
    bool ghost_undeformed = true;

    // A colour bar with the range on it, and a line of text along the
    // top. A picture of a stress field with no scale on it is decoration.
    bool legend = true;
    std::string caption;
    // What the colour bar is measuring, written under it: "von Mises,
    // Pa", "mm", and so on.
    std::string units;

    std::array<unsigned char, 3> background{18, 20, 24};
};

struct MovieReport {
    int frames = 0;
    int width = 0;
    int height = 0;
    double seconds = 0.0;
    // The colour range actually used, which is what a caller needs to
    // write a caption that is true.
    double field_min = 0.0;
    double field_max = 0.0;
    double displacement_scale = 1.0;
    long long bytes = 0;
};

// The camera resolved against a model: the centre it looks at and the
// radius it has to fit.
//
// COMPUTED ONCE, FROM THE UNDEFORMED SHAPE, AND HELD FIXED FOR EVERY
// FRAME. Fitting the camera to each frame in turn would zoom gently in
// and out over the cycle, because the deformed model really is a
// different size in each one -- and the resulting picture is one in which
// nothing appears to move, since the thing that moved and the frame
// around it moved together. This is the single easiest way to produce a
// mode animation that is subtly, unaccountably wrong, so the fit is
// deliberately not per-frame.
struct MovieView {
    cad::Vec3d centre;
    double radius = 1.0;
    // The bounding box as well as the sphere around it, because fitting
    // a long thin part to its bounding *sphere* wastes most of the
    // frame: a ten-to-one beam seen broadside fills a tenth of the
    // height it was given room for. The corners are projected and the
    // distance tightened onto them.
    cad::Vec3d low;
    cad::Vec3d high;
};
MovieView FitView(const AnalysisModel &model);

// A caption pinned to a point in the world rather than to the frame:
// drawn where that point projects, and hidden when it is behind the
// camera. Part L.2's annotation labels arrive this way.
struct Annotation {
    cad::Vec3d at;
    std::string text;
    std::array<unsigned char, 3> color{235, 235, 240};
};

// Several meshes at once, which is what a scene is. `RenderFrame` below
// is this with one mesh and an optional ghost, kept because that is what
// a film of a single result wants and it reads better at the call site.
bool RenderScene(const std::vector<const RenderMesh *> &meshes,
                 const std::vector<Annotation> &labels, const MovieView &view,
                 const MovieOptions &options, double turn, double frame_min, double frame_max,
                 std::vector<unsigned char> *out, std::string *error);

// One frame, as RGBA8, top row first, `width * height * 4` bytes. `turn`
// is added to the camera's yaw.
//
// Exposed rather than kept private because a still is worth having on its
// own -- a report wants a picture, not a film -- and because a test can
// say something precise about one image and almost nothing about a video.
bool RenderFrame(const RenderMesh &mesh, const RenderMesh *ghost, const MovieView &view,
                 const MovieOptions &options, double turn, double frame_min, double frame_max,
                 std::vector<unsigned char> *out, std::string *error);

// A film. One frame per entry of `displacement`; `values` holds either
// one field per frame or exactly one field reused for all of them, which
// is what a mode animation wants -- a mode shape has no stress field of
// its own, only a shape.
//
// The colour range is taken over *every* frame and then held fixed, for
// the same reason the camera is: a per-frame range repaints the picture
// each frame and turns a growing stress into a constant one.
bool WriteMovie(const AnalysisModel &model,
                const std::vector<std::vector<cad::Vec3d>> &displacement,
                const std::vector<std::vector<double>> &values, const MovieOptions &options,
                const std::string &path, MovieReport *out, std::string *error);

// The same picture as a single PNG, for a report or a plan.
bool WriteStill(const AnalysisModel &model, const std::vector<cad::Vec3d> &displacement,
                const std::vector<double> &values, const MovieOptions &options,
                const std::string &path, MovieReport *out, std::string *error);

}  // namespace fem

#endif
