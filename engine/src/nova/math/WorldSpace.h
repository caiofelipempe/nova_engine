#pragma once

/// @file
/// @brief The world: a 16 km torus of fixed-point positions (Q13.18).

#include "nova/core/Base.h"
#include "nova/math/Units.h"

namespace nova {

/// @brief The space positions live in: every axis is 16 384 m long and **circular** (a 3-torus).
///
/// A position is three `int32` in Q13.18 fixed point: steps of 2⁻¹⁸ m (≈ 3.8 µm, the smallest
/// movement that matters to the physics), ±8 192 m. The lap is the integer's own overflow: leaving
/// one side enters the other, and the difference of two positions is the shortest way around.
/// The whole world fits the range where `float` physics is precise, so nothing needs a floating
/// origin.
///
/// `WorldInt` and `kFractionBits` are the only things a future large-world build
/// (`NOVA_LARGE_WORLDS`: int64, chunks) changes; see docs/WORLD_SPACE.md.
struct WorldSpace {
    using WorldInt = i32;                              ///< Integer of a position axis.
    static constexpr int kFractionBits = 18;           ///< Fraction bits of a position.
    static constexpr f64 kUnit = 1.0 / (1 << kFractionBits); ///< Meters per step (2⁻¹⁸ m ≈ 3.8 µm).
    static constexpr f64 kLap = 4294967296.0 * kUnit;  ///< One turn of an axis: 2³² steps = 16 384 m.
    static constexpr f64 kHalfLap = kLap / 2.0;        ///< Positions are in [−kHalfLap, kHalfLap).
};

} // namespace nova
