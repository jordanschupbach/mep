#ifndef MEP_CAD_TOPOLOGY_H
#define MEP_CAD_TOPOLOGY_H

#include "cad_curve.h"
#include "cad_math.h"
#include "cad_surface.h"

#include <memory>
#include <string>
#include <vector>

// The boundary representation (plans/CAD_FEM_PLAN.md Part B.1).
//
// A solid is described by its boundary: which surfaces bound it, which
// parts of those surfaces, and how they meet. That is what separates a
// CAD model from the mesh in model3d_doc.h -- a mesh knows where its
// triangles are, a B-rep knows that this face is a cylinder of radius 5
// meeting that plane along a circle.
//
// Structure, outermost first:
//
//   Body    one solid (or several disjoint lumps of one)
//    Shell   a closed, oriented set of faces; one outer, any number of
//            inner ones bounding voids
//     Face    a bounded region of one Surface
//      Loop    one closed boundary of that region; one outer, any number
//              of inner ones bounding holes
//       CoEdge  one use of an Edge by a Loop, carrying the direction that
//               use runs in and the edge's curve in the face's own
//               parameter space (the p-curve)
//        Edge    a bounded piece of one Curve3, between two Vertices
//         Vertex  a point
//
// The CoEdge is the piece that is easy to mistake for redundant and is
// not. An edge is shared by exactly two faces in a closed solid, and each
// face traverses it in the opposite direction and sees it in its own,
// different parameter space. Attaching direction and p-curve to the
// *use* rather than to the edge is what lets one edge serve both.
//
// Storage is flat arrays with ids that are indices into them -- not a
// pointer graph. That makes a Model copyable, serialisable and free of
// ownership questions, the same reasoning model3d_doc.h's Scene records
// for itself. Geometry is held by shared_ptr<const>: it is immutable once
// built and genuinely shared (the two faces along an edge reference one
// curve), so copying a Model copies the topology and shares the geometry.
namespace cad {

using EntityId = int;
inline constexpr EntityId kNoEntity = -1;

// Which way a use runs relative to the thing it uses. For a CoEdge,
// Forward means the loop traverses the edge in the direction its curve is
// parameterized. For a Face, Forward means the surface's own normal is
// the face's outward normal.
enum class Orientation { Forward, Reversed };

inline Orientation Flip(Orientation o) {
    return o == Orientation::Forward ? Orientation::Reversed : Orientation::Forward;
}

struct Vertex {
    EntityId id = kNoEntity;
    Vec3d point;
    // Per-entity, not global. A vertex where three imported faces almost
    // meet legitimately has a looser tolerance than one this kernel just
    // constructed; see Tolerance's own note in cad_math.h for why one
    // global epsilon cannot serve both.
    double tolerance = 1e-7;
};

struct Edge {
    EntityId id = kNoEntity;
    int curve = -1;  // index into Model::curves
    EntityId start_vertex = kNoEntity;
    EntityId end_vertex = kNoEntity;
    // The piece of the curve this edge is, which need not be the curve's
    // whole domain -- one circle can carry several arcs.
    double t_start = 0.0;
    double t_end = 1.0;
    double tolerance = 1e-7;
    // A closed edge (a full circle bounding a disc) has the same vertex at
    // both ends. Common enough that callers should test for it rather
    // than assume two distinct vertices.
    bool IsClosed() const { return start_vertex == end_vertex && start_vertex != kNoEntity; }
};

struct CoEdge {
    EntityId id = kNoEntity;
    EntityId edge = kNoEntity;
    Orientation orientation = Orientation::Forward;
    // Index into Model::pcurves, or -1 if none has been built yet. See
    // cad_pcurve.h -- a p-curve is a Curve3 confined to the z = 0 plane,
    // where x is the face's u and y its v.
    int pcurve = -1;
    EntityId loop = kNoEntity;
};

struct Loop {
    EntityId id = kNoEntity;
    // In traversal order: each coedge's end meets the next one's start.
    std::vector<EntityId> coedges;
    EntityId face = kNoEntity;
    // Exactly one loop per face is the outer boundary; the rest bound
    // holes in it.
    bool is_outer = true;
};

struct Face {
    EntityId id = kNoEntity;
    int surface = -1;  // index into Model::surfaces
    Orientation orientation = Orientation::Forward;
    std::vector<EntityId> loops;
    EntityId shell = kNoEntity;
    double tolerance = 1e-7;
    std::string name;  // optional, for diagnostics and Part B.4
};

struct Shell {
    EntityId id = kNoEntity;
    std::vector<EntityId> faces;
    // The outer shell bounds the material; an inner shell bounds a void
    // inside it (a bubble). A body has exactly one outer shell.
    bool is_outer = true;
    EntityId body = kNoEntity;
};

struct Body {
    EntityId id = kNoEntity;
    std::vector<EntityId> shells;
    std::string name;
};

// Topological counts, as the Euler-Poincare check and most diagnostics
// want them.
struct TopologyCounts {
    int vertices = 0;
    int edges = 0;
    int faces = 0;
    int loops = 0;
    int shells = 0;
    // Inner loops only -- the "holes" term in the Euler-Poincare formula.
    int holes = 0;
};

class Model {
public:
    Model() = default;

    // --- Geometry ------------------------------------------------------
    int AddCurve(std::shared_ptr<const Curve3> curve);
    int AddSurface(std::shared_ptr<const Surface> surface);
    // A p-curve: a Curve3 in the z = 0 plane, x = u, y = v.
    int AddPCurve(std::shared_ptr<const Curve3> pcurve);

    const Curve3 *CurveAt(int index) const;
    const Surface *SurfaceAt(int index) const;
    const Curve3 *PCurveAt(int index) const;
    int CurveCount() const { return static_cast<int>(curves_.size()); }
    int SurfaceCount() const { return static_cast<int>(surfaces_.size()); }
    int PCurveCount() const { return static_cast<int>(pcurves_.size()); }

    // --- Topology creation ---------------------------------------------
    EntityId AddVertex(const Vec3d &point, double tolerance = 1e-7);
    EntityId AddEdge(int curve, EntityId start_vertex, EntityId end_vertex, double t_start, double t_end,
                     double tolerance = 1e-7);
    EntityId AddCoEdge(EntityId edge, Orientation orientation, int pcurve = -1);
    EntityId AddLoop(const std::vector<EntityId> &coedges, bool is_outer = true);
    EntityId AddFace(int surface, Orientation orientation, const std::vector<EntityId> &loops,
                     const std::string &name = "", double tolerance = 1e-7);
    EntityId AddShell(const std::vector<EntityId> &faces, bool is_outer = true);
    EntityId AddBody(const std::vector<EntityId> &shells, const std::string &name = "");

    // --- Access ---------------------------------------------------------
    // Return nullptr for an unknown id rather than throwing or asserting:
    // a boolean operation routinely asks about entities that may have
    // been removed, and branching on null is cheaper than tracking that.
    const Vertex *GetVertex(EntityId id) const;
    const Edge *GetEdge(EntityId id) const;
    const CoEdge *GetCoEdge(EntityId id) const;
    const Loop *GetLoop(EntityId id) const;
    const Face *GetFace(EntityId id) const;
    const Shell *GetShell(EntityId id) const;
    const Body *GetBody(EntityId id) const;
    Vertex *GetVertex(EntityId id);
    Edge *GetEdge(EntityId id);
    CoEdge *GetCoEdge(EntityId id);
    Loop *GetLoop(EntityId id);
    Face *GetFace(EntityId id);
    Shell *GetShell(EntityId id);
    Body *GetBody(EntityId id);

    const std::vector<Vertex> &Vertices() const { return vertices_; }
    const std::vector<Edge> &Edges() const { return edges_; }
    const std::vector<CoEdge> &CoEdges() const { return coedges_; }
    const std::vector<Loop> &Loops() const { return loops_; }
    const std::vector<Face> &Faces() const { return faces_; }
    const std::vector<Shell> &Shells() const { return shells_; }
    const std::vector<Body> &Bodies() const { return bodies_; }

    // --- Derived queries -------------------------------------------------

    // Every coedge referencing this edge. In a closed manifold solid there
    // are exactly two; anything else is a defect the validity checker
    // reports (a free edge, or a non-manifold one where three or more
    // faces meet).
    std::vector<EntityId> CoEdgesOfEdge(EntityId edge) const;
    // The faces meeting along an edge, via its coedges.
    std::vector<EntityId> FacesOfEdge(EntityId edge) const;
    // Every edge used by a face, in no particular order, deduplicated.
    std::vector<EntityId> EdgesOfFace(EntityId face) const;
    // Every edge and vertex of a shell or body.
    std::vector<EntityId> EdgesOfShell(EntityId shell) const;
    std::vector<EntityId> VerticesOfShell(EntityId shell) const;

    TopologyCounts CountsOfShell(EntityId shell) const;
    TopologyCounts CountsOfBody(EntityId body) const;

    // The 3D point at a parameter along an edge, honouring the edge's own
    // parameter range. Threaded through everywhere, so it earns a name
    // rather than being open-coded at each site.
    Vec3d EdgePoint(EntityId edge, double t) const;
    Vec3d EdgeStartPoint(EntityId edge) const;
    Vec3d EdgeEndPoint(EntityId edge) const;
    // The same, but in the direction a *coedge* traverses: start and end
    // swap for a reversed coedge, which is the whole point of the type.
    Vec3d CoEdgeStartPoint(EntityId coedge) const;
    Vec3d CoEdgeEndPoint(EntityId coedge) const;
    EntityId CoEdgeStartVertex(EntityId coedge) const;
    EntityId CoEdgeEndVertex(EntityId coedge) const;

    // The face's outward normal at a surface parameter, which is the
    // surface normal flipped when the face is reversed. Confusing these
    // two is the most common orientation bug in a B-rep, so the face-level
    // one is the only one most code should call.
    Vec3d FaceNormal(EntityId face, double u, double v) const;

    Box3d Bounds() const;
    Box3d BoundsOfFace(EntityId face) const;

private:
    std::vector<Vertex> vertices_;
    std::vector<Edge> edges_;
    std::vector<CoEdge> coedges_;
    std::vector<Loop> loops_;
    std::vector<Face> faces_;
    std::vector<Shell> shells_;
    std::vector<Body> bodies_;
    std::vector<std::shared_ptr<const Curve3>> curves_;
    std::vector<std::shared_ptr<const Surface>> surfaces_;
    std::vector<std::shared_ptr<const Curve3>> pcurves_;
};

// ---------------------------------------------------------------------
// Primitive solids
//
// Needed here rather than in Part E because there is otherwise nothing to
// validate, tessellate or intersect: a topology layer with no way to
// build a body cannot be tested at all. These four cover the interesting
// cases deliberately -- the box has only ordinary faces, the cylinder has
// a seam, the sphere has a seam *and* two poles where the surface is
// degenerate, and the torus is genus 1 with two seams.
// ---------------------------------------------------------------------

// An axis-aligned box. Six planar faces, eight vertices, twelve edges.
bool MakeBox(const Vec3d &min_corner, const Vec3d &size, Model *model, EntityId *out_body);

// A capped cylinder about the z axis: a cylindrical face with a seam,
// plus two planar caps.
bool MakeCylinder(const Vec3d &base_center, const Vec3d &axis, double radius, double height, Model *model,
                  EntityId *out_body);

// A sphere: one face, one seam edge running pole to pole, two vertices at
// the poles where the surface's normal is undefined.
bool MakeSphere(const Vec3d &center, double radius, Model *model, EntityId *out_body);

// A torus: one face, two seam edges, one vertex. Genus 1, which is the
// case the Euler-Poincare check has to get right rather than assume away.
bool MakeTorus(const Vec3d &center, const Vec3d &axis, double major_radius, double minor_radius, Model *model,
               EntityId *out_body);

}  // namespace cad

#endif
