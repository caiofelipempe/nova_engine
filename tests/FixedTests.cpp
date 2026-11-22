// Fixed-point numbers of the simulation (nova/math/Fixed.h, lib fpm). Run: ctest --preset ... or NovaFixedTests.exe
#include <nova/math/Fixed.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <vector>

using namespace nova;

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
    CHECK(toDouble(fromRaw<Speed>(1)) == std::ldexp(1.0, -16));                  // 15 µm/s
    CHECK_NEAR(toDouble(fromRaw<Angle>(std::numeric_limits<i32>::max())), 8.0, 1e-7);
    CHECK_NEAR(toDouble(fromRaw<Unit>(std::numeric_limits<i32>::max())), 2.0, 1e-8);
    CHECK(toDouble(fromRaw<Force>(1)) == 1.0 / 256.0);                           // 4 mN
    // The rule: one 1/60 s step of the smallest value of each kind moves ≤ 3.8 µm.
    const f64 step = 1.0 / 60.0, limit = std::ldexp(1.0, -18) * 1.0001;
    CHECK(toDouble(fromRaw<Speed>(1)) * step <= limit);                          // a speed for a step
    CHECK(toDouble(fromRaw<Force>(1)) / 1.0 * step * step <= limit);             // a force on 1 kg
    CHECK(toDouble(fromRaw<Angle>(1)) * 1000.0 <= limit);                        // an angle, 1 km from the center
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
    CHECK(zero<Speed>().raw_value() == 0 && LengthVec{}.x.raw_value() == 0 && FixedQuat{}.w.raw_value() == 1 << 30);
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
    CHECK(dot<Length>(v, n) == toFixed<Length>(13.0) || std::abs(toDouble(dot<Length>(v, n)) - 13.0) < 1e-5);
    const LengthVec x = LengthVec::fromGlm(glm::dvec3(1, 0, 0)), y = LengthVec::fromGlm(glm::dvec3(0, 1, 0));
    CHECK(cross<Length>(x, y) == LengthVec::fromGlm(glm::dvec3(0, 0, 1)));
    CHECK(v.scaled(toFixed<Scalar>(0.5)) == LengthVec::fromGlm(glm::dvec3(1.5, 2.0, 6.0)));
    CHECK((v - v) == LengthVec{} && (v + -v) == LengthVec{});
}

void quaternions() {
    // Matches glm within the format's precision.
    const glm::quat g = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1, 2, 3)));
    const FixedQuat q = FixedQuat::fromGlm(g);
    const glm::vec3 p(1.25f, -2.0f, 3.5f);
    const glm::vec3 expected = g * p;
    const glm::dvec3 got = q.rotate(LengthVec::fromGlm(p)).toDouble();
    CHECK(glm::length(glm::dvec3(expected) - got) < 1e-5);
    // From axis and angle (fpm's deterministic sin / cos).
    const FixedQuat a = FixedQuat::fromAxisAngle(LengthVec::fromGlm(glm::vec3(0, 0, 1)).normalized(), toFixed<Angle>(glm::half_pi<f64>()));
    const glm::dvec3 turned = a.rotate(LengthVec::fromGlm(glm::vec3(2, 0, 0))).toDouble();
    CHECK(glm::length(turned - glm::dvec3(0, 2, 0)) < 1e-5);
    // Composition order as glm, inverse, renormalization.
    const FixedQuat b = FixedQuat::fromGlm(glm::angleAxis(-1.1f, glm::vec3(0, 1, 0)));
    const glm::dvec3 ab = (a * b).rotate(LengthVec::fromGlm(p)).toDouble();
    CHECK(glm::length(ab - glm::dvec3((a.toGlm() * b.toGlm()) * p)) < 1e-5);
    const glm::dvec3 back = q.conjugate().rotate(q.rotate(LengthVec::fromGlm(p))).toDouble();
    CHECK(glm::length(back - glm::dvec3(p)) < 1e-5);
    // A long chain of small turns stays unit and close to the float reference.
    FixedQuat acc;
    glm::dquat ref(1, 0, 0, 0);
    const FixedQuat stepq = FixedQuat::fromGlm(glm::angleAxis(0.01f, glm::normalize(glm::vec3(0.3f, 1, -0.2f))));
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
            p += FixedVec3<Length>{mul<Length>(v.x, dt), mul<Length>(v.y, dt), mul<Length>(v.z, dt)};
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
        py += detail::roundShift<16 + 16 - 18>(vy * 1092);
    }
    CHECK(a.y.raw_value() == py);
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
    };
    for (auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
