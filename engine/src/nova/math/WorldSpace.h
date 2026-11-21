#pragma once

/// @file
/// @brief World configuration for integer global positions: unit (scale), per-axis size and
///        boundary (barrier or wrap-around), plus a small unsigned 128-bit integer.

#include "nova/core/Base.h"
#include "nova/math/Units.h"

#include <array>

namespace nova {

/// @brief Unsigned 128-bit integer (exact squared lengths of integer offsets).
struct UInt128 {
    u64 hi = 0; ///< High 64 bits.
    u64 lo = 0; ///< Low 64 bits.

    /// @brief Full product of two 64-bit values.
    static UInt128 mul(u64 a, u64 b);
    /// @brief Sum (wraps at 2^128, which squared lengths of three int64 never reach).
    friend UInt128 operator+(UInt128 a, UInt128 b) {
        UInt128 r{a.hi + b.hi, a.lo + b.lo};
        if (r.lo < a.lo) ++r.hi;
        return r;
    }
    friend bool operator==(const UInt128&, const UInt128&) = default;
    friend bool operator<(const UInt128& a, const UInt128& b) { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }
    friend bool operator>(const UInt128& a, const UInt128& b) { return b < a; }
    friend bool operator<=(const UInt128& a, const UInt128& b) { return !(b < a); }
    friend bool operator>=(const UInt128& a, const UInt128& b) { return !(a < b); }
    /// @brief Nearest double (53 significant bits).
    f64 toDouble() const;
};

/// @brief What happens at the edges of a world axis.
enum class WorldBoundary : u8 {
    Barrier, ///< Positions stop at the edge (saturate).
    Wrap,    ///< The axis loops: leaving one edge enters the other, distances cross the seam.
};

/// @brief One axis of a WorldSpaceDesc.
struct WorldAxisDesc {
    f64 size = 0.0;                                ///< Extent in meters (0 = the largest the unit allows).
    WorldBoundary boundary = WorldBoundary::Barrier; ///< Edge behavior.
};

/// @brief Settings of a world (fixed for its lifetime: positions are stored in its units).
struct WorldSpaceDesc {
    /// @brief Meters per integer step. Default 2^-32 m (32.32 fixed point, ~0.23 nm).
    f64 unit = 1.0 / 4294967296.0;
    /// @brief X, Y, Z.
    std::array<WorldAxisDesc, 3> axes{};

    /// @brief Sets the step size in any metric unit: `setUnit(1, LengthUnit::Millimeter)`.
    WorldSpaceDesc& setUnit(f64 value, LengthUnit u) {
        unit = toMeters(value, u);
        return *this;
    }
    /// @brief Sets one axis: `setAxis(0, 50, LengthUnit::Kilometer, WorldBoundary::Wrap)`.
    WorldSpaceDesc& setAxis(int axis, f64 size, LengthUnit u, WorldBoundary boundary) {
        axes[axis] = {toMeters(size, u), boundary};
        return *this;
    }
};

/// @brief Resolved world configuration used by GlobalPosition / GlobalOffset arithmetic.
///
/// Each axis covers `span` integer steps centered on 0, `[min, max]`:
/// - **Barrier** axes hold at most 2^63 steps, so the difference of any two positions fits in an
///   `int64`; moving past an edge saturates at it.
/// - **Wrap** axes hold up to 2^64 steps; arithmetic is modulo the span (with the full 2^64 span it
///   is the integer's own overflow) and differences take the shortest way around.
///
/// Arithmetic uses the *active* world space (setActive()); a scene sets its own when it becomes
/// current. Changing it invalidates positions stored under the previous one.
class WorldSpace {
public:
    /// @brief Integer layout of one axis.
    struct Axis {
        WorldBoundary boundary = WorldBoundary::Barrier; ///< Edge behavior.
        bool full = false; ///< Wrap over all 2^64 values (span would be 2^64, not representable).
        u64 span = 0;      ///< Number of valid steps (unused when `full`).
        i64 min = 0;       ///< Lowest valid value.
        i64 max = 0;       ///< Highest valid value.
    };

    static constexpr u64 kMaxBarrierSpan = 1ull << 63; ///< Largest span of a barrier axis.

    /// @brief Default world: 32.32 units, barrier on every axis at the largest size (~±2.1 million km).
    WorldSpace() : WorldSpace(WorldSpaceDesc{}) {}
    /// @brief Resolves `desc`; out-of-range sizes are clamped (and logged).
    explicit WorldSpace(const WorldSpaceDesc& desc);

    /// @brief Settings as given (sizes after clamping).
    const WorldSpaceDesc& desc() const { return m_desc; }
    /// @brief Meters per integer step.
    f64 unit() const { return m_desc.unit; }
    /// @brief Integer layout of axis `i` (0 = X, 1 = Y, 2 = Z).
    const Axis& axis(int i) const { return m_axes[i]; }
    /// @brief Extent of axis `i` in meters.
    f64 sizeMeters(int i) const;
    /// @brief Step size expressed in `u` (e.g. nanometers).
    f64 unitIn(LengthUnit u) const { return fromMeters(m_desc.unit, u); }
    /// @brief Extent of axis `i` expressed in `u` (e.g. kilometers).
    f64 sizeIn(int i, LengthUnit u) const { return fromMeters(sizeMeters(i), u); }

    /// @name Per-axis arithmetic (used by GlobalPosition)
    /// @{
    /// @brief Brings any integer into the axis range (wrap: modulo; barrier: clamp).
    i64 normalize(int i, i64 value) const;
    /// @brief `position + offset` on axis `i`. Sets `hitBarrier` when a barrier stopped the move.
    i64 add(int i, i64 position, i64 offset, bool& hitBarrier) const;
    /// @brief `a - b` on axis `i` (wrap: shortest way around). Exact; always fits.
    i64 difference(int i, i64 a, i64 b) const;
    /// @}

    /// @brief The world space positions are currently interpreted in.
    static const WorldSpace& active();
    /// @brief Replaces the active world space.
    static void setActive(const WorldSpace& space);

private:
    WorldSpaceDesc m_desc;
    std::array<Axis, 3> m_axes{};
};

} // namespace nova
