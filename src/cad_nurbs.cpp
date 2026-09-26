#include "cad_nurbs.h"

#include <algorithm>
#include <cmath>

namespace cad {
namespace {

inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// Binomial coefficients, cached up to a degree far beyond anything a real
// NURBS uses. Needed by the rational derivative formulas, which are the
// only place in this file where a combinatorial factor appears.
double Binomial(int n, int k) {
    if (k < 0 || k > n) return 0.0;
    static constexpr int kMax = 40;
    static double table[kMax + 1][kMax + 1];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i <= kMax; ++i) {
            table[i][0] = 1.0;
            for (int j = 1; j <= i; ++j) {
                table[i][j] = (j == i) ? 1.0 : table[i - 1][j - 1] + table[i - 1][j];
            }
            for (int j = i + 1; j <= kMax; ++j) table[i][j] = 0.0;
        }
        ready = true;
    }
    if (n > kMax) return 0.0;
    return table[n][k];
}

double Distance4D(const Vec4d &a, const Vec4d &b) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    const double dw = a.w - b.w;
    return std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw);
}

// Banded Gaussian elimination without pivoting, for the interpolation
// system in InterpolateCurve. The collocation matrix of a B-spline basis
// at its own averaged parameters is banded (half-bandwidth `degree`) and
// totally positive, which means it is non-singular and diagonally
// dominant enough that pivoting is provably unnecessary -- the standard
// result behind every B-spline interpolation routine. Using a dense
// solver here instead would be O(n^3) and would make interpolating the
// few thousand points that an intersection curve produces (Part C.3)
// impractical for no reason.
//
// `a` is n x n stored densely but only accessed within the band.
template <typename Point>
bool SolveBanded(std::vector<double> &a, int n, int bandwidth, std::vector<Point> &rhs) {
    for (int k = 0; k < n; ++k) {
        const double pivot = a[Idx(k * n + k)];
        if (std::fabs(pivot) < 1e-300) return false;
        const int last = std::min(n - 1, k + bandwidth);
        for (int i = k + 1; i <= last; ++i) {
            const double factor = a[Idx(i * n + k)] / pivot;
            if (factor == 0.0) continue;
            const int last_col = std::min(n - 1, k + bandwidth);
            for (int j = k; j <= last_col; ++j) a[Idx(i * n + j)] -= factor * a[Idx(k * n + j)];
            rhs[Idx(i)] -= rhs[Idx(k)] * factor;
        }
    }
    for (int i = n; i-- > 0;) {
        Point sum = rhs[Idx(i)];
        const int last = std::min(n - 1, i + bandwidth);
        for (int j = i + 1; j <= last; ++j) sum -= rhs[Idx(j)] * a[Idx(i * n + j)];
        rhs[Idx(i)] = sum / a[Idx(i * n + i)];
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------
// Knot vectors
// ---------------------------------------------------------------------

bool ValidateKnotVector(const std::vector<double> &knots, int degree, int control_point_count, std::string *error) {
    if (degree < 1) {
        *error = "degree must be at least 1";
        return false;
    }
    if (control_point_count < degree + 1) {
        *error = "a degree-" + std::to_string(degree) + " curve needs at least " + std::to_string(degree + 1) +
                 " control points, got " + std::to_string(control_point_count);
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(control_point_count + degree + 1);
    if (knots.size() != expected) {
        *error = "knot vector must have control_point_count + degree + 1 = " + std::to_string(expected) +
                 " entries, got " + std::to_string(knots.size());
        return false;
    }
    for (std::size_t i = 1; i < knots.size(); ++i) {
        if (knots[i] < knots[i - 1]) {
            *error = "knot vector is not non-decreasing at index " + std::to_string(i);
            return false;
        }
    }
    if (!(knots.back() > knots.front())) {
        *error = "knot vector spans an empty parameter range";
        return false;
    }
    // Interior multiplicity above `degree` would disconnect the curve.
    // The end knots are allowed degree+1 (that is what "clamped" means),
    // so the scan runs strictly between them.
    std::size_t i = 0;
    while (i < knots.size()) {
        std::size_t j = i;
        while (j < knots.size() && knots[j] == knots[i]) ++j;
        const int multiplicity = static_cast<int>(j - i);
        const bool at_start = (i == 0);
        const bool at_end = (j == knots.size());
        const int limit = (at_start || at_end) ? degree + 1 : degree;
        if (multiplicity > limit) {
            *error = "knot " + std::to_string(knots[i]) + " has multiplicity " + std::to_string(multiplicity) +
                     ", above the limit of " + std::to_string(limit) +
                     (at_start || at_end ? " for an end knot" : " for an interior knot");
            return false;
        }
        i = j;
    }
    return true;
}

std::vector<double> ClampedUniformKnots(int degree, int control_point_count) {
    std::vector<double> knots;
    if (degree < 1 || control_point_count < degree + 1) return knots;
    const int total = control_point_count + degree + 1;
    knots.assign(Idx(total), 0.0);
    const int interior = control_point_count - degree - 1;
    for (int i = 0; i <= degree; ++i) knots[Idx(i)] = 0.0;
    for (int i = 1; i <= interior; ++i) {
        knots[Idx(degree + i)] = static_cast<double>(i) / static_cast<double>(interior + 1);
    }
    for (int i = 0; i <= degree; ++i) knots[Idx(total - 1 - i)] = 1.0;
    return knots;
}

bool IsClamped(const std::vector<double> &knots, int degree) {
    if (knots.size() < static_cast<std::size_t>(2 * (degree + 1))) return false;
    for (int i = 1; i <= degree; ++i) {
        if (knots[Idx(i)] != knots[0]) return false;
        if (knots[knots.size() - 1 - Idx(i)] != knots.back()) return false;
    }
    return true;
}

void KnotDomain(const std::vector<double> &knots, int degree, int control_point_count, double *out_lo,
                double *out_hi) {
    if (knots.empty()) {
        *out_lo = 0.0;
        *out_hi = 0.0;
        return;
    }
    *out_lo = knots[Idx(std::min(degree, static_cast<int>(knots.size()) - 1))];
    *out_hi = knots[Idx(std::min(control_point_count, static_cast<int>(knots.size()) - 1))];
}

std::vector<KnotSpan> InteriorKnots(const std::vector<double> &knots, int degree, int control_point_count) {
    std::vector<KnotSpan> result;
    double lo = 0.0;
    double hi = 0.0;
    KnotDomain(knots, degree, control_point_count, &lo, &hi);
    std::size_t i = 0;
    while (i < knots.size()) {
        std::size_t j = i;
        while (j < knots.size() && knots[j] == knots[i]) ++j;
        if (knots[i] > lo && knots[i] < hi) {
            KnotSpan span;
            span.value = knots[i];
            span.multiplicity = static_cast<int>(j - i);
            result.push_back(span);
        }
        i = j;
    }
    return result;
}

// ---------------------------------------------------------------------
// Basis functions
// ---------------------------------------------------------------------

int FindSpan(int degree, const std::vector<double> &knots, double u, int control_point_count) {
    const int n = control_point_count - 1;
    if (degree < 0 || n < degree || knots.size() < static_cast<std::size_t>(n + degree + 2)) return -1;
    // The domain's upper endpoint belongs to the last non-empty span. Without
    // this special case a binary search for u == knots[n+1] runs off the end,
    // and evaluating a curve at its own endpoint is not an edge case -- it is
    // what every tessellator does first.
    if (u >= knots[Idx(n + 1)]) return n;
    if (u <= knots[Idx(degree)]) return degree;
    int low = degree;
    int high = n + 1;
    int mid = (low + high) / 2;
    while (u < knots[Idx(mid)] || u >= knots[Idx(mid + 1)]) {
        if (u < knots[Idx(mid)]) {
            high = mid;
        } else {
            low = mid;
        }
        mid = (low + high) / 2;
    }
    return mid;
}

void BasisFunctions(int span, double u, int degree, const std::vector<double> &knots, double *out) {
    // A2.2. The recurrence is arranged so no division by zero can occur:
    // `right[r+1] + left[j-r]` is a knot *difference* that is provably
    // positive for every term the algorithm actually touches, which is why
    // the textbook formulation never needs the 0/0 guard the naive
    // Cox-de Boor recurrence does.
    std::vector<double> left(Idx(degree + 1), 0.0);
    std::vector<double> right(Idx(degree + 1), 0.0);
    out[0] = 1.0;
    for (int j = 1; j <= degree; ++j) {
        left[Idx(j)] = u - knots[Idx(span + 1 - j)];
        right[Idx(j)] = knots[Idx(span + j)] - u;
        double saved = 0.0;
        for (int r = 0; r < j; ++r) {
            const double denominator = right[Idx(r + 1)] + left[Idx(j - r)];
            const double temp = out[r] / denominator;
            out[r] = saved + right[Idx(r + 1)] * temp;
            saved = left[Idx(j - r)] * temp;
        }
        out[j] = saved;
    }
}

void BasisFunctionDerivatives(int span, double u, int degree, int max_derivative, const std::vector<double> &knots,
                              std::vector<std::vector<double>> *out) {
    // A2.3.
    out->assign(Idx(max_derivative + 1), std::vector<double>(Idx(degree + 1), 0.0));
    std::vector<std::vector<double>> ndu(Idx(degree + 1), std::vector<double>(Idx(degree + 1), 0.0));
    std::vector<double> left(Idx(degree + 1), 0.0);
    std::vector<double> right(Idx(degree + 1), 0.0);
    ndu[0][0] = 1.0;
    for (int j = 1; j <= degree; ++j) {
        left[Idx(j)] = u - knots[Idx(span + 1 - j)];
        right[Idx(j)] = knots[Idx(span + j)] - u;
        double saved = 0.0;
        for (int r = 0; r < j; ++r) {
            // Lower triangle holds the knot differences, upper the basis
            // values -- one array doing two jobs, as the book has it.
            ndu[Idx(j)][Idx(r)] = right[Idx(r + 1)] + left[Idx(j - r)];
            const double temp = ndu[Idx(r)][Idx(j - 1)] / ndu[Idx(j)][Idx(r)];
            ndu[Idx(r)][Idx(j)] = saved + right[Idx(r + 1)] * temp;
            saved = left[Idx(j - r)] * temp;
        }
        ndu[Idx(j)][Idx(j)] = saved;
    }
    for (int j = 0; j <= degree; ++j) (*out)[0][Idx(j)] = ndu[Idx(j)][Idx(degree)];

    std::vector<std::vector<double>> a(2, std::vector<double>(Idx(degree + 1), 0.0));
    for (int r = 0; r <= degree; ++r) {
        int s1 = 0;
        int s2 = 1;
        a[0][0] = 1.0;
        for (int k = 1; k <= max_derivative; ++k) {
            double d = 0.0;
            const int rk = r - k;
            const int pk = degree - k;
            if (r >= k) {
                a[Idx(s2)][0] = a[Idx(s1)][0] / ndu[Idx(pk + 1)][Idx(rk)];
                d = a[Idx(s2)][0] * ndu[Idx(rk)][Idx(pk)];
            }
            const int j1 = (rk >= -1) ? 1 : -rk;
            const int j2 = (r - 1 <= pk) ? (k - 1) : (degree - r);
            for (int j = j1; j <= j2; ++j) {
                a[Idx(s2)][Idx(j)] =
                    (a[Idx(s1)][Idx(j)] - a[Idx(s1)][Idx(j - 1)]) / ndu[Idx(pk + 1)][Idx(rk + j)];
                d += a[Idx(s2)][Idx(j)] * ndu[Idx(rk + j)][Idx(pk)];
            }
            if (r <= pk) {
                a[Idx(s2)][Idx(k)] = -a[Idx(s1)][Idx(k - 1)] / ndu[Idx(pk + 1)][Idx(r)];
                d += a[Idx(s2)][Idx(k)] * ndu[Idx(r)][Idx(pk)];
            }
            (*out)[Idx(k)][Idx(r)] = d;
            std::swap(s1, s2);
        }
    }
    // The factor p!/(p-k)!, accumulated rather than computed per term.
    int factor = degree;
    for (int k = 1; k <= max_derivative; ++k) {
        for (int j = 0; j <= degree; ++j) (*out)[Idx(k)][Idx(j)] *= static_cast<double>(factor);
        factor *= (degree - k);
    }
}

double BasisFunctionDirect(int i, int degree, const std::vector<double> &knots, double u) {
    // The Cox-de Boor recurrence, evaluated literally. Exponential in
    // `degree` and written for clarity rather than speed -- it exists to
    // be an independent check on BasisFunctions above, so it deliberately
    // shares no code with it.
    const int m = static_cast<int>(knots.size()) - 1;
    if (i < 0 || i + degree + 1 > m) return 0.0;
    if (degree == 0) {
        // The half-open convention, with the domain's right endpoint
        // included in the last non-empty span so the basis still sums to
        // one there.
        if (u >= knots[Idx(i)] && u < knots[Idx(i + 1)]) return 1.0;
        if (u == knots[Idx(m)] && knots[Idx(i)] <= u && u < knots[Idx(m)]) return 1.0;
        // Right endpoint: belongs to the last span with non-zero width.
        if (u == knots[Idx(m)] && knots[Idx(i)] < knots[Idx(i + 1)] && knots[Idx(i + 1)] == knots[Idx(m)]) {
            return 1.0;
        }
        return 0.0;
    }
    // 0/0 is defined to be 0 here, the standard convention that makes
    // repeated knots work.
    const double left_denominator = knots[Idx(i + degree)] - knots[Idx(i)];
    const double right_denominator = knots[Idx(i + degree + 1)] - knots[Idx(i + 1)];
    double result = 0.0;
    if (left_denominator > 0.0) {
        result += (u - knots[Idx(i)]) / left_denominator * BasisFunctionDirect(i, degree - 1, knots, u);
    }
    if (right_denominator > 0.0) {
        result +=
            (knots[Idx(i + degree + 1)] - u) / right_denominator * BasisFunctionDirect(i + 1, degree - 1, knots, u);
    }
    return result;
}

// ---------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------

Vec4d CurvePointHomogeneous(int degree, const std::vector<double> &knots, const std::vector<Vec4d> &control,
                            double u) {
    const int count = static_cast<int>(control.size());
    const int span = FindSpan(degree, knots, u, count);
    if (span < 0) return Vec4d{0, 0, 0, 0};
    std::vector<double> basis(Idx(degree + 1), 0.0);
    BasisFunctions(span, u, degree, knots, basis.data());
    Vec4d point{0, 0, 0, 0};
    for (int i = 0; i <= degree; ++i) point += control[Idx(span - degree + i)] * basis[Idx(i)];
    return point;
}

void CurveDerivativesHomogeneous(int degree, const std::vector<double> &knots, const std::vector<Vec4d> &control,
                                 double u, int max_derivative, std::vector<Vec4d> *out) {
    // A3.2.
    out->assign(Idx(max_derivative + 1), Vec4d{0, 0, 0, 0});
    const int count = static_cast<int>(control.size());
    const int span = FindSpan(degree, knots, u, count);
    if (span < 0) return;
    // Derivatives above the degree are identically zero: the curve is a
    // piecewise polynomial of that degree and nothing more.
    const int computed = std::min(max_derivative, degree);
    std::vector<std::vector<double>> derivatives;
    BasisFunctionDerivatives(span, u, degree, computed, knots, &derivatives);
    for (int k = 0; k <= computed; ++k) {
        Vec4d sum{0, 0, 0, 0};
        for (int j = 0; j <= degree; ++j) {
            sum += control[Idx(span - degree + j)] * derivatives[Idx(k)][Idx(j)];
        }
        (*out)[Idx(k)] = sum;
    }
}

void RationalDerivatives(const std::vector<Vec4d> &homogeneous, int max_derivative, std::vector<Vec3d> *out) {
    // A4.2. The quotient rule, applied repeatedly: if C(u) = A(u)/w(u)
    // then
    //     C^(k) = (A^(k) - sum_{i=1..k} binom(k,i) w^(i) C^(k-i)) / w
    // Every lower-order derivative feeds forward, which is exactly why
    // the projection of the homogeneous derivative is not the answer --
    // that would be the i=0 term alone.
    out->assign(Idx(max_derivative + 1), Vec3d{});
    if (homogeneous.empty()) return;
    const double w0 = homogeneous[0].w;
    if (w0 == 0.0) return;
    const int available = static_cast<int>(homogeneous.size()) - 1;
    for (int k = 0; k <= max_derivative; ++k) {
        Vec3d value = (k <= available) ? homogeneous[Idx(k)].Weighted() : Vec3d{};
        for (int i = 1; i <= k; ++i) {
            const double wi = (i <= available) ? homogeneous[Idx(i)].w : 0.0;
            value -= (*out)[Idx(k - i)] * (Binomial(k, i) * wi);
        }
        (*out)[Idx(k)] = value / w0;
    }
}

// ---------------------------------------------------------------------
// Shape-preserving operations
// ---------------------------------------------------------------------

bool InsertKnot(int degree, std::vector<double> *knots, std::vector<Vec4d> *control, double u, int multiplicity) {
    // A5.1.
    if (multiplicity <= 0) return true;
    const int np = static_cast<int>(control->size()) - 1;
    const int mp = static_cast<int>(knots->size()) - 1;
    const int span = FindSpan(degree, *knots, u, np + 1);
    if (span < 0) return false;
    double lo = 0.0;
    double hi = 0.0;
    KnotDomain(*knots, degree, np + 1, &lo, &hi);
    if (u < lo || u > hi) return false;

    // Existing multiplicity of u.
    int s = 0;
    for (std::size_t i = 0; i < knots->size(); ++i) {
        if ((*knots)[i] == u) ++s;
    }
    // Inserting past multiplicity `degree` would break the curve into
    // disconnected pieces; clamp rather than refuse, since callers
    // (SplitCurve in particular) legitimately ask for "as much as
    // possible".
    const int r = std::min(multiplicity, degree - s);
    if (r <= 0) return true;

    const int k = span;
    // Sizes, carefully: `mp` and `np` are the last *indices* of the knot
    // and control arrays, not their lengths, so the new arrays run
    // 0..mp+r and 0..np+r -- that is mp+r+1 and np+r+1 entries. Allocating
    // one more than that leaves a zero control point on the end, which
    // evaluates correctly everywhere (nothing references it) and so hides
    // from any check that only compares curve points.
    std::vector<double> new_knots(Idx(mp + r + 1), 0.0);
    std::vector<Vec4d> new_control(Idx(np + r + 1), Vec4d{0, 0, 0, 0});

    for (int i = 0; i <= k; ++i) new_knots[Idx(i)] = (*knots)[Idx(i)];
    for (int i = 1; i <= r; ++i) new_knots[Idx(k + i)] = u;
    for (int i = k + 1; i <= mp; ++i) new_knots[Idx(i + r)] = (*knots)[Idx(i)];

    for (int i = 0; i <= k - degree; ++i) new_control[Idx(i)] = (*control)[Idx(i)];
    for (int i = k - s; i <= np; ++i) new_control[Idx(i + r)] = (*control)[Idx(i)];

    std::vector<Vec4d> temp(Idx(degree - s + 1), Vec4d{0, 0, 0, 0});
    for (int i = 0; i <= degree - s; ++i) temp[Idx(i)] = (*control)[Idx(k - degree + i)];

    int l = 0;
    for (int j = 1; j <= r; ++j) {
        l = k - degree + j;
        for (int i = 0; i <= degree - j - s; ++i) {
            const double denominator = (*knots)[Idx(i + k + 1)] - (*knots)[Idx(l + i)];
            const double alpha = (denominator != 0.0) ? (u - (*knots)[Idx(l + i)]) / denominator : 0.0;
            temp[Idx(i)] = temp[Idx(i + 1)] * alpha + temp[Idx(i)] * (1.0 - alpha);
        }
        new_control[Idx(l)] = temp[0];
        new_control[Idx(k + r - j - s)] = temp[Idx(degree - j - s)];
    }
    for (int i = l + 1; i < k - s; ++i) new_control[Idx(i)] = temp[Idx(i - l)];

    *knots = std::move(new_knots);
    *control = std::move(new_control);
    return true;
}

bool RefineKnots(int degree, std::vector<double> *knots, std::vector<Vec4d> *control,
                 const std::vector<double> &new_knots) {
    // A5.4. Repeated single insertion would be O(r * n); this is O(n + r).
    if (new_knots.empty()) return true;
    std::vector<double> sorted = new_knots;
    std::sort(sorted.begin(), sorted.end());

    const int n = static_cast<int>(control->size()) - 1;
    const int m = n + degree + 1;
    const int r = static_cast<int>(sorted.size()) - 1;
    const int a = FindSpan(degree, *knots, sorted[0], n + 1);
    int b = FindSpan(degree, *knots, sorted[Idx(r)], n + 1);
    if (a < 0 || b < 0) return false;
    b = b + 1;

    std::vector<Vec4d> qw(Idx(n + r + 2), Vec4d{0, 0, 0, 0});
    std::vector<double> ubar(Idx(m + r + 2), 0.0);
    for (int j = 0; j <= a - degree; ++j) qw[Idx(j)] = (*control)[Idx(j)];
    for (int j = b - 1; j <= n; ++j) qw[Idx(j + r + 1)] = (*control)[Idx(j)];
    for (int j = 0; j <= a; ++j) ubar[Idx(j)] = (*knots)[Idx(j)];
    for (int j = b + degree; j <= m; ++j) ubar[Idx(j + r + 1)] = (*knots)[Idx(j)];

    int i = b + degree - 1;
    int k = b + degree + r;
    for (int j = r; j >= 0; --j) {
        while (sorted[Idx(j)] <= (*knots)[Idx(i)] && i > a) {
            qw[Idx(k - degree - 1)] = (*control)[Idx(i - degree - 1)];
            ubar[Idx(k)] = (*knots)[Idx(i)];
            --k;
            --i;
        }
        qw[Idx(k - degree - 1)] = qw[Idx(k - degree)];
        for (int l = 1; l <= degree; ++l) {
            const int index = k - degree + l;
            double alpha = ubar[Idx(k + l)] - sorted[Idx(j)];
            if (std::fabs(alpha) == 0.0) {
                qw[Idx(index - 1)] = qw[Idx(index)];
            } else {
                alpha /= (ubar[Idx(k + l)] - (*knots)[Idx(i - degree + l)]);
                qw[Idx(index - 1)] = qw[Idx(index - 1)] * alpha + qw[Idx(index)] * (1.0 - alpha);
            }
        }
        ubar[Idx(k)] = sorted[Idx(j)];
        --k;
    }
    *knots = std::move(ubar);
    *control = std::move(qw);
    return true;
}

int RemoveKnot(int degree, std::vector<double> *knots, std::vector<Vec4d> *control, double u, int times,
               double tolerance) {
    // A5.8. Unlike every other operation here, removal is conditional:
    // a knot can only come out if the curve can still be represented
    // without it to within `tolerance`. The algorithm runs the insertion
    // recurrence backwards and checks, at each step, whether the two
    // reconstructions of the same control point agree.
    const int n = static_cast<int>(control->size()) - 1;
    const int m = n + degree + 1;
    int r = -1;
    for (int i = 0; i <= m; ++i) {
        if ((*knots)[Idx(i)] == u) r = i;
    }
    if (r < 0) return 0;
    int s = 0;
    for (int i = 0; i <= m; ++i) {
        if ((*knots)[Idx(i)] == u) ++s;
    }
    double lo = 0.0;
    double hi = 0.0;
    KnotDomain(*knots, degree, n + 1, &lo, &hi);
    if (u <= lo || u >= hi) return 0;  // end knots keep the curve clamped

    const int order = degree + 1;
    const int fout = (2 * r - s - degree) / 2;
    int last = r - s;
    int first = r - degree;
    std::vector<Vec4d> temp(Idx(2 * degree + 1), Vec4d{0, 0, 0, 0});

    int removed = 0;
    for (int t = 0; t < times; ++t) {
        const int off = first - 1;
        temp[0] = (*control)[Idx(off)];
        temp[Idx(last + 1 - off)] = (*control)[Idx(last + 1)];
        int i = first;
        int j = last;
        int ii = 1;
        int jj = last - off;
        bool can_remove = false;
        while (j - i > t) {
            const double alfi = (u - (*knots)[Idx(i)]) / ((*knots)[Idx(i + order + t)] - (*knots)[Idx(i)]);
            const double alfj = (u - (*knots)[Idx(j - t)]) / ((*knots)[Idx(j + order)] - (*knots)[Idx(j - t)]);
            temp[Idx(ii)] = ((*control)[Idx(i)] - temp[Idx(ii - 1)] * (1.0 - alfi)) * (1.0 / alfi);
            temp[Idx(jj)] = ((*control)[Idx(j)] - temp[Idx(jj + 1)] * alfj) * (1.0 / (1.0 - alfj));
            ++i;
            ++ii;
            --j;
            --jj;
        }
        if (j - i < t) {
            can_remove = Distance4D(temp[Idx(ii - 1)], temp[Idx(jj + 1)]) <= tolerance;
        } else {
            const double alfi = (u - (*knots)[Idx(i)]) / ((*knots)[Idx(i + order + t)] - (*knots)[Idx(i)]);
            const Vec4d reconstructed = temp[Idx(ii + t + 1)] * alfi + temp[Idx(ii - 1)] * (1.0 - alfi);
            can_remove = Distance4D((*control)[Idx(i)], reconstructed) <= tolerance;
        }
        if (!can_remove) break;

        i = first;
        j = last;
        while (j - i > t) {
            (*control)[Idx(i)] = temp[Idx(i - off)];
            (*control)[Idx(j)] = temp[Idx(j - off)];
            ++i;
            --j;
        }
        --first;
        ++last;
        ++removed;
    }
    if (removed == 0) return 0;

    for (int k = r + 1; k <= m; ++k) (*knots)[Idx(k - removed)] = (*knots)[Idx(k)];
    knots->resize(Idx(m + 1 - removed));

    int j = fout;
    int i = j;
    for (int k = 1; k < removed; ++k) {
        if (k % 2 == 1) {
            ++i;
        } else {
            --j;
        }
    }
    for (int k = i + 1; k <= n; ++k) {
        (*control)[Idx(j)] = (*control)[Idx(k)];
        ++j;
    }
    control->resize(Idx(n + 1 - removed));
    return removed;
}

bool ElevateDegree(int *degree, std::vector<double> *knots, std::vector<Vec4d> *control, int times) {
    // A5.9. The longest algorithm in the book, and the shape of it is:
    // decompose into Bezier segments, elevate each one (which for a
    // Bezier is a simple binomial recombination), then remove the
    // artificial knots that the decomposition introduced. The bookkeeping
    // is doing all three at once in a single pass, which is why it looks
    // the way it does.
    if (times <= 0) return true;
    const int p = *degree;
    const int n = static_cast<int>(control->size()) - 1;
    const int m = n + p + 1;
    const int ph = p + times;
    const int ph2 = ph / 2;

    std::vector<std::vector<double>> bezalfs(Idx(ph + 1), std::vector<double>(Idx(p + 1), 0.0));
    bezalfs[0][0] = 1.0;
    bezalfs[Idx(ph)][Idx(p)] = 1.0;
    for (int i = 1; i <= ph2; ++i) {
        const double inv = 1.0 / Binomial(ph, i);
        const int mpi = std::min(p, i);
        for (int j = std::max(0, i - times); j <= mpi; ++j) {
            bezalfs[Idx(i)][Idx(j)] = inv * Binomial(p, j) * Binomial(times, i - j);
        }
    }
    for (int i = ph2 + 1; i <= ph - 1; ++i) {
        const int mpi = std::min(p, i);
        for (int j = std::max(0, i - times); j <= mpi; ++j) {
            bezalfs[Idx(i)][Idx(j)] = bezalfs[Idx(ph - i)][Idx(p - j)];
        }
    }

    // Generous upper bounds: each of the (at most n) knot spans can add
    // `times` knots, plus the elevated end clamps.
    const std::size_t capacity = Idx(n + 1) * Idx(times + 1) + Idx(2 * (ph + 1)) + 8;
    std::vector<Vec4d> qw(capacity, Vec4d{0, 0, 0, 0});
    std::vector<double> uh(capacity + Idx(ph + 1), 0.0);
    std::vector<Vec4d> bpts(Idx(p + 1), Vec4d{0, 0, 0, 0});
    std::vector<Vec4d> ebpts(Idx(ph + 1), Vec4d{0, 0, 0, 0});
    std::vector<Vec4d> next_bpts(Idx(p - 1 > 0 ? p - 1 : 1), Vec4d{0, 0, 0, 0});
    std::vector<double> alfs(Idx(p > 0 ? p : 1), 0.0);

    int mh = ph;
    int kind = ph + 1;
    int r = -1;
    int a = p;
    int b = p + 1;
    int cind = 1;
    double ua = (*knots)[0];
    qw[0] = (*control)[0];
    for (int i = 0; i <= ph; ++i) uh[Idx(i)] = ua;
    for (int i = 0; i <= p; ++i) bpts[Idx(i)] = (*control)[Idx(i)];

    while (b < m) {
        int i = b;
        while (b < m && (*knots)[Idx(b)] == (*knots)[Idx(b + 1)]) ++b;
        const int mul = b - i + 1;
        mh = mh + mul + times;
        const double ub = (*knots)[Idx(b)];
        const int oldr = r;
        r = p - mul;
        const int lbz = (oldr > 0) ? ((oldr + 2) / 2) : 1;
        const int rbz = (r > 0) ? (ph - (r + 1) / 2) : ph;

        if (r > 0) {
            const double numer = ub - ua;
            for (int k = p; k > mul; --k) alfs[Idx(k - mul - 1)] = numer / ((*knots)[Idx(a + k)] - ua);
            for (int j = 1; j <= r; ++j) {
                const int save = r - j;
                const int sj = mul + j;
                for (int k = p; k >= sj; --k) {
                    bpts[Idx(k)] = bpts[Idx(k)] * alfs[Idx(k - sj)] + bpts[Idx(k - 1)] * (1.0 - alfs[Idx(k - sj)]);
                }
                next_bpts[Idx(save)] = bpts[Idx(p)];
            }
        }
        for (int e = lbz; e <= ph; ++e) {
            ebpts[Idx(e)] = Vec4d{0, 0, 0, 0};
            const int mpi = std::min(p, e);
            for (int j = std::max(0, e - times); j <= mpi; ++j) {
                ebpts[Idx(e)] += bpts[Idx(j)] * bezalfs[Idx(e)][Idx(j)];
            }
        }

        if (oldr > 1) {
            int first = kind - 2;
            int last = kind;
            const double den = ub - ua;
            const double bet = (ub - uh[Idx(kind - 1)]) / den;
            for (int tr = 1; tr < oldr; ++tr) {
                int ii = first;
                int jj = last;
                int kj = jj - kind + 1;
                while (jj - ii > tr) {
                    if (ii < cind) {
                        const double alf = (ub - uh[Idx(ii)]) / (ua - uh[Idx(ii)]);
                        qw[Idx(ii)] = qw[Idx(ii)] * alf + qw[Idx(ii - 1)] * (1.0 - alf);
                    }
                    if (jj >= lbz) {
                        if (jj - tr <= kind - ph + oldr) {
                            const double gam = (ub - uh[Idx(jj - tr)]) / den;
                            ebpts[Idx(kj)] = ebpts[Idx(kj)] * gam + ebpts[Idx(kj + 1)] * (1.0 - gam);
                        } else {
                            ebpts[Idx(kj)] = ebpts[Idx(kj)] * bet + ebpts[Idx(kj + 1)] * (1.0 - bet);
                        }
                    }
                    ++ii;
                    --jj;
                    --kj;
                }
                --first;
                ++last;
            }
        }

        if (a != p) {
            for (int e = 0; e < ph - oldr; ++e) {
                uh[Idx(kind)] = ua;
                ++kind;
            }
        }
        for (int j = lbz; j <= rbz; ++j) {
            qw[Idx(cind)] = ebpts[Idx(j)];
            ++cind;
        }
        if (b < m) {
            for (int j = 0; j < r; ++j) bpts[Idx(j)] = next_bpts[Idx(j)];
            for (int j = r; j <= p; ++j) bpts[Idx(j)] = (*control)[Idx(b - p + j)];
            a = b;
            ++b;
            ua = ub;
        } else {
            for (int e = 0; e <= ph; ++e) uh[Idx(kind + e)] = ub;
        }
    }

    const int nh = mh - ph - 1;
    control->assign(qw.begin(), qw.begin() + static_cast<std::ptrdiff_t>(Idx(nh + 1)));
    knots->assign(uh.begin(), uh.begin() + static_cast<std::ptrdiff_t>(Idx(nh + ph + 2)));
    *degree = ph;
    return true;
}

bool SplitCurve(int degree, const std::vector<double> &knots, const std::vector<Vec4d> &control, double u,
                std::vector<double> *left_knots, std::vector<Vec4d> *left_control,
                std::vector<double> *right_knots, std::vector<Vec4d> *right_control) {
    double lo = 0.0;
    double hi = 0.0;
    KnotDomain(knots, degree, static_cast<int>(control.size()), &lo, &hi);
    if (!(u > lo && u < hi)) return false;

    // Raise u to full multiplicity: the curve then has degree+1 coincident
    // knots there, which is exactly a clamped boundary, so the two halves
    // can simply be read off.
    std::vector<double> work_knots = knots;
    std::vector<Vec4d> work_control = control;
    int existing = 0;
    for (double k : knots) {
        if (k == u) ++existing;
    }
    if (!InsertKnot(degree, &work_knots, &work_control, u, degree - existing)) return false;

    // The split index: the first control point of the right half.
    const int span = FindSpan(degree, work_knots, u, static_cast<int>(work_control.size()));
    const int split = span - degree;

    left_control->assign(work_control.begin(), work_control.begin() + static_cast<std::ptrdiff_t>(Idx(split + 1)));
    left_knots->assign(work_knots.begin(), work_knots.begin() + static_cast<std::ptrdiff_t>(Idx(split + degree + 1)));
    left_knots->push_back(u);

    right_control->assign(work_control.begin() + static_cast<std::ptrdiff_t>(Idx(split)), work_control.end());
    right_knots->assign(Idx(degree + 1), u);
    right_knots->insert(right_knots->end(),
                        work_knots.begin() + static_cast<std::ptrdiff_t>(Idx(split + degree + 1)), work_knots.end());
    return true;
}

void ReverseCurve(std::vector<double> *knots, std::vector<Vec4d> *control) {
    // Reflect the knot vector about the middle of its own domain, so the
    // reversed curve keeps the same parameter range rather than a mirrored
    // one -- callers overwhelmingly want [a,b] to stay [a,b].
    const double lo = knots->front();
    const double hi = knots->back();
    std::vector<double> reversed(knots->size(), 0.0);
    for (std::size_t i = 0; i < knots->size(); ++i) {
        reversed[i] = lo + hi - (*knots)[knots->size() - 1 - i];
    }
    *knots = std::move(reversed);
    std::reverse(control->begin(), control->end());
}

BezierSegments DecomposeCurve(int degree, const std::vector<double> &knots, const std::vector<Vec4d> &control) {
    // A5.6.
    BezierSegments result;
    result.degree = degree;
    const int n = static_cast<int>(control.size()) - 1;
    const int m = n + degree + 1;
    if (degree < 1 || n < degree) return result;

    double lo = 0.0;
    double hi = 0.0;
    KnotDomain(knots, degree, n + 1, &lo, &hi);

    int a = degree;
    int b = degree + 1;
    std::vector<std::vector<Vec4d>> segments;
    std::vector<Vec4d> current(Idx(degree + 1), Vec4d{0, 0, 0, 0});
    std::vector<Vec4d> next(Idx(degree + 1), Vec4d{0, 0, 0, 0});
    std::vector<double> alphas(Idx(degree > 0 ? degree : 1), 0.0);
    for (int i = 0; i <= degree; ++i) current[Idx(i)] = control[Idx(i)];
    result.breakpoints.push_back(lo);

    while (b < m) {
        int i = b;
        while (b < m && knots[Idx(b + 1)] == knots[Idx(b)]) ++b;
        const int multiplicity = b - i + 1;
        if (multiplicity < degree) {
            const double numer = knots[Idx(b)] - knots[Idx(a)];
            for (int j = degree; j > multiplicity; --j) {
                alphas[Idx(j - multiplicity - 1)] = numer / (knots[Idx(a + j)] - knots[Idx(a)]);
            }
            const int r = degree - multiplicity;
            for (int j = 1; j <= r; ++j) {
                const int save = r - j;
                const int s = multiplicity + j;
                for (int k = degree; k >= s; --k) {
                    const double alpha = alphas[Idx(k - s)];
                    current[Idx(k)] = current[Idx(k)] * alpha + current[Idx(k - 1)] * (1.0 - alpha);
                }
                if (b < m) next[Idx(save)] = current[Idx(degree)];
            }
        }
        segments.push_back(current);
        result.breakpoints.push_back(knots[Idx(b)]);
        if (b < m) {
            for (int j = degree - multiplicity; j <= degree; ++j) current[Idx(j)] = control[Idx(b - degree + j)];
            for (int j = 0; j < degree - multiplicity; ++j) current[Idx(j)] = next[Idx(j)];
            a = b;
            ++b;
        }
    }
    // No trailing push: the book's `nb = nb+1` happens *inside* the loop,
    // so the final segment has already been emitted by the time `b`
    // reaches `m` and the loop exits. Appending here as well would
    // duplicate the last segment -- and because that duplicate is a
    // perfectly valid Bezier covering a zero-width parameter interval, it
    // would reconstruct the curve correctly and hide itself from any
    // check that only compares evaluated points.
    if (result.breakpoints.back() != hi) result.breakpoints.push_back(hi);
    result.segments = std::move(segments);
    return result;
}

// ---------------------------------------------------------------------
// Fitting
// ---------------------------------------------------------------------

std::vector<double> ComputeParameters(const std::vector<Vec3d> &points, Parameterization kind) {
    const std::size_t count = points.size();
    std::vector<double> parameters(count, 0.0);
    if (count < 2) return parameters;
    if (kind == Parameterization::Uniform) {
        for (std::size_t i = 0; i < count; ++i) {
            parameters[i] = static_cast<double>(i) / static_cast<double>(count - 1);
        }
        return parameters;
    }
    // Chord length uses the distance itself; centripetal its square root.
    // The square root is Lee's rule, and the reason it is the default is
    // narrow but important: where the data turns sharply, chord-length
    // parameterization gives the long chord a large parameter interval,
    // and the interpolating curve overshoots into a visible loop.
    // Centripetal damps that.
    std::vector<double> distances(count, 0.0);
    double total = 0.0;
    for (std::size_t i = 1; i < count; ++i) {
        const double d = (points[i] - points[i - 1]).Length();
        distances[i] = (kind == Parameterization::Centripetal) ? std::sqrt(d) : d;
        total += distances[i];
    }
    if (total <= 0.0) {
        for (std::size_t i = 0; i < count; ++i) {
            parameters[i] = static_cast<double>(i) / static_cast<double>(count - 1);
        }
        return parameters;
    }
    double accumulated = 0.0;
    for (std::size_t i = 1; i + 1 < count; ++i) {
        accumulated += distances[i];
        parameters[i] = accumulated / total;
    }
    parameters[count - 1] = 1.0;
    return parameters;
}

bool InterpolateCurve(const std::vector<Vec3d> &points, int degree, Parameterization parameterization,
                      std::vector<double> *out_knots, std::vector<Vec4d> *out_control) {
    return InterpolateCurveWithParameters(points, degree, ComputeParameters(points, parameterization), out_knots,
                                          out_control);
}

bool InterpolateCurveWithParameters(const std::vector<Vec3d> &points, int degree,
                                    const std::vector<double> &parameters, std::vector<double> *out_knots,
                                    std::vector<Vec4d> *out_control) {
    std::vector<Vec4d> weighted;
    weighted.reserve(points.size());
    for (const Vec3d &p : points) weighted.push_back(Vec4d::FromWeighted(p, 1.0));
    return InterpolateHomogeneous(weighted, degree, parameters, out_knots, out_control);
}

bool InterpolateHomogeneous(const std::vector<Vec4d> &points, int degree, const std::vector<double> &parameters,
                            std::vector<double> *out_knots, std::vector<Vec4d> *out_control) {
    // A9.1, in homogeneous coordinates.
    //
    // Interpolating the *weighted* coordinates (wx, wy, wz, w) rather
    // than the Euclidean ones is what makes rational skinning work. A
    // circle is a rational quadratic whose middle control points carry
    // weight cos(delta/2); projecting to 3D first and interpolating
    // there discards those weights, and the resulting surface is a
    // polynomial through the circle's control points -- visibly not a
    // circle, and wrong by about 6% of the radius for a quarter span.
    const int n = static_cast<int>(points.size()) - 1;
    if (degree < 1 || n < degree) return false;
    if (parameters.size() != points.size()) return false;

    // Knots by averaging, which is what guarantees the collocation matrix
    // is non-singular (de Boor's theorem: every basis function is
    // non-zero at its own parameter).
    std::vector<double> knots(Idx(n + degree + 2), 0.0);
    for (int i = 0; i <= degree; ++i) knots[Idx(i)] = 0.0;
    for (int i = 0; i <= degree; ++i) knots[Idx(n + 1 + i)] = 1.0;
    for (int j = 1; j <= n - degree; ++j) {
        double sum = 0.0;
        for (int i = j; i <= j + degree - 1; ++i) sum += parameters[Idx(i)];
        knots[Idx(j + degree)] = sum / static_cast<double>(degree);
    }

    const int size = n + 1;
    std::vector<double> matrix(Idx(size) * Idx(size), 0.0);
    std::vector<Vec4d> rhs(Idx(size), Vec4d{});
    std::vector<double> basis(Idx(degree + 1), 0.0);
    for (int i = 0; i <= n; ++i) {
        const int span = FindSpan(degree, knots, parameters[Idx(i)], size);
        if (span < 0) return false;
        BasisFunctions(span, parameters[Idx(i)], degree, knots, basis.data());
        for (int j = 0; j <= degree; ++j) matrix[Idx(i * size + span - degree + j)] = basis[Idx(j)];
        rhs[Idx(i)] = points[Idx(i)];
    }
    if (!SolveBanded(matrix, size, degree, rhs)) return false;
    *out_control = std::move(rhs);
    *out_knots = std::move(knots);
    return true;
}

bool ApproximateCurve(const std::vector<Vec3d> &points, int degree, int control_point_count,
                      Parameterization parameterization, std::vector<double> *out_knots,
                      std::vector<Vec4d> *out_control) {
    // Least squares with the endpoints interpolated exactly, following the
    // book's A9.6 in structure but solving with Householder QR rather than
    // the normal equations. The normal equations square the condition
    // number, and a dense sampling of a nearly-straight curve -- exactly
    // what the intersection marcher produces -- is where that starts to
    // matter.
    //
    // The result is non-rational (all weights 1). Fitting the weights too
    // is a genuinely different, nonlinear problem, and every standard
    // approximation routine leaves them alone for that reason.
    const int m = static_cast<int>(points.size()) - 1;
    const int n = control_point_count - 1;
    if (degree < 1 || n < degree || m < n) return false;

    const std::vector<double> parameters = ComputeParameters(points, parameterization);
    std::vector<double> knots(Idx(n + degree + 2), 0.0);
    for (int i = 0; i <= degree; ++i) knots[Idx(i)] = 0.0;
    for (int i = 0; i <= degree; ++i) knots[Idx(n + 1 + i)] = 1.0;
    // Knot placement spreads the interior knots over the data's own
    // parameter distribution, so spans carry roughly equal numbers of
    // points.
    const double d = static_cast<double>(m + 1) / static_cast<double>(n - degree + 1);
    for (int j = 1; j <= n - degree; ++j) {
        const double fd = static_cast<double>(j) * d;
        const int i = static_cast<int>(fd);
        const double alpha = fd - static_cast<double>(i);
        const int lo = std::max(0, std::min(m, i - 1));
        const int hi = std::max(0, std::min(m, i));
        knots[Idx(j + degree)] = (1.0 - alpha) * parameters[Idx(lo)] + alpha * parameters[Idx(hi)];
    }

    // Unknowns are the interior control points; the two ends are fixed.
    const int unknowns = n - 1;
    if (unknowns <= 0) {
        out_control->assign(Idx(control_point_count), Vec4d{});
        (*out_control)[0] = Vec4d::FromWeighted(points.front(), 1.0);
        (*out_control)[Idx(n)] = Vec4d::FromWeighted(points.back(), 1.0);
        *out_knots = std::move(knots);
        return true;
    }
    const int rows = m - 1;
    if (rows < unknowns) return false;

    MatrixNd design(Idx(rows), Idx(unknowns));
    std::vector<std::vector<double>> residual(3, std::vector<double>(Idx(rows), 0.0));
    std::vector<double> basis(Idx(degree + 1), 0.0);
    for (int k = 1; k <= m - 1; ++k) {
        const int span = FindSpan(degree, knots, parameters[Idx(k)], control_point_count);
        if (span < 0) return false;
        BasisFunctions(span, parameters[Idx(k)], degree, knots, basis.data());
        double n0 = 0.0;
        double nn = 0.0;
        for (int j = 0; j <= degree; ++j) {
            const int column = span - degree + j;
            if (column == 0) {
                n0 = basis[Idx(j)];
            } else if (column == n) {
                nn = basis[Idx(j)];
            } else if (column > 0 && column < n) {
                design(Idx(k - 1), Idx(column - 1)) = basis[Idx(j)];
            }
        }
        // Move the two fixed endpoints' contributions to the right side.
        const Vec3d target = points[Idx(k)] - points.front() * n0 - points.back() * nn;
        residual[0][Idx(k - 1)] = target.x;
        residual[1][Idx(k - 1)] = target.y;
        residual[2][Idx(k - 1)] = target.z;
    }

    out_control->assign(Idx(control_point_count), Vec4d{});
    (*out_control)[0] = Vec4d::FromWeighted(points.front(), 1.0);
    (*out_control)[Idx(n)] = Vec4d::FromWeighted(points.back(), 1.0);
    std::vector<std::vector<double>> solved(3);
    for (int c = 0; c < 3; ++c) {
        if (!design.SolveLeastSquares(residual[Idx(c)], &solved[Idx(c)])) return false;
    }
    for (int i = 0; i < unknowns; ++i) {
        const Vec3d p{solved[0][Idx(i)], solved[1][Idx(i)], solved[2][Idx(i)]};
        (*out_control)[Idx(i + 1)] = Vec4d::FromWeighted(p, 1.0);
    }
    *out_knots = std::move(knots);
    return true;
}

// ---------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------

Vec4d SurfacePointHomogeneous(int degree_u, int degree_v, const std::vector<double> &knots_u,
                              const std::vector<double> &knots_v, const std::vector<Vec4d> &control, int count_u,
                              int count_v, double u, double v) {
    // A3.5. Evaluate in v first for each of the degree_u+1 relevant rows,
    // then combine those in u -- the tensor product reduced to two curve
    // evaluations rather than a double sum over the whole grid.
    const int span_u = FindSpan(degree_u, knots_u, u, count_u);
    const int span_v = FindSpan(degree_v, knots_v, v, count_v);
    if (span_u < 0 || span_v < 0) return Vec4d{0, 0, 0, 0};
    std::vector<double> basis_u(Idx(degree_u + 1), 0.0);
    std::vector<double> basis_v(Idx(degree_v + 1), 0.0);
    BasisFunctions(span_u, u, degree_u, knots_u, basis_u.data());
    BasisFunctions(span_v, v, degree_v, knots_v, basis_v.data());
    Vec4d point{0, 0, 0, 0};
    for (int i = 0; i <= degree_u; ++i) {
        Vec4d row{0, 0, 0, 0};
        for (int j = 0; j <= degree_v; ++j) {
            row += control[SurfaceIndex(span_u - degree_u + i, span_v - degree_v + j, count_v)] * basis_v[Idx(j)];
        }
        point += row * basis_u[Idx(i)];
    }
    return point;
}

void SurfaceDerivativesHomogeneous(int degree_u, int degree_v, const std::vector<double> &knots_u,
                                   const std::vector<double> &knots_v, const std::vector<Vec4d> &control,
                                   int count_u, int count_v, double u, double v, int max_derivative,
                                   std::vector<std::vector<Vec4d>> *out) {
    // A3.6.
    out->assign(Idx(max_derivative + 1), std::vector<Vec4d>(Idx(max_derivative + 1), Vec4d{0, 0, 0, 0}));
    const int span_u = FindSpan(degree_u, knots_u, u, count_u);
    const int span_v = FindSpan(degree_v, knots_v, v, count_v);
    if (span_u < 0 || span_v < 0) return;
    const int du = std::min(max_derivative, degree_u);
    const int dv = std::min(max_derivative, degree_v);
    std::vector<std::vector<double>> ders_u;
    std::vector<std::vector<double>> ders_v;
    BasisFunctionDerivatives(span_u, u, degree_u, du, knots_u, &ders_u);
    BasisFunctionDerivatives(span_v, v, degree_v, dv, knots_v, &ders_v);

    std::vector<Vec4d> temp(Idx(degree_v + 1), Vec4d{0, 0, 0, 0});
    for (int k = 0; k <= du; ++k) {
        for (int s = 0; s <= degree_v; ++s) {
            temp[Idx(s)] = Vec4d{0, 0, 0, 0};
            for (int r = 0; r <= degree_u; ++r) {
                temp[Idx(s)] +=
                    control[SurfaceIndex(span_u - degree_u + r, span_v - degree_v + s, count_v)] *
                    ders_u[Idx(k)][Idx(r)];
            }
        }
        const int dd = std::min(max_derivative - k, dv);
        for (int l = 0; l <= dd; ++l) {
            Vec4d sum{0, 0, 0, 0};
            for (int s = 0; s <= degree_v; ++s) sum += temp[Idx(s)] * ders_v[Idx(l)][Idx(s)];
            (*out)[Idx(k)][Idx(l)] = sum;
        }
    }
}

void RationalSurfaceDerivatives(const std::vector<std::vector<Vec4d>> &homogeneous, int max_derivative,
                                std::vector<std::vector<Vec3d>> *out) {
    // A4.4. Same quotient-rule correction as the curve case, now summed
    // over both indices -- the inner double loop subtracts every already-
    // computed lower-order partial weighted by the matching weight
    // derivative.
    out->assign(Idx(max_derivative + 1), std::vector<Vec3d>(Idx(max_derivative + 1), Vec3d{}));
    if (homogeneous.empty() || homogeneous[0].empty()) return;
    const double w00 = homogeneous[0][0].w;
    if (w00 == 0.0) return;
    const int available = static_cast<int>(homogeneous.size()) - 1;
    auto weight_at = [&homogeneous, available](int i, int j) {
        if (i > available || j > available) return 0.0;
        if (Idx(j) >= homogeneous[Idx(i)].size()) return 0.0;
        return homogeneous[Idx(i)][Idx(j)].w;
    };
    for (int k = 0; k <= max_derivative; ++k) {
        for (int l = 0; l <= max_derivative - k; ++l) {
            Vec3d value;
            if (k <= available && Idx(l) < homogeneous[Idx(k)].size()) {
                value = homogeneous[Idx(k)][Idx(l)].Weighted();
            }
            for (int j = 1; j <= l; ++j) {
                value -= (*out)[Idx(k)][Idx(l - j)] * (Binomial(l, j) * weight_at(0, j));
            }
            for (int i = 1; i <= k; ++i) {
                value -= (*out)[Idx(k - i)][Idx(l)] * (Binomial(k, i) * weight_at(i, 0));
                Vec3d inner;
                for (int j = 1; j <= l; ++j) {
                    inner += (*out)[Idx(k - i)][Idx(l - j)] * (Binomial(l, j) * weight_at(i, j));
                }
                value -= inner * Binomial(k, i);
            }
            (*out)[Idx(k)][Idx(l)] = value / w00;
        }
    }
}

bool SurfaceIsoCurve(int degree_u, int degree_v, const std::vector<double> &knots_u,
                     const std::vector<double> &knots_v, const std::vector<Vec4d> &control, int count_u,
                     int count_v, bool fix_u, double fixed_value, int *out_degree,
                     std::vector<double> *out_knots, std::vector<Vec4d> *out_control) {
    // Holding one parameter fixed collapses the tensor product to a curve
    // in the other, whose control points are the fixed direction's basis
    // functions applied down each row (or column) of the control grid.
    if (fix_u) {
        const int span = FindSpan(degree_u, knots_u, fixed_value, count_u);
        if (span < 0) return false;
        std::vector<double> basis(Idx(degree_u + 1), 0.0);
        BasisFunctions(span, fixed_value, degree_u, knots_u, basis.data());
        out_control->assign(Idx(count_v), Vec4d{0, 0, 0, 0});
        for (int j = 0; j < count_v; ++j) {
            Vec4d sum{0, 0, 0, 0};
            for (int i = 0; i <= degree_u; ++i) {
                sum += control[SurfaceIndex(span - degree_u + i, j, count_v)] * basis[Idx(i)];
            }
            (*out_control)[Idx(j)] = sum;
        }
        *out_knots = knots_v;
        *out_degree = degree_v;
        return true;
    }
    const int span = FindSpan(degree_v, knots_v, fixed_value, count_v);
    if (span < 0) return false;
    std::vector<double> basis(Idx(degree_v + 1), 0.0);
    BasisFunctions(span, fixed_value, degree_v, knots_v, basis.data());
    out_control->assign(Idx(count_u), Vec4d{0, 0, 0, 0});
    for (int i = 0; i < count_u; ++i) {
        Vec4d sum{0, 0, 0, 0};
        for (int j = 0; j <= degree_v; ++j) {
            sum += control[SurfaceIndex(i, span - degree_v + j, count_v)] * basis[Idx(j)];
        }
        (*out_control)[Idx(i)] = sum;
    }
    *out_knots = knots_u;
    *out_degree = degree_u;
    return true;
}

bool InsertKnotSurface(int degree_u, int degree_v, std::vector<double> *knots_u, std::vector<double> *knots_v,
                       std::vector<Vec4d> *control, int *count_u, int *count_v, bool in_u, double value,
                       int multiplicity) {
    // A5.3, expressed as repeated curve insertion: inserting into u is the
    // same operation applied independently down each column of the control
    // grid, with the u knot vector updated once.
    if (multiplicity <= 0) return true;
    if (in_u) {
        std::vector<double> updated_knots;
        std::vector<Vec4d> updated;
        int new_count = 0;
        for (int j = 0; j < *count_v; ++j) {
            std::vector<Vec4d> column(Idx(*count_u), Vec4d{});
            for (int i = 0; i < *count_u; ++i) column[Idx(i)] = (*control)[SurfaceIndex(i, j, *count_v)];
            std::vector<double> column_knots = *knots_u;
            if (!InsertKnot(degree_u, &column_knots, &column, value, multiplicity)) return false;
            if (j == 0) {
                new_count = static_cast<int>(column.size());
                updated.assign(Idx(new_count) * Idx(*count_v), Vec4d{});
                updated_knots = column_knots;
            }
            for (int i = 0; i < new_count; ++i) updated[SurfaceIndex(i, j, *count_v)] = column[Idx(i)];
        }
        *control = std::move(updated);
        *knots_u = std::move(updated_knots);
        *count_u = new_count;
        return true;
    }
    std::vector<double> updated_knots;
    std::vector<Vec4d> updated;
    int new_count = 0;
    for (int i = 0; i < *count_u; ++i) {
        std::vector<Vec4d> row(Idx(*count_v), Vec4d{});
        for (int j = 0; j < *count_v; ++j) row[Idx(j)] = (*control)[SurfaceIndex(i, j, *count_v)];
        std::vector<double> row_knots = *knots_v;
        if (!InsertKnot(degree_v, &row_knots, &row, value, multiplicity)) return false;
        if (i == 0) {
            new_count = static_cast<int>(row.size());
            updated.assign(Idx(*count_u) * Idx(new_count), Vec4d{});
            updated_knots = row_knots;
        }
        for (int j = 0; j < new_count; ++j) updated[SurfaceIndex(i, j, new_count)] = row[Idx(j)];
    }
    *control = std::move(updated);
    *knots_v = std::move(updated_knots);
    *count_v = new_count;
    return true;
}

}  // namespace cad
