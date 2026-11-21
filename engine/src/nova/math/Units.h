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

// ------------------------------------------------------------------- time --

/// @brief Time units (the engine's base unit is the second; exact time is kept in flicks, see Time.h).
enum class TimeUnit : u8 {
    Nanosecond,  ///< 1e-9 s
    Microsecond, ///< 1e-6 s
    Millisecond, ///< 1e-3 s
    Second,      ///< 1 s
    Minute,      ///< 60 s
    Hour,        ///< 3600 s
    Day,         ///< 86400 s
};

/// @brief Seconds in one `unit`.
constexpr f64 secondsPer(TimeUnit unit) {
    switch (unit) {
        case TimeUnit::Nanosecond: return 1e-9;
        case TimeUnit::Microsecond: return 1e-6;
        case TimeUnit::Millisecond: return 1e-3;
        case TimeUnit::Second: return 1.0;
        case TimeUnit::Minute: return 60.0;
        case TimeUnit::Hour: return 3600.0;
        case TimeUnit::Day: return 86400.0;
    }
    return 1.0;
}

/// @brief Symbol for display ("ms", "h"...).
constexpr const char* unitSymbol(TimeUnit unit) {
    switch (unit) {
        case TimeUnit::Nanosecond: return "ns";
        case TimeUnit::Microsecond: return "us";
        case TimeUnit::Millisecond: return "ms";
        case TimeUnit::Second: return "s";
        case TimeUnit::Minute: return "min";
        case TimeUnit::Hour: return "h";
        case TimeUnit::Day: return "d";
    }
    return "s";
}

/// @brief `value` expressed in `from`, converted to `to` (e.g. `convertTime(1.5, Hour, Minute) == 90`).
constexpr f64 convertTime(f64 value, TimeUnit from, TimeUnit to) {
    return from == to ? value : value * secondsPer(from) / secondsPer(to);
}

/// @brief `value` in `unit`, in seconds.
constexpr f64 toSeconds(f64 value, TimeUnit unit) { return value * secondsPer(unit); }
/// @brief `seconds` expressed in `unit`.
constexpr f64 fromSeconds(f64 seconds, TimeUnit unit) { return seconds / secondsPer(unit); }

/// @brief The largest unit (days → ns) in which `seconds` reads as at least 1 — for display.
constexpr TimeUnit readableTimeUnit(f64 seconds) {
    const f64 s = seconds < 0 ? -seconds : seconds;
    constexpr TimeUnit order[] = {TimeUnit::Day, TimeUnit::Hour, TimeUnit::Minute, TimeUnit::Second,
                                  TimeUnit::Millisecond, TimeUnit::Microsecond};
    for (TimeUnit u : order)
        if (s >= secondsPer(u)) return u;
    return TimeUnit::Nanosecond;
}

// ----------------------------------------------------------------- length --

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
