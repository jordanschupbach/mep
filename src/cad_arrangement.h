#ifndef MEP_CAD_ARRANGEMENT_H
#define MEP_CAD_ARRANGEMENT_H

#include "cad_curve.h"
#include "cad_math.h"

#include <memory>
#include <vector>

// A planar arrangement: a set of curves in the plane, cut at every
// crossing, assembled into a half-edge structure, and read back as the
// regions they divide the plane into (plans/CAD_FEM_PLAN.md Parts C.4 and
// D.4).
//
// Two very different callers need exactly this. A boolean cuts a face
// along the curves where the other solid meets it, and the pieces the
// face falls into are the regions of the arrangement of its trimming
// loops and those new curves, in the face's own parameter space. A
// sketcher finds the closed profiles the user has drawn, and those are
// the regions of the arrangement of the sketch's curves. The two differ
// only in what they hang off each curve, which is what `tag` is for.
//
// Writing it twice would mean finding the same handful of subtle bugs
// twice -- a closed curve having only one endpoint and so never becoming
// a half-edge, a curve's two cycles being mistaken for a region and its
// own hole, the unbounded outer face being mistaken for a hole, an
// interior point that is on the boundary by round-off. Each of those
// produced a plausible-looking wrong answer rather than a failure, and
// each is recorded where it is handled below.
//
// Curves are `Curve3` confined to z = 0, which is the p-curve convention
// Part B.2 already uses, so a face's p-curves need no conversion and a
// sketch's curves are built that way to begin with.
namespace cad {

// One input curve, or one piece of it after splitting.
struct ArrangementSegment {
    std::shared_ptr<const Curve3> curve;
    double t0 = 0.0;
    double t1 = 1.0;
    // The caller's own identifier for whatever this curve came from: a
    // face's coedge, a sketch entity. Carried through splitting and
    // reversal untouched, so a region's boundary can be traced back to
    // the geometry that produced it.
    int tag = -1;
};

struct ArrangementVertex {
    Vec2d position;
};

struct ArrangementHalfEdge {
    int origin = -1;
    int twin = -1;
    int next = -1;
    // Oriented from `origin` toward the twin's origin, so t0 is the end
    // this half-edge leaves from even when that reverses the curve.
    ArrangementSegment segment;
    bool visited = false;
};

struct ArrangementCycle {
    std::vector<int> half_edges;
    // Positive for a cycle bounding a region, negative for one bounding a
    // hole in a region or for the arrangement's unbounded outer face.
    double signed_area = 0.0;
};

// A region of the arrangement: a bounded area with a counter-clockwise
// outer boundary and any number of holes.
struct ArrangementRegion {
    int outer_cycle = -1;
    std::vector<int> hole_cycles;
    // Points in the region's interior, deepest first -- see
    // InteriorPoints for why more than one is kept.
    std::vector<Vec2d> interior;
    double area = 0.0;
};

class Arrangement {
public:
    void AddSegment(const ArrangementSegment &segment) { input_.push_back(segment); }
    void AddSegment(std::shared_ptr<const Curve3> curve, int tag);

    // `samples` is passed to the curve-curve intersector when looking for
    // crossings. The vertex-merging tolerance is derived from the
    // curves' own extent rather than taken from the caller, so that it
    // means the same distance whether the plane is parameterized 0..1 or
    // 0..2*pi.
    bool Build(int samples = 64);

    const std::vector<ArrangementCycle> &Cycles() const { return cycles_; }
    const std::vector<ArrangementHalfEdge> &HalfEdges() const { return half_edges_; }
    const std::vector<ArrangementVertex> &Vertices() const { return vertices_; }
    double VertexTolerance() const { return vertex_tolerance_; }

    // A polygon approximating one cycle, for containment tests and area.
    std::vector<Vec2d> CyclePolygon(const ArrangementCycle &cycle, int samples_per_edge = 8) const;
    std::vector<Vec2d> CyclePolygon(int cycle, int samples_per_edge = 8) const;

    // The regions, each with its holes attached and interior points
    // found. `interior_candidates` is how many interior points to look
    // for per region; a caller that will classify the region against
    // something else wants several, one that only needs to know where the
    // region is wants one.
    std::vector<ArrangementRegion> Regions(int interior_candidates = 4) const;

    // Whether two cycles trace the same edges in opposite directions.
    bool IsTwinCycle(const ArrangementCycle &a, const ArrangementCycle &b) const;

    // The segments of one cycle, in traversal order.
    std::vector<ArrangementSegment> CycleSegments(int cycle) const;

private:
    int VertexFor(const Vec2d &p);
    void SplitAtIntersections(int samples);
    bool BuildHalfEdges();
    double OutgoingAngle(const ArrangementHalfEdge &edge) const;
    void LinkFaces();
    void ExtractCycles();

    double vertex_tolerance_ = 1e-9;
    std::vector<ArrangementSegment> input_;
    std::vector<ArrangementSegment> pieces_;
    std::vector<ArrangementVertex> vertices_;
    std::vector<ArrangementHalfEdge> half_edges_;
    std::vector<ArrangementCycle> cycles_;
};

// --- Polygon helpers, shared by everything that reads an arrangement ---

// Crossing parity. Answers by round-off for a point on the boundary,
// which is why callers probe with interior points rather than with
// whatever vertex is to hand.
bool PointInPolygon2(const Vec2d &p, const std::vector<Vec2d> &polygon);

// The distance from a point to a polygon's boundary.
double DistanceToPolygon(const Vec2d &p, const std::vector<Vec2d> &polygon);

// Points strictly inside `outer` and outside every hole, deepest first
// and spread apart.
//
// The centroid works for a convex region and not otherwise, so a grid is
// swept -- but taking the first candidate the parity test accepts is not
// enough. Grid lines land on the region's own edges (an L-shaped piece
// cut from a square has its reflex corner at a round coordinate, and so
// does every grid), and there the parity test's answer is a coin flip.
// The candidate is chosen by depth instead: the grid point furthest from
// every bounding segment. That is the one place a rounding error cannot
// reach, and it is stable under refinement rather than dependent on scan
// order.
//
// More than one is returned because one is not always usable. A point
// that happens to lie on *another* solid's boundary classifies as neither
// inside nor outside it, and the region gets dropped -- a coincidence of
// position, not a coincident face, so the answer is to ask somewhere else
// rather than to give up.
bool InteriorPoints(const std::vector<Vec2d> &outer, const std::vector<std::vector<Vec2d>> &holes, int wanted,
                    std::vector<Vec2d> *out);
bool InteriorPoint(const std::vector<Vec2d> &outer, const std::vector<std::vector<Vec2d>> &holes, Vec2d *out);

// The signed area of a polygon: positive counter-clockwise.
double PolygonSignedArea(const std::vector<Vec2d> &polygon);

}  // namespace cad

#endif
