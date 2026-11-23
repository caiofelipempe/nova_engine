#pragma once

/// @file
/// @brief The world: a 3-torus of fixed-point positions (i64 Q45.18), circular on every axis.

#include "nova/core/Base.h"
#include "nova/math/Units.h"

#include <glm/glm.hpp>
#include <glm/ext/vector_int3_sized.hpp>

#include <cmath>

namespace nova {

/// @brief The space positions live in: three `int64` in Q45.18 fixed point, circular on every axis.
///
/// A step is 2⁻¹⁸ m (≈ 3.8 µm, the smallest movement that matters to the physics). The world is
/// always circular: each axis has a size (WorldSize, 16 384 m by default), leaving one side enters
/// the other, and the difference of two positions is the shortest way around.
struct WorldSpace {
    using WorldInt = i64;                                    ///< Integer of a position axis.
    static constexpr int kFractionBits = 18;                 ///< Fraction bits of a position.
    static constexpr i64 kStepsPerMeter = i64(1) << kFractionBits; ///< Steps in one meter.
    static constexpr f64 kUnit = 1.0 / static_cast<f64>(kStepsPerMeter); ///< Meters per step (2⁻¹⁸ m ≈ 3.8 µm).
    static constexpr f64 kDefaultSize = 16384.0;             ///< Default size of an axis (m): 2³² steps.
    static constexpr f64 kMinSize = 1.0;                     ///< Smallest size of an axis (m).
    static constexpr f64 kMaxSize = 1099511627776.0;         ///< Largest size of an axis (m): 2⁴⁰ m = 2⁵⁸ steps.
};

/// @brief The world's size per axis. Every axis is circular: positions run from −size/2 to
///        +size/2, and leaving one side enters the other.
///
/// Sizes are whole, even numbers of steps. One size is **active** at a time (Scene activates its
/// own): GlobalPosition and GlobalOffset arithmetic goes around it.
/// @code
/// scene.setWorldSize(nova::WorldSize::meters(48, 48, 48)); // a tiny world
/// @endcode
struct WorldSize {
    static constexpr i64 kDefaultSteps = i64(1) << 32; ///< 16 384 m.

    glm::i64vec3 steps{kDefaultSteps}; ///< Size of each axis in steps (even).

    /// @brief From meters per axis; 0 (or anything not positive) leaves that axis at the default.
    ///        Sizes are clamped to [kMinSize, kMaxSize] and rounded to an even number of steps.
    static WorldSize meters(f64 x, f64 y, f64 z) {
        WorldSize w;
        const f64 m[3] = {x, y, z};
        for (int i = 0; i < 3; ++i) w.steps[i] = toSteps(m[i]);
        return w;
    }
    /// @brief The same size on every axis (see meters()).
    static WorldSize cube(f64 size) { return meters(size, size, size); }

    /// @brief Size of axis `i` in meters.
    f64 size(int i) const { return static_cast<f64>(steps[i]) * WorldSpace::kUnit; }
    /// @brief Sizes in meters.
    glm::dvec3 sizes() const { return {size(0), size(1), size(2)}; }
    /// @brief Half the size of axis `i`, in steps.
    i64 halfSteps(int i) const { return steps[i] / 2; }
    /// @brief The smallest axis, in meters.
    f64 smallest() const { return std::fmin(size(0), std::fmin(size(1), size(2))); }
    /// @brief Every axis at the default size.
    bool isDefault() const { return *this == WorldSize{}; }

    /// @brief `v` (any step count) on axis `i`, brought into [−size/2, size/2).
    i64 wrap(i64 v, int i) const {
        const i64 lap = steps[i], half = lap / 2;
        if (v >= -half && v < half) return v; // the common case: already there
        i64 r = v % lap;                      // (−lap, lap)
        if (r >= half) r -= lap;
        else if (r < -half) r += lap;
        return r;
    }

    /// @brief `meters` brought to a valid size in steps (see meters()).
    static i64 toSteps(f64 meters) {
        if (!(meters > 0.0)) return kDefaultSteps;
        const f64 m = std::fmin(std::fmax(meters, WorldSpace::kMinSize), WorldSpace::kMaxSize);
        return std::llround(m * static_cast<f64>(WorldSpace::kStepsPerMeter) * 0.5) * 2;
    }

    bool operator==(const WorldSize&) const = default;

    /// @brief The size positions go around now.
    static const WorldSize& active() { return s_active; }
    /// @brief Makes `size` the active one (Scene does it for its own world).
    static void setActive(const WorldSize& size) { s_active = size; }

private:
    static NOVA_API WorldSize s_active;
};

/// @brief Makes a size active for a scope, then restores the previous one.
class ScopedWorldSize {
public:
    explicit ScopedWorldSize(const WorldSize& size) : m_previous(WorldSize::active()) { WorldSize::setActive(size); }
    ~ScopedWorldSize() { WorldSize::setActive(m_previous); }
    ScopedWorldSize(const ScopedWorldSize&) = delete;
    ScopedWorldSize& operator=(const ScopedWorldSize&) = delete;

private:
    WorldSize m_previous;
};

} // namespace nova
