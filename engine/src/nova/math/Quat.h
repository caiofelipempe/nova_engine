#pragma once

/// @file
/// @brief Quat: rotations as fixed-point unit quaternions (Q1.30 by default; any Fixed format with
///        one integer bit), converted to and from glm, with every rotation operation of the engine:
///        composition, rotation of vectors, axis-angle, Euler angles in any of the six axis orders,
///        slerp/nlerp, angle between rotations and rotateTowards.
///
/// Rotations are stored as quaternions (TransformComponent::rotation); Euler angles are a view for
/// people. Angles are always given per axis — `x` = pitch (around X), `y` = yaw (around Y),
/// `z` = roll (around Z) — and the EulerOrder says in which order they are applied.

#include "nova/math/Trig.h"
#include "nova/math/Vec3.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace nova {

/// @brief Order in which the three axis rotations are applied, around the object's **local** axes
///        (intrinsic): `YXZ` = yaw first, then pitch around the new X, then roll around the new Z.
///
/// Equivalently, the quaternion is the product in that order: `YXZ` → `qY * qX * qZ`.
/// Reading the letters backwards gives the same rotation around the fixed world axes (extrinsic).
enum class EulerOrder : u8 {
    XYZ,
    XZY,
    YXZ, ///< Yaw → pitch → roll: cameras and characters (the engine default; same as Unity).
    YZX,
    ZXY,
    ZYX, ///< Roll → yaw → pitch (local); what `glm::quat(vec3)` and older Nova scene files use.
};

/// @brief Default order of the engine's Euler helpers.
constexpr EulerOrder kDefaultEulerOrder = EulerOrder::YXZ;

/// @brief Name of an order ("YXZ").
const char* eulerOrderName(EulerOrder order);

namespace detail {
// Euler angles in radians that rebuild the unit quaternion `q` in `order` (in double).
glm::dvec3 eulerFromQuat(const glm::dquat& q, EulerOrder order);
} // namespace detail

/// @brief A rotation: a unit quaternion of fixed-point components `T` (Q1.30 by default, |c| < 2).
///
/// Built from a `glm::quat` implicitly (normalized and quantized: `rotation = glm::angleAxis(...)`),
/// back to glm only explicitly (toGlm()). Products of unit quaternions drift from length 1 only by
/// rounding: normalized() fixes it exactly, renormalize() only when it is needed.
template <typename T = Unit>
struct QuatT {
    static_assert(storageBits<T> <= 32, "Quat: components of up to 32 bits (products in 64)");
    static_assert(storageBits<T> - 1 - fractionBits<T> == 1, "Quat: components need exactly one integer bit");
    static constexpr int kF = int(fractionBits<T>); ///< Fraction bits of the components.
    using Component = T;                             ///< The component type.

    T w = fromRaw<T>(i64(1) << kF), x = zero<T>(), y = zero<T>(), z = zero<T>();

    /// @brief The identity rotation.
    constexpr QuatT() = default;
    /// @brief The nearest unit quaternion to `q` (implicit: this is where a float rotation is quantized).
    QuatT(const glm::quat& q) : QuatT(glm::dquat(q)) {}
    /// @brief The nearest unit quaternion to `q`.
    QuatT(const glm::dquat& q) {
        const f64 len = glm::length(q);
        if (!(len > 1e-12)) return; // identity
        const glm::dquat n = q / len;
        w = toFixed<T>(n.w);
        x = toFixed<T>(n.x);
        y = toFixed<T>(n.y);
        z = toFixed<T>(n.z);
    }
    /// @brief From raw components (taken as they are).
    static constexpr QuatT fromComponents(T w_, T x_, T y_, T z_) {
        QuatT q;
        q.w = w_;
        q.x = x_;
        q.y = y_;
        q.z = z_;
        return q;
    }

    /// @brief The identity rotation.
    static constexpr QuatT identity() { return {}; }
    /// @brief The nearest quaternion to `q` (normalized).
    static QuatT fromGlm(const glm::quat& q) { return QuatT(q); }
    /// @brief As floats (GPU, physics engine).
    glm::quat toGlm() const { return glm::quat(toFloat(w), toFloat(x), toFloat(y), toFloat(z)); }
    /// @brief Exact, as doubles.
    glm::dquat toDouble() const {
        return glm::dquat(nova::toDouble(w), nova::toDouble(x), nova::toDouble(y), nova::toDouble(z));
    }
    /// @brief As floats.
    explicit operator glm::quat() const { return toGlm(); }
    /// @brief The same rotation with components of another format.
    template <typename R>
    constexpr QuatT<R> as() const { return QuatT<R>::fromComponents(convert<R>(w), convert<R>(x), convert<R>(y), convert<R>(z)); }

    /// @name Building rotations
    /// @{
    /// @brief Rotation of `angle` around `axis`, of any length and format (normalized here: an axis
    ///        that is not unit length turns by `angle` all the same; zero axis: identity).
    template <typename A>
    static QuatT fromAxisAngle(const Vec3<A>& axis, Angle angle) {
        const UnitVec n = axis.normalized(); // already unit: the same bits back, to a step
        if (n == UnitVec()) return identity();
        // Half of the signed reading: q and −q are the same rotation, so the half turn is unambiguous.
        const Angle half = Angle::fromRaw(static_cast<u32>(angle.signedRaw() >> 1));
        Unit s, c;
        sinCos(half, s, c);
        return fromComponents(convert<T>(c), mul<T>(n.x, s), mul<T>(n.y, s), mul<T>(n.z, s)).normalized();
    }
    /// @brief Rotation of `angle` around the X axis.
    static QuatT aroundX(Angle angle) { return fromAxisAngle(UnitVec(1.0, 0.0, 0.0), angle); }
    /// @brief Rotation of `angle` around the Y axis.
    static QuatT aroundY(Angle angle) { return fromAxisAngle(UnitVec(0.0, 1.0, 0.0), angle); }
    /// @brief Rotation of `angle` around the Z axis.
    static QuatT aroundZ(Angle angle) { return fromAxisAngle(UnitVec(0.0, 0.0, 1.0), angle); }
    /// @brief Rotation from Euler angles (`x` pitch, `y` yaw, `z` roll) applied in `order`.
    static QuatT fromEuler(const EulerAngles& e, EulerOrder order = kDefaultEulerOrder) {
        const QuatT qx = aroundX(e.x), qy = aroundY(e.y), qz = aroundZ(e.z);
        QuatT q;
        switch (order) { // intrinsic: the product in application order
            case EulerOrder::XYZ: q = qx * qy * qz; break;
            case EulerOrder::XZY: q = qx * qz * qy; break;
            case EulerOrder::YXZ: q = qy * qx * qz; break;
            case EulerOrder::YZX: q = qy * qz * qx; break;
            case EulerOrder::ZXY: q = qz * qx * qy; break;
            case EulerOrder::ZYX: q = qz * qy * qx; break;
        }
        return q.normalized();
    }
    /// @brief fromEuler() with angles in degrees (editor, scene files).
    static QuatT fromEulerDegrees(const glm::dvec3& degrees, EulerOrder order = kDefaultEulerOrder) {
        return fromEuler({Angle::fromDegrees(degrees.x), Angle::fromDegrees(degrees.y), Angle::fromDegrees(degrees.z)}, order);
    }
    /// @brief fromEuler() with angles in radians.
    static QuatT fromEulerRadians(const glm::dvec3& radians, EulerOrder order = kDefaultEulerOrder) {
        return fromEuler(EulerAngles(radians), order);
    }
    /// @}

    /// @name Euler angles
    /// The middle rotation lies in [−90°, 90°], the others in [−180°, 180°). At the singularity
    /// (middle angle ±90°, "gimbal lock") the last angle is reported as 0.
    /// @{
    /// @brief Euler angles that rebuild this rotation in `order`.
    EulerAngles toEuler(EulerOrder order = kDefaultEulerOrder) const {
        return EulerAngles(detail::eulerFromQuat(toDouble(), order));
    }
    /// @brief toEuler() in degrees (editor, scene files).
    glm::dvec3 toEulerDegrees(EulerOrder order = kDefaultEulerOrder) const {
        const EulerAngles e = toEuler(order);
        return {e.x.degrees(), e.y.degrees(), e.z.degrees()};
    }
    /// @}

    friend constexpr bool operator==(const QuatT& a, const QuatT& b) {
        return a.w == b.w && a.x == b.x && a.y == b.y && a.z == b.z;
    }
    /// @brief The same rotation with every component negated (q and −q turn the same way).
    friend constexpr QuatT operator-(const QuatT& q) { return fromComponents(-q.w, -q.x, -q.y, -q.z); }
    /// @brief The inverse rotation (for unit quaternions).
    constexpr QuatT conjugate() const { return fromComponents(w, -x, -y, -z); }
    /// @brief The inverse rotation (same as conjugate(): rotations are unit quaternions).
    constexpr QuatT inverse() const { return conjugate(); }
    /// @brief Composition: `a * b` applies `b` first, then `a` (as glm).
    friend constexpr QuatT operator*(const QuatT& a, const QuatT& b) {
        auto p = [](T u, T v) { return static_cast<i64>(u.raw_value()) * v.raw_value(); };
        auto r = [](i64 v) { return fromRaw<T>(detail::roundShift<kF>(v)); };
        return fromComponents(r(p(a.w, b.w) - p(a.x, b.x) - p(a.y, b.y) - p(a.z, b.z)),
                              r(p(a.w, b.x) + p(a.x, b.w) + p(a.y, b.z) - p(a.z, b.y)),
                              r(p(a.w, b.y) - p(a.x, b.z) + p(a.y, b.w) + p(a.z, b.x)),
                              r(p(a.w, b.z) + p(a.x, b.y) - p(a.y, b.x) + p(a.z, b.w)));
    }
    QuatT& operator*=(const QuatT& o) { return *this = *this * o; }

    /// @brief Exact squared length, in raw units² (2·F fraction bits).
    constexpr u64 lengthSquaredRaw() const {
        auto sq = [](T v) { const i64 r = v.raw_value(); return static_cast<u64>(r * r); };
        return sq(w) + sq(x) + sq(y) + sq(z); // components < 2: no overflow
    }
    /// @brief Back to unit length (exact integer square root). A zero quaternion becomes the identity.
    constexpr QuatT normalized() const {
        const u64 len = isqrt(lengthSquaredRaw());
        if (len == 0) return identity();
        auto n = [&](T v) { return fromRaw<T>(detail::roundDiv(static_cast<i64>(v.raw_value()) * (i64(1) << kF), static_cast<i64>(len))); };
        return fromComponents(n(w), n(x), n(y), n(z));
    }
    /// @brief Brings the quaternion back to unit length only if it drifted more than rounding does
    ///        (one exact squared length when it is already unit: the common case).
    constexpr QuatT renormalized() const {
        constexpr i64 one = i64(1) << (2 * kF);
        constexpr i64 tolerance = one >> (kF - 4); // relative 2^−(F−4): a few steps of every component
        const i64 drift = static_cast<i64>(lengthSquaredRaw()) - one;
        return (drift > -tolerance && drift < tolerance) ? *this : normalized();
    }

    /// @brief `v` rotated (any 32-bit vector format): v + 2w(q×v) + 2q×(q×v). Intermediates stay in
    ///        64 bits, so any vector shorter than the format's range rotates without overflow (one
    ///        rounding per term); the result has the same length, so it stays in range too.
    template <typename V>
    constexpr Vec3<V> rotate(const Vec3<V>& v) const {
        static_assert(storageBits<V> <= 32, "rotate: vectors of up to 32 bits");
        const i64 ux = x.raw_value(), uy = y.raw_value(), uz = z.raw_value(), uw = w.raw_value();
        const i64 vx = v.x.raw_value(), vy = v.y.raw_value(), vz = v.z.raw_value();
        // t = 2 (u × v), in the vector's units.
        const i64 tx = 2 * detail::roundShift<kF>(uy * vz - uz * vy);
        const i64 ty = 2 * detail::roundShift<kF>(uz * vx - ux * vz);
        const i64 tz = 2 * detail::roundShift<kF>(ux * vy - uy * vx);
        const i64 rx = vx + detail::roundShift<kF>(uw * tx) + detail::roundShift<kF>(uy * tz - uz * ty);
        const i64 ry = vy + detail::roundShift<kF>(uw * ty) + detail::roundShift<kF>(uz * tx - ux * tz);
        const i64 rz = vz + detail::roundShift<kF>(uw * tz) + detail::roundShift<kF>(ux * ty - uy * tx);
        return {fromRaw<V>(rx), fromRaw<V>(ry), fromRaw<V>(rz)};
    }
    /// @brief rotate() as an operator (as glm: `q * v`).
    template <typename V>
    friend constexpr Vec3<V> operator*(const QuatT& q, const Vec3<V>& v) { return q.rotate(v); }

    /// @name Local axes (the engine's convention: right = +X, up = +Y, forward = −Z)
    /// @{
    UnitVec right() const { return rotate(UnitVec(1.0, 0.0, 0.0)); }
    UnitVec up() const { return rotate(UnitVec(0.0, 1.0, 0.0)); }
    UnitVec forward() const { return rotate(UnitVec(0.0, 0.0, -1.0)); }
    /// @}
};

/// @brief Is `Q` a QuatT?
template <typename Q>
struct IsQuat : std::false_type {};
template <typename T>
struct IsQuat<QuatT<T>> : std::true_type {};

/// @brief 4D dot product of two quaternions (cosine of half the angle between them), in `Unit`.
template <typename Q, std::enable_if_t<IsQuat<Q>::value, int> = 0>
constexpr Unit dot(const Q& a, const Q& b) {
    const i64 p = static_cast<i64>(a.w.raw_value()) * b.w.raw_value() + static_cast<i64>(a.x.raw_value()) * b.x.raw_value() +
                  static_cast<i64>(a.y.raw_value()) * b.y.raw_value() + static_cast<i64>(a.z.raw_value()) * b.z.raw_value();
    constexpr int shift = 2 * Q::kF - int(fractionBits<Unit>);
    return fromRaw<Unit>(detail::roundShift<shift>(p));
}

/// @brief Normalized linear interpolation along the shortest arc: cheaper than slerp, same path,
///        but not constant speed (fine for small steps, e.g. smoothing every frame). `t` in [0, 1].
template <typename T>
constexpr QuatT<T> nlerp(const QuatT<T>& a, const QuatT<T>& b, Unit t) {
    const QuatT<T> to = dot(a, b) < 0 ? -b : b;
    auto l = [&](T p, T q) { return p + mul<T>(q - p, t); };
    const QuatT<T> r = QuatT<T>::fromComponents(l(a.w, to.w), l(a.x, to.x), l(a.y, to.y), l(a.z, to.z));
    return r.lengthSquaredRaw() == 0 ? a : r.normalized(); // may be as short as ~0.71: a real normalization
}

/// @brief Spherical interpolation along the shortest arc: constant angular speed. `t` in [0, 1].
///        The weights come from the standard library (see Trig.h), the blend is fixed point.
template <typename T>
QuatT<T> slerp(const QuatT<T>& a, const QuatT<T>& b, Unit t) {
    QuatT<T> to = b;
    f64 cosAngle = toDouble(dot(a, b));
    if (cosAngle < 0.0) { // q and −q are the same rotation: take the short way
        to = -b;
        cosAngle = -cosAngle;
    }
    if (cosAngle > 0.9995) return nlerp(a, to, t); // nearly equal: nlerp is exact enough
    const f64 angle = std::acos(cosAngle > 1.0 ? 1.0 : cosAngle);
    const f64 s = std::sin(angle), td = toDouble(t);
    const Unit wa = toFixed<Unit>(std::sin((1.0 - td) * angle) / s), wb = toFixed<Unit>(std::sin(td * angle) / s);
    auto m = [&](T p, T q) { return fromRaw<T>(detail::roundShift<int(fractionBits<Unit>)>(
                                 static_cast<i64>(p.raw_value()) * wa.raw_value() + static_cast<i64>(q.raw_value()) * wb.raw_value())); };
    return QuatT<T>::fromComponents(m(a.w, to.w), m(a.x, to.x), m(a.y, to.y), m(a.z, to.z)).renormalized();
}

/// @brief Angle between two rotations, in [0, π] (π reads as −π signed: compare raw() values).
template <typename T>
Angle angleBetween(const QuatT<T>& a, const QuatT<T>& b) {
    const Unit c = abs(dot(a, b));
    return acos(c > Unit(1) ? Unit(1) : c) * 2;
}

/// @brief Rotates `from` towards `to` by at most `maxStep` (taken as its size; reaches `to` exactly
///        when close).
template <typename T>
QuatT<T> rotateTowards(const QuatT<T>& from, const QuatT<T>& to, Angle maxStep) {
    const u32 angle = angleBetween(from, to).raw(); // [0, π]: the unsigned reading orders it
    const u32 step = maxStep.abs().raw();
    if (angle <= step || angle < 4) return to;
    const i64 t = (static_cast<i64>(step) << fractionBits<Unit>) / angle; // step / angle < 1
    return slerp(from, to, fromRaw<Unit>(t));
}

/// @brief A rotation with Q1.30 components: the engine's rotation type.
using Quat = QuatT<Unit>;
/// @brief A compact rotation (Q1.14 components): storage, the network.
using Quat16 = QuatT<Q<1, 14>>;

} // namespace nova
