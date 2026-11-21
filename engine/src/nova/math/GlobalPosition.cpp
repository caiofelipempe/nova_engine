#include "nova/math/GlobalPosition.h"

#include <cmath>

namespace nova {

namespace {

constexpr i64 kMax = std::numeric_limits<i64>::max();
constexpr i64 kMin = std::numeric_limits<i64>::min();
constexpr f64 kTwoPow32 = 4294967296.0;
constexpr f64 kTwoPow63 = 9223372036854775808.0;
constexpr f64 kTwoPow64 = 18446744073709551616.0;

// Nearest int64 to a finite double, saturating (NaN → 0).
i64 toSteps(f64 v) {
    if (std::isnan(v)) return 0;
    if (v >= kTwoPow63) return kMax;
    if (v <= -kTwoPow63) return kMin;
    return static_cast<i64>(std::llround(v));
}

u64 magnitude(i64 v) { return v >= 0 ? static_cast<u64>(v) : static_cast<u64>(-(v + 1)) + 1; }

// v * t rounded to the nearest step, precise to well under one step even when |v| > 2^53:
// v = hi * 2^32 + lo, and the rounding error of hi * t is recovered with an FMA.
i64 scaleSteps(i64 v, f64 t) {
    if (t == 0.0 || v == 0) return 0;
    if (t == 1.0) return v;
    if (!std::isfinite(t)) return 0;
    const i64 hi = v >> 32; // arithmetic shift: floor(v / 2^32)
    const u64 lo = static_cast<u64>(v) & 0xFFFFFFFFull;
    const f64 hp = static_cast<f64>(hi) * t;
    const f64 err = std::fma(static_cast<f64>(hi), t, -hp); // exact error of the product
    const f64 whole = std::floor(hp);
    const f64 low = ((hp - whole) + err) * kTwoPow32 + static_cast<f64>(lo) * t;
    // result = whole * 2^32 + low; whole must fit in 32 bits for the shift below.
    if (whole >= 2147483648.0) return kMax;
    if (whole < -2147483648.0) return kMin;
    const i64 base = static_cast<i64>(whole) * (i64(1) << 32);
    return detail::saturatingAdd(base, toSteps(low));
}

} // namespace

// ------------------------------------------------------------- GlobalOffset --
GlobalOffset GlobalOffset::fromMeters(const glm::dvec3& m) {
    const f64 unit = WorldSpace::active().unit();
    return {toSteps(m.x / unit), toSteps(m.y / unit), toSteps(m.z / unit)};
}

glm::dvec3 GlobalOffset::toMeters() const {
    const f64 unit = WorldSpace::active().unit();
    return {static_cast<f64>(x) * unit, static_cast<f64>(y) * unit, static_cast<f64>(z) * unit};
}

glm::dvec3 GlobalOffset::toUnits(LengthUnit u) const {
    // One factor from steps to the target unit: a single rounding per component.
    const f64 k = WorldSpace::active().unit() / metersPer(u);
    return {static_cast<f64>(x) * k, static_cast<f64>(y) * k, static_cast<f64>(z) * k};
}

UInt128 GlobalOffset::lengthSquared() const {
    UInt128 sum;
    for (int i = 0; i < 3; ++i) {
        const u64 m = magnitude((*this)[i]);
        sum = sum + UInt128::mul(m, m);
    }
    return sum;
}

f64 GlobalOffset::length() const { return std::sqrt(lengthSquared().toDouble()) * WorldSpace::active().unit(); }

f64 GlobalOffset::length(LengthUnit u) const {
    return std::sqrt(lengthSquared().toDouble()) * (WorldSpace::active().unit() / metersPer(u));
}

bool GlobalOffset::isShorterThan(f64 meters) const {
    if (!(meters > 0.0)) return false;
    const f64 steps = meters / WorldSpace::active().unit();
    if (steps >= kTwoPow64) return true; // longer than any offset can be... on one axis; check exactly below
    // length < r  ⇔  length² < r²  with r = ceil(steps) when steps is fractional: compare against
    // floor and fall back to double for the fractional part (only matters within one step).
    const u64 r = static_cast<u64>(steps);
    const UInt128 l2 = lengthSquared();
    const UInt128 r2 = UInt128::mul(r, r);
    if (l2 < r2) return true;
    const UInt128 r2next = UInt128::mul(r + 1, r + 1);
    if (l2 >= r2next) return false;
    return std::sqrt(l2.toDouble()) < steps; // within the last step: double is exact enough
}

GlobalOffset GlobalOffset::scaled(f64 t) const { return {scaleSteps(x, t), scaleSteps(y, t), scaleSteps(z, t)}; }

GlobalOffset operator*(const GlobalOffset& a, i64 n) {
    GlobalOffset r;
    for (int i = 0; i < 3; ++i) {
        const i64 v = a[i];
        const bool negative = (v < 0) != (n < 0);
        const UInt128 p = UInt128::mul(magnitude(v), magnitude(n));
        const u64 limit = negative ? (u64(1) << 63) : static_cast<u64>(kMax);
        if (p.hi != 0 || p.lo > limit) r[i] = negative ? kMin : kMax;
        else r[i] = negative ? static_cast<i64>(0 - p.lo) : static_cast<i64>(p.lo);
    }
    return r;
}

int compareLength(const GlobalOffset& a, const GlobalOffset& b) {
    const UInt128 la = a.lengthSquared(), lb = b.lengthSquared();
    return la < lb ? -1 : (lb < la ? 1 : 0);
}

// ----------------------------------------------------------- GlobalPosition --
GlobalPosition GlobalPosition::fromRaw(i64 x, i64 y, i64 z) {
    const WorldSpace& w = WorldSpace::active();
    GlobalPosition p;
    p.m_v[0] = w.normalize(0, x);
    p.m_v[1] = w.normalize(1, y);
    p.m_v[2] = w.normalize(2, z);
    return p;
}

GlobalPosition GlobalPosition::fromMeters(const glm::dvec3& m) {
    const WorldSpace& w = WorldSpace::active();
    GlobalPosition p;
    for (int i = 0; i < 3; ++i) {
        f64 steps = m[i] / w.unit();
        const WorldSpace::Axis& a = w.axis(i);
        if (a.boundary == WorldBoundary::Wrap && std::isfinite(steps) && std::abs(steps) >= kTwoPow63) {
            // Beyond int64: reduce modulo the span first so the value loops instead of saturating.
            steps = std::fmod(steps, a.full ? kTwoPow64 : static_cast<f64>(a.span));
        }
        f64 reduced = steps;
        if (a.full) { // map into [-2^63, 2^63) so the conversion below keeps the wrapped value
            if (reduced >= kTwoPow63) reduced -= kTwoPow64;
            else if (reduced < -kTwoPow63) reduced += kTwoPow64;
        }
        p.m_v[i] = w.normalize(i, toSteps(reduced));
    }
    return p;
}

glm::dvec3 GlobalPosition::toMeters() const {
    const f64 unit = WorldSpace::active().unit();
    return {static_cast<f64>(m_v[0]) * unit, static_cast<f64>(m_v[1]) * unit, static_cast<f64>(m_v[2]) * unit};
}

glm::dvec3 GlobalPosition::toUnits(LengthUnit u) const {
    const f64 k = WorldSpace::active().unit() / metersPer(u);
    return {static_cast<f64>(m_v[0]) * k, static_cast<f64>(m_v[1]) * k, static_cast<f64>(m_v[2]) * k};
}

u8 GlobalPosition::move(const GlobalOffset& offset) {
    const WorldSpace& w = WorldSpace::active();
    u8 hit = 0;
    for (int i = 0; i < 3; ++i) {
        bool stopped = false;
        m_v[i] = w.add(i, m_v[i], offset[i], stopped);
        if (stopped) hit |= static_cast<u8>(1u << i);
    }
    return hit;
}

GlobalOffset operator-(const GlobalPosition& a, const GlobalPosition& b) {
    const WorldSpace& w = WorldSpace::active();
    return {w.difference(0, a.m_v[0], b.m_v[0]), w.difference(1, a.m_v[1], b.m_v[1]),
            w.difference(2, a.m_v[2], b.m_v[2])};
}

// ------------------------------------------------------------------ helpers --
f64 distance(const GlobalPosition& a, const GlobalPosition& b) { return (b - a).length(); }

f64 distance(const GlobalPosition& a, const GlobalPosition& b, LengthUnit u) { return (b - a).length(u); }

UInt128 distanceSquared(const GlobalPosition& a, const GlobalPosition& b) { return (b - a).lengthSquared(); }

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
    const glm::dvec3 rotated = glm::dquat(rotation) * local;
    return pivot + GlobalOffset::fromMeters(rotated);
}

} // namespace nova
