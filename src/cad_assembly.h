#ifndef MEP_CAD_ASSEMBLY_H
#define MEP_CAD_ASSEMBLY_H

#include "cad_feature.h"
#include "cad_math.h"
#include "cad_naming.h"
#include "cad_topology.h"

#include <memory>
#include <string>
#include <vector>

// Datums and assemblies (plans/CAD_FEM_PLAN.md Part E.6).
//
// A DATUM IS A PIECE OF GEOMETRY WITH NO MATERIAL: a point, an axis or a
// plane, used to position things against. It is the same three kinds
// whatever it was made from -- the axis of a hole, the plane a face lies
// in, the line where two planes meet -- and reducing every reference to
// one of those three is what stops the mate solver below from needing to
// know anything about B-reps.
//
// AN ASSEMBLY IS INSTANCES AND MATES. Each instance is a part placed
// somewhere; each mate is a condition between a datum on one instance and
// a datum on another. Solving is finding placements that satisfy the
// mates, which is a least-squares problem in exactly the way Part D.2's
// sketches are -- and it is solved by the same machinery: forward-mode
// dual numbers for exact Jacobians (cad_dual.h, extracted from D.2 for
// this), Levenberg-Marquardt to drive it, and the rank of the Jacobian to
// report how many degrees of freedom are left.
//
// WHAT IS DIFFERENT IN THREE DIMENSIONS is the rotation. A sketch point
// is two numbers and its derivative is trivial; a placement is six
// degrees of freedom of which three are angular, and how those three are
// written down decides whether the solver works. Euler angles lock;
// a rotation matrix is nine numbers with six constraints between them.
// This uses a quaternion: four numbers, one constraint, no singularity
// anywhere, and rotation by it is a polynomial -- so the dual arithmetic
// differentiates it exactly with no special cases. The one constraint,
// that it be a unit quaternion, is carried as an ordinary residual
// alongside the mates, which is also what makes the degree-of-freedom
// count come out right: seven parameters an instance, one of them spent
// on the gauge, leaves six.
namespace cad {

// --- Rotations ---------------------------------------------------------

struct Quatd {
    double w = 1.0, x = 0.0, y = 0.0, z = 0.0;

    static Quatd FromAxisAngle(const Vec3d &axis, double angle);
    // The rotation taking `from` to `to`, by the shortest path.
    static Quatd Between(const Vec3d &from, const Vec3d &to);
    Quatd Normalized() const;
    Quatd Conjugate() const { return Quatd{w, -x, -y, -z}; }
    Vec3d Rotate(const Vec3d &v) const;
    Quatd operator*(const Quatd &o) const;
    double Norm() const { return std::sqrt(w * w + x * x + y * y + z * z); }
};

struct Placement {
    Vec3d position;
    Quatd rotation;

    Vec3d Apply(const Vec3d &point) const { return position + rotation.Rotate(point); }
    Vec3d ApplyDirection(const Vec3d &direction) const { return rotation.Rotate(direction); }
    Mat4d ToMatrix() const;
};

// --- Datums ------------------------------------------------------------

enum class DatumKind { Point, Axis, Plane };

struct Datum {
    DatumKind kind = DatumKind::Plane;
    // A point on it. For a Point that is the whole story.
    Vec3d origin;
    // The axis's direction, or the plane's normal. Unit. Unused for a
    // Point, where it is left as whatever it was.
    Vec3d direction{0.0, 0.0, 1.0};
    std::string name;
};

// A planar face gives a Plane whose normal is the face's *outward* one;
// a cylindrical face gives its Axis.
bool DatumFromFace(const Model &model, EntityId face, Datum *out, std::string *error);
// A straight edge gives an Axis along it; a circular one gives the Axis
// through its centre, which is what a bolt hole's rim is usually meant
// to stand for.
bool DatumFromEdge(const Model &model, EntityId edge, Datum *out, std::string *error);
bool DatumFromVertex(const Model &model, EntityId vertex, Datum *out, std::string *error);

// A plane moved along its own normal.
bool OffsetDatumPlane(const Datum &plane, double distance, Datum *out, std::string *error);
bool DatumPlaneThroughPoints(const Vec3d &a, const Vec3d &b, const Vec3d &c, Datum *out,
                             std::string *error);
// The line where two planes meet.
bool DatumAxisFromPlanes(const Datum &a, const Datum &b, Datum *out, std::string *error);
// The point where an axis meets a plane.
bool DatumPointFromAxisAndPlane(const Datum &axis, const Datum &plane, Datum *out, std::string *error);

// The same datum seen from the assembly rather than from its part.
Datum PlaceDatum(const Datum &datum, const Placement &placement);

// --- Mates -------------------------------------------------------------

enum class MateKind {
    // Two planes flush and facing each other. Removes three degrees of
    // freedom: two angular and one along the shared normal.
    Coincident,
    // The same, held a distance apart along the first plane's normal.
    Offset,
    // Two axes collinear, either way round -- a pin in a hole. Removes
    // four: two angular and two across the axis.
    Concentric,
    // Two directions parallel, either way round. Removes two.
    Parallel,
    // A fixed angle between two directions. Removes one.
    Angle,
    // Two points together. Removes three.
    PointOnPoint,
    // Two points a fixed distance apart. Removes one.
    Distance,
};

struct MateEnd {
    int instance = -1;
    // In the part's own coordinates, so it survives the instance moving.
    Datum datum;

    // WHERE THE DATUM CAME FROM, so that it survives the part being
    // *rebuilt* as well -- which moving does not cover and is the harder
    // half. A mate that remembers only "the plane z = 1" is wrong the
    // moment the plate is thickened; one that remembers "the face this
    // names" is still right, and Part B.4 is what turns that name back
    // into a face. When the name cannot be honoured the mate is reported
    // rather than quietly left pointing at stale geometry.
    bool named = false;
    EntityName source;
    // Which face or edge of the part the name refers to. A face gives a
    // plane or a cylinder's axis; an edge gives its line or its circle's
    // axis. See DatumFromFace and DatumFromEdge.
    bool source_is_edge = false;
};

// Records where a datum came from, so the mate can find it again after
// the part it belongs to has been rebuilt.
bool CaptureMateEnd(const Model &model, EntityId face_or_edge, bool is_edge, int instance,
                    int generating_feature, const std::string &role, MateEnd *out, std::string *error);

struct Mate {
    int id = -1;
    MateKind kind = MateKind::Coincident;
    std::string name;
    MateEnd a;
    MateEnd b;
    // The offset for Offset, the angle in radians for Angle, the distance
    // for Distance. Ignored otherwise.
    double value = 0.0;
    // Faces flush the *same* way round rather than facing each other.
    bool flip = false;
    bool suppressed = false;
};

// --- The assembly -------------------------------------------------------

struct AssemblyReport {
    bool ok = false;
    std::string error;
    int iterations = 0;
    double residual = 0.0;
    // How much freedom the assembly has left. Zero means every instance
    // is pinned down; a positive number is not an error -- a hinge is
    // supposed to have one -- but it is the thing a user wants told.
    int degrees_of_freedom = 0;
    // Mates that are still not satisfied once the solve has stopped.
    // Over-constraining an assembly consistently is harmless and leaves
    // this empty; over-constraining it inconsistently is what fills it,
    // and naming which mates disagree is the only useful thing to say.
    std::vector<int> unsatisfied;
    // Mates whose geometry could not be found again after a rebuild.
    // Separate from `unsatisfied` because they are a different problem
    // with a different fix: one means the assembly is contradictory, the
    // other that a mate is pointing at something that is no longer there.
    std::vector<int> lost;
};

class Assembly {
public:
    struct Part {
        std::string name;
        // A part is one of three things, and the first two are the same
        // thing at different stages of its life: a finished body, or the
        // feature tree that produces one. Holding the tree is what makes
        // an assembly parametric all the way down -- change a dimension,
        // rebuild, and the mates re-find their faces by name.
        Model model;
        EntityId body = kNoEntity;
        std::shared_ptr<FeatureTree> tree;
        // ...or another assembly, used rigidly: its own mates are solved
        // within it, and from outside it moves as one piece. That is the
        // usual meaning of a sub-assembly and it is what makes a large
        // assembly tractable, since the parameters the outer solve
        // carries are six per sub-assembly rather than six per part in it.
        std::shared_ptr<Assembly> sub;
    };
    struct Instance {
        int id = -1;
        int part = -1;
        std::string name;
        Placement placement;
        // A grounded instance does not move. An assembly needs at least
        // one, for the same reason a sketch needs a fixed point: without
        // it every solution is a whole family, related by moving the
        // entire assembly at once.
        bool grounded = false;
    };

    int AddPart(Model model, EntityId body, const std::string &name);
    // A part that is rebuilt from its feature tree. The tree is shared,
    // so a caller holding the same pointer can change a dimension and
    // call RebuildParts.
    int AddPartFromTree(std::shared_ptr<FeatureTree> tree, const std::string &name);
    // A sub-assembly, placed as one rigid piece.
    int AddSubAssembly(std::shared_ptr<Assembly> assembly, const std::string &name);
    int AddInstance(int part, const std::string &name, const Placement &at = {}, bool grounded = false);
    int AddMate(const Mate &mate);
    bool RemoveMate(int id);

    const Part *GetPart(int id) const;
    const Instance *GetInstance(int id) const;
    Instance *GetInstanceMutable(int id);
    const Mate *GetMate(int id) const;
    Mate *GetMateMutable(int id);
    const std::vector<Instance> &Instances() const { return instances_; }
    const std::vector<Mate> &Mates() const { return mates_; }

    // Replays every feature-tree-backed part and re-resolves the mates
    // that remember where their datums came from. Any mate whose geometry
    // has gone is listed in `lost` and left alone, so the rest of the
    // assembly still solves and the user is told which references broke
    // rather than which assembly did.
    bool RebuildParts(std::vector<int> *lost, std::string *error);

    // Re-resolves the named mates against the parts as they are now,
    // without rebuilding anything. Called for you by RebuildParts.
    bool ResolveNamedMates(std::vector<int> *lost, std::string *error);

    // Finds placements satisfying the mates, starting from wherever the
    // instances are now -- so a good initial guess helps, and a solved
    // assembly re-solves immediately after a dimension changes.
    bool Solve(AssemblyReport *report);

    // Every instance placed, in one model, with sub-assemblies unpacked
    // all the way down. This is what gets tessellated, rendered, or
    // handed to a mesher.
    bool Flatten(Model *out, std::vector<EntityId> *out_bodies, std::string *error) const;

    // Solves this assembly and every sub-assembly under it, innermost
    // first -- which is the order that means anything, since a
    // sub-assembly is rigid from outside and its shape has to be settled
    // before it can be placed.
    bool SolveDeep(AssemblyReport *report);

private:
    std::vector<Part> parts_;
    std::vector<Instance> instances_;
    std::vector<Mate> mates_;
    int next_instance_ = 0;
    int next_mate_ = 0;
};

}  // namespace cad

#endif
