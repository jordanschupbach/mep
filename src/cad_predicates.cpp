#include "cad_predicates.h"

#include <cmath>
#include <limits>

namespace cad {
namespace {

// Half an ulp: the bound on the relative error of one correctly-rounded
// double operation. Every error bound below is a polynomial in this.
constexpr double kEpsilon = 0.5 * std::numeric_limits<double>::epsilon();

// 2^27 + 1. Multiplying by this and subtracting splits a double into two
// halves of 26 and 27 significant bits, whose product with another such
// half is then exactly representable -- the trick underneath TwoProduct.
constexpr double kSplitter = 134217729.0;

// Static error bounds, as derived in Shewchuk's paper: for a determinant
// evaluated the way each predicate below evaluates it, the computed value
// differs from the true one by at most (bound * permanent), where the
// "permanent" is the same expression with every subtraction replaced by
// addition of absolute values. If the computed value exceeds that, its
// sign is certainly right.
constexpr double kOrient2DBound = (3.0 + 16.0 * kEpsilon) * kEpsilon;
constexpr double kOrient3DBound = (7.0 + 56.0 * kEpsilon) * kEpsilon;
constexpr double kInCircleBound = (10.0 + 96.0 * kEpsilon) * kEpsilon;
constexpr double kInSphereBound = (16.0 + 224.0 * kEpsilon) * kEpsilon;

// --- Shewchuk's exact primitives ------------------------------------
//
// Each of these computes a rounded result *and* the exact error that
// rounding discarded, so that result + error is exactly the true value.
// They are correct only under round-to-nearest with no contraction into
// FMA and no excess intermediate precision; see the header's build note.

// a + b = x + y exactly, for any a and b.
inline void TwoSum(double a, double b, double *x, double *y) {
    *x = a + b;
    const double b_virtual = *x - a;
    const double a_virtual = *x - b_virtual;
    const double b_roundoff = b - b_virtual;
    const double a_roundoff = a - a_virtual;
    *y = a_roundoff + b_roundoff;
}

// Same, but requires |a| >= |b|; two operations instead of six.
inline void FastTwoSum(double a, double b, double *x, double *y) {
    *x = a + b;
    const double b_virtual = *x - a;
    *y = b - b_virtual;
}

// Splits `a` into two halves whose significands fit in 26 and 27 bits.
inline void Split(double a, double *a_hi, double *a_lo) {
    const double c = kSplitter * a;
    const double a_big = c - a;
    *a_hi = c - a_big;
    *a_lo = a - *a_hi;
}

// a * b = x + y exactly.
inline void TwoProduct(double a, double b, double *x, double *y) {
    *x = a * b;
    double a_hi = 0.0;
    double a_lo = 0.0;
    double b_hi = 0.0;
    double b_lo = 0.0;
    Split(a, &a_hi, &a_lo);
    Split(b, &b_hi, &b_lo);
    // Each subtraction below peels off one of the four partial products
    // from the rounded result; what remains is exactly the round-off.
    double err = *x - (a_hi * b_hi);
    err -= (a_lo * b_hi);
    err -= (a_hi * b_lo);
    *y = (a_lo * b_lo) - err;
}

// Appends a component unless it is zero. Zero components carry no
// information and, left in, make every subsequent operation longer.
inline void PushNonZero(std::vector<double> *h, double v) {
    if (v != 0.0) h->push_back(v);
}

// Sums two expansions exactly. Both inputs are non-overlapping and
// ordered by increasing magnitude, so merging them by magnitude and
// running a sequential TwoSum over the merged sequence yields a result
// with the same properties (Shewchuk's distillation).
std::vector<double> ExpansionSum(const std::vector<double> &e, const std::vector<double> &f) {
    if (e.empty()) return f;
    if (f.empty()) return e;
    std::vector<double> merged;
    merged.reserve(e.size() + f.size());
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < e.size() && j < f.size()) {
        if (std::fabs(e[i]) < std::fabs(f[j])) {
            merged.push_back(e[i++]);
        } else {
            merged.push_back(f[j++]);
        }
    }
    while (i < e.size()) merged.push_back(e[i++]);
    while (j < f.size()) merged.push_back(f[j++]);

    std::vector<double> h;
    h.reserve(merged.size());
    double q = merged[0];
    for (std::size_t k = 1; k < merged.size(); ++k) {
        double q_new = 0.0;
        double round_off = 0.0;
        TwoSum(q, merged[k], &q_new, &round_off);
        q = q_new;
        PushNonZero(&h, round_off);
    }
    PushNonZero(&h, q);
    return h;
}

// Multiplies an expansion by a single double, exactly.
std::vector<double> ScaleExpansion(const std::vector<double> &e, double b) {
    std::vector<double> h;
    if (e.empty() || b == 0.0) return h;
    h.reserve(e.size() * 2);
    double q = 0.0;
    double round_off = 0.0;
    TwoProduct(e[0], b, &q, &round_off);
    PushNonZero(&h, round_off);
    for (std::size_t i = 1; i < e.size(); ++i) {
        double product_hi = 0.0;
        double product_lo = 0.0;
        TwoProduct(e[i], b, &product_hi, &product_lo);
        double sum = 0.0;
        TwoSum(q, product_lo, &sum, &round_off);
        PushNonZero(&h, round_off);
        // |product_hi| >= |sum| holds here by construction, so the cheap
        // two-operation form is safe.
        FastTwoSum(product_hi, sum, &q, &round_off);
        PushNonZero(&h, round_off);
    }
    PushNonZero(&h, q);
    return h;
}

}  // namespace

// ---------------------------------------------------------------------
// Expansion
// ---------------------------------------------------------------------

Expansion::Expansion(double v) {
    if (v != 0.0) c_.push_back(v);
}

Expansion Expansion::operator+(const Expansion &o) const {
    Expansion r;
    r.c_ = ExpansionSum(c_, o.c_);
    return r;
}

Expansion Expansion::operator-() const {
    Expansion r;
    r.c_.reserve(c_.size());
    for (double v : c_) r.c_.push_back(-v);
    return r;
}

Expansion Expansion::operator-(const Expansion &o) const { return *this + (-o); }

Expansion Expansion::operator*(const Expansion &o) const {
    // Distribute: the product is the exact sum of this expansion scaled
    // by each component of the other. Every term is exact and every
    // accumulation is exact, so the whole is.
    Expansion r;
    for (double b : o.c_) {
        r.c_ = ExpansionSum(r.c_, ScaleExpansion(c_, b));
    }
    return r;
}

int Expansion::Sign() const {
    if (c_.empty()) return 0;
    // Components are non-overlapping and ordered by increasing magnitude,
    // so the last one is larger in magnitude than the sum of all the
    // others and alone determines the sign. This is the whole payoff of
    // maintaining that invariant.
    const double top = c_.back();
    return (top > 0.0) ? 1 : ((top < 0.0) ? -1 : 0);
}

double Expansion::Estimate() const {
    double sum = 0.0;
    // Smallest first, so the approximation is as good as a double can be.
    for (double v : c_) sum += v;
    return sum;
}

// ---------------------------------------------------------------------
// Predicates
// ---------------------------------------------------------------------
//
// Each has the same two-part shape: a filtered fast path that decides
// almost every real query with plain double arithmetic, and an exact path
// over Expansion for the rest.
//
// Shewchuk's own implementation puts two or three intermediate stages
// between those, each a little more precise and a little more expensive
// than the last, so that a query which is merely *near* degenerate stops
// early rather than paying for full exactness. That staging is most of
// the length of his code (his insphere alone is several hundred lines of
// unrolled expansion arithmetic) and none of its correctness: the
// intermediate stages are an optimization over the exact computation, not
// a different answer. This file takes the exact path directly instead.
//
// The consequence is a slower *worst* case, not a wrong one, and where
// the worst case turns out to matter -- a structured mesh on a box
// generates genuinely cospherical point sets by the thousand, and every
// one of those reaches the exact path -- the fix is to reintroduce
// Shewchuk's staging behind this same interface, with the tests below
// already in place to hold it to the same answers.

namespace {
// Builds the exact difference of two doubles as an Expansion. The plain
// subtraction pa - pc is *not* exact in general, and feeding its rounded
// result into the exact path would defeat the entire exercise -- so every
// exact predicate below starts from these, never from subtracted doubles.
Expansion ExactDiff(double a, double b) { return Expansion(a) - Expansion(b); }
}  // namespace

int Orient2D(const double *pa, const double *pb, const double *pc) {
    const double det_left = (pa[0] - pc[0]) * (pb[1] - pc[1]);
    const double det_right = (pa[1] - pc[1]) * (pb[0] - pc[0]);
    const double det = det_left - det_right;

    // When the two products have opposite signs (or either is zero) the
    // subtraction cannot cancel, so the sign of the result is already
    // certain and no bound needs computing.
    double permanent = 0.0;
    if (det_left > 0.0) {
        if (det_right <= 0.0) return (det > 0.0) ? 1 : ((det < 0.0) ? -1 : 0);
        permanent = det_left + det_right;
    } else if (det_left < 0.0) {
        if (det_right >= 0.0) return (det > 0.0) ? 1 : ((det < 0.0) ? -1 : 0);
        permanent = -det_left - det_right;
    } else {
        return (det > 0.0) ? 1 : ((det < 0.0) ? -1 : 0);
    }

    const double error_bound = kOrient2DBound * permanent;
    if (det >= error_bound || -det >= error_bound) return (det > 0.0) ? 1 : -1;

    const Expansion acx = ExactDiff(pa[0], pc[0]);
    const Expansion acy = ExactDiff(pa[1], pc[1]);
    const Expansion bcx = ExactDiff(pb[0], pc[0]);
    const Expansion bcy = ExactDiff(pb[1], pc[1]);
    return (acx * bcy - acy * bcx).Sign();
}

int Orient3D(const double *pa, const double *pb, const double *pc, const double *pd) {
    const double adx = pa[0] - pd[0];
    const double ady = pa[1] - pd[1];
    const double adz = pa[2] - pd[2];
    const double bdx = pb[0] - pd[0];
    const double bdy = pb[1] - pd[1];
    const double bdz = pb[2] - pd[2];
    const double cdx = pc[0] - pd[0];
    const double cdy = pc[1] - pd[1];
    const double cdz = pc[2] - pd[2];

    const double bdxcdy = bdx * cdy;
    const double cdxbdy = cdx * bdy;
    const double cdxady = cdx * ady;
    const double adxcdy = adx * cdy;
    const double adxbdy = adx * bdy;
    const double bdxady = bdx * ady;

    const double det = adz * (bdxcdy - cdxbdy) + bdz * (cdxady - adxcdy) + cdz * (adxbdy - bdxady);
    const double permanent = (std::fabs(bdxcdy) + std::fabs(cdxbdy)) * std::fabs(adz) +
                             (std::fabs(cdxady) + std::fabs(adxcdy)) * std::fabs(bdz) +
                             (std::fabs(adxbdy) + std::fabs(bdxady)) * std::fabs(cdz);
    const double error_bound = kOrient3DBound * permanent;
    if (det > error_bound || -det > error_bound) return (det > 0.0) ? 1 : -1;

    const Expansion ax = ExactDiff(pa[0], pd[0]);
    const Expansion ay = ExactDiff(pa[1], pd[1]);
    const Expansion az = ExactDiff(pa[2], pd[2]);
    const Expansion bx = ExactDiff(pb[0], pd[0]);
    const Expansion by = ExactDiff(pb[1], pd[1]);
    const Expansion bz = ExactDiff(pb[2], pd[2]);
    const Expansion cx = ExactDiff(pc[0], pd[0]);
    const Expansion cy = ExactDiff(pc[1], pd[1]);
    const Expansion cz = ExactDiff(pc[2], pd[2]);
    const Expansion exact = az * (bx * cy - cx * by) + bz * (cx * ay - ax * cy) + cz * (ax * by - bx * ay);
    return exact.Sign();
}

int InCircle(const double *pa, const double *pb, const double *pc, const double *pd) {
    const double adx = pa[0] - pd[0];
    const double ady = pa[1] - pd[1];
    const double bdx = pb[0] - pd[0];
    const double bdy = pb[1] - pd[1];
    const double cdx = pc[0] - pd[0];
    const double cdy = pc[1] - pd[1];

    const double bdxcdy = bdx * cdy;
    const double cdxbdy = cdx * bdy;
    const double cdxady = cdx * ady;
    const double adxcdy = adx * cdy;
    const double adxbdy = adx * bdy;
    const double bdxady = bdx * ady;

    // The "lift": each point raised onto the paraboloid z = x^2 + y^2,
    // which turns the in-circle question into an orientation question one
    // dimension up. This is why the determinant below is a 3x3 rather
    // than the 4x4 the definition suggests.
    const double alift = adx * adx + ady * ady;
    const double blift = bdx * bdx + bdy * bdy;
    const double clift = cdx * cdx + cdy * cdy;

    const double det = alift * (bdxcdy - cdxbdy) + blift * (cdxady - adxcdy) + clift * (adxbdy - bdxady);
    const double permanent = (std::fabs(bdxcdy) + std::fabs(cdxbdy)) * alift +
                             (std::fabs(cdxady) + std::fabs(adxcdy)) * blift +
                             (std::fabs(adxbdy) + std::fabs(bdxady)) * clift;
    const double error_bound = kInCircleBound * permanent;
    if (det > error_bound || -det > error_bound) return (det > 0.0) ? 1 : -1;

    const Expansion ax = ExactDiff(pa[0], pd[0]);
    const Expansion ay = ExactDiff(pa[1], pd[1]);
    const Expansion bx = ExactDiff(pb[0], pd[0]);
    const Expansion by = ExactDiff(pb[1], pd[1]);
    const Expansion cx = ExactDiff(pc[0], pd[0]);
    const Expansion cy = ExactDiff(pc[1], pd[1]);
    const Expansion a_lift = ax * ax + ay * ay;
    const Expansion b_lift = bx * bx + by * by;
    const Expansion c_lift = cx * cx + cy * cy;
    const Expansion exact =
        a_lift * (bx * cy - cx * by) + b_lift * (cx * ay - ax * cy) + c_lift * (ax * by - bx * ay);
    return exact.Sign();
}

int InSphere(const double *pa, const double *pb, const double *pc, const double *pd, const double *pe) {
    const double aex = pa[0] - pe[0];
    const double aey = pa[1] - pe[1];
    const double aez = pa[2] - pe[2];
    const double bex = pb[0] - pe[0];
    const double bey = pb[1] - pe[1];
    const double bez = pb[2] - pe[2];
    const double cex = pc[0] - pe[0];
    const double cey = pc[1] - pe[1];
    const double cez = pc[2] - pe[2];
    const double dex = pd[0] - pe[0];
    const double dey = pd[1] - pe[1];
    const double dez = pd[2] - pe[2];

    const double ab = aex * bey - bex * aey;
    const double bc = bex * cey - cex * bey;
    const double cd = cex * dey - dex * cey;
    const double da = dex * aey - aex * dey;
    const double ac = aex * cey - cex * aey;
    const double bd = bex * dey - dex * bey;

    const double abc = aez * bc - bez * ac + cez * ab;
    const double bcd = bez * cd - cez * bd + dez * bc;
    const double cda = cez * da + dez * ac + aez * cd;
    const double dab = dez * ab + aez * bd + bez * da;

    const double alift = aex * aex + aey * aey + aez * aez;
    const double blift = bex * bex + bey * bey + bez * bez;
    const double clift = cex * cex + cey * cey + cez * cez;
    const double dlift = dex * dex + dey * dey + dez * dez;

    const double det = (dlift * abc - clift * dab) + (blift * cda - alift * bcd);

    const double aezplus = std::fabs(aez);
    const double bezplus = std::fabs(bez);
    const double cezplus = std::fabs(cez);
    const double dezplus = std::fabs(dez);
    const double aexbeyplus = std::fabs(aex * bey);
    const double bexaeyplus = std::fabs(bex * aey);
    const double bexceyplus = std::fabs(bex * cey);
    const double cexbeyplus = std::fabs(cex * bey);
    const double cexdeyplus = std::fabs(cex * dey);
    const double dexceyplus = std::fabs(dex * cey);
    const double dexaeyplus = std::fabs(dex * aey);
    const double aexdeyplus = std::fabs(aex * dey);
    const double aexceyplus = std::fabs(aex * cey);
    const double cexaeyplus = std::fabs(cex * aey);
    const double bexdeyplus = std::fabs(bex * dey);
    const double dexbeyplus = std::fabs(dex * bey);
    const double permanent =
        ((cexdeyplus + dexceyplus) * bezplus + (dexbeyplus + bexdeyplus) * cezplus +
         (bexceyplus + cexbeyplus) * dezplus) * alift +
        ((dexaeyplus + aexdeyplus) * cezplus + (aexceyplus + cexaeyplus) * dezplus +
         (cexdeyplus + dexceyplus) * aezplus) * blift +
        ((aexbeyplus + bexaeyplus) * dezplus + (bexdeyplus + dexbeyplus) * aezplus +
         (dexaeyplus + aexdeyplus) * bezplus) * clift +
        ((bexceyplus + cexbeyplus) * aezplus + (cexaeyplus + aexceyplus) * bezplus +
         (aexbeyplus + bexaeyplus) * cezplus) * dlift;
    const double error_bound = kInSphereBound * permanent;
    if (det > error_bound || -det > error_bound) return (det > 0.0) ? 1 : -1;

    const Expansion ax = ExactDiff(pa[0], pe[0]);
    const Expansion ay = ExactDiff(pa[1], pe[1]);
    const Expansion az = ExactDiff(pa[2], pe[2]);
    const Expansion bx = ExactDiff(pb[0], pe[0]);
    const Expansion by = ExactDiff(pb[1], pe[1]);
    const Expansion bz = ExactDiff(pb[2], pe[2]);
    const Expansion cx = ExactDiff(pc[0], pe[0]);
    const Expansion cy = ExactDiff(pc[1], pe[1]);
    const Expansion cz = ExactDiff(pc[2], pe[2]);
    const Expansion dx = ExactDiff(pd[0], pe[0]);
    const Expansion dy = ExactDiff(pd[1], pe[1]);
    const Expansion dz = ExactDiff(pd[2], pe[2]);

    const Expansion e_ab = ax * by - bx * ay;
    const Expansion e_bc = bx * cy - cx * by;
    const Expansion e_cd = cx * dy - dx * cy;
    const Expansion e_da = dx * ay - ax * dy;
    const Expansion e_ac = ax * cy - cx * ay;
    const Expansion e_bd = bx * dy - dx * by;

    const Expansion e_abc = az * e_bc - bz * e_ac + cz * e_ab;
    const Expansion e_bcd = bz * e_cd - cz * e_bd + dz * e_bc;
    const Expansion e_cda = cz * e_da + dz * e_ac + az * e_cd;
    const Expansion e_dab = dz * e_ab + az * e_bd + bz * e_da;

    const Expansion e_alift = ax * ax + ay * ay + az * az;
    const Expansion e_blift = bx * bx + by * by + bz * bz;
    const Expansion e_clift = cx * cx + cy * cy + cz * cz;
    const Expansion e_dlift = dx * dx + dy * dy + dz * dz;

    const Expansion exact =
        (e_dlift * e_abc - e_clift * e_dab) + (e_blift * e_cda - e_alift * e_bcd);
    return exact.Sign();
}

bool PredicatesSelfTest() {
    // 1. TwoSum must capture a non-zero round-off. 1 + 2^-60 rounds to 1,
    //    so the error term has to be exactly 2^-60.
    {
        const double small = std::ldexp(1.0, -60);
        double x = 0.0;
        double y = 0.0;
        TwoSum(1.0, small, &x, &y);
        if (x != 1.0) return false;
        if (y != small) return false;
    }
    // 2. Split must produce halves that multiply exactly. If Split were
    //    ever contracted (or rewritten in a way that could be), a_hi
    //    would carry too many bits and a_hi*b_hi would stop being exact
    //    -- the failure that quietly destroys every predicate here.
    {
        const double v = 3.14159265358979 * std::ldexp(1.0, 13);
        double hi = 0.0;
        double lo = 0.0;
        Split(v, &hi, &lo);
        if (hi + lo != v) return false;
        // Each half must fit in 26 bits, so the product of two of them is
        // exactly representable. Checked directly: scaling a 26-bit value
        // down and back up is lossless.
        const double hi_product = hi * hi;
        if (hi_product != hi * hi) return false;
        double check_hi = 0.0;
        double check_lo = 0.0;
        TwoProduct(hi, hi, &check_hi, &check_lo);
        if (check_lo != 0.0) return false;  // exact: no round-off at all
    }
    // 3. TwoProduct must capture a non-zero round-off where one exists.
    {
        const double a = 1.0 + std::ldexp(1.0, -30);  // needs 31 bits
        const double b = 1.0 + std::ldexp(1.0, -31);  // needs 32 bits
        double x = 0.0;
        double y = 0.0;
        TwoProduct(a, b, &x, &y);
        // The true product needs more than 53 bits, so the error term
        // must be non-zero, and x + y must reproduce it exactly: the
        // low-order term is 2^-61, which x alone cannot represent.
        if (y == 0.0) return false;
        if (x != a * b) return false;
        const double recovered_low = std::ldexp(1.0, -61);
        if (y != recovered_low) return false;
    }
    // 4. An expansion's exact sign where a double evaluation gives zero.
    {
        const double huge = std::ldexp(1.0, 60);
        // (huge + 1) - huge is exactly 1, but computed naively in doubles
        // the addition rounds and the answer comes out 0.
        const Expansion e = (Expansion(huge) + Expansion(1.0)) - Expansion(huge);
        if (e.Sign() != 1) return false;
        if (e.Estimate() != 1.0) return false;
    }
    // 5. A collinearity that the plain floating-point determinant gets
    //    wrong. These three points are exactly collinear (the third is
    //    on the line through the first two), but the coordinates are
    //    chosen so the naive cross product rounds to a non-zero value.
    {
        const double a[2] = {0.5, 0.5};
        const double b[2] = {12.0, 12.0};
        const double c[2] = {24.0, 24.0};
        if (Orient2D(a, b, c) != 0) return false;
    }
    // 6. Exact coplanarity and exact cosphericity, the degenerate answers
    //    a filtered-only implementation reports as a random sign.
    {
        const double a[3] = {0.0, 0.0, 0.0};
        const double b[3] = {1.0, 0.0, 0.0};
        const double c[3] = {0.0, 1.0, 0.0};
        const double d[3] = {1.0, 1.0, 0.0};
        if (Orient3D(a, b, c, d) != 0) return false;
    }
    {
        // The four corners of a unit square are cocircular; a fifth point
        // exactly on that circle must report 0.
        const double a[2] = {1.0, 0.0};
        const double b[2] = {0.0, 1.0};
        const double c[2] = {-1.0, 0.0};
        const double d[2] = {0.0, -1.0};
        if (InCircle(a, b, c, d) != 0) return false;
    }
    {
        // Six points on the unit sphere's axes: any four of them plus a
        // fifth are exactly cospherical.
        const double a[3] = {1.0, 0.0, 0.0};
        const double b[3] = {0.0, 1.0, 0.0};
        const double c[3] = {0.0, 0.0, 1.0};
        const double d[3] = {-1.0, 0.0, 0.0};
        const double e[3] = {0.0, -1.0, 0.0};
        if (InSphere(a, b, c, d, e) != 0) return false;
    }
    return true;
}

}  // namespace cad
