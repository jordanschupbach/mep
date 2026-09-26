#ifndef MEP_CAD_MASS_H
#define MEP_CAD_MASS_H

#include "cad_math.h"
#include "cad_surface.h"

#include <vector>

// Mass properties (plans/CAD_FEM_PLAN.md Part A.6): volume, area,
// centroid and inertia tensor of a solid bounded by oriented surfaces.
//
// This is the single most useful verification hook in the whole kernel,
// which is why it arrives at the end of Part A rather than waiting for
// the topology that will eventually feed it. Every later operation has a
// volume it must preserve or change by a known amount:
//
//   * A boolean is checked by V(A or B) + V(A and B) = V(A) + V(B), which
//     holds for any correct implementation and for essentially no
//     incorrect one (Part C.5).
//   * A fillet removes a computable amount of material.
//   * An import is checked by comparing against the source system's own
//     reported volume -- the number every CAD package puts in its
//     properties dialog, so a STEP round-trip has an external oracle
//     (Part F.3).
//
// Everything is computed by the divergence theorem: a volume integral
// over the solid becomes a surface integral over its boundary, and a
// surface integral over a parametric patch becomes an ordinary double
// integral, because the vector area element n dA is exactly
// (Su x Sv) du dv. No volume meshing is involved and none is needed.
//
// LIMITATION, and it is the reason this is Part A.6 and not Part B:
// faces are untrimmed here. Each face contributes its whole rectangular
// parameter domain, so the surfaces handed in must already tile the
// solid's boundary exactly -- six planes for a box, a sphere for a ball,
// a tube plus two discs for a cylinder. Once Part B gives faces real
// trimming loops, the same integrals run over the trimmed region instead
// and nothing else about this file changes.
namespace cad {

// One boundary face. `reversed` flips the surface's own normal, which is
// what lets a single Surface be used as the boundary of the solid on
// either side of it -- the inside of a cylindrical hole is the same
// CylinderSurface as the outside of a pin.
struct MassFace {
    const Surface *surface = nullptr;
    bool reversed = false;
};

struct MassProperties {
    double volume = 0.0;
    double area = 0.0;
    Vec3d centroid;
    // Inertia tensor about the centroid, and about the world origin. Both
    // are wanted often enough -- the first for dynamics, the second for
    // assembling a system -- that computing one and making callers apply
    // the parallel-axis theorem themselves is a false economy.
    Mat3d inertia_centroid;
    Mat3d inertia_origin;
    double density = 1.0;
    double mass = 0.0;

    // The principal moments of inertia (eigenvalues of the centroidal
    // tensor), sorted ascending.
    void PrincipalMoments(double *out_sorted_three) const;
};

struct MassOptions {
    double density = 1.0;
    // Gauss-Legendre order per parameter direction, per cell.
    int quadrature_order = 8;
    // Each face's parameter domain is divided into this many cells in
    // each direction before the rule is applied. A fixed rule is exact
    // for a polynomial integrand -- which a planar face gives -- but a
    // sphere's integrand is not polynomial, and subdividing converges far
    // faster than raising the order on a single cell.
    int subdivisions = 8;
};

// Returns false if any face is null. Does not check that the faces
// actually form a closed boundary: that is a topological question this
// layer cannot answer, and it is what Part B.3's validity checker is for.
// The symptom of an unclosed boundary is a volume that is wrong rather
// than an error, so callers building faces by hand should sanity-check
// the result against something they know.
//
// AND EACH SURFACE IS INTEGRATED OVER ITS WHOLE PARAMETER DOMAIN, which
// is the trap in this signature and is worth spelling out because the
// arguments cannot express anything else. A `MassFace` is a surface and
// a sense; it carries no trimming, so there is no way for this to know
// which part of the surface a face actually uses. That is fine when the
// domain *is* the face -- which is how `MakeBox` and the rest of Part
// B.5 build their primitives, so every test here passes -- and silently
// catastrophic when it is not. A box written to STEP and read back has
// untrimmed planes with domains of -1e5 to 1e5, and the same 24 m^3
// solid measures 1.2e11. Before passing a real body's faces to this,
// check that their surfaces are trimmed to them; if they are not, or if
// you do not know where the body came from, integrate the tessellated
// boundary instead (see `ComputeBodyMass` in src/cad_fem_api.cpp), which
// honours trimming by construction and trades exactness on curved faces
// for an answer that does not depend on provenance.
bool ComputeMassProperties(const std::vector<MassFace> &faces, MassProperties *out,
                           const MassOptions &options = {});

// Surface area alone, when the volume is not wanted or the faces do not
// bound a solid at all (an open shell).
double ComputeArea(const std::vector<MassFace> &faces, const MassOptions &options = {});

}  // namespace cad

#endif
