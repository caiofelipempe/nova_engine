#pragma once

/// @file
/// @brief Fixed-point numbers of the simulation state (lib `fpm`): scalar types, vectors and
///        quaternions, each with its binary point placed for the smallest variation it must carry.
///
/// Rule for the binary point: one physics step (1/60 s) of the smallest meaningful value must move
/// things by at most 3.8 µm (2⁻¹⁸ m), well below Jolt's 0.1 mm collision tolerance. Every value is
/// an `int32` (products use an `int64` intermediate), so arithmetic is exact, needs no 128 bits
/// and gives the same bits on every platform and compiler.
///
/// | Alias    | Format | Step            | Range         | Used for                                  |
/// |----------|--------|-----------------|---------------|-------------------------------------------|
/// | Length   | Q13.18 | 3.8 µm          | ±8 192 m      | positions in the 16 km world, sizes, offsets |
/// | Distance | Q14.17 | 7.6 µm          | ±16 384 m     | lengths of vectors (torus diagonals)      |
/// | Speed    | Q15.16 | 15 µm/s         | ±32 768 m/s   | velocity, acceleration, gravity           |
/// | Scalar   | Q15.16 | 1.5·10⁻⁵        | ±32 768       | scale, coefficients, mass, angular speed  |
/// | Angle    | Q3.28  | 3.7·10⁻⁹ rad    | ±8 rad        | angles, limits, slopes                    |
/// | Unit     | Q1.30  | 9.3·10⁻¹⁰       | ±2            | quaternion components, unit directions    |
/// | Force    | Q23.8  | 4 mN            | ±8.4 MN       | forces, impulses, torques                 |
///
/// Angles and quaternions follow the rule for the bodies they turn: one step of rotation moves a
/// point 1 km from the center (the largest single body) by at most 3.8 µm.
///
/// Overflow of a signed value is undefined behavior in C++: values must stay in range (world
/// positions that wrap around do their wrapping arithmetic in unsigned integers, see GlobalPosition).

#include "nova/core/Base.h"

#include <fpm/fixed.hpp>
#include <fpm/math.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace nova {

/// @brief A 32-bit fixed-point number with `F` fraction bits.
template <unsigned F>
using Fixed = fpm::fixed<i32, i64, F>;

using Fx13_18 = Fixed<18>; ///< Q13.18.
using Fx14_17 = Fixed<17>; ///< Q14.17.
using Fx15_16 = Fixed<16>; ///< Q15.16.
using Fx3_28 = Fixed<28>;  ///< Q3.28.
using Fx1_30 = Fixed<30>;  ///< Q1.30.
using Fx23_8 = Fixed<8>;   ///< Q23.8.

using Length = Fx13_18;   ///< Meters: positions in the world, sizes, offsets (3.8 µm, ±8 192 m).
using Distance = Fx14_17; ///< Meters: lengths of vectors, up to a torus diagonal (7.6 µm, ±16 384 m).
using Speed = Fx15_16;    ///< m/s or m/s²: velocity, acceleration, gravity.
using Scalar = Fx15_16;   ///< Dimensionless or kg or rad/s: scale, coefficients, mass, angular speed.
using Angle = Fx3_28;     ///< Radians, |a| < 8.
using Unit = Fx1_30;      ///< Quaternion components and unit directions, |u| < 2.
using Force = Fx23_8;     ///< N, N·s or N·m.

/// @brief Fraction bits of a Fixed type.
template <typename T>
struct FixedTraits;
template <unsigned F>
struct FixedTraits<Fixed<F>> {
    static constexpr unsigned fraction = F; ///< Fraction bits.
};
/// @brief Fraction bits of a Fixed type.
template <typename T>
constexpr unsigned fractionBits = FixedTraits<T>::fraction;

/// @brief A Fixed value from its raw integer.
template <typename T>
constexpr T fromRaw(i32 raw) { return T::from_raw_value(raw); }
/// @brief Zero of a Fixed type (fpm leaves default-constructed values uninitialized).
template <typename T>
constexpr T zero() { return T::from_raw_value(0); }
/// @brief The nearest value of `T` to `v` (rounded, not truncated).
template <typename T>
inline T toFixed(f64 v) {
    return T::from_raw_value(static_cast<i32>(std::llround(v * static_cast<f64>(1ll << fractionBits<T>))));
}
/// @brief Exact value as a double.
template <typename T>
constexpr f64 toDouble(T v) { return static_cast<f64>(v.raw_value()) / static_cast<f64>(1ll << fractionBits<T>); }
/// @brief Nearest float (for the GPU, ImGui and the physics engine).
template <typename T>
constexpr f32 toFloat(T v) { return static_cast<f32>(toDouble(v)); }

namespace detail {
// (p / 2^S) rounded to nearest, ties away from zero (the same on every platform); S < 0 multiplies.
template <int S>
constexpr i64 roundShift(i64 p) {
    if constexpr (S <= 0) {
        return p * (i64(1) << -S);
    } else {
        constexpr i64 half = i64(1) << (S - 1);
        return p >= 0 ? (p + half) >> S : -((-p + half) >> S);
    }
}
} // namespace detail

/// @brief Product of two Fixed values of any formats, in format `R` (exact 64-bit product, rounded).
template <typename R, typename A, typename B>
constexpr R mul(A a, B b) {
    const i64 p = static_cast<i64>(a.raw_value()) * b.raw_value();
    constexpr int shift = int(fractionBits<A>) + int(fractionBits<B>) - int(fractionBits<R>);
    return R::from_raw_value(static_cast<i32>(detail::roundShift<shift>(p)));
}
/// @brief Quotient `a / b` in format `R` (exact 64-bit division, rounded).
template <typename R, typename A, typename B>
constexpr R div(A a, B b) {
    // a / b in R = (aRaw · 2^(FR + FB − FA)) / bRaw.
    constexpr int shift = int(fractionBits<R>) + int(fractionBits<B>) - int(fractionBits<A>);
    i64 n = a.raw_value();
    if constexpr (shift >= 0) n *= i64(1) << shift;
    else n >>= -shift;
    const i64 d = b.raw_value();
    const i64 q = (n >= 0) == (d > 0) ? (n + d / 2) / d : (n - d / 2) / d;
    return R::from_raw_value(static_cast<i32>(q));
}
/// @brief The same value in another format (rounded when it loses bits).
template <typename R, typename A>
constexpr R convert(A a) {
    constexpr int shift = int(fractionBits<A>) - int(fractionBits<R>);
    return R::from_raw_value(static_cast<i32>(detail::roundShift<shift>(a.raw_value())));
}

/// @brief Integer square root: floor(√v), exact for every 64-bit value.
constexpr u64 isqrt(u64 v) {
    u64 r = 0, bit = u64(1) << 62;
    while (bit > v) bit >>= 2;
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/// @brief A 3D vector of Fixed values (glm only accepts IEEE types).
template <typename T>
struct FixedVec3 {
    T x = zero<T>(), y = zero<T>(), z = zero<T>();

    constexpr FixedVec3() = default;
    constexpr FixedVec3(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}
    /// @brief The nearest vector to `v`.
    static FixedVec3 fromGlm(const glm::dvec3& v) { return {toFixed<T>(v.x), toFixed<T>(v.y), toFixed<T>(v.z)}; }
    /// @brief The nearest vector to `v`.
    static FixedVec3 fromGlm(const glm::vec3& v) { return fromGlm(glm::dvec3(v)); }
    /// @brief As floats (GPU, physics engine).
    glm::vec3 toGlm() const { return {toFloat(x), toFloat(y), toFloat(z)}; }
    /// @brief Exact, as doubles.
    glm::dvec3 toDouble() const { return {nova::toDouble(x), nova::toDouble(y), nova::toDouble(z)}; }
    /// @brief The same vector in another format.
    template <typename R>
    constexpr FixedVec3<R> as() const { return {convert<R>(x), convert<R>(y), convert<R>(z)}; }

    T& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    const T& operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }

    friend constexpr bool operator==(const FixedVec3& a, const FixedVec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
    friend constexpr FixedVec3 operator+(const FixedVec3& a, const FixedVec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr FixedVec3 operator-(const FixedVec3& a, const FixedVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr FixedVec3 operator-(const FixedVec3& a) { return {-a.x, -a.y, -a.z}; }
    FixedVec3& operator+=(const FixedVec3& o) { return *this = *this + o; }
    FixedVec3& operator-=(const FixedVec3& o) { return *this = *this - o; }

    /// @brief Each component times `s` (any format), result in this vector's format.
    template <typename S>
    constexpr FixedVec3 scaled(S s) const { return {mul<T>(x, s), mul<T>(y, s), mul<T>(z, s)}; }

    /// @brief Exact squared length, in raw units² (2·F fraction bits). No overflow for any value.
    constexpr u64 lengthSquaredRaw() const {
        auto sq = [](T v) { const i64 r = v.raw_value(); return static_cast<u64>(r * r); };
        return sq(x) + sq(y) + sq(z); // each ≤ 2^62: the sum fits in 64 bits
    }
    /// @brief Length, rounded down to the format's step (exact integer square root).
    constexpr Distance length() const {
        const u64 rootRaw = isqrt(lengthSquaredRaw()); // in this format's fraction
        constexpr int shift = int(fractionBits<T>) - int(fractionBits<Distance>);
        return Distance::from_raw_value(static_cast<i32>(detail::roundShift<shift>(static_cast<i64>(rootRaw))));
    }
    /// @brief Exact: is the vector shorter than `r`?
    constexpr bool isShorterThan(T r) const {
        const u64 rr = static_cast<u64>(static_cast<i64>(r.raw_value()) * r.raw_value());
        return lengthSquaredRaw() < rr;
    }
    /// @brief Unit vector in the same direction (zero for a zero vector).
    constexpr FixedVec3<Unit> normalized() const {
        const u64 len = isqrt(lengthSquaredRaw());
        if (len == 0) return {};
        auto n = [&](T v) {
            const i64 num = static_cast<i64>(v.raw_value()) << fractionBits<Unit>; // ≤ 2^61
            const i64 l = static_cast<i64>(len);
            return Unit::from_raw_value(static_cast<i32>(num >= 0 ? (num + l / 2) / l : (num - l / 2) / l));
        };
        return {n(x), n(y), n(z)};
    }
};

/// @brief Dot product of two vectors, in format `R`.
template <typename R, typename A, typename B>
constexpr R dot(const FixedVec3<A>& a, const FixedVec3<B>& b) {
    const i64 p = static_cast<i64>(a.x.raw_value()) * b.x.raw_value() + static_cast<i64>(a.y.raw_value()) * b.y.raw_value() +
                  static_cast<i64>(a.z.raw_value()) * b.z.raw_value();
    constexpr int shift = int(fractionBits<A>) + int(fractionBits<B>) - int(fractionBits<R>);
    return R::from_raw_value(static_cast<i32>(detail::roundShift<shift>(p)));
}
/// @brief Cross product of two vectors, in format `R`.
template <typename R, typename A, typename B>
constexpr FixedVec3<R> cross(const FixedVec3<A>& a, const FixedVec3<B>& b) {
    constexpr int shift = int(fractionBits<A>) + int(fractionBits<B>) - int(fractionBits<R>);
    auto c = [&](A p, B q, A r, B s) {
        const i64 v = static_cast<i64>(p.raw_value()) * q.raw_value() - static_cast<i64>(r.raw_value()) * s.raw_value();
        return R::from_raw_value(static_cast<i32>(detail::roundShift<shift>(v)));
    };
    return {c(a.y, b.z, a.z, b.y), c(a.z, b.x, a.x, b.z), c(a.x, b.y, a.y, b.x)};
}

using LengthVec = FixedVec3<Length>; ///< Offsets and sizes in meters.
using SpeedVec = FixedVec3<Speed>;   ///< Velocities, accelerations.
using ScalarVec = FixedVec3<Scalar>; ///< Scales, angular velocities.
using UnitVec = FixedVec3<Unit>;     ///< Directions, axes.
using ForceVec = FixedVec3<Force>;   ///< Forces, impulses, torques.

/// @brief A rotation: unit quaternion with Q1.30 components.
struct FixedQuat {
    Unit w = fromRaw<Unit>(1 << 30), x = zero<Unit>(), y = zero<Unit>(), z = zero<Unit>();

    /// @brief The identity rotation.
    static constexpr FixedQuat identity() { return {}; }
    /// @brief The nearest quaternion to `q` (normalized).
    static FixedQuat fromGlm(const glm::quat& q) {
        const glm::dquat n = glm::normalize(glm::dquat(q));
        FixedQuat r;
        r.w = toFixed<Unit>(n.w);
        r.x = toFixed<Unit>(n.x);
        r.y = toFixed<Unit>(n.y);
        r.z = toFixed<Unit>(n.z);
        return r;
    }
    /// @brief As floats (GPU, physics engine).
    glm::quat toGlm() const { return glm::quat(toFloat(w), toFloat(x), toFloat(y), toFloat(z)); }
    /// @brief Rotation of `angle` around unit `axis` (deterministic sin/cos of fpm).
    static FixedQuat fromAxisAngle(const UnitVec& axis, Angle angle) {
        const Angle half = Angle::from_raw_value(angle.raw_value() / 2);
        const Unit s = convert<Unit>(fpm::sin(half)), c = convert<Unit>(fpm::cos(half));
        FixedQuat q;
        q.w = c;
        q.x = mul<Unit>(axis.x, s);
        q.y = mul<Unit>(axis.y, s);
        q.z = mul<Unit>(axis.z, s);
        return q.normalized();
    }

    friend constexpr bool operator==(const FixedQuat& a, const FixedQuat& b) {
        return a.w == b.w && a.x == b.x && a.y == b.y && a.z == b.z;
    }
    /// @brief The inverse rotation (for unit quaternions).
    constexpr FixedQuat conjugate() const {
        FixedQuat q;
        q.w = w;
        q.x = -x;
        q.y = -y;
        q.z = -z;
        return q;
    }
    /// @brief Composition: `a * b` applies `b` first, then `a` (as glm).
    friend constexpr FixedQuat operator*(const FixedQuat& a, const FixedQuat& b) {
        auto p = [](Unit u, Unit v) { return static_cast<i64>(u.raw_value()) * v.raw_value(); };
        auto r = [](i64 v) { return Unit::from_raw_value(static_cast<i32>(detail::roundShift<30>(v))); };
        FixedQuat q;
        q.w = r(p(a.w, b.w) - p(a.x, b.x) - p(a.y, b.y) - p(a.z, b.z));
        q.x = r(p(a.w, b.x) + p(a.x, b.w) + p(a.y, b.z) - p(a.z, b.y));
        q.y = r(p(a.w, b.y) - p(a.x, b.z) + p(a.y, b.w) + p(a.z, b.x));
        q.z = r(p(a.w, b.z) + p(a.x, b.y) - p(a.y, b.x) + p(a.z, b.w));
        return q;
    }
    /// @brief Back to unit length (exact integer square root).
    constexpr FixedQuat normalized() const {
        auto sq = [](Unit v) { const i64 r = v.raw_value(); return static_cast<u64>(r * r); };
        const u64 len = isqrt(sq(w) + sq(x) + sq(y) + sq(z)); // components < 2: no overflow
        if (len == 0) return identity();
        auto n = [&](Unit v) {
            const i64 num = static_cast<i64>(v.raw_value()) << 30;
            const i64 l = static_cast<i64>(len);
            return Unit::from_raw_value(static_cast<i32>(num >= 0 ? (num + l / 2) / l : (num - l / 2) / l));
        };
        FixedQuat q;
        q.w = n(w);
        q.x = n(x);
        q.y = n(y);
        q.z = n(z);
        return q;
    }
    /// @brief `v` rotated (any vector format): v + 2w(q×v) + 2q×(q×v). Intermediates stay in 64 bits,
    ///        so any vector shorter than the format's range rotates without overflow (one rounding
    ///        per term); the result has the same length, so it stays in range too.
    template <typename T>
    constexpr FixedVec3<T> rotate(const FixedVec3<T>& v) const {
        const i64 ux = x.raw_value(), uy = y.raw_value(), uz = z.raw_value(), uw = w.raw_value();
        const i64 vx = v.x.raw_value(), vy = v.y.raw_value(), vz = v.z.raw_value();
        // t = 2 (u × v), in the vector's units.
        const i64 tx = 2 * detail::roundShift<30>(uy * vz - uz * vy);
        const i64 ty = 2 * detail::roundShift<30>(uz * vx - ux * vz);
        const i64 tz = 2 * detail::roundShift<30>(ux * vy - uy * vx);
        const i64 rx = vx + detail::roundShift<30>(uw * tx) + detail::roundShift<30>(uy * tz - uz * ty);
        const i64 ry = vy + detail::roundShift<30>(uw * ty) + detail::roundShift<30>(uz * tx - ux * tz);
        const i64 rz = vz + detail::roundShift<30>(uw * tz) + detail::roundShift<30>(ux * ty - uy * tx);
        return {T::from_raw_value(static_cast<i32>(rx)), T::from_raw_value(static_cast<i32>(ry)),
                T::from_raw_value(static_cast<i32>(rz))};
    }
};

} // namespace nova
