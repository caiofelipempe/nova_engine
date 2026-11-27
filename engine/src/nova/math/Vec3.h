#pragma once

/// @file
/// @brief Vec3: a 3D vector of fixed-point values (any Fixed format, 8 to 64 bits, or Angle),
///        converted to and from glm.

#include "nova/math/Scalar.h"

#include <glm/glm.hpp>

namespace nova {

/// @brief A 3D vector of Fixed values (glm only accepts IEEE types), or of Angles (Euler angles).
///
/// Built from glm implicitly (`scale = glm::vec3(2.0f)`: this is where a float vector is quantized)
/// or from numbers (`offset = {0, 0.9, 0}`); back to glm only explicitly (toGlm()). Lengths,
/// normalization, dot and cross products need 32-bit components (their products fit in 64 bits).
template <typename T>
struct Vec3 {
    using Component = T; ///< The component type.

    T x = zero<T>(), y = zero<T>(), z = zero<T>();

    constexpr Vec3() = default;
    constexpr Vec3(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}
    /// @brief The nearest vector to (x, y, z), e.g. `offset = {0, 0.9, 0}` (radians for angles).
    Vec3(f64 x_, f64 y_, f64 z_) : x(toFixed<T>(x_)), y(toFixed<T>(y_)), z(toFixed<T>(z_)) {}
    /// @brief The nearest vector to `v` (implicit: this is where a float vector is quantized).
    Vec3(const glm::vec3& v) : Vec3(f64(v.x), f64(v.y), f64(v.z)) {}
    /// @brief The nearest vector to `v`.
    Vec3(const glm::dvec3& v) : Vec3(v.x, v.y, v.z) {}
    /// @brief The nearest vector to `v`.
    static Vec3 fromGlm(const glm::dvec3& v) { return Vec3(v); }
    /// @brief The nearest vector to `v`.
    static Vec3 fromGlm(const glm::vec3& v) { return Vec3(v); }
    /// @brief The same value in every component.
    static constexpr Vec3 splat(T v) { return {v, v, v}; }
    /// @brief As floats (GPU, physics engine).
    glm::vec3 toGlm() const { return {toFloat(x), toFloat(y), toFloat(z)}; }
    /// @brief As floats.
    explicit operator glm::vec3() const { return toGlm(); }
    /// @brief Exact, as doubles.
    glm::dvec3 toDouble() const { return {nova::toDouble(x), nova::toDouble(y), nova::toDouble(z)}; }
    /// @brief The same vector in another format.
    template <typename R>
    constexpr Vec3<R> as() const { return {convert<R>(x), convert<R>(y), convert<R>(z)}; }

    T& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    const T& operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }

    friend constexpr bool operator==(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
    friend constexpr Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr Vec3 operator-(const Vec3& a) { return {-a.x, -a.y, -a.z}; }
    Vec3& operator+=(const Vec3& o) { return *this = *this + o; }
    Vec3& operator-=(const Vec3& o) { return *this = *this - o; }

    /// @brief Each component times `s` (any format), result in this vector's format.
    template <typename S>
    constexpr Vec3 scaled(S s) const { return {mul<T>(x, s), mul<T>(y, s), mul<T>(z, s)}; }

    /// @brief Exact squared length, in raw units² (2·F fraction bits). No overflow for any value.
    constexpr u64 lengthSquaredRaw() const {
        static_assert(storageBits<T> <= 32, "lengthSquaredRaw: 64-bit components need 128 bits");
        auto sq = [](T v) { const i64 r = v.raw_value(); return static_cast<u64>(r * r); };
        return sq(x) + sq(y) + sq(z); // each ≤ 2^62: the sum fits in 64 bits
    }
    /// @brief Length in format `R` (default Distance), rounded down to this format's step (exact
    ///        integer square root).
    template <typename R = Distance>
    constexpr R length() const {
        const u64 rootRaw = isqrt(lengthSquaredRaw()); // in this format's fraction
        constexpr int shift = int(fractionBits<T>) - int(fractionBits<R>);
        return R::from_raw_value(static_cast<StorageOf<R>>(detail::roundShift<shift>(static_cast<i64>(rootRaw))));
    }
    /// @brief Exact: is the vector shorter than `r`?
    constexpr bool isShorterThan(T r) const {
        const u64 rr = static_cast<u64>(static_cast<i64>(r.raw_value()) * r.raw_value());
        return lengthSquaredRaw() < rr;
    }
    /// @brief Unit vector in the same direction (zero for a zero vector).
    constexpr Vec3<Unit> normalized() const {
        const u64 len = isqrt(lengthSquaredRaw());
        if (len == 0) return {};
        auto n = [&](T v) {
            const i64 num = static_cast<i64>(v.raw_value()) * (i64(1) << fractionBits<Unit>); // ≤ 2^61
            return Unit::from_raw_value(static_cast<i32>(detail::roundDiv(num, static_cast<i64>(len))));
        };
        return {n(x), n(y), n(z)};
    }
};

/// @brief Dot product of two vectors, in format `R`.
template <typename R, typename A, typename B>
constexpr R dot(const Vec3<A>& a, const Vec3<B>& b) {
    static_assert(storageBits<A> + storageBits<B> <= 64, "dot: the products need more than 64 bits");
    const i64 p = static_cast<i64>(a.x.raw_value()) * b.x.raw_value() + static_cast<i64>(a.y.raw_value()) * b.y.raw_value() +
                  static_cast<i64>(a.z.raw_value()) * b.z.raw_value();
    constexpr int shift = int(fractionBits<A>) + int(fractionBits<B>) - int(fractionBits<R>);
    return R::from_raw_value(static_cast<StorageOf<R>>(detail::roundShift<shift>(p)));
}
/// @brief Cross product of two vectors, in format `R`.
template <typename R, typename A, typename B>
constexpr Vec3<R> cross(const Vec3<A>& a, const Vec3<B>& b) {
    static_assert(storageBits<A> + storageBits<B> <= 64, "cross: the products need more than 64 bits");
    constexpr int shift = int(fractionBits<A>) + int(fractionBits<B>) - int(fractionBits<R>);
    auto c = [&](A p, B q, A r, B s) {
        const i64 v = static_cast<i64>(p.raw_value()) * q.raw_value() - static_cast<i64>(r.raw_value()) * s.raw_value();
        return R::from_raw_value(static_cast<StorageOf<R>>(detail::roundShift<shift>(v)));
    };
    return {c(a.y, b.z, a.z, b.y), c(a.z, b.x, a.x, b.z), c(a.x, b.y, a.y, b.x)};
}

using LengthVec = Vec3<Length>;             ///< Offsets and sizes in meters.
using DistanceVec = Vec3<Distance>;         ///< Longer offsets in meters.
using SpeedVec = Vec3<Speed>;               ///< Velocities.
using AccelerationVec = Vec3<Acceleration>; ///< Accelerations, gravity.
using ScaleVec = Vec3<Scale>;               ///< Scales per axis.
using UnitVec = Vec3<Unit>;                 ///< Directions, axes.
using ForceVec = Vec3<Force>;               ///< Forces, impulses, torques.
using EulerAngles = Vec3<Angle>;            ///< Euler angles: x = pitch, y = yaw, z = roll (see EulerOrder).
using GlobalLengthVec = Vec3<GlobalLength>; ///< Coordinates in the whole world.

} // namespace nova
