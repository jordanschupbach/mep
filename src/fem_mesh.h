#ifndef MEP_FEM_MESH_H
#define MEP_FEM_MESH_H

#include "cad_math.h"
#include "cad_topology.h"
#include "fem_model.h"

#include <array>
#include <map>
#include <string>
#include <vector>

// Meshing: the CAD to FEM bridge (plans/CAD_FEM_PLAN.md Part G).
//
// THIS PART IS WHY THE TWO HALVES ARE IN ONE PROGRAM. A mesher that
// reads a tessellation is a mesher that has already lost: it can only
// place nodes on triangles somebody else chose, so a fillet gets however
// many elements across it that the *viewer's* chord tolerance happened to
// produce, and a mid-side node on a curved boundary can only be put on a
// chord rather than on the surface. Meshing from the B-rep means the
// sizing field can ask a face what its curvature is, the surface mesher
// can work in the face's own parameters, and Part G.5 can project a
// mid-edge node onto the exact surface. None of those is available from a
// mesh file, and together they are the whole argument for the coupling.
//
// The order below is the order the plan gives and the order the pieces
// depend on each other: a size field, then a mesh of each face, then the
// volume between them, then quality, then curvature.
namespace fem {

// --- G.1: the sizing field ---------------------------------------------

struct SizingOptions {
    // The size to aim for away from any feature. Zero means "work it out
    // from the body", which is what a caller who has not thought about it
    // wants and is a twentieth of the body's diagonal.
    double target = 0.0;
    // Hard bounds. Zero means derived: a hundredth and twice the target.
    double min_size = 0.0;
    double max_size = 0.0;
    // How much a curved face may turn across one element, in radians.
    // This is what puts elements across a fillet: a face of radius r gets
    // a size of about r * angle there, whatever the target says.
    double curvature_angle = 0.35;
    // The largest ratio allowed between the sizes at two neighbouring
    // points. Without it a field seeded from a small feature jumps
    // straight back to the target and the elements next to the feature
    // are badly graded.
    double growth = 1.5;
    // Look for thin walls, and size across them so that a thin part gets
    // at least a couple of elements through its thickness rather than one
    // badly shaped one.
    bool thin_wall = true;
    int thin_wall_elements = 2;
    // How finely the octree may divide. A guard rather than a tuning
    // knob: eight levels is a million cells.
    int max_depth = 8;
    // LOCAL REFINEMENTS, APPLIED AFTER THE FIELD IS BUILT AND BEFORE IT
    // IS SMOOTHED. This is how an error estimator's verdict reaches the
    // mesher (Part J.5) without the mesher knowing what an error
    // estimator is, and it is the same door Part H.2's "mesh this named
    // face at 0.5" goes through. A list rather than a callback because a
    // meshing run has to be reproducible from its options alone.
    struct Refinement {
        cad::Vec3d centre;
        double radius = 0.0;
        double size = 0.0;
    };
    std::vector<Refinement> refinements;
};

// A background octree carrying a target element size.
//
// Deliberately a *background* structure rather than something attached to
// the geometry: the volume mesher asks for a size at points that are not
// on any face, and a field that could only answer on the boundary would
// have nothing to say about the interior.
class SizingField {
public:
    bool Build(const cad::Model &model, const std::vector<cad::EntityId> &bodies,
               const SizingOptions &options, std::string *error);

    // The target size at a point. Defined everywhere, including outside
    // the body, because the volume mesher asks about circumcentres that
    // may briefly wander out.
    double At(const cad::Vec3d &point) const;

    // A local override: everything within `radius` of `centre` is sized
    // at `size` or finer. This is how Part H.2's "mesh this named face at
    // 0.5" will reach the mesher without the mesher knowing what a name
    // is.
    void Refine(const cad::Vec3d &centre, double radius, double size);
    // Re-applies the growth limit. Called by Build; call it again after
    // the last Refine.
    void Smooth();

    const cad::Box3d &Extent() const { return extent_; }
    double TargetSize() const { return target_; }
    double MinSize() const { return min_size_; }
    int CellCount() const { return static_cast<int>(cells_.size()); }

private:
    struct Cell {
        cad::Box3d box;
        double size = 0.0;
        int children[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        bool IsLeaf() const { return children[0] < 0; }
    };
    int Insert(const cad::Vec3d &point, double size, int depth_limit);
    int LeafAt(const cad::Vec3d &point) const;
    // Every leaf whose box overlaps `region`. Used to enumerate a cell's
    // face neighbours exactly: a big cell may touch any number of small
    // ones, and probing its face at a few points finds some of them and
    // misses the rest -- which leaves the ones it missed ungraded.
    void LeavesOverlapping(int cell, const cad::Box3d &region, std::vector<int> *out) const;

    std::vector<Cell> cells_;
    cad::Box3d extent_;
    double target_ = 1.0;
    double min_size_ = 0.01;
    double max_size_ = 2.0;
    double growth_ = 1.5;
    int max_depth_ = 8;
};

// --- G.2: the surface mesh ----------------------------------------------

// One triangle of the surface mesh, tagged with the CAD face it came
// from. The tag is not decoration: Part H.2 attaches loads and restraints
// to faces, and a mesh that had forgotten which face each triangle
// belonged to could not carry them.
struct SurfaceTriangle {
    int a = 0, b = 0, c = 0;
    cad::EntityId face = cad::kNoEntity;
};

struct SurfaceMesh {
    std::vector<cad::Vec3d> nodes;
    // Where each node came from, for Part G.5's projection: the face and
    // its (u, v) for an interior node, the edge and its parameter for one
    // on an edge, and kNoEntity for a vertex node.
    struct Provenance {
        cad::EntityId face = cad::kNoEntity;
        cad::EntityId edge = cad::kNoEntity;
        cad::EntityId vertex = cad::kNoEntity;
        double u = 0.0, v = 0.0;  // face parameters, or t in u for an edge
    };
    std::vector<Provenance> provenance;
    std::vector<SurfaceTriangle> triangles;

    int NodeCount() const { return static_cast<int>(nodes.size()); }
    int TriangleCount() const { return static_cast<int>(triangles.size()); }
    // Every directed edge used once and its reverse once -- the same test
    // Part B.5's tessellation has to pass, and for the same reason.
    bool IsClosed() const;
    double SignedVolume() const;
};

struct SurfaceMeshOptions {
    SizingOptions sizing;
    // The smallest angle a surface triangle may have, in degrees.
    // Ruppert's refinement terminates for anything up to about 20.7
    // degrees; past that it can cycle, so the cap is real rather than
    // cautious.
    double min_angle = 25.0;
    // Refinement budget per face, so a pathological face cannot run away.
    int max_points_per_face = 20000;
    cad::Tolerance tolerance;
};

struct MeshReport {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    int nodes = 0;
    int triangles = 0;
    int tetrahedra = 0;
    // Quality, as Part G.4 reports it.
    double min_dihedral = 0.0;   // degrees
    double max_dihedral = 0.0;
    double worst_aspect = 0.0;
    double min_scaled_jacobian = 0.0;
    int slivers = 0;
    // Nodes the volume mesher added beyond the surface mesh's own. It
    // adds none today -- the boundary it produces is a re-triangulation
    // of the surface mesh's nodes rather than a recovery of its
    // triangles -- and the count is kept because refinement (Part G.4)
    // is where extra nodes will come from.
    int steiner_points = 0;
    // Re-triangulations made to get rid of bad elements (Part G.4).
    int flips = 0;
    // How many rounds of flip-and-smooth the quality repair needed. One
    // is the common case; more means the mesh was graded or thin enough
    // that the two had to take turns.
    int repair_rounds = 0;
    // Surface nodes that turned out to be the same point and were merged.
    // Reported rather than silent: a mesh that needed welding came from
    // geometry whose edges very nearly coincide, and that is worth
    // knowing even though the mesh is now sound.
    int welded_nodes = 0;
    // The smallest angle of any boundary triangle, in degrees. Reported
    // beside the dihedral because when a volume mesh is bad the first
    // question is always whether the surface it was built on was bad
    // first, and a tetrahedron cannot be better than the triangle it
    // stands on.
    double min_surface_angle = 0.0;
    // Raising the order (Part G.5): how many mid-side nodes there are,
    // how many of them the exact geometry actually moved off the straight
    // position, and how many had to be walked back towards it to keep
    // their elements from turning inside out.
    int mid_nodes = 0;
    int mid_nodes_curved = 0;
    int mid_nodes_backed_off = 0;
};

// Meshes every face of the bodies, sharing the nodes on edges between
// them so that adjacent faces match node for node -- which is what makes
// the result a closed surface rather than a pile of unrelated patches,
// and is the single most common thing a naive per-face mesher gets wrong.
bool MeshSurface(const cad::Model &model, const std::vector<cad::EntityId> &bodies,
                 const SurfaceMeshOptions &options, const SizingField &sizing, SurfaceMesh *out,
                 MeshReport *report);

// --- G.3 and G.4: the volume mesh ---------------------------------------

struct Tetrahedron {
    int a = 0, b = 0, c = 0, d = 0;
};

// THE EIGHT-NODE ORDERING, ONCE, EXPLICITLY. The one every mesh format
// and every element library uses: nodes 0-3 go round one face, nodes 4-7
// round the opposite face in the same rotational sense, and node 4 sits
// across from node 0. Wound so that 0-1-2-3 seen from *outside* the
// element turns clockwise -- equivalently, the 0-1-2-3 face's outward
// normal points away from 4-5-6-7 -- which is what makes the Jacobian
// positive. Getting this wrong gives a mesh that looks right, measures
// right and solves to nonsense.
struct Hexahedron {
    std::array<int, 8> n{};
};

// A quadrilateral of a hex mesh's boundary, tagged with its CAD face for
// the same reason SurfaceTriangle is: Part H.2 attaches loads to faces,
// not to nodes.
struct SurfaceQuad {
    int a = 0, b = 0, c = 0, d = 0;
    cad::EntityId face = cad::kNoEntity;
};

struct VolumeMesh {
    std::vector<cad::Vec3d> nodes;
    std::vector<SurfaceMesh::Provenance> provenance;
    std::vector<Tetrahedron> tets;
    // For a second-order mesh: the ten-node connectivity, with the
    // mid-edge nodes appended to `nodes`. Empty for a first-order mesh.
    std::vector<std::array<int, 10>> tets10;
    // Surface triangles of the finished volume mesh, still tagged with
    // their CAD face so that Part H.2 can attach conditions to them.
    std::vector<SurfaceTriangle> boundary;
    // Part G.6's hexahedra, and the quadrilaterals of their boundary.
    // Empty for a tetrahedral mesh. A mesh carries one or the other; the
    // sweeper writes these and the tetrahedral mesher writes the others,
    // and nothing yet produces both at once.
    std::vector<Hexahedron> hexes;
    std::vector<SurfaceQuad> boundary_quads;
    // The same triangles as six-node ones, in the order `boundary` is in:
    // the three corners, then the middles of edges ab, bc and ca. Empty
    // for a first-order mesh. A pressure on a curved face is applied to
    // these, so that it acts on the curve and not on its chords.
    std::vector<std::array<int, 6>> boundary6;

    int NodeCount() const { return static_cast<int>(nodes.size()); }
    int TetCount() const { return static_cast<int>(tets.size()); }
    int HexCount() const { return static_cast<int>(hexes.size()); }
};

struct VolumeMeshOptions {
    SurfaceMeshOptions surface;
    // Insert interior points to the sizing field. Off gives a mesh whose
    // only nodes are on the boundary, which is what a thin part wants and
    // what a blocky one very much does not.
    bool interior_points = true;
    // Quality improvement (Part G.4).
    bool smooth = true;
    int smoothing_passes = 3;
    bool remove_slivers = true;
    // A mesh whose worst element is below this is reported rather than
    // returned as if it were fine. In degrees.
    double min_acceptable_dihedral = 5.0;
};

// The whole pipeline: size, surface, volume, quality.
bool MeshBody(const cad::Model &model, const std::vector<cad::EntityId> &bodies,
              const VolumeMeshOptions &options, VolumeMesh *out, MeshReport *report);

// --- G.5: curved second-order elements ----------------------------------

// Adds mid-edge nodes and projects them onto the exact B-rep surface
// where the edge lies on one.
//
// THIS IS THE CONCRETE PAYOFF of keeping exact geometry, and the
// validity check is what makes it usable rather than merely clever: a
// mid-node projected onto a tight fillet can invert the element it
// belongs to, and an inverted element is worse than a straight one. Where
// that happens the node is backed off towards the straight position until
// the element is valid again, and the number of times that was needed is
// reported rather than hidden.
bool MakeSecondOrder(const cad::Model &model, VolumeMesh *mesh, MeshReport *report);

// The volume a second-order mesh actually encloses, integrated over its
// curved elements rather than read off their corners.
//
// This is the number that says whether curving the mid-side nodes did
// anything, and it is why it is worth exposing: a straight mesh of a
// sphere under-measures it by the sum of all the little caps between the
// chords and the surface, and a curved one should give most of that
// back. Zero for a first-order mesh, which has no curved elements to
// integrate over.
double CurvedVolume(const VolumeMesh &mesh);

// Every second-order element's quadratic map is the right way out
// everywhere inside it, not merely at its corners -- which is a stronger
// question than the first-order one and cannot be answered by looking at
// the corner nodes, because a quadratic map folds in the middle. Called
// by CheckMesh, and worth calling directly after anything that moves a
// mid-side node. True for a first-order mesh, which has none.
bool CheckCurvature(const VolumeMesh &mesh, std::string *error);

// --- G.6: structured and hex meshing -------------------------------------

struct SweepMeshOptions {
    SizingOptions sizing;
    // Layers through the sweep direction. Zero means work it out from the
    // sizing field.
    int layers = 0;
    SurfaceMeshOptions surface;
    // Smoothing passes over the quadrilateral mesh of the source face,
    // before it is swept. Smoothing the profile once is worth far more
    // than smoothing the solid afterwards, because every layer inherits
    // it.
    int smoothing_passes = 6;
};

// Sweeps a prismatic body into hexahedra: mesh the source face with
// quadrilaterals, then extrude it along the sweep direction.
//
// Restricted to what can be checked: a body of one planar source face,
// one planar target face parallel to it, and side faces joining them.
// That is every extruded profile Part E.2 produces, which is the case
// worth having -- Hex8 behaves far better than Tet4 under bending, and a
// prismatic part is exactly the sort that gets bent.
bool MeshSweep(const cad::Model &model, cad::EntityId body, const SweepMeshOptions &options,
               VolumeMesh *out, MeshReport *report);

// --- G.7: verification ---------------------------------------------------

struct MeshQuality {
    double min_dihedral = 0.0;
    double max_dihedral = 0.0;
    double worst_aspect = 0.0;
    double min_scaled_jacobian = 0.0;
    double min_volume = 0.0;
    int inverted = 0;
    int slivers = 0;
    // A histogram of the minimum dihedral angle, in ten-degree bins, for
    // comparing one mesher's output against another's.
    std::array<int, 18> dihedral_histogram{};
};

// The scaled Jacobian of a hexahedron: the smallest, over its eight
// corners, of the triple product of the three edges there divided by
// their lengths. One for a cube, zero for a flattened element, negative
// for one turned inside out.
//
// This rather than a dihedral angle because it is what a hex is judged by
// everywhere: a hex has twenty-four face angles and no single worst one
// that means anything, while the Jacobian is exactly the quantity a
// solver's element routines divide by.
double HexScaledJacobian(const std::vector<cad::Vec3d> &nodes, const Hexahedron &hex);
double HexVolume(const std::vector<cad::Vec3d> &nodes, const Hexahedron &hex);

// Every hexahedron the right way out, every node it names existing, and
// the quadrilateral boundary closed -- each face belonging to one element
// or two, never three, and each edge of the boundary to exactly two
// boundary faces. Called by CheckMesh. True for a mesh with no hexahedra.
bool CheckHexes(const VolumeMesh &mesh, std::string *error);

// The Delaunay tetrahedralisation of a point set, exposed on its own
// because it is the piece most worth testing without a body wrapped
// round it: a failure here and a failure in the meshing that uses it
// look identical from the outside and have nothing to do with each
// other.
void TetrahedralisePoints(const std::vector<cad::Vec3d> &points, std::vector<Tetrahedron> *out);

MeshQuality MeasureQuality(const VolumeMesh &mesh);
// Every element positive, the boundary closed, and the orientation
// consistent. Reports what is wrong rather than only that something is.
bool CheckMesh(const VolumeMesh &mesh, std::string *error);

// The mesh as an analysis model. Only the element types fem_model.h knows
// survive; the rest is reported rather than silently dropped.
bool ToAnalysisModel(const VolumeMesh &mesh, const Material &material, Model *out, std::string *error);

}  // namespace fem

#endif
