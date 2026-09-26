#ifndef MEP_CAD_NURBS_H
#define MEP_CAD_NURBS_H

#include "cad_math.h"

#include <string>
#include <vector>

// The B-spline and NURBS engine (plans/CAD_FEM_PLAN.md Parts A.2 and
// A.3): knot vectors, basis functions, evaluation, the shape-changing
// operations (knot insertion and removal, degree elevation, splitting,
// Bezier decomposition) and fitting.
//
// This file is deliberately *geometry-free*. It knows about knot vectors
// and homogeneous control points and nothing else -- no curve type, no
// surface type, no topology. cad_curve.h and cad_surface.h are the
// geometry; they call in here for the mathematics. Keeping the split
// means the algorithms below can be tested directly against an
// independent implementation of the same recurrence, which is exactly
// what cad_nurbs_test.cpp does, rather than only through the geometry
// that uses them.
//
// The algorithms follow Piegl and Tiller, *The NURBS Book* (2nd edition),
// and each one names the algorithm it implements (A2.1, A5.1 and so on).
// That is not decoration: these are intricate, and being able to check an
// implementation line by line against a published, widely-used reference
// is most of what makes them trustworthy. Where this file departs from
// the book -- and it does, in a few places where the book's pseudocode
// assumes Fortran-style fixed arrays or omits a degenerate case -- the
// departure is called out at the site.
//
// Control points are homogeneous (cad_math.h's Vec4d) throughout. A
// non-rational B-spline is simply the case where every weight is 1, and
// is not given a separate code path: the cost is one division per
// evaluation, and a second set of routines for the polynomial case would
// double the surface area of the most delicate code in the kernel for a
// saving nothing here is close to needing.
namespace cad {

// ---------------------------------------------------------------------
// Knot vectors
// ---------------------------------------------------------------------

// A knot vector is valid when it has exactly control_point_count + degree
// + 1 entries, is non-decreasing, and has no interior knot repeated more
// than `degree` times (a knot of multiplicity degree+1 would split the
// curve into disconnected pieces). Clamped vectors additionally repeat
// the first and last knot degree+1 times, which is what makes the curve
// pass through its end control points.
bool ValidateKnotVector(const std::vector<double> &knots, int degree, int control_point_count, std::string *error);

// The clamped uniform knot vector for the given degree and control point
// count: degree+1 zeros, evenly spaced interior knots, degree+1 ones.
// The default when a caller has control points and no opinion about
// parameterization.
std::vector<double> ClampedUniformKnots(int degree, int control_point_count);

// True when the knot vector is clamped at both ends.
bool IsClamped(const std::vector<double> &knots, int degree);

// The parameter range a curve or surface with this knot vector is
// actually defined over: [knots[degree], knots[n]] where n is the control
// point count. Evaluating outside it is meaningless, not merely
// inaccurate, because the basis functions do not sum to one there.
void KnotDomain(const std::vector<double> &knots, int degree, int control_point_count, double *out_lo,
                double *out_hi);

// The distinct interior knot values and their multiplicities -- the
// curve's own natural subdivision points, which the tessellator and the
// intersector both want (a curve is only C^(degree-multiplicity)
// continuous across one, so it is exactly where a subdivision scheme
// should break).
struct KnotSpan {
    double value = 0.0;
    int multiplicity = 0;
};
std::vector<KnotSpan> InteriorKnots(const std::vector<double> &knots, int degree, int control_point_count);

// ---------------------------------------------------------------------
// Basis functions
// ---------------------------------------------------------------------

// A2.1: the index of the knot span containing `u`, i.e. the i with
// knots[i] <= u < knots[i+1], clamped so that the domain's upper endpoint
// belongs to the last non-empty span rather than falling off the end.
// Returns -1 for a malformed knot vector.
int FindSpan(int degree, const std::vector<double> &knots, double u, int control_point_count);

// A2.2: the degree+1 basis functions that are non-zero at `u`, written
// into `out` (which must have room for degree+1). The rest are zero by
// construction and never computed -- which is the whole reason B-splines
// are tractable, and why nothing here ever builds the full basis.
void BasisFunctions(int span, double u, int degree, const std::vector<double> &knots, double *out);

// A2.3: the non-zero basis functions and their derivatives up to
// `max_derivative`. `out` is indexed [derivative][function], sized
// (max_derivative+1) x (degree+1).
void BasisFunctionDerivatives(int span, double u, int degree, int max_derivative, const std::vector<double> &knots,
                              std::vector<std::vector<double>> *out);

// The value of a single basis function N_{i,p}(u), evaluated by the
// Cox-de Boor recurrence directly. Quadratically slower than
// BasisFunctions above and present for exactly one reason: it is the
// independent implementation the tests check that one against. A bug
// shared between an algorithm and its test is the failure mode that
// matters, and these two share no code at all.
double BasisFunctionDirect(int i, int degree, const std::vector<double> &knots, double u);

// ---------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------

// A3.1/A4.1: the homogeneous point on the curve at `u`. Callers wanting a
// Euclidean point call .Project() on the result.
Vec4d CurvePointHomogeneous(int degree, const std::vector<double> &knots, const std::vector<Vec4d> &control,
                            double u);

// A3.2: homogeneous derivatives 0..max_derivative at `u`. Derivatives
// beyond `degree` are identically zero and returned as such.
void CurveDerivativesHomogeneous(int degree, const std::vector<double> &knots, const std::vector<Vec4d> &control,
                                 double u, int max_derivative, std::vector<Vec4d> *out);

// A4.2: turns homogeneous derivatives into Euclidean ones. This is the
// step that is easy to get wrong and impossible to notice: the Euclidean
// derivative of a rational curve is *not* the projection of the
// homogeneous derivative -- the quotient rule brings in every lower-order
// derivative through a binomial sum. Getting it wrong yields a first
// derivative that looks plausible (it points the right way) and a second
// derivative that is silently nonsense, which then shows up as wrong
// curvature in a fillet three parts of the plan later.
void RationalDerivatives(const std::vector<Vec4d> &homogeneous, int max_derivative, std::vector<Vec3d> *out);

// ---------------------------------------------------------------------
// Shape-preserving operations
//
// Every operation in this section changes the curve's *representation*
// without changing the curve. That invariant is what the tests check --
// not the resulting control points, which are an implementation detail,
// but that the geometry is unmoved to within round-off.
// ---------------------------------------------------------------------

// A5.1: inserts knot `u` `multiplicity` times. The most fundamental
// operation here -- splitting, Bezier decomposition and the boolean
// imprinting in Part C.4 are all built on it.
bool InsertKnot(int degree, std::vector<double> *knots, std::vector<Vec4d> *control, double u, int multiplicity);

// A5.4: inserts many knots at once. Materially faster than repeated
// single insertion when refining a whole curve, which the tessellator and
// the intersection marcher both do.
bool RefineKnots(int degree, std::vector<double> *knots, std::vector<Vec4d> *control,
                 const std::vector<double> &new_knots);

// A5.8: removes knot `u` up to `times` times, but only while the curve
// stays within `tolerance` of its original shape. Returns how many were
// actually removed, which may be zero -- unlike insertion, removal is not
// always possible, and a caller that assumes it succeeded will silently
// deform its geometry. The tolerance is a real distance in model space.
int RemoveKnot(int degree, std::vector<double> *knots, std::vector<Vec4d> *control, double u, int times,
               double tolerance);

// A5.9: raises the degree by `times`, leaving the curve unchanged.
// Needed wherever two curves must share a degree before they can be
// combined -- lofting through sections of different degrees (Part A.4),
// and every surface-surface intersection that fits one curve to another.
bool ElevateDegree(int *degree, std::vector<double> *knots, std::vector<Vec4d> *control, int times);

// Splits the curve at `u` into two curves, each clamped and each covering
// one side of the split. Implemented by inserting `u` to full
// multiplicity and then partitioning -- which is why it cannot fail for a
// `u` strictly inside the domain.
bool SplitCurve(int degree, const std::vector<double> &knots, const std::vector<Vec4d> &control, double u,
                std::vector<double> *left_knots, std::vector<Vec4d> *left_control,
                std::vector<double> *right_knots, std::vector<Vec4d> *right_control);

// Reverses the parameter direction, so the curve traces the same points
// in the opposite order. Needed constantly by the topology in Part B,
// where an edge is used in both directions by the two faces sharing it.
void ReverseCurve(std::vector<double> *knots, std::vector<Vec4d> *control);

// A5.6: decomposes into Bezier segments -- one set of degree+1 control
// points per segment. The subdivision half of surface-surface
// intersection (Part C.3) works on these, because a Bezier segment's
// control polygon bounds it, which a B-spline's does only loosely.
struct BezierSegments {
    int degree = 0;
    // segment[i] holds degree+1 control points.
    std::vector<std::vector<Vec4d>> segments;
    // The parameter interval of the original curve each segment covers,
    // so a hit found on a segment can be mapped back.
    std::vector<double> breakpoints;  // segments.size() + 1 values
};
BezierSegments DecomposeCurve(int degree, const std::vector<double> &knots, const std::vector<Vec4d> &control);

// ---------------------------------------------------------------------
// Fitting
// ---------------------------------------------------------------------

// How sample points are assigned parameter values before fitting. The
// choice visibly changes the resulting curve, and the default is not
// arbitrary: chord length is the usual recommendation, but centripetal
// (Lee's square-root rule) handles sharp turns markedly better, which is
// the case a CAD user notices because it is where an interpolated curve
// otherwise loops or overshoots.
enum class Parameterization { Uniform, ChordLength, Centripetal };

std::vector<double> ComputeParameters(const std::vector<Vec3d> &points, Parameterization kind);

// A9.1: a curve of the given degree passing exactly through every point.
// Returns false if there are too few points for the degree.
bool InterpolateCurve(const std::vector<Vec3d> &points, int degree, Parameterization parameterization,
                      std::vector<double> *out_knots, std::vector<Vec4d> *out_control);

// The same, with the parameter values supplied rather than derived from
// the points. Needed by surface skinning (LoftSurface), where every
// control-point column must be interpolated against *one shared*
// parameterization -- letting each column compute its own from its own
// point spacing gives each a different knot vector, and the columns then
// cannot be the rows of a single tensor-product grid. `parameters` must
// be strictly increasing and have one entry per point.
bool InterpolateCurveWithParameters(const std::vector<Vec3d> &points, int degree,
                                    const std::vector<double> &parameters, std::vector<double> *out_knots,
                                    std::vector<Vec4d> *out_control);

// The same again, over homogeneous control points, so that *weights* are
// interpolated along with positions. Skinning a surface through rational
// sections (any section that is a circle or an arc) must go through this
// one: projecting to 3D and interpolating there silently discards the
// weights that make a conic a conic.
bool InterpolateHomogeneous(const std::vector<Vec4d> &points, int degree, const std::vector<double> &parameters,
                            std::vector<double> *out_knots, std::vector<Vec4d> *out_control);

// Least-squares approximation with a fixed control point count -- the
// curve passes through the first and last points exactly and near the
// rest. The tool for turning a dense sampled polyline (the output of the
// intersection marcher in Part C.3) back into a compact curve.
bool ApproximateCurve(const std::vector<Vec3d> &points, int degree, int control_point_count,
                      Parameterization parameterization, std::vector<double> *out_knots,
                      std::vector<Vec4d> *out_control);

// ---------------------------------------------------------------------
// Surfaces (tensor product)
// ---------------------------------------------------------------------

// Control points for a surface are stored row-major in u: the point at
// (i, j) is control[i * count_v + j], with i indexing u and j indexing v.
// Every surface routine below takes the two counts explicitly rather than
// a 2D container, so the same flat storage works for a NurbsSurface, a
// temporary in a lofting routine and a sub-patch of a decomposition.
inline std::size_t SurfaceIndex(int i, int j, int count_v) {
    return static_cast<std::size_t>(i) * static_cast<std::size_t>(count_v) + static_cast<std::size_t>(j);
}

// A3.5/A4.3.
Vec4d SurfacePointHomogeneous(int degree_u, int degree_v, const std::vector<double> &knots_u,
                              const std::vector<double> &knots_v, const std::vector<Vec4d> &control, int count_u,
                              int count_v, double u, double v);

// A3.6: homogeneous partial derivatives up to total order
// `max_derivative`, written as out[k][l] = the (k in u, l in v) partial.
void SurfaceDerivativesHomogeneous(int degree_u, int degree_v, const std::vector<double> &knots_u,
                                   const std::vector<double> &knots_v, const std::vector<Vec4d> &control,
                                   int count_u, int count_v, double u, double v, int max_derivative,
                                   std::vector<std::vector<Vec4d>> *out);

// A4.4: the surface counterpart of RationalDerivatives, with the same
// warning attached -- the binomial correction runs over both indices here.
void RationalSurfaceDerivatives(const std::vector<std::vector<Vec4d>> &homogeneous, int max_derivative,
                                std::vector<std::vector<Vec3d>> *out);

// Extracts an isoparametric curve: the curve traced by holding u (or v)
// fixed. The workhorse of surface tessellation and of the marching
// scheme in Part C.3, and the cheapest way to turn a surface question
// into a curve question.
bool SurfaceIsoCurve(int degree_u, int degree_v, const std::vector<double> &knots_u,
                     const std::vector<double> &knots_v, const std::vector<Vec4d> &control, int count_u,
                     int count_v, bool fix_u, double fixed_value, int *out_degree,
                     std::vector<double> *out_knots, std::vector<Vec4d> *out_control);

// Inserts a knot into a surface in one direction, leaving the other
// alone. The surface analogue of InsertKnot, and what trimming and
// splitting a face are built on in Part B.
bool InsertKnotSurface(int degree_u, int degree_v, std::vector<double> *knots_u, std::vector<double> *knots_v,
                       std::vector<Vec4d> *control, int *count_u, int *count_v, bool in_u, double value,
                       int multiplicity);

}  // namespace cad

#endif
