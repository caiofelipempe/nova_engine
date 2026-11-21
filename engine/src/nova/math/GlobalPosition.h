#pragma once

/// @file
/// @brief Exact integer world positions (GlobalPosition) and the offsets between them (GlobalOffset).

#include "nova/math/WorldSpace.h"

#include <glm/glm.hpp>
#include <glm/ext/vector_int3_sized.hpp>
#include <glm/gtc/quaternion.hpp>

#include <limits>

namespace nova {

/// @brief A displacement between world positions, in integer steps of the world unit.
///
/// Integer operations are exact (`+`, `-`, `* integer`, comparisons of lengths); they saturate at
/// the int64 limits instead of overflowing. Conversions to/from meters use the active WorldSpace.
/// @code
/// nova::GlobalOffset step = nova::GlobalOffset::fromMeters(velocity * dt);
/// glm::vec3 local = (enemy - player).toVec3();   // small offsets are exact enough in float
/// @endcode
struct GlobalOffset {
    i64 x = 0; ///< X steps.
    i64 y = 0; ///< Y steps.
    i64 z = 0; ///< Z steps.

    constexpr GlobalOffset() = default;
    /// @brief From raw integer steps.
    constexpr GlobalOffset(i64 x_, i64 y_, i64 z_) : x(x_), y(y_), z(z_) {}
    /// @brief From raw integer steps in a glm vector.
    explicit constexpr GlobalOffset(const glm::i64vec3& steps) : x(steps.x), y(steps.y), z(steps.z) {}
    /// @brief Raw integer steps as a glm vector (for custom integer math with glm).
    constexpr glm::i64vec3 raw() const { return {x, y, z}; }

    /// @brief From meters (rounded to the nearest step, saturating).
    static GlobalOffset fromMeters(const glm::dvec3& meters);
    /// @brief From meters (rounded to the nearest step, saturating).
    static GlobalOffset fromMeters(const glm::vec3& meters) { return fromMeters(glm::dvec3(meters)); }
    /// @brief In meters, as double (53 significant bits: exact up to 2^53 steps per axis).
    glm::dvec3 toMeters() const;
    /// @brief In meters, as float — for local geometry, physics and the GPU (keep offsets small).
    glm::vec3 toVec3() const { return glm::vec3(toMeters()); }
    /// @brief From a value in any metric unit (`fromUnits({0, 0, 250}, LengthUnit::Millimeter)`).
    static GlobalOffset fromUnits(const glm::dvec3& value, LengthUnit u) { return fromMeters(value * metersPer(u)); }
    /// @brief Expressed in a metric unit, as double.
    glm::dvec3 toUnits(LengthUnit u) const;

    /// @brief Component by index (0 = x).
    i64& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    /// @brief Component by index (0 = x).
    i64 operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }

    /// @brief Exact squared length, in steps².
    UInt128 lengthSquared() const;
    /// @brief Length in meters (double precision).
    f64 length() const;
    /// @brief Length expressed in a metric unit.
    f64 length(LengthUnit u) const;
    /// @brief Exact: is the length below `meters`?
    bool isShorterThan(f64 meters) const;
    /// @brief This offset times `t`, rounded to the nearest step (near-exact even for huge offsets).
    GlobalOffset scaled(f64 t) const;

    friend bool operator==(const GlobalOffset&, const GlobalOffset&) = default;
    friend GlobalOffset operator+(const GlobalOffset& a, const GlobalOffset& b) {
        return {detail::saturatingAdd(a.x, b.x), detail::saturatingAdd(a.y, b.y), detail::saturatingAdd(a.z, b.z)};
    }
    friend GlobalOffset operator-(const GlobalOffset& a) {
        return {detail::saturatingNegate(a.x), detail::saturatingNegate(a.y), detail::saturatingNegate(a.z)};
    }
    friend GlobalOffset operator-(const GlobalOffset& a, const GlobalOffset& b) { return a + (-b); }
    /// @brief Exact integer multiple (saturating).
    friend GlobalOffset operator*(const GlobalOffset& a, i64 n);
    friend GlobalOffset operator*(i64 n, const GlobalOffset& a) { return a * n; }
    GlobalOffset& operator+=(const GlobalOffset& o) { return *this = *this + o; }
    GlobalOffset& operator-=(const GlobalOffset& o) { return *this = *this - o; }
    GlobalOffset& operator*=(i64 n) { return *this = *this * n; }
};

/// @brief Exact comparison of lengths: negative if `a` is shorter, 0 if equal, positive if longer.
int compareLength(const GlobalOffset& a, const GlobalOffset& b);

/// @brief A point in the world: three int64 in fixed point (WorldSpace::unit meters per step).
///
/// Always inside the active WorldSpace: barrier axes saturate at their edges, wrap axes loop.
/// Relations between points are integer and exact — `b - a` is a GlobalOffset and
/// `a + (b - a) == b` bit for bit, across wrap seams too. Meters appear only at the edges:
/// fromMeters()/toMeters() (double) to initialize and display, float vectors for movement.
/// @code
/// auto p = nova::GlobalPosition::fromMeters({1'250'000.0, 12.5, -30.0});
/// p += velocity * dt;                        // float meters → exact integer step
/// float d = float(nova::distance(p, target)); // meters
/// @endcode
class GlobalPosition {
public:
    /// @brief The world origin.
    constexpr GlobalPosition() = default;
    /// @brief From meters (implicit, so `transform.position = {0, 3, 10}` reads naturally).
    GlobalPosition(f64 xMeters, f64 yMeters, f64 zMeters) : GlobalPosition(fromMeters({xMeters, yMeters, zMeters})) {}
    /// @brief From meters (implicit).
    GlobalPosition(const glm::dvec3& meters) : GlobalPosition(fromMeters(meters)) {}
    /// @brief From meters (implicit). Far from the origin a float cannot say where you are: prefer
    ///        moving existing positions by offsets (`position += velocity * dt`).
    GlobalPosition(const glm::vec3& meters) : GlobalPosition(fromMeters(glm::dvec3(meters))) {}

    /// @brief From raw steps, brought into the world (wrap: modulo; barrier: clamped).
    static GlobalPosition fromRaw(i64 x, i64 y, i64 z);
    /// @brief From raw steps in a glm vector.
    static GlobalPosition fromRaw(const glm::i64vec3& steps) { return fromRaw(steps.x, steps.y, steps.z); }
    /// @brief From meters (rounded to the nearest step; wrap axes loop, barrier axes clamp).
    static GlobalPosition fromMeters(const glm::dvec3& meters);
    /// @brief In meters, as double (for display/logs; exact up to 2^53 steps per axis).
    glm::dvec3 toMeters() const;
    /// @brief In meters from the world origin, as float. Loses precision far from the origin: for
    ///        rendering and physics prefer offsets from a nearby point (`(p - camera).toVec3()`).
    glm::vec3 toVec3() const { return glm::vec3(toMeters()); }
    /// @brief From a value in any metric unit (`fromUnits({1500, 0, 20}, LengthUnit::Kilometer)`).
    static GlobalPosition fromUnits(const glm::dvec3& value, LengthUnit u) { return fromMeters(value * metersPer(u)); }
    /// @brief Expressed in a metric unit, as double (`p.toUnits(LengthUnit::Kilometer)`).
    glm::dvec3 toUnits(LengthUnit u) const;

    /// @brief Raw steps on axis `i` (0 = x).
    i64 raw(int i) const { return m_v[i]; }
    /// @brief Raw steps as a glm vector. Plain glm arithmetic on them ignores the world's
    ///        boundaries: use GlobalOffset / the operators here for positions.
    glm::i64vec3 raw() const { return {m_v[0], m_v[1], m_v[2]}; }

    /// @brief Moves by `offset` (exact). @return Bit mask of the axes stopped by a barrier (bit i = axis i).
    u8 move(const GlobalOffset& offset);
    /// @brief Moves by `meters` (rounded to the nearest step). @return Axes stopped by a barrier.
    u8 move(const glm::vec3& meters) { return move(GlobalOffset::fromMeters(meters)); }

    friend bool operator==(const GlobalPosition&, const GlobalPosition&) = default;
    /// @brief Exact offset from `b` to `a` (wrap axes: the shortest way around).
    friend GlobalOffset operator-(const GlobalPosition& a, const GlobalPosition& b);
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
    i64 m_v[3] = {0, 0, 0};
};

/// @brief Distance in meters (double precision).
f64 distance(const GlobalPosition& a, const GlobalPosition& b);
/// @brief Distance expressed in a metric unit.
f64 distance(const GlobalPosition& a, const GlobalPosition& b, LengthUnit u);
/// @brief Exact squared distance in steps².
UInt128 distanceSquared(const GlobalPosition& a, const GlobalPosition& b);
/// @brief Exact: are `a` and `b` closer than `meters`?
bool isWithin(const GlobalPosition& a, const GlobalPosition& b, f64 meters);
/// @brief Point at `t` from `a` (0) to `b` (1), along the shortest way (wrap seams included).
GlobalPosition lerp(const GlobalPosition& a, const GlobalPosition& b, f64 t);
/// @brief Moves `from` towards `to` by at most `maxMeters` (reaches `to` exactly when close enough).
GlobalPosition moveTowards(const GlobalPosition& from, const GlobalPosition& to, f64 maxMeters);
/// @brief Rotates `point` around `pivot`.
GlobalPosition rotateAround(const GlobalPosition& point, const GlobalPosition& pivot, const glm::quat& rotation);

} // namespace nova
