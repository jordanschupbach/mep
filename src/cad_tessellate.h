#ifndef MEP_CAD_TESSELLATE_H
#define MEP_CAD_TESSELLATE_H

#include "cad_math.h"
#include "cad_topology.h"

#include <string>
#include <vector>

// Tessellation (plans/CAD_FEM_PLAN.md Part B.5): turning a B-rep into
// triangles for display and export.
//
// This is the one-way bridge out of the CAD kernel. Everything upstream
// of it is exact; everything downstream is an approximation to a chosen
// tolerance, and nothing ever comes back. The mesh produced here is for
// *looking at* and for exporting to formats that only carry triangles --
// it is emphatically not the finite-element mesh, which Part G builds by
// a completely different route (metric-driven Delaunay refinement in
// parameter space, curved second-order elements projected back onto the
// exact surface). Confusing the two would give a structural analysis the
// accuracy of a viewport.
//
// The property that makes this non-trivial is watertightness. Two faces
// meeting along an edge must produce exactly the same points along it, or
// the mesh has cracks -- visible as light leaks in a render, and fatal to
// any volume computation. That is achieved by tessellating each *edge*
// once, shared, and having each face take its boundary points from there
// rather than from its own surface. The parameter-space coordinates are
// per-face (they must be -- each face sees the edge in its own
// parameters), but the 3D positions are one set of numbers used twice.
namespace cad {

struct TessellationOptions {
    // Maximum distance between the mesh and the true surface.
    double chord_tolerance = 1e-2;
    // Maximum turn between adjacent samples along an edge, in radians.
    double angle_tolerance = 0.35;
    // Upper bound on refinement, so a pathological face cannot run away.
    int max_triangles_per_face = 20000;
    Tolerance tolerance;
};

// A triangle mesh. Deliberately its own type rather than model3d_doc.h's
// MeshData: that type belongs to the polygonal modeler, and having the
// CAD kernel depend on it would couple two subsystems the plan
// specifically keeps apart. Converting is a copy of three arrays at the
// call site.
struct TessellationMesh {
    std::vector<Vec3d> positions;
    std::vector<Vec3d> normals;
    // 3 per triangle, indices into positions.
    std::vector<int> indices;
    // Which face each triangle came from, so a viewer can colour or pick
    // by face and a diagnostic can say where a bad triangle came from.
    std::vector<EntityId> triangle_face;

    int VertexCount() const { return static_cast<int>(positions.size()); }
    int TriangleCount() const { return static_cast<int>(indices.size() / 3); }

    // Signed volume of the mesh, by the divergence theorem over its
    // triangles. For a correctly oriented closed mesh this is positive
    // and converges to the solid's true volume -- which makes it the
    // single strongest check available on a tessellation, since it is
    // wrong if the mesh has a crack, if any triangle is wound backwards,
    // or if the geometry was approximated too coarsely.
    double SignedVolume() const;
    // Every edge of the triangle mesh used exactly twice, in opposite
    // directions. True exactly when the mesh is closed and consistently
    // oriented; the direct test of watertightness.
    bool IsClosed() const;
};

// Tessellates one face. Boundary points come from `edge_samples`, which
// the caller fills by tessellating each edge once -- pass an empty map to
// have this tessellate the edges itself, which is correct but gives no
// sharing and therefore no watertightness across faces.
bool TessellateFace(const Model &model, EntityId face, const TessellationOptions &options,
                    TessellationMesh *out, std::string *error);

// Tessellates a whole body, sharing edge samples across faces so the
// result is watertight.
bool TessellateBody(const Model &model, EntityId body, const TessellationOptions &options,
                    TessellationMesh *out, std::string *error);

// Everything in the model.
bool TessellateModel(const Model &model, const TessellationOptions &options, TessellationMesh *out,
                     std::string *error);

}  // namespace cad

#endif
