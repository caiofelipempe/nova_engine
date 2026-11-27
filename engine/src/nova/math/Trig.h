#pragma once

/// @file
/// @brief Trigonometry of binary angles (Angle) and fixed-point values.
///
/// The functions compute in `double` with the C++ standard library (`std::sin`, `std::atan2`...)
/// and quantize the result back to the engine's formats: `Unit` (Q1.30) for sines and cosines,
/// Angle for the inverse functions. `double` keeps all 32 bits of an angle (a float has 24).
///
/// Multiples of a quarter turn are exact (sin 90° is 1, cos 90° is 0): the quadrant comes from the
/// angle's two high bits before the standard functions see it.
///
/// @note The standard library is not guaranteed to give the same bits on every compiler and
///       platform. Quantizing to Q1.30 hides almost every difference, but a result that falls
///       within ~10⁻¹⁶ of a rounding boundary may still differ. Every trigonometric call of the
///       simulation goes through this file, so an integer implementation can replace it here.

#include "nova/math/Scalar.h"

#include <cmath>

namespace nova {

namespace detail {
constexpr f64 kTwoPi = 6.28318530717958647692;
// sin and cos of an angle, exact at the quadrant boundaries.
inline void sinCosD(Angle a, f64& s, f64& c) {
    const u32 quadrant = a.raw() >> 30;
    const f64 r = static_cast<f64>(a.raw() & 0x3FFFFFFFu) * (kTwoPi / Angle::kTurn); // [0, π/2)
    const f64 sr = std::sin(r), cr = std::cos(r);
    switch (quadrant) {
        case 0: s = sr, c = cr; break;
        case 1: s = cr, c = -sr; break;
        case 2: s = -sr, c = -cr; break;
        default: s = -cr, c = sr; break;
    }
}
} // namespace detail

/// @brief Sine.
inline Unit sin(Angle a) {
    f64 s, c;
    detail::sinCosD(a, s, c);
    return toFixed<Unit>(s);
}
/// @brief Cosine.
inline Unit cos(Angle a) {
    f64 s, c;
    detail::sinCosD(a, s, c);
    return toFixed<Unit>(c);
}
/// @brief Sine and cosine together.
inline void sinCos(Angle a, Unit& s, Unit& c) {
    f64 sd, cd;
    detail::sinCosD(a, sd, cd);
    s = toFixed<Unit>(sd);
    c = toFixed<Unit>(cd);
}
/// @brief Tangent (saturates near ±90°).
inline Scalar tan(Angle a) {
    f64 s, c;
    detail::sinCosD(a, s, c);
    return toFixed<Scalar>(s / c);
}
/// @brief Arc sine of `v` (clamped to [−1, 1]), in [−90°, 90°].
template <typename T, std::enable_if_t<isFixed<T>, int> = 0>
inline Angle asin(T v) {
    const f64 d = toDouble(v);
    return Angle::fromRadians(std::asin(d < -1.0 ? -1.0 : d > 1.0 ? 1.0 : d));
}
/// @brief Arc cosine of `v` (clamped to [−1, 1]), in [0°, 180°] (180° reads as −180° signed).
template <typename T, std::enable_if_t<isFixed<T>, int> = 0>
inline Angle acos(T v) {
    const f64 d = toDouble(v);
    return Angle::fromRadians(std::acos(d < -1.0 ? -1.0 : d > 1.0 ? 1.0 : d));
}
/// @brief Arc tangent.
template <typename T, std::enable_if_t<isFixed<T>, int> = 0>
inline Angle atan(T v) {
    return Angle::fromRadians(std::atan(toDouble(v)));
}
/// @brief Angle of the point (x, y) from the +X axis (any format, both the same or not).
template <typename Y, typename X, std::enable_if_t<isFixed<Y> && isFixed<X>, int> = 0>
inline Angle atan2(Y y, X x) {
    return Angle::fromRadians(std::atan2(toDouble(y), toDouble(x)));
}

/// @brief Square root of a non-negative value (fpm, exact rounding; negative gives 0).
template <typename T, std::enable_if_t<isFixed<T> && (storageBits<T> <= 32), int> = 0>
inline T sqrt(T v) {
    if (v.raw_value() <= 0) return zero<T>();
    return T(fpm::sqrt(typename T::Base(v)));
}

} // namespace nova
