#pragma once

/// @file
/// @brief World positions (GlobalPosition) and the offsets between them (GlobalOffset): Q45.18
///        fixed point on the circular world (see WorldSpace, WorldSize).

#include "nova/math/Quat.h"
#include "nova/math/WorldSpace.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace nova {

namespace detail {
// Two's complement arithmetic in unsigned (defined overflow); the world's lap comes after.
constexpr i64 rawAdd(i64 a, i64 b) { return static_cast<i64>(static_cast<u64>(a) + static_cast<u64>(b)); }
constexpr i64 rawSub(i64 a, i64 b) { return static_cast<i64>(static_cast<u64>(a) - static_cast<u64>(b)); }
// Brought into the active world (every axis around its own size).
inline glm::i64vec3 wrapped(const glm::i64vec3& v) {
    const WorldSize& w = WorldSize::active();
    return {w.wrap(v.x, 0), w.wrap(v.y, 1), w.wrap(v.z, 2)};
}
} // namespace detail

/// @brief A displacement between world positions, in steps of 2⁻¹⁸ m.
///
/// Offsets live on the circular world too (the active WorldSize): adding them is exact and loops
/// around the world (an offset of one whole lap is zero), and the offset between two positions is
/// always the shortest way. After any operation each axis is within [−size/2, size/2).
/// @code
/// nova::GlobalOffset step = nova::GlobalOffset::fromMeters(velocity * dt);
/// glm::vec3 local = (enemy - player).toVec3();
/// @endcode
struct GlobalOffset : glm::i64vec3 {
    constexpr GlobalOffset() : glm::i64vec3(0) {}
    /// @brief From raw steps (taken as they are).
    constexpr GlobalOffset(i64 x_, i64 y_, i64 z_) : glm::i64vec3(x_, y_, z_) {}
    /// @brief From raw steps in a glm vector (taken as they are).
    explicit constexpr GlobalOffset(const glm::i64vec3& steps) : glm::i64vec3(steps) {}
    /// @brief Raw steps as a glm vector.
    constexpr glm::i64vec3 raw() const { return *this; }

    /// @brief From meters (rounded to the nearest step; whole laps drop out).
    static GlobalOffset fromMeters(const glm::dvec3& meters);
    /// @brief From meters (rounded to the nearest step; whole laps drop out).
    static GlobalOffset fromMeters(const glm::vec3& meters) { return fromMeters(glm::dvec3(meters)); }
    /// @brief In meters (exact below 2⁵³ steps per axis).
    glm::dvec3 toMeters() const;
    /// @brief In meters, as float (local geometry, physics, the GPU).
    glm::vec3 toVec3() const { return glm::vec3(toMeters()); }
    /// @brief From a value in any metric unit.
    static GlobalOffset fromUnits(const glm::dvec3& value, LengthUnit u) { return fromMeters(value * metersPer(u)); }
    /// @brief Expressed in a metric unit.
    glm::dvec3 toUnits(LengthUnit u) const;
    /// @brief The same offset as a fixed-point Length vector (same bits; only for offsets within
    ///        ±8 192 m, the range of Length).
    constexpr LengthVec toFixed() const {
        return {fromRaw<Length>(static_cast<i32>(x)), fromRaw<Length>(static_cast<i32>(y)),
                fromRaw<Length>(static_cast<i32>(z))};
    }
    /// @brief From a fixed-point Length vector (same bits).
    static constexpr GlobalOffset fromFixed(const LengthVec& v) {
        return {v.x.raw_value(), v.y.raw_value(), v.z.raw_value()};
    }
    /// @brief The same offset as a 64-bit GlobalLength vector (same bits, any length).
    constexpr GlobalLengthVec toGlobalLength() const {
        return {fromRaw<GlobalLength>(x), fromRaw<GlobalLength>(y), fromRaw<GlobalLength>(z)};
    }
    /// @brief From a 64-bit GlobalLength vector (same bits, brought into the active world).
    static GlobalOffset fromGlobalLength(const GlobalLengthVec& v) {
        return GlobalOffset(detail::wrapped({v.x.raw_value(), v.y.raw_value(), v.z.raw_value()}));
    }

    /// @brief Largest axis (in steps) for which lengthSquared() is exact: 2³¹ (8 192 m).
    static constexpr i64 kExactAxis = i64(1) << 31;
    /// @brief True when every axis is within ±kExactAxis (lengthSquared() is exact).
    constexpr bool isShort() const {
        auto ok = [](i64 v) { return v >= -kExactAxis && v <= kExactAxis; };
        return ok(x) && ok(y) && ok(z);
    }
    /// @brief Squared length in steps²: exact when isShort() (3 · 2⁶² < 2⁶⁴), the largest u64 otherwise.
    constexpr u64 lengthSquared() const {
        if (!isShort()) return ~u64(0);
        auto sq = [](i64 v) { return static_cast<u64>(v * v); };
        return sq(x) + sq(y) + sq(z);
    }
    /// @brief Length in meters.
    f64 length() const;
    /// @brief Length expressed in a metric unit.
    f64 length(LengthUnit u) const;
    /// @brief Is the length below `meters`? Exact for short offsets (isShort()).
    bool isShorterThan(f64 meters) const;
    /// @brief This offset times `factor`, rounded to the nearest step (loops around the world).
    GlobalOffset scaled(f64 factor) const;
    /// @brief This offset times a fixed-point `factor` (Scalar, Unit...): exact integer math (one
    ///        rounding) when the products fit 64 bits, as scaled(f64) otherwise.
    template <typename S, std::enable_if_t<isFixed<S>, int> = 0>
    GlobalOffset scaled(S factor) const {
        const i64 f = static_cast<i64>(factor.raw_value());
        auto mag = [](i64 v) { return v < 0 ? 0 - static_cast<u64>(v) : static_cast<u64>(v); };
        const u64 af = mag(f);
        const u64 limit = af == 0 ? ~u64(0) : (u64(1) << 62) / af;
        if (mag(x) >= limit || mag(y) >= limit || mag(z) >= limit) return scaled(toDouble(factor));
        constexpr int F = int(fractionBits<S>);
        return GlobalOffset(detail::wrapped({detail::roundShift<F>(x * f), detail::roundShift<F>(y * f), detail::roundShift<F>(z * f)}));
    }
    /// @brief Length as a fixed-point Distance (exact integer square root, rounded down; saturates
    ///        past the range of Distance).
    Distance lengthFixed() const;
    /// @brief Is the length below `d`? Exact for short offsets (isShort()).
    bool isShorterThan(Distance d) const;

    friend constexpr bool operator==(const GlobalOffset&, const GlobalOffset&) = default;
    friend GlobalOffset operator+(const GlobalOffset& a, const GlobalOffset& b) {
        return GlobalOffset(
            detail::wrapped({detail::rawAdd(a.x, b.x), detail::rawAdd(a.y, b.y), detail::rawAdd(a.z, b.z)}));
    }
    friend GlobalOffset operator-(const GlobalOffset& a) {
        return GlobalOffset(detail::wrapped({detail::rawSub(0, a.x), detail::rawSub(0, a.y), detail::rawSub(0, a.z)}));
    }
    friend GlobalOffset operator-(const GlobalOffset& a, const GlobalOffset& b) {
        return GlobalOffset(
            detail::wrapped({detail::rawSub(a.x, b.x), detail::rawSub(a.y, b.y), detail::rawSub(a.z, b.z)}));
    }
    /// @brief Integer multiple (exact, loops around the world).
    friend GlobalOffset operator*(const GlobalOffset& a, i64 n);
    friend GlobalOffset operator*(i64 n, const GlobalOffset& a) { return a * n; }
    GlobalOffset& operator+=(const GlobalOffset& o) { return *this = *this + o; }
    GlobalOffset& operator-=(const GlobalOffset& o) { return *this = *this - o; }
    GlobalOffset& operator*=(i64 n) { return *this = *this * n; }
};

/// @brief Integer multiple (exact, loops around the world).
GlobalOffset operator*(const GlobalOffset& a, i64 n);

/// @brief Comparison of lengths: negative if `a` is shorter, 0 if equal, positive if longer.
///        Exact for short offsets (GlobalOffset::isShort()).
int compareLength(const GlobalOffset& a, const GlobalOffset& b);

/// @brief A point of the world: three Q45.18 coordinates on the circular world (WorldSize).
///
/// Every value is a valid position: moving past one side enters the other, and coordinates are
/// always within [−size/2, size/2) of the active world. Relations between points are exact:
/// `b - a` is the shortest GlobalOffset and `a + (b - a) == b` bit for bit, across the seam too.
/// @code
/// nova::GlobalPosition p{1250.0, 12.5, -30.0};
/// p += velocity * dt;                         // float meters → exact steps
/// double d = nova::distance(p, target);       // meters
/// @endcode
class GlobalPosition {
public:
    /// @brief The world origin.
    constexpr GlobalPosition() = default;
    /// @brief From meters (implicit, so `transform.position = {0, 3, 10}` reads naturally).
    GlobalPosition(f64 xMeters, f64 yMeters, f64 zMeters) : GlobalPosition(fromMeters({xMeters, yMeters, zMeters})) {}
    /// @brief From meters (implicit).
    GlobalPosition(const glm::dvec3& meters) : GlobalPosition(fromMeters(meters)) {}
    /// @brief From meters (implicit).
    GlobalPosition(const glm::vec3& meters) : GlobalPosition(fromMeters(glm::dvec3(meters))) {}

    /// @brief From raw steps (brought into the active world).
    static GlobalPosition fromRaw(i64 x, i64 y, i64 z) { return fromRaw(glm::i64vec3(x, y, z)); }
    /// @brief From raw steps in a glm vector (brought into the active world).
    static GlobalPosition fromRaw(const glm::i64vec3& steps) {
        GlobalPosition p;
        p.m_steps = detail::wrapped(steps);
        return p;
    }
    /// @brief From meters (rounded to the nearest step; any value, it loops around the world).
    static GlobalPosition fromMeters(const glm::dvec3& meters);
    /// @brief In meters, in [−size/2, size/2).
    glm::dvec3 toMeters() const;
    /// @brief In meters, as float.
    glm::vec3 toVec3() const { return glm::vec3(toMeters()); }
    /// @brief From a value in any metric unit.
    static GlobalPosition fromUnits(const glm::dvec3& value, LengthUnit u) { return fromMeters(value * metersPer(u)); }
    /// @brief Expressed in a metric unit.
    glm::dvec3 toUnits(LengthUnit u) const;
    /// @brief The coordinates as fixed-point Lengths from the origin (same bits; only within ±8 192 m).
    constexpr LengthVec toFixed() const { return GlobalOffset(m_steps).toFixed(); }
    /// @brief The coordinates as 64-bit GlobalLengths (same bits).
    constexpr GlobalLengthVec coords() const { return GlobalOffset(m_steps).toGlobalLength(); }
    /// @brief From 64-bit GlobalLength coordinates (brought into the active world).
    static GlobalPosition fromCoords(const GlobalLengthVec& c) {
        return fromRaw(c.x.raw_value(), c.y.raw_value(), c.z.raw_value());
    }

    /// @brief Raw steps on axis `i` (0 = x).
    constexpr i64 raw(int i) const { return m_steps[i]; }
    /// @brief Raw steps as a glm vector.
    constexpr const glm::i64vec3& raw() const { return m_steps; }
    /// @brief The same point brought into the active world (after the world's size changed).
    GlobalPosition rewrapped() const { return fromRaw(m_steps); }

    /// @brief Moves by `offset` (exact, around the world).
    void move(const GlobalOffset& offset) {
        m_steps = detail::wrapped({detail::rawAdd(m_steps.x, offset.x), detail::rawAdd(m_steps.y, offset.y),
                                   detail::rawAdd(m_steps.z, offset.z)});
    }
    /// @brief Moves by `meters` (rounded to the nearest step).
    void move(const glm::vec3& meters) { move(GlobalOffset::fromMeters(meters)); }

    friend constexpr bool operator==(const GlobalPosition&, const GlobalPosition&) = default;
    /// @brief Exact offset from `b` to `a`: the shortest way around the world.
    friend GlobalOffset operator-(const GlobalPosition& a, const GlobalPosition& b) {
        return GlobalOffset(a.m_steps) - GlobalOffset(b.m_steps);
    }
    friend GlobalPosition operator+(GlobalPosition p, const GlobalOffset& o) {
        p.move(o);
        return p;
    }
    friend GlobalPosition operator-(GlobalPosition p, const GlobalOffset& o) {
        p.move(-o);
        return p;
    }
    friend GlobalPosition operator+(GlobalPosition p, const glm::vec3& meters) {
        p.move(meters);
        return p;
    }
    friend GlobalPosition operator-(GlobalPosition p, const glm::vec3& meters) {
        p.move(-meters);
        return p;
    }
    GlobalPosition& operator+=(const GlobalOffset& o) {
        move(o);
        return *this;
    }
    GlobalPosition& operator-=(const GlobalOffset& o) {
        move(-o);
        return *this;
    }
    GlobalPosition& operator+=(const glm::vec3& meters) {
        move(meters);
        return *this;
    }
    GlobalPosition& operator-=(const glm::vec3& meters) {
        move(-meters);
        return *this;
    }

private:
    glm::i64vec3 m_steps{0}; // Q45.18 steps (2⁻¹⁸ m)
};

/// @brief Distance in meters (the shortest way around).
f64 distance(const GlobalPosition& a, const GlobalPosition& b);
/// @brief Distance expressed in a metric unit.
f64 distance(const GlobalPosition& a, const GlobalPosition& b, LengthUnit u);
/// @brief Squared distance in steps² (exact within 8 192 m per axis, see GlobalOffset::lengthSquared).
inline u64 distanceSquared(const GlobalPosition& a, const GlobalPosition& b) { return (b - a).lengthSquared(); }
/// @brief Are `a` and `b` closer than `meters`? (exact within 8 192 m per axis)
bool isWithin(const GlobalPosition& a, const GlobalPosition& b, f64 meters);
/// @brief Are `a` and `b` closer than `d`? (exact within 8 192 m per axis)
bool isWithin(const GlobalPosition& a, const GlobalPosition& b, Distance d);
/// @brief Point at `t` from `a` (0) to `b` (1), along the shortest way (across the seam too).
GlobalPosition lerp(const GlobalPosition& a, const GlobalPosition& b, f64 t);
/// @brief Same, with a fixed-point `t` (exact integer math for offsets within ±8 192 m).
GlobalPosition lerp(const GlobalPosition& a, const GlobalPosition& b, Unit t);
/// @brief Moves `from` towards `to` by at most `maxMeters` (reaches `to` exactly when close enough).
GlobalPosition moveTowards(const GlobalPosition& from, const GlobalPosition& to, f64 maxMeters);
/// @brief Same, with a fixed-point distance (exact integer math for offsets within ±8 192 m).
GlobalPosition moveTowards(const GlobalPosition& from, const GlobalPosition& to, Distance maxDistance);
/// @brief Rotates `point` around `pivot` (fixed point for points within 8 192 m of the pivot).
GlobalPosition rotateAround(const GlobalPosition& point, const GlobalPosition& pivot, const Quat& rotation);

} // namespace nova
