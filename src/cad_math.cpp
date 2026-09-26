#include "cad_math.h"

#include <algorithm>
#include <cstdlib>

namespace cad {
namespace {

// Shared by every Gauss-Legendre consumer: rules are cached per order,
// thread_local so mep-fem's worker threads never contend for them (the
// editor process itself is single-threaded, but this file is linked into
// both, and a mutex on a path taken once per element would be absurd).
struct GlRule {
    std::vector<double> nodes;
    std::vector<double> weights;
};

// Legendre P_n(x) and its derivative, by the standard three-term
// recurrence. Returns P_n, writes P'_n. The derivative form used here,
// n*(x*P_n - P_{n-1})/(x^2 - 1), is singular at x = +/-1 -- which is
// harmless because no Gauss-Legendre node is ever at an endpoint.
double LegendreP(int n, double x, double *deriv) {
    double p_prev = 1.0;   // P_0
    double p_curr = x;     // P_1
    if (n == 0) {
        *deriv = 0.0;
        return 1.0;
    }
    for (int k = 2; k <= n; ++k) {
        const double kd = static_cast<double>(k);
        const double p_next = ((2.0 * kd - 1.0) * x * p_curr - (kd - 1.0) * p_prev) / kd;
        p_prev = p_curr;
        p_curr = p_next;
    }
    *deriv = static_cast<double>(n) * (x * p_curr - p_prev) / (x * x - 1.0);
    return p_curr;
}

// QUADPACK's dqk15 tables: the 15 Kronrod abscissae on [-1,1] given as
// the 8 non-negative ones in decreasing order (index 7 is the centre),
// their Kronrod weights, and the 7-point Gauss weights for the subset at
// odd indices. Tabulated rather than computed: unlike Gauss-Legendre
// (whose nodes are roots of a polynomial a few lines of recurrence
// produce), the Kronrod extension's nodes come from a Stieltjes
// polynomial whose construction is a substantial algorithm in its own
// right and is not worth carrying for one fixed order.
constexpr double kXgk[8] = {0.991455371120813, 0.949107912342759, 0.864864423359769, 0.741531185599394,
                            0.586087235467691, 0.405845151377397, 0.207784955007898, 0.000000000000000};
constexpr double kWgk[8] = {0.022935322010529, 0.063092092629979, 0.104790010322250, 0.140653259715525,
                            0.169004726639267, 0.190350578064785, 0.204432940075298, 0.209482141084728};
constexpr double kWg[4] = {0.129484966168870, 0.279705391489277, 0.381830050505119, 0.417959183673469};

// One G7-K15 panel over [a,b]: returns the Kronrod estimate and writes
// QUADPACK's own error estimate, which is not simply |K - G| -- that
// difference is an unreliable bound on its own (it can be accidentally
// tiny for an integrand the rule is resolving badly). The scaling below
// is dqk15's: the raw difference is normalized by resasc, the integral of
// |f - mean|, and raised to the 3/2 power, which reflects the actual
// asymptotic relationship between the two rules' errors.
double GaussKronrod15(const std::function<double(double)> &f, double a, double b, double *abs_err) {
    const double center = 0.5 * (a + b);
    const double half_length = 0.5 * (b - a);
    const double abs_half_length = std::fabs(half_length);

    const double fc = f(center);
    double result_k = kWgk[7] * fc;
    double result_g = kWg[3] * fc;
    double result_abs = std::fabs(result_k);

    double fv1[7];
    double fv2[7];
    for (int j = 0; j < 7; ++j) {
        const double offset = half_length * kXgk[j];
        fv1[j] = f(center - offset);
        fv2[j] = f(center + offset);
        const double fsum = fv1[j] + fv2[j];
        result_k += kWgk[j] * fsum;
        result_abs += kWgk[j] * (std::fabs(fv1[j]) + std::fabs(fv2[j]));
        // The Gauss subset sits at the odd Kronrod indices 1,3,5 (plus
        // the centre, already added above).
        if (j % 2 == 1) result_g += kWg[(j - 1) / 2] * fsum;
    }

    const double mean = result_k * 0.5;
    double result_asc = kWgk[7] * std::fabs(fc - mean);
    for (int j = 0; j < 7; ++j) {
        result_asc += kWgk[j] * (std::fabs(fv1[j] - mean) + std::fabs(fv2[j] - mean));
    }

    const double result = result_k * half_length;
    result_abs *= abs_half_length;
    result_asc *= abs_half_length;

    double err = std::fabs((result_k - result_g) * half_length);
    if (result_asc != 0.0 && err != 0.0) {
        err = result_asc * std::min(1.0, std::pow(200.0 * err / result_asc, 1.5));
    }
    // Floor at the unavoidable round-off of summing |f| over the panel --
    // no adaptive scheme can do better than this, and without the floor a
    // smooth integrand drives the bisection to max_depth chasing noise.
    const double round_off_floor = 50.0 * kEps * result_abs;
    if (result_abs > std::numeric_limits<double>::min() / (50.0 * kEps)) {
        err = std::max(round_off_floor, err);
    }
    *abs_err = err;
    return result;
}

double AdaptiveQuadratureRec(const std::function<double(double)> &f, double a, double b, double abs_tol, int depth,
                             int max_depth, double *out_error) {
    double err = 0.0;
    const double whole = GaussKronrod15(f, a, b, &err);
    if (err <= abs_tol || depth >= max_depth) {
        *out_error += err;
        return whole;
    }
    const double mid = 0.5 * (a + b);
    // Halving the tolerance per side keeps the *total* error under
    // abs_tol; giving each side the full tolerance (a common shortcut)
    // silently doubles the achievable error at every level of recursion.
    const double left = AdaptiveQuadratureRec(f, a, mid, abs_tol * 0.5, depth + 1, max_depth, out_error);
    const double right = AdaptiveQuadratureRec(f, mid, b, abs_tol * 0.5, depth + 1, max_depth, out_error);
    return left + right;
}

}  // namespace

// ---------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------

Vec3d Vec3d::AnyPerpendicular() const {
    // Cross with whichever world axis this vector leans on least: the
    // resulting cross product then has the largest magnitude available,
    // which is exactly the well-conditioned choice.
    const double ax = std::fabs(x);
    const double ay = std::fabs(y);
    const double az = std::fabs(z);
    Vec3d axis{0.0, 0.0, 1.0};
    if (ax <= ay && ax <= az) {
        axis = {1.0, 0.0, 0.0};
    } else if (ay <= az) {
        axis = {0.0, 1.0, 0.0};
    }
    return Cross(axis).Normalized();
}

// ---------------------------------------------------------------------
// Intervals
// ---------------------------------------------------------------------

Interval Interval::operator*(const Interval &o) const {
    // The four corner products: a sign-case analysis would be faster but
    // has eight branches and is a well-known source of off-by-one-case
    // bugs, and this is never the bottleneck.
    const double p0 = lo * o.lo;
    const double p1 = lo * o.hi;
    const double p2 = hi * o.lo;
    const double p3 = hi * o.hi;
    return {std::min(std::min(p0, p1), std::min(p2, p3)), std::max(std::max(p0, p1), std::max(p2, p3))};
}

// ---------------------------------------------------------------------
// Mat3d
// ---------------------------------------------------------------------

Mat3d Mat3d::Zero() {
    Mat3d r;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) r.m[i][j] = 0.0;
    return r;
}

Mat3d Mat3d::FromColumns(const Vec3d &c0, const Vec3d &c1, const Vec3d &c2) {
    Mat3d r;
    for (std::size_t i = 0; i < 3; ++i) {
        r.m[i][0] = c0[i];
        r.m[i][1] = c1[i];
        r.m[i][2] = c2[i];
    }
    return r;
}

Mat3d Mat3d::FromRows(const Vec3d &r0, const Vec3d &r1, const Vec3d &r2) {
    Mat3d r;
    for (std::size_t j = 0; j < 3; ++j) {
        r.m[0][j] = r0[j];
        r.m[1][j] = r1[j];
        r.m[2][j] = r2[j];
    }
    return r;
}

Mat3d Mat3d::Rotation(const Vec3d &axis, double angle) {
    const Vec3d u = axis.Normalized();
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double t = 1.0 - c;
    Mat3d r;
    r.m[0][0] = t * u.x * u.x + c;
    r.m[0][1] = t * u.x * u.y - s * u.z;
    r.m[0][2] = t * u.x * u.z + s * u.y;
    r.m[1][0] = t * u.x * u.y + s * u.z;
    r.m[1][1] = t * u.y * u.y + c;
    r.m[1][2] = t * u.y * u.z - s * u.x;
    r.m[2][0] = t * u.x * u.z - s * u.y;
    r.m[2][1] = t * u.y * u.z + s * u.x;
    r.m[2][2] = t * u.z * u.z + c;
    return r;
}

Vec3d Mat3d::operator*(const Vec3d &v) const {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
}

Mat3d Mat3d::operator*(const Mat3d &o) const {
    Mat3d r = Mat3d::Zero();
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j)
            for (std::size_t k = 0; k < 3; ++k) r.m[i][j] += m[i][k] * o.m[k][j];
    return r;
}

Mat3d Mat3d::operator+(const Mat3d &o) const {
    Mat3d r;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) r.m[i][j] = m[i][j] + o.m[i][j];
    return r;
}

Mat3d Mat3d::operator*(double s) const {
    Mat3d r;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) r.m[i][j] = m[i][j] * s;
    return r;
}

double Mat3d::Determinant() const {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

Mat3d Mat3d::Transposed() const {
    Mat3d r;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) r.m[i][j] = m[j][i];
    return r;
}

bool Mat3d::Inverse(Mat3d *out, const Tolerance &tol) const {
    const double det = Determinant();
    if (std::fabs(det) <= tol.linear) return false;
    const double inv_det = 1.0 / det;
    Mat3d r;
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv_det;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv_det;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv_det;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv_det;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv_det;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv_det;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv_det;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv_det;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv_det;
    *out = r;
    return true;
}

bool Mat3d::Solve(const Vec3d &b, Vec3d *out, const Tolerance &tol) const {
    Mat3d inv;
    if (!Inverse(&inv, tol)) return false;
    *out = inv * b;
    return true;
}

// ---------------------------------------------------------------------
// Mat4d
// ---------------------------------------------------------------------

Mat4d Mat4d::Translation(const Vec3d &t) {
    Mat4d r;
    r.m[0][3] = t.x;
    r.m[1][3] = t.y;
    r.m[2][3] = t.z;
    return r;
}

Mat4d Mat4d::Scaling(const Vec3d &s) {
    Mat4d r;
    r.m[0][0] = s.x;
    r.m[1][1] = s.y;
    r.m[2][2] = s.z;
    return r;
}

Mat4d Mat4d::Rotation(const Vec3d &axis, double angle) {
    const Mat3d rot = Mat3d::Rotation(axis, angle);
    Mat4d r;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) r.m[i][j] = rot.m[i][j];
    return r;
}

Mat4d Mat4d::Frame(const Vec3d &origin, const Vec3d &z_axis, const Vec3d &x_axis) {
    // STEP's AXIS2_PLACEMENT_3D semantics exactly: the axis (z) is
    // authoritative, the ref_direction (x) is only a hint for where zero
    // parameter lies and is projected perpendicular to z rather than
    // being trusted to already be so. Files in the wild routinely carry
    // an x that is a degree or two off, and a kernel that assumes
    // orthonormality produces a subtly sheared placement from them.
    const Vec3d z = z_axis.Normalized();
    Vec3d x = x_axis - z * z.Dot(x_axis);
    if (x.LengthSquared() <= kEps) {
        x = z.AnyPerpendicular();
    } else {
        x = x.Normalized();
    }
    const Vec3d y = z.Cross(x);
    Mat4d r;
    for (std::size_t i = 0; i < 3; ++i) {
        r.m[i][0] = x[i];
        r.m[i][1] = y[i];
        r.m[i][2] = z[i];
        r.m[i][3] = origin[i];
    }
    return r;
}

Mat3d Mat4d::LinearPart() const {
    Mat3d r;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) r.m[i][j] = m[i][j];
    return r;
}

Mat4d Mat4d::operator*(const Mat4d &o) const {
    Mat4d r;
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 4; ++k) sum += m[i][k] * o.m[k][j];
            r.m[i][j] = sum;
        }
    }
    return r;
}

Vec3d Mat4d::TransformPoint(const Vec3d &p) const {
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
            m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
}

Vec3d Mat4d::TransformVector(const Vec3d &v) const {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
}

Vec3d Mat4d::TransformNormal(const Vec3d &n) const {
    Mat3d inv;
    if (!LinearPart().Inverse(&inv)) return TransformVector(n);
    return inv.Transposed() * n;
}

double Mat4d::LinearDeterminant() const {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

bool Mat4d::Inverse(Mat4d *out, const Tolerance &tol) const {
    // Gauss-Jordan with partial pivoting on [M | I]. The affine shortcut
    // (invert the 3x3, negate the translation through it) would be faster
    // and is correct for every matrix this kernel actually builds -- but
    // STEP and glTF can both hand us a general matrix, and a silently
    // wrong inverse for one is far more expensive than the handful of
    // extra operations here.
    double a[4][8];
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            a[i][j] = m[i][j];
            a[i][j + 4] = (i == j) ? 1.0 : 0.0;
        }
    }
    for (std::size_t col = 0; col < 4; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < 4; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
        }
        if (std::fabs(a[pivot][col]) <= tol.linear) return false;
        if (pivot != col) {
            for (std::size_t j = 0; j < 8; ++j) std::swap(a[col][j], a[pivot][j]);
        }
        const double inv_pivot = 1.0 / a[col][col];
        for (std::size_t j = 0; j < 8; ++j) a[col][j] *= inv_pivot;
        for (std::size_t r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double factor = a[r][col];
            if (factor == 0.0) continue;
            for (std::size_t j = 0; j < 8; ++j) a[r][j] -= factor * a[col][j];
        }
    }
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t j = 0; j < 4; ++j) out->m[i][j] = a[i][j + 4];
    return true;
}

// ---------------------------------------------------------------------
// MatrixNd
// ---------------------------------------------------------------------

void MatrixNd::SetIdentity() {
    Fill(0.0);
    const std::size_t n = std::min(rows_, cols_);
    for (std::size_t i = 0; i < n; ++i) (*this)(i, i) = 1.0;
}

MatrixNd MatrixNd::Transposed() const {
    MatrixNd r(cols_, rows_);
    for (std::size_t i = 0; i < rows_; ++i)
        for (std::size_t j = 0; j < cols_; ++j) r(j, i) = (*this)(i, j);
    return r;
}

MatrixNd MatrixNd::operator*(const MatrixNd &o) const {
    MatrixNd r(rows_, o.cols_);
    if (cols_ != o.rows_) return r;
    for (std::size_t i = 0; i < rows_; ++i) {
        for (std::size_t k = 0; k < cols_; ++k) {
            const double aik = (*this)(i, k);
            if (aik == 0.0) continue;
            for (std::size_t j = 0; j < o.cols_; ++j) r(i, j) += aik * o(k, j);
        }
    }
    return r;
}

std::vector<double> MatrixNd::operator*(const std::vector<double> &v) const {
    std::vector<double> r(rows_, 0.0);
    if (v.size() != cols_) return r;
    for (std::size_t i = 0; i < rows_; ++i) {
        double sum = 0.0;
        for (std::size_t j = 0; j < cols_; ++j) sum += (*this)(i, j) * v[j];
        r[i] = sum;
    }
    return r;
}

bool MatrixNd::SolveLU(const std::vector<double> &b, std::vector<double> *out, double tol) const {
    if (rows_ != cols_ || b.size() != rows_) return false;
    const std::size_t n = rows_;
    std::vector<double> a = a_;  // working copy; the caller's matrix is const
    std::vector<double> x = b;
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < n; ++r) {
            if (std::fabs(a[r * n + col]) > std::fabs(a[pivot * n + col])) pivot = r;
        }
        if (std::fabs(a[pivot * n + col]) <= tol) return false;
        if (pivot != col) {
            for (std::size_t j = 0; j < n; ++j) std::swap(a[col * n + j], a[pivot * n + j]);
            std::swap(x[col], x[pivot]);
        }
        const double diag = a[col * n + col];
        for (std::size_t r = col + 1; r < n; ++r) {
            const double factor = a[r * n + col] / diag;
            if (factor == 0.0) continue;
            a[r * n + col] = 0.0;
            for (std::size_t j = col + 1; j < n; ++j) a[r * n + j] -= factor * a[col * n + j];
            x[r] -= factor * x[col];
        }
    }
    // Back substitution. Reverse iteration over an unsigned index, so the
    // loop counts down from n and the body uses i-1 -- the idiom that
    // avoids the wrap-around bug the naive `for (size_t i = n-1; i >= 0)`
    // has.
    for (std::size_t i = n; i-- > 0;) {
        double sum = x[i];
        for (std::size_t j = i + 1; j < n; ++j) sum -= a[i * n + j] * x[j];
        x[i] = sum / a[i * n + i];
    }
    *out = x;
    return true;
}

bool MatrixNd::SolveCholesky(const std::vector<double> &b, std::vector<double> *out, double tol) const {
    if (rows_ != cols_ || b.size() != rows_) return false;
    const std::size_t n = rows_;
    std::vector<double> l(n * n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j <= i; ++j) {
            double sum = (*this)(i, j);
            for (std::size_t k = 0; k < j; ++k) sum -= l[i * n + k] * l[j * n + k];
            if (i == j) {
                // A non-positive pivot means the matrix is not positive
                // definite. That is not an error here -- it is the signal
                // LevenbergMarquardt reads to know its damping is still
                // too small -- so it returns false rather than aborting.
                if (sum <= tol) return false;
                l[i * n + j] = std::sqrt(sum);
            } else {
                l[i * n + j] = sum / l[j * n + j];
            }
        }
    }
    std::vector<double> y(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double sum = b[i];
        for (std::size_t k = 0; k < i; ++k) sum -= l[i * n + k] * y[k];
        y[i] = sum / l[i * n + i];
    }
    std::vector<double> x(n, 0.0);
    for (std::size_t i = n; i-- > 0;) {
        double sum = y[i];
        for (std::size_t k = i + 1; k < n; ++k) sum -= l[k * n + i] * x[k];
        x[i] = sum / l[i * n + i];
    }
    *out = x;
    return true;
}

bool MatrixNd::SolveLeastSquares(const std::vector<double> &b, std::vector<double> *out, double tol) const {
    if (b.size() != rows_ || rows_ < cols_ || cols_ == 0) return false;
    const std::size_t m = rows_;
    const std::size_t n = cols_;
    std::vector<double> a = a_;
    std::vector<double> rhs = b;
    // Householder QR in place: each reflector zeroes the sub-diagonal of
    // one column and is applied to the remaining columns and to the rhs.
    for (std::size_t k = 0; k < n; ++k) {
        double norm = 0.0;
        for (std::size_t i = k; i < m; ++i) norm += a[i * n + k] * a[i * n + k];
        norm = std::sqrt(norm);
        if (norm <= tol) return false;  // rank-deficient
        // Sign chosen to move *away* from the existing diagonal entry:
        // the opposite choice can cancel catastrophically when the column
        // is already nearly axis-aligned, which for a fitting matrix is
        // the common case rather than the exotic one.
        const double alpha = (a[k * n + k] > 0.0) ? -norm : norm;
        std::vector<double> v(m - k, 0.0);
        for (std::size_t i = k; i < m; ++i) v[i - k] = a[i * n + k];
        v[0] -= alpha;
        double v_norm_sq = 0.0;
        for (double vi : v) v_norm_sq += vi * vi;
        if (v_norm_sq <= tol * tol) {
            a[k * n + k] = alpha;
            continue;
        }
        for (std::size_t j = k; j < n; ++j) {
            double dot = 0.0;
            for (std::size_t i = k; i < m; ++i) dot += v[i - k] * a[i * n + j];
            const double factor = 2.0 * dot / v_norm_sq;
            for (std::size_t i = k; i < m; ++i) a[i * n + j] -= factor * v[i - k];
        }
        double dot_rhs = 0.0;
        for (std::size_t i = k; i < m; ++i) dot_rhs += v[i - k] * rhs[i];
        const double factor_rhs = 2.0 * dot_rhs / v_norm_sq;
        for (std::size_t i = k; i < m; ++i) rhs[i] -= factor_rhs * v[i - k];
    }
    std::vector<double> x(n, 0.0);
    for (std::size_t i = n; i-- > 0;) {
        double sum = rhs[i];
        for (std::size_t j = i + 1; j < n; ++j) sum -= a[i * n + j] * x[j];
        if (std::fabs(a[i * n + i]) <= tol) return false;
        x[i] = sum / a[i * n + i];
    }
    *out = x;
    return true;
}

// --- Singular value decomposition -------------------------------------

namespace {

// One sweep of one-sided Jacobi over the columns of `a` (m x n, stored
// row-major), accumulating the right rotations into `v`. Returns the
// largest off-diagonality it found, so the caller can stop sweeping.
double JacobiSweep(std::vector<double> &a, std::vector<double> &v, std::size_t m, std::size_t n) {
    double worst = 0.0;
    for (std::size_t p = 0; p + 1 < n; ++p) {
        for (std::size_t q = p + 1; q < n; ++q) {
            double alpha = 0.0;
            double beta = 0.0;
            double gamma = 0.0;
            for (std::size_t i = 0; i < m; ++i) {
                const double ap = a[i * n + p];
                const double aq = a[i * n + q];
                alpha += ap * ap;
                beta += aq * aq;
                gamma += ap * aq;
            }
            if (alpha <= 0.0 || beta <= 0.0) continue;
            // The measure of how far these two columns are from being
            // orthogonal, scaled so it is comparable across columns of
            // very different lengths -- which is the whole point of the
            // one-sided form, and what gives the small singular values
            // their relative accuracy.
            const double off = std::fabs(gamma) / std::sqrt(alpha * beta);
            worst = std::max(worst, off);
            if (off <= 1e-15) continue;
            // The rotation that makes them orthogonal, in the stable
            // form (t from the smaller root of the quadratic, rather
            // than theta from an arctangent).
            const double zeta = (beta - alpha) / (2.0 * gamma);
            const double t = (zeta >= 0.0) ? 1.0 / (zeta + std::sqrt(1.0 + zeta * zeta))
                                           : -1.0 / (-zeta + std::sqrt(1.0 + zeta * zeta));
            const double c = 1.0 / std::sqrt(1.0 + t * t);
            const double s = c * t;
            for (std::size_t i = 0; i < m; ++i) {
                const double ap = a[i * n + p];
                const double aq = a[i * n + q];
                a[i * n + p] = c * ap - s * aq;
                a[i * n + q] = s * ap + c * aq;
            }
            for (std::size_t i = 0; i < n; ++i) {
                const double vp = v[i * n + p];
                const double vq = v[i * n + q];
                v[i * n + p] = c * vp - s * vq;
                v[i * n + q] = s * vp + c * vq;
            }
        }
    }
    return worst;
}

// The decomposition for the m >= n case, which is all JacobiSweep can do.
bool SvdTall(const MatrixNd &a, MatrixNd *u, std::vector<double> *s, MatrixNd *v) {
    const std::size_t m = a.Rows();
    const std::size_t n = a.Cols();
    std::vector<double> work(m * n, 0.0);
    for (std::size_t i = 0; i < m; ++i) {
        for (std::size_t j = 0; j < n; ++j) work[i * n + j] = a(i, j);
    }
    std::vector<double> right(n * n, 0.0);
    for (std::size_t i = 0; i < n; ++i) right[i * n + i] = 1.0;

    // Thirty sweeps is far more than convergence needs (it is quadratic
    // once the off-diagonality is small, and eight or so is typical); the
    // count is a guard against a pathological input, not a schedule.
    for (int sweep = 0; sweep < 30; ++sweep) {
        if (JacobiSweep(work, right, m, n) <= 1e-15) break;
    }

    // The columns of the rotated matrix are now orthogonal; their norms
    // are the singular values and their normalisations are U.
    std::vector<std::pair<double, std::size_t>> order;
    order.reserve(n);
    for (std::size_t j = 0; j < n; ++j) {
        double norm = 0.0;
        for (std::size_t i = 0; i < m; ++i) norm += work[i * n + j] * work[i * n + j];
        order.push_back({std::sqrt(norm), j});
    }
    std::sort(order.begin(), order.end(),
              [](const std::pair<double, std::size_t> &x, const std::pair<double, std::size_t> &y) {
                  return x.first > y.first;
              });

    s->assign(n, 0.0);
    if (u != nullptr) *u = MatrixNd(m, n);
    if (v != nullptr) *v = MatrixNd(n, n);
    for (std::size_t k = 0; k < n; ++k) {
        const double sigma = order[k].first;
        const std::size_t j = order[k].second;
        (*s)[k] = sigma;
        if (v != nullptr) {
            for (std::size_t i = 0; i < n; ++i) (*v)(i, k) = right[i * n + j];
        }
        if (u != nullptr) {
            // A zero singular value leaves the corresponding column of U
            // undetermined; it is left at zero rather than filled with a
            // made-up direction, since no caller here uses it and a
            // fabricated one would make U look orthogonal when it is not.
            if (sigma > 0.0) {
                for (std::size_t i = 0; i < m; ++i) (*u)(i, k) = work[i * n + j] / sigma;
            }
        }
    }
    return true;
}

}  // namespace

bool Svd(const MatrixNd &a, MatrixNd *u, std::vector<double> *s, MatrixNd *v) {
    if (a.Rows() == 0 || a.Cols() == 0) return false;
    if (a.Rows() >= a.Cols()) return SvdTall(a, u, s, v);
    // A wide matrix is decomposed through its transpose: A = U S V^T
    // gives A^T = V S U^T, so the two orthogonal factors simply swap.
    // A sketch is wide far more often than not -- that is what an
    // under-constrained sketch *is* -- so this is the usual path, not the
    // exceptional one.
    MatrixNd tall_u;
    MatrixNd tall_v;
    if (!SvdTall(a.Transposed(), &tall_u, s, &tall_v)) return false;
    if (u != nullptr) *u = tall_v;
    if (v != nullptr) *v = tall_u;
    return true;
}

int MatrixRank(const MatrixNd &a, double relative_tolerance) {
    std::vector<double> s;
    if (!Svd(a, nullptr, &s, nullptr)) return -1;
    if (s.empty()) return 0;
    const double cutoff = s.front() * relative_tolerance;
    int rank = 0;
    for (double sigma : s) {
        if (sigma > cutoff) ++rank;
    }
    return rank;
}

int NullSpace(const MatrixNd &a, std::vector<std::vector<double>> *basis, double relative_tolerance) {
    basis->clear();
    MatrixNd v;
    std::vector<double> s;
    if (!Svd(a, nullptr, &s, &v)) return -1;
    const std::size_t n = a.Cols();
    const double cutoff = s.empty() ? 0.0 : s.front() * relative_tolerance;

    // The right singular vectors with a non-zero singular value. Those
    // are the ones the decomposition actually determines: a zero singular
    // value leaves its vector arbitrary, and for a wide matrix -- which
    // is what an under-constrained sketch always is -- it is not even
    // stored, because that decomposition is computed through the
    // transpose and the vectors in question are columns of the *other*
    // factor. So the null space is built as the orthogonal complement of
    // what is known, rather than read off what is not.
    std::vector<std::vector<double>> range;
    for (std::size_t k = 0; k < s.size() && k < v.Cols(); ++k) {
        if (s[k] <= cutoff) continue;
        std::vector<double> direction(n, 0.0);
        for (std::size_t i = 0; i < n && i < v.Rows(); ++i) direction[i] = v(i, k);
        range.push_back(std::move(direction));
    }
    const std::size_t wanted = n - range.size();

    // Pivoted Gram-Schmidt on the coordinate axes: each round takes the
    // axis whose component orthogonal to everything found so far is
    // largest. Taking them in index order instead would eventually pick
    // an axis that is almost entirely spanned already, and normalising
    // its residual would amplify the rounding error in it into a
    // direction that is not quite in the null space.
    std::vector<bool> used(n, false);
    while (basis->size() < wanted) {
        std::vector<double> best;
        double best_norm = -1.0;
        std::size_t best_axis = 0;
        for (std::size_t axis = 0; axis < n; ++axis) {
            if (used[axis]) continue;
            std::vector<double> candidate(n, 0.0);
            candidate[axis] = 1.0;
            // Twice, which is the standard cure for classical
            // Gram-Schmidt losing orthogonality on the first pass.
            for (int pass = 0; pass < 2; ++pass) {
                for (const std::vector<std::vector<double>> *set : {&range, basis}) {
                    for (const std::vector<double> &q : *set) {
                        double dot = 0.0;
                        for (std::size_t i = 0; i < n; ++i) dot += candidate[i] * q[i];
                        for (std::size_t i = 0; i < n; ++i) candidate[i] -= dot * q[i];
                    }
                }
            }
            double norm = 0.0;
            for (double x : candidate) norm += x * x;
            norm = std::sqrt(norm);
            if (norm > best_norm) {
                best_norm = norm;
                best = std::move(candidate);
                best_axis = axis;
            }
        }
        if (best_norm <= 1e-9) break;
        for (double &x : best) x /= best_norm;
        basis->push_back(std::move(best));
        used[best_axis] = true;
    }
    return static_cast<int>(basis->size());
}

// ---------------------------------------------------------------------
// Root finding and minimization
// ---------------------------------------------------------------------

SolveResult NewtonBracketed(const std::function<double(double, double *)> &f, double a, double b, double *root,
                            const SolveOptions &opt) {
    SolveResult res;
    double deriv_lo = 0.0;
    double deriv_hi = 0.0;
    double f_lo = f(a, &deriv_lo);
    double f_hi = f(b, &deriv_hi);
    if (f_lo == 0.0) {
        *root = a;
        res.status = SolveStatus::Converged;
        return res;
    }
    if (f_hi == 0.0) {
        *root = b;
        res.status = SolveStatus::Converged;
        return res;
    }
    if ((f_lo > 0.0) == (f_hi > 0.0)) {
        // No sign change: there may still be a root in here, but nothing
        // in this routine can guarantee finding it, and silently
        // returning one endpoint would be worse than saying so.
        res.status = SolveStatus::Diverged;
        return res;
    }
    // Orient so that f(lo) < 0 < f(hi); every bracket update below then
    // reads the same way regardless of which end started negative.
    double lo = a;
    double hi = b;
    if (f_lo > 0.0) std::swap(lo, hi);

    double x = 0.5 * (lo + hi);
    double step_prev = std::fabs(hi - lo);
    double step = step_prev;
    double deriv = 0.0;
    double fx = f(x, &deriv);
    for (int i = 0; i < opt.max_iterations; ++i) {
        res.iterations = i + 1;
        // Take a bisection step when the Newton step would leave the
        // bracket, or when it is not at least halving the interval every
        // two iterations. Without that second test a Newton iteration
        // near an inflection can creep inside the bracket forever.
        const bool newton_out_of_range = ((x - hi) * deriv - fx) * ((x - lo) * deriv - fx) > 0.0;
        const bool newton_too_slow = std::fabs(2.0 * fx) > std::fabs(step_prev * deriv);
        step_prev = step;
        if (newton_out_of_range || newton_too_slow) {
            step = 0.5 * (hi - lo);
            x = lo + step;
        } else {
            step = fx / deriv;
            x -= step;
        }
        if (std::fabs(step) < opt.x_tol) {
            *root = x;
            res.status = SolveStatus::Converged;
            res.residual = std::fabs(fx);
            return res;
        }
        fx = f(x, &deriv);
        if (std::fabs(fx) <= opt.f_tol) {
            *root = x;
            res.status = SolveStatus::Converged;
            res.residual = std::fabs(fx);
            return res;
        }
        if (fx < 0.0) {
            lo = x;
        } else {
            hi = x;
        }
    }
    *root = x;
    res.residual = std::fabs(fx);
    res.status = SolveStatus::MaxIterations;
    return res;
}

SolveResult BrentRoot(const std::function<double(double)> &f, double a, double b, double *root,
                      const SolveOptions &opt) {
    SolveResult res;
    double fa = f(a);
    double fb = f(b);
    if (fa == 0.0) {
        *root = a;
        res.status = SolveStatus::Converged;
        return res;
    }
    if (fb == 0.0) {
        *root = b;
        res.status = SolveStatus::Converged;
        return res;
    }
    if ((fa > 0.0) == (fb > 0.0)) {
        res.status = SolveStatus::Diverged;
        return res;
    }
    double c = a;
    double fc = fa;
    double d = b - a;
    double e = d;
    for (int i = 0; i < opt.max_iterations; ++i) {
        res.iterations = i + 1;
        if ((fb > 0.0) == (fc > 0.0)) {
            c = a;
            fc = fa;
            d = b - a;
            e = d;
        }
        if (std::fabs(fc) < std::fabs(fb)) {
            a = b;
            b = c;
            c = a;
            fa = fb;
            fb = fc;
            fc = fa;
        }
        const double tol1 = 2.0 * kEps * std::fabs(b) + 0.5 * opt.x_tol;
        const double xm = 0.5 * (c - b);
        if (std::fabs(xm) <= tol1 || fb == 0.0) {
            *root = b;
            res.status = SolveStatus::Converged;
            res.residual = std::fabs(fb);
            return res;
        }
        if (std::fabs(e) >= tol1 && std::fabs(fa) > std::fabs(fb)) {
            // Inverse quadratic interpolation through (a,fa),(b,fb),(c,fc)
            // -- or plain secant when only two distinct points exist.
            const double s = fb / fa;
            double p = 0.0;
            double q = 0.0;
            if (a == c) {
                p = 2.0 * xm * s;
                q = 1.0 - s;
            } else {
                const double qq = fa / fc;
                const double r = fb / fc;
                p = s * (2.0 * xm * qq * (qq - r) - (b - a) * (r - 1.0));
                q = (qq - 1.0) * (r - 1.0) * (s - 1.0);
            }
            if (p > 0.0) q = -q;
            p = std::fabs(p);
            const double min1 = 3.0 * xm * q - std::fabs(tol1 * q);
            const double min2 = std::fabs(e * q);
            if (2.0 * p < std::min(min1, min2)) {
                e = d;
                d = p / q;
            } else {
                d = xm;
                e = d;
            }
        } else {
            d = xm;
            e = d;
        }
        a = b;
        fa = fb;
        if (std::fabs(d) > tol1) {
            b += d;
        } else {
            b += (xm > 0.0 ? tol1 : -tol1);
        }
        fb = f(b);
    }
    *root = b;
    res.residual = std::fabs(fb);
    res.status = SolveStatus::MaxIterations;
    return res;
}

SolveResult BrentMinimize(const std::function<double(double)> &f, double a, double b, double *arg_min,
                          double *min_value, const SolveOptions &opt) {
    SolveResult res;
    // The golden ratio's conjugate, the fraction Brent's method falls back
    // to whenever the parabolic fit is not usable.
    constexpr double kGolden = 0.3819660112501051;
    double x = a + kGolden * (b - a);
    double w = x;
    double v = x;
    double fx = f(x);
    double fw = fx;
    double fv = fx;
    double d = 0.0;
    double e = 0.0;
    for (int i = 0; i < opt.max_iterations; ++i) {
        res.iterations = i + 1;
        const double xm = 0.5 * (a + b);
        const double tol1 = std::sqrt(kEps) * std::fabs(x) + opt.x_tol;
        const double tol2 = 2.0 * tol1;
        if (std::fabs(x - xm) <= tol2 - 0.5 * (b - a)) {
            *arg_min = x;
            *min_value = fx;
            res.status = SolveStatus::Converged;
            res.residual = fx;
            return res;
        }
        bool use_golden = true;
        if (std::fabs(e) > tol1) {
            // Fit a parabola through (x,fx), (w,fw), (v,fv).
            const double r = (x - w) * (fx - fv);
            double q = (x - v) * (fx - fw);
            double p = (x - v) * q - (x - w) * r;
            q = 2.0 * (q - r);
            if (q > 0.0) p = -p;
            q = std::fabs(q);
            const double e_prev = e;
            e = d;
            // Accept the parabolic step only if it lands inside the
            // bracket and is less than half the step before last --
            // Brent's own guard against the fit walking off a flat region.
            if (std::fabs(p) < std::fabs(0.5 * q * e_prev) && p > q * (a - x) && p < q * (b - x)) {
                d = p / q;
                const double u_trial = x + d;
                if (u_trial - a < tol2 || b - u_trial < tol2) d = (xm > x ? tol1 : -tol1);
                use_golden = false;
            }
        }
        if (use_golden) {
            e = (x >= xm) ? (a - x) : (b - x);
            d = kGolden * e;
        }
        const double u = (std::fabs(d) >= tol1) ? (x + d) : (x + (d > 0.0 ? tol1 : -tol1));
        const double fu = f(u);
        if (fu <= fx) {
            if (u >= x) {
                a = x;
            } else {
                b = x;
            }
            v = w;
            fv = fw;
            w = x;
            fw = fx;
            x = u;
            fx = fu;
        } else {
            if (u < x) {
                a = u;
            } else {
                b = u;
            }
            if (fu <= fw || w == x) {
                v = w;
                fv = fw;
                w = u;
                fw = fu;
            } else if (fu <= fv || v == x || v == w) {
                v = u;
                fv = fu;
            }
        }
    }
    *arg_min = x;
    *min_value = fx;
    res.residual = fx;
    res.status = SolveStatus::MaxIterations;
    return res;
}

SolveResult NewtonSolve(const std::function<void(const std::vector<double> &, std::vector<double> *, MatrixNd *)> &residual_and_jacobian,
                        std::vector<double> *x, const SolveOptions &opt) {
    SolveResult res;
    const std::size_t n = x->size();
    if (n == 0) {
        res.status = SolveStatus::Converged;
        return res;
    }
    std::vector<double> r(n, 0.0);
    MatrixNd j(n, n);
    for (int it = 0; it < opt.max_iterations; ++it) {
        res.iterations = it + 1;
        j.Fill(0.0);
        residual_and_jacobian(*x, &r, &j);
        double r_norm = 0.0;
        for (double ri : r) r_norm += ri * ri;
        r_norm = std::sqrt(r_norm);
        res.residual = r_norm;
        if (r_norm <= opt.f_tol) {
            res.status = SolveStatus::Converged;
            return res;
        }
        std::vector<double> neg_r(n);
        for (std::size_t i = 0; i < n; ++i) neg_r[i] = -r[i];
        std::vector<double> dx;
        if (!j.SolveLU(neg_r, &dx)) {
            res.status = SolveStatus::Singular;
            return res;
        }
        double dx_norm = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            (*x)[i] += dx[i];
            dx_norm += dx[i] * dx[i];
        }
        if (std::sqrt(dx_norm) <= opt.x_tol) {
            res.status = SolveStatus::Converged;
            return res;
        }
    }
    res.status = SolveStatus::MaxIterations;
    return res;
}

SolveResult LevenbergMarquardt(const std::function<void(const std::vector<double> &, std::vector<double> *, MatrixNd *)> &residual_and_jacobian,
                               std::vector<double> *x, std::size_t residual_count, const SolveOptions &opt,
                               double lambda0) {
    SolveResult res;
    const std::size_t n = x->size();
    const std::size_t m = residual_count;
    if (n == 0 || m == 0) {
        res.status = SolveStatus::Converged;
        return res;
    }
    std::vector<double> r(m, 0.0);
    MatrixNd jac(m, n);
    auto cost = [](const std::vector<double> &v) {
        double s = 0.0;
        for (double e : v) s += e * e;
        return s;
    };

    jac.Fill(0.0);
    residual_and_jacobian(*x, &r, &jac);
    double current_cost = cost(r);
    double lambda = lambda0;

    for (int it = 0; it < opt.max_iterations; ++it) {
        res.iterations = it + 1;
        res.residual = std::sqrt(current_cost);
        if (res.residual <= opt.f_tol) {
            res.status = SolveStatus::Converged;
            return res;
        }
        // Normal equations: (JtJ + lambda*diag(JtJ)) dx = -Jt r.
        MatrixNd jt = jac.Transposed();
        MatrixNd jtj = jt * jac;
        std::vector<double> jtr = jt * r;
        for (std::size_t i = 0; i < n; ++i) jtr[i] = -jtr[i];

        bool stepped = false;
        // The largest curvature in the problem, used below to put a floor
        // under the damping.
        double largest_diagonal = 0.0;
        for (std::size_t i = 0; i < n; ++i) largest_diagonal = std::max(largest_diagonal, jtj(i, i));
        const double damping_floor = (largest_diagonal > 0.0) ? largest_diagonal * 1e-6 : 1.0;
        // Inner loop: raise the damping until the damped system is both
        // solvable and actually reduces the cost. Bounded so a hopeless
        // problem terminates rather than spinning -- at lambda this large
        // the step is numerically zero anyway.
        for (int inner = 0; inner < 32 && lambda < 1e14; ++inner) {
            MatrixNd damped = jtj;
            for (std::size_t i = 0; i < n; ++i) {
                // Marquardt's scaling: damp each parameter in proportion
                // to its own curvature, so the step is invariant to the
                // units the parameters happen to be measured in.
                //
                // With a floor, because scaling by a parameter's *own*
                // curvature leaves a nearly flat direction almost
                // undamped, and the Gauss-Newton step along such a
                // direction is enormous -- gradient over curvature, with
                // a curvature of nothing. A sketch's construction line,
                // whose length no constraint depends on except through a
                // second-order term, was stretched from ten units to
                // fourteen hundred that way; the residual was not much
                // worse for it, which is exactly why nothing pushed back.
                // Flooring the damping at a fraction of the largest
                // curvature keeps those directions where they were,
                // which for an under-constrained problem -- the normal
                // state of a sketch -- is the only sensible answer.
                damped(i, i) += lambda * std::max(jtj(i, i), damping_floor);
            }
            std::vector<double> dx;
            if (!damped.SolveCholesky(jtr, &dx)) {
                lambda *= 10.0;
                continue;
            }
            std::vector<double> trial = *x;
            double dx_norm = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                trial[i] += dx[i];
                dx_norm += dx[i] * dx[i];
            }
            dx_norm = std::sqrt(dx_norm);
            std::vector<double> trial_r(m, 0.0);
            MatrixNd trial_j(m, n);
            residual_and_jacobian(trial, &trial_r, &trial_j);
            const double trial_cost = cost(trial_r);
            if (trial_cost < current_cost) {
                *x = trial;
                r = trial_r;
                jac = trial_j;
                current_cost = trial_cost;
                lambda = std::max(lambda * 0.1, 1e-14);
                stepped = true;
                if (dx_norm <= opt.x_tol) {
                    res.residual = std::sqrt(current_cost);
                    res.status = SolveStatus::Converged;
                    return res;
                }
                break;
            }
            lambda *= 10.0;
        }
        if (!stepped) {
            // No damping value produced an improvement: this is a genuine
            // local minimum of the least-squares cost, which for an
            // over-constrained sketch is the right answer and not a
            // failure -- so the residual is reported and the caller
            // decides whether it is small enough to call solved.
            res.residual = std::sqrt(current_cost);
            res.status = (res.residual <= opt.f_tol) ? SolveStatus::Converged : SolveStatus::MaxIterations;
            return res;
        }
    }
    res.residual = std::sqrt(current_cost);
    res.status = SolveStatus::MaxIterations;
    return res;
}

// ---------------------------------------------------------------------
// Quadrature
// ---------------------------------------------------------------------

void GaussLegendreRule(int n, std::vector<double> *nodes, std::vector<double> *weights) {
    if (n < 1) {
        nodes->clear();
        weights->clear();
        return;
    }
    thread_local std::vector<GlRule> cache;
    const std::size_t index = static_cast<std::size_t>(n);
    if (cache.size() <= index) cache.resize(index + 1);
    GlRule &entry = cache[index];
    if (entry.nodes.empty()) {
        const std::size_t count = static_cast<std::size_t>(n);
        entry.nodes.assign(count, 0.0);
        entry.weights.assign(count, 0.0);
        // Only half the roots are computed: they are symmetric about 0,
        // and computing both halves independently would let round-off
        // break that symmetry, which shows up downstream as a quadrature
        // rule that integrates odd functions to something other than zero.
        const std::size_t half = (count + 1) / 2;
        for (std::size_t i = 0; i < half; ++i) {
            // Chebyshev-like starting guess; accurate enough that Newton
            // converges in three or four iterations for every order.
            double x = std::cos(kPi * (static_cast<double>(i) + 0.75) / (static_cast<double>(n) + 0.5));
            double deriv = 0.0;
            for (int iter = 0; iter < 100; ++iter) {
                const double p = LegendreP(n, x, &deriv);
                const double dx = p / deriv;
                x -= dx;
                if (std::fabs(dx) <= 1e-15) break;
            }
            LegendreP(n, x, &deriv);
            const double w = 2.0 / ((1.0 - x * x) * deriv * deriv);
            entry.nodes[i] = -x;
            entry.nodes[count - 1 - i] = x;
            entry.weights[i] = w;
            entry.weights[count - 1 - i] = w;
        }
    }
    *nodes = entry.nodes;
    *weights = entry.weights;
}

double GaussLegendre(const std::function<double(double)> &f, double a, double b, int n) {
    std::vector<double> nodes;
    std::vector<double> weights;
    GaussLegendreRule(n, &nodes, &weights);
    const double half = 0.5 * (b - a);
    const double mid = 0.5 * (a + b);
    double sum = 0.0;
    for (std::size_t i = 0; i < nodes.size(); ++i) sum += weights[i] * f(mid + half * nodes[i]);
    return sum * half;
}

double AdaptiveQuadrature(const std::function<double(double)> &f, double a, double b, double abs_tol, int max_depth,
                          double *out_error) {
    double error = 0.0;
    const double result = AdaptiveQuadratureRec(f, a, b, abs_tol, 0, max_depth, &error);
    if (out_error != nullptr) *out_error = error;
    return result;
}

}  // namespace cad
