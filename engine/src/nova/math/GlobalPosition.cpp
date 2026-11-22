#include "nova/math/GlobalPosition.h"

#include <cmath>

namespace nova {

namespace {

constexpr f64 kTwoPow31 = 2147483648.0;
constexpr f64 kTwoPow32 = 4294967296.0;

// Nearest step to `steps` (any finite value), around the torus: whole laps drop out.
i32 toWrappedSteps(f64 steps) {
    if (!std::isfinite(steps)) return 0;
    f64 r = std::fmod(steps, kTwoPow32); // (−2³², 2³²), exact
    if (r >= kTwoPow31) r -= kTwoPow32;
    else if (r < -kTwoPow31) r += kTwoPow32;
    // |r| ≤ 2³¹, so it rounds to a value whose low 32 bits are the step (2³¹ ≡ −2³¹).
    return static_cast<i32>(static_cast<u32>(static_cast<u64>(std::llround(r))));
}

} // namespace

// ------------------------------------------------------------- GlobalOffset --
GlobalOffset GlobalOffset::fromMeters(const glm::dvec3& m) {
    constexpr f64 perMeter = 1.0 / WorldSpace::kUnit;
    return {toWrappedSteps(m.x * perMeter), toWrappedSteps(m.y * perMeter), toWrappedSteps(m.z * perMeter)};
}

glm::dvec3 GlobalOffset::toMeters() const { return glm::dvec3(x, y, z) * WorldSpace::kUnit; }

glm::dvec3 GlobalOffset::toUnits(LengthUnit u) const { return glm::dvec3(x, y, z) * (WorldSpace::kUnit / metersPer(u)); }

f64 GlobalOffset::length() const { return std::sqrt(static_cast<f64>(lengthSquared())) * WorldSpace::kUnit; }

f64 GlobalOffset::length(LengthUnit u) const {
    return std::sqrt(static_cast<f64>(lengthSquared())) * (WorldSpace::kUnit / metersPer(u));
}

bool GlobalOffset::isShorterThan(f64 meters) const {
    if (!(meters > 0.0)) return false;
    const f64 steps = meters / WorldSpace::kUnit;
    if (steps >= kTwoPow32) return true; // longer than any offset (≤ 2³¹·√3)
    // length < steps ⇔ length² < steps², exact on the integer part; the last step in double.
    const u64 n = static_cast<u64>(steps);
    const u64 l2 = lengthSquared();
    if (l2 < n * n) return true;
    if (l2 >= (n + 1) * (n + 1)) return false;
    return std::sqrt(static_cast<f64>(l2)) < steps;
}

GlobalOffset GlobalOffset::scaled(f64 factor) const {
    return {toWrappedSteps(x * factor), toWrappedSteps(y * factor), toWrappedSteps(z * factor)};
}

int compareLength(const GlobalOffset& a, const GlobalOffset& b) {
    const u64 la = a.lengthSquared(), lb = b.lengthSquared();
    return la < lb ? -1 : (lb < la ? 1 : 0);
}

// ----------------------------------------------------------- GlobalPosition --
GlobalPosition GlobalPosition::fromMeters(const glm::dvec3& m) { return fromRaw(GlobalOffset::fromMeters(m).raw()); }

glm::dvec3 GlobalPosition::toMeters() const { return GlobalOffset(m_steps).toMeters(); }

glm::dvec3 GlobalPosition::toUnits(LengthUnit u) const { return GlobalOffset(m_steps).toUnits(u); }

// ------------------------------------------------------------------ helpers --
f64 distance(const GlobalPosition& a, const GlobalPosition& b) { return (b - a).length(); }

f64 distance(const GlobalPosition& a, const GlobalPosition& b, LengthUnit u) { return (b - a).length(u); }

bool isWithin(const GlobalPosition& a, const GlobalPosition& b, f64 meters) { return (b - a).isShorterThan(meters); }

GlobalPosition lerp(const GlobalPosition& a, const GlobalPosition& b, f64 t) {
    if (t == 1.0) return b;
    return a + (b - a).scaled(t);
}

GlobalPosition moveTowards(const GlobalPosition& from, const GlobalPosition& to, f64 maxMeters) {
    const GlobalOffset d = to - from;
    const f64 len = d.length();
    if (len <= maxMeters || len == 0.0) return to;
    return from + d.scaled(maxMeters / len);
}

GlobalPosition rotateAround(const GlobalPosition& point, const GlobalPosition& pivot, const glm::quat& rotation) {
    const glm::dvec3 local = (point - pivot).toMeters();
    return pivot + GlobalOffset::fromMeters(glm::dquat(rotation) * local);
}

} // namespace nova
