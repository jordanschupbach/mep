#ifndef MEP_FEM_VIZ_H
#define MEP_FEM_VIZ_H

#include "fem_model.h"
#include "fem_solve.h"

#include <vector>

// Turning a solved result into something you can look at
// (plans/CAD_FEM_PLAN.md Part 0.6, and the seed of Part J.2).
//
// Deliberately produces plain float/index arrays rather than a MeshData or
// a gfx::Mesh: those types belong to the 3D modeler and the renderer, and
// the FEM layer has no business depending on either. The caller -- the
// editor's result pane, a smoke test, an export -- assembles these arrays
// into whatever it needs. The arrays are already in exactly the layout
// gfx::Mesh wants, so that assembly is a memcpy, not a conversion.
//
// This is also where Part 0.4's widened index type earns itself: the
// surface of a mesh of any interesting size has well over 65,536 vertices,
// and it is drawn *indexed* so that the per-vertex field interpolates
// smoothly across shared vertices. De-indexing (what every importer in
// this tree used to do to dodge the 16-bit cap) would have given every
// triangle its own three vertices and turned a smooth contour into a
// faceted one.
namespace fem {

// Which scalar to colour by.
enum class ResultField {
    VonMises,
    DisplacementMagnitude,
    // Signed, so tension and compression are distinguishable -- the thing
    // a von Mises plot structurally cannot show.
    MaxPrincipalStress,
};

enum class ColorMap {
    // Perceptually uniform, monotonic in lightness, and readable with any
    // common form of colour blindness. The default despite the
    // engineering convention being a blue-to-red rainbow, because a
    // rainbow map's uneven lightness invents boundaries that are not in
    // the data -- the eye reads the yellow-to-green transition as a step
    // change when the underlying field is perfectly smooth. Since the
    // whole point of a contour plot is to locate where a field changes
    // fastest, a map that lies about that is worse than merely ugly.
    Viridis,
    // The blue-to-red map every commercial FEM package defaults to.
    // Offered because people read results by comparing them against what
    // they are used to, and being unable to reproduce the familiar
    // picture is a real cost.
    BlueToRed,
    // Greyscale, for printing and for figures that carry their own colour
    // elsewhere.
    Greyscale,
};

struct ResultMeshOptions {
    // Displacements are typically microns on a part measured in metres,
    // so a true-scale deformed shape is indistinguishable from the
    // original. The default exaggerates until the largest displacement is
    // a fixed fraction of the model's diagonal, which keeps the picture
    // legible at any scale without the caller having to know anything
    // about the units.
    bool auto_scale_displacement = true;
    double auto_scale_fraction = 0.08;  // of the bounding-box diagonal
    double displacement_scale = 1.0;    // used when auto_scale is false

    ResultField field = ResultField::VonMises;
    ColorMap color_map = ColorMap::Viridis;

    // Colour range. Auto uses the field's own min and max over the
    // surface; a fixed range is what makes two load cases comparable by
    // eye, which auto-ranging actively prevents.
    bool auto_range = true;
    double range_min = 0.0;
    double range_max = 0.0;

    // Draw the undeformed shape instead (all displacements zero) while
    // still colouring by the field.
    bool undeformed = false;
};

struct ResultMesh {
    std::vector<float> positions;       // 3 floats per vertex, deformed
    std::vector<float> normals;         // 3 floats per vertex
    std::vector<unsigned char> colors;  // 4 bytes per vertex, RGBA
    std::vector<unsigned int> indices;  // 3 per triangle
    // The range actually used for the colour mapping, so a caller can
    // draw a legend that means something.
    double field_min = 0.0;
    double field_max = 0.0;
    double displacement_scale_used = 1.0;
    int surface_vertex_count = 0;
    int surface_triangle_count = 0;
};

// Extracts the outer surface of the model's hex mesh -- the element faces
// belonging to exactly one element -- and builds a drawable, coloured,
// indexed mesh from it. Interior faces are dropped: they are invisible,
// and for a solid mesh they outnumber the surface faces by a wide margin.
ResultMesh BuildResultMesh(const Model &model, const Result &result, const ResultMeshOptions &options = {});

// The colour map on its own, for legends and for anything else that needs
// to agree with the mesh's colours exactly. `t` is clamped to [0,1].
void MapColor(ColorMap map, double t, unsigned char out_rgba[4]);

}  // namespace fem

#endif
