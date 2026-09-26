#ifndef MEP_CAD_PREDICATES_H
#define MEP_CAD_PREDICATES_H

#include <cstddef>
#include <vector>

// Exact geometric predicates (plans/CAD_FEM_PLAN.md Part 0.2).
//
// These answer four questions -- is C left of AB, is D above ABC, is D
// inside the circle ABC, is E inside the sphere ABCD -- and they answer
// them *exactly*, never "probably". That distinction is not pedantry.
// Delaunay refinement (Part G.3) decides whether to flip a triangle by
// asking InCircle; if two nearby queries disagree about the same
// configuration because each was rounded differently, the mesher can
// build a triangulation that is not a triangulation at all -- overlapping
// elements, or an infinite flip loop that never terminates. Boundary
// recovery walks a face by repeatedly asking Orient3D which side of a
// plane a point is on, and a single inconsistent answer sends the walk
// into a cycle. Both failure modes are notoriously hard to debug from the
// symptom, because the arithmetic that caused them looks entirely
// reasonable in isolation.
//
// The implementation follows Shewchuk ("Adaptive Precision Floating-Point
// Arithmetic and Fast Robust Geometric Predicates", 1997): a fast
// floating-point evaluation with a rigorous error bound decides the
// common case, and only when the computed value is too close to zero to
// trust does the exact path run. It differs from Shewchuk's own code in
// one deliberate way, documented in the .cpp: where he interposes several
// progressively-more-precise stages between the filter and full exactness,
// this goes straight from the filter to an exact expansion computation.
// Same answers, a fraction of the code, and slower only on the inputs
// that reach the exact path at all.
//
// BUILD NOTE: this file's arithmetic assumes each floating-point
// operation is individually rounded to double. Contracting a multiply and
// an add into a fused multiply-add would break the error terms the exact
// arithmetic is built from, so cad_predicates.cpp is compiled with
// -ffp-contract=off (see CMakeLists.txt).
//
// As written, the primitives happen not to *need* that flag, and this was
// checked rather than assumed: built with -mfma -ffp-contract=fast, GCC
// emits FMA instructions into this translation unit and every test still
// passes, bit for bit. The reason is structural -- Split's product feeds
// two consumers, so it cannot be contracted, and TwoProduct's subtractions
// remove products that are exactly representable by construction, where
// fusing changes nothing. The flag is kept anyway, because that argument
// has to be re-made from scratch after any edit to those six functions,
// and a silent loss of exactness is close to undebuggable from its
// symptoms. PredicatesSelfTest() is the runtime backstop for both.
namespace cad {

// Sign of the orientation determinant of three 2D points, each given as
// two doubles. Returns +1 if a, b, c occur in counter-clockwise order,
// -1 if clockwise, 0 if exactly collinear.
int Orient2D(const double *pa, const double *pb, const double *pc);

// Sign of the orientation determinant of four 3D points. Returns +1 when
// d lies below the plane abc (meaning a, b, c appear counter-clockwise
// seen from above d, i.e. the tetrahedron abcd has positive orientation),
// -1 when above, 0 when exactly coplanar.
//
// This sign convention is Shewchuk's, and it is the one the tetrahedral
// mesher's element-inversion test is written against: a tet whose
// Orient3D is not positive has negative volume.
int Orient3D(const double *pa, const double *pb, const double *pc, const double *pd);

// Returns +1 if d lies inside the circle through a, b, c, -1 if outside,
// 0 if exactly cocircular -- assuming a, b, c are in counter-clockwise
// order. If they are clockwise the sign is reversed, which is why every
// caller either establishes the orientation first or multiplies by the
// Orient2D result.
int InCircle(const double *pa, const double *pb, const double *pc, const double *pd);

// Returns +1 if e lies inside the sphere through a, b, c, d, -1 if
// outside, 0 if exactly cospherical -- assuming abcd is positively
// oriented per Orient3D. Same orientation caveat as InCircle.
int InSphere(const double *pa, const double *pb, const double *pc, const double *pd, const double *pe);

// Verifies at runtime that the floating-point environment is the one the
// exact arithmetic above assumes: that a multiply and a subtract are not
// being contracted into an FMA, and that the exact-sum and exact-product
// primitives really are exact on inputs chosen so their error terms are
// non-zero. Also runs a handful of exactly-degenerate predicate queries
// whose answers a filtered-but-inexact implementation gets wrong.
//
// Returns true when everything holds. Called by the unit test, and cheap
// enough (a few dozen flops) to call from a solver's startup if the
// build ever grows a configuration where the compile flags might differ.
bool PredicatesSelfTest();

// ---------------------------------------------------------------------
// Exact expansion arithmetic
// ---------------------------------------------------------------------

// A multiple-component floating-point expansion: a value represented as
// an unevaluated sum of doubles that are non-overlapping and ordered by
// increasing magnitude. Any real number produced from doubles by +, - and
// * has such a representation, and the sign of the whole is simply the
// sign of its largest component -- which is what makes an exact sign test
// possible without any bignum library, integer conversion or rational
// arithmetic.
//
// Exposed rather than kept private to the predicates because the kernel
// has other places that need an exactly-signed determinant (the boolean
// classifier's ray-crossing parity in Part C.4 among them), and because
// it is far easier to test directly than through the predicates alone.
class Expansion {
public:
    Expansion() = default;
    explicit Expansion(double v);

    // All three are exact: no rounding occurs anywhere in them.
    Expansion operator+(const Expansion &o) const;
    Expansion operator-(const Expansion &o) const;
    Expansion operator*(const Expansion &o) const;
    Expansion operator-() const;

    // The exact sign of the represented value: +1, -1 or 0.
    int Sign() const;
    // The nearest double to the represented value -- for display and for
    // assertions, never for a comparison that has to be right.
    double Estimate() const;
    // Number of components; a rough measure of how much work the exact
    // path did, used by the test to confirm the fast path is actually
    // being taken where it should be.
    std::size_t ComponentCount() const { return c_.size(); }

private:
    // Non-overlapping, increasing magnitude, no zero components. An empty
    // vector represents exactly zero.
    std::vector<double> c_;
};

}  // namespace cad

#endif
