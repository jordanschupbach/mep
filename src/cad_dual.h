#ifndef MEP_CAD_DUAL_H
#define MEP_CAD_DUAL_H

#include <array>
#include <cmath>
#include <cstddef>

// Forward-mode dual numbers, and the bookkeeping that turns them into
// rows of a Jacobian.
//
// Each Dual carries a value and its partial derivatives with respect to
// the handful of parameters the residual being evaluated touches. The
// arithmetic applies the chain rule, so writing the residual writes the
// Jacobian row with it -- which is the whole reason the sketch solver in
// Part D.2 has exact derivatives rather than finite differences, and why
// it converges quadratically on a well-posed sketch instead of crawling.
//
// Extracted from cad_constraint.cpp when Part E.6's assembly solver
// needed the same thing in three dimensions. Shared rather than copied,
// because the softening below is the kind of thing that gets fixed in
// one place and forgotten in the other.
//
// SIXTEEN SLOTS is comfortably more than any residual in the kernel
// needs. The widest in Part D.2 is an angle or a collinearity between two
// lines, at eight; the widest in Part E.6 is a mate between two
// instances, at fourteen.
namespace cad {

inline constexpr int kMaxLocal = 16;

namespace dual_detail {
inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }
}  // namespace dual_detail

struct Dual {
    double v = 0.0;
    std::array<double, kMaxLocal> d{};

    Dual() = default;
    // Implicit, so a constant can be written as a plain number inside a
    // residual and the formula reads the way it does on paper.
    Dual(double value) : v(value) {}  // NOLINT(google-explicit-constructor)
};

inline Dual operator+(const Dual &a, const Dual &b) {
    using dual_detail::Idx;
    Dual r;
    r.v = a.v + b.v;
    for (int i = 0; i < kMaxLocal; ++i) r.d[Idx(i)] = a.d[Idx(i)] + b.d[Idx(i)];
    return r;
}
inline Dual operator-(const Dual &a, const Dual &b) {
    using dual_detail::Idx;
    Dual r;
    r.v = a.v - b.v;
    for (int i = 0; i < kMaxLocal; ++i) r.d[Idx(i)] = a.d[Idx(i)] - b.d[Idx(i)];
    return r;
}
inline Dual operator*(const Dual &a, const Dual &b) {
    using dual_detail::Idx;
    Dual r;
    r.v = a.v * b.v;
    for (int i = 0; i < kMaxLocal; ++i) r.d[Idx(i)] = a.d[Idx(i)] * b.v + a.v * b.d[Idx(i)];
    return r;
}
inline Dual operator/(const Dual &a, const Dual &b) {
    using dual_detail::Idx;
    Dual r;
    r.v = a.v / b.v;
    const double inv = 1.0 / (b.v * b.v);
    for (int i = 0; i < kMaxLocal; ++i) r.d[Idx(i)] = (a.d[Idx(i)] * b.v - a.v * b.d[Idx(i)]) * inv;
    return r;
}
inline Dual Sqrt(const Dual &a) {
    using dual_detail::Idx;
    Dual r;
    r.v = std::sqrt(a.v);
    // At zero the derivative is infinite. Every use goes through a
    // softened length or absolute value, which adds a positive term
    // precisely so this cannot be reached; the guard is for the case a
    // caller adds a new residual and forgets.
    const double scale = (r.v > 0.0) ? 0.5 / r.v : 0.0;
    for (int i = 0; i < kMaxLocal; ++i) r.d[Idx(i)] = a.d[Idx(i)] * scale;
    return r;
}
inline Dual Atan2(const Dual &y, const Dual &x) {
    using dual_detail::Idx;
    Dual r;
    r.v = std::atan2(y.v, x.v);
    const double denominator = x.v * x.v + y.v * y.v;
    const double scale = (denominator > 0.0) ? 1.0 / denominator : 0.0;
    for (int i = 0; i < kMaxLocal; ++i) {
        r.d[Idx(i)] = (x.v * y.d[Idx(i)] - y.v * x.d[Idx(i)]) * scale;
    }
    return r;
}

// Lengths and absolute values are softened by a term that is negligible
// against the model but keeps the square root away from zero, where its
// derivative does not exist. Without it a solver step that happens to
// make two points coincide -- which is exactly what a coincidence
// constraint is trying to do -- produces an infinite Jacobian entry and
// the solve ends in NaNs rather than in an answer.
struct Softening {
    double squared = 1e-24;
};

inline Dual SmoothAbs(const Dual &a, const Softening &soft) {
    return Sqrt(a * a + Dual(soft.squared));
}

// Maps each dual slot to the column of the Jacobian it belongs in. A
// parameter that does not exist -- a fixed point's coordinate, a
// grounded instance's position -- gets no slot and enters the arithmetic
// as a constant, which is exactly right: its derivative is zero and there
// is no column to put it in.
struct LocalVars {
    std::array<int, kMaxLocal> column{};
    int count = 0;

    Dual Variable(double value, int global_column) {
        Dual d(value);
        if (global_column < 0 || count >= kMaxLocal) return d;
        d.d[dual_detail::Idx(count)] = 1.0;
        column[dual_detail::Idx(count)] = global_column;
        ++count;
        return d;
    }
};

}  // namespace cad

#endif
