// Trigonometry of binary angles (nova/math/Trig.h). Run: ctest --preset ... or NovaTrigTests.exe
#include <nova/math/Trig.h>

#include <cmath>
#include <cstdio>
#include <functional>
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

const Unit kOne = Unit(1), kZero = Unit(0);

void quarterTurns() {
    // Exact at every multiple of 90°: the quadrant comes from the bits, not from π.
    CHECK(sin(Angle()) == kZero && cos(Angle()) == kOne);
    CHECK(sin(Angle::quarter()) == kOne && cos(Angle::quarter()) == kZero);
    CHECK(sin(Angle::half()) == kZero && cos(Angle::half()) == -kOne);
    CHECK(sin(Angle::fromDegrees(270)) == -kOne && cos(Angle::fromDegrees(270)) == kZero);
    CHECK(sin(Angle::fromDegrees(-90)) == -kOne);
    // Known values.
    CHECK(std::abs(toDouble(sin(Angle::fromDegrees(30))) - 0.5) < 1e-9);
    CHECK(std::abs(toDouble(cos(Angle::fromDegrees(60))) - 0.5) < 1e-9);
    CHECK(std::abs(toDouble(tan(Angle::fromDegrees(45))) - 1.0) < 1e-4);
    Unit s, c;
    sinCos(Angle::fromDegrees(45), s, c);
    CHECK(s == c && std::abs(toDouble(s) - std::sqrt(0.5)) < 1e-9);
}

void againstStd() {
    // Within one step of Q1.30 of the standard library, everywhere on the circle.
    std::mt19937 rng(11);
    bool close = true, unit = true;
    for (int i = 0; i < 100000; ++i) {
        const Angle a = Angle::fromRaw(static_cast<u32>(rng()));
        const f64 r = a.radians();
        const f64 s = toDouble(sin(a)), c = toDouble(cos(a));
        close &= std::abs(s - std::sin(r)) <= std::ldexp(1.0, -30) && std::abs(c - std::cos(r)) <= std::ldexp(1.0, -30);
        unit &= std::abs(s * s + c * c - 1.0) < 1e-8;
    }
    CHECK(close && unit);
}

void inverses() {
    CHECK(asin(kOne) == Angle::quarter() && asin(-kOne) == Angle::fromDegrees(-90) && asin(kZero) == Angle());
    CHECK(acos(kOne) == Angle() && acos(kZero) == Angle::quarter() && acos(-kOne) == Angle::half());
    CHECK(asin(Unit(1.5)) == Angle::quarter()); // clamped
    CHECK(atan2(Length(1), Length(0)) == Angle::quarter() && atan2(Length(0), Length(-1)) == Angle::half());
    CHECK(atan2(Length(-1), Length(-1)) == Angle::fromDegrees(-135) && atan(Scalar(1)) == Angle::fromDegrees(45));
    CHECK(atan2(Speed(2), Length(2)) == Angle::fromDegrees(45)); // any formats
    // Round trips: an angle → sine and cosine → atan2 comes back within a few steps of Q1.30.
    std::mt19937 rng(5);
    bool back = true;
    for (int i = 0; i < 100000; ++i) {
        const Angle a = Angle::fromRaw(static_cast<u32>(rng()));
        const Angle b = atan2(sin(a), cos(a));
        back &= std::abs((b - a).signedRaw()) <= 8; // 2⁻³⁰ of a unit circle ≈ 6 steps of 2⁻³² turn
    }
    CHECK(back);
}

void roots() {
    CHECK(nova::sqrt(Scalar(9)) == Scalar(3) && nova::sqrt(Length(2.25)) == Length(1.5));
    CHECK(nova::sqrt(Scalar(-4)) == Scalar(0));
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"quarter turns are exact, known values", quarterTurns},
        {"sin / cos against the standard library", againstStd},
        {"asin / acos / atan / atan2 and round trips", inverses},
        {"square roots", roots},
    };
    for (auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
