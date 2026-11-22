// Headless tests of exact time: flicks, Duration / TimePoint arithmetic, time units, std::chrono
// interop, cycles for shaders and the scene clock.
#include <nova/Nova.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
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

bool near(f64 a, f64 b, f64 eps) { return std::abs(a - b) <= eps; }

void rates() {
    // Every common frame and audio rate is a whole number of flicks.
    const i64 hz[] = {24, 25, 30, 48, 50, 60, 72, 90, 100, 120, 144, 240,
                      8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
    bool exact = true;
    for (i64 f : hz) exact &= Duration::perSecond(f) * f == Duration::seconds(1);
    CHECK(exact);
    CHECK(Duration::perSecond(60).flicks() == 11'760'000);
    CHECK(Duration::milliseconds(1).flicks() == 705'600);

    // A day of 60 Hz steps adds up to exactly one day; float seconds drift.
    Duration total;
    f32 floatTotal = 0.0f;
    const Duration step = Duration::perSecond(60);
    for (int i = 0; i < 60 * 60 * 60 * 24; ++i) {
        total += step;
        floatTotal += 1.0f / 60.0f;
    }
    CHECK(total == Duration::days(1));
    CHECK(std::abs(floatTotal - 86400.0f) > 10.0f); // float lost more than 10 seconds in a day
}

void arithmetic() {
    CHECK(Duration::seconds(1) / Duration::perSecond(60) == 60);
    CHECK(Duration::seconds(1) % Duration::milliseconds(300) == Duration::milliseconds(100));
    CHECK(Duration::seconds(-1) % Duration::milliseconds(300) == -Duration::milliseconds(100));
    CHECK(Duration::minutes(3) / 3 == Duration::minutes(1));
    CHECK(Duration::hours(2) - Duration::minutes(30) == Duration::minutes(90));
    CHECK(Duration::milliseconds(250) < Duration::seconds(1) && Duration::seconds(1) >= Duration::milliseconds(1000));
    CHECK(Duration::seconds(1).scaled(0.5) == Duration::milliseconds(500));
    // Saturation instead of overflow.
    const Duration huge = Duration::days(i64(1) << 50);
    CHECK(huge.flicks() == std::numeric_limits<i64>::max());
    CHECK((huge + Duration::seconds(1)) == huge);
    CHECK(-Duration::fromFlicks(std::numeric_limits<i64>::min()) == huge);
    CHECK(near(Duration::fromFlicks(std::numeric_limits<i64>::max()).toUnits(TimeUnit::Day) / 365.25, 414.0, 1.0)); // ±414 years

    TimePoint a = TimePoint::fromSeconds(10.0), b = a + Duration::milliseconds(16);
    CHECK(b - a == Duration::milliseconds(16) && a + (b - a) == b && b > a);
    CHECK((b - Duration::milliseconds(16)) == a);
}

void units() {
    CHECK(Duration::milliseconds(250).seconds() == 0.25);
    CHECK(Duration::fromUnits(1.5, TimeUnit::Hour) == Duration::minutes(90));
    CHECK(near(Duration::minutes(90).toUnits(TimeUnit::Hour), 1.5, 1e-12));
    CHECK(near(Duration::perSecond(60).toUnits(TimeUnit::Millisecond), 16.6666666667, 1e-9));
    CHECK(convertTime(1.5, TimeUnit::Hour, TimeUnit::Minute) == 90.0);
    CHECK(toSeconds(2.0, TimeUnit::Minute) == 120.0 && fromSeconds(7200.0, TimeUnit::Hour) == 2.0);
    CHECK(readableTimeUnit(90.0) == TimeUnit::Minute && readableTimeUnit(0.004) == TimeUnit::Millisecond &&
          readableTimeUnit(3e-8) == TimeUnit::Nanosecond && readableTimeUnit(200000.0) == TimeUnit::Day);
    CHECK(std::string(unitSymbol(TimeUnit::Minute)) == "min");
    // Sub-flick units round to the nearest flick (1 µs = 705.6 flicks).
    CHECK(Duration::fromUnits(1.0, TimeUnit::Microsecond).flicks() == 706);
    CHECK(Duration::fromSeconds(std::nan("")) == Duration());
}

void chrono() {
    using namespace std::chrono;
    CHECK(Duration::fromChrono(milliseconds(16)) == Duration::milliseconds(16));
    CHECK(Duration::fromChrono(hours(1)) == Duration::hours(1));
    CHECK(Duration::fromChrono(nanoseconds(1000)).flicks() == 706);
    CHECK(Duration::fromChrono(duration<double>(0.5)) == Duration::milliseconds(500));
    CHECK(duration_cast<milliseconds>(Duration::seconds(2).toChrono()).count() == 2000);
}

void cycles() {
    // Shaders get seconds within an hour: precise however long the game has been running.
    const TimePoint late = TimePoint() + Duration::days(10) + Duration::milliseconds(1);
    CHECK(near(late.secondsInCycle(Duration::hours(1)), 0.001, 1e-12));
    CHECK(static_cast<f32>(late.seconds()) == 864000.0f); // the millisecond is lost in float
    CHECK(TimePoint::fromSeconds(3600.0).secondsInCycle(Duration::hours(1)) == 0.0);
    CHECK(near((TimePoint() - Duration::seconds(1)).secondsInCycle(Duration::seconds(60)), 59.0, 1e-12));
}

void sceneClock() {
    Scene scene;
    for (int i = 0; i < 60; ++i) scene.update(Duration::perSecond(60));
    CHECK(scene.time() == TimePoint() + Duration::seconds(1));
    scene.update(0.5f); // float seconds still accepted
    CHECK(scene.time() == TimePoint() + Duration::milliseconds(1500));
    CHECK(scene.fixedTimeStep == Duration::perSecond(60));

    // Shader time restarts every hour.
    scene.update(Duration::hours(1));
    RenderScene rs;
    scene.buildRenderScene(rs, 1.0f);
    CHECK(near(rs.time, 1.5, 1e-6));
    CHECK(Scene::kShaderTimeCycle == Duration::hours(1));
}

void glmInterop() {
    GlobalOffset o(glm::i32vec3(1, -2, 3));
    CHECK(o == GlobalOffset(1, -2, 3) && o.raw() == glm::i32vec3(1, -2, 3));
    GlobalPosition p = GlobalPosition::fromRaw(glm::i32vec3(5, 6, 7));
    CHECK(p.raw() == glm::i32vec3(5, 6, 7));
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Log::init();
    Log::core().set_level(spdlog::level::off);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"frame and audio rates are exact; no drift", rates},
        {"Duration / TimePoint arithmetic", arithmetic},
        {"time units", units},
        {"std::chrono interop", chrono},
        {"cycles (shader time)", cycles},
        {"scene clock", sceneClock},
        {"glm interop of positions", glmInterop},
    };
    for (auto& [name, fn] : tests) {
        int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    Log::shutdown();
    return g_failures == 0 ? 0 : 1;
}
