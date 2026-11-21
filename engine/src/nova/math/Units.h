#pragma once

/// @file
/// @brief Metric length units and conversions (the engine's base unit is the meter).

#include "nova/core/Base.h"

namespace nova {

/// @brief Metric length units.
enum class LengthUnit : u8 {
    Nanometer,  ///< 1e-9 m
    Micrometer, ///< 1e-6 m
    Millimeter, ///< 1e-3 m
    Centimeter, ///< 1e-2 m
    Decimeter,  ///< 1e-1 m
    Meter,      ///< 1 m (the engine's unit)
    Kilometer,  ///< 1e3 m
    Megameter,  ///< 1e6 m (1000 km)
};

/// @brief Meters in one `unit`.
constexpr f64 metersPer(LengthUnit unit) {
    switch (unit) {
        case LengthUnit::Nanometer: return 1e-9;
        case LengthUnit::Micrometer: return 1e-6;
        case LengthUnit::Millimeter: return 1e-3;
        case LengthUnit::Centimeter: return 1e-2;
        case LengthUnit::Decimeter: return 1e-1;
        case LengthUnit::Meter: return 1.0;
        case LengthUnit::Kilometer: return 1e3;
        case LengthUnit::Megameter: return 1e6;
    }
    return 1.0;
}

/// @brief Symbol for display ("mm", "km"...).
constexpr const char* unitSymbol(LengthUnit unit) {
    switch (unit) {
        case LengthUnit::Nanometer: return "nm";
        case LengthUnit::Micrometer: return "um";
        case LengthUnit::Millimeter: return "mm";
        case LengthUnit::Centimeter: return "cm";
        case LengthUnit::Decimeter: return "dm";
        case LengthUnit::Meter: return "m";
        case LengthUnit::Kilometer: return "km";
        case LengthUnit::Megameter: return "Mm";
    }
    return "m";
}

/// @brief `value` expressed in `from`, converted to `to` (e.g. `convertLength(2.5, Kilometer, Meter) == 2500`).
constexpr f64 convertLength(f64 value, LengthUnit from, LengthUnit to) {
    return from == to ? value : value * metersPer(from) / metersPer(to);
}

/// @brief `value` in `unit`, in meters.
constexpr f64 toMeters(f64 value, LengthUnit unit) { return value * metersPer(unit); }
/// @brief `meters` expressed in `unit`.
constexpr f64 fromMeters(f64 meters, LengthUnit unit) { return meters / metersPer(unit); }

/// @brief The largest unit (km → nm) in which `meters` reads as at least 1 — for display.
constexpr LengthUnit readableUnit(f64 meters) {
    const f64 m = meters < 0 ? -meters : meters;
    constexpr LengthUnit order[] = {LengthUnit::Megameter, LengthUnit::Kilometer, LengthUnit::Meter,
                                    LengthUnit::Centimeter, LengthUnit::Millimeter, LengthUnit::Micrometer};
    for (LengthUnit u : order)
        if (m >= metersPer(u)) return u;
    return LengthUnit::Nanometer;
}

} // namespace nova
