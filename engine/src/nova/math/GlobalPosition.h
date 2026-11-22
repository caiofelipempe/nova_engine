#pragma once

/// @file
/// @brief World positions (GlobalPosition) and the offsets between them (GlobalOffset): Q13.18
///        fixed point on the 16 km torus (see WorldSpace).

#include "nova/math/Fixed.h"
#include "nova/math/WorldSpace.h"

#include <glm/glm.hpp>
#include <glm/ext/vector_int3_sized.hpp>
#include <glm/gtc/quaternion.hpp>

namespace nova {

namespace detail {
// Two's complement arithmetic on the torus: unsigned, so the overflow is the lap (and defined).
constexpr i32 wrapAdd(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
constexpr i32 wrapSub(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
constexpr i32 wrapNeg(i32 a) { return static_cast<i32>(0u - static_cast<u32>(a)); }
constexpr i32 wrapMul(i32 a, i32 n) { return static_cast<i32>(static_cast<u32>(a) * static_cast<u32>(n)); }
} // namespace detail

/// @brief A displacement between world positions, in steps of 2⁻¹⁸ m (a Length vector).
///
/// Offsets live on the torus too: adding them is exact and loops around the lap (an offset of
/// one whole lap is zero), and the offset between two positions is always the shortest way.
/// @code
/// nova::GlobalOffset step = nova::GlobalOffset::fromMeters(velocity * dt);
/// glm::vec3 local = (enemy - player).toVec3();
/// @endcode
struct GlobalOffset : glm::i32vec3 {
    constexpr GlobalOffset() : glm::i32vec3(0) {}
    /// @brief From raw steps.
    constexpr GlobalOffset(i32 x_, i32 y_, i32 z_) : glm::i32vec3(x_, y_, z_) {}
    /// @brief From raw steps in a glm vector.
    explicit constexpr GlobalOffset(const glm::i32vec3& steps) : glm::i32vec3(steps) {}
    /// @brief Raw steps as a glm vector.
    constexpr glm::i32vec3 raw() const { return *this; }

    /// @brief From meters (rounded to the nearest step; whole laps drop out).
    static GlobalOffset fromMeters(const glm::dvec3& meters);
    /// @brief From meters (rounded to the nearest step; whole laps drop out).
    static GlobalOffset fromMeters(const glm::vec3& meters) { return fromMeters(glm::dvec3(meters)); }
    /// @brief In meters, exact (a step count times 2⁻¹⁸ fits a double).
    glm::dvec3 toMeters() const;
    /// @brief In meters, as float (local geometry, physics, the GPU).
    glm::vec3 toVec3() const { return glm::vec3(toMeters()); }
    /// @brief From a value in any metric unit.
    static GlobalOffset fromUnits(const glm::dvec3& value, LengthUnit u) { return fromMeters(value * metersPer(u)); }
    /// @brief Expressed in a metric unit.
    glm::dvec3 toUnits(LengthUnit u) const;
    /// @brief The same offset as a fixed-point Length vector (same bits).
    constexpr LengthVec toFixed() const { return {fromRaw<Length>(x), fromRaw<Length>(y), fromRaw<Length>(z)}; }
    /// @brief From a fixed-point Length vector (same bits).
    static constexpr GlobalOffset fromFixed(const LengthVec& v) {
        return {v.x.raw_value(), v.y.raw_value(), v.z.raw_value()};
    }

    /// @brief Exact squared length in steps² (fits: 3 · 2⁶² < 2⁶⁴).
    constexpr u64 lengthSquared() const {
        auto sq = [](i32 v) { return static_cast<u64>(static_cast<i64>(v) * v); };
        return sq(x) + sq(y) + sq(z);
    }
    /// @brief Length in meters.
    f64 length() const;
    /// @brief Length expressed in a metric unit.
    f64 length(LengthUnit u) const;
    /// @brief Exact: is the length below `meters`?
    bool isShorterThan(f64 meters) const;
    /// @brief This offset times `factor`, rounded to the nearest step (loops around the lap).
    GlobalOffset scaled(f64 factor) const;

    friend constexpr bool operator==(const GlobalOffset&, const GlobalOffset&) = default;
    friend constexpr GlobalOffset operator+(const GlobalOffset& a, const GlobalOffset& b) {
        return {detail::wrapAdd(a.x, b.x), detail::wrapAdd(a.y, b.y), detail::wrapAdd(a.z, b.z)};
    }
    friend constexpr GlobalOffset operator-(const GlobalOffset& a) {
        return {detail::wrapNeg(a.x), detail::wrapNeg(a.y), detail::wrapNeg(a.z)};
    }
    friend constexpr GlobalOffset operator-(const GlobalOffset& a, const GlobalOffset& b) {
        return {detail::wrapSub(a.x, b.x), detail::wrapSub(a.y, b.y), detail::wrapSub(a.z, b.z)};
    }
    /// @brief Integer multiple (loops around the lap).
    friend constexpr GlobalOffset operator*(const GlobalOffset& a, i32 n) {
        return {detail::wrapMul(a.x, n), detail::wrapMul(a.y, n), detail::wrapMul(a.z, n)};
    }
    friend constexpr GlobalOffset operator*(i32 n, const GlobalOffset& a) { return a * n; }
    GlobalOffset& operator+=(const GlobalOffset& o) { return *this = *this + o; }
    GlobalOffset& operator-=(const GlobalOffset& o) { return *this = *this - o; }
    GlobalOffset& operator*=(i32 n) { return *this = *this * n; }
};

/// @brief Exact comparison of lengths: negative if `a` is shorter, 0 if equal, positive if longer.
int compareLength(const GlobalOffset& a, const GlobalOffset& b);

/// @brief A point of the world: three Q13.18 coordinates on the 16 km torus (see WorldSpace).
///
/// Every value is a valid position: moving past one side enters the other. Relations between
/// points are exact: `b - a` is the shortest GlobalOffset and `a + (b - a) == b` bit for bit,
/// across the seam too.
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

    /// @brief From raw steps.
    static constexpr GlobalPosition fromRaw(i32 x, i32 y, i32 z) {
        GlobalPosition p;
        p.m_steps = {x, y, z};
        return p;
    }
    /// @brief From raw steps in a glm vector.
    static constexpr GlobalPosition fromRaw(const glm::i32vec3& steps) { return fromRaw(steps.x, steps.y, steps.z); }
    /// @brief From meters (rounded to the nearest step; any value, it loops around the torus).
    static GlobalPosition fromMeters(const glm::dvec3& meters);
    /// @brief In meters, in [−8 192, 8 192), exact.
    glm::dvec3 toMeters() const;
    /// @brief In meters, as float (precise to ≤ 0.5 mm anywhere in the world).
    glm::vec3 toVec3() const { return glm::vec3(toMeters()); }
    /// @brief From a value in any metric unit.
    static GlobalPosition fromUnits(const glm::dvec3& value, LengthUnit u) { return fromMeters(value * metersPer(u)); }
    /// @brief Expressed in a metric unit.
    glm::dvec3 toUnits(LengthUnit u) const;
    /// @brief The coordinates as fixed-point Lengths from the origin (same bits).
    constexpr LengthVec toFixed() const { return GlobalOffset(m_steps).toFixed(); }

    /// @brief Raw steps on axis `i` (0 = x).
    constexpr i32 raw(int i) const { return m_steps[i]; }
    /// @brief Raw steps as a glm vector.
    constexpr const glm::i32vec3& raw() const { return m_steps; }

    /// @brief Moves by `offset` (exact, around the torus).
    constexpr void move(const GlobalOffset& offset) {
        for (int i = 0; i < 3; ++i) m_steps[i] = detail::wrapAdd(m_steps[i], offset[i]);
    }
    /// @brief Moves by `meters` (rounded to the nearest step).
    void move(const glm::vec3& meters) { move(GlobalOffset::fromMeters(meters)); }

    friend constexpr bool operator==(const GlobalPosition&, const GlobalPosition&) = default;
    /// @brief Exact offset from `b` to `a`: the shortest way around the torus.
    friend constexpr GlobalOffset operator-(const GlobalPosition& a, const GlobalPosition& b) {
        return GlobalOffset(a.m_steps) - GlobalOffset(b.m_steps);
    }
    friend constexpr GlobalPosition operator+(GlobalPosition p, const GlobalOffset& o) {
        p.move(o);
        return p;
    }
    friend constexpr GlobalPosition operator-(GlobalPosition p, const GlobalOffset& o) {
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
    glm::i32vec3 m_steps{0}; // Q13.18 steps (2⁻¹⁸ m)
};

/// @brief Distance in meters (the shortest way around).
f64 distance(const GlobalPosition& a, const GlobalPosition& b);
/// @brief Distance expressed in a metric unit.
f64 distance(const GlobalPosition& a, const GlobalPosition& b, LengthUnit u);
/// @brief Exact squared distance in steps².
constexpr u64 distanceSquared(const GlobalPosition& a, const GlobalPosition& b) { return (b - a).lengthSquared(); }
/// @brief Exact: are `a` and `b` closer than `meters`?
bool isWithin(const GlobalPosition& a, const GlobalPosition& b, f64 meters);
/// @brief Point at `t` from `a` (0) to `b` (1), along the shortest way (across the seam too).
GlobalPosition lerp(const GlobalPosition& a, const GlobalPosition& b, f64 t);
/// @brief Moves `from` towards `to` by at most `maxMeters` (reaches `to` exactly when close enough).
GlobalPosition moveTowards(const GlobalPosition& from, const GlobalPosition& to, f64 maxMeters);
/// @brief Rotates `point` around `pivot`.
GlobalPosition rotateAround(const GlobalPosition& point, const GlobalPosition& pivot, const glm::quat& rotation);

} // namespace nova
