// Fixed-point scalars, binary angles and vectors of the simulation (nova/math/Scalar.h, Vec3.h, lib fpm).
// Run: ctest --preset ... or NovaScalarTests.exe
#include <nova/math/Quat.h>
#include <nova/math/Time.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <type_traits>
#include <vector>

using namespace nova;

using Scalar = Q<15, 16>; // a plain format (no dimension), for the tests

// What compiles and what does not, as concepts (a requires-expression outside a template is an error).
template <typename A, typename B>
concept Addable = requires(A a, B b) { a + b; };
template <typename A, typename B>
concept Multipliable = requires(A a, B b) { a * b; };

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAILED: %s (%s:%d)\n", expr, file, line);
    }
}
#define CHECK(x) check((x), #x, __FILE__, __LINE__)
void checkNear(double a, double b, double eps, const char* expr, const char* file, int line) {
    check(std::abs(a - b) <= eps, expr, file, line);
    if (std::abs(a - b) > eps) std::printf("    got %.12f, expected %.12f (+-%g)\n", a, b, eps);
}
#define CHECK_NEAR(a, b, eps) checkNear(static_cast<double>(a), static_cast<double>(b), static_cast<double>(eps), #a " ~= " #b, __FILE__, __LINE__)

void formats() {
    // Steps and ranges are what the table says.
    CHECK(toDouble(fromRaw<Length>(1)) == std::ldexp(1.0, -18));                 // 3.8 µm
    CHECK_NEAR(toDouble(fromRaw<Length>(std::numeric_limits<i32>::max())), 8192.0, 1e-5);
    CHECK_NEAR(toDouble(fromRaw<Distance>(std::numeric_limits<i32>::max())), 16384.0, 1e-4);
    CHECK(toDouble(fromRaw<Speed>(1)) == std::ldexp(1.0, -17));                  // 7.6 µm/s
    CHECK(Angle::fromRaw(1).turns() == std::ldexp(1.0, -32));                     // 1.46·10⁻⁹ rad
    CHECK(AngleSpeed::fromRaw(1).turnsPerSecond() == std::ldexp(1.0, -25));       // turns/s
    CHECK_NEAR(AngleSpeed::fromRaw(std::numeric_limits<i32>::max()).turnsPerSecond(), 64.0, 1e-6);
    CHECK(toDouble(fromRaw<GlobalLength>(1)) == std::ldexp(1.0, -18));           // the world step
    CHECK_NEAR(toDouble(fromRaw<Unit>(std::numeric_limits<i32>::max())), 2.0, 1e-8);
    CHECK(toDouble(fromRaw<Force>(1)) == 1.0 / 256.0);                           // 4 mN
    // The rule: one 1/60 s step of the smallest value of each kind moves ≤ 3.8 µm.
    const f64 step = 1.0 / 60.0, limit = std::ldexp(1.0, -18) * 1.0001;
    CHECK(toDouble(fromRaw<Speed>(1)) * step <= limit);                          // a speed for a step
    CHECK(toDouble(fromRaw<Force>(1)) / 1.0 * step * step <= limit);             // a force on 1 kg
    CHECK(Angle::fromRaw(1).radiansPositive() * 1000.0 <= limit);                // an angle, 1 km from the center
    CHECK(AngleSpeed::fromRaw(1).radiansPerSecond() * step * 1000.0 <= limit);    // an angular speed for a step
    CHECK(toDouble(fromRaw<Unit>(1)) * 2.0 * 1000.0 <= limit);                   // a quaternion step, same lever
}

void conversions() {
    // Rounded to nearest, and exact both ways for representable values.
    CHECK(toFixed<Length>(1.5).raw_value() == 3 << 17);
    CHECK(toDouble(toFixed<Length>(-1234.25)) == -1234.25);
    const f64 tiny = std::ldexp(1.0, -18);
    CHECK(toFixed<Length>(tiny * 0.49).raw_value() == 0 && toFixed<Length>(tiny * 0.51).raw_value() == 1);
    CHECK(toFixed<Length>(-tiny * 0.51).raw_value() == -1);
    CHECK(convert<Distance>(toFixed<Length>(3.0)) == toFixed<Distance>(3.0));
    CHECK(convert<Length>(fromRaw<Distance>(1)).raw_value() == 2);   // finer: exact
    CHECK(convert<Distance>(fromRaw<Length>(3)).raw_value() == 2);   // coarser: 1.5 rounds away from zero
    CHECK(convert<Distance>(fromRaw<Length>(-3)).raw_value() == -2);
    CHECK(zero<Speed>().raw_value() == 0 && LengthVec{}.x.raw_value() == 0 && Quat{}.w.raw_value() == 1 << 30);
}

void products() {
    // Exact 64-bit products and quotients, rounded once, across formats.
    const Speed v = toFixed<Speed>(2.0);
    const Scalar dt = toFixed<Scalar>(0.5);
    CHECK(mul<Length>(v, dt) == toFixed<Length>(1.0));                 // m/s · s = m
    CHECK(mul<Length>(toFixed<Length>(8000.0), toFixed<Scalar>(1.0)) == toFixed<Length>(8000.0));
    CHECK(div<Speed>(toFixed<Length>(3.0), toFixed<Scalar>(2.0)) == toFixed<Speed>(1.5));
    CHECK(div<Unit>(toFixed<Length>(1.0), toFixed<Length>(4.0)) == toFixed<Unit>(0.25));
    CHECK(div<Scalar>(toFixed<Length>(-3.0), toFixed<Length>(2.0)) == toFixed<Scalar>(-1.5));
    // Rounding is symmetric: −(a·b) = (−a)·b, whatever the platform.
    std::mt19937 rng(7);
    std::uniform_int_distribution<i32> any(-(1 << 28), 1 << 28); // a: ±1 km; b: ±4: products stay in range
    bool symmetric = true;
    for (int i = 0; i < 10000; ++i) {
        const Length a = fromRaw<Length>(any(rng));
        const Scalar b = fromRaw<Scalar>(any(rng) >> 12);
        symmetric &= mul<Length>(-a, b).raw_value() == -mul<Length>(a, b).raw_value();
        const f64 exact = toDouble(a) * toDouble(b);
        symmetric &= std::abs(toDouble(mul<Length>(a, b)) - exact) <= std::ldexp(1.0, -19) + 1e-12; // half a step
    }
    CHECK(symmetric);
}

void squareRoots() {
    CHECK(isqrt(0) == 0 && isqrt(1) == 1 && isqrt(15) == 3 && isqrt(16) == 4);
    CHECK(isqrt(~u64(0)) == 0xFFFFFFFFull); // floor(√(2^64 − 1))
    std::mt19937_64 rng(3);
    bool exact = true;
    for (int i = 0; i < 10000; ++i) {
        const u64 v = rng() >> (rng() % 63);
        const u64 r = isqrt(v);
        exact &= r * r <= v && (r + 1) > v / (r + 1); // r = floor(√v)
    }
    CHECK(exact);
}

void vectors() {
    // The worst case of the 16 km torus: a full-range diagonal. No overflow, exact length.
    const Length max = fromRaw<Length>(std::numeric_limits<i32>::max());
    const LengthVec corner{max, max, -max};
    CHECK(corner.lengthSquaredRaw() == 3ull * static_cast<u64>(static_cast<i64>(max.raw_value()) * max.raw_value()));
    CHECK_NEAR(toDouble(corner.length()), 8192.0 * std::sqrt(3.0), 1e-4);
    // Lengths, comparisons, unit vectors.
    const LengthVec v = LengthVec::fromGlm(glm::dvec3(3.0, 4.0, 12.0));
    CHECK(v.length() == toFixed<Distance>(13.0));
    CHECK(v.isShorterThan(toFixed<Length>(13.0001)) && !v.isShorterThan(toFixed<Length>(13.0)));
    const UnitVec n = v.normalized();
    CHECK_NEAR(toDouble(n.x), 3.0 / 13.0, 1e-9);
    CHECK_NEAR(toDouble(n.z), 12.0 / 13.0, 1e-9);
    CHECK(LengthVec{}.normalized() == UnitVec{});
    // Longer than the format of the length: it saturates, never wraps to a negative length.
    CHECK(SpeedVec(16000.0, 16000.0, 0.0).length() == fromRaw<Distance>(std::numeric_limits<i32>::max()));
    CHECK(SpeedVec(16000.0, 16000.0, 0.0).length<Speed>() == fromRaw<Speed>(std::numeric_limits<i32>::max()));
    CHECK(dot<Length>(v, n) == toFixed<Length>(13.0) || std::abs(toDouble(dot<Length>(v, n)) - 13.0) < 1e-5);
    const LengthVec x = LengthVec::fromGlm(glm::dvec3(1, 0, 0)), y = LengthVec::fromGlm(glm::dvec3(0, 1, 0));
    CHECK(cross<Length>(x, y) == LengthVec::fromGlm(glm::dvec3(0, 0, 1)));
    CHECK(v.scaled(toFixed<Scalar>(0.5)) == LengthVec::fromGlm(glm::dvec3(1.5, 2.0, 6.0)));
    CHECK((v - v) == LengthVec{} && (v + -v) == LengthVec{});
}

void quaternions() {
    // Matches glm within the format's precision.
    const glm::quat g = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1, 2, 3)));
    const Quat q = Quat::fromGlm(g);
    const glm::vec3 p(1.25f, -2.0f, 3.5f);
    const glm::vec3 expected = g * p;
    const glm::dvec3 got = q.rotate(LengthVec::fromGlm(p)).toDouble();
    CHECK(glm::length(glm::dvec3(expected) - got) < 1e-5);
    // From axis and angle (fpm's deterministic sin / cos).
    const Quat a = Quat::fromAxisAngle(LengthVec::fromGlm(glm::vec3(0, 0, 1)).normalized(), toFixed<Angle>(glm::half_pi<f64>()));
    const glm::dvec3 turned = a.rotate(LengthVec::fromGlm(glm::vec3(2, 0, 0))).toDouble();
    CHECK(glm::length(turned - glm::dvec3(0, 2, 0)) < 1e-5);
    // Composition order as glm, inverse, renormalization.
    const Quat b = Quat::fromGlm(glm::angleAxis(-1.1f, glm::vec3(0, 1, 0)));
    const glm::dvec3 ab = (a * b).rotate(LengthVec::fromGlm(p)).toDouble();
    CHECK(glm::length(ab - glm::dvec3((a.toGlm() * b.toGlm()) * p)) < 1e-5);
    const glm::dvec3 back = q.conjugate().rotate(q.rotate(LengthVec::fromGlm(p))).toDouble();
    CHECK(glm::length(back - glm::dvec3(p)) < 1e-5);
    // A long chain of small turns stays unit and close to the float reference.
    Quat acc;
    glm::dquat ref(1, 0, 0, 0);
    const Quat stepq = Quat::fromGlm(glm::angleAxis(0.01f, glm::normalize(glm::vec3(0.3f, 1, -0.2f))));
    for (int i = 0; i < 10000; ++i) {
        acc = (stepq * acc).normalized();
        ref = glm::dquat(stepq.toGlm()) * ref;
    }
    const f64 len = std::sqrt(toDouble(acc.w) * toDouble(acc.w) + toDouble(acc.x) * toDouble(acc.x) +
                              toDouble(acc.y) * toDouble(acc.y) + toDouble(acc.z) * toDouble(acc.z));
    CHECK_NEAR(len, 1.0, 1e-8);
    CHECK(std::abs(glm::dot(glm::dquat(acc.toGlm()), glm::normalize(ref))) > 1.0 - 1e-6);
    // Rotating the largest vectors does not overflow (the length is kept).
    const Length big = fromRaw<Length>(std::numeric_limits<i32>::max()); // just under 8192 m
    const LengthVec far{big, zero<Length>(), zero<Length>()};
    CHECK_NEAR(toDouble(q.rotate(far).length()), toDouble(big), 1e-3);
}

void determinism() {
    // Integer arithmetic only: two runs give the same bits, and the result does not depend on
    // floating-point settings (the reference below is computed from raw integers by hand).
    auto run = [] {
        LengthVec p = LengthVec::fromGlm(glm::dvec3(10.0, 20.0, -5.0));
        SpeedVec v = SpeedVec::fromGlm(glm::dvec3(1.5, 0.0, -0.25));
        const Speed g = toFixed<Speed>(-9.81);
        const Scalar dt = fromRaw<Scalar>(1092); // ≈ 1/60 s in Q15.16
        for (int i = 0; i < 600; ++i) {
            v.y = v.y + mul<Speed>(g, dt);
            p += Vec3<Length>{mul<Length>(v.x, dt), mul<Length>(v.y, dt), mul<Length>(v.z, dt)};
        }
        return p;
    };
    const LengthVec a = run(), b = run();
    CHECK(a == b);
    // The same steps with raw integers.
    i64 vy = 0, py = toFixed<Length>(20.0).raw_value();
    const i64 g = toFixed<Speed>(-9.81).raw_value();
    for (int i = 0; i < 600; ++i) {
        vy += detail::roundShift<16>(g * 1092);
        py += detail::roundShift<17 + 16 - 18>(vy * 1092);
    }
    CHECK(a.y.raw_value() == py);
}

// Every size, 8 to 64 bits, with the binary point anywhere.
void sizes() {
    static_assert(std::is_same_v<Q<3, 4>, Fixed<i8, 4>> && std::is_same_v<Q<7, 8>, Fixed<i16, 8>>);
    static_assert(std::is_same_v<Q<13, 18>::Base, Length::Base> && std::is_same_v<Q<45, 18>, Fixed<i64, 18>>);
    static_assert(sizeof(Q<3, 4>) == 1 && sizeof(Q<7, 8>) == 2 && sizeof(Q<15, 16>) == 4 && sizeof(Q<45, 18>) == 8);
    // 8 bits: Q3.4, step 1/16, range ±8 (saturating).
    using Q8 = Q<3, 4>;
    CHECK(Q8(1.5).raw_value() == 24 && toDouble(Q8(2.25) * Q8(2)) == 4.5);
    CHECK(Q8(100).raw_value() == 127 && Q8(-100).raw_value() == -128);
    // 16 bits: Q7.8.
    using Q16 = Q<7, 8>;
    CHECK(toDouble(Q16(3.5) / Q16(2)) == 1.75 && Q16(-0.5) < Q16(0));
    CHECK(Vec3<Q16>(1.0, 2.0, 2.0).length<Q16>() == Q16(3));
    // Across sizes: products, quotients and conversions keep the exact value.
    CHECK(mul<Length>(Q8(1.5), Length(10)) == Length(15));
    CHECK(convert<Q16>(Length(2.5)) == Q16(2.5) && convert<Length>(Q8(-3.25)) == Length(-3.25));
    // 64 bits: add, subtract, compare, convert (no products: they need 128 bits).
    const GlobalLength far = GlobalLength(1.0e9); // 1 million km, exact to the step
    CHECK(far.raw_value() == i64(1'000'000'000) << 18);
    CHECK(far + GlobalLength(0.5) - far == GlobalLength(0.5) && -far < far && far * 3 == GlobalLength(3.0e9));
    CHECK(convert<Length>(GlobalLength(12.25)) == Length(12.25));
    CHECK(convert<GlobalLength>(Length(-7.5)) == GlobalLength(-7.5));
    const GlobalLengthVec v = GlobalLengthVec(1e9, -2e9, 3.0);
    CHECK(v - v == GlobalLengthVec() && v.toDouble() == glm::dvec3(1e9, -2e9, 3.0));
    // Fractional multiples of 64-bit values do not compile (the fraction would be dropped).
    static_assert(!Multipliable<GlobalLength, f64> && !Multipliable<f64, GlobalLength> && Multipliable<GlobalLength, int>);
    CHECK(fromRaw<GlobalLength>(std::numeric_limits<i64>::max()) > GlobalLength(3.5e13) && GlobalLength(1e14) == fromRaw<GlobalLength>(std::numeric_limits<i64>::max()));
}

// Every dimension is its own type, in the format the rule and its range give it.
void dimensions() {
    // Sizes: the smallest integer for the precision and the range.
    static_assert(sizeof(Friction) == 2 && sizeof(GravityFactor) == 2 && sizeof(DampingRatio) == 2);
    static_assert(sizeof(Speed) == 4 && sizeof(Acceleration) == 4 && sizeof(Mass) == 4 && sizeof(Scale) == 4 &&
                  sizeof(Restitution) == 4 && sizeof(Damping) == 4 && sizeof(Frequency) == 4);
    // Same format, different dimensions: never mixed implicitly.
    static_assert(std::is_same_v<Unit::Base, Restitution::Base> && !std::is_same_v<Unit, Restitution>);
    static_assert(!std::is_convertible_v<Unit, Restitution> && !std::is_convertible_v<Restitution, Unit>);
    static_assert(!std::is_convertible_v<Speed, Distance> && !std::is_convertible_v<Friction, GravityFactor>);
    static_assert(std::is_convertible_v<f32, Friction> && std::is_convertible_v<int, Mass>); // numbers go in
    CHECK(convert<Unit>(Restitution(0.25)) == Unit(0.25));                                // explicitly
    // The rule: one 1/60 s step of the smallest step moves at most 3.8 µm (2⁻¹⁸ m).
    const f64 dt = 1.0 / 60.0, g = 9.81, limit = std::ldexp(1.0, -18) * 1.0001;
    CHECK(toDouble(fromRaw<Speed>(1)) * dt <= limit);
    CHECK(toDouble(fromRaw<Acceleration>(1)) * dt * dt <= limit);
    CHECK(toDouble(fromRaw<Friction>(1)) * g * dt * dt <= limit);
    CHECK(toDouble(fromRaw<GravityFactor>(1)) * g * dt * dt <= limit);
    CHECK(toDouble(fromRaw<Restitution>(1)) * 100.0 * dt <= limit);  // a 100 m/s bounce
    CHECK(toDouble(fromRaw<Damping>(1)) * 100.0 * dt * dt <= limit); // at 100 m/s
    CHECK(toDouble(fromRaw<Scale>(1)) * 4.0 <= limit);               // a 4 m mesh
    // Ranges.
    CHECK(toDouble(fromRaw<Speed>(std::numeric_limits<i32>::max())) > 16000.0);
    CHECK(toDouble(fromRaw<Acceleration>(std::numeric_limits<i32>::max())) > 2000.0);
    CHECK(toDouble(fromRaw<Mass>(std::numeric_limits<i32>::max())) > 131000.0);
    CHECK(toDouble(fromRaw<Scale>(std::numeric_limits<i32>::max())) > 2000.0);
    CHECK(Friction(40).raw_value() == std::numeric_limits<i16>::max()); // saturates
    // Common values stay close: 9.81 m/s², a 10 g body, the default coefficients.
    CHECK(std::abs(toDouble(Acceleration(9.81)) - 9.81) < 1e-6);
    CHECK(std::abs(toDouble(Mass(0.01)) - 0.01) / 0.01 < 0.007);
    CHECK(std::abs(toDouble(Friction(0.5)) - 0.5) < 1e-9 && std::abs(toDouble(Restitution(0.1)) - 0.1) < 1e-9);
    // Not even through fpm's own operators (its base is protected).
    static_assert(!Addable<Unit, Restitution> && !Addable<Restitution, Unit> && !Multipliable<Length, Distance>);
    static_assert(Addable<Unit, Unit> && Multipliable<Unit, int> && std::is_constructible_v<double, Unit>);
}

// Unit literals: the nearest value of each dimension.
void unitLiterals() {
    CHECK(2.5_m == Length(2.5) && 50_cm == Length(0.5) && 3_mm == Length(0.003) && 2_km == Distance(2000));
    CHECK(36_kmh == Speed(10) && 9.81_mps2 == Acceleration(9.81) && 70_kg == Mass(70) && 100_N == Force(100));
    CHECK(90_deg == Angle::quarter() && 0.5_turn == Angle::half() && 360_deg == Angle());
    CHECK(120_degps == AngleSpeed::fromDegreesPerSecond(120) && 60_rpm == AngleSpeed::fromTurnsPerSecond(1));
    CHECK(std::abs((3.14159265358979_rad - Angle::half()).signedRaw()) <= 2);
    static_assert(std::is_same_v<decltype(1_m), Length> && std::is_same_v<decltype(1.0_deg), Angle>);
}

// Binary angles: a turn is 2^32, they wrap around by themselves; comparisons are signed.
void angles() {
    CHECK(Angle::fromDegrees(90) == Angle::quarter() && Angle::fromDegrees(180) == Angle::half());
    CHECK(Angle::fromDegrees(360) == Angle() && Angle::fromDegrees(-90).raw() == 3u << 30);
    CHECK(Angle::fromDegrees(350) + Angle::fromDegrees(20) == Angle::fromDegrees(10)); // wraps, exactly
    CHECK(Angle::fromTurns(1e6 + 0.25) == Angle::quarter());                         // whole turns drop out
    CHECK(Angle::fromRadians(glm::pi<f64>() / 2) == Angle::quarter());
    CHECK(Angle::fromDegrees(270).degrees() == -90.0 && Angle::fromDegrees(270).degreesPositive() == 270.0);
    CHECK(Angle::half().degrees() == -180.0 && Angle::half().abs() == Angle::half());
    CHECK(Angle::fromDegrees(-45) < Angle::fromDegrees(10) && Angle::fromDegrees(200) < Angle::fromDegrees(0)); // signed
    CHECK(-Angle::fromDegrees(30) == Angle::fromDegrees(330) && Angle::fromDegrees(100) * 4 == Angle::fromDegrees(40));
    CHECK(Angle::fromDegrees(90).scaled(Scalar(0.5)) == Angle::fromDegrees(45));
    static_assert(!Multipliable<Angle, f64> && !Multipliable<f32, Angle> && Multipliable<Angle, int>); // use scaled()
    CHECK(Angle::fromTurns(std::nan("")) == Angle());
    // Compact angles: 16 bits, exact back for multiples of their step.
    CHECK(Angle16(Angle::fromDegrees(90)).toAngle() == Angle::quarter());
    CHECK(std::abs((Angle16(Angle::fromDegrees(33.3)).toAngle() - Angle::fromDegrees(33.3)).signedRaw()) <= 1 << 15);
    // Angular speeds integrate exactly over integer time (flicks).
    const AngleSpeed s = AngleSpeed::fromDegreesPerSecond(90); // a quarter turn per second
    CHECK(angleTurned(s, kFlicksPerSecond) == Angle::quarter());
    CHECK(angleTurned(s, kFlicksPerSecond * 5) == Angle::quarter()); // 450° = 90°
    CHECK(angleTurned(-s, kFlicksPerSecond) == Angle::fromDegrees(-90));
    Angle sum;
    const Duration step = Duration::perSecond(60);
    for (int i = 0; i < 60; ++i) sum += angleTurned(s, step.flicks());
    CHECK(std::abs((sum - Angle::quarter()).signedRaw()) <= 60); // one rounding per step, at most
    CHECK(angleTurned(AngleSpeed::fromRaw(std::numeric_limits<i32>::max()), kFlicksPerSecond * 1000).raw() ==
          static_cast<u32>(static_cast<u64>(std::numeric_limits<i32>::max()) * 1000 << 7)); // huge: no overflow
    CHECK(std::abs(AngleSpeed::fromRadiansPerSecond(3.0).radiansPerSecond() - 3.0) < 1e-6);
    static_assert(!std::is_convertible_v<f32, AngleSpeed> && !std::is_convertible_v<AngleSpeed, f32>); // units are explicit
}

// The Fixed type itself: zero by default, numbers in, floats only out, formats kept apart.
void numbers() {
    Scalar s;
    CHECK(s.raw_value() == 0);                                // fpm leaves it uninitialized; Fixed does not
    s = 2.5f;                                                 // a number becomes fixed point implicitly
    CHECK(s.raw_value() == 5 << 15);
    CHECK(s == 2.5 && s > 2 && s < 2.6f && 2 < s && s >= 2.5f && s <= 2.5);
    CHECK(s * 2 == 5 && s + 0.5f == 3 && s - 1 == 1.5 && s / 2 == 1.25 && -s == -2.5);
    s += 0.5f;
    s *= 2;
    CHECK(s == 6);
    const Length l = -45;                                     // integers too
    CHECK(toDouble(l) == -45.0);
    CHECK(Scalar(1e9).raw_value() == INT32_MAX && Scalar(-1e9).raw_value() == INT32_MIN); // saturates
    CHECK(Scalar(std::nan("")).raw_value() == 0);
    // fpm functions work on its base(), and their results come back as Fixed explicitly.
    const Scalar r(fpm::sqrt(Scalar(9).base()));
    CHECK(std::abs(toDouble(r) - 3.0) < 1e-4);
    // Formats never mix implicitly; floats only leave explicitly.
    static_assert(!std::is_convertible_v<Scalar, Length>);
    static_assert(!std::is_convertible_v<Length, Scalar>);
    static_assert(!std::is_convertible_v<Scalar, f32>);
    static_assert(std::is_convertible_v<f32, Scalar>);
    static_assert(sizeof(Length) == 4 && sizeof(LengthVec) == 12 && sizeof(Quat) == 16);
    CHECK(static_cast<f32>(Scalar(0.5)) == 0.5f && toFloat(Scalar(0.5)) == 0.5f);
    // Vectors and quaternions are quantized from glm where they are assigned.
    LengthVec v = glm::vec3(1.0f, 2.0f, 3.0f);
    CHECK(v == LengthVec(1.0, 2.0, 3.0) && v.toGlm() == glm::vec3(1, 2, 3));
    v = {0, 0.5, -1};
    CHECK(v.y.raw_value() == 1 << 17 && toDouble(v.z) == -1.0);
    const Quat q = glm::angleAxis(1.0f, glm::vec3(0, 1, 0));
    CHECK(std::abs(glm::length(q.toDouble()) - 1.0) < 1e-8);
    const Quat unnormalized = glm::quat(2, 0, 0, 0);
    CHECK(unnormalized == Quat::identity());
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"formats: steps, ranges and the minimum-movement rule", formats},
        {"conversions: nearest, exact round trips", conversions},
        {"products and quotients across formats", products},
        {"integer square root", squareRoots},
        {"vectors: torus diagonals, lengths, units, dot, cross", vectors},
        {"quaternions: rotation, composition, renormalization", quaternions},
        {"determinism: integer-only results", determinism},
        {"the Fixed type: numbers in, floats out, formats apart", numbers},
        {"every size: 8, 16, 32 and 64 bits", sizes},
        {"dimensions: own types, formats from the rule and the range", dimensions},
        {"binary angles: wraps, signed comparisons, exact integration", angles},
        {"unit literals: 2.5_m, 90_deg, 9.81_mps2...", unitLiterals},
    };
    for (auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
