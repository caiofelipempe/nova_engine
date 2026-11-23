#pragma once

/// @file
/// @brief WorldOrigin: the point the physics engine's float coordinates are measured from.

#include "nova/math/GlobalPosition.h"

#include <bit>

namespace nova {

/// @brief The origin of the physics engine's float coordinates: an exact GlobalPosition (i64
///        Q45.18) on a grid, that follows a focus (a player, the camera) with hysteresis.
///
/// The physics engine sees `p − origin` in float meters, so it is precise around the focus
/// wherever the focus is in the world. The origin only takes values on a grid of **cells** — a
/// power of two of steps per axis, at most 256 m and at most an eighth of the axis — so a move is
/// a few cells: exact in integers and in floats. It moves when the focus is more than 5/8 of a cell
/// away on some axis (to the cell under the focus), so walking back and forth across a cell border
/// moves nothing.
/// @code
/// nova::WorldOrigin origin;
/// if (origin.needsMove(player)) shiftEngine(origin.moveTo(nova::WorldOrigin::snapped(player)));
/// glm::vec3 local = origin.toLocal(body);
/// @endcode
class WorldOrigin {
public:
    /// @brief Largest cell: 2²⁶ steps (256 m): the origin stays within 160 m of the focus per axis,
    ///        where floats are 15 µm apart or closer.
    static constexpr i64 kMaxCellSteps = i64(1) << 26;

    /// @brief The origin at the world origin.
    WorldOrigin() = default;
    /// @brief The origin at `p`, snapped to the grid.
    explicit WorldOrigin(const GlobalPosition& p) : m_position(snapped(p)) {}

    /// @brief Cell size on axis `axis` of the active world, in steps.
    static i64 cellSteps(int axis) {
        const i64 eighth = WorldSize::active().steps[axis] / 8;
        if (eighth >= kMaxCellSteps) return kMaxCellSteps;
        return eighth < 1 ? 1 : static_cast<i64>(std::bit_floor(static_cast<u64>(eighth)));
    }
    /// @brief `p` rounded to the nearest grid point.
    static GlobalPosition snapped(const GlobalPosition& p) {
        glm::i64vec3 r = p.raw();
        for (int i = 0; i < 3; ++i) {
            const i64 c = cellSteps(i);
            const int shift = std::countr_zero(static_cast<u64>(c));
            r[i] = ((r[i] + c / 2) >> shift) << shift; // arithmetic shift: floor, also below zero
        }
        return GlobalPosition::fromRaw(r);
    }

    /// @brief The origin.
    const GlobalPosition& position() const { return m_position; }
    operator const GlobalPosition&() const { return m_position; }

    /// @brief True when `focus` is more than 5/8 of a cell away on some axis.
    bool needsMove(const GlobalPosition& focus) const {
        const GlobalOffset d = focus - m_position;
        for (int i = 0; i < 3; ++i) {
            const i64 limit = cellSteps(i) / 8 * 5;
            if (d[i] > limit || d[i] < -limit) return true;
        }
        return false;
    }
    /// @brief Moves to `target` by an amount floats represent exactly, and returns it (float
    ///        meters, what the engine's coordinates shift by). Lands on `target` exactly when the
    ///        move is a few cells (always, unless it crosses the seam of a world whose size is not
    ///        a whole number of cells).
    glm::vec3 moveTo(const GlobalPosition& target) {
        const glm::vec3 shift = (target - m_position).toVec3();
        m_position += GlobalOffset::fromMeters(shift);
        return shift;
    }
    /// @brief Follows `focus`: moves to its cell when needsMove(). Returns the shift (zero if none).
    glm::vec3 follow(const GlobalPosition& focus) {
        return needsMove(focus) ? moveTo(snapped(focus)) : glm::vec3(0.0f);
    }

    /// @brief `p` in float meters from the origin (the shortest way).
    glm::vec3 toLocal(const GlobalPosition& p) const { return (p - m_position).toVec3(); }
    /// @brief Float meters from the origin back to a position.
    GlobalPosition toGlobal(const glm::vec3& p) const { return m_position + p; }

    bool operator==(const WorldOrigin&) const = default;

private:
    GlobalPosition m_position;
};

} // namespace nova
