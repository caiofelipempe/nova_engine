#pragma once

/// @file
/// @brief The world's size and edges per axis: wrap around or barrier. Applied by the Scene,
///        outside the physics engine.

#include "nova/math/GlobalPosition.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace nova {

/// @brief What happens at the edges of the world along one axis.
enum class WorldEdge : u8 {
    Wrap,    ///< Circular: leaving one side enters at the other (velocity kept).
    Barrier, ///< Not circular: walls at the edges (physics bodies), and a stop for what they miss.
};

/// @brief Size and edge of the world along one axis (centered on the origin).
struct WorldAxis {
    f64 size = WorldSpace::kLap;    ///< Meters, from −size/2 to +size/2.
    WorldEdge edge = WorldEdge::Wrap; ///< Wrap or barrier.
    bool operator==(const WorldAxis&) const = default;
};

/// @brief Which edge an entity's center reached (see Script::onWorldEdge).
struct WorldEdgeHit {
    int axis = 0;       ///< 0 = X, 1 = Y, 2 = Z.
    int side = 1;       ///< +1: the positive edge, −1: the negative one.
    WorldEdge edge = WorldEdge::Wrap; ///< Wrapped to the other side, or a barrier (wall touched or center stopped).
};

/// @brief The world's shape: per axis, a size and an edge (wrap or barrier).
///
/// The default is the whole 16 km torus on every axis (positions wrap by themselves: nothing to
/// do). Otherwise, per axis:
/// - **Wrap** (circular, smaller than the torus): what crosses an edge is moved to the other one,
///   velocity kept, outside the physics engine (bodies do not collide across that edge).
/// - **Barrier** (not circular): while simulating, the scene puts static walls just outside the
///   edges (kWallThickness thick; bodies collide and bounce on them, Script::onWorldEdge reports
///   the touch). What the walls do not hold — entities without a body, bodies in layers that do not
///   collide with layer 0, a tunneling body — is stopped by its **center**: back to the edge, with
///   the velocity along that axis zeroed.
/// The center rules use the transform position (for characters, the capsule center), after every
/// physics step and once per update for entities without simulated bodies.
struct WorldBounds {
    /// @brief Largest barrier size (m): leaves room so no step crosses the torus seam unseen.
    static constexpr f64 kMaxBarrierSize = WorldSpace::kLap - WorldSpace::kLap / 16.0;
    /// @brief Smallest size of an axis (m).
    static constexpr f64 kMinSize = 1.0;
    /// @brief Thickness of the barrier walls (m), outside the world.
    static constexpr f64 kWallThickness = 4.0;
    /// @brief Half length of the walls along axes that are the whole torus (m).
    static constexpr f64 kWallReach = WorldSpace::kLap / 4.0;

    std::array<WorldAxis, 3> axes; ///< X, Y, Z.

    /// @brief A world `x` × `y` × `z` meters with the same edge on every limited axis; a size of
    ///        0 leaves that axis as the whole torus (wrapping by itself).
    static WorldBounds box(f64 x, f64 y, f64 z, WorldEdge edge) {
        WorldBounds b;
        const f64 sizes[3] = {x, y, z};
        for (int i = 0; i < 3; ++i)
            if (sizes[i] > 0.0) b.axes[i] = {sizes[i], edge};
        return b.valid();
    }

    /// @brief True when an axis is the whole torus with wrapping (nothing to enforce).
    bool isNative(int axis) const { return axes[axis].edge == WorldEdge::Wrap && axes[axis].size >= WorldSpace::kLap; }
    /// @brief True when no axis needs enforcing.
    bool isNative() const { return isNative(0) && isNative(1) && isNative(2); }

    /// @brief Sizes brought into range (barriers up to kMaxBarrierSize, wraps up to the lap).
    WorldBounds valid() const {
        WorldBounds b = *this;
        for (WorldAxis& a : b.axes) {
            const f64 max = a.edge == WorldEdge::Barrier ? kMaxBarrierSize : WorldSpace::kLap;
            a.size = a.size != a.size ? max : std::clamp(a.size, kMinSize, max);
        }
        return b;
    }

    /// @brief Half the size of an axis, in world steps.
    i64 halfSteps(int axis) const { return std::llround(axes[axis].size * 0.5 / WorldSpace::kUnit); }

    /// @brief Brings a center back inside: barriers clamp it, small wraps move it one size.
    /// @param hits Optional: receives the edges reached (at most one per axis).
    /// @return Bit `i` set when axis `i` was changed.
    u32 constrain(GlobalPosition& p, std::array<WorldEdgeHit, 3>* hits = nullptr) const {
        glm::i32vec3 raw = p.raw();
        u32 changed = 0;
        for (int i = 0; i < 3; ++i) {
            if (isNative(i)) continue;
            const i64 half = halfSteps(i), v = raw[i];
            i64 out = v;
            int side = 0;
            if (axes[i].edge == WorldEdge::Barrier) {
                if (v > half) out = half, side = 1;
                else if (v < -half) out = -half, side = -1;
            } else { // wrap within [−half, +half)
                if (v >= half) out = v - 2 * half, side = 1;
                else if (v < -half) out = v + 2 * half, side = -1;
            }
            if (!side) continue;
            raw[i] = static_cast<i32>(out);
            changed |= 1u << i;
            if (hits) (*hits)[static_cast<size_t>(i)] = {i, side, axes[i].edge};
        }
        if (changed) p = GlobalPosition::fromRaw(raw);
        return changed;
    }

    /// @brief True when an axis has barrier walls.
    bool hasWalls(int axis) const { return axes[axis].edge == WorldEdge::Barrier; }

    bool operator==(const WorldBounds&) const = default;
};

} // namespace nova
