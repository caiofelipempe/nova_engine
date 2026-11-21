#pragma once

/// @file
/// @brief Exact time: instants (TimePoint) and intervals (Duration) in integer flicks.
///
/// A flick is 1/705,600,000 s (~1.42 ns). It divides every common frame rate (24, 25, 30, 48, 50,
/// 60, 72, 90, 100, 120, 144 Hz) and audio rate (8–192 kHz) exactly, so fixed time steps are whole
/// numbers: accumulating them never drifts, and 60 steps of 1/60 s are exactly one second.
/// An int64 of flicks covers ±414 years.

#include "nova/math/Integer.h"
#include "nova/math/Units.h"

#include <chrono>
#include <cmath>
#include <compare>
#include <ratio>

namespace nova {

/// @brief Flicks in one second.
constexpr i64 kFlicksPerSecond = 705'600'000;
/// @brief std::chrono duration type counting flicks (interop with std::chrono clocks).
using Flicks = std::chrono::duration<i64, std::ratio<1, kFlicksPerSecond>>;

/// @brief A span of time, exact in flicks. Integer operations saturate instead of overflowing.
/// @code
/// nova::Duration step = nova::Duration::perSecond(60);        // exactly 11,760,000 flicks
/// nova::Duration cooldown = nova::Duration::milliseconds(250);
/// if (now - lastShot >= cooldown) shoot();
/// float dt = step.secondsF();                                  // float seconds for physics/scripts
/// @endcode
class Duration {
public:
    constexpr Duration() = default;

    /// @name Exact construction
    /// @{
    /// @brief From raw flicks.
    static constexpr Duration fromFlicks(i64 flicks) { return Duration(flicks); }
    /// @brief Whole milliseconds (exact: 705,600 flicks each).
    static constexpr Duration milliseconds(i64 ms) { return Duration(detail::saturatingMul(ms, kFlicksPerSecond / 1000)); }
    /// @brief Whole seconds (exact).
    static constexpr Duration seconds(i64 s) { return Duration(detail::saturatingMul(s, kFlicksPerSecond)); }
    /// @brief Whole minutes (exact).
    static constexpr Duration minutes(i64 m) { return Duration(detail::saturatingMul(m, 60 * kFlicksPerSecond)); }
    /// @brief Whole hours (exact).
    static constexpr Duration hours(i64 h) { return Duration(detail::saturatingMul(h, 3600 * kFlicksPerSecond)); }
    /// @brief Whole days (exact).
    static constexpr Duration days(i64 d) { return Duration(detail::saturatingMul(d, 86400 * kFlicksPerSecond)); }
    /// @brief One period of a frequency: `perSecond(60)` = 1/60 s. Exact for every common frame and
    ///        audio rate; otherwise rounded to the nearest flick.
    static constexpr Duration perSecond(i64 hz) {
        return hz <= 0 ? Duration() : Duration((kFlicksPerSecond + hz / 2) / hz);
    }
    /// @}

    /// @name Conversions
    /// @{
    /// @brief From seconds (rounded to the nearest flick, saturating; NaN → 0).
    static Duration fromSeconds(f64 seconds) { return Duration(toFlicks(seconds * static_cast<f64>(kFlicksPerSecond))); }
    /// @brief From a value in any time unit (`fromUnits(16.6, TimeUnit::Millisecond)`).
    static Duration fromUnits(f64 value, TimeUnit u) { return fromSeconds(nova::toSeconds(value, u)); }
    /// @brief From any std::chrono duration (rounded to the nearest flick).
    template <typename Rep, typename Period>
    static Duration fromChrono(std::chrono::duration<Rep, Period> d) {
        return Duration(std::chrono::round<Flicks>(d).count());
    }
    /// @brief Raw flicks.
    constexpr i64 flicks() const { return m_flicks; }
    /// @brief As a std::chrono duration.
    constexpr Flicks toChrono() const { return Flicks(m_flicks); }
    /// @brief Seconds, as double (exact to the flick for spans up to ~148 days).
    constexpr f64 seconds() const { return static_cast<f64>(m_flicks) / static_cast<f64>(kFlicksPerSecond); }
    /// @brief Seconds, as float — for frame deltas and other short spans.
    constexpr f32 secondsF() const { return static_cast<f32>(seconds()); }
    /// @brief Expressed in any time unit.
    constexpr f64 toUnits(TimeUnit u) const { return nova::fromSeconds(seconds(), u); }
    /// @}

    /// @brief This span times `factor` (e.g. slow motion), rounded to the nearest flick.
    Duration scaled(f64 factor) const { return Duration(toFlicks(static_cast<f64>(m_flicks) * factor)); }

    friend constexpr bool operator==(Duration, Duration) = default;
    friend constexpr auto operator<=>(Duration, Duration) = default;
    friend constexpr Duration operator+(Duration a, Duration b) { return Duration(detail::saturatingAdd(a.m_flicks, b.m_flicks)); }
    friend constexpr Duration operator-(Duration a) { return Duration(detail::saturatingNegate(a.m_flicks)); }
    friend constexpr Duration operator-(Duration a, Duration b) { return a + (-b); }
    friend constexpr Duration operator*(Duration a, i64 n) { return Duration(detail::saturatingMul(a.m_flicks, n)); }
    friend constexpr Duration operator*(i64 n, Duration a) { return a * n; }
    /// @brief Integer division (truncates toward zero).
    friend constexpr Duration operator/(Duration a, i64 n) { return n == 0 ? Duration() : Duration(a.m_flicks / n); }
    /// @brief How many whole `b` fit in `a` (truncates toward zero).
    friend constexpr i64 operator/(Duration a, Duration b) { return b.m_flicks == 0 ? 0 : a.m_flicks / b.m_flicks; }
    /// @brief Remainder of `a / b` (same sign as `a`).
    friend constexpr Duration operator%(Duration a, Duration b) {
        return b.m_flicks == 0 ? a : Duration(a.m_flicks % b.m_flicks);
    }
    constexpr Duration& operator+=(Duration d) { return *this = *this + d; }
    constexpr Duration& operator-=(Duration d) { return *this = *this - d; }
    constexpr Duration& operator*=(i64 n) { return *this = *this * n; }

private:
    constexpr explicit Duration(i64 flicks) : m_flicks(flicks) {}
    static i64 toFlicks(f64 v) {
        if (std::isnan(v)) return 0;
        if (v >= 9223372036854775808.0) return detail::kI64Max;
        if (v <= -9223372036854775808.0) return detail::kI64Min;
        return static_cast<i64>(std::llround(v));
    }
    i64 m_flicks = 0;
};

/// @brief An instant on a clock (e.g. a scene's time), in flicks since the clock's start.
///
/// Relations between instants are exact integers: `b - a` is a Duration, `a + d` an instant.
/// For float consumers (shaders, animation curves) use secondsInCycle(), which stays precise
/// however long the game runs.
class TimePoint {
public:
    /// @brief The clock's start.
    constexpr TimePoint() = default;
    /// @brief From raw flicks since the start.
    static constexpr TimePoint fromFlicks(i64 flicks) { return TimePoint(Duration::fromFlicks(flicks)); }
    /// @brief From seconds since the start.
    static TimePoint fromSeconds(f64 seconds) { return TimePoint(Duration::fromSeconds(seconds)); }

    /// @brief Raw flicks since the start.
    constexpr i64 flicks() const { return m_sinceStart.flicks(); }
    /// @brief Time since the start.
    constexpr Duration sinceStart() const { return m_sinceStart; }
    /// @brief Seconds since the start, as double (for display; precise to the flick for ~148 days).
    constexpr f64 seconds() const { return m_sinceStart.seconds(); }
    /// @brief Seconds within a repeating cycle, in `[0, period)`: exact integer modulo, so a float of
    ///        this stays precise forever (the engine gives shaders `secondsInCycle(1 hour)`).
    constexpr f64 secondsInCycle(Duration period) const {
        if (period.flicks() <= 0) return seconds();
        i64 r = flicks() % period.flicks();
        if (r < 0) r += period.flicks();
        return Duration::fromFlicks(r).seconds();
    }

    friend constexpr bool operator==(TimePoint, TimePoint) = default;
    friend constexpr auto operator<=>(TimePoint, TimePoint) = default;
    /// @brief Exact span from `b` to `a`.
    friend constexpr Duration operator-(TimePoint a, TimePoint b) { return a.m_sinceStart - b.m_sinceStart; }
    friend constexpr TimePoint operator+(TimePoint t, Duration d) { return TimePoint(t.m_sinceStart + d); }
    friend constexpr TimePoint operator+(Duration d, TimePoint t) { return t + d; }
    friend constexpr TimePoint operator-(TimePoint t, Duration d) { return TimePoint(t.m_sinceStart - d); }
    constexpr TimePoint& operator+=(Duration d) { return *this = *this + d; }
    constexpr TimePoint& operator-=(Duration d) { return *this = *this - d; }

private:
    constexpr explicit TimePoint(Duration sinceStart) : m_sinceStart(sinceStart) {}
    Duration m_sinceStart;
};

} // namespace nova
