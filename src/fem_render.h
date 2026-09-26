#ifndef MEP_FEM_RENDER_H
#define MEP_FEM_RENDER_H

#include "fem_result.h"
#include "fem_viz.h"

#include <array>
#include <string>
#include <vector>

// Result visualisation (plans/CAD_FEM_PLAN.md Part J.2).
//
// ONE DECOMPOSITION, AND EVERYTHING IS BUILT ON IT. Every element the
// library has is split into linear tetrahedra once, and then:
//
//   * the drawable surface is the boundary of that tetrahedralisation --
//     the faces belonging to exactly one tet -- so no per-element face
//     table is needed and no element type can be forgotten;
//   * an isosurface is marching tetrahedra over it;
//   * and a cut plane is *the same function*, because a plane section is
//     the isosurface of the signed distance to the plane. Writing them
//     as two routines would be writing the same polygon-through-a-tet
//     twice, and the second copy is where the two would disagree.
//
// Marching tetrahedra rather than marching cubes, and not only because
// the mesh is already tetrahedra: a tetrahedron has no ambiguous case.
// Marching cubes has fifteen, several of which admit two topologies, and
// choosing them inconsistently between neighbouring cells leaves holes in
// a surface that is supposed to be closed. A tetrahedron's four vertices
// admit exactly two shapes -- one vertex cut off, or two from two -- and
// neither has a choice in it.
//
// Deliberately produces plain float and index arrays rather than a
// gfx::Mesh: those belong to the renderer, and the FEM layer has no
// business depending on it. The arrays are already in the layout
// gfx::Mesh wants, so the caller's assembly is a memcpy.
namespace fem {

// Every element as linear tetrahedra over the model's own nodes. No
// points are invented, which is what lets a field be carried through
// without interpolation.
//
// A quadratic tetrahedron splits into eight sub-tetrahedra on its
// mid-edge nodes, so its curved faces are drawn as four triangles each
// and its curvature survives. A Hex20's does not: splitting it properly
// needs face and body centres, which are not nodes, so it is decomposed
// on its eight corners and drawn with straight edges. That is a real
// limitation and it is in the plan rather than hidden here.
struct TetView {
    std::vector<std::array<int, 4>> tets;
    std::vector<int> element_of_tet;
};

bool Decompose(const AnalysisModel &model, TetView *out, std::string *error);

// The boundary triangles of a decomposition, wound so that the normal
// points out of the solid.
bool BoundaryTriangles(const AnalysisModel &model, const TetView &view,
                       std::vector<std::array<int, 3>> *out, std::string *error);

struct RenderOptions {
    // Displacements are typically microns on a part measured in metres,
    // so a true-scale deformed shape is indistinguishable from the
    // original. The default exaggerates until the largest displacement is
    // a fixed fraction of the model's diagonal, which keeps the picture
    // legible at any scale without the caller knowing anything about the
    // units.
    bool auto_scale = true;
    double auto_scale_fraction = 0.08;
    double displacement_scale = 1.0;
    bool undeformed = false;

    ColorMap color_map = ColorMap::Viridis;
    // Auto uses the field's own range over what is drawn. A fixed range is
    // what makes two load cases comparable by eye, which auto-ranging
    // actively prevents -- so it is offered rather than assumed.
    bool auto_range = true;
    double range_min = 0.0;
    double range_max = 0.0;

    // Draw only the part on the positive side of this plane. A cut-away,
    // as distinct from a section: the section is the flat face, this is
    // the solid behind it.
    bool clip = false;
    cad::Vec3d clip_point;
    cad::Vec3d clip_normal{1, 0, 0};
};

struct RenderMesh {
    std::vector<float> positions;       // 3 per vertex
    std::vector<float> normals;         // 3 per vertex
    std::vector<unsigned char> colors;  // 4 per vertex, RGBA
    std::vector<unsigned int> indices;  // 3 per triangle
    // Line segments, for vector plots and wireframes: 2 indices each,
    // into the same vertex arrays.
    std::vector<unsigned int> line_indices;

    double field_min = 0.0;
    double field_max = 0.0;
    double scale_used = 1.0;
    int vertex_count = 0;
    int triangle_count = 0;

    // Sums of the triangles' areas and of their signed contributions to
    // the enclosed volume. Not decoration: a surface whose area does not
    // match the geometry, or whose enclosed volume does not match the
    // model's, is a surface with holes or with a face wound backwards,
    // and neither is visible in a picture until it is too late.
    double area = 0.0;
    double enclosed_volume = 0.0;
};

// The deformed outer surface, coloured by `values` (one per node, from
// `SampleField`).
bool RenderSurface(const AnalysisModel &model, const std::vector<cad::Vec3d> &displacement,
                   const std::vector<double> &values, const RenderOptions &options,
                   RenderMesh *out, std::string *error);

// The flat section where a plane passes through the solid, coloured by
// the same values. The normal is the plane's; the section is drawn from
// both sides.
bool RenderSection(const AnalysisModel &model, const std::vector<cad::Vec3d> &displacement,
                   const std::vector<double> &values, const cad::Vec3d &point,
                   const cad::Vec3d &normal, const RenderOptions &options, RenderMesh *out,
                   std::string *error);

// The surface where `values` equals `level`, coloured by a second field.
// Colouring by something other than the field being contoured is the
// whole point of an isosurface plot -- the shape of one field, painted
// with another -- so `colour_by` may be empty to colour by `values`
// itself, which is then a constant and draws flat.
bool RenderIsosurface(const AnalysisModel &model, const std::vector<cad::Vec3d> &displacement,
                      const std::vector<double> &values, double level,
                      const std::vector<double> &colour_by, const RenderOptions &options,
                      RenderMesh *out, std::string *error);

struct VectorOptions {
    // Draw one arrow per this many nodes. A vector at every node of a
    // real mesh is a solid block of colour, not a picture.
    int stride = 1;
    bool auto_scale = true;
    double auto_scale_fraction = 0.05;
    double scale = 1.0;
    double head_fraction = 0.3;
    ColorMap color_map = ColorMap::Viridis;
    bool auto_range = true;
    double range_min = 0.0;
    double range_max = 0.0;
};

// Arrow glyphs, as line segments, coloured by magnitude. Used for
// displacement and for reaction forces, which is the plot that shows at a
// glance whether a model is held the way it was meant to be.
bool RenderVectors(const AnalysisModel &model, const std::vector<cad::Vec3d> &at,
                   const std::vector<cad::Vec3d> &vectors, const VectorOptions &options,
                   RenderMesh *out, std::string *error);

}  // namespace fem

#endif
