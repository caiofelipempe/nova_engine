#include "nova/math/GlobalPosition.h"

#include <cmath>

namespace nova {

WorldSize WorldSize::s_active;

namespace {

// Nearest step to `steps` (any finite value) on axis `axis` of the active world: whole laps drop out.
i64 toWrappedSteps(f64 steps, int axis) {
    if (!std::isfinite(steps)) return 0;
    const WorldSize& w = WorldSize::active();
    const f64 lap = static_cast<f64>(w.steps[axis]);
    f64 r = std::fmod(steps, lap); // (−lap, lap), exact
    if (r >= lap * 0.5) r -= lap;
    else if (r < -lap * 0.5) r += lap;
    return w.wrap(std::llround(r), axis); // rounding may reach +size/2
}

// a · n modulo the lap, without overflow: values stay below 2⁵⁸, sums below 2⁵⁹.
i64 mulWrapped(i64 a, i64 n, int axis) {
    const WorldSize& w = WorldSize::active();
    a = w.wrap(a, axis);
    const bool negative = n < 0;
    u64 k = negative ? 0u - static_cast<u64>(n) : static_cast<u64>(n);
    i64 result = 0;
    while (k) {
        if (k & 1u) result = w.wrap(result + a, axis);
        a = w.wrap(a * 2, axis);
        k >>= 1;
    }
    return negative ? w.wrap(-result, axis) : result;
}

f64 lengthSteps(const GlobalOffset& o) {
    if (o.isShort()) return std::sqrt(static_cast<f64>(o.lengthSquared()));
    return std::sqrt(static_cast<f64>(o.x) * o.x + static_cast<f64>(o.y) * o.y + static_cast<f64>(o.z) * o.z);
}

} // namespace

// ------------------------------------------------------------- GlobalOffset --
GlobalOffset GlobalOffset::fromMeters(const glm::dvec3& m) {
    constexpr f64 perMeter = static_cast<f64>(WorldSpace::kStepsPerMeter);
    return {toWrappedSteps(m.x * perMeter, 0), toWrappedSteps(m.y * perMeter, 1), toWrappedSteps(m.z * perMeter, 2)};
}

glm::dvec3 GlobalOffset::toMeters() const {
    return glm::dvec3(static_cast<f64>(x), static_cast<f64>(y), static_cast<f64>(z)) * WorldSpace::kUnit;
}

glm::dvec3 GlobalOffset::toUnits(LengthUnit u) const { return toMeters() / metersPer(u); }

f64 GlobalOffset::length() const { return lengthSteps(*this) * WorldSpace::kUnit; }

f64 GlobalOffset::length(LengthUnit u) const { return lengthSteps(*this) * (WorldSpace::kUnit / metersPer(u)); }

bool GlobalOffset::isShorterThan(f64 meters) const {
    if (!(meters > 0.0)) return false;
    const f64 steps = meters * static_cast<f64>(WorldSpace::kStepsPerMeter);
    if (!isShort()) return lengthSteps(*this) < steps;
    if (steps >= 4294967296.0) return true; // longer than any short offset (≤ 2³¹·√3 < 2³²)
    // length < steps ⇔ length² < steps², exact on the integer part; the last step in double.
    const u64 n = static_cast<u64>(steps);
    const u64 l2 = lengthSquared();
    if (l2 < n * n) return true;
    if (l2 >= (n + 1) * (n + 1)) return false;
    return std::sqrt(static_cast<f64>(l2)) < steps;
}

GlobalOffset GlobalOffset::scaled(f64 factor) const {
    return {toWrappedSteps(static_cast<f64>(x) * factor, 0), toWrappedSteps(static_cast<f64>(y) * factor, 1),
            toWrappedSteps(static_cast<f64>(z) * factor, 2)};
}

GlobalOffset operator*(const GlobalOffset& a, i64 n) {
    return {mulWrapped(a.x, n, 0), mulWrapped(a.y, n, 1), mulWrapped(a.z, n, 2)};
}

int compareLength(const GlobalOffset& a, const GlobalOffset& b) {
    if (a.isShort() && b.isShort()) {
        const u64 la = a.lengthSquared(), lb = b.lengthSquared();
        return la < lb ? -1 : (lb < la ? 1 : 0);
    }
    const f64 la = lengthSteps(a), lb = lengthSteps(b);
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
