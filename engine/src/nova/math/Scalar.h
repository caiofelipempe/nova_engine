#pragma once

/// @file
/// @brief Fixed-point numbers of the simulation state: the `Fixed<Int, F, Tag>` template (8 to 64
///        bits, `F` fraction bits), binary angles (Angle) and the engine's dimensions (Length, Speed...).
///
/// Every dimension is its own type (a `Tag`): two of the same format (Restitution and Unit are both
/// Q1.30) still do not mix. Its format follows two rules:
/// - **Precision:** one physics step (1/60 s) of the smallest step of the value must move things by
///   at most 3.8 µm (2⁻¹⁸ m), well below Jolt's 0.1 mm collision tolerance.
/// - **Size:** the smallest integer (8, 16, 32 or 64 bits) that holds that precision over the range
///   the value needs; the bits left over go to the fraction (more precision, for free).
///
/// Products use an `int64` intermediate (32-bit operands at most), so arithmetic is exact, needs no
/// 128 bits and gives the same bits on every platform and compiler.
///
/// | Dimension     | Format       | Bits | Step           | Range            | Why                                          |
/// |---------------|--------------|------|----------------|------------------|----------------------------------------------|
/// | Length        | Q13.18       | 32   | 3.8 µm         | ±8 192 m         | the rule itself; the torus of 16 km           |
/// | Distance      | Q14.17       | 32   | 7.6 µm         | ±16 384 m        | lengths of vectors, up to torus diagonals     |
/// | GlobalLength  | Q45.18       | 64   | 3.8 µm         | ±3.5·10¹³ m      | coordinates in the whole world                |
/// | Speed         | Q14.17       | 32   | 7.6 µm/s       | ±16 384 m/s      | needs 2⁻¹³ m/s (2 µm a step); orbital speeds  |
/// | Acceleration  | Q11.20       | 32   | 9.5·10⁻⁷ m/s²  | ±2 048 m/s²      | needs 2⁻⁷ m/s²; up to ~200 g                  |
/// | Mass          | Q17.14       | 32   | 61 mg          | ±131 t           | vehicles and ships; 10 g bodies within 0.6 %  |
/// | Force         | Q23.8        | 32   | 4 mN           | ±8.4 MN          | 4 mN on 1 kg moves 1.1 µm a step              |
/// | Unit          | Q1.30        | 32   | 9.3·10⁻¹⁰      | ±2               | quaternions, directions: 1 km lever           |
/// | Scale         | Q11.20       | 32   | 9.5·10⁻⁷       | ±2 048           | a step moves a 4 m mesh's corner 3.8 µm       |
/// | Friction      | Q5.10        | 16   | 9.8·10⁻⁴       | ±32              | needs 1.4·10⁻³ (step·g·dt² ≤ 3.8 µm)          |
/// | Restitution   | Q1.30        | 32   | 9.3·10⁻¹⁰      | ±2               | needs 2.3·10⁻⁶ (a 100 m/s bounce, one step)   |
/// | Damping       | Q10.21       | 32   | 4.8·10⁻⁷ /s    | ±1 024 /s        | needs 1.4·10⁻⁴ /s at 100 m/s                  |
/// | GravityFactor | Q5.10        | 16   | 9.8·10⁻⁴       | ±32              | needs 1.4·10⁻³ (step·g·dt² ≤ 3.8 µm)          |
/// | Frequency     | Q11.20       | 32   | 9.5·10⁻⁷ Hz    | ±2 048 Hz        | springs from 0.01 Hz, within 0.01 %           |
/// | DampingRatio  | Q1.14        | 16   | 6.1·10⁻⁵       | ±2               | 0 (bouncy) to 1 (critical), and over          |
/// | Weight        | Q3.28        | 32   | 3.7·10⁻⁹       | ±8               | a step between poses 1 m apart: 3.7 nm        |
/// | WeightRate    | Q10.21       | 32   | 4.8·10⁻⁷ /s    | ±1 024 /s        | a step for a frame: 8·10⁻⁹ of weight          |
/// | Angle         | 32-bit turn  | 32   | 1.46·10⁻⁹ rad  | one turn (wraps) | 1.46 µm at 1 km                               |
/// | AngleSpeed    | Q6.25 turn/s | 32   | 1.9·10⁻⁷ rad/s | ±64 turns/s      | 3.1 µm a step at 1 km                         |
///
/// Angles are binary (Angle): a whole turn is 2³², so they wrap around by themselves, exactly.
/// What is not simulation state (light intensities, playback rates, ratios worked out on the way)
/// is a plain `float` or `double`: fixed point converts from and to them in one call.
///
/// The fpm library does the arithmetic of the 8, 16 and 32-bit formats. 64-bit values (`Fixed<i64, F>`,
/// GlobalLength) only add, subtract, compare and convert: there is no portable 128-bit intermediate
/// for their products, and the simulation does its math in 32 bits relative to the WorldOrigin.
///
/// Overflow of a signed value is undefined behavior in C++: values must stay in range (world
/// positions that wrap around do their wrapping arithmetic in unsigned integers, see GlobalPosition).
///
/// Conversions: a value is quantized where it enters (`Friction(0.5)`, `toFixed<T>(v)`,
/// `Angle::fromDegrees(45)`, and vectors and quaternions straight from glm); it leaves as float only
/// explicitly (`toFloat(v)`, `v.toGlm()`, `a.radians()`), at the GPU, ImGui and the physics engine.

#include "nova/core/Base.h"

#include <fpm/fixed.hpp>
#include <fpm/math.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace nova {

namespace detail {
template <int Bits> struct StorageForBits;
template <> struct StorageForBits<8> { using type = i8; };
template <> struct StorageForBits<16> { using type = i16; };
template <> struct StorageForBits<32> { using type = i32; };
template <> struct StorageForBits<64> { using type = i64; };

template <typename Int> struct WiderInt;
template <> struct WiderInt<i8> { using type = i16; };
template <> struct WiderInt<i16> { using type = i32; };
template <> struct WiderInt<i32> { using type = i64; };

// (p / 2^S) rounded to nearest, ties away from zero (the same on every platform); S < 0 multiplies.
template <int S>
constexpr i64 roundShift(i64 p) {
    if constexpr (S <= 0) {
        return p * (i64(1) << -S);
    } else {
        constexpr i64 half = i64(1) << (S - 1);
        return p >= 0 ? (p + half) >> S : -((-p + half) >> S);
    }
}
// n / d rounded to nearest, ties away from zero.
constexpr i64 roundDiv(i64 n, i64 d) { return (n >= 0) == (d > 0) ? (n + d / 2) / d : (n - d / 2) / d; }
} // namespace detail

/// @brief The signed integer of `Bits` bits (8, 16, 32 or 64).
template <int Bits>
using StorageFor = typename detail::StorageForBits<Bits>::type;

/// @brief The nearest raw value with `fraction` bits to `v` in a `bits`-bit integer (rounded half
///        away from zero; saturates outside the range, NaN gives 0).
inline i64 toRawBits(f64 v, unsigned fraction, unsigned bits) {
    const f64 r = std::round(std::ldexp(v, static_cast<int>(fraction)));
    if (r != r) return 0;
    const f64 limit = std::ldexp(1.0, static_cast<int>(bits) - 1); // 2^(bits−1)
    if (r <= -limit) return bits >= 64 ? std::numeric_limits<i64>::min() : -(i64(1) << (bits - 1));
    if (r >= limit) return bits >= 64 ? std::numeric_limits<i64>::max() : (i64(1) << (bits - 1)) - 1;
    return static_cast<i64>(r);
}
/// @brief The nearest raw value of an `Int` with `fraction` bits to `v` (saturating).
template <typename Int>
inline Int toRaw(f64 v, unsigned fraction) {
    return static_cast<Int>(toRawBits(v, fraction, sizeof(Int) * 8));
}
/// @brief Exact value of a raw number with `fraction` bits (exact below 2⁵³).
inline f64 fromRawValue(i64 raw, unsigned fraction) { return std::ldexp(static_cast<f64>(raw), -static_cast<int>(fraction)); }

/// @brief A fixed-point number: an `Int` (i8, i16, i32 or i64) with `F` fraction bits, zero by
///        default, that takes plain numbers.
///
/// A number becomes fixed point implicitly (`mass = 2.5f`, `limit = -45`), rounded to the nearest
/// step; arithmetic and comparisons with numbers convert them the same way, so `mass * 2` and
/// `friction > 0.5f` work and stay exact integer math. Dimensions never mix implicitly (Length +
/// Mass does not compile, nor Unit + Restitution of the same format: use mul<R>(), convert<R>());
/// back to float only explicitly (toFloat()).
///
/// `Tag` makes a dimension its own type (void: a plain format, `Q<I, F>`). The 8, 16 and 32-bit
/// formats are an `fpm::fixed` underneath (fpm works on their `base()`: `fpm::sqrt(x.base())`; its
/// results come back explicitly, `Length(fpm::sqrt(x.base()))`).
template <typename Int, unsigned F, typename Tag = void>
class Fixed : protected fpm::fixed<Int, typename detail::WiderInt<Int>::type, F> {
    // Protected: fpm's own operators take any fpm::fixed, so two dimensions of the same format
    // (Unit and Restitution) would add through them. fpm functions get base() explicitly.
public:
    using Base = fpm::fixed<Int, typename detail::WiderInt<Int>::type, F>; ///< The fpm type.
    using Base::raw_value;
    using Storage = Int;                                                    ///< The raw integer.
    using Dimension = Tag;                                                  ///< What it measures.
    static constexpr unsigned kFraction = F;                                ///< Fraction bits.

    /// @brief Zero.
    constexpr Fixed() noexcept : Base(Base::from_raw_value(0)) {}
    /// @brief From the fpm type (results of fpm functions; explicit: it carries no dimension).
    constexpr explicit Fixed(Base v) noexcept : Base(v) {}
    /// @brief The nearest value to a number (saturates outside the range).
    template <typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
    Fixed(T v) noexcept : Base(Base::from_raw_value(toRaw<Int>(static_cast<f64>(v), F))) {}

    /// @brief A value from its raw integer.
    static constexpr Fixed from_raw_value(Int raw) noexcept { return Fixed(Base::from_raw_value(raw)); }
    /// @brief The fpm value (for fpm's functions: `Length(fpm::sqrt(x.base()))`).
    constexpr Base base() const noexcept { return static_cast<const Base&>(*this); }
    /// @brief The value as a number (explicit: `static_cast<double>(x)`).
    template <typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
    constexpr explicit operator T() const noexcept { return static_cast<T>(base()); }

    friend constexpr Fixed operator-(Fixed a) noexcept { return Fixed(-a.base()); }
    friend constexpr Fixed operator+(Fixed a, Fixed b) noexcept { return Fixed(a.base() + b.base()); }
    friend constexpr Fixed operator-(Fixed a, Fixed b) noexcept { return Fixed(a.base() - b.base()); }
    friend constexpr Fixed operator*(Fixed a, Fixed b) noexcept { return Fixed(a.base() * b.base()); }
    friend constexpr Fixed operator/(Fixed a, Fixed b) noexcept { return Fixed(a.base() / b.base()); }
    friend constexpr bool operator==(Fixed a, Fixed b) noexcept { return a.raw_value() == b.raw_value(); }
    friend constexpr bool operator<(Fixed a, Fixed b) noexcept { return a.raw_value() < b.raw_value(); }
    friend constexpr bool operator>(Fixed a, Fixed b) noexcept { return a.raw_value() > b.raw_value(); }
    friend constexpr bool operator<=(Fixed a, Fixed b) noexcept { return a.raw_value() <= b.raw_value(); }
    friend constexpr bool operator>=(Fixed a, Fixed b) noexcept { return a.raw_value() >= b.raw_value(); }

#define NOVA_FIXED_MIXED(op, R)                                                                              \
    template <typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>                                \
    friend R operator op(Fixed a, T b) noexcept { return a op Fixed(b); }                                    \
    template <typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>                                \
    friend R operator op(T a, Fixed b) noexcept { return Fixed(a) op b; }
    NOVA_FIXED_MIXED(+, Fixed)
    NOVA_FIXED_MIXED(-, Fixed)
    NOVA_FIXED_MIXED(*, Fixed)
    NOVA_FIXED_MIXED(/, Fixed)
    NOVA_FIXED_MIXED(==, bool)
    NOVA_FIXED_MIXED(<, bool)
    NOVA_FIXED_MIXED(>, bool)
    NOVA_FIXED_MIXED(<=, bool)
    NOVA_FIXED_MIXED(>=, bool)
#undef NOVA_FIXED_MIXED

    Fixed& operator+=(Fixed o) noexcept { return *this = *this + o; }
    Fixed& operator-=(Fixed o) noexcept { return *this = *this - o; }
    Fixed& operator*=(Fixed o) noexcept { return *this = *this * o; }
    Fixed& operator/=(Fixed o) noexcept { return *this = *this / o; }
};

/// @brief A 64-bit fixed-point number (world coordinates): adds, subtracts, compares and converts.
///
/// No `*` or `/`: their exact product needs 128 bits, which C++ has no portable type for. Convert
/// to a 32-bit format near the WorldOrigin first (`convert<Length>(a - b)`), or scale by integers.
template <unsigned F, typename Tag>
class Fixed<i64, F, Tag> {
    static_assert(F < 63, "Fraction must leave room for the sign");

public:
    using Storage = i64;                     ///< The raw integer.
    using Dimension = Tag;                   ///< What it measures.
    static constexpr unsigned kFraction = F; ///< Fraction bits.

    /// @brief Zero.
    constexpr Fixed() noexcept = default;
    /// @brief The nearest value to a number (saturates outside the range).
    template <typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
    Fixed(T v) noexcept : m_raw(toRaw<i64>(static_cast<f64>(v), F)) {}

    /// @brief A value from its raw integer.
    static constexpr Fixed from_raw_value(i64 raw) noexcept {
        Fixed f;
        f.m_raw = raw;
        return f;
    }
    /// @brief The raw integer.
    constexpr i64 raw_value() const noexcept { return m_raw; }

    friend constexpr Fixed operator-(Fixed a) noexcept { return from_raw_value(-a.m_raw); }
    friend constexpr Fixed operator+(Fixed a, Fixed b) noexcept { return from_raw_value(a.m_raw + b.m_raw); }
    friend constexpr Fixed operator-(Fixed a, Fixed b) noexcept { return from_raw_value(a.m_raw - b.m_raw); }
    /// @brief Integer multiple (exact).
    friend constexpr Fixed operator*(Fixed a, i64 n) noexcept { return from_raw_value(a.m_raw * n); }
    friend constexpr Fixed operator*(i64 n, Fixed a) noexcept { return from_raw_value(a.m_raw * n); }
    /// @brief No fractional multiples: the fraction would be dropped (convert to 32 bits first).
    template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
    friend Fixed operator*(Fixed, T) = delete;
    template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
    friend Fixed operator*(T, Fixed) = delete;
    friend constexpr bool operator==(Fixed a, Fixed b) noexcept { return a.m_raw == b.m_raw; }
    friend constexpr bool operator<(Fixed a, Fixed b) noexcept { return a.m_raw < b.m_raw; }
    friend constexpr bool operator>(Fixed a, Fixed b) noexcept { return a.m_raw > b.m_raw; }
    friend constexpr bool operator<=(Fixed a, Fixed b) noexcept { return a.m_raw <= b.m_raw; }
    friend constexpr bool operator>=(Fixed a, Fixed b) noexcept { return a.m_raw >= b.m_raw; }

    Fixed& operator+=(Fixed o) noexcept { return *this = *this + o; }
    Fixed& operator-=(Fixed o) noexcept { return *this = *this - o; }

private:
    i64 m_raw = 0;
};

/// @brief The format with `I` integer bits and `F` fraction bits (plus the sign): `Q<13, 18>` is a
///        32-bit Q13.18. `1 + I + F` must be 8, 16, 32 or 64.
template <int I, int F>
using Q = Fixed<StorageFor<1 + I + F>, static_cast<unsigned>(F)>;
/// @brief A dimension: the format Q`I`.`F` as its own type, told apart by `Tag`.
template <int I, int F, typename Tag>
using Dimension = Fixed<StorageFor<1 + I + F>, static_cast<unsigned>(F), Tag>;

/// @brief Is `T` a Fixed type?
template <typename T>
struct IsFixed : std::false_type {};
template <typename Int, unsigned F, typename Tag>
struct IsFixed<Fixed<Int, F, Tag>> : std::true_type {};
/// @brief Is `T` a Fixed type?
template <typename T>
constexpr bool isFixed = IsFixed<T>::value;

/// @brief Fraction bits of a Fixed type (or of its fpm base).
template <typename T>
struct FixedTraits;
template <typename Int, unsigned F, typename Tag>
struct FixedTraits<Fixed<Int, F, Tag>> {
    static constexpr unsigned fraction = F; ///< Fraction bits.
    using Storage = Int;                    ///< Raw integer.
};
template <typename Int, typename Wide, unsigned F>
struct FixedTraits<fpm::fixed<Int, Wide, F>> {
    static constexpr unsigned fraction = F; ///< Fraction bits.
    using Storage = Int;                    ///< Raw integer.
};
/// @brief Fraction bits of a Fixed type.
template <typename T>
constexpr unsigned fractionBits = FixedTraits<T>::fraction;
/// @brief Raw integer of a Fixed type.
template <typename T>
using StorageOf = typename FixedTraits<T>::Storage;
/// @brief Bits of the raw integer of a Fixed type.
template <typename T>
constexpr unsigned storageBits = sizeof(StorageOf<T>) * 8;

/// @brief A Fixed value from its raw integer.
template <typename T>
constexpr T fromRaw(i64 raw) { return T::from_raw_value(static_cast<StorageOf<T>>(raw)); }
/// @brief Zero of a Fixed type.
template <typename T>
constexpr T zero() { return T::from_raw_value(0); }
/// @brief The nearest value of `T` to `v` (rounded, not truncated; saturates outside the range).
template <typename T>
inline T toFixed(f64 v) {
    return T::from_raw_value(toRaw<StorageOf<T>>(v, fractionBits<T>));
}
/// @brief Exact value as a double (exact below 2⁵³ raw steps).
template <typename T, std::enable_if_t<isFixed<T>, int> = 0>
inline f64 toDouble(T v) { return fromRawValue(static_cast<i64>(v.raw_value()), fractionBits<T>); }
/// @brief Nearest float (for the GPU, ImGui and the physics engine).
template <typename T, std::enable_if_t<isFixed<T>, int> = 0>
inline f32 toFloat(T v) { return static_cast<f32>(toDouble(v)); }

/// @brief Product of two Fixed values of any formats, in format `R` (exact 64-bit product, rounded).
template <typename R, typename A, typename B>
constexpr R mul(A a, B b) {
    static_assert(storageBits<A> + storageBits<B> <= 64, "mul: the product needs more than 64 bits");
    const i64 p = static_cast<i64>(a.raw_value()) * static_cast<i64>(b.raw_value());
    constexpr int shift = int(fractionBits<A>) + int(fractionBits<B>) - int(fractionBits<R>);
    return R::from_raw_value(static_cast<StorageOf<R>>(detail::roundShift<shift>(p)));
}
/// @brief Quotient `a / b` in format `R` (exact 64-bit division, rounded).
template <typename R, typename A, typename B>
constexpr R div(A a, B b) {
    static_assert(storageBits<A> <= 32 && storageBits<B> <= 32, "div: 64-bit operands need 128 bits");
    // a / b in R = (aRaw · 2^(FR + FB − FA)) / bRaw.
    constexpr int shift = int(fractionBits<R>) + int(fractionBits<B>) - int(fractionBits<A>);
    i64 n = a.raw_value();
    if constexpr (shift >= 0) n *= i64(1) << shift;
    else n >>= -shift;
    return R::from_raw_value(static_cast<StorageOf<R>>(detail::roundDiv(n, b.raw_value())));
}
/// @brief The same value in another format, of any size (rounded when it loses bits; the value
///        must fit the target).
template <typename R, typename A>
constexpr R convert(A a) {
    constexpr int shift = int(fractionBits<A>) - int(fractionBits<R>);
    return R::from_raw_value(static_cast<StorageOf<R>>(detail::roundShift<shift>(static_cast<i64>(a.raw_value()))));
}

/// @brief Integer square root: floor(√v), exact for every 64-bit value.
constexpr u64 isqrt(u64 v) {
    u64 r = 0, bit = u64(1) << 62;
    while (bit > v) bit >>= 2;
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/// @brief Absolute value.
template <typename T, std::enable_if_t<isFixed<T>, int> = 0>
constexpr T abs(T v) { return v.raw_value() < 0 ? -v : v; }
/// @brief The smaller value.
template <typename T, std::enable_if_t<isFixed<T>, int> = 0>
constexpr T min(T a, T b) { return b < a ? b : a; }
/// @brief The larger value.
template <typename T, std::enable_if_t<isFixed<T>, int> = 0>
constexpr T max(T a, T b) { return a < b ? b : a; }
/// @brief `v` limited to [lo, hi].
template <typename T, std::enable_if_t<isFixed<T>, int> = 0>
constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : hi < v ? hi : v; }

// ------------------------------------------------------------------ angles --

/// @brief A binary angle: an unsigned 32-bit fraction of a turn (2³² = 2π), so it wraps around by
///        itself, exactly (no remainder, no overflow).
///
/// The same bits read two ways: unsigned, in [0, 2π) (turns(), radiansPositive()), or signed, in
/// [−π, π) (signedRaw(), radians(), degrees()). Comparisons use the signed reading, which is what
/// limits want (`hinge >= Angle::fromDegrees(-45)`); a difference of angles is the shortest turn.
/// @code
/// nova::Angle yaw = nova::Angle::fromDegrees(350);
/// yaw += nova::Angle::fromDegrees(20);   // 10°, exactly
/// float r = yaw.radiansF();              // for glm / the GPU
/// @endcode
class Angle {
public:
    static constexpr f64 kTurn = 4294967296.0; ///< Raw steps in a turn (2³²).

    /// @brief Zero.
    constexpr Angle() noexcept = default;
    /// @brief From the raw fraction of a turn.
    static constexpr Angle fromRaw(u32 raw) noexcept {
        Angle a;
        a.m_raw = raw;
        return a;
    }
    /// @brief Same as fromRaw() (the interface of Fixed, for generic code).
    static constexpr Angle from_raw_value(u32 raw) noexcept { return fromRaw(raw); }
    /// @brief The nearest angle to `turns` turns (any value: whole turns drop out).
    static Angle fromTurns(f64 turns) noexcept {
        if (!(turns == turns) || std::isinf(turns)) return {};
        const f64 frac = turns - std::floor(turns);                       // [0, 1)
        return fromRaw(static_cast<u32>(static_cast<u64>(std::llround(frac * kTurn)))); // 2³² wraps to 0
    }
    /// @brief The nearest angle to `radians`.
    static Angle fromRadians(f64 radians) noexcept { return fromTurns(radians / (2.0 * kPi)); }
    /// @brief The nearest angle to `degrees`.
    static Angle fromDegrees(f64 degrees) noexcept { return fromTurns(degrees / 360.0); }
    /// @brief Half a turn (π).
    static constexpr Angle half() noexcept { return fromRaw(u32(1) << 31); }
    /// @brief A quarter of a turn (π/2).
    static constexpr Angle quarter() noexcept { return fromRaw(u32(1) << 30); }

    /// @brief The raw fraction of a turn (unsigned reading).
    constexpr u32 raw() const noexcept { return m_raw; }
    /// @brief Same as raw() (the interface of Fixed, for generic code).
    constexpr u32 raw_value() const noexcept { return m_raw; }
    /// @brief The raw value read as signed: [−2³¹, 2³¹) = [−π, π).
    constexpr i32 signedRaw() const noexcept { return static_cast<i32>(m_raw); }

    /// @brief Turns in [0, 1).
    constexpr f64 turns() const noexcept { return static_cast<f64>(m_raw) / kTurn; }
    /// @brief Turns in [−0.5, 0.5).
    constexpr f64 signedTurns() const noexcept { return static_cast<f64>(signedRaw()) / kTurn; }
    /// @brief Radians in [−π, π).
    constexpr f64 radians() const noexcept { return signedTurns() * (2.0 * kPi); }
    /// @brief Radians in [0, 2π).
    constexpr f64 radiansPositive() const noexcept { return turns() * (2.0 * kPi); }
    /// @brief Degrees in [−180, 180).
    constexpr f64 degrees() const noexcept { return signedTurns() * 360.0; }
    /// @brief Degrees in [0, 360).
    constexpr f64 degreesPositive() const noexcept { return turns() * 360.0; }
    /// @brief Radians in [−π, π) as float (glm, the GPU, the physics engine).
    constexpr f32 radiansF() const noexcept { return static_cast<f32>(radians()); }

    /// @brief The size of the angle, |signed reading| (π stays π).
    constexpr Angle abs() const noexcept { return signedRaw() < 0 ? -*this : *this; }

    friend constexpr Angle operator-(Angle a) noexcept { return fromRaw(0u - a.m_raw); }
    friend constexpr Angle operator+(Angle a, Angle b) noexcept { return fromRaw(a.m_raw + b.m_raw); }
    friend constexpr Angle operator-(Angle a, Angle b) noexcept { return fromRaw(a.m_raw - b.m_raw); }
    /// @brief Integer multiple (wraps).
    friend constexpr Angle operator*(Angle a, i32 n) noexcept { return fromRaw(a.m_raw * static_cast<u32>(n)); }
    friend constexpr Angle operator*(i32 n, Angle a) noexcept { return a * n; }
    /// @brief No fractional multiples: the fraction would be dropped (use scaled(), e.g. with a Unit).
    template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
    friend Angle operator*(Angle, T) = delete;
    template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
    friend Angle operator*(T, Angle) = delete;
    Angle& operator+=(Angle o) noexcept { return *this = *this + o; }
    Angle& operator-=(Angle o) noexcept { return *this = *this - o; }

    friend constexpr bool operator==(Angle a, Angle b) noexcept { return a.m_raw == b.m_raw; }
    friend constexpr bool operator!=(Angle a, Angle b) noexcept { return a.m_raw != b.m_raw; }
    /// @name Comparisons of the signed readings ([−π, π)).
    /// @{
    friend constexpr bool operator<(Angle a, Angle b) noexcept { return a.signedRaw() < b.signedRaw(); }
    friend constexpr bool operator>(Angle a, Angle b) noexcept { return a.signedRaw() > b.signedRaw(); }
    friend constexpr bool operator<=(Angle a, Angle b) noexcept { return a.signedRaw() <= b.signedRaw(); }
    friend constexpr bool operator>=(Angle a, Angle b) noexcept { return a.signedRaw() >= b.signedRaw(); }
    /// @}

    /// @brief This angle times `s` (any Fixed format up to 32 bits; signed reading, rounded, wraps).
    template <typename S>
    constexpr Angle scaled(S s) const noexcept {
        const i64 p = static_cast<i64>(signedRaw()) * static_cast<i64>(s.raw_value());
        return fromRaw(static_cast<u32>(static_cast<u64>(detail::roundShift<int(fractionBits<S>)>(p))));
    }

private:
    static constexpr f64 kPi = 3.14159265358979323846;
    u32 m_raw = 0;
};

/// @brief A compact binary angle (2¹⁶ = one turn, step 9.6·10⁻⁵ rad): for storage and the network,
///        not for simulation (convert to Angle to compute).
class Angle16 {
public:
    constexpr Angle16() noexcept = default;
    /// @brief The nearest compact angle to `a` (rounded).
    constexpr Angle16(Angle a) noexcept : m_raw(static_cast<u16>((a.raw() + 0x8000u) >> 16)) {}
    /// @brief From the raw fraction of a turn.
    static constexpr Angle16 fromRaw(u16 raw) noexcept {
        Angle16 a;
        a.m_raw = raw;
        return a;
    }
    /// @brief The raw fraction of a turn.
    constexpr u16 raw() const noexcept { return m_raw; }
    /// @brief The exact same angle in full precision.
    constexpr Angle toAngle() const noexcept { return Angle::fromRaw(static_cast<u32>(m_raw) << 16); }
    constexpr operator Angle() const noexcept { return toAngle(); }
    friend constexpr bool operator==(Angle16 a, Angle16 b) noexcept { return a.m_raw == b.m_raw; }

private:
    u16 m_raw = 0;
};

/// @brief Exact value of an angle in radians, in [−π, π) (the generic interface, for vectors of angles).
inline f64 toDouble(Angle a) { return a.radians(); }
/// @brief An angle in radians, as float.
inline f32 toFloat(Angle a) { return a.radiansF(); }
/// @brief The nearest angle to `radians` (the generic interface, for vectors of angles).
template <>
inline Angle toFixed<Angle>(f64 radians) { return Angle::fromRadians(radians); }
/// @brief Zero angle.
template <>
constexpr Angle zero<Angle>() { return Angle(); }

// -------------------------------------------------------------- dimensions --

// See the table at the top of the file for each format.
using Length = Dimension<13, 18, struct LengthTag>;              ///< m: positions near the origin, sizes, offsets.
using Distance = Dimension<14, 17, struct DistanceTag>;          ///< m: lengths of vectors, up to a torus diagonal.
using GlobalLength = Dimension<45, 18, struct GlobalLengthTag>;  ///< m in the whole world (64 bits, the step of Length).
using Speed = Dimension<14, 17, struct SpeedTag>;                ///< m/s: velocity.
using Acceleration = Dimension<11, 20, struct AccelerationTag>;  ///< m/s²: acceleration, gravity.
using Mass = Dimension<17, 14, struct MassTag>;                  ///< kg.
using Force = Dimension<23, 8, struct ForceTag>;                 ///< N, N·s or N·m.
using Unit = Dimension<1, 30, struct UnitTag>;                   ///< Quaternion components, unit directions, |u| < 2.
using Scale = Dimension<11, 20, struct ScaleTag>;                ///< Size factor of an object (per axis).
using Friction = Dimension<5, 10, struct FrictionTag>;           ///< Friction coefficient (16 bits).
using Restitution = Dimension<1, 30, struct RestitutionTag>;     ///< Bounciness, 0..1.
using Damping = Dimension<10, 21, struct DampingTag>;            ///< 1/s: velocity damping.
using GravityFactor = Dimension<5, 10, struct GravityFactorTag>; ///< Gravity multiplier (16 bits).
using Frequency = Dimension<11, 20, struct FrequencyTag>;        ///< Hz.
using DampingRatio = Dimension<1, 14, struct DampingRatioTag>;   ///< Spring damping ratio (16 bits).
using Weight = Dimension<3, 28, struct WeightTag>;               ///< Share of a mix (animation blending).
using WeightRate = Dimension<10, 21, struct WeightRateTag>;      ///< Weight change per second (fades).

/// @brief An angular speed: turns per second in Q6.25 (±64 turns/s ≈ 400 rad/s, step 1.9·10⁻⁷ rad/s:
///        one physics step of it turns a point 1 km away by 3.1 µm, within the rule).
///
/// Its own type, built only with a unit (`AngleSpeed::fromDegreesPerSecond(90)`): a plain number
/// would be ambiguous between turns, radians and degrees. angleTurned() integrates it exactly.
class AngleSpeed {
public:
    using Turns = Q<6, 25>; ///< The raw format: turns per second.

    /// @brief Zero.
    constexpr AngleSpeed() noexcept = default;
    /// @brief From turns per second (any Fixed value of the raw format).
    constexpr explicit AngleSpeed(Turns turnsPerSecond) noexcept : m_turns(turnsPerSecond) {}
    /// @brief From the raw integer (2⁻²⁵ turn/s steps).
    static constexpr AngleSpeed fromRaw(i32 raw) noexcept { return AngleSpeed(Turns::from_raw_value(raw)); }
    /// @brief Same as fromRaw() (the interface of Fixed, for generic code).
    static constexpr AngleSpeed from_raw_value(i32 raw) noexcept { return fromRaw(raw); }
    /// @brief The nearest speed to `turns` per second.
    static AngleSpeed fromTurnsPerSecond(f64 turns) noexcept { return AngleSpeed(toFixed<Turns>(turns)); }
    /// @brief The nearest speed to `radians` per second.
    static AngleSpeed fromRadiansPerSecond(f64 radians) noexcept { return fromTurnsPerSecond(radians / (2.0 * 3.14159265358979323846)); }
    /// @brief The nearest speed to `degrees` per second.
    static AngleSpeed fromDegreesPerSecond(f64 degrees) noexcept { return fromTurnsPerSecond(degrees / 360.0); }

    /// @brief The raw integer.
    constexpr i32 raw() const noexcept { return m_turns.raw_value(); }
    /// @brief Same as raw() (the interface of Fixed, for generic code).
    constexpr i32 raw_value() const noexcept { return raw(); }
    /// @brief In turns per second (exact, fixed point).
    constexpr Turns turns() const noexcept { return m_turns; }
    /// @brief In turns per second.
    f64 turnsPerSecond() const noexcept { return toDouble(m_turns); }
    /// @brief In radians per second.
    f64 radiansPerSecond() const noexcept { return turnsPerSecond() * (2.0 * 3.14159265358979323846); }
    /// @brief In degrees per second.
    f64 degreesPerSecond() const noexcept { return turnsPerSecond() * 360.0; }
    /// @brief In radians per second, as float (the physics engine).
    f32 radiansPerSecondF() const noexcept { return static_cast<f32>(radiansPerSecond()); }

    friend constexpr AngleSpeed operator-(AngleSpeed a) noexcept { return AngleSpeed(-a.m_turns); }
    friend constexpr AngleSpeed operator+(AngleSpeed a, AngleSpeed b) noexcept { return AngleSpeed(a.m_turns + b.m_turns); }
    friend constexpr AngleSpeed operator-(AngleSpeed a, AngleSpeed b) noexcept { return AngleSpeed(a.m_turns - b.m_turns); }
    AngleSpeed& operator+=(AngleSpeed o) noexcept { return *this = *this + o; }
    AngleSpeed& operator-=(AngleSpeed o) noexcept { return *this = *this - o; }
    friend constexpr bool operator==(AngleSpeed a, AngleSpeed b) noexcept { return a.raw() == b.raw(); }
    friend constexpr bool operator<(AngleSpeed a, AngleSpeed b) noexcept { return a.raw() < b.raw(); }
    friend constexpr bool operator>(AngleSpeed a, AngleSpeed b) noexcept { return a.raw() > b.raw(); }
    friend constexpr bool operator<=(AngleSpeed a, AngleSpeed b) noexcept { return a.raw() <= b.raw(); }
    friend constexpr bool operator>=(AngleSpeed a, AngleSpeed b) noexcept { return a.raw() >= b.raw(); }
    /// @brief This speed times `s` (any Fixed format up to 32 bits, rounded).
    template <typename S>
    constexpr AngleSpeed scaled(S s) const noexcept { return AngleSpeed(mul<Turns>(m_turns, s)); }

private:
    Turns m_turns;
};

/// @brief An angular speed in radians per second (the generic interface, for vectors of them).
inline f64 toDouble(AngleSpeed s) { return s.radiansPerSecond(); }
/// @brief An angular speed in radians per second, as float.
inline f32 toFloat(AngleSpeed s) { return s.radiansPerSecondF(); }
/// @brief The nearest angular speed to `radiansPerSecond` (the generic interface, for vectors).
template <>
inline AngleSpeed toFixed<AngleSpeed>(f64 radiansPerSecond) { return AngleSpeed::fromRadiansPerSecond(radiansPerSecond); }
/// @brief Zero angular speed.
template <>
constexpr AngleSpeed zero<AngleSpeed>() { return AngleSpeed(); }

/// @brief Unit literals for the dimensions: `2.5_m`, `90_deg`, `9.81_mps2`, `120_degps`...
///
/// An inline namespace: `using namespace nova;` brings them, or `using namespace nova::literals;`
/// alone. Each one is the nearest value of its dimension (rounded like any number that enters).
/// @code
/// using namespace nova::literals;
/// rb.mass = 2_kg;
/// joint.angleMax = 80_deg;
/// rotator.speed = 0.5_turnps;
/// @endcode
inline namespace literals {
// `operator"" suffix` with a space: `""suffix` would be a single token, and the macro parameter would
// not be replaced in it.
#define NOVA_FIXED_LITERAL(suffix, Type, expr)                                                                        \
    inline Type operator"" suffix(long double v) { const f64 x = static_cast<f64>(v); return expr; }                  \
    inline Type operator"" suffix(unsigned long long v) { const f64 x = static_cast<f64>(v); return expr; }
NOVA_FIXED_LITERAL(_m, Length, Length(x))                         ///< Meters.
NOVA_FIXED_LITERAL(_cm, Length, Length(x * 0.01))                 ///< Centimeters.
NOVA_FIXED_LITERAL(_mm, Length, Length(x * 0.001))                ///< Millimeters.
NOVA_FIXED_LITERAL(_km, Distance, Distance(x * 1000.0))           ///< Kilometers (a Distance: up to 16 km).
NOVA_FIXED_LITERAL(_mps, Speed, Speed(x))                         ///< Meters per second.
NOVA_FIXED_LITERAL(_kmh, Speed, Speed(x / 3.6))                   ///< Kilometers per hour.
NOVA_FIXED_LITERAL(_mps2, Acceleration, Acceleration(x))          ///< Meters per second squared.
NOVA_FIXED_LITERAL(_kg, Mass, Mass(x))                            ///< Kilograms.
NOVA_FIXED_LITERAL(_N, Force, Force(x))                           ///< Newtons (also N·s, N·m).
NOVA_FIXED_LITERAL(_Hz, Frequency, Frequency(x))                  ///< Hertz.
NOVA_FIXED_LITERAL(_deg, Angle, Angle::fromDegrees(x))            ///< Degrees.
NOVA_FIXED_LITERAL(_rad, Angle, Angle::fromRadians(x))            ///< Radians.
NOVA_FIXED_LITERAL(_turn, Angle, Angle::fromTurns(x))             ///< Turns.
NOVA_FIXED_LITERAL(_degps, AngleSpeed, AngleSpeed::fromDegreesPerSecond(x)) ///< Degrees per second.
NOVA_FIXED_LITERAL(_radps, AngleSpeed, AngleSpeed::fromRadiansPerSecond(x)) ///< Radians per second.
NOVA_FIXED_LITERAL(_turnps, AngleSpeed, AngleSpeed::fromTurnsPerSecond(x))  ///< Turns per second.
NOVA_FIXED_LITERAL(_rpm, AngleSpeed, AngleSpeed::fromTurnsPerSecond(x / 60.0)) ///< Turns per minute.
#undef NOVA_FIXED_LITERAL
} // namespace literals

/// @brief The turn made at angular speed `s` in `flicks` (1/705 600 000 s, see Time.h): exact up to
///        one rounding (to the nearest step, ties away from zero), whole turns drop out.
constexpr Angle angleTurned(AngleSpeed s, i64 flicks) {
    constexpr i64 kFlicks = 705'600'000; // = kFlicksPerSecond (Time.h)
    constexpr int kToTurn = 32 - int(fractionBits<AngleSpeed::Turns>); // AngleSpeed raw → Angle raw: ×2⁷
    const i64 raw = s.raw();
    const bool negative = (raw < 0) != (flicks < 0);
    const u64 speed = static_cast<u64>(raw < 0 ? -raw : raw); // ≤ 2³¹
    const u64 t = flicks < 0 ? 0 - static_cast<u64>(flicks) : static_cast<u64>(flicks);
    const u64 seconds = t / kFlicks, rest = t % kFlicks; // rest < 2³⁰
    // Whole seconds: speed · 2⁷ steps each (wraps mod 2³²: whole turns drop out).
    u64 steps = (speed << kToTurn) * seconds;
    // The rest: speed · rest · 2⁷ / flicksPerSecond, rounded (speed · rest < 2⁶¹).
    const u64 x = speed * rest;
    const u64 q = x / kFlicks, r = x % kFlicks;
    steps += (q << kToTurn) + (((r << kToTurn) + kFlicks / 2) / kFlicks);
    const u32 turned = static_cast<u32>(steps);
    return Angle::fromRaw(negative ? 0u - turned : turned);
}

} // namespace nova
