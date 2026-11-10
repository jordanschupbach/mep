#ifndef MEP_UINT128_H
#define MEP_UINT128_H

// mep::u128: an unsigned 128-bit integer for the literal-range checks in
// the Rust and Crystal parsers. GCC and Clang have `unsigned __int128`;
// MSVC does not, so there it is a small two-word struct with just the
// operations those checks use (division only by values below 2^32).

#include <cassert>
#include <cstdint>

namespace mep {

#if defined(__SIZEOF_INT128__)

using u128 = unsigned __int128;

#else

class u128 {
public:
    constexpr u128() = default;
    constexpr u128(uint64_t v) : lo_(v) {}  // NOLINT(google-explicit-constructor): mirrors the builtin

    template <typename T>
    constexpr explicit operator T() const { return static_cast<T>(lo_); }

    friend constexpr u128 operator~(u128 a) { return Make(~a.hi_, ~a.lo_); }
    friend constexpr u128 operator+(u128 a, u128 b) {
        const uint64_t lo = a.lo_ + b.lo_;
        return Make(a.hi_ + b.hi_ + (lo < a.lo_ ? 1 : 0), lo);
    }
    friend constexpr u128 operator-(u128 a, u128 b) {
        return Make(a.hi_ - b.hi_ - (a.lo_ < b.lo_ ? 1 : 0), a.lo_ - b.lo_);
    }
    friend constexpr u128 operator*(u128 a, u128 b) {
        // (a.hi*2^64 + a.lo) * (b.hi*2^64 + b.lo), mod 2^128.
        u128 r = Mul64(a.lo_, b.lo_);
        r.hi_ += a.hi_ * b.lo_ + a.lo_ * b.hi_;
        return r;
    }
    friend constexpr u128 operator/(u128 a, u128 b) { return DivMod(a, b, nullptr); }
    friend constexpr u128 operator%(u128 a, u128 b) {
        uint64_t rem = 0;
        DivMod(a, b, &rem);
        return rem;
    }
    friend constexpr u128 operator<<(u128 a, int n) {
        if (n <= 0) return a;
        if (n >= 128) return {};
        if (n >= 64) return Make(a.lo_ << (n - 64), 0);
        return Make((a.hi_ << n) | (a.lo_ >> (64 - n)), a.lo_ << n);
    }
    friend constexpr bool operator==(u128 a, u128 b) { return a.hi_ == b.hi_ && a.lo_ == b.lo_; }
    friend constexpr bool operator!=(u128 a, u128 b) { return !(a == b); }
    friend constexpr bool operator<(u128 a, u128 b) { return a.hi_ != b.hi_ ? a.hi_ < b.hi_ : a.lo_ < b.lo_; }
    friend constexpr bool operator>(u128 a, u128 b) { return b < a; }
    friend constexpr bool operator<=(u128 a, u128 b) { return !(b < a); }
    friend constexpr bool operator>=(u128 a, u128 b) { return !(a < b); }

    constexpr u128 &operator+=(u128 b) { return *this = *this + b; }
    constexpr u128 &operator*=(u128 b) { return *this = *this * b; }
    constexpr u128 &operator/=(u128 b) { return *this = *this / b; }

private:
    static constexpr u128 Make(uint64_t hi, uint64_t lo) {
        u128 r;
        r.hi_ = hi;
        r.lo_ = lo;
        return r;
    }
    // The full 128-bit product of two 64-bit values.
    static constexpr u128 Mul64(uint64_t a, uint64_t b) {
        const uint64_t a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
        const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
        const uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
        return Make(p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (p00 & 0xffffffffu));
    }
    // Long division by a divisor below 2^32, one 32-bit digit at a time.
    static constexpr u128 DivMod(u128 a, u128 b, uint64_t *rem) {
        assert(b.hi_ == 0 && b.lo_ != 0 && b.lo_ <= 0xffffffffu);
        const uint64_t d = b.lo_;
        const uint64_t digits[4] = {a.hi_ >> 32, a.hi_ & 0xffffffffu, a.lo_ >> 32, a.lo_ & 0xffffffffu};
        uint64_t q[4] = {};
        uint64_t r = 0;
        for (int i = 0; i < 4; i++) {
            const uint64_t x = (r << 32) | digits[i];
            q[i] = x / d;
            r = x % d;
        }
        if (rem) *rem = r;
        return Make((q[0] << 32) | q[1], (q[2] << 32) | q[3]);
    }

    uint64_t hi_ = 0;
    uint64_t lo_ = 0;
};

#endif

}  // namespace mep

#endif
