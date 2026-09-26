#ifndef MEP_VIEW_SCENE_H
#define MEP_VIEW_SCENE_H

#include "cad_topology.h"
#include "fem_modal.h"
#include "fem_render.h"

#include <array>
#include <string>
#include <vector>

// What a viewer shows (plans/CAD_FEM_PLAN.md Part L.2).
//
// A scene is a list of drawables, some labels, and the colour range they
// share. It is a *value*: building one touches no editor state, no GPU
// and no file, which is what lets the same scene be drawn into a pane,
// written to a PNG and filmed without three code paths agreeing by
// accident.
//
// A DRAWABLE IS A fem::RenderMesh, DELIBERATELY, rather than a type of
// this file's own. Part J.2 already produces exactly that for a result,
// and a parallel struct would mean a conversion on the commonest path in
// the whole viewer -- and a second place for the colour convention to
// drift. A CAD body, a raw array from a script and an annotation line
// are all turned *into* one instead, which is the cheap direction.
//
// WHAT A SCENE DOES NOT HAVE is a camera or a time. Those belong to the
// viewer looking at it, not to the thing being looked at: two panes can
// show one scene from two directions, and the script that builds a scene
// should not have to know which of them asked.
namespace view {

struct Label {
    cad::Vec3d at;
    std::string text;
    std::array<unsigned char, 4> color{235, 235, 240, 255};
};

struct Item {
    fem::RenderMesh mesh;
    // For the viewer's own listing, and for a script that wants to
    // replace one item rather than rebuild the scene.
    std::string name;
};

struct Scene {
    std::vector<Item> items;
    std::vector<Label> labels;

    // The field the colours mean, when they mean one. A scene of plain
    // geometry has none and draws no colour bar, which is the honest
    // thing: a legend on a picture that is not measuring anything is
    // worse than no legend.
    bool has_field = false;
    double field_min = 0.0;
    double field_max = 0.0;
    std::string units;
    // What the viewer is called. Unlike everything above it, this
    // survives Clear(): it is a title, set once beside the part it
    // names, not something derived from what is in the scene.
    std::string caption;

    bool Empty() const;
    int TriangleCount() const;
    int VertexCount() const;
    // The box everything occupies. False, and an untouched box, for an
    // empty scene -- so a caller can tell "nothing here" from "something
    // here of zero size".
    bool Bounds(cad::Vec3d *low, cad::Vec3d *high) const;
    void Clear();
    // Widens the field range to take in another item's. Used when a
    // scene holds two results and one colour bar has to serve both.
    void NoteField(double low, double high, const std::string &units_of_it);
};

// --- Builders ---------------------------------------------------------
//
// Each one appends to the scene rather than replacing it, because a
// scene is routinely more than one thing -- a deformed result next to
// the undeformed part it came from, with a probe marker on it.

struct PartOptions {
    std::array<unsigned char, 4> color{150, 170, 200, 255};
    // Zero means "from the body's own size", which is the only default
    // that is right for a part measured in microns and one measured in
    // metres.
    double chord_tolerance = 0.0;
    // The edges of the tessellation, as lines. Off by default: a
    // tessellated cylinder has a great many of them and none of them is
    // a real edge of the part.
    bool facets = false;
    std::string name;
};
bool AddPart(Scene *scene, const cad::Model &model, cad::EntityId body,
             const PartOptions &options, std::string *error);

struct ResultOptions {
    fem::RenderOptions render;
    fem::ScalarField field = fem::ScalarField::VonMises;
    double strength = 250e6;
    std::string name;
};
bool AddResult(Scene *scene, const fem::AnalysisModel &model,
               const std::vector<cad::Vec3d> &displacement, const fem::StressField &stress,
               const ResultOptions &options, std::string *error);

// One mode, at one point in its cycle.
//
// THE PHASE IS THE TIME, and the amplitude is not the eigenvector's.
// fem_animate.h explains why at length: a mode shape is mass-normalised,
// so its magnitude is a number in units of one over the square root of
// mass with no interpretation as a displacement at all. The amplitude is
// therefore taken from the geometry, exactly as a deformed-shape scale
// is, and `phase` runs 0 to 2*pi over one cycle.
bool AddMode(Scene *scene, const fem::AnalysisModel &model, const fem::ModalResult &modes,
             int mode, double phase, double amplitude_fraction, const fem::RenderOptions &render,
             const std::string &name, std::string *error);

// Geometry a script worked out for itself. Doubles in, because that is
// what Lua has; converted once, here.
struct RawMesh {
    std::vector<double> positions;       // 3 per vertex
    std::vector<int> indices;            // 3 per triangle, may be empty
    std::vector<int> line_indices;       // 2 per segment, may be empty
    std::vector<unsigned char> colors;   // 4 per vertex, or empty for one colour
    std::array<unsigned char, 4> color{200, 200, 210, 255};
    std::string name;
};
bool AddMesh(Scene *scene, const RawMesh &raw, std::string *error);

void AddLine(Scene *scene, const cad::Vec3d &a, const cad::Vec3d &b,
             const std::array<unsigned char, 4> &color, const std::string &name = "");
// A small three-axis cross, which reads as a point at any zoom where a
// single pixel would not.
void AddPoint(Scene *scene, const cad::Vec3d &at, double size,
              const std::array<unsigned char, 4> &color, const std::string &name = "");
void AddLabel(Scene *scene, const cad::Vec3d &at, const std::string &text,
              const std::array<unsigned char, 4> &color);

}  // namespace view

#endif
