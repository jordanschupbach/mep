// Windowless coverage for cad_predicates.h (plans/CAD_FEM_PLAN.md Part
// 0.2). Links nothing but its own translation unit and cad_predicates.cpp.
//
// Testing an exact predicate needs an oracle that is itself exact, and
// this file gets one three different ways, because each catches a
// different class of mistake:
//
//  1. Configurations that are degenerate *by construction* -- points
//     placed on a line, a plane, a circle or a sphere using only small
//     integers, so every coordinate and every intermediate product is
//     exactly representable and the true answer is provably 0.
//  2. Those same configurations perturbed by exactly one ulp with
//     std::nextafter. The true sign is then known from the direction of
//     the perturbation, and it is a sign no inexact implementation can
//     recover -- the perturbation is orders of magnitude below the
//     rounding error of the naive determinant.
//  3. The filtered predicate cross-checked against the exact Expansion
//     arithmetic on thousands of random and near-degenerate inputs. This
//     is what actually validates the error bounds: if a bound were too
//     loose, the fast path would return a confident wrong sign, and only
//     a direct comparison against the exact computation finds it.
//
// Antisymmetry is checked throughout as a fourth, cheap invariant:
// swapping two arguments must flip the sign exactly, which an
// implementation with an inconsistent filter violates.

#include "cad_predicates.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

int g_checks = 0;

void Check(bool condition, const char *expression, int line) {
    ++g_checks;
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// The oracles: the same determinants the predicates compute, but written
// directly over exact expansion arithmetic with no filter in front. Slow
// and obviously correct, which is exactly what an oracle should be.
using cad::Expansion;

Expansion Diff(double a, double b) { return Expansion(a) - Expansion(b); }

int ExactOrient2D(const double *pa, const double *pb, const double *pc) {
    const Expansion acx = Diff(pa[0], pc[0]);
    const Expansion acy = Diff(pa[1], pc[1]);
    const Expansion bcx = Diff(pb[0], pc[0]);
    const Expansion bcy = Diff(pb[1], pc[1]);
    return (acx * bcy - acy * bcx).Sign();
}

int ExactOrient3D(const double *pa, const double *pb, const double *pc, const double *pd) {
    const Expansion ax = Diff(pa[0], pd[0]);
    const Expansion ay = Diff(pa[1], pd[1]);
    const Expansion az = Diff(pa[2], pd[2]);
    const Expansion bx = Diff(pb[0], pd[0]);
    const Expansion by = Diff(pb[1], pd[1]);
    const Expansion bz = Diff(pb[2], pd[2]);
    const Expansion cx = Diff(pc[0], pd[0]);
    const Expansion cy = Diff(pc[1], pd[1]);
    const Expansion cz = Diff(pc[2], pd[2]);
    return (az * (bx * cy - cx * by) + bz * (cx * ay - ax * cy) + cz * (ax * by - bx * ay)).Sign();
}

int ExactInCircle(const double *pa, const double *pb, const double *pc, const double *pd) {
    const Expansion ax = Diff(pa[0], pd[0]);
    const Expansion ay = Diff(pa[1], pd[1]);
    const Expansion bx = Diff(pb[0], pd[0]);
    const Expansion by = Diff(pb[1], pd[1]);
    const Expansion cx = Diff(pc[0], pd[0]);
    const Expansion cy = Diff(pc[1], pd[1]);
    const Expansion a_lift = ax * ax + ay * ay;
    const Expansion b_lift = bx * bx + by * by;
    const Expansion c_lift = cx * cx + cy * cy;
    return (a_lift * (bx * cy - cx * by) + b_lift * (cx * ay - ax * cy) + c_lift * (ax * by - bx * ay)).Sign();
}

int ExactInSphere(const double *pa, const double *pb, const double *pc, const double *pd, const double *pe) {
    const Expansion ax = Diff(pa[0], pe[0]);
    const Expansion ay = Diff(pa[1], pe[1]);
    const Expansion az = Diff(pa[2], pe[2]);
    const Expansion bx = Diff(pb[0], pe[0]);
    const Expansion by = Diff(pb[1], pe[1]);
    const Expansion bz = Diff(pb[2], pe[2]);
    const Expansion cx = Diff(pc[0], pe[0]);
    const Expansion cy = Diff(pc[1], pe[1]);
    const Expansion cz = Diff(pc[2], pe[2]);
    const Expansion dx = Diff(pd[0], pe[0]);
    const Expansion dy = Diff(pd[1], pe[1]);
    const Expansion dz = Diff(pd[2], pe[2]);
    const Expansion ab = ax * by - bx * ay;
    const Expansion bc = bx * cy - cx * by;
    const Expansion cd = cx * dy - dx * cy;
    const Expansion da = dx * ay - ax * dy;
    const Expansion ac = ax * cy - cx * ay;
    const Expansion bd = bx * dy - dx * by;
    const Expansion abc = az * bc - bz * ac + cz * ab;
    const Expansion bcd = bz * cd - cz * bd + dz * bc;
    const Expansion cda = cz * da + dz * ac + az * cd;
    const Expansion dab = dz * ab + az * bd + bz * da;
    const Expansion a_lift = ax * ax + ay * ay + az * az;
    const Expansion b_lift = bx * bx + by * by + bz * bz;
    const Expansion c_lift = cx * cx + cy * cy + cz * cz;
    const Expansion d_lift = dx * dx + dy * dy + dz * dz;
    return ((d_lift * abc - c_lift * dab) + (b_lift * cda - a_lift * bcd)).Sign();
}

void TestExpansionArithmetic() {
    // Exactness where a double cannot hold the result: 2^60 + 1 - 2^60.
    const double huge = std::ldexp(1.0, 60);
    const Expansion one = (Expansion(huge) + Expansion(1.0)) - Expansion(huge);
    CHECK(one.Sign() == 1);
    CHECK(one.Estimate() == 1.0);
    // The naive double computation really does lose it, which is what
    // makes the check above meaningful rather than vacuous.
    CHECK((huge + 1.0) - huge == 0.0);

    // Zero is represented by no components at all, and its sign is 0.
    const Expansion zero = Expansion(5.0) - Expansion(5.0);
    CHECK(zero.Sign() == 0);
    CHECK(zero.ComponentCount() == 0);
    CHECK(zero.Estimate() == 0.0);
    CHECK(Expansion().Sign() == 0);

    // Multiplication is exact where doubles round: (2^30+1)^2 needs 61
    // bits, so the square as a double is wrong but the expansion is not.
    const double a = std::ldexp(1.0, 30) + 1.0;
    const Expansion sq = Expansion(a) * Expansion(a);
    CHECK(sq.ComponentCount() >= 2);
    const Expansion expected = Expansion(std::ldexp(1.0, 60)) + Expansion(std::ldexp(1.0, 31)) + Expansion(1.0);
    CHECK((sq - expected).Sign() == 0);

    // Sign, negation and subtraction agree with each other.
    CHECK((Expansion(3.0) - Expansion(5.0)).Sign() == -1);
    CHECK((-(Expansion(3.0) - Expansion(5.0))).Sign() == 1);
    CHECK((Expansion(-2.0) * Expansion(-3.0)).Estimate() == 6.0);
    CHECK((Expansion(0.0) * Expansion(7.0)).Sign() == 0);

    // Distributivity over a chain long enough to grow several components.
    Expansion acc;
    for (int i = 1; i <= 40; ++i) {
        acc = acc + Expansion(std::ldexp(1.0, -i)) * Expansion(std::ldexp(1.0, i));
    }
    // Forty exact ones.
    CHECK((acc - Expansion(40.0)).Sign() == 0);
}

void TestOrient2D() {
    const double a[2] = {0.0, 0.0};
    const double b[2] = {1.0, 0.0};
    const double left[2] = {0.0, 1.0};
    const double right[2] = {0.0, -1.0};
    CHECK(cad::Orient2D(a, b, left) > 0);
    CHECK(cad::Orient2D(a, b, right) < 0);
    // Antisymmetry and cyclic invariance.
    CHECK(cad::Orient2D(b, a, left) < 0);
    CHECK(cad::Orient2D(b, left, a) > 0);
    CHECK(cad::Orient2D(left, a, b) > 0);
    // Degenerate: a repeated point is exactly collinear.
    CHECK(cad::Orient2D(a, b, b) == 0);
    CHECK(cad::Orient2D(a, a, left) == 0);

    // Exactly collinear by construction: integer points on one ray, all
    // products well under 2^53, so this is exactly collinear in double.
    for (int m = 2; m < 40; ++m) {
        const double p[2] = {0.0, 0.0};
        const double q[2] = {3.0, 7.0};
        const double r[2] = {3.0 * static_cast<double>(m), 7.0 * static_cast<double>(m)};
        CHECK(cad::Orient2D(p, q, r) == 0);
        // One ulp off the line, in each direction: an inexact predicate
        // returns 0 (or a random sign) for both of these.
        double up[2] = {r[0], std::nextafter(r[1], 1e300)};
        double down[2] = {r[0], std::nextafter(r[1], -1e300)};
        CHECK(cad::Orient2D(p, q, up) > 0);
        CHECK(cad::Orient2D(p, q, down) < 0);
        CHECK(cad::Orient2D(p, q, up) == ExactOrient2D(p, q, up));
        CHECK(cad::Orient2D(p, q, down) == ExactOrient2D(p, q, down));
    }

    // Far from the origin, where the coordinates' magnitude dwarfs the
    // feature size -- the situation a CAD model in a global coordinate
    // frame is permanently in, and the one where a naive determinant has
    // no significant digits left at all.
    {
        const double base = 1e7;
        const double p[2] = {base, base};
        const double q[2] = {base + 1.0, base + 1.0};
        const double r[2] = {base + 2.0, base + 2.0};
        CHECK(cad::Orient2D(p, q, r) == 0);
        double off[2] = {r[0], std::nextafter(r[1], 1e300)};
        CHECK(cad::Orient2D(p, q, off) > 0);
    }
}

void TestOrient3D() {
    const double a[3] = {0.0, 0.0, 0.0};
    const double b[3] = {1.0, 0.0, 0.0};
    const double c[3] = {0.0, 1.0, 0.0};
    const double below[3] = {0.0, 0.0, -1.0};
    const double above[3] = {0.0, 0.0, 1.0};
    // Sign convention: positive when the tetrahedron abcd has positive
    // volume, i.e. d is below the plane abc.
    CHECK(cad::Orient3D(a, b, c, below) > 0);
    CHECK(cad::Orient3D(a, b, c, above) < 0);
    // Swapping two vertices inverts a tetrahedron.
    CHECK(cad::Orient3D(b, a, c, below) < 0);
    // Coplanar by construction, including the repeated-point case.
    const double in_plane[3] = {5.0, 7.0, 0.0};
    CHECK(cad::Orient3D(a, b, c, in_plane) == 0);
    CHECK(cad::Orient3D(a, b, c, c) == 0);

    // Integer lattice points on a plane, perturbed by one ulp in z.
    for (int i = 1; i < 12; ++i) {
        for (int j = 1; j < 12; ++j) {
            const double di = static_cast<double>(i);
            const double dj = static_cast<double>(j);
            const double p[3] = {di, dj, 0.0};
            CHECK(cad::Orient3D(a, b, c, p) == 0);
            double up[3] = {di, dj, std::nextafter(0.0, 1e300)};
            double down[3] = {di, dj, std::nextafter(0.0, -1e300)};
            CHECK(cad::Orient3D(a, b, c, up) < 0);
            CHECK(cad::Orient3D(a, b, c, down) > 0);
            CHECK(cad::Orient3D(a, b, c, up) == ExactOrient3D(a, b, c, up));
        }
    }

    // A slanted plane, so the degeneracy is not axis-aligned: z = x + 2y
    // over integer x and y is exact.
    for (int i = -6; i <= 6; ++i) {
        for (int j = -6; j <= 6; ++j) {
            const double di = static_cast<double>(i);
            const double dj = static_cast<double>(j);
            const double p0[3] = {0.0, 0.0, 0.0};
            const double p1[3] = {1.0, 0.0, 1.0};
            const double p2[3] = {0.0, 1.0, 2.0};
            const double p3[3] = {di, dj, di + 2.0 * dj};
            CHECK(cad::Orient3D(p0, p1, p2, p3) == 0);
        }
    }
}

void TestInCircle() {
    // The unit circle's four axis points: any fourth is exactly on the
    // circle through the other three.
    const double e[2] = {1.0, 0.0};
    const double n[2] = {0.0, 1.0};
    const double w[2] = {-1.0, 0.0};
    const double s[2] = {0.0, -1.0};
    CHECK(cad::InCircle(e, n, w, s) == 0);
    const double inside[2] = {0.0, 0.0};
    const double outside[2] = {2.0, 2.0};
    // e, n, w is counter-clockwise, so a point inside reports +1.
    CHECK(cad::Orient2D(e, n, w) > 0);
    CHECK(cad::InCircle(e, n, w, inside) > 0);
    CHECK(cad::InCircle(e, n, w, outside) < 0);
    // Reversing the orientation of the defining triangle flips the sign,
    // the caveat the header calls out.
    CHECK(cad::InCircle(w, n, e, inside) < 0);

    // A radius-5 circle whose points all have integer coordinates, so
    // cocircularity is exact: (5,0),(4,3),(3,4),(0,5),(-3,4),(-4,3)...
    const double pts[8][2] = {{5.0, 0.0},  {4.0, 3.0},   {3.0, 4.0},   {0.0, 5.0},
                              {-3.0, 4.0}, {-4.0, 3.0},  {-5.0, 0.0},  {0.0, -5.0}};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t j = i + 1; j < 8; ++j) {
            for (std::size_t k = j + 1; k < 8; ++k) {
                for (std::size_t l = k + 1; l < 8; ++l) {
                    CHECK(cad::InCircle(pts[i], pts[j], pts[k], pts[l]) == 0);
                }
            }
        }
    }
    // One ulp outward from the circle must read as outside (for a
    // counter-clockwise triangle) and one ulp inward as inside. This is
    // the exact test the Delaunay flip decision depends on.
    {
        const double p0[2] = {5.0, 0.0};
        const double p1[2] = {0.0, 5.0};
        const double p2[2] = {-5.0, 0.0};
        CHECK(cad::Orient2D(p0, p1, p2) > 0);
        double out[2] = {3.0, std::nextafter(4.0, 1e300)};
        double in[2] = {3.0, std::nextafter(4.0, -1e300)};
        CHECK(cad::InCircle(p0, p1, p2, out) < 0);
        CHECK(cad::InCircle(p0, p1, p2, in) > 0);
        CHECK(cad::InCircle(p0, p1, p2, out) == ExactInCircle(p0, p1, p2, out));
        CHECK(cad::InCircle(p0, p1, p2, in) == ExactInCircle(p0, p1, p2, in));
    }
}

void TestInSphere() {
    // Radius-3 sphere with integer points: 1+4+4 = 9 and 9+0+0 = 9.
    const double pts[10][3] = {{3.0, 0.0, 0.0},  {0.0, 3.0, 0.0},  {0.0, 0.0, 3.0},  {-3.0, 0.0, 0.0},
                               {0.0, -3.0, 0.0}, {0.0, 0.0, -3.0}, {1.0, 2.0, 2.0},  {2.0, 1.0, 2.0},
                               {2.0, 2.0, 1.0},  {-1.0, -2.0, -2.0}};
    for (std::size_t i = 0; i < 10; ++i) {
        for (std::size_t j = i + 1; j < 10; ++j) {
            for (std::size_t k = j + 1; k < 10; ++k) {
                for (std::size_t l = k + 1; l < 10; ++l) {
                    for (std::size_t m = l + 1; m < 10; ++m) {
                        CHECK(cad::InSphere(pts[i], pts[j], pts[k], pts[l], pts[m]) == 0);
                    }
                }
            }
        }
    }

    // A positively-oriented tetrahedron on that sphere, then points just
    // inside and just outside it.
    const double a[3] = {3.0, 0.0, 0.0};
    const double b[3] = {0.0, 3.0, 0.0};
    const double c[3] = {0.0, 0.0, 3.0};
    const double d[3] = {-3.0, 0.0, 0.0};
    const int orientation = cad::Orient3D(a, b, c, d);
    CHECK(orientation != 0);
    const double center[3] = {0.0, 0.0, 0.0};
    const double far[3] = {10.0, 10.0, 10.0};
    // The sign convention holds for a positively-oriented tetrahedron; if
    // this one came out negative, both answers invert together.
    CHECK(cad::InSphere(a, b, c, d, center) * orientation > 0);
    CHECK(cad::InSphere(a, b, c, d, far) * orientation < 0);

    // One ulp in and out from an exactly-cospherical point.
    double out[3] = {1.0, 2.0, std::nextafter(2.0, 1e300)};
    double in[3] = {1.0, 2.0, std::nextafter(2.0, -1e300)};
    CHECK(cad::InSphere(a, b, c, d, out) != 0);
    CHECK(cad::InSphere(a, b, c, d, in) != 0);
    CHECK(cad::InSphere(a, b, c, d, out) == -cad::InSphere(a, b, c, d, in));
    CHECK(cad::InSphere(a, b, c, d, out) == ExactInSphere(a, b, c, d, out));
    CHECK(cad::InSphere(a, b, c, d, in) == ExactInSphere(a, b, c, d, in));
}

// The heart of the matter: the filtered predicates must agree with exact
// arithmetic on every input, including the ones engineered to sit right
// at the filter's threshold. A bound that is too loose shows up here and
// essentially nowhere else.
void TestFilterAgreesWithExact() {
    std::mt19937 rng(20260923);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    std::uniform_int_distribution<int> small(-8, 8);

    int nonzero_2d = 0;
    int zero_2d = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        double a[2];
        double b[2];
        double c[2];
        if (trial % 3 == 0) {
            // Fully random: almost always decided by the fast path.
            for (std::size_t i = 0; i < 2; ++i) {
                a[i] = unit(rng);
                b[i] = unit(rng);
                c[i] = unit(rng);
            }
        } else if (trial % 3 == 1) {
            // Small integers: degeneracies are common and exact.
            for (std::size_t i = 0; i < 2; ++i) {
                a[i] = static_cast<double>(small(rng));
                b[i] = static_cast<double>(small(rng));
                c[i] = static_cast<double>(small(rng));
            }
        } else {
            // Nearly collinear: c placed on the line ab, then nudged by a
            // few ulps. These land in the filter's uncertain band, which
            // is exactly where it must defer rather than guess.
            a[0] = unit(rng);
            a[1] = unit(rng);
            b[0] = unit(rng);
            b[1] = unit(rng);
            const double t = unit(rng);
            c[0] = a[0] + t * (b[0] - a[0]);
            c[1] = a[1] + t * (b[1] - a[1]);
            const int nudge = small(rng);
            for (int n = 0; n < std::abs(nudge); ++n) {
                c[1] = std::nextafter(c[1], nudge > 0 ? 1e300 : -1e300);
            }
        }
        const int filtered = cad::Orient2D(a, b, c);
        const int exact = ExactOrient2D(a, b, c);
        CHECK(filtered == exact);
        // Antisymmetry under a swap, checked on the filtered path.
        CHECK(cad::Orient2D(b, a, c) == -filtered);
        CHECK(cad::Orient2D(a, c, b) == -filtered);
        if (filtered == 0) {
            ++zero_2d;
        } else {
            ++nonzero_2d;
        }
    }
    // Both branches were actually exercised -- otherwise the loop above
    // proves much less than it appears to.
    CHECK(zero_2d > 100);
    CHECK(nonzero_2d > 1000);

    int zero_3d = 0;
    for (int trial = 0; trial < 8000; ++trial) {
        double a[3];
        double b[3];
        double c[3];
        double d[3];
        if (trial % 4 == 0) {
            for (std::size_t i = 0; i < 3; ++i) {
                a[i] = unit(rng);
                b[i] = unit(rng);
                c[i] = unit(rng);
                d[i] = unit(rng);
            }
        } else if (trial % 4 == 1) {
            for (std::size_t i = 0; i < 3; ++i) {
                a[i] = static_cast<double>(small(rng));
                b[i] = static_cast<double>(small(rng));
                c[i] = static_cast<double>(small(rng));
                d[i] = static_cast<double>(small(rng));
            }
        } else if (trial % 4 == 2) {
            // Exactly coplanar by construction: d is an *integer* combination
            // of the edges from a, with every coordinate small enough that
            // the arithmetic is exact. Randomly nudging a point toward a
            // plane (the branch below) almost never lands exactly on it, so
            // without this the exact path would go essentially untested.
            const int u = small(rng);
            const int v = small(rng);
            for (std::size_t i = 0; i < 3; ++i) {
                a[i] = static_cast<double>(small(rng));
                b[i] = static_cast<double>(small(rng));
                c[i] = static_cast<double>(small(rng));
                d[i] = a[i] + static_cast<double>(u) * (b[i] - a[i]) + static_cast<double>(v) * (c[i] - a[i]);
            }
        } else {
            // d placed in the plane of abc, then nudged.
            for (std::size_t i = 0; i < 3; ++i) {
                a[i] = unit(rng);
                b[i] = unit(rng);
                c[i] = unit(rng);
            }
            const double s = unit(rng);
            const double t = unit(rng);
            for (std::size_t i = 0; i < 3; ++i) d[i] = a[i] + s * (b[i] - a[i]) + t * (c[i] - a[i]);
            const int nudge = small(rng);
            for (int n = 0; n < std::abs(nudge); ++n) {
                d[2] = std::nextafter(d[2], nudge > 0 ? 1e300 : -1e300);
            }
        }
        const int filtered = cad::Orient3D(a, b, c, d);
        CHECK(filtered == ExactOrient3D(a, b, c, d));
        CHECK(cad::Orient3D(b, a, c, d) == -filtered);
        CHECK(cad::Orient3D(a, b, d, c) == -filtered);
        if (filtered == 0) ++zero_3d;
    }
    CHECK(zero_3d > 1000);

    int zero_ic = 0;
    for (int trial = 0; trial < 8000; ++trial) {
        double a[2];
        double b[2];
        double c[2];
        double d[2];
        if (trial % 3 == 0) {
            for (std::size_t i = 0; i < 2; ++i) {
                a[i] = unit(rng);
                b[i] = unit(rng);
                c[i] = unit(rng);
                d[i] = unit(rng);
            }
        } else if (trial % 3 == 1) {
            for (std::size_t i = 0; i < 2; ++i) {
                a[i] = static_cast<double>(small(rng));
                b[i] = static_cast<double>(small(rng));
                c[i] = static_cast<double>(small(rng));
                d[i] = static_cast<double>(small(rng));
            }
        } else {
            // Exactly cocircular by construction, from the integer points
            // of a radius-5 circle, shifted by an exactly-representable
            // offset so the centre is not always the origin.
            static const double kCircle[8][2] = {{5.0, 0.0},  {4.0, 3.0},  {3.0, 4.0},  {0.0, 5.0},
                                                 {-3.0, 4.0}, {-4.0, 3.0}, {-5.0, 0.0}, {0.0, -5.0}};
            std::uniform_int_distribution<int> pick(0, 7);
            const double ox = static_cast<double>(small(rng));
            const double oy = static_cast<double>(small(rng));
            int idx[4];
            // Four distinct points of the circle.
            for (std::size_t n = 0; n < 4; ++n) {
                bool duplicate = true;
                while (duplicate) {
                    idx[n] = pick(rng);
                    duplicate = false;
                    for (std::size_t m = 0; m < n; ++m) {
                        if (idx[m] == idx[n]) duplicate = true;
                    }
                }
            }
            for (std::size_t i = 0; i < 2; ++i) {
                const double offset = (i == 0) ? ox : oy;
                a[i] = kCircle[idx[0]][i] + offset;
                b[i] = kCircle[idx[1]][i] + offset;
                c[i] = kCircle[idx[2]][i] + offset;
                d[i] = kCircle[idx[3]][i] + offset;
            }
        }
        const int filtered = cad::InCircle(a, b, c, d);
        CHECK(filtered == ExactInCircle(a, b, c, d));
        // Swapping two of the three defining points flips the sign.
        CHECK(cad::InCircle(b, a, c, d) == -filtered);
        if (filtered == 0) ++zero_ic;
    }
    CHECK(zero_ic > 1000);

    int zero_is = 0;
    for (int trial = 0; trial < 3000; ++trial) {
        double a[3];
        double b[3];
        double c[3];
        double d[3];
        double e[3];
        if (trial % 3 == 0) {
            for (std::size_t i = 0; i < 3; ++i) {
                a[i] = unit(rng);
                b[i] = unit(rng);
                c[i] = unit(rng);
                d[i] = unit(rng);
                e[i] = unit(rng);
            }
        } else if (trial % 3 == 1) {
            for (std::size_t i = 0; i < 3; ++i) {
                a[i] = static_cast<double>(small(rng));
                b[i] = static_cast<double>(small(rng));
                c[i] = static_cast<double>(small(rng));
                d[i] = static_cast<double>(small(rng));
                e[i] = static_cast<double>(small(rng));
            }
        } else {
            // Exactly cospherical by construction, from the integer points
            // of a radius-3 sphere (1+4+4 = 9 = 9+0+0), shifted by an
            // exactly-representable offset. Same reasoning as the
            // cocircular branch above: randomness alone will not produce
            // these, and they are the whole point of an exact predicate.
            static const double kSphere[10][3] = {{3.0, 0.0, 0.0},  {0.0, 3.0, 0.0},  {0.0, 0.0, 3.0},
                                                  {-3.0, 0.0, 0.0}, {0.0, -3.0, 0.0}, {0.0, 0.0, -3.0},
                                                  {1.0, 2.0, 2.0},  {2.0, 1.0, 2.0},  {2.0, 2.0, 1.0},
                                                  {-1.0, -2.0, -2.0}};
            std::uniform_int_distribution<int> pick(0, 9);
            const double offsets[3] = {static_cast<double>(small(rng)), static_cast<double>(small(rng)),
                                       static_cast<double>(small(rng))};
            int idx[5];
            for (std::size_t n = 0; n < 5; ++n) {
                bool duplicate = true;
                while (duplicate) {
                    idx[n] = pick(rng);
                    duplicate = false;
                    for (std::size_t m = 0; m < n; ++m) {
                        if (idx[m] == idx[n]) duplicate = true;
                    }
                }
            }
            for (std::size_t i = 0; i < 3; ++i) {
                a[i] = kSphere[idx[0]][i] + offsets[i];
                b[i] = kSphere[idx[1]][i] + offsets[i];
                c[i] = kSphere[idx[2]][i] + offsets[i];
                d[i] = kSphere[idx[3]][i] + offsets[i];
                e[i] = kSphere[idx[4]][i] + offsets[i];
            }
        }
        const int filtered = cad::InSphere(a, b, c, d, e);
        CHECK(filtered == ExactInSphere(a, b, c, d, e));
        CHECK(cad::InSphere(b, a, c, d, e) == -filtered);
        if (filtered == 0) ++zero_is;
    }
    CHECK(zero_is > 500);
}

// Translating every point by the same vector must not change any
// predicate's answer, as long as the translation is exact in floating
// point (a power of two keeps it so). A predicate that folded the
// translation into its filter incorrectly fails this.
void TestTranslationInvariance() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    for (int trial = 0; trial < 2000; ++trial) {
        double a[2];
        double b[2];
        double c[2];
        for (std::size_t i = 0; i < 2; ++i) {
            a[i] = unit(rng);
            b[i] = unit(rng);
            c[i] = unit(rng);
        }
        const int base = cad::Orient2D(a, b, c);
        for (int shift_exp = 1; shift_exp <= 10; ++shift_exp) {
            const double shift = std::ldexp(1.0, shift_exp);
            double a2[2] = {a[0] + shift, a[1] + shift};
            double b2[2] = {b[0] + shift, b[1] + shift};
            double c2[2] = {c[0] + shift, c[1] + shift};
            // Only meaningful while the shifted coordinates are still
            // exact; beyond that the *points* changed, not the predicate.
            if (a2[0] - shift != a[0] || b2[0] - shift != b[0] || c2[0] - shift != c[0]) continue;
            if (a2[1] - shift != a[1] || b2[1] - shift != b[1] || c2[1] - shift != c[1]) continue;
            CHECK(cad::Orient2D(a2, b2, c2) == base);
        }
    }
}

}  // namespace

int main() {
    // Runs first: everything below is meaningless if the floating-point
    // environment is not the one the exact arithmetic assumes. A failure
    // here almost certainly means -ffp-contract=off was lost.
    if (!cad::PredicatesSelfTest()) {
        std::fprintf(stderr,
                     "PredicatesSelfTest failed -- the exact arithmetic is not exact in this build.\n"
                     "Most likely cause: cad_predicates.cpp was compiled without -ffp-contract=off,\n"
                     "so a multiply and a subtract were fused and the round-off terms are wrong.\n");
        return 1;
    }
    ++g_checks;

    TestExpansionArithmetic();
    TestOrient2D();
    TestOrient3D();
    TestInCircle();
    TestInSphere();
    TestFilterAgreesWithExact();
    TestTranslationInvariance();
    std::printf("cad_predicates_test passed (%d checks)\n", g_checks);
    return 0;
}
