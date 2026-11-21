#include "nova/math/WorldSpace.h"

#include "nova/core/Log.h"

#include <cmath>
#include <limits>

namespace nova {

namespace {

constexpr f64 kTwoPow63 = 9223372036854775808.0;
constexpr f64 kTwoPow64 = 18446744073709551616.0;

// v mod S in [0, S), for any int64 and span S >= 1 (no overflow).
u64 modSpan(i64 v, u64 span) {
    if (v >= 0) return static_cast<u64>(v) % span;
    const u64 magnitude = static_cast<u64>(-(v + 1)) + 1; // |v| without overflowing at INT64_MIN
    const u64 r = magnitude % span;
    return r == 0 ? 0 : span - r;
}

} // namespace

// ----------------------------------------------------------------- UInt128 --
UInt128 UInt128::mul(u64 a, u64 b) {
    const u64 aLo = a & 0xFFFFFFFFull, aHi = a >> 32;
    const u64 bLo = b & 0xFFFFFFFFull, bHi = b >> 32;
    const u64 ll = aLo * bLo, lh = aLo * bHi, hl = aHi * bLo, hh = aHi * bHi;
    const u64 mid = (ll >> 32) + (lh & 0xFFFFFFFFull) + (hl & 0xFFFFFFFFull);
    UInt128 r;
    r.lo = (mid << 32) | (ll & 0xFFFFFFFFull);
    r.hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    return r;
}

f64 UInt128::toDouble() const { return static_cast<f64>(hi) * kTwoPow64 + static_cast<f64>(lo); }

// -------------------------------------------------------------- WorldSpace --
WorldSpace::WorldSpace(const WorldSpaceDesc& desc) : m_desc(desc) {
    if (!(m_desc.unit > 0.0) || !std::isfinite(m_desc.unit)) {
        NOVA_CORE_ERROR("WorldSpace: invalid unit {}; using 2^-32 m", m_desc.unit);
        m_desc.unit = WorldSpaceDesc{}.unit;
    }
    for (int i = 0; i < 3; ++i) {
        WorldAxisDesc& d = m_desc.axes[i];
        Axis& a = m_axes[i];
        a.boundary = d.boundary;
        const f64 steps = d.size > 0.0 ? d.size / m_desc.unit : 0.0;
        if (d.size < 0.0 || !std::isfinite(d.size))
            NOVA_CORE_ERROR("WorldSpace: invalid size {} on axis {}; using the maximum", d.size, i);

        if (d.boundary == WorldBoundary::Wrap) {
            if (!(steps > 0.0) || steps >= kTwoPow64) {
                if (steps > kTwoPow64) NOVA_CORE_WARN("WorldSpace: axis {} larger than 2^64 steps; clamped", i);
                a.full = true;
                a.min = std::numeric_limits<i64>::min();
                a.max = std::numeric_limits<i64>::max();
                continue;
            }
            a.span = static_cast<u64>(steps + 0.5);
        } else {
            if (!(steps > 0.0) || steps >= kTwoPow63) {
                if (steps > kTwoPow63) NOVA_CORE_WARN("WorldSpace: barrier axis {} larger than 2^63 steps; clamped", i);
                a.span = kMaxBarrierSpan;
            } else {
                a.span = static_cast<u64>(steps + 0.5);
            }
        }
        if (a.span < 2) a.span = 2;
        a.min = -static_cast<i64>(a.span / 2);
        a.max = static_cast<i64>(static_cast<u64>(a.min) + (a.span - 1));
    }
    for (int i = 0; i < 3; ++i) m_desc.axes[i].size = sizeMeters(i);
}

f64 WorldSpace::sizeMeters(int i) const {
    const Axis& a = m_axes[i];
    return (a.full ? kTwoPow64 : static_cast<f64>(a.span)) * m_desc.unit;
}

i64 WorldSpace::normalize(int i, i64 value) const {
    const Axis& a = m_axes[i];
    if (a.full) return value;
    if (a.boundary == WorldBoundary::Barrier) return value < a.min ? a.min : (value > a.max ? a.max : value);
    // Wrap: (value - min) mod span, without overflow.
    u64 offset;
    if (value >= a.min) {
        offset = (static_cast<u64>(value) - static_cast<u64>(a.min)) % a.span;
    } else {
        const u64 below = (static_cast<u64>(a.min) - static_cast<u64>(value)) % a.span;
        offset = below == 0 ? 0 : a.span - below;
    }
    return static_cast<i64>(static_cast<u64>(a.min) + offset);
}

i64 WorldSpace::add(int i, i64 position, i64 offset, bool& hitBarrier) const {
    const Axis& a = m_axes[i];
    if (a.full) return static_cast<i64>(static_cast<u64>(position) + static_cast<u64>(offset)); // native wrap
    if (a.boundary == WorldBoundary::Barrier) {
        // position ∈ [min, max] with |min|, |max| <= 2^62: neither bound below overflows.
        if (offset > 0 && position > a.max - offset) {
            hitBarrier = true;
            return a.max;
        }
        if (offset < 0 && position < a.min - offset) {
            hitBarrier = true;
            return a.min;
        }
        return position + offset;
    }
    // Wrap with an arbitrary span: modular addition of two values in [0, span).
    const u64 x = static_cast<u64>(position) - static_cast<u64>(a.min);
    const u64 y = modSpan(offset, a.span);
    const u64 sum = x >= a.span - y ? x - (a.span - y) : x + y;
    return static_cast<i64>(static_cast<u64>(a.min) + sum);
}

i64 WorldSpace::difference(int i, i64 a, i64 b) const {
    const Axis& ax = m_axes[i];
    if (ax.full) return static_cast<i64>(static_cast<u64>(a) - static_cast<u64>(b)); // shortest by construction
    if (ax.boundary == WorldBoundary::Barrier) return a - b; // span <= 2^63: always fits
    const u64 xa = static_cast<u64>(a) - static_cast<u64>(ax.min);
    const u64 xb = static_cast<u64>(b) - static_cast<u64>(ax.min);
    const u64 r = xa >= xb ? xa - xb : ax.span - (xb - xa); // forward distance in [0, span)
    const u64 half = ax.span - ax.span / 2;                 // ceil(span / 2)
    return r >= half ? -static_cast<i64>(ax.span - r) : static_cast<i64>(r);
}

namespace {
WorldSpace& activeSpace() {
    static WorldSpace space;
    return space;
}
} // namespace

const WorldSpace& WorldSpace::active() { return activeSpace(); }
void WorldSpace::setActive(const WorldSpace& space) { activeSpace() = space; }

} // namespace nova
