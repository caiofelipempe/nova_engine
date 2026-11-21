#pragma once

/// @file
/// @brief Integer helpers shared by exact world positions and time: saturating int64 arithmetic
///        and a small unsigned 128-bit integer.

#include "nova/core/Base.h"

#include <limits>

namespace nova {

/// @brief Unsigned 128-bit integer (exact products and squared lengths of int64 values).
struct UInt128 {
    u64 hi = 0; ///< High 64 bits.
    u64 lo = 0; ///< Low 64 bits.

    /// @brief Full product of two 64-bit values.
    static constexpr UInt128 mul(u64 a, u64 b) {
        const u64 aLo = a & 0xFFFFFFFFull, aHi = a >> 32;
        const u64 bLo = b & 0xFFFFFFFFull, bHi = b >> 32;
        const u64 ll = aLo * bLo, lh = aLo * bHi, hl = aHi * bLo, hh = aHi * bHi;
        const u64 mid = (ll >> 32) + (lh & 0xFFFFFFFFull) + (hl & 0xFFFFFFFFull);
        return {hh + (lh >> 32) + (hl >> 32) + (mid >> 32), (mid << 32) | (ll & 0xFFFFFFFFull)};
    }
    /// @brief Sum (wraps at 2^128).
    friend constexpr UInt128 operator+(UInt128 a, UInt128 b) {
        UInt128 r{a.hi + b.hi, a.lo + b.lo};
        if (r.lo < a.lo) ++r.hi;
        return r;
    }
    friend bool operator==(const UInt128&, const UInt128&) = default;
    friend constexpr bool operator<(const UInt128& a, const UInt128& b) { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }
    friend constexpr bool operator>(const UInt128& a, const UInt128& b) { return b < a; }
    friend constexpr bool operator<=(const UInt128& a, const UInt128& b) { return !(b < a); }
    friend constexpr bool operator>=(const UInt128& a, const UInt128& b) { return !(a < b); }
    /// @brief Nearest double (53 significant bits).
    constexpr f64 toDouble() const { return static_cast<f64>(hi) * 18446744073709551616.0 + static_cast<f64>(lo); }
};

namespace detail {

constexpr i64 kI64Max = std::numeric_limits<i64>::max();
constexpr i64 kI64Min = std::numeric_limits<i64>::min();

/// @brief `a + b`, saturating at the int64 limits.
constexpr i64 saturatingAdd(i64 a, i64 b) {
    if (b > 0 && a > kI64Max - b) return kI64Max;
    if (b < 0 && a < kI64Min - b) return kI64Min;
    return a + b;
}
/// @brief `-a`, saturating (−INT64_MIN → INT64_MAX).
constexpr i64 saturatingNegate(i64 a) { return a == kI64Min ? kI64Max : -a; }
/// @brief `|a|` as unsigned (no overflow at INT64_MIN).
constexpr u64 magnitude(i64 a) { return a >= 0 ? static_cast<u64>(a) : static_cast<u64>(-(a + 1)) + 1; }
/// @brief `a * b`, saturating at the int64 limits.
constexpr i64 saturatingMul(i64 a, i64 b) {
    const bool negative = (a < 0) != (b < 0);
    const UInt128 p = UInt128::mul(magnitude(a), magnitude(b));
    const u64 limit = negative ? (u64(1) << 63) : static_cast<u64>(kI64Max);
    if (p.hi != 0 || p.lo > limit) return negative ? kI64Min : kI64Max;
    return negative ? static_cast<i64>(0 - p.lo) : static_cast<i64>(p.lo);
}

} // namespace detail

} // namespace nova
