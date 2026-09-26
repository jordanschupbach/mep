#ifndef MEP_CAD_DRAWING_H
#define MEP_CAD_DRAWING_H

#include "cad_tessellate.h"
#include "cad_topology.h"

#include <string>
#include <vector>

// 2D drawing views from the B-rep (plans/CAD_FEM_PLAN.md Part F.4).
//
// A drawing view is the body's edges projected onto a plane, with the
// ones behind material drawn dashed or not at all. That second part is
// hidden-line removal, and it is the whole of the problem: projecting
// edges is a matrix multiply.
//
// HOW IT IS DONE HERE, and why. The exact method -- intersect every edge
// against every face's silhouette in the projection plane and keep the
// intervals that survive -- is the right one for a finished drawing and
// is a substantial piece of work in its own right. What is here instead
// is sampling: each edge is cut into short pieces and each piece's
// midpoint is tested against the tessellated body along the view
// direction. That is exact in the only sense that matters for the
// result -- a piece is hidden or it is not -- and inexact only in *where*
// the transition happens, to within one piece. Refining costs time and
// nothing else.
//
// The compromise is stated rather than hidden because it decides what
// this is good for: a view to look at, check and print, not a
// dimensioned drawing whose line ends are to be trusted to the
// micrometre.
namespace cad {

struct DrawingViewOptions {
    // The view direction, pointing *from* the viewer *into* the scene.
    Vec3d direction{0.0, -1.0, 0.0};
    // Which way is up on the sheet. Its component perpendicular to the
    // direction is used, so it need not be exactly square to it.
    Vec3d up{0.0, 0.0, 1.0};
    TessellationOptions tessellation;
    // Pieces per edge. More moves each visible/hidden transition closer
    // to where it belongs.
    int pieces_per_edge = 64;
    // How far in front of a surface a point must be to count as in front
    // of it, as a fraction of the model's size. Sized against the model
    // rather than absolute, because an edge lying *on* a face -- which
    // every edge of a solid does, on two of them -- must not shadow
    // itself.
    double relative_bias = 1e-5;
    // Include the silhouettes of curved faces, which are where a
    // cylinder's outline comes from and are not edges of the body at all.
    bool silhouettes = true;
};

struct DrawingSegment {
    Vec2d a;
    Vec2d b;
    bool hidden = false;
    // The edge it came from, or kNoEntity for a silhouette.
    EntityId edge = kNoEntity;
};

struct DrawingView {
    std::vector<DrawingSegment> segments;
    // The extent of everything drawn, for fitting it on a sheet.
    Interval x;
    Interval y;
    int visible = 0;
    int hidden = 0;
};

// Projects the bodies and works out what is hidden.
bool MakeDrawingView(const Model &model, const std::vector<EntityId> &bodies,
                     const DrawingViewOptions &options, DrawingView *out, std::string *error);

// The view as SVG, which is the format that can be looked at without
// anything else in the tree being involved. Hidden lines are dashed.
std::string DrawingViewToSvg(const DrawingView &view, double width_mm, double margin_mm = 10.0);

}  // namespace cad

#endif
