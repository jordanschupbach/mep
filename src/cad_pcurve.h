#ifndef MEP_CAD_PCURVE_H
#define MEP_CAD_PCURVE_H

#include "cad_math.h"
#include "cad_topology.h"

#include <string>
#include <vector>

// P-curves (plans/CAD_FEM_PLAN.md Part B.2): an edge's curve expressed in
// the parameter space of the face that uses it.
//
// Every face is a region of a surface, and the region is described by
// loops drawn in that surface's (u,v) plane. So each coedge needs the
// edge's 3D curve written as a 2D curve in *its own face's* parameters --
// and the two faces along an edge need different ones, which is why the
// p-curve belongs to the coedge rather than to the edge.
//
// REPRESENTATION: a p-curve is a Curve3 confined to the z = 0 plane, with
// x holding u and y holding v. That is not a hack for its own sake -- it
// means a straight p-curve is an exact Line3, a circular one an exact
// Circle3, and everything else a NurbsCurve3, all evaluated, split,
// refined and inverted by machinery that already exists and is already
// tested. A separate 2D curve hierarchy would duplicate cad_curve.h
// almost line for line to save one unused coordinate.
//
// The two cases that make this hard, both of which the plan calls out:
//
//   * SEAMS. On a surface closed in u (a cylinder, sphere or torus), the
//     parameters u and u + 2*pi name the same 3D point. Projecting a
//     seam edge therefore has two equally valid answers, and the two
//     coedges of that edge need *different* ones -- the face's boundary
//     runs up one side of the parameter rectangle and down the other.
//     Choosing per coedge in isolation cannot work; the choice is made by
//     requiring the loop to close in parameter space.
//   * POLES. At a sphere's pole the surface is degenerate: every u maps
//     to the same point, so projection returns an arbitrary one. The
//     value has to come from the neighbouring samples instead.
namespace cad {

struct PCurveOptions {
    // Samples taken along the edge before fitting. More costs time and
    // buys accuracy on a curve whose image in parameter space is not one
    // of the exact families below.
    int sample_count = 64;
    // Degree of the fitted NURBS, used only when the samples are not
    // recognised as a line or a circle.
    int fit_degree = 3;
    // A fitted p-curve is accepted when every sample lies within this
    // distance of it, measured in parameter space.
    double fit_tolerance = 1e-7;
    Tolerance tolerance;
};

// Builds (or rebuilds) the p-curves of every coedge of one loop, together.
// Together is the only way that works: the seam choice for one coedge is
// fixed by its neighbours, not by anything intrinsic to it.
bool BuildLoopPCurves(Model *model, EntityId loop, const PCurveOptions &options, std::string *error);

// Every loop of every face that is missing at least one p-curve.
bool BuildAllPCurves(Model *model, const PCurveOptions &options, std::string *error);

// Evaluates a p-curve as a 2D parameter-space point. Trivial, but it
// reads far better than `.Point(t)` followed by dropping z, and it is the
// only place the z = 0 convention is assumed.
Vec2d PCurvePoint(const Curve3 &pcurve, double t);
Vec2d PCurveTangent(const Curve3 &pcurve, double t);

// The signed area enclosed by a loop in its face's parameter space,
// positive when the loop runs counter-clockwise. An outer loop must be
// positive and an inner (hole) loop negative; this is what the validity
// checker tests loop orientation with.
//
// The value is a *polygonal* approximation -- the p-curves are sampled
// and the shoelace formula applied -- so it is exact for a loop of
// straight p-curves (every planar face, every seam) and low for a curved
// one: a circle of radius r sampled 16 times reads 6.888 rather than
// pi*r^2 = 7.069. That is fine for the only thing it is used for, which
// is the sign. Anything wanting a real area should integrate, as
// cad_mass.h does.
//
// Gaps between one coedge's p-curve end and the next one's start are
// bridged with a straight segment. That is not papering over a defect: a
// sphere's face is bounded by a seam used twice, and the two ends of the
// parameter rectangle at the poles are genuinely degenerate -- they have
// no edge because they collapse to a point. Bridging them is what makes
// the area come out as the rectangle it actually is.
double LoopSignedArea(const Model &model, EntityId loop, int samples_per_coedge = 16);

}  // namespace cad

#endif
